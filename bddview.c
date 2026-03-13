/*************************************************************
 * bddview.c — Midway BDD/BDB background viewer
 * Portable C99 + SDL2, no other dependencies.
 *
 * BDB: ASCII text, object placement list
 * BDD: binary image container (indexed palette)
 *
 * Usage:  bddview  <file.BDB>    auto-loads matching .BDD
 *         bddview  <file.BDD>    image-grid view, no world layout
 *
 * Keys:
 *   Arrow keys / left-drag   Scroll
 *   Scroll wheel / +/-       Zoom in/out
 *   Home                     Reset view
 *   Esc                      Quit
 *************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <SDL.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define strcasecmp  _stricmp
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#endif

/* ------------------------------------------------------------------ */
/* Types                                                                */
/* ------------------------------------------------------------------ */

#define MAX_IMAGES   256
#define MAX_OBJECTS 1024

typedef struct {
    int    idx;     /* hex index from BDD header                */
    int    w, h;    /* pixel dimensions                         */
    int    flags;   /* DMA control bit 0 (from BDD header)      */
    int    pal_idx; /* which palette to use (-1 = none yet)     */
    Uint8 *pix;     /* w*h palette-indexed bytes                */
} Img;

typedef struct {
    int wx;         /* DMA ctrl word (scroll rate + flip flags) */
    int depth;      /* depth / horizontal position              */
    int sy;         /* screen Y                                 */
    int ii;         /* image index into BDD (hex)               */
    int fl;         /* palette index                            */
    int hfl;        /* horizontal flip (wx bit 4)               */
    int vfl;        /* vertical flip   (wx bit 5)               */
    int order;      /* original BDB file index (for stable sort) */
} Obj;

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

static Img  g_img[MAX_IMAGES];  static int g_ni = 0;
static Obj  g_obj[MAX_OBJECTS]; static int g_no = 0;
static char g_name[64] = "";
static int  g_have_bdb = 0;

/* BDB save state — header and module lines stored verbatim on load */
#define MAX_MODULES 32
static char g_bdb_path[512]                = "";
static char g_bdb_header[256]              = "";
static char g_bdb_modules[MAX_MODULES][256];
static int  g_bdb_num_modules              = 0;

/* Palettes loaded from BDD tail section (15-bit RGB → ARGB8888).
   Stored in the order they appear in the BDD file; the BDB fl field
   is the 0-based index into this list. */
#define MAX_PALS 64
static Uint32 g_pals[MAX_PALS][256];
static int    g_pal_count[MAX_PALS]; /* number of entries in each palette */
static int    g_n_pals = 0;

/* Tooltip texture (built on hover, freed on mouse move) */
static SDL_Texture *g_tip_tex = NULL;
static int g_tip_x, g_tip_y, g_tip_w, g_tip_h;

/* Save-confirmation popup */
static SDL_Texture *g_popup_tex = NULL;
static int g_popup_w, g_popup_h;
static int g_confirm_save = 0; /* 1 = popup visible, waiting for Y/N */

/* forward declarations */
static Img *img_find(int idx);
static void font_draw_str(SDL_Surface *surf, int x, int y, const char *s, Uint32 fg);

/* ------------------------------------------------------------------ */
/* BDD loader                                                           */
/* ------------------------------------------------------------------ */

