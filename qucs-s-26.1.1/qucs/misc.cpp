/***************************************************************************
                                 misc.cpp
                                ----------
    begin                : Wed Nov 11 2004
    copyright            : (C) 2014 by YodaLee
    email                : lc85301@gmail.com
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
 * \file misc.cpp
 * \Definition of some miscellaneous function
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <cmath>
#include "misc.h"
#include "ink.h"
#include "apptheme.h"
#include "main.h"
#include "qucs.h"
#include "schematic.h"

#include <cstdio>
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QMimeData>
#include <QSaveFile>
#include <QUrl>
#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX   // std::max below, not the macro
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <QtWidgets>


QString misc::getWindowTitle()
{
    QString title = QStringLiteral("%1 %2").arg(QUCS_NAME,PACKAGE_VERSION);
    if (title.endsWith(".0")) {
        title.chop(2);
    }
#if defined(GIT)
    if (title.endsWith(".99")) {
        QString hash = GIT;
        if (!hash.isEmpty()) {
            title.append("-").append(hash);
        }
    }
#endif

    return title;
}


bool misc::isDarkTheme()
{
    // The application's palette: the platform's, or the theme setting's.
    return qucs_s::apptheme::isDark();
}

QColor misc::paperColor()
{
    if (QucsSettings.PaperFollowsTheme) {
        const QColor paper = qucs_s::apptheme::paper(QucsSettings.Theme);
        if (paper.isValid()) return paper;
    }
    return QucsSettings.BGColor.isValid() ? QucsSettings.BGColor : QColor(255, 250, 225);
}

QColor misc::gridColor(const QColor& setting)
{
    if (QucsSettings.PaperFollowsTheme) {
        const QColor grid = qucs_s::apptheme::grid(QucsSettings.Theme);
        if (grid.isValid()) return grid;
    }
    return qucs_s::ink::on(setting);
}

QString misc::getIconPath(const QString &file)
{
    QString icon_path =":bitmaps/svg/"; // look for svg version first
    icon_path += file + ".svg";
    if (QFile::exists(icon_path)) {
        return icon_path;
    }
    icon_path =":bitmaps/";
    icon_path += file + ".png";
    return icon_path;
}

// #########################################################################
QString misc::complexRect(double real, double imag, int Precision)
{
  QString Text;
  if(fabs(imag) < 1e-250) Text = QString::number(real,'g',Precision);
  else {
    Text = QString::number(imag,'g',Precision);
    if(Text.at(0) == '-') {
      Text.replace(0,1,'j');
      Text = '-'+Text;
    }
    else  Text = "+j"+Text;
    Text = QString::number(real,'g',Precision) + Text;
  }
  return Text;
}

QString misc::complexDeg(double real, double imag, int Precision)
{
  QString Text;
  if(fabs(imag) < 1e-250) Text = QString::number(real,'g',Precision);
  else {
    Text  = QString::number(sqrt(real*real+imag*imag),'g',Precision) + " / ";
    Text += QString::number(180.0/pi*atan2(imag,real),'g',Precision) + QString::fromUtf8("°");
  }
  return Text;
}

QString misc::complexRad (double real, double imag, int Precision)
{
  QString Text;
  if(fabs(imag) < 1e-250) Text = QString::number(real,'g',Precision);
  else {
    Text  = QString::number(sqrt(real*real+imag*imag),'g',Precision);
    Text += " / " + QString::number(atan2(imag,real),'g',Precision) + "rad";
  }
  return Text;
}

// #########################################################################
QString misc::StringNum(double num, char form, int Precision)
{
  int a = 0;
  char *p, Buffer[512], Format[6] = "%.00g";

  if(Precision < 0) {
    Format[1]  = form;
    Format[2]  = 0;
  }
  else {
    Format[4]  = form;
    Format[2] += Precision / 10;
    Format[3] += Precision % 10;
  }
  sprintf(Buffer, Format, num);
  p = strchr(Buffer, 'e');
  if(p) {
    p++;
    if(*(p++) == '+') { a = 1; }   // remove '+' of exponent
    if(*p == '0') { a++; p++; }    // remove leading zeros of exponent
    if(a > 0)
      do {
        *(p-a) = *p;
      } while(*(p++) != 0);    // override characters not needed
  }

  return QString(Buffer);
}

// #########################################################################
QString misc::StringNiceNum(double num)
{
  char Format[6] = "%.8e";
  if(fabs(num) < 1e-250)  return QStringLiteral("0");  // avoid many problems
  if(fabs(log10(fabs(num))) < 3.0)  Format[3] = 'g';

  int a = 0;
  char *p, *pe, Buffer[512];

  sprintf(Buffer, Format, num);
  p = pe = strchr(Buffer, 'e');
  if(p) {
    if(*(++p) == '+') { a = 1; }    // remove '+' of exponent
    if(*(++p) == '0') { a++; p++; } // remove leading zeros of exponent
    if(a > 0)
      do {
        *(p-a) = *p;
      } while(*(p++) != 0);  // override characters not needed

    // In 'g' format, trailing zeros are already cut off !!!
    p = strchr(Buffer, '.');
    if(p) {
      if(!pe)  pe = Buffer + strlen(Buffer);
      p = pe-1;
      while(*p == '0')   // looking for unnecessary zero characters
        if((--p) <= Buffer)  break;
      if(*p != '.')  p++;  // no digit after decimal point ?
      while( (*(p++) = *(pe++)) != 0 ) ;  // overwrite zero characters
    }
  }

  return QString(Buffer);
}

// #########################################################################
void misc::str2num(const QString& s_, double& Number, QString& Unit, double& Factor)
{
  QString str = s_.trimmed();

/*  int i=0;
  bool neg = false;
  if(str[0] == '-') {      // check sign
    neg = true;
    i++;
  }
  else if(str[0] == '+')  i++;

  double num = 0.0;
  for(;;) {
    if(str[i] >= '0')  if(str[i] <= '9') {
      num = 10.0*num + double(str[i]-'0');
    }
  }*/

  QRegularExpression Expr( QRegularExpression("[^0-9\\x2E\\x2D\\x2B]") );
  auto i = str.indexOf( Expr );
  if(i >= 0)
    if((str.at(i).toLatin1() | 0x20) == 'e') {
      auto j = str.indexOf( Expr , ++i);
      if(j == i)  j--;
      i = j;
    }

  // (No letters: a number alone, with no unit - not itself as its unit.)
  Number = i < 0 ? str.toDouble() : str.left(i).toDouble();
  Unit   = i < 0 ? QString() : str.mid(i).trimmed();
  if(Unit.length()>0)
  {
    switch(Unit.at(0).toLatin1()) {
      case 'T': Factor = 1e12;  break;
      case 'G': Factor = 1e9;   break;
      case 'M': Factor = 1e6;   break;
      case 'k': Factor = 1e3;   break;
      case 'c': Factor = 1e-2;  break;
      case 'm': Factor = 1e-3;  break;
      case 'u': Factor = 1e-6;  break;
      case 'n': Factor = 1e-9;  break;
      case 'p': Factor = 1e-12; break;
      case 'f': Factor = 1e-15; break;
  //    case 'd':
      default:  Factor = 1.0;
    }
  }
  else
  {
    Factor = 1.0;
  }
}

