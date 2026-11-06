// Standalone check of src/math/PlayfieldBoard:
//   clang++ -std=c++20 -I src tools/playfieldboard_check.cpp src/math/PlayfieldBoard.cpp src/math/PlasticMesh.cpp -o /tmp/pbc && /tmp/pbc [--exact] [fixture ...]
// Also build it with -O2 -ffast-math (the engine's flags).
// Each fixture (tools/fixtures/*.pfboard.txt) was dumped by the editor's board build before it moved into PlayfieldBoard: the table rectangle, the board
// settings, every hole and the meshes that build produced. The check rebuilds from the holes and compares with a 1e-5 tolerance (--exact: bit for bit; clang and gcc differ by ~1e-7 from float contraction, g++ -O2 -ffast-math gave 0) and reports the largest difference.
// The fixtures have no bevels (phase 1): top, walls and backing must come out as they were. The floor collider gained the Falls Through holes' walls
// (P2-2): its flat face must still be the fixture's floor, followed by those walls.
// Then synthetic cases: overlaps, outlines, and the bevels (profile, default and insert pockets, the board edge and hole-to-hole clamps, the floor's chamfer).
#include "math/PlayfieldBoard.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
using namespace PlayfieldBoard;
static int fails = 0;
static double gTol = 1e-5; // --exact: 0, a bit-for-bit comparison
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Fixture
{
   float rect[4] = {};
   float thick = 0.f;
   int detail = 1, floorMode = 0;
   float uvm[6] = {};
   std::vector<BoardHole> holes;
   std::vector<char> overlap;
   PlasticMesh::Mesh board, walls, floor;
};

static bool ReadMesh(FILE* f, PlasticMesh::Mesh& m)
{
   char tag[32], name[32];
   size_t nv = 0, ni = 0;
   if (fscanf(f, "%31s %31s %zu %zu", tag, name, &nv, &ni) != 4 || strcmp(tag, "mesh") != 0)
      return false;
   m.verts.resize(nv);
   for (PlasticMesh::Vertex& v : m.verts)
      if (fscanf(f, " V %f %f %f %f %f %f %f %f", &v.x, &v.y, &v.z, &v.nx, &v.ny, &v.nz, &v.tu, &v.tv) != 8)
         return false;
   m.idx.resize(ni);
   for (size_t i = 0; i < ni; i += 3)
      if (fscanf(f, " T %u %u %u", &m.idx[i], &m.idx[i + 1], &m.idx[i + 2]) != 3)
         return false;
   return true;
}

static bool Load(const char* path, Fixture& fx)
{
   FILE* f = fopen(path, "r");
   if (!f)
      return false;
   int ver = 0;
   size_t n = 0;
   bool ok = fscanf(f, " PFBOARD %d", &ver) == 1 && ver == 1 && fscanf(f, " rect %f %f %f %f", &fx.rect[0], &fx.rect[1], &fx.rect[2], &fx.rect[3]) == 4
      && fscanf(f, " params %f %d %d", &fx.thick, &fx.detail, &fx.floorMode) == 3 && fscanf(f, " uvm %f %f %f %f %f %f", &fx.uvm[0], &fx.uvm[1], &fx.uvm[2], &fx.uvm[3], &fx.uvm[4], &fx.uvm[5]) == 6
      && fscanf(f, " holes %zu", &n) == 1;
   for (size_t i = 0; ok && i < n; i++)
   {
      BoardHole h;
      int overlap = 0;
      size_t np = 0;
      ok = fscanf(f, " H %d %d %f %f %f %f %f %d %zu", &h.kind, &h.flags, &h.cx, &h.cy, &h.diameter, &h.ex, &h.ey, &overlap, &np) == 9;
      h.polygon.resize(np * 2);
      for (size_t k = 0; ok && k < np * 2; k++)
         ok = fscanf(f, " %f", &h.polygon[k]) == 1;
      fx.holes.push_back(std::move(h));
      fx.overlap.push_back((char)overlap);
   }
   ok = ok && ReadMesh(f, fx.board) && ReadMesh(f, fx.walls) && ReadMesh(f, fx.floor);
   fclose(f);
   return ok;
}

// Largest component difference between two meshes; -1 when the vertex or index counts or any index differ
static double Compare(const PlasticMesh::Mesh& a, const PlasticMesh::Mesh& b)
{
   if (a.verts.size() != b.verts.size() || a.idx != b.idx)
      return -1.;
   double worst = 0.;
   for (size_t i = 0; i < a.verts.size(); i++)
   {
      const float p[8] = { a.verts[i].x, a.verts[i].y, a.verts[i].z, a.verts[i].nx, a.verts[i].ny, a.verts[i].nz, a.verts[i].tu, a.verts[i].tv };
      const float q[8] = { b.verts[i].x, b.verts[i].y, b.verts[i].z, b.verts[i].nx, b.verts[i].ny, b.verts[i].nz, b.verts[i].tu, b.verts[i].tv };
      for (int k = 0; k < 8; k++)
         worst = std::max(worst, (double)fabsf(p[k] - q[k]));
   }
   return worst;
}

