/*
 * CrosshairFree - main.c
 * A free, open-source crosshair editor + overlay for Windows. Plain C + Win32.
 *
 *  - Overlay : click-through, always-on-top layered window with per-pixel alpha.
 *  - Editor  : layers (cross, rectangle, ellipse, triangle, PNG) each with its own colour,
 *              opacity, rotation, position, size and outline.
 *  - Library : saved crosshairs with live previews.
 *  - UI      : custom flat immediate-mode UI, fully re-colourable, scales to the screen.
 *              All text uses the embedded "Valve Rg" font (see app.rc); every slider has a number box.
 *  - Idle    : no timers run while nothing animates.
 *
 * Hotkeys (rebindable in Settings): default F8 = show/hide crosshair, F9 = open/close menu.
 * License: MIT
 */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "xh.h"

/* ------------------------------------------------------------- constants */
#define APP_NAME   "CrosshairFree"
#define WM_TRAY    (WM_APP + 1)
#define OVW        512                 /* overlay window size in px (layers may reach +-256px) */
#define LW         1040                /* menu size, logical px */
#define LH         660
#define TBH        38                  /* title bar height */
#define HK_TOGGLE  1
#define HK_MENU    2
#define ID_UITIMER 1
#define ID_TOPMOST 3
#define CFG_MAGIC  0x58484C33u
#define NSLOT      18
#define NBUILTIN   6
#define IDI_APP    1                   /* icon resource, see app.rc */
#define IDR_FONT   101                 /* embedded UI font resource, see app.rc */
#define UI_FACE    "Valve Rg"

/* ----------------------------------------------------------------- theme */
typedef struct { uint32_t bg, panel, widget, border, accent, text, dim; int round; } Theme;
#define NTHEMES 5
static const char *THEME_NAMES[NTHEMES] = { "Black & Red", "Graphite", "Light", "Green", "Purple" };
static const Theme THEMES[NTHEMES] = {
    { 0x0B0B0C, 0x141416, 0x1F1F22, 0x2E2E33, 0xE03131, 0xECECEC, 0x8B8B93, 3 },
    { 0x16181D, 0x1E2128, 0x2A2D37, 0x3B3F4C, 0x4C8DFF, 0xE6E8EE, 0x8E94A4, 3 },
    { 0xE6E7EB, 0xF6F6F8, 0xDADCE3, 0xC3C6D0, 0x2B6CE0, 0x1A1B1F, 0x6B6F7B, 3 },
    { 0x080C09, 0x0E140F, 0x17211A, 0x253629, 0x32D74B, 0xD7F5DC, 0x6F9077, 2 },
    { 0x0F0B14, 0x17121F, 0x221B2E, 0x362A4B, 0xA855F7, 0xEEE8F8, 0x8F83A8, 3 },
};
static Theme TH;
#define C_BG     TH.bg
#define C_PANEL  TH.panel
#define C_WIDGET TH.widget
#define C_BORDER TH.border
#define C_ACCENT TH.accent
#define C_TEXT   TH.text
#define C_DIM    TH.dim

/* ---------------------------------------------------------------- state */
static HINSTANCE g_inst;
static HWND  g_ov, g_ui;
static XH    X;                                  /* the crosshair being edited / shown */
static int   g_sel = 0, g_sub = 0, g_page = 0;
static int   g_show = 1, g_bg = 0, g_autostart = 0;
static char  g_cfgpath[MAX_PATH];
static int   g_scrW, g_scrH;

static UINT  hk_vk[2] = { VK_F8, VK_F9 }, hk_mod[2] = { 0, 0 };   /* [0] show/hide, [1] menu */
static int   hk_capture = -1;

typedef struct { int used, builtin; char name[32]; XH x; } Slot;
static Slot  lib[NSLOT];
static int   lib_sel = -1;
static char  lib_name[32];
static XH    draft; static int have_draft;

static uint32_t *ovBuf; static HBITMAP ovBmp; static HDC ovDC;
static uint32_t *uiBuf; static HBITMAP uiBmp; static HDC uiDC; static int UW, UH; static float UI = 1.0f;
static uint32_t *pvTmp, *pvComp; static int PVW, PVH;          /* editor preview buffers (physical px) */
static uint32_t *thTmp; static uint32_t *thumb[NSLOT]; static int TW, TH_;   /* library thumbnails */
static NOTIFYICONDATAA g_nid;
static HICON g_icon, g_icon_sm;
static float g_dpi = 1.0f;                            /* system DPI scale (for native menus) */
static int   g_fontok;                                /* embedded font registered? */

static unsigned rev = 1, ov_rev = 0;                   /* crosshair revision counters */
static int  ui_timer_on = 0;
static void touch(void) { rev++; }
static int  g_dump = 0;

