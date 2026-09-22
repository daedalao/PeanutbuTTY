#ifdef __ALTIVEC__
#include <altivec.h>
#undef bool
#undef vector
#undef pixel
#define vector __vector
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pty.h>
#include <termios.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <getopt.h>
#include <sys/inotify.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include "icon_data.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

/* --- GPU & Performance Constants --- */
#define VBO_COUNT       3
#define ATLAS_SIZE      1024
#define MAX_PARAMS      16
#define MAX_CSI         256
#define MAX_OSC         1024
#define DEFAULT_COLS    80
#define DEFAULT_ROWS    24
#define READ_BUF        16384
#define TITLE_MAX       256

/* --- Runtime configuration (defaults overridable in ~/.config/peanutbutty.conf) --- */
static struct Cfg {
    char font[128];
    int size, padding, scrollback, blink_ms;
    uint32_t fg, bg, cursor;
    uint32_t fg0, bg0, cur0;          /* startup values, for OSC 110/111/112 reset */
    uint32_t pal[16]; int pal_set[16]; /* per-slot palette overrides */
} CFG;

#define COLOR_BG        CFG.bg
#define COLOR_FG        CFG.fg
#define COLOR_CURSOR    CFG.cursor
#define SCROLLBACK      CFG.scrollback
#define BLINK_MS        CFG.blink_ms

static int parse_hex_color(const char *s, uint32_t *out) {
    if (*s == '#') s++;
    char *end; unsigned long v = strtoul(s, &end, 16);
    if (end - s != 6) return 0;
    *out = (uint32_t)v; return 1;
}

static char conf_path_s[512]; /* resolved config file path, for inotify live reload */
static volatile sig_atomic_t reload_requested;
static void on_sigusr1(int s) { (void)s; reload_requested = 1; }

static void config_load(void) {
    strcpy(CFG.font, "monospace");
    CFG.size = 12; CFG.padding = 8; CFG.scrollback = 5000; CFG.blink_ms = 530;
    CFG.fg = 0xc5c8c6; CFG.bg = 0x000000; CFG.cursor = 0xaeafad;
    memset(CFG.pal_set, 0, sizeof(CFG.pal_set));
    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(path, sizeof(path), "%s/peanutbutty.conf", xdg);
    else if (home) snprintf(path, sizeof(path), "%s/.config/peanutbutty.conf", home);
    else path[0] = 0;
    strcpy(conf_path_s, path);
    FILE *f = path[0] ? fopen(path, "r") : NULL;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *p = line; while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || !*p) continue;
            char *eq = strchr(p, '='); if (!eq) continue;
            *eq = 0; char *key = p, *val = eq + 1;
            char *ke = eq; while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
            while (*val == ' ' || *val == '\t') val++;
            char *ve = val + strlen(val); while (ve > val && (ve[-1] == '\n' || ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
            if (!strcmp(key, "font")) { strncpy(CFG.font, val, sizeof(CFG.font)-1); CFG.font[sizeof(CFG.font)-1] = 0; }
            else if (!strcmp(key, "size")) { int v = atoi(val); if (v >= 6 && v <= 72) CFG.size = v; }
            else if (!strcmp(key, "padding")) { int v = atoi(val); if (v >= 0 && v <= 64) CFG.padding = v; }
            else if (!strcmp(key, "scrollback")) { int v = atoi(val); if (v >= 0 && v <= 100000) CFG.scrollback = v > 0 ? v : 1; }
            else if (!strcmp(key, "blink")) { int v = atoi(val); if (v >= 100 && v <= 5000) CFG.blink_ms = v; }
            else if (!strcmp(key, "foreground")) parse_hex_color(val, &CFG.fg);
            else if (!strcmp(key, "background")) parse_hex_color(val, &CFG.bg);
            else if (!strcmp(key, "cursor")) parse_hex_color(val, &CFG.cursor);
            else if (!strncmp(key, "color", 5)) {
                int idx = atoi(key + 5);
                if (idx >= 0 && idx < 16 && parse_hex_color(val, &CFG.pal[idx])) CFG.pal_set[idx] = 1;
            }
        }
        fclose(f);
    }
    CFG.fg0 = CFG.fg; CFG.bg0 = CFG.bg; CFG.cur0 = CFG.cursor;
}

/* --- GL 2.0 Helpers --- */
#ifndef GL_VERTEX_SHADER
#  define GL_VERTEX_SHADER 0x8B31
#  define GL_FRAGMENT_SHADER 0x8B30
#  define GL_COMPILE_STATUS 0x8B81
#  define GL_LINK_STATUS 0x8B82
#  define GL_ARRAY_BUFFER 0x8892
#  define GL_STREAM_DRAW 0x88E0
#  define GL_TEXTURE0 0x84C0
#endif

typedef void (APIENTRY *PFN_glShaderSource) (GLuint,GLsizei,const GLchar*const*,const GLint*);
typedef GLuint (APIENTRY *PFN_glCreateShader) (GLenum);
typedef void (APIENTRY *PFN_glCompileShader) (GLuint);
typedef void (APIENTRY *PFN_glGetShaderiv) (GLuint,GLenum,GLint*);
typedef void (APIENTRY *PFN_glGetShaderInfoLog) (GLuint,GLsizei,GLsizei*,GLchar*);
typedef GLuint (APIENTRY *PFN_glCreateProgram) (void);
typedef void (APIENTRY *PFN_glAttachShader) (GLuint,GLuint);
typedef void (APIENTRY *PFN_glLinkProgram) (GLuint);
typedef void (APIENTRY *PFN_glGetProgramiv) (GLuint,GLenum,GLint*);
typedef void (APIENTRY *PFN_glGetProgramInfoLog) (GLuint,GLsizei,GLsizei*,GLchar*);
typedef void (APIENTRY *PFN_glDeleteShader) (GLuint);
typedef void (APIENTRY *PFN_glUseProgram) (GLuint);
typedef GLint (APIENTRY *PFN_glGetUniformLocation) (GLuint,const GLchar*);
typedef GLint (APIENTRY *PFN_glGetAttribLocation) (GLuint,const GLchar*);
typedef void (APIENTRY *PFN_glUniform1i) (GLint,GLint);
typedef void (APIENTRY *PFN_glUniform1f) (GLint,GLfloat);
typedef void (APIENTRY *PFN_glUniform4f) (GLint,GLfloat,GLfloat,GLfloat,GLfloat);
typedef void (APIENTRY *PFN_glUniformMatrix4fv) (GLint,GLsizei,GLboolean,const GLfloat*);
typedef void (APIENTRY *PFN_glGenBuffers) (GLsizei,GLuint*);
typedef void (APIENTRY *PFN_glBindBuffer) (GLenum,GLuint);
typedef void (APIENTRY *PFN_glBufferData) (GLenum,GLsizeiptr,const void*,GLenum);
typedef void (APIENTRY *PFN_glBufferSubData) (GLenum,GLintptr,GLsizeiptr,const void*);
typedef void (APIENTRY *PFN_glEnableVertexAttribArray) (GLuint);
typedef void (APIENTRY *PFN_glDisableVertexAttribArray) (GLuint);
typedef void (APIENTRY *PFN_glVertexAttribPointer) (GLuint,GLint,GLenum,GLboolean,GLsizei,const void*);
typedef void (APIENTRY *PFN_glActiveTexture) (GLenum);

static PFN_glShaderSource gl_ShaderSource; static PFN_glCreateShader gl_CreateShader;
static PFN_glCompileShader gl_CompileShader; static PFN_glGetShaderiv gl_GetShaderiv;
static PFN_glGetShaderInfoLog gl_GetShaderInfoLog; static PFN_glCreateProgram gl_CreateProgram;
static PFN_glAttachShader gl_AttachShader; static PFN_glLinkProgram gl_LinkProgram;
static PFN_glGetProgramiv gl_GetProgramiv; static PFN_glGetProgramInfoLog gl_GetProgramInfoLog;
static PFN_glDeleteShader gl_DeleteShader; static PFN_glUseProgram gl_UseProgram;
static PFN_glGetUniformLocation gl_GetUniformLocation; static PFN_glGetAttribLocation gl_GetAttribLocation;
static PFN_glUniform1i gl_Uniform1i; static PFN_glUniform1f gl_Uniform1f;
static PFN_glUniform4f gl_Uniform4f; static PFN_glUniformMatrix4fv gl_UniformMatrix4fv;
static PFN_glGenBuffers gl_GenBuffers; static PFN_glBindBuffer gl_BindBuffer;
static PFN_glBufferData gl_BufferData; static PFN_glBufferSubData gl_BufferSubData;
static PFN_glEnableVertexAttribArray gl_EnableVertexAttribArray; static PFN_glDisableVertexAttribArray gl_DisableVertexAttribArray;
static PFN_glVertexAttribPointer gl_VertexAttribPointer; static PFN_glActiveTexture gl_ActiveTexture;

#define GLPROC(type, name) gl_##name = (type)glXGetProcAddress((const GLubyte*)"gl" #name)
static void gl2_load(void) {
    GLPROC(PFN_glShaderSource, ShaderSource); GLPROC(PFN_glCreateShader, CreateShader);
    GLPROC(PFN_glCompileShader, CompileShader); GLPROC(PFN_glGetShaderiv, GetShaderiv);
    GLPROC(PFN_glGetShaderInfoLog, GetShaderInfoLog); GLPROC(PFN_glCreateProgram, CreateProgram);
    GLPROC(PFN_glAttachShader, AttachShader); GLPROC(PFN_glLinkProgram, LinkProgram);
    GLPROC(PFN_glGetProgramiv, GetProgramiv); GLPROC(PFN_glGetProgramInfoLog, GetProgramInfoLog);
    GLPROC(PFN_glDeleteShader, DeleteShader); GLPROC(PFN_glUseProgram, UseProgram);
    GLPROC(PFN_glGetUniformLocation, GetUniformLocation); GLPROC(PFN_glGetAttribLocation, GetAttribLocation);
    GLPROC(PFN_glUniform1i, Uniform1i); GLPROC(PFN_glUniform1f, Uniform1f);
    GLPROC(PFN_glUniform4f, Uniform4f); GLPROC(PFN_glUniformMatrix4fv, UniformMatrix4fv);
    GLPROC(PFN_glGenBuffers, GenBuffers); GLPROC(PFN_glBindBuffer, BindBuffer);
    GLPROC(PFN_glBufferData, BufferData); GLPROC(PFN_glBufferSubData, BufferSubData);
    GLPROC(PFN_glEnableVertexAttribArray, EnableVertexAttribArray); GLPROC(PFN_glDisableVertexAttribArray, DisableVertexAttribArray);
    GLPROC(PFN_glVertexAttribPointer, VertexAttribPointer); GLPROC(PFN_glActiveTexture, ActiveTexture);
}

#define glShaderSource gl_ShaderSource
#define glCreateShader gl_CreateShader
#define glCompileShader gl_CompileShader
#define glGetShaderiv gl_GetShaderiv
#define glGetShaderInfoLog gl_GetShaderInfoLog
#define glCreateProgram gl_CreateProgram
#define glAttachShader gl_AttachShader
#define glLinkProgram gl_LinkProgram
#define glGetProgramiv gl_GetProgramiv
#define glGetProgramInfoLog gl_GetProgramInfoLog
#define glDeleteShader gl_DeleteShader
#define glUseProgram gl_UseProgram
#define glGetUniformLocation gl_GetUniformLocation
#define glGetAttribLocation gl_GetAttribLocation
#define glUniform1i gl_Uniform1i
#define glUniform1f gl_Uniform1f
#define glUniform4f gl_Uniform4f
#define glUniformMatrix4fv gl_UniformMatrix4fv
#define glGenBuffers gl_GenBuffers
#define glBindBuffer gl_BindBuffer
#define glBufferData gl_BufferData
#define glBufferSubData gl_BufferSubData
#define glEnableVertexAttribArray gl_EnableVertexAttribArray
#define glDisableVertexAttribArray gl_DisableVertexAttribArray
#define glVertexAttribPointer gl_VertexAttribPointer
#define glActiveTexture gl_ActiveTexture

/* --- Data Structures --- */
#define ATTR_BOLD       (1 << 0)
#define ATTR_ITALIC     (1 << 1)
#define ATTR_UNDERLINE  (1 << 2)
#define ATTR_BLINK      (1 << 3)
#define ATTR_REVERSE    (1 << 4)
#define ATTR_STRIKE     (1 << 5)
#define ATTR_DIM        (1 << 6)
#define ATTR_INVIS      (1 << 7)
#define ATTR_URL        (1 << 8)
#define ATTR_UNDERCURL  (1 << 9)
#define ATTR_UL_DOUBLE  (1 << 10)

#define UL_DEFAULT 0xFFFFFFFFu /* Cell.ul sentinel: underline uses the fg colour */

typedef struct { uint32_t codepoint; uint32_t fg, bg, ul; uint16_t attrs; } Cell;
typedef struct { Cell *cells; int cols; int dirty; int wrapped; int prompt; } Line;
typedef struct { Line *lines; int head, count, cap; } ScrollBuf;
typedef struct { float x, y, u, v; uint32_t color; uint32_t pad[3]; } Vertex;
typedef struct { int x, y; int pw; uint16_t attrs; uint32_t fg, bg; } SavedCursor;

typedef struct { uint32_t id; int x, y, w, h; GLuint tex; int active; } SixelImg;
typedef struct {
    uint8_t *pix; int w, h, x, y; uint32_t pal[256]; int cur_pal;
    int params[8]; int nparam; char pmode; /* pending '#'/'"'/'!' parameter run */
    int repeat; int max_x, max_y;          /* !n repeat count; content extent for cropping */
} SixelCtx;

typedef struct Terminal {
    int cols, rows; Line *screen, *alt_screen; int alt_active;
    int cx, cy; int cursor_visible, cursor_blink, auto_wrap, origin_mode;
    SavedCursor saved, saved_alt; uint16_t attrs; uint32_t fg, bg;
    int scroll_top, scroll_bottom; int *tabs; ScrollBuf sb; int sb_offset;
    enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_DCS, ST_SIXEL, ST_G0_SET, ST_G1_SET } parser_state;
    char csi_buf[MAX_CSI]; int csi_len; int params[MAX_PARAMS]; int nparams;
    uint32_t param_sub; /* bitmask: params[i] was introduced by ':' (SGR subparameter) */
    char csi_inter, csi_final; char osc_buf[MAX_OSC]; int osc_len;
    char csi_priv; /* private-mode marker byte (0x3C-0x3F: '<' '=' '>' '?'), kept apart from intermediates */
    uint32_t utf8_codepoint; int utf8_remaining;
    int sel_x1, sel_y1, sel_x2, sel_y2; int sel_active;
    char title[TITLE_MAX]; int full_dirty, dirty, synchronized_update;
    uint32_t last_char;
    int charset; /* 0: G0, 1: G1 */
    int g0_charset, g1_charset; /* 0: ASCII, 1: Special Graphics */
    char current_url[MAX_OSC]; SixelImg sixels[64]; int sixel_count;
    SixelCtx sixel_ctx;
    char dcs_type; /* 0: unknown/other, 1: sixel, 2: XTGETTCAP (+q) */
    /* Modern TUI features */
    int mouse_mode; /* 0:off, 1000:click, 1002:motion, 1003:all */
    int mouse_ext;  /* 1006: SGR */
    int focus_report, bracketed_paste, app_cursor_keys, app_keypad;
    int pending_wrap; /* VT100 deferred wrap: set after writing to last column */
    uint32_t ul;      /* current SGR 58 underline colour (UL_DEFAULT = fg) */
    int cursor_style; /* DECSCUSR: 0/1 blink block, 2 block, 3/4 underline, 5/6 bar */
    int has_blink;    /* recomputed each frame: any ATTR_BLINK cell visible */
    uint64_t sync_deadline; /* mode 2026 safety timeout */
    int kitty_stack[8]; int kitty_n; /* kitty keyboard protocol flag stack; [0] is the always-present base */
    char cwd[512];    /* OSC 7 reported working directory (percent-decoded path) */
} Terminal;

#define CP_WIDE_CONTINUATION 0xFFFFFFFE

static struct {
    Display *dpy; Window win; int screen; GLXContext ctx; GLXWindow glx_win;
    int win_w, win_h; GLuint prog; GLint u_proj, u_tex, u_win_h, u_win_w, a_pos, a_uv, a_color;
    Terminal *term; int pty_fd; pid_t child_pid; int cursor_phase, running;
    Cursor ptr_hand, ptr_text;
    char *sel_text;            /* owned selection contents served to requestors */
    int last_mrx, last_mry;    /* last mouse-report cell, to dedupe motion events */
    int focused;               /* window focus, for hollow cursor / blink gating */
    uint64_t bell_until;       /* visual bell: draw border frame until this time */
    int resize_pending, pend_cols, pend_rows; uint64_t resize_deadline; /* debounced reflow */
    uint64_t last_click_ms; int click_count, click_cx, click_cy; /* double/triple click */
    int reinit_needed; /* set when GL context may have been lost (screen blank/resume) */
    XIM xim; XIC xic;  /* input method (ibus/fcitx/dead keys); NULL = raw XLookupString */
    int ino_fd;        /* inotify watch on the config directory for live reload */
    char conf_path[512];
    struct { /* scrollback search (Ctrl+Shift+F) */
        int active; uint32_t q[64]; int qlen;
        int abs;   /* absolute line index (0 = oldest scrollback) of current match, -1 none */
        int count; /* lines with at least one match */
    } search;
    struct { /* keyboard URL hints (Ctrl+Shift+U) */
        int active, n;
        struct { int row, c1, c2; char label; } item[36];
    } hints;
} G;

struct {
    GLuint vbo[VBO_COUNT]; Vertex *vbo_cpu[VBO_COUNT]; int current_vbo;
    int max_vertices; GLuint atlas_tex; FT_Library ft_lib; FT_Face ft_face;
    FT_Face style_faces[4];              /* [style]: 0 regular, 1 bold, 2 italic, 3 bold-italic (NULL = synthesise) */
    FT_Face fb_faces[4]; int fb_count;   /* fallback faces for glyphs the main font lacks */
    FcConfig *fc;                        /* kept alive for lazy fallback lookup */
    int font_sz, font_w, font_h, font_as;
    struct { int u, v, w, h, bl, bt; uint32_t key; } glyphs[4096];
    int glyph_count; int atlas_x, atlas_y, atlas_h;
    uint8_t *atlas_cpu; /* CPU shadow of atlas texture for fast re-upload after context loss */
} G_hw;

/* --- Shaders --- */
static const char *VERT_SRC = "#version 110\nattribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_color;\nuniform mat4 u_proj; varying vec2 v_uv; varying vec4 v_color;\nvoid main() {\ngl_Position = u_proj * vec4(a_pos, 0.0, 1.0); v_uv = a_uv; v_color = a_color;\n}\n";
/* Color packing: R300 GPU reads vertex attribute bytes in memory order as RGBA.
 * rgba_bytes() builds a uint32_t whose memory layout is [r,g,b,a] on either
 * endianness (big-endian G4: r<<24; little-endian POWER8/x86: r in low byte).
 * That is exactly what the GPU sees. DO NOT change the byte order; the GPU
 * is little-endian but reads 4-byte attributes as individual bytes, so no
 * 32-bit word swap occurs for GL_UNSIGNED_BYTE vertex attributes.
 * Scanline and vignette effects removed: too costly on R300 fill rate.
 * Saturation boost (1.4) compensates for muted Tomorrow Night palette; tune as needed. */
static const char *FRAG_SRC =
    "#version 110\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    float mask = texture2D(u_tex, v_uv).a;\n"
    "    vec4 c = v_color * mask;\n"
    "    float luma = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
    "    c.rgb = mix(vec3(luma), c.rgb, 1.4);\n"
    "    gl_FragColor = c;\n"
    "}\n";

/* --- Core Logic --- */
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n"); exit(1);
}

static uint64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

static void set_urgency(int on) {
    XWMHints *h = XGetWMHints(G.dpy, G.win), local;
    if (!h) { memset(&local, 0, sizeof(local)); h = &local; }
    if (on) h->flags |= XUrgencyHint; else h->flags &= ~XUrgencyHint;
    XSetWMHints(G.dpy, G.win, h);
    if (h != &local) XFree(h);
}

static void bell(void) {
    G.bell_until = now_ms() + 150;
    if (!G.focused) set_urgency(1);
}

/* Produce a uint32 whose *memory* byte order is [r,g,b,a] regardless of host
 * endianness. GL_UNSIGNED_BYTE attributes/textures are read byte-by-byte, so
 * the big-endian G4 and little-endian POWER8/x86 need different shifts. */