static double CheckFixture(const char* path)
{
   Fixture fx;
   if (FILE* const probe = fopen(path, "r"))
      fclose(probe);
   else
   {
      printf("%s: not present, skipped (the large fixtures are fork-only)\n", path);
      return 0.;
   }
   if (!Load(path, fx))
   {
      CHECK(false, "cannot read fixture %s", path);
      return 0.;
   }
   BoardParams p;
   p.outline = { fx.rect[0], fx.rect[1], fx.rect[2], fx.rect[1], fx.rect[2], fx.rect[3], fx.rect[0], fx.rect[3] };
   p.thickness = fx.thick;
   p.detail = fx.detail;
   for (int i = 0; i < 6; i++)
      p.uvm[i] = fx.uvm[i];
   std::vector<BoardHole> holes = fx.holes;
   if (fx.floorMode != 0)
      for (BoardHole& h : holes)
         h.flags &= ~kFallsThrough; // Keep Existing floor: what the editor does
   BoardMeshes m;
   BoardReport r;
   CHECK(BuildBoard(p, holes, m, r), "%s: BuildBoard failed", path);
   PlasticMesh::Mesh walls = m.walls; // The editor appends the backing to the walls part
   const unsigned base = (unsigned)walls.verts.size();
   walls.verts.insert(walls.verts.end(), m.backing.verts.begin(), m.backing.verts.end());
   for (unsigned i : m.backing.idx)
      walls.idx.push_back(base + i);
   // The floor: its flat face first (the fixture's floor), then the Falls Through holes' walls, each vertex one of the walls part's (same loops, bevel 0)
   PlasticMesh::Mesh flatFloor;
   const size_t nfv = std::min(fx.floor.verts.size(), m.floor.verts.size()), nfi = std::min(fx.floor.idx.size(), m.floor.idx.size());
   flatFloor.verts.assign(m.floor.verts.begin(), m.floor.verts.begin() + nfv);
   flatFloor.idx.assign(m.floor.idx.begin(), m.floor.idx.begin() + nfi);
   {
      std::set<std::tuple<float, float, float, float, float>> wallVerts;
      for (const PlasticMesh::Vertex& v : m.walls.verts)
         wallVerts.insert({ v.x, v.y, v.z, v.nx, v.ny });
      size_t notInWalls = 0, fallsHoles = 0;
      for (size_t i = nfv; i < m.floor.verts.size(); i++)
         notInWalls += !wallVerts.count({ m.floor.verts[i].x, m.floor.verts[i].y, m.floor.verts[i].z, m.floor.verts[i].nx, m.floor.verts[i].ny });
      for (size_t i = 0; i < holes.size(); i++)
         fallsHoles += (holes[i].flags & kCut) && (holes[i].flags & kFallsThrough) && !r.overlap[i];
      CHECK(notInWalls == 0 && (fallsHoles == 0) == (m.floor.verts.size() == nfv), "%s: floor walls: %zu vertices not in the walls part, %zu Falls Through holes, %zu wall vertices", path, notInWalls, fallsHoles,
         m.floor.verts.size() - nfv);
      CHECK(m.backing.idx == flatFloor.idx, "%s: the backing is the floor's flat face", path);
   }
   const double dt = Compare(m.top, fx.board), dw = Compare(walls, fx.walls), df = Compare(flatFloor, fx.floor);
   CHECK(dt >= 0., "%s: top differs in vertex or index counts (%zu/%zu verts, %zu/%zu idx)", path, m.top.verts.size(), fx.board.verts.size(), m.top.idx.size(), fx.board.idx.size());
   CHECK(dw >= 0., "%s: walls+backing differ in vertex or index counts (%zu/%zu verts)", path, walls.verts.size(), fx.walls.verts.size());
   CHECK(df >= 0., "%s: floor differs in vertex or index counts (%zu/%zu verts)", path, m.floor.verts.size(), fx.floor.verts.size());
   const double worst = std::max({ dt, dw, df });
   CHECK(worst <= gTol, "%s: largest difference %g > %g", path, worst, gTol);
   CHECK(r.overlap == fx.overlap, "%s: overlap flags differ", path);
   printf("%s: %zu holes, %d built, %d overlapping; top %zu, walls %zu, backing %zu, floor %zu triangles; largest difference top %.3g walls+backing %.3g floor %.3g\n", path, holes.size(), r.built, r.overlapping, r.topTriangles,
      r.wallTriangles, r.backingTriangles, r.floorTriangles, dt, dw, df);
   return worst;
}

static BoardHole Round(float x, float y, float d, int flags = kCut)
{
   BoardHole h;
   h.kind = kRound;
   h.flags = flags;
   h.cx = x;
   h.cy = y;
   h.diameter = d;
   return h;
}

// Synthetic cases that need no fixture: counts, the overlap rules, a hole beyond the edge, a non-rectangular outline, degenerate input
static void Synthetic()
{
   BoardParams p;
   p.outline = { 0, 0, 100, 0, 100, 200, 0, 200 };
   p.thickness = 10.f;
   p.uvm[0] = 0.01f;
   p.uvm[4] = 0.005f;
   std::vector<BoardHole> holes = { Round(30, 30, 10), Round(34, 30, 10), Round(70, 100, 12, kCut | kFallsThrough), Round(3, 100, 10), Round(50, 150, 8, 0) };
   BoardMeshes m;
   BoardReport r;
   CHECK(BuildBoard(p, holes, m, r), "synthetic build");
   CHECK(r.overlap == std::vector<char>({ 1, 1, 0, 1, 0 }), "overlap flags: two touching holes, one over the edge, one uncut");
   CHECK(r.built == 1 && r.overlapping == 3, "built %d overlapping %d", r.built, r.overlapping);
   CHECK(!m.top.idx.empty() && !m.walls.idx.empty() && !m.backing.idx.empty() && !m.floor.idx.empty(), "meshes empty");
   CHECK(m.floor.idx.size() > 6, "the Falls Through hole is cut from the floor");
   CHECK(m.backing.idx.size() < m.floor.idx.size() && std::equal(m.backing.idx.begin(), m.backing.idx.end(), m.floor.idx.begin()), "backing: the floor's flat face (the floor adds the hole's walls)");
   for (const PlasticMesh::Vertex& v : m.backing.verts)
      CHECK(v.z == -p.thickness - 0.5f, "backing depth");

   // A hexagonal board: a hole inside builds, one reaching out through the slanted edge is left out
   BoardParams hex;
   hex.outline = { 50, 0, 100, 25, 100, 75, 50, 100, 0, 75, 0, 25 };
   hex.thickness = 10.f;
   hex.uvm[0] = 0.01f;
   hex.uvm[4] = 0.01f;
   std::vector<BoardHole> hh = { Round(50, 50, 20), Round(8, 12, 14) };
   BoardMeshes hm;
   BoardReport hr;
   CHECK(BuildBoard(hex, hh, hm, hr), "hexagon build");
   CHECK(hr.overlap == std::vector<char>({ 0, 1 }), "hexagon: the hole through the slanted edge is left out (%d %d)", hr.overlap[0], hr.overlap[1]);
   CHECK(hr.built == 1, "hexagon built %d", hr.built);

   BoardParams bad = p;
   bad.outline = { 0, 0, 100, 0 };
   BoardMeshes bm;
   BoardReport br;
   CHECK(!BuildBoard(bad, holes, bm, br), "an outline of two points fails");
   bad.outline = { 0, 0, 0, 0, 0, 200, 0, 200 };
   CHECK(!BuildBoard(bad, holes, bm, br), "a zero-width rectangle fails");
   CHECK(bm.top.verts.empty(), "meshes untouched on failure");
}