/* ---------------------------------------------------------------- utils */
static double now_sec(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static uint32_t lerpc(uint32_t a, uint32_t b, float t)
{
    t = clampf(t, 0, 1);
    int r = (int)(((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
    int g = (int)(((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
    int bl = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}
static inline float ease(float t) { t = clampf(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }
static uint32_t hovc(void) { return lerpc(C_WIDGET, C_TEXT, 0.10f); }
static uint32_t onacc(void)
{
    float l = (0.299f * ((C_ACCENT >> 16) & 255) + 0.587f * ((C_ACCENT >> 8) & 255) + 0.114f * (C_ACCENT & 255)) / 255.0f;
    return l > 0.6f ? 0x101010 : 0xFFFFFF;
}
static Layer *sel_layer(void) { return (g_sel >= 0 && g_sel < X.n) ? &X.L[g_sel] : NULL; }

static void xh_sanitize(XH *x)
{
    if (x->n < 0) x->n = 0;
    if (x->n > XH_MAXL) x->n = XH_MAXL;
    for (int i = 0; i < x->n; i++) {
        Layer *l = &x->L[i];
        if (l->type < 0 || l->type >= LT_COUNT) l->type = LT_RECT;
        l->name[sizeof l->name - 1] = 0;
        l->r &= 255; l->g &= 255; l->b &= 255; l->ol_r &= 255; l->ol_g &= 255; l->ol_b &= 255;
        l->alpha = clampf(l->alpha, 0, 1); l->ol_alpha = clampf(l->ol_alpha, 0, 1);
        l->tint = (l->tint < 0 || l->tint > 2) ? 0 : l->tint;
        l->arms &= 15;
    }
}

/* ---------------------------------------------------------- persistence */
static void cfg_save(void)
{
    const XH *refs[NSLOT + 2]; int nr = 0;
    refs[nr++] = &X; if (have_draft) refs[nr++] = &draft;
    for (int i = NBUILTIN; i < NSLOT; i++) if (lib[i].used) refs[nr++] = &lib[i].x;
    xh_img_prune(refs, nr);

    FILE *f = fopen(g_cfgpath, "wb"); if (!f) return;
    uint32_t hdr[4] = { CFG_MAGIC, sizeof(XH), sizeof(Theme), sizeof(Slot) };
    fwrite(hdr, sizeof hdr, 1, f);
    int g[12] = { g_show, g_bg, lib_sel, have_draft, (int)hk_vk[0], (int)hk_vk[1], (int)hk_mod[0], (int)hk_mod[1], g_page, 0, 0, 0 };
    fwrite(g, sizeof g, 1, f);
    fwrite(&TH, sizeof TH, 1, f); fwrite(&X, sizeof X, 1, f); fwrite(&draft, sizeof draft, 1, f);
    for (int i = NBUILTIN; i < NSLOT; i++) fwrite(&lib[i], sizeof(Slot), 1, f);
    int cnt = 0; for (int i = 0; i < XH_MAXIMG; i++) if (xh_img[i].px) cnt++;
    fwrite(&cnt, sizeof cnt, 1, f);
    for (int i = 0; i < XH_MAXIMG; i++) if (xh_img[i].px) {
        int hd[3] = { i, xh_img[i].w, xh_img[i].h };
        fwrite(hd, sizeof hd, 1, f); fwrite(xh_img[i].px, 4, (size_t)hd[1] * hd[2], f);
    }
    fclose(f);
}
static void cfg_load(void)
{
    FILE *f = fopen(g_cfgpath, "rb"); if (!f) return;
    uint32_t hdr[4]; int g[12]; Theme th; XH x, dr; static Slot sl[NSLOT]; int ok = 0;
    if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == CFG_MAGIC && hdr[1] == sizeof(XH) && hdr[2] == sizeof(Theme) &&
        hdr[3] == sizeof(Slot) && fread(g, sizeof g, 1, f) == 1 && fread(&th, sizeof th, 1, f) == 1 &&
        fread(&x, sizeof x, 1, f) == 1 && fread(&dr, sizeof dr, 1, f) == 1) {
        ok = 1;
        for (int i = NBUILTIN; i < NSLOT; i++) if (fread(&sl[i], sizeof(Slot), 1, f) != 1) ok = 0;
    }
    if (ok) {
        int cnt = 0; ok = fread(&cnt, sizeof cnt, 1, f) == 1 && cnt >= 0 && cnt <= XH_MAXIMG;
        for (int k = 0; ok && k < cnt; k++) {
            int hd[3];
            if (fread(hd, sizeof hd, 1, f) != 1 || hd[0] < 0 || hd[0] >= XH_MAXIMG || hd[1] < 1 || hd[2] < 1 ||
                hd[1] > XH_IMGMAX || hd[2] > XH_IMGMAX) { ok = 0; break; }
            uint32_t *px = (uint32_t *)malloc((size_t)hd[1] * hd[2] * 4);
            if (!px || fread(px, 4, (size_t)hd[1] * hd[2], f) != (size_t)hd[1] * hd[2]) { free(px); ok = 0; break; }
            free(xh_img[hd[0]].px); xh_img[hd[0]].px = px; xh_img[hd[0]].w = hd[1]; xh_img[hd[0]].h = hd[2];
        }
    }
    fclose(f);
    if (!ok) return;
    TH = th; TH.round = (int)clampf((float)TH.round, 0, 10);
    X = x; xh_sanitize(&X); draft = dr; xh_sanitize(&draft); have_draft = g[3] != 0;
    g_show = g[0] != 0; g_bg = (g[1] >= 0 && g[1] < 4) ? g[1] : 0; lib_sel = (g[2] >= -1 && g[2] < NSLOT) ? g[2] : -1;
    for (int i = 0; i < 2; i++)
        if (g[4 + i] > 0 && g[4 + i] < 256 && (unsigned)g[6 + i] < 16) { hk_vk[i] = (UINT)g[4 + i]; hk_mod[i] = (UINT)g[6 + i]; }
    for (int i = NBUILTIN; i < NSLOT; i++) { lib[i] = sl[i]; lib[i].builtin = 0; lib[i].name[31] = 0; xh_sanitize(&lib[i].x); }
}

static const char *RUNKEY = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static int autostart_get(void)
{
    HKEY k; int ok = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUNKEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        ok = RegQueryValueExA(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; RegCloseKey(k);
    }
    return ok;
}
static void autostart_set(int on)
{
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUNKEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        char p[MAX_PATH + 2]; GetModuleFileNameA(NULL, p + 1, MAX_PATH); p[0] = '"'; strcat(p, "\"");
        RegSetValueExA(k, APP_NAME, 0, REG_SZ, (const BYTE *)p, (DWORD)strlen(p) + 1);
    } else RegDeleteValueA(k, APP_NAME);
    RegCloseKey(k);
}

/* ------------------------------------------------------------- clipboard */
static int clip_get(char *out, int n)
{
    int ok = 0;
    if (!OpenClipboard(g_ui ? g_ui : g_ov)) return 0;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        const char *p = (const char *)GlobalLock(h);
        if (p) { strncpy(out, p, (size_t)n - 1); out[n - 1] = 0; ok = 1; GlobalUnlock(h); }
    }
    CloseClipboard();
    return ok;
}
static void clip_set(const char *s)
{
    if (!OpenClipboard(g_ui ? g_ui : g_ov)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, strlen(s) + 1);
    if (h) { memcpy(GlobalLock(h), s, strlen(s) + 1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
}

/* ------------------------------------------------------------ PNG loading */
static int png_load_w(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 8 || sz > 32L * 1024 * 1024) { fclose(f); return -1; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);
    int w, h, n; unsigned char *d = stbi_load_from_memory(buf, (int)sz, &w, &h, &n, 4);
    free(buf);
    if (!d) return -1;
    int nw = w, nh = h;
    if (w > XH_IMGMAX || h > XH_IMGMAX) {
        float s = XH_IMGMAX / (float)(w > h ? w : h);
        nw = (int)(w * s); nh = (int)(h * s); if (nw < 1) nw = 1; if (nh < 1) nh = 1;
    }
    uint32_t *px = (uint32_t *)malloc((size_t)nw * nh * 4);
    if (!px) { stbi_image_free(d); return -1; }
    for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {          /* box filter, premultiplied */
        int sx0 = x * w / nw, sx1 = (x + 1) * w / nw, sy0 = y * h / nh, sy1 = (y + 1) * h / nh;
        if (sx1 <= sx0) sx1 = sx0 + 1;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        float r = 0, g = 0, b = 0, a = 0; int c = 0;
        for (int yy = sy0; yy < sy1; yy++) for (int xx = sx0; xx < sx1; xx++) {
            const unsigned char *p = d + ((size_t)yy * w + xx) * 4; float al = p[3] / 255.0f;
            r += p[0] * al; g += p[1] * al; b += p[2] * al; a += p[3]; c++;
        }
        px[y * nw + x] = ((uint32_t)(a / c + 0.5f) << 24) | ((uint32_t)(r / c + 0.5f) << 16) |
                         ((uint32_t)(g / c + 0.5f) << 8) | (uint32_t)(b / c + 0.5f);
    }
    stbi_image_free(d);
    return xh_img_add(nw, nh, px);
}

/* ---------------------------------------------------------------- hotkeys */
static int hk_register(void)
{
    int ok = 1;
    UnregisterHotKey(g_ov, HK_TOGGLE); UnregisterHotKey(g_ov, HK_MENU);
    for (int i = 0; i < 2; i++)
        if (!RegisterHotKey(g_ov, i + 1, hk_mod[i] | MOD_NOREPEAT, hk_vk[i])) ok = 0;
    return ok;
}
static void key_name(UINT vk, UINT mod, char *out, int n)
{
    char nm[64]; out[0] = 0;
    if (mod & MOD_CONTROL) strncat(out, "Ctrl+", (size_t)n - strlen(out) - 1);
    if (mod & MOD_ALT)     strncat(out, "Alt+", (size_t)n - strlen(out) - 1);
    if (mod & MOD_SHIFT)   strncat(out, "Shift+", (size_t)n - strlen(out) - 1);
    LONG lp = (LONG)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16;
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_NUMLOCK: case VK_DIVIDE: lp |= 1L << 24;
    }
    if (GetKeyNameTextA(lp, nm, sizeof nm) <= 0) snprintf(nm, sizeof nm, "Key %u", vk);
    strncat(out, nm, (size_t)n - strlen(out) - 1);
}
static void say(const char *s);
static void hk_capture_key(UINT vk)
{
    int i = hk_capture; UINT mod = 0;
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
    case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: return;
    }
    if (vk == VK_ESCAPE) { hk_capture = -1; hk_register(); say("Rebind cancelled"); return; }
    if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)    mod |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000)   mod |= MOD_SHIFT;
    hk_capture = -1;
    if (vk == hk_vk[1 - i] && mod == hk_mod[1 - i]) { hk_register(); say("That key is already used by the other action"); return; }
    UINT ov = hk_vk[i], om = hk_mod[i];
    hk_vk[i] = vk; hk_mod[i] = mod;
    if (!hk_register()) {
        hk_vk[i] = ov; hk_mod[i] = om; hk_register(); say("That key is unavailable (used by another app)");
        return;
    }
    char nm[64], m[120]; key_name(vk, mod, nm, sizeof nm);
    snprintf(m, sizeof m, "Hotkey set to %s", nm); say(m);
    cfg_save();
}

/* ---------------------------------------------------------------- overlay */
static void ov_update(void)
{
    if (!g_ov) return;
    xh_render(&X, ovBuf, OVW, OVW, 1.0f);
    POINT p = { g_scrW / 2 - OVW / 2 + (int)X.off_x, g_scrH / 2 - OVW / 2 + (int)X.off_y };
    SIZE s = { OVW, OVW }; POINT z = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_ov, NULL, &p, &s, ovDC, &z, 0, &bf, ULW_ALPHA);
}
static void ov_visibility(void)
{
    ShowWindow(g_ov, g_show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (g_show) ov_update();
}

/* ============================================================ UI raster */
static float gA = 1, gox = 0;               /* group alpha / x offset for page transitions */
static uint32_t fadeBg;
static float dt_ = 0.016f;
static int g_anim = 0;

static inline void blend_i(int i, uint32_t c, float a)
{
    if (a <= 0) return;
    if (a >= 0.999f) { uiBuf[i] = c; return; }
    uint32_t d = uiBuf[i];
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255, sr = (c >> 16) & 255, sg = (c >> 8) & 255, sb = c & 255;
    uiBuf[i] = ((uint32_t)(int)(dr + (sr - dr) * a) << 16) | ((uint32_t)(int)(dg + (sg - dg) * a) << 8) | (uint32_t)(int)(db + (sb - db) * a);
}
static inline void bpx(int x, int y, uint32_t c, float a)
{
    if ((unsigned)x >= (unsigned)UW || (unsigned)y >= (unsigned)UH) return;
    blend_i(y * UW + x, c, a * gA > 1 ? 1 : a * gA);
}

/* anti-aliased rounded rectangle (logical coords) */
static void rrect(float x, float y, float w, float h, float r, uint32_t c, float a)
{
    x = (x + gox) * UI; y *= UI; w *= UI; h *= UI; r *= UI;
    if (w <= 0 || h <= 0) return;
    float hw = w * 0.5f, hh = h * 0.5f, cx = x + hw, cy = y + hh, aa = a * gA;
    if (r > hh) r = hh;
    if (r > hw) r = hw;
    if (r < 0) r = 0;
    int x0 = (int)floorf(x), x1 = (int)ceilf(x + w), y0 = (int)floorf(y), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > UW) x1 = UW;
    if (y1 > UH) y1 = UH;
    int sx0 = (int)floorf(x + r + 0.5f) + 1, sx1 = (int)ceilf(x + w - r - 1.5f);
    if (sx0 < x0) sx0 = x0;
    if (sx1 > x1) sx1 = x1;
    for (int py = y0; py < y1; py++) {
        float qy = fabsf(py + 0.5f - cy) - (hh - r);
        int deep = qy < -1.0f;
        for (int px = x0; px < x1; px++) {
            if (deep && px >= sx0 && px < sx1) {                      /* fully covered span */
                for (int k = px; k < sx1; k++) blend_i(py * UW + k, c, aa);
                px = sx1 - 1; continue;
            }
            float qx = fabsf(px + 0.5f - cx) - (hw - r);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, m = qx > qy ? qx : qy; if (m > 0) m = 0;
            float cv = 0.5f - (sqrtf(ox * ox + oy * oy) + m - r);
            if (cv <= 0) continue;
            if (cv > 1) cv = 1;
            blend_i(py * UW + px, c, cv * aa > 1 ? 1 : cv * aa);
        }
    }
}
static void rect(float x, float y, float w, float h, uint32_t c) { rrect(x, y, w, h, 0, c, 1); }
static void circle(float cx, float cy, float r, uint32_t c, float a) { rrect(cx - r, cy - r, r * 2, r * 2, r, c, a); }
/* bordered box with the theme's corner roundness */
static void box(float x, float y, float w, float h, uint32_t fill, uint32_t border)
{
    float r = (float)TH.round;
    rrect(x, y, w, h, r, border, 1);
    rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill, 1);
}
static void panel(float x, float y, float w, float h) { box(x, y, w, h, C_PANEL, C_BORDER); }

/* The app logo (same design as the exe icon, see make_logo.py) drawn with UI primitives.
 * The crosshair follows the theme's accent colour. s = size in logical px. */
static void draw_logo(float x, float y, float s)
{
    const uint32_t tile = 0x121216, dot = 0xFFF4F0;
    float k = s / 256.0f, cx = x + s * 0.5f, cy = y + s * 0.5f;
    rrect(x, y, s, s, 58 * k, lerpc(tile, 0xFFFFFF, 0.18f), 1);                 /* tile + rim light */
    rrect(x + 0.8f, y + 0.8f, s - 1.6f, s - 1.6f, 57 * k, tile, 1);
    float a0 = 28 * k, al = 76 * k, at = 26 * k;                                /* arms (heavier: this is a small mark) */
    circle(cx, cy, 73 * k, C_ACCENT, 1); circle(cx, cy, 57 * k, tile, 1);       /* ring, broken where the arms pass */
    rect(cx - 24 * k, cy - 80 * k, 48 * k, 160 * k, tile); rect(cx - 80 * k, cy - 24 * k, 160 * k, 48 * k, tile);
    rect(cx + a0, cy - at / 2, al, at, C_ACCENT); rect(cx - a0 - al, cy - at / 2, al, at, C_ACCENT);
    rect(cx - at / 2, cy + a0, at, al, C_ACCENT); rect(cx - at / 2, cy - a0 - al, at, al, C_ACCENT);
    circle(cx, cy, 14 * k, dot, 1);
}

/* copy an opaque physical-pixel image into the UI buffer */
static void blit(int dx, int dy, const uint32_t *src, int w, int h)
{
    for (int y = 0; y < h; y++) {
        int yy = dy + y; if ((unsigned)yy >= (unsigned)UH) continue;
        int xs = dx < 0 ? -dx : 0, xe = (dx + w > UW) ? UW - dx : w;
        if (xe <= xs) continue;
        if (gA >= 0.999f) memcpy(&uiBuf[yy * UW + dx + xs], &src[y * w + xs], (size_t)(xe - xs) * 4);
        else for (int x = xs; x < xe; x++) blend_i(yy * UW + dx + x, src[y * w + x] & 0xFFFFFF, gA);
    }
}

/* ---- text: fits its box by shrinking the font, then ellipsis */
enum { F_N, F_B, F_T, F_S };
static const int FSZ[4] = { 12, 12, 14, 10 }, FWT[4] = { FW_BOLD, FW_BOLD, FW_BOLD, FW_BOLD };   /* the font only ships in Bold */
static HFONT fcache[4][96];

