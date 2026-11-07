// Times of one adaptive round's parts: indicator, marking, bisection, against the solve and a remesh.
#include "fem_adapt.h"
#include "fem_solver.h"
#include "fem_mesh.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <cstdio>
using namespace qucs_s::fem;
int main(int argc,char**argv){
 QCoreApplication app(argc,argv); QFile f(argv[1]); if(!f.open(QIODevice::ReadOnly)) return 1; QString why; auto m=Model::fromJson(f.readAll(),&why);
 auto ps=evaluateParameters(*m); auto g=buildGeometry(*m,*ps); MeshSizes sz=meshSizes(*m,*ps,*g.topology,&why);
 std::shared_ptr<const Mesh> mesh=buildMesh(g.topology,sz).mesh;
 // Up to about 100k triangles by cutting everything.
 while(mesh->triangles.size()<60000) mesh=bisectMesh(*mesh,std::vector<char>(mesh->triangles.size(),1),sz.periodic);
 QElapsedTimer t; t.start(); auto r=solveStudy(*m,ps,mesh,QString()); double ts=t.elapsed();
 t.restart(); auto ind=refinementIndicator(*r.solution); double ti=t.elapsed();
 t.restart(); auto mk=markBulk(ind.eta); double tm=t.elapsed();
 t.restart(); auto next=bisectMesh(*mesh,mk,sz.periodic); double tb=t.elapsed();
 // A remesh of the same marks, as the old step does.
 MeshSizes s2=sz; for(size_t k=0;k<mk.size();++k) if(mk[k]){const auto&v=mesh->triangles[k]; QPointF c=(mesh->nodes[v[0]]+mesh->nodes[v[1]]+mesh->nodes[v[2]])/3.0; double a=std::abs((mesh->nodes[v[1]].x()-mesh->nodes[v[0]].x())*(mesh->nodes[v[2]].y()-mesh->nodes[v[0]].y())-(mesh->nodes[v[2]].x()-mesh->nodes[v[0]].x())*(mesh->nodes[v[1]].y()-mesh->nodes[v[0]].y()))/2; double h=std::sqrt(4*a/std::sqrt(3.0))/2; s2.points.push_back({c.x(),c.y(),h}); s2.hmin=std::min(s2.hmin,h);}
 t.restart(); auto rm=buildMesh(g.topology,s2); double tr=t.elapsed();
 printf("%s: %zu triangles; solve %.0f ms, indicator %.0f ms, marking %.0f ms, bisection %.0f ms (to %zu); a remesh of the same marks %.0f ms (to %zu)\n",argv[1],mesh->triangles.size(),ts,ti,tm,tb,next->triangles.size(),tr,rm.mesh?rm.mesh->triangles.size():0);
}
