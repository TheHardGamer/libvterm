#include "scrollback.h"

#include <string.h>

#define SB_BLOCK_SIZE (128 * 1024)

/* Record layout (all multi-byte fields via memcpy, records are unaligned):
 *   u32 len        size of the whole record
 *   u16 cols       number of cells
 *   u8  flags
 *   u8  reserved
 *   [pens]         see the SB_PENS_* flags
 *   text           one entry per cell:
 *                    0x00                 empty cell
 *                    0xFF                 second half of a wide character
 *                    UTF-8 sequence       first character
 *                    0xFD UTF-8 sequence  each further (combining) character
 */
enum {
  SB_CONTINUATION = 1 << 0,
  SB_PENS_DEFAULT = 1 << 1, /* every cell has the default pen: nothing stored */
  SB_PENS_ONE     = 1 << 2, /* every cell has the same pen: stored once */
  /* neither: u16 runs, then per run: u16 cells + pen */
};

#define SB_HDR 8
#define SB_MARK_EMPTY 0x00
#define SB_MARK_MORE  0xFD
#define SB_MARK_WIDE  0xFF

struct SbBlock
{
  SbBlock *prev, *next;
  uint8_t *data;
  size_t cap, used;
  size_t nlines;
};

INTERNAL void vterm_scrollback_init(VTermScrollback *sb, VTerm *vt)
{
  memset(sb, 0, sizeof(*sb));
  sb->vt = vt;
}

static void free_blocks(VTermScrollback *sb)
{
  SbBlock *b = sb->first;
  while(b) {
    SbBlock *next = b->next;
    vterm_allocator_free(sb->vt, b->data);
    vterm_allocator_free(sb->vt, b);
    b = next;
  }
  sb->first = sb->last = NULL;
}

INTERNAL void vterm_scrollback_clear(VTermScrollback *sb)
{
  free_blocks(sb);
  sb->head = sb->count = 0;
  sb->bytes = 0;
}

INTERNAL void vterm_scrollback_free(VTermScrollback *sb)
{
  free_blocks(sb);
  vterm_allocator_free(sb->vt, sb->index);
  vterm_allocator_free(sb->vt, sb->scratch);
  sb->index = NULL;
  sb->scratch = NULL;
}

static inline uint8_t *line_at(const VTermScrollback *sb, size_t idx)
{
  return sb->index[(sb->head + idx) & (sb->index_cap - 1)];
}

static inline uint32_t record_len(const uint8_t *rec)
{
  uint32_t len;
  memcpy(&len, rec, sizeof(len));
  return len;
}

/* Drops the oldest row */
static void drop_oldest(VTermScrollback *sb)
{
  uint8_t *rec = line_at(sb, 0);
  sb->bytes -= record_len(rec);
  sb->head = (sb->head + 1) & (sb->index_cap - 1);
  sb->count--;

  SbBlock *b = sb->first;
  if(--b->nlines == 0) {
    if(b->next) {
      sb->first = b->next;
      b->next->prev = NULL;
      vterm_allocator_free(sb->vt, b->data);
      vterm_allocator_free(sb->vt, b);
    }
    else
      b->used = 0; /* the only block: reuse it */
  }
}

static void enforce_limits(VTermScrollback *sb)
{
  while(sb->count > 0 && (sb->count > sb->max_lines || sb->bytes > sb->max_bytes))
    drop_oldest(sb);
}

INTERNAL void vterm_scrollback_set_limits(VTermScrollback *sb, size_t max_lines, size_t max_bytes)
{
  sb->max_lines = max_lines;
  sb->max_bytes = max_bytes;
  if(!max_lines || !max_bytes) {
    sb->max_lines = 0;
    vterm_scrollback_clear(sb);
    return;
  }
  enforce_limits(sb);
}

