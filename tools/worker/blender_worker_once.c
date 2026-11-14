#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <string.h>

typedef struct { char *entryParams; struct { void *head; } fdList; } NCPArgs;
typedef int (*BlenderInitFn)(int, const char **);

static const char *kLog = "/data/storage/el2/base/haps/entry/files/headless.log";
static const char *kOut = "/data/storage/el2/base/haps/entry/files/headless_result.txt";

static void logline(int fd, const char *s) {
    if (fd >= 0) { dprintf(fd, "%s\n", s); }
}

void Main(NCPArgs args) {
    int fd = open(kLog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    logline(fd, "worker start");
    void *h = dlopen("libblender.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        char buf[512]; snprintf(buf, sizeof(buf), "dlopen failed: %s", dlerror());
        logline(fd, buf); close(fd); return;
    }
    BlenderInitFn init = (BlenderInitFn)dlsym(h, "Blender_Init");
    if (!init) { logline(fd, "dlsym Blender_Init failed"); close(fd); return; }
    const char *expr =
        "import bpy,numpy\n"
        "bpy.ops.mesh.primitive_cube_add()\n"
        "o=bpy.context.object\n"
        "m=o.modifiers.new('S','SUBSURF'); m.levels=2\n"
        "dg=bpy.context.evaluated_depsgraph_get(); me=o.evaluated_get(dg).to_mesh()\n"
        "open('/data/storage/el2/base/haps/entry/files/headless_result.txt','w').write(f'VER {bpy.app.version_string} NUMPY {numpy.__version__} SUB {len(me.vertices)} {len(me.polygons)}')\n";
    const char *argv[] = {"blender", "--background", "--factory-startup", "--python-expr", expr};
    logline(fd, "calling Blender_Init");
    int rc = init(5, argv);
    char buf[128]; snprintf(buf, sizeof(buf), "Blender_Init rc=%d", rc);
    logline(fd, buf);
    close(fd);
}
