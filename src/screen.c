#include "vterm_internal.h"

#include <stdio.h>
#include <string.h>

#include "rect.h"
#include "utf8.h"

#define UNICODE_SPACE 0x20
#define UNICODE_LINEFEED 0x0a

#undef DEBUG_REFLOW

#include "scrollback.h"

struct VTermScreen
{
  VTerm *vt;
  VTermState *state;

  const VTermScreenCallbacks *callbacks;
  void *cbdata;
  bool callbacks_has_pushline4;
  bool sb_trim; /* hand trailing blank cells to sb_pushline as absent, see vterm_screen_set_sb_trim() */

  VTermDamageSize damage_merge;
  /* start_row == -1 => no damage */
  VTermRect damaged;
  VTermRect pending_scrollrect;
  int pending_scroll_downward, pending_scroll_rightward;

  int rows;
  int cols;

  unsigned int global_reverse : 1;
  unsigned int reflow : 1;

  /* Primary and Altscreen. buffers[1] is lazily allocated as needed */
  ScreenCell *buffers[2];

  /* Row pointer arrays for each buffer. row_ptrs[buf] is an array of ScreenCell*
   * that points into buffers[buf]; row_ptrs_active points to the current
   * buffer's array. Using row pointers allows vertical scrolling by rotating
   * pointers instead of memmove()-ing cell data. */
  ScreenCell **row_ptrs[2];
  ScreenCell **row_ptrs_active;

  /* buffer will == buffers[0] or buffers[1], depending on altscreen */
  ScreenCell *buffer;

  /* buffer for a single screen row used in scrollback storage callbacks */
  VTermScreenCell *sb_buffer;

  /* scrolled-off rows, when the library keeps them (see vterm_screen_enable_scrollback) */
  VTermScrollback sb;

  ScreenPen pen;
  bool mode_synchronized; /* track current sync state */

  /* Interned pens. `pens[id]` is unique among the entries; `pen_hash` is an
   * open-addressing table of id+1 (0 = empty). */
  ScreenPen *pens;
  uint32_t npens, pens_cap;
  uint32_t *pen_hash;
  uint32_t pen_hash_cap; /* power of two, at least twice pens_cap */
  uint32_t pen_id;       /* id of `pen`; valid unless pen_dirty */
  bool pen_dirty;
  uint32_t last_pen_id;  /* the id the last lookup found, tried first next time */
  int pen_gc_lock;       /* > 0 while cells exist that the collector cannot see */
};

_Static_assert(sizeof(ScreenPen) % sizeof(uint32_t) == 0, "pens are hashed a word at a time");

static inline uint32_t pen_hash_of(const ScreenPen *pen)
{
  uint32_t words[sizeof(ScreenPen) / sizeof(uint32_t)];
  memcpy(words, pen, sizeof(words));
  uint32_t h = 2166136261u;
  for(size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    h ^= words[i];
    h *= 16777619u;
    h ^= h >> 15;
  }
  return h;
}

static void pens_rehash(VTermScreen *screen)
{
  memset(screen->pen_hash, 0, sizeof(uint32_t) * screen->pen_hash_cap);
  uint32_t mask = screen->pen_hash_cap - 1;
  for(uint32_t id = 0; id < screen->npens; id++) {
    uint32_t slot = pen_hash_of(&screen->pens[id]) & mask;
    while(screen->pen_hash[slot])
      slot = (slot + 1) & mask;
    screen->pen_hash[slot] = id + 1;
  }
}

static void pens_resize_tables(VTermScreen *screen, uint32_t cap)
{
  ScreenPen *pens = vterm_allocator_malloc(screen->vt, sizeof(ScreenPen) * cap);
  if(screen->pens) {
    memcpy(pens, screen->pens, sizeof(ScreenPen) * screen->npens);
    vterm_allocator_free(screen->vt, screen->pens);
  }
  screen->pens = pens;
  screen->pens_cap = cap;

  vterm_allocator_free(screen->vt, screen->pen_hash);
  screen->pen_hash_cap = cap * 2;
  screen->pen_hash = vterm_allocator_malloc(screen->vt, sizeof(uint32_t) * screen->pen_hash_cap);
  pens_rehash(screen);
}

/* Drops the pens no cell uses, renumbering the survivors (cells are rewritten) */
static void pens_collect(VTermScreen *screen)
{
  uint32_t n = screen->npens;
  uint32_t *map = vterm_allocator_malloc(screen->vt, sizeof(uint32_t) * n);
  memset(map, 0xFF, sizeof(uint32_t) * n);

  for(int b = 0; b < 2; b++) {
    if(!screen->buffers[b])
      continue;
    size_t count = (size_t)screen->rows * screen->cols;
    for(size_t i = 0; i < count; i++) {
      uint32_t id = screen->buffers[b][i].pen;
      if(id < n)
        map[id] = 0;
    }
  }
  if(!screen->pen_dirty && screen->pen_id < n)
    map[screen->pen_id] = 0;

  uint32_t kept = 0;
  for(uint32_t id = 0; id < n; id++)
    if(map[id] == 0) {
      map[id] = kept;
      screen->pens[kept++] = screen->pens[id];
    }
  screen->npens = kept;

  for(int b = 0; b < 2; b++) {
    if(!screen->buffers[b])
      continue;
    size_t count = (size_t)screen->rows * screen->cols;
    for(size_t i = 0; i < count; i++) {
      uint32_t id = screen->buffers[b][i].pen;
      if(id < n)
        screen->buffers[b][i].pen = map[id];
    }
  }
  if(!screen->pen_dirty && screen->pen_id < n)
    screen->pen_id = map[screen->pen_id];
  screen->last_pen_id = 0;

  vterm_allocator_free(screen->vt, map);
  pens_rehash(screen);
}

/* Returns the id of `pen`, adding it to the table if it is new. May renumber
 * the pens (and every cell), so ids from before the call must not be reused. */
static uint32_t intern_pen(VTermScreen *screen, const ScreenPen *pen)
{
  if(screen->last_pen_id < screen->npens &&
     memcmp(&screen->pens[screen->last_pen_id], pen, sizeof(ScreenPen)) == 0)
    return screen->last_pen_id;

  ScreenPen key = *pen; /* `pen` may point into the table */
  uint32_t mask = screen->pen_hash_cap - 1;
  uint32_t slot = pen_hash_of(&key) & mask;
  while(screen->pen_hash[slot]) {
    uint32_t id = screen->pen_hash[slot] - 1;
    if(memcmp(&screen->pens[id], &key, sizeof(ScreenPen)) == 0) {
      screen->last_pen_id = id;
      return id;
    }
    slot = (slot + 1) & mask;
  }

  if(screen->npens == screen->pens_cap) {
    if(!screen->pen_gc_lock)
      pens_collect(screen);
    if(screen->npens >= screen->pens_cap / 2)
      pens_resize_tables(screen, screen->pens_cap * 2);
    return intern_pen(screen, &key);
  }

  uint32_t id = screen->npens++;
  screen->pens[id] = key;
  screen->pen_hash[slot] = id + 1;
  screen->last_pen_id = id;
  return id;
}

static inline uint32_t current_pen(VTermScreen *screen)
{
  if(screen->pen_dirty) {
    screen->pen_id = intern_pen(screen, &screen->pen);
    screen->pen_dirty = false;
  }
  return screen->pen_id;
}

static inline const ScreenPen *cell_pen(const VTermScreen *screen, const ScreenCell *cell)
{
  return &screen->pens[cell->pen];
}

/* Invariant: every entry of ScreenCell.chars after the first 0 is also 0, so
 * a whole cell can be copied without looking for the terminator. */
static inline void clearcell(VTermScreen *screen, ScreenCell *cell)
{
  memset(cell->chars, 0, sizeof(cell->chars));
  cell->pen = current_pen(screen);
}

static inline ScreenCell *getcell(const VTermScreen *screen, int row, int col)
{
  if(row < 0 || row >= screen->rows)
    return NULL;
  if(col < 0 || col >= screen->cols)
    return NULL;
  return screen->row_ptrs_active[row] + col;
}

static void alloc_buffer(VTermScreen *screen, int bufidx, int rows, int cols)
{
  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * rows * cols);

  ScreenCell blank;
  memset(blank.chars, 0, sizeof(blank.chars));
  blank.pen = current_pen(screen);
  for(int i = 0; i < rows * cols; i++)
    new_buffer[i] = blank;

  screen->buffers[bufidx] = new_buffer;

  ScreenCell **new_row_ptrs = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell*) * rows);
  for(int row = 0; row < rows; row++) {
    new_row_ptrs[row] = new_buffer + (row * cols);
  }

  screen->row_ptrs[bufidx] = new_row_ptrs;
}

