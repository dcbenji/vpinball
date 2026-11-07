// license:GPLv3+

#include "PlayfieldBoard.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace PlayfieldBoard
{
namespace
{
using PlasticMesh::CurvePoint;

struct Vec2
{
   float x, y;
};

struct Box
{
   float left, top, right, bottom;
};

// The mesh builder's hole (the one place the hole kinds map to PlasticMesh)
PlasticMesh::Hole ToMeshHole(const BoardHole& h, const float bevel = 0.f)
{
   PlasticMesh::Hole m;
   m.bevel = bevel; // Resolved and clamped by BuildBoard: Build uses it as is
   m.x = h.cx;
   m.y = h.cy;
   m.diameter = h.diameter;
   m.x2 = h.ex;
   m.y2 = h.ey;
   m.pill = h.kind == kSlot;
   if (h.kind == kPolygon)
      for (size_t i = 0; i + 1 < h.polygon.size(); i += 2)
         m.polygon.push_back({ h.polygon[i], h.polygon[i + 1], false });
   return m;
}

// The hole as the closed loop it is cut as (clockwise), flattened to the chord error tol
std::vector<CurvePoint> HoleLoop(const BoardHole& h, const float tol) { return PlasticMesh::HoleOutline(ToMeshHole(h), tol); }

std::vector<CurvePoint> OutlinePoints(const std::vector<float>& xy)
{
   std::vector<CurvePoint> res;
   for (size_t i = 0; i + 1 < xy.size(); i += 2)
      res.push_back({ xy[i], xy[i + 1], false });
   return res;
}

// The outline counter-clockwise (shoelace area > 0, the way the walls, the top and the clamps expect it; PlasticMesh::Build reverses a clockwise outline by
// itself, AddWalls does not: a clockwise traced outline would give inward wall normals)
void MakeCounterClockwise(std::vector<CurvePoint>& p)
{
   double a = 0.0;
   for (size_t i = 0; i < p.size(); i++)
   {
      const CurvePoint &u = p[i], &v = p[(i + 1) % p.size()];
      a += (double)u.x * v.y - (double)v.x * u.y;
   }
   if (a < 0.0)
      std::reverse(p.begin(), p.end());
}

// Two edges cross, or come within 0.01 VPU of each other: holes that touch, share an edge or lie on each other count as overlapping
bool SegmentsTouch(const CurvePoint& a, const CurvePoint& b, const CurvePoint& c, const CurvePoint& d)
{
   const auto orient = [](const CurvePoint& p, const CurvePoint& q, const CurvePoint& r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
   const float d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
   if (((d1 > 0.f) != (d2 > 0.f)) && ((d3 > 0.f) != (d4 > 0.f)))
      return true;
   const auto dist2 = [](const CurvePoint& p, const CurvePoint& q, const CurvePoint& r) // p to the segment q r
   {
      const float ex = r.x - q.x, ey = r.y - q.y, l2 = ex * ex + ey * ey;
      const float t = l2 > 1e-12f ? std::clamp(((p.x - q.x) * ex + (p.y - q.y) * ey) / l2, 0.f, 1.f) : 0.f;
      const float dx = q.x + t * ex - p.x, dy = q.y + t * ey - p.y;
      return dx * dx + dy * dy;
   };
   constexpr float eps2 = 0.01f * 0.01f;
   return dist2(a, c, d) <= eps2 || dist2(b, c, d) <= eps2 || dist2(c, a, b) <= eps2 || dist2(d, a, b) <= eps2;
}

bool InsideLoop(const std::vector<CurvePoint>& loop, const float x, const float y)
{
   bool in = false;
   for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
      if (((loop[i].y > y) != (loop[j].y > y)) && (x < (loop[j].x - loop[i].x) * (y - loop[i].y) / (loop[j].y - loop[i].y) + loop[i].x))
         in = !in;
   return in;
}

// The board top at z = 0 of the outline with these holes cut, each grown by its bevel (bevels[i], already clamped; the outline is never inset).
// res also holds each hole's contour and top ring as Build used them (Result::contourOfHole maps holes[i] to them)
bool BuildTop(const std::vector<CurvePoint>& outline, const std::vector<const BoardHole*>& holes, const std::vector<float>& bevels, const BoardParams& bp, PlasticMesh::Result& res)
{
   std::vector<PlasticMesh::Hole> cut;
   for (size_t i = 0; i < holes.size(); i++)
      cut.push_back(ToMeshHole(*holes[i], bevels[i]));
   PlasticMesh::Params params;
   params.bottomCap = false; // Its bottom cap would face up into the wood and is never seen
   params.height = -bp.thickness;
   params.thickness = bp.thickness;
   params.bevel = 0.f;
   params.outlineBevel = 0.f;
   params.bevelSegments = 1;
   params.flatten = PlasticMesh::FlattenTolerance(bp.detail);
   for (int i = 0; i < 6; i++)
      params.uvm[i] = bp.uvm[i];
   return PlasticMesh::Build(outline, cut, params, res);
}

// Straight walls from z = 0 down to -thickness along a closed loop. outward: the loop runs counter-clockwise (the board edge), else clockwise (a hole).
// The right-hand normal of each edge points away from the wood either way. u = running length / (4 thickness), v = depth / thickness
void AddWalls(const std::vector<CurvePoint>& loop, const float thickness, PlasticMesh::Mesh& m)
{
   const size_t n = loop.size();
   if (n < 3)
      return;
   std::vector<Vec2> en(n); // right normal of edge i -> i+1
   for (size_t i = 0; i < n; i++)
   {
      const CurvePoint &a = loop[i], &b = loop[(i + 1) % n];
      const float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
      en[i] = l > 1e-6f ? Vec2 { dy / l, -dx / l } : Vec2 { 0.f, 0.f };
   }
   const auto vertexNormal = [&](const size_t i, const size_t edge) // smooth across shallow turns (circles), sharp at corners
   {
      const Vec2 &p = en[(i + n - 1) % n], &q = en[i % n];
      if (p.x * q.x + p.y * q.y > 0.866f)
      {
         const Vec2 s { p.x + q.x, p.y + q.y };
         const float l = sqrtf(s.x * s.x + s.y * s.y);
         return l > 1e-6f ? Vec2 { s.x / l, s.y / l } : en[edge];
      }
      return en[edge];
   };
   float along = 0.f;
   const float uScale = 1.f / (4.f * thickness);
   for (size_t i = 0; i < n; i++)
   {
      const size_t j = (i + 1) % n;
      const CurvePoint &a = loop[i], &b = loop[j];
      const float l = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
      if (l <= 1e-6f)
         continue;
      const Vec2 na = vertexNormal(i, i), nb = vertexNormal(j, i);
      const unsigned base = static_cast<unsigned>(m.verts.size());
      const float u0 = along * uScale, u1 = (along + l) * uScale;
      m.verts.push_back({ a.x, a.y, -thickness, na.x, na.y, 0.f, u0, 1.f });
      m.verts.push_back({ b.x, b.y, -thickness, nb.x, nb.y, 0.f, u1, 1.f });
      m.verts.push_back({ b.x, b.y, 0.f, nb.x, nb.y, 0.f, u1, 0.f });
      m.verts.push_back({ a.x, a.y, 0.f, na.x, na.y, 0.f, u0, 0.f });
      m.idx.insert(m.idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 }); // As PlasticMesh's walls: (a0, a1, b1), (a0, b1, b0)
      along += l;
   }
}

// A hole's wall from -thickness up to z = -bevel, then its bevel up to its top ring at z = 0: rings j = 1..segs at contour + (topRing - contour) (1 - cos th_j),
// z = -bevel + bevel sin th_j, th_j = 90 deg j / segs, normal (n cos th_j, sin th_j) with n the wall's own horizontal normal (one segment: a chamfer,
// its normal at 45 deg). u as the walls; v = depth / thickness on the wall, then along the profile's length from the wall top (v = bevel / thickness)
// to the surface (v = 0), so the bevel samples the image's top rows (the face ply) and never wraps past them. With bevel 0, the straight wall.
void AddHoleWalls(const std::vector<CurvePoint>& loop, const std::vector<CurvePoint>& topRing, const float bevel, int segs, const float thickness, PlasticMesh::Mesh& m)
{
   if (!(bevel > 0.f) || topRing.size() != loop.size())
   {
      AddWalls(loop, thickness, m);
      return;
   }
   const size_t n = loop.size();
   if (n < 3)
      return;
   std::vector<Vec2> en(n); // right normal of edge i -> i+1 (into the hole), as AddWalls
   for (size_t i = 0; i < n; i++)
   {
      const CurvePoint &a = loop[i], &b = loop[(i + 1) % n];
      const float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
      en[i] = l > 1e-6f ? Vec2 { dy / l, -dx / l } : Vec2 { 0.f, 0.f };
   }
   const auto vertexNormal = [&](const size_t i, const size_t edge)
   {
      const Vec2 &p = en[(i + n - 1) % n], &q = en[i % n];
      if (p.x * q.x + p.y * q.y > 0.866f)
      {
         const Vec2 s { p.x + q.x, p.y + q.y };
         const float l = sqrtf(s.x * s.x + s.y * s.y);
         return l > 1e-6f ? Vec2 { s.x / l, s.y / l } : en[edge];
      }
      return en[edge];
   };
   // The profile rings: horizontal fraction of (topRing - contour), z, normal weights. Ring 0 is the wall top, ring segs the top ring itself
   segs = std::max(1, segs);
   struct Ring
   {
      float f, z, nh, nz;
   };
   std::vector<Ring> rings;
   for (int j = 0; j <= segs; j++)
   {
      const double th = 0.5 * 3.14159265358979323846 * j / segs;
      const double nth = segs == 1 ? 0.25 * 3.14159265358979323846 : th;
      rings.push_back({ j == segs ? 1.f : static_cast<float>(1.0 - std::cos(th)), j == segs ? 0.f : static_cast<float>(-bevel + bevel * std::sin(th)), static_cast<float>(std::cos(nth)),
         static_cast<float>(std::sin(nth)) });
   }
   const auto at = [&](const size_t i, const int j)
   {
      const float f = rings[j].f;
      return j == segs ? Vec2 { topRing[i].x, topRing[i].y } : Vec2 { loop[i].x + (topRing[i].x - loop[i].x) * f, loop[i].y + (topRing[i].y - loop[i].y) * f };
   };
   // v along the profile, per vertex (each vertex moves by its own mitre)
   std::vector<float> len(n, 0.f);
   std::vector<std::vector<float>> run(n, std::vector<float>(segs + 1, 0.f));
   for (size_t i = 0; i < n; i++)
   {
      for (int j = 1; j <= segs; j++)
      {
         const Vec2 p = at(i, j - 1), q = at(i, j);
         const float dz = rings[j].z - rings[j - 1].z;
         run[i][j] = run[i][j - 1] + sqrtf((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + dz * dz);
      }
      len[i] = run[i][segs];
   }
   const float vTop = bevel / thickness; // The wall top
   const auto vOf = [&](const size_t i, const int j) { return len[i] > 1e-6f ? vTop * (1.f - run[i][j] / len[i]) : vTop * (1.f - static_cast<float>(j) / segs); };
   float along = 0.f;
   const float uScale = 1.f / (4.f * thickness);
   for (size_t i = 0; i < n; i++)
   {
      const size_t j = (i + 1) % n;
      const CurvePoint &a = loop[i], &b = loop[j];
      const float l = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
      if (l <= 1e-6f)
         continue;
      const Vec2 na = vertexNormal(i, i), nb = vertexNormal(j, i);
      const float u0 = along * uScale, u1 = (along + l) * uScale;
      unsigned base = static_cast<unsigned>(m.verts.size());
      if (bevel < thickness) // A bevel as deep as the wood leaves no straight wall
      {
         m.verts.push_back({ a.x, a.y, -thickness, na.x, na.y, 0.f, u0, 1.f });
         m.verts.push_back({ b.x, b.y, -thickness, nb.x, nb.y, 0.f, u1, 1.f });
         m.verts.push_back({ b.x, b.y, -bevel, nb.x, nb.y, 0.f, u1, vTop });
         m.verts.push_back({ a.x, a.y, -bevel, na.x, na.y, 0.f, u0, vTop });
         m.idx.insert(m.idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
      }
      for (int r = 0; r < segs; r++)
      {
         const Ring &lo = rings[r], &hi = rings[r + 1];
         const Vec2 pa0 = at(i, r), pb0 = at(j, r), pa1 = at(i, r + 1), pb1 = at(j, r + 1);
         base = static_cast<unsigned>(m.verts.size());
         m.verts.push_back({ pa0.x, pa0.y, lo.z, na.x * lo.nh, na.y * lo.nh, lo.nz, u0, vOf(i, r) });
         m.verts.push_back({ pb0.x, pb0.y, lo.z, nb.x * lo.nh, nb.y * lo.nh, lo.nz, u1, vOf(j, r) });
         m.verts.push_back({ pb1.x, pb1.y, hi.z, nb.x * hi.nh, nb.y * hi.nh, hi.nz, u1, vOf(j, r + 1) });
         m.verts.push_back({ pa1.x, pa1.y, hi.z, na.x * hi.nh, na.y * hi.nh, hi.nz, u0, vOf(i, r + 1) });
         m.idx.insert(m.idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
      }
      along += l;
   }
}

double SegDist(const CurvePoint& p, const CurvePoint& a, const CurvePoint& b) // p to the segment a b
{
   const double ex = (double)b.x - a.x, ey = (double)b.y - a.y, l2 = ex * ex + ey * ey;
   const double t = l2 > 0.0 ? std::clamp((((double)p.x - a.x) * ex + ((double)p.y - a.y) * ey) / l2, 0.0, 1.0) : 0.0;
   const double dx = a.x + t * ex - p.x, dy = a.y + t * ey - p.y;
   return std::sqrt(dx * dx + dy * dy);
}

// The least distance between two closed loops that neither cross nor nest (the overlap sweep left those out): the least point-to-segment distance both ways
double LoopDist(const std::vector<CurvePoint>& a, const std::vector<CurvePoint>& b, const bool bClosed = true)
{
   double d = 1e300;
   const size_t nb = bClosed ? b.size() : b.size() - 1;
   for (const CurvePoint& p : a)
      for (size_t q = 0; q < nb; q++)
         d = std::min(d, SegDist(p, b[q], b[(q + 1) % b.size()]));
   for (const CurvePoint& p : b)
      for (size_t q = 0; q < a.size(); q++)
         d = std::min(d, SegDist(p, a[q], a[(q + 1) % a.size()]));
   return d;
}

Box BoxOf(const std::vector<CurvePoint>& pts)
{
   Box b { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
   for (const CurvePoint& p : pts)
   {
      b.left = std::min(b.left, p.x);
      b.right = std::max(b.right, p.x);
      b.top = std::min(b.top, p.y);
      b.bottom = std::max(b.bottom, p.y);
   }
   return b;
}

// The outline is the four corners of an axis-aligned rectangle (what the editor passes): holes are tested against its box, as the board always did
bool IsAxisRect(const std::vector<CurvePoint>& o)
{
   if (o.size() != 4)
      return false;
   for (size_t i = 0; i < 4; i++)
   {
      const CurvePoint &a = o[i], &b = o[(i + 1) % 4];
      if (a.x != b.x && a.y != b.y)
         return false;
   }
   return true;
}

// A hole loop reaches the board edge or leaves the board. A rectangular outline: its box is touched or passed (the edge is part of the cut). Any other: a point
// outside, or an edge touching the outline. Cheap enough for every hole drag (a traced outline has up to 4096 points): the hole's box is rejected against the
// outline's box first, only the outline edges near the hole's box meet the hole's edges, and with no edge touching, one point of the loop decides inside or out
bool LeavesBoard(const std::vector<CurvePoint>& loop, const Box& b, const std::vector<CurvePoint>& outline, const bool rect, const Box& ob)
{
   if (rect)
      return b.left <= ob.left || b.right >= ob.right || b.top <= ob.top || b.bottom >= ob.bottom;
   if (b.left < ob.left || b.right > ob.right || b.top < ob.top || b.bottom > ob.bottom)
      return true; // some of the loop is outside the outline's box
   constexpr float g = 0.01f; // SegmentsTouch's distance
   for (size_t j = 0; j < outline.size(); j++)
   {
      const CurvePoint &c = outline[j], &d = outline[(j + 1) % outline.size()];
      if (std::max(c.x, d.x) < b.left - g || std::min(c.x, d.x) > b.right + g || std::max(c.y, d.y) < b.top - g || std::min(c.y, d.y) > b.bottom + g)
         continue;
      for (size_t i = 0; i < loop.size(); i++)
         if (SegmentsTouch(loop[i], loop[(i + 1) % loop.size()], c, d))
            return true;
   }
   return !InsideLoop(outline, loop[0].x, loop[0].y); // No edge touches: the whole loop is on one side
}
// A hole's grown top ring folded: an edge runs against its contour edge (a strip of wood between two parts of a concave hole narrower than about twice
// the bevel), or two of its edges cross. Convex holes never fold
bool RingFolds(const std::vector<CurvePoint>& contour, const std::vector<CurvePoint>& ring)
{
   const size_t n = ring.size();
   if (n != contour.size() || n < 3)
      return false;
   for (size_t i = 0; i < n; i++)
   {
      const CurvePoint &c0 = contour[i], &c1 = contour[(i + 1) % n], &r0 = ring[i], &r1 = ring[(i + 1) % n];
      if ((double)(c1.x - c0.x) * (r1.x - r0.x) + (double)(c1.y - c0.y) * (r1.y - r0.y) <= 0.0)
         return true;
   }
   const auto orient = [](const CurvePoint& p, const CurvePoint& q, const CurvePoint& r) { return ((double)q.x - p.x) * ((double)r.y - p.y) - ((double)q.y - p.y) * ((double)r.x - p.x); };
   for (size_t i = 0; i < n; i++)
      for (size_t j = i + 2; j < n; j++)
      {
         if (i == 0 && j == n - 1)
            continue; // adjacent through the closing edge
         const CurvePoint &a = ring[i], &b = ring[(i + 1) % n], &c = ring[j], &d = ring[(j + 1) % n];
         if (std::max(a.x, b.x) < std::min(c.x, d.x) || std::max(c.x, d.x) < std::min(a.x, b.x) || std::max(a.y, b.y) < std::min(c.y, d.y) || std::max(c.y, d.y) < std::min(a.y, b.y))
            continue;
         const double d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
         if (((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) && ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0)))
            return true;
      }
   return false;
}

// The longest mitre of a loop (as PlasticMesh's Analyse: 1 / cos(half the turn), at most 2): the grown ring moves no point farther than bevel x this
double MaxMitre(const std::vector<CurvePoint>& p)
{
   const size_t n = p.size();
   double m = 1.0;
   for (size_t i = 0; i < n; i++)
   {
      const CurvePoint &a = p[(i + n - 1) % n], &b = p[i], &c = p[(i + 1) % n];
      double ux = (double)b.x - a.x, uy = (double)b.y - a.y, vx = (double)c.x - b.x, vy = (double)c.y - b.y;
      const double lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);
      if (lu <= 1e-12 || lv <= 1e-12)
         continue;
      const double d = (ux * vx + uy * vy) / (lu * lv);
      m = std::max(m, 1.0 + d < 1e-6 ? 1.0 : std::min(2.0, std::sqrt(2.0 / (1.0 + d))));
   }
   return m;
}
} // namespace

bool BuildBoard(const BoardParams& params, const std::vector<BoardHole>& holes, BoardMeshes& meshes, BoardReport& report)
{
   report = BoardReport();
   std::vector<CurvePoint> outline = OutlinePoints(params.outline);
   if (outline.size() >= 3)
      MakeCounterClockwise(outline);
   std::vector<CurvePoint> floorOutline = OutlinePoints(params.floorOutline);
   if (floorOutline.size() >= 3)
      MakeCounterClockwise(floorOutline);
   else
      floorOutline.clear(); // empty (or unusable) = the board's outline
   const bool ownFloorOutline = !floorOutline.empty();
   const float tol = PlasticMesh::FlattenTolerance(params.detail);
   const bool haveOutline = outline.size() >= 3;
   const bool rect = haveOutline && IsAxisRect(outline);
   const Box ob = haveOutline ? BoxOf(outline) : Box { 0.f, 0.f, 0.f, 0.f };

   // Cut holes that leave the board or touch another cut hole are left out until moved
   const size_t n = holes.size();
   std::vector<std::vector<CurvePoint>> loops(n);
   std::vector<Box> box(n);
   report.overlap.assign(n, 0);
   std::vector<size_t> order;
   for (size_t i = 0; i < n; i++)
   {
      if (!(holes[i].flags & kCut))
         continue;
      loops[i] = HoleLoop(holes[i], tol);
      if (loops[i].size() < 3)
      {
         report.skipped++;
         continue;
      }
      box[i] = BoxOf(loops[i]);
      if (haveOutline && LeavesBoard(loops[i], box[i], outline, rect, ob))
         report.overlap[i] = 1;
      order.push_back(i);
   }
   std::ranges::sort(order, [&box](const size_t a, const size_t b) { return box[a].left < box[b].left; });
   for (size_t oi = 0; oi < order.size(); oi++) // ponytail: sweep on x, then every edge pair; a grid if PF-scale masks make this slow
      for (size_t oj = oi + 1; oj < order.size() && box[order[oj]].left <= box[order[oi]].right; oj++)
      {
         const size_t i = order[oi], j = order[oj];
         if (box[j].top > box[i].bottom || box[j].bottom < box[i].top)
            continue;
         const auto &a = loops[i], &b = loops[j];
         bool touch = InsideLoop(a, b[0].x, b[0].y) || InsideLoop(b, a[0].x, a[0].y);
         for (size_t p = 0; p < a.size() && !touch; p++)
            for (size_t q = 0; q < b.size() && !touch; q++)
               touch = SegmentsTouch(a[p], a[(p + 1) % a.size()], b[q], b[(q + 1) % b.size()]);
         if (touch)
            report.overlap[i] = report.overlap[j] = 1;
      }
   for (size_t i = 0; i < n; i++)
      if (report.overlap[i])
         report.overlapping++;
   if (report.overlapping > 0)
      report.warnings.push_back(std::to_string(report.overlapping) + " holes leave the board or touch another hole and are left out");

   if (!haveOutline || ob.right <= ob.left || ob.bottom <= ob.top)
      return false; // Degenerate board: the caller keeps its meshes

   // Built holes, in input order, and their bevels: resolved (-1 = the default, insert pockets 0), capped at the thickness, then clamped against the
   // board edge (the outline is never inset: the grown top ring stays 0.1 clear of it) and against every other hole (each to under half their gap)
   std::vector<size_t> built;
   for (size_t i = 0; i < n; i++)
      if ((holes[i].flags & kCut) && loops[i].size() >= 3 && !report.overlap[i])
         built.push_back(i);
   const float thick = params.thickness;
   std::vector<float> want(n, 0.f), bev(n, 0.f);
   float maxWant = 0.f;
   for (const size_t i : built)
   {
      const BoardHole& h = holes[i];
      const float b = (h.flags & kUnderInsert) ? 0.f : h.bevel < 0.f ? params.defaultBevel : h.bevel;
      want[i] = std::max(0.f, b);
      bev[i] = std::min(want[i], thick);
      maxWant = std::max(maxWant, bev[i]);
   }
   constexpr double kGap = 0.1;
   std::vector<double> mm(n, 1.0);
   for (const size_t i : built)
      mm[i] = MaxMitre(loops[i]);
   if (maxWant > 0.f)
   {
      for (const size_t i : built) // The board edge
      {
         if (!(bev[i] > 0.f))
            continue;
         double d;
         if (rect)
            d = std::min(std::min(box[i].left - ob.left, ob.right - box[i].right), std::min(box[i].top - ob.top, ob.bottom - box[i].bottom));
         else
         {
            d = 1e300;
            const float g = static_cast<float>(bev[i] * mm[i] + kGap);
            for (size_t q = 0; q < outline.size(); q++) // Only the outline edges near the hole's box grown by its bevel
            {
               const CurvePoint &a = outline[q], &b = outline[(q + 1) % outline.size()];
               if (std::max(a.x, b.x) < box[i].left - g || std::min(a.x, b.x) > box[i].right + g || std::max(a.y, b.y) < box[i].top - g || std::min(a.y, b.y) > box[i].bottom + g)
                  continue;
               const std::vector<CurvePoint> seg { a, b };
               d = std::min(d, LoopDist(loops[i], seg, false));
            }
         }
         bev[i] = std::min(bev[i], static_cast<float>(std::max(0.0, (d - kGap) / mm[i])));
      }
      std::vector<float> cap(n, FLT_MAX); // Hole to hole, from the resolved bevels (so the result does not depend on the order)
      std::vector<size_t> byLeft = built;
      std::ranges::sort(byLeft, [&box](const size_t a, const size_t b) { return box[a].left < box[b].left; });
      for (size_t oi = 0; oi < byLeft.size(); oi++)
         for (size_t oj = oi + 1; oj < byLeft.size() && box[byLeft[oj]].left <= box[byLeft[oi]].right + 4.f * maxWant + static_cast<float>(kGap); oj++)
         {
            const size_t i = byLeft[oi], j = byLeft[oj];
            const float g = static_cast<float>(std::min(want[i], thick) * mm[i] + std::min(want[j], thick) * mm[j]) + static_cast<float>(kGap);
            if (!(g > static_cast<float>(kGap)) || box[j].left > box[i].right + g || box[j].top > box[i].bottom + g || box[j].bottom < box[i].top - g)
               continue;
            const double d = LoopDist(loops[i], loops[j]);
            if (g > d)
            {
               const float c = static_cast<float>(std::max(0.0, (d - kGap) / (mm[i] + mm[j])));
               cap[i] = std::min(cap[i], c);
               cap[j] = std::min(cap[j], c);
            }
         }
      for (const size_t i : built)
         bev[i] = std::min(bev[i], cap[i]);
   }
   for (const size_t i : built)
   {
      if (bev[i] < 1e-4f)
         bev[i] = 0.f;
      else if (bev[i] > thick - 1e-3f)
         bev[i] = thick; // No sliver of straight wall under a bevel as deep as the wood
   }

   // The top, then the folds: a concave hole whose grown ring folds gets half its bevel and the top is built again (4 times at most, then none)
   std::vector<const BoardHole*> cut;
   for (const size_t i : built)
      cut.push_back(&holes[i]);
   PlasticMesh::Result topRes;
   for (int pass = 0;; pass++)
   {
      std::vector<float> cutBevel;
      for (const size_t i : built)
         cutBevel.push_back(bev[i]);
      if (!BuildTop(outline, cut, cutBevel, params, topRes))
         return false;
      bool again = false;
      for (size_t c = 0; c < built.size(); c++)
         if (const int k = topRes.contourOfHole[c]; k >= 0 && topRes.effBevel[k] > 0.f && RingFolds(topRes.contours[k], topRes.topRings[k]))
         {
            const size_t i = built[c];
            bev[i] = pass < 4 && bev[i] * 0.5f >= 1e-4f ? bev[i] * 0.5f : 0.f;
            again = true;
         }
      if (!again)
         break;
   }

   report.bevel.assign(n, 0.f);
   report.clamped.assign(n, 0);
   for (const size_t i : built)
   {
      report.bevel[i] = bev[i];
      if (bev[i] < want[i])
      {
         report.clamped[i] = 1;
         report.bevelClamped++;
      }
   }
   if (report.bevelClamped > 0)
      report.warnings.push_back(std::to_string(report.bevelClamped) + " hole bevels are smaller than set: near the board edge or another hole, too wide for the shape, or over the thickness");

   std::vector<const BoardHole*> falls;
   std::vector<float> fallsBevel, fallsFlat;
   for (const size_t i : built)
      if (holes[i].flags & kFallsThrough)
      {
         falls.push_back(&holes[i]);
         fallsBevel.push_back(bev[i] >= kFloorBevelMin ? bev[i] : 0.f);
         fallsFlat.push_back(0.f);
      }

   BoardMeshes out;
   PlasticMesh::Result floorRes;
   if (!BuildTop(ownFloorOutline ? floorOutline : outline, falls, fallsBevel, params, floorRes))
      return false;
   report.built = static_cast<int>(cut.size());
   out.top = std::move(topRes.top);

   // Walls: the board edge and every built hole with its bevel, from the contours and top rings Build used (never re-flattened)
   AddWalls(outline, params.thickness, out.walls);
   for (size_t c = 0; c < cut.size(); c++)
      if (const int k = topRes.contourOfHole[c]; k >= 0 && !(cut[c]->flags & kUnderInsert)) // An insert pocket has no plywood wall: the insert fills it (its rim, below)
         AddHoleWalls(topRes.contours[k], topRes.topRings[k], topRes.effBevel[k], params.bevelSegments, params.thickness, out.walls);

   // The floor collider: the flat face with the Falls Through holes cut, then their walls (normals into the hole: they keep a dropped ball in), with one
   // chamfer for a bevel of kFloorBevelMin or more. The backing is the flat face with the holes at their cut size, lowered below the wood, on the dark
   // rows of the plywood image
   out.floor = std::move(floorRes.top);
   const bool anyFloorBevel = std::ranges::any_of(fallsBevel, [](const float b) { return b > 0.f; });
   if (anyFloorBevel || ownFloorOutline) // The backing follows the board's outline, the floor its own
   {
      PlasticMesh::Result flat;
      if (!BuildTop(outline, falls, fallsFlat, params, flat))
         return false;
      out.backing = std::move(flat.top);
   }
   else
      out.backing = out.floor;
   for (size_t c = 0; c < falls.size(); c++)
      if (const int k = floorRes.contourOfHole[c]; k >= 0)
         AddHoleWalls(floorRes.contours[k], floorRes.topRings[k], floorRes.effBevel[k], 1, params.thickness, out.floor);
   // Insert pockets: an insert body and an ink cap on each built Under Insert hole's contour (bevel 0, so its top ring is the contour), and the pocket's floor
   // and rim (its side walls in the art's colour at the rim, facing in: no plywood wall) at the body's bottom (opaque, facing up: what the see-through body shows below it instead of the walls and backing)
   InsertPocket::Params ip;
   for (int i = 0; i < 6; i++)
      ip.uvm[i] = params.uvm[i];
   const float bottom = std::max(0.2f, std::min(params.insertDepth, params.thickness - 0.5f)); // Never below the wood (the backing is at -thickness - 0.5)
   ip.depth = bottom + ip.top; // top is -0.1
   InsertPocket::Params fp = ip;
   fp.capZ = -bottom;
   for (size_t c = 0; c < cut.size(); c++)
      if (const int k = topRes.contourOfHole[c]; k >= 0 && (cut[c]->flags & kUnderInsert))
         if (((cut[c]->flags & kPlacedBody) || InsertPocket::AddPuck(topRes.contours[k], ip, out.inserts)) && InsertPocket::AddCap(topRes.contours[k], ip, out.insertCaps)) // A placed body: the author's own part fills the pocket
         {
            InsertPocket::AddCap(topRes.contours[k], fp, out.pocketFloors);
            InsertPocket::AddRim(topRes.contours[k], ip, 0.f, -bottom, kRimInset, out.pocketFloors);
            report.pockets++;
         }
   for (PlasticMesh::Vertex& v : out.backing.verts)
   {
      v.z = -params.thickness - 0.5f;
      v.tu = 0.5f;
      v.tv = 0.995f;
   }

   report.topTriangles = out.top.idx.size() / 3;
   report.wallTriangles = out.walls.idx.size() / 3;
   report.backingTriangles = out.backing.idx.size() / 3;
   report.floorTriangles = out.floor.idx.size() / 3;
   report.insertTriangles = out.inserts.idx.size() / 3;
   report.capTriangles = out.insertCaps.idx.size() / 3;
   meshes = std::move(out);
   return true;
}

}
