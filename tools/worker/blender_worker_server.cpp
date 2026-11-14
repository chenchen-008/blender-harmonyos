#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <string.h>

typedef struct { char *entryParams; struct { void *head; } fdList; } NCPArgs;
typedef int (*BlenderInitFn)(int, const char **);

static const char *kLog = "/data/storage/el2/base/haps/entry/files/worker_server.log";

static void logline(int fd, const char *s) { if (fd >= 0) dprintf(fd, "%s\n", s); }

extern "C" void Main(NCPArgs args) {
    int fd = open(kLog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    logline(fd, "worker server start");
    void *h = dlopen("libblender.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { char b[512]; snprintf(b, sizeof(b), "dlopen: %s", dlerror()); logline(fd, b); close(fd); return; }
    BlenderInitFn init = (BlenderInitFn)dlsym(h, "Blender_Init");
    if (!init) { logline(fd, "no Blender_Init"); close(fd); return; }
    const char *expr = R"PY(
import bpy, sys, os, json, socket, traceback, io
sock_path = '/data/storage/el2/base/haps/entry/files/blender_worker.sock'
ready_path = sock_path + '.ready'
try:
    os.unlink(sock_path)
except OSError:
    pass
try:
    os.unlink(ready_path)
except OSError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(sock_path)
server.listen(4)
open(ready_path, 'w').write(str(os.getpid()))
while True:
    conn, _ = server.accept()
    data = b''
    while not data.endswith(b'\n'):
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    if not data:
        conn.close()
        continue
    try:
        req = json.loads(data.decode('utf-8'))
    except Exception:
        conn.close()
        continue
    if req.get('quit'):
        conn.close()
        break
    out = io.StringIO()
    err = io.StringIO()
    old_out, old_err = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = out, err
    resp = {'ok': False, 'stdout': '', 'stderr': '', 'error': ''}
    try:
        ns = {'bpy': bpy, '__name__': '__blender_worker__'}
        exec(req.get('code', ''), ns, ns)
        resp['ok'] = True
    except Exception:
        resp['error'] = traceback.format_exc()
    finally:
        sys.stdout, sys.stderr = old_out, old_err
        resp['stdout'] = out.getvalue()
        resp['stderr'] = err.getvalue()
    try:
        conn.sendall((json.dumps(resp) + '\n').encode('utf-8'))
    finally:
        conn.close()
try:
    os.unlink(sock_path)
except OSError:
    pass
)PY";
    const char *argv[] = {"blender", "--background", "--factory-startup", "--python-expr", expr};
    logline(fd, "calling Blender_Init");
    int rc = init(5, argv);
    char b[128]; snprintf(b, sizeof(b), "Blender_Init rc=%d", rc); logline(fd, b);
    close(fd);
}
