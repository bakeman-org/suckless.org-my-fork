#include "config.h"
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define RECENT_MAX 10
static char recent_cache_path[512];
static char **recent_items = NULL;
static size_t recent_count = 0;

static int fuzzy_match(const char *needle, const char *haystack,
                       int case_insensitive);
static void recent_set_path(void);
static void recent_load(void);
static void recent_save(const char *selected_text);
static void recent_free(void);
static int in_recent(const char *s);
static struct item *reorder_by_recency(struct item *src);

void *ecalloc(size_t nmemb, size_t size) {
  if (size != 0 && nmemb > SIZE_MAX / size) {
    errno = ENOMEM;
    fprintf(
        stderr,
        "ERROR: ecalloc: nmemb * size overflows (requested %ju x %ju bytes)\n",
        (uintmax_t)nmemb, (uintmax_t)size);
    return NULL;
  }
  void *p;
  if (!(p = calloc(nmemb, size)))
    fprintf(stderr, "ERROR: calloc: %s", strerror(errno));
  return p;
}

typedef struct {
  Cursor cursor;
} Cur;

typedef struct Fnt {
  Display *dpy;
  unsigned int h;
  XftFont *xfont;
  FcPattern *pattern;
  struct Fnt *next;
} Fnt;

enum { ColFg, ColBg };
typedef XftColor Clr;
typedef struct {
  unsigned int w, h;
  Display *dpy;
  int screen;
  Window root;
  Drawable drawable;
  GC gc;
  Clr *scheme;
  Fnt *fonts;
} Drw;

Drw *drw_create(Display *dpy, int screen, Window root, unsigned int w,
                unsigned int h) {
  Drw *drw = ecalloc(1, sizeof(Drw));
  drw->dpy = dpy;
  drw->screen = screen;
  drw->root = root;
  drw->w = w;
  drw->h = h;
  drw->drawable = XCreatePixmap(dpy, root, w, h, DefaultDepth(dpy, screen));
  drw->gc = XCreateGC(dpy, root, 0, NULL);
  XSetLineAttributes(dpy, drw->gc, 1, LineSolid, CapButt, JoinMiter);
  return drw;
}

void drw_resize(Drw *drw, unsigned int w, unsigned int h) {
  if (!drw)
    return;
  drw->w = w;
  drw->h = h;
  if (drw->drawable)
    XFreePixmap(drw->dpy, drw->drawable);
  drw->drawable = XCreatePixmap(drw->dpy, drw->root, w, h,
                                DefaultDepth(drw->dpy, drw->screen));
}

static Fnt *xfont_create(Drw *drw, const char *fontname,
                         FcPattern *fontpattern) {
  Fnt *font;
  XftFont *xfont = NULL;
  FcPattern *pattern = NULL;
  if (fontname) {
    if (!(xfont = XftFontOpenName(drw->dpy, drw->screen, fontname))) {
      fprintf(stderr, "error, cannot load font from name: '%s'\n", fontname);
      return NULL;
    }
    if (!(pattern = FcNameParse((FcChar8 *)fontname))) {
      fprintf(stderr, "error, cannot parse font name to pattern: '%s'\n",
              fontname);
      XftFontClose(drw->dpy, xfont);
      return NULL;
    }
  } else if (fontpattern) {
    if (!(xfont = XftFontOpenPattern(drw->dpy, fontpattern))) {
      fprintf(stderr, "error, cannot load font from pattern.\n");
      return NULL;
    }
  } else {
    fprintf(stderr, "no font specified.");
  }
  font = ecalloc(1, sizeof(Fnt));
  font->xfont = xfont;
  font->pattern = pattern;
  font->h = xfont->ascent + xfont->descent;
  font->dpy = drw->dpy;
  return font;
}

static void xfont_free(Fnt *font) {
  if (!font)
    return;
  if (font->pattern)
    FcPatternDestroy(font->pattern);
  XftFontClose(font->dpy, font->xfont);
  free(font);
}

Fnt *drw_fontset_create(Drw *drw, const char *fonts[], size_t fontcount) {
  Fnt *cur, *ret = NULL;
  size_t i;
  if (!drw || !fonts)
    return NULL;
  for (i = 1; i <= fontcount; i++) {
    if ((cur = xfont_create(drw, fonts[fontcount - i], NULL))) {
      cur->next = ret;
      ret = cur;
    }
  }
  return (drw->fonts = ret);
}

void drw_fontset_free(Fnt *font) {
  if (font) {
    drw_fontset_free(font->next);
    xfont_free(font);
  }
}
void drw_free(Drw *drw) {
  XFreePixmap(drw->dpy, drw->drawable);
  XFreeGC(drw->dpy, drw->gc);
  drw_fontset_free(drw->fonts);
  free(drw);
}
void drw_clr_create(Drw *drw, Clr *dest, const char *clrname) {
  if (!drw || !dest || !clrname)
    return;
  if (!XftColorAllocName(drw->dpy, DefaultVisual(drw->dpy, drw->screen),
                         DefaultColormap(drw->dpy, drw->screen), clrname, dest))
    fprintf(stderr, "error, cannot allocate color '%s'", clrname);
}
Clr *drw_scm_create(Drw *drw, const char *clrnames[], size_t clrcount) {
  Clr *ret;
  if (!drw || !clrnames || clrcount < 2 ||
      !(ret = ecalloc(clrcount, sizeof(XftColor))))
    return NULL;
  for (size_t i = 0; i < clrcount; i++)
    drw_clr_create(drw, &ret[i], clrnames[i]);
  return ret;
}
void drw_setfontset(Drw *drw, Fnt *set) {
  if (drw)
    drw->fonts = set;
}
void drw_setscheme(Drw *drw, Clr *scm) {
  if (drw)
    drw->scheme = scm;
}
void drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h,
              int filled, int invert) {
  if (!drw || !drw->scheme)
    return;
  XSetForeground(drw->dpy, drw->gc,
                 invert ? drw->scheme[ColBg].pixel : drw->scheme[ColFg].pixel);
  if (filled)
    XFillRectangle(drw->dpy, drw->drawable, drw->gc, x, y, w, h);
  else
    XDrawRectangle(drw->dpy, drw->drawable, drw->gc, x, y, w - 1, h - 1);
}

