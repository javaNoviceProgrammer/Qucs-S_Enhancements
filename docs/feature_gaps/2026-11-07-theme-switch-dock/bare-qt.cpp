// Bare Qt, nothing of Qucs-S: a window whose palette reaches a child only when
// the window has no style sheet. On a Mac in dark mode, on a real display:
//   bare-qt                        child follows
//   bare-qt 'QLabel { margin: 0 }' child stays dark; window and label follow
// Build against Qt6::Widgets. (2026-11-07-theme-switch-dock.md)
#include <QtWidgets>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QMainWindow window;
    QWidget child;
    auto* layout = new QVBoxLayout(&child);
    QLabel label("a label");
    layout->addWidget(&label);
    window.setCentralWidget(&child);
    window.resize(300, 200);
    if (argc > 1) window.setStyleSheet(argv[1]);
    window.show();
    const auto report = [&](const char* when) {
        // Run the event loop first: Qt tells the window by a posted event.
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const auto colour = [](const QWidget* w) { return w->palette().color(QPalette::Window).name(); };
        qInfo().noquote() << when << "app" << QApplication::palette().color(QPalette::Window).name() << "window" << colour(&window)
                          << "child" << colour(&child) << "label" << colour(&label);
    };
    report("start");
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Light);
    report("light");
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    report("dark");
}
