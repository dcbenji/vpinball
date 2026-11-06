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
PlasticMesh::Hole ToMeshHole(const BoardHole& h)
{
   PlasticMesh::Hole m;
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

// The board top at z = 0 of the outline with these holes cut
bool BuildTop(const std::vector<CurvePoint>& outline, const std::vector<const BoardHole*>& holes, const BoardParams& bp, PlasticMesh::Mesh& top)
{
   std::vector<PlasticMesh::Hole> cut;
   for (const BoardHole* const hole : holes)
      cut.push_back(ToMeshHole(*hole));
   PlasticMesh::Params params;
   params.bottomCap = false; // Its bottom cap would face up into the wood and is never seen
   params.height = -bp.thickness;
   params.thickness = bp.thickness;
   params.bevel = 0.f;
   params.bevelSegments = 1;
   params.flatten = PlasticMesh::FlattenTolerance(bp.detail);
   for (int i = 0; i < 6; i++)
      params.uvm[i] = bp.uvm[i];
   PlasticMesh::Result res;
   if (!PlasticMesh::Build(outline, cut, params, res))
      return false;
   top = std::move(res.top);
   return true;
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

// A hole loop reaches the board edge or leaves the board. A rectangular outline: its box is touched or passed (the edge is part of the cut). Any other: a point outside, or an edge touching the outline.
bool LeavesBoard(const std::vector<CurvePoint>& loop, const Box& b, const std::vector<CurvePoint>& outline, const bool rect, const Box& ob)
{
   if (rect)
      return b.left <= ob.left || b.right >= ob.right || b.top <= ob.top || b.bottom >= ob.bottom;
   for (const CurvePoint& p : loop)
      if (!InsideLoop(outline, p.x, p.y))
         return true;
   for (size_t i = 0; i < loop.size(); i++)
      for (size_t j = 0; j < outline.size(); j++)
         if (SegmentsTouch(loop[i], loop[(i + 1) % loop.size()], outline[j], outline[(j + 1) % outline.size()]))
            return true;
   return false;
}
} // namespace

bool BuildBoard(const BoardParams& params, const std::vector<BoardHole>& holes, BoardMeshes& meshes, BoardReport& report)
{
   report = BoardReport();
   const std::vector<CurvePoint> outline = OutlinePoints(params.outline);
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

   std::vector<const BoardHole*> cut, falls;
   for (size_t i = 0; i < n; i++)
      if ((holes[i].flags & kCut) && loops[i].size() >= 3 && !report.overlap[i])
      {
         cut.push_back(&holes[i]);
         if (holes[i].flags & kFallsThrough)
            falls.push_back(&holes[i]);
      }

   BoardMeshes out;
   if (!BuildTop(outline, cut, params, out.top) || !BuildTop(outline, falls, params, out.floor))
      return false;
   report.built = static_cast<int>(cut.size());

   // Walls: the board edge and every built hole, then the backing (the floor face lowered, all on the dark rows of the plywood image)
   AddWalls(outline, params.thickness, out.walls);
   for (size_t i = 0; i < n; i++)
      if ((holes[i].flags & kCut) && loops[i].size() >= 3 && !report.overlap[i])
         AddWalls(loops[i], params.thickness, out.walls);
   out.backing = out.floor;
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
   meshes = std::move(out);
   return true;
}

}
