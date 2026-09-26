/*
 * A monkey for the main window: a seeded random walk over what a user can
 * do to open schematics - gestures on the canvas in every mouse mode (with
 * modifiers, drags interrupted by undo, double clicks that open property
 * dialogs), the menu and toolbar actions, keys, the wheel, tabs, symbol
 * view, hierarchy, closing and reopening - with every dialog that comes up
 * either cancelled or filled with odd values and accepted.
 *
 * It asserts nothing about the result: the application must survive it.
 * Run under ASan/UBSan, where a use after free, an out-of-bounds read or
 * UB anywhere on the way ends the run with a report; a watchdog turns a
 * step that does not finish (a hang, a modal loop nobody can leave) into
 * an abort with the main thread's stack. The last steps are printed
 * either way, and the seed reproduces the walk:
 *
 *   QUCS_MONKEY_SEED=<n> QUCS_MONKEY_STEPS=<n> test_gui_monkey
 *
 * QUCS_MONKEY_VERBOSE=1 prints every step. The default is a short walk,
 * so that CI runs it on every push; stress runs take many seeds.
 */
#include <QtTest>
#include <QKeyEvent>
#include <QToolButton>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileDialog>
#include <QFontDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QWheelEvent>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <memory>
#include <random>
#include <thread>
#ifndef _WIN32
#  include <pthread.h>
#endif

#include "config.h"
#include "qucs.h"
#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "filebrowser.h"
#ifdef QUCS_HAVE_QTPDF
#include "pdfdoc.h"
#include <QPdfWriter>
#endif
#include "schematic.h"
#include "textdoc.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "autosave.h"
#include "crashhandler.h"
#include "components/component.h"
#include "wire.h"
#include "diagrams/diagram.h"
#include "paintings/painting.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#    define MONKEY_SANITIZER 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define MONKEY_SANITIZER 1
#endif
#ifdef MONKEY_SANITIZER
#  include <sanitizer/common_interface_defs.h>
#endif

namespace {

// The last steps, printed when the run dies (sanitizer report, signal,
// watchdog) so that a finding says what led to it.
constexpr int kTrail = 48;
std::string g_trail[kTrail];
std::atomic<int> g_trailNext{0};
bool g_verbose = false;
unsigned g_seed = 1;

void note(const QString& what)
{
    const int i = g_trailNext.fetch_add(1);
    g_trail[i % kTrail] = QStringLiteral("[%1] %2").arg(i).arg(what).toStdString();
    if (g_verbose) {
        std::fprintf(stderr, "monkey %s\n", g_trail[i % kTrail].c_str());
        std::fflush(stderr);
    }
}

void printTrail()
{
    const int n = g_trailNext.load();
    std::fprintf(stderr, "\n=== monkey: seed %u, last steps ===\n", g_seed);
    for (int i = std::max(0, n - kTrail); i < n; ++i)
        std::fprintf(stderr, "  %s\n", g_trail[i % kTrail].c_str());
    std::fflush(stderr);
}

#ifndef MONKEY_SANITIZER
void onFatalSignal(int sig)
{
    printTrail();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

// Aborts the main thread (its stack is what matters for a hang) when the
// walk makes no progress for a while.
class Watchdog
{
public:
    explicit Watchdog(int seconds) : a_limit(seconds)
#ifndef _WIN32
        , a_main(pthread_self())
#endif
    {
        a_thread = std::thread([this] {
            while (!a_stop) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (a_stop) break;
                if (++a_idle > a_limit * 4) {
                    std::fprintf(stderr, "\n=== monkey: no progress for %d s - a hang ===\n", a_limit);
                    printTrail();
#ifndef _WIN32
                    pthread_kill(a_main, SIGABRT);   // the main thread's stack
#else
                    std::abort();
#endif
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    std::_Exit(3);
                }
            }
        });
    }
    ~Watchdog()
    {
        a_stop = true;
        a_thread.join();
    }
    void pet() { a_idle = 0; }

private:
    int a_limit;
#ifndef _WIN32
    pthread_t a_main;
#endif
    std::atomic<bool> a_stop{false};
    std::atomic<int> a_idle{0};
    std::thread a_thread;
};

int envInt(const char* name, int fallback)
{
    bool ok = false;
    const int v = qEnvironmentVariable(name).toInt(&ok);
    return ok ? v : fallback;
}

} // namespace

