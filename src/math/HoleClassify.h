// license:GPLv3+

// Playfield Maker phase 2 (P2-3) "Sorting": classifies the shapes Find Holes traced as insert pocket, through hole or edge cut.
// No engine dependency (standard library and MaskTrace.h only), so it can be checked standalone: clang++ -std=c++20 -I src tools/holeclassify_check.cpp
//   - Insert pocket: a traced insert card's footprint covers the shape's centre or more than half its area (MaskTrace::UnderInsert, phase 1's rule), or an
//     Insert Light's centre lies inside the shape. Both are inputs: the caller builds the card hulls (MaskTrace::ConvexHull of each card's mesh through its
//     matrix) and the light centres, and leaves out lights with Show Bulb Mesh on (they draw a bulb, so they sit in a hole, not under an insert).
//   - Through hole: everything else.
//   - Edge cut: a shape touching the image edge (Trace lists them in Result::outlineCuts instead of tracing them). Counted, not stored as a hole.
// All coordinates are table units (VPU), the same space as the traced shapes.

#pragma once

#include <cstring>
#include "MaskTrace.h"

#include <vector>

namespace HoleClassify
{

enum class Kind { Through = 0, InsertPocket = 1 }; // the Kind selector; InsertPocket = the Under Insert bit (4) of the saved hole flags

enum class Why : uint8_t { None = 0, Card = 1, Light = 2 }; // what made a shape an insert pocket

struct Footprints
{
   std::vector<std::vector<MaskTrace::Pt>> hulls; // one per traced insert card: its footprint polygon (either winding; fewer than 3 points are ignored)
   std::vector<MaskTrace::Pt> lightCentres; // Insert Lights picked by the author, Show Bulb Mesh off only (the caller filters); empty = none picked
};

struct Classes
{
   std::vector<Kind> kind; // per traced hole, same order as MaskTrace::Result::holes
   std::vector<Why> why; // per hole, None for through holes
   std::vector<int> card; // per hole: index of the first hull that made it a pocket, -1 if none (a pocket from a light only has -1 and Why::Light)
   int pockets = 0, throughs = 0;
   int byCard = 0, byLight = 0; // pockets by reason (a shape both under a card and over a light counts as Card)
   int edgeCuts = 0; // MaskTrace::Result::outlineCuts.size()
   bool haveEvidence = false; // any usable hull or light centre was given. False means every shape was a through hole only by default: the panel line
                              // "N shapes without a traced card or insert light were treated as through-holes" uses throughs, whatever this says.
};

// Is the point inside the hole's shape (a circle, a capsule, or a polygon by even-odd)?
inline bool PointInHole(const MaskTrace::Hole& h, float px, float py)
{
   using namespace MaskTrace;
   if (h.type == HoleType::Round)
      return std::hypot((double)px - h.x, (double)py - h.y) <= 0.5 * h.diameter;
   if (h.type == HoleType::Slot)
      return detail::SegDist({ px, py }, { h.x, h.y }, { h.x2, h.y2 }) <= 0.5 * h.diameter;
   const std::vector<Pt>& p = h.polygon;
   if (p.size() < 3)
      return false;
   bool in = false;
   for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
      if ((p[i].y > py) != (p[j].y > py) && (double)px < ((double)p[j].x - p[i].x) * ((double)py - p[i].y) / ((double)p[j].y - p[i].y) + p[i].x)
         in = !in;
   return in;
}

// A finite float? A bit test: -ffast-math (the engine builds with it) makes isnan / isfinite constant. Takes a reference on purpose: under
// -ffinite-math-only clang marks by-value float parameters nofpclass(nan inf) and folds the test to true.
inline bool Finite(const float& f)
{
   uint32_t b;
   std::memcpy(&b, &f, 4);
   return (b & 0x7f800000u) != 0x7f800000u;
}

inline Classes Classify(const MaskTrace::Result& traced, const Footprints& fp)
{
   using namespace MaskTrace;
   Classes r;
   const size_t n = traced.holes.size();
   r.kind.assign(n, Kind::Through);
   r.why.assign(n, Why::None);
   r.card.assign(n, -1);
   r.edgeCuts = (int)traced.outlineCuts.size();

   std::vector<std::vector<Pt>> hulls; // MaskTrace::UnderInsert wants counter-clockwise hulls
   std::vector<int> hullIndex;
   for (size_t k = 0; k < fp.hulls.size(); k++)
   {
      std::vector<Pt> h = fp.hulls[k];
      bool ok = h.size() >= 3;
      for (const Pt& q : h)
         ok = ok && Finite(q.x) && Finite(q.y);
      if (!ok)
         continue;
      std::vector<detail::D2> d;
      for (const Pt& q : h)
         d.push_back({ q.x, q.y });
      const double a = detail::SignedArea(d);
      if (!(a != 0.))
         continue; // degenerate: a line
      if (a < 0.)
         std::reverse(h.begin(), h.end());
      hulls.push_back(std::move(h));
      hullIndex.push_back((int)k);
   }
   std::vector<Pt> lights;
   for (const Pt& q : fp.lightCentres)
      if (Finite(q.x) && Finite(q.y))
         lights.push_back(q);
   r.haveEvidence = !hulls.empty() || !lights.empty();

   for (size_t i = 0; i < n; i++)
   {
      const Hole& h = traced.holes[i];
      for (size_t k = 0; k < hulls.size(); k++)
         if (UnderInsert(h, hulls[k]))
         {
            r.kind[i] = Kind::InsertPocket, r.why[i] = Why::Card, r.card[i] = hullIndex[k];
            break;
         }
      if (r.kind[i] == Kind::Through)
         for (const Pt& c : lights)
            if (PointInHole(h, c.x, c.y))
            {
               r.kind[i] = Kind::InsertPocket, r.why[i] = Why::Light;
               break;
            }
      if (r.kind[i] == Kind::InsertPocket)
         r.pockets++, (r.why[i] == Why::Card ? r.byCard : r.byLight)++;
      else
         r.throughs++;
   }
   return r;
}

} // namespace HoleClassify
