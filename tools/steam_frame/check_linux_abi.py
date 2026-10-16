#!/usr/bin/env python3
"""Reject a DiabloVR package that will not run on the Steam Frame's runtime.

The ceiling is glibc 2.39 and GLIBCXX 3.4.32 (the build container is Ubuntu 24.04, gcc 13). The
Frame itself has glibc 2.39, and its SteamLinuxRuntime_4-arm64 has glibc 2.41 and GLIBCXX 3.4.33. For every ELF in the package it checks

  * it is a 64-bit aarch64 object,
  * it needs no glibc / libstdc++ symbol version newer than the floor,
  * its RUNPATH holds only $ORIGIN-relative entries (no build tree paths),
  * every DT_NEEDED is a system library or is found through its own RUNPATH inside the package
    (the whole dependency closure, as the dynamic loader would search it).

    python3 tools/steam_frame/check_linux_abi.py build-frame/package/DiabloVR

Exits non-zero if anything fails, after printing every problem.
"""

import argparse
import os
import struct
import sys

# What the package may take from the running system. glibc and libstdc++ are the runtime's, and their
# age is what the symbol version ceilings bound. Everything else (SDL2, DXVK) travels in lib/:
# the SteamLinuxRuntime has copies of some of them, but not necessarily ones that match.
# Libraries the engine dlopen()s (libvulkan, libX11, libpulse, libasound, ...) are not DT_NEEDED
# anywhere, so they are neither checked nor bundled: the Frame's runtime and driver provide them.
SYSTEM_LIBS = {
    "ld-linux-aarch64.so.1",
    "libc.so.6",
    "libm.so.6",
    "libdl.so.2",
    "libpthread.so.0",
    "librt.so.1",
    "libgcc_s.so.1",
    "libstdc++.so.6",
}

EM_AARCH64 = 183
DT_NEEDED, DT_RPATH, DT_RUNPATH = 1, 15, 29


def parse_version(text):
    """GLIBC_2.38 -> (2, 38). Unversioned tags sort below everything."""
    tail = text.rsplit("_", 1)[-1]
    try:
        return tuple(int(part) for part in tail.split("."))
    except ValueError:
        return (0,)


def _cstr(blob, offset):
    return blob[offset:blob.index(b"\0", offset)].decode("utf-8", "replace")


