/***************************************************************************
                                 globals.cpp
                                -------------
    Application-wide state and the settings file. Split out of main.cpp so
    that the unit tests can link the core without the program entry point.
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <QApplication>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include "qucs.h"
#include "main.h"
#include "settings.h"
#include "misc.h"
#include "crashhandler.h"
#include "apptheme.h"
#include "extsimkernels/spicecompat.h"

tQucsSettings QucsSettings;

QucsApp *QucsMain = nullptr;  // the Qucs application itself
QString lastDir;    // to remember last directory for several dialogs
QStringList qucsPathList;
VersionTriplet QucsVersion; // Qucs version string

// #########################################################################
// Loads the settings file and stores the settings.
bool loadSettings()
{
    QucsSettingsFile settings;

    QucsSettings.DefaultSimulator = _settings::Get().item<int>("DefaultSimulator");
    QucsSettings.firstRun = _settings::Get().item<bool>("firstRun");

    /*** Temporarily continue to use QucsSettings to make sure all settings convert okay and remain compatible ***/
    QucsSettings.font.fromString(_settings::Get().item<QString>("font"));
    QucsSettings.appFont.fromString(_settings::Get().item<QString>("appFont"));
    QucsSettings.textFont.fromString(_settings::Get().item<QString>("textFont"));
    QucsSettings.largeFontSize = _settings::Get().item<double>("LargeFontSize");
    QucsSettings.maxUndo = _settings::Get().item<int>("maxUndo");
    QucsSettings.NodeWiring = _settings::Get().item<int>("NodeWiring");
    QucsSettings.BGColor = _settings::Get().item<QString>("BGColor");
    QucsSettings.Editor = _settings::Get().item<QString>("Editor");
    QucsSettings.FileTypes = _settings::Get().item<QStringList>("FileTypes");
    QucsSettings.Language = _settings::Get().item<QString>("Language");

    // Editor syntax highlighting settings.
    QucsSettings.Comment = _settings::Get().item<QString>("Comment");
    QucsSettings.String = _settings::Get().item<QString>("String");
    QucsSettings.Integer = _settings::Get().item<QString>("Integer");
    QucsSettings.Real = _settings::Get().item<QString>("Real");
    QucsSettings.Character = _settings::Get().item<QString>("Character");
    QucsSettings.Type = _settings::Get().item<QString>("Type");
    QucsSettings.Attribute = _settings::Get().item<QString>("Attribute");
    QucsSettings.Directive = _settings::Get().item<QString>("Directive");
    QucsSettings.Task = _settings::Get().item<QString>("Task");

    // TODO: Convert this to the new settings model.
    if(settings.contains("Qucsator")) {
        QucsSettings.Qucsator = settings.value("Qucsator").toString();
        QFileInfo inf(QucsSettings.Qucsator);
        QucsSettings.QucsatorDir = inf.canonicalPath() + QDir::separator();
        if (QucsSettings.Qucsconv.isEmpty())
            QucsSettings.Qucsconv = QStandardPaths::findExecutable("qucsconv_rf",{QucsSettings.QucsatorDir});
    } else {
        QucsSettings.Qucsator = QStandardPaths::findExecutable("qucsator_rf",{QucsSettings.BinDir});
        QucsSettings.QucsatorDir = QucsSettings.BinDir;
        if (QucsSettings.Qucsconv.isEmpty())
            QucsSettings.Qucsconv = QStandardPaths::findExecutable("qucsconv_rf",{QucsSettings.BinDir});
    }

    QucsSettings.AdmsXmlBinDir.setPath(_settings::Get().item<QString>("AdmsXmlBinDir"));
    QucsSettings.AscoBinDir.setPath(_settings::Get().item<QString>("AscoBinDir"));
    QucsSettings.NgspiceExecutable = _settings::Get().item<QString>("NgspiceExecutable");
    QucsSettings.XyceExecutable = _settings::Get().item<QString>("XyceExecutable");
    QucsSettings.XyceParExecutable = _settings::Get().item<QString>("XyceParExecutable");
    QucsSettings.SpiceOpusExecutable = _settings::Get().item<QString>("SpiceOpusExecutable");
    QucsSettings.NProcs = _settings::Get().item<int>("Nprocs");

    // TODO: Currently the default settings cannot include other settings during initialisation. This is a
    // problem for this setting as it needs to include the QucsWorkDir setting. Therefore, set the default to an
    // empty string and populate it here by brute force.
    QucsSettings.S4Qworkdir = _settings::Get().item<QString>("S4Q_workdir");
    if (QucsSettings.S4Qworkdir == "")
      QucsSettings.S4Qworkdir = QDir::toNativeSeparators(QucsSettings.QucsWorkDir.absolutePath()+"/spice4qucs");

    QucsSettings.OctaveExecutable = _settings::Get().item<QString>("OctaveExecutable");
    QucsSettings.OpenVAFExecutable = _settings::Get().item<QString>("OpenVAFExecutable");
    QucsSettings.PythonExecutable = _settings::Get().item<QString>("PythonExecutable");

    QucsSettings.RFLayoutExecutable = _settings::Get().item<QString>("RFLayoutExecutable");
    QucsSettings.ResolveSpicePrefix = _settings::Get().item<bool>("ResolveSpicePrefix");

    QucsSettings.qucsWorkspaceDir.setPath(_settings::Get().item<QString>("QucsHomeDir"));
    QucsSettings.QucsWorkDir = QucsSettings.qucsWorkspaceDir;
    QucsSettings.tempFilesDir.setPath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));

    QucsSettings.IgnoreFutureVersion = _settings::Get().item<bool>("IgnoreVersion");
    QucsSettings.GraphAntiAliasing = _settings::Get().item<bool>("GraphAntiAliasing");
    QucsSettings.TextAntiAliasing = _settings::Get().item<bool>("TextAntiAliasing");
    QucsSettings.fullTraceName = _settings::Get().item<bool>("fullTraceName");
    QucsSettings.alwaysPrefixDataset = _settings::Get().item<bool>("alwaysPrefixDataset");
    QucsSettings.ContentTreeView = _settings::Get().item<bool>("ContentTreeView");
    QucsSettings.ContentFolderIcons = _settings::Get().item<bool>("ContentFolderIcons");
    QucsSettings.ContentAutoRefresh = _settings::Get().item<bool>("ContentAutoRefresh");
    QucsSettings.ContentRefreshSeconds = qBound(1, _settings::Get().item<int>("ContentRefreshSeconds"), 3600);
    QucsSettings.SimulationConsoleHost = _settings::Get().item<int>("SimulationConsoleHost");
    if (!settings.contains("SimulationConsoleHost") && settings.contains("SimulationConsoleDock")
        && !settings.value("SimulationConsoleDock").toBool())   // the earlier two-way setting
        QucsSettings.SimulationConsoleHost = tQucsSettings::SimConsoleWindow;
    if (QucsSettings.SimulationConsoleHost < tQucsSettings::SimConsoleDock
        || QucsSettings.SimulationConsoleHost > tQucsSettings::SimConsoleLegacyWindow)
        QucsSettings.SimulationConsoleHost = tQucsSettings::SimConsoleDock;
    QucsSettings.Theme = qucs_s::apptheme::bounded(_settings::Get().item<int>("Theme"));
    QucsSettings.RecentProjects = _settings::Get().item<QString>("RecentProjects").split("*", Qt::SkipEmptyParts);
    QucsSettings.RecentDocs = _settings::Get().item<QString>("RecentDocs").split("*", Qt::SkipEmptyParts);
    QucsSettings.numRecentDocs = QucsSettings.RecentDocs.count();
    QucsSettings.spiceExtensions << "*.sp" << "*.cir" << "*.spc" << "*.spi";

    // If present read in the list of directory paths in which Qucs should
    // search for subcircuit schematics
    int npaths = settings.beginReadArray("Paths");
    for (int i = 0; i < npaths; ++i)
    {
        settings.setArrayIndex(i);
        QString apath = settings.value("path").toString();
        qucsPathList.append(apath);
    }
    settings.endArray();

    QucsSettings.numRecentDocs = 0;

    return true;
}

