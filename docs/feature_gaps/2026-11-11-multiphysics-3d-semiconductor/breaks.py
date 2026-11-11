#!/usr/bin/env python3
"""breaks.py: Multiphysics 3D's semiconductors (11 November) undone on
purpose, one piece at a time: the carriers' Scharfetter-Gummel fluxes,
Poisson's charge, the boxes' shares, the contacts, recombination, the
mobility models, heterojunctions, traps, polarization, Fowler-Nordheim,
circuits, the Hall effect, impact ionization, hot carriers, optical
generation, noise, the heat, the reported currents. Each built and the
test functions that should catch it run - test_fem3_semiconductor's binary
with ctest's own environment (offscreen), its functions named: the whole
suite takes some ten minutes. Sources put back whatever happens.
BRK_ONLY_CHECK=1 checks the anchors; BRK_ONLY=3,7 runs those alone;
HUNT_RUNS=<dir> for logs. It works on the checkout it is in, and its Release
build beside it (build-rel/)."""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Q = os.path.join(ROOT, "qucs-s-26.1.1", "qucs") + "/"
B = os.path.join(ROOT, "build-rel")
RUNS = os.environ.get("HUNT_RUNS", os.path.join(ROOT, "..", "multiphysics3d-semiconductor-breaks"))
os.makedirs(RUNS, exist_ok=True)

SM = "multiphysics/fem3_semiconductor.cpp"