class ElfFile:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as handle:
            self.data = handle.read()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 2 or self.data[5] != 1:
            raise ValueError("not a little-endian 64-bit ELF")
        (self.e_machine,) = struct.unpack_from("<H", self.data, 18)
        (sh_off,) = struct.unpack_from("<Q", self.data, 40)
        sh_entsize, sh_num, sh_strndx = struct.unpack_from("<HHH", self.data, 58)
        self.sections = []
        for index in range(sh_num):
            f = struct.unpack_from("<IIQQQQIIQQ", self.data, sh_off + index * sh_entsize)
            self.sections.append({"name_off": f[0], "type": f[1], "offset": f[4], "size": f[5]})
        names = self._raw(self.sections[sh_strndx])
        for section in self.sections:
            section["name"] = _cstr(names, section["name_off"])

    def _raw(self, section):
        return self.data[section["offset"]:section["offset"] + section["size"]]

    def blob(self, name):
        for section in self.sections:
            if section["name"] == name:
                return self._raw(section)
        return b""

    def dynamic(self):
        """(DT_NEEDED names, RPATH, RUNPATH)."""
        dynamic, strings = self.blob(".dynamic"), self.blob(".dynstr")
        needed, rpath, runpath = [], None, None
        for offset in range(0, max(0, len(dynamic) - 15), 16):
            tag, value = struct.unpack_from("<qQ", dynamic, offset)
            if tag == 0:
                break
            if tag == DT_NEEDED:
                needed.append(_cstr(strings, value))
            elif tag == DT_RPATH:
                rpath = _cstr(strings, value)
            elif tag == DT_RUNPATH:
                runpath = _cstr(strings, value)
        return needed, rpath, runpath

    def imported_versions(self):
        """Map each required symbol version (GLIBC_2.34, ...) to the symbols that pull it in."""
        section = next((s for s in self.sections if s["name"] == ".gnu.version_r"), None)
        if not section:
            return {}
        body, strings = self._raw(section), self.blob(".dynstr")
        by_index, offset = {}, 0
        while offset < len(body):
            _, count, _, aux_off, next_off = struct.unpack_from("<HHIII", body, offset)
            aux = offset + aux_off
            for _ in range(count):
                _, _, other, name_off, aux_next = struct.unpack_from("<IHHII", body, aux)
                by_index[other] = _cstr(strings, name_off)
                if not aux_next:
                    break
                aux += aux_next
            if not next_off:
                break
            offset += next_off
        versym, dynsym = self.blob(".gnu.version"), self.blob(".dynsym")
        result = {version: [] for version in by_index.values()}
        for index in range(min(len(versym) // 2, len(dynsym) // 24)):
            (raw,) = struct.unpack_from("<H", versym, index * 2)
            version = by_index.get(raw & 0x7FFF)
            if version is not None:
                (name_off,) = struct.unpack_from("<I", dynsym, index * 24)
                result[version].append(_cstr(strings, name_off))
        return result


def find_elves(root):
    elves = []
    for directory, _, files in os.walk(root):
        for name in sorted(files):
            path = os.path.join(directory, name)
            if os.path.islink(path) or not os.path.isfile(path):
                continue
            with open(path, "rb") as handle:
                if handle.read(4) == b"\x7fELF":
                    elves.append(path)
    return sorted(elves)


def resolve_runpath(runpath, elf_dir):
    """The directories a RUNPATH names, with $ORIGIN expanded; None entries are not $ORIGIN-based."""
    dirs = []
    for entry in runpath.split(":"):
        if entry.startswith("$ORIGIN") or entry.startswith("${ORIGIN}"):
            entry = entry.replace("${ORIGIN}", "$ORIGIN")
            dirs.append(os.path.normpath(entry.replace("$ORIGIN", elf_dir, 1)))
        elif entry:
            dirs.append(None)
    return dirs


def check_package(root, max_glibc, max_glibcxx):
    limits = {"GLIBC": "GLIBC_" + max_glibc, "GLIBCXX": "GLIBCXX_" + max_glibcxx}
    problems = []
    elves = find_elves(root)
    if not elves:
        problems.append(f"{root}: no ELF files")
    worst = {prefix: (0,) for prefix in limits}
    for path in elves:
        rel = os.path.relpath(path, root)
        try:
            elf = ElfFile(path)
        except (ValueError, struct.error, IndexError) as error:
            problems.append(f"{rel}: {error}")
            continue
        if elf.e_machine != EM_AARCH64:
            problems.append(f"{rel}: machine {elf.e_machine}, not aarch64 ({EM_AARCH64})")

        versions = elf.imported_versions()
        for prefix, ceiling in limits.items():
            required = [v for v in versions if v.startswith(prefix + "_")]
            if not required:
                continue
            top = max(required, key=parse_version)
            worst[prefix] = max(worst[prefix], parse_version(top))
            if parse_version(top) > parse_version(ceiling):
                culprits = ", ".join(sorted(set(versions[top]))[:6]) or "?"
                problems.append(f"{rel}: needs {top} (ceiling {ceiling}) via {culprits}")

        needed, rpath, runpath = elf.dynamic()
        if rpath:
            problems.append(f"{rel}: has DT_RPATH {rpath} (use RUNPATH)")
        search = []
        if runpath:
            elf_dir = os.path.dirname(os.path.abspath(path))
            for directory, entry in zip(resolve_runpath(runpath, elf_dir), runpath.split(":")):
                if directory is None:
                    problems.append(f"{rel}: RUNPATH entry {entry!r} is not $ORIGIN-relative")
                else:
                    search.append(directory)
        for lib in needed:
            if lib in SYSTEM_LIBS:
                continue
            found = [d for d in search if os.path.isfile(os.path.join(d, lib))]
            if not found:
                where = runpath or "no RUNPATH"
                problems.append(f"{rel}: needs {lib}, not in a system path and not found through RUNPATH ({where})")
            elif not os.path.abspath(found[0]).startswith(os.path.abspath(root) + os.sep):
                problems.append(f"{rel}: {lib} resolves outside the package ({found[0]})")
    return elves, worst, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("package", help="package directory (build-frame/package/DiabloVR)")
    parser.add_argument("--max-glibc", default="2.39")
    parser.add_argument("--max-glibcxx", default="3.4.32")
    args = parser.parse_args()

    root = os.path.abspath(args.package)
    elves, worst, problems = check_package(root, args.max_glibc, args.max_glibcxx)
    for problem in problems:
        print("FAIL  " + problem)
    if problems:
        print(f"\n{len(problems)} problem(s) in {len(elves)} binaries: the package would not run on the Frame.")
        return 1
    top = ", ".join("%s_%s" % (prefix, ".".join(map(str, version))) for prefix, version in sorted(worst.items()))
    print(f"ok    {len(elves)} aarch64 binaries, highest needed {top} (ceilings glibc {args.max_glibc}, "
          f"GLIBCXX {args.max_glibcxx}); every dependency is a system library or inside the package")
    return 0


if __name__ == "__main__":
    sys.exit(main())
