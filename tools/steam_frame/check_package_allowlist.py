#!/usr/bin/env python3
"""Reject a DiabloVR package that holds anything but the port's own files.

The package is an allowlist: every file has to match a pattern below. The game's data (DIABDAT.MPQ,
spawn.mpq, hellfire.mpq, hf*.mpq, any MPQ at all but the port's own devilutionx.mpq), saves, the
config file and Vulkan drivers or loaders are also named in a denylist, so the error says what a
stray file is. The user's own copy of the game goes to the device's data folder, never in here.

    python3 tools/steam_frame/check_package_allowlist.py build-frame/package/DiabloVR [--vr]

--vr allows the vrassets/ folder. Without it the package is the flat game.
"""

import os
import re
import sys

ALLOWED = [
    r"devilutionx",
    r"devilutionx\.mpq",                  # the game's own assets, when smpq built them
    r"assets/[A-Za-z0-9_.+ /-]+",         # the game's own assets, loose
    r"mods/hf/[A-Za-z0-9_.+ /-]+",        # the game's own Hellfire mod, loose (needs the user's hellfire.mpq)
    r"lib/[A-Za-z0-9_.+-]+\.so(\.[0-9]+)*",
    r"LAUNCH-STEAM-FRAME\.sh",
    r"BUILD-METADATA\.json",
]
VR_ALLOWED = [
    r"vrassets/[A-Za-z0-9_.+ /-]+",
]

# Directories the package may have. assets/, mods/hf/ and vrassets/ may have subfolders.
DIRS = [r"lib", r"assets(/[A-Za-z0-9_.+ -]+)*", r"mods", r"mods/hf(/[A-Za-z0-9_.+ -]+)*"]
VR_DIRS = [r"vrassets(/[A-Za-z0-9_.+ -]+)*"]

# Named in the error message when a file looks like something that must never travel in the package.
# These are checked first and apply everywhere, assets/ included.
DENIED = [
    (r"(?i).*\.mpq", "an MPQ archive (game data; only the port's own devilutionx.mpq may be packaged)"),
    (r"(?i)(.*/)?[^/]*\.(sv|hsv|sav|save)", "a save game"),
    (r"(?i)(.*/)?(save|saves|multi_\d+)(/.*)?", "a save folder"),
    (r"(?i)(.*/)?(diablo|hellfire)\.ini", "a config file"),
    (r"(?i)(.*/)?[^/]*_icd[^/]*\.json", "a Vulkan driver manifest"),
    (r"(?i)(.*/)?(icd\.d|vulkan|implicit_layer\.d|explicit_layer\.d)(/.*)?", "a Vulkan driver or layer folder"),
    (r"(?i)(.*/)?lib(vulkan|vklayer|gallium|egl|glx|gl)[^/]*\.so[^/]*", "a Vulkan or GL driver or loader (the device has its own)"),
]

# The binary and SDL2 are the largest things here, a few tens of MB. Nothing legitimate is near this.
MAX_FILE_BYTES = 64 * 1024 * 1024


def main():
    args = [a for a in sys.argv[1:] if a != "--vr"]
    vr = len(args) != len(sys.argv) - 1
    if len(args) != 1:
        print(__doc__)
        return 2
    root = os.path.abspath(args[0])
    if not os.path.isdir(root):
        print(f"FAIL  {root} is not a directory")
        return 1

    allowed = [re.compile(p + r"\Z") for p in ALLOWED + (VR_ALLOWED if vr else [])]
    dirs = [re.compile(p + r"\Z") for p in DIRS + (VR_DIRS if vr else [])]
    denied = [(re.compile(p + r"\Z"), why) for p, why in DENIED]

    problems, count, total = [], 0, 0
    for directory, subdirs, files in os.walk(root):
        for name in sorted(files) + sorted(subdirs):
            path = os.path.join(directory, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            kind = next((why for pattern, why in denied if pattern.match(rel)), None)
            if rel == "devilutionx.mpq":
                kind = None
            if kind:
                problems.append(f"{rel}: {kind}")
                continue
            if os.path.islink(path):
                problems.append(f"{rel}: symbolic link")
                continue
            if os.path.isdir(path):
                if not any(pattern.match(rel) for pattern in dirs):
                    problems.append(f"{rel}/: directory is not part of the package layout")
                continue
            count += 1
            size = os.path.getsize(path)
            total += size
            if name.startswith("._") or name == ".DS_Store":
                problems.append(f"{rel}: macOS metadata file")
            elif not any(pattern.match(rel) for pattern in allowed):
                problems.append(f"{rel}: not in the allowlist")
            elif size > MAX_FILE_BYTES:
                problems.append(f"{rel}: {size} bytes is too big for one of the port's files")

    for problem in problems:
        print("FAIL  " + problem)
    if problems:
        print(f"\n{len(problems)} problem(s): the package holds files that are not the port's own.")
        return 1
    print(f"ok    {count} files, {total / 1048576:.1f} MiB, every one on the allowlist, no game data, saves or Vulkan drivers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