BREAKS = [
 ("electrons' flux its Bernoulli functions swapped",
  [(SM, "            const double g = w * (ni * bm - nj * bp);", "            const double g = w * (ni * bp - nj * bm);")], ["shortDiode"]),
 ("holes' flux its field's sign turned",
  [(SM, "            const double g = w * (pi * bp - pj * bm);", "            const double g = w * (pi * bm - pj * bp);")], ["shortDiode"]),
 ("Poisson's charge its electrons' sign turned",
  [(SM, "        F(iV(i)) -= Q * (bP * p - bN * n + box * (nd - na));\n        add(iV(i), iP(i)", "        F(iV(i)) -= Q * (bP * p + bN * n + box * (nd - na));\n        add(iV(i), iP(i)")],
  ["shortDiode"]),
 ("a corner's Voronoi share halved",
  [(SM, "    return (dot(vx, vy, vx, vy) * atQ + dot(ux, uy, ux, uy) * atR) / (16 * area);", "    return (dot(vx, vy, vx, vy) * atQ + dot(ux, uy, ux, uy) * atR) / (32 * area);")],
  ["uniformInZ"]),
 ("a vertical edge's coupling times the height, not over it",
  [(SM, "            a_edges[std::size_t(ed)].eps += eps * share / h;", "            a_edges[std::size_t(ed)].eps += eps * share * h * 1e12;")], ["junctionAtRest"]),
 ("an ohmic contact's potential without its built-in part",
  [(SM, "                held[std::size_t(iV(i))] = v0 + u;\n                // (Its minority", "                held[std::size_t(iV(i))] = v0;\n                // (Its minority")],
  ["junctionAtRest"]),
 ("a gate's work function's sign turned",
  [(SM, "                held[std::size_t(iV(i))] = v0 - (ct.phim - a_refChi - a_refDEc) / Q;", "                held[std::size_t(iV(i))] = v0 + (ct.phim - a_refChi - a_refDEc) / Q;")],
  ["mosCapacitor"]),
 ("SRH's excess n p + ni²",
  [(SM, "ni = std::sqrt(ni2), ex = n * p - ni2;", "ni = std::sqrt(ni2), ex = n * p + ni2;")], ["recombinationRates"]),
 ("Caughey and Thomas's mobility without its exponent",
  [(SM, "            mun = m.ct[0][0] + (m.mun - m.ct[0][0]) / (1 + std::pow(N / m.ct[0][1], m.ct[0][2]));",
    "            mun = m.ct[0][0] + (m.mun - m.ct[0][0]) / (1 + N / m.ct[0][1]);")], ["resistorMobility"]),
 ("generation recombining",
  [(SM, "        double RV = rbox * recombined - box * a_generation[std::size_t(i)];", "        double RV = rbox * recombined + box * a_generation[std::size_t(i)];")],
  ["photodiodeCollects"]),
 ("a Schottky barrier's electrons e^(-0.9 φB/kT)",
  [(SM, "const double nB = m.Nc * std::exp(-(c.phiB - a_gap[std::size_t(i)] / 2) / (KB * a_T));", "const double nB = m.Nc * std::exp(-0.9 * (c.phiB - a_gap[std::size_t(i)] / 2) / (KB * a_T));")],
  ["schottkyDiode"]),
 ("the equilibrium path taken under generation",
  [(SM, "    for (double g : a_generation) equilibrium = equilibrium && g == 0;\n", "")], ["photodiodeCollects"]),
 ("a surface charge's sign turned",
  [(SM, "            F(iV(i)) -= area * sf.rhos;", "            F(iV(i)) += area * sf.rhos;")], ["mosCapacitor"]),
 ("the reported current holes less electrons the wrong way",
  [(SM, "                I += Q * a_C0 * (gp - gn) * dw;", "                I += Q * a_C0 * (gn - gp) * dw;")], ["resistorMobility"]),
 ("a floor's charge lumped by thirds, not its boxes' shares",
  [(SM, "            const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];\n            if (!mf.quad) {", "            const Mesh3::Face& mf = mesh.faces[std::size_t(fi)];\n            if (false) {")],
  ["interfaceTraps"]),
 ("an interface trap's charge's sign turned",
  [(SM, "                    F(iV(i)) += Q * lv.N * area * (sf.acceptor ? st.f : st.f - 1);", "                    F(iV(i)) -= Q * lv.N * area * (sf.acceptor ? st.f : st.f - 1);")],
  ["interfaceTraps"]),
 ("traps following at once in small signal",
  [(SM, "                const cd den(st.D, omega);", "                const cd den(st.D, 0.0);")], ["interfaceTraps"]),
 ("recombining over the other side's part of a heterojunction's box",
  [(SM, "                    if (own >= 0 && sameBands(d, own)) a_recBox[std::size_t(i)] += share * h / 2;", "                    if (own >= 0) a_recBox[std::size_t(i)] += share * h / 2;")],
  ["heterojunctionsAgainst2D"]),
 ("abrupt: an end's carriers on its node's bands, not the edge's",
  [(SM, "            *fN = std::exp(*an - a_an[k]);", "            *fN = 1;")], ["heterojunctionsAgainst2D"]),
 ("thermionic emission's Bernoulli toward the wrong end",
  [(SM, "            ed.wn *= Gn / (Gn + ed.wn * a_Vt * bern(ed.emitsAtI ? -Dn : Dn));", "            ed.wn *= Gn / (Gn + ed.wn * a_Vt * bern(ed.emitsAtI ? Dn : -Dn));")],
  ["heterojunctionsAgainst2D"]),
 ("no tunnelling through the spike",
  [(SM, "            const double Gn = ed.vRn * (1 + (field.empty() ? 0.0 : detail::tunnelling(F, ed.stepN, ed.massN, KB * a_T))) * face;",
    "            const double Gn = ed.vRn * face;")], ["heterojunctionsAgainst2D"]),
 ("equal gaps' node the higher domain's",
  [(SM, "(std::abs(g - hh) <= 1e-9 * std::abs(hh) && d < prev)", "(std::abs(g - hh) <= 1e-9 * std::abs(hh) && d > prev)")], ["modelsAgainst2D"]),
 ("trap-assisted tunnelling's trap depth half the gap",
  [(SM, "            const double depthN = (ecEi - m.Et) / kT, depthP = (eiEv + m.Et) / kT;", "            const double depthN = (0.5 * m.Eg - m.Et) / kT, depthP = (0.5 * m.Eg + m.Et) / kT;")],
  ["modelsAgainst2D"]),
 ("polarization's sign turned",
  [(SM, "                sf.rhos = face * (*pb - *pf) * nc;", "                sf.rhos = face * (*pf - *pb) * nc;")], ["polarizationCharge"]),
 ("Fowler-Nordheim's B 3/2 of itself",
  [(SM, "            fn.B = 8 * M_PI * std::sqrt(2 * mox * Mass0) * std::pow(phiB, 1.5) / (3 * Q * Hp);", "            fn.B = 8 * M_PI * std::sqrt(2 * mox * Mass0) * std::pow(phiB, 1.5) / (2 * Q * Hp);")],
  ["fowlerNordheim"]),
 ("Fowler-Nordheim's barrier its whole step",
  [(SM, "                        E = fn.phi / (s + step * (fn.phi - a) / (b - a));", "                        E = fn.phi / (s + step);")], ["fowlerNordheim"]),
 ("a circuit's drop the wrong way",
  [(SM, "        double g = Vc - applied[std::size_t(c)] + R * (I + Ic);", "        double g = Vc - applied[std::size_t(c)] - R * (I + Ic);")], ["contactCircuits"]),
 ("Cp left out in time",
  [(SM, "            Ic = ct.Cp * (a_mass * Vc - historyOf(c, 0));", "            Ic = 0;")], ["contactCircuits"]),
 ("the electrons' Hall sign the holes'",
  [(SM, "            const double sgn = c == 0 ? -1.0 : 1.0;", "            const double sgn = 1.0;")], ["hallBar"]),
 ("the current along the field not slowed, 1/(1 + β²)",
  [(SM, "            mun /= 1 + m.rHn * m.rHn * pr.mun * pr.mun * B2;\n", "")], ["hallBar"]),
 ("Lombardi's without surface roughness",
  [(SM, "                    return 1 / (1 / mu + damp / ac + damp / sr);", "                    return 1 / (1 / mu + damp / ac);")], ["surfaceScatteringAgainst2D"]),
 ("Yamaguchi's without the field across the interface",
  [(SM, "                const double ms = mu0 / std::sqrt(1 + c[3] * damp * perp);", "                const double ms = mu0;")], ["surfaceScatteringAgainst2D"]),
 ("saturation by the field where the force along the current drives it",
  [(SM, "                Fn = std::min(whole, std::sqrt(gn[0] * gn[0] + gn[1] * gn[1] + gn[2] * gn[2]));", "                Fn = whole;")], ["modelsAgainst2D"]),
 ("the transferred electrons' velocity twice its fall",
  [(SM, "                return F > 0 ? (mun0 + vsn / F * r4) / (1 + r4) : mun0;", "                return F > 0 ? (mun0 + 2 * vsn / F * r4) / (1 + r4) : mun0;")], ["modelsAgainst2D"]),
 ("hot carriers relaxing twice as fast",
  [(SM, "            const double k = 1.5 * KB * std::max(c, floor) * corners[std::size_t(i)] / (electrons ? m.tauWn : m.tauWp);",
    "            const double k = 3.0 * KB * std::max(c, floor) * corners[std::size_t(i)] / (electrons ? m.tauWn : m.tauWp);")], ["modelsAgainst2D"]),
 ("impact ionization's currents fitted unweighted",
  [(SM, "            weighed(false, cnv);\n            weighed(true, cpv);\n", "")], ["impactAgainst2D"]),
 ("an electron's ionization integral without its holes' feedback",
  [(SM, "                in += 0.5 * (an[k] * std::exp(K[n - 1] - K[k]) + an[k - 1] * std::exp(K[n - 1] - K[k - 1])) * dx;",
    "                in += 0.5 * (an[k] + an[k - 1]) * dx;")], ["impactAgainst2D"]),
 ("stimulated recombination without its threshold",
  [(SM, "            const double rate = volume > 0 ? st.c * std::max(0.0, recombined / volume - st.R0) : 0.0;",
    "            const double rate = volume > 0 ? st.c * std::max(0.0, recombined / volume) : 0.0;")], ["modelsAgainst2D"]),
 ("optical generation half the light's loss (½ ω ε0 Im εr |E|²)",
  [(SM, "            const double lost = omega * Eps0 * (sum[std::size_t(i)] / vol[std::size_t(i)]) * E * E * P0;   // W/m³",
    "            const double lost = 0.5 * omega * Eps0 * (sum[std::size_t(i)] / vol[std::size_t(i)]) * E * E * P0;   // W/m³")], ["opticalGeneration"]),
 ("diffusion noise twice Nyquist's",
  [(SM, "            const double rate = 2 * w * Vt * harmonic(ci, cj, electrons ? Dn : -Dp);", "            const double rate = 4 * w * Vt * harmonic(ci, cj, electrons ? Dn : -Dp);")], ["noise"]),
 ("noise: an edge's own flux into a contact left out",
  [(SM, "                direct[std::size_t(ck)] += (electrons ? -Q : Q) * sign;", "                direct[std::size_t(ck)] += 0 * sign;")], ["noise"]),
 ("the heat the total current's potential drop",
  [(SM, "        const double P = -Q * gn * (phin[std::size_t(i)] - phin[std::size_t(j)]) + Q * gp * (phip[std::size_t(i)] - phip[std::size_t(j)]);",
    "        const double P = Q * (gp - gn) * (x(iV(i)) - x(iV(j)));")], ["heatOfLight"]),
 ("the light's pairs' energy not heat",
  [(SM, "        heat[std::size_t(i)] += made * a_box[std::size_t(i)];\n", "")], ["heatOfLight"]),
 ("the lattice's temperature not slowing the mobility",
  [(SM, "            mun *= std::pow(TL / a_T, -m.zetan);", "            mun *= 1;")], ["selfHeating"]),
 ("an obtuse triangle's edge's turned current left out of the reported one",
  [(SM, "                if (e.wn > 0) {\n                    const EdgeState st = edgeState(e, x, lgN, lgP);\n                    gn = e.wn",
    "                if (e.wn <= 0) continue;\n                if (e.wn > 0) {\n                    const EdgeState st = edgeState(e, x, lgN, lgP);\n                    gn = e.wn")],
  ["modelsAgainst2D"]),
]