static void grow_index(VTermScrollback *sb)
{
  size_t new_cap = sb->index_cap ? sb->index_cap * 2 : 1024;
  uint8_t **new_index = vterm_allocator_malloc(sb->vt, new_cap * sizeof(*new_index));
  for(size_t i = 0; i < sb->count; i++)
    new_index[i] = line_at(sb, i);
  vterm_allocator_free(sb->vt, sb->index);
  sb->index = new_index;
  sb->index_cap = new_cap;
  sb->head = 0;
}

static inline uint8_t *put_utf8(uint8_t *p, uint32_t cp)
{
  if(cp < 0x80)
    *p++ = (uint8_t)cp;
  else if(cp < 0x800) {
    *p++ = 0xC0 | (cp >> 6);
    *p++ = 0x80 | (cp & 0x3F);
  }
  else if(cp < 0x10000) {
    *p++ = 0xE0 | (cp >> 12);
    *p++ = 0x80 | ((cp >> 6) & 0x3F);
    *p++ = 0x80 | (cp & 0x3F);
  }
  else {
    if(cp > 0x10FFFF)
      cp = 0xFFFD;
    *p++ = 0xF0 | (cp >> 18);
    *p++ = 0x80 | ((cp >> 12) & 0x3F);
    *p++ = 0x80 | ((cp >> 6) & 0x3F);
    *p++ = 0x80 | (cp & 0x3F);
  }
  return p;
}

static inline const uint8_t *get_utf8(const uint8_t *p, const uint8_t *end, uint32_t *cp)
{
  uint8_t c = *p++;
  if(c < 0x80) { *cp = c; return p; }
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
  uint32_t v = c & (0x3F >> extra);
  while(extra-- && p < end)
    v = (v << 6) | (*p++ & 0x3F);
  *cp = v;
  return p;
}

/* A trailing cell that shows nothing: no character, default background, and no
 * attribute that draws on an empty cell. */
static inline bool blank_tail(const ScreenCell *cell, const ScreenPen *pens)
{
  const ScreenPen *pen = &pens[cell->pen];
  return cell->chars[0] == 0 &&
         VTERM_COLOR_IS_DEFAULT_BG(&pen->bg) &&
         !pen->reverse && !pen->underline && !pen->strike;
}

/* Worst case size of the record for `cols` cells */
static size_t max_record(int cols)
{
  return SB_HDR + 2 + (size_t)cols * (2 + sizeof(ScreenPen)) + (size_t)cols * (4 + (VTERM_MAX_CHARS_PER_CELL - 1) * 5);
}

static uint8_t *alloc_record(VTermScrollback *sb, size_t len)
{
  SbBlock *b = sb->last;
  if(!b || b->cap - b->used < len) {
    b = vterm_allocator_malloc(sb->vt, sizeof(*b));
    b->cap = len > SB_BLOCK_SIZE ? len : SB_BLOCK_SIZE;
    b->data = vterm_allocator_malloc(sb->vt, b->cap);
    b->used = 0;
    b->nlines = 0;
    b->next = NULL;
    b->prev = sb->last;
    if(sb->last)
      sb->last->next = b;
    else
      sb->first = b;
    sb->last = b;
  }
  uint8_t *rec = b->data + b->used;
  b->used += len;
  b->nlines++;
  return rec;
}

