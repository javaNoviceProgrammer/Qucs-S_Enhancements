// estimate_probe.cpp - a 3D solve's estimate against what it took: a 1 mm
// box of heat (its bottom held, a source through it) meshed at a size,
// solved at an order by a solver; its estimate, its seconds, and its peak
// footprint over the footprint before (sampled every 5 ms).
//
//   estimate_probe <hmax mm> <order 1|2> <solver auto|iterative|direct>
//
// Built against the engine (build/qucs/multiphysics/libfem.a) as
// test_fem3_solve is; one size a process, so each peak is its own.
#include "fem3_estimate.h"
#include "fem3_geometry.h"
#include "fem3_mesh.h"
#include "fem3_solver.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>

#include <mach/mach.h>

#include <atomic>
#include <cstdio>
#include <thread>

using namespace qucs_s::fem;

static double footprint()
{
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &count) != KERN_SUCCESS) return 0;
    return double(info.phys_footprint);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) return 2;
    const QString hmax = QString::fromLatin1(argv[1]), order = QString::fromLatin1(argv[2]), solver = QString::fromLatin1(argv[3]);
    Model m = Model::blank3d();
    m.component().props.insert(QStringLiteral("unit"), QStringLiteral("mm"));
    Node box = m.make(QStringLiteral("box"));
    box.label = QStringLiteral("b");
    box.props.insert(QStringLiteral("size"), QJsonArray{QStringLiteral("1"), QStringLiteral("1")});
    box.props.insert(QStringLiteral("height"), QStringLiteral("1"));
    m.add(m.geometry().tag, box);
    Node heat = m.make(QStringLiteral("heat"));
    heat.props.insert(QStringLiteral("order"), order);
    const QString ht = heat.tag;
    m.add(m.component().tag, heat);
    for (Node& c : m.find(ht)->children)
        if (c.type == QLatin1String("heat/solid")) {
            c.props.insert(QStringLiteral("source"), QStringLiteral("user"));
            c.props.insert(QStringLiteral("k"), QStringLiteral("1[W/(m*K)]"));
        }
    auto side = [](const char* s) {
        QJsonArray of;
        of.append(QStringLiteral("b"));
        return QJsonObject{{QStringLiteral("of"), of}, {QStringLiteral("side"), QString::fromLatin1(s)}};
    };
    Node bottom = m.make(QStringLiteral("heat/temperature"));
    bottom.props.insert(QStringLiteral("selection"), side("bottom"));
    bottom.props.insert(QStringLiteral("T0"), QStringLiteral("300[K]"));
    m.add(ht, bottom);
    // (A source through it: its field no polynomial of order 1 or 2, so
    // multigrid takes its usual iterations.)
    Node source = m.make(QStringLiteral("heat/source"));
    source.props.insert(QStringLiteral("selection"), QJsonObject{{QStringLiteral("all"), true}});
    source.props.insert(QStringLiteral("kind"), QStringLiteral("density"));
    source.props.insert(QStringLiteral("Q0"), QStringLiteral("1e9[W/m^3]*(1 + sin(7*x/1[mm])*cos(5*y/1[mm]))"));
    m.add(ht, source);
    m.mesh().props.insert(QStringLiteral("custom"), true);
    m.mesh().props.insert(QStringLiteral("hmax"), hmax);
    const auto ps = evaluateParameters(m);
    const GeometryBuild3 g = buildGeometry3(m, *ps);
    if (!g.ok()) return 3;
    const Mesh3Build mb = buildMesh3(m, *ps, g.topology, {});
    if (!mb.mesh) return 4;
    std::shared_ptr<const Mesh3> mesh = mb.mesh;
    QElapsedTimer t;
    t.start();
    const std::vector<Estimate3> est = estimate3(m, mesh, solver);
    const double estimating = t.elapsed() / 1000.0;
    const double before = footprint();
    std::atomic<bool> done{false};
    std::atomic<double> most{before};
    std::thread sampler([&] {
        while (!done) {
            const double f = footprint();
            if (f > most) most = f;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    SolveAsked3 asked;
    asked.solver = solver;
    t.restart();
    const StudyResult3 r = solveStudy3(m, ps, mesh, {}, {}, asked);
    const double seconds = t.elapsed() / 1000.0;
    done = true;
    sampler.join();
    if (!r.solution) {
        std::fprintf(stderr, "%s\n", qPrintable(r.error));
        return 5;
    }
    const Estimate3& e = est.front();
    const double peak = most - before;
    std::printf("%s\t%s\t%s\t%lld\t%lld\t%lld\t%d\t%.4g\t%.4g\t%.4g\t%.4g\t%.3f\n", argv[1], argv[2], argv[3], (long long)e.unknowns,
                (long long)e.nonzeros, (long long)e.elements, e.perElement, e.bytes() / 1048576.0, peak / 1048576.0, e.seconds(), seconds,
                estimating);
    for (const QString& l : r.log)
        if (l.contains(QLatin1String("unknowns"))) std::fprintf(stderr, "  %s\n", qPrintable(l));
    return 0;
}
