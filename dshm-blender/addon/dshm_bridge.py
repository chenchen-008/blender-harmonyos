# -*- coding: utf-8 -*-
# DSHM Blender Bridge - Blender-side addon for DSHM (DeepSeek Harness Mobile)
# Tested target: Blender 4.5.x on HarmonyOS (also works on desktop 4.2+).
#
# Protocol: newline-delimited JSON over TCP (127.0.0.1:9876 by default).
#   request : {"type":"ping","params":{},"token":"..."} then LF
#   success : {"status":"success","result":{...}} then LF
#   failure : {"status":"error","message":"..."} then LF
#
# Threading: socket threads only enqueue jobs; every bpy call runs on the main
# thread via bpy.app.timers, so the addon never touches bpy from a worker.
bl_info = {
    "name": "DSHM Blender Bridge",
    "author": "DSHM",
    "version": (0, 1, 0),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > DSHM",
    "description": "Local TCP bridge so DSHM can see and drive Blender",
    "category": "Interface",
}

import base64
import ctypes
import hmac
import io
import json
import math
import os
import queue
import socket
import sys
import tempfile
import threading
import time
import traceback

import bpy
import mathutils

TAG = "[dshm-bridge]"
DEFAULT_PORT = 9876

# Blender addon translations. Only used when the Blender UI language is 简体中文.
TRANSLATIONS = {
    "zh_HANS": {
        ("*", "DSHM Bridge"): "DSHM 桥",
        ("*", "Host"): "主机",
        ("*", "Port"): "端口",
        ("*", "Token"): "令牌",
        ("*", "Auto-Start"): "自动启动",
        ("*", "Keep Alive in Background"): "后台保活",
        ("*", "Status: listening"): "状态：监听中",
        ("*", "Status: stopped"): "状态：已停止",
        ("*", "Start"): "启动",
        ("*", "Stop"): "停止",
        ("*", "Start DSHM Bridge"): "启动 DSHM 桥",
        ("*", "Stop DSHM Bridge"): "停止 DSHM 桥",
        ("*", "DSHM Blender Bridge"): "DSHM Blender 桥",
        ("*", "Connection Info"): "连接信息",
        ("*", "Start Bridge"): "启动桥",
        ("*", "Stop Bridge"): "停止桥",
        ("*", "Keep alive: on"): "后台保活：开",
        ("*", "Keep alive: off"): "后台保活：关",
        }
}


def log(*args):
    try:
        print(TAG, *args, flush=True)
    except Exception:
        pass


def _jsonable(value, depth=0):
    if depth > 4:
        return str(value)
    if value is None or isinstance(value, (bool, int, float, str)):
        if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
            return None
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable(v, depth + 1) for v in value]
    if isinstance(value, dict):
        return {str(k): _jsonable(v, depth + 1) for k, v in value.items()}
    try:
        return [_jsonable(v, depth + 1) for v in value]
    except Exception:
        return str(value)


class Kernel:
    """Persistent namespace shared by every execute_code call (REPL semantics)."""

    def run(self, path):
        with open(os.path.expanduser(path), "r", encoding="utf-8") as handle:
            code = handle.read()
        exec(compile(code, path, "exec"), self.__dict__)
        return True


KERNEL = Kernel()


class BridgeServer:
    def __init__(self, host, port, token):
        self.host = host
        self.port = int(port)
        self.token = token or ""
        self.socket = None
        self.thread = None
        self.running = False
        self.jobs = queue.Queue()

    def start(self):
        if self.running:
            return False
        if self.host not in ("127.0.0.1", "localhost", "::1") and not self.token:
            raise RuntimeError("non-loopback host requires a token")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.host, self.port))
        sock.listen(8)
        self.socket = sock
        self.running = True
        self.thread = threading.Thread(target=self._accept_loop, name="dshm-bridge-accept", daemon=True)
        self.thread.start()
        if not bpy.app.timers.is_registered(_drain_jobs):
            bpy.app.timers.register(_drain_jobs, persistent=True)
        log("listening on %s:%s" % (self.host, self.port))
        return True

    def stop(self):
        self.running = False
        sock = self.socket
        self.socket = None
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass
        try:
            if bpy.app.timers.is_registered(_drain_jobs):
                bpy.app.timers.unregister(_drain_jobs)
        except Exception:
            pass
        log("stopped")

    def _accept_loop(self):
        while self.running and self.socket is not None:
            try:
                conn, addr = self.socket.accept()
            except Exception:
                break
            threading.Thread(target=self._client, args=(conn, addr), name="dshm-bridge-client", daemon=True).start()

    def _read_request(self, conn):
        conn.settimeout(2.0)
        buf = b""
        while True:
            try:
                chunk = conn.recv(65536)
            except socket.timeout:
                chunk = b""
            if chunk:
                buf += chunk
                if len(buf) > (4 << 20):
                    return None
                try:
                    return json.loads(buf.decode("utf-8"))
                except Exception:
                    continue
            else:
                if buf:
                    try:
                        return json.loads(buf.decode("utf-8"))
                    except Exception:
                        return None
                return None

    def _client(self, conn, addr):
        try:
            req = self._read_request(conn)
            if not isinstance(req, dict):
                self._send(conn, {"status": "error", "message": "invalid JSON request"})
                return
            if self.token:
                got = str(req.get("token") or "")
                if not hmac.compare_digest(got, self.token):
                    self._send(conn, {"status": "error", "message": "unauthorized"})
                    return
            timeout = float(req.get("timeout") or 120.0)
            ev = threading.Event()
            box = {}

            def job():
                try:
                    box["value"] = _dispatch(req)
                except BaseException:
                    box["error"] = traceback.format_exc()
                finally:
                    ev.set()

            self.jobs.put(job)
            if not ev.wait(max(1.0, min(timeout, 1800.0))):
                self._send(conn, {"status": "error", "message": "main thread busy (timeout)"})
                return
            if "error" in box:
                self._send(conn, {"status": "error", "message": box["error"][-4000:]})
            else:
                self._send(conn, {"status": "success", "result": box.get("value")})
        except Exception:
            try:
                self._send(conn, {"status": "error", "message": traceback.format_exc()[-2000:]})
            except Exception:
                pass
        finally:
            try:
                conn.close()
            except Exception:
                pass

    @staticmethod
    def _send(conn, payload):
        data = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
        conn.sendall(data)