// #########################################################################
QString misc::num2str(double Num, int Precision, QString unit)
{
  char c = 0;
  if (!std::isfinite(Num)) {   // "1e308k": no prefix for inf or nan
    QString Str = QString::number(Num);
    if (unit != "m") Str += unit;
    return Str;
  }
  double cal = fabs(Num);
  if(cal > 1e-20) {
    cal = log10(cal) / 3.0;
    if(cal < -0.2)  cal -= 0.98;
    int Expo = int(cal);

    if(Expo >= -5) if(Expo <= 4)
      switch(Expo) {
        case -5: c = 'f'; break;
        case -4: c = 'p'; break;
        case -3: c = 'n'; break;
        case -2: c = 'u'; break;
        case -1: c = 'm'; break;
        case  1: c = 'k'; break;
        case  2: c = 'M'; break;
        case  3: c = 'G'; break;
        case  4: c = 'T'; break;
      }

    if(c)  Num /= pow(10.0, double(3*Expo));
  }

  QString Str;
  if (Precision == -1) {
      Str = QString::number(Num);
  } else {
      Str = QString::number(Num,'f',Precision);
      qDebug() << Str;
  }

  if(c)  Str += c;

  if (unit != "m") Str += unit;

  return Str;
}

QColor misc::ColorFromString(const QString& color)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return QColor::fromString(color);
#else
    QColor c;
    c.setNamedColor(color);
    return c;
#endif

}

// #########################################################################
void misc::convert2Unicode(QString& Text)
{
  bool ok;
  int i = 0;
  QString n;
  unsigned short ch;
  while((i=Text.indexOf("\\x", i)) >= 0) {
    n = Text.mid(i, 6);
    ch = n.mid(2).toUShort(&ok, 16);
    if(ok)  Text.replace(n, QChar(ch));
    i++;
  }
  Text.replace("\\n", "\n");
  Text.replace("\\\\", "\\");
}

// #########################################################################
void misc::convert2ASCII(QString& Text)
{
  Text.replace('\\', "\\\\");
  Text.replace('\n', "\\n");

  int i = 0;
  QChar ch;
  char Str[8];
  while(Text.size()<i) {  // convert special characters
    if(ch > QChar(0x7F)) {
      sprintf(Str, "\\x%04X", ch.unicode());
      Text.replace(ch, Str);
    }
  }
}