class TestGuiMonkey : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QStringList a_schematics;           // copies of the examples
    QStringList a_pdfs;                 // PDF documents to read beside them
    std::mt19937 a_rng;
    QPointer<QucsApp> a_app;
    QTimer a_dialogTimer;
    QHash<QWidget*, int> a_dialogAge;   // ticks a dialog has been up
    int a_dialogsAccepted = 0, a_dialogsRejected = 0, a_boxes = 0, a_popups = 0;

    // Straight from the engine, which the standard specifies bit for bit
    // (its distributions it does not): a seed walks the same way on every
    // platform, so a walk that fails in CI on Linux replays on a Mac.
    int pick(int n) { return n <= 1 ? 0 : int(a_rng() % unsigned(n)); }
    bool chance(double p) { return (a_rng() >> 8) < p * double(1u << 24); }
    template <typename T> T pickOf(const QList<T>& list) { return list.at(pick(list.size())); }

    static QString oddText(std::mt19937& rng)
    {
        const auto below = [&rng](int n) { return int(rng() % unsigned(n)); };
        static const QStringList odd = {
            "", " ", "0", "-1", "1", "-0", "1e308", "-1e308", "1e-320", "nan", "inf", "-inf",
            "2147483647", "-2147483648", "99999999999999999999", "1k", "1 2 3", "abc", "\"", "'",
            "<", ">", "</Components>", "=", "==", "@", "#", "%", "\\", "(", ")", "[]", "{}",
            "0x7fffffff", "1.", ".5", "+-1", "1e", "1e+", "10p", "R1", "v(out)", "@r1[resistance]",
            "\t", "a\nb", "éè中", "0.001", "100", "-100",
        };
        const int i = below(int(odd.size()) + 1);
        if (i == odd.size()) return QString(100 + below(4901), QChar('9'));
        return odd.at(i);
    }

    // ---- dialogs -----------------------------------------------------------
    // Property editors whose OK applies the (odd) values to the document.
    static bool acceptable(QWidget* w)
    {
        static const QStringList editors = {
            "ComponentDialog", "DiagramDialog", "MarkerDialog", "LabelDialog", "ID_Dialog",
            "ArrowDialog", "FillDialog", "GraphicTextDialog", "NgSweepDialog", "NgOptDialog",
            "NgStatisticsDialog", "SpiceLibCompDialog", "CustomSimDialog", "TextBoxDialog",
            "MagCoreDialog", "OptimizeDialog", "SettingsDialog", "SymbolDialog", "SweepDialog",
            "PortSymbolDialog", "EquationDialog",
        };
        return editors.contains(QString::fromLatin1(w->metaObject()->className()));
    }

    void scramble(QWidget* dialog)
    {
        for (auto* e : dialog->findChildren<QLineEdit*>())
            if (e->isVisible() && e->isEnabled() && !e->isReadOnly() && chance(0.3))
                e->setText(oddText(a_rng));
        for (auto* c : dialog->findChildren<QComboBox*>())
            if (c->isVisible() && c->isEnabled() && c->count() > 0 && chance(0.25)) {
                if (c->isEditable() && chance(0.3)) c->setEditText(oddText(a_rng));
                else c->setCurrentIndex(pick(c->count()));
            }
        for (auto* b : dialog->findChildren<QCheckBox*>())
            if (b->isVisible() && b->isEnabled() && chance(0.25))
                b->toggle();
        for (auto* s : dialog->findChildren<QAbstractSpinBox*>())
            if (s->isVisible() && s->isEnabled() && chance(0.25))
                s->stepBy(pick(41) - 20);
        for (auto* t : dialog->findChildren<QTableWidget*>())
            if (t->isVisible() && t->isEnabled() && t->rowCount() > 0 && t->columnCount() > 0 && chance(0.4)) {
                QTableWidgetItem* it = t->item(pick(t->rowCount()), pick(t->columnCount()));
                if (it && (it->flags() & Qt::ItemIsEditable)) it->setText(oddText(a_rng));
                if (chance(0.3)) t->setCurrentCell(pick(t->rowCount()), 0);
            }
        for (auto* p : dialog->findChildren<QPlainTextEdit*>())
            if (p->isVisible() && p->isEnabled() && !p->isReadOnly() && chance(0.2))
                p->setPlainText(oddText(a_rng));
    }

    static QAbstractButton* buttonLabelled(QWidget* dialog, const QStringList& labels)
    {
        for (auto* b : dialog->findChildren<QAbstractButton*>()) {
            if (!b->isVisible() || !b->isEnabled()) continue;
            if (labels.contains(b->text().remove('&').trimmed(), Qt::CaseInsensitive)) return b;
        }
        return nullptr;
    }

    // Deals with whatever is up: popup menus, message boxes, modal and
    // modeless dialogs. Only posts the click that ends a dialog: a click
    // may open the next modal loop, and that must not run inside this one.
    void handleDialogs()
    {
        if (QWidget* popup = QApplication::activePopupWidget()) {
            ++a_popups;
            note(QStringLiteral("  close popup %1").arg(popup->metaObject()->className()));
            popup->close();
            return;
        }
        QWidget* modal = QApplication::activeModalWidget();
        QList<QWidget*> up;
        if (modal) up << modal;
        for (QWidget* w : QApplication::topLevelWidgets())
            if (w != modal && w != a_app && w->isVisible() && qobject_cast<QDialog*>(w))
                up << w;
        for (QWidget* w : up) {
            const int age = ++a_dialogAge[w];
            if (age == 1) {
                end(w);
            } else if (age > 60) {                 // ~1 s: refused its button
                note(QStringLiteral("  force-reject %1").arg(w->metaObject()->className()));
                if (auto* d = qobject_cast<QDialog*>(w)) d->reject(); else w->close();
                a_dialogAge[w] = 1;
            }
        }
        // Forget the ones that are gone.
        for (auto it = a_dialogAge.begin(); it != a_dialogAge.end();)
            if (!up.contains(it.key())) it = a_dialogAge.erase(it); else ++it;
    }

    void end(QWidget* w)
    {
        const QString cls = QString::fromLatin1(w->metaObject()->className());
        if (auto* box = qobject_cast<QMessageBox*>(w)) {
            ++a_boxes;
            QList<QAbstractButton*> buttons = box->buttons();
            if (buttons.isEmpty()) { post(box, [box] { box->reject(); }); return; }
            QAbstractButton* b = buttons.at(pick(buttons.size()));
            note(QStringLiteral("  message box \"%1\" -> %2")
                     .arg(box->text().left(60).simplified(), b->text().remove('&')));
            post(b, [b] { b->click(); });
            return;
        }
        auto* dialog = qobject_cast<QDialog*>(w);
        if (!dialog) { w->close(); return; }
        const bool system = qobject_cast<QFileDialog*>(w) || qobject_cast<QColorDialog*>(w)
                            || qobject_cast<QFontDialog*>(w) || qobject_cast<QInputDialog*>(w)
                            || qobject_cast<QProgressDialog*>(w);
        if (!system && acceptable(w) && chance(0.6)) {
            scramble(w);
            QAbstractButton* ok = buttonLabelled(w, {"OK", "Ok", "Apply"});
            if (!ok) {
                if (auto* box = w->findChild<QDialogButtonBox*>())
                    for (auto* b : box->buttons())
                        if (box->buttonRole(b) == QDialogButtonBox::AcceptRole) { ok = b; break; }
            }
            if (ok) {
                ++a_dialogsAccepted;
                note(QStringLiteral("  %1: odd values, %2").arg(cls, ok->text().remove('&')));
                QPointer<QAbstractButton> button(ok);
                post(ok, [button] { if (button) button->click(); });
                return;
            }
        }
        ++a_dialogsRejected;
        note(QStringLiteral("  %1: cancel").arg(cls));
        QPointer<QDialog> d(dialog);
        post(dialog, [d] { if (d) d->reject(); });
    }

    template <typename F> static void post(QObject* context, F f)
    {
        QTimer::singleShot(0, context, f);
    }

    // ---- the walk ----------------------------------------------------------
    Schematic* schematic() const { return a_app ? a_app->currentSchematic() : nullptr; }

    // A point of the viewport: over an element half the time, anywhere
    // (or just outside) otherwise.
    QPoint somewhere(Schematic* sch)
    {
        QWidget* vp = sch->viewport();
        if (chance(0.55)) {
            QList<QPoint> model;
            if (sch->getSymbolMode()) {
                for (auto* p : sch->a_SymbolPaints) model << QPoint(p->cx, p->cy);
            } else {
                for (auto* c : sch->a_DocComps) model << QPoint(c->cx, c->cy);
                for (auto* w : sch->a_DocWires) model << QPoint(w->x1, w->y1) << QPoint(w->x1 / 2 + w->x2 / 2, w->y1);
                for (auto* d : sch->a_DocDiags) model << QPoint(d->cx + 5, d->cy - 5) << QPoint(d->cx + d->x2 / 2, d->cy - d->y2 / 2);
                for (auto* p : sch->a_DocPaints) model << QPoint(p->cx, p->cy);
            }
            if (!model.isEmpty()) {
                QPoint p = sch->modelToViewport(pickOf(model));
                p += QPoint(pick(9) - 4, pick(9) - 4);
                if (vp->rect().contains(p)) return p;
            }
        }
        return QPoint(pick(vp->width() + 40) - 20, pick(vp->height() + 40) - 20);
    }

    Qt::KeyboardModifiers modifiers()
    {
        switch (pick(8)) {
        case 0: return Qt::ShiftModifier;
        case 1: return Qt::ControlModifier;
        case 2: return Qt::AltModifier;
        default: return Qt::NoModifier;
        }
    }

    QList<QAction*> modeActions() const
    {
        QucsApp* a = a_app;
        return {a->select, a->select, a->select, a->insWire, a->insLabel, a->insGround, a->insPort,
                a->insEquation, a->magPlus, a->editRotate, a->editMirror, a->editMirrorY, a->editPaste,
                a->editActivate, a->editDelete, a->setMarker, a->moveText, a->onGrid,
                a->setDiagramLimits, a->resetDiagramLimits, a->editStretch, a->editMove};
    }

    QList<QAction*> commandActions() const
    {
        QucsApp* a = a_app;
        // Not here: anything that starts another program, a simulation, a
        // print job, the browser, quits, or reaches beyond the documents
        // (projects, libraries, modules, the workspace).
        return {a->fileNew, a->textNew, a->symNew, a->fileSave, a->fileSaveAll,
                a->fileClose, a->fileCloseOthers, a->fileCloseAllLeft, a->fileCloseAllRight,
                a->fileSettings, a->editCut, a->editCopy, a->editCopy, a->magAll, a->magSel,
                a->magOne, a->magMinus, a->symEdit, a->symEdit, a->intoH, a->popH, a->undo, a->undo,
                a->undo, a->redo, a->redo, a->TabFirstAction, a->TabLastAction, a->TabNextAction,
                a->TabPreviousAction, a->editCopyImage, a->exportAsImage, a->showGrid,
                a->alignTop, a->alignBottom, a->alignLeft, a->alignRight, a->distrHor, a->distrVert,
                a->selectAll, a->selectAll, a->changeProps, a->editFind, a->selectMarker,
                a->centerHor, a->centerVert, a->checkSchematicAction, a->symRecreate,
                a->symPinOrder, a->showNet, a->dpl_sch, a->fileOpen, a->symLoad, a->symSaveAs,
                a->findChild<QAction*>(QStringLiteral("lockToolbars"))};
    }

    void gesture(Schematic* sch)
    {
        QWidget* vp = sch->viewport();
        const Qt::KeyboardModifiers mods = modifiers();
        const QPoint a = somewhere(sch);
        switch (pick(12)) {
        case 0: case 1: case 2:
            note(QStringLiteral("click %1,%2 mods %3").arg(a.x()).arg(a.y()).arg(int(mods)));
            QTest::mouseClick(vp, Qt::LeftButton, mods, a);
            break;
        case 3: case 4: case 5: {
            const QPoint b = somewhere(sch);
            const int steps = 1 + pick(8);
            const bool interrupt = chance(0.2);
            note(QStringLiteral("drag %1,%2 -> %3,%4 in %5 moves%6")
                     .arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y()).arg(steps)
                     .arg(interrupt ? ", undo/redo/close in between" : ""));
            QTest::mousePress(vp, Qt::LeftButton, mods, a);
            for (int i = 1; i <= steps; ++i) {
                QTest::mouseMove(vp, a + (b - a) * i / steps);
                if (interrupt && i == steps / 2 + 1) {
                    QAction* act = pickOf(QList<QAction*>{a_app->undo, a_app->redo, a_app->editDelete,
                                                           a_app->selectAll, a_app->magAll});
                    note(QStringLiteral("  mid-drag: %1").arg(act->text().remove('&')));
                    act->trigger();
                    QCoreApplication::processEvents();
                    if (schematic() != sch) return;   // the document went away
                }
            }
            QTest::mouseRelease(vp, Qt::LeftButton, mods, b);
            break;
        }
        case 6: case 10: case 11:
            note(QStringLiteral("double click %1,%2").arg(a.x()).arg(a.y()));
            QTest::mouseDClick(vp, Qt::LeftButton, mods, a);
            break;
        case 7:
            note(QStringLiteral("right click %1,%2").arg(a.x()).arg(a.y()));
            QTest::mouseClick(vp, Qt::RightButton, mods, a);
            break;
        case 8: {
            const int delta = (pick(2) ? 120 : -120) * (1 + pick(3));
            const Qt::KeyboardModifiers wm = pickOf(QList<Qt::KeyboardModifiers>{
                Qt::NoModifier, Qt::ControlModifier, Qt::ShiftModifier});
            note(QStringLiteral("wheel %1 at %2,%3 mods %4").arg(delta).arg(a.x()).arg(a.y()).arg(int(wm)));
            QWheelEvent ev(a, vp->mapToGlobal(a), QPoint(), QPoint(0, delta), Qt::NoButton, wm,
                           Qt::NoScrollPhase, false);
            QApplication::sendEvent(vp, &ev);
            break;
        }
        default: {
            static const QList<int> keys = {Qt::Key_Escape, Qt::Key_Delete, Qt::Key_Backspace,
                                            Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down,
                                            Qt::Key_Return, Qt::Key_Tab, Qt::Key_Space, Qt::Key_R,
                                            Qt::Key_Plus, Qt::Key_Minus};
            const int key = pickOf(keys);
            note(QStringLiteral("key %1 mods %2").arg(key, 0, 16).arg(int(mods)));
            // Not QTest::keyClick(): its ASCII table asserts on Return, Delete, ...
            const QString text = key >= 0x20 && key < 0x7f ? QString(QChar(key)).toLower() : QString();
            QKeyEvent press(QEvent::KeyPress, key, mods, text);
            QApplication::sendEvent(vp, &press);
            QKeyEvent release(QEvent::KeyRelease, key, mods, text);
            QApplication::sendEvent(vp, &release);
            break;
        }
        }
    }