static void damagerect(VTermScreen *screen, VTermRect rect)
{
  if (screen->mode_synchronized)
    return;
  VTermRect emit;

  switch(screen->damage_merge) {
  case VTERM_DAMAGE_CELL:
    /* Always emit damage event */
    emit = rect;
    break;

  case VTERM_DAMAGE_ROW:
    /* Emit damage longer than one row. Try to merge with existing damage in
     * the same row */
    if(rect.end_row > rect.start_row + 1) {
      // Bigger than 1 line - flush existing, emit this
      vterm_screen_flush_damage(screen);
      emit = rect;
    }
    else if(screen->damaged.start_row == -1) {
      // None stored yet
      screen->damaged = rect;
      return;
    }
    else if(rect.start_row == screen->damaged.start_row) {
      // Merge with the stored line
      if(screen->damaged.start_col > rect.start_col)
        screen->damaged.start_col = rect.start_col;
      if(screen->damaged.end_col < rect.end_col)
        screen->damaged.end_col = rect.end_col;
      return;
    }
    else {
      // Emit the currently stored line, store a new one
      emit = screen->damaged;
      screen->damaged = rect;
    }
    break;

  case VTERM_DAMAGE_SCREEN:
  case VTERM_DAMAGE_SCROLL:
    /* Never emit damage event */
    if(screen->damaged.start_row == -1)
      screen->damaged = rect;
    else {
      rect_expand(&screen->damaged, &rect);
    }
    return;

  default:
    DEBUG_LOG("TODO: Maybe merge damage for level %d\n", screen->damage_merge);
    return;
  }

  if(screen->callbacks && screen->callbacks->damage)
    (*screen->callbacks->damage)(emit, screen->cbdata);
}

static void damagescreen(VTermScreen *screen)
{
  VTermRect rect = {
    .start_row = 0,
    .end_row   = screen->rows,
    .start_col = 0,
    .end_col   = screen->cols,
  };

  damagerect(screen, rect);
}

static int putglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
  VTermScreen *screen = user;
  ScreenCell *cell = getcell(screen, pos.row, pos.col);

  if(!cell)
    return 0;

  ScreenPen pen;
  if(info->chars[0])
    pen = screen->pen;
  else
    pen = *cell_pen(screen, cell);

  int i;
  for(i = 0; i < VTERM_MAX_CHARS_PER_CELL && info->chars[i]; i++)
    cell->chars[i] = info->chars[i];
  for( ; i < VTERM_MAX_CHARS_PER_CELL; i++)
    cell->chars[i] = 0;

  int width = info->width;
  for(int col = 1; col < width; col++) {
    ScreenCell *gap = getcell(screen, pos.row, pos.col + col);
    if(!gap) {
      /* A wide glyph wider than what is left of the row (e.g. on a 1-column
       * screen): only the part that exists gets written. */
      width = col;
      break;
    }
    memset(gap->chars, 0, sizeof(gap->chars));
    gap->chars[0] = (uint32_t)-1;
  }

  VTermRect rect = {
    .start_row = pos.row,
    .end_row   = pos.row+1,
    .start_col = pos.col,
    .end_col   = pos.col+width,
  };

  pen.protected_cell = info->protected_cell;
  pen.dwl            = info->dwl;
  pen.dhl            = info->dhl;
  cell->pen = intern_pen(screen, &pen);

  damagerect(screen, rect);

  return 1;
}

INTERNAL int vterm_screen_putglyphs(VTermScreen *screen, VTermPos pos, const uint32_t *chars, int count, const VTermGlyphInfo *info)
{
  if(!screen || !chars || count <= 0)
    return 0;

  if(pos.row < 0 || pos.row >= screen->rows)
    return 0;
  if(pos.col < 0 || pos.col >= screen->cols)
    return 0;

  if(count > screen->cols - pos.col)
    count = screen->cols - pos.col;

  ScreenCell *rowstart = screen->row_ptrs_active[pos.row] + pos.col;

  ScreenPen pen = screen->pen;
  pen.protected_cell = info->protected_cell;
  pen.dwl            = info->dwl;
  pen.dhl            = info->dhl;
  /* putglyphs is given the pen on every call; screen->pen may be unchanged so
   * intern_pen's one-entry cache mostly answers this */

  const uint32_t pen_id = intern_pen(screen, &pen);
  for(int i = 0; i < count; i++) {
    ScreenCell *cell = rowstart + i;
    cell->chars[0] = chars[i];
    for(int j = 1; j < VTERM_MAX_CHARS_PER_CELL; j++)
      cell->chars[j] = 0;
    cell->pen = pen_id;
  }

  VTermRect rect = {
    .start_row = pos.row,
    .end_row   = pos.row + 1,
    .start_col = pos.col,
    .end_col   = pos.col + count,
  };
  damagerect(screen, rect);

  return count;
}

/* A trailing cell that shows nothing: no character, default background, and no
 * attribute that draws on an empty cell. */
static inline bool cell_is_blank_tail(const VTermScreen *screen, const ScreenCell *cell)
{
  const ScreenPen *pen = cell_pen(screen, cell);
  return cell->chars[0] == 0 &&
         VTERM_COLOR_IS_DEFAULT_BG(&pen->bg) &&
         !pen->reverse && !pen->underline && !pen->strike;
}

static void convert_row(const VTermScreen *screen, const ScreenCell *rowstart, VTermScreenCell *cells, int limit);

static inline bool sb_wanted(const VTermScreen *screen)
{
  return screen->sb.max_lines ||
    (screen->callbacks && screen->callbacks->sb_pushline) ||
    (screen->callbacks_has_pushline4 && screen->callbacks && screen->callbacks->sb_pushline4);
}

/* `rowstart` is the first cell of a row of the primary screen. It is passed in,
 * not looked up, because a resize pushes rows of the primary screen while the
 * alternate one may be the active one. */
static void sb_pushline_from_row(VTermScreen *screen, const ScreenCell *rowstart, bool continuation)
{
  int cols = screen->cols;

  if(screen->sb.max_lines) {
    vterm_scrollback_push(&screen->sb, rowstart, cols, screen->pens, !screen->global_reverse, continuation);
    return;
  }

  if(screen->sb_trim && !screen->global_reverse) {
    while(cols > 0 && cell_is_blank_tail(screen, rowstart + cols - 1))
      cols--;
  }

  convert_row(screen, rowstart, screen->sb_buffer, cols);

  if(screen->callbacks_has_pushline4 && screen->callbacks->sb_pushline4)
    (screen->callbacks->sb_pushline4)(cols, screen->sb_buffer, continuation, screen->cbdata);
  else
    (screen->callbacks->sb_pushline)(cols, screen->sb_buffer, screen->cbdata);
}

static int premove(VTermRect rect, void *user)
{
  VTermScreen *screen = user;

  if(sb_wanted(screen) &&
     rect.start_row == 0 && rect.start_col == 0 &&        // starts top-left corner
     rect.end_col == screen->cols &&                      // full width
     screen->buffer == screen->buffers[BUFIDX_PRIMARY]) { // not altscreen
    for(int row = 0; row < rect.end_row; row++) {
      const VTermLineInfo *lineinfo = vterm_state_get_lineinfo(screen->state, row);
      sb_pushline_from_row(screen, screen->row_ptrs_active[row], lineinfo->continuation);
    }
  }

  return 1;
}

static void reverse_row_ptrs(ScreenCell **row_ptrs, int start, int end)
{
  end--;
  while(start < end) {
    ScreenCell *tmp = row_ptrs[start];
    row_ptrs[start] = row_ptrs[end];
    row_ptrs[end] = tmp;
    start++;
    end--;
  }
}

static void rotate_row_ptrs(ScreenCell **row_ptrs, int start, int end, int k)
{
  int n = end - start;
  if(n <= 0 || k <= 0 || k >= n)
    return;

  reverse_row_ptrs(row_ptrs, start, start + k);
  reverse_row_ptrs(row_ptrs, start + k, end);
  reverse_row_ptrs(row_ptrs, start, end);
}

