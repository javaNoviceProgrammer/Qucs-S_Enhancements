/*
 * The Theme setting: System, Dark or Light - the platform's look - or a
 * designed theme (Daylight, Nord, Dracula...) for the application's
 * windows, menus and dialogs. On the offscreen platform Qt cannot switch
 * the appearance, so this exercises the palette the platform themes fall
 * back to; the designed themes' colours, style and style sheet; the paper,
 * grid, component list and text editor that follow a theme; the settings
 * dialog's control, View > Theme and the setting's storage.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QComboBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTabWidget>
#include <QTabBar>
#include <QAbstractButton>
#include <QProxyStyle>
#include <QPainter>
#include <QMenu>
#include <QMenuBar>
#include <QActionGroup>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QStatusBar>
#include <QStyleFactory>

#include <algorithm>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "apptheme.h"
#include "designedstyle.h"
#include "ink.h"
#include "settings.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"
#include "dialogs/qucssettingsdialog.h"
#include "textdoc.h"
#include "simulationconsole.h"

using namespace qucs_s::apptheme;

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The setting and the look are global: put both back whatever happens.
struct ThemeGuard {
    int theme = QucsSettings.Theme;
    ~ThemeGuard()
    {
        QucsSettings.Theme = theme;
        _settings::Get().setItem<int>("Theme", theme);
        apply(System);
    }
};

// Whether the designed style draws the application: the application's
// style, or - with a style sheet on - the one under Qt's style-sheet style.
bool designedStyleShown()
{
    QStyle* style = QApplication::style();
    if (dynamic_cast<qucs_s::apptheme::DesignedStyle*>(style) != nullptr) return true;
    for (QStyle* under : style->findChildren<QStyle*>(QString(), Qt::FindDirectChildrenOnly))
        if (dynamic_cast<qucs_s::apptheme::DesignedStyle*>(under) != nullptr) return true;
    return false;
}

QComboBox* themeCombo(QWidget* dialog)
{
    return dialog->findChild<QComboBox*>("themeCombo");
}

// The App Style combo: the one listing the styles.
QComboBox* styleCombo(QWidget* dialog)
{
    for (QComboBox* c : dialog->findChildren<QComboBox*>())
        if (c->count() == QStyleFactory::keys().size() && c->findText(QStyleFactory::keys().constFirst()) >= 0)
            return c;
    return nullptr;
}

// The themes a combo offers, separators left out.
QList<int> offered(QComboBox* combo)
{
    QList<int> themes;
    for (int i = 0; i < combo->count(); ++i)
        if (combo->itemData(i).isValid()) themes << combo->itemData(i).toInt();
    return themes;
}

// The component list of the main window.
QListWidget* componentList(QucsApp& app)
{
    for (QListWidget* l : app.findChildren<QListWidget*>())
        if (l->viewMode() == QListView::IconMode) return l;
    return nullptr;
}

// The pixels of an image left dark blue on \a paper: the colour the symbols
// of Qucs are drawn in for white. Only a blue below the paper in red and
// green counts: text lighter than the paper, anti-aliased - on Linux with
// coloured subpixel fringes - stays between the paper and itself in every
// channel, so it never does.
QList<QPoint> darkBlue(const QImage& image, const QColor& paper)
{
    QList<QPoint> found;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() > 200 && c.blue() > 90 && c.blue() > c.red() + 50 && c.blue() > c.green() + 50
                && c.value() < 170 && c.red() < std::min(60, paper.red() - 10) && c.green() < paper.green() - 10)
                found << QPoint(x, y);
        }
    return found;
}

// A failure's message: how many and the first few, where and what colour.
QByteArray describe(const QImage& image, const QList<QPoint>& found)
{
    QStringList first;
    for (const QPoint& p : found.mid(0, 8))
        first << QStringLiteral("(%1,%2) %3").arg(p.x()).arg(p.y()).arg(image.pixelColor(p).name());
    return QStringLiteral("%1 dark blue pixels: %2").arg(found.size()).arg(first.join(QStringLiteral(", "))).toUtf8();
}

// A style that closes tabs on the left, as the macOS style does.
class ClosesOnTheLeft : public QProxyStyle
{
public:
    ClosesOnTheLeft() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
                  QStyleHintReturn* ret = nullptr) const override
    {
        if (hint == SH_TabBar_CloseButtonPosition) return QTabBar::LeftSide;
        return QProxyStyle::styleHint(hint, option, widget, ret);
    }
};

// The side a tab bar's style puts the close buttons on.
QTabBar::ButtonPosition closeSide(QTabBar* bar)
{
    return QTabBar::ButtonPosition(bar->style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, bar));
}

struct PaperGuard {
    bool follows = QucsSettings.PaperFollowsTheme;
    QColor background = QucsSettings.BGColor;
    ~PaperGuard() { QucsSettings.PaperFollowsTheme = follows; QucsSettings.BGColor = background; }
};

QPushButton* button(QWidget* w, const QString& text)
{
    for (QPushButton* b : w->findChildren<QPushButton*>())
        if (b->text() == text) return b;
    return nullptr;
}
} // namespace

class TestAppTheme : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QColor systemWindow;   // the window colour the platform gave us
    bool systemIsDark = false;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.Theme = System;
        QucsSettings.BGColor = QColor(255, 250, 225);   // the document background, as shipped
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        systemWindow = QApplication::palette().color(QPalette::Window);
        systemIsDark = isDark();
    }

    void storedValuesAreThemes()
    {
        QCOMPARE(themes().size(), 13);
        QCOMPARE(themes().mid(0, 3), QList<int>({System, Dark, Light}));
        for (int theme : themes()) {
            QCOMPARE(bounded(theme), theme);
            QVERIFY(!name(theme).isEmpty());
        }
        QCOMPARE(bounded(-1), int(System));
        QCOMPARE(bounded(13), int(System));
        QCOMPARE(bounded(1234), int(System));
        // The designed ones, light first; the ids are what the settings keep.
        QCOMPARE(designed().size(), 10);
        QCOMPARE(designed().constFirst().id, int(Daylight));
        QCOMPARE(designed().constLast().id, int(CatppuccinMocha));
        QCOMPARE(name(Nord), QString("Nord"));
        QCOMPARE(name(SolarizedDark), QString("Solarized Dark"));
        QVERIFY(designedTheme(System) == nullptr && designedTheme(Light) == nullptr);
        QVERIFY(designedTheme(Dracula) != nullptr && designedTheme(Dracula)->dark);
        QVERIFY(designedTheme(Paper) != nullptr && !designedTheme(Paper)->dark);
    }

    void everyDesignedThemeIsReadable()
    {
        using qucs_s::ink::contrast;
        for (const Designed& d : designed()) {
            const Colours& c = d.colours;
            const QByteArray what = d.name;
            QVERIFY2(qucs_s::ink::isDark(c.window) == d.dark, what);
            QVERIFY2(qucs_s::ink::isDark(c.paper) == d.dark, what);   // dark paper inks the symbols
            // Text as WCAG asks of body text; secondary text as of large
            // text; the accent's own text too.
            QVERIFY2(contrast(c.text, c.window) >= 4.5, what);
            QVERIFY2(contrast(c.text, c.base) >= 4.5, what);
            QVERIFY2(contrast(c.text, c.surface) >= 4.5, what);
            QVERIFY2(contrast(c.text, c.raised) >= 4.5, what);
            QVERIFY2(contrast(c.muted, c.surface) >= 3.0, what);
            QVERIFY2(contrast(c.muted, c.window) >= 3.0, what);
            QVERIFY2(contrast(c.onAccent, c.accent) >= 4.5, what);
            // Disabled text dim but there; the grid seen on the paper; the
            // frames seen on the window.
            QVERIFY2(contrast(c.disabled, c.window) >= 2.0, what);
            QVERIFY2(contrast(c.disabled, c.window) < contrast(c.text, c.window), what);
            QVERIFY2(contrast(c.grid, c.paper) >= 1.6, what);
            QVERIFY2(contrast(c.border, c.window) >= 1.15, what);
            // Its palette: every role its own, the text lighter than the
            // window exactly when the theme is dark.
            const QPalette p = palette(c);
            QCOMPARE(p.color(QPalette::Window), c.window);
            QCOMPARE(p.color(QPalette::Base), c.base);
            QCOMPARE(p.color(QPalette::Highlight), c.accent);
            QCOMPARE(p.color(QPalette::HighlightedText), c.onAccent);
            QCOMPARE(p.color(QPalette::Mid), c.border);
            QCOMPARE(p.color(QPalette::Disabled, QPalette::Text), c.disabled);
            QVERIFY2(p.isBrushSet(QPalette::Inactive, QPalette::Highlight), what);
            QCOMPARE(p.color(QPalette::WindowText).value() > p.color(QPalette::Window).value(), d.dark);
            // Its style sheet names its colours.
            const QString sheet = styleSheet(c);
            QVERIFY2(sheet.contains(c.surface.name()) && sheet.contains(c.accent.name()), what);
            QVERIFY2(!swatch(d.id).isNull(), what);
        }
    }

    void aDesignedThemeDrawsWithItsOwnStyle()
    {
        ThemeGuard guard;
        const QString nativeName = QApplication::style()->name();
        QVERIFY(!designedInUse());
        apply(Nord);
        QVERIFY(designedInUse());
        QCOMPARE(current(), int(Nord));
        QVERIFY(designedStyleShown());
        QVERIFY(qApp->styleSheet().contains(designedTheme(Nord)->colours.accent.name()));
        QCOMPARE(QApplication::palette().color(QPalette::Window), designedTheme(Nord)->colours.window);
        QVERIFY(isDark());
        QVERIFY(misc::isDarkTheme());
        QCOMPARE(nativeStyle().toLower(), nativeName.toLower());   // what System will go back to

        // Another designed theme: the same style, its colours.
        apply(Daylight);
        QVERIFY(designedStyleShown());
        QVERIFY(!isDark());
        QVERIFY(qApp->styleSheet().contains(designedTheme(Daylight)->colours.surface.name()));
        QVERIFY(!qApp->styleSheet().contains(designedTheme(Nord)->colours.surface.name()));

        // A platform theme: its style back, no style sheet, the palette too.
        apply(System);
        QVERIFY(!designedInUse());
        QVERIFY(!designedStyleShown());
        QCOMPARE(QApplication::style()->name().toLower(), nativeName.toLower());
        QVERIFY(qApp->styleSheet().isEmpty());
        QCOMPARE(QApplication::palette().color(QPalette::Window), systemWindow);
        QCOMPARE(QApplication::palette().resolveMask(), QPalette::ResolveMask(0));

        // Dark and Light over a designed theme take the platform's style too.
        apply(Dracula);
        apply(Dark);
        QVERIFY(!designedStyleShown());
        QVERIFY(qApp->styleSheet().isEmpty());
        QVERIFY(isDark());
    }

    void theStyleForThePlatformThemesWaits()
    {
        // App Style chosen while a designed theme is on: kept for when a
        // platform theme comes back, not shown under the designed one.
        ThemeGuard guard;
        const QString nativeName = QApplication::style()->name();
        QString other;
        for (const QString& key : QStyleFactory::keys())
            if (key.compare(nativeName, Qt::CaseInsensitive) != 0) other = key;
        if (other.isEmpty()) QSKIP("only one style here");
        apply(Graphite);
        setNativeStyle(other);
        QVERIFY(designedStyleShown());
        QCOMPARE(nativeStyle(), other);
        apply(Light);
        QCOMPARE(QApplication::style()->name().toLower(), other.toLower());
        // Outside a designed theme it is shown at once.
        setNativeStyle(nativeName);
        QCOMPARE(QApplication::style()->name().toLower(), nativeName.toLower());
        setNativeStyle("NoSuchStyle");   // ignored
        QCOMPARE(QApplication::style()->name().toLower(), nativeName.toLower());
    }

    void thePaperAndGridFollowTheTheme()
    {
        ThemeGuard guard;
        PaperGuard paperGuard;
        const QColor gridSetting(25, 25, 25);
        QucsSettings.BGColor = QColor(255, 250, 225);
        for (const Designed& d : designed()) {
            QucsSettings.Theme = d.id;
            apply(d.id);
            QucsSettings.PaperFollowsTheme = true;
            QCOMPARE(misc::paperColor(), d.colours.paper);
            QCOMPARE(misc::gridColor(gridSetting), d.colours.grid);
            QucsSettings.PaperFollowsTheme = false;
            QCOMPARE(misc::paperColor(), QucsSettings.BGColor);
            QCOMPARE(misc::gridColor(gridSetting), gridSetting);   // on no dark paper, as it is
        }
        // The platform themes: dark paper in Dark, the background in Light.
        QucsSettings.PaperFollowsTheme = true;
        QucsSettings.Theme = Dark;
        apply(Dark);
        QCOMPARE(misc::paperColor(), qucs_s::ink::darkPaperColour());
        QCOMPARE(misc::gridColor(gridSetting), gridSetting);   // fitted to the paper where it is drawn
        QucsSettings.Theme = Light;
        apply(Light);
        QCOMPARE(misc::paperColor(), QucsSettings.BGColor);
    }

    void aButtonKeepsTheColourItShows()
    {
        // A colour picker is a button whose palette carries the colour:
        // the designed style fills a button with its palette's colour.
        ThemeGuard guard;
        apply(Nord);
        QWidget holder;
        auto* picker = new QPushButton(&holder);
        picker->setGeometry(0, 0, 120, 30);
        misc::setWidgetBackgroundColor(picker, QColor(200, 40, 40));
        auto* box = new QCheckBox(&holder);
        box->setGeometry(0, 40, 30, 30);
        box->setChecked(true);
        holder.resize(130, 80);
        const QImage img = holder.grab().toImage();
        const qreal r = img.devicePixelRatio();
        QCOMPARE(img.pixelColor(QPoint(60 * r, 15 * r)), QColor(200, 40, 40));
        // A checked box is the accent.
        int accentPixels = 0;
        const QColor accent = designedTheme(Nord)->colours.accent;
        for (int y = int(40 * r); y < int(70 * r); ++y)
            for (int x = 0; x < int(30 * r); ++x)
                if (img.pixelColor(x, y) == accent) ++accentPixels;
        QVERIFY(accentPixels > 20);
    }

    void theSettingsDialogListsEveryTheme()
    {
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        {
            QucsSettingsDialog dlg(&app);
            QComboBox* combo = themeCombo(&dlg);
            QComboBox* style = styleCombo(&dlg);
            QVERIFY(combo != nullptr && style != nullptr);
            QCOMPARE(offered(combo), themes());
            QCOMPARE(combo->count(), themes().size() + 2);   // the designed light ones and dark ones apart
            QVERIFY(!combo->itemIcon(combo->findData(int(Nord))).isNull());
            QVERIFY(style->isEnabled());
            combo->setCurrentIndex(combo->findData(int(Nord)));
            QVERIFY(!style->isEnabled());   // a designed theme draws with its own
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QCOMPARE(QucsSettings.Theme, int(Nord));
            QCOMPARE(_settings::Get().item<int>("Theme"), int(Nord));
            QVERIFY(designedInUse());
            QVERIFY(QucsSettings.hasDarkTheme);
        }
        {
            QucsSettingsDialog dlg(&app);
            QComboBox* combo = themeCombo(&dlg);
            QCOMPARE(combo->currentData().toInt(), int(Nord));
            QVERIFY(!styleCombo(&dlg)->isEnabled());
            combo->setCurrentIndex(combo->findData(int(Light)));
            QVERIFY(styleCombo(&dlg)->isEnabled());
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QCOMPARE(QucsSettings.Theme, int(Light));
            QVERIFY(!designedInUse());
        }
    }

    void viewThemeSwitchesAtOnce()
    {
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        QActionGroup* group = nullptr;
        for (QActionGroup* g : app.findChildren<QActionGroup*>())
            if (g->actions().size() == themes().size()) group = g;
        QVERIFY(group != nullptr);
        QList<int> listed;
        for (QAction* a : group->actions()) listed << a->data().toInt();
        QCOMPARE(listed, themes());
        QVERIFY(group->checkedAction() != nullptr);
        QCOMPARE(group->checkedAction()->data().toInt(), QucsSettings.Theme);
        QAction* dracula = group->actions().at(themes().indexOf(Dracula));
        QCOMPARE(dracula->text(), QString("Dracula"));
        dracula->trigger();
        QCOMPARE(QucsSettings.Theme, int(Dracula));
        QCOMPARE(_settings::Get().item<int>("Theme"), int(Dracula));
        QVERIFY(designedInUse());
        QVERIFY(dracula->isChecked());
        QVERIFY(QucsSettings.hasDarkTheme);
        group->actions().at(0)->trigger();   // System
        QCOMPARE(QucsSettings.Theme, int(System));
        QVERIFY(!designedInUse());
        QVERIFY(group->actions().at(0)->isChecked());
    }

    void theComponentListFollowsTheTheme()
    {
        // The list takes the theme's colours; on a dark list the icons,
        // drawn in dark blue for white, are inked light.
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        app.setAttribute(Qt::WA_DontShowOnScreen);
        app.resize(1000, 700);
        app.show();
        QListWidget* list = componentList(app);
        QVERIFY(list != nullptr);
        QVERIFY(list->count() > 0);
        auto look = [&] {
            QCoreApplication::processEvents();
            return list->viewport()->grab().toImage();
        };
        app.applyTheme(Daylight);
        QVERIFY(list->styleSheet().contains(designedTheme(Daylight)->colours.base.name()));
        QVERIFY(darkBlue(look(), designedTheme(Daylight)->colours.base).size() > 50);
        app.applyTheme(Nord);
        QVERIFY(list->styleSheet().contains(designedTheme(Nord)->colours.base.name()));
        const QImage nord = look();
        const QList<QPoint> left = darkBlue(nord, designedTheme(Nord)->colours.base);
        QVERIFY2(left.isEmpty(), describe(nord, left).constData());
        QCOMPARE(nord.pixelColor(2, 2), designedTheme(Nord)->colours.base);
        app.applyTheme(System);
        QCOMPARE(list->palette().color(QPalette::Base).rgba(), QApplication::palette().color(QPalette::Base).rgba());
    }

    void theTextEditorTakesADesignedThemesColours()
    {
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        app.resize(900, 600);
        app.setAttribute(Qt::WA_DontShowOnScreen);
        app.show();
        const QString file = dir.filePath("colours.va");
        { QFile f(file); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("module m(a);\n// a comment\nendmodule\n"); }
        QVERIFY(app.gotoPage(file, false, false));
        TextDoc* doc = qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        auto background = [&] {
            QCoreApplication::processEvents();
            const QImage img = doc->viewport()->grab().toImage();
            return img.pixelColor(img.width() - 4, img.height() - 4);
        };
        app.applyTheme(Dracula);
        QCOMPARE(background(), designedTheme(Dracula)->colours.base);
        // Nothing drawn in the colours meant for white is left dark on it.
        const QImage dracula = doc->viewport()->grab().toImage();
        const QList<QPoint> left = darkBlue(dracula, designedTheme(Dracula)->colours.base);
        QVERIFY2(left.isEmpty(), describe(dracula, left).constData());
        app.applyTheme(SolarizedLight);
        QCOMPARE(background(), designedTheme(SolarizedLight)->colours.base);
        app.applyTheme(Dark);   // a platform theme: black on white, as before
        QCOMPARE(background(), QColor(Qt::white));
    }

    void everyTabKeepsItsCloseButton()
    {
        // On macOS a tab's "modified" marker went into the right-hand
        // button place, where the macOS style has no close button - and
        // where Fusion and the designed themes have it: new tabs had none.
        // A tab opened before a theme switch kept its close button on the
        // old side, where Qt takes no click on it.
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        app.setAttribute(Qt::WA_DontShowOnScreen);
        app.resize(1000, 700);
        app.show();
        const QString features = QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/NGspice features/";
        QVERIFY(app.gotoPage(features + "RC_lowpass_montecarlo.sch", false, false));
        app.applyTheme(Nord);
        QVERIFY(app.gotoPage(features + "LC_lowpass_ngopt.sch", false, false));
        QTabBar* bar = app.DocumentTab->tabBar();
        QVERIFY(bar->count() >= 2);
        // Tabs with a close button, shown, in the place the style has for
        // it; -1 when a close button is in the other place.
        auto closeButtons = [&] {
            int n = 0;
            const QTabBar::ButtonPosition side = closeSide(bar);
            const QTabBar::ButtonPosition other = side == QTabBar::LeftSide ? QTabBar::RightSide : QTabBar::LeftSide;
            for (int i = 0; i < bar->count(); ++i) {
                if (qobject_cast<QAbstractButton*>(bar->tabButton(i, other)) != nullptr) return -1;
                auto* close = qobject_cast<QAbstractButton*>(bar->tabButton(i, side));
                if (close != nullptr && close->isVisible()) ++n;
            }
            return n;
        };
        QCOMPARE(closeButtons(), bar->count());
        // A style that closes on the left, and back.
        ClosesOnTheLeft left;
        bar->setStyle(&left);
        QCOMPARE(closeSide(bar), QTabBar::LeftSide);
        app.applyLook();
        QCOMPARE(closeButtons(), bar->count());
        bar->setStyle(nullptr);
        app.applyLook();
        QCOMPARE(closeSide(bar), QTabBar::RightSide);
        QCOMPARE(closeButtons(), bar->count());
        app.applyTheme(System);
        QCOMPARE(closeButtons(), bar->count());
        app.applyTheme(Daylight);
        QCOMPARE(closeButtons(), bar->count());
        // A click on one closes its document.
        const int before = bar->count();
        auto* close = qobject_cast<QAbstractButton*>(bar->tabButton(before - 1, closeSide(bar)));
        QVERIFY(close != nullptr);
        close->click();
        QTRY_COMPARE(bar->count(), before - 1);
    }

    void theFallbackPalettesAreReadable()
    {
        // What a platform that cannot switch its appearance gets.
        const QPalette dark = darkPalette();
        QVERIFY(dark.color(QPalette::Window).value() < 0x60);
        QVERIFY(dark.color(QPalette::Base).value() < 0x60);
        QVERIFY(dark.color(QPalette::Text).value() > 0xc0);
        QVERIFY(dark.color(QPalette::WindowText).value() > 0xc0);
        QVERIFY(dark.color(QPalette::ButtonText).value() > 0xc0);
        QVERIFY(dark.color(QPalette::HighlightedText).value() > 0xc0);
        // Disabled text is dimmer than enabled text, still readable.
        QVERIFY(dark.color(QPalette::Disabled, QPalette::Text).value() < dark.color(QPalette::Text).value());
        QVERIFY(dark.color(QPalette::Disabled, QPalette::Text).value() > dark.color(QPalette::Window).value() + 0x30);
        // Every role is the palette's own: nothing of the platform's shows through.
        QVERIFY(dark.isBrushSet(QPalette::Active, QPalette::Window));
        QVERIFY(dark.isBrushSet(QPalette::Disabled, QPalette::Text));

        const QPalette light = lightPalette();
        QVERIFY(light.color(QPalette::Window).value() > 0xc0);
        QVERIFY(light.color(QPalette::Base).value() > 0xe0);
        QVERIFY(light.color(QPalette::Text).value() < 0x40);
        QVERIFY(light.color(QPalette::WindowText).value() < 0x40);
    }

    void darkLightAndBackToTheSystem()
    {
        ThemeGuard guard;
        apply(Dark);
        QVERIFY(isDark());
        QVERIFY(misc::isDarkTheme());

        apply(Light);
        QVERIFY(!isDark());
        const QPalette light = QApplication::palette();

        // Applying the same theme again changes nothing.
        apply(Light);
        QCOMPARE(QApplication::palette(), light);

        apply(System);
        QCOMPARE(QApplication::palette().color(QPalette::Window), systemWindow);
        // Nothing of ours is left on the palette: the platform's colours
        // come through again, now and when the system changes them.
        QCOMPARE(QApplication::palette().resolveMask(), QPalette::ResolveMask(0));
        QVERIFY(!QCoreApplication::testAttribute(Qt::AA_SetPalette));

        // An unknown value is the system's look.
        apply(Dark);
        apply(42);
        QCOMPARE(QApplication::palette().color(QPalette::Window), systemWindow);
    }

    void whereThePlatformCannotSwitchThePaletteDoesIt()
    {
        // The offscreen platform answers no request for a colour scheme,
        // so the palette is the way; where it does (Cocoa, Windows) no
        // palette is forced and the native controls follow instead.
        ThemeGuard guard;
        apply(Dark);
        const bool platformSwitched = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        if (platformSwitched)
            QVERIFY(!QCoreApplication::testAttribute(Qt::AA_SetPalette));
        else
            QVERIFY(QCoreApplication::testAttribute(Qt::AA_SetPalette));
        QVERIFY(isDark());
    }

    void theSettingIsLoadedBoundedAndSaved()
    {
        ThemeGuard guard;
        QCOMPARE(_settings::Get().itemDefault<int>("Theme"), int(System));
        _settings::Get().setItem<int>("Theme", 42);   // no theme
        QCOMPARE(bounded(_settings::Get().item<int>("Theme")), int(System));   // as loadSettings() does
        QucsSettings.Theme = Dark;
        saveApplSettings();
        QCOMPARE(_settings::Get().item<int>("Theme"), int(Dark));
    }

    void theTextEditorKeepsItsBackgroundInTheDarkTheme()
    {
        // The editor is black on white whatever the theme, and whatever
        // the schematic's document background. (The main window has a
        // style sheet, and the style-sheet style used to put the theme's
        // base colour back on the editor each time it was shown.)
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        app.resize(900, 600);
        app.show();
        const QString file = dir.filePath("colours.txt");
        { QFile f(file); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("line one\nline two\n"); }
        QVERIFY(app.gotoPage(file, false, false));
        TextDoc* doc = qobject_cast<TextDoc*>(app.DocumentTab->currentWidget());
        QVERIFY(doc != nullptr);
        auto background = [&] {
            QCoreApplication::processEvents();
            const QImage img = doc->viewport()->grab().toImage();
            return img.pixelColor(img.width() - 4, img.height() - 4);   // below the last line
        };
        const QColor paper(Qt::white);
        QVERIFY(QucsSettings.BGColor != paper);   // the schematic's paper is another colour
        QCOMPARE(background(), paper);
        apply(Dark);
        QTest::qWait(50);
        QVERIFY(isDark());
        QCOMPARE(background(), paper);
        // Dark text on the light paper, not the theme's light text.
        const QImage img = doc->viewport()->grab().toImage();
        int darkPixels = 0;
        for (int y = 0; y < qMin(img.height(), 20); ++y)
            for (int x = 0; x < qMin(img.width(), 80); ++x)
                if (img.pixelColor(x, y).value() < 0x40) ++darkPixels;
        QVERIFY(darkPixels > 0);
        // The schematic's document background does not reach the editor.
        struct BgGuard { QColor c = QucsSettings.BGColor; ~BgGuard() { QucsSettings.BGColor = c; } } bgGuard;
        QucsSettings.BGColor = QColor(0x20, 0x20, 0x40);
        doc->applyDocumentColors();
        QCOMPARE(background(), paper);
        apply(System);
        QTest::qWait(50);
        QCOMPARE(background(), paper);
    }

    void theSettingsDialogHasTheThemeChoice()
    {
        ThemeGuard guard;
        QucsApp app(false);
        MainGuard mainGuard(&app);
        {
            QucsSettingsDialog dlg(&app);
            QComboBox* combo = themeCombo(&dlg);
            QVERIFY(combo != nullptr);
            QCOMPARE(combo->itemText(0), QString("System"));
            QCOMPARE(combo->itemText(1), QString("Dark"));
            QCOMPARE(combo->itemText(2), QString("Light"));
            QCOMPARE(combo->currentData().toInt(), int(System));   // what is set now
            combo->setCurrentIndex(1);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QCOMPARE(QucsSettings.Theme, int(Dark));
            QCOMPARE(_settings::Get().item<int>("Theme"), int(Dark));   // saved
            QVERIFY(isDark());
            QVERIFY(QucsSettings.hasDarkTheme);
            // The windows follow: Qt posts them the palette change.
            QTRY_VERIFY(app.palette().color(QPalette::Window).value() < 0x60);
        }
        {
            // Opened again it shows the choice made; Defaults puts System back.
            QucsSettingsDialog dlg(&app);
            QComboBox* combo = themeCombo(&dlg);
            QVERIFY(combo != nullptr);
            QCOMPARE(combo->currentData().toInt(), int(Dark));
            QPushButton* defaults = button(&dlg, "Default Values");
            QVERIFY(defaults != nullptr);
            defaults->click();
            QCOMPARE(combo->currentData().toInt(), int(System));
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
            QCOMPARE(QucsSettings.Theme, int(System));
            QCOMPARE(isDark(), systemIsDark);
            QCOMPARE(QucsSettings.hasDarkTheme, systemIsDark);
            QTRY_COMPARE(app.palette().color(QPalette::Window), systemWindow);
        }
    }

    // For a look at every theme: QUCS_TEST_GRAB=<dir> saves the main window
    // with an example open (its paper following the theme), the View menu
    // and the settings dialog, in each. Run without QT_QPA_PLATFORM=offscreen
    // to see the platform's own style too.
    void gallery()
    {
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (grabDir.isEmpty()) QSKIP("QUCS_TEST_GRAB not set");
        ThemeGuard guard;
        struct PaperGuard { bool p = QucsSettings.PaperFollowsTheme; ~PaperGuard() { QucsSettings.PaperFollowsTheme = p; } } paperGuard;
        QucsSettings.PaperFollowsTheme = true;
        const QString example = QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/NGspice features/RC_lowpass_montecarlo.sch";
        QVERIFY(QFile::exists(example));
        const QString only = qEnvironmentVariable("QUCS_TEST_THEME");   // one theme's name, or all
        for (int theme : themes()) {
            const QString file = name(theme).toLower().replace(' ', '-');
            if (!only.isEmpty() && file != only) continue;
            QucsApp app(false);
            MainGuard mainGuard(&app);
            app.applyTheme(theme);
            app.setAttribute(Qt::WA_DontShowOnScreen);
            app.resize(1440, 900);
            app.show();
            QVERIFY(app.gotoPage(example, false, false));
            app.applyLook();
            QTest::qWait(2300);   // past the "Ready." of the status bar
            app.grab().save(grabDir + "/" + file + "-main.png");
            // Two documents: the tabs and their close buttons.
            const QString second = QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/NGspice features/LC_lowpass_ngopt.sch";
            QVERIFY(app.gotoPage(second, false, false));
            QTest::qWait(100);
            {
                QTabBar* bar = app.DocumentTab->tabBar();
                app.grab(QRect(app.DocumentTab->mapTo(&app, QPoint(0, 0)), QSize(700, bar->height() + 6)))
                    .save(grabDir + "/" + file + "-tabs.png");
            }
            // A Verilog-A source in the editor, the simulation console below.
            const QString va = QStringLiteral(QUCS_EXAMPLES_DIR) + "/ngspice/OpenVAF/Tunnel_Ngspice_prj/tunnel.va";
            QVERIFY(app.gotoPage(va, false, false));
            app.simulationConsole()->viewAction()->setChecked(true);
            QTest::qWait(300);
            app.grab().save(grabDir + "/" + file + "-editor.png");
            // The side panel's other tabs.
            for (QTabWidget* side : app.findChildren<QTabWidget*>())
                if (side->tabPosition() == QTabWidget::West) {
                    QImage strip;
                    for (int i = 0; i < side->count(); ++i) {
                        side->setCurrentIndex(i);
                        QTest::qWait(100);
                        const QImage tab = side->grab().toImage();
                        if (strip.isNull()) {
                            strip = QImage(tab.width() * side->count(), tab.height(), tab.format());
                            strip.setDevicePixelRatio(tab.devicePixelRatio());
                        }
                        QPainter p(&strip);
                        p.drawImage(QPointF(i * tab.width() / tab.devicePixelRatio(), 0), tab);
                    }
                    strip.save(grabDir + "/" + file + "-side.png");
                    side->setCurrentIndex(2);
                }
            for (QAction* a : app.menuBar()->actions())
                if (a->menu() != nullptr && a->text().remove('&') == "View") {
                    QMenu* menu = a->menu();
                    menu->setAttribute(Qt::WA_DontShowOnScreen);
                    menu->popup(QPoint(0, 0));
                    QTest::qWait(50);
                    menu->grab().save(grabDir + "/" + file + "-menu.png");
                    menu->hide();
                }
            QucsSettingsDialog dlg(&app);
            dlg.setAttribute(Qt::WA_DontShowOnScreen);
            dlg.show();
            if (auto* tabs = dlg.findChild<QTabWidget*>()) tabs->setCurrentIndex(1);
            QTest::qWait(100);
            dlg.grab().save(grabDir + "/" + file + "-settings.png");
        }
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestAppTheme test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_app_theme.moc"
