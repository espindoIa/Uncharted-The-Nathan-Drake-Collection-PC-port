"""Vector redraw of a packed BMFont atlas (one glyph per channel): every glyph's outlines fitted to its
coverage (glyphfit4.fit_glyph_checked), written as SVG and rendered with exact area coverage at 4x into the same
channels. Prints the round-trip error per glyph (4x render box-filtered back to 1x against the atlas),
next to the error of the bicubic + contrast upscale the runtime falls back to.
Usage: python tools/ui_assets/make_font_atlas.py <atlas: .tga/.png, or raw RGBA with WxH in the name> <fnt> <out prefix>"""
import sys, time, json, re
import numpy as np
from PIL import Image
import glyphfit as gf
import glyphfit4 as g4

K = 4


def load(path):
    m = re.search(r'(\d+)x(\d+)\.(raw|rgba)$', path)
    if m:
        w, h = int(m.group(1)), int(m.group(2))
        return np.fromfile(path, np.uint8).reshape(h, w, 4).astype(np.float64)
    return np.asarray(Image.open(path).convert('RGBA'), dtype=np.float64)


def read_fnt(path):
    chars = []
    for l in open(path, encoding='utf-8', errors='replace').read().split('\n'):
        if l.startswith('char '):
            d = dict(kv.split('=', 1) for kv in l.split()[1:] if '=' in kv)
            c = {k: int(v) for k, v in d.items() if v.lstrip('-').isdigit()}
            if c.get('width', 0) > 0 and c.get('height', 0) > 0:
                chars.append(c)
    return chars


def bicubic_contrast(plane, contrast=2.0):
    # the runtime fallback (gow3_atlas_resample.h UpscaleBicubic), for comparison
    h, w = plane.shape

    def mat(n):
        pos = (np.arange(n * K) + 0.5) / K - 0.5
        b = np.floor(pos).astype(int)
        t = pos - b
        wts = [((-0.5 * t + 1) * t - 0.5) * t, (1.5 * t - 2.5) * t * t + 1, ((-1.5 * t + 2) * t + 0.5) * t, (0.5 * t - 0.5) * t * t]
        M = np.zeros((n * K, n))
        for j, wt in enumerate(wts):
            np.add.at(M, (np.arange(n * K), np.clip(b - 1 + j, 0, n - 1)), wt)
        return M
    v = np.clip(mat(h) @ plane @ mat(w).T, 0, 255)
    return np.clip((v - 127.5) * contrast + 127.5, 0, 255)


def main():
    sys.stdout.reconfigure(errors='backslashreplace')  # Cyrillic/CJK glyph names on a cp1252 console
    src, fnt, out = sys.argv[1:4]
    img = load(src)
    H, W = img.shape[:2]
    chars = read_fnt(fnt)
    # BMFont channel bits: 1 blue, 2 green, 4 red, 8 alpha (planes of an RGBA image). Not guessed from the
    # ink: packed cells of different channels overlap, and U1/U2 have all four in use.
    chan = {1: 2, 2: 1, 4: 0, 8: 3}
    print('channel bits -> planes', chan)
    t0 = time.time()
    hi = np.zeros((H * K, W * K, 4))
    rows, svg = [], []
    for p in range(4):
        cells = [c for c in chars if chan.get(c['chnl']) == p]
        if not cells:
            hi[..., p] = np.kron(img[..., p], np.ones((K, K)))  # unused plane: as it is
            continue
        plane = img[..., p]
        bic = bicubic_contrast(plane)
        polys = []
        for c in cells:
            x0, y0, w, h = c['x'], c['y'], c['width'], c['height']
            tgt = plane[y0:y0 + h, x0:x0 + w]
            if tgt.max() < 128:
                continue
            loops = [[C[k] for k in range(len(C))] for C in g4.fit_glyph_checked(tgt, x0, y0)]
            sh = np.array([x0, y0])
            big = gf.coverage([(gf.flatten(s) - sh) * K for s in loops], w * K, h * K) * 255.0
            er = big.reshape(h, K, w, K).mean((1, 3)) - tgt
            eb = bic[y0 * K:(y0 + h) * K, x0 * K:(x0 + w) * K].reshape(h, K, w, K).mean((1, 3)) - tgt
            rows.append(dict(char=chr(c['id']), id=c['id'], plane=p, segs=sum(len(s) for s in loops),
                             mae=float(np.abs(er).mean()), max=float(np.abs(er).max()),
                             bic_mae=float(np.abs(eb).mean()), bic_max=float(np.abs(eb).max())))
            polys += [gf.flatten(s) * K for s in loops]
            svg.append((c['id'], p, ' '.join(gf.svg_d(s) for s in loops)))
        hi[..., p] = gf.coverage(polys, W * K, H * K) * 255.0
        # anything outside the glyph cells (should be nothing) stays as it was
        mask = np.zeros((H, W), bool)
        for c in cells:
            mask[c['y']:c['y'] + c['height'], c['x']:c['x'] + c['width']] = True
        if (plane[~mask] > 0).any():
            print(f'  plane {p}: {(plane[~mask] > 0).sum()} inked texels outside the cells kept as they were')
            hi[..., p] = np.where(np.kron(mask, np.ones((K, K))) > 0, hi[..., p], np.kron(plane, np.ones((K, K))))
        print(f'plane {p}: {len(cells)} glyphs, {time.time() - t0:.0f}s', flush=True)
    Image.fromarray(np.clip(hi + 0.5, 0, 255).astype(np.uint8), 'RGBA').save(out + '_x4.png')
    names = 'RGBA'
    with open(out + '.svg', 'w') as f:
        f.write(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W * K}" height="{H * K}" viewBox="0 0 {W} {H}">\n')
        for p in range(4):
            g = [s for s in svg if s[1] == p]
            if g:
                f.write(f'<g id="plane-{names[p]}" fill="#000">\n')
                for cid, _, d in g:
                    f.write(f'<path id="glyph-{cid}" fill-rule="nonzero" d="{d}"/>\n')
                f.write('</g>\n')
        f.write('</svg>\n')
    json.dump(rows, open(out + '_err.json', 'w'), indent=0)
    m = np.array([r['mae'] for r in rows]); mx = np.array([r['max'] for r in rows])
    b = np.array([r['bic_mae'] for r in rows])
    print('glyphs %d: round-trip MAE mean %.2f median %.2f p95 %.2f worst %.2f | max err median %.0f p95 %.0f worst %.0f'
          % (len(rows), m.mean(), np.median(m), np.percentile(m, 95), m.max(), np.median(mx), np.percentile(mx, 95), mx.max()))
    print('bicubic+contrast fallback: MAE mean %.2f median %.2f p95 %.2f worst %.2f' % (b.mean(), np.median(b), np.percentile(b, 95), b.max()))
    for r in sorted(rows, key=lambda r: -r['mae'])[:10]:
        print('  %r id %d MAE %.2f max %.0f (bicubic %.2f) segs %d' % (r['char'], r['id'], r['mae'], r['max'], r['bic_mae'], r['segs']))
    print('total %.0fs' % (time.time() - t0))


if __name__ == '__main__':
    main()
