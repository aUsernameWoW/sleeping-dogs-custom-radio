"""The station logo, 私家台 ("private station", as in 私家車, a private car) in neon tubes: generates
logo.svg, then renders it (render.py: logo_512.png, logo_128.png, build\\logo_preview.png).

  python neon.py            logo.svg + renders
  python neon.py debug [W]  also build\\neon_debug.png: each character's tubes over the Noto Sans SC glyph
                            (weight W, default 700; the font must be installed) on a 1-unit grid

Tubes are straight glass with circular bends, like a real sign. Each character is drawn in a 40x40 box,
y down, the em box of Noto Sans SC at font-size 40, whose Bold glyphs the skeletons were traced over.
A tube is a list of points (x, y) or (x, y, r), r being the bend radius at that vertex (default BEND);
'Z' at the end closes it. A leading 'S', 'E' or 'SE' marks ends that stop short of the tubes drawn after
this one, leaving GAP between the glass as where one tube passes in front of another on a sign; unmarked
ends and crossings join, as the strokes do in the typeface. Every run reports tube pairs that neither
join nor clear GAP (a hairline slit reads as a flaw); keep that list empty.
"""
import math, pathlib, sys
import render

HERE = pathlib.Path(__file__).parent
STROKE = 4.0      # tube width
BEND = 2.0        # default bend radius
GAP = 1.0         # between a gapped end and the tube in front of it
LETTER_GAP = 4.4  # between the lit extents of neighboring characters
GLOW = 2.2        # blur radius of the halo; the HUD keeps only alpha, so the glow is a soft alpha fringe
GLOW_ALPHA = 0.6

# Rules: corners bend with BEND..2.5; a long 撇 gets one wide bend (18) for the brush curve; a stroke that
# meets another at a T stops short (GAP) and comes first, so the other one is drawn in front; branches join.
CHARS = {
	'私': [
		[(4.2, 14.6), (16.8, 14.6)],                    # 禾: 横
		[(10.2, 18.0), (3.4, 29.0)],                    # 撇
		[(12.8, 19.8), (16.6, 23.8)],                   # 点
		['S', (10.8, 8.2), (10.8, 36.6)],               # 竖
		[(16.8, 3.4), (4.4, 6.8)],                      # top 撇
		['E', (27.6, 3.4), (23.8, 20.0, 18), (19.6, 32.6, 2.5), (35.2, 30.4)],  # 厶: 撇折
		[(30.8, 20.0), (37.2, 34.2)],                   # 点
	],
	'家': [
		['S', (20.4, 22.0), (12.2, 25.8, 18), (3.8, 27.8)],  # 豕: 撇 (from the 弯钩)
		['S', (23.6, 27.0), (14.4, 31.4, 18), (3.6, 34.8)],  # 撇
		['S', (16.8, 19.6), (22.8, 25.4, 6), (22.8, 36.8, 3.5), (17.2, 36.4)],  # 弯钩
		[(33.4, 17.6), (27.8, 23.6, 1.5), (37.2, 34.0)],   # 撇, 捺
		[(21.4, 15.0), (12.6, 19.6, 18), (4.0, 21.4)],  # 撇
		[(10.8, 13.0), (29.2, 13.0)],                   # 横
		['E', (19.7, 0.4), (20.2, 2.0)],                # 宀: 点
		[(5.2, 12.0), (5.2, 7.2), (34.8, 7.2), (34.8, 12.0)],  # 点, 横钩
	],
	'台': [
		['E', (18.0, 2.4), (12.6, 9.6, 18), (6.0, 15.8, 2.5), (30.4, 14.4)],  # 厶: 撇折
		[(25.8, 7.0), (34.8, 17.0)],                    # 点
		[(8.8, 23.0, 2.5), (31.2, 23.0, 2.5), (31.2, 36.2, 2.5), (8.8, 36.2, 2.5), 'Z'],  # 口
	],
}

def parse(tube):
	"""(gap flags, closed, points with radii)."""
	flags = tube[0] if isinstance(tube[0], str) else ''
	pts = [p for p in tube if not isinstance(p, str)]
	return flags, tube[-1] == 'Z', [p if len(p) == 3 else (p[0], p[1], BEND) for p in pts]