/* Registers the TTF embedded in the exe for this process only (no install, nothing left behind). */
static void font_load(void)
{
    HRSRC r = FindResourceA(g_inst, MAKEINTRESOURCEA(IDR_FONT), RT_RCDATA);
    HGLOBAL h = r ? LoadResource(g_inst, r) : NULL;
    void *p = h ? LockResource(h) : NULL; DWORD sz = r ? SizeofResource(g_inst, r) : 0, n = 0;
    if (p && sz && AddFontMemResourceEx(p, sz, NULL, &n) && n) g_fontok = 1;
}
static HFONT getfont(int f, int phys)
{
    if (phys < 6) phys = 6;
    if (phys > 95) phys = 95;
    if (!fcache[f][phys])
        fcache[f][phys] = CreateFontA(-phys, 0, 0, 0, FWT[f], 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                      g_fontok ? UI_FACE : "Segoe UI");
    return fcache[f][phys];
}
static float textw(const char *s, int f)       /* logical width at nominal size */
{
    SIZE sz; SelectObject(uiDC, getfont(f, (int)(FSZ[f] * UI + 0.5f)));
    GetTextExtentPoint32A(uiDC, s, (int)strlen(s), &sz);
    return sz.cx / UI;
}
static void text(float x, float y, float w, float h, const char *s, int f, uint32_t col, UINT fmt)
{
    int phys = (int)(FSZ[f] * UI + 0.5f), minp = 8;
    RECT rc = { (int)((x + gox) * UI), (int)(y * UI), (int)((x + gox + w) * UI), (int)((y + h) * UI) };
    int aw = rc.right - rc.left;
    SelectObject(uiDC, getfont(f, phys));
    if (fmt & DT_WORDBREAK) {
        for (;;) {
            RECT t = { 0, 0, aw, 0 };
            DrawTextA(uiDC, s, -1, &t, fmt | DT_CALCRECT | DT_NOPREFIX);
            if (t.bottom <= rc.bottom - rc.top || phys <= minp) break;
            phys--; SelectObject(uiDC, getfont(f, phys));
        }
        fmt |= DT_NOPREFIX;
    } else {
        SIZE sz; GetTextExtentPoint32A(uiDC, s, (int)strlen(s), &sz);
        while (sz.cx > aw && phys > minp) { phys--; SelectObject(uiDC, getfont(f, phys)); GetTextExtentPoint32A(uiDC, s, (int)strlen(s), &sz); }
        fmt |= DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
    }
    uint32_t c = lerpc(fadeBg, col, gA);
    SetBkMode(uiDC, TRANSPARENT); SetTextColor(uiDC, RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
    DrawTextA(uiDC, s, -1, &rc, fmt);
}
#define TL (DT_LEFT | DT_VCENTER)
#define TC (DT_CENTER | DT_VCENTER)
#define TR (DT_RIGHT | DT_VCENTER)

/* ======================================================== immediate-mode UI */
static float mx = -1000, my = -1000;
static int m_down, m_press, m_release, active, wid;
static float ws[2048][2]; static unsigned char wsinit[2048];
static int tf_focus, tf_hit; static char chq[32]; static int chq_n;
static float page_t = 0, menu_a = 0, tab_x = 0; static int menu_target = 0;
static char toast[160]; static float toast_t = 0;

static float approach(float v, float t, float k, float dt)
{
    float d = t - v;
    if (fabsf(d) < 0.002f) return t;
    g_anim = 1;
    return v + d * (1.0f - expf(-k * dt));
}
static int hitr(float x, float y, float w, float h)
{
    return mx >= x + gox && mx < x + gox + w && my >= y && my < y + h;
}
static void say(const char *s) { strncpy(toast, s, sizeof toast - 1); toast[sizeof toast - 1] = 0; toast_t = 2.8f; }
#define CHG(c) do { if (c) touch(); } while (0)

static void hdr(float x, float y, float w, const char *s)
{
    text(x, y, w, 16, s, F_S, C_DIM, TL);
    rect(x, y + 18, w, 1, C_BORDER);
}

static int button(float x, float y, float w, float h, const char *lab, int primary)
{
    int id = ++wid, hv = hitr(x, y, w, h), fired = 0;
    if (hv && m_press) active = id;
    if (m_release && active == id && hv) fired = 1;
    ws[id][0] = approach(ws[id][0], (float)hv, 18, dt_);
    ws[id][1] = approach(ws[id][1], (active == id && m_down && hv) ? 1.0f : 0.0f, 30, dt_);
    uint32_t fill, bord, tc;
    if (primary) {
        fill = lerpc(C_ACCENT, 0xFFFFFF, 0.10f * ws[id][0] - 0.10f * ws[id][1]); bord = C_ACCENT; tc = onacc();
    } else {
        fill = lerpc(lerpc(C_WIDGET, hovc(), ws[id][0]), C_ACCENT, 0.35f * ws[id][1]);
        bord = lerpc(C_BORDER, C_DIM, ws[id][0]); tc = C_TEXT;
    }
    box(x, y, w, h, fill, bord);
    fadeBg = fill; text(x + 4, y, w - 8, h, lab, F_N, tc, TC);
    return fired;
}

static int checkbox(float x, float y, float w, const char *lab, int *v)
{
    int id = ++wid, hv = hitr(x, y, w, 26), ch = 0;
    if (hv && m_press) active = id;
    if (m_release && active == id && hv) { *v = !*v; ch = 1; }
    if (!wsinit[id]) { ws[id][0] = (float)*v; wsinit[id] = 1; }
    ws[id][0] = approach(ws[id][0], (float)*v, 22, dt_);
    ws[id][1] = approach(ws[id][1], (float)hv, 18, dt_);
    float k = ws[id][0], by = y + 5;
    box(x, by, 16, 16, C_WIDGET, lerpc(C_BORDER, C_ACCENT, k > 0.5f ? 1.0f : ws[id][1] * 0.7f));
    if (k > 0.01f) { float s = 8 * k; rrect(x + 8 - s / 2, by + 8 - s / 2, s, s, TH.round > 1 ? 1.0f : 0.0f, C_ACCENT, 1); }
    fadeBg = C_PANEL; text(x + 24, y, w - 24, 26, lab, F_N, lerpc(C_DIM, C_TEXT, 0.55f + 0.45f * ws[id][1]), TL);
    return ch;
}

/* number shown in a slider's edit box: shortest form, up to 3 decimals ("3.25", "8", "0.375") */
static void num_str(char *o, int n, float v)
{
    snprintf(o, (size_t)n, "%.3f", v);
    int len = (int)strlen(o);
    while (len > 0 && o[len - 1] == '0') o[--len] = 0;
    if (len > 0 && o[len - 1] == '.') o[--len] = 0;
    if (!o[0] || !strcmp(o, "-0")) snprintf(o, (size_t)n, "0");
}
static char nf_buf[16]; static int nf_all;          /* edit-box text and "everything is selected" flag */

/* Slider + number box. Drag the bar (snaps to `step`) or click the box and type ANY value within
 * [mn, mxv], e.g. 3.25 or 0.375. whole = 1 restricts typing to whole numbers. Returns 1 on change. */
static int slider2(float x, float y, float w, const char *lab, float *v, float mn, float mxv, float step, const char *fmt, int whole)
{
    const float NFW = 66, NFH = 20, NUW = 30;
    int id = ++wid, idn = ++wid, ch = 0;
    uint32_t fb = fadeBg;
    const char *unit = strchr(fmt, 'f'); unit = unit ? unit + 1 : ""; while (*unit == ' ') unit++;
    float fx = x + w - NUW - NFW - 4, fy = y - 2;
    int hvn = hitr(fx, fy, NFW, NFH), hv = hitr(x, y, w, 36) && !hvn;

    /* --- bar */
    if (hv && m_press) active = id;
    if (active == id && (m_down || m_press)) {
        float t = clampf((mx - gox - x) / w, 0, 1);
        float nv = roundf((mn + t * (mxv - mn)) / step) * step;
        if (nv != *v) { *v = nv; ch = 1; }
    }

    /* --- number box: click to edit, Enter / click elsewhere to finish */
    int foc = (tf_focus == idn);
    if (hvn && m_press) {
        if (!foc) { num_str(nf_buf, sizeof nf_buf, *v); nf_all = 1; }
        tf_focus = idn; tf_hit = 1; foc = 1;
    }
    if (foc) {
        int edited = 0; g_anim = 1;
        for (int i = 0; i < chq_n; i++) {
            unsigned char c = (unsigned char)chq[i]; int len = (int)strlen(nf_buf);
            if (c == 13 || c == 9) { tf_focus = 0; foc = 0; break; }
            if (c == 8) { if (nf_all) { nf_buf[0] = 0; nf_all = 0; } else if (len) nf_buf[len - 1] = 0; edited = 1; continue; }
            if (c == ',') c = '.';
            if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) continue;
            if (c == '-' && ((!nf_all && len > 0) || mn >= 0)) continue;
            if (c == '.' && (whole || (!nf_all && strchr(nf_buf, '.')))) continue;
            if (nf_all) { nf_buf[0] = 0; len = 0; nf_all = 0; }
            if (len < (int)sizeof nf_buf - 2) { nf_buf[len] = (char)c; nf_buf[len + 1] = 0; edited = 1; }
        }
        chq_n = 0;
        if (edited) {                                               /* apply live while typing */
            int dig = 0; for (const char *q = nf_buf; *q; q++) if (*q >= '0' && *q <= '9') dig = 1;
            if (dig) {
                float nv = clampf((float)atof(nf_buf), mn, mxv);
                nv = whole ? roundf(nv) : roundf(nv * 1000.0f) / 1000.0f;
                if (nv != *v) { *v = nv; ch = 1; }
            }
        }
    }

    /* --- draw */
    float t = clampf((*v - mn) / (mxv - mn), 0, 1);
    if (!wsinit[id]) { ws[id][0] = t; wsinit[id] = 1; }
    ws[id][0] = approach(ws[id][0], t, 34, dt_);
    float hov = approach(ws[id][1], (hv || active == id) ? 1.0f : 0.0f, 18, dt_); ws[id][1] = hov;
    char buf[24];
    if (foc) snprintf(buf, sizeof buf, "%s", nf_buf); else num_str(buf, sizeof buf, *v);
    fadeBg = fb;
    text(x, y, fx - x - 6, 18, lab, F_N, lerpc(C_DIM, C_TEXT, 0.5f + 0.5f * hov), TL);
    ws[idn][0] = approach(ws[idn][0], (float)(foc || hvn), 18, dt_);
    box(fx, fy, NFW, NFH, C_WIDGET, foc ? C_ACCENT : lerpc(C_BORDER, C_DIM, ws[idn][0] * 0.6f));
    if (foc && nf_all && buf[0]) {                                   /* selection highlight */
        float tw = fminf(textw(buf, F_N), NFW - 12);
        rrect(fx + NFW - 6 - tw - 1, fy + 3, tw + 2, NFH - 6, 1, C_ACCENT, 0.45f);
    }
    fadeBg = C_WIDGET; text(fx + 6, fy, NFW - 12, NFH, buf, F_N, C_TEXT, TR);
    if (foc && ((int)(now_sec() * 2.0) & 1)) rect(fx + NFW - 5, fy + 4, 1, NFH - 8, C_TEXT);
    if (unit[0]) { fadeBg = fb; text(fx + NFW + 6, fy, NUW - 4, NFH, unit, F_S, C_DIM, TL); }
    fadeBg = fb;
    float kx = x + ws[id][0] * w;
    rrect(x, y + 25, w, 4, TH.round > 1 ? 2.0f : 0.0f, C_WIDGET, 1);
    if (kx - x > 0.5f) rrect(x, y + 25, kx - x, 4, TH.round > 1 ? 2.0f : 0.0f, C_ACCENT, 1);
    rrect(kx - 3.5f, y + 19, 7, 16, TH.round > 1 ? 2.0f : 0.0f, lerpc(C_TEXT, C_ACCENT, 0.45f * hov), 1);
    return ch;
}
static int slider(float x, float y, float w, const char *lab, float *v, float mn, float mxv, float step, const char *fmt)
{
    return slider2(x, y, w, lab, v, mn, mxv, step, fmt, 0);
}

static int segmented(float x, float y, float w, float h, const char **labs, int n, int *sel)
{
    int ch = 0; float bw = w / n;
    for (int i = 0; i < n; i++) {
        int id = ++wid, hv = hitr(x + i * bw, y, bw, h);
        if (hv && m_press) active = id;
        if (m_release && active == id && hv && *sel != i) { *sel = i; ch = 1; }
        ws[id][0] = approach(ws[id][0], (float)hv, 18, dt_);
        ws[id][1] = approach(ws[id][1], (float)(*sel == i), 20, dt_);
        uint32_t fill = lerpc(lerpc(C_WIDGET, hovc(), ws[id][0]), C_ACCENT, ws[id][1]);
        if (i == 0) rect(x, y, w, h, C_BORDER);
        rect(x + i * bw + 1, y + 1, bw - 1 - (i == n - 1 ? 1 : 0), h - 2, fill);
        fadeBg = fill; text(x + i * bw + 3, y, bw - 6, h, labs[i], F_N, lerpc(C_TEXT, onacc(), ws[id][1]), TC);
    }
    return ch;
}

/* single-line text input; returns 1 when the text changed. filter: 0 any, 1 hex digits */
static int textfield(float x, float y, float w, float h, char *buf, int cap, int filter)
{
    int id = ++wid, hv = hitr(x, y, w, h), ch = 0;
    if (hv && m_press) { tf_focus = id; tf_hit = 1; }
    int foc = (tf_focus == id);
    if (foc) {
        g_anim = 1;
        for (int i = 0; i < chq_n; i++) {
            unsigned char c = (unsigned char)chq[i]; int len = (int)strlen(buf);
            if (c == 8) { if (len) { buf[len - 1] = 0; ch = 1; } }
            else if (c == 13) { tf_focus = 0; foc = 0; }
            else if (c >= 32 && c < 127 && len < cap - 1) {
                if (filter == 1 && !strchr("0123456789abcdefABCDEF", c)) continue;
                buf[len] = (char)c; buf[len + 1] = 0; ch = 1;
            }
        }
        chq_n = 0;
    }
    ws[id][0] = approach(ws[id][0], (float)(foc || hv), 18, dt_);
    box(x, y, w, h, C_WIDGET, foc ? C_ACCENT : lerpc(C_BORDER, C_DIM, ws[id][0] * 0.6f));
    fadeBg = C_WIDGET; text(x + 7, y, w - 14, h, buf, F_N, C_TEXT, TL);
    if (foc && ((int)(now_sec() * 2.0) & 1)) {
        float cw = textw(buf, F_N); if (cw > w - 16) cw = w - 16;
        rect(x + 7 + cw + 1, y + 5, 1, h - 10, C_TEXT);
    }
    return ch;
}

