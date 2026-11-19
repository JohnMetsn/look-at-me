// Look at me! — per-pixel eye renderer (integer / fixed point only: apps build for cortex-m3 soft float).
// Port of concept/eye-sketches-v1.html. Units: Q8 pixels for lid curves, Q12 for sphere coordinates.
#include "eye.h"
#include <math.h>

#define TEX 96
#define RI 1761                 // iris radius on the unit sphere, 0.43 in Q12
#define PR0 115                 // texture rest pupil 0.45 (Q8)

static uint8_t *s_blob;         // resource EYE: iris | veins | noise
static const uint8_t *s_iris, *s_vein, *s_noise;

static Settings s_set;
static uint8_t s_lut1[64][3], s_lut2[64][3];
static uint8_t s_colon;

// LUTs
static uint16_t s_sqrt_pz[1024];   // pz (Q12) for d2 >> 14
static uint16_t s_tsq[1024];       // t (Q8) for t^2 in 1/1024
static uint8_t s_exp[256];         // exp(-i/32) Q8
static uint8_t s_fall[256];        // night skin light falloff for de^2 = i/64
static float s_bump_u[65], s_bump_l[65], s_bump_b[65];

// per column statics
static int16_t s_u8[SCR_W];
static int32_t s_yc[SCR_W], s_a47[SCR_W], s_a10[SCR_W], s_a22[SCR_W], s_a24[SCR_W], s_a04[SCR_W], s_a03[SCR_W];
static int32_t s_crease[SCR_W], s_lob[SCR_W], s_px[SCR_W];
static uint16_t s_cw[SCR_W], s_dx2[SCR_W], s_cdiv[SCR_W];
static int16_t s_dyn0[SCR_W], s_dyn1[SCR_W];  // rows that change with the eye state, per column
// per frame
static int32_t s_up[SCR_W], s_lo[SCR_W];

static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
static const uint8_t SKIN_RAMP[4] = {0xC0, 0xD0, 0xE5, 0xFA};    // 000000 550000 AA5555 FFAAAA
// Black surround: the dark brown step becomes black, so only the lids show (owner photo, 2026-11-19)
static const uint8_t SKIN_RAMP_BLACK[4] = {0xC0, 0xC0, 0xE5, 0xFA};
static const uint8_t SCLERA_RAMP[4] = {0xD5, 0xE5, 0xEA, 0xFF};  // 555555 AA5555 AAAAAA FFFFFF

static inline int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static inline int sm8(int t) { t = clampi(t, 0, 256); return (t * t * (768 - 2 * t)) >> 16; }
static inline uint8_t qc(int v, int off) { v = (v + off + 42) / 85; return (uint8_t)clampi(v, 0, 3); }
static inline uint8_t rgb_byte(int r, int g, int b, int off) {
  return (uint8_t)(0xC0 | (qc(r, off) << 4) | (qc(g, off) << 2) | qc(b, off));
}
static inline uint8_t ramp4(const uint8_t *ramp, int s, int bay, bool dith) {
  int p = s * 3;
  int idx = dith ? (p + bay * 16 + 8) >> 8 : (p + 128) >> 8;
  return ramp[clampi(idx, 0, 3)];
}

static float bump(float u, float p) {
  if (u <= 0 || u >= 1) return 0;
  float k = logf(0.5f) / logf(p);
  return powf(sinf(3.14159265f * powf(u, k)), 0.85f);
}
static float bump_tab(const float *t, float u) {
  if (u <= 0 || u >= 1) return 0;
  float f = u * 64; int i = (int)f; float r = f - i;
  return t[i] + (t[i + 1] - t[i]) * r;
}

