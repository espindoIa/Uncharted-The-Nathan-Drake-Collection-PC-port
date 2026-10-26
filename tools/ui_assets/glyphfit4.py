"""Outline fitter v4: smooth G1 Bezier outlines optimised against the coverage.
1. 0.5 iso-lines, corners where two straight sides meet (glyphfit.find_corners), rounded corners replaced
   by the side lines' intersection.
2. Each run between corners Gaussian-smoothed (ends pinned) and fitted with few cubic Beziers
   (Schneider, loose tolerance): the hinted 1080p staircase is not followed.
3. Levenberg-Marquardt on the join parameters against the exact area coverage of the glyph: a smooth join
   is (point, tangent angle, in and out handle lengths), so it stays G1; a corner has its own in and out
   angles. The Jacobian is analytic (control points -> flattened points -> ink moved per pixel)."""
import numpy as np
import glyphfit as gf

PER = 16  # flattened points per segment


def gauss_run(R, sigma_pts, closed):
    if sigma_pts <= 0 or len(R) < 5:
        return R
    r = min(int(3 * sigma_pts), len(R) - 2)
    if r < 1:
        return R
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma_pts) ** 2)
    k /= k.sum()
    if closed:
        P = np.concatenate([R[-r:], R, R[:r]])
        return np.stack([np.convolve(P[:, i], k, 'valid') for i in range(2)], 1)
    # open run, ends pinned: smooth the offset from the chord so the ends do not move
    n = len(R)
    t = np.linspace(0, 1, n)[:, None]
    base = R[0] * (1 - t) + R[-1] * t
    Dv = R - base
    P = np.concatenate([-Dv[r:0:-1], Dv, -Dv[-2:-r - 2:-1]])  # odd reflection keeps 0 at the ends
    S = np.stack([np.convolve(P[:, i], k, 'valid') for i in range(2)], 1)
    S[0] = 0
    S[-1] = 0
    return base + S


