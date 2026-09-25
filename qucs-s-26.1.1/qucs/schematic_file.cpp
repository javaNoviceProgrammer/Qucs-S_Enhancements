/***************************************************************************
                              schematic_file.cpp
                             --------------------
    begin                : Sat Mar 27 2004
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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <QtCore>
#include <QMessageBox>
#include <QDir>
#include <QStringList>
#include <QPlainTextEdit>
#include <QTextStream>
#include <QList>
#include <QProcess>
#include <QDebug>

#include "main.h"
#include "node.h"
#include "wire.h"
#include "schematic.h"
#include "diagrams/diagrams.h"
#include "paintings/paintings.h"
#include "components/spicefile.h"
#include "components/vhdlfile.h"
#include "components/verilogfile.h"
#include "components/libcomp.h"
#include "components/sparamfile.h"
#include "module.h"
#include "misc.h"
#include "conductor_index.h"
#include "extsimkernels/abstractspicekernel.h"
#include "extsimkernels/s2spice.h"
#include "vamodule.h"
#include <QJsonDocument>
#include <QJsonObject>

#include <optional>


// Here the subcircuits, SPICE components etc are collected. It must be
// global to also work within the subcircuits.
SubMap FileList;


// -------------------------------------------------------------
// Creates a Qucs file format (without document properties) in the returning
// string. This is used to copy the selected elements into the clipboard.
QString Schematic::createClipboardFile()
{
  int z=0;  // counts selected elements

  QString s("<Qucs Schematic " PACKAGE_VERSION ">\n");

  // Build element document.
  s += "<Components>\n";
  for(Component* pc : *a_Components)
    if(pc->isSelected) {
      s += pc->save()+"\n";  z++; }
  s += "</Components>\n";

  s += "<Wires>\n";
  for(Wire* pw : *a_Wires)
    if(pw->isSelected) {
      z++;
      if(pw->hasLabel()) if(!pw->label()->isSelected) {
        s += pw->save().section('"', 0, 0)+"\"\" 0 0 0>\n";
        continue;
      }
      s += pw->save()+"\n";
    }
  for(Node *pn : *a_Nodes)
    if(pn->hasLabel()) if(pn->label()->isSelected) {
      s += pn->label()->save()+"\n";  z++; }
  s += "</Wires>\n";

  s += "<Diagrams>\n";
  for(Diagram* pd : *a_Diagrams)
    if(pd->isSelected) {
      s += pd->save()+"\n";  z++; }
  s += "</Diagrams>\n";

  s += "<Paintings>\n";
  for(Painting* pp : *a_Paintings)
    if(pp->isSelected)
      if ((a_isSymbolOnly && pp->Name.startsWith(".PortSym")) || pp->Name.at(0) != '.') {  // subcircuit specific -> do not copy
        s += "<"+pp->save()+">\n";  z++; }
  s += "</Paintings>\n";

  if(z == 0) return "";   // return empty if no selection

  return s;
}

// -------------------------------------------------------------
// Only read fields without loading them.
bool Schematic::loadIntoNothing(QTextStream *stream)
{
  QString Line, cstr;
  while(!stream->atEnd()) {
    Line = stream->readLine();
    if(Line.startsWith("</")) return true;
  }

  misc::reportError(QObject::tr("Format Error:\n'Painting' field is not closed!"));
  return false;
}

// -------------------------------------------------------------
// Paste from clipboard.
bool Schematic::pasteFromClipboard(QTextStream *stream, std::list<Element*> *pe)
{
  // First, check if clipboard contains image data or file path
  QClipboard* clipboard = QApplication::clipboard();
  // Null when the platform has no clipboard (offscreen, some Wayland
  // compositors) or nothing was ever copied.
  const QMimeData* mimeData = clipboard->mimeData();

  // Check for image data first
  if (mimeData && mimeData->hasImage()) {
    QImage clipboardImage = clipboard->image();
    if (!clipboardImage.isNull()) {
      // Create an ImagePainting with the clipboard image
      ImagePainting* imagePainting = new ImagePainting();

      // Convert QImage to QPixmap and set it
      QPixmap pixmap = QPixmap::fromImage(clipboardImage);

      // Set the image data directly in the ImagePainting
      imagePainting->setImageFromPixmap(pixmap);
      imagePainting->setPlacement(0, 0, clipboardImage.width(), clipboardImage.height());

      // Add to the elements list
      pe->push_back(imagePainting);

      return true;
    }
  }

  // Check for text that might be a file path to an image
  if (mimeData && mimeData->hasText()) {
    QString clipboardText = clipboard->text().trimmed();

    // Check if the text looks like a file path and if it's an image file
    if (!clipboardText.isEmpty() && isImageFilePath(clipboardText)) {
      ImagePainting* imagePainting = new ImagePainting();
      imagePainting->setImageFromPath(clipboardText);

      if (!imagePainting->embeddedImage().isNull()) {
        imagePainting->setPlacement(0, 0, imagePainting->getImageWidth(),
                                    imagePainting->getImageHeight());
        pe->push_back(imagePainting);
        return true;
      }
      delete imagePainting;
    }
  }

  // Check for file URLs (drag and drop from file manager)
  if (mimeData && mimeData->hasUrls()) {
    QList<QUrl> urls = mimeData->urls();
    for (const QUrl& url : urls) {
      if (url.isLocalFile()) {
        QString filePath = url.toLocalFile();
        if (isImageFilePath(filePath)) {
          ImagePainting* imagePainting = new ImagePainting();
          imagePainting->setImageFromPath(filePath);

          if (imagePainting->embeddedImage().isNull()) {
            delete imagePainting;
            continue;
          }

          // Set position (offset multiple images if there are several)
          const int defaultX = 100 + (pe->size() * 20); // Offset each image
          const int defaultY = 100 + (pe->size() * 20);
          imagePainting->setPlacement(defaultX, defaultY,
                                      defaultX + imagePainting->getImageWidth(),
                                      defaultY + imagePainting->getImageHeight());

          // Add to the elements list
          pe->push_back(imagePainting);
        }
      }
    }

    // If we processed any image files, return true
    if (!pe->empty()) {
      return true;
    }
  }

  // If no image in clipboard, proceed with normal text-based clipboard processing
  QString Line;
  Line = stream->readLine();
  if(Line.left(16) != "<Qucs Schematic ")   // wrong file type ?
    return false;
  QString s = PACKAGE_VERSION;
  Line = Line.mid(16, Line.length()-17);
  if(Line != s) {  // wrong version number ?
    misc::reportError(QObject::tr("Wrong document version: ")+Line);
    return false;
  }
  // read content in symbol edit mode *************************
  if(a_symbolMode) {
    while(!stream->atEnd()) {
      Line = stream->readLine();
      if(Line == "<Components>") {
        if(!loadIntoNothing(stream)) return false; }
      else
      if(Line == "<Wires>") {
        if(!loadIntoNothing(stream)) return false; }
      else
      if(Line == "<Diagrams>") {
        if(!loadIntoNothing(stream)) return false; }
      else
      if(Line == "<Paintings>") {
        if(!loadPaintings(stream, (std::list<Painting*>*)pe)) return false; }
      else {
        misc::reportError(QObject::tr("Clipboard Format Error:\nUnknown field!"));
        return false;
      }
    }
    return true;
  }
  // read content in schematic edit mode *************************
  while(!stream->atEnd()) {
    Line = stream->readLine();
    if(Line == "<Components>") {
      if(!loadComponents(stream, (std::list<Component*>*)pe)) return false; }
    else
    if(Line == "<Wires>") {
      if(!loadWires(stream, pe)) return false; }
    else
    if(Line == "<Diagrams>") {
      if(!loadDiagrams(stream, (std::list<Diagram*>*)pe)) return false; }
    else
    if(Line == "<Paintings>") {
      if(!loadPaintings(stream, (std::list<Painting*>*)pe)) return false; }
    else {
      misc::reportError(QObject::tr("Clipboard Format Error:\nUnknown field!"));
      return false;
    }
  }
  return true;
}

bool Schematic::isImageFilePath(const QString& path) {
  return qucs_s::EmbeddedImage::isSupportedFile(path);
}


// -------------------------------------------------------------
// Writes this schematic's symbol into a file of its own - the same shape
// a ".sym" document is saved in - so that another schematic can take it.
bool Schematic::saveSymbolToFile(const QString& path)
{
  QFile file(path);
  if(!file.open(QIODevice::WriteOnly)) return false;

  QTextStream stream(&file);
  stream << "<Qucs Schematic " << PACKAGE_VERSION << ">\n";
  stream << "<Symbol>\n";
  for(auto* pp : a_SymbolPaints)
    stream << "  <" << pp->save() << ">\n";
  stream << "</Symbol>\n";
  stream.flush();

  const bool ok = file.error() == QFile::NoError;
  file.close();
  return ok;
}

// -------------------------------------------------------------
// Replaces the drawing of this schematic's symbol with the one in
// another file (a ".sym", or the symbol of a schematic). The ports stay
// as this schematic has them - their numbers belong to it - but each
// moves to where the file puts the port of the same number. Returns
// what went wrong, or an empty string.
QString Schematic::loadSymbolFromFile(const QString& path)
{
  QFile file(path);
  if(!file.open(QIODevice::ReadOnly))
    return QObject::tr("Cannot read \"%1\".").arg(path);

  QString text = QString::fromUtf8(file.readAll());
  file.close();

  QTextStream stream(&text, QIODevice::ReadOnly);
  QString line = stream.readLine().trimmed();
  if(!line.startsWith("<Qucs Schematic "))
    return QObject::tr("\"%1\" is not a Qucs-S document.").arg(path);

  while(!stream.atEnd()) {
    line = stream.readLine().trimmed();
    if(line == "<Symbol>") break;
  }
  if(line != "<Symbol>")
    return QObject::tr("\"%1\" holds no symbol.").arg(path);

  std::list<Painting*> loaded;
  if(!loadPaintings(&stream, &loaded)) {
    for(auto* pp : loaded) delete pp;
    return QObject::tr("The symbol in \"%1\" could not be read.").arg(path);
  }
  if(loaded.empty()) {
    return QObject::tr("The symbol in \"%1\" is empty.").arg(path);
  }

  // Out with the old drawing, the ports excepted.
  for(auto pp = a_SymbolPaints.begin(); pp != a_SymbolPaints.end();) {
    if((*pp)->Name == ".PortSym ") { ++pp; continue; }
    delete *pp;
    pp = a_SymbolPaints.erase(pp);
  }

  for(Painting* pp : loaded) {
    if(pp->Name != ".PortSym ") {
      a_SymbolPaints.push_back(pp);
      continue;
    }
    // A port of the incoming symbol places ours of the same number; one
    // this schematic does not have is dropped, and one it has that the
    // file does not place simply stays where it was.
    const PortSymbol* incoming = static_cast<PortSymbol*>(pp);
    for(auto* mine : a_SymbolPaints)
      if(mine->Name == ".PortSym "
         && static_cast<PortSymbol*>(mine)->numberStr == incoming->numberStr) {
        static_cast<PortSymbol*>(mine)->placeLike(*incoming);
        break;
      }
    delete pp;
  }

  adjustPortNumbers();   // the names and directions are this schematic's
  return QString();
}

// -------------------------------------------------------------
int Schematic::saveSymbolCpp (void)
{
  QFileInfo info (a_DocName);
  QString cppfile = info.absolutePath () + QDir::separator() + a_DataSet;
  QFile file (cppfile);

  if (!file.open (QIODevice::WriteOnly)) {
    misc::reportError(QObject::tr("Cannot save C++ file \"%1\"!").arg(cppfile));
    return -1;
  }

  QTextStream stream (&file);

  // automatically compute boundings of drawing
  int xmin = INT_MAX;
  int ymin = INT_MAX;
  int xmax = INT_MIN;
  int ymax = INT_MIN;
  int x1, y1;
  int maxNum = 0;

  stream << "  // symbol drawing code\n";
  for (auto* pp : a_SymbolPaints) {
    if (pp->Name == ".ID ") continue;
    if (pp->Name == ".PortSym ") {
      if (((PortSymbol*)pp)->numberStr.toInt() > maxNum)
        maxNum = ((PortSymbol*)pp)->numberStr.toInt();
      x1 = ((PortSymbol*)pp)->cx;
      y1 = ((PortSymbol*)pp)->cy;
      if (x1 < xmin) xmin = x1;
      if (x1 > xmax) xmax = x1;
      if (y1 < ymin) ymin = y1;
      if (y1 > ymax) ymax = y1;
      continue;
    }
    auto br = pp->boundingRect();
    xmin = std::min(xmin, br.left());
    xmax = std::max(xmax, br.left() + br.width());
    ymin = std::min(ymin, br.top());
    ymax = std::max(ymax, br.top() + br.height());

    stream << "  " << pp->saveCpp () << "\n";
  }

  stream << "\n  // terminal definitions\n";
  for (int i = 1; i <= maxNum; i++) {
    for (auto* pp : a_SymbolPaints) {
      if (pp->Name == ".PortSym ") {
        if (((PortSymbol*)pp)->numberStr.toInt() == i) {
          stream << "  " << pp->saveCpp () << "\n";
        }
      }
    }
  }

  stream << "\n  // symbol boundings\n"
    << "  x1 = " << xmin << "; " << "  y1 = " << ymin << ";\n"
    << "  x2 = " << xmax << "; " << "  y2 = " << ymax << ";\n";

  stream << "\n  // property text position\n";
  for (auto* pp : a_SymbolPaints)
    if (pp->Name == ".ID ")
      stream << "  " << pp->saveCpp () << "\n";

  file.close ();
  return 0;
}

int Schematic::savePropsJSON()
{
  QFileInfo info (a_DocName);
  const QString base = info.absolutePath() + QDir::separator() + info.baseName();
  const QString vafilename = base + ".va";
  const QString osdifile = base + ".osdi";
  const QString jsonfile = base + "_props.json";

  QFile vafile(vafilename);
  if (!vafile.open (QIODevice::ReadOnly)) {
    misc::reportError(QObject::tr("Cannot open Verilog-A file \"%1\"!").arg(vafilename));
    return -1;
  }
  const QString source = QString::fromUtf8(vafile.readAll());
  vafile.close();

  // The library OpenVAF built knows the parameters best (their units, the
  // instance ones) - if it was built from this source; the source itself
  // otherwise, and before there is a library.
  qucs_s::vamodule::VerilogModule module;
  const QFileInfo osdi(osdifile);
  QString why;
  const bool fromLibrary = osdi.exists() && osdi.lastModified() >= QFileInfo(vafilename).lastModified()
                           && qucs_s::vamodule::readOsdi(osdifile, info.baseName(), &module, &why);
  if (!why.isEmpty())
    qWarning().noquote() << why << "- the parameters are read from" << vafilename;
  if (!fromLibrary)
    module = qucs_s::vamodule::readSource(source, info.baseName());
  if (module.name.isEmpty()) {
    misc::reportError(QObject::tr("There is no module in the Verilog-A file \"%1\"!").arg(vafilename));
    return -1;
  }

  QFile file (jsonfile);
  if (!file.open (QIODevice::WriteOnly)) {
    misc::reportError(QObject::tr("Cannot save JSON props file \"%1\"!").arg(jsonfile));
    return -1;
  }
  file.write(QJsonDocument(qucs_s::vamodule::propsObject(module)).toJson(QJsonDocument::Indented));
  return 0;
}

// save symbol paintings in JSON format
int Schematic::saveSymbolJSON()
{
  QFileInfo info (a_DocName);
  QString jsonfile = info.absolutePath () + QDir::separator()
                   + info.baseName() + "_sym.json";

  qDebug() << "saveSymbolJson for " << jsonfile;

  QFile file (jsonfile);

  if (!file.open (QIODevice::WriteOnly)) {
    misc::reportError(QObject::tr("Cannot save JSON symbol file \"%1\"!").arg(jsonfile));
    return -1;
  }

  QTextStream stream (&file);

  // automatically compute boundings of drawing
  int xmin = INT_MAX;
  int ymin = INT_MAX;
  int xmax = INT_MIN;
  int ymax = INT_MIN;
  int x1, y1;
  int maxNum = 0;

  stream << "{\n";

  stream << "\"paintings\" : [\n";

  // symbol drawing code"
  for (auto* pp : a_SymbolPaints) {
    if (pp->Name == ".ID ") continue;
    if (pp->Name == ".PortSym ") {
      if (((PortSymbol*)pp)->numberStr.toInt() > maxNum)
        maxNum = ((PortSymbol*)pp)->numberStr.toInt();
      x1 = ((PortSymbol*)pp)->cx;
      y1 = ((PortSymbol*)pp)->cy;
      if (x1 < xmin) xmin = x1;
      if (x1 > xmax) xmax = x1;
      if (y1 < ymin) ymin = y1;
      if (y1 > ymax) ymax = y1;
      continue;
    }
    auto br = pp->boundingRect();
    xmin = std::min(xmin, br.left());
    xmax = std::max(xmax, br.left() + br.width());
    ymin = std::min(ymin, br.top());
    ymax = std::max(ymax, br.top() + br.height());
    stream << "  " << pp->saveJSON() << "\n";
  }

  // terminal definitions
  //stream << "terminal \n";
  for (int i = 1; i <= maxNum; i++) {
    for (auto* pp : a_SymbolPaints) {
      if (pp->Name == ".PortSym ") {
        if (((PortSymbol*)pp)->numberStr.toInt() == i) {
          stream << "  " << pp->saveJSON () << "\n";
        }
      }
    }
  }

  stream << "],\n"; //end of paintings JSON array

  // symbol boundings
  stream
    << "  \"x1\" : " << xmin << ",\n" << "  \"y1\" : " << ymin << ",\n"
    << "  \"x2\" : " << xmax << ",\n" << "  \"y2\" : " << ymax << ",\n";

  // property text position
  for (auto* pp : a_SymbolPaints)
    if (pp->Name == ".ID ")
      stream << "  " << pp->saveJSON () << "\n";

  stream << "}\n";

  file.close ();
  return 0;


}

// -------------------------------------------------------------
// Returns the number of subcircuit ports.
bool Schematic::writeTo(const QString& path)
{
  return writeDocument(path);
}

// Serialises the document (or, for a .sym document, only its symbol) into
// the given file. Everything that saveDocument() does beyond that - the
// Verilog-A symbol exports - is not part of writing the document itself.
bool Schematic::writeDocument(const QString& path)
{
  QFile file(path);
  if(!file.open(QIODevice::WriteOnly))
    return false;

  QTextStream stream(&file);

  stream << "<Qucs Schematic " << PACKAGE_VERSION << ">\n";

  // Special case of saving a file when we want to save *only*
  // the symbol defintion (i.e. to create a "symbol file")
  if (a_DocName.endsWith(".sym")) {
      stream << "<Symbol>\n";
      for(auto* pp : a_SymbolPaints) {
          stream << "  <" << pp->save() << ">\n";
      }
      stream << "</Symbol>\n";
      file.close();
      return true;
  }

  stream << "<Properties>\n";
  if(a_symbolMode) {
    stream << "  <View=" << a_tmpViewX1<<","<<a_tmpViewY1<<","
      << a_tmpViewX2<<","<<a_tmpViewY2<< ",";
    stream <<a_tmpScale<<","<<a_tmpPosX<<","<<a_tmpPosY << ">\n";
  }
  else {
    stream << "  <View=" << a_ViewX1<<","<<a_ViewY1<<","
      << a_ViewX2<<","<<a_ViewY2<< ",";
    stream << a_Scale <<","<<contentsX()<<","<<contentsY() << ">\n";
  }
  stream << "  <Grid=" << a_GridX<<","<<a_GridY<<","
    << a_GridOn << ">\n";
  stream << "  <DataSet=" << a_DataSet << ">\n";
  stream << "  <DataDisplay=" << a_DataDisplay << ">\n";
  stream << "  <OpenDisplay=" << a_SimOpenDpl << ">\n";
  stream << "  <Script=" << a_Script << ">\n";
  stream << "  <RunScript=" << a_SimRunScript << ">\n";
  stream << "  <showFrame=" << static_cast<int>(a_showFrame) << ">\n";

  QString t;
  misc::convert2ASCII(t = a_Frame_Text0);
  stream << "  <FrameText0=" << t << ">\n";
  misc::convert2ASCII(t = a_Frame_Text1);
  stream << "  <FrameText1=" << t << ">\n";
  misc::convert2ASCII(t = a_Frame_Text2);
  stream << "  <FrameText2=" << t << ">\n";
  misc::convert2ASCII(t = a_Frame_Text3);
  stream << "  <FrameText3=" << t << ">\n";
  stream << "</Properties>\n";

  stream << "<Symbol>\n";     // save all paintings for symbol
  for(auto* pp : a_SymbolPaints)
    stream << "  <" << pp->save() << ">\n";
  stream << "</Symbol>\n";

  stream << "<Components>\n";    // save all components
  for(Component *pc : a_DocComps)
    stream << "  " << pc->save() << "\n";
  stream << "</Components>\n";

  stream << "<Wires>\n";    // save all wires
  for(Wire *pw : a_DocWires)
    stream << "  " << pw->save() << "\n";

  // save all labeled nodes as wires
  for(Node *pn : a_DocNodes)
    if(pn->hasLabel()) stream << "  " << pn->label()->save() << "\n";
  stream << "</Wires>\n";

  stream << "<Diagrams>\n";    // save all diagrams
  for(Diagram *pd : a_DocDiags)
    stream << "  " << pd->save() << "\n";
  stream << "</Diagrams>\n";

  stream << "<Paintings>\n";     // save all paintings
  for(auto* pp : a_DocPaints)
    stream << "  <" << pp->save() << ">\n";
  stream << "</Paintings>\n";

  file.close();
  return true;
}

int Schematic::saveDocument()
{
  if(!writeDocument(a_DocName)) {
    misc::reportError(QObject::tr("Cannot save document!"));
    return -1;
  }

  // additionally save symbol C++ code if in a symbol drawing and the
  // associated file is a Verilog-A file
  if (fileSuffix () == "sym") {
    if (fileSuffix (a_DataDisplay) == "va") {
      saveSymbolCpp ();
      saveSymbolJSON ();
      if (QucsSettings.DefaultSimulator == spicecompat::simNgspice) {
          savePropsJSON();
      } else if (QucsSettings.DefaultSimulator == spicecompat::simQucsator) {
          // TODO slit this into another method, or merge into saveSymbolJSON
          // handle errors in separate
          qDebug() << "  -> Run adms for symbol";

          QString vaFile;

    //      QDir prefix = QDir(QucsSettings.BinDir);

          QFileInfo inf(QucsSettings.Qucsator);
          QString QucsatorPath = inf.path()+QDir::separator();
          QDir include = QDir(QucsatorPath+"../include/qucs-core");

          //pick admsXml from settings
          QString admsXml = misc::canonicalDir(QucsSettings.AdmsXmlBinDir);

#if defined(_WIN32) || defined(__MINGW32__)
          admsXml = QDir::toNativeSeparators(admsXml+"/"+"admsXml.exe");
    #else
          admsXml = QDir::toNativeSeparators(admsXml+"/"+"admsXml");
    #endif

          QString workDir = QucsSettings.QucsWorkDir.absolutePath();

          qDebug() << "App path : " << qApp->applicationDirPath();
          qDebug() << "workdir"  << workDir;
          qDebug() << "workspacedir"  << QucsSettings.qucsWorkspaceDir.absolutePath();

          vaFile = QucsSettings.QucsWorkDir.filePath(fileBase()+".va");

          QStringList Arguments;
          Arguments << QDir::toNativeSeparators(vaFile)
                    << "-I" << QDir::toNativeSeparators(include.absolutePath())
                    << "-e" << QDir::toNativeSeparators(include.absoluteFilePath("qucsMODULEguiJSONsymbol.xml"))
                    << "-A" << "dyload";

    //      QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    //      env.insert("PATH", env.value("PATH") );

          QFile file(admsXml);
          if ( !file.exists() ){
            QMessageBox::critical(this, tr("Error"),
                                  tr("Program admsXml not found: %1\n\n"
                                      "Set the admsXml location on the application settings.").arg(admsXml));
            return -1;
          }

          qDebug() << "Command: " << admsXml << Arguments.join(" ");

          // need to cd into project to run admsXml?
          QDir::setCurrent(workDir);

          QProcess builder;
          builder.setProcessChannelMode(QProcess::MergedChannels);

          builder.start(admsXml, Arguments);


          // how to capture [warning]? need to modify admsXml?
          // TODO put stdout, stderr into a dock window, not messagebox
          if (!builder.waitForFinished()) {
            QString cmdString = QStringLiteral("%1 %2\n\n").arg(admsXml, Arguments.join(" "));
            cmdString = cmdString + builder.errorString();
            QMessageBox::critical(this, tr("Error"), cmdString);
          }
          else {
            QString cmdString = QStringLiteral("%1 %2\n\n").arg(admsXml, Arguments.join(" "));
            cmdString = cmdString + builder.readAll();
            QMessageBox::information(this, tr("Status"), cmdString);
          }
      }



      // _props.json and _sym.json in one object, _symbol.json - next to
      // this symbol, where the two were written. This is an auxiliary
      // export: the document itself is already written, so a problem here
      // is reported but does not fail the save.
      const QString base = QFileInfo(a_DocName).absolutePath() + QDir::separator() + fileBase();
      QFile f1(base + "_props.json");
      QFile f2(base + "_sym.json");
      QFile f3(base + "_symbol.json");
      QString why1, why2;
      const QJsonObject props = f1.open(QIODevice::ReadOnly) ? qucs_s::vamodule::parseJson(f1.readAll(), &why1) : QJsonObject();
      const QJsonObject symbol = f2.open(QIODevice::ReadOnly) ? qucs_s::vamodule::parseJson(f2.readAll(), &why2) : QJsonObject();
      if (props.isEmpty() || symbol.isEmpty()) {
        QMessageBox::warning(this, tr("Warning"),
                             tr("Cannot read the generated symbol JSON files.") + "\n" + why1 + "\n" + why2);
      } else if (!f3.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Warning"),
                             tr("Cannot write %1.").arg(f3.fileName()));
      } else {
        f3.write(QJsonDocument(qucs_s::vamodule::merged(props, symbol)).toJson(QJsonDocument::Indented));
      }

      // TODO choose icon, default to something or provided png

    } // if DataDisplay va
  } // if suffix .sym

  return 0;
}

// -------------------------------------------------------------
bool Schematic::loadProperties(QTextStream *stream)
{
  bool ok = true;
  QString Line, cstr, nstr;
  while(!stream->atEnd()) {
    Line = stream->readLine();
    if(Line.startsWith("</")) return true;  // field end ?
    Line = Line.trimmed();
    if(Line.isEmpty()) continue;

    if(Line.at(0) != '<') {
      misc::reportError(QObject::tr("Format Error:\nWrong property field limiter!"));
      return false;
    }
    if(Line.at(Line.length()-1) != '>') {
      misc::reportError(QObject::tr("Format Error:\nWrong property field limiter!"));
      return false;
    }
    Line = Line.mid(1, Line.length()-2);   // cut off start and end character

    cstr = Line.section('=',0,0);    // property type
    nstr = Line.section('=',1,1);    // property value
    if(cstr == "View") {
      a_ViewX1 = nstr.section(',',0,0).toInt(&ok); if(ok) {
      a_ViewY1 = nstr.section(',',1,1).toInt(&ok); if(ok) {
      a_ViewX2 = nstr.section(',',2,2).toInt(&ok); if(ok) {
      a_ViewY2 = nstr.section(',',3,3).toInt(&ok); if(ok) {
      a_Scale  = nstr.section(',',4,4).toDouble(&ok); if(ok) {
      a_tmpViewX1 = nstr.section(',',5,5).toInt(&ok); if(ok)
      a_tmpViewY1 = nstr.section(',',6,6).toInt(&ok); }}}}}
    }
    else if(cstr == "Grid") {
      setGridX(nstr.section(',',0,0).toInt(&ok)); if(ok) {
      setGridY(nstr.section(',',1,1).toInt(&ok)); if(ok) {
      if(nstr.section(',',2,2).toInt(&ok) == 0) a_GridOn = false;
      else a_GridOn = true; }}
    }
    else if(cstr == "DataSet") a_DataSet = nstr;
    else if(cstr == "DataDisplay") a_DataDisplay = nstr;
    else if(cstr == "OpenDisplay")
    if(nstr.toInt(&ok) == 0) a_SimOpenDpl = false;
    else a_SimOpenDpl = true;
    else if(cstr == "Script") a_Script = nstr;
    else if(cstr == "RunScript")
    if(nstr.toInt(&ok) == 0) a_SimRunScript = false;
    else a_SimRunScript = true;
    else if(cstr == "showFrame"){
        bool ok = false;
        int value = nstr.toInt(&ok);
        if (ok) {
            a_showFrame = static_cast<FrameSize>(value);
        } else {
            a_showFrame = FrameSize::None;  // Fallback
        }
    }else if(cstr == "FrameText0") misc::convert2Unicode(a_Frame_Text0 = nstr);
    else if(cstr == "FrameText1") misc::convert2Unicode(a_Frame_Text1 = nstr);
    else if(cstr == "FrameText2") misc::convert2Unicode(a_Frame_Text2 = nstr);
    else if(cstr == "FrameText3") misc::convert2Unicode(a_Frame_Text3 = nstr);
    else {
      misc::reportError(QObject::tr("Format Error:\nUnknown property: ")+cstr);
      return false;
    }
    if(!ok) {
      misc::reportError(QObject::tr("Format Error:\nNumber expected in property field!"));
      return false;
    }
  }

  misc::reportError(QObject::tr("Format Error:\n'Property' field is not closed!"));
  return false;
}

// ---------------------------------------------------
// Inserts a component without performing logic for wire optimization.
void Schematic::simpleInsertComponent(Component *c)
{
  c->setSchematic(this);   // see insertComponent()
  // connect every node of component
  for (Port *pp : c->Ports) {
    Node* pn = provideNode(c->cx + pp->x, c->cy + pp->y);

    pn->connect(c);  // connect schematic node to component node
    if (!pp->Type.isEmpty()) {
      pn->DType = pp->Type;
    }

    pp->Connection = pn;  // connect component node to schematic node
  }

  a_DocComps.push_back(c);
}

// -------------------------------------------------------------
bool Schematic::loadComponents(QTextStream *stream, std::list<Component*> *List)
{
  QString Line, cstr;
  Component *c;
  std::optional<IndexedInsertion> indexed;
  if (List == nullptr) indexed.emplace(this);
  while(!stream->atEnd()) {
    Line = stream->readLine();
    if(Line.startsWith("</")) return true;
    Line = Line.trimmed();
    if(Line.isEmpty()) continue;

    /// \todo enable user to load partial schematic, skip unknown components
    c = getComponentFromName(Line, this);
    if(!c) return false;

    if(List) {  // "paste" ?
      int z;
      for(z=c->Name.length()-1; z>=0; z--) // cut off number of component name
        if(!c->Name.at(z).isDigit()) break;
      c->Name = c->Name.left(z+1);
      List->push_back(c);
    }
    else  simpleInsertComponent(c);
  }

  misc::reportError(QObject::tr("Format Error:\n'Component' field is not closed!"));
  return false;
}

// -------------------------------------------------------------
// Inserts a wire without performing logic for optimizing.
void Schematic::simpleInsertWire(Wire *pw)
{
  Node* pn = provideNode(pw->P1());

  if(pw->P1() == pw->P2()) {
    pn->acquireLabel(pw->releaseLabel());   // wire with length zero are just node labels
    delete pw;           // delete wire because this is not a wire
    return;
  }
  pn->connect(pw);  // connect schematic node to component node
  pw->Port1 = pn;

  pn = provideNode(pw->P2());
  pn->connect(pw);  // connect schematic node to component node
  pw->Port2 = pn;

  a_DocWires.push_back(pw);
  if (a_insertionIndex != nullptr) {
    a_insertionIndex->add(pw);
  }
}

// -------------------------------------------------------------
bool Schematic::loadWires(QTextStream *stream, std::list<Element*> *List)
{
  Wire *w;
  QString Line;
  std::optional<IndexedInsertion> indexed;
  if (List == nullptr) indexed.emplace(this);
  while(!stream->atEnd()) {
    Line = stream->readLine();
    if(Line.startsWith("</")) return true;
    Line = Line.trimmed();
    if(Line.isEmpty()) continue;

    w = new Wire();
    if(!w->load(Line)) {
      misc::reportError(QObject::tr("Format Error:\nWrong 'wire' line format!"));
      delete w;
      return false;
    }

    // When this pointer is not null, "paste" operation is in progress.
    // In this case loaded elements must be placed in the list and not
    // into schematic.
    if(List) {

      // Special case: node label. It's stored as zero-length wire.
      // Only the label is kept, so it becomes "free" i.e. not having
      // a host element like wire or node. We must be careful to treat
      // such labels in a special way in other parts of the codebase.
      if (w->P1() == w->P2() && w->hasLabel()) {
        List->push_back(w->releaseLabel().release());
        delete w;
        continue;
      }

      List->push_back(w);

      // Label is also added to the list as *independent* element. This is
      // because items of this list a subject of moving, rotating, etc. and
      // label must be treated the same way.
      //
      // Think of the list as of "selected items" and it will instantly make
      // sense.
      //
      // Label ownership is still controlled by the host wire.
      if(w->hasLabel())  List->push_back(w->label());
    }
    else {
      simpleInsertWire(w);
    }
  }

  misc::reportError(QObject::tr("Format Error:\n'Wire' field is not closed!"));
  return false;
}

// -------------------------------------------------------------
bool Schematic::loadDiagrams(QTextStream *stream, std::list<Diagram*> *List)
{
  Diagram *d;
  QString Line, cstr;
  while(!stream->atEnd()) {
    Line = stream->readLine();
    if(Line.startsWith("</")) return true;
    Line = Line.trimmed();
    if(Line.isEmpty()) continue;

    cstr = Line.section(' ',0,0);    // diagram type
         if(cstr == "<Rect") d = new RectDiagram();
    else if(cstr == "<Polar") d = new PolarDiagram();
    else if(cstr == "<Tab") d = new TabDiagram();
    else if(cstr == "<Smith") d = new SmithDiagram();
    else if(cstr == "<ySmith") d = new SmithDiagram(0,0,false);
    else if(cstr == "<PS") d = new PSDiagram();
    else if(cstr == "<SP") d = new PSDiagram(0,0,false);
    else if(cstr == "<Rect3D") d = new Rect3DDiagram();
    else if(cstr == "<Curve") d = new CurveDiagram();
    else if(cstr == "<Time") d = new TimingDiagram();
    else if(cstr == "<Truth") d = new TruthDiagram();
    else if(cstr == "<Histogram") d = new HistogramDiagram();
    else {
      misc::reportError(QObject::tr("Format Error:\nUnknown diagram!"));
      return false;
    }

    if(!d->load(Line, stream)) {
      misc::reportError(QObject::tr("Format Error:\nWrong 'diagram' line format!"));
      delete d;
      return false;
    }
    List->push_back(d);
  }

  misc::reportError(QObject::tr("Format Error:\n'Diagram' field is not closed!"));
  return false;
}

// -------------------------------------------------------------
bool Schematic::loadPaintings(QTextStream *stream, std::list<Painting*> *List)
{
  Painting *p=0;
  QString Line, cstr;
  while(!stream->atEnd()) {
    Line = stream->readLine();
    if (Line.trimmed().isEmpty()) continue;

    if(Line.startsWith("</")) return true;

    Line = Line.trimmed();
    if(Line.isEmpty()) continue;
    if( (Line.at(0) != '<') || (Line.at(Line.length()-1) != '>')) {
      misc::reportError(QObject::tr("Format Error:\nWrong 'painting' line delimiter!"));
      return false;
    }
    Line = Line.mid(1, Line.length()-2);  // cut off start and end character

    cstr = Line.section(' ',0,0);    // painting type
         if(cstr == "Line") p = new GraphicLine();
    else if(cstr == "EArc") p = new EllipseArc();
    else if(cstr == ".PortSym") p = new PortSymbol();
    else if(cstr == ".ID") p = new ID_Text();
    else if(cstr == "Text") p = new GraphicText();
    else if(cstr == "Rectangle") p = new qucs::Rectangle();
    else if(cstr == "Arrow") p = new Arrow();
    else if(cstr == "Ellipse") p = new qucs::Ellipse();
    else if(cstr == "ImagePainting") p = new ImagePainting();
    else if(cstr == "Polyline") p = new PolylinePainting();
    else {
      misc::reportError(QObject::tr("Format Error:\nUnknown painting!"));
      return false;
    }

    if(!p->load(Line)) {
      misc::reportError(QObject::tr("Format Error:\nWrong 'painting' line format!"));
      delete p;
      return false;
    }
    List->push_back(p);
  }

  misc::reportError(QObject::tr("Format Error:\n'Painting' field is not closed!"));
  return false;
}

/*!
 * \brief Schematic::loadDocument tries to load a schematic document.
 * \return true/false in case of success/failure
 */