bool eye_init(void) {
  ResHandle h = resource_get_handle(RESOURCE_ID_EYE);
  size_t n = resource_size(h);
  s_blob = malloc(n);
  if (!s_blob) return false;
  resource_load(h, s_blob, n);
  s_iris = s_blob;
  s_vein = s_blob + TEX * TEX;
  s_noise = s_blob + 2 * TEX * TEX;

  for (int i = 0; i < 1024; i++) {
    float d2 = (i + 0.5f) / 1024.0f;
    s_sqrt_pz[i] = (uint16_t)(sqrtf(1 - d2 > 0 ? 1 - d2 : 0) * 4096);
    s_tsq[i] = (uint16_t)(sqrtf(i / 1024.0f) * 256);
  }
  for (int i = 0; i < 256; i++) {
    s_exp[i] = (uint8_t)clampi((int)(expf(-i / 32.0f) * 256), 0, 255);
  }
  for (int i = 0; i <= 64; i++) { s_bump_u[i] = bump(i / 64.0f, 0.56f); s_bump_l[i] = bump(i / 64.0f, 0.40f); s_bump_b[i] = bump(i / 64.0f, 0.66f); }

  const float a = EYE_A, cx = EYE_CX, cy = EYE_CY, ex = cx + 0.02f * a;
  for (int x = 0; x < SCR_W; x++) {
    float xf = x + 0.5f, u = (xf - (cx - a)) / (2 * a), uc = u < 0 ? 0 : u > 1 ? 1 : u;
    float bu = bump(u, 0.56f), bl = bump(u, 0.40f);
    float yc = cy + a * (0.10f - 0.24f * uc);
    s_u8[x] = (int16_t)clampi((int)(u * 256), -2000, 2000);
    s_yc[x] = (int32_t)(yc * 256);
    s_a47[x] = (int32_t)(a * 0.47f * bu * 256);
    s_a10[x] = (int32_t)(a * 0.10f * bu * 256);
    s_a22[x] = (int32_t)(a * 0.22f * bl * 256);
    s_a24[x] = (int32_t)(a * 0.24f * bl * 256);
    s_a04[x] = (int32_t)(a * 0.04f * bl * 256);
    s_a03[x] = (int32_t)(a * 0.03f * bl * 256);
    s_crease[x] = s_yc[x] - s_a47[x] - (int32_t)(a * 0.26f * bu * 256);
    s_lob[x] = s_yc[x] + s_a24[x];
    float cw = 0;
    if (u > 0 && u < 1) { float t = (u - 0.12f) / 0.33f; t = t < 0 ? 0 : t > 1 ? 1 : t; cw = t * t * (3 - 2 * t) * bu; }
    s_cw[x] = (uint16_t)(cw * 256);
    float dx = (xf - cx) / (1.4f * a);
    s_dx2[x] = (uint16_t)clampi((int)(dx * dx * 256), 0, 60000);
    s_px[x] = (int32_t)((xf - ex) * 4096 / a);
    s_cdiv[x] = 0;
    // dynamic rows: lid bulge up to the crease, lash tips above the open lid, lashes hanging below the closed lid
    if (u > -0.03f && u < 1.15f) {
      float top1 = yc - a * 0.73f * bu, top2 = yc - a * (0.57f * bu + 0.25f);
      float top = top1 < top2 ? top1 : top2;
      float bot = yc + a * 0.22f * bl + a * 0.25f;
      s_dyn0[x] = (int16_t)(top - 2);
      s_dyn1[x] = (int16_t)(bot + 3);
    } else { s_dyn0[x] = 0; s_dyn1[x] = 0; }
  }
  return true;
}

void eye_deinit(void) { free(s_blob); s_blob = NULL; }

static void build_lut(uint8_t lut[64][3], uint32_t c) {
  float cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
  for (int i = 0; i < 64; i++) {
    float v = i / 63.0f, k[3];
    float base[3] = {cr, cg, cb};
    for (int j = 0; j < 3; j++) {
      float D = base[j] * 0.2f, L = base[j] + (255 - base[j]) * 0.55f;
      k[j] = v < 0.5f ? D + (base[j] - D) * v * 2 : base[j] + (L - base[j]) * (v - 0.5f) * 2;
      lut[i][j] = (uint8_t)clampi((int)k[j], 0, 255);
    }
  }
}