SERVER = None


def _drain_jobs():
    server = SERVER
    if server is None:
        return None
    for _ in range(8):
        try:
            job = server.jobs.get_nowait()
        except queue.Empty:
            break
        job()
    return 0.05


def _prefs():
    try:
        return bpy.context.preferences.addons[__name__].preferences
    except Exception:
        return None


def _ensure_server():
    global SERVER
    prefs = _prefs()
    if prefs is None:
        raise RuntimeError("addon preferences unavailable")
    if SERVER is not None and SERVER.running:
        return SERVER
    SERVER = BridgeServer(prefs.host, prefs.port, prefs.token)
    SERVER.start()
    return SERVER


def _stop_server():
    global SERVER
    if SERVER is not None:
        SERVER.stop()
        SERVER = None


def _scene_info(params):
    scene = bpy.context.scene
    view_layer = bpy.context.view_layer
    limit = int(params.get("limit") or 100)
    limit = max(1, min(limit, 500))
    objects = []
    for obj in list(scene.objects)[:limit]:
        objects.append({
            "name": obj.name,
            "type": obj.type,
            "location": [round(v, 4) for v in obj.location],
            "dimensions": [round(v, 4) for v in obj.dimensions],
            "parent": obj.parent.name if obj.parent else None,
            "hide_viewport": bool(obj.hide_viewport),
            "hide_render": bool(obj.hide_render),
            "selected": bool(obj.select_get()),
        })
    return {
        "scene": scene.name,
        "filepath": bpy.data.filepath or None,
        "frame": scene.frame_current,
        "engine": scene.render.engine,
        "unit_scale": scene.unit_settings.scale_length,
        "objects_total": len(scene.objects),
        "objects": objects,
        "collections": [c.name for c in bpy.data.collections],
        "active_object": view_layer.objects.active.name if view_layer.objects.active else None,
    }


def _object_info(params):
    name = params.get("name") or params.get("object_name")
    if not name:
        raise ValueError("get_object_info needs name")
    obj = bpy.data.objects.get(name)
    if obj is None:
        raise ValueError("object not found: %s" % name)
    mesh_stats = None
    if obj.type == "MESH" and obj.data is not None:
        mesh_stats = {
            "vertices": len(obj.data.vertices),
            "edges": len(obj.data.edges),
            "polygons": len(obj.data.polygons),
            "materials": [m.name if m else None for m in obj.data.materials],
        }
    return {
        "name": obj.name,
        "type": obj.type,
        "data": obj.data.name if obj.data else None,
        "location": [round(v, 6) for v in obj.location],
        "rotation_euler": [round(v, 6) for v in obj.rotation_euler],
        "scale": [round(v, 6) for v in obj.scale],
        "dimensions": [round(v, 6) for v in obj.dimensions],
        "parent": obj.parent.name if obj.parent else None,
        "children": [c.name for c in obj.children],
        "hide_viewport": bool(obj.hide_viewport),
        "hide_render": bool(obj.hide_render),
        "selected": bool(obj.select_get()),
        "mesh": mesh_stats,
        "modifiers": [{"name": m.name, "type": m.type} for m in obj.modifiers],
        "vertex_groups": len(obj.vertex_groups),
        "custom": _jsonable({k: obj[k] for k in obj.keys()}),
    }


def _execute_code(params):
    code = str(params.get("code") or "")
    if not code.strip():
        raise ValueError("execute_code needs code")
    namespace = {
        "bpy": bpy,
        "math": math,
        "mathutils": mathutils,
        "Vector": mathutils.Vector,
        "K": KERNEL,
        "__name__": "__dshm__",
    }
    out, err = io.StringIO(), io.StringIO()
    old_out, old_err = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = out, err
    started = time.time()
    try:
        exec(compile(code, "<dshm>", "exec"), namespace, namespace)
        return {
            "executed": True,
            "stdout": out.getvalue()[-8000:],
            "stderr": err.getvalue()[-8000:],
            "ms": int((time.time() - started) * 1000),
        }
    except BaseException:
        return {
            "executed": False,
            "stdout": out.getvalue()[-8000:],
            "stderr": err.getvalue()[-8000:],
            "traceback": traceback.format_exc()[-8000:],
            "ms": int((time.time() - started) * 1000),
        }
    finally:
        sys.stdout, sys.stderr = old_out, old_err


def _viewport_png(params):
    max_size = int(params.get("max_size") or 640)
    max_size = max(160, min(max_size, 1600))
    temp = tempfile.gettempdir()
    raw = os.path.join(temp, "dshm_bridge_raw.png")
    out = os.path.join(temp, "dshm_bridge_shot.png")
    t0 = time.time()
    area = None
    region = None
    for candidate in bpy.context.screen.areas if bpy.context.screen else []:
        if candidate.type == "VIEW_3D":
            area = candidate
            region = next((r for r in candidate.regions if r.type == "WINDOW"), None)
            break

    # Maleoon/EEVEE: "Rendered" viewport shading can be black. Capture with the
    # Workbench "Solid" shading instead, then restore the user's setting.
    shading_old = None
    if area is not None:
        try:
            shading_old = area.spaces.active.shading.type
            area.spaces.active.shading.type = "SOLID"
        except Exception:
            shading_old = None

    image = None
    try:
        with bpy.context.temp_override(area=area, region=region, window=bpy.context.window):
            if area is not None and region is not None:
                bpy.ops.screen.screenshot_area(filepath=raw)
            else:
                bpy.ops.screen.screenshot(filepath=raw)
        image = bpy.data.images.load(raw, check_existing=False)
        width, height = image.size
        scale = min(1.0, float(max_size) / float(max(width, height) or 1))
        new_w = max(1, int(width * scale))
        new_h = max(1, int(height * scale))
        if (new_w, new_h) != (width, height):
            image.scale(new_w, new_h)
        image.filepath_raw = out
        image.file_format = "PNG"
        image.save()
        with open(out, "rb") as handle:
            data = handle.read()
        return {
            "ok": True,
            "width": new_w,
            "height": new_h,
            "bytes": len(data),
            "ms": int((time.time() - t0) * 1000),
            "png_base64": base64.b64encode(data).decode("ascii"),
        }
    finally:
        if image is not None:
            try:
                bpy.data.images.remove(image)
            except Exception:
                pass
        if area is not None and shading_old is not None:
            try:
                area.spaces.active.shading.type = shading_old
            except Exception:
                pass


