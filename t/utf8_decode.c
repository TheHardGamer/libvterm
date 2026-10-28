/* The UTF-8 decoder must give the same code points however the input is cut into calls and however
 * small the output buffer is. Whole-buffer calls can take the 8-bytes-at-a-time ASCII fast path;
 * one-byte calls never can. So any difference between them is a fast-path bug.
 *
 *   cc -O1 -g -fsanitize=address,undefined -std=gnu99 -Isrc t/utf8_decode.c src/*.c -o t/utf8_decode
 *   ./t/utf8_decode [cases] [first-seed]
 *
 * A control character (C0 or DEL) makes the decoder return with the byte still unread; the parser
 * handles it, so this driver records it (as a negative number) and steps over it.
 */
#include "vterm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state;
static uint32_t rnd(void){ rng_state = rng_state*6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(rng_state>>33); }
static int rn(int n){ return n>0 ? (int)(rnd()%n) : 0; }

#define MAXIN 512
#define MAXOUT (MAXIN*2 + 8)

/* Decodes in[0..n) cut into calls of up to `chunkmax` bytes (0: one call) into out[]; returns the count */
static size_t run(const unsigned char *in, size_t n, int chunkmax, int cplen_max, long *out)
{
  VTermEncodingInstance e;
  e.enc = vterm_lookup_encoding(ENC_UTF8, 'u');
  e.enc->init(e.enc, e.data);

  size_t no = 0, off = 0;
  while(off < n) {
    size_t len = chunkmax ? (size_t)(1 + rn(chunkmax)) : n - off;
    if(off + len > n)
      len = n - off;
    size_t pos = 0;
    while(pos < len) {
      uint32_t cp[64];
      int cpi = 0;
      int cplen = 1 + rn(cplen_max);
      e.enc->decode(e.enc, e.data, cp, &cpi, cplen, (const char *)in + off, &pos, len);
      for(int i = 0; i < cpi; i++)
        out[no++] = (long)cp[i];
      if(pos < len && cpi < cplen) {
        unsigned char c = in[off + pos];
        if(!(c < 0x20 || c == 0x7f)) {
          fprintf(stderr, "decoder stopped at byte %02x that is not a control character\n", c);
          exit(2);
        }
        out[no++] = -(long)c - 1;
        pos++;
      }
    }
    off += len;
  }
  return no;
}

static size_t make_input(unsigned char *buf)
{
  size_t n = 0;
  int segs = 1 + rn(14);
  while(segs-- && n < MAXIN - 8) {
    switch(rn(8)) {
    case 0: case 1: case 2: { /* a run of printable ASCII, long enough for the fast path */
      int len = rn(40);
      while(len-- && n < MAXIN - 8) buf[n++] = (unsigned char)(0x20 + rn(0x5f));
      break; }
    case 3: /* a control character or DEL, at any offset within an 8 byte window */
      buf[n++] = rn(2) ? (unsigned char)rn(0x20) : 0x7f;
      break;
    case 4: { /* valid multi-byte sequences */
      int count = 1 + rn(4);
      while(count-- && n < MAXIN - 8) {
        static const unsigned char seqs[][4] = {
          {0xc3,0xa9,0,0}, {0xe2,0x82,0xac,0}, {0xf0,0x9f,0x98,0x80}, {0xe4,0xb8,0xad,0}, {0xcc,0x81,0,0}};
        int k = rn(5);
        int l = seqs[k][2] == 0 ? 2 : (seqs[k][3] == 0 ? 3 : 4);
        for(int i = 0; i < l; i++) buf[n++] = seqs[k][i];
      }
      break; }
    case 5: /* stray continuation bytes */
      buf[n++] = (unsigned char)(0x80 + rn(0x40));
      break;
    case 6: { /* a lead byte, maybe cut short by what follows */
      buf[n++] = (unsigned char)(0xc0 + rn(0x38));
      if(rn(2)) buf[n++] = (unsigned char)(0x80 + rn(0x40));
      break; }
    default: /* anything */
      buf[n++] = (unsigned char)rn(256);
      break;
    }
  }
  return n;
}

/* A sequence cut short by an ASCII byte makes the decoder emit two code points. With room for only one
 * it must not write the second past the end of the output buffer (the parser's buffer holds exactly
 * `cplen` entries). The buffers here are exactly that big, so AddressSanitizer notices any overrun. */
static void test_output_room(void)
{
  static const unsigned char in[] = { 0xe2, 0x82, 'a', 'b' }; /* a 3 byte lead, cut short by 'a' */
  for(int cplen = 1; cplen <= 4; cplen++) {
    for(int lead = 1; lead <= 2; lead++) { /* after the lead, or after the lead and one continuation */
      VTermEncodingInstance e;
      e.enc = vterm_lookup_encoding(ENC_UTF8, 'u');
      e.enc->init(e.enc, e.data);

      uint32_t *cp = malloc((size_t)cplen * sizeof(uint32_t));
      unsigned char input[8];
      size_t n = 0;
      for(int i = 0; i < lead; i++) input[n++] = in[i];
      input[n++] = 'a';
      input[n++] = 'b';

      long got[16]; size_t ng = 0;
      size_t pos = 0;
      while(pos < n) {
        int cpi = 0;
        size_t before = pos;
        e.enc->decode(e.enc, e.data, cp, &cpi, cplen, (const char *)input, &pos, n);
        for(int i = 0; i < cpi; i++) got[ng++] = (long)cp[i];
        if(pos == before && cpi == 0) { fprintf(stderr, "no progress (cplen %d)\n", cplen); exit(2); }
      }
      free(cp);
      /* the same text, whatever the room: replacement character, 'a', 'b' */
      if(ng != 3 || got[0] != 0xFFFD || got[1] != 'a' || got[2] != 'b') {
        fprintf(stderr, "wrong output with room for %d (got %zu code points)\n", cplen, ng);
        exit(1);
      }
    }
  }
}

int main(int argc, char **argv)
{
  test_output_room();
  uint64_t cases = argc > 1 ? strtoull(argv[1], 0, 10) : 200000;
  uint64_t seed0 = argc > 2 ? strtoull(argv[2], 0, 10) : 1;
  static long a[MAXOUT], b[MAXOUT];
  unsigned char buf[MAXIN];
  uint64_t fastable = 0;

  for(uint64_t seed = seed0; seed < seed0 + cases; seed++) {
    rng_state = seed * 2654435761ULL + 12345;
    size_t n = make_input(buf);
    const int cplen_max = 1 + rn(64);

    /* byte at a time: the reference, it cannot use the fast path */
    size_t nb = run(buf, n, 1, cplen_max, b);

    for(int mode = 0; mode < 3; mode++) {
      int chunkmax = mode == 0 ? 0 : (mode == 1 ? 9 : 100);
      size_t na = run(buf, n, chunkmax, cplen_max, a);
      if(na != nb || memcmp(a, b, na * sizeof(long))) {
        fprintf(stderr, "MISMATCH seed %llu mode %d (n=%zu, %zu vs %zu code points)\n",
            (unsigned long long)seed, mode, n, na, nb);
        for(size_t i = 0; i < n; i++) fprintf(stderr, "%02x ", buf[i]);
        fprintf(stderr, "\n");
        return 1;
      }
    }
    if(n >= 8) fastable++;
  }
  printf("utf8_decode: %llu cases equal (%llu long enough for the fast path)\n",
      (unsigned long long)cases, (unsigned long long)fastable);
  return 0;
}