bool Schematic::loadDocument()
{
  QFile file(a_DocName);
  if(!file.open(QIODevice::ReadOnly)) {
    /// \todo implement unified error/warning handling GUI and CLI
    if (QucsMain != nullptr)
      misc::reportError(QObject::tr("Cannot load document: ")+a_DocName);
    else
      qCritical() << "Schematic::loadDocument:"
                  << QObject::tr("Cannot load document: ")+a_DocName;
    return false;
  }

  // Keep reference to source file (the schematic file)
  setFileInfo(a_DocName);

  QString Line;
  QTextStream stream(&file);

  // read header **************************
  do {
    if(stream.atEnd()) {
      file.close();
      return true;
    }

    Line = stream.readLine();
  } while(Line.isEmpty());

  if(Line.left(16) != "<Qucs Schematic ") {  // wrong file type ?
    file.close();
    misc::reportError(QObject::tr("Wrong document type: ")+a_DocName);
    return false;
  }

  Line = Line.mid(16, Line.length()-17);
  if(!misc::checkVersion(Line)) { // wrong version number ?
    if (QucsMain == nullptr) {
      // Command line and tests: nobody can answer a dialog, and a modal
      // box would block forever. Warn and try to open the file anyway.
      qWarning() << "Schematic::loadDocument:"
                 << QObject::tr("Wrong document version") << Line
                 << "in" << a_DocName << "- trying to open it anyway";
    } else {
      QMessageBox::StandardButton result;
      result = QMessageBox::warning(nullptr,
                                    QObject::tr("Warning"),
                                    QObject::tr("Wrong document version \n") +
                                                a_DocName + "\n" +
                                    QObject::tr("Try to open it anyway?"),
                                    QMessageBox::Yes|QMessageBox::No);

      if (result==QMessageBox::No) {
          file.close();
          return false;
      }
    }

    //misc::reportError(// QObject::tr("Wrong document version: ")+Line);
  }

  // read content *************************
  while(!stream.atEnd()) {
    Line = stream.readLine();
    Line = Line.trimmed();
    if(Line.isEmpty()) continue;

    if(Line == "<Symbol>") {
      if (!loadPaintings(&stream, &a_SymbolPaints)) {
        file.close();
        return false;
      }
    }
    else
    if(Line == "<Properties>") {
      if(!loadProperties(&stream)) { file.close(); return false; } }
    else
    if(Line == "<Components>") {
      if(!loadComponents(&stream)) { file.close(); return false; } }
    else
    if(Line == "<Wires>") {
      if(!loadWires(&stream)) { file.close(); return false; } }
    else
    if(Line == "<Diagrams>") {
      if (!loadDiagrams(&stream, &a_DocDiags)) { file.close(); return false; }
    }
    else
    if(Line == "<Paintings>") {
      if (!loadPaintings(&stream, &a_DocPaints)) { file.close(); return false; }
    }
    else {
       qDebug() << Line;
       misc::reportError(QObject::tr("File Format Error:\nUnknown field!"));
      file.close();
      return false;
    }
  }

  file.close();
  return true;
}

