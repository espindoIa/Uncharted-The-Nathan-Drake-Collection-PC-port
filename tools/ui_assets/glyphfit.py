"""Helpers for glyphfit4: exact-area coverage rasterizer, 0.5 iso-lines, corner finding and Schneider's cubic
Bezier fitting (least squares, Newton reparameterisation, split at the worst point). numpy only."""
import numpy as np


# ---- exact-area rasterizer: exact horizontally, S sub-scanlines vertically ----------------------
def coverage(polys, w, h, S=16):
    if not polys:
        return np.zeros((h, w))
    P0 = np.concatenate(polys)
    P1 = np.concatenate([np.roll(p, -1, axis=0) for p in polys])
    x0, y0, x1, y1 = P0[:, 0], P0[:, 1], P1[:, 0], P1[:, 1]
    sy = (np.arange(h * S) + 0.5) / S
    lo = np.searchsorted(sy, np.minimum(y0, y1), 'left')
    hi = np.searchsorted(sy, np.maximum(y0, y1), 'left')
    cnt = hi - lo
    ev = np.nonzero(cnt > 0)[0]
    cv = cnt[ev]
    total = int(cv.sum())
    if not total:
        return np.zeros((h, w))
    e = np.repeat(ev, cv)
    starts = np.cumsum(cv) - cv
    sl = lo[e] + np.arange(total) - np.repeat(starts, cv)
    y = sy[sl]
    t = (y - y0[e]) / (y1[e] - y0[e])
    x = x0[e] + t * (x1[e] - x0[e])
    d = np.where(y1[e] > y0[e], 1.0, -1.0)
    xi = np.floor(x)
    frac = x - xi
    xi = np.clip(xi, 0, w).astype(np.int64)
    frac = np.where(x < 0, 0.0, frac)
    frac = np.where(x >= w, 0.0, frac)
    row = sl // S
    idx = row * (w + 2) + xi
    n = h * (w + 2)
    flat = np.bincount(idx, weights=d * (1 - frac), minlength=n)[:n] + \
        np.bincount(idx + 1, weights=d * frac, minlength=n + 1)[:n]
    acc = np.cumsum(flat.reshape(h, w + 2), axis=1)[:, :w] / S
    return np.clip(np.abs(acc), 0.0, 1.0)


# ---- marching squares: closed 0.5 iso-lines, outlines counter-clockwise-positive, holes negative ---
def contours(img, level):
    h, w = img.shape
    p = np.pad(img.astype(float), 1, constant_values=0.0)
    segs, pts = [], {}
    above = p >= level
    for i in range(p.shape[0] - 1):
        row_any = above[i].any() or above[i + 1].any()
        if not row_any:
            continue
        for j in range(p.shape[1] - 1):
            a = (above[i, j], above[i, j + 1], above[i + 1, j + 1], above[i + 1, j])
            if all(a) or not any(a):
                continue
            v = (p[i, j], p[i, j + 1], p[i + 1, j + 1], p[i + 1, j])
            c = [(j, i), (j + 1, i), (j + 1, i + 1), (j, i + 1)]
            keys = [('h', i, j), ('v', i, j + 1), ('h', i + 1, j), ('v', i, j)]
            ends = [(0, 1), (1, 2), (3, 2), (0, 3)]
            on = []
            for k, (s, e) in enumerate(ends):
                if a[s] != a[e]:
                    if keys[k] not in pts:
                        t = (level - v[s]) / (v[e] - v[s])
                        pts[keys[k]] = (c[s][0] + t * (c[e][0] - c[s][0]), c[s][1] + t * (c[e][1] - c[s][1]))
                    on.append(k)
            if len(on) == 2:
                segs.append((keys[on[0]], keys[on[1]]))
            else:  # saddle: split by the centre value
                centre = sum(v) / 4 >= level
                for s, e in ([(0, 3), (1, 2)] if centre else [(0, 1), (2, 3)]):
                    segs.append((keys[s], keys[e]))
    adj = {}
    for s, e in segs:
        adj.setdefault(s, []).append(e)
        adj.setdefault(e, []).append(s)
    seen, loops = set(), []
    for s, e in segs:
        if (s, e) in seen or (e, s) in seen:
            continue
        loop, cur, prev = [s], e, s
        seen.add((s, e))
        while cur != s:
            loop.append(cur)
            nb = [n for n in adj[cur] if n != prev]
            nxt = nb[0] if nb else s
            seen.add((cur, nxt))
            prev, cur = cur, nxt
        loops.append(np.array([(pts[k][0] - 0.5, pts[k][1] - 0.5) for k in loop]))
    out = []
    for k, lp in enumerate(loops):
        depth = sum(inside(o, lp[0]) for m, o in enumerate(loops) if m != k)
        if (depth % 2 == 0) != (area(lp) > 0):
            lp = lp[::-1]
        out.append(lp)
    return out


