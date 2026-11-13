/*
 * CrosshairFree - xh.h
 * Layer-based crosshair model + anti-aliased software renderer + code importers/exporters.
 * No OS dependencies: plain C99, so it can be unit-tested anywhere.
 *
 * License: MIT
 */
#ifndef XH_H
#define XH_H

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XH_MAXL    16      /* max layers per crosshair        */
#define XH_MAXIMG  32      /* max PNGs kept in the image pool */
#define XH_IMGMAX  128     /* PNGs are downscaled to this     */

/* ------------------------------------------------------------------ model */
enum { LT_CROSS = 0, LT_RECT, LT_ELLIPSE, LT_TRI, LT_IMAGE, LT_COUNT };
static const char *xh_typename[LT_COUNT] = { "Cross", "Rectangle", "Ellipse", "Triangle", "PNG image" };

typedef struct { int w, h; uint32_t *px; } XImg;           /* premultiplied BGRA */
static XImg xh_img[XH_MAXIMG];

typedef struct {
    int   type, visible;
    char  name[24];
    int   r, g, b;                    /* fill colour (PNG: tint colour)        */
    float alpha;                      /* fill opacity 0..1                     */
    float x, y, rot;                  /* offset from crosshair centre, degrees */
    float w, h;                       /* size.  CROSS: w = arm length, h = arm thickness */
    float gap;                        /* CROSS: distance centre -> arm start   */
    float stroke;                     /* RECT/ELLIPSE/TRI: 0 = filled, >0 = hollow with this border width */
    float radius;                     /* RECT corner radius                    */
    int   arms;                       /* CROSS: bit0 right, bit1 bottom, bit2 left, bit3 top */
    int   ol_on; float ol_thick, ol_alpha; int ol_r, ol_g, ol_b;   /* outline */
    int   img, tint, lock;            /* PNG: pool index, 0 original / 1 tint / 2 flat colour, lock aspect */
} Layer;

typedef struct {
    int   n;
    Layer L[XH_MAXL];
    float off_x, off_y;               /* whole-crosshair offset from screen centre */
} XH;

static void xh_layer_init(Layer *l, int type)
{
    memset(l, 0, sizeof *l);
    l->type = type; l->visible = 1; l->r = l->g = l->b = 255; l->alpha = 1;
    l->arms = 15; l->ol_on = 1; l->ol_thick = 1; l->ol_alpha = 0.8f; l->img = -1; l->lock = 1;
    switch (type) {
    case LT_CROSS:   l->w = 8;  l->h = 2;  l->gap = 4; break;
    case LT_RECT:    l->w = 8;  l->h = 8;  break;
    case LT_ELLIPSE: l->w = 12; l->h = 12; break;
    case LT_TRI:     l->w = 12; l->h = 12; break;
    }
}
static void xh_unique_name(const XH *x, Layer *l, const char *base)
{
    for (int k = 1; k < 100; k++) {
        char nm[32]; snprintf(nm, sizeof nm, "%s %d", base, k);
        int clash = 0;
        for (int i = 0; i < x->n; i++) if (&x->L[i] != l && !strcmp(x->L[i].name, nm)) clash = 1;
        if (!clash) { snprintf(l->name, sizeof l->name, "%s", nm); return; }
    }
}
static int xh_add(XH *x, int type)
{
    if (x->n >= XH_MAXL) return -1;
    Layer *l = &x->L[x->n];
    xh_layer_init(l, type);
    xh_unique_name(x, l, xh_typename[type]);
    return x->n++;
}
static void xh_del(XH *x, int i)
{
    if (i < 0 || i >= x->n) return;
    memmove(&x->L[i], &x->L[i + 1], (size_t)(x->n - i - 1) * sizeof(Layer));
    x->n--;
    memset(&x->L[x->n], 0, sizeof(Layer));
}
static int xh_dup(XH *x, int i)
{
    if (i < 0 || i >= x->n || x->n >= XH_MAXL) return -1;
    memmove(&x->L[i + 2], &x->L[i + 1], (size_t)(x->n - i - 1) * sizeof(Layer));
    x->L[i + 1] = x->L[i];
    xh_unique_name(x, &x->L[i + 1], xh_typename[x->L[i].type]);
    x->L[i + 1].x += 4; x->L[i + 1].y += 4;
    x->n++;
    return i + 1;
}
static int xh_move(XH *x, int i, int d)       /* d = +1 moves up the list (towards the top of the stack) */
{
    int j = i + d;
    if (i < 0 || i >= x->n || j < 0 || j >= x->n) return i;
    Layer t = x->L[i]; x->L[i] = x->L[j]; x->L[j] = t;
    return j;
}

