// Probes of the CI hunt of 13 October 2026 (A4): slots to paste into
// TestFemOptoelectronics (tests/test_fem_optoelectronics.cpp), above
// anElectroabsorptionModulator. zzMeshOut prints the photodiode's mesh as this
// platform makes it (run it with -maxwarnings 0: QtTest stops at 2000 lines);
// zzSolveOnMesh and zzWhereSlow read such a mesh from PROBE_MESH
// (mesh_linux.txt beside this file is CI's Linux's); zzRefine needs
// PROBE_STRIPE_H, zzExampleFigures none (switches.diff has both switches).

    // (A ci/ branch's diagnostic, not for main: aPhotodiodesStepResponse's
    // mesh as this platform makes it, printed whole - its nodes to 17 digits.)
    void zzMeshOut()
    {
        const QString light = QStringLiteral(R"({"type": "optical", "mode": "emw", "P0": "1[uW]", "selection": {"only": ["pplus", "absorber", "nplus"]}},)");
        const Model m = modelOf(stripe(QStringLiteral("15[um]"), QStringLiteral(R"("material": "Germanium on silicon")"), QStringLiteral("0.7"), light,
                                       QStringLiteral(R"x({"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]}]})x")));
        const auto ps = evaluateParameters(m);
        const GeometryBuild g = buildGeometry(m, *ps);
        QVERIFY(g.ok());
        const MeshBuild b = buildMesh(m, *ps, g.topology);
        QVERIFY2(b.mesh, qPrintable(b.error));
        const Mesh& mesh = *b.mesh;
        const Topology& t = *g.topology;
        qInfo("MESHH %.17g %d %d %d %d topology %d %d %d %d", mesh.unitScale, int(mesh.nodes.size()), int(mesh.triangles.size()), int(mesh.edges.size()),
              int(mesh.pointNode.size()), int(t.vertices.size()), int(t.boundaries.size()), int(t.domains.size()), int(t.points.size()));
        for (const QPointF& p : mesh.nodes) qInfo("MESHN %.17g %.17g", p.x(), p.y());
        for (std::size_t k = 0; k < mesh.triangles.size(); ++k)
            qInfo("MESHT %d %d %d %d", mesh.triangles[k][0], mesh.triangles[k][1], mesh.triangles[k][2], mesh.domain[k]);
        for (const Mesh::Edge& e : mesh.edges) qInfo("MESHE %d %d %d %d %.17g %.17g %d %d", e.a, e.b, e.boundary, e.curve, e.ta, e.tb, e.left, e.right);
        QStringList points;
        for (int n : mesh.pointNode) points << QString::number(n);
        qInfo("MESHP %s", qPrintable(points.join(QLatin1Char(' '))));
    }

    // (A probe: aPhotodiodesStepResponse's studies on a mesh read from
    // PROBE_MESH - zzMeshOut's lines, as a CI log has them.)
    void zzSolveOnMesh()
    {
        QFile file(qEnvironmentVariable("PROBE_MESH"));
        if (!file.open(QIODevice::ReadOnly)) QSKIP("no PROBE_MESH");
        auto mesh = std::make_shared<Mesh>();
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine());
            const int at = line.indexOf(QLatin1String("MESH"));
            if (at < 0) continue;
            const QStringList w = line.mid(at).simplified().split(QLatin1Char(' '));
            if (w[0] == QLatin1String("MESHH")) mesh->unitScale = w[1].toDouble();
            else if (w[0] == QLatin1String("MESHN")) mesh->nodes.emplace_back(w[1].toDouble(), w[2].toDouble());
            else if (w[0] == QLatin1String("MESHT")) {
                mesh->triangles.push_back({w[1].toInt(), w[2].toInt(), w[3].toInt()});
                mesh->domain.push_back(w[4].toInt());
            } else if (w[0] == QLatin1String("MESHE")) {
                Mesh::Edge e;
                e.a = w[1].toInt();
                e.b = w[2].toInt();
                e.boundary = w[3].toInt();
                e.curve = w[4].toInt();
                e.ta = w[5].toDouble();
                e.tb = w[6].toDouble();
                e.left = w[7].toInt();
                e.right = w[8].toInt();
                mesh->edges.push_back(e);
            } else if (w[0] == QLatin1String("MESHP"))
                for (int k = 1; k < w.size(); ++k) mesh->pointNode.push_back(w[k].toInt());
        }
        qInfo("read %d nodes, %d triangles, %d edges", int(mesh->nodes.size()), int(mesh->triangles.size()), int(mesh->edges.size()));
        auto model = [](const QString& power, const QString& last) {
            const QString light = QStringLiteral(R"({"type": "optical", "mode": "emw", "P0": "%1", "selection": {"only": ["pplus", "absorber", "nplus"]}},)").arg(power);
            return modelOf(stripe(QStringLiteral("15[um]"), QStringLiteral(R"("material": "Germanium on silicon")"), QStringLiteral("0.7"), light,
                                  QStringLiteral(R"x({"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]},
                {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.9"}, %1]})x").arg(last)));
        };
        const Model fm = model(QStringLiteral("1[uW]"), QStringLiteral(R"x({"type": "stationary", "physics": ["semi"]},
                {"type": "frequency", "physics": ["semi"], "frequencies": "1[MHz], 1[GHz], 5[GHz], 10[GHz], 20[GHz]"})x"));
        const auto ps = evaluateParameters(fm);
        const GeometryBuild g = buildGeometry(fm, *ps);
        QVERIFY(g.ok());
        mesh->topology = g.topology;
        mesh->computeStatistics();
        const StudyResult fr = solveStudy(fm, ps, mesh, QString());
        QVERIFY2(fr.error.isEmpty(), qPrintable(fr.error));
        Solution& f = *fr.solution;
        for (const QString& line : f.log) qInfo("log: %s", qPrintable(line.left(200)));
        f.select(0);
        const double lit = global(f, QStringLiteral("semi.I_k")), hLow = global(f, QStringLiteral("semi.Hop_k"));
        QStringList h;
        for (int k = 1; k < 5; ++k) {
            f.select(k);
            h << QStringLiteral("%1 GHz %2").arg(f.snapshotFrequency(k) / 1e9).arg(global(f, QStringLiteral("semi.Hop_k")) / hLow, 0, 'f', 6);
        }
        qInfo("rest: I_k %.8g A; Hop_k at 1 MHz %.8g; of it: %s", lit, hLow, qPrintable(h.join(QLatin1String(", "))));
        const Model tm = model(QStringLiteral("1[uW]*min(t/0.5[ps], 1)"), QStringLiteral(R"x({"type": "transient", "physics": ["semi"],
                "times": "range(0, 1[ps], 16[ps]), range(18[ps], 2[ps], 40[ps]), range(45[ps], 5[ps], 100[ps])", "rtol": "1e-3"})x"));
        const auto ts = evaluateParameters(tm);
        const StudyResult tr = solveStudy(tm, ts, mesh, QString());
        QVERIFY2(tr.error.isEmpty(), qPrintable(tr.error));
        Solution& s = *tr.solution;
        s.select(0);
        const double dark = global(s, QStringLiteral("semi.I_k"));
        QStringList trace;
        for (int k = 0; k < s.snapshotCount(); ++k) {
            s.select(k);
            if (k % 5 == 0 || k == s.snapshotCount() - 1)
                trace << QStringLiteral("%1 %2").arg(s.snapshotTime(k) * 1e12, 0, 'g', 4).arg((global(s, QStringLiteral("semi.I_k")) - dark) / (lit - dark), 0, 'f', 6);
        }
        qInfo("dark %.8g; the step by ps: %s", dark, qPrintable(trace.join(QLatin1String("; "))));
    }

    // (A probe: on PROBE_MESH's mesh, the carriers still missing at 40 ps
    // against the photodiode lit at rest - where its slow part fills.)
    void zzWhereSlow()
    {
        QFile file(qEnvironmentVariable("PROBE_MESH"));
        if (!file.open(QIODevice::ReadOnly)) QSKIP("no PROBE_MESH");
        auto mesh = std::make_shared<Mesh>();
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine());
            const int at = line.indexOf(QLatin1String("MESH"));
            if (at < 0) continue;
            const QStringList w = line.mid(at).simplified().split(QLatin1Char(' '));
            if (w[0] == QLatin1String("MESHH")) mesh->unitScale = w[1].toDouble();
            else if (w[0] == QLatin1String("MESHN")) mesh->nodes.emplace_back(w[1].toDouble(), w[2].toDouble());
            else if (w[0] == QLatin1String("MESHT")) {
                mesh->triangles.push_back({w[1].toInt(), w[2].toInt(), w[3].toInt()});
                mesh->domain.push_back(w[4].toInt());
            } else if (w[0] == QLatin1String("MESHE")) {
                Mesh::Edge e;
                e.a = w[1].toInt();
                e.b = w[2].toInt();
                e.boundary = w[3].toInt();
                e.curve = w[4].toInt();
                e.ta = w[5].toDouble();
                e.tb = w[6].toDouble();
                e.left = w[7].toInt();
                e.right = w[8].toInt();
                mesh->edges.push_back(e);
            } else if (w[0] == QLatin1String("MESHP"))
                for (int k = 1; k < w.size(); ++k) mesh->pointNode.push_back(w[k].toInt());
        }
        auto model = [](const QString& power, const QString& last) {
            const QString light = QStringLiteral(R"({"type": "optical", "mode": "emw", "P0": "%1", "selection": {"only": ["pplus", "absorber", "nplus"]}},)").arg(power);
            return modelOf(stripe(QStringLiteral("15[um]"), QStringLiteral(R"("material": "Germanium on silicon")"), QStringLiteral("0.7"), light,
                                  QStringLiteral(R"x({"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]},
                {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.9"}, %1]})x").arg(last)));
        };
        const Model fm = model(QStringLiteral("1[uW]"), QStringLiteral(R"x({"type": "stationary", "physics": ["semi"]})x"));
        const auto ps = evaluateParameters(fm);
        const GeometryBuild g = buildGeometry(fm, *ps);
        QVERIFY(g.ok());
        mesh->topology = g.topology;
        mesh->computeStatistics();
        const StudyResult fr = solveStudy(fm, ps, mesh, QString());
        QVERIFY2(fr.error.isEmpty(), qPrintable(fr.error));
        Solution& f = *fr.solution;
        f.select(-1);
        const int fi = f.fieldIndex(QStringLiteral("semi"));
        const std::vector<double> rest = f.fields()[std::size_t(fi)].values;
        const Model tm = model(QStringLiteral("1[uW]*min(t/0.5[ps], 1)"), QStringLiteral(R"x({"type": "transient", "physics": ["semi"],
                "times": "range(0, 1[ps], 16[ps]), range(18[ps], 2[ps], 40[ps])", "rtol": "1e-3"})x"));
        const auto ts = evaluateParameters(tm);
        const StudyResult tr = solveStudy(tm, ts, mesh, QString());
        QVERIFY2(tr.error.isEmpty(), qPrintable(tr.error));
        Solution& s = *tr.solution;
        s.select(s.snapshotCount() - 1);
        const std::vector<double> now = s.fields()[std::size_t(s.fieldIndex(QStringLiteral("semi")))].values;
        const std::size_t N = mesh->nodes.size();
        QVERIFY(rest.size() >= 3 * N && now.size() >= 3 * N);
        // Each node's box (a third of each triangle's area), its domains.
        std::vector<double> box(N, 0.0);
        std::vector<QSet<int>> domains(N);
        for (std::size_t t = 0; t < mesh->triangles.size(); ++t) {
            const auto& v = mesh->triangles[t];
            const QPointF a = mesh->nodes[std::size_t(v[0])], b = mesh->nodes[std::size_t(v[1])], c = mesh->nodes[std::size_t(v[2])];
            const double area = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y())) / 2;
            for (int k : v) {
                box[std::size_t(k)] += area / 3;
                domains[std::size_t(k)].insert(mesh->domain[t]);
            }
        }
        std::map<QString, std::pair<double, double>> byDomains;   // the domains' set: missing electrons, holes (µm² m⁻³)
        struct Node {
            double dn, dp;
            std::size_t i;
        };
        std::vector<Node> nodes;
        double totalN = 0, totalP = 0;
        for (std::size_t i = 0; i < N; ++i) {
            const double dn = (rest[N + i] - now[N + i]) * box[i], dp = (rest[2 * N + i] - now[2 * N + i]) * box[i];
            QList<int> ds = domains[i].values();
            std::sort(ds.begin(), ds.end());
            QStringList names;
            for (int d : ds) names << QString::number(d);
            auto& [sn, sp] = byDomains[names.join(QLatin1Char('+'))];
            sn += dn;
            sp += dp;
            totalN += dn;
            totalP += dp;
            nodes.push_back({dn, dp, i});
        }
        qInfo("missing at 40 ps (µm² m⁻³, box-weighted): electrons %.4g, holes %.4g", totalN, totalP);
        for (const auto& [key, v] : byDomains) qInfo("  nodes of domains %s: electrons %.4g, holes %.4g", qPrintable(key), v.first, v.second);
        std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return std::abs(a.dp) + std::abs(a.dn) > std::abs(b.dp) + std::abs(b.dn); });
        for (std::size_t k = 0; k < 15 && k < nodes.size(); ++k) {
            const Node& n = nodes[k];
            QList<int> ds = domains[n.i].values();
            std::sort(ds.begin(), ds.end());
            QStringList names;
            for (int d : ds) names << QString::number(d);
            qInfo("  node %d at (%.5f, %.5f) in %s: electrons %.4g (n %.4g at rest), holes %.4g (p %.4g at rest)", int(n.i), mesh->nodes[n.i].x(), mesh->nodes[n.i].y(),
                  qPrintable(names.join(QLatin1Char('+'))), n.dn, rest[N + n.i], n.dp, rest[2 * N + n.i]);
        }
    }

    // (A probe: the photodiode lit, at rest and at 10 GHz, at PROBE_STRIPE_H.)
    void zzRefine()
    {
        const QString light = QStringLiteral(R"({"type": "optical", "mode": "emw", "P0": "1[uW]", "selection": {"only": ["pplus", "absorber", "nplus"]}},)");
        const Model m = modelOf(stripe(QStringLiteral("15[um]"), QStringLiteral(R"("material": "Germanium on silicon")"), QStringLiteral("0.7"), light,
                                       QStringLiteral(R"x({"type": "study", "steps": [{"type": "stationary", "physics": ["semi"]},
                {"type": "mode", "physics": ["emw"], "frequency": "1550[nm]", "modes": 1, "neff": "3.9"}, {"type": "stationary", "physics": ["semi"]},
                {"type": "frequency", "physics": ["semi"], "frequencies": "1[MHz], 1[GHz], 10[GHz]"}]})x")));
        const Run r = run(m);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        Solution& f = *r.solution;
        f.select(0);
        const double lit = global(f, QStringLiteral("semi.I_k")), h0 = global(f, QStringLiteral("semi.Hop_k"));
        f.select(1);
        const double h1 = global(f, QStringLiteral("semi.Hop_k"));
        f.select(2);
        const double h10 = global(f, QStringLiteral("semi.Hop_k"));
        qInfo("REFINE h %s %s: %d nodes: I_k %.6g A, H(1 GHz) %.6f, H(10 GHz) %.6f", qPrintable(qEnvironmentVariable("PROBE_STRIPE_H")),
              qPrintable(qEnvironmentVariable("PROBE_IFACE", QStringLiteral("first"))), int(f.mesh().nodes.size()), lit, h1 / h0, h10 / h0);
    }

    // (A probe: the two examples' figures as their descriptions give them.)
    void zzExampleFigures()
    {
        for (const char* power : {"1[mW]", "25[uW]"}) {
            QFile f(QStringLiteral(QUCS_EXAMPLES_DIR "/multiphysics/ge_photodiode.qfem"));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QByteArray json = f.readAll();
            QVERIFY(json.contains(R"("expression": "1[mW]")"));
            json.replace(R"("expression": "1[mW]")", QByteArray(R"("expression": ")") + power + "\"");
            Model m = modelOf(json);
            const Run r = run(m, QStringLiteral("std2"));
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            Solution& s = *r.solution;
            std::vector<double> fr, own, rl;
            for (int k = 0; k < s.snapshotCount(); ++k) {
                s.select(k);
                fr.push_back(s.snapshotFrequency(k));
                own.push_back(global(s, QStringLiteral("semi.Hop_k")));
                rl.push_back(global(s, QStringLiteral("semi.HopRL_k")));
            }
            auto f3 = [&](const std::vector<double>& h) {
                for (std::size_t k = 1; k < h.size(); ++k)
                    if (h[k] / h[0] < M_SQRT1_2 && h[k - 1] / h[0] >= M_SQRT1_2) {
                        const double a = std::log(h[k - 1] / h[0]) - std::log(M_SQRT1_2), b = std::log(h[k - 1] / h[0]) - std::log(h[k] / h[0]);
                        return std::exp(std::log(fr[k - 1]) + a / b * (std::log(fr[k]) - std::log(fr[k - 1])));
                    }
                return double(NAN);
            };
            s.select(0);
            qInfo("FIG photodiode at %s: I_k %.5g A, its own 3 dB at %.1f GHz, through 50 Ω %.1f GHz", power, global(s, QStringLiteral("semi.I_k")), f3(own) / 1e9, f3(rl) / 1e9);
        }
        Model m = example(QStringLiteral("gesi_eam.qfem"));
        const Run r = run(m, QStringLiteral("std1"));
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QStringList said;
        double l0 = NAN;
        for (const auto& point : r.study.solutions) {
            const double l = global(*point, QStringLiteral("emw.loss")) * 50e-6;
            if (std::isnan(l0)) l0 = l;
            said << QStringLiteral("%1 V %2 dB (%3 more), %4 mA").arg(point->sweptValues.value(QStringLiteral("Va"))).arg(l, 0, 'f', 3).arg(l - l0, 0, 'f', 3)
                        .arg(global(*point, QStringLiteral("semi.I_k")) * 1e3, 0, 'f', 4);
        }
        qInfo("FIG modulator: %s", qPrintable(said.join(QLatin1String("; "))));
    }

