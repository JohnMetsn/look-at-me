# Look at me! — design decisions

Watchface for `emery` (Pebble Time 2, 200×228) and `gabbro` (Pebble Round 2, 260×260).
Concept sketches: `concept/eye-sketches-v1.html`.

## Brief (owner, 2026-11-19)

- A realistic almond-shaped feminine eye with a slight East Asian look. It should feel uncanny.
- The eye is closed by default. A glance opens it: it searches, finds the wearer, and keeps looking at them while it
  counter-rotates against wrist tilt. Then it closes again.
- Owner picks from sketch v1: layout **C** (time under the eye), skin **Night**, liner and lashes as in the mockup.
- The settings page offers:
  - iris colour: presets plus any custom colour;
  - pupil shape;
  - time font: Orbitron, Michroma or Chakra Petch;
  - pixels: flat or dithered;
  - wake trigger: tilt (default) or shake;
  - how long the eye stares.

## Why C and not Alloy

CLAUDE.md prefers Alloy, so the reasons for C are recorded here before any code:

1. Each animation frame shades ~25–30k pixels one by one: a ray-traced eyeball, a remapped iris texture and lid
   shadows. Alloy's XS interpreter cannot do that per-pixel work at about 15 fps.
2. Direct frame-buffer access (`graphics_capture_frame_buffer` + `gbitmap_get_data_row_info`) is needed to write
   palette bytes, including round rows on gabbro.
3. Memory must be tight and predictable: textures are loaded once, with no bitmap churn
   (see the Alloy PebbleBitmap leak note).

Apps are compiled for `-mcpu=cortex-m3` (soft float), so the per-pixel path is integer and fixed-point only:

- sqrt comes from LUTs;
- trig comes from Pebble's `sin_lookup`/`cos_lookup`;
- floats are used only once per frame or when settings change.

## Rendering model (per frame)

- Static rows (brow and cheek, plus time and date) are redrawn only on a full redraw: minute tick, settings or
  appearance change. During an animation, only the eye band (`cy − 0.78a … cy + 0.52a`) is recomputed. The frame
  buffer keeps the other rows.
- **Eyeball:** a sphere of radius `a` with orthographic projection. The gaze rotation is Ry(ax)·Rx(ay). The iris is
  treated as a flat disc of radius 0.43, which is anatomically right because the iris is a flat plane under the cornea.
  Pupil dilation remaps the iris radius, so the fibres compress the way they do in a real eye.
- **Iris texture:** an offline 96×96 grey texture made by `tools/make_textures.py` with the same noise as the sketch.
  The low 6 bits hold luminance, bit 6 marks the heterochromia sector and bit 7 marks the inner hazel zone. On the
  watch the texture is recoloured from a 64-entry LUT built from the chosen colour, so any colour works.
- **Skin and sclera:** hand-picked palette ramps per material. Plain per-channel quantisation produced olive and
  grey bands in the sketch.
- **Specular window:** fixed in screen space while the iris moves under it. This is the main depth cue.
- **Liner, lashes and brow:** drawn with GContext after the frame buffer is released.
- **Dither:** a 4×4 Bayer matrix. It is a setting, so both modes can be compared on the real panels.

## Glance timeline

Total ≈ 7.5 s for the "Normal" stare:

| Time | What happens |
|---|---|
| 0–0.3 s | lid flutter |
| 0.3–0.8 s | the eye opens, looking away |
| 0.85 s | saccade with overshoot |
| 1.1 s | lock on the wearer, pupil dilates |
| then | tracking with micro-saccades; slow blink at 3.4 s |
| 6.3–7.5 s | the eye closes, the gaze sinks |

"Short" and "Long" stare settings move the close time to 4 s or 9.3 s.

**Tracking:** the accelerometer pose at the moment the eye wakes is the reference "wearer is straight in front".
After that the gaze is the counter-rotation:

- `gx = −Δx / 550`
- `gy = +Δy / 380`
- `Δ` is in mG and the value is smoothed.

## Triggers and battery

- **Tilt (default):** the raise-to-look detector from Pejsek a Kočička. It samples at 10 Hz in batches of 10, so the
  watch wakes once per second.
- **Shake:** the system tap service, which is the lowest-power option.
- **During the animation only:** 25 Hz samples drive the gaze. The face then returns to the trigger mode.
- **Quiet Time:** the eye sleeps unless the setting says otherwise.

## Layout C geometry

| | emery 200×228 | gabbro 260×260 |
|---|---|---|
| eye centre / half-width a | (96, 92), 94 | (126, 104), 112 |
| time centre / font px | (100, 176), Orbitron 42 | (130, 194), Orbitron 46 |
| date centre / font px | (100, 212), 15 | (130, 232), 16 |

Timeline Quick View: the whole scene is lifted by the covered height, and glances pause while it is shown.

## Unverified until tested on the physical watches

- Frame time on real hardware.
- Battery cost of tilt mode with this face.
- How the Night skin ramp and the dithering look on the reflective Time 2 panel and on Round 2.