/* ------------------------------------------------------------ image pool */
static int xh_img_add(int w, int h, uint32_t *px)     /* takes ownership of px */
{
    for (int i = 0; i < XH_MAXIMG; i++)
        if (!xh_img[i].px) { xh_img[i].w = w; xh_img[i].h = h; xh_img[i].px = px; return i; }
    free(px);
    return -1;
}
static void xh_img_prune(const XH *const *list, int n)   /* frees images no crosshair uses */
{
    unsigned char used[XH_MAXIMG]; memset(used, 0, sizeof used);
    for (int k = 0; k < n; k++)
        for (int i = 0; i < list[k]->n; i++) {
            const Layer *l = &list[k]->L[i];
            if (l->type == LT_IMAGE && l->img >= 0 && l->img < XH_MAXIMG) used[l->img] = 1;
        }
    for (int i = 0; i < XH_MAXIMG; i++)
        if (!used[i] && xh_img[i].px) { free(xh_img[i].px); xh_img[i].px = NULL; xh_img[i].w = xh_img[i].h = 0; }
}

/* --------------------------------------------------------------- renderer */
static inline float xh_cov(float sd) { float c = 0.5f - sd; return c < 0 ? 0 : (c > 1 ? 1 : c); }
static inline float xh_c01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static inline float sd_box(float px, float py, float hw, float hh)
{
    float dx = fabsf(px) - hw, dy = fabsf(py) - hh;
    float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    float in = dx > dy ? dx : dy; if (in > 0) in = 0;
    return sqrtf(ox * ox + oy * oy) + in;
}
static inline float sd_rbox(float px, float py, float hw, float hh, float r)
{
    float m = hw < hh ? hw : hh; if (r > m) r = m; if (r < 0) r = 0;
    float qx = fabsf(px) - (hw - r), qy = fabsf(py) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy; if (in > 0) in = 0;
    return sqrtf(ox * ox + oy * oy) + in - r;
}
static inline float sd_ellipse(float px, float py, float rx, float ry)
{
    if (rx <= 0 || ry <= 0) return 1e9f;
    if (fabsf(rx - ry) < 1e-3f) return sqrtf(px * px + py * py) - rx;
    float k0 = sqrtf((px / rx) * (px / rx) + (py / ry) * (py / ry));
    float k1 = sqrtf((px / (rx * rx)) * (px / (rx * rx)) + (py / (ry * ry)) * (py / (ry * ry)));
    if (k1 < 1e-9f) return -(rx < ry ? rx : ry);
    return k0 * (k0 - 1.0f) / k1;
}
static inline float sd_tri(float px, float py, float w, float h)   /* isosceles, apex up, centred on bbox */
{
    float qx = w * 0.5f, qy = h;
    px = fabsf(px); py = py + h * 0.5f;
    float t = xh_c01((px * qx + py * qy) / (qx * qx + qy * qy));
    float ax = px - qx * t, ay = py - qy * t;
    float bt = xh_c01(px / qx), bx = px - qx * bt, by = py - qy;
    float d0 = ax * ax + ay * ay, d1 = -(px * qy - py * qx);
    float e0 = bx * bx + by * by, e1 = -(py - qy);
    float dx = d0 < e0 ? d0 : e0, dy = d1 < e1 ? d1 : e1;
    float s = dy > 0 ? 1.0f : (dy < 0 ? -1.0f : 0.0f);
    return -sqrtf(dx) * s;
}

typedef struct { float w, h, gap, stroke, radius, olT; } XP;       /* zoom-scaled layer params */

static inline float xh_layer_sd(const Layer *l, const XP *p, float u, float v)
{
    float sd;
    switch (l->type) {
    case LT_CROSS: {
        float hl = p->w * 0.5f, ht = p->h * 0.5f, c = p->gap + hl;
        sd = 1e9f;
        if (l->arms & 1) { float d = sd_box(u - c, v, hl, ht);  if (d < sd) sd = d; }
        if (l->arms & 2) { float d = sd_box(v - c, -u, hl, ht); if (d < sd) sd = d; }
        if (l->arms & 4) { float d = sd_box(-u - c, -v, hl, ht); if (d < sd) sd = d; }
        if (l->arms & 8) { float d = sd_box(-v - c, u, hl, ht); if (d < sd) sd = d; }
        return sd;
    }
    case LT_RECT:    sd = sd_rbox(u, v, p->w * 0.5f, p->h * 0.5f, p->radius); break;
    case LT_ELLIPSE: sd = sd_ellipse(u, v, p->w * 0.5f, p->h * 0.5f); break;
    default:         sd = sd_tri(u, v, p->w, p->h); break;
    }
    if (p->stroke > 0) sd = fabsf(sd + p->stroke * 0.5f) - p->stroke * 0.5f;
    return sd;
}

