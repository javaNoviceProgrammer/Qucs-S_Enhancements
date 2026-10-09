/*
 * The commit a build is of (qucs_commit.h, QUCS_COMMIT, written at every
 * build by cmake/commit.cmake), which the About box, --version and a crash
 * report name. Read once when the tree was configured, it named that commit
 * long after - a build of 26.1.7 said 2ce3821, from weeks before - and the
 * About box named none.
 */
#include <QtTest>
#include <QTabWidget>
#include <QTextBrowser>
#include <QLabel>
#include <QProcess>
#include <QStandardPaths>

#include "config.h"
#include "qucs_commit.h"
#include "dialogs/aboutdialog.h"

class TestBuildCommit : public QObject
{
    Q_OBJECT

private slots:
    // The commit checked out where the sources are (the repository found
    // above them), when git and a repository are there.
    void theCommitIsTheOneCheckedOut()
    {
        const QString git = QStandardPaths::findExecutable("git");
        if (git.isEmpty()) QSKIP("no git here");
        QProcess p;
        p.start(git, {"-C", QStringLiteral(QUCS_EXAMPLES_DIR "/.."), "rev-parse", "--short=7", "HEAD"});
        QVERIFY(p.waitForFinished(30000));
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) QSKIP("the sources are in no repository");
        QCOMPARE(QStringLiteral(QUCS_COMMIT), QString::fromUtf8(p.readAllStandardOutput()).trimmed());
    }

    // Help > About names it after the version: "Version 26.1.7 (a4e23cf)".
    void theAboutBoxNamesIt()
    {
        AboutDialog about(nullptr);
        const QString commit = QStringLiteral(QUCS_COMMIT);
        const QString shown = commit.isEmpty() ? QStringLiteral("Version %1\n").arg(PACKAGE_VERSION)
                                               : QStringLiteral("Version %1 (%2)\n").arg(PACKAGE_VERSION, commit);
        QStringList texts;
        for (const QLabel* label : about.findChildren<QLabel*>()) texts << label->text();
        QVERIFY2(texts.contains(shown), qPrintable(texts.join(" | ").left(600)));
    }

    // The Authors tab: this build's team first - Meisam Bahadori, then
    // Claude Code -, Qucs-S's after it; so again when the tab is shown
    // again (the other names shuffled).
    void theEnhancedTeamComesFirst()
    {
        AboutDialog about(nullptr);
        QTextBrowser* authors = nullptr;
        for (QTextBrowser* b : about.findChildren<QTextBrowser*>())
            if (b->toPlainText().contains("Qucs-S project team:")) authors = b;
        QVERIFY(authors != nullptr);
        const auto check = [authors] {
            const QString text = authors->toPlainText();
            QVERIFY2(text.startsWith("Enhanced Qucs-S project team:"), qPrintable(text.left(200)));
            const qsizetype meisam = text.indexOf("Meisam Bahadori"), claude = text.indexOf("Claude Code"),
                            qucs = text.indexOf("\nQucs-S project team:");
            QVERIFY(meisam > 0 && claude > meisam && qucs > claude);
            QCOMPARE(text.count("Meisam Bahadori"), 1);
        };
        check();
        auto* tabs = about.findChild<QTabWidget*>();
        QVERIFY(tabs != nullptr);
        for (int k = 0; k < tabs->count(); ++k) tabs->setCurrentIndex(k);
        tabs->setCurrentIndex(tabs->indexOf(authors));
        check();
    }
};

QTEST_MAIN(TestBuildCommit)
#include "test_build_commit.moc"