void drw_font_getexts(Fnt *font, const char *text, unsigned int len,
                      unsigned int *w, unsigned int *h) {
  XGlyphInfo ext;
  if (!font || !text)
    return;
  XftTextExtentsUtf8(font->dpy, font->xfont, (XftChar8 *)text, len, &ext);
  if (w)
    *w = ext.xOff;
  if (h)
    *h = font->h;
}

unsigned int drw_fontset_getwidth(Drw *drw, const char *text);

#define UTF_INVALID 0xFFFD
int utf8decode(const char *s_in, long *u, int *err) {
  const unsigned char *s = (const unsigned char *)s_in;
  *u = UTF_INVALID;
  *err = 1;

  if (*s == '\0')
    return 0;

  if ((s[0] & 0x80) == 0) {
    *u = s[0];
    *err = 0;
    return 1;
  }

  if ((s[0] & 0xE0) == 0xC0) {
    if (s[1] == '\0' || (s[1] & 0xC0) != 0x80)
      return 1;
    *u = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    if (*u < 0x80)
      return 1;
    *err = 0;
    return 2;
  }

  if ((s[0] & 0xF0) == 0xE0) {
    if (s[1] == '\0' || (s[1] & 0xC0) != 0x80)
      return 1;
    if (s[2] == '\0' || (s[2] & 0xC0) != 0x80)
      return 2;
    *u = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    if (*u < 0x800)
      return 1;
    if ((*u >= 0xD800) && (*u <= 0xDFFF))
      return 3;
    *err = 0;
    return 3;
  }

  if ((s[0] & 0xF8) == 0xF0) {
    if (s[1] == '\0' || (s[1] & 0xC0) != 0x80)
      return 1;
    if (s[2] == '\0' || (s[2] & 0xC0) != 0x80)
      return 2;
    if (s[3] == '\0' || (s[3] & 0xC0) != 0x80)
      return 3;
    *u = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) |
         (s[3] & 0x3F);
    if (*u < 0x10000 || *u > 0x10FFFF)
      return 1;
    *err = 0;
    return 4;
  }
  return 1;
}

int drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h,
             unsigned int lpad, const char *text, int invert) {
  int ty, ellipsis_x = 0;
  unsigned int tmpw, ew, ellipsis_w = 0, ellipsis_len, hash, h0, h1;
  XftDraw *d = NULL;
  Fnt *usedfont, *curfont, *nextfont;
  int utf8strlen, utf8charlen, utf8err, render = x || y || w || h;
  long utf8codepoint = 0;
  const char *utf8str;
  FcCharSet *fccharset;
  FcPattern *fcpattern;
  FcPattern *match;
  XftResult result;
  int charexists = 0, overflow = 0;
  static unsigned int nomatches[128], ellipsis_width, invalid_width;
  static const char invalid[] = "�";
  if (!drw || (render && (!drw->scheme || !w)) || !text || !drw->fonts)
    return 0;
  if (!render) {
    w = invert ? invert : ~invert;
  } else {
    XSetForeground(drw->dpy, drw->gc,
                   drw->scheme[invert ? ColFg : ColBg].pixel);
    XFillRectangle(drw->dpy, drw->drawable, drw->gc, x, y, w, h);
    if (w < lpad)
      return x + w;
    d = XftDrawCreate(drw->dpy, drw->drawable,
                      DefaultVisual(drw->dpy, drw->screen),
                      DefaultColormap(drw->dpy, drw->screen));
    x += lpad;
    w -= lpad;
  }
  usedfont = drw->fonts;
  if (!ellipsis_width && render)
    ellipsis_width = drw_fontset_getwidth(drw, "...");
  if (!invalid_width && render)
    invalid_width = drw_fontset_getwidth(drw, invalid);
  while (true) {
    ew = ellipsis_len = utf8err = utf8charlen = utf8strlen = 0;
    utf8str = text;
    nextfont = NULL;
    while (*text) {
      utf8charlen = utf8decode(text, &utf8codepoint, &utf8err);
      for (curfont = drw->fonts; curfont; curfont = curfont->next) {
        charexists = charexists ||
                     XftCharExists(drw->dpy, curfont->xfont, utf8codepoint);
        if (charexists) {
          drw_font_getexts(curfont, text, utf8charlen, &tmpw, NULL);
          if (ew + ellipsis_width <= w) {
            ellipsis_x = x + ew;
            ellipsis_w = w - ew;
            ellipsis_len = utf8strlen;
          }
          if (ew + tmpw > w) {
            overflow = 1;
            if (!render)
              x += tmpw;
            else
              utf8strlen = ellipsis_len;
          } else if (curfont == usedfont) {
            text += utf8charlen;
            utf8strlen += utf8err ? 0 : utf8charlen;
            ew += utf8err ? 0 : tmpw;
          } else {
            nextfont = curfont;
          }
          break;
        }
      }
      if (overflow || !charexists || nextfont || utf8err)
        break;
      else
        charexists = 0;
    }
    if (utf8strlen) {
      if (render) {
        ty = y + (h - usedfont->h) / 2 + usedfont->xfont->ascent;
        XftDrawStringUtf8(d, &drw->scheme[invert ? ColBg : ColFg],
                          usedfont->xfont, x, ty, (XftChar8 *)utf8str,
                          utf8strlen);
      }
      x += ew;
      w -= ew;
    }
    if (utf8err && (!render || invalid_width < w)) {
      if (render)
        drw_text(drw, x, y, w, h, 0, invalid, invert);
      x += invalid_width;
      w -= invalid_width;
    }
    if (render && overflow)
      drw_text(drw, ellipsis_x, y, ellipsis_w, h, 0, "...", invert);
    if (!*text || overflow) {
      break;
    } else if (nextfont) {
      charexists = 0;
      usedfont = nextfont;
    } else {
      charexists = 1;
      hash = (unsigned int)utf8codepoint;
      hash = ((hash >> 16) ^ hash) * 0x21F0AAAD;
      hash = ((hash >> 15) ^ hash) * 0xD35A2D97;
      size_t nomatches_len = sizeof(nomatches) / sizeof(nomatches[0]);
      h0 = ((hash >> 15) ^ hash) % nomatches_len;
      h1 = (hash >> 17) % nomatches_len;
      if (nomatches[h0] == utf8codepoint || nomatches[h1] == utf8codepoint)
        goto no_match;
      fccharset = FcCharSetCreate();
      FcCharSetAddChar(fccharset, utf8codepoint);
      if (!drw->fonts->pattern) {
        fprintf(
            stderr,
            "the first font in the cache must be loaded from a font string.");
      }
      fcpattern = FcPatternDuplicate(drw->fonts->pattern);
      FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
      FcPatternAddBool(fcpattern, FC_SCALABLE, FcTrue);
      FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
      FcDefaultSubstitute(fcpattern);
      match = XftFontMatch(drw->dpy, drw->screen, fcpattern, &result);
      FcCharSetDestroy(fccharset);
      FcPatternDestroy(fcpattern);
      if (match) {
        usedfont = xfont_create(drw, NULL, match);
        if (usedfont &&
            XftCharExists(drw->dpy, usedfont->xfont, utf8codepoint)) {
          for (curfont = drw->fonts; curfont->next; curfont = curfont->next)
            ;
          curfont->next = usedfont;
        } else {
          xfont_free(usedfont);
          nomatches[nomatches[h0] ? h1 : h0] = utf8codepoint;
        no_match:
          usedfont = drw->fonts;
        }
      }
    }
  }
  if (d)
    XftDrawDestroy(d);
  return x + (render ? w : 0);
}

