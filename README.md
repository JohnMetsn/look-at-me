# Look at me!

A watchface for **Pebble Time 2** and **Pebble Round 2**. A realistic eye sleeps on your wrist.
When you look at your watch, it opens, searches, finds you and keeps looking at you.

![Look at me! on Pebble Time 2 and Pebble Round 2](store/reddit/look-at-me.gif)

**Get it:** [Pebble app store](https://apps.rePebble.com/363ab94be8474e259b7bd7e5)

## What it does

- **It wakes when you look.** Raise your wrist and hold it still for a moment. You can also choose a shake of the
  wrist, or the moment your backlight turns on.
- **It finds you.** The lids flutter and the eye opens. It glances somewhere else first, then jumps to you with two
  quick saccades, and the pupil widens when it locks on.
- **It keeps looking at you.** The accelerometer tracks your wrist, and the eye turns the opposite way, so it stays
  on you while you move. It blinks rarely and slowly. Tiny micro-saccades keep it alive, and the upper lid follows
  the gaze like a real one.
- **Then it goes back to sleep.** The time and date are always readable, even with the eye closed.

## How it's drawn

Everything is drawn in real time, pixel by pixel. Each frame ray-traces the eyeball as a sphere:

- the iris is a flat disc under the cornea, and its texture compresses when the pupil dilates;
- the veins sit on the eyeball and turn with it;
- the reflection of a window rides on the cornea;
- the lids, crease, liner and lashes follow the eye's state.

All of it is fixed-point C, because Pebble apps run without a hardware FPU. During a glance only the pixels around
the eye are redrawn, which gives about 15 fps on a real Pebble Time 2.

## Settings

- **Iris:** 15 presets in two groups, plus any custom colour.
  - Standard: Espresso, Hazel, Forest, Steel blue, Ice grey, Amber.
  - Unusual: Violet, Albino red, Ghost, Cat gold, Neon cyan, Toxic, Split heterochromia, Void, Demon.
- **Pupil:** like the iris, round, or a cat-like slit.
- **Black surround:** only the lids show. Turn it off for skin across the whole screen.
- **Window reflection** on the eye, on or off.
- **Time font:** Orbitron, Michroma or Chakra Petch.
- **Pixels:** flat 64-colour palette, or dithered for softer shading.
- **Wake the eye:** tilt, shake or both, and optionally when the backlight turns on.
- **How long it stares:** short, normal or long.
- **Veins:** none, subtle or bloodshot.
- **Clock:** 12 h, 24 h or like your watch.
- **Quiet Time:** the eye sleeps during Quiet Time.

## Build

You need the Pebble SDK 4.44 or newer and pebble-tool 5.2 or newer.

```sh
pebble build
pebble install --emulator emery     # Pebble Time 2
pebble install --emulator gabbro    # Pebble Round 2
```

Development builds:

- `DEMO_OPEN=1 pebble build` keeps the eye open and tracking.
- `DEMO_DITHER=1` forces the dithered palette.
- `CAPTURE=1` / `CAPTURE=2` build frame-by-frame capture versions for `tools/capture.sh`.

The iris, vein and skin-noise textures come from `tools/make_textures.py`. `DESIGN.md` describes the rendering and
the glance timeline.

## License

Code: MIT, see `LICENSE`. Fonts: SIL Open Font License 1.1, see `resources/fonts/OFL-*.txt`.

It's a hobby project, so take it apart and make your own eye. If you enjoy it and feel like buying me a coffee:
https://ko-fi.com/U1O02AUNZB
