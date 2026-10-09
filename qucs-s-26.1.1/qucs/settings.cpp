#include <QApplication>
#include <QStandardPaths>

#include "main.h"
#include "settings.h"
#include "extsimkernels/spicecompat.h"

settingsManager::settingsManager()
    :QucsSettingsFile()
{
    // qDebug() << this << " created " << organizationName() << " " << applicationName();

    initAliases();
    initDefaults();
}

settingsManager::~settingsManager()
{
    // qDebug() << this << " destroyed";
}

void settingsManager::resetDefaults(const QString &group)
{
    qDebug() << "Reset settings group " << group;

    if (group == "All") {
        // Remove all settings (including those for which there is no default).
        // clear();

        // Repopulate with known defaults.
        for (auto const& item : m_Defaults) {
            qDebug() << "Resetting item " << item.first << " " << item.second;
            setValue(item.first, item.second);
        }
    }

    else {
        beginGroup(group);
        for (const QString& key : allKeys()) {
            setValue(key, m_Defaults[key]);
        }
    }
}

void settingsManager::initDefaults()
{
    m_Defaults["DefaultSimulator"] = spicecompat::simNotSpecified;
    m_Defaults["firstRun"] = true;
    m_Defaults["font"] = QApplication::font();
    m_Defaults["appFont"] = QApplication::font();
    m_Defaults["LargeFontSize"] = static_cast<double>(16.0);
    m_Defaults["GridColor"] = QColor(qRgb(25, 25, 25));
    m_Defaults["DefaultGraphLineWidth"] = "1";
    m_Defaults["maxUndo"] = 20;
    m_Defaults["AutosaveInterval"] = 120;   // seconds; 0 disables autosave
    m_Defaults["QucsHomeDir"] = QDir::homePath() + QDir::toNativeSeparators("/QucsWorkspace");

#ifdef Q_OS_WIN
    m_Defaults["NgspiceExecutable"] = "ngspice_con.exe";
    m_Defaults["XyceExecutable"] = "Xyce.exe";
    m_Defaults["RFLayoutExecutable"] = "qucsrflayout.exe";
    m_Defaults["OctaveExecutable"] = "octave.exe";
#else
    m_Defaults["NgspiceExecutable"] = "ngspice";
    #ifndef Q_OS_MACOS
        m_Defaults["XyceExecutable"] = "/usr/local/Xyce-Release-6.8.0-OPENSOURCE/bin/Xyce";
    #else
        m_Defaults["XyceExecutable"] = "Xyce";
    #endif
    m_Defaults["RFLayoutExecutable"] = "qucsrflayout";
    m_Defaults["OctaveExecutable"] = "octave";
#endif

    m_Defaults["XyceParExecutable"] = "mpirun -np %p /usr/local/Xyce-Release-6.8.0-OPENMPI-OPENSOURCE/bin/Xyce";
    // (In a test's own cache when it names one, as misc::cacheDir() has it:
    // a headless simulation of a test - qucs-s -n --run, qucs.simulate() -
    // wrote its netlist into the user's.)
    m_Defaults["S4Q_workdir"] = QDir::toNativeSeparators(
                                (qEnvironmentVariableIsEmpty("QUCS_CACHE_DIR")
                                     ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                                     : qEnvironmentVariable("QUCS_CACHE_DIR"))
                                + "/qucs-s");
    m_Defaults["Nprocs"] = 4;
    m_Defaults["SpiceOpusExecutable"] = "spiceopus";
    m_Defaults["SimParameters"] = "";
    m_Defaults["GraphAntiAliasing"] = false;
    m_Defaults["TextAntiAliasing"] = false;
    m_Defaults["fullTraceName"] = false;
    m_Defaults["ContentTreeView"] = true;   // sub-trees per folder
    m_Defaults["AnyFolderIsProject"] = false;
    m_Defaults["RestoreWorkspace"] = true;
    m_Defaults["RestoreProject"] = true;
    m_Defaults["RestoreDocuments"] = true;
    m_Defaults["RestorePanels"] = true;
    m_Defaults["RestoreWindowGeometry"] = true;
    m_Defaults["ContentFolderIcons"] = false;
    m_Defaults["ContentAutoRefresh"] = true;
    m_Defaults["ContentRefreshSeconds"] = 3;
    m_Defaults["FileNameCap"] = 50;
    m_Defaults["ComponentSymbols"] = 0;   // qucs_s::symbols::US
    m_Defaults["PythonToolbar"] = true;              // shown while a Python script is in front
    m_Defaults["PythonMessagesAtLineEnds"] = false;  // PythonDoc::messagesAtLineEnds()
    m_Defaults["PythonCompleteAsYouType"] = true;    // PythonDoc::completeAsYouType()
    m_Defaults["PythonInterpreters"] = QStringList();   // chosen with Browse on the Python toolbar
    m_Defaults["PythonAutoClose"] = true;            // PythonDoc::autoClose(): brackets and quotes closed as typed
    m_Defaults["PythonInlinePlots"] = true;          // matplotlib's figures in the Python Plots pane
    m_Defaults["PythonTypeChecker"] = QString("auto");   // PythonDoc::typeChecker(): auto, mypy, pyright or off
    m_Defaults["PythonRunSettings"] = QString("{}");   // a script's arguments, folder, environment (qucs_s::python::RunSettings)
    m_Defaults["PythonDebugLibraryCode"] = false;    // the debugger steps into Python's library and packages too
    m_Defaults["PythonFormatOnSave"] = false;        // PythonDoc::formatOnSave()
    m_Defaults["PythonWatches"] = QStringList();      // the debugger's watch list (PythonRunConsole::watches())
    m_Defaults["PythonInlineValues"] = true;         // PythonDoc::inlineValuesShown(): values at line ends while stopped
    m_Defaults["PythonBreakOnRaised"] = false;       // the debugger stops where an exception is raised, caught or not
    m_Defaults["ShowPinNames"] = true;
    m_Defaults["ShowPinDirections"] = false;
    m_Defaults["EmbedVerilogAInLibraries"] = true;
    m_Defaults["LibraryGroundPin"] = false;
    m_Defaults["WriteTextDocSettings"] = true;
    m_Defaults["WheelZooms"] = true;
    m_Defaults["ColourWires"] = false;
    m_Defaults["PaperFollowsTheme"] = false;
    m_Defaults["DiagramTheme"] = "";   // the theme new diagrams start with (diagramtheme.h)
    m_Defaults["GridMode"] = 0;
    m_Defaults["SimulationConsoleHost"] = 0;   // tQucsSettings::SimConsoleDock
    m_Defaults["RequireGround"] = true;
    m_Defaults["CheckCommands"] = false;   // tQucsSettings::CheckCommands
    m_Defaults["NgspiceMathFuncs"] = true;   // tQucsSettings::NgspiceMathFuncs
    m_Defaults["DatasetBinary"] = true;      // tQucsSettings::DatasetBinary
    m_Defaults["DatasetTextLimitMB"] = 10;   // tQucsSettings::DatasetTextLimitMB
    m_Defaults["Theme"] = 0;   // qucs_s::apptheme::System
    m_Defaults["LockToolbars"] = false;
    m_Defaults["NgspiceCompatMode"] = spicecompat::NgspDefault;
    m_Defaults["AllowFlexibleWires"] = false;
    m_Defaults["AllowLayingWiresAnew"] = false;
}

void settingsManager::initAliases()
{
    m_Aliases["IgnoreVersion"] = QStringList({"IngnoreVersion"});
}