// #########################################################################
// Converts a path to an absolute path
QString misc::properAbsFileName(const QString& filename, Schematic* sch)
{
  QString fName = filename;
  QFileInfo fileInfo(fName);

  if ( fileInfo.isAbsolute() ) {
    if ( fileInfo.exists() ) return fileInfo.canonicalFilePath();
    fName = fileInfo.fileName();
  }

  if ( sch != nullptr ) {
    fileInfo.setFile(sch->getFileInfo().dir().filePath(fName));
    if ( fileInfo.exists() ) return fileInfo.canonicalFilePath();
  }

  // A path relative to the project directory (the Content panel refers to
  // files in subdirectories that way), then the bare name in the usual places.
  fileInfo.setFile(QucsSettings.QucsWorkDir.filePath(fName));
  if ( fileInfo.exists() ) return fileInfo.canonicalFilePath();

  fName = fileInfo.fileName();

  fileInfo.setFile(QucsSettings.QucsWorkDir.filePath(fName));
  if ( fileInfo.exists() ) return fileInfo.canonicalFilePath();

  // A library of the author's own, named by where it was on their machine
  // (/users/russo/.qucs/user_lib/LT1057.lib), shipped in a user_lib folder
  // beside the schematic: looked for there, and in the project's and the
  // workspace's user_lib. (Not found, an SpLib part had no pins, and the
  // labels on them nothing to hold them.)
  QStringList userLibs;
  if (sch != nullptr) userLibs << sch->getFileInfo().dir().filePath(QStringLiteral("user_lib"));
  userLibs << QucsSettings.QucsWorkDir.filePath(QStringLiteral("user_lib"))
           << QucsSettings.qucsWorkspaceDir.filePath(QStringLiteral("user_lib"));
  for (const QString& path : std::as_const(userLibs)) {
    fileInfo.setFile(QDir(path).filePath(fName));
    if ( fileInfo.exists() ) return fileInfo.canonicalFilePath();
  }

  // A file of the shipped library, named by where that was on the
  // author's machine (C:/QUCS-S 24.3.0/share/qucs-s/library/XyceDigital.lib):
  // the same file in this installation's library. (Not found, the SPICE
  // library parts of the Xyce digital examples had no pins.)
  QString inLibrary = filename;
  inLibrary.replace(QLatin1Char('\\'), QLatin1Char('/'));
  const qsizetype at = inLibrary.lastIndexOf(QStringLiteral("/library/"), -1, Qt::CaseInsensitive);
  if (at >= 0 && !QucsSettings.LibDir.isEmpty()) {
    fileInfo.setFile(QDir(QucsSettings.LibDir).filePath(inLibrary.mid(at + 9)));
    if ( fileInfo.exists() ) return fileInfo.canonicalFilePath();
  }

  for (const QString& path : qucsPathList) {
    fileInfo.setFile(QDir(path).filePath(fName));
    if ( fileInfo.exists() ) return fileInfo.canonicalFilePath();
  }

  return filename;
}

// #########################################################################
QString misc::properFileName(const QString& Name)
{
  QFileInfo Info(Name);
  return Info.fileName();
}

// #########################################################################
namespace {
// QDirIterator would also walk hidden directories, so recurse by hand:
// no hidden entries (dot names, checked here as well - Qt 6.10 on macOS
// let a ".hidden" directory through the QDir filter), and no symbolic
// links to directories (cycles).
// Folders other programs fill with untold files, never a project's own:
// not entered. (A CMake build tree has its CMakeCache.txt.)
bool heavyFolder(const QFileInfo& folder)
{
  static const QStringList names = {QStringLiteral("node_modules"), QStringLiteral("__pycache__"), QStringLiteral("venv")};
  return names.contains(folder.fileName()) || QFileInfo::exists(folder.filePath() + QStringLiteral("/CMakeCache.txt"));
}

// \a budget: the files and folders still to be looked at; false once it is spent.
bool collectProjectFiles(const QDir& base, const QDir& dir, const QStringList& nameFilters, QStringList& files, int& budget)
{
  if (--budget < 0) return false;
  for (const QFileInfo& fi : dir.entryInfoList(nameFilters, QDir::Files, QDir::Unsorted)) {
    if (fi.fileName().startsWith(QLatin1Char('.'))) continue;
    if (--budget < 0) return false;
    files.append(base.relativeFilePath(fi.filePath()));
  }
  for (const QFileInfo& fi : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Unsorted))
    if (!fi.fileName().startsWith(QLatin1Char('.')) && !heavyFolder(fi)
        && !collectProjectFiles(base, QDir(fi.filePath()), nameFilters, files, budget))
      return false;
  return true;
}
} // namespace