// -------------------------------------------------------------
// Creates a Qucs file format (without document properties) in the returning
// string. This is used to save state for undo operation.
QString Schematic::createUndoString(char Op)
{
  // Build element document.
  QString s = "  \n";
  s.replace(0,1,Op);
  for(auto* pc : a_DocComps)
    s += pc->save()+"\n";
  s += "</>\n";  // short end flag

  for(auto* pw : a_DocWires)
    s += pw->save()+"\n";
  // save all labeled nodes as wires
  for(Node *pn : a_DocNodes)
    if(pn->hasLabel()) s += pn->label()->save()+"\n";
  s += "</>\n";

  for(auto* pd : a_DocDiags)
    s += pd->save()+"\n";
  s += "</>\n";

  for(auto* pp : a_DocPaints)
    s += "<"+pp->save()+">\n";
  s += "</>\n";

  return s;
}

// -------------------------------------------------------------
// Same as "createUndoString(char Op)" but for symbol edit mode.
QString Schematic::createSymbolUndoString(char Op)
{

  // Build element document.
  QString s = "  \n";
  s.replace(0,1,Op);
  s += "</>\n";  // short end flag for components
  s += "</>\n";  // short end flag for wires
  s += "</>\n";  // short end flag for diagrams

  for(auto* pp : a_SymbolPaints)
    s += "<"+pp->save()+">\n";
  s += "</>\n";

  return s;
}