void eye_apply_settings(const Settings *s) {
  s_set = *s;
  // skin light falloff; "dark" lets the lids rise out of a black surround (owner, 2026-11-19)
  for (int i = 0; i < 256; i++) {
    float de2 = i / 64.0f;
    float f = s->dark ? expf(-2.0f * de2 * de2) : expf(-de2 * 1.15f);
    s_fall[i] = (uint8_t)clampi((int)(f * 255), 0, 255);
  }
  build_lut(s_lut1, s->iris);
  build_lut(s_lut2, s->imode ? s->iris2 : s->iris);
  int r = (s->iris >> 16) & 255, g = (s->iris >> 8) & 255, b = s->iris & 255;
  s_colon = rgb_byte(r + ((255 - r) * 3) / 10, g + ((255 - g) * 3) / 10, b + ((255 - b) * 3) / 10, 0);
}
uint8_t eye_tint_colon(void) { return s_colon; }

void eye_band(int *y0, int *y1) {
  int a = SCR_H, b = 0;
  for (int x = 0; x < SCR_W; x++) {
    if (s_dyn1[x] <= s_dyn0[x]) continue;
    if (s_dyn0[x] < a) a = s_dyn0[x];
    if (s_dyn1[x] > b) b = s_dyn1[x];
  }
  *y0 = a < 0 ? 0 : a;
  *y1 = b > SCR_H ? SCR_H : b;
}

static void lids_frame(const EyeState *st) {
  for (int x = 0; x < SCR_W; x++) {
    int u = s_u8[x];
    if (u <= 0 || u >= 256) { s_up[x] = s_lo[x] = s_yc[x]; continue; }
    int32_t upO = s_yc[x] - s_a47[x] + ((st->gy * s_a10[x]) >> 8);
    int32_t closed = s_yc[x] + s_a22[x];
    int32_t up = closed + (((upO - closed) * st->open) >> 8);
    int32_t lowO = s_yc[x] + s_a24[x] + ((st->gy * s_a04[x]) >> 8);
    int32_t lo = lowO - ((s_a03[x] * (256 - st->open)) >> 8);
    s_up[x] = up;
    s_lo[x] = lo > up ? lo : up;
    int32_t den = up - s_crease[x], mn = EYE_A * 31;  // 0.12a in Q8
    s_cdiv[x] = (uint16_t)clampi(den > mn ? den : mn, 1, 65535);
  }
}

