// Throwaway probes of the Create Library dialog (Project > Create Library):
// pasted into TestLibraryPaths's private slots in qucs/tests/
// test_library_paths.cpp, built and run once each, then taken out.

    // d1: "A library with this name already exists! Rewrite?" answered No,
    // another name typed, Next again: how many components the library gets.
    void huntRewriteNoThenRename()
    {
        QucsSettings.QucsWorkDir.setPath(project);
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "proj";
        const QString r = "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n";
        write(project + "/amp.sch", schematicWith("  <Port P1 1 220 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                                                  "  <Port P2 1 280 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n" + r).toUtf8());
        write(userLib + "/Taken.lib", library("Taken", "Old"));
        LibraryDialog dialog(&app);
        dialog.fillSchematicList({"amp.sch"});
        dialog.findChild<QComboBox*>("destination")->setCurrentIndex(0);
        for (QCheckBox* box : dialog.findChildren<QCheckBox*>())
            if (box->text() == "Add subcircuit description") box->setChecked(false);
        auto* name = dialog.findChild<QLineEdit*>();
        name->setText("Taken");
        QTimer no;
        QObject::connect(&no, &QTimer::timeout, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                qInfo() << "box:" << box->text();
                box->button(QMessageBox::No)->click();
            }
        });
        no.start(50);
        QMetaObject::invokeMethod(&dialog, "slotCreateNext");
        no.stop();
        name->setText("Fresh");
        QMetaObject::invokeMethod(&dialog, "slotCreateNext");
        QFile f(userLib + "/Fresh.lib");
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        qInfo() << "Fresh.lib components:" << text.count("\n<Component ") << "(one subcircuit chosen)";
        qInfo() << "Taken.lib still the old one:" << QFile(userLib + "/Taken.lib").size();
        QFile::remove(userLib + "/Fresh.lib");
        QFile::remove(userLib + "/Taken.lib");
        app.ProjName.clear();
    }

    // d2: Rewrite? Yes, and the new library cannot be made (a subcircuit's
    // own subcircuit is missing): what is left of the old one.
    void huntRewriteThatFails()
    {
        QucsSettings.QucsWorkDir.setPath(project);
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "proj";
        write(project + "/outer.sch", schematicWith("  <Port P1 1 220 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                                                    "  <Port P2 1 280 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n"
                                                    "  <Sub SUB1 1 250 100 -26 15 0 0 \"gone.sch\" 0>\n").toUtf8());
        write(userLib + "/Kept.lib", library("Kept", "Old"));
        const qint64 before = QFile(userLib + "/Kept.lib").size();
        LibraryDialog dialog(&app);
        dialog.fillSchematicList({"outer.sch"});
        dialog.findChild<QComboBox*>("destination")->setCurrentIndex(0);
        for (QCheckBox* box : dialog.findChildren<QCheckBox*>())
            if (box->text() == "Add subcircuit description") box->setChecked(false);
        dialog.findChild<QLineEdit*>()->setText("Kept");
        QTimer yes;
        QObject::connect(&yes, &QTimer::timeout, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                qInfo() << "box:" << box->text();
                if (QAbstractButton* yes = box->button(QMessageBox::Yes)) yes->click();
                else box->accept();
            }
        });
        yes.start(50);
        QMetaObject::invokeMethod(&dialog, "slotCreateNext");
        yes.stop();
        for (QPlainTextEdit* e : dialog.findChildren<QPlainTextEdit*>()) qInfo().noquote() << "messages:" << e->toPlainText();
        qInfo() << "Kept.lib was" << before << "bytes; now there:" << QFileInfo::exists(userLib + "/Kept.lib")
                << "in the trash:" << QDir(QString::fromLocal8Bit(qgetenv("QUCS_TRASH_DIR"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        QFile::remove(project + "/outer.sch");
        app.ProjName.clear();
    }

    // d3: a description with <...> in it, through Next and Previous.
    void huntDescriptionThroughPrevious()
    {
        QucsSettings.QucsWorkDir.setPath(project);
        QucsApp app(false);
        MainGuard guard(&app);
        app.ProjName = "proj";
        const QByteArray sub = schematicWith("  <Port P1 1 220 100 -23 12 0 0 \"1\" 1 \"analog\" 0>\n"
                                             "  <Port P2 1 280 100 4 12 1 2 \"2\" 1 \"analog\" 0>\n"
                                             "  <R R1 1 250 100 -26 15 0 0 \"1 kOhm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"european\" 0>\n").toUtf8();
        write(project + "/one.sch", sub);
        write(project + "/two.sch", sub);
        LibraryDialog dialog(&app);
        dialog.fillSchematicList({"one.sch", "two.sch"});
        dialog.findChild<QComboBox*>("destination")->setCurrentIndex(0);
        dialog.findChild<QLineEdit*>()->setText("Described");
        QMetaObject::invokeMethod(&dialog, "slotCreateNext");
        auto* text = dialog.findChild<QTextEdit*>();
        const QString typed = "Amplifier, pins <in> and <out>; gain <b>10</b>";
        text->setPlainText(typed);
        QMetaObject::invokeMethod(&dialog, "slotNextDescr");
        text->setPlainText("second");
        QMetaObject::invokeMethod(&dialog, "slotPrevDescr");
        qInfo() << "typed:" << typed;
        qInfo() << "shown after Previous:" << text->toPlainText();
        QMetaObject::invokeMethod(&dialog, "slotSave");
        QFile f(userLib + "/Described.lib");
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray lib = f.readAll();
        const int at = lib.indexOf("<Description>");
        qInfo() << "written:" << lib.mid(at, lib.indexOf("</Description>", at) - at + 14);
        QFile::remove(userLib + "/Described.lib");
        app.ProjName.clear();
    }
