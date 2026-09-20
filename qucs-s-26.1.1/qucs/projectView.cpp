/*
 * ProjectView.cpp - implementation of project model
 *   the model manage the files in project directory
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

#include "projectView.h"
#include "schematic.h"
#include "misc.h"

#include <QString>
#include <QStringList>
#include <QDir>
#include <QStandardItemModel>
#include <QDebug>

ProjectView::ProjectView(QWidget *parent)
  : QTreeView(parent)
{
  m_projPath = QString();
  m_projPath = QString();
  m_valid = false;
  m_model = new QStandardItemModel(8, 2, this);

  refresh();

  this->setModel(m_model);
  this->setEditTriggers(QAbstractItemView::NoEditTriggers);
  this->setSelectionMode(QAbstractItemView::ExtendedSelection); // Allow multiple selection
}

ProjectView::~ProjectView()
{
  delete m_model;
}

int ProjectView::categoryOf(const QModelIndex& idx) const
{
  if (!idx.isValid()) return -1;
  return idx.parent().isValid() ? idx.parent().row() : idx.row();
}

void
ProjectView::setProjPath(const QString &path)
{
  // check if path exist
  m_valid = !path.isEmpty() && QDir(path).exists();

  if (m_valid) {
    m_projPath = path; // full path
    m_projName = QDir(m_projPath).dirName(); // only project directory name
    if (m_projName.endsWith("_prj")) {
      m_projName.chop(4);// remove "_prj" from name
    } else { // should not happen
      qWarning() << "ProjectView::setProjPath() : path does not end in '_prj' (" << m_projName << ")";
    }
  }
  refresh();
}

// refresh using projectPath
void
ProjectView::refresh()
{
  // Keep the categories the user has opened; the first fill shows Schematics.
  QList<int> expanded;
  for (int row = 0; row < m_model->rowCount(); ++row)
    if (isExpanded(m_model->index(row, 0)))
      expanded.append(row);
  if (m_model->rowCount() == 0)
    expanded.append(Schematics);

  m_model->clear();

  QStringList header;
  header << tr("Content of %1").arg(m_projName) << tr("Note");
  m_model->setHorizontalHeaderLabels(header);

  appendRow(m_model->invisibleRootItem(), tr("Datasets"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Data Displays"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Verilog"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Verilog-A"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Osdi"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("VHDL"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Octave"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Schematics"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Symbols"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("SPICE"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Others"), QString(""));

  for (int row : expanded)
    setExpanded(m_model->index(row, 0), true);

  if (!m_valid) {
    return;
  }

  // put all files into "Content"-ListView: those of the project directory
  // and of any subdirectory, named relative to the project
  const QDir workPath(m_projPath);
  for (const QString& fileName : misc::projectFiles(workPath)) {
    const QFileInfo info(workPath.filePath(fileName));
    const QString extName = info.suffix().toLower();
    const QString fullExtName = info.completeSuffix().toLower();

    QList<QStandardItem *> columnData;
    columnData.append(new QStandardItem(fileName));

    if(extName == "dat" || fullExtName == "dat.ngspice" ||
       fullExtName == "dat.xyce" || fullExtName == "dat.spopus" ) {
      appendChild(Datasets, columnData);
    }
    else if(extName == "dpl") {
      appendChild(DataDisplays, columnData);
    }
    else if(extName == "v") {
      appendChild(Verilog, columnData);
    }
    else if(extName == "va") {
      appendChild(VerilogA, columnData);
    }
    else if(extName == "osdi") {
      appendChild(Osdi, columnData);
    }
    else if((extName == "vhdl") || (extName == "vhd")) {
      appendChild(VHDL, columnData);
    }
    else if((extName == "m") || (extName == "oct")) {
      appendChild(Octave, columnData);
    }
    else if(extName == "sch") {
      // test if it's a valid schematic file
      int n = Schematic::testFile(info.filePath());
      if(n >= 0) {
        if(n > 0) { // is a subcircuit
          columnData.append(new QStandardItem(QString::number(n)+tr("-port")));
        }
        appendChild(Schematics, columnData);
      } else {
        qDeleteAll(columnData);
      }
    } else if (extName == "sym") {
        appendChild(Symbols,columnData);
    } else if ((extName == "cir") || (extName=="ckt") ||
             (extName=="sp")) {
        appendChild(SPICE,columnData);
    }
    else {
      appendChild(Others, columnData);
    }
  }

  resizeColumnToContents(0);
}

QStringList ProjectView::exportSchematic()
{
  QStringList list;
  QStandardItem *item = m_model->item(Schematics, 0);
  for (int i = 0; i < item->rowCount(); ++i) {
    if (item->child(i,1)) {
      list.append(item->child(i,0)->text());
    }
  }
  return list;
}
