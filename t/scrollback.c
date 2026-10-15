/* Checks for the library-managed scrollback store (vterm_screen_enable_scrollback).
 *   cc -O1 -g -fsanitize=address,undefined -std=gnu99 -Iinclude -Isrc t/scrollback.c src/*.c -o sbtest && ./sbtest */
#include "vterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while(0)

static void feed(VTerm *vt, const char *s){ vterm_input_write(vt, s, strlen(s)); }

int main(void)
{
  VTerm *vt = vterm_new(3, 10);
  vterm_set_utf8(vt, 1);
  VTermScreen *sc = vterm_obtain_screen(vt);
  vterm_screen_enable_scrollback(sc, 3, 1 << 20);
  vterm_screen_reset(sc, 1);

  /* plain text, a coloured word, a wide character and a combining mark */
  feed(vt, "one\r\n");
  feed(vt, "\x1b[1;31mred\x1b[0m ok\r\n");
  feed(vt, "\xe4\xb8\x80x\xc3\xa9\xcc\x81\r\n");
  feed(vt, "last\r\n");
  feed(vt, "five\r\nsix\r\n");

  /* three rows on screen, six lines written: four scrolled off, the oldest dropped (limit 3) */
  CHECK(vterm_screen_scrollback_count(sc) == 3);

  VTermScreenCell cells[16];
  bool cont;
  int n = vterm_screen_scrollback_get(sc, 0, cells, 16, &cont);
  CHECK(n == 6 && cells[0].chars[0] == 'r' && cells[0].attrs.bold && !cells[4].attrs.bold); /* trailing blanks are dropped */
  CHECK(cells[2].chars[0] == 'd' && cells[3].chars[0] == ' ' && cells[5].chars[0] == 'k');
  CHECK(!VTERM_COLOR_IS_DEFAULT_FG(&cells[0].fg) && VTERM_COLOR_IS_DEFAULT_FG(&cells[4].fg));

  n = vterm_screen_scrollback_get(sc, 1, cells, 16, &cont);
  CHECK(n == 4);
  CHECK(cells[0].chars[0] == 0x4e00 && cells[0].width == 2 && cells[1].chars[0] == (uint32_t)-1);
  CHECK(cells[2].chars[0] == 'x');
  CHECK(cells[3].chars[0] == 0xe9 && cells[3].chars[1] == 0x301 && cells[3].chars[2] == 0);

  n = vterm_screen_scrollback_get(sc, 2, cells, 2, &cont);
  CHECK(n == 2 && cells[0].chars[0] == 'l'); /* cut to maxcols */
  CHECK(vterm_screen_scrollback_get(sc, 3, cells, 16, &cont) == 0); /* out of range */

  /* growing the window takes rows back out of the store */
  vterm_set_size(vt, 5, 10);
  CHECK(vterm_screen_scrollback_count(sc) == 1);
  VTermScreenCell c;
  vterm_screen_get_cell(sc, (VTermPos){0, 0}, &c);
  CHECK(c.chars[0] == 0x4e00 && c.width == 2);
  vterm_screen_get_cell(sc, (VTermPos){1, 0}, &c);
  CHECK(c.chars[0] == 'l');

  uint64_t pushed, popped;
  vterm_screen_scrollback_counters(sc, &pushed, &popped);
  CHECK(pushed == 4 && popped == 2);

  feed(vt, "\x1b[3J");
  CHECK(vterm_screen_scrollback_count(sc) == 0);

  /* a tiny byte budget keeps at least nothing, never overflows */
  vterm_screen_enable_scrollback(sc, 100, 16);
  for(int i = 0; i < 50; i++) feed(vt, "a line that is longer than the budget\r\n");
  CHECK(vterm_screen_scrollback_bytes(sc) <= 16);

  /* turning it off empties it */
  vterm_screen_enable_scrollback(sc, 0, 0);
  CHECK(vterm_screen_scrollback_count(sc) == 0);

  vterm_free(vt);

  /* a resize while the alternate screen is up pushes the primary screen's rows, not the alternate's */
  vt = vterm_new(4, 10);
  vterm_set_utf8(vt, 1);
  sc = vterm_obtain_screen(vt);
  vterm_screen_enable_altscreen(sc, 1);
  vterm_screen_enable_scrollback(sc, 100, 1 << 20);
  vterm_screen_reset(sc, 1);
  feed(vt, "p1\r\np2\r\np3\r\np4");
  feed(vt, "\x1b[?1049hALT1\r\nALT2\r\nALT3\r\nALT4");
  vterm_set_size(vt, 2, 10);
  n = 0;
  for(size_t i = 0; i < vterm_screen_scrollback_count(sc); i++) {
    vterm_screen_scrollback_get(sc, i, cells, 16, &cont);
    CHECK(cells[0].chars[0] == 'p');
    n++;
  }
  CHECK(n == 2);
  vterm_free(vt);

  if(failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("scrollback store: ok\n");
  return 0;
}
