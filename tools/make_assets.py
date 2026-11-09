"""Amplify-OS "Pearl" design system: logo extraction + icon set + wallpaper."""
import os, math
from PIL import Image, ImageDraw, ImageFilter

OUT = '/data/assets'
os.makedirs(OUT + '/png', exist_ok=True)
os.makedirs(OUT + '/px', exist_ok=True)

# ---------------------------------------------------------------- logo mark
src = Image.open('/data/logo_src.jpg').convert('RGB')
W, H = src.size
# the mark sits in the upper-middle of the artwork
crop = src.crop((int(W*0.22), int(H*0.14), int(W*0.78), int(H*0.52)))
crop = crop.resize((crop.width*2, crop.height*2), Image.LANCZOS)
px = crop.load()
bg = (237, 224, 234)
mask = Image.new('L', crop.size, 0)
mp = mask.load()
for y in range(crop.height):
    for x in range(crop.width):
        r, g, b = px[x, y]
        d = math.sqrt((r-bg[0])**2 + (g-bg[1])**2 + (b-bg[2])**2)
        a = (d - 8) / 45.0
        a = 0.0 if a < 0 else (1.0 if a > 1 else a)
        mp[x, y] = int(a * 255)
logo = crop.convert('RGBA')
logo.putalpha(mask)
logo = logo.crop(logo.getbbox())
logo.save(OUT + '/logo_mark.png')
print('logo', logo.size)

# ---------------------------------------------------------------- palette
G1 = (239, 177, 63)    # #EFB13F
G2 = (222, 126, 92)    # #DE7E5C
G3 = (188, 74, 126)    # #BC4A7E

S = 512          # master icon size
R = 124          # corner radius (squircle-ish)
SW = 34          # stroke width


def gradient_tile():
    """Diagonal orange->coral->magenta tile with soft sheen."""
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    grad = Image.new('RGBA', (S, S))
    gp = grad.load()
    for y in range(S):
        for x in range(S):
            t = (x + y) / (2.0 * (S - 1))
            if t < 0.5:
                u = t * 2
                c = tuple(int(G1[i] + (G2[i] - G1[i]) * u) for i in range(3))
            else:
                u = (t - 0.5) * 2
                c = tuple(int(G2[i] + (G3[i] - G2[i]) * u) for i in range(3))
            gp[x, y] = c + (255,)
    # sheen: soft white glow in the top-left
    sheen = Image.new('L', (S, S), 0)
    sd = ImageDraw.Draw(sheen)
    sd.ellipse((-S*0.35, -S*0.55, S*0.95, S*0.45), fill=70)
    sheen = sheen.filter(ImageFilter.GaussianBlur(60))
    white = Image.new('RGBA', (S, S), (255, 255, 255, 255))
    grad = Image.composite(Image.alpha_composite(grad, Image.merge('RGBA', (
        white.split()[0], white.split()[1], white.split()[2], sheen))), grad,
        Image.new('L', (S, S), 255))
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S-1, S-1), R, fill=255)
    img.paste(grad, (0, 0), mask)
    return img


TILE = gradient_tile()


class Pen:
    def __init__(self, img, color=(255, 255, 255, 255), w=SW):
        self.d = ImageDraw.Draw(img)
        self.c = color
        self.w = w

    def line(self, pts, w=None):
        w = w or self.w
        self.d.line(pts, fill=self.c, width=w, joint='curve')
        for (x, y) in pts:
            r = w / 2.0
            self.d.ellipse((x-r, y-r, x+r, y+r), fill=self.c)

    def rrect(self, box, rad, w=None):
        self.d.rounded_rectangle(box, rad, outline=self.c, width=w or self.w)

    def circle(self, cx, cy, r, w=None):
        self.d.ellipse((cx-r, cy-r, cx+r, cy+r), outline=self.c, width=w or self.w)

    def dot(self, cx, cy, r):
        self.d.ellipse((cx-r, cy-r, cx+r, cy+r), fill=self.c)

    def arc(self, box, a0, a1, w=None):
        self.d.arc(box, a0, a1, fill=self.c, width=w or self.w)


def new_icon():
    return TILE.copy()


def glyph_files(p):
    p.line([(150, 118), (312, 118), (362, 168), (362, 394), (150, 394), (150, 118)])
    p.line([(306, 120), (306, 172), (358, 172)])
    p.line([(206, 250), (306, 250)])
    p.line([(206, 316), (306, 316)])


def glyph_folder(p):
    p.line([(130, 370), (130, 156), (232, 156), (272, 202), (382, 202), (382, 370), (130, 370)])


def glyph_browser(p):
    p.circle(256, 256, 132)
    p.line([(126, 256), (386, 256)])
    p.d.ellipse((186, 124, 326, 388), outline=p.c, width=p.w)


def glyph_mail(p):
    p.rrect((124, 160, 388, 352), 34)
    p.line([(150, 186), (256, 274), (362, 186)])


def glyph_terminal(p):
    p.rrect((116, 138, 396, 374), 40)
    p.line([(176, 218), (232, 260), (176, 302)])
    p.line([(264, 306), (338, 306)])


