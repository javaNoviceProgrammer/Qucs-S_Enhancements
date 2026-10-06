/***************************************************************************
                               libcomp.cpp
                              -------------
    begin                : Fri Jun 10 2005
    copyright            : (C) 2005 by Michael Margraf
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

#include "libcomp.h"
#include "main.h"
#include "misc.h"
#include "node.h"
#include "projectlibraries.h"
#include "schematic.h"
#include "extsimkernels/qucs2spice.h"
#include "extsimkernels/spicecompat.h"


#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDateTime>
#include <QHash>
#include <QSet>

#include <functional>
#include <QDebug>

LibComp::LibComp()
{
  Type = isComponent;   // both analog and digital
  Description = QObject::tr("Component taken from Qucs library");

  Ports.append(new Port(0,  0));  // dummy port because of being device

  Model = "Lib";
  Name  = "X";
  SpiceModel = "X";

  Props.append(new Property("Lib", "", true,
		QObject::tr("name of qucs library file")));
  Props.append(new Property("Comp", "", true,
		QObject::tr("name of component in library")));
}

// ---------------------------------------------------------------------
Component* LibComp::newOne()
{
  LibComp *p = new LibComp();
  p->Props.at(0)->Value = Props.at(0)->Value;
  p->Props.at(1)->Value = Props.at(1)->Value;
  p->recreate();
  return p;
}

QString LibComp::componentModel()
{
  // As the library panel reads it (makeModelString): a model of one line
  // is that component, whatever the symbol - the library's default one too.
  QString model;
  if (Props.size() < 2 || loadSection("Model", model) < 0) return {};
  model = model.trimmed();
  return model.startsWith('<') && model.endsWith('>') && !model.contains('\n') ? model : QString();
}

// ---------------------------------------------------------------------
// Makes the schematic symbol subcircuit with the correct number
// of ports.
void LibComp::createSymbol()
{
  tx = INT_MIN;
  ty = INT_MIN;
  if(loadSymbol() > 0) {
    if(tx == INT_MIN)  tx = x1+4;
    if(ty == INT_MIN)  ty = y2+4;
    namePinsFromModel();
  }
  else {
    // only paint a rectangle
    Lines.append(new qucs::Line(-15, -15, 15, -15, QPen(Qt::darkBlue,2)));
    Lines.append(new qucs::Line( 15, -15, 15,  15, QPen(Qt::darkBlue,2)));
    Lines.append(new qucs::Line(-15,  15, 15,  15, QPen(Qt::darkBlue,2)));
    Lines.append(new qucs::Line(-15, -15,-15,  15, QPen(Qt::darkBlue,2)));

    x1 = -18; y1 = -18;
    x2 =  18; y2 =  18;

    tx = x1+4;
    ty = y2+4;
  }
}

// ---------------------------------------------------------------------
// Loads the section with name "Name" from library file into "Section".
int LibComp::loadSection(const QString& Name, QString& Section,
             QStringList *Includes, QStringList *Attach)
{
  return loadSectionOf(libraryFile(), Props.at(1)->Value, Name, Section, Includes, Attach);
}

int LibComp::loadSectionOf(const QString& libraryFile, const QString& comp, const QString& Name, QString& Section,
                           QStringList *Includes, QStringList *Attach)
{
  QFile file(libraryFile);
  if(!file.open(QIODevice::ReadOnly))
    return -1;

  QString libDefaultSymbol;

  QTextStream ReadWhole(&file);
  Section = ReadWhole.readAll();
  file.close();


  if(Section.left(14) != "<Qucs Library ")  // wrong file type ?
    return -2;

  int Start, End = Section.indexOf(' ', 14);
  if(End < 15) return -3;
  QString Line = Section.mid(14, End-14); // extract version string
  VersionTriplet LibVersion = VersionTriplet(Line);
  if (LibVersion > QucsVersion) {// wrong version number ?
      if (!QucsSettings.IgnoreFutureVersion) {
          return -3;
      }
  }

  if(Name == "Symbol") {
    Start = Section.indexOf("\n<", 14); // library has default symbol
    if(Start > 0)
      if(Section.mid(Start+2, 14) == "DefaultSymbol>") {
        Start += 16;
        End = Section.indexOf("\n</DefaultSymbol>", Start);
        if(End < 0)  return -9;
        libDefaultSymbol = Section.mid(Start, End-Start);
      }
  }

  // search component
  Line = "\n<Component " + comp + ">";
  Start = Section.indexOf(Line);
  if(Start < 0)  return -4;  // component not found
  Start = Section.indexOf('\n', Start);
  if(Start < 0)  return -5;  // file corrupt
  Start++;
  End = Section.indexOf("\n</Component>", Start);
  if(End < 0)  return -6;  // file corrupt
  Section = Section.mid(Start, End-Start+1);
  
  // search model includes
  if(Includes) {
    int StartI, EndI;
    StartI = Section.indexOf("<"+Name+"Includes");
    if(StartI >= 0) {  // includes found
      StartI = Section.indexOf('"', StartI);
      if(StartI < 0)  return -10;  // file corrupt
      EndI = Section.indexOf('>', StartI);
      if(EndI < 0)  return -11;  // file corrupt
      StartI++; EndI--;
      QString inc = Section.mid(StartI, EndI-StartI);
      QStringList f = inc.split(QRegularExpression("\"\\s+\""));
      for(QStringList::Iterator it = f.begin(); it != f.end(); ++it ) {
	Includes->append(*it);
      }
    }
  }

  // search attached files
  if(Attach) {
    int StartI, EndI;
    StartI = Section.indexOf("<"+Name+"Attach");
    if(StartI >= 0) {  // includes found
      StartI = Section.indexOf('"', StartI);
      if(StartI < 0)  return -10;  // file corrupt
      EndI = Section.indexOf('>', StartI);
      if(EndI < 0)  return -11;  // file corrupt
      StartI++; EndI--;
      QString inc = Section.mid(StartI, EndI-StartI);
      QStringList f = inc.split(QRegularExpression("\"\\s+\""));
      for(QStringList::Iterator it = f.begin(); it != f.end(); ++it ) {
    Attach->append(*it);
      }
    }
  }

  // search model
  Start = Section.indexOf("<"+Name+">");
  if(Start < 0) {
    if((Name == "Symbol") && (!libDefaultSymbol.isEmpty())) {
      // component does not define its own symbol but the library defines a default symbol
      Section = libDefaultSymbol;
      return 0;
    } else {
      return -7;  // symbol not found
    }
  }
  Start = Section.indexOf('\n', Start);
  if(Start < 0)  return -8;  // file corrupt
  while(Section.at(++Start) == ' ') ;
  End = Section.indexOf("</"+Name+">", Start);
  if(End < 0)  return -9;  // file corrupt

  // snip actual model
  Section = Section.mid(Start, End-Start);
  return 0;
}

// ---------------------------------------------------------------------
QString LibComp::description()
{
  QString text;
  if (Props.size() < 2 || loadSection("Description", text) < 0) return QString();
  return text.simplified();
}

// ---------------------------------------------------------------------
void LibComp::namePinsFromModel()
{
  QString model;
  if (Ports.isEmpty() || loadSection("Model", model) < 0) return;
  // Its subcircuits (they nest): each one's ports and lines.
  struct Def {
    QStringList ports, lines;
  };
  QHash<QString, Def> defs;
  QString top;
  QStringList open;
  static const QRegularExpression space(QStringLiteral("\\s+"));
  for (const QString& raw : model.split(QLatin1Char('\n'))) {
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1String(".Def:End"))) {
      if (!open.isEmpty()) open.removeLast();
      continue;
    }
    if (line.startsWith(QLatin1String(".Def:"))) {
      const QStringList fields = line.mid(5).split(space, Qt::SkipEmptyParts);
      if (fields.isEmpty()) return;
      if (top.isEmpty()) top = fields.first();
      defs.insert(fields.first(), Def{fields.mid(1), {}});
      open << fields.first();
      continue;
    }
    if (!open.isEmpty() && !line.isEmpty()) defs[open.last()].lines << line;
  }
  // (The part's own subcircuit, Lib_Comp, where the model defines others
  // first - AD825's and LM3886's inner models come before it.)
  if (const QString own = createType(); defs.contains(own)) top = own;
  if (top.isEmpty() || defs.value(top).ports.size() != Ports.size()) return;
  // A node's name: its own (_netC, _netP_INN, the Boyle models' _netN_INP),
  // else the name of the port of the subcircuit it goes into.
  static const QRegularExpression named(QStringLiteral("^_net(?:[PN]_)?([A-Za-z][A-Za-z0-9_]*)$"));
  std::function<QString(const QString&, const QString&, int)> nameOf = [&](const QString& def, const QString& node, int depth) {
    if (const QRegularExpressionMatch m = named.match(node); m.hasMatch()) return m.captured(1);
    if (depth > 4) return QString();
    for (const QString& line : defs.value(def).lines) {
      if (!line.startsWith(QLatin1String("Sub:"))) continue;
      QStringList nodes;
      QString inner;
      for (const QString& f : line.split(space, Qt::SkipEmptyParts).mid(1)) {
        if (f.startsWith(QLatin1String("Type=\""))) inner = f.mid(6).chopped(1);
        else if (!f.contains(QLatin1Char('='))) nodes << f;
      }
      const qsizetype at = nodes.indexOf(node);
      if (at < 0 || !defs.contains(inner) || at >= defs.value(inner).ports.size()) continue;
      if (const QString name = nameOf(inner, defs.value(inner).ports.at(at), depth + 1); !name.isEmpty()) return name;
    }
    return QString();
  };
  QStringList names;
  for (const QString& node : defs.value(top).ports) {
    const QString name = nameOf(top, node, 0);
    if (name.isEmpty() || names.contains(name, Qt::CaseInsensitive)) return;
    names << name;
  }
  for (int i = 0; i < Ports.size(); ++i)
    if (Ports.at(i)->Name.isEmpty()) Ports.at(i)->Name = names.at(i);
}

// ---------------------------------------------------------------------
// Loads the symbol for the subcircuit from the schematic file and
// returns the number of painting elements.
int LibComp::loadSymbol()
{
  int z, Result;
  QString FileString, Line;
  z = loadSection("Symbol", FileString);
  if(z < 0) {
    if(z != -7)  return z;

    // If library component not defined as subcircuit, then load
    // new component and transfer data to this component.
    z = loadSection("Model", Line);
    if(z < 0)  return z;

    std::shared_ptr<Component> pc(getComponentFromName(Line));
    if(!pc)  return -20;
    copyComponent(pc.get());

    return 1;
  }


  z  = 0;
  x1 = y1 = INT_MAX;
  x2 = y2 = INT_MIN;

  QTextStream stream(&FileString, QIODevice::ReadOnly);
  while(!stream.atEnd()) {
    Line = stream.readLine();
    Line = Line.trimmed();
    if(Line.isEmpty())  continue;
    if(Line.at(0) != '<') return -11;
    if(Line.at(Line.length()-1) != '>') return -12;
    Line = Line.mid(1, Line.length()-2); // cut off start and end character
    Result = analyseLine(Line, 2);
    if(Result < 0) return -13;   // line format error
    z += Result;
  }

  x1 -= 4;  x2 += 4;   // enlarge component boundings a little
  y1 -= 4;  y2 += 4;
  return z;      // return number of ports
}

// -------------------------------------------------------
QString LibComp::libraryFile() const
{
  return libraryFileOf(Props.first()->Value, containingSchematic != nullptr ? containingSchematic->getFileInfo().dir().path() : QString(),
                       Props.at(1)->Value);
}

QStringList LibComp::librariesNamed(const QString& lib, const QString& folder)
{
  return misc::properAbsFileNamesIn(QDir(QucsSettings.LibDir).absoluteFilePath(lib + ".lib"), folder);
}

QString LibComp::libraryFileOf(const QString& lib, const QString& folder, const QString& comp, const QString& project)
{
  const QString file = QDir(QucsSettings.LibDir).absoluteFilePath(lib + ".lib");
  if (comp.isEmpty()) return misc::properAbsFileNameIn(file, folder);
  // (Not the first file of the name: a project's library of it with no
  // such part, made after the part was placed from a search path's, took
  // the part - its pins and its Verilog-A gone.)
  const auto having = [&comp](const QString& library) { return hasComponent(library, comp); };
  // The library at its path - a path it names, the installed one of a
  // name - when that has it: the one meant.
  if (const QString at = QFileInfo(file).canonicalFilePath(); !at.isEmpty() && having(at)) return at;
  // Else the one the project linked the Verilog-A of from, of those of its name that have it.
  QStringList linked;
  const QString name = QFileInfo(file).completeBaseName();
  linked << qucs_s::projectlibraries::linkedFolders(project.isEmpty() ? QucsSettings.QucsWorkDir.absolutePath() : project, name);
  if (!folder.isEmpty()) linked << qucs_s::projectlibraries::linkedFolders(folder, name);   // (a project's schematic netlisted with none open)
  if (!linked.isEmpty()) {
    const QString found = misc::properAbsFileNameWhere(file, folder, [&](const QString& library) {
      return linked.contains(qucs_s::projectlibraries::folderOf(library)) && having(library);
    });
    if (!found.isEmpty()) return found;
  }
  const QString found = misc::properAbsFileNameWhere(file, folder, having);
  return found.isEmpty() ? file : found;   // (none has it: not loaded)
}

QStringList LibComp::librariesNamedLike(const QString& libraryFile)
{
  const QFileInfo info(libraryFile);
  QStringList others = librariesNamed(info.completeBaseName(), QucsSettings.QucsWorkDir.absolutePath());
  others.removeAll(info.canonicalFilePath());
  return others;
}

namespace {

// What a library says of one of its components.
struct LibraryPart {
  bool alwaysLoad = false;   // <AlwaysLoadOSDI>
  QStringList pins;          // its SPICE model's (<Spice>) .SUBCKT's pins, in order
};

// A library's components, read once while the file is unchanged - as
// loadSectionOf() finds one: "\n<Component NAME>" to "\n</Component>", in
// a Qucs library.
struct LibraryRead {
  QDateTime modified;
  qint64 size = -1;
  QStringList order;
  QHash<QString, LibraryPart> parts;
};

// The pins of the .SUBCKT line of the subcircuit \a name in \a spice - its
// continuation lines (+) joined -, else of the last one there (the
// component's own comes after those it places): the names after the
// subcircuit's up to its parameters (R=1k, params:).
QStringList subcircuitPins(const QString& spice, const QString& name)
{
  QStringList lines;
  for (const QString& raw : spice.split(QLatin1Char('\n'))) {
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('+')) && !lines.isEmpty()) lines.last() += QLatin1Char(' ') + line.mid(1);
    else lines << line;
  }
  QStringList found;
  for (const QString& line : std::as_const(lines)) {
    const QStringList words = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (words.size() < 2 || words.first().compare(QLatin1String(".SUBCKT"), Qt::CaseInsensitive) != 0) continue;
    QStringList pins;
    for (qsizetype i = 2; i < words.size(); ++i) {
      if (words.at(i).contains(QLatin1Char('=')) || words.at(i).compare(QLatin1String("params:"), Qt::CaseInsensitive) == 0) break;
      pins << words.at(i);
    }
    found = pins;
    if (words.at(1).compare(name, Qt::CaseInsensitive) == 0) return pins;
  }
  return found;
}

const LibraryRead& readLibrary(const QString& libraryFile)
{
  static QHash<QString, LibraryRead> known;
  static const LibraryRead none;
  const QFileInfo info(libraryFile);
  if (!info.isFile()) return none;
  const QString key = info.absoluteFilePath();
  auto it = known.find(key);
  if (it != known.end() && it->modified == info.lastModified() && it->size == info.size()) return *it;
  LibraryRead read{info.lastModified(), info.size(), {}, {}};
  QFile f(key);
  if (f.open(QIODevice::ReadOnly)) {
    QTextStream stream(&f);
    const QString text = stream.readAll();   // (read as loadSectionOf() reads it: a byte order mark left out)
    if (text.startsWith(QLatin1String("<Qucs Library ")))
      for (qsizetype at = text.indexOf(QLatin1String("\n<Component ")); at >= 0; at = text.indexOf(QLatin1String("\n<Component "), at + 1)) {
        const qsizetype close = text.indexOf(QLatin1Char('>'), at);
        if (close < 0) continue;
        const QString name = text.mid(at + 12, close - at - 12);
        const qsizetype end = text.indexOf(QLatin1String("\n</Component>"), close);
        const QString definition = text.mid(close + 1, end < 0 ? -1 : end - close - 1);
        LibraryPart part;
        part.alwaysLoad = definition.contains(QRegularExpression(QStringLiteral("\\n\\s*<AlwaysLoadOSDI>")));
        const qsizetype spice = definition.indexOf(QLatin1String("<Spice>"));
        if (spice >= 0) {
          const qsizetype spiceEnd = definition.indexOf(QLatin1String("</Spice>"), spice);
          // The subcircuit a part names (createType()): by the library's
          // name - a part's Lib is that, or the library's path without
          // ".lib" - and the component's.
          part.pins = subcircuitPins(definition.mid(spice + 7, spiceEnd < 0 ? -1 : spiceEnd - spice - 7),
                                     LibComp::subcircuitName(info.completeBaseName(), name));
        }
        if (!read.parts.contains(name)) read.order << name;
        read.parts.insert(name, part);
      }
  }
  return *known.insert(key, read);
}

} // namespace

bool LibComp::hasComponent(const QString& libraryFile, const QString& comp)
{
  return readLibrary(libraryFile).parts.contains(comp);
}

bool LibComp::takesGround(const QString& libraryFile, const QString& comp, int pins)
{
  const LibraryRead& read = readLibrary(libraryFile);
  const auto it = read.parts.constFind(comp);
  // No SPICE model (its Qucs model made one), or no .SUBCKT in it: as it
  // always was.
  if (it == read.parts.constEnd() || it->pins.isEmpty()) return true;
  // The part's pins and no more: no ground pin.
  if (it->pins.size() == pins) return false;
  // One more - gnd, as Create Library writes it - or a count that fits
  // neither, whose netlist no simulator takes either way: as it always was.
  return true;
}

QStringList LibComp::alwaysLoaded(const QString& libraryFile)
{
  const LibraryRead& read = readLibrary(libraryFile);
  QStringList marked;
  for (const QString& name : read.order)
    if (read.parts.value(name).alwaysLoad) marked << name;
  return marked;
}

QString LibComp::referenceTo(const QString& libraryFile)
{
  const QFileInfo info(libraryFile);
  QString path = info.absoluteFilePath();
  path.chop(4);   // ".lib"
  // (A name with a dot: the libraries panel's parser names it shorter.)
  const QString name = info.completeBaseName();
  if (name.isEmpty() || name.contains(QLatin1Char('.'))) return path;
  const QString real = info.canonicalFilePath();
  return !real.isEmpty() && QFileInfo(libraryFileOf(name, QString())).canonicalFilePath() == real ? name : path;
}

QString LibComp::getSubcircuitFile()
{
  QString FileName = libraryFile();
  FileName.chop(4);
  return FileName;
}

// -------------------------------------------------------
bool LibComp::createSubNetlist(QTextStream *stream, QStringList &FileList,
			       int type)
{
  int r = -1;
  QString FileString;
  QStringList Includes;
  if(type&1) {
    r = loadSection("Model", FileString, &Includes);
  } else if(type&2) {
    r = loadSection("VHDLModel", FileString, &Includes);
  } else if(type&4) {
    r = loadSection("VerilogModel", FileString, &Includes);
  } else if(type&8) {
    r = loadSection("Spice",FileString, &Includes);
    if (r<0) {
        r = loadSection("Model", FileString, &Includes); // Ngspice
        FileString = qucs2spice::convert_netlist(FileString);
    }
  } else if (type&16) {
      r = loadSection("Spice",FileString, &Includes);
      if (r<0) {
          r = loadSection("Model", FileString, &Includes); // Ngspice
          FileString = qucs2spice::convert_netlist(FileString,true);
      }
  }
  if(r < 0)  return false;

  // also include files
  int error = 0;
  for(QStringList::Iterator it = Includes.begin();
      it != Includes.end(); ++it ) {
    QString s = getSubcircuitFile()+"/"+*it;
    if(FileList.indexOf(s) >= 0) continue;
    FileList.append(s);

    // load file and stuff into stream
    QFile file(s);
    if(!file.open(QIODevice::ReadOnly)) {
      error++;
    } else {
      QByteArray FileContent = file.readAll();
      file.close();
      //?stream->writeRawBytes(FileContent.data(), FileContent.size());
      (*stream) << FileContent.data();
      qDebug() << "hi from libcomp";
    }
  }

  (*stream) << "\n" << FileString << "\n";
  return error > 0 ? false : true;
}

// -------------------------------------------------------
QString LibComp::createType()
{
  return subcircuitName(Props.at(0)->Value, Props.at(1)->Value);
}

QString LibComp::subcircuitName(const QString& lib, const QString& comp)
{
  return misc::properName(misc::properFileName(lib) + "_" + comp);
}

// -------------------------------------------------------
QString LibComp::netlist()
{
  QString s = "Sub:"+Name;   // output as subcircuit

  // output all node names
  for (Port *p1 : Ports)
    s += " "+p1->Connection->Name;   // node names

  // output property
  s += " Type=\""+createType()+"\"";   // type for subcircuit

  // output user defined parameters
  for(int i = 2;i<Props.size();i++)
    s += " "+Props.at(i)->Name+"=\""+Props.at(i)->Value+"\"";

  return s + '\n';
}

// -------------------------------------------------------
QString LibComp::verilogCode(int)
{
  QString s = "  Sub_" + createType() + " " + Name + " (";

  // output all node names
  QListIterator<Port *> iport(Ports);
  Port *pp = iport.next();
  if(pp)  s += pp->Connection->Name;
  while (iport.hasNext()) {
    pp = iport.next();
    s += ", "+pp->Connection->Name;   // node names
  }

  s += ");\n";
  return s;
}

// -------------------------------------------------------
QString LibComp::vhdlCode(int)
{
  QString s = "  " + Name + ": entity Sub_" + createType() + " port map (";

  // output all node names
  QListIterator<Port *> iport(Ports);
  Port *pp = iport.next();
  if(pp)  s += pp->Connection->Name;
  while (iport.hasNext()) {
    pp = iport.next();
    s += ", "+pp->Connection->Name;   // node names
  }

  s += ");\n";
  return s;
}

QString LibComp::spice_netlist(spicecompat::SpiceDialect dialect /* = spicecompat::SPICEDefault */)
{
    Q_UNUSED(dialect);

    // The first pin of a library's subcircuit that has one for its ground
    // (gnd: takesGround()) tied to the circuit's; one made without has none.
    QString s = SpiceModel + Name;
    if (takesGround(libraryFile(), Props.at(1)->Value, int(Ports.size()))) s += QStringLiteral(" 0");
    for (Port *p1 : Ports)
      s += " "  + spicecompat::normalize_node_name(p1->Connection->Name);   // node names
    s += " " + createType();

    // output user defined parameters
    for(int i = 2;i<Props.size();i++) {
      QString val = spicecompat::normalize_value(Props.at(i)->Value);
      s += " "+Props.at(i)->Name+"="+val;
    }
    s +="\n";

    return s;
}