QStringList misc::projectFiles(const QDir& root, const QStringList& nameFilters, bool* complete)
{
  const QDir base(root.absolutePath());
  QStringList files;
  int budget = MaxProjectEntries;
  const bool all = collectProjectFiles(base, base, nameFilters, files, budget);
  if (complete != nullptr) *complete = all;

  // Sorted by directory first, so the root's files come first and the
  // files of one subdirectory stay together.
  std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
    const QStringView dirA = QStringView(a).left(std::max<qsizetype>(a.lastIndexOf('/'), 0));
    const QStringView dirB = QStringView(b).left(std::max<qsizetype>(b.lastIndexOf('/'), 0));
    if (int c = dirA.compare(dirB, Qt::CaseInsensitive)) return c < 0;
    if (int c = a.compare(b, Qt::CaseInsensitive)) return c < 0;
    return a < b;
  });
  return files;
}

// #########################################################################
QString misc::canonicalDir(const QDir& dir)
{
  return dir.path().isEmpty() ? QString() : dir.canonicalPath();
}

// #########################################################################
QString misc::scratchDir()
{
  if (QucsMain != nullptr && !QucsMain->ProjName.isEmpty())
    return projectScratch(QucsSettings.QucsWorkDir.absolutePath());
  return QucsSettings.S4Qworkdir;
}

QString misc::cacheDir()
{
  if (!QucsSettings.workspaceOfRun.isEmpty())
    return QDir::toNativeSeparators(QucsSettings.workspaceOfRun + QStringLiteral("/spice4qucs"));
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString misc::projectScratch(const QString& projectDir)
{
  const QDir project(projectDir);
  const QString inside = project.absoluteFilePath(QLatin1String(ScratchFolder));
  const QString name = project.dirName();
  if ((name.size() > 4 && name.endsWith(QLatin1String("_prj"))) || QFileInfo(inside).isDir())
    return QDir::toNativeSeparators(inside);
  // (Named by the folder, and told apart from another of its name.)
  const QByteArray key = QCryptographicHash::hash(QDir::cleanPath(project.absolutePath()).toUtf8(), QCryptographicHash::Sha1).toHex().left(10);
  return QDir::toNativeSeparators(cacheDir() + QStringLiteral("/projects/")
                                  + name + QLatin1Char('-') + QString::fromLatin1(key));
}

// #########################################################################
QString misc::scratchDirFor(const QString& docName)
{
  const QString root = scratchDir();
  if (QucsMain == nullptr || QucsMain->ProjName.isEmpty()) return root;
  QString name;
  if (!docName.isEmpty()) {
    const QFileInfo info(docName);
    QString relative = QDir::fromNativeSeparators(QucsSettings.QucsWorkDir.relativeFilePath(info.absoluteFilePath()));
    if (relative.startsWith(QLatin1String("../")) || QDir::isAbsolutePath(relative))
      relative = info.fileName();   // not inside the project
    name = relative;
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > name.lastIndexOf(QLatin1Char('/'))) name.truncate(dot);   // drop the extension
  }
  if (name.isEmpty() || name == QLatin1String(".")) name = QStringLiteral("untitled");
  return QDir::toNativeSeparators(root + QLatin1Char('/') + name);
}

// #########################################################################
bool misc::isTextFile(const QString& path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return false;
  return !f.read(8192).contains('\0');
}

// #########################################################################
QStringList misc::localFiles(const QMimeData* data)
{
  QStringList files;
  if (data == nullptr || !data->hasUrls()) return files;
  for (const QUrl& url : data->urls())
    if (url.isLocalFile()) files.append(QDir::toNativeSeparators(url.toLocalFile()));
  return files;
}

// #########################################################################
namespace {
// The file a path names, as the file system knows it: its device and
// inode (volume serial number and file index on Windows). False when the
// path names nothing.
bool fileIdentity(const QString& path, quint64& device, quint64& index)
{
#ifdef Q_OS_WIN
  const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
  HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()), 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info;
  const bool ok = GetFileInformationByHandle(h, &info);
  CloseHandle(h);
  if (!ok) return false;
  device = info.dwVolumeSerialNumber;
  index = (quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  return true;
#else
  struct stat st;
  if (::stat(QFile::encodeName(path).constData(), &st) != 0) return false;
  device = quint64(st.st_dev);
  index = quint64(st.st_ino);
  return true;
#endif
}
}

bool misc::isSameFile(const QString& a, const QString& b)
{
  if (a.isEmpty() || b.isEmpty()) return false;
  if (QDir::cleanPath(QFileInfo(a).absoluteFilePath()) == QDir::cleanPath(QFileInfo(b).absoluteFilePath()))
    return true;
  quint64 deviceA = 0, indexA = 0, deviceB = 0, indexB = 0;
  if (!fileIdentity(a, deviceA, indexA) || !fileIdentity(b, deviceB, indexB)) return false;
  return deviceA == deviceB && indexA == indexB;
}