static inline uint32_t rgba_bytes(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
#else
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
#endif
}

static inline uint32_t pack_color(uint32_t c, uint8_t a) {
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >> 8) & 0xFF;
    uint8_t b = c & 0xFF;
    if (a < 255) {
        r = (uint8_t)((uint32_t)r * a / 255);
        g = (uint8_t)((uint32_t)g * a / 255);
        b = (uint8_t)((uint32_t)b * a / 255);
    }
    return rgba_bytes(r, g, b, a);
}

/* Face that can actually draw cp in the requested style (0 reg, 1 bold,
 * 2 italic, 3 bold-italic): the styled face, the main face, a cached fallback,
 * or a new fallback resolved through fontconfig's coverage query. */
static FT_Face face_for(uint32_t cp, int style) {
    if (style && G_hw.style_faces[style] && FT_Get_Char_Index(G_hw.style_faces[style], cp))
        return G_hw.style_faces[style];
    if (FT_Get_Char_Index(G_hw.ft_face, cp)) return G_hw.ft_face;
    for (int i = 0; i < G_hw.fb_count; i++)
        if (FT_Get_Char_Index(G_hw.fb_faces[i], cp)) return G_hw.fb_faces[i];
    if (G_hw.fb_count < 4 && G_hw.fc) {
        FcPattern *pat = FcPatternCreate();
        FcCharSet *cs = FcCharSetCreate(); FcCharSetAddChar(cs, cp);
        FcPatternAddCharSet(pat, FC_CHARSET, cs);
        FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
        FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
        FcConfigSubstitute(G_hw.fc, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult fres;
        FcPattern *match = FcFontMatch(G_hw.fc, pat, &fres);
        FT_Face nf = NULL;
        if (match) {
            FcChar8 *file = NULL;
            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
                if (FT_New_Face(G_hw.ft_lib, (const char*)file, 0, &nf)) nf = NULL;
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pat); FcCharSetDestroy(cs);
        if (nf) {
            FT_Set_Pixel_Sizes(nf, 0, G_hw.font_sz);
            G_hw.fb_faces[G_hw.fb_count++] = nf;
            if (FT_Get_Char_Index(nf, cp)) return nf;
        }
    }
    return G_hw.ft_face; /* give up: render .notdef */
}

static int glyph_cache[8192];
static int glyph_cache_valid = 0;

static void glyph_cache_reset(void) {
    memset(glyph_cache, -1, sizeof(glyph_cache));
    glyph_cache_valid = 1;
    G_hw.glyph_count = 0;
    G_hw.atlas_x = 18; G_hw.atlas_y = 0; G_hw.atlas_h = 0;
}

/* style: bit0 bold, bit1 italic — part of the atlas key */
static int get_glyph(uint32_t cp, int style) {
    if (!G_hw.ft_face) return 0;
    if (!glyph_cache_valid) glyph_cache_reset();
    if (!G_hw.style_faces[style]) style = 0; /* no real styled face: base glyph, synthesised at draw time */
    uint32_t key = cp | ((uint32_t)style << 22);
    int h = key % 8192;
    if (glyph_cache[h] != -1 && G_hw.glyphs[glyph_cache[h]].key == key) return glyph_cache[h];
    for (int i = 0; i < G_hw.glyph_count; i++) if (G_hw.glyphs[i].key == key) return (glyph_cache[h] = i);
    if (G_hw.glyph_count >= (int)(sizeof(G_hw.glyphs)/sizeof(G_hw.glyphs[0]))) return 0;
    FT_Face face = face_for(cp, style);
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return 0;
    FT_Bitmap *bm = &face->glyph->bitmap;
    if (G_hw.atlas_x + (int)bm->width + 1 >= ATLAS_SIZE) { G_hw.atlas_x = 0; G_hw.atlas_y += G_hw.atlas_h + 1; G_hw.atlas_h = 0; }
    if (G_hw.atlas_y + (int)bm->rows + 1 >= ATLAS_SIZE) return 0;
    int id = G_hw.glyph_count++;
    G_hw.glyphs[id].u = G_hw.atlas_x; G_hw.glyphs[id].v = G_hw.atlas_y; G_hw.glyphs[id].w = bm->width; G_hw.glyphs[id].h = bm->rows;
    G_hw.glyphs[id].bl = face->glyph->bitmap_left; G_hw.glyphs[id].bt = face->glyph->bitmap_top; G_hw.glyphs[id].key = key;
    /* Write into CPU shadow so we can re-upload in one shot after context loss */
    if (G_hw.atlas_cpu) {
        for (unsigned int row = 0; row < bm->rows; row++)
            memcpy(G_hw.atlas_cpu + (G_hw.atlas_y + row) * ATLAS_SIZE + G_hw.atlas_x,
                   bm->buffer + row * bm->pitch, bm->width);
    }
    glBindTexture(GL_TEXTURE_2D, G_hw.atlas_tex); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, G_hw.atlas_x, G_hw.atlas_y, bm->width, bm->rows, GL_LUMINANCE, GL_UNSIGNED_BYTE, bm->buffer);

    G_hw.atlas_x += (int)bm->width + 1; if ((int)bm->rows > G_hw.atlas_h) G_hw.atlas_h = bm->rows;
    return (glyph_cache[h] = id);
}

static void draw_rect_hw(Vertex *vbo, float x, float y, float w, float h, uint32_t color, int *v_idx) {
    Vertex *v = &vbo[*v_idx];
    float u = 8.0f / ATLAS_SIZE, v_uv = 8.0f / ATLAS_SIZE;
    x = floorf(x); y = floorf(y); w = floorf(w); h = floorf(h);
    v[0] = (Vertex){ x, y, u, v_uv, color, {0,0,0} }; v[1] = (Vertex){ x+w, y, u, v_uv, color, {0,0,0} };
    v[2] = (Vertex){ x+w, y+h, u, v_uv, color, {0,0,0} }; v[3] = (Vertex){ x, y+h, u, v_uv, color, {0,0,0} };
    *v_idx += 4;
}

static void draw_glyph_hw(Vertex *vbo, int id, float x, float y, uint32_t color, int italic, int *v_idx) {
    float u0 = (float)G_hw.glyphs[id].u / ATLAS_SIZE, v0 = (float)G_hw.glyphs[id].v / ATLAS_SIZE;
    float u1 = (float)(G_hw.glyphs[id].u + G_hw.glyphs[id].w) / ATLAS_SIZE;
    float v1 = (float)(G_hw.glyphs[id].v + G_hw.glyphs[id].h) / ATLAS_SIZE;
    float ox = floorf(x + (float)G_hw.glyphs[id].bl), oy = floorf(y + (float)(G_hw.font_as - G_hw.glyphs[id].bt));
    float w = (float)G_hw.glyphs[id].w, h = (float)G_hw.glyphs[id].h;
    /* Synthetic oblique: shear the quad's top edge right by ~1/5 of its height */
    float sh = italic ? floorf(h * 0.2f + 0.5f) : 0.0f;
    Vertex *v = &vbo[*v_idx];
    v[0] = (Vertex){ ox+sh, oy, u0, v0, color, {0,0,0} }; v[1] = (Vertex){ ox+w+sh, oy, u1, v0, color, {0,0,0} };
    v[2] = (Vertex){ ox+w, oy+h, u1, v1, color, {0,0,0} }; v[3] = (Vertex){ ox, oy+h, u0, v1, color, {0,0,0} };
    *v_idx += 4;
}

/* --- Procedural box drawing ---------------------------------------------
 * Font glyphs for U+2500-259F rarely fill the whole cell (the advance is
 * narrower than font_w, and font_h includes line gap the glyph never
 * covers), which leaves gaps in tmux borders and TUI frames.  Draw them as
 * exact-cell rectangles instead, like kitty/foot/st-boxdraw do.
 *
 * BOX_MAP encodes U+2500-257F as four 2-bit arm weights
 * (0 none, 1 light, 2 heavy, 3 double: L bits 0-1, R 2-3, U 4-5, D 6-7)
 * plus a dash count in bits 8-10. Zero = not handled (diagonals). */
#define B_(l,r,u,d) ((l) | (r)<<2 | (u)<<4 | (d)<<6)
static const uint16_t BOX_MAP[0x80] = {
    /* 2500 ─ ━ │ ┃ */ B_(1,1,0,0), B_(2,2,0,0), B_(0,0,1,1), B_(0,0,2,2),
    /* 2504 ┄ ┅ ┆ ┇ */ B_(1,1,0,0)|3<<8, B_(2,2,0,0)|3<<8, B_(0,0,1,1)|3<<8, B_(0,0,2,2)|3<<8,
    /* 2508 ┈ ┉ ┊ ┋ */ B_(1,1,0,0)|4<<8, B_(2,2,0,0)|4<<8, B_(0,0,1,1)|4<<8, B_(0,0,2,2)|4<<8,
    /* 250C ┌ ┍ ┎ ┏ */ B_(0,1,0,1), B_(0,2,0,1), B_(0,1,0,2), B_(0,2,0,2),
    /* 2510 ┐ ┑ ┒ ┓ */ B_(1,0,0,1), B_(2,0,0,1), B_(1,0,0,2), B_(2,0,0,2),
    /* 2514 └ ┕ ┖ ┗ */ B_(0,1,1,0), B_(0,2,1,0), B_(0,1,2,0), B_(0,2,2,0),
    /* 2518 ┘ ┙ ┚ ┛ */ B_(1,0,1,0), B_(2,0,1,0), B_(1,0,2,0), B_(2,0,2,0),
    /* 251C ├ ┝ ┞ ┟ */ B_(0,1,1,1), B_(0,2,1,1), B_(0,1,2,1), B_(0,1,1,2),
    /* 2520 ┠ ┡ ┢ ┣ */ B_(0,1,2,2), B_(0,2,2,1), B_(0,2,1,2), B_(0,2,2,2),
    /* 2524 ┤ ┥ ┦ ┧ */ B_(1,0,1,1), B_(2,0,1,1), B_(1,0,2,1), B_(1,0,1,2),
    /* 2528 ┨ ┩ ┪ ┫ */ B_(1,0,2,2), B_(2,0,2,1), B_(2,0,1,2), B_(2,0,2,2),
    /* 252C ┬ ┭ ┮ ┯ */ B_(1,1,0,1), B_(2,1,0,1), B_(1,2,0,1), B_(2,2,0,1),
    /* 2530 ┰ ┱ ┲ ┳ */ B_(1,1,0,2), B_(2,1,0,2), B_(1,2,0,2), B_(2,2,0,2),
    /* 2534 ┴ ┵ ┶ ┷ */ B_(1,1,1,0), B_(2,1,1,0), B_(1,2,1,0), B_(2,2,1,0),
    /* 2538 ┸ ┹ ┺ ┻ */ B_(1,1,2,0), B_(2,1,2,0), B_(1,2,2,0), B_(2,2,2,0),
    /* 253C ┼ ┽ ┾ ┿ */ B_(1,1,1,1), B_(2,1,1,1), B_(1,2,1,1), B_(2,2,1,1),
    /* 2540 ╀ ╁ ╂ ╃ */ B_(1,1,2,1), B_(1,1,1,2), B_(1,1,2,2), B_(2,1,2,1),
    /* 2544 ╄ ╅ ╆ ╇ */ B_(1,2,2,1), B_(2,1,1,2), B_(1,2,1,2), B_(2,2,2,1),
    /* 2548 ╈ ╉ ╊ ╋ */ B_(2,2,1,2), B_(2,1,2,2), B_(1,2,2,2), B_(2,2,2,2),
    /* 254C ╌ ╍ ╎ ╏ */ B_(1,1,0,0)|2<<8, B_(2,2,0,0)|2<<8, B_(0,0,1,1)|2<<8, B_(0,0,2,2)|2<<8,
    /* 2550 ═ ║ ╒ ╓ */ B_(3,3,0,0), B_(0,0,3,3), B_(0,3,0,1), B_(0,1,0,3),
    /* 2554 ╔ ╕ ╖ ╗ */ B_(0,3,0,3), B_(3,0,0,1), B_(1,0,0,3), B_(3,0,0,3),
    /* 2558 ╘ ╙ ╚ ╛ */ B_(0,3,1,0), B_(0,1,3,0), B_(0,3,3,0), B_(3,0,1,0),
    /* 255C ╜ ╝ ╞ ╟ */ B_(1,0,3,0), B_(3,0,3,0), B_(0,3,1,1), B_(0,1,3,3),
    /* 2560 ╠ ╡ ╢ ╣ */ B_(0,3,3,3), B_(3,0,1,1), B_(1,0,3,3), B_(3,0,3,3),
    /* 2564 ╤ ╥ ╦ ╧ */ B_(3,3,0,1), B_(1,1,0,3), B_(3,3,0,3), B_(3,3,1,0),
    /* 2568 ╨ ╩ ╪ ╫ */ B_(1,1,3,0), B_(3,3,3,0), B_(3,3,1,1), B_(1,1,3,3),
    /* 256C ╬ ╭ ╮ ╯ */ B_(3,3,3,3), B_(0,1,0,1), B_(1,0,0,1), B_(1,0,1,0),
    /* 2570 ╰ ╱ ╲ ╳ */ B_(0,1,1,0), 0, 0, 0,
    /* 2574 ╴ ╵ ╶ ╷ */ B_(1,0,0,0), B_(0,0,1,0), B_(0,1,0,0), B_(0,0,0,1),
    /* 2578 ╸ ╹ ╺ ╻ */ B_(2,0,0,0), B_(0,0,2,0), B_(0,2,0,0), B_(0,0,0,2),
    /* 257C ╼ ╽ ╾ ╿ */ B_(1,2,0,0), B_(0,0,1,2), B_(2,1,0,0), B_(0,0,2,1),
};
#undef B_

/* Solid triangle as a degenerate quad (for powerline separators) */
static void draw_tri_hw(Vertex *vbo, float x0, float y0, float x1, float y1,
                        float x2, float y2, uint32_t color, int *v_idx) {
    float u = 8.0f / ATLAS_SIZE;
    Vertex *v = &vbo[*v_idx];
    v[0] = (Vertex){ x0, y0, u, u, color, {0,0,0} };
    v[1] = (Vertex){ x1, y1, u, u, color, {0,0,0} };
    v[2] = (Vertex){ x2, y2, u, u, color, {0,0,0} };
    v[3] = (Vertex){ x2, y2, u, u, color, {0,0,0} };
    *v_idx += 4;
}

/* Returns 1 if cp was drawn procedurally, 0 to fall back to the font. */
static int draw_special_hw(Vertex *vbo, uint32_t cp, float fx, float fy,
                           uint32_t fg, uint8_t alpha, int *v_idx) {
    int w = G_hw.font_w, h = G_hw.font_h;
    int x = (int)fx, y = (int)fy;
    uint32_t col = pack_color(fg, alpha);
    #define RECT(rx, ry, rw, rh) draw_rect_hw(vbo, (float)(rx), (float)(ry), (float)(rw), (float)(rh), col, v_idx)

    if (cp >= 0x2580 && cp <= 0x259F) { /* block elements */
        int wl = w/2, hh = h/2;
        if (cp == 0x2580) RECT(x, y, w, h - h/2);
        else if (cp <= 0x2588) { int k = cp - 0x2580; int bh = h*k/8; if (bh < 1) bh = 1; RECT(x, y+h-bh, w, bh); }
        else if (cp <= 0x258F) { int k = 8 - (int)(cp - 0x2588); int bw = w*k/8; if (bw < 1) bw = 1; RECT(x, y, bw, h); }
        else if (cp == 0x2590) RECT(x+wl, y, w-wl, h);
        else if (cp <= 0x2593) { /* ░▒▓: solid fill at 25/50/75% strength */
            col = pack_color(fg, (uint8_t)(alpha * (cp - 0x2590) / 4));
            RECT(x, y, w, h);
        }
        else if (cp == 0x2594) { int bh = h/8; if (bh < 1) bh = 1; RECT(x, y, w, bh); }
        else if (cp == 0x2595) { int bw = w/8; if (bw < 1) bw = 1; RECT(x+w-bw, y, bw, h); }
        else { /* 2596-259F quadrants: bits UL=1 UR=2 LL=4 LR=8 */
            static const uint8_t qm[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
            uint8_t m = qm[cp - 0x2596];
            if (m & 1) RECT(x, y, wl, hh);
            if (m & 2) RECT(x+wl, y, w-wl, hh);
            if (m & 4) RECT(x, y+hh, wl, h-hh);
            if (m & 8) RECT(x+wl, y+hh, w-wl, h-hh);
        }
        return 1;
    }
    if (cp == 0xE0B0) { draw_tri_hw(vbo, fx, fy, fx+w, fy+h/2.0f, fx, fy+h, col, v_idx); return 1; }
    if (cp == 0xE0B2) { draw_tri_hw(vbo, fx+w, fy, fx, fy+h/2.0f, fx+w, fy+h, col, v_idx); return 1; }
    if (cp < 0x2500 || cp > 0x257F) return 0;

    uint16_t e = BOX_MAP[cp - 0x2500];
    if (!e) return 0; /* diagonals ╱╲╳: the font's slanted strokes are fine */
    int th = h / 16; if (th < 1) th = 1;   /* light stroke */
    int hv = th + 2;                        /* heavy stroke */
    int aL = e & 3, aR = (e >> 2) & 3, aU = (e >> 4) & 3, aD = (e >> 6) & 3;
    int dash = (e >> 8) & 7;
    #define TW(a) ((a) == 1 ? th : (a) == 2 ? hv : (a) == 3 ? 3*th : 0)

    if (dash) { /* pure dashed line, both arms same weight */
        int t = TW(aL ? aL : aU);
        if (aL) {
            int ys = y + (h - t)/2;
            for (int i = 0; i < dash; i++) {
                int x0 = x + w*i/dash, len = (w/dash)*2/3; if (len < 1) len = 1;
                RECT(x0, ys, len, t);
            }
        } else {
            int xs = x + (w - t)/2;
            for (int i = 0; i < dash; i++) {
                int y0 = y + h*i/dash, len = (h/dash)*2/3; if (len < 1) len = 1;
                RECT(xs, y0, t, len);
            }
        }
        return 1;
    }

    /* Junction band: widest of the perpendicular arms, so arms meet cleanly */
    int tv = TW(aU) > TW(aD) ? TW(aU) : TW(aD);
    int tho = TW(aL) > TW(aR) ? TW(aL) : TW(aR);
    int vxs = x + (w - tv)/2, hys = y + (h - tho)/2;

    /* One horizontal (dir 0) or vertical (dir 1) arm; doubles are two strokes */
    #define ARM(a, dir, from_low) do { \
        int t = TW(a); if (!t) break; \
        int lines = (a) == 3 ? 2 : 1, lt = (a) == 3 ? th : t; \
        for (int li = 0; li < lines; li++) { \
            int off = (a) == 3 ? li * 2 * th : 0; \
            if (!(dir)) { \
                int ys = y + (h - t)/2 + off; \
                int e0 = tv ? (from_low ? vxs + tv : vxs) : x + w/2; \
                if (from_low) RECT(x, ys, e0 - x + (tv ? 0 : lt), lt); \
                else RECT(e0 - (tv ? 0 : lt), ys, x + w - e0 + (tv ? 0 : lt), lt); \
            } else { \
                int xs = x + (w - t)/2 + off; \
                int e0 = tho ? (from_low ? hys + tho : hys) : y + h/2; \
                if (from_low) RECT(xs, y, lt, e0 - y + (tho ? 0 : lt)); \
                else RECT(xs, e0 - (tho ? 0 : lt), lt, y + h - e0 + (tho ? 0 : lt)); \
            } \
        } \
    } while (0)

    ARM(aL, 0, 1); ARM(aR, 0, 0); ARM(aU, 1, 1); ARM(aD, 1, 0);
    #undef ARM
    #undef TW
    #undef RECT
    return 1;
}

static uint32_t PALETTE[256];
static void palette_init(void) {
    static const uint32_t a16[16] = { 0x000000, 0xcc6666, 0xb5bd68, 0xde935f, 0x81a2be, 0xb294bb, 0x8abeb7, 0xc5c8c6, 0x969896, 0xcc6666, 0xb5bd68, 0xf0c674, 0x81a2be, 0xb294bb, 0x8abeb7, 0xffffff };
    for (int i=0; i<16; i++) PALETTE[i] = a16[i] & 0xFFFFFF;
    static const int cube[6] = {0, 95, 135, 175, 215, 255};
    for (int i=0; i<216; i++) PALETTE[16+i] = ((cube[i/36]<<16) | (cube[(i/6)%6]<<8) | cube[i%6]) & 0xFFFFFF;
    for (int i=0; i<24; i++) { int v=8+i*10; PALETTE[232+i] = ((v<<16)|(v<<8)|v) & 0xFFFFFF; }
    for (int i=0; i<16; i++) if (CFG.pal_set[i]) PALETTE[i] = CFG.pal[i];
}