def pieces(closed, pts):
	"""The tube as ('L', a, b) segments and ('A', p1, p2, r, sweep, center, angle0, angle1) bends, in order."""
	n = len(pts)

	def bend(i):
		(ax, ay, _), (px, py, r), (bx, by, _) = pts[i - 1], pts[i], pts[(i + 1) % n]
		d1x, d1y, d2x, d2y = px - ax, py - ay, bx - px, by - py
		l1, l2 = math.hypot(d1x, d1y), math.hypot(d2x, d2y)
		turn = math.atan2(d1x * d2y - d1y * d2x, d1x * d2x + d1y * d2y)
		if abs(turn) < 1e-6:
			return None
		t = min(r * math.tan(abs(turn) / 2), l1 / 2, l2 / 2)
		r = t / math.tan(abs(turn) / 2)
		p1 = (px - d1x / l1 * t, py - d1y / l1 * t)
		p2 = (px + d2x / l2 * t, py + d2y / l2 * t)
		side = 1 if turn > 0 else -1  # turn > 0 is clockwise on a y-down screen: the center is to the right
		c = (p1[0] - d1y / l1 * r * side, p1[1] + d1x / l1 * r * side)
		a0 = math.atan2(p1[1] - c[1], p1[0] - c[0])
		return p1, p2, r, 1 if turn > 0 else 0, c, a0, a0 + turn

	bends = {i: bend(i) for i in (range(n) if closed else range(1, n - 1))}
	out = []
	pos = (bends[0][1] if bends[0] else pts[0][:2]) if closed else pts[0][:2]
	for i in (list(range(1, n)) + [0]) if closed else range(1, n):
		b = bends.get(i)
		if b:
			out.append(('L', pos, b[0]))
			out.append(('A',) + b)
			pos = b[1]
		else:
			out.append(('L', pos, pts[i][:2]))
			pos = pts[i][:2]
	return out

def svg_path(tube):
	_, closed, pts = parse(tube)
	f = lambda v: f'{v:.2f}'.rstrip('0').rstrip('.')
	ps = pieces(closed, pts)
	d = f'M{f(ps[0][1][0])} {f(ps[0][1][1])}'
	for p in ps:
		if p[0] == 'L':
			d += f' L{f(p[2][0])} {f(p[2][1])}'
		else:
			d += f' A{f(p[3])} {f(p[3])} 0 0 {p[4]} {f(p[2][0])} {f(p[2][1])}'
	return d + (' Z' if closed else '')

def samples(tube, step=0.1):
	"""Points along the tube's center line, bends included."""
	_, closed, pts = parse(tube)
	out = []
	for p in pieces(closed, pts):
		if p[0] == 'L':
			(ax, ay), (bx, by) = p[1], p[2]
			k = max(1, int(math.hypot(bx - ax, by - ay) / step))
			out += [(ax + (bx - ax) * j / k, ay + (by - ay) * j / k) for j in range(k + 1)]
		else:
			_, _, _, r, _, c, a0, a1 = p
			k = max(1, int(abs(a1 - a0) * r / step))
			out += [(c[0] + r * math.cos(a0 + (a1 - a0) * j / k), c[1] + r * math.sin(a0 + (a1 - a0) * j / k))
				for j in range(k + 1)]
	return out

def dist(p, points):
	return min(math.hypot(p[0] - q[0], p[1] - q[1]) for q in points)

def trimmed(ch):
	"""The character's tubes with each gapped end moved back along its tube (round cap kept) until it clears
	every tube drawn after it by GAP. Later tubes are final before earlier ones are trimmed against them."""
	tubes = [list(t) for t in CHARS[ch]]
	for i in range(len(tubes) - 2, -1, -1):
		flags, closed, _ = parse(tubes[i])
		later = [samples(t) for t in tubes[i + 1:]]
		pts = [p for p in tubes[i] if not isinstance(p, str)]
		for flag, end, nxt in (('S', 0, 1), ('E', len(pts) - 1, len(pts) - 2)):
			if flag not in flags:
				continue
			p, q = pts[end], pts[nxt]
			length = math.hypot(q[0] - p[0], q[1] - p[1])
			at = lambda s: (p[0] + (q[0] - p[0]) * s / length, p[1] + (q[1] - p[1]) * s / length)
			s = 0.0
			while min(dist(at(s), t) for t in later) < STROKE + GAP:
				s += 0.05
				if s > length - 1.5:
					sys.exit(f'{ch} tube {i}: end {flag} is buried in a later tube')
			pts[end] = at(s) + tuple(p[2:])
		tubes[i] = ([flags] if flags else []) + pts + (['Z'] if closed else [])
	return tubes

