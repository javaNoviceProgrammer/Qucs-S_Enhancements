/*
 * What a click on a link in a document does: links.h.
 */
#include "links.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>

namespace qucs_s::links {

bool opensItself(const QString& path)
{
    static const QStringList ours = {"md", "markdown", "sch", "dpl", "sym", "txt", "csv", "tsv", "xlsx", "xlsm",
                                     "cir", "net", "sp", "spice", "lib", "inc", "mod", "ckt", "va", "v", "sv",
                                     "vhd", "vhdl", "m", "py", "json", "xml", "pdf", "log", "zip"};
    return ours.contains(QFileInfo(path).suffix().toLower());
}

Target resolve(const QUrl& url, const QString& baseDir)
{
    Target t;
    t.url = url;
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto")) {
        t.action = Action::Outside;
        return t;
    }
    // A single letter is a Windows drive (C:/...), not a scheme.
    if (!scheme.isEmpty() && scheme != QLatin1String("file") && scheme.size() > 1) {
        t.action = url.isValid() ? Action::Ask : Action::Nothing;
        return t;
    }
    QString path = url.isLocalFile() ? url.toLocalFile() : (scheme.size() == 1 ? url.toString() : url.path());
    if (path.isEmpty()) return t;
    if (QFileInfo(path).isRelative() && !baseDir.isEmpty()) path = QDir(baseDir).absoluteFilePath(path);
    const QFileInfo info(path);
    if (!info.exists()) return t;
    t.path = QDir::cleanPath(info.absoluteFilePath());
    t.action = info.isFile() && opensItself(t.path) ? Action::Open : Action::Reveal;
    return t;
}

void reveal(const QString& path)
{
#if defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#elif defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
    const QFileInfo info(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath() : info.absolutePath()));
#endif
}

void follow(QWidget* parent, const QUrl& url, const QString& baseDir, const std::function<void(const QString&)>& open)
{
    const Target t = resolve(url, baseDir);
    switch (t.action) {
    case Action::Nothing:
        return;
    case Action::Outside:
        QDesktopServices::openUrl(t.url);
        return;
    case Action::Open:
        if (open) open(t.path);
        return;
    case Action::Reveal: {
#if defined(Q_OS_MACOS)
        const QString show = QObject::tr("Show in Finder");
#elif defined(Q_OS_WIN)
        const QString show = QObject::tr("Show in Explorer");
#else
        const QString show = QObject::tr("Show in the File Manager");
#endif
        QMessageBox box(QMessageBox::Question, QObject::tr("Link"),
                        QObject::tr("The link leads to “%1”, which Qucs-S does not open.").arg(QFileInfo(t.path).fileName()),
                        QMessageBox::NoButton, parent);
        box.setObjectName(QStringLiteral("linkQuestion"));
        box.setInformativeText(QObject::tr("%1\n\nIt is not opened from here: a program would run. Show it where it is?")
                                   .arg(QDir::toNativeSeparators(t.path)));
        QPushButton* yes = box.addButton(show, QMessageBox::AcceptRole);
        yes->setObjectName(QStringLiteral("linkReveal"));
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.setEscapeButton(cancel);
        box.exec();
        if (box.clickedButton() == yes) reveal(t.path);
        return;
    }
    case Action::Ask: {
        QMessageBox box(QMessageBox::Question, QObject::tr("Link"),
                        QObject::tr("Open this link with the program the system has for “%1:”?").arg(t.url.scheme()),
                        QMessageBox::NoButton, parent);
        box.setObjectName(QStringLiteral("linkQuestion"));
        box.setInformativeText(t.url.toDisplayString());
        QPushButton* yes = box.addButton(QObject::tr("Open"), QMessageBox::AcceptRole);
        yes->setObjectName(QStringLiteral("linkOpen"));
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.setEscapeButton(cancel);
        box.exec();
        if (box.clickedButton() == yes) QDesktopServices::openUrl(t.url);
        return;
    }
    }
}

} // namespace qucs_s::links