static int moverect_internal(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  int cols = src.end_col - src.start_col;
  int downward = src.start_row - dest.start_row;

  /* an empty rectangle may sit outside the screen, where there are no cells to point at */
  if(cols <= 0)
    return 1;

  /* Full-width vertical scroll: just rotate row pointers, then let erase_internal
   * clear the exposed rows. This avoids memmove()-ing every cell. */
  if(dest.start_col == 0 && src.start_col == 0 && cols == screen->cols) {
    int rect_start = dest.start_row < src.start_row ? dest.start_row : src.start_row;
    int rect_end   = dest.end_row   > src.end_row   ? dest.end_row   : src.end_row;
    int height = rect_end - rect_start;

    int k = downward % height;
    if(k < 0)
      k += height;

    rotate_row_ptrs(screen->row_ptrs_active, rect_start, rect_end, k);

    return 1;
  }

  int init_row, test_row, inc_row;
  if(downward < 0) {
    init_row = dest.end_row - 1;
    test_row = dest.start_row - 1;
    inc_row  = -1;
  }
  else {
    init_row = dest.start_row;
    test_row = dest.end_row;
    inc_row  = +1;
  }

  for(int row = init_row; row != test_row; row += inc_row)
    memmove(getcell(screen, row, dest.start_col),
            getcell(screen, row + downward, src.start_col),
            cols * sizeof(ScreenCell));

  return 1;
}

static int moverect_user(VTermRect dest, VTermRect src, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->moverect) {
    if(screen->damage_merge != VTERM_DAMAGE_SCROLL)
      // Avoid an infinite loop
      vterm_screen_flush_damage(screen);

    if((*screen->callbacks->moverect)(dest, src, screen->cbdata))
      return 1;
  }

  damagerect(screen, dest);

  return 1;
}

/* Fill n cells from one template by doubling memcpy()s. Much cheaper than
 * assigning a compound literal per cell. */
static void fill_cells(ScreenCell *dst, int n, const ScreenCell *tmpl)
{
  if(n <= 0)
    return;

  dst[0] = *tmpl;
  for(int filled = 1; filled < n; filled *= 2) {
    int chunk = filled < n - filled ? filled : n - filled;
    memcpy(dst + filled, dst, (size_t)chunk * sizeof(ScreenCell));
  }
}

static int erase_internal(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  ScreenCell blank;
  memset(&blank, 0, sizeof(blank));

  /* Only copy .fg and .bg; leave things like rv in reset state */
  ScreenPen blank_pen;
  memset(&blank_pen, 0, sizeof(blank_pen));
  blank_pen.fg = screen->pen.fg;
  blank_pen.bg = screen->pen.bg;
  blank_pen.uline_color = screen->pen.uline_color; /* so that a blank cell is a default cell when nothing else is set */
  const uint32_t plain_id = intern_pen(screen, &blank_pen);
  screen->pen_gc_lock++; /* ids interned below are held across the loop */

  for(int row = rect.start_row; row < screen->state->rows && row < rect.end_row; row++) {
    const VTermLineInfo *info = vterm_state_get_lineinfo(screen->state, row);

    if(info->doublewidth || info->doubleheight) {
      blank_pen.dwl = info->doublewidth;
      blank_pen.dhl = info->doubleheight;
      blank.pen = intern_pen(screen, &blank_pen);
    }
    else
      blank.pen = plain_id;

    if(!selective) {
      fill_cells(getcell(screen, row, rect.start_col), rect.end_col - rect.start_col, &blank);
      continue;
    }

    for(int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if(cell_pen(screen, cell)->protected_cell)
        continue;

      *cell = blank;
    }
  }

  screen->pen_gc_lock--;
  return 1;
}

static int erase_user(VTermRect rect, int selective, void *user)
{
  VTermScreen *screen = user;

  damagerect(screen, rect);

  return 1;
}

static int erase(VTermRect rect, int selective, void *user)
{
  erase_internal(rect, selective, user);
  return erase_user(rect, 0, user);
}

static int scrollrect(VTermRect rect, int downward, int rightward, void *user)
{
  VTermScreen *screen = user;

  if(screen->damage_merge != VTERM_DAMAGE_SCROLL) {
    vterm_scroll_rect(rect, downward, rightward,
        moverect_internal, erase_internal, screen);

    vterm_screen_flush_damage(screen);

    vterm_scroll_rect(rect, downward, rightward,
        moverect_user, erase_user, screen);

    return 1;
  }

  if(screen->damaged.start_row != -1 &&
     !rect_intersects(&rect, &screen->damaged)) {
    vterm_screen_flush_damage(screen);
  }

  if(screen->pending_scrollrect.start_row == -1) {
    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward  = downward;
    screen->pending_scroll_rightward = rightward;
  }
  else if(rect_equal(&screen->pending_scrollrect, &rect) &&
     ((screen->pending_scroll_downward  == 0 && downward  == 0) ||
      (screen->pending_scroll_rightward == 0 && rightward == 0))) {
    screen->pending_scroll_downward  += downward;
    screen->pending_scroll_rightward += rightward;
  }
  else {
    vterm_screen_flush_damage(screen);

    screen->pending_scrollrect = rect;
    screen->pending_scroll_downward  = downward;
    screen->pending_scroll_rightward = rightward;
  }

  vterm_scroll_rect(rect, downward, rightward,
      moverect_internal, erase_internal, screen);

  if(screen->damaged.start_row == -1)
    return 1;

  if(rect_contains(&rect, &screen->damaged)) {
    /* Scroll region entirely contains the damage; just move it */
    vterm_rect_move(&screen->damaged, -downward, -rightward);
    rect_clip(&screen->damaged, &rect);
  }
  /* There are a number of possible cases here, but lets restrict this to only
   * the common case where we might actually gain some performance by
   * optimising it. Namely, a vertical scroll that neatly cuts the damage
   * region in half.
   */
  else if(rect.start_col <= screen->damaged.start_col &&
          rect.end_col   >= screen->damaged.end_col &&
          rightward == 0) {
    if(screen->damaged.start_row >= rect.start_row &&
       screen->damaged.start_row  < rect.end_row) {
      screen->damaged.start_row -= downward;
      if(screen->damaged.start_row < rect.start_row)
        screen->damaged.start_row = rect.start_row;
      if(screen->damaged.start_row > rect.end_row)
        screen->damaged.start_row = rect.end_row;
    }
    if(screen->damaged.end_row >= rect.start_row &&
       screen->damaged.end_row  < rect.end_row) {
      screen->damaged.end_row -= downward;
      if(screen->damaged.end_row < rect.start_row)
        screen->damaged.end_row = rect.start_row;
      if(screen->damaged.end_row > rect.end_row)
        screen->damaged.end_row = rect.end_row;
    }
  }
  else {
    DEBUG_LOG("TODO: Just flush and redo damaged=" STRFrect " rect=" STRFrect "\n",
        ARGSrect(screen->damaged), ARGSrect(rect));
  }

  return 1;
}

static int movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->movecursor)
    return (*screen->callbacks->movecursor)(pos, oldpos, visible, screen->cbdata);

  return 0;
}

static int setpenattr(VTermAttr attr, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  screen->pen_dirty = true;

  switch(attr) {
  case VTERM_ATTR_BOLD:
    screen->pen.bold = val->boolean;
    return 1;
  case VTERM_ATTR_UNDERLINE:
    screen->pen.underline = val->number;
    return 1;
  case VTERM_ATTR_ITALIC:
    screen->pen.italic = val->boolean;
    return 1;
  case VTERM_ATTR_BLINK:
    screen->pen.blink = val->boolean;
    return 1;
  case VTERM_ATTR_REVERSE:
    screen->pen.reverse = val->boolean;
    return 1;
  case VTERM_ATTR_CONCEAL:
    screen->pen.conceal = val->boolean;
    return 1;
  case VTERM_ATTR_STRIKE:
    screen->pen.strike = val->boolean;
    return 1;
  case VTERM_ATTR_FONT:
    screen->pen.font = val->number;
    return 1;
  case VTERM_ATTR_FOREGROUND:
    screen->pen.fg = val->color;
    return 1;
  case VTERM_ATTR_BACKGROUND:
    screen->pen.bg = val->color;
    return 1;
  case VTERM_ATTR_SMALL:
    screen->pen.small = val->boolean;
    return 1;
  case VTERM_ATTR_BASELINE:
    screen->pen.baseline = val->number;
    return 1;
  case VTERM_ATTR_UNDERLINE_COLOR:
    screen->pen.uline_color = val->color;
    return 1;
  case VTERM_ATTR_LINK:
    screen->pen.link = (uint32_t)val->number;
    return 1;

  case VTERM_N_ATTRS:
    return 0;
  }

  return 0;
}

