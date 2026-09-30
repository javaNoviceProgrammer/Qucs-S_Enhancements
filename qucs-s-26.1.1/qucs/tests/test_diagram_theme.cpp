/*
 * A diagram's theme (diagramtheme.h, the diagram dialog's Theme tab): the
 * colour of each of its parts - background, plot area, frame, grid, each
 * axis, title, a table's texts, the legend - automatic or chosen. Chosen
 * ones are drawn, saved after the title and read back; a diagram without
 * any is saved as before. Automatic is how diagrams were drawn - nothing
 * under them on light paper, a white card on dark paper - each part
 * fitted to the background it is on. The Theme tab sets them all, from a
 * ready-made theme or one by one, shows the diagram in them, and keeps a
 * default for new diagrams; Claude's add_diagram and edit_diagram set
 * them too.
 */
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include "config.h"
#include "ink.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "qucs.h"
#include "qucscontrol.h"
#include "mouseactions.h"
#include "schematic.h"
#include "settings.h"
#include "diagrams/diagramdialog.h"
#include "diagrams/diagramtheme.h"
#include "diagrams/graph.h"
#include "diagrams/polardiagram.h"
#include "diagrams/rectdiagram.h"
#include "diagrams/tabdiagram.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace theme = qucs_s::diagramtheme;
using Part = theme::Part;

namespace {

// A rectangular diagram at (100, 400), 400 x 300: v on the left axis, w
// on the right one, the legend top right, the title "Theme".
const char* kRectLine = "<Rect 100 400 400 300 3 #c0c0c0 1 00 1 0 0.2 1 1 0 0.2 1 1 0 2 10 315 0 225 1 0 0 2 -1 "
                        "\"\" \"\" \"\" \"Theme\">";

QString writeDataset(const QString& dir)
{
    const QString file = dir + "/theme.dat";
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return {};
    QTextStream s(&f);
    s << "<Qucs Dataset " PACKAGE_VERSION ">\n<indep x 11>\n";
    for (int i = 0; i <= 10; ++i) s << i / 10.0 << "\n";
    s << "</indep>\n<dep v x>\n";
    for (int i = 0; i <= 10; ++i) s << i / 10.0 << "\n";
    s << "</dep>\n<dep w x>\n";
    for (int i = 0; i <= 10; ++i) s << double(10 - i) << "\n";   // (down, across v)
    s << "</dep>\n";
    return file;
}

template <class D = RectDiagram>
D* makeDiagram(const QString& line, const QString& body, const QString& data = {})
{
    QString text = body;
    QTextStream stream(&text, QIODevice::ReadOnly);
    auto* d = new D();
    if (!d->load(line, &stream)) {
        delete d;
        return nullptr;
    }
    if (!data.isEmpty()) d->loadGraphData(data);
    return d;
}

RectDiagram* rect(const QString& data, const QString& line = kRectLine)
{
    return makeDiagram(line, "<\"v\" #0000ff 1 3 0 0 0>\n<\"w\" #ff0000 1 3 0 0 1>\n</Rect>\n", data);
}

// The diagram painted on \a canvas (as the schematic paints it: the canvas
// is the paper).
QImage render(Diagram* d, const QColor& canvas = Qt::white)
{
    QImage img(700, 600, QImage::Format_RGB32);
    img.fill(canvas);
    QPainter p(&img);
    p.setFont(QFont("Helvetica", 12));
    const qucs_s::ink::Paper paper(canvas);
    d->paint(&p);
    return img;
}

int countOf(const QImage& img, const QRect& area, const std::function<bool(QColor)>& is)
{
    int n = 0;
    const QRect a = area.intersected(img.rect());
    for (int y = a.top(); y <= a.bottom(); ++y)
        for (int x = a.left(); x <= a.right(); ++x)
            if (is(img.pixelColor(x, y))) ++n;
    return n;
}

int countOf(const QImage& img, const QRect& area, const QColor& c)
{
    return countOf(img, area, [&c](QColor p) { return p.rgb() == c.rgb(); });
}



// The areas of a diagram laid out as kRectLine's, in the image.
struct Areas {
    QRect frame, inside, below, left, right, above, legend, ticks, leftTicks, rightTicks;
};
Areas areasOf(const Diagram* d)
{
    Areas a;
    a.frame = QRect(d->cx, d->cy - d->y2, d->x2 + 1, d->y2 + 1);
    a.inside = a.frame.adjusted(2, 2, -2, -2);
    // Its numbers and labels, clear of the ticks (5 pixels out of the
    // frame), the y axes' above the x axis' numbers (centred under its
    // ends); the x axis' ticks.
    a.below = QRect(d->cx - 10, d->cy + 7, d->x2 + 20, 40);
    a.left = QRect(d->cx - 70, d->cy - d->y2, 63, d->y2 - 20);
    a.right = QRect(d->cx + d->x2 + 7, d->cy - d->y2, 70, d->y2 - 20);
    a.ticks = QRect(d->cx + 1, d->cy + 1, d->x2 - 2, 4);
    a.leftTicks = QRect(d->cx - 5, d->cy - d->y2 + 1, 5, d->y2 - 2);
    a.rightTicks = QRect(d->cx + d->x2 + 1, d->cy - d->y2 + 1, 5, d->y2 - 2);
    a.above = QRect(d->cx, d->cy - d->y2 - 40, d->x2, 38);
    a.legend = QRect(d->cx + d->x2 / 2, d->cy - d->y2 + 2, d->x2 / 2 - 2, 80);
    return a;
}

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

} // namespace

