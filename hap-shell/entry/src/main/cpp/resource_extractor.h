// entry/src/main/cpp/resource_extractor.h
#pragma once

#include <rawfile/raw_file_manager.h>
#include <string>

namespace blender_ohos {

struct ExtractResult {
    bool         ok                 = false;
    bool         skipped            = false;   // stamp matched
    std::string  archive;                       // rawfile name
    std::string  extract_to;                    // target directory
    size_t       compressed_bytes   = 0;
    size_t       uncompressed_bytes = 0;
    int          files_written      = 0;
    int          dirs_created       = 0;
    std::string  message;                       // one-line status
};

// Extract `archive_name` from the HAP's rawfile/ into `extract_to`, using
// libzstd (loaded via dlopen) to decompress and a small ustar/GNU-tar
// parser to lay files onto disk. A .stamp file records the compressed
// size so repeat calls are no-ops unless force==true or the size changed.
ExtractResult ExtractRawfileArchive(NativeResourceManager* rm,
                                     const std::string&    archive_name,
                                     const std::string&    extract_to,
                                     bool                  force);

}  // namespace blender_ohos