static int settermprop(VTermProp prop, VTermValue *val, void *user)
{
  VTermScreen *screen = user;

  switch(prop) {
  case VTERM_PROP_ALTSCREEN:
    if(val->boolean && !screen->buffers[BUFIDX_ALTSCREEN])
      return 0;

    screen->buffer = val->boolean ? screen->buffers[BUFIDX_ALTSCREEN] : screen->buffers[BUFIDX_PRIMARY];
    screen->row_ptrs_active = val->boolean ? screen->row_ptrs[BUFIDX_ALTSCREEN] : screen->row_ptrs[BUFIDX_PRIMARY];
    /* only send a damage event on disable; because during enable there's an
     * erase that sends a damage anyway
     */
    if(!val->boolean)
      damagescreen(screen);
    break;
  case VTERM_PROP_REVERSE:
    screen->global_reverse = val->boolean;
    damagescreen(screen);
    break;
  case VTERM_PROP_SYNCOUTPUT: // Handle Synchronized Output Mode updates
    screen->mode_synchronized = val->boolean;
    if(!val->boolean) {
      /* Mode disabled: paint the complete updated state to your emulator canvas */
      damagescreen(screen);
    }
    break;
  default:
    ; /* ignore */
  }

  if(screen->callbacks && screen->callbacks->settermprop)
    return (*screen->callbacks->settermprop)(prop, val, screen->cbdata);

  return 1;
}

static int bell(void *user)
{
  VTermScreen *screen = user;

  if(screen->callbacks && screen->callbacks->bell)
    return (*screen->callbacks->bell)(screen->cbdata);

  return 0;
}

/* How many cells are non-blank
 * Returns the position of the first blank cell in the trailing blank end */
static int line_popcount(ScreenCell *buffer, int row, int rows, int cols)
{
  int col = cols - 1;
  while(col >= 0 && buffer[row * cols + col].chars[0] == 0)
    col--;
  return col + 1;
}

/* Stores a public cell (as handed back by the scrollback) in the internal format */
static void cell_from_public(VTermScreen *screen, ScreenCell *dst, const VTermScreenCell *src)
{
  int i = 0;
  for( ; i < VTERM_MAX_CHARS_PER_CELL && src->chars[i]; i++)
    dst->chars[i] = src->chars[i];
  for( ; i < VTERM_MAX_CHARS_PER_CELL; i++)
    dst->chars[i] = 0;

  ScreenPen pen;
  memset(&pen, 0, sizeof(pen));
  pen.bold      = src->attrs.bold;
  pen.underline = src->attrs.underline;
  pen.italic    = src->attrs.italic;
  pen.blink     = src->attrs.blink;
  pen.reverse   = src->attrs.reverse ^ screen->global_reverse;
  pen.conceal   = src->attrs.conceal;
  pen.strike    = src->attrs.strike;
  pen.font      = src->attrs.font;
  pen.small     = src->attrs.small;
  pen.baseline  = src->attrs.baseline;

  pen.fg = src->fg;
  pen.bg = src->bg;
  pen.uline_color = src->uline;
  pen.link        = src->link;
  dst->pen = intern_pen(screen, &pen);
}

/* Lays a run of `n` cells (a logical line) out on rows `cols` wide and returns the number of rows
 * it takes. A double-width character that would straddle the right edge moves to the next row whole.
 * With `out` (room for that many rows) the cells are written there, blank padding included.
 * `src_cols` is the width the run was taken from (0 if it is one stream, with no padding).
 * `cursor_idx` is an index into the run: where that cell lands is returned in *cur_row and *cur_col,
 * an index past the end continues after the last cell. */
static int reflow_run(VTermScreen *screen, const ScreenCell *s, int n, int src_cols, int cols, ScreenCell *out,
    int cursor_idx, int *cur_row, int *cur_col)
{
  int row = 0, col = 0;
  for(int i = 0; i < n; i++) {
    /* the blank that ended a source row because the next character was a wide one is not text */
    if(src_cols && i % src_cols == src_cols - 1 && s[i].chars[0] == 0 && i + 2 < n && s[i + 2].chars[0] == (uint32_t)-1) {
      if(i == cursor_idx)
        cursor_idx = i + 1;
      continue;
    }
    bool wide = i + 1 < n && s[i + 1].chars[0] == (uint32_t)-1;
    if(col == cols || (wide && cols >= 2 && col == cols - 1)) {
      if(out)
        for( ; col < cols; col++)
          clearcell(screen, out + (size_t)row * cols + col);
      row++;
      col = 0;
    }
    if(i == cursor_idx)
      *cur_row = row, *cur_col = col;
    if(out)
      out[(size_t)row * cols + col] = s[i];
    col++;
  }
  if(cursor_idx >= n)
    *cur_row = row, *cur_col = col + (cursor_idx - n);
  if(out)
    for( ; col < cols; col++)
      clearcell(screen, out + (size_t)row * cols + col);
  return row + 1;
}

/* A line popped off the scrollback, oldest cell first */
typedef struct
{
  ScreenCell *cells;
  int n;
} PulledLine;

/* Takes whole logical lines off the end of the library scrollback, newest first, and returns them
 * as rows `old_cols` wide, oldest first, with their continuation flags: *rows_out rows in *cells_out
 * and *info_out. It takes the line the top screen row continues, if `top_continues`, and then lines until
 * they would fill `new_rows` rows at `new_cols` columns, so a resize can re-wrap them with the screen. */
static int pull_scrollback(VTermScreen *screen, int old_cols, int new_cols, int new_rows, bool top_continues,
    ScreenCell **cells_out, VTermLineInfo **info_out)
{
  VTermScrollback *sb = &screen->sb;
  PulledLine *lines = NULL;
  int nlines = 0, cap = 0;
  int height = 0, total_rows = 0;

  while(sb->count && (top_continues || height < new_rows)) {
    /* rows of one line, newest first, until the one that starts it */
    ScreenCell **rows = NULL;
    int *rowlen = NULL;
    int nrows = 0, rcap = 0, len = 0;
    bool cont;
    do {
      int cols = vterm_scrollback_newest_cols(sb);
      VTermScreenCell *tmp = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * (cols ? cols : 1));
      int n = vterm_scrollback_pop(sb, tmp, cols, screen->global_reverse, &cont);
      if(n < 0)
        n = 0;
      if(nrows == rcap) {
        int ncap = rcap ? rcap * 2 : 4;
        ScreenCell **nr = vterm_allocator_malloc(screen->vt, sizeof(*nr) * ncap);
        int *nl = vterm_allocator_malloc(screen->vt, sizeof(*nl) * ncap);
        if(rows) {
          memcpy(nr, rows, sizeof(*nr) * nrows);
          memcpy(nl, rowlen, sizeof(*nl) * nrows);
          vterm_allocator_free(screen->vt, rows);
          vterm_allocator_free(screen->vt, rowlen);
        }
        rows = nr, rowlen = nl, rcap = ncap;
      }
      rows[nrows] = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * (n ? n : 1));
      for(int c = 0; c < n; c++)
        cell_from_public(screen, &rows[nrows][c], &tmp[c]);
      rowlen[nrows] = n;
      len += n;
      nrows++;
      vterm_allocator_free(screen->vt, tmp);
    } while(cont && sb->count);

    PulledLine line;
    line.n = len;
    line.cells = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * (len ? len : 1));
    int at = len;
    for(int r = 0; r < nrows; r++) { /* newest row first: fill from the back */
      at -= rowlen[r];
      memcpy(line.cells + at, rows[r], sizeof(ScreenCell) * rowlen[r]);
      vterm_allocator_free(screen->vt, rows[r]);
    }
    vterm_allocator_free(screen->vt, rows);
    vterm_allocator_free(screen->vt, rowlen);

    if(nlines == cap) {
      int ncap = cap ? cap * 2 : 8;
      PulledLine *nl = vterm_allocator_malloc(screen->vt, sizeof(*nl) * ncap);
      if(lines) {
        memcpy(nl, lines, sizeof(*nl) * nlines);
        vterm_allocator_free(screen->vt, lines);
      }
      lines = nl, cap = ncap;
    }
    lines[nlines++] = line;

    height += reflow_run(screen, line.cells, line.n, 0, new_cols, NULL, -1, NULL, NULL);
    total_rows += reflow_run(screen, line.cells, line.n, 0, old_cols, NULL, -1, NULL, NULL);
    top_continues = false;
  }

  ScreenCell *cells = NULL;
  VTermLineInfo *info = NULL;
  if(total_rows) {
    cells = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * (size_t)total_rows * old_cols);
    info = vterm_allocator_malloc(screen->vt, sizeof(VTermLineInfo) * total_rows);
    int row = 0;
    for(int i = nlines - 1; i >= 0; i--) { /* oldest line is the last one popped */
      int nr = reflow_run(screen, lines[i].cells, lines[i].n, 0, old_cols, cells + (size_t)row * old_cols, -1, NULL, NULL);
      for(int r = 0; r < nr; r++) {
        info[row + r] = (VTermLineInfo){ 0 };
        info[row + r].continuation = r > 0;
      }
      row += nr;
    }
  }
  for(int i = 0; i < nlines; i++)
    vterm_allocator_free(screen->vt, lines[i].cells);
  if(lines)
    vterm_allocator_free(screen->vt, lines);

  *cells_out = cells;
  *info_out = info;
  return total_rows;
}

