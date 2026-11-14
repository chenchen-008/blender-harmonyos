// entry/src/main/cpp/resource_extractor.cpp
//
// Reads a *.tar.zst from HAP rawfile/, decompresses via libzstd (dlopened),
// and feeds the byte stream into a small ustar/GNU tar parser that writes
// each entry to disk. State is tracked with a single-byte-counter block
// state machine so we never buffer more than one 512-byte header at a time.

#include "resource_extractor.h"

#include <rawfile/raw_file.h>
#include <rawfile/raw_file_manager.h>

#include <hilog/log.h>

#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#undef  LOG_TAG
#define LOG_TAG "Blender_NAPI_EXT"
#define LOGI(f, ...) OH_LOG_INFO (LOG_APP, f, ##__VA_ARGS__)
#define LOGW(f, ...) OH_LOG_WARN (LOG_APP, f, ##__VA_ARGS__)
#define LOGE(f, ...) OH_LOG_ERROR(LOG_APP, f, ##__VA_ARGS__)

namespace blender_ohos {

// ── libzstd bindings (dlopen'd) ─────────────────────────────────────────
// Forward-declare the two buffer structs — zstd's ABI is stable.
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
typedef struct { const void* src; size_t size; size_t pos; } Z_InBuf;
typedef struct { void*       dst; size_t size; size_t pos; } Z_OutBuf;

namespace z {
    bool         loaded              = false;
    ZSTD_DCtx*   (*createDCtx)()     = nullptr;
    size_t       (*freeDCtx)(ZSTD_DCtx*) = nullptr;
    size_t       (*decompressStream)(ZSTD_DCtx*, Z_OutBuf*, Z_InBuf*) = nullptr;
    unsigned     (*isError)(size_t)  = nullptr;
    const char*  (*getErrorName)(size_t) = nullptr;
}

static bool ensure_zstd() {
    if (z::loaded) return true;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if (z::loaded) return true;

    void* h = dlopen("libzstd.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        LOGE("dlopen libzstd.so.1 failed: %{public}s", dlerror());
        return false;
    }
    #define SYM(v, n) do { \
        v = reinterpret_cast<decltype(v)>(dlsym(h, n)); \
        if (!v) { LOGE("dlsym %{public}s failed", n); return false; } \
    } while (0)
    SYM(z::createDCtx,       "ZSTD_createDCtx");
    SYM(z::freeDCtx,         "ZSTD_freeDCtx");
    SYM(z::decompressStream, "ZSTD_decompressStream");
    SYM(z::isError,          "ZSTD_isError");
    SYM(z::getErrorName,     "ZSTD_getErrorName");
    #undef SYM
    z::loaded = true;
    // LOGI("libzstd bound OK");
    return true;
}

// ── filesystem helpers ──────────────────────────────────────────────────
static bool mkdir_p(const std::string& path, mode_t mode = 0755) {
    if (path.empty()) return true;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        if (!mkdir_p(path.substr(0, slash), mode)) return false;
    }
    if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
        LOGE("mkdir %{public}s: %{public}s", path.c_str(), strerror(errno));
        return false;
    }
    return true;
}

static bool ensure_parent_dir(const std::string& file_path) {
    size_t slash = file_path.find_last_of('/');
    if (slash == std::string::npos) return true;
    return mkdir_p(file_path.substr(0, slash));
}

// ── tar consumer ────────────────────────────────────────────────────────
// ustar layout: 512-byte header, followed by data padded to 512.
// Only the entry types we actually see in our archives are handled:
//   '\0'/'0' regular file, '5' directory, 'L' GNU long name.
class TarSink {
public:
    explicit TarSink(std::string base) : base_(std::move(base)) {}
    ~TarSink() { if (cur_) fclose(cur_); }

