// drawline.c: Functions for drawing window lines on the screen.
// This is the middle level, drawscreen.c is the top and grid.c the lower level.

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvim/ascii.h"
#include "nvim/buffer.h"
#include "nvim/charset.h"
#include "nvim/cursor.h"
#include "nvim/cursor_shape.h"
#include "nvim/decoration.h"
#include "nvim/decoration_provider.h"
#include "nvim/diff.h"
#include "nvim/drawline.h"
#include "nvim/drawscreen.h"
#include "nvim/eval.h"
#include "nvim/extmark_defs.h"
#include "nvim/fold.h"
#include "nvim/globals.h"
#include "nvim/grid.h"
#include "nvim/highlight.h"
#include "nvim/highlight_group.h"
#include "nvim/indent.h"
#include "nvim/mark.h"
#include "nvim/match.h"
#include "nvim/mbyte.h"
#include "nvim/memline.h"
#include "nvim/memory.h"
#include "nvim/move.h"
#include "nvim/option.h"
#include "nvim/option_vars.h"
#include "nvim/plines.h"
#include "nvim/pos.h"
#include "nvim/quickfix.h"
#include "nvim/sign.h"
#include "nvim/spell.h"
#include "nvim/state.h"
#include "nvim/statusline.h"
#include "nvim/strings.h"
#include "nvim/syntax.h"
#include "nvim/terminal.h"
#include "nvim/types.h"
#include "nvim/ui.h"
#include "nvim/vim.h"

#define MB_FILLER_CHAR '<'  // character used when a double-width character
                            // doesn't fit.

/// possible draw states in win_line(), drawn in sequence.
typedef enum {
  WL_START = 0,  // nothing done yet
  WL_CMDLINE,    // cmdline window column
  WL_FOLD,       // 'foldcolumn'
  WL_SIGN,       // column for signs
  WL_NR,         // line number
  WL_STC,        // 'statuscolumn'
  WL_BRI,        // 'breakindent'
  WL_SBR,        // 'showbreak' or 'diff'
  WL_LINE,       // text in the line
} LineDrawState;

/// structure with variables passed between win_line() and other functions
typedef struct {
  LineDrawState draw_state;  ///< what to draw next

  linenr_T lnum;             ///< line number to be drawn
  foldinfo_T foldinfo;       ///< fold info for this line

  int startrow;              ///< first row in the window to be drawn
  int row;                   ///< row in the window, excl w_winrow

  colnr_T vcol;              ///< virtual column, before wrapping
  int col;                   ///< visual column on screen, after wrapping
  int boguscols;             ///< nonexistent columns added to "col" to force wrapping
  int vcol_off;              ///< offset for concealed characters

  int off;                   ///< offset relative start of line

  int cul_attr;              ///< set when 'cursorline' active
  int line_attr;             ///< attribute for the whole line
  int line_attr_lowprio;     ///< low-priority attribute for the line

  int fromcol;               ///< start of inverting
  int tocol;                 ///< end of inverting

  colnr_T vcol_sbr;          ///< virtual column after showbreak
  bool need_showbreak;       ///< overlong line, skipping first x chars

  int char_attr;             ///< attributes for next character

  int n_extra;               ///< number of extra bytes
  int n_attr;                ///< chars with special attr
  char *p_extra;             ///< string of extra chars, plus NUL, only used
                             ///< when c_extra and c_final are NUL
  int extra_attr;            ///< attributes for p_extra
  int c_extra;               ///< extra chars, all the same
  int c_final;               ///< final char, mandatory if set

  int n_closing;             ///< number of chars in fdc which will be closing

  bool extra_for_extmark;    ///< n_extra set for inline virtual text

  // saved "extra" items for when draw_state becomes WL_LINE (again)
  int saved_n_extra;
  char *saved_p_extra;
  bool saved_extra_for_extmark;
  int saved_c_extra;
  int saved_c_final;
  int saved_char_attr;

  char extra[57];            ///< sign, line number and 'fdc' must fit in here

  hlf_T diff_hlf;            ///< type of diff highlighting

  int n_virt_lines;          ///< nr of virtual lines
  int filler_lines;          ///< nr of filler lines to be drawn
  int filler_todo;           ///< nr of filler lines still to do + 1
  SignTextAttrs sattrs[SIGN_SHOW_MAX];  ///< sign attributes for the sign column
  /// do consider wrapping in linebreak mode only after encountering
  /// a non whitespace char
  bool need_lbr;

  VirtText virt_inline;
  size_t virt_inline_i;
  HlMode virt_inline_hl_mode;

  bool reset_extra_attr;

  int skip_cells;            ///< nr of cells to skip for w_leftcol
                             ///< or w_skipcol or concealing
  int skipped_cells;         ///< nr of skipped cells for virtual text
                             ///< to be added to wlv.vcol later
} winlinevars_T;

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "drawline.c.generated.h"
#endif

static char *extra_buf = NULL;
static size_t extra_buf_size = 0;

static char *get_extra_buf(size_t size)
{
  size = MAX(size, 64);
  if (extra_buf_size < size) {
    xfree(extra_buf);
    extra_buf = xmalloc(size);
    extra_buf_size = size;
  }
  return extra_buf;
}

#ifdef EXITFREE
void drawline_free_all_mem(void)
{
  xfree(extra_buf);
}
#endif

/// Advance **color_cols
///
/// @return  true when there are columns to draw.
static bool advance_color_col(int vcol, int **color_cols)
{
  while (**color_cols >= 0 && vcol > **color_cols) {
    (*color_cols)++;
  }
  return **color_cols >= 0;
}

/// Used when 'cursorlineopt' contains "screenline": compute the margins between
/// which the highlighting is used.
static void margin_columns_win(win_T *wp, int *left_col, int *right_col)
{
  // cache previous calculations depending on w_virtcol
  static int saved_w_virtcol;
  static win_T *prev_wp;
  static int prev_left_col;
  static int prev_right_col;
  static int prev_col_off;

  int cur_col_off = win_col_off(wp);
  int width1;
  int width2;

  if (saved_w_virtcol == wp->w_virtcol && prev_wp == wp
      && prev_col_off == cur_col_off) {
    *right_col = prev_right_col;
    *left_col = prev_left_col;
    return;
  }

  width1 = wp->w_width_inner - cur_col_off;
  width2 = width1 + win_col_off2(wp);

  *left_col = 0;
  *right_col = width1;

  if (wp->w_virtcol >= (colnr_T)width1) {
    *right_col = width1 + ((wp->w_virtcol - width1) / width2 + 1) * width2;
  }
  if (wp->w_virtcol >= (colnr_T)width1 && width2 > 0) {
    *left_col = (wp->w_virtcol - width1) / width2 * width2 + width1;
  }

  // cache values
  prev_left_col = *left_col;
  prev_right_col = *right_col;
  prev_wp = wp;
  saved_w_virtcol = wp->w_virtcol;
  prev_col_off = cur_col_off;
}

/// Put a single char from an UTF-8 buffer into a line buffer.
///
/// If `*pp` is a double-width char and only one cell is left, emit a space,
/// and don't advance *pp
///
/// Handles composing chars
static int line_putchar(buf_T *buf, const char **pp, schar_T *dest, int maxcells, int vcol)
{
  const char *p = *pp;
  int cells = utf_ptr2cells(p);
  int c_len = utfc_ptr2len(p);
  assert(maxcells > 0);
  if (cells > maxcells) {
    dest[0] = schar_from_ascii(' ');
    return 1;
  }

  if (*p == TAB) {
    cells = MIN(tabstop_padding(vcol, buf->b_p_ts, buf->b_p_vts_array), maxcells);
  }

  if (cells < maxcells && dest[cells] == 0) {
    dest[cells] = schar_from_ascii(' ');
  }
  if (*p == TAB) {
    for (int c = 0; c < cells; c++) {
      dest[c] = schar_from_ascii(' ');
    }
  } else {
    int u8c;
    dest[0] = utfc_ptr2schar(p, &u8c);
    if (cells > 1) {
      dest[1] = 0;
    }
  }

  *pp += c_len;
  return cells;
}

static void draw_virt_text(win_T *wp, buf_T *buf, int col_off, int *end_col, int win_row)
{
  DecorState *state = &decor_state;
  const int max_col = wp->w_grid.cols;
  int right_pos = max_col;
  bool do_eol = state->eol_col > -1;
  for (size_t i = 0; i < kv_size(state->active); i++) {
    DecorRange *item = &kv_A(state->active, i);
    if (!(item->start_row == state->row && decor_virt_pos(&item->decor))) {
      continue;
    }
    if (item->draw_col == -1) {
      bool updated = true;
      if (item->decor.virt_text_pos == kVTRightAlign) {
        right_pos -= item->decor.virt_text_width;
        item->draw_col = right_pos;
      } else if (item->decor.virt_text_pos == kVTEndOfLine && do_eol) {
        item->draw_col = state->eol_col;
      } else if (item->decor.virt_text_pos == kVTWinCol) {
        item->draw_col = MAX(col_off + item->decor.col, 0);
      } else {
        updated = false;
      }
      if (updated && (item->draw_col < 0 || item->draw_col >= wp->w_grid.cols)) {
        // Out of window, don't draw at all.
        item->draw_col = INT_MIN;
      }
    }
    if (item->draw_col < 0) {
      continue;
    }
    int col = 0;
    if (item->decor.ui_watched) {
      // send mark position to UI
      col = item->draw_col;
      WinExtmark m = { (NS)item->ns_id, item->mark_id, win_row, col };
      kv_push(win_extmark_arr, m);
    }
    if (kv_size(item->decor.virt_text)) {
      int vcol = item->draw_col - col_off;
      col = draw_virt_text_item(buf, item->draw_col, item->decor.virt_text,
                                item->decor.hl_mode, max_col, vcol);
    }
    item->draw_col = INT_MIN;  // deactivate
    if (item->decor.virt_text_pos == kVTEndOfLine && do_eol) {
      state->eol_col = col + 1;
    }

    *end_col = MAX(*end_col, col);
  }
}

static int draw_virt_text_item(buf_T *buf, int col, VirtText vt, HlMode hl_mode, int max_col,
                               int vcol)
{
  const char *p = "";
  int virt_attr = 0;
  size_t virt_pos = 0;

  while (col < max_col) {
    if (!*p) {
      if (virt_pos >= kv_size(vt)) {
        break;
      }
      virt_attr = 0;
      p = next_virt_text_chunk(vt, &virt_pos, &virt_attr);
      if (p == NULL) {
        break;
      }
    }
    if (*p == NUL) {
      continue;
    }
    int attr;
    bool through = false;
    if (hl_mode == kHlModeCombine) {
      attr = hl_combine_attr(linebuf_attr[col], virt_attr);
    } else if (hl_mode == kHlModeBlend) {
      through = (*p == ' ');
      attr = hl_blend_attrs(linebuf_attr[col], virt_attr, &through);
    } else {
      attr = virt_attr;
    }
    schar_T dummy[2];
    int maxcells = max_col - col;
    int cells = line_putchar(buf, &p, through ? dummy : &linebuf_char[col],
                             maxcells, vcol);
    for (int c = 0; c < cells; c++) {
      linebuf_attr[col] = attr;
      col++;
    }
    vcol += cells;
  }
  return col;
}

/// Return true if CursorLineSign highlight is to be used.
static bool use_cursor_line_highlight(win_T *wp, linenr_T lnum)
{
  return wp->w_p_cul
         && lnum == wp->w_cursorline
         && (wp->w_p_culopt_flags & CULOPT_NBR);
}

static char fdc_buf[MB_MAXCHAR * 10 + 1];

/// Setup for drawing the 'foldcolumn', if there is one.
static void handle_foldcolumn(win_T *wp, winlinevars_T *wlv)
{
  int fdc = compute_foldcolumn(wp, 0);
  if (fdc <= 0) {
    return;
  }

  // Use a separate buffer as `extra_buf` might be in use.
  wlv->n_extra = (int)fill_foldcolumn(fdc_buf, wp, wlv->foldinfo, wlv->lnum,
                                      &wlv->n_closing);
  fdc_buf[wlv->n_extra] = NUL;
  wlv->p_extra = fdc_buf;
  wlv->c_extra = NUL;
  wlv->c_final = NUL;
  if (use_cursor_line_highlight(wp, wlv->lnum)) {
    wlv->char_attr = win_hl_attr(wp, HLF_CLF);
  } else {
    wlv->char_attr = win_hl_attr(wp, HLF_FC);
  }
}

/// Fills the foldcolumn at "p" for window "wp".
/// Only to be called when 'foldcolumn' > 0.
///
/// @param[out] p  Char array to write into
/// @param lnum    Absolute current line number
/// @param closed  Whether it is in 'foldcolumn' mode
///
/// Assume monocell characters
/// @return number of chars added to \param p
size_t fill_foldcolumn(char *p, win_T *wp, foldinfo_T foldinfo, linenr_T lnum, int *n_closing)
{
  int i = 0;
  int fdc = compute_foldcolumn(wp, 0);    // available cell width
  size_t char_counter = 0;
  int symbol = 0;
  int len = 0;
  bool closed = foldinfo.fi_level != 0 && foldinfo.fi_lines > 0;
  // Init to all spaces.
  memset(p, ' ', MB_MAXCHAR * (size_t)fdc + 1);

  int level = foldinfo.fi_level;

  // If the column is too narrow, we start at the lowest level that
  // fits and use numbers to indicate the depth.
  int first_level = level - fdc - closed + 1;
  if (first_level < 1) {
    first_level = 1;
  }

  for (i = 0; i < MIN(fdc, level); i++) {
    if (foldinfo.fi_lnum == lnum
        && first_level + i >= foldinfo.fi_low_level) {
      symbol = wp->w_p_fcs_chars.foldopen;
    } else if (first_level == 1) {
      symbol = wp->w_p_fcs_chars.foldsep;
    } else if (first_level + i <= 9) {
      symbol = '0' + first_level + i;
    } else {
      symbol = '>';
    }

    len = utf_char2bytes(symbol, &p[char_counter]);
    char_counter += (size_t)len;
    if (first_level + i >= level) {
      i++;
      break;
    }
  }

  int n_closing_val = i;

  if (closed) {
    if (symbol != 0) {
      // rollback previous write
      char_counter -= (size_t)len;
      memset(&p[char_counter], ' ', (size_t)len);
      n_closing_val--;
    }
    len = utf_char2bytes(wp->w_p_fcs_chars.foldclosed, &p[char_counter]);
    char_counter += (size_t)len;
  }

  if (n_closing) {
    *n_closing = n_closing_val;
  }

  return MAX(char_counter + (size_t)(fdc - i), (size_t)fdc);
}