static void resize_buffer(VTermScreen *screen, int bufidx, int new_rows, int new_cols, bool active, VTermStateFields *statefields)
{
  int old_rows = screen->rows;
  /* the alternate screen belongs to a full-screen program, which redraws it itself */
  const bool reflow = screen->reflow && bufidx == BUFIDX_PRIMARY;
  int old_cols = screen->cols;

  /* Because full-width vertical scrolls just rotate row pointers, the physical
   * backing buffer may not be in logical row order. Flatten the buffer we are
   * resizing into a temporary logical-order copy, then discard the old backing. */
  ScreenCell *old_buffer_flat = screen->buffers[bufidx];
  ScreenCell **old_row_ptrs = screen->row_ptrs[bufidx];

  VTermLineInfo *orig_lineinfo = statefields->lineinfos[bufidx];
  VTermLineInfo *old_lineinfo = orig_lineinfo;

  /* With reflow and the library scrollback, the lines at the top of the screen are re-wrapped together
   * with the scrollback they continue: the rows pulled out of the scrollback are put in front of the screen
   * ones and handed back (as far as they do not fit) when the rows are pushed again. */
  bool pull_mode = reflow && active && screen->sb.max_lines;
  ScreenCell *pulled_cells = NULL;
  VTermLineInfo *pulled_info = NULL;
  int pulled = 0;
  if(pull_mode && screen->sb.count)
    pulled = pull_scrollback(screen, old_cols, new_cols, new_rows,
        old_lineinfo && old_lineinfo[0].continuation, &pulled_cells, &pulled_info);
  int rows_ext = old_rows + pulled;

  ScreenCell *old_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * (size_t)rows_ext * old_cols);
  if(pulled) {
    memcpy(old_buffer, pulled_cells, sizeof(ScreenCell) * (size_t)pulled * old_cols);
    old_lineinfo = vterm_allocator_malloc(screen->vt, sizeof(VTermLineInfo) * rows_ext);
    memcpy(old_lineinfo, pulled_info, sizeof(VTermLineInfo) * pulled);
    memcpy(old_lineinfo + pulled, orig_lineinfo, sizeof(VTermLineInfo) * old_rows);
    vterm_allocator_free(screen->vt, pulled_cells);
    vterm_allocator_free(screen->vt, pulled_info);
  }
  for(int row = 0; row < old_rows; row++) {
    ScreenCell *dst = old_buffer + (size_t)(pulled + row) * old_cols;
    if(old_row_ptrs[row])
      memcpy(dst, old_row_ptrs[row], old_cols * sizeof(ScreenCell));
    else
      memset(dst, 0, old_cols * sizeof(ScreenCell));
  }

  ScreenCell *new_buffer = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell) * new_rows * new_cols);
  VTermLineInfo *new_lineinfo = vterm_allocator_malloc(screen->vt, sizeof(new_lineinfo[0]) * new_rows);

  int old_row = rows_ext - 1;
  int new_row = new_rows - 1;

  VTermPos old_cursor = statefields->pos;
  VTermPos new_cursor = { -1, -1 };
  if(pulled)
    old_cursor.row += pulled;

#ifdef DEBUG_REFLOW
  fprintf(stderr, "Resizing from %dx%d to %dx%d; cursor was at (%d,%d)\n",
      old_cols, old_rows, new_cols, new_rows, old_cursor.col, old_cursor.row);