QString LibComp::cdl_netlist()
{
    return spice_netlist(spicecompat::CDL);
}

QStringList LibComp::getVerilogAFiles()
{
  return verilogAFilesOf(libraryFile(), Props.at(1)->Value);
}

QStringList LibComp::verilogAFilesOf(const QString& libraryFile, const QString& comp)
{
  QString content;
  QStringList includes, attach;
  if (loadSectionOf(libraryFile, comp, "Spice", content, &includes, &attach) < 0)
    return {};
  QString library = libraryFile;
  library.chop(4);   // its folder: the file without ".lib"
  const QDir folder(library);
  QStringList files;
  for (const QString &file : std::as_const(attach)) {
    if (file.endsWith(".osdi", Qt::CaseInsensitive)) {
      files.append(folder.absoluteFilePath(file));
    } else if (file.endsWith(".va", Qt::CaseInsensitive)) {
      files.append(folder.absoluteFilePath(file));
      const QString model = folder.absoluteFilePath(QFileInfo(file).completeBaseName() + ".osdi");
      if (QFileInfo(model).isFile() && !files.contains(model)) files.append(model);
    }
  }
  return files;
}

QString LibComp::getSpiceLibrary()
{
  QStringList files;
  QString content;
  QStringList includes,attach;

  int r = loadSection("Spice",content,&includes,&attach);
  if (r<0) {
    return QString();
  }
  for (const auto &file : attach) {
    if (file.endsWith(".cir", Qt::CaseInsensitive) ||
        file.endsWith(".ckt", Qt::CaseInsensitive) ||
        file.endsWith(".lib", Qt::CaseInsensitive) ||
        file.endsWith(".sp", Qt::CaseInsensitive)) {
      files.append(getSubcircuitFile()+'/'+file);
    }
  }

  QString s;
  for (const auto &file: files) { // for netlist
    s += QStringLiteral(".INCLUDE \"%1\"\n").arg(file);
  }
  return s;
}