def _scope_makedirs(path):
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    except OSError as exc:
        raise ValueError(
            "cannot create directory for %s: %s. Blender on HarmonyOS can only write its own sandbox "
            "(bpy.app.tempdir) and its public home (~). Pass a path under one of those." % (path, exc)
        )


def _export_scene(params):
    fmt = str(params.get("format") or "obj").lower()
    raw = str(params.get("filepath") or "").strip()
    ext = {"obj": "obj", "stl": "stl", "ply": "ply", "fbx": "fbx", "glb": "glb", "gltf": "gltf"}.get(fmt, "dat")
    if raw:
        path = os.path.abspath(os.path.expanduser(raw))
    else:
        path = os.path.abspath(os.path.join(bpy.app.tempdir or tempfile.gettempdir(), "dshm_export." + ext))
    _scope_makedirs(path)
    selected = bool(params.get("selected_only"))
    apply_modifiers = bool(params.get("apply_modifiers", True))
    t0 = time.time()
    if fmt in ("glb", "gltf"):
        try:
            bpy.ops.export_scene.gltf(
                filepath=path,
                export_format="GLB" if fmt == "glb" else "GLTF_SEPARATE",
                use_selection=selected,
                export_apply=apply_modifiers,
            )
        except ModuleNotFoundError as exc:
            if "numpy" in str(exc):
                raise ValueError("this Blender build has no numpy, so glTF/GLB export is unavailable; use obj/stl/ply instead")
            raise
    elif fmt == "obj":
        try:
            bpy.ops.wm.obj_export(filepath=path, export_selected_objects=selected, apply_modifiers=apply_modifiers)
        except Exception:
            bpy.ops.export_scene.obj(filepath=path, use_selection=selected, use_mesh_modifiers=apply_modifiers)
    elif fmt == "stl":
        try:
            bpy.ops.wm.stl_export(filepath=path, export_selected_objects=selected)
        except Exception:
            bpy.ops.export_mesh.stl(filepath=path, use_selection=selected)
    elif fmt == "fbx":
        bpy.ops.export_scene.fbx(filepath=path, use_selection=selected, apply_scale_options="FBX_SCALE_ALL")
    elif fmt == "ply":
        bpy.ops.wm.ply_export(filepath=path, export_selected_objects=selected)
    else:
        raise ValueError("unsupported format: %s (use obj/stl/ply/glb/gltf/fbx)" % fmt)
    size = os.path.getsize(path) if os.path.exists(path) else 0
    result = {"format": fmt, "path": path, "bytes": size, "ms": int((time.time() - t0) * 1000)}
    if params.get("inline") and size and size <= (8 << 20):
        with open(path, "rb") as handle:
            result["data_base64"] = base64.b64encode(handle.read()).decode("ascii")
    return result


def _save_mainfile(params):
    path = str(params.get("filepath") or "")
    if path:
        path = os.path.abspath(os.path.expanduser(path))
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=path)
    else:
        bpy.ops.wm.save_mainfile()
    return {"path": bpy.data.filepath, "bytes": os.path.getsize(bpy.data.filepath) if bpy.data.filepath else 0}


def _loop_run(params):
    setup = str(params.get("setup") or "")
    step = str(params.get("step") or "")
    measure = str(params.get("measure") or "")
    iterations = max(1, min(int(params.get("iterations") or 100), 200000))
    budget_ms = max(100, min(int(params.get("budget_ms") or 20000), 3600000))
    measure_every = max(1, min(int(params.get("measure_every") or 1), 10000))
    minimize = bool(params.get("minimize", True))
    top_k = max(1, min(int(params.get("top_k") or 10), 100))
    ns = {
        "bpy": bpy,
        "K": KERNEL,
        "math": math,
        "random": __import__("random"),
        "i": 0,
        "params": {},
        "metrics": {},
        "violations": [],
        "score": None,
    }
    t0 = time.time()
    board = []
    best = None
    ran = 0
    if setup:
        exec(compile(setup, "<loop-setup>", "exec"), ns, ns)
    for i in range(iterations):
        if (time.time() - t0) * 1000 >= budget_ms:
            break
        ns["i"] = i
        try:
            exec(compile(step, "<loop-step>", "exec"), ns, ns)
            if measure and i % measure_every == 0:
                exec(compile(measure, "<loop-measure>", "exec"), ns, ns)
                score = ns.get("score")
                if isinstance(score, (int, float)) and math.isfinite(float(score)):
                    entry = {
                        "i": i,
                        "score": float(score),
                        "params": _jsonable(ns.get("params") or {}),
                        "metrics": _jsonable(ns.get("metrics") or {}),
                    }
                    board.append(entry)
                    board = sorted(board, key=lambda x: x["score"], reverse=not minimize)[:top_k]
                    if best is None:
                        best = entry
                    elif (score < best["score"]) if minimize else (score > best["score"]):
                        best = entry
            ran += 1
        except BaseException:
            return {
                "ok": False,
                "ran": ran,
                "i": i,
                "traceback": traceback.format_exc()[-6000:],
                "board": board,
                "best": best,
                "ms": int((time.time() - t0) * 1000),
            }
    return {
        "ok": True,
        "ran": ran,
        "iterations": iterations,
        "best": best,
        "board": board,
        "top_k": top_k,
        "minimize": minimize,
        "budget_ms": budget_ms,
        "ms": int((time.time() - t0) * 1000),
    }


