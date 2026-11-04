// Standalone check of src/math/PlasticMesh: clang++ -std=c++20 -I src tools/plasticmesh_check.cpp src/math/PlasticMesh.cpp -o /tmp/pmc && /tmp/pmc (writes main.txt / stadium.txt, the top-view triangles)
#include "math/PlasticMesh.h"
#include <cstdio>
#include <cmath>
#include <map>
#include <tuple>
#include <algorithm>
using namespace PlasticMesh;
static int fails=0;
#define CHECK(c,...) do{ if(!(c)){ fails++; printf("FAIL %s: ",#c); printf(__VA_ARGS__); printf("\n"); } }while(0)

static double triArea(const Vertex&a,const Vertex&b,const Vertex&c){ return 0.5*((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)); }
static double polyArea(const std::vector<CurvePoint>&p){ double a=0; for(size_t i=0;i<p.size();i++){auto&q=p[(i+1)%p.size()]; a+=p[i].x*q.y-q.x*p[i].y;} return 0.5*fabs(a);}
static double capArea(const Mesh&m,int&inv){ double a=0; inv=0; for(size_t t=0;t<m.idx.size();t+=3){ double ar=triArea(m.verts[m.idx[t]],m.verts[m.idx[t+1]],m.verts[m.idx[t+2]]); a+=ar; if(ar<-1e-6) inv++; } return a; }

struct Key{ long x,y,z; bool operator<(const Key&o)const{return std::tie(x,y,z)<std::tie(o.x,o.y,o.z);} };
static Key K(const Vertex&v){ return {lround(v.x*1000),lround(v.y*1000),lround(v.z*1000)}; }

// all meshes welded by position: every undirected edge shared by exactly two triangles. The bottom cap faces up like the top (freezy's rule), so
// it runs the same way round as the walls at the shared rim; everything else (top cap + bevel) must be consistently oriented, open only at the z=height rim.
static void manifold(const char*name,std::vector<const Mesh*> ms,bool expectClosed){
  std::map<std::pair<Key,Key>,int> u;
  int degenerate=0;
  for(auto m:ms) for(size_t t=0;t<m->idx.size();t+=3){
    Key k[3]; for(int i=0;i<3;i++) k[i]=K(m->verts[m->idx[t+i]]);
    for(int i=0;i<3;i++){ auto a=k[i],b=k[(i+1)%3]; if(!(a<b)&&!(b<a)){degenerate++;continue;} if(b<a) std::swap(a,b); u[{a,b}]++; }
  }
  int bad=0; for(auto&kv:u) if(kv.second!=2) bad++;
  // top + bevel only (ms[1], ms[2]) directed consistency
  int rimOpen=0, otherOpen=0;
  if(ms.size()==3){ std::map<std::pair<Key,Key>,int> e;
    for(int mi=1;mi<3;mi++) for(size_t t=0;t<ms[mi]->idx.size();t+=3){ Key k[3]; for(int i=0;i<3;i++) k[i]=K(ms[mi]->verts[ms[mi]->idx[t+i]]); for(int i=0;i<3;i++){ if(!(k[i]<k[(i+1)%3])&&!(k[(i+1)%3]<k[i])) continue; e[{k[i],k[(i+1)%3]}]++; } }
    for(auto&kv:e){ if(kv.second>1) otherOpen++; if(!e.count({kv.first.second,kv.first.first})){ if(kv.first.first.z==50000&&kv.first.second.z==50000) rimOpen++; else otherOpen++; } } }
  printf("%s: %zu edges, not-shared-by-2: %d, zero-length: %d, top+bevel orientation breaks off the rim %d (rim edges %d)\n",name,u.size(),bad,degenerate,otherOpen,rimOpen);
  if(expectClosed) CHECK(bad==0&&otherOpen==0,"%s not a closed manifold",name);
}
static void windingVsNormals(const char*n,const Mesh&m){
  int bad=0; for(size_t t=0;t<m.idx.size();t+=3){ auto&a=m.verts[m.idx[t]],&b=m.verts[m.idx[t+1]],&c=m.verts[m.idx[t+2]];
    double ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z,wx=c.x-a.x,wy=c.y-a.y,wz=c.z-a.z;
    double cx=uy*wz-uz*wy,cy=uz*wx-ux*wz,cz=ux*wy-uy*wx; double l=sqrt(cx*cx+cy*cy+cz*cz); if(l<1e-9) continue;
    double d=(cx*(a.nx+b.nx+c.nx)+cy*(a.ny+b.ny+c.ny)+cz*(a.nz+b.nz+c.nz))/l; if(d<=0) bad++; }
  CHECK(bad==0,"%s: %d triangles wound against their normals",n,bad); printf("%s winding ok (%zu tris)\n",n,m.idx.size()/3);
}