/* --- Terminal Emulation Logic --- */
static void term_resize(Terminal *t, int cols, int rows);
static void term_clear_sixels(Terminal *t);
static void utf8_encode(char **p, uint32_t cp);

static Line *alloc_lines(int rows, int cols) {
    Line *l = calloc(rows, sizeof(Line));
    for (int i=0; i<rows; i++) {
        l[i].cells = calloc(cols, sizeof(Cell)); l[i].cols = cols; l[i].dirty = 1;
        for (int j=0; j<cols; j++) { l[i].cells[j].fg = COLOR_FG; l[i].cells[j].bg = COLOR_BG; l[i].cells[j].ul = UL_DEFAULT; l[i].cells[j].codepoint = ' '; }
    }
    return l;
}

static Terminal *term_new(int cols, int rows) {
    Terminal *t = calloc(1, sizeof(Terminal)); t->cols = cols; t->rows = rows; t->last_char = 32;
    t->screen = alloc_lines(rows, cols); t->alt_screen = alloc_lines(rows, cols);
    t->tabs = calloc(cols, sizeof(int)); for (int i=0; i<cols; i+=8) t->tabs[i]=1;
    t->scroll_top = 0; t->scroll_bottom = rows-1; t->cursor_visible = 1; t->cursor_blink = 1; t->auto_wrap = 1;
    t->fg = COLOR_FG; t->bg = COLOR_BG; t->ul = UL_DEFAULT; t->cursor_style = 1;
    t->sb.cap = SCROLLBACK; t->sb.lines = calloc(SCROLLBACK, sizeof(Line));
    for (int i=0; i<SCROLLBACK; i++) { t->sb.lines[i].cols = cols; t->sb.lines[i].cells = calloc(cols, sizeof(Cell)); }
    for (int i=0; i<16; i++) t->sixel_ctx.pal[i] = PALETTE[i];
    t->kitty_n = 1; /* base entry, flags 0 */
    return t;
}

static void sb_push(Terminal *t, Line *src) {
    int idx = (t->sb.head + t->sb.count) % t->sb.cap;
    if (t->sb.count == t->sb.cap) t->sb.head = (t->sb.head + 1) % t->sb.cap; else t->sb.count++;
    if (t->sb.lines[idx].cols != t->cols) { free(t->sb.lines[idx].cells); t->sb.lines[idx].cells = calloc(t->cols, sizeof(Cell)); t->sb.lines[idx].cols = t->cols; }
    memcpy(t->sb.lines[idx].cells, src->cells, t->cols * sizeof(Cell));
    t->sb.lines[idx].wrapped = src->wrapped;
    t->sb.lines[idx].prompt = src->prompt;
}

static void term_clear_line(Terminal *t, Line *l, int from, int to) {
    for (int i=from; i<=to && i<t->cols; i++) { l->cells[i].codepoint = ' '; l->cells[i].fg = t->fg; l->cells[i].bg = t->bg; l->cells[i].ul = UL_DEFAULT; l->cells[i].attrs = 0; }
    if (from <= 0 && to >= t->cols-1) { l->wrapped = 0; l->prompt = 0; }
}

static void term_scroll_up(Terminal *t, int n) {
    Line *scr = t->alt_active ? t->alt_screen : t->screen;
    for (int j=0; j<n; j++) {
        if (!t->alt_active && t->scroll_top == 0 && t->scroll_bottom == t->rows-1) sb_push(t, &scr[t->scroll_top]);
        Line tmp = scr[t->scroll_top];
        for (int i=t->scroll_top; i<t->scroll_bottom; i++) scr[i] = scr[i+1];
        scr[t->scroll_bottom] = tmp; term_clear_line(t, &scr[t->scroll_bottom], 0, t->cols-1);
    }
}

static void term_scroll_down(Terminal *t, int n) {
    Line *scr = t->alt_active ? t->alt_screen : t->screen;
    for (int j=0; j<n; j++) {
        Line tmp = scr[t->scroll_bottom];
        for (int i=t->scroll_bottom; i>t->scroll_top; i--) scr[i] = scr[i-1];
        scr[t->scroll_top] = tmp; term_clear_line(t, &scr[t->scroll_top], 0, t->cols-1);
    }
}

static void term_putchar(Terminal *t, uint32_t cp) {
    int active_cs = (t->charset == 0) ? t->g0_charset : t->g1_charset;
    if (active_cs == 1 && cp >= 0x5F && cp <= 0x7E) {
        static const uint32_t vt100_map[] = {
            0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
            0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7
        };
        cp = vt100_map[cp - 0x5F];
    }
    int w = wcwidth(cp); if (w < 1) w = 1;
    Line *scr = t->alt_active ? t->alt_screen : t->screen;

    /* VT100 deferred wrap: if pending and autowrap on, wrap now before placing char */
    if (t->pending_wrap && t->auto_wrap) {
        scr[t->cy].wrapped = 1; /* this line continues on the next: reflow joins them */
        t->cx = 0; t->pending_wrap = 0;
        if (++t->cy > t->scroll_bottom) { t->cy = t->scroll_bottom; term_scroll_up(t, 1); }
        scr = t->alt_active ? t->alt_screen : t->screen;
    } else {
        t->pending_wrap = 0;
    }

    /* If wide char won't fit on this line, wrap it */
    if (t->cx + w > t->cols) {
        if (!t->auto_wrap) return; /* no room and no wrap: discard */
        scr[t->cy].wrapped = 1;
        t->cx = 0; if (++t->cy > t->scroll_bottom) { t->cy = t->scroll_bottom; term_scroll_up(t, 1); }
        scr = t->alt_active ? t->alt_screen : t->screen;
    }

    Cell *cell = &scr[t->cy].cells[t->cx];
    cell->codepoint = cp; cell->attrs = t->attrs; cell->fg = t->fg; cell->bg = t->bg; cell->ul = t->ul;
    t->last_char = cp;

    if (w == 2 && t->cx < t->cols - 1) {
        scr[t->cy].cells[t->cx + 1].codepoint = CP_WIDE_CONTINUATION;
        scr[t->cy].cells[t->cx + 1].attrs = t->attrs;
        scr[t->cy].cells[t->cx + 1].fg = t->fg;
        scr[t->cy].cells[t->cx + 1].bg = t->bg;
        scr[t->cy].cells[t->cx + 1].ul = t->ul;
    }

    t->cx += w;
    if (t->cx >= t->cols) {
        /* Don't wrap yet — set pending flag, clamp cursor to last column */
        t->pending_wrap = t->auto_wrap;
        t->cx = t->cols - 1;
    }
}

#define UL_MASK (ATTR_UNDERLINE | ATTR_UNDERCURL | ATTR_UL_DOUBLE)

/* Parse extended colour after a 38/48/58 introducer at index *pi.
 * Handles both ;-separated and :-subparameter forms, with or without the
 * colour-space id (38:2::R:G:B). Advances *pi past consumed params. */
static int sgr_ext_color(Terminal *t, int *pi, uint32_t *out) {
    int i = *pi;
    if (i+1 >= t->nparams) return 0;
    if (t->params[i+1] == 5 && i+2 < t->nparams) {
        *out = PALETTE[t->params[i+2] & 0xFF]; *pi = i+2; return 1;
    }
    if (t->params[i+1] == 2) {
        int subs = 0;
        for (int k = i+1; k < t->nparams && ((t->param_sub >> k) & 1); k++) subs++;
        int j = (subs >= 5) ? i+3 : i+2; /* skip colour-space id in the long colon form */
        if (j+2 < t->nparams+0 && j+2 <= t->nparams-1) {
            *out = ((t->params[j] & 0xFF) << 16) | ((t->params[j+1] & 0xFF) << 8) | (t->params[j+2] & 0xFF);
            *pi = j+2; return 1;
        }
    }
    return 0;
}

static void term_sgr(Terminal *t) {
    if (t->nparams == 0) { t->attrs = 0; t->fg = COLOR_FG; t->bg = COLOR_BG; t->ul = UL_DEFAULT; return; }
    for (int i=0; i<t->nparams; i++) {
        int p = t->params[i];
        uint32_t c;
        if (p == 0) { t->attrs = 0; t->fg = COLOR_FG; t->bg = COLOR_BG; t->ul = UL_DEFAULT; }
        else if (p == 1) t->attrs |= ATTR_BOLD;
        else if (p == 2) t->attrs |= ATTR_DIM;
        else if (p == 3) t->attrs |= ATTR_ITALIC;
        else if (p == 4) {
            if (i+1 < t->nparams && ((t->param_sub >> (i+1)) & 1)) { /* CSI 4:x m underline styles */
                int st = t->params[++i];
                t->attrs &= ~UL_MASK;
                if (st == 1) t->attrs |= ATTR_UNDERLINE;
                else if (st == 2) t->attrs |= ATTR_UL_DOUBLE;
                else if (st >= 3 && st <= 5) t->attrs |= ATTR_UNDERCURL; /* curly/dotted/dashed */
            } else t->attrs = (t->attrs & ~UL_MASK) | ATTR_UNDERLINE;
        }
        else if (p == 5) t->attrs |= ATTR_BLINK;
        else if (p == 7) t->attrs |= ATTR_REVERSE;
        else if (p == 8) t->attrs |= ATTR_INVIS;
        else if (p == 9) t->attrs |= ATTR_STRIKE;
        else if (p == 21) t->attrs = (t->attrs & ~UL_MASK) | ATTR_UL_DOUBLE;
        else if (p == 22) t->attrs &= ~(ATTR_BOLD | ATTR_DIM);
        else if (p == 23) t->attrs &= ~ATTR_ITALIC;
        else if (p == 24) t->attrs &= ~UL_MASK;
        else if (p == 25) t->attrs &= ~ATTR_BLINK;
        else if (p == 27) t->attrs &= ~ATTR_REVERSE;
        else if (p == 28) t->attrs &= ~ATTR_INVIS;
        else if (p == 29) t->attrs &= ~ATTR_STRIKE;
        else if (p >= 30 && p <= 37) t->fg = PALETTE[p-30];
        else if (p == 38) { if (sgr_ext_color(t, &i, &c)) t->fg = c; else break; }
        else if (p == 39) t->fg = COLOR_FG;
        else if (p >= 40 && p <= 47) t->bg = PALETTE[p-40];
        else if (p == 48) { if (sgr_ext_color(t, &i, &c)) t->bg = c; else break; }
        else if (p == 49) t->bg = COLOR_BG;
        else if (p == 58) { if (sgr_ext_color(t, &i, &c)) t->ul = c; else break; }
        else if (p == 59) t->ul = UL_DEFAULT;
        else if (p >= 90 && p <= 97) t->fg = PALETTE[p-90+8];
        else if (p >= 100 && p <= 107) t->bg = PALETTE[p-100+8];
    }
}

static void term_insert_lines(Terminal *t, int n) {
    Line *scr = t->alt_active ? t->alt_screen : t->screen;
    if (t->cy < t->scroll_top || t->cy > t->scroll_bottom) return;
    if (n > t->scroll_bottom - t->cy + 1) n = t->scroll_bottom - t->cy + 1;
    for (int i=0; i<n; i++) {
        Line tmp = scr[t->scroll_bottom];
        for (int r=t->scroll_bottom; r>t->cy; r--) scr[r] = scr[r-1];
        scr[t->cy] = tmp; term_clear_line(t, &scr[t->cy], 0, t->cols-1);
    }
}

static void term_delete_lines(Terminal *t, int n) {
    Line *scr = t->alt_active ? t->alt_screen : t->screen;
    if (t->cy < t->scroll_top || t->cy > t->scroll_bottom) return;
    if (n > t->scroll_bottom - t->cy + 1) n = t->scroll_bottom - t->cy + 1;
    for (int i=0; i<n; i++) {
        Line tmp = scr[t->cy];
        for (int r=t->cy; r<t->scroll_bottom; r++) scr[r] = scr[r+1];
        scr[t->scroll_bottom] = tmp; term_clear_line(t, &scr[t->scroll_bottom], 0, t->cols-1);
    }
}

/* DECRQM status of a DEC private mode: 0 unrecognized, 1 set, 2 reset */
static int dec_mode_status(Terminal *t, int m) {
    switch (m) {
        case 1:    return t->app_cursor_keys ? 1 : 2;
        case 6:    return t->origin_mode ? 1 : 2;
        case 7:    return t->auto_wrap ? 1 : 2;
        case 25:   return t->cursor_visible ? 1 : 2;
        case 47: case 1047: case 1049: return t->alt_active ? 1 : 2;
        case 1000: case 1002: case 1003: return t->mouse_mode == m ? 1 : 2;
        case 1006: return t->mouse_ext == 1006 ? 1 : 2;
        case 1004: return t->focus_report ? 1 : 2;
        case 2004: return t->bracketed_paste ? 1 : 2;
        case 2026: return t->synchronized_update ? 1 : 2;
        default:   return 0;
    }
}

static void term_csi(Terminal *t) {
    t->pending_wrap = 0;
    int p1 = t->nparams > 0 && t->params[0] > 0 ? t->params[0] : 1;
    int p2 = t->nparams > 1 && t->params[1] > 0 ? t->params[1] : 1;
    Line *scr = t->alt_active ? t->alt_screen : t->screen;

    switch (t->csi_final) {
        /* CUU/CUD/CNL/CPL: clamp to screen edges (not scroll region) in normal mode */
        case 'A': t->cy -= p1; if (t->cy < 0) t->cy = 0; break;
        case 'B': t->cy += p1; if (t->cy >= t->rows) t->cy = t->rows - 1; break;
        case 'C': t->cx = fmin(t->cols - 1, t->cx + p1); break;
        case 'D': t->cx = fmax(0, t->cx - p1); break;
        case 'E': t->cy += p1; if (t->cy >= t->rows) t->cy = t->rows - 1; t->cx = 0; break;
        case 'F': t->cy -= p1; if (t->cy < 0) t->cy = 0; t->cx = 0; break;
        case 'G': t->cx = fmin(t->cols - 1, p1 - 1); break;
        case 'H': case 'f': {
            int top = t->origin_mode ? t->scroll_top : 0;
            int bot = t->origin_mode ? t->scroll_bottom : t->rows - 1;
            t->cy = top + p1 - 1; if (t->cy > bot) t->cy = bot; if (t->cy < top) t->cy = top;
            t->cx = fmin(t->cols - 1, p2 - 1); break;
        }
        case 'J': {
            /* Default param is 0 (erase cursor to end), NOT 1 */
            int jp = t->nparams > 0 ? t->params[0] : 0;
            if (jp == 0) { term_clear_line(t, &scr[t->cy], t->cx, t->cols-1); for (int r=t->cy+1; r<t->rows; r++) term_clear_line(t, &scr[r], 0, t->cols-1); }
            else if (jp == 1) { term_clear_line(t, &scr[t->cy], 0, t->cx); for (int r=0; r<t->cy; r++) term_clear_line(t, &scr[r], 0, t->cols-1); }
            else { for (int r=0; r<t->rows; r++) term_clear_line(t, &scr[r], 0, t->cols-1); term_clear_sixels(t); }
            break;
        }
        case 'K': {
            /* Default param is 0 (erase cursor to end of line), NOT 1 */
            int kp = t->nparams > 0 ? t->params[0] : 0;
            if (kp == 0) term_clear_line(t, &scr[t->cy], t->cx, t->cols-1);
            else if (kp == 1) term_clear_line(t, &scr[t->cy], 0, t->cx);
            else term_clear_line(t, &scr[t->cy], 0, t->cols-1);
            break;
        }
        case '@': /* ICH: Insert Character */
            {
                int n = p1; if (n > t->cols - t->cx) n = t->cols - t->cx;
                memmove(&scr[t->cy].cells[t->cx+n], &scr[t->cy].cells[t->cx], (t->cols-t->cx-n)*sizeof(Cell));
                term_clear_line(t, &scr[t->cy], t->cx, t->cx+n-1);
            }
            break;
        case 'L': term_insert_lines(t, p1); break;
        case 'M': term_delete_lines(t, p1); break;
        case 'P': /* DCH: Delete Character */
            {
                int n = p1; if (n > t->cols - t->cx) n = t->cols - t->cx;
                memmove(&scr[t->cy].cells[t->cx], &scr[t->cy].cells[t->cx+n], (t->cols-t->cx-n)*sizeof(Cell));
                term_clear_line(t, &scr[t->cy], t->cols-n, t->cols-1);
            }
            break;
        case 'S': term_scroll_up(t, p1); break;
        case 'T': term_scroll_down(t, p1); break;
        case 'X': /* ECH: Erase Character */
            term_clear_line(t, &scr[t->cy], t->cx, fmin(t->cols-1, t->cx + p1 - 1)); break;
        case 'd': t->cy = fmin(t->rows - 1, p1 - 1); break;
        case 'm':
            if (t->csi_priv == '>') break; /* Xterm: Modify Other Keys, just ignore */
            term_sgr(t); break;
        case 'r': {
            int _top = (t->nparams > 0 && t->params[0] > 0) ? t->params[0] - 1 : 0;
            int _bot = (t->nparams > 1 && t->params[1] > 0) ? t->params[1] - 1 : t->rows - 1;
            t->scroll_top = (_top < t->rows) ? _top : 0;
            t->scroll_bottom = (_bot < t->rows) ? _bot : t->rows - 1;
            if (t->scroll_top >= t->scroll_bottom) { t->scroll_top = 0; t->scroll_bottom = t->rows - 1; }
            /* DECSTBM homes cursor per VT spec (to origin if DECOM active) */
            t->cx = 0; t->cy = t->origin_mode ? t->scroll_top : 0;
            break;
        }
        case 's':
            t->saved.x = t->cx; t->saved.y = t->cy; t->saved.pw = t->pending_wrap;
            t->saved.attrs = t->attrs; t->saved.fg = t->fg; t->saved.bg = t->bg;
            break;
        case 'u': /* kitty keyboard protocol (with private marker) or ANSI restore cursor */
            if (t->csi_priv == '?') { /* query current flags */
                char b[16]; int n = sprintf(b, "\033[?%du", t->kitty_stack[t->kitty_n-1]);
                write(G.pty_fd, b, n);
            } else if (t->csi_priv == '>') { /* push flags (default 1) */
                int f = t->nparams > 0 ? t->params[0] : 1;
                if (t->kitty_n < 8) t->kitty_stack[t->kitty_n++] = f;
                else t->kitty_stack[7] = f;
            } else if (t->csi_priv == '<') { /* pop n entries (default 1) */
                int n = t->nparams > 0 && t->params[0] > 0 ? t->params[0] : 1;
                t->kitty_n -= n; if (t->kitty_n < 1) t->kitty_n = 1;
            } else if (t->csi_priv == '=') { /* set flags on the current entry */
                t->kitty_stack[t->kitty_n-1] = t->nparams > 0 ? t->params[0] : 0;
            } else {
                t->cx = t->saved.x; t->cy = t->saved.y; t->pending_wrap = t->saved.pw;
                t->attrs = t->saved.attrs; t->fg = t->saved.fg; t->bg = t->saved.bg;
            }
            break;
        case 'b': /* REP: Repeat Character */
            if (t->last_char >= 32) {
                for (int i=0; i<p1; i++) term_putchar(t, t->last_char);
            }
            break;
        case 'c': /* DA: Device Attributes */
            if (t->csi_priv == '>') write(G.pty_fd, "\033[>1;10;0c", 10); /* Secondary DA */
            else write(G.pty_fd, "\033[?62;4c", 8); /* Primary DA: VT220 with Sixel */
            break;
        case 'q': /* DECSCUSR: CSI Ps SP q — cursor shape */
            if (t->csi_inter == ' ') {
                int s = t->nparams > 0 ? t->params[0] : 0;
                if (s < 0 || s > 6) s = 1;
                t->cursor_style = s ? s : 1;
            } /* '"' = DECSCA, ignore */
            break;
        case 'n': /* DSR: Device Status Report / CPR: Cursor Position Report */
            if (p1 == 5) write(G.pty_fd, "\033[0n", 4);
            else if (p1 == 6) { char _b[32]; int _n=sprintf(_b, "\033[%d;%dR", t->cy+1, t->cx+1); write(G.pty_fd, _b, _n); }
            break;
        case 'p': /* DECRQM: CSI ? Ps $ p (private) / CSI Ps $ p (ANSI) */
            if (t->csi_inter == '$') {
                char b[32]; int n;
                int m = t->nparams > 0 ? t->params[0] : 0;
                if (t->csi_priv == '?') n = sprintf(b, "\033[?%d;%d$y", m, dec_mode_status(t, m));
                else n = sprintf(b, "\033[%d;0$y", m); /* no ANSI modes tracked */
                write(G.pty_fd, b, n);
            }
            break;
        case 'h':
            if (t->csi_priv == '?') {
                for (int i=0; i<t->nparams; i++) {
                    int p = t->params[i];
                    if (p == 1) t->app_cursor_keys = 1;
                    else if (p == 6) { t->origin_mode = 1; t->cx = 0; t->cy = t->scroll_top; }
                    else if (p == 7) t->auto_wrap = 1;
                    else if (p == 25) t->cursor_visible = 1;
                    else if (p == 47 || p == 1047) t->alt_active = 1;
                    else if (p == 1000) t->mouse_mode = 1000;
                    else if (p == 1002) t->mouse_mode = 1002;
                    else if (p == 1003) t->mouse_mode = 1003;
                    else if (p == 1006) t->mouse_ext = 1006;
                    else if (p == 1004) t->focus_report = 1;
                    else if (p == 2004) t->bracketed_paste = 1;
                    else if (p == 1049) {
                        /* Save cursor, clear alt screen, switch to it */
                        t->saved_alt.x = t->cx; t->saved_alt.y = t->cy;
                        t->saved_alt.fg = t->fg; t->saved_alt.bg = t->bg; t->saved_alt.attrs = t->attrs;
                        Line *_alt = t->alt_screen;
                        for (int _r = 0; _r < t->rows; _r++) term_clear_line(t, &_alt[_r], 0, t->cols - 1);
                        term_clear_sixels(t);
                        t->alt_active = 1; t->cx = 0; t->cy = 0; t->sb_offset = 0;
                    }
                    else if (p == 2026) { t->synchronized_update = 1; t->sync_deadline = now_ms() + 150; }
                }
            }
            break;
        case 'l':
            if (t->csi_priv == '?') {
                for (int i=0; i<t->nparams; i++) {
                    int p = t->params[i];
                    if (p == 1) t->app_cursor_keys = 0;
                    else if (p == 6) { t->origin_mode = 0; t->cx = 0; t->cy = 0; }
                    else if (p == 7) t->auto_wrap = 0;
                    else if (p == 25) t->cursor_visible = 0;
                    else if (p == 47 || p == 1047) t->alt_active = 0;
                    else if (p == 1000 || p == 1002 || p == 1003) t->mouse_mode = 0;
                    else if (p == 1006) t->mouse_ext = 0;
                    else if (p == 1004) t->focus_report = 0;
                    else if (p == 2004) t->bracketed_paste = 0;
                    else if (p == 1049) {
                        /* Restore cursor and switch back to main screen */
                        t->alt_active = 0;
                        t->cx = t->saved_alt.x; t->cy = t->saved_alt.y;
                        t->fg = t->saved_alt.fg; t->bg = t->saved_alt.bg; t->attrs = t->saved_alt.attrs;
                    }
                    else if (p == 2026) t->synchronized_update = 0;
                }
            }
            break;
        case 't': /* Window Management */
            if (p1 == 14) { char b[32]; int n = sprintf(b, "\033[4;%d;%dt", t->rows * G_hw.font_h, t->cols * G_hw.font_w); write(G.pty_fd, b, n); }
            else if (p1 == 18) { char b[32]; int n = sprintf(b, "\033[8;%d;%dt", t->rows, t->cols); write(G.pty_fd, b, n); }
            break;
    }
}

