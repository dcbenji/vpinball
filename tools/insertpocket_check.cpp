// Standalone check of src/math/InsertPocket.h (the insert body and ink cap of a cut insert pocket):
//   clang++ -std=c++20 -I src tools/insertpocket_check.cpp -o /tmp/ipc && /tmp/ipc
// Also build it with -O2 -ffast-math (the engine's flags).
// For a round insert (both windings), a square, a concave L, a long thin arrow, a contour with repeated points, and degenerate input:
// the shell is closed (welded by position, every directed edge has its reverse exactly once), every triangle's right-hand normal agrees with its
// vertex normals, the top / bottom / cap faces sit at their heights and cover the contour's area, the volume is area x depth (divergence theorem),
// the walls follow the contour exactly (no vertex off it), and every UV is the table mapping of its vertex. No NaN / Inf (bit test, fast-math safe).
#include "math/InsertPocket.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>
using namespace InsertPocket;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool BadBits(const float f) { unsigned u; memcpy(&u, &f, 4); return ((u >> 23) & 0xff) == 0xff; }

struct Key
{
   long x, y, z;
   bool operator<(const Key& o) const { return std::tie(x, y, z) < std::tie(o.x, o.y, o.z); }
   bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
};
static Key K(const Vertex& v) { return { lround(v.x * 1000.), lround(v.y * 1000.), lround(v.z * 1000.) }; }

// Directed edges welded by position: the number without exactly one reverse (an open or doubled edge)
static int OpenEdges(const Mesh& m)
{
   std::map<std::pair<Key, Key>, int> e;
   for (size_t t = 0; t + 2 < m.idx.size(); t += 3)
      for (int i = 0; i < 3; i++)
      {
         const Key a = K(m.verts[m.idx[t + i]]), b = K(m.verts[m.idx[t + (i + 1) % 3]]);
         if (!(a == b))
            e[{ a, b }]++;
      }
   int bad = 0;
   for (const auto& [k, n] : e)
   {
      const auto r = e.find({ k.second, k.first });
      bad += n != 1 || r == e.end() || r->second != 1;
   }
   return bad;
}

static int AgainstNormals(const Mesh& m)
{
   int bad = 0;
   for (size_t t = 0; t + 2 < m.idx.size(); t += 3)
   {
      const Vertex &a = m.verts[m.idx[t]], &b = m.verts[m.idx[t + 1]], &c = m.verts[m.idx[t + 2]];
      const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
      const double cx = uy * wz - uz * wy, cy = uz * wx - ux * wz, cz = ux * wy - uy * wx, l = sqrt(cx * cx + cy * cy + cz * cz);
      if (l < 1e-9)
      {
         bad++; // Build drops slivers: none should be left
         continue;
      }
      bad += (cx * (a.nx + b.nx + c.nx) + cy * (a.ny + b.ny + c.ny) + cz * (a.nz + b.nz + c.nz)) / l <= 0.;
   }
   return bad;
}

// Signed volume (divergence theorem) and the area of the faces at height z facing up (+1) or down (-1)
static double Volume(const Mesh& m)
{
   double v = 0.;
   for (size_t t = 0; t + 2 < m.idx.size(); t += 3)
   {
      const Vertex &a = m.verts[m.idx[t]], &b = m.verts[m.idx[t + 1]], &c = m.verts[m.idx[t + 2]];
      v += (a.x * ((double)b.y * c.z - (double)b.z * c.y) - a.y * ((double)b.x * c.z - (double)b.z * c.x) + a.z * ((double)b.x * c.y - (double)b.y * c.x)) / 6.;
   }
   return v;
}
static double FaceArea(const Mesh& m, const float z, const float nz)
{
   double s = 0.;
   for (size_t t = 0; t + 2 < m.idx.size(); t += 3)
   {
      const Vertex &a = m.verts[m.idx[t]], &b = m.verts[m.idx[t + 1]], &c = m.verts[m.idx[t + 2]];
      if (a.z != z || b.z != z || c.z != z || a.nz != nz)
         continue;
      s += 0.5 * fabs(((double)b.x - a.x) * ((double)c.y - a.y) - ((double)b.y - a.y) * ((double)c.x - a.x));
   }
   return s;
}

