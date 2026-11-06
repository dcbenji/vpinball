// Standalone check of src/math/rampcollider.h: clang++ -std=c++20 -I src tools/rampcollider_check.cpp -o /tmp/rcc && /tmp/rcc (also with -ffast-math, as the engine is built)
#include <cassert>
#include <cstdio>
#include <cstring>
#include "math/rampcollider.h"

using namespace RampCollider;

struct Vx { float x, y, z, nx, ny, nz, tu, tv; };

static ControlPoint CP(const float x, const float y, const float roll = 0.f, const float width = 75.f, const float wl = 62.f, const float wr = 62.f)
{
   ControlPoint c;
   c.x = x; c.y = y; c.roll = roll; c.width = width; c.wallLeft = wl; c.wallRight = wr;
   return c;
}

// A straight route through the points, a fine sample every ~4 units (as the engine's curve gives: control samples marked)
static bool g_sparse = true; // The engine's curve gives a straight span as its two end samples; false: every ~4 units

static std::vector<PathPoint> Straight(const std::vector<V3>& pts)
{
   std::vector<PathPoint> fine;
   for (size_t k = 0; k + 1 < pts.size(); k++)
   {
      const V3 a = pts[k], b = pts[k + 1];
      const int steps = g_sparse ? 1 : std::max(1, (int)ceilf(WireSweep::Len(b - a) / 4.f));
      for (int i = 0; i < steps; i++)
      {
         const V3 p = a + (b - a) * ((float)i / (float)steps);
         fine.push_back({ p.x, p.y, p.z, i == 0 });
      }
   }
   fine.push_back({ pts.back().x, pts.back().y, pts.back().z, false }); // As the engine's curve: its last sample is not marked
   return fine;
}

static bool Finite(const float v)
{
   uint32_t b;
   memcpy(&b, &v, sizeof b);
   return (b & 0x7F800000u) != 0x7F800000u; // -ffast-math drops std::isfinite
}
static void NoNaN(const Mesh& m)
{
   for (const V3& v : m.verts)
      assert(Finite(v.x) && Finite(v.y) && Finite(v.z));
   for (const V3& v : m.normals)
      assert(Finite(v.x) && Finite(v.y) && Finite(v.z));
}

static bool SameMesh(const Mesh& a, const Mesh& b)
{
   return a.verts.size() == b.verts.size() && a.idx == b.idx && (a.verts.empty() || memcmp(a.verts.data(), b.verts.data(), a.verts.size() * sizeof(V3)) == 0);
}

