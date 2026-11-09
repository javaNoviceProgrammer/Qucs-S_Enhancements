/*
 * The tool buttons with a menu - Reopen Closed Tab on the Running Jobs
 * toolbar, the pens of the layout viewer and of the PPTX viewer, and every
 * other one on a toolbar or a viewer's bar: the menu's arrow beside the
 * icon and its text, never on them, in every theme and style, at one and
 * two device pixels a pixel, with the icon of the button's size and larger
 * ones, with and without the text beside it.
 *
 * Qt's style-sheet style keeps no room for the arrow on a tool button with
 * a border or padding of its own (a toolbar's under a designed theme, a
 * viewer's bar's, every one of the main window's on macOS): it lays the
 * label out across the whole button and draws the arrow over its right
 * edge. The room is kept by the style sheets (apptheme::menuButtonRules()).
 *
 * Each button is painted by its style as its paint event would, the button
 * itself left alone, at the width a bar gives it (its size hint): with an
 * opaque square for its icon and with a transparent one; with its text and
 * without; with its menu and without. What the opaque icon changes is the
 * icon as it shows - all of it, a square, or the arrow's part lies on it;
 * what the menu adds, the icon and text away, is the arrow - none of it
 * where the icon or the text is.
 */
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOptionToolButton>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "apptheme.h"
#include "settings.h"
#include "layoutdoc.h"
#include "pptxdoc.h"
#include "pdfdoc.h"
#include "imagedoc.h"
#include "sheetdoc.h"
#include "zipdoc.h"
#include "markup.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

using namespace qucs_s::apptheme;

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

struct ThemeGuard {
    int theme = QucsSettings.Theme;
    QString native = nativeStyle();
    ~ThemeGuard()
    {
        QucsSettings.Theme = theme;
        apply(System);
        setNativeStyle(native);
    }
};

bool hasMenu(const QToolButton* b)
{
    return b->menu() != nullptr || (b->defaultAction() != nullptr && b->defaultAction()->menu() != nullptr);
}

// The pixels \a in where \a keep holds, and the rectangle around them.
struct Pixels {
    QRect box;
    int count = 0;
    QImage mask;   // (white where kept)
};

template <typename Keep>
Pixels pixels(const QSize& size, Keep keep)
{
    Pixels p;
    p.mask = QImage(size, QImage::Format_Mono);
    p.mask.fill(0);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            if (keep(x, y)) {
                p.mask.setPixel(x, y, 1);
                p.box |= QRect(x, y, 1, 1);
                ++p.count;
            }
    return p;
}

// The style option \a b paints itself with (QToolButton's own is protected:
// reached through a pointer to the member, called on the button as it is -
// cast to this class, which it is not, GCC's UBSan ended the test on Linux).
struct OptionOf : QToolButton {
    static QStyleOptionToolButton of(const QToolButton* b)
    {
        QStyleOptionToolButton opt;
        void (QToolButton::*init)(QStyleOptionToolButton*) const = &OptionOf::initStyleOption;
        (b->*init)(&opt);
        return opt;
    }
};

// \a b painted by its style with \a opt, on nothing, at \a ratio device
// pixels a pixel: as its paint event does, the button left as it is.
QImage drawn(const QToolButton* b, const QStyleOptionToolButton& opt, qreal ratio)
{
    QImage image(opt.rect.size() * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    b->style()->drawComplexControl(QStyle::CC_ToolButton, &opt, &painter, b);
    return image;
}

// An icon of \a size all \a colour, as a real one: a pixmap for one and
// for two device pixels a pixel, in every mode and state.
QIcon square(const QSize& size, const QColor& colour)
{
    QIcon icon;
    for (qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(size * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(colour);
        for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled, QIcon::Active, QIcon::Selected})
            for (QIcon::State state : {QIcon::Off, QIcon::On}) icon.addPixmap(pixmap, mode, state);
    }
    return icon;
}

// What a button is put back to: its size is asked with other icons and text.
struct Kept {
    QToolButton* b;
    QString text;
    QSize iconSize;
    Qt::ToolButtonStyle style;
    explicit Kept(QToolButton* button)
        : b(button), text(button->text()), iconSize(button->iconSize()), style(button->toolButtonStyle())
    {
    }
    ~Kept()
    {
        const QSignalBlocker block(b);
        b->setText(text);
        b->setIconSize(iconSize);
        b->setToolButtonStyle(style);
    }
};