static double DistToLoop(const std::vector<CurvePoint>& p, const float x, const float y)
{
   double d = 1e300;
   for (size_t i = 0; i < p.size(); i++)
   {
      const CurvePoint &a = p[i], &b = p[(i + 1) % p.size()];
      const double ex = (double)b.x - a.x, ey = (double)b.y - a.y, l2 = ex * ex + ey * ey;
      const double t = l2 > 0. ? std::fmin(1., std::fmax(0., (((double)x - a.x) * ex + ((double)y - a.y) * ey) / l2)) : 0.;
      d = std::fmin(d, std::hypot(a.x + t * ex - x, a.y + t * ey - y));
   }
   return d;
}

static void Case(const char* name, const std::vector<CurvePoint>& contour, const double area)
{
   Params prm;
   prm.uvm[0] = 1.f / 952.f;
   prm.uvm[2] = 0.01f;
   prm.uvm[4] = 1.f / 2103.f;
   prm.uvm[5] = -0.02f;
   Mesh puck, cap;
   const bool ok = AddPuck(contour, prm, puck);
   const bool okc = AddCap(contour, prm, cap);
   CHECK(ok && okc, "%s: built", name);
   if (!ok || !okc)
      return;
   const float zb = prm.top - prm.depth;
   const int open = OpenEdges(puck), against = AgainstNormals(puck), againstCap = AgainstNormals(cap);
   const double vol = Volume(puck), top = FaceArea(puck, prm.top, 1.f), bot = FaceArea(puck, zb, -1.f), capA = FaceArea(cap, prm.capZ, 1.f);
   int badUv = 0, badZ = 0, nan = 0, off = 0;
   for (const Mesh* m : { &puck, &cap })
      for (const Vertex& v : m->verts)
      {
         badUv += fabs(v.tu - (prm.uvm[0] * v.x + prm.uvm[1] * v.y + prm.uvm[2])) > 1e-6f || fabs(v.tv - (prm.uvm[3] * v.x + prm.uvm[4] * v.y + prm.uvm[5])) > 1e-6f;
         badZ += m == &cap ? v.z != prm.capZ : (v.z != prm.top && v.z != zb);
         for (const float f : { v.x, v.y, v.z, v.nx, v.ny, v.nz, v.tu, v.tv })
            nan += BadBits(f);
         off += DistToLoop(contour, v.x, v.y) > 1e-4 && (m == &cap || v.nz == 0.f); // the walls (and every boundary point) lie on the contour itself
      }
   // Wall normals are horizontal and point out of the insert: a step from a wall triangle's centre along its normal leaves the area (from the centre, not a
   // vertex: at a concave corner a vertex's own edge normal runs along the next wall)
   int inward = 0;
   for (size_t t = 0; t + 2 < puck.idx.size(); t += 3)
   {
      const Vertex &a = puck.verts[puck.idx[t]], &b = puck.verts[puck.idx[t + 1]], &c = puck.verts[puck.idx[t + 2]];
      if (a.nz != 0.f)
         continue;
      const float x = (a.x + b.x + c.x) / 3.f + 0.01f * (a.nx + b.nx + c.nx), y = (a.y + b.y + c.y) / 3.f + 0.01f * (a.ny + b.ny + c.ny);
      bool in = false;
      for (size_t i = 0, j = contour.size() - 1; i < contour.size(); j = i++)
         if (((contour[i].y > y) != (contour[j].y > y)) && (x < (contour[j].x - contour[i].x) * (y - contour[i].y) / (contour[j].y - contour[i].y) + contour[i].x))
            in = !in;
      inward += in;
   }
   printf("%-24s verts %4zu tris %4zu | cap %4zu tris | area %.4f (want %.4f) top %.4f bottom %.4f cap %.4f | volume %.3f (want %.3f)\n", name, puck.verts.size(), puck.idx.size() / 3,
      cap.idx.size() / 3, top, area, top, bot, capA, vol, area * prm.depth);
   CHECK(open == 0, "%s: closed shell (%d open or doubled edges)", name, open);
   CHECK(against == 0 && againstCap == 0, "%s: winding agrees with normals (%d puck, %d cap)", name, against, againstCap);
   CHECK(fabs(top - area) < 1e-3 * area && fabs(bot - area) < 1e-3 * area && fabs(capA - area) < 1e-3 * area, "%s: faces cover the contour", name);
   CHECK(fabs(vol - area * prm.depth) < 2e-3 * area * prm.depth, "%s: volume %.3f vs %.3f", name, vol, area * prm.depth);
   CHECK(badUv == 0 && badZ == 0 && nan == 0, "%s: uv %d z %d nan %d", name, badUv, badZ, nan);
   CHECK(off == 0, "%s: %d wall / cap vertices off the contour", name, off);
   CHECK(inward == 0, "%s: %d wall normals point into the insert", name, inward);
}

