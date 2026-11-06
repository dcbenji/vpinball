// Standalone check of src/math/PlasticMesh: clang++ -std=c++20 -I src tools/plasticmesh_check.cpp src/math/PlasticMesh.cpp -o /tmp/pmc && /tmp/pmc (writes main.txt / stadium.txt, the top-view triangles)
// Also build it with -O2 -ffast-math (the engine's flags). `pmc --sig` prints the plastic scenario signatures table (kBase) for the current PlasticMesh.cpp.
#include "math/PlasticMesh.h"
#include <cstdio>
#include <cmath>
#include <map>
#include <tuple>
#include <algorithm>
#include <cstring>
#include <ctime>
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


// ---- before/after earcut: the plastic scenarios built with the old ear clipper gave these signatures (vertex bytes FNV-1a, triangle count, cap area).
// Vertices (cap rings, walls) must be byte-identical; the cap triangles may be a different triangulation of the same polygon (same count, same area).
struct Sig { const char*name; unsigned long long vh[3]; size_t tc[3]; double area[2]; };
static unsigned long long fnv(const Mesh&m){ unsigned long long h=1469598103934665603ULL; const unsigned char*b=(const unsigned char*)m.verts.data(); for(size_t i=0;i<m.verts.size()*sizeof(Vertex);i++){ h^=b[i]; h*=1099511628211ULL; } return h; }
static std::vector<std::pair<const char*,Result>> plasticScenarios(){
  std::vector<CurvePoint> ol={{0,0,false},{100,0,false},{130,40,true},{100,80,false},{60,50,false},{0,80,false},{-20,40,true}};
  std::vector<CurvePoint> st={{0,0,false},{60,0,false},{80,20,true},{60,40,true},{0,40,false},{-20,20,true}};
  Params P; std::vector<std::pair<const char*,Result>> v; auto add=[&](const char*n,const std::vector<CurvePoint>&o,std::vector<Hole> h,Params q){ Result r; if(!Build(o,h,q,r)) printf("scenario %s failed to build\n",n); v.push_back({n,std::move(r)}); };
  add("main",ol,{{20,20,12},{80,22,6.4f}},P);
  add("noholes",st,{},P);
  Params b0=P; b0.bevel=0; add("bevel0",ol,{{20,20,12},{80,22,6.4f}},b0);
  Params d5=P; d5.flatten=FlattenTolerance(5); add("detail5",ol,{{20,20,12},{80,22,6.4f}},d5);
  Hole pl{40,35,6.4f}; pl.x2=70; pl.y2=45; pl.pill=true; add("pill",ol,{pl,{20,20,12}},P);
  add("many",ol,{{20,20,6},{40,20,6},{60,20,6},{80,20,6},{20,40,6},{40,60,6},{80,50,6},{3,20,10},{100,50,300}},P);
  add("band",ol,{{50,5,6}},P);
  Params ch=P; ch.bevelSegments=1; add("chamfer",st,{{10,20,8},{40,20,8}},ch);
  return v;
}
// Generated by the check's --sig mode on the pre-earcut PlasticMesh.cpp (HEAD 4a88e604a), clang arm64; the vertex hashes depend on the float math, hence one table per mode.
// "many" and "band" (holes whose bevel the outline clamps) regenerated for the crack fix: their bevel rings now use the clamped bevel; bottom and top unchanged.
#ifdef __FAST_MATH__
static const Sig kBase[]={
{"main",{0x75a5a29d66dd6c64ULL,0x559a1e14b3a9e9d2ULL,0x2862a8d32b57b3dcULL},{51,51,294},{9216.8688,8304.9961}},
{"noholes",{0x4247df2c03463aabULL,0x1e063ec555fdc1abULL,0x23afe976eaa34075ULL},{26,26,168},{4003.7097,3539.3624}},
{"bevel0",{0x75a5a29d66dd6c64ULL,0xc1ba9c046a9f8eb6ULL,0xbdd71117e81d1b23ULL},{51,51,98},{9216.8688,9216.8688}},
{"detail5",{0x540f6085a8abed4fULL,0xf3484028f84ef067ULL,0xe4e50be4a648530fULL},{206,206,1224},{9239.7504,8326.4395}},
{"pill",{0xcc277084336940e8ULL,0x599d51aa4d70240aULL,0x3692d055a7061cb3ULL},{53,53,306},{9014.4833,7982.4773}},
{"many",{0x499f65d6b87721ffULL,0xc6479c7288466013ULL,0x4332e03f3d1480b6ULL},{121,121,764},{9118.5891,7985.8085}},
{"band",{0x954839c17a544d28ULL,0x59822862fa5a1366ULL,0x4cfa0af2d05e069aULL},{37,37,174},{9328.5889,8548.5106}},
{"chamfer",{0x204758f263b28483ULL,0x9c1ceec84f98165fULL,0xc0d2b6643d158e39ULL},{54,54,208},{3907.7098,3325.7302}},
};
#else
static const Sig kBase[]={
{"main",{0x75a5a29d66dd6c64ULL,0x559a1e14b3a9e9d2ULL,0x138c9bdcd47e8a59ULL},{51,51,294},{9216.8688,8304.9961}},
{"noholes",{0x4247df2c03463aabULL,0x1e063ec555fdc1abULL,0x23afe976eaa34075ULL},{26,26,168},{4003.7097,3539.3624}},
{"bevel0",{0x75a5a29d66dd6c64ULL,0xc1ba9c046a9f8eb6ULL,0x39764f56f7a7b809ULL},{51,51,98},{9216.8688,9216.8688}},
{"detail5",{0x540f6085a8abed4fULL,0xf3484028f84ef067ULL,0xe4e50be4a648530fULL},{206,206,1224},{9239.7504,8326.4395}},
{"pill",{0xcc277084336940e8ULL,0x599d51aa4d70240aULL,0x8bc65a7ae53490e9ULL},{53,53,306},{9014.4833,7982.4773}},
{"many",{0x499f65d6b87721ffULL,0xc6479c7288466013ULL,0xc6123d006dd624dcULL},{121,121,764},{9118.5891,7985.8085}},
{"band",{0x954839c17a544d28ULL,0x59822862fa5a1366ULL,0x44fa04e8a2c213ddULL},{37,37,174},{9328.5889,8548.5106}},
{"chamfer",{0x204758f263b28483ULL,0x9c1ceec84f98165fULL,0x915946eea4512ca3ULL},{54,54,208},{3907.7098,3325.7302}},
};
#endif


