# libvterm fork

A fork of libvterm from neovim with some updates and fixes

### Features on top of vterm

- Faster (profiled using manual code, 'perf' tool)
- Memory leaks detected by valgrind, ubsan and tests have been fixed
- Scrollback kept by the library (`vterm_screen_enable_scrollback()`): scrolled-off rows are stored
  compactly (about a byte per plain ASCII cell, the pen once per run of equal pens), read back with
  `vterm_screen_scrollback_get()`, and pulled back in by resizing. Off by default; the `sb_pushline`
  callbacks still work.
- Cells hold an index into a table of interned pens instead of a copy of the pen, so writing, erasing
  and scrolling move far less memory.
- Wide (CJK) characters are written in batches like narrow ones.
- `ESC [ 3 J` ends a pending wrap like any other CSI sequence, whether or not `sb_clear` is set.
- (SGR 58 / 59), in both the semicolon form (`58;2;r;g;b`,`58;5;n`), exposed as `VTERM_ATTR_UNDERLINE_COLOR` and `VTermScreenCell.uline`.
- OSC 8 hyperlinks are stored as an id in each cell (`VTermScreenCell.link`).
  `vterm_screen_get_hyperlink()` / `vterm_state_get_hyperlink()` turn an id back into
  its URI. The most recent 4096 distinct links are kept. Over-long or malformed links
  are ignored.
-  (DECSET/DECRST 2026) is reported as `VTERM_PROP_SYNCOUTPUT` and answered to DECRQM queries (`CSI ? 2026 $ p`).
- A combining mark after a batched run of characters no longer overwrites the next
  cell (e.g. `e` followed by U+0301).
- `REP` (CSI b) after a zero-width character no longer hangs.
- Resizing while the alternate screen is up no longer pushes the alternate screen's rows into the scrollback.
- Control characters (width -1) in text no longer move the cursor backwards.
- Restoring a saved cursor (`ESC 8`) after the screen shrank no longer writes off the
  end of the screen.
- A scroll region left empty or inverted by a resize falls back to the whole screen.
- A wide character on a one-column screen no longer writes outside the row.
- Very long numbers in CSI and OSC sequences saturate instead of overflowing.
- The selection buffer libvterm allocates itself is freed with the state.

## Performance

`t/vtbench.c` is the benchmark suite. It uses only the public API, so one file builds against any revision of
the library, and it makes its input itself from a fixed seed: nothing to download. The terminal is set up the way
the application sets it up (150x40, library scrollback, reflow on, damage callbacks), with no rendering and no pty.

| Workload            | What is in it                                                             |
|---------------------|---------------------------------------------------------------------------|
| `ascii_lines`       | lines of 40-80 characters, scrolling into the scrollback                  |
| `ascii_long_lines`  | lines of 250-400 characters, wrapping on a 150 column screen              |
| `unicode_mixed`     | ASCII with wide (CJK), combining, Cyrillic and emoji characters           |
| `sgr_colors`        | text with a colour or attribute change every few words                    |
| `csi_cursor`        | cursor addressing, erase, insert and delete characters                    |
| `scroll_region`     | `DECSTBM`, insert/delete lines, scroll up/down (an editor scrolling)      |
| `fullscreen_redraw` | the alternate screen redrawn over and over in colours (a TUI)             |
| `osc_and_marks`     | titles, OSC 8 links, OSC 7, and OSC 133 marks around short lines          |
| `osc_without_marks` / `osc133_marks_only` | the two halves of the one above                     |
| `long_escapes`      | very long OSC, DCS and APC strings                                        |

and three operations, in milliseconds: `resize_drag` (a resize step with 100000 rows of history),
`resize_height`, and `rewrap_*` (cutting the history to a narrower screen).

**Timed run**, to see the speed in MiB/s: `make bench`, or build it yourself and pin it to one core
(the header of `t/vtbench.c` has the options):

    cc -O3 -flto=auto -DNDEBUG -std=gnu99 -Iinclude -Isrc t/vtbench.c src/[a-z]*.c -o t/vtbench
    taskset -c 3 ./t/vtbench 1.0 5          # seconds per trial, trials; add a name to run only that one

Typical numbers, i9-13950HX under WSL2 on one pinned core (about 5% from run to run, 10% on the OSC ones):