// What is wrong with \a b drawn so, or nothing. \a icon: with an icon
// (\a iconSize), \a withText: with its text (beside the icon).
QString lookAt(QToolButton* b, qreal ratio, bool icon, const QSize& iconSize, bool withText)
{
    {
        const QSignalBlocker block(b);
        b->setIconSize(iconSize);
        b->setToolButtonStyle(!icon ? Qt::ToolButtonTextOnly
                                    : withText ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
        if (withText && b->text().isEmpty()) b->setText(QStringLiteral("Menu"));
    }
    // At rest: not hovered, pressed nor checked; the width a bar's layout gives it.
    QStyleOptionToolButton opt = OptionOf::of(b);
    opt.rect = QRect(QPoint(), b->sizeHint());
    opt.state &= ~(QStyle::State_MouseOver | QStyle::State_Sunken | QStyle::State_On | QStyle::State_HasFocus);
    if (b->autoRaise()) opt.state &= ~QStyle::State_Raised;   // (raised while the pointer is on it)
    opt.activeSubControls = QStyle::SC_None;
    QStyleOptionToolButton bare = opt;   // no menu: no arrow
    bare.features &= ~(QStyleOptionToolButton::Menu | QStyleOptionToolButton::HasMenu
                       | QStyleOptionToolButton::MenuButtonPopup);
    bare.subControls &= ~QStyle::SC_ToolButtonMenu;

    const auto with = [&](QStyleOptionToolButton o, const QIcon& i) {
        o.icon = i;
        return drawn(b, o, ratio);
    };
    const QIcon clearIcon = square(iconSize, Qt::transparent);
    const QImage clear = with(opt, icon ? clearIcon : QIcon());
    const QSize size = clear.size();
    const auto differ = [&](const QImage& one, const QImage& two) {
        return pixels(size, [&](int x, int y) { return one.pixel(x, y) != two.pixel(x, y); });
    };

    // The text: what it adds (with the menu, in the same rectangle).
    const QIcon none = icon ? clearIcon : QIcon();
    QStyleOptionToolButton mute = opt, bareMute = bare;
    mute.text.clear();
    bareMute.text.clear();
    Pixels text;
    if (withText) {
        text = differ(with(opt, none), with(mute, none));
        if (text.count == 0) return QStringLiteral("no text drawn");
    }
    // The arrow: what the menu adds, the icon and text away (where they
    // would be, whatever it lies on).
    const Pixels arrow = differ(with(mute, none), with(bareMute, none));

    if (const QString grab = qEnvironmentVariable("QUCS_TEST_GRAB_BUTTONS"); !grab.isEmpty()) {
        const QString stem = QStringLiteral("%1/%2-%3-%4x-%5%6")
                                 .arg(grab, name(current()) + "-" + nativeStyle()
                                                + (b->parentWidget() ? "-" + b->parentWidget()->objectName() : QString()),
                                      b->objectName())
                                 .arg(ratio).arg(iconSize.width()).arg(withText ? "-text" : "");
        with(opt, square(iconSize, QColor(220, 30, 200))).save(stem + ".png");
        with(opt, none).save(stem + "-clear.png");
    }
    // (Qt's macOS style draws no arrow off the screen: the check is only
    // that nothing of the menu lies on the icon or the text.)
    const bool drawsArrows = designedInUse() || !nativeStyle().contains("mac", Qt::CaseInsensitive);
    if (b->popupMode() == QToolButton::MenuButtonPopup && arrow.count == 0 && drawsArrows)
        return QStringLiteral("no arrow drawn");

    const auto onBox = [&](const QRect& box) {
        int on = 0;
        for (int y = box.top(); y <= box.bottom(); ++y)
            for (int x = box.left(); x <= box.right(); ++x) on += arrow.mask.pixelIndex(x, y);
        return on;
    };
    // And where the style says the arrow's part is, drawn or not.
    QRect part;
    if (opt.subControls & QStyle::SC_ToolButtonMenu) {
        const QRect r = b->style()->subControlRect(QStyle::CC_ToolButton, &opt, QStyle::SC_ToolButtonMenu, b);
        part = QRect((QPointF(r.topLeft()) * ratio).toPoint(), (QSizeF(r.size()) * ratio).toSize());
    }
    if (icon) {
        // The icon: what an opaque square changes - all of it, a square
        // (nothing on it, no edge cut off), and under the designed themes'
        // style sheet as large as without the menu. (A platform style may
        // draw it a little smaller in a tight button - Fusion's 21 pixels
        // in 19 -: whole, not the menu's doing.)
        const QIcon solid = square(iconSize, QColor(220, 30, 200));
        const Pixels shown = differ(with(opt, solid), clear);
        const Pixels alone = differ(with(bare, solid), with(bare, clearIcon));
        const bool whole = shown.count > 0 && shown.count == shown.box.width() * shown.box.height()
                        && std::abs(shown.box.width() - shown.box.height()) <= 1;
        if (!whole || (designedInUse() && shown.box.size() != alone.box.size()))
            return QStringLiteral("the icon shows %1 pixels in %2 x %3; %4 x %5 without the menu")
                .arg(shown.count).arg(shown.box.width()).arg(shown.box.height())
                .arg(alone.box.width()).arg(alone.box.height());
        if (const int on = onBox(shown.box)) return QStringLiteral("the arrow on the icon: %1 pixels").arg(on);
        if (part.intersects(shown.box)) return QStringLiteral("the arrow's part on the icon");
    }
    if (withText) {
        if (const int on = onBox(text.box)) return QStringLiteral("the arrow on the text: %1 pixels").arg(on);
        if (part.intersects(text.box)) return QStringLiteral("the arrow's part on the text");
    }
    return {};
}

QString nameOf(const QToolButton* b)
{
    QString name = b->objectName();
    if (name.isEmpty() && b->defaultAction() != nullptr) name = b->defaultAction()->objectName();
    if (name.isEmpty()) name = b->toolTip().left(40);
    if (name.isEmpty()) name = b->text();
    for (const QObject* o = b->parent(); o != nullptr; o = o->parent())
        if (!o->objectName().isEmpty()) return o->objectName() + "/" + name;
    return name;
}

} // namespace

