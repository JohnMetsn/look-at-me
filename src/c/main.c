// Look at me! — a closed eye that wakes when you look at the watch, finds you and keeps looking at you.
// Core: settings, triggers (raise to look / shake), the glance timeline, accelerometer tracking, text.
#include <pebble.h>
#include "eye.h"

#define FRAME_MS 66
enum { PK_FORMAT = 1, PK_SETTINGS = 2 };
#define FORMAT_VERSION 3

static Window *s_win;
static Layer *s_canvas;
static Settings s_set;
static GFont s_tfont, s_dfont;
static int s_font_loaded = -1;
static bool s_full = true;
static int s_last_lift = -1;

// animation
static bool s_anim;
static uint64_t s_anim_t0;
static AppTimer *s_timer;
static uint32_t s_frames, s_render_ms, s_px_ms, s_ov_ms;
static bool s_tracking, s_have_ref, s_look_on;
static int s_ax, s_ay, s_ax0, s_ay0;
static float s_smx, s_smy;

// time_ms() follows the wall clock, which can step backwards (phone time sync): keep it monotonic.
static uint64_t now_ms(void) {
  static uint64_t last, skew;
  time_t s; uint16_t ms; time_ms(&s, &ms);
  uint64_t t = (uint64_t)s * 1000 + ms + skew;
  if (t < last) { skew += last - t; t = last; }
  last = t;
  return t;
}
static int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// ------------------------------------------------------------------ settings
static void settings_default(void) {
  s_set = (Settings){.iris = 0x3A6AAC, .iris2 = 0x3A6AAC, .sclera = -1, .imode = 0, .pupil = 0, .prad = 0, .font = 0,
                     .dither = 0, .wake = 5, .stare = 1, .veins = 1, .quiet = 1, .clock = 0, .glint = 1, .dark = 1};
}
static void settings_load(void) {
  settings_default();
  if (persist_read_int(PK_FORMAT) == FORMAT_VERSION && persist_get_size(PK_SETTINGS) == (int)sizeof s_set)
    persist_read_data(PK_SETTINGS, &s_set, sizeof s_set);
}
static void settings_save(void) {
  persist_write_int(PK_FORMAT, FORMAT_VERSION);
  persist_write_data(PK_SETTINGS, &s_set, sizeof s_set);
}

#if defined(PBL_ROUND)
static const uint32_t TFONT[3] = {RESOURCE_ID_TG_ORB_46, RESOURCE_ID_TG_MIC_40, RESOURCE_ID_TG_CHK_50};
static const uint32_t DFONT[3] = {RESOURCE_ID_DG_ORB_16, RESOURCE_ID_DG_MIC_14, RESOURCE_ID_DG_CHK_17};
#else
static const uint32_t TFONT[3] = {RESOURCE_ID_TE_ORB_42, RESOURCE_ID_TE_MIC_36, RESOURCE_ID_TE_CHK_45};
static const uint32_t DFONT[3] = {RESOURCE_ID_DE_ORB_15, RESOURCE_ID_DE_MIC_13, RESOURCE_ID_DE_CHK_16};
#endif
static void load_fonts(void) {
  int f = clampi(s_set.font, 0, 2);
  if (f == s_font_loaded) return;
  if (s_tfont) fonts_unload_custom_font(s_tfont);
  if (s_dfont) fonts_unload_custom_font(s_dfont);
  s_tfont = fonts_load_custom_font(resource_get_handle(TFONT[f]));
  s_dfont = fonts_load_custom_font(resource_get_handle(DFONT[f]));
  s_font_loaded = f;
}