static void dump(const char*f,const Result&r,const std::vector<CurvePoint>&flat){
  FILE*o=fopen(f,"w"); for(auto&p:flat) fprintf(o,"O %f %f\n",p.x,p.y);
  auto d=[&](const char*tag,const Mesh&m){ for(size_t t=0;t<m.idx.size();t+=3) fprintf(o,"%s %f %f %f %f %f %f\n",tag,m.verts[m.idx[t]].x,m.verts[m.idx[t]].y,m.verts[m.idx[t+1]].x,m.verts[m.idx[t+1]].y,m.verts[m.idx[t+2]].x,m.verts[m.idx[t+2]].y); };
  d("B",r.bottom); d("T",r.top); d("W",r.bevel); fclose(o); }

int main(){ setbuf(stdout,0);
  std::vector<CurvePoint> ol={{0,0,false},{100,0,false},{130,40,true},{100,80,false},{60,50,false},{0,80,false},{-20,40,true}};
  std::vector<Hole> holes={{20,20,12},{80,22,6.4f}};
  // arc flatten error: arc through (100,0),(130,40),(100,80)
  {
    auto fl=FlattenOutline(ol,0.25f);
    // circle through the 3 points
    double ax=100,ay=0,bx=130,by=40,cx=100,cy=80; double d=2*(ax*(by-cy)+bx*(cy-ay)+cx*(ay-by));
    double ux=((ax*ax+ay*ay)*(by-cy)+(bx*bx+by*by)*(cy-ay)+(cx*cx+cy*cy)*(ay-by))/d, uy=((ax*ax+ay*ay)*(cx-bx)+(bx*bx+by*by)*(ax-cx)+(cx*cx+cy*cy)*(bx-ax))/d;
    double R=hypot(ax-ux,ay-uy); double maxOff=0,maxSag=0; int on=0;
    for(size_t i=0;i<fl.size();i++){ auto&p=fl[i],&q=fl[(i+1)%fl.size()];
      if(fabs(hypot(p.x-ux,p.y-uy)-R)<1e-2 && p.x>=100-1e-3 && p.x<=130+1e-3){ on++; }
      if(fabs(hypot(p.x-ux,p.y-uy)-R)<1e-2 && fabs(hypot(q.x-ux,q.y-uy)-R)<1e-2){ double c2=hypot(q.x-p.x,q.y-p.y); double sag=R-sqrt(std::max(0.0,R*R-c2*c2/4)); maxSag=std::max(maxSag,sag);} }
    printf("arc R=%.2f, flattened pts on circle %d, max sagitta %.4f\n",R,on,maxSag);
    CHECK(maxSag<=0.25+1e-4,"sagitta %f",maxSag); CHECK(on>=5,"arc not flattened");
    // closure: first point of arc equals corner (100,0)
    bool has=false; for(auto&p:fl) if(fabs(p.x-100)<1e-3&&fabs(p.y)<1e-3) has=true; CHECK(has,"tangent corner kept");
  }
  Params P; Result r;
  CHECK(Build(ol,holes,P,r),"build");
  auto flat=FlattenOutline(ol,0.25f);
  double holesA=0; for(auto&h:holes){ double rr=h.diameter/2; int n=0; /* polygonal area */ }
  // polygon hole area: recompute from the bottom cap difference below
  int inv; double aB=capArea(r.bottom,inv); printf("bottom cap area %.3f inverted %d, outline area %.3f\n",aB,inv,polyArea(flat));
  CHECK(inv==0,"inverted bottom"); 
  double aT=capArea(r.top,inv); CHECK(inv==0,"inverted top"); printf("top cap area %.3f (inset)\n",aT);
  CHECK(aT<aB&&aT>0,"inset area");
  // hole polygons: the bottom area = outline - sum polygon holes; polygon circle area <= pi r^2 within flatten
  double circles=0; for(auto&h:holes) circles+=M_PI*h.diameter*h.diameter/4;
  printf("outline - circles = %.3f ; bottom cap = %.3f ; diff %.3f\n",polyArea(flat)-circles,aB,aB-(polyArea(flat)-circles));
  
  { double hp=0; for(auto&h:holes){ double rr=h.diameter/2; int n=std::max(12,(int)ceil(2*M_PI/std::max(2*acos(std::max(-1.0,1.0-0.25/rr)),1e-3))); hp+=0.5*n*rr*rr*sin(2*M_PI/n); double sag=rr-rr*cos(M_PI/n); printf("hole d=%.1f n=%d sagitta %.3f\n",h.diameter,n,sag); CHECK(sag<=0.25,"hole sag"); }
    printf("bottom cap vs outline - polygon holes: %.4f\n",aB-(polyArea(flat)-hp)); CHECK(fabs(aB-(polyArea(flat)-hp))<0.01,"bottom area"); }
  // bevel 0: top cap == bottom cap exactly
  { Params q=P; q.bevel=0; Result r0; CHECK(Build(ol,holes,q,r0),"build b0"); int i2; double a0=capArea(r0.top,i2); printf("bevel 0: top %.4f bottom %.4f\n",a0,capArea(r0.bottom,i2)); CHECK(fabs(a0-capArea(r0.bottom,i2))<1e-3,"b0 areas"); manifold("bevel0 all",{&r0.bottom,&r0.top,&r0.bevel},true);}
  manifold("all three",{&r.bottom,&r.top,&r.bevel},true);
  manifold("chamfer all",{},true);
  windingVsNormals("bottom",r.bottom); windingVsNormals("top",r.top); windingVsNormals("bevel",r.bevel);
  for(int segs:{1,2,4}){ Params q=P; q.bevelSegments=segs; Result rr; CHECK(Build(ol,holes,q,rr),"b"); char nm[40]; sprintf(nm,"segs %d",segs); manifold(nm,{&rr.bottom,&rr.top,&rr.bevel},true); windingVsNormals(nm,rr.bevel);}
  // normals unit length; z extent
  float zmin=1e9,zmax=-1e9; double nerr=0; for(auto*m:{&r.bottom,&r.top,&r.bevel}) for(auto&v:m->verts){ zmin=std::min(zmin,v.z); zmax=std::max(zmax,v.z); nerr=std::max<double>(nerr,fabs(sqrt(v.nx*v.nx+v.ny*v.ny+v.nz*v.nz)-1)); }
  printf("z %.2f..%.2f, max normal length error %.2e\n",zmin,zmax,nerr); CHECK(fabsf(zmin-50)<1e-4&&fabsf(zmax-55.6f)<1e-3,"z range"); CHECK(nerr<1e-4,"normals");
  // outline must not grow: bevel verts within original polygon bounds
  { float mnx=1e9; for(auto&v:r.bevel.verts) mnx=std::min(mnx,v.x); float omn=1e9; for(auto&p:flat) omn=std::min(omn,p.x); CHECK(mnx>=omn-1e-3,"part grew %f < %f",mnx,omn);}  
  // two consecutive smooth points + a third polygon: stadium-like
  { std::vector<CurvePoint> st={{0,0,false},{60,0,false},{80,20,true},{60,40,true},{0,40,false},{-20,20,true}};
    Result s; CHECK(Build(st,{},P,s),"stadium"); manifold("stadium",{&s.bottom,&s.top,&s.bevel},true); int i3; printf("stadium bottom area %.2f vs polygon %.2f inv %d\n",capArea(s.bottom,i3),polyArea(FlattenOutline(st,0.25f)),i3); CHECK(i3==0,"inv"); dump("stadium.txt",s,FlattenOutline(st,0.25f)); }
  // clockwise input outline and a hole outside
  { std::vector<CurvePoint> cw(ol.rbegin(),ol.rend()); std::vector<Hole> hh=holes; hh.push_back({300,300,10}); Result s; CHECK(Build(cw,hh,P,s),"cw"); CHECK(s.skippedHoles==1,"skipped"); manifold("cw outline",{&s.bottom,&s.top,&s.bevel},true);}  
  // degenerate
  { Result s; CHECK(!Build({{0,0,false},{1,1,false}},{},P,s),"2 pts"); }
  dump("main.txt",r,flat);
  printf(fails?"FAILED %d\n":"ALL OK\n",fails); return fails?1:0; }
