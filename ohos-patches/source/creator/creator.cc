/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup creator
 */

#include <cstdlib>
#include <cstring>

#ifdef WIN32
#  include "utfconv.hh"
#  include <windows.h>
#  ifdef WITH_CPU_CHECK
#    pragma comment(linker, "/include:cpu_check_win32")
#  endif
#endif

#if defined(WITH_TBB_MALLOC) && defined(_MSC_VER) && defined(NDEBUG)
#  pragma comment(lib, "tbbmalloc_proxy.lib")
#  pragma comment(linker, "/include:__TBB_malloc_proxy")
#endif

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "DNA_genfile.h"

#include "BLI_fftw.hh"
#include "BLI_string.h"
#include "BLI_system.h"
#include "BLI_task.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

/* Mostly initialization functions. */
#include "BKE_appdir.hh"
#include "BKE_blender.hh"
#include "BKE_brush.hh"
#include "BKE_callbacks.hh"
#include "BKE_context.hh"
#include "BKE_cpp_types.hh"
#include "BKE_global.hh"
#include "BKE_idtype.hh"
#include "BKE_material.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_particle.h"
#include "BKE_shader_fx.h"
#include "BKE_sound.h"
#include "BKE_vfont.hh"
#include "BKE_volume.hh"

#ifndef WITH_PYTHON_MODULE
#  include "BLI_args.h"
#endif

#include "DEG_depsgraph.hh"

#include "IMB_imbuf.hh" /* For #IMB_init. */

#include "MOV_util.hh"

#include "RE_engine.h"
#include "RE_texture.h"

#include "ED_datafiles.h"

#include "WM_api.hh"

#include "RNA_define.hh"

#ifdef WITH_OPENGL_BACKEND
#  include "GPU_compilation_subprocess.hh"
#endif

#ifdef WITH_FREESTYLE
#  include "FRS_freestyle.h"
#endif

#include <csignal>

#ifdef __FreeBSD__
#  include <floatingpoint.h>
#endif

#ifdef WITH_BINRELOC
#  include "binreloc.h"
#endif

#ifdef WITH_LIBMV
#  include "libmv-capi.h"
#elif defined(WITH_CYCLES_LOGGING)
#  include "CCL_api.h"
#endif

#ifdef __OHOS__
#  include <cstdlib>
#  include <string>
#  include <sys/stat.h>  /* mkdir */
#  include <sys/types.h>
#endif

#include "creator_intern.h"

/* OHOS PATCH: expose libblender.so as a native library callable from ArkTS.
 *
 * On OHOS we do not fork a process and call main() from a shell; instead the
 * HAP loads libblender.so via napi_load_module and drives frames explicitly
 * by calling Blender_Step() at display refresh rate. The four entry points
 * defined at the bottom of this file (Blender_Init/Step/IsRunning/Exit) are
 * the minimal C ABI the ArkTS bridge needs. */
#ifdef __OHOS__
#  include "WM_api.hh"          /* WM_exit */
#  include "wm_window.hh"       /* wm_window_events_process */
#  include "wm_event_system.hh" /* wm_event_do_{handlers,notifiers} */
#  include "wm_draw.hh"         /* wm_draw_update */

/* Global bContext captured inside main() after CTX_create() so that the
 * per-frame entry point (Blender_Step) can reach it. bContext is in the
 * global namespace (not the blender:: namespace), forward-declared in
 * BKE_context.hh via C-style `struct bContext;`. */
static struct bContext *g_ohos_ctx = nullptr;

/* Handed in from ArkTS via Blender_SetNativeWindow before Blender_Init.
 * GHOST-OHOS will pick these up when creating the Vulkan surface.
 * Kept as C-linkage globals so GHOST can reach them without pulling in
 * creator.cc as a compilation unit. */
extern "C" void    *g_ghost_ohos_native_window = nullptr;
extern "C" uint32_t g_ghost_ohos_win_w = 0;
extern "C" uint32_t g_ghost_ohos_win_h = 0;
#endif /* Own include. */

/* -------------------------------------------------------------------- */
/** \name Local Defines
 * \{ */

/* When building as a Python module, don't use special argument handling
 * so the module loading logic can control the `argv` & `argc`. */
