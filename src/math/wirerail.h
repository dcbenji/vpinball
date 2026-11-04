// license:GPLv3+

// Wire Rail: tubes and a ball channel swept along a 3D route, after freezy's Wire Rail in VPE (creators-guide/editor/wire-rails): up to six wires placed
// by (x right of the route, z up from it), a frame that follows the route and a per-point roll, and a collider that is not the tubes but flat facets through the
// points where a ball resting on wires 1 and 2 would touch each wire, swept along the route.
// Header only and templated on the vertex type so it can be checked standalone (tools/wirerail_check.cpp). No dependency on the engine.

#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "math/wiresweep.h"

namespace WireRail
{

using WireSweep::V3;

inline constexpr int kMaxWires = 6;
inline constexpr float kDefaultWireDiameter = 6.5f;
inline constexpr float kDefaultBallDiameter = 50.f;
inline constexpr float kDefaultCapBevel = 0.5f;
inline constexpr int kDefaultSides = 10;
inline constexpr int kDefaultCurvatureDetail = 8;
inline constexpr float kDefaultLength = 500.f;

struct V2
{
   float x, y;
};
inline V2 operator+(V2 a, V2 b) { return { a.x + b.x, a.y + b.y }; }
inline V2 operator-(V2 a, V2 b) { return { a.x - b.x, a.y - b.y }; }
inline V2 operator*(V2 a, float s) { return { a.x * s, a.y * s }; }
inline float Dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline float Len(V2 a) { return sqrtf(Dot(a, a)); }
inline float Cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
inline V2 Norm(V2 a, V2 fallback = { 0.f, 0.f })
{
   const float l = Len(a);
   return l > 1e-6f ? a * (1.f / l) : fallback;
}

struct Wire
{
   float x = 0.f, z = 0.f; // Right of the route, up from it
   bool active = true;
};

// freezy's presets: bottom pair 30 apart (one wire: under the ball), raised pair 30 out and 30 up (three wires: only on the Third Rail side), top wires 60 up
inline std::vector<Wire> DefaultWires(const int count, const bool thirdRailRight)
{
   std::vector<Wire> w;
   if (count <= 1)
      return { Wire {} };
   w.push_back({ -15.f, 0.f, true });
   w.push_back({ 15.f, 0.f, true });
   if (count == 3)
      w.push_back({ thirdRailRight ? 30.f : -30.f, 30.f, true });
   if (count >= 4)
   {
      w.push_back({ -30.f, 30.f, true });
      w.push_back({ 30.f, 30.f, true });
   }
   if (count == 5)
      w.push_back({ 0.f, 60.f, true });
   if (count >= 6)
   {
      w.push_back({ -15.f, 60.f, true });
      w.push_back({ 15.f, 60.f, true });
   }
   return w;
}

inline bool SameWires(const std::vector<Wire>& a, const std::vector<Wire>& b)
{
   if (a.size() != b.size())
      return false;
   for (size_t i = 0; i < a.size(); i++)
      if (fabsf(a[i].x - b[i].x) > 1e-4f || fabsf(a[i].z - b[i].z) > 1e-4f)
         return false;
   return true;
}

// Rails changed (freezy's ResizeRailCount): a set still at its preset is laid out again, customized wires stay, added wires come at their preset position, on
inline std::vector<Wire> ResizeWires(std::vector<Wire> wires, const int count, const bool thirdRailRight)
{
   const std::vector<Wire> next = DefaultWires(count, thirdRailRight);
   const bool atPreset = SameWires(wires, DefaultWires((int)wires.size(), thirdRailRight));
   for (size_t i = 0; i < wires.size() && i < next.size() && atPreset; i++)
   {
      wires[i].x = next[i].x;
      wires[i].z = next[i].z;
   }
   const size_t kept = wires.size();
   wires.resize(count);
   for (size_t i = kept; i < next.size(); i++)
      wires[i] = next[i];
   return wires;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Ball channel profile: the 2D fit of WireRailChannelProfile

struct Profile
{
   std::vector<V2> verts; // In the cross-section plane
   std::vector<std::pair<int, int>> spans; // Facets, as pairs of verts
   V2 ball { 0.f, 0.f }; // Centre of the ball resting on wires 1 and 2
   bool closed = false; // A roof: five or six wires and the ball does not fit through the top
};

namespace Detail
{
   inline constexpr float kFullTurn = 6.28318530717958647692f;
   inline constexpr int kMaxFacets = 8;

   struct Facet
   {
      V2 point { 0.f, 0.f }, normal { 0.f, 0.f };
      float angle = 0.f;
      V2 wire { 0.f, 0.f };
      float radius = 0.f;
      int index = -1;
   };

   inline float NormalizeAngle(float a)
   {
      while (a < 0.f)
         a += kFullTurn;
      return a;
   }
   inline float AngularDistance(const float a, const float b) { return fabsf(atan2f(sinf(a - b), cosf(a - b))); }

   inline int FindGap(const std::vector<Facet>& sorted, const float target)
   {
      const int n = (int)sorted.size();
      for (int i = 0; i < n; i++)
      {
         const float start = sorted[i].angle;
         float end = sorted[(i + 1) % n].angle;
         if (i == n - 1)
            end += kFullTurn;
         float t = target;
         if (t < start)
            t += kFullTurn;
         if (t >= start && t <= end)
            return i;
      }
      return n - 1;
   }

   inline std::vector<Facet> OrderAroundGap(const std::vector<Facet>& sorted, const int gap)
   {
      const int n = (int)sorted.size();
      if (n <= 1)
         return sorted;
      std::vector<Facet> out;
      float prev = -1e30f;
      for (int k = 1; k <= n; k++)
      {
         Facet f = sorted[(gap + k) % n];
         while (f.angle <= prev)
            f.angle += kFullTurn;
         prev = f.angle;
         out.push_back(f);
      }
      return out;
   }

   inline bool Intersect(const Facet& a, const Facet& b, V2& out)
   {
      const float det = Cross(a.normal, b.normal);
      if (fabsf(det) < 1e-5f)
         return false;
      const float da = Dot(a.normal, a.point), db = Dot(b.normal, b.point);
      out = { (da * b.normal.y - a.normal.y * db) / det, (a.normal.x * db - da * b.normal.x) / det };
      return std::isfinite(out.x) && std::isfinite(out.y);
   }

   // A gap between two wires the ball fits through, facing up (the top opening of a five or six wire rail). -1: none
   inline int PassableTopGap(const std::vector<Facet>& sorted, const float ballDiameter)
   {
      const int n = (int)sorted.size();
      const float topInward = NormalizeAngle(-kFullTurn * 0.25f);
      const auto covers = [&](const Facet& f)
      {
         const float a = asinf(std::min(f.radius / (f.radius + ballDiameter * 0.5f), 1.f));
         return AngularDistance(f.angle, topInward) <= a;
      };
      const auto clearGap = [](const Facet& a, const Facet& b) { return Len(a.wire - b.wire) - a.radius - b.radius; };
      const int containing = FindGap(sorted, topInward);
      const int next = (containing + 1) % n;
      if (!covers(sorted[containing]) && !covers(sorted[next]) && clearGap(sorted[containing], sorted[next]) > ballDiameter + 1e-5f)
         return containing;
      int best = -1, bestA = 1 << 30, bestB = 1 << 30;
      for (int i = 0; i < n; i++)
      {
         const Facet& a = sorted[i];
         const Facet& b = sorted[(i + 1) % n];
         if ((!covers(a) && !covers(b)) || clearGap(a, b) <= ballDiameter + 1e-5f)
            continue;
         const int lo = std::min(a.index, b.index), hi = std::max(a.index, b.index);
         if (best >= 0 && (lo > bestA || (lo == bestA && hi >= bestB)))
            continue;
         best = i;
         bestA = lo;
         bestB = hi;
      }
      return best;
   }
}

// 'all' are every wire of the rail (the side the ball rests on is the one nearest the centre of all of them), the fit uses the active ones.
// Wires 1 and 2 of the active ones carry the ball. False: 'error' says why, in the creator's terms.
inline bool FitProfile(const std::vector<Wire>& all, const float wireRadius, const float ballRadius, Profile& profile, std::string& error)
{
   using namespace Detail;
   profile = Profile {};
   std::vector<V2> wires;
   std::vector<int> names;
   V2 lo { 1e30f, 1e30f }, hi { -1e30f, -1e30f };
   for (size_t i = 0; i < all.size(); i++)
   {
      lo = { std::min(lo.x, all[i].x - wireRadius), std::min(lo.y, all[i].z - wireRadius) };
      hi = { std::max(hi.x, all[i].x + wireRadius), std::max(hi.y, all[i].z + wireRadius) };
      if (all[i].active)
      {
         wires.push_back({ all[i].x, all[i].z });
         names.push_back((int)i + 1);
      }
   }
   if (wires.empty())
   {
      error = "No wire is on: the ball has nothing to roll on";
      return false;
   }
   if (wireRadius <= 0.f || ballRadius <= 0.f)
   {
      error = "Wire and ball sizes must be positive";
      return false;
   }
   const V2 hint = (lo + hi) * 0.5f;

   // The resting ball: on top of a single wire, else touching wires 1 and 2, on the side of the wire bundle
   V2 ball;
   if (wires.size() == 1)
      ball = wires[0] + V2 { 0.f, (hint.y < wires[0].y ? -1.f : 1.f) * (wireRadius + ballRadius) };
   else
   {
      const V2 delta = wires[1] - wires[0];
      const float sep = Len(delta);
      if (sep <= 1e-5f)
      {
         error = "Wires " + std::to_string(names[0]) + " and " + std::to_string(names[1]) + " are at the same place";
         return false;
      }
      const float r = wireRadius + ballRadius;
      if (sep > 2.f * r)
      {
         error = "The ball falls between wires " + std::to_string(names[0]) + " and " + std::to_string(names[1]) + ": they are " + std::to_string((int)roundf(sep)) + " apart, at most "
            + std::to_string((int)floorf(2.f * r)) + " fits";
         return false;
      }
      const V2 mid = wires[0] + delta * 0.5f;
      const V2 perp = Norm({ -delta.y, delta.x });
      const float h = sqrtf(std::max(0.f, r * r - sep * sep * 0.25f));
      const V2 a = mid + perp * h, b = mid - perp * h;
      const float da = Dot(a - hint, a - hint), db = Dot(b - hint, b - hint);
      ball = fabsf(da - db) <= 1e-4f ? (a.y >= b.y ? a : b) : (da < db ? a : b);
   }

   // A contact facet per wire, sorted by the angle of its normal (towards the ball)
   std::vector<Facet> facets;
   for (size_t i = 0; i < wires.size(); i++)
   {
      const V2 d = ball - wires[i];
      if (Len(d) < 1e-4f)
      {
         error = "Wire " + std::to_string(names[i]) + " lies on the centre of the ball";
         return false;
      }
      // Not in VPE: a wire inside the resting ball makes a facet through the ball, it would be pushed out of the rail
      if (Len(d) < wireRadius + ballRadius - 0.01f)
      {
         error = "Wire " + std::to_string(names[i]) + " is inside the ball resting on wires " + std::to_string(names[0]) + " and " + std::to_string(names[1]);
         return false;
      }
      Facet f;
      f.normal = Norm(d);
      f.point = wires[i] + f.normal * wireRadius;
      f.angle = NormalizeAngle(atan2f(f.normal.y, f.normal.x));
      f.wire = wires[i];
      f.radius = wireRadius;
      f.index = (int)i;
      facets.push_back(f);
   }
   std::sort(facets.begin(), facets.end(), [](const Facet& a, const Facet& b) { return a.angle < b.angle; });

   const int topGap = wires.size() >= 5 ? PassableTopGap(facets, ballRadius * 2.f) : -1;
   const bool openTop = topGap >= 0;
   const bool closed = wires.size() >= 5 && !openTop;

   V2 center { 0.f, 0.f };
   for (const Facet& f : facets)
      center = center + f.wire;
   center = center * (1.f / (float)facets.size());
   const V2 opening = Norm(ball - center, { 0.f, 1.f });
   const std::vector<Facet> ordered = closed ? facets : OrderAroundGap(facets, openTop ? topGap : FindGap(facets, NormalizeAngle(atan2f(-opening.y, -opening.x))));

   // Chamfers tangent to the ball in the widest gaps, up to 8 facets, so that the floor wraps the ball (none where a top opening connects the wires directly)
   std::vector<Facet> lines = ordered;
   if (!openTop)
   {
      const int n = (int)ordered.size();
      const int gaps = closed ? n : std::max(0, n - 1);
      const int chamfers = std::min(std::max(0, kMaxFacets - n), gaps);
      std::vector<std::pair<float, int>> byAngle;
      for (int i = 0; i < gaps; i++)
      {
         const int next = (i + 1) % n;
         byAngle.push_back({ (next == 0 ? ordered[next].angle + kFullTurn : ordered[next].angle) - ordered[i].angle, i });
      }
      std::stable_sort(byAngle.begin(), byAngle.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      std::vector<bool> chamfered(n, false);
      for (int i = 0; i < chamfers; i++)
         chamfered[byAngle[i].second] = true;
      lines.clear();
      for (int i = 0; i < n; i++)
      {
         lines.push_back(ordered[i]);
         if (!chamfered[i])
            continue;
         const Facet& a = ordered[i];
         const Facet& b = ordered[(i + 1) % n];
         Facet c;
         c.normal = Norm(a.normal + b.normal, a.normal);
         c.point = ball - c.normal * ballRadius;
         c.angle = (a.angle + (b.angle <= a.angle ? b.angle + kFullTurn : b.angle)) * 0.5f;
         lines.push_back(c);
      }
   }

   profile.ball = ball;
   profile.closed = closed;
   const int n = (int)lines.size();
   const auto perp = [](const V2 v) { return V2 { -v.y, v.x }; };
   const float rim = std::max(5.f, ballRadius * 0.2f);
   if (n == 1)
   {
      const V2 t = perp(lines[0].normal);
      profile.verts = { lines[0].point - t * rim, lines[0].point + t * rim };
      profile.spans = { { 0, 1 } };
      return true;
   }
   const auto parallel = [&]()
   {
      error = "Two neighbouring wires give parallel facets: move one of them";
      profile = Profile {};
      return false;
   };
   if (closed)
   {
      for (int i = 0; i < n; i++)
      {
         V2 v;
         if (!Intersect(lines[(i - 1 + n) % n], lines[i], v))
            return parallel();
         profile.verts.push_back(v);
      }
      for (int i = 0; i < n; i++)
         profile.spans.push_back({ i, (i + 1) % n });
      return true;
   }
   if (openTop)
   {
      for (const Facet& l : lines)
         profile.verts.push_back(l.point);
      for (int i = 0; i < n - 1; i++)
         profile.spans.push_back({ i, i + 1 });
      return true;
   }
   std::vector<V2> corners;
   for (int i = 1; i < n; i++)
   {
      V2 v;
      if (!Intersect(lines[i - 1], lines[i], v))
         return parallel();
      corners.push_back(v);
   }
   const V2 firstDir = Norm(corners.front() - lines.front().point, perp(lines.front().normal));
   const V2 lastDir = Norm(lines.back().point - corners.back(), perp(lines.back().normal));
   profile.verts.push_back(lines.front().point - firstDir * rim);
   profile.verts.insert(profile.verts.end(), corners.begin(), corners.end());
   profile.verts.push_back(lines.back().point + lastDir * rim);
   for (int i = 0; i < n; i++)
      profile.spans.push_back({ i, i + 1 });
   return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Route

struct PathPoint
{
   float x, y, z;
   bool control; // A control point of the route, carrying a roll
};

struct Sample
{
   V3 p;
   float roll; // Degrees
};

struct Frame
{
   V3 p, t, r, u; // Position, tangent, right, up
};

// The fine route with the rolls of its control points (rolls[k] for the k-th point marked control; the last point of the route is always the last roll),
// spans where the roll changes subdivided so that the roll eases in steps of at most 2 degrees, then thinned to the rows the sweep needs: a row is
// kept where the tangent or the roll turned by more than 45 / detail degrees, or the route left the chord by more than 4 / detail (freezy's Curvature Detail).
inline std::vector<Sample> BuildRows(const std::vector<PathPoint>& fine, const std::vector<float>& rolls, const int curvatureDetail)
{
   const int detail = std::clamp(curvatureDetail, 2, 32);
   const float maxAngle = (45.f / (float)detail) * 3.14159265358979f / 180.f;
   const float maxRoll = 45.f / (float)detail;
   const float maxDev = 4.f / (float)detail;
   std::vector<Sample> all;
   // Spans between control points
   std::vector<size_t> spanStart;
   for (size_t i = 0; i < fine.size(); i++)
      if (fine[i].control || i == 0)
         spanStart.push_back(i);
   spanStart.push_back(fine.size() - 1);
   for (size_t s = 0; s + 1 < spanStart.size(); s++)
   {
      const size_t a = spanStart[s], b = spanStart[s + 1];
      if (b <= a)
         continue;
      const float r0 = s < rolls.size() ? rolls[s] : (rolls.empty() ? 0.f : rolls.back());
      const float r1 = s + 1 < rolls.size() ? rolls[s + 1] : r0;
      float total = 0.f;
      for (size_t i = a; i < b; i++)
         total += WireSweep::Len(V3 { fine[i + 1].x - fine[i].x, fine[i + 1].y - fine[i].y, fine[i + 1].z - fine[i].z });
      const float easedRoll = r0 == r1 ? 0.f : fabsf(r1 - r0);
      float done = 0.f;
      for (size_t i = a; i < b; i++)
      {
         const V3 pa { fine[i].x, fine[i].y, fine[i].z }, pb { fine[i + 1].x, fine[i + 1].y, fine[i + 1].z };
         const float len = WireSweep::Len(pb - pa);
         const int sub = total > 0.f && easedRoll > 0.f ? std::max(1, (int)ceilf(easedRoll * len / total / 2.f)) : 1;
         for (int k = 0; k < sub; k++)
         {
            const float f = (float)k / (float)sub;
            const float u = total > 0.f ? (done + len * f) / total : 0.f;
            const float e = u * u * (3.f - 2.f * u);
            all.push_back({ pa + (pb - pa) * f, r0 + (r1 - r0) * e });
         }
         done += len;
      }
   }
   if (fine.empty())
      return {};
   all.push_back({ V3 { fine.back().x, fine.back().y, fine.back().z }, rolls.empty() ? 0.f : rolls.back() });

   std::vector<Sample> rows;
   rows.push_back(all.front());
   size_t anchor = 0;
   const auto dirAt = [&](const size_t i) { return WireSweep::Norm(all[i + 1].p - all[i].p); };
   for (size_t i = 1; i + 1 < all.size(); i++)
   {
      bool keep = fabsf(all[i + 1].roll - all[anchor].roll) > maxRoll;
      const V3 dirA = dirAt(anchor), dirI = dirAt(i);
      keep |= acosf(std::clamp(WireSweep::Dot(dirA, dirI), -1.f, 1.f)) > maxAngle;
      const V3 chord = all[i + 1].p - all[anchor].p;
      const float cl = WireSweep::Len(chord);
      for (size_t k = anchor + 1; k <= i && !keep && cl > 1e-6f; k++)
         keep = WireSweep::Len(WireSweep::Cross(all[k].p - all[anchor].p, chord)) / cl > maxDev;
      if (keep)
      {
         rows.push_back(all[i]);
         anchor = i;
      }
   }
   rows.push_back(all.back());
   return rows;
}

// Frames of the rows: a rising rail stays upright (right is horizontal, up leans with the slope), the roll turns both about the tangent, positive raises the right side.
inline std::vector<Frame> BuildFrames(const std::vector<Sample>& rows)
{
   using WireSweep::Norm;
   const int n = (int)rows.size();
   std::vector<Frame> f(n);
   V3 prevRight { 1.f, 0.f, 0.f };
   for (int i = 0; i < n; i++)
   {
      const V3 t = Norm(rows[i < n - 1 ? i + 1 : i].p - rows[i > 0 ? i - 1 : i].p);
      V3 r { t.y, -t.x, 0.f }; // Tangent x world up. +Y travel has +X on its right, as in VPE
      if (WireSweep::Len(r) < 0.02f) // Vertical: keep the last right, square to the tangent
         r = prevRight - t * WireSweep::Dot(prevRight, t);
      else if (i > 0 && WireSweep::Dot(r, prevRight) < 0.f) // Past the vertical in a loop the cross product flips: keep the side
         r = r * -1.f;
      r = Norm(r);
      prevRight = r;
      const V3 u = Norm(WireSweep::Cross(r, t));
      const float a = rows[i].roll * 3.14159265358979f / 180.f, c = cosf(a), s = sinf(a);
      f[i] = { rows[i].p, t, Norm(r * c + u * s), Norm(u * c - r * s) };
   }
   return f;
}

// The smallest turning radius of the route in the plane of its rows, for the 'wires fold on themselves' warning (a large value on a straight route)
inline float MinRadius(const std::vector<Frame>& f)
{
   float res = 1e30f;
   for (size_t i = 1; i + 1 < f.size(); i++)
   {
      const float len = WireSweep::Len(f[i + 1].p - f[i - 1].p) * 0.5f;
      const float ang = acosf(std::clamp(WireSweep::Dot(WireSweep::Norm(f[i].p - f[i - 1].p), WireSweep::Norm(f[i + 1].p - f[i].p)), -1.f, 1.f));
      if (ang > 1e-3f)
         res = std::min(res, len / ang);
   }
   return res;
}

// Heights of 'z' between the control points first and last on a straight grade by the plan distance along the route. plan[k] = plan distance of control point k.
inline bool Grade(const std::vector<float>& plan, std::vector<float>& z, const int first, const int last)
{
   if (first < 0 || last >= (int)z.size() || last >= (int)plan.size() || first >= last)
      return false;
   const float total = plan[last] - plan[first];
   if (total <= 1e-5f)
      return false;
   for (int k = first + 1; k < last; k++)
      z[k] = z[first] + (z[last] - z[first]) * (plan[k] - plan[first]) / total;
   return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Meshes

// One wire: a tube along its centreline (parallel transported rings, as WireSweep), chamfered at both ends by one segment of 'bevel', closed by a disc
template <class Vertex>
bool AppendTube(const std::vector<V3>& centerline, const float radius, int sides, float bevel, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
{
   using WireSweep::Norm;
   sides = std::clamp(sides, 3, 64);
   std::vector<V3> p;
   for (const V3& c : centerline)
      if (p.empty() || WireSweep::Len(c - p.back()) > 1e-4f)
         p.push_back(c);
   const int n = (int)p.size();
   if (n < 2)
      return false;
   std::vector<float> s(n, 0.f);
   for (int i = 1; i < n; i++)
      s[i] = s[i - 1] + WireSweep::Len(p[i] - p[i - 1]);
   const float total = s[n - 1] > 0.f ? s[n - 1] : 1.f;
   bevel = std::clamp(bevel, 0.f, radius * 0.95f);
   bevel = std::min({ bevel, WireSweep::Len(p[1] - p[0]) * 0.49f, WireSweep::Len(p[n - 1] - p[n - 2]) * 0.49f });
   const V3 endS = p[0], endE = p[n - 1];
   const V3 tS = Norm(p[1] - p[0]), tE = Norm(p[n - 1] - p[n - 2]);
   p[0] = endS + tS * bevel; // The chamfer starts this far in
   p[n - 1] = endE - tE * bevel;

   const float twoPi = 6.28318530717958647692f;
   const auto ring = [&](const V3& c, const V3& nrm, const V3& bin, const float r, const V3& tiltBy, const float tilt, const float u)
   {
      for (int j = 0; j < sides; j++)
      {
         const float a = twoPi * (float)j / (float)sides;
         const V3 radial = nrm * cosf(a) + bin * sinf(a);
         const V3 pos = c + radial * r;
         const V3 nn = tilt >= 2.f ? tiltBy : Norm(radial * (1.f - tilt) + tiltBy * tilt);
         Vertex v;
         v.x = pos.x;
         v.y = pos.y;
         v.z = pos.z;
         v.nx = nn.x;
         v.ny = nn.y;
         v.nz = nn.z;
         v.tu = u;
         v.tv = (float)j / (float)sides;
         verts.push_back(v);
      }
   };
   const auto strip = [&](const unsigned first, const int count)
   {
      for (int i = 0; i < count - 1; i++)
         for (int j = 0; j < sides; j++)
         {
            const unsigned q0 = first + i * sides + j, q1 = first + i * sides + ((j + 1) % sides);
            const unsigned q2 = first + (i + 1) * sides + j, q3 = first + (i + 1) * sides + ((j + 1) % sides);
            idx.insert(idx.end(), { q0, q1, q2, q3, q2, q1 });
         }
   };

   const unsigned base = (unsigned)verts.size();
   V3 prevN {}, firstN {}, firstB {}, lastN {}, lastB {};
   for (int i = 0; i < n; i++)
   {
      const V3 t = Norm(i == n - 1 ? p[i] - p[i - 1] : (i == 0 ? p[1] - p[0] : p[i + 1] - p[i - 1]));
      V3 nrm;
      if (i == 0)
         nrm = Norm(WireSweep::Cross(t, fabsf(t.z) > 0.999f ? V3 { 1.f, 0.f, 0.f } : V3 { 0.f, 0.f, 1.f }));
      else
      {
         nrm = Norm(prevN - t * WireSweep::Dot(prevN, t));
         if (WireSweep::Len(nrm) < 0.5f)
            nrm = Norm(WireSweep::Cross(t, fabsf(t.z) > 0.999f ? V3 { 1.f, 0.f, 0.f } : V3 { 0.f, 0.f, 1.f }));
      }
      const V3 bin = Norm(WireSweep::Cross(t, nrm));
      prevN = nrm;
      if (i == 0)
      {
         firstN = nrm;
         firstB = bin;
      }
      lastN = nrm;
      lastB = bin;
      ring(p[i], nrm, bin, radius, t, 0.f, s[i] / total);
   }
   strip(base, n);

   // The two ends. Vertices of their own: the chamfer and the disc are shaded flat. A ring carries its tilt as a blend towards the outward end direction
   const auto capEnd = [&](const bool atEnd)
   {
      const V3 tip = atEnd ? endE : endS;
      const V3 inner = atEnd ? p[n - 1] : p[0];
      const V3 out = atEnd ? tE : tS * -1.f;
      const V3 nrm = atEnd ? lastN : firstN, bin = atEnd ? lastB : firstB;
      const float u = atEnd ? 1.f : 0.f;
      if (bevel > 1e-4f)
      {
         const unsigned first = (unsigned)verts.size();
         if (atEnd) // Rings in the direction of travel, as the sweep: outward winding
         {
            ring(inner, nrm, bin, radius, out, 0.5f, u);
            ring(tip, nrm, bin, radius - bevel, out, 0.5f, u);
         }
         else
         {
            ring(tip, nrm, bin, radius - bevel, out, 0.5f, u);
            ring(inner, nrm, bin, radius, out, 0.5f, u);
         }
         strip(first, 2);
      }
      const unsigned center = (unsigned)verts.size();
      Vertex cv;
      cv.x = tip.x;
      cv.y = tip.y;
      cv.z = tip.z;
      cv.nx = out.x;
      cv.ny = out.y;
      cv.nz = out.z;
      cv.tu = u;
      cv.tv = 0.5f;
      verts.push_back(cv);
      ring(tip, nrm, bin, radius - bevel, out, 2.f, u);
      for (int j = 0; j < sides; j++)
      {
         const unsigned a0 = center + 1 + j, a1 = center + 1 + ((j + 1) % sides);
         if (atEnd)
            idx.insert(idx.end(), { center, a0, a1 });
         else
            idx.insert(idx.end(), { center, a1, a0 });
      }
   };
   capEnd(false);
   capEnd(true);
   return true;
}

inline std::vector<V3> WireCenterline(const std::vector<Frame>& frames, const Wire& w)
{
   std::vector<V3> c;
   c.reserve(frames.size());
   for (const Frame& f : frames)
      c.push_back(f.p + f.r * w.x + f.u * w.z);
   return c;
}

// The visible wires
template <class Vertex>
bool BuildTubes(const std::vector<Frame>& frames, const std::vector<Wire>& wires, const float radius, const int sides, const float bevel, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
{
   bool any = false;
   for (const Wire& w : wires)
      if (w.active)
         any |= AppendTube<Vertex>(WireCenterline(frames, w), radius, sides, bevel, verts, idx);
   return any;
}

// The profile swept along the route: rows of profile vertices, quads two sided as the ball can come from either side (a HitTriangle is one sided).
// 'edges': the long edges (a ball rolling off a facet edge), as line colliders. As in VPE no edges across the route: they lie in the floor and would only give the ball
// a seam to catch on, and at an entry a lip a little proud of what feeds the rail would kick the ball out (measured: 11 at speed 40).
struct Channel
{
   std::vector<V3> verts;
   std::vector<unsigned> tris;
   std::vector<std::pair<unsigned, unsigned>> edges;
   int rowSize = 0, rows = 0;
};

inline Channel BuildChannel(const std::vector<Frame>& frames, const Profile& profile)
{
   Channel c;
   c.rowSize = (int)profile.verts.size();
   c.rows = (int)frames.size();
   if (c.rowSize < 2 || c.rows < 2)
      return Channel {};
   c.verts.reserve((size_t)c.rowSize * c.rows);
   for (const Frame& f : frames)
      for (const V2& v : profile.verts)
         c.verts.push_back(f.p + f.r * v.x + f.u * v.y);
   for (int i = 0; i + 1 < c.rows; i++)
   {
      const unsigned row = (unsigned)(i * c.rowSize), next = row + (unsigned)c.rowSize;
      for (const auto& span : profile.spans)
      {
         const unsigned a = row + span.first, b = next + span.first, d = row + span.second, e = next + span.second;
         c.tris.insert(c.tris.end(), { a, b, d, b, e, d, a, d, b, b, d, e });
      }
      for (int v = 0; v < c.rowSize; v++)
         c.edges.push_back({ row + v, next + v });
   }
   return c;
}

}
