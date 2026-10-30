/* Benchmarks for libvterm, using only its public API so the same file builds against any revision of the
 * library. Every workload is generated in memory from a fixed seed, so the input is identical on every build.
 *
 * Timed (MiB/s of input, median and best of the trials; ms for the operations):
 *   cc -O3 -flto=auto -DNDEBUG -std=gnu99 -Iinclude -Isrc t/vtbench.c src/[a-z]*.c -o t/vtbench
 *   taskset -c 3 ./t/vtbench [seconds per trial = 1.0] [trials = 5] [name filter]      (or: make bench)
 * Timing on a laptop or under WSL moves by 5-10% from one run to the next. To compare two builds, count
 * instructions instead: they do not depend on the state of the machine (t/vtbench-compare.sh).
 *
 * One workload, fed once, nothing timed (for valgrind), or written to a file for other tools:
 *   ./t/vtbench --once <name> <MiB> [file]        VTBENCH_GENONLY=1 makes the input without parsing it
 *
 * Build with -DNO_REWRAP against a revision of the library that has no vterm_screen_rewrap_scrollback().
 *
 * The terminal is set up the way the application sets it up: 40x150, library scrollback, reflow on,
 * row-merged damage with a damage and a settermprop callback. */
#include "vterm.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROWS 40
#define COLS 150
#define SCROLLBACK 10000

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void){ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }
static uint32_t rn(uint32_t n){ return rnd() % n; }

/* ---- a growable byte buffer ---- */
typedef struct { uint8_t *p; size_t len, cap; } Buf;
static void put(Buf *b, const void *s, size_t n){
  if(b->len + n > b->cap){ b->cap = (b->cap ? b->cap * 2 : 1 << 20); while(b->cap < b->len + n) b->cap *= 2; b->p = realloc(b->p, b->cap); }
  memcpy(b->p + b->len, s, n); b->len += n;
}
static void puts_(Buf *b, const char *s){ put(b, s, strlen(s)); }
static void putf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void putf(Buf *b, const char *fmt, ...){ char t[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap); put(b, t, n); }

static const char *words[] = { "the","terminal","emulator","scroll","buffer","render","line","cursor","error","warning","build","target",
  "linking","compiling","src","core","index","value","string","token","parse","state","frame","glyph","atlas","pixel","width","height" };
#define NWORDS (sizeof words / sizeof words[0])

static void text(Buf *b, int len){ int n = 0; while(n < len){ const char *w = words[rn(NWORDS)]; size_t l = strlen(w); put(b, w, l); put(b, " ", 1); n += l + 1; } }

