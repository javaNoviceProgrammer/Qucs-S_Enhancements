// One check's meshes, the estimate on each prefix, and the error made:
// probe6 file value tolerance refine exact [maxunknowns=2e6]
#include "fem_adapt.h"
#include "fem_solver.h"
#include "fem_mesh.h"
#include "fem_results.h"
#include "fem_optimize.h"
#include "fem_edge.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdio>
#include <functional>
using namespace qucs_s::fem;
int main(int argc,char**argv){
 QCoreApplication app(argc,argv);
 QFile f(argv[1]); if(!f.open(QIODevice::ReadOnly)) return 1; QString why; auto m0=Model::fromJson(f.readAll(),&why); if(!m0) return 1; m0->fileName=QFileInfo(f).absoluteFilePath();
 const QString value=argv[2]; const double tol=atof(argv[3]); const QString refine=argv[4]; const double exact=atof(argv[5]); const double maxu=argc>6?atof(argv[6]):2e6;
 Model m=*m0; for(Node& s:m.root.children) if(s.type=="study"){ s.children.erase(std::remove_if(s.children.begin(),s.children.end(),[](const Node&c){return c.type=="sweep";}),s.children.end()); for(Node& st:s.children){st.props.remove("adaptive");}}
 Node n=m.make("convergence"); n.props.insert("tolerance",QString::number(tol)); n.props.insert("refine",refine); n.props.insert("maxmeshes",40); n.props.insert("maxunknowns",QString::number(maxu)); n.props.insert("sweep","own");
 QJsonArray rows; rows.append(QJsonArray{value,"",""}); n.props.insert("values",rows);
 Node* a=m.add(m.root.tag,n,-1,nullptr); ConvergenceResult r=runConvergence(m,a->tag,evaluateParameters(m),nullptr,nullptr);
 std::vector<double> ns,qs; const int order=getenv("ORDER")?atoi(getenv("ORDER")):1;
 for(const ConvergenceRow& row:r.rows){ ns.push_back(row.unknowns); qs.push_back(row.values[0]); const ValueEstimate e=estimateValue(ns,qs,order);
  printf("  %-8s %9.0f  %.12g  err %.2e  said %.2e  %s\n", qPrintable(row.kind), row.unknowns, row.values[0], std::abs(row.values[0]-exact)/std::abs(exact), e.error/std::abs(exact), getenv("WHY")?qPrintable(e.why):"");}
 const auto& v=r.values[0];
 printf("%s %s tol %g: %s, %d solves, %.0f in all; made %.2e, said %.2e (%s), limit %.12g%s\n", argv[1], qPrintable(refine), tol, qPrintable(r.status), r.solves, r.unknownsSolved, std::abs(v.value-exact)/std::abs(exact), v.relative, qPrintable(v.trust), v.limit, r.converged && std::abs(v.value-exact)/std::abs(exact)>std::max(v.relative,0.0)?"  <-- SAID BELOW MADE":"");
}
