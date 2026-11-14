# Blender 4.5 OHOS (HarmonyOS/OpenHarmony) platform configuration
# Used together with build_blender.sh

message(STATUS "[OHOS] Loading HarmonyOS platform config")

# ---- Single entry point: deps dist root (passed via -D from build_blender.sh) ----
if(NOT DEFINED OHOS_DIST_DIR)
  message(FATAL_ERROR "OHOS_DIST_DIR must be set (point to deps/dist/arm64-v8a)")
endif()

# Auto-derive Python directory from OHOS_DIST_DIR
get_filename_component(_ohos_deps_root "${OHOS_DIST_DIR}" DIRECTORY)
set(OHOS_PYTHON_DIR "${_ohos_deps_root}/python-arm64-v8a" CACHE PATH "OHOS Python install root")
unset(_ohos_deps_root)

set(OHOS_INC "${OHOS_DIST_DIR}/include")
set(OHOS_LIB "${OHOS_DIST_DIR}/lib")

# ---- Global include / link paths ----
include_directories(SYSTEM "${OHOS_INC}")
include_directories(SYSTEM "${OHOS_INC}/freetype2")
link_directories("${OHOS_LIB}")

# ---- OHOS platform definitions ----
set(__OHOS__ TRUE)
add_definitions(-D__OHOS__=1 -DWITH_OHOS_PTHREAD_STUB -D_GNU_SOURCE)
add_compile_options(-Wno-unused-command-line-argument)

# ---- Global dependency search paths ----
set(CMAKE_PREFIX_PATH "${OHOS_DIST_DIR}" "${OHOS_PYTHON_DIR}" ${CMAKE_PREFIX_PATH})

# ---- Python static linking ----
# Version auto-detected from the installed headers
file(GLOB _py_include_dir "${OHOS_PYTHON_DIR}/include/python*")
if(_py_include_dir)
  get_filename_component(_py_version_dir "${_py_include_dir}" NAME)
  string(REPLACE "python" "" _py_ver "${_py_version_dir}")
  message(STATUS "[OHOS-PYTHON] Detected version: ${_py_ver} (from ${_py_version_dir})")
else()
  message(FATAL_ERROR "Python headers not found under ${OHOS_PYTHON_DIR}/include/")
endif()

set(PYTHON_VERSION        "${_py_ver}")
set(PYTHON_INCLUDE_DIR    "${OHOS_PYTHON_DIR}/include/python${_py_ver}")
set(PYTHON_INCLUDE_DIRS   "${PYTHON_INCLUDE_DIR}")
set(PYTHON_LIBRARY        "${OHOS_PYTHON_DIR}/lib/libpython${_py_ver}.so")
set(PYTHON_LIBPATH        "${OHOS_PYTHON_DIR}/lib")
set(PYTHON_ROOT_DIR       "${OHOS_PYTHON_DIR}")

set(WITH_PYTHON_INSTALL       OFF CACHE BOOL "" FORCE)
set(WITH_PYTHON_INSTALL_NUMPY OFF CACHE BOOL "" FORCE)
set(WITH_PYTHON_MODULE        OFF CACHE BOOL "" FORCE)
set(WITH_PYTHON_SAFETY        OFF CACHE BOOL "" FORCE)

include_directories(SYSTEM "${PYTHON_INCLUDE_DIR}")
link_directories("${PYTHON_LIBPATH}")
message(STATUS "[OHOS-PYTHON] ${PYTHON_LIBRARY}")

# ---- libepoxy ----
set(WITH_SYSTEM_EPOXY ON CACHE BOOL "" FORCE)
set(Epoxy_FOUND TRUE)
set(Epoxy_INCLUDE_DIR "${OHOS_INC}")
set(Epoxy_LIBRARY     "${OHOS_LIB}/libepoxy.so")
set(Epoxy_LIBRARIES   "${Epoxy_LIBRARY}")
set(Epoxy_INCLUDE_DIRS "${Epoxy_INCLUDE_DIR}")