class TestDiagramTheme : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QString data;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.font = QFont("Helvetica", 12);   // (what the axes are laid out in)
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        data = writeDataset(dir.path());
        QVERIFY(!data.isEmpty());
    }

    void cleanup() { theme::setDefaultForNewDiagrams(theme::Theme()); }

    // Chosen colours are saved after the title - "" when it has none - and
    // read back; older versions read the title and no further. A diagram
    // with none is saved as before; the grid's colour stays where it was,
    // the light grey of old automatic.
    void aThemeIsSavedAndReadBack()
    {
        theme::Theme t;
        t.choose(Part::Background, QColor(0x10, 0x14, 0x18));
        t.choose(Part::LegendBackground, QColor(0x20, 0x30, 0x40, 0x80));
        QCOMPARE(t.toString(), QStringLiteral("background=#101418 legend_background=#80203040"));
        QCOMPARE(theme::Theme::fromString(t.toString()), t);
        QVERIFY(theme::Theme::fromString("").isAutomatic());
        // What does not read is left out.
        const theme::Theme odd = theme::Theme::fromString("background=#zzz frame=#123456 nothing=#ffffff =#000000 grid");
        QCOMPARE(odd.toString(), QStringLiteral("frame=#123456"));

        QScopedPointer<RectDiagram> d(rect(data));
        QVERIFY(d);
        QVERIFY(d->theme().isAutomatic());
        const QString before = d->save().section('\n', 0, 0);
        QVERIFY(before.endsWith("\"\" \"\" \"\" \"Theme\">"));

        theme::Theme chosen = t;
        chosen.choose(Part::Grid, QColor(0x33, 0x44, 0x55));
        chosen.choose(Part::Text, Qt::red);   // (a table's: not a rectangular diagram's)
        d->setTheme(chosen);
        chosen.choose(Part::Text, QColor());
        QCOMPARE(d->theme(), chosen);
        const QString line = d->save().section('\n', 0, 0);
        QCOMPARE(line.section(' ', 6, 6), QStringLiteral("#334455"));   // the grid's colour where it was
        QVERIFY2(line.endsWith("\"Theme\" \"background=#101418 legend_background=#80203040\">"), qPrintable(line));
        QCOMPARE(line.section('"', 7, 7), QStringLiteral("Theme"));   // what an older version reads
        QScopedPointer<RectDiagram> again(makeDiagram(line, "</Rect>\n"));
        QVERIFY(again);
        QCOMPARE(again->theme(), chosen);
        QCOMPARE(again->title, QStringLiteral("Theme"));

        // No title: an empty one before the colours.
        d->title.clear();
        const QString untitled = d->save().section('\n', 0, 0);
        QVERIFY2(untitled.endsWith("\"\" \"\" \"\" \"\" \"background=#101418 legend_background=#80203040\">"), qPrintable(untitled));
        QScopedPointer<RectDiagram> read(makeDiagram(untitled, "</Rect>\n"));
        QVERIFY(read && read->title.isEmpty());
        QCOMPARE(read->theme(), chosen);

        // Back to automatic: the line of before.
        d->title = "Theme";
        d->setTheme(theme::Theme());
        QCOMPARE(d->save().section('\n', 0, 0), before);
        QCOMPARE(d->GridPen.color(), QColor(Qt::lightGray));
    }

    // Automatic: nothing under it on light paper (a cream canvas shows
    // through), a white card under all it draws on dark paper; the frame
    // black on both.
    void automaticIsHowDiagramsWereDrawn()
    {
        QScopedPointer<RectDiagram> d(rect(data));
        const Areas a = areasOf(d.data());
        const QPoint corner(d->cx - 3, d->cy - d->y2 - 3);   // clear of its numbers, on its card
        const QColor cream(0xfd, 0xf6, 0xe3), dark = qucs_s::ink::darkPaperColour();
        const QImage onCream = render(d.data(), cream);
        QCOMPARE(onCream.pixelColor(corner), cream);
        QVERIFY(countOf(onCream, a.inside, cream) > a.inside.width() * a.inside.height() / 2);
        QVERIFY(countOf(onCream, QRect(a.frame.left() + 20, a.frame.top(), 100, 1), Qt::black) > 50);
        const QImage onDark = render(d.data(), dark);
        QCOMPARE(onDark.pixelColor(corner), QColor(Qt::white));
        QCOMPARE(countOf(onDark, a.inside, dark), 0);
        QVERIFY(countOf(onDark, QRect(a.frame.left() + 20, a.frame.top(), 100, 1), Qt::black) > 50);
        // The traces as they are (light paper, a card on dark).
        QVERIFY(countOf(onCream, a.inside, QColor(0, 0, 255)) > 50);
        QVERIFY(countOf(onDark, a.inside, QColor(0, 0, 255)) > 50);
    }

    // Each part chosen is drawn in its colour, where it is.
    void eachPartIsDrawnInItsColour()
    {
        QScopedPointer<RectDiagram> d(rect(data));
        const struct {
            Part part;
            QColor color;
        } chosen[] = {{Part::Background, QColor(0xf0, 0xe0, 0xd0)}, {Part::PlotArea, QColor(0xe0, 0xf0, 0xe0)},
                      {Part::Frame, QColor(0x80, 0x00, 0x80)},      {Part::Grid, QColor(0x00, 0xc0, 0xc0)},
                      {Part::XAxis, QColor(0xc0, 0x40, 0x00)},      {Part::YAxis, QColor(0x00, 0x80, 0x00)},
                      {Part::RightAxis, QColor(0x80, 0x40, 0x00)},  {Part::Title, QColor(0x00, 0x40, 0xc0)},
                      {Part::LegendBackground, QColor(0xff, 0xff, 0xc0)}, {Part::LegendBorder, QColor(0xc0, 0x00, 0x40)},
                      {Part::LegendText, QColor(0x40, 0x00, 0xc0)}};
        theme::Theme t;
        for (const auto& c : chosen) t.choose(c.part, c.color);
        d->setTheme(t);
        const Areas a = areasOf(d.data());
        const QImage img = render(d.data());
        if (const QString grabs = qEnvironmentVariable("THEME_GRABS"); !grabs.isEmpty()) img.save(grabs + "/each part.png");
        const QPoint corner(d->cx - 3, d->cy - d->y2 - 3);
        QCOMPARE(img.pixelColor(corner), chosen[0].color);
        QVERIFY(countOf(img, a.inside, chosen[1].color) > a.inside.width() * a.inside.height() / 3);
        QVERIFY(countOf(img, QRect(a.frame.left() + 20, a.frame.top(), 100, 1), chosen[2].color) > 50);
        QVERIFY(countOf(img, a.inside, chosen[3].color) > 100);
        QVERIFY(countOf(img, a.below, chosen[4].color) > 20);
        QVERIFY(countOf(img, a.ticks, chosen[4].color) > 10);
        QVERIFY(countOf(img, a.leftTicks, chosen[5].color) > 10);
        QVERIFY(countOf(img, a.rightTicks, chosen[6].color) > 10);
        QVERIFY(countOf(img, a.left, chosen[5].color) > 20);
        QVERIFY(countOf(img, a.right, chosen[6].color) > 20);
        QVERIFY(countOf(img, a.above, chosen[7].color) > 10);
        QVERIFY(countOf(img, a.legend, chosen[8].color) > 200);
        // (Antialiased, over the legend's and the plot area's colours:
        // reddish, as nothing else there is.)
        QVERIFY(countOf(img, a.legend, [](QColor p) { return p.red() > 150 && p.green() < 150 && p.blue() < 150; }) > 50);
        QVERIFY(countOf(img, a.legend, chosen[10].color) > 5);
        // Each where it is, and nowhere else: the axes' colours not in
        // the others' places.
        QCOMPARE(countOf(img, a.left, chosen[4].color), 0);
        QCOMPARE(countOf(img, a.below, chosen[5].color), 0);
        QCOMPARE(countOf(img, a.left, chosen[6].color), 0);
        // The traces keep theirs.
        QVERIFY(countOf(img, a.inside, QColor(0, 0, 255)) > 50);
        QVERIFY(countOf(img, a.inside, QColor(255, 0, 0)) > 50);
    }

    // What is automatic fits the background chosen: on a dark one the
    // frame, the numbers and the legend's text turn light, the legend
    // dark, a dark trace lighter; a see-through background ("no
    // background") shows the canvas, and on a dark canvas the parts fit it.
    void automaticPartsShowOnTheBackgroundChosen()
    {
        const QColor night(0x10, 0x14, 0x18);
        theme::Theme t;
        t.choose(Part::Background, night);
        const theme::Colors c = theme::colorsOn(t, Qt::white);
        QCOMPARE(c.card, night);
        QCOMPARE(c.outside, night);
        for (Part p : {Part::Frame, Part::XAxis, Part::YAxis, Part::RightAxis, Part::Title, Part::LegendBorder})
            QVERIFY2(qucs_s::ink::contrast(c.of(p), night) >= 3.0, qPrintable(theme::keyOf(p)));
        QVERIFY(qucs_s::ink::isDark(c.legend));
        QVERIFY(qucs_s::ink::contrast(c.of(Part::LegendText), c.legend) >= 3.0);
        QVERIFY(qucs_s::ink::contrast(c.of(Part::Grid), night) < 3.0);   // faint still

        // Automatic, on white: black, light grey, the white legend of old.
        const theme::Colors plain = theme::colorsOn(theme::Theme(), Qt::white);
        QVERIFY(!plain.card.isValid());
        QCOMPARE(plain.of(Part::Frame), QColor(Qt::black));
        QCOMPARE(plain.of(Part::Grid), QColor(Qt::lightGray));
        QCOMPARE(plain.of(Part::LegendBackground), QColor(255, 255, 255, 230));
        QCOMPARE(plain.of(Part::LegendBorder), QColor(Qt::darkGray));
        // On dark paper: the white card, and the same colours on it.
        const theme::Colors card = theme::colorsOn(theme::Theme(), qucs_s::ink::darkPaperColour());
        QCOMPARE(card.card, QColor(Qt::white));
        QCOMPARE(card.of(Part::Frame), QColor(Qt::black));
        // No background, on dark paper: the canvas, and the parts fit it.
        const QColor dark = qucs_s::ink::darkPaperColour();
        const theme::Colors bare = theme::colorsOn(theme::preset(theme::Preset::NoCard), dark);
        QCOMPARE(bare.outside, dark);
        QVERIFY(qucs_s::ink::contrast(bare.of(Part::Frame), dark) >= 3.0);

        QScopedPointer<RectDiagram> d(rect(data));
        d->setTheme(t);
        const Areas a = areasOf(d.data());
        const QImage img = render(d.data());
        if (const QString grabs = qEnvironmentVariable("THEME_GRABS"); !grabs.isEmpty()) img.save(grabs + "/dark background.png");
        QVERIFY(countOf(img, QRect(a.frame.left() + 20, a.frame.top(), 100, 1), c.of(Part::Frame)) > 50);
        QVERIFY(countOf(img, a.left, c.of(Part::YAxis)) > 20);
        QVERIFY(countOf(img, a.inside, c.of(Part::Grid)) > 100);   // the faint grid of dark paper
        QCOMPARE(countOf(img, a.inside, QColor(Qt::lightGray)), 0);
        QCOMPARE(countOf(img, a.inside, QColor(0, 0, 255)), 0);   // dark blue does not show on it: lighter
        const QColor lighter = [&] {
            const qucs_s::ink::Paper paper(night);
            return qucs_s::ink::on(QColor(0, 0, 255));
        }();
        QVERIFY(countOf(img, a.inside, lighter) > 50);

        QScopedPointer<RectDiagram> bareDiagram(rect(data));
        bareDiagram->setTheme(theme::preset(theme::Preset::NoCard));
        const QImage onDark = render(bareDiagram.data(), dark);
        if (const QString grabs = qEnvironmentVariable("THEME_GRABS"); !grabs.isEmpty()) onDark.save(grabs + "/no background.png");
        QCOMPARE(onDark.pixelColor(d->cx - 3, d->cy - d->y2 - 3), dark);
        QVERIFY(countOf(onDark, QRect(a.frame.left() + 20, a.frame.top(), 100, 1), bare.of(Part::Frame)) > 50);

        // A polar chart's plot area is its circle; its numbers inside it
        // fit the plot area.
        QScopedPointer<PolarDiagram> polar(makeDiagram<PolarDiagram>(
            "<Polar 100 400 300 300 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 \"\" \"\" \"\">",
            "<\"v\" #0000ff 1 3 0 0 0>\n</Polar>\n", data));
        QVERIFY(polar);
        theme::Theme round;
        round.choose(Part::Background, Qt::white);
        round.choose(Part::PlotArea, night);
        polar->setTheme(round);
        const QImage chart = render(polar.data());
        if (const QString grabs = qEnvironmentVariable("THEME_GRABS"); !grabs.isEmpty()) chart.save(grabs + "/polar.png");
        QCOMPARE(chart.pixelColor(polar->cx + 150 - 32, polar->cy - 150 - 32), night);   // in the circle, between rings
        QCOMPARE(chart.pixelColor(polar->cx + 4, polar->cy - 4), QColor(Qt::white));   // in its corner, out of it
        QVERIFY(qucs_s::ink::contrast(polar->colors().of(Part::YAxis), night) >= 3.0);
    }

    // A table has its background, its rules and its texts; the colours of
    // parts it does not have are not kept.
    void aTableHasItsOwnParts()
    {
        QScopedPointer<TabDiagram> tab(makeDiagram<TabDiagram>(
            "<Tab 100 400 300 200 3 #c0c0c0 1 00 1 0 1 1 1 0 1 1 1 0 1 1 315 0 225 1 0 0 0 -1 \"\" \"\" \"\">",
            "<\"v\" #0000ff 0 3 1 0 0>\n</Tab>\n", data));
        QVERIFY(tab);
        QCOMPARE(tab->themeParts(), (QList<Part>{Part::Background, Part::Frame, Part::Text}));
        tab->setTheme(theme::preset(theme::Preset::Dark));
        const theme::Theme kept = tab->theme();
        QCOMPARE(kept.toString(), QStringLiteral("background=#1e1f22 frame=#8c8f94 text=#d4d6d9"));
        const QImage img = render(tab.data());
        if (const QString grabs = qEnvironmentVariable("THEME_GRABS"); !grabs.isEmpty()) img.save(grabs + "/table.png");
        const QRect table(tab->cx + 2, tab->cy - tab->y2 + 2, tab->x2 - 4, tab->y2 - 4);
        QVERIFY(countOf(img, table, QColor(0x1e, 0x1f, 0x22)) > table.width() * table.height() / 2);
        QVERIFY(countOf(img, table, QColor(0xd4, 0xd6, 0xd9)) > 10);
        QVERIFY(countOf(img, QRect(tab->cx, tab->cy - tab->y2, tab->x2, 1), QColor(0x8c, 0x8f, 0x94)) > 100);
    }

    // The Theme tab: a row for each part the diagram has (the grid's
    // colour moved there from Properties), a ready-made theme to start
    // from - Custom when the colours are no one's - a reset for each, the
    // diagram shown in them; applied to the diagram, and kept as the
    // default for new diagrams.
    void theThemeTabSetsThemAll()
    {
        QScopedPointer<RectDiagram> d(rect(data));
        auto* dialog = new DiagramDialog(d.data(), nullptr);   // WA_DeleteOnClose
        auto* tabs = dialog->findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        QStringList titles;
        for (int i = 0; i < tabs->count(); ++i) titles << tabs->tabText(i);
        QCOMPARE(titles, (QStringList{"Data", "Properties", "Limits", "Theme", "Import"}));
        for (QLabel* label : dialog->findChildren<QLabel*>()) QVERIFY(label->text() != "Grid Color:");
        const QList<Part> parts = d->themeParts();
        QCOMPARE(parts.size(), 11);
        for (Part p : parts) {
            auto* button = dialog->findChild<QPushButton*>("theme_" + theme::keyOf(p));
            QVERIFY2(button != nullptr, qPrintable(theme::keyOf(p)));
            QCOMPARE(button->text(), QStringLiteral("Automatic"));
            QVERIFY(!dialog->findChild<QToolButton*>("theme_" + theme::keyOf(p) + "_auto")->isEnabled());
        }
        QVERIFY(dialog->findChild<QPushButton*>("theme_text") == nullptr);
        auto* preset = dialog->findChild<QComboBox*>("diagramThemePreset");
        QVERIFY(preset != nullptr);
        QCOMPARE(preset->currentText(), QStringLiteral("Automatic"));
        auto* model = qobject_cast<QStandardItemModel*>(preset->model());
        const int mine = preset->findData(-1), custom = preset->findData(-2);
        QVERIFY(!model->item(mine)->isEnabled());   // no default saved
        QVERIFY(!model->item(custom)->isEnabled());

        // Dark: every part's colour.
        const int dark = preset->findData(int(theme::Preset::Dark));
        preset->setCurrentIndex(dark);
        emit preset->activated(dark);
        QCOMPARE(dialog->findChild<QPushButton*>("theme_background")->text(), QStringLiteral("#1E1F22"));
        QCOMPARE(dialog->findChild<QPushButton*>("theme_legend_background")->text(), QStringLiteral("#E62B2D30"));
        QCOMPARE(preset->currentText(), QStringLiteral("Dark"));
        // The preview draws it so, on the diagram's canvas.
        auto* preview = dialog->findChild<QWidget*>("diagramThemePreview");
        QVERIFY(preview != nullptr);
        preview->resize(400, 300);
        const QImage shown = preview->grab().toImage().convertToFormat(QImage::Format_RGB32);
        QVERIFY(countOf(shown, shown.rect(), QColor(0x26, 0x28, 0x2c)) > 1000);
        QVERIFY(d->theme().isAutomatic());   // (not yet applied)
        if (const QString grabs = qEnvironmentVariable("THEME_GRABS"); !grabs.isEmpty()) {
            tabs->setCurrentIndex(3);
            dialog->resize(dialog->sizeHint());
            dialog->grab().save(grabs + "/theme tab.png");
        }

        // One reset: Custom.
        dialog->findChild<QToolButton*>("theme_frame_auto")->click();
        QCOMPARE(dialog->findChild<QPushButton*>("theme_frame")->text(), QStringLiteral("Automatic"));
        QCOMPARE(preset->currentIndex(), custom);
        QVERIFY(model->item(custom)->isEnabled());

        QVERIFY(QMetaObject::invokeMethod(dialog, "slotApply"));
        theme::Theme expected = theme::preset(theme::Preset::Dark);
        expected.choose(Part::Frame, QColor());
        expected.choose(Part::Text, QColor());   // (a table's)
        QCOMPARE(d->theme(), expected);
        QCOMPARE(d->GridPen.color(), QColor(0x3c, 0x3f, 0x44));

        // The default for new diagrams.
        // (All the tab's colours: a table placed then has Dark's texts.)
        dialog->findChild<QPushButton*>("diagramThemeSaveDefault")->click();
        theme::Theme saved = theme::preset(theme::Preset::Dark);
        saved.choose(Part::Frame, QColor());
        QCOMPARE(theme::defaultForNewDiagrams(), saved);
        QVERIFY(model->item(mine)->isEnabled());
        const int automatic = preset->findData(int(theme::Preset::Automatic));
        preset->setCurrentIndex(automatic);
        emit preset->activated(automatic);
        QCOMPARE(dialog->findChild<QPushButton*>("theme_background")->text(), QStringLiteral("Automatic"));
        preset->setCurrentIndex(mine);
        emit preset->activated(mine);
        QCOMPARE(dialog->findChild<QPushButton*>("theme_background")->text(), QStringLiteral("#1E1F22"));
        QCOMPARE(preset->currentIndex(), mine);
        dialog->close();

        // A table's: its three parts.
        TabDiagram tab;
        auto* tabDialog = new DiagramDialog(&tab, nullptr);
        QStringList rows;
        for (QPushButton* b : tabDialog->findChildren<QPushButton*>())
            if (b->objectName().startsWith("theme_")) rows << b->objectName();
        rows.sort();
        QCOMPARE(rows, (QStringList{"theme_background", "theme_frame", "theme_text"}));
        tabDialog->close();
    }

    // Claude's tools: add_diagram starts from the default for new
    // diagrams and takes a theme - a preset, then parts - as edit_diagram
    // does; get_schematic lists the colours chosen; what does not read is
    // said so.
    void claudeSetsTheColours()
    {
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("false");
        QucsSettings.S4Qworkdir = dir.filePath("s4q");
        QDir().mkpath(dir.filePath("workspace"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("workspace"));
        QucsApp app(false);
        MainGuard guard(&app);
        auto* control = app.findChild<QucsControl*>();
        QVERIFY(control != nullptr);
        const auto json = [](const QJsonObject& r) {
            for (const QJsonValue& v : r.value("content").toArray())
                if (v.toObject().value("type").toString() == "text")
                    return QJsonDocument::fromJson(v.toObject().value("text").toString().toUtf8()).object();
            return QJsonObject();
        };
        QVERIFY(!control->callNow("new_document", {{"kind", "schematic"}}).value("isError").toBool());

        QJsonObject r = control->callNow("add_diagram", {{"theme", QJsonObject{{"preset", "dark"}, {"frame", "auto"}, {"title", "white"}}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        QJsonObject colors = json(r).value("theme").toObject();
        QCOMPARE(colors.value("background").toString(), QStringLiteral("#1e1f22"));
        QCOMPARE(colors.value("title").toString(), QStringLiteral("#ffffff"));
        QCOMPARE(colors.value("grid").toString(), QStringLiteral("#3c3f44"));
        QVERIFY(!colors.contains("frame"));
        QVERIFY(!colors.contains("text"));   // (a table's)

        r = control->callNow("edit_diagram", {{"theme", QJsonObject{{"background", "#80ffffff"}, {"legend_text", "auto"}}}});
        QVERIFY2(!r.value("isError").toBool(), qPrintable(QucsControl::textOf(r)));
        colors = json(r).value("theme").toObject();
        QCOMPARE(colors.value("background").toString(), QStringLiteral("#80ffffff"));
        QVERIFY(!colors.contains("legend_text"));

        // What does not read.
        r = control->callNow("edit_diagram", {{"theme", QJsonObject{{"background", "notacolour"}}}});
        QVERIFY(r.value("isError").toBool());
        QVERIFY(QucsControl::textOf(r).contains("no color"));
        r = control->callNow("edit_diagram", {{"theme", QJsonObject{{"axes", "#000000"}}}});
        QVERIFY(r.value("isError").toBool());
        QVERIFY(QucsControl::textOf(r).contains("x_axis"));
        r = control->callNow("edit_diagram", {{"theme", QJsonObject{{"preset", "neon"}}}});
        QVERIFY(r.value("isError").toBool());
        r = control->callNow("edit_diagram", {{"theme", "dark"}});
        QVERIFY(r.value("isError").toBool());

        // Automatic again: nothing listed.
        r = control->callNow("edit_diagram", {{"theme", QJsonObject{{"preset", "automatic"}}}});
        QVERIFY(!json(r).contains("theme"));

        // A new one in the default.
        theme::Theme mine;
        mine.choose(Part::PlotArea, QColor(0xee, 0xee, 0xff));
        theme::setDefaultForNewDiagrams(mine);
        r = control->callNow("add_diagram", {{"x", 600}, {"y", 400}});
        QCOMPARE(json(r).value("theme").toObject(), (QJsonObject{{"plot_area", "#eeeeff"}}));
        r = control->callNow("add_diagram", {{"x", 1000}, {"y", 400}, {"theme", QJsonObject{{"preset", "automatic"}}}});
        QVERIFY(!json(r).contains("theme"));

        // A diagram placed by hand: in the default too.
        Schematic* sch = app.currentSchematic();
        QVERIFY(sch != nullptr);
        const size_t before = sch->a_Diagrams->size();
        app.view->selElem = new RectDiagram();
        bool opened = false;
        QTimer::singleShot(0, [&opened] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<DiagramDialog*>(w)) {
                    // (It shows the default before it is placed.)
                    auto* button = dialog->findChild<QPushButton*>("theme_plot_area");
                    opened = button != nullptr && button->text() == QStringLiteral("#EEEEFF");
                    // (Placed when something was set: a title.)
                    dialog->findChild<QLineEdit*>("diagramTitle")->setText("placed");
                    QMetaObject::invokeMethod(dialog, "slotOK");
                }
        });
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        app.view->MPressElement(sch, &press, 1400, 400);
        QVERIFY(opened);
        QCOMPARE(sch->a_Diagrams->size(), before + 1);
        QCOMPARE(sch->a_Diagrams->back()->theme(), mine);

        for (QucsDoc* doc : app.allDocuments()) doc->setDocChanged(false);
        if (Schematic* s = app.currentSchematic()) s->setChanged(false);
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestDiagramTheme test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_diagram_theme.moc"
