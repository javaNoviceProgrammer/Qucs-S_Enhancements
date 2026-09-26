/*
 * statusbar.h - the main window's status bar: what the tool in hand does,
 * the cursor and the diagram value under it, the selection, the grid and
 * the zoom; the schematic's problems, the last simulation and the
 * simulator; whether the document is saved; the theme
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_STATUSBAR_H
#define QUCS_STATUSBAR_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

class QucsApp;
class Schematic;
class TextDoc;
class SimulationRun;
class Diagram;
struct MappedPoint;
class QLabel;
class QMenu;
class QTimer;
class QToolButton;
class QWidget;

namespace qucs_s::status {

class ChipRow;
class HintLabel;

/// The unit of a quantity a diagram shows, told by its name: time s,
/// frequency Hz, a voltage (out.v, out.Vt, v(out)) V, a current (Pr1.I,
/// i(R1)) A, dB(...) dB, a phase (phase(...), arg(...)) degrees; empty
/// when the name does not tell.
QString unitOf(const QString& name);

/// \a value with an SI prefix and \a unit, to four significant digits or
/// with \a decimals places after the point: "1.235 ms", "50 Ω". Decibels
/// and degrees take no prefix: "−3.01 dB".
QString withUnit(double value, const QString& unit, int decimals = -1);

/// What the cursor readout says at \a p on a rectangular diagram: each
/// axis by its variable (or x, y1, y2 for several), its value in the
/// diagram's notation, with the unit where the variable tells one:
/// "time 1.235 ms · out.Vt 3.012 V".
QString readout(const Diagram* diagram, const MappedPoint& p);

/// How long before \a now \a then was: "just now", "5 min ago", "at
/// 14:05", "on 3 Sep".
QString age(const QDateTime& then, const QDateTime& now);

/// The options that make \a simulator (spicecompat::Simulator) print its
/// version, empty for one that has none.
QStringList versionArguments(int simulator);

/// The version \a simulator names in \a output (of versionArguments()),
/// empty when it names none.
QString versionIn(int simulator, const QString& output);

/// The duration of a run: "0.42 s", "12.3 s", "2 min 5 s".
QString duration(qint64 milliseconds);

} // namespace qucs_s::status

/*!
 * The status bar of the main window. On the left, what the tool in hand
 * does (and the application's passing messages). On the right, chips,
 * each shown while it has something to say, the least important dropped
 * first when the window is narrow:
 *
 *  - the diagram readout under the cursor, the selection (a click zooms
 *    to it), the cursor position (line and column in a text document),
 *    the language a text document is highlighted as (a menu chooses it
 *    for the files of its suffix), the grid (a click shows or hides it)
 *    and the zoom (a menu);
 *  - the electrical rule check of the schematic, again after each edit
 *    (a click lists the problems and goes to the first), the last
 *    simulation - running, its time, its warnings, failed (a click shows
 *    its output) - and the simulator with its version (a menu switches);
 *  - whether the document is saved, and when (a click saves it);
 *  - Claude Code: whether it is at work, what it runs, whether it waits
 *    for the user's permission (a click shows or hides its dock);
 *  - the theme (a menu).
 *
 * The application tells it about the cursor, the document in front, the
 * simulations and the theme; the rest it reads when the document in
 * front is drawn again or edited.
 */
class StatusPanel : public QObject
{
    Q_OBJECT

public:
    explicit StatusPanel(QucsApp* app);
    ~StatusPanel() override;

    /// The cursor moved: model coordinates in a schematic, line and column
    /// in a text document; \a readout the value of the diagram under it.
    void setCursor(int x, int y, const QString& readout);
    /// Another document is in front, or the one in front was replaced.
    void documentChanged();
    /// Brings everything shown up to date at the next turn of the event
    /// loop; refresh() at once.
    void scheduleRefresh();
    void refresh();
    /// The look changed: the colours of the marks, the theme's swatch.
    void themeChanged();

    /// A simulation with an external simulator is on its way; the chip
    /// follows it to its end.
    void watchRun(SimulationRun* run, const QString& document);
    /// Qucsator's simulation, which reports through the application.
    enum class Outcome { Succeeded, Warned, Failed, Stopped };
    void runStarted(const QString& document);
    void runEnded(Outcome outcome, int warnings);
    /// Forgets the last run (a project opened or closed).
    void clearRun();

    /// The hint on the left, whole (the label elides it).
    QString hint() const;
    /// The simulator's version, when it has been asked (empty until then).
    QString simulatorVersion() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Tone { None, Ok, Warn, Error, Busy };

    QucsApp* a_app;
    qucs_s::status::HintLabel* a_hint;
    qucs_s::status::ChipRow* a_row;
    QLabel* a_readout;
    QToolButton* a_selection;
    QLabel* a_position;
    QToolButton* a_language;
    QToolButton* a_grid;
    QToolButton* a_zoom;
    QToolButton* a_problems;
    QToolButton* a_run;
    QToolButton* a_simulator;
    QToolButton* a_saved;
    QToolButton* a_claude;
    QToolButton* a_theme;

    QTimer* a_refreshTimer;   // coalesces refresh()
    QTimer* a_checkTimer;     // the rule check, a moment after an edit
    QTimer* a_clock;          // a running simulation's seconds, the ages

    QPointer<QWidget> a_watched;   // the document in front, followed
    int a_checkedSimulator = -1;   // the simulator the last check was for

    // The last simulation.
    enum class RunState { None, Running, Ended };
    RunState a_runState = RunState::None;
    Outcome a_outcome = Outcome::Succeeded;
    int a_runWarnings = 0;
    qint64 a_runTime = 0;
    QElapsedTimer a_runClock;
    QString a_runDocument;
    bool a_runExternal = false;   // an external simulator's (the console has its output)

    // The simulators' versions, by simulator, program and its date; the
    // programs being asked. Asked once the window is shown.
    bool a_mayAsk = false;
    QSet<QString> a_asking;

    void check();
    void updateHint(Schematic* doc);
    void updateSelection(Schematic* doc);
    void updateGrid(Schematic* doc);
    void updateZoom(Schematic* doc);
    void updateLanguage(TextDoc* doc);
    void updateRun();
    void updateSimulator();
    void updateSaved();
    void updateClaude();
    void updateTheme();
    QString modeHint(Schematic* doc) const;
    QString versionKey(int simulator) const;
    void askVersion(int simulator);
    void styleChips();
    QColor toneColour(Tone tone) const;
    void setChip(QToolButton* chip, const QString& text, Tone tone);
    void popUp(QToolButton* chip, QMenu* menu);
    QMenu* zoomMenu();
    QMenu* languageMenu();
    QMenu* simulatorMenu();
    void follow(QWidget* document);
};

#endif // QUCS_STATUSBAR_H
