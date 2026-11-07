/*
 * The real Qucs-S, its real settings dialog, the real NSFontPanel: this library,
 * put in with DYLD_INSERT_LIBRARIES, opens Application Settings > Appearance,
 * presses a font button and works the panel with AppKit events posted to NSApp:
 * a click in the size box, Delete, the digits, Return, a click on the red button.
 * (From inside, as the shell has neither Accessibility nor Screen Recording.)
 *
 *   DRIVE_BUTTON   0 Schematic, 1 Application, 2 Text document font
 *   DRIVE_SIZE     the size to type; empty: touch nothing, just close the panel
 *   DRIVE_CLOSE    red (default) | esc | return | none (leave it for a person)
 *   DRIVE_END      apply | ok | cancel, the settings window's button afterwards
 *   DRIVE_CLICKS   clicks in the panel before it closes, "x,y;x,y", points from its top left
 *   DRIVE_POPUP    1: open the size box's list with its arrow and click the middle of it
 *   DRIVE_OUT      folder for pictures; a line per step goes to drive.log there
 */
#import <AppKit/AppKit.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <unistd.h>
#include <cstdio>
#include <functional>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QGridLayout>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QLayoutItem>

static QString env(const char *n, const char *d = "") { return qEnvironmentVariableIsSet(n) ? qEnvironmentVariable(n) : QString::fromUtf8(d); }
// In a file of its own: the app takes stderr for itself.
static void say(const QString &s) {
    static FILE *log = fopen((env("DRIVE_OUT", ".") + "/drive.log").toUtf8().constData(), "w");
    if (log) { fprintf(log, "%s\n", s.toUtf8().constData()); fflush(log); }
}

static NSView *findView(NSView *v, const char *needle) {
    if (strstr(v.className.UTF8String, needle)) return v;
    for (NSView *s in v.subviews) if (NSView *r = findView(s, needle)) return r;
    return nil;
}
static NSFontPanel *panel() { return [NSFontPanel sharedFontPanel]; }
static void panelState(const QString &tag) {
    NSFontPanel *p = panel();
    void *buttons = nullptr;
    if (p.delegate) object_getInstanceVariable(p.delegate, "mPanelButtons", &buttons);
    NSFont *sel = [NSFontManager sharedFontManager].selectedFont;
    NSView *frameView = p.contentView.superview;
    NSView *combo = frameView ? findView(frameView, "FontPanelFontSizeComboButton") : nil;
    say(QStringLiteral("panel[%1] visible=%2 key=%3 frame=%4x%5 selected=%6 %7 sizeBox='%8' Qt's buttons=%9 in the window=%10")
        .arg(tag).arg(p.visible).arg(p.keyWindow).arg(p.frame.size.width).arg(p.frame.size.height)
        .arg(QString::fromNSString(sel.fontName ?: @"nil")).arg(sel.pointSize)
        .arg(combo ? QString::fromNSString([(id)combo stringValue]) : QStringLiteral("(no box)"))
        .arg(buttons ? "made" : "none").arg(frameView && findView(frameView, "QNSPanelContentsWrapper") ? "yes" : "no"));
    if (!frameView) return;
    NSBitmapImageRep *rep = [frameView bitmapImageRepForCachingDisplayInRect:frameView.bounds];
    [frameView cacheDisplayInRect:frameView.bounds toBitmapImageRep:rep];
    NSString *path = [NSString stringWithFormat:@"%@/panel-%@.png", env("DRIVE_OUT", ".").toNSString(), tag.toNSString()];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
}
static NSPoint centre(NSView *v, CGFloat xfrac) {
    NSRect r = [v convertRect:v.bounds toView:nil];
    return NSMakePoint(r.origin.x + r.size.width * xfrac, r.origin.y + r.size.height / 2);
}
static void click(NSWindow *w, NSPoint at) {
    for (NSEventType t : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp})
        [NSApp postEvent:[NSEvent mouseEventWithType:t location:at modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                                        windowNumber:w.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1] atStart:NO];
}
static void keys(NSWindow *w, NSString *chars, unsigned short code) {
    for (NSUInteger i = 0; i < chars.length; ++i) {
        NSString *c = [chars substringWithRange:NSMakeRange(i, 1)];
        for (NSEventType t : {NSEventTypeKeyDown, NSEventTypeKeyUp})
            [NSApp postEvent:[NSEvent keyEventWithType:t location:NSZeroPoint modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                                          windowNumber:w.windowNumber context:nil characters:c charactersIgnoringModifiers:c isARepeat:NO keyCode:code] atStart:NO];
    }
}

static QPointer<QDialog> settings;
static QPushButton *fontButton(int row) {
    if (!settings) return nullptr;
    for (auto *tabs : settings->findChildren<QTabWidget*>())
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i).remove('&') == "Appearance") {
                tabs->setCurrentIndex(i);
                auto *grid = tabs->widget(i)->findChild<QGridLayout*>();
                if (!grid) return nullptr;
                QLayoutItem *item = grid->itemAtPosition(row, 1);
                return item ? qobject_cast<QPushButton*>(item->widget()) : nullptr;
            }
    return nullptr;
}
static void settingsButton(const QString &label) {
    for (auto *b : settings->findChildren<QPushButton*>())
        if (b->text().remove('&') == label) { say("settings window: pressing " + label); b->click(); return; }
    say("settings window: no button " + label);
}
struct Steps {
    QList<std::pair<int, std::function<void()>>> list; int at = 0;
    void add(int ms, std::function<void()> f) { list.append({ms, std::move(f)}); }
    void run() { if (at >= list.size()) return; auto s = list[at++]; QTimer::singleShot(s.first, qApp, [this, s]{ s.second(); run(); }); }
};
static Steps *steps;

