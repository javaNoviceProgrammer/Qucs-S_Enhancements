// Adaptive check against fem_converge's uniform route, on one model and value:
// probe3 file value tolerance [refine=auto] [exact]
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
static QString dividedBy(const QString& text, double factor){const QString t=text.trimmed(); if(t.isEmpty())return t; bool n=false; double v=t.toDouble(&n); if(n) return formatNumber(v/factor,6); return QStringLiteral("(%1)/%2").arg(t, formatNumber(factor,6));}
// (fem_converge's refineMesh, as in qucscontrol_fem.cpp)
static void refineMesh(Model& m, const Node& meshWas, double h0, double hmin0, double factor){
 Node* mesh=m.find(m.mesh().tag); *mesh=meshWas; const bool wasCustom=meshWas.props.value("custom").toBool();
 for(const char* key:{"hmin","growth","curvature","narrow"}) if(!wasCustom) mesh->props.remove(QLatin1String(key));
 mesh->props.insert("custom",true); mesh->props.insert("hmax",formatNumber(h0/factor,6));
 if(hmin0>0 && hmin0/factor<h0/factor) mesh->props.insert("hmin",formatNumber(hmin0/factor,6));
 std::function<void(Node&)> under=[&](Node& n){for(Node& c:n.children){ if(c.type=="mesh/size"||c.type=="mesh/layered") for(const char* key:{"hmax","hlateral","hwall"}){const QString was=c.props.value(QLatin1String(key)).toString(); if(!was.trimmed().isEmpty()) c.props.insert(QLatin1String(key),dividedBy(was,factor));}
  if(c.type=="mesh/distribution"||c.type=="mesh/layered") if(c.props.contains("count")){int count=c.props.value("count").toInt(); if(count>0) c.props.insert("count",std::clamp(qRound(count*factor),1,100000));}
  under(c);}};
 under(*mesh);
}
// (As fem_solve and the check count them.)
static double unknowns(const Solution& s){double d=0;for(const Field& f:s.fields())if(f.solved) d+=f.unknowns>=0?f.unknowns:(f.space?f.space->size()*f.count():0); return d;}
static ConvergenceResult adapt(Model m,const QString& value,double tol,const QString& refine,double maxu){
 Node n=m.make("convergence"); n.props.insert("tolerance",QString::number(tol)); n.props.insert("refine",refine); n.props.insert("maxmeshes",40); n.props.insert("maxunknowns",QString::number(maxu)); n.props.insert("sweep","own");
 QJsonArray rows; rows.append(QJsonArray{value,"",""}); n.props.insert("values",rows);
 Node* a=m.add(m.root.tag,n,-1,nullptr); return runConvergence(m,a->tag,evaluateParameters(m),nullptr,nullptr);
}
int main(int argc,char**argv){
 QCoreApplication app(argc,argv);
 QFile f(argv[1]); if(!f.open(QIODevice::ReadOnly)) return 1; QString why; auto m0=Model::fromJson(f.readAll(),&why); if(!m0) return 1; m0->fileName=QFileInfo(f).absoluteFilePath();
 const QString value=argv[2]; const double tol=atof(argv[3]); const QString refine=argc>4?argv[4]:"auto"; double exact=argc>5?atof(argv[5]):NAN;
 // Sweeps out: the model's own values, as the adaptive one with "own".
 Model m=*m0; for(Node& s:m.root.children) if(s.type=="study"){ s.children.erase(std::remove_if(s.children.begin(),s.children.end(),[](const Node&c){return c.type=="sweep";}),s.children.end()); for(Node& st:s.children){st.props.remove("adaptive");}}
 QElapsedTimer t;
 double refTime=0;
 if(std::isnan(exact)){ t.start(); ConvergenceResult ref=adapt(m,value,tol/30,refine,getenv("REFMAX")?atof(getenv("REFMAX")):4e6); refTime=t.elapsed()/1e3; exact=ref.values.empty()?NAN:ref.values[0].limit; printf("reference: %s, limit %.12g (tolerance %g, %d solves, %.0f unknowns largest, %.1f s)\n", qPrintable(ref.status), exact, tol/30, ref.solves, ref.largest, refTime);}
 else printf("reference: closed form %.12g\n",exact);
 t.restart(); ConvergenceResult a=adapt(m,value,tol,refine,2e6); const double ta=t.elapsed()/1e3;
 const double ea=a.values.empty()?NAN:std::abs(a.values[0].value-exact)/std::abs(exact);
 printf("ADAPTIVE (%s) tol %g: %s, %d solves%s, %.0f unknowns in all, largest %.0f, %.2f s; value %.10g, actual error %.2e, said %.2e (%s)\n", qPrintable(refine), tol, qPrintable(a.status), a.solves, a.adjoints?qPrintable(QStringLiteral(" +%1 adjoints").arg(a.adjoints)):"", a.unknownsSolved, a.largest, ta, a.values.empty()?NAN:a.values[0].value, ea, a.values.empty()?NAN:a.values[0].relative, a.values.empty()?"":qPrintable(a.values[0].trust));
 // fem_converge's route: factors 1, 2, 4, 8, 16 of every size.
 auto ps=evaluateParameters(m); auto g=buildGeometry(m,*ps); MeshSizes sz=meshSizes(m,*ps,*g.topology,&why);
 const Node meshWas=*m.find(m.mesh().tag);
 double sum=0; int solves=0; double prev=NAN, prevN=0; bool matched=false; double tsum=0; bool claimed=false;
 for(double fac:{1.0,2.0,4.0,8.0,16.0,32.0}){
  Model mm=m; if(fac>1) refineMesh(mm,meshWas,sz.hmax,sz.hmin,fac);
  t.restart(); auto b=buildMesh(mm,*ps,g.topology); if(!b.mesh){printf(" uniform f=%g: mesh failed\n",fac);break;}
  if(b.mesh->triangles.size()>2.5e6){printf(" uniform f=%g: %zu triangles, past the limit\n",fac,b.mesh->triangles.size());break;}
  StudyRun run=runStudy(mm,ps,g.topology,b.mesh,QString()); const double ts=t.elapsed()/1e3; tsum+=ts;
  if(!run.ok()){printf(" uniform f=%g: %s\n",fac,qPrintable(run.error.left(100)));break;}
  Solution& s=*run.solutions.front(); SolutionSet set; set.solutions.push_back(run.solutions.front());
  auto at=instances(set,false,0,{},{},modesAsked(set,"auto")); Solution* sel=select(set,at.back());
  double v=NAN; Dim d; QString w; solutionValue(*sel,value,&v,&d,&w);
  const double N=unknowns(s); sum+=N; ++solves;
  const double err=std::abs(v-exact)/std::abs(exact);
  const double moved=std::isnan(prev)?NAN:std::abs(v-prev)/std::abs(v); const bool finer=prevN>0 && N/prevN>=1.5;
  const bool says=!std::isnan(moved) && finer && moved<=tol;
  printf(" uniform f=%-3g %8.0f unknowns, %.2f s: value %.10g, actual error %.2e; fem_converge [..%g]: moved %.2e%s -> %s\n", fac, N, ts, v, err, fac, moved, finer?"":" (hardly finer)", says?"converged":"not converged");
  if(says && !claimed){claimed=true; printf("   fem_converge first says converged here, %d solves, %.0f unknowns in all, its value's actual error %.2e%s\n", solves, sum, err, err>tol?"  <-- WRONG: above the tolerance":"");}
  if(!matched && err<=ea){matched=true; printf("   uniform reaches the adaptive check's accuracy here: %d solves, %.0f unknowns in all (%.1fx), %.2f s (%.1fx)\n", solves, sum, sum/a.unknownsSolved, tsum, tsum/ta);}
  prev=v; prevN=N; if(matched && claimed) break;
  if(ts>60) break;
 }
 if(!matched) printf("   uniform did not reach the adaptive check's accuracy (%.2e) within %d solves, %.0f unknowns in all\n", ea, solves, sum);
}