#ifdef QUCS_HAVE_QTPDF
    static void writePdf(const QString& path, int pages)
    {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        QTextDocument doc;
        QString html = QStringLiteral("<h1>Datasheet</h1><p>Absolute maximum ratings: 5 V, 20 mA.</p>");
        for (int i = 2; i <= pages; ++i)
            html += QStringLiteral("<p style='page-break-before:always'>Page %1: the needle, the gain, the noise.</p>").arg(i);
        doc.setHtml(html);
        doc.print(&writer);
    }

    // Something done in a PDF document in front: scrolled, zoomed, text
    // selected and copied, searched, its pages at the side, its page
    // typed, the file written anew.
    void pdfStep(PdfDoc* pdf)
    {
        qucs_s::pdf::PageView* view = pdf->view();
        QWidget* vp = view->viewport();
        const QPoint a(pick(std::max(1, vp->width())), pick(std::max(1, vp->height())));
        const QPoint b(pick(std::max(1, vp->width())), pick(std::max(1, vp->height())));
        switch (pick(12)) {
        case 0: {
            const int delta = (pick(2) ? 120 : -120) * (1 + pick(3));
            const Qt::KeyboardModifiers m = chance(0.4) ? Qt::ControlModifier : Qt::NoModifier;
            note(QStringLiteral("pdf: wheel %1 mods %2").arg(delta).arg(int(m)));
            QWheelEvent ev(a, vp->mapToGlobal(a), QPoint(), QPoint(0, delta), Qt::NoButton, m, Qt::NoScrollPhase, false);
            QApplication::sendEvent(vp, &ev);
            break;
        }
        case 1:
        case 2: {
            const Qt::MouseButton button = chance(0.2) ? Qt::MiddleButton : Qt::LeftButton;
            note(QStringLiteral("pdf: drag %1,%2 to %3,%4").arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y()));
            QTest::mousePress(vp, button, Qt::NoModifier, a);
            for (int i = 1; i <= 3; ++i) {
                const QPoint p = a + (b - a) * i / 3;
                QMouseEvent move(QEvent::MouseMove, QPointF(p), vp->mapToGlobal(QPointF(p)), button, button, Qt::NoModifier);
                QApplication::sendEvent(vp, &move);
            }
            QTest::mouseRelease(vp, button, Qt::NoModifier, b);
            break;
        }
        case 3:
            note(QStringLiteral("pdf: double click %1,%2").arg(a.x()).arg(a.y()));
            QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, a);
            break;
        case 4: {
            static const QList<int> keys = {Qt::Key_Home, Qt::Key_End, Qt::Key_Space, Qt::Key_PageDown, Qt::Key_PageUp,
                                            Qt::Key_Up, Qt::Key_Down, Qt::Key_Escape, Qt::Key_C};
            const int key = pickOf(keys);
            const Qt::KeyboardModifiers m = key == Qt::Key_C ? Qt::ControlModifier : Qt::NoModifier;
            note(QStringLiteral("pdf: key %1").arg(key, 0, 16));
            QKeyEvent press(QEvent::KeyPress, key, m);
            QApplication::sendEvent(view, &press);
            break;
        }
        case 5: {
            const QString text = pickOf(QStringList{"needle", "Page", "gain", "", "x", oddText(a_rng).left(12)});
            note(QStringLiteral("pdf: find %1").arg(text));
            pdf->find(text);
            for (int i = pick(4); i > 0; --i) pdf->findNext(chance(0.3));
            if (chance(0.3)) pdf->hideSearch();
            break;
        }
        case 6:
            note("pdf: sidebar");
            pdf->setSidebarShown(!pdf->sidebar()->isVisible());
            if (pdf->thumbnails()->count() > 0 && chance(0.5)) pdf->thumbnails()->setCurrentRow(pick(pdf->thumbnails()->count()));
            break;
        case 7: {
            const int mode = pick(4);
            note(QStringLiteral("pdf: zoom %1").arg(mode));
            if (mode == 0) view->setFit(qucs_s::pdf::PageView::Fit::Width);
            else if (mode == 1) view->setFit(qucs_s::pdf::PageView::Fit::Page);
            else view->setZoom(qucs_s::pdf::PageView::MinZoom + pick(1000) / 1000.0 * 6);
            break;
        }
        case 8: {
            const int pages = 1 + pick(6);
            note(QStringLiteral("pdf: written anew, %1 pages").arg(pages));
            writePdf(pdf->getDocName(), pages);
            break;
        }
        case 9: {
            const QString text = pickOf(QStringList{"1", "2", "99", "0", "-3", "iv", ""});
            note(QStringLiteral("pdf: page %1").arg(text));
            pdf->pageField()->setText(text);
            QMetaObject::invokeMethod(pdf->pageField(), "returnPressed");
            break;
        }
        case 10:
            note("pdf: select the page, copy");
            pdf->selectAll();
            pdf->copySelection();
            break;
        default:
            note("pdf: next and previous pages");
            pdf->nextPage();
            if (chance(0.5)) pdf->previousPage();
            if (chance(0.2)) pdf->lastPage();
            break;
        }
    }