static int bdd_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "bdd: cannot open %s\n", path); return 0; }

    char ln[128];
    /* first line: version/count — consume and ignore */
    if (!fgets(ln, sizeof ln, f)) { fclose(f); return 0; }

    char leftover[128] = "";  /* line consumed by image loop but not used */
    g_ni = 0;
    while (!feof(f) && g_ni < MAX_IMAGES) {
        if (!fgets(ln, sizeof ln, f)) break;
        ln[strcspn(ln, "\r\n")] = '\0';
        if (!ln[0]) continue;

        /* header: <idx_hex> <w> <h> <flags> */
        char a[16], b[16], c[16], d[16];
        if (sscanf(ln, "%15s %15s %15s %15s", a, b, c, d) < 4) {
            /* Likely the first palette header — save it for the palette loop */
            snprintf(leftover, sizeof leftover, "%s", ln);
            break;
        }

        int idx = (int)strtol(a, NULL, 16);
        int w   = atoi(b);
        int h   = atoi(c);
        int fl  = atoi(d);

        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) break;

        Uint8 *pix = (Uint8 *)malloc((size_t)(w * h));
        if (!pix) { fprintf(stderr, "bdd: out of memory\n"); break; }

        if ((int)fread(pix, 1, (size_t)(w * h), f) != w * h) {
            fprintf(stderr, "bdd: truncated at image idx=0x%X\n", idx);
            free(pix); break;
        }

        g_img[g_ni].idx     = idx;
        g_img[g_ni].w       = w;
        g_img[g_ni].h       = h;
        g_img[g_ni].flags   = fl;
        g_img[g_ni].pal_idx = -1;
        g_img[g_ni].pix     = pix;
        g_ni++;
    }

    /* ---- palette section: <NAME> <count>\n + count*2 bytes of 15-bit RGB ----
       Palettes are numbered in file order; BDB fl field is the palette index. */
    g_n_pals = 0;
    {
        char pln[128];
        int use_leftover = (leftover[0] != '\0');
        while (g_n_pals < MAX_PALS) {
            if (use_leftover) {
                snprintf(pln, sizeof pln, "%s", leftover);
                use_leftover = 0;
            } else if (!fgets(pln, sizeof pln, f)) {
                break;
            }
            pln[strcspn(pln, "\r\n")] = '\0';
            if (!pln[0]) continue;
            char pname[64]; int cnt;
            if (sscanf(pln, "%63s %d", pname, &cnt) != 2) continue;
            if (cnt <= 0 || cnt > 256) continue;
            Uint8 buf[512];
            if ((int)fread(buf, 2, (size_t)cnt, f) != cnt) break;
            int pi = g_n_pals++;
            g_pal_count[pi] = cnt;
            for (int i = 0; i < cnt; i++) {
                Uint16 c = (Uint16)(buf[i*2] | (buf[i*2+1] << 8));
                Uint8 r = (Uint8)(((c >> 10) & 0x1F) << 3);
                Uint8 g = (Uint8)(((c >>  5) & 0x1F) << 3);
                Uint8 b = (Uint8)((c & 0x1F) << 3);
                g_pals[pi][i] = 0xFF000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
            }
            for (int i = cnt; i < 256; i++) g_pals[pi][i] = 0xFF000000u;
            fprintf(stderr, "bdd: palette[%d] = %s (%d entries)\n", pi, pname, cnt);
        }
    }

    fclose(f);
    if (g_ni == 0) { fprintf(stderr, "bdd: no images loaded from %s\n", path); return 0; }
    fprintf(stderr, "bdd: loaded %d images from %s\n", g_ni, path);
    return 1;
}

/* ------------------------------------------------------------------ */
/* BDB loader                                                           */
/* ------------------------------------------------------------------ */

static int bdb_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "bdb: cannot open %s\n", path); return 0; }

    snprintf(g_bdb_path, sizeof g_bdb_path, "%s", path);

    char ln[256];

    /* line 1: name world_w world_h max_depth num_modules num_pals num_objects */
    int num_modules = 1;
    if (fgets(ln, sizeof ln, f)) {
        /* store verbatim (strip trailing newline for clean re-emission) */
        snprintf(g_bdb_header, sizeof g_bdb_header, "%s", ln);
        g_bdb_header[strcspn(g_bdb_header, "\r\n")] = '\0';

        char nm[64]; int ww, wh, md, nm2, np, no2;
        if (sscanf(ln, "%63s %d %d %d %d %d %d",
                   nm, &ww, &wh, &md, &nm2, &np, &no2) >= 5) {
            num_modules = nm2;
            snprintf(g_name, sizeof g_name, "%s", nm);
        } else if (sscanf(ln, "%63s", nm) == 1) {
            snprintf(g_name, sizeof g_name, "%s", nm);
        }
    }

    /* store and skip module lines */
    g_bdb_num_modules = 0;
    for (int m = 0; m < num_modules && m < MAX_MODULES; m++) {
        if (!fgets(ln, sizeof ln, f)) break;
        snprintf(g_bdb_modules[g_bdb_num_modules], 256, "%s", ln);
        g_bdb_modules[g_bdb_num_modules][strcspn(g_bdb_modules[g_bdb_num_modules], "\r\n")] = '\0';
        g_bdb_num_modules++;
    }

    g_no = 0;
    while (fgets(ln, sizeof ln, f) && g_no < MAX_OBJECTS) {
        char a[16], b[16], c[16], d[16], e[16];
        if (sscanf(ln, "%15s %15s %15s %15s %15s", a, b, c, d, e) < 5) continue;
        g_obj[g_no].wx    = (int)strtol(a, NULL, 16);
        g_obj[g_no].depth = atoi(b);
        g_obj[g_no].sy    = atoi(c);
        g_obj[g_no].ii    = (int)strtol(d, NULL, 16);
        g_obj[g_no].fl    = atoi(e);
        g_obj[g_no].hfl   = (g_obj[g_no].wx & 0x10) != 0;
        g_obj[g_no].vfl   = (g_obj[g_no].wx & 0x20) != 0;
        g_obj[g_no].order = g_no;
        g_no++;
    }

    fclose(f);

    /* Sort by wx high byte (parallax layer) ascending, file order within each layer */
    if (g_no > 1) {
        /* insertion sort — stable, small N */
        for (int i = 1; i < g_no; i++) {
            Obj tmp = g_obj[i];
            int key = (tmp.wx >> 8) & 0xFF;
            int j = i - 1;
            while (j >= 0 && (((g_obj[j].wx >> 8) & 0xFF) > key ||
                               (((g_obj[j].wx >> 8) & 0xFF) == key && g_obj[j].order > tmp.order))) {
                g_obj[j + 1] = g_obj[j];
                j--;
            }
            g_obj[j + 1] = tmp;
        }
    }

    /* Assign palette index to each image from the BDB fl field */
    for (int i = 0; i < g_no; i++) {
        Img *im = img_find(g_obj[i].ii);
        if (im && im->pal_idx < 0)
            im->pal_idx = g_obj[i].fl;
    }

    fprintf(stderr, "bdb: loaded %d objects from %s\n", g_no, path);
    return g_no > 0;
}

