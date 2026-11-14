#!/usr/bin/env python3
"""
Recursively expand .so dependencies via DT_NEEDED.

Usage:
    python3 tools/analyze_deps.py [--tree|--flat] [--manifest OUT] [SO ...]

Default entry point:
    dist/build/blender-ohos/lib/libblender.so

Search order for each NEEDED soname (first hit wins):
    1. deps/dist/arm64-v8a/lib/
    2. dist/build/blender-ohos/lib/

Anything not found in those trees is checked against a system-provided
allowlist (libc/libdl/libvulkan/...); the rest is flagged MISSING.

Manifest format (one entry per line, tab-separated):
    <dst_name>\t<abs_src_path>
Consumed by stage_libs.sh.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from collections import deque

ROOT = Path(__file__).resolve().parent.parent
SEARCH_DIRS = [
    ROOT / "deps/dist/arm64-v8a/lib",
    ROOT / "deps/dist/python-arm64-v8a/lib",
    ROOT / "dist/build/blender-ohos/lib",
]
DEFAULT_ENTRIES = [
    ROOT / "dist/build/blender-ohos/lib/libblender.so",
]

# Plugins loaded at runtime via dlopen (invisible to DT_NEEDED analysis) but
# required on-device. Add any future dlopened plugins here.
EXTRA_PLUGINS = [
    # OpenImageDenoise device plugin (oidn dlopens the CPU device).
    ROOT / "deps/dist/arm64-v8a/lib/libOpenImageDenoise_device_cpu.so.2.3.3",
]

# Provided by the OHOS device / NDK runtime; do not stage.
SYSTEM_LIBS = {
    # musl + toolchain
    "libc.so", "libdl.so", "libm.so", "libpthread.so", "librt.so",
    "libgcc_s.so.1", "libunwind.so",
    "ld-musl-aarch64.so.1", "ld-musl-arm.so.1",
    # OHOS NDK
    "libhilog.so", "libhilog_ndk.z.so",
    "libace_napi.z.so", "libace_ndk.z.so",
    "libnative_window.so", "libnative_vsync.so", "libnative_drawing.so",
    "libnative_buffer.so", "libnative_image.so",
    "libohaudio.so", "libohcamera.so",
    # Graphics
    "libEGL.so", "libGLESv3.so", "libGLESv2.so", "libvulkan.so",
    # C++ runtime — NDK auto-injects this into HAP libs/.
    "libc++_shared.so",
}

# ── llvm-readelf discovery ─────────────────────────────────────────────────
def find_readelf() -> str:
    for c in [
        os.environ.get("READELF"),
        "/apps/harmony/sdk/default/openharmony/native/llvm/bin/llvm-readelf",
    ]:
        if c and Path(c).is_file():
            return c
    for name in ("llvm-readelf", "readelf", "aarch64-linux-gnu-readelf"):
        p = shutil.which(name)
        if p:
            return p
    sys.exit("ERROR: no readelf found. Set READELF=/path/to/readelf and retry.")

READELF = find_readelf()

# ── ELF probing ────────────────────────────────────────────────────────────
_NEEDED_RE = re.compile(r"\(NEEDED\).*\[([^\]]+)\]")

def read_needed(so_path: Path) -> list[str]:
    """Return the DT_NEEDED list for so_path (empty on any failure)."""
    r = subprocess.run(
        [READELF, "-d", str(so_path)],
        capture_output=True, text=True, check=False,
    )
    if r.returncode != 0:
        return []
    # Some deps were linked with a relative path in DT_NEEDED
    # (e.g. "src/lib/Iex/libIex-3_2.so"); the device loader needs a plain
    # soname, so resolve by basename here as well.
    return [Path(m.group(1)).name for line in r.stdout.splitlines()
                                if (m := _NEEDED_RE.search(line))]

# ── Lookup ─────────────────────────────────────────────────────────────────
def find_in_search_dirs(soname: str) -> tuple[Path, str] | None:
    """Return (resolved real path, source-tag) or None."""
    base = Path(soname).name
    for d in SEARCH_DIRS:
        candidate = d / base
        if candidate.exists() or candidate.is_symlink():
            return candidate.resolve(), tag_for(d)
    return None

def tag_for(d: Path) -> str:
    s = str(d)
    if "deps/dist" in s:  return "deps"
    if "blender-ohos" in s: return "blender"
    return "other"

# ── Walk ───────────────────────────────────────────────────────────────────
class Node:
    __slots__ = ("soname", "path", "source", "needed", "size")
    def __init__(self, soname, path, source):
        self.soname, self.path, self.source = soname, path, source
        self.needed: list[str] = []
        self.size = path.stat().st_size if (path and path.is_file()) else 0

def walk(entries: list[Path]) -> dict[str, Node]:
    """BFS across DT_NEEDED starting from the given ELF files."""
    seen: dict[str, Node] = {}
    q: deque[str] = deque()

    for e in entries:
        if not e.is_file():
            print(f"WARN: entry not found: {e}", file=sys.stderr)
            continue
        soname = e.name
        n = Node(soname, e.resolve(), "entry")
        n.needed = read_needed(e)
        seen[soname] = n
        q.extend(n.needed)

    while q:
        soname = q.popleft()
        if soname in seen:
            continue
        if soname in SYSTEM_LIBS:
            seen[soname] = Node(soname, None, "system")
            continue
        hit = find_in_search_dirs(soname)
        if hit is None:
            seen[soname] = Node(soname, None, "MISSING")
            continue
        real, src = hit
        n = Node(soname, real, src)
        n.needed = read_needed(real)
        seen[soname] = n
        q.extend(n.needed)

    return seen

# ── main ───────────────────────────────────────────────────────────────────
def main():
    nodes = walk(DEFAULT_ENTRIES)

    lines = []
    for n in sorted(nodes.values(), key=lambda x: x.soname.lower()):
        if n.source not in ("deps", "blender") or not n.path:
            continue
        lines.append(f"{n.soname}\t{n.path}")
    lines.append(f"libblender.so\t{DEFAULT_ENTRIES[0]}")
    for extra in EXTRA_PLUGINS:
        if not extra.exists():
            print(f"WARNING: EXTRA_PLUGIN missing: {extra}", file=sys.stderr)
            continue
        lines.append(f"{extra.name}\t{extra}")
    print("\n".join(lines))

    # Non-zero exit iff there's any MISSING lib.
    return 1 if any(n.source == "MISSING" for n in nodes.values()) else 0

if __name__ == "__main__":
    sys.exit(main())
