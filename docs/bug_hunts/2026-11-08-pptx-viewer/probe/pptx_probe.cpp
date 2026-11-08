// pptx_probe: the deck reader and its drawing alone (pptx.h), no window.
//
//   pptx_probe [--scale S] [--bench] [--threads N] deck...
//
// Each deck read, then each slide drawn at scale S (1: 72 dpi), its parts
// and a search; a line a deck, "deck: <path>" before it starts (a crash
// pinned to the last such line). --bench: the read's time, the first
// slide's, every slide's, and the peak memory.
#include "pptx.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <sys/resource.h>
#include <mach/mach.h>

static long nowMB()
{
    mach_task_basic_info info{};
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&info), &n);
    return long(info.resident_size / (1024 * 1024));
}

#include <cstdio>

static long peakMB()
{
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return long(u.ru_maxrss / (1024 * 1024));   // (bytes on macOS)
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    qreal scale = 1;
    bool bench = false;
    int repeat = 1;
    QString out;
    QStringList decks;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--scale") && i + 1 < args.size()) scale = args.at(++i).toDouble();
        else if (args.at(i) == QLatin1String("--bench")) bench = true;
        else if (args.at(i) == QLatin1String("--out") && i + 1 < args.size()) out = args.at(++i);
        else if (args.at(i) == QLatin1String("--repeat") && i + 1 < args.size()) repeat = args.at(++i).toInt();
        else decks << args.at(i);
    }
    for (const QString& path : decks) {
        std::printf("deck: %s\n", qPrintable(path));
        std::fflush(stdout);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            std::printf("  cannot open\n");
            continue;
        }
        const QByteArray bytes = f.readAll();
        QElapsedTimer clock;
        clock.start();
        QString why;
        auto deck = qucs_s::pptx::Presentation::read(bytes, &why);
        const qint64 readMs = clock.elapsed();
        if (deck == nullptr) {
            std::printf("  refused (%lld ms): %s\n", readMs, qPrintable(why.left(200)));
            std::fflush(stdout);
            continue;
        }
        const long afterRead = nowMB();
        long inked = 0;
        qint64 first = -1, total = 0, worst = 0;
        for (int r = 0; r < repeat; ++r)
            for (int i = 0; i < deck->slideCount(); ++i) {
                QElapsedTimer one;
                one.start();
                const QImage image = deck->render(i, scale);
                const qint64 ms = one.elapsed();
                if (first < 0) first = ms;
                total += ms;
                worst = std::max(worst, ms);
                if (!out.isEmpty() && r == 0) image.save(QStringLiteral("%1/%2-%3.png").arg(out, QFileInfo(path).completeBaseName()).arg(i + 1));
                if (!image.isNull()) inked += image.pixel(image.width() / 2, image.height() / 2) != 0xffffffff;
                if (!bench) {
                    (void)deck->parts(i);
                    // A tile at 800 %, from the slide's middle; its pictures readied as another thread would.
                    const QSize big = deck->pixels(8);
                    (void)deck->render(i, 8, QRect(big.width() / 2 - 128, big.height() / 2 - 128, 256, 256));
                    deck->prepare(i, 0.15);
                    QImage small(64, 48, QImage::Format_ARGB32_Premultiplied);
                    small.fill(Qt::white);
                    QPainter p(&small);
                    deck->render(i, &p, QRectF(0, 0, 64, 48));
                }
            }
        if (!bench) (void)deck->find(QStringLiteral("e"));
        for (int i = 0; i < deck->slideCount() && !out.isEmpty(); ++i)
            if (!deck->slide(i).notShown.isEmpty()) std::printf("  slide %d not shown: %s\n", i + 1, qPrintable(deck->slide(i).notShown.join(QStringLiteral(" | "))));
        std::printf("  %d slides, read %lld ms, first slide %lld ms, all %lld ms (worst %lld), peak %ld MB, %ld inked, %ld MB after reading, %ld MB at the end\n", deck->slideCount(), readMs, first,
                    total, worst, peakMB(), inked, afterRead, nowMB());
        std::fflush(stdout);
    }
    return 0;
}