/* ---- colour picker */
static void rgb2hsv(int r, int g, int b, float *h, float *s, float *v)
{
    float R = r / 255.0f, G = g / 255.0f, B = b / 255.0f;
    float mxc = fmaxf(R, fmaxf(G, B)), mnc = fminf(R, fminf(G, B)), d = mxc - mnc;
    *v = mxc; *s = mxc > 0 ? d / mxc : 0;
    float hh = 0;
    if (d > 0) {
        if (mxc == R) hh = fmodf((G - B) / d, 6.0f); else if (mxc == G) hh = (B - R) / d + 2; else hh = (R - G) / d + 4;
        hh /= 6.0f; if (hh < 0) hh += 1;
    }
    *h = hh;
}
static void hsv2rgb(float h, float s, float v, int *r, int *g, int *b)
{
    float c = v * s, hp = h * 6.0f, xx = c * (1 - fabsf(fmodf(hp, 2.0f) - 1)), m = v - c, R = 0, G = 0, B = 0;
    int i = (int)hp % 6;
    switch (i) { case 0: R = c; G = xx; break; case 1: R = xx; G = c; break; case 2: G = c; B = xx; break;
                 case 3: G = xx; B = c; break; case 4: R = xx; B = c; break; default: R = c; B = xx; break; }
    *r = (int)((R + m) * 255 + 0.5f); *g = (int)((G + m) * 255 + 0.5f); *b = (int)((B + m) * 255 + 0.5f);
}
static const uint32_t PAL9[9] = { 0xFFFFFF, 0x000000, 0xFF3B30, 0xFF9F0A, 0xFFE600, 0x34C759, 0x00E5FF, 0x4D7CFF, 0xFF2DD0 };
#define CP_H 158

/* hue / saturation-value picker + hex field + palette. Height = CP_H. */
static int colorpick(float x, float y, float w, int *r, int *g, int *b)
{
    int ids = ++wid, idh = ++wid, ch = 0;
    float sw = w - 30, hgt = 120, H, S, V;
    rgb2hsv(*r, *g, *b, &H, &S, &V);
    if (!wsinit[ids]) { ws[ids][0] = H; wsinit[ids] = 1; }
    if (S > 0.003f && V > 0.003f) ws[ids][0] = H;
    H = ws[ids][0];
    if (hitr(x, y, sw, hgt) && m_press) active = ids;
    if (hitr(x + sw + 8, y, 22, hgt) && m_press) active = idh;
    if (active == ids && (m_down || m_press)) {
        S = clampf((mx - gox - x) / sw, 0, 1); V = 1 - clampf((my - y) / hgt, 0, 1);
        hsv2rgb(H, S, V, r, g, b); ch = 1;
    }
    if (active == idh && (m_down || m_press)) {
        H = clampf((my - y) / hgt, 0, 0.9999f); ws[ids][0] = H; hsv2rgb(H, S, V, r, g, b); ch = 1;
    }
    /* draw */
    rect(x - 1, y - 1, sw + 2, hgt + 2, C_BORDER); rect(x + sw + 7, y - 1, 24, hgt + 2, C_BORDER);
    int hr, hg, hb; hsv2rgb(H, 1, 1, &hr, &hg, &hb);
    int X0 = (int)((x + gox) * UI), Y0 = (int)(y * UI), SWp = (int)(sw * UI), Hp = (int)(hgt * UI);
    for (int py = 0; py < Hp; py++) {
        float v = 1.0f - (py + 0.5f) / Hp;
        for (int px = 0; px < SWp; px++) {
            float s = (px + 0.5f) / SWp;
            int cr = (int)(v * ((1 - s) * 255 + s * hr)), cg = (int)(v * ((1 - s) * 255 + s * hg)), cb = (int)(v * ((1 - s) * 255 + s * hb));
            bpx(X0 + px, Y0 + py, ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb, 1);
        }
    }
    int HX = (int)((x + sw + 8 + gox) * UI), HW = (int)(22 * UI);
    for (int py = 0; py < Hp; py++) {
        int cr, cg, cb; hsv2rgb((py + 0.5f) / Hp * 0.9999f, 1, 1, &cr, &cg, &cb);
        for (int px = 0; px < HW; px++) bpx(HX + px, Y0 + py, ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb, 1);
    }
    uint32_t cur = ((uint32_t)*r << 16) | ((uint32_t)*g << 8) | (uint32_t)*b;
    float mxp = x + S * sw, myp = y + (1 - V) * hgt;
    circle(mxp, myp, 6, 0x000000, 1); circle(mxp, myp, 5, 0xFFFFFF, 1); circle(mxp, myp, 3.5f, cur, 1);
    float hy = y + H * hgt;
    rect(x + sw + 6, hy - 2, 26, 4, 0x000000); rect(x + sw + 7, hy - 1, 24, 2, 0xFFFFFF);

    /* hex + palette */
    float ry = y + hgt + 8;
    static char hexb[8]; int fid = wid + 1;
    if (tf_focus != fid) snprintf(hexb, sizeof hexb, "%02X%02X%02X", *r, *g, *b);
    fadeBg = C_PANEL; text(x, ry, 14, 26, "#", F_N, C_DIM, TL);
    if (textfield(x + 14, ry, 74, 26, hexb, 7, 1) && strlen(hexb) == 6) {
        int nr, ng, nb; if (xh_hex(hexb, &nr, &ng, &nb)) { *r = nr; *g = ng; *b = nb; ch = 1; }
    }
    float px0 = x + 100, step = (w - 100) / 9.0f;
    for (int i = 0; i < 9; i++) {
        int id = ++wid, hv = hitr(px0 + i * step, ry + 3, step - 4, 20);
        if (hv && m_press) active = id;
        if (m_release && active == id && hv) { *r = (PAL9[i] >> 16) & 255; *g = (PAL9[i] >> 8) & 255; *b = PAL9[i] & 255; ch = 1; }
        ws[id][0] = approach(ws[id][0], (float)hv, 18, dt_);
        box(px0 + i * step, ry + 3, step - 4, 20, PAL9[i], lerpc(C_BORDER, C_TEXT, ws[id][0]));
    }
    return ch;
}

/* ============================================================= scenes */
static const uint32_t BGT[4] = { 0x1B1C22, 0x5E8E52, 0xC2AC80, 0xE6EAF2 }, BGB[4] = { 0x121317, 0x47703F, 0x9E8A62, 0xC5CCDA };

static void comp_scene(uint32_t *dst, const uint32_t *src, int W, int H, int bgi, int guides, const Layer *sel, float z)
{
    float cx = W * 0.5f, cy = H * 0.5f, ca = 1, sa = 0, hw = 0, hh = 0, R = 0, scx = 0, scy = 0;
    uint32_t gc = (bgi == 3) ? 0x000000 : 0xFFFFFF;
    if (sel) {
        float ang = sel->rot * 0.01745329252f; ca = cosf(ang); sa = sinf(ang);
        xh_layer_half(sel, &hw, &hh); hw *= z; hh *= z;
        if (sel->ol_on && sel->type != LT_IMAGE) { hw += sel->ol_thick * z; hh += sel->ol_thick * z; }
        R = sqrtf(hw * hw + hh * hh) + 2; scx = cx + sel->x * z; scy = cy + sel->y * z;
    }
    for (int y = 0; y < H; y++) {
        uint32_t base = (y < H * 0.58f) ? BGT[bgi] : BGB[bgi];
        for (int x = 0; x < W; x++) {
            uint32_t c = base;
            if (guides && (fabsf(x + 0.5f - cx) <= 0.5f || fabsf(y + 0.5f - cy) <= 0.5f)) c = lerpc(c, gc, 0.09f);
            uint32_t s = src[y * W + x];
            if (s >> 24) {
                float k = 1.0f - (float)(s >> 24) / 255.0f;
                int rr = (int)(((s >> 16) & 255) + ((c >> 16) & 255) * k), gg = (int)(((s >> 8) & 255) + ((c >> 8) & 255) * k), bb = (int)((s & 255) + (c & 255) * k);
                if (rr > 255) rr = 255;
                if (gg > 255) gg = 255;
                if (bb > 255) bb = 255;
                c = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
            }
            if (sel) {
                float Xp = x + 0.5f - scx, Yp = y + 0.5f - scy;
                if (fabsf(Xp) < R && fabsf(Yp) < R) {
                    float d = sd_box(Xp * ca + Yp * sa, -Xp * sa + Yp * ca, hw, hh), k = 1.0f - fabsf(d);
                    if (k > 0) c = lerpc(c, C_ACCENT, k * 0.9f);
                }
            }
            dst[y * W + x] = c;
        }
    }
}

static float fit_zoom(const XH *x, int W, int H, float maxz)
{
    float ext = xh_extent(x), z = ((W < H ? W : H) * 0.44f) / ext;
    return clampf(z, 1.0f, maxz);
}

static void thumb_invalidate_all(void) { for (int i = 0; i < NSLOT; i++) { free(thumb[i]); thumb[i] = NULL; } }
static void thumb_make(int i)
{
    if (!lib[i].used || thumb[i]) return;
    thumb[i] = (uint32_t *)malloc((size_t)TW * TH_ * 4);
    if (!thumb[i]) return;
    float z = fit_zoom(&lib[i].x, TW, TH_, 5.0f);
    xh_render(&lib[i].x, thTmp, TW, TH_, z);
    comp_scene(thumb[i], thTmp, TW, TH_, g_bg, 0, NULL, z);
}

/* ============================================================== actions */
static void do_import(void)
{
    char buf[1100]; const char *fmt = "?"; XH t;
    if (!clip_get(buf, sizeof buf)) { say("Clipboard is empty - copy a crosshair code first"); return; }
    memset(&t, 0, sizeof t);
    if (xh_import_auto(buf, &t, g_scrH, &fmt)) {
        t.off_x = X.off_x; t.off_y = X.off_y; X = t; g_sel = 0; touch();
        char m[160]; snprintf(m, sizeof m, "Imported %s crosshair as layers", fmt); say(m);
    } else say("Not a supported crosshair code (CS2 / CS:GO / Valorant)");
}
static void add_png_path(const wchar_t *path)
{
    if (X.n >= XH_MAXL) { say("Layer limit reached (16)"); return; }
    int idx = png_load_w(path);
    if (idx < 0) { say("Couldn't read that PNG"); return; }
    int a = xh_add(&X, LT_IMAGE); if (a < 0) return;
    Layer *l = &X.L[a]; l->img = idx; l->ol_on = 0;
    float s = 48.0f / (float)(xh_img[idx].w > xh_img[idx].h ? xh_img[idx].w : xh_img[idx].h);
    l->w = fmaxf(1, floorf(xh_img[idx].w * s)); l->h = fmaxf(1, floorf(xh_img[idx].h * s));
    g_sel = a; touch(); say("PNG added - resize, tint and move it in the editor");
}
static void pick_png(void)
{
    wchar_t path[MAX_PATH] = L""; OPENFILENAMEW o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = g_ui;
    o.lpstrFilter = L"PNG images (*.png)\0*.png\0All files\0*.*\0"; o.lpstrFile = path; o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) add_png_path(path);
}
static void lib_load(int i)
{
    if (i < 0 || i >= NSLOT || !lib[i].used) return;
    {   XH t = X; if (lib_sel >= 0) { t.off_x = lib[lib_sel].x.off_x; t.off_y = lib[lib_sel].x.off_y; }
        if (lib_sel < 0 || memcmp(&t, &lib[lib_sel].x, sizeof(XH)) != 0) { draft = X; have_draft = 1; } }
    float ox = X.off_x, oy = X.off_y;
    X = lib[i].x; X.off_x = ox; X.off_y = oy; g_sel = 0; lib_sel = i; touch();
    char m[100]; snprintf(m, sizeof m, "Loaded \"%s\"", lib[i].name); say(m);
}
static void lib_store(int i, const char *name)
{
    lib[i].used = 1; lib[i].builtin = 0; lib[i].x = X;
    snprintf(lib[i].name, sizeof lib[i].name, "%s", name);
    free(thumb[i]); thumb[i] = NULL; lib_sel = i; cfg_save();
}
static void default_name(char *out, int n)
{
    for (int k = 1; k < 100; k++) {
        char nm[32]; snprintf(nm, sizeof nm, "Crosshair %d", k); int clash = 0;
        for (int i = 0; i < NSLOT; i++) if (lib[i].used && !strcmp(lib[i].name, nm)) clash = 1;
        if (!clash) { snprintf(out, (size_t)n, "%s", nm); return; }
    }
    snprintf(out, (size_t)n, "Crosshair");
}

