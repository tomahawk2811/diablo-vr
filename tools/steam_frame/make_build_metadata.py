#!/usr/bin/env python3
"""Records what a DiabloVR package was built from, and the SHA-256 of every file in it.

    make_build_metadata.py source OUT.json
        Write the state of the source tree (revision, clean or dirty, a fingerprint of the changes).
        build.sh runs this when it builds the game.

    make_build_metadata.py package DIR --source OUT.json [--vr] [--tool NAME=VERSION ...]
        Write DIR/BUILD-METADATA.json. The source state is the one saved when the game was built.
        It is also read again now, and the file says whether the tree changed in between.

Runs on the host, where git works. deploy.sh compares the installed files with the hashes.
A tree with uncommitted or untracked source files is "dirty". The fingerprint covers the diff
against HEAD and the content of untracked files, so two dirty builds can be told apart.
"""

import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
METADATA = "BUILD-METADATA.json"


def git(*args, raw=False):
    out = subprocess.run(["git", "-C", REPO, *args], check=True, capture_output=True).stdout
    return out if raw else out.decode()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def source_state():
    state = {
        "revision": git("rev-parse", "HEAD").strip(),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD").strip(),
        "subject": git("log", "-1", "--format=%s").strip(),
        "commit_time": git("log", "-1", "--format=%cI").strip(),
    }
    # build-* is ignored; anything else that differs makes the tree dirty.
    status = git("status", "--porcelain=v1", "--untracked-files=normal").splitlines()
    state["tree_state"] = "dirty" if status else "clean"
    if status:
        state["dirty_files"] = [line[3:] for line in status][:50]
        fingerprint = hashlib.sha256(git("diff", "HEAD", "--binary", raw=True))
        untracked = git("ls-files", "--others", "--exclude-standard", "-z").split("\0")
        for rel in sorted(path for path in untracked if path):
            path = os.path.join(REPO, rel)
            if os.path.isfile(path) and not os.path.islink(path):
                fingerprint.update(f"{rel}\0{sha256_file(path)}\0".encode())
        state["changes_sha256"] = fingerprint.hexdigest()
    return state


def write_source(out):
    state = source_state()
    with open(out, "w") as handle:
        json.dump(state, handle, indent=2)
        handle.write("\n")
    print(f"source {state['revision'][:12]} ({state['tree_state']})")


def write_package(args):
    root = os.path.abspath(args.package)
    with open(args.source) as handle:
        built = json.load(handle)
    now = source_state()
    same = all(built.get(k) == now.get(k) for k in ("revision", "tree_state", "changes_sha256"))

    files = {}
    for directory, _, names in os.walk(root):
        for name in names:
            path = os.path.join(directory, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if rel == METADATA:
                continue
            files[rel] = {
                "size": os.path.getsize(path),
                "mode": "%04o" % (os.stat(path).st_mode & 0o7777),
                "sha256": sha256_file(path),
            }

    metadata = {
        "schema": 1,
        "package": "DiabloVR",
        "target": "aarch64-linux-gnu",
        "vr": args.vr,
        "created": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": built,
        "source_changed_since_build": not same,
        "tools": dict(item.split("=", 1) for item in args.tool),
        "game_data": "none (the package never holds the game's own data; it goes to the device's data folder)",
        "files": dict(sorted(files.items())),
    }
    if not same:
        metadata["source_at_packaging"] = now
    with open(os.path.join(root, METADATA), "w") as handle:
        json.dump(metadata, handle, indent=2)
        handle.write("\n")
    note = "" if same else ", the tree changed since the game was built"
    print(f"wrote {METADATA}: {len(files)} files, built from {built['revision'][:12]} ({built['tree_state']}){note}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    one = sub.add_parser("source")
    one.add_argument("out")
    two = sub.add_parser("package")
    two.add_argument("package")
    two.add_argument("--source", required=True)
    two.add_argument("--vr", action="store_true")
    two.add_argument("--tool", action="append", default=[], metavar="NAME=VERSION")
    args = parser.parse_args()
    if args.command == "source":
        write_source(args.out)
    else:
        write_package(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
