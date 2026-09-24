/*
 * What the application prints while it starts, opens Application Settings
 * and draws its toolbars: the warnings of 26.1.2's first builds, gone at
 * their source - toolbar icons with blurs QtSvg could not draw at toolbar
 * size, the AdmsXml and ASCO folders nobody sets, the text font of a new
 * configuration, the library blacklist before a simulator is chosen, a
 * connection to a slot that does not exist, the consoles' font, and
 * macOS's hint about the icons' generic font family.
 */
#include <QtTest>
#include <QDirIterator>
#include <QFontDatabase>
#include <QIcon>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "config.h"
#include "qucs.h"
#include "module.h"
#include "main.h"
#include "misc.h"
#include "settings.h"
#include "processconsole.h"
#include "qucslib_common.h"
#include "simulationconsole.h"
#include "dialogs/qucssettingsdialog.h"
#include "extsimkernels/spicecompat.h"
#include "isolated_settings.h"

namespace {

struct MainGuard {
    explicit MainGuard(QucsApp* app) { QucsMain = app; }
    ~MainGuard() { QucsMain = nullptr; }
};

// The warnings printed while it lives (what the application's own
// handler prints: not the hint it leaves out).
class Warnings
{
public:
    Warnings() : m_previous(qInstallMessageHandler(&Warnings::handler)) { s_list = &m_list; }
    ~Warnings()
    {
        qInstallMessageHandler(m_previous);
        s_list = nullptr;
    }
    const QStringList& list() const { return m_list; }
    QString text() const { return m_list.join('\n'); }

private:
    static void handler(QtMsgType type, const QMessageLogContext& context, const QString& text)
    {
        if (s_list != nullptr && (type == QtWarningMsg || type == QtCriticalMsg)
            && !isGenericFontFamilyHint(context, text))
            s_list->append(text);
    }
    static inline QStringList* s_list = nullptr;
    QStringList m_list;
    QtMessageHandler m_previous;
};

} // namespace