# ---- FreeType + Brotli ----
set(FREETYPE_FOUND TRUE)
set(FREETYPE_INCLUDE_DIRS "${OHOS_INC}" "${OHOS_INC}/freetype2")
set(FREETYPE_LIBRARY "${OHOS_LIB}/libfreetype.so")
set(FREETYPE_LIBRARIES ${FREETYPE_LIBRARY})
set(BROTLI_FOUND TRUE)
set(BROTLI_INCLUDE_DIRS "${OHOS_INC}")
set(BROTLIDEC_LIBRARY    "${OHOS_LIB}/libbrotlidec.so")
set(BROTLICOMMON_LIBRARY "${OHOS_LIB}/libbrotlicommon.so")
set(BROTLI_LIBRARIES ${BROTLIDEC_LIBRARY} ${BROTLICOMMON_LIBRARY})

# ---- OpenEXR (needed for studio-light world HDRI .exr files) ----
set(OPENEXR_INCLUDE_DIRS
  "${OHOS_DIST_DIR}/include"
  "${OHOS_DIST_DIR}/include/OpenEXR"
  "${OHOS_DIST_DIR}/include/Imath"
  CACHE STRING "OpenEXR include dirs")
set(OPENEXR_LIBRARIES
  "${OHOS_DIST_DIR}/lib/libOpenEXR-3_2.so"
  "${OHOS_DIST_DIR}/lib/libOpenEXRCore-3_2.so"
  "${OHOS_DIST_DIR}/lib/libIex-3_2.so"
  "${OHOS_DIST_DIR}/lib/libIlmThread-3_2.so"
  "${OHOS_DIST_DIR}/lib/libImath-3_1.so"
  CACHE STRING "OpenEXR libraries")

# ---- Build as shared library (libblender.so for ArkTS/NAPI) ----

# ---- Graphics backend: Vulkan only ----
set(WITH_VULKAN_BACKEND   ON  CACHE BOOL "" FORCE)
set(WITH_OPENGL_BACKEND   OFF CACHE BOOL "" FORCE)
set(WITH_METAL_BACKEND    OFF CACHE BOOL "" FORCE)

# ---- GHOST: use DUMMY first to pass compilation, switch to OHOS later ----
set(WITH_GHOST_DUMMY      ON  CACHE BOOL "" FORCE)
set(WITH_GHOST_OHOS       OFF CACHE BOOL "" FORCE)
set(WITH_GHOST_X11        OFF CACHE BOOL "" FORCE)
set(WITH_GHOST_WAYLAND    OFF CACHE BOOL "" FORCE)
set(WITH_GHOST_SDL        OFF CACHE BOOL "" FORCE)

# ---- Disable desktop / mobile-unavailable features ----
set(WITH_X11              OFF CACHE BOOL "" FORCE)
set(WITH_SDL              OFF CACHE BOOL "" FORCE)
set(WITH_OPENAL           OFF CACHE BOOL "" FORCE)
set(WITH_XR_OPENXR        OFF CACHE BOOL "" FORCE)
set(WITH_INPUT_IME        OFF CACHE BOOL "" FORCE)
set(WITH_INPUT_NDOF       OFF CACHE BOOL "" FORCE)
set(WITH_INSTALL_PORTABLE OFF CACHE BOOL "" FORCE)
set(WITH_DOC_MANPAGE      OFF CACHE BOOL "" FORCE)
set(WITH_BUILDINFO        OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_GLOG      OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_GFLAGS    OFF CACHE BOOL "" FORCE)