// NaN / Inf by bit pattern: under -ffast-math x != x and isfinite are folded away
static bool badBits(float f){ unsigned u; memcpy(&u,&f,4); return ((u>>23)&0xff)==0xff; }
static int badVerts(const Mesh&m){ int n=0; for(auto&v:m.verts) for(float f:{v.x,v.y,v.z,v.nx,v.ny,v.nz,v.tu,v.tv}) if(badBits(f)){n++;break;} return n; }
static double ringArea(const std::vector<CurvePoint>&p){ double a=0; for(size_t i=0;i<p.size();i++){auto&q=p[(i+1)%p.size()]; a+=p[i].x*q.y-q.x*p[i].y;} return 0.5*a; }
// Area of the polygon grown outward by d with mitred corners (Steiner): A + P d + d^2 sum tan(turn/2), turns signed relative to the polygon's own winding.
static double grownArea(std::vector<CurvePoint> p,double d){ if(ringArea(p)<0) std::reverse(p.begin(),p.end()); double A=ringArea(p),P=0,T=0; size_t n=p.size();
  for(size_t i=0;i<n;i++){ auto&a=p[(i+n-1)%n],&b=p[i],&c=p[(i+1)%n]; double ux=b.x-a.x,uy=b.y-a.y,vx=c.x-b.x,vy=c.y-b.y; P+=hypot(vx,vy); T+=tan(0.5*atan2(ux*vy-uy*vx,ux*vx+uy*vy)); }
  return A+P*d+d*d*T; }
static int idxOutOfRange(const Mesh&m){ int n=0; for(unsigned i:m.idx) if(i>=m.verts.size()) n++; return n; }

static void dump(const char*f,const Result&r,const std::vector<CurvePoint>&flat){
  FILE*o=fopen(f,"w"); for(auto&p:flat) fprintf(o,"O %f %f\n",p.x,p.y);
  auto d=[&](const char*tag,const Mesh&m){ for(size_t t=0;t<m.idx.size();t+=3) fprintf(o,"%s %f %f %f %f %f %f\n",tag,m.verts[m.idx[t]].x,m.verts[m.idx[t]].y,m.verts[m.idx[t+1]].x,m.verts[m.idx[t+1]].y,m.verts[m.idx[t+2]].x,m.verts[m.idx[t+2]].y); };
  d("B",r.bottom); d("T",r.top); d("W",r.bevel); fclose(o); }