#endif

    void step()
    {
        Schematic* sch = schematic();
        const int what = pick(100);
#ifdef QUCS_HAVE_QTPDF
        if (auto* pdf = qobject_cast<PdfDoc*>(a_app->DocumentTab->currentWidget()); pdf != nullptr && what < 45) {
            pdfStep(pdf);
            return;
        }
#endif
        if (sch && what < 45) {
            if (chance(0.3)) {
                QAction* mode = pickOf(modeActions());
                note(QStringLiteral("mode %1").arg(mode->text().remove('&')));
                if (mode->isEnabled()) mode->trigger();
                QCoreApplication::processEvents();
                if (schematic() != sch) return;
            }
            gesture(sch);
        } else if (what < 75) {
            QAction* act = pickOf(commandActions());
            note(QStringLiteral("action %1%2").arg(act->text().remove('&'), act->isEnabled() ? "" : " (disabled)"));
            if (act->isEnabled()) act->trigger();
        } else if (what < 82) {
            const QString file = !a_pdfs.isEmpty() && chance(0.2) ? pickOf(a_pdfs) : pickOf(a_schematics);
            note(QStringLiteral("open %1").arg(QFileInfo(file).fileName()));
            a_app->gotoPage(file);
        } else if (what < 86 && sch) {
            // Insert a component chosen in the component list.
            QListWidget* list = a_app->CompComps;
            // The group chooser is the list's sibling (a private member).
            QComboBox* groups = nullptr;
            if (list && list->parentWidget())
                for (QComboBox* c : list->parentWidget()->findChildren<QComboBox*>(Qt::FindDirectChildrenOnly))
                    if (c->count() > 3) groups = c;
            if (groups && groups->count() > 0) groups->setCurrentIndex(pick(groups->count()));
            if (list && list->count() > 0) {
                QListWidgetItem* item = list->item(pick(list->count()));
                note(QStringLiteral("component %1 / %2").arg(groups ? groups->currentText() : "?", item->text()));
                list->setCurrentItem(item);
                emit list->itemClicked(item);
                QCoreApplication::processEvents();
                if (schematic() == sch) {
                    const QPoint p = somewhere(sch);
                    QTest::mouseMove(sch->viewport(), p);
                    note(QStringLiteral("  place at %1,%2").arg(p.x()).arg(p.y()));
                    QTest::mouseClick(sch->viewport(), Qt::LeftButton, Qt::NoModifier, p);
                }
            }
        } else if (what < 90 && a_app->DocumentTab->count() > 0) {
            const int i = pick(a_app->DocumentTab->count());
            note(QStringLiteral("tab %1").arg(i));
            a_app->DocumentTab->setCurrentIndex(i);
        } else if (what < 93) {
            const QSize s(200 + pick(1400), 150 + pick(1000));
            note(QStringLiteral("resize %1x%2").arg(s.width()).arg(s.height()));
            a_app->resize(s);
        } else if (what < 96) {
            if (auto* text = qobject_cast<TextDoc*>(a_app->DocumentTab->currentWidget())) {
                note("type into text document");
                for (const QChar ch : oddText(a_rng).left(200)) {
                    QKeyEvent key(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(ch));
                    QApplication::sendEvent(text, &key);
                }
            } else if (sch) {
                note(QStringLiteral("zoom %1").arg(chance(0.5) ? "in" : "out"));
                sch->zoomBy(chance(0.5) ? 8.0 : 0.125);
            }
        } else if (what < 97) {
            claudeStep();
        } else if (what < 99) {
            fileBrowserStep();
        } else {
            note("escape");
            a_app->slotEscape();
        }
    }

    // The claude of monkey_claude.sh (next to this file): it answers each
    // prompt with a reply as it is written, a line that is not the
    // protocol, Markdown, an edit of one of the open schematics (Qucs-S
    // loads it again) that asks for permission - and now and then dies in
    // the middle of the turn.
    void writeFakeClaude()
    {
        const QString path = dir.filePath("fake-claude");
        const QString source = QFileInfo(QStringLiteral(__FILE__)).absolutePath() + QStringLiteral("/monkey_claude.sh");
        if (!QFile::copy(source, path)) return;
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("QUCS_CLAUDE", path.toUtf8());
        const QString edited = a_schematics.at(int(g_seed % unsigned(a_schematics.size())));
        qputenv("QUCS_FAKE_EDIT", edited.toUtf8());
        // Which of its tool calls it makes goes round with each prompt: a
        // walk sends a few, so each seed starts elsewhere in the round.
        QFile count(edited + QStringLiteral(".count"));
        if (count.open(QIODevice::WriteOnly)) count.write(QByteArray::number(g_seed * 3u));
    }

    // Something done in the Claude Code dock.
    // The File Browser: its views, filters and options, its folders -
    // within the walk's copies of the examples - and a schematic or data
    // display opened from it. Nothing it would hand to the system (a file
    // Qucs-S does not open itself, the file manager, the trash), and no
    // Enter, which opens a file where it is the activation key.
    void fileBrowserStep()
    {
        FileBrowser* fb = a_app->fileBrowserPanel();
        if (fb == nullptr) return;
        const QString home = QDir::cleanPath(dir.filePath("examples"));
        const auto within = [&home](const QString& path) { return path == home || path.startsWith(home + QLatin1Char('/')); };
        if (!within(fb->location())) fb->setLocation(home);
        const QStringList shown = fb->shownNames();
        const auto some = [&]() -> QString {
            return shown.isEmpty() ? QString() : QDir(fb->location()).filePath(pickOf(shown));
        };
        switch (pick(9)) {
        case 0: {
            const auto view = FileBrowser::View(pick(6));
            note(QStringLiteral("files: view %1").arg(int(view)));
            fb->setView(view);
            break;
        }
        case 1: {
            const QString text = chance(0.5) ? QString() : oddText(a_rng).left(1 + pick(3));
            note(QStringLiteral("files: filter \"%1\"").arg(text));
            fb->setFilterText(text);
            break;
        }
        case 2:
            note("files: hidden, Qucs-S files only");
            if (chance(0.5)) fb->setShowHidden(!fb->showHidden());
            else fb->setQucsFilesOnly(!fb->qucsFilesOnly());
            break;
        case 3:
            note("files: back, forward or up");
            if (chance(0.3)) fb->back();
            else if (chance(0.5)) fb->forward();
            else if (fb->location() != home) fb->up();
            if (!within(fb->location())) fb->setLocation(home);
            break;
        case 4: {
            // A folder entered or opened in place; a schematic or display opened.
            const QString path = some();
            const QFileInfo info(path);
            if (path.isEmpty() || !within(path)) break;
            if (info.isDir() || info.suffix() == QLatin1String("sch") || info.suffix() == QLatin1String("dpl")) {
                note(QStringLiteral("files: activate %1").arg(info.fileName()));
                fb->activate(path);
            }
            if (!within(fb->location())) fb->setLocation(home);
            break;
        }
        case 5:
            note("files: select");
            fb->selectPath(some());
            break;
        case 6: {
            note("files: keys");
            QAbstractItemView* view = fb->currentView();
            view->setFocus();
            const Qt::Key keys[] = {Qt::Key_Down, Qt::Key_Up, Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End,
                                    Qt::Key_PageDown, Qt::Key_Backspace, Qt::Key_A};
            for (int i = 0; i < 1 + pick(4); ++i) QTest::keyClick(view, keys[pick(int(std::size(keys)))]);
            if (!within(fb->location())) fb->setLocation(home);
            break;
        }
        case 7: {
            note("files: new folder");
            if (!fb->createFolder(fb->location()).isEmpty()) a_app->slotEscape();
            break;
        }
        default: {
            // Its context menu, built; only copying a path is done.
            const QString path = chance(0.8) ? some() : QString();
            note(QStringLiteral("files: menu of %1").arg(QFileInfo(path).fileName()));
            QMenu* menu = fb->contextMenuFor(path);
            for (QAction* a : menu->actions())
                if (a->text() == QLatin1String("Copy Path")) a->trigger();
            delete menu;
            break;
        }
        }
    }

    void claudeStep()
    {
        ClaudeCodeTabs* tabs = a_app->claudeCode();
        if (tabs == nullptr) return;
        ClaudeCodePanel* panel = tabs->current();
        if (panel == nullptr) return;
        switch (pick(15)) {
        case 0:
            note("claude: show or hide the dock");
            a_app->toggleClaudeCode();
            break;
        case 1: {
            note("claude: send a prompt");
            panel->composer()->setPlainText(oddText(a_rng).left(300) + QStringLiteral(" ?"));
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(panel->composer(), &enter);
            break;
        }
        case 2: {
            const int which = pick(3);
            QToolButton* b = which == 0 ? panel->denyButton() : which == 1 ? panel->allowButton() : panel->allowEditsButton();
            note(QStringLiteral("claude: %1").arg(b->text()));
            if (panel->permissionCard()->isVisibleTo(panel) && b->isVisibleTo(panel)) b->click();
            break;
        }
        case 3:
            note("claude: stop");
            panel->stopTurn();
            break;
        case 4:
            note("claude: new conversation");
            panel->newConversation();
            break;
        case 9:
            note("claude: a conversation in a new tab");
            if (tabs->count() < 6) panel->newButton()->click();
            break;
        case 10: {
            const QList<ClaudeCodePanel*> all = tabs->panels();
            ClaudeCodePanel* closing = pickOf(all);
            note(QStringLiteral("claude: close the tab of %1").arg(closing->title()));
            tabs->closeConversation(closing, false);
            break;
        }
        case 11: {
            const QList<ClaudeCodePanel*> all = tabs->panels();
            note("claude: another tab");
            tabs->showConversation(pickOf(all));
            break;
        }
        case 13: {
            // A tab renamed: written in, then kept, left, or left open for
            // what comes next.
            const QList<ClaudeCodePanel*> all = tabs->panels();
            ClaudeCodePanel* named = pickOf(all);
            note(QStringLiteral("claude: rename the tab of %1").arg(named->title()));
            tabs->renameConversation(named);
            if (QLineEdit* editor = tabs->renameEditor()) {
                editor->setText(chance(0.2) ? QString() : oddText(a_rng).left(120));
                const int how = pick(3);
                if (how < 2) {
                    QKeyEvent key(QEvent::KeyPress, how == 0 ? Qt::Key_Return : Qt::Key_Escape, Qt::NoModifier);
                    QApplication::sendEvent(editor, &key);
                }
            }
            break;
        }
        case 14: {
            // An item of a tab's menu.
            const int index = pick(tabs->count());
            std::unique_ptr<QMenu> menu(tabs->tabMenu(index));
            QList<QAction*> actions;
            for (QAction* a : menu->actions())
                if (!a->isSeparator()) actions << a;
            if (actions.isEmpty()) break;
            QAction* a = pickOf(actions);
            note(QStringLiteral("claude: tab menu %1").arg(a->text()));
            if (a->isEnabled()) a->trigger();
            break;
        }
        case 12: {
            // A line of tools, or one, opened or folded.
            note("claude: open or fold the tools");
            QStringList keys;
            QTextDocument* doc = panel->transcript()->document();
            for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
                for (auto it = b.begin(); !it.atEnd(); ++it) {
                    const QString href = it.fragment().charFormat().anchorHref();
                    if (href.startsWith(QLatin1String("toggle:"))) keys << href;
                }
            if (!keys.isEmpty()) emit panel->transcript()->anchorClicked(QUrl(pickOf(keys)));
            break;
        }
        case 5: {
            QList<QAction*> actions;
            for (QMenu* menu : panel->findChildren<QMenu*>())
                for (QAction* a : menu->actions())
                    if (!a->isSeparator() && a->menu() == nullptr && a->objectName() != QLatin1String("claudeShowFolder"))
                        actions << a;
            if (actions.isEmpty()) break;
            QAction* a = pickOf(actions);
            note(QStringLiteral("claude: menu %1").arg(a->text().remove('&')));
            if (a->isEnabled()) a->trigger();
            break;
        }
        case 6:
            note("claude: status bar chip");
            if (auto* chip = a_app->findChild<QToolButton*>(QStringLiteral("statusClaude"))) chip->click();
            break;
        case 7:
            note("claude: the open document along or not");
            panel->attachButton()->toggle();
            break;
        default: {
            const QString folder = chance(0.3) ? QString() : QFileInfo(pickOf(a_schematics)).absolutePath();
            note(QStringLiteral("claude: work in %1").arg(folder.isEmpty() ? QStringLiteral("the workspace") : QDir(folder).dirName()));
            panel->setWorkingDirectory(folder);
            break;
        }
        }
    }

    void walk(int steps)
    {
        Watchdog dog(envInt("QUCS_MONKEY_HANG_SECONDS", 90));
        for (int i = 0; i < steps; ++i) {
            dog.pet();
            step();
            QCoreApplication::processEvents();
            if (!a_app) QFAIL("the main window went away");
        }
        note("close everything");
        a_app->slotEscape();
        a_app->slotFileCloseAll();   // message boxes answered by the handler
        QTest::qWait(50);
        dog.pet();
    }

    void copyExamples()
    {
        const QString src = QStringLiteral(QUCS_EXAMPLES_DIR "/ngspice");
        QDirIterator it(src, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            if (f.contains("/OpenVAF/")) continue;   // need modules built in the GUI
            const QString dst = dir.filePath("examples/" + QDir(src).relativeFilePath(f));
            QDir().mkpath(QFileInfo(dst).path());
            QFile::copy(f, dst);
            if (f.endsWith(".sch")) a_schematics << dst;
        }
        a_schematics.sort();
#ifdef QUCS_HAVE_QTPDF
        // A datasheet to read: several pages of text, no link (a link
        // clicked would open a browser).
        a_pdfs << dir.filePath("examples/datasheet.pdf");
        writePdf(a_pdfs.first(), 4);
#endif
        // A sweep family to plot, so that the auto colours, markers and the
        // per-curve legend are on the canvas too.
        const QString sweep = dir.filePath("examples/NGspice features/RC_lowpass_ngsweep.dat.ngspice");
        QFile f(sweep);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream s(&f);
            s << "<Qucs Dataset " PACKAGE_VERSION ">\n<indep frequency 40>\n";
            for (int i = 0; i < 40; ++i) s << 10.0 * std::pow(10.0, i / 8.0) << "\n";
            s << "</indep>\n<indep r1_resistance 5>\n";
            for (int k = 0; k < 5; ++k) s << 250.0 * std::pow(2.0, k) << "\n";
            s << "</indep>\n<dep ngsweep1.v(out) frequency r1_resistance>\n";
            for (int k = 0; k < 5; ++k)
                for (int i = 0; i < 40; ++i) {
                    const double x = 10.0 * std::pow(10.0, i / 8.0) / (1e6 / (250.0 * std::pow(2.0, k)));
                    s << 1.0 / (1 + x * x) << "-j" << x / (1 + x * x) << "\n";
                }
            s << "</dep>\n<indep r1_resistance 5>\n";
            for (int k = 0; k < 5; ++k) s << 250.0 * std::pow(2.0, k) << "\n";
            s << "</indep>\n<dep fc r1_resistance>\n";
            for (int k = 0; k < 5; ++k) s << 1e6 / (250.0 * std::pow(2.0, k)) << "\n";
            s << "</dep>\n";
        }
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        // The Claude Code dock gets clicks and keys too: never the real
        // claude, which would be sent whatever the walk typed (on Unix a
        // script that answers as it does, below).
        qputenv("QUCS_CLAUDE", "/nonexistent/claude");
        g_seed = unsigned(envInt("QUCS_MONKEY_SEED", 1));
        g_verbose = !qEnvironmentVariableIsEmpty("QUCS_MONKEY_VERBOSE");
        a_rng.seed(g_seed);
