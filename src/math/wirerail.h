// license:GPLv3+

// Wire Rail: tubes and a ball channel swept along a 3D route, after freezy's Wire Rail in VPE (creators-guide/editor/wire-rails): up to six wires placed
// by (x right of the route, z up from it), a frame that follows the route and a per-point roll, and a collider that is not the tubes but flat facets through the
// points where a ball resting on wires 1 and 2 would touch each wire, swept along the route.
// Header only and templated on the vertex type so it can be checked standalone (tools/wirerail_check.cpp). No dependency on the engine.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
   bool force = false; // BuildRows keeps this sample as a row (the Ramp Collider: its control points and the end of its entry ease). The rail never sets it
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
   std::vector<char> allForce; // Parallel to 'all': the sample came from a fine sample with PathPoint::force
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
            allForce.push_back(k == 0 && fine[i].force ? 1 : 0);
         }
         done += len;
      }
   }
   if (fine.empty())
      return {};
   all.push_back({ V3 { fine.back().x, fine.back().y, fine.back().z }, rolls.empty() ? 0.f : rolls.back() });
   allForce.push_back(1);

   std::vector<Sample> rows;
   rows.push_back(all.front());
   size_t anchor = 0;
   const auto dirAt = [&](const size_t i) { return WireSweep::Norm(all[i + 1].p - all[i].p); };
   for (size_t i = 1; i + 1 < all.size(); i++)
   {
      bool keep = allForce[i] != 0 || fabsf(all[i + 1].roll - all[anchor].roll) > maxRoll;
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
// Options and a report for a tube that meets a fitting (phase 2b): an end left open is not capped and has no bevel (the fitting's tube continues from its ring), and the
// ring orientation of the first ring can be seeded so that it lines up with the ring of the rail it meets. The defaults give the tube as before, byte for byte
struct TubeOpts
{
   bool capStart = true, capEnd = true;
   bool seeded = false;
   V3 seed { 0.f, 0.f, 0.f }; // The side direction of the first ring (the mouth ring of the rail), made square to the first segment
};
struct TubeInfo
{
   int firstRing = -1, lastRing = -1; // Index of the first vertex of the first and of the last ring of the tube proper (the caps come after)
   V3 firstSide { 0.f, 0.f, 0.f }, lastSide { 0.f, 0.f, 0.f }; // The side direction of those rings
};

template <class Vertex>
bool AppendTube(const std::vector<V3>& centerline, const float radius, int sides, float bevel, std::vector<Vertex>& verts, std::vector<unsigned>& idx, const TubeOpts* opts = nullptr,
   TubeInfo* info = nullptr)
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
   const bool capS = opts == nullptr || opts->capStart, capE = opts == nullptr || opts->capEnd;
   const float bevelS = capS ? bevel : 0.f, bevelE = capE ? bevel : 0.f;
   const V3 endS = p[0], endE = p[n - 1];
   const V3 tS = Norm(p[1] - p[0]), tE = Norm(p[n - 1] - p[n - 2]);
   p[0] = endS + tS * bevelS; // The chamfer starts this far in
   p[n - 1] = endE - tE * bevelE;

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
      {
         nrm = Norm(WireSweep::Cross(t, fabsf(t.z) > 0.999f ? V3 { 1.f, 0.f, 0.f } : V3 { 0.f, 0.f, 1.f }));
         if (opts != nullptr && opts->seeded)
         {
            const V3 sq = opts->seed - t * WireSweep::Dot(opts->seed, t);
            if (WireSweep::Len(sq) > 1e-4f)
               nrm = Norm(sq);
         }
      }
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
   if (info != nullptr)
   {
      info->firstRing = (int)base;
      info->lastRing = (int)(base + (unsigned)(n - 1) * (unsigned)sides);
      info->firstSide = firstN;
      info->lastSide = lastN;
   }

   // The two ends. Vertices of their own: the chamfer and the disc are shaded flat. A ring carries its tilt as a blend towards the outward end direction
   const auto endCap = [&](const bool atEnd)
   {
      const float bv = atEnd ? bevelE : bevelS;
      const V3 tip = atEnd ? endE : endS;
      const V3 inner = atEnd ? p[n - 1] : p[0];
      const V3 out = atEnd ? tE : tS * -1.f;
      const V3 nrm = atEnd ? lastN : firstN, bin = atEnd ? lastB : firstB;
      const float u = atEnd ? 1.f : 0.f;
      if (bv > 1e-4f)
      {
         const unsigned first = (unsigned)verts.size();
         if (atEnd) // Rings in the direction of travel, as the sweep: outward winding
         {
            ring(inner, nrm, bin, radius, out, 0.5f, u);
            ring(tip, nrm, bin, radius - bv, out, 0.5f, u);
         }
         else
         {
            ring(tip, nrm, bin, radius - bv, out, 0.5f, u);
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
      ring(tip, nrm, bin, radius - bv, out, 2.f, u);
      for (int j = 0; j < sides; j++)
      {
         const unsigned a0 = center + 1 + j, a1 = center + 1 + ((j + 1) % sides);
         if (atEnd)
            idx.insert(idx.end(), { center, a0, a1 });
         else
            idx.insert(idx.end(), { center, a1, a0 });
      }
   };
   if (capS)
      endCap(false);
   if (capE)
      endCap(true);
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
struct ChannelPiece
{
   unsigned firstVert = 0; // Index of the piece's first row vertex in Channel::verts
   int rowSize = 0, rows = 0;
};

struct Channel
{
   std::vector<V3> verts;
   std::vector<unsigned> tris;
   std::vector<std::pair<unsigned, unsigned>> edges;
   int rowSize = 0, rows = 0; // Of the first piece
   // Phase 2b: the route cut at every rail trim, a profile per piece (one piece without trims, built as before)
   std::vector<ChannelPiece> pieces;
   size_t quadIndices = 0; // The leading indices of 'tris' are quads of 12 (two sided pairs); what follows (a hairpin's box) are single triangles
   // What is the physics of a hairpin's end loop only (Terminal Impact): flags per triangle (3 indices), edge and vertex, empty when nothing is
   std::vector<unsigned char> triTerminal, edgeTerminal, vertTerminal;
};

// Exact compare: BuildChannel is deterministic from the same frames and profile
inline bool SameChannel(const Channel& a, const Channel& b)
{
   return a.rowSize == b.rowSize && a.rows == b.rows && a.verts.size() == b.verts.size() && a.tris == b.tris && a.edges == b.edges && a.quadIndices == b.quadIndices
      && a.triTerminal == b.triTerminal && a.edgeTerminal == b.edgeTerminal && a.vertTerminal == b.vertTerminal && a.pieces.size() == b.pieces.size()
      && (a.verts.empty() || memcmp(a.verts.data(), b.verts.data(), a.verts.size() * sizeof(V3)) == 0);
}

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
   c.pieces.push_back({ 0u, c.rowSize, c.rows });
   c.quadIndices = c.tris.size();
   return c;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Fixtures: freezy's supports (Ring, Rung, Cradle, Stand) and their solder blobs (WireRailGeometryGenerator / WireRailSolderMeshGenerator in VPE). Visual only: they
// are in the render mesh, never in the ball channel. Each is fitted to the wires at its Position, a distance along the route.

inline constexpr int kFixtureRing = 0, kFixtureRung = 1, kFixtureCradle = 2, kFixtureStand = 3;
// Phase 2b: the end fittings. Unlike the supports they change the ball channel: they trim rails in the collider, and the hairpin and the elbow add colliders of their own
inline constexpr int kFixtureHairpin = 4, kFixtureElbow = 5, kFixtureTrim = 6;
inline constexpr int kFixtureTypeCount = 7;
inline constexpr size_t kMaxFixtures = 256;
inline constexpr int kEndStart = 0, kEndEnd = 1;
inline bool IsEndFitting(const int type) { return type >= kFixtureHairpin && type <= kFixtureTrim; }

// One flat struct for every type (freezy has a class per type): only the fields of its type are used
struct Fixture
{
   int type = kFixtureRing;
   float position = 0.f; // 3D length along the route
   bool enabled = true;
   float solderThreshold = 2.f, solderSize = 1.f;
   float offsetX = 0.f, offsetZ = 0.f; // Ring, Cradle, Rung and the Stand's attachment
   int density = 32; // Ring, Cradle
   // Ring
   float scale = 1.f;
   bool cutout = false;
   float cutoutStart = 60.f, cutoutEnd = 120.f;
   bool straight = false;
   float straightStart = 210.f, straightEnd = 330.f;
   // Rung (angle), Rung and Stand attachment (length)
   float angle = 0.f, length = 0.f;
   // Cradle
   float bottomLength = 8.f, leftArm = 85.f, rightArm = 85.f, armAngle = 53.130102f, rotation = 0.f, cornerRadius = 8.f;
   // Stand
   bool legLeft = false;
   V3 startVector { 0.f, 0.f, -1.f };
   float startLength = 40.f;
   V3 footPosition { 15.f, -22.5f, -80.f };
   V3 footRotation { 0.f, 0.f, 0.f };
   bool footClockwise = false;
   float footWidth = 30.f, footArm = 30.f, footConnectedArm = 30.f;
   // End fittings (no Position: the endpoint is where they are, the stored position is the route start or end)
   int endpoint = kEndEnd; // Hairpin, Elbow, Rail Trim
   int railA = 0, railB = 1; // Hairpin, Elbow: the two rails taken over (indices, wire 1 is 0)
   float loopDiameter = 60.f, leadLength = 40.f, tangentLength = 15.f; // Hairpin (density 24 and rotation are the shared fields)
   float fitOffset = 0.f; // Hairpin and Elbow: how far the attachment sits back from the endpoint, along the rails (0 hairpin, 40 elbow)
   float dropLength = 80.f, zAngle = 0.f; // Elbow
   float cutoff[kMaxWires] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f }; // Rail Trim: per wire; Elbow: the other rails' cutoffs (render only). Six slots always, a Rails count change keeps them
};

inline const char* FixtureTypeName(const int type)
{
   switch (type)
   {
   case kFixtureRing: return "Ring";
   case kFixtureRung: return "Rung";
   case kFixtureCradle: return "Cradle";
   case kFixtureStand: return "Stand";
   case kFixtureHairpin: return "Hairpin";
   case kFixtureElbow: return "Elbow";
   case kFixtureTrim: return "Rail Trim";
   default: return "Fixture";
   }
}
inline std::string FixtureName(const Fixture& f, const int index) { return std::string(FixtureTypeName(f.type)) + " " + std::to_string(index + 1); }

// The first two active wires: an elbow's default pair (freezy's TryGetDefaultEndpointRailPair). False: fewer than two, the pair is 0 and 1
inline bool DefaultRailPair(const std::vector<Wire>& wires, int& a, int& b)
{
   a = 0;
   b = std::min(1, std::max(0, (int)wires.size() - 1));
   int first = -1;
   for (int i = 0; i < (int)wires.size(); i++)
   {
      if (!wires[i].active)
         continue;
      if (first < 0)
      {
         first = i;
         continue;
      }
      a = first;
      b = i;
      return true;
   }
   return false;
}

// A new fixture at 'position' with freezy's defaults (WireRailComponent.AddRingFixture and the others). An end fitting is added at the End: its position is the route length
// ('position' is that); a hairpin has density 24, an elbow its Offset 40 and the first two active of 'wires' as its pair (the hairpin takes wires 1 and 2, as freezy's)
inline Fixture DefaultFixture(const int type, const float position, const std::vector<Wire>* wires = nullptr)
{
   Fixture f;
   f.type = type;
   f.position = position;
   if (type == kFixtureHairpin)
      f.density = 24;
   else if (type == kFixtureElbow)
   {
      f.fitOffset = 40.f;
      if (wires != nullptr)
         DefaultRailPair(*wires, f.railA, f.railB);
   }
   return f;
}

// As freezy's Ensure*Initialized on load: bad values are clamped (a not-a-number takes the default), the type stays whatever it is (a newer file)
inline void ClampFixture(Fixture& f)
{
   const Fixture def;
   const auto fin = [](float& v, const float d)
   {
      // A bit test: the engine builds with -ffast-math, where the compiler drops x != x and std::isfinite
      uint32_t bits;
      memcpy(&bits, &v, sizeof(bits));
      if ((bits & 0x7F800000u) == 0x7F800000u) // Exponent all ones: not-a-number or infinity
         v = d;
   };
   fin(f.position, 0.f);
   fin(f.solderThreshold, def.solderThreshold);
   fin(f.solderSize, def.solderSize);
   fin(f.offsetX, 0.f);
   fin(f.offsetZ, 0.f);
   fin(f.scale, def.scale);
   fin(f.cutoutStart, def.cutoutStart);
   fin(f.cutoutEnd, def.cutoutEnd);
   fin(f.straightStart, def.straightStart);
   fin(f.straightEnd, def.straightEnd);
   fin(f.angle, 0.f);
   fin(f.length, 0.f);
   fin(f.bottomLength, def.bottomLength);
   fin(f.leftArm, def.leftArm);
   fin(f.rightArm, def.rightArm);
   fin(f.armAngle, def.armAngle);
   fin(f.rotation, 0.f);
   fin(f.cornerRadius, def.cornerRadius);
   fin(f.startLength, def.startLength);
   fin(f.footWidth, def.footWidth);
   fin(f.footArm, def.footArm);
   fin(f.footConnectedArm, def.footConnectedArm);
   fin(f.loopDiameter, def.loopDiameter);
   fin(f.leadLength, def.leadLength);
   fin(f.tangentLength, def.tangentLength);
   fin(f.fitOffset, 0.f);
   fin(f.dropLength, def.dropLength);
   fin(f.zAngle, 0.f);
   for (float& c : f.cutoff)
      fin(c, 0.f);
   for (V3* v : { &f.startVector, &f.footPosition, &f.footRotation })
   {
      fin(v->x, 0.f);
      fin(v->y, 0.f);
      fin(v->z, 0.f);
   }
   f.position = std::max(0.f, f.position);
   f.density = std::clamp(f.density, f.type == kFixtureHairpin ? 4 : 3, 128);
   f.scale = std::max(0.1f, f.scale);
   f.cutoutStart = std::clamp(f.cutoutStart, 0.f, 360.f);
   f.cutoutEnd = std::clamp(f.cutoutEnd, 0.f, 360.f);
   f.straightStart = std::clamp(f.straightStart, 0.f, 360.f);
   f.straightEnd = std::clamp(f.straightEnd, 0.f, 360.f);
   f.angle = std::clamp(f.angle, 0.f, 360.f);
   f.armAngle = std::clamp(f.armAngle, 1.f, 179.f);
   f.rotation = std::clamp(f.rotation, 0.f, 360.f);
   f.bottomLength = std::max(0.1f, f.bottomLength);
   f.leftArm = std::max(0.f, f.leftArm);
   f.rightArm = std::max(0.f, f.rightArm);
   f.cornerRadius = std::max(0.1f, f.cornerRadius);
   f.startLength = std::max(0.f, f.startLength);
   f.footWidth = std::max(0.1f, f.footWidth);
   f.footArm = std::max(0.f, f.footArm);
   f.footConnectedArm = std::max(0.f, f.footConnectedArm);
   f.solderThreshold = std::max(0.f, f.solderThreshold);
   f.solderSize = std::max(0.01f, f.solderSize);
   f.endpoint = f.endpoint == kEndStart ? kEndStart : kEndEnd;
   f.railA = std::clamp(f.railA, 0, kMaxWires - 1);
   f.railB = std::clamp(f.railB, 0, kMaxWires - 1);
   f.loopDiameter = std::max(0.1f, f.loopDiameter);
   f.leadLength = std::max(0.f, f.leadLength);
   f.tangentLength = std::max(0.f, f.tangentLength);
   f.fitOffset = std::max(0.f, f.fitOffset);
   f.dropLength = std::max(0.1f, f.dropLength);
   for (float& c : f.cutoff)
      c = std::max(0.f, c);
   if (WireSweep::Len(f.startVector) > 1e-4f)
      f.startVector = WireSweep::Norm(f.startVector);
   else
      f.startVector = def.startVector;
}

// freezy's Level button (AlignAngleRangeHorizontally): keeps the width of an angle range and recentres it on straight up or down, whichever is nearer
inline V2 LevelAngleRange(float start, float end)
{
   const auto repeat = [](const float t, const float len) { return std::clamp(t - floorf(t / len) * len, 0.f, len); };
   const auto delta = [&](const float cur, const float tgt)
   {
      float d = repeat(tgt - cur, 360.f);
      if (d > 180.f)
         d -= 360.f;
      return d;
   };
   start = std::clamp(start, 0.f, 360.f);
   end = std::clamp(end, 0.f, 360.f);
   const float raw = end - start;
   if (fabsf(raw) >= 359.999f)
      return { start, end };
   const float sweep = fmodf(raw + 360.f, 360.f);
   if (sweep <= 0.001f)
      return { start, end };
   const float half = sweep * 0.5f;
   const float center = repeat(start + half, 360.f);
   const float aligned = fabsf(delta(center, 90.f)) <= fabsf(delta(center, 270.f)) ? 90.f : 270.f;
   const float alignedStart = repeat(aligned - half, 360.f);
   float alignedEnd = repeat(aligned + half, 360.f);
   if (fabsf(alignedEnd) < 1e-4f && aligned + half > 0.f)
      alignedEnd = 360.f;
   return { alignedStart, alignedEnd };
}

// freezy's Mirror button of a stand: flips the side, negates x of the start vector and the foot position, y and z of the foot rotation, x of the attachment offset
inline void MirrorStand(Fixture& f)
{
   f.legLeft = !f.legLeft;
   f.startVector.x = -f.startVector.x;
   f.footPosition.x = -f.footPosition.x;
   f.footRotation.y = -f.footRotation.y;
   f.footRotation.z = -f.footRotation.z;
   f.offsetX = -f.offsetX;
   f.footClockwise = !f.footClockwise;
}

// Object > Mirror of the rail: the route keeps its direction and the section is mirrored (x, right of the route, changes sign), as the wires' x and the rolls
// are. A ring's angles (anticlockwise from the right) go to 180 - a, so its cut out and straight ranges swap ends; a rung turns the other way; a cradle swaps
// its arms and turns the other way; a stand is freezy's Mirror button. Mirroring twice gives the fixture back
inline void MirrorFixture(Fixture& f)
{
   // No negative zero (it would show as -0 and be saved so). The engine builds with -ffast-math, which drops a float test for zero and a select on the bits, so
   // the sign bit is cleared by integer arithmetic, the same as MirrorParts' NoNegZero (kept here: this header stands alone for wirerail_check)
   const auto plus = [](float& v)
   {
      uint32_t b;
      std::memcpy(&b, &v, sizeof(b));
      const uint32_t magnitude = b & 0x7fffffffu;
      b &= 0x7fffffffu | (((magnitude + 0x7fffffffu) >> 31) << 31);
      std::memcpy(&v, &b, sizeof(b));
   };
   const auto neg = [&plus](const float v)
   {
      float r = -v;
      plus(r);
      return r;
   };
   const auto wrap = [](const float a) { return a - 360.f * floorf(a / 360.f); };
   const auto range = [&wrap](float& start, float& end)
   {
      if (fabsf(end - start) >= 359.999f)
         return; // The whole circle: the same mirrored
      const float s = start;
      start = wrap(180.f - end);
      end = wrap(180.f - s);
   };
   switch (f.type)
   {
   case kFixtureRing:
      range(f.cutoutStart, f.cutoutEnd);
      range(f.straightStart, f.straightEnd);
      break;
   case kFixtureRung: f.angle = wrap(-f.angle); plus(f.angle); break; // Kept in 0..360 (a load and every edit clamp to it: -20 would come back as 0)
   case kFixtureCradle:
      std::swap(f.leftArm, f.rightArm);
      f.rotation = wrap(360.f - f.rotation); // Kept in 0..360 (the build clamps to it)
      break;
   case kFixtureStand:
   {
      MirrorStand(f); // Its attachment's offset too
      plus(f.startVector.x);
      plus(f.footPosition.x);
      plus(f.footRotation.y);
      plus(f.footRotation.z);
      plus(f.offsetX);
      return;
   }
   case kFixtureHairpin: f.rotation = wrap(360.f - f.rotation); plus(f.rotation); return; // The loop's diameter turned about the route turns the other way; the wires keep their numbers
   case kFixtureElbow: f.zAngle = neg(f.zAngle); return; // The bend turns the other way about the route's up
   case kFixtureTrim: return; // Wire numbers and distances do not change
   default: return; // A type of a later version is left as it is
   }
   f.offsetX = neg(f.offsetX);
}

// The wires at a distance along the route. One set for the whole route in 2a; layouts (2c) change it along the route
inline const std::vector<Wire>& WiresAt(const std::vector<Wire>& wires, const float /*distance*/) { return wires; }

// 3D length along the route to each row
inline std::vector<float> RowDistances(const std::vector<Frame>& frames)
{
   std::vector<float> d(frames.size(), 0.f);
   for (size_t i = 1; i < frames.size(); i++)
      d[i] = d[i - 1] + WireSweep::Len(frames[i].p - frames[i - 1].p);
   return d;
}

// The frame at a 3D length: lerped between the rows (position, tangent, right, up), then made square again. Past either end it is the end
inline Frame FrameAt(const std::vector<Frame>& frames, const std::vector<float>& rowDist, float d)
{
   if (frames.empty())
      return Frame { { 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f } };
   if (frames.size() == 1 || rowDist.size() != frames.size())
      return frames.front();
   d = std::clamp(d, 0.f, rowDist.back());
   size_t i = (size_t)(std::upper_bound(rowDist.begin(), rowDist.end(), d) - rowDist.begin());
   i = i == 0 ? 0 : std::min(i - 1, frames.size() - 2);
   const float span = rowDist[i + 1] - rowDist[i];
   const float t = span > 1e-9f ? std::clamp((d - rowDist[i]) / span, 0.f, 1.f) : 0.f;
   const Frame &a = frames[i], &b = frames[i + 1];
   using WireSweep::Cross;
   using WireSweep::Dot;
   using WireSweep::Norm;
   Frame f;
   f.p = a.p + (b.p - a.p) * t;
   V3 tan = Norm(a.t + (b.t - a.t) * t);
   if (WireSweep::Len(tan) < 0.5f)
      tan = a.t;
   V3 r = a.r + (b.r - a.r) * t;
   r = r - tan * Dot(r, tan);
   r = Norm(r);
   if (WireSweep::Len(r) < 0.5f)
      r = Norm(a.r - tan * Dot(a.r, tan));
   f.t = tan;
   f.r = r;
   f.u = Norm(Cross(r, tan));
   return f;
}

struct FixtureShape
{
   bool built = false; // Has geometry (a disabled fixture is built, only left out of the mesh)
   std::string reason; // Why it can not be built
   bool inBallPath = false; // Its centreline passes through where the resting ball is: it does not collide, the ball goes through it
   float position = 0.f; // As built: clamped to the route
   V3 anchor { 0.f, 0.f, 0.f }; // The route point at the position
   std::vector<V3> centerline; // The tube's centreline, 3D (the overlay)
   std::vector<V2> section; // The same in the cross-section, x right and y up from the route
   int triangles = 0; // In the render mesh, blobs included
   // End fittings (the position is the attachment, the anchor its route point)
   std::string info; // freezy's note for a fitting that is built
   std::vector<V3> centerline2; // An elbow: the second rail's path (the first is 'centerline')
   int railA = -1, railB = -1;
   unsigned trimmedMask = 0; // Bit i: wire i is trimmed away at the attachment (the preview greys it)
};

namespace Detail
{
   inline constexpr float kPi = 3.14159265358979323846f;
   inline constexpr float kTwoPi = 6.28318530717958647692f;
   inline float Rad(const float d) { return d * kPi / 180.f; }
   inline float Sgn(const float v) { return v > 0.f ? 1.f : (v < 0.f ? -1.f : 0.f); }
   inline V3 NormSafe(const V3& v, const V3& fallback)
   {
      const float l2 = WireSweep::Dot(v, v);
      return l2 > 1e-30f ? v * (1.f / sqrtf(l2)) : fallback;
   }
   inline V2 Perp(const V2 v) { return { -v.y, v.x }; }
   inline V2 Rot(const V2 v, const float c, const float s) { return { v.x * c - v.y * s, v.x * s + v.y * c }; }
   inline void AddDistinct(std::vector<V3>& pts, const V3& p)
   {
      if (!pts.empty())
      {
         const V3 d = pts.back() - p;
         if (WireSweep::Dot(d, d) <= 1e-10f)
            return;
      }
      pts.push_back(p);
   }
   inline void AddDistinct(std::vector<V2>& pts, const V2 p)
   {
      if (!pts.empty())
      {
         const V2 d = pts.back() - p;
         if (Dot(d, d) <= 1e-10f)
            return;
      }
      pts.push_back(p);
   }

   inline V3 At(const Frame& f, const V2 o) { return f.p + f.r * o.x + f.u * o.y; }

   // The wires at the frame in its cross-section plane: offsets (x right, y up) and the bounds of the circles
   struct Section
   {
      std::vector<V2> offsets; // By wire index (all wires, off ones too)
      std::vector<bool> active;
      V2 lo { 1e30f, 1e30f }, hi { -1e30f, -1e30f };
      bool any = false;
   };
   inline Section MakeSection(const std::vector<Wire>& wires, const float wireR)
   {
      Section s;
      for (const Wire& w : wires)
      {
         s.offsets.push_back({ w.x, w.z });
         s.active.push_back(w.active);
         if (!w.active)
            continue;
         s.any = true;
         s.lo = { std::min(s.lo.x, w.x - wireR), std::min(s.lo.y, w.z - wireR) };
         s.hi = { std::max(s.hi.x, w.x + wireR), std::max(s.hi.y, w.z + wireR) };
      }
      return s;
   }

   // Ring: the visible arc of the circle (TryGetVisibleArc). False: nothing of it is left. 'closed': the whole ring
   inline bool RingArc(const Fixture& f, float& start, float& sweep, bool& closed)
   {
      start = 0.f;
      sweep = kTwoPi;
      closed = true;
      if (!f.cutout)
         return true;
      const float raw = f.cutoutEnd - f.cutoutStart;
      const float cut = fabsf(raw) >= 359.999f ? 360.f : fmodf(raw + 360.f, 360.f);
      if (cut <= 0.001f)
         return true;
      const float visible = 360.f - cut;
      if (visible <= 0.001f)
         return false;
      start = Rad(f.cutoutEnd);
      sweep = Rad(visible);
      closed = false;
      return true;
   }
   inline bool RingStraight(const Fixture& f, float& start, float& sweep)
   {
      start = Rad(f.straightStart);
      const float raw = f.straightEnd - f.straightStart;
      const float deg = fabsf(raw) >= 359.999f ? 360.f : fmodf(raw + 360.f, 360.f);
      sweep = Rad(deg);
      return f.straight && deg > 0.001f && deg < 359.999f;
   }
   // A point of the ring's centreline at an angle, the chord inside the straight section
   inline V2 RingOffset(const Fixture& f, const float angle, const float radius)
   {
      const V2 circular { cosf(angle) * radius, sinf(angle) * radius };
      float s, sw;
      if (!RingStraight(f, s, sw))
         return circular;
      const float progress = fmodf(angle - s + kPi * 4.f, kTwoPi);
      if (progress > sw)
         return circular;
      const V2 a { cosf(s) * radius, sinf(s) * radius };
      const float e = s + sw;
      const V2 b { cosf(e) * radius, sinf(e) * radius };
      const float k = progress / sw;
      return a + (b - a) * k;
   }
   inline std::vector<float> RingAngles(const Fixture& f, const float start, const float sweep, const bool closed, const int segments)
   {
      std::vector<float> angles;
      const int base = closed ? segments : segments + 1;
      for (int i = 0; i < base; i++)
         angles.push_back(start + sweep * (float)i / (float)segments);
      float ss, sw;
      if (RingStraight(f, ss, sw))
         for (const float boundary : { ss, ss + sw })
            for (int turn = -2; turn <= 2; turn++)
            {
               const float un = boundary + (float)turn * kTwoPi;
               const bool atEnd = fabsf(un - (start + sweep)) < 1e-5f;
               if (un >= start - 1e-5f && un <= start + sweep + 1e-5f && (!closed || !atEnd))
                  angles.push_back(un);
            }
      std::sort(angles.begin(), angles.end());
      for (int i = (int)angles.size() - 1; i > 0; i--)
         if (fabsf(angles[i] - angles[i - 1]) < 1e-5f)
            angles.erase(angles.begin() + i);
      return angles;
   }

   struct RingFit
   {
      V2 center { 0.f, 0.f };
      float baseRadius = 0.f, radius = 0.f;
   };
   // The circle round all the active wires: centre of their bounding box, the farthest wire edge plus the tube. False: no wire
   inline bool FitRing(const Fixture& f, const std::vector<Wire>& wires, const float wireR, RingFit& fit)
   {
      const Section s = MakeSection(wires, wireR);
      if (!s.any)
         return false;
      const V2 auto_ = (s.lo + s.hi) * 0.5f;
      float env = 0.f;
      for (size_t i = 0; i < wires.size(); i++)
         if (wires[i].active)
            env = std::max(env, Len(s.offsets[i] - auto_) + wireR);
      fit.center = auto_ + V2 { f.offsetX, f.offsetZ };
      fit.baseRadius = env + wireR;
      fit.radius = std::max(wireR, fit.baseRadius * f.scale);
      return true;
   }

   // The ring's centreline: the points of the tube (a whole ring does not repeat its first point)
   inline bool RingPoints(const Fixture& f, const Frame& fr, const std::vector<Wire>& wires, const float wireR, std::vector<V3>& pts, bool& closed, std::string& why)
   {
      RingFit fit;
      if (!FitRing(f, wires, wireR, fit))
      {
         why = "needs a wire on";
         return false;
      }
      float start, sweep;
      if (!RingArc(f, start, sweep, closed))
      {
         why = "the cutout leaves nothing of the ring";
         return false;
      }
      const int segments = std::max(2, (int)ceilf((float)f.density * sweep / kTwoPi));
      const std::vector<float> angles = RingAngles(f, start, sweep, closed, segments);
      pts.clear();
      for (const float a : angles)
         pts.push_back(At(fr, fit.center + RingOffset(f, a, fit.radius)));
      return true;
   }

   // A rung between wires 1 and 2 (rail indices 0 and 1) as freezy's TryEvaluateRungProfile: in the cross-section start and end offsets
   inline bool RungEnds(const Fixture& f, const std::vector<Wire>& wires, const float wireR, const float angleDeg, V2& start, V2& end, std::string& why)
   {
      if (wires.size() < 2 || !wires[0].active || !wires[1].active)
      {
         why = "needs wires 1 and 2 on";
         return false;
      }
      const Section s = MakeSection(wires, wireR);
      const V2 w1 = s.offsets[0], w2 = s.offsets[1];
      const V2 dir = Norm(w2 - w1, { 1.f, 0.f });
      const V2 aStart = w1 + dir * wireR, aEnd = w2 - dir * wireR;
      const V2 origin = (s.lo + s.hi) * 0.5f;
      const float a = Rad(angleDeg);
      const V2 d { cosf(a), sinf(a) };
      const V2 bottom = (aStart + aEnd) * 0.5f;
      const V2 rel = bottom - origin;
      const V2 rotated = origin + V2 { rel.x * d.x - rel.y * d.y, rel.x * d.y + rel.y * d.x };
      const V2 center = rotated + V2 { f.offsetX, f.offsetZ };
      const float length = std::max(0.1f, Len(aEnd - aStart) + f.length);
      start = center - d * (length * 0.5f);
      end = center + d * (length * 0.5f);
      return true;
   }

   // Cradle: the outer tangents of wires (1, 3) and (2, 4) meet at the tip
   inline bool OuterTangent(const V2 bottom, const V2 upper, const float bottomR, const float upperR, const V2 outward, V2& point, V2& direction)
   {
      const V2 delta = upper - bottom;
      const float dist = Len(delta);
      if (dist <= 1e-5f)
         return false;
      const V2 along = delta * (1.f / dist);
      const float normalAlong = (bottomR - upperR) / dist;
      if (fabsf(normalAlong) >= 1.f)
         return false;
      const float across = sqrtf(1.f - normalAlong * normalAlong);
      const V2 perp = Perp(along);
      const V2 n1 = along * normalAlong + perp * across;
      const V2 n2 = along * normalAlong - perp * across;
      const V2 normal = Dot(n1, outward) >= Dot(n2, outward) ? n1 : n2;
      point = bottom + normal * bottomR;
      direction = { -normal.y, normal.x };
      if (Dot(direction, delta) < 0.f)
         direction = direction * -1.f;
      return true;
   }
   inline bool DefaultCradleOrigin(const std::vector<V2>& offsets, const std::vector<bool>& active, const float wireR, const float tubeR, const float centerX, V2& origin)
   {
      if (offsets.size() < 4 || !active[0] || !active[1] || !active[2] || !active[3])
         return false;
      const float s0 = Sgn(offsets[0].x - centerX), s1 = Sgn(offsets[1].x - centerX), s2 = Sgn(offsets[2].x - centerX), s3 = Sgn(offsets[3].x - centerX);
      V2 lp, ld, rp, rd;
      if (s0 == 0.f || s1 == 0.f || s0 != s2 || s1 != s3 || s0 == s1 || !OuterTangent(offsets[0], offsets[2], wireR + tubeR, wireR + tubeR, { s0, 0.f }, lp, ld)
         || !OuterTangent(offsets[1], offsets[3], wireR + tubeR, wireR + tubeR, { s1, 0.f }, rp, rd))
         return false;
      const float den = Cross(ld, rd);
      if (fabsf(den) <= 1e-5f)
         return false;
      const float dist = Cross(rp - lp, rd) / den;
      origin = lp + ld * dist;
      return std::isfinite(origin.x) && std::isfinite(origin.y);
   }
   inline constexpr float kMaxCornerSpan = 0.45f;
   inline constexpr float kCornerStep = kPi / 12.f; // 15 degrees

   // The rounded corners of the cradle polyline; false: the radius that fits is under the tube radius
   inline bool RoundCradle(const std::vector<V2>& points, const float desiredRadius, const float minRadius, const int density, std::vector<V2>& rounded)
   {
      rounded.clear();
      AddDistinct(rounded, points.front());
      for (size_t i = 1; i + 1 < points.size(); i++)
      {
         const V2 prev = points[i - 1], corner = points[i], next = points[i + 1];
         const V2 in = corner - prev, out = next - corner;
         const float inLen = Len(in), outLen = Len(out);
         if (inLen <= 1e-5f || outLen <= 1e-5f)
         {
            AddDistinct(rounded, corner);
            continue;
         }
         const V2 inDir = in * (1.f / inLen), outDir = out * (1.f / outLen);
         const float dot = std::clamp(Dot(inDir, outDir), -1.f, 1.f);
         const float cross = inDir.x * outDir.y - inDir.y * outDir.x;
         const float turn = acosf(dot);
         if (turn <= Rad(0.5f) || fabsf(cross) <= 1e-6f)
         {
            AddDistinct(rounded, corner);
            continue;
         }
         const float scale = tanf(turn * 0.5f);
         const float tangent = std::min(std::max(0.05f, desiredRadius) * scale, std::min(inLen, outLen) * kMaxCornerSpan);
         const float radius = tangent / scale;
         if (radius + 1e-5f < minRadius)
            return false;
         const V2 start = corner - inDir * tangent, end = corner + outDir * tangent;
         const float sign = Sgn(cross);
         const V2 center = start + V2 { -inDir.y, inDir.x } * (radius * sign);
         const V2 sr = start - center;
         const int segs = std::max(2, (int)ceilf(std::max((float)density * turn / kTwoPi, turn / kCornerStep)));
         AddDistinct(rounded, start);
         for (int k = 1; k < segs; k++)
         {
            const float a = turn * sign * (float)k / (float)segs;
            AddDistinct(rounded, center + Rot(sr, cosf(a), sinf(a)));
         }
         AddDistinct(rounded, end);
      }
      AddDistinct(rounded, points.back());
      return true;
   }

   // The minimum span of an arm or the bottom for the corner to round at the tube (freezy's GetMinimumRoundedSpan)
   inline float MinRoundedSpan(const float tubeR, const float armAngle)
   {
      const float corner = Rad((180.f - armAngle) * 0.5f);
      return tubeR * tanf(corner * 0.5f) / kMaxCornerSpan;
   }

   inline bool CradlePoints(const Fixture& f, const Frame& fr, const std::vector<Wire>& wires, const float wireR, std::vector<V3>& pts, std::string& why)
   {
      const Section s = MakeSection(wires, wireR);
      if (!s.any)
      {
         why = "needs a wire on";
         return false;
      }
      const float tubeR = wireR;
      const float centerX = (s.lo.x + s.hi.x) * 0.5f;
      V2 fitted;
      const V2 tip = DefaultCradleOrigin(s.offsets, s.active, wireR, tubeR, centerX, fitted) && fitted.y < s.lo.y - 1e-5f ? fitted : V2 { centerX, s.lo.y - 30.f - tubeR * 2.f };
      // The anchor is frozen at the default fit: editing the bottom length or the arm angle reshapes the cradle in place
      const Fixture def;
      const float defHalf = Rad(def.armAngle * 0.5f);
      const float rise = def.bottomLength * 0.5f / tanf(defHalf);
      const V2 origin = tip + V2 { 0.f, rise } + V2 { f.offsetX, f.offsetZ };

      // Clamps at build time (freezy writes them back): arms and bottom no shorter than a rounded corner needs, the radius no smaller than the tube
      const float angle = std::clamp(f.armAngle, 1.f, 179.f);
      const float minSpan = MinRoundedSpan(tubeR, angle);
      float left = f.leftArm > 0.f ? std::max(f.leftArm, minSpan) : 0.f;
      float right = f.rightArm > 0.f ? std::max(f.rightArm, minSpan) : 0.f;
      float bottom = std::max(0.1f, f.bottomLength);
      if (left > 0.f || right > 0.f)
         bottom = std::max(bottom, minSpan);
      const float cornerRadius = std::max(tubeR, f.cornerRadius);

      const float half = Rad(angle * 0.5f);
      const V2 ld { -sinf(half), cosf(half) }, rd { sinf(half), cosf(half) };
      const V2 lb { -bottom * 0.5f, 0.f }, rb { bottom * 0.5f, 0.f };
      std::vector<V2> raw;
      if (left > 1e-5f)
         raw.push_back(lb + ld * left);
      raw.push_back(lb);
      raw.push_back(rb);
      if (right > 1e-5f)
         raw.push_back(rb + rd * right);
      const float rot = Rad(std::clamp(f.rotation, 0.f, 360.f));
      const float rc = cosf(rot), rs = sinf(rot);
      for (V2& p : raw)
         p = origin + V2 { p.x * rc - p.y * rs, p.x * rs + p.y * rc };
      std::vector<V2> rounded;
      if (!RoundCradle(raw, cornerRadius, tubeR, std::clamp(f.density, 3, 128), rounded) || rounded.size() < 2)
      {
         why = "the corner radius doesn't fit";
         return false;
      }
      pts.clear();
      for (const V2& o : rounded)
         pts.push_back(At(fr, o));
      return true;
   }

   // The U-shaped foot in its own xy plane
   inline std::vector<V3> UHookPoints(const float width, float armLength, float connectedLength, int bendSegments, const bool clockwise)
   {
      const float radius = std::max(0.05f, width * 0.5f);
      armLength = std::max(0.f, armLength);
      connectedLength = std::max(0.f, connectedLength);
      bendSegments = std::max(2, bendSegments);
      const float arcCenterY = -armLength * 0.5f + radius * 0.5f;
      const float openEndY = armLength * 0.5f + radius * 0.5f;
      std::vector<V3> pts { { -radius, arcCenterY + connectedLength, 0.f }, { -radius, arcCenterY, 0.f } };
      for (int i = 1; i <= bendSegments; i++)
      {
         const float a = kPi + kPi * (float)i / (float)bendSegments;
         pts.push_back({ cosf(a) * radius, arcCenterY + sinf(a) * radius, 0.f });
      }
      pts.push_back({ radius, openEndY, 0.f });
      if (clockwise)
         for (V3& p : pts)
            p.x = -p.x;
      return pts;
   }

   // Euler XYZ as Unity's quaternion.EulerXYZ: x first, then y, then z (R = Rz Ry Rx), the angles in degrees
   inline V3 RotateXYZ(const V3& deg, const V3& v)
   {
      const float cx = cosf(Rad(deg.x)), sx = sinf(Rad(deg.x)), cy = cosf(Rad(deg.y)), sy = sinf(Rad(deg.y)), cz = cosf(Rad(deg.z)), sz = sinf(Rad(deg.z));
      const V3 a { v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx }; // about x
      const V3 b { a.x * cy + a.z * sy, a.y, -a.x * sy + a.z * cy }; // about y
      return { b.x * cz - b.y * sz, b.x * sz + b.y * cz, b.z }; // about z
   }

   // The stand's leg path with its bends rounded at 'desiredRadius' (BuildRoundedLegPath). False: the path folds back at the attachment
   inline bool RoundLegPath(const std::vector<V3>& points, const int lastCorner, const float desiredRadius, std::vector<V3>& rounded)
   {
      using WireSweep::Cross;
      using WireSweep::Dot;
      using WireSweep::Len;
      rounded.clear();
      AddDistinct(rounded, points[0]);
      const int finalRounded = std::min(lastCorner, (int)points.size() - 2);
      for (int i = 1; i + 1 < (int)points.size(); i++)
      {
         const V3 prev = points[i - 1], corner = points[i], next = points[i + 1];
         if (i == 1)
         {
            const V3 a = WireSweep::Norm(corner - prev), b = WireSweep::Norm(next - corner);
            if (Dot(a, a) > 1e-8f && Dot(b, b) > 1e-8f && Dot(a, b) <= -cosf(Rad(5.f)))
               return false;
         }
         bool done = false;
         if (i <= finalRounded)
         {
            const V3 in = corner - prev, out = next - corner;
            const float inLen = Len(in), outLen = Len(out);
            if (inLen > 1e-5f && outLen > 1e-5f)
            {
               const V3 inDir = in * (1.f / inLen), outDir = out * (1.f / outLen);
               const float turn = acosf(std::clamp(Dot(inDir, outDir), -1.f, 1.f));
               if (turn > Rad(0.5f) && turn < kPi - Rad(0.5f))
               {
                  const float scale = tanf(turn * 0.5f);
                  const float tangent = std::min(std::max(0.05f, desiredRadius) * scale, std::min(inLen, outLen) * 0.45f);
                  const V3 axis = WireSweep::Norm(Cross(inDir, outDir));
                  if (tangent > 1e-5f && scale > 1e-5f && Dot(axis, axis) > 1e-8f)
                  {
                     const float radius = tangent / scale;
                     const V3 start = corner - inDir * tangent, end = corner + outDir * tangent;
                     const V3 center = start + Cross(axis, inDir) * radius;
                     const V3 sr = start - center;
                     const int segs = std::max(2, (int)ceilf(turn / kCornerStep));
                     AddDistinct(rounded, start);
                     for (int k = 1; k < segs; k++)
                     {
                        const float a = turn * (float)k / (float)segs, c = cosf(a), s = sinf(a);
                        // Rodrigues: sr turned about the axis
                        AddDistinct(rounded, center + sr * c + Cross(axis, sr) * s + axis * (Dot(axis, sr) * (1.f - c)));
                     }
                     AddDistinct(rounded, end);
                     done = true;
                  }
               }
            }
         }
         if (!done)
            AddDistinct(rounded, corner);
      }
      AddDistinct(rounded, points.back());
      return true;
   }

   // The stand: an attachment rung between wires 1 and 2 at angle 0, a leg from one end of it down to a U-shaped foot. 'attach': the attachment's two ends
   inline bool StandPoints(const Fixture& f, const Frame& fr, const std::vector<Wire>& wires, const float wireR, std::vector<V3>& pts, std::vector<V3>& attach, std::string& why)
   {
      V2 s2, e2;
      if (!RungEnds(f, wires, wireR, 0.f, s2, e2, why))
         return false;
      const V3 aStart = At(fr, s2), aEnd = At(fr, e2);
      attach = { aStart, aEnd };
      const V3 legStart = f.legLeft ? aStart : aEnd;
      const V3 sv = f.startVector;
      const V3 dir = NormSafe(fr.r * sv.x + fr.t * sv.y + fr.u * sv.z, fr.u * -1.f);
      const V3 elbow = legStart + dir * f.startLength;
      const std::vector<V3> local = UHookPoints(f.footWidth, f.footArm, f.footConnectedArm, 12, f.footClockwise);
      std::vector<V3> foot;
      for (const V3& lp : local)
      {
         const V3 l = f.footPosition + RotateXYZ(f.footRotation, lp);
         foot.push_back(legStart + fr.r * l.x + fr.t * l.y + fr.u * l.z);
      }
      std::vector<V3> leg { legStart };
      AddDistinct(leg, elbow);
      AddDistinct(leg, foot[0]);
      std::vector<V3> combined { f.legLeft ? aEnd : aStart };
      for (const V3& p : leg)
         AddDistinct(combined, p);
      const int lastCorner = (int)combined.size() - 1;
      for (size_t i = 1; i < foot.size(); i++)
         AddDistinct(combined, foot[i]);
      if (combined.size() < 2)
      {
         why = "has no path";
         return false;
      }
      if (!RoundLegPath(combined, lastCorner, wireR * 2.f, pts))
      {
         why = "the leg folds back through its attachment";
         return false;
      }
      return true;
   }
}

// A whole ring: a closed tube of rings of vertices, no caps, the side kept by parallel transport with the leftover twist spread round the loop. The centreline is planar for a ring so there is none to speak of
template <class Vertex>
bool AppendClosedTube(const std::vector<V3>& centerline, const float radius, int sides, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
{
   using WireSweep::Cross;
   using WireSweep::Dot;
   using WireSweep::Norm;
   sides = std::clamp(sides, 3, 64);
   std::vector<V3> p;
   for (const V3& c : centerline)
      if (p.empty() || WireSweep::Len(c - p.back()) > 1e-4f)
         p.push_back(c);
   if (p.size() > 1 && WireSweep::Len(p.back() - p.front()) <= 1e-4f)
      p.pop_back();
   const int n = (int)p.size();
   if (n < 3)
      return false;
   std::vector<float> s(n + 1, 0.f);
   for (int i = 1; i <= n; i++)
      s[i] = s[i - 1] + WireSweep::Len(p[i % n] - p[i - 1]);
   const float total = s[n] > 0.f ? s[n] : 1.f;
   std::vector<V3> t(n);
   for (int i = 0; i < n; i++)
      t[i] = Norm(p[(i + 1) % n] - p[(i + n - 1) % n]);
   const auto start = [&](const V3& tan) { return Norm(Cross(tan, fabsf(tan.z) > 0.999f ? V3 { 1.f, 0.f, 0.f } : V3 { 0.f, 0.f, 1.f })); };
   std::vector<V3> nrm(n + 1);
   nrm[0] = start(t[0]);
   for (int i = 1; i <= n; i++)
   {
      const V3& tan = t[i % n];
      V3 v = Norm(nrm[i - 1] - tan * Dot(nrm[i - 1], tan));
      if (WireSweep::Len(v) < 0.5f)
         v = start(tan);
      nrm[i] = v;
   }
   // The transported side back at the start against the side it started with
   const float twist = atan2f(Dot(Cross(nrm[0], nrm[n]), t[0]), Dot(nrm[0], nrm[n]));
   const unsigned base = (unsigned)verts.size();
   for (int i = 0; i < n; i++)
   {
      const float a = -twist * (float)i / (float)n;
      const V3 nn = Norm(nrm[i] * cosf(a) + Cross(t[i], nrm[i]) * sinf(a));
      const V3 bin = Norm(Cross(t[i], nn));
      for (int j = 0; j < sides; j++)
      {
         const float ang = Detail::kTwoPi * (float)j / (float)sides;
         const V3 radial = nn * cosf(ang) + bin * sinf(ang);
         const V3 pos = p[i] + radial * radius;
         Vertex v;
         v.x = pos.x;
         v.y = pos.y;
         v.z = pos.z;
         v.nx = radial.x;
         v.ny = radial.y;
         v.nz = radial.z;
         v.tu = s[i] / total;
         v.tv = (float)j / (float)sides;
         verts.push_back(v);
      }
   }
   for (int i = 0; i < n; i++)
      for (int j = 0; j < sides; j++)
      {
         const unsigned q0 = base + i * sides + j, q1 = base + i * sides + ((j + 1) % sides);
         const unsigned q2 = base + ((i + 1) % n) * sides + j, q3 = base + ((i + 1) % n) * sides + ((j + 1) % sides);
         idx.insert(idx.end(), { q0, q1, q2, q3, q2, q1 });
      }
   return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Solder (WireRailSolderMeshGenerator): a blob where a fixture's wire comes within the Solder Threshold of a rail. The random numbers are freezy's, so the same input gives
// the same bytes

namespace Detail
{
   inline uint32_t Bits(const float v)
   {
      uint32_t u;
      memcpy(&u, &v, sizeof u);
      return u;
   }
   inline uint32_t Mix(uint32_t seed, const uint32_t value)
   {
      seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
      seed ^= seed >> 16;
      seed *= 0x7feb352du;
      seed ^= seed >> 15;
      return seed;
   }
   inline float RandomRange(uint32_t& state, const float lo, const float hi)
   {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      const float unit = (float)(state & 0x00ffffffu) / 16777215.f;
      return lo + unit * (hi - lo);
   }

   struct Touch
   {
      V3 firstPoint, secondPoint, firstTangent, secondTangent;
      float firstRadius = 0.f, secondRadius = 0.f, surfaceDistance = 0.f;
      V3 Position() const { return (firstPoint + secondPoint) * 0.5f; }
   };

   inline void ClosestPointsBetweenSegments(const V3& firstStart, const V3& firstEnd, const V3& secondStart, const V3& secondEnd, V3& firstPoint, V3& secondPoint)
   {
      using WireSweep::Dot;
      const V3 fd = firstEnd - firstStart, sd = secondEnd - secondStart, delta = firstStart - secondStart;
      const float fl2 = Dot(fd, fd), sl2 = Dot(sd, sd);
      const float sProj = Dot(sd, delta);
      float ft, st;
      if (fl2 <= 1e-10f && sl2 <= 1e-10f)
      {
         firstPoint = firstStart;
         secondPoint = secondStart;
         return;
      }
      if (fl2 <= 1e-10f)
      {
         ft = 0.f;
         st = std::clamp(sProj / sl2, 0.f, 1.f);
      }
      else
      {
         const float fProj = Dot(fd, delta);
         if (sl2 <= 1e-10f)
         {
            st = 0.f;
            ft = std::clamp(-fProj / fl2, 0.f, 1.f);
         }
         else
         {
            const float dd = Dot(fd, sd);
            const float den = fl2 * sl2 - dd * dd;
            ft = den > 1e-10f ? std::clamp((dd * sProj - fProj * sl2) / den, 0.f, 1.f) : 0.f;
            st = (dd * ft + sProj) / sl2;
            if (st < 0.f)
            {
               st = 0.f;
               ft = std::clamp(-fProj / fl2, 0.f, 1.f);
            }
            else if (st > 1.f)
            {
               st = 1.f;
               ft = std::clamp((dd - fProj) / fl2, 0.f, 1.f);
            }
         }
      }
      firstPoint = firstStart + fd * ft;
      secondPoint = secondStart + sd * st;
   }

   // A rail's straight piece against the fixture's polyline: the closest of its segments, if its surface gap is within the threshold
   inline bool FindTouch(const V3& railStart, const V3& railEnd, const float railR, const std::vector<V3>& path, const float pathR, const float threshold, Touch& touch)
   {
      bool found = false;
      for (size_t i = 0; i + 1 < path.size(); i++)
      {
         Touch c;
         ClosestPointsBetweenSegments(railStart, railEnd, path[i], path[i + 1], c.firstPoint, c.secondPoint);
         c.surfaceDistance = WireSweep::Len(c.firstPoint - c.secondPoint) - std::max(0.f, railR) - std::max(0.f, pathR);
         if (c.surfaceDistance > std::max(0.f, threshold) + 1e-4f || (found && c.surfaceDistance >= touch.surfaceDistance))
            continue;
         c.firstTangent = NormSafe(railEnd - railStart, { 0.f, 1.f, 0.f });
         c.secondTangent = NormSafe(path[i + 1] - path[i], { 1.f, 0.f, 0.f });
         c.firstRadius = std::max(0.f, railR);
         c.secondRadius = std::max(0.f, pathR);
         touch = c;
         found = true;
      }
      return found;
   }

   inline constexpr float kSolderBaseSize = 0.8f;
   inline constexpr int kBlobRadial = 6, kBlobRings = 3, kBlobVerts = kBlobRadial * kBlobRings + 2;

   // One blob: 6 around x 3 rings and two poles, 36 triangles with vertices of their own, normals of the ellipsoid
   template <class Vertex>
   void AppendBlob(const Touch& touch, const float solderSize, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
   {
      using WireSweep::Cross;
      using WireSweep::Dot;
      const V3 center = touch.Position();
      const V3 firstAxis = NormSafe(touch.firstTangent, { 0.f, 1.f, 0.f });
      V3 secondAxis = touch.secondTangent - firstAxis * Dot(touch.secondTangent, firstAxis);
      if (Dot(secondAxis, secondAxis) <= 1e-8f)
      {
         const V3 sep = touch.secondPoint - touch.firstPoint;
         secondAxis = sep - firstAxis * Dot(sep, firstAxis);
      }
      if (Dot(secondAxis, secondAxis) <= 1e-8f)
      {
         const V3 fallback = fabsf(firstAxis.z) < 0.9f ? V3 { 0.f, 0.f, 1.f } : V3 { 1.f, 0.f, 0.f };
         secondAxis = fallback - firstAxis * Dot(fallback, firstAxis);
      }
      secondAxis = WireSweep::Norm(secondAxis);
      V3 normalAxis = NormSafe(Cross(firstAxis, secondAxis), { 1.f, 0.f, 0.f });
      const V3 separationAxis = touch.secondPoint - touch.firstPoint;
      if (Dot(normalAxis, separationAxis) < 0.f)
         normalAxis = normalAxis * -1.f;

      uint32_t seed = 0x811c9dc5u;
      seed = Mix(seed, Bits(center.x));
      seed = Mix(seed, Bits(center.y));
      seed = Mix(seed, Bits(center.z));
      seed = Mix(seed, Bits(touch.firstRadius));
      seed = Mix(seed, Bits(touch.secondRadius));
      uint32_t state = seed == 0 ? 0x9e3779b9u : seed;
      float firstExtent = (touch.firstRadius * 1.25f + touch.secondRadius * 0.35f) * RandomRange(state, 0.92f, 1.08f);
      float secondExtent = (touch.secondRadius * 1.25f + touch.firstRadius * 0.35f) * RandomRange(state, 0.92f, 1.08f);
      const float centerDistance = WireSweep::Len(touch.firstPoint - touch.secondPoint);
      float normalExtent = std::max(std::max(touch.firstRadius, touch.secondRadius) * 0.75f, centerDistance * 0.5f + std::min(touch.firstRadius, touch.secondRadius) * 0.25f)
         * RandomRange(state, 0.92f, 1.08f);
      const float size = std::max(0.01f, solderSize) * kSolderBaseSize;
      firstExtent *= size;
      secondExtent *= size;
      normalExtent *= size;

      V3 positions[kBlobVerts];
      V2 uvs[kBlobVerts];
      positions[0] = center - normalAxis * (normalExtent * RandomRange(state, 0.9f, 1.02f));
      uvs[0] = { 0.5f, 0.f };
      for (int ring = 0; ring < kBlobRings; ring++)
      {
         const float t = (float)ring / (float)(kBlobRings - 1);
         const float latitude = -kPi * 0.25f + t * (kPi * 0.5f);
         const float latitudeRadius = cosf(latitude), latitudeHeight = sinf(latitude);
         const float angleOffset = RandomRange(state, -0.14f, 0.14f);
         for (int radial = 0; radial < kBlobRadial; radial++)
         {
            const float angle = kTwoPi * (float)radial / (float)kBlobRadial + angleOffset + RandomRange(state, -0.08f, 0.08f);
            const float radialVariation = RandomRange(state, 0.84f, 1.16f);
            const float normalVariation = RandomRange(state, 0.9f, 1.1f);
            const V3 r = firstAxis * (cosf(angle) * firstExtent) + secondAxis * (sinf(angle) * secondExtent);
            const int vi = 1 + ring * kBlobRadial + radial;
            positions[vi] = center + r * latitudeRadius * radialVariation + normalAxis * normalExtent * latitudeHeight * normalVariation;
            uvs[vi] = { (float)radial / (float)kBlobRadial, ((float)ring + 1.f) / ((float)kBlobRings + 1.f) };
         }
      }
      positions[kBlobVerts - 1] = center + normalAxis * (normalExtent * RandomRange(state, 0.9f, 1.02f));
      uvs[kBlobVerts - 1] = { 0.5f, 1.f };

      const float fe2 = std::max(firstExtent * firstExtent, 1e-8f), se2 = std::max(secondExtent * secondExtent, 1e-8f), ne2 = std::max(normalExtent * normalExtent, 1e-8f);
      const auto smoothNormal = [&](const V3& position)
      {
         const V3 offset = position - center;
         const V3 e = firstAxis * (Dot(offset, firstAxis) / fe2) + secondAxis * (Dot(offset, secondAxis) / se2) + normalAxis * (Dot(offset, normalAxis) / ne2);
         return NormSafe(e, NormSafe(offset, normalAxis));
      };
      const auto triangle = [&](int a, int b, int c)
      {
         V3 first = positions[a], second = positions[b], third = positions[c];
         const V3 face = Cross(second - first, third - first);
         if (Dot(face, (first + second + third) * (1.f / 3.f) - center) < 0.f)
         {
            std::swap(second, third);
            std::swap(b, c);
         }
         const unsigned at = (unsigned)verts.size();
         const V3 pp[3] = { first, second, third };
         const int ii[3] = { a, b, c };
         for (int k = 0; k < 3; k++)
         {
            const V3 nn = smoothNormal(pp[k]);
            Vertex v;
            v.x = pp[k].x;
            v.y = pp[k].y;
            v.z = pp[k].z;
            v.nx = nn.x;
            v.ny = nn.y;
            v.nz = nn.z;
            v.tu = uvs[ii[k]].x;
            v.tv = uvs[ii[k]].y;
            verts.push_back(v);
         }
         idx.insert(idx.end(), { at, at + 1, at + 2 });
      };
      for (int radial = 0; radial < kBlobRadial; radial++)
      {
         const int next = (radial + 1) % kBlobRadial;
         triangle(0, 1 + next, 1 + radial);
         for (int ring = 0; ring < kBlobRings - 1; ring++)
         {
            const int first = 1 + ring * kBlobRadial, second = first + kBlobRadial;
            triangle(first + radial, first + next, second + radial);
            triangle(first + next, second + next, second + radial);
         }
         const int last = 1 + (kBlobRings - 1) * kBlobRadial;
         triangle(kBlobVerts - 1, last + radial, last + next);
      }
   }
}

inline constexpr int kTrianglesPerBlob = Detail::kBlobRadial * Detail::kBlobRings * 2;

///////////////////////////////////////////////////////////////////////////////////////////////////
// End fittings (phase 2b): freezy's Hairpin, Elbow and Rail Trim (WireRailEndpointTrimUtility, TryEvaluateHairpinProfile / ElbowProfile, AppendHairpin / AppendElbow, the
// collider of WireRailColliderGenerator). They sit at the start or the end of the route and change the ball channel: they trim rails, and the hairpin and the elbow add
// colliders of their own. A rail is trimmed by distances inward from an endpoint, the largest over all the fittings at that end.

// Distances inward from the route's start and end, per wire
struct Trims
{
   float start[kMaxWires] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
   float end[kMaxWires] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
   bool Any() const
   {
      for (int i = 0; i < kMaxWires; i++)
         if (start[i] > 1e-5f || end[i] > 1e-5f)
            return true;
      return false;
   }
};
// The rails that end in a hairpin or an elbow: no cap there, the fitting's tube continues from the ring
struct FittedEnds
{
   bool start[kMaxWires] = { false, false, false, false, false, false };
   bool end[kMaxWires] = { false, false, false, false, false, false };
   bool Any() const
   {
      for (int i = 0; i < kMaxWires; i++)
         if (start[i] || end[i])
            return true;
      return false;
   }
};

namespace Detail
{
   inline constexpr float kMinSpan = 1e-5f;
   inline constexpr int kElbowBendSegments = 6; // max(2, ceil((pi / 2) / (pi / 12)))
   inline constexpr int kHairpinColliderLeadSegments = 4, kHairpinColliderArcSegments = 12;

   inline bool Attached(const Fixture& f, const int rail) { return rail == f.railA || rail == f.railB; }

   // A fitting as freezy's Ensure*Initialized would store it, built from this copy; the stored values are left alone (an undo of a clamp would need a record of its own)
   inline Fixture EffectiveFitting(const Fixture& in, const int railCount, const float total)
   {
      Fixture f = in;
      f.endpoint = f.endpoint == kEndStart ? kEndStart : kEndEnd;
      const float length = std::max(0.f, total);
      const int top = std::max(0, railCount - 1); // Both fittings clamp Rail A and B to Rails - 1 (a hairpin was left to fail, S1)
      f.railA = std::clamp(f.railA, 0, top);
      f.railB = std::clamp(f.railB, 0, top);
      f.fitOffset = std::clamp(f.fitOffset, 0.f, length);
      for (float& c : f.cutoff)
         c = std::clamp(c, 0.f, length);
      if (f.type == kFixtureElbow)
         for (int i = 0; i < kMaxWires; i++)
            if (Attached(f, i))
               f.cutoff[i] = 0.f; // The attached rails continue into the elbow: fixed at 0
      return f;
   }

   // Where a hairpin or an elbow attaches: the offset back from its endpoint
   inline float AttachDistance(const Fixture& f, const float total)
   {
      const float off = std::max(0.f, f.fitOffset);
      return std::clamp(f.endpoint == kEndStart ? off : total - off, 0.f, std::max(0.f, total));
   }

   // IsHairpinGeneratable / IsElbowGeneratable: two different rails, the offset leaves some route, both rails on at the attachment
   inline bool Generatable(const Fixture& f, const std::vector<Wire>& wires, const float total)
   {
      if (f.railA == f.railB || f.railA < 0 || f.railB < 0)
         return false;
      if (std::max(0.f, f.fitOffset) >= total - kMinSpan) // The offset uses up the route: clamping it would put the fitting at the other endpoint
         return false;
      const std::vector<Wire>& ws = WiresAt(wires, AttachDistance(f, total));
      return f.railA < (int)ws.size() && f.railB < (int)ws.size() && ws[f.railA].active && ws[f.railB].active;
   }

   // HasRailTrimConflict: another fitting already shortens one of the two rails at this endpoint. 'self' is the fitting asking (-1: none), skipped. Only the fittings
   // with use[j] count
   inline bool HasTrimConflict(const std::vector<Fixture>& eff, const std::vector<unsigned char>& use, const std::vector<Wire>& wires, const float total, const int endpoint,
      const int a, const int b, const int self)
   {
      if (a < 0 || b < 0)
         return false;
      const int n = (int)wires.size();
      for (int j = 0; j < (int)eff.size(); j++)
      {
         if (!use[j] || j == self)
            continue;
         const Fixture& o = eff[j];
         if (o.type == kFixtureElbow && !Generatable(o, wires, total))
            continue; // An elbow that is not built contributes no trims and so no conflicts
         if (o.type == kFixtureTrim && o.endpoint == endpoint)
         {
            if ((a < n && o.cutoff[a] > kMinSpan) || (b < n && o.cutoff[b] > kMinSpan))
               return true;
         }
         else if (o.type == kFixtureElbow && o.endpoint == endpoint)
         {
            if (self >= 0 && o.fitOffset > kMinSpan && o.railA != o.railB && (Attached(o, a) || Attached(o, b)))
               return true; // The other elbow's Offset shortens its two rails
            if ((!Attached(o, a) && a < n && o.cutoff[a] > kMinSpan) || (!Attached(o, b) && b < n && o.cutoff[b] > kMinSpan))
               return true;
         }
      }
      return false;
   }

   inline V3 RotateAbout(const V3& axis, const float angle, const V3& v)
   {
      const float c = cosf(angle), s = sinf(angle);
      return v * c + WireSweep::Cross(axis, v) * s + axis * (WireSweep::Dot(axis, v) * (1.f - c));
   }
   inline float DistSq(const V3& a, const V3& b)
   {
      const V3 d = a - b;
      return WireSweep::Dot(d, d);
   }
}

// What the fittings of a rail do, worked out once per rebuild: the fittings as built (clamped copies), which ones are built, why not, and the trims and fitted ends they cause.
// 'onlyEnabled' = the render (a fitting that is switched off leaves the render mesh: no trim, no cap change, no tube; freezy builds the render mesh from the enabled ones);
// off, the collider and the panel (everything counts)
struct FittingPlan
{
   std::vector<Fixture> eff; // By fixture index
   std::vector<unsigned char> use; // Counts in this plan
   std::vector<unsigned char> ok; // A hairpin or elbow that is built
   std::vector<std::string> why; // For one that is not
   Trims trims; // With the elbows' own cutoffs (the render); ColliderTrims leaves them out
   FittedEnds fitted;
};

inline FittingPlan PlanFittings(const std::vector<Fixture>& fixtures, const std::vector<Wire>& wires, const float total, const bool onlyEnabled)
{
   using namespace Detail;
   FittingPlan p;
   const int count = (int)fixtures.size();
   const int n = (int)wires.size();
   p.eff.resize(count);
   p.use.assign(count, 0);
   p.ok.assign(count, 0);
   p.why.assign(count, std::string());
   bool anyFitting = false;
   for (int j = 0; j < count; j++)
   {
      p.eff[j] = IsEndFitting(fixtures[j].type) ? EffectiveFitting(fixtures[j], n, total) : fixtures[j];
      p.use[j] = IsEndFitting(fixtures[j].type) && (!onlyEnabled || fixtures[j].enabled);
      anyFitting |= p.use[j] != 0;
   }
   if (!anyFitting)
      return p;
   for (int j = 0; j < count; j++)
   {
      if (!p.use[j])
         continue;
      const Fixture& f = p.eff[j];
      if (f.type == kFixtureTrim)
      {
         p.ok[j] = 1;
         continue;
      }
      const char* name = f.type == kFixtureHairpin ? "Hairpin" : "Elbow";
      std::string& why = p.why[j];
      if (total <= kMinSpan)
         why = "The route is too short.";
      else if (f.railA == f.railB)
         why = std::string("Select two different rails. Invalid ") + name + "s are not generated.";
      else if (f.fitOffset >= total - kMinSpan)
         why = f.type == kFixtureHairpin ? "The offset consumes the whole route. Move it back so the loop has rail to attach to; the Hairpin is not generated."
                                         : "The offset consumes the whole route. Move it back so the elbow has rail to attach to; the Elbow is not generated.";
      else if (!Generatable(f, wires, total))
         why = std::string("Both selected rails must be active at this endpoint. Inactive ") + name + "s are not generated.";
      else if (HasTrimConflict(p.eff, p.use, wires, total, f.endpoint, f.railA, f.railB, j))
         why = f.type == kFixtureHairpin ? "A Rail Trim shortens an attached rail at this endpoint. Remove the conflict or select different rails; the Hairpin is not generated."
                                         : "Another endpoint cutoff shortens an attached rail. Remove the conflict or select different rails; the Elbow is not generated.";
      p.ok[j] = why.empty();
   }
   // WireRailEndpointTrimUtility.Collect and CollectFittedRailEnds
   for (int j = 0; j < count; j++)
   {
      if (!p.use[j])
         continue;
      const Fixture& f = p.eff[j];
      float* dest = f.endpoint == kEndStart ? p.trims.start : p.trims.end;
      if (f.type == kFixtureTrim)
      {
         for (int i = 0; i < n && i < kMaxWires; i++)
            dest[i] = std::max(dest[i], f.cutoff[i]);
      }
      else if (f.type == kFixtureElbow && p.ok[j])
      {
         for (int i = 0; i < n && i < kMaxWires; i++)
            dest[i] = std::max(dest[i], Attached(f, i) ? std::max(0.f, f.fitOffset) : f.cutoff[i]);
      }
      else if (f.type == kFixtureHairpin && p.ok[j] && f.fitOffset > kMinSpan)
      {
         dest[f.railA] = std::max(dest[f.railA], f.fitOffset);
         dest[f.railB] = std::max(dest[f.railB], f.fitOffset);
      }
      if ((f.type == kFixtureHairpin || f.type == kFixtureElbow) && p.ok[j])
      {
         bool* fit = f.endpoint == kEndStart ? p.fitted.start : p.fitted.end;
         fit[f.railA] = true;
         fit[f.railB] = true;
      }
   }
   return p;
}

// The trims the collider uses: the same, except that an elbow contributes only its Offset on its two rails, never its Other Rail Cutoffs (includeElbow false, then
// ShortenElbowAttachedRailColliders)
inline Trims ColliderTrims(const std::vector<Fixture>& fixtures, const std::vector<Wire>& wires, const FittingPlan& all)
{
   using namespace Detail;
   Trims t;
   const int n = (int)wires.size();
   for (int j = 0; j < (int)fixtures.size(); j++)
   {
      if (!all.use[j])
         continue;
      const Fixture& f = all.eff[j];
      float* dest = f.endpoint == kEndStart ? t.start : t.end;
      if (f.type == kFixtureTrim)
         for (int i = 0; i < n && i < kMaxWires; i++)
            dest[i] = std::max(dest[i], f.cutoff[i]);
      else if ((f.type == kFixtureHairpin || f.type == kFixtureElbow) && all.ok[j] && f.fitOffset > kMinSpan)
      {
         dest[f.railA] = std::max(dest[f.railA], f.fitOffset);
         dest[f.railB] = std::max(dest[f.railB], f.fitOffset);
      }
   }
   return t;
}

// The tubes with the trims and the fitted ends (render). A wire's centreline is the frame at its start cut, the rows strictly between, and the frame at its end cut; a wire
// that is not trimmed uses the rows as they are, so a rail without fittings is built byte for byte as BuildTubes does. 'rings' gets, per wire, the first and last ring of its
// tube and their side direction: where a fitting's mouth meets it
struct TubeRings
{
   int first[kMaxWires] = { -1, -1, -1, -1, -1, -1 };
   int last[kMaxWires] = { -1, -1, -1, -1, -1, -1 };
   V3 firstSide[kMaxWires] = {};
   V3 lastSide[kMaxWires] = {};
};

// The centreline of a wire with its start and end cut by the distances inward (the frame at each cut, the rows strictly between); the rows as they are when it is not cut.
// Empty: nothing of the wire is left
inline std::vector<V3> TrimmedWireCenterline(const std::vector<Frame>& frames, const std::vector<float>& rowDist, const Wire& w, const float trimStart, const float trimEnd)
{
   const float total = rowDist.empty() ? 0.f : rowDist.back();
   if ((trimStart <= 1e-5f && trimEnd <= 1e-5f) || rowDist.size() != frames.size())
      return WireCenterline(frames, w);
   const float a = std::clamp(trimStart, 0.f, total), b = total - std::clamp(trimEnd, 0.f, total);
   if (b - a <= 1e-4f)
      return {};
   const auto at = [&](const Frame& f) { return f.p + f.r * w.x + f.u * w.z; };
   std::vector<V3> line;
   line.push_back(at(a <= 1e-5f ? frames.front() : FrameAt(frames, rowDist, a)));
   for (size_t i = 0; i < frames.size(); i++)
      if (rowDist[i] > a + 1e-5f && rowDist[i] < b - 1e-5f)
         line.push_back(at(frames[i]));
   line.push_back(at(b >= total - 1e-5f ? frames.back() : FrameAt(frames, rowDist, b)));
   return line;
}

template <class Vertex>
bool BuildTubesTrimmed(const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float radius, const int sides, const float bevel,
   const Trims& trims, const FittedEnds& fitted, TubeRings* rings, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
{
   bool any = false;
   for (size_t w = 0; w < wires.size() && w < (size_t)kMaxWires; w++)
   {
      if (!wires[w].active)
         continue;
      const std::vector<V3> line = TrimmedWireCenterline(frames, rowDist, wires[w], trims.start[w], trims.end[w]);
      if (line.size() < 2)
         continue; // Nothing of this wire is left
      TubeOpts opts;
      opts.capStart = !fitted.start[w];
      opts.capEnd = !fitted.end[w];
      TubeInfo info;
      if (AppendTube<Vertex>(line, radius, sides, bevel, verts, idx, &opts, &info))
      {
         any = true;
         if (rings != nullptr)
         {
            rings->first[w] = info.firstRing;
            rings->last[w] = info.lastRing;
            rings->firstSide[w] = info.firstSide;
            rings->lastSide[w] = info.lastSide;
         }
      }
   }
   return any;
}

// The path of a hairpin: a Bezier lead from each rail to its end of a half circle (the loop), as TryEvaluateHairpinProfile
struct HairpinPath
{
   Frame frame {};
   V3 outward {};
   std::vector<V3> pts; // Lead A, the arc, lead B back
   int terminalStartSpan = 0, terminalEndSpan = 0; // The spans of 'pts' that are the arc
};

namespace Detail
{
   inline std::vector<V3> BuildLead(const V3& attachment, const V3& arcPoint, const V3& outward, const float tangentLength, const int segments)
   {
      const float maxHandle = WireSweep::Len(attachment - arcPoint) * 0.49f;
      const float handle = std::min(std::max(0.f, tangentLength), maxHandle);
      const V3 c1 = attachment + outward * handle, c2 = arcPoint - outward * handle;
      std::vector<V3> pts((size_t)segments + 1);
      for (int i = 0; i <= segments; i++)
      {
         const float t = (float)i / (float)segments, inv = 1.f - t;
         pts[i] = attachment * (inv * inv * inv) + c1 * (3.f * inv * inv * t) + c2 * (3.f * inv * t * t) + arcPoint * (t * t * t);
      }
      return pts;
   }
}

inline bool HairpinPoints(const Fixture& f, const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float total, const int leadSegments,
   const int arcSegments, HairpinPath& out)
{
   using namespace Detail;
   out = HairpinPath {};
   if (frames.size() < 2 || !Generatable(f, wires, total))
      return false;
   const float d = AttachDistance(f, total);
   const Frame fr = FrameAt(frames, rowDist, d);
   const std::vector<Wire>& ws = WiresAt(wires, d);
   const auto railAt = [&](const int i) { return fr.p + fr.r * ws[i].x + fr.u * ws[i].z; };
   const V3 a = railAt(f.railA), b = railAt(f.railB);
   const V3 outward = f.endpoint == kEndStart ? fr.t * -1.f : fr.t;
   V3 pairAxis = NormSafe(b - a, fr.r);
   pairAxis = NormSafe(RotateAbout(fr.t, Rad(f.rotation), pairAxis), fr.r);
   const V3 center = (a + b) * 0.5f + outward * f.leadLength;
   const float radius = f.loopDiameter * 0.5f;
   V3 arcA = center - pairAxis * radius, arcB = center + pairAxis * radius;
   if (DistSq(a, arcA) > DistSq(a, arcB))
   {
      pairAxis = pairAxis * -1.f;
      std::swap(arcA, arcB);
   }
   const std::vector<V3> leadA = BuildLead(a, arcA, outward, f.tangentLength, leadSegments);
   std::vector<V3> leadB = BuildLead(b, arcB, outward, f.tangentLength, leadSegments);
   std::reverse(leadB.begin(), leadB.end());
   std::vector<V3> terminal((size_t)arcSegments + 1);
   for (int i = 0; i <= arcSegments; i++)
   {
      const float angle = kPi * (float)i / (float)arcSegments;
      terminal[i] = center - pairAxis * (cosf(angle) * radius) + outward * (sinf(angle) * radius);
   }
   std::vector<V3>& pts = out.pts;
   for (const V3& p : leadA)
      AddDistinct(pts, p);
   out.terminalStartSpan = (int)pts.size() - 1;
   for (size_t i = 1; i < terminal.size(); i++)
      AddDistinct(pts, terminal[i]);
   out.terminalEndSpan = (int)pts.size() - 1;
   for (size_t i = 1; i < leadB.size(); i++)
      AddDistinct(pts, leadB[i]);
   out.frame = fr;
   out.outward = outward;
   return pts.size() >= 2;
}

// The two paths of an elbow: from each rail's attachment out along the route's direction, a quarter bend of the radius of the wire's diameter (as freezy has it, not the
// radius) and straight down for the drop
struct ElbowPath
{
   Frame frame {};
   V3 outward {}, down {};
   std::vector<V3> a, b;
};

inline bool ElbowPoints(const Fixture& f, const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float total,
   const float wireDiameter, ElbowPath& out)
{
   using namespace Detail;
   out = ElbowPath {};
   if (frames.size() < 2 || !Generatable(f, wires, total))
      return false;
   const float d = AttachDistance(f, total);
   const Frame fr = FrameAt(frames, rowDist, d);
   const std::vector<Wire>& ws = WiresAt(wires, d);
   V3 outward = f.endpoint == kEndStart ? fr.t * -1.f : fr.t;
   outward = NormSafe(RotateAbout(fr.u, Rad(f.zAngle), outward), outward);
   const V3 down = fr.u * -1.f;
   const float drop = std::max(0.1f, f.dropLength);
   const float bend = std::min(std::max(0.05f, wireDiameter), drop);
   const V3 normal = NormSafe(WireSweep::Cross(outward, down), V3 { 0.f, 0.f, 0.f });
   if (WireSweep::Dot(normal, normal) <= 1e-8f)
      return false;
   const auto path = [&](const V3& attachment)
   {
      std::vector<V3> pts;
      AddDistinct(pts, attachment);
      const V3 center = attachment + down * bend;
      const V3 startRadius = down * -bend;
      for (int i = 1; i <= kElbowBendSegments; i++)
      {
         const float angle = kPi * 0.5f * (float)i / (float)kElbowBendSegments;
         AddDistinct(pts, center + RotateAbout(normal, angle, startRadius));
      }
      AddDistinct(pts, center + outward * bend + down * std::max(0.f, drop - bend));
      return pts;
   };
   out.a = path(fr.p + fr.r * ws[f.railA].x + fr.u * ws[f.railA].z);
   out.b = path(fr.p + fr.r * ws[f.railB].x + fr.u * ws[f.railB].z);
   out.frame = fr;
   out.outward = outward;
   out.down = down;
   return out.a.size() >= 2 && out.a.size() == out.b.size();
}

// BridgeRings: a band of triangles between a rail's last ring and its fitting's mouth ring, the vertices of both rings reused, their normals averaged. The rings are matched by
// the rotation (and the reversal, a fitting at the start leaves the other way round) that moves the vertices least
template <class Vertex>
void BridgeRings(std::vector<Vertex>& verts, std::vector<unsigned>& idx, const int ringA, const int ringB, const int sides)
{
   using WireSweep::Cross;
   using WireSweep::Dot;
   if (ringA < 0 || ringB < 0 || (size_t)ringA + sides > verts.size() || (size_t)ringB + sides > verts.size())
      return;
   const auto pos = [&](const int k) { return V3 { verts[k].x, verts[k].y, verts[k].z }; };
   const auto map = [&](const int i, const int offset, const bool reversed) { return reversed ? (offset - i + 2 * sides) % sides : (i + offset) % sides; };
   V3 ca { 0.f, 0.f, 0.f }, cb { 0.f, 0.f, 0.f };
   for (int i = 0; i < sides; i++)
   {
      ca = ca + pos(ringA + i);
      cb = cb + pos(ringB + i);
   }
   const V3 axisCenter = (ca + cb) * (1.f / (float)(sides * 2));
   int bestOffset = 0;
   bool bestReversed = false;
   float best = 1e30f;
   for (int rev = 0; rev < 2; rev++)
      for (int offset = 0; offset < sides; offset++)
      {
         float sum = 0.f;
         for (int i = 0; i < sides; i++)
         {
            const V3 d = pos(ringA + i) - pos(ringB + map(i, offset, rev == 1));
            sum += Dot(d, d);
         }
         if (sum < best)
         {
            best = sum;
            bestOffset = offset;
            bestReversed = rev == 1;
         }
      }
   // One averaged normal for each welded pair, so the shading is continuous across the seam
   for (int i = 0; i < sides; i++)
   {
      Vertex& va = verts[ringA + i];
      Vertex& vb = verts[ringB + map(i, bestOffset, bestReversed)];
      const V3 n { va.nx + vb.nx, va.ny + vb.ny, va.nz + vb.nz };
      const float l = WireSweep::Len(n);
      if (l > 1e-3f)
      {
         const V3 u = n * (1.f / l);
         va.nx = vb.nx = u.x;
         va.ny = vb.ny = u.y;
         va.nz = vb.nz = u.z;
      }
   }
   for (int i = 0; i < sides; i++)
   {
      const int next = (i + 1) % sides;
      const int a0 = ringA + i, a1 = ringA + next;
      const int b0 = ringB + map(i, bestOffset, bestReversed), b1 = ringB + map(next, bestOffset, bestReversed);
      const V3 centroid = (pos(a0) + pos(a1) + pos(b0) + pos(b1)) * 0.25f;
      const V3 normal = Cross(pos(b0) - pos(a0), pos(b1) - pos(a0));
      if (Dot(normal, centroid - axisCenter) >= 0.f)
         idx.insert(idx.end(), { (unsigned)a0, (unsigned)b0, (unsigned)b1, (unsigned)a0, (unsigned)b1, (unsigned)a1 });
      else
         idx.insert(idx.end(), { (unsigned)a0, (unsigned)b1, (unsigned)b0, (unsigned)a0, (unsigned)a1, (unsigned)b1 });
   }
}

// Does the path turn back on itself at a point (freezy's tubes refuse such a path)
inline bool PathReverses(const std::vector<V3>& p)
{
   for (size_t i = 1; i + 1 < p.size(); i++)
   {
      const V3 in = WireSweep::Norm(p[i] - p[i - 1]), out = WireSweep::Norm(p[i + 1] - p[i]);
      if (WireSweep::Dot(in, in) > 1e-8f && WireSweep::Dot(out, out) > 1e-8f && WireSweep::Dot(in, out) <= -cosf(Detail::Rad(0.5f)))
         return true;
   }
   return false;
}

// The hairpins and elbows: their shapes (built or not, why, the centreline for the overlay and the preview), and for those that are on in the render plan, the tubes, each mouth
// welded to the rail it takes over. After the supports. 'all' is the plan with everything, 'render' the one with the enabled fittings only
template <class Vertex>
void BuildEndFittings(const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float wireRadius, const int sides, const float bevel,
   const std::vector<Fixture>& fixtures, const FittingPlan& all, const FittingPlan& render, const TubeRings& rings, std::vector<Vertex>& verts, std::vector<unsigned>& idx,
   std::vector<FixtureShape>& shapes)
{
   using namespace Detail;
   if (frames.size() < 2 || rowDist.size() != frames.size() || shapes.size() != fixtures.size())
      return;
   const float total = rowDist.back();
   struct Seam
   {
      int endpoint, railA, railB, mouthA, mouthB;
   };
   std::vector<Seam> seams;
   for (size_t k = 0; k < fixtures.size(); k++)
   {
      if (!IsEndFitting(fixtures[k].type))
         continue;
      const Fixture& f = all.eff[k];
      FixtureShape& shape = shapes[k];
      const bool rendered = fixtures[k].enabled && render.ok[k];
      if (f.type == kFixtureTrim)
      {
         shape.built = true;
         shape.position = f.endpoint == kEndStart ? 0.f : total;
         shape.anchor = (f.endpoint == kEndStart ? frames.front() : frames.back()).p;
         shape.info = "Zero leaves a rail unchanged. Multiple trims at one endpoint use the largest offset per rail.";
         continue;
      }
      if (!all.ok[k])
      {
         shape.reason = all.why[k];
         // Where it would attach, for the preview
         shape.position = AttachDistance(f, total);
         shape.anchor = FrameAt(frames, rowDist, shape.position).p;
         continue;
      }
      shape.position = AttachDistance(f, total);
      const Frame at = FrameAt(frames, rowDist, shape.position);
      shape.anchor = at.p;
      shape.railA = f.railA;
      shape.railB = f.railB;
      const int idx0 = (int)idx.size();
      if (f.type == kFixtureHairpin)
      {
         const int leadSegs = std::max(2, (int)ceilf((float)f.density * 0.25f)), arcSegs = std::max(2, (int)ceilf((float)f.density * 0.5f));
         HairpinPath path;
         if (!HairpinPoints(f, frames, rowDist, wires, total, leadSegs, arcSegs, path))
         {
            shape.reason = "The loop has no path.";
            continue;
         }
         shape.built = true;
         shape.centerline = path.pts;
         shape.info = "The terminal semicircle uses Terminal Impact physics when it is on; its leads use the rail's physics.";
         if (rendered && !PathReverses(path.pts))
         {
            TubeOpts opts;
            opts.capStart = opts.capEnd = false;
            const int ring = f.endpoint == kEndStart ? rings.first[f.railA] : rings.last[f.railA];
            if (ring >= 0)
            {
               opts.seeded = true;
               opts.seed = f.endpoint == kEndStart ? rings.firstSide[f.railA] : rings.lastSide[f.railA];
            }
            TubeInfo info;
            if (AppendTube<Vertex>(path.pts, wireRadius, sides, bevel, verts, idx, &opts, &info))
               seams.push_back({ f.endpoint, f.railA, f.railB, info.firstRing, info.lastRing });
         }
      }
      else
      {
         ElbowPath path;
         if (!ElbowPoints(f, frames, rowDist, wires, total, wireRadius * 2.f, path))
         {
            shape.reason = "The elbow has no path.";
            continue;
         }
         shape.built = true;
         shape.centerline = path.a;
         shape.centerline2 = path.b;
         shape.info = "The offset shortens the two attached rails' colliders, and two vertical faces extend the floor down at the drop point. Other Rail Cutoffs change only the visible tubes.";
         if (rendered && !PathReverses(path.a) && !PathReverses(path.b))
         {
            int mouth[2] = { -1, -1 };
            for (int which = 0; which < 2; which++)
            {
               const int rail = which == 0 ? f.railA : f.railB;
               TubeOpts opts;
               opts.capStart = false; // Open at the rail, capped (bevelled) at the bottom
               opts.capEnd = true;
               if ((f.endpoint == kEndStart ? rings.first[rail] : rings.last[rail]) >= 0)
               {
                  opts.seeded = true;
                  opts.seed = f.endpoint == kEndStart ? rings.firstSide[rail] : rings.lastSide[rail];
               }
               TubeInfo info;
               if (AppendTube<Vertex>(which == 0 ? path.a : path.b, wireRadius, sides, bevel, verts, idx, &opts, &info))
                  mouth[which] = info.firstRing;
            }
            if (mouth[0] >= 0 && mouth[1] >= 0)
               seams.push_back({ f.endpoint, f.railA, f.railB, mouth[0], mouth[1] });
         }
      }
      shape.triangles = (int)((idx.size() - (size_t)idx0) / 3);
   }
   for (const Seam& s : seams)
   {
      const int ringA = s.endpoint == kEndStart ? rings.first[s.railA] : rings.last[s.railA];
      const int ringB = s.endpoint == kEndStart ? rings.first[s.railB] : rings.last[s.railB];
      BridgeRings<Vertex>(verts, idx, ringA, s.mouthA, sides);
      BridgeRings<Vertex>(verts, idx, ringB, s.mouthB, sides);
   }
}

// The wires cut by the trims at a distance, as a bit mask (for the preview), using the plan with everything on
inline unsigned TrimmedMaskAt(const Trims& trims, const int wireCount, const float total, const float d)
{
   unsigned mask = 0;
   for (int i = 0; i < wireCount && i < kMaxWires; i++)
      if (d < trims.start[i] - 1e-4f || d > total - trims.end[i] + 1e-4f)
         mask |= 1u << i;
   return mask;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The ball channel with the fittings: the route cut at every trim distance and a profile fitted to the wires that are left in each piece (AppendTrimmedSegment), the hairpin's
// box and the elbow's drop faces on top

struct ChannelGap
{
   float from, to; // Along the route: no channel here, the remaining wires can't hold a ball
};

inline void AppendPiece(Channel& c, const Channel& part)
{
   const unsigned base = (unsigned)c.verts.size();
   ChannelPiece piece;
   piece.firstVert = base;
   piece.rowSize = part.rowSize;
   piece.rows = part.rows;
   c.verts.insert(c.verts.end(), part.verts.begin(), part.verts.end());
   for (const unsigned t : part.tris)
      c.tris.push_back(t + base);
   for (const auto& e : part.edges)
      c.edges.push_back({ e.first + base, e.second + base });
   if (c.pieces.empty())
   {
      c.rowSize = part.rowSize;
      c.rows = part.rows;
   }
   c.pieces.push_back(piece);
}

inline Channel BuildTrimmedChannel(const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float wireRadius, const float ballRadius,
   const Profile& full, const Trims& trims, std::vector<ChannelGap>* gaps)
{
   if (gaps != nullptr)
      gaps->clear();
   if (!trims.Any() || frames.size() < 2 || rowDist.size() != frames.size())
      return BuildChannel(frames, full); // No fitting that trims: one piece, built exactly as before
   const float total = rowDist.back();
   const int n = std::min((int)wires.size(), kMaxWires);
   std::vector<float> cuts { 0.f, total };
   for (int i = 0; i < n; i++)
   {
      if (!wires[i].active)
         continue;
      for (const float v : { trims.start[i], total - trims.end[i] })
         if (v > 1e-5f && v < total - 1e-5f)
            cuts.push_back(v);
   }
   std::sort(cuts.begin(), cuts.end());
   for (int k = (int)cuts.size() - 1; k > 0; k--)
      if (cuts[k] - cuts[k - 1] <= 1e-5f)
         cuts.erase(cuts.begin() + k);
   Channel c;
   int authored = 0;
   for (int i = 0; i < n; i++)
      authored += wires[i].active ? 1 : 0;
   for (size_t k = 0; k + 1 < cuts.size(); k++)
   {
      const float a = cuts[k], b = cuts[k + 1];
      if (b - a <= 1e-5f)
         continue;
      const float mid = (a + b) * 0.5f;
      std::vector<Wire> set = wires;
      int present = 0;
      for (int i = 0; i < n; i++)
      {
         const bool in = wires[i].active && mid + 1e-5f >= trims.start[i] && mid - 1e-5f <= total - trims.end[i];
         set[i].active = in;
         present += in ? 1 : 0;
      }
      if (present == 0)
         continue; // No wire left here: the rail has ended, nothing to say
      Profile local;
      const Profile* profile = &full;
      if (present < authored)
      {
         std::string error;
         if (!FitProfile(set, wireRadius, ballRadius, local, error))
         {
            if (gaps != nullptr)
            {
               if (!gaps->empty() && fabsf(gaps->back().to - a) <= 1e-4f)
                  gaps->back().to = b;
               else
                  gaps->push_back({ a, b });
            }
            continue;
         }
         profile = &local;
      }
      std::vector<Frame> rows;
      rows.push_back(a <= 1e-5f ? frames.front() : FrameAt(frames, rowDist, a));
      for (size_t r = 0; r < frames.size(); r++)
         if (rowDist[r] > a + 1e-5f && rowDist[r] < b - 1e-5f)
            rows.push_back(frames[r]);
      rows.push_back(b >= total - 1e-5f ? frames.back() : FrameAt(frames, rowDist, b));
      const Channel part = BuildChannel(rows, *profile);
      if (!part.verts.empty())
         AppendPiece(c, part);
   }
   c.quadIndices = c.tris.size();
   return c;
}

// A floor span faces up toward the resting ball: those are what the elbow extends down
inline bool IsFloorSpan(const Profile& p, const std::pair<int, int>& span)
{
   const V2 s = p.verts[span.first], e = p.verts[span.second];
   const V2 edge = e - s;
   if (Dot(edge, edge) <= 1e-8f)
      return false;
   V2 inward = Norm({ -edge.y, edge.x });
   const V2 mid = (s + e) * 0.5f;
   if (Dot(inward, p.ball - mid) < 0.f)
      inward = inward * -1.f;
   return inward.y > 1e-4f;
}

// The elbow's drop faces: at the attachment the channel is fitted from the wires that are still there, and of its floor spans the one or two meeting at the lowest vertex are
// carried straight down by the Drop Length as two sided quads, with line colliders on their vertical edges and their bottom edge. The ball rolls to the shortened end and is guided
// down rather than falling off a cliff. False: no faces (the wires left there can't hold a ball)
inline bool AppendElbowDrop(Channel& c, const Fixture& f, const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float wireRadius,
   const float ballRadius, const Trims& trims)
{
   using namespace Detail;
   if (frames.size() < 2 || rowDist.size() != frames.size())
      return false;
   const float total = rowDist.back();
   const float d = AttachDistance(f, total);
   const Frame fr = FrameAt(frames, rowDist, d);
   std::vector<Wire> set = wires;
   int present = 0;
   const int n = std::min((int)wires.size(), kMaxWires);
   for (int i = 0; i < n; i++)
   {
      const bool in = wires[i].active && d >= trims.start[i] - 1e-4f && d <= total - trims.end[i] + 1e-4f;
      set[i].active = in;
      present += in ? 1 : 0;
   }
   Profile p;
   std::string error;
   if (present == 0 || !FitProfile(set, wireRadius, ballRadius, p, error) || p.spans.empty())
      return false;
   int lowest = -1;
   float lowestY = 1e30f;
   for (const auto& span : p.spans)
   {
      if (!IsFloorSpan(p, span))
         continue;
      for (const int v : { span.first, span.second })
         if (p.verts[v].y < lowestY)
         {
            lowestY = p.verts[v].y;
            lowest = v;
         }
   }
   if (lowest < 0)
      return false;
   const V3 down = fr.u * -1.f;
   bool any = false;
   for (const auto& span : p.spans)
   {
      if (!IsFloorSpan(p, span) || (span.first != lowest && span.second != lowest))
         continue;
      const V3 top0 = fr.p + fr.r * p.verts[span.first].x + fr.u * p.verts[span.first].y;
      const V3 top1 = fr.p + fr.r * p.verts[span.second].x + fr.u * p.verts[span.second].y;
      const unsigned base = (unsigned)c.verts.size();
      c.verts.push_back(top0);
      c.verts.push_back(top0 + down * f.dropLength);
      c.verts.push_back(top1);
      c.verts.push_back(top1 + down * f.dropLength);
      // The pattern of BuildChannel: a = row, b = next, d = row, e = next
      const unsigned a = base, b = base + 1, dd = base + 2, e = base + 3;
      c.tris.insert(c.tris.end(), { a, b, dd, b, e, dd, a, dd, b, b, dd, e });
      c.edges.push_back({ a, b });
      c.edges.push_back({ dd, e });
      c.edges.push_back({ b, e });
      any = true;
   }
   return any;
}

// The hairpin's collider: a square tube along the same path (4 lead and 12 arc segments), side = the wire diameter, its corners at 45 degrees. Outward facing triangles (a
// HitTriangle is one sided and the ball is outside the tube), a line collider on each of the four long corner lines of a span and a point at each vertex. The spans of the arc are
// recorded as terminal. The Channel keeps its quads first: these single triangles go last
struct BoxTerminal
{
   std::vector<size_t> tris, edges, verts; // Indices (triangles counted by 3) that belong to the arc
};

inline bool AppendHairpinBox(Channel& c, const Fixture& f, const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float total,
   const float wireDiameter, BoxTerminal& terminal)
{
   using namespace Detail;
   HairpinPath path;
   if (!HairpinPoints(f, frames, rowDist, wires, total, kHairpinColliderLeadSegments, kHairpinColliderArcSegments, path) || PathReverses(path.pts))
      return false;
   const std::vector<V3>& pts = path.pts;
   const int np = (int)pts.size();
   const float half = wireDiameter * 0.5f; // The side of the box is the wire diameter: its corners are half of it out along each of the two directions
   // Frames along the path: transported from the route's frame at the attachment, as AppendPolylineSweep (the first ring's side is the path's start projected on the route's
   // tangent, else the route's side)
   std::vector<V3> right(np), up(np);
   std::vector<V3> tan(np);
   for (int i = 0; i < np; i++)
   {
      const V3 fallback = path.frame.u * -1.f;
      V3 t;
      if (i == 0)
         t = NormSafe(pts[1] - pts[0], fallback);
      else if (i == np - 1)
         t = NormSafe(pts[i] - pts[i - 1], fallback);
      else
      {
         const V3 in = NormSafe(pts[i] - pts[i - 1], fallback);
         const V3 out = NormSafe(pts[i + 1] - pts[i], in);
         t = NormSafe(in + out, in);
      }
      tan[i] = t;
      V3 r;
      if (i == 0)
      {
         r = path.frame.t - t * WireSweep::Dot(path.frame.t, t);
         if (WireSweep::Dot(r, r) <= 1e-8f)
            r = path.frame.r - t * WireSweep::Dot(path.frame.r, t);
         r = NormSafe(r, path.frame.r);
      }
      else
      {
         // Rotate the previous side by the rotation that takes the previous tangent to this one
         const V3 axis = WireSweep::Cross(tan[i - 1], t);
         const float s = WireSweep::Len(axis), cs = std::clamp(WireSweep::Dot(tan[i - 1], t), -1.f, 1.f);
         V3 tr = right[i - 1];
         if (s > 1e-6f)
            tr = RotateAbout(axis * (1.f / s), atan2f(s, cs), right[i - 1]);
         tr = tr - t * WireSweep::Dot(tr, t);
         r = NormSafe(tr, right[i - 1]);
      }
      const V3 u = NormSafe(WireSweep::Cross(r, t), path.frame.u);
      right[i] = NormSafe(WireSweep::Cross(t, u), r);
      up[i] = u;
   }
   const unsigned base = (unsigned)c.verts.size();
   for (int i = 0; i < np; i++)
      for (int k = 0; k < 4; k++)
      {
         // Corners at 45, 135, 225, 315 degrees
         const float sr = (k == 0 || k == 3) ? 1.f : -1.f, su = (k == 0 || k == 1) ? 1.f : -1.f;
         c.verts.push_back(pts[i] + right[i] * (sr * half) + up[i] * (su * half));
      }
   const auto vert = [&](const int ring, const int corner) { return base + (unsigned)(ring * 4 + corner); };
   const int termA = path.terminalStartSpan, termB = path.terminalEndSpan;
   for (int span = 0; span + 1 < np; span++)
   {
      const bool arc = span >= termA && span < termB;
      const V3 centre = (pts[span] + pts[span + 1]) * 0.5f;
      for (int face = 0; face < 4; face++)
      {
         const int next = (face + 1) % 4;
         const unsigned va = vert(span, face), vb = vert(span, next), vc = vert(span + 1, next), vd = vert(span + 1, face);
         const V3 pa = c.verts[va], pb = c.verts[vb], pc = c.verts[vc], pd = c.verts[vd];
         const V3 outward = (pa + pb + pc + pd) * 0.25f - centre;
         const bool flip = WireSweep::Dot(WireSweep::Cross(pb - pa, pc - pa), outward) < 0.f;
         for (int tri = 0; tri < 2; tri++)
         {
            if (arc)
               terminal.tris.push_back(c.tris.size() / 3);
            const unsigned t1 = tri == 0 ? vb : vc, t2 = tri == 0 ? vc : vd;
            c.tris.push_back(va);
            c.tris.push_back(flip ? t2 : t1);
            c.tris.push_back(flip ? t1 : t2);
         }
      }
      for (int corner = 0; corner < 4; corner++)
      {
         if (arc)
            terminal.edges.push_back(c.edges.size());
         c.edges.push_back({ vert(span, corner), vert(span + 1, corner) });
      }
   }
   for (int ring = 0; ring < np; ring++)
      if (ring >= termA && ring <= termB)
         for (int corner = 0; corner < 4; corner++)
            terminal.verts.push_back(vert(ring, corner));
   return true;
}

// The whole channel: the trimmed pieces, the elbows' drop faces, then the hairpins' boxes. Where the rail's own profile did not fit, there is nothing (the panel's red error)
inline Channel BuildFittingChannel(const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float wireRadius, const float ballRadius,
   const Profile& full, const std::vector<Fixture>& fixtures, const FittingPlan& all, std::vector<ChannelGap>* gaps)
{
   const float total = rowDist.empty() ? 0.f : rowDist.back();
   const Trims trims = ColliderTrims(fixtures, wires, all);
   Channel c = BuildTrimmedChannel(frames, rowDist, wires, wireRadius, ballRadius, full, trims, gaps);
   bool anyFitting = false;
   for (size_t j = 0; j < fixtures.size(); j++)
      anyFitting |= (fixtures[j].type == kFixtureHairpin || fixtures[j].type == kFixtureElbow) && all.ok[j];
   if (!anyFitting)
      return c;
   for (size_t j = 0; j < fixtures.size(); j++)
      if (fixtures[j].type == kFixtureElbow && all.ok[j])
         AppendElbowDrop(c, all.eff[j], frames, rowDist, wires, wireRadius, ballRadius, trims);
   c.quadIndices = c.tris.size();
   BoxTerminal terminal;
   for (size_t j = 0; j < fixtures.size(); j++)
      if (fixtures[j].type == kFixtureHairpin && all.ok[j])
         AppendHairpinBox(c, all.eff[j], frames, rowDist, wires, total, wireRadius * 2.f, terminal);
   if (!terminal.tris.empty() || !terminal.edges.empty() || !terminal.verts.empty())
   {
      c.triTerminal.assign(c.tris.size() / 3, 0);
      c.edgeTerminal.assign(c.edges.size(), 0);
      c.vertTerminal.assign(c.verts.size(), 0);
      for (const size_t i : terminal.tris)
         c.triTerminal[i] = 1;
      for (const size_t i : terminal.edges)
         c.edgeTerminal[i] = 1;
      for (const size_t i : terminal.verts)
         c.vertTerminal[i] = 1;
   }
   return c;
}

// Every enabled fixture of the rail in list order, each followed by its blobs, appended to the mesh of the tubes. 'shapes' gets one entry per fixture (built or not, why not,
// its centreline for the overlay and the cross-section). A fixture past the end of the route is built at the end.
template <class Vertex>
void BuildFixtures(const std::vector<Frame>& frames, const std::vector<float>& rowDist, const std::vector<Wire>& wires, const float wireRadius, const int sides, const float bevel,
   const float ballRadius, const Profile& profile, const std::vector<Fixture>& fixtures, std::vector<Vertex>& verts, std::vector<unsigned>& idx, std::vector<FixtureShape>& shapes,
   const Trims* trims = nullptr)
{
   using namespace Detail;
   shapes.assign(fixtures.size(), FixtureShape {});
   if (frames.size() < 2 || rowDist.size() != frames.size())
   {
      for (FixtureShape& s : shapes)
         s.reason = "the route is too short";
      return;
   }
   const float total = rowDist.back();
   for (size_t k = 0; k < fixtures.size(); k++)
   {
      const Fixture& fx = fixtures[k];
      FixtureShape& shape = shapes[k];
      if (fx.type < 0 || fx.type >= kFixtureTypeCount)
      {
         shape.reason = "made by a newer version";
         continue;
      }
      if (IsEndFitting(fx.type))
         continue; // BuildEndFittings, after the supports
      const float d = std::clamp(fx.position, 0.f, total);
      shape.position = d;
      const Frame fr = FrameAt(frames, rowDist, d);
      shape.anchor = fr.p;
      const std::vector<Wire>& ws = WiresAt(wires, d);

      std::vector<V3> pts; // The tube's centreline
      std::vector<std::vector<V3>> solderPaths; // What the blobs are found against
      bool closed = false;
      std::string why;
      bool ok = false;
      switch (fx.type)
      {
      case kFixtureRing:
         ok = RingPoints(fx, fr, ws, wireRadius, pts, closed, why);
         if (ok)
         {
            std::vector<V3> path = pts;
            if (closed && path.size() > 1)
               path.push_back(path.front());
            solderPaths.push_back(path);
         }
         break;
      case kFixtureRung:
      {
         V2 a, b;
         ok = RungEnds(fx, ws, wireRadius, fx.angle, a, b, why);
         if (ok)
         {
            pts = { At(fr, a), At(fr, b) };
            solderPaths.push_back(pts);
         }
         break;
      }
      case kFixtureCradle:
         ok = CradlePoints(fx, fr, ws, wireRadius, pts, why);
         if (ok)
            solderPaths.push_back(pts);
         break;
      case kFixtureStand:
      {
         std::vector<V3> attach;
         ok = StandPoints(fx, fr, ws, wireRadius, pts, attach, why);
         if (ok)
            solderPaths.push_back(attach); // The attachment only: no blobs at the foot
         break;
      }
      }
      if (!ok)
      {
         shape.reason = why;
         continue;
      }
      shape.built = true;
      shape.centerline = pts;
      if (closed && pts.size() > 1)
         shape.centerline.push_back(pts.front());
      for (const V3& p : shape.centerline)
      {
         const V3 rel = p - fr.p;
         shape.section.push_back({ WireSweep::Dot(rel, fr.r), WireSweep::Dot(rel, fr.u) });
      }
      if (!profile.verts.empty() && shape.section.size() >= 2)
      {
         const float limit = ballRadius + wireRadius - 0.01f;
         for (size_t i = 0; i + 1 < shape.section.size() && !shape.inBallPath; i++)
         {
            const V2 a = shape.section[i], b = shape.section[i + 1], ab = b - a;
            const float l2 = Dot(ab, ab);
            const float t = l2 > 1e-12f ? std::clamp(Dot(profile.ball - a, ab) / l2, 0.f, 1.f) : 0.f;
            shape.inBallPath = Len(profile.ball - (a + ab * t)) < limit;
         }
      }
      if (!fx.enabled)
         continue;

      const size_t idx0 = idx.size();
      if (fx.type == kFixtureRing && closed)
         AppendClosedTube<Vertex>(pts, wireRadius, sides, verts, idx);
      else
         AppendTube<Vertex>(pts, wireRadius, sides, bevel, verts, idx);

      // Blobs, one per wire that comes within the threshold of a path
      const float railR = wireRadius, pathR = wireRadius;
      const bool atStart = d <= 1e-4f, atEnd = d >= total - 1e-4f;
      const float span = std::max(8.f, railR + pathR + fx.solderThreshold) * 2.f;
      for (size_t wi = 0; wi < ws.size(); wi++)
      {
         const Wire& w = ws[wi];
         if (!w.active)
            continue;
         // A wire a fitting has trimmed away at this Position is not there to solder to
         if (trims != nullptr && wi < (size_t)kMaxWires && (d < trims->start[wi] - 1e-4f || d > total - trims->end[wi] + 1e-4f))
            continue;
         const V3 at = fr.p + fr.r * w.x + fr.u * w.z;
         const V3 railStart = atStart ? at : at - fr.t * span;
         const V3 railEnd = atEnd ? at : at + fr.t * span;
         for (const std::vector<V3>& path : solderPaths)
         {
            Touch touch;
            if (FindTouch(railStart, railEnd, railR, path, pathR, fx.solderThreshold, touch))
               AppendBlob<Vertex>(touch, fx.solderSize, verts, idx);
         }
      }
      shape.triangles = (int)((idx.size() - idx0) / 3);
   }
}

}