/* ================================================================ pages */
static const char *TYPETAG[LT_COUNT] = { "cross", "rect", "ellipse", "triangle", "png" };
static float pv_zoom = 1, drag_zoom, drag_mx0, drag_my0, drag_lx, drag_ly; static int drag = 0;
static unsigned pv_rev; static int pv_sel = -2, pv_bg = -1, pv_built = 0;

static void page_editor(void)
{
    Layer *L;
    /* ---------- left: layers ---------- */
    wid = 10; fadeBg = C_PANEL;
    panel(8, 46, 240, 606);
    hdr(18, 54, 220, "LAYERS");
    if (X.n == 0) text(18, 84, 220, 40, "No layers yet. Add one below.", F_N, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
    for (int r = 0; r < X.n; r++) {
        int i = X.n - 1 - r; float y = 80 + r * 24;
        int ide = ++wid, idr = ++wid, hve = hitr(20, y + 4, 18, 18), hvr = hitr(16, y, 224, 24) && !hve;
        if (hve && m_press) active = ide;
        if (m_release && active == ide && hve) { X.L[i].visible = !X.L[i].visible; touch(); }
        if (hvr && m_press) active = idr;
        if (m_release && active == idr && hvr && g_sel != i) { g_sel = i; tf_focus = 0; memset(wsinit + 1000, 0, 640); }
        ws[idr][0] = approach(ws[idr][0], (float)hvr, 18, dt_);
        ws[idr][1] = approach(ws[idr][1], (float)(g_sel == i), 22, dt_);
        uint32_t rowc = lerpc(lerpc(C_PANEL, hovc(), ws[idr][0] * 0.7f), C_WIDGET, ws[idr][1]);
        rrect(16, y, 224, 24, (float)TH.round, rowc, 1);
        if (ws[idr][1] > 0.01f) rect(16, y + 2, 3 * ws[idr][1], 20, C_ACCENT);
        box(22, y + 5, 14, 14, X.L[i].visible ? C_ACCENT : C_WIDGET, X.L[i].visible ? C_ACCENT : C_BORDER);
        fadeBg = rowc;
        text(44, y, 130, 24, X.L[i].name, F_N, X.L[i].visible ? C_TEXT : C_DIM, TL);
        text(176, y, 58, 24, TYPETAG[X.L[i].type], F_S, C_DIM, TR);
    }
    fadeBg = C_PANEL;
    hdr(18, 470, 220, "ADD LAYER");
    { float bw = (224 - 14) / 3.0f;
      if (button(18, 492, bw, 30, "Cross", 0))     { int a = xh_add(&X, LT_CROSS);   if (a >= 0) { g_sel = a; touch(); } else say("Layer limit reached (16)"); }
      if (button(18 + bw + 7, 492, bw, 30, "Rect", 0))    { int a = xh_add(&X, LT_RECT);    if (a >= 0) { g_sel = a; touch(); } else say("Layer limit reached (16)"); }
      if (button(18 + 2 * (bw + 7), 492, bw, 30, "Ellipse", 0)) { int a = xh_add(&X, LT_ELLIPSE); if (a >= 0) { g_sel = a; touch(); } else say("Layer limit reached (16)"); }
      if (button(18, 528, bw, 30, "Triangle", 0)) { int a = xh_add(&X, LT_TRI);     if (a >= 0) { g_sel = a; touch(); } else say("Layer limit reached (16)"); }
      if (button(18 + bw + 7, 528, 224 - bw - 7, 30, "Add PNG image...", 1)) pick_png();
    }
    hdr(18, 566, 220, "SELECTED LAYER");
    { float bw = (224 - 21) / 4.0f; L = sel_layer();
      if (button(18, 588, bw, 28, "Copy", 0) && L)   { int a = xh_dup(&X, g_sel); if (a >= 0) { g_sel = a; touch(); } else say("Layer limit reached (16)"); }
      if (button(18 + bw + 7, 588, bw, 28, "Delete", 0) && L) { xh_del(&X, g_sel); if (g_sel >= X.n) g_sel = X.n - 1; if (g_sel < 0) g_sel = 0; touch(); }
      if (button(18 + 2 * (bw + 7), 588, bw, 28, "Up", 0) && L)   { g_sel = xh_move(&X, g_sel, 1); touch(); }
      if (button(18 + 3 * (bw + 7), 588, bw, 28, "Down", 0) && L) { g_sel = xh_move(&X, g_sel, -1); touch(); }
    }
    text(18, 620, 220, 28, "Tip: drop a .png onto this window. Arrow keys nudge the layer, Delete removes it.", F_S, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);

    /* ---------- right: properties ---------- */
    L = sel_layer();
    panel(672, 46, 360, 606);
    if (!L) {
        text(684, 60, 336, 60, "Select a layer on the left, or add one.", F_N, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else {
        fadeBg = C_PANEL;
        text(684, 54, 230, 22, L->name, F_B, C_TEXT, TL);
        text(920, 54, 100, 22, xh_typename[L->type], F_S, C_DIM, TR);
        static const char *subs[3] = { "Shape", "Color", "Outline" };
        wid = 1000 - 3; segmented(684, 82, 336, 28, subs, 3, &g_sub);
        wid = 1000 + (g_sub * 5 + L->type) * 40; float x = 684, w = 336, y = 124;
        fadeBg = C_PANEL;
        if (g_sub == 0) {
            switch (L->type) {
            case LT_CROSS:
                CHG(slider(x, y, w, "Arm length", &L->w, 0, 100, 0.5f, "%.1f px")); y += 40;
                CHG(slider(x, y, w, "Arm thickness", &L->h, 0.5f, 24, 0.5f, "%.1f px")); y += 40;
                CHG(slider(x, y, w, "Gap from center", &L->gap, -20, 100, 0.5f, "%.1f px")); y += 42;
                { static const int order[4] = { 3, 0, 1, 2 }; static const char *an[4] = { "Top", "Right", "Bottom", "Left" };
                  text(x, y, 120, 20, "Arms", F_N, C_DIM, TL); y += 22;
                  for (int k = 0; k < 4; k++) {
                      int a = (L->arms >> order[k]) & 1;
                      if (checkbox(x + k * 84, y, 80, an[k], &a)) { L->arms = (L->arms & ~(1 << order[k])) | (a << order[k]); touch(); }
                  }
                  y += 38; }
                break;
            case LT_RECT:
                CHG(slider(x, y, w, "Width", &L->w, 1, 200, 0.5f, "%.1f px")); y += 40;
                CHG(slider(x, y, w, "Height", &L->h, 1, 200, 0.5f, "%.1f px")); y += 40;
                CHG(slider(x, y, w, "Corner radius", &L->radius, 0, 50, 0.5f, "%.1f px")); y += 40;
                CHG(slider(x, y, w, "Border width (0 = filled)", &L->stroke, 0, 40, 0.5f, "%.1f px")); y += 40;
                break;
            case LT_ELLIPSE: case LT_TRI:
                CHG(slider(x, y, w, "Width", &L->w, 1, 200, 0.5f, "%.1f px")); y += 40;
                CHG(slider(x, y, w, "Height", &L->h, 1, 200, 0.5f, "%.1f px")); y += 40;
                CHG(slider(x, y, w, "Border width (0 = filled)", &L->stroke, 0, 40, 0.5f, "%.1f px")); y += 40;
                break;
            default: {
                float iw = 1, ih = 1;
                if (L->img >= 0 && L->img < XH_MAXIMG && xh_img[L->img].px) { iw = (float)xh_img[L->img].w; ih = (float)xh_img[L->img].h; }
                if (slider(x, y, w, "Width", &L->w, 1, 250, 0.5f, "%.1f px")) { if (L->lock) L->h = fmaxf(1, roundf(L->w * ih / iw * 100) / 100); touch(); } y += 40;
                if (slider(x, y, w, "Height", &L->h, 1, 250, 0.5f, "%.1f px")) { if (L->lock) L->w = fmaxf(1, roundf(L->h * iw / ih * 100) / 100); touch(); } y += 40;
                checkbox(x, y, w, "Lock aspect ratio", &L->lock); y += 34;
                break; }
            }
            CHG(slider(x, y, w, "Rotation", &L->rot, -180, 180, 1, "%.0f deg")); y += 40;
            CHG(slider(x, y, w, "Position X", &L->x, -250, 250, 0.5f, "%.1f px")); y += 40;
            CHG(slider(x, y, w, "Position Y", &L->y, -250, 250, 0.5f, "%.1f px")); y += 40;
            text(x, y + 4, w, 30, "Tip: you can also drag the selected layer in the preview.", F_S, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else if (g_sub == 1) {
            if (L->type == LT_IMAGE) {
                static const char *tn[3] = { "Original", "Tint", "Flat color" };
                CHG(segmented(x, y, w, 28, tn, 3, &L->tint)); y += 38;
            }
            if (L->type != LT_IMAGE || L->tint > 0) { CHG(colorpick(x, y, w, &L->r, &L->g, &L->b)); y += CP_H + 12; }
            else { text(x, y, w, 40, "Original PNG colors are used. Pick Tint or Flat color to recolor it.", F_N, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK); y += 50; }
            CHG(slider(x, y, w, "Opacity", &L->alpha, 0, 1, 0.01f, "%.2f"));
        } else {
            if (L->type == LT_IMAGE) {
                text(x, y, w, 40, "PNG layers have no outline. Use a Flat color tint and a second copy underneath if you need one.", F_N, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
            } else {
                CHG(checkbox(x, y, w, "Draw outline", &L->ol_on)); y += 34;
                CHG(slider(x, y, w, "Thickness", &L->ol_thick, 0, 12, 0.5f, "%.1f px")); y += 40;
                CHG(slider(x, y, w, "Opacity", &L->ol_alpha, 0, 1, 0.01f, "%.2f")); y += 46;
                CHG(colorpick(x, y, w, &L->ol_r, &L->ol_g, &L->ol_b));
            }
        }
    }

    /* ---------- centre: preview + position + actions ---------- */
    wid = 420; fadeBg = C_BG;
    float cxp = 256, cw = 408;
    rect(cxp, 46, cw, 334, C_BORDER);
    if (rev != pv_rev || g_sel != pv_sel || g_bg != pv_bg || !pv_built || drag) {
        pv_zoom = drag ? drag_zoom : fit_zoom(&X, PVW, PVH, 6.0f);
        xh_render(&X, pvTmp, PVW, PVH, pv_zoom);
        comp_scene(pvComp, pvTmp, PVW, PVH, g_bg, 1, sel_layer(), pv_zoom);
        pv_rev = rev; pv_sel = g_sel; pv_bg = g_bg; pv_built = 1;
    }
    blit((int)((cxp + 1 + gox) * UI), (int)(47 * UI), pvComp, PVW, PVH);
    /* drag to move the selected layer */
    L = sel_layer();
    if (L && hitr(cxp, 46, cw, 334) && m_press) { drag = 1; drag_mx0 = mx; drag_my0 = my; drag_lx = L->x; drag_ly = L->y; drag_zoom = pv_zoom; }
    if (drag) {
        if (!m_down || !L) drag = 0;
        else {
            float nx = roundf((drag_lx + (mx - drag_mx0) * UI / drag_zoom) * 2) / 2, ny = roundf((drag_ly + (my - drag_my0) * UI / drag_zoom) * 2) / 2;
            nx = clampf(nx, -250, 250); ny = clampf(ny, -250, 250);
            if (nx != L->x || ny != L->y) { L->x = nx; L->y = ny; touch(); }
        }
    }
    fadeBg = C_BG;
    text(cxp, 386, 250, 22, "Drag in the preview to move the selected layer", F_S, C_DIM, TL);
    { char z[32]; snprintf(z, sizeof z, "%.1fx", pv_zoom); text(cxp + 252, 386, 60, 22, z, F_S, C_DIM, TR); }
    for (int i = 0; i < 4; i++) {
        float bx = cxp + cw - 4 * 24 + i * 24; int id = ++wid, hv = hitr(bx, 387, 20, 20);
        if (hv && m_press) active = id;
        if (m_release && active == id && hv && g_bg != i) { g_bg = i; thumb_invalidate_all(); }
        ws[id][0] = approach(ws[id][0], (float)(g_bg == i), 20, dt_);
        box(bx, 387, 20, 20, BGT[i], lerpc(C_BORDER, C_ACCENT, ws[id][0]));
    }
    fadeBg = C_BG;
    hdr(cxp, 414, cw, "SCREEN POSITION  (whole crosshair)");
    { fadeBg = C_BG;
      float rx = (float)(g_scrW / 2), ry = (float)(g_scrH / 2);
      CHG(slider2(cxp, 438, cw, "Horizontal offset", &X.off_x, -rx, rx, 1, "%.0f px", 1));
      CHG(slider2(cxp, 478, cw, "Vertical offset", &X.off_y, -ry, ry, 1, "%.0f px", 1)); }
    if (checkbox(cxp, 522, cw, "Show crosshair on screen", &g_show)) { ov_visibility(); }
    { float bw = (cw - 12) / 3.0f;
      if (button(cxp, 556, bw, 34, "Paste & Import", 1)) do_import();
      if (button(cxp + bw + 6, 556, bw, 34, "Copy Valorant code", 0)) {
          char b[512]; xh_export_valorant(&X, b, sizeof b); clip_set(b); say("Valorant code copied to clipboard"); }
      if (button(cxp + 2 * (bw + 6), 556, bw, 34, "Reset", 0)) {
          float ox = X.off_x, oy = X.off_y; xh_make_preset(&X, 0); X.off_x = ox; X.off_y = oy; g_sel = 0; touch(); say("Reset to default"); }
    }
    { char a[64], b[64], l[200]; key_name(hk_vk[0], hk_mod[0], a, sizeof a); key_name(hk_vk[1], hk_mod[1], b, sizeof b);
      fadeBg = C_BG;
      snprintf(l, sizeof l, "%s show/hide crosshair   |   %s open/close menu   |   Ctrl+V imports a game code", a, b);
      text(cxp, 598, cw, 40, l, F_S, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK); }
}

static void page_library(void)
{
    wid = 500; fadeBg = C_BG;
    /* toolbar */
    if (!lib_name[0] && tf_focus == 0) default_name(lib_name, sizeof lib_name);
    text(8, 46, 50, 30, "Name", F_N, C_DIM, TL);
    textfield(58, 46, 220, 30, lib_name, 31, 0);
    if (button(288, 46, 120, 30, "Save as new", 1)) {
        int s = -1; for (int i = NBUILTIN; i < NSLOT; i++) if (!lib[i].used) { s = i; break; }
        if (s < 0) say("All save slots are full - overwrite or delete one");
        else { lib_store(s, lib_name[0] ? lib_name : "Crosshair"); lib_name[0] = 0; say("Saved to the library"); }
    }
    if (button(414, 46, 150, 30, "Overwrite selected", 0)) {
        if (lib_sel < NBUILTIN) say("Click one of your saves first (presets are read-only)");
        else { lib_store(lib_sel, lib[lib_sel].name); say("Save updated"); }
    }
    if (button(570, 46, 90, 30, "Rename", 0)) {
        if (lib_sel < NBUILTIN) say("Click one of your saves first");
        else if (lib_name[0]) { snprintf(lib[lib_sel].name, sizeof lib[lib_sel].name, "%s", lib_name); cfg_save(); say("Renamed"); }
    }
    if (button(666, 46, 80, 30, "Delete", 0)) {
        if (lib_sel < NBUILTIN) say("Click one of your saves first");
        else { lib[lib_sel].used = 0; free(thumb[lib_sel]); thumb[lib_sel] = NULL; lib_sel = -1; cfg_save(); say("Deleted"); }
    }
    if (button(752, 46, 150, 30, "Restore last draft", 0)) {
        if (have_draft) { XH t = X; X = draft; draft = t; g_sel = 0; touch(); say("Restored your previous unsaved crosshair"); }
        else say("No draft to restore");
    }
    text(908, 46, 124, 30, "Click a card to load it", F_S, C_DIM, TR);

    /* grid */
    const float cw = 164, ch = 172, gap = 8, y0 = 88;
    for (int i = 0; i < NSLOT; i++) {
        float x = 8 + (i % 6) * (cw + gap), y = y0 + (i / 6) * (ch + gap);
        int id = ++wid, hv = hitr(x, y, cw, ch);
        if (hv && m_press) active = id;
        if (m_release && active == id && hv) {
            if (lib[i].used) lib_load(i);
            else { lib_store(i, lib_name[0] ? lib_name : "Crosshair"); lib_name[0] = 0; say("Saved to the library"); }
        }
        ws[id][0] = approach(ws[id][0], (float)hv, 18, dt_);
        ws[id][1] = approach(ws[id][1], (float)(lib_sel == i), 20, dt_);
        uint32_t bc = lerpc(lerpc(C_BORDER, C_DIM, ws[id][0]), C_ACCENT, ws[id][1]);
        box(x, y, cw, ch, C_PANEL, bc);
        if (ws[id][1] > 0.5f) rect(x + 1, y + 1, cw - 2, 2, C_ACCENT);
        if (lib[i].used) {
            thumb_make(i);
            if (thumb[i]) blit((int)((x + 1 + gox) * UI), (int)((y + 1) * UI), thumb[i], TW, TH_);
            fadeBg = C_PANEL;
            text(x + 8, y + 134, cw - 16, 18, lib[i].name, F_B, C_TEXT, TL);
            text(x + 8, y + 151, cw - 16, 16, lib[i].builtin ? "PRESET" : "SAVED", F_S, C_DIM, TL);
        } else {
            fadeBg = C_PANEL;
            text(x, y + 40, cw, 50, "+", F_T, lerpc(C_DIM, C_TEXT, ws[id][0]), TC);
            text(x + 8, y + 90, cw - 16, 20, "Save current here", F_N, lerpc(C_DIM, C_TEXT, ws[id][0]), TC);
            text(x + 8, y + 151, cw - 16, 16, "EMPTY", F_S, C_DIM, TL);
        }
    }
}

static void page_settings(void)
{
    wid = 700; fadeBg = C_PANEL;
    /* ----- left: keybinds / general / info ----- */
    panel(8, 46, 500, 606);
    float x = 24, w = 468, y = 56;
    hdr(x, y, w, "HOTKEYS  (click a button, then press the new key)"); y += 30;
    static const char *hl[2] = { "Show / hide crosshair", "Open / close menu" };
    for (int i = 0; i < 2; i++) {
        char nm[64]; int cap = (hk_capture == i);
        if (cap) snprintf(nm, sizeof nm, "Press a key..."); else key_name(hk_vk[i], hk_mod[i], nm, sizeof nm);
        fadeBg = C_PANEL; text(x, y, w - 200, 32, hl[i], F_N, C_TEXT, TL);
        if (button(x + w - 190, y, 190, 32, nm, cap)) {
            hk_capture = i; UnregisterHotKey(g_ov, HK_TOGGLE); UnregisterHotKey(g_ov, HK_MENU); SetFocus(g_ui);
        }
        y += 40;
    }
    fadeBg = C_PANEL;
    text(x, y, w, 20, "Modifiers work too (Ctrl / Alt / Shift + key). Esc cancels.", F_S, C_DIM, TL); y += 40;
    hdr(x, y, w, "GENERAL"); y += 30;
    if (checkbox(x, y, w, "Start with Windows", &g_autostart)) autostart_set(g_autostart);
    y += 40;
    hdr(x, y, w, "SUPPORTED GAME CODES"); y += 30;
    fadeBg = C_PANEL;
    text(x, y, w, 20, "CS2:   CS...    and old   CSGO-xxxxx-xxxxx-...", F_N, C_TEXT, TL); y += 24;
    text(x, y, w, 20, "Valorant:   0;P;c;5;0l;4;0o;2;0a;1;...", F_N, C_TEXT, TL); y += 28;
    text(x, y, w, 60, "Imported codes become normal layers you can edit freely. Game dynamics (spread, recoil) are not simulated.", F_S, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK); y += 54;
    hdr(x, y, w, "ABOUT"); y += 30;
    text(x, y, w, 80, "CrosshairFree v2.1 - MIT license. Works over windowed / borderless games; exclusive fullscreen can hide overlays. "
         "The program never touches game memory, but check your game's rules about overlays.", F_S, C_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK); y += 90;
    if (button(x, y, 190, 32, "Quit CrosshairFree", 0)) DestroyWindow(g_ov);

    /* ----- right: theme ----- */
    panel(516, 46, 516, 606);
    x = 532; w = 484; y = 56;
    hdr(x, y, w, "INTERFACE THEME"); y += 30;
    { float bw = (w - (NTHEMES - 1) * 6) / NTHEMES;
      for (int i = 0; i < NTHEMES; i++)
          if (button(x + i * (bw + 6), y, bw, 30, THEME_NAMES[i], 0)) { TH = THEMES[i]; thumb_invalidate_all(); pv_built = 0; } }
    y += 42;
    static int th_sel = 4;
    { float rf = (float)TH.round;
      if (slider(x, y, w, "Corner roundness", &rf, 0, 10, 1, "%.0f px")) TH.round = (int)rf; }
    y += 44;
    static const char *tn[7] = { "Window background", "Panels", "Buttons & slider tracks", "Borders", "Accent (sliders, active items)", "Text", "Secondary text" };
    uint32_t *tc[7] = { &TH.bg, &TH.panel, &TH.widget, &TH.border, &TH.accent, &TH.text, &TH.dim };
    hdr(x, y, w, "CUSTOM COLORS  (pick one, then edit it below)"); y += 28;
    for (int i = 0; i < 7; i++) {
        int id = ++wid, hv = hitr(x, y, w, 26);
        if (hv && m_press) active = id;
        if (m_release && active == id && hv) { th_sel = i; tf_focus = 0; memset(wsinit + 780, 0, 40); }
        ws[id][0] = approach(ws[id][0], (float)hv, 18, dt_);
        ws[id][1] = approach(ws[id][1], (float)(th_sel == i), 20, dt_);
        uint32_t rowc = lerpc(lerpc(C_PANEL, hovc(), ws[id][0] * 0.7f), C_WIDGET, ws[id][1]);
        rrect(x, y, w, 26, (float)TH.round, rowc, 1);
        box(x + 6, y + 4, 36, 18, *tc[i], C_BORDER);
        fadeBg = rowc; text(x + 52, y, w - 60, 26, tn[i], F_N, C_TEXT, TL);
        y += 29;
    }
    y += 8;
    { uint32_t *c = tc[th_sel]; int r = (*c >> 16) & 255, g = (*c >> 8) & 255, b = *c & 255;
      wid = 790;
      if (colorpick(x, y, w, &r, &g, &b)) { *c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; thumb_invalidate_all(); pv_built = 0; } }
}

static const char *PAGES[3] = { "Editor", "Library", "Settings" };

/* ================================================================ frame */
static void ui_set_alpha(void)
{
    if (!g_dump) SetLayeredWindowAttributes(g_ui, 0, (BYTE)(255.0f * ease(menu_a)), LWA_ALPHA);
}

static void ui_render(void)
{
    static double last = 0; double t = now_sec();
    dt_ = (float)(t - last); last = t;
    if (dt_ > 0.05f || dt_ <= 0) dt_ = 0.016f;
    g_anim = 0; gA = 1; gox = 0; fadeBg = C_BG; tf_hit = 0;

    for (int i = 0; i < UW * UH; i++) uiBuf[i] = C_BG;

    float pm = menu_a;
    menu_a = approach(menu_a, (float)menu_target, 12, dt_);
    if (menu_a != pm) ui_set_alpha();

    /* ---- title bar ---- */
    wid = 900;
    rect(0, 0, LW, TBH, C_PANEL); rect(0, TBH - 1, LW, 1, C_BORDER);
    fadeBg = C_PANEL;
    draw_logo(9, 7, 24);
    text(42, 0, 144, TBH, "CrosshairFree", F_T, C_TEXT, TL);
    for (int i = 0; i < 3; i++) {
        float x = 190 + i * 96; int id = ++wid, hv = hitr(x, 0, 96, TBH - 1);
        if (hv && m_press) active = id;
        if (m_release && active == id && hv && g_page != i) { g_page = i; page_t = 0; tf_focus = 0; }
        ws[id][0] = approach(ws[id][0], (float)hv, 18, dt_);
        fadeBg = C_PANEL; text(x, 0, 96, TBH - 1, PAGES[i], F_B, lerpc(C_DIM, C_TEXT, ws[id][0] * 0.7f + (g_page == i ? 1.0f : 0.0f)), TC);
    }
    { float tx = 190 + g_page * 96; if (tab_x == 0) tab_x = tx; tab_x = approach(tab_x, tx, 18, dt_); rect(tab_x + 18, TBH - 3, 60, 3, C_ACCENT); }
    { int id = ++wid, hv = hitr(LW - 44, 0, 44, TBH - 1), fired = 0;
      if (hv && m_press) active = id;
      if (m_release && active == id && hv) fired = 1;
      ws[id][0] = approach(ws[id][0], (float)hv, 20, dt_);
      rect(LW - 44, 0, 43, TBH - 1, lerpc(C_PANEL, C_ACCENT, ws[id][0]));
      fadeBg = lerpc(C_PANEL, C_ACCENT, ws[id][0]); text(LW - 44, 0, 43, TBH - 1, "X", F_B, lerpc(C_DIM, onacc(), ws[id][0]), TC);
      if (fired) menu_target = 0; }

    /* ---- page ---- */
    page_t = approach(page_t, 1, 9, dt_);
    gA = ease(page_t); gox = (1 - ease(page_t)) * 14; fadeBg = C_BG;
    switch (g_page) { case 0: page_editor(); break; case 1: page_library(); break; default: page_settings(); break; }
    gA = 1; gox = 0;

    /* ---- toast ---- */
    if (toast_t > 0) {
        toast_t -= dt_; g_anim = 1;
        float a = clampf(fminf(toast_t * 5, (2.8f - toast_t) * 8), 0, 1);
        float tw = fminf(560.0f, textw(toast, F_N) + 44), tx = (LW - tw) / 2, ty = LH - 56 - (1 - ease(a)) * -8;
        gA = ease(a);
        box(tx, ty, tw, 34, C_WIDGET, C_ACCENT);
        fadeBg = C_WIDGET; text(tx + 14, ty, tw - 28, 34, toast, F_N, C_TEXT, TC);
        gA = 1;
    }

    /* ---- frame ---- */
    for (int x = 0; x < UW; x++) { uiBuf[x] = C_BORDER; uiBuf[(UH - 1) * UW + x] = C_BORDER; }
    for (int y = 0; y < UH; y++) { uiBuf[y * UW] = C_BORDER; uiBuf[y * UW + UW - 1] = C_BORDER; }

    if (!m_down && tf_focus && m_press && !tf_hit) tf_focus = 0;
    if (m_press && !tf_hit) tf_focus = 0;
    chq_n = 0;
    if (m_release) { active = 0; m_release = 0; }
    m_press = 0;
    if (ov_rev != rev) { ov_update(); ov_rev = rev; }
}

static void ui_kick(void)
{
    if (!ui_timer_on) { SetTimer(g_ui, ID_UITIMER, 15, NULL); ui_timer_on = 1; }
    InvalidateRect(g_ui, NULL, FALSE);
}
static void ui_show(int show)
{
    if (show) {
        menu_target = 1; page_t = 0;
        if (!IsWindowVisible(g_ui)) { menu_a = 0; ui_set_alpha(); ShowWindow(g_ui, SW_SHOW); }
        SetForegroundWindow(g_ui);
    } else menu_target = 0;
    ui_kick();
}

static LRESULT CALLBACK ui_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(h, &ps);
        ui_render();
        BitBlt(ps.hdc, 0, 0, UW, UH, uiDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        int animated = g_anim || m_down;
        if (menu_target == 0 && menu_a <= 0.001f) {
            ShowWindow(h, SW_HIDE); cfg_save(); tf_focus = 0;
            if (ui_timer_on) { KillTimer(h, ID_UITIMER); ui_timer_on = 0; }
        } else if (!animated && ui_timer_on) { KillTimer(h, ID_UITIMER); ui_timer_on = 0; }
        else if (animated && !ui_timer_on) { SetTimer(h, ID_UITIMER, 15, NULL); ui_timer_on = 1; }
        return 0;
    }
    case WM_TIMER: if (w == ID_UITIMER) InvalidateRect(h, NULL, FALSE); return 0;
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
        if (p.y < (int)(TBH * UI) && (p.x < (int)(186 * UI) || (p.x > (int)(490 * UI) && p.x < UW - (int)(48 * UI)))) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 }; TrackMouseEvent(&te);
        mx = GET_X_LPARAM(l) / UI; my = GET_Y_LPARAM(l) / UI; ui_kick(); return 0;
    }
    case WM_MOUSELEAVE: mx = my = -1000; ui_kick(); return 0;
    case WM_LBUTTONDOWN: SetCapture(h); SetFocus(h); m_down = 1; m_press = 1;
        mx = GET_X_LPARAM(l) / UI; my = GET_Y_LPARAM(l) / UI; ui_kick(); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); m_down = 0; m_release = 1;
        mx = GET_X_LPARAM(l) / UI; my = GET_Y_LPARAM(l) / UI; ui_kick(); return 0;
    case WM_CHAR:
        if (tf_focus && chq_n < 31) chq[chq_n++] = (char)w;
        ui_kick(); return 0;
    case WM_SYSKEYDOWN:
        if (hk_capture >= 0) { hk_capture_key((UINT)w); ui_kick(); return 0; }
        break;
    case WM_KILLFOCUS:
        if (hk_capture >= 0) { hk_capture = -1; hk_register(); ui_kick(); }
        return 0;
    case WM_KEYDOWN:
        if (hk_capture >= 0) { hk_capture_key((UINT)w); ui_kick(); return 0; }
        if (w == VK_ESCAPE) { if (tf_focus) tf_focus = 0; else ui_show(0); ui_kick(); return 0; }
        if (!tf_focus && g_page == 0 && sel_layer()) {
            Layer *l = sel_layer(); float st = (GetKeyState(VK_SHIFT) & 0x8000) ? 5.0f : 1.0f; int used = 1;
            if (w == VK_LEFT) l->x = clampf(l->x - st, -250, 250);
            else if (w == VK_RIGHT) l->x = clampf(l->x + st, -250, 250);
            else if (w == VK_UP) l->y = clampf(l->y - st, -250, 250);
            else if (w == VK_DOWN) l->y = clampf(l->y + st, -250, 250);
            else if (w == VK_DELETE) { xh_del(&X, g_sel); if (g_sel >= X.n) g_sel = X.n - 1; if (g_sel < 0) g_sel = 0; }
            else used = 0;
            if (used) { touch(); ui_kick(); return 0; }
        }
        if (w == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (tf_focus) { char b[64]; if (clip_get(b, sizeof b)) for (int i = 0; b[i] && chq_n < 31; i++) if ((unsigned char)b[i] >= 32) chq[chq_n++] = b[i]; }
            else do_import();
            ui_kick(); return 0;
        }
        return 0;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        if (DragQueryFileW((HDROP)w, 0, path, MAX_PATH)) { add_png_path(path); g_page = 0; }
        DragFinish((HDROP)w); ui_kick(); return 0;
    }
    case WM_CLOSE: ui_show(0); return 0;
    case WM_ACTIVATE: if (LOWORD(w) != WA_INACTIVE) ui_kick(); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ------------------------------------------------------------- overlay proc */
/* The tray menu is owner-drawn so it uses the app font and theme instead of the system menu font. */
typedef struct { char txt[112]; int checked, sep; } MItem;
static MItem g_mi[4];
static COLORREF cref(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }
static HFONT menu_font(void) { return getfont(F_N, (int)(FSZ[F_N] * g_dpi * 1.08f + 0.5f)); }
static void tray_menu(void)
{
    HMENU m = CreatePopupMenu(); POINT p; GetCursorPos(&p);
    char k1[64], k2[64];
    key_name(hk_vk[1], hk_mod[1], k1, sizeof k1); key_name(hk_vk[0], hk_mod[0], k2, sizeof k2);
    memset(g_mi, 0, sizeof g_mi);
    snprintf(g_mi[0].txt, sizeof g_mi[0].txt, "Open menu   (%s)", k1);
    snprintf(g_mi[1].txt, sizeof g_mi[1].txt, "Crosshair visible   (%s)", k2); g_mi[1].checked = g_show;
    g_mi[2].sep = 1;
    snprintf(g_mi[3].txt, sizeof g_mi[3].txt, "Exit");
    AppendMenuA(m, MF_OWNERDRAW, 1, (LPCSTR)&g_mi[0]);
    AppendMenuA(m, MF_OWNERDRAW, 2, (LPCSTR)&g_mi[1]);
    AppendMenuA(m, MF_OWNERDRAW | MF_DISABLED, 0, (LPCSTR)&g_mi[2]);
    AppendMenuA(m, MF_OWNERDRAW, 3, (LPCSTR)&g_mi[3]);
    HBRUSH bg = CreateSolidBrush(cref(C_PANEL));
    MENUINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi; mi.fMask = MIM_BACKGROUND; mi.hbrBack = bg;
    SetMenuInfo(m, &mi);
    SetForegroundWindow(g_ov);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_ov, NULL);
    DestroyMenu(m); DeleteObject(bg);
    if (c == 1) ui_show(1);
    else if (c == 2) { g_show = !g_show; ov_visibility(); cfg_save(); }
    else if (c == 3) DestroyWindow(g_ov);
}
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_HOTKEY:
        if (w == HK_TOGGLE) { g_show = !g_show; ov_visibility(); cfg_save(); }
        else if (w == HK_MENU) ui_show(!(IsWindowVisible(g_ui) && menu_target));
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        if (mi->CtlType != ODT_MENU) break;
        const MItem *it = (const MItem *)mi->itemData;
        if (it->sep) { mi->itemWidth = 0; mi->itemHeight = (UINT)(9 * g_dpi); }
        else {
            HDC dc = GetDC(h); HGDIOBJ of = SelectObject(dc, menu_font()); SIZE sz;
            GetTextExtentPoint32A(dc, it->txt, (int)strlen(it->txt), &sz);
            SelectObject(dc, of); ReleaseDC(h, dc);
            mi->itemWidth = (UINT)(sz.cx + 52 * g_dpi); mi->itemHeight = (UINT)(30 * g_dpi);
        }
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)l;
        if (d->CtlType != ODT_MENU) break;
        const MItem *it = (const MItem *)d->itemData; RECT rc = d->rcItem; float sc = g_dpi;
        int sel = (d->itemState & ODS_SELECTED) && !it->sep;
        HBRUSH b = CreateSolidBrush(cref(sel ? C_ACCENT : C_PANEL)); FillRect(d->hDC, &rc, b); DeleteObject(b);
        if (it->sep) {
            int my_ = (rc.top + rc.bottom) / 2; RECT ln = { rc.left + (int)(10 * sc), my_, rc.right - (int)(10 * sc), my_ + 1 };
            HBRUSH lb = CreateSolidBrush(cref(C_BORDER)); FillRect(d->hDC, &ln, lb); DeleteObject(lb);
        } else {
            if (it->checked) {
                int bs = (int)(8 * sc + 0.5f), bx = rc.left + (int)(14 * sc), by = (rc.top + rc.bottom - bs) / 2;
                RECT r8 = { bx, by, bx + bs, by + bs };
                HBRUSH cb = CreateSolidBrush(cref(sel ? onacc() : C_ACCENT)); FillRect(d->hDC, &r8, cb); DeleteObject(cb);
            }
            HGDIOBJ of = SelectObject(d->hDC, menu_font());
            SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, cref(sel ? onacc() : C_TEXT));
            RECT tr = { rc.left + (int)(34 * sc), rc.top, rc.right - (int)(14 * sc), rc.bottom };
            DrawTextA(d->hDC, it->txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(d->hDC, of);
        }
        return TRUE;
    }
    case WM_TIMER:
        if (w == ID_TOPMOST && g_show) SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    case WM_TRAY:
        if (l == WM_LBUTTONUP) ui_show(!(IsWindowVisible(g_ui) && menu_target));
        else if (l == WM_RBUTTONUP) tray_menu();
        return 0;
    case WM_DESTROY:
        cfg_save();
        Shell_NotifyIconA(NIM_DELETE, &g_nid);
        UnregisterHotKey(h, HK_TOGGLE); UnregisterHotKey(h, HK_MENU);
        if (g_ui) DestroyWindow(g_ui);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ------------------------------------------------------------ setup helpers */
