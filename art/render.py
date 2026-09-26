"""SVG -> PNG through a headless Chromium browser (Edge, or Chrome/another one named by $CHROMIUM), and a
mock of the game's radio HUD to judge a logo in place.

  python render.py [SVG]    SVG (default logo.svg, viewBox 0 0 128 64) -> <stem>_512.png, <stem>_128.png,
                            and build\\<stem>_preview.png

The PNGs follow the game's logo convention: black RGB, alpha = coverage. Only alpha shows in game: the HUD
tints the logo white (RadioStations.swf places the holder with cxform mult 0, add 255). 512x256 is the
logo for a texture of our own (Scaleform samples it at full size, sharper on a 4K screen); 128x64 is the
size of the game's logos. The preview uses the HUD's metal backing from the workspace's extracted\\ folder
(game art, not in this repo; `tools\\extract.ps1 export '^BACKING_9SLICE_METAL_1$' extracted\\ui-style`)
and falls back to a plain dark panel without it.
"""
import html, os, pathlib, subprocess, sys
from PIL import Image

HERE = pathlib.Path(__file__).parent
BUILD = HERE / 'build'
EXTRACTED = HERE.parent.parent.parent / 'extracted'
BROWSERS = [os.environ.get('CHROMIUM'), r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
	r'C:\Program Files\Google\Chrome\Application\chrome.exe']

def browser():
	for b in BROWSERS:
		if b and os.path.exists(b):
			return b
	sys.exit('no Edge or Chrome found; set CHROMIUM to a Chromium-based browser')

def shoot(page, out, w, h):
	"""Screenshot of an HTML file at w x h, transparent where the page has no background."""
	out.unlink(missing_ok=True)
	subprocess.run([browser(), '--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run',
		f'--user-data-dir={BUILD / "browser-profile"}', '--default-background-color=00000000',
		'--force-device-scale-factor=1', f'--window-size={w},{h}', f'--screenshot={out}', page.as_uri()],
		check=True, capture_output=True, timeout=60)
	if not out.exists():
		sys.exit(f'no screenshot of {page}')

def page(name, body, style=''):
	p = BUILD / f'_{name}.html'
	p.write_text(f'<!doctype html><html><head><meta charset="utf-8"><style>body {{ margin: 0 }} {style}</style></head>'
		f'<body>{body}</body></html>', encoding='utf-8')
	return p

def render_svg(svg):
	"""<stem>_512.png and <stem>_128.png next to the SVG."""
	BUILD.mkdir(exist_ok=True)
	raw = BUILD / f'_{svg.stem}_raw.png'
	shoot(page(svg.stem, f'<img src="{svg.as_uri()}" width="512" height="256" style="display:block">'), raw, 512, 256)
	alpha = Image.open(raw).convert('RGBA').crop((0, 0, 512, 256)).getchannel('A')
	for w, h in ((512, 256), (128, 64)):
		im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
		im.putalpha(alpha if w == 512 else alpha.resize((w, h), Image.Resampling.BOX))
		im.save(svg.parent / f'{svg.stem}_{w}.png')

def hud(logo, name='SDRADIO', title='Artist - Some Song', u=2):
	"""The RadioStations widget at u px per movie unit: holder 128x64 at (0,0), station name at (140,6), song
	title at (140,30.4), backing from (-20,-15.4)."""
	backing = EXTRACTED / 'ui-style' / 'BACKING_9SLICE_METAL_1.png'
	panel = (f'border-image: url({backing.as_uri()}) 24 fill / {8 * u}px stretch' if backing.exists()
		else 'background: #111; border: 2px solid #555')
	return (f'<div style="display:inline-flex; align-items:flex-start; padding:{15.4 * u}px {20 * u}px; {panel}">'
		f'<img src="{logo.as_uri()}" style="width:{128 * u}px; height:{64 * u}px; filter:brightness(0) invert(1)">'
		f'<div style="margin-left:{12 * u}px; color:#fff; font-family:Bahnschrift,sans-serif; white-space:nowrap">'
		f'<div style="margin-top:{6 * u}px; font-size:{19 * u}px; font-weight:700; line-height:{24 * u}px">{html.escape(name)}</div>'
		f'<div style="font-size:{14 * u}px; line-height:{20 * u}px">{html.escape(title)}</div></div></div>')

def preview(svg):
	"""build\\<stem>_preview.png: the logo at 3x, then in the HUD mock from the 512 and the 128 texture."""
	big, small = svg.parent / f'{svg.stem}_512.png', svg.parent / f'{svg.stem}_128.png'
	label = 'color:#ccc; font:20px Segoe UI,sans-serif; margin:18px 0 6px'
	body = (f'<div style="background:#181818; line-height:0"><img src="{big.as_uri()}" width="1536" height="768" '
		f'style="filter:brightness(0) invert(1)"></div><div style="padding:0 24px 24px">'
		f'<div style="{label}">512x256 texture</div>{hud(big)}<div style="{label}">128x64 texture</div>{hud(small)}</div>')
	style = 'body { width: 1536px; background: radial-gradient(circle at 20% 30%, #3a2a4a, #0d0f16 60%) }'
	shoot(page(f'{svg.stem}_preview', body, style), BUILD / f'{svg.stem}_preview.png', 1536, 768 + 2 * (192 + 56))

if __name__ == '__main__':
	svg = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else HERE / 'logo.svg'
	render_svg(svg)
	preview(svg)
	print(f'{svg.stem}_512.png, {svg.stem}_128.png, build\\{svg.stem}_preview.png')
