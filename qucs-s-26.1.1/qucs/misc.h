/***************************************************************************
                                  misc.h
                                 --------
    begin                : Wed Nov 12 2004
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
#ifndef MISC_H
#define MISC_H

/*!
 * \file misc.h
 * \Declaration of some miscellaneous function
 */

#include <QPushButton>

#define Q_UINT32 uint32_t

class QDir;
class QMimeData;
class Schematic;

namespace misc {
  QString complexRect(double, double, int Precision=3);
  QString complexDeg (double, double, int Precision=3);
  QString complexRad (double, double, int Precision=3);
  QString StringNum  (double, char form='g', int Precision=3);
  void    str2num    (const QString&, double&, QString&, double&);
  QString num2str    (double, int Precision = -1, QString unit="");
  QColor ColorFromString(const QString& color);
  QString StringNiceNum(double);
  void    convert2Unicode(QString&);
  void    convert2ASCII(QString&);
  QString properName(const QString&);
  /// The font a symbol writes its pin names in: the application font, a
  /// little smaller, so a name fits between two pins.
  QFont pinFont();
  QString properAbsFileName(const QString&, Schematic* sch = nullptr);
  QString properFileName(const QString&);
  /// The files of a project: every regular file below root, at any depth,
  /// as paths relative to root ("models/bjt.va"). Hidden directories and
  /// symbolic links to directories are not entered. With nameFilters
  /// ("*.va") only matching names are returned. Sorted with the root's
  /// own files first, then directory by directory.
  QStringList projectFiles(const QDir& root, const QStringList& nameFilters = QStringList());
  /// The folder inside a project that takes its temporary files (netlists,
  /// simulator output, logs), listed by the Content panel as "Scratch".
  inline const char* const ScratchFolder = "Scratch";
  /// Where a simulation writes its temporary files: the open project's
  /// Scratch folder, otherwise (no project, headless) the simulator work
  /// directory from the settings.
  QString scratchDir();
  /// Where the simulations of one schematic write their temporary files:
  /// a folder of the schematic's name (its path relative to the project,
  /// without the extension - "amp" for amp.sch, "sub/amp" for sub/amp.sch)
  /// inside scratchDir(), so every simulated schematic has a folder of
  /// its own and a new run of the same schematic updates the same folder.
  /// "untitled" for a schematic without a name; a schematic outside the
  /// project gets its base name. Without a project it is scratchDir()
  /// itself, as headless runs expect.
  QString scratchDirFor(const QString& docName);
  /// Whether the file looks like text: readable and no NUL byte in its
  /// first 8 KiB. Decides what a dropped file of unknown type opens with.
  bool    isTextFile(const QString& path);
  /// The local files a drag carries (file:// URLs), empty for any other
  /// kind of drag.
  QStringList localFiles(const QMimeData* data);
  bool    VHDL_Time(QString&, const QString&);
  bool    VHDL_Delay(QString&, const QString&);
  bool    Verilog_Time(QString&, const QString&);
  bool    Verilog_Delay(QString&, const QString&);
  QString Verilog_Param(const QString);
  bool    checkVersion(QString&);
  void    reportError(const QString& text);
  /// Coordinates read from a file are limited to this range so that the
  /// arithmetic done on them afterwards (bounding boxes, margins, zoom,
  /// printing scale) cannot overflow int. 16.7 M units is far beyond any
  /// schematic; only damaged files get clamped.
  constexpr int MaxCoordinate = 1 << 24;
  inline int clampCoordinate(int v)
  { return v < -MaxCoordinate ? -MaxCoordinate : (v > MaxCoordinate ? MaxCoordinate : v); }
  /// s[i], or def when the string is shorter: for fields parsed out of a
  /// file, where at() would index past the end.
  inline QChar charAt(const QString& s, qsizetype i, QChar def = QChar())
  { return i < s.size() ? s.at(i) : def; }
  QString expandEnvVars(const QString&);

  inline const QColor getWidgetForegroundColor(const QWidget *q)
  { return q->palette().color(q->foregroundRole()); }

  inline const QColor getWidgetBackgroundColor(const QWidget *q)
  { return q->palette().color(q->backgroundRole()); }

  inline void setWidgetForegroundColor(QWidget *q, const QColor &c)
  { QPalette p = q->palette(); p.setColor(q->foregroundRole(), c); q->setPalette(p); }

  inline void setWidgetBackgroundColor(QWidget *q, const QColor &c)
  { QPalette p = q->palette(); p.setColor(q->backgroundRole(), c); q->setPalette(p); }

  inline void setPickerColor(QPushButton *p, const QColor &c)
    {
        // set color, to be able to get it later
        setWidgetBackgroundColor(p, c);

        // draw pixmap, background color is not being rendered on some platforms
        QPixmap pixmap(35, 10);
        pixmap.fill(c);
        QIcon icon(pixmap);
        p->setIcon(icon);
        p->setIconSize(pixmap.rect().size());
    }

  QStringList parseCmdArgs(const QString &program);
  QString getIconPath(const QString &file);
  bool isDarkTheme();
  QString getWindowTitle();
  QString wildcardToRegularExpression(const QString &wc_str, const bool enableEscaping);

  bool simulatorExists(const QString &exe_file);
  QString unwrapExePath(const QString &exe_file);

  void draw_richtext(QPainter* painter, int x, int y, const QString& text, QRectF* br = nullptr);
  void draw_resize_handle(QPainter* painter, const QPointF& center);

  void getSymbolPatternsList(QStringList &symbols);
  QString formatValue(const QString& input, int precision);

}

/*! handle the application version string
 *
 *  loosely modeled after the standard Semantic Versioning
 */
class VersionTriplet {
 public:
  VersionTriplet();
  VersionTriplet(const QString&);

  bool operator==(const VersionTriplet& v2);
  bool operator>(const VersionTriplet& v2);
  bool operator<(const VersionTriplet& v2);
  bool operator>=(const VersionTriplet& v2);
  bool operator<=(const VersionTriplet& v2);

  QString toString();

 private:
  int major, minor, patch;
};

#endif
