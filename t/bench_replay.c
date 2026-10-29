/* Library-only parser benchmark, replaying data recorded from kitty's `kitten __benchmark`: no PTY, no
 * renderer, no window, and kitten is not needed. Only vterm_input_write() on a 150x40 screen with the
 * library scrollback on is timed.
 *
 * Build like the library is built, run on one pinned core:
 *   cc -O3 -flto -DNDEBUG -std=gnu99 -Iinclude -Isrc t/bench_replay.c src/[a-z]*.c -o bench_replay
 *   taskset -c 6 ./bench_replay t/data/kitten-benchmark.bin.gz alt
 *   taskset -c 6 ./bench_replay t/data/kitten-benchmark.bin.gz scroll
 *
 * usage: bench_replay DATA alt|scroll [seconds per trial = 1.5] [trials = 5]
 *   DATA    t/data/kitten-benchmark.bin.gz (read with the system gzip), or any recording of kitten's
 *           benchmark output; see t/make_bench_data.py for how the file was made
 *   alt     every test runs on the alternate screen (kitten's default)
 *   scroll  every test runs on the primary screen with the library scrollback (kitten --with-scrollback)
 * Output, one line per kitten test: name, then median, best and worst MiB/s over the trials.
 * The data is split at kitten's "Running: <test>" lines, each test's bytes are replayed over and over
 * until a trial has run for the given time, in 64 KiB pieces. The screen mode is set here, whatever the
 * data says: any ESC [ ? 1049 h / l in it is dropped. Compare two builds by running both on the same
 * file, taking turns. */
#include "vterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int damage(VTermRect r, void *u){ (void)r; (void)u; return 1; }
static int prop(VTermProp p, VTermValue *v, void *u){ (void)p; (void)v; (void)u; return 1; }
static void sink(const char *s, size_t l, void *u){ (void)s; (void)l; (void)u; }

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static VTerm *make_vt(VTermScreen **scp)
{
  VTerm *vt = vterm_new(40, 150);
  vterm_set_utf8(vt, 1);
  vterm_output_set_callback(vt, sink, NULL);
  VTermScreen *sc = vterm_obtain_screen(vt);
  static VTermScreenCallbacks cb; cb.damage = damage; cb.settermprop = prop;
  vterm_screen_set_callbacks(sc, &cb, NULL);
  vterm_screen_set_damage_merge(sc, VTERM_DAMAGE_ROW);
  vterm_screen_enable_altscreen(sc, 1);
  vterm_screen_enable_scrollback(sc, 3000, (size_t)3000 * 150 * 8);
  vterm_screen_reset(sc, 1);
  *scp = sc;
  return vt;
}

static void feed(VTerm *vt, const char *p, size_t n)
{
  for(size_t off = 0; off < n; off += 65536) {
    size_t l = n - off < 65536 ? n - off : 65536;
    vterm_input_write(vt, p + off, l);
  }
}

static void feedstr(VTerm *vt, const char *s){ vterm_input_write(vt, s, strlen(s)); }

/* The whole file in memory; a name ending in .gz goes through the system gzip */
static char *load(const char *file, size_t *len)
{
  size_t cap = 1 << 22, n = 0;
  char *buf = malloc(cap);
  FILE *f;
  int gz = strlen(file) > 3 && !strcmp(file + strlen(file) - 3, ".gz");
  if(gz) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "gzip -dc '%s'", file);
    f = popen(cmd, "r");
  }
  else
    f = fopen(file, "rb");
  if(!f || !buf) return NULL;
  for(;;) {
    if(n == cap) { cap *= 2; buf = realloc(buf, cap); if(!buf) return NULL; }
    size_t r = fread(buf + n, 1, cap - n, f);
    if(!r) break;
    n += r;
  }
  if(gz) pclose(f); else fclose(f);
  *len = n;
  return buf;
}

int main(int argc, char **argv)
{
  if(argc < 3 || (strcmp(argv[2], "alt") && strcmp(argv[2], "scroll"))) {
    fprintf(stderr, "usage: %s DATA alt|scroll [seconds per trial] [trials]\n", argv[0]);
    return 2;
  }
  const char *file = argv[1];
  int alt = !strcmp(argv[2], "alt");
  double min_s = argc > 3 ? atof(argv[3]) : 1.5;
  if(min_s <= 0) min_s = 1.5;
  int trials = argc > 4 ? atoi(argv[4]) : 5;
  if(trials < 1) trials = 1;
  if(trials > 16) trials = 16;

  size_t n = 0;
  char *buf = load(file, &n);
  if(!buf || !n) { fprintf(stderr, "cannot read %s\n", file); return 1; }

  size_t pos[64]; char names[64][64]; int nm = 0;
  for(size_t i = 0; i + 9 < n && nm < 64; i++) {
    if(!memcmp(buf + i, "Running: ", 9)) {
      size_t j = i + 9, k = 0;
      while(j < n && buf[j] != '\r' && buf[j] != '\n' && buf[j] != 0x1b && k < 63) names[nm][k++] = buf[j++];
      names[nm][k] = 0; pos[nm++] = i;
    }
  }
  if(!nm) { fprintf(stderr, "no \"Running: ...\" lines: is %s a recording of kitten's benchmark?\n", file); return 1; }

  static const char *order[] = {"Only ASCII chars", "Unicode chars", "Unique multi-codepoint Unicode cells",
                                "CSI codes with few chars", "Long escape codes", "Images"};
  for(int t = 0; t < 6; t++) {
    /* all runs of this test, in order, as one replay unit, without screen switches */
    size_t cap = 0;
    for(int i = 0; i < nm; i++) if(!strcmp(names[i], order[t])) cap += (i + 1 < nm ? pos[i + 1] : n) - pos[i];
    if(!cap) { fprintf(stderr, "%s: not in the data\n", order[t]); continue; }
    char *unit = malloc(cap); size_t u = 0;
    for(int i = 0; i < nm; i++) {
      if(strcmp(names[i], order[t])) continue;
      size_t e = i + 1 < nm ? pos[i + 1] : n;
      for(size_t p = pos[i]; p < e; ) {
        if(e - p >= 8 && !memcmp(buf + p, "\x1b[?1049", 7) && (buf[p + 7] == 'h' || buf[p + 7] == 'l')) { p += 8; continue; }
        unit[u++] = buf[p++];
      }
    }

    double rates[16]; int nr = 0;
    for(int trial = 0; trial < trials; trial++) {
      VTermScreen *sc; VTerm *vt = make_vt(&sc);
      if(alt) feedstr(vt, "\x1b[?1049h\x1b[H\x1b[2J");
      feed(vt, unit, u); /* one untimed pass: warm caches, reach a steady screen */
      double t0 = now(); size_t total = 0;
      do {
        feed(vt, unit, u); total += u;
        if(alt) feedstr(vt, "\x1b[?1049l\x1b[?1049h\x1b[H\x1b[2J"); /* the next run starts on a fresh alternate screen */
      } while(now() - t0 < min_s);
      vterm_screen_flush_damage(sc);
      rates[nr++] = total / 1048576.0 / (now() - t0);
      vterm_free(vt);
    }
    for(int i = 0; i < nr; i++) for(int j = i + 1; j < nr; j++) if(rates[j] < rates[i]) { double x = rates[i]; rates[i] = rates[j]; rates[j] = x; }
    printf("%s\t%.1f\t%.1f\t%.1f\n", order[t], rates[nr / 2], rates[nr - 1], rates[0]);
    free(unit);
  }
  free(buf);
  return 0;
}