static void script() {
    const int row = env("DRIVE_BUTTON", "2").toInt();
    const QString size = env("DRIVE_SIZE");
    const QString how = env("DRIVE_CLOSE", "red");
    const QString end = env("DRIVE_END", "apply");
    QMainWindow *main = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *m = qobject_cast<QMainWindow*>(w); m && QString(m->metaObject()->className()) == "QucsApp") main = m;
    QAction *action = nullptr;
    for (QAction *a : main->findChildren<QAction*>())
        if (a->text().remove('&').startsWith("Application Settings")) action = a;
    say(QStringLiteral("main window up; settings action %1").arg(action ? "found" : "NOT found"));
    steps = new Steps;
    steps->add(100, [action]{ QTimer::singleShot(0, action, &QAction::trigger); });
    steps->add(1500, [row]{
        for (QWidget *w : QApplication::topLevelWidgets())
            if (QString(w->metaObject()->className()) == "QucsSettingsDialog") settings = qobject_cast<QDialog*>(w);
        QPushButton *b = fontButton(row);
        say(QStringLiteral("settings window %1; button %2 reads '%3'").arg(settings ? "open" : "NOT open").arg(row).arg(b ? b->text() : "?"));
        if (b) QTimer::singleShot(0, b, &QPushButton::click);
    });
    steps->add(1800, []{ panelState("open"); });
    if (!size.isEmpty()) {
        steps->add(200, []{
            NSView *combo = findView(panel().contentView.superview, "FontPanelFontSizeComboButton");
            say("clicking the size box"); if (combo) click(panel(), centre(combo, 0.3)); });
        steps->add(500, []{ keys(panel(), @"\x7f\x7f\x7f", 51); });
        steps->add(300, [size]{ say("typing " + size + ", Return"); keys(panel(), size.toNSString(), 0); });
        steps->add(300, []{ keys(panel(), @"\r", 36); });
        steps->add(700, []{ panelState("typed"); });
    }
    for (const QString &c : env("DRIVE_CLICKS").split(';', Qt::SkipEmptyParts)) {
        const QStringList xy = c.split(',');
        steps->add(400, [xy, c]{
            say("clicking the panel at " + c);
            click(panel(), NSMakePoint(xy[0].toDouble(), panel().contentView.superview.bounds.size.height - xy[1].toDouble()));
        });
        steps->add(700, [c]{ panelState("clicked-" + QString(c).replace(',', '_')); });
    }
    if (env("DRIVE_POPUP") == "1") {
        steps->add(300, []{
            NSView *combo = findView(panel().contentView.superview, "FontPanelFontSizeComboButton");
            say("clicking the size box's arrow"); if (combo) click(panel(), centre(combo, 0.82)); });
        steps->add(900, []{
            QStringList w; for (NSWindow *x in NSApp.windows) if (x.visible) w << QString::fromNSString(x.className);
            say("windows up: " + w.join(", "));
            for (NSWindow *x in NSApp.windows) if (x.visible && [x.className containsString:@"ComboBoxWindow"]) {
                say(QStringLiteral("clicking the middle of its list, %1x%2").arg(x.frame.size.width).arg(x.frame.size.height));
                click(x, NSMakePoint(x.frame.size.width / 2, x.frame.size.height / 2));
            } });
        steps->add(900, []{ panelState("popup"); });
    }
    if (how == "none") { say("left for a person"); return; }
    steps->add(300, [how]{
        say("panel: closing by " + how);
        if (how == "esc") keys(panel(), @"\x1b", 53);
        else if (how == "return") keys(panel(), @"\r", 36);
        else click(panel(), centre([panel() standardWindowButton:NSWindowCloseButton], 0.5));
    });
    steps->add(1000, [row]{
        panelState("closed");
        QPushButton *b = fontButton(row);
        say(QStringLiteral("after the panel: panel visible=%1, button %2 reads '%3'").arg(panel().visible).arg(row).arg(b ? b->text() : "?"));
        if (settings) settings->grab().save(env("DRIVE_OUT", ".") + "/settings.png");
    });
    steps->add(300, [end]{
        if (panel().visible) { say("panel still open: closing it"); [panel() performClose:nil]; }
        settingsButton(end == "ok" ? "OK" : end == "cancel" ? "Cancel" : "Apply");
    });
    steps->add(800, []{ say("done"); fflush(nullptr); _exit(0); });
    steps->run();
}

static void poll() {
    static int polls = 0;
    if (++polls % 20 == 1) {
        QStringList tops;
        if (qApp) for (QWidget *w : QApplication::topLevelWidgets()) if (w->isVisible()) tops << QString(w->metaObject()->className()) + ":" + w->windowTitle();
        say(QStringLiteral("poll %1: qApp=%2 visible tops: %3").arg(polls).arg(qApp != nullptr).arg(tops.join(" | ")));
    }
    bool up = false;
    if (qApp) for (QWidget *w : QApplication::topLevelWidgets())
        if (QString(w->metaObject()->className()) == "QucsApp" && w->isVisible()) up = true;
    if (up) { dispatch_async(dispatch_get_main_queue(), ^{ QTimer::singleShot(1500, qApp, script); }); return; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ poll(); });
}
__attribute__((constructor)) static void start() {
    say("loaded");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ poll(); });
}