static int utf8_decode(Terminal *t, uint8_t byte, uint32_t *cp) {
    if (t->utf8_remaining > 0) {
        if ((byte & 0xC0) == 0x80) { t->utf8_codepoint = (t->utf8_codepoint << 6) | (byte & 0x3F);
            if (--t->utf8_remaining == 0) { *cp = t->utf8_codepoint; return 1; } return 0; }
        t->utf8_remaining = 0;
    }
    if (byte < 0x80) { *cp = byte; return 1; }
    if ((byte & 0xE0) == 0xC0) { t->utf8_codepoint = byte & 0x1F; t->utf8_remaining = 1; return 0; }
    if ((byte & 0xF0) == 0xE0) { t->utf8_codepoint = byte & 0x0F; t->utf8_remaining = 2; return 0; }
    if ((byte & 0xF8) == 0xF0) { t->utf8_codepoint = byte & 0x07; t->utf8_remaining = 3; return 0; }
    *cp = 0xFFFD; return 1;
}

/* X11 colour spec: "#RRGGBB" or "rgb:RR/GG/BB" (1-4 hex digits per channel) */
static int parse_color_spec(const char *s, uint32_t *out) {
    if (s[0] == '#') return parse_hex_color(s, out);
    if (strncmp(s, "rgb:", 4)) return 0;
    s += 4;
    uint32_t comp[3];
    for (int i = 0; i < 3; i++) {
        char *end; unsigned long v = strtoul(s, &end, 16);
        int digits = (int)(end - s);
        if (digits < 1 || digits > 4) return 0;
        if (digits == 1) v *= 17;
        else if (digits == 3) v >>= 4;
        else if (digits == 4) v >>= 8;
        comp[i] = (uint32_t)v & 0xFF;
        s = end;
        if (i < 2) { if (*s != '/') return 0; s++; }
    }
    *out = (comp[0] << 16) | (comp[1] << 8) | comp[2];
    return 1;
}

static const char B64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_decode(const char *s, size_t *outlen) {
    static int8_t rev[256]; static int rev_init = 0;
    if (!rev_init) { memset(rev, -1, sizeof(rev)); for (int i = 0; i < 64; i++) rev[(uint8_t)B64_CHARS[i]] = i; rev_init = 1; }
    size_t len = strlen(s);
    char *out = malloc(len * 3 / 4 + 4); size_t o = 0;
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int8_t v = rev[(uint8_t)s[i]];
        if (v < 0) continue; /* skip '=', whitespace, junk */
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (char)(acc >> bits); }
    }
    out[o] = 0; *outlen = o;
    return out;
}

