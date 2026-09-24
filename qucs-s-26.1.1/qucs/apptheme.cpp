/*
 * apptheme.cpp - the application's look: the system's, dark or light, or
 * one of the designed themes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "apptheme.h"
#include "designedstyle.h"
#include "ink.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace qucs_s::apptheme {

namespace {

// Whether a palette of ours is on the application, as opposed to the
// platform's: what "System" has to undo.
bool g_paletteForced = false;
// Whether the designed themes' style and style sheet are on it.
bool g_designedOn = false;
// The theme last applied.
int g_current = System;
// The style of the platform's themes; empty until known.
QString g_nativeStyle;

// Whether the platform gives the requested appearance itself. Cocoa and
// Windows do (Qt 6.8 on); the generic Unix and offscreen platforms leave
// the request unanswered.
bool platformShows(Qt::ColorScheme scheme)
{
    return QGuiApplication::styleHints()->colorScheme() == scheme;
}

void forcePalette(const QPalette& palette)
{
    QApplication::setPalette(palette);
    g_paletteForced = true;
}

// Back to the platform's palette: a palette that resolves nothing of its
// own makes Qt take every role from the platform theme again, now and
// when the system's appearance changes later.
void releasePalette()
{
    if (!g_paletteForced) return;
    QPalette platform;
    platform.setResolveMask(0);
    QApplication::setPalette(platform);
    g_paletteForced = false;
}

QString styleName(const QStyle* style)
{
    return style != nullptr ? style->name() : QString();
}

// The designed themes' style on the application, remembering the one it
// replaces.
void designedStyleOn()
{
    if (g_designedOn) return;
    if (g_nativeStyle.isEmpty()) g_nativeStyle = styleName(QApplication::style());
    QApplication::setStyle(new DesignedStyle);
    g_designedOn = true;
}

// The platform themes' style back, and no style sheet of ours.
void designedStyleOff()
{
    if (!g_designedOn) return;
    qApp->setStyleSheet(QString());
    QStyle* style = QStyleFactory::create(g_nativeStyle);
    if (style == nullptr && !QStyleFactory::keys().isEmpty())
        style = QStyleFactory::create(QStyleFactory::keys().constFirst());
    if (style != nullptr) QApplication::setStyle(style);
    g_designedOn = false;
}

QColor hex(const char* spec) { return QColor(QLatin1String(spec)); }

Colours colours(const char* window, const char* surface, const char* base, const char* alternate,
                const char* raised, const char* border, const char* text, const char* muted,
                const char* disabled, const char* accent, const char* onAccent, const char* link,
                const char* paper, const char* grid)
{
    return Colours{hex(window), hex(surface), hex(base), hex(alternate), hex(raised), hex(border),
                   hex(text), hex(muted), hex(disabled), hex(accent), hex(onAccent), hex(link),
                   hex(paper), hex(grid)};
}

QString css(const QColor& c)
{
    if (c.alpha() == 255) return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

} // namespace

QColor mix(const QColor& a, const QColor& b, double t)
{
    const QColor x = a.toRgb(), y = b.toRgb();
    auto channel = [t](int p, int q) { return qBound(0, qRound(p + (q - p) * t), 255); };
    return QColor(channel(x.red(), y.red()), channel(x.green(), y.green()), channel(x.blue(), y.blue()),
                  channel(x.alpha(), y.alpha()));
}

const QList<Designed>& designed()
{
    //                      window     surface    base       alternate  raised     border
    //                      text       muted      disabled   accent     onAccent   link
    //                      paper      grid
    static const QList<Designed> themes = {
        {Daylight, QT_TRANSLATE_NOOP("apptheme", "Daylight"), false,
         colours("#f6f7f9", "#eceef1", "#ffffff", "#f5f7f9", "#ffffff", "#d0d7de",
                 "#1f2328", "#59636e", "#8c959f", "#0a66d0", "#ffffff", "#0a66d0",
                 "#ffffff", "#b0b7c0")},
        {Paper, QT_TRANSLATE_NOOP("apptheme", "Paper"), false,
         colours("#f3efe4", "#e8e2d2", "#fffcf2", "#f7f2e6", "#fffcf2", "#d3c9b2",
                 "#2e2a22", "#665e4e", "#a39a86", "#a64d17", "#ffffff", "#96440f",
                 "#fffae1", "#948b74")},
        {SolarizedLight, QT_TRANSLATE_NOOP("apptheme", "Solarized Light"), false,
         colours("#eee8d5", "#e4ddc8", "#fdf6e3", "#f6f0dd", "#fdf6e3", "#d5cdb4",
                 "#073642", "#52666d", "#93a1a1", "#1c72ad", "#ffffff", "#1c72ad",
                 "#fdf6e3", "#93a1a1")},
        {CatppuccinLatte, QT_TRANSLATE_NOOP("apptheme", "Catppuccin Latte"), false,
         colours("#eff1f5", "#e6e9ef", "#f8f9fb", "#eceff4", "#f8f9fb", "#ccd0da",
                 "#4c4f69", "#62657c", "#9ca0b0", "#8839ef", "#ffffff", "#1e66f5",
                 "#eff1f5", "#9ca0b0")},
        {Graphite, QT_TRANSLATE_NOOP("apptheme", "Graphite"), true,
         colours("#1f1f1f", "#181818", "#262626", "#2b2b2b", "#2d2d2d", "#3a3a3a",
                 "#e0e0e0", "#a3a3a3", "#6e6e6e", "#2a6fcc", "#ffffff", "#5aa7ff",
                 "#1e1f22", "#46494f")},
        {Nord, QT_TRANSLATE_NOOP("apptheme", "Nord"), true,
         colours("#2e3440", "#272c36", "#3b4252", "#373e4c", "#3b4252", "#434c5e",
                 "#eceff4", "#a3adc0", "#65718a", "#88c0d0", "#2e3440", "#88c0d0",
                 "#2e3440", "#4c566a")},
        {Dracula, QT_TRANSLATE_NOOP("apptheme", "Dracula"), true,
         colours("#282a36", "#21222c", "#1f2029", "#262834", "#343746", "#3b3e51",
                 "#f8f8f2", "#b3b7d3", "#6d7aa8", "#bd93f9", "#21222c", "#8be9fd",
                 "#282a36", "#4d5170")},
        {OneDark, QT_TRANSLATE_NOOP("apptheme", "One Dark"), true,
         colours("#282c34", "#21252b", "#1d2025", "#2c313a", "#333842", "#3a3f4b",
                 "#d7dae0", "#9da5b4", "#636b78", "#61afef", "#1d2025", "#61afef",
                 "#282c34", "#4b5263")},
        {SolarizedDark, QT_TRANSLATE_NOOP("apptheme", "Solarized Dark"), true,
         colours("#002b36", "#00232c", "#073642", "#04303b", "#0a3844", "#1a4a56",
                 "#93a1a1", "#839496", "#5b7178", "#1c72ad", "#ffffff", "#2aa198",
                 "#002b36", "#2f5560")},
        {CatppuccinMocha, QT_TRANSLATE_NOOP("apptheme", "Catppuccin Mocha"), true,
         colours("#1e1e2e", "#181825", "#181825", "#1c1c2b", "#313244", "#45475a",
                 "#cdd6f4", "#a6adc8", "#6c7086", "#cba6f7", "#1e1e2e", "#89b4fa",
                 "#1e1e2e", "#45475a")},
    };
    return themes;
}

const Designed* designedTheme(int theme)
{
    for (const Designed& d : designed())
        if (d.id == theme) return &d;
    return nullptr;
}

QList<int> themes()
{
    QList<int> all{System, Dark, Light};
    for (const Designed& d : designed()) all << d.id;
    return all;
}

QString name(int theme)
{
    switch (theme) {
    case System: return QCoreApplication::translate("apptheme", "System");
    case Dark: return QCoreApplication::translate("apptheme", "Dark");
    case Light: return QCoreApplication::translate("apptheme", "Light");
    default: break;
    }
    const Designed* d = designedTheme(theme);
    return d != nullptr ? QCoreApplication::translate("apptheme", d->name) : QString();
}

int bounded(int theme)
{
    return theme == Dark || theme == Light || designedTheme(theme) != nullptr ? theme : System;
}

void apply(int theme)
{
    theme = bounded(theme);
    g_current = theme;
    QStyleHints* hints = QGuiApplication::styleHints();
    if (const Designed* d = designedTheme(theme)) {
        designedStyleOn();
        // The window frames and the platform's dialogs follow the theme.
        hints->setColorScheme(d->dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
        forcePalette(palette(d->colours));
        qApp->setStyleSheet(styleSheet(d->colours));
        return;
    }
    designedStyleOff();
    if (theme == System) {
        hints->unsetColorScheme();
        releasePalette();
        return;
    }
    const Qt::ColorScheme scheme = theme == Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light;
    hints->setColorScheme(scheme);
    if (platformShows(scheme)) {
        releasePalette();
        return;
    }
    forcePalette(theme == Dark ? darkPalette() : lightPalette());
}

int current()
{
    return g_current;
}

bool designedInUse()
{
    return g_designedOn;
}

QString nativeStyle()
{
    if (!g_designedOn || g_nativeStyle.isEmpty()) return styleName(QApplication::style());
    return g_nativeStyle;
}

void setNativeStyle(const QString& style)
{
    if (style.isEmpty() || !QStyleFactory::keys().contains(style, Qt::CaseInsensitive)) return;
    g_nativeStyle = style;
    if (g_designedOn) return;   // shown when a platform theme comes back
    if (styleName(QApplication::style()).compare(style, Qt::CaseInsensitive) == 0) return;
    if (QStyle* s = QStyleFactory::create(style)) QApplication::setStyle(s);
}

bool isDark()
{
    const QPalette p = QApplication::palette();
    return p.color(QPalette::WindowText).value() > p.color(QPalette::Window).value();
}

QColor paper(int theme)
{
    if (const Designed* d = designedTheme(theme)) return d->colours.paper;
    // A platform theme: the dark paper when it shows dark, the document
    // background otherwise.
    if (bounded(theme) == Dark || (bounded(theme) == System && isDark())) return ink::darkPaperColour();
    return QColor();
}

QColor grid(int theme)
{
    if (const Designed* d = designedTheme(theme)) return d->colours.grid;
    return QColor();
}

QPalette darkPalette()
{
    // The colours of the Fusion style's dark scheme, spelled out so that
    // every style shows them.
    const QColor window(0x35, 0x35, 0x35), base(0x23, 0x23, 0x23), text(0xe6, 0xe6, 0xe6);
    const QColor disabled(0x80, 0x80, 0x80), highlight(0x2a, 0x82, 0xda);
    QPalette p(window, window);   // Light, Mid, Dark and Shadow derived from the button colour
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::LinkVisited, highlight.darker(120));
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x50, 0x50, 0x50));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

QPalette lightPalette()
{
    // The colours of the Fusion style's light scheme. (Its standardPalette()
    // is not that: it follows the system's scheme since Qt 6.5.)
    const QColor window(0xef, 0xef, 0xef), text(Qt::black), disabled(0xbe, 0xbe, 0xbe);
    const QColor highlight(0x30, 0x8c, 0xc6);
    QPalette p(window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::AlternateBase, QColor(0xf7, 0xf7, 0xf7));
    p.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xdc));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, Qt::blue);
    p.setColor(QPalette::LinkVisited, Qt::magenta);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x91, 0x9e, 0xa9));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, Qt::white);
    return p;
}

QPalette palette(const Colours& c)
{
    const bool dark = ink::isDark(c.window);
    QPalette p;
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool off = group == QPalette::Disabled;
        const QColor text = off ? c.disabled : c.text;
        p.setColor(group, QPalette::Window, c.window);
        p.setColor(group, QPalette::WindowText, text);
        p.setColor(group, QPalette::Base, off ? mix(c.base, c.window, 0.5) : c.base);
        p.setColor(group, QPalette::AlternateBase, c.alternate);
        p.setColor(group, QPalette::Text, text);
        p.setColor(group, QPalette::PlaceholderText, c.disabled);
        p.setColor(group, QPalette::Button, c.raised);
        p.setColor(group, QPalette::ButtonText, text);
        p.setColor(group, QPalette::BrightText, dark ? QColor(Qt::white) : QColor(Qt::black));
        p.setColor(group, QPalette::ToolTipBase, c.raised);
        p.setColor(group, QPalette::ToolTipText, c.text);
        // The frames: Mid is the border the style draws buttons and fields
        // with, Light and Dark its bevels (the designed style draws none).
        p.setColor(group, QPalette::Light, mix(c.raised, Qt::white, dark ? 0.08 : 0.6));
        p.setColor(group, QPalette::Midlight, mix(c.raised, c.border, 0.5));
        p.setColor(group, QPalette::Mid, c.border);
        p.setColor(group, QPalette::Dark, mix(c.border, c.text, 0.25));
        p.setColor(group, QPalette::Shadow, mix(c.window, Qt::black, 0.6));
        p.setColor(group, QPalette::Highlight, off ? mix(c.accent, c.window, 0.6) : c.accent);
        p.setColor(group, QPalette::HighlightedText, off ? c.disabled : c.onAccent);
        p.setColor(group, QPalette::Link, c.link);
        p.setColor(group, QPalette::LinkVisited, mix(c.link, c.text, 0.3));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        p.setColor(group, QPalette::Accent, off ? mix(c.accent, c.window, 0.6) : c.accent);
#endif
    }
    return p;
}

QString styleSheet(const Colours& c)
{
    const bool dark = ink::isDark(c.window);
    const QColor hover = mix(c.surface, c.text, dark ? 0.10 : 0.07);
    const QColor pressed = mix(c.surface, c.text, dark ? 0.17 : 0.13);
    const QColor accentSoft = withAlpha(c.accent, dark ? 70 : 50);
    const QColor accentSofter = withAlpha(c.accent, dark ? 100 : 75);
    const QColor accentLine = withAlpha(c.accent, 150);
    const QColor scroll = mix(c.window, c.text, dark ? 0.26 : 0.24);
    const QColor scrollHover = mix(c.window, c.text, dark ? 0.42 : 0.40);
    const QColor menu = dark ? c.raised : c.base;

    QString sheet = QStringLiteral(R"(
QToolTip { color: {text}; background-color: {menu}; border: 1px solid {border}; padding: 4px 6px; }

QMainWindow::separator { background: {window}; width: 4px; height: 4px; }
QMainWindow::separator:hover { background: {accentLine}; }

QToolBar { background: {surface}; border: none; border-bottom: 1px solid {border}; padding: 3px 4px; spacing: 2px; }
QToolBar::separator:horizontal { background: {border}; width: 1px; margin: 7px 5px; }
QToolBar::separator:vertical { background: {border}; height: 1px; margin: 5px 7px; }
QToolBar QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 2px; }
QToolBar QToolButton:hover { background: {hover}; border-color: {hover}; }
QToolBar QToolButton:pressed { background: {pressed}; border-color: {pressed}; }
QToolBar QToolButton:checked { background: {accentSoft}; border-color: {accentLine}; }
QToolBar QToolButton:checked:hover { background: {accentSofter}; }

QDockWidget { color: {text}; }
QDockWidget::title { background: {surface}; padding: 6px 8px; border-bottom: 1px solid {border}; text-align: left; }
QDockWidget::close-button, QDockWidget::float-button { border: none; background: transparent; padding: 2px; }
QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: {hover}; border-radius: 4px; }

QStatusBar { background: {surface}; color: {muted}; border-top: 1px solid {border}; }
QStatusBar::item { border: none; }

QMenuBar { background: {surface}; border-bottom: 1px solid {border}; }
QMenuBar::item { background: transparent; padding: 4px 10px; border-radius: 4px; }
QMenuBar::item:selected { background: {hover}; }
QMenuBar::item:pressed { background: {pressed}; }

QMenu { background: {menu}; border: 1px solid {border}; padding: 4px 0px; }
QMenu::item { color: {text}; background: transparent; padding: 5px 26px 5px 8px; margin: 0px 4px; border-radius: 4px; }
QMenu::item:selected { background: {accent}; color: {onAccent}; }
QMenu::item:disabled { color: {disabled}; background: transparent; }
QMenu::separator { height: 1px; background: {border}; margin: 4px 10px; }
QMenu::icon { padding-left: 6px; }

QTabWidget::pane { border: 1px solid {border}; top: -1px; }
QTabBar { qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: {muted}; border: none; border-bottom: 2px solid transparent; padding: 6px 12px; margin: 0px; }
QTabBar::tab:selected { color: {text}; border-bottom-color: {accent}; }
QTabBar::tab:hover:!selected { color: {text}; background: {hover}; }
QTabBar::tab:left { border: none; border-left: 2px solid transparent; padding: 12px 6px; }
QTabBar::tab:left:selected { border-left-color: {accent}; }
QTabBar::tab:bottom { border: none; border-top: 2px solid transparent; }
QTabBar::tab:bottom:selected { border-top-color: {accent}; }

QHeaderView::section { background: {surface}; color: {muted}; border: none; border-right: 1px solid {border}; border-bottom: 1px solid {border}; padding: 4px 8px; }

QScrollBar:vertical { background: transparent; width: 12px; margin: 0px; border: none; }
QScrollBar::handle:vertical { background: {scroll}; min-height: 32px; border-radius: 4px; margin: 2px 2px 2px 3px; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0px; border: none; }
QScrollBar::handle:horizontal { background: {scroll}; min-width: 32px; border-radius: 4px; margin: 3px 2px 2px 2px; }
QScrollBar::handle:hover { background: {scrollHover}; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QAbstractScrollArea::corner { background: transparent; }

QSplitter::handle { background: {border}; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QGroupBox { border: 1px solid {border}; border-radius: 6px; margin-top: 14px; padding: 10px 6px 6px 6px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 10px; padding: 0px 4px; color: {muted}; }

QComboBox QAbstractItemView { background: {menu}; border: 1px solid {border}; selection-background-color: {accent}; selection-color: {onAccent}; outline: 0px; }
)");
    const QList<QPair<QString, QColor>> tokens = {
        {"{window}", c.window},       {"{surface}", c.surface},       {"{border}", c.border},
        {"{text}", c.text},           {"{muted}", c.muted},           {"{disabled}", c.disabled},
        {"{accentSofter}", accentSofter}, {"{accentSoft}", accentSoft}, {"{accentLine}", accentLine},
        {"{accent}", c.accent},       {"{onAccent}", c.onAccent},     {"{menu}", menu},
        {"{hover}", hover},           {"{pressed}", pressed},         {"{scrollHover}", scrollHover},
        {"{scroll}", scroll},
    };
    for (const auto& [token, colour] : tokens) sheet.replace(token, css(colour));
    return sheet;
}

QIcon swatch(int theme)
{
    // Three bands: the window, the accent, the paper.
    QColor window, accent, paperColour;
    if (const Designed* d = designedTheme(theme)) {
        window = d->colours.surface;
        accent = d->colours.accent;
        paperColour = d->colours.paper;
    } else {
        const QPalette p = theme == Dark ? darkPalette() : theme == Light ? lightPalette() : QPalette();
        window = theme == System ? QColor(0x9a, 0x9a, 0x9a) : p.color(QPalette::Window);
        accent = theme == System ? QColor(0x5a, 0x5a, 0x5a) : p.color(QPalette::Highlight);
        paperColour = theme == Dark ? ink::darkPaperColour() : theme == Light ? QColor(Qt::white)
                                                                             : QColor(0xd8, 0xd8, 0xd8);
    }
    QIcon icon;
    for (int size : {16, 32}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(0.5, 0.5, size - 1.0, size - 1.0);
        QPainterPath clip;
        clip.addRoundedRect(r, size / 5.0, size / 5.0);
        p.setClipPath(clip);
        const double third = size / 3.0;
        p.fillRect(QRectF(0, 0, size, third), window);
        p.fillRect(QRectF(0, third, size, third), accent);
        p.fillRect(QRectF(0, 2 * third, size, size - 2 * third), paperColour);
        p.setClipping(false);
        p.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, size / 5.0, size / 5.0);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

} // namespace qucs_s::apptheme
