# The mean of ln r between two axis-aligned rectangles, in closed form:
# F(X, Y) with d4F/dX2dY2 = ln r, summed over the 16 corner differences;
# far pairs (ten sizes apart) by their centres plus the second-order term.
# python3 -B meanlog.py checks it against Gauss-Legendre quadrature and a
# rectangle's own against Gray's formula.
import numpy as np

def F(X, Y):
    X = np.asarray(X, float); Y = np.asarray(Y, float)
    r2 = X * X + Y * Y
    with np.errstate(divide='ignore', invalid='ignore'):
        lnr = np.where(r2 > 0, 0.5 * np.log(np.where(r2 > 0, r2, 1.0)), 0.0)
        ay = np.where(X != 0, np.arctan(Y / np.where(X != 0, X, 1.0)), 0.0)   # atan(Y/X), its X^3 Y factor nought at X = 0
        ax = np.where(Y != 0, np.arctan(X / np.where(Y != 0, Y, 1.0)), 0.0)
    X2, Y2 = X * X, Y * Y
    return (-(X2 * X2 - 6 * X2 * Y2 + Y2 * Y2) / 24 * lnr + (X2 * X * Y * ay + X * Y2 * Y * ax) / 6 - 25.0 / 48 * X2 * Y2)

def meanLogClosed(r1, r2):
    """r1, r2: arrays (..., 4) of x0, x1, y0, y1. The mean of ln r over both."""
    r1 = np.asarray(r1, float); r2 = np.asarray(r2, float)
    xs1 = (r1[..., 0], r1[..., 1]); xs2 = (r2[..., 0], r2[..., 1])
    ys1 = (r1[..., 2], r1[..., 3]); ys2 = (r2[..., 2], r2[..., 3])
    # sigma_ij = (-1)^(i+j+1): (a2-c1) +, (a1-c1) -, (a2-c2) -, (a1-c2) +
    sx = {(1, 0): 1, (0, 0): -1, (1, 1): -1, (0, 1): 1}
    tot = 0.0
    for (i, j), s1 in sx.items():
        X = xs1[i] - xs2[j]
        for (k, l), s2 in sx.items():
            Y = ys1[k] - ys2[l]
            tot = tot + s1 * s2 * F(X, Y)
    A1 = (r1[..., 1] - r1[..., 0]) * (r1[..., 3] - r1[..., 2])
    A2 = (r2[..., 1] - r2[..., 0]) * (r2[..., 3] - r2[..., 2])
    return tot / (A1 * A2)

def meanLogFar(r1, r2):
    r1 = np.asarray(r1, float); r2 = np.asarray(r2, float)
    X = 0.5 * (r1[..., 0] + r1[..., 1] - r2[..., 0] - r2[..., 1])
    Y = 0.5 * (r1[..., 2] + r1[..., 3] - r2[..., 2] - r2[..., 3])
    w1 = r1[..., 1] - r1[..., 0]; h1 = r1[..., 3] - r1[..., 2]
    w2 = r2[..., 1] - r2[..., 0]; h2 = r2[..., 3] - r2[..., 2]
    d2 = X * X + Y * Y
    return 0.5 * np.log(d2) - (w1 * w1 - h1 * h1 + w2 * w2 - h2 * h2) / 24 * (X * X - Y * Y) / (d2 * d2)

def meanLog(r1, r2, near=10.0):
    r1 = np.asarray(r1, float); r2 = np.asarray(r2, float)
    X = 0.5 * (r1[..., 0] + r1[..., 1] - r2[..., 0] - r2[..., 1])
    Y = 0.5 * (r1[..., 2] + r1[..., 3] - r2[..., 2] - r2[..., 3])
    size = np.maximum(np.maximum(r1[..., 1] - r1[..., 0], r1[..., 3] - r1[..., 2]),
                      np.maximum(r2[..., 1] - r2[..., 0], r2[..., 3] - r2[..., 2]))
    close = np.hypot(X, Y) < near * size
    out = np.empty(np.broadcast(X, Y).shape)
    if np.any(close):
        out[close] = meanLogClosed(r1[close] if r1.ndim > 1 else r1, r2[close] if r2.ndim > 1 else r2)
    if np.any(~close):
        out[~close] = meanLogFar(r1[~close] if r1.ndim > 1 else r1, r2[~close] if r2.ndim > 1 else r2)
    return out

def grayLogGmd(a, b):
    import math
    return (math.log(math.hypot(a, b)) - (a * a / (12 * b * b)) * math.log(1 + b * b / (a * a))
            - (b * b / (12 * a * a)) * math.log(1 + a * a / (b * b)) + (2 * a / (3 * b)) * math.atan(b / a)
            + (2 * b / (3 * a)) * math.atan(a / b) - 25.0 / 12)

if __name__ == "__main__":
    # brute force: Gauss-Legendre over sub-boxes (the singular self and touching pairs need many)
    def brute(r1, r2, n=24, sub=6):
        gx, gw = np.polynomial.legendre.leggauss(n)
        def pts(a, b):
            e = np.linspace(a, b, sub + 1); x = []; w = []
            for k in range(sub):
                m, h = 0.5 * (e[k] + e[k + 1]), 0.5 * (e[k + 1] - e[k]); x.append(m + h * gx); w.append(h * gw)
            return np.concatenate(x), np.concatenate(w)
        x1, wx1 = pts(r1[0], r1[1]); y1, wy1 = pts(r1[2], r1[3]); x2, wx2 = pts(r2[0], r2[1]); y2, wy2 = pts(r2[2], r2[3])
        tot = 0.0
        for a, wa in zip(x1, wx1):
            X = a - x2[:, None, None]; 
            for b, wb in zip(y1, wy1):
                Y = b - y2[None, :, None]
                # integrate over x2 (axis 0), y2 (axis 1)
                r2_ = X[:, :, 0] ** 2 + Y[:, :, 0] ** 2
                v = 0.5 * np.log(np.maximum(r2_, 1e-300))
                tot += wa * wb * np.sum(wx2[:, None] * wy2[None, :] * v)
        A1 = (r1[1] - r1[0]) * (r1[3] - r1[2]); A2 = (r2[1] - r2[0]) * (r2[3] - r2[2])
        return tot / (A1 * A2)
    cases = [((0, 1, 0, 1), (3, 4, 0.5, 2.5)), ((0, 1, 0, 0.2), (1, 3, 0, 0.2)), ((0, 2, 0, 1), (0, 2, 1, 2)),
             ((0, 1, 0, 1), (0.5, 1.5, 0.5, 1.5)), ((0, 3, 0, 0.1), (5, 6, -2, 4)), ((-1, 1, -1, 1), (20, 21, 30, 32))]
    for r1, r2 in cases:
        print(r1, r2, "closed %.10f far %.10f brute %.10f" % (meanLogClosed(r1, r2), meanLogFar(r1, r2), brute(r1, r2)))
    for a, b in ((1, 1), (1, 0.1), (5, 0.3), (0.02, 6.5)):
        print("self %g x %g: closed %.10f  Gray %.10f  brute %.10f" % (a, b, meanLogClosed((0, a, 0, b), (0, a, 0, b)), grayLogGmd(a, b), brute((0, a, 0, b), (0, a, 0, b), 24, 8)))