static uint32_t *make_dib(int w, int h, HDC *dc, HBITMAP *bmp)
{
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL; HDC sdc = GetDC(NULL);
    *dc = CreateCompatibleDC(sdc);
    *bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(*dc, *bmp); ReleaseDC(NULL, sdc);
    return (uint32_t *)bits;
}
static HICON make_icon(void)
{
    enum { S = 32 };
    XH ic; memset(&ic, 0, sizeof ic);
    int a = xh_add(&ic, LT_CROSS); ic.L[a].r = 0; ic.L[a].g = 229; ic.L[a].b = 255; ic.L[a].w = 6; ic.L[a].h = 2; ic.L[a].gap = 3; ic.L[a].ol_on = 0;
    a = xh_add(&ic, LT_RECT); ic.L[a].w = ic.L[a].h = 2; ic.L[a].r = 0; ic.L[a].g = 229; ic.L[a].b = 255; ic.L[a].ol_on = 0;
    uint32_t src[S * S]; xh_render(&ic, src, S, S, 1.7f);
    HDC dc; HBITMAP cb; uint32_t *px = make_dib(S, S, &dc, &cb);
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        float d = sqrtf((x + 0.5f - S / 2) * (x + 0.5f - S / 2) + (y + 0.5f - S / 2) * (y + 0.5f - S / 2));
        float bgc = clampf(S / 2 - 0.5f - d, 0, 1);
        uint32_t s = src[y * S + x]; float sa = (s >> 24) / 255.0f, oa = sa + bgc * (1 - sa);
        float r = ((s >> 16) & 255) + 20 * bgc * (1 - sa), g = ((s >> 8) & 255) + 22 * bgc * (1 - sa), b = (s & 255) + 28 * bgc * (1 - sa);
        if (oa > 0.001f) {
            int ia = (int)(oa * 255), ir = (int)clampf(r / oa, 0, 255), ig = (int)clampf(g / oa, 0, 255), ib = (int)clampf(b / oa, 0, 255);
            px[y * S + x] = ((uint32_t)ia << 24) | ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | (uint32_t)ib;
        } else px[y * S + x] = 0;
    }
    HBITMAP mask = CreateBitmap(S, S, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, cb };
    HICON hi = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(cb); DeleteDC(dc);
    return hi;
}

