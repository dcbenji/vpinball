// license:GPLv3+

#include "PlasticMesh.h"
#include "earcut.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PlasticMesh
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kSharp = 30.0 * kPi / 180.0; // normals split at corners sharper than this

struct P2
{
   double x, y;
};
inline P2 operator+(P2 a, P2 b) { return { a.x + b.x, a.y + b.y }; }
inline P2 operator-(P2 a, P2 b) { return { a.x - b.x, a.y - b.y }; }
inline P2 operator*(P2 a, double s) { return { a.x * s, a.y * s }; }
inline double Dot(P2 a, P2 b) { return a.x * b.x + a.y * b.y; }
inline double Cross(P2 a, P2 b) { return a.x * b.y - a.y * b.x; }
inline double Len(P2 a) { return std::sqrt(Dot(a, a)); }
inline P2 Norm(P2 a)
{
   const double l = Len(a);
   return l > 1e-12 ? a * (1.0 / l) : P2 { 0.0, 0.0 };
}

double SignedArea(const std::vector<P2>& p)
{
   double a = 0.0;
   for (size_t i = 0, n = p.size(); i < n; i++)
      a += Cross(p[i], p[(i + 1) % n]);
   return 0.5 * a;
}

// Appends the arc through a, p, b (in that order along the arc), a excluded and b included, flattened to the chord error tol.
void AppendArc(std::vector<P2>& out, P2 a, P2 p, P2 b, double tol)
{
   const double c = Cross(p - a, b - p);
   const double chord = Len(b - a);
   if (std::fabs(c) < 1e-9 * (1.0 + chord * chord))
   {
      // Straight: keep p only if it lies between a and b (beyond them it would make a zero-width spike)
      const double t = chord > 1e-12 ? Dot(p - a, b - a) / (chord * chord) : 0.5;
      if (t > 0.0 && t < 1.0)
         out.push_back(p);
      out.push_back(b);
      return;
   }
   // circumcentre
   const double d = 2.0 * (a.x * (p.y - b.y) + p.x * (b.y - a.y) + b.x * (a.y - p.y));
   const double a2 = Dot(a, a), p2 = Dot(p, p), b2 = Dot(b, b);
   const P2 o { (a2 * (p.y - b.y) + p2 * (b.y - a.y) + b2 * (a.y - p.y)) / d, (a2 * (b.x - p.x) + p2 * (a.x - b.x) + b2 * (p.x - a.x)) / d };
   const double r = Len(a - o);
   if (r > 1e7)
   {
      const double t = chord > 1e-12 ? Dot(p - a, b - a) / (chord * chord) : 0.5;
      if (t > 0.0 && t < 1.0)
         out.push_back(p);
      out.push_back(b);
      return;
   }
   const double aa = std::atan2(a.y - o.y, a.x - o.x), ab = std::atan2(b.y - o.y, b.x - o.x);
   double sweep;
   if (c > 0.0) // counter-clockwise
   {
      sweep = ab - aa;
      while (sweep <= 0.0)
         sweep += 2.0 * kPi;
   }
   else
   {
      sweep = aa - ab;
      while (sweep <= 0.0)
         sweep += 2.0 * kPi;
      sweep = -sweep;
   }
   const double step = 2.0 * std::acos(std::max(-1.0, 1.0 - tol / r)); // sagitta = r (1 - cos(step / 2)) <= tol
   const int n = std::min(kMaxArcSegments, std::max(2, (int)std::ceil(std::fabs(sweep) / std::max(step, 1e-3))));
   for (int i = 1; i < n; i++)
   {
      const double t = aa + sweep * ((double)i / n);
      out.push_back({ o.x + r * std::cos(t), o.y + r * std::sin(t) });
   }
   out.push_back(b);
}

std::vector<P2> Flatten(const std::vector<CurvePoint>& pts, double tol)
{
   const int n = (int)pts.size();
   std::vector<P2> out;
   auto at = [&](int i) { return P2 { pts[((i % n) + n) % n].x, pts[((i % n) + n) % n].y }; };
   for (int i = 0; i < n; i++)
   {
      if (!pts[i].smooth)
      {
         out.push_back(at(i));
         continue;
      }
      const P2 p = at(i);
      const P2 a = pts[((i - 1) % n + n) % n].smooth ? (at(i - 1) + p) * 0.5 : at(i - 1);
      const P2 b = pts[(i + 1) % n].smooth ? (at(i + 1) + p) * 0.5 : at(i + 1);
      // the previous item already emitted a (the corner itself, or the previous arc's end at the midpoint)
      AppendArc(out, a, p, b, tol);
   }
   // drop repeated points (including the closing one)
   std::vector<P2> q;
   for (const P2& v : out)
      if (q.empty() || Len(v - q.back()) > 1e-4)
         q.push_back(v);
   while (q.size() > 1 && Len(q.front() - q.back()) <= 1e-4)
      q.pop_back();
   return q;
}