INTERNAL void vterm_scrollback_push(VTermScrollback *sb, const ScreenCell *row, int cols, const ScreenPen *pens, bool trim, bool continuation)
{
  if(!sb->max_lines)
    return;

  if(trim)
    while(cols > 0 && blank_tail(row + cols - 1, pens))
      cols--;
  if(cols > 0xFFFF)
    cols = 0xFFFF;

  size_t need = max_record(cols);
  if(need > sb->scratch_cap) {
    vterm_allocator_free(sb->vt, sb->scratch);
    sb->scratch_cap = need;
    sb->scratch = vterm_allocator_malloc(sb->vt, need);
  }

  uint8_t *rec = sb->scratch;
  uint8_t *p = rec + SB_HDR;
  uint8_t flags = continuation ? SB_CONTINUATION : 0;

  /* pens: find the runs first, they are small compared to the text */
  int nruns = 0;
  uint8_t *runs_at = p + 2;
  uint8_t *rp = runs_at;
  {
    int start = 0;
    for(int col = 1; col <= cols; col++) {
      if(col == cols || row[col].pen != row[start].pen) {
        if(cols > 0) {
          uint16_t n = (uint16_t)(col - start);
          memcpy(rp, &n, 2);
          memcpy(rp + 2, &pens[row[start].pen], sizeof(ScreenPen));
          rp += 2 + sizeof(ScreenPen);
          nruns++;
        }
        start = col;
      }
    }
  }

  if(nruns == 0 || (nruns == 1 && sb->default_pen_valid &&
        memcmp(&pens[row[0].pen], &sb->default_pen, sizeof(ScreenPen)) == 0)) {
    flags |= SB_PENS_DEFAULT;
  }
  else if(nruns == 1) {
    flags |= SB_PENS_ONE;
    memmove(p, rp - sizeof(ScreenPen), sizeof(ScreenPen)); /* the run sits just after the count */
    p += sizeof(ScreenPen);
  }
  else {
    uint16_t n = (uint16_t)nruns;
    memcpy(p, &n, 2);
    p += 2 + (rp - runs_at);
    /* runs are already in place right after the count */
  }

  /* text */
  for(int col = 0; col < cols; col++) {
    const uint32_t *ch = row[col].chars;
    uint32_t c0 = ch[0];
    if(c0 - 1 < 0x7F && ch[1] == 0) { /* printable ASCII or control, non-zero */
      *p++ = (uint8_t)c0;
    }
    else if(c0 == 0)
      *p++ = SB_MARK_EMPTY;
    else if(c0 == (uint32_t)-1)
      *p++ = SB_MARK_WIDE;
    else {
      p = put_utf8(p, c0);
      for(int i = 1; i < VTERM_MAX_CHARS_PER_CELL && ch[i]; i++) {
        *p++ = SB_MARK_MORE;
        p = put_utf8(p, ch[i]);
      }
    }
  }

  uint32_t len = (uint32_t)(p - rec);
  uint16_t ncols = (uint16_t)cols;
  memcpy(rec, &len, 4);
  memcpy(rec + 4, &ncols, 2);
  rec[6] = flags;
  rec[7] = 0;

  if(sb->count == sb->index_cap)
    grow_index(sb);

  uint8_t *dst = alloc_record(sb, len);
  memcpy(dst, rec, len);
  sb->index[(sb->head + sb->count) & (sb->index_cap - 1)] = dst;
  sb->count++;
  sb->bytes += len;
  sb->pushed++;

  enforce_limits(sb);
}