#ifdef MONKEY_SANITIZER
        // The sanitizers report the fault (a handler of ours would take
        // their SIGSEGV away) and call this before they exit.
        __sanitizer_set_death_callback(printTrail);
#else
        std::signal(SIGSEGV, onFatalSignal);
#ifdef SIGBUS
        std::signal(SIGBUS, onFatalSignal);
#endif
        std::signal(SIGFPE, onFatalSignal);
        std::signal(SIGILL, onFatalSignal);
#endif

        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        // QucsApp lists the simulators it can find and puts up a modal
        // error box when there is none: name one that exists (and is never
        // run - the walk does not simulate).
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QDir().mkpath(dir.filePath("work"));
        QDir().mkpath(dir.filePath("workspace"));
        QDir().mkpath(dir.filePath("temp"));
        QucsSettings.QucsWorkDir.setPath(dir.filePath("work"));
        QucsSettings.qucsWorkspaceDir.setPath(dir.filePath("workspace"));
        QucsSettings.projsDir.setPath(dir.filePath("workspace"));
        QucsSettings.tempFilesDir.setPath(dir.filePath("temp"));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
        qucs_s::autosave::setDirectory(dir.filePath("autosave"));
        qucs_s::crash::setReportDirectory(dir.filePath("crash-reports"));
        copyExamples();
        QVERIFY(a_schematics.size() > 20);