unsigned int drw_fontset_getwidth(Drw *drw, const char *text) {
  if (!drw || !drw->fonts || !text)
    return 0;
  return drw_text(drw, 0, 0, 0, 0, 0, text, 0);
}

void drw_map(Drw *drw, Window win, int x, int y, unsigned int w,
             unsigned int h) {
  if (!drw)
    return;
  XCopyArea(drw->dpy, drw->drawable, win, drw->gc, x, y, w, h, x, y);
  XSync(drw->dpy, False);
}

unsigned int drw_fontset_getwidth_clamp(Drw *drw, const char *text,
                                        unsigned int n) {
  unsigned int tmp = 0;
  if (drw && drw->fonts && text && n)
    tmp = drw_text(drw, 0, 0, 0, 0, 0, text, n);
  return MIN(n, tmp);
}

Cur *drw_cur_create(Drw *drw, int shape) {
  Cur *cur;
  if (!drw || !(cur = ecalloc(1, sizeof(Cur))))
    return NULL;
  cur->cursor = XCreateFontCursor(drw->dpy, shape);
  return cur;
}

void drw_cur_free(Drw *drw, Cur *cursor) {
  if (!cursor)
    return;
  XFreeCursor(drw->dpy, cursor->cursor);
  free(cursor);
}

#define INTERSECT(x, y, w, h, r)                                               \
  (MAX(0, MIN((x) + (w), (r).x_org + (r).width) - MAX((x), (r).x_org)) *       \
   MAX(0, MIN((y) + (h), (r).y_org + (r).height) - MAX((y), (r).y_org)))
#define TEXTW(X) (drw_fontset_getwidth(drw, (X)) + lrpad)

struct item {
  char *text;
#ifdef DMENU_CENTER_PATCH
  unsigned int width;
#endif
  struct item *left, *right;
  int out;
};

static char text[BUFSIZ] = "";
static char *embed;
static int bh;
static unsigned int mw, mh;
static unsigned int inputw = 0, promptw;
static int lrpad;
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1, screen;
static Atom clip, utf8;
static Display *dpy;
static Window root, parentwin, win;
static XIC xic;
static Drw *drw;
static Clr *scheme[SchemeLast];
static int fuzzy = 0; // 默认子串匹配，Ctrl+\ 切换到模糊匹配

/* 输入为“程序名 + 参数”时，作为可选项插入到列表首位 */
static struct item rawitem;
static char rawcmd[sizeof text];

/* 底部提示栏（Emacs mode line 风格）与帮助面板 */
static int hintbarh;
static int help_visible = 0;
static size_t match_count = 0;

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

static unsigned int textw_clamp(const char *str, unsigned int n) {
  unsigned int w = drw_fontset_getwidth_clamp(drw, str, n) + lrpad;
  return MIN(w, n);
}

static void appenditem(struct item *item, struct item **list,
                       struct item **last) {
  if (*last)
    (*last)->right = item;
  else
    *list = item;
  item->left = *last;
  item->right = NULL;
  *last = item;
}

static void calcoffsets(void) {
  unsigned int i = 0;
  unsigned int n;
  if (lines > 0)
    n = lines * (unsigned int)bh;
  else
    n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));

  for (i = 0, next = curr; next; next = next->right) {
    i += (lines > 0) ? (unsigned int)bh : textw_clamp(next->text, n);
    if (i > n)
      break;
  }

  for (i = 0, prev = curr; prev && prev->left; prev = prev->left) {
    i += (lines > 0) ? (unsigned int)bh : textw_clamp(prev->left->text, n);
    if (i > n)
      break;
  }
}

#ifdef DMENU_CENTER_PATCH
static int max_textw(void) {
  int len = 0;
  for (struct item *item = items; item && item->text; item++)
    len = MAX(item->width, len);
  return len;
}
#endif

static int fuzzy_match(const char *needle, const char *haystack,
                       int case_insensitive) {
  const char *n = needle;
  const char *h = haystack;
  unsigned char cn, ch;

  if (!*n)
    return 1;

  for (; *h; h++) {
    cn = (unsigned char)*n;
    ch = (unsigned char)*h;
    if (case_insensitive) {
      cn = tolower(cn);
      ch = tolower(ch);
    }
    if (cn == ch) {
      n++;
      if (!*n)
        return 1;
    }
  }
  return 0;
}

static void recent_set_path(void) {
  const char *xdg_cache = getenv("XDG_CACHE_HOME");
  char *home = getenv("HOME");
  if (xdg_cache && *xdg_cache) {
    snprintf(recent_cache_path, sizeof(recent_cache_path), "%s/dmenux/recent",
             xdg_cache);
  } else if (home) {
    snprintf(recent_cache_path, sizeof(recent_cache_path),
             "%s/.cache/dmenux/recent", home);
  } else {
    recent_cache_path[0] = '\0';
  }
}

static void recent_free(void) {
  size_t i;
  for (i = 0; i < recent_count; i++) {
    free(recent_items[i]);
  }
  free(recent_items);
  recent_items = NULL;
  recent_count = 0;
}

