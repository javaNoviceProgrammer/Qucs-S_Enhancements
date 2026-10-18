// Probe functions of the 2026-10-17 hunt, pasted into the private slots of test_canvas_drawing.cpp
// (each skips unless its environment variable is set; see the report).

    void huntCulledDiagrams()
    {
        const QString seed = qEnvironmentVariable("HUNT_SEED");
        if (seed.isEmpty()) QSKIP("probe");
        QucsApp app(false);
        MainGuard guard(&app);
        Schematic* s = open(app, seed, {1100, 800});
        QVERIFY(s != nullptr);
        const auto missing = [](const QImage& a, const QImage& b, const QRect& area) -> std::optional<QPoint> {
            const QRect r = area & a.rect();
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) {
                    const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                    if (std::max({std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)), std::abs(qBlue(p) - qBlue(q))}) > 64)
                        return QPoint(x, y);
                }
            return std::nullopt;
        };
        const QList<QPoint> centres{{1100, 700}, {1300, 500}, {1500, 400}, {1300, 300}, {100, 400}, {1000, 650}};
        int k = 0;
        for (const bool selected : {false, true}) {
            for (Diagram* d : s->a_DocDiags)
                for (Graph* g : d->Graphs) g->isSelected = selected;
            for (const double z : {3.0, 7.0}) {
                for (const QPoint& c : centres) {
                    s->zoomBy(z / s->getScale());
                    s->centerOn(c);
                    pump();
                    const QImage whole = canvasImage(s);
                    app.magPlus->trigger();
                    const QPoint from = s->viewport()->rect().center() - QPoint(150, 150);
                    QTest::mousePress(s->viewport(), Qt::LeftButton, Qt::NoModifier, from);
                    pump();
                    moveTo(s, from + QPoint(6, 4), Qt::LeftButton);
                    const QImage step = canvasImage(s);
                    QTest::keyClick(s->viewport(), Qt::Key_Escape);
                    pump();
                    app.select->trigger();
                    const QRect band = QRect(from, from + QPoint(6, 4)).normalized().adjusted(-3, -3, 3, 3);
                    std::optional<QPoint> at;
                    for (const QRect& r : (QRegion(whole.rect()) - QRegion(band)))
                        if (!at) at = missing(whole, step, r);
                    qInfo("selected %d zoom %.0f centre (%d,%d): %s", int(selected), z, c.x(), c.y(),
                          at ? qPrintable(QStringLiteral("DIFFERS at (%1,%2) %3 against %4").arg(at->x()).arg(at->y())
                                              .arg(whole.pixelColor(*at).name(), step.pixelColor(*at).name())) : "same");
                    if (at && !qEnvironmentVariable("HUNT_SHOTS").isEmpty()) {
                        whole.save(qEnvironmentVariable("HUNT_SHOTS") + QStringLiteral("/cull%1_whole.png").arg(k));
                        step.save(qEnvironmentVariable("HUNT_SHOTS") + QStringLiteral("/cull%1_step.png").arg(k));
                    }
                    ++k;
                }
            }
        }
    }