/* ---- workloads ---- */
static void gen_ascii(Buf *b, int minlen, int maxlen, size_t target){
  while(b->len < target){ text(b, minlen + rn(maxlen - minlen + 1)); puts_(b, "\r\n"); }
}
static void gen_unicode(Buf *b, size_t target){
  while(b->len < target){
    for(int i = 0; i < 12; i++){
      switch(rn(6)){
        case 0: { uint32_t c = 0x4E00 + rn(0x2000); char u[4]; u[0] = 0xE0 | (c >> 12); u[1] = 0x80 | ((c >> 6) & 63); u[2] = 0x80 | (c & 63); put(b, u, 3); break; } /* wide */
        case 1: puts_(b, "e\xcc\x81"); break;                                  /* combining accent */
        case 2: puts_(b, "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 "); break; /* Cyrillic */
        case 3: { char u[4] = { (char)0xF0, (char)0x9F, (char)(0x98 | rn(4)), (char)(0x80 | rn(64)) }; put(b, u, 4); break; } /* emoji */
        default: text(b, 6);
      }
    }
    puts_(b, "\r\n");
  }
}
static void gen_sgr(Buf *b, size_t target){
  while(b->len < target){
    int n = 4 + rn(12);
    for(int i = 0; i < n; i++){
      switch(rn(4)){
        case 0: putf(b, "\x1b[38;2;%u;%u;%um", rn(256), rn(256), rn(256)); break;
        case 1: putf(b, "\x1b[%um", 31 + rn(7)); break;
        case 2: putf(b, "\x1b[1;38;5;%um", rn(256)); break;
        default: puts_(b, "\x1b[0m");
      }
      { const char *w = words[rn(NWORDS)]; put(b, w, strlen(w)); }
      puts_(b, " ");
    }
    puts_(b, "\x1b[0m\r\n");
  }
}
static void gen_csi_cursor(Buf *b, size_t target){
  while(b->len < target){
    putf(b, "\x1b[%u;%uH", 1 + rn(ROWS), 1 + rn(COLS - 30));
    switch(rn(6)){
      case 0: puts_(b, "\x1b[K"); break;
      case 1: puts_(b, "\x1b[2K"); break;
      case 2: putf(b, "\x1b[%uC", 1 + rn(10)); break;
      case 3: putf(b, "\x1b[%uP", 1 + rn(5)); break;
      case 4: putf(b, "\x1b[%u@", 1 + rn(5)); break;
      default: putf(b, "\x1b[%uA", 1 + rn(5));
    }
    text(b, 4 + rn(20));
  }
}
static void gen_scroll_region(Buf *b, size_t target){
  while(b->len < target){
    putf(b, "\x1b[%u;%ur", 2 + rn(5), ROWS - 1 - rn(5));
    for(int i = 0; i < 40; i++){ text(b, 20 + rn(60)); puts_(b, "\r\n"); }
    putf(b, "\x1b[%uL", 1 + rn(3)); putf(b, "\x1b[%uM", 1 + rn(3));
    putf(b, "\x1b[%uS", 1 + rn(3)); putf(b, "\x1b[%uT", 1 + rn(3));
    puts_(b, "\x1b[r");
  }
}
static void gen_fullscreen(Buf *b, size_t target){
  puts_(b, "\x1b[?1049h");
  while(b->len < target){
    puts_(b, "\x1b[H");
    for(int r = 0; r < ROWS; r++){
      for(int c = 0; c < COLS; c += 10){ putf(b, "\x1b[38;5;%u;48;5;%um", rn(256), rn(256)); put(b, "0123456789", 10); }
      puts_(b, "\x1b[0m");
      if(r + 1 < ROWS) puts_(b, "\r\n");
    }
  }
  puts_(b, "\x1b[?1049l");
}
static void gen_osc(Buf *b, size_t target){
  unsigned n = 0;
  while(b->len < target){
    putf(b, "\x1b]0;title %u\x07", n);
    putf(b, "\x1b]8;;http://example.com/%u\x1b\\link %u\x1b]8;;\x1b\\ ", n, n);
    putf(b, "\x1b]7;file://host/home/user/dir%u\x07", n % 50);
    puts_(b, "\x1b]133;A\x07$ cmd\r\n\x1b]133;C\x07");
    text(b, 30); puts_(b, "\r\n\x1b]133;D;0\x07");
    n++;
  }
}
static void gen_osc_plain(Buf *b, size_t target){ /* the same without the OSC 133 marks */
  unsigned n = 0;
  while(b->len < target){
    putf(b, "\x1b]0;title %u\x07", n);
    putf(b, "\x1b]8;;http://example.com/%u\x1b\\link %u\x1b]8;;\x1b\\ ", n, n);
    putf(b, "\x1b]7;file://host/home/user/dir%u\x07", n % 50);
    puts_(b, "$ cmd\r\n"); text(b, 30); puts_(b, "\r\n");
    n++;
  }
}
static void gen_osc133(Buf *b, size_t target){ /* only the marks, around short lines */
  while(b->len < target){
    puts_(b, "\x1b]133;A\x07$ cmd\r\n\x1b]133;C\x07"); text(b, 30); puts_(b, "\r\n\x1b]133;D;0\x07");
  }
}
static void gen_long_escape(Buf *b, size_t target){
  while(b->len < target){
    puts_(b, "\x1b]0;"); for(int i = 0; i < 600; i++) put(b, "t", 1); puts_(b, "\x07");
    puts_(b, "\x1bP"); for(int i = 0; i < 800; i++) put(b, "q", 1); puts_(b, "\x1b\\");
    puts_(b, "\x1b_"); for(int i = 0; i < 400; i++) put(b, "g", 1); puts_(b, "\x1b\\");
  }
}