double PointSegDist(P2 p, P2 a, P2 b)
{
   const P2 e = b - a;
   const double l2 = Dot(e, e);
   const double t = l2 > 0.0 ? std::max(0.0, std::min(1.0, Dot(p - a, e) / l2)) : 0.0;
   return Len(p - (a + e * t));
}

double SegSegDist(P2 a, P2 b, P2 c, P2 d)
{
   const double d1 = Cross(b - a, c - a), d2 = Cross(b - a, d - a), d3 = Cross(d - c, a - c), d4 = Cross(d - c, b - c);
   if (((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0)) && d1 != 0.0 && d2 != 0.0 && d3 != 0.0 && d4 != 0.0)
      return 0.0;
   return std::min(std::min(PointSegDist(a, c, d), PointSegDist(b, c, d)), std::min(PointSegDist(c, a, b), PointSegDist(d, a, b)));
}

// A hole as a polygon, clockwise (the material is on the left). A round hole: the circle; a pill: the capsule round both centres
std::vector<P2> HoleLoop(const Hole& h, double tol)
{
   if (h.polygon.size() >= 3) // a polygon hole: clockwise, consecutive duplicates (and the closing one) removed
   {
      std::vector<P2> c;
      for (const CurvePoint& v : h.polygon)
         if (c.empty() || Len(P2 { v.x, v.y } - c.back()) > 1e-4)
            c.push_back({ v.x, v.y });
      while (c.size() > 1 && Len(c.front() - c.back()) <= 1e-4)
         c.pop_back();
      if (c.size() >= 3 && SignedArea(c) > 0.0)
         std::reverse(c.begin(), c.end());
      return c;
   }
   const double r = 0.5 * h.diameter;
   const P2 a { h.x, h.y }, b { h.x2, h.y2 };
   std::vector<P2> c;
   const double step = std::max(2.0 * std::acos(std::max(-1.0, 1.0 - tol / r)), 1e-3);
   if (h.pill && Len(b - a) > 1e-3)
   {
      const int half = std::min(kMaxArcSegments / 2 - 1, std::max(6, (int)std::ceil(kPi / step)));
      const double t0 = std::atan2(b.y - a.y, b.x - a.x);
      for (int k = 0; k <= half; k++) // round b, from its left side over its far end to its right side
      {
         const double t = t0 + 0.5 * kPi - kPi * k / half;
         c.push_back({ b.x + r * std::cos(t), b.y + r * std::sin(t) });
      }
      for (int k = 0; k <= half; k++) // round a, back
      {
         const double t = t0 - 0.5 * kPi - kPi * k / half;
         c.push_back({ a.x + r * std::cos(t), a.y + r * std::sin(t) });
      }
      return c;
   }
   const int n = std::min(kMaxArcSegments, std::max(12, (int)std::ceil(2.0 * kPi / step)));
   for (int i = 0; i < n; i++)
   {
      const double t = -2.0 * kPi * i / n;
      c.push_back({ h.x + r * std::cos(t), h.y + r * std::sin(t) });
   }
   return c;
}

bool PointInPolygon(const std::vector<P2>& p, P2 v)
{
   bool in = false;
   for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
      if (((p[i].y > v.y) != (p[j].y > v.y)) && (v.x < (p[j].x - p[i].x) * (v.y - p[i].y) / (p[j].y - p[i].y) + p[i].x))
         in = !in;
   return in;
}

// ---- triangulation (earcut: holes supported, near-linear on long contours) ----

struct Tri
{
   int a, b, c;
};