def blur(img, sigma):
    r = int(np.ceil(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    p = np.pad(img, r)
    p = np.apply_along_axis(lambda v: np.convolve(v, k, 'valid'), 0, p)
    return np.apply_along_axis(lambda v: np.convolve(v, k, 'valid'), 1, p)


def corner_sites(window, sigma=0.7, arc=1.5, min_turn=45.0, step=0.25):
    """Where the outline still turns sharply after the bitmap is blurred: a real corner keeps most of its
    angle within `arc` px, a hinting step on a curve or a flat (one texel high) almost none."""
    sites = []
    for lp in gf.contours(blur(window.astype(float), sigma), 127.5):
        if len(lp) < 6:
            continue
        D, st = gf.resample_closed(lp, step)
        k = max(1, int(round(0.5 * arc / st)))
        a = D - np.roll(D, k, 0)
        b = np.roll(D, -k, 0) - D
        turn = np.degrees(np.arccos(np.clip(np.sum(a * b, 1) / (np.hypot(*a.T) * np.hypot(*b.T) + 1e-12), -1, 1)))
        peak = (turn >= min_turn) & (turn >= np.roll(turn, 1)) & (turn >= np.roll(turn, -1))
        sites += list(D[peak])
    return np.array(sites).reshape(-1, 2)


def initial(window, x0, y0, sigma_px=0.8, tol=0.18, step=0.25, site_dist=1.3):
    """Loops as lists of joins: dict(P, corner, a_in, a_out, th_in, th_out) (th: tangent angle of travel)."""
    loops = gf.contours(window, 127.5)
    sites = corner_sites(window) + np.array([x0, y0])
    out = []
    for lp in loops:
        if len(lp) < 3:
            continue
        D, st = gf.resample_closed(lp + np.array([x0, y0]), step)
        if len(lp) >= 6:
            D = gf.smooth_closed(D, 3)
            cs = gf.find_corners(D, st)
            if len(sites):
                cs = [c for c in cs if np.min(np.hypot(*(sites - c[1]).T)) <= site_dist]
                # sites with no corner found (short sides: serif tips): corner from shorter side fits
                for q in sites:
                    if any(np.hypot(*(q - c[1])) <= site_dist for c in cs):
                        continue
                    i = int(np.argmin(np.hypot(*(D - q).T)))
                    if np.hypot(*(D[i] - q)) > site_dist:
                        continue
                    short = gf.find_corners(D[np.r_[i - 12:i + 13] % len(D)], st, near=0.6, far=2.2,
                                            min_turn=40.0, straight=0.2, max_shift=1.2)
                    if short:
                        j, X, db, df = min(short, key=lambda c: abs(c[0] - 12))
                        cs.append(((i + j - 12) % len(D), X, db, df))
                cs.sort(key=lambda c: c[0])
                dedup = []
                for c in cs:
                    if all(min(abs(c[0] - d[0]), len(D) - abs(c[0] - d[0])) * st >= 1.5 for d in dedup):
                        dedup.append(c)
                cs = dedup
            else:
                cs = []
        else:
            cs = []
        P, cidx = gf.sharpen(D, cs, 1.0, st)
        n = len(P)
        sig = sigma_px / st
        segs, corner_flags = [], []
        if not cidx:
            P = gauss_run(P, sig, True)
            cuts = [int(round(q * n / 4)) for q in range(4)]
            tans = [gf.local_tangent(P, c, 3) for c in cuts]
            for q in range(4):
                a, b = cuts[q], (cuts[q + 1] if q < 3 else cuts[0] + n)
                run = P[[k % n for k in range(a, b + 1)]]
                s = gf.fit_cubic(run, tans[q], -tans[(q + 1) % 4], tol)
                segs += s
                corner_flags += [False] * len(s)
        else:
            order = list(cidx) + [cidx[0] + n]
            for r in range(len(cidx)):
                run = P[[k % n for k in range(order[r], order[r + 1] + 1)]]
                if len(run) < 3:
                    run = np.vstack([run[0], 0.5 * (run[0] + run[-1]), run[-1]])
                run = gauss_run(run, sig, False)
                t1 = run[min(4, len(run) - 1)] - run[0]
                t2 = run[max(len(run) - 5, 0)] - run[-1]
                t1 /= np.hypot(*t1) + 1e-12
                t2 /= np.hypot(*t2) + 1e-12
                s = gf.fit_cubic(run, t1, t2, tol)
                segs += s
                corner_flags += [True] + [False] * (len(s) - 1)
        out.append(to_joins(segs, corner_flags))
    return out


def to_joins(segs, corner_flags):
    joins = []
    m = len(segs)
    for k in range(m):
        prev = segs[k - 1]
        cur = segs[k]
        P = cur[0]
        vin = P - prev[2]   # incoming tangent (travel direction)
        vout = cur[1] - P
        a_in, a_out = np.hypot(*vin), np.hypot(*vout)
        th_in = np.arctan2(vin[1], vin[0]) if a_in > 1e-9 else np.arctan2(*(P - prev[0])[::-1])
        th_out = np.arctan2(vout[1], vout[0]) if a_out > 1e-9 else np.arctan2(*(cur[3] - P)[::-1])
        if not corner_flags[k]:
            # G1: one angle (the mean direction)
            th = np.arctan2(np.sin(th_in) * a_in + np.sin(th_out) * a_out, np.cos(th_in) * a_in + np.cos(th_out) * a_out)
            th_in = th_out = th
        joins.append(dict(P=P.copy(), corner=bool(corner_flags[k]), a_in=max(a_in, 0.05), a_out=max(a_out, 0.05),
                          th_in=th_in, th_out=th_out))
    return joins


# parameter vector per join: smooth [x, y, th, a_in, a_out]; corner [x, y, th_in, th_out, a_in, a_out]
def pack(loops):
    v = []
    for L in loops:
        for j in L:
            v += [j['P'][0], j['P'][1]]
            v += [j['th_in'], j['th_out']] if j['corner'] else [j['th_in']]
            v += [j['a_in'], j['a_out']]
    return np.array(v)


def layout(loops):
    """Per join: (offset of its parameters, corner?)."""
    lay, o = [], 0
    for L in loops:
        ll = []
        for j in L:
            ll.append((o, j['corner']))
            o += 6 if j['corner'] else 5
        lay.append(ll)
    return lay, o


def controls(v, lay):
    """Control points (m, 4, 2) per loop and dC/dv as a sparse list of (seg, ctrl, axis, param, value)."""
    out = []
    for ll in lay:
        m = len(ll)
        C = np.zeros((m, 4, 2))
        dC = []
        for k in range(m):
            o, corner = ll[k]
            on, cn = ll[(k + 1) % m]
            x, y = v[o], v[o + 1]
            if corner:
                th_out, a_out = v[o + 3], v[o + 5]
                th_out_i = o + 3
                a_out_i = o + 5
            else:
                th_out, a_out = v[o + 2], v[o + 4]
                th_out_i, a_out_i = o + 2, o + 4
            xn, yn = v[on], v[on + 1]
            if cn:
                th_in, a_in = v[on + 2], v[on + 4]
                th_in_i, a_in_i = on + 2, on + 4
            else:
                th_in, a_in = v[on + 2], v[on + 3]
                th_in_i, a_in_i = on + 2, on + 3
            co, so = np.cos(th_out), np.sin(th_out)
            ci, si = np.cos(th_in), np.sin(th_in)
            C[k, 0] = (x, y)
            C[k, 1] = (x + a_out * co, y + a_out * so)
            C[k, 2] = (xn - a_in * ci, yn - a_in * si)
            C[k, 3] = (xn, yn)
            dC += [(k, 0, 0, o, 1.0), (k, 0, 1, o + 1, 1.0),
                   (k, 1, 0, o, 1.0), (k, 1, 1, o + 1, 1.0),
                   (k, 1, 0, th_out_i, -a_out * so), (k, 1, 1, th_out_i, a_out * co),
                   (k, 1, 0, a_out_i, co), (k, 1, 1, a_out_i, so),
                   (k, 2, 0, on, 1.0), (k, 2, 1, on + 1, 1.0),
                   (k, 2, 0, th_in_i, a_in * si), (k, 2, 1, th_in_i, -a_in * ci),
                   (k, 2, 0, a_in_i, -ci), (k, 2, 1, a_in_i, -si),
                   (k, 3, 0, on, 1.0), (k, 3, 1, on + 1, 1.0)]
        out.append((C, dC))
    return out


_t = np.arange(PER) / PER
BERN = np.stack([(1 - _t) ** 3, 3 * (1 - _t) ** 2 * _t, 3 * (1 - _t) * _t ** 2, _t ** 3], 1)  # PER x 4


def flat(C):
    return np.einsum('pc,mcd->mpd', BERN, C).reshape(-1, 2)


def fit(window, x0=0, y0=0, iters=25, prior=0.02, S=16, init=None):
    h, w = window.shape
    T = window.ravel() / 255.0
    sh = np.array([x0, y0])
    loops = init if init is not None else initial(window, x0, y0)
    if not loops:
        return []
    lay, nv = layout(loops)
    v = pack(loops)
    v0 = v.copy()
    # prior: stay near the smooth start (angles and handle lengths weigh less than positions)
    pw = np.zeros(nv)
    for ll in lay:
        for o, corner in ll:
            pw[o:o + 2] = 1.0
            pw[o + 2:o + (4 if corner else 3)] = 0.5
            pw[o + (4 if corner else 3):o + (6 if corner else 5)] = 0.3
    pw *= prior

    def cov(v):
        cs = controls(v, lay)
        return gf.coverage([flat(C) - sh for C, _ in cs], w, h, S).ravel(), cs

    c, cs = cov(v)
    r = c - T
    sign = None
    cost = r @ r + np.sum((pw * (v - v0)) ** 2)
    lam = 1e-2
    for it in range(iters):
        J = np.zeros((h * w, nv))
        for (C, dC) in cs:
            F = flat(C)
            m = len(C)
            seg = np.roll(F, -1, 0) - F
            L = np.hypot(*seg.T)
            nrm = np.stack([seg[:, 1], -seg[:, 0]], 1) / (L[:, None] + 1e-12)
            # point normal and the length it stands for
            Np = nrm + np.roll(nrm, 1, 0)
            Np /= np.hypot(*Np.T)[:, None] + 1e-12
            Lp = 0.5 * (L + np.roll(L, 1))
            if sign is None:
                # outward normal adds ink?
                test = gf.coverage([F + 0.05 * Np - sh], w, h, S).sum() - gf.coverage([F - sh], w, h, S).sum()
                sign = 1.0 if test >= 0 else -1.0
            pix = []
            for side in (0.02, -0.02):
                q = F + side * Np - sh
                px = np.clip(np.floor(q[:, 0]).astype(int), 0, w - 1)
                py = np.clip(np.floor(q[:, 1]).astype(int), 0, h - 1)
                pix.append(py * w + px)
            # d ink(pixel) / d point = 0.5 * Lp * Np (per side); point = sum_c BERN[p, c] * C[seg, c]
            for (k, cc, ax, pi, val) in dC:
                rows = np.arange(k * PER, (k + 1) * PER) % len(F)
                wgt = BERN[:, cc] * val * Lp[rows] * Np[rows, ax] * sign * 0.5
                # the end point of segment k is also the start of segment k+1 (BERN covers t<1 only)
                for pp in pix:
                    np.add.at(J[:, pi], pp[rows], wgt)
        Jp = np.diag(pw)
        H = J.T @ J + Jp.T @ Jp
        g = J.T @ r + Jp.T @ (pw * (v - v0))
        improved = False
        for _ in range(8):
            step = np.linalg.solve(H + lam * np.diag(np.diag(H) + 1e-9), -g)
            step = np.clip(step, -0.3, 0.3)
            vn = v + step
            # handle lengths stay positive and within 0.6 of their segment's chord (no loops or spikes)
            for ll in lay:
                m = len(ll)
                for k, (o, corner) in enumerate(ll):
                    a0 = o + (4 if corner else 3)
                    po, pn = ll[k - 1][0], ll[(k + 1) % m][0]
                    ch_in = np.hypot(vn[o] - vn[po], vn[o + 1] - vn[po + 1])
                    ch_out = np.hypot(vn[pn] - vn[o], vn[pn + 1] - vn[o + 1])
                    vn[a0] = np.clip(vn[a0], 0.02, max(0.6 * ch_in, 0.02))
                    vn[a0 + 1] = np.clip(vn[a0 + 1], 0.02, max(0.6 * ch_out, 0.02))
            cn_, csn = cov(vn)
            rn = cn_ - T
            costn = rn @ rn + np.sum((pw * (vn - v0)) ** 2)
            if costn < cost:
                v, r, cs, cost, lam, improved = vn, rn, csn, costn, max(lam / 3, 1e-6), True
                break
            lam *= 4
        if not improved:
            break
    return [C for C, _ in cs]


def roundtrip_mae(window, x0, y0, Cs, K=4):
    h, w = window.shape
    big = gf.coverage([(flat(C) - np.array([x0, y0])) * K for C in Cs], w * K, h * K) * 255.0
    return float(np.abs(big.reshape(h, K, w, K).mean((1, 3)) - window).mean())


def fit_glyph_checked(window, x0=0, y0=0, retry_above=2.0):
    """fit_glyph; a glyph whose round trip stays above `retry_above` (a long first Bezier that the
    optimisation could not bend into a notch: 25 Oct, U3 'g') is refitted with a tighter start (tol 0.06)
    and the better result kept."""
    Cs = fit_glyph(window, x0, y0)
    e = roundtrip_mae(window, x0, y0, Cs)
    if e > retry_above:
        Cs2 = fit_glyph(window, x0, y0, tol=0.06)
        if roundtrip_mae(window, x0, y0, Cs2) < e:
            return Cs2
    return Cs


def loop_area(C):
    F = flat(C)
    return gf.area(F)


def turn_at(C, k):
    vin = C[k][0] - C[k - 1][2]
    vout = C[k][1] - C[k][0]
    return np.degrees(abs(np.angle(np.exp(1j * (np.arctan2(vout[1], vout[0]) - np.arctan2(vin[1], vin[0]))))))


def fit_glyph(window, x0=0, y0=0, sigma_px=0.4, tol=0.08, prior=0.01, min_turn_kept=28.0, min_area=0.8):
    """v4 outlines of one glyph. After a first fit: loops smaller than min_area px^2 (one-texel specks) are
    dropped, corners that came out nearly straight (< min_turn_kept degrees) become smooth joins, and every
    other corner stays. The 1080p atlases are hinted, so a staircase step reads as two short straight
    sides; corner_sites() in initial() already keeps only the corners the blurred bitmap still turns at."""
    init = initial(window, x0, y0, sigma_px=sigma_px, tol=tol)
    Cs = fit(window, x0, y0, init=init, prior=prior)
    if not Cs:
        return []
    kept, flags = [], []
    for C, L in zip(Cs, init):
        if abs(loop_area(C)) < min_area:
            continue
        kept.append(C)
        flags.append([L[k]['corner'] and turn_at(C, k) >= min_turn_kept for k in range(len(C))])
    if not kept:
        return []

    def refit(Cs, flags, iters):
        loops = [to_joins([C[k] for k in range(len(C))], f) for C, f in zip(Cs, flags)]
        return fit(window, x0, y0, init=loops, prior=prior, iters=iters)

    return refit(kept, flags, 15)