    bool feed(const uint8_t* data, size_t len) {
        while (len > 0 && !eof_) {
            if (state_ == S_HEADER) {
                size_t need = 512 - hdr_fill_;
                size_t take = len < need ? len : need;
                memcpy(hdr_ + hdr_fill_, data, take);
                hdr_fill_ += take;
                data += take; len -= take;
                if (hdr_fill_ == 512) {
                    hdr_fill_ = 0;
                    if (!process_header()) return false;
                    if (block_remain_ == 0 && !eof_) finish_entry();
                }
            } else {
                // S_DATA or S_LONGNAME
                size_t take = len < block_remain_ ? len : block_remain_;
                size_t writable = 0;
                if (data_written_ < data_size_) {
                    writable = data_size_ - data_written_;
                    if (writable > take) writable = take;
                }
                if (state_ == S_DATA && cur_ && writable) {
                    if (fwrite(data, 1, writable, cur_) != writable) {
                        err_ = std::string("fwrite failed on ") + cur_path_ + ": " + strerror(errno);
                        return false;
                    }
                    total_bytes_ += writable;
                } else if (state_ == S_LONGNAME && writable) {
                    pending_longname_.append(reinterpret_cast<const char*>(data), writable);
                }
                data          += take;
                len           -= take;
                data_written_ += writable;
                block_remain_ -= take;
                if (block_remain_ == 0) finish_entry();
            }
        }
        return true;
    }

    bool finish() { if (cur_) { fclose(cur_); cur_ = nullptr; } return err_.empty(); }

    int    files() const { return files_; }
    int    dirs()  const { return dirs_; }
    size_t bytes() const { return total_bytes_; }
    const std::string& error() const { return err_; }

private:
    enum State { S_HEADER, S_DATA, S_LONGNAME };

    static uint64_t parse_octal(const char* p, size_t n) {
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) {
            char c = p[i];
            if (c == 0 || c == ' ') break;
            if (c >= '0' && c <= '7') v = v * 8 + (c - '0');
        }
        return v;
    }
    static bool is_zero_block(const uint8_t* b) {
        for (int i = 0; i < 512; ++i) if (b[i]) return false;
        return true;
    }

    bool process_header() {
        if (is_zero_block(hdr_)) { eof_ = true; return true; }

        std::string name;
        if (!pending_longname_.empty()) {
            while (!pending_longname_.empty() && pending_longname_.back() == '\0')
                pending_longname_.pop_back();
            name = std::move(pending_longname_);
            pending_longname_.clear();
        } else {
            char nbuf[101] = {0}; memcpy(nbuf, hdr_, 100);
            char pbuf[156] = {0}; memcpy(pbuf, hdr_ + 345, 155);
            name = pbuf[0] ? (std::string(pbuf) + "/" + nbuf) : std::string(nbuf);
        }

        data_size_    = static_cast<size_t>(parse_octal(reinterpret_cast<const char*>(hdr_) + 124, 12));
        size_t pad    = (512 - (data_size_ % 512)) % 512;
        block_remain_ = data_size_ + pad;
        data_written_ = 0;

        char typeflag = static_cast<char>(hdr_[156]);

        if (typeflag == 'L' || typeflag == 'K') {
            pending_longname_.clear();
            pending_longname_.reserve(data_size_);
            state_ = S_LONGNAME;
            return true;
        }
        if (typeflag == '5') {
            if (!mkdir_p(base_ + "/" + name)) { err_ = "mkdir failed: " + name; return false; }
            ++dirs_;
            state_ = S_DATA;    // skip any trailing data
            return true;
        }
        if (typeflag == '\0' || typeflag == '0') {
            std::string full = base_ + "/" + name;
            if (!ensure_parent_dir(full)) { err_ = "cannot mkparent for: " + full; return false; }
            cur_ = fopen(full.c_str(), "wb");
            if (!cur_) { err_ = "cannot open: " + full + ": " + strerror(errno); return false; }
            cur_path_ = full;
            state_ = S_DATA;
            return true;
        }
        // Symlinks, hardlinks, pax headers, etc — skip data.
        state_ = S_DATA;
        return true;
    }

    void finish_entry() {
        if (state_ == S_DATA && cur_) {
            fclose(cur_); cur_ = nullptr;
            ++files_;
            cur_path_.clear();
        }
        state_ = S_HEADER;
    }

    std::string base_;
    uint8_t     hdr_[512]{};
    size_t      hdr_fill_ = 0;
    State       state_ = S_HEADER;
    size_t      data_size_    = 0;
    size_t      data_written_ = 0;
    size_t      block_remain_ = 0;
    std::FILE*  cur_ = nullptr;
    std::string cur_path_;
    std::string pending_longname_;
    int         files_ = 0;
    int         dirs_  = 0;
    size_t      total_bytes_ = 0;
    bool        eof_ = false;
    std::string err_;
};

