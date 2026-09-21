/*
 * The Terminal and Python Shell docks: a ProcessConsole runs a shell (or
 * an interpreter) on a pseudo-terminal, shows its output as plain text
 * and sends it the lines typed. Covered here with /bin/sh, with the
 * python3 on PATH when there is one, and as QucsApp sets the docks up.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QAction>
#include <QDockWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "processconsole.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

#ifndef Q_OS_WIN
#include <csignal>
#include <cerrno>
#endif

namespace {
struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

QPushButton* button(QWidget* w, const QString& text)
{
    for (QPushButton* b : w->findChildren<QPushButton*>())
        if (b->text() == text) return b;
    return nullptr;
}
} // namespace

class TestProcessConsole : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.firstRun = false;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.XyceExecutable = "xyce";
        QucsSettings.SpiceOpusExecutable = "spiceopus";
        QucsSettings.S4Qworkdir = dir.filePath("work");
        QucsSettings.tempFilesDir.setPath(dir.filePath("work"));
        QVERIFY(QDir().mkpath(dir.filePath("work")));
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

#ifndef Q_OS_WIN
    void theShellRunsCommandsAndKeepsItsState()
    {
        ProcessConsole console;
        console.setProgram("/bin/sh", {});
        console.setWorkingDirectory(dir.path());
        console.show();
        QVERIFY(QTest::qWaitForWindowExposed(&console));   // starts the shell
        QTRY_VERIFY(console.isRunning());
        const qint64 pid = console.process()->processId();
        QVERIFY(pid > 0);

        console.sendLine("echo hello-$((1+2))");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains("hello-3"), 5000);
        // The directory persists between commands: it is one shell.
        console.sendLine("cd / && pwd");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains(QStringLiteral("\n/\n")), 5000);
        console.sendLine("pwd");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().count(QStringLiteral("\n/\n")) >= 2, 5000);
        // The output is text: escape sequences dropped, a carriage return
        // starting the line over, a backspace taking a character back.
        console.sendLine("printf 'a\\033[31mb\\033[0m\\rXY\\n'; printf 'abc\\b\\bZ\\n'");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains(QStringLiteral("\nXY\n")), 5000);
        QVERIFY(!console.outputText().contains("\033"));
        QVERIFY(console.outputText().contains(QStringLiteral("\naZ\n")));
        QVERIFY(!console.outputText().contains(QStringLiteral("\nab")));

        // Interrupt reaches the foreground job, not the shell.
        console.sendLine("sleep 30 && echo slept");
        QTest::qWait(300);
        console.interrupt();
        console.sendLine("echo after-interrupt");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains("after-interrupt"), 5000);
        QVERIFY(!console.outputText().contains("\nslept\n"));   // the echo of the line is there, its result not
        QVERIFY(console.isRunning());
        QCOMPARE(console.process()->processId(), pid);

        // Restart is a fresh shell.
        button(&console, "Restart")->click();
        QTRY_VERIFY(console.isRunning());
        QVERIFY(console.process()->processId() != pid);
        QVERIFY(console.outputText().contains("restarted"));
        console.sendLine("echo again");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains("again"), 5000);
        // "exit" ends it, and the console says so.
        console.sendLine("exit 3");
        QTRY_VERIFY_WITH_TIMEOUT(!console.isRunning(), 5000);
        QTRY_VERIFY(console.outputText().contains("exited with code 3"));
    }

    void theInputLineHasAHistory()
    {
        ProcessConsole console;
        console.setProgram("/bin/sh", {});
        console.setWorkingDirectory(dir.path());
        console.show();
        QVERIFY(QTest::qWaitForWindowExposed(&console));
        QTRY_VERIFY(console.isRunning());
        QLineEdit* input = console.inputLine();
        input->setFocus();
        QTest::keyClicks(input, "echo one");
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains("\none\n"), 5000);
        QVERIFY(input->text().isEmpty());
        QTest::keyClicks(input, "echo two");
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains("\ntwo\n"), 5000);
        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->text(), QString("echo two"));
        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->text(), QString("echo one"));
        QTest::keyClick(input, Qt::Key_Up);                 // stays at the oldest
        QCOMPARE(input->text(), QString("echo one"));
        QTest::keyClick(input, Qt::Key_Down);
        QCOMPARE(input->text(), QString("echo two"));
        QTest::keyClick(input, Qt::Key_Down);               // back to the empty draft
        QCOMPARE(input->text(), QString());
        // Ctrl-L clears the output.
        QTest::keyClick(input, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(console.outputText().isEmpty());
    }

    void theProgramGoesWithTheConsole()
    {
        qint64 pid = -1;
        {
            ProcessConsole console;
            console.setProgram("/bin/sh", {});
            console.setWorkingDirectory(dir.path());
            QVERIFY(console.start());
            pid = console.process()->processId();
            QVERIFY(pid > 0);
            QCOMPARE(::kill(static_cast<pid_t>(pid), 0), 0);   // alive
        }
        // Gone, and collected: no zombie left behind.
        QVERIFY(::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH);
    }

    void theProjectDirectoryButtonChangesDirectory()
    {
        ProcessConsole console;
        console.setProgram("/bin/sh", {});
        console.setWorkingDirectory(dir.path());
        console.setChangeDirectoryCommand("cd %1", ProcessConsole::ShellQuoting);
        QVERIFY(console.start());
        // No project open: the home directory. (With a project it is the
        // project directory - see QucsApp.)
        button(&console, "Project dir")->click();
        console.sendLine("pwd");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains("\n" + QDir::homePath() + "\n"), 5000);
        // Quoting: a path with a space and a quote.
        QCOMPARE(ProcessConsole::quotedForShell("/a b/it's"), QString("'/a b/it'\\''s'"));
        QCOMPARE(ProcessConsole::quotedForPython("C:\\x\\y \"q\""), QString("\"C:\\\\x\\\\y \\\"q\\\"\""));
    }

    void pythonAnswers()
    {
        const QString python = QStandardPaths::findExecutable("python3");
        if (python.isEmpty()) QSKIP("python3 is not installed");
        ProcessConsole console;
        console.setProgram(python, {"-i", "-u"}, {"PYTHON_BASIC_REPL=1"});
        console.setWorkingDirectory(dir.path());
        console.setChangeDirectoryCommand("import os; os.chdir(%1)", ProcessConsole::PythonQuoting);
        QVERIFY(console.start());
        console.sendLine("print(6*7)");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains("42"), 10000);
        QVERIFY(console.outputText().contains(">>>"));   // the prompt, as on a terminal
        button(&console, "Project dir")->click();
        console.sendLine("import os; print(os.getcwd() == os.path.expanduser('~'))");
        QTRY_VERIFY_WITH_TIMEOUT(console.outputText().contains("True"), 10000);
        console.sendLine("raise SystemExit(0)");
        QTRY_VERIFY_WITH_TIMEOUT(!console.isRunning(), 5000);
    }
#endif

    void aProgramThatDoesNotExistIsReported()
    {
        ProcessConsole console;
        console.setProgram(dir.filePath("no-such-program"), {});
        QVERIFY(!console.start());
        QVERIFY(!console.isRunning());
        QVERIFY2(console.outputText().contains("Could not start"), qPrintable(console.outputText()));
    }

    void theDocksAreThereHiddenAndOnTheViewMenu()
    {
        QucsSettings.PythonExecutable.clear();
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.terminalConsole() != nullptr);
        QVERIFY(app.pythonConsole() != nullptr);
        QVERIFY(app.terminalDockWidget()->isHidden());
        QVERIFY(app.pythonDockWidget()->isHidden());
        QVERIFY(!app.terminalConsole()->isRunning());       // nothing runs until shown
        QVERIFY(!app.pythonConsole()->isRunning());
        QCOMPARE(app.terminalConsole()->program(), QucsApp::shellProgram());
#ifndef Q_OS_WIN
        QVERIFY(QFileInfo(QucsApp::shellProgram()).isExecutable());
#endif
        QAction* terminalAction = app.terminalDockWidget()->toggleViewAction();
        QAction* pythonAction = app.pythonDockWidget()->toggleViewAction();
        QCOMPARE(terminalAction->text(), QString("&Terminal"));
        QCOMPARE(pythonAction->text(), QString("&Python Shell"));

        // The Python interpreter follows the setting.
        QucsSettings.PythonExecutable = dir.filePath("my-python");
        app.updateConsolePrograms();
        QCOMPARE(app.pythonConsole()->program(), dir.filePath("my-python"));
        QucsSettings.PythonExecutable.clear();
        app.updateConsolePrograms();
        QVERIFY(app.pythonConsole()->program().contains("python"));
    }

#ifndef Q_OS_WIN
    void showingTheTerminalDockStartsTheShellInTheProject()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        app.show();
        QVERIFY(QTest::qWaitForWindowExposed(&app));
        app.terminalConsole()->setProgram("/bin/sh", {});   // not the login shell of the account
        app.terminalDockWidget()->toggleViewAction()->trigger();
        QVERIFY(app.terminalDockWidget()->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(app.terminalConsole()->isRunning(), 5000);
        app.terminalConsole()->sendLine("pwd");
        QTRY_VERIFY_WITH_TIMEOUT(app.terminalConsole()->outputText().contains("\n" + QDir::homePath() + "\n"), 5000);
        // For a look at it: QUCS_TEST_GRAB=<dir> saves a picture of the window.
        const QString grabDir = qEnvironmentVariable("QUCS_TEST_GRAB");
        if (!grabDir.isEmpty()) {
            app.terminalConsole()->sendLine("ls /");
            QTest::qWait(300);
            app.resize(1200, 760);
            QTest::qWait(100);
            app.grab().save(grabDir + "/terminal-dock.png");
        }
        app.terminalDockWidget()->toggleViewAction()->trigger();
        QVERIFY(!app.terminalDockWidget()->isVisible());
        QVERIFY(app.terminalConsole()->isRunning());        // hiding does not end it
    }
#endif
};

int main(int argc, char** argv)
{
    int one = 1;   // QucsApp opens every argument as a document
    QApplication app(one, argv);
    TestProcessConsole test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_process_console.moc"