static char *b64_encode(const char *s, size_t len) {
    char *out = malloc((len + 2) / 3 * 4 + 1); size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint8_t)s[i] << 16;
        if (i+1 < len) v |= (uint8_t)s[i+1] << 8;
        if (i+2 < len) v |= (uint8_t)s[i+2];
        out[o++] = B64_CHARS[(v >> 18) & 63]; out[o++] = B64_CHARS[(v >> 12) & 63];
        out[o++] = i+1 < len ? B64_CHARS[(v >> 6) & 63] : '=';
        out[o++] = i+2 < len ? B64_CHARS[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

static void retint_defaults(uint32_t ofg, uint32_t obg);
static void term_handle_osc(Terminal *t) {
    if (t->osc_len < 1) return;
    t->osc_buf[t->osc_len] = 0;
    char *arg = strchr(t->osc_buf, ';');
    int code = atoi(t->osc_buf);
    if (arg) arg++;
    if (code == 0 || code == 2) {
        if (arg) {
            strncpy(t->title, arg, TITLE_MAX-1);
            t->title[TITLE_MAX-1]=0;
            XStoreName(G.dpy, G.win, t->title);
        }
    } else if (code == 4 && arg) { /* set/query palette entries: 4;idx;spec[;idx;spec...] */
        char *p = arg;
        while (p && *p) {
            int idx = atoi(p);
            char *spec = strchr(p, ';'); if (!spec) break;
            spec++;
            char *next = strchr(spec, ';'); if (next) *next++ = 0;
            if (idx >= 0 && idx < 256) {
                if (spec[0] == '?') {
                    uint32_t c = PALETTE[idx]; char resp[64];
                    int n = sprintf(resp, "\033]4;%d;rgb:%02x%02x/%02x%02x/%02x%02x\007", idx,
                                    (c>>16)&0xFF,(c>>16)&0xFF,(c>>8)&0xFF,(c>>8)&0xFF,c&0xFF,c&0xFF);
                    write(G.pty_fd, resp, n);
                } else parse_color_spec(spec, &PALETTE[idx]);
            }
            p = next;
        }
        t->full_dirty = 1;
    } else if (code == 104) { /* reset palette (entry list ignored: full reset) */
        palette_init();
        t->full_dirty = 1;
    } else if ((code == 10 || code == 11 || code == 12) && arg) { /* default fg/bg/cursor: query or set */
        uint32_t *tgt = code == 10 ? &CFG.fg : code == 11 ? &CFG.bg : &CFG.cursor;
        if (arg[0] == '?') {
            uint32_t c = *tgt; char resp[64];
            int n = sprintf(resp, "\033]%d;rgb:%02x%02x/%02x%02x/%02x%02x\007", code,
                            (c>>16)&0xFF, (c>>16)&0xFF, (c>>8)&0xFF, (c>>8)&0xFF, c&0xFF, c&0xFF);
            write(G.pty_fd, resp, n);
        } else { uint32_t ofg = CFG.fg, obg = CFG.bg; parse_color_spec(arg, tgt); retint_defaults(ofg, obg); }
    } else if (code == 110) { uint32_t o = CFG.fg; CFG.fg = CFG.fg0; retint_defaults(o, CFG.bg); }
    else if (code == 111) { uint32_t o = CFG.bg; CFG.bg = CFG.bg0; retint_defaults(CFG.fg, o); }
    else if (code == 112) CFG.cursor = CFG.cur0;
    else if (code == 52 && arg) { /* clipboard access: 52;<sel>;<base64|?> */
        char *data = strchr(arg, ';');
        if (data) {
            char sel_kind = arg[0]; data++;
            if (data[0] == '?') { /* query: reply with our clipboard text */
                char *b64 = b64_encode(G.sel_text ? G.sel_text : "", G.sel_text ? strlen(G.sel_text) : 0);
                char pre[32]; int pn = sprintf(pre, "\033]52;%c;", sel_kind ? sel_kind : 'c');
                write(G.pty_fd, pre, pn); write(G.pty_fd, b64, strlen(b64)); write(G.pty_fd, "\007", 1);
                free(b64);
            } else { /* set: remote copy lands in our CLIPBOARD (and PRIMARY for 'p') */
                size_t dl; char *txt = b64_decode(data, &dl);
                free(G.sel_text); G.sel_text = txt;
                XSetSelectionOwner(G.dpy, XInternAtom(G.dpy, "CLIPBOARD", False), G.win, CurrentTime);
                for (const char *q = arg; q < data - 1; q++)
                    if (*q == 'p') { XSetSelectionOwner(G.dpy, XA_PRIMARY, G.win, CurrentTime); break; }
            }
        }
    } else if (code == 133 && arg) { /* semantic prompts: A = prompt start marks the line */
        if (arg[0] == 'A' && !t->alt_active) {
            Line *scr = t->screen;
            scr[t->cy].prompt = 1;
        }
    } else if (code == 7 && arg) { /* working directory: 7;file://host/path */
        const char *p = strncmp(arg, "file://", 7) ? arg : arg + 7;
        if (*p && *p != '/') { p = strchr(p, '/'); if (!p) p = ""; } /* skip hostname */
        int o = 0;
        while (*p && o < (int)sizeof(t->cwd)-1) { /* percent-decode */
            if (p[0] == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
                int hi = isdigit(p[1]) ? p[1]-'0' : (tolower(p[1])-'a'+10);
                int lo = isdigit(p[2]) ? p[2]-'0' : (tolower(p[2])-'a'+10);
                t->cwd[o++] = (char)((hi << 4) | lo); p += 3;
            } else t->cwd[o++] = *p++;
        }
        t->cwd[o] = 0;
    } else if (code == 8) { /* OSC 8: Hyperlinks */
        char *url = arg ? strchr(arg, ';') : NULL;
        if (url) {
            strncpy(t->current_url, url+1, MAX_OSC-1);
            t->current_url[MAX_OSC-1]=0;
            if (t->current_url[0]) t->attrs |= ATTR_URL; else t->attrs &= ~ATTR_URL;
        }
    }
}

static void term_clear_sixels(Terminal *t) {
    for (int i = 0; i < 64; i++) if (t->sixels[i].active) { glDeleteTextures(1, &t->sixels[i].tex); t->sixels[i].active = 0; }
    t->sixel_count = 0;
}

#define SIXEL_MAX_DIM 1024

static void term_handle_sixel(Terminal *t, uint8_t b) {
    SixelCtx *sc = &t->sixel_ctx;
    if (b == 0x1B || b == 0x9C) { /* ST terminates the image */
        if (sc->pix && sc->max_x > 0 && sc->max_y > 0) {
            int w = sc->max_x, h = sc->max_y;
            GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            uint32_t *rgba = calloc((size_t)w * h, 4);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                uint8_t pi = sc->pix[y * sc->w + x];
                /* index 0 = never painted = transparent; stored palette index is +1 */
                if (pi) { uint32_t p = sc->pal[pi - 1]; rgba[y * w + x] = rgba_bytes((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF, 0xFF); }
            }
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            int sid = t->sixel_count % 64;
            if (t->sixels[sid].active) glDeleteTextures(1, &t->sixels[sid].tex);
            t->sixels[sid] = (SixelImg){ (uint32_t)sid, (int)(t->cx*G_hw.font_w), (int)(t->cy*G_hw.font_h), w, h, tex, 1 };
            t->sixel_count++; free(rgba); free(sc->pix); sc->pix = NULL;
            /* Move the cursor below the image like other sixel terminals do */
            int adv = (h + G_hw.font_h - 1) / G_hw.font_h;
            for (int i = 0; i < adv; i++) { if (t->cy >= t->scroll_bottom) term_scroll_up(t, 1); else t->cy++; }
            t->cx = 0;
        } else if (sc->pix) { free(sc->pix); sc->pix = NULL; }
        t->parser_state = ST_GROUND; return;
    }
    if (!sc->pix) {
        sc->w = SIXEL_MAX_DIM; sc->h = SIXEL_MAX_DIM;
        sc->pix = calloc((size_t)sc->w * sc->h, 1);
        sc->x = sc->y = 0; sc->max_x = sc->max_y = 0;
        sc->pmode = 0; sc->repeat = 1; sc->cur_pal = 0;
    }
    if (sc->pmode) { /* accumulating parameters for '#', '"' or '!' */
        if (b >= '0' && b <= '9') { sc->params[sc->nparam] = sc->params[sc->nparam] * 10 + (b - '0'); return; }
        if (b == ';') { if (sc->nparam < 7) sc->nparam++; return; }
        int np = sc->nparam + 1;
        if (sc->pmode == '#') { /* colour select, or define: #Pc;2;R;G;B (percent) / #Pc;1;H;L;S */
            int c = sc->params[0]; if (c < 0) c = 0; if (c > 254) c = 254;
            if (np >= 5 && sc->params[1] == 2) {
                int r = sc->params[2] * 255 / 100, g = sc->params[3] * 255 / 100, bl = sc->params[4] * 255 / 100;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (bl > 255) bl = 255;
                sc->pal[c] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
            }
            sc->cur_pal = c;
        } else if (sc->pmode == '!') {
            sc->repeat = sc->params[0] > 0 ? sc->params[0] : 1;
            if (sc->repeat > SIXEL_MAX_DIM) sc->repeat = SIXEL_MAX_DIM;
        } /* '"' raster attributes: aspect/size hints — we crop to content instead */
        sc->pmode = 0; /* fall through to process b */
    }
    if (b == '#' || b == '"' || b == '!') { sc->pmode = b; memset(sc->params, 0, sizeof(sc->params)); sc->nparam = 0; return; }
    if (b >= 63 && b <= 126) {
        int bits = b - 63, rep = sc->repeat; sc->repeat = 1;
        while (rep-- > 0) {
            if (sc->x < sc->w) {
                if (bits) {
                    for (int i = 0; i < 6; i++) if (bits & (1 << i)) {
                        int py = sc->y + i;
                        if (py < sc->h) { sc->pix[py * sc->w + sc->x] = (uint8_t)(sc->cur_pal + 1); if (py + 1 > sc->max_y) sc->max_y = py + 1; }
                    }
                    if (sc->x + 1 > sc->max_x) sc->max_x = sc->x + 1;
                }
            }
            sc->x++;
        }
    } else if (b == '$') {
        sc->x = 0;
    } else if (b == '-') {
        sc->x = 0; sc->y += 6;
    }
}


/* XTGETTCAP: reply to \eP+q<hex-names>\e\\ with the caps we truthfully have.
 * Names arrive ;-separated and hex-encoded; replies are hex-encoded too. */
static const char *tcap_lookup(const char *name) {
    if (!strcmp(name, "TN") || !strcmp(name, "name")) return "xterm-256color";
    if (!strcmp(name, "Co") || !strcmp(name, "colors")) return "256";
    if (!strcmp(name, "RGB")) return "8/8/8";
    return NULL;
}

static void xtgettcap_reply(Terminal *t) {
    t->osc_buf[t->osc_len] = 0;
    char resp[512]; int rn = 0, any = 0;
    char *save, *tok = strtok_r(t->osc_buf, ";", &save);
    for (; tok; tok = strtok_r(NULL, ";", &save)) {
        char name[64]; int nl = 0;
        for (const char *h = tok; h[0] && h[1] && nl < (int)sizeof(name)-1; h += 2) {
            int hi = isdigit(h[0]) ? h[0]-'0' : (tolower(h[0])-'a'+10);
            int lo = isdigit(h[1]) ? h[1]-'0' : (tolower(h[1])-'a'+10);
            name[nl++] = (char)((hi << 4) | lo);
        }
        name[nl] = 0;
        const char *val = tcap_lookup(name);
        if (!val) continue;
        if (any) resp[rn++] = ';';
        for (const char *p = name; *p && rn < (int)sizeof(resp)-8; p++) rn += sprintf(resp+rn, "%02X", (uint8_t)*p);
        resp[rn++] = '=';
        for (const char *p = val; *p && rn < (int)sizeof(resp)-8; p++) rn += sprintf(resp+rn, "%02X", (uint8_t)*p);
        any = 1;
    }
    if (any) {
        write(G.pty_fd, "\033P1+r", 5);
        write(G.pty_fd, resp, rn);
        write(G.pty_fd, "\033\\", 2);
    } else write(G.pty_fd, "\033P0+r\033\\", 7);
}

static void term_process_byte(Terminal *t, uint8_t byte) {
    uint32_t cp; if (!utf8_decode(t, byte, &cp)) return;
    if (t->parser_state == ST_GROUND) {
        if (cp < 0x20) {
            if (cp == 0x1B) t->parser_state = ST_ESC;
            else if (cp == '\n') { t->cy++; if (t->cy > t->scroll_bottom) { t->cy = t->scroll_bottom; term_scroll_up(t, 1); } }
            else if (cp == '\r') { t->cx = 0; t->pending_wrap = 0; }
            else if (cp == 0x07) bell();
            else if (cp == '\b') t->cx = t->cx > 0 ? t->cx - 1 : 0;
            else if (cp == '\t') do { t->cx++; } while (t->cx < t->cols-1 && !t->tabs[t->cx]);
            else if (cp == 0x0E) { t->charset = 1; }
            else if (cp == 0x0F) { t->charset = 0; }
        } else term_putchar(t, cp);
    } else if (t->parser_state == ST_ESC) {
        if (cp == '[') { t->parser_state = ST_CSI; t->csi_len = 0; t->nparams = 0; memset(t->params, 0, sizeof(t->params)); t->csi_inter = 0; t->csi_priv = 0; t->param_sub = 0; }
        else if (cp == ']') { t->parser_state = ST_OSC; t->osc_len = 0; }
        else if (cp == 'P') { t->parser_state = ST_DCS; t->nparams = 0; memset(t->params, 0, sizeof(t->params)); t->csi_inter = 0; }
        else if (cp == '(') t->parser_state = ST_G0_SET;
        else if (cp == ')') t->parser_state = ST_G1_SET;
        else if (cp == '=') { t->app_keypad = 1; t->parser_state = ST_GROUND; }
        else if (cp == '>') { t->app_keypad = 0; t->parser_state = ST_GROUND; }
        else if (cp == '7') { /* DECSC: save cursor + attrs + colors */
            t->saved.x = t->cx; t->saved.y = t->cy; t->saved.pw = t->pending_wrap;
            t->saved.attrs = t->attrs; t->saved.fg = t->fg; t->saved.bg = t->bg;
            t->parser_state = ST_GROUND;
        }
        else if (cp == '8') { /* DECRC */
            t->cx = t->saved.x; t->cy = t->saved.y; t->pending_wrap = t->saved.pw;
            t->attrs = t->saved.attrs; t->fg = t->saved.fg; t->bg = t->saved.bg;
            t->parser_state = ST_GROUND;
        }
        else if (cp == 'c') { /* RIS: full reset */
            t->attrs = 0; t->fg = COLOR_FG; t->bg = COLOR_BG;
            t->scroll_top = 0; t->scroll_bottom = t->rows - 1;
            t->origin_mode = 0; t->auto_wrap = 1; t->cursor_visible = 1;
            t->alt_active = 0; t->mouse_mode = 0; t->mouse_ext = 0;
            t->focus_report = 0; t->bracketed_paste = 0;
            t->app_cursor_keys = 0; t->app_keypad = 0;
            t->charset = 0; t->g0_charset = 0; t->g1_charset = 0;
            t->kitty_n = 1; t->kitty_stack[0] = 0;
            t->cx = 0; t->cy = 0; t->pending_wrap = 0; t->sb_offset = 0;
            for (int r = 0; r < t->rows; r++) {
                term_clear_line(t, &t->screen[r], 0, t->cols - 1);
                term_clear_line(t, &t->alt_screen[r], 0, t->cols - 1);
            }
            for (int i = 0; i < t->cols; i++) t->tabs[i] = (i % 8 == 0);
            term_clear_sixels(t);
            t->parser_state = ST_GROUND;
        }
        else if (cp == 'M') { /* RI: Reverse Index */
            if (t->cy == t->scroll_top) term_scroll_down(t, 1);
            else if (t->cy > 0) t->cy--;
            t->parser_state = ST_GROUND;
        }
        else if (cp == 'D') { /* IND: Index */
            if (t->cy == t->scroll_bottom) term_scroll_up(t, 1);
            else if (t->cy < t->rows-1) t->cy++;
            t->parser_state = ST_GROUND;
        }
        else if (cp == 'E') { t->cx = 0; if (t->cy == t->scroll_bottom) term_scroll_up(t, 1); else if (t->cy < t->rows-1) t->cy++; t->parser_state = ST_GROUND; }
        else t->parser_state = ST_GROUND;
    } else if (t->parser_state == ST_CSI) {
        if (cp >= 0x30 && cp <= 0x39) { /* digit 0-9 */
            if (t->nparams == 0) t->nparams = 1;
            t->params[t->nparams-1] = t->params[t->nparams-1]*10 + (cp - '0');
        } else if (cp == ';') { if (t->nparams < MAX_PARAMS) t->nparams++; }
        else if (cp == ':') { /* SGR subparameter separator */
            if (t->nparams == 0) t->nparams = 1;
            if (t->nparams < MAX_PARAMS) { t->nparams++; t->param_sub |= 1u << (t->nparams-1); }
        }
        else if (cp >= 0x3C && cp <= 0x3F) t->csi_priv = cp; /* private marker: <, =, >, ? */
        else if (cp >= 0x20 && cp <= 0x2F) t->csi_inter = cp; /* intermediate byte */
        else { t->csi_final = cp; term_csi(t); t->parser_state = ST_GROUND; }
    } else if (t->parser_state == ST_OSC) {
        if (cp == 0x07) { term_handle_osc(t); t->parser_state = ST_GROUND; }
        else if (cp == 0x1B) { term_handle_osc(t); t->parser_state = ST_ESC; } /* ESC \ = ST */
        else if (t->osc_len < MAX_OSC-1) t->osc_buf[t->osc_len++] = cp;
    } else if (t->parser_state == ST_DCS) {
        if (t->dcs_type == 2) { /* XTGETTCAP: buffer hex-encoded cap names until ST */
            if (cp == 0x1B) { xtgettcap_reply(t); t->dcs_type = 0; t->parser_state = ST_ESC; }
            else if (t->osc_len < MAX_OSC-1) t->osc_buf[t->osc_len++] = (char)cp;
        } else if (cp >= '0' && cp <= '9') {
            if (t->nparams == 0) t->nparams = 1;
            t->params[t->nparams-1] = t->params[t->nparams-1]*10 + (cp - '0');
        } else if (cp == ';') {
            if (t->nparams < MAX_PARAMS) t->nparams++;
        } else if (cp == 'q') {
            if (t->csi_inter == '+') { t->dcs_type = 2; t->osc_len = 0; } /* XTGETTCAP: stay in DCS to consume */
            else if (t->csi_inter == 0) { t->dcs_type = 1; t->parser_state = ST_SIXEL; }
        } else if (cp == 0x1B) {
            t->dcs_type = 0; t->parser_state = ST_ESC;
        } else {
            t->csi_inter = (char)cp; /* intermediate/intro byte (e.g. '+' in \eP+q) */
        }
    } else if (t->parser_state == ST_G0_SET) {
        if (cp == '0') t->g0_charset = 1; /* DEC Special Graphics */
        else if (cp == 'B' || cp == 'A') t->g0_charset = 0; /* ASCII / UK */
        t->parser_state = ST_GROUND;
    } else if (t->parser_state == ST_G1_SET) {
        if (cp == '0') t->g1_charset = 1; /* DEC Special Graphics */
        else if (cp == 'B' || cp == 'A') t->g1_charset = 0; /* ASCII / UK */
        t->parser_state = ST_GROUND;
    } else if (t->parser_state == ST_SIXEL) {
        term_handle_sixel(t, byte);
    } else t->parser_state = ST_GROUND;
}

static void scan_urls(Terminal *t, int row) {
    Line *l = (t->alt_active ? t->alt_screen : t->screen) + row;
    for (int i=0; i<l->cols; i++) l->cells[i].attrs &= ~ATTR_URL;
    for (int i=0; i<l->cols-7; i++) {
        if (l->cells[i].codepoint == 'h' && l->cells[i+1].codepoint == 't' && 
            l->cells[i+2].codepoint == 't' && l->cells[i+3].codepoint == 'p') {
            int start = i; int len = 4;
            if (l->cells[i+4].codepoint == 's') len++;
            if (l->cells[i+len].codepoint == ':' && l->cells[i+len+1].codepoint == '/' && l->cells[i+len+2].codepoint == '/') {
                i += len + 3;
                while (i < l->cols && !isspace(l->cells[i].codepoint) && l->cells[i].codepoint != '"' && l->cells[i].codepoint != '\'') {
                    l->cells[i].attrs |= ATTR_URL; i++;
                }
                for (int k=start; k<i; k++) l->cells[k].attrs |= ATTR_URL;
            }
        }
    }
}

static void term_process(Terminal *t, const uint8_t *buf, int len) {
    if (len > 0) t->dirty = 1;
    int old_cy = t->cy;
    for (int i=0; i<len; i++) {
        if (t->parser_state == ST_GROUND && buf[i] >= 0x20 && buf[i] < 0x7F) term_putchar(t, buf[i]);
        else term_process_byte(t, buf[i]);
    }
    /* Rescan affected lines for URLs */
    int r0 = old_cy < t->cy ? old_cy : t->cy, r1 = old_cy < t->cy ? t->cy : old_cy;
    for (int r=r0; r<=r1 && r<t->rows; r++) if (r >= 0) scan_urls(t, r);
}

/* Line shown at screen row r, accounting for scrollback offset (Shift+PgUp).
 * Scrollback lines can be narrower than the current grid after a resize —
 * callers must guard column accesses with `c < line->cols`. */
/* Is cell (r,c) of the visible grid inside the active mouse selection? */
static int cell_selected(Terminal *t, int r, int c) {
    if (!t->sel_active) return 0;
    int y1 = fmin(t->sel_y1, t->sel_y2), y2 = fmax(t->sel_y1, t->sel_y2);
    int x1 = (t->sel_y1 < t->sel_y2) ? t->sel_x1 : ((t->sel_y1 > t->sel_y2) ? t->sel_x2 : fmin(t->sel_x1, t->sel_x2));
    int x2 = (t->sel_y1 < t->sel_y2) ? t->sel_x2 : ((t->sel_y1 > t->sel_y2) ? t->sel_x1 : fmax(t->sel_x1, t->sel_x2));
    if (r > y1 && r < y2) return 1;
    if (r == y1 && r == y2) return c >= x1 && c <= x2;
    if (r == y1) return c >= x1;
    if (r == y2) return c <= x2;
    return 0;
}

static Line *visible_line(Terminal *t, int r) {
    if (t->alt_active || t->sb_offset <= 0) return (t->alt_active ? t->alt_screen : t->screen) + r;
    int sb_r = t->sb.count - t->sb_offset + r;
    if (sb_r < 0) sb_r = 0;
    if (sb_r < t->sb.count) return &t->sb.lines[(t->sb.head + sb_r) % t->sb.cap];
    return &t->screen[sb_r - t->sb.count];
}

/* Line at absolute position i: 0 = oldest scrollback line, then the live screen */
static Line *term_abs_line(Terminal *t, int i) {
    if (i < t->sb.count) return &t->sb.lines[(t->sb.head + i) % t->sb.cap];
    return &t->screen[i - t->sb.count];
}

/* --- Scrollback search --- */

/* Mark columns of ln matching the query in flags (if non-NULL, maxc entries).
 * Case-insensitive; returns the number of matches on the line. */
static int search_row_flags(Line *ln, uint8_t *flags, int maxc) {
    int q = G.search.qlen, n = 0;
    if (!q) return 0;
    for (int c = 0; c + q <= ln->cols; c++) {
        int k = 0;
        while (k < q && (uint32_t)towlower((wint_t)ln->cells[c+k].codepoint) == G.search.q[k]) k++;
        if (k == q) {
            n++;
            if (flags) for (int j = 0; j < q && c+j < maxc; j++) flags[c+j] = 1;
            c += q - 1;
        }
    }
    return n;
}

static void search_scroll_to(Terminal *t, int abs) {
    int off = t->sb.count - abs + t->rows/2 - 1;
    if (off > t->sb.count) off = t->sb.count;
    if (off < 0) off = 0;
    t->sb_offset = off;
}

/* Recount matches and jump: dir -1 = older (up), +1 = newer (down),
 * 0 = nearest match at or above the bottom of the current view. */
static void search_move(Terminal *t, int dir) {
    int N = t->sb.count + t->rows;
    G.search.count = 0;
    if (!G.search.qlen) { G.search.abs = -1; return; }
    for (int i = 0; i < N; i++) if (search_row_flags(term_abs_line(t, i), NULL, 0)) G.search.count++;
    if (!G.search.count) { G.search.abs = -1; return; }
    int bottom = t->sb.count - t->sb_offset + t->rows - 1;
    int start = dir == 0 ? bottom : (G.search.abs >= 0 ? G.search.abs : bottom) + dir;
    if (start >= N) start = N - 1;
    if (start < 0) start = 0;
    int step = dir > 0 ? 1 : -1;
    for (int i = start; i >= 0 && i < N; i += step)
        if (search_row_flags(term_abs_line(t, i), NULL, 0)) { G.search.abs = i; search_scroll_to(t, i); return; }
    /* no match in that direction: stay put */
}

/* --- URL opening + keyboard hints --- */
static void open_url_run(Line *ln, int start, int end) {
    char url[MAX_OSC]; char *p = url;
    for (int i = start; i <= end && i < ln->cols && p < url + MAX_OSC - 5; i++) {
        uint32_t cp = ln->cells[i].codepoint;
        if (cp == CP_WIDE_CONTINUATION) continue;
        utf8_encode(&p, cp);
    }
    *p = 0;
    if (fork() == 0) { execlp("xdg-open", "xdg-open", url, NULL); _exit(0); }
}

static void hints_build(Terminal *t) {
    static const char labels[] = "asdfghjklqwertyuiopzxcvbnm0123456789";
    G.hints.n = 0;
    for (int r = 0; r < t->rows && G.hints.n < 36; r++) {
        Line *ln = visible_line(t, r);
        int c = 0;
        while (c < ln->cols && G.hints.n < 36) {
            if (ln->cells[c].attrs & ATTR_URL) {
                int s = c;
                while (c < ln->cols && (ln->cells[c].attrs & ATTR_URL)) c++;
                G.hints.item[G.hints.n].row = r;
                G.hints.item[G.hints.n].c1 = s;
                G.hints.item[G.hints.n].c2 = c - 1;
                G.hints.item[G.hints.n].label = labels[G.hints.n];
                G.hints.n++;
            } else c++;
        }
    }
    G.hints.active = G.hints.n > 0;
}

/* --- Optimized Rendering Engine --- */
static void render_frame(void) {
    Terminal *t = G.term;
    /* Resize is handled by ConfigureNotify in the event loop — no X11 round-trip here */
    glViewport(0, 0, G.win_w, G.win_h);

    G_hw.current_vbo = (G_hw.current_vbo + 1) % VBO_COUNT;
    Vertex *vbo_ptr = G_hw.vbo_cpu[G_hw.current_vbo];
    int v_idx = 0; float g_proj[16]; memset(g_proj, 0, sizeof(g_proj));
    g_proj[0] = 2.0f/G.win_w; g_proj[5] = -2.0f/G.win_h; g_proj[10] = -1.0f;
    /* Translate the cell grid in from the window edge by the configured padding */
    g_proj[12] = -1.0f + 2.0f*CFG.padding/G.win_w; g_proj[13] = 1.0f - 2.0f*CFG.padding/G.win_h; g_proj[15] = 1.0f;

    float cr = ((COLOR_BG >> 16) & 0xFF) / 255.0f, cg = ((COLOR_BG >> 8) & 0xFF) / 255.0f, cb = (COLOR_BG & 0xFF) / 255.0f;
    glClearColor(cr, cg, cb, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(G.prog); glUniformMatrix4fv(G.u_proj, 1, GL_FALSE, g_proj);
    glUniform1f(G.u_win_h, (float)G.win_h);
    glUniform1f(G.u_win_w, (float)G.win_w);
    glBindTexture(GL_TEXTURE_2D, G_hw.atlas_tex);

    static uint8_t sflags[1024];
    int have_search = G.search.active && G.search.qlen;

    for (int r=0; r<t->rows; r++) {
        Line *ln = visible_line(t, r);
        float py = (float)(r * G_hw.font_h); int start_c = -1; uint32_t last_bg = COLOR_BG;
        if (have_search) {
            memset(sflags, 0, t->cols < 1024 ? t->cols : 1024);
            search_row_flags(ln, sflags, t->cols < 1024 ? t->cols : 1024);
        }
        for (int c=0; c<t->cols; c++) {
            Cell *bcell = (c < ln->cols) ? &ln->cells[c] : NULL;
            uint32_t current_bg = bcell ? bcell->bg : COLOR_BG;
            int in_match = have_search && c < 1024 && sflags[c];
            /* Selection/search invert the cell's displayed colours; combined with
             * ATTR_REVERSE the two inversions cancel so the cell stays visible. */
            if (bcell && (((bcell->attrs & ATTR_REVERSE) != 0) ^ (cell_selected(t, r, c) || in_match))) current_bg = bcell->fg;
            if (current_bg != last_bg) {
                if (start_c != -1) draw_rect_hw(vbo_ptr, (float)(start_c*G_hw.font_w), py, (float)((c-start_c)*G_hw.font_w), (float)G_hw.font_h, pack_color(last_bg, 255), &v_idx);
                start_c = (current_bg != COLOR_BG) ? c : -1; last_bg = current_bg;
            }
        }
        if (start_c != -1) draw_rect_hw(vbo_ptr, (float)(start_c*G_hw.font_w), py, (float)((t->cols-start_c)*G_hw.font_w), (float)G_hw.font_h, pack_color(last_bg, 255), &v_idx);
    }

    int any_blink = 0;
    for (int r=0; r<t->rows; r++) {
        Line *ln = visible_line(t, r);
        float py = (float)(r * G_hw.font_h);
        int ncols = t->cols < ln->cols ? t->cols : ln->cols;
        if (have_search) {
            memset(sflags, 0, ncols < 1024 ? ncols : 1024);
            search_row_flags(ln, sflags, ncols < 1024 ? ncols : 1024);
        }
        for (int c=0; c<ncols; c++) {
            Cell *cell = &ln->cells[c];
            if (cell->attrs & ATTR_BLINK) any_blink = 1;
            if (cell->codepoint <= 32 || cell->codepoint == CP_WIDE_CONTINUATION) continue;
            if (cell->attrs & ATTR_INVIS) continue;
            if ((cell->attrs & ATTR_BLINK) && !G.cursor_phase) continue;
            /* ATTR_REVERSE, selection and search matches swap fg/bg: text drawn in bg
             * color over fg-colored background. Must mirror the background pass above,
             * otherwise text is painted fg-on-fg and disappears (was the case for
             * selections until 2026-09-22). */
            int inverted = ((cell->attrs & ATTR_REVERSE) != 0) ^ (cell_selected(t, r, c) || (have_search && c < 1024 && sflags[c]));
            uint32_t draw_fg = inverted ? cell->bg : cell->fg;
            /* Bold → bright: map palette[0-7] to palette[8-15] for basic ANSI colors */
            if ((cell->attrs & ATTR_BOLD) && !inverted) {
                for (int _pi = 0; _pi < 8; _pi++) {
                    if (draw_fg == PALETTE[_pi]) { draw_fg = PALETTE[_pi + 8]; break; }
                }
            }
            /* ATTR_DIM: reduce alpha so text blends with background */
            uint8_t glyph_alpha = (cell->attrs & ATTR_DIM) ? 160 : 255;
            uint32_t fgc = pack_color(draw_fg, glyph_alpha);
            if (!draw_special_hw(vbo_ptr, cell->codepoint, (float)(c*G_hw.font_w), py, draw_fg, glyph_alpha, &v_idx)) {
                int style = ((cell->attrs & ATTR_BOLD) ? 1 : 0) | ((cell->attrs & ATTR_ITALIC) ? 2 : 0);
                int gid = get_glyph(cell->codepoint, style);
                /* Synthesise only what a real styled face didn't provide */
                int have_face = G_hw.style_faces[style] != NULL;
                int synth_bold = (cell->attrs & ATTR_BOLD) && !have_face;
                int synth_ital = (cell->attrs & ATTR_ITALIC) && !have_face;
                if (synth_bold) draw_glyph_hw(vbo_ptr, gid, (float)(c*G_hw.font_w+1), py, fgc, synth_ital, &v_idx);
                draw_glyph_hw(vbo_ptr, gid, (float)(c*G_hw.font_w), py, fgc, synth_ital, &v_idx);
            }
            if (cell->attrs & (ATTR_UNDERLINE | ATTR_UNDERCURL | ATTR_UL_DOUBLE | ATTR_URL)) {
                int w = (c < ncols - 1 && ln->cells[c+1].codepoint == CP_WIDE_CONTINUATION) ? 2 : 1;
                uint32_t ulc = (cell->ul != UL_DEFAULT) ? pack_color(cell->ul, glyph_alpha) : fgc;
                float ux = (float)(c*G_hw.font_w), uw = (float)(w*G_hw.font_w);
                if (cell->attrs & ATTR_UNDERCURL) {
                    /* 3 short segments per cell alternating between two rows: a cheap wave */
                    float seg = uw / 3.0f;
                    for (int k = 0; k < 3; k++) {
                        float yy = ((c*3 + k) & 1) ? py + G_hw.font_h - 1 : py + G_hw.font_h - 3;
                        draw_rect_hw(vbo_ptr, ux + k*seg, yy, seg, 1, ulc, &v_idx);
                    }
                } else if (cell->attrs & ATTR_UL_DOUBLE) {
                    draw_rect_hw(vbo_ptr, ux, py + G_hw.font_h - 1, uw, 1, ulc, &v_idx);
                    draw_rect_hw(vbo_ptr, ux, py + G_hw.font_h - 3, uw, 1, ulc, &v_idx);
                } else {
                    draw_rect_hw(vbo_ptr, ux, py + G_hw.font_h - 2, uw, 1, ulc, &v_idx);
                }
            }
            if (cell->attrs & ATTR_STRIKE) {
                draw_rect_hw(vbo_ptr, (float)(c*G_hw.font_w), py + G_hw.font_h/2 - 1, (float)G_hw.font_w, 1, fgc, &v_idx);
            }
        }
    }
    t->has_blink = any_blink;
    if (t->cursor_visible && t->sb_offset == 0) {
        float cx = (float)t->cx * G_hw.font_w, cy = (float)t->cy * G_hw.font_h;
        float fw = (float)G_hw.font_w, fh = (float)G_hw.font_h;
        uint32_t cc = pack_color(COLOR_CURSOR, 200);
        int style = t->cursor_style ? t->cursor_style : 1;
        int steady = (style % 2 == 0);
        if (!G.focused) { /* hollow outline when unfocused */
            draw_rect_hw(vbo_ptr, cx, cy, fw, 1, cc, &v_idx);
            draw_rect_hw(vbo_ptr, cx, cy+fh-1, fw, 1, cc, &v_idx);
            draw_rect_hw(vbo_ptr, cx, cy, 1, fh, cc, &v_idx);
            draw_rect_hw(vbo_ptr, cx+fw-1, cy, 1, fh, cc, &v_idx);
        } else if (steady || G.cursor_phase) {
            if (style <= 2) draw_rect_hw(vbo_ptr, cx, cy, fw, fh, pack_color(COLOR_CURSOR, 160), &v_idx);
            else if (style <= 4) draw_rect_hw(vbo_ptr, cx, cy+fh-2, fw, 2, cc, &v_idx);
            else draw_rect_hw(vbo_ptr, cx, cy, 2, fh, cc, &v_idx);
        }
    }
    if (G.bell_until > now_ms()) { /* visual bell: bright frame around the content */
        float W = (float)G.win_w, H = (float)G.win_h, p = (float)CFG.padding;
        uint32_t bc = pack_color(COLOR_CURSOR, 255);
        draw_rect_hw(vbo_ptr, -p, -p, W, 3, bc, &v_idx);
        draw_rect_hw(vbo_ptr, -p, H-3-p, W, 3, bc, &v_idx);
        draw_rect_hw(vbo_ptr, -p, -p, 3, H, bc, &v_idx);
        draw_rect_hw(vbo_ptr, W-3-p, -p, 3, H, bc, &v_idx);
    }
    if (G.search.active) { /* search bar over the bottom grid row */
        float fw = (float)G_hw.font_w, fh = (float)G_hw.font_h;
        float by = (float)((t->rows - 1) * G_hw.font_h);
        uint32_t tc = pack_color(COLOR_BG, 255);
        draw_rect_hw(vbo_ptr, -CFG.padding, by, (float)G.win_w, fh + CFG.padding, pack_color(COLOR_FG, 255), &v_idx);
        float x = 0;
        draw_glyph_hw(vbo_ptr, get_glyph('/', 0), x, by, tc, 0, &v_idx); x += fw;
        for (int i = 0; i < G.search.qlen; i++, x += fw)
            draw_glyph_hw(vbo_ptr, get_glyph(G.search.q[i], 0), x, by, tc, 0, &v_idx);
        char info[48];
        if (G.search.qlen == 0) snprintf(info, sizeof(info), "  (type to search)");
        else snprintf(info, sizeof(info), "  %d line%s", G.search.count, G.search.count == 1 ? "" : "s");
        for (const char *s = info; *s; s++, x += fw)
            if (*s != ' ') draw_glyph_hw(vbo_ptr, get_glyph((uint8_t)*s, 0), x, by, tc, 0, &v_idx);
    }
    if (G.hints.active) { /* URL hint labels */
        float fw = (float)G_hw.font_w, fh = (float)G_hw.font_h;
        for (int i = 0; i < G.hints.n; i++) {
            float hx = (float)(G.hints.item[i].c1 * G_hw.font_w), hy = (float)(G.hints.item[i].row * G_hw.font_h);
            draw_rect_hw(vbo_ptr, hx, hy, fw, fh, pack_color(COLOR_CURSOR, 255), &v_idx);
            draw_glyph_hw(vbo_ptr, get_glyph((uint8_t)G.hints.item[i].label, 0), hx, hy, pack_color(COLOR_BG, 255), 0, &v_idx);
        }
    }

    glBindTexture(GL_TEXTURE_2D, G_hw.atlas_tex);
    glBindBuffer(GL_ARRAY_BUFFER, G_hw.vbo[G_hw.current_vbo]);
    glBufferSubData(GL_ARRAY_BUFFER, 0, v_idx * sizeof(Vertex), vbo_ptr);
    glEnableVertexAttribArray(G.a_pos); glEnableVertexAttribArray(G.a_uv); glEnableVertexAttribArray(G.a_color);
    glVertexAttribPointer(G.a_pos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glVertexAttribPointer(G.a_uv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)8);
    glVertexAttribPointer(G.a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)16);
    glDrawArrays(GL_QUADS, 0, v_idx);

    int nsix = t->sixel_count < 64 ? t->sixel_count : 64;
    for (int i=0; i<nsix && t->sb_offset == 0; i++) {
        SixelImg *s = &t->sixels[i];
        if (s->active) {
            glBindTexture(GL_TEXTURE_2D, s->tex); int sv_idx = 0;
            draw_rect_hw(vbo_ptr + v_idx, (float)s->x, (float)s->y, (float)s->w, (float)s->h, pack_color(0xFFFFFF, 255), &sv_idx);
            glBufferSubData(GL_ARRAY_BUFFER, v_idx * sizeof(Vertex), sv_idx * sizeof(Vertex), vbo_ptr + v_idx);
            glDrawArrays(GL_QUADS, v_idx, sv_idx);
        }
    }
    glXSwapBuffers(G.dpy, G.win);
}

/* Recreate all GL objects (shaders, VBOs, atlas texture) from CPU state after
 * a context loss caused by screen blank / DPMS / resume.  Does NOT touch
 * FreeType or the glyph metadata — those are still valid. */
static void gl_recreate_resources(void) {
    /* Shaders */
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &VERT_SRC, NULL); glCompileShader(v);
    glShaderSource(f, 1, &FRAG_SRC, NULL); glCompileShader(f);
    G.prog = glCreateProgram(); glAttachShader(G.prog, v); glAttachShader(G.prog, f); glLinkProgram(G.prog);
    glDeleteShader(v); glDeleteShader(f);
    G.u_proj  = glGetUniformLocation(G.prog, "u_proj");
    G.u_tex   = glGetUniformLocation(G.prog, "u_tex");
    G.u_win_h = glGetUniformLocation(G.prog, "u_win_h");
    G.u_win_w = glGetUniformLocation(G.prog, "u_win_w");
    G.a_pos   = glGetAttribLocation(G.prog, "a_pos");
    G.a_uv    = glGetAttribLocation(G.prog, "a_uv");
    G.a_color = glGetAttribLocation(G.prog, "a_color");

    /* Atlas texture — single upload from CPU shadow */
    glGenTextures(1, &G_hw.atlas_tex); glBindTexture(GL_TEXTURE_2D, G_hw.atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_INTENSITY8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, G_hw.atlas_cpu);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* VBOs */
    glGenBuffers(VBO_COUNT, G_hw.vbo);
    for (int i = 0; i < VBO_COUNT; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, G_hw.vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, G_hw.max_vertices * sizeof(Vertex), NULL, GL_STREAM_DRAW);
    }
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, G.win_w, G.win_h);
    float r = ((COLOR_BG >> 16) & 0xFF) / 255.0f, g = ((COLOR_BG >> 8) & 0xFF) / 255.0f, b = (COLOR_BG & 0xFF) / 255.0f;
    glClearColor(r, g, b, 1.0f);
    G.reinit_needed = 0;
}