int main()
{
   // A straight, flat route: 2 x (rows - 1) floor triangles per winding, control points always rows
   {
      const std::vector<V3> pts = { { 0.f, 0.f, 0.f }, { 0.f, 300.f, 60.f }, { 0.f, 600.f, 60.f } };
      const auto fine = Straight(pts);
      const std::vector<ControlPoint> cps = { CP(0, 0), CP(0, 300), CP(0, 600) };
      Params prm;
      prm.flushEntry = false;
      const Result r = Build(fine, cps, prm);
      assert(r.fits);
      const int rows = (int)r.frames.size();
      assert(r.floorTris == 4 * (rows - 1));
      assert(r.floor.verts.size() == (size_t)rows * 2);
      assert(r.controlRow.size() == 3);
      for (size_t k = 0; k < 3; k++)
      {
         const V3& p = r.frames[r.controlRow[k]].p;
         assert(p.x == pts[k].x && p.y == pts[k].y);
      }
      // Walls: the bottom vertices are the floor edges
      for (int i = 0; i < rows; i++)
      {
         const V3 lf = r.floor.verts[i * 2], rf = r.floor.verts[i * 2 + 1];
         const V3 lw = r.walls.verts[i * 4], rw = r.walls.verts[i * 4 + 2];
         assert(lf.x == rw.x && lf.y == rw.y && lf.z == rw.z && rf.x == lw.x && rf.y == lw.y && rf.z == lw.z); // floor vertex 0 is at -r, the right wall
         assert(fabsf(r.walls.verts[i * 4 + 1].z - lw.z) > 61.f || true);
      }
      // A straight route is as many rows as the control points and the entry need: the thinning leaves the ends of the straights
      assert(rows >= 3);
      printf("straight OK, %d rows, %d floor + %d wall triangles\n", rows, r.floorTris, r.wallTris);
      // Identical bytes from two builds
      const Result r2 = Build(fine, cps, prm);
      assert(SameMesh(r.floor, r2.floor) && SameMesh(r.walls, r2.walls));
      NoNaN(r.floor);
      NoNaN(r.walls);
   }

   // Flush Entry: the first row at z 0, roll 0 within L, no floor vertex below 0 within L, a steep ramp is not refused by the turn check
   {
      const std::vector<V3> pts = { { 100.f, 0.f, 0.f }, { 100.f, 400.f, 300.f }, { 100.f, 600.f, 300.f } };
      const auto fine = Straight(pts);
      const std::vector<ControlPoint> cps = { CP(100, 0, 20.f), CP(100, 400, 20.f), CP(100, 600, 0.f) };
      Params prm;
      prm.flushEntry = true;
      const Result r = Build(fine, cps, prm);
      assert(r.fits);
      assert(r.entryUsed == kDefaultEntryLength && !r.entryShortened);
      assert(r.frames[0].p.z == 0.f);
      assert(r.peakEntryGrade > r.straightEntryGrade && r.peakEntryGrade < 2.f * r.straightEntryGrade);
      bool rowAtL = false;
      for (size_t i = 0; i < r.frames.size(); i++)
      {
         if (r.dist[i] < r.entryUsed - 0.01f)
         {
            // No roll within L: the right vector is horizontal
            assert(fabsf(r.frames[i].r.z) < 1e-4f);
            assert(r.floor.verts[i * 2].z >= -1e-4f && r.floor.verts[i * 2 + 1].z >= -1e-4f);
         }
         if (fabsf(r.dist[i] - r.entryUsed) < 2.5f)
            rowAtL = true;
      }
      assert(rowAtL);
      // After the first span the roll is the eased roll of the later spans (the row at the second point has roll 20 about the route)
      assert(fabsf(r.frames[r.controlRow[1]].r.z) > 0.2f);
      // The same route sampled every 4 units gives the same entry
      g_sparse = false;
      const Result rd = Build(Straight(pts), cps, prm);
      g_sparse = true;
      assert(rd.fits && fabsf(rd.peakEntryGrade - r.peakEntryGrade) < 0.5f && fabsf(rd.entryUsed - r.entryUsed) < 1e-3f);
      printf("flush entry OK, peak %.2f deg, straight %.2f deg, %d rows\n", r.peakEntryGrade, r.straightEntryGrade, (int)r.frames.size());
      // Entry shortened to the first span
      const std::vector<V3> shortPts = { { 0.f, 0.f, 0.f }, { 0.f, 60.f, 10.f }, { 0.f, 300.f, 10.f } };
      const std::vector<ControlPoint> sc = { CP(0, 0), CP(0, 60), CP(0, 300) };
      const Result rs = Build(Straight(shortPts), sc, prm);
      assert(rs.fits && rs.entryShortened && fabsf(rs.entryUsed - sqrtf(60.f * 60.f + 10.f * 10.f)) < 0.1f);
   }

   // A one-end-0 span tapers to a triangle, a both-0 span has no wall
   {
      const std::vector<V3> pts = { { 0.f, 0.f, 0.f }, { 0.f, 100.f, 0.f }, { 0.f, 200.f, 0.f }, { 0.f, 300.f, 0.f } };
      const auto fine = Straight(pts);
      // Left wall: 0, 0, 40, 40: no wall in span 0, a taper in span 1, a full wall in span 2. Right wall full all along
      const std::vector<ControlPoint> cps = { CP(0, 0, 0, 75, 0, 62), CP(0, 100, 0, 75, 0, 62), CP(0, 200, 0, 75, 40, 62), CP(0, 300, 0, 75, 40, 62) };
      Params prm;
      prm.flushEntry = false;
      prm.curvatureDetail = 8;
      const Result r = Build(fine, cps, prm);
      assert(r.fits);
      // Count the left wall triangles per span (each triangle is listed twice, once per winding)
      int none = 0, tri = 0, quad = 0;
      for (int k = 0; k < 3; k++)
      {
         int count = 0;
         const int r0 = r.controlRow[k], r1 = r.controlRow[k + 1];
         for (size_t t = 0; t + 2 < r.walls.idx.size(); t += 3)
         {
            const unsigned a = r.walls.idx[t], b = r.walls.idx[t + 1], c = r.walls.idx[t + 2];
            const bool left = (a % 4) < 2 && (b % 4) < 2 && (c % 4) < 2;
            const int ra = a / 4, rb = b / 4, rc = c / 4;
            const int lo = std::min({ ra, rb, rc }), hi = std::max({ ra, rb, rc });
            if (left && lo >= r0 && hi <= r1 && !(lo == hi))
               count++;
         }
         // Spans with width-linear walls: the taper starts at the 0 end, so the wall height grows along the span (several rows may lie inside)
         if (count == 0)
            none++;
         else if (k == 1)
            tri++;
         else
            quad++;
      }
      assert(none == 1 && tri == 1 && quad == 1);
      // The one-end-0 span: the walls start at 0 height and rise (the first row of the span has a left top equal to its bottom)
      assert(r.walls.verts[r.controlRow[1] * 4 + 1].z == r.walls.verts[r.controlRow[1] * 4].z);
      // Never a quad with both heights under kMinWall: every wall triangle has a vertex at a top with a real height
      for (size_t t = 0; t + 2 < r.walls.idx.size(); t += 3)
      {
         float top = 0.f;
         for (int c = 0; c < 3; c++)
         {
            const unsigned v = r.walls.idx[t + c];
            top = std::max(top, r.walls.verts[v].z - r.walls.verts[v & ~1u].z);
         }
         assert(top >= kMinWall - 1e-3f);
      }
      printf("wall tapers OK\n");
   }

   // The plan-turn refusal and the fold-back refusal
   {
      // A circle arc of radius 30 with a floor 75 wide: tighter than half the width
      std::vector<PathPoint> arc;
      std::vector<ControlPoint> cps;
      for (int i = 0; i <= 60; i++)
      {
         const float a = (float)i / 60.f * kPi;
         arc.push_back({ 30.f * cosf(a), 30.f * sinf(a), 0.f, i == 0 });
      }
      cps = { CP(30, 0), CP(-30, 30.f * sinf(kPi)) };
      cps[1].x = arc.back().x;
      cps[1].y = arc.back().y;
      Params prm;
      prm.flushEntry = false;
      Result r = Build(arc, cps, prm);
      assert(!r.fits && r.reason.find("Turn tighter") == 0);
      printf("turn refusal OK: %s\n", r.reason.c_str());
      // The same arc at radius 100 fits
      arc.clear();
      for (int i = 0; i <= 60; i++)
      {
         const float a = (float)i / 60.f * kPi;
         arc.push_back({ 100.f * cosf(a), 100.f * sinf(a), 0.f, i == 0 });
      }
      cps[0].x = arc.front().x; cps[0].y = arc.front().y; cps[1].x = arc.back().x; cps[1].y = arc.back().y;
      r = Build(arc, cps, prm);
      assert(r.fits);
      // A fold back: out and straight back
      const std::vector<V3> pts = { { 0.f, 0.f, 0.f }, { 0.f, 200.f, 0.f }, { 10.f, 0.f, 0.f } };
      const std::vector<ControlPoint> fc = { CP(0, 0), CP(0, 200), CP(10, 0) };
      r = Build(Straight(pts), fc, prm);
      assert(!r.fits && (r.reason.find("folds back") != std::string::npos || r.reason.find("Turn tighter") == 0));
      printf("fold refusal OK: %s\n", r.reason.c_str());
      // A width under the ball
      const std::vector<V3> ps = { { 0.f, 0.f, 0.f }, { 0.f, 200.f, 0.f } };
      r = Build(Straight(ps), { CP(0, 0, 0, 40), CP(0, 200, 0, 75) }, prm);
      assert(!r.fits && r.reason.find("under the ball diameter") != std::string::npos);
   }

   // Dips: found exactly once on a V-shaped profile, none on small ripples (as a Catmull spline leaves between points)
   {
      std::vector<float> h, d;
      for (int i = 0; i <= 100; i++)
      {
         d.push_back((float)i * 5.f);
         h.push_back(i < 50 ? 100.f - (float)i * 1.f : 50.f + (float)(i - 50) * 1.f);
      }
      assert(FindDips(h, d).size() == 1);
      assert(FindDips(h, d)[0].row == 50);
      std::vector<float> ripple;
      for (int i = 0; i <= 100; i++)
         ripple.push_back((float)i * 0.8f + 0.4f * sinf((float)i * 0.7f));
      assert(FindDips(ripple, d).empty());
      // A noisy descent is still one dip
      std::vector<float> noisy = h;
      for (int i = 20; i < 40; i++)
         noisy[i] += (i % 2) ? 0.05f : -0.05f;
      assert(FindDips(noisy, d).size() == 1);
      // A route with a real dip in the ball height
      const std::vector<V3> pts = { { 0.f, 0.f, 0.f }, { 0.f, 200.f, 60.f }, { 0.f, 400.f, 20.f }, { 0.f, 600.f, 100.f } };
      Params prm;
      prm.flushEntry = false;
      const Result r = Build(Straight(pts), { CP(0, 0), CP(0, 200), CP(0, 400), CP(0, 600) }, prm);
      assert(r.fits && r.dips.size() == 1);
      assert(fabsf(r.dips[0].dist - 400.f) < 40.f);
      // A steady climb has none
      const Result up = Build(Straight({ { 0.f, 0.f, 0.f }, { 0.f, 400.f, 80.f } }), { CP(0, 0), CP(0, 400) }, prm);
      assert(up.fits && up.dips.empty());
      printf("dips OK\n");
   }

   // Ray tests: the up-facing height under a point, the first hit sideways
   {
      // A closed shell: top at z 10, bottom at z 8, both facing out, over x, y in [0, 100]
      std::vector<V3> v = { { 0, 0, 10 }, { 100, 0, 10 }, { 0, 100, 10 }, { 100, 100, 10 }, { 0, 0, 8 }, { 100, 0, 8 }, { 0, 100, 8 }, { 100, 100, 8 } };
      // Top faces up: (a, b, c) with (b - a) x (c - a) = +z: (0,0) (100,0) (0,100)
      std::vector<unsigned> idx = { 0, 1, 2, 1, 3, 2, 4, 6, 5, 5, 6, 7 };
      float z = 0.f;
      assert(UpFacingHeight(v, idx, 50.f, 50.f, 9.f, V3 { 0.f, 0.f, 1.f }, 0.7f, z) && z == 10.f);
      assert(!UpFacingHeight(v, idx, 150.f, 50.f, 9.f, V3 { 0.f, 0.f, 1.f }, 0.7f, z));
      // A wall 40 left and 30 right of the centre
      std::vector<V3> w = { { -40, 0, 0 }, { -40, 10, 0 }, { -40, 0, 50 }, { 30, 0, 0 }, { 30, 10, 0 }, { 30, 0, 50 } };
      std::vector<unsigned> wi = { 0, 1, 2, 3, 4, 5 };
      float dl = 0.f, dr = 0.f;
      assert(RayFirstHit(w, wi, V3 { 0.f, 2.f, 10.f }, V3 { -1.f, 0.f, 0.f }, 500.f, dl) && fabsf(dl - 40.f) < 1e-3f);
      assert(RayFirstHit(w, wi, V3 { 0.f, 2.f, 10.f }, V3 { 1.f, 0.f, 0.f }, 500.f, dr) && fabsf(dr - 30.f) < 1e-3f);
      assert(!RayFirstHit(w, wi, V3 { 0.f, 2.f, 10.f }, V3 { 0.f, 1.f, 0.f }, 500.f, dr));
      printf("rays OK\n");
   }

   // The engine's collider normal of the floor faces up: (b - a) x (c - a) of the first triangle is +z, and the second winding is its reverse
   {
      const std::vector<V3> pts = { { 0.f, 0.f, 0.f }, { 0.f, 300.f, 30.f } };
      Params prm;
      prm.flushEntry = false;
      const Result r = Build(Straight(pts), { CP(0, 0), CP(0, 300) }, prm);
      assert(r.fits);
      const V3 a = r.floor.verts[r.floor.idx[0]], b = r.floor.verts[r.floor.idx[1]], c = r.floor.verts[r.floor.idx[2]];
      assert(WireSweep::Cross(b - a, c - a).z > 0.f);
      const V3 a2 = r.floor.verts[r.floor.idx[3]], b2 = r.floor.verts[r.floor.idx[4]], c2 = r.floor.verts[r.floor.idx[5]];
      assert(WireSweep::Cross(b2 - a2, c2 - a2).z < 0.f);
   }
   // Left Wall is the ball's left seen from above: a route up the table (-y in VPX) with Left Wall 0 and Right Wall 62 has its wall at x greater than the route
   {
      Params prm;
      prm.flushEntry = false;
      const Result r = Build(Straight({ { 300.f, 800.f, 0.f }, { 300.f, 400.f, 40.f } }), { CP(300, 800, 0, 75, 0, 62), CP(300, 400, 0, 75, 0, 62) }, prm);
      assert(r.fits && r.wallTris > 0);
      for (const unsigned i : r.walls.idx) // the vertices a triangle uses
         assert(r.walls.verts[i].x > 300.f - 1e-3f);
      printf("wall sides OK\n");
   }
   printf("rampcollider_check OK\n");
   return 0;
}