static void xh_sample(const XImg *im, float tx, float ty, float *o)    /* premultiplied B,G,R,A (0..255) */
{
    int x0 = (int)floorf(tx), y0 = (int)floorf(ty);
    float fx = tx - x0, fy = ty - y0;
    o[0] = o[1] = o[2] = o[3] = 0;
    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
        int xx = x0 + i, yy = y0 + j;
        if (xx < 0 || yy < 0 || xx >= im->w || yy >= im->h) continue;
        float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        uint32_t p = im->px[yy * im->w + xx];
        o[0] += (p & 255) * wgt; o[1] += ((p >> 8) & 255) * wgt;
        o[2] += ((p >> 16) & 255) * wgt; o[3] += ((p >> 24) & 255) * wgt;
    }
}

/* half-extents of a layer (unrotated, without outline) */
static void xh_layer_half(const Layer *l, float *hw, float *hh)
{
    if (l->type == LT_CROSS) {
        float reach = l->gap + l->w; if (reach < 0) reach = 0;
        float r = sqrtf(reach * reach + l->h * l->h * 0.25f);
        *hw = *hh = r;
    } else { *hw = l->w * 0.5f; *hh = l->h * 0.5f; }
}
static float xh_layer_radius(const Layer *l)
{
    float hw, hh; xh_layer_half(l, &hw, &hh);
    float r = sqrtf(hw * hw + hh * hh);
    if (l->type != LT_IMAGE && l->ol_on) r += l->ol_thick;
    return r + 2;
}
static float xh_extent(const XH *x)      /* furthest distance from the centre any layer reaches */
{
    float e = 6;
    for (int i = 0; i < x->n; i++) {
        if (!x->L[i].visible) continue;
        float d = sqrtf(x->L[i].x * x->L[i].x + x->L[i].y * x->L[i].y) + xh_layer_radius(&x->L[i]);
        if (d > e) e = d;
    }
    return e;
}