static void font_set_metrics(int sz) {
    G_hw.font_sz = sz;
    for (int s = 0; s < 4; s++) if (G_hw.style_faces[s]) FT_Set_Pixel_Sizes(G_hw.style_faces[s], 0, sz);
    for (int i = 0; i < G_hw.fb_count; i++) FT_Set_Pixel_Sizes(G_hw.fb_faces[i], 0, sz);
    G_hw.font_h = G_hw.ft_face->size->metrics.height >> 6;
    G_hw.font_as = G_hw.ft_face->size->metrics.ascender >> 6;
    G_hw.font_w = G_hw.ft_face->size->metrics.max_advance >> 6;
    if (G_hw.font_w > G_hw.font_h * 0.7f || G_hw.font_w <= 0) G_hw.font_w = (G_hw.font_h * 6) / 10;
    if (G_hw.font_h <= 0) G_hw.font_h = 14;
    if (G_hw.font_as <= 0) G_hw.font_as = (G_hw.font_h * 8) / 10;
}

/* Tell the WM our cell geometry so interactive resizes snap to it.
 * Called at startup and again whenever the cell size or padding changes. */
static void wm_set_size_hints(void) {
    XSizeHints *sh = XAllocSizeHints();
    if (!sh) return;
    sh->flags = PResizeInc | PBaseSize | PMinSize;
    sh->width_inc = G_hw.font_w; sh->height_inc = G_hw.font_h;
    sh->base_width = sh->base_height = 2*CFG.padding;
    sh->min_width = 2*CFG.padding + 20*G_hw.font_w; sh->min_height = 2*CFG.padding + 4*G_hw.font_h;
    XSetWMNormalHints(G.dpy, G.win, sh);
    XFree(sh);
}

/* Runtime font size change (Ctrl+Plus/Minus/0, Ctrl+wheel): rebuild atlas + grid */
static void font_resize(int newsz) {
    if (newsz < 6) newsz = 6;
    if (newsz > 48) newsz = 48;
    if (newsz == G_hw.font_sz) return;
    font_set_metrics(newsz);
    memset(G_hw.atlas_cpu, 0, ATLAS_SIZE * ATLAS_SIZE);
    for (int y = 0; y < 16; y++) memset(G_hw.atlas_cpu + y * ATLAS_SIZE, 255, 16);
    glyph_cache_reset();
    glBindTexture(GL_TEXTURE_2D, G_hw.atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_INTENSITY8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, G_hw.atlas_cpu);
    int cols = (G.win_w - 2*CFG.padding) / G_hw.font_w;
    int rows = (G.win_h - 2*CFG.padding) / G_hw.font_h;
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    term_resize(G.term, cols, rows);
    wm_set_size_hints();
}

/* After the default fg/bg change (config reload or OSC 10/11), cells and
 * the parser's current colours that still wear the old defaults follow
 * the new ones — otherwise a later erase paints the old colour explicitly. */
static void retint_defaults(uint32_t ofg, uint32_t obg) {
    Terminal *t = G.term;
    if (!t || (ofg == CFG.fg && obg == CFG.bg)) return;
    for (int pass = 0; pass < 3; pass++) {
        int n = pass == 2 ? t->sb.count : t->rows;
        for (int r = 0; r < n; r++) {
            Line *l = pass == 0 ? &t->screen[r] : pass == 1 ? &t->alt_screen[r] : term_abs_line(t, r);
            for (int c = 0; c < l->cols; c++) {
                if (l->cells[c].fg == ofg) l->cells[c].fg = CFG.fg;
                if (l->cells[c].bg == obg) l->cells[c].bg = CFG.bg;
            }
        }
    }
    if (t->fg == ofg) t->fg = CFG.fg;
    if (t->bg == obg) t->bg = CFG.bg;
    if (t->saved.fg == ofg) t->saved.fg = CFG.fg;
    if (t->saved.bg == obg) t->saved.bg = CFG.bg;
    if (t->saved_alt.fg == ofg) t->saved_alt.fg = CFG.fg;
    if (t->saved_alt.bg == obg) t->saved_alt.bg = CFG.bg;
    t->full_dirty = 1;
}

/* Live config reload (SIGUSR1 or the config file changing on disk).
 * Applies colours, blink, padding and font size; font family and
 * scrollback depth still require a restart. */
static void config_reapply(void) {
    char oldfont[128]; strcpy(oldfont, CFG.font);
    int oldsb = CFG.scrollback;
    uint32_t ofg = CFG.fg, obg = CFG.bg;
    config_load();
    palette_init();
    retint_defaults(ofg, obg);
    if (strcmp(oldfont, CFG.font))
        fprintf(stderr, "peanutbutty: font family change requires restart\n");
    strcpy(CFG.font, oldfont); /* loaded faces stay authoritative */
    if (CFG.scrollback != oldsb) {
        fprintf(stderr, "peanutbutty: scrollback change requires restart\n");
        CFG.scrollback = oldsb;
    }
    if (CFG.size != G_hw.font_sz) font_resize(CFG.size);
    else { /* padding may have changed the grid */
        int cols = (G.win_w - 2*CFG.padding) / G_hw.font_w;
        int rows = (G.win_h - 2*CFG.padding) / G_hw.font_h;
        if (cols < 2) cols = 2;
        if (rows < 1) rows = 1;
        term_resize(G.term, cols, rows);
        wm_set_size_hints();
    }
    G.term->full_dirty = 1;
}

static void init_hw_resources(Display *dpy, GLXDrawable drawable) {
    void (*sMESA)(unsigned int) = (void(*)(unsigned int))glXGetProcAddress((const GLubyte*)"glXSwapIntervalMESA");
    void (*sEXT)(Display*, GLXDrawable, int) = (void(*)(Display*, GLXDrawable, int))glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");
    if (sMESA) sMESA(1); else if (sEXT) sEXT(dpy, drawable, 1);
    
    FT_Init_FreeType(&G_hw.ft_lib);

    FcConfig *fcc = FcInitLoadConfigAndFonts();
    G_hw.fc = fcc; /* kept alive for per-glyph fallback lookups */

    /* Load the four style faces of the configured family (bold/italic may
     * resolve to the same file as regular, in which case we synthesise). */
    char reg_file[512] = "";
    static const struct { int weight, slant; } styles[4] = {
        { FC_WEIGHT_REGULAR, FC_SLANT_ROMAN }, { FC_WEIGHT_BOLD, FC_SLANT_ROMAN },
        { FC_WEIGHT_REGULAR, FC_SLANT_ITALIC }, { FC_WEIGHT_BOLD, FC_SLANT_ITALIC },
    };
    for (int s = 0; s < 4; s++) {
        FcPattern *pat = FcNameParse((const FcChar8*)CFG.font);
        FcPatternAddInteger(pat, FC_WEIGHT, styles[s].weight);
        FcPatternAddInteger(pat, FC_SLANT, styles[s].slant);
        FcConfigSubstitute(fcc, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult fres;
        FcPattern *match = FcFontMatch(fcc, pat, &fres);
        if (match) {
            FcChar8 *file = NULL;
            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
                if (s == 0) {
                    strncpy(reg_file, (const char*)file, sizeof(reg_file)-1);
                    if (FT_New_Face(G_hw.ft_lib, (const char*)file, 0, &G_hw.ft_face)) G_hw.ft_face = NULL;
                } else if (strcmp((const char*)file, reg_file)) { /* real styled variant exists */
                    FT_Face f = NULL;
                    if (!FT_New_Face(G_hw.ft_lib, (const char*)file, 0, &f)) G_hw.style_faces[s] = f;
                }
            }
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pat);
    }

    if (!G_hw.ft_face) {
        const char *fnts[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/liberation/LiberationMono-Regular.ttf"};
        for (int i=0; i<3; i++) if (!FT_New_Face(G_hw.ft_lib, fnts[i], 0, &G_hw.ft_face)) break;
    }
    if (!G_hw.ft_face) die("Monospace font not found");
    G_hw.style_faces[0] = G_hw.ft_face;

    font_set_metrics(CFG.size);

    if (!G_hw.atlas_cpu) G_hw.atlas_cpu = calloc(ATLAS_SIZE * ATLAS_SIZE, 1);
    for (int y = 0; y < 16; y++) memset(G_hw.atlas_cpu + y * ATLAS_SIZE, 255, 16);
    glGenTextures(1, &G_hw.atlas_tex); glBindTexture(GL_TEXTURE_2D, G_hw.atlas_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_INTENSITY8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, G_hw.atlas_cpu);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    G_hw.atlas_x = 18; G_hw.atlas_y = 0; G_hw.atlas_h = 0;
    G_hw.max_vertices = 512*1024; glGenBuffers(VBO_COUNT, G_hw.vbo);
    for (int i=0; i<VBO_COUNT; i++) {
        G_hw.vbo_cpu[i] = malloc(G_hw.max_vertices * sizeof(Vertex));
        glBindBuffer(GL_ARRAY_BUFFER, G_hw.vbo[i]); glBufferData(GL_ARRAY_BUFFER, G_hw.max_vertices * sizeof(Vertex), NULL, GL_STREAM_DRAW);
    }
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    float r = ((COLOR_BG >> 16) & 0xFF) / 255.0f, g = ((COLOR_BG >> 8) & 0xFF) / 255.0f, b = (COLOR_BG & 0xFF) / 255.0f;
    glClearColor(r, g, b, 1.0f);
}

/* Reflow the main screen + scrollback to a new width, rejoining lines that
 * were soft-wrapped (Line.wrapped) and re-wrapping them at the new width.
 * The alt screen is clamp-copied: fullscreen apps redraw on SIGWINCH anyway. */
static void term_resize(Terminal *t, int cols, int rows) {
    if (cols < 1 || rows < 1) return;
    if (cols == t->cols && rows == t->rows) return;

    /* Cursor being tracked: the live one, or the saved main-screen cursor
     * while the alt screen is active. */
    int mcx = t->alt_active ? t->saved_alt.x : t->cx;
    int mcy = t->alt_active ? t->saved_alt.y : t->cy;

    /* --- Gather source physical lines, oldest first --- */
    int nsrc = t->sb.count + t->rows;
    Line **src = malloc(nsrc * sizeof(Line*));
    int k = 0;
    for (int i = 0; i < t->sb.count; i++) src[k++] = &t->sb.lines[(t->sb.head + i) % t->sb.cap];
    for (int r = 0; r < t->rows; r++) src[k++] = &t->screen[r];
    int cur_src = t->sb.count + mcy;
    /* Drop trailing all-blank screen rows below the cursor: they'd inflate the
     * logical line count and push real content off the top. The new screen is
     * blank-padded at the bottom anyway. */
    while (nsrc - 1 > cur_src) {
        Line *tl = src[nsrc-1];
        int blank = !tl->wrapped;
        for (int c = 0; blank && c < tl->cols; c++)
            if (tl->cells[c].codepoint != ' ' || tl->cells[c].bg != COLOR_BG) blank = 0;
        if (!blank) break;
        nsrc--;
    }

    /* --- Re-emit into new-width physical lines --- */
    int out_cap = nsrc + 16, out_n = 0;
    Line *out = calloc(out_cap, sizeof(Line));
    int out_x = 0, need_line = 1;
    int cur_row = -1, cur_col = 0;
    #define REFLOW_NEWLINE() do { \
        if (out_n == out_cap) { out_cap *= 2; out = realloc(out, out_cap * sizeof(Line)); } \
        memset(&out[out_n], 0, sizeof(Line)); \
        out[out_n].cells = calloc(cols, sizeof(Cell)); out[out_n].cols = cols; out[out_n].dirty = 1; \
        for (int _j = 0; _j < cols; _j++) { out[out_n].cells[_j].codepoint = ' '; out[out_n].cells[_j].fg = COLOR_FG; out[out_n].cells[_j].bg = COLOR_BG; out[out_n].cells[_j].ul = UL_DEFAULT; } \
        out_n++; out_x = 0; \
    } while (0)

    for (int s = 0; s < nsrc; s++) {
        Line *ls = src[s];
        int len = ls->cols;
        if (!ls->wrapped) { /* trim trailing default blanks off hard-ended lines */
            while (len > 0 && ls->cells[len-1].codepoint == ' ' && ls->cells[len-1].bg == COLOR_BG && !(ls->cells[len-1].attrs & ~ATTR_URL)) len--;
        }
        if (s == cur_src && mcx + 1 > len) { len = mcx + 1 < ls->cols ? mcx + 1 : ls->cols; }
        for (int c = 0; c < len; c++) {
            int wide = (c + 1 < ls->cols && ls->cells[c+1].codepoint == CP_WIDE_CONTINUATION);
            if (need_line || out_x >= cols || (wide && out_x == cols - 1)) {
                if (!need_line && out_n > 0) out[out_n-1].wrapped = 1;
                REFLOW_NEWLINE();
                need_line = 0;
            }
            if (s == cur_src && c == (mcx < len ? mcx : len - 1)) { cur_row = out_n - 1; cur_col = out_x; }
            if (c == 0 && ls->prompt) out[out_n-1].prompt = 1;
            out[out_n-1].cells[out_x++] = ls->cells[c];
        }
        if (len == 0 && need_line) { REFLOW_NEWLINE(); need_line = 0; out[out_n-1].prompt = ls->prompt; }
        if (s == cur_src && cur_row < 0) { cur_row = out_n - 1; cur_col = out_x < cols ? out_x : cols - 1; }
        if (!ls->wrapped) { need_line = 1; out_x = 0; }
    }
    if (out_n == 0) REFLOW_NEWLINE();
    if (cur_row < 0) { cur_row = out_n - 1; cur_col = 0; }
    #undef REFLOW_NEWLINE

    /* --- Split output into scrollback + screen, keeping the cursor on screen --- */
    int S = out_n > rows ? out_n - rows : 0;
    if (cur_row < S) S = cur_row;

    Line *nscr = calloc(rows, sizeof(Line));
    for (int r = 0; r < rows; r++) {
        int oi = S + r;
        if (oi < out_n) nscr[r] = out[oi];
        else {
            nscr[r].cells = calloc(cols, sizeof(Cell)); nscr[r].cols = cols;
            for (int j = 0; j < cols; j++) { nscr[r].cells[j].codepoint = ' '; nscr[r].cells[j].fg = COLOR_FG; nscr[r].cells[j].bg = COLOR_BG; nscr[r].cells[j].ul = UL_DEFAULT; }
        }
        nscr[r].dirty = 1;
    }
    for (int oi = S + rows; oi < out_n; oi++) free(out[oi].cells); /* lines pushed past the bottom */

    Line *nsb = calloc(t->sb.cap, sizeof(Line));
    int first = S > t->sb.cap ? S - t->sb.cap : 0;
    int cnt = S - first;
    for (int i = 0; i < cnt; i++) nsb[i] = out[first + i];
    for (int i = 0; i < first; i++) free(out[i].cells); /* oldest history dropped */

    /* --- Free old storage, install new --- */
    for (int i = 0; i < t->sb.cap; i++) free(t->sb.lines[i].cells);
    free(t->sb.lines);
    for (int r = 0; r < t->rows; r++) free(t->screen[r].cells);
    free(t->screen);
    free(src); free(out);
    t->sb.lines = nsb; t->sb.head = 0; t->sb.count = cnt;
    t->screen = nscr;

    Line *nalt = alloc_lines(rows, cols);
    for (int r = 0; r < rows && r < t->rows; r++)
        for (int c = 0; c < cols && c < t->cols; c++)
            nalt[r].cells[c] = t->alt_screen[r].cells[c];
    for (int r = 0; r < t->rows; r++) free(t->alt_screen[r].cells);
    free(t->alt_screen);
    t->alt_screen = nalt;

    t->cols = cols; t->rows = rows;
    t->scroll_top = 0; t->scroll_bottom = rows - 1;
    mcx = cur_col < cols ? cur_col : cols - 1;
    mcy = cur_row - S; if (mcy < 0) mcy = 0; if (mcy >= rows) mcy = rows - 1;
    if (t->alt_active) {
        t->saved_alt.x = mcx; t->saved_alt.y = mcy;
        if (t->cx >= cols) t->cx = cols - 1;
        if (t->cy >= rows) t->cy = rows - 1;
    } else {
        t->cx = mcx; t->cy = mcy;
    }
    t->pending_wrap = 0; t->sb_offset = 0;
    free(t->tabs); t->tabs = calloc(cols, sizeof(int)); for (int i=0; i<cols; i+=8) t->tabs[i]=1;
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(G.pty_fd, TIOCSWINSZ, &ws);
}

