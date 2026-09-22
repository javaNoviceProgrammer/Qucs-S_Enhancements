/***************************************************************************
                                 main.cpp
                                ----------
    begin                : Thu Aug 28 2003
    copyright            : (C) 2003 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
/*!
 * \file main.cpp
 * \brief Implementation of the main application.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <ctype.h>
#include <locale.h>
#include <iostream>

#include <QApplication>
#include <QSettings>
#include <QString>
#include <QStringList>
//#include <QTextCodec>
#include <QTranslator>
#include <QFile>
#include <QMessageBox>
#include <QRegularExpression>
#include <QtSvg>
#include <QCommandLineParser>
#include <QTextStream>
#include <QScopedPointer>

#include "qucs.h"
#include "main.h"
#include "node.h"
#include "printerwriter.h"
#include "imagewriter.h"
#include "schematic.h"
#include "settings.h"
#include "module.h"
#include "misc.h"
#include "autosave.h"
#include "crashhandler.h"
#include "shellenvironment.h"
#include "apptheme.h"
#include "systemopen.h"


#include "extsimkernels/ngspice.h"
#include "extsimkernels/xyce.h"
#include "extsimkernels/CdlNetlistWriter.h"

#if defined(_WIN32) ||defined(__MINGW32__)
#include <windows.h>  //for OutputDebugString
#endif

#if defined(_WIN32) ||defined(__MINGW32__)
#define executableSuffix ".exe"
#else
#define executableSuffix ""
#endif

// Application-wide globals, loadSettings() and saveApplSettings() live in
// globals.cpp so that the unit tests can link the core without main().

Schematic* openSchematic(const QString& schematicFileName)
{
    qDebug() << "*** try to load schematic :" << schematicFileName;

    QFile file(schematicFileName);  // save simulator messages
    if (file.open(QIODevice::ReadOnly))
    {
        file.close();
    }
    else
    {
        fprintf(stderr, "Error: Could not load schematic %s\n", schematicFileName.toLatin1().data());
        return nullptr;
    }

    // populate Modules list
    //Module::registerModules ();

    // new schematic from file
    Schematic *schematic = new Schematic(nullptr, schematicFileName);

    // load schematic file if possible
    if (!schematic->loadDocument())
    {
        fprintf(stderr, "Error: Could not load schematic %s\n", schematicFileName.toLatin1().data());
        delete schematic;
        return nullptr;
    }

    return schematic;
}

int doNetlist(QString schematicFileName, QString netlistFileName, bool netlist2Console)
{
    QucsSettings.DefaultSimulator = spicecompat::simQucsator;
    Module::registerModules();

    QScopedPointer<Schematic> schematic(openSchematic(schematicFileName));
    if (!schematic)
    {
        return 1;
    }

    if (!netlist2Console)
    {
        qDebug() << "*** try to write netlist  :" << netlistFileName;
    }

    QScopedPointer<QFile> netlistFile;
    QScopedPointer<QTextStream> netlistStream;
    QScopedPointer<QString> netlistString;

    if (!netlist2Console)
    {
        netlistFile.reset(new QFile(netlistFileName));
        if (!netlistFile->open(QIODevice::WriteOnly))
        {
            fprintf(stderr, "Error: Could not load netlist %s\n", netlistFileName.toLatin1().data());
            return -1;
        }

        netlistStream.reset(new QTextStream(netlistFile.get()));
    }
    else
    {
        netlistString.reset(new QString);
        netlistStream.reset(new QTextStream(netlistString.get()));
    }

    QPlainTextEdit errText;  // dummy
    QStringList Collect;  // clear list for NodeSets, SPICE components etc.
    int SimPorts = schematic->prepareNetlist(*netlistStream, Collect, &errText);

    if (SimPorts < -5)
    {
        QByteArray ba = netlistFileName.toLatin1();
        fprintf(stderr, "Error: Could not prepare netlist %s\n", ba.data());
        /// \todo better handling for error/warnings
        qCritical() << errText.toPlainText();
        return 1;
    }

    // output NodeSets, SPICE simulations etc.
    for (QStringList::Iterator it = Collect.begin(); it != Collect.end(); ++it)
    {
        // don't put library includes into netlist...
        if ((*it).right(4) != ".lst" &&
            (*it).right(5) != ".vhdl" &&
            (*it).right(4) != ".vhd" &&
            (*it).right(2) != ".v")
        {
            *netlistStream << *it << '\n';
        }
    }

    *netlistStream << '\n';

    schematic->createNetlist(*netlistStream, SimPorts);

    if (netlist2Console)
    {
        std::cout << std::endl << netlistString->toLatin1().constData() << std::endl;
    }

    return 0;
}

int runNgspice(QString schematicFileName, QString dataset)
{
    QucsSettings.DefaultSimulator = spicecompat::simNgspice;
    Module::registerModules();

    QScopedPointer<Schematic> schematic(openSchematic(schematicFileName));
    if (!schematic)
    {
        return 1;
    }

    QScopedPointer<Ngspice> ngspice(new Ngspice(schematic.get()));
    ngspice->slotSimulate();
    bool ok = ngspice->waitEndOfSimulation();
    if (!ok)
    {
        fprintf(stderr, "Ngspice timed out or start error!\n");
        return -1;
    }
    else
    {
        ngspice->convertToQucsData(dataset);
    }

    return 0;
}

int runXyce(QString schematicFileName, QString dataset)
{
    QucsSettings.DefaultSimulator = spicecompat::simXyce;
    Module::registerModules();

    QScopedPointer<Schematic> schematic(openSchematic(schematicFileName));
    if (!schematic)
    {
        return 1;
    }

    QScopedPointer<Xyce> xyce(new Xyce(schematic.get()));
    xyce->slotSimulate();
    bool ok = xyce->waitEndOfSimulation();
    if (!ok)
    {
        fprintf(stderr, "Xyce timed out or start error!\n");
        return -1;
    }
    else
    {
        xyce->convertToQucsData(dataset);
    }

    return 0;
}

int doNgspiceNetlist(QString schematicFileName, QString netlistFileName, bool netlist2Console)
{
    QucsSettings.DefaultSimulator = spicecompat::simNgspice;
    Module::registerModules();

    QScopedPointer<Schematic> schematic(openSchematic(schematicFileName));
    if (!schematic)
    {
        return 1;
    }

    QScopedPointer<Ngspice> ngspice(new Ngspice(schematic.get()));
    ngspice->SaveNetlist(netlistFileName, netlist2Console);

    if (!netlist2Console && !QFile::exists(netlistFileName))
    {
        return -1;
    }

    return 0;
}

int doCdlNetlist(
        QString schematicFileName,
        QString netlistFileName,
        bool netlist2Console,
        bool resolveSpicePrefix)
{
    QucsSettings.DefaultSimulator = spicecompat::simNgspice;
    Module::registerModules();

    QScopedPointer<Schematic> schematic(openSchematic(schematicFileName));
    if (!schematic)
    {
        return -1;
    }

    QScopedPointer<QTextStream> netlistStream;
    QScopedPointer<QString> netlistString;
    QScopedPointer<QFile> cdlFile;

    if (netlist2Console)
    {
        netlistString.reset(new QString());
        netlistStream.reset(new QTextStream(netlistString.get()));
    }
    else
    {
        cdlFile.reset(new QFile(netlistFileName));

        if (cdlFile->open(QFile::WriteOnly))
        {
            netlistStream.reset(new QTextStream(cdlFile.get()));
        }
        else
        {
            QString msg = QStringLiteral(
                    "Tried to save netlist \nto %1\n(could not open for writing!)")
                .arg(netlistFileName);
            QString finalMsg = QStringLiteral(
                    "%1\n This could be an error in the QSettings settings file\n"
                    "(usually in ~/.config/qucs/qucs_s.conf)\n"
                    "The value for S4Q_workdir (default:/spice4qucs) needs to be writeable!")
                .arg(msg);
            QMessageBox::critical(nullptr, QStringLiteral("Problem with SaveNetlist"), finalMsg, QMessageBox::Ok);

            return -1;
        }
    }

    CdlNetlistWriter cdlWriter(*netlistStream, schematic.get(), resolveSpicePrefix);
    if (!cdlWriter.write())
    {
        QMessageBox::critical(
                nullptr,
                QStringLiteral("Save CDL netlist"),
                QStringLiteral("Save CDL netlist failed!"),
                QMessageBox::Ok);
        return -1;
    }

    if (netlist2Console)
    {
        std::cout << std::endl << netlistString->toUtf8().constData() << std::endl;
    }
    else
    {
        if (!QFile::exists(netlistFileName))
        {
            return -1;
        }
    }

    return 0;
}

int doXyceNetlist(QString schematicFileName, QString netlistFileName, bool netlist2Console)
{
    QucsSettings.DefaultSimulator = spicecompat::simXyce;
    Module::registerModules();
    QScopedPointer<Schematic> schematic(openSchematic(schematicFileName));

    if (!schematic)
    {
        return 1;
    }

    QScopedPointer<Xyce> xyce(new Xyce(schematic.get()));
    xyce->SaveNetlist(netlistFileName, netlist2Console);

    if (!netlist2Console && !QFile::exists(netlistFileName))
    {
        return -1;
    }

    return 0;
}

int doPrint(QString schematicFileName, QString printFile,
    QString page, int dpi, QString color, QString orientation)
{
    // Rendering does not depend on a simulator. Every component loads
    // whichever one is selected, but a part of another simulator is drawn
    // in grey; with simNotSpecified none is, so a print looks the same
    // whatever simulator the settings name.
    QucsSettings.DefaultSimulator = spicecompat::simNotSpecified;
    Module::registerModules();
    QScopedPointer<Schematic> schematic(openSchematic(schematicFileName));
    if (!schematic)
    {
        return 1;
    }

    schematic->a_Nodes = &(schematic->a_DocNodes);
    schematic->a_Wires = &(schematic->a_DocWires);
    schematic->a_Diagrams = &(schematic->a_DocDiags);
    schematic->a_Paintings = &(schematic->a_DocPaints);
    schematic->a_Components = &(schematic->a_DocComps);
    schematic->reloadGraphs();

    qDebug() << "*** try to print file  :" << printFile;

    // determine filetype
    if (printFile.endsWith(".pdf")) {
        //initial printer
        PrinterWriter *Printer = new PrinterWriter();
        Printer->setFitToPage(true);
        Printer->noGuiPrint(schematic.get(), printFile, page, dpi, color, orientation);
    }
    else
    {
        ImageWriter *Printer = new ImageWriter("");
        Printer->noGuiPrint(schematic.get(), printFile, color);
    }

    return 0;
}

/*!
 * \brief createIcons Create component icons (png) from command line.
 */