bool misc::copyFileOver(const QString& source, const QString& target, QString* error)
{
  auto fail = [error](const QString& why) {
    if (error != nullptr) *error = why;
    return false;
  };
  if (isSameFile(source, target)) return true;
  QFile in(source);
  if (!in.open(QIODevice::ReadOnly))
    return fail(QObject::tr("%1 cannot be read: %2").arg(QDir::toNativeSeparators(source), in.errorString()));
  // QSaveFile writes beside the target and renames over it on commit();
  // until then, and when anything fails, the target is left as it was.
  QSaveFile out(target);
  if (!out.open(QIODevice::WriteOnly))
    return fail(QObject::tr("%1 cannot be written: %2").arg(QDir::toNativeSeparators(target), out.errorString()));
  QByteArray block;
  while (!(block = in.read(1 << 20)).isEmpty())
    if (out.write(block) != block.size()) {
      out.cancelWriting();
      return fail(QObject::tr("%1 cannot be written: %2").arg(QDir::toNativeSeparators(target), out.errorString()));
    }
  if (in.error() != QFileDevice::NoError) {
    out.cancelWriting();
    return fail(QObject::tr("%1 cannot be read: %2").arg(QDir::toNativeSeparators(source), in.errorString()));
  }
  if (!out.commit())
    return fail(QObject::tr("%1 cannot be written: %2").arg(QDir::toNativeSeparators(target), out.errorString()));
  return true;
}

// #########################################################################
// Takes a file name (with path) and replaces all special characters.
QFont misc::pinFont()
{
  QFont font = QucsSettings.font;
  if (font.pointSizeF() > 0.0)
    font.setPointSizeF(std::max(5.0, font.pointSizeF() * 0.85));
  else if (font.pixelSize() > 0)
    font.setPixelSize(std::max(6, int(font.pixelSize() * 0.85)));
  return font;
}

QFont misc::canvasFont(QFont font)
{
  font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoSubpixelAntialias));
  return font;
}

// #########################################################################
QString misc::properName(const QString& Name)
{
  QString s = Name;
  QFileInfo Info(s);
  if(Info.suffix() == "sch")
    s.chop(4);
  if(s.isEmpty())   // a subcircuit without a file: nothing to name
    return s;
  if(s.at(0) <= '9') if(s.at(0) >= '0')
    s = 'n' + s;
  s.replace(QRegularExpression("\\W"), "_"); // none [a-zA-Z0-9] into "_"
  s.replace("__", "_");  // '__' not allowed in VHDL
  if(s.at(0) == '_')
    s = 'n' + s;
  return s;
}

// #########################################################################
// Takes a string and expands all $VAR/${VAR} environment variables
// NOTE: This doesn't do recursive expansion.
QString misc::expandEnvVars(const QString& input)
{
  QString result = input;
  // Group 1: ${VAR} (braces), Group 2: $VAR (alphanumeric/underscore)
  static const QRegularExpression envVarRegex(R"(\$\{([^}]+)\}|\$([A-Za-z_]\w*))");

  QList<QRegularExpressionMatch> matches;
  auto it = envVarRegex.globalMatch(result);
  while (it.hasNext()) {
      matches.append(it.next());
  }

  // Iterate from back of the string, to maintain valid string offsets during replacement
  for (int i = matches.size() - 1; i >= 0; --i) {
      const auto& match = matches.at(i);

      // Check if either brace (Group 1) or alphanumeric group (Group 2) matched
      QString varName = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
      QString envValue = qEnvironmentVariable(varName.toUtf8());

      // Replace captured group with expanded value of environment variable (only if it exists)
      if (!envValue.isNull()) {
          result.replace(match.capturedStart(), match.capturedLength(), envValue);
      }
  }

  return result;
}

// #########################################################################
// Creates and returns delay time for VHDL entities.
bool misc::VHDL_Delay(QString& td, const QString& Name)
{
  if(strtod(td.toLatin1(), 0) != 0.0) {  // delay time property
    if(!misc::VHDL_Time(td, Name))
      return false;    // time has not VHDL format
    td = " after " + td;
    return true;
  }
  else if(isalpha(td.toLatin1()[0])) {
    td = " after " + td;
    return true;
  }
  else {
    td = "";
    return true;
  }
}

// #########################################################################
// Checks and corrects a time (number & unit) according VHDL standard.
bool misc::VHDL_Time(QString& t, const QString& Name)
{
  char *p;
  QByteArray ba = t.toLatin1();
  double Time = strtod(ba.data(), &p);
  while(*p == ' ') p++;
  for(;;) {
    if(Time >= 0.0) {
      if(strcmp(p, "fs") == 0)  break;
      if(strcmp(p, "ps") == 0)  break;
      if(strcmp(p, "ns") == 0)  break;
      if(strcmp(p, "us") == 0)  break;
      if(strcmp(p, "ms") == 0)  break;
      if(strcmp(p, "sec") == 0) break;
      if(strcmp(p, "min") == 0) break;
      if(strcmp(p, "hr") == 0)  break;
    }
    t = QString::fromUtf8("§")  + QObject::tr("Error: Wrong time format in \"%1\". Use positive number with units").arg(Name)
            + " fs, ps, ns, us, ms, sec, min, hr.\n";
    return false;
  }

  t = QString::number(Time) + " " + QString(p);  // the space is mandatory !
  return true;
}

