/*
 * The login shell's environment, brought into a process started from
 * the desktop: the dump is parsed after its marker, PATH is merged with
 * the shell's order kept and the process's own additions in front, what
 * the process has is not touched, shell-only variables are left out -
 * and a real shell answers.
 */
#include <QtTest>
#include <QTemporaryDir>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include "shellenvironment.h"

using namespace qucs_s::shellenv;

class TestShellEnvironment : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

    QString writeScript(const QString& name, const QString& text)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
        f.write(text.toUtf8());
        f.close();
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        return path;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
    }

    void theDumpIsWhatFollowsTheMarker()
    {
        QByteArray out("Welcome to some shell\nType help for help\n");
        out += "__QUCS_ENVIRONMENT__";
        out += '\0';
        out += "HOME=/home/x";
        out += '\0';
        out += "PATH=/a:/b";
        out += '\0';
        out += "MULTI=line one\nline two";   // a value with a newline survives the NUL separation
        out += '\0';
        out += "9BAD=x";                     // not a variable name
        out += '\0';
        out += "=novalue";
        out += '\0';
        QCOMPARE(parseEnvironmentDump(out), (QStringList{"HOME=/home/x", "PATH=/a:/b", "MULTI=line one\nline two"}));
        QVERIFY(parseEnvironmentDump("no marker here\0A=1").isEmpty());
        QVERIFY(parseEnvironmentDump(QByteArray()).isEmpty());
    }

    void pathKeepsTheShellsOrderWithTheProcessOwnAdditionsInFront()
    {
        // From the Finder: the bare system PATH, all of it in the shell's.
        QCOMPARE(mergedPath("/usr/bin:/bin:/usr/sbin:/sbin", "/opt/homebrew/bin:/Users/x/bin:/usr/bin:/bin:/usr/sbin:/sbin"),
                 QString("/opt/homebrew/bin:/Users/x/bin:/usr/bin:/bin:/usr/sbin:/sbin"));
        // From a terminal: the same PATH, unchanged.
        QCOMPARE(mergedPath("/opt/homebrew/bin:/usr/bin", "/opt/homebrew/bin:/usr/bin"), QString("/opt/homebrew/bin:/usr/bin"));
        // Given on purpose for this start: stays in front.
        QCOMPARE(mergedPath("/my/tools:/usr/bin:/bin", "/opt/homebrew/bin:/usr/bin:/bin"),
                 QString("/my/tools:/opt/homebrew/bin:/usr/bin:/bin"));
        QCOMPARE(mergedPath("", "/a:/b"), QString("/a:/b"));
        QCOMPARE(mergedPath("/a:/b", ""), QString("/a:/b"));
    }

    void onlyWhatTheProcessLacksIsAdded()
    {
        const QStringList shell{"PATH=/opt/homebrew/bin:/usr/bin:/bin", "SPICE_LIB_DIR=/models", "HOME=/home/shell",
                                "PWD=/somewhere", "SHLVL=3", "TERM=xterm", "_=/usr/bin/env", "OLDPWD=/x"};
        const QStringList process{"HOME=/home/me", "USER=me", "PATH=/usr/bin:/bin"};
        const QStringList changes = changesFor(shell, process, "/usr/bin:/bin");
        QCOMPARE(changes, (QStringList{"PATH=/opt/homebrew/bin:/usr/bin:/bin", "SPICE_LIB_DIR=/models"}));
        // Nothing new: nothing to change.
        QVERIFY(changesFor({"PATH=/usr/bin:/bin", "HOME=/x"}, process, "/usr/bin:/bin").isEmpty());
    }

