#!/usr/bin/env python3
"""Store assets and the Reddit animation for Look at me!

Input: capture/<platform>/fNNN.png and pose_N.png from tools/capture.sh (raw palette, no watch frame).
Run:   uv run --with pillow python tools/make_media.py
"""
import glob
import math
import os

from PIL import Image, ImageDraw, ImageFont, ImageChops

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..'))
CAP = os.path.join(ROOT, 'capture')
STORE = os.path.join(ROOT, 'store')
FONT = os.path.join(ROOT, 'resources', 'fonts', 'Orbitron-Bold.ttf')
FPS = 15
W, H = 1280, 720
BG = (8, 6, 7)
ACCENT = (143, 182, 230)


def font(size):
    return ImageFont.truetype(FONT, size)


def frames(pl):
    return [Image.open(p).convert('RGB') for p in sorted(glob.glob(os.path.join(CAP, pl, 'f*.png')))]


def pose(pl, i):
    return Image.open(os.path.join(CAP, pl, 'pose_%d.png' % i)).convert('RGB')


def round_mask(size):
    m = Image.new('L', size, 0)
    ImageDraw.Draw(m).ellipse((0, 0, size[0] - 1, size[1] - 1), fill=255)
    return m


# ---------------------------------------------------------------- watches
def watch_time2(screen, s=2):
    sw, sh = screen.width * s, screen.height * s
    bw, bh = sw + 64, sh + 84
    img = Image.new('RGBA', (bw, bh), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, bw - 1, bh - 1), radius=46, fill=(30, 30, 33, 255), outline=(70, 70, 76, 255), width=3)
    d.rounded_rectangle((22, 30, bw - 23, bh - 31), radius=14, fill=(0, 0, 0, 255))
    img.paste(screen.resize((sw, sh), Image.NEAREST), (32, 42))
    return img


