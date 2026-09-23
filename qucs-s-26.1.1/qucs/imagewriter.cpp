/*
 * imagewriter.cpp - implementation of writer to image
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

#include "schematic.h"
#include "imagewriter.h"
#include "graphicsexport.h"
#include "dialogs/exportdialog.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>

using namespace qucs_s::graphicsexport;

ImageWriter::ImageWriter(QString lastfile)
{
  onlyDiagram = false;
  lastExportFilename = lastfile;
}

ImageWriter::~ImageWriter()
{
}

QString ImageWriter::getLastSavedFile()
{
    return lastExportFilename;
}

namespace {

// The file offered: named after the document, in the folder of the last
// export (the document's own before the first), in its format.
QString offeredFile(Schematic* sch, const QString& last)
{
  const QFileInfo doc(sch->getDocName());
  if (doc.fileName().isEmpty())
    return last;
  const QFileInfo previous(last);
  const bool exportedBefore = !last.isEmpty()
      && previous.absoluteFilePath() != QDir::home().absoluteFilePath(QStringLiteral("export.png"));
  const QDir folder = exportedBefore ? previous.absoluteDir() : doc.absoluteDir();
  const std::optional<Format> format = formatOf(last);
  return folder.absoluteFilePath(doc.completeBaseName() + QLatin1Char('.')
                                 + suffix(format.value_or(Format::Png)));
}

} // namespace

int ImageWriter::print(QWidget *doc)
{
  auto *sch = dynamic_cast<Schematic*>(doc);
  if (sch == nullptr)
    return -1;

  ExportDialog dlg(area(sch, false).size(), area(sch, true).size(),
                   offeredFile(sch, lastExportFilename), doc->window());
  if (onlyDiagram)
    dlg.setDiagram();

  const int result = dlg.exec();
  if (result == QDialog::Rejected)
    return -1;

  const Options options = dlg.options();
  if (result == ExportDialog::Copied) {
    QGuiApplication::clipboard()->setMimeData(mimeData(sch, options));
    m_copied = true;
    return 0;
  }

  const QString filename = dlg.fileName();
  const Format format = dlg.format();
  QStringList existing;
  for (const QString& file : format == Format::PdfTex ? QStringList{filename, pdfOf(filename)}
                                                      : QStringList{filename})
    if (QFile::exists(file))
      existing << QDir::toNativeSeparators(file);
  if (!existing.isEmpty()) {
    const int r = QMessageBox::question(doc->window(), QObject::tr("Overwrite"),
                                        QObject::tr("%1 already exists.\nOverwrite?")
                                            .arg(existing.join(QObject::tr(" and "))),
                                        QMessageBox::Yes | QMessageBox::No);
    if (r != QMessageBox::Yes)
      return -1;
  }
  lastExportFilename = filename;

  QString error;
  if (!write(sch, filename, format, options, &error)) {
    QMessageBox::critical(doc->window(), QObject::tr("Export to image"), error);
    return -1;
  }
  return 0;
}
