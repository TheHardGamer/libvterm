/* Correctness checks for reflow, including its interaction with the library scrollback.
 *   cc -O1 -g -fsanitize=address,undefined -std=gnu99 -Iinclude -Isrc t/reflow.c src/<all>.c -o reflowtest && ./reflowtest
 *
 * dump() joins scrollback and screen rows back into logical lines (a row with the
 * continuation flag belongs to the line above it), so a test can compare the text
 * without caring where the wraps fall. */
#include "vterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while(0)
#define CHECK_DUMP(vt, want) do { char b[4096]; dump(vt, b, sizeof b); \
    if(strcmp(b, want)) { printf("FAIL line %d:\n  want %s\n  got  %s\n", __LINE__, want, b); failures++; } } while(0)

static void feed(VTerm *vt, const char *s){ vterm_input_write(vt, s, strlen(s)); }

static char *put(char *p, char *end, const VTermScreenCell *c, int n, bool cont)
{
  (void)cont;
  while(n > 0 && c[n - 1].chars[0] == 0) /* never written: the padding left where a wide character moved on */
    n--;
  for(int i = 0; i < n && p < end - 2; i += c[i].width ? c[i].width : 1) {
    uint32_t ch = c[i].chars[0];
    *p++ = ch == 0 ? ' ' : ch == (uint32_t)-1 ? '?' : ch < 128 ? (char)ch : '#';
  }
  return p;
}

/* logical lines, '|' between them, trailing blank rows and trailing spaces of a line dropped */
static void dump(VTerm *vt, char *out, size_t cap)
{
  VTermScreen *sc = vterm_obtain_screen(vt);
  VTermState *st = vterm_obtain_state(vt);
  int rows, cols;
  vterm_get_size(vt, &rows, &cols);
  char *p = out, *end = out + cap;
  char line[2048]; char *lp = line;
  bool have = false;
  VTermScreenCell cells[512];

  size_t nsb = vterm_screen_scrollback_count(sc);
  for(size_t i = 0; i < nsb + rows; i++) {
    bool cont = false; int n;
    if(i < nsb) {
      n = vterm_screen_scrollback_get(sc, i, cells, 512, &cont);
    }
    else {
      int r = (int)(i - nsb);
      for(int c = 0; c < cols; c++) vterm_screen_get_cell(sc, (VTermPos){r, c}, &cells[c]);
      n = cols;
      cont = vterm_state_get_lineinfo(st, r)->continuation;
    }
    if(!cont && have) {
      while(lp > line && lp[-1] == ' ') lp--;
      *lp = 0;
      p += snprintf(p, end - p, "%s|", line);
      lp = line;
    }
    have = true;
    lp = put(lp, line + sizeof line, cells, n, cont);
  }
  while(lp > line && lp[-1] == ' ') lp--;
  *lp = 0;
  p += snprintf(p, end - p, "%s", line);
  /* drop trailing empty lines */
  while(p > out && p[-1] == '|') *--p = 0;
}

static VTerm *mk(int rows, int cols, bool reflow, size_t sbmax)
{
  VTerm *vt = vterm_new(rows, cols);
  vterm_set_utf8(vt, 1);
  VTermScreen *sc = vterm_obtain_screen(vt);
  if(sbmax) vterm_screen_enable_scrollback(sc, sbmax, 1 << 20);
  vterm_screen_enable_reflow(sc, reflow);
  vterm_screen_reset(sc, 1);
  return vt;
}

static void cursor(VTerm *vt, int *r, int *c)
{
  VTermPos p; vterm_state_get_cursorpos(vterm_obtain_state(vt), &p); *r = p.row; *c = p.col;
}


/* Property test: whatever the sequence of resizes, the logical lines (text, with the wraps taken
 * out) stay exactly what was written, and the cursor stays after the last character. */
static uint64_t rng = 88172645463325252ULL;
static unsigned rnd(unsigned n){ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng % n); }

static void property_tests(int cases)
{
  for(int k = 0; k < cases; k++) {
    int rows = 2 + rnd(9), cols = 3 + rnd(40);
    bool wide_ok = rnd(2);
    VTerm *vt = mk(rows, cols, true, 100000);
    char model[4096]; char *m = model;
    char in[8192]; char *ip = in;
    int nlines = 1 + rnd(12);
    for(int l = 0; l < nlines; l++) {
      int len = rnd(5) == 0 ? 0 : 1 + rnd(90);
      for(int i = 0; i < len; i++) {
        unsigned r = rnd(10);
        if(wide_ok && r == 0) { memcpy(ip, "\xe4\xb8\x80", 3); ip += 3; *m++ = '#'; }
        else if(r == 1) { *ip++ = ' '; *m++ = ' '; }
        else { char ch = 'a' + rnd(26); *ip++ = ch; *m++ = ch; }
      }
      if(l + 1 < nlines) { *ip++ = '\r'; *ip++ = '\n'; *m++ = '|'; }
    }
    *ip = 0; *m = 0;
    /* what dump() gives for it: spaces at the end of a line and empty lines at the end go */
    char want[4096]; char *w = want; char *ls = model;
    for(char *q = model; ; q++) {
      if(*q == '|' || *q == 0) {
        char *e = q; while(e > ls && e[-1] == ' ') e--;
        memcpy(w, ls, e - ls); w += e - ls;
        if(*q == '|') *w++ = '|';
        ls = q + 1;
        if(*q == 0) break;
      }
    }
    *w = 0;
    while(w > want && w[-1] == '|') *--w = 0;

    feed(vt, in);
    char got[4096]; dump(vt, got, sizeof got);
    if(strcmp(got, want)) { printf("FAIL property %d: not as written at %dx%d\n  want %s\n  got  %s\n", k, rows, cols, want, got); failures++; vterm_free(vt); continue; }
    for(int step = 0; step < 8; step++) {
      int nr = 2 + rnd(9), nc = 3 + rnd(40);
      vterm_set_size(vt, nr, nc);
      dump(vt, got, sizeof got);
      if(strcmp(got, want)) {
        printf("FAIL property %d step %d: %dx%d -> %dx%d (started %dx%d, wide %d)\n  want %s\n  got  %s\n",
            k, step, rows, cols, nr, nc, rows, cols, wide_ok, want, got);
        failures++;
        break;
      }
      rows = nr; cols = nc;
    }
    vterm_free(vt);
    if(failures > 5) break;
  }
}