// ---- bevels
static double CapArea(const PlasticMesh::Mesh& m, int& inverted)
{
   double a = 0.;
   inverted = 0;
   for (size_t t = 0; t < m.idx.size(); t += 3)
   {
      const PlasticMesh::Vertex &p = m.verts[m.idx[t]], &q = m.verts[m.idx[t + 1]], &r = m.verts[m.idx[t + 2]];
      const double ar = 0.5 * ((q.x - p.x) * (double)(r.y - p.y) - (q.y - p.y) * (double)(r.x - p.x));
      a += ar;
      inverted += ar < -1e-6;
   }
   return a;
}
static double CircleGrown(const double d, const double b) { return M_PI * (0.5 * d + b) * (0.5 * d + b); }
struct WKey
{
   long x, y, z;
   bool operator<(const WKey& o) const { return std::tie(x, y, z) < std::tie(o.x, o.y, o.z); }
};
static WKey Wk(const PlasticMesh::Vertex& v) { return { lround(v.x * 1000), lround(v.y * 1000), lround(v.z * 1000) }; }
// Meshes welded by position: every directed edge has its reverse, once, except along the open rim at z = rimZ. Returns the breaks off the rim
// With openEdge set, the edges at z = 0 along that rectangle's border are open too (the floor has no outline walls)
static int ShellBreaks(const std::vector<const PlasticMesh::Mesh*>& ms, const float rimZ, int& rim, const float* openEdge = nullptr)
{
   std::map<std::pair<WKey, WKey>, int> e;
   for (const PlasticMesh::Mesh* m : ms)
      for (size_t t = 0; t < m->idx.size(); t += 3)
         for (int i = 0; i < 3; i++)
         {
            const WKey a = Wk(m->verts[m->idx[t + i]]), b = Wk(m->verts[m->idx[t + (i + 1) % 3]]);
            if (!(a < b) && !(b < a))
               continue;
            e[{ a, b }]++;
         }
   int bad = 0;
   rim = 0;
   const long rz = lround(rimZ * 1000);
   for (const auto& kv : e)
   {
      if (kv.second > 1)
         bad++;
      if (!e.count({ kv.first.second, kv.first.first }))
      {
         const auto onEdge = [openEdge](const WKey& k)
         { return openEdge && k.z == 0 && (k.x == lround(openEdge[0] * 1000) || k.x == lround(openEdge[2] * 1000) || k.y == lround(openEdge[1] * 1000) || k.y == lround(openEdge[3] * 1000)); };
         if ((kv.first.first.z == rz && kv.first.second.z == rz) || (onEdge(kv.first.first) && onEdge(kv.first.second)))
            rim++;
         else
            bad++;
      }
   }
   return bad;
}
static int AgainstNormals(const PlasticMesh::Mesh& m)
{
   int bad = 0;
   for (size_t t = 0; t < m.idx.size(); t += 3)
   {
      const PlasticMesh::Vertex &a = m.verts[m.idx[t]], &b = m.verts[m.idx[t + 1]], &c = m.verts[m.idx[t + 2]];
      const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
      const double cx = uy * wz - uz * wy, cy = uz * wx - ux * wz, cz = ux * wy - uy * wx, l = sqrt(cx * cx + cy * cy + cz * cz);
      if (l < 1e-9)
         continue;
      bad += (cx * (a.nx + b.nx + c.nx) + cy * (a.ny + b.ny + c.ny) + cz * (a.nz + b.nz + c.nz)) / l <= 0.;
   }
   return bad;
}
static bool BadBits(const float f) { unsigned u; memcpy(&u, &f, 4); return ((u >> 23) & 0xff) == 0xff; } // NaN / Inf by bit pattern (fast-math safe)