// ------------------------------------------------------------------ glance timeline
typedef struct { float open, gx, gy, pupil; const char *phase; } Glance;
// Faster wake (owner feedback 2026-11-19): open by 0.45 s, locked on the wearer by 0.78 s.
#define T_FLUTTER 120
#define T_OPEN 450
#define T_SACC1 500
#define T_LOCK 720
#define T_DILATE 1200
static int close_at(void) { return s_set.stare == 0 ? 3600 : s_set.stare == 2 ? 9000 : 6000; }
#define T_CLOSE_LEN 1100
static int glance_len(void) { return close_at() + T_CLOSE_LEN + 100; }
static float easeo(float t) { t = clampf(t, 0, 1); float u = 1 - t; return 1 - u * u * u; }
static float easeio(float t) { t = clampf(t, 0, 1); if (t < 0.5f) return 4 * t * t * t; float u = -2 * t + 2; return 1 - u * u * u / 2; }
static float smoothf(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static float hash01(uint32_t k, uint32_t salt) {
  uint32_t h = k * 374761393u + salt * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return ((h ^ (h >> 16)) & 0xFFFF) / 65536.0f;
}

static void glance(float t, float Tx, float Ty, uint32_t treal, Glance *s) {
  const float SX = 0.25f, SY = 0.6f, QX = -0.6f, QY = 0.12f;
  s->open = 0; s->gx = SX; s->gy = SY; s->pupil = 0.36f; s->phase = "sleep";
  if (t < 0) return;
  if (t < T_FLUTTER) { s->open = 0.13f * sin_lookup((int32_t)(t / T_FLUTTER * 32768)) / 65536.0f; s->phase = "flutter"; return; }
  float fx = SX + (QX - SX) * 0.8f, fy = SY + (QY - SY) * 0.8f;
  if (t < T_OPEN) { float k = easeo((t - T_FLUTTER) / (T_OPEN - T_FLUTTER)); s->open = k; s->gx = SX + (fx - SX) * k; s->gy = SY + (fy - SY) * k; s->phase = "open"; return; }
  s->open = 1;
  float ox = Tx + 0.24f, oy = Ty - 0.14f;
  if (t < T_LOCK) {
    if (t < T_SACC1) { s->gx = fx; s->gy = fy; }
    else { float k = easeo((t - T_SACC1) / 60); s->gx = fx + (ox - fx) * k; s->gy = fy + (oy - fy) * k; }
    s->phase = "search"; return;
  }
  float k = easeo((t - T_LOCK) / 60);
  uint32_t mk = treal / 620;
  float mx = t > T_DILATE ? (hash01(mk, 1) - 0.5f) * 0.07f : 0, my = t > T_DILATE ? (hash01(mk, 2) - 0.5f) * 0.05f : 0;
  s->gx = ox + (Tx - ox) * k + mx;
  s->gy = oy + (Ty - oy) * k + my;
  if (t < T_DILATE) { s->pupil = 0.36f + 0.16f * easeo((t - T_LOCK - 50) / 430); s->phase = "lock"; }
  else { s->pupil = 0.52f - 0.07f * smoothf(T_DILATE, 4700, t); s->phase = "track"; }
  int ca = close_at();
  float blinks[2] = {3000, 6400};
  for (int i = 0; i < 2; i++) {
    float b = blinks[i];
    if (b + 500 > ca) continue;
    if (t >= b && t < b + 400) { float q = t < b + 150 ? (t - b) / 150 : 1 - (t - b - 150) / 250; s->open = 1 - clampf(q, 0, 1); s->phase = "blink"; }
  }
  if (t >= ca) {
    float q = easeio((t - ca) / T_CLOSE_LEN);
    s->open = 1 - q; s->gy += 0.4f * q; s->pupil += (0.36f - s->pupil) * q; s->phase = "close";
    if (q >= 1) { s->gx = Tx * 0.3f; s->gy = 0.55f; s->phase = "sleep"; }
  }
}

#if defined(CAPTURE)
// Capture build (emulator only): every emu-tap advances one frame. CAPTURE=1 plays one glance at 15 fps with a
// scripted wearer moving a little; CAPTURE=2 steps through store poses with different irises.
static int s_cap;
typedef struct { uint32_t iris, iris2; int32_t sclera; uint8_t imode, pupil, prad, dither; float open, gx, gy, pr; } Pose;
static const Pose POSES[] = {
  {0x3A6AAC, 0x3A6AAC, -1, 0, 0, 0, 0, 0.0f, 0.25f, 0.6f, 0.36f},
  {0x3A6AAC, 0x3A6AAC, -1, 0, 0, 0, 0, 1.0f, -0.08f, 0.02f, 0.48f},
  {0xEAAE0A, 0xEAAE0A, -1, 0, 1, 0, 0, 1.0f, -0.35f, 0.05f, 0.5f},
  {0x5C7C3A, 0x8A541C, -1, 1, 0, 0, 1, 1.0f, 0.2f, -0.1f, 0.42f},
  {0x783CC8, 0x783CC8, -1, 0, 0, 0, 0, 1.0f, 0.05f, 0.3f, 0.52f},
  {0xD61818, 0xD61818, 0x221A1A, 0, 1, 0, 0, 1.0f, -0.1f, 0.0f, 0.5f},
};
static EyeState capture_state(void) {
  Glance g;
  {  // frame index from the watch clock: set the emulator to 10:00:00 + 4*frame seconds (emu-set-time)
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    s_cap = ((tm->tm_hour - 10) * 3600 + tm->tm_min * 60 + tm->tm_sec) / 4;
    if (s_cap < 0) s_cap = 0;
  }
  if (CAPTURE == 2) {
    const Pose *p = &POSES[s_cap % (int)(sizeof POSES / sizeof POSES[0])];
    s_set.iris = p->iris; s_set.iris2 = p->iris2; s_set.sclera = p->sclera; s_set.imode = p->imode;
    s_set.pupil = p->pupil; s_set.prad = p->prad; s_set.dither = p->dither;
    eye_apply_settings(&s_set);
    g.open = p->open; g.gx = p->gx; g.gy = p->gy; g.pupil = p->pr;
  } else {
    float t = s_cap * 66.0f - 400.0f;  // a few closed frames first
    float tx = 0, ty = 0;
    if (t > T_LOCK + 900) { float k = (t - T_LOCK - 900) / 1000.0f; tx = 0.28f * sin_lookup((int32_t)(k * 9000)) / 65536.0f; ty = -0.12f * sin_lookup((int32_t)(k * 6000)) / 65536.0f; }
    glance(t, tx, ty, (uint32_t)(t > 0 ? t : 0), &g);
  }
  EyeState st = {.open = clampi((int)(g.open * 256), 0, 256), .gx = clampi((int)(g.gx * 256), -256, 256),
                 .gy = clampi((int)(g.gy * 256), -256, 256), .pupil = clampi((int)(g.pupil * 256), 20, 240)};
  return st;
}
static void cap_tick(struct tm *t, TimeUnits u) { (void)t; (void)u; s_full = true; layer_mark_dirty(s_canvas); }
#endif

static EyeState current_state(void) {
#if defined(CAPTURE)
  return capture_state();
#endif
  Glance g;
  float t = -1;
  uint32_t treal = 0;
  if (s_anim) {
    treal = (uint32_t)(now_ms() - s_anim_t0);
    t = treal;
#if defined(DEMO_OPEN)
    if (t > 1300) t = 1300 + (float)(((uint32_t)t - 1300) % 1600);
#endif
  }
  // target from the accelerometer: counter-rotate against the wrist since the eye woke up
  float tx = 0, ty = 0;
  if (s_anim && !s_have_ref && t >= T_LOCK - 80) { s_ax0 = s_ax; s_ay0 = s_ay; s_have_ref = true; }
  if (s_have_ref) {
    tx = clampf(-(s_ax - s_ax0) / 550.0f, -0.95f, 0.95f);
    ty = clampf((s_ay - s_ay0) / 380.0f, -0.9f, 0.9f);
  }
  s_smx += (tx - s_smx) * 0.22f;
  s_smy += (ty - s_smy) * 0.22f;
  glance(t, s_smx, s_smy, treal, &g);
  float pr = g.pupil;
  if (s_set.prad) pr = clampf(s_set.prad / 100.0f + (g.pupil - 0.45f) * 0.4f, 0.1f, 0.92f);
  EyeState st = {
    .open = clampi((int)(g.open * 256), 0, 256),
    .gx = clampi((int)(g.gx * 256), -256, 256),
    .gy = clampi((int)(g.gy * 256), -256, 256),
    .pupil = clampi((int)(pr * 256), 20, 240),
  };
  return st;
}

// ------------------------------------------------------------------ text
static void draw_text_at(GContext *ctx, const char *txt, GFont f, int x, int y, GColor c) {
  graphics_context_set_text_color(ctx, c);
  graphics_draw_text(ctx, txt, f, GRect(x, y, SCR_W, 80), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}
static GSize text_size(const char *txt, GFont f) {
  return graphics_text_layout_get_content_size(txt, f, GRect(0, 0, SCR_W, 80), GTextOverflowModeFill, GTextAlignmentLeft);
}
static void outlined(GContext *ctx, const char *txt, GFont f, int x, int y, GColor fill, int ow) {
  static const int8_t O1[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  static const int8_t O2[4][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}};
  for (int i = 0; i < 8; i++) draw_text_at(ctx, txt, f, x + O1[i][0], y + O1[i][1], GColorBlack);
  if (ow > 1) for (int i = 0; i < 4; i++) draw_text_at(ctx, txt, f, x + O2[i][0], y + O2[i][1], GColorBlack);
  draw_text_at(ctx, txt, f, x, y, fill);
}
static void draw_texts(GContext *ctx, int lift) {
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  bool h24 = s_set.clock == 1 || (s_set.clock == 0 && clock_is_24h_style());
  char hh[4], mm[4], date[16];
  int hour = tm->tm_hour;
#if defined(CAPTURE)
  hour = 10; tm->tm_min = 10;
#endif
  if (!h24) { hour %= 12; if (hour == 0) hour = 12; }
  snprintf(hh, sizeof hh, "%02d", hour);
  snprintf(mm, sizeof mm, "%02d", tm->tm_min);
  static const char *DAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *MONS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  snprintf(date, sizeof date, "%s %d %s", DAYS[tm->tm_wday], tm->tm_mday, MONS[tm->tm_mon]);

  GSize sh = text_size(hh, s_tfont), sc = text_size(":", s_tfont), sm = text_size(mm, s_tfont);
  int gap = 2, tot = sh.w + sc.w + sm.w + 2 * gap;
  int x = TIME_X - tot / 2, y = TIME_Y - lift - (sh.h * 62) / 100;
  outlined(ctx, hh, s_tfont, x, y, GColorWhite, 2);
  outlined(ctx, ":", s_tfont, x + sh.w + gap, y, (GColor){.argb = eye_tint_colon()}, 2);
  outlined(ctx, mm, s_tfont, x + sh.w + sc.w + 2 * gap, y, GColorWhite, 2);
  GSize sd = text_size(date, s_dfont);
  outlined(ctx, date, s_dfont, DATE_X - sd.w / 2, DATE_Y - lift - (sd.h * 62) / 100, GColorLightGray, 1);
}

// ------------------------------------------------------------------ drawing
static void next_frame(void *d) { (void)d; s_timer = NULL; layer_mark_dirty(s_canvas); }
static void stop_tracking(void);
static void subscribe_triggers(void);
static uint64_t s_last_end;

static void update_proc(Layer *layer, GContext *ctx) {
  GRect ub = layer_get_unobstructed_bounds(layer);
  int lift = SCR_H - ub.size.h;
  if (lift < 0) lift = 0;
  uint64_t t0 = now_ms();
  EyeState st = current_state();
  static int s_drawn_min = -1;
  time_t tnow = time(NULL);
  int cur_min = (int)(tnow / 60);
  bool full = s_full || lift != s_last_lift || cur_min != s_drawn_min;
  s_drawn_min = cur_min;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (fb) {
    int y0 = 0, y1 = SCR_H;
    if (!full) { eye_band(&y0, &y1); y0 -= lift; y1 -= lift; }
    eye_render(fb, &st, y0, y1, lift, full);
    graphics_release_frame_buffer(ctx, fb);
    uint64_t t1 = now_ms();
    eye_overlay(ctx, &st, lift, full);
    if (s_anim) { s_px_ms += (uint32_t)(t1 - t0); s_ov_ms += (uint32_t)(now_ms() - t1); }
    s_full = false;
    s_last_lift = lift;
  } else full = true;
  if (full) draw_texts(ctx, lift);  // text rows are outside the eye band and persist
  uint32_t dt = (uint32_t)(now_ms() - t0);
  if (!s_anim) return;
  s_frames++;
  s_render_ms += dt;
#if !defined(DEMO_OPEN)
  uint32_t el = (uint32_t)(now_ms() - s_anim_t0);
  if (el >= (uint32_t)glance_len()) {
    s_anim = false;
    uint32_t nf = s_frames ? s_frames : 1;
    APP_LOG(APP_LOG_LEVEL_INFO, "glance: %lu frames in %lu ms (%lu fps), render avg %lu ms (pixels %lu, overlay %lu), heap free %u",
            (unsigned long)s_frames, (unsigned long)el, (unsigned long)(s_frames * 1000 / (el ? el : 1)),
            (unsigned long)(s_render_ms / nf), (unsigned long)(s_px_ms / nf), (unsigned long)(s_ov_ms / nf), (unsigned)heap_bytes_free());
    stop_tracking();
    s_last_end = now_ms();
    s_full = true;
    layer_mark_dirty(s_canvas);
    return;
  }
#else
  if (s_frames % 60 == 0)
    APP_LOG(APP_LOG_LEVEL_INFO, "demo: render avg %lu ms (pixels %lu, overlay %lu), heap free %u", (unsigned long)(s_render_ms / s_frames),
            (unsigned long)(s_px_ms / s_frames), (unsigned long)(s_ov_ms / s_frames), (unsigned)heap_bytes_free());
#endif
  if (s_timer) app_timer_cancel(s_timer);
  s_timer = app_timer_register(FRAME_MS > dt ? FRAME_MS - dt : 1, next_frame, NULL);
}

// ------------------------------------------------------------------ tracking & triggers
// Tracking: the reference pose ("the wearer is straight in front of the eye") is taken when the eye locks on,
// then it slowly follows the wrist (time constant ~2 s). Quick wrist turns are counter-rotated; once the wrist
// settles in a new pose the eye drifts back to looking straight out at the wearer.
static int s_axf, s_ayf;  // filtered sample x16
static void track_handler(AccelData *d, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (d[i].did_vibrate) continue;
    s_axf += (d[i].x * 16 - s_axf) / 2;
    s_ayf += (d[i].y * 16 - s_ayf) / 2;
    s_ax = s_axf / 16; s_ay = s_ayf / 16;
    if (s_have_ref) { s_ax0 += (s_ax - s_ax0) / 48; s_ay0 += (s_ay - s_ay0) / 48; }
  }
}
static void start_tracking(int x, int y) {
  accel_data_service_unsubscribe();
  s_look_on = false;
  s_tracking = true;
  s_axf = x * 16; s_ayf = y * 16;
  s_ax = x; s_ay = y;
  accel_data_service_subscribe(1, track_handler);
  accel_service_set_sampling_rate(ACCEL_SAMPLING_25HZ);
}
static void stop_tracking(void) {
  if (!s_tracking) return;
  accel_data_service_unsubscribe();
  s_tracking = false;
  subscribe_triggers();
}
static bool quiet_now(void) { return s_set.quiet && quiet_time_is_active(); }

static void start_glance(bool force, int refx, int refy) {
  if (s_anim) return;
  if (!force && quiet_now()) return;
  if (!force && now_ms() - s_last_end < 1500) return;  // the eye just closed
  if (layer_get_unobstructed_bounds(s_canvas).size.h < SCR_H) return;  // Timeline Quick View: no glance
  s_anim = true;
  s_anim_t0 = now_ms();
  s_frames = s_render_ms = s_px_ms = s_ov_ms = 0;
  s_smx = s_smy = 0;
  s_have_ref = false;  // taken at lock-on, see current_state()
  start_tracking(refx, refy);
  s_full = true;
  layer_mark_dirty(s_canvas);
}

static void tap_handler(AccelAxisType axis, int32_t dir) {
  (void)axis; (void)dir;
  if (s_set.wake & 2) start_glance(false, 0, 0);
}
static void backlight_handler(bool on) {
  if (on && (s_set.wake & 4)) start_glance(false, 0, 0);
}
// Raise to look: the wrist changed pose within the last ~1.5 s (also a slow, smooth raise) and the screen is
// now held still and tilted toward the wearer. Works from any start pose, including hands on a keyboard.
#define LOOK_RING 15
static int16_t s_ring[LOOK_RING][3];
static int s_ring_n, s_ring_i, s_look_run;
static void look_handler(AccelData *d, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (d[i].did_vibrate) { s_look_run = 0; continue; }
    int x = d[i].x, y = d[i].y, z = d[i].z;
    // largest pose change against any sample of the last 1.5 s
    int moved = 0, prev = s_ring_n ? (s_ring_i + LOOK_RING - 1) % LOOK_RING : -1, dv = 0;
    for (int k = 0; k < s_ring_n; k++) {
      int m = abs(x - s_ring[k][0]) + abs(y - s_ring[k][1]) + abs(z - s_ring[k][2]);
      if (m > moved) moved = m;
    }
    if (prev >= 0) dv = abs(x - s_ring[prev][0]) + abs(y - s_ring[prev][1]) + abs(z - s_ring[prev][2]);
    s_ring[s_ring_i][0] = x; s_ring[s_ring_i][1] = y; s_ring[s_ring_i][2] = z;
    s_ring_i = (s_ring_i + 1) % LOOK_RING;
    if (s_ring_n < LOOK_RING) s_ring_n++;
    // facing: screen tilted up toward the eyes (anything from flat to about 70 degrees)
    bool facing = z < -320 && x > -700 && x < 700 && y < 750;
    bool steady = dv < 150;
    if (facing && steady) s_look_run++; else s_look_run = 0;
    if (s_look_run >= 2 && moved > 420) {
      s_look_run = 0;
      s_ring_n = 0;  // a new raise is needed for the next glance
      start_glance(false, x, y);
      return;
    }
  }
}
// Raise to look streams 10 Hz in batches of 4 (2.5 wake-ups per second, quicker reaction).
// Shake uses the system's low-power tap detection; Light reacts when the backlight turns on.
static void subscribe_triggers(void) {
  if (s_tracking) return;
  accel_tap_service_unsubscribe();
  backlight_service_unsubscribe();
  if (s_look_on) { accel_data_service_unsubscribe(); s_look_on = false; }
  if (quiet_now()) return;
  if (s_set.wake & 2) accel_tap_service_subscribe(tap_handler);
  if (s_set.wake & 4) backlight_service_subscribe(backlight_handler);
  if (s_set.wake & 1) {
    accel_data_service_subscribe(4, look_handler);
    accel_service_set_sampling_rate(ACCEL_SAMPLING_10HZ);
    s_look_on = true;
  }
}