/// Get information needed to display the sign in line "wlv->lnum" in window "wp".
/// If "nrcol" is true, the sign is going to be displayed in the number column.
/// Otherwise the sign is going to be displayed in the sign column. If there is no
/// sign, draw blank cells instead.
static void get_sign_display_info(bool nrcol, win_T *wp, winlinevars_T *wlv, int sign_idx,
                                  int sign_cul_attr)
{
  SignTextAttrs sattr = wlv->sattrs[sign_idx];
  wlv->c_final = NUL;

  if (sattr.text && wlv->row == wlv->startrow + wlv->filler_lines && wlv->filler_todo <= 0) {
    size_t fill = nrcol ? (size_t)number_width(wp) - SIGN_WIDTH : 0;
    size_t sign_len = strlen(sattr.text);

    // Spaces + sign:    "  " + ">>"     + ' '
    wlv->n_extra = (int)(fill + sign_len + nrcol);
    if (nrcol) {
      memset(wlv->extra, ' ', (size_t)wlv->n_extra);
    }
    memcpy(wlv->extra + fill, sattr.text, sign_len);
    wlv->p_extra = wlv->extra;
    wlv->c_extra = NUL;
    wlv->char_attr = (use_cursor_line_highlight(wp, wlv->lnum) && sign_cul_attr)
                     ? sign_cul_attr : sattr.hl_id ? syn_id2attr(sattr.hl_id) : 0;
  } else {
    wlv->c_extra = ' ';
    wlv->n_extra = nrcol ? number_width(wp) + 1 : SIGN_WIDTH;
    if (!nrcol) {
      wlv->char_attr = win_hl_attr(wp, use_cursor_line_highlight(wp, wlv->lnum) ? HLF_CLS : HLF_SC);
    }
  }
}

static inline void get_line_number_str(win_T *wp, linenr_T lnum, char *buf, size_t buf_len)
{
  linenr_T num;
  char *fmt = "%*" PRIdLINENR " ";

  if (wp->w_p_nu && !wp->w_p_rnu) {
    // 'number' + 'norelativenumber'
    num = lnum;
  } else {
    // 'relativenumber', don't use negative numbers
    num = abs(get_cursor_rel_lnum(wp, lnum));
    if (num == 0 && wp->w_p_nu && wp->w_p_rnu) {
      // 'number' + 'relativenumber'
      num = lnum;
      fmt = "%-*" PRIdLINENR " ";
    }
  }

  snprintf(buf, buf_len, fmt, number_width(wp), num);
}

/// Return true if CursorLineNr highlight is to be used for the number column.
/// - 'cursorline' must be set
/// - "wlv->lnum" must be the cursor line
/// - 'cursorlineopt' has "number"
/// - don't highlight filler lines (when in diff mode)
/// - When line is wrapped and 'cursorlineopt' does not have "line", only highlight the line number
///   itself on the first screenline of the wrapped line, otherwise highlight the number column of
///   all screenlines of the wrapped line.
static bool use_cursor_line_nr(win_T *wp, winlinevars_T *wlv)
{
  return wp->w_p_cul
         && wlv->lnum == wp->w_cursorline
         && (wp->w_p_culopt_flags & CULOPT_NBR)
         && (wlv->row == wlv->startrow + wlv->filler_lines
             || (wlv->row > wlv->startrow + wlv->filler_lines
                 && (wp->w_p_culopt_flags & CULOPT_LINE)));
}

static int get_line_number_attr(win_T *wp, winlinevars_T *wlv)
{
  if (use_cursor_line_nr(wp, wlv)) {
    // TODO(vim): Can we use CursorLine instead of CursorLineNr
    // when CursorLineNr isn't set?
    return win_hl_attr(wp, HLF_CLN);
  }

  if (wp->w_p_rnu) {
    if (wlv->lnum < wp->w_cursor.lnum) {
      // Use LineNrAbove
      return win_hl_attr(wp, HLF_LNA);
    }
    if (wlv->lnum > wp->w_cursor.lnum) {
      // Use LineNrBelow
      return win_hl_attr(wp, HLF_LNB);
    }
  }

  return win_hl_attr(wp, HLF_N);
}

/// Display the absolute or relative line number.  After the first row fill with
/// blanks when the 'n' flag isn't in 'cpo'.
static void handle_lnum_col(win_T *wp, winlinevars_T *wlv, int sign_num_attr, int sign_cul_attr)
{
  bool has_cpo_n = vim_strchr(p_cpo, CPO_NUMCOL) != NULL;

  if ((wp->w_p_nu || wp->w_p_rnu)
      && (wlv->row == wlv->startrow + wlv->filler_lines || !has_cpo_n)
      // there is no line number in a wrapped line when "n" is in
      // 'cpoptions', but 'breakindent' assumes it anyway.
      && !((has_cpo_n && !wp->w_p_bri) && wp->w_skipcol > 0 && wlv->lnum == wp->w_topline)) {
    // If 'signcolumn' is set to 'number' and a sign is present in "lnum",
    // then display the sign instead of the line number.
    if (*wp->w_p_scl == 'n' && *(wp->w_p_scl + 1) == 'u' && wlv->sattrs[0].text) {
      get_sign_display_info(true, wp, wlv, 0, sign_cul_attr);
    } else {
      // Draw the line number (empty space after wrapping).
      if (wlv->row == wlv->startrow + wlv->filler_lines
          && (wp->w_skipcol == 0 || wlv->row > 0 || (wp->w_p_nu && wp->w_p_rnu))) {
        get_line_number_str(wp, wlv->lnum, wlv->extra, sizeof(wlv->extra));
        if (wp->w_skipcol > 0 && wlv->startrow == 0) {
          for (wlv->p_extra = wlv->extra; *wlv->p_extra == ' '; wlv->p_extra++) {
            *wlv->p_extra = '-';
          }
        }
        if (wp->w_p_rl) {  // reverse line numbers
          char *num = skipwhite(wlv->extra);
          rl_mirror_ascii(num, skiptowhite(num));
        }
        wlv->p_extra = wlv->extra;
        wlv->c_extra = NUL;
      } else {
        wlv->c_extra = ' ';
      }
      wlv->c_final = NUL;
      wlv->n_extra = number_width(wp) + 1;
      if (sign_num_attr > 0) {
        wlv->char_attr = sign_num_attr;
      } else {
        wlv->char_attr = get_line_number_attr(wp, wlv);
      }
    }
  }
}

/// Prepare and build the 'statuscolumn' string for line "lnum" in window "wp".
/// Fill "stcp" with the built status column string and attributes.
/// This can be called three times per win_line(), once for virt_lines, once for
/// the start of the buffer line "lnum" and once for the wrapped lines.
///
/// @param[out] stcp  Status column attributes
static void get_statuscol_str(win_T *wp, linenr_T lnum, int virtnum, statuscol_T *stcp)
{
  // When called for the first non-filler row of line "lnum" set num v:vars
  linenr_T relnum = virtnum == 0 ? abs(get_cursor_rel_lnum(wp, lnum)) : -1;

  // When a buffer's line count has changed, make a best estimate for the full
  // width of the status column by building with "w_nrwidth_line_count". Add
  // potentially truncated width and rebuild before drawing anything.
  if (wp->w_statuscol_line_count != wp->w_nrwidth_line_count) {
    wp->w_statuscol_line_count = wp->w_nrwidth_line_count;
    set_vim_var_nr(VV_VIRTNUM, 0);
    build_statuscol_str(wp, wp->w_nrwidth_line_count, 0, stcp);
    if (stcp->truncate > 0) {
      // Add truncated width to avoid unnecessary redraws
      int addwidth = MIN(stcp->truncate, MAX_NUMBERWIDTH - wp->w_nrwidth);
      stcp->truncate = 0;
      stcp->width += addwidth;
      wp->w_nrwidth += addwidth;
      wp->w_nrwidth_width = wp->w_nrwidth;
      wp->w_valid &= ~VALID_WCOL;
    }
  }
  set_vim_var_nr(VV_VIRTNUM, virtnum);

  int width = build_statuscol_str(wp, lnum, relnum, stcp);
  // Force a redraw in case of error or when truncated
  if (*wp->w_p_stc == NUL || (stcp->truncate > 0 && wp->w_nrwidth < MAX_NUMBERWIDTH)) {
    if (stcp->truncate > 0) {  // Avoid truncating 'statuscolumn'
      wp->w_nrwidth = MIN(MAX_NUMBERWIDTH, wp->w_nrwidth + stcp->truncate);
      wp->w_nrwidth_width = wp->w_nrwidth;
    } else {  // 'statuscolumn' reset due to error
      wp->w_nrwidth_line_count = 0;
      wp->w_nrwidth = (wp->w_p_nu || wp->w_p_rnu) * number_width(wp);
    }
    wp->w_redr_statuscol = true;
    return;
  }

  // Reset text/highlight pointer and current attr for new line
  stcp->textp = stcp->text;
  stcp->hlrecp = stcp->hlrec;
  stcp->cur_attr = stcp->num_attr;
  stcp->text_end = stcp->text + strlen(stcp->text);

  int fill = stcp->width - width;
  if (fill > 0) {
    // Fill up with ' '
    memset(stcp->text_end, ' ', (size_t)fill);
    *(stcp->text_end += fill) = NUL;
  }
}

/// Get information needed to display the next segment in the 'statuscolumn'.
/// If not yet at the end, prepare for next segment and decrement "wlv->draw_state".
///
/// @param stcp  Status column attributes
/// @param[in,out]  wlv
static void get_statuscol_display_info(statuscol_T *stcp, winlinevars_T *wlv)
{
  wlv->c_extra = NUL;
  wlv->c_final = NUL;
  do {
    wlv->draw_state = WL_STC;
    wlv->char_attr = stcp->cur_attr;
    wlv->p_extra = stcp->textp;
    char *const section_end = stcp->hlrecp->start ? stcp->hlrecp->start : stcp->text_end;
    wlv->n_extra = (int)(section_end - stcp->textp);
    // Prepare for next highlight section if not yet at the end
    if (section_end < stcp->text_end) {
      int hl = stcp->hlrecp->userhl;
      stcp->textp = stcp->hlrecp->start;
      stcp->cur_attr = hl < 0 ? syn_id2attr(-hl) : stcp->num_attr;
      stcp->hlrecp++;
      wlv->draw_state = WL_STC - 1;
    }
    // Skip over empty highlight sections
  } while (wlv->n_extra == 0 && stcp->textp < stcp->text_end);
  if (wlv->n_extra > 0) {
    static char transbuf[(MAX_NUMBERWIDTH + 9 + 9 * 2) * MB_MAXBYTES + 1];
    wlv->n_extra = (int)transstr_buf(wlv->p_extra, wlv->n_extra, transbuf, sizeof transbuf, true);
    wlv->p_extra = transbuf;
  }
}

static void handle_breakindent(win_T *wp, winlinevars_T *wlv)
{
  if (wp->w_briopt_sbr && wlv->draw_state == WL_BRI - 1
      && *get_showbreak_value(wp) != NUL) {
    // draw indent after showbreak value
    wlv->draw_state = WL_BRI;
  } else if (wp->w_briopt_sbr && wlv->draw_state == WL_SBR) {
    // after the showbreak, draw the breakindent
    wlv->draw_state = WL_BRI - 1;
  }

  // draw 'breakindent': indent wrapped text accordingly
  if (wlv->draw_state == WL_BRI - 1 && wlv->n_extra == 0) {
    wlv->draw_state = WL_BRI;
    // if wlv->need_showbreak is set, breakindent also applies
    if (wp->w_p_bri && (wlv->row != wlv->startrow || wlv->need_showbreak)
        && wlv->filler_lines == 0) {
      wlv->char_attr = 0;
      if (wlv->diff_hlf != (hlf_T)0) {
        wlv->char_attr = win_hl_attr(wp, (int)wlv->diff_hlf);
      }
      wlv->p_extra = NULL;
      wlv->c_extra = ' ';
      wlv->c_final = NUL;
      wlv->n_extra = get_breakindent_win(wp, ml_get_buf(wp->w_buffer, wlv->lnum));
      if (wlv->row == wlv->startrow) {
        wlv->n_extra -= win_col_off2(wp);
        if (wlv->n_extra < 0) {
          wlv->n_extra = 0;
        }
      }
      if (wp->w_skipcol > 0 && wlv->startrow == 0 && wp->w_p_wrap && wp->w_briopt_sbr) {
        wlv->need_showbreak = false;
      }
      // Correct end of highlighted area for 'breakindent',
      // required wen 'linebreak' is also set.
      if (wlv->tocol == wlv->vcol) {
        wlv->tocol += wlv->n_extra;
      }
    }
  }
}

static void handle_showbreak_and_filler(win_T *wp, winlinevars_T *wlv)
{
  if (wlv->filler_todo > wlv->filler_lines - wlv->n_virt_lines) {
    // TODO(bfredl): check this doesn't inhibit TUI-style
    //               clear-to-end-of-line.
    wlv->c_extra = ' ';
    wlv->c_final = NUL;
    wlv->n_extra = wp->w_grid.cols - wlv->col;
    wlv->char_attr = 0;
  } else if (wlv->filler_todo > 0) {
    // Draw "deleted" diff line(s)
    if (char2cells(wp->w_p_fcs_chars.diff) > 1) {
      wlv->c_extra = '-';
      wlv->c_final = NUL;
    } else {
      wlv->c_extra = wp->w_p_fcs_chars.diff;
      wlv->c_final = NUL;
    }
    wlv->n_extra = wp->w_grid.cols - wlv->col;
    wlv->char_attr = win_hl_attr(wp, HLF_DED);
  }

  char *const sbr = get_showbreak_value(wp);
  if (*sbr != NUL && wlv->need_showbreak) {
    // Draw 'showbreak' at the start of each broken line.
    wlv->p_extra = sbr;
    wlv->c_extra = NUL;
    wlv->c_final = NUL;
    wlv->n_extra = (int)strlen(sbr);
    wlv->char_attr = win_hl_attr(wp, HLF_AT);
    if (wp->w_skipcol == 0 || wlv->startrow != 0 || !wp->w_p_wrap) {
      wlv->need_showbreak = false;
    }
    wlv->vcol_sbr = wlv->vcol + mb_charlen(sbr);

    // Correct start of highlighted area for 'showbreak'.
    if (wlv->fromcol >= wlv->vcol && wlv->fromcol < wlv->vcol_sbr) {
      wlv->fromcol = wlv->vcol_sbr;
    }

    // Correct end of highlighted area for 'showbreak',
    // required when 'linebreak' is also set.
    if (wlv->tocol == wlv->vcol) {
      wlv->tocol += wlv->n_extra;
    }
    // Combine 'showbreak' with 'cursorline', prioritizing 'showbreak'.
    if (wlv->cul_attr) {
      wlv->char_attr = hl_combine_attr(wlv->cul_attr, wlv->char_attr);
    }
  }
}