# ---- Experimental: disable unverified modules, enable gradually ----
set(WITH_CYCLES             OFF CACHE BOOL "" FORCE)
set(WITH_LIBMV              OFF CACHE BOOL "" FORCE)
set(WITH_CYCLES_OSL       OFF CACHE BOOL "" FORCE)
set(WITH_AUDASPACE        OFF CACHE BOOL "" FORCE)
set(WITH_USD              OFF CACHE BOOL "" FORCE)
set(WITH_MATERIALX        OFF CACHE BOOL "" FORCE)
set(WITH_BOOST            ON  CACHE BOOL "" FORCE)
set(WITH_TBB              ON  CACHE BOOL "" FORCE)
set(WITH_FFTW3            OFF CACHE BOOL "" FORCE)
set(WITH_POTRACE          OFF CACHE BOOL "" FORCE)
set(WITH_HARU             OFF CACHE BOOL "" FORCE)
set(WITH_GMP              OFF CACHE BOOL "" FORCE)
set(WITH_MANIFOLD         OFF CACHE BOOL "" FORCE)
set(WITH_OPENVDB          ON  CACHE BOOL "" FORCE)
set(WITH_NANOVDB          ON  CACHE BOOL "" FORCE)
set(WITH_OPENSUBDIV       OFF CACHE BOOL "" FORCE)
set(WITH_OPENIMAGEDENOISE ON  CACHE BOOL "" FORCE)
set(WITH_BULLET           OFF CACHE BOOL "" FORCE)
set(WITH_ALEMBIC          OFF CACHE BOOL "" FORCE)
set(WITH_MOD_FLUID        OFF CACHE BOOL "" FORCE)
set(WITH_MOD_OCEANSIM     OFF CACHE BOOL "" FORCE)
set(WITH_MEM_JEMALLOC     OFF CACHE BOOL "" FORCE)
set(WITH_IMAGE_OPENJPEG   ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_WEBP       ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_CINEON     ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_HDR        ON  CACHE BOOL "" FORCE)
set(WITH_HYDRA            OFF CACHE BOOL "" FORCE)
set(WITH_PUGIXML          OFF CACHE BOOL "" FORCE)
set(WITH_OPENCOLORIO      ON  CACHE BOOL "" FORCE)
set(WITH_CPU_SIMD         OFF CACHE BOOL "" FORCE)
set(WITH_INTERNATIONAL    ON  CACHE BOOL "" FORCE)
set(WITH_CODEC_FFMPEG     OFF CACHE BOOL "" FORCE)
set(WITH_CODEC_SNDFILE    OFF CACHE BOOL "" FORCE)
set(WITH_FFMPEG           OFF CACHE BOOL "" FORCE)
set(WITH_OPENIMAGEIO      ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_OPENEXR    ON  CACHE BOOL "" FORCE)
set(WITH_IMAGE_TIFF       ON  CACHE BOOL "" FORCE)

# ---- Linker: allow undefined Vulkan/EGL symbols (resolved at runtime) ----
set(CMAKE_SHARED_LINKER_FLAGS
  "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--allow-shlib-undefined -Wl,--export-dynamic")

message(STATUS "[OHOS] platform config loaded")
set(WITH_OPENCOLLADA OFF CACHE BOOL "" FORCE)

# ---- Vulkan runtime library ----
find_library(VULKAN_LIBRARY vulkan)
if(VULKAN_LIBRARY)
  message(STATUS "[OHOS-VULKAN] ${VULKAN_LIBRARY}")
  list(APPEND PLATFORM_LINKLIBS ${VULKAN_LIBRARY})
endif()

# ---- Additional runtime libraries ----
# Compression / image / font / color libs pulled in via dist/lib
list(APPEND PLATFORM_LINKLIBS
  "${OHOS_LIB}/libz.so"
  "${OHOS_LIB}/libzstd.so"
  "${OHOS_LIB}/libbz2.so"
  "${OHOS_LIB}/libdeflate.so"
  "${OHOS_LIB}/libjpeg.so"
  "${OHOS_LIB}/libpng.so"
  "${OHOS_LIB}/libtiff.so"
  "${OHOS_LIB}/libwebp.so"
  "${OHOS_LIB}/libfreetype.so"
  "${OHOS_LIB}/libexpat.so"
  "${OHOS_LIB}/libfmt.so"
  "${OHOS_LIB}/libtbb.so"
  "${OHOS_LIB}/libImath.so"
  "${OHOS_LIB}/libIex.so"
  "${OHOS_LIB}/libIlmThread.so"
  "${OHOS_LIB}/libOpenEXR.so"
  "${OHOS_LIB}/libOpenImageIO.so"
  "${OHOS_LIB}/libOpenImageIO_Util.so"
  "${OHOS_LIB}/libOpenColorIO.so"
  "${OHOS_LIB}/libshaderc_shared.so"
  "${PYTHON_LIBRARY}"
  # Python native module dependencies (statically embedded in libpython3.11.a)
  "${OHOS_LIB}/liblzma.so"
  "${OHOS_LIB}/libsqlite3.a"
  "${OHOS_LIB}/libssl.a"
  "${OHOS_LIB}/libcrypto.a"
  "${OHOS_LIB}/libffi.a"
  "${OHOS_LIB}/libgdbm.a"
  "${OHOS_LIB}/libncurses.a"
  "${OHOS_LIB}/libpanel.a"
  "${OHOS_LIB}/libmenu.a"
  "${OHOS_LIB}/libreadline.a"
  "${OHOS_LIB}/libhistory.a"
  "${OHOS_LIB}/libuuid.a"
  # gettext (referenced by BLT_translation stubs even with WITH_INTERNATIONAL=OFF)
  "${OHOS_LIB}/libintl.a"
  # OHOS system libs
  "$ENV{OHOS_SDK}/native/sysroot/usr/lib/aarch64-linux-ohos/libhilog_ndk.z.so"
  dl
  m
  pthread
)

