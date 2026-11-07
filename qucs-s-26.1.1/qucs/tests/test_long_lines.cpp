/*
 * test_long_lines.cpp - a long line in a label of the window
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

// A label of one line asks for the width of its whole text, and its layout
// passes that on to the window. A tab's status of a few hundred characters
// made the main window wider than the screen, the Claude Code dock pushed
// off its edge (2 November). The labels whose text the program writes give
// way now (ElidedLabel): cut with an ellipsis, whole in their tool tips.
// Each document kind is opened here in the main window, the Claude Code
// dock beside it.

#include <QtTest>
#include <QDockWidget>
#include <QJsonDocument>
#include <QScrollBar>
#include <QTextBrowser>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QStyle>
#include <QTemporaryDir>
#include <QTextDocumentFragment>
#include <QToolButton>
#include <QToolTip>

#include "config.h"
#include "apptheme.h"
#include "claudecode.h"
#include "claudecodepanel.h"
#include "elidedlabel.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "layoutdoc.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "multiphysicsdoc.h"
#include "qucs.h"
#include "qucsdoc.h"
#include "rf25doc.h"
#include "rf25/rf25_model.h"

namespace rf = qucs_s::rf25;

namespace {

QJsonArray rowOf(const QStringList& cells)
{
    QJsonArray out;
    for (const QString& c : cells) out.append(c);
    return out;
}

/// Where a widget is in the window: its own name, else its class, up to
/// the window.
QString whereIs(const QWidget* w)
{
    QStringList path;
    for (const QObject* o = w; o != nullptr && !o->inherits("QMainWindow"); o = o->parent())
        path.prepend(o->objectName().isEmpty() ? QString::fromLatin1(o->metaObject()->className()) : o->objectName());
    return path.join(QLatin1Char('/'));
}

/// The tool tip a widget shows where the pointer rests on it, as plain text
/// (none: empty).
QString tipOf(QWidget* w)
{
    // A tip hidden fades for 300 ms, its text still told.
    QToolTip::hideText();
    for (int i = 0; i < 100 && QToolTip::isVisible(); ++i) QTest::qWait(20);
    const QPoint at = w->rect().center();
    QHelpEvent help(QEvent::ToolTip, at, w->mapToGlobal(at));
    QApplication::sendEvent(w, &help);
    return QToolTip::isVisible() ? QTextDocumentFragment::fromHtml(QToolTip::text()).toPlainText().simplified() : QString();
}

} // namespace

static int pictureTheme()
{
    return qEnvironmentVariable("QUCS_CLAUDE_PICTURES_THEME") == QStringLiteral("light") ? qucs_s::apptheme::Light : qucs_s::apptheme::Dark;
}

class TestLongLines : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QucsApp* app = nullptr;

    QString path(const QString& name) const { return dir.filePath("workspace/" + name); }

    static void settle()
    {
        for (int i = 0; i < 3; ++i) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            QCoreApplication::processEvents();
        }
        // The main window's layout, which sizes the docks, run as its layout
        // request would run it: a dock is held to its least width only then.
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w->isVisible() && w->layout() != nullptr) w->layout()->activate();
        QCoreApplication::processEvents();
    }

    /// The window as it was opened, its Claude Code dock shown.
    void reset()
    {
        app->resize(1300, 900);
        settle();
    }

    /// The tab in front, with nothing left running that would write its
    /// labels: a layout read, a model's layout and mesh, a geometry built.
    bool quiet()
    {
        QWidget* front = app->DocumentTab->currentWidget();
        QDeadlineTimer deadline(120000);
        auto busy = [front] {
            if (auto* rf25 = dynamic_cast<RF25Doc*>(front)) return rf25->isBusy();
            if (auto* layout = dynamic_cast<LayoutDoc*>(front)) return layout->isLoading();
            if (auto* mp = dynamic_cast<MultiphysicsDoc*>(front)) return mp->building() || mp->isBusy();
            return false;
        };
        while (busy() && !deadline.hasExpired()) QTest::qWait(50);
        QTest::qWait(200);
        settle();
        return !busy();
    }

    /// An example copied into the workspace, writable.
    QString copied(const QString& example, const QString& name)
    {
        const QString to = path(name);
        QFile::remove(to);
        if (!QFile::copy(QStringLiteral(QUCS_EXAMPLES_DIR "/") + example, to)) return {};
        QFile(to).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return to;
    }

    /// A model of one metal over a substrate on the multiphysics examples'
    /// layout, a port on it.
    QString model()
    {
        if (copied(QStringLiteral("multiphysics/thermo_optic_heater.gds"), QStringLiteral("heater.gds")).isEmpty())
            return {};
        rf::Model m = rf::Model::blank();
        m.mesh().props.insert(QStringLiteral("cells_across"), 0);
        rf::Node port = m.make(QStringLiteral("rf_edge_port"));
        port.props.insert(QStringLiteral("conductor"), QStringLiteral("M1"));
        if (m.add(QStringLiteral("ports"), port) == nullptr) return {};
        m.layout().props.insert(QStringLiteral("file"), QStringLiteral("heater.gds"));
        QJsonArray rows;
        rows.append(rowOf({QStringLiteral("31"), QStringLiteral("0"), QStringLiteral("M1")}));
        m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), rows);
        const QString file = path(QStringLiteral("heater.qrf25"));
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(m.toJson());
        return file;
    }

    /// One file of each kind a tab holds.
    QList<QPair<QString, QString>> kinds()
    {
        QList<QPair<QString, QString>> out;
        out << qMakePair(QStringLiteral("RF 2.5D"), model());
        out << qMakePair(QStringLiteral("layout"), path(QStringLiteral("heater.gds")));
        out << qMakePair(QStringLiteral("multiphysics"),
                         copied(QStringLiteral("multiphysics/ge_photodiode.qfem"), QStringLiteral("photodiode.qfem")));
        out << qMakePair(QStringLiteral("schematic"),
                         copied(QStringLiteral("ngspice/General Electronics/RC_filter_FFT.sch"), QStringLiteral("rc.sch")));
        const QString text = path(QStringLiteral("notes.txt"));
        QFile t(text);
        if (t.open(QIODevice::WriteOnly)) t.write("A line of text.\n");
        t.close();
        out << qMakePair(QStringLiteral("text"), text);
        const QString sheet = path(QStringLiteral("table.csv"));
        QFile s(sheet);
        if (s.open(QIODevice::WriteOnly)) s.write("f,S11\n1e9,-12\n2e9,-15\n");
        s.close();
        out << qMakePair(QStringLiteral("sheet"), sheet);
        const QString image = path(QStringLiteral("picture.png"));
        QImage picture(64, 48, QImage::Format_ARGB32);
        picture.fill(Qt::darkCyan);
        picture.save(image);
        out << qMakePair(QStringLiteral("image"), image);
        const QString pdf = path(QStringLiteral("page.pdf"));
        {
            QPdfWriter writer(pdf);
            QPainter painter(&writer);
            painter.drawText(QPointF(100, 100), QStringLiteral("A page."));
        }
        out << qMakePair(QStringLiteral("PDF"), pdf);
        const QString xml = path(QStringLiteral("model.xml"));
        QFile x(xml);
        if (x.open(QIODevice::WriteOnly)) x.write("<model><node/></model>\n");
        x.close();
        out << qMakePair(QStringLiteral("XML"), xml);
        // An archive of nothing: its end of central directory alone.
        const QString zip = path(QStringLiteral("empty.zip"));
        QFile z(zip);
        if (z.open(QIODevice::WriteOnly)) z.write(QByteArray("PK\x05\x06", 4) + QByteArray(18, '\0'));
        z.close();
        out << qMakePair(QStringLiteral("zip"), zip);
        return out;
    }

    /// Where a label's long text first reaches a minimum width: the label's
    /// own sizes, then the first ancestor whose least width took it.
    static QString carrier(const QLabel* l)
    {
        auto policy = [](const QWidget* w) { return int(w->sizePolicy().horizontalPolicy()); };
        QString out = QStringLiteral("label least %1, hint %2, policy %3")
                          .arg(l->minimumSizeHint().width()).arg(l->sizeHint().width()).arg(policy(l));
        for (const QWidget* w = l->parentWidget(); w != nullptr && !w->isWindow(); w = w->parentWidget())
            if (w->minimumSizeHint().width() > 3000 || w->minimumWidth() > 3000) {
                out += QStringLiteral("; first wide: %1 (%2) least %3, minimum %4, policy %5")
                           .arg(w->objectName(), QString::fromLatin1(w->metaObject()->className()))
                           .arg(w->minimumSizeHint().width()).arg(w->minimumWidth()).arg(policy(w));
                break;
            }
        return out;
    }

    struct Widening
    {
        QString where, text;
        int grew = 0;
    };

    /// Each label of one line in the window given a long text in turn: those
    /// that made the window wider than it was, or asked it to be. \a probed
    /// counts the labels tried.
    QList<Widening> widening(int& probed)
    {
        const QString longText = QStringLiteral("a long line of text ").repeated(100);
        QList<Widening> out;
        for (QLabel* l : app->findChildren<QLabel*>()) {
            if (!l->isVisible() || l->window() != app || l->wordWrap() || !l->pixmap().isNull() || l->movie() != nullptr)
                continue;
            // The window as it was, measured afresh: a label's text put back
            // shrinks the window's least width only once its layout has run.
            reset();
            const int width = app->width(), least = app->minimumSizeHint().width();
            const QString was = l->text();
            ++probed;
            l->setText(longText);
            settle();
            const int grew = std::max(app->width() - width, app->minimumSizeHint().width() - least);
            if (grew > 4) out.append({whereIs(l), was.left(70) + QStringLiteral(" | ") + carrier(l), grew});
            l->setText(was);
            settle();
        }
        reset();
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QucsSettings.tempFilesDir.setPath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("s4q"));
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsSettings.ExamplesDir = QStringLiteral(QUCS_EXAMPLES_DIR "/");
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        QucsSettings.font = QApplication::font();
        QucsSettings.appFont = QApplication::font();
        QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        Module::registerModules();
        // (The pictures' theme, set as the app sets it, before its window.)
        if (qEnvironmentVariableIsSet("QUCS_CLAUDE_PICTURES")) qucs_s::apptheme::apply(pictureTheme());
        app = new QucsApp(false);
        QucsMain = app;
        app->resize(1300, 900);
        app->show();
        app->claudeDockWidget()->show();
        settle();
        // The dock keeps the size it has, as restoreState leaves the docks at
        // a start (the panels restored, the default): a dock never sized
        // follows its contents' preferred widths - Qt's first run.
        app->resizeDocks({app->claudeDockWidget()}, {app->claudeDockWidget()->width()}, Qt::Horizontal);
        settle();
    }

    void cleanupTestCase()
    {
        app->closeAllFiles();
        delete app;
        QucsMain = nullptr;
    }

    /// The label alone, beside a button: it asks the width of a few
    /// characters; a line too long for it is drawn cut with an ellipsis and
    /// whole in its tool tip, before the tool tip of its own; text() whole;
    /// rich text cut as its plain text; with word wrap a QLabel as it was.
    void aLongLineIsCutAndWholeInItsTip()
    {
        QWidget row;
        auto* layout = new QHBoxLayout(&row);
        auto* label = new ElidedLabel(&row);
        label->setContentsMargins(8, 2, 8, 2);
        layout->addWidget(label, 1);
        layout->addWidget(new QPushButton(QStringLiteral("Solve"), &row));
        row.resize(320, 40);
        row.show();
        QVERIFY(QTest::qWaitForWindowExposed(&row));
        const QString whole = QStringLiteral("a long line of text ").repeated(60).trimmed();
        // Set as the tabs set it, through a QLabel*.
        static_cast<QLabel*>(label)->setText(whole);
        settle();
        QCOMPARE(label->text(), whole);
        QCOMPARE(label->minimumSizeHint().width(), 8 + 8 + label->fontMetrics().averageCharWidth() * 4);
        QVERIFY2(row.minimumSizeHint().width() < 320, qPrintable(QString::number(row.minimumSizeHint().width())));
        QCOMPARE(row.width(), 320);
        QVERIFY(label->elided());
        const QString shown = label->shown();
        QVERIFY2(shown.endsWith(QChar(0x2026)) && shown.size() < whole.size() && whole.startsWith(shown.chopped(1)),
                 qPrintable(shown));
        QVERIFY(label->fontMetrics().horizontalAdvance(shown) <= label->contentsRect().width());
        QCOMPARE(tipOf(label), whole);
        // Drawn as a QLabel draws the line it shows, pixel for pixel.
        QWidget host;
        auto* twin = new QLabel(shown, &host);
        twin->setContentsMargins(label->contentsMargins());
        twin->setGeometry(label->geometry());
        host.resize(row.size());
        QCOMPARE(label->grab().toImage(), twin->grab().toImage());

        label->setToolTip(QStringLiteral("What the solve said"));
        const QString both = tipOf(label);
        QVERIFY2(both.startsWith(whole) && both.endsWith(QStringLiteral("What the solve said")), qPrintable(both));

        // Room enough: drawn whole, its own tool tip alone; its least width
        // never more than its text's, and a text shorter than a few
        // characters asks its own.
        label->setText(QStringLiteral("Solved"));
        settle();
        QVERIFY(!label->elided());
        QCOMPARE(label->shown(), QStringLiteral("Solved"));
        QCOMPARE(tipOf(label), QStringLiteral("What the solve said"));
        QVERIFY(label->minimumSizeHint().width() <= label->QLabel::minimumSizeHint().width());
        QVERIFY(!label->grab().isNull());
        label->setText(QStringLiteral("ok"));
        settle();
        QCOMPARE(label->minimumSizeHint().width(), label->QLabel::minimumSizeHint().width());
        label->setToolTip(QString());
        QVERIFY(tipOf(label).isEmpty());

        label->setText(QStringLiteral("<b>S11</b> ") + whole);
        settle();
        QVERIFY(label->elided());
        QVERIFY2(label->shown().startsWith(QStringLiteral("S11 a long line")), qPrintable(label->shown()));
        QVERIFY2(tipOf(label).startsWith(QStringLiteral("S11 a long line")), qPrintable(tipOf(label)));

        // Word wrap: a QLabel as it was, a word longer than the row too.
        label->setWordWrap(true);
        settle();
        QVERIFY(!label->elided());
        label->setText(QStringLiteral("frequency_sweep_").repeated(20) + QStringLiteral(" points"));
        settle();
        QVERIFY(!label->elided());
        QCOMPARE(label->minimumSizeHint(), label->QLabel::minimumSizeHint());
    }

    /// A tool button of a file's name beside another: it asks for its icon
    /// and a few characters; a name too long for it is drawn cut in the
    /// middle, its kind kept, as the button draws that cut name; text() whole.
    void aLongNameIsCutInItsButton()
    {
        QWidget row;
        auto* layout = new QHBoxLayout(&row);
        auto* chip = new ElidedToolButton(&row);
        chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        chip->setIcon(row.style()->standardIcon(QStyle::SP_FileIcon));
        layout->addWidget(chip);
        layout->addStretch(1);
        layout->addWidget(new QPushButton(QStringLiteral("Send"), &row));
        row.resize(260, 40);
        row.show();
        QVERIFY(QTest::qWaitForWindowExposed(&row));
        const QString name = QStringLiteral("a_model_of_the_coplanar_line_").repeated(6) + QStringLiteral("v2.qrf25");
        static_cast<QToolButton*>(chip)->setText(name);
        settle();
        QCOMPARE(chip->text(), name);
        const int words = chip->fontMetrics().size(Qt::TextShowMnemonic, name).width();
        QCOMPARE(chip->minimumSizeHint().width(),
                 chip->QToolButton::sizeHint().width() - words + chip->fontMetrics().averageCharWidth() * 4);
        QVERIFY2(row.minimumSizeHint().width() < 260, qPrintable(QString::number(row.minimumSizeHint().width())));
        QVERIFY(chip->elided());
        const QString shown = chip->shown();
        QVERIFY2(shown.contains(QChar(0x2026)) && shown.startsWith(QStringLiteral("a_model")) && shown.endsWith(QStringLiteral(".qrf25")),
                 qPrintable(shown));
        // Drawn as the button draws the name it shows.
        QWidget host;
        auto* twin = new QToolButton(&host);
        twin->setToolButtonStyle(chip->toolButtonStyle());
        twin->setIcon(chip->icon());
        twin->setText(shown);
        twin->setGeometry(chip->geometry());
        host.resize(row.size());
        QCOMPARE(chip->grab().toImage(), twin->grab().toImage());

        // Room enough: drawn whole; its least width never more than its
        // name's, and a name shorter than a few characters asks its own.
        chip->setText(QStringLiteral("rc.sch"));
        settle();
        QVERIFY(!chip->elided());
        QCOMPARE(chip->shown(), QStringLiteral("rc.sch"));
        QVERIFY(chip->minimumSizeHint().width() <= chip->QToolButton::minimumSizeHint().width());
        QVERIFY(!chip->grab().isNull());
        chip->setText(QStringLiteral("rc"));
        settle();
        QCOMPARE(chip->minimumSizeHint(), chip->QToolButton::minimumSizeHint());
    }

    /// A document of a name of two hundred characters in front: the Claude
    /// Code panel's chip of it, and pinned its pin's, cut, and the window and
    /// the dock as they were with a short name.
    void aDocumentsLongNameLeavesTheWindowAsItWas()
    {
        const QString shortName = path(QStringLiteral("n.txt"));
        const QString longName = path(QStringLiteral("a_note_on_the_coplanar_line_").repeated(7) + QStringLiteral("v2.txt"));
        for (const QString& f : {shortName, longName}) {
            QFile t(f);
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write("A line of text.\n");
        }
        QDockWidget* dock = app->claudeDockWidget();
        auto* attach = dock->findChild<QToolButton*>(QStringLiteral("claudeAttach"));
        auto* pin = dock->findChild<QToolButton*>(QStringLiteral("claudePin"));
        QVERIFY(attach != nullptr && pin != nullptr);
        QVERIFY(app->gotoPage(shortName) && quiet());
        reset();
        const int width = app->width(), least = app->minimumSizeHint().width(), docked = dock->width();
        QVERIFY(app->gotoPage(longName) && quiet());
        // The panel shows a name to its cap of characters, which leaves it
        // hundreds of pixels; the whole name in its tool tip.
        const QString name = QFileInfo(longName).fileName();
        QTRY_COMPARE(attach->text(), misc::shownFileName(longName));
        settle();
        QVERIFY2(app->width() == width && app->minimumSizeHint().width() <= least + 4 && dock->width() == docked
                     && dock->minimumSizeHint().width() <= docked,
                 qPrintable(QStringLiteral("the window %1 to %2, its least %3 to %4, the dock %5 to %6, its least %7")
                                .arg(width).arg(app->width()).arg(least).arg(app->minimumSizeHint().width())
                                .arg(docked).arg(dock->width()).arg(dock->minimumSizeHint().width())));
        auto* chip = dynamic_cast<ElidedToolButton*>(attach);
        QVERIFY2(chip != nullptr && chip->elided() && chip->shown().contains(QChar(0x2026)), "the chip drawn whole, not cut");
        QVERIFY(attach->toolTip().contains(name));
        // Pinned, the pin names it.
        QVERIFY(pin->isEnabled());
        pin->click();
        QTRY_COMPARE(pin->text(), misc::shownFileName(longName));
        settle();
        QVERIFY2(app->width() == width && app->minimumSizeHint().width() <= least + 4 && dock->width() == docked
                     && dock->minimumSizeHint().width() <= docked,
                 qPrintable(QStringLiteral("pinned: the window %1 to %2, its least %3 to %4, the dock %5 to %6, its least %7")
                                .arg(width).arg(app->width()).arg(least).arg(app->minimumSizeHint().width())
                                .arg(docked).arg(dock->width()).arg(dock->minimumSizeHint().width())));
        auto* pinned = dynamic_cast<ElidedToolButton*>(pin);
        QVERIFY2(pinned != nullptr && pinned->elided() && pinned->shown().contains(QChar(0x2026)), "the pin drawn whole, not cut");
        pin->click();
        QTRY_VERIFY(pin->text().isEmpty());
        app->closeAllFiles();
    }

    /// Each kind of tab, the Claude Code dock beside it: every label that
    /// gives way given a line of thousands of characters, and the window and
    /// the dock keep their widths, the label drawing it cut and holding it
    /// whole - the RF 2.5D status of the screenshots among them.
    void aTabsLongLineLeavesTheWindowAsItWas()
    {
        // The labels the program writes long lines in, whatever they are made
        // of: each is held to the window's width as the others are.
        const QStringList named{QStringLiteral("rfStatus"),     QStringLiteral("rfLayoutLabel"), QStringLiteral("layoutCell"),
                                QStringLiteral("layoutInfoText"), QStringLiteral("mpStatus"),    QStringLiteral("imageFacts"),
                                QStringLiteral("xmlStatus"),    QStringLiteral("zipStatus"),     QStringLiteral("claudeState"),
                                QStringLiteral("claudeModel")};
        const QString whole = QStringLiteral("a long line of text ").repeated(150).trimmed();
        QDockWidget* dock = app->claudeDockWidget();
        QSet<QString> seen;
        for (const auto& [kind, file] : kinds()) {
            QVERIFY2(!file.isEmpty() && app->gotoPage(file), qPrintable(kind));
            QVERIFY2(quiet(), qPrintable(kind));
            for (QLabel* l : app->findChildren<QLabel*>()) {
                auto* label = dynamic_cast<ElidedLabel*>(l);
                if (!l->isVisible() || l->window() != app || (label == nullptr && !named.contains(l->objectName()))) continue;
                reset();
                const int width = app->width(), least = app->minimumSizeHint().width(), docked = dock->width();
                const QString was = l->text();
                l->setText(whole);
                settle();
                const QString what = kind + QStringLiteral(": ") + whereIs(l);
                QVERIFY2(app->width() == width && app->minimumSizeHint().width() <= least + 4,
                         qPrintable(what + QStringLiteral(" - the window %1 to %2, its least %3 to %4")
                                               .arg(width).arg(app->width()).arg(least).arg(app->minimumSizeHint().width())));
                QVERIFY2(dock->width() == docked && dock->minimumSizeHint().width() <= docked,
                         qPrintable(what + QStringLiteral(" - the dock %1 to %2, its least %3")
                                               .arg(docked).arg(dock->width()).arg(dock->minimumSizeHint().width())));
                QVERIFY2(label != nullptr, qPrintable(what + QStringLiteral(" - drawn whole, not cut")));
                QVERIFY2(label->elided() && label->shown().endsWith(QChar(0x2026)), qPrintable(what));
                QCOMPARE(l->text(), whole);
                seen.insert(l->objectName());
                l->setText(was);
                settle();
            }
        }
        reset();
        for (const QString& name : named) QVERIFY2(seen.contains(name), qPrintable(name + QStringLiteral(" not seen")));
    }

    /// Opt-in (QUCS_LABEL_AUDIT): every label of one line in the window that
    /// a long text would widen it by, for each kind of tab, with where its
    /// width went. Headings whose text never changes are among them.
    void audit()
    {
        if (qEnvironmentVariableIsEmpty("QUCS_LABEL_AUDIT")) QSKIP("set QUCS_LABEL_AUDIT to list the labels");
        for (const auto& [kind, file] : kinds()) {
            QVERIFY2(!file.isEmpty() && app->gotoPage(file), qPrintable(kind));
            QVERIFY2(quiet(), qPrintable(kind));
            reset();
            QStringList parts{QStringLiteral("the tabs %1").arg(app->centralWidget()->minimumSizeHint().width())};
            for (QDockWidget* d : app->findChildren<QDockWidget*>())
                if (d->isVisible() && !d->isFloating())
                    parts << QStringLiteral("%1 %2").arg(d->windowTitle()).arg(d->minimumSizeHint().width());
            qInfo().noquote() << "==" << kind << ": the window asks" << app->minimumSizeHint().width() << "px -" << parts.join(QStringLiteral(", "));
            int probed = 0;
            const QList<Widening> found = widening(probed);
            qInfo().noquote() << "  " << found.size() << "of" << probed << "label(s) widen it";
            for (const Widening& w : found)
                qInfo().noquote() << "  +" << w.grew << "px" << w.where << "|" << w.text;
        }
    }

    // Opt in (QUCS_CLAUDE_PICTURES=<folder>, QUCS_CLAUDE_PICTURES_THEME=light or
    // dark): a reply with a long line of code, as the user reported it on 7
    // November, in the main window's dock, as pictures to look at - on a real
    // display when run with the platform's own plugin, not the offscreen one.
    void pictures()
    {
        const QString folder = qEnvironmentVariable("QUCS_CLAUDE_PICTURES");
        if (folder.isEmpty()) QSKIP("QUCS_CLAUDE_PICTURES names a folder for the pictures");
        auto* panel = app->claudeDockWidget()->findChild<ClaudeCodePanel*>();
        QVERIFY(panel != nullptr);
        const auto feed = [panel](const QString& markdown) {
            const QJsonObject text{{"type", "text"}, {"text", markdown}};
            const QJsonObject message{{"content", QJsonArray{text}}};
            panel->session()->handleLine(QJsonDocument(QJsonObject{{"type", "assistant"}, {"message", message}}).toJson(QJsonDocument::Compact));
            panel->renderNow();
        };
        const QString line = QStringLiteral("launchctl setenv SPICE_LIB_DIR \"/Volumes/Archive 01/git/Ngspice_OpenVAF_Enhancements/bin/macos/apple-silicon\"");
        const QString reply = QStringLiteral(
            "| Setting | spinit warning |\n|---|---|\n| `apple-silicon/scripts` | Yes |\n| `apple-silicon` | **No warning, and no errors** |\n\n"
            "One line in `.zshrc`, which covers ngspice run from a terminal:\n\n"
            "```bash\nexport SPICE_LIB_DIR=\"/Volumes/Archive 01/git/Ngspice_OpenVAF_Enhancements/bin/macos/apple-silicon\"\n```\n\n"
            "**Make it visible to the app**, then quit and reopen Qucs-S:\n\n```\n%1\n```\n\n"
            "Several lines, indented, one with no spaces:\n\n"
            "```python\ndef first():\n        return \"%2\"\n\nprint(first())\n```\n\n"
            "Then the words around it: this setting is lost on reboot, so it needs a LaunchAgent to persist.")
                                  .arg(line, QString(160, QLatin1Char('x')));
        const QString wide = QStringLiteral("A formula wider than the view, and words after it that still wrap at the view's width, "
                                            "with nothing cut off at the left:\n\n$$%1x$$\n\nAnd code after it:\n\n```\n%2\n```")
                                 .arg(QStringLiteral("x+").repeated(120), line);
        const QString blob = QStringLiteral("A blob of data, with places to break it that show nowhere:\n\n```\n%1\n```\n\nThe same in words: %2.")
                                 .arg(QStringLiteral("Tm93IGlzIHRoZSB0aW1l").repeated(150), QStringLiteral("Tm93IGlzIHRoZSB0aW1l").repeated(150));
        for (const auto& [name, markdown] : {std::pair{"code", reply}, std::pair{"wide", wide}, std::pair{"blob", blob}}) {
            panel->newConversation();
            feed(markdown);
            panel->transcript()->verticalScrollBar()->setValue(0);
            settle();
            const QString theme = pictureTheme() == qucs_s::apptheme::Dark ? QStringLiteral("dark") : QStringLiteral("light");
            const QString file = QDir(folder).filePath(QStringLiteral("%1-%2.png").arg(name, theme));
            QVERIFY(app->grab().save(file));
            QVERIFY(panel->transcript()->grab().save(QDir(folder).filePath(QStringLiteral("%1-%2-dock.png").arg(name, theme))));
            QVERIFY(panel->exportConversation(QDir(folder).filePath(QStringLiteral("%1-%2.pdf").arg(name, theme)), ClaudeCodePanel::ExportFormat::Pdf));
            qInfo("%s (%s): the dock %d wide, sideways %d of %d", qPrintable(file), qPrintable(QApplication::platformName()), app->claudeDockWidget()->width(),
                  panel->transcript()->horizontalScrollBar()->value(), panel->transcript()->horizontalScrollBar()->maximum());
        }
    }
};

QTEST_MAIN(TestLongLines)
#include "test_long_lines.moc"