def watch_round2(screen, s=2):
    sw = screen.width * s
    bw = sw + 70
    img = Image.new('RGBA', (bw, bw), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((0, 0, bw - 1, bw - 1), fill=(30, 30, 33, 255), outline=(70, 70, 76, 255), width=3)
    d.ellipse((24, 24, bw - 25, bw - 25), fill=(0, 0, 0, 255))
    scr = screen.resize((sw, sw), Image.NEAREST)
    img.paste(scr, (35, 35), round_mask((sw, sw)))
    return img


# ---------------------------------------------------------------- intro text
LINES = [[('ASHLEY,', (255, 255, 255))], [('LOOK', (255, 255, 255)), ('AT', (255, 255, 255)), ('ME!!!', ACCENT)]]
WORD_AT = [0, 9, 15, 21]  # frames at 15 fps


def text_layer(t, dim):
    """t = frame index; dim 0..1 = how far the text has receded into the background."""
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    f = font(150)
    gap = 40
    ys = [H // 2 - 175, H // 2 + 15]
    k = 0
    for li, line in enumerate(LINES):
        widths = [f.getbbox(w)[2] - f.getbbox(w)[0] for w, _ in line]
        x = (W - (sum(widths) + gap * (len(line) - 1))) // 2
        for (word, col), ww in zip(line, widths):
            start = WORD_AT[k]
            k += 1
            if t >= start:
                p = min(1.0, (t - start) / 4.0)
                scale = 1.35 - 0.35 * (1 - (1 - p) ** 3)
                alpha = int(255 * min(1.0, p * 1.6))
                c = tuple(int(col[i] * (1 - dim) + 58 * dim) for i in range(3))
                wimg = Image.new('RGBA', (ww + 40, 200), (0, 0, 0, 0))
                ImageDraw.Draw(wimg).text((20 - f.getbbox(word)[0], 20), word, font=f, fill=c + (alpha,))
                if scale != 1:
                    wimg = wimg.resize((int(wimg.width * scale), int(wimg.height * scale)), Image.LANCZOS)
                cx, cy = x + ww // 2, ys[li] + 100
                layer.alpha_composite(wimg, (cx - wimg.width // 2, cy - wimg.height // 2))
            x += ww + gap
    if dim > 0:  # receding: grow slightly, like it falls behind the watches
        s = 1 + 0.12 * dim
        big = layer.resize((int(W * s), int(H * s)), Image.LANCZOS)
        layer = big.crop(((big.width - W) // 2, (big.height - H) // 2, (big.width - W) // 2 + W, (big.height - H) // 2 + H))
    return layer


def reddit_animation():
    fe, fg = frames('emery'), frames('gabbro')
    n = min(len(fe), len(fg))
    intro, fade = 36, 12
    total = intro + n + 8
    out = []
    t2 = watch_time2(fe[0])
    r2 = watch_round2(fg[0])
    gap = 70
    x0 = (W - (t2.width + gap + r2.width)) // 2
    pos_t2 = (x0, (H - t2.height) // 2)
    pos_r2 = (x0 + t2.width + gap, (H - r2.height) // 2)
    for t in range(total):
        frame = Image.new('RGBA', (W, H), BG + (255,))
        dim = 0.0 if t < intro else min(1.0, (t - intro) / fade)
        dim = 1 - (1 - dim) ** 2
        frame.alpha_composite(text_layer(t, dim))
        if t >= intro:
            i = min(n - 1, t - intro)
            a = min(1.0, (t - intro) / 8.0)
            wl = Image.new('RGBA', (W, H), (0, 0, 0, 0))
            wl.alpha_composite(watch_time2(fe[i]), pos_t2)
            wl.alpha_composite(watch_round2(fg[i]), pos_r2)
            if a < 1:
                wl.putalpha(ImageChops.multiply(wl.getchannel('A'), Image.new('L', (W, H), int(255 * a))))
            frame.alpha_composite(wl)
        out.append(frame.convert('RGB'))
    return out


def save_gif(frames_rgb, path, fps=FPS):
    pal = [f.quantize(colors=128, method=Image.MEDIANCUT, dither=Image.NONE) for f in frames_rgb]
    pal[0].save(path, save_all=True, append_images=pal[1:], duration=int(1000 / fps), loop=0, optimize=False, disposal=1)


# ---------------------------------------------------------------- store
def store_assets():
    os.makedirs(os.path.join(STORE, 'store-time2'), exist_ok=True)
    os.makedirs(os.path.join(STORE, 'store-round2'), exist_ok=True)
    names = {1: '2_steel_blue', 2: '3_cat_gold', 3: '4_hazel_dithered', 5: '5_demon'}
    for pl, folder in (('emery', 'store-time2'), ('gabbro', 'store-round2')):
        for i, name in names.items():
            pose(pl, i).save(os.path.join(STORE, folder, name + '.png'))
        fr = frames(pl)
        save_gif(fr, os.path.join(STORE, folder, '1_look_at_me.gif'))
    # icons: the open steel-blue eye, cropped around the iris
    e = pose('emery', 1)
    crop = e.crop((22, 22, 162, 162))
    crop.resize((144, 144), Image.LANCZOS).save(os.path.join(STORE, 'icon_large_144.png'))
    crop.resize((80, 80), Image.LANCZOS).save(os.path.join(STORE, 'icon_small_80.png'))
    # banners 720x320
    for pl, name in (('emery', 'banner_time2.png'), ('gabbro', 'banner_round2.png')):
        b = Image.new('RGB', (720, 320), BG)
        p = pose(pl, 1)
        eye = p.crop((0, 0, p.width, 150 if pl == 'emery' else 175))
        sc = 320 / eye.height
        eye = eye.resize((int(eye.width * sc), 320), Image.NEAREST)
        b.paste(eye, (720 - eye.width + (40 if pl == 'gabbro' else 0), 0))
        g = Image.new('L', (720, 320), 0)
        gd = ImageDraw.Draw(g)
        for x in range(380):
            gd.line((x, 0, x, 320), fill=int(255 * (1 - x / 380) ** 0.7))
        b.paste(Image.new('RGB', (720, 320), BG), (0, 0), g)
        d = ImageDraw.Draw(b)
        d.text((36, 100), 'LOOK', font=font(64), fill=(255, 255, 255))
        d.text((36, 172), 'AT ME!', font=font(64), fill=ACCENT)
        b.save(os.path.join(STORE, name))


def main():
    store_assets()
    os.makedirs(os.path.join(STORE, 'reddit'), exist_ok=True)
    anim = reddit_animation()
    save_gif(anim, os.path.join(STORE, 'reddit', 'look-at-me.gif'))
    anim[36 + 30].save(os.path.join(STORE, 'reddit', 'look-at-me_still.png'))
    print('frames', len(anim))
    for p in sorted(glob.glob(os.path.join(STORE, '**', '*.*'), recursive=True)):
        print(os.path.relpath(p, ROOT), os.path.getsize(p))


if __name__ == '__main__':
    main()