static void utf8_encode(char **p, uint32_t cp) {
    char *o = *p;
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = 0xC0 | (cp >> 6); *o++ = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { *o++ = 0xE0 | (cp >> 12); *o++ = 0x80 | ((cp >> 6) & 0x3F); *o++ = 0x80 | (cp & 0x3F); }
    else { *o++ = 0xF0 | (cp >> 18); *o++ = 0x80 | ((cp >> 12) & 0x3F); *o++ = 0x80 | ((cp >> 6) & 0x3F); *o++ = 0x80 | (cp & 0x3F); }
    *p = o;
}

/* Build a UTF-8 string from the current selection (on the visible lines,
 * so a selection made while scrolled back copies what's on screen). */
static char *extract_selection(Terminal *t) {
    int y1 = fmin(t->sel_y1, t->sel_y2), y2 = fmax(t->sel_y1, t->sel_y2);
    int x1 = (t->sel_y1 < t->sel_y2) ? t->sel_x1 : ((t->sel_y1 > t->sel_y2) ? t->sel_x2 : fmin(t->sel_x1, t->sel_x2));
    int x2 = (t->sel_y1 < t->sel_y2) ? t->sel_x2 : ((t->sel_y1 > t->sel_y2) ? t->sel_x1 : fmax(t->sel_x1, t->sel_x2));
    char *buf = malloc((size_t)(y2 - y1 + 1) * (t->cols + 2) * 4), *p = buf;
    for (int r = y1; r <= y2; r++) {
        Line *ln = visible_line(t, r);
        int c1 = (r == y1) ? x1 : 0, c2 = (r == y2) ? x2 : t->cols - 1;
        if (c2 > ln->cols - 1) c2 = ln->cols - 1;
        char *line_start = p;
        for (int c = c1; c <= c2; c++) {
            uint32_t cp = ln->cells[c].codepoint;
            if (cp == CP_WIDE_CONTINUATION) continue;
            if (cp < ' ') cp = ' ';
            utf8_encode(&p, cp);
        }
        while (p > line_start && p[-1] == ' ') p--; /* trim trailing blanks */
        if (r < y2) *p++ = '\n';
    }
    *p = 0;
    return buf;
}

static void own_selection(Time time, int clipboard_too) {
    if (!G.term->sel_active) return;
    free(G.sel_text);
    G.sel_text = extract_selection(G.term);
    XSetSelectionOwner(G.dpy, XA_PRIMARY, G.win, time);
    if (clipboard_too) XSetSelectionOwner(G.dpy, XInternAtom(G.dpy, "CLIPBOARD", False), G.win, time);
}

/* Pixel → cell coordinate, accounting for window padding, clamped to grid */
static int px_col(int x) {
    int c = (x - CFG.padding) / G_hw.font_w;
    if (c < 0) c = 0;
    if (c >= G.term->cols) c = G.term->cols - 1;
    return c;
}
static int px_row(int y) {
    int r = (y - CFG.padding) / G_hw.font_h;
    if (r < 0) r = 0;
    if (r >= G.term->rows) r = G.term->rows - 1;
    return r;
}

/* Send an xterm mouse event to the application.
 * type: 0 = press, 1 = release, 2 = motion. btn: 0/1/2 = left/middle/right,
 * 3 = none (any-motion mode), 64/65 = wheel up/down. */