// #########################################################################
// Saves the settings in the settings file.
bool saveApplSettings()
{
    QucsSettingsFile settings;

    // Note: It is not really necessary to take the following reference, but it
    // arguably makes the code slightly cleaner - thoughts? To be clear:
    // qs.item<int>() is identical to _settings::get().item<int>()
    settingsManager& qs = _settings::Get();

    qs.setItem<int>("DefaultSimulator", QucsSettings.DefaultSimulator);
    qs.setItem<bool>("firstRun", false);
    qs.setItem<QString>("font", QucsSettings.font.toString());
    qs.setItem<QString>("appFont", QucsSettings.appFont.toString());
    qs.setItem<QString>("textFont", QucsSettings.textFont.toString());
    if (QucsMain != nullptr) {
      qs.setItem<QByteArray>("MainWindowGeometry", QucsMain->saveGeometry());
    }

    // store LargeFontSize as a string, so it will be also human-readable in the settings file (will be a @Variant() otherwise)
    qs.setItem<QString>("LargeFontSize", QString::number(QucsSettings.largeFontSize));
    qs.setItem<unsigned int>("maxUndo", QucsSettings.maxUndo);
    qs.setItem<unsigned int>("NodeWiring", QucsSettings.NodeWiring);
    qs.setItem<QString>("BGColor", QucsSettings.BGColor.name());
    qs.setItem<QString>("Editor", QucsSettings.Editor);
    qs.setItem<QStringList>("FileTypes", QucsSettings.FileTypes);
    qs.setItem<QString>("Language", QucsSettings.Language);
    qs.setItem<QString>("Comment", QucsSettings.Comment.name());
    qs.setItem<QString>("String", QucsSettings.String.name());
    qs.setItem<QString>("Integer", QucsSettings.Integer.name());
    qs.setItem<QString>("Real", QucsSettings.Real.name());
    qs.setItem<QString>("Character", QucsSettings.Character.name());
    qs.setItem<QString>("Type", QucsSettings.Type.name());
    qs.setItem<QString>("Attribute", QucsSettings.Attribute.name());
    qs.setItem<QString>("Directive", QucsSettings.Directive.name());
    qs.setItem<QString>("Task", QucsSettings.Task.name());
    qs.setItem<QString>("AdmsXmlBinDir", QucsSettings.AdmsXmlBinDir.canonicalPath());
    qs.setItem<QString>("AscoBinDir", QucsSettings.AscoBinDir.canonicalPath());
    qs.setItem<QString>("NgspiceExecutable",QucsSettings.NgspiceExecutable);
    qs.setItem<QString>("XyceExecutable",QucsSettings.XyceExecutable);
    qs.setItem<QString>("XyceParExecutable",QucsSettings.XyceParExecutable);
    qs.setItem<QString>("SpiceOpusExecutable",QucsSettings.SpiceOpusExecutable);
    qs.setItem<QString>("Qucsator",QucsSettings.Qucsator);
    qs.setItem<int>("Nprocs",QucsSettings.NProcs);
    qs.setItem<QString>("S4Q_workdir",QucsSettings.S4Qworkdir);
    qs.setItem<QString>("OctaveExecutable",QucsSettings.OctaveExecutable);
    qs.setItem<QString>("OpenVAFExecutable",QucsSettings.OpenVAFExecutable);
    qs.setItem<QString>("PythonExecutable",QucsSettings.PythonExecutable);
    qs.setItem<QString>("QucsHomeDir", QucsSettings.qucsWorkspaceDir.canonicalPath());
    qs.setItem<bool>("IgnoreVersion", QucsSettings.IgnoreFutureVersion);
    qs.setItem<bool>("GraphAntiAliasing", QucsSettings.GraphAntiAliasing);
    qs.setItem<bool>("TextAntiAliasing", QucsSettings.TextAntiAliasing);
    qs.setItem<bool>("fullTraceName",QucsSettings.fullTraceName);
    qs.setItem<bool>("alwaysPrefixDataset",QucsSettings.alwaysPrefixDataset);
    qs.setItem<bool>("ContentTreeView",QucsSettings.ContentTreeView);
    qs.setItem<bool>("ContentFolderIcons",QucsSettings.ContentFolderIcons);
    qs.setItem<bool>("ContentAutoRefresh",QucsSettings.ContentAutoRefresh);
    qs.setItem<int>("ContentRefreshSeconds",QucsSettings.ContentRefreshSeconds);
    qs.setItem<int>("SimulationConsoleHost",QucsSettings.SimulationConsoleHost);
    qs.setItem<int>("Theme",QucsSettings.Theme);

    // Copy the list of directory paths in which Qucs should
    // search for subcircuit schematics from qucsPathList
    settings.remove("Paths");
    settings.beginWriteArray("Paths");
    int i = 0;
    for (QString& path: qucsPathList) {
         settings.setArrayIndex(i);
         settings.setValue("path", path);
         i++;
     }
     settings.endArray();

  return true;
}