// The grown top rings as BuildBoard built them (PlasticMesh::Build again with the reported bevels, as BuildTop does), then: every ring inside the
// outline and at least 0.1 from it, the rings at least 0.1 apart (none crossing), none folded (an edge against its contour edge, or self-crossing)
static double SegSeg(const PlasticMesh::CurvePoint& a, const PlasticMesh::CurvePoint& b, const PlasticMesh::CurvePoint& c, const PlasticMesh::CurvePoint& d)
{
   const auto orient = [](const PlasticMesh::CurvePoint& p, const PlasticMesh::CurvePoint& q, const PlasticMesh::CurvePoint& r) { return ((double)q.x - p.x) * ((double)r.y - p.y) - ((double)q.y - p.y) * ((double)r.x - p.x); };
   const double d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
   if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0)
      return 0.;
   const auto ps = [](const PlasticMesh::CurvePoint& p, const PlasticMesh::CurvePoint& q, const PlasticMesh::CurvePoint& r)
   {
      const double ex = (double)r.x - q.x, ey = (double)r.y - q.y, l2 = ex * ex + ey * ey;
      const double t = l2 > 0 ? std::clamp((((double)p.x - q.x) * ex + ((double)p.y - q.y) * ey) / l2, 0., 1.) : 0.;
      return std::hypot(q.x + t * ex - p.x, q.y + t * ey - p.y);
   };
   return std::min(std::min(ps(a, c, d), ps(b, c, d)), std::min(ps(c, a, b), ps(d, a, b)));
}
static double LoopGap(const std::vector<PlasticMesh::CurvePoint>& a, const std::vector<PlasticMesh::CurvePoint>& b)
{
   double g = 1e300;
   for (size_t i = 0; i < a.size(); i++)
      for (size_t j = 0; j < b.size(); j++)
         g = std::min(g, SegSeg(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()]));
   return g;
}
static int RingProblems(const BoardParams& bp, const std::vector<BoardHole>& holes, const BoardReport& r, std::string& what)
{
   std::vector<PlasticMesh::CurvePoint> ol;
   for (size_t i = 0; i + 1 < bp.outline.size(); i += 2)
      ol.push_back({ bp.outline[i], bp.outline[i + 1], false });
   std::vector<PlasticMesh::Hole> mh;
   for (size_t i = 0; i < holes.size(); i++)
   {
      if (!(holes[i].flags & kCut) || r.overlap[i])
         continue;
      PlasticMesh::Hole m;
      m.x = holes[i].cx, m.y = holes[i].cy, m.diameter = holes[i].diameter, m.x2 = holes[i].ex, m.y2 = holes[i].ey, m.pill = holes[i].kind == kSlot;
      if (holes[i].kind == kPolygon)
         for (size_t k = 0; k + 1 < holes[i].polygon.size(); k += 2)
            m.polygon.push_back({ holes[i].polygon[k], holes[i].polygon[k + 1], false });
      m.bevel = r.bevel[i];
      mh.push_back(m);
   }
   PlasticMesh::Params p;
   p.bottomCap = false, p.height = -bp.thickness, p.thickness = bp.thickness, p.bevel = 0.f, p.outlineBevel = 0.f, p.bevelSegments = 1, p.flatten = PlasticMesh::FlattenTolerance(bp.detail);
   PlasticMesh::Result res;
   if (!PlasticMesh::Build(ol, mh, p, res))
   {
      what = "rebuild failed";
      return 1;
   }
   int bad = 0;
   char b[160];
   for (size_t k = 1; k < res.topRings.size(); k++)
   {
      const auto &ring = res.topRings[k], &c = res.contours[k];
      double edge = LoopGap(ring, ol);
      bool inside = true;
      for (const auto& v : ring)
      {
         bool in = false;
         for (size_t i = 0, j = ol.size() - 1; i < ol.size(); j = i++)
            if (((ol[i].y > v.y) != (ol[j].y > v.y)) && (v.x < (ol[j].x - ol[i].x) * (v.y - ol[i].y) / (ol[j].y - ol[i].y) + ol[i].x))
               in = !in;
         inside &= in;
      }
      if (!inside || edge < 0.1 - 2e-3)
      {
         bad++;
         snprintf(b, sizeof(b), "ring %zu off the board or %.4f from the edge; ", k, edge);
         what += b;
      }
      for (size_t i = 0; i < ring.size(); i++)
      {
         const size_t j = (i + 1) % ring.size();
         if ((double)(c[j].x - c[i].x) * (ring[j].x - ring[i].x) + (double)(c[j].y - c[i].y) * (ring[j].y - ring[i].y) <= 0.)
         {
            bad++;
            snprintf(b, sizeof(b), "ring %zu edge %zu reversed; ", k, i);
            what += b;
            break;
         }
      }
      for (size_t i = 0; i < ring.size(); i++)
         for (size_t j = i + 2; j < ring.size(); j++)
            if (!(i == 0 && j == ring.size() - 1) && SegSeg(ring[i], ring[i + 1], ring[j], ring[(j + 1) % ring.size()]) == 0.)
            {
               bad++;
               snprintf(b, sizeof(b), "ring %zu crosses itself; ", k);
               what += b;
               i = j = ring.size();
            }
      for (size_t l = k + 1; l < res.topRings.size(); l++)
         if (const double g = LoopGap(ring, res.topRings[l]); g < 0.1 - 2e-3)
         {
            bad++;
            snprintf(b, sizeof(b), "rings %zu and %zu %.4f apart; ", k, l, g);
            what += b;
         }
   }
   return bad;
}
static BoardHole Poly(std::vector<float> xy, float bevel = -1.f)
{
   BoardHole h;
   h.kind = kPolygon;
   h.polygon = std::move(xy);
   h.bevel = bevel;
   return h;
}

