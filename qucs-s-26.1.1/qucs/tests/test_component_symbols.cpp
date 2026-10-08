/*
 * test_component_symbols.cpp - US or European symbols for new parts
 * (Application Settings > Settings > Component symbols, QucsSettings.
 * ComponentSymbols, symbolstyle.h): US by default - a resistor's zigzag, a
 * diode on a broken line, the logic gates' distinctive shapes - European
 * when set - a box, an unbroken line, DIN 40900 rectangles. A part placed
 * keeps its own. The components panel and its icons, Claude's
 * add_component and the synthesis programs' designs follow the setting.
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QtTest>
#include <QComboBox>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

#include "components/component.h"
#include "components/resistor.h"
#include "config.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "schematic.h"
#include "settings.h"
#include "symbolstyle.h"

using qucs_s::symbols::European;
using qucs_s::symbols::US;

namespace {

// The parts that have both: their model, and their Symbol in US and in
// European.
struct Kind {
    const char* model;
    const char* us;
    const char* european;
};
const Kind kKinds[] = {{"R", "US", "european"},    {"Diode", "US", "normal"},     {"AND", "old", "DIN40900"},
                       {"OR", "old", "DIN40900"},  {"NAND", "old", "DIN40900"},   {"NOR", "old", "DIN40900"},
                       {"XOR", "old", "DIN40900"}, {"XNOR", "old", "DIN40900"},   {"Buf", "old", "DIN40900"},
                       {"Inv", "old", "DIN40900"}};

QString symbolOf(const Component* c)
{
    for (const Property* p : c->Props)
        if (p->Name == QLatin1String("Symbol")) return p->Value;
    return QStringLiteral("(none)");
}

// What it draws: its lines', arcs' and ellipses' places.
QString drawing(const Component* c)
{
    QStringList out;
    for (const qucs::Line* l : c->Lines) out << QStringLiteral("L%1,%2,%3,%4").arg(l->x1).arg(l->y1).arg(l->x2).arg(l->y2);
    for (const qucs::Arc* a : c->Arcs) out << QStringLiteral("A%1,%2,%3,%4").arg(a->x).arg(a->y).arg(a->w).arg(a->h);
    for (const qucs::Ellips* e : c->Ellipses) out << QStringLiteral("E%1,%2").arg(e->x).arg(e->y);
    return out.join(QLatin1Char(' '));
}

// The model's part with its Symbol set to \a symbol, drawn anew.
std::unique_ptr<Component> drawnAs(const char* model, const QString& symbol)
{
    std::unique_ptr<Component> c(Module::getComponent(QString::fromLatin1(model)));
    for (Property* p : c->Props)
        if (p->Name == QLatin1String("Symbol")) p->Value = symbol;
    c->recreate();
    return c;
}

QImage iconOf(Component* c)
{
    QPixmap p(128, 128);
    c->paintIcon(&p);
    return p.toImage();
}

struct StyleGuard {
    ~StyleGuard()
    {
        QucsSettings.ComponentSymbols = US;
        qucs_s::symbols::tellTools(US);
    }
};

} // namespace

class TestComponentSymbols : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;
    QucsControl* control = nullptr;

    QJsonObject call(const QString& tool, const QJsonObject& args = {}, int timeoutMs = 60000)
    {
        return control->callNow(tool, args, timeoutMs);
    }
    static bool failed(const QJsonObject& r) { return r.value("isError").toBool(); }
    static QString text(const QJsonObject& r) { return QucsControl::textOf(r); }
    static QJsonObject json(const QJsonObject& r)
    {
        for (const QJsonValue& v : r.value(QStringLiteral("content")).toArray())
            if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text"))
                return QJsonDocument::fromJson(v.toObject().value(QStringLiteral("text")).toString().toUtf8()).object();
        return {};
    }
    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }
    // The Symbol of each resistor a schematic file has.
    static QStringList resistorsIn(const QString& file)
    {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) return {QStringLiteral("(no file)")};
        static const QRegularExpression resistor(QStringLiteral("^\\s*<R [^\\n]*\"([^\"]*)\"\\s+\\d+>\\s*$"),
                                                 QRegularExpression::MultilineOption);
        QStringList out;
        for (auto it = resistor.globalMatch(QString::fromUtf8(f.readAll())); it.hasNext();) out << it.next().captured(1);
        return out;
    }
    // The texts of the components panel's entries, in the category of the
    // lumped components.
    QStringList lumpedEntries()
    {
        const int index = Category::getCategories().indexOf(QObject::tr("lumped components"));
        if (index < 0 || !QMetaObject::invokeMethod(app, "slotSetCompView", Q_ARG(int, index))) return {};
        QStringList out;
        for (int i = 0; i < app->CompComps->count(); ++i) out << app->CompComps->item(i)->text();
        return out;
    }
    // The settings this test runs with - again after the store is read.
    void configure()
    {
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsSettings.font = QApplication::font();
        QucsSettings.appFont = QApplication::font();
        QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }
    bool reload()
    {
        const bool read = loadSettings();
        configure();
        return read;
    }
    void closeAll()
    {
        for (QucsDoc* doc : app->allDocuments()) {
            if (auto* sch = dynamic_cast<Schematic*>(doc)) sch->setChanged(false);
            doc->setDocChanged(false);
        }
        app->closeAllFiles();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
        QVERIFY(reload());
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    void cleanupTestCase()
    {
        if (app != nullptr) {
            closeAll();
            delete app;
        }
        QucsMain = nullptr;
    }

    // US unless set: the setting's default, what a fresh store reads, and
    // what the programs Qucs-S starts are told.
    void usIsTheDefault()
    {
        QCOMPARE(_settings::Get().itemDefault<int>("ComponentSymbols"), int(US));
        QCOMPARE(tQucsSettings().ComponentSymbols, int(US));
        QCOMPARE(QucsSettings.ComponentSymbols, int(US));   // (read from a store without it)
        QCOMPARE(qgetenv(qucs_s::symbols::environmentVariable()), QByteArray("US"));
        // A store's value out of range: the nearest there is.
        _settings::Get().setItem<int>("ComponentSymbols", 7);
        QVERIFY(reload());
        QCOMPARE(QucsSettings.ComponentSymbols, int(European));
        _settings::Get().setItem<int>("ComponentSymbols", -3);
        QVERIFY(reload());
        QCOMPARE(QucsSettings.ComponentSymbols, int(US));
        _settings::Get().setItem<int>("ComponentSymbols", int(US));
        QVERIFY(reload());
        QCOMPARE(QucsSettings.ComponentSymbols, int(US));
    }

    // Each part that has both, made new: its Symbol and its drawing as the
    // setting says - and the two drawings differ.
    void newPartsAreDrawnAsSet_data()
    {
        QTest::addColumn<int>("style");
        QTest::newRow("US") << int(US);
        QTest::newRow("European") << int(European);
    }
    void newPartsAreDrawnAsSet()
    {
        QFETCH(int, style);
        StyleGuard guard;
        QucsSettings.ComponentSymbols = style;
        for (const Kind& k : kKinds) {
            const QString want = QString::fromLatin1(style == European ? k.european : k.us);
            std::unique_ptr<Component> c(Module::getComponent(QString::fromLatin1(k.model)));
            QVERIFY2(c != nullptr, k.model);
            QCOMPARE(symbolOf(c.get()), want);
            QCOMPARE(drawing(c.get()), drawing(drawnAs(k.model, want).get()));
            QVERIFY2(drawing(drawnAs(k.model, QString::fromLatin1(k.us)).get())
                         != drawing(drawnAs(k.model, QString::fromLatin1(k.european)).get()),
                     k.model);
        }
        QCOMPARE(symbolOf(std::make_unique<Resistor>().get()), QString(style == European ? "european" : "US"));
    }

    // A part placed keeps its own, whatever the setting: loaded (a file, a
    // paste, an undo), and placed again after it (newOne).
    void aPartKeepsItsOwnSymbol()
    {
        StyleGuard guard;
        for (const Kind& k : kKinds)
            for (const char* own : {k.us, k.european})
                for (int style : {int(US), int(European)}) {
                    QucsSettings.ComponentSymbols = style;
                    QString line = drawnAs(k.model, QString::fromLatin1(own))->save();
                    QucsSettings.ComponentSymbols = style == US ? European : US;
                    std::unique_ptr<Component> c(getComponentFromName(line));
                    QVERIFY2(c != nullptr, qPrintable(line));
                    QCOMPARE(symbolOf(c.get()), QString::fromLatin1(own));
                    QCOMPARE(drawing(c.get()), drawing(drawnAs(k.model, QString::fromLatin1(own)).get()));
                    if (QLatin1String(k.model) != QLatin1String("Diode")) {   // (a diode placed again is a new one)
                        std::unique_ptr<Component> again(c->newOne());
                        QCOMPARE(symbolOf(again.get()), QString::fromLatin1(own));
                    }
                }
        // A diode's other symbols are left as they are.
        QucsSettings.ComponentSymbols = European;
        QString zener = drawnAs("Diode", "Zener")->save();
        QucsSettings.ComponentSymbols = US;
        QCOMPARE(symbolOf(std::unique_ptr<Component>(getComponentFromName(zener)).get()), QString("Zener"));
        // The old resistor of the other symbol, <Rus>: US.
        QucsSettings.ComponentSymbols = European;
        QString rus = QStringLiteral("<Rus R1 1 100 100 -26 15 0 0 \"1k\" 1 \"26.85\" 0 \"US\" 0>");
        QCOMPARE(symbolOf(std::unique_ptr<Component>(getComponentFromName(rus)).get()), QString("US"));
    }

    // The panel's resistors: "Resistor" as set, and the other one beside it
    // - "Resistor European" when US is set, "Resistor US" when European. Its
    // icons are drawn as the parts are.
    void thePanelHasBothResistors()
    {
        StyleGuard guard;
        for (int style : {int(US), int(European)}) {
            QucsSettings.ComponentSymbols = style;
            QString name;
            char* file = nullptr;
            std::unique_ptr<Element> first(Resistor::info(name, file, true));
            QCOMPARE(name, QString("Resistor"));
            QCOMPARE(QByteArray(file), QByteArray(style == US ? "resistor_us" : "resistor"));
            QCOMPARE(symbolOf(dynamic_cast<Component*>(first.get())), QString(style == US ? "US" : "european"));
            std::unique_ptr<Element> other(Resistor::info_other(name, file, true));
            QCOMPARE(name, QString(style == US ? "Resistor European" : "Resistor US"));
            QCOMPARE(symbolOf(dynamic_cast<Component*>(other.get())), QString(style == US ? "european" : "US"));

            Module::registerModules();
            Module* r = Module::Modules.value("R");
            QVERIFY(r != nullptr && r->icon != nullptr);
            Resistor drawn(style == European);
            QCOMPARE(r->icon->toImage(), iconOf(&drawn));
            Resistor otherDrawn(style != European);
            QVERIFY(r->icon->toImage() != iconOf(&otherDrawn));
            QStringList names;
            for (Module* m : Category::getModules(QObject::tr("lumped components"))) {
                QString n;
                char* f = nullptr;
                std::unique_ptr<Element> e(m->info(n, f, true));
                names << n;
            }
            QVERIFY2(names.contains("Resistor") && names.contains(style == US ? "Resistor European" : "Resistor US")
                         && !names.contains(style == US ? "Resistor US" : "Resistor European"),
                     qPrintable(names.join(", ")));
        }
        Module::registerModules();
    }

    // What a synthesis program's design gives its resistors: \a style, by
    // the Symbol of each <R> line (the rest left alone); what the program
    // reads, from Qucs-S or else the settings.
    void aDesignsResistorsAreStyled()
    {
        using qucs_s::symbols::styled;
        const QString design = QStringLiteral("<Components>\n"
                                              "  <R R1 1 180 200 -15 60 0 1 \"96.2 Ohm\" 1 \"26.85\" 0 \"0.0\" 0 \"0.0\" 0 \"26.85\" 0 \"US\" 0>\n"
                                              "  <R R2 1 200 340 15 -26 0 1 \"4.7k\" 1 \"26.85\" 0 \"european\" 0>\r\n"
                                              "  <L L1 1 300 -30 15 -26 0 -1 \"4n\" 1 \"26.85\" 0 \"US\" 0>\n"
                                              "  <Diode D1 1 100 100 15 -26 0 1 \"1e-15 A\" 1 \"normal\" 0>\n"
                                              "  <OpAmp OP1 1 400 200 -26 42 0 0 \"1e6\" 1 \"15 V\" 0>\n"
                                              "</Components>\n<Paintings>\n  <Text 50 122 10 #000000 0 \"R european US\">\n</Paintings>\n");
        const QString european = styled(design, European);
        QVERIFY(european.contains("\"0.0\" 0 \"26.85\" 0 \"european\" 0>\n  <R R2"));
        QVERIFY(european.contains("\"26.85\" 0 \"european\" 0>\r\n  <L L1"));
        QCOMPARE(european.count("\"european\" 0>"), 2);
        QCOMPARE(european.count("<L L1 1 300 -30 15 -26 0 -1 \"4n\" 1 \"26.85\" 0 \"US\" 0>"), 1);   // (no resistor)
        QVERIFY(european.contains("\"R european US\""));
        const QString us = styled(design, US);
        QCOMPARE(us.count("\"european\""), 0);
        QCOMPARE(us.count("0 \"US\" 0>"), 3);   // the resistors and the inductor
        QCOMPARE(styled(us, European), european);
        QCOMPARE(styled(european, US), us);
        QCOMPARE(qucs_s::symbols::styleNamed("European", US), European);
        QCOMPARE(qucs_s::symbols::styleNamed(" us ", European), US);
        QCOMPARE(qucs_s::symbols::styleNamed("1", US), European);
        QCOMPARE(qucs_s::symbols::styleNamed("Martian", European), European);

        StyleGuard guard;
        qucs_s::symbols::tellTools(European);
        QCOMPARE(qucs_s::symbols::forTools(), European);
        qucs_s::symbols::tellTools(US);
        QCOMPARE(qucs_s::symbols::forTools(), US);
    }

    // The choice in the dialog: applied - new parts, the panel and its
    // icons, the programs started from now on - kept for the next start,
    // and US again with Default Values. Claude reads and sets it by its
    // label.
    void theSettingsDialogHasTheChoice()
    {
        StyleGuard guard;
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        control = app->findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        QVERIFY2(lumpedEntries().contains("Resistor European"), qPrintable(lumpedEntries().join(", ")));
        {
            QucsSettingsDialog dlg(app);
            auto* combo = dlg.findChild<QComboBox*>("componentSymbolsCombo");
            QVERIFY(combo != nullptr);
            QCOMPARE(combo->currentText(), QString("US (ANSI/IEEE)"));
            QVERIFY(combo->toolTip().contains("zigzag") && combo->toolTip().contains("keeps its own"));
            combo->setCurrentIndex(combo->findData(int(European)));
            QCOMPARE(combo->currentText(), QString("European (IEC)"));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        }
        QCOMPARE(QucsSettings.ComponentSymbols, int(European));
        QCOMPARE(_settings::Get().item<int>("ComponentSymbols"), int(European));
        QCOMPARE(qgetenv(qucs_s::symbols::environmentVariable()), QByteArray("European"));
        const QStringList entries = lumpedEntries();
        QVERIFY2(entries.contains("Resistor") && entries.contains("Resistor US") && !entries.contains("Resistor European"),
                 qPrintable(entries.join(", ")));
        Resistor box(true);
        QCOMPARE(Module::Modules.value("R")->icon->toImage(), iconOf(&box));
        // Kept for the next start.
        QucsSettings.ComponentSymbols = US;
        QVERIFY(reload());
        QCOMPARE(QucsSettings.ComponentSymbols, int(European));
        {
            QucsSettingsDialog dlg(app);
            auto* combo = dlg.findChild<QComboBox*>("componentSymbolsCombo");
            QCOMPARE(combo->currentText(), QString("European (IEC)"));   // as set
            for (QPushButton* b : dlg.findChildren<QPushButton*>())
                if (b->text() == "Default Values") b->click();
            QCOMPARE(combo->currentText(), QString("US (ANSI/IEEE)"));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        }
        QCOMPARE(QucsSettings.ComponentSymbols, int(US));
        QVERIFY2(lumpedEntries().contains("Resistor European"), qPrintable(lumpedEntries().join(", ")));

        // Claude's: by its label, with its choices.
        const QString key = QStringLiteral("Settings/Component symbols");
        QJsonObject r = call("get_settings", {{"scope", "app"}, {"keys", QJsonArray{key}}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        const QJsonObject setting = json(r).value("settings").toArray().first().toObject();
        QCOMPARE(setting.value("key").toString(), key);
        QCOMPARE(setting.value("value").toString(), QString("US (ANSI/IEEE)"));
        QCOMPARE(setting.value("choices").toArray(), (QJsonArray{"US (ANSI/IEEE)", "European (IEC)"}));
        r = call("set_settings", {{"scope", "app"}, {"values", QJsonObject{{key, "European (IEC)"}}}}, 20000);
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(QucsSettings.ComponentSymbols, int(European));
        QVERIFY(lumpedEntries().contains("Resistor US"));
        r = call("set_settings", {{"scope", "app"}, {"values", QJsonObject{{key, "US (ANSI/IEEE)"}}}}, 20000);
        QVERIFY2(!failed(r) && QucsSettings.ComponentSymbols == US, qPrintable(text(r)));
    }

    // Settings imported (File > Import Settings): the panel's parts and
    // icons as they say.
    void importedSettingsRedrawThePanel()
    {
        StyleGuard guard;
        QVERIFY(app != nullptr);
        QucsSettings.ComponentSymbols = European;
        QString error;
        QVERIFY2(app->exportSettingsTo(dir.filePath("european.json"), &error), qPrintable(error));
        QucsSettings.ComponentSymbols = US;
        app->refreshComponentsPanel();
        QVERIFY(lumpedEntries().contains("Resistor European"));
        QString report;
        QVERIFY2(app->importSettingsFrom(dir.filePath("european.json"), false, &report), qPrintable(report));
        configure();
        QCOMPARE(QucsSettings.ComponentSymbols, int(European));
        QCOMPARE(qgetenv(qucs_s::symbols::environmentVariable()), QByteArray("European"));
        QVERIFY2(lumpedEntries().contains("Resistor US"), qPrintable(lumpedEntries().join(", ")));
        Resistor box(true);
        QCOMPARE(Module::Modules.value("R")->icon->toImage(), iconOf(&box));
        _settings::Get().setItem<int>("ComponentSymbols", int(US));
        QucsSettings.ComponentSymbols = US;
        app->refreshComponentsPanel();
    }

    // Set while a symbol is edited: its panel keeps the paintings, and the
    // schematic's, when it is back, has the parts as now set.
    void whileASymbolIsEditedItsPanelStays()
    {
        StyleGuard guard;
        QVERIFY(app != nullptr);
        app->slotFileNew();
        Schematic* sch = app->currentSchematic();
        QVERIFY(sch != nullptr);
        app->slotSymbolEdit();
        QVERIFY(sch->getSymbolMode());
        const auto entries = [this] {
            QStringList out;
            for (int i = 0; i < app->CompComps->count(); ++i) out << app->CompComps->item(i)->text();
            return out;
        };
        const QStringList paintings = entries();
        QVERIFY2(!paintings.isEmpty() && !paintings.contains("Resistor"), qPrintable(paintings.join(", ")));
        // The panel's list of categories: the paintings alone.
        QComboBox* categories = nullptr;
        for (QComboBox* c : app->findChildren<QComboBox*>())
            if (c->count() == 1 && c->itemText(0) == QObject::tr("paintings")) categories = c;
        QVERIFY(categories != nullptr);
        {
            QucsSettingsDialog dlg(app);
            auto* combo = dlg.findChild<QComboBox*>("componentSymbolsCombo");
            combo->setCurrentIndex(combo->findData(int(European)));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        }
        QCOMPARE(QucsSettings.ComponentSymbols, int(European));
        QCOMPARE(entries(), paintings);
        QCOMPARE(categories->count(), 1);
        app->slotSymbolEdit();
        QVERIFY(!sch->getSymbolMode());
        QVERIFY2(lumpedEntries().contains("Resistor US"), qPrintable(lumpedEntries().join(", ")));
        Resistor box(true);
        QCOMPARE(Module::Modules.value("R")->icon->toImage(), iconOf(&box));
        {
            QucsSettingsDialog dlg(app);
            auto* combo = dlg.findChild<QComboBox*>("componentSymbolsCombo");
            combo->setCurrentIndex(combo->findData(int(US)));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        }
        closeAll();
    }

    // Claude's parts and designs: a resistor added, an attenuator's, an
    // active filter's, a power combiner's isolation resistor and a match's
    // load (a program each, and the matching dialog in the window) - as
    // set.
    void claudesPartsAndDesignsAreDrawnAsSet_data()
    {
        QTest::addColumn<int>("style");
        QTest::newRow("US") << int(US);
        QTest::newRow("European") << int(European);
    }
    void claudesPartsAndDesignsAreDrawnAsSet()
    {
        QFETCH(int, style);
        QVERIFY(control != nullptr);
        StyleGuard guard;
        QJsonObject r = call("set_settings", {{"scope", "app"},
                                              {"values", QJsonObject{{"Settings/Component symbols", style == US ? "US (ANSI/IEEE)" : "European (IEC)"}}}},
                             20000);
        QVERIFY2(!failed(r) && QucsSettings.ComponentSymbols == style, qPrintable(text(r)));
        const QString want = style == US ? QStringLiteral("US") : QStringLiteral("european");
        const QString tag = QTest::currentDataTag();

        QFile blank(path("parts_" + tag + ".sch"));
        QVERIFY(blank.open(QIODevice::WriteOnly));
        blank.write("<Qucs Schematic " PACKAGE_VERSION ">\n<Components>\n</Components>\n<Wires>\n</Wires>\n");
        blank.close();
        r = call("open_document", {{"path", blank.fileName()}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("add_component", {{"path", blank.fileName()}, {"type", "R"}, {"x", 200}, {"y", 200}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("add_component", {{"path", blank.fileName()}, {"type", "Diode"}, {"x", 400}, {"y", 200}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        r = call("add_component", {{"path", blank.fileName()}, {"type", "AND"}, {"x", 600}, {"y", 200}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        Schematic* sch = app->currentSchematic();
        QVERIFY(sch != nullptr);
        QStringList symbols;
        for (const Component* c : *sch->a_Components) symbols << c->Model + "=" + symbolOf(c);
        QCOMPARE(symbols, (QStringList{"R=" + want, QString("Diode=") + (style == US ? "US" : "normal"),
                                       QString("AND=") + (style == US ? "old" : "DIN40900")}));
        closeAll();

        r = call("synthesize_attenuator", {{"topology", "pi"}, {"attenuation", 10}, {"save_as", path("pad_" + tag + ".sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(resistorsIn(path("pad_" + tag + ".sch")), QStringList(3, want));
        r = call("synthesize_filter", {{"kind", "active"}, {"response", "butterworth"}, {"order", 2}, {"fc", 1000}, {"save_as", path("active_" + tag + ".sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QStringList filter = resistorsIn(path("active_" + tag + ".sch"));
        QVERIFY2(filter.size() >= 2 && filter == QStringList(filter.size(), want), qPrintable(filter.join(", ")));
        r = call("synthesize_power_combiner", {{"f", 1e9}, {"save_as", path("wilkinson_" + tag + ".sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(resistorsIn(path("wilkinson_" + tag + ".sch")), QStringList{want});
        r = call("synthesize_matching", {{"z_load", "10-j20"}, {"z_source", 50}, {"f", "900 MHz"}, {"save_as", path("match_" + tag + ".sch")}});
        QVERIFY2(!failed(r), qPrintable(text(r)));
        QCOMPARE(resistorsIn(path("match_" + tag + ".sch")), QStringList{want});
        closeAll();
    }
};

QTEST_MAIN(TestComponentSymbols)
#include "test_component_symbols.moc"