// #########################################################################
// Returns parameters for Verilog modules.
QString misc::Verilog_Param(const QString Value)
{
  if(strtod(Value.toLatin1(), 0) != 0.0) {
    QString td = Value;
    if(!misc::Verilog_Time(td, "parameter"))
      return Value;
    else
      return td;
  }
  else
    return Value;
}

// #########################################################################
// Creates and returns delay time for Verilog modules.
bool misc::Verilog_Delay(QString& td, const QString& Name)
{
  if(strtod(td.toLatin1(), 0) != 0.0) {  // delay time property
    if(!misc::Verilog_Time(td, Name))
      return false;    // time has not Verilog format
    td = " #" + td;
    return true;
  }
  else if(isalpha(td.toLatin1()[0])) {
    td = " #" + td;
    return true;
  }
  else {
    td = "";
    return true;
  }
}

// #########################################################################
// Checks and corrects a time (number & unit) according Verilog standard.
bool misc::Verilog_Time(QString& t, const QString& Name)
{
  char *p;
  QByteArray ba = t.toLatin1();
  double Time = strtod(ba.data(), &p);
  double factor = 1.0;
  while(*p == ' ') p++;
  for(;;) {
    if(Time >= 0.0) {
      if(strcmp(p, "fs") == 0) { factor = 1e-3; break; }
      if(strcmp(p, "ps") == 0) { factor = 1;  break; }
      if(strcmp(p, "ns") == 0) { factor = 1e3;  break; }
      if(strcmp(p, "us") == 0) { factor = 1e6;  break; }
      if(strcmp(p, "ms") == 0) { factor = 1e9;  break; }
      if(strcmp(p, "sec") == 0) { factor = 1e12; break; }
      if(strcmp(p, "min") == 0) { factor = 1e12*60; break; }
      if(strcmp(p, "hr") == 0)  { factor = 1e12*60*60; break; }
    }
    t = QString::fromUtf8("§")  + QObject::tr("Error: Wrong time format in \"%1\". Use positive number with units").arg(Name)
            + " fs, ps, ns, us, ms, sec, min, hr.\n";
    return false;
  }

  t = QString::number(Time*factor);
  return true;
}

// #########################################################################
bool misc::checkVersion(QString& Line)
{
  VersionTriplet SchVersion(Line);
  if (SchVersion > QucsVersion) return false;
  return true;
}

// Reports an error from loading or saving a document. While the GUI is
// running it is shown to the user; from the command line and in tests a
// modal box would wait forever for a click, so it goes to the log instead.
namespace {
misc::ErrorCapture* s_errorCapture = nullptr;
}

misc::ErrorCapture::ErrorCapture() : a_outer(s_errorCapture)
{
  s_errorCapture = this;
}

misc::ErrorCapture::~ErrorCapture()
{
  s_errorCapture = a_outer;
}

bool misc::ErrorCapture::active()
{
  return s_errorCapture != nullptr;
}

void misc::reportError(const QString& text)
{
  if (s_errorCapture != nullptr) {
    s_errorCapture->a_errors << text;
    return;
  }
  if (QucsMain != nullptr)
    QMessageBox::critical(nullptr, QObject::tr("Error"), text);
  else
    qCritical().noquote() << text;
}

// a small class to handle the application version string
//   loosely modeled after the standard Semantic Versioning...
VersionTriplet::VersionTriplet(const QString& version) {
  // Missing components ("26.1", "abc", "") and non-numeric ones read as 0;
  // value() is safe where at() would index past the end.
  const QStringList vl = version.trimmed().split('.');
  major = vl.value(0).toUInt();
  minor = vl.value(1).toUInt();
  patch = vl.value(2).toUInt();
}

QStringList misc::parseCmdArgs(const QString &program)
{
    QStringList args;
    QString tmp;
    int quoteCount = 0;
    bool inQuote = false;
    // handle quoting. tokens can be surrounded by double quotes
    // "hello world". three consecutive double quotes represent
    // the quote character itself.
    for (int i = 0; i < program.size(); ++i) {
        if (program.at(i) == QLatin1Char('"')) {
            ++quoteCount;
            if (quoteCount == 3) {
                // third consecutive quote
                quoteCount = 0;
                tmp += program.at(i);
            }
            continue;
        }
        if (quoteCount) {
            if (quoteCount == 1)
                inQuote = !inQuote;
            quoteCount = 0;
        }
        if (!inQuote && program.at(i).isSpace()) {
            if (!tmp.isEmpty()) {
                args += tmp;
                tmp.clear();
            }
        } else {
            tmp += program.at(i);
        }
    }
    if (!tmp.isEmpty())
        args += tmp;
    return args;
}