class TestQuietStart : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath("settings"));
        QucsSettings.DefaultSimulator = spicecompat::simNgspice;
        QucsSettings.NgspiceExecutable = QStandardPaths::findExecutable("sh");
        QucsSettings.firstRun = false;
        QucsSettings.maxUndo = 20;
        QucsVersion = VersionTriplet(PACKAGE_VERSION);
        Module::registerModules();
    }

    // Every icon of the application at the sizes a toolbar, a menu or the
    // component panel draws it. A blur on a detail smaller than a pixel
    // gave QtSvg a buffer of no size: "The requested buffer size is too
    // big, ignoring", twelve times as the main window came up.
    void iconsDrawWithoutWarnings()
    {
        int icons = 0;
        QStringList noisy;
        QDirIterator it(":/bitmaps", {"*.svg"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            ++icons;
            Warnings warnings;
            for (int size : {16, 22, 24, 32, 48, 64}) {
                const QPixmap pixmap = QIcon(path).pixmap(QSize(size, size));
                QVERIFY2(!pixmap.isNull(), qPrintable(path));
            }
            if (!warnings.list().isEmpty())
                noisy << path + ": " + warnings.list().first();
        }
        QVERIFY(icons > 100);
        QVERIFY2(noisy.isEmpty(), qPrintable(noisy.join('\n')));
    }

    // A folder setting nobody has set (the AdmsXml and ASCO folders of most
    // configurations) is an empty path: canonicalPath() of it said "Empty
    // filename passed to function" every time Application Settings opened
    // and every time the settings were saved.
    void anUnsetFolderIsEmpty()
    {
        QDir unset;
        unset.setPath(QString());
        QDir set;
        set.setPath(dir.path());
        Warnings warnings;
        QCOMPARE(misc::canonicalDir(unset), QString());
        QCOMPARE(misc::canonicalDir(set), QDir(dir.path()).canonicalPath());
        QVERIFY2(warnings.list().isEmpty(), qPrintable(warnings.text()));
    }

    // Before a simulator is chosen (the first start) there is no blacklist
    // to read: the library tree opened a file of no name, twice.
    void noBlacklistBeforeASimulatorIsChosen()
    {
        const int simulator = QucsSettings.DefaultSimulator;
        QFile blacklist(dir.filePath("ngspice.blacklist"));
        QVERIFY(blacklist.open(QIODevice::WriteOnly));
        blacklist.write("Bipolar.lib\n\nDiodes.lib\n");
        blacklist.close();
        {
            Warnings warnings;
            QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
            QCOMPARE(getBlacklistedLibraries(dir.path()), QStringList());
            QucsSettings.DefaultSimulator = spicecompat::simNgspice;
            QCOMPARE(getBlacklistedLibraries(dir.path()), QStringList({"Bipolar.lib", "Diodes.lib"}));
            QVERIFY2(warnings.list().isEmpty(), qPrintable(warnings.text()));
        }
        QucsSettings.DefaultSimulator = simulator;
    }

    // A new configuration has no text font: reading it gave "QFont::
    // fromString: Invalid description '(empty)'" and left the font as it
    // was. It still keeps the font main() chose; a saved one is read.
    void aFontNotSavedYetKeepsItsDefault()
    {
        // loadSettings() reads every setting: this test's, as initTestCase
        // set them, come back afterwards
        struct Restore {
            tQucsSettings saved = QucsSettings;
            ~Restore() { QucsSettings = saved; }
        } restore;
        const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        QucsSettings.textFont = fixed;
        _settings::Get().remove("textFont");
        {
            Warnings warnings;
            QVERIFY(loadSettings());
            QCOMPARE(QucsSettings.textFont, fixed);
            QVERIFY2(!warnings.text().contains("Invalid description"), qPrintable(warnings.text()));
        }
        QFont saved = fixed;
        saved.setPointSize(fixed.pointSize() + 3);
        _settings::Get().setItem<QString>("textFont", saved.toString());
        QVERIFY(loadSettings());
        QCOMPARE(QucsSettings.textFont.pointSize(), saved.pointSize());
        _settings::Get().remove("textFont");
    }

    // Application Settings opened, applied and saved with the folders
    // unset: it connected the search path table to a slot it does not
    // have, and read the unset folders as above.
    void theSettingsDialogIsQuiet()
    {
        QucsApp app(false);
        MainGuard guard(&app);
        QucsSettings.AdmsXmlBinDir.setPath(QString());
        QucsSettings.AscoBinDir.setPath(QString());
        Warnings warnings;
        {
            QucsSettingsDialog dlg(&app);
            QVERIFY(QMetaObject::invokeMethod(&dlg, "slotApply"));
        }
        QVERIFY(saveApplSettings());
        QStringList relevant;
        for (const QString& w : warnings.list())
            if (w.contains("No such slot") || w.contains("No such signal")
                || w.contains("filename") || w.contains("file name"))
                relevant << w;
        QVERIFY2(relevant.isEmpty(), qPrintable(relevant.join('\n')));
        QCOMPARE(_settings::Get().item<QString>("AdmsXmlBinDir"), QString());
        QCOMPARE(_settings::Get().item<QString>("AscoBinDir"), QString());
    }

    // The icons' texts ask for the generic "sans-serif", which CoreText does
    // not have: on macOS, Qt listed the names of every font once and said
    // so, "Replace uses of missing font family". The application leaves
    // that hint out for "sans-serif" and keeps it for any other family.
    void theGenericFamilyHintIsLeftOut()
    {
        const QString generic = QStringLiteral(
            "Populating font family aliases took 52 ms. Replace uses of missing font family "
            "\"Sans-serif\" with one that exists to avoid this cost. ");
        QString other = generic;
        other.replace("Sans-serif", "Monospace");
        const QMessageLogContext fonts(nullptr, 0, nullptr, "qt.qpa.fonts");
        const QMessageLogContext elsewhere(nullptr, 0, nullptr, "default");
        QVERIFY(isGenericFontFamilyHint(fonts, generic));
        QVERIFY(!isGenericFontFamilyHint(fonts, other));
        QVERIFY(!isGenericFontFamilyHint(elsewhere, generic));
        QVERIFY(!isGenericFontFamilyHint(fonts, "Something else about \"Sans-serif\""));

        // and every icon with a text asks for that family, or none
        QStringList others;
        QDirIterator it(":/bitmaps", {"*.svg"}, QDir::Files, QDirIterator::Subdirectories);
        static const QRegularExpression text("<text[\\s>]");
        static const QRegularExpression family("font-family\\s*[:=]\\s*[\"']?([^;\"']+)");
        while (it.hasNext()) {
            QFile icon(it.next());
            QVERIFY(icon.open(QIODevice::ReadOnly));
            const QString svg = QString::fromUtf8(icon.readAll());
            if (!svg.contains(text))
                continue;
            for (auto m = family.globalMatch(svg); m.hasNext();) {
                const QString name = m.next().captured(1).trimmed();
                if (name.compare("sans-serif", Qt::CaseInsensitive) != 0)
                    others << icon.fileName() + ": " + name;
            }
        }
        QVERIFY2(others.isEmpty(), qPrintable(others.join('\n')));
    }

    // The consoles write in the system's fixed-pitch font, at 10 points.
    // They asked for a family named "monospace", which only fontconfig
    // knows: on macOS and Windows Qt searched every font for it and said
    // so. (The offscreen platform's fixed font is itself "monospace", so
    // here this checks the font is the platform's, not the warning.)
    void theConsolesUseTheFixedFont()
    {
        const QString family = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
        ProcessConsole terminal;
        QCOMPARE(terminal.outputView()->font().family(), family);
        QCOMPARE(terminal.outputView()->font().pointSize(), 10);
        QucsApp app(false);
        MainGuard guard(&app);
        QVERIFY(app.simulationConsole() != nullptr);
        QCOMPARE(app.simulationConsole()->console()->font().family(), family);
        QCOMPARE(app.simulationConsole()->console()->font().pointSize(), 10);
    }
};

QTEST_MAIN(TestQuietStart)
#include "test_quiet_start.moc"