/* BDB save                                                             */
/* ------------------------------------------------------------------ */

static int bdb_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "bdb: cannot write %s\n", path); return 0; }

    fprintf(f, "%s\n", g_bdb_header);
    for (int m = 0; m < g_bdb_num_modules; m++)
        fprintf(f, "%s\n", g_bdb_modules[m]);

    /* write objects in original file order */
    Obj *sorted[MAX_OBJECTS];
    for (int i = 0; i < g_no; i++) sorted[i] = &g_obj[i];
    /* insertion sort by order field */
    for (int i = 1; i < g_no; i++) {
        Obj *tmp = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j]->order > tmp->order) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = tmp;
    }

    for (int i = 0; i < g_no; i++) {
        Obj *o = sorted[i];
        fprintf(f, "%X %d %d %X %d\n", o->wx, o->depth, o->sy, o->ii, o->fl);
    }

    fclose(f);
    fprintf(stderr, "bdb: saved %d objects to %s\n", g_no, path);
    return 1;
}

/* Copy src → dst byte-for-byte (used for .BAK) */
static int file_copy(const char *src, const char *dst)
{
    FILE *in  = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) {
        if (in)  fclose(in);
        if (out) fclose(out);
        return 0;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 1;
}

/* Save-confirmation popup                                              */
/* ------------------------------------------------------------------ */

static void popup_free(void)
{
    if (g_popup_tex) { SDL_DestroyTexture(g_popup_tex); g_popup_tex = NULL; }
}

static void popup_build(SDL_Renderer *rend, int ww, int wh)
{
    popup_free();

    const char *line0 = "Save changes to:";
    const char *line2 = "Y = save    N = cancel";
    int namelen  = (int)strlen(g_bdb_path);
    int wide     = namelen > (int)strlen(line2) ? namelen : (int)strlen(line2);
    if (wide < (int)strlen(line0)) wide = (int)strlen(line0);

    int pad = 10;
    int lh  = 12;
    int sw  = wide * 8 + pad * 2;
    int sh  = 4 * lh  + pad * 2;  /* 4 lines */

    SDL_Surface *surf = SDL_CreateRGBSurface(0, sw, sh, 32,
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
    if (!surf) return;

    SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 20, 20, 36, 240));

    /* border */
    SDL_Rect border = { 0, 0, sw, sh };
    Uint32 bcol = SDL_MapRGBA(surf->format, 200, 200, 80, 255);
    for (int x = 0; x < sw; x++) {
        ((Uint32 *)surf->pixels)[x]                        = bcol;
        ((Uint32 *)surf->pixels)[(sh-1)*(sw) + x]         = bcol;
    }
    for (int y = 0; y < sh; y++) {
        ((Uint32 *)surf->pixels)[y * sw]                   = bcol;
        ((Uint32 *)surf->pixels)[y * sw + sw - 1]          = bcol;
    }

    Uint32 white  = SDL_MapRGBA(surf->format, 255, 255, 255, 255);
    Uint32 yellow = SDL_MapRGBA(surf->format, 240, 230, 100, 255);
    font_draw_str(surf, pad, pad,          line0,       white);
    font_draw_str(surf, pad, pad + lh,     g_bdb_path,  yellow);
    font_draw_str(surf, pad, pad + lh * 3, line2,       white);

    g_popup_tex = SDL_CreateTextureFromSurface(rend, surf);
    SDL_FreeSurface(surf);
    if (!g_popup_tex) return;
    SDL_SetTextureBlendMode(g_popup_tex, SDL_BLENDMODE_BLEND);
    g_popup_w = sw;
    g_popup_h = sh;
    (void)ww; (void)wh;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static Img *img_find(int idx)
{
    for (int i = 0; i < g_ni; i++)
        if (g_img[i].idx == idx) return &g_img[i];
    return NULL;
}

static void img_free(void)
{
    for (int i = 0; i < g_ni; i++) { free(g_img[i].pix); g_img[i].pix = NULL; }
    g_ni = 0;
}

/* Build path2 from path1 by replacing the extension.
   On non-Windows also returns uppercase variant in path2_up. */
