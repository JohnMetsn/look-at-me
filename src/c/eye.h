#pragma once
#include <pebble.h>

// Geometry for layout C (time under the eye). See DESIGN.md.
#if defined(PBL_ROUND)
#define SCR_W 260
#define SCR_H 260
#define EYE_CX 126
#define EYE_CY 104
#define EYE_A 112
#define TIME_X 130
#define TIME_Y 194
#define DATE_X 130
#define DATE_Y 232
#else
#define SCR_W 200
#define SCR_H 228
#define EYE_CX 96
#define EYE_CY 92
#define EYE_A 94
#define TIME_X 100
#define TIME_Y 176
#define DATE_X 100
#define DATE_Y 212
#endif

typedef struct {
  uint32_t iris, iris2;  // 0xRRGGBB
  int32_t sclera;        // -1 = natural sclera ramp, else 0xRRGGBB
  uint8_t imode;         // 0 plain, 1 hazel (inner colour), 2 sector heterochromia
  uint8_t pupil;         // 0 round, 1 slit
  uint8_t prad;          // 0 = natural pupil, else rest radius in percent of the iris
  uint8_t font;          // 0 Orbitron, 1 Michroma, 2 Chakra Petch
  uint8_t dither;        // 0 flat palette, 1 ordered dither
  uint8_t wake;          // bit0 tilt (raise to look), bit1 shake
  uint8_t stare;         // 0 short, 1 normal, 2 long
  uint8_t veins;         // 0 none, 1 subtle, 2 bloodshot
  uint8_t quiet;         // 1 = the eye sleeps during Quiet Time
  uint8_t clock;         // 0 like the watch, 1 24 h, 2 12 h
  uint8_t glint;         // 1 = window reflection on the cornea
  uint8_t dark;          // 1 = black surround: only the lids show, everything else is black
} Settings;

// Q8 state: open 0..256, gx/gy -256..256 (gaze), pupil = radius as a fraction of the iris (Q8)
typedef struct { int open, gx, gy, pupil; } EyeState;

bool eye_init(void);
void eye_deinit(void);
void eye_apply_settings(const Settings *s);
// Writes palette bytes for rows [y0, y1) of the frame buffer. lift = rows hidden by Timeline Quick View.
// full = false: only pixels whose value depends on the eye state are rewritten (the frame buffer keeps the rest).
void eye_render(GBitmap *fb, const EyeState *st, int y0, int y1, int lift, bool full);
// Liner, lashes, brow (GContext, after the frame buffer is released)
void eye_overlay(GContext *ctx, const EyeState *st, int lift, bool brow);
// Band of rows that changes while the eye animates
void eye_band(int *y0, int *y1);
uint8_t eye_tint_colon(void);