static void recent_load(void) {
  FILE *fp;
  char buf[1024];
  recent_free();
  if (!recent_cache_path[0])
    return;

  fp = fopen(recent_cache_path, "r");
  if (!fp)
    return;

  while (fgets(buf, sizeof(buf), fp) && recent_count < RECENT_MAX) {
    size_t l = strlen(buf);
    if (l > 0 && buf[l - 1] == '\n')
      buf[l - 1] = '\0';
    if (!*buf)
      continue;
    char *dup = strdup(buf);
    if (!dup)
      continue;
    char **tmp = realloc(recent_items, (recent_count + 1) * sizeof(char *));
    if (!tmp) {
      free(dup);
      break;
    }
    recent_items = tmp;
    recent_items[recent_count++] = dup;
  }
  fclose(fp);
}

static void recent_save(const char *selected_text) {
  FILE *fp;
  char dirpath[512];
  size_t i, j;
  if (!recent_cache_path[0] || !selected_text || !*selected_text)
    return;

  for (i = 0; i < recent_count;) {
    if (!strcmp(recent_items[i], selected_text)) {
      free(recent_items[i]);
      for (j = i; j < recent_count - 1; j++)
        recent_items[j] = recent_items[j + 1];
      recent_count--;
    } else {
      i++;
    }
  }

  char **tmp = realloc(recent_items, (recent_count + 1) * sizeof(char *));
  if (!tmp)
    return;
  recent_items = tmp;
  for (i = recent_count; i > 0; i--)
    recent_items[i] = recent_items[i - 1];
  recent_items[0] = strdup(selected_text);
  if (recent_count < RECENT_MAX)
    recent_count++;
  else
    recent_count = RECENT_MAX;

  snprintf(dirpath, sizeof(dirpath), "%s", recent_cache_path);
  char *slash = strrchr(dirpath, '/');
  if (slash) {
    *slash = '\0';
    mkdir(dirpath, 0700);
  }

  fp = fopen(recent_cache_path, "w");
  if (!fp)
    return;
  for (i = 0; i < recent_count; i++) {
    fprintf(fp, "%s\n", recent_items[i]);
  }
  fclose(fp);
}

static int in_recent(const char *s) {
  size_t i;
  for (i = 0; i < recent_count; i++) {
    if (!strcmp(recent_items[i], s))
      return (int)i + 1;
  }
  return 0;
}

static struct item *reorder_by_recency(struct item *src) {
  struct item *recent_head = NULL, *recent_tail = NULL;
  struct item *rest_head = NULL, *rest_tail = NULL;
  struct item *cur, *next;

  for (cur = src; cur; cur = next) {
    next = cur->right;
    cur->left = cur->right = NULL;
    if (in_recent(cur->text)) {
      appenditem(cur, &recent_head, &recent_tail);
    } else {
      appenditem(cur, &rest_head, &rest_tail);
    }
  }
  if (recent_tail) {
    recent_tail->right = rest_head;
    if (rest_head)
      rest_head->left = recent_tail;
    return recent_head;
  } else {
    return rest_head;
  }
}

void cleanup(void) {
  size_t i;
  XUngrabKeyboard(dpy, CurrentTime);
  for (i = 0; i < SchemeLast; i++)
    free(scheme[i]);
  for (i = 0; items && items[i].text; ++i)
    free(items[i].text);
  free(items);
  recent_free();
  drw_free(drw);
  XSync(dpy, False);
  XCloseDisplay(dpy);
}

static int drawitem(struct item *item, int x, int y, int w) {
  if (item == sel)
    drw_setscheme(drw, scheme[SchemeSel]);
  else if (item->out)
    drw_setscheme(drw, scheme[SchemeOut]);
  else
    drw_setscheme(drw, scheme[SchemeNorm]);
  return drw_text(drw, x, y, w, bh, lrpad / 2, item->text, 0);
}

/* 底部常驻提示栏：当前匹配模式、匹配数、原始命令状态与快捷键 */
static void drawhintbar(void) {
  char left[128], right[128];
  int y = (int)mh - hintbarh;
  int pad = lrpad / 2;
  unsigned int rw;

  if (help_visible)
    snprintf(left, sizeof left, "按键帮助");
  else if (sel == &rawitem)
    snprintf(left, sizeof left, "[%s%s] %zu 项  原始命令",
             fuzzy ? "模糊" : "子串",
             fstrncmp == strncasecmp ? "/i" : "", match_count);
  else
    snprintf(left, sizeof left, "[%s%s] %zu 项", fuzzy ? "模糊" : "子串",
             fstrncmp == strncasecmp ? "/i" : "", match_count);
  snprintf(right, sizeof right, "F1 帮助  ^\\ 切换  Tab 补全  Esc 退出");

  drw_setscheme(drw, scheme[SchemeHint]);
  drw_rect(drw, 0, y, mw, hintbarh, 1, 1);

  rw = TEXTW(right);
  if (rw + 3 * (unsigned int)pad < mw) {
    drw_text(drw, (int)(mw - rw - pad), y, rw, hintbarh, 0, right, 0);
    drw_text(drw, pad, y, (int)(mw - rw - 3 * pad), hintbarh, 0, left, 0);
  } else {
    drw_text(drw, pad, y, (int)(mw - 2 * pad), hintbarh, 0, left, 0);
  }
}

/* 居中帮助面板，列出所有按键 */
static void drawhelpbox(void) {
  static const char *helpinfo[] = {
      "Enter       运行 / 选择",
      "Shift+Enter 原样输出输入",
      "Ctrl+Enter  输出后保持打开",
      "Tab         补全到选中项",
      "Ctrl+\\      切换模糊 / 子串匹配",
      "Alt+j/k     下一项 / 上一项",
      "Ctrl+W/U    删除前一词 / 删到行首",
      "Ctrl+Y      粘贴",
      "Esc         退出",
  };
  int n = (int)(sizeof helpinfo / sizeof helpinfo[0]);
  int pad = lrpad / 2;
  int lh = bh;
  int avail = (int)mh - hintbarh;
  int maxrows = avail / lh;
  int rows, i, bx, by;
  unsigned int bw;

  if (maxrows < 2)
    return;
  rows = (n + 1 < maxrows) ? n + 1 : maxrows;
  bw = TEXTW("按键帮助");
  for (i = 0; i < n; i++)
    bw = MAX(bw, TEXTW(helpinfo[i]));
  bw = MIN(bw + 2 * (unsigned int)pad, mw);

  bx = (int)((mw - bw) / 2);
  by = (avail - rows * lh) / 2;

  drw_setscheme(drw, scheme[SchemeSel]);
  drw_rect(drw, bx - 2, by - 2, bw + 4, rows * lh + 4, 1, 1);
  drw_setscheme(drw, scheme[SchemeHint]);
  drw_rect(drw, bx, by, bw, rows * lh, 1, 1);
  drw_text(drw, bx, by, bw, lh, pad, "按键帮助", 0);
  for (i = 0; i < rows - 1; i++)
    drw_text(drw, bx, by + (i + 1) * lh, bw, lh, pad, helpinfo[i], 0);
}