void eye_render(GBitmap *fb, const EyeState *st, int y0, int y1, int lift, bool full) {
  if (!s_blob) return;
  lids_frame(st);
  const int a = EYE_A;
#if defined(DEMO_DITHER)
  const bool dith = true;
#else
  const bool dith = s_set.dither;
#endif
  // gaze rotation (Q12)
  int32_t angx = (st->gx * 5737) / 256, angy = (st->gy * 3964) / 256;  // 0.55 rad, 0.38 rad
  int32_t sax = sin_lookup(angx) >> 4, cax = cos_lookup(angx) >> 4, say = sin_lookup(angy) >> 4, cay = cos_lookup(angy) >> 4;
  const int32_t ex8 = (EYE_CX * 256) + (a * 256 * 2) / 100, ey8 = (EYE_CY * 256) - (a * 256 * 5) / 100;
  // iris centre on screen (Q8 px) for the REM bulge under a closed lid
  const int32_t gxs8 = ex8 + ((a * ((sax * cay) >> 12)) >> 4), gys8 = ey8 + ((a * say) >> 4);
  const int pr = st->pupil;
  // glints ride on the cornea: 70 % with the iris, 30 % fixed toward the light; never on the sclera
  const int32_t gcx = (sax * cay) >> 12, gcy = say;
  const int32_t hcx = (3 * -942 + 7 * (gcx - 820)) / 10, hcy = (3 * -942 + 7 * (gcy - 820)) / 10;
  const int32_t s2cx = gcx + 700, s2cy = gcy + 410;
  const bool glint = s_set.glint;
  const int32_t RI2 = RI * RI;
  const int32_t CORN2 = (RI * 115 / 100) * (RI * 115 / 100), LIM2 = (RI * 106 / 100) * (RI * 106 / 100);
  const int32_t SHU = a * 51, SHL = a * 15, SIGC = a * 9, W03 = a * 77, LO02 = a * 5, WL = a * 6;
  const int slit_a = (pr * 77) >> 8;
  const int vstr = s_set.veins == 0 ? 0 : s_set.veins == 1 ? 90 : 200;
  const int rem = st->open < 128 ? 256 - 2 * st->open : 0;
  const int32_t rem_den = (a * 77 >> 8) * (a * 77 >> 8);  // (0.3a)^2 in px^2
  const bool nat_sclera = s_set.sclera < 0;
  const int scr = (s_set.sclera >> 16) & 255, scg = (s_set.sclera >> 8) & 255, scb = s_set.sclera & 255;

  for (int y = y0; y < y1; y++) {
    if (y < 0 || y >= SCR_H) continue;
    GBitmapDataRowInfo ri = gbitmap_get_data_row_info(fb, y);
    int ys = y + lift;
    int32_t ysq = (ys << 8) + 128;
    int32_t py = ((ysq - ey8) * 16) / a;  // Q12
    float dyf = (ys + 0.5f - EYE_CY + 0.1f * a) / ((s_set.dark ? 1.05f : 1.2f) * a);
    int dy2 = clampi((int)(dyf * dyf * 256), 0, 60000);
    int xa = ri.min_x, xb = ri.max_x;
    for (int x = xa; x <= xb; x++) {
      if (!full && (ys < s_dyn0[x] || ys >= s_dyn1[x])) continue;
      int u = s_u8[x];
      int32_t up = s_up[x], lo = s_lo[x];
      int32_t px = s_px[x];
      int32_t d2 = px * px + py * py;
      bool in_sph = d2 < (1 << 24);
      int bay = BAYER[((y & 3) << 2) | (x & 3)];
      int off = dith ? (((bay * 2 + 1) * 85) >> 5) - 42 : 0;
      uint8_t out;
      if (u > 0 && u < 256 && ysq > up && ysq < lo) {
        int r, g, b;
        bool ramp = false;
        int sclera_s = 0;
        if (!in_sph) { r = 70; g = 30; b = 28; }
        else {
          int32_t pz = s_sqrt_pz[d2 >> 14];
          int32_t x1 = (px * cax - pz * sax) >> 12, z1 = (px * sax + pz * cax) >> 12;
          int32_t y2 = (py * cay - z1 * say) >> 12, z2 = (py * say + z1 * cay) >> 12;
          int32_t lamb = (px * -1848 + py * -2259 + pz * 2875) >> 16;  // Q8
          if (lamb < 0) lamb = 0;
          int sh = ((149 + ((107 * sm8((ysq - up) * 256 / SHU)) >> 8)) * (210 + ((46 * sm8((lo - ysq) * 256 / SHL)) >> 8))) >> 8;
          int32_t r2 = x1 * x1 + y2 * y2;
          bool front = z2 > 0;
          if (front && r2 < RI2) {
            int t = s_tsq[clampi((int)((uint32_t)r2 * 1023u / (uint32_t)RI2), 0, 1023)];
            bool inP;
            if (s_set.pupil == 1) {
              int sx = x1 * 256 / RI, sy = y2 * 256 / RI;
              int nx = slit_a > 0 ? sx * 256 / slit_a : 9999, ny = sy * 256 / 241;
              inP = nx * nx + ny * ny < 65536;
            } else inP = t < pr;
            if (inP) { r = 10; g = 8; b = 9; }
            else {
              int tt = t < 1 ? 1 : t;
              int tp = s_set.pupil == 1 ? t : PR0 + (t - pr) * (256 - PR0) / (256 - pr > 1 ? 256 - pr : 1);
              int tx = 48 + (x1 * tp * 48) / (tt * RI), ty = 48 + (y2 * tp * 48) / (tt * RI);
              uint8_t tb = s_iris[clampi(ty, 0, 95) * TEX + clampi(tx, 0, 95)];
              const uint8_t *c = ((s_set.imode == 2 && (tb & 0x40)) || (s_set.imode == 1 && (tb & 0x80))) ? s_lut2[tb & 63] : s_lut1[tb & 63];
              int m = ((184 + ((108 * lamb) >> 8)) * sh) >> 8;
              r = (c[0] * m) >> 8; g = (c[1] * m) >> 8; b = (c[2] * m) >> 8;
            }
          } else {
            int apx = px < 0 ? -px : px;  // Q12
            int kk = ((169 + ((87 * lamb) >> 8)) * (169 + ((87 * (pz >> 4)) >> 8))) >> 8;
            kk = (kk * (256 - ((56 * sm8(((apx >> 4) - 128) * 2)) >> 8))) >> 8;
            int lim = 256;
            if (front && r2 < LIM2) lim = 205 + ((51 * sm8((r2 - RI2) / ((LIM2 - RI2) >> 8))) >> 8);
            int s = (((kk * sh) >> 8) * lim) >> 8;
            int vv = 0;
            if (vstr) {
              int wv = sm8(((apx >> 4) - 72) * 256 / 171);
              if (wv > 0) {
                int vx = clampi((x1 + 4096) * 96 >> 13, 0, 95), vy = clampi((y2 + 4096) * 96 >> 13, 0, 95);
                vv = (s_vein[vy * TEX + vx] * wv >> 8) * vstr >> 8;
                vv = clampi(vv * 410 >> 8, 0, 256);
              }
            }
            if (nat_sclera && vv < 115) { ramp = true; sclera_s = (s * 312) >> 8; }
            int sr = nat_sclera ? 236 : scr, sg = nat_sclera ? 231 : scg, sb = nat_sclera ? 224 : scb;
            r = (sr * s) >> 8; g = (sg * s) >> 8; b = (sb * s) >> 8;
            int pk = (107 * sm8(((apx >> 4) - 115) * 256 / 141)) >> 8;  // pink corners
            if (nat_sclera) { r += ((((214 * s) >> 8) - r) * pk) >> 8; g += ((((158 * s) >> 8) - g) * pk) >> 8; b += ((((148 * s) >> 8) - b) * pk) >> 8; }
            if (vv) { int vr = (176 * sh) >> 8, vg = (34 * sh) >> 8, vb = (40 * sh) >> 8; r += ((vr - r) * vv) >> 8; g += ((vg - g) * vv) >> 8; b += ((vb - b) * vv) >> 8; }
          }
          // specular window (fixed in screen space; the iris slides under it)
          int32_t hxd = px - hcx, hyd = py - hcy;
          if (glint && front && r2 < CORN2 && hxd > -262 && hxd < 262 && hyd > -213 && hyd < 213 && !(hxd > -34 && hxd < 34) && !(hyd > -29 && hyd < 29)) {
            int hx = hxd * 256 / 262, hy = hyd * 256 / 213;
            int hx2 = hx * hx >> 8, hy2 = hy * hy >> 8;
            int w = (hx2 * hx2 >> 8) + (hy2 * hy2 >> 8);
            if (w < 256) {
              int k = 235;
              int amt = (k * sm8((256 - w) * 256 / 115)) >> 8;
              if (ramp) { ramp = false; }
              r += ((255 - r) * amt) >> 8; g += ((255 - g) * amt) >> 8; b += ((255 - b) * amt) >> 8;
            }
          }
          int32_t s2x = px - s2cx, s2y = py - s2cy;
          if (glint && front && r2 < CORN2 && s2x > -123 && s2x < 123 && s2y > -123 && s2y < 123) {
            int dd = (s2x * s2x + s2y * s2y);
            if (dd < 123 * 123) {
              int amt = (179 * sm8((123 * 123 - dd) * 256 / (123 * 123 - 49 * 49))) >> 8;
              ramp = false;
              r += ((255 - r) * amt) >> 8; g += ((255 - g) * amt) >> 8; b += ((255 - b) * amt) >> 8;
            }
          }
        }
        if (u < 19) { int k = sm8((19 - u) * 256 / 15); ramp = false; r += ((198 - r) * k) >> 8; g += ((104 - g) * k) >> 8; b += ((100 - b) * k) >> 8; }
        if (lo - ysq < WL && u > 10 && u < 248) { ramp = false; r += ((188 - r) * 218) >> 8; g += ((110 - g) * 218) >> 8; b += ((102 - b) * 218) >> 8; }
        out = ramp ? ramp4(SCLERA_RAMP, sclera_s, bay, dith) : rgb_byte(r, g, b, off);
      } else {
        // skin, night ramp
        int de2 = (s_dx2[x] + dy2) >> 2;
        int bb = s_fall[clampi(de2, 0, 255)];
        if (u > 0 && u < 256) {
          if (ysq <= up) {
            int32_t cy8 = s_crease[x];
            int v = (up - ysq) * 256 / s_cdiv[x];
            if (v < 256) bb = (bb * (256 + ((26 * (sin_lookup(v * 128) >> 8)) >> 8))) >> 8;
            int zq = clampi((ysq - cy8) * 256 / SIGC, -1024, 1024);
            int e = s_exp[clampi((zq * zq) >> 11, 0, 255)];
            bb = (bb * (256 - ((((82 * s_cw[x]) >> 8) * e) >> 8))) >> 8;
            if (ysq < cy8) bb = (bb * 230) >> 8;
          } else {
            int w = (ysq - s_lob[x]) * 256 / W03;
            if (w < 31) bb = (bb * 274) >> 8;
            else { int q = clampi((w - 218) * 256 / 64, -1024, 1024); bb = (bb * (256 - ((33 * s_exp[clampi((q * q) >> 11, 0, 255)]) >> 8))) >> 8; }
          }
        }
        if (in_sph && ysq < lo + LO02) {
          int32_t pz = s_sqrt_pz[d2 >> 14];
          bb = (bb * (218 + ((30 * (pz >> 4)) >> 8))) >> 8;
          if (rem) {
            int32_t dx = (((x << 8) + 128) - gxs8) >> 8, dy = (ysq - gys8) >> 8;
            int idx = (int)((dx * dx + dy * dy) * 32 / rem_den);
            if (idx < 255) bb = (bb * (256 + ((((18 * rem) >> 8) * s_exp[idx]) >> 8))) >> 8;
          }
        }
        bb = (bb * (244 + ((24 * s_noise[((ys & 63) << 6) | (x & 63)]) >> 8))) >> 8;
        out = ramp4(s_set.dark ? SKIN_RAMP_BLACK : SKIN_RAMP, (bb * 200) >> 8, bay, dith);
      }
      ri.data[x] = out;
    }
  }
}