static void apply_cursorline_highlight(win_T *wp, winlinevars_T *wlv)
{
  wlv->cul_attr = win_hl_attr(wp, HLF_CUL);
  HlAttrs ae = syn_attr2entry(wlv->cul_attr);
  // We make a compromise here (#7383):
  //  * low-priority CursorLine if fg is not set
  //  * high-priority ("same as Vim" priority) CursorLine if fg is set
  if (ae.rgb_fg_color == -1 && ae.cterm_fg_color == 0) {
    wlv->line_attr_lowprio = wlv->cul_attr;
  } else {
    if (!(State & MODE_INSERT) && bt_quickfix(wp->w_buffer)
        && qf_current_entry(wp) == wlv->lnum) {
      wlv->line_attr = hl_combine_attr(wlv->cul_attr, wlv->line_attr);
    } else {
      wlv->line_attr = wlv->cul_attr;
    }
  }
}

/// Checks if there is more inline virtual text that need to be drawn.
static bool has_more_inline_virt(winlinevars_T *wlv, ptrdiff_t v)
{
  if (wlv->virt_inline_i < kv_size(wlv->virt_inline)) {
    return true;
  }
  DecorState *state = &decor_state;
  for (size_t i = 0; i < kv_size(state->active); i++) {
    DecorRange *item = &kv_A(state->active, i);
    if (item->start_row != state->row
        || !kv_size(item->decor.virt_text)
        || item->decor.virt_text_pos != kVTInline
        || item->decor.virt_text_width == 0) {
      continue;
    }
    if (item->draw_col >= -1 && item->start_col >= v) {
      return true;
    }
  }
  return false;
}

static void handle_inline_virtual_text(win_T *wp, winlinevars_T *wlv, ptrdiff_t v)
{
  while (wlv->n_extra == 0) {
    if (wlv->virt_inline_i >= kv_size(wlv->virt_inline)) {
      // need to find inline virtual text
      wlv->virt_inline = VIRTTEXT_EMPTY;
      wlv->virt_inline_i = 0;
      DecorState *state = &decor_state;
      for (size_t i = 0; i < kv_size(state->active); i++) {
        DecorRange *item = &kv_A(state->active, i);
        if (item->start_row != state->row
            || !kv_size(item->decor.virt_text)
            || item->decor.virt_text_pos != kVTInline
            || item->decor.virt_text_width == 0) {
          continue;
        }
        if (item->draw_col >= -1 && item->start_col == v) {
          wlv->virt_inline = item->decor.virt_text;
          wlv->virt_inline_hl_mode = item->decor.hl_mode;
          item->draw_col = INT_MIN;
          break;
        }
      }
      if (!kv_size(wlv->virt_inline)) {
        // no more inline virtual text here
        break;
      }
    } else {
      // already inside existing inline virtual text with multiple chunks
      int attr = 0;
      char *text = next_virt_text_chunk(wlv->virt_inline, &wlv->virt_inline_i, &attr);
      if (text == NULL) {
        continue;
      }
      wlv->p_extra = text;
      wlv->n_extra = (int)strlen(text);
      if (wlv->n_extra == 0) {
        continue;
      }
      wlv->c_extra = NUL;
      wlv->c_final = NUL;
      wlv->extra_attr = attr;
      wlv->n_attr = mb_charlen(text);
      // If the text didn't reach until the first window
      // column we need to skip cells.
      if (wlv->skip_cells > 0) {
        // FIXME: this should use virt_text_width instead
        int virt_text_len = wlv->n_attr;
        if (virt_text_len > wlv->skip_cells) {
          int len = mb_charlen2bytelen(wlv->p_extra, wlv->skip_cells);
          wlv->n_extra -= len;
          wlv->p_extra += len;
          wlv->n_attr -= wlv->skip_cells;
          // Skipped cells needed to be accounted for in vcol.
          wlv->skipped_cells += wlv->skip_cells;
          wlv->skip_cells = 0;
        } else {
          // the whole text is left of the window, drop
          // it and advance to the next one
          wlv->skip_cells -= virt_text_len;
          // Skipped cells needed to be accounted for in vcol.
          wlv->skipped_cells += virt_text_len;
          wlv->n_attr = 0;
          wlv->n_extra = 0;
          // go to the start so the next virtual text chunk can be selected.
          continue;
        }
      }
      assert(wlv->n_extra > 0);
      wlv->extra_for_extmark = true;
    }
  }
}

static colnr_T get_trailcol(win_T *wp, const char *ptr, const char *line)
{
  colnr_T trailcol = MAXCOL;
  // find start of trailing whitespace
  if (wp->w_p_lcs_chars.trail) {
    trailcol = (colnr_T)strlen(ptr);
    while (trailcol > 0 && ascii_iswhite(ptr[trailcol - 1])) {
      trailcol--;
    }
    trailcol += (colnr_T)(ptr - line);
  }

  return trailcol;
}

static colnr_T get_leadcol(win_T *wp, const char *ptr, const char *line)
{
  colnr_T leadcol = 0;

  // find end of leading whitespace
  if (wp->w_p_lcs_chars.lead || wp->w_p_lcs_chars.leadmultispace != NULL) {
    leadcol = 0;
    while (ascii_iswhite(ptr[leadcol])) {
      leadcol++;
    }
    if (ptr[leadcol] == NUL) {
      // in a line full of spaces all of them are treated as trailing
      leadcol = 0;
    } else {
      // keep track of the first column not filled with spaces
      leadcol += (colnr_T)(ptr - line + 1);
    }
  }

  return leadcol;
}

/// Start a screen line at column zero.
static void win_line_start(win_T *wp, winlinevars_T *wlv, bool save_extra)
{
  wlv->col = 0;
  wlv->off = 0;
  wlv->need_lbr = false;

  if (save_extra) {
    // reset the drawing state for the start of a wrapped line
    wlv->draw_state = WL_START;
    wlv->saved_n_extra = wlv->n_extra;
    wlv->saved_p_extra = wlv->p_extra;
    wlv->saved_extra_for_extmark = wlv->extra_for_extmark;
    wlv->saved_c_extra = wlv->c_extra;
    wlv->saved_c_final = wlv->c_final;
    wlv->need_lbr = true;
    wlv->saved_char_attr = wlv->char_attr;

    wlv->n_extra = 0;
  }
}

/// Called when wlv->draw_state is set to WL_LINE.
static void win_line_continue(winlinevars_T *wlv)
{
  if (wlv->saved_n_extra > 0) {
    // Continue item from end of wrapped line.
    wlv->n_extra = wlv->saved_n_extra;
    wlv->saved_n_extra = 0;
    wlv->c_extra = wlv->saved_c_extra;
    wlv->c_final = wlv->saved_c_final;
    wlv->p_extra = wlv->saved_p_extra;
    wlv->extra_for_extmark = wlv->saved_extra_for_extmark;
    wlv->char_attr = wlv->saved_char_attr;
  } else {
    wlv->char_attr = 0;
  }
}