static void drawmenu(void) {
  unsigned int curpos;
  struct item *item;
  int x = 0, y = 0;
  unsigned int w;

  drw_setscheme(drw, scheme[SchemeNorm]);
  drw_rect(drw, 0, 0, mw, mh, 1, 1);

  if (prompt && *prompt) {
    drw_setscheme(drw, scheme[SchemeSel]);
    x = drw_text(drw, x, 0, promptw, bh, lrpad / 2, prompt, 0);
  }

  w = (lines > 0 || !matches) ? mw - (unsigned int)x : inputw;
  drw_setscheme(drw, scheme[SchemeNorm]);
  drw_text(drw, x, 0, w, bh, lrpad / 2, text, 0);

  curpos = TEXTW(text) - TEXTW(&text[cursor]);
  if ((curpos += (unsigned int)(lrpad / 2 - 1)) < w) {
    drw_setscheme(drw, scheme[SchemeNorm]);
    drw_rect(drw, x + (int)curpos, 2, 2, bh - 4, 1, 0);
  }

  if (lines > 0) {
    for (item = curr; item != next; item = item->right)
      drawitem(item, x, y += bh, mw - x);
  } else if (matches) {
    x += inputw;
    w = TEXTW("<");
    if (curr->left) {
      drw_setscheme(drw, scheme[SchemeNorm]);
      drw_text(drw, x, 0, w, bh, lrpad / 2, "<", 0);
    }
    x += w;
    for (item = curr; item != next; item = item->right)
      x = drawitem(item, x, 0, textw_clamp(item->text, mw - x - TEXTW(">")));
    if (next) {
      w = TEXTW(">");
      drw_setscheme(drw, scheme[SchemeNorm]);
      drw_text(drw, mw - w, 0, w, bh, lrpad / 2, ">", 0);
    }
  }
  if (help_visible)
    drawhelpbox();
  drawhintbar();
  drw_map(drw, win, 0, 0, mw, mh);
}

static void grabfocus(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
  Window focuswin;
  int i, revertwin;
  for (i = 0; i < 100; ++i) {
    XGetInputFocus(dpy, &focuswin, &revertwin);
    if (focuswin == win)
      return;
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
    nanosleep(&ts, NULL);
  }
  fprintf(stderr, "cannot grab focus");
}

static void grabkeyboard(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  int i;
  if (embed)
    return;
  for (i = 0; i < 1000; i++) {
    if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
                      GrabModeAsync, CurrentTime) == GrabSuccess)
      return;
    nanosleep(&ts, NULL);
  }
  fprintf(stderr, "cannot grab keyboard");
}

char *cistrstr(const char *h, const char *n) {
  size_t i;
  if (!n[0])
    return (char *)h;
  for (; *h; ++h) {
    for (i = 0;
         n[i] && tolower((unsigned char)n[i]) == tolower((unsigned char)h[i]);
         ++i)
      ;
    if (n[i] == '\0')
      return (char *)h;
  }
  return NULL;
}

/* 判断 word 是否为 PATH 中的可执行文件（支持绝对/相对路径） */
static int word_is_executable(const char *word) {
  const char *path, *p, *end;
  char buf[4096];
  size_t dlen, wlen = strlen(word);

  if (strchr(word, '/'))
    return access(word, X_OK) == 0;
  if (!(path = getenv("PATH")))
    return 0;
  for (p = path; *p; p = end + 1) {
    end = strchr(p, ':');
    dlen = end ? (size_t)(end - p) : strlen(p);
    if (dlen > 0 && dlen + 1 + wlen < sizeof buf) {
      memcpy(buf, p, dlen);
      buf[dlen] = '/';
      memcpy(buf + dlen + 1, word, wlen + 1);
      if (access(buf, X_OK) == 0)
        return 1;
    }
    if (!end)
      break;
  }
  return 0;
}

/* 输入是否为“程序名 + 参数”形式：含空格，且首词是列表项或可执行程序 */
static int is_raw_command(void) {
  const char *sp = strchr(text, ' ');
  char word[1024];
  size_t len;
  int first_is_item = 0;
  struct item *it;

  if (!sp || sp == text)
    return 0;
  len = (size_t)(sp - text);
  if (len >= sizeof word)
    return 0;
  memcpy(word, text, len);
  word[len] = '\0';

  /* 完整输入已是列表项时按普通选择处理 */
  for (it = items; it && it->text; it++) {
    if (!strcmp(it->text, text))
      return 0;
    if (!strcmp(it->text, word))
      first_is_item = 1;
  }
  return first_is_item || word_is_executable(word);
}