#endif

  /* Keep track of the final row that is knonw to be blank, so we know what
   * spare space we have for scrolling into
   */
  int final_blank_row = new_rows;

  while(old_row >= 0) {
    int old_row_end = old_row;
    /* TODO: Stop if dwl or dhl */
    while(reflow && old_lineinfo && old_row >= 0 && old_lineinfo[old_row].continuation)
      old_row--;
    /* The top row may itself continue a line that already scrolled off: the group starts there */
    if(old_row < 0)
      old_row = 0;
    int old_row_start = old_row;

    int width = 0;
    for(int row = old_row_start; row <= old_row_end; row++) {
      if(reflow && row < (rows_ext - 1) && old_lineinfo[row + 1].continuation)
        width += old_cols;
      else
        width += line_popcount(old_buffer, row, rows_ext, old_cols);
    }

    if(final_blank_row == (new_row + 1) && width == 0)
      final_blank_row = new_row;

    const ScreenCell *run = old_buffer + (size_t)old_row_start * old_cols;
    int new_height = reflow
      ? reflow_run(screen, run, width, old_cols, new_cols, NULL, -1, NULL, NULL)
      : 1;

    int new_row_end = new_row;
    int new_row_start = new_row - new_height + 1;

    old_row = old_row_start;
    int old_col = 0;

    int spare_rows = new_rows - final_blank_row;

    if(new_row_start < 0 && /* we'd fall off the top */
        spare_rows >= 0 && /* we actually have spare rows */
        (!active || new_cursor.row == -1 || (new_cursor.row - new_row_start) < new_rows))
    {
      /* Attempt to scroll content down into the blank rows at the bottom to
       * make it fit
       */
      int downwards = -new_row_start;
      if(downwards > spare_rows)
        downwards = spare_rows;
      int rowcount = new_rows - downwards;

#ifdef DEBUG_REFLOW
      fprintf(stderr, "  scroll %d rows +%d downwards\n", rowcount, downwards);
#endif

      memmove(&new_buffer[downwards * new_cols], &new_buffer[0],   rowcount * new_cols * sizeof(ScreenCell));
      memmove(&new_lineinfo[downwards],          &new_lineinfo[0], rowcount            * sizeof(new_lineinfo[0]));

      new_row += downwards;
      new_row_start += downwards;
      new_row_end += downwards;

      if(new_cursor.row >= 0)
        new_cursor.row += downwards;

      final_blank_row += downwards;
    }

#ifdef DEBUG_REFLOW
    fprintf(stderr, "  rows [%d..%d] <- [%d..%d] width=%d\n",
        new_row_start, new_row_end, old_row_start, old_row_end, width);
#endif

    if(new_row_start < 0) {
      /* the whole line goes to the scrollback */
      old_row = old_row_end;
      if(old_row_start <= old_cursor.row && old_cursor.row <= old_row_end) {
        new_cursor.row = 0;
        new_cursor.col = old_cursor.col;
        if(new_cursor.col >= new_cols)
          new_cursor.col = new_cols-1;
      }
      break;
    }

    if(reflow) {
      int cursor_idx = -1;
      if(active && old_row_start <= old_cursor.row && old_cursor.row <= old_row_end)
        cursor_idx = (old_cursor.row - old_row_start) * old_cols + old_cursor.col;
      int cur_row = 0, cur_col = 0;
      reflow_run(screen, run, width, old_cols, new_cols, new_buffer + (size_t)new_row_start * new_cols,
          cursor_idx, &cur_row, &cur_col);
      if(cursor_idx >= 0) {
        new_cursor.row = new_row_start + cur_row;
        new_cursor.col = cur_col < new_cols ? cur_col : new_cols - 1;
      }
      for(new_row = new_row_start; new_row <= new_row_end; new_row++)
        new_lineinfo[new_row].continuation = (new_row > new_row_start);

      old_row = old_row_start - 1;
      new_row = new_row_start - 1;
      continue;
    }

    for(new_row = new_row_start, old_row = old_row_start; new_row <= new_row_end; new_row++) {
      int count = width >= new_cols ? new_cols : width;
      width -= count;

      int new_col = 0;

      while(count) {
        /* TODO: This could surely be done a lot faster by memcpy()'ing the entire range */
        new_buffer[new_row * new_cols + new_col] = old_buffer[old_row * old_cols + old_col];

        if(old_cursor.row == old_row && old_cursor.col == old_col)
          new_cursor.row = new_row, new_cursor.col = new_col;

        old_col++;
        if(old_col == old_cols) {
          old_row++;

          if(!reflow) {
            new_col++;
            break;
          }
          old_col = 0;
        }

        new_col++;
        count--;
      }

      if(old_cursor.row == old_row && old_cursor.col >= old_col) {
        new_cursor.row = new_row, new_cursor.col = (old_cursor.col - old_col + new_col);
        if(new_cursor.col >= new_cols)
          new_cursor.col = new_cols-1;
      }

      while(new_col < new_cols) {
        clearcell(screen, &new_buffer[new_row * new_cols + new_col]);
        new_col++;
      }

      new_lineinfo[new_row].continuation = (new_row > new_row_start);
    }

    old_row = old_row_start - 1;
    new_row = new_row_start - 1;
  }

  if(old_cursor.row <= old_row) {
    /* cursor would have moved entirely off the top of the screen; lets just
     * bring it within range */
    new_cursor.row = 0, new_cursor.col = old_cursor.col;
    if(new_cursor.col >= new_cols)
      new_cursor.col = new_cols-1;
  }

  /* We really expect the cursor position to be set by now */
  if(active && (new_cursor.row == -1 || new_cursor.col == -1)) {
    /* The reflow lost track of the cursor (its row was folded away, for
     * instance): keep it on screen at the nearest cell. */
    new_cursor.row = old_cursor.row < new_rows ? old_cursor.row : new_rows - 1;
    new_cursor.col = old_cursor.col < new_cols ? old_cursor.col : new_cols - 1;
    if(new_cursor.row < 0) new_cursor.row = 0;
    if(new_cursor.col < 0) new_cursor.col = 0;
  }

  if(old_row >= 0 && bufidx == BUFIDX_PRIMARY) {
    /* Push spare lines to scrollback buffer */
    if(sb_wanted(screen))
      for(int row = 0; row <= old_row; row++) {
        const VTermLineInfo *lineinfo = old_lineinfo + row;
        sb_pushline_from_row(screen, old_buffer + (size_t)row * old_cols, lineinfo->continuation);
      }
    if(active)
      statefields->pos.row -= (old_row + 1);
  }
  if(new_row >= 0 && bufidx == BUFIDX_PRIMARY && !pull_mode &&
      (screen->sb.max_lines || (screen->callbacks && screen->callbacks->sb_popline))) {
    /* Try to backfill rows by popping scrollback buffer */
    while(new_row >= 0) {
      if(screen->sb.max_lines) {
        int n = vterm_scrollback_pop(&screen->sb, screen->sb_buffer, old_cols, screen->global_reverse, NULL);
        if(n < 0)
          break;
        if(n < old_cols) {
          VTermScreenCell blank;
          vterm_scrollback_blank_cell(&screen->sb, &blank, screen->global_reverse);
          for(int c = n; c < old_cols; c++)
            screen->sb_buffer[c] = blank;
        }
      }
      else if(!(screen->callbacks->sb_popline(old_cols, screen->sb_buffer, screen->cbdata)))
        break;

      VTermPos pos = { .row = new_row };
      for(pos.col = 0; pos.col < old_cols && pos.col < new_cols; pos.col += screen->sb_buffer[pos.col].width) {
        VTermScreenCell *src = &screen->sb_buffer[pos.col];
        ScreenCell *dst = &new_buffer[pos.row * new_cols + pos.col];

        cell_from_public(screen, dst, src);

        if(src->width == 2 && pos.col < (new_cols-1)) {
          memset((dst + 1)->chars, 0, sizeof((dst + 1)->chars));
          (dst + 1)->chars[0] = (uint32_t) -1;
        }
      }
      for( ; pos.col < new_cols; pos.col++)
        clearcell(screen, &new_buffer[pos.row * new_cols + pos.col]);
      new_row--;

      if(active)
        statefields->pos.row++;
    }
  }
  if(new_row >= 0) {
    /* Scroll new rows back up to the top and fill in blanks at the bottom */
    int moverows = new_rows - new_row - 1;
    memmove(&new_buffer[0], &new_buffer[(new_row + 1) * new_cols], moverows * new_cols * sizeof(ScreenCell));
    memmove(&new_lineinfo[0], &new_lineinfo[new_row + 1], moverows * sizeof(new_lineinfo[0]));

    new_cursor.row -= (new_row + 1);

    for(new_row = moverows; new_row < new_rows; new_row++) {
      for(int col = 0; col < new_cols; col++)
        clearcell(screen, &new_buffer[new_row * new_cols + col]);
      new_lineinfo[new_row] = (VTermLineInfo){ 0 };
    }
  }

  vterm_allocator_free(screen->vt, old_buffer);
  vterm_allocator_free(screen->vt, old_buffer_flat);
  screen->buffers[bufidx] = new_buffer;

  if(screen->row_ptrs[bufidx])
    vterm_allocator_free(screen->vt, screen->row_ptrs[bufidx]);

  ScreenCell **new_row_ptrs = vterm_allocator_malloc(screen->vt, sizeof(ScreenCell*) * new_rows);
  for(int row = 0; row < new_rows; row++) {
    new_row_ptrs[row] = new_buffer + (row * new_cols);
  }
  screen->row_ptrs[bufidx] = new_row_ptrs;
  if(active)
    screen->row_ptrs_active = new_row_ptrs;

  if(old_lineinfo != orig_lineinfo)
    vterm_allocator_free(screen->vt, old_lineinfo);
  vterm_allocator_free(screen->vt, orig_lineinfo);
  statefields->lineinfos[bufidx] = new_lineinfo;

  if(active)
    statefields->pos = new_cursor;

  return;
}

static int resize(int new_rows, int new_cols, VTermStateFields *fields, void *user)
{
  VTermScreen *screen = user;

  int altscreen_active = (screen->buffers[BUFIDX_ALTSCREEN] && screen->buffer == screen->buffers[BUFIDX_ALTSCREEN]);

  int old_rows = screen->rows;
  int old_cols = screen->cols;

  if(new_cols > old_cols) {
    /* Ensure that ->sb_buffer is large enough for a new or and old row */
    if(screen->sb_buffer)
      vterm_allocator_free(screen->vt, screen->sb_buffer);

    screen->sb_buffer = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * new_cols);
  }

  screen->pen_gc_lock++; /* the new buffers are not installed yet, the collector would not see their cells */
  resize_buffer(screen, 0, new_rows, new_cols, !altscreen_active, fields);
  if(screen->buffers[BUFIDX_ALTSCREEN])
    resize_buffer(screen, 1, new_rows, new_cols, altscreen_active, fields);
  screen->pen_gc_lock--;
  if(!screen->buffers[BUFIDX_ALTSCREEN])
  if(new_rows != old_rows) {
    /* We don't need a full resize of the altscreen because it isn't enabled
     * but we should at least keep the lineinfo the right size */
    vterm_allocator_free(screen->vt, fields->lineinfos[BUFIDX_ALTSCREEN]);

    VTermLineInfo *new_lineinfo = vterm_allocator_malloc(screen->vt, sizeof(new_lineinfo[0]) * new_rows);
    for(int row = 0; row < new_rows; row++)
      new_lineinfo[row] = (VTermLineInfo){ 0 };

    fields->lineinfos[BUFIDX_ALTSCREEN] = new_lineinfo;
  }

  screen->buffer = altscreen_active ? screen->buffers[BUFIDX_ALTSCREEN] : screen->buffers[BUFIDX_PRIMARY];
  screen->row_ptrs_active = altscreen_active ? screen->row_ptrs[BUFIDX_ALTSCREEN] : screen->row_ptrs[BUFIDX_PRIMARY];

  screen->rows = new_rows;
  screen->cols = new_cols;

  if(new_cols <= old_cols) {
    if(screen->sb_buffer)
      vterm_allocator_free(screen->vt, screen->sb_buffer);

    screen->sb_buffer = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * new_cols);
  }

  /* TODO: Maaaaybe we can optimise this if there's no reflow happening */
  damagescreen(screen);

  if(screen->callbacks && screen->callbacks->resize)
    return (*screen->callbacks->resize)(new_rows, new_cols, screen->cbdata);

  return 1;
}

static int setlineinfo(int row, const VTermLineInfo *newinfo, const VTermLineInfo *oldinfo, void *user)
{
  VTermScreen *screen = user;

  if(newinfo->doublewidth != oldinfo->doublewidth ||
     newinfo->doubleheight != oldinfo->doubleheight) {
    screen->pen_gc_lock++;
    uint32_t from = (uint32_t)-1, to = 0;
    for(int col = 0; col < screen->cols; col++) {
      ScreenCell *cell = getcell(screen, row, col);
      if(cell->pen != from) {
        ScreenPen pen = *cell_pen(screen, cell);
        pen.dwl = newinfo->doublewidth;
        pen.dhl = newinfo->doubleheight;
        from = cell->pen;
        to = intern_pen(screen, &pen);
      }
      cell->pen = to;
    }
    screen->pen_gc_lock--;

    VTermRect rect = {
      .start_row = row,
      .end_row   = row + 1,
      .start_col = 0,
      .end_col   = newinfo->doublewidth ? screen->cols / 2 : screen->cols,
    };
    damagerect(screen, rect);

    if(newinfo->doublewidth) {
      rect.start_col = screen->cols / 2;
      rect.end_col   = screen->cols;

      erase_internal(rect, 0, user);
    }
  }

  return 1;
}