#if defined(WIN32) && !defined(WITH_PYTHON_MODULE)
#  define USE_WIN32_UNICODE_ARGS
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Local Application State
 * \{ */

/* Written to by `creator_args.cc`. */
ApplicationState app_state = []() {
  ApplicationState app_state{};
  app_state.signal.use_crash_handler = true;
  app_state.signal.use_abort_handler = true;
  app_state.exit_code_on_error.python = 0;
  app_state.main_arg_deferred = nullptr;
  return app_state;
}();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Application Level Callbacks
 *
 * Initialize callbacks for the modules that need them.
 * \{ */

static void callback_mem_error(const char *errorStr)
{
  fputs(errorStr, stderr);
  fflush(stderr);
}

static void main_callback_setup()
{
  /* Error output from the guarded allocation routines. */
  MEM_set_error_callback(callback_mem_error);
}

/** Free data on early exit (if Python calls `sys.exit()` while parsing args for eg). */
struct CreatorAtExitData {
#ifndef WITH_PYTHON_MODULE
  bArgs *ba;
#endif

#ifdef USE_WIN32_UNICODE_ARGS
  char **argv;
  int argv_num;
#endif

#if defined(WITH_PYTHON_MODULE) && !defined(USE_WIN32_UNICODE_ARGS)
  void *_empty; /* Prevent empty struct error with MSVC. */
#endif
};

static void callback_main_atexit(void *user_data)
{
  CreatorAtExitData *app_init_data = static_cast<CreatorAtExitData *>(user_data);

#ifndef WITH_PYTHON_MODULE
  if (app_init_data->ba) {
    BLI_args_destroy(app_init_data->ba);
    app_init_data->ba = nullptr;
  }
#else
  UNUSED_VARS(app_init_data); /* May be unused. */
#endif

#ifdef USE_WIN32_UNICODE_ARGS
  if (app_init_data->argv) {
    while (app_init_data->argv_num) {
      free((void *)app_init_data->argv[--app_init_data->argv_num]);
    }
    free((void *)app_init_data->argv);
    app_init_data->argv = nullptr;
  }
#else
  UNUSED_VARS(app_init_data); /* May be unused. */
#endif
}

static void callback_clg_fatal(void *fp)
{
  BLI_system_backtrace(static_cast<FILE *>(fp));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blender as a Stand-Alone Python Module (bpy)
 *
 * While not officially supported, this can be useful for Python developers.
 * See: https://developer.blender.org/docs/handbook/building_blender/python_module/
 * \{ */

#ifdef WITH_PYTHON_MODULE

/* Called in `bpy_interface.cc` when building as a Python module. */
int main_python_enter(int argc, const char **argv);
void main_python_exit();

/* Rename the `main(..)` function, allowing Python initialization to call it. */
#  define main main_python_enter
static void *evil_C = nullptr;

#  ifdef __APPLE__
/* Environment is not available in macOS shared libraries. */
#    include <crt_externs.h>
char **environ = nullptr;
#  endif /* __APPLE__ */

#endif /* WITH_PYTHON_MODULE */

/** \} */

/* -------------------------------------------------------------------- */
/** \name GMP Allocator Workaround
 * \{ */

#if (defined(WITH_TBB_MALLOC) && defined(_MSC_VER) && defined(NDEBUG) && defined(WITH_GMP)) || \
    defined(DOXYGEN)
#  include "gmp.h"
#  include "tbb/scalable_allocator.h"

void *gmp_alloc(size_t size)
{
  return scalable_malloc(size);
}
void *gmp_realloc(void *ptr, size_t /*old_size*/, size_t new_size)
{
  return scalable_realloc(ptr, new_size);
}

void gmp_free(void *ptr, size_t /*size*/)
{
  scalable_free(ptr);
}
/**
 * Use TBB's scalable_allocator on Windows.
 * `TBBmalloc` correctly captures all allocations already,
 * however, GMP is built with MINGW since it doesn't build with MSVC,
 * which TBB has issues hooking into automatically.
 */
void gmp_blender_init_allocator()
{
  mp_set_memory_functions(gmp_alloc, gmp_realloc, gmp_free);
}
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Function
 * \{ */

#if defined(__APPLE__)
extern "C" int GHOST_HACK_getFirstFile(char buf[]);
#endif

/**
 * Blender's main function responsibilities are:
 * - setup subsystems.
 * - handle arguments.
 * - run #WM_main() event loop,
 *   or exit immediately when running in background-mode.
 */
int main(int argc,
#ifdef USE_WIN32_UNICODE_ARGS
         const char ** /*argv_c*/
#else
         const char **argv
#endif
)
{
  bContext *C;
#ifndef WITH_PYTHON_MODULE
  bArgs *ba;
#endif

  /* Ensure we free data on early-exit. */
  CreatorAtExitData app_init_data = {nullptr};
  BKE_blender_atexit_register(callback_main_atexit, &app_init_data);

/* Un-buffered `stdout` makes `stdout` and `stderr` better synchronized, and helps
 * when stepping through code in a debugger (prints are immediately
 * visible). However disabling buffering causes lock contention on windows
 * see #76767 for details, since this is a debugging aid, we do not enable
 * the un-buffered behavior for release builds. */
#ifndef NDEBUG
  setvbuf(stdout, nullptr, _IONBF, 0);
#endif

#ifdef WIN32
#  ifdef USE_WIN32_UNICODE_ARGS
  /* Win32 Unicode Arguments. */
  {
    /* NOTE: Can't use `guardedalloc` allocation here, as it's not yet initialized
     * (it depends on the arguments passed in, which is what we're getting here!). */
    wchar_t **argv_16 = CommandLineToArgvW(GetCommandLineW(), &argc);
    app_init_data.argv = static_cast<char **>(malloc(argc * sizeof(char *)));
    for (int i = 0; i < argc; i++) {
      app_init_data.argv[i] = alloc_utf_8_from_16(argv_16[i], 0);
    }
    LocalFree(argv_16);

    /* Free on early-exit. */
    app_init_data.argv_num = argc;
  }
  const char **argv = const_cast<const char **>(app_init_data.argv);
#  endif /* USE_WIN32_UNICODE_ARGS */
#endif   /* WIN32 */

#if defined(WITH_OPENGL_BACKEND) && BLI_SUBPROCESS_SUPPORT
  if (STREQ(argv[0], "--compilation-subprocess")) {
    BLI_assert(argc == 2);
    GPU_compilation_subprocess_run(argv[1]);
    return 0;
  }
#endif

  /* NOTE: Special exception for guarded allocator type switch:
   *       we need to perform switch from lock-free to fully
   *       guarded allocator before any allocation happened.
   */
  {
    int i;
    for (i = 0; i < argc; i++) {
      if (STR_ELEM(argv[i], "-d", "--debug", "--debug-memory", "--debug-all")) {
        printf("Switching to fully guarded memory allocator.\n");
        MEM_use_guarded_allocator();
        break;
      }
      if (STR_ELEM(argv[i], "--", "-c", "--command")) {
        break;
      }
    }
    MEM_init_memleak_detection();
  }

#ifdef BUILD_DATE
  {
    const time_t temp_time = build_commit_timestamp;
    const tm *tm = gmtime(&temp_time);
    if (LIKELY(tm)) {
      strftime(build_commit_date, sizeof(build_commit_date), "%Y-%m-%d", tm);
      strftime(build_commit_time, sizeof(build_commit_time), "%H:%M", tm);
    }
    else {
      const char *unknown = "date-unknown";
      STRNCPY(build_commit_date, unknown);
      STRNCPY(build_commit_time, unknown);
    }
  }
#endif

  /* Initialize logging. */
  CLG_init();
  CLG_fatal_fn_set(callback_clg_fatal);

  C = CTX_create();
#ifdef __OHOS__
  /* Hand the freshly created context to the OHOS entry-point layer.
   * On desktop platforms main() would go on to call WM_main(C) which
   * never returns, so this save was never needed there. */
  g_ohos_ctx = C;
#endif

#ifdef WITH_PYTHON_MODULE
#  ifdef __APPLE__
  environ = *_NSGetEnviron();
#  endif

#  undef main
  evil_C = C;
#endif

#ifdef WITH_BINRELOC
  br_init(nullptr);
#endif

#ifdef WITH_LIBMV
  libmv_initLogging(argv[0]);
#elif defined(WITH_CYCLES_LOGGING)
  CCL_init_logging(argv[0]);
#endif

#if defined(WITH_TBB_MALLOC) && defined(_MSC_VER) && defined(NDEBUG) && defined(WITH_GMP)
  gmp_blender_init_allocator();
#endif

  main_callback_setup();

#if defined(__APPLE__) && !defined(WITH_PYTHON_MODULE) && !defined(WITH_HEADLESS)
  /* Patch to ignore argument finder gives us (PID?). */
  if (argc == 2 && STRPREFIX(argv[1], "-psn_")) {
    static char firstfilebuf[512];

    argc = 1;

    if (GHOST_HACK_getFirstFile(firstfilebuf)) {
      argc = 2;
      argv[1] = firstfilebuf;
    }
  }
#endif

#ifdef __FreeBSD__
  fpsetmask(0);
#endif

  /* Initialize path to executable. */
  BKE_appdir_program_path_init(argv[0]);

  BLI_threadapi_init();

  DNA_sdna_current_init();

  BKE_blender_globals_init(); /* `blender.cc` */

  BKE_cpp_types_init();
  BKE_idtype_init();
  BKE_modifier_init();
  BKE_shaderfx_init();
  BKE_volumes_init();
  DEG_register_node_types();

  BKE_brush_system_init();
  RE_texture_rng_init();

  BKE_callback_global_init();

/* First test for background-mode (#Global.background). */
#ifndef WITH_PYTHON_MODULE
  ba = BLI_args_create(argc, argv); /* Skip binary path. */

  /* Ensure we free on early exit. */
  app_init_data.ba = ba;

  main_args_setup(C, ba, false);

  /* Begin argument parsing, ignore leaks so arguments that call #exit
   * (such as `--version` & `--help`) don't report leaks. */
  MEM_use_memleak_detection(false);

  /* Parse environment handling arguments. */
  BLI_args_parse(ba, ARG_PASS_ENVIRONMENT, nullptr, nullptr);

#else
  /* Using preferences or user startup makes no sense for #WITH_PYTHON_MODULE. */
  G.factory_startup = true;
#endif

  /* After parsing #ARG_PASS_ENVIRONMENT such as `--env-*`,
   * since they impact `BKE_appdir` behavior. */
  BKE_appdir_init();

  /* After parsing number of threads argument. */
  BLI_task_scheduler_init();

  /* Initialize FFTW threading support. */
  blender::fftw::initialize_float();

#ifndef WITH_PYTHON_MODULE
  /* The settings pass includes:
   * - Background-mode assignment (#Global.background), checked by other subsystems
   *   which may be skipped in background mode.
   * - The animation player may be launched which takes over argument passing,
   *   initializes the sub-systems it needs which have not yet been started.
   *   The animation player will call `exit(..)` too, so code after this call
   *   never runs when it's invoked.
   * - All the `--debug-*` flags.
   */
  BLI_args_parse(ba, ARG_PASS_SETTINGS, nullptr, nullptr);

  main_signal_setup();
#endif

  /* Must be initialized after #BKE_appdir_init to account for color-management paths. */
  IMB_init();
  /* Keep after #ARG_PASS_SETTINGS since debug flags are checked. */
  MOV_init();

  /* After #ARG_PASS_SETTINGS arguments, this is so #WM_main_playanim skips #RNA_init. */
  RNA_init();

  RE_engines_init();
  blender::bke::node_system_init();
  BKE_particle_init_rng();
  /* End second initialization. */

#if defined(WITH_PYTHON_MODULE) || defined(WITH_HEADLESS)
  /* Python module mode ALWAYS runs in background-mode (for now). */
  G.background = true;
  /* Manually using `--background` also forces the audio device. */
  BKE_sound_force_device("None");
#else
  if (G.background) {
    main_signal_setup_background();
  }
#endif

  /* Background render uses this font too. */
  BKE_vfont_builtin_register(datatoc_bfont_pfb, datatoc_bfont_pfb_size);

  /* Initialize FFMPEG if built in, also needed for background-mode if videos are
   * rendered via FFMPEG. */
  BKE_sound_init_once();

  BKE_materials_init();

#ifndef WITH_PYTHON_MODULE
  if (G.background == 0) {
    BLI_args_parse(ba, ARG_PASS_SETTINGS_GUI, nullptr, nullptr);
  }
  BLI_args_parse(ba, ARG_PASS_SETTINGS_FORCE, nullptr, nullptr);
#endif

  WM_init(C, argc, argv);

#ifndef WITH_PYTHON
  printf(
      "\n* WARNING * - Blender compiled without Python!\n"
      "this is not intended for typical usage\n\n");
#endif

#ifdef WITH_FREESTYLE
  /* Initialize Freestyle. */
  FRS_init();
  FRS_set_context(C);
#endif

/* OK we are ready for it. */
#ifndef WITH_PYTHON_MODULE
  /* Handles #ARG_PASS_FINAL. */
  BLI_args_parse(ba, ARG_PASS_FINAL, main_args_handle_load_file, C);
#endif

  /* Explicitly free data allocated for argument parsing:
   * - `ba`
   * - `argv` on WIN32.
   */
  callback_main_atexit(&app_init_data);
  BKE_blender_atexit_unregister(callback_main_atexit, &app_init_data);

  /* End argument parsing, allow memory leaks to be printed. */
  MEM_use_memleak_detection(true);

/* Paranoid, avoid accidental re-use. */
#ifndef WITH_PYTHON_MODULE
  ba = nullptr;
  (void)ba;
#endif

#ifdef USE_WIN32_UNICODE_ARGS
  argv = nullptr;
  (void)argv;
#endif

#ifndef WITH_PYTHON_MODULE
  if (G.background) {
    int exit_code;
    if (app_state.main_arg_deferred != nullptr) {
      exit_code = main_arg_deferred_handle();
      main_arg_deferred_free();
    }
    else {
      exit_code = G.is_break ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    /* Using window-manager API in background-mode is a bit odd, but works fine. */
    WM_exit(C, exit_code);
  }
  else {
    /* Not supported, although it could be made to work if needed. */
    BLI_assert(app_state.main_arg_deferred == nullptr);

    /* Shows the splash as needed. */
    WM_init_splash_on_startup(C);

#ifndef __OHOS__
    /* WM_main() is Blender's blocking GHOST event loop; it never returns.
     * On OHOS the HAP owns the event loop, so we skip this call and rely
     * on the host driving Blender_Step() from ArkTS at display refresh. */
    WM_main(C);
#endif
  }
  /* Neither #WM_exit, #WM_main return, this quiets CLANG's `unreachable-code-return` warning. */
#ifndef __OHOS__
  BLI_assert_unreachable();
#endif

#endif /* !WITH_PYTHON_MODULE */

  return 0;

} /* End of `int main(...)` function. */

#ifdef WITH_PYTHON_MODULE
void main_python_exit()
{
  WM_exit_ex((bContext *)evil_C, true, false);
  evil_C = nullptr;
}
#endif


#ifdef __OHOS__
/* ── OHOS native entry points ──────────────────────────────────────────
 * These five symbols are the entire C ABI between libblender.so and the
 * ArkTS/NAPI bridge in the HAP. Anything the host wants to observe or
 * drive on Blender must go through this surface.
 *
 * Contract:
 *   Blender_SetNativeWindow(win, w, h)
 *                           → optional but recommended before Init; sets
 *                             the OHNativeWindow* GHOST-OHOS uses to build
 *                             its Vulkan surface.
 *   Blender_Init(argc, argv) → non-zero on failure. Call exactly once
 *                              before any Step / IsRunning / Exit.
 *   Blender_Step()          → drive one frame; must be cheap enough to
 *                              call at 60/120 Hz.
 *   Blender_IsRunning()     → 1 while the WM has at least one window.
 *                              Currently a stub that always returns 1;
 *                              tighten once GHOST-OHOS is wired up.
 *   Blender_Exit()          → tear down. Safe to call multiple times.
 *
 * Visibility: extern "C" + the SHARED library's default -fvisibility
 * already puts these in .dynsym, so an additional
 * __attribute__((visibility("default"))) would be redundant. The
 * volatile function-pointer anchors below protect IsRunning / Exit
 * against LTO's dead-code elimination in case someone builds with
 * -flto -Wl,--gc-sections in the future. */

/**
 * Point Blender at its sandbox layout.
 *
 * The HAP unpacks resources into a fixed sandbox path; ArkTS calls
 * extractRawfile() into this same path before Blender_Init(). Nothing
 * needs to be passed in from the host — we hardcode the sandbox root
 * (same root GHOST_SystemPathsOHOS uses) and derive everything else.
 */
static void ohos_setup_env(void)
{
  const char *files_dir = "/data/storage/el2/base/haps/entry/files";

  static std::string blender_root  = std::string(files_dir) + "/blender";
  static std::string datafiles_dir = blender_root + "/datafiles";
  static std::string scripts_dir   = blender_root + "/scripts";
  static std::string python_home   = std::string(files_dir) + "/python_home";
  static std::string python_stdlib = python_home + "/lib/python3.11";
  static std::string python_path   = python_stdlib + ":" + python_stdlib + "/lib-dynload";
  static std::string tmp_dir       = std::string(files_dir) + "/tmp";

  /* BKE_appdir root — scripts, datafiles, presets all hang off here. */
  setenv("BLENDER_SYSTEM_RESOURCES", blender_root.c_str(),  1);
  setenv("BLENDER_SYSTEM_DATAFILES", datafiles_dir.c_str(), 1);
  setenv("BLENDER_SYSTEM_SCRIPTS",   scripts_dir.c_str(),   1);

  /* Python: PYTHONHOME alone isn't enough on OHOS — BPY_python_start
   * queries BLENDER_SYSTEM_PYTHON too, missing it → Py_ExitStatusException. */
  setenv("PYTHONHOME",              python_home.c_str(), 1);
  setenv("PYTHONPATH",              python_path.c_str(), 1);
  setenv("BLENDER_SYSTEM_PYTHON",   python_home.c_str(), 1);
  setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
  setenv("PYTHONNOUSERSITE",        "1", 1);

  mkdir(tmp_dir.c_str(), 0755);
  static std::string home_dir;
  /* ArkTS side calls EntryAbility.ensurePublicHomeDirs() via
   * DocumentPickerMode.DOWNLOAD before notifyDatafilesReady(), which
   * lets the system materialize Download/<bundleName> with persistent
   * write access. We probe it here; on success Blender's File Browser
   * lands on a user-visible public directory. Fallback to sandbox if
   * the directory isn't ready yet (first launch race, unsupported ROM). */
  /* C plan: the HAP passes the real public home (derived from the actual
   * bundle name) through DSHM_PUBLIC_HOME before Blender_Init. Keep the
   * historical path as a fallback for older HAP shells. */
  const char *env_public_home = getenv("DSHM_PUBLIC_HOME");
  const char *public_home = (env_public_home && *env_public_home)
      ? env_public_home
      : "/storage/Users/currentUser/Download/com.suwan.blenderonharmonyos";
  bool public_ok = false;
  if (public_home && *public_home) {
    const std::string probe = std::string(public_home) + "/.blender_write_probe";
    FILE *fp = fopen(probe.c_str(), "w");
    if (fp) {
      fclose(fp);
      unlink(probe.c_str());
      public_ok = true;
    }
  }
  if (public_ok) {
    home_dir = public_home;
    fprintf(stderr, "[ohos] HOME → public: %s\n", home_dir.c_str());
  }
  else {
    home_dir = files_dir;
    if (public_home && *public_home) {
      fprintf(stderr, "[ohos] HOME → sandbox fallback: %s (public %s probe errno=%d %s)\n",
              home_dir.c_str(), public_home, errno, strerror(errno));
    }
    else {
      fprintf(stderr, "[ohos] HOME → sandbox (no picker path set)\n");
    }
  }
  setenv("HOME",   home_dir.c_str(), 1);
  setenv("TMPDIR", tmp_dir.c_str(),  1);
}

/* Host must call this before Blender_Init so GHOST-OHOS can build its
 * Vulkan swapchain against the correct OHNativeWindow. Passing nullptr
 * clears the binding, useful when the surface is destroyed. */
extern "C" void Blender_SetNativeWindow(void *native_window,
                                        uint32_t w, uint32_t h)
{
  g_ghost_ohos_native_window = native_window;
  g_ghost_ohos_win_w = w;
  g_ghost_ohos_win_h = h;
}

extern "C" int Blender_Init(int argc, const char **argv)
{
  /* OHOS has no console; freopen so Blender's fprintf(stderr) diagnostics
   * land in a file we can pull via `hdc file recv`. Line-buffered so
   * `tail -f` shows output without waiting on 4K page flushes. */
  static bool redirected = false;
  if (!redirected) {
    const char *out_path = "/data/storage/el2/base/haps/entry/files/app.out.log";
    const char *err_path = "/data/storage/el2/base/haps/entry/files/app.err.log";
    std::freopen(out_path, "w", stdout);
    std::freopen(err_path, "w", stderr);
    /* Line-buffered so tail -f shows content promptly. */
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    redirected = true;
  }

  /* Currently reuses the stock main(). This runs the full desktop init
   * pipeline including BLI_args_parse; it works for factory-startup but
   * will need to be replaced by an explicit init sequence before we can
   * pass --gpu-backend=vulkan or a rawfile-based PYTHONHOME. */
  ohos_setup_env();

  return main(argc, argv);
}

extern "C" void Blender_Step(void)
{
  /* Guard against being called before Init or after Exit — either
   * scenario would dereference a null bContext deep inside the wm_*
   * calls. Silent early-return is safer than aborting the HAP. */
  if (!g_ohos_ctx) return;

  /* One frame of Blender's main loop, mirroring WM_main_iter(). Split
   * out so we can be re-entered from the host's animation callback. */
  wm_window_events_process(g_ohos_ctx);
  wm_event_do_handlers(g_ohos_ctx);
  wm_event_do_notifiers(g_ohos_ctx);
  wm_draw_update(g_ohos_ctx);
}

extern "C" int Blender_IsRunning(void)
{
  /* Stub. Once GHOST-OHOS is wired up this should return
   * (CTX_wm_manager(g_ohos_ctx)->windows.first != nullptr). */
  return 1;
}

extern "C" void Blender_Exit(void)
{
  if (g_ohos_ctx) {
    /* WM_exit frees the WM, blend file, all subsystems. Idempotent
     * only when we clear g_ohos_ctx afterwards, hence the assignment. */
    WM_exit(g_ohos_ctx, 0);
    g_ohos_ctx = nullptr;
  }
}

/* Belt-and-braces: keep the two rarely-called symbols alive even under
 * aggressive LTO. `volatile` blocks constant-folding of the pointer,
 * and __attribute__((used)) tells the compiler to emit the definition
 * even if no local translation unit references it. */
__attribute__((used)) int  (* volatile _ohos_force_Blender_IsRunning)(void) = &Blender_IsRunning;
__attribute__((used)) void (* volatile _ohos_force_Blender_Exit)(void)      = &Blender_Exit;
#endif  /* __OHOS__ */

/** \} */

#ifdef __OHOS__
/* STAGE_B_FORCE_LINK_ON_POINTER
 *
 * Blender_OnPointerEvent is defined in bf_intern_ghost's GHOST_SystemOHOS.cc.
 * Nothing in the Blender core references it (the ArkTS host calls it via
 * dlsym), so without a force-link ref the static-lib linker would drop
 * the whole translation unit and dlsym would return NULL at runtime. */
extern "C" void Blender_OnPointerEvent(int kind, int button, float fx, float fy);
__attribute__((used)) static void (*g_force_link_on_pointer_event)(int, int, float, float) =
    &Blender_OnPointerEvent;
#endif

#ifdef __OHOS__
/* STAGE_D_FORCE_LINK_ON_SURFACE_RESIZE
 *
 * Blender_OnSurfaceResize lives in bf_intern_ghost and is only called
 * externally (via dlsym from the ArkTS host). Force-link so the static
 * lib linker does not drop the translation unit. */
extern "C" void Blender_OnSurfaceResize(uint32_t w, uint32_t h);
__attribute__((used)) static void (*g_force_link_on_surface_resize)(uint32_t, uint32_t) =
    &Blender_OnSurfaceResize;
#endif

#ifdef __OHOS__
/* STAGE_G_FORCE_LINK_ON_KEY_EVENT
 *
 * Blender_OnKeyEvent lives in bf_intern_ghost and is called via dlsym
 * from the ArkTS host. Force-link so the static-lib linker does not
 * drop the translation unit. */
extern "C" void Blender_OnKeyEvent(int wid, int action, int keyCode, int metaState);
__attribute__((used)) static void (*g_force_link_on_key_event)(int, int, int, int) =
    &Blender_OnKeyEvent;
#endif

#ifdef __OHOS__
/* STAGE_H_FORCE_LINK
 *
 * All Blender_OnSubWindow*  /  Blender_SetSubWindowHooks exports live in
 * bf_intern_ghost. Keep the static-lib linker from dropping them. */
extern "C" void Blender_SetSubWindowHooks(void (*)(int, int, int, const char *),
                                          void (*)(int));
extern "C" void Blender_OnSubWindowSurfaceReady(int, void *, uint32_t, uint32_t);
extern "C" void Blender_OnSubWindowClosed(int);
__attribute__((used)) static void (*g_force_link_set_sub_hooks)(
    void (*)(int, int, int, const char *), void (*)(int)) =
    &Blender_SetSubWindowHooks;
__attribute__((used)) static void (*g_force_link_on_sub_ready)(
    int, void *, uint32_t, uint32_t) = &Blender_OnSubWindowSurfaceReady;
__attribute__((used)) static void (*g_force_link_on_sub_closed)(int) =
    &Blender_OnSubWindowClosed;
#endif