// ── stamp file ──────────────────────────────────────────────────────────
static std::string stamp_path(const std::string& d) { return d + "/.stamp"; }

static bool stamp_matches(const std::string& d, long expected) {
    FILE* f = fopen(stamp_path(d).c_str(), "rb");
    if (!f) return false;
    long v = -1;
    int n = fscanf(f, "%ld", &v);
    fclose(f);
    return n == 1 && v == expected;
}

static void stamp_write(const std::string& d, long size) {
    FILE* f = fopen(stamp_path(d).c_str(), "wb");
    if (!f) return;
    fprintf(f, "%ld\n", size);
    fclose(f);
}

// ── main entry ──────────────────────────────────────────────────────────
ExtractResult ExtractRawfileArchive(NativeResourceManager* rm,
                                     const std::string&    archive_name,
                                     const std::string&    extract_to,
                                     bool                  force) {
    ExtractResult r;
    r.archive    = archive_name;
    r.extract_to = extract_to;

    if (!ensure_zstd()) { r.message = "libzstd not loaded"; return r; }

    RawFile* raw = OH_ResourceManager_OpenRawFile(rm, archive_name.c_str());
    if (!raw) {
        r.message = "OpenRawFile failed: " + archive_name;
        LOGE("%{public}s", r.message.c_str());
        return r;
    }
    long csize = OH_ResourceManager_GetRawFileSize(raw);
    r.compressed_bytes = static_cast<size_t>(csize);

    if (!force && stamp_matches(extract_to, csize)) {
        OH_ResourceManager_CloseRawFile(raw);
        r.ok = true; r.skipped = true;
        r.message = "up-to-date (stamp: " + std::to_string(csize) + " B)";
        return r;
    }

    if (!mkdir_p(extract_to)) {
        OH_ResourceManager_CloseRawFile(raw);
        r.message = "mkdir failed: " + extract_to;
        return r;
    }

    std::vector<uint8_t> compressed(static_cast<size_t>(csize));
    int got = OH_ResourceManager_ReadRawFile(raw, compressed.data(), csize);
    OH_ResourceManager_CloseRawFile(raw);
    if (got != csize) {
        r.message = "short rawfile read: " + std::to_string(got) + "/" + std::to_string(csize);
        return r;
    }

    ZSTD_DCtx* dctx = z::createDCtx();
    if (!dctx) { r.message = "ZSTD_createDCtx null"; return r; }

    std::vector<uint8_t> out_buf(128 * 1024);
    Z_InBuf in { compressed.data(), compressed.size(), 0 };
    TarSink sink(extract_to);

    while (in.pos < in.size) {
        Z_OutBuf out { out_buf.data(), out_buf.size(), 0 };
        size_t rc = z::decompressStream(dctx, &out, &in);
        if (z::isError(rc)) {
            r.message = std::string("zstd: ") + z::getErrorName(rc);
            z::freeDCtx(dctx);
            return r;
        }
        if (!sink.feed(out_buf.data(), out.pos)) {
            r.message = "tar: " + sink.error();
            z::freeDCtx(dctx);
            return r;
        }
    }
    z::freeDCtx(dctx);
    sink.finish();

    r.uncompressed_bytes = sink.bytes();
    r.files_written      = sink.files();
    r.dirs_created       = sink.dirs();

    stamp_write(extract_to, csize);

    r.ok = true;
    r.message = "extracted";
    return r;
}

}  // namespace blender_ohos
