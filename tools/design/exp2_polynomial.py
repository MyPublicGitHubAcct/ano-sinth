#!/usr/bin/env python3
"""Designs the polynomial behind anoesis::dsp::fastExp2 and fastTanh.

    python3 tools/design/exp2_polynomial.py

We need q(f) ~= 2^f - 1 on f in [0, 1]. fastExp2 returns 2^i * (1 + q(f)) and
fastTanh needs e^(2x) - 1 = (2^i - 1) + 2^i * q(f), which keeps full relative
precision near x = 0 because q(f) itself is small there.

Constraints, both built into the basis so they hold exactly:
  q(0) = 0  -> 2^n is exact at integers;
  q(1) = 1  -> continuous across integer boundaries (2^i * 2 == 2^(i+1)).
Write q(f) = f * g(f) with g(f) = 1 + (f - 1) * h(f), h a cubic. Then q has
degree 5 and the Remez exchange below minimises the maximum *relative* error
of q, i.e. of g against g*(f) = (2^f - 1) / f. A relative bound on q also
bounds 2^f: |dq| / 2^f <= |dq| / (2^f - 1) * (2^f - 1) / 2^f < rel(q) / 2.

Pure Python (no numpy), double precision, so it runs anywhere.
Prints C++ float literals in Horner order (c1 .. c5, q = f*(c1 + f*(c2 + ...))).
"""
import math

LN2 = math.log(2.0)
DEG_H = 3                 # h is a cubic -> 4 unknowns
N_UNK = DEG_H + 1


def g_star(f):
    return LN2 if f == 0.0 else math.expm1(f * LN2) / f


def solve(a, b):
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                k = m[r][c] / m[c][c]
                for j in range(c, n + 1):
                    m[r][j] -= k * m[c][j]
    return [m[i][n] / m[i][i] for i in range(n)]


def g_of(h, f):
    hv = sum(c * f ** k for k, c in enumerate(h))
    return 1.0 + (f - 1.0) * hv


def rel_err(h, f):
    return (g_of(h, f) - g_star(f)) / g_star(f)


def remez():
    # Initial reference: Chebyshev-like points in [0, 1).
    pts = [0.5 - 0.5 * math.cos(math.pi * j / N_UNK) for j in range(N_UNK + 1)]
    pts[-1] = 0.97
    grid = [i / 20000.0 for i in range(20000)]  # excludes f = 1, where error is 0
    for _ in range(30):
        # (x-1) h(x) - E (-1)^j g*(x) = g*(x) - 1
        a = [[(x - 1.0) * x ** k for k in range(N_UNK)] + [-((-1) ** j) * g_star(x)] for j, x in enumerate(pts)]
        b = [g_star(x) - 1.0 for x in pts]
        sol = solve(a, b)
        h = sol[:N_UNK]
        # New reference: the extremum of each run of equal error sign.
        errs = [rel_err(h, x) for x in grid]
        runs, cur = [], [0]
        for i in range(1, len(grid)):
            if (errs[i] >= 0) == (errs[cur[0]] >= 0):
                cur.append(i)
            else:
                runs.append(cur)
                cur = [i]
        runs.append(cur)
        ext = [max(r, key=lambda i: abs(errs[i])) for r in runs]
        while len(ext) > N_UNK + 1:  # drop the smallest end extremum
            ext.pop(0 if abs(errs[ext[0]]) < abs(errs[ext[-1]]) else -1)
        pts = [grid[i] for i in ext]
    return h, max(abs(e) for e in errs)


def main():
    h, err = remez()
    # Expand q(f) = f * (1 + (f - 1) * h(f)) into c1..c5.
    # g(f) = 1 - h0 + (h0 - h1) f + (h1 - h2) f^2 + (h2 - h3) f^3 + h3 f^4
    g = [1.0 - h[0], h[0] - h[1], h[1] - h[2], h[2] - h[3], h[3]]
    print(f"max relative error of q (double-precision coefficients): {err:.3e}")
    print(f"sum of coefficients (q(1), must be 1): {sum(g):.17g}")
    for i, c in enumerate(g, start=1):
        print(f"c{i} = {c:.9e}f")


if __name__ == "__main__":
    main()