| Workload            | MiB/s |
|---------------------|-------|
| `ascii_lines`       | 160   |
| `ascii_long_lines`  | 240   |
| `unicode_mixed`     | 90    |
| `sgr_colors`        | 125   |
| `csi_cursor`        | 175   |
| `scroll_region`     | 310   |
| `fullscreen_redraw` | 260   |
| `osc_and_marks`     | 70    |
| `long_escapes`      | 2750  |

**Comparing two builds**: `t/vtbench-compare.sh` builds the suite against git revisions, counts instructions under valgrind and
prints a table with the first revision as the reference. `WORKTREE` is the files as they are now:

    t/vtbench-compare.sh HEAD WORKTREE
    t/vtbench-compare.sh 333a755 HEAD WORKTREE
    t/vtbench-compare.sh --mib 4 v1.0 WORKTREE

## Reflow

With `vterm_screen_enable_reflow()` a resize re-wraps the lines of the primary screen. With the library
scrollback it also pulls the scrollback lines the top of the screen continues (and enough older ones to
fill a taller screen) into the resize and hands back what does not fit, so a line is never cut
between scrollback and screen, and growing the window rejoins what shrinking wrapped. A double-width
character is never split at the edge. The alternate screen is not reflowed. The work happens only on a
resize, at a cost of the screen plus the lines pulled, whatever the size of the history (about 75 us
for 40x150 with 100000 rows of history); reflow off keeps the old behaviour and the same throughput.

History rows keep the width they scrolled off with, so after the window gets narrower the part of a row past the
right edge is out of sight. `vterm_screen_rewrap_scrollback()` cuts the rows that are too wide into rows of the
screen's width (text, wrap flags and OSC 133 marks kept). It reads the whole history, so call it once resizing
has stopped, not on every step; it is a single check when no row is too wide, and returns how many rows it cut. With 100000 rows of history it takes
about 30 ms when a quarter of the rows are too wide and 290 ms when all of them are.

## OSC 133 marks

`ESC ] 133 ; A|C|D` (prompt start, output start, command done) sets a `VTERM_LINEMARK_*` bit on the row
the cursor is on, in `VTermLineInfo.marks`. The marks stay with the text: they are stored in the spare
bits of the per-row info and of the scrollback record (no extra memory, no extra work while scrolling),
and a reflow puts the marks of a wrapped line on its first row. A row fully erased loses its marks and
the alternate screen takes none. Read them with `vterm_state_get_lineinfo()` for the screen and
`vterm_screen_scrollback_marks()` for the scrollback.

## Testing

The test suite is in `t/`. Each `t/*.test` file is a script of input, and the expected
screen, cursor, pen and output state, run by `t/harness.c`.

```sh
make test
```

Tests are checked with AddressSanitizer and UBSan as well:

```sh
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -std=gnu99 -Iinclude -Isrc t/harness.c src/*.c -o /tmp/harness
ASAN_OPTIONS=detect_leaks=1 perl t/run-test.pl t/10state_putglyph.test -e /tmp/harness
```

`t/scrollback.c` checks the library-managed scrollback (`make test` runs it too).

`t/reflow.c` checks that reflow keeps the text: it joins scrollback and screen rows back into logical
lines and compares them after shrinking and growing (wide characters, the alternate screen, lines that
spilled into the scrollback), then runs random text through random resize sequences and requires the
logical lines to stay exactly what was written.

`t/compare.c` runs the same random input through two builds, or through the callback and the
library way of keeping scrollback, and prints everything observable (every cell, the cursor, every
stored row) so the outputs can be diffed. Against a build of an older commit, and between the two
scrollback modes, it must show no difference; see the header of the file.

`t/fuzz.c` is a random stress harness. It feeds biased garbage (escape sequences,
UTF-8, combining marks, wide characters) with random resizes and scrollback callbacks,
under sanitizers:

```sh
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -std=gnu99 -Iinclude -Isrc t/fuzz.c src/*.c -o fuzz
FUZZ_REFLOW=0 ./fuzz 60 1     # random seeds for 60 seconds, starting at seed 1
FUZZ_REFLOW=0 ./fuzz 1 4341   # replay one seed
```

Environment variables for the fuzzer: , and `FUZZ_CHUNK1=1` feeds input one byte at a time.

Reflow (`FUZZ_REFLOW=1`) is fuzzed too: its out-of-range read in `resize_buffer()` (a top row that continues a line already scrolled off) and an `abort()` when the cursor got lost are fixed.