QString misc::wildcardToRegularExpression(const QString &wc_str, const bool enableEscaping)
{
    const int wclen = wc_str.length();
    QString rx;
    int i = 0;
    bool isEscaping = false; // the previous character is '\'
    const QChar *wc = wc_str.unicode();

    while (i < wclen) {
        const QChar c = wc[i++];
        switch (c.unicode()) {
        case '\\':
            if (enableEscaping) {
                if (isEscaping) {
                    rx += QLatin1String("\\\\");
                } // we insert the \\ later if necessary
                if (i+1 == wclen) { // the end
                    rx += QLatin1String("\\\\");
                }
            } else {
                rx += QLatin1String("\\\\");
            }
            isEscaping = true;
            break;
        case '*':
            if (isEscaping) {
                rx += QLatin1String("\\*");
                isEscaping = false;
            } else {
                rx += QLatin1String(".*");
            }
            break;
        case '?':
            if (isEscaping) {
                rx += QLatin1String("\\?");
                isEscaping = false;
            } else {
                rx += QLatin1Char('.');
            }

            break;
        case '$':
        case '(':
        case ')':
        case '+':
        case '.':
        case '^':
        case '{':
        case '|':
        case '}':
            if (isEscaping) {
                isEscaping = false;
                rx += QLatin1String("\\\\");
            }
            rx += QLatin1Char('\\');
            rx += c;
            break;
         case '[':
            if (isEscaping) {
                isEscaping = false;
                rx += QLatin1String("\\[");
            } else {
                rx += c;
                if (wc[i] == QLatin1Char('^'))
                    rx += wc[i++];
                if (i < wclen) {
                    if (rx[i] == QLatin1Char(']'))
                        rx += wc[i++];
                    while (i < wclen && wc[i] != QLatin1Char(']')) {
                        if (wc[i] == QLatin1Char('\\'))
                            rx += QLatin1Char('\\');
                        rx += wc[i++];
                    }
                }
            }
             break;

        case ']':
            if(isEscaping){
                isEscaping = false;
                rx += QLatin1String("\\");
            }
            rx += c;
            break;

        default:
            if(isEscaping){
                isEscaping = false;
                rx += QLatin1String("\\\\");
            }
            rx += c;
        }
    }
    return rx;
}

bool misc::simulatorExists(const QString &exe_file)
{
    if (exe_file.trimmed().isEmpty()) return false;   // "$PATH/" would be a directory
    if (QFile::exists(exe_file)) return true; // absolute path

    QFileInfo inf(exe_file); // try to find exe in $PATH
    char *p = getenv("PATH");
    QStringList paths;
    bool found = false;
    if (p != nullptr) paths = QString(p).split(':');
    for (const auto &pp : paths) {
        inf.setFile(pp + QDir::separator() + exe_file);
        if (inf.exists()) {
            found = true;
            break;
        }
    }
    return found;
}

QString misc::unwrapExePath(const QString &exe_file)
{
    if (exe_file.trimmed().isEmpty()) return exe_file;
    if (QFile::exists(exe_file)) return exe_file; // absolute path

    QFileInfo inf(exe_file); // try to find exe in $PATH
    char *p = getenv("PATH");
    QStringList paths;
    QString abs_exe_path = exe_file;
    if (p != nullptr) paths = QString(p).split(':');
    for (const auto &pp : paths) {
        inf.setFile(pp + QDir::separator() + exe_file);
        if (inf.exists()) {
            abs_exe_path = inf.canonicalFilePath();
            break;
        }
    }
    return abs_exe_path;
}

void misc::getSymbolPatternsList(QStringList &symbols)
{
  QString dir_name = QucsSettings.BinDir + "/../share/" QUCS_NAME "/symbols/";
  QDir sym_dir(dir_name);
  QStringList sym_files = sym_dir.entryList(QDir::Files);
  for (const QString& file : sym_files) {
    QFileInfo inf(file);
    symbols.append(inf.baseName());
  }
}

VersionTriplet::VersionTriplet(){
  major = minor = patch = 0;
}

bool VersionTriplet::operator==(const VersionTriplet& v2) {
  if (this->major != v2.major)
    return false;
  if (this->minor != v2.minor)
    return false;
  return true;
}

bool VersionTriplet::operator>(const VersionTriplet& v2) {
  if (this->major < v2.major)
    return false;
  if (this->major > v2.major)
    return true;

  if (this->minor < v2.minor)
    return false;
  if (this->minor > v2.minor)
    return true;

  return false;
}

bool VersionTriplet::operator<(const VersionTriplet& v2) {
  if (this->major > v2.major)
    return false;
  if (this->major < v2.major)
    return true;

  if (this->minor > v2.minor)
    return false;
  if (this->minor < v2.minor)
    return true;

  return false;
}