static std::vector<CurvePoint> Circle(const float cx, const float cy, const float r, const int n, const bool cw)
{
   std::vector<CurvePoint> p;
   for (int i = 0; i < n; i++)
   {
      const double a = (cw ? -2. : 2.) * M_PI * i / n;
      p.push_back({ static_cast<float>(cx + r * cos(a)), static_cast<float>(cy + r * sin(a)), false });
   }
   return p;
}
static double PolyArea(const std::vector<CurvePoint>& p) { return 0.5 * fabs(Area2(p)); }

int main()
{
   const std::vector<CurvePoint> round = Circle(400.f, 900.f, 22.f, 48, true); // a 1.75 in round insert, clockwise as the board's hole loops
   Case("round, clockwise", round, PolyArea(round));
   const std::vector<CurvePoint> roundCcw = Circle(400.f, 900.f, 22.f, 48, false);
   Case("round, counter-clockwise", roundCcw, PolyArea(roundCcw));
   const std::vector<CurvePoint> square = { { 100, 100, false }, { 140, 100, false }, { 140, 140, false }, { 100, 140, false } };
   Case("square", square, 1600.);
   const std::vector<CurvePoint> ell = { { 0, 0, false }, { 60, 0, false }, { 60, 20, false }, { 20, 20, false }, { 20, 80, false }, { 0, 80, false } };
   Case("concave L", ell, 60. * 20. + 20. * 60.);
   const std::vector<CurvePoint> arrow = { { 500, 1000, false }, { 520, 940, false }, { 540, 1000, false }, { 524, 990, false }, { 524, 1100, false }, { 516, 1100, false }, { 516, 990, false } };
   Case("arrow (thin shaft)", arrow, PolyArea(arrow));
   std::vector<CurvePoint> dup = square;
   dup.insert(dup.begin() + 2, dup[1]); // a repeated point
   dup.push_back(dup[0]); // and the closing point repeated
   Case("repeated points", dup, 1600.);

   // Degenerate: nothing is added
   Params prm;
   Mesh m;
   CHECK(!AddPuck({ { 0, 0, false }, { 1, 0, false } }, prm, m) && m.verts.empty(), "two points: no puck");
   CHECK(!AddPuck({ { 0, 0, false }, { 1, 0, false }, { 2, 0, false } }, prm, m) && m.verts.empty(), "collinear: no puck");
   CHECK(!AddCap({ { 5, 5, false }, { 5, 5, false }, { 5, 5, false }, { 5, 5, false } }, prm, m) && m.verts.empty(), "one point: no cap");
   Params flat = prm;
   flat.depth = 0.f;
   CHECK(!AddPuck(square, flat, m) && m.verts.empty(), "depth 0: no puck");
   // Two pockets into one mesh: indices stay with their own vertices
   Mesh two;
   CHECK(AddPuck(square, prm, two) && AddPuck(round, prm, two), "two pucks");
   CHECK(OpenEdges(two) == 0 && AgainstNormals(two) == 0, "two pucks in one mesh: closed and wound right");
   printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
   return fails ? 1 : 0;
}