static void make_ext(const char *src, const char *ext,
                     char *out, size_t outsz)
{
    strncpy(out, src, outsz - 1);
    out[outsz - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (!dot) dot = out + strlen(out);
    strncpy(dot, ext, outsz - (size_t)(dot - out) - 1);
}

/* Try to open a file; on Linux also try the uppercase extension variant */
static FILE *fopen_try(const char *path, const char *mode,
                       char *resolved, size_t rsz)
{
    FILE *f = fopen(path, mode);
    if (f) { snprintf(resolved, rsz, "%s", path); return f; }

#ifndef _WIN32
    /* try uppercasing the extension */
    char up[512];
    strncpy(up, path, sizeof up - 1);
    char *dot = strrchr(up, '.');
    if (dot) {
        for (char *p = dot + 1; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        f = fopen(up, mode);
        if (f) { snprintf(resolved, rsz, "%s", up); return f; }
    }
#endif
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                            */
/* ------------------------------------------------------------------ */

/* Greyscale palette-indexed image → SDL_Texture.
   Pixel value 0 is fully transparent (common "background" colour). */
static SDL_Texture *img_to_tex(SDL_Renderer *rend, const Img *im)
{
    SDL_Surface *surf = SDL_CreateRGBSurface(
        0, im->w, im->h, 32,
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
    if (!surf) return NULL;

    Uint32 *dst = (Uint32 *)surf->pixels;
    const Uint8 *src = im->pix;

    const Uint32 *pal = (im->pal_idx >= 0 && im->pal_idx < g_n_pals)
                        ? g_pals[im->pal_idx] : NULL;

    for (int i = 0; i < im->w * im->h; i++) {
        Uint8 v = src[i];
        if (v == 0) { dst[i] = 0u; continue; }
        dst[i] = pal ? pal[v] : (0xFF000000u | (Uint32)(v * 0x010101u));
    }

    SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_BLEND);
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    SDL_FreeSurface(surf);
    return tex;
}


/* ------------------------------------------------------------------ */
/* Bitmap font + tooltip                                               */
/* ------------------------------------------------------------------ */

/* 8x8 bitmap font, ASCII 32-127. Each byte is one row, MSB = left pixel. */
static const Uint8 g_font8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 32   */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* 33 ! */
    {0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00}, /* 34 " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 35 # */
    {0x0C,0x3F,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 36 $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 37 % */
    {0x1C,0x36,0x1C,0x6F,0x3B,0x33,0x6E,0x00}, /* 38 & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 39 ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 40 ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 41 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 42 * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 43 + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 44 , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 45 - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* 46 . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 47 / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 48 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 49 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 50 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 51 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 52 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 53 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 54 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 55 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 56 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 57 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* 58 : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x0C}, /* 59 ; */
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, /* 60 < */
    {0x00,0x00,0x3F,0x00,0x3F,0x00,0x00,0x00}, /* 61 = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 62 > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 63 ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 64 @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 65 A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 66 B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 67 C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 68 D */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 69 E */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 70 F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 71 G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 72 H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 73 I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 74 J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 75 K */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 76 L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 77 M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 78 N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 79 O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 80 P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 81 Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 82 R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 83 S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 84 T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 85 U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 86 V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 87 W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 88 X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 89 Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 90 Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 91 [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 92 \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 93 ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 94 ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 95 _ */
    {0x06,0x06,0x0C,0x00,0x00,0x00,0x00,0x00}, /* 96 ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 97 a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 98 b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 99 c */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* 100 d */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* 101 e */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* 102 f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* 103 g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* 104 h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* 105 i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* 106 j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* 107 k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 108 l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* 109 m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* 110 n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* 111 o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* 112 p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* 113 q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* 114 r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* 115 s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* 116 t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* 117 u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 118 v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 119 w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* 120 x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* 121 y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* 122 z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* 123 { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 124 | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* 125 } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* 126 ~ */
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, /* 127   */
};

static void font_draw_char(SDL_Surface *surf, int x, int y,
                           unsigned char c, Uint32 fg)
{
    if (c < 32 || c > 127) c = '?';
    const Uint8 *glyph = g_font8[c - 32];
    int pitch = surf->pitch / 4;
    Uint32 *pixels = (Uint32 *)surf->pixels;
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (glyph[row] & (1u << col))
                pixels[(y + row) * pitch + (x + col)] = fg;
}

static void font_draw_str(SDL_Surface *surf, int x, int y,
                          const char *s, Uint32 fg)
{
    while (*s) { font_draw_char(surf, x, y, (unsigned char)*s++, fg); x += 8; }
}

#define TIP_PAD 5
#define TIP_LH  11   /* line height: 8px glyph + 3px gap */
#define TIP_MAX 32
#define TIP_COL 56

static void tooltip_free(void)
{
    if (g_tip_tex) { SDL_DestroyTexture(g_tip_tex); g_tip_tex = NULL; }
}

static void tooltip_build(SDL_Renderer *rend,
                           int mx, int my,
                           int view_x, int view_y, int zoom,
                           int ww, int wh)
{
    tooltip_free();

    int wx = mx / zoom + view_x;
    int wy = my / zoom + view_y;

    char lines[TIP_MAX][TIP_COL];
    int  nl = 0;

    /* top-most object first (reverse paint order) */
    for (int i = g_no - 1; i >= 0 && nl + 1 < TIP_MAX; i--) {
        Obj *o  = &g_obj[i];
        Img *im = img_find(o->ii);
        if (!im) continue;
        if (wx < o->depth || wx >= o->depth + im->w) continue;
        if (wy < o->sy    || wy >= o->sy    + im->h) continue;
        int pal = (im->pal_idx >= 0) ? im->pal_idx : o->fl;
        snprintf(lines[nl++], TIP_COL,
            "[%d] ii=0x%04X  %dx%d  pal=%d", i, o->ii, im->w, im->h, pal);
        snprintf(lines[nl++], TIP_COL,
            "  Z=%-4d sy=%-4d  wx=0x%04X  hfl=%d vfl=%d",
            o->depth, o->sy, o->wx, o->hfl, o->vfl);
    }

    if (nl == 0) return;

    /* measure width */
    int max_chars = 0;
    for (int i = 0; i < nl; i++) {
        int l = (int)strlen(lines[i]);
        if (l > max_chars) max_chars = l;
    }
    int sw = max_chars * 8 + TIP_PAD * 2;
    int sh = nl * TIP_LH  + TIP_PAD * 2;

    SDL_Surface *surf = SDL_CreateRGBSurface(0, sw, sh, 32,
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
    if (!surf) return;

    /* background */
    SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 14, 14, 24, 225));

    /* border */
    {
        Uint32 bc = SDL_MapRGBA(surf->format, 110, 110, 170, 255);
        int pitch = surf->pitch / 4;
        Uint32 *px = (Uint32 *)surf->pixels;
        for (int x = 0; x < sw; x++) { px[x] = bc; px[(sh-1)*pitch+x] = bc; }
        for (int y = 0; y < sh; y++) { px[y*pitch] = bc; px[y*pitch+sw-1] = bc; }
    }

    /* text — alternate warm/cool colours per object (pairs of lines) */
    for (int i = 0; i < nl; i++) {
        Uint32 fg = ((i / 2) % 2 == 0)
            ? SDL_MapRGBA(surf->format, 240, 230, 150, 255)  /* warm yellow */
            : SDL_MapRGBA(surf->format, 150, 220, 255, 255); /* cool blue   */
        font_draw_str(surf, TIP_PAD, TIP_PAD + i * TIP_LH, lines[i], fg);
    }

    SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_BLEND);
    g_tip_tex = SDL_CreateTextureFromSurface(rend, surf);
    SDL_FreeSurface(surf);
    if (!g_tip_tex) return;
    SDL_SetTextureBlendMode(g_tip_tex, SDL_BLENDMODE_BLEND);

    g_tip_w = sw;
    g_tip_h = sh;
    g_tip_x = mx + 14;
    g_tip_y = my + 14;
    if (g_tip_x + sw > ww) g_tip_x = mx - sw - 4;
    if (g_tip_y + sh > wh) g_tip_y = my - sh - 4;
}

/* ------------------------------------------------------------------ */
/* Image-grid view (BDD only, no BDB)                                  */
/* ------------------------------------------------------------------ */

static void draw_grid_view(SDL_Renderer *rend, SDL_Texture **tex,
                           int win_w, 
                           int scroll_x, int scroll_y, int zoom)
{
    int pad = 8 * zoom;
    int cx = -scroll_x, cy = -scroll_y;
    int row_h = 0;

    for (int i = 0; i < g_ni; i++) {
        if (!tex[i]) continue;
        Img *im = &g_img[i];
        int tw = im->w * zoom, th = im->h * zoom;

        if (cx + tw + pad > win_w) { cx = -scroll_x; cy += row_h + pad; row_h = 0; }
        if (row_h < th) row_h = th;

        SDL_Rect dst = { cx, cy, tw, th };
        SDL_RenderCopy(rend, tex[i], NULL, &dst);

        /* border */
        SDL_SetRenderDrawColor(rend, 80, 80, 100, 255);
        SDL_RenderDrawRect(rend, &dst);

        cx += tw + pad;
    }
}

/* ------------------------------------------------------------------ */
/* World view (BDB + BDD)                                              */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

#define WIN_W 1280
#define WIN_H  720

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: bddview <file.BDB>\n");
        fprintf(stderr, "       bddview <file.BDD>\n");
        return 1;
    }

    const char *arg = argv[1];
    char bdb_path[512] = "", bdd_path[512] = "";

    /* Determine which file was given, derive paths for both */
    size_t alen = strlen(arg);
    const char *ext = (alen >= 4) ? (arg + alen - 4) : "";

    if (strcasecmp(ext, ".bdb") == 0) {
        make_ext(arg, ".bdb", bdb_path, sizeof bdb_path);
        make_ext(arg, ".bdd", bdd_path, sizeof bdd_path);
    } else if (strcasecmp(ext, ".bdd") == 0) {
        make_ext(arg, ".bdd", bdd_path, sizeof bdd_path);
        make_ext(arg, ".bdb", bdb_path, sizeof bdb_path);
    } else {
        fprintf(stderr, "Unknown extension: expected .BDB or .BDD\n");
        return 1;
    }

    /* Load BDD (required) */
    {
        char resolved[512] = "";
        FILE *tf = fopen_try(bdd_path, "rb", resolved, sizeof resolved);
        if (!tf) { fprintf(stderr, "Cannot find BDD file: %s\n", bdd_path); return 1; }
        fclose(tf);
        snprintf(bdd_path, sizeof bdd_path, "%s", resolved);
    }
    if (!bdd_load(bdd_path)) return 1;

    /* Load BDB (optional) */
    {
        char resolved[512] = "";
        FILE *tf = fopen_try(bdb_path, "r", resolved, sizeof resolved);
        if (tf) {
            fclose(tf);
            snprintf(bdb_path, sizeof bdb_path, "%s", resolved);
            g_have_bdb = bdb_load(bdb_path);
        } else {
            fprintf(stderr, "BDB not found — showing image grid\n");
        }
    }

    /* Draw order: BDB file order (as authored) */

    /* World bounds — X axis = depth, Y axis = sy */
    int wx_min = INT_MAX, wx_max = INT_MIN;
    int wy_min = INT_MAX, wy_max = INT_MIN;
    if (g_have_bdb) {
        for (int i = 0; i < g_no; i++) {
            Obj *o = &g_obj[i];
            Img *im = img_find(o->ii);
            if (!im) continue;
            int iw = im->w, ih = im->h;
            if (o->depth        < wx_min) wx_min = o->depth;
            if (o->depth + iw   > wx_max) wx_max = o->depth + iw;
            if (o->sy           < wy_min) wy_min = o->sy;
            if (o->sy + ih      > wy_max) wy_max = o->sy + ih;
        }
    }
    if (wx_min == INT_MAX) { wx_min = 0; wx_max = WIN_W; wy_min = 0; wy_max = WIN_H; }

    /* SDL init */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    char title[256];
    snprintf(title, sizeof title, "BDD Viewer — %s",
             g_name[0] ? g_name : arg);

    SDL_Window *win = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    SDL_Renderer *rend = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rend)
        rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!rend) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }

    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);

    /* Build one texture per image */
    SDL_Texture **textures = (SDL_Texture **)calloc(
        (size_t)g_ni, sizeof(SDL_Texture *));
    if (!textures) { fprintf(stderr, "out of memory\n"); return 1; }

    for (int i = 0; i < g_ni; i++)
        textures[i] = img_to_tex(rend, &g_img[i]);

    /* View state */
    int view_x     = wx_min;   /* world X at left edge */
    int view_y     = wy_min;   /* world Y at top edge  */
    int zoom       = 1;
    int dragging   = 0;
    int drag_ox, drag_oy, drag_vx, drag_vy;

    /* Toggle flags */
    int show_grid     = 1;  /* Shift+T */
    int show_borders  = 1;  /* Shift+B */
    int show_objects  = 1;  /* Shift+O */

    /* Object drag state (Ctrl+LMB) */
    int obj_drag_idx   = -1; /* index into g_obj, -1 = none */
    int obj_drag_ox    = 0, obj_drag_oy    = 0; /* mouse origin (screen px) */
    int obj_drag_depth0= 0, obj_drag_sy0   = 0; /* object origin (world) */

    /* Software key-repeat state */