/* Renders into a premultiplied BGRA buffer (W*H uint32), centred on the buffer. */
static void xh_render(const XH *x, uint32_t *buf, int W, int H, float z)
{
    memset(buf, 0, (size_t)W * H * 4);
    for (int li = 0; li < x->n; li++) {
        const Layer *l = &x->L[li];
        if (!l->visible || l->alpha <= 0) continue;
        if (l->type == LT_IMAGE && (l->img < 0 || l->img >= XH_MAXIMG || !xh_img[l->img].px)) continue;

        XP p; p.w = l->w * z; p.h = l->h * z; p.gap = l->gap * z; p.stroke = l->stroke * z;
        p.radius = l->radius * z; p.olT = (l->ol_on && l->type != LT_IMAGE) ? l->ol_thick * z : 0;
        if (p.w <= 0 || p.h <= 0) continue;

        /* pixel-grid snapping so odd widths stay crisp */
        float hx = 0, hy = 0, gapadj = 0;
        if (l->rot == 0 && l->type != LT_IMAGE && l->type != LT_TRI) {
            if (l->type == LT_CROSS) {
                int odd = ((int)floorf(p.h + 0.5f)) & 1;
                hx = hy = odd ? 0.5f : 0.0f; gapadj = hx;
            } else {
                hx = (((int)floorf(p.w + 0.5f)) & 1) ? 0.5f : 0.0f;
                hy = (((int)floorf(p.h + 0.5f)) & 1) ? 0.5f : 0.0f;
            }
        }
        float cx = W * 0.5f + l->x * z + hx, cy = H * 0.5f + l->y * z + hy;
        XP q = p; q.gap += gapadj;

        float R = xh_layer_radius(l) * z;
        int x0 = (int)floorf(cx - R) - 1, x1 = (int)ceilf(cx + R) + 1;
        int y0 = (int)floorf(cy - R) - 1, y1 = (int)ceilf(cy + R) + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;

        float ang = l->rot * 0.01745329252f, ca = cosf(ang), sa = sinf(ang);
        const XImg *im = (l->type == LT_IMAGE) ? &xh_img[l->img] : NULL;
        float taps = 1, tapo = 0;
        if (im) { float s = (float)im->w / p.w; if (s > 1.5f) { taps = 4; tapo = 0.25f * s; } }

        for (int py = y0; py < y1; py++) {
            for (int px = x0; px < x1; px++) {
                float X = px + 0.5f - cx, Y = py + 0.5f - cy;
                float u = X * ca + Y * sa, v = -X * sa + Y * ca;
                float sR, sG, sB, sA;                         /* layer contribution, premultiplied 0..255 */

                if (im) {
                    float acc[4] = { 0, 0, 0, 0 }, o[4];
                    int nt = (int)taps;
                    for (int t = 0; t < nt; t++) {
                        float ox = (nt > 1) ? ((t & 1) ? tapo : -tapo) : 0, oy = (nt > 1) ? ((t & 2) ? tapo : -tapo) : 0;
                        float tx = (u / p.w + 0.5f) * im->w - 0.5f + ox, ty = (v / p.h + 0.5f) * im->h - 0.5f + oy;
                        xh_sample(im, tx, ty, o);
                        acc[0] += o[0]; acc[1] += o[1]; acc[2] += o[2]; acc[3] += o[3];
                    }
                    float inv = 1.0f / nt;
                    float b = acc[0] * inv, g = acc[1] * inv, r = acc[2] * inv, a = acc[3] * inv;
                    if (a <= 0.01f) continue;
                    if (l->tint == 1) { r *= l->r / 255.0f; g *= l->g / 255.0f; b *= l->b / 255.0f; }
                    else if (l->tint == 2) { float k = a / 255.0f; r = l->r * k; g = l->g * k; b = l->b * k; }
                    sR = r * l->alpha; sG = g * l->alpha; sB = b * l->alpha; sA = a * l->alpha;
                } else {
                    float sd = xh_layer_sd(l, &q, u, v);
                    if (sd > q.olT + 1.0f) continue;
                    float cF = xh_cov(sd) * l->alpha, cO = 0;
                    if (q.olT > 0) {
                        cO = xh_cov(sd - q.olT) - xh_cov(sd);
                        if (cO < 0) cO = 0;
                        cO *= l->ol_alpha;
                    }
                    float A = cF + cO * (1.0f - cF);
                    sR = l->r * cF + l->ol_r * cO * (1.0f - cF);
                    sG = l->g * cF + l->ol_g * cO * (1.0f - cF);
                    sB = l->b * cF + l->ol_b * cO * (1.0f - cF);
                    sA = A * 255.0f;
                }
                uint32_t d = buf[py * W + px];
                float k = 1.0f - sA / 255.0f;
                float oA = sA + (float)(d >> 24) * k, oR = sR + (float)((d >> 16) & 255) * k;
                float oG = sG + (float)((d >> 8) & 255) * k, oB = sB + (float)(d & 255) * k;
                int ia = (int)(oA + 0.5f), ir = (int)(oR + 0.5f), ig = (int)(oG + 0.5f), ib = (int)(oB + 0.5f);
                if (ia > 255) ia = 255;
                if (ir > ia) ir = ia;
                if (ig > ia) ig = ia;
                if (ib > ia) ib = ia;
                buf[py * W + px] = ((uint32_t)ia << 24) | ((uint32_t)ir << 16) | ((uint32_t)ig << 8) | (uint32_t)ib;
            }
        }
    }
}

/* ------------------------------------------------ simple (single-style) model
 * The game-code importers fill this flat structure; xh_from_simple() turns it into layers. */
typedef struct {
    int   r, g, b; float opacity, rotation; int tstyle;
    int   in_on;  float in_len, in_thick, in_gap, in_alpha;
    int   out_on; float out_len, out_thick, out_gap, out_alpha;
    int   dot_on; float dot_size, dot_alpha; int dot_round;
    int   ring_on; float ring_r, ring_thick, ring_alpha;
    int   ol_on;  float ol_thick, ol_alpha; int ol_r, ol_g, ol_b;
} XHS;