// rings[0] outline (area > 0), others holes (area < 0). Triangles index the concatenated ring points, all counter-clockwise.
std::vector<Tri> Triangulate(const std::vector<std::vector<P2>>& rings)
{
   std::vector<std::vector<std::array<double, 2>>> poly(rings.size());
   std::vector<P2> pos;
   for (size_t r = 0; r < rings.size(); r++)
   {
      poly[r].reserve(rings[r].size());
      for (const P2& v : rings[r])
      {
         poly[r].push_back({ v.x, v.y });
         pos.push_back(v);
      }
   }
   const std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
   std::vector<Tri> tris;
   tris.reserve(idx.size() / 3);
   for (size_t i = 0; i + 2 < idx.size(); i += 3)
   {
      Tri t { (int)idx[i], (int)idx[i + 1], (int)idx[i + 2] };
      if (Cross(pos[t.b] - pos[t.a], pos[t.c] - pos[t.a]) < 0.0) // keep the old orientation (counter-clockwise, up-facing) whatever earcut's output winding is
         std::swap(t.b, t.c);
      tris.push_back(t);
   }
   return tris;
}

// ---- mesh ----

struct Ctx
{
   const Params* p;
   void Uv(Vertex& v) const
   {
      v.tu = p->uvm[0] * v.x + p->uvm[1] * v.y + p->uvm[2];
      v.tv = p->uvm[3] * v.x + p->uvm[4] * v.y + p->uvm[5];
   }
   Vertex Make(double x, double y, double z, double nx, double ny, double nz) const
   {
      Vertex v { (float)x, (float)y, (float)z, (float)nx, (float)ny, (float)nz, 0.f, 0.f };
      Uv(v);
      return v;
   }
};

void AddCap(const Ctx& c, const std::vector<std::vector<P2>>& rings, double z, Mesh& m)
{
   for (const auto& r : rings)
      for (const P2& v : r)
         m.verts.push_back(c.Make(v.x, v.y, z, 0.0, 0.0, 1.0));
   for (const Tri& t : Triangulate(rings))
   {
      m.idx.push_back((unsigned)t.a);
      m.idx.push_back((unsigned)t.b);
      m.idx.push_back((unsigned)t.c);
   }
}

// Per-contour data: edge normals, mitre vectors (toward the material, clamped to 2), and the vertex columns for the walls.
struct ContourInfo
{
   std::vector<P2> mitre;
   std::vector<P2> nIn, nOut; // outward (away from the material) horizontal normal used by the edge ending / starting at the vertex
};

ContourInfo Analyse(const std::vector<P2>& p)
{
   const int n = (int)p.size();
   ContourInfo ci;
   ci.mitre.resize(n);
   ci.nIn.resize(n);
   ci.nOut.resize(n);
   std::vector<P2> left(n); // left normal of edge i -> i+1, which points to the material
   for (int i = 0; i < n; i++)
   {
      const P2 e = Norm(p[(i + 1) % n] - p[i]);
      left[i] = { -e.y, e.x };
   }
   for (int i = 0; i < n; i++)
   {
      const P2 lp = left[(i + n - 1) % n], ln = left[i];
      const double d = Dot(lp, ln);
      P2 m;
      if (1.0 + d < 1e-6)
         m = lp;
      else
         m = (lp + ln) * (1.0 / (1.0 + d));
      const double ml = Len(m);
      if (ml > 2.0)
         m = m * (2.0 / ml);
      ci.mitre[i] = m;
      const double turn = std::acos(std::max(-1.0, std::min(1.0, d)));
      if (turn > kSharp)
      {
         ci.nIn[i] = lp * -1.0;
         ci.nOut[i] = ln * -1.0;
      }
      else
      {
         const P2 s = Norm((lp + ln) * -1.0);
         ci.nIn[i] = ci.nOut[i] = s;
      }
   }
   return ci;
}