def glyph_settings(p):
    p.circle(256, 256, 74)
    for i in range(8):
        a = math.radians(i * 45)
        x0, y0 = 256 + math.cos(a) * 118, 256 + math.sin(a) * 118
        x1, y1 = 256 + math.cos(a) * 166, 256 + math.sin(a) * 166
        p.line([(x0, y0), (x1, y1)], w=30)


def glyph_gallery(p):
    p.rrect((116, 140, 396, 372), 36)
    p.dot(196, 208, 22)
    p.line([(146, 336), (232, 252), (296, 314), (340, 274), (376, 316)])


def glyph_music(p):
    p.line([(216, 336), (216, 150), (368, 118), (368, 302)])
    p.dot(184, 340, 46)
    p.dot(336, 306, 46)


def glyph_calendar(p):
    p.rrect((120, 156, 392, 386), 34)
    p.line([(120, 226), (392, 226)])
    p.line([(184, 122), (184, 180)])
    p.line([(328, 122), (328, 180)])
    for cx in (196, 256, 316):
        p.dot(cx, 300, 15)


def glyph_notes(p):
    p.rrect((140, 116, 372, 396), 34)
    p.line([(196, 190), (316, 190)])
    p.line([(196, 256), (316, 256)])
    p.line([(196, 322), (270, 322)])


def glyph_store(p):
    p.rrect((132, 186, 380, 388), 38)
    p.arc((198, 96, 314, 250), 180, 360)


def glyph_trash(p):
    p.line([(136, 172), (376, 172)])
    p.line([(206, 172), (216, 126), (296, 126), (306, 172)])
    p.line([(172, 172), (192, 388), (320, 388), (340, 172)])
    p.line([(228, 224), (232, 336)])
    p.line([(284, 224), (280, 336)])


GLYPHS = {
    'files': glyph_files, 'folder': glyph_folder, 'browser': glyph_browser,
    'mail': glyph_mail, 'terminal': glyph_terminal, 'settings': glyph_settings,
    'gallery': glyph_gallery, 'music': glyph_music, 'calendar': glyph_calendar,
    'notes': glyph_notes, 'store': glyph_store, 'trash': glyph_trash,
}

icons = {}
for name, fn in GLYPHS.items():
    img = new_icon()
    fn(Pen(img))
    icons[name] = img

# amplify: white pearl tile with the real logo mark
amp = Image.new('RGBA', (S, S), (0, 0, 0, 0))
tile = Image.new('RGBA', (S, S))
tp = tile.load()
for y in range(S):
    for x in range(S):
        u = (x + y) / (2.0 * (S - 1))
        c = tuple(int(255 + (v - 255) * u) for v in (241, 227, 236))
        tp[x, y] = c + (255,)
m = Image.new('L', (S, S), 0)
ImageDraw.Draw(m).rounded_rectangle((0, 0, S-1, S-1), R, fill=255)
amp.paste(tile, (0, 0), m)
lw = int(S * 0.62)
lh = int(lw * logo.height / logo.width)
amp.alpha_composite(logo.resize((lw, lh), Image.LANCZOS),
                    ((S - lw) // 2, (S - lh) // 2))
icons['amplify'] = amp

ORDER = ['amplify', 'files', 'folder', 'browser', 'mail', 'terminal',
         'settings', 'gallery', 'music', 'calendar', 'notes', 'store', 'trash']

for n in ORDER:
    icons[n].save(f'{OUT}/png/{n}.png')
    icons[n].resize((48, 48), Image.LANCZOS).save(f'{OUT}/px/{n}.png')

# logo bitmaps for the OS (wallpaper watermark + taskbar)
logo.resize((220, int(220 * logo.height / logo.width)), Image.LANCZOS).save(OUT + '/px/logo220.png')
logo.resize((24, max(1, int(24 * logo.height / logo.width))), Image.LANCZOS).save(OUT + '/px/logo24.png')

# ---------------------------------------------------------------- wallpaper
WW, WH = 2560, 1440
wall = Image.new('RGB', (WW, WH))
wp = wall.load()
top, mid, bot = (249, 238, 244), (241, 227, 236), (230, 216, 229)
for y in range(WH):
    t = y / (WH - 1)
    if t < 0.5:
        u = t * 2
        c = tuple(int(top[i] + (mid[i] - top[i]) * u) for i in range(3))
    else:
        u = (t - 0.5) * 2
        c = tuple(int(mid[i] + (bot[i] - mid[i]) * u) for i in range(3))
    for x in range(WW):
        wp[x, y] = c
glow = Image.new('L', (WW, WH), 0)
ImageDraw.Draw(glow).ellipse((WW*0.18, WH*0.05, WW*0.82, WH*0.95), fill=120)
glow = glow.filter(ImageFilter.GaussianBlur(260))
wall = Image.composite(Image.new('RGB', (WW, WH), (255, 255, 255)), wall, glow)
wall = wall.convert('RGBA')
markw = 780
mark = logo.resize((markw, int(markw * logo.height / logo.width)), Image.LANCZOS)
ma = mark.split()[3].point(lambda a: int(a * 0.26))
mark.putalpha(ma)
wall.alpha_composite(mark, ((WW - mark.width)//2, (WH - mark.height)//2 - 40))
wall.convert('RGB').save(OUT + '/Amplify-OS_wallpaper.png')
print('done')