static void xhs_default(XHS *x)
{
    memset(x, 0, sizeof *x);
    x->r = 0; x->g = 255; x->b = 102; x->opacity = 1.0f;
    x->in_on = 1;  x->in_len = 6;  x->in_thick = 2; x->in_gap = 4; x->in_alpha = 1;
    x->out_on = 0; x->out_len = 3; x->out_thick = 2; x->out_gap = 10; x->out_alpha = 0.35f;
    x->dot_size = 2; x->dot_alpha = 1;
    x->ring_r = 14; x->ring_thick = 1.5f; x->ring_alpha = 1;
    x->ol_on = 1; x->ol_thick = 1; x->ol_alpha = 0.8f;
}

static void xh_from_simple(const XHS *s, XH *o)
{
    float ox = o->off_x, oy = o->off_y;
    memset(o, 0, sizeof *o); o->off_x = ox; o->off_y = oy;
    int id;
#define COMMON(l, a) do { l->r = s->r; l->g = s->g; l->b = s->b; l->alpha = (a) * s->opacity; \
        l->ol_on = s->ol_on; l->ol_thick = s->ol_thick; l->ol_alpha = s->ol_alpha; \
        l->ol_r = s->ol_r; l->ol_g = s->ol_g; l->ol_b = s->ol_b; } while (0)
    if (s->in_on && s->in_len > 0 && s->in_thick > 0 && (id = xh_add(o, LT_CROSS)) >= 0) {
        Layer *l = &o->L[id]; snprintf(l->name, sizeof l->name, "Inner lines");
        l->w = s->in_len; l->h = s->in_thick; l->gap = s->in_gap; l->rot = s->rotation;
        l->arms = s->tstyle ? 7 : 15; COMMON(l, s->in_alpha);
    }
    if (s->out_on && s->out_len > 0 && s->out_thick > 0 && (id = xh_add(o, LT_CROSS)) >= 0) {
        Layer *l = &o->L[id]; snprintf(l->name, sizeof l->name, "Outer lines");
        l->w = s->out_len; l->h = s->out_thick; l->gap = s->out_gap; l->rot = s->rotation;
        l->arms = s->tstyle ? 7 : 15; COMMON(l, s->out_alpha);
    }
    if (s->ring_on && (id = xh_add(o, LT_ELLIPSE)) >= 0) {
        Layer *l = &o->L[id]; snprintf(l->name, sizeof l->name, "Ring");
        l->w = l->h = s->ring_r * 2 + s->ring_thick; l->stroke = s->ring_thick; COMMON(l, s->ring_alpha);
    }
    if (s->dot_on && s->dot_size > 0 && (id = xh_add(o, s->dot_round ? LT_ELLIPSE : LT_RECT)) >= 0) {
        Layer *l = &o->L[id]; snprintf(l->name, sizeof l->name, "Dot");
        l->w = l->h = s->dot_size; COMMON(l, s->dot_alpha);
    }
#undef COMMON
}

/* best-effort reverse mapping, used for exporting a Valorant code */
static void xh_to_simple(const XH *x, XHS *s)
{
    xhs_default(s);
    s->in_on = s->out_on = s->dot_on = s->ring_on = s->ol_on = 0;
    int crosses = 0, gotc = 0;
    for (int i = 0; i < x->n; i++) {
        const Layer *l = &x->L[i];
        if (!l->visible) continue;
        if (l->type == LT_CROSS && crosses < 2) {
            if (!gotc) {
                s->r = l->r; s->g = l->g; s->b = l->b; gotc = 1;
                s->ol_on = l->ol_on; s->ol_thick = l->ol_thick; s->ol_alpha = l->ol_alpha;
                s->ol_r = l->ol_r; s->ol_g = l->ol_g; s->ol_b = l->ol_b;
            }
            if (crosses == 0) { s->in_on = 1; s->in_len = l->w; s->in_thick = l->h; s->in_gap = l->gap; s->in_alpha = l->alpha; }
            else              { s->out_on = 1; s->out_len = l->w; s->out_thick = l->h; s->out_gap = l->gap; s->out_alpha = l->alpha; }
            crosses++;
        } else if ((l->type == LT_RECT || l->type == LT_ELLIPSE) && l->stroke == 0 && l->w <= 16 && !s->dot_on) {
            s->dot_on = 1; s->dot_size = l->w; s->dot_alpha = l->alpha;
            if (!gotc) { s->r = l->r; s->g = l->g; s->b = l->b; gotc = 1; }
        }
    }
}

/* ---------------------------------------------------------------- presets */
#define XH_NPRESETS 6
static const char *xh_preset_names[XH_NPRESETS] = { "Classic", "Tiny Dot", "Gap Cross", "Ring Dot", "T-Style", "Cyan Plus" };