/* ---- the terminal, set up as the application does ---- */
static int on_damage(VTermRect r, void *u){ (void)r; (void)u; return 1; }
static int on_prop(VTermProp p, VTermValue *v, void *u){ (void)p; (void)v; (void)u; return 1; }
static VTermScreenCallbacks cbs = { .damage = on_damage, .settermprop = on_prop };

static VTerm *make(int rows, int cols, size_t scrollback){
  VTerm *vt = vterm_new(rows, cols);
  vterm_set_utf8(vt, 1);
  VTermScreen *sc = vterm_obtain_screen(vt);
  vterm_screen_set_callbacks(sc, &cbs, NULL);
  vterm_screen_set_damage_merge(sc, VTERM_DAMAGE_ROW);
  vterm_screen_enable_altscreen(sc, 1);
  vterm_screen_enable_reflow(sc, 1);
  vterm_screen_reset(sc, 1);
  vterm_screen_enable_scrollback(sc, scrollback, scrollback * COLS * 8);
  return vt;
}

static int cmp(const void *a, const void *b){ double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void throughput(const char *name, Buf *b, double secs, int trials, const char *filter){
  if(filter && !strstr(name, filter)) return;
  double rates[32];
  for(int t = 0; t < trials; t++){
    VTerm *vt = make(ROWS, COLS, SCROLLBACK);
    VTermScreen *sc = vterm_obtain_screen(vt);
    size_t total = 0; double t0 = now_s(), el;
    do {
      for(size_t off = 0; off < b->len; off += 65536){
        size_t n = b->len - off < 65536 ? b->len - off : 65536;
        vterm_input_write(vt, (const char *)b->p + off, n);
        vterm_screen_flush_damage(sc);
        total += n;
      }
      el = now_s() - t0;
    } while(el < secs);
    rates[t] = total / el / (1024.0 * 1024.0);
    vterm_free(vt);
  }
  qsort(rates, trials, sizeof rates[0], cmp);
  printf("%-28s %9.1f %9.1f   MiB/s (median, best)\n", name, rates[trials / 2], rates[trials - 1]);
  fflush(stdout);
}

/* ---- operations ---- */
static void fill_history(VTerm *vt, int rows_n, int len){
  Buf b = {0}; char line[512]; memset(line, 'x', sizeof line);
  for(int i = 0; i < rows_n; i++){ put(&b, line, len); puts_(&b, "\r\n"); if(b.len > 1 << 20){ vterm_input_write(vt, (char *)b.p, b.len); b.len = 0; } }
  vterm_input_write(vt, (char *)b.p, b.len); free(b.p);
}
static void ops(const char *filter){
  if(!filter || strstr("resize_drag", filter)){
    VTerm *vt = make(ROWS, 200, 100000); fill_history(vt, 100100, 190);
    double worst = 0, tot = 0; int steps = 0;
    for(int c = 199; c >= 100; c--, steps++){ double t0 = now_s(); vterm_set_size(vt, ROWS, c); double d = now_s() - t0; tot += d; if(d > worst) worst = d; }
    printf("%-28s avg %7.3f ms  worst %7.3f ms per step (100k history, 200->100 cols)\n", "resize_drag", tot / steps * 1e3, worst * 1e3);
    vterm_free(vt);
  }
  if(!filter || strstr("resize_height", filter)){
    VTerm *vt = make(ROWS, COLS, SCROLLBACK); fill_history(vt, 12000, 100);
    double t0 = now_s(); for(int i = 0; i < 100; i++){ vterm_set_size(vt, 20, COLS); vterm_set_size(vt, ROWS, COLS); }
    printf("%-28s %7.3f ms per grow+shrink\n", "resize_height", (now_s() - t0) / 100 * 1e3);
    vterm_free(vt);
  }
#ifndef NO_REWRAP
  if(!filter || strstr("rewrap", filter)){
    for(int k = 0; k < 2; k++){
      VTerm *vt = make(ROWS, 200, 100000); fill_history(vt, 100100, k ? 190 : 120);
      vterm_set_size(vt, ROWS, 100);
      double t0 = now_s(); int added = vterm_screen_rewrap_scrollback(vterm_obtain_screen(vt)); double d = now_s() - t0;
      printf("%-28s %7.2f ms (100k rows of %d cells -> 100 cols, %+d rows)\n", k ? "rewrap_all_long" : "rewrap_some_long", d * 1e3, k ? 190 : 120, added);
      vterm_free(vt);
    }
  }
#endif
}

static int once(const char *name, double mib, const char *dump){
  size_t target = (size_t)(mib * 1048576);
  Buf b = {0};
  if(!strcmp(name, "ascii_lines")) gen_ascii(&b, 40, 80, target);
  else if(!strcmp(name, "ascii_long_lines")) gen_ascii(&b, 250, 400, target);
  else if(!strcmp(name, "unicode_mixed")) gen_unicode(&b, target);
  else if(!strcmp(name, "sgr_colors")) gen_sgr(&b, target);
  else if(!strcmp(name, "csi_cursor")) gen_csi_cursor(&b, target);
  else if(!strcmp(name, "scroll_region")) gen_scroll_region(&b, target);
  else if(!strcmp(name, "fullscreen_redraw")) gen_fullscreen(&b, target);
  else if(!strcmp(name, "osc_and_marks")) gen_osc(&b, target);
  else if(!strcmp(name, "osc_without_marks")) gen_osc_plain(&b, target);
  else if(!strcmp(name, "osc133_marks_only")) gen_osc133(&b, target);
  else if(!strcmp(name, "long_escapes")) gen_long_escape(&b, target);
  else return 2;
  if(dump){ FILE *f = fopen(dump, "wb"); fwrite(b.p, 1, b.len, f); fclose(f); free(b.p); return 0; }
  if(getenv("VTBENCH_GENONLY")){ free(b.p); return 0; } /* what it costs to make the input, to subtract */
  VTerm *vt = make(ROWS, COLS, SCROLLBACK);
  VTermScreen *sc = vterm_obtain_screen(vt);
  for(size_t off = 0; off < b.len; off += 65536){
    size_t n = b.len - off < 65536 ? b.len - off : 65536;
    vterm_input_write(vt, (const char *)b.p + off, n);
    vterm_screen_flush_damage(sc);
  }
  vterm_free(vt); free(b.p);
  return 0;
}

int main(int argc, char **argv){
  if(argc > 3 && !strcmp(argv[1], "--once")) return once(argv[2], atof(argv[3]), argc > 4 ? argv[4] : NULL);
  double secs = argc > 1 ? atof(argv[1]) : 1.0;
  int trials = argc > 2 ? atoi(argv[2]) : 5; if(trials > 31) trials = 31;
  const char *filter = argc > 3 ? argv[3] : NULL;
  size_t target = 8u << 20;
  Buf b;
  b = (Buf){0}; gen_ascii(&b, 40, 80, target);      throughput("ascii_lines", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_ascii(&b, 250, 400, target);    throughput("ascii_long_lines", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_unicode(&b, target);            throughput("unicode_mixed", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_sgr(&b, target);                throughput("sgr_colors", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_csi_cursor(&b, target);         throughput("csi_cursor", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_scroll_region(&b, target);      throughput("scroll_region", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_fullscreen(&b, target);         throughput("fullscreen_redraw", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_osc(&b, target);                throughput("osc_and_marks", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_osc_plain(&b, target);          throughput("osc_without_marks", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_osc133(&b, target);             throughput("osc133_marks_only", &b, secs, trials, filter); free(b.p);
  b = (Buf){0}; gen_long_escape(&b, target);        throughput("long_escapes", &b, secs, trials, filter); free(b.p);
  ops(filter);
  return 0;
}