static int sb_clear(void *user) {
  VTermScreen *screen = user;

  vterm_scrollback_clear(&screen->sb);

  if(screen->callbacks && screen->callbacks->sb_clear)
    if((*screen->callbacks->sb_clear)(screen->cbdata))
      return 1;

  return 0;
}

static VTermStateCallbacks state_cbs = {
  .putglyph    = &putglyph,
  .movecursor  = &movecursor,
  .premove     = &premove,
  .scrollrect  = &scrollrect,
  .erase       = &erase,
  .setpenattr  = &setpenattr,
  .settermprop = &settermprop,
  .bell        = &bell,
  .resize      = &resize,
  .setlineinfo = &setlineinfo,
  .sb_clear    = &sb_clear,
};

static VTermScreen *screen_new(VTerm *vt)
{
  VTermState *state = vterm_obtain_state(vt);
  if(!state)
    return NULL;

  VTermScreen *screen = vterm_allocator_malloc(vt, sizeof(VTermScreen));
  memset(screen, 0, sizeof(*screen));
  int rows, cols;

  vterm_get_size(vt, &rows, &cols);

  screen->vt = vt;
  screen->state = state;

  screen->damage_merge = VTERM_DAMAGE_CELL;
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;

  screen->rows = rows;
  screen->cols = cols;

  screen->global_reverse = false;
  screen->reflow = false;

  pens_resize_tables(screen, 256);
  screen->pen_dirty = true;

  screen->callbacks = NULL;
  screen->cbdata    = NULL;
  screen->callbacks_has_pushline4 = false;

  alloc_buffer(screen, BUFIDX_PRIMARY, rows, cols);

  screen->buffer = screen->buffers[BUFIDX_PRIMARY];
  screen->row_ptrs_active = screen->row_ptrs[BUFIDX_PRIMARY];

  screen->sb_buffer = vterm_allocator_malloc(screen->vt, sizeof(VTermScreenCell) * cols);
  vterm_scrollback_init(&screen->sb, vt);

  vterm_state_set_callbacks(screen->state, &state_cbs, screen);
  vterm_state_callbacks_has_premove(screen->state);

  return screen;
}

INTERNAL void vterm_screen_free(VTermScreen *screen)
{
  vterm_allocator_free(screen->vt, screen->buffers[BUFIDX_PRIMARY]);
  vterm_allocator_free(screen->vt, screen->row_ptrs[BUFIDX_PRIMARY]);
  if(screen->buffers[BUFIDX_ALTSCREEN]) {
    vterm_allocator_free(screen->vt, screen->buffers[BUFIDX_ALTSCREEN]);
    vterm_allocator_free(screen->vt, screen->row_ptrs[BUFIDX_ALTSCREEN]);
  }

  vterm_allocator_free(screen->vt, screen->sb_buffer);
  vterm_allocator_free(screen->vt, screen->pens);
  vterm_allocator_free(screen->vt, screen->pen_hash);
  vterm_scrollback_free(&screen->sb);

  vterm_allocator_free(screen->vt, screen);
}

void vterm_screen_reset(VTermScreen *screen, int hard)
{
  screen->damaged.start_row = -1;
  screen->pending_scrollrect.start_row = -1;
  vterm_state_reset(screen->state, hard);
  screen->pen_dirty = true;
  screen->sb.default_pen = screen->pen;
  screen->sb.default_pen_valid = true;
  vterm_screen_flush_damage(screen);
}

static size_t _get_chars(const VTermScreen *screen, const int utf8, void *buffer, size_t len, const VTermRect rect)
{
  size_t outpos = 0;
  int padding = 0;

#define PUT(c)                                             \
  if(utf8) {                                               \
    size_t thislen = utf8_seqlen(c);                       \
    if(buffer && outpos + thislen <= len)                  \
      outpos += fill_utf8((c), (char *)buffer + outpos);   \
    else                                                   \
      outpos += thislen;                                   \
  }                                                        \
  else {                                                   \
    if(buffer && outpos + 1 <= len)                        \
      ((uint32_t*)buffer)[outpos++] = (c);                 \
    else                                                   \
      outpos++;                                            \
  }

  for(int row = rect.start_row; row < rect.end_row; row++) {
    for(int col = rect.start_col; col < rect.end_col; col++) {
      ScreenCell *cell = getcell(screen, row, col);

      if(cell->chars[0] == 0)
        // Erased cell, might need a space
        padding++;
      else if(cell->chars[0] == (uint32_t)-1)
        // Gap behind a double-width char, do nothing
        ;
      else {
        while(padding) {
          PUT(UNICODE_SPACE);
          padding--;
        }
        for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; i++) {
          PUT(cell->chars[i]);
        }
      }
    }

    if(row < rect.end_row - 1) {
      PUT(UNICODE_LINEFEED);
      padding = 0;
    }
  }

  return outpos;
}

size_t vterm_screen_get_chars(const VTermScreen *screen, uint32_t *chars, size_t len, const VTermRect rect)
{
  return _get_chars(screen, 0, chars, len, rect);
}

size_t vterm_screen_get_text(const VTermScreen *screen, char *str, size_t len, const VTermRect rect)
{
  return _get_chars(screen, 1, str, len, rect);
}

/* Copy internal to external representation of a screen cell */
int vterm_screen_get_cell(const VTermScreen *screen, VTermPos pos, VTermScreenCell *cell)
{
  ScreenCell *intcell = getcell(screen, pos.row, pos.col);
  if(!intcell)
    return 0;

  for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL; i++) {
    cell->chars[i] = intcell->chars[i];
    if(!intcell->chars[i])
      break;
  }

  set_public_attrs(cell, cell_pen(screen, intcell), screen->global_reverse);

  if(pos.col < (screen->cols - 1) &&
     getcell(screen, pos.row, pos.col + 1)->chars[0] == (uint32_t)-1)
    cell->width = 2;
  else
    cell->width = 1;

  return 1;
}

/* Converts `limit` cells starting at `rowstart` to the public format */
static void convert_row(const VTermScreen *screen, const ScreenCell *rowstart, VTermScreenCell *cells, int limit)
{
  uint32_t last_pen = (uint32_t)-1;
  VTermScreenCell cached;
  memset(&cached, 0, sizeof(cached));
  int global_reverse = screen->global_reverse;

  for(int col = 0; col < limit; col++) {
    const ScreenCell *intcell = rowstart + col;
    VTermScreenCell *cell = &cells[col];

    /* Internal cells keep unused chars[] entries zeroed, so copy them all. */
    memcpy(cell->chars, intcell->chars, sizeof(cell->chars));

    if(intcell->pen != last_pen) {
      set_public_attrs(cell, cell_pen(screen, intcell), global_reverse);
      last_pen = intcell->pen;
      cached.attrs = cell->attrs;
      cached.fg = cell->fg;
      cached.bg = cell->bg;
      cached.uline = cell->uline;
      cached.link = cell->link;
    } else {
      cell->attrs = cached.attrs;
      cell->fg = cached.fg;
      cell->bg = cached.bg;
      cell->uline = cached.uline;
      cell->link = cached.link;
    }

    if(col < (screen->cols - 1) &&
       (rowstart + col + 1)->chars[0] == (uint32_t)-1)
      cell->width = 2;
    else
      cell->width = 1;
  }

}

int vterm_screen_get_row(const VTermScreen *screen, int row, VTermScreenCell *cells, int maxcols)
{
  if(row < 0 || row >= screen->rows || !cells || maxcols <= 0)
    return 0;

  int limit = maxcols < screen->cols ? maxcols : screen->cols;

  convert_row(screen, screen->row_ptrs_active[row], cells, limit);

  return limit;
}

int vterm_screen_is_eol(const VTermScreen *screen, VTermPos pos)
{
  /* This cell is EOL if this and every cell to the right is black */
  for(; pos.col < screen->cols; pos.col++) {
    ScreenCell *cell = getcell(screen, pos.row, pos.col);
    if(cell->chars[0] != 0)
      return 0;
  }

  return 1;
}

VTermScreen *vterm_obtain_screen(VTerm *vt)
{
  if(vt->screen)
    return vt->screen;

  VTermScreen *screen = screen_new(vt);
  vt->screen = screen;

  return screen;
}

const char *vterm_screen_get_hyperlink(const VTermScreen *screen, uint32_t id)
{
  return vterm_state_get_hyperlink(screen->state, id);
}