static void xh_make_preset(XH *o, int i)
{
    XHS s; xhs_default(&s);
    switch (i) {
    case 1: s.in_on = 0; s.dot_on = 1; s.dot_size = 3; s.r = s.g = s.b = 255; break;
    case 2: s.r = s.g = s.b = 255; s.in_len = 7; s.in_gap = 7; break;
    case 3: s.ring_on = 1; s.ring_r = 12; s.dot_on = 1; s.dot_size = 2; s.in_on = 0; s.r = 0; s.g = 229; s.b = 255; break;
    case 4: s.tstyle = 1; s.in_len = 8; s.in_gap = 3; s.r = 255; s.g = 230; s.b = 0; break;
    case 5: s.r = 0; s.g = 229; s.b = 255; s.in_len = 5; s.in_gap = 2; s.dot_on = 1; s.dot_size = 2; break;
    }
    xh_from_simple(&s, o);
}

/* ------------------------------------------------------ CS2 / CS:GO codes */
/* Layouts: https://github.com/akiver/csgo-sharecode/blob/main/doc/code_layout.md */
static const char XH_DICT[] = "ABCDEFGHJKLMNOPQRSTUVWXYZabcdefhijkmnopqrstuvwxyz23456789";

static int xh_b57(const char *s, int n, uint8_t *out, int nb)
{
    memset(out, 0, (size_t)nb);
    for (int i = n - 1; i >= 0; i--) {
        if (!s[i]) return 0;
        const char *p = strchr(XH_DICT, s[i]);
        if (!p) return 0;
        int carry = (int)(p - XH_DICT);
        for (int j = nb - 1; j >= 0; j--) {
            int v = out[j] * 57 + carry;
            out[j] = (uint8_t)(v & 255);
            carry = v >> 8;
        }
        if (carry) return 0;
    }
    return 1;
}

typedef struct {
    int style, dot, tstyle, recoil, r, g, b, a, outline_mode, orr, og, ob, oa;
    float gap, len, thick; int pixel_units, ref_h; float outline_thick;
} XHCS;

static void xh_from_cs(const XHCS *c, XHS *o, int screen_h)
{
    float scale, gap, len, th;
    xhs_default(o);
    if (c->pixel_units) {
        scale = (c->ref_h > 0) ? (float)screen_h / (float)c->ref_h : 1.0f;
        gap = c->gap * scale; len = c->len * scale; th = c->thick * scale;
    } else {
        /* legacy units are relative to a 600px tall reference; gap has a +4 base (approximation) */
        scale = (float)screen_h / 600.0f;
        gap = (c->gap + 4.0f) * scale; len = c->len * scale; th = c->thick * scale;
        if (th < 1) th = 1;
    }
    int circle = (c->style == 1 || c->style == 3), dotonly = (c->style == 6);
    o->r = c->r; o->g = c->g; o->b = c->b;
    o->opacity = c->a / 255.0f; o->tstyle = c->tstyle;
    o->in_on = !(circle || dotonly);
    o->in_len = len; o->in_thick = th; o->in_gap = gap; o->in_alpha = 1;
    o->dot_on = c->dot || dotonly; o->dot_size = th < 1 ? 1 : th; o->dot_alpha = 1;
    if (circle) { o->ring_on = 1; o->ring_r = fmaxf(4.0f, len + gap); o->ring_thick = th; }
    o->ol_on = c->outline_mode != 0;
    o->ol_thick = (c->outline_mode == 2) ? 0.5f : (c->outline_thick > 0 ? c->outline_thick * scale : scale);
    o->ol_alpha = c->oa / 255.0f;
    o->ol_r = c->orr; o->ol_g = c->og; o->ol_b = c->ob;
}