#define KR_DELAY_MS  400
#define KR_RATE_MS    50
    SDL_Keycode kr_sym  = SDLK_UNKNOWN;
    Uint32      kr_next = 0;

    /* Title caching — only update when string changes */
    char cur_title[256] = "";

    /* Hover state for debug info */
#define HOVER_DELAY_MS 1200
    int    hover_x = -1, hover_y = -1;   /* last mouse pos */
    Uint32 hover_since = 0;               /* ticks when mouse settled */
    int    hover_printed = 0;             /* already printed for this hover */

    int running = 1;
    SDL_Event ev;

    while (running) {
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0; break;

            case SDL_DROPFILE:
                /* Re-load if user drops a new file */
                img_free();
                g_no = 0; g_have_bdb = 0; g_name[0] = '\0';
                for (int i = 0; i < g_ni; i++)
                    if (textures[i]) { SDL_DestroyTexture(textures[i]); textures[i] = NULL; }

                make_ext(ev.drop.file, ".bdd", bdd_path, sizeof bdd_path);
                make_ext(ev.drop.file, ".bdb", bdb_path, sizeof bdb_path);
                {
                    char res[512] = "";
                    FILE *tf = fopen_try(bdd_path, "rb", res, sizeof res);
                    if (!tf) tf = fopen_try(ev.drop.file, "rb", res, sizeof res);
                    if (tf) { fclose(tf); snprintf(bdd_path, sizeof bdd_path, "%s", res); bdd_load(bdd_path); }
                    tf = fopen_try(bdb_path, "r", res, sizeof res);
                    if (tf) { fclose(tf); snprintf(bdb_path, sizeof bdb_path, "%s", res); g_have_bdb = bdb_load(bdb_path); }
                }
                /* preserve BDB file order */
                wx_min = INT_MAX; wx_max = INT_MIN;
                wy_min = INT_MAX; wy_max = INT_MIN;
                for (int i = 0; i < g_no; i++) {
                    Obj *o = &g_obj[i];
                    Img *im = img_find(o->ii);
                    if (!im) continue;
                    if (o->depth      < wx_min) wx_min = o->depth;
                    if (o->depth+im->w > wx_max) wx_max = o->depth+im->w;
                    if (o->sy         < wy_min) wy_min = o->sy;
                    if (o->sy+im->h   > wy_max) wy_max = o->sy+im->h;
                }
                if (wx_min == INT_MAX) { wx_min = 0; wx_max = WIN_W; wy_min = 0; wy_max = WIN_H; }
                textures = (SDL_Texture **)realloc(textures, (size_t)g_ni * sizeof(SDL_Texture *));
                for (int i = 0; i < g_ni; i++)
                    textures[i] = img_to_tex(rend, &g_img[i]);
                SDL_free(ev.drop.file);
                view_x = wx_min; view_y = wy_min;
                break;

            case SDL_KEYDOWN:
                if (!ev.key.repeat) {
                    kr_sym  = ev.key.keysym.sym;
                    kr_next = SDL_GetTicks() + KR_DELAY_MS;
                }
                switch (ev.key.keysym.sym) {
                case SDLK_LEFT:    view_x -= 64 / zoom; break;
                case SDLK_RIGHT:   view_x += 64 / zoom; break;
                case SDLK_UP:      view_y -= 32 / zoom; break;
                case SDLK_DOWN:    view_y += 32 / zoom; break;
                case SDLK_EQUALS:
                case SDLK_PLUS:
                case SDLK_KP_PLUS:  if (zoom < 8) zoom++; break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS: if (zoom > 1) zoom--; break;
                case SDLK_HOME:    view_x = wx_min; view_y = wy_min; zoom = 1; break;
                case SDLK_s:
                    if ((ev.key.keysym.mod & KMOD_CTRL) && g_have_bdb && !g_confirm_save) {
                        g_confirm_save = 1;
                        popup_build(rend, ww, wh);
                    }
                    break;
                case SDLK_y:
                    if (g_confirm_save) {
                        /* backup then save */
                        char bak[520];
                        snprintf(bak, sizeof bak, "%s.BAK", g_bdb_path);
                        if (!file_copy(g_bdb_path, bak))
                            fprintf(stderr, "bdb: warning — could not create %s\n", bak);
                        else
                            fprintf(stderr, "bdb: backup written to %s\n", bak);
                        bdb_save(g_bdb_path);
                        g_confirm_save = 0;
                        popup_free();
                    }
                    break;
                case SDLK_n:
                    if (g_confirm_save) {
                        g_confirm_save = 0;
                        popup_free();
                    }
                    break;
                case SDLK_ESCAPE:
                    if (g_confirm_save) {
                        g_confirm_save = 0;
                        popup_free();
                    } else {
                        running = 0;
                    }
                    break;
                case SDLK_t:
                    if (ev.key.keysym.mod & KMOD_SHIFT) show_grid    ^= 1;
                    break;
                case SDLK_b:
                    if (ev.key.keysym.mod & KMOD_SHIFT) show_borders ^= 1;
                    break;
                case SDLK_o:
                    if (ev.key.keysym.mod & KMOD_SHIFT) show_objects ^= 1;
                    break;
                }
                break;

            case SDL_KEYUP:
                if (ev.key.keysym.sym == kr_sym)
                    kr_sym = SDLK_UNKNOWN;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (ev.button.clicks >= 1 && (SDL_GetModState() & KMOD_CTRL) && g_have_bdb) {
                        /* Ctrl+LMB — pick topmost object under cursor */
                        int wx2 = ev.button.x / zoom + view_x;
                        int wy2 = ev.button.y / zoom + view_y;
                        obj_drag_idx = -1;
                        for (int i = g_no - 1; i >= 0; i--) {
                            Obj *o  = &g_obj[i];
                            Img *im = img_find(o->ii);
                            if (!im) continue;
                            if (wx2 < o->depth || wx2 >= o->depth + im->w) continue;
                            if (wy2 < o->sy    || wy2 >= o->sy    + im->h) continue;
                            obj_drag_idx    = i;
                            obj_drag_ox     = ev.button.x;
                            obj_drag_oy     = ev.button.y;
                            obj_drag_depth0 = o->depth;
                            obj_drag_sy0    = o->sy;
                            tooltip_free();
                            break;
                        }
                    } else {
                        dragging = 1;
                        drag_ox = ev.button.x; drag_oy = ev.button.y;
                        drag_vx = view_x;      drag_vy = view_y;
                    }
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (obj_drag_idx >= 0) {
                        Obj *o = &g_obj[obj_drag_idx];
                        fprintf(stderr, "obj[%d] moved to depth=%d sy=%d\n",
                                obj_drag_idx, o->depth, o->sy);
                        obj_drag_idx = -1;
                    }
                    dragging = 0;
                }
                break;

            case SDL_MOUSEMOTION:
                if (obj_drag_idx >= 0) {
                    Obj *o  = &g_obj[obj_drag_idx];
                    o->depth = obj_drag_depth0 + (ev.motion.x - obj_drag_ox) / zoom;
                    o->sy    = obj_drag_sy0    + (ev.motion.y - obj_drag_oy) / zoom;
                } else if (dragging) {
                    view_x = drag_vx - (ev.motion.x - drag_ox) / zoom;
                    view_y = drag_vy - (ev.motion.y - drag_oy) / zoom;
                }
                if (ev.motion.x != hover_x || ev.motion.y != hover_y) {
                    hover_x = ev.motion.x;
                    hover_y = ev.motion.y;
                    hover_since = SDL_GetTicks();
                    hover_printed = 0;
                    tooltip_free();
                }
                break;

            case SDL_MOUSEWHEEL:
                if (ev.wheel.y > 0 && zoom < 8) zoom++;
                if (ev.wheel.y < 0 && zoom > 1) zoom--;
                break;
            }
        }

        /* Software key repeat */
        if (kr_sym != SDLK_UNKNOWN) {
            Uint32 now = SDL_GetTicks();
            if (now >= kr_next) {
                kr_next = now + KR_RATE_MS;
                switch (kr_sym) {
                case SDLK_LEFT:  view_x -= 64 / zoom; break;
                case SDLK_RIGHT: view_x += 64 / zoom; break;
                case SDLK_UP:    view_y -= 32 / zoom; break;
                case SDLK_DOWN:  view_y += 32 / zoom; break;
                default: break;
                }
            }
        }

        /* Hover tooltip */
        if (g_have_bdb && hover_x >= 0 && !hover_printed &&
            SDL_GetTicks() - hover_since >= HOVER_DELAY_MS)
        {
            hover_printed = 1;
            tooltip_build(rend, hover_x, hover_y, view_x, view_y, zoom, ww, wh);
        }

        /* Update window title only when state changes */
        {
            char t[256];
            snprintf(t, sizeof t,
                "BDD Viewer — %s   [%dx zoom]  pos (%d,%d)  objects=%d  images=%d",
                g_name[0] ? g_name : "?",
                zoom, view_x, view_y, g_no, g_ni);
            if (strcmp(t, cur_title) != 0) {
                SDL_SetWindowTitle(win, t);
                snprintf(cur_title, sizeof cur_title, "%s", t);
            }
        }

        SDL_SetRenderDrawColor(rend, 18, 18, 28, 255);
        SDL_RenderClear(rend);

        if (!g_have_bdb) {
            /* ---- image grid view ---- */
            draw_grid_view(rend, textures, ww, view_x, view_y, zoom);
        } else {
            /* ---- world layout view ---- */

            /* grid */
            if (show_grid) {
                SDL_SetRenderDrawColor(rend, 32, 32, 48, 255);
                for (int gx = (view_x / 64) * 64; gx < view_x + ww / zoom + 64; gx += 64) {
                    int sx = (gx - view_x) * zoom;
                    SDL_RenderDrawLine(rend, sx, 0, sx, wh);
                }
                for (int gy = (view_y / 32) * 32; gy < view_y + wh / zoom + 32; gy += 32) {
                    int sy = (gy - view_y) * zoom;
                    SDL_RenderDrawLine(rend, 0, sy, ww, sy);
                }
            }

            /* objects — sorted by wx layer (far first), file order within layer */
            if (show_objects) {
                for (int i = 0; i < g_no; i++) {
                    Obj *o = &g_obj[i];

                    Img *im = img_find(o->ii);
                    if (!im) continue;

                    int ti = (int)(im - g_img);
                    if (!textures[ti]) continue;

                    int sx = (o->depth - view_x) * zoom;
                    int sy = (o->sy - view_y) * zoom;
                    int tw = im->w * zoom;
                    int th = im->h * zoom;

                    /* cull offscreen */
                    if (sx + tw < 0 || sx > ww) continue;
                    if (sy + th < 0 || sy > wh) continue;

                    SDL_Rect dst = { sx, sy, tw, th };
                    SDL_RendererFlip flip = SDL_FLIP_NONE;
                    if (o->hfl) flip |= SDL_FLIP_HORIZONTAL;
                    if (o->vfl) flip |= SDL_FLIP_VERTICAL;
                    SDL_RenderCopyEx(rend, textures[ti], NULL, &dst,
                                     0.0, NULL, flip);

                    /* thin highlight border */
                    if (show_borders) {
                        SDL_SetRenderDrawColor(rend, 60, 60, 90, 180);
                        SDL_RenderDrawRect(rend, &dst);
                    }
                }
            }
        }

        /* Tooltip overlay */
        if (g_tip_tex) {
            SDL_Rect dst = { g_tip_x, g_tip_y, g_tip_w, g_tip_h };
            SDL_RenderCopy(rend, g_tip_tex, NULL, &dst);
        }

        /* Save-confirmation popup (centered) */
        if (g_confirm_save && g_popup_tex) {
            SDL_Rect dst = { (ww - g_popup_w) / 2, (wh - g_popup_h) / 2,
                             g_popup_w, g_popup_h };
            SDL_RenderCopy(rend, g_popup_tex, NULL, &dst);
        }

        SDL_RenderPresent(rend);
    }

    /* Cleanup */
    popup_free();
    tooltip_free();
    for (int i = 0; i < g_ni; i++)
        if (textures[i]) SDL_DestroyTexture(textures[i]);
    free(textures);
    img_free();
    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