/// Display line "lnum" of window "wp" on the screen.
/// wp->w_virtcol needs to be valid.
///
/// @param lnum         line to display
/// @param startrow     first row relative to window grid
/// @param endrow       last grid row to be redrawn
/// @param number_only  only update the number column
/// @param spv          'spell' related variables kept between calls for "wp"
/// @param foldinfo     fold info for this line
/// @param[in, out] providers  decoration providers active this line
///                            items will be disables if they cause errors
///                            or explicitly return `false`.
///
/// @return             the number of last row the line occupies.
int win_line(win_T *wp, linenr_T lnum, int startrow, int endrow, bool number_only, spellvars_T *spv,
             foldinfo_T foldinfo, DecorProviders *providers)
{
  winlinevars_T wlv;                  // variables passed between functions

  colnr_T vcol_prev = -1;             // "wlv.vcol" of previous character
  char *line;                         // current line
  char *ptr;                          // current position in "line"
  ScreenGrid *grid = &wp->w_grid;     // grid specific to the window

  static char *at_end_str = "";       // used for p_extra when displaying curwin->w_p_lcs_chars.eol
                                      // at end-of-line
  const bool has_fold = foldinfo.fi_level != 0 && foldinfo.fi_lines > 0;

  int saved_attr2 = 0;                  // char_attr saved for n_attr
  int n_attr3 = 0;                      // chars with overruling special attr
  int saved_attr3 = 0;                  // char_attr saved for n_attr3

  int fromcol_prev = -2;                // start of inverting after cursor
  bool noinvcur = false;                // don't invert the cursor
  bool lnum_in_visual_area = false;
  pos_T pos;
  ptrdiff_t v;

  bool attr_pri = false;                // char_attr has priority
  bool area_highlighting = false;       // Visual or incsearch highlighting in this line
  int vi_attr = 0;                      // attributes for Visual and incsearch highlighting
  int area_attr = 0;                    // attributes desired by highlighting
  int search_attr = 0;                  // attributes desired by 'hlsearch'
  int vcol_save_attr = 0;               // saved attr for 'cursorcolumn'
  int decor_attr = 0;                   // attributes desired by syntax and extmarks
  bool has_syntax = false;              // this buffer has syntax highl.
  int folded_attr = 0;                  // attributes for folded line
  int save_did_emsg;
  int eol_hl_off = 0;                   // 1 if highlighted char after EOL
  bool draw_color_col = false;          // highlight colorcolumn
  int *color_cols = NULL;               // pointer to according columns array
#define SPWORDLEN 150
  char nextline[SPWORDLEN * 2];         // text with start of the next line
  int nextlinecol = 0;                  // column where nextline[] starts
  int nextline_idx = 0;                 // index in nextline[] where next line
                                        // starts
  int spell_attr = 0;                   // attributes desired by spelling
  int word_end = 0;                     // last byte with same spell_attr
  int cur_checked_col = 0;              // checked column for current line
  int extra_check = 0;                  // has syntax or linebreak
  int multi_attr = 0;                   // attributes desired by multibyte
  int mb_l = 1;                         // multi-byte byte length
  int mb_c = 0;                         // decoded multi-byte character
  schar_T mb_schar;                     // complete screen char
  int change_start = MAXCOL;            // first col of changed area
  int change_end = -1;                  // last col of changed area
  bool in_multispace = false;           // in multiple consecutive spaces
  int multispace_pos = 0;               // position in lcs-multispace string
  int line_attr_save;
  int line_attr_lowprio_save;

  bool search_attr_from_match = false;  // if search_attr is from :match
  bool has_decor = false;               // this buffer has decoration

  int saved_search_attr = 0;            // search_attr to be used when n_extra goes to zero
  int saved_area_attr = 0;              // idem for area_attr
  int saved_decor_attr = 0;             // idem for decor_attr
  bool saved_search_attr_from_match = false;

  int win_col_offset = 0;               // offset for window columns
  bool area_active = false;             // whether in Visual selection, for virtual text
  bool decor_need_recheck = false;      // call decor_recheck_draw_col() at next char

  char buf_fold[FOLD_TEXT_LEN];         // Hold value returned by get_foldtext
  VirtText fold_vt = VIRTTEXT_EMPTY;
  char *foldtext_free = NULL;

  // 'cursorlineopt' has "screenline" and cursor is in this line
  bool cul_screenline = false;
  // margin columns for the screen line, needed for when 'cursorlineopt'
  // contains "screenline"
  int left_curline_col = 0;
  int right_curline_col = 0;

  int match_conc = 0;              ///< cchar for match functions
  bool on_last_col = false;
  int syntax_flags = 0;
  int syntax_seqnr = 0;
  int prev_syntax_id = 0;
  int conceal_attr = win_hl_attr(wp, HLF_CONCEAL);
  bool is_concealing = false;
  int did_wcol = false;
  int old_boguscols = 0;
#define VCOL_HLC (wlv.vcol - wlv.vcol_off)
#define FIX_FOR_BOGUSCOLS \
  { \
    wlv.n_extra += wlv.vcol_off; \
    wlv.vcol -= wlv.vcol_off; \
    wlv.vcol_off = 0; \
    wlv.col -= wlv.boguscols; \
    old_boguscols = wlv.boguscols; \
    wlv.boguscols = 0; \
  }

  assert(startrow < endrow);

  CLEAR_FIELD(wlv);

  wlv.lnum = lnum;
  wlv.foldinfo = foldinfo;
  wlv.startrow = startrow;
  wlv.row = startrow;
  wlv.fromcol = -10;
  wlv.tocol = MAXCOL;
  wlv.vcol_sbr = -1;

  buf_T *buf = wp->w_buffer;
  const bool end_fill = (lnum == buf->b_ml.ml_line_count + 1);

  if (!number_only) {
    // To speed up the loop below, set extra_check when there is linebreak,
    // trailing white space and/or syntax processing to be done.
    extra_check = wp->w_p_lbr;
    if (syntax_present(wp) && !wp->w_s->b_syn_error && !wp->w_s->b_syn_slow
        && !has_fold && !end_fill) {
      // Prepare for syntax highlighting in this line.  When there is an
      // error, stop syntax highlighting.
      save_did_emsg = did_emsg;
      did_emsg = false;
      syntax_start(wp, lnum);
      if (did_emsg) {
        wp->w_s->b_syn_error = true;
      } else {
        did_emsg = save_did_emsg;
        if (!wp->w_s->b_syn_slow) {
          has_syntax = true;
          extra_check = true;
        }
      }
    }

    has_decor = decor_redraw_line(wp, lnum - 1, &decor_state);

    decor_providers_invoke_line(wp, providers, lnum - 1, &has_decor);

    if (has_decor) {
      extra_check = true;
    }

    // Check for columns to display for 'colorcolumn'.
    color_cols = wp->w_buffer->terminal ? NULL : wp->w_p_cc_cols;
    if (color_cols != NULL) {
      draw_color_col = advance_color_col(VCOL_HLC, &color_cols);
    }

    // handle Visual active in this window
    if (VIsual_active && wp->w_buffer == curwin->w_buffer) {
      pos_T *top, *bot;

      if (ltoreq(curwin->w_cursor, VIsual)) {
        // Visual is after curwin->w_cursor
        top = &curwin->w_cursor;
        bot = &VIsual;
      } else {
        // Visual is before curwin->w_cursor
        top = &VIsual;
        bot = &curwin->w_cursor;
      }
      lnum_in_visual_area = (lnum >= top->lnum && lnum <= bot->lnum);
      if (VIsual_mode == Ctrl_V) {
        // block mode
        if (lnum_in_visual_area) {
          wlv.fromcol = wp->w_old_cursor_fcol;
          wlv.tocol = wp->w_old_cursor_lcol;
        }
      } else {
        // non-block mode
        if (lnum > top->lnum && lnum <= bot->lnum) {
          wlv.fromcol = 0;
        } else if (lnum == top->lnum) {
          if (VIsual_mode == 'V') {       // linewise
            wlv.fromcol = 0;
          } else {
            getvvcol(wp, top, (colnr_T *)&wlv.fromcol, NULL, NULL);
            if (gchar_pos(top) == NUL) {
              wlv.tocol = wlv.fromcol + 1;
            }
          }
        }
        if (VIsual_mode != 'V' && lnum == bot->lnum) {
          if (*p_sel == 'e' && bot->col == 0
              && bot->coladd == 0) {
            wlv.fromcol = -10;
            wlv.tocol = MAXCOL;
          } else if (bot->col == MAXCOL) {
            wlv.tocol = MAXCOL;
          } else {
            pos = *bot;
            if (*p_sel == 'e') {
              getvvcol(wp, &pos, (colnr_T *)&wlv.tocol, NULL, NULL);
            } else {
              getvvcol(wp, &pos, NULL, NULL, (colnr_T *)&wlv.tocol);
              wlv.tocol++;
            }
          }
        }
      }

      // Check if the char under the cursor should be inverted (highlighted).
      if (!highlight_match && lnum == curwin->w_cursor.lnum && wp == curwin
          && cursor_is_block_during_visual(*p_sel == 'e')) {
        noinvcur = true;
      }

      // if inverting in this line set area_highlighting
      if (wlv.fromcol >= 0) {
        area_highlighting = true;
        vi_attr = win_hl_attr(wp, HLF_V);
      }
      // handle 'incsearch' and ":s///c" highlighting
    } else if (highlight_match
               && wp == curwin
               && !has_fold
               && lnum >= curwin->w_cursor.lnum
               && lnum <= curwin->w_cursor.lnum + search_match_lines) {
      if (lnum == curwin->w_cursor.lnum) {
        getvcol(curwin, &(curwin->w_cursor),
                (colnr_T *)&wlv.fromcol, NULL, NULL);
      } else {
        wlv.fromcol = 0;
      }
      if (lnum == curwin->w_cursor.lnum + search_match_lines) {
        pos.lnum = lnum;
        pos.col = search_match_endcol;
        getvcol(curwin, &pos, (colnr_T *)&wlv.tocol, NULL, NULL);
      }
      // do at least one character; happens when past end of line
      if (wlv.fromcol == wlv.tocol && search_match_endcol) {
        wlv.tocol = wlv.fromcol + 1;
      }
      area_highlighting = true;
      vi_attr = win_hl_attr(wp, HLF_I);
    }
  }

  int bg_attr = win_bg_attr(wp);

  int linestatus = 0;
  wlv.filler_lines = diff_check_with_linestatus(wp, lnum, &linestatus);
  if (wlv.filler_lines < 0 || linestatus < 0) {
    if (wlv.filler_lines == -1 || linestatus == -1) {
      if (diff_find_change(wp, lnum, &change_start, &change_end)) {
        wlv.diff_hlf = HLF_ADD;             // added line
      } else if (change_start == 0) {
        wlv.diff_hlf = HLF_TXD;             // changed text
      } else {
        wlv.diff_hlf = HLF_CHD;             // changed line
      }
    } else {
      wlv.diff_hlf = HLF_ADD;               // added line
    }
    if (linestatus == 0) {
      wlv.filler_lines = 0;
    }
    area_highlighting = true;
  }
  VirtLines virt_lines = KV_INITIAL_VALUE;
  wlv.n_virt_lines = decor_virt_lines(wp, lnum, &virt_lines, has_fold);
  wlv.filler_lines += wlv.n_virt_lines;
  if (lnum == wp->w_topline) {
    wlv.filler_lines = wp->w_topfill;
    wlv.n_virt_lines = MIN(wlv.n_virt_lines, wlv.filler_lines);
  }
  wlv.filler_todo = wlv.filler_lines;

  // Cursor line highlighting for 'cursorline' in the current window.
  if (wp->w_p_cul && wp->w_p_culopt_flags != CULOPT_NBR && lnum == wp->w_cursorline
      // Do not show the cursor line in the text when Visual mode is active,
      // because it's not clear what is selected then.
      && !(wp == curwin && VIsual_active)) {
    cul_screenline = (wp->w_p_wrap && (wp->w_p_culopt_flags & CULOPT_SCRLINE));
    if (!cul_screenline) {
      apply_cursorline_highlight(wp, &wlv);
    } else {
      margin_columns_win(wp, &left_curline_col, &right_curline_col);
    }
    area_highlighting = true;
  }

  int line_attr = 0;
  int sign_cul_attr = 0;
  int sign_num_attr = 0;
  // TODO(bfredl, vigoux): line_attr should not take priority over decoration!
  decor_redraw_signs(wp, buf, wlv.lnum - 1, wlv.sattrs, &line_attr, &sign_cul_attr, &sign_num_attr);

  statuscol_T statuscol = { 0 };
  if (*wp->w_p_stc != NUL) {
    // Draw the 'statuscolumn' if option is set.
    statuscol.draw = true;
    statuscol.sattrs = wlv.sattrs;
    statuscol.foldinfo = foldinfo;
    statuscol.width = win_col_off(wp) - (cmdwin_type != 0 && wp == curwin);
    statuscol.use_cul = use_cursor_line_highlight(wp, lnum);
    statuscol.sign_cul_id = statuscol.use_cul ? sign_cul_attr : 0;
    statuscol.num_attr = sign_num_attr > 0 ? syn_id2attr(sign_num_attr) : 0;
  } else {
    if (sign_cul_attr > 0) {
      sign_cul_attr = syn_id2attr(sign_cul_attr);
    }
    if (sign_num_attr > 0) {
      sign_num_attr = syn_id2attr(sign_num_attr);
    }
  }
  if (line_attr > 0) {
    wlv.line_attr = syn_id2attr(line_attr);
  }

  // Highlight the current line in the quickfix window.
  if (bt_quickfix(wp->w_buffer) && qf_current_entry(wp) == lnum) {
    wlv.line_attr = win_hl_attr(wp, HLF_QFL);
  }

  if (wlv.line_attr_lowprio || wlv.line_attr) {
    area_highlighting = true;
  }

  if (cul_screenline) {
    line_attr_save = wlv.line_attr;
    line_attr_lowprio_save = wlv.line_attr_lowprio;
  }

  if (spv->spv_has_spell && !number_only) {
    // Prepare for spell checking.
    extra_check = true;

    // When a word wrapped from the previous line the start of the
    // current line is valid.
    if (lnum == spv->spv_checked_lnum) {
      cur_checked_col = spv->spv_checked_col;
    }
    // Previous line was not spell checked, check for capital. This happens
    // for the first line in an updated region or after a closed fold.
    if (spv->spv_capcol_lnum == 0 && check_need_cap(wp, lnum, 0)) {
      spv->spv_cap_col = 0;
    } else if (lnum != spv->spv_capcol_lnum) {
      spv->spv_cap_col = -1;
    }
    spv->spv_checked_lnum = 0;

    // Get the start of the next line, so that words that wrap to the
    // next line are found too: "et<line-break>al.".
    // Trick: skip a few chars for C/shell/Vim comments
    nextline[SPWORDLEN] = NUL;
    if (lnum < wp->w_buffer->b_ml.ml_line_count) {
      line = ml_get_buf(wp->w_buffer, lnum + 1);
      spell_cat_line(nextline + SPWORDLEN, line, SPWORDLEN);
    }
    assert(!end_fill);
    line = ml_get_buf(wp->w_buffer, lnum);

    // If current line is empty, check first word in next line for capital.
    ptr = skipwhite(line);
    if (*ptr == NUL) {
      spv->spv_cap_col = 0;
      spv->spv_capcol_lnum = lnum + 1;
    } else if (spv->spv_cap_col == 0) {
      // For checking first word with a capital skip white space.
      spv->spv_cap_col = (int)(ptr - line);
    }

    // Copy the end of the current line into nextline[].
    if (nextline[SPWORDLEN] == NUL) {
      // No next line or it is empty.
      nextlinecol = MAXCOL;
      nextline_idx = 0;
    } else {
      v = (ptrdiff_t)strlen(line);
      if (v < SPWORDLEN) {
        // Short line, use it completely and append the start of the
        // next line.
        nextlinecol = 0;
        memmove(nextline, line, (size_t)v);
        STRMOVE(nextline + v, nextline + SPWORDLEN);
        nextline_idx = (int)v + 1;
      } else {
        // Long line, use only the last SPWORDLEN bytes.
        nextlinecol = (int)v - SPWORDLEN;
        memmove(nextline, line + nextlinecol, SPWORDLEN);
        nextline_idx = SPWORDLEN + 1;
      }
    }
  }

  line = end_fill ? "" : ml_get_buf(wp->w_buffer, lnum);
  ptr = line;

  colnr_T trailcol = MAXCOL;  // start of trailing spaces
  colnr_T leadcol = 0;        // start of leading spaces

  int lcs_eol_one = wp->w_p_lcs_chars.eol;     // 'eol'  until it's been used
  int lcs_prec_todo = wp->w_p_lcs_chars.prec;  // 'prec' until it's been used

  if (wp->w_p_list && !has_fold && !end_fill) {
    if (wp->w_p_lcs_chars.space
        || wp->w_p_lcs_chars.multispace != NULL
        || wp->w_p_lcs_chars.leadmultispace != NULL
        || wp->w_p_lcs_chars.trail
        || wp->w_p_lcs_chars.lead
        || wp->w_p_lcs_chars.nbsp) {
      extra_check = true;
    }
    trailcol = get_trailcol(wp, ptr, line);
    leadcol = get_leadcol(wp, ptr, line);
  }

  // 'nowrap' or 'wrap' and a single line that doesn't fit: Advance to the
  // first character to be displayed.
  if (wp->w_p_wrap) {
    v = startrow == 0 ? wp->w_skipcol : 0;
  } else {
    v = wp->w_leftcol;
  }
  if (v > 0 && !number_only) {
    char *prev_ptr = ptr;
    chartabsize_T cts;
    int charsize = 0;
    int head = 0;

    init_chartabsize_arg(&cts, wp, lnum, wlv.vcol, line, ptr);
    cts.cts_max_head_vcol = (int)v;
    while (cts.cts_vcol < v && *cts.cts_ptr != NUL) {
      head = 0;
      charsize = win_lbr_chartabsize(&cts, &head);
      cts.cts_vcol += charsize;
      prev_ptr = cts.cts_ptr;
      MB_PTR_ADV(cts.cts_ptr);
      if (wp->w_p_list) {
        in_multispace = *prev_ptr == ' ' && (*cts.cts_ptr == ' '
                                             || (prev_ptr > line && prev_ptr[-1] == ' '));
        if (!in_multispace) {
          multispace_pos = 0;
        } else if (cts.cts_ptr >= line + leadcol
                   && wp->w_p_lcs_chars.multispace != NULL) {
          multispace_pos++;
          if (wp->w_p_lcs_chars.multispace[multispace_pos] == NUL) {
            multispace_pos = 0;
          }
        } else if (cts.cts_ptr < line + leadcol
                   && wp->w_p_lcs_chars.leadmultispace != NULL) {
          multispace_pos++;
          if (wp->w_p_lcs_chars.leadmultispace[multispace_pos] == NUL) {
            multispace_pos = 0;
          }
        }
      }
    }
    wlv.vcol = cts.cts_vcol;
    ptr = cts.cts_ptr;
    clear_chartabsize_arg(&cts);

    // When:
    // - 'cuc' is set, or
    // - 'colorcolumn' is set, or
    // - 'virtualedit' is set, or
    // - the visual mode is active,
    // the end of the line may be before the start of the displayed part.
    if (wlv.vcol < v && (wp->w_p_cuc
                         || draw_color_col
                         || virtual_active()
                         || (VIsual_active && wp->w_buffer == curwin->w_buffer))) {
      wlv.vcol = (colnr_T)v;
    }

    // Handle a character that's not completely on the screen: Put ptr at
    // that character but skip the first few screen characters.
    if (wlv.vcol > v) {
      wlv.vcol -= charsize;
      ptr = prev_ptr;
    }

    if (v > wlv.vcol) {
      wlv.skip_cells = (int)v - wlv.vcol - head;
    }

    // Adjust for when the inverted text is before the screen,
    // and when the start of the inverted text is before the screen.
    if (wlv.tocol <= wlv.vcol) {
      wlv.fromcol = 0;
    } else if (wlv.fromcol >= 0 && wlv.fromcol < wlv.vcol) {
      wlv.fromcol = wlv.vcol;
    }

    // When w_skipcol is non-zero, first line needs 'showbreak'
    if (wp->w_p_wrap) {
      wlv.need_showbreak = true;
    }
    // When spell checking a word we need to figure out the start of the
    // word and if it's badly spelled or not.
    if (spv->spv_has_spell) {
      size_t len;
      colnr_T linecol = (colnr_T)(ptr - line);
      hlf_T spell_hlf = HLF_COUNT;

      pos = wp->w_cursor;
      wp->w_cursor.lnum = lnum;
      wp->w_cursor.col = linecol;
      len = spell_move_to(wp, FORWARD, true, true, &spell_hlf);

      // spell_move_to() may call ml_get() and make "line" invalid
      line = ml_get_buf(wp->w_buffer, lnum);
      ptr = line + linecol;

      if (len == 0 || (int)wp->w_cursor.col > ptr - line) {
        // no bad word found at line start, don't check until end of a
        // word
        spell_hlf = HLF_COUNT;
        word_end = (int)(spell_to_word_end(ptr, wp) - line + 1);
      } else {
        // bad word found, use attributes until end of word
        assert(len <= INT_MAX);
        word_end = wp->w_cursor.col + (int)len + 1;

        // Turn index into actual attributes.
        if (spell_hlf != HLF_COUNT) {
          spell_attr = highlight_attr[spell_hlf];
        }
      }
      wp->w_cursor = pos;

      // Need to restart syntax highlighting for this line.
      if (has_syntax) {
        syntax_start(wp, lnum);
      }
    }
  }

  // Correct highlighting for cursor that can't be disabled.
  // Avoids having to check this for each character.
  if (wlv.fromcol >= 0) {
    if (noinvcur) {
      if ((colnr_T)wlv.fromcol == wp->w_virtcol) {
        // highlighting starts at cursor, let it start just after the
        // cursor
        fromcol_prev = wlv.fromcol;
        wlv.fromcol = -1;
      } else if ((colnr_T)wlv.fromcol < wp->w_virtcol) {
        // restart highlighting after the cursor
        fromcol_prev = wp->w_virtcol;
      }
    }
    if (wlv.fromcol >= wlv.tocol) {
      wlv.fromcol = -1;
    }
  }

  if (!number_only && !has_fold && !end_fill) {
    v = ptr - line;
    area_highlighting |= prepare_search_hl_line(wp, lnum, (colnr_T)v,
                                                &line, &screen_search_hl, &search_attr,
                                                &search_attr_from_match);
    ptr = line + v;  // "line" may have been updated
  }

  win_line_start(wp, &wlv, false);

  // won't highlight after TERM_ATTRS_MAX columns
  int term_attrs[TERM_ATTRS_MAX] = { 0 };
  if (wp->w_buffer->terminal) {
    terminal_get_line_attributes(wp->w_buffer->terminal, wp, lnum, term_attrs);
    extra_check = true;
  }

  int sign_idx = 0;
  int virt_line_index;
  int virt_line_offset = -1;
  // Repeat for the whole displayed line.
  while (true) {
    int has_match_conc = 0;  ///< match wants to conceal
    int decor_conceal = 0;

    bool did_decrement_ptr = false;

    // Skip this quickly when working on the text.
    if (wlv.draw_state != WL_LINE) {
      if (cul_screenline) {
        wlv.cul_attr = 0;
        wlv.line_attr = line_attr_save;
        wlv.line_attr_lowprio = line_attr_lowprio_save;
      }

      if (wlv.draw_state == WL_CMDLINE - 1 && wlv.n_extra == 0) {
        wlv.draw_state = WL_CMDLINE;
        if (cmdwin_type != 0 && wp == curwin) {
          // Draw the cmdline character.
          wlv.n_extra = 1;
          wlv.c_extra = cmdwin_type;
          wlv.c_final = NUL;
          wlv.char_attr = win_hl_attr(wp, HLF_AT);
        }
      }

      if (wlv.draw_state == WL_FOLD - 1 && wlv.n_extra == 0) {
        if (wlv.filler_todo > 0) {
          int index = wlv.filler_todo - (wlv.filler_lines - wlv.n_virt_lines);
          if (index > 0) {
            virt_line_index = (int)kv_size(virt_lines) - index;
            assert(virt_line_index >= 0);
            virt_line_offset = kv_A(virt_lines, virt_line_index).left_col ? 0 : win_col_off(wp);
          }
        }
        if (virt_line_offset == 0) {
          // Skip the column states if there is a "virt_left_col" line.
          wlv.draw_state = WL_BRI - 1;
        } else if (statuscol.draw) {
          // Skip fold, sign and number states if 'statuscolumn' is set.
          wlv.draw_state = WL_STC - 1;
        }
      }

      if (wlv.draw_state == WL_FOLD - 1 && wlv.n_extra == 0) {
        wlv.draw_state = WL_FOLD;
        handle_foldcolumn(wp, &wlv);
      }

      // sign column, this is hit until sign_idx reaches count
      if (wlv.draw_state == WL_SIGN - 1 && wlv.n_extra == 0) {
        // Show the sign column when desired.
        wlv.draw_state = WL_SIGN;
        if (wp->w_scwidth > 0) {
          get_sign_display_info(false, wp, &wlv, sign_idx, sign_cul_attr);
          if (++sign_idx < wp->w_scwidth) {
            wlv.draw_state = WL_SIGN - 1;
          } else {
            sign_idx = 0;
          }
        }
      }

      if (wlv.draw_state == WL_NR - 1 && wlv.n_extra == 0) {
        // Show the line number, if desired.
        wlv.draw_state = WL_NR;
        handle_lnum_col(wp, &wlv, sign_num_attr, sign_cul_attr);
      }

      if (wlv.draw_state == WL_STC - 1 && wlv.n_extra == 0) {
        wlv.draw_state = WL_STC;
        // Draw the 'statuscolumn' if option is set.
        if (statuscol.draw) {
          if (sign_num_attr == 0) {
            statuscol.num_attr = get_line_number_attr(wp, &wlv);
          }
          if (statuscol.textp == NULL) {
            v = (ptr - line);
            get_statuscol_str(wp, lnum, wlv.row - startrow - wlv.filler_lines, &statuscol);
            if (!end_fill) {
              // Get the line again as evaluating 'statuscolumn' may free it.
              line = ml_get_buf(wp->w_buffer, lnum);
              ptr = line + v;
            }
            if (wp->w_redr_statuscol) {
              break;
            }
          }
          get_statuscol_display_info(&statuscol, &wlv);
        }
      }

      if (wlv.draw_state == WL_STC && wlv.n_extra == 0) {
        win_col_offset = wlv.off;
      }

      // Check if 'breakindent' applies and show it.
      // May change wlv.draw_state to WL_BRI or WL_BRI - 1.
      if (wlv.n_extra == 0) {
        handle_breakindent(wp, &wlv);
      }

      if (wlv.draw_state == WL_SBR - 1 && wlv.n_extra == 0) {
        wlv.draw_state = WL_SBR;
        handle_showbreak_and_filler(wp, &wlv);
      }

      if (wlv.draw_state == WL_LINE - 1 && wlv.n_extra == 0) {
        sign_idx = 0;
        wlv.draw_state = WL_LINE;
        if (has_decor && wlv.row == startrow + wlv.filler_lines) {
          // hide virt_text on text hidden by 'nowrap' or 'smoothscroll'
          decor_redraw_col(wp, (colnr_T)(ptr - line) - 1, wlv.off, true, &decor_state);
        }
        win_line_continue(&wlv);  // use wlv.saved_ values
      }
    }

    if (cul_screenline && wlv.draw_state == WL_LINE
        && wlv.vcol >= left_curline_col
        && wlv.vcol < right_curline_col) {
      apply_cursorline_highlight(wp, &wlv);
    }

    // When still displaying '$' of change command, stop at cursor
    if (((dollar_vcol >= 0
          && wp == curwin
          && lnum == wp->w_cursor.lnum
          && wlv.vcol >= wp->w_virtcol)
         || (number_only && wlv.draw_state > WL_STC))
        && wlv.filler_todo <= 0) {
      draw_virt_text(wp, buf, win_col_offset, &wlv.col, wlv.row);
      // don't clear anything after wlv.col
      win_put_linebuf(wp, wlv.row, 0, wlv.col, wlv.col, bg_attr, false);
      // Pretend we have finished updating the window.  Except when
      // 'cursorcolumn' is set.
      if (wp->w_p_cuc) {
        wlv.row = wp->w_cline_row + wp->w_cline_height;
      } else {
        wlv.row = grid->rows;
      }
      break;
    }

    const bool draw_folded = wlv.draw_state == WL_LINE && has_fold
                             && wlv.row == startrow + wlv.filler_lines;
    if (draw_folded && wlv.n_extra == 0) {
      wlv.char_attr = folded_attr = win_hl_attr(wp, HLF_FL);
    }

    int extmark_attr = 0;
    if (wlv.draw_state == WL_LINE
        && (area_highlighting || spv->spv_has_spell || extra_check)) {
      if (wlv.n_extra == 0 || !wlv.extra_for_extmark) {
        wlv.reset_extra_attr = false;
      }

      if (has_decor && wlv.n_extra == 0) {
        // Duplicate the Visual area check after this block,
        // but don't check inside p_extra here.
        if (wlv.vcol == wlv.fromcol
            || (wlv.vcol + 1 == wlv.fromcol
                && (wlv.n_extra == 0 && utf_ptr2cells(ptr) > 1))
            || (vcol_prev == fromcol_prev
                && vcol_prev < wlv.vcol
                && wlv.vcol < wlv.tocol)) {
          area_active = true;
        } else if (area_active
                   && (wlv.vcol == wlv.tocol
                       || (noinvcur && wlv.vcol == wp->w_virtcol))) {
          area_active = false;
        }

        bool selected = (area_active || (area_highlighting && noinvcur
                                         && wlv.vcol == wp->w_virtcol));
        if (decor_need_recheck) {
          decor_recheck_draw_col(wlv.off, selected, &decor_state);
          decor_need_recheck = false;
        }
        extmark_attr = decor_redraw_col(wp, (colnr_T)v, wlv.off, selected, &decor_state);

        if (!has_fold && wp->w_buffer->b_virt_text_inline > 0) {
          handle_inline_virtual_text(wp, &wlv, v);
          if (wlv.n_extra > 0 && wlv.virt_inline_hl_mode <= kHlModeReplace) {
            // restore search_attr and area_attr when n_extra is down to zero
            // TODO(bfredl): this is ugly as fuck. look if we can do this some other way.
            saved_search_attr = search_attr;
            saved_area_attr = area_attr;
            saved_decor_attr = decor_attr;
            saved_search_attr_from_match = search_attr_from_match;
            search_attr = 0;
            area_attr = 0;
            decor_attr = 0;
            search_attr_from_match = false;
          }
        }
      }

      int *area_attr_p
        = wlv.extra_for_extmark && wlv.virt_inline_hl_mode <= kHlModeReplace
          ? &saved_area_attr : &area_attr;

      // handle Visual or match highlighting in this line
      if (wlv.vcol == wlv.fromcol
          || (wlv.vcol + 1 == wlv.fromcol
              && ((wlv.n_extra == 0 && utf_ptr2cells(ptr) > 1)
                  || (wlv.n_extra > 0 && wlv.p_extra != NULL
                      && utf_ptr2cells(wlv.p_extra) > 1)))
          || (vcol_prev == fromcol_prev
              && vcol_prev < wlv.vcol               // not at margin
              && wlv.vcol < wlv.tocol)) {
        *area_attr_p = vi_attr;                     // start highlighting
        area_active = true;
      } else if (*area_attr_p != 0
                 && (wlv.vcol == wlv.tocol
                     || (noinvcur && wlv.vcol == wp->w_virtcol))) {
        *area_attr_p = 0;                           // stop highlighting
        area_active = false;
      }

      if (!has_fold && wlv.n_extra == 0) {
        // Check for start/end of 'hlsearch' and other matches.
        // After end, check for start/end of next match.
        // When another match, have to check for start again.
        v = (ptr - line);
        search_attr = update_search_hl(wp, lnum, (colnr_T)v, &line, &screen_search_hl,
                                       &has_match_conc, &match_conc, lcs_eol_one,
                                       &on_last_col, &search_attr_from_match);
        ptr = line + v;  // "line" may have been changed

        // Do not allow a conceal over EOL otherwise EOL will be missed
        // and bad things happen.
        if (*ptr == NUL) {
          has_match_conc = 0;
        }
      }

      if (wlv.diff_hlf != (hlf_T)0) {
        // When there is extra text (eg: virtual text) it gets the
        // diff highlighting for the line, but not for changed text.
        if (wlv.diff_hlf == HLF_CHD && ptr - line >= change_start
            && wlv.n_extra == 0) {
          wlv.diff_hlf = HLF_TXD;                   // changed text
        }
        if (wlv.diff_hlf == HLF_TXD && ((ptr - line > change_end && wlv.n_extra == 0)
                                        || (wlv.n_extra > 0 && wlv.extra_for_extmark))) {
          wlv.diff_hlf = HLF_CHD;                   // changed line
        }
        wlv.line_attr = win_hl_attr(wp, (int)wlv.diff_hlf);
        // Overlay CursorLine onto diff-mode highlight.
        if (wlv.cul_attr) {
          wlv.line_attr = 0 != wlv.line_attr_lowprio  // Low-priority CursorLine
                          ? hl_combine_attr(hl_combine_attr(wlv.cul_attr, wlv.line_attr),
                                            hl_get_underline())
                          : hl_combine_attr(wlv.line_attr, wlv.cul_attr);
        }
      }

      // Decide which of the highlight attributes to use.
      attr_pri = true;

      if (area_attr != 0) {
        wlv.char_attr = hl_combine_attr(wlv.line_attr, area_attr);
        if (!highlight_match) {
          // let search highlight show in Visual area if possible
          wlv.char_attr = hl_combine_attr(search_attr, wlv.char_attr);
        }
      } else if (search_attr != 0) {
        wlv.char_attr = hl_combine_attr(wlv.line_attr, search_attr);
      } else if (wlv.line_attr != 0
                 && ((wlv.fromcol == -10 && wlv.tocol == MAXCOL)
                     || wlv.vcol < wlv.fromcol
                     || vcol_prev < fromcol_prev
                     || wlv.vcol >= wlv.tocol)) {
        // Use wlv.line_attr when not in the Visual or 'incsearch' area
        // (area_attr may be 0 when "noinvcur" is set).
        wlv.char_attr = wlv.line_attr;
      } else {
        attr_pri = false;
        wlv.char_attr = decor_attr;
      }

      if (folded_attr != 0) {
        wlv.char_attr = hl_combine_attr(folded_attr, wlv.char_attr);
      }
    }

    if (draw_folded && wlv.n_extra == 0 && wlv.col == win_col_offset) {
      linenr_T lnume = lnum + foldinfo.fi_lines - 1;
      memset(buf_fold, ' ', FOLD_TEXT_LEN);
      wlv.p_extra = get_foldtext(wp, lnum, lnume, foldinfo, buf_fold, &fold_vt);
      wlv.n_extra = (int)strlen(wlv.p_extra);

      if (wlv.p_extra != buf_fold) {
        foldtext_free = wlv.p_extra;
      }
      wlv.c_extra = NUL;
      wlv.c_final = NUL;
      wlv.p_extra[wlv.n_extra] = NUL;

      // Get the line again as evaluating 'foldtext' may free it.
      line = ml_get_buf(wp->w_buffer, lnum);
      ptr = line + v;
    }

    if (draw_folded && wlv.n_extra == 0 && wlv.col < grid->cols) {
      // Fill rest of line with 'fold'.
      wlv.c_extra = wp->w_p_fcs_chars.fold;
      wlv.c_final = NUL;
      wlv.n_extra = grid->cols - wlv.col;
    }

    if (draw_folded && wlv.n_extra != 0 && wlv.col >= grid->cols) {
      // Truncate the folding.
      wlv.n_extra = 0;
    }

    // Get the next character to put on the screen.
    //
    // The "p_extra" points to the extra stuff that is inserted to
    // represent special characters (non-printable stuff) and other
    // things.  When all characters are the same, c_extra is used.
    // If c_final is set, it will compulsorily be used at the end.
    // "p_extra" must end in a NUL to avoid utfc_ptr2len() reads past
    // "p_extra[n_extra]".
    // For the '$' of the 'list' option, n_extra == 1, p_extra == "".
    if (wlv.n_extra > 0) {
      if (wlv.c_extra != NUL || (wlv.n_extra == 1 && wlv.c_final != NUL)) {
        mb_c = (wlv.n_extra == 1 && wlv.c_final != NUL) ? wlv.c_final : wlv.c_extra;
        mb_schar = schar_from_char(mb_c);
        wlv.n_extra--;
      } else {
        assert(wlv.p_extra != NULL);
        mb_l = utfc_ptr2len(wlv.p_extra);
        mb_schar = utfc_ptr2schar(wlv.p_extra, &mb_c);
        // mb_l=0 at the end-of-line NUL
        if (mb_l > wlv.n_extra || mb_l == 0) {
          mb_l = 1;
        }

        // If a double-width char doesn't fit display a '>' in the last column.
        // Don't advance the pointer but put the character at the start of the next line.
        if (wlv.col >= grid->cols - 1 && utf_char2cells(mb_c) == 2) {
          mb_c = '>';
          mb_l = 1;
          (void)mb_l;
          mb_schar = schar_from_ascii(mb_c);
          multi_attr = win_hl_attr(wp, HLF_AT);

          if (wlv.cul_attr) {
            multi_attr = 0 != wlv.line_attr_lowprio
                         ? hl_combine_attr(wlv.cul_attr, multi_attr)
                         : hl_combine_attr(multi_attr, wlv.cul_attr);
          }
        } else {
          wlv.n_extra -= mb_l;
          wlv.p_extra += mb_l;
        }
      }

      // Only restore search_attr and area_attr after "n_extra" in
      // the next screen line is also done.
      if (wlv.n_extra <= 0) {
        if (wlv.saved_n_extra <= 0) {
          if (search_attr == 0) {
            search_attr = saved_search_attr;
            saved_search_attr = 0;
          }
          if (area_attr == 0 && *ptr != NUL) {
            area_attr = saved_area_attr;
            saved_area_attr = 0;
          }
          if (decor_attr == 0) {
            decor_attr = saved_decor_attr;
            saved_decor_attr = 0;
          }

          if (wlv.extra_for_extmark) {
            // wlv.extra_attr should be used at this position but not
            // any further.
            wlv.reset_extra_attr = true;
          }
        }
        wlv.extra_for_extmark = false;
      }
    } else if (has_fold) {
      // skip writing the buffer line itself
      mb_c = NUL;
    } else {
      char *prev_ptr = ptr;

      // first byte of next char
      int c0 = (uint8_t)(*ptr);
      if (c0 == NUL) {
        // no more cells to skip
        wlv.skip_cells = 0;
      }

      // Get a character from the line itself.
      mb_l = utfc_ptr2len(ptr);
      mb_schar = utfc_ptr2schar(ptr, &mb_c);

      // Overlong encoded ASCII or ASCII with composing char
      // is displayed normally, except a NUL.
      if (mb_l > 1 && mb_c < 0x80) {
        c0 = mb_c;
      }

      if ((mb_l == 1 && c0 >= 0x80)
          || (mb_l >= 1 && mb_c == 0)
          || (mb_l > 1 && (!vim_isprintc(mb_c)))) {
        // Illegal UTF-8 byte: display as <xx>.
        // Non-printable character : display as ? or fullwidth ?.
        transchar_hex(wlv.extra, mb_c);
        if (wp->w_p_rl) {  // reverse
          rl_mirror_ascii(wlv.extra, NULL);
        }

        wlv.p_extra = wlv.extra;
        mb_c = mb_ptr2char_adv((const char **)&wlv.p_extra);
        mb_schar = schar_from_char(mb_c);
        wlv.n_extra = (int)strlen(wlv.p_extra);
        wlv.c_extra = NUL;
        wlv.c_final = NUL;
        if (area_attr == 0 && search_attr == 0) {
          wlv.n_attr = wlv.n_extra + 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_8);
          saved_attr2 = wlv.char_attr;               // save current attr
        }
      } else if (mb_l == 0) {        // at the NUL at end-of-line
        mb_l = 1;
      }
      // If a double-width char doesn't fit display a '>' in the
      // last column; the character is displayed at the start of the
      // next line.
      if (wlv.col >= grid->cols - 1 && utf_char2cells(mb_c) == 2) {
        mb_c = '>';
        mb_l = 1;
        mb_schar = schar_from_ascii(mb_c);
        multi_attr = win_hl_attr(wp, HLF_AT);
        // Put pointer back so that the character will be
        // displayed at the start of the next line.
        ptr--;
        did_decrement_ptr = true;
      } else if (*ptr != NUL) {
        ptr += mb_l - 1;
      }

      // If a double-width char doesn't fit at the left side display a '<' in
      // the first column.  Don't do this for unprintable characters.
      if (wlv.skip_cells > 0 && mb_l > 1 && wlv.n_extra == 0) {
        wlv.n_extra = 1;
        wlv.c_extra = MB_FILLER_CHAR;
        wlv.c_final = NUL;
        mb_c = ' ';
        mb_l = 1;
        mb_schar = schar_from_ascii(mb_c);
        if (area_attr == 0 && search_attr == 0) {
          wlv.n_attr = wlv.n_extra + 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_AT);
          saved_attr2 = wlv.char_attr;             // save current attr
        }
      }
      ptr++;

      decor_attr = 0;
      if (extra_check) {
        bool no_plain_buffer = (wp->w_s->b_p_spo_flags & SPO_NPBUFFER) != 0;
        bool can_spell = !no_plain_buffer;

        // Get extmark and syntax attributes, unless still at the start of the line
        // (double-wide char that doesn't fit).
        v = (ptr - line);
        if (has_syntax && v > 0) {
          // Get the syntax attribute for the character.  If there
          // is an error, disable syntax highlighting.
          save_did_emsg = did_emsg;
          did_emsg = false;

          decor_attr = get_syntax_attr((colnr_T)v - 1,
                                       spv->spv_has_spell ? &can_spell : NULL, false);

          if (did_emsg) {
            wp->w_s->b_syn_error = true;
            has_syntax = false;
          } else {
            did_emsg = save_did_emsg;
          }

          if (wp->w_s->b_syn_slow) {
            has_syntax = false;
          }

          // Need to get the line again, a multi-line regexp may
          // have made it invalid.
          line = ml_get_buf(wp->w_buffer, lnum);
          ptr = line + v;

          // no concealing past the end of the line, it interferes
          // with line highlighting.
          syntax_flags = (mb_c == 0) ? 0 : get_syntax_info(&syntax_seqnr);
        }

        if (has_decor && v > 0) {
          // extmarks take preceedence over syntax.c
          decor_attr = hl_combine_attr(decor_attr, extmark_attr);
          decor_conceal = decor_state.conceal;
          can_spell = TRISTATE_TO_BOOL(decor_state.spell, can_spell);
        }

        if (decor_attr) {
          if (!attr_pri) {
            if (wlv.cul_attr) {
              wlv.char_attr = 0 != wlv.line_attr_lowprio
                              ? hl_combine_attr(wlv.cul_attr, decor_attr)
                              : hl_combine_attr(decor_attr, wlv.cul_attr);
            } else {
              wlv.char_attr = decor_attr;
            }
          } else {
            wlv.char_attr = hl_combine_attr(decor_attr, wlv.char_attr);
          }
        } else if (!attr_pri) {
          wlv.char_attr = 0;
        }

        // Check spelling (unless at the end of the line).
        // Only do this when there is no syntax highlighting, the
        // @Spell cluster is not used or the current syntax item
        // contains the @Spell cluster.
        v = (ptr - line);
        if (spv->spv_has_spell && v >= word_end && v > cur_checked_col) {
          spell_attr = 0;
          // do not calculate cap_col at the end of the line or when
          // only white space is following
          if (mb_c != 0 && (*skipwhite(prev_ptr) != NUL) && can_spell) {
            char *p;
            hlf_T spell_hlf = HLF_COUNT;
            v -= mb_l - 1;

            // Use nextline[] if possible, it has the start of the
            // next line concatenated.
            if ((prev_ptr - line) - nextlinecol >= 0) {
              p = nextline + ((prev_ptr - line) - nextlinecol);
            } else {
              p = prev_ptr;
            }
            spv->spv_cap_col -= (int)(prev_ptr - line);
            size_t tmplen = spell_check(wp, p, &spell_hlf, &spv->spv_cap_col, spv->spv_unchanged);
            assert(tmplen <= INT_MAX);
            int len = (int)tmplen;
            word_end = (int)v + len;

            // In Insert mode only highlight a word that
            // doesn't touch the cursor.
            if (spell_hlf != HLF_COUNT
                && (State & MODE_INSERT)
                && wp->w_cursor.lnum == lnum
                && wp->w_cursor.col >=
                (colnr_T)(prev_ptr - line)
                && wp->w_cursor.col < (colnr_T)word_end) {
              spell_hlf = HLF_COUNT;
              spell_redraw_lnum = lnum;
            }

            if (spell_hlf == HLF_COUNT && p != prev_ptr
                && (p - nextline) + len > nextline_idx) {
              // Remember that the good word continues at the
              // start of the next line.
              spv->spv_checked_lnum = lnum + 1;
              spv->spv_checked_col = (int)((p - nextline) + len - nextline_idx);
            }

            // Turn index into actual attributes.
            if (spell_hlf != HLF_COUNT) {
              spell_attr = highlight_attr[spell_hlf];
            }

            if (spv->spv_cap_col > 0) {
              if (p != prev_ptr && (p - nextline) + spv->spv_cap_col >= nextline_idx) {
                // Remember that the word in the next line
                // must start with a capital.
                spv->spv_capcol_lnum = lnum + 1;
                spv->spv_cap_col = (int)((p - nextline) + spv->spv_cap_col - nextline_idx);
              } else {
                // Compute the actual column.
                spv->spv_cap_col += (int)(prev_ptr - line);
              }
            }
          }
        }
        if (spell_attr != 0) {
          if (!attr_pri) {
            wlv.char_attr = hl_combine_attr(wlv.char_attr, spell_attr);
          } else {
            wlv.char_attr = hl_combine_attr(spell_attr, wlv.char_attr);
          }
        }

        if (wp->w_buffer->terminal) {
          wlv.char_attr = hl_combine_attr(term_attrs[wlv.vcol], wlv.char_attr);
        }

        // we don't want linebreak to apply for lines that start with
        // leading spaces, followed by long letters (since it would add
        // a break at the beginning of a line and this might be unexpected)
        //
        // So only allow to linebreak, once we have found chars not in
        // 'breakat' in the line.
        if (wp->w_p_lbr && !wlv.need_lbr && mb_c != NUL
            && !vim_isbreak((uint8_t)(*ptr))) {
          wlv.need_lbr = true;
        }
        // Found last space before word: check for line break.
        if (wp->w_p_lbr && c0 == mb_c && mb_c < 128 && wlv.need_lbr
            && vim_isbreak(mb_c) && !vim_isbreak((uint8_t)(*ptr))) {
          int mb_off = utf_head_off(line, ptr - 1);
          char *p = ptr - (mb_off + 1);
          chartabsize_T cts;

          init_chartabsize_arg(&cts, wp, lnum, wlv.vcol, line, p);
          // do not want virtual text to be counted here
          cts.cts_has_virt_text = false;
          wlv.n_extra = win_lbr_chartabsize(&cts, NULL) - 1;
          clear_chartabsize_arg(&cts);

          if (on_last_col && mb_c != TAB) {
            // Do not continue search/match highlighting over the
            // line break, but for TABs the highlighting should
            // include the complete width of the character
            search_attr = 0;
          }

          if (mb_c == TAB && wlv.n_extra + wlv.col > grid->cols) {
            wlv.n_extra = tabstop_padding(wlv.vcol, wp->w_buffer->b_p_ts,
                                          wp->w_buffer->b_p_vts_array) - 1;
          }
          wlv.c_extra = mb_off > 0 ? MB_FILLER_CHAR : ' ';
          wlv.c_final = NUL;
          if (mb_c < 128 && ascii_iswhite(mb_c)) {
            if (mb_c == TAB) {
              // See "Tab alignment" below.
              FIX_FOR_BOGUSCOLS;
            }
            if (!wp->w_p_list) {
              mb_c = ' ';
              mb_schar = schar_from_ascii(mb_c);
            }
          }
        }

        if (wp->w_p_list) {
          in_multispace = mb_c == ' ' && (*ptr == ' ' || (prev_ptr > line && prev_ptr[-1] == ' '));
          if (!in_multispace) {
            multispace_pos = 0;
          }
        }

        // 'list': Change char 160 to 'nbsp' and space to 'space'.
        // But not when the character is followed by a composing
        // character (use mb_l to check that).
        if (wp->w_p_list
            && ((((mb_c == 160 && mb_l == 2) || (mb_c == 0x202f && mb_l == 3))
                 && wp->w_p_lcs_chars.nbsp)
                || (mb_c == ' '
                    && mb_l == 1
                    && (wp->w_p_lcs_chars.space
                        || (in_multispace && wp->w_p_lcs_chars.multispace != NULL))
                    && ptr - line >= leadcol
                    && ptr - line <= trailcol))) {
          if (in_multispace && wp->w_p_lcs_chars.multispace != NULL) {
            mb_c = wp->w_p_lcs_chars.multispace[multispace_pos++];
            if (wp->w_p_lcs_chars.multispace[multispace_pos] == NUL) {
              multispace_pos = 0;
            }
          } else {
            mb_c = (mb_c == ' ') ? wp->w_p_lcs_chars.space : wp->w_p_lcs_chars.nbsp;
          }
          wlv.n_attr = 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_0);
          saved_attr2 = wlv.char_attr;  // save current attr
          mb_schar = schar_from_char(mb_c);
        }

        if (mb_c == ' ' && mb_l == 1 && ((trailcol != MAXCOL && ptr > line + trailcol)
                                         || (leadcol != 0 && ptr < line + leadcol))) {
          if (leadcol != 0 && in_multispace && ptr < line + leadcol
              && wp->w_p_lcs_chars.leadmultispace != NULL) {
            mb_c = wp->w_p_lcs_chars.leadmultispace[multispace_pos++];
            if (wp->w_p_lcs_chars.leadmultispace[multispace_pos] == NUL) {
              multispace_pos = 0;
            }
          } else if (ptr > line + trailcol && wp->w_p_lcs_chars.trail) {
            mb_c = wp->w_p_lcs_chars.trail;
          } else if (ptr < line + leadcol && wp->w_p_lcs_chars.lead) {
            mb_c = wp->w_p_lcs_chars.lead;
          } else if (leadcol != 0 && wp->w_p_lcs_chars.space) {
            mb_c = wp->w_p_lcs_chars.space;
          }

          wlv.n_attr = 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_0);
          saved_attr2 = wlv.char_attr;  // save current attr
          mb_schar = schar_from_char(mb_c);
        }
      }

      // Handling of non-printable characters.
      if (!vim_isprintc(mb_c)) {
        // when getting a character from the file, we may have to
        // turn it into something else on the way to putting it on the screen.
        if (mb_c == TAB && (!wp->w_p_list || wp->w_p_lcs_chars.tab1)) {
          int tab_len = 0;
          colnr_T vcol_adjusted = wlv.vcol;  // removed showbreak length
          char *const sbr = get_showbreak_value(wp);

          // Only adjust the tab_len, when at the first column after the
          // showbreak value was drawn.
          if (*sbr != NUL && wlv.vcol == wlv.vcol_sbr && wp->w_p_wrap) {
            vcol_adjusted = wlv.vcol - mb_charlen(sbr);
          }
          // tab amount depends on current column
          tab_len = tabstop_padding(vcol_adjusted,
                                    wp->w_buffer->b_p_ts,
                                    wp->w_buffer->b_p_vts_array) - 1;

          if (!wp->w_p_lbr || !wp->w_p_list) {
            wlv.n_extra = tab_len;
          } else {
            char *p;
            int saved_nextra = wlv.n_extra;

            if (wlv.vcol_off > 0) {
              // there are characters to conceal
              tab_len += wlv.vcol_off;
            }
            // boguscols before FIX_FOR_BOGUSCOLS macro from above.
            if (wp->w_p_lcs_chars.tab1 && old_boguscols > 0
                && wlv.n_extra > tab_len) {
              tab_len += wlv.n_extra - tab_len;
            }

            if (tab_len > 0) {
              // If wlv.n_extra > 0, it gives the number of chars
              // to use for a tab, else we need to calculate the
              // width for a tab.
              int tab2_len = utf_char2len(wp->w_p_lcs_chars.tab2);
              int len = tab_len * tab2_len;
              if (wp->w_p_lcs_chars.tab3) {
                len += utf_char2len(wp->w_p_lcs_chars.tab3) - tab2_len;
              }
              if (wlv.n_extra > 0) {
                len += wlv.n_extra - tab_len;
              }
              mb_c = wp->w_p_lcs_chars.tab1;
              p = get_extra_buf((size_t)len + 1);
              memset(p, ' ', (size_t)len);
              p[len] = NUL;
              wlv.p_extra = p;
              for (int i = 0; i < tab_len; i++) {
                if (*p == NUL) {
                  tab_len = i;
                  break;
                }
                int lcs = wp->w_p_lcs_chars.tab2;

                // if tab3 is given, use it for the last char
                if (wp->w_p_lcs_chars.tab3 && i == tab_len - 1) {
                  lcs = wp->w_p_lcs_chars.tab3;
                }
                p += utf_char2bytes(lcs, p);
                wlv.n_extra += utf_char2len(lcs) - (saved_nextra > 0 ? 1 : 0);
              }

              // n_extra will be increased by FIX_FOX_BOGUSCOLS
              // macro below, so need to adjust for that here
              if (wlv.vcol_off > 0) {
                wlv.n_extra -= wlv.vcol_off;
              }
            }
          }

          {
            int vc_saved = wlv.vcol_off;

            // Tab alignment should be identical regardless of
            // 'conceallevel' value. So tab compensates of all
            // previous concealed characters, and thus resets
            // vcol_off and boguscols accumulated so far in the
            // line. Note that the tab can be longer than
            // 'tabstop' when there are concealed characters.
            FIX_FOR_BOGUSCOLS;

            // Make sure, the highlighting for the tab char will be
            // correctly set further below (effectively reverts the
            // FIX_FOR_BOGSUCOLS macro).
            if (wlv.n_extra == tab_len + vc_saved && wp->w_p_list
                && wp->w_p_lcs_chars.tab1) {
              tab_len += vc_saved;
            }
          }

          if (wp->w_p_list) {
            mb_c = (wlv.n_extra == 0 && wp->w_p_lcs_chars.tab3)
                   ? wp->w_p_lcs_chars.tab3 : wp->w_p_lcs_chars.tab1;
            if (wp->w_p_lbr && wlv.p_extra != NULL && *wlv.p_extra != NUL) {
              wlv.c_extra = NUL;  // using p_extra from above
            } else {
              wlv.c_extra = wp->w_p_lcs_chars.tab2;
            }
            wlv.c_final = wp->w_p_lcs_chars.tab3;
            wlv.n_attr = tab_len + 1;
            wlv.extra_attr = win_hl_attr(wp, HLF_0);
            saved_attr2 = wlv.char_attr;  // save current attr
          } else {
            wlv.c_final = NUL;
            wlv.c_extra = ' ';
            mb_c = ' ';
          }
          mb_schar = schar_from_char(mb_c);
        } else if (mb_c == NUL
                   && (wp->w_p_list
                       || ((wlv.fromcol >= 0 || fromcol_prev >= 0)
                           && wlv.tocol > wlv.vcol
                           && VIsual_mode != Ctrl_V
                           && wlv.col < grid->cols
                           && !(noinvcur
                                && lnum == wp->w_cursor.lnum
                                && wlv.vcol == wp->w_virtcol)))
                   && lcs_eol_one > 0) {
          // Display a '$' after the line or highlight an extra
          // character if the line break is included.
          // For a diff line the highlighting continues after the "$".
          if (wlv.diff_hlf == (hlf_T)0
              && wlv.line_attr == 0
              && wlv.line_attr_lowprio == 0) {
            // In virtualedit, visual selections may extend beyond end of line
            if (!(area_highlighting && virtual_active()
                  && wlv.tocol != MAXCOL && wlv.vcol < wlv.tocol)) {
              wlv.p_extra = at_end_str;
            }
            wlv.n_extra = 0;
          }
          if (wp->w_p_list && wp->w_p_lcs_chars.eol > 0) {
            mb_c = wp->w_p_lcs_chars.eol;
          } else {
            mb_c = ' ';
          }
          lcs_eol_one = -1;
          ptr--;  // put it back at the NUL
          wlv.extra_attr = win_hl_attr(wp, HLF_AT);
          wlv.n_attr = 1;
          mb_schar = schar_from_char(mb_c);
        } else if (mb_c != NUL) {
          wlv.p_extra = transchar_buf(wp->w_buffer, mb_c);
          if (wlv.n_extra == 0) {
            wlv.n_extra = byte2cells(mb_c) - 1;
          }
          if ((dy_flags & DY_UHEX) && wp->w_p_rl) {
            rl_mirror_ascii(wlv.p_extra, NULL);   // reverse "<12>"
          }
          wlv.c_extra = NUL;
          wlv.c_final = NUL;
          if (wp->w_p_lbr) {
            char *p;

            mb_c = (uint8_t)(*wlv.p_extra);
            p = get_extra_buf((size_t)wlv.n_extra + 1);
            memset(p, ' ', (size_t)wlv.n_extra);
            strncpy(p,  // NOLINT(runtime/printf)
                    wlv.p_extra + 1,
                    (size_t)strlen(wlv.p_extra) - 1);
            p[wlv.n_extra] = NUL;
            wlv.p_extra = p;
          } else {
            wlv.n_extra = byte2cells(mb_c) - 1;
            mb_c = (uint8_t)(*wlv.p_extra++);
          }
          wlv.n_attr = wlv.n_extra + 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_8);
          saved_attr2 = wlv.char_attr;  // save current attr
          mb_schar = schar_from_ascii(mb_c);
        } else if (VIsual_active
                   && (VIsual_mode == Ctrl_V || VIsual_mode == 'v')
                   && virtual_active()
                   && wlv.tocol != MAXCOL
                   && wlv.vcol < wlv.tocol
                   && wlv.col < grid->cols) {
          mb_c = ' ';
          mb_schar = schar_from_char(mb_c);
          ptr--;  // put it back at the NUL
        }
      }

      if (wp->w_p_cole > 0
          && (wp != curwin || lnum != wp->w_cursor.lnum || conceal_cursor_line(wp))
          && ((syntax_flags & HL_CONCEAL) != 0 || has_match_conc > 0 || decor_conceal > 0)
          && !(lnum_in_visual_area && vim_strchr(wp->w_p_cocu, 'v') == NULL)) {
        wlv.char_attr = conceal_attr;
        if (((prev_syntax_id != syntax_seqnr && (syntax_flags & HL_CONCEAL) != 0)
             || has_match_conc > 1 || decor_conceal > 1)
            && (syn_get_sub_char() != NUL
                || (has_match_conc && match_conc)
                || (decor_conceal && decor_state.conceal_char)
                || wp->w_p_cole == 1)
            && wp->w_p_cole != 3) {
          // First time at this concealed item: display one
          // character.
          if (has_match_conc && match_conc) {
            mb_c = match_conc;
          } else if (decor_conceal && decor_state.conceal_char) {
            mb_c = decor_state.conceal_char;
            if (decor_state.conceal_attr) {
              wlv.char_attr = decor_state.conceal_attr;
            }
          } else if (syn_get_sub_char() != NUL) {
            mb_c = syn_get_sub_char();
          } else if (wp->w_p_lcs_chars.conceal != NUL) {
            mb_c = wp->w_p_lcs_chars.conceal;
          } else {
            mb_c = ' ';
          }

          prev_syntax_id = syntax_seqnr;

          if (wlv.n_extra > 0) {
            wlv.vcol_off += wlv.n_extra;
          }
          wlv.vcol += wlv.n_extra;
          if (wp->w_p_wrap && wlv.n_extra > 0) {
            wlv.boguscols += wlv.n_extra;
            wlv.col += wlv.n_extra;
          }
          wlv.n_extra = 0;
          wlv.n_attr = 0;
        } else if (wlv.skip_cells == 0) {
          is_concealing = true;
          wlv.skip_cells = 1;
        }
        mb_schar = schar_from_char(mb_c);
      } else {
        prev_syntax_id = 0;
        is_concealing = false;
      }

      if (wlv.skip_cells > 0 && did_decrement_ptr) {
        // not showing the '>', put pointer back to avoid getting stuck
        ptr++;
      }
    }  // end of printing from buffer content

    // In the cursor line and we may be concealing characters: correct
    // the cursor column when we reach its position.
    if (!did_wcol && wlv.draw_state == WL_LINE
        && wp == curwin && lnum == wp->w_cursor.lnum
        && conceal_cursor_line(wp)
        && (int)wp->w_virtcol <= wlv.vcol + wlv.skip_cells) {
      wp->w_wcol = wlv.col - wlv.boguscols;
      wp->w_wrow = wlv.row;
      did_wcol = true;
      wp->w_valid |= VALID_WCOL|VALID_WROW|VALID_VIRTCOL;
    }

    // Don't override visual selection highlighting.
    if (wlv.n_attr > 0 && wlv.draw_state == WL_LINE && !search_attr_from_match) {
      wlv.char_attr = hl_combine_attr(wlv.char_attr, wlv.extra_attr);
      if (wlv.reset_extra_attr) {
        wlv.reset_extra_attr = false;
        wlv.extra_attr = 0;
        // search_attr_from_match can be restored now that the extra_attr has been applied
        search_attr_from_match = saved_search_attr_from_match;
      }
    }

    // Handle the case where we are in column 0 but not on the first
    // character of the line and the user wants us to show us a
    // special character (via 'listchars' option "precedes:<char>".
    if (lcs_prec_todo != NUL
        && wp->w_p_list
        && (wp->w_p_wrap ? (wp->w_skipcol > 0 && wlv.row == 0) : wp->w_leftcol > 0)
        && wlv.filler_todo <= 0
        && wlv.draw_state > WL_STC
        && mb_c != NUL) {
      mb_c = wp->w_p_lcs_chars.prec;
      lcs_prec_todo = NUL;
      if (utf_char2cells(mb_c) > 1) {
        // Double-width character being overwritten by the "precedes"
        // character, need to fill up half the character.
        wlv.c_extra = MB_FILLER_CHAR;
        wlv.c_final = NUL;
        wlv.n_extra = 1;
        wlv.n_attr = 2;
        wlv.extra_attr = win_hl_attr(wp, HLF_AT);
      }
      mb_schar = schar_from_char(mb_c);
      saved_attr3 = wlv.char_attr;  // save current attr
      wlv.char_attr = win_hl_attr(wp, HLF_AT);  // overwriting char_attr
      n_attr3 = 1;
    }

    // At end of the text line or just after the last character.
    if (mb_c == NUL && eol_hl_off == 0) {
      // flag to indicate whether prevcol equals startcol of search_hl or
      // one of the matches
      bool prevcol_hl_flag = get_prevcol_hl_flag(wp, &screen_search_hl,
                                                 (colnr_T)(ptr - line) - 1);

      // Invert at least one char, used for Visual and empty line or
      // highlight match at end of line. If it's beyond the last
      // char on the screen, just overwrite that one (tricky!)  Not
      // needed when a '$' was displayed for 'list'.
      if (wp->w_p_lcs_chars.eol == lcs_eol_one
          && ((area_attr != 0 && wlv.vcol == wlv.fromcol
               && (VIsual_mode != Ctrl_V
                   || lnum == VIsual.lnum
                   || lnum == curwin->w_cursor.lnum))
              // highlight 'hlsearch' match at end of line
              || prevcol_hl_flag)) {
        int n = 0;

        if (wlv.col >= grid->cols) {
          n = -1;
        }
        if (n != 0) {
          // At the window boundary, highlight the last character
          // instead (better than nothing).
          wlv.off += n;
          wlv.col += n;
        } else {
          // Add a blank character to highlight.
          linebuf_char[wlv.off] = schar_from_ascii(' ');
        }
        if (area_attr == 0 && !has_fold) {
          // Use attributes from match with highest priority among
          // 'search_hl' and the match list.
          get_search_match_hl(wp,
                              &screen_search_hl,
                              (colnr_T)(ptr - line),
                              &wlv.char_attr);
        }

        int eol_attr = wlv.char_attr;
        if (wlv.cul_attr) {
          eol_attr = hl_combine_attr(wlv.cul_attr, eol_attr);
        }
        linebuf_attr[wlv.off] = eol_attr;
        linebuf_vcol[wlv.off] = MAXCOL;
        wlv.col++;
        wlv.off++;
        wlv.vcol++;
        eol_hl_off = 1;
      }
    }

    // At end of the text line.
    if (mb_c == NUL) {
      // Highlight 'cursorcolumn' & 'colorcolumn' past end of the line.
      if (wp->w_p_wrap) {
        v = wlv.startrow == 0 ? wp->w_skipcol : 0;
      } else {
        v = wp->w_leftcol;
      }

      // check if line ends before left margin
      if (wlv.vcol < v + wlv.col - win_col_off(wp)) {
        wlv.vcol = (colnr_T)v + wlv.col - win_col_off(wp);
      }
      // Get rid of the boguscols now, we want to draw until the right
      // edge for 'cursorcolumn'.
      wlv.col -= wlv.boguscols;
      wlv.boguscols = 0;

      if (draw_color_col) {
        draw_color_col = advance_color_col(VCOL_HLC, &color_cols);
      }

      bool has_virttext = false;
      // Make sure alignment is the same regardless
      // if listchars=eol:X is used or not.
      int eol_skip = (wp->w_p_lcs_chars.eol == lcs_eol_one && eol_hl_off == 0
                      ? 1 : 0);

      if (has_decor) {
        has_virttext = decor_redraw_eol(wp, &decor_state, &wlv.line_attr, wlv.col + eol_skip);
      }

      if (((wp->w_p_cuc
            && wp->w_virtcol >= VCOL_HLC - eol_hl_off
            && wp->w_virtcol < grid->cols * (ptrdiff_t)(wlv.row - startrow + 1) + v
            && lnum != wp->w_cursor.lnum)
           || draw_color_col || wlv.line_attr_lowprio || wlv.line_attr
           || wlv.diff_hlf != 0 || has_virttext)) {
        int rightmost_vcol = 0;

        if (wp->w_p_cuc) {
          rightmost_vcol = wp->w_virtcol;
        }

        if (draw_color_col) {
          // determine rightmost colorcolumn to possibly draw
          for (int i = 0; color_cols[i] >= 0; i++) {
            if (rightmost_vcol < color_cols[i]) {
              rightmost_vcol = color_cols[i];
            }
          }
        }

        int cuc_attr = win_hl_attr(wp, HLF_CUC);
        int mc_attr = win_hl_attr(wp, HLF_MC);

        int diff_attr = 0;
        if (wlv.diff_hlf == HLF_TXD) {
          wlv.diff_hlf = HLF_CHD;
        }
        if (wlv.diff_hlf != 0) {
          diff_attr = win_hl_attr(wp, (int)wlv.diff_hlf);
        }

        int base_attr = hl_combine_attr(wlv.line_attr_lowprio, diff_attr);
        if (base_attr || wlv.line_attr || has_virttext) {
          rightmost_vcol = INT_MAX;
        }

        while (wlv.col < grid->cols) {
          linebuf_char[wlv.off] = schar_from_ascii(' ');
          linebuf_vcol[wlv.off] = MAXCOL;
          wlv.col++;
          if (draw_color_col) {
            draw_color_col = advance_color_col(VCOL_HLC, &color_cols);
          }

          int col_attr = base_attr;

          if (wp->w_p_cuc && VCOL_HLC == wp->w_virtcol) {
            col_attr = cuc_attr;
          } else if (draw_color_col && VCOL_HLC == *color_cols) {
            col_attr = hl_combine_attr(wlv.line_attr_lowprio, mc_attr);
          }

          col_attr = hl_combine_attr(col_attr, wlv.line_attr);

          linebuf_attr[wlv.off] = col_attr;
          wlv.off++;

          if (VCOL_HLC >= rightmost_vcol) {
            break;
          }

          wlv.vcol += 1;
        }
      }

      // TODO(bfredl): integrate with the common beyond-the-end-loop
      if (wp->w_buffer->terminal) {
        // terminal buffers may need to highlight beyond the end of the logical line
        while (wlv.col >= 0 && wlv.col < grid->cols) {
          linebuf_char[wlv.off] = schar_from_ascii(' ');
          linebuf_attr[wlv.off] = wlv.vcol >= TERM_ATTRS_MAX ? 0 : term_attrs[wlv.vcol];
          linebuf_vcol[wlv.off] = wlv.vcol;
          wlv.off++;
          wlv.vcol++;
          wlv.col++;
        }
      }

      if (kv_size(fold_vt) > 0) {
        draw_virt_text_item(buf, win_col_offset, fold_vt, kHlModeCombine, grid->cols, 0);
      }
      draw_virt_text(wp, buf, win_col_offset, &wlv.col, wlv.row);
      win_put_linebuf(wp, wlv.row, 0, wlv.col, grid->cols, bg_attr, false);
      wlv.row++;

      // Update w_cline_height and w_cline_folded if the cursor line was
      // updated (saves a call to plines_win() later).
      if (wp == curwin && lnum == curwin->w_cursor.lnum) {
        curwin->w_cline_row = startrow;
        curwin->w_cline_height = wlv.row - startrow;
        curwin->w_cline_folded = has_fold;
        curwin->w_valid |= (VALID_CHEIGHT|VALID_CROW);
        conceal_cursor_used = conceal_cursor_line(curwin);
      }
      break;
    }

    // Show "extends" character from 'listchars' if beyond the line end and
    // 'list' is set.
    if (wp->w_p_lcs_chars.ext != NUL
        && wlv.draw_state == WL_LINE
        && wp->w_p_list
        && !wp->w_p_wrap
        && wlv.filler_todo <= 0
        && wlv.col == grid->cols - 1
        && !has_fold) {
      if (has_decor && *ptr == NUL && lcs_eol_one == 0) {
        // Tricky: there might be a virtual text just _after_ the last char
        decor_redraw_col(wp, (colnr_T)v, wlv.off, false, &decor_state);
      }
      if (*ptr != NUL
          || lcs_eol_one > 0
          || (wlv.n_extra > 0 && (wlv.c_extra != NUL || *wlv.p_extra != NUL))
          || has_more_inline_virt(&wlv, v)) {
        mb_c = wp->w_p_lcs_chars.ext;
        wlv.char_attr = win_hl_attr(wp, HLF_AT);
        mb_schar = schar_from_char(mb_c);
      }
    }

    // advance to the next 'colorcolumn'
    if (draw_color_col) {
      draw_color_col = advance_color_col(VCOL_HLC, &color_cols);
    }

    // Highlight the cursor column if 'cursorcolumn' is set.  But don't
    // highlight the cursor position itself.
    // Also highlight the 'colorcolumn' if it is different than
    // 'cursorcolumn'
    // Also highlight the 'colorcolumn' if 'breakindent' and/or 'showbreak'
    // options are set
    vcol_save_attr = -1;
    if ((wlv.draw_state == WL_LINE
         || wlv.draw_state == WL_BRI
         || wlv.draw_state == WL_SBR)
        && !lnum_in_visual_area
        && search_attr == 0
        && area_attr == 0
        && wlv.filler_todo <= 0) {
      if (wp->w_p_cuc && VCOL_HLC == wp->w_virtcol
          && lnum != wp->w_cursor.lnum) {
        vcol_save_attr = wlv.char_attr;
        wlv.char_attr = hl_combine_attr(win_hl_attr(wp, HLF_CUC), wlv.char_attr);
      } else if (draw_color_col && VCOL_HLC == *color_cols) {
        vcol_save_attr = wlv.char_attr;
        wlv.char_attr = hl_combine_attr(win_hl_attr(wp, HLF_MC), wlv.char_attr);
      }
    }

    // Apply lowest-priority line attr now, so everything can override it.
    if (wlv.draw_state == WL_LINE) {
      wlv.char_attr = hl_combine_attr(wlv.line_attr_lowprio, wlv.char_attr);
    }

    if (wlv.draw_state == WL_LINE) {
      vcol_prev = wlv.vcol;
    }

    // Store character to be displayed.
    // Skip characters that are left of the screen for 'nowrap'.
    if (wlv.draw_state < WL_LINE || wlv.skip_cells <= 0) {
      // Store the character.
      linebuf_char[wlv.off] = mb_schar;
      if (multi_attr) {
        linebuf_attr[wlv.off] = multi_attr;
        multi_attr = 0;
      } else {
        linebuf_attr[wlv.off] = wlv.char_attr;
      }

      if (wlv.draw_state > WL_STC && wlv.filler_todo <= 0) {
        linebuf_vcol[wlv.off] = wlv.vcol;
      } else if (wlv.draw_state == WL_FOLD) {
        if (wlv.n_closing > 0) {
          linebuf_vcol[wlv.off] = -3;
          wlv.n_closing--;
        } else {
          linebuf_vcol[wlv.off] = -2;
        }
      } else {
        linebuf_vcol[wlv.off] = -1;
      }

      if (utf_char2cells(mb_c) > 1) {
        // Need to fill two screen columns.
        wlv.off++;
        wlv.col++;
        // UTF-8: Put a 0 in the second screen char.
        linebuf_char[wlv.off] = 0;
        linebuf_attr[wlv.off] = linebuf_attr[wlv.off - 1];

        if (wlv.draw_state > WL_STC && wlv.filler_todo <= 0) {
          linebuf_vcol[wlv.off] = ++wlv.vcol;
        } else {
          linebuf_vcol[wlv.off] = -1;
        }

        // When "wlv.tocol" is halfway through a character, set it to the end
        // of the character, otherwise highlighting won't stop.
        if (wlv.tocol == wlv.vcol) {
          wlv.tocol++;
        }
      }
      wlv.off++;
      wlv.col++;
    } else if (wp->w_p_cole > 0 && is_concealing) {
      wlv.skip_cells--;
      wlv.vcol_off++;
      if (wlv.n_extra > 0) {
        wlv.vcol_off += wlv.n_extra;
      }
      if (wp->w_p_wrap) {
        // Special voodoo required if 'wrap' is on.
        //
        // Advance the column indicator to force the line
        // drawing to wrap early. This will make the line
        // take up the same screen space when parts are concealed,
        // so that cursor line computations aren't messed up.
        //
        // To avoid the fictitious advance of 'wlv.col' causing
        // trailing junk to be written out of the screen line
        // we are building, 'boguscols' keeps track of the number
        // of bad columns we have advanced.
        if (wlv.n_extra > 0) {
          wlv.vcol += wlv.n_extra;
          wlv.col += wlv.n_extra;
          wlv.boguscols += wlv.n_extra;
          wlv.n_extra = 0;
          wlv.n_attr = 0;
        }

        if (utf_char2cells(mb_c) > 1) {
          // Need to fill two screen columns.
          wlv.boguscols++;
          wlv.col++;
        }

        wlv.boguscols++;
        wlv.col++;
      } else {
        if (wlv.n_extra > 0) {
          wlv.vcol += wlv.n_extra;
          wlv.n_extra = 0;
          wlv.n_attr = 0;
        }
      }
    } else {
      wlv.skip_cells--;
    }

    // The skipped cells need to be accounted for in vcol.
    if (wlv.draw_state > WL_STC && wlv.skipped_cells > 0) {
      wlv.vcol += wlv.skipped_cells;
      wlv.skipped_cells = 0;
    }

    // Only advance the "wlv.vcol" when after the 'number' or
    // 'relativenumber' column.
    if (wlv.draw_state > WL_STC
        && wlv.filler_todo <= 0) {
      wlv.vcol++;
    }

    if (vcol_save_attr >= 0) {
      wlv.char_attr = vcol_save_attr;
    }

    // restore attributes after "predeces" in 'listchars'
    if (wlv.draw_state > WL_STC && n_attr3 > 0 && --n_attr3 == 0) {
      wlv.char_attr = saved_attr3;
    }

    // restore attributes after last 'listchars' or 'number' char
    if (wlv.n_attr > 0 && wlv.draw_state == WL_LINE && --wlv.n_attr == 0) {
      wlv.char_attr = saved_attr2;
    }

    if (has_decor && wlv.filler_todo <= 0 && wlv.col >= grid->cols) {
      // At the end of screen line: might need to peek for decorations just after
      // this position.
      if (!has_fold && wp->w_p_wrap && wlv.n_extra == 0) {
        decor_redraw_col(wp, (int)(ptr - line), -3, false, &decor_state);
        // Check position/hiding of virtual text again on next screen line.
        decor_need_recheck = true;
      } else if (has_fold || !wp->w_p_wrap) {
        // Without wrapping, we might need to display right_align and win_col
        // virt_text for the entire text line.
        decor_redraw_col(wp, MAXCOL, -1, true, &decor_state);
      }
    }

    // At end of screen line and there is more to come: Display the line
    // so far.  If there is no more to display it is caught above.
    if (wlv.col >= grid->cols && (!has_fold || virt_line_offset >= 0)
        && (wlv.draw_state != WL_LINE
            || *ptr != NUL
            || wlv.filler_todo > 0
            || (wp->w_p_list && wp->w_p_lcs_chars.eol != NUL
                && wlv.p_extra != at_end_str)
            || (wlv.n_extra != 0 && (wlv.c_extra != NUL || *wlv.p_extra != NUL))
            || has_more_inline_virt(&wlv, v))) {
      bool wrap = wp->w_p_wrap       // Wrapping enabled.
                  && wlv.filler_todo <= 0          // Not drawing diff filler lines.
                  && lcs_eol_one != -1         // Haven't printed the lcs_eol character.
                  && wlv.row != endrow - 1     // Not the last line being displayed.
                  && (grid->cols == Columns  // Window spans the width of the screen,
                      || ui_has(kUIMultigrid))  // or has dedicated grid.
                  && !wp->w_p_rl;              // Not right-to-left.

      int draw_col = wlv.col - wlv.boguscols;
      if (virt_line_offset >= 0) {
        draw_virt_text_item(buf, virt_line_offset, kv_A(virt_lines, virt_line_index).line,
                            kHlModeReplace, grid->cols, 0);
      } else if (wlv.filler_todo <= 0) {
        draw_virt_text(wp, buf, win_col_offset, &draw_col, wlv.row);
      }

      win_put_linebuf(wp, wlv.row, 0, draw_col, grid->cols, bg_attr, wrap);
      if (wrap) {
        ScreenGrid *current_grid = grid;
        int current_row = wlv.row;
        int dummy_col = 0;  // unused
        grid_adjust(&current_grid, &current_row, &dummy_col);

        // Force a redraw of the first column of the next line.
        current_grid->attrs[current_grid->line_offset[current_row + 1]] = -1;
      }

      wlv.boguscols = 0;
      wlv.vcol_off = 0;
      wlv.row++;

      // When not wrapping and finished diff lines, or when displayed
      // '$' and highlighting until last column, break here.
      if ((!wp->w_p_wrap && wlv.filler_todo <= 0) || lcs_eol_one == -1) {
        break;
      }

      // When the window is too narrow draw all "@" lines.
      if (wlv.draw_state != WL_LINE && wlv.filler_todo <= 0) {
        win_draw_end(wp, '@', ' ', true, wlv.row, wp->w_grid.rows, HLF_AT);
        set_empty_rows(wp, wlv.row);
        wlv.row = endrow;
      }

      // When line got too long for screen break here.
      if (wlv.row == endrow) {
        wlv.row++;
        break;
      }

      win_line_start(wp, &wlv, true);

      lcs_prec_todo = wp->w_p_lcs_chars.prec;
      if (wlv.filler_todo <= 0) {
        wlv.need_showbreak = true;
      }
      if (statuscol.draw) {
        if (vim_strchr(p_cpo, CPO_NUMCOL) && wlv.row > startrow + wlv.filler_lines) {
          statuscol.draw = false;  // don't draw status column if "n" is in 'cpo'
        } else {
          statuscol.textp = NULL;  // re-evaluate with new v:virtnum
        }
      }
      wlv.filler_todo--;
      virt_line_offset = -1;
      // When the filler lines are actually below the last line of the
      // file, don't draw the line itself, break here.
      if (wlv.filler_todo == 0 && (wp->w_botfill || end_fill)) {
        break;
      }
    }
  }     // for every character in the line

  clear_virttext(&fold_vt);
  kv_destroy(virt_lines);
  xfree(foldtext_free);
  return wlv.row;
}

