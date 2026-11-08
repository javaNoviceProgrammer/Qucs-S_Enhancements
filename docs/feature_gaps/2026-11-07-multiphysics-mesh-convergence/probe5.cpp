// Adaptive rounds on the L corner with marking fractions: the error against the exact energy.
#include "fem_adapt.h"
#include "fem_solver.h"
#include "fem_mesh.h"
#include "fem_optimize.h"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>
using namespace qucs_s::fem;
int main(int argc,char**argv){
 QCoreApplication app(argc,argv); QFile f(argv[1]); if(!f.open(QIODevice::ReadOnly)) return 1; QString why; auto m=Model::fromJson(f.readAll(),&why);
 const double exact=atof(argv[2]); const double share=atof(argv[3]), least=atof(argv[4]);
 auto ps=evaluateParameters(*m); auto g=buildGeometry(*m,*ps); MeshSizes sz=meshSizes(*m,*ps,*g.topology,&why);
 std::shared_ptr<const Mesh> mesh=buildMesh(g.topology,sz).mesh;
 double n0=0,e0=0; 
 for(int r=0;r<40;++r){
  auto s=solveStudy(*m,ps,mesh,QString()); if(!s.solution){printf("fail %s\n",qPrintable(s.error));break;}
  double v; Dim d; QString w; solutionValue(*s.solution,"en",&v,&d,&w);
  double N=0; for(auto&fl:s.solution->fields()) if(fl.solved) N+=fl.space->size()*fl.count();
  double err=std::abs(v-exact)/exact; double hmin=1e9; for(size_t t=0;t<mesh->triangles.size();++t){auto&tr=mesh->triangles[t];auto A=mesh->nodes[tr[0]],B=mesh->nodes[tr[1]],C=mesh->nodes[tr[2]];hmin=std::min(hmin,std::sqrt(4*std::abs((B.x()-A.x())*(C.y()-A.y())-(C.x()-A.x())*(B.y()-A.y()))/2/std::sqrt(3.0)));}
  printf("  N=%8.0f err=%.3e hmin=%.2e rate=%s\n",N,err,hmin, r? qPrintable(QString::number(-std::log(err/e0)/std::log(N/n0),'f',2)):"");
  n0=N;e0=err; if(N>4e5||err<1e-7) break;
  auto ind=refinementIndicator(*s.solution); auto mk=markBulk(ind.eta,share,least); mesh=bisectMesh(*mesh,mk);
 }
}