void vterm_screen_set_sb_trim(VTermScreen *screen, bool trim)
{
  screen->sb_trim = trim;
}

void vterm_screen_enable_reflow(VTermScreen *screen, bool reflow)
{
  screen->reflow = reflow;
}

#undef vterm_screen_set_reflow
void vterm_screen_set_reflow(VTermScreen *screen, bool reflow)
{
  vterm_screen_enable_reflow(screen, reflow);
}

void vterm_screen_enable_altscreen(VTermScreen *screen, int altscreen)
{
  if(!screen->buffers[BUFIDX_ALTSCREEN] && altscreen) {
    int rows, cols;
    vterm_get_size(screen->vt, &rows, &cols);

    alloc_buffer(screen, BUFIDX_ALTSCREEN, rows, cols);
  }
}

void vterm_screen_set_callbacks(VTermScreen *screen, const VTermScreenCallbacks *callbacks, void *user)
{
  screen->callbacks = callbacks;
  screen->cbdata = user;
}

void *vterm_screen_get_cbdata(VTermScreen *screen)
{
  return screen->cbdata;
}

void vterm_screen_callbacks_has_pushline4(VTermScreen *screen)
{
  screen->callbacks_has_pushline4 = true;
}

void vterm_screen_set_unrecognised_fallbacks(VTermScreen *screen, const VTermStateFallbacks *fallbacks, void *user)
{
  vterm_state_set_unrecognised_fallbacks(screen->state, fallbacks, user);
}

void *vterm_screen_get_unrecognised_fbdata(VTermScreen *screen)
{
  return vterm_state_get_unrecognised_fbdata(screen->state);
}

void vterm_screen_flush_damage(VTermScreen *screen)
{
  if(screen->pending_scrollrect.start_row != -1) {
    vterm_scroll_rect(screen->pending_scrollrect, screen->pending_scroll_downward, screen->pending_scroll_rightward,
        moverect_user, erase_user, screen);

    screen->pending_scrollrect.start_row = -1;
  }

  if(screen->damaged.start_row != -1) {
    if(screen->callbacks && screen->callbacks->damage)
      (*screen->callbacks->damage)(screen->damaged, screen->cbdata);

    screen->damaged.start_row = -1;
  }
}

void vterm_screen_set_damage_merge(VTermScreen *screen, VTermDamageSize size)
{
  vterm_screen_flush_damage(screen);
  screen->damage_merge = size;
}

static int attrs_differ(const VTermScreen *screen, VTermAttrMask attrs, ScreenCell *ca, ScreenCell *cb)
{
  const ScreenPen *a = cell_pen(screen, ca), *b = cell_pen(screen, cb);
  if(ca->pen == cb->pen)
    return 0;
  if((attrs & VTERM_ATTR_BOLD_MASK)       && (a->bold != b->bold))
    return 1;
  if((attrs & VTERM_ATTR_UNDERLINE_MASK)  && (a->underline != b->underline))
    return 1;
  if((attrs & VTERM_ATTR_ITALIC_MASK)     && (a->italic != b->italic))
    return 1;
  if((attrs & VTERM_ATTR_BLINK_MASK)      && (a->blink != b->blink))
    return 1;
  if((attrs & VTERM_ATTR_REVERSE_MASK)    && (a->reverse != b->reverse))
    return 1;
  if((attrs & VTERM_ATTR_CONCEAL_MASK)    && (a->conceal != b->conceal))
    return 1;
  if((attrs & VTERM_ATTR_STRIKE_MASK)     && (a->strike != b->strike))
    return 1;
  if((attrs & VTERM_ATTR_FONT_MASK)       && (a->font != b->font))
    return 1;
  if((attrs & VTERM_ATTR_FOREGROUND_MASK) && !vterm_color_is_equal(&a->fg, &b->fg))
    return 1;
  if((attrs & VTERM_ATTR_BACKGROUND_MASK) && !vterm_color_is_equal(&a->bg, &b->bg))
    return 1;
  if((attrs & VTERM_ATTR_SMALL_MASK)    && (a->small != b->small))
    return 1;
  if((attrs & VTERM_ATTR_BASELINE_MASK)    && (a->baseline != b->baseline))
    return 1;

  return 0;
}

int vterm_screen_get_attrs_extent(const VTermScreen *screen, VTermRect *extent, VTermPos pos, VTermAttrMask attrs)
{
  ScreenCell *target = getcell(screen, pos.row, pos.col);

  // TODO: bounds check
  extent->start_row = pos.row;
  extent->end_row   = pos.row + 1;

  if(extent->start_col < 0)
    extent->start_col = 0;
  if(extent->end_col < 0)
    extent->end_col = screen->cols;

  int col;

  for(col = pos.col - 1; col >= extent->start_col; col--)
    if(attrs_differ(screen, attrs, target, getcell(screen, pos.row, col)))
      break;
  extent->start_col = col + 1;

  for(col = pos.col + 1; col < extent->end_col; col++)
    if(attrs_differ(screen, attrs, target, getcell(screen, pos.row, col)))
      break;
  extent->end_col = col - 1;

  return 1;
}

void vterm_screen_convert_color_to_rgb(const VTermScreen *screen, VTermColor *col)
{
  vterm_state_convert_color_to_rgb(screen->state, col);
}

static void reset_default_colours(VTermScreen *screen, ScreenCell *buffer)
{
  uint32_t from = (uint32_t)-1, to = 0;
  screen->pen_gc_lock++;
  for(int i = 0; i < screen->rows * screen->cols; i++) {
    ScreenCell *cell = &buffer[i];
    if(cell->pen != from) {
      ScreenPen pen = *cell_pen(screen, cell);
      if(VTERM_COLOR_IS_DEFAULT_FG(&pen.fg))
        pen.fg = screen->pen.fg;
      if(VTERM_COLOR_IS_DEFAULT_BG(&pen.bg))
        pen.bg = screen->pen.bg;
      from = cell->pen;
      to = intern_pen(screen, &pen);
    }
    cell->pen = to;
  }
  screen->pen_gc_lock--;
}

void vterm_screen_set_default_colors(VTermScreen *screen, const VTermColor *default_fg, const VTermColor *default_bg)
{
  vterm_state_set_default_colors(screen->state, default_fg, default_bg);

  if(default_fg && VTERM_COLOR_IS_DEFAULT_FG(&screen->pen.fg)) {
    screen->pen.fg = *default_fg;
    screen->pen.fg.type = (screen->pen.fg.type & ~VTERM_COLOR_DEFAULT_MASK)
                        | VTERM_COLOR_DEFAULT_FG;
  }

  if(default_bg && VTERM_COLOR_IS_DEFAULT_BG(&screen->pen.bg)) {
    screen->pen.bg = *default_bg;
    screen->pen.bg.type = (screen->pen.bg.type & ~VTERM_COLOR_DEFAULT_MASK)
                        | VTERM_COLOR_DEFAULT_BG;
  }

  screen->pen_dirty = true;

  /* rows stored with the default pen follow the default colours, like the cells on screen */
  if(screen->sb.default_pen_valid) {
    if(VTERM_COLOR_IS_DEFAULT_FG(&screen->sb.default_pen.fg))
      screen->sb.default_pen.fg = screen->pen.fg;
    if(VTERM_COLOR_IS_DEFAULT_BG(&screen->sb.default_pen.bg))
      screen->sb.default_pen.bg = screen->pen.bg;
  }

  reset_default_colours(screen, screen->buffers[0]);
  if(screen->buffers[1])
    reset_default_colours(screen, screen->buffers[1]);
}

void vterm_screen_enable_scrollback(VTermScreen *screen, size_t max_lines, size_t max_bytes)
{
  vterm_scrollback_set_limits(&screen->sb, max_lines, max_bytes);
}

size_t vterm_screen_scrollback_count(const VTermScreen *screen)
{
  return screen->sb.count;
}

size_t vterm_screen_scrollback_bytes(const VTermScreen *screen)
{
  return screen->sb.bytes;
}

int vterm_screen_scrollback_get(const VTermScreen *screen, size_t idx, VTermScreenCell *cells, int maxcols, bool *continuation)
{
  int n = vterm_scrollback_get(&screen->sb, idx, cells, maxcols, screen->global_reverse, continuation);
  return n < 0 ? 0 : n;
}

void vterm_screen_scrollback_clear(VTermScreen *screen)
{
  vterm_scrollback_clear(&screen->sb);
}

void vterm_screen_scrollback_counters(const VTermScreen *screen, uint64_t *pushed, uint64_t *popped)
{
  if(pushed) *pushed = screen->sb.pushed;
  if(popped) *popped = screen->sb.popped;
}
