/*
 * multiphysics3dpanel.cpp - the Multiphysics 3D tab: its start page, a 3D
 *                           model's tree and the settings of its nodes
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "multiphysics3dpanel.h"

#include "fem_materials.h"
#include "main.h"
#include "multiphysicspanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace qucs_s::fem;

namespace {

constexpr int TagRole = Qt::UserRole + 1;

QIcon glyph(const QString& icon)
{
    return MultiphysicsPanel::nodeGlyph(icon);
}

QString cellText(const QJsonValue& v)
{
    if (v.isDouble()) return formatNumber(v.toDouble(), 15);
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return v.toString();
}

QString levelText(Level level)
{
    switch (level) {
    case Level::Domain: return QCoreApplication::translate("Multiphysics3DPanel", "domains");
    case Level::Boundary: return QCoreApplication::translate("Multiphysics3DPanel", "faces");
    case Level::Edge: return QCoreApplication::translate("Multiphysics3DPanel", "edges");
    case Level::Point: return QCoreApplication::translate("Multiphysics3DPanel", "points");
    case Level::None: break;
    }
    return {};
}

} // namespace

Multiphysics3DPanel::Multiphysics3DPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("Multiphysics3DPanel"));
    auto* all = new QVBoxLayout(this);
    all->setContentsMargins(0, 0, 0, 0);
    a_pages = new QStackedWidget(this);
    all->addWidget(a_pages);

    // No 3D model in front: make one, open one.
    a_start = new QWidget(a_pages);
    {
        auto* v = new QVBoxLayout(a_start);
        v->setContentsMargins(10, 10, 10, 10);
        v->addWidget(new QLabel(tr("<b>Multiphysics 3D</b>"), a_start));
        auto* about = new QLabel(tr("A 3D finite element model: its solids - boxes and cylinders along z, sketches extruded "
                                    "(rectangles, circles, polygons, a drawing's or a layout's shapes), a chip's layer stack over a "
                                    "layout -, united, cut and intersected, made one into domains, faces, edges and points; its "
                                    "materials, and its mesh of prisms swept from a mesh of its plan; heat, electrostatics, currents, "
                                    "solid mechanics and light solved on it. Open a model (.qfem3d) to see its tree here."),
                                 a_start);
        about->setWordWrap(true);
        about->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        about->setMinimumHeight(0);
        v->addWidget(about);
        a_newButton = new QPushButton(tr("New 3D Model"), a_start);
        a_newButton->setObjectName(QStringLiteral("mp3NewModel"));
        connect(a_newButton, &QPushButton::clicked, this, &Multiphysics3DPanel::newModelRequested);
        auto* open = new QPushButton(tr("Open Model…"), a_start);
        connect(open, &QPushButton::clicked, this, [this] { emit openModelRequested(QString()); });
        auto* buttons = new QHBoxLayout;
        buttons->addWidget(a_newButton);
        buttons->addWidget(open);
        buttons->addStretch(1);
        v->addLayout(buttons);
        v->addWidget(new QLabel(tr("Examples"), a_start));
        a_examples = new QListWidget(a_start);
        a_examples->setObjectName(QStringLiteral("mp3Examples"));
        v->addWidget(a_examples, 1);
        v->addWidget(new QLabel(tr("Recent models"), a_start));
        a_recent = new QListWidget(a_start);
        v->addWidget(a_recent, 1);
        for (QListWidget* list : {a_examples, a_recent})
            connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
                emit openModelRequested(item->data(Qt::UserRole).toString());
            });
    }
    a_pages->addWidget(a_start);

    a_splitter = new QSplitter(Qt::Vertical, a_pages);
    a_splitter->setObjectName(QStringLiteral("mp3PanelSplitter"));
    a_tree = new QTreeWidget(a_splitter);
    a_tree->setObjectName(QStringLiteral("mp3Tree"));
    a_tree->setColumnCount(2);
    a_tree->setHeaderHidden(true);
    a_tree->header()->setStretchLastSection(false);
    a_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    a_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    a_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    a_tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    connect(a_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (a_rebuilding || !item || !a_doc) return;
        a_doc->focusNode(item->data(0, TagRole).toString());
        showSettings();
    });
    connect(a_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = a_tree->itemAt(pos);
        if (!item) return;
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
        if (n && item->text(0) != n->name()) renameNode(tag, item->text(0));
    });
    a_scroll = new QScrollArea(a_splitter);
    a_scroll->setWidgetResizable(true);
    a_scroll->setFrameShape(QFrame::NoFrame);
    a_form = new QWidget;
    a_form->setObjectName(QStringLiteral("mp3Settings"));
    a_scroll->setWidget(a_form);
    a_splitter->addWidget(a_tree);
    a_splitter->addWidget(a_scroll);
    a_splitter->setStretchFactor(0, 1);
    a_splitter->setStretchFactor(1, 1);
    a_splitter->setSizes({460, 440});
    a_pages->addWidget(a_splitter);
    fillStart();
    a_pages->setCurrentWidget(a_start);
}

void Multiphysics3DPanel::fillStart()
{
    a_examples->clear();
    const QDir examples(QDir(QucsSettings.ExamplesDir).filePath(QStringLiteral("multiphysics3d")));
    for (const QFileInfo& f : examples.entryInfoList({QStringLiteral("*.qfem3d")}, QDir::Files, QDir::Name)) {
        QFile file(f.absoluteFilePath());
        QJsonObject o;
        if (file.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(file.read(1 << 20)).object();
        const QString title = o.value(QStringLiteral("title")).toString();
        auto* item = new QListWidgetItem(glyph(QStringLiteral("model")), title.isEmpty() ? f.completeBaseName() : title, a_examples);
        item->setData(Qt::UserRole, f.absoluteFilePath());
        item->setToolTip(o.value(QStringLiteral("description")).toString());
    }
    a_recent->clear();
    for (const QString& r : QucsSettings.RecentDocs)
        if (r.endsWith(QLatin1String(".qfem3d"), Qt::CaseInsensitive) && QFileInfo::exists(r)) {
            auto* item = new QListWidgetItem(glyph(QStringLiteral("model")), QFileInfo(r).fileName(), a_recent);
            item->setData(Qt::UserRole, r);
            item->setToolTip(r);
        }
}

void Multiphysics3DPanel::setDocument(Multiphysics3DDoc* doc)
{
    if (a_doc == doc) return;
    for (const QMetaObject::Connection& c : a_connections) disconnect(c);
    a_connections.clear();
    a_doc = doc;
    if (!doc) {
        fillStart();
        a_pages->setCurrentWidget(a_start);
        return;
    }
    a_connections << connect(doc, &Multiphysics3DDoc::modelChanged, this, [this] {
        rebuildTree();
        scheduleSettings();
    });
    a_connections << connect(doc, &Multiphysics3DDoc::stateChanged, this, [this] {
        updateStatusColumn();
        scheduleSettings();
    });
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
    selectNode(doc->focus().isEmpty() ? doc->model().geometry().tag : doc->focus());
}

// ------------------------------------------------------------------ The tree

QTreeWidgetItem* Multiphysics3DPanel::itemOf(const QString& tag) const
{
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        if ((*it)->data(0, TagRole).toString() == tag) return *it;
        ++it;
    }
    return nullptr;
}

QString Multiphysics3DPanel::currentTag() const
{
    QTreeWidgetItem* item = a_tree->currentItem();
    return item ? item->data(0, TagRole).toString() : QString();
}

void Multiphysics3DPanel::selectNode(const QString& tag)
{
    if (QTreeWidgetItem* item = itemOf(tag)) {
        a_tree->setCurrentItem(item);
        a_tree->scrollToItem(item);
    }
}

void Multiphysics3DPanel::addItems(QTreeWidgetItem* parent, const Node& node)
{
    QHash<QString, int> ofType, seen;
    for (const Node& c : node.children)
        if (c.label.isEmpty()) ++ofType[c.type];
    for (const Node& c : node.children) {
        auto* item = new QTreeWidgetItem(parent);
        const NodeKind* k = c.kind();
        QString shown = c.name();
        if (c.label.isEmpty() && k && ofType.value(c.type) > 1) shown = k->title + QLatin1Char(' ') + QString::number(++seen[c.type]);
        if (c.type == QLatin1String("component")) shown = c.label.isEmpty() ? tr("Component (3D)") : c.label;
        item->setText(0, shown);
        item->setIcon(0, glyph(c.type == QLatin1String("component") ? QStringLiteral("component3d") : k ? k->icon : QString()));
        item->setData(0, TagRole, c.tag);
        Qt::ItemFlags flags = item->flags();
        if (k && !k->fixed) flags |= Qt::ItemIsEditable;
        item->setFlags(flags);
        QStringList tip;
        if (k) tip << k->title;
        if (k && k->property(QStringLiteral("selection")))
            tip << describeSelection3(c.selection(), k->property(QStringLiteral("level")) ? levelOf(c.text(QStringLiteral("level"))) : k->selection);
        item->setToolTip(0, tip.join(QLatin1String(": ")));
        if (!c.enabled) {
            QFont f = item->font(0);
            f.setItalic(true);
            item->setFont(0, f);
            item->setForeground(0, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        } else if (c.visibility == QLatin1String("hidden")) {
            // (Hidden in the view, not disabled: dimmed, not slanted.)
            item->setForeground(0, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        }
        addItems(item, c);
    }
}

void Multiphysics3DPanel::rebuildTree()
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
    root->setText(0, a_doc->getDocName().isEmpty() ? tr("untitled.qfem3d") : QFileInfo(a_doc->getDocName()).fileName());
    root->setIcon(0, glyph(QStringLiteral("model")));
    root->setData(0, TagRole, m.root.tag);
    addItems(root, m.root);
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        const QString tag = (*it)->data(0, TagRole).toString();
        (*it)->setExpanded(first || !collapsed.contains(tag));
        ++it;
    }
    if (QTreeWidgetItem* item = itemOf(current)) a_tree->setCurrentItem(item);
    a_rebuilding = false;
    updateStatusColumn();
}

void Multiphysics3DPanel::updateStatusColumn()
{
    if (!a_doc) return;
    if (QTreeWidgetItem* root = a_tree->topLevelItem(0))
        root->setText(0, a_doc->getDocName().isEmpty() ? tr("untitled.qfem3d") : QFileInfo(a_doc->getDocName()).fileName());
    const QColor grey = QApplication::palette().color(QPalette::Disabled, QPalette::Text);
    const QColor red(198, 40, 40);
    QTreeWidgetItemIterator it(a_tree);
    while (*it) {
        QTreeWidgetItem* item = *it;
        ++it;
        const QString tag = item->data(0, TagRole).toString();
        const Node* n = a_doc->model().find(tag);
        if (!n) continue;
        QString status;
        QColor color = grey;
        const GeometryBuild3& g = a_doc->geometry();
        if (n->type == QLatin1String("geometry")) {
            if (a_doc->geometryBeingBuilt()) status = tr("being built");
            else if (a_doc->geometryStale()) status = tr("not built");
            else if (!g.ok()) {
                status = tr("failed");
                color = red;
            } else {
                const int count = int(g.topology->domains.size());
                status = count == 1 ? tr("1 domain") : tr("%1 domains").arg(count);
            }
        } else if (n->type == QLatin1String("mesh")) {
            status = a_doc->meshBeingMade() ? tr("being made")
                     : a_doc->meshStale() || !a_doc->mesh()
                         ? tr("not built")
                         : tr("%1 prisms").arg(QLocale().toString(qlonglong(a_doc->mesh()->prisms.size())));
        } else if (!a_doc->geometryStale() && g.errors.contains(tag)) {
            status = tr("error");
            color = red;
            item->setToolTip(0, g.errors.value(tag));
        } else if (!a_doc->geometryStale() && g.warnings.contains(tag)) {
            item->setToolTip(0, g.warnings.value(tag));
        }
        // (What the view draws of it, where it says nothing else.)
        if (status.isEmpty()) {
            const Multiphysics3DDoc::Visibility v = a_doc->visibility(tag);
            if (v == Multiphysics3DDoc::Visibility::Hidden) status = tr("hidden");
            else if (v == Multiphysics3DDoc::Visibility::Frame) status = tr("frame only");
        }
        item->setText(1, status);
        item->setForeground(1, color);
    }
}

QMenu* Multiphysics3DPanel::menuFor(const QString& tag)
{
    if (!a_doc) return nullptr;
    const Model& m = a_doc->model();
    const Node* n = m.find(tag);
    if (!n) return nullptr;
    const NodeKind* k = n->kind();
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("mp3NodeMenu"));
    if (k) {
        if (n->type == QLatin1String("materials")) {
            QMenu* lib = menu->addMenu(tr("Add Material from Library"));
            QHash<QString, QMenu*> categories;
            QList<MaterialEntry> entries = MultiphysicsPanel::userMaterials();
            entries << builtinMaterials();
            for (const MaterialEntry& e : entries) {
                QMenu*& cat = categories[e.category];
                if (!cat) cat = lib->addMenu(e.category);
                QAction* a = cat->addAction(e.name, this, [this, tag, e] { addNode(tag, QStringLiteral("material:") + e.name); });
                a->setToolTip(e.description);
            }
            menu->addAction(glyph(QStringLiteral("material")), tr("Blank Material"), this, [this, tag] { addNode(tag, QStringLiteral("material")); });
        } else {
            const QStringList kinds = childKinds(*n, m.spaceUnder(*n));
            QMenu* add = nullptr;
            for (const QString& type : kinds) {
                const NodeKind* ck = nodeKind(type);
                if (!ck || ck->fixed || (ck->unique && n->child(type))) continue;
                if (!add) add = menu->addMenu(tr("Add"));
                add->addAction(glyph(ck->icon), ck->title, this, [this, tag, type] { addNode(tag, type); });
            }
        }
        menu->addSeparator();
    }
    if (n->type == QLatin1String("geometry") || (k && k->group == QLatin1String("geometry")))
        menu->addAction(tr("Build All"), a_doc.data(), [this] {
            a_doc->showGeometry();
            a_doc->requestGeometry();
        });
    if (n->type == QLatin1String("mesh") || (k && k->group == QLatin1String("mesh")))
        menu->addAction(tr("Build Mesh"), a_doc.data(), &Multiphysics3DDoc::buildMesh);
    // An eigenmode expansion or a full-wave solve over wavelengths: its
    // spectrum.
    if (n->type == QLatin1String("eme") || n->type == QLatin1String("waves3") || n->type == QLatin1String("waves3t")) {
        QAction* spectrum = menu->addAction(tr("Plot Spectrum"), a_doc.data(), [this] {
            QString why;
            if (a_doc->showSpectrum(true, &why).isEmpty()) QMessageBox::information(this, tr("Plot Spectrum"), why);
        });
        spectrum->setEnabled(a_doc->solution() && a_doc->solution()->spectral && a_doc->solution()->snapshotCount() > 1);
    }
    if (k && !k->fixed && n->type != QLatin1String("model")) {
        menu->addSeparator();
        if (!n->isDefault) menu->addAction(tr("Duplicate"), this, [this, tag] { duplicateNode(tag); });
        menu->addAction(n->enabled ? tr("Disable") : tr("Enable"), this, [this, tag, on = !n->enabled] { setNodeEnabled(tag, on); });
        // What the 3D view draws of it - the view's alone, the model as it
        // is: the two ways it is not drawn now.
        if (a_doc->canHide(tag)) {
            using V = Multiphysics3DDoc::Visibility;
            const V now = a_doc->visibility(tag);
            const struct {
                V v;
                QString text, tip;
                const char* name;
            } ways[] = {{V::Shown, tr("Show"), tr("Draw it in the 3D view again"), "mp3Show"},
                        {V::Hidden, tr("Hide"), tr("Leave it out of the 3D view: it is built, meshed and solved as before"), "mp3Hide"},
                        {V::Frame, tr("Frame Only"), tr("Draw its edges alone in the 3D view, not its faces"), "mp3FrameOnly"}};
            for (const auto& w : ways) {
                if (w.v == now) continue;
                QAction* a = menu->addAction(w.text, this, [this, tag, v = w.v] { setNodeVisibility(tag, v); });
                a->setObjectName(QString::fromLatin1(w.name));
                a->setToolTip(w.tip);
            }
        }
        menu->addAction(tr("Rename"), this, [this, tag] {
            if (QTreeWidgetItem* item = itemOf(tag)) a_tree->editItem(item, 0);
        });
        menu->addAction(tr("Move Up"), this, [this, tag] { moveNode(tag, -1); });
        menu->addAction(tr("Move Down"), this, [this, tag] { moveNode(tag, 1); });
        if (!n->isDefault) {
            QAction* del = menu->addAction(tr("Delete"), this, [this, tag] { removeNode(tag); });
            del->setShortcut(QKeySequence::Delete);
        }
    }
    return menu;
}

QString Multiphysics3DPanel::addNode(const QString& parentTag, const QString& type)
{
    if (!a_doc) return {};
    Model m = a_doc->model();
    Node node;
    QString what;
    if (type.startsWith(QLatin1String("material:"))) {
        const QString name = type.mid(9);
        std::optional<MaterialEntry> e = findMaterial(name, MultiphysicsPanel::userMaterials());
        if (!e) return {};
        node = materialNode(m, *e);
        what = tr("Add %1").arg(e->name);
    } else {
        node = m.make(type);
        const NodeKind* k = node.kind();
        if (!k) return {};
        what = tr("Add %1").arg(k->title);
    }
    QString why;
    Node* added = m.add(parentTag, node, -1, &why);
    if (!added) {
        QMessageBox::information(this, tr("Add"), why);
        return {};
    }
    const QString tag = added->tag;
    a_doc->setModel(m, what);
    selectNode(tag);
    return tag;
}

bool Multiphysics3DPanel::removeNode(const QString& tag)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    const Node* n = m.find(tag);
    if (!n) return false;
    const QString name = n->name();
    QString why;
    if (!m.remove(tag, &why)) {
        QMessageBox::information(this, tr("Delete"), why);
        return false;
    }
    a_doc->setModel(m, tr("Delete %1").arg(name));
    return true;
}

bool Multiphysics3DPanel::duplicateNode(const QString& tag)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    int index = -1;
    Node* parent = m.parentOf(tag, &index);
    if (!parent) return false;
    Node copy = parent->children[std::size_t(index)];
    const NodeKind* k = copy.kind();
    if (!k || k->fixed || copy.isDefault) return false;
    std::function<void(Node&)> clearTags = [&](Node& n) {
        n.tag.clear();
        n.visibility.clear();   // (the copy drawn, where the first is hidden)
        // (A sketch's objects names of their own too.)
        if (n.kind() && n.kind()->group == QLatin1String("geometry")) n.label.clear();
        for (Node& c : n.children) clearTags(c);
    };
    clearTags(copy);
    const QString parentTag = parent->tag;
    const QString name = parent->children[std::size_t(index)].name();
    // (Each sketch object's name made its own, as it is added.)
    std::vector<Node> sketch = std::move(copy.children);
    copy.children.clear();
    Node* added = m.add(parentTag, copy, index + 1);
    if (!added) return false;
    const QString newTag = added->tag;
    for (Node& c : sketch) m.add(newTag, c);
    a_doc->setModel(m, tr("Duplicate %1").arg(name));
    selectNode(newTag);
    return true;
}

bool Multiphysics3DPanel::moveNode(const QString& tag, int by)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    int index = -1;
    Node* parent = m.parentOf(tag, &index);
    if (!parent) return false;
    const int to = index + by;
    if (to < 0 || to >= int(parent->children.size())) return false;
    const NodeKind* other = parent->children[std::size_t(to)].kind();
    if (other && other->fixed) return false;
    std::swap(parent->children[std::size_t(index)], parent->children[std::size_t(to)]);
    a_doc->setModel(m, by < 0 ? tr("Move Up") : tr("Move Down"));
    selectNode(tag);
    return true;
}

bool Multiphysics3DPanel::setNodeEnabled(const QString& tag, bool enabled)
{
    if (!a_doc) return false;
    return a_doc->editNode(tag, [enabled](Node& n) { n.enabled = enabled; }, enabled ? tr("Enable") : tr("Disable"));
}

bool Multiphysics3DPanel::setNodeVisibility(const QString& tag, Multiphysics3DDoc::Visibility visibility)
{
    return a_doc && a_doc->setVisibility(tag, visibility);
}

bool Multiphysics3DPanel::renameNode(const QString& tag, const QString& label)
{
    if (!a_doc) return false;
    Model m = a_doc->model();
    Node* n = m.find(tag);
    if (!n) return false;
    const QString name = label.trimmed();
    const NodeKind* k = n->kind();
    if (k && k->group == QLatin1String("geometry") && n->type != QLatin1String("form union")) {
        static const QRegularExpression ok(QStringLiteral("^[A-Za-z_][A-Za-z0-9_ .-]*$"));
        if (name.isEmpty() || !ok.match(name).hasMatch() || (m.objectNames().contains(name) && name != n->label)) {
            QMessageBox::information(this, tr("Rename"), tr("%1 cannot be its name: a name of its own, letters and digits.").arg(name));
            rebuildTree();
            return false;
        }
        const QString old = n->label;
        n->label = name;
        m.renameObject(old, name);
    } else {
        n->label = name == (k ? k->title : QString()) ? QString() : name;
    }
    a_doc->setModel(m, tr("Rename"));
    return true;
}

// ------------------------------------------------------------------ The settings

void Multiphysics3DPanel::scheduleSettings()
{
    if (a_settingsPending) return;
    a_settingsPending = true;
    QTimer::singleShot(0, this, [this] {
        a_settingsPending = false;
        showSettings();
    });
}

void Multiphysics3DPanel::commit(const QString& key, const QJsonValue& value, const QString& what)
{
    if (!a_doc || a_shownTag.isEmpty()) return;
    const Node* n = a_doc->model().find(a_shownTag);
    if (!n || n->value(key) == value) return;
    a_focusKey = key;
    const NodeKind* k = n->kind();
    const PropertyDef* p = k ? k->property(key) : nullptr;
    a_doc->editNode(a_shownTag, [key, value](Node& node) { node.props.insert(key, value); },
                    what.isEmpty() ? tr("Change %1").arg(p ? p->label : key) : what);
}

QStringList Multiphysics3DPanel::objectsFor(const Node& node) const
{
    if (!a_doc) return {};
    Model m = a_doc->model();
    const Node* parent = m.parentOf(node.tag);
    if (parent && parent->type == QLatin1String("extrude")) {
        // A sketch's features use its own objects alone.
        QStringList out;
        for (const Node& c : parent->children) {
            if (c.tag == node.tag) break;
            if (!c.label.isEmpty()) out << c.label;
        }
        return out;
    }
    return a_doc->model().objectNames(node.tag);
}

void Multiphysics3DPanel::showSettings()
{
    if (!a_doc) return;
    const QString tag = currentTag();
    const Model& m = a_doc->model();
    const Node* n = m.find(tag);
    QWidget* focused = QApplication::focusWidget();
    const bool editingHere = focused && a_form->isAncestorOf(focused) && tag == a_shownTag;
    if (editingHere && (qobject_cast<QLineEdit*>(focused) || qobject_cast<QTableWidget*>(focused)
                        || (focused->parent() && focused->parent()->inherits("QTableWidget"))))
        return;   // (its commit makes the form again when it has ended)
    a_shownTag = tag;
    a_editors.clear();
    delete a_form;
    a_form = new QWidget;
    a_form->setObjectName(QStringLiteral("mp3Settings"));
    auto* v = new QVBoxLayout(a_form);
    v->setContentsMargins(8, 6, 8, 6);
    if (!n) {
        a_scroll->setWidget(a_form);
        return;
    }
    const NodeKind* k = n->kind();
    const QString title = n->type == QLatin1String("component") ? tr("Component (3D)") : k ? k->title : n->type;
    v->addWidget(new QLabel(QStringLiteral("<b>%1</b> <span style='color:gray'>(%2)</span>").arg(title.toHtmlEscaped(), n->tag.toHtmlEscaped()), a_form));
    if (k && !k->help.isEmpty() && n->type != QLatin1String("component")) {
        auto* help = new QLabel(k->help, a_form);
        help->setWordWrap(true);
        QPalette pal = help->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::Disabled, QPalette::WindowText));
        help->setPalette(pal);
        v->addWidget(help);
    }
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    if (k && !k->fixed && n->type != QLatin1String("model")) {
        const bool object = k->group == QLatin1String("geometry");
        auto* label = new QLineEdit(object ? n->label : n->name(), a_form);
        label->setObjectName(QStringLiteral("mp3Label"));
        connect(label, &QLineEdit::editingFinished, this, [this, tag, label] {
            const Node* now = a_doc ? a_doc->model().find(tag) : nullptr;
            if (now && label->text() != now->name() && label->text() != now->label) renameNode(tag, label->text());
        });
        form->addRow(object ? tr("Object name") : tr("Label"), label);
        a_editors.insert(QStringLiteral("label"), label);
    }
    if (n->type == QLatin1String("model")) {
        auto* title2 = new QLineEdit(n->label, a_form);
        title2->setObjectName(QStringLiteral("mp3Title"));
        connect(title2, &QLineEdit::editingFinished, this, [this, tag, title2] {
            if (a_doc) a_doc->editNode(tag, [t = title2->text()](Node& node) { node.label = t; }, tr("Change Title"));
        });
        form->addRow(tr("Title"), title2);
    }
    if (k)
        for (const PropertyDef& p : k->properties) {
            // (A property of the plane's alone - a step's adaptive refinement
            // - not shown.)
            if (!(p.spaces & a_doc->model().space()) || !propertyVisible(*n, p, k)) continue;
            QWidget* editor = makeEditor(*n, p);
            if (!editor) continue;
            if (editor->toolTip().isEmpty()) editor->setToolTip(p.tooltip);
            a_editors.insert(p.key, editor);
            if (p.kind == PropertyDef::Selection || p.kind == PropertyDef::Rows || p.kind == PropertyDef::Points) {
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
    // What it made: a feature's error, the geometry's or the mesh's counts.
    const GeometryBuild3& g = a_doc->geometry();
    QString said;
    if (!a_doc->geometryStale() && g.errors.contains(tag)) said = QStringLiteral("<span style='color:#c62828'>%1</span>").arg(g.errors.value(tag).toHtmlEscaped());
    else if (!a_doc->geometryStale() && g.warnings.contains(tag)) said = g.warnings.value(tag).toHtmlEscaped();
    else if (n->type == QLatin1String("geometry") && g.ok() && g.topology) {
        const Topology3& t = *g.topology;
        said = tr("%1 domains, %2 faces, %3 edges, %4 points; %5 levels in z").arg(t.domains.size()).arg(t.faces.size()).arg(t.edges.size()).arg(t.points.size()).arg(t.levels.size());
    } else if (n->type == QLatin1String("mesh") && a_doc->mesh() && !a_doc->meshStale()) {
        const Mesh3& mesh = *a_doc->mesh();
        said = tr("%1 prisms, %2 nodes: the plan's %3 triangles through %4 layers; its smallest angle %5°, its worst stretch %6")
                   .arg(QLocale().toString(qlonglong(mesh.prisms.size())), QLocale().toString(qlonglong(mesh.nodes.size())),
                        QLocale().toString(qlonglong(mesh.plan ? mesh.plan->triangles.size() : 0)))
                   .arg(mesh.heights.size() > 1 ? int(mesh.heights.size()) - 1 : 0)
                   .arg(mesh.minAngle, 0, 'f', 1)
                   .arg(mesh.maxStretch, 0, 'g', 3);
    }
    if (!said.isEmpty()) {
        auto* note = new QLabel(said, a_form);
        note->setObjectName(QStringLiteral("mp3Said"));
        note->setWordWrap(true);
        v->addWidget(note);
    }
    if (n->type == QLatin1String("geometry") || (k && k->group == QLatin1String("geometry"))) {
        auto* build = new QPushButton(tr("Build All"), a_form);
        connect(build, &QPushButton::clicked, this, [this] {
            if (!a_doc) return;
            a_doc->showGeometry();
            a_doc->requestGeometry();
        });
        v->addWidget(build);
    } else if (n->type == QLatin1String("mesh")) {
        auto* build = new QPushButton(tr("Build Mesh"), a_form);
        build->setObjectName(QStringLiteral("mp3BuildMeshButton"));
        connect(build, &QPushButton::clicked, this, [this] {
            if (a_doc) a_doc->buildMesh();
        });
        v->addWidget(build);
    } else if (QStringList{QStringLiteral("global"), QStringLiteral("integral"), QStringLiteral("average"), QStringLiteral("maximum"),
                           QStringLiteral("minimum"), QStringLiteral("pointeval")}
                   .contains(n->type)) {
        auto* evaluate = new QPushButton(tr("Evaluate"), a_form);
        evaluate->setObjectName(QStringLiteral("mp3EvaluateButton"));
        const QString tag = n->tag;
        connect(evaluate, &QPushButton::clicked, this, [this, tag] {
            if (a_doc) a_doc->evaluate(tag);
        });
        v->addWidget(evaluate);
    } else if (k && k->group == QLatin1String("export")) {
        auto* run = new QPushButton(tr("Export"), a_form);
        run->setObjectName(QStringLiteral("mp3ExportButton"));
        const QString tag = n->tag;
        connect(run, &QPushButton::clicked, this, [this, tag] {
            if (a_doc) a_doc->runExport(tag);
        });
        v->addWidget(run);
    } else if (n->type == QLatin1String("plotgroup3d")) {
        auto* plot = new QPushButton(tr("Plot"), a_form);
        plot->setObjectName(QStringLiteral("mp3PlotButton"));
        const QString tag = n->tag;
        connect(plot, &QPushButton::clicked, this, [this, tag] {
            if (a_doc) a_doc->showPlotGroup(tag);
        });
        v->addWidget(plot);
    }
    v->addStretch(1);
    a_scroll->setWidget(a_form);
    if (QWidget* again = a_editors.value(a_focusKey)) {
        again->setFocus();
        a_focusKey.clear();
    }
}

QWidget* Multiphysics3DPanel::makeEditor(const Node& node, const PropertyDef& p)
{
    const QString key = p.key;
    switch (p.kind) {
    case PropertyDef::Expression: {
        auto* edit = new QLineEdit(node.text(key), a_form);
        edit->setObjectName(QStringLiteral("mp3Edit_") + key);
        if (p.unit == QLatin1String("length")) edit->setPlaceholderText(tr("in the geometry's unit, or with one: 3[mm]"));
        else if (!p.unit.isEmpty() && p.unit != QLatin1String("angle")) edit->setPlaceholderText(p.unit);
        // Its value, as the parameters make it, in the tooltip.
        const QString text = node.text(key);
        if (!text.trimmed().isEmpty() && a_doc) {
            Expression::Options options;
            if (p.unit == QLatin1String("length")) {
                const std::optional<Unit> u = parseUnit(a_doc->model().component().text(QStringLiteral("unit")));
                options.lengthUnit = u ? u->scale : 1e-3;
                options.numbersAreLengths = true;
            }
            const Expression e = Expression::compile(text, a_doc->parameters()->scope, options);
            QString tip = p.tooltip;
            const QString value = !e.isValid() ? e.error()
                                  : e.isConstant() ? (p.unit == QLatin1String("length") ? tr("= %1 (the geometry's unit)").arg(formatNumber(e.constant(), 6))
                                                                                        : QStringLiteral("= %1").arg(formatQuantity(e.constant(), e.dim())))
                                                   : tr("not a constant");
            edit->setToolTip(tip.isEmpty() ? value : tip + QStringLiteral("\n") + value);
            if (!e.isValid()) edit->setStyleSheet(QStringLiteral("QLineEdit { color: #c62828; }"));
        }
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text().trimmed()); });
        return edit;
    }
    case PropertyDef::Pair: {
        auto* w = new QWidget(a_form);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        const QStringList values = node.pair(key);
        QLineEdit* x = new QLineEdit(values.value(0), w);
        QLineEdit* y = new QLineEdit(values.value(1), w);
        x->setObjectName(QStringLiteral("mp3Edit_") + key + QStringLiteral("_x"));
        y->setObjectName(QStringLiteral("mp3Edit_") + key + QStringLiteral("_y"));
        x->setPlaceholderText(QStringLiteral("x"));
        y->setPlaceholderText(QStringLiteral("y"));
        h->addWidget(x);
        h->addWidget(y);
        auto done = [this, key, x, y] { commit(key, QJsonArray{x->text().trimmed(), y->text().trimmed()}); };
        connect(x, &QLineEdit::editingFinished, this, done);
        connect(y, &QLineEdit::editingFinished, this, done);
        return w;
    }
    case PropertyDef::Text:
    case PropertyDef::Terminal: {
        auto* edit = new QLineEdit(node.text(key), a_form);
        edit->setObjectName(QStringLiteral("mp3Edit_") + key);
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text()); });
        return edit;
    }
    case PropertyDef::Choice: {
        auto* box = new QComboBox(a_form);
        box->setObjectName(QStringLiteral("mp3Edit_") + key);
        const unsigned space = a_doc ? a_doc->model().space() : unsigned(Spaces::ThreeD);
        for (int i = 0; i < p.choices.size(); ++i)
            if (p.choiceIn(i, space) || node.text(key) == p.choices[i]) box->addItem(p.choiceLabels.value(i, p.choices[i]), p.choices[i]);
        box->setCurrentIndex(std::max(0, box->findData(node.text(key))));
        connect(box, &QComboBox::activated, this, [this, key, box](int) { commit(key, box->currentData().toString()); });
        return box;
    }
    case PropertyDef::Bool: {
        auto* check = new QCheckBox(a_form);
        check->setObjectName(QStringLiteral("mp3Edit_") + key);
        check->setChecked(node.flag(key));
        connect(check, &QCheckBox::toggled, this, [this, key](bool on) { commit(key, on); });
        return check;
    }
    case PropertyDef::Integer: {
        auto* spin = new QSpinBox(a_form);
        spin->setObjectName(QStringLiteral("mp3Edit_") + key);
        spin->setRange(0, 1000000);
        spin->setValue(node.integer(key));
        connect(spin, &QSpinBox::editingFinished, this, [this, key, spin] { commit(key, spin->value()); });
        return spin;
    }
    case PropertyDef::Selection: return selectionEditor(node, p);
    case PropertyDef::Rows: return rowsEditor(node, p);
    case PropertyDef::Points: return pointsEditor(node, p);
    case PropertyDef::Objects: {
        auto* w = new QWidget(a_form);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(node.list(key).join(QLatin1String(", ")), w);
        edit->setObjectName(QStringLiteral("mp3Edit_") + key);
        edit->setPlaceholderText(tr("objects' names, split by commas"));
        auto* pickObjects = new QToolButton(w);
        pickObjects->setText(QStringLiteral("…"));
        pickObjects->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(pickObjects);
        const QStringList chosen = node.list(key);
        for (const QString& name : objectsFor(node)) {
            QAction* a = menu->addAction(name);
            a->setCheckable(true);
            a->setChecked(chosen.contains(name));
            connect(a, &QAction::toggled, this, [this, key, name](bool on) {
                const Node* now = a_doc ? a_doc->model().find(a_shownTag) : nullptr;
                if (!now) return;
                QStringList list = now->list(key);
                if (on && !list.contains(name)) list << name;
                if (!on) list.removeAll(name);
                commit(key, QJsonArray::fromStringList(list));
            });
        }
        pickObjects->setMenu(menu);
        h->addWidget(edit, 1);
        h->addWidget(pickObjects);
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] {
            QStringList list;
            for (const QString& s : edit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) list << s.trimmed();
            commit(key, QJsonArray::fromStringList(list));
        });
        return w;
    }
    case PropertyDef::File: {
        auto* w = new QWidget(a_form);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(node.text(key), w);
        edit->setObjectName(QStringLiteral("mp3Edit_") + key);
        const bool layout = node.type == QLatin1String("stack");
        edit->setPlaceholderText(layout ? tr("a GDSII or OASIS layout, beside the model") : tr("a DXF, SVG, GDSII or OASIS file, beside the model"));
        auto* browse = new QToolButton(w);
        browse->setText(QStringLiteral("…"));
        browse->setObjectName(QStringLiteral("mp3Browse_") + key);
        h->addWidget(edit, 1);
        h->addWidget(browse);
        connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text().trimmed()); });
        connect(browse, &QToolButton::clicked, this, [this, key, edit, layout] {
            const QString model = a_doc ? a_doc->getDocName() : QString();
            const QDir dir = model.isEmpty() ? QDir::home() : QFileInfo(model).absoluteDir();
            const QString layouts = tr("Layouts (*.gds *.gds2 *.gdsii *.gds.gz *.oas *.oasis)");
            const QString chosen = layout ? QFileDialog::getOpenFileName(this, tr("A Layout"), dir.absolutePath(), layouts + tr(";;All files (*)"))
                                          : QFileDialog::getOpenFileName(this, tr("Import a Drawing or a Layout"), dir.absolutePath(),
                                                                         tr("Drawings and layouts (*.dxf *.svg *.gds *.gds2 *.gdsii *.gds.gz *.oas *.oasis);;All files (*)"));
            if (chosen.isEmpty()) return;
            const QString value = model.isEmpty() ? chosen : dir.relativeFilePath(chosen);
            edit->setText(value);
            commit(key, value);
        });
        return w;
    }
    default: break;
    }
    // (A step's physics: those of the model, each checked to be solved.)
    if (p.kind == PropertyDef::Physics && a_doc) {
        auto* list = new QListWidget(a_form);
        list->setObjectName(QStringLiteral("mp3Edit_") + key);
        const QStringList chosen = node.list(key);
        for (const Node* ph : a_doc->model().physics()) {
            auto* item = new QListWidgetItem(QStringLiteral("%1 (%2)").arg(ph->name(), ph->tag), list);
            item->setData(Qt::UserRole, ph->tag);
            item->setCheckState(chosen.isEmpty() || chosen.contains(ph->tag) ? Qt::Checked : Qt::Unchecked);
        }
        list->setMaximumHeight(std::min(120, 22 * (list->count() + 1)));
        connect(list, &QListWidget::itemChanged, this, [this, key, list] {
            QStringList on;
            bool all = true;
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->checkState() == Qt::Checked) on << list->item(i)->data(Qt::UserRole).toString();
                else all = false;
            }
            commit(key, all ? QJsonArray() : QJsonArray::fromStringList(on));
        });
        return list;
    }
    // (A kind of value no editor above takes: its text.)
    auto* edit = new QLineEdit(node.text(key), a_form);
    edit->setObjectName(QStringLiteral("mp3Edit_") + key);
    connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { commit(key, edit->text().trimmed()); });
    return edit;
}

QWidget* Multiphysics3DPanel::selectionEditor(const Node& node, const PropertyDef& p)
{
    const NodeKind* k = node.kind();
    Level level = p.level != Level::None ? p.level : k ? k->selection : Level::Domain;
    if (k && k->property(QStringLiteral("level"))) level = levelOf(node.text(QStringLiteral("level")));
    if (level == Level::None) level = Level::Domain;
    const QJsonObject rule = node.selection();
    auto* w = new QWidget(a_form);
    w->setObjectName(QStringLiteral("mp3Selection"));
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    struct Kind {
        QString key, label;
    };
    QList<Kind> kinds = {{QStringLiteral("numbers"), tr("Numbers chosen")}, {QStringLiteral("all"), tr("All %1").arg(levelText(level))}};
    if (level == Level::Domain) {
        kinds << Kind{QStringLiteral("objects"), tr("Domains in solids")} << Kind{QStringLiteral("only"), tr("Solids' own domains")}
              << Kind{QStringLiteral("layer"), tr("Layers of a stack")};
    } else {
        kinds << Kind{QStringLiteral("of"), tr("On solids' surfaces")} << Kind{QStringLiteral("layer"), tr("On layers' surfaces")};
        if (level == Level::Boundary)
            kinds << Kind{QStringLiteral("between"), tr("Between solids")} << Kind{QStringLiteral("exterior"), tr("Exterior faces")}
                  << Kind{QStringLiteral("interior"), tr("Interior faces")} << Kind{QStringLiteral("normal"), tr("Facing a direction")};
        kinds << Kind{QStringLiteral("plane"), tr("In a plane")};
    }
    kinds << Kind{QStringLiteral("box"), tr("In a box")} << Kind{QStringLiteral("named"), tr("A named selection")};
    QString current = QStringLiteral("numbers");
    for (const Kind& kd : kinds)
        if (rule.contains(kd.key)) current = kd.key;
    auto* box = new QComboBox(w);
    box->setObjectName(QStringLiteral("mp3SelectionKind"));
    for (const Kind& kd : kinds) box->addItem(kd.label, kd.key);
    box->setCurrentIndex(std::max(0, box->findData(current)));
    v->addWidget(box);
    auto joined = [](const QJsonValue& value) {
        QStringList list;
        if (value.isString()) list << value.toString();
        for (const QJsonValue& x : value.toArray()) list << cellText(x);
        return list.join(QLatin1String(", "));
    };
    auto* args = new QLineEdit(w);
    args->setObjectName(QStringLiteral("mp3SelectionArgs"));
    QComboBox* side = nullptr;
    if (current == QLatin1String("numbers")) {
        args->setText(joined(rule.value(QStringLiteral("numbers"))));
        args->setPlaceholderText(tr("numbers, split by commas"));
    } else if (current == QLatin1String("objects") || current == QLatin1String("only") || current == QLatin1String("of") || current == QLatin1String("layer")) {
        args->setText(joined(rule.value(current)));
        args->setPlaceholderText(tr("solids' names, split by commas"));
    } else if (current == QLatin1String("between")) {
        const QJsonArray pair = rule.value(QStringLiteral("between")).toArray();
        args->setText(joined(pair.at(0)) + QStringLiteral("; ") + joined(pair.at(1)));
        args->setPlaceholderText(tr("solids; solids"));
    } else if (current == QLatin1String("box")) {
        args->setText(joined(rule.value(QStringLiteral("box"))));
        args->setPlaceholderText(tr("x0, y0, z0, x1, y1, z1 (the geometry's unit)"));
    } else if (current == QLatin1String("normal")) {
        args->setText(joined(rule.value(QStringLiteral("normal"))));
        args->setPlaceholderText(tr("x, y, z: 0, 0, 1"));
    } else if (current == QLatin1String("plane")) {
        const QJsonObject plane = rule.value(QStringLiteral("plane")).toObject();
        const QString axis = plane.isEmpty() ? QStringLiteral("z") : plane.keys().front();
        args->setText(QStringLiteral("%1 = %2").arg(axis, cellText(plane.value(axis))));
        args->setPlaceholderText(tr("z = 3 (or x =, y =)"));
    } else if (current == QLatin1String("named")) {
        args->setText(rule.value(QStringLiteral("named")).toString());
        args->setPlaceholderText(tr("the selection's name"));
    } else {
        args->hide();
    }
    v->addWidget(args);
    if (current == QLatin1String("of") || current == QLatin1String("layer")) {
        side = new QComboBox(w);
        side->setObjectName(QStringLiteral("mp3SelectionSide"));
        side->addItem(tr("All of them"), QString());
        side->addItem(tr("Those facing down"), QStringLiteral("bottom"));
        side->addItem(tr("Those facing up"), QStringLiteral("top"));
        side->addItem(tr("Those facing left (-x)"), QStringLiteral("left"));
        side->addItem(tr("Those facing right (+x)"), QStringLiteral("right"));
        side->addItem(tr("Those facing the front (-y)"), QStringLiteral("front"));
        side->addItem(tr("Those facing the back (+y)"), QStringLiteral("back"));
        side->setCurrentIndex(std::max(0, side->findData(rule.value(QStringLiteral("side")).toString())));
        v->addWidget(side);
    }
    // What it picks now.
    QString picked;
    if (a_doc && !a_doc->geometryStale() && a_doc->geometry().topology) {
        QString why;
        const QVector<int> got = resolveSelection3(*a_doc->geometry().topology, a_doc->model(), level, rule, &why);
        QStringList numbers;
        for (int i : got) numbers << QString::number(i + 1);
        picked = numbers.isEmpty() ? tr("none") : numbers.join(QLatin1String(", "));
        if (!why.isEmpty()) picked += QStringLiteral(" <span style='color:#c62828'>(%1)</span>").arg(why.toHtmlEscaped());
    } else {
        picked = tr("(build the geometry to see them)");
    }
    auto* shown = new QLabel(QStringLiteral("%1: %2").arg(describeSelection3(rule, level).toHtmlEscaped(), picked), w);
    shown->setObjectName(QStringLiteral("mp3SelectionShown"));
    shown->setWordWrap(true);
    v->addWidget(shown);
    auto ruleFrom = [args, side, box]() -> QJsonObject {
        const QString kind = box->currentData().toString();
        auto names = [](const QString& textIn) {
            QJsonArray a;
            for (const QString& s : textIn.split(QLatin1Char(','), Qt::SkipEmptyParts)) a.append(s.trimmed());
            return a;
        };
        auto numbersOf = [](const QString& textIn, bool integers) {
            QJsonArray a;
            for (const QString& s : textIn.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts))
                if (integers) a.append(s.toInt());
                else a.append(s.toDouble());
            return a;
        };
        if (kind == QLatin1String("all")) return {{QStringLiteral("all"), true}};
        if (kind == QLatin1String("exterior") || kind == QLatin1String("interior")) return {{kind, true}};
        if (kind == QLatin1String("objects") || kind == QLatin1String("only")) return {{kind, names(args->text())}};
        if (kind == QLatin1String("of") || kind == QLatin1String("layer")) {
            QJsonObject o{{kind, names(args->text())}};
            if (side && !side->currentData().toString().isEmpty()) o.insert(QStringLiteral("side"), side->currentData().toString());
            return o;
        }
        if (kind == QLatin1String("between")) {
            const QStringList two = args->text().split(QLatin1Char(';'));
            return {{QStringLiteral("between"), QJsonArray{names(two.value(0)), names(two.value(1))}}};
        }
        if (kind == QLatin1String("box")) {
            QJsonArray a = numbersOf(args->text(), false);
            while (a.size() < 6) a.append(0);
            return {{QStringLiteral("box"), a}};
        }
        if (kind == QLatin1String("normal")) {
            QJsonArray a = numbersOf(args->text(), false);
            if (a.isEmpty()) a = QJsonArray{0, 0, 1};
            return {{QStringLiteral("normal"), a}};
        }
        if (kind == QLatin1String("plane")) {
            const QStringList parts = args->text().split(QLatin1Char('='));
            const QString axis = parts.size() == 2 ? parts[0].trimmed().toLower() : QStringLiteral("z");
            const double at = parts.size() == 2 ? parts[1].trimmed().toDouble() : args->text().trimmed().toDouble();
            return {{QStringLiteral("plane"), QJsonObject{{axis == QLatin1String("x") || axis == QLatin1String("y") ? axis : QStringLiteral("z"), at}}}};
        }
        if (kind == QLatin1String("named")) return {{QStringLiteral("named"), args->text().trimmed()}};
        return {{QStringLiteral("numbers"), numbersOf(args->text(), true)}};
    };
    connect(box, &QComboBox::activated, this, [this, ruleFrom, rule, box, level] {
        // A rule of another kind: numbers from what it picks now, where it can.
        const QString kind = box->currentData().toString();
        QJsonObject fresh = ruleFrom();
        if (kind == QLatin1String("numbers") && a_doc && !a_doc->geometryStale() && a_doc->geometry().topology) {
            QJsonArray numbers;
            for (int i : resolveSelection3(*a_doc->geometry().topology, a_doc->model(), level, rule)) numbers.append(i + 1);
            fresh = {{QStringLiteral("numbers"), numbers}};
        }
        commit(QStringLiteral("selection"), fresh, tr("Change Selection"));
    });
    connect(args, &QLineEdit::editingFinished, this, [this, ruleFrom] { commit(QStringLiteral("selection"), ruleFrom(), tr("Change Selection")); });
    if (side) connect(side, &QComboBox::activated, this, [this, ruleFrom] { commit(QStringLiteral("selection"), ruleFrom(), tr("Change Selection")); });
    return w;
}

QWidget* Multiphysics3DPanel::rowsEditor(const Node& node, const PropertyDef& p)
{
    const QString key = p.key;
    const bool parameters = node.type == QLatin1String("parameters");
    auto* w = new QWidget(a_form);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* table = new QTableWidget(w);
    table->setObjectName(QStringLiteral("mp3Rows"));
    const QStringList headers = parameters ? QStringList{tr("Name"), tr("Expression"), tr("Value"), tr("Description")}
                                : p.choiceLabels.isEmpty() ? QStringList{QStringLiteral("x"), QStringLiteral("f(x)")}
                                                           : p.choiceLabels;
    const int columns = int(headers.size());
    table->setColumnCount(columns);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    const QJsonArray rows = node.value(key).toArray();
    table->setRowCount(int(rows.size()));
    const std::shared_ptr<ParameterScope> ps = a_doc ? a_doc->parameters() : nullptr;
    for (int r = 0; r < rows.size(); ++r) {
        if (parameters) {
            const QJsonObject o = rows.at(r).toObject();
            const QString name = o.value(QStringLiteral("name")).toString();
            table->setItem(r, 0, new QTableWidgetItem(name));
            table->setItem(r, 1, new QTableWidgetItem(o.value(QStringLiteral("expression")).toString()));
            QString value;
            QColor color = QApplication::palette().color(QPalette::Text);
            if (ps && ps->errors.contains(name)) {
                value = ps->errors.value(name);
                color = QColor(198, 40, 40);
            } else if (ps && ps->values.contains(name)) {
                value = formatQuantity(ps->values.value(name), ps->dims.value(name));
            }
            auto* shown = new QTableWidgetItem(value);
            shown->setFlags(shown->flags() & ~Qt::ItemIsEditable);
            shown->setForeground(color);
            table->setItem(r, 2, shown);
            table->setItem(r, 3, new QTableWidgetItem(o.value(QStringLiteral("description")).toString()));
        } else {
            const QJsonArray cells = rows.at(r).toArray();
            for (int c = 0; c < columns; ++c) table->setItem(r, c, new QTableWidgetItem(cellText(cells.at(c))));
        }
    }
    table->resizeColumnsToContents();
    table->setMinimumHeight(std::min(320, 30 + 26 * std::max(3, int(rows.size()) + 1)));
    v->addWidget(table);
    auto collect = [table, parameters, columns]() {
        QJsonArray out;
        for (int r = 0; r < table->rowCount(); ++r) {
            auto cell = [&](int c) { return table->item(r, c) ? table->item(r, c)->text().trimmed() : QString(); };
            if (parameters) {
                if (cell(0).isEmpty() && cell(1).isEmpty()) continue;
                QJsonObject o{{QStringLiteral("name"), cell(0)}, {QStringLiteral("expression"), cell(1)}};
                if (!cell(3).isEmpty()) o.insert(QStringLiteral("description"), cell(3));
                out.append(o);
            } else {
                bool any = false;
                QJsonArray row;
                for (int c = 0; c < columns; ++c) {
                    row.append(cell(c));
                    any = any || !cell(c).isEmpty();
                }
                if (any) out.append(row);
            }
        }
        return out;
    };
    connect(table, &QTableWidget::itemChanged, this, [this, key, collect, parameters](QTableWidgetItem* item) {
        if (parameters && item->column() == 2) return;
        a_focusKey = key;
        commit(key, collect(), parameters ? tr("Change Parameters") : tr("Change Rows"));
    });
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), w);
    add->setObjectName(QStringLiteral("mp3RowsAdd"));
    auto* remove = new QPushButton(tr("Remove"), w);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    v->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, [this, key, collect, parameters, node] {
        QJsonArray rows2 = collect();
        if (parameters) {
            QSet<QString> names;
            for (const QJsonValue& r : rows2) names.insert(r.toObject().value(QStringLiteral("name")).toString());
            QString name = QStringLiteral("p1");
            for (int i = 2; names.contains(name); ++i) name = QStringLiteral("p%1").arg(i);
            rows2.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("expression"), QStringLiteral("1")}});
        } else if (node.type == QLatin1String("stack")) {
            // A layer on the last: named, a micrometre thick.
            double top = 0;
            for (const QJsonValue& r : rows2) {
                const QJsonArray cells = r.toArray();
                top = std::max(top, cellText(cells.at(2)).toDouble() + cellText(cells.at(3)).toDouble());
            }
            rows2.append(QJsonArray{QStringLiteral("layer%1").arg(rows2.size() + 1), QString(), formatNumber(top, 12), QStringLiteral("1")});
        } else {
            rows2.append(QJsonArray{QStringLiteral("0"), QStringLiteral("0")});
        }
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

QWidget* Multiphysics3DPanel::pointsEditor(const Node& node, const PropertyDef& p)
{
    const QString key = p.key;
    // Its columns: those its property names (a point's x, y, z), else a
    // sketch's corners' x and y.
    const QStringList columns = p.choices.isEmpty() ? QStringList{QStringLiteral("x"), QStringLiteral("y")} : p.choices;
    const int n = int(columns.size());
    const bool corners = p.choices.isEmpty();
    auto* w = new QWidget(a_form);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* table = new QTableWidget(w);
    table->setObjectName(QStringLiteral("mp3Points"));
    table->setColumnCount(n);
    table->setHorizontalHeaderLabels(columns);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    const QJsonArray points = node.value(key).toArray();
    table->setRowCount(int(points.size()));
    for (int r = 0; r < points.size(); ++r) {
        const QJsonArray row = points.at(r).toArray();
        for (int c = 0; c < n; ++c) table->setItem(r, c, new QTableWidgetItem(cellText(row.at(c))));
    }
    table->setMinimumHeight(std::min(280, 30 + 26 * std::max(3, int(points.size()) + 1)));
    v->addWidget(table);
    auto collect = [table, n] {
        QJsonArray out;
        for (int r = 0; r < table->rowCount(); ++r) {
            QJsonArray row;
            bool any = false;
            for (int c = 0; c < n; ++c) {
                const QString t = table->item(r, c) ? table->item(r, c)->text().trimmed() : QString();
                any = any || !t.isEmpty();
                row.append(t);
            }
            if (any) out.append(row);
        }
        return out;
    };
    connect(table, &QTableWidget::itemChanged, this, [this, key, collect, corners] {
        a_focusKey = key;
        commit(key, collect(), corners ? tr("Change Corners") : tr("Change Points"));
    });
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), w);
    auto* remove = new QPushButton(tr("Remove"), w);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch(1);
    v->addLayout(buttons);
    connect(add, &QPushButton::clicked, this, [this, key, collect, n, corners] {
        QJsonArray rows = collect();
        QJsonArray zero;
        for (int c = 0; c < n; ++c) zero.append(QStringLiteral("0"));
        rows.append(zero);
        commit(key, rows, corners ? tr("Add a Corner") : tr("Add a Point"));
    });
    connect(remove, &QPushButton::clicked, this, [this, key, table, collect, corners] {
        const int r = table->currentRow();
        if (r < 0) return;
        QJsonArray rows = collect();
        if (r < rows.size()) rows.removeAt(r);
        commit(key, rows, corners ? tr("Remove a Corner") : tr("Remove a Point"));
    });
    return w;
}