def check(ch, tubes):
	ss = [samples(t, 0.2) for t in tubes]
	ok = True
	for i in range(len(tubes)):
		for j in range(i + 1, len(tubes)):
			d = min(dist(p, ss[j]) for p in ss[i])
			if STROKE - 0.3 <= d < STROKE + GAP - 0.05:
				print(f'{ch}: tubes {i} and {j} are {d - STROKE:+.2f} apart (hairline)')
				ok = False
	return ok

def write_svg(out):
	# Lay the characters out by their lit extents: equal gaps, the whole word centered in 128x64.
	chars = {ch: trimmed(ch) for ch in CHARS}
	boxes = {}
	for ch, tubes in chars.items():
		check(ch, tubes)
		pts = [p for t in tubes for p in samples(t, 0.2)]
		boxes[ch] = (min(p[0] for p in pts) - STROKE / 2, min(p[1] for p in pts) - STROKE / 2,
			max(p[0] for p in pts) + STROKE / 2, max(p[1] for p in pts) + STROKE / 2)
	width = sum(b[2] - b[0] for b in boxes.values()) + LETTER_GAP * (len(boxes) - 1)
	top, bottom = min(b[1] for b in boxes.values()), max(b[3] for b in boxes.values())
	x, y = (128 - width) / 2, (64 - (bottom - top)) / 2 - top
	groups = ''
	for ch, tubes in chars.items():
		groups += f'\n\t\t\t<g transform="translate({x - boxes[ch][0]:.2f} {y:.2f})">' + ''.join(
			f'\n\t\t\t\t<path d="{svg_path(t)}"/>' for t in tubes) + '\n\t\t\t</g>'
		x += boxes[ch][2] - boxes[ch][0] + LETTER_GAP
	out.write_text(f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 64">
	<!-- SDRadio station logo 私家台, generated by neon.py (edit that, not this). -->
	<defs>
		<filter id="glow" x="-10%" y="-20%" width="120%" height="140%">
			<feGaussianBlur stdDeviation="{GLOW}"/>
		</filter>
		<g id="tubes" fill="none" stroke="#000" stroke-width="{STROKE}" stroke-linecap="round" stroke-linejoin="round">{groups}
		</g>
	</defs>
	<use href="#tubes" filter="url(#glow)" opacity="{GLOW_ALPHA}"/>
	<use href="#tubes"/>
</svg>
''', encoding='utf-8')
	print(f'{out.name}: lettering {width:.1f} x {bottom - top:.1f} of 128 x 64')

def debug(weight):
	grid = ''.join(f'<path d="M{v} 0 V40 M0 {v} H40" stroke="{"#79a" if v % 5 == 0 else "#dde"}" '
		f'stroke-width="{0.1 if v % 5 == 0 else 0.05}"/>' for v in range(0, 41))
	labels = ''.join(f'<text x="{v}" y="-0.4" font-size="1.1" text-anchor="middle" fill="#fff">{v}</text>'
		f'<text x="-0.5" y="{v + 0.4}" font-size="1.1" text-anchor="end" fill="#fff">{v}</text>' for v in range(0, 41, 5))
	cells = ''
	for ch in CHARS:
		tubes = ''.join(f'<path d="{svg_path(t)}"/>' for t in trimmed(ch))
		cells += f'''<svg viewBox="-2 -2 43 43" width="860" height="860" font-family="Segoe UI">
<rect x="0" y="0" width="40" height="40" fill="#fff"/>{grid}{labels}
<text x="20" y="35.2" text-anchor="middle" font-family="Noto Sans SC" font-weight="{weight}" font-size="40" fill="#bbb">{ch}</text>
<g fill="none" stroke="rgba(230,40,40,0.28)" stroke-width="{STROKE}" stroke-linecap="round" stroke-linejoin="round">{tubes}</g>
<g fill="none" stroke="#d00" stroke-width="0.15">{tubes}</g></svg>'''
	page = render.page('neon_debug', cells, 'body { background: #666; display: flex }')
	render.shoot(page, render.BUILD / 'neon_debug.png', 860 * len(CHARS), 860)
	print('build\\neon_debug.png')

if __name__ == '__main__':
	sys.stdout.reconfigure(encoding='utf-8')  # pyright: ignore[reportAttributeAccessIssue]
	svg = HERE / 'logo.svg'
	write_svg(svg)
	render.render_svg(svg)
	render.preview(svg)
	print('logo_512.png, logo_128.png, build\\logo_preview.png')
	if len(sys.argv) > 1 and sys.argv[1] == 'debug':
		debug(sys.argv[2] if len(sys.argv) > 2 else 700)