// -------------------------------------------------------------
// Is quite similar to "loadDocument()" but with less error checking.
// Used for "undo" function.
bool Schematic::rebuild(QString *s)
{
  deleteAllElements();	// delete whole document

  QString Line;
  QTextStream stream(s, QIODevice::ReadOnly);
  Line = stream.readLine();  // skip identity byte

  // read content *************************
  const bool ok = loadComponents(&stream)
               && loadWires(&stream)
               && loadDiagrams(&stream, &a_DocDiags)
               && loadPaintings(&stream, &a_DocPaints);

  // Whoever cached pointers into the old document must let go of them now.
  emit signalDocumentRebuilt(this);
  return ok;
}

// -------------------------------------------------------------
// Same as "rebuild(QString *s)" but for symbol edit mode.
bool Schematic::rebuildSymbol(QString *s)
{
  deleteSymbolPaintings();	// delete whole document

  QString Line;
  QTextStream stream(s, QIODevice::ReadOnly);
  Line = stream.readLine();  // skip identity byte

  // read content *************************
  Line = stream.readLine();  // skip components
  Line = stream.readLine();  // skip wires
  Line = stream.readLine();  // skip diagrams

  const bool ok = loadPaintings(&stream, &a_SymbolPaints);

  emit signalDocumentRebuilt(this);
  return ok;
}


