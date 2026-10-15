#ifndef __VTERM_SCROLLBACK_H__
#define __VTERM_SCROLLBACK_H__

#include "screen_types.h"

/* Compact scrollback store, owned by the screen.
 *
 * A scrolled-off row is kept as a short record: the text as UTF-8 (one byte
 * per ASCII cell) plus the pen, stored once per run of equal pens (and not at
 * all for a row drawn entirely with the default pen). Rows are packed back to
 * back into large blocks, so storing one costs no allocation of its own and
 * dropping the oldest rows frees whole blocks at a time. */

typedef struct SbBlock SbBlock;

typedef struct
{
  VTerm *vt;

  size_t max_lines; /* 0 = store disabled */
  size_t max_bytes;

  SbBlock *first, *last;

  uint8_t **index;   /* ring of record pointers, oldest first */
  size_t index_cap;  /* power of two */
  size_t head, count;
  size_t bytes;      /* sum of record sizes */

  uint8_t *scratch;
  size_t scratch_cap;

  ScreenPen default_pen;
  bool default_pen_valid;

  uint64_t pushed, popped;
} VTermScrollback;

INTERNAL void vterm_scrollback_init(VTermScrollback *sb, VTerm *vt);
INTERNAL void vterm_scrollback_free(VTermScrollback *sb);
INTERNAL void vterm_scrollback_set_limits(VTermScrollback *sb, size_t max_lines, size_t max_bytes);
INTERNAL void vterm_scrollback_clear(VTermScrollback *sb);

/* Stores `cols` cells of a row; trailing blank cells are dropped when `trim` */
INTERNAL void vterm_scrollback_push(VTermScrollback *sb, const ScreenCell *row, int cols, const ScreenPen *pens, bool trim, bool continuation);

/* Expands row `idx` (0 = oldest); returns the number of cells written, at most maxcols */
INTERNAL int vterm_scrollback_get(const VTermScrollback *sb, size_t idx, VTermScreenCell *cells, int maxcols, int global_reverse, bool *continuation);

/* Removes the newest row and expands it like vterm_scrollback_get; -1 if empty */
INTERNAL int vterm_scrollback_pop(VTermScrollback *sb, VTermScreenCell *cells, int maxcols, int global_reverse, bool *continuation);

INTERNAL void vterm_scrollback_blank_cell(const VTermScrollback *sb, VTermScreenCell *cell, int global_reverse);

#endif
