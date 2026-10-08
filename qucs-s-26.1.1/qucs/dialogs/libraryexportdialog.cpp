/***************************************************************************
                           libraryexportdialog.cpp
                          -------------------------
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "libraryexportdialog.h"
#include "main.h"
#include "schematic.h"
#include "components/component.h"
#include "spicecomponents/sp_libraryexport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

LibraryOptions::LibraryOptions(QWidget* parent) : QWidget(parent)
{
  QVBoxLayout* all = new QVBoxLayout(this);
  all->setContentsMargins(0, 0, 0, 0);
  m_heldBy = new QLabel(this);
  m_heldBy->setWordWrap(true);
  m_heldBy->setObjectName(QStringLiteral("libraryHeldBy"));
  m_heldBy->hide();
  all->addWidget(m_heldBy);

  alwaysLoadOSDI = new QCheckBox(tr("Always load its Verilog-A (OSDI) in the project's circuits"), this);
  alwaysLoadOSDI->setObjectName(QStringLiteral("alwaysLoadOSDI"));
  all->addWidget(alwaysLoadOSDI);
  QLabel* libraryNote = new QLabel(
      tr("Made into a library part, this subcircuit is marked in the library: every circuit of a "
         "project that has the library - its own library, or one whose part a schematic of it "
         "places - loads the Verilog-A models (OSDI) of the devices in this subcircuit, whether "
         "the part is placed or not. For a model a circuit uses where Qucs-S cannot see it.\n"
         "Off: a circuit loads only the models its parts use."), this);
  libraryNote->setWordWrap(true);
  all->addWidget(libraryNote);
  all->addSpacing(8);

  QLabel* cardsLabel = new QLabel(tr("SPICE .model cards of its own:"), this);
  all->addWidget(cardsLabel);
  modelCards = new QPlainTextEdit(this);
  modelCards->setObjectName(QStringLiteral("modelCards"));
  modelCards->setAccessibleName(tr("SPICE .model cards"));   // (get_settings' key is its label's: Library/SPICE .model cards of its own)
  modelCards->setPlaceholderText(QStringLiteral(".model resmod va_res r=1k"));
  modelCards->setLineWrapMode(QPlainTextEdit::NoWrap);
  modelCards->setTabChangesFocus(true);
  cardsLabel->setBuddy(modelCards);
  all->addWidget(modelCards, 1);
  QLabel* cardsNote = new QLabel(
      tr("Written at the top level of the SPICE netlist - not inside this subcircuit - of a circuit that "
         "places it, as a subcircuit or as a library part made of it (and of its own netlist): the global "
         ".model card of a Verilog-A device in it (an N device whose model is resmod: .model resmod va_res "
         "r=1k). A .MODEL block placed in it is the subcircuit's own instead. One card a line, + lines going "
         "on with one, * comments; ngspice and Xyce read them, Qucsator does not."), this);
  cardsNote->setWordWrap(true);
  all->addWidget(cardsNote);
  alwaysModelCards = new QCheckBox(tr("Always write its .model cards in the project's circuits"), this);
  alwaysModelCards->setObjectName(QStringLiteral("alwaysModelCards"));
  alwaysModelCards->setToolTip(
      tr("Made into a library part, its .model cards above are marked in the library: every ngspice circuit of "
         "a project that has the library - its own library, or one whose part a schematic of it places - has "
         "them at the top of its netlist, whether the part is placed or not, and loads the Verilog-A models "
         "(OSDI) of the library part they name, as the mark above does. For a model a circuit uses where "
         "Qucs-S cannot see it.\nOff: a circuit has the cards of the parts it places."));
  all->addWidget(alwaysModelCards);
  all->addSpacing(8);

  // Its own ground pin, or the application's choice.
  QGridLayout* pin = new QGridLayout;
  QLabel* pinLabel = new QLabel(tr("Ground pin (gnd) in its exported subcircuit:"), this);
  pin->addWidget(pinLabel, 0, 0);
  groundPin = new QComboBox(this);
  groundPin->setObjectName(QStringLiteral("groundPin"));
  groundPin->addItem(tr("As Application Settings say (now: %1)")
                         .arg(QucsSettings.LibraryGroundPin ? tr("with gnd") : tr("without")),
                     int(LibrarySettings::Default));
  groundPin->addItem(tr("With a first pin gnd"), int(LibrarySettings::With));
  groundPin->addItem(tr("Without a gnd pin"), int(LibrarySettings::Without));
  groundPin->setToolTip(
      tr("Project > Create Library gives this subcircuit's SPICE model (.SUBCKT) a first pin, gnd, that every part "
         "made of it ties to the circuit's ground, when Application Settings > Settings > Ground pin (gnd) in "
         "exported subcircuits asks for it. Here the subcircuit chooses for itself instead: with gnd (for a Qucs-S "
         "26.1.5 or earlier, which always ties one) or without it (another program reads its pins as they are). "
         "A part tells from its library's .SUBCKT which it has."));
  pinLabel->setBuddy(groundPin);
  pin->addWidget(groundPin, 0, 1);
  pin->setColumnStretch(1, 1);
  all->addLayout(pin);
}

void LibraryOptions::setSettings(const LibrarySettings& s)
{
  alwaysLoadOSDI->setChecked(s.alwaysLoadOSDI);
  modelCards->setPlainText(s.modelCards);
  alwaysModelCards->setChecked(s.alwaysModelCards);
  groundPin->setCurrentIndex(std::max(0, groundPin->findData(int(s.groundPin))));
}

LibrarySettings LibraryOptions::settings() const
{
  LibrarySettings s;
  s.alwaysLoadOSDI = alwaysLoadOSDI->isChecked();
  s.modelCards = modelCards->toPlainText().trimmed();
  s.alwaysModelCards = alwaysModelCards->isChecked();
  s.groundPin = static_cast<LibrarySettings::GroundPin>(groundPin->currentData().toInt());
  return s;
}

QStringList LibraryOptions::rejectedCards() const
{
  QStringList rejected;
  Schematic::modelCardsOf(modelCards->toPlainText(), &rejected);
  return rejected;
}

void LibraryOptions::setHeldBy(const QString& text)
{
  m_heldBy->setText(text);
  m_heldBy->setVisible(!text.isEmpty());
}

QSize LibraryOptions::minimumSizeHint() const
{
  // A tab's page is given its minimum size as if its wrapped labels were
  // one line each (a tab widget asks no height for its width): the height
  // they need at the narrowest it is laid out.
  QSize size = QWidget::minimumSizeHint();
  size.setWidth(std::max(size.width(), 480));
  if (layout() != nullptr && layout()->hasHeightForWidth())
    size.setHeight(std::max(size.height(), layout()->totalHeightForWidth(size.width())));
  return size;
}

QSize LibraryOptions::sizeHint() const
{
  return QWidget::sizeHint().expandedTo(minimumSizeHint());
}

// -----------------------------------------------------------
LibraryExportDialog::LibraryExportDialog(Component* part, Schematic* doc)
    : QDialog(doc), m_part(part), m_doc(doc)
{
  setWindowTitle(tr("Library Export"));
  QVBoxLayout* all = new QVBoxLayout(this);

  QLabel* intro = new QLabel(
      tr("Project > Create Library makes this subcircuit a library part with these settings. They are its Document "
         "Settings > Library too, which show and change them while this part is placed."), this);
  intro->setWordWrap(true);
  all->addWidget(intro);

  QGridLayout* top = new QGridLayout;
  QLabel* nameLabel = new QLabel(tr("Name:"), this);
  top->addWidget(nameLabel, 0, 0);
  m_name = new QLineEdit(part->Name, this);
  m_name->setObjectName(QStringLiteral("name"));
  m_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z][A-Za-z0-9_]*")), m_name));
  nameLabel->setBuddy(m_name);
  top->addWidget(m_name, 0, 1);
  all->addLayout(top);
  m_shown = new QCheckBox(tr("Show its settings on the schematic"), this);
  m_shown->setObjectName(QStringLiteral("shown"));
  bool shown = false;
  for (const Property* p : part->Props) shown = shown || p->display;
  m_shown->setChecked(shown);
  all->addWidget(m_shown);

  m_options = new LibraryOptions(this);
  m_options->setSettings(LibraryExport::settingsOf(part));
  if (part->isActive != COMP_IS_ACTIVE)
    m_options->setHeldBy(tr("It is off: Create Library uses none of these settings until it is on again."));
  all->addWidget(m_options, 1);

  QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &LibraryExportDialog::slotOK);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  all->addWidget(buttons);
  resize(520, 560);
}

void LibraryExportDialog::slotOK()
{
  // Only cards go into a netlist: a line of another kind is said, and
  // nothing is applied (as Document Settings > Library does).
  if (const QStringList rejected = m_options->rejectedCards(); !rejected.isEmpty()) {
    QMessageBox::warning(this, windowTitle(),
                         tr("These lines are no .model card (nor a + line going on with one, nor a * comment), and only "
                            "cards go into the netlist:\n\n%1\n\nNothing was applied.").arg(rejected.join(QLatin1Char('\n'))));
    return;
  }
  const QString name = m_name->text().trimmed();
  if (name != m_part->Name) {
    if (name.isEmpty() || m_doc->getComponentByName(name) != nullptr) {
      QMessageBox::warning(this, windowTitle(),
                           name.isEmpty() ? tr("It needs a name.") : tr("There is a component named %1 already.").arg(name));
      return;
    }
    m_part->Name = name;
  }
  LibraryExport::setSettings(m_part, m_options->settings());
  for (Property* p : m_part->Props) p->display = m_shown->isChecked();
  m_doc->recreateComponent(m_part);   // (its texts)
  accept();
}
