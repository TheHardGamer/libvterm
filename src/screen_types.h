#ifndef __VTERM_SCREEN_TYPES_H__
#define __VTERM_SCREEN_TYPES_H__

#include "vterm_internal.h"

/* State of the pen at some moment in time, also used in a cell */
typedef struct
{
  /* After the bitfield */
  VTermColor   fg, bg;
  VTermColor   uline_color;
  uint32_t     link;

  unsigned int bold      : 1;
  unsigned int underline : 2;
  unsigned int italic    : 1;
  unsigned int blink     : 1;
  unsigned int reverse   : 1;
  unsigned int conceal   : 1;
  unsigned int strike    : 1;
  unsigned int font      : 4; /* 0 to 9 */
  unsigned int small     : 1;
  unsigned int baseline  : 2;

  /* Extra state storage that isn't strictly pen-related */
  unsigned int protected_cell : 1;
  unsigned int dwl            : 1; /* on a DECDWL or DECDHL line */
  unsigned int dhl            : 2; /* on a DECDHL line (1=top 2=bottom) */
} ScreenPen;

/* Internal representation of a screen cell. Pens are interned: a screen has far
 * fewer distinct pens than cells, so a cell holds the index of its pen and two
 * cells have the same pen exactly when the indexes match. */
typedef struct
{
  uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
  uint32_t pen; /* index into the screen's table of pens, see intern_pen() */
} ScreenCell;

/* Copy the pen attributes from an internal ScreenPen to a public VTermScreenCell.
 * This is a separate helper because vterm_screen_get_row reuses the same
 * attributes for runs of identical cells. */
static inline void set_public_attrs(VTermScreenCell *cell, const ScreenPen *pen, int global_reverse)
{
  cell->attrs.bold      = pen->bold;
  cell->attrs.underline = pen->underline;
  cell->attrs.italic    = pen->italic;
  cell->attrs.blink     = pen->blink;
  cell->attrs.reverse   = pen->reverse ^ global_reverse;
  cell->attrs.conceal   = pen->conceal;
  cell->attrs.strike    = pen->strike;
  cell->attrs.font      = pen->font;
  cell->attrs.small     = pen->small;
  cell->attrs.baseline  = pen->baseline;

  cell->attrs.dwl = pen->dwl;
  cell->attrs.dhl = pen->dhl;

  cell->fg = pen->fg;
  cell->bg = pen->bg;
  cell->uline = pen->uline_color;
  cell->link  = pen->link;
}

#endif