static void win_put_linebuf(win_T *wp, int row, int coloff, int endcol, int clear_width,
                            int bg_attr, bool wrap)
{
  ScreenGrid *grid = &wp->w_grid;

  int start_col = 0;

  if (wp->w_p_rl) {
    linebuf_mirror(&start_col, &clear_width, grid->cols);
    endcol = grid->cols - 1 - endcol;
  }

  // Take care of putting "<<<" on the first line for 'smoothscroll'.
  if (row == 0 && wp->w_skipcol > 0
      // do not overwrite the 'showbreak' text with "<<<"
      && *get_showbreak_value(wp) == NUL
      // do not overwrite the 'listchars' "precedes" text with "<<<"
      && !(wp->w_p_list && wp->w_p_lcs_chars.prec != 0)) {
    int off = 0;
    if (wp->w_p_nu && wp->w_p_rnu) {
      // do not overwrite the line number, change "123 text" to "123<<<xt".
      while (off < grid->cols && ascii_isdigit(schar_get_ascii(linebuf_char[off]))) {
        off++;
      }
    }

    for (int i = 0; i < 3 && off < grid->cols; i++) {
      if (off + 1 < grid->cols && linebuf_char[off + 1] == NUL) {
        // When the first half of a double-width character is
        // overwritten, change the second half to a space.
        linebuf_char[off + 1] = schar_from_ascii(' ');
      }
      linebuf_char[off] = schar_from_ascii('<');
      linebuf_attr[off] = HL_ATTR(HLF_AT);
      off++;
    }
  }

  grid_adjust(&grid, &row, &coloff);
  grid_put_linebuf(grid, row, coloff, start_col, endcol, clear_width, wp->w_p_rl, bg_attr, wrap);
}
