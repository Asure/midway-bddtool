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
#  include <commdlg.h>
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
static char   g_pal_name[MAX_PALS][64]; /* palette names from BDD file */

/* BDD file path (needed for rewrite on TGA import) */
static char g_bdd_path[512] = "";

/* TGA path-input overlay (Ctrl+L) */
static int  g_path_input_open = 0;
static char g_path_input_buf[512] = "";
static int  g_path_input_len = 0;

/* Tooltip texture (built on hover, freed on mouse move) */
static SDL_Texture *g_tip_tex = NULL;
static int g_tip_x, g_tip_y, g_tip_w, g_tip_h;

/* Save-confirmation popup */
static SDL_Texture *g_popup_tex = NULL;
static int g_popup_w, g_popup_h;
static int g_confirm_save = 0; /* 1 = popup visible, waiting for Y/N */

/* Object picker (Tab key) */
#define PICKER_W      170  /* panel width on right edge             */
#define PICKER_ITEM_H  70  /* row height per image entry            */
#define PICKER_THUMB   54  /* max thumbnail size (px)               */

static int           g_picker_open  = 0;
static int           g_picker_scroll= 0;
static int           g_place_img    = -1; /* g_img[] index pending placement */
static SDL_Texture  *g_pick_label[MAX_IMAGES];
static int           g_pick_labels_built = 0;
static int           g_last_obj = -1;   /* g_obj[] index of last dragged/placed object */

/* forward declarations */
static Img *img_find(int idx);
static void font_draw_str(SDL_Surface *surf, int x, int y, const char *s, Uint32 fg);
static void tooltip_build_obj(SDL_Renderer *rend, int oi, int ax, int ay, int ww, int wh);
static int  bdd_import_tga(const char *tga_path);
static int  bdd_save(void);
static int  file_copy(const char *src, const char *dst);

/* ------------------------------------------------------------------ */
/* BDD loader                                                           */
/* ------------------------------------------------------------------ */

static int bdd_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "bdd: cannot open %s\n", path); return 0; }
    snprintf(g_bdd_path, sizeof g_bdd_path, "%s", path);

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
            snprintf(g_pal_name[pi], 64, "%s", pname);
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

/* System file-open dialog                                              */
/* ------------------------------------------------------------------ */

/* Opens a native file-chooser for TGA files.
   Returns 1 and fills out[0..outsz] on success, 0 on cancel/error.
   Falls back to the text-input overlay if no dialog tool is found.    */
static int open_file_dialog(char *out, int outsz)
{
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    out[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "TGA Files\0*.TGA;*.tga\0All Files\0*.*\0";
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)outsz;
    ofn.lpstrTitle  = "Load TGA";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
#else
    /* Try zenity, then kdialog; return 0 to trigger text-input fallback */
    const char *cmds[] = {
        "zenity --file-selection --title='Load TGA' "
            "--file-filter='TGA files (*.tga *.TGA) | *.tga *.TGA' 2>/dev/null",
        "kdialog --getopenfilename . '*.tga *.TGA' 2>/dev/null",
        NULL
    };
    for (int i = 0; cmds[i]; i++) {
        FILE *p = popen(cmds[i], "r");
        if (!p) continue;
        char buf[512] = "";
        fgets(buf, sizeof buf, p);
        pclose(p);
        buf[strcspn(buf, "\r\n")] = '\0';
        if (buf[0]) { snprintf(out, outsz, "%s", buf); return 1; }
    }
    return 0; /* no dialog tool — caller shows text input */
#endif
}

/* BDD save — rewrites the BDD file from current g_img[] and g_pals[]  */
/* ------------------------------------------------------------------ */