#ifndef Q_OS_WIN
    void aScriptedShellIsReadAsALoginShell()
    {
        // Stands in for the login shell: whatever the arguments, it prints
        // a greeting and then the marker and an environment.
        const QString shell = writeScript("fake-shell.sh",
            "#!/bin/sh\n"
            "echo 'Welcome, this is the profile talking'\n"
            "printf '%s\\0' __QUCS_ENVIRONMENT__\n"
            "printf 'PATH=/from/shell/bin:/usr/bin:/bin\\0'\n"
            "printf 'QUCS_TEST_FROM_SHELL=yes\\0'\n"
            "printf 'HOME=/should/not/replace\\0'\n"
            "printf 'SHLVL=2\\0'\n");
        const QStringList entries = loginShellEnvironment(shell, 5000);
        QVERIFY2(entries.contains("QUCS_TEST_FROM_SHELL=yes"), qPrintable(entries.join(" | ")));
        QVERIFY(entries.contains("PATH=/from/shell/bin:/usr/bin:/bin"));

        const QString homeBefore = qEnvironmentVariable("HOME");
        const QString pathBefore = qEnvironmentVariable("PATH");
        qunsetenv("QUCS_TEST_FROM_SHELL");
        const QStringList changed = importLoginShellEnvironment(shell, 5000);
        QVERIFY2(changed.contains("QUCS_TEST_FROM_SHELL"), qPrintable(changed.join(", ")));
        QVERIFY(changed.contains("PATH"));
        QVERIFY(!changed.contains("HOME"));     // had one
        QVERIFY(!changed.contains("SHLVL"));    // a shell's own
        QCOMPARE(qEnvironmentVariable("QUCS_TEST_FROM_SHELL"), QString("yes"));
        QCOMPARE(qEnvironmentVariable("HOME"), homeBefore);
        QVERIFY(qEnvironmentVariable("PATH").contains("/from/shell/bin"));
        // ...and every directory the process had is still there.
        for (const QString& d : pathBefore.split(':', Qt::SkipEmptyParts))
            QVERIFY2(qEnvironmentVariable("PATH").split(':').contains(d), qPrintable(d));
        // A second import changes nothing more.
        QVERIFY(importLoginShellEnvironment(shell, 5000).isEmpty());
        qunsetenv("QUCS_TEST_FROM_SHELL");
        qputenv("PATH", pathBefore.toLocal8Bit());
    }

    void aShellThatHangsOrSaysNothingIsGivenUpOn()
    {
        const QString silent = writeScript("silent-shell.sh", "#!/bin/sh\nexit 0\n");
        QVERIFY(loginShellEnvironment(silent, 2000).isEmpty());
        const QString hanging = writeScript("hanging-shell.sh", "#!/bin/sh\nsleep 30\n");
        QElapsedTimer clock;
        clock.start();
        QVERIFY(loginShellEnvironment(hanging, 1000).isEmpty());
        QVERIFY(clock.elapsed() < 6000);      // two tries, a second each, not thirty
        // A shell that does not exist: /bin/sh stands in, and whatever it
        // adds, the process's PATH directories all stay.
        const QString pathBefore = qEnvironmentVariable("PATH");
        importLoginShellEnvironment(dir.filePath("no-such-shell"), 5000);
        for (const QString& d : pathBefore.split(':', Qt::SkipEmptyParts))
            QVERIFY2(qEnvironmentVariable("PATH").split(':').contains(d), qPrintable(d));
        qputenv("PATH", pathBefore.toLocal8Bit());
    }

    void theRealShellsAnswer()
    {
        // /bin/sh, and the account's shell when it is another one.
        QStringList shells{"/bin/sh"};
        const QString own = qEnvironmentVariable("SHELL");
        if (!own.isEmpty() && own != "/bin/sh") shells << own;
        for (const QString& shell : shells) {
            const QStringList entries = loginShellEnvironment(shell, 8000);
            QVERIFY2(!entries.isEmpty(), qPrintable(shell + ": nothing"));
            QStringList names;
            for (const QString& e : entries) names << e.section('=', 0, 0);
            QVERIFY2(names.contains("PATH"), qPrintable(shell + ": " + names.join(",")));
            QVERIFY2(names.contains("HOME"), qPrintable(shell));
        }
    }

    void theSwitchOffIsHonoured()
    {
        qputenv("QUCS_NO_SHELL_ENV", "1");
        QVERIFY(importLoginShellEnvironment("/bin/sh", 5000).isEmpty());
        qunsetenv("QUCS_NO_SHELL_ENV");
    }
#endif
};

QTEST_MAIN(TestShellEnvironment)
#include "test_shell_environment.moc"