# ---- Cycles (CPU renderer, mirroring macOS arm64 setup) ----
set(WITH_CYCLES_EMBREE            ON  CACHE BOOL "" FORCE)
set(WITH_CYCLES_OPENIMAGEDENOISE  ON  CACHE BOOL "" FORCE)
set(WITH_CYCLES_PATH_GUIDING      ON  CACHE BOOL "" FORCE)

# ---- Find dependencies ----------------------------------------------------
#
# NOTE: platform_ohos.cmake is loaded twice: once via `cmake -C` (initial
# cache, before project() enables C/CXX) and once from CMakeLists.txt
# include(). find_package() must only run in the second pass (PROJECT_NAME
# set): the first pass would fail because CheckSymbolExists() etc. need
# C/CXX enabled.
#
# Every dependency below is REQUIRED: a missing library aborts configure
# instead of silently disabling a feature (see the OpenEXR/.exr incident).
# Point each finder at the deps install root first - the stock Find*.cmake
# modules default to /opt/lib/... and would otherwise miss the OHOS deps.
if(PROJECT_NAME)
  set(OPENIMAGEIO_ROOT_DIR      "${OHOS_DIST_DIR}")
  set(EMBREE_ROOT_DIR           "${OHOS_DIST_DIR}")
  set(OPENVDB_ROOT_DIR          "${OHOS_DIST_DIR}")
  set(OPENIMAGEDENOISE_ROOT_DIR "${OHOS_DIST_DIR}")
  set(OPENCOLORIO_ROOT_DIR      "${OHOS_DIST_DIR}")
  set(OPENSUBDIV_ROOT_DIR       "${OHOS_DIST_DIR}")

  # CONFIG-mode package locations under deps/lib/cmake/.
  set(TBB_DIR               "${OHOS_DIST_DIR}/lib/cmake/TBB"              CACHE PATH "" FORCE)
  set(OpenImageIO_DIR       "${OHOS_DIST_DIR}/lib/cmake/OpenImageIO"       CACHE PATH "" FORCE)
  set(OpenImageDenoise_DIR  "${OHOS_DIST_DIR}/lib/cmake/OpenImageDenoise-2.3.3" CACHE PATH "" FORCE)
  set(OpenSubdiv_DIR        "${OHOS_DIST_DIR}/lib/cmake/OpenSubdiv"        CACHE PATH "" FORCE)
  set(OpenVDB_DIR           "${OHOS_DIST_DIR}/lib/cmake/OpenVDB"           CACHE PATH "" FORCE)
  set(openpgl_DIR           "${OHOS_DIST_DIR}/lib/cmake/openpgl-0.6.0"     CACHE PATH "" FORCE)
  set(Embree_DIR            "${OHOS_DIST_DIR}/lib/cmake/embree-4.3.3"      CACHE PATH "" FORCE)
  set(OpenColorIO_DIR       "${OHOS_DIST_DIR}/lib/cmake/OpenColorIO"       CACHE PATH "" FORCE)
  set(Imath_DIR             "${OHOS_DIST_DIR}/lib/cmake/Imath"             CACHE PATH "" FORCE)
  set(OpenEXR_DIR           "${OHOS_DIST_DIR}/lib/cmake/OpenEXR"           CACHE PATH "" FORCE)
  set(fmt_DIR               "${OHOS_DIST_DIR}/lib/cmake/fmt"               CACHE PATH "" FORCE)
  set(tiff_DIR              "${OHOS_DIST_DIR}/lib/cmake/tiff"              CACHE PATH "" FORCE)
  set(JPEG_DIR              "${OHOS_DIST_DIR}/lib/cmake/libjpeg-turbo"     CACHE PATH "" FORCE)
  set(expat_DIR             "${OHOS_DIST_DIR}/lib/cmake/expat-2.6.2"       CACHE PATH "" FORCE)
  set(pystring_DIR          "${OHOS_DIST_DIR}/lib/cmake/pystring"          CACHE PATH "" FORCE)
  set(yaml-cpp_DIR          "${OHOS_DIST_DIR}/lib/cmake/yaml-cpp"          CACHE PATH "" FORCE)
  set(minizip-ng_DIR        "${OHOS_DIST_DIR}/lib/cmake/minizip-ng"        CACHE PATH "" FORCE)
  set(zstd_DIR              "${OHOS_DIST_DIR}/lib/cmake/zstd"              CACHE PATH "" FORCE)

  # Module-mode variables: pre-seed the find_path/find_library targets so the
  # Find*.cmake modules use the exact deps locations without searching.
  set(OPENIMAGEIO_INCLUDE_DIR       "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(OPENIMAGEIO_INCLUDE_DIRS      "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(OPENIMAGEIO_LIBRARY           "${OHOS_DIST_DIR}/lib/libOpenImageIO.so" CACHE PATH "" FORCE)
  set(OPENIMAGEIO_UTIL_LIBRARY      "${OHOS_DIST_DIR}/lib/libOpenImageIO_Util.so" CACHE PATH "" FORCE)
  set(OPENIMAGEIO_LIBRARIES         "${OPENIMAGEIO_LIBRARY};${OPENIMAGEIO_UTIL_LIBRARY}" CACHE PATH "" FORCE)
  set(OPENCOLORIO_INCLUDE_DIRS      "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(OPENCOLORIO_LIBRARIES         "${OHOS_DIST_DIR}/lib/libOpenColorIO.so" CACHE PATH "" FORCE)
  set(OPENIMAGEDENOISE_INCLUDE_DIR  "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(OPENIMAGEDENOISE_INCLUDE_DIRS "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(OPENIMAGEDENOISE_LIBRARY      "${OHOS_DIST_DIR}/lib/libOpenImageDenoise.so" CACHE PATH "" FORCE)
  set(OPENIMAGEDENOISE_OPENIMAGEDENOISE_LIBRARY "${OHOS_DIST_DIR}/lib/libOpenImageDenoise.so" CACHE PATH "" FORCE)
  set(OPENIMAGEDENOISE_LIBRARIES    "${OHOS_DIST_DIR}/lib/libOpenImageDenoise.so" CACHE PATH "" FORCE)
  set(OPENVDB_INCLUDE_DIR           "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(OPENVDB_INCLUDE_DIRS          "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(OPENVDB_LIBRARY               "${OHOS_DIST_DIR}/lib/libopenvdb.so" CACHE PATH "" FORCE)
  set(OPENVDB_LIBRARIES             "${OHOS_DIST_DIR}/lib/libopenvdb.so" CACHE PATH "" FORCE)
  set(EMBREE_EMBREE4_LIBRARY        "${OHOS_DIST_DIR}/lib/libembree4.so" CACHE PATH "" FORCE)
  set(EMBREE_INCLUDE_DIR            "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(EMBREE_INCLUDE_DIRS           "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(EMBREE_LIBRARIES              "${OHOS_DIST_DIR}/lib/libembree4.so" CACHE PATH "" FORCE)
  set(OPENJPEG_INCLUDE_DIR          "${OHOS_DIST_DIR}/include/openjpeg-2.5" CACHE PATH "" FORCE)
  set(OPENJPEG_INCLUDE_DIRS         "${OHOS_DIST_DIR}/include/openjpeg-2.5" CACHE PATH "" FORCE)
  set(OPENJPEG_LIBRARY              "${OHOS_DIST_DIR}/lib/libopenjp2.so" CACHE PATH "" FORCE)
  set(OPENJPEG_LIBRARIES            "${OHOS_DIST_DIR}/lib/libopenjp2.so" CACHE PATH "" FORCE)
  set(WEBP_INCLUDE_DIR              "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(WEBP_INCLUDE_DIRS             "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(WEBP_WEBP_LIBRARY             "${OHOS_DIST_DIR}/lib/libwebp.so" CACHE PATH "" FORCE)
  set(WEBP_LIBRARIES                "${OHOS_DIST_DIR}/lib/libwebp.so" CACHE PATH "" FORCE)
  set(TIFF_INCLUDE_DIR              "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(TIFF_INCLUDE_DIRS             "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(TIFF_LIBRARY                  "${OHOS_DIST_DIR}/lib/libtiff.so" CACHE PATH "" FORCE)
  set(TIFF_LIBRARIES                "${OHOS_DIST_DIR}/lib/libtiff.so" CACHE PATH "" FORCE)
  set(OPENPGL_INCLUDE_DIR           "${OHOS_DIST_DIR}/include/openpgl" CACHE PATH "" FORCE)
  set(OPENPGL_LIBRARIES             "${OHOS_DIST_DIR}/lib/libopenpgl.a" CACHE PATH "" FORCE)
  set(BLOSC_LIBRARIES               "${OHOS_DIST_DIR}/lib/libblosc.so" CACHE PATH "" FORCE)
  set(TBB_INCLUDE_DIRS              "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(TBB_LIBRARIES                 "${OHOS_DIST_DIR}/lib/libtbb.so" CACHE PATH "" FORCE)
  set(Boost_INCLUDE_DIR             "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)
  set(BOOST_INCLUDE_DIR             "${OHOS_DIST_DIR}/include"  CACHE PATH "" FORCE)

  # ---- Required packages --------------------------------------------------
  if(WITH_TBB)
    find_package(TBB REQUIRED)
  endif()

  if(WITH_BOOST)
    # Blender only needs Boost headers (no link); FindBoost is brittle on
    # cross builds, so verify the headers explicitly instead.
    if(NOT EXISTS "${OHOS_DIST_DIR}/include/boost/version.hpp")
      message(FATAL_ERROR "Boost headers not found: ${OHOS_DIST_DIR}/include/boost/version.hpp")
    endif()
    set(BOOST_INCLUDE_DIR "${OHOS_DIST_DIR}/include" CACHE PATH "" FORCE)
    set(Boost_INCLUDE_DIR "${OHOS_DIST_DIR}/include" CACHE PATH "" FORCE)
    set(BOOST_LIBRARIES)
  endif()

  if(WITH_OPENSUBDIV)
    find_package(OpenSubdiv CONFIG REQUIRED)
    # The CONFIG package only exports imported targets; Blender's
    # intern/opensubdiv expects the module-mode variables.
    get_target_property(OPENSUBDIV_LIBRARIES OpenSubdiv::osdCPU LOCATION)
    get_target_property(OPENSUBDIV_INCLUDE_DIRS OpenSubdiv::osdCPU INTERFACE_INCLUDE_DIRECTORIES)
    # The deps build of OpenSubdiv was configured without OpenGL so its GPU
    # sources (glslPatchShaderSource etc.) were not compiled in. Provide the
    # GLSL shader-source helpers as a small supplementary static lib.
    list(APPEND OPENSUBDIV_LIBRARIES "${OHOS_DIST_DIR}/lib/libosd_glsl.a")
    message(STATUS "Found OpenSubdiv: ${OPENSUBDIV_LIBRARIES}")
  endif()

  if(WITH_OPENVDB)
    find_package(OpenVDB REQUIRED)
    list(APPEND OPENVDB_LIBRARIES ${BLOSC_LIBRARIES})
  endif()

  if(WITH_IMAGE_OPENJPEG)
    find_package(OpenJPEG REQUIRED)
  endif()

  find_package(OpenImageIO CONFIG REQUIRED)

  if(WITH_CYCLES AND WITH_CYCLES_EMBREE)
    find_package(Embree 4.0.0 REQUIRED)
  endif()

  if(WITH_OPENIMAGEDENOISE)
    find_package(OpenImageDenoise REQUIRED)
  endif()

  if(WITH_CYCLES AND WITH_CYCLES_PATH_GUIDING)
    find_package(openpgl CONFIG REQUIRED)
    get_target_property(OPENPGL_LIBRARIES openpgl::openpgl LOCATION)
    get_target_property(OPENPGL_INCLUDE_DIR openpgl::openpgl INTERFACE_INCLUDE_DIRECTORIES)
    message(STATUS "Found OpenPGL: ${OPENPGL_LIBRARIES}")
  endif()
endif()