static int bdd_save(void)
{
    if (!g_bdd_path[0]) { fprintf(stderr, "bdd: no path to save\n"); return 0; }

    FILE *f = fopen(g_bdd_path, "wb");
    if (!f) { fprintf(stderr, "bdd: cannot write %s\n", g_bdd_path); return 0; }

    fprintf(f, "%d\n", g_ni);
    for (int i = 0; i < g_ni; i++) {
        Img *im = &g_img[i];
        fprintf(f, "%X %d %d %d\n", im->idx, im->w, im->h, im->flags);
        fwrite(im->pix, 1, (size_t)(im->w * im->h), f);
    }
    for (int i = 0; i < g_n_pals; i++) {
        fprintf(f, "%s %d\n", g_pal_name[i], g_pal_count[i]);
        for (int j = 0; j < g_pal_count[i]; j++) {
            Uint32 c = g_pals[i][j];
            Uint16 v;
            if (j == 0) {
                v = 0; /* transparent */
            } else {
                Uint8 r  = (c >> 16) & 0xFF;
                Uint8 g2 = (c >>  8) & 0xFF;
                Uint8 b  =  c        & 0xFF;
                v = (Uint16)(((r >> 3) << 10) | ((g2 >> 3) << 5) | (b >> 3));
            }
            fwrite(&v, 2, 1, f);
        }
    }
    fclose(f);
    fprintf(stderr, "bdd: saved %d images, %d palettes to %s\n",
            g_ni, g_n_pals, g_bdd_path);
    return 1;
}

/* TGA loader (type 1 — 8-bit paletted with BGR555 colour map)         */
/* ------------------------------------------------------------------ */

static int tga_load(const char *path, int *out_w, int *out_h,
                    Uint8 **out_pix, Uint16 **out_pal, int *out_pal_cnt)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "tga: cannot open %s\n", path); return 0; }

    Uint8 hdr[18];
    if (fread(hdr, 1, 18, f) < 18) { fclose(f); return 0; }

    int id_len     =  hdr[0];
    int cmap_type  =  hdr[1];
    int img_type   =  hdr[2];
    int cmap_count = (hdr[6] << 8) | hdr[5];
    int cmap_depth =  hdr[7];
    int width      = (hdr[13] << 8) | hdr[12];
    int height     = (hdr[15] << 8) | hdr[14];
    int pix_depth  =  hdr[16];
    int img_desc   =  hdr[17];

    if (img_type != 1 || pix_depth != 8 || cmap_type != 1) {
        fprintf(stderr, "tga: only 8-bit paletted TGA supported (got type=%d depth=%d)\n",
                img_type, pix_depth);
        fclose(f); return 0;
    }

    fseek(f, id_len, SEEK_CUR);

    int bpp = (cmap_depth + 7) / 8;
    Uint16 *pal = (Uint16 *)malloc((size_t)cmap_count * sizeof(Uint16));
    if (!pal) { fclose(f); return 0; }
    for (int i = 0; i < cmap_count; i++) {
        Uint8 buf[4] = {0};
        if (fread(buf, 1, (size_t)bpp, f) != (size_t)bpp) { free(pal); fclose(f); return 0; }
        pal[i] = (Uint16)(buf[0] | (buf[1] << 8));
    }

    int npix = width * height;
    Uint8 *raw = (Uint8 *)malloc((size_t)npix);
    Uint8 *pix = (Uint8 *)malloc((size_t)npix);
    if (!raw || !pix) { free(raw); free(pix); free(pal); fclose(f); return 0; }
    if ((int)fread(raw, 1, (size_t)npix, f) < npix) {
        free(raw); free(pix); free(pal); fclose(f); return 0;
    }
    fclose(f);

    /* flip vertically if origin is bottom-left (img_desc bit 5 = 0) */
    if (img_desc & 0x20) {
        memcpy(pix, raw, (size_t)npix);
    } else {
        for (int row = 0; row < height; row++)
            memcpy(pix + row * width, raw + (height - 1 - row) * width, (size_t)width);
    }
    free(raw);

    *out_w = width; *out_h = height;
    *out_pix = pix; *out_pal = pal; *out_pal_cnt = cmap_count;
    return 1;
}

/* Import a TGA into g_img[] / g_pals[] and rewrite the BDD           */
/* ------------------------------------------------------------------ */