// ***************************************************************
// *****                                                     *****
// *****             Functions to create netlist             *****
// *****                                                     *****
// ***************************************************************

void Schematic::createNodeSet(QStringList& Collect, int& countInit,
          Conductor *pw, Node *p1)
{
  if(pw->hasLabel())
    if(!pw->label()->initValue.isEmpty())
      Collect.append("NodeSet:NS" + QString::number(countInit++) + " " +
                     p1->Name + " U=\"" + pw->label()->initValue + "\"");
}

// ---------------------------------------------------
void Schematic::throughAllNodes(bool User, QStringList& Collect,
        int& countInit)
{
  int z=0;

  for(Node* pn : a_DocNodes) {
    if(pn->Name.isEmpty() == User) {
      continue;  // already named ?
    }
    if(!User) {
      if(a_isAnalog)
        pn->Name = "_net";
      else
        pn->Name = "net_net";   // VHDL names must not begin with '_'
      pn->Name += QString::number(z++);  // create numbered node name
    }
    else if(pn->State) {
      continue;  // already worked on
    }

    if(a_isAnalog) createNodeSet(Collect, countInit, pn, pn);

    pn->State = 1;
    propagateNode(Collect, countInit, pn);
  }
}

// ---------------------------------------------------
// A subcircuit's pin is netlisted as the net its port sits on, so a net
// with no label of its own would reach the .SUBCKT line as "_net7". Give
// it the name of the port instead - the name the symbol writes beside
// that pin - unless something else on the schematic already answers to
// it, which would join two nets that are not connected.
//
// Runs after the labelled nets have been propagated, so a port whose node
// is still unnamed is on a net without a label anywhere.
void Schematic::nameUnlabelledPortNets(QStringList& Collect, int& countInit)
{
  // What a netlist may call a node. A component name is normally this
  // already; one that is not keeps the generated name.
  static const QRegularExpression plainName("^[A-Za-z_][A-Za-z0-9_]*$");

  QSet<QString> taken;
  for (Node* pn : a_DocNodes)
    if (!pn->Name.isEmpty()) taken.insert(pn->Name);

  for (Component* pc : a_DocComps) {
    if (pc->Model != "Port") continue;
    if (pc->isActive != COMP_IS_ACTIVE) continue;
    if (pc->Ports.isEmpty()) continue;

    Node* pn = pc->Ports.first()->Connection;
    if (pn == nullptr || !pn->Name.isEmpty()) continue;

    if (!plainName.match(pc->Name).hasMatch()) continue;
    // VHDL names must not begin with '_', as elsewhere in the netlister
    const QString name = a_isAnalog ? pc->Name : "net" + pc->Name;
    if (taken.contains(name)) continue;

    pn->Name = name;
    taken.insert(name);
    if (a_isAnalog) createNodeSet(Collect, countInit, pn, pn);
    pn->State = 1;
    propagateNode(Collect, countInit, pn);
  }
}

// ----------------------------------------------------------
// Checks whether this file is a qucs file and whether it is an subcircuit.
// It returns the number of subcircuit ports.
int Schematic::testFile(const QString& DocName)
{
  QFile file(DocName);
  if(!file.open(QIODevice::ReadOnly)) {
    return -1;
  }

  QString Line;
  // .........................................
  // To strongly speed up the file read operation the whole file is
  // read into the memory in one piece.
  QTextStream ReadWhole(&file);
  QString FileString = ReadWhole.readAll();
  file.close();
  QTextStream stream(&FileString, QIODevice::ReadOnly);


  // read header ........................
  do {
    if(stream.atEnd()) {
      file.close();
      return -2;
    }
    Line = stream.readLine();
    Line = Line.trimmed();
  } while(Line.isEmpty());

  if(Line.left(16) != "<Qucs Schematic ") {  // wrong file type ?
    file.close();
    return -3;
  }

  Line = Line.mid(16, Line.length()-17);
  if(!misc::checkVersion(Line)) { // wrong version number ?
      if (!QucsSettings.IgnoreFutureVersion) {
          file.close();
          return -4;
      }
    //file.close();
    //return -4;
  }

  // read content ....................
  while(!stream.atEnd()) {
    Line = stream.readLine();
    if(Line == "<Components>") break;
  }

  int z=0;
  while(!stream.atEnd()) {
    Line = stream.readLine();
    if(Line == "</Components>") {
      file.close();
      return z;       // return number of ports
    }

    Line = Line.trimmed();
    QString s = Line.section(' ',0,0);    // component type
    if(s == "<Port") z++;
  }
  return -5;  // component field not closed
}

// ---------------------------------------------------
// Collects the signal names for digital simulations.
void Schematic::collectDigitalSignals(void)
{
  for(auto* pn : a_DocNodes) {
    DigMap::Iterator it = a_Signals.find(pn->Name);
    if(it == a_Signals.end()) { // avoid redeclaration of signal
      a_Signals.insert(pn->Name, DigSignal(pn->Name, pn->DType));
    } else if (!pn->DType.isEmpty()) {
      it.value().Type = pn->DType;
    }
  }
}

// ---------------------------------------------------
// Propagates the given node to connected component ports.
void Schematic::propagateNode(QStringList& Collect,
              int& countInit, Node* start_node)
{
  bool setName=false;
  std::list<Node*> Cons;

  Cons.push_back(start_node);
  for(auto it = Cons.begin(); it != Cons.end(); it++) {
    auto* node = *it;

    for (auto* wire : node->wires()) {
      if (node != wire->Port1) {
        if (wire->Port1->Name.isEmpty()) {
          wire->Port1->Name = start_node->Name;
          wire->Port1->State = 1;
          Cons.push_back(wire->Port1);
          setName = true;
        }
      }
      else {
        if (wire->Port2->Name.isEmpty()) {
          wire->Port2->Name = start_node->Name;
          wire->Port2->State = 1;
          Cons.push_back(wire->Port2);
          setName = true;
        }
      }

      if (setName) {
        if (a_isAnalog) createNodeSet(Collect, countInit, wire, start_node);
          setName = false;
      }

    }
  }
  Cons.clear();
}