static void Bevels()
{
   constexpr float W = 400.f, H = 600.f, T = 23.5f;
   BoardParams p;
   p.outline = { 0, 0, W, 0, W, H, 0, H };
   p.thickness = T;
   p.uvm[0] = 1.f / W;
   p.uvm[4] = 1.f / H;
   p.detail = 2;

   // One GI hole with a 2 VPU bevel, 3 segments: the top hole grows by 2, the walls carry the rounded profile, top and walls close (open only at -T)
   {
      BoardHole h = Round(200, 300, 27.3f);
      h.bevel = 2.f;
      BoardMeshes m;
      BoardReport r;
      CHECK(BuildBoard(p, { h }, m, r), "bevel build");
      int inv = 0;
      const double area = CapArea(m.top, inv), want = (double)W * H - CircleGrown(27.3, 2.0);
      printf("bevel 2: top area %.2f, rectangle - hole grown by 2 %.2f, report bevel %.3f clamped %d\n", area, want, r.bevel[0], r.bevelClamped);
      CHECK(fabs(area - want) < 0.01 * CircleGrown(27.3, 2.0) && inv == 0, "bevel: top hole grown by the bevel (%.2f vs %.2f, %d inverted)", area, want, inv);
      CHECK(r.bevel[0] == 2.f && r.bevelClamped == 0, "bevel: report %.3f, clamped %d", r.bevel[0], r.bevelClamped);
      int rim = 0;
      const int breaks = ShellBreaks({ &m.top, &m.walls }, -T, rim);
      printf("bevel 2: top + walls welded, breaks off the z = -T rim %d (rim edges %d)\n", breaks, rim);
      CHECK(breaks == 0, "bevel: the top and the bevelled walls close (cap ring = bevel top ring)");
      CHECK(AgainstNormals(m.walls) == 0, "bevel: walls wound against their normals");
      // The profile: per hole edge one wall quad and 3 bevel quads; normals: the top ring points up, the wall top ring horizontal
      int up = 0, flat = 0, bad = 0, vBad = 0;
      float vMin = 1e9f, vMax = -1e9f;
      for (const PlasticMesh::Vertex& v : m.walls.verts)
      {
         bad += BadBits(v.x) || BadBits(v.y) || BadBits(v.z) || BadBits(v.nx) || BadBits(v.ny) || BadBits(v.nz) || BadBits(v.tu) || BadBits(v.tv);
         const bool hole = fabsf(v.x - 200.f) < 30.f && fabsf(v.y - 300.f) < 30.f;
         if (!hole)
            continue;
         vMin = std::min(vMin, v.tv);
         vMax = std::max(vMax, v.tv);
         if (v.z == 0.f)
         {
            up += fabsf(v.nz - 1.f) < 1e-5f;
            vBad += v.tv != 0.f;
         }
         if (v.z == -2.f && v.nz == 0.f)
         {
            flat++;
            vBad += fabsf(v.tv - 2.f / T) > 1e-6f;
         }
      }
      printf("bevel 2: hole wall vertices at the top %d (normal up), at the wall top (horizontal) %d; hole v %.4f..%.4f\n", up, flat, vMin, vMax);
      CHECK(up > 0 && flat > 0 && vBad == 0 && vMin == 0.f && vMax == 1.f && bad == 0, "bevel profile normals / v (v 0 at the surface, bevel / thickness at the wall top, 1 at the bottom)");
      // Same hole with bevel 0: 4 times fewer hole wall triangles than 1 + 3 rings
      BoardHole h0 = h;
      h0.bevel = 0.f;
      BoardMeshes m0;
      BoardReport r0;
      BuildBoard(p, { h0 }, m0, r0);
      const size_t edgeTris0 = m0.walls.idx.size() / 3, edgeTris = m.walls.idx.size() / 3;
      CHECK(edgeTris - 8 == 4 * (edgeTris0 - 8), "segments: a wall and 3 rings per hole edge (%zu vs %zu triangles)", edgeTris, edgeTris0);
      p.bevelSegments = 1;
      BoardMeshes m1;
      BuildBoard(p, { h }, m1, r0);
      CHECK(m1.walls.idx.size() / 3 - 8 == 2 * (edgeTris0 - 8), "one segment: a wall and a chamfer");
      int c45 = 0;
      for (const PlasticMesh::Vertex& v : m1.walls.verts)
         c45 += v.z > -2.f + 1e-4f || (v.z == -2.f && v.nz > 0.5f) ? fabsf(v.nz - 0.70710678f) < 1e-5f : 0;
      CHECK(c45 > 0, "chamfer normal at 45 deg");
      p.bevelSegments = 3;
   }

   // Sentinels: -1 = the board's default, 0 = off, > 0 custom; insert pockets always 0
   {
      p.defaultBevel = 1.47f;
      BoardHole a = Round(100, 100, 27.3f), b = Round(300, 100, 27.3f), c = Round(100, 400, 27.3f), d = Round(300, 400, 27.3f, kCut | kUnderInsert);
      b.bevel = 0.f;
      c.bevel = 3.f;
      d.bevel = 3.f;
      BoardMeshes m;
      BoardReport r;
      BuildBoard(p, { a, b, c, d }, m, r);
      printf("sentinels: default %.3f, off %.3f, custom %.3f, insert pocket %.3f\n", r.bevel[0], r.bevel[1], r.bevel[2], r.bevel[3]);
      CHECK(r.bevel == std::vector<float>({ 1.47f, 0.f, 3.f, 0.f }) && r.bevelClamped == 0, "bevel sentinels");
      p.defaultBevel = 0.f;
   }

   // The board edge clamp: the outline is never inset, the grown hole stays 0.1 clear of it (and a bevel over the thickness is capped)
   {
      BoardHole a = Round(20, 300, 27.3f), b = Round(200, 300, 27.3f);
      a.bevel = 10.f; // its rim is 6.35 from the edge: 6.25 left
      b.bevel = 40.f; // over the thickness
      BoardMeshes m;
      BoardReport r;
      BuildBoard(p, { a, b }, m, r);
      float xmin = 1e9f;
      for (const PlasticMesh::Vertex& v : m.top.verts)
         if (v.x > 0.f && fabsf(v.y - 300.f) < 40.f)
            xmin = std::min(xmin, v.x);
      printf("edge clamp: bevel 10 at 6.35 from the edge -> %.4f, nearest top ring x %.4f; bevel 40 -> %.3f (thickness %.1f); clamped %d\n", r.bevel[0], xmin, r.bevel[1], T, r.bevelClamped);
      // The flattened circle's leftmost vertex is a little inside the true circle: the clamp uses the loop as built
      // and the clamp allows for the mitre (the 12-gon's vertices move by bevel / cos 15 deg): below 6.35 - 0.1 by about that factor
      CHECK(r.bevel[0] >= 6.25f / 1.04f && r.bevel[0] < 6.35f && xmin >= 0.1f - 1e-3f && r.bevel[1] == T && r.bevelClamped == 2 && r.clamped == std::vector<char>({ 1, 1 }), "edge clamp");
      int rim = 0;
      CHECK(ShellBreaks({ &m.top, &m.walls }, -T, rim) == 0, "edge clamp: top and walls close");
      // A non-rectangular outline: the same clamp by segment distance
      BoardParams hex = p;
      hex.outline = { 200, 0, 400, 150, 400, 450, 200, 600, 0, 450, 0, 150 };
      BoardHole e = Round(200, 300, 27.3f), f = Round(30, 300, 27.3f);
      e.bevel = 30.f;
      f.bevel = 20.f;
      BoardMeshes hm;
      BoardReport hr;
      BuildBoard(hex, { e, f }, hm, hr);
      printf("edge clamp, hexagon: far hole 30 -> %.3f (thickness), hole 16.35 from the edge 20 -> %.3f\n", hr.bevel[0], hr.bevel[1]);
      CHECK(hr.bevel[0] == T && hr.bevel[1] >= 16.25f / 1.04f && hr.bevel[1] <= 16.25f, "edge clamp on a polygon outline");
      int rim2 = 0;
      CHECK(ShellBreaks({ &hm.top, &hm.walls }, -T, rim2) == 0, "hexagon: top and walls close (a bevel as deep as the wood: no straight wall)");
   }

   // Hole to hole: two holes 6 VPU apart, bevels 5 and 1: both to under half the gap (2.95), the far pair untouched
   {
      BoardHole a = Round(100, 100, 27.3f), b = Round(100 + 27.3f + 6.f, 100, 27.3f), c = Round(300, 400, 27.3f), d = Round(300 + 27.3f + 20.f, 400, 27.3f);
      a.bevel = 5.f;
      b.bevel = 1.f;
      c.bevel = d.bevel = 5.f;
      BoardMeshes m;
      BoardReport r;
      BuildBoard(p, { a, b, c, d }, m, r);
      printf("hole clamp: gap 6, bevels 5 / 1 -> %.4f / %.4f; gap 20, 5 / 5 -> %.3f / %.3f; clamped %d\n", r.bevel[0], r.bevel[1], r.bevel[2], r.bevel[3], r.bevelClamped);
      CHECK(r.bevel[0] < 3.f && r.bevel[0] > 2.9f && r.bevel[1] == 1.f && r.bevel[2] == 5.f && r.bevel[3] == 5.f && r.bevelClamped == 1, "hole-to-hole clamp");
      int inv = 0, rim = 0;
      CapArea(m.top, inv);
      CHECK(inv == 0 && ShellBreaks({ &m.top, &m.walls }, -T, rim) == 0, "hole clamp: top not folded, shell closed");
      // Both large: each to under half the gap
      a.bevel = b.bevel = 5.f;
      BuildBoard(p, { a, b }, m, r);
      CHECK(r.bevel[0] == r.bevel[1] && r.bevel[0] < 3.f && r.bevel[0] > 2.9f && r.bevelClamped == 2, "hole clamp, both large: %.4f %.4f", r.bevel[0], r.bevel[1]);
   }

   // The review's failing cases (P2-2 geometry review B1, S1): the clamps must allow for the mitre (a grown vertex moves bevel x 1 / cos(half its turn),
   // up to 2), and a concave hole whose grown ring would fold gets a smaller bevel. Board 1000 x 2000, Curve Detail 1
   {
      BoardParams q;
      q.outline = { 0, 0, 1000, 0, 1000, 2000, 0, 2000 };
      q.thickness = T;
      q.defaultBevel = 1.47f;
      struct Case
      {
         const char* name;
         std::vector<BoardHole> holes;
      };
      BoardHole far = Round(975, 1000, 10.f);
      far.bevel = 23.5f;
      const float s = 21.77f;
      const std::vector<Case> cases = {
         { "two d 10 holes 13 apart, Default 1.47", { Round(500, 500, 10.f), Round(513, 500, 10.f) } },
         { "squares corner to corner 2.5 apart, 1.47", { Poly({ 400, 400, 420, 400, 420, 420, 400, 420 }), Poly({ 400 + s, 400 + s, 420 + s, 400 + s, 420 + s, 420 + s, 400 + s, 420 + s }) } },
         { "diamond tip 1.2 from the edge, 1.47", { Poly({ 998.8f, 500, 988.8f, 510, 978.8f, 500, 988.8f, 490 }) } },
         { "d 10 hole 20 from the edge, bevel 23.5", { far } },
         { "U-shaped hole, a tongue 2 wide, bevel 3", { Poly({ 400, 400, 440, 400, 440, 460, 421, 460, 421, 420, 419, 420, 419, 460, 400, 460 }, 3.f) } },
      };
      for (const Case& c : cases)
      {
         BoardMeshes m;
         BoardReport r;
         CHECK(BuildBoard(q, c.holes, m, r), "%s: build", c.name);
         std::string what;
         const int bad = RingProblems(q, c.holes, r, what);
         int rim = 0, inv = 0;
         const int breaks = ShellBreaks({ &m.top, &m.walls }, -T, rim, nullptr);
         CapArea(m.top, inv);
         std::string bevs;
         for (const float b : r.bevel)
            bevs += " " + std::to_string(b).substr(0, 6);
         printf("review case \"%s\": bevels%s, clamped %d; ring problems %d, shell breaks %d, cap inverted %d, walls against normals %d\n", c.name, bevs.c_str(), r.bevelClamped, bad, breaks, inv,
            AgainstNormals(m.walls));
         CHECK(bad == 0 && breaks == 0 && inv == 0 && AgainstNormals(m.walls) == 0 && r.bevelClamped > 0, "%s: %s", c.name, what.c_str());
      }
      // The U hole with a bevel its tongue allows (0.5): kept as set, not folded
      BoardMeshes m;
      BoardReport r;
      const std::vector<BoardHole> u = { Poly({ 400, 400, 440, 400, 440, 460, 421, 460, 421, 420, 419, 420, 419, 460, 400, 460 }, 0.5f) };
      BuildBoard(q, u, m, r);
      std::string what;
      CHECK(r.bevel[0] == 0.5f && r.bevelClamped == 0 && RingProblems(q, u, r, what) == 0, "U hole, bevel 0.5 kept (%.3f) %s", r.bevel[0], what.c_str());
   }

   // The floor collider: a Falls Through hole with bevel 6 gets one chamfer (the board's 3 segments do not apply), one with 1.5 (< 2) a straight wall;
   // the backing is the flat face with both at their cut size; no outline walls in the floor
   {
      BoardHole a = Round(150, 300, 60.f, kCut | kFallsThrough), b = Round(300, 300, 27.3f, kCut | kFallsThrough), c = Round(100, 100, 27.3f);
      a.bevel = 6.f;
      b.bevel = 1.5f;
      c.bevel = 6.f;
      BoardMeshes m;
      BoardReport r;
      CHECK(BuildBoard(p, { a, b, c }, m, r), "floor build");
      int inv = 0;
      const double floorArea = CapArea(m.floor, inv); // the walls are vertical or slanted: their xy area is only the chamfer's
      int chamfer = 0, straight = 0, outline = 0;
      double flatArea = 0.;
      std::vector<unsigned> capIdx(m.backing.idx);
      for (size_t t = 0; t < capIdx.size(); t += 3)
      {
         const PlasticMesh::Vertex &p0 = m.floor.verts[capIdx[t]], &p1 = m.floor.verts[capIdx[t + 1]], &p2 = m.floor.verts[capIdx[t + 2]];
         flatArea += 0.5 * ((p1.x - p0.x) * (double)(p2.y - p0.y) - (p1.y - p0.y) * (double)(p2.x - p0.x));
      }
      const size_t capVerts = m.backing.verts.size();
      (void)capVerts;
      for (const PlasticMesh::Vertex& v : m.floor.verts)
      {
         if (v.nz == 1.f)
            continue;
         outline += v.x <= 0.f || v.x >= W || v.y <= 0.f || v.y >= H;
         if (fabsf(v.x - 150.f) < 40.f)
            chamfer += fabsf(v.nz - 0.70710678f) < 1e-5f;
         else
            straight += v.nz == 0.f;
      }
      const double flatWant = (double)W * H - CircleGrown(60.0, 6.0) - CircleGrown(27.3, 0.0);
      int inv2 = 0;
      const double backArea = CapArea(m.backing, inv2), backWant = (double)W * H - CircleGrown(60.0, 0.0) - CircleGrown(27.3, 0.0);
      printf("floor: flat face %.1f (want %.1f: the bevel 6 hole grown, the 1.5 one at its cut), chamfer vertices %d, straight wall vertices %d, outline walls %d; backing %.1f (want %.1f)\n",
         flatArea, flatWant, chamfer, straight, outline, backArea, backWant);
      CHECK(fabs(flatArea - flatWant) < 30. && chamfer > 0 && straight > 0 && outline == 0, "floor: the chamfer only on the bevel >= 2 hole, no outline walls");
      CHECK(fabs(backArea - backWant) < 30. && inv2 == 0, "backing: the holes at their cut size");
      (void)floorArea;
      // One chamfer segment: the floor's wall triangles for hole a are 2 quads per edge, and the floor closes as a shell open at -T
      int rim = 0;
      const float rectEdge[4] = { 0.f, 0.f, W, H };
      CHECK(ShellBreaks({ &m.floor }, -T, rim, rectEdge) == 0, "floor: the flat face and the hole walls close");
      CHECK(AgainstNormals(m.floor) == 0, "floor wound against its normals");
      int intoHole = 0, away = 0;
      for (const PlasticMesh::Vertex& v : m.floor.verts)
         if (v.nz < 0.99f && fabsf(v.x - 300.f) < 20.f && fabsf(v.y - 300.f) < 20.f)
            (v.nx * (300.f - v.x) + v.ny * (300.f - v.y) > 0.f ? intoHole : away)++;
      CHECK(intoHole > 0 && away == 0, "floor walls face into the hole (%d / %d)", intoHole, away);
      int segRing = 0;
      for (const PlasticMesh::Vertex& v : m.floor.verts)
         segRing += fabsf(v.x - 150.f) < 40.f && v.z < 0.f && v.z > -6.f + 1e-3f;
      CHECK(segRing == 0, "floor chamfer is one segment (no intermediate ring), %d", segRing);
   }
}