/*!
 * \brief qucsMessageOutput handles qDebug, qWarning, qCritical, qFatal.
 * \param type Message type (Qt enum)
 * \param msg Message
 *
 * The message handler is used to get control of the messages.
 * Particularly on Windows, as the messages are sent to the debugger and do not
 * show on the terminal. The handler could also be extended to create a log
 * mechanism.
 * <http://qt-project.org/doc/qt-4.8/debug.html#warning-and-debugging-messages>
 * <http://qt-project.org/doc/qt-4.8/qtglobal.html#qInstallMsgHandler>
 */
void qucsMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    qucs_s::crash::noteMessage(type, msg);   // kept for the crash report
    QByteArray localMsg = msg.toLocal8Bit();
    const char *file = context.file ? context.file : "";
    const char *function = context.function ? context.function : "";
    switch (type) {
        case QtDebugMsg:
            fprintf(stderr, "Debug: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
            break;
        case QtInfoMsg:
            fprintf(stderr, "Info: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
            break;
        case QtWarningMsg:
            fprintf(stderr, "Warning: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
            break;
        case QtCriticalMsg:
            fprintf(stderr, "Critical: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
            break;
        case QtFatalMsg:
            fprintf(stderr, "Fatal: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
            break;
    }
    fflush(stderr);
}