// ------------------------------------------------------------------ overlays
static float lid_up_f(float u, const EyeState *st) {
  const float a = EYE_A;
  float uc = u < 0 ? 0 : u > 1 ? 1 : u;
  float yc = EYE_CY + a * (0.10f - 0.24f * uc);
  float bu = bump_tab(s_bump_u, u), bl = bump_tab(s_bump_l, u);
  float upO = yc - a * 0.47f * bu + (st->gy / 256.0f) * a * 0.10f * bu;
  float closed = yc + a * 0.22f * bl;
  return closed + (upO - closed) * (st->open / 256.0f);
}
static float lid_lo_f(float u, const EyeState *st) {
  const float a = EYE_A;
  float uc = u < 0 ? 0 : u > 1 ? 1 : u;
  float yc = EYE_CY + a * (0.10f - 0.24f * uc);
  float bl = bump_tab(s_bump_l, u);
  float lo = yc + a * 0.24f * bl + (st->gy / 256.0f) * a * 0.04f * bl - a * 0.03f * bl * (1 - st->open / 256.0f);
  float up = lid_up_f(u, st);
  return lo > up ? lo : up;
}
static inline float cosr(float rad) { return cos_lookup((int32_t)(rad * 10430.378f)) / 65536.0f; }
static inline float sinr(float rad) { return sin_lookup((int32_t)(rad * 10430.378f)) / 65536.0f; }
static inline float hashf(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (h & 0xFFFFFF) / 16777216.0f;
}

