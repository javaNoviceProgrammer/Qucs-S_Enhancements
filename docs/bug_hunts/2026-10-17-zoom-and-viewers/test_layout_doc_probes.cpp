// Probe functions of the 2026-10-17 hunt, pasted into the private slots of test_layout_doc.cpp
// (each skips unless its environment variable is set; see the report).

    void huntReloadWhileHidden()
    {
        if (!qEnvironmentVariableIsSet("HUNT_PROBE")) QSKIP("probe");
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Layout, true);
        struct Back {
            ~Back() { QucsSettingsFile().remove(qucs_s::gpu::settingKey(qucs_s::gpu::Viewer::Layout)); }
        } back;
        writeSample(path("aba.gds"), false);
        LayoutDoc* doc = open(path("aba.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        QVERIFY(view->canvas() != nullptr);
        QTRY_VERIFY(view->canvas()->builds() >= 1);
        const auto reload = [&](bool extra) {
            QSignalSpy reloaded(doc, &LayoutDoc::reloaded);
            QTest::qWait(1100);
            writeSample(path("aba.gds"), false, 0, extra);
            return reloaded.wait(20000);
        };
        const QRectF region(-2, -5, 50, 30);
        int tries = 0, hit = 0;
        for (; tries < 6 && !hit; ++tries) {
            // In front: the one with the extra square, drawn.
            app->DocumentTab->setCurrentWidget(doc);
            QVERIFY(reload(true));
            view->showRegion(region);
            int frames = view->canvas()->frames();
            QTRY_VERIFY(view->canvas()->frames() > frames);
            const Layout* painted = doc->layout().get();
            const int builds = view->canvas()->builds();
            // Behind another tab: read again twice, without the square.
            LayoutDoc* other = open(path("sample.oas"));
            QVERIFY(other != nullptr && other != doc);
            QVERIFY(reload(false));
            QVERIFY(reload(false));
            const bool same = doc->layout().get() == painted;
            app->DocumentTab->setCurrentWidget(doc);
            view->showRegion(region);
            frames = view->canvas()->frames();
            QTRY_VERIFY(view->canvas()->frames() > frames);
            qInfo("try %d: the last read at the painted one's address %d; builds %d then %d", tries, int(same), builds, view->canvas()->builds());
            if (same) {
                hit = 1;
                const QString shots = qEnvironmentVariable("HUNT_SHOTS");
                if (!shots.isEmpty()) view->picture().save(shots + "/aba_gpu.png");
                qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Layout, false);
                QTest::qWait(400);
                view->showRegion(region);
                QTest::qWait(400);
                if (!shots.isEmpty()) view->picture().save(shots + "/aba_cpu.png");
            }
        }
        closeAll();
    }

    void huntOverBudget()
    {
        if (!qEnvironmentVariableIsSet("HUNT_PROBE")) QSKIP("probe");
        qputenv("QUCS_LAYOUT_GPU_BUDGET", "5");
        qucs_s::gpu::setWanted(qucs_s::gpu::Viewer::Layout, true);
        struct Back {
            ~Back() {
                QucsSettingsFile().remove(qucs_s::gpu::settingKey(qucs_s::gpu::Viewer::Layout));
                qunsetenv("QUCS_LAYOUT_GPU_BUDGET");
            }
        } back;
        LayoutDoc* doc = open(path("sample.gds"));
        QVERIFY(doc != nullptr);
        LayoutView* view = doc->view();
        QVERIFY(view->canvas() != nullptr);
        QTRY_VERIFY(view->canvas()->frames() > 0);
        const int builds = view->canvas()->builds(), frames = view->canvas()->frames();
        for (int i = 0; i < 10; ++i) {
            view->setView(view->center() + QPointF(0.5, 0), view->scale());
            QTest::qWait(60);
        }
        qInfo("over the budget: %d frames, %d builds over them (the CPU drawing %d times)", view->canvas()->frames() - frames,
              view->canvas()->builds() - builds, view->renders());
        closeAll();
    }