#include <iostream>

/*!
 * \brief Schematic::throughAllComps
 * Goes through all schematic components and allows special component
 * handling, e.g. like subcircuit netlisting.
 * \param stream is a pointer to the text stream used to collect the netlist
 * \param countInit is the reference to a counter for nodesets (initial conditions)
 * \param Collect is the reference to a list of collected nodesets
 * \param ErrText is pointer to the QPlainTextEdit used for error messages
 * \param NumPorts counter for the number of ports
 * \return true in case of success (false otherwise)
 */
bool Schematic::throughAllComps(QTextStream *stream, int& countInit,
                   QStringList& Collect, QPlainTextEdit *ErrText, int NumPorts)
{
  bool r;
  QString s;

  // give the ground nodes the name "gnd", and insert subcircuits etc.
  for (auto* pc : a_DocComps) {

    if(pc->isActive != COMP_IS_ACTIVE) continue;

    if(pc->Model == "CMD") continue; // Skip "system command" component. It must not go to the simulation backend

    // check analog/digital typed components
    if(a_isAnalog) {
      if((pc->Type & isAnalogComponent) == 0) {
        ErrText->appendPlainText(QObject::tr("ERROR: Component \"%1\" has no analog model.").arg(pc->Name));
        return false;
      }
    } else {
      if((pc->Type & isDigitalComponent) == 0) {
        ErrText->appendPlainText(QObject::tr("ERROR: Component \"%1\" has no digital model.").arg(pc->Name));
        return false;
      }
    }

    // handle ground symbol
    if(pc->Model == "GND") {
      pc->Ports.first()->Connection->Name = "gnd";
      continue;
    }

    // handle subcircuits
    if(pc->Model == "Sub")
    {
      int i;
      // tell the subcircuit it belongs to this schematic
      pc->setSchematic (this);
      QString f = pc->getSubcircuitFile();
      SubMap::Iterator it = FileList.find(f);
      if(it != FileList.end())
      {
        if (!it.value().PortTypes.isEmpty())
        {
          i = 0;
          // apply in/out signal types of subcircuit
          for (Port *pp : pc->Ports)
          {
            pp->Type = it.value().PortTypes[i];
            pp->Connection->DType = pp->Type;
            i++;
          }
        }
        continue;   // insert each subcircuit just one time
      }

      // The subcircuit has not previously been added
      SubFile sub = SubFile("SCH", f);
      FileList.insert(f, sub);


      // load subcircuit schematic
      s = pc->Props.first()->Value;
      Schematic *d = new Schematic(0, pc->getSubcircuitFile());
      if(!d->loadDocument())      // load document if possible
      {
          delete d;
          /// \todo implement error/warning message dispatcher for GUI and CLI modes.
          QString message = QObject::tr("ERROR: Cannot load subcircuit \"%1\".").arg(s);
          if (QucsMain != nullptr) // GUI is running
            ErrText->appendPlainText(message);
          else // command line
            qCritical() << "Schematic::throughAllComps" << message;
          return false;
      }
      d->a_DocName = s;
      d->a_isVerilog = a_isVerilog;
      d->a_isAnalog = a_isAnalog;
      d->a_creatingLib = a_creatingLib;
      r = d->createSubNetlist(stream, countInit, Collect, ErrText, NumPorts);
      if (r)
      {
        i = 0;
        // save in/out signal types of subcircuit
        for (Port *pp : pc->Ports)
        {
            //if(i>=d->a_PortTypes.count())break;
            pp->Type = d->a_PortTypes[i];
            pp->Connection->DType = pp->Type;
            i++;
        }
        sub.PortTypes = d->a_PortTypes;
        FileList.insert(f,sub);
        //FileList.replace(f, sub);
      }
      delete d;
      if(!r)
      {
        return false;
      }
      continue;
    } // if(pc->Model == "Sub")

    if(LibComp* lib = dynamic_cast</*const*/LibComp*>(pc)) {
      if(a_creatingLib) {
        ErrText->appendPlainText(
        QObject::tr("WARNING: Skipping library component \"%1\".").
        arg(pc->Name));
        continue;
      }
      QString scfile = pc->getSubcircuitFile();
      s = scfile + "/" + pc->Props.at(1)->Value;
      SubMap::Iterator it = FileList.find(s);
      if(it != FileList.end())
        continue;   // insert each library subcircuit just one time
      FileList.insert(s, SubFile("LIB", s));

      unsigned whatisit = a_isAnalog?1:(a_isVerilog?4:2);
      if(a_isAnalog) {
        if (QucsSettings.DefaultSimulator!=spicecompat::simQucsator) {
            if (QucsSettings.DefaultSimulator==spicecompat::simXyce)
                whatisit = 16;
            else whatisit = 8;
        } else whatisit = 1;
      }
      r = lib->createSubNetlist(stream, Collect, whatisit);

      if(!r) {
        ErrText->appendPlainText(
        QObject::tr("ERROR: \"%1\": Cannot load library component \"%2\" from \"%3\"").
        arg(pc->Name, pc->Props.at(1)->Value, scfile));
        return false;
      }
      continue;
    }

    // handle SPICE subcircuit components
    if(pc->Model == "SPICE") {
      s = pc->Props.first()->Value;
      // tell the spice component it belongs to this schematic
      pc->setSchematic (this);
      if(s.isEmpty()) {
        ErrText->appendPlainText(QObject::tr("ERROR: No file name in SPICE component \"%1\".").
                        arg(pc->Name));
        return false;
      }
      QString f = pc->getSubcircuitFile();
      SubMap::Iterator it = FileList.find(f);
      if(it != FileList.end())
        continue;   // insert each spice component just one time
      FileList.insert(f, SubFile("CIR", f));

      SpiceFile *sf = (SpiceFile*)pc;
      if (QucsSettings.DefaultSimulator != spicecompat::simQucsator)
          r = sf->createSpiceSubckt(stream);
      else r = sf->createSubNetlist(stream);
      ErrText->appendPlainText(sf->getErrorText());
      if(!r){
        return false;
      }
      continue;
    }

    if (pc->Model == "SPfile" &&
        QucsSettings.DefaultSimulator == spicecompat::simNgspice) {
        QString f = pc->getSubcircuitFile();
        QString sub_name = "Sub_" + pc->Model + "_" + pc->Name;
        S2Spice *conv = new S2Spice();
        conv->setFile(f);
        conv->setDeviceName(sub_name);
        bool r = conv->convertTouchstone(stream);
        QString msg = conv->getErrText();
        if (!r) {
            QMessageBox::warning(this,tr("Netlist error"), msg);
            return false;
        } else if (!msg.isEmpty()) {
            QMessageBox::warning(this,tr("S2Spice warning"), msg);
        }
        delete conv;
    }

    // handle digital file subcircuits
    if(pc->Model == "VHDL" || pc->Model == "Verilog") {
      if(a_isVerilog && pc->Model == "VHDL")
        continue;
      if(!a_isVerilog && pc->Model == "Verilog")
        continue;
      s = pc->Props.front()->Value;
      if(s.isEmpty()) {
        ErrText->appendPlainText(QObject::tr("ERROR: No file name in %1 component \"%2\".").
          arg(pc->Model).
          arg(pc->Name));
        return false;
      }
      QString f = pc->getSubcircuitFile();
      SubMap::Iterator it = FileList.find(f);
      if(it != FileList.end())
        continue;   // insert each vhdl/verilog component just one time
      s = ((pc->Model == "VHDL") ? "VHD" : "VER");
      FileList.insert(f, SubFile(s, f));

      if(pc->Model == "VHDL") {
        VHDL_File *vf = (VHDL_File*)pc;
        r = vf->createSubNetlist(stream);
        ErrText->appendPlainText(vf->getErrorText());
        if(!r) {
          return false;
        }
      }
      if(pc->Model == "Verilog") {
        Verilog_File *vf = (Verilog_File*)pc;
        r = vf->createSubNetlist(stream);
        ErrText->appendPlainText(vf->getErrorText());
        if(!r) {
          return false;
        }
      }
      continue;
    }
  }
  return true;
}

// ---------------------------------------------------
// Follows the wire lines in order to determine the node names for
// each component. Output into "stream", NodeSets are collected in
// "Collect" and counted with "countInit".
bool Schematic::giveNodeNames(QTextStream *stream, int& countInit,
                   QStringList& Collect, QPlainTextEdit *ErrText, int NumPorts)
{
  // delete the node names
  for(Node *pn : a_DocNodes) {
    pn->State = 0;
    if(pn->hasLabel()) {
      if(a_isAnalog)
        pn->Name = pn->label()->Name;
      else
        pn->Name = "net" + pn->label()->Name;
    }
    else pn->Name = "";
  }

  // set the wire names to the connected node
  for(Wire *pw : a_DocWires)
    if(pw->hasLabel()) {
      if(a_isAnalog)
        pw->Port1->Name = pw->label()->Name;
      else  // avoid to use reserved VHDL words
        pw->Port1->Name = "net" + pw->label()->Name;
    }

  // go through components
  if(!throughAllComps(stream, countInit, Collect, ErrText, NumPorts)){
    fprintf(stderr, "Error: Could not go throughAllComps\n");
    return false;
  }

  // work on named nodes first in order to preserve the user given names
  throughAllNodes(true, Collect, countInit);

  // a subcircuit port on a net that carries no label names that net, so
  // the pin of the .SUBCKT is called what the symbol shows beside it
  nameUnlabelledPortNets(Collect, countInit);

  // give names to the remaining (unnamed) nodes
  throughAllNodes(false, Collect, countInit);

  if(!a_isAnalog) // collect all node names for VHDL signal declaration
    collectDigitalSignals();

  return true;
}

