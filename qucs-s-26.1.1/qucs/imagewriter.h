/*
 * imagewriter.h - declaraction of writer to image
 *
 * Copyright (C) 2014, Yodalee, lc85301@gmail.com
 *
 * This file is part of Qucs
 *
 * Qucs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Qucs.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef IMAGEWRITER_H_
#define IMAGEWRITER_H_ value

#include <QString>

class QWidget;

// File > Export as image: the export dialog for a document, then the file
// written (graphicsexport) or the drawing on the clipboard.
class ImageWriter
{
public:
  ImageWriter (QString lastfile);
  virtual ~ImageWriter ();
  // 0 when a file was written or the clipboard filled, -1 otherwise.
  int print(QWidget *);

  QString getLastSavedFile();
  // The drawing went to the clipboard rather than to a file.
  bool copied() const { return m_copied; }

  void setDiagram(bool diagram) { onlyDiagram = diagram; };
private:
  bool onlyDiagram;
  bool m_copied = false;
  QString lastExportFilename;
};

#endif
