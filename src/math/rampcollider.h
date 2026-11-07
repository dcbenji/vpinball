// license:GPLv3+

// Ramp Collider: the collider of a ramp whose look is modelled elsewhere (a Blender part), drawn in the editor over it. A route of control points (x, y, height,
// width, the height of the left and right wall, roll) is swept into a floor and two walls of triangles, the rows being the Wire Rail's (math/wirerail.h).
// The floor and the walls are two parts because they want different friction (walls 0.02, floor 0.3). Both are plain triangle lists that the generic
// primitive collider turns into triangles, edges and points, so an older engine plays them too.
// Header only, no dependency on the engine, checked standalone by tools/rampcollider_check.cpp.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "math/wirerail.h"

namespace RampCollider
{

using WireRail::Frame;
using WireRail::PathPoint;
using WireSweep::V3;

inline constexpr float kDefaultWidth = 75.f; // Native Ramp defaults (DefaultPropsRamp)
inline constexpr float kDefaultWall = 62.f;
inline constexpr float kDefaultEntryLength = 94.f; // 2 in
inline constexpr int kDefaultCurvatureDetail = 32; // A new row every 1.4 degrees of tangent turn
inline constexpr float kMinWall = 0.5f; // A wall end under this is no wall
inline constexpr int kTriangleWarning = 3000;
inline constexpr float kDipRise = 1.f; // A dip is a local minimum with a rise of more than this on both sides
inline constexpr float kPi = 3.14159265358979f;

// A control point as the builder reads it (the DragPoint's fields). width <= 0 and a wall < 0 mean 'not set': the defaults
struct ControlPoint
{
   float x = 0.f, y = 0.f; // Matched to the route's control samples
   float roll = 0.f;
   float width = 0.f;
   float wallLeft = -1.f, wallRight = -1.f;
};

struct Params
{
   bool flushEntry = true;
   float entryLength = kDefaultEntryLength;
   int curvatureDetail = kDefaultCurvatureDetail;
   float ballDiameter = WireRail::kDefaultBallDiameter;
   V3 up { 0.f, 0.f, 1.f }; // Against gravity, unit length
};

struct Mesh
{
   std::vector<V3> verts, normals;
   std::vector<unsigned> idx;
};

struct Dip
{
   int row = 0;
   float dist = 0.f; // Along the route
   float depth = 0.f; // The smaller of the two rises
};

struct Result
{
   bool fits = false;
   std::string reason; // Why it does not fit
   std::vector<Frame> frames; // The rows
   std::vector<float> dist; // 3D length along the route to each row
   std::vector<float> width, wallLeft, wallRight; // Per row
   std::vector<int> controlRow; // Per control point of the route that took a row (in order); the row it starts
   Mesh floor, walls;
   int floorTris = 0, wallTris = 0; // Both windings counted
   bool entryShortened = false; // Flush Entry: the entry length was clamped to the first span
   float entryUsed = 0.f;
   float peakEntryGrade = 0.f; // Degrees, the steepest part of the eased entry
   float straightEntryGrade = 0.f; // Degrees, the straight line from 0 to the height at the end of the entry
   std::vector<float> grade; // Per row: degrees against gravity, positive uphill
   float minGrade = 0.f, maxGrade = 0.f;
   std::vector<float> ballHeight; // Per row: the ball's centre against gravity
   std::vector<Dip> dips;
   float minPlanRadius = 1e30f;
};

inline float SmootherStep(const float x)
{
   const float t = std::clamp(x, 0.f, 1.f);
   return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

inline float PointWidth(const ControlPoint& p) { return p.width > 0.f ? p.width : kDefaultWidth; }
inline float PointWallLeft(const ControlPoint& p) { return p.wallLeft >= 0.f ? p.wallLeft : kDefaultWall; }
inline float PointWallRight(const ControlPoint& p) { return p.wallRight >= 0.f ? p.wallRight : kDefaultWall; }

inline std::string Num(const float v, const int decimals = 0)
{
   char b[48];
   snprintf(b, sizeof b, "%.*f", decimals, v);
   return b;
}

// The dips in a profile of ball heights: a drop of more than kDipRise from the highest point so far, then a rise of more than kDipRise from the bottom
// (noise inside the descent does not count a second time). 'dist' is the length along the route of each entry
inline std::vector<Dip> FindDips(const std::vector<float>& h, const std::vector<float>& dist)
{
   std::vector<Dip> dips;
   if (h.size() < 3)
      return dips;
   float peak = h[0], valley = h[0];
   float peakBefore = h[0]; // The highest point before the valley
   int valleyRow = 0;
   bool descending = false;
   for (size_t i = 1; i < h.size(); i++)
   {
      if (!descending)
      {
         if (h[i] > peak)
            peak = h[i];
         else if (h[i] < peak - kDipRise)
         {
            descending = true;
            peakBefore = peak;
            valley = h[i];
            valleyRow = (int)i;
         }
      }
      else
      {
         if (h[i] < valley)
         {
            valley = h[i];
            valleyRow = (int)i;
         }
         else if (h[i] > valley + kDipRise)
         {
            dips.push_back({ valleyRow, dist[valleyRow], std::min(peakBefore - valley, h[i] - valley) });
            descending = false;
            peak = h[i];
         }
      }
   }
   return dips;
}

inline void AddTri(Mesh& m, const unsigned a, const unsigned b, const unsigned c)
{
   m.idx.push_back(a);
   m.idx.push_back(b);
   m.idx.push_back(c);
}
// Both sides solid: the triangle and the same triangle reversed, on the same vertices (the engine's edge set dedupes the shared edges)
inline void AddTwoSided(Mesh& m, const unsigned a, const unsigned b, const unsigned c)
{
   AddTri(m, a, b, c);
   AddTri(m, a, c, b);
}

// Builds the floor and the walls from the fine route (as WireRail::PathPoint, the control samples marked) and the control points in order.
// A triangle (a, b, c) faces the way (b - a) x (c - a) points, which the engine's collider takes as the front: the front of the floor is up
inline Result Build(const std::vector<PathPoint>& fineIn, const std::vector<ControlPoint>& points, const Params& prm)
{
   Result res;
   std::vector<PathPoint> path = fineIn;
   if (path.size() < 2 || points.size() < 2)
   {
      res.reason = "A ramp collider needs two points";
      return res;
   }
   path.back().control = true; // The engine's curve does not mark its very last sample (DragPointCurve::GetRgVertex), it is the last point's
   if (prm.flushEntry && prm.entryLength > 0.f)
   {
      // The engine's curve gives a straight span as its two end samples: the entry ease needs samples to act on. Segments over 4 units within the entry
      // (and the 8 after it) are cut into pieces of at most 4, on the straight chord. The route is the same polyline
      std::vector<PathPoint> dense;
      dense.reserve(path.size() + 64);
      float along = 0.f;
      for (size_t i = 0; i < path.size(); i++)
      {
         dense.push_back(path[i]);
         if (i + 1 == path.size())
            break;
         const V3 a { path[i].x, path[i].y, path[i].z }, b { path[i + 1].x, path[i + 1].y, path[i + 1].z };
         const float len = WireSweep::Len(b - a);
         if (along < prm.entryLength + 8.f && len > 4.f)
         {
            const int pieces = std::min(256, (int)ceilf(len / 4.f));
            for (int k = 1; k < pieces; k++)
            {
               const V3 p = a + (b - a) * ((float)k / (float)pieces);
               dense.push_back({ p.x, p.y, p.z, false });
            }
         }
         along += len;
      }
      path.swap(dense);
   }
   const size_t n = path.size();
   // The control samples and the points they came from (coincident points give no sample)
   std::vector<int> ptOf(n, -1);
   {
      size_t next = 0;
      for (size_t i = 0; i < n; i++)
      {
         if (!path[i].control)
            continue;
         size_t k = next;
         while (k < points.size() && (points[k].x != path[i].x || points[k].y != path[i].y))
            k++;
         if (k < points.size())
         {
            ptOf[i] = (int)k;
            next = k + 1;
         }
      }
   }
   std::vector<size_t> ctrl; // Fine index of each active control point
   for (size_t i = 0; i < n; i++)
      if (ptOf[i] >= 0)
         ctrl.push_back(i);
   if (ctrl.size() < 2)
   {
      res.reason = "A ramp collider needs two distinct points";
      return res;
   }
   std::vector<float> s(n, 0.f); // 3D length of the fine route, before the entry ease
   for (size_t i = 1; i < n; i++)
      s[i] = s[i - 1] + WireSweep::Len(V3 { path[i].x - path[i - 1].x, path[i].y - path[i - 1].y, path[i].z - path[i - 1].z });
   if (s[n - 1] < 1e-3f)
   {
      res.reason = "A ramp collider needs two distinct points";
      return res;
   }

   // Flush Entry: the floor leaves the playfield flat (z 0), eased to the route's height over L, by smootherstep
   float L = 0.f;
   size_t forcedAt = 0;
   if (prm.flushEntry)
   {
      const float span = s[ctrl[1]] - s[ctrl[0]];
      L = std::min(std::max(prm.entryLength, 0.f), span);
      res.entryShortened = prm.entryLength > span + 1e-3f;
      if (L < 0.5f)
         L = 0.f;
      res.entryUsed = L;
      if (L > 0.f)
      {
         float best = 1e30f;
         for (size_t i = ctrl[0] + 1; i < ctrl[1]; i++)
            if (fabsf(s[i] - L) < best)
            {
               best = fabsf(s[i] - L);
               forcedAt = i;
            }
         float zEnd = path[ctrl[0]].z, sEnd = 0.f;
         for (size_t i = 0; i < n && s[i] <= L; i++)
         {
            zEnd = path[i].z;
            sEnd = s[i];
         }
         for (size_t i = 0; i < n && s[i] < L; i++)
            path[i].z *= SmootherStep(s[i] / L);
         // The steepest part of the eased entry, over windows of about 2 units
         float peak = 0.f;
         size_t a = 0;
         for (size_t b = 1; b < n && s[b] <= L + 1e-3f; b++)
         {
            while (a + 1 < b && s[b] - s[a + 1] >= 2.f)
               a++;
            const float len = WireSweep::Len(V3 { path[b].x - path[a].x, path[b].y - path[a].y, path[b].z - path[a].z });
            if (len > 0.5f)
               peak = std::max(peak, std::asin(std::clamp((path[b].z - path[a].z) / len, -1.f, 1.f)));
         }
         res.peakEntryGrade = peak * 180.f / kPi;
         res.straightEntryGrade = sEnd > 0.f ? std::asin(std::clamp(zEnd / sEnd, -1.f, 1.f)) * 180.f / kPi : 0.f;
      }
   }
   for (size_t i = 0; i < n; i++)
      path[i].force = path[i].control || (L > 0.f && i == forcedAt);

   // Rows. The rolls are the real ones, so the thinning and the roll subdivision see them
   std::vector<float> rolls;
   for (size_t i = 0; i < n; i++)
      if (path[i].control)
         rolls.push_back(ptOf[i] >= 0 ? points[ptOf[i]].roll : 0.f);
   rolls.push_back(points.back().roll);
   std::vector<WireRail::Sample> rows = WireRail::BuildRows(path, rolls, prm.curvatureDetail);
   if (rows.size() < 2)
   {
      res.reason = "A ramp collider needs two distinct points";
      return res;
   }
   const int rn = (int)rows.size();
   res.dist.assign(rn, 0.f);
   for (int i = 1; i < rn; i++)
      res.dist[i] = res.dist[i - 1] + WireSweep::Len(rows[i].p - rows[i - 1].p);
   // The row each control point starts: the row at its x and y
   res.controlRow.assign(ctrl.size(), -1);
   {
      size_t k = 0;
      for (int i = 0; i < rn && k < ctrl.size(); i++)
         while (k < ctrl.size() && rows[i].p.x == path[ctrl[k]].x && rows[i].p.y == path[ctrl[k]].y)
            res.controlRow[k++] = i;
   }
   for (const int r : res.controlRow)
      if (r < 0)
      {
         res.reason = "The route lost a control point (two points on the same spot?)";
         return res;
      }
   // Flush Entry, the roll: none within L, then eased in over the rest of the first span. Values only, the rows stay
   if (L > 0.f)
   {
      const float s1 = res.dist[res.controlRow[1]];
      for (int i = 0; i < rn && res.dist[i] < s1; i++)
         rows[i].roll = res.dist[i] < L ? 0.f : rows[i].roll * SmootherStep((res.dist[i] - L) / std::max(s1 - L, 1e-4f));
   }
   res.frames = WireRail::BuildFrames(rows);

   // Width and the walls per row: linear by length within the span between two control points (as a native Ramp's width)
   res.width.assign(rn, 0.f);
   res.wallLeft.assign(rn, 0.f);
   res.wallRight.assign(rn, 0.f);
   for (size_t k = 0; k + 1 < ctrl.size(); k++)
   {
      const ControlPoint& a = points[ptOf[ctrl[k]]];
      const ControlPoint& b = points[ptOf[ctrl[k + 1]]];
      const int r0 = res.controlRow[k], r1 = res.controlRow[k + 1];
      const float span = res.dist[r1] - res.dist[r0];
      for (int i = r0; i <= r1; i++)
      {
         const float t = span > 1e-6f ? (res.dist[i] - res.dist[r0]) / span : 0.f;
         res.width[i] = PointWidth(a) + (PointWidth(b) - PointWidth(a)) * t;
         res.wallLeft[i] = PointWallLeft(a) + (PointWallLeft(b) - PointWallLeft(a)) * t;
         res.wallRight[i] = PointWallRight(a) + (PointWallRight(b) - PointWallRight(a)) * t;
      }
   }
   float minWidth = 1e30f;
   for (size_t k = 0; k < ctrl.size(); k++)
   {
      minWidth = std::min(minWidth, PointWidth(points[ptOf[ctrl[k]]]));
   }

   // The analysis that does not need a fit: grade against gravity and the ball's height
   res.grade.assign(rn, 0.f);
   res.ballHeight.assign(rn, 0.f);
   const float ballR = prm.ballDiameter * 0.5f;
   for (int i = 0; i < rn; i++)
   {
      const Frame& f = res.frames[i];
      res.grade[i] = std::asin(std::clamp(WireSweep::Dot(f.t, prm.up), -1.f, 1.f)) * 180.f / kPi;
      res.ballHeight[i] = WireSweep::Dot(f.p + f.u * ballR, prm.up);
   }
   res.minGrade = *std::min_element(res.grade.begin(), res.grade.end());
   res.maxGrade = *std::max_element(res.grade.begin(), res.grade.end());
   res.dips = FindDips(res.ballHeight, res.dist);

   // No fit
   if (minWidth < prm.ballDiameter)
   {
      res.reason = "Width " + Num(minWidth, 1) + " is under the ball diameter " + Num(prm.ballDiameter, 0);
      return res;
   }
   // The tightest turn in plan (the route seen from above; a steep climb or the entry ease is no turn): the inside edge of the floor folds when it is tighter
   // than half the width there
   for (int i = 1; i + 1 < rn; i++)
   {
      const float ax = rows[i].p.x - rows[i - 1].p.x, ay = rows[i].p.y - rows[i - 1].p.y;
      const float bx = rows[i + 1].p.x - rows[i].p.x, by = rows[i + 1].p.y - rows[i].p.y;
      const float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
      if (la < 1e-4f || lb < 1e-4f)
         continue;
      const float c = (ax * bx + ay * by) / (la * lb);
      if (c < 0.f)
      {
         res.reason = "Route folds back at " + Num(res.dist[i]) + " units";
         return res;
      }
      // The radius of the circle through three rows: the turn of an arc whatever its sampling, and at a corner the legs count (short ones are tight)
      const float cross = ax * by - ay * bx;
      if (fabsf(cross) < 1e-9f)
         continue;
      const float lc = sqrtf((ax + bx) * (ax + bx) + (ay + by) * (ay + by));
      const float radius = la * lb * lc / (2.f * fabsf(cross));
      res.minPlanRadius = std::min(res.minPlanRadius, radius);
      if (radius < res.width[i] * 0.5f + 1.f) // The row's own width (a wide mouth next to a narrow bend does not make the bend a fault)
      {
         res.reason = "Turn tighter than half the width at " + Num(res.dist[i]) + " units";
         return res;
      }
   }
   for (int i = 0; i + 1 < rn; i++)
      if (WireSweep::Dot(res.frames[i].r, res.frames[i + 1].r) < 0.f)
      {
         res.reason = "Route folds back at " + Num(res.dist[i]) + " units";
         return res;
      }

   // The floor: two vertices per row (left, right), a quad per row span, both windings
   Mesh& fl = res.floor;
   for (int i = 0; i < rn; i++)
   {
      const Frame& f = res.frames[i];
      const float h = res.width[i] * 0.5f;
      fl.verts.push_back(f.p - f.r * h);
      fl.verts.push_back(f.p + f.r * h);
      fl.normals.push_back(f.u);
      fl.normals.push_back(f.u);
   }
   for (int i = 0; i + 1 < rn; i++)
   {
      const unsigned l0 = (unsigned)(i * 2), r0 = l0 + 1, l1 = l0 + 2, r1 = l0 + 3;
      AddTwoSided(fl, l0, r0, l1);
      AddTwoSided(fl, r0, r1, l1);
   }
   res.floorTris = (int)(fl.idx.size() / 3);

   // The walls: per side, from the floor edge up the row's up vector by the wall height. A span with both ends under kMinWall has no wall, one end
   // under it tapers to a triangle. The bottom vertices are the floor edge's positions
   Mesh& wl = res.walls;
   for (int i = 0; i < rn; i++)
   {
      const Frame& f = res.frames[i];
      const float h = res.width[i] * 0.5f;
      const V3 lb = f.p + f.r * h, rb = f.p - f.r * h; // The frame's r points to the ball's left seen from above in VPX (+y toward the player): left wall on +r
      const float hl = res.wallLeft[i] >= kMinWall ? res.wallLeft[i] : 0.f, hr = res.wallRight[i] >= kMinWall ? res.wallRight[i] : 0.f;
      wl.verts.push_back(lb); // 4i: left bottom, 4i + 1: left top, 4i + 2: right bottom, 4i + 3: right top
      wl.verts.push_back(lb + f.u * hl);
      wl.verts.push_back(rb);
      wl.verts.push_back(rb + f.u * hr);
      wl.normals.push_back(f.r * -1.f);
      wl.normals.push_back(f.r * -1.f);
      wl.normals.push_back(f.r);
      wl.normals.push_back(f.r);
   }
   for (int i = 0; i + 1 < rn; i++)
      for (int side = 0; side < 2; side++)
      {
         const float h0 = side == 0 ? res.wallLeft[i] : res.wallRight[i], h1 = side == 0 ? res.wallLeft[i + 1] : res.wallRight[i + 1];
         const bool w0 = h0 >= kMinWall, w1 = h1 >= kMinWall;
         const unsigned b0 = (unsigned)(i * 4 + side * 2), t0 = b0 + 1, b1 = b0 + 4, t1 = b1 + 1;
         if (w0 && w1)
         {
            AddTwoSided(wl, b0, b1, t0);
            AddTwoSided(wl, b1, t1, t0);
         }
         else if (w0)
            AddTwoSided(wl, b0, b1, t0);
         else if (w1)
            AddTwoSided(wl, b0, b1, t1);
      }
   res.wallTris = (int)(wl.idx.size() / 3);
   res.fits = true;
   return res;
}

// Appends a mesh as engine vertices (Vertex3D_NoTex2 or alike: x y z nx ny nz tu tv), replacing what the lists hold
template <class Vertex>
void ToVertices(const Mesh& m, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
{
   verts.resize(m.verts.size());
   for (size_t i = 0; i < m.verts.size(); i++)
   {
      Vertex v {};
      v.x = m.verts[i].x;
      v.y = m.verts[i].y;
      v.z = m.verts[i].z;
      v.nx = m.normals[i].x;
      v.ny = m.normals[i].y;
      v.nz = m.normals[i].z;
      v.tu = 0.f;
      v.tv = 0.f;
      verts[i] = v;
   }
   idx = m.idx;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Ray tests against a reference mesh (triangles of V3, the mesh through its full matrix): brute force, on demand only

// The z of the reference under (x, y) on an up-facing face, taking the hit nearest 'nearZ'. A face faces up when its normal (the engine's: (b - a) x (c - a))
// has a dot of more than minDot with 'up'. False: no such face under the point
inline bool UpFacingHeight(const std::vector<V3>& verts, const std::vector<unsigned>& idx, const float x, const float y, const float nearZ, const V3 up, const float minDot, float& z)
{
   bool found = false;
   float bestD = 1e30f;
   for (size_t t = 0; t + 2 < idx.size(); t += 3)
   {
      const V3 a = verts[idx[t]], b = verts[idx[t + 1]], c = verts[idx[t + 2]];
      const V3 n = WireSweep::Norm(WireSweep::Cross(b - a, c - a));
      if (WireSweep::Dot(n, up) <= minDot || fabsf(n.z) < 1e-4f)
         continue;
      // Barycentric in plan
      const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
      if (fabsf(d) < 1e-12f)
         continue;
      const float l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
      const float l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
      const float l3 = 1.f - l1 - l2;
      if (l1 < -1e-5f || l2 < -1e-5f || l3 < -1e-5f)
         continue;
      const float hz = l1 * a.z + l2 * b.z + l3 * c.z;
      if (fabsf(hz - nearZ) < bestD)
      {
         bestD = fabsf(hz - nearZ);
         z = hz;
         found = true;
      }
   }
   return found;
}

// The first hit of a ray (either side of a triangle), as the distance along the unit direction. False: none within maxDist
inline bool RayFirstHit(const std::vector<V3>& verts, const std::vector<unsigned>& idx, const V3 o, const V3 dir, const float maxDist, float& dist)
{
   bool found = false;
   float best = maxDist;
   for (size_t t = 0; t + 2 < idx.size(); t += 3)
   {
      const V3 a = verts[idx[t]], b = verts[idx[t + 1]], c = verts[idx[t + 2]];
      const V3 e1 = b - a, e2 = c - a;
      const V3 p = WireSweep::Cross(dir, e2);
      const float det = WireSweep::Dot(e1, p);
      if (fabsf(det) < 1e-9f)
         continue;
      const float inv = 1.f / det;
      const V3 tv = o - a;
      const float u = WireSweep::Dot(tv, p) * inv;
      if (u < 0.f || u > 1.f)
         continue;
      const V3 q = WireSweep::Cross(tv, e1);
      const float v = WireSweep::Dot(dir, q) * inv;
      if (v < 0.f || u + v > 1.f)
         continue;
      const float d = WireSweep::Dot(e2, q) * inv;
      if (d > 1e-4f && d < best)
      {
         best = d;
         found = true;
      }
   }
   if (found)
      dist = best;
   return found;
}

// A native Ramp's floor on the closest point of its central curve to a collider point (Ramp::GetSurfaceFit): height, width, plan distance
struct SurfaceFit
{
   bool ok = false;
   float height = 0.f, width = 0.f, dist = 0.f;
};

// Fit to several Ramps (a route over two joined ramps): the candidate whose central curve is nearest, the first on a tie. False: no candidate has a curve
inline bool NearestFit(const std::vector<SurfaceFit>& candidates, SurfaceFit& best)
{
   bool found = false;
   for (const SurfaceFit& c : candidates)
      if (c.ok && (!found || c.dist < best.dist))
      {
         best = c;
         found = true;
      }
   return found;
}

} // namespace RampCollider