// ---------------------------------------------------
bool Schematic::createLibNetlist(QTextStream *stream, QPlainTextEdit *ErrText,
          int NumPorts)
{
  int countInit = 0;
  QStringList Collect;
  Collect.clear();
  FileList.clear();
  a_Signals.clear();
  // Apply node names and collect subcircuits and file include
  a_creatingLib = true;
  if(!giveNodeNames(stream, countInit, Collect, ErrText, NumPorts)) {
    a_creatingLib = false;
    return false;
  }
  a_creatingLib = false;

  // Marking start of actual top-level subcircuit
  QString c;
  if(!a_isAnalog) {
    if (a_isVerilog)
      c = "///";
    else
      c = "---";
  }
  else c = "###";
  (*stream) << "\n" << c << " TOP LEVEL MARK " << c << "\n";

  // Emit subcircuit components
  createSubNetlistPlain(stream, ErrText, NumPorts);

  a_Signals.clear();  // was filled in "giveNodeNames()"
  return true;
}

//#define VHDL_SIGNAL_TYPE "bit"
//#define VHDL_LIBRARIES   ""
#define VHDL_SIGNAL_TYPE "std_logic"
#define VHDL_LIBRARIES   "\nlibrary ieee;\nuse ieee.std_logic_1164.all;\n"

// ---------------------------------------------------
void Schematic::createSubNetlistPlain(QTextStream *stream, QPlainTextEdit *ErrText,
                                      int NumPorts)
{
  int i, z;
  QString s;
  QStringList SubcircuitPortNames;
  QStringList SubcircuitPortTypes;
  QStringList InPorts;
  QStringList OutPorts;
  QStringList InOutPorts;
  QStringList::iterator it_name;
  QStringList::iterator it_type;

  // probably creating a library currently
  QTextStream * tstream = stream;
  QFile ofile;
  if(a_creatingLib) {
    QString f = misc::properAbsFileName(a_DocName) + ".lst";
    ofile.setFileName(f);
    if(!ofile.open(QIODevice::WriteOnly)) {
      ErrText->appendPlainText(tr("ERROR: Cannot create library file \"%s\".").arg(f));
      return;
    }
    tstream = new QTextStream(&ofile);
  }

  // collect subcircuit ports and sort their node names into
  // "SubcircuitPortNames"
  a_PortTypes.clear();
  for(auto* pc : a_DocComps) {
    if(pc->Model.at(0) == '.') { // no simulations in subcircuits
      ErrText->appendPlainText(
        QObject::tr("WARNING: Ignore simulation component in subcircuit \"%1\".").arg(a_DocName)+"\n");
      continue;
    }
    else if(pc->Model == "Port") {
      i = pc->Props.first()->Value.toInt();
      for(z=SubcircuitPortNames.size(); z<i; z++) { // add empty port names
        SubcircuitPortNames.append(" ");
        SubcircuitPortTypes.append(" ");
      }
      it_name = SubcircuitPortNames.begin();
      it_type = SubcircuitPortTypes.begin();
      for(int n=1;n<i;n++)
      {
        it_name++;
        it_type++;
      }
      (*it_name) = pc->Ports.first()->Connection->Name;
      DigMap::Iterator it = a_Signals.find(*it_name);
      if(it!=a_Signals.end())
        (*it_type) = it.value().Type;
      // propagate type to port symbol
      pc->Ports.first()->Connection->DType = *it_type;

      if(!a_isAnalog) {
        if (a_isVerilog) {
          a_Signals.remove(*it_name); // remove node name
          switch(pc->Props.at(1)->Value.isEmpty() ? '\0' : pc->Props.at(1)->Value.at(0).toLatin1()) {
            case 'a':
              InOutPorts.append(*it_name);
              break;
            case 'o':
              OutPorts.append(*it_name);
              break;
              default:
                InPorts.append(*it_name);
          }
        }
        else {
          // remove node name of output port
          a_Signals.remove(*it_name);
          switch(pc->Props.at(1)->Value.isEmpty() ? '\0' : pc->Props.at(1)->Value.at(0).toLatin1()) {
            case 'a':
              (*it_name) += " : inout"; // attribute "analog" is "inout"
              break;
            case 'o': // output ports need workaround
              a_Signals.insert(*it_name, DigSignal(*it_name, *it_type));
              (*it_name) = "net_out" + (*it_name);
              (*it_name) += " : " + pc->Props.at(1)->Value;
              break;
            default:
              (*it_name) += " : " + pc->Props.at(1)->Value;
          }
          (*it_name) += " " + ((*it_type).isEmpty() ?
          VHDL_SIGNAL_TYPE : (*it_type));
        }
      }
    }
  }

  // remove empty subcircuit ports (missing port numbers)
  for(it_name = SubcircuitPortNames.begin(),
      it_type = SubcircuitPortTypes.begin();
      it_name != SubcircuitPortNames.end(); ) {
    if(*it_name == " ") {
      it_name = SubcircuitPortNames.erase(it_name);
      it_type = SubcircuitPortTypes.erase(it_type);
    } else {
      a_PortTypes.append(*it_type);
      it_name++;
      it_type++;
    }
  }

  QString f = misc::properFileName(a_DocName);
  QString Type = misc::properName(f);




  if (QucsSettings.DefaultSimulator == spicecompat::simQucsator ||
      !a_isAnalog) {

        if(a_isAnalog) {
            // ..... analog subcircuit ...................................
            (*tstream) << "\n.Def:" << Type << " " << SubcircuitPortNames.join(" ");
            for (auto* pi : a_SymbolPaints)
              if(pi->Name == ".ID ") {
                ID_Text *pid = (ID_Text*)pi;
                for (const auto& sub_param : pid->subParameters) {
                  s = sub_param->name; // keep 'Name' unchanged
                  (*tstream) << " " << s.replace("=", "=\"") << '"';
                }
                break;
              }
            (*tstream) << '\n';

            // write all components with node names into netlist file
            for (auto* pc : a_DocComps)
              (*tstream) << pc->getNetlist();

            (*tstream) << ".Def:End\n";

          }
          else {
            if (a_isVerilog) {
              // ..... digital subcircuit ...................................
              (*tstream) << "\nmodule Sub_" << Type << " ("
                      << SubcircuitPortNames.join(", ") << ");\n";

              // subcircuit in/out connections
              if(!InPorts.isEmpty())
                (*tstream) << " input " << InPorts.join(", ") << ";\n";
              if(!OutPorts.isEmpty())
                (*tstream) << " output " << OutPorts.join(", ") << ";\n";
              if(!InOutPorts.isEmpty())
                (*tstream) << " inout " << InOutPorts.join(", ") << ";\n";

              // subcircuit connections
              if(!a_Signals.isEmpty()) {
                QList<DigSignal> values = a_Signals.values();
                QList<DigSignal>::const_iterator it;
                for (it = values.constBegin(); it != values.constEnd(); ++it) {
                  (*tstream) << " wire " << (*it).Name << ";\n";
                }
              }
              (*tstream) << "\n";

              // subcircuit parameters
              for (auto* pi : a_SymbolPaints)
                if(pi->Name == ".ID ") {
                  ID_Text *pid = (ID_Text*)pi;
                  for (const auto& sub_param : pid->subParameters) {
                    s = sub_param->name.section('=', 0,0);
                    QString v = misc::Verilog_Param(sub_param->name.section('=', 1,1));
                    (*tstream) << " parameter " << s << " = " << v << ";\n";
                  }
                  (*tstream) << "\n";
                  break;
                }

              // write all equations into netlist file
              for (auto* pc : a_DocComps) {
                if(pc->Model == "Eqn") {
                  (*tstream) << pc->get_Verilog_Code(NumPorts);
                }
              }

              if(a_Signals.find("gnd") != a_Signals.end())
              (*tstream) << " assign gnd = 0;\n"; // should appear only once

              // write all components into netlist file
              for (auto* pc : a_DocComps) {
                if(pc->Model != "Eqn") {
                  s = pc->get_Verilog_Code(NumPorts);
                  if(s.length()>0 && s.at(0) == '\xA7') {  //section symbol
                    ErrText->insertPlainText(s.mid(1));
                  }
                  else (*tstream) << s;
                }
              }

              (*tstream) << "endmodule\n";
            } else {
              // ..... digital subcircuit ...................................
              (*tstream) << VHDL_LIBRARIES;
              (*tstream) << "entity Sub_" << Type << " is\n";

              QString generic_str;
              for (auto* pi : a_SymbolPaints) {
                if(pi->Name == ".ID ") {
                  ID_Text *pid = (ID_Text*)pi;



                  for (const auto& sub_param : pid->subParameters) {
                    s = sub_param->name;
                    QString t = sub_param->type.isEmpty() ? "real" : sub_param->type;
                    generic_str += s.replace("=", " : "+t+" := ") + ";\n ";
                  }


                  break;
                }
              }
              if (!generic_str.isEmpty()) {
                (*tstream) << " generic (";
                (*tstream) << generic_str;
                (*tstream) << ");\n";
              }

              (*tstream) << " port ("
                        << SubcircuitPortNames.join(";\n ") << ");\n";


              (*tstream) << "end entity;\n"
                          << "use work.all;\n"
                          << "architecture Arch_Sub_" << Type << " of Sub_" << Type
                          << " is\n";

              if(!a_Signals.isEmpty()) {
                QList<DigSignal> values = a_Signals.values();
                QList<DigSignal>::const_iterator it;
                for (it = values.constBegin(); it != values.constEnd(); ++it) {
                  (*tstream) << " signal " << (*it).Name << " : "
                  << ((*it).Type.isEmpty() ?
                  VHDL_SIGNAL_TYPE : (*it).Type) << ";\n";
                }
              }

              // write all equations into netlist file
              for (auto* pc : a_DocComps) {
                if(pc->Model == "Eqn") {
                  ErrText->insertPlainText(
                              QObject::tr("WARNING: Equations in \"%1\" are 'time' typed.").
                  arg(pc->Name));
                  (*tstream) << pc->get_VHDL_Code(NumPorts);
                }
              }

              (*tstream) << "begin\n";

              if(a_Signals.find("gnd") != a_Signals.end())
              (*tstream) << " gnd <= '0';\n"; // should appear only once

              // write all components into netlist file
              for (auto* pc : a_DocComps) {
                if(pc->Model != "Eqn") {
                    s = pc->get_VHDL_Code(NumPorts);
                    if(s.length()>0 && s.at(0) == '\xA7') {  //section symbol
                      ErrText->insertPlainText(s.mid(1));
                  }
                  else (*tstream) << s;
                }
              }

              (*tstream) << "end architecture;\n";
            }
          }

    }


  // close file
  if(a_creatingLib) {
    ofile.close();
    delete tstream;
  }
}
// ---------------------------------------------------
// Write the netlist as subcircuit to the text stream 'stream'.
bool Schematic::createSubNetlist(QTextStream *stream, int& countInit,
                     QStringList& Collect, QPlainTextEdit *ErrText, int NumPorts)
{
//  int Collect_count = Collect.count();   // position for this subcircuit

  // TODO: NodeSets have to be put into the subcircuit block.
  if(!giveNodeNames(stream, countInit, Collect, ErrText, NumPorts)){
    fprintf(stderr, "Error giving NodeNames in createSubNetlist\n");
    return false;
  }

/*  Example for TODO
      for(it = Collect.at(Collect_count); it != Collect.end(); )
      if((*it).left(4) == "use ") {  // output all subcircuit uses
        (*stream) << (*it);
        it = Collect.remove(it);
      }
      else it++;*/

  // Emit subcircuit components
   createSubNetlistPlain(stream, ErrText, NumPorts);
   if (QucsSettings.DefaultSimulator != spicecompat::simQucsator &&
       a_isAnalog) {
      AbstractSpiceKernel *kern = new AbstractSpiceKernel(this);
      QStringList err_lst;
      if (!kern->checkSchematic(err_lst)) {
          QString s = QStringLiteral("Subcircuit %1 contains SPICE-incompatible components.\n"
                              "Check these components: %2 \n")
                  .arg(this->a_DocName).arg(err_lst.join("; "));
          ErrText->insertPlainText(s);
          return false;
      }
      kern->createSubNetlist(*stream);

      delete kern;
  }


  a_Signals.clear();  // was filled in "giveNodeNames()"
  return true;
}