PICK_STATE = {"active": False, "started": 0.0}


def _pick_start(_params):
    try:
        bpy.ops.object.select_all(action="DESELECT")
    except Exception:
        pass
    PICK_STATE["active"] = True
    PICK_STATE["started"] = time.time()
    return {"started": True, "hint": "click an object in the Blender viewport"}


def _pick_poll(_params):
    if not PICK_STATE["active"]:
        raise ValueError("no active pick request")
    names = [o.name for o in bpy.context.selected_objects]
    if names:
        PICK_STATE["active"] = False
        return {
            "picked": names[0],
            "names": names,
            "elapsed_ms": int((time.time() - PICK_STATE["started"]) * 1000),
        }
    return {
        "pending": True,
        "elapsed_ms": int((time.time() - PICK_STATE["started"]) * 1000),
    }


def _pick_cancel(_params):
    PICK_STATE["active"] = False
    return {"cancelled": True}


JOBS = {}
JOB_SEQ = [0]


def _job_start(params):
    code = str(params.get("code") or "")
    if not code.strip():
        raise ValueError("job_start needs code")
    JOB_SEQ[0] += 1
    job_id = "job-%d-%d" % (os.getpid(), JOB_SEQ[0])
    JOBS[job_id] = {
        "id": job_id,
        "name": str(params.get("name") or job_id),
        "code": code,
        "status": "queued",
        "created": time.time(),
        "started": None,
        "finished": None,
        "stdout": "",
        "stderr": "",
        "traceback": "",
    }
    if not bpy.app.timers.is_registered(_job_tick):
        bpy.app.timers.register(_job_tick, first_interval=0.1)
    return {"id": job_id, "status": "queued", "name": JOBS[job_id]["name"]}


def _job_tick():
    job = None
    for candidate in JOBS.values():
        if candidate["status"] == "queued":
            job = candidate
            break
    if job is None:
        return None
    job["status"] = "running"
    job["started"] = time.time()
    try:
        result = _execute_code({"code": job["code"]})
        job["stdout"] = result.get("stdout") or ""
        job["stderr"] = result.get("stderr") or ""
        job["traceback"] = result.get("traceback") or ""
        job["status"] = "done" if result.get("executed") else "error"
    except BaseException:
        job["traceback"] = traceback.format_exc()[-8000:]
        job["status"] = "error"
    job["finished"] = time.time()
    job["ms"] = int((job["finished"] - job["started"]) * 1000)
    return 0.05


def _job_status(params):
    job_id = str(params.get("id") or "")
    job = JOBS.get(job_id)
    if job is None:
        raise ValueError("job not found: " + job_id)
    return {
        "id": job["id"],
        "name": job["name"],
        "status": job["status"],
        "ms": job.get("ms"),
        "stdout": job["stdout"][-8000:],
        "stderr": job["stderr"][-8000:],
        "traceback": job["traceback"][-8000:] if job["traceback"] else "",
    }


def _job_list(_params):
    return {
        "jobs": [
            {"id": j["id"], "name": j["name"], "status": j["status"], "ms": j.get("ms")}
            for j in JOBS.values()
        ]
    }


def _home_candidates():
    """Candidate home dirs, from explicit HAP env to Blender defaults."""
    result = []
    for value in (os.environ.get("DSHM_PUBLIC_HOME"), os.path.expanduser("~"),
                  getattr(bpy.app, "tempdir", None), tempfile.gettempdir()):
        if not value:
            continue
        try:
            value = os.path.abspath(value)
        except Exception:
            continue
        if value not in result:
            result.append(value)
    return result


def _public_home():
    for path in _home_candidates():
        if os.path.isdir(path):
            return path
    return tempfile.gettempdir()


def _data_home():
    candidates = [os.path.join(p, ".dshm-blender") for p in _home_candidates()]
    for path in candidates:
        if os.path.isdir(path):
            return path
    home = candidates[0] if candidates else os.path.join(tempfile.gettempdir(), ".dshm-blender")
    os.makedirs(home, exist_ok=True)
    return home


def _vec(value, default):
    if isinstance(value, (list, tuple)) and len(value) >= 3:
        return mathutils.Vector((float(value[0]), float(value[1]), float(value[2])))
    if isinstance(value, str):
        parts = value.replace(",", " ").split()
        if len(parts) >= 3:
            return mathutils.Vector((float(parts[0]), float(parts[1]), float(parts[2])))
    return mathutils.Vector(default)


def _save_png(path, max_size):
    image = bpy.data.images.load(path, check_existing=False)
    try:
        width, height = image.size
        scale = min(1.0, float(max_size) / float(max(width, height) or 1))
        new_w = max(1, int(width * scale))
        new_h = max(1, int(height * scale))
        if (new_w, new_h) != (width, height):
            image.scale(new_w, new_h)
        out = os.path.join(tempfile.gettempdir(), "dshm_render_view.png")
        image.filepath_raw = out
        image.file_format = "PNG"
        image.save()
    finally:
        bpy.data.images.remove(image)
    with open(out, "rb") as handle:
        data = handle.read()
    return new_w, new_h, data