class TestMenuButtons : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    // The tool buttons with a menu of the main window's toolbars and the
    // viewers' bars, each where it is (the viewers' on their own).
    QList<QToolButton*> buttonsOf(const QList<QWidget*>& roots)
    {
        QList<QToolButton*> all;
        for (QWidget* root : roots)
            for (QToolButton* b : root->findChildren<QToolButton*>()) {
                // (Qt's own - a menu bar's extension - are not ours.)
                if (!hasMenu(b) || all.contains(b) || b->objectName().startsWith("qt_")) continue;
                b->ensurePolished();
                all << b;
            }
        return all;
    }

    // Every button of \a roots, drawn every way: what is wrong, a line each.
    QStringList check(const QList<QWidget*>& roots, const QString& look)
    {
        QStringList wrong;
        for (QToolButton* b : buttonsOf(roots)) {
            const Kept kept(b);
            const bool icon = !b->icon().isNull() && b->toolButtonStyle() != Qt::ToolButtonTextOnly;
            const int own = b->iconSize().width();
            for (qreal ratio : {1.0, 2.0})
                for (int side : icon ? QList<int>{own, 16, 24, 32} : QList<int>{own})
                    for (bool text : icon ? QList<bool>{false, true} : QList<bool>{true}) {
                        const QString problem = lookAt(b, ratio, icon, QSize(side, side), text);
                        if (!problem.isEmpty())
                            wrong << QStringLiteral("%1 (%2, %3x, %4%5): %6")
                                         .arg(nameOf(b), look).arg(ratio)
                                         .arg(icon ? QStringLiteral("%1 px").arg(side) : QStringLiteral("no icon"))
                                         .arg(text ? QStringLiteral(", text") : QString(), problem);
                    }
        }
        wrong.removeDuplicates();
        return wrong;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.Theme = System;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // The ones the arrow covered, there to be checked.
    void theButtonsAreFound()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        LayoutDoc layout(&app, dir.filePath("none.gds"));
        PptxDoc deck(&app, dir.filePath("none.pptx"));
        QStringList names;
        for (QToolButton* b : buttonsOf({&app, &layout, &deck})) names << b->objectName();
        QVERIFY2(names.contains("reopenClosedButton"), qPrintable(names.join(", ")));
        QCOMPARE(names.count("penButton"), 2);
    }

    void theArrowIsBesideTheIcon_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<QString>("native");   // the platform themes' style
        for (const QString& style : QStyleFactory::keys())
            QTest::newRow(qPrintable("System, " + style)) << int(System) << style;
        for (int theme : themes())
            if (theme != System) QTest::newRow(qPrintable(name(theme))) << theme << QString();
    }

    void theArrowIsBesideTheIcon()
    {
        QFETCH(int, theme);
        QFETCH(QString, native);
        ThemeGuard themeGuard;
        if (!native.isEmpty()) setNativeStyle(native);
        QucsApp app(false);
        MainGuard guard(&app);
        app.applyTheme(theme);
        LayoutDoc layout(&app, dir.filePath("none.gds"));
        PptxDoc deck(&app, dir.filePath("none.pptx"));
        PdfDoc pdf(&app, dir.filePath("none.pdf"));
        ImageDoc image(&app, dir.filePath("none.png"));
        SheetDoc sheet(&app, dir.filePath("none.xlsx"));
        ZipDoc zip(&app, dir.filePath("none.zip"));
        QList<QWidget*> roots{&app, &layout, &deck, &pdf, &image, &sheet, &zip};
        for (QWidget* w : roots) {
            w->setAttribute(Qt::WA_DontShowOnScreen);
            w->resize(1200, 800);
            w->show();
        }
        QTest::qWait(50);   // (the viewers' restyle, a palette change later)
        const QStringList wrong = check(roots, native.isEmpty() ? name(theme) : native);
        QVERIFY2(wrong.isEmpty(), qPrintable("\n" + wrong.join('\n')));
    }

    // For a look: QUCS_TEST_GRAB=<dir> saves the Running Jobs toolbar and
    // the layout and PPTX viewers' bars in a few themes (QT_SCALE_FACTOR=2
    // for two device pixels a pixel).
    void gallery()
    {
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (grabDir.isEmpty()) QSKIP("QUCS_TEST_GRAB not set");
        for (int theme : {int(System), int(Light), int(Dark), int(Daylight), int(Nord), int(Dracula)}) {
            ThemeGuard themeGuard;
            QucsApp app(false);
            MainGuard guard(&app);
            app.applyTheme(theme);
            LayoutDoc layout(&app, dir.filePath("none.gds"));
            PptxDoc deck(&app, dir.filePath("none.pptx"));
            for (QWidget* w : QList<QWidget*>{&app, &layout, &deck}) {
                w->setAttribute(Qt::WA_DontShowOnScreen);
                w->resize(1400, 800);
                w->show();
            }
            QTest::qWait(100);
            const QString stem = grabDir + "/" + name(theme).toLower().replace(' ', '-');
            // Each button with what is either side of it.
            const auto around = [&](QWidget* button, const QString& file) {
                if (button == nullptr) return;
                QWidget* bar = button->parentWidget();
                const QRect r = button->geometry().adjusted(-90, 0, 40, 0) & bar->rect();
                bar->grab(QRect(r.x(), 0, r.width(), bar->height())).save(stem + file);
            };
            around(app.runsToolbar->findChild<QToolButton*>("reopenClosedButton"), "-jobs.png");
            around(layout.penButton(), "-layout.png");
            around(deck.penButton(), "-pptx.png");
        }
    }

    // A button without a menu is as it was: no room kept for an arrow.
    void aButtonWithoutAMenuKeepsItsSize()
    {
        ThemeGuard themeGuard;
        QucsApp app(false);
        MainGuard guard(&app);
        app.applyTheme(Nord);
        LayoutDoc layout(&app, dir.filePath("none.gds"));
        layout.setAttribute(Qt::WA_DontShowOnScreen);
        layout.show();
        QTest::qWait(50);
        for (QToolButton* b : layout.findChildren<QToolButton*>()) {
            if (hasMenu(b) || b->icon().isNull() || b->toolButtonStyle() != Qt::ToolButtonIconOnly) continue;
            const int side = b->iconSize().width();
            QVERIFY2(b->sizeHint().width() <= side + 12, qPrintable(nameOf(b)));
        }
        for (QToolBar* bar : app.findChildren<QToolBar*>())
            for (QAction* a : bar->actions())
                if (auto* b = qobject_cast<QToolButton*>(bar->widgetForAction(a)))
                    if (!hasMenu(b) && b->toolButtonStyle() == Qt::ToolButtonIconOnly)
                        QVERIFY2(b->sizeHint().width() <= b->iconSize().width() + 12, qPrintable(nameOf(b)));
    }
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    // QUCS_TEST_GRAB_PLATFORM=cocoa (with QUCS_TEST_GRAB): the gallery's
    // pictures in the platform's own style, at its screen's ratio - ctest
    // puts every test offscreen, where Qt's macOS style draws no arrow.
    // (Nothing is shown on the screen: the windows are not.)
    if (const QByteArray platform = qgetenv("QUCS_TEST_GRAB_PLATFORM"); !platform.isEmpty())
        qputenv("QT_QPA_PLATFORM", platform);
    QApplication app(one, argv);
    TestMenuButtons test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_menu_buttons.moc"