void createIcons() {

  int nCats = 0, nComps = 0;

  if(!QDir("./bitmaps_generated").exists()){
    QDir().mkdir("bitmaps_generated");
  }
  Module::registerModules ();
  QStringList cats = Category::getCategories ();

  for (const QString& category: cats) {

    QList<Module *> Comps;
    Comps = Category::getModules(category);

    // crash with diagrams, skip
    if(category == "diagrams") break;

    char * File;
    QString Name;

    for (Module *Mod: Comps) {
      if (Mod->info) {

        Element *e = (Mod->info) (Name, File, true);

        Component *c = (Component* ) e;

        QList<qucs::Line *> Lines      = c->Lines;
        QList<struct qucs::Arc *> Arcs = c-> Arcs;
        QList<qucs::Rect *> Rects      = c-> Rects;
        QList<qucs::Ellips *> Ellips     = c-> Ellipses;
        QList<Port *> Ports      = c->Ports;
        QList<Text*> Texts       = c->Texts;

        QGraphicsScene *scene = new QGraphicsScene();

        for (qucs::Line *l : Lines) {
          scene->addLine(l->x1, l->y1, l->x2, l->y2, l->style);
        }

        for (struct qucs::Arc *a: Arcs) {
          // we need an open item here; QGraphisEllipseItem draws a filled ellipse and doesn't do the job here...
          QPainterPath *path = new QPainterPath();
          // the components do not contain the angles in degrees but in 1/16th degrees -> conversion needed
          path->arcMoveTo(a->x,a->y,a->w,a->h,a->angle/16);
          path->arcTo(a->x,a->y,a->w,a->h,a->angle/16,a->arclen/16);
          scene->addPath(*path);
        }

        for(qucs::Rect *a: Rects) {
          scene->addRect(a->x, a->y, a->w, a->h, a->Pen, a->Brush);
        }

        for(qucs::Ellips *a: Ellips) {
          scene->addEllipse(a->x, a->y, a->w, a->h, a->Pen, a->Brush);
        }

        for(Port *p: Ports) {
          scene->addEllipse(p->x-4, p->y-4, 8, 8, QPen(Qt::red));
        }

        for(Text *t: Texts) {
          QFont myFont;
          myFont.setPointSize(10);
          QGraphicsTextItem* item  = new QGraphicsTextItem(t->s);
          item->setX(t->x);
          item->setY(t->y);
          item->setFont(myFont);

          scene->addItem(item);
        }

        // this uses the size of the component as icon size
        // Qt bug ? The returned sceneRect() is often 1 px short on bottom
        //   and right sides without anti-aliasing. 1 px more missing on top
        //   and left when anti-aliasing is used
        QRectF rScene = scene->sceneRect().adjusted(-1,-1,1,1);
        // image and scene need to be the same size, since render()
        //   will fill the entire image, otherwise the scaling will
        //   introduce artifacts
        QSize sImage = rScene.size().toSize(); // rounding seems not to be an issue
        // ARGB32_Premultiplied is faster (Qt docs)
        //QImage image(sImage.toSize(), QImage::Format_ARGB32);
        QImage image(sImage, QImage::Format_ARGB32_Premultiplied);
        // this uses a fixed size for the icon (32 x 32)
        //QImage image(32, 32, QImage::Format_ARGB32);
        image.fill(Qt::transparent);

        QPainter painter(&image);
        QPainter::RenderHints hints = QPainter::RenderHints();
        // Ask to antialias drawings if requested
        if (QucsSettings.GraphAntiAliasing) hints |= QPainter::Antialiasing;
        // Ask to antialias text if requested
        if (QucsSettings.TextAntiAliasing) hints |= QPainter::TextAntialiasing;
        painter.setRenderHints(hints);

        // pass target and source size eplicitly, otherwise sceneRect() is used
        //   for the source size, which is often wrong (see comment above)
        scene->render(&painter, image.rect(), rScene);

        image.save("./bitmaps_generated/" + QString(File) + ".png");

        fprintf(stdout, "[%s] %s\n", category.toLatin1().data(), File);
      }
      nComps++;
    } // module
    nCats++;
  } // category
  fprintf(stdout, "Created %i component icons from %i categories\n", nComps, nCats);
}

