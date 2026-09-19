/*
 * Drives the simulator-output parsers over a directory of ngspice output
 * files, the way the GUI and "qucs-s --run" do after a simulation:
 *
 *   simout_harness <schematic.sch> <workdir> <dataset-out>
 *
 * The schematic is netlisted (which is what decides the output file names
 * the kernel expects), then AbstractSpiceKernel::convertToQucsData() reads
 * spice4qucs.* from <workdir> and writes the Qucs dataset. The files may be
 * damaged in any way; the harness must exit normally (any exit code below
 * 128 and no sanitizer report). Used by scripts/ci/fuzz-simout.py.
 */
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

#include "config.h"
#include "main.h"
#include "misc.h"
#include "module.h"
#include "schematic.h"
#include "extsimkernels/ngspice.h"
#include "extsimkernels/spicecompat.h"

namespace {

class Harness : public Ngspice
{
public:
    using Ngspice::Ngspice;
    bool dcOpOnly() const { return a_DC_OP_only; }
    void setDcOpOnly(bool on) { a_DC_OP_only = on; }
    QStringList outputs() const { return a_output_files; }
};

int usage()
{
    fprintf(stderr, "usage: simout_harness <schematic.sch> <workdir> <dataset-out>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
        return usage();
    int one = 1;   // QApplication must not see our arguments
    QApplication app(one, argv);

    const QString schPath = QString::fromLocal8Bit(argv[1]);
    const QString workdir = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    const QString dataset = QString::fromLocal8Bit(argv[3]);

    QucsSettings.DefaultSimulator = spicecompat::simNgspice;
    QucsSettings.alwaysPrefixDataset = false;   // as "qucs-s --run"; see below
    QucsSettings.S4Qworkdir = workdir;
    QucsSettings.maxUndo = 20;
    QucsVersion = VersionTriplet(PACKAGE_VERSION);
    Module::registerModules();

    Schematic sch(nullptr, schPath);
    if (!sch.load()) {
        fprintf(stderr, "cannot load %s\n", qPrintable(schPath));
        return 3;
    }

    // Once without the "<simulation>." dataset prefix (the CLI default) and
    // once with it (the GUI setting most examples need), since the prefix
    // is applied by rewriting the variable names the parsers produced.
    for (bool prefix : {false, true}) {
        QucsSettings.alwaysPrefixDataset = prefix;
        Harness kernel(&sch);
        kernel.setWorkdir(workdir);
        // Netlisting fills in the list of output files the parsers will
        // look for; the netlist itself is not needed.
        kernel.SaveNetlist(QDir(workdir).filePath("harness.cir"), false);
        if (!prefix)
            fprintf(stderr, "outputs: %s\n", qPrintable(kernel.outputs().join(' ')));
        const QString out = prefix ? dataset + ".prefixed" : dataset;

        // As the GUI would convert it...
        kernel.convertToQucsData(out);
        // ...and the full conversion too when the GUI would only read the
        // operating point, so that every parser sees every file.
        if (kernel.dcOpOnly()) {
            kernel.setDcOpOnly(false);
            kernel.convertToQucsData(out + ".full");
        }
    }
    return 0;
}
