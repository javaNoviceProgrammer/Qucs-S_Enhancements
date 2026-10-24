/*
 * rf25panel.cpp - the RF 2.5D panel: the model's tree and the settings of
 *                 the node chosen
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "rf25panel.h"

#include "main.h"
#include "misc.h"
#include "multiphysicspanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

using namespace qucs_s::rf25;
using qucs_s::fem::PropertyDef;

namespace {

constexpr int TagRole = Qt::UserRole + 1;

/// A node's icon: the one drawer of them (multiphysicspanel.cpp), which
/// has this tree's glyph names too.
QIcon glyph(const QString& icon)
{
    return MultiphysicsPanel::nodeGlyph(icon);
}

/// A table cell as text, whatever JSON holds it.
QString cellText(const QJsonValue& v)
{
    if (v.isDouble()) return qucs_s::fem::formatNumber(v.toDouble(), 15);
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

} // namespace

// ------------------------------------------------------------------ The panel

RF25Panel::RF25Panel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("RF25Panel"));
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    a_pages = new QStackedWidget(this);
    all->addWidget(a_pages);

    // No model in front: make one, open one.
    a_start = new QWidget(a_pages);
    {
        auto* v = new QVBoxLayout(a_start);
        v->setContentsMargins(10, 10, 10, 10);
        v->addWidget(new QLabel(tr("<b>RF 2.5D</b>"), a_start));
        auto* about = new QLabel(tr("A planar structure solved as a stack and a layout: the layers in z - the substrate, "
                                    "the dielectrics, the metals, the vias between them, the ground and the cover -, the "
                                    "shapes in x and y from a GDSII or OASIS file with a map that says which layer of the "
                                    "file is which layer of the stack, ports on the metals, a mesh and a frequency sweep. "
                                    "This build holds the model, its stack and its layout; the solver comes after it. "
                                    "Open a model (.qrf25) to see its tree here."),
                                 a_start);
        about->setWordWrap(true);
        // (Wrapped to the dock's width: its length is not to widen the dock -
        // and narrow the window's documents - nor to make it taller.)
        about->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        about->setMinimumHeight(0);
        v->addWidget(about);
        a_newButton = new QPushButton(tr("New Model"), a_start);
        a_newButton->setObjectName(QStringLiteral("rfNewModel"));
        connect(a_newButton, &QPushButton::clicked, this, &RF25Panel::newModelRequested);
        auto* open = new QPushButton(tr("Open Model…"), a_start);
        open->setObjectName(QStringLiteral("rfOpenModel"));
        connect(open, &QPushButton::clicked, this, [this] { emit openModelRequested(QString()); });
        auto* buttons = new QHBoxLayout;
        buttons->addWidget(a_newButton);
        buttons->addWidget(open);
        buttons->addStretch(1);
        v->addLayout(buttons);
        v->addWidget(new QLabel(tr("Recent models"), a_start));
        a_recent = new QListWidget(a_start);
        a_recent->setObjectName(QStringLiteral("rfRecent"));
        v->addWidget(a_recent, 1);
        connect(a_recent, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
            emit openModelRequested(item->data(Qt::UserRole).toString());
        });
    }
    a_pages->addWidget(a_start);

    a_splitter = new QSplitter(Qt::Vertical, a_pages);
    a_splitter->setObjectName(QStringLiteral("rfPanelSplitter"));
    a_tree = new QTreeWidget(a_splitter);
    a_tree->setObjectName(QStringLiteral("rfTree"));
    a_tree->setColumnCount(2);
    a_tree->setHeaderHidden(true);
    a_tree->header()->setStretchLastSection(false);
    a_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    a_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    a_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    a_tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    a_tree->installEventFilter(this);
    connect(a_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (a_rebuilding || item == nullptr || !a_doc) return;
        a_doc->focusNode(item->data(0, TagRole).toString());
        showSettings();
    });
    connect(a_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = a_tree->itemAt(pos);
        if (item == nullptr) return;
        a_tree->setCurrentItem(item);
        if (QMenu* menu = menuFor(item->data(0, TagRole).toString())) {
            menu->exec(a_tree->viewport()->mapToGlobal(pos));
            menu->deleteLater();
        }
    });
    connect(a_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (a_rebuilding || column != 0 || !a_doc) return;
        const QString tag = item->data(0, TagRole).toString();
        const Node* n = a_doc->model().find(tag);
        const NodeKind* k = n != nullptr ? kindOf(*n) : nullptr;
        // A part of the tree itself is not renamed: the root's row is the
        // model's file name, written there whenever the tree is filled, and
        // a rename would have put that name into the model.
        if (k != nullptr && k->fixed) return;
        if (n != nullptr && item->text(0) != nameOf(*n)) renameNode(tag, item->text(0));
    });
    a_scroll = new QScrollArea(a_splitter);
    a_scroll->setWidgetResizable(true);
    a_scroll->setFrameShape(QFrame::NoFrame);
    a_form = new QWidget;
    a_form->setObjectName(QStringLiteral("rfSettings"));
    a_scroll->setWidget(a_form);
    a_splitter->addWidget(a_tree);
    a_splitter->addWidget(a_scroll);
    a_splitter->setStretchFactor(0, 1);
    a_splitter->setStretchFactor(1, 1);
    a_splitter->setSizes({500, 420});
    a_pages->addWidget(a_splitter);
    fillStart();
    a_pages->setCurrentWidget(a_start);
}

void RF25Panel::fillStart()
{
    a_recent->clear();
    for (const QString& r : QucsSettings.RecentDocs)
        if (r.endsWith(QLatin1String(".qrf25"), Qt::CaseInsensitive) && QFileInfo::exists(r)) {
            auto* item = new QListWidgetItem(glyph(QStringLiteral("model")), QFileInfo(r).fileName(), a_recent);
            item->setData(Qt::UserRole, r);
            item->setToolTip(r);
        }
}

void RF25Panel::setDocument(RF25Doc* doc)
{
    if (a_doc == doc) return;
    for (const QMetaObject::Connection& c : a_connections) disconnect(c);
    a_connections.clear();
    a_doc = doc;
    if (doc == nullptr) {
        fillStart();
        a_pages->setCurrentWidget(a_start);
        return;
    }
    a_connections << connect(doc, &RF25Doc::modelChanged, this, [this] {
        rebuildTree();
        scheduleSettings();
    });
    a_connections << connect(doc, &RF25Doc::stateChanged, this, [this] {
        updateStatusColumn();
        scheduleSettings();
    });
    // The shapes and the mesh made again: what the Processing, the Mesh
    // and the Layout say they did is of that run.
    a_connections << connect(doc, &RF25Doc::preparedChanged, this, [this] { scheduleSettings(); });
    a_connections << connect(doc, &RF25Doc::solvedChanged, this, [this] { scheduleSettings(); });
    a_connections << connect(doc, &RF25Doc::focusChanged, this, [this](const QString& tag) {
        if (currentTag() != tag) selectNode(tag);
    });
    // Closed: let go without comparing it (a QPointer's comparison casts what
    // is by now only a QWidget, which UBSan says is undefined).
    a_connections << connect(doc, &QObject::destroyed, this, [this] {
        for (const QMetaObject::Connection& c : a_connections) disconnect(c);
        a_connections.clear();
        a_doc.clear();
        fillStart();
        a_pages->setCurrentWidget(a_start);
    });
    a_pages->setCurrentWidget(a_splitter);
    a_shownTag.clear();
    rebuildTree();
    const Node* stack = doc->model().root.child(QStringLiteral("rf_stack"));
    selectNode(doc->focusedNode().isEmpty() ? (stack != nullptr ? stack->tag : doc->model().root.tag) : doc->focusedNode());
}

// ------------------------------------------------------------------ The tree

QTreeWidgetItem* RF25Panel::itemOf(const QString& tag) const
{
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        if ((*it)->data(0, TagRole).toString() == tag) return *it;
        ++it;
    }
    return nullptr;
}

QString RF25Panel::currentTag() const
{
    return a_tree->currentItem() != nullptr ? a_tree->currentItem()->data(0, TagRole).toString() : QString();
}

void RF25Panel::selectNode(const QString& tag)
{
    if (QTreeWidgetItem* item = itemOf(tag)) {
        a_tree->setCurrentItem(item);
        a_tree->scrollToItem(item);
    }
}

void RF25Panel::addItems(QTreeWidgetItem* parent, const Node& node)
{
    for (const Node& c : node.children) {
        auto* item = new QTreeWidgetItem(parent);
        const NodeKind* k = kindOf(c);
        item->setText(0, nameOf(c));
        item->setIcon(0, glyph(k != nullptr ? k->icon : QString()));
        item->setData(0, TagRole, c.tag);
        Qt::ItemFlags flags = item->flags();
        if (k != nullptr && !k->fixed) flags |= Qt::ItemIsEditable;
        item->setFlags(flags);
        if (k != nullptr) item->setToolTip(0, k->title);
        if (!c.enabled) {
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        }
        addItems(item, c);
    }
}

void RF25Panel::rebuildTree()
{
    if (!a_doc) return;
    a_rebuilding = true;
    const QString current = currentTag();
    QSet<QString> collapsed;
    {
        QTreeWidgetItemIterator it(a_tree);
        while (*it) {
            if (!(*it)->isExpanded() && (*it)->childCount() > 0) collapsed.insert((*it)->data(0, TagRole).toString());
            ++it;
        }
    }
    const bool first = a_tree->topLevelItemCount() == 0;
    a_tree->clear();
    const Model& m = a_doc->model();
    auto* root = new QTreeWidgetItem(a_tree);
    root->setText(0, a_doc->getDocName().isEmpty() ? tr("untitled.qrf25") : QFileInfo(a_doc->getDocName()).fileName());
    root->setIcon(0, glyph(QStringLiteral("model")));
    root->setData(0, TagRole, m.root.tag);
    addItems(root, m.root);
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        (*it)->setExpanded(first || !collapsed.contains((*it)->data(0, TagRole).toString()));
        ++it;
    }
    if (QTreeWidgetItem* item = itemOf(current)) a_tree->setCurrentItem(item);
    a_rebuilding = false;
    updateStatusColumn();
}

void RF25Panel::updateStatusColumn()
{
    if (!a_doc) return;
    // The root: the model's file, as it is named now (saved since, renamed).
    if (QTreeWidgetItem* root = a_tree->topLevelItem(0))
        root->setText(0, a_doc->getDocName().isEmpty() ? tr("untitled.qrf25") : QFileInfo(a_doc->getDocName()).fileName());
    const QColor grey = QApplication::palette().color(QPalette::Disabled, QPalette::Text);
    const QColor red(198, 40, 40);
    const Stack stack = a_doc->model().stack();
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        ++it;
        const QString tag = item->data(0, TagRole).toString();
        const Node* n = a_doc->model().find(tag);
        if (n == nullptr) continue;
        QString status;
        QColor colour = grey;
        if (n->type == QLatin1String("rf_model")) {
            const int missing = int(a_doc->problems().size());
            if (missing > 0) {
                status = tr("%1 missing").arg(missing);
                colour = red;
            } else {
                status = tr("whole");
            }
        } else if (n->type == QLatin1String("rf_stack")) {
            status = tr("%1 layer(s)").arg(stack.layers.size());
            if (!stack.ok()) colour = red;
        } else if (n->type == QLatin1String("rf_layout")) {
            if (a_doc->isBusy()) status = tr("being read");
            else if (!a_doc->layoutProblem().isEmpty()) {
                status = tr("not read");
                colour = red;
                item->setToolTip(0, a_doc->layoutProblem());
            } else if (a_doc->layout()) {
                status = tr("%1 shape(s)").arg(QLocale().toString(qlonglong(a_doc->layout()->shapeCount)));
            } else {
                status = tr("no file");
            }
        } else if (n->type == QLatin1String("rf_ports")) {
            status = tr("%1 port(s)").arg(n->children.size());
        } else if (const Layer* layer = stack.named(nameOf(*n)); layer != nullptr && layer->tag == tag) {
            // A layer of the stack: how thick it is, and where it sits.
            if (layer->z1 > layer->z0)
                status = qucs_s::fem::formatQuantity(layer->z1 - layer->z0, qucs_s::fem::Dim::length());
            else if (layer->kind == Layer::Via)
                status = tr("%1 to %2").arg(layer->from, layer->to);
        }
        item->setText(1, status);
        item->setForeground(1, colour);
    }
}

QMenu* RF25Panel::menuFor(const QString& tag)
{
    if (!a_doc) return nullptr;
    const Model& m = a_doc->model();
    const Node* n = m.find(tag);
    if (n == nullptr) return nullptr;
    const NodeKind* k = kindOf(*n);
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("rfNodeMenu"));
    // What may be added under it.
    if (k != nullptr && !k->children.isEmpty()) {
        QMenu* add = n->type == QLatin1String("rf_model") ? menu : menu->addMenu(tr("Add"));
        for (const QString& type : k->children) {
            const NodeKind* ck = nodeKind(type);
            if (ck == nullptr || ck->fixed) continue;
            if (ck->unique && n->child(type) != nullptr) continue;
            add->addAction(glyph(ck->icon), n->type == QLatin1String("rf_model") ? tr("Add %1").arg(ck->title) : ck->title, this,
                           [this, tag, type] { addNode(tag, type); });
        }
        if (add != menu && add->actions().isEmpty()) {
            menu->removeAction(add->menuAction());
            add->deleteLater();
        }
        menu->addSeparator();
    }
    if (n->type == QLatin1String("rf_layout"))
        menu->addAction(tr("Read the Layout Again"), a_doc.data(), &RF25Doc::refreshLayout);
    if (!menu->actions().isEmpty() && !menu->actions().last()->isSeparator()) menu->addSeparator();
    const bool editable = k != nullptr && !k->fixed;
    if (editable) {
        menu->addAction(tr("Rename"), this, [this, tag] {
            if (QTreeWidgetItem* item = itemOf(tag)) a_tree->editItem(item, 0);
        });
        menu->addAction(tr("Duplicate"), this, [this, tag] { duplicateNode(tag); });
        menu->addAction(n->enabled ? tr("Disable") : tr("Enable"), this, [this, tag, on = !n->enabled] { setNodeEnabled(tag, on); });
        menu->addAction(tr("Move Up"), this, [this, tag] { moveNode(tag, -1); });
        menu->addAction(tr("Move Down"), this, [this, tag] { moveNode(tag, 1); });
        QAction* del = menu->addAction(tr("Delete"), this, [this, tag] { removeNode(tag); });
        del->setShortcut(QKeySequence::Delete);
    }
    return menu;
}

QString RF25Panel::addNode(const QString& parentTag, const QString& type)
{
    if (!a_doc) return {};
    Model m = a_doc->model();
    Node node = m.make(type);
    const NodeKind* k = kindOf(node);
    if (k == nullptr) return {};
    QString why;
    Node* added = m.add(parentTag, node, -1, &why);
    if (added == nullptr) {
        misc::reportError(why);
        return {};
    }
    const QString tag = added->tag;
    a_doc->setModel(m, tr("Add %1").arg(k->title));
    selectNode(tag);
    return tag;
}

bool RF25Panel::removeNode(const QString& tag)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    const Node* n = m.find(tag);
    if (n == nullptr) return false;
    const QString name = nameOf(*n);
    QString why;
    if (!m.remove(tag, &why)) {
        misc::reportError(why);
        return false;
    }
    a_doc->setModel(m, tr("Delete %1").arg(name));
    return true;
}

bool RF25Panel::duplicateNode(const QString& tag)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    int index = -1;
    Node* parent = m.parentOf(tag, &index);
    if (parent == nullptr) return false;
    Node copy = parent->children[std::size_t(index)];
    const NodeKind* k = kindOf(copy);
    if (k == nullptr || k->fixed) return false;
    // New tags throughout, and a name of its own: the layer map and the
    // ports name a layer, and two of a name would be one layer to them.
    std::function<void(Node&)> clearTags = [&](Node& n) {
        n.tag.clear();
        for (Node& c : n.children) clearTags(c);
    };
    clearTags(copy);
    const QString was = copy.label;
    if (!was.isEmpty()) {
        QStringList taken;
        for (const Node& c : parent->children) taken << c.label;
        QString name = was + tr(" (copy)");
        for (int i = 2; taken.contains(name); ++i) name = was + tr(" (copy %1)").arg(i);
        copy.label = name;
    }
    Node* added = m.add(parent->tag, copy, index + 1);
    if (added == nullptr) return false;
    const QString newTag = added->tag;
    a_doc->setModel(m, tr("Duplicate %1").arg(was.isEmpty() ? k->title : was));
    selectNode(newTag);
    return true;
}

bool RF25Panel::moveNode(const QString& tag, int by)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    int index = -1;
    Node* parent = m.parentOf(tag, &index);
    if (parent == nullptr) return false;
    const int to = index + by;
    if (to < 0 || to >= int(parent->children.size())) return false;
    // Not past a fixed node (the layout's map and its processing stay put).
    const NodeKind* other = kindOf(parent->children[std::size_t(to)]);
    if (other != nullptr && other->fixed) return false;
    std::swap(parent->children[std::size_t(index)], parent->children[std::size_t(to)]);
    a_doc->setModel(m, by < 0 ? tr("Move Up") : tr("Move Down"));
    selectNode(tag);
    return true;
}

bool RF25Panel::setNodeEnabled(const QString& tag, bool enabled)
{
    if (!a_doc) return false;
    return a_doc->editNode(tag, [enabled](Node& n) { n.enabled = enabled; }, enabled ? tr("Enable") : tr("Disable"));
}

bool RF25Panel::renameNode(const QString& tag, const QString& label)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    Node* n = m.find(tag);
    if (n == nullptr) return false;
    const NodeKind* k = kindOf(*n);
    const QString name = label.trimmed();
    const QString old = n->label;
    if (name == old) return false;
    // A layer of the stack is named by the layer map, the vias and the
    // ports: renamed here, it is renamed there too, or they would point at
    // a layer that is gone.
    const bool layer = k != nullptr && k->group == QLatin1String("stack");
    if (layer && !name.isEmpty())
        for (const Node& c : m.stackNode().children)
            if (c.tag != tag && c.label == name) {
                misc::reportError(tr("%1 is another layer's name: the layer map, the vias and the ports name a layer, "
                                     "so each needs a name of its own.")
                                      .arg(name));
                rebuildTree();
                return false;
            }
    n->label = name == (k != nullptr ? k->title : QString()) ? QString() : name;
    if (layer) m.renameLayer(old, n->label);
    a_doc->setModel(m, tr("Rename"));
    return true;
}

bool RF25Panel::eventFilter(QObject* watched, QEvent* event)
{
    // (An item's name being edited: its editor has the keys, not the tree.)
    if (watched == a_tree && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const QString tag = currentTag();
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && !tag.isEmpty()) {
            removeNode(tag);
            return true;
        }
        if (key->key() == Qt::Key_F2 && !tag.isEmpty()) {
            if (QTreeWidgetItem* item = itemOf(tag)) a_tree->editItem(item, 0);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ------------------------------------------------------------------ The settings

void RF25Panel::scheduleSettings()
{
    if (a_settingsPending) return;
    a_settingsPending = true;
    QTimer::singleShot(0, this, [this] {
        a_settingsPending = false;
        showSettings();
    });
}

void RF25Panel::commit(const QString& key, const QJsonValue& value, const QString& what)
{
    if (!a_doc || a_shownTag.isEmpty()) return;
    const Node* n = a_doc->model().find(a_shownTag);
    if (n == nullptr || valueOf(*n, key) == value) return;
    a_focusKey = key;
    const NodeKind* k = kindOf(*n);
    const PropertyDef* p = k != nullptr ? k->property(key) : nullptr;
    a_doc->editNode(a_shownTag, [key, value](Node& node) { node.props.insert(key, value); },
                    what.isEmpty() ? tr("Change %1").arg(p != nullptr ? p->label : key) : what);
}

QString RF25Panel::check(const QString& text, const qucs_s::fem::Dim& expected, bool* bad) const
{
    *bad = false;
    if (!a_doc) return {};
    if (text.trimmed().isEmpty()) return tr("not given");
    qucs_s::fem::Scope scope(&qucs_s::fem::Scope::builtins());
    for (const qucs_s::fem::Model::Parameter& p : a_doc->model().parameters()) {
        const qucs_s::fem::Expression e = qucs_s::fem::Expression::compile(p.expression, scope);
        if (e.isValid() && e.isConstant()) scope.setConstant(p.name, e.constant(), e.dim());
    }
    const qucs_s::fem::Expression e = qucs_s::fem::Expression::compile(text, scope);
    if (!e.isValid()) {
        *bad = true;
        return e.error();
    }
    if (!e.isConstant()) return tr("it is not a constant here");
    QString said = QStringLiteral("= %1").arg(qucs_s::fem::formatQuantity(e.constant(), e.dim().isNone() ? expected : e.dim()));
    if (!e.dim().isNone() && !expected.isNone() && !(e.dim() == expected))
        said += tr(" - but %1 is expected").arg(qucs_s::fem::dimName(expected));
    return said;
}

void RF25Panel::showSettings()
{
    if (!a_doc) return;
    const QString tag = currentTag();
    const Model& m = a_doc->model();
    const Node* n = m.find(tag);
    // The editor with the focus keeps it when only its value came back.
    QWidget* focused = QApplication::focusWidget();
    const bool editingHere = focused != nullptr && a_form->isAncestorOf(focused) && tag == a_shownTag;
    if (editingHere && (qobject_cast<QLineEdit*>(focused) != nullptr || qobject_cast<QTableWidget*>(focused) != nullptr
                        || focused->parent()->inherits("QTableWidget")))
        return;   // (its commit rebuilds the form when it has ended)
    a_shownTag = tag;
    a_editors.clear();
    delete a_form;
    a_form = new QWidget;
    a_form->setObjectName(QStringLiteral("rfSettings"));
    auto* v = new QVBoxLayout(a_form);
    v->setContentsMargins(8, 6, 8, 6);
    if (n == nullptr) {
        a_scroll->setWidget(a_form);
        return;
    }
    const NodeKind* k = kindOf(*n);
    v->addWidget(new QLabel(QStringLiteral("<b>%1</b> <span style='color:gray'>(%2)</span>")
                                .arg((k != nullptr ? k->title : n->type).toHtmlEscaped(), n->tag.toHtmlEscaped()),
                            a_form));
    if (k != nullptr && !k->help.isEmpty()) {
        auto* help = new QLabel(k->help, a_form);
        help->setWordWrap(true);
        QPalette pal = help->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::Disabled, QPalette::WindowText));
        help->setPalette(pal);
        v->addWidget(help);
    }
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft);
    if (k != nullptr && !k->fixed) {
        auto* label = new QLineEdit(n->label, a_form);
        label->setObjectName(QStringLiteral("rfLabel"));
        connect(label, &QLineEdit::editingFinished, this, [this, tag, label] {
            const Node* now = a_doc ? a_doc->model().find(tag) : nullptr;
            if (now != nullptr && label->text() != now->label) renameNode(tag, label->text());
        });
        form->addRow(k->group == QLatin1String("stack") ? tr("Name") : tr("Label"), label);
        a_editors.insert(QStringLiteral("label"), label);
    }
    if (k != nullptr)
        for (const PropertyDef& p : k->properties) {
            if (!qucs_s::fem::propertyVisible(*n, p, k)) continue;
            QWidget* editor = makeEditor(*n, p);
            if (editor == nullptr) continue;
            editor->setToolTip(editor->toolTip().isEmpty() ? p.tooltip : editor->toolTip());
            // (A row of widgets - a file and its browse button - puts its
            // own line edit in: that, not the row, is what is typed into.)
            if (!a_editors.contains(p.key)) a_editors.insert(p.key, editor);
            if (p.kind == PropertyDef::Rows) {
                form->addRow(new QLabel(QStringLiteral("<b>%1</b>").arg(p.label.toHtmlEscaped()), a_form));
                form->addRow(editor);
            } else {
                auto* label = new QLabel(p.label, a_form);
                label->setToolTip(p.tooltip);
                label->setWordWrap(true);
                form->addRow(label, editor);
            }
        }
    v->addLayout(form);
    // An export writes: the button that does it, and what it said.
    if (n->type == QLatin1String("rf_touchstone")) {
        auto* write = new QPushButton(tr("Write"), a_form);
        write->setObjectName(QStringLiteral("rfWriteExport"));
        write->setToolTip(tr("Writes the file at the study's own frequencies. The model must be solved first."));
        auto* said = new QLabel(a_form);
        said->setObjectName(QStringLiteral("rfExportSaid"));
        said->setWordWrap(true);
        said->setTextInteractionFlags(Qt::TextSelectableByMouse);
        said->hide();
        const QString tag = n->tag;
        connect(write, &QPushButton::clicked, this, [this, tag, said] {
            if (a_doc == nullptr) return;
            QString why;
            const QString path = a_doc->writeExport(tag, &why);
            if (path.isEmpty()) {
                said->setText(tr("It could not be written: %1").arg(why));
            } else {
                said->setText(tr("Written to %1.").arg(path));
            }
            said->show();
        });
        v->addWidget(write);
        v->addWidget(said);
    }
    // What this node did, where it is one that does something: the
    // processing's steps under the Processing, the cells and what they
    // would cost under the Mesh, both made of the layout as read.
    QStringList did;
    const std::shared_ptr<const Processed> processed = a_doc->processed();
    const std::shared_ptr<const Mesh> mesh = a_doc->mesh();
    QStringList cannot;
    const bool itsOwn = n->type == QLatin1String("rf_processing") || n->type == QLatin1String("rf_mesh")
                        || n->type == QLatin1String("rf_layout");
    // (The Results node is not one of those: what it shows is of the last
    // solve, which a run of the shapes and the mesh has already thrown
    // away, so it says so for itself.)
    if (n->type == QLatin1String("rf_processing")) {
        if (processed) {
            did = processed->report.lines;
            cannot = processed->problems;
        }
    } else if (n->type == QLatin1String("rf_mesh")) {
        if (mesh) {
            did = mesh->lines;
            cannot = mesh->problems;
        }
    } else if (n->type == QLatin1String("rf_layout")) {
        if (processed)
            did << tr("%1 shape(s) sent to a layer of the stack, %2 left out by the map.")
                       .arg(processed->report.read).arg(processed->report.ignored);
    } else if (n->type == QLatin1String("rf_results")) {
        // What the solve found, where there is one: the capacitance of
        // each net, and each as a line.
        if (a_doc->isSolving()) {
            did << tr("Solving…");
        } else if (a_doc->extraction() && a_doc->extraction()->ok()) {
            did = a_doc->solvedLines();
            did += a_doc->lineSummary();
            did += a_doc->sParameterSummary();
        } else if (!a_doc->solveProblem().isEmpty()) {
            cannot << a_doc->solveProblem();
        } else {
            did << tr("Nothing is solved yet: the tab's Solve finds the capacitance of the conductors.");
        }
    }
    if (a_doc->isPreparing() && itsOwn) {
        did.clear();
        cannot.clear();
        did << tr("Being made…");
    }
    if (!did.isEmpty()) {
        auto* report = new QLabel(QStringLiteral("• ") + did.join(QStringLiteral("\n• ")), a_form);
        report->setObjectName(QStringLiteral("rfNodeReport"));
        report->setWordWrap(true);
        report->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(report);
    }
    // What is missing, where it is about this node: the stack's problems
    // under the stack, the whole model's under the model.
    QStringList said = cannot;
    if (n->type == QLatin1String("rf_model")) said = a_doc->problems();
    else if (n->type == QLatin1String("rf_stack")) said = m.stack().problems;
    else if (n->type == QLatin1String("rf_layout") && !a_doc->layoutProblem().isEmpty()) said << a_doc->layoutProblem();
    if (!said.isEmpty()) {
        auto* problems = new QLabel(QStringLiteral("• ") + said.join(QStringLiteral("\n• ")), a_form);
        problems->setObjectName(QStringLiteral("rfNodeProblems"));
        problems->setWordWrap(true);
        problems->setStyleSheet(QStringLiteral("QLabel { color: rgb(198, 40, 40); }"));
        v->addWidget(problems);
    }
    v->addStretch(1);
    a_scroll->setWidget(a_form);
    if (QWidget* again = a_editors.value(a_focusKey)) {
        // Back to where the user was.
        again->setFocus();
        a_focusKey.clear();
    }
}

QWidget* RF25Panel::makeEditor(const Node& node, const PropertyDef& p)
{
    const QString key = p.key;
    switch (p.kind) {
    case PropertyDef::Expression: {
        // Checked as it is typed: its value, or why it is not one.
        auto* edit = new QLineEdit(textOf(node, key), a_form);
        edit->setObjectName(QStringLiteral("rfEdit_") + key);
        if (!p.unit.isEmpty()) edit->setPlaceholderText(p.unit);
        const auto say = [this, edit, expected = p.dim()] {
            bool bad = false;
            const QString said = check(edit->text(), expected, &bad);
            edit->setToolTip(said);
            QPalette pal = edit->palette();
            pal.setColor(QPalette::Text, bad ? QColor(198, 40, 40) : QApplication::palette().color(QPalette::Text));
            edit->setPalette(pal);
        };
        say();
        connect(edit, &QLineEdit::textEdited, this, [say](const QString&) { say(); });
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text().trimmed()); });
        return edit;
    }
    case PropertyDef::Text: {
        auto* edit = new QLineEdit(textOf(node, key), a_form);
        edit->setObjectName(QStringLiteral("rfEdit_") + key);
        // A layer of the stack by name (a via's metals, a port's
        // conductor): those of the stack offered.
        if (key == QLatin1String("conductor") || key == QLatin1String("from") || key == QLatin1String("to")) {
            auto* w = new QWidget(a_form);
            auto* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            auto* pick = new QToolButton(w);
            pick->setText(QStringLiteral("…"));
            pick->setObjectName(QStringLiteral("rfPick_") + key);
            pick->setPopupMode(QToolButton::InstantPopup);
            auto* menu = new QMenu(pick);
            QStringList names = a_doc ? a_doc->model().stack().conductors() : QStringList();
            if (key != QLatin1String("conductor")) names << QStringLiteral("ground");
            for (const QString& name : names)
                menu->addAction(name, this, [this, key, edit, name] {
                    edit->setText(name);
                    commit(key, name);
                });
            pick->setMenu(menu);
            edit->setParent(w);
            h->addWidget(edit, 1);
            h->addWidget(pick);
            connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text().trimmed()); });
            a_editors.insert(key, edit);   // (the line, not the row: the tests type into it)
            return w;
        }
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text()); });
        return edit;
    }
    case PropertyDef::Choice: {
        auto* box = new QComboBox(a_form);
        box->setObjectName(QStringLiteral("rfEdit_") + key);
        for (int i = 0; i < p.choices.size(); ++i) box->addItem(p.choiceLabels.value(i, p.choices[i]), p.choices[i]);
        box->setCurrentIndex(std::max(0, box->findData(textOf(node, key))));
        connect(box, &QComboBox::activated, this, [this, key, box](int) { commit(key, box->currentData().toString()); });
        return box;
    }
    case PropertyDef::Bool: {
        auto* check = new QCheckBox(a_form);
        check->setObjectName(QStringLiteral("rfEdit_") + key);
        check->setChecked(flagOf(node, key));
        connect(check, &QCheckBox::toggled, this, [this, key](bool on) { commit(key, on); });
        return check;
    }
    case PropertyDef::Integer: {
        auto* spin = new QSpinBox(a_form);
        spin->setObjectName(QStringLiteral("rfEdit_") + key);
        spin->setRange(0, 1000000);
        spin->setValue(integerOf(node, key));
        connect(spin, &QSpinBox::editingFinished, this, [this, key, spin] { commit(key, spin->value()); });
        return spin;
    }
    case PropertyDef::Rows: return rowsEditor(node, p);
    case PropertyDef::File: {
        auto* w = new QWidget(a_form);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(textOf(node, key), w);
        edit->setObjectName(QStringLiteral("rfEdit_") + key);
        const bool written = node.type == QLatin1String("rf_touchstone");
        edit->setPlaceholderText(written ? tr("empty: model_%1 beside the model").arg(node.tag)
                                         : tr("a GDSII or OASIS layout, beside the model"));
        auto* browse = new QToolButton(w);
        browse->setText(QStringLiteral("…"));
        browse->setObjectName(QStringLiteral("rfBrowse_") + key);
        h->addWidget(edit, 1);
        h->addWidget(browse);
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text().trimmed()); });
        connect(browse, &QToolButton::clicked, this, [this, key, edit, written] {
            const QString model = a_doc ? a_doc->getDocName() : QString();
            const QDir dir = model.isEmpty() ? QDir(QucsSettings.QucsWorkDir) : QFileInfo(model).absoluteDir();
            const QString chosen =
                written ? QFileDialog::getSaveFileName(this, tr("Export to"), dir.absolutePath(),
                                                       tr("Touchstone files (*.s1p *.s2p *.s3p *.s4p);;All files (*)"))
                        : QFileDialog::getOpenFileName(this, tr("A Layout"), dir.absolutePath(),
                                                       tr("Layouts (*.gds *.gds2 *.gdsii *.gds.gz *.oas *.oasis);;All files (*)"));
            if (chosen.isEmpty()) return;
            // Relative to the model, when it is saved: the two move together.
            const QString value = model.isEmpty() ? chosen : dir.relativeFilePath(chosen);
            edit->setText(value);
            commit(key, value);
        });
        a_editors.insert(key, edit);   // (the line, not the row)
        return w;
    }
    default: break;
    }
    return nullptr;
}

QWidget* RF25Panel::rowsEditor(const Node& node, const PropertyDef& p)
{
    const QString key = p.key;
    auto* w = new QWidget(a_form);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* table = new QTableWidget(w);
    table->setObjectName(QStringLiteral("rfRows_") + key);
    const QStringList headers = p.choiceLabels.isEmpty() ? QStringList{QStringLiteral("1"), QStringLiteral("2")} : p.choiceLabels;
    const int columns = int(headers.size());
    table->setColumnCount(columns);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    const QJsonArray rows = valueOf(node, key).toArray();
    table->setRowCount(int(rows.size()));
    for (int r = 0; r < rows.size(); ++r) {
        const QJsonArray cells = rows.at(r).toArray();
        for (int c = 0; c < columns; ++c) table->setItem(r, c, new QTableWidgetItem(cellText(cells.at(c))));
    }
    table->resizeColumnsToContents();
    table->setMinimumHeight(std::min(320, 30 + 26 * std::max(3, int(rows.size()) + 1)));
    v->addWidget(table);
    const auto collect = [table, columns] {
        QJsonArray out;
        for (int r = 0; r < table->rowCount(); ++r) {
            const auto cell = [&](int c) { return table->item(r, c) != nullptr ? table->item(r, c)->text().trimmed() : QString(); };
            bool empty = true;
            for (int c = 0; c < columns; ++c) empty = empty && cell(c).isEmpty();
            if (empty) continue;
            // (A row is built by appending: a braced list of one item is a
            // copy of that item to CI's older AppleClang, not a list of it.)
            QJsonArray row;
            for (int c = 0; c < columns; ++c) row.append(cell(c));
            out.append(row);
        }
        return out;
    };
    connect(table, &QTableWidget::itemChanged, this, [this, key, collect](QTableWidgetItem*) {
        a_focusKey = key;
        commit(key, collect(), tr("Change the Table"));
    });
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), w);
    add->setObjectName(QStringLiteral("rfRowsAdd_") + key);
    auto* remove = new QPushButton(tr("Remove"), w);
    remove->setObjectName(QStringLiteral("rfRowsRemove_") + key);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    v->addLayout(buttons);
    const bool parameters = node.type == QLatin1String("rf_parameters");
    connect(add, &QPushButton::clicked, this, [this, key, collect, columns, parameters] {
        QJsonArray rows2 = collect();
        QJsonArray row;
        if (parameters) {
            // A name of its own, and a value to start from.
            QStringList taken;
            for (const QJsonValue& r : std::as_const(rows2)) taken << r.toArray().at(0).toString();
            QString name = QStringLiteral("p1");
            for (int i = 2; taken.contains(name); ++i) name = QStringLiteral("p%1").arg(i);
            row.append(name);
            row.append(QStringLiteral("1"));
        } else {
            for (int c = 0; c < columns; ++c) row.append(QString());
        }
        rows2.append(row);
        commit(key, rows2, tr("Add a Row"));
    });
    connect(remove, &QPushButton::clicked, this, [this, key, table, collect] {
        const int r = table->currentRow();
        if (r < 0) return;
        QJsonArray rows2 = collect();
        if (r < rows2.size()) rows2.removeAt(r);
        commit(key, rows2, tr("Remove a Row"));
    });
    return w;
}