/*!
 * \brief createDocData Create data used for documentation.
 *
 * It creates the following:
 *  - list of categories: categories.txt
 *  - category directory, ex.: ./lumped components/
 *    - CSV with component data fields. Ex [component#]_data.csv
 *    - CSV with component properties. Ex [component#]_props.csv
 */
void createDocData() {

  QMap<int, QString> typeMap;
  typeMap.insert(0x30000, "Component");
  typeMap.insert(0x30002, "ComponentText");
  typeMap.insert(0x10000, "AnalogComponent");
  typeMap.insert(0x20000, "DigitalComponent") ;

  Module::registerModules ();
  QStringList cats = Category::getCategories ();
  int nCats = cats.size();

  QStringList catHeader;
  catHeader << "# Note: auto-generated file (changes will be lost on update)";
  QFile file("categories.txt");
  if (!file.open(QFile::WriteOnly | QFile::Text)) return;
  QTextStream out(&file);
  out << cats.join("\n");
  file.close();

  int nComps = 0;

  // table for quick reference, schematic and netlist entry
  for (const QString& category: cats) {

    QList<Module *> Comps;
    Comps = Category::getModules(category);

    // \fixme, crash with diagrams, skip
    if(category == "diagrams") break;

    // one dir per category
    QString curDir = "./"+category+"/";
    qDebug() << "Creating dir:" << curDir;
    if(!QDir(curDir).exists()){
        QDir().mkdir(curDir);
    }

    char * File;
    QString Name;

    int num = 0; // component id inside category

    for (Module *Mod: Comps) {
        num += 1;

        nComps += 1;

        Element *e = (Mod->info) (Name, File, true);
        Component *c = (Component* ) e;

        // object info
        QStringList compData;

        compData << "# Note: auto-generated file (changes will be lost on update)";
        compData << "Caption; "           + Name;
        compData << "Description; "       + c->Description;
        compData << "Identifier; ``"      + c->Model + "``"; // backticks for reST verbatim
        compData << "Default name; ``"    + c->Name  + "``";
        compData << "Type; "              + typeMap.value(c->Type);
        compData << "Bitmap file; "       + QString(File);
        compData << "Properties; "        + QString::number(c->Props.count());
        compData << "Category; "          + category;

        // 001_data.csv - CSV file with component data
        QString ID = QStringLiteral("%1").arg(num,3,10,QChar('0'));
        QString objDataFile;
        objDataFile = QStringLiteral("%1_data.csv").arg( ID  ) ;

        QFile file(curDir + objDataFile);
        if (!file.open(QFile::WriteOnly | QFile::Text)) return;
        QTextStream out(&file);
        out << compData.join("\n");
        file.close();
        fprintf(stdout, "[%s] %s %s \n", category.toLatin1().data(), c->Model.toLatin1().data(), file.fileName().toLatin1().data());

        QStringList compProps;
        compProps << "# Note: auto-generated file (changes will be lost on update)";
        compProps << QStringLiteral("# %1; %2; %3; %4").arg(  "Name", "Value", "Display", "Description");
        for (Property *prop : c->Props) {
          compProps << QStringLiteral("%1; \"%2\"; %3; \"%4\"").arg(
                         prop->Name,
                         prop->Value,
                         prop->display?"yes":"no",
                         prop->Description.replace("\"","\"\"")); // escape quote in quote
        }

        // 001_props.csv - CSV file with component properties
        QString objPropFile = QStringLiteral("%1_prop.csv").arg( ID ) ;

        QFile fileProps(curDir + objPropFile );
        if (!fileProps.open(QFile::WriteOnly | QFile::Text)) return;
        QTextStream outProps(&fileProps);
        outProps << compProps.join("\n");
        compProps.clear();
        file.close();
        fprintf(stdout, "[%s] %s %s \n", category.toLatin1().data(), c->Model.toLatin1().data(), fileProps.fileName().toLatin1().data());
    } // module
  } // category
  fprintf(stdout, "Created data for %i components from %i categories\n", nComps, nCats);
}