// ---------------------------------------------------
// Detect simulation domain (analog/digital) by looking at component types.
bool Schematic::isDigitalCircuit()
{
  for (Component *pc : a_DocComps) {
      if(pc->isActive == COMP_IS_OPEN) continue;
      if(pc->Model.at(0) == '.' && pc->Model == ".Digi") {
          return true;  // Verilog simulation detected
      }
  }
  return false;  // Verilog simulation not found
}

// ---------------------------------------------------
// Determines the node names and writes subcircuits into netlist file.
int Schematic::prepareNetlist(QTextStream& stream, QStringList& Collect,
                              QPlainTextEdit *ErrText)
{
  if(a_showBias > 0) a_showBias = -1;  // do not show DC bias anymore

  a_isVerilog = false;
  a_isAnalog = true;
  bool isTruthTable = false;
  int allTypes = 0, NumPorts = 0;

  // Detect simulation domain (analog/digital) by looking at component types.
  for (Component *pc : a_DocComps) {
    if(pc->isActive == COMP_IS_OPEN) continue;
    if(pc->Model.at(0) == '.') {
      if(pc->Model == ".Digi") {
        if(allTypes & isDigitalComponent) {
          ErrText->appendPlainText(
             QObject::tr("ERROR: Only one digital simulation allowed."));
          return -10;
        }
        if(pc->Props.front()->Value != "TimeList")
          isTruthTable = true;
        if(pc->Props.back()->Value != "VHDL")
          a_isVerilog = true;
        allTypes |= isDigitalComponent;
        a_isAnalog = false;
      }
      else allTypes |= isAnalogComponent;
      if((allTypes & isComponent) == isComponent) {
        ErrText->appendPlainText(
           QObject::tr("ERROR: Analog and digital simulations cannot be mixed."));
        return -10;
      }
    }
    else if(pc->Model == "DigiSource") NumPorts++;
  }

  if((allTypes & isAnalogComponent) == 0) {
    if(allTypes == 0) {
      // If no simulation exists, assume analog simulation. There may
      // be a simulation within a SPICE file. Otherwise Qucsator will
      // output an error.
      a_isAnalog = true;
      allTypes |= isAnalogComponent;
      NumPorts = -1;
    }
    else {
      if(NumPorts < 1 && isTruthTable) {
        ErrText->appendPlainText(
           QObject::tr("ERROR: Digital simulation needs at least one digital source."));
        return -10;
      }
      if(!isTruthTable) NumPorts = 0;
    }
  }
  else {
    NumPorts = -1;
    a_isAnalog = true;
  }

  // first line is documentation
  bool has_header = true;
  if(allTypes & isAnalogComponent) {
    if (QucsSettings.DefaultSimulator != spicecompat::simQucsator) {
      has_header = false;
    } else {
      stream << '#';
    }
  } else if (a_isVerilog) {
    stream << "//";
  } else {
    stream << "--";
  }
  if (has_header) {
    stream << " Qucs " << PACKAGE_VERSION << "  " << a_DocName << "\n";
  }

  // set timescale property for verilog schematics
  if (a_isVerilog) {
    stream << "\n`timescale 1ps/100fs\n";
  }

  int countInit = 0;  // counts the nodesets to give them unique names

  if(!giveNodeNames(&stream, countInit, Collect, ErrText, NumPorts)){
    fprintf(stderr, "Error giving NodeNames\n");
    return -10;
  }

  if(allTypes & isAnalogComponent){
    return NumPorts;
  }

  if (!a_isVerilog) {
    stream << VHDL_LIBRARIES;
    stream << "entity TestBench is\n"
      << "end entity;\n"
      << "use work.all;\n";
  }
  return NumPorts;
}

// ---------------------------------------------------
// Write the beginning of digital netlist to the text stream 'stream'.
void Schematic::beginNetlistDigital(QTextStream& stream)
{
  if (a_isVerilog) {
    stream << "module TestBench ();\n";
    QList<DigSignal> values = a_Signals.values();
    QList<DigSignal>::const_iterator it;
    for (it = values.constBegin(); it != values.constEnd(); ++it) {
      stream << "  wire " << (*it).Name << ";\n";
    }
    stream << "\n";
  } else {
    stream << "architecture Arch_TestBench of TestBench is\n";
    QList<DigSignal> values = a_Signals.values();
    QList<DigSignal>::const_iterator it;
    for (it = values.constBegin(); it != values.constEnd(); ++it) {
      stream << "  signal " << (*it).Name << " : "
        << ((*it).Type.isEmpty() ?
        VHDL_SIGNAL_TYPE : (*it).Type) << ";\n";
    }
    stream << "begin\n";
  }

  if(a_Signals.find("gnd") != a_Signals.end()) {
    if (a_isVerilog) {
      stream << "  assign gnd = 0;\n";
    } else {
      stream << "  gnd <= '0';\n";  // should appear only once
    }
  }
}

// ---------------------------------------------------
// Write the end of digital netlist to the text stream 'stream'.
void Schematic::endNetlistDigital(QTextStream& stream)
{
  if (a_isVerilog) {
  } else {
    stream << "end architecture;\n";
  }
}

// ---------------------------------------------------
// write all components with node names into the netlist file
QString Schematic::createNetlist(QTextStream& stream, int NumPorts)
{
  if(!a_isAnalog) {
    beginNetlistDigital(stream);
  }

  a_Signals.clear();  // was filled in "giveNodeNames()"
  FileList.clear();

  QString s, Time;
  for(Component *pc : a_DocComps) {
    if(pc->Model == "CMD") continue; // Skip "system command" component. It must not go to the simulation backend
    if(a_isAnalog) {
      s = pc->getNetlist();
    }
    else {
      if(pc->Model == ".Digi" && pc->isActive) {  // simulation component ?
        if(NumPorts > 0) { // truth table simulation ?
          if (a_isVerilog)
            Time = QString::number((1 << NumPorts));
          else
            Time = QString::number((1 << NumPorts) - 1) + " ns";
        } else {
          Time = pc->Props.at(1)->Value;
        if (a_isVerilog) {
          if(!misc::Verilog_Time(Time, pc->Name)) return Time;
        } else {
          if(!misc::VHDL_Time(Time, pc->Name)) return Time;  // wrong time format
        }
        }
      }
      if (a_isVerilog) {
        s = pc->get_Verilog_Code(NumPorts);
      } else {
        s = pc->get_VHDL_Code(NumPorts);
      }
      if (s.length()>0 && s.at(0) == '\xA7'){
          return s; // return error
      }
    }
    stream << s;
  }

  if(!a_isAnalog) {
    endNetlistDigital(stream);
  }

  return Time;
}


void Schematic::clearSignalsAndFileList()
{
    a_Signals.clear();  // was filled in "giveNodeNames()"
    FileList.clear();
}

void Schematic::clearSignals()
{
    a_Signals.clear();
}
// vim:ts=8:sw=2:noet