def area(l):
    return 0.5 * np.sum(l[:, 0] * np.roll(l[:, 1], -1) - np.roll(l[:, 0], -1) * l[:, 1])


def inside(poly, q):
    x, y = poly[:, 0], poly[:, 1]
    x2, y2 = np.roll(x, -1), np.roll(y, -1)
    cross = ((y > q[1]) != (y2 > q[1])) & (q[0] < (x2 - x) * (q[1] - y) / (y2 - y + 1e-12) + x)
    return bool(cross.sum() % 2)


def resample_closed(loop, step):
    seg = np.hypot(*(np.roll(loop, -1, 0) - loop).T)
    s = np.concatenate([[0], np.cumsum(seg)])
    n = max(8, int(round(s[-1] / step)))
    u = np.linspace(0, s[-1], n, endpoint=False)
    closed = np.vstack([loop, loop[:1]])
    return np.stack([np.interp(u, s, closed[:, 0]), np.interp(u, s, closed[:, 1])], 1), s[-1] / n


# ---- corners ------------------------------------------------------------------------------------
def line_fit(P):
    c = P.mean(0)
    _, sv, vt = np.linalg.svd(P - c)
    d = vt[0]
    resid = np.abs((P - c) @ vt[1])
    return c, d, resid.max()


def find_corners(D, step, near=1.3, far=3.6, min_turn=50.0, straight=0.16, max_shift=1.6):
    """D: densely resampled closed loop. A corner: the sides from `near` to `far` px before and after the
    point are straight (max deviation < `straight` px) and turn by more than `min_turn` degrees. Returns
    [(index, corner point, tangent in, tangent out)] with the corner at the side lines' intersection."""
    n = len(D)
    k0, k1 = int(round(near / step)), int(round(far / step))
    if n < 2 * k1 + 4:
        return []
    cand = []
    for i in range(n):
        back = D[[(i - k) % n for k in range(k1, k0 - 1, -1)]]
        fwd = D[[(i + k) % n for k in range(k0, k1 + 1)]]
        cb, db, rb = line_fit(back)
        cf, df, rf = line_fit(fwd)
        if rb > straight or rf > straight:
            continue
        if np.dot(db, back[-1] - back[0]) < 0:
            db = -db
        if np.dot(df, fwd[-1] - fwd[0]) < 0:
            df = -df
        turn = np.degrees(np.arccos(np.clip(np.dot(db, df), -1, 1)))
        if turn < min_turn:
            continue
        # intersection of the two side lines
        M = np.array([db, -df]).T
        if abs(np.linalg.det(M)) < 1e-6:
            continue
        t = np.linalg.solve(M, cf - cb)
        X = cb + t[0] * db
        if np.hypot(*(X - D[i])) > max_shift:
            continue
        cand.append((turn, i, X, db, df))
    # one corner per cluster: the largest turn, others within `far` dropped
    keep = []
    for c in sorted(cand, key=lambda c: -c[0]):
        if all(min(abs(c[1] - k[1]), n - abs(c[1] - k[1])) * step > far for k in keep):
            keep.append(c)
    return [(i, X, db, df) for _, i, X, db, df in sorted(keep, key=lambda c: c[1])]


# ---- Schneider FitCurves --------------------------------------------------------------------------
def bez(P, t):
    t = t[:, None]
    return (1 - t) ** 3 * P[0] + 3 * (1 - t) ** 2 * t * P[1] + 3 * (1 - t) * t ** 2 * P[2] + t ** 3 * P[3]


def bez_d1(P, t):
    t = t[:, None]
    return 3 * (1 - t) ** 2 * (P[1] - P[0]) + 6 * (1 - t) * t * (P[2] - P[1]) + 3 * t ** 2 * (P[3] - P[2])


def bez_d2(P, t):
    t = t[:, None]
    return 6 * (1 - t) * (P[2] - 2 * P[1] + P[0]) + 6 * t * (P[3] - 2 * P[2] + P[1])


