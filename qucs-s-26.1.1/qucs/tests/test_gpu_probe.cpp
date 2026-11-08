/*
 * test_gpu_probe.cpp - the check of whether the GPU can draw, on a display
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
// The tests run on the offscreen platform, where rhiAvailable() asks no
// backend: its check of OpenGL - Linux's - never ran in them, and it ended
// the program on Linux the first time it was asked, as Application
// Settings opened (docs/bug_hunts/2026-11-08-linux-debug-ci.md, F8). Run
// here in a process of its own on the machine's own platform - Cocoa,
// Windows, X11 or Wayland where there is a display -, whatever it finds,
// and the process has to end as it should.

#include "rhicanvas.h"

#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtTest>

#include <cstdio>
#include <cstring>

class TestGpuProbe : public QObject
{
    Q_OBJECT

private slots:
    void theOpenGLCheckEndsCleanly()
    {
#if !QT_CONFIG(opengl)
        QSKIP("This Qt has no OpenGL.");
#else
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
        if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
            QSKIP("No display here (neither DISPLAY nor WAYLAND_DISPLAY).");
#endif
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("QT_QPA_PLATFORM"));   // (the machine's own)
        p.setProcessEnvironment(env);
        p.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--probe")});
        QVERIFY(p.waitForStarted());
        QVERIFY2(p.waitForFinished(120000), "the check did not end in two minutes");
        const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
        const QString err = QString::fromLocal8Bit(p.readAllStandardError());
        qInfo("%s", qPrintable(out.trimmed()));
        QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0,
                 qPrintable(QStringLiteral("exit %1 (%2): %3").arg(p.exitCode()).arg(int(p.exitStatus())).arg(err.right(2000))));
        QVERIFY2(out.contains(QStringLiteral("OpenGL checked")), qPrintable(out + err.right(2000)));
#endif
    }
};

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--probe") == 0) {
        QGuiApplication app(argc, argv);
#if QT_CONFIG(opengl)
        const bool fit = qucs_s::gpu::openGLFit();
        std::printf("OpenGL checked on %s: %s\n", qPrintable(QGuiApplication::platformName()), fit ? "fit for the viewers" : "not fit");
#endif
        std::fflush(stdout);
        return 0;
    }
    QGuiApplication app(argc, argv);
    TestGpuProbe test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gpu_probe.moc"