bool VersionTriplet::operator>=(const VersionTriplet& v2) {
  return !((*this) < v2);
}

bool VersionTriplet::operator<=(const VersionTriplet& v2) {
  return !((*this) > v2);
}

QString VersionTriplet::toString() {
  return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

// This function can parse TeX sequences _{…} and ^{…} in given string and
// render them as subscripts and superscripts respectively. It does it
// by manipulating text position and font size.
//
// The implementation is taken from ViewPainter::drawTextMapped and adapted
// to work with QPainter and as standalone function outside of class.
void misc::draw_richtext(QPainter* painter, int x, int y, const QString &text, QRectF* br) {
  QRectF all_bounding_rect;
  int current_text_x = x;
  int current_text_y = y;
  int i = 0;

  const bool font_size_in_pixels = painter->font().pixelSize() != -1;

  QFont smaller_font{ painter->font() };

  if (font_size_in_pixels) {
    smaller_font.setPixelSize(static_cast<int>(smaller_font.pixelSize() * 0.8));
  } else {
    smaller_font.setPointSizeF(smaller_font.pointSizeF() * 0.8);
  }

  const double font_size = font_size_in_pixels ? smaller_font.pixelSize() : smaller_font.pointSizeF();
  const int subscript_offset = static_cast<int>(std::round(0.6 * font_size));
  const int superscript_offset = static_cast<int>(std::round(-0.3 * font_size));

  while (text.length()>i) {
    if ((text[i].toLatin1() == '_' || text[i].toLatin1() == '^')) {
      if ((i+1) >= text.length()) break;
      bool is_sub = text[i++].toLatin1() == '_';
      int len = 0;

      if (text[i] == '{') {
        i++;
        while (!text[i+len].isNull() && text[i+len].toLatin1() != '}') len++;
      }

      painter->save();
      painter->setFont(smaller_font);

      QRect fragment_br;
      painter->drawText(
        current_text_x, current_text_y + (is_sub ? subscript_offset : superscript_offset),
        1, 1,
        Qt::TextDontClip,
        text.mid(i, len ? len : 1),
        &fragment_br);
      painter->restore();
      all_bounding_rect |= fragment_br;

      current_text_x += fragment_br.width();
      i += len ? len + 1 : 1;
    }
    else
    {
      int len = 0;
      while (text.length()>(i+len)
             /*!Text[i+len].isNull()*/ && text[i+len].toLatin1() != '_' &&
	     text[i+len].toLatin1() != '^' && text[i+len].toLatin1() != '\n')
			len++;

      QRect fragment_br;
      painter->drawText(
        current_text_x, current_text_y, 1, 1,
        Qt::TextDontClip,
        text.mid(i, len),
        &fragment_br);

      all_bounding_rect |= fragment_br;
      current_text_x += fragment_br.width();

      if (text.length()>(i+len)&&text[i+len].toLatin1() == '\n') {
		    current_text_x = x;
		    current_text_y = all_bounding_rect.bottom();
		    i++;
      }
      i += len;
    }
  }

  if (br) {
    *br = all_bounding_rect;
  }
}

// Some elements on schematics may be resized and have "resize handles"
// for this. The handles are little squares which are drawn in the same
// size despite of schematic's zoom factor.
// This function draws a handle with center at given point.
void misc::draw_resize_handle(QPainter* painter, const QPointF& center) {
  // The received central point has some "real" coordinates which are tied
  // to an element for which the handle belongs.
  // Painter almost certainly has the "scale" transformation, but the handle
  // must be drawn in the same size, without any scaling.
  // Given all above, this is the way to draw a resize handle:
  // 1. Find out where on canvas lies the central point and remember these
  //    coordinates
  // 2. Remove all transformations and draw the handle in its natural size
  static QRectF resize_handle{0, 0, 10, 10};  // nothing special, just a size
  resize_handle.moveCenter(painter->transform().map(center));

  static QTransform transform{}; // reset transformation
  painter->save();
  painter->setTransform(transform);
  painter->setPen(QPen{Qt::darkRed, 2});
  painter->drawRect(resize_handle);
  painter->restore();
}

QString misc::formatValue(const QString& input, int precision) {
    QRegularExpression regex(R"(([+-]?\d*\.?\d+)([a-zA-Z%]+)?)");
    QRegularExpressionMatch match = regex.match(input);

    if (match.hasMatch()) {
        QString numberPart = match.captured(1);
        QString unitPart = match.captured(2);

        double value = numberPart.toDouble();
        QString formattedNumber = QString::number(value, 'f', precision);

        formattedNumber = formattedNumber.remove(QRegularExpression(R"(0+$)"));
        formattedNumber = formattedNumber.remove(QRegularExpression(R"(\.$)"));

        return formattedNumber + unitPart;
    }
    return input;
}