static int xh_import_cs(const char *in, XHS *o, int screen_h, const char **fmt)
{
    char s[128]; int n = 0;
    for (const char *p = in; *p && n < 120; p++)
        if (*p != '-' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') s[n++] = *p;
    s[n] = 0;
    uint8_t b[32]; XHCS c; memset(&c, 0, sizeof c);

    if (n == 29 && !strncmp(s, "CSGO", 4)) {                       /* legacy container, 18 bytes */
        if (!xh_b57(s + 4, 25, b, 18)) return 0;
        unsigned sum = 0; for (int i = 1; i < 18; i++) sum += b[i];
        if ((sum & 255) != b[0]) return 0;
        int ver = b[1];
        if (ver == 1) {
            static const int col[6][3] = {{250,50,50},{50,250,50},{250,250,50},{50,50,250},{0,0,0},{50,250,250}};
            c.gap = (int8_t)b[2] / 10.0f; c.outline_thick = b[3] / 2.0f;
            c.r = b[4]; c.g = b[5]; c.b = b[6]; c.a = b[7];
            int ci = b[10] & 7;
            if (ci != 4 && ci < 6) { c.r = col[ci][0]; c.g = col[ci][1]; c.b = col[ci][2]; }
            c.outline_mode = (b[10] & 8) ? 1 : 0;
            c.thick = b[12] / 10.0f; c.dot = (b[13] >> 4) & 1;
            if (!((b[13] >> 6) & 1)) c.a = 255;
            c.tstyle = (b[13] >> 7) & 1; c.len = b[14] / 10.0f;
            c.orr = c.og = c.ob = 0; c.oa = 255; c.pixel_units = 0; c.style = 4;
            if (fmt) *fmt = "CS:GO / CS2 (legacy code)";
        } else if (ver == 3 || ver == 4) {
            uint32_t f = b[10] | (b[11] << 8) | (b[12] << 16) | ((uint32_t)b[13] << 24);
            c.style = b[2] & 15; c.dot = (b[2] >> 6) & 1; c.tstyle = (b[2] >> 7) & 1;
            c.r = b[3]; c.g = b[4]; c.b = b[5]; c.a = b[6]; c.gap = b[7]; c.len = b[8];
            c.thick = (float)((f >> 23) & 31);
            c.outline_mode = (ver == 3) ? (((b[2] >> 5) & 1) ? 1 : 0) : (int)((f >> 28) & 3);
            c.orr = c.og = c.ob = 0; c.oa = 255; c.outline_thick = 1;
            c.pixel_units = 1; c.ref_h = b[14] | (b[15] << 8);
            if (fmt) *fmt = (ver == 3) ? "CS2 (Sep 2026 code)" : "CS2 (Sep 2026 code v4)";
        } else return 0;
    } else if (n == 46 && s[0] == 'C' && s[1] == 'S') {          /* cs2 container, 32 bytes */
        if (!xh_b57(s + 2, 44, b, 32)) return 0;
        unsigned sum = 0; for (int i = 1; i < 32; i++) sum += b[i];
        if ((sum & 255) != b[0] || b[1] != 1) return 0;
        c.ref_h = b[2] | (b[3] << 8);
        if (c.ref_h == 0) return 0;
        c.style = b[4] & 15; c.dot = (b[4] >> 6) & 1; c.tstyle = (b[4] >> 7) & 1;
        c.r = b[5]; c.g = b[6]; c.b = b[7]; c.a = b[8];
        c.orr = b[9]; c.og = b[10]; c.ob = b[11]; c.oa = b[12];
        c.thick = b[13]; c.outline_mode = b[14]; c.gap = (int8_t)b[15]; c.len = b[16];
        c.outline_thick = 1; c.pixel_units = 1;
        if (fmt) *fmt = "CS2";
    } else return 0;
    xh_from_cs(&c, o, screen_h);
    return 1;
}

/* ------------------------------------------------------------- Valorant */
static int xh_hex(const char *s, int *r, int *g, int *b)
{
    int n = (int)strlen(s);
    if (n != 6 && n != 8) return 0;
    for (int i = 0; i < n; i++) if (!strchr("0123456789abcdefABCDEF", s[i])) return 0;
    unsigned v; char t[3] = { 0, 0, 0 };
    t[0] = s[0]; t[1] = s[1]; sscanf(t, "%x", &v); *r = (int)v;
    t[0] = s[2]; t[1] = s[3]; sscanf(t, "%x", &v); *g = (int)v;
    t[0] = s[4]; t[1] = s[5]; sscanf(t, "%x", &v); *b = (int)v;
    return 1;
}

static int xh_import_valorant(const char *in, XHS *o)
{
    static const int vc[8][3] = {{255,255,255},{0,255,0},{127,255,0},{223,255,0},{255,255,0},{0,255,255},{255,0,255},{255,0,0}};
    char buf[1024]; strncpy(buf, in, 1023); buf[1023] = 0;
    char *tok[256]; int n = 0;
    for (char *p = strtok(buf, ";\r\n \t"); p && n < 256; p = strtok(NULL, ";\r\n \t")) tok[n++] = p;
    if (n < 3) return 0;
    xhs_default(o);
    o->r = o->g = o->b = 255;
    o->in_on = 1; o->in_len = 6; o->in_thick = 2; o->in_gap = 3; o->in_alpha = 1;
    o->out_on = 1; o->out_len = 2; o->out_thick = 2; o->out_gap = 10; o->out_alpha = 0.35f;
    o->dot_on = 0; o->dot_size = 2; o->dot_alpha = 1;
    o->ol_on = 0; o->ol_thick = 1; o->ol_alpha = 0.5f; o->ol_r = o->ol_g = o->ol_b = 0;
    int sec = 0, have_p = 0, have_c = 0, cidx = 0, have_u = 0, ur = 255, ug = 255, ub = 255;
    for (int i = 0; i < n;) {
        const char *k = tok[i];
        if (i == 0 && !strcmp(k, "0")) { i++; continue; }
        if (k[1] == 0 && (k[0] == 'P' || k[0] == 'A' || k[0] == 'S')) { sec = k[0]; if (sec == 'P') have_p = 1; i++; continue; }
        if (i + 1 >= n) break;
        const char *v = tok[i + 1]; i += 2;
        if (sec != 'P') continue;
        float f = (float)atof(v);
        if      (!strcmp(k, "c")) { cidx = atoi(v); have_c = 1; }
        else if (!strcmp(k, "u")) { if (xh_hex(v, &ur, &ug, &ub)) have_u = 1; }
        else if (!strcmp(k, "o")) { o->ol_alpha = f; o->ol_on = f > 0; }
        else if (!strcmp(k, "t")) { o->ol_thick = f; }
        else if (!strcmp(k, "d")) { o->dot_on = atoi(v) != 0; }
        else if (!strcmp(k, "z")) { o->dot_size = f; }
        else if (!strcmp(k, "a")) { o->dot_alpha = f; }
        else if (!strcmp(k, "0l")) { o->in_len = f; }
        else if (!strcmp(k, "0t")) { o->in_thick = f; }
        else if (!strcmp(k, "0o")) { o->in_gap = f; }
        else if (!strcmp(k, "0a")) { o->in_alpha = f; }
        else if (!strcmp(k, "0b")) { o->in_on = atoi(v) != 0; }
        else if (!strcmp(k, "1l")) { o->out_len = f; }
        else if (!strcmp(k, "1t")) { o->out_thick = f; }
        else if (!strcmp(k, "1o")) { o->out_gap = f; }
        else if (!strcmp(k, "1a")) { o->out_alpha = f; }
        else if (!strcmp(k, "1b")) { o->out_on = atoi(v) != 0; }
    }
    if (!have_p) return 0;
    if (have_u && (!have_c || cidx == 8)) { o->r = ur; o->g = ug; o->b = ub; }
    else if (cidx >= 0 && cidx < 8) { o->r = vc[cidx][0]; o->g = vc[cidx][1]; o->b = vc[cidx][2]; }
    return 1;
}

static void xh_export_valorant(const XH *x, char *out, size_t n)
{
    XHS s; xh_to_simple(x, &s);
    snprintf(out, n,
        "0;P;c;8;u;%02X%02X%02XFF;o;%g;t;%g;d;%d;z;%g;a;%g;"
        "0b;%d;0l;%g;0t;%g;0o;%g;0a;%g;1b;%d;1l;%g;1t;%g;1o;%g;1a;%g",
        s.r, s.g, s.b, s.ol_on ? s.ol_alpha : 0.0f, s.ol_thick,
        s.dot_on, s.dot_size, s.dot_alpha,
        s.in_on, s.in_len, s.in_thick, s.in_gap, s.in_alpha,
        s.out_on, s.out_len, s.out_thick, s.out_gap, s.out_alpha);
}

/* -------------------------------------------------------- auto-detection */
/* Add new games here: parse into an XHS and return 1 on success. */
static int xh_import_auto(const char *in, XH *o, int screen_h, const char **fmt)
{
    XHS s;
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') in++;
    if (in[0] == 'C' && in[1] == 'S') { if (!xh_import_cs(in, &s, screen_h, fmt)) return 0; }
    else if (strchr(in, ';') && xh_import_valorant(in, &s)) { if (fmt) *fmt = "Valorant"; }
    else return 0;
    xh_from_simple(&s, o);
    return 1;
}

#endif