static void match(void) {
  static char **tokv = NULL;
  static int tokn = 0;
  char buf[sizeof text], *s;
  int i, tokc = 0;
  struct item *item;
  strcpy(buf, text);
  for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
    if (++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
      fprintf(stderr, "cannot realloc %zu bytes:", tokn * sizeof *tokv);

  matches = matchend = NULL;

  for (item = items; item && item->text; item++) {
    int ok = 1;
    for (i = 0; i < tokc; i++) {
      int icase = (fstrstr == cistrstr) ? 1 : 0;

      if (fuzzy) {
        if (!fuzzy_match(tokv[i], item->text, icase)) {
          ok = 0;
          break;
        }
      } else {
        if (icase) {
          if (!cistrstr(item->text, tokv[i])) {
            ok = 0;
            break;
          }
        } else {
          if (!strstr(item->text, tokv[i])) {
            ok = 0;
            break;
          }
        }
      }
    }
    if (!ok)
      continue;
    appenditem(item, &matches, &matchend);
  }

  matches = reorder_by_recency(matches);

  /* “程序名 + 参数”作为首选项，回车即可直接运行输入的完整命令 */
  if (is_raw_command()) {
    strcpy(rawcmd, text);
    rawitem.text = rawcmd;
    rawitem.out = 0;
#ifdef DMENU_CENTER_PATCH
    rawitem.width = TEXTW(rawcmd);
#endif
    rawitem.left = NULL;
    rawitem.right = matches;
    if (matches)
      matches->left = &rawitem;
    matches = &rawitem;
  }

  matchend = NULL;
  match_count = 0;
  for (struct item *p = matches; p; p = p->right) {
    matchend = p;
    if (p != &rawitem)
      match_count++;
  }

  curr = sel = matches;
  calcoffsets();
}

static void insert(const char *str, ssize_t n) {
  if (strlen(text) + n > sizeof text - 1)
    return;
  memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
  if (n > 0)
    memcpy(&text[cursor], str, n);
  cursor += n;
  match();
}
static size_t nextrune(int inc) {
  ssize_t n;
  for (n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
    ;
  return n;
}
static void movewordedge(int dir) {
  if (dir < 0) {
    while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
      cursor = nextrune(-1);
    while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
      cursor = nextrune(-1);
  } else {
    while (text[cursor] && strchr(worddelimiters, text[cursor]))
      cursor = nextrune(+1);
    while (text[cursor] && !strchr(worddelimiters, text[cursor]))
      cursor = nextrune(+1);
  }
}
static void keypress(XKeyEvent *ev) {
  char buf[64];
  int len;
  KeySym ksym = NoSymbol;
  Status status;
  len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);

  /* 帮助面板打开时，除 F1 外的按键先关闭面板（Esc 只关闭面板不退出） */
  if (help_visible && ksym != XK_F1) {
    help_visible = 0;
    if (ksym == XK_Escape) {
      drawmenu();
      return;
    }
  }

  switch (status) {
  default:
    return;
  case XLookupChars:
    goto insert;
  case XLookupKeySym:
  case XLookupBoth:
    break;
  }
  if (ev->state & ControlMask) {
    // Ctrl+\ 切换模糊匹配
    if (ksym == XK_backslash) {
      fuzzy = !fuzzy;
      match();
      drawmenu();
      return;
    }
    switch (ksym) {
    case XK_a:
      ksym = XK_Home;
      break;
    case XK_b:
      ksym = XK_Left;
      break;
    case XK_c:
      ksym = XK_Escape;
      break;
    case XK_d:
      ksym = XK_Delete;
      break;
    case XK_e:
      ksym = XK_End;
      break;
    case XK_f:
      ksym = XK_Right;
      break;
    case XK_g:
      ksym = XK_Escape;
      break;
    case XK_h:
      ksym = XK_BackSpace;
      break;
    case XK_i:
      ksym = XK_Tab;
      break;
    case XK_j:
    case XK_J:
    case XK_m:
    case XK_M:
      ksym = XK_Return;
      ev->state &= ~ControlMask;
      break;
    case XK_n:
      ksym = XK_Down;
      break;
    case XK_p:
      ksym = XK_Up;
      break;
    case XK_k:
      text[cursor] = '\0';
      match();
      break;
    case XK_u:
      insert(NULL, 0 - cursor);
      break;

    case XK_w:
      while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
        insert(NULL, nextrune(-1) - cursor);
      while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
        insert(NULL, nextrune(-1) - cursor);
      break;
    case XK_y:
    case XK_Y:
      XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY, utf8,
                        utf8, win, CurrentTime);
      return;
    case XK_Left:
    case XK_KP_Left:
      movewordedge(-1);
      goto draw;
    case XK_Right:
    case XK_KP_Right:
      movewordedge(+1);
      goto draw;
    case XK_Return:
    case XK_KP_Enter:
      break;
    case XK_bracketleft:
      cleanup();
      exit(1);
    default:
      return;
    }
  } else if (ev->state & Mod1Mask) {
    switch (ksym) {
    case XK_b:
      movewordedge(-1);
      goto draw;
    case XK_f:
      movewordedge(+1);
      goto draw;
    case XK_g:
      ksym = XK_Home;
      break;
    case XK_G:
      ksym = XK_End;
      break;
    case XK_h:
      ksym = XK_Up;
      break;
    case XK_j:
      ksym = XK_Next;
      break;
    case XK_k:
      ksym = XK_Prior;
      break;
    case XK_l:
      ksym = XK_Down;
      break;
    default:
      return;
    }
  }
  switch (ksym) {
  default:
  insert:
    if (!iscntrl((unsigned char)*buf))
      insert(buf, len);
    break;
  case XK_Delete:
  case XK_KP_Delete:
    if (text[cursor] == '\0')
      return;
    cursor = nextrune(+1);
    __attribute__((fallthrough));
  case XK_BackSpace:
    if (cursor == 0)
      return;
    insert(NULL, nextrune(-1) - cursor);
    break;
  case XK_End:
  case XK_KP_End:
    if (text[cursor] != '\0') {
      cursor = strlen(text);
      break;
    }
    if (next) {
      curr = matchend;
      calcoffsets();
      curr = prev;
      calcoffsets();
      while (next && (curr = curr->right))
        calcoffsets();
    }
    sel = matchend;
    break;
  case XK_Escape:
    cleanup();
    exit(1);
  case XK_F1:
    help_visible = !help_visible;
    break;
  case XK_Home:
  case XK_KP_Home:
    if (sel == matches) {
      cursor = 0;
      break;
    }
    sel = curr = matches;
    calcoffsets();
    break;
  case XK_Left:
  case XK_KP_Left:
    if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
      cursor = nextrune(-1);
      break;
    }
    if (lines > 0)
      return;
    __attribute__((fallthrough));
  case XK_Up:
  case XK_KP_Up:
    if (sel && sel->left && (sel = sel->left)->right == curr) {
      curr = prev;
      calcoffsets();
    }
    break;
  case XK_Next:
  case XK_KP_Next:
    if (!next)
      return;
    sel = curr = next;
    calcoffsets();
    break;
  case XK_Prior:
  case XK_KP_Prior:
    if (!prev)
      return;
    sel = curr = prev;
    calcoffsets();
    break;
  case XK_Return:
  case XK_KP_Enter: {
    const char *out_text = (sel && !(ev->state & ShiftMask)) ? sel->text : text;
    puts(out_text);
    if (sel && sel != &rawitem && sel->text && *sel->text) {
      recent_save(sel->text);
    }
    if (!(ev->state & ControlMask)) {
      cleanup();
      exit(0);
    }
    if (sel)
      sel->out = 1;
    break;
  }
  case XK_Right:
  case XK_KP_Right:
    if (text[cursor] != '\0') {
      cursor = nextrune(+1);
      break;
    }
    if (lines > 0)
      return;
    __attribute__((fallthrough));
  case XK_Down:
  case XK_KP_Down:
    if (sel && sel->right && (sel = sel->right) == next) {
      curr = next;
      calcoffsets();
    }
    break;
  case XK_Tab:
    if (!sel)
      return;
    cursor = strnlen(sel->text, sizeof text - 1);
    memcpy(text, sel->text, cursor);
    text[cursor] = '\0';
    match();
    break;
  }