/*!
 * \brief createListNetEntry prints to stdout the available netlist formats
 *
 * Prints the default component entries format for:
 *  - Qucs schematic
 *  - Qucsator netlist
 */
void createListComponentEntry(){

  Module::registerModules ();
  QStringList cats = Category::getCategories ();
  // table for quick reference, schematic and netlist entry
  for (QString category: cats) {

    QList<Module *> Comps;
    Comps = Category::getModules(category);

    // \fixme, crash with diagrams, skip
    if(category == "diagrams") break;

    char * File;
    QString Name;

    for (Module *Mod: Comps) {
      Element *e = (Mod->info) (Name, File, true);
      Component *c = (Component* ) e;

      QString qucsEntry = c->save();
      fprintf(stdout, "%s; qucs    ; %s\n", c->Model.toLatin1().data(), qucsEntry.toLatin1().data());

      // add dummy ports/wires, avoid segfault
      int port = 0;
      for (Port *p: c->Ports) {
        Node *n = new Node(0,0);
        n->Name="_net"+QString::number(port);
        p->Connection = n;
        port +=1;
      }

      // skip Subcircuit, segfault, there is nothing to netlist
      if (c->Model == "Sub" or c->Model == ".Opt") {
        fprintf(stdout, "WARNING, qucsator netlist not generated for %s\n\n", c->Model.toLatin1().data());
        continue;
      }

      QString qucsatorEntry = c->getNetlist();
      fprintf(stdout, "%s; qucsator; %s\n", c->Model.toLatin1().data(), qucsatorEntry.toLatin1().data());
      } // module
    } // category
}