// ------------------------------------------------------------------ services
static bool s_was_quiet;
static void tick(struct tm *t, TimeUnits changed) {
  (void)t; (void)changed;
  bool q = quiet_now();
  if (q != s_was_quiet) { s_was_quiet = q; subscribe_triggers(); }
  s_full = true;
  layer_mark_dirty(s_canvas);
}
static void ub_change(AnimationProgress p, void *ctx) { (void)p; (void)ctx; s_full = true; layer_mark_dirty(s_canvas); }

static int32_t tint(DictionaryIterator *it, uint32_t key, int32_t def) {
  Tuple *t = dict_find(it, key);
  return t ? t->value->int32 : def;
}
static void inbox(DictionaryIterator *it, void *ctx) {
  (void)ctx;
  if (dict_find(it, MESSAGE_KEY_S_IRIS)) {
    s_set.iris = (uint32_t)tint(it, MESSAGE_KEY_S_IRIS, (int32_t)s_set.iris) & 0xFFFFFF;
    s_set.iris2 = (uint32_t)tint(it, MESSAGE_KEY_S_IRIS2, (int32_t)s_set.iris) & 0xFFFFFF;
    s_set.imode = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_IMODE, 0), 0, 2);
    s_set.pupil = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_PUPIL, 0), 0, 1);
    s_set.prad = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_PRAD, 0), 0, 95);
    s_set.sclera = tint(it, MESSAGE_KEY_S_SCLERA, -1);
    s_set.font = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_FONT, s_set.font), 0, 2);
    s_set.dither = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_DITHER, s_set.dither), 0, 1);
    s_set.wake = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_WAKE, s_set.wake), 1, 7);
    s_set.stare = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_STARE, s_set.stare), 0, 2);
    s_set.veins = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_VEINS, s_set.veins), 0, 2);
    s_set.quiet = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_QUIET, s_set.quiet), 0, 1);
    s_set.clock = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_CLOCK, s_set.clock), 0, 2);
    s_set.glint = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_GLINT, s_set.glint), 0, 1);
    s_set.dark = (uint8_t)clampi(tint(it, MESSAGE_KEY_S_DARK, s_set.dark), 0, 1);
    settings_save();
    eye_apply_settings(&s_set);
    load_fonts();
    subscribe_triggers();
    s_full = true;
    layer_mark_dirty(s_canvas);
  }
  if (dict_find(it, MESSAGE_KEY_DEMO)) start_glance(true, 0, 0);
}
static void say_hello(void *d) {
  (void)d;
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) { dict_write_int8(out, MESSAGE_KEY_HELLO, 1); app_message_outbox_send(); }
}