#define LINER_N 40
static GPoint s_liner[2 * (LINER_N + 1) + 2];
static GPathInfo s_liner_info = {.num_points = 0, .points = s_liner};
static GPath *s_liner_path;

void eye_overlay(GContext *ctx, const EyeState *st, int lift, bool brow) {
  const float a = EYE_A, cx = EYE_CX, cy = EYE_CY;
  const float open = st->open / 256.0f;
  const int ly = -lift;
  GColor lash = GColorBlack;
  // liner + wing
  int n = 0;
  for (int i = 0; i <= LINER_N; i++) {
    float u = 0.02f + 0.98f * i / LINER_N;
    s_liner[n++] = GPoint((int)(cx - a + 2 * a * u + 0.5f), (int)(lid_up_f(u, st) - a * (0.010f + 0.045f * u * u) + 0.5f) + ly);
  }
  float yco = cy + a * (0.10f - 0.24f);
  s_liner[n++] = GPoint((int)(cx + a * 1.21f), (int)(yco - a * 0.17f) + ly);
  s_liner[n++] = GPoint((int)(cx + a * 1.0f), (int)(yco + 0.012f * a) + ly);
  for (int i = LINER_N; i >= 0; i--) {
    float u = 0.02f + 0.98f * i / LINER_N;
    s_liner[n++] = GPoint((int)(cx - a + 2 * a * u + 0.5f), (int)(lid_up_f(u, st) + 0.5f) + ly);
  }
  s_liner_info.num_points = n;
  if (!s_liner_path) s_liner_path = gpath_create(&s_liner_info);
  s_liner_path->num_points = n;
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_fill_color(ctx, lash);
  gpath_draw_filled(ctx, s_liner_path);
  graphics_context_set_stroke_color(ctx, lash);
  graphics_context_set_stroke_width(ctx, 1);
  gpath_draw_outline(ctx, s_liner_path);

  // upper lashes (curl up when open, hang down when closed)
  for (int i = 0; i < 34; i++) {
    float u = 0.08f + 0.9f * i / 33 + (hashf(i, 7) - 0.5f) * 0.012f;
    float bx = cx - a + 2 * a * u, by = lid_up_f(u, st) - a * 0.012f;
    float L = a * (0.06f + 0.17f * u * sqrtf(u)) * (0.85f + 0.3f * hashf(i, 3));
    float tO = -1.5708f + 1.05f * (u - 0.30f), tC = 1.5708f + 0.55f * (u - 0.5f) - 0.15f;
    float th = tC + (tO - tC) * open, cu = 0.45f * (open * 2 - 1);
    GPoint p0 = GPoint((int)bx, (int)by + ly);
    GPoint p1 = GPoint((int)(bx + 0.5f * L * cosr(th + cu)), (int)(by + 0.5f * L * sinr(th + cu)) + ly);
    GPoint p2 = GPoint((int)(bx + L * cosr(th)), (int)(by + L * sinr(th)) + ly);
    graphics_context_set_stroke_width(ctx, u > 0.62f ? 2 : 1);
    graphics_draw_line(ctx, p0, p1);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, p1, p2);
  }
  // lower lashes
  if (open > 0.3f) {
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    for (int i = 0; i < 10; i++) {
      float u = 0.45f + 0.5f * i / 9, bx = cx - a + 2 * a * u, by = lid_lo_f(u, st) + 0.6f;
      float L = a * (0.02f + 0.045f * u) * (open - 0.3f) / 0.7f, th = 1.5708f - 0.7f * u;
      graphics_draw_line(ctx, GPoint((int)bx, (int)by + ly), GPoint((int)(bx + L * cosr(th)), (int)(by + L * sinr(th)) + ly));
    }
  }
  // brow (static rows: drawn on full redraws only)
  if (brow) {
    graphics_context_set_stroke_color(ctx, GColorBlack);
    for (int i = 0; i < 80; i++) {
      float u = i / 79.0f, bxp = cx - 0.86f * a + 2.0f * a * u + (hashf(i, 5) - 0.5f) * 2;
      float byp = cy - 0.94f * a - 0.30f * a * bump_tab(s_bump_b, u) + 0.10f * a * u + (hashf(i, 9) - 0.5f) * a * 0.05f * (1 - u);
      float th = -0.75f + 0.85f * u + (hashf(i, 2) - 0.5f) * 0.2f, L = a * (0.11f - 0.06f * u);
      graphics_context_set_stroke_width(ctx, u < 0.4f ? 2 : 1);
      graphics_draw_line(ctx, GPoint((int)bxp, (int)byp + ly), GPoint((int)(bxp + L * cosr(th)), (int)(byp + L * sinr(th)) + ly));
    }
  }
}
