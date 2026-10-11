#!/usr/bin/env python3
"""Offline CPU hotspot analysis for EliteDangerous64.exe (build 332841).

Reads a CSV of raw render-thread CPU samples and the game executable, which
is opened read-only, and reports:

  * per function, from the x64 .pdata RUNTIME_FUNCTION table: exclusive
    (leaf) and inclusive (on-stack) sample counts, the RVA range and size, and
    the top callers taken from the recorded stack frames. A chained unwind
    fragment (UNW_FLAG_CHAININFO, a hot/cold split) is attributed to its
    primary function, whose RVA range is the key;
  * with --disasm 0xRVA, that function disassembled with capstone when it is
    installed. Each instruction is annotated with its exclusive samples, each
    call with the samples whose stack returned through it, and the direct
    call targets are listed. Without capstone the tool says so and skips it;
  * with --pe-info, the PE timestamp and image size checked against the build
    this tool is pinned to, and the .pdata function count with its parse time.

Annotating (--samples, --disasm) refuses unless the exe matches build 332841
by PE timestamp and image size. --force annotates anyway, with a warning, and
the RVAs then belong to whatever build the exe is.

Sample CSV contract (the profiler's output: one sample per row, no header
needed):

    module,rva_hex,frame1_module,frame1_rva_hex,...,frame8_module,frame8_rva_hex

  * module      the module holding the sampled leaf instruction, by basename,
                e.g. EliteDangerous64.exe; compared case-insensitively
  * rva_hex     that leaf's RVA within the module, hex, with or without 0x
  * frameN_*    the N-th caller frame, innermost first, the same format, at
                most 8 frames
  * Fewer frames is fine. A short row, a blank frame module cell, or a frame
    module with no RVA ends the frame list at that point.
  * Blank lines and lines starting with # are skipped. A line whose first two
    cells are module,rva_hex is a header and is skipped.
  * A row whose leaf or frame RVA does not parse is counted as malformed and
    reported. It is not fatal.
  * A leaf outside the game exe still counts. Its frames that lie in the exe
    get inclusive and caller credit; the leaf gets no exclusive credit.
  * Each row is one sample.

Exit codes: 0 ok; 1 usage or read error; 2 refused (build mismatch).

Usage:
    python tools\\elite_hotspots.py --pe-info [--exe PATH]
    python tools\\elite_hotspots.py --samples CSV [--top N] [--callers K] [--exe PATH]
    python tools\\elite_hotspots.py --samples CSV --disasm 0xRVA [--force]
    python tools\\elite_hotspots.py --self-test
"""
import argparse
import bisect
import collections
import csv
import datetime
import io
import os
import struct
import sys
import time

BUILD = 332841
PINNED_TIMESTAMP = 1788384820
PINNED_IMAGE_SIZE = 104894464
GAME_EXE = "EliteDangerous64.exe"
# The Epic install (no resolver in tools\install_edvr.py covers it; the path
# is the one docs\exit-crash-4d78c51.md uses). --exe overrides it.
DEFAULT_EXE = (r"C:\Program Files\Epic Games\EliteDangerous\Products"
               r"\elite-dangerous-odyssey-64\EliteDangerous64.exe")
MAX_FRAMES = 8
EXCEPTION_DIRECTORY = 3
UNW_CHAININFO = 0x4
RF = struct.Struct("<III")
REFUSED = 2


class PEError(ValueError):
    pass


class Image:
    """A read-only view of a PE32+ image: identity fields, sections, and
    the exception directory (.pdata). Reads go through read_at only."""

    def __init__(self, fh):
        self.fh = fh
        fh.seek(0, os.SEEK_END)
        self.size = fh.tell()
        if self.read_at(0, 2) != b"MZ":
            raise PEError("not a PE file (no MZ header)")
        nt = struct.unpack("<I", self.read_at(0x3C, 4))[0]
        if self.read_at(nt, 4) != b"PE\0\0":
            raise PEError("bad PE signature")
        fhdr = self.read_at(nt + 4, 20)
        machine, nsec, self.timestamp = struct.unpack_from("<HHI", fhdr, 0)
        opt_size = struct.unpack_from("<H", fhdr, 16)[0]
        if machine != 0x8664:
            raise PEError("not an x64 image (machine 0x%04x)" % machine)
        opt = self.read_at(nt + 24, opt_size)
        if len(opt) < 112 or struct.unpack_from("<H", opt, 0)[0] != 0x20B:
            raise PEError("not a PE32+ optional header")
        self.size_of_image = struct.unpack_from("<I", opt, 56)[0]
        ndirs = min(struct.unpack_from("<I", opt, 108)[0], (opt_size - 112) // 8, 16)
        self.directories = [struct.unpack_from("<II", opt, 112 + 8 * i)
                            for i in range(ndirs)]
        if not 0 < nsec <= 96:
            raise PEError("invalid section count %d" % nsec)
        table = self.read_at(nt + 24 + opt_size, 40 * nsec)
        self.sections = []
        for i in range(nsec):
            name = table[40 * i:40 * i + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, rva, rawsize, raw = struct.unpack_from("<IIII", table, 40 * i + 8)
            self.sections.append((name, rva, vsize, raw, rawsize))

    def read_at(self, off, n):
        if off < 0 or n < 0 or off + n > self.size:
            raise PEError("read of %d bytes at 0x%x lies outside the file" % (n, off))
        self.fh.seek(off)
        data = self.fh.read(n)
        if len(data) != n:
            raise PEError("short read at 0x%x" % off)
        return data

    def file_offset(self, rva, size):
        for _name, sec_rva, vsize, raw, rawsize in self.sections:
            span = max(vsize, rawsize)
            if sec_rva <= rva and rva + size <= sec_rva + span:
                delta = rva - sec_rva
                if delta + size > rawsize:
                    raise PEError("RVA 0x%x+%d enters the zero-fill of its section" % (rva, size))
                return raw + delta
        raise PEError("RVA 0x%x+%d lies in no section" % (rva, size))

    def read_rva(self, rva, n):
        return self.read_at(self.file_offset(rva, n), n)


class FunctionTable:
    """RVA ranges from .pdata. A function's key is its primary (begin, end)."""

    def __init__(self):
        self.total = 0          # RUNTIME_FUNCTION slots in the directory
        self.invalid = 0        # slots skipped: empty or out of the image
        self.unreadable = 0     # unwind info that could not be read
        self.ranges = []        # sorted (begin, end, key)
        self.begins = []
        self.parts = collections.defaultdict(list)   # key -> [(begin, end)]
        self.fragment_count = 0

    def lookup(self, rva):
        i = bisect.bisect_right(self.begins, rva) - 1
        if i < 0:
            return None
        begin, end, key = self.ranges[i]
        return key if begin <= rva < end else None

    def functions(self):
        return list(self.parts)


def _primary(img, entry, table, depth=0):
    """The primary function an entry belongs to. Follows the chain of
    UNW_FLAG_CHAININFO entries (a fragment's trailing RUNTIME_FUNCTION names
    the function it belongs to)."""
    begin, end, unwind = entry
    try:
        head = img.read_rva(unwind, 4)
    except PEError:
        table.unreadable += 1
        return (begin, end)
    if not (head[0] >> 3) & UNW_CHAININFO or depth >= 32:
        return (begin, end)
    count = head[2]
    slots = count + (count & 1)          # codes are padded to an even count
    try:
        chained = RF.unpack(img.read_rva(unwind + 4 + 2 * slots, RF.size))
    except PEError:
        table.unreadable += 1
        return (begin, end)
    return _primary(img, chained, table, depth + 1)


def build_table(img):
    table = FunctionTable()
    rva, size = (img.directories[EXCEPTION_DIRECTORY]
                 if len(img.directories) > EXCEPTION_DIRECTORY else (0, 0))
    if not rva or not size:
        raise PEError("no exception directory (.pdata) in the image")
    if size % RF.size:
        raise PEError("exception directory size %d is not a multiple of 12" % size)
    raw = img.read_rva(rva, size)
    table.total = size // RF.size
    entries = []
    for i in range(0, size, RF.size):
        begin, end, unwind = RF.unpack_from(raw, i)
        if begin == 0 and end == 0 and unwind == 0:
            continue                      # alignment padding, not a function
        if not 0 < begin < end <= img.size_of_image:
            table.invalid += 1
            continue
        entries.append((begin, end, unwind))
    entries.sort()
    ranges = []
    for begin, end, unwind in entries:
        key = _primary(img, (begin, end, unwind), table)
        if key != (begin, end):
            table.fragment_count += 1
        ranges.append((begin, end, key))
        table.parts[key].append((begin, end))
    ranges.sort()
    table.ranges = ranges
    table.begins = [r[0] for r in ranges]
    return table


def parse_rva(text):
    t = text.strip()
    if t[:2].lower() == "0x":
        t = t[2:]
    if not t or t[0] in "+-":
        raise ValueError("not a hex RVA: %r" % text)
    return int(t, 16)


Sample = collections.namedtuple("Sample", "module rva frames")


def read_samples(lines):
    """Parse the sample CSV contract. Returns (samples, malformed_count)."""
    samples, malformed = [], 0
    for row in csv.reader(lines):
        cells = [c.strip() for c in row]
        if not any(cells) or cells[0].startswith("#"):
            continue
        if len(cells) >= 2 and cells[0].lower() == "module" and cells[1].lower() == "rva_hex":
            continue
        try:
            if not cells[0]:
                raise ValueError("no module")
            if len(cells) < 2 or not cells[1]:
                raise ValueError("no leaf RVA")
            rva = parse_rva(cells[1])
            frames = []
            for i in range(2, min(len(cells), 2 + 2 * MAX_FRAMES), 2):
                if not cells[i] or i + 1 >= len(cells) or not cells[i + 1]:
                    break
                frames.append((cells[i].lower(), parse_rva(cells[i + 1])))
        except ValueError:
            malformed += 1
            continue
        samples.append(Sample(cells[0].lower(), rva, tuple(frames)))
    return samples, malformed


class Report:
    def __init__(self):
        self.rows = 0            # samples parsed
        self.malformed = 0
        self.leaf_exe = 0        # leaf inside the game exe
        self.leaf_other = 0      # leaf in another module
        self.unknown = 0         # exe leaf outside every .pdata function
        self.unattributed = 0    # no exe function anywhere on the stack
        self.excl = collections.Counter()
        self.incl = collections.Counter()
        self.callers = collections.defaultdict(collections.Counter)
        self.leaf_at = collections.Counter()   # exact leaf RVA -> samples
        self.site_at = collections.Counter()   # frame RVA -> samples


def aggregate(samples, table, malformed=0):
    rep = Report()
    rep.malformed = malformed
    exe = GAME_EXE.lower()
    for s in samples:
        rep.rows += 1
        in_exe = s.module == exe
        leaf_key = None
        if in_exe:
            rep.leaf_exe += 1
            leaf_key = table.lookup(s.rva)
            if leaf_key is None:
                rep.unknown += 1
            rep.leaf_at[s.rva] += 1
        else:
            rep.leaf_other += 1
        stack = [leaf_key]
        for mod, rva in s.frames:
            if mod == exe:
                stack.append(table.lookup(rva))
                rep.site_at[rva] += 1
            else:
                stack.append(None)
        if leaf_key is not None:
            rep.excl[leaf_key] += 1
        known = [k for k in stack if k is not None]
        if not known:
            rep.unattributed += 1
        for k in set(known):
            rep.incl[k] += 1
        pairs = set()
        for i in range(len(stack) - 1):
            if stack[i] is not None and stack[i + 1] is not None:
                pairs.add((stack[i], stack[i + 1]))   # stack[i+1] called stack[i]
        for f, g in pairs:
            rep.callers[f][g] += 1
    return rep


def ranked(rep):
    keys = set(rep.excl) | set(rep.incl)
    return sorted(keys, key=lambda k: (-rep.excl[k], -rep.incl[k], k))


def build_identity(img):
    ok = img.timestamp == PINNED_TIMESTAMP and img.size_of_image == PINNED_IMAGE_SIZE
    try:
        when = datetime.datetime.fromtimestamp(
            img.timestamp, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    except (OverflowError, OSError, ValueError):
        when = "unrepresentable"
    line = "PE timestamp %d (%s), image size %d (0x%X)" % (
        img.timestamp, when, img.size_of_image, img.size_of_image)
    if ok:
        line += ": matches build %d" % BUILD
    else:
        line += (": does NOT match build %d (expects timestamp %d, image size %d)"
                 % (BUILD, PINNED_TIMESTAMP, PINNED_IMAGE_SIZE))
    return ok, line


def _pdata_line(table, ms):
    return ("pdata: %d RUNTIME_FUNCTION entries (%d invalid, %d unwind unreadable) -> "
            "%d functions, %d chained fragments, parsed in %.1f ms"
            % (table.total, table.invalid, table.unreadable,
               len(table.parts), table.fragment_count, ms))


def pe_info(img, out=None):
    out = out or sys.stdout
    ok, line = build_identity(img)
    print("%s: %s" % (GAME_EXE, line), file=out)
    t0 = time.perf_counter()
    table = build_table(img)
    print(_pdata_line(table, (time.perf_counter() - t0) * 1000.0), file=out)
    return 0 if ok else REFUSED


def import_capstone():
    try:
        import capstone
        from capstone import x86  # noqa: F401  (the operand constants)
    except ImportError:
        return None
    return capstone


def disassemble(img, table, rva, rep, capstone_mod, out):
    key = table.lookup(rva)
    if key is None:
        print("0x%08X is not inside any .pdata function" % rva, file=out)
        return 1
    parts = sorted(table.parts[key])
    print("", file=out)
    print("function 0x%08X-0x%08X (size 0x%X, %d part(s)): exclusive %d, inclusive %d"
          % (key[0], key[1], key[1] - key[0], len(parts),
             rep.excl[key], rep.incl[key]), file=out)
    if capstone_mod is None:
        print("capstone is not installed (import capstone failed): disassembly skipped. "
              "Install it yourself with: pip install capstone", file=out)
        return 0
    cs = capstone_mod.Cs(capstone_mod.CS_ARCH_X86, capstone_mod.CS_MODE_64)
    cs.detail = True
    imm = getattr(getattr(capstone_mod, "x86", None), "X86_OP_IMM", 2)
    called = collections.Counter()      # call-target key -> call sites
    via = collections.Counter()         # call-target key -> stack samples
    outside = 0
    starts = set()
    for begin, end in parts:
        code = img.read_rva(begin, end - begin)
        for ins in cs.disasm(code, begin):
            starts.add(ins.address)
            note = ""
            if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == imm:
                target = ins.operands[0].imm
                tkey = table.lookup(target)
                stacked = rep.site_at.get(ins.address + ins.size, 0)
                if tkey is None:
                    outside += 1
                    note = "-> 0x%08X (outside .pdata)" % target
                else:
                    called[tkey] += 1
                    via[tkey] += stacked
                    note = "-> 0x%08X (fn 0x%08X-0x%08X)" % (target, tkey[0], tkey[1])
            print("  0x%08X  %-6s %-22s excl=%d via=%d %s" % (
                ins.address, ins.mnemonic, ins.op_str,
                rep.leaf_at.get(ins.address, 0),
                rep.site_at.get(ins.address + ins.size, 0), note), file=out)
    off = sum(n for addr, n in rep.leaf_at.items()
              if addr not in starts and any(b <= addr < e for b, e in parts))
    if off:
        print("leaf samples on no instruction boundary in this function: %d "
              "(a wrong build, or RVAs read from another mapping)" % off, file=out)
    print("", file=out)
    print("call targets (stack samples through the call):", file=out)
    for tkey in sorted(called, key=lambda k: (-via[k], k)):
        print("  0x%08X-0x%08X  call sites %d  stack samples %d"
              % (tkey[0], tkey[1], called[tkey], via[tkey]), file=out)
    if outside:
        print("  outside .pdata: %d call site(s)" % outside, file=out)
    return 0


def analyse(img, sample_lines, top=30, callers=3, force=False, disasm_rva=None,
            capstone_mod=None, out=None):
    """Returns (exit_code, Report or None)."""
    out = out or sys.stdout
    ok, line = build_identity(img)
    print("%s: %s" % (GAME_EXE, line), file=out)
    if not ok:
        if not force:
            print("refusing to annotate: this exe is not build %d. The samples' RVAs "
                  "would be read against another build's functions. Pass --force to "
                  "annotate anyway." % BUILD, file=out)
            return REFUSED, None
        print("WARNING: --force: annotating an exe that is not build %d; "
              "the RVA mapping may be wrong." % BUILD, file=out)
    t0 = time.perf_counter()
    table = build_table(img)
    print(_pdata_line(table, (time.perf_counter() - t0) * 1000.0), file=out)
    samples, malformed = read_samples(sample_lines)
    rep = aggregate(samples, table, malformed)
    total = rep.rows
    print("samples: %d rows; %d with an exe leaf (%d outside every .pdata function), "
          "%d with a leaf elsewhere; %d with no exe function on the stack; %d malformed"
          % (total, rep.leaf_exe, rep.unknown, rep.leaf_other, rep.unattributed,
             rep.malformed), file=out)

    def pct(n):
        return 100.0 * n / total if total else 0.0

    print("", file=out)
    print("%-5s %8s %7s %8s %7s  %-23s %8s %6s" % (
        "rank", "excl", "excl%", "incl", "incl%", "function (RVA range)", "size", "parts"),
        file=out)
    for rank, key in enumerate(ranked(rep)[:top], 1):
        print("%-5d %8d %6.2f%% %8d %6.2f%%  0x%08X-0x%08X %8s %6d" % (
            rank, rep.excl[key], pct(rep.excl[key]), rep.incl[key], pct(rep.incl[key]),
            key[0], key[1], "0x%X" % (key[1] - key[0]), len(table.parts[key])), file=out)
        for caller, n in rep.callers.get(key, collections.Counter()).most_common(callers):
            print("%-5s   called from 0x%08X-0x%08X  %d" % (
                "", caller[0], caller[1], n), file=out)
    if disasm_rva is not None:
        rc = disassemble(img, table, disasm_rva, rep, capstone_mod, out)
        if rc:
            return rc, rep
    return 0, rep


# ---------------------------------------------------------------- self-test

def _synthetic_pe(timestamp=PINNED_TIMESTAMP, size_of_image=PINNED_IMAGE_SIZE):
    """A minimal PE32+ image in memory. Its .pdata holds six entries: three
    plain functions, and a primary C with two chained fragments (D, then E,
    which chains through D to C). G calls C with a direct call."""
    buf = bytearray(0xC00)
    struct.pack_into("<H", buf, 0, 0x5A4D)                  # MZ
    struct.pack_into("<I", buf, 0x3C, 0x80)                 # e_lfanew
    buf[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", buf, 0x84, 0x8664, 2, timestamp, 0, 0, 240, 0x22)
    opt = 0x98
    struct.pack_into("<H", buf, opt, 0x20B)
    struct.pack_into("<I", buf, opt + 56, size_of_image)
    struct.pack_into("<I", buf, opt + 108, 16)
    struct.pack_into("<II", buf, opt + 112 + 8 * EXCEPTION_DIRECTORY, 0x3000, 6 * RF.size)
    sec = opt + 240
    buf[sec:sec + 8] = b".text\0\0\0"
    # VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData
    struct.pack_into("<IIII", buf, sec + 8, 0x800, 0x1000, 0x800, 0x200)
    buf[sec + 40:sec + 48] = b".pdata\0\0"
    struct.pack_into("<IIII", buf, sec + 48, 0x200, 0x3000, 0x200, 0xA00)

    def raw_text(rva):
        return 0x200 + (rva - 0x1000)

    def raw_pdata(rva):
        return 0xA00 + (rva - 0x3000)

    buf[0x200:0xA00] = b"\x90" * 0x800
    # G at 0x1500: call C (0x1200) then ret. rel32 = 0x1200 - (0x1500 + 5).
    struct.pack_into("<Bi", buf, raw_text(0x1500), 0xE8, 0x1200 - 0x1505)
    buf[raw_text(0x1505)] = 0xC3
    entries = [(0x1000, 0x1100, 0x3100), (0x1100, 0x1200, 0x3110),
               (0x1200, 0x1300, 0x3140), (0x1400, 0x1450, 0x3160),
               (0x1460, 0x1470, 0x3180), (0x1500, 0x1506, 0x31A0)]
    for i, (b, e, u) in enumerate(entries):
        RF.pack_into(buf, raw_pdata(0x3000) + RF.size * i, b, e, u)
    unwind = {
        0x3100: b"\x01\x00\x00\x00",                          # A: plain
        0x3110: b"\x01\x00\x03\x00" + b"\x00" * 8,           # B: 3 codes, padded
        0x3140: b"\x01\x00\x00\x00",                          # C: plain
        0x3160: b"\x21\x00\x00\x00" + RF.pack(0x1200, 0x1300, 0x3140),  # D chains C
        0x3180: b"\x21\x00\x00\x00" + RF.pack(0x1400, 0x1450, 0x3160),  # E chains D
        0x31A0: b"\x01\x00\x00\x00",                          # G: plain
    }
    for u, data in unwind.items():
        buf[raw_pdata(u):raw_pdata(u) + len(data)] = data
    return bytes(buf)


# (module, leaf, frames) rows; the header, a comment and a blank line are
# included on purpose, and so is one malformed row.
SELF_TEST_CSV = [
    "module,rva_hex,frame1_module,frame1_rva_hex,frame2_module,frame2_rva_hex",
    "# a comment, skipped",
    "",
    "EliteDangerous64.exe,0x1010,EliteDangerous64.exe,0x1210,EliteDangerous64.exe,0x1502",
    "EliteDangerous64.exe,0x1050,EliteDangerous64.exe,0x1410",
    "EliteDangerous64.exe,0x1460",
    "EliteDangerous64.exe,0x1205,d3d11.dll,0x77000,EliteDangerous64.exe,0x1502",
    "d3d11.dll,0x77000",
    "EliteDangerous64.exe,0x2000,EliteDangerous64.exe,0x1502",
    "EliteDangerous64.exe,0x1500,EliteDangerous64.exe,0x1000,",
    "EliteDangerous64.exe,zz",
    "EliteDangerous64.exe,0x1100",
    "EliteDangerous64.exe,0x1100,EliteDangerous64.exe",
    "ntdll.dll,0x1000,EliteDangerous64.exe,0x1502",
]


def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    good = Image(io.BytesIO(_synthetic_pe()))
    check(good.timestamp == PINNED_TIMESTAMP and good.size_of_image == PINNED_IMAGE_SIZE,
          "synthetic identity fields read back")
    ok, _ = build_identity(good)
    check(ok, "synthetic image matches the pinned build")
    table = build_table(good)
    A, B, C, G = (0x1000, 0x1100), (0x1100, 0x1200), (0x1200, 0x1300), (0x1500, 0x1506)
    check(table.total == 6 and table.invalid == 0 and table.unreadable == 0,
          "six pdata entries, none invalid or unreadable")
    check(sorted(table.functions()) == sorted([A, B, C, G]),
          "chained fragments fold into four functions")
    check(table.fragment_count == 2, "two chained fragments found")
    check(len(table.parts[C]) == 3, "primary C owns itself and both fragments")
    check(len(table.parts[A]) == 1, "plain function A has one part")
    for rva, want in [(0x1000, A), (0x10FF, A), (0x1100, B), (0x1205, C),
                      (0x1410, C), (0x1460, C), (0x1469, C), (0x1470, None),
                      (0x1450, None), (0x1505, G), (0x1600, None), (0x2000, None)]:
        check(table.lookup(rva) == want, "lookup 0x%X -> %s" % (rva, want))

    samples, malformed = read_samples(SELF_TEST_CSV)
    check(malformed == 1, "one malformed row counted, the rest kept")
    check(len(samples) == 10, "ten samples read (header, comment, blank skipped)")
    check(samples[0].frames == (("elitedangerous64.exe", 0x1210),
                                ("elitedangerous64.exe", 0x1502)),
          "frames read innermost first")
    check(samples[7].frames == (), "short row (sample 8) has no frames")
    check(len(samples[3].frames) == 2, "a non-exe frame stays in the list")
    wide = ["EliteDangerous64.exe,0x1000," + ",".join(["EliteDangerous64.exe,0x1000"] * 10)]
    wide_samples, _ = read_samples(wide)
    check(len(wide_samples[0].frames) == MAX_FRAMES, "frames capped at eight")

    buf = io.StringIO()
    rc, rep = analyse(good, SELF_TEST_CSV, out=buf)
    check(rc == 0 and rep is not None, "good build annotates")
    check(rep.rows == 10 and rep.malformed == 1, "report row and malformed counts")
    check(rep.leaf_exe == 8 and rep.leaf_other == 2, "leaf module split")
    check(rep.unknown == 1 and rep.unattributed == 1, "unknown leaf and unattributed sample")
    check(rep.excl[A] == 2 and rep.excl[C] == 2 and rep.excl[B] == 2 and rep.excl[G] == 1,
          "exclusive counts")
    check(rep.incl[A] == 3 and rep.incl[C] == 4 and rep.incl[B] == 2 and rep.incl[G] == 5,
          "inclusive counts")
    check(rep.callers[A][C] == 2 and rep.callers[C][G] == 1 and rep.callers[G][A] == 1,
          "callers from consecutive exe frames")
    check(not rep.callers[C] or sum(rep.callers[C].values()) == 1,
          "a non-exe frame breaks the caller chain")
    check(ranked(rep)[:4] == [C, A, B, G], "ranking: excl, then incl, then RVA")
    check(rep.site_at[0x1502] == 4, "frame sites counted per RVA")
    check("build 332841" in buf.getvalue() and "matches" in buf.getvalue(),
          "annotated output names the build")

    bad = Image(io.BytesIO(_synthetic_pe(timestamp=PINNED_TIMESTAMP + 1)))
    buf = io.StringIO()
    rc, rep = analyse(bad, SELF_TEST_CSV, out=buf)
    check(rc == REFUSED and rep is None, "build mismatch refuses to annotate")
    check("refusing to annotate" in buf.getvalue(), "refusal says why")
    check(pe_info(bad, out=io.StringIO()) == REFUSED, "pe-info reports the mismatch")
    check(pe_info(good, out=io.StringIO()) == 0, "pe-info passes on the pinned build")
    buf = io.StringIO()
    rc, forced = analyse(bad, SELF_TEST_CSV, force=True, out=buf)
    check(rc == 0 and forced is not None and forced.excl[C] == 2,
          "--force annotates a mismatched build")
    check("WARNING" in buf.getvalue(), "--force prints a warning")

    buf = io.StringIO()
    rc, _ = analyse(good, SELF_TEST_CSV, disasm_rva=0x1500, capstone_mod=None, out=buf)
    check(rc == 0 and "capstone is not installed" in buf.getvalue(),
          "without capstone the disassembly is skipped, not an error")
    cs = import_capstone()
    if cs is None:
        print("elite_hotspots self-test: capstone not installed, disassembly check skipped")
    else:
        buf = io.StringIO()
        rc, _ = analyse(good, SELF_TEST_CSV, disasm_rva=0x1500, capstone_mod=cs, out=buf)
        text = buf.getvalue()
        check(rc == 0, "disassembly with capstone succeeds")
        check(any(line.lstrip().startswith("0x00001500") and "excl=1" in line
                  for line in text.splitlines()),
              "the call at 0x1500 is annotated with its leaf samples")
        check("-> 0x00001200 (fn 0x00001200-0x00001300)" in text,
              "the call target is resolved to the primary function")
        check("call sites 1" in text, "call targets are listed")
        check("no instruction boundary" not in text,
              "no boundary note when every leaf sits on an instruction")
        buf = io.StringIO()
        analyse(good, SELF_TEST_CSV + ["EliteDangerous64.exe,0x1501"],
                disasm_rva=0x1500, capstone_mod=cs, out=buf)
        check("no instruction boundary in this function: 1" in buf.getvalue(),
              "a leaf sample mid-instruction is reported")

    if failures:
        for what in failures:
            print("self-test FAILED: " + what)
        return 1
    print("elite_hotspots self-test OK")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", default=DEFAULT_EXE,
                    help="path to EliteDangerous64.exe (opened read-only)")
    ap.add_argument("--pe-info", action="store_true",
                    help="print the PE timestamp and image size against build %d, "
                         "and the .pdata function count" % BUILD)
    ap.add_argument("--samples", metavar="CSV",
                    help="the sample CSV (contract in the module docstring)")
    ap.add_argument("--top", type=int, default=30, help="functions to list (default 30)")
    ap.add_argument("--callers", type=int, default=3,
                    help="callers listed per function (default 3)")
    ap.add_argument("--disasm", metavar="0xRVA",
                    help="disassemble the function containing this RVA, annotated")
    ap.add_argument("--force", action="store_true",
                    help="annotate even though the exe is not build %d" % BUILD)
    ap.add_argument("--self-test", action="store_true",
                    help="run the synthetic-PE self-test (no game files needed)")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not (args.pe_info or args.samples or args.disasm):
        ap.error("give --pe-info, --samples CSV, or --disasm 0xRVA")
    try:
        disasm_rva = parse_rva(args.disasm) if args.disasm else None
    except ValueError:
        ap.error("--disasm needs a hex RVA such as 0x1234")
    try:
        with open(args.exe, "rb") as fh:
            img = Image(fh)
            if not (args.samples or args.disasm):
                return pe_info(img)
            lines = []
            if args.samples:
                with open(args.samples, newline="", encoding="utf-8", errors="replace") as f:
                    lines = list(f)
            rc, _ = analyse(img, lines, top=args.top, callers=args.callers,
                            force=args.force, disasm_rva=disasm_rva,
                            capstone_mod=import_capstone())
            return rc
    except (OSError, PEError, ValueError) as exc:
        print("elite_hotspots: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