#ifndef Q_OS_WIN
        writeFakeClaude();
#endif

        connect(&a_dialogTimer, &QTimer::timeout, this, &TestGuiMonkey::handleDialogs);
        a_dialogTimer.start(15);
    }

    void cleanupTestCase()
    {
        a_dialogTimer.stop();
        std::fprintf(stderr, "monkey: seed %u, %d steps noted; dialogs: %d accepted with odd values, "
                             "%d cancelled; %d message boxes, %d popups\n",
                     g_seed, g_trailNext.load(), a_dialogsAccepted, a_dialogsRejected, a_boxes, a_popups);
    }

    void survivesARandomWalk()
    {
        const int steps = envInt("QUCS_MONKEY_STEPS", 400);
        std::fprintf(stderr, "monkey: seed %u, %d steps\n", g_seed, steps);
        auto* app = new QucsApp(false);
        a_app = app;
        QucsMain = app;
        app->resize(1200, 800);
        app->show();
        QVERIFY(QTest::qWaitForWindowExposed(app));
        // A few documents to start from.
        for (int i = 0; i < 4; ++i) app->gotoPage(pickOf(a_schematics));
        walk(steps);
        QVERIFY(a_app);
        delete app;
        QucsMain = nullptr;
    }
};

// Not QTEST_MAIN: QucsApp opens every command-line argument as a document,
// so QtTest's own arguments (function names, -v2, ...) must not reach the
// QApplication that QucsApp inspects.
int main(int argc, char** argv)
{
    int one = 1;
    QApplication app(one, argv);
    TestGuiMonkey test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_gui_monkey.moc"
