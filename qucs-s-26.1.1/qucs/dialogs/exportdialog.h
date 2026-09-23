/***************************************************************************
                               exportdialog.h
                              ------------------
    begin                : Thu Nov 28 2013
    copyright            : (C) 2013 by Vadim Kuznetzov
    email                : <ra3xdh@gmail.com>
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#ifndef EXPORTDIALOG_H
#define EXPORTDIALOG_H

#include "graphicsexport.h"

#include <QDialog>
#include <QSize>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

// File > Export as image: the file, its format (PNG, JPEG, BMP, TIFF,
// WebP, SVG, PDF, EPS, PDF + LaTeX) and how it is drawn; or the drawing on
// the clipboard. The choices are kept for the next time.
class ExportDialog : public QDialog
{
    Q_OBJECT
public:
    /// What exec() returns after "Copy to Clipboard".
    enum { Copied = 2 };

    /// \a size and \a selectionSize: what is drawn, in units of the
    /// schematic, of everything and of the selection (empty: nothing is
    /// selected).
    ExportDialog(QSize size, QSize selectionSize, const QString& fileName, QWidget* parent = nullptr);

    /// A diagram's export: its selection, only.
    void setDiagram();

    /// The file, with the suffix of the format.
    QString fileName() const;
    qucs_s::graphicsexport::Format format() const;
    qucs_s::graphicsexport::Options options() const;

    // The controls, for the tests.
    QLineEdit* fileEdit() const { return m_file; }
    QComboBox* formatBox() const { return m_format; }
    QCheckBox* selectionBox() const { return m_selection; }
    QDoubleSpinBox* scaleBox() const { return m_scale; }
    QSpinBox* dpiBox() const { return m_dpi; }
    QSpinBox* widthBox() const { return m_width; }
    QSpinBox* heightBox() const { return m_height; }
    QComboBox* coloursBox() const { return m_colours; }
    QCheckBox* transparentBox() const { return m_transparent; }
    QSpinBox* qualityBox() const { return m_quality; }
    QCheckBox* outlinesBox() const { return m_outlines; }
    QLabel* noteLabel() const { return m_note; }
    QPushButton* copyButton() const { return m_copy; }

public slots:
    void accept() override;

private slots:
    void browse();
    void fileEdited(const QString& text);
    void formatChosen();
    void selectionToggled();
    void outlinesToggled(bool on);

private:
    QSize drawnSize() const;
    void setScale(double scale, const QObject* from);
    void updateControls();
    void load();
    void store() const;

    QSize m_size;
    QSize m_selectionSize;
    double m_scaleValue = 1.0;        // the spin box shows it rounded
    bool m_outlinesSvg = true;        // what was chosen for each format
    bool m_outlinesPdf = false;

    QFormLayout* m_form;
    QLineEdit* m_file;
    QComboBox* m_format;
    QCheckBox* m_selection;
    QDoubleSpinBox* m_scale;
    QSpinBox* m_dpi;
    QSpinBox* m_width;
    QSpinBox* m_height;
    QLabel* m_physical;
    QComboBox* m_colours;
    QCheckBox* m_transparent;
    QSpinBox* m_quality;
    QCheckBox* m_outlines;
    QLabel* m_note;
    QPushButton* m_copy;
    QWidget* m_scaleRow;
    QWidget* m_sizeRow;
};

#endif // EXPORTDIALOG_H