// #########################################################################
// ##########                                                     ##########
// ##########                  Program Start                      ##########
// ##########                                                     ##########
// #########################################################################
int main(int argc, char *argv[])
{
    qInstallMessageHandler(qucsMessageOutput);
    // set the Qucs version string
    QucsVersion = VersionTriplet(PACKAGE_VERSION);

    // apply default settings
    //QucsSettings.font = QFont("Helvetica", 12);
    QucsSettings.largeFontSize = 16.0;
    QucsSettings.maxUndo = 20;
    QucsSettings.NodeWiring = 0;

    // initially center the application
    QApplication app(argc, argv);
    // From here on, what the system asks to open is kept for the main
    // window: on macOS a document opened from the Finder or the Dock comes
    // as an event, not an argument, and may come before there is a window.
    qucs_s::systemopen::Receiver systemOpen;
    //QDesktopWidget *d = app.desktop();
    QucsSettings.font = QApplication::font();
    QucsSettings.appFont = QApplication::font();
    QucsSettings.textFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QucsSettings.font.setPointSize(12);

    // default
    QString QucsWorkdirPath = QDir::homePath()+QDir::toNativeSeparators ("/QucsWorkspace");
    QucsSettings.qucsWorkspaceDir.setPath(QucsWorkdirPath);
    QucsSettings.QucsWorkDir.setPath(QucsSettings.qucsWorkspaceDir.canonicalPath());

    // QUCS_SETTINGS_DIR=<dir>: this run keeps its settings in an INI file
    // under <dir> instead of the user's own store - for trying a build out
    // without touching one's preferences. Before the first use of the store.
    // Its crash reports, autosave copies and running-session marker are
    // kept there too: a trial run that is killed must not have the user's
    // own next start report an unclean exit, or offer its documents.
    const QString settingsDir = qEnvironmentVariable("QUCS_SETTINGS_DIR");
    if (!settingsDir.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
        qucs_s::crash::setReportDirectory(settingsDir + QStringLiteral("/crash-reports"));
        qucs_s::autosave::setDirectory(settingsDir + QStringLiteral("/autosave"));
    }

    // load existing settings (if any)
    loadSettings();

    /* restore saved style */
    QString savedStyle = _settings::Get().item<QString>("AppStyle");
    QStyle* style = QStyleFactory::create(savedStyle);
    if (style) {
        QApplication::setStyle(style);
    }
    /* restore saved style */
    qucs_s::apptheme::apply(QucsSettings.Theme);

    QDir().mkpath(QucsSettings.qucsWorkspaceDir.absolutePath());
    QDir().mkpath(QucsSettings.tempFilesDir.absolutePath());

    // continue to set up overrides or default settings (some are saved on exit)

    // check for relocation env variable
    QDir QucsDir;
    QString QucsApplicationPath = QCoreApplication::applicationDirPath();
#ifdef __APPLE__
    // The resources sit next to the executable (qucs-s.app/Contents/MacOS/
    // share) or, for the tools in Contents/MacOS/bin and for a plain
    // prefix install (prefix/bin), one level up. Cutting the path at the
    // first "/bin" took a "bin" anywhere above the bundle for that.
    QucsDir = QDir(QucsApplicationPath);
    if (QucsDir.dirName() == QLatin1String("bin")) QucsDir.cdUp();
#else
    QucsDir = QDir(QucsApplicationPath);
    QucsDir.cdUp();
#endif

    QucsSettings.BinDir = QucsApplicationPath.contains("bin") ?
                            (QucsApplicationPath + QDir::separator()) : QucsDir.absoluteFilePath("bin/");
    QucsSettings.LangDir = QucsDir.canonicalPath() + "/share/" QUCS_NAME "/lang/";

    QucsSettings.LibDir = QucsDir.canonicalPath() + "/share/" QUCS_NAME "/library/";
    QucsSettings.SpiceLibDir = QucsDir.canonicalPath() + "/share/" QUCS_NAME "/spicelibrary/";
    QucsSettings.OctaveDir = QucsDir.canonicalPath() + "/share/" QUCS_NAME "/octave/";
    QucsSettings.ExamplesDir = QucsDir.canonicalPath() + "/share/" QUCS_NAME "/examples/";
    QucsSettings.DocDir = QucsDir.canonicalPath() + "/share/" QUCS_NAME "/docs/";
    QucsSettings.Editor = "qucs";

    /// \todo Make the setting up of all executables below more consistent
    char *var = nullptr; // Don't use QUCSDIR with Qucs-S
    var = getenv("QUCSATOR");
    if (var != nullptr) {
        QucsSettings.QucsatorVar = QString(var);
    }
    else {
        QucsSettings.QucsatorVar = "";
    }

    var = getenv("QUCSCONV");
    if (var != nullptr) {
        QucsSettings.Qucsconv = QString(var);
    }

    var = getenv("ADMSXMLBINDIR");
    if (var != nullptr) {
        QucsSettings.AdmsXmlBinDir.setPath(QString(var));
    }
    else {
        // default admsXml bindir same as Qucs
        QString admsExec;
#if defined(_WIN32) || defined(__MINGW32__)
        admsExec = QDir::toNativeSeparators(QucsSettings.BinDir+"/"+"admsXml.exe");
#else
        admsExec = QDir::toNativeSeparators(QucsSettings.BinDir+"/"+"admsXml");
#endif
        QFile adms(admsExec);
        if (adms.exists())
        {
            QucsSettings.AdmsXmlBinDir.setPath(QucsSettings.BinDir);
        }
    }

    var = getenv("ASCOBINDIR");
    if (var != nullptr) {
        QucsSettings.AscoBinDir.setPath(QString(var));
    }
    else {
        // default ASCO bindir same as Qucs
        QString ascoExec;
#if defined(_WIN32) || defined(__MINGW32__)
        ascoExec = QDir::toNativeSeparators(QucsSettings.BinDir+"/"+"asco.exe");
#else
        ascoExec = QDir::toNativeSeparators(QucsSettings.BinDir+"/"+"asco");
#endif
        QFile asco(ascoExec);
        if (asco.exists())
        {
            QucsSettings.AscoBinDir.setPath(QucsSettings.BinDir);
        }
    }

    var = getenv("QUCS_OCTAVE");
    if (var != nullptr) {
        QucsSettings.QucsOctave = QString(var);
    } else {
        QucsSettings.QucsOctave.clear();
    }

    if(!QucsSettings.BGColor.isValid())
      QucsSettings.BGColor.setRgb(255, 250, 225);

    // syntax highlighting
    if(!QucsSettings.Comment.isValid())
        QucsSettings.Comment = Qt::gray;
    if(!QucsSettings.String.isValid())
        QucsSettings.String = Qt::red;
    if(!QucsSettings.Integer.isValid())
        QucsSettings.Integer = Qt::blue;
    if(!QucsSettings.Real.isValid())
        QucsSettings.Real = Qt::darkMagenta;
    if(!QucsSettings.Character.isValid())
        QucsSettings.Character = Qt::magenta;
    if(!QucsSettings.Type.isValid())
        QucsSettings.Type = Qt::darkRed;
    if(!QucsSettings.Attribute.isValid())
        QucsSettings.Attribute = Qt::darkCyan;
    if(!QucsSettings.Directive.isValid())
        QucsSettings.Directive = Qt::darkCyan;
    if(!QucsSettings.Task.isValid())
        QucsSettings.Task = Qt::darkRed;

    QucsSettings.sysDefaultFont = QApplication::font();
    QApplication::setFont(QucsSettings.appFont);

    QTranslator tor(nullptr);
    QString lang = QucsSettings.Language;
    if (lang.isEmpty()) {
        QLocale loc;
        lang = loc.name();
//      lang = QTextCodec::locale();
    }
    static_cast<void>(tor.load( QStringLiteral("qucs_") + lang, QucsSettings.LangDir));
    QApplication::installTranslator( &tor );

    // This seems to be necessary on a few system to make strtod()
    // work properly !???!
    setlocale (LC_NUMERIC, "C");

#ifdef GIT
    const QString applicationVersion(QString::fromUtf8("qucs s%1 (%2)").arg(PACKAGE_VERSION).arg(GIT));
#else
    const QString applicationVersion(QString::fromUtf8("Qucs %1").arg(PACKAGE_VERSION));
#endif

    QCoreApplication::setApplicationVersion(applicationVersion);
    QStringList cmdArgs;
    for (int i = 0; i < argc; ++i)
    {
        cmdArgs << argv[i];
    }

    QCommandLineParser parser;
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    parser.addVersionOption();

    parser.addOptions({
        {{"h", "help"}, QCoreApplication::translate("main", "display this help and exit")},
        {{"n", "netlist"}, QCoreApplication::translate("main", "convert Qucs schematic into netlist")},
        {{"p", "print"}, QCoreApplication::translate("main", "print Qucs schematic to file (eps needs inkscape)")},
        {"page", QCoreApplication::translate("main", "set print page size (default A4)"), "A4|A3|B4|B5", "A4"},
        {"dpi", QCoreApplication::translate("main", "set dpi value (default 96)"), "NUMBER", "96"},
        {"color", QCoreApplication::translate("main", "set color mode (default RGB)"), "RGB|BW", "RGB"},
        {"orin", QCoreApplication::translate("main", "set orientation (default portraid)"), "portraid|landscape", "portraid"},
        {"i", QCoreApplication::translate("main", "use file as input schematic"), "FILENAME"},
        {"o", QCoreApplication::translate("main", "use file as output netlist"), "FILENAME"},
        {"ngspice", QCoreApplication::translate("main", "create Ngspice netlist")},
        {"cdl", QCoreApplication::translate("main", "create CDL netlist")},
        {"xyce", QCoreApplication::translate("main", "Xyce netlist")},
        {"run", QCoreApplication::translate("main", "execute Ngspice/Xyce immediately")},
        {"icons", QCoreApplication::translate("main", "create component icons under ./bitmaps_generated")},
        {"doc", QCoreApplication::translate(
                "main",
                "dump data for documentation:\n"
                "* file with of categories: categories.txt\n"
                "* one directory per category (e.g. ./lumped\n"
                "   components/)\n"
                "   - CSV file with component data\n"
                "     ([comp#]_data.csv)\n"
                "   - CSV file with component properties.\n"
                "     ([comp#]_props.csv)"
                )
        },
        {"list-entries", QCoreApplication::translate("main", "list component entry formats for schematic and netlist")},
        {{"c", "netlist2Console"}, QCoreApplication::translate("main", "write netlist to console")},
        {{"x", "spiceprefix"}, QCoreApplication::translate("main", "resolve spice prefix during netlist CDL")},
    });

    parser.process(cmdArgs);

    if (parser.isSet("help"))
    {
        // some modification of the Qt-generated usage text

        QString helpText = parser.helpText();
        helpText.insert(
                helpText.indexOf('\n', 0)+1,
                QString::fromUtf8(
                    "       qucs -n -i FILENAME -o FILENAME\n"
                    "       qucs -p -i FILENAME -o FILENAME.[pdf|png|svg|eps]\n"));

        QRegularExpression optIndent(QString::fromUtf8("--page|--dpi|--color|--orin|--ngspice|--xyce|--run|--cdl"));
        int idx;
        int from = 0;
        while ((idx = helpText.indexOf(optIndent, from)) != -1)
        {
            helpText.insert(idx, "  ");
            from = idx +3;
        }

        std::cout << helpText.toUtf8().constData();
        exit(0);
    }

    const bool netlist_flag(parser.isSet("netlist"));
    const bool print_flag(parser.isSet("print"));
    const QString page(parser.value("page"));
    const int dpi(parser.value("dpi").toInt());
    const QString color(parser.value("color"));
    const QString orientation(parser.value("orin"));
    const QString inputfile(parser.value("i"));
    const QString outputfile(parser.value("o"));
    const bool ngspice_flag(parser.isSet("ngspice"));
    const bool cdl_flag(parser.isSet("cdl"));
    const bool xyce_flag(parser.isSet("xyce"));
    const bool run_flag(parser.isSet("run"));
    const bool netlist2Console(parser.isSet("netlist2Console"));
    const bool spiceprefix(parser.isSet("spiceprefix"));

#if 0
    std::cout << "Current cli values:" << std::endl;
    std::cout << "netlist_flag: " << netlist_flag << std::endl;
    std::cout << "print_flag: " << print_flag << std::endl;
    std::cout << "page: " << page.toUtf8().constData() << std::endl;
    std::cout << "dpi: " << dpi << std::endl;
    std::cout << "color: " << color.toUtf8().constData() << std::endl;
    std::cout << "orientation: " << orientation.toUtf8().constData() << std::endl;
    std::cout << "inputfile: " << inputfile.toUtf8().constData() << std::endl;
    std::cout << "outputfile: " << outputfile.toUtf8().constData() << std::endl;
    std::cout << "ngspice_flag: " << ngspice_flag << std::endl;
    std::cout << "cdl_flag: " << cdl_flag << std::endl;
    std::cout << "xyce_flag: " << xyce_flag << std::endl;
    std::cout << "run_flag: " << run_flag << std::endl;
    std::cout << "netlist2Console: " << netlist2Console << std::endl;
    std::cout << "spiceprefix: " << spiceprefix << std::endl;
    std::cout << "icons: " << parser.isSet("icons") << std::endl;
    std::cout << "doc: " << parser.isSet("doc") << std::endl;
    std::cout << "list-entries: " << parser.isSet("list-entries") << std::endl;

    exit(0);
#endif

    if (parser.isSet("icons"))
    {
        createIcons();
        return 0;
    }

    if (parser.isSet("doc"))
    {
        createDocData();
        return 0;
    }

    if (parser.isSet("list-entries"))
    {
        createListComponentEntry();
        return 0;
    }

    // check operation and its required arguments
    if (netlist_flag and print_flag)
    {
        fprintf(stderr, "Error: --print and --netlist cannot be used together\n");
        return -1;
    }
    else if (cdl_flag and run_flag)
    {
        fprintf(stderr, "Error: --cdl and --run cannot be used together\n");
        return -1;
    }
    else if (((ngspice_flag || xyce_flag || cdl_flag) && print_flag) || (run_flag && print_flag))
    {
        fprintf(stderr, "Error: --print and Ngspice/CDL/Xyce cannot be used together\n");
        return -1;
    }
    else if (netlist_flag or print_flag)
    {
        if (inputfile.isEmpty())
        {
            fprintf(stderr, "Error: Expected input file.\n");
            return -1;
        }
        if (!netlist2Console && outputfile.isEmpty())
        {
            fprintf(stderr, "Error: Expected output file.\n");
            return -1;
        }
        // create netlist from schematic
        if (netlist_flag)
        {
            if (!run_flag)
            {
                if (ngspice_flag)
                {
                    return doNgspiceNetlist(inputfile, outputfile, netlist2Console);
                }
                else if (cdl_flag)
                {
                    return doCdlNetlist(inputfile, outputfile, netlist2Console, spiceprefix);
                }
                else if (xyce_flag)
                {
                    return doXyceNetlist(inputfile, outputfile, netlist2Console);
                }
                else
                {
                    return doNetlist(inputfile, outputfile, netlist2Console);
                }
            }
            else
            {
                if (ngspice_flag)
                {
                    return runNgspice(inputfile, outputfile);
                }
                else if (xyce_flag)
                {
                    return runXyce(inputfile, outputfile);
                }
                else
                {
                    return 1;
                }
            }
        }
        else if (print_flag)
        {
            return doPrint(inputfile, outputfile, page, dpi, color, orientation);
        }
    }

    // Started from the Finder, the Dock or a desktop menu, the process has
    // a bare environment: bring in the login shell's (PATH, exports), as
    // a terminal would have given, before anything looks for a program.
    // QUCS_NO_SHELL_ENV=1 leaves the environment as it is.
    const QStringList imported = qucs_s::shellenv::importLoginShellEnvironment();
    if (!imported.isEmpty())
        qInfo().noquote() << "environment from the login shell:" << imported.join(QLatin1String(", "));

    // From here on a crash writes a report and rescues modified documents.
    const bool crashedLastTime = qucs_s::crash::markSessionStart();
    qucs_s::crash::install([] {
        if (QucsMain != nullptr)
            QucsMain->autosaveAll(/*emergency=*/true);
    });

    QucsMain = new QucsApp(netlist2Console);
    //1a.setMainWidget(QucsMain);

    QucsMain->show();
    // The documents and project named on the command line: qucs-s FILE...,
    // a desktop's file manager (qucs-s.desktop), Windows' file types (the
    // installer). The -i of the batch modes is taken as one of them.
    QStringList named = parser.positionalArguments();
    if (!inputfile.isEmpty()) named.prepend(inputfile);
    if (!named.isEmpty()) QucsMain->openFromSystem(named);
    QucsMain->recoverPreviousSession(crashedLastTime);
    systemOpen.setTarget([](const QStringList &items) {
        if (QucsMain != nullptr) QucsMain->openFromSystem(items);
    });
    int result = app.exec();
    //saveApplSettings(QucsMain);

    // A clean exit: every document was saved or discarded by the user, so
    // the autosave copies are stale, and the session marker comes down.
    qucs_s::autosave::clear();
    qucs_s::crash::markSessionEnd();
    return result;
}
