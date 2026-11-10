/*
 * test_qucsator_robustness.cpp - qucsator_rf on damaged netlists: an error
 *                                said, never a signal (the hunt of 9
 *                                November, Q1)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class TestQucsatorRobustness : public QObject
{
    Q_OBJECT

    struct Run {
        bool crashed = false, finished = false;
        int code = 0;
        QString said;
    };
    /// qucsator_rf on \a netlist, in a folder of its own.
    static Run simulate(const QByteArray& netlist)
    {
        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("in.net")));
        if (!f.open(QIODevice::WriteOnly)) return {};
        f.write(netlist);
        f.close();
        QProcess p;
        p.setWorkingDirectory(dir.path());
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.setStandardInputFile(QProcess::nullDevice());
        p.start(QStringLiteral(QUCSATOR), {QStringLiteral("-i"), f.fileName(), QStringLiteral("-o"), dir.filePath(QStringLiteral("out.dat"))});
        Run r;
        r.finished = p.waitForFinished(120000);
        if (!r.finished) p.kill();
        r.crashed = p.exitStatus() == QProcess::CrashExit;
        r.code = p.exitCode();
        r.said = QString::fromLocal8Bit(p.readAll());
        return r;
    }

private slots:
    /// An S-parameter file part whose Touchstone file is missing: said. One
    /// of one node compared its uninitialised count of ports with its own 0
    /// and read the missing file's data - SIGSEGV (the hunt of 9 November,
    /// Q1); one of two said a port count of whatever the memory held.
    void aMissingTouchstoneFile()
    {
        const QByteArray head = ".SP:SP1 Type=\"log\" Start=\"1 Hz\" Stop=\"10 GHz\" Points=\"11\" Noise=\"no\" saveCVs=\"no\" saveAll=\"no\"\n"
                                "R:R1 _net0 _net1 R=\"50 Ohm\" Temp=\"26.85\" Tc1=\"0.0\" Tc2=\"0.0\" Tnom=\"26.85\"\n"
                                "C:C1 _net1 gnd C=\"1 pF\" V=\"\"\n"
                                "Pac:P1 _net0 gnd Num=\"1\" Z=\"50 Ohm\" P=\"0 dBm\" f=\"1 GHz\" Temp=\"26.85\"\n"
                                "Pac:P2 _net2 gnd Num=\"2\" Z=\"50 Ohm\" P=\"0 dBm\" f=\"1 GHz\" Temp=\"26.85\"\n";
        for (const char* nodes : {"X1t2 gnd", "X1 _net2 gnd", "X1 _net2 _net1 gnd"}) {
            const Run r = simulate(head + "SPfile:" + nodes + " File=\"{./missing.s2p}\" Data=\"rectangular\" Interpolator=\"linear\" duringDC=\"open\"\n");
            QVERIFY2(r.finished, nodes);
            QVERIFY2(!r.crashed, qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(nodes), r.said.right(600))));
            QVERIFY2(r.said.contains(QLatin1String("missing.s2p")), qPrintable(r.said.right(600)));
            QVERIFY2(!r.said.contains(QLatin1String("-port")) || r.said.contains(QLatin1String("could not be read")), qPrintable(r.said.right(600)));
        }
    }
};

QTEST_GUILESS_MAIN(TestQucsatorRobustness)
#include "test_qucsator_robustness.moc"