def apply(edits, back):
    for path, old, new in edits:
        p = Q + path
        s = open(p, encoding="utf-8").read()
        if s.count(old) != 1:
            raise SystemExit("%s: anchor %d times: %s" % (path, s.count(old), old[:90]))
        if back is not None and p not in back:
            back[p] = s
        open(p, "w", encoding="utf-8").write(s.replace(old, new))


def run(functions, log):
    """The suite's binary, ctest's environment, the functions named; True when it fails."""
    b = subprocess.run(["cmake", "--build", B, "--target", "test_fem3_semiconductor", "-j8"], capture_output=True, text=True)
    open(log, "w").write(b.stdout + b.stderr)
    if b.returncode != 0:
        return "build failed"
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", ASAN_OPTIONS="detect_leaks=0")
    t = subprocess.run([os.path.join(B, "qucs", "tests", "test_fem3_semiconductor")] + functions + ["-maxwarnings", "0"],
                       cwd=os.path.join(B, "qucs", "tests"), env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=1500)
    open(log, "a").write(t.stdout + t.stderr)
    return "caught" if t.returncode != 0 else "MISSED"


def main():
    only = os.environ.get("BRK_ONLY_CHECK")
    which = {int(n) for n in os.environ.get("BRK_ONLY", "").split(",") if n.strip()}
    results = []
    for i, (name, edits, functions) in enumerate(BREAKS, 1):
        if which and i not in which:
            continue
        back = {}
        print("\n=== %d/%d %s" % (i, len(BREAKS), name), flush=True)
        try:
            apply(edits, back)
            if only:
                results.append((i, name, "anchors ok"))
                continue
            results.append((i, name, run(functions, os.path.join(RUNS, "break%02d.log" % i))))
            print(results[-1][2], flush=True)
        finally:
            for p, s in back.items():
                open(p, "w", encoding="utf-8").write(s)
    if not only:
        subprocess.run(["cmake", "--build", B, "--target", "test_fem3_semiconductor", "-j8"], capture_output=True)
    print("\n".join("%2d %-70s %s" % r for r in results))
    sys.exit(1 if any(r[2] == "MISSED" for r in results) else 0)


if __name__ == "__main__":
    main()
