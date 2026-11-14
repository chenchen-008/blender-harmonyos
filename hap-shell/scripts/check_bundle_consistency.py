#!/usr/bin/env python3
"""Fail fast when the HAP bundle name and native public_home disagree.

This is the check that would have caught the tiny-UI regression:
app.json5 / EntryAbility.PUBLIC_HOME / libblender.so must all use the same
com.*.blenderonharmonyos value.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "AppScope" / "app.json5"
ETS = ROOT / "entry/src/main/ets/entryability/EntryAbility.ets"
LIB = ROOT / "entry/libs/arm64-v8a/libblender.so"


def main() -> int:
    app = json.loads(APP.read_text(encoding="utf-8"))
    bundle = str(app["app"]["bundleName"])

    ets = ETS.read_text(encoding="utf-8")
    match = re.search(r"PUBLIC_HOME\s*=\s*'([^']+)'", ets)
    if not match:
        print("FAIL: PUBLIC_HOME not found in EntryAbility.ets")
        return 1
    suffix = match.group(1).rstrip("/").rsplit("/", 1)[-1]

    data = LIB.read_bytes()
    found = sorted({m.decode("ascii") for m in re.findall(rb"com\.[a-z0-9_.-]+\.blenderonharmonyos", data)})

    problems = []
    if suffix != bundle:
        problems.append("EntryAbility PUBLIC_HOME suffix %r != app bundle %r" % (suffix, bundle))
    if found != [bundle]:
        problems.append("libblender.so public_home %r != app bundle %r" % (found, bundle))

    print("bundle       =", bundle)
    print("public_home  =", suffix)
    print("libblender   =", found)
    if problems:
        for p in problems:
            print("FAIL:", p)
        return 1
    print("OK: bundle/public_home/libblender are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