static void write_bmp(const char *path)           /* developer aid: --dump=file.bmp */
{
    BITMAPFILEHEADER fh; BITMAPINFOHEADER ih; memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + UW * UH * 4;
    ih.biSize = sizeof ih; ih.biWidth = UW; ih.biHeight = -UH; ih.biPlanes = 1; ih.biBitCount = 32;
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(uiBuf, 4, (size_t)UW * UH, f); fclose(f);
}
static int argint(const char *cmd, const char *key, int def)
{
    const char *p = strstr(cmd, key); return p ? atoi(p + strlen(key)) : def;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd0, int show)
{
    (void)hp; (void)cmd0; (void)show;
    const char *cmd = GetCommandLineA();
    g_inst = hi; g_dump = strstr(cmd, "--dump=") != NULL;
    font_load();
    HANDLE mtx = CreateMutexA(NULL, TRUE, g_dump ? "CrosshairFree_Dump" : "CrosshairFree_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    GdiSetBatchLimit(1);

    GetModuleFileNameA(NULL, g_cfgpath, MAX_PATH);
    char *sl = strrchr(g_cfgpath, '\\'); if (sl) *(sl + 1) = 0;
    strcat(g_cfgpath, g_dump ? "crosshair_free_dump.cfg" : "crosshair_free.cfg");

    TH = THEMES[0];
    for (int i = 0; i < NBUILTIN; i++) { lib[i].used = 1; lib[i].builtin = 1; snprintf(lib[i].name, sizeof lib[i].name, "%s", xh_preset_names[i]); xh_make_preset(&lib[i].x, i); }
    xh_make_preset(&X, 0);
    cfg_load();
    g_autostart = autostart_get();

    g_scrW = GetSystemMetrics(SM_CXSCREEN); g_scrH = GetSystemMetrics(SM_CYSCREEN);
    RECT wa; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
    { const char *p = strstr(cmd, "--screen="); int a, b; if (p && sscanf(p + 9, "%dx%d", &a, &b) == 2) { g_scrW = aw = a; g_scrH = b; ah = b - 40; } }
    HDC sdc = GetDC(NULL); UI = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f; ReleaseDC(NULL, sdc);
    g_dpi = UI;
    { const char *p = strstr(cmd, "--dpi="); if (p) UI = (float)atof(p + 6) / 96.0f; }
    float fit = fminf((aw * 0.96f) / LW, (ah * 0.96f) / LH);        /* shrink the whole UI to fit small screens */
    if (UI > fit) UI = fit;
    if (UI < 0.55f) UI = 0.55f;
    UW = (int)(LW * UI); UH = (int)(LH * UI);
    PVW = ((int)(406 * UI)) & ~1; PVH = ((int)(332 * UI)) & ~1;
    TW = ((int)(162 * UI)) & ~1;  TH_ = ((int)(130 * UI)) & ~1;

    ovBuf = make_dib(OVW, OVW, &ovDC, &ovBmp);
    uiBuf = make_dib(UW, UH, &uiDC, &uiBmp);
    pvTmp = (uint32_t *)malloc((size_t)PVW * PVH * 4); pvComp = (uint32_t *)malloc((size_t)PVW * PVH * 4);
    thTmp = (uint32_t *)malloc((size_t)TW * TH_ * 4);
    g_icon    = (HICON)LoadImageA(hi, MAKEINTRESOURCEA(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    g_icon_sm = (HICON)LoadImageA(hi, MAKEINTRESOURCEA(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_icon) g_icon = make_icon();                  /* fallback if the resource is missing */
    if (!g_icon_sm) g_icon_sm = g_icon;

    WNDCLASSEXA wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.hInstance = hi; wc.hIcon = g_icon; wc.hIconSm = g_icon_sm;
    wc.lpfnWndProc = ov_proc; wc.lpszClassName = "CrosshairFreeOverlay";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = ui_proc; wc.lpszClassName = "CrosshairFreeMenu";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.style = CS_DROPSHADOW;
    RegisterClassExA(&wc);

    g_ov = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           "CrosshairFreeOverlay", APP_NAME, WS_POPUP, g_scrW / 2 - OVW / 2, g_scrH / 2 - OVW / 2, OVW, OVW,
                           NULL, NULL, hi, NULL);
    g_ui = CreateWindowExA(WS_EX_LAYERED | WS_EX_APPWINDOW, "CrosshairFreeMenu", APP_NAME, WS_POPUP,
                           wa.left + (aw - UW) / 2, wa.top + (ah - UH) / 2, UW, UH, NULL, NULL, hi, NULL);
    DragAcceptFiles(g_ui, TRUE);
    {   /* Windows 11 rounded corners (ignored on older versions) */
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        if (dwm) {
            typedef HRESULT (WINAPI *DwmSet)(HWND, DWORD, LPCVOID, DWORD);
            DwmSet f = (DwmSet)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
            int pref = 2;
            if (f) f(g_ui, 33, &pref, sizeof pref);
            FreeLibrary(dwm);
        }
    }
    ui_set_alpha();

    if (g_dump) {                                   /* render a frame to a BMP and quit (used for testing) */
        char path[MAX_PATH] = ""; const char *p = strstr(cmd, "--dump="); sscanf(p + 7, "%259s", path);
        g_page = argint(cmd, "--page=", 0); g_sub = argint(cmd, "--sub=", 0); g_sel = argint(cmd, "--sel=", 0);
        { int t = argint(cmd, "--theme=", 0); if (t > 0 && t < NTHEMES) TH = THEMES[t]; }
        if (strstr(cmd, "--demo")) {
            XH d; memset(&d, 0, sizeof d);
            int a = xh_add(&d, LT_CROSS); d.L[a].r = 255; d.L[a].g = 60; d.L[a].b = 60; d.L[a].w = 10; d.L[a].gap = 6; d.L[a].rot = 12;
            a = xh_add(&d, LT_ELLIPSE); d.L[a].w = d.L[a].h = 34; d.L[a].stroke = 2; d.L[a].r = 80; d.L[a].g = 200; d.L[a].b = 255;
            a = xh_add(&d, LT_RECT); d.L[a].w = 6; d.L[a].h = 6; d.L[a].radius = 1;
            a = xh_add(&d, LT_TRI); d.L[a].w = 12; d.L[a].h = 10; d.L[a].y = -24; d.L[a].r = 255; d.L[a].g = 230; d.L[a].b = 0;
            uint32_t *px = (uint32_t *)malloc(32 * 32 * 4);
            for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { float dd = hypotf(x - 15.5f, y - 15.5f); int c = (dd < 14 && dd > 9) ? 255 : 0; px[y * 32 + x] = (c << 24) | (c << 16) | (c << 8) | c; }
            int im = xh_img_add(32, 32, px);
            a = xh_add(&d, LT_IMAGE); d.L[a].img = im; d.L[a].w = d.L[a].h = 30; d.L[a].x = 40; d.L[a].y = 30; d.L[a].ol_on = 0; d.L[a].tint = 2; d.L[a].r = 0; d.L[a].g = 255; d.L[a].b = 120;
            X = d; if (g_sel >= X.n) g_sel = X.n - 1;
            lib[NBUILTIN].used = 1; lib[NBUILTIN].x = d; snprintf(lib[NBUILTIN].name, 32, "My demo emblem");
            lib_sel = NBUILTIN;
        }
        menu_a = 1; menu_target = 1; page_t = 1; tab_x = 190 + g_page * 96.0f;
        {   /* developer test hooks: --click=x,y (logical px) then --keys=text  ('#' = Enter, '<' = Backspace) */
            float cx_ = -1, cy_ = -1; char keys[64] = ""; const char *pc = strstr(cmd, "--click="), *pk = strstr(cmd, "--keys=");
            if (pc) sscanf(pc + 8, "%f,%f", &cx_, &cy_);
            if (pk) sscanf(pk + 7, "%63s", keys);
            for (int i = 0; i < 14; i++) {
                if (i == 7 && strstr(cmd, "--hk")) hk_capture = 0;
                if (i == 3 && cx_ >= 0) { mx = cx_; my = cy_; m_down = 1; m_press = 1; }
                if (i == 4 && cx_ >= 0) { m_down = 0; m_release = 1; }
                if (i == 8) for (const char *k = keys; *k && chq_n < 31; k++) chq[chq_n++] = *k == '#' ? 13 : (*k == '<' ? 8 : *k);
                ui_render();
            }
        }
        write_bmp(path);
        return 0;
    }

    if (!hk_register()) say("A hotkey is in use by another app - rebind it in Settings");

    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid; g_nid.hWnd = g_ov; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY; g_nid.hIcon = g_icon_sm;
    strcpy(g_nid.szTip, "CrosshairFree");
    Shell_NotifyIconA(NIM_ADD, &g_nid);

    ov_visibility(); ov_rev = rev;
    SetTimer(g_ov, ID_TOPMOST, 3000, NULL);
    ui_show(1);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (mtx) CloseHandle(mtx);
    return 0;
}