draw:
  drawmenu();
}
static void paste(void) {
  char *p, *q;
  int di;
  unsigned long dl;
  Atom da;
  if (XGetWindowProperty(dpy, win, utf8, 0, (sizeof text / 4) + 1, False, utf8,
                         &da, &di, &dl, &dl, (unsigned char **)&p) == Success &&
      p) {
    insert(p, (q = strchr(p, '\n')) ? q - p : (ssize_t)strlen(p));
    XFree(p);
  }
  drawmenu();
}
static void readstdin(void) {
  char *line = NULL;
  size_t i, itemsiz = 0, linesiz = 0;
  ssize_t len;
  for (i = 0; (len = getline(&line, &linesiz, stdin)) != -1; i++) {
    if (i + 1 >= itemsiz) {
      itemsiz += 256;
      if (!(items = realloc(items, itemsiz * sizeof(*items))))
        fprintf(stderr, "cannot realloc %zu bytes:", itemsiz * sizeof(*items));
    }
    if (line[len - 1] == '\n')
      line[len - 1] = '\0';
    if (!(items[i].text = strdup(line)))
      fprintf(stderr, "strdup:");
#ifdef DMENU_CENTER_PATCH
    items[i].width = TEXTW(line);
#endif
    items[i].out = 0;
  }
  free(line);
  if (items)
    items[i].text = NULL;
  lines = MIN(lines, i);
}
static void run(void) {
  XEvent ev;
  while (!XNextEvent(dpy, &ev)) {
    if (XFilterEvent(&ev, win))
      continue;
    switch (ev.type) {
    case DestroyNotify:
      if (ev.xdestroywindow.window != win)
        break;
      cleanup();
      exit(1);
    case Expose:
      if (ev.xexpose.count == 0)
        drw_map(drw, win, 0, 0, mw, mh);
      break;
    case FocusIn:
      if (ev.xfocus.window != win)
        grabfocus();
      break;
    case KeyPress:
      keypress(&ev.xkey);
      break;
    case SelectionNotify:
      if (ev.xselection.property == utf8)
        paste();
      break;
    case VisibilityNotify:
      if (ev.xvisibility.state != VisibilityUnobscured)
        XRaiseWindow(dpy, win);
      break;
    case ButtonPress: {
      XButtonEvent *bev = &ev.xbutton;
      if (bev->button == 4) {
        if (sel && sel->left && (sel = sel->left)->right == curr) {
          curr = prev;
          calcoffsets();
        }
        drawmenu();
      } else if (bev->button == 5) {
        if (sel && sel->right && (sel = sel->right) == next) {
          curr = next;
          calcoffsets();
        }
        drawmenu();
      } else if (bev->button == 1) {
        if (sel) {
          puts(sel->text);
          if (sel != &rawitem)
            recent_save(sel->text);
          cleanup();
          exit(0);
        }
      }
    } break;
    }
  }
}
static void setup(void) {
  int x, y, i, j;
  unsigned int du;
  XSetWindowAttributes swa;
  XIM xim;
  Window w, dw, *dws;
  XWindowAttributes wa;
  XClassHint ch = {"dmenu", "dmenu"};
  XineramaScreenInfo *info;
  Window pw;
  int a, di, n, area = 0;
  for (j = 0; j < SchemeLast; j++)
    scheme[j] = drw_scm_create(drw, colors[j], 2);
  clip = XInternAtom(dpy, "CLIPBOARD", False);
  utf8 = XInternAtom(dpy, "UTF8_STRING", False);
  bh = drw->fonts->h + 2;
  lines = MAX(lines, 0);
  hintbarh = bh;
  mh = (lines + 1) * (unsigned int)bh + (unsigned int)hintbarh;
#ifdef DMENU_CENTER_PATCH
  promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;
#endif
  i = 0;
  if (parentwin == root && (info = XineramaQueryScreens(dpy, &n))) {
    XGetInputFocus(dpy, &w, &di);
    if (mon >= 0 && mon < n)
      i = mon;
    else if (w != root && w != PointerRoot && w != None) {
      do {
        if (XQueryTree(dpy, (pw = w), &dw, &w, &dws, &du) && dws)
          XFree(dws);
      } while (w != root && w != pw);
      if (XGetWindowAttributes(dpy, pw, &wa))
        for (j = 0; j < n; j++)
          if ((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) >
              area) {
            area = a;
            i = j;
          }
    }
    if (mon < 0 && !area &&
        XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
      for (i = 0; i < n; i++)
        if (INTERSECT(x, y, 1, 1, info[i]) != 0)
          break;
#ifndef DMENU_CENTER_PATCH
    x = info[i].x_org;
    y = info[i].y_org + (topbar ? 0 : info[i].height - (int)mh);
    mw = info[i].width;
#elif defined(DMENU_CENTER_PATCH)
    if (centered) {
      mw = MIN(MAX(max_textw() + promptw, min_width), info[i].width);
      x = info[i].x_org + ((info[i].width - mw) / 2);
      y = info[i].y_org + ((info[i].height - mh) / menu_height_ratio);
    } else {
      x = info[i].x_org;
      y = info[i].y_org + (topbar ? 0 : info[i].height - (int)mh);
      mw = info[i].width;
    }
#endif

    XFree(info);
  } else {
    if (!XGetWindowAttributes(dpy, parentwin, &wa))
      fprintf(stderr, "could not get embedding window attributes: 0x%lx",
              parentwin);

#ifndef DMENU_CENTER_PATCH
    x = 0;
    y = topbar ? 0 : wa.height - (int)mh;
    mw = wa.width;
#elif defined(DMENU_CENTER_PATCH)
    if (centered) {
      mw = MIN(MAX(max_textw() + promptw, min_width), wa.width);
      x = (wa.width - mw) / 2;
      y = (wa.height - mh) / 2;
    } else {
      x = 0;
      y = topbar ? 0 : wa.height - mh;
      mw = wa.width;
    }
#endif
  }
  promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;
  inputw = mw / 3;
  match();
  swa.override_redirect = True;
  swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
  swa.event_mask =
      ExposureMask | KeyPressMask | VisibilityChangeMask | ButtonPressMask;
  win = XCreateWindow(dpy, root, x, y, mw, mh, 0, CopyFromParent,
                      CopyFromParent, CopyFromParent,
                      CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
  XSetClassHint(dpy, win, &ch);
  if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
    fprintf(stderr, "XOpenIM failed: could not open input device");
  xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                  XNClientWindow, win, XNFocusWindow, win, NULL);
  XMapRaised(dpy, win);
  if (embed) {
    XReparentWindow(dpy, win, parentwin, x, y);
    XSelectInput(dpy, parentwin, FocusChangeMask | SubstructureNotifyMask);
    if (XQueryTree(dpy, parentwin, &dw, &w, &dws, &du) && dws) {
      for (i = 0; (unsigned int)i < du && dws[i] != win; ++i)
        XSelectInput(dpy, dws[i], FocusChangeMask);
      XFree(dws);
    }
    grabfocus();
  }
  drw_resize(drw, mw, mh);
  drawmenu();
}

void usage(void) {
  fprintf(stderr,
          "Usage: dmenu [OPTIONS]\n"
          "A dynamic menu for X11 that reads input from stdin and displays "
          "selectable items.\n\n"
          "Behavior Options:\n"
          "  -b                Display menu at the bottom of the screen "
          "(default: top)\n"
          "  -f                Fast mode: grab keyboard before reading stdin "
          "(reduces lag)\n"
          "  -i                Case-insensitive item matching (e.g., 'Apple' "
          "matches 'apple')\n"
          "  Ctrl+\\           Toggle fuzzy / substring matching\n"
          "  F1                Toggle keybinding help\n"
#ifdef DMENU_CENTER_PATCH
          "  -c                Let the X11 window be center\n"
#endif
          "  -v                Print version information and exit\n\n"
          "Layout Options:\n"
          "  -l         Display  number of items vertically (default: "
          "horizontal list)\n"
          "  -p        Show  text at the start of the input field (e.g., "
          "'Search:')\n\n"
          "Appearance Options:\n"
          "  -fn         Use  for text rendering (e.g., 'monospace:size=10')\n"
          "  -nb        Normal background color (e.g., '#222222')\n"
          "  -nf        Normal text color (e.g., '#ffffff')\n"
          "  -sb        Selected item background color (e.g., '#0055ff')\n"
          "  -sf        Selected item text color (e.g., '#ffffff')\n\n"
          "Positioning Options:\n"
          "  -m       Display menu on monitor  (0 = first monitor, default: "
          "active monitor)\n"
          "  -w      Embed menu inside the window with ID  (instead of "
          "top-level window)\n\n"
          "Example:\n"
          "  echo -e 'Option 1\nOption 2\nOption 3' | dmenu -l 3 -p 'Select:' "
          "-fn 'sans:size=11' -nb '#1a1a1a' -nf '#f0f0f0'\n");
  exit(0);
}

int main(int argc, char *argv[]) {
#ifdef USER_PROMPT_PATCH
  prompt = getpwuid(getuid()) ? strcat(getpwuid(getuid())->pw_name,
#ifdef PLUS_DEFAULT_PROMPT
                                       "❯ ")
#else
                                       "")
#endif
                              : "Unknown";
#endif

  XWindowAttributes wa;
  int i, fast = 0;
  for (i = 1; i < argc; i++)
    if (!strcmp(argv[i], "-v")) {
      puts("dmenu-" DMENU_VERSION);
      exit(0);
    } else if (!strcmp(argv[i], "-b"))
      topbar = 0;
    else if (!strcmp(argv[i], "-f"))
      fast = 1;

#ifdef DMENU_CENTER_PATCH
    else if (!strcmp(argv[i], "-c"))
      centered = 1;
#endif
    else if (!strcmp(argv[i], "-i")) {
      fstrncmp = strncasecmp;
      fstrstr = cistrstr;
    } else if (i + 1 == argc)
      usage();
    else if (!strcmp(argv[i], "-l"))
      lines = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-m"))
      mon = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-p"))
      prompt = argv[++i];
    else if (!strcmp(argv[i], "-fn"))
      fonts[0] = argv[++i];
    else if (!strcmp(argv[i], "-nb"))
      colors[SchemeNorm][ColBg] = argv[++i];
    else if (!strcmp(argv[i], "-nf"))
      colors[SchemeNorm][ColFg] = argv[++i];
    else if (!strcmp(argv[i], "-sb"))
      colors[SchemeSel][ColBg] = argv[++i];
    else if (!strcmp(argv[i], "-sf"))
      colors[SchemeSel][ColFg] = argv[++i];
    else if (!strcmp(argv[i], "-w"))
      embed = argv[++i];
    else
      usage();
  if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
    fputs("warning: no locale support\n", stderr);
  if (!(dpy = XOpenDisplay(NULL)))
    fprintf(stderr, "cannot open display");
  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);
  if (!embed || !(parentwin = strtol(embed, NULL, 0)))
    parentwin = root;
  if (!XGetWindowAttributes(dpy, parentwin, &wa))
    fprintf(stderr, "could not get embedding window attributes: 0x%lx",
            parentwin);
  drw = drw_create(dpy, screen, root, wa.width, wa.height);

  if (!drw_fontset_create(drw, fonts, sizeof(fonts) / sizeof(fonts[0])))
    fprintf(stderr, "no fonts could be loaded.");
  if (!drw->fonts || !drw->fonts->xfont) {
    fprintf(stderr, "failed to load valid font (check font name/size)");
  }
  lrpad = drw->fonts->h;

  recent_set_path();
  recent_load();

  if (fast && !isatty(0)) {
    grabkeyboard();
    readstdin();
  } else {
    readstdin();
    grabkeyboard();
  }
  setup();
  run();
  return 1;
}