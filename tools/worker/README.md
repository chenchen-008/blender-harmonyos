# Native headless worker (PoC)

These files are the proof-of-concept used to start Blender in a HarmonyOS
native child process.

- blender_worker_server.cpp: exports Main(NativeChildProcess_Args), loads
  libblender.so with dlopen, calls Blender_Init(--background), then serves
  Python jobs over a Unix socket.
- blender_worker_once.c: one-shot version that runs a --python-expr and exits.
- worker_test.c: minimal child process smoke test.

Compile with the OHOS Linux clang:

    CC=/opt/ohos-sdk/linux/native/llvm/bin/clang
    CXX=/opt/ohos-sdk/linux/native/llvm/bin/clang++
    SYSROOT=/opt/ohos-sdk/linux/native/sysroot

    $CXX --target=aarch64-linux-ohos --sysroot=$SYSROOT \
      -shared -fPIC -O2 -o libblender_worker.so blender_worker_server.cpp -ldl

The .so must be placed in the HAP native libs and started with:

    OH_Ability_StartNativeChildProcess("libblender_worker.so:Main", args, options, &pid)

Blender datafiles and python_home are expected under the app sandbox:

    /data/storage/el2/base/haps/entry/files/blender
    /data/storage/el2/base/haps/entry/files/python_home

See ../../dshm-blender/docs/headless-worker-PoC.md for results and limits.