def generate(d, u, t1, t2):
    p0, p3 = d[0], d[-1]
    b0, b1, b2, b3 = (1 - u) ** 3, 3 * (1 - u) ** 2 * u, 3 * (1 - u) * u ** 2, u ** 3
    A1, A2 = b1[:, None] * t1, b2[:, None] * t2
    C = np.array([[np.sum(A1 * A1), np.sum(A1 * A2)], [np.sum(A1 * A2), np.sum(A2 * A2)]])
    tmp = d - (b0 + b1)[:, None] * p0 - (b2 + b3)[:, None] * p3
    X = np.array([np.sum(A1 * tmp), np.sum(A2 * tmp)])
    det = C[0, 0] * C[1, 1] - C[0, 1] * C[1, 0]
    seg = np.hypot(*(p3 - p0))
    if abs(det) > 1e-12:
        a1 = (X[0] * C[1, 1] - X[1] * C[0, 1]) / det
        a2 = (C[0, 0] * X[1] - C[1, 0] * X[0]) / det
    else:
        a1 = a2 = seg / 3
    if a1 < 1e-3 * seg or a2 < 1e-3 * seg or a1 > 2 * seg or a2 > 2 * seg:
        a1 = a2 = seg / 3
    return np.array([p0, p0 + a1 * t1, p3 + a2 * t2, p3])


def chord_param(d):
    s = np.concatenate([[0], np.cumsum(np.hypot(*np.diff(d, axis=0).T))])
    return s / s[-1]


def reparam(d, u, P):
    q, q1, q2 = bez(P, u) - d, bez_d1(P, u), bez_d2(P, u)
    num = np.sum(q * q1, 1)
    den = np.sum(q1 * q1, 1) + np.sum(q * q2, 1)
    un = u - np.where(np.abs(den) > 1e-9, num / den, 0)
    un[0], un[-1] = 0, 1
    return np.clip(np.maximum.accumulate(un), 0, 1)


def fit_cubic(d, t1, t2, tol, depth=0):
    if len(d) <= 2:
        seg = np.hypot(*(d[-1] - d[0])) / 3
        return [np.array([d[0], d[0] + seg * t1, d[-1] + seg * t2, d[-1]])]
    u = chord_param(d)
    P = generate(d, u, t1, t2)
    err = np.hypot(*(bez(P, u) - d).T)
    if err.max() < tol:
        return [P]
    if err.max() < 4 * tol:
        for _ in range(20):
            u = reparam(d, u, P)
            P = generate(d, u, t1, t2)
            err = np.hypot(*(bez(P, u) - d).T)
            if err.max() < tol:
                return [P]
    if depth > 12 or len(d) < 6:
        return [P]
    k = int(np.argmax(err[1:-1])) + 1
    k = min(max(k, 2), len(d) - 3)
    tc = d[k - 1] - d[k + 1]
    tc = tc / (np.hypot(*tc) + 1e-12)
    return fit_cubic(d[:k + 1], t1, tc, tol, depth + 1) + fit_cubic(d[k:], -tc, t2, tol, depth + 1)


def local_tangent(D, i, span):
    n = len(D)
    idx = [(i + k) % n for k in range(-span, span + 1)]
    c, d, _ = line_fit(D[idx])
    fwd = D[(i + span) % n] - D[(i - span) % n]
    return d if np.dot(d, fwd) >= 0 else -d


def smooth_closed(D, iters):
    for _ in range(iters):
        for f in (0.5, -0.53):
            D = D + f * (0.5 * (np.roll(D, 1, 0) + np.roll(D, -1, 0)) - D)
    return D




def sharpen(D, corners, trim_px, st):
    """Dense loop with the rounded points around each corner replaced by the corner point.
    Returns (points, index of each corner in the new array)."""
    n = len(D)
    if not corners:
        return D.copy(), []
    trim = int(round(trim_px / st))
    pts, cidx = [], []
    for ci, (i, X, _, _) in enumerate(corners):
        j = corners[(ci + 1) % len(corners)][0]
        span = (j - i) % n or n
        cidx.append(len(pts))
        pts.append(X)
        pts += [D[(i + k) % n] for k in range(trim, span - trim + 1)]
    return np.array(pts), cidx



def flatten(segs, per=12):
    t = np.arange(per) / per
    return np.concatenate([bez(P, t) for P in segs])



def svg_d(segs):
    s = f'M{segs[0][0, 0]:.3f} {segs[0][0, 1]:.3f}'
    for P in segs:
        s += f'C{P[1, 0]:.3f} {P[1, 1]:.3f} {P[2, 0]:.3f} {P[2, 1]:.3f} {P[3, 0]:.3f} {P[3, 1]:.3f}'
    return s + 'Z'