static int expand(const VTermScrollback *sb, const uint8_t *rec, VTermScreenCell *cells, int maxcols, int global_reverse, bool *continuation)
{
  uint32_t len = record_len(rec);
  uint16_t cols;
  memcpy(&cols, rec + 4, 2);
  uint8_t flags = rec[6];
  const uint8_t *end = rec + len;
  const uint8_t *p = rec + SB_HDR;

  if(continuation)
    *continuation = !!(flags & SB_CONTINUATION);

  int limit = cols < maxcols ? cols : maxcols;

  /* pens */
  ScreenPen pen_one;
  uint16_t nruns = 0;
  const uint8_t *runs = NULL;
  if(flags & SB_PENS_DEFAULT) {
    if(sb->default_pen_valid)
      pen_one = sb->default_pen;
    else
      memset(&pen_one, 0, sizeof(pen_one));
  }
  else if(flags & SB_PENS_ONE) {
    memcpy(&pen_one, p, sizeof(ScreenPen));
    p += sizeof(ScreenPen);
  }
  else {
    memcpy(&nruns, p, 2);
    runs = p + 2;
    p += 2 + (size_t)nruns * (2 + sizeof(ScreenPen));
  }

  VTermScreenCell attrs; /* template carrying the attributes of the current run */
  memset(&attrs, 0, sizeof(attrs));
  size_t run_left = (size_t)-1;
  if(!runs)
    set_public_attrs(&attrs, &pen_one, global_reverse);

  for(int col = 0; col < limit; col++) {
    VTermScreenCell *cell = &cells[col];

    if(runs) {
      if(run_left == 0 || run_left == (size_t)-1) {
        if(nruns == 0) {
          /* malformed, cannot happen */
          memset(&pen_one, 0, sizeof(pen_one));
          set_public_attrs(&attrs, &pen_one, global_reverse);
          run_left = (size_t)-1 >> 1;
        }
        else {
          uint16_t n;
          memcpy(&n, runs, 2);
          memcpy(&pen_one, runs + 2, sizeof(ScreenPen));
          runs += 2 + sizeof(ScreenPen);
          nruns--;
          run_left = n;
          set_public_attrs(&attrs, &pen_one, global_reverse);
        }
      }
      run_left--;
    }

    cell->attrs = attrs.attrs;
    cell->fg = attrs.fg;
    cell->bg = attrs.bg;
    cell->uline = attrs.uline;
    cell->link = attrs.link;
    cell->width = 1;

    int n = 1;
    if(p >= end || *p == SB_MARK_EMPTY) {
      cell->chars[0] = 0;
      if(p < end)
        p++;
    }
    else if(*p == SB_MARK_WIDE) {
      cell->chars[0] = (uint32_t)-1;
      p++;
      if(col > 0)
        cells[col - 1].width = 2;
    }
    else {
      p = get_utf8(p, end, &cell->chars[0]);
      while(p < end && *p == SB_MARK_MORE) {
        uint32_t cp;
        p = get_utf8(p + 1, end, &cp);
        if(n < VTERM_MAX_CHARS_PER_CELL)
          cell->chars[n++] = cp;
      }
    }
    for(; n < VTERM_MAX_CHARS_PER_CELL; n++)
      cell->chars[n] = 0;
  }

  /* a wide character cut off by maxcols keeps its width, as get_row does */
  if(limit > 0 && limit < cols && p < end && *p == SB_MARK_WIDE)
    cells[limit - 1].width = 2;

  return limit;
}

INTERNAL int vterm_scrollback_get(const VTermScrollback *sb, size_t idx, VTermScreenCell *cells, int maxcols, int global_reverse, bool *continuation)
{
  if(idx >= sb->count || maxcols <= 0)
    return -1;
  return expand(sb, line_at(sb, idx), cells, maxcols, global_reverse, continuation);
}

INTERNAL int vterm_scrollback_pop(VTermScrollback *sb, VTermScreenCell *cells, int maxcols, int global_reverse, bool *continuation)
{
  if(!sb->count)
    return -1;

  uint8_t *rec = line_at(sb, sb->count - 1);
  int n = expand(sb, rec, cells, maxcols, global_reverse, continuation);

  sb->bytes -= record_len(rec);
  sb->count--;
  sb->popped++;

  SbBlock *b = sb->last;
  if(--b->nlines == 0) {
    if(b->prev) {
      sb->last = b->prev;
      b->prev->next = NULL;
      vterm_allocator_free(sb->vt, b->data);
      vterm_allocator_free(sb->vt, b);
    }
    else
      b->used = 0;
  }
  else
    b->used = (size_t)(rec - b->data);

  return n;
}

INTERNAL void vterm_scrollback_blank_cell(const VTermScrollback *sb, VTermScreenCell *cell, int global_reverse)
{
  ScreenPen pen;
  if(sb->default_pen_valid)
    pen = sb->default_pen;
  else
    memset(&pen, 0, sizeof(pen));
  memset(cell, 0, sizeof(*cell));
  set_public_attrs(cell, &pen, global_reverse);
  cell->width = 1;
}