void AddBevelWalls(const Ctx& c, const std::vector<P2>& p, double z0, int segs, double bevel, double thickness, Mesh& m)
{
   const int n = (int)p.size();
   const ContourInfo ci = Analyse(p);
   const double zb = z0 + thickness - bevel; // where the walls end and the bevel starts

   // ring definitions: (inset, z, normal horizontal weight, normal z)
   struct Ring
   {
      double off, z, nh, nz;
   };
   std::vector<Ring> wall { { 0.0, z0, 1.0, 0.0 }, { 0.0, bevel > 0.0 ? zb : z0 + thickness, 1.0, 0.0 } };
   std::vector<Ring> bev;
   if (bevel > 0.0)
   {
      for (int k = 0; k <= segs; k++)
      {
         const double th = 0.5 * kPi * k / segs;
         const double nth = segs == 1 ? 0.25 * kPi : th;
         // The last ring is the top cap's ring exactly (the same inset, 1 - cos(90 deg) is not exactly 1 in double)
         bev.push_back({ k == segs ? bevel : bevel * (1.0 - std::cos(th)), zb + bevel * std::sin(th), std::cos(nth), std::sin(nth) });
      }
   }

   auto strip = [&](const std::vector<Ring>& rings) {
      if (rings.size() < 2)
         return;
      const unsigned base = (unsigned)m.verts.size();
      // columns: per vertex an "out" column (edge starting here) and an "in" column (edge ending here), shared when the normals agree
      std::vector<int> colOut(n), colIn(n);
      std::vector<std::array<P2, 2>> colN; // normals of each column
      std::vector<int> colVert;
      for (int i = 0; i < n; i++)
      {
         colOut[i] = (int)colN.size();
         colN.push_back({ ci.nOut[i], P2 { 0, 0 } });
         colVert.push_back(i);
         if (Len(ci.nIn[i] - ci.nOut[i]) > 1e-9)
         {
            colIn[i] = (int)colN.size();
            colN.push_back({ ci.nIn[i], P2 { 0, 0 } });
            colVert.push_back(i);
         }
         else
            colIn[i] = colOut[i];
      }
      const int nc = (int)colN.size();
      for (const Ring& r : rings)
         for (int k = 0; k < nc; k++)
         {
            const int i = colVert[k];
            const P2 q = p[i] + ci.mitre[i] * r.off;
            const P2 nn = colN[k][0];
            m.verts.push_back(c.Make(q.x, q.y, r.z, nn.x * r.nh, nn.y * r.nh, r.nz));
         }
      for (size_t r = 0; r + 1 < rings.size(); r++)
         for (int i = 0; i < n; i++)
         {
            const int j = (i + 1) % n;
            const unsigned a0 = base + (unsigned)(r * nc + colOut[i]), a1 = base + (unsigned)(r * nc + colIn[j]);
            const unsigned b0 = base + (unsigned)((r + 1) * nc + colOut[i]), b1 = base + (unsigned)((r + 1) * nc + colIn[j]);
            m.idx.insert(m.idx.end(), { a0, a1, b1, a0, b1, b0 });
         }
   };
   strip(wall);
   strip(bev);
}

} // namespace

std::vector<CurvePoint> FlattenOutline(const std::vector<CurvePoint>& outline, float tol)
{
   std::vector<CurvePoint> res;
   if (outline.size() < 3)
      return res;
   for (const P2& v : Flatten(outline, tol))
      res.push_back({ (float)v.x, (float)v.y, false });
   return res;
}

std::vector<CurvePoint> HoleOutline(const Hole& hole, float tol)
{
   std::vector<CurvePoint> res;
   if (hole.polygon.size() < 3 && hole.diameter <= 2e-4f)
      return res;
   for (const P2& v : HoleLoop(hole, std::max(0.01, (double)tol)))
      res.push_back({ (float)v.x, (float)v.y, false });
   return res;
}