static void mouse_report(int btn, int px, int py, int type, unsigned state) {
    Terminal *t = G.term;
    int cx = px_col(px) + 1, cy = px_row(py) + 1;
    if (type == 2) { /* dedupe motion reports within one cell */
        if (cx == G.last_mrx && cy == G.last_mry) return;
    }
    G.last_mrx = cx; G.last_mry = cy;
    int code = btn + ((state & ShiftMask) ? 4 : 0) + ((state & Mod1Mask) ? 8 : 0) + ((state & ControlMask) ? 16 : 0);
    if (type == 2) code += 32;
    char b[48]; int n;
    if (t->mouse_ext == 1006) {
        n = snprintf(b, sizeof(b), "\033[<%d;%d;%d%c", code, cx, cy, type == 1 ? 'm' : 'M');
    } else {
        if (type == 1) code = (code & ~3) | 3; /* legacy release = button 3 */
        if (cx > 222 || cy > 222) return;      /* legacy coords are single bytes */
        n = snprintf(b, sizeof(b), "\033[M%c%c%c", 32 + code, 32 + cx, 32 + cy);
    }
    write(G.pty_fd, b, n);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    config_load();
    palette_init();
    G.win_w = 1024; G.win_h = 768; G.focused = 1;
    int opt;
    while ((opt = getopt(argc, argv, "w:h:")) != -1) {
        switch (opt) {
            case 'w': G.win_w = atoi(optarg); break;
            case 'h': G.win_h = atoi(optarg); break;
        }
    }

    G.dpy = XOpenDisplay(NULL); if (!G.dpy) die("Can't open display");
    G.screen = DefaultScreen(G.dpy);

    int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
    XVisualInfo *vi = glXChooseVisual(G.dpy, G.screen, attr);
    if (!vi) die("No suitable visual found");

    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(G.dpy, RootWindow(G.dpy, vi->screen), vi->visual, AllocNone);
    swa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask | FocusChangeMask | VisibilityChangeMask;
    G.win = XCreateWindow(G.dpy, RootWindow(G.dpy, vi->screen), 0, 0, G.win_w, G.win_h, 0,
                          vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(G.dpy, G.win);
    XStoreName(G.dpy, G.win, "PeanutBuTTY");
    XClassHint ch = { (char*)"peanutbutty", (char*)"PeanutBuTTY" };
    XSetClassHint(G.dpy, G.win, &ch);
    { /* window icon: _NET_WM_ICON is a CARDINAL/32 array of {w, h, ARGB...} per size */
        size_t n = 0;
        for (int i = 0; i < ICON_LEVEL_COUNT; i++) n += 2 + (size_t)ICON_LEVELS[i].size * ICON_LEVELS[i].size;
        unsigned long *buf = malloc(n * sizeof(*buf));
        if (buf) {
            size_t k = 0;
            for (int i = 0; i < ICON_LEVEL_COUNT; i++) {
                int sz = ICON_LEVELS[i].size; const uint8_t *p = ICON_LEVELS[i].argb;
                buf[k++] = (unsigned long)sz; buf[k++] = (unsigned long)sz;
                for (int j = 0; j < sz * sz; j++, p += 4)
                    buf[k++] = ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
            }
            XChangeProperty(G.dpy, G.win, XInternAtom(G.dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32,
                            PropModeReplace, (unsigned char*)buf, (int)n);
            free(buf);
        }
    }
    Atom wm_delete = XInternAtom(G.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(G.dpy, G.win, &wm_delete, 1);
    signal(SIGPIPE, SIG_IGN); /* don't die writing to the pty after the shell exits */

    /* Input method (ibus/fcitx/dead keys); fall back to raw XLookupString without one */
    XSetLocaleModifiers("");
    G.xim = XOpenIM(G.dpy, NULL, NULL, NULL);
    if (!G.xim) { XSetLocaleModifiers("@im=none"); G.xim = XOpenIM(G.dpy, NULL, NULL, NULL); }
    if (G.xim) {
        G.xic = XCreateIC(G.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, G.win, XNFocusWindow, G.win, NULL);
        if (G.xic) {
            unsigned long fevt = 0;
            XGetICValues(G.xic, XNFilterEvents, &fevt, NULL);
            XSelectInput(G.dpy, G.win, swa.event_mask | fevt);
        }
    }

    /* Live config reload: SIGUSR1, or the config file changing on disk */
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    sigaction(SIGUSR1, &sa, NULL); /* no SA_RESTART: select returns EINTR and the loop reapplies */
    G.ino_fd = -1;
    if (conf_path_s[0]) {
        G.ino_fd = inotify_init1(IN_NONBLOCK);
        if (G.ino_fd >= 0) {
            char dir[512]; strcpy(dir, conf_path_s);
            char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
            if (inotify_add_watch(G.ino_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
                close(G.ino_fd); G.ino_fd = -1;
            }
        }
    }

    G.ctx = glXCreateContext(G.dpy, vi, NULL, True);
    glXMakeCurrent(G.dpy, G.win, G.ctx);
    gl2_load(); init_hw_resources(G.dpy, G.win);

    int cols = (G.win_w - 2*CFG.padding) / G_hw.font_w;
    int rows = (G.win_h - 2*CFG.padding) / G_hw.font_h;
    if (cols < 40) { cols = 40; }
    if (rows < 10) { rows = 10; }

    wm_set_size_hints();

    glViewport(0, 0, G.win_w, G.win_h);

    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &VERT_SRC, NULL); glCompileShader(v);
    glShaderSource(f, 1, &FRAG_SRC, NULL); glCompileShader(f);
    G.prog = glCreateProgram(); glAttachShader(G.prog, v); glAttachShader(G.prog, f); glLinkProgram(G.prog);
    GLint ok; glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(G.prog, 512, NULL, log); die("Shader link error: %s", log); }

    G.u_proj = glGetUniformLocation(G.prog, "u_proj"); G.u_tex = glGetUniformLocation(G.prog, "u_tex");
    G.u_win_h = glGetUniformLocation(G.prog, "u_win_h"); G.u_win_w = glGetUniformLocation(G.prog, "u_win_w");
    G.a_pos = glGetAttribLocation(G.prog, "a_pos");
    G.a_uv = glGetAttribLocation(G.prog, "a_uv"); G.a_color = glGetAttribLocation(G.prog, "a_color");
    G.term = term_new(cols, rows);
    
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    int master, slave; openpty(&master, &slave, NULL, NULL, &ws); G.pty_fd = master; G.child_pid = fork();
    if (G.child_pid == 0) { setsid(); ioctl(slave, TIOCSCTTY, 0); dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        setenv("TERM", "xterm-256color", 1); setenv("COLORTERM", "truecolor", 1);
        const char *sh = getenv("SHELL"); if (!sh || !*sh) sh = "/bin/sh";
        execl(sh, sh, NULL); _exit(127); }
    close(slave); /* parent must not hold the slave open, or EOF never reaches the master */

    fprintf(stderr, "peanutbutty: Grid %dx%d, Font %dx%d\n", cols, rows, G_hw.font_w, G_hw.font_h);
    XRaiseWindow(G.dpy, G.win);
    G.ptr_text = XCreateFontCursor(G.dpy, 152); /* XC_xterm */
    G.ptr_hand = XCreateFontCursor(G.dpy, 60);  /* XC_hand2 */
    XDefineCursor(G.dpy, G.win, G.ptr_text);
    
    fcntl(master, F_SETFL, O_NONBLOCK); G.running = 1; uint64_t last_blink = 0;
    while (G.running) {
        uint64_t f_start = now_ms(); int render = 0;
        /* Blink only when something on screen actually blinks: a focused
         * blinking-style cursor, or blink-attribute cells. Otherwise pin the
         * phase on so idle wakeups stop entirely. */
        int cursor_blinks = G.focused && G.term->cursor_visible && G.term->sb_offset == 0 &&
                            ((G.term->cursor_style ? G.term->cursor_style : 1) % 2 == 1);
        int need_blink = cursor_blinks || (G.focused && G.term->has_blink);
        if (need_blink) {
            if (f_start - last_blink > (uint64_t)BLINK_MS) { G.cursor_phase = !G.cursor_phase; last_blink = f_start; render = 1; }
        } else if (!G.cursor_phase) { G.cursor_phase = 1; last_blink = f_start; render = 1; }
        /* Visual bell expiry */
        if (G.bell_until && f_start >= G.bell_until) { G.bell_until = 0; render = 1; }
        /* Debounced reflow after an interactive resize settles */
        if (G.resize_pending && f_start >= G.resize_deadline) {
            G.resize_pending = 0;
            term_resize(G.term, G.pend_cols, G.pend_rows);
            render = 1;
        }
        /* Mode 2026 safety: a stuck synchronized update must not freeze us */
        if (G.term->synchronized_update && f_start > G.term->sync_deadline) G.term->synchronized_update = 0;
        /* Live config reload: SIGUSR1 or the config file rewritten on disk */
        if (G.ino_fd >= 0) {
            char ibuf[1024] __attribute__((aligned(8))); int rn;
            while ((rn = read(G.ino_fd, ibuf, sizeof(ibuf))) > 0) {
                for (int off = 0; off + (int)sizeof(struct inotify_event) <= rn; ) {
                    struct inotify_event *ie = (struct inotify_event*)(ibuf + off);
                    if (ie->len && !strcmp(ie->name, "peanutbutty.conf")) reload_requested = 1;
                    off += sizeof(*ie) + ie->len;
                }
            }
        }
        if (reload_requested) { reload_requested = 0; config_reapply(); render = 1; }
        while (XPending(G.dpy)) {
            XEvent ev; XNextEvent(G.dpy, &ev);
            if (XFilterEvent(&ev, None)) continue;
            if (ev.type == Expose) render = 1;
            else if (ev.type == ClientMessage) {
                if ((Atom)ev.xclient.data.l[0] == wm_delete) G.running = 0;
            }
            else if (ev.type == VisibilityNotify) {
                /* Window becoming visible after blank/DPMS — GL resources may be stale */
                if (ev.xvisibility.state != VisibilityFullyObscured)
                    G.reinit_needed = 1;
            }
            else if (ev.type == ConfigureNotify) {
                if (ev.xconfigure.width != G.win_w || ev.xconfigure.height != G.win_h) {
                    G.win_w = ev.xconfigure.width; G.win_h = ev.xconfigure.height;
                    glViewport(0, 0, G.win_w, G.win_h);
                    int nc = (G.win_w - 2*CFG.padding) / G_hw.font_w;
                    int nr = (G.win_h - 2*CFG.padding) / G_hw.font_h;
                    if (nc < 2) nc = 2;
                    if (nr < 1) nr = 1;
                    /* Debounce: reflowing 5000 lines of history per motion event
                     * during an interactive resize would stall the UI */
                    G.pend_cols = nc; G.pend_rows = nr;
                    G.resize_pending = 1; G.resize_deadline = now_ms() + 80;
                    render = 1;
                }
            }
            else if (ev.type == MotionNotify) {
                int cx = px_col(ev.xmotion.x);
                int cy = px_row(ev.xmotion.y);
                int report = G.term->mouse_mode && !(ev.xmotion.state & ShiftMask);
                {
                    Line *ln = visible_line(G.term, cy);
                    if (cx < ln->cols && (ln->cells[cx].attrs & ATTR_URL) && (ev.xmotion.state & ControlMask))
                        XDefineCursor(G.dpy, G.win, G.ptr_hand);
                    else
                        XDefineCursor(G.dpy, G.win, G.ptr_text);
                    if (report && G.term->mouse_mode >= 1002) {
                        int btn = -1;
                        if (ev.xmotion.state & Button1Mask) btn = 0;
                        else if (ev.xmotion.state & Button2Mask) btn = 1;
                        else if (ev.xmotion.state & Button3Mask) btn = 2;
                        if (btn >= 0 || G.term->mouse_mode == 1003)
                            mouse_report(btn >= 0 ? btn : 3, ev.xmotion.x, ev.xmotion.y, 2, ev.xmotion.state);
                    } else if (!report && (ev.xmotion.state & Button1Mask)) {
                        G.term->sel_x2 = cx; G.term->sel_y2 = cy; render = 1;
                    }
                }
            }
            else if (ev.type == ButtonPress) {
                int report = G.term->mouse_mode && !(ev.xbutton.state & ShiftMask);
                if (ev.xbutton.button == 4 || ev.xbutton.button == 5) {
                    int up = (ev.xbutton.button == 4);
                    if (ev.xbutton.state & ControlMask) { /* Ctrl+wheel zooms the font */
                        font_resize(G_hw.font_sz + (up ? 1 : -1));
                        render = 1;
                        continue;
                    }
                    if (report) mouse_report(up ? 64 : 65, ev.xbutton.x, ev.xbutton.y, 0, ev.xbutton.state);
                    else if (G.term->alt_active) { /* wheel scrolls fullscreen apps via arrow keys */
                        const char *a = up ? (G.term->app_cursor_keys ? "\033OA" : "\033[A")
                                           : (G.term->app_cursor_keys ? "\033OB" : "\033[B");
                        for (int i = 0; i < 3; i++) write(G.pty_fd, a, 3);
                    } else {
                        G.term->sb_offset += up ? 3 : -3;
                        if (G.term->sb_offset > G.term->sb.count) G.term->sb_offset = G.term->sb.count;
                        if (G.term->sb_offset < 0) G.term->sb_offset = 0;
                        render = 1;
                    }
                } else if (report) {
                    mouse_report(ev.xbutton.button - 1, ev.xbutton.x, ev.xbutton.y, 0, ev.xbutton.state);
                } else if (ev.xbutton.button == 1) {
                    int cx = px_col(ev.xbutton.x);
                    int cy = px_row(ev.xbutton.y);
                    {
                        Line *ln = visible_line(G.term, cy);
                        /* Multi-click: 2 = word, 3 = line */
                        uint64_t nw = now_ms();
                        if (nw - G.last_click_ms < 400 && cx == G.click_cx && cy == G.click_cy) G.click_count++;
                        else G.click_count = 1;
                        G.last_click_ms = nw; G.click_cx = cx; G.click_cy = cy;
                        if (cx < ln->cols && (ln->cells[cx].attrs & ATTR_URL) && (ev.xbutton.state & ControlMask)) {
                            /* Find URL bounds */
                            int start = cx, end = cx;
                            while (start > 0 && (ln->cells[start-1].attrs & ATTR_URL)) start--;
                            while (end < ln->cols-1 && (ln->cells[end+1].attrs & ATTR_URL)) end++;
                            char url[MAX_OSC] = {0};
                            for (int i=0; i<=(end-start) && i<MAX_OSC-1; i++) url[i] = (char)ln->cells[start+i].codepoint;
                            if (fork() == 0) {
                                execlp("xdg-open", "xdg-open", url, NULL);
                                exit(0);
                            }
                        } else if (G.click_count == 2 && cx < ln->cols) { /* word select */
                            #define IS_WORD(cp) ((cp) > 127 || isalnum((int)(cp)) || strchr("-_./~", (int)(cp)))
                            int s2 = cx, e2 = cx;
                            if (IS_WORD(ln->cells[cx].codepoint)) {
                                while (s2 > 0 && IS_WORD(ln->cells[s2-1].codepoint)) s2--;
                                while (e2 < ln->cols-1 && IS_WORD(ln->cells[e2+1].codepoint)) e2++;
                            }
                            #undef IS_WORD
                            G.term->sel_x1 = s2; G.term->sel_x2 = e2;
                            G.term->sel_y1 = G.term->sel_y2 = cy;
                            G.term->sel_active = 1; render = 1;
                        } else if (G.click_count >= 3) { /* line select */
                            G.term->sel_x1 = 0; G.term->sel_x2 = G.term->cols - 1;
                            G.term->sel_y1 = G.term->sel_y2 = cy;
                            G.term->sel_active = 1; render = 1;
                        } else {
                            G.term->sel_x1 = G.term->sel_x2 = cx;
                            G.term->sel_y1 = G.term->sel_y2 = cy;
                            G.term->sel_active = 1; render = 1;
                        }
                    }
                } else if (ev.xbutton.button == 2) {
                    XConvertSelection(G.dpy, XA_PRIMARY, XInternAtom(G.dpy, "UTF8_STRING", False), XInternAtom(G.dpy, "XSEL_DATA", False), G.win, ev.xbutton.time);
                }
            }
            else if (ev.type == SelectionNotify) {
                if (ev.xselection.property != None) {
                    Atom type; int format; unsigned long nitems, after; unsigned char *data;
                    XGetWindowProperty(G.dpy, G.win, ev.xselection.property, 0, 1024*1024, True, AnyPropertyType, &type, &format, &nitems, &after, &data);
                    if (data) {
                        if (G.term->bracketed_paste) write(G.pty_fd, "\033[200~", 6);
                        write(G.pty_fd, data, nitems);
                        if (G.term->bracketed_paste) write(G.pty_fd, "\033[201~", 6);
                        XFree(data);
                    }
                }
            }
            else if (ev.type == ButtonRelease) {
                int report = G.term->mouse_mode && !(ev.xbutton.state & ShiftMask);
                if (report && ev.xbutton.button >= 1 && ev.xbutton.button <= 3) {
                    mouse_report(ev.xbutton.button - 1, ev.xbutton.x, ev.xbutton.y, 1, ev.xbutton.state);
                } else if (ev.xbutton.button == 1) {
                    if (G.click_count == 1 && G.term->sel_x1 == G.term->sel_x2 && G.term->sel_y1 == G.term->sel_y2) {
                        G.term->sel_active = 0; render = 1;
                    } else {
                        own_selection(ev.xbutton.time, 0); /* PRIMARY only; Ctrl+Shift+C for CLIPBOARD */
                    }
                }
            }
            else if (ev.type == SelectionRequest) {
                XSelectionRequestEvent *req = &ev.xselectionrequest;
                XSelectionEvent sev = { SelectionNotify, .display = G.dpy, .requestor = req->requestor,
                    .selection = req->selection, .target = req->target, .property = req->property, .time = req->time };
                Atom utf8 = XInternAtom(G.dpy, "UTF8_STRING", False);
                Atom targets = XInternAtom(G.dpy, "TARGETS", False);
                if (sev.property == None) sev.property = req->target; /* obsolete clients */
                if (req->target == targets) {
                    Atom tl[3] = { targets, utf8, XA_STRING };
                    XChangeProperty(G.dpy, req->requestor, sev.property, XA_ATOM, 32, PropModeReplace, (unsigned char*)tl, 3);
                } else if ((req->target == utf8 || req->target == XA_STRING) && G.sel_text) {
                    XChangeProperty(G.dpy, req->requestor, sev.property, req->target, 8, PropModeReplace, (unsigned char*)G.sel_text, strlen(G.sel_text));
                } else sev.property = None;
                XSendEvent(G.dpy, req->requestor, True, 0, (XEvent*)&sev);
            }
            else if (ev.type == FocusIn) {
                G.focused = 1; set_urgency(0); render = 1;
                if (G.xic) XSetICFocus(G.xic);
                if (G.term->focus_report) write(G.pty_fd, "\033[I", 3);
            }
            else if (ev.type == FocusOut) {
                G.focused = 0; render = 1;
                if (G.xic) XUnsetICFocus(G.xic);
                if (G.term->focus_report) write(G.pty_fd, "\033[O", 3);
            }
            else if (ev.type == KeyPress) {
                char buf[64]; KeySym ks = NoSymbol; Status xst = 0;
                int n;
                if (G.xic) {
                    n = Xutf8LookupString(G.xic, &ev.xkey, buf, sizeof(buf)-1, &ks, &xst);
                    if (xst == XBufferOverflow) n = 0;
                } else n = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
                unsigned st = ev.xkey.state;
                /* URL hint mode: a label opens the URL; any other character key exits */
                if (G.hints.active) {
                    if (n == 0) continue; /* bare modifiers keep the mode alive */
                    if (n == 1 && !(st & (ControlMask | Mod1Mask)))
                        for (int i = 0; i < G.hints.n; i++)
                            if (G.hints.item[i].label == buf[0]) {
                                open_url_run(visible_line(G.term, G.hints.item[i].row),
                                             G.hints.item[i].c1, G.hints.item[i].c2);
                                break;
                            }
                    G.hints.active = 0; render = 1;
                    continue;
                }
                /* Scrollback search mode swallows all keys */
                if (G.search.active) {
                    if (ks == XK_Escape) G.search.active = 0;
                    else if (ks == XK_Return || ks == XK_Up ||
                             ((st & ControlMask) && (st & ShiftMask) && (ks == XK_F || ks == XK_f)))
                        search_move(G.term, -1);
                    else if (ks == XK_Down) search_move(G.term, +1);
                    else if (ks == XK_BackSpace) { if (G.search.qlen) G.search.qlen--; search_move(G.term, 0); }
                    else if (n > 0 && !(st & ControlMask)) {
                        for (int bi = 0; bi < n && G.search.qlen < 64; ) {
                            uint8_t b0 = (uint8_t)buf[bi]; uint32_t cp; int adv = 1;
                            if (b0 < 0x80) cp = b0;
                            else if ((b0 & 0xE0) == 0xC0 && bi+1 < n) { cp = ((b0 & 0x1F) << 6) | (buf[bi+1] & 0x3F); adv = 2; }
                            else if ((b0 & 0xF0) == 0xE0 && bi+2 < n) { cp = ((b0 & 0x0F) << 12) | ((buf[bi+1] & 0x3F) << 6) | (buf[bi+2] & 0x3F); adv = 3; }
                            else { bi++; continue; }
                            bi += adv;
                            if (cp >= 0x20) G.search.q[G.search.qlen++] = (uint32_t)towlower((wint_t)cp);
                        }
                        search_move(G.term, 0);
                    }
                    render = 1;
                    continue;
                }
                /* Copy / paste */
                if ((st & ControlMask) && (st & ShiftMask) && (ks == XK_C || ks == XK_c)) {
                    own_selection(ev.xkey.time, 1);
                    continue;
                }
                if ((st & ControlMask) && (st & ShiftMask) && (ks == XK_V || ks == XK_v)) {
                    XConvertSelection(G.dpy, XInternAtom(G.dpy, "CLIPBOARD", False),
                                      XInternAtom(G.dpy, "UTF8_STRING", False),
                                      XInternAtom(G.dpy, "XSEL_DATA", False), G.win, ev.xkey.time);
                    continue;
                }
                /* Font size: Ctrl+Minus / Ctrl+Equal(Plus) / Ctrl+0, with or without
                 * Shift, plus the keypad keys. Match on the unshifted keysym so it works
                 * on layouts where Shift+Minus isn't Underscore. */
                if (st & ControlMask) {
                    KeySym base = XLookupKeysym(&ev.xkey, 0);
                    int dir = 0, reset = 0;
                    if (ks == XK_plus || ks == XK_equal || base == XK_plus || base == XK_equal ||
                        ks == XK_KP_Add) dir = +1;
                    else if (ks == XK_minus || ks == XK_underscore || base == XK_minus ||
                             ks == XK_KP_Subtract) dir = -1;
                    else if (ks == XK_0 || ks == XK_parenright || base == XK_0 ||
                             ks == XK_KP_0 || ks == XK_KP_Insert) reset = 1;
                    if (dir || reset) {
                        font_resize(reset ? CFG.size : G_hw.font_sz + dir);
                        render = 1;
                        continue;
                    }
                }
                /* Scrollback search */
                if ((st & ControlMask) && (st & ShiftMask) && (ks == XK_F || ks == XK_f) && !G.term->alt_active) {
                    G.search.active = 1; G.search.qlen = 0; G.search.abs = -1; G.search.count = 0;
                    render = 1;
                    continue;
                }
                /* URL hints */
                if ((st & ControlMask) && (st & ShiftMask) && (ks == XK_U || ks == XK_u)) {
                    hints_build(G.term);
                    render = 1;
                    continue;
                }
                /* Jump to previous / next shell prompt (OSC 133 marks) */
                if ((st & ControlMask) && (st & ShiftMask) && (ks == XK_Up || ks == XK_Down) && !G.term->alt_active) {
                    Terminal *t = G.term;
                    int top = t->sb.count - t->sb_offset;
                    if (ks == XK_Up) {
                        for (int i = top - 1; i >= 0; i--)
                            if (term_abs_line(t, i)->prompt) {
                                t->sb_offset = t->sb.count - i;
                                if (t->sb_offset > t->sb.count) t->sb_offset = t->sb.count;
                                break;
                            }
                    } else {
                        int found = 0, N = t->sb.count + t->rows;
                        for (int i = top + 1; i < N; i++)
                            if (term_abs_line(t, i)->prompt) {
                                t->sb_offset = t->sb.count - i;
                                if (t->sb_offset < 0) t->sb_offset = 0;
                                found = 1;
                                break;
                            }
                        if (!found) t->sb_offset = 0;
                    }
                    render = 1;
                    continue;
                }
                /* Scrollback paging */
                if ((st & ShiftMask) && (ks == XK_Prior || ks == XK_Next) && !G.term->alt_active) {
                    int page = G.term->rows - 1; if (page < 1) page = 1;
                    G.term->sb_offset += (ks == XK_Prior) ? page : -page;
                    if (G.term->sb_offset > G.term->sb.count) G.term->sb_offset = G.term->sb.count;
                    if (G.term->sb_offset < 0) G.term->sb_offset = 0;
                    render = 1;
                    continue;
                }
                /* Any other key snaps the view back to the live screen */
                if (G.term->sb_offset && ks != XK_Shift_L && ks != XK_Shift_R &&
                    ks != XK_Control_L && ks != XK_Control_R && ks != XK_Alt_L && ks != XK_Alt_R) {
                    G.term->sb_offset = 0; render = 1;
                }
                /* kitty keyboard protocol, level 1 (disambiguate escape codes):
                 * Esc and modified keys get unambiguous CSI u encodings; plain
                 * Enter/Tab/Backspace keep their legacy bytes per the spec. */
                if (G.term->kitty_stack[G.term->kitty_n-1] & 1) {
                    int kmod = 1 + ((st & ShiftMask) ? 1 : 0) + ((st & Mod1Mask) ? 2 : 0) + ((st & ControlMask) ? 4 : 0);
                    char kb[32]; int kn = 0;
                    if (ks == XK_Escape) {
                        kn = kmod > 1 ? sprintf(kb, "\033[27;%du", kmod) : sprintf(kb, "\033[27u");
                    } else if (st & (ControlMask | Mod1Mask)) {
                        KeySym base = XLookupKeysym(&ev.xkey, 0);
                        uint32_t kcp = 0;
                        if (base >= 0x20 && base <= 0x7E) kcp = (uint32_t)base;
                        else if (base == XK_Return) kcp = 13;
                        else if (base == XK_Tab) kcp = 9;
                        else if (base == XK_BackSpace) kcp = 127;
                        if (kcp) kn = sprintf(kb, "\033[%u;%du", kcp, kmod);
                    }
                    if (kn) { write(G.pty_fd, kb, kn); continue; }
                }
                /* Keysym table first: X's lookup string turns Delete into 0x7f
                 * (identical to Backspace) and Backspace into 0x08, so the
                 * table must win; the lookup string is only for plain text. */
                {
                    const char *seq = NULL; char sbuf[64];
                    int mod = 1 + (ev.xkey.state & ShiftMask ? 1 : 0) + (ev.xkey.state & Mod1Mask ? 2 : 0) + (ev.xkey.state & ControlMask ? 4 : 0);
                    
                    switch (ks) {
                        case XK_Up:    if (mod>1) { sprintf(sbuf, "\033[1;%dA", mod); seq=sbuf; } else seq = G.term->app_cursor_keys ? "\033OA" : "\033[A"; break;
                        case XK_Down:  if (mod>1) { sprintf(sbuf, "\033[1;%dB", mod); seq=sbuf; } else seq = G.term->app_cursor_keys ? "\033OB" : "\033[B"; break;
                        case XK_Right: if (mod>1) { sprintf(sbuf, "\033[1;%dC", mod); seq=sbuf; } else seq = G.term->app_cursor_keys ? "\033OC" : "\033[C"; break;
                        case XK_Left:  if (mod>1) { sprintf(sbuf, "\033[1;%dD", mod); seq=sbuf; } else seq = G.term->app_cursor_keys ? "\033OD" : "\033[D"; break;
                        case XK_Home:  if (mod>1) { sprintf(sbuf, "\033[1;%dH", mod); seq=sbuf; } else seq = "\033[H"; break;
                        case XK_End:   if (mod>1) { sprintf(sbuf, "\033[1;%dF", mod); seq=sbuf; } else seq = "\033[F"; break;
                        case XK_Prior: if (mod>1) { sprintf(sbuf, "\033[5;%d~", mod); seq=sbuf; } else seq = "\033[5~"; break;
                        case XK_Next:  if (mod>1) { sprintf(sbuf, "\033[6;%d~", mod); seq=sbuf; } else seq = "\033[6~"; break;
                        case XK_Insert: if (mod>1) { sprintf(sbuf, "\033[2;%d~", mod); seq=sbuf; } else seq = "\033[2~"; break;
                        case XK_Delete: if (mod>1) { sprintf(sbuf, "\033[3;%d~", mod); seq=sbuf; } else seq = "\033[3~"; break;
                        case XK_F1: if (mod>1) { sprintf(sbuf, "\033[1;%dP", mod); seq=sbuf; } else seq = "\033OP"; break;
                        case XK_F2: if (mod>1) { sprintf(sbuf, "\033[1;%dQ", mod); seq=sbuf; } else seq = "\033OQ"; break;
                        case XK_F3: if (mod>1) { sprintf(sbuf, "\033[1;%dR", mod); seq=sbuf; } else seq = "\033OR"; break;
                        case XK_F4: if (mod>1) { sprintf(sbuf, "\033[1;%dS", mod); seq=sbuf; } else seq = "\033OS"; break;
                        case XK_F5:  sprintf(sbuf, "\033[15;%d~", mod); seq=sbuf; break;
                        case XK_F6:  sprintf(sbuf, "\033[17;%d~", mod); seq=sbuf; break;
                        case XK_F7:  sprintf(sbuf, "\033[18;%d~", mod); seq=sbuf; break;
                        case XK_F8:  sprintf(sbuf, "\033[19;%d~", mod); seq=sbuf; break;
                        case XK_F9:  sprintf(sbuf, "\033[20;%d~", mod); seq=sbuf; break;
                        case XK_F10: sprintf(sbuf, "\033[21;%d~", mod); seq=sbuf; break;
                        case XK_F11: sprintf(sbuf, "\033[23;%d~", mod); seq=sbuf; break;
                        case XK_F12: sprintf(sbuf, "\033[24;%d~", mod); seq=sbuf; break;
                        case XK_KP_Up:    seq = G.term->app_keypad ? "\033Ox" : "\033[A"; break;
                        case XK_KP_Down:  seq = G.term->app_keypad ? "\033Or" : "\033[B"; break;
                        case XK_KP_Right: seq = G.term->app_keypad ? "\033Ov" : "\033[C"; break;
                        case XK_KP_Left:  seq = G.term->app_keypad ? "\033Ot" : "\033[D"; break;
                        case XK_KP_Begin: seq = G.term->app_keypad ? "\033Ou" : "\033[E"; break;
                        case XK_KP_Insert: seq = G.term->app_keypad ? "\033Op" : "\033[2~"; break;
                        case XK_KP_Delete: seq = G.term->app_keypad ? "\033On" : "\033[3~"; break;
                        case XK_BackSpace: seq = "\177"; break;
                        case XK_Return: seq = "\r"; break;
                        case XK_Tab:
                            if (ev.xkey.state & ShiftMask) seq = "\033[Z";
                            else seq = "\t";
                            break;
                        case XK_Escape: seq = "\033"; break;
                    }
                    if (seq) {
                        if ((ev.xkey.state & Mod1Mask) && (ks == XK_Return || ks == XK_Tab || ks == XK_BackSpace)) { char esc = 0x1B; write(G.pty_fd, &esc, 1); }
                        write(G.pty_fd, seq, strlen(seq));
                    } else if (n > 0) {
                        if (ev.xkey.state & Mod1Mask) { char esc = 0x1B; write(G.pty_fd, &esc, 1); }
                        write(G.pty_fd, buf, n);
                    }
                }
            }
        }
        /* Recreate GL objects if screen was blanked/DPMS and context was lost */
        if (G.reinit_needed) { gl_recreate_resources(); render = 1; }
        /* Drain the PTY fully before rendering to batch all available output into one frame */
        uint8_t buf[READ_BUF] __attribute__((aligned(16)));
        int n = -1, drained = 0;
        while (drained < 64 && (n = read(G.pty_fd, buf, sizeof(buf))) > 0) {
            term_process(G.term, buf, n); render = 1; drained++;
        }
        /* Shell exited: pty master reads EOF (or EIO on Linux) */
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            G.running = 0;
        /* Only render if NOT in synchronized update mode OR if update just finished.
         * glXSwapBuffers with vsync provides the necessary GPU sync — no glFinish needed. */
        if (render && !G.term->synchronized_update) { render_frame(); }

        /* Sleep policy: frame-pace at ~60fps while output flows; otherwise
         * wake only for the next blink (if anything blinks), a pending bell /
         * debounced resize, or fd activity. Idle cost is near zero. */
        uint64_t now = now_ms();
        long wait_ms;
        if (drained > 0) {
            uint64_t elapsed = now - f_start;
            wait_ms = elapsed < 16 ? (long)(16 - elapsed) : 0;
        } else if (G.bell_until || G.resize_pending) {
            wait_ms = 20;
        } else if (need_blink) {
            long u = (long)((last_blink + BLINK_MS) - now);
            wait_ms = u < 1 ? 1 : u;
        } else {
            wait_ms = 10000;
        }
        if (wait_ms > 0 && G.running) {
            struct timeval tv = { wait_ms / 1000, (int)(wait_ms % 1000) * 1000 };
            fd_set fds; FD_ZERO(&fds); FD_SET(G.pty_fd, &fds); FD_SET(ConnectionNumber(G.dpy), &fds);
            int maxfd = (int)fmax(G.pty_fd, ConnectionNumber(G.dpy));
            if (G.ino_fd >= 0) { FD_SET(G.ino_fd, &fds); if (G.ino_fd > maxfd) maxfd = G.ino_fd; }
            select(maxfd + 1, &fds, NULL, NULL, &tv);
        }
    }
    kill(G.child_pid, SIGHUP);
    waitpid(G.child_pid, NULL, WNOHANG);
    XCloseDisplay(G.dpy);
    return 0;
}
