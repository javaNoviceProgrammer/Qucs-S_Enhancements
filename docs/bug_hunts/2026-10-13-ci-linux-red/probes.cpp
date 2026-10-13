// Probes of the CI hunt of 13 October 2026: slots to paste into TestFemDevices
// (tests/test_fem_devices.cpp), above aSweepStartsFromTheLast; run one by name with
// its environment: PROBE_W and PROBE_F (zzProbeFn: widths and interface sizes, µm),
// PROBE_H (zzProbeDiode, zzProbeBeyond: mesh sizes, µm), PROBE_V (zzProbeBeyond: volts).

    void zzProbeFn()
    {
        constexpr double H = 6.62607015e-34, M0 = 9.1093837015e-31;
        const double phiB = 3.1 * Q, mox = 0.42;
        const double A = Q * Q * Q / (8 * M_PI * H * phiB * mox), B = 8 * M_PI * std::sqrt(2 * mox * M0) * std::pow(phiB, 1.5) / (3 * Q * H);
        const QStringList widths = qEnvironmentVariable("PROBE_W", QStringLiteral("0.1")).split(QLatin1Char(','));
        const QStringList fines = qEnvironmentVariable("PROBE_F", QStringLiteral("0.002")).split(QLatin1Char(','));
        for (const QString& w : widths)
            for (const QString& f : fines) {
                QByteArray json = mosCapacitor(QStringLiteral(R"(, {"type": "fn", "selection": {"of": ["surface"], "side": "top"}})"),
                                               QStringLiteral(R"({"type": "stationary"})"), w, f);
                json.replace(R"("expression": "0[V]")", R"("expression": "10[V]")");
                const Run r = run(modelOf(json));
                if (!r.error.isEmpty()) {
                    qInfo("w %s f %s: %s", qPrintable(w), qPrintable(f), qPrintable(r.error));
                    continue;
                }
                const double W = w.toDouble() * 1e-6;
                const double Eox = at1(*r.solution, QStringLiteral("semi.normE"), W * 1e6 / 2, 0.005);
                const double J = A * Eox * Eox * std::exp(-B / Eox), I = global(*r.solution, QStringLiteral("semi.Ifn"));
                double lo = 1e300, hi = 0, sumJ = 0;
                const int m = 400;
                for (int i = 0; i < m; ++i) {
                    const double x = W * 1e6 * (i + 0.5) / m;
                    const double E = at1(*r.solution, QStringLiteral("semi.normE"), x, 0.0002);
                    lo = std::min(lo, E);
                    hi = std::max(hi, E);
                    sumJ += A * E * E * std::exp(-B / E) / m;
                }
                QStringList depths;
                for (double y : {0.0002, 0.001, 0.002, 0.003, 0.005, 0.008}) {
                    double a = 1e300, b = 0;
                    for (int i = 0; i < m; ++i) {
                        const double E = at1(*r.solution, QStringLiteral("semi.normE"), W * 1e6 * (i + 0.5) / m, y);
                        a = std::min(a, E);
                        b = std::max(b, E);
                    }
                    depths << QStringLiteral("%1 nm: %2..%3").arg(y * 1000).arg(a / 1e8, 0, 'f', 4).arg(b / 1e8, 0, 'f', 4);
                }
                double vLo = 1e300, vHi = -1e300;
                for (int i = 0; i < m; ++i) {
                    const double V = at1(*r.solution, QStringLiteral("semi.V"), W * 1e6 * (i + 0.5) / m, -1e-6);
                    vLo = std::min(vLo, V);
                    vHi = std::max(vHi, V);
                }
                qInfo("   %s; V at the interface %.5f..%.5f", qPrintable(depths.join(QLatin1String(", "))), vLo, vHi);
                // Estimates over the interface's edges: the field at the
                // turning point (depth 3.1 V/E), the mean over the barrier
                // (where V has fallen 3.1 V), the field at fixed depths.
                const Mesh& mesh = r.solution->mesh();
                double Ib = 0, Ic = 0, Id = 0, Ie = 0, len = 0;
                for (const Mesh::Edge& e : mesh.edges) {
                    const QPointF a = mesh.nodes[std::size_t(e.a)], b = mesh.nodes[std::size_t(e.b)];
                    if (std::abs(a.y()) > 1e-9 || std::abs(b.y()) > 1e-9) continue;
                    const double L = std::abs(b.x() - a.x()) * 1e-6, xm = (a.x() + b.x()) / 2;
                    len += L;
                    // (b): the field at the turning point, iterated.
                    double E = Eox;
                    for (int it = 0; it < 4; ++it) E = at1(*r.solution, QStringLiteral("semi.normE"), xm, std::min(3.1 / E * 1e6, 0.0099));
                    Ib += A * E * E * std::exp(-B / E) * L;
                    // (c): the turning point where V has fallen 3.1 V: bisection along y.
                    const double V0 = at1(*r.solution, QStringLiteral("semi.V"), xm, 0.0);
                    double lo = 0, hi = 0.0099;
                    for (int it = 0; it < 50; ++it) {
                        const double mid = (lo + hi) / 2;
                        const double drop = std::abs(at1(*r.solution, QStringLiteral("semi.V"), xm, mid) - V0);
                        (drop < 3.1 ? lo : hi) = mid;
                    }
                    const double Ec = 3.1 / (lo * 1e-6);
                    Ic += A * Ec * Ec * std::exp(-B / Ec) * L;
                    const double Ed = at1(*r.solution, QStringLiteral("semi.normE"), xm, 0.002);
                    Id += A * Ed * Ed * std::exp(-B / Ed) * L;
                    const double Ee = at1(*r.solution, QStringLiteral("semi.normE"), xm, 0.005);
                    Ie += A * Ee * Ee * std::exp(-B / Ee) * L;
                }
                double Eavg = 0;
                for (int i = 0; i < m; ++i) Eavg += at1(*r.solution, QStringLiteral("semi.normE"), W * 1e6 * (i + 0.5) / m, 0.005) / m;
                const double Jr = A * Eavg * Eavg * std::exp(-B / Eavg) * W;
                qInfo("   Eavg %.5f: Ifn %.5f, turning point's field %.5f, the barrier's mean %.5f, at 2 nm %.5f, at 5 nm %.5f (of J(Eavg) W)", Eavg / 1e8, I / Jr, Ib / Jr, Ic / Jr,
                      Id / Jr, Ie / Jr);
                Q_UNUSED(len);
                int clamped = 0;
                for (const QString& line : r.solution->log)
                    if (line.contains(QLatin1String("not Delaunay"))) ++clamped;
                qInfo("w %s f %s: nodes %d: I/(J W) %.5f (Eox %.5f MV/cm); at the interface E %.5f..%.5f MV/cm, <J> W / I %.5f%s", qPrintable(w), qPrintable(f),
                      int(r.solution->mesh().nodes.size()), I / (J * W), Eox / 1e8, lo / 1e8, hi / 1e8, sumJ * W / I, clamped ? " NOT DELAUNAY" : "");
            }
    }

    void zzProbeBeyond()
    {
        const QStringList hs = qEnvironmentVariable("PROBE_H", QStringLiteral("0.12")).split(QLatin1Char(','));
        const QStringList vs = qEnvironmentVariable("PROBE_V", QStringLiteral("-60")).split(QLatin1Char(','));
        for (const QString& h : hs)
            for (const QString& v : vs) {
                QByteArray beyond = diode(QStringLiteral("1e16[1/cm^3]"), QStringLiteral("4"), QStringLiteral(R"(, {"type": "impact", "selection": {"all": true}})"),
                                          QStringLiteral(R"({"type": "stationary"})"), h);
                beyond.replace(R"({"name": "Va", "expression": "0[V]"})", QStringLiteral(R"({"name": "Va", "expression": "%1[V]"})").arg(v).toUtf8());
                QElapsedTimer timer;
                timer.start();
                const Run broken = run(modelOf(beyond));
                QString last;
                if (broken.solution)
                    for (const QString& line : broken.solution->log) last = line;
                qInfo("h %s at %s V: error '%s'; I_k %g; %lld ms; last log: %s", qPrintable(h), qPrintable(v), qPrintable(broken.error.left(200)),
                      broken.solution ? global(*broken.solution, QStringLiteral("semi.I_k")) : NAN, timer.elapsed(), qPrintable(last.left(200)));
            }
    }

    void zzProbeDiode()
    {
        const QStringList hs = qEnvironmentVariable("PROBE_H", QStringLiteral("0.1")).split(QLatin1Char(','));
        for (const QString& h : hs) {
            const Run r = run(modelOf(diode(QStringLiteral("1e16[1/cm^3]"), QStringLiteral("4"), QString(),
                                            QStringLiteral(R"({"type": "sweep", "parameters": [["Va", "-10", "V"]]}, {"type": "stationary"})"), h)));
            if (!r.error.isEmpty()) {
                qInfo("h %s: %s", qPrintable(h), qPrintable(r.error));
                continue;
            }
            QString said;
            for (const auto& point : r.study.solutions)
                for (const QString& line : point->log)
                    if (line.contains(QLatin1String("not Delaunay"))) said = line;
            qInfo("h %s: nodes %d %s", qPrintable(h), int(r.study.solutions.front()->mesh().nodes.size()), qPrintable(said));
            const Mesh& mesh = r.study.solutions.front()->mesh();
            std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> around;
            for (int t = 0; t < int(mesh.triangles.size()); ++t)
                for (int k = 0; k < 3; ++k) {
                    const int a = mesh.triangles[std::size_t(t)][std::size_t(k)], b = mesh.triangles[std::size_t(t)][std::size_t((k + 1) % 3)];
                    around[{std::min(a, b), std::max(a, b)}].push_back({t, mesh.triangles[std::size_t(t)][std::size_t((k + 2) % 3)]});
                }
            std::set<std::pair<int, int>> segs;
            for (const Mesh::Edge& e : mesh.edges) segs.insert({std::min(e.a, e.b), std::max(e.a, e.b)});
            for (const auto& [edge, list] : around) {
                double sum = 0, mag = 0;
                QStringList angles;
                for (const auto& [t, o] : list) {
                    const QPointF u = mesh.nodes[std::size_t(edge.first)] - mesh.nodes[std::size_t(o)], v = mesh.nodes[std::size_t(edge.second)] - mesh.nodes[std::size_t(o)];
                    const double c = (u.x() * v.x() + u.y() * v.y()) / std::abs(u.x() * v.y() - u.y() * v.x());
                    sum += c;
                    mag += std::abs(c);
                    angles << QString::number(std::atan2(std::abs(u.x() * v.y() - u.y() * v.x()), u.x() * v.x() + u.y() * v.y()) * 180 / M_PI, 'f', 3)
                                  + QStringLiteral("@(%1,%2)").arg(mesh.nodes[std::size_t(o)].x(), 0, 'g', 8).arg(mesh.nodes[std::size_t(o)].y(), 0, 'g', 8);
                }
                if (sum < -1e-9 * mag)
                    qInfo("  edge (%.8g,%.8g)-(%.8g,%.8g)%s: cot sum %.3g, opposite %s", mesh.nodes[std::size_t(edge.first)].x(), mesh.nodes[std::size_t(edge.first)].y(),
                          mesh.nodes[std::size_t(edge.second)].x(), mesh.nodes[std::size_t(edge.second)].y(), segs.count(edge) ? " (a segment)" : "", sum,
                          qPrintable(angles.join(QLatin1String(", "))));
            }
        }
    }