bool Build(const std::vector<CurvePoint>& outline, const std::vector<Hole>& holes, const Params& params, Result& out)
{
   if (outline.size() < 3)
      return false;
   const double tol = std::max(0.01, (double)params.flatten);
   std::vector<P2> ol = Flatten(outline, tol);
   if (ol.size() < 3 || std::fabs(SignedArea(ol)) < 1e-6)
      return false;
   if (SignedArea(ol) < 0.0)
      std::reverse(ol.begin(), ol.end());

   Result res;
   std::vector<std::vector<P2>> contours { ol };
   // Per contour: its bevel before the clamp (the outline: outlineBevel or bevel; a hole: its own when >= 0, else bevel), and the most its top ring may
   // move toward the material (holes: grow), 1e300 = no clamp. A hole with its own bevel is not clamped here: the caller did it.
   const double warnBevel = std::max(0.0, std::min((double)params.bevel, (double)params.thickness));
   const double outlineB = params.outlineBevel >= 0.f ? std::max(0.0, std::min((double)params.outlineBevel, (double)params.thickness)) : warnBevel;
   std::vector<double> want { outlineB };
   std::vector<double> topGrow { 1e300 };
   res.contourOfHole.assign(holes.size(), -1);
   for (size_t hi = 0; hi < holes.size(); hi++)
   {
      const Hole& h = holes[hi];
      const bool own = h.bevel >= 0.f;
      const double holeB = own ? std::max(0.0, std::min((double)h.bevel, (double)params.thickness)) : warnBevel;
      if (h.polygon.size() >= 3)
      {
         const std::vector<P2> c = HoleLoop(h, tol);
         bool inside = c.size() >= 3;
         for (size_t i = 0; i < c.size() && inside; i++)
            inside = PointInPolygon(ol, c[i]);
         if (!inside)
         {
            res.skippedHoles++;
            continue;
         }
         double edgeDist = 1e300; // polygon to outline, r = 0
         for (size_t i = 0, k = c.size() - 1; i < c.size(); k = i++)
            for (size_t j = 0, l = ol.size() - 1; j < ol.size(); l = j++)
               edgeDist = std::min(edgeDist, SegSegDist(c[k], c[i], ol[l], ol[j]));
         if (edgeDist < holeB + outlineB)
            res.crossingHoles++;
         topGrow.push_back(own ? 1e300 : std::max(0.0, std::min(holeB, edgeDist - outlineB - 0.1)));
         want.push_back(holeB);
         res.contourOfHole[hi] = (int)contours.size();
         contours.push_back(c);
         continue;
      }
      const double r = 0.5 * h.diameter;
      if (r <= 1e-4)
         continue;
      const P2 ha { h.x, h.y }, hb { h.x2, h.y2 };
      const bool pill = h.pill && Len(hb - ha) > 1e-3;
      if (!PointInPolygon(ol, ha) && !(pill && (PointInPolygon(ol, hb) || PointInPolygon(ol, (ha + hb) * 0.5))))
      {
         res.skippedHoles++;
         continue;
      }
      double edgeDist = 1e300; // centre (a pill: the segment between its centres) to the outline edge
      for (size_t i = 0, k = ol.size() - 1; i < ol.size(); k = i++)
         edgeDist = std::min(edgeDist, pill ? SegSegDist(ha, hb, ol[k], ol[i]) : PointSegDist(ha, ol[k], ol[i]));
      if (edgeDist < r + (holeB + outlineB)) // inside the band where the top rings (outline inset, hole grown by the bevel) overlap: warning only
         res.crossingHoles++;
      // A hole near the edge: its top ring grows only as far as it stays clear of the outline's inset top ring (it would fold the top cap)
      topGrow.push_back(own ? 1e300 : std::max(0.0, std::min(holeB, edgeDist - outlineB - r - 0.1)));
      want.push_back(holeB);
      const std::vector<P2> c = HoleLoop(h, tol);
      res.contourOfHole[hi] = (int)contours.size();
      contours.push_back(c);
   }

   const double thick = std::max(0.01, (double)params.thickness);
   const int segs = std::max(1, params.bevelSegments);
   const double z0 = params.height;
   const Ctx ctx { &params };

   if (params.bottomCap)
      AddCap(ctx, contours, z0, res.bottom);
   // One bevel per contour, used by both the top inset and the bevel rings, so they meet (a hole near the edge grows less: topGrow)
   std::vector<double> eff(contours.size());
   for (size_t k = 0; k < contours.size(); k++)
   {
      const double d = std::min(std::min(want[k], thick), topGrow[k]);
      eff[k] = d > 1e-4 ? d : 0.0; // A clamp that leaves rounding dust (edgeDist - bevel - r - 0.1 = 4e-16) builds a straight wall, not zero-height rings
   }
   std::vector<std::vector<P2>> inset = contours;
   for (size_t k = 0; k < inset.size(); k++)
      if (eff[k] > 0.0)
      {
         auto& ring = inset[k];
         const ContourInfo ci = Analyse(ring);
         for (size_t i = 0; i < ring.size(); i++)
            ring[i] = ring[i] + ci.mitre[i] * eff[k];
      }
   AddCap(ctx, inset, z0 + thick, res.top);
   for (size_t k = 0; k < contours.size(); k++)
      AddBevelWalls(ctx, contours[k], z0, segs, eff[k], thick, res.bevel);
   const auto toCurve = [](const std::vector<P2>& ring)
   {
      std::vector<CurvePoint> c;
      c.reserve(ring.size());
      for (const P2& v : ring)
         c.push_back({ (float)v.x, (float)v.y, false });
      return c;
   };
   for (size_t k = 0; k < contours.size(); k++)
   {
      res.contours.push_back(toCurve(contours[k]));
      res.topRings.push_back(toCurve(inset[k]));
      res.effBevel.push_back((float)eff[k]);
   }

   if ((params.bottomCap && res.bottom.idx.empty()) || res.top.idx.empty() || res.bevel.idx.empty())
      return false;
   out = std::move(res);
   return true;
}

}