def _render_view(params):
    max_size = max(160, min(int(params.get("max_size") or 960), 1600))
    from_pos = _vec(params.get("from"), (7.0, -7.0, 5.0))
    look_at = _vec(params.get("look_at"), (0.0, 0.0, 0.0))
    lens = float(params.get("lens") or 50.0)
    scene = bpy.context.scene
    cam_data = bpy.data.cameras.new("DSHM_ViewCam")
    cam = bpy.data.objects.new("DSHM_ViewCam", cam_data)
    scene.collection.objects.link(cam)
    old_camera = scene.camera
    old_x, old_y, old_pct = scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage
    old_path = scene.render.filepath
    old_fmt = scene.render.image_settings.file_format
    old_engine = scene.render.engine
    old_film = scene.render.film_transparent
    old_w, old_h = int(params.get("width") or 960), int(params.get("height") or 540)
    t0 = time.time()
    try:
        cam.location = from_pos
        direction = look_at - from_pos
        if direction.length < 1e-6:
            direction = mathutils.Vector((0.0, 0.0, -1.0))
        cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
        cam_data.lens = lens
        scene.camera = cam
        scene.render.resolution_x = old_w
        scene.render.resolution_y = old_h
        scene.render.resolution_percentage = 100
        scene.render.image_settings.file_format = "PNG"
        requested_engine = str(params.get("engine") or "workbench").lower()
        if requested_engine in ("eevee", "eevee_next", "blender_eevee_next"):
            target_engine = "BLENDER_EEVEE_NEXT"
        elif requested_engine in ("cycles", "blender_cycles"):
            target_engine = "CYCLES"
        else:
            target_engine = "BLENDER_WORKBENCH"
        try:
            scene.render.engine = target_engine
        except Exception:
            scene.render.engine = "BLENDER_WORKBENCH"
        scene.render.film_transparent = False
        raw = os.path.join(tempfile.gettempdir(), "dshm_render_view_raw.png")
        scene.render.filepath = raw
        bpy.ops.render.render(write_still=True)
        width, height, data = _save_png(raw, max_size)
        return {
            "ok": True,
            "from": list(from_pos),
            "look_at": list(look_at),
            "lens": lens,
            "width": width,
            "height": height,
            "bytes": len(data),
            "engine": scene.render.engine,
            "ms": int((time.time() - t0) * 1000),
            "png_base64": base64.b64encode(data).decode("ascii"),
        }
    finally:
        scene.camera = old_camera
        scene.render.resolution_x = old_x
        scene.render.resolution_y = old_y
        scene.render.resolution_percentage = old_pct
        scene.render.filepath = old_path
        scene.render.image_settings.file_format = old_fmt
        scene.render.engine = old_engine
        scene.render.film_transparent = old_film
        try:
            bpy.data.objects.remove(cam, do_unlink=True)
        except Exception:
            pass
        try:
            bpy.data.cameras.remove(cam_data)
        except Exception:
            pass


TXN_MARKS = {}