static int bdd_import_tga(const char *tga_path)
{
    if (!g_bdd_path[0]) { fprintf(stderr, "tga: load a BDD first\n"); return 0; }
    if (g_ni  >= MAX_IMAGES) { fprintf(stderr, "tga: MAX_IMAGES reached\n"); return 0; }
    if (g_n_pals >= MAX_PALS) { fprintf(stderr, "tga: MAX_PALS reached\n"); return 0; }

    int w, h, pal_cnt;
    Uint8  *pix = NULL;
    Uint16 *pal = NULL;
    if (!tga_load(tga_path, &w, &h, &pix, &pal, &pal_cnt)) return 0;

    /* new image idx = max existing + 1 */
    int max_idx = 0;
    for (int i = 0; i < g_ni; i++)
        if (g_img[i].idx > max_idx) max_idx = g_img[i].idx;
    int new_idx = max_idx + 1;

    /* add palette */
    int pi = g_n_pals++;
    g_pal_count[pi] = pal_cnt;

    /* derive palette name from filename (no path, no extension, uppercase) */
    const char *base = tga_path;
    for (const char *s = tga_path; *s; s++)
        if (*s == '/' || *s == '\\') base = s + 1;
    snprintf(g_pal_name[pi], 64, "%s", base);
    char *dot = strrchr(g_pal_name[pi], '.');
    if (dot) *dot = '\0';
    for (char *p = g_pal_name[pi]; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);

    for (int i = 0; i < pal_cnt; i++) {
        Uint16 c = pal[i];
        if (i == 0) {
            g_pals[pi][i] = 0;
        } else {
            Uint8 r  = (Uint8)(((c >> 10) & 0x1F) << 3);
            Uint8 g2 = (Uint8)(((c >>  5) & 0x1F) << 3);
            Uint8 b  = (Uint8)( (c        & 0x1F) << 3);
            g_pals[pi][i] = 0xFF000000u | ((Uint32)r << 16) | ((Uint32)g2 << 8) | b;
        }
    }
    for (int i = pal_cnt; i < 256; i++) g_pals[pi][i] = 0xFF000000u;
    free(pal);

    /* add image */
    Img *im    = &g_img[g_ni++];
    im->idx    = new_idx;
    im->w      = w;
    im->h      = h;
    im->flags  = 0;
    im->pal_idx = pi;
    im->pix    = pix;

    /* backup and rewrite BDD */
    char bak[520];
    snprintf(bak, sizeof bak, "%s.BAK", g_bdd_path);
    file_copy(g_bdd_path, bak);
    bdd_save();

    g_pick_labels_built = 0;

    fprintf(stderr, "tga: imported %s as idx=0x%02X  pal=%d  %dx%d\n",
            tga_path, new_idx, pi, w, h);
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
/* Path-input overlay (Ctrl+L)                                          */
/* ------------------------------------------------------------------ */

static void draw_path_input(SDL_Renderer *rend, int ww, int wh)
{
    int bw = 480, bh = 52;
    int bx = (ww - bw) / 2, by = (wh - bh) / 2;

    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = { bx, by, bw, bh };
    SDL_SetRenderDrawColor(rend, 14, 14, 26, 245);
    SDL_RenderFillRect(rend, &bg);
    SDL_SetRenderDrawColor(rend, 180, 180, 80, 255);
    SDL_RenderDrawRect(rend, &bg);

    /* label */
    SDL_Surface *surf = SDL_CreateRGBSurface(0, bw - 8, 38, 32,
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
    if (!surf) return;
    SDL_FillRect(surf, NULL, 0);
    Uint32 lc = SDL_MapRGBA(surf->format, 180, 180, 100, 255);
    Uint32 tc = SDL_MapRGBA(surf->format, 220, 220, 255, 255);
    font_draw_str(surf, 0, 0,  "Load TGA — enter path and press Enter:", lc);
    /* draw input text with blinking cursor */
    char display[520];
    snprintf(display, sizeof display, "%s", g_path_input_buf);
    if ((SDL_GetTicks() / 500) % 2 == 0) {
        int len = (int)strlen(display);
        if (len < (int)sizeof(display) - 1) { display[len] = '_'; display[len+1] = '\0'; }
    }
    font_draw_str(surf, 0, 14, display, tc);

    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_Rect dst = { bx + 4, by + 7, bw - 8, 38 };
    SDL_RenderCopy(rend, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

/* Object picker functions                                              */
/* ------------------------------------------------------------------ */

static void picker_free_labels(void)
{
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (g_pick_label[i]) { SDL_DestroyTexture(g_pick_label[i]); g_pick_label[i] = NULL; }
    }
    g_pick_labels_built = 0;
}

static void picker_build_labels(SDL_Renderer *rend)
{
    for (int i = 0; i < g_ni; i++) {
        if (g_pick_label[i]) continue;
        Img *im = &g_img[i];
        char l1[24], l2[20];
        snprintf(l1, sizeof l1, "ii=0x%02X", im->idx);
        snprintf(l2, sizeof l2, "%d x %d",   im->w, im->h);
        int sw = (int)(strlen(l1) > strlen(l2) ? strlen(l1) : strlen(l2)) * 8;
        SDL_Surface *s = SDL_CreateRGBSurface(0, sw, 19, 32,
            0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
        if (!s) continue;
        SDL_FillRect(s, NULL, 0);
        font_draw_str(s, 0,  0, l1, SDL_MapRGBA(s->format, 210, 220, 255, 255));
        font_draw_str(s, 0, 11, l2, SDL_MapRGBA(s->format, 150, 200, 150, 255));
        g_pick_label[i] = SDL_CreateTextureFromSurface(rend, s);
        SDL_FreeSurface(s);
        if (g_pick_label[i])
            SDL_SetTextureBlendMode(g_pick_label[i], SDL_BLENDMODE_BLEND);
    }
    g_pick_labels_built = 1;
}

static void draw_picker(SDL_Renderer *rend, SDL_Texture **textures,
                        int ww, int wh, int mx, int my)
{
    if (!g_pick_labels_built) picker_build_labels(rend);

    int px  = ww - PICKER_W;
    int pad = 6;

    /* background */
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    SDL_Rect bg = { px, 0, PICKER_W, wh };
    SDL_SetRenderDrawColor(rend, 14, 14, 26, 242);
    SDL_RenderFillRect(rend, &bg);
    SDL_SetRenderDrawColor(rend, 80, 80, 150, 255);
    SDL_RenderDrawLine(rend, px, 0, px, wh);

    for (int i = g_picker_scroll; i < g_ni; i++) {
        int iy = (i - g_picker_scroll) * PICKER_ITEM_H;
        if (iy >= wh) break;

        Img *im = &g_img[i];

        /* hover highlight */
        if (mx >= px && my >= iy && my < iy + PICKER_ITEM_H) {
            SDL_Rect hi = { px + 1, iy, PICKER_W - 1, PICKER_ITEM_H - 1 };
            SDL_SetRenderDrawColor(rend, 55, 55, 110, 200);
            SDL_RenderFillRect(rend, &hi);
        }

        /* thumbnail — scale to fit PICKER_THUMB box */
        if (textures[i]) {
            float sc = (float)PICKER_THUMB / (float)(im->w > im->h ? im->w : im->h);
            if (sc > 1.0f) sc = 1.0f;
            int tw = (int)(im->w * sc), th = (int)(im->h * sc);
            SDL_Rect dst = {
                px + pad + (PICKER_THUMB - tw) / 2,
                iy + (PICKER_ITEM_H - th) / 2,
                tw, th
            };
            SDL_RenderCopy(rend, textures[i], NULL, &dst);
        }

        /* label */
        if (g_pick_label[i]) {
            int lx = px + pad + PICKER_THUMB + 6;
            SDL_Rect ldst = { lx, iy + (PICKER_ITEM_H - 19) / 2,
                              PICKER_W - (lx - px) - 4, 19 };
            SDL_RenderCopy(rend, g_pick_label[i], NULL, &ldst);
        }

        /* divider */
        SDL_SetRenderDrawColor(rend, 35, 35, 55, 255);
        SDL_RenderDrawLine(rend, px, iy + PICKER_ITEM_H - 1,
                           px + PICKER_W, iy + PICKER_ITEM_H - 1);
    }

    /* scroll arrows */
    SDL_SetRenderDrawColor(rend, 140, 140, 210, 255);
    if (g_picker_scroll > 0) {
        int ax = px + PICKER_W - 10;
        SDL_RenderDrawLine(rend, ax - 5, 14, ax, 6);
        SDL_RenderDrawLine(rend, ax,     6,  ax + 5, 14);
    }
    if (g_picker_scroll + wh / PICKER_ITEM_H < g_ni - 1) {
        int ax = px + PICKER_W - 10;
        SDL_RenderDrawLine(rend, ax - 5, wh - 14, ax, wh - 6);
        SDL_RenderDrawLine(rend, ax,     wh - 6,  ax + 5, wh - 14);
    }
}

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

/* Build tooltip for a single known object — used for real-time WX feedback */
static void tooltip_build_obj(SDL_Renderer *rend, int oi,
                               int ax, int ay, int ww, int wh)
{
    tooltip_free();
    if (oi < 0 || oi >= g_no) return;

    Obj *o  = &g_obj[oi];
    Img *im = img_find(o->ii);
    int pal = im ? ((im->pal_idx >= 0) ? im->pal_idx : o->fl) : o->fl;

    char lines[4][TIP_COL];
    int  nl = 0;
    snprintf(lines[nl++], TIP_COL,
        "[%d] ii=0x%04X  %dx%d  pal=%d",
        oi, o->ii, im ? im->w : 0, im ? im->h : 0, pal);
    snprintf(lines[nl++], TIP_COL,
        "  Z=%-4d sy=%-4d  wx=0x%04X  hfl=%d vfl=%d",
        o->depth, o->sy, o->wx, o->hfl, o->vfl);
    snprintf(lines[nl++], TIP_COL,
        "  layer 0x%02X", (o->wx >> 8) & 0xFF);

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
    SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 14, 14, 24, 225));
    {
        Uint32 bc = SDL_MapRGBA(surf->format, 110, 200, 110, 255); /* green border = editing */
        int pitch = surf->pitch / 4;
        Uint32 *px = (Uint32 *)surf->pixels;
        for (int x = 0; x < sw; x++) { px[x] = bc; px[(sh-1)*pitch+x] = bc; }
        for (int y = 0; y < sh; y++) { px[y*pitch] = bc; px[y*pitch+sw-1] = bc; }
    }
    Uint32 fg0 = SDL_MapRGBA(surf->format, 240, 230, 150, 255);
    Uint32 fg1 = SDL_MapRGBA(surf->format, 120, 240, 140, 255); /* bright green for layer line */
    font_draw_str(surf, TIP_PAD, TIP_PAD + 0 * TIP_LH, lines[0], fg0);
    font_draw_str(surf, TIP_PAD, TIP_PAD + 1 * TIP_LH, lines[1], fg0);
    font_draw_str(surf, TIP_PAD, TIP_PAD + 2 * TIP_LH, lines[2], fg1);

    SDL_SetSurfaceBlendMode(surf, SDL_BLENDMODE_BLEND);
    g_tip_tex = SDL_CreateTextureFromSurface(rend, surf);
    SDL_FreeSurface(surf);
    if (!g_tip_tex) return;
    SDL_SetTextureBlendMode(g_tip_tex, SDL_BLENDMODE_BLEND);

    g_tip_w = sw; g_tip_h = sh;
    g_tip_x = ax + 14;
    g_tip_y = ay + 14;
    if (g_tip_x + sw > ww) g_tip_x = ax - sw - 4;
    if (g_tip_y + sh > wh) g_tip_y = ay - sh - 4;
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
                picker_free_labels();
                g_picker_open = 0; g_place_img = -1;
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
                /* path-input mode: handle backspace/return, swallow everything else */
                if (g_path_input_open) {
                    if (ev.key.keysym.sym == SDLK_BACKSPACE && g_path_input_len > 0) {
                        g_path_input_buf[--g_path_input_len] = '\0';
                    } else if (ev.key.keysym.sym == SDLK_RETURN ||
                               ev.key.keysym.sym == SDLK_KP_ENTER) {
                        g_path_input_open = 0;
                        SDL_StopTextInput();
                        if (g_path_input_len > 0) {
                            /* import TGA then rebuild textures */
                            int old_ni = g_ni;
                            if (bdd_import_tga(g_path_input_buf) && g_ni > old_ni) {
                                textures = (SDL_Texture **)realloc(
                                    textures, (size_t)g_ni * sizeof(SDL_Texture *));
                                for (int i = old_ni; i < g_ni; i++)
                                    textures[i] = img_to_tex(rend, &g_img[i]);
                            }
                        }
                    } else if (ev.key.keysym.sym == SDLK_ESCAPE) {
                        g_path_input_open = 0;
                        SDL_StopTextInput();
                    }
                    break;
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
                case SDLK_l:
                    if ((ev.key.keysym.mod & KMOD_CTRL) && !g_path_input_open) {
                        char dlg_path[512] = "";
                        if (open_file_dialog(dlg_path, sizeof dlg_path)) {
                            /* dialog returned a path — import immediately */
                            int old_ni = g_ni;
                            if (bdd_import_tga(dlg_path) && g_ni > old_ni) {
                                textures = (SDL_Texture **)realloc(
                                    textures, (size_t)g_ni * sizeof(SDL_Texture *));
                                for (int i = old_ni; i < g_ni; i++)
                                    textures[i] = img_to_tex(rend, &g_img[i]);
                            }
                        } else {
                            /* no system dialog — fall back to text input */
                            g_path_input_open = 1;
                            g_path_input_buf[0] = '\0';
                            g_path_input_len = 0;
                            SDL_StartTextInput();
                        }
                    }
                    break;
                case SDLK_TAB:
                    if (g_have_bdb) {
                        g_picker_open ^= 1;
                        g_place_img = -1;
                    }
                    break;
                case SDLK_ESCAPE:
                    if (g_path_input_open) {
                        g_path_input_open = 0;
                        SDL_StopTextInput();
                    } else if (g_confirm_save) {
                        g_confirm_save = 0;
                        popup_free();
                    } else if (g_place_img >= 0) {
                        g_place_img = -1;
                    } else if (g_picker_open) {
                        g_picker_open = 0;
                    } else {
                        running = 0;
                    }
                    break;
                case SDLK_z:
                    if (g_last_obj >= 0 && g_last_obj < g_no) {
                        Obj *o = &g_obj[g_last_obj];
                        o->hfl ^= 1;
                        o->wx  = (o->wx & ~0x10) | (o->hfl ? 0x10 : 0);
                        fprintf(stderr, "obj[%d] hfl=%d\n", g_last_obj, o->hfl);
                    }
                    break;
                case SDLK_x:
                    if (g_last_obj >= 0 && g_last_obj < g_no) {
                        Obj *o = &g_obj[g_last_obj];
                        o->vfl ^= 1;
                        o->wx  = (o->wx & ~0x20) | (o->vfl ? 0x20 : 0);
                        fprintf(stderr, "obj[%d] vfl=%d\n", g_last_obj, o->vfl);
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

            case SDL_TEXTINPUT:
                if (g_path_input_open) {
                    int add = (int)strlen(ev.text.text);
                    if (g_path_input_len + add < (int)sizeof(g_path_input_buf) - 1) {
                        memcpy(g_path_input_buf + g_path_input_len, ev.text.text, (size_t)add);
                        g_path_input_len += add;
                        g_path_input_buf[g_path_input_len] = '\0';
                    }
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int bx = ev.button.x, by = ev.button.y;
                    if (g_picker_open && bx >= ww - PICKER_W) {
                        /* click inside picker panel — select image */
                        int idx = g_picker_scroll + by / PICKER_ITEM_H;
                        if (idx >= 0 && idx < g_ni) {
                            g_place_img   = idx;
                            g_picker_open = 0;
                        }
                    } else if (g_place_img >= 0) {
                        /* place new object at cursor world position */
                        if (g_no < MAX_OBJECTS) {
                            Obj *o  = &g_obj[g_no];
                            Img *im = &g_img[g_place_img];
                            o->wx    = 0x4100;
                            o->depth = bx / zoom + view_x;
                            o->sy    = by / zoom + view_y;
                            o->ii    = im->idx;
                            o->fl    = (im->pal_idx >= 0) ? im->pal_idx : 0;
                            o->hfl   = 0;
                            o->vfl   = 0;
                            o->order = g_no;
                            g_no++;
                            /* insert into sorted position */
                            Obj tmp = g_obj[g_no - 1];
                            int key = (tmp.wx >> 8) & 0xFF;
                            int j   = g_no - 2;
                            while (j >= 0 && (((g_obj[j].wx >> 8) & 0xFF) > key ||
                                              (((g_obj[j].wx >> 8) & 0xFF) == key &&
                                               g_obj[j].order > tmp.order))) {
                                g_obj[j + 1] = g_obj[j]; j--;
                            }
                            g_obj[j + 1] = tmp;
                            g_last_obj = (int)((&g_obj[j + 1]) - g_obj);
                        }
                        g_place_img = -1;
                    } else if ((SDL_GetModState() & KMOD_CTRL) && g_have_bdb) {
                        /* Ctrl+LMB — drag existing object */
                        int wx2 = bx / zoom + view_x;
                        int wy2 = by / zoom + view_y;
                        obj_drag_idx = -1;
                        for (int i = g_no - 1; i >= 0; i--) {
                            Obj *o  = &g_obj[i];
                            Img *im = img_find(o->ii);
                            if (!im) continue;
                            if (wx2 < o->depth || wx2 >= o->depth + im->w) continue;
                            if (wy2 < o->sy    || wy2 >= o->sy    + im->h) continue;
                            obj_drag_idx    = i;
                            obj_drag_ox     = bx;
                            obj_drag_oy     = by;
                            obj_drag_depth0 = o->depth;
                            obj_drag_sy0    = o->sy;
                            tooltip_free();
                            break;
                        }
                    } else if (!g_picker_open) {
                        dragging = 1;
                        drag_ox = bx; drag_oy = by;
                        drag_vx = view_x; drag_vy = view_y;
                    }
                }
                if (ev.button.button == SDL_BUTTON_RIGHT && g_place_img >= 0)
                    g_place_img = -1;
                break;

            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (obj_drag_idx >= 0) {
                        Obj *o = &g_obj[obj_drag_idx];
                        fprintf(stderr, "obj[%d] moved to depth=%d sy=%d\n",
                                obj_drag_idx, o->depth, o->sy);
                        g_last_obj   = obj_drag_idx;
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
                if (g_picker_open) {
                    if (ev.wheel.y < 0 && g_picker_scroll < g_ni - 1) g_picker_scroll++;
                    if (ev.wheel.y > 0 && g_picker_scroll > 0)        g_picker_scroll--;
                } else if ((SDL_GetModState() & KMOD_CTRL) && g_last_obj >= 0 && g_last_obj < g_no) {
                    /* Ctrl+wheel — adjust wx high byte (parallax layer) of last object */
                    int saved_order = g_obj[g_last_obj].order;
                    Obj *o = &g_obj[g_last_obj];
                    int hi = (o->wx >> 8) & 0xFF;
                    hi = (ev.wheel.y > 0) ? (hi + 1) & 0xFF : (hi - 1) & 0xFF;
                    o->wx = (hi << 8) | (o->wx & 0xFF);
                    /* re-sort */
                    for (int i = 1; i < g_no; i++) {
                        Obj tmp = g_obj[i];
                        int key = (tmp.wx >> 8) & 0xFF;
                        int j   = i - 1;
                        while (j >= 0 && (((g_obj[j].wx >> 8) & 0xFF) > key ||
                                          (((g_obj[j].wx >> 8) & 0xFF) == key &&
                                           g_obj[j].order > tmp.order))) {
                            g_obj[j + 1] = g_obj[j]; j--;
                        }
                        g_obj[j + 1] = tmp;
                    }
                    /* find new position of the moved object and update tooltip */
                    for (int i = 0; i < g_no; i++) {
                        if (g_obj[i].order == saved_order) { g_last_obj = i; break; }
                    }
                    tooltip_build_obj(rend, g_last_obj, hover_x, hover_y, ww, wh);
                    hover_printed = 1; /* suppress auto-hover overwrite */
                } else {
                    if (ev.wheel.y > 0 && zoom < 8) zoom++;
                    if (ev.wheel.y < 0 && zoom > 1) zoom--;
                }
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

        /* Path-input overlay */
        if (g_path_input_open)
            draw_path_input(rend, ww, wh);

        /* Ghost image — follows cursor when a placement is pending */
        if (g_place_img >= 0 && g_place_img < g_ni && textures[g_place_img]) {
            Img *im = &g_img[g_place_img];
            SDL_Rect dst = { hover_x, hover_y, im->w * zoom, im->h * zoom };
            SDL_SetTextureAlphaMod(textures[g_place_img], 160);
            SDL_RenderCopy(rend, textures[g_place_img], NULL, &dst);
            SDL_SetTextureAlphaMod(textures[g_place_img], 255);
        }

        /* Object picker panel */
        if (g_picker_open)
            draw_picker(rend, textures, ww, wh, hover_x, hover_y);

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
    picker_free_labels();
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