// ---- P2-3: a traced outline (a rectangle with two edge notches): the board, walls and backing follow it, the floor collider stays its own rectangle,
// a clockwise outline builds the same board, and the leaves-the-board test stays cheap with a 4096-point outline
static bool InsidePoly(const std::vector<float>& xy, float x, float y)
{
   bool in = false;
   const size_t n = xy.size() / 2;
   for (size_t i = 0, j = n - 1; i < n; j = i++)
   {
      const float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
      if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
         in = !in;
   }
   return in;
}

static void Outlines()
{
   const std::vector<float> notched = { 0, 0, 1000, 0, 1000, 2000, 800, 2000, 800, 1940, 700, 1940, 700, 2000, 0, 2000, 0, 420, 60, 420, 60, 300, 0, 300 };
   const std::vector<float> rectOutline = { 0, 0, 1000, 0, 1000, 2000, 0, 2000 };
   const double notchedArea = 2000000. - 60. * 120. - 100. * 60.;
   BoardParams p;
   p.outline = notched;
   p.floorOutline = rectOutline;
   p.thickness = 23.5f;
   p.uvm[0] = 0.001f;
   p.uvm[4] = 0.0005f;
   std::vector<BoardHole> holes = { Round(500, 1000, 40), Round(30, 360, 40), Round(80, 360, 40), Round(200, 360, 40), Round(750, 1700, 60, kCut | kFallsThrough), Round(750, 1960, 40) };
   BoardMeshes m;
   BoardReport r;
   CHECK(BuildBoard(p, holes, m, r), "notched build");
   CHECK(r.overlap == std::vector<char>({ 0, 1, 1, 0, 0, 1 }), "notched overlap flags: in a notch, touching a notch wall, fine, fine, fine, over the notch edge");
   CHECK(r.built == 3, "notched built %d", r.built);
   int inv = 0;
   const double circle40 = M_PI * 20. * 20., circle60 = M_PI * 30. * 30.;
   const double top = CapArea(m.top, inv), back = CapArea(m.backing, inv), floor = CapArea(m.floor, inv);
   printf("outline: top %.0f (want %.0f), backing %.0f (want %.0f), floor %.0f (want %.0f)\n", top, notchedArea - 2 * circle40 - circle60, back, notchedArea - circle60, floor, 2000000. - circle60);
   CHECK(fabs(top - (notchedArea - 2 * circle40 - circle60)) < 100., "top follows the outline");
   CHECK(fabs(back - (notchedArea - circle60)) < 40., "backing follows the outline");
   CHECK(fabs(floor - (2000000. - circle60)) < 40., "the floor stays the rectangle");
   for (const PlasticMesh::Vertex& v : m.floor.verts)
      CHECK(v.x >= -1e-3f && v.x <= 1000.001f && v.y >= -1e-3f && v.y <= 2000.001f, "floor inside the rectangle");
   int floorCorner = 0;
   for (const PlasticMesh::Vertex& v : m.floor.verts)
      floorCorner += v.z == 0.f && (v.x == 0.f && v.y == 0.f);
   CHECK(floorCorner > 0, "the floor keeps the rectangle's corner");
   int topInNotch = 0;
   for (const PlasticMesh::Vertex& v : m.top.verts)
      topInNotch += v.x < 59.f && v.y > 301.f && v.y < 419.f;
   CHECK(topInNotch == 0, "no board in the notch");
   // Walls on the outline: each triangle faces away from the wood (its geometric normal) and its vertex normals agree
   const auto outlineWalls = [&](const BoardMeshes& bm2, const std::vector<float>& ol, int& n, int& bad)
   {
      n = bad = 0;
      for (size_t t = 0; t < bm2.walls.idx.size(); t += 3)
      {
         const PlasticMesh::Vertex &a = bm2.walls.verts[bm2.walls.idx[t]], &b = bm2.walls.verts[bm2.walls.idx[t + 1]], &c = bm2.walls.verts[bm2.walls.idx[t + 2]];
         const float cx = (a.x + b.x + c.x) / 3.f, cy = (a.y + b.y + c.y) / 3.f;
         bool on = false;
         for (size_t i = 0; i < ol.size() / 2 && !on; i++)
         {
            const size_t j = (i + 1) % (ol.size() / 2);
            const float ax = ol[2 * i], ay = ol[2 * i + 1], bx = ol[2 * j], by = ol[2 * j + 1], dx = bx - ax, dy = by - ay;
            const float u = std::clamp(((cx - ax) * dx + (cy - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
            on = hypotf(ax + u * dx - cx, ay + u * dy - cy) < 1e-3f;
         }
         if (!on)
            continue;
         n++;
         const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
         const float gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gl = hypotf(gx, gy);
         const bool geomOut = gl > 0.f && !InsidePoly(ol, cx + 0.5f * gx / gl, cy + 0.5f * gy / gl);
         const bool normalsAgree = a.nx * gx + a.ny * gy > 0.f && b.nx * gx + b.ny * gy > 0.f && c.nx * gx + c.ny * gy > 0.f;
         bad += !(geomOut && normalsAgree);
      }
   };
   int wn, wbad;
   outlineWalls(m, notched, wn, wbad);
   CHECK(wn >= 12 && wbad == 0, "outline walls face away from the wood (%d of %d wrong)", wbad, wn);

   // The same board from a clockwise outline: same counts, same areas, no inward normals
   BoardParams cw = p;
   cw.outline.clear();
   for (size_t i = notched.size() / 2; i-- > 0;)
   {
      cw.outline.push_back(notched[2 * i]);
      cw.outline.push_back(notched[2 * i + 1]);
   }
   cw.floorOutline = { 0, 0, 0, 2000, 1000, 2000, 1000, 0 }; // clockwise too
   BoardMeshes cm;
   BoardReport cr;
   CHECK(BuildBoard(cw, holes, cm, cr), "clockwise build");
   CHECK(cr.overlap == r.overlap && cr.built == r.built, "clockwise: same holes built");
   CHECK(cm.top.idx.size() == m.top.idx.size() && cm.walls.idx.size() == m.walls.idx.size() && cm.backing.idx.size() == m.backing.idx.size() && cm.floor.idx.size() == m.floor.idx.size(), "clockwise: same triangle counts");
   int inv2 = 0;
   CHECK(fabs(CapArea(cm.top, inv2) - top) < 1e-2 && inv2 == 0 && fabs(CapArea(cm.backing, inv2) - back) < 1e-2 && fabs(CapArea(cm.floor, inv2) - floor) < 1e-2, "clockwise: same areas, nothing inverted");
   outlineWalls(cm, notched, wn, wbad);
   CHECK(wn >= 12 && wbad == 0, "clockwise outline: walls face away from the wood (%d of %d wrong)", wbad, wn);

   // Empty floorOutline = the outline (the notch is then in the floor too)
   BoardParams same = p;
   same.floorOutline.clear();
   BoardMeshes sm;
   BoardReport sr;
   CHECK(BuildBoard(same, holes, sm, sr), "same-outline build");
   CHECK(fabs(CapArea(sm.floor, inv) - (notchedArea - circle60)) < 40., "empty floorOutline: the floor follows the outline");

   // Cost of the leaves-the-board test: 4096 outline points, 400 holes
   BoardParams big;
   big.thickness = 23.5f;
   big.uvm[0] = 0.001f;
   big.uvm[4] = 0.0005f;
   for (int i = 0; i < 4096; i++) // a jagged ellipse
   {
      const double a = 2. * M_PI * i / 4096., k = 1. + 0.004 * ((i % 7) - 3);
      big.outline.push_back((float)(500. + 480. * cos(a) * k));
      big.outline.push_back((float)(1000. + 980. * sin(a) * k));
   }
   std::vector<BoardHole> many;
   for (int i = 0; i < 400; i++)
      many.push_back(Round(40.f + 920.f * (float)((i * 37) % 50) / 50.f, 40.f + 1920.f * (float)((i * 11) % 80) / 80.f, 12.f));
   BoardMeshes bm;
   BoardReport br;
   const auto t0 = std::chrono::steady_clock::now();
   const bool ok = BuildBoard(big, many, bm, br);
   const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
   int outside = 0;
   for (size_t i = 0; i < many.size(); i++)
      outside += !InsidePoly(big.outline, many[i].cx, many[i].cy) ? 1 : 0;
   printf("outline 4096 points, 400 holes: %d built, %d overlapping (%d centres outside), %.1f ms\n", br.built, br.overlapping, outside, ms);
   CHECK(ok && br.overlapping >= outside, "big outline build");
}

int main(int argc, char** argv)
{
   std::vector<const char*> files;
   for (int i = 1; i < argc; i++)
      if (strcmp(argv[i], "--exact") == 0)
         gTol = 0.;
      else
         files.push_back(argv[i]);
   if (files.empty())
   {
      files.push_back("tools/fixtures/pf-maker.pfboard.txt");
      files.push_back("tools/fixtures/blank-maker.pfboard.txt");
      files.push_back("tools/fixtures/pf-maker-allcut.pfboard.txt");
   }
   double worst = 0.;
   for (const char* f : files)
      worst = std::max(worst, CheckFixture(f));
   printf("largest difference over all fixtures: %.3g (tolerance %g)\n", worst, gTol);
   Synthetic();
   Bevels();
   Outlines();
   printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
   return fails ? 1 : 0;
}
