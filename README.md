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

kitten benchmark on library only, no rendering or pty, 150x40 screen, one pinned core of an i9-13950HX:
All the values follow MB/s unit
| Category                             | Alternate screen | Primary screen, library scrollback |
|--------------------------------------|------------------|------------------------------------|
| ASCII                                | 173              | 110                                |
| Unicode                              | 322              | 226                                |
| Unique multi-codepoint Unicode cells | 199              | 170                                |
| CSI codes with few chars             | 169              | 165                                |
| Long escape codes                    | 3200             | 3300                               |
| Images                               | 3300             | 3300                               |

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

Reflow (`FUZZ_REFLOW=1`) is fuzzed too now: its out-of-range read in `resize_buffer()` (a top row that continues a line already scrolled off) and an `abort()` when the cursor got lost are fixed.