static void win_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, update_proc);
  layer_add_child(root, s_canvas);
}
static void win_unload(Window *w) { (void)w; layer_destroy(s_canvas); }

static void init(void) {
  settings_load();
  if (!eye_init()) APP_LOG(APP_LOG_LEVEL_ERROR, "eye_init: out of memory");
  eye_apply_settings(&s_set);
  load_fonts();
  s_win = window_create();
  window_set_background_color(s_win, GColorClear);
  window_set_window_handlers(s_win, (WindowHandlers){.load = win_load, .unload = win_unload});
  window_stack_push(s_win, false);
  tick_timer_service_subscribe(MINUTE_UNIT, tick);
  unobstructed_area_service_subscribe((UnobstructedAreaHandlers){.change = ub_change}, NULL);
  app_message_register_inbox_received(inbox);
  app_message_open(256, 32);
  app_timer_register(1500, say_hello, NULL);
  s_was_quiet = quiet_now();
#if defined(CAPTURE)
  tick_timer_service_subscribe(SECOND_UNIT, cap_tick);
#else
  subscribe_triggers();
#endif
  APP_LOG(APP_LOG_LEVEL_INFO, "Look at me! ready, heap free %u", (unsigned)heap_bytes_free());
#if defined(DEMO_OPEN)
  start_glance(true, 0, 0);
#endif
}
static void deinit(void) {
  if (s_timer) app_timer_cancel(s_timer);
  accel_data_service_unsubscribe();
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_win);
  eye_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
