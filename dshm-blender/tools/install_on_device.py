# Install the DSHM Blender plugin into the running DSHM core tree.
# Run inside DSHM (python tool) with the local HTTP server on 127.0.0.1:8002.
import os, json, sys, shutil, urllib.request, traceback

BASE = "http://127.0.0.1:8002"
FILES = [
    "package.json",
    "cordis.patch.yml",
    "lib/index.js",
    "lib/protocol.js",
    "lib/plan-catalog.js",
    "addon/dshm_bridge.py",
    "README.md",
]

def fetch(rel):
    return urllib.request.urlopen(BASE + "/" + rel, timeout=15).read()

def main():
    home = os.environ.get("DSH_HOME") or ""
    if not home:
        # DSHM's sandbox home is <files>/dsh/home; try the documented location.
        base = os.environ.get("HOME") or ""
        home = os.path.join(base, "dsh", "home") if base else ""
    if not home or not os.path.isdir(home):
        raise RuntimeError("DSH_HOME not found; pass it in the prompt or run from the host process")

    # DSHM 1.1.3 layout: profiles/patch live in <files>/dsh/home, but the core
    # tree lives in <files>/dsh/cores (a sibling of home). Accept both.
    candidate_cores = [
        os.environ.get("DSHM_CORES") or "",
        os.path.join(home, "cores"),
        os.path.join(os.path.dirname(home), "cores"),
    ]
    cores = ""
    for candidate in candidate_cores:
        if candidate and os.path.isdir(candidate):
            cores = candidate
            break
    if not cores:
        raise RuntimeError("cores dir not found; tried: " + ", ".join([c for c in candidate_cores if c]))
    print("HOME=" + home)
    print("CORES=" + cores)
    current = ""
    try:
        state = json.load(open(os.path.join(cores, "state.json"), "r", encoding="utf-8"))
        current = str(state.get("current") or "")
    except Exception:
        current = ""
    if not current or not os.path.isdir(os.path.join(cores, current)):
        versions = [d for d in os.listdir(cores) if os.path.isdir(os.path.join(cores, d)) and not d.endswith(".staging")]
        versions.sort()
        if not versions:
            raise RuntimeError("no core tree under " + cores)
        current = versions[-1]
    core = os.path.join(cores, current)

    # DSHM's Loader builds its bare-package table from bundle dependency
    # closures. A plain insert row is not enough: add this package to
    # @deepseek-ai/dsh-web-app's dependencies (with a one-time backup).
    webapp = os.path.join(core, "node_modules", "@deepseek-ai", "dsh-web-app", "package.json")
    if os.path.isfile(webapp):
        with open(webapp, "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
        deps = manifest.setdefault("dependencies", {})
        if deps.get("@dsh-external/dshm-blender") != "0.1.0":
            backup = webapp + ".bak-dshm-blender"
            if not os.path.isfile(backup):
                shutil.copy2(webapp, backup)
            deps["@dsh-external/dshm-blender"] = "0.1.0"
            with open(webapp, "w", encoding="utf-8") as handle:
                json.dump(manifest, handle, ensure_ascii=False, indent=2)
                handle.write("\n")
            print("WEBAPP_DEP=added")
        else:
            print("WEBAPP_DEP=already")

    targets = [os.path.join(core, "node_modules", "@dsh-external", "dshm-blender")]
    profiles = os.path.join(home, "profiles")
    if os.path.isdir(profiles):
        for name in os.listdir(profiles):
            pdir = os.path.join(profiles, name)
            if os.path.isfile(os.path.join(pdir, "package.json")):
                targets.append(os.path.join(pdir, "node_modules", "@dsh-external", "dshm-blender"))

    payload = {}
    for rel in FILES:
        try:
            payload[rel] = fetch(rel)
        except Exception as exc:
            if rel == "README.md":
                continue
            raise RuntimeError("prefetch " + rel + " failed: " + str(exc))

    written = []
    for dest in targets:
        if os.path.isdir(dest):
            shutil.rmtree(dest, ignore_errors=True)
        for rel in FILES:
            if rel not in payload:
                continue
            path = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(payload[rel])
            written.append(path)

    patch = os.path.join(home, "cordis.patch.yml")
    old = ""
    if os.path.isfile(patch):
        old = open(patch, "r", encoding="utf-8").read()
    if "dshm-blender" not in old:
        if old and not old.endswith("\n"):
            old += "\n"
        old += "\n# DSHM Blender plugin (installed by install_plugin.py)\n- insert:\n    - id: dshm-blender\n      name: '@dsh-external/dshm-blender'\n      config: {}\n"
        with open(patch, "w", encoding="utf-8") as handle:
            handle.write(old)

    print("INSTALL_OK")
    print("core=" + core)
    print("targets=" + str(len(targets)))
    print("patch=" + patch)
    print("files=" + str(len(written)))
    print("size=" + str(sum(os.path.getsize(p) for p in written)))

try:
    main()
except Exception:
    print("INSTALL_FAIL")
    traceback.print_exc()