def _txn_snapshot(params):
    txn_id = str(params.get("id") or ("txn-" + str(int(time.time() * 1000))))
    folder = os.path.join(_data_home(), "txn")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, txn_id + ".blend")
    t0 = time.time()
    bpy.ops.wm.save_as_mainfile(filepath=path, copy=True)
    meta = {
        "id": txn_id,
        "path": path,
        "bytes": os.path.getsize(path) if os.path.exists(path) else 0,
        "scene": bpy.context.scene.name,
        "objects": len(bpy.context.scene.objects),
        "created": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    with open(os.path.join(folder, txn_id + ".json"), "w", encoding="utf-8") as handle:
        json.dump(meta, handle, ensure_ascii=False, indent=2)
    meta["ms"] = int((time.time() - t0) * 1000)
    return meta


def _txn_restore(params):
    txn_id = str(params.get("id") or "")
    if not txn_id:
        raise ValueError("txn_restore needs id")
    path = os.path.join(_data_home(), "txn", txn_id + ".blend")
    if not os.path.isfile(path):
        raise ValueError("snapshot not found: " + txn_id)
    bpy.ops.wm.open_mainfile(filepath=path)
    return {"restored": True, "id": txn_id, "path": path, "objects": len(bpy.context.scene.objects)}


def _txn_mark(params):
    mark_id = str(params.get("id") or ("mark-" + str(int(time.time() * 1000))))
    marks = {}
    for obj in bpy.context.scene.objects:
        marks[obj.name] = {
            "location": list(obj.location),
            "rotation_euler": list(obj.rotation_euler),
            "rotation_mode": obj.rotation_mode,
            "scale": list(obj.scale),
            "hide_viewport": bool(obj.hide_viewport),
            "hide_render": bool(obj.hide_render),
            "select": bool(obj.select_get()),
        }
    TXN_MARKS[mark_id] = marks
    return {"marked": True, "id": mark_id, "objects": len(marks)}


def _txn_revert(params):
    mark_id = str(params.get("id") or "")
    if mark_id not in TXN_MARKS:
        raise ValueError("mark not found: " + mark_id)
    marks = TXN_MARKS[mark_id]
    restored = 0
    missing = []
    for name, state in marks.items():
        obj = bpy.data.objects.get(name)
        if obj is None:
            missing.append(name)
            continue
        obj.rotation_mode = state["rotation_mode"]
        obj.location = state["location"]
        obj.rotation_euler = state["rotation_euler"]
        obj.scale = state["scale"]
        obj.hide_viewport = state["hide_viewport"]
        obj.hide_render = state["hide_render"]
        try:
            obj.select_set(state["select"])
        except Exception:
            pass
        restored += 1
    return {"reverted": True, "id": mark_id, "restored": restored, "missing": missing}


def _txn_list(_params):
    folder = os.path.join(_data_home(), "txn")
    os.makedirs(folder, exist_ok=True)
    snapshots = []
    for name in sorted(os.listdir(folder)):
        if name.endswith(".json"):
            try:
                with open(os.path.join(folder, name), "r", encoding="utf-8") as handle:
                    snapshots.append(json.load(handle))
            except Exception:
                pass
    return {"snapshots": snapshots, "marks": sorted(TXN_MARKS)}


def _preset_path(name):
    safe = "".join(c for c in str(name) if c.isalnum() or c in "._-")
    if not safe:
        raise ValueError("invalid preset name")
    folder = os.path.join(_data_home(), "presets")
    os.makedirs(folder, exist_ok=True)
    return os.path.join(folder, safe + ".json")


def _preset_save(params):
    name = str(params.get("name") or "").strip()
    values = params.get("values") or {}
    if not name or not isinstance(values, dict):
        raise ValueError("preset_save needs name and values object")
    path = _preset_path(name)
    payload = {"name": name, "values": values, "saved": time.strftime("%Y-%m-%dT%H:%M:%S")}
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
    return {"saved": True, "name": name, "path": path, "keys": sorted(values)}


def _preset_apply(params):
    name = str(params.get("name") or "").strip()
    path = _preset_path(name)
    if not os.path.isfile(path):
        raise ValueError("preset not found: " + name)
    with open(path, "r", encoding="utf-8") as handle:
        payload = json.load(handle)
    scene = bpy.context.scene
    applied = []
    failed = []
    for key, value in (payload.get("values") or {}).items():
        target = scene
        parts = str(key).split(".")
        try:
            for part in parts[:-1]:
                target = getattr(target, part)
            setattr(target, parts[-1], value)
            applied.append(key)
        except Exception as exc:
            failed.append({"key": key, "error": str(exc)})
    return {"applied": applied, "failed": failed, "preset": name}


def _preset_list(_params):
    folder = os.path.join(_data_home(), "presets")
    os.makedirs(folder, exist_ok=True)
    presets = []
    for name in sorted(os.listdir(folder)):
        if name.endswith(".json"):
            try:
                with open(os.path.join(folder, name), "r", encoding="utf-8") as handle:
                    preset = json.load(handle)
                presets.append({"name": preset.get("name"), "keys": sorted((preset.get("values") or {}).keys())})
            except Exception:
                pass
    return {"presets": presets}


def _preset_delete(params):
    name = str(params.get("name") or "").strip()
    path = _preset_path(name)
    if os.path.isfile(path):
        os.remove(path)
        return {"deleted": True, "name": name}
    return {"deleted": False, "name": name, "reason": "not found"}


def _object_names(params, fallback_selected=True):
    names = params.get("names")
    if isinstance(names, str):
        names = [names]
    if isinstance(names, list) and names:
        return [str(n) for n in names]
    if fallback_selected:
        selected = [o.name for o in bpy.context.selected_objects if o.type == "MESH"]
        if selected:
            return selected
    raise ValueError("no object names given and nothing selected")


def _opt_decimate(params):
    names = _object_names(params)
    ratio = max(0.01, min(float(params.get("ratio") or 0.5), 0.99))
    apply_modifier = bool(params.get("apply", True))
    results = []
    for name in names:
        obj = bpy.data.objects.get(name)
        if obj is None or obj.type != "MESH":
            results.append({"name": name, "error": "mesh not found"})
            continue
        before = len(obj.data.vertices)
        modifier = obj.modifiers.new("DSHM_Decimate", "DECIMATE")
        modifier.ratio = ratio
        modifier.decimate_type = "COLLAPSE"
        if apply_modifier:
            bpy.context.view_layer.objects.active = obj
            bpy.ops.object.select_all(action="DESELECT")
            obj.select_set(True)
            bpy.ops.object.modifier_apply(modifier=modifier.name)
        after = len(obj.data.vertices)
        results.append({"name": name, "before": before, "after": after, "ratio": ratio, "applied": apply_modifier})
    return {"results": results}


def _opt_join(params):
    names = _object_names(params)
    objects = [bpy.data.objects.get(n) for n in names]
    objects = [o for o in objects if o is not None and o.type == "MESH"]
    if len(objects) < 2:
        raise ValueError("join needs at least two mesh objects")
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    active = objects[0]
    bpy.context.view_layer.objects.active = active
    before = len(bpy.context.scene.objects)
    bpy.ops.object.join()
    return {"joined": True, "into": active.name, "objects_before": before, "objects_after": len(bpy.context.scene.objects)}


def _perf_stats(_params):
    scene = bpy.context.scene
    by_type = {}
    verts = 0
    polys = 0
    for obj in scene.objects:
        by_type[obj.type] = by_type.get(obj.type, 0) + 1
        if obj.type == "MESH" and obj.data is not None:
            verts += len(obj.data.vertices)
            polys += len(obj.data.polygons)
    return {
        "scene": scene.name,
        "objects": len(scene.objects),
        "by_type": by_type,
        "vertices": verts,
        "polygons": polys,
        "materials": len(bpy.data.materials),
        "images": len(bpy.data.images),
        "lights": len(bpy.data.lights),
        "engine": scene.render.engine,
        "resolution": [scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage],
        "samples": getattr(getattr(scene, "eevee", None), "taa_render_samples", None),
    }


def _perf_bench(params):
    scene = bpy.context.scene
    old_path = scene.render.filepath
    old_fmt = scene.render.image_settings.file_format
    raw = os.path.join(tempfile.gettempdir(), "dshm_perf_bench.png")
    t0 = time.time()
    try:
        scene.render.filepath = raw
        scene.render.image_settings.file_format = "PNG"
        bpy.ops.render.render(write_still=True)
        size = os.path.getsize(raw) if os.path.exists(raw) else 0
        return {"rendered": True, "ms": int((time.time() - t0) * 1000), "bytes": size, "path": raw}
    finally:
        scene.render.filepath = old_path
        scene.render.image_settings.file_format = old_fmt




# ── Headless worker (HarmonyOS native child process) ─────────────────────
# The worker is a separate process started through the official
# OH_Ability_StartNativeChildProcess API. It loads libblender.so in
# --background mode and exposes a Unix-socket JSON protocol. The addon runs
# in the GUI process and only proxies requests; no child_process is used.
_WORKER_SOCK = "/data/storage/el2/base/haps/entry/files/blender_worker.sock"
_WORKER_READY = _WORKER_SOCK + ".ready"
_WORKER = {"pid": 0}


def _worker_connect(timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(float(timeout))
    s.connect(_WORKER_SOCK)
    return s


def _worker_ping():
    try:
        s = _worker_connect(1.0)
        s.close()
        return True
    except Exception:
        return False


def _worker_start(_params):
    if _worker_ping():
        pid = 0
        try:
            with open(_WORKER_READY, "r") as fh:
                pid = int(fh.read().strip() or "0")
        except Exception:
            pass
        _WORKER["pid"] = pid
        return {"ok": True, "already": True, "pid": pid, "socket": _WORKER_SOCK}

    for stale in (_WORKER_READY, _WORKER_SOCK):
        try:
            os.unlink(stale)
        except OSError:
            pass

    class _FdList(ctypes.Structure):
        _fields_ = [("head", ctypes.c_void_p)]

    class _Args(ctypes.Structure):
        _fields_ = [("entryParams", ctypes.c_char_p), ("fdList", _FdList)]

    class _Options(ctypes.Structure):
        _fields_ = [("isolationMode", ctypes.c_int), ("reserved", ctypes.c_int64)]

    try:
        lib = ctypes.CDLL("libchild_process.so")
        fn = lib.OH_Ability_StartNativeChildProcess
        fn.argtypes = [ctypes.c_char_p, _Args, _Options, ctypes.POINTER(ctypes.c_int32)]
        fn.restype = ctypes.c_int
        pid = ctypes.c_int32(0)
        rc = fn(b"libblender_worker.so:Main", _Args(b"", _FdList(None)), _Options(0, 0), ctypes.byref(pid))
    except Exception as exc:
        return {"ok": False, "error": "start failed: %s" % exc}
    if rc != 0:
        return {"ok": False, "error": "child process rc=%d" % rc}

    deadline = time.time() + 90.0
    while time.time() < deadline:
        if os.path.exists(_WORKER_READY):
            try:
                with open(_WORKER_READY, "r") as fh:
                    ready_pid = int(fh.read().strip() or "0")
            except Exception:
                ready_pid = 0
            if _worker_ping():
                _WORKER["pid"] = ready_pid or pid.value
                return {"ok": True, "pid": _WORKER["pid"], "socket": _WORKER_SOCK, "rc": rc}
        time.sleep(0.5)
    return {"ok": False, "pid": pid.value, "error": "worker not ready in 90s"}


def _worker_run(params):
    code = str(params.get("code") or "")
    if not code:
        raise ValueError("code is required")
    timeout = float(params.get("timeout") or 120.0)
    if not _worker_ping():
        start = _worker_start({})
        if not start.get("ok"):
            return start
    t0 = time.time()
    s = _worker_connect(timeout)
    try:
        s.sendall((json.dumps({"code": code}) + "\n").encode("utf-8"))
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    finally:
        s.close()
    if not buf:
        return {"ok": False, "error": "empty response from worker"}
    result = json.loads(buf.decode("utf-8", "replace"))
    result["ms"] = int((time.time() - t0) * 1000)
    return result


def _worker_stop(_params):
    if not _worker_ping():
        return {"ok": True, "already": True}
    try:
        s = _worker_connect(3.0)
        s.sendall((json.dumps({"quit": 1}) + "\n").encode("utf-8"))
        s.close()
    except Exception as exc:
        return {"ok": False, "error": str(exc)}
    _WORKER["pid"] = 0
    return {"ok": True, "stopped": True}


COMMANDS = {
    "ping": "health check (version / pid / background)",
    "get_addon_info": "addon name, protocol and capabilities",
    "get_scene_info": "scene, objects (limited), collections, active object",
    "get_object_info": "one object: transform, mesh stats, modifiers, custom props",
    "execute_code": "run Python on Blender main thread with persistent K",
    "get_viewport_png": "capture the 3D viewport as base64 PNG",
    "export_scene": "export glb/gltf/obj/stl/fbx/ply",
    "save_mainfile": "save the current .blend",
    "render_view": "render a custom camera view to PNG",
    "txn_snapshot": "save a .blend snapshot (file-level rollback)",
    "txn_restore": "restore a .blend snapshot",
    "txn_mark": "mark object transforms in memory",
    "txn_revert": "revert object transforms to a mark",
    "txn_list": "list snapshots and marks",
    "preset_save": "save scene/render preset JSON",
    "preset_apply": "apply a preset by dotted scene paths",
    "preset_list": "list presets",
    "preset_delete": "delete a preset",
    "opt_decimate": "decimate mesh objects safely",
    "opt_join": "join mesh objects",
    "perf_stats": "scene/render statistics",
    "perf_bench": "render one frame and time it",
    "loop_run": "inner search loop: setup/step/measure, returns best board",
    "pick_start": "ask the user to click an object",
    "pick_poll": "poll the current pick selection",
    "pick_cancel": "cancel an active pick request",
    "job_start": "queue a Python job; runs on the main thread via a timer",
    "job_status": "poll a job",
    "job_list": "list jobs",
    "worker_start": "start headless native child worker",
    "worker_run": "run Python in headless worker (code, timeout)",
    "worker_stop": "stop headless worker",
    "list_commands": "this catalog",
}


def _dispatch(req):
    cmd = str(req.get("type") or req.get("cmd") or "")
    params = req.get("params") or {}
    if cmd == "ping":
        return {
            "pong": True,
            "addon": "dshm-blender-bridge",
            "version": "0.1.0",
            "blender": bpy.app.version_string,
            "python": sys.version.split()[0],
            "pid": os.getpid(),
            "background": bool(bpy.app.background),
            "home": _public_home(),
        }
    if cmd == "get_addon_info":
        return {"name": "DSHM Blender Bridge", "version": "0.1.0", "protocol": "dshm-json-lines",
                "commands": sorted(COMMANDS), "home": _public_home()}
    if cmd == "get_scene_info":
        return _scene_info(params)
    if cmd == "get_object_info":
        return _object_info(params)
    if cmd == "execute_code":
        return _execute_code(params)
    if cmd in ("get_viewport_png", "get_viewport_screenshot", "screenshot"):
        return _viewport_png(params)
    if cmd == "export_scene":
        return _export_scene(params)
    if cmd == "save_mainfile":
        return _save_mainfile(params)
    if cmd == "render_view":
        return _render_view(params)
    if cmd == "txn_snapshot":
        return _txn_snapshot(params)
    if cmd == "txn_restore":
        return _txn_restore(params)
    if cmd == "txn_mark":
        return _txn_mark(params)
    if cmd == "txn_revert":
        return _txn_revert(params)
    if cmd == "txn_list":
        return _txn_list(params)
    if cmd == "preset_save":
        return _preset_save(params)
    if cmd == "preset_apply":
        return _preset_apply(params)
    if cmd == "preset_list":
        return _preset_list(params)
    if cmd == "preset_delete":
        return _preset_delete(params)
    if cmd == "opt_decimate":
        return _opt_decimate(params)
    if cmd == "opt_join":
        return _opt_join(params)
    if cmd == "perf_stats":
        return _perf_stats(params)
    if cmd == "perf_bench":
        return _perf_bench(params)
    if cmd == "loop_run":
        return _loop_run(params)
    if cmd == "pick_start":
        return _pick_start(params)
    if cmd == "pick_poll":
        return _pick_poll(params)
    if cmd == "pick_cancel":
        return _pick_cancel(params)
    if cmd == "job_start":
        return _job_start(params)
    if cmd == "job_status":
        return _job_status(params)
    if cmd == "worker_start":
        return _worker_start(params)
    if cmd == "worker_run":
        return _worker_run(params)
    if cmd == "worker_stop":
        return _worker_stop(params)
    if cmd == "job_list":
        return _job_list(params)
    if cmd == "list_commands":
        return {"commands": [{"name": k, "description": v} for k, v in sorted(COMMANDS.items())]}
    raise ValueError("unknown command: %s" % cmd)


class DSHM_OT_start(bpy.types.Operator):
    bl_idname = "dshm.start_bridge"
    bl_label = "Start DSHM Bridge"
    bl_description = "Start the local TCP bridge for DSHM"

    def execute(self, context):
        try:
            _ensure_server()
            self.report({"INFO"}, "DSHM bridge listening on %s:%s" % (SERVER.host, SERVER.port))
        except Exception as exc:
            self.report({"ERROR"}, "DSHM bridge failed: %s" % exc)
            return {"CANCELLED"}
        return {"FINISHED"}


class DSHM_OT_stop(bpy.types.Operator):
    bl_idname = "dshm.stop_bridge"
    bl_label = "Stop DSHM Bridge"
    bl_description = "Stop the local TCP bridge"

    def execute(self, context):
        _stop_server()
        self.report({"INFO"}, "DSHM bridge stopped")
        return {"FINISHED"}


class DSHM_OT_show_info(bpy.types.Operator):
    bl_idname = "dshm.show_info"
    bl_label = "Connection Info"
    bl_description = "Show DSHM bridge host, port and status"

    def invoke(self, context, event):
        prefs = _prefs()
        running = SERVER is not None and SERVER.running
        host = prefs.host if prefs is not None else "?"
        port = prefs.port if prefs is not None else "?"
        def draw(menu, _context):
            layout = menu.layout
            layout.label(text="Host: " + str(host))
            layout.label(text="Port: " + str(port))
            layout.label(text="Status: listening" if running else "Status: stopped")

        context.window_manager.popup_menu(draw, title="DSHM Blender Bridge", icon="INFO")
        return {"FINISHED"}


class DSHM_PT_panel(bpy.types.Panel):
    bl_label = "DSHM Bridge"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "DSHM"

    def draw(self, context):
        layout = self.layout
        prefs = _prefs()
        if prefs is not None:
            layout.prop(prefs, "host")
            layout.prop(prefs, "port")
            layout.prop(prefs, "token")
            layout.prop(prefs, "auto_start")
        running = SERVER is not None and SERVER.running
        layout.label(text="Status: listening" if running else "Status: stopped")
        row = layout.row(align=True)
        row.operator("dshm.start_bridge", text="Start")
        row.operator("dshm.stop_bridge", text="Stop")


class DSHM_Preferences(bpy.types.AddonPreferences):
    bl_idname = __name__

    host: bpy.props.StringProperty(name="Host", default="127.0.0.1",
                                   description="127.0.0.1 for same-device; 0.0.0.0 for LAN (token required)")
    port: bpy.props.IntProperty(name="Port", default=DEFAULT_PORT, min=1024, max=65535)
    token: bpy.props.StringProperty(name="Token", default="", subtype="PASSWORD",
                                    description="Optional shared secret; required for non-loopback host")
    auto_start: bpy.props.BoolProperty(name="Auto-Start", default=True)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "host")
        layout.prop(self, "port")
        layout.prop(self, "token")
        layout.prop(self, "auto_start")


def _auto_start():
    prefs = _prefs()
    if prefs is not None and prefs.auto_start and not bpy.app.background:
        try:
            _ensure_server()
        except Exception as exc:
            log("auto-start failed:", exc)
    return None


CLASSES = (DSHM_OT_start, DSHM_OT_stop, DSHM_OT_show_info, DSHM_PT_panel, DSHM_Preferences)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    try:
        bpy.app.translations.register(__name__, TRANSLATIONS)
    except Exception as exc:
        log("translation register failed:", exc)
    bpy.app.timers.register(_auto_start, first_interval=1.0)
    log("registered")


def unregister():
    try:
        bpy.app.translations.unregister(__name__)
    except Exception:
        pass
    try:
        bpy.app.timers.unregister(_auto_start)
    except Exception:
        pass
    _stop_server()
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    log("unregistered")


if __name__ == "__main__":
    register()
