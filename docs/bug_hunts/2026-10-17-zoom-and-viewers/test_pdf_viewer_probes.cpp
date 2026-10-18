// Probe functions of the 2026-10-17 hunt, pasted into the private slots of test_pdf_viewer.cpp
// (each skips unless its environment variable is set; see the report).

    void huntZoomInTheMargin()
    {
        if (!qEnvironmentVariableIsSet("HUNT_PROBE")) QSKIP("probe");
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        doc->firstPage();
        view->setFit(PageView::Fit::Page);
        shown(view);
        const QRect page = view->pageRect(0);
        qInfo("page %d %d %d %d in a view %d by %d", page.x(), page.y(), page.width(), page.height(), view->viewport()->width(), view->viewport()->height());
        // The pointer in the grey left of the page, half way down.
        const QPoint at(std::max(2, page.left() / 2), page.top() + page.height() / 2);
        QVERIFY(at.x() < page.left());
        const auto notch = [view](QPoint where) {
            QWheelEvent e(QPointF(where), view->viewport()->mapToGlobal(QPointF(where)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                          Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(view->viewport(), &e);
        };
        notch(at);
        QTRY_VERIFY(!view->zooming());
        const QRect after = view->pageRect(0);
        qInfo("after a notch at (%d,%d): page %d %d %d %d", at.x(), at.y(), after.x(), after.y(), after.width(), after.height());
        // The same with the pointer on the page, for comparison.
        view->setFit(PageView::Fit::Page);
        shown(view);
        const QPoint on(page.left() + 10, page.top() + page.height() / 2);
        notch(on);
        QTRY_VERIFY(!view->zooming());
        const QRect after2 = view->pageRect(0);
        qInfo("after a notch at (%d,%d) on the page: page %d %d %d %d", on.x(), on.y(), after2.x(), after2.y(), after2.width(), after2.height());
        view->setFit(PageView::Fit::Width);
    }

    void huntPinchOutOverManyPages()
    {
        if (!qEnvironmentVariableIsSet("HUNT_PROBE")) QSKIP("probe");
        const QString many = dir.filePath("many.pdf");
        QVERIFY(writePdf(many, 80));
        QVERIFY(app->gotoPage(many));
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        QTRY_COMPARE(doc->pageCount(), 80);
        view->setFit(PageView::Fit::Width);
        shown(view);
        auto* timer = new PaintTimer(view->viewport());
        const QPoint middle = view->viewport()->rect().center();
        for (int i = 0; i < 50; ++i) {
            QNativeGestureEvent e(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, middle, middle,
                                  view->viewport()->mapToGlobal(QPointF(middle)), -0.045, QPointF());
            QCoreApplication::sendEvent(view->viewport(), &e);
            view->viewport()->repaint();
        }
        QList<qint64> t = timer->times;
        std::sort(t.begin(), t.end());
        qInfo("pinch out to %.3f over 80 pages: %lld frames, median %.1f ms, worst %.1f ms, the five worst %.1f %.1f %.1f %.1f %.1f; tiles rendered meanwhile %d",
              view->zoom(), qint64(t.size()), t.at(t.size() / 2) / 1000.0, t.last() / 1000.0, t.at(t.size() - 1) / 1000.0, t.at(t.size() - 2) / 1000.0,
              t.at(t.size() - 3) / 1000.0, t.at(t.size() - 4) / 1000.0, t.at(t.size() - 5) / 1000.0, 0);
        delete timer;
        // From 16 times, the only tiles of the page in sight there: out.
        app->closeAllFiles();
        QVERIFY(app->gotoPage(many));
        doc = pdf();
        view = doc->view();
        QTRY_COMPARE(doc->pageCount(), 80);
        doc->firstPage();
        view->setZoom(PageView::MaxZoom);
        shown(view);
        auto* timer2 = new PaintTimer(view->viewport());
        for (int i = 0; i < 40; ++i) {
            QNativeGestureEvent e(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, middle, middle,
                                  view->viewport()->mapToGlobal(QPointF(middle)), -0.12, QPointF());
            QCoreApplication::sendEvent(view->viewport(), &e);
            view->viewport()->repaint();
        }
        QList<qint64> u = timer2->times;
        std::sort(u.begin(), u.end());
        qInfo("from 16 out to %.3f: %lld frames, median %.1f ms, worst %.1f ms", view->zoom(), qint64(u.size()), u.at(u.size() / 2) / 1000.0, u.last() / 1000.0);
        delete timer2;
    }

    void huntOddPages()
    {
        const QString odd = qEnvironmentVariable("HUNT_ODD");
        if (odd.isEmpty()) QSKIP("probe");
        for (const QString& name : QDir(odd).entryList({"*.pdf"}, QDir::Files, QDir::Name)) {
            const QString file = odd + "/" + name;
            QElapsedTimer t;
            t.start();
            QVERIFY(app->gotoPage(file));
            PdfDoc* doc = pdf();
            QVERIFY(doc != nullptr);
            PageView* view = doc->view();
            QTest::qWait(100);
            const QSizeF size = doc->document()->pagePointSize(0);
            view->setFit(PageView::Fit::Width);
            view->viewport()->repaint();
            view->setFit(PageView::Fit::Page);
            view->viewport()->repaint();
            view->zoomSmoothly(1.25);
            QTest::qWait(400);
            view->setZoom(PageView::MaxZoom);
            view->viewport()->repaint();
            view->setZoom(PageView::MinZoom);
            view->viewport()->repaint();
            view->selectText(0, QPointF(0, 0), QPointF(400, 800));
            view->selectPage(0);
            doc->findAll(QStringLiteral("odd"));
            qInfo("%s: %d pages, the first %g by %g points; content %d by %d; %lld ms", qPrintable(name), doc->pageCount(), size.width(), size.height(),
                  view->horizontalScrollBar()->maximum(), view->verticalScrollBar()->maximum(), t.elapsed());
            app->closeAllFiles();
            QTest::qWait(50);
        }
    }

    void huntGpuZoom()
    {
        if (!qEnvironmentVariableIsSet("HUNT_PROBE")) QSKIP("probe");
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Pdf, true);
        QTRY_VERIFY(view->gpu());
        doc->firstPage();
        view->setFit(PageView::Fit::Width);
        shown(view);
        const QRectF text = doc->document()->getAllText(0).boundingRectangle();
        const QPoint fingers = (QPointF(view->pageRect(0).topLeft()) + (text.topLeft() + QPointF(80, 14)) * view->pixelsPerPoint()).toPoint();
        const auto whiteShare = [view](const QImage& shot) {
            const QRect area = view->pageRect(view->currentPage()).intersected(view->viewport()->rect()).adjusted(8, 8, -8, -8);
            int white = 0, all = 0;
            for (int y = area.top(); y < area.bottom(); y += 2)
                for (int x = area.left(); x < area.right(); x += 2, ++all)
                    if (shot.pixelColor(QPoint(x, y) * shot.devicePixelRatio()).lightness() > 245) ++white;
            return all > 0 ? double(white) / all : 0.0;
        };
        auto* timer = new PaintTimer(view->viewport());
        const int scaled = view->tilesScaled(), rendered = view->tilesRendered();
        for (int i = 0; i < 20; ++i) {
            QNativeGestureEvent e(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, fingers, fingers,
                                  view->viewport()->mapToGlobal(QPointF(fingers)), 0.03, QPointF());
            QCoreApplication::sendEvent(view->viewport(), &e);
            view->viewport()->repaint();
        }
        const QImage mid = view->viewport()->grab().toImage();
        const double during = whiteShare(mid);
        QNativeGestureEvent end(Qt::EndNativeGesture, QPointingDevice::primaryPointingDevice(), 2, fingers, fingers,
                                view->viewport()->mapToGlobal(QPointF(fingers)), 0, QPointF());
        QCoreApplication::sendEvent(view->viewport(), &end);
        const QImage after = shown(view);
        qInfo("GPU: scaled %d, rendered %d during; frames median %.1f worst %.1f ms; white %.4f during, %.4f after", view->tilesScaled() - scaled,
              view->tilesRendered() - rendered, timer->median() / 1000.0, timer->worst / 1000.0, during, whiteShare(after));
        const QString shots = qEnvironmentVariable("HUNT_SHOTS");
        if (!shots.isEmpty()) {
            mid.save(shots + "/gpu_mid.png");
            after.save(shots + "/gpu_after.png");
        }
        delete timer;
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Pdf, false);
        view->setFit(PageView::Fit::Width);
    }

    void huntScatteredText()
    {
        const QString dirOf = qEnvironmentVariable("HUNT_SCATTER");
        if (dirOf.isEmpty()) QSKIP("probe");
        for (const QString& name : QDir(dirOf).entryList({"*.pdf"}, QDir::Files, QDir::Name)) {
            QVERIFY(app->gotoPage(dirOf + "/" + name));
            PdfDoc* doc = pdf();
            PageView* view = doc->view();
            QTRY_VERIFY(doc->pageCount() == 1);
            view->setFit(PageView::Fit::Page);
            shown(view);
            QElapsedTimer t;
            QList<qint64> times;
            int found = 0;
            QRandomGenerator rng(11);
            for (int k = 0; k < 40; ++k) {
                const QPointF a(rng.bounded(612.0), rng.bounded(792.0)), b(rng.bounded(612.0), rng.bounded(792.0));
                t.start();
                if (view->selectText(0, a, b)) ++found;
                times << t.nsecsElapsed() / 1000;
            }
            std::sort(times.begin(), times.end());
            qInfo("%s: 40 selections between random points, %d with text: median %.1f ms, worst %.1f ms", qPrintable(name), found,
                  times.at(times.size() / 2) / 1000.0, times.last() / 1000.0);
            times.clear();
            found = 0;
            for (int k = 0; k < 40; ++k) {
                const QPointF a(rng.bounded(600.0), rng.bounded(780.0));
                t.start();
                if (view->selectText(0, a, a + QPointF(5, 5))) ++found;
                times << t.nsecsElapsed() / 1000;
            }
            std::sort(times.begin(), times.end());
            qInfo("%s: 40 selections 5 points long, %d with text: median %.1f ms, worst %.1f ms; the selection %lld characters", qPrintable(name), found,
                  times.at(times.size() / 2) / 1000.0, times.last() / 1000.0, qint64(view->selectedText().size()));
            app->closeAllFiles();
        }
    }

    void huntReloadMidStep()
    {
        if (!qEnvironmentVariableIsSet("HUNT_PROBE")) QSKIP("probe");
        PdfDoc* doc = pdf();
        PageView* view = doc->view();
        view->setZoom(1.0);
        shown(view);
        view->zoomSmoothly(2.0);
        QTest::qWait(40);
        const qreal shownBefore = view->zoom(), target = view->targetZoom();
        QVERIFY(doc->reload());
        QTest::qWait(400);
        qInfo("a step from 1 to %.3f, reloaded at %.3f: the zoom after %.3f", target, shownBefore, view->zoom());
        view->setFit(PageView::Fit::Width);
    }

