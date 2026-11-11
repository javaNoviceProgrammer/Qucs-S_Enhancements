/*
 * test_qucsator_robustness.cpp - qucsator_rf on damaged netlists, and
 *                                qucsconv_rf on damaged data and SPICE
 *                                files: an error said, never a signal or a
 *                                sanitizer's report (the hunts of 9 November,
 *                                Q1, and 10 November, R1-R4, U3, U4)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <QProcess>
#include <QPair>
#include <tuple>
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
    /// qucsator_rf on \a netlist, in a folder of its own (\a files beside it:
    /// name, content).
    static Run simulate(const QByteArray& netlist, const QList<QPair<QString, QByteArray>>& files = {})
    {
        QTemporaryDir dir;
        if (!write(dir.filePath(QStringLiteral("in.net")), netlist)) return {};
        for (const auto& [name, content] : files)
            if (!write(dir.filePath(name), content)) return {};
        return run(QStringLiteral(QUCSATOR), {QStringLiteral("-i"), dir.filePath(QStringLiteral("in.net")), QStringLiteral("-o"), dir.filePath(QStringLiteral("out.dat"))},
                   dir.path());
    }
    /// qucsconv_rf on \a content, from \a in to \a out.
    static Run convert(const QByteArray& content, const char* in, const char* out)
    {
        QTemporaryDir dir;
        if (!write(dir.filePath(QStringLiteral("in")), content)) return {};
        return run(QStringLiteral(QUCSCONV),
                   {QStringLiteral("-if"), QLatin1String(in), QStringLiteral("-of"), QLatin1String(out), QStringLiteral("-i"), dir.filePath(QStringLiteral("in")),
                    QStringLiteral("-o"), dir.filePath(QStringLiteral("out"))},
                   dir.path());
    }
    static bool write(const QString& path, const QByteArray& content)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(content);
        return true;
    }
    static Run run(const QString& program, const QStringList& args, const QString& in)
    {
        QProcess p;
        p.setWorkingDirectory(in);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.setStandardInputFile(QProcess::nullDevice());
        p.start(program, args);
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

    // --- the hunt of 10 November ------------------------------------------

    /// A Touchstone file of `#` alone - an option line with no options, the
    /// defaults' - read: its list of options was never made, and the checker
    /// asked it its length (R3): the simulator's S-parameter file part and the
    /// converter both ended with SIGSEGV.
    void aTouchstoneFileOfAHashAlone()
    {
        const QByteArray netlist = ".SP:SP1 Type=\"lin\" Start=\"1 GHz\" Stop=\"2 GHz\" Points=\"3\" Noise=\"no\" saveCVs=\"no\" saveAll=\"no\"\n"
                                   "Pac:P1 _net0 gnd Num=\"1\" Z=\"50 Ohm\" P=\"0 dBm\" f=\"1 GHz\" Temp=\"26.85\"\n"
                                   "SPfile:X1 _net0 gnd File=\"{./hash.s1p}\" Data=\"rectangular\" Interpolator=\"linear\" duringDC=\"open\"\n";
        for (const QByteArray& file : {QByteArray("#\n"), QByteArray("#\n1 0.5 0\n2 0.4 -10\n")}) {
            const Run s = simulate(netlist, {{QStringLiteral("hash.s1p"), file}});
            QVERIFY2(s.finished && !s.crashed, qPrintable(s.said.right(600)));
            const Run c = convert(file, "touchstone", "qucsdata");
            QVERIFY2(c.finished && !c.crashed, qPrintable(c.said.right(600)));
        }
        // (with data, `#` alone is the defaults': GHz, S, MA, 50 Ohm)
        QCOMPARE(convert("#\n1 0.5 0\n2 0.4 -10\n", "touchstone", "qucsdata").code, 0);
    }

    /// A CSV column whose name begins with a NUL byte: an empty name, which
    /// the converter's check wrote `_` over the terminator of and read on -
    /// a heap overflow (R4). The simulator's file-driven source reads its CSV
    /// through it.
    void aCsvColumnNamedNothing()
    {
        const QByteArray csv("\"\0x\";\"y\"\n0;1\n1e-3;2\n", 20);
        const Run c = convert(csv, "csv", "qucsdata");
        QVERIFY2(c.finished && !c.crashed, qPrintable(c.said.right(600)));
        const QByteArray netlist = ".TR:TR1 Type=\"lin\" Start=\"0\" Stop=\"1 ms\" Points=\"11\" IntegrationMethod=\"Trapezoidal\" Order=\"2\" InitialStep=\"1 ns\" "
                                   "MinStep=\"1e-16\" MaxIter=\"150\" RelTol=\"0.001\" AbsTol=\"1 pA\" Vntol=\"1 uV\" Temp=\"26.85\" LTEreltol=\"1e-3\" "
                                   "LTEabstol=\"1e-6\" LTEfactor=\"1\" Solver=\"CroutLU\" relaxTSR=\"no\" initialDC=\"yes\" MaxStep=\"0\"\n"
                                   "Vfile:V1 _net0 gnd File=\"{./v.csv}\" Interpolator=\"linear\" Repeat=\"no\" G=\"1\" T=\"0\"\n"
                                   "R:R1 _net0 gnd R=\"1 kOhm\" Temp=\"26.85\" Tc1=\"0.0\" Tc2=\"0.0\" Tnom=\"26.85\"\n";
        const Run s = simulate(netlist, {{QStringLiteral("v.csv"), csv}});
        QVERIFY2(s.finished && !s.crashed, qPrintable(s.said.right(600)));
    }

    /// A VCD file's `$upscope` before any `$scope` (no scope to leave), and
    /// its `$dumpvars` before the first time stamp (no change set to put the
    /// values in): each a NULL dereferenced (R1). The values dumped first are
    /// the ones at 0.
    void vcdCommandsOutOfOrder()
    {
        const Run up = convert("$upscope $end\n$enddefinitions $end\n", "vcd", "qucsdata");
        QVERIFY2(up.finished && !up.crashed, qPrintable(up.said.right(600)));
        const Run dump = convert("$scope module top $end\n$var wire 1 ! clk $end\n$upscope $end\n$enddefinitions $end\n"
                                 "$dumpvars\n0!\n$end\n#10\n1!\n",
                                 "vcd", "qucsdata");
        QVERIFY2(dump.finished && !dump.crashed, qPrintable(dump.said.right(600)));
        QCOMPARE(dump.code, 0);
    }

    /// Names past 255 characters, `sprintf`ed into buffers of 256 (R2): a
    /// SPICE line the converter cannot translate (its comment), a part's
    /// instance (the Qucs netlist's), a library's component, a CITI
    /// variable. Overflows of the stack and of a static buffer; the first
    /// aborted on the stack guard in Release.
    void longNames()
    {
        const QByteArray x(300, 'x'), y(1100, 'y');
        for (const auto& [content, in, out] :
             {std::tuple<QByteArray, const char*, const char*>{"* a long name\nB" + x + " 1 0 V=1\nR1 1 0 1k\n.end\n", "spice", "qucs"},
              {"* a long name\nR" + x + " 1 0 1k\n.end\n", "spice", "qucs"},
              {"* a long name\nD" + y + " 1 0 D1\n.model D1 D\n.end\n", "spice", "qucslib"},
              {"CITIFILE A.01.00\nNAME DATA\nVAR F" + x + " MAG 2\nDATA S RI\nVAR_LIST_BEGIN\n1\n2\nVAR_LIST_END\nBEGIN\n0.1,0.2\n0.3,0.4\nEND\n",
               "citi", "qucsdata"}}) {
            const Run r = convert(content, in, out);
            QVERIFY2(r.finished && !r.crashed, qPrintable(QStringLiteral("%1 -> %2: %3").arg(QLatin1String(in), QLatin1String(out), r.said.right(600))));
        }
    }

    /// Undefined behaviour the sanitizers stop on (U3, U4): a ZVR file's
    /// points count past an int, cast from a double; a node's name of 40
    /// characters, its hash shifted until negative and then shifted again.
    void undefinedBehaviour()
    {
        const QByteArray zvr = "ZVR, 2.0\nstart;stop;unit;points;type;offset\n1e9;3e9;Hz;1e20;S;0\npara;fmt;format;aa;bb;cc\n"
                               "S11;none;RI;none;none;none\nxx;yy\nnone;none\nfreq;reS11;imS11\n1e9;0.1;0.2\n2e9;0.3;0.4\n";
        const Run z = convert(zvr, "zvr", "qucsdata");
        QVERIFY2(z.finished && !z.crashed, qPrintable(z.said.right(600)));
        const QByteArray node(40, 'n');
        const Run h = convert("* a long node\nR1 " + node + " 0 1k\nR2 " + node + " 0 2k\n.end\n", "spice", "qucs");
        QVERIFY2(h.finished && !h.crashed, qPrintable(h.said.right(600)));
    }
};

QTEST_GUILESS_MAIN(TestQucsatorRobustness)
#include "test_qucsator_robustness.moc"