int main(void)
{
  char A100[101]; memset(A100, 'A', 100); A100[100] = 0;
  char want[256];

  /* shrink then grow, all on screen */
  { VTerm *vt = mk(5, 80, true, 100);
    feed(vt, A100);
    vterm_set_size(vt, 5, 40); CHECK_DUMP(vt, A100);
    vterm_set_size(vt, 5, 80); CHECK_DUMP(vt, A100);
    vterm_free(vt); }

  /* shrink so far that the line spills into scrollback, then grow back */
  { VTerm *vt = mk(3, 80, true, 100);
    feed(vt, A100); feed(vt, "\r\nsecond\r\nthird");
    snprintf(want, sizeof want, "%s|second|third", A100);
    CHECK_DUMP(vt, want);
    vterm_set_size(vt, 3, 10); CHECK_DUMP(vt, want);
    vterm_set_size(vt, 3, 80); CHECK_DUMP(vt, want);
    vterm_set_size(vt, 3, 7);  CHECK_DUMP(vt, want);
    vterm_set_size(vt, 6, 33); CHECK_DUMP(vt, want);
    vterm_free(vt); }

  /* hard newlines never merge */
  { VTerm *vt = mk(5, 20, true, 100);
    feed(vt, "aaaa\r\nbbbb\r\ncccc");
    vterm_set_size(vt, 5, 80); CHECK_DUMP(vt, "aaaa|bbbb|cccc");
    vterm_set_size(vt, 5, 3);  CHECK_DUMP(vt, "aaaa|bbbb|cccc");
    vterm_free(vt); }

  /* spaces at a wrap boundary survive */
  { VTerm *vt = mk(5, 80, true, 100);
    feed(vt, "aaaa bbbb cccc");
    vterm_set_size(vt, 5, 5); CHECK_DUMP(vt, "aaaa bbbb cccc");
    vterm_set_size(vt, 5, 80); CHECK_DUMP(vt, "aaaa bbbb cccc");
    vterm_free(vt); }

  /* the same with the line already pushed into scrollback while narrow */
  { VTerm *vt = mk(2, 5, true, 100);
    feed(vt, "aaaa bbbb cccc\r\nz");
    CHECK_DUMP(vt, "aaaa bbbb cccc|z");
    vterm_set_size(vt, 2, 80); CHECK_DUMP(vt, "aaaa bbbb cccc|z");
    vterm_free(vt); }

  /* cursor stays on the same character through shrink and grow */
  { VTerm *vt = mk(5, 20, true, 100);
    feed(vt, "0123456789ABCDEFGHIJ0123456789");  /* 30 chars on 20 cols: cursor at row 1 col 10 */
    int r, c; cursor(vt, &r, &c); CHECK(r == 1 && c == 10);
    vterm_set_size(vt, 5, 10); cursor(vt, &r, &c); CHECK((r == 3 && c == 0) || (r == 2 && c == 9));
    vterm_set_size(vt, 5, 30); cursor(vt, &r, &c); CHECK(r == 0 && c == 29);
    vterm_free(vt); }

  /* alt screen is not reflowed and the primary comes back intact */
  { VTerm *vt = mk(4, 20, true, 100);
    vterm_screen_enable_altscreen(vterm_obtain_screen(vt), 1);
    feed(vt, "primary line that is long");
    feed(vt, "\x1b[?1049h" "ALTSCREEN-CONTENT-IS-LONG");
    vterm_set_size(vt, 4, 10);
    feed(vt, "\x1b[?1049l");
    CHECK_DUMP(vt, "primary line that is long");
    vterm_free(vt); }

  /* wide characters at the wrap boundary */
  { VTerm *vt = mk(4, 6, true, 100);
    feed(vt, "ab\xe4\xb8\x80\xe4\xb8\x80\xe4\xb8\x80xy");
    char b0[256]; dump(vt, b0, sizeof b0);
    vterm_set_size(vt, 4, 5); CHECK_DUMP(vt, b0);
    vterm_set_size(vt, 4, 3); CHECK_DUMP(vt, b0);
    vterm_set_size(vt, 4, 6); CHECK_DUMP(vt, b0);
    vterm_free(vt); }

  /* attributes travel with the text */
  { VTerm *vt = mk(4, 20, true, 100);
    feed(vt, "\x1b[1mBOLDBOLDBOLD\x1b[0m plain");
    vterm_set_size(vt, 4, 5);
    VTermScreenCell c;
    vterm_screen_get_cell(vterm_obtain_screen(vt), (VTermPos){1, 0}, &c);
    CHECK(c.chars[0] == 'O' || c.chars[0] == 'B');
    CHECK(c.attrs.bold);
    vterm_screen_get_cell(vterm_obtain_screen(vt), (VTermPos){2, 4}, &c);
    CHECK(!c.attrs.bold);
    vterm_free(vt); }

  property_tests(3000);

  printf(failures ? "%d FAILED\n" : "all ok\n", failures);
  return failures != 0;
}