int main(int argc,char**argv){ setbuf(stdout,0);
  if(argc>1&&!strcmp(argv[1],"--sig")){ for(auto&sc:plasticScenarios()){ int i0,i1; const Result&r=sc.second; printf("{\"%s\",{0x%llxULL,0x%llxULL,0x%llxULL},{%zu,%zu,%zu},{%.4f,%.4f}},\n",sc.first,fnv(r.bottom),fnv(r.top),fnv(r.bevel),r.bottom.idx.size()/3,r.top.idx.size()/3,r.bevel.idx.size()/3,capArea(r.bottom,i0),capArea(r.top,i1)); } return 0; }

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
  double aT=capArea(r.top,inv); printf("top cap area %.3f (inset)\n",aT);
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
    Result s; CHECK(Build(st,{},P,s),"stadium"); manifold("stadium",{&s.bottom,&s.top,&s.bevel},true); int i3; printf("stadium bottom area %.2f vs polygon %.2f inv %d\n",capArea(s.bottom,i3),polyArea(FlattenOutline(st,0.25f)),i3); CHECK(fabs(capArea(s.bottom,i3)-polyArea(FlattenOutline(st,0.25f)))<1e-2,"stadium bottom area"); dump("stadium.txt",s,FlattenOutline(st,0.25f)); }
  // clockwise input outline and a hole outside
  { std::vector<CurvePoint> cw(ol.rbegin(),ol.rend()); std::vector<Hole> hh=holes; hh.push_back({300,300,10}); Result s; CHECK(Build(cw,hh,P,s),"cw"); CHECK(s.skippedHoles==1,"skipped"); manifold("cw outline",{&s.bottom,&s.top,&s.bevel},true);}  
  // holes inside the outline but inside the band r + 2*bevel of the edge (the top cap's inset rings meet there): built, counted, top cap not inverted
  for(float y:{9.f,6.f,5.f,4.f,3.5f}){ std::vector<Hole> hb={{50,y,6}}; Result s; bool ok=Build(ol,hb,P,s); int inv=-1; double ta=ok?capArea(s.top,inv):0; Result s0; Build(ol,{},P,s0); int i0; double top0=capArea(s0.top,i0); double dg=std::max(0.0,std::min((double)P.bevel,y-P.bevel-3.0-0.1)); double want=top0-grownArea(HoleOutline(Hole{50,y,6},0.25f),dg);
    printf("band hole y=%.1f (r 3, bevel %.2f): build %d crossing %d, top area %.2f, expected %.2f (no-hole top %.2f minus the hole grown by %.2f)\n",y,P.bevel,ok,s.crossingHoles,ta,want,top0,dg); CHECK(ok,"band build"); CHECK((y<6.8f)==(s.crossingHoles==1),"band counted y=%.1f",y); if(y>=5.f) CHECK(fabs(ta-want)<0.5,"band hole y=%.1f: top area %.2f, expected %.2f",y,ta,want); // y < r+bevel (4.9): the rings still overlap, warned only; earcut caps part of the hole there, so no area is asserted
    if(ok) manifold("band hole",{&s.bottom,&s.top,&s.bevel},false); }
  // holes crossing the outline edge, overlapping each other, bigger than the part, straddling a corner: counted, built, terminates
  { std::vector<Hole> hc={{20,20,12},{3,20,10},{100,50,300},{0.5f,0.5f,8},{30,20,12},{30,20,12}}; Result s; auto t0=clock(); bool ok=Build(ol,hc,P,s); printf("crossing holes: build %d crossing %d skipped %d, %.0f ms\n",ok,s.crossingHoles,s.skippedHoles,1000.0*(clock()-t0)/CLOCKS_PER_SEC); CHECK(s.crossingHoles>=2,"crossing counted"); CHECK(clock()-t0<5*CLOCKS_PER_SEC,"hang");
    Result n; CHECK(Build(ol,holes,P,n)&&n.crossingHoles==0,"no false crossing"); }
  // Pills: a capsule hole through all three meshes, manifold, no inverted caps; capsule loop clockwise and as long as it should be
  { Params q=P; std::vector<Hole> hp; Hole pl{40,35,6.4f}; pl.x2=70; pl.y2=45; pl.pill=true; hp.push_back(pl); Hole rd{20,20,12}; hp.push_back(rd);
    Result s; CHECK(Build(ol,hp,q,s),"pill build"); manifold("pill",{&s.bottom,&s.top,&s.bevel},true); windingVsNormals("pill bevel",s.bevel);
    int iv; double aB2=capArea(s.bottom,iv); 
    auto loop=HoleOutline(pl,0.25f); double la=0; for(size_t i=0;i<loop.size();i++){auto&a=loop[i],&b=loop[(i+1)%loop.size()]; la+=a.x*b.y-b.x*a.y;} la*=0.5;
    double L=hypot(30.0,10.0),r=3.2; printf("pill loop %zu pts, signed area %.3f (capsule %.3f)\n",loop.size(),la,L*2*r+M_PI*r*r);
    CHECK(la<0,"capsule loop is clockwise"); CHECK(fabs(-la-(L*2*r+M_PI*r*r))<1.6,"capsule area (a coarse polygon at tolerance 0.25)");
    // the hole is open: bottom area = outline - capsule polygon - round hole polygon
    auto rl=HoleOutline(rd,0.25f); double ra=0; for(size_t i=0;i<rl.size();i++){auto&a=rl[i],&b=rl[(i+1)%rl.size()]; ra+=a.x*b.y-b.x*a.y;} ra=-0.5*ra;
    printf("pill bottom %.3f vs outline - holes %.3f\n",aB2,polyArea(flat)+la-ra); CHECK(fabs(aB2-(polyArea(flat)+la-ra))<0.01,"pill bottom area");
    // zero-length or no pill flag: a round hole, identical to the round build
    Hole z=pl; z.x2=z.x; z.y2=z.y; Hole nf=pl; nf.pill=false; Result r1,r2,r3; Build(ol,{Hole{40,35,6.4f}},P,r1); Build(ol,{z},P,r2); Build(ol,{nf},P,r3);
    CHECK(r1.bottom.verts.size()==r2.bottom.verts.size()&&r1.bottom.verts.size()==r3.bottom.verts.size(),"degenerate / unflagged pill reads as round");
    // outside test: one end outside, middle inside is kept; both ends and the middle outside is skipped; the edge warning uses the segment
    Hole half=pl; half.x=-30; half.y=40; half.x2=30; half.y2=40; Result s2; Build(ol,{half},P,s2); CHECK(s2.skippedHoles==0,"pill with one end outside kept");
    Hole away=pl; away.x=300; away.x2=330; Result s3; Build(ol,{away},P,s3); CHECK(s3.skippedHoles==1,"pill outside skipped");
    Hole near=pl; near.x=10; near.y=2; near.x2=90; near.y2=2; Result s4; Build(ol,{near},P,s4); printf("pill along the edge: crossing %d\n",s4.crossingHoles); CHECK(s4.crossingHoles==1,"segment-distance edge warning");
  }
  // Curve Detail: tolerance 0.25 / n^2, detail 1 identical to the default, more detail = more triangles, never above 96 segments on one arc or hole circle
  { printf("tolerances: "); for(int d=1;d<=5;d++) printf("%d:%.4f ",d,FlattenTolerance(d)); printf("\n"); CHECK(FlattenTolerance(1)==0.25f&&fabsf(FlattenTolerance(5)-0.01f)<1e-6f,"tolerances");
    size_t prev=0; for(int d=1;d<=5;d++){ Params q=P; q.flatten=FlattenTolerance(d); Result s; CHECK(Build(ol,holes,q,s),"detail build"); size_t tc=(s.bottom.idx.size()+s.top.idx.size()+s.bevel.idx.size())/3;
      printf("detail %d: %zu triangles (bottom %zu top %zu bevel %zu)\n",d,tc,s.bottom.idx.size()/3,s.top.idx.size()/3,s.bevel.idx.size()/3); CHECK(tc>prev,"more detail, more triangles"); prev=tc; manifold("detail",{&s.bottom,&s.top,&s.bevel},true);
      auto fl=FlattenOutline(ol,q.flatten); (void)fl; }
    Result d1; Build(ol,holes,P,d1); Params q1=P; q1.flatten=FlattenTolerance(1); Result d1b; Build(ol,holes,q1,d1b);
    CHECK(d1.bottom.verts.size()==d1b.bottom.verts.size()&&memcmp(d1.bottom.verts.data(),d1b.bottom.verts.data(),d1.bottom.verts.size()*sizeof(Vertex))==0&&d1.bevel.idx==d1b.bevel.idx,"detail 1 == default");
    // the cap: a huge arc and a huge hole at the finest detail
    std::vector<CurvePoint> big={{0,0,false},{1000,0,false},{1500,500,true},{1000,1000,false},{0,1000,false}}; Hole bh{200,500,600}; Hole bp{600,500,300}; bp.x2=900; bp.y2=500; bp.pill=true;
    for(int d:{1,5}){ float t=FlattenTolerance(d); auto fl=FlattenOutline(big,t); size_t arcPts=0; for(auto&p:fl) if(p.x>1000.5f) arcPts++; auto hl=HoleOutline(bh,t); auto pl2=HoleOutline(bp,t);
      printf("detail %d huge: arc points beyond the corners %zu, hole circle %zu pts, pill %zu pts\n",d,arcPts,hl.size(),pl2.size()); CHECK(arcPts<=96,"arc cap"); CHECK(hl.size()<=96,"hole cap"); CHECK(pl2.size()<=96,"pill cap"); }
    // a small hole at detail 5 is not capped
    auto sh=HoleOutline(Hole{0,0,12},FlattenTolerance(5)); printf("12 VPU hole at detail 5: %zu pts\n",sh.size()); CHECK(sh.size()>40&&sh.size()<=96,"small hole fine detail");
  }

  // ---- earcut: plastics before/after. Vertex streams byte-identical to the old ear-clipping build, same triangle counts, same cap areas.
  { auto sc=plasticScenarios(); CHECK(sc.size()==sizeof(kBase)/sizeof(kBase[0]),"scenario count");
    for(size_t i=0;i<sc.size()&&i<sizeof(kBase)/sizeof(kBase[0]);i++){ const Result&q=sc[i].second; const Sig&b=kBase[i]; int a0,a1; double ab=capArea(q.bottom,a0),at=capArea(q.top,a1);
      bool vh=fnv(q.bottom)==b.vh[0]&&fnv(q.top)==b.vh[1]&&fnv(q.bevel)==b.vh[2], tc=q.bottom.idx.size()/3==b.tc[0]&&q.top.idx.size()/3==b.tc[1]&&q.bevel.idx.size()/3==b.tc[2];
      // the "band"/"many" scenarios have overlapping or edge-crossing holes (invalid input); there only the vertex bytes and counts are compared
      bool strict=strcmp(sc[i].first,"many")&&strcmp(sc[i].first,"band"); bool ar=fabs(ab-b.area[0])<1e-2&&fabs(at-b.area[1])<1e-2;
      printf("earcut vs old %-8s verts %s, tri counts %s, areas %.4f/%.4f (old %.4f/%.4f)%s inverted %d/%d\n",sc[i].first,vh?"identical":"DIFFER",tc?"same":"DIFFER",ab,at,b.area[0],b.area[1],ar?"":" DIFFER",a0,a1);
      CHECK(vh,"%s vertex bytes changed",sc[i].first); CHECK(tc,"%s triangle counts changed",sc[i].first); if(strict) CHECK(ar,"%s cap area changed",sc[i].first);
      manifold(sc[i].first,{&q.bottom,&q.top,&q.bevel},strict); } }

  // ---- polygon holes
  { std::vector<CurvePoint> L={{30,20,false},{50,20,false},{50,28,false},{38,28,false},{38,50,false},{30,50,false}}; // L shape, counter-clockwise
    std::vector<CurvePoint> Lcw(L.rbegin(),L.rend()); std::vector<CurvePoint> Ldup=L; Ldup.insert(Ldup.begin()+2,Ldup[1]); Ldup.push_back(Ldup[0]);
    double la=fabs(ringArea(L)); auto flat2=FlattenOutline(ol,0.25f); double oa=polyArea(flat2);
    Result ref; Hole rd{80,22,6.4f}; Build(ol,{rd},P,ref); int iv;
    for(int variant=0;variant<3;variant++){ Hole ph; ph.polygon=variant==0?L:variant==1?Lcw:Ldup; Result s; CHECK(Build(ol,{ph,rd},P,s),"polygon build"); const char*nm=variant==0?"polygon hole ccw":variant==1?"polygon hole cw":"polygon hole dups";
      double ab=capArea(s.bottom,iv); double at=capArea(s.top,iv); int r2; double topRef=capArea(ref.top,r2); double wantTop=topRef-grownArea(L,P.bevel); printf("%s: top %.3f expected %.3f\n",nm,at,wantTop); CHECK(fabs(at-wantTop)<0.05,"%s top area %.3f expected %.3f",nm,at,wantTop);
      int a2; double rb=capArea(ref.bottom,a2); printf("%s: bottom %.3f = outline-with-round %.3f - L %.3f (diff %.5f), skipped %d crossing %d, nan %d\n",nm,ab,rb,la,ab-(rb-la),s.skippedHoles,s.crossingHoles,badVerts(s.bottom)+badVerts(s.top)+badVerts(s.bevel));
      CHECK(fabs(ab-(rb-la))<1e-2,"%s bottom area",nm); CHECK(s.skippedHoles==0&&s.crossingHoles==0,"%s counts",nm); CHECK(at>0&&at<ab,"%s top inset",nm);
      CHECK(badVerts(s.bottom)+badVerts(s.top)+badVerts(s.bevel)==0,"%s nan",nm);
      manifold(nm,{&s.bottom,&s.top,&s.bevel},true); windingVsNormals(nm,s.bevel); windingVsNormals(nm,s.bottom); windingVsNormals(nm,s.top);
      // the reflex corner of the L: its walls must have a normal pointing into the hole (away from the material), checked by winding above; also no vertex in the bevel mesh outside the outline
      CHECK(idxOutOfRange(s.bottom)+idxOutOfRange(s.top)+idxOutOfRange(s.bevel)==0,"%s index range",nm); }
    // outside / partly outside / too few points: skipped, not built; the rest of the build is unharmed
    Hole out; out.polygon={{300,300,false},{320,300,false},{320,320,false}}; Hole part; part.polygon={{90,30,false},{140,30,false},{140,50,false}}; Hole two; two.polygon={{10,10,false},{20,20,false}}; two.diameter=0;
    Result s; CHECK(Build(ol,{out,part,two,rd},P,s),"polygon skip build"); printf("polygon outside / partly outside / 2 points: skipped %d\n",s.skippedHoles); CHECK(s.skippedHoles==2,"outside polygons skipped (the 2-point one is a zero-diameter round hole, ignored)");
    Result a; Build(ol,{rd},P,a); CHECK(a.bottom.idx.size()==s.bottom.idx.size(),"skipped polygons leave the mesh as without them");
    // a polygon near the edge counts as crossing (distance polygon to outline, r = 0): a triangle 3 VPU from the bottom edge (bevel 1.9: band 2 x bevel = 3.8)
    Hole nr; nr.polygon={{40,3,false},{50,3,false},{45,12,false}}; Result s2; CHECK(Build(ol,{nr},P,s2),"near edge"); printf("polygon 3 VPU from the edge: crossing %d\n",s2.crossingHoles); CHECK(s2.crossingHoles==1,"near-edge polygon counted");
    int i2; { Result s0; Build(ol,{},P,s0); int i0; double want=capArea(s0.top,i0)-grownArea(nr.polygon,1.0); double got=capArea(s2.top,i2); printf("near-edge triangle top %.3f expected %.3f (grown by 1.0; the apex mitre is clamped at 2, so within 0.5)\n",got,want); CHECK(fabs(got-want)<0.5,"near-edge top area %.3f expected %.3f",got,want); }
    // HoleOutline of a polygon: clockwise, deduplicated, ignores diameter 0
    Hole ph; ph.polygon=Ldup; auto lo=HoleOutline(ph,0.25f); printf("HoleOutline polygon: %zu pts, signed area %.3f\n",lo.size(),ringArea(lo)); CHECK(lo.size()==6&&ringArea(lo)<0,"polygon outline cw, deduped");
    // a hole made as a polygon of a circle equals the round hole's area
    Hole circ; auto cl=HoleOutline(Hole{50,40,20},0.25f); circ.polygon=cl; Result sc2,sr; Build(ol,{circ},P,sc2); Build(ol,{Hole{50,40,20}},P,sr); // (not 12: that circle's 30 degree turns sit on the kSharp wall split threshold)
 int j2;
    printf("polygon-of-circle: bottom %zu/%zu tris, area %.4f/%.4f, walls %zu/%zu verts\n",sc2.bottom.idx.size()/3,sr.bottom.idx.size()/3,capArea(sc2.bottom,j2),capArea(sr.bottom,j2),sc2.bevel.verts.size(),sr.bevel.verts.size()); CHECK(sc2.bottom.idx.size()==sr.bottom.idx.size()&&fabs(capArea(sc2.bottom,j2)-capArea(sr.bottom,j2))<1e-2&&sc2.bevel.verts.size()==sr.bevel.verts.size(),"polygon of a circle = round hole");
    // a round hole with a polygon: round data ignored
    Hole both=ph; both.x=50; both.y=40; both.diameter=30; Hole pure; pure.polygon=L; Result sb,sp; Build(ol,{both},P,sb); Build(ol,{pure},P,sp); CHECK(sb.bottom.idx==sp.bottom.idx&&sb.bevel.idx==sp.bevel.idx,"polygon wins over circle fields"); }

  // ---- bottomCap = false
  { Params q=P; q.bottomCap=false; Result nb,wb; CHECK(Build(ol,holes,q,nb),"no bottom build"); Build(ol,holes,P,wb);
    CHECK(nb.bottom.verts.empty()&&nb.bottom.idx.empty(),"bottom empty"); CHECK(nb.top.idx==wb.top.idx&&nb.bevel.idx==wb.bevel.idx&&nb.top.verts.size()==wb.top.verts.size()&&memcmp(nb.top.verts.data(),wb.top.verts.data(),wb.top.verts.size()*sizeof(Vertex))==0&&memcmp(nb.bevel.verts.data(),wb.bevel.verts.data(),wb.bevel.verts.size()*sizeof(Vertex))==0,"top and walls identical to the capped build");
    printf("bottomCap false: bottom %zu tris, top %zu, bevel %zu\n",nb.bottom.idx.size()/3,nb.top.idx.size()/3,nb.bevel.idx.size()/3);
    // default is on
    CHECK(Params().bottomCap,"bottomCap defaults to true");
    // a tiny outline that only builds with bottom off? no: failing top still fails
    Result f; CHECK(!Build({{0,0,false},{1,1,false}},{},q,f),"2 pts still fail with bottomCap false"); }

  // ---- the playfield-board case: rectangle outline, height = -Thickness, bevel 0, no bottom cap, many round / slot / polygon holes
  { const float W=952,H=2103,T=23.5f; std::vector<CurvePoint> rect={{0,0,false},{W,0,false},{W,H,false},{0,H,false}}; Params b; b.height=-T; b.thickness=T; b.bevel=0; b.bottomCap=false;
    b.uvm[0]=1/W; b.uvm[1]=0; b.uvm[2]=0; b.uvm[3]=0; b.uvm[4]=1/H; b.uvm[5]=0;
    std::vector<Hole> hs; srand(7);
    for(int i=0;i<48;i++){ Hole h; h.x=40+(i%6)*150.f+(i/6)*3; h.y=60+(i/6)*200.f; h.diameter=29.f; hs.push_back(h); } // GI-size round holes on a grid (0.58 in = 29 VPU... spec's GI)
    for(int i=0;i<12;i++){ Hole h; h.x=80+(i%4)*200.f; h.y=1500+(i/4)*80.f; h.diameter=8.7f; h.x2=h.x+60; h.y2=h.y; h.pill=true; hs.push_back(h); }
    double polyTotal=0; for(int i=0;i<20;i++){ Hole h; double cx=100+(i%5)*160.0, cy=1750+(i/5)*70.0; for(int k=0;k<200;k++){ double t=-2*M_PI*k/200; double rr=22+6*sin(5*t+i); h.polygon.push_back({(float)(cx+rr*cos(t)),(float)(cy+rr*sin(t)),false}); } polyTotal+=fabs(ringArea(h.polygon)); hs.push_back(h); } // 200-point wobbly insert-shaped holes
    Result s; auto t0=clock(); bool ok=Build(rect,hs,b,s); double ms=1000.0*(clock()-t0)/CLOCKS_PER_SEC;
    printf("board: build %d, %.0f ms, top %zu tris, walls %zu tris, skipped %d crossing %d, bottom %zu tris\n",ok,ms,s.top.idx.size()/3,s.bevel.idx.size()/3,s.skippedHoles,s.crossingHoles,s.bottom.idx.size()/3);
    CHECK(ok,"board builds with bottomCap false"); CHECK(s.bottom.idx.empty()&&s.bottom.verts.empty(),"board has no bottom mesh"); CHECK(s.skippedHoles==0&&s.crossingHoles==0,"board holes all inside");
    CHECK(ms<2000,"board build time"); int iv; double at=capArea(s.top,iv); 
    double roundA=0; for(size_t i=0;i<60;i++){ auto lp=HoleOutline(hs[i],0.25f); roundA+=fabs(ringArea(lp)); }
    printf("board top area %.2f vs rectangle - holes %.2f\n",at,(double)W*H-roundA-polyTotal); CHECK(fabs(at-((double)W*H-roundA-polyTotal))<5e-6*W*H,"board top area");
    float zmin=1e9,zmax=-1e9; for(auto&v:s.top.verts){ CHECK(v.z==0.f&&v.nz==1.f,"top z / normal"); } for(auto&v:s.bevel.verts){ zmin=std::min(zmin,v.z); zmax=std::max(zmax,v.z); }
    printf("board wall z %.2f..%.2f\n",zmin,zmax); CHECK(zmin==-T&&zmax==0.f,"wall z range"); // bevel 0: straight walls, no inset
    for(auto&v:s.top.verts) if(v.z!=0.f){CHECK(false,"top z");break;}
    // bevel 0: the top ring equals the wall's top ring (no inset), so the top and walls close the shell except at z = -T (open rim)
    manifold("board top+walls",{&s.top,&s.bevel},false);
    { std::map<std::pair<Key,Key>,int> e; for(const Mesh*m:{&s.top,&s.bevel}) for(size_t t=0;t<m->idx.size();t+=3){ Key k[3]; for(int i=0;i<3;i++) k[i]=K(m->verts[m->idx[t+i]]); for(int i=0;i<3;i++) e[{k[i],k[(i+1)%3]}]++; }
      int dup=0,unpaired=0,rimBad=0; for(auto&kv:e){ if(kv.second>1) dup++; if(!e.count({kv.first.second,kv.first.first})){ unpaired++; if(!(kv.first.first.z==-(long)lround(T*1000)&&kv.first.second.z==-(long)lround(T*1000))) rimBad++; } }
      printf("board: duplicate directed edges %d, unpaired edges %d (of which off the z=-T rim %d)\n",dup,unpaired,rimBad); { int shown=0; for(auto&kv:e) if(!e.count({kv.first.second,kv.first.first})&&!(kv.first.first.z==-23500&&kv.first.second.z==-23500)&&shown++<6) printf("  unpaired (%ld,%ld,%ld)->(%ld,%ld,%ld)\n",kv.first.first.x,kv.first.first.y,kv.first.first.z,kv.first.second.x,kv.first.second.y,kv.first.second.z); } CHECK(dup==0&&rimBad==0,"board shell closed except the open bottom rim"); }
    windingVsNormals("board top",s.top); windingVsNormals("board walls",s.bevel);
    CHECK(badVerts(s.top)+badVerts(s.bevel)==0,"board nan / inf"); CHECK(idxOutOfRange(s.top)+idxOutOfRange(s.bevel)==0,"board index range");
    // uv rectangle mapping: u,v in 0..1 on the top
    { float mn=1e9,mx=-1e9; for(auto&v:s.top.verts){ mn=std::min(mn,std::min(v.tu,v.tv)); mx=std::max(mx,std::max(v.tu,v.tv)); } printf("board top uv range %.3f..%.3f\n",mn,mx); CHECK(mn>=-1e-5f&&mx<=1+1e-5f,"uv range"); }
    // same with bottomCap true: builds, bottom has the same triangle count as top (bevel 0)
    Params b2=b; b2.bottomCap=true; Result s2; CHECK(Build(rect,hs,b2,s2),"board with bottom cap"); CHECK(s2.bottom.idx.size()==s2.top.idx.size(),"bevel 0: bottom and top triangulate alike"); 
    // same board, bevel 0, run twice: identical bytes (deterministic)
    Result s3; Build(rect,hs,b,s3); CHECK(s3.top.idx==s.top.idx&&memcmp(s3.top.verts.data(),s.top.verts.data(),s.top.verts.size()*sizeof(Vertex))==0,"deterministic");
    // PF scale: 4-10k contour points over two faces
    { std::vector<Hole> big; size_t pts=0; for(int i=0;i<40;i++){ Hole h; double cx=60+(i%8)*110.0, cy=100+(i/8)*300.0; for(int k=0;k<250;k++){ double t=-2*M_PI*k/250; double rr=30+8*sin(7*t+i); h.polygon.push_back({(float)(cx+rr*cos(t)),(float)(cy+rr*sin(t)),false}); } pts+=250; big.push_back(h); }
      Result s4; auto t1=clock(); bool ok4=Build(rect,big,b2,s4); double ms4=1000.0*(clock()-t1)/CLOCKS_PER_SEC; printf("PF scale: %zu hole points (x2 faces), build %d in %.0f ms, top tris %zu\n",pts,ok4,ms4,s4.top.idx.size()/3); CHECK(ok4,"pf scale build"); CHECK(ms4<1500,"pf scale time"); int i5; double pa=0; for(auto&h:big) pa+=fabs(ringArea(h.polygon)); double got5=capArea(s4.top,i5); printf("PF scale top area %.2f expected %.2f\n",got5,(double)W*H-pa); CHECK(fabs(got5-((double)W*H-pa))<5e-6*W*H,"pf scale top area"); } }

  // ---- crack fix: a hole near the outline whose bevel is clamped (topGrow) uses that one bevel for its top inset and its bevel rings, so the rings'
  // top vertices are the cap ring exactly (before, the rings used the full bevel: a gap between the cap and the rim). Partly clamped and fully clamped (to 0).
  for(float hy:{6.5f,5.0f}){ Result s; CHECK(Build(ol,{{50,hy,6},{20,40,8}},P,s),"near-edge build"); const float zt=P.height+P.thickness;
    auto key=[](const Vertex&v){ return std::make_tuple(v.x,v.y,v.z); }; std::map<std::tuple<float,float,float>,int> cap,ring;
    for(auto&v:s.top.verts) cap[key(v)]++; for(auto&v:s.bevel.verts) if(v.z==zt) ring[key(v)]++;
    int missing=0,extra=0; for(auto&kv:cap) if(!ring.count(kv.first)) missing++; for(auto&kv:ring) if(!cap.count(kv.first)) extra++;
    printf("near-edge hole y %.1f: cap ring verts %zu, bevel top verts %zu, cap verts not on a ring %d, ring top verts not on the cap %d\n",hy,cap.size(),ring.size(),missing,extra);
    CHECK(missing==0&&extra==0,"clamped hole: ring top vertices equal the cap ring (y %.1f)",hy); manifold(hy>6.f?"near-edge partly clamped":"near-edge clamped to 0",{&s.bottom,&s.top,&s.bevel},true); windingVsNormals("near-edge bevel",s.bevel); }

  // degenerate
  { Result s; CHECK(!Build({{0,0,false},{1,1,false}},{},P,s),"2 pts"); }
  dump("main.txt",r,flat);
  printf(fails?"FAILED %d\n":"ALL OK\n",fails); return fails?1:0; }
