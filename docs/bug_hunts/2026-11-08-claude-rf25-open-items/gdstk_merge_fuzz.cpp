// gdstk::merge against touching rectangles: sets joined along edges, counted against what merge returns, and
// merge repeated (B1). Build: clang++ -std=c++17 -O2 -I<repo>/qucs-s-26.1.1/third_party/gdstk/include gdstk_merge_fuzz.cpp
// <build>/third_party/gdstk/libgdstk.a -lz
#include <gdstk/gdstk.hpp>
#include <cstdio>
#include <random>
#include <vector>
using namespace gdstk;
struct R { double x0,y0,x1,y1; };
static Polygon* rectp(const R& r){ Polygon* p=(Polygon*)allocate_clear(sizeof(Polygon)); Vec2 v; v.x=r.x0;v.y=r.y0;p->point_array.append(v); v.x=r.x1;p->point_array.append(v); v.y=r.y1;p->point_array.append(v); v.x=r.x0;p->point_array.append(v); return p;}
static int mergeCount(const std::vector<R>& rs, double scaling){ Array<Polygon*> in{}; for(auto&r:rs) in.append(rectp(r)); Array<Polygon*> out{}; merge(in,scaling,out); int n=(int)out.count; for(uint64_t i=0;i<out.count;i++){out[i]->clear();free_allocation(out[i]);} for(uint64_t i=0;i<in.count;i++){in[i]->clear();free_allocation(in[i]);} return n;}
static Array<Polygon*> mergeArr(Array<Polygon*>& in,double sc){ Array<Polygon*> out{}; merge(in,sc,out); return out;}
static void freeArr(Array<Polygon*>& a){ for(uint64_t i=0;i<a.count;i++){a[i]->clear();free_allocation(a[i]);} a.clear();}
static int remerge(const std::vector<R>& rs,double sc){ Array<Polygon*> in{}; for(auto&r:rs) in.append(rectp(r)); Array<Polygon*> o1=mergeArr(in,sc); Array<Polygon*> o2=mergeArr(o1,sc); int n=o2.count; freeArr(in);freeArr(o1);freeArr(o2); return n;}
static int closing(const std::vector<R>& rs,double sc){ Array<Polygon*> in{}; for(auto&r:rs) in.append(rectp(r)); Array<Polygon*> g{}; offset(in,1.0/sc,OffsetJoin::Miter,2,sc,true,g); Array<Polygon*> s{}; offset(g,-1.0/sc,OffsetJoin::Miter,2,sc,true,s); int n=s.count; double a=0; for(uint64_t i=0;i<s.count;i++) a+=s[i]->area(); double a0=0; for(auto&r:rs) a0+=(r.x1-r.x0)*(r.y1-r.y0); freeArr(in);freeArr(g);freeArr(s); return n;}
static int iter(const std::vector<R>& rs,double sc,int* passes){ Array<Polygon*> in{}; for(auto&r:rs) in.append(rectp(r)); Array<Polygon*> cur=mergeArr(in,sc); freeArr(in); *passes=1; for(int k=0;k<10;k++){ Array<Polygon*> nx=mergeArr(cur,sc); bool same=nx.count==cur.count; freeArr(cur); cur=nx; if(same) break; ++*passes;} int n=cur.count; freeArr(cur); return n;}
static bool touch(const R&a,const R&b){ // share an edge segment of positive length, or overlap
  double ox=std::min(a.x1,b.x1)-std::max(a.x0,b.x0), oy=std::min(a.y1,b.y1)-std::max(a.y0,b.y0);
  return (ox>0&&oy>=0)||(oy>0&&ox>=0);}
static int comps(const std::vector<R>& rs){ int n=rs.size(); std::vector<int> p(n); for(int i=0;i<n;i++)p[i]=i; auto f=[&](int x){while(p[x]!=x)x=p[x]=p[p[x]];return x;}; for(int i=0;i<n;i++)for(int j=i+1;j<n;j++) if(touch(rs[i],rs[j])) p[f(i)]=f(j); int c=0; for(int i=0;i<n;i++) if(f(i)==i)c++; return c;}
int main(){
  // A line with a stub touching its lower edge at no vertex of it.
  printf("line+stub: %d\n", mergeCount({{0,0,20000,700},{8000,-5000,12000,0}},1000));
  std::mt19937 g(1); int bad=0, tried=0;
  for(int t=0;t<300000;t++){
    int n=2+g()%7; std::vector<R> rs; rs.push_back({0,0,double(1+g()%40)*100,double(1+g()%40)*100});
    while((int)rs.size()<n){ const R& b=rs[g()%rs.size()]; int side=g()%4; double w=double(1+g()%40)*100,h=double(1+g()%40)*100; double off; R r;
      if(side<2){ off=b.x0+double(g()%41)*100- w/2; r={off, side==0? b.y1: b.y0-h, off+w, side==0? b.y1+h: b.y0}; }
      else { off=b.y0+double(g()%41)*100- h/2; r={side==2? b.x1: b.x0-w, off, side==2? b.x1+w: b.x0, off+h}; }
      rs.push_back(r);}
    tried++; int c=comps(rs), m=mergeCount(rs,1000); int ps; int mi=iter(rs,1000,&ps); if(m!=c) bad++; if(mi!=c) printf("ITER FAILS (%d passes)\n",ps); else if(ps>2) printf("needed %d passes\n",ps);
    if(0){ if(0)printf("case %d: %zu rects, %d connected pieces, merge gave %d\n",t,rs.size(),c,m); if(0)for(auto&r:rs) printf("  {%g,%g,%g,%g}\n",r.x0,r.y0,r.x1,r.y1);} }
  printf("tried %d, bad %d\n",tried,bad);
}
