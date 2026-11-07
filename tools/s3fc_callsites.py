#!/usr/bin/env python3
"""
s3fc_callsites.py - offline analysis of a Sims 3 executable for Sims3FreezeCursor.

Answers three questions without launching the game:

  1. How many places in the image match our MonoScriptHost::ProcessTasks pattern?
     We have always taken the FIRST match and never counted. If a build has two,
     we have been hooking whichever came first in memory order.

  2. Is the address we resolve actually called from anywhere? A function nothing
     calls is not the function we think it is.

  3. How many call sites does it have? One means call-site hooking is viable and
     leaves the target's bytes untouched.

Usage:   python s3fc_callsites.py "C:\\path\\to\\TS3W.exe"

Only works on an unpacked image. The EA App TS3.exe has an encrypted .text
(entropy 8.000, one prologue in 12 MB) and will report zero of everything -
that is the packing, not a result. Steam/retail TS3W.exe is unpacked and fine.
"""

import re
import struct
import sys

# The pattern Addresses::Initialize scans for, and the constant it falls back to.
PROCESSTASKS_PATTERN = ("55 8B EC 83 E4 F8 83 EC 50 53 55 56 8B D9 8B 83 ?? ?? ?? ?? "
                        "8D B3 ?? ?? ?? ?? 57 50 8B CE")
PROCESSTASKS_FALLBACK = 0x00D7FC60

KNOWN_BUILDS = {
    0x52D872DA: "Retail 1.67.2.024002",
    0x52DEC247: "Steam 1.67.2.024037",
    0x6707155C: "EA App 1.69.47.024017",
    0x568D4BAC: "EA App 1.69.43.024017",
}

IMAGE_SCN_MEM_EXECUTE = 0x20000000


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:2] != b"MZ":
            raise SystemExit("not a PE file (no MZ)")
        e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise SystemExit("not a PE file (no PE signature)")

        fh = e_lfanew + 4
        (self.machine, self.nsections, self.timestamp,
         _psym, _nsym, self.opt_size, _chars) = struct.unpack_from("<HHIIIHH", d, fh)
        oh = fh + 20
        self.magic = struct.unpack_from("<H", d, oh)[0]
        if self.magic != 0x10B:
            raise SystemExit("expected a 32-bit PE (PE32), got magic 0x%X" % self.magic)
        self.imagebase = struct.unpack_from("<I", d, oh + 28)[0]

        self.sections = []
        sh = oh + self.opt_size
        for i in range(self.nsections):
            off = sh + i * 40
            name = d[off:off + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", d, off + 8)
            characteristics = struct.unpack_from("<I", d, off + 36)[0]
            self.sections.append({
                "name": name, "va": va, "vsize": vsize,
                "rawptr": rawptr, "rawsize": rawsize,
                "exec": bool(characteristics & IMAGE_SCN_MEM_EXECUTE),
            })

    def exec_sections(self):
        return [s for s in self.sections if s["exec"] and s["rawsize"] > 0]

    def va_of(self, section, index_in_raw):
        return self.imagebase + section["va"] + index_in_raw

    def read_va(self, va, n):
        for s in self.sections:
            start = self.imagebase + s["va"]
            if start <= va < start + max(s["vsize"], s["rawsize"]):
                off = s["rawptr"] + (va - start)
                if off + n <= len(self.data):
                    return self.data[off:off + n]
        return b""


def pattern_to_regex(pattern):
    parts = pattern.split()
    out = b""
    for p in parts:
        if p == "??":
            out += b"."
        else:
            out += re.escape(bytes([int(p, 16)]))
    return re.compile(out, re.DOTALL)


def find_pattern(img, pattern):
    rx = pattern_to_regex(pattern)
    hits = []
    for s in img.exec_sections():
        raw = img.data[s["rawptr"]:s["rawptr"] + s["rawsize"]]
        for m in rx.finditer(raw):
            hits.append((img.va_of(s, m.start()), s["name"]))
    return hits


def find_call_sites(img, target_va):
    """Every E8 (call rel32) and E9 (jmp rel32) whose computed target is target_va."""
    sites = []
    for s in img.exec_sections():
        raw = img.data[s["rawptr"]:s["rawptr"] + s["rawsize"]]
        base = img.va_of(s, 0)
        for opcode, kind in ((0xE8, "call"), (0xE9, "jmp")):
            i = raw.find(bytes([opcode]))
            while i != -1:
                if i + 5 <= len(raw):
                    rel = struct.unpack_from("<i", raw, i + 1)[0]
                    if (base + i + 5 + rel) == target_va:
                        sites.append((base + i, kind, s["name"]))
                i = raw.find(bytes([opcode]), i + 1)
    return sites


def hexbytes(b):
    return " ".join("%02X" % c for c in b) if b else "(unreadable)"


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    img = Image(sys.argv[1])

    print("=" * 72)
    print("image base   : 0x%08X" % img.imagebase)
    print("TimeDateStamp: 0x%08X  (%s)"
          % (img.timestamp, KNOWN_BUILDS.get(img.timestamp, "UNRECOGNISED BUILD")))
    print("sections     : " + ", ".join(
        "%s%s" % (s["name"], "*" if s["exec"] else "") for s in img.sections)
        + "        (* = executable)")
    print("=" * 72)

    # --- 1. how many pattern matches? -------------------------------------
    hits = find_pattern(img, PROCESSTASKS_PATTERN)
    print()
    print("ProcessTasks pattern matches: %d" % len(hits))
    if not hits:
        print("  NONE. Either this is a packed image (EA App), or the pattern")
        print("  does not describe any function in this build.")
    for va, sect in hits:
        marker = "   <-- equals kProcessTasksFallback" if va == PROCESSTASKS_FALLBACK else ""
        print("  0x%08X  in %-8s  %s%s" % (va, sect, hexbytes(img.read_va(va, 16)), marker))

    # --- 2/3. call sites --------------------------------------------------
    candidates = [va for va, _ in hits]
    if PROCESSTASKS_FALLBACK not in candidates:
        candidates.append(PROCESSTASKS_FALLBACK)

    for va in candidates:
        print()
        label = "pattern match" if va in [h[0] for h in hits] else "documented fallback"
        print("call sites targeting 0x%08X  (%s):" % (va, label))
        print("  bytes there: %s" % hexbytes(img.read_va(va, 16)))
        sites = find_call_sites(img, va)
        if not sites:
            print("  NONE FOUND.")
            print("  Nothing in the image calls or jumps to this address. Either it is")
            print("  reached only indirectly (through a vtable or a function pointer),")
            print("  or it is not a function entry point at all.")
        else:
            print("  %d site(s):" % len(sites))
            for site, kind, sect in sites:
                ctx = img.read_va(site - 8, 21)
                print("    0x%08X  %-4s  in %-8s   %s" % (site, kind, sect, hexbytes(ctx)))
                print("                                        %s^^ the 5 bytes we would patch"
                      % (" " * 24))

    print()
    print("=" * 72)
    print("Reading the result:")
    print("  >1 pattern match      -> the scan has been picking one arbitrarily")
    print("   0 call sites         -> that address is not called directly; hooking")
    print("                          the call site is not possible, and the address")
    print("                          itself deserves suspicion")
    print("   1 call site          -> call-site hooking is viable and leaves the")
    print("                          target's own bytes untouched")
    print("  many call sites       -> viable but every site must be patched")
    print("=" * 72)


if __name__ == "__main__":
    main()
