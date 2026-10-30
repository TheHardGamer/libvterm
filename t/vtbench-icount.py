#!/usr/bin/env python3
"""Instructions each build of t/vtbench needs for every workload, counted by valgrind (cachegrind).

    t/vtbench-icount.py [--mib 2] label=path/to/vtbench [label=path ...]

Counted instructions do not depend on the state of the machine (frequency, other programs, WSL), so a
1% change shows up as 1%, where a timed run moves by 5-10% on its own. The cost of making the input
is counted separately and taken off. The first build is the reference for the percentages. Usually
run through t/vtbench-compare.sh, which builds the builds from git revisions."""
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

WORKLOADS = ["ascii_lines", "ascii_long_lines", "unicode_mixed", "sgr_colors", "csi_cursor", "scroll_region",
             "fullscreen_redraw", "osc_and_marks", "osc_without_marks", "osc133_marks_only", "long_escapes"]

def instructions(binary, name, mib, gen_only):
    env = dict(os.environ)
    if gen_only:
        env["VTBENCH_GENONLY"] = "1"
    out = subprocess.run(["valgrind", "--tool=cachegrind", "--cache-sim=no", "--cachegrind-out-file=/dev/null",
                          binary, "--once", name, mib], capture_output=True, text=True, env=env).stderr
    m = re.search(r"I\s+refs:\s+([\d,]+)", out)
    if not m:
        sys.exit(f"valgrind gave no instruction count for {binary} {name}:\n{out}")
    return int(m.group(1).replace(",", ""))

def main():
    args = sys.argv[1:]
    mib = "2"
    if args[:1] == ["--mib"]:
        mib, args = args[1], args[2:]
    builds = [a.split("=", 1) for a in args]
    if not builds or any(len(b) != 2 for b in builds):
        sys.exit(__doc__)
    jobs = [(label, path, w, g) for w in WORKLOADS for label, path in builds for g in (False, True)]
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex:  # counts do not depend on load, so run them side by side
        counts = list(ex.map(lambda j: instructions(j[1], j[2], mib, j[3]), jobs))
    table = {}
    for (label, _, w, g), c in zip(jobs, counts):
        table.setdefault((w, label), [0, 0])[1 if g else 0] = c
    labels = [b[0] for b in builds]
    print(f"millions of instructions to parse {mib} MiB" + " " * 3 + "".join(f"{l:>14}" for l in labels)
          + "".join(f"  {l} vs {labels[0]}" for l in labels[1:]))
    for w in WORKLOADS:
        v = {l: table[(w, l)][0] - table[(w, l)][1] for l in labels}
        row = f"{w:40}" + "".join(f"{v[l] / 1e6:14.1f}" for l in labels)
        for l in labels[1:]:
            row += f"  {100 * (v[l] - v[labels[0]]) / v[labels[0]]:+10.2f}%    "
        print(row)

if __name__ == "__main__":
    main()
