// filename: config.h

#ifdef USER_PROMPT_PATCH // this is litely change the prompt as your USERNAME, for example etcix is my username, prompt shows etcix
#include <sys/types.h>
#include <pwd.h>
#endif
#include <X11/extensions/Xinerama.h>

static int topbar = 0; // 0 for bottom, any other value pos or neg be top
static const char *fonts[] = {
    "Noto Sans:size=32",           // 通用无衬线字体，支持多语言
    "Roboto:size=32",              // 现代简洁字体
    "Helvetica Neue:size=32",      // 经典无衬线字体
    "Arial:size=32",               // 系统常用字体
    "sans-serif:size=32",          // 系统默认字体 fallback
    "WenQuanYi Micro Hei:size=32", // 中文字体支持
    "SimHei:size=32",              // 中文字体 fallback
};
// default prompt
static const char *prompt = "❯ ";
enum Schema
{
    SchemeNorm,
    SchemeSel,
    SchemeOut,
    SchemeHint,
    SchemeLast,
};
static const char *colors[SchemeLast][2] = {
#ifdef GRUVBOX_PATCH
    [SchemeNorm] = {"#ebdbb2", "#282828"},
    [SchemeSel] = {"#ebdbb2", "#98971a"},
    [SchemeOut] = {"#ebdbb2", "#8ec07c"},
    [SchemeHint] = {"#a89984", "#1d2021"},
#elif defined(DACULA_PATCH)
    [SchemeNorm] = {"#f8f8f2", "#282a36"},
    [SchemeSel] = {"#f8f8f2", "#6272a4"},
    [SchemeOut] = {"#000000", "#00ffff"},
    [SchemeHint] = {"#6272a4", "#21222c"},
#elif defined(GRUBER_DARKER_PATCH)
    [SchemeNorm] = {
        "#f4f4ff", // Foreground: rgb(62628,62628,65535) → 62628/256≈244, 65535/256=255 → #f4f4ff
        "#282828"  // Background: rgb(10306,10306,10306) → 10306/256≈40 → #282828 (core dark gray bg)
    },

    /* 1: SchemeSel (selected/highlighted items) */
    [SchemeSel] = {
        "#000000", // Foreground: Match "Grep Search Pattern" fg (rgb(0,0,0)) → #000000 (black text)
        "#f4f4f4"  // Background: Match "Grep Search Pattern" bg (rgb(62915,62915,62915)) → 62915/256≈246 → #f4f4f4 (light gray bg)
    },

    /* 2: SchemeOut (output/result text, e.g., selected value passed to stdout) */
    [SchemeOut] = {
        "#000000", // Foreground: Same as SchemeSel fg (consistent high contrast)
        "#a8a8ff"  // Background: Derived from "PrimaryHighlight" (rgb(24497,25118,32530)) → ~#606280 → adjusted to #a8a8ff (soft blue for output)
    },

    /* 3: SchemeHint (bottom mode line / help text) */
    [SchemeHint] = {
        "#a8a8b0", // 低对比度提示文字
        "#1a1a1a"  // 比主背景更暗，形成 Emacs mode line 效果
    },
#elif defined(XTHEME_PATCH)
    // SchemeNorm: Normal/unselected items (use DMENU_FOREGROUND + DMENU_BACKGROUND)
    [SchemeNorm] = {"#839496", "#002b36"},
    // SchemeSel: Selected/highlighted items (use DMENU_SELFOREGROUND + DMENU_SELBACKGROUND)
    [SchemeSel] = {"#002b36", "#839496"},
    // SchemeOut: Output/result text (use DMENU_OUTFOREGROUND + DMENU_OUTBACKGROUND)
    [SchemeOut] = {"#000000", "#00ffff"},
    // SchemeHint: Bottom mode line / help text
    [SchemeHint] = {"#93a1a1", "#073642"}
#else // default theme
    [SchemeNorm] = {"#f8f8f2", "#282a36"}, // 浅灰文字，深紫背景（类似Dracula主题）
    [SchemeSel] = {"#ffffff", "#bd93f9"},  // 白色文字，紫色高亮（选中项）
    [SchemeOut] = {"#282a36", "#50fa7b"},  // 深色文字，绿色背景（已输出项）
    [SchemeHint] = {"#8a8a9a", "#1e1e2e"}, // 底部提示栏：低对比度文字
#endif
};
static unsigned int lines = 5;
static const char worddelimiters[] = " /?\"&[](){}<>|;:,.!@#$%^&*~";

// center patch
#ifdef DMENU_CENTER_PATCH
int centered = 0;
static int min_width = 500;
static const float menu_height_ratio = 5.0f;
#endif // center patch macro

// version of dmenux.c
#define DMENU_VERSION "5.2"

// config space part
