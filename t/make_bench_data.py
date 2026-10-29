#!/usr/bin/env python3
"""Builds t/data/kitten-benchmark.bin.gz, the input of t/bench_replay.c. Only needed to regenerate it:
the committed file is all the benchmark uses, kitten is not needed to run it.

    t/make_bench_data.py --kitten /path/to/kitten OUT.gz      records kitten's benchmark, then trims it
    t/make_bench_data.py --from RECORDING OUT.gz              trims a recording made earlier

kitten's tests send a few MB each, most of it a block repeated over and over (the Unicode text, the long
escape codes, the images are all repeats; ASCII and CSI are random). The file keeps, for each test, the
first part of one run: enough to give the library the same mix, cut at a boundary between escape
sequences / characters so nothing is split. bench_replay then replays each test's bytes in a loop.
The recording runs kitten on a 150x40 pseudo-terminal and answers its CSI 5 n / CSI 6 n queries here.
"""
import fcntl, gzip, os, pty, re, select, struct, sys, termios

# bytes kept per test (the unique multi-codepoint test is structured, so it keeps more)
KEEP = {
    b"Only ASCII chars": 512 << 10,
    b"Unicode chars": 512 << 10,
    b"Unique multi-codepoint Unicode cells": 1 << 20,
    b"CSI codes with few chars": 512 << 10,
    b"Long escape codes": 256 << 10,
    b"Images": 256 << 10,
}


def record(kitten):
    pid, master = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 150, 0, 0))
        os.environ["TERM"] = "xterm-256color"
        os.execvp(kitten, [kitten, "__benchmark", "--repetitions", "2"])
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 150, 0, 0))
    chunks, tail = [], b""
    while True:
        try:
            ready, _, _ = select.select([master], [], [], 120)
            if not ready:
                sys.exit("kitten produced nothing for 120 s")
            data = os.read(master, 1 << 20)
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
        window = tail + data
        for _ in range(window.count(b"\x1b[5n")):
            os.write(master, b"\x1b[0n")
        for _ in range(window.count(b"\x1b[6n")):
            os.write(master, b"\x1b[40;1R")
        tail = window[-3:]
    os.waitpid(pid, 0)
    return b"".join(chunks)


def token_end(d, i):
    """End of the escape sequence / UTF-8 character / byte that starts at d[i]."""
    n = len(d)
    b = d[i]
    if b == 0x1B and i + 1 < n:
        c = d[i + 1]
        if c == 0x5B:  # CSI: parameters, intermediates, final byte
            j = i + 2
            while j < n and 0x30 <= d[j] <= 0x3F:
                j += 1
            while j < n and 0x20 <= d[j] <= 0x2F:
                j += 1
            return min(j + 1, n)
        if c in b"]_P^X":  # a string, ended by ST (and for OSC also by BEL)
            j = i + 2
            while j < n:
                if d[j] == 0x1B and j + 1 < n and d[j + 1] == 0x5C:
                    return j + 2
                if d[j] == 0x07 and c == 0x5D:
                    return j + 1
                j += 1
            return n
        return min(i + 2, n)
    if b >= 0xF0:
        return min(i + 4, n)
    if b >= 0xE0:
        return min(i + 3, n)
    if b >= 0xC0:
        return min(i + 2, n)
    return i + 1


def trim(rec):
    marks = [(m.start(), m.group(1)) for m in re.finditer(rb"Running: ([^\r\n\x1b]+)", rec)]
    runs = {}
    for k, (pos, name) in enumerate(marks):
        end = marks[k + 1][0] if k + 1 < len(marks) else len(rec)
        runs.setdefault(name, []).append(rec[pos:end])
    out = []
    for name, keep in KEEP.items():
        if name not in runs:
            sys.exit(f"the recording has no test called {name.decode()!r}")
        run = max(runs[name], key=len)
        # header: "Running: ..." and everything up to the start of the data (the sync-output begin)
        begin = run.index(b"\x1b[?2026h") + 8
        i, limit = begin, min(len(run) - 3000, begin + keep)  # the last 3000 bytes are the "waiting" tail
        while i < limit:
            i = token_end(run, i)
        out.append(run[:begin] + run[begin:i] + b"\x1b[?2026l")
    return b"".join(out)


def main():
    args = sys.argv[1:]
    if len(args) != 3 or args[0] not in ("--kitten", "--from"):
        sys.exit(__doc__)
    rec = record(args[1]) if args[0] == "--kitten" else open(args[1], "rb").read()
    data = trim(rec)
    # no name and no time in the gzip header: the same data gives the same file, wherever it is written
    with open(args[2], "wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(data)
    print(f"{len(data)} bytes of data -> {os.path.getsize(args[2])} bytes in {args[2]}")


main()
