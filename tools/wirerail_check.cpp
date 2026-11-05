// Standalone check of src/math/wirerail.h: clang++ -std=c++20 -I src tools/wirerail_check.cpp -o /tmp/wrc && /tmp/wrc (also with -ffast-math, as the engine is built)
#include <cassert>
#include <cstdio>
#include <cstring>
#include "math/wirerail.h"

using namespace WireRail;

struct Vx { float x, y, z, nx, ny, nz, tu, tv; };

static float DistToSegment(V2 p, V2 a, V2 b)
{
   const V2 d = b - a;
   const float l2 = Dot(d, d);
   const float t = l2 > 0.f ? std::clamp(Dot(p - a, d) / l2, 0.f, 1.f) : 0.f;
   return Len(p - (a + d * t));
}

// The ball resting on a profile touches its facets and never crosses one
static void CheckProfile(const Profile& p, const float ballR, const int minTouch)
{
   int touching = 0;
   for (const auto& s : p.spans)
   {
      const float d = DistToSegment(p.ball, p.verts[s.first], p.verts[s.second]);
      assert(d > ballR - 0.01f);
      if (d < ballR + 0.01f)
         touching++;
   }
   assert(touching >= minTouch);
}

static std::vector<Frame> Route(const std::vector<PathPoint>& fine, const std::vector<float>& rolls = {}, const int detail = 8) { return BuildFrames(BuildRows(fine, rolls, detail)); }

int main()
{
   const float wireR = kDefaultWireDiameter * 0.5f, ballR = kDefaultBallDiameter * 0.5f;
   std::string err;

   // Presets: freezy's table
   assert(DefaultWires(1, true).size() == 1 && DefaultWires(1, true)[0].x == 0.f);
   assert(DefaultWires(3, false)[2].x == -30.f && DefaultWires(3, true)[2].x == 30.f && DefaultWires(3, true)[2].z == 30.f);
   assert(DefaultWires(4, true)[3].x == 30.f && DefaultWires(4, true)[3].z == 30.f);
   assert(DefaultWires(5, true)[4].z == 60.f && DefaultWires(5, true)[4].x == 0.f);
   assert(DefaultWires(6, true)[5].x == 15.f && DefaultWires(6, true)[5].z == 60.f);
   // Rails changed: presets relay, customized wires stay
   {
      auto w = ResizeWires(DefaultWires(4, true), 2, true);
      assert(w.size() == 2 && w[1].x == 15.f);
      w = ResizeWires(DefaultWires(2, true), 1, true);
      assert(w.size() == 1 && w[0].x == 0.f);
      w = DefaultWires(4, true);
      w[0].x = -18.f;
      w = ResizeWires(w, 6, true);
      assert(w.size() == 6 && w[0].x == -18.f && w[5].z == 60.f);
   }

   // Fits, from one wire to six
   for (int count = 1; count <= 6; count++)
   {
      const auto wires = DefaultWires(count, true);
      Profile p;
      const bool ok = FitProfile(wires, wireR, ballR, p, err);
      if (!ok)
         printf("rails %d: %s\n", count, err.c_str());
      assert(ok);
      assert(p.verts.size() >= 2 && !p.spans.empty());
      for (const auto& s : p.spans)
         assert(s.first < (int)p.verts.size() && s.second < (int)p.verts.size());
      assert(p.closed == (count >= 5)); // The defaults at 5 and 6: the top gap is narrower than the ball, so a roof
      CheckProfile(p, ballR, count == 1 ? 1 : 2);
      printf("rails %d: ball at (%.2f, %.2f), %d facets\n", count, p.ball.x, p.ball.y, (int)p.spans.size());
   }
   {
      // Habitrail: the numbers by hand. Ball centre on the two bottom wires: sqrt(28.25^2 - 15^2) = 23.94 above them
      Profile p;
      assert(FitProfile(DefaultWires(4, true), wireR, ballR, p, err));
      assert(fabsf(p.ball.x) < 1e-3f && fabsf(p.ball.y - 23.94f) < 0.01f);
      assert(p.spans.size() == 7 && !p.closed); // 4 contact facets and 3 chamfers, open at the top
      // Symmetric, floor under the ball
      float lowest = 1e9f;
      for (const V2& v : p.verts)
         lowest = fminf(lowest, v.y);
      assert(fabsf(lowest + 1.06f) < 0.8f);
      float top = -1e9f, wide = 0.f;
      for (const V2& v : p.verts)
      {
         top = fmaxf(top, v.y);
         wide = fmaxf(wide, fabsf(v.x));
      }
      assert(top > p.ball.y && wide < 32.f); // The walls stand above the ball's centre, inside the raised wires
   }
   {
      // A wide open top passes the ball: no roof
      auto w = DefaultWires(6, true);
      w[4].x = -40.f;
      w[5].x = 40.f;
      w[2].x = -40.f;
      w[3].x = 40.f;
      Profile p;
      assert(FitProfile(w, wireR, ballR, p, err));
      assert(!p.closed);
   }
   {
      // What can't fit says so
      Profile p;
      auto w = DefaultWires(2, true);
      w[1].x = 60.f;
      assert(!FitProfile(w, wireR, ballR, p, err) && err.find("falls between") != std::string::npos);
      w = DefaultWires(2, true);
      w[1] = w[0];
      assert(!FitProfile(w, wireR, ballR, p, err) && err.find("same place") != std::string::npos);
      w = DefaultWires(4, true);
      w[2].x = -10.f;
      w[2].z = 10.f;
      assert(!FitProfile(w, wireR, ballR, p, err) && err.find("inside the ball") != std::string::npos);
      w = DefaultWires(2, true);
      w[0].active = w[1].active = false;
      assert(!FitProfile(w, wireR, ballR, p, err) && err.find("No wire") != std::string::npos);
      // A wire turned off: the fit is the rail without it
      w = DefaultWires(4, true);
      w[3].active = false;
      assert(FitProfile(w, wireR, ballR, p, err));
      assert(p.spans.size() == 5);
   }

   // Frames: flat, rising, roll
   {
      const auto f = Route({ { 0, 0, 10, true }, { 0, 500, 10, false } });
      assert(f.size() == 2);
      assert(fabsf(f[0].r.x - 1.f) < 1e-5f && fabsf(f[0].u.z - 1.f) < 1e-5f); // +Y travel has +X on its right, as in VPE
      const auto up = Route({ { 0, 0, 0, true }, { 0, 100, 100, false } });
      assert(fabsf(up[0].r.x - 1.f) < 1e-5f && up[0].u.z > 0.7f && up[0].u.y < -0.7f); // Upright: right stays flat, up leans with the slope
      const auto roll = Route({ { 0, 0, 0, true }, { 0, 500, 0, false } }, { 0.f, 30.f });
      assert(roll.size() > 4); // The roll gets rows on a straight
      const Frame& e = roll.back();
      assert(fabsf(e.r.z - 0.5f) < 1e-3f); // Positive raises the right side
      assert(fabsf(WireSweep::Dot(e.r, e.u)) < 1e-4f && fabsf(WireSweep::Dot(e.t, e.u)) < 1e-4f);
      // Through the vertical: the frame stays finite and square
      const auto drop = Route({ { 0, 0, 200, true }, { 0, 0.001f, 100, false }, { 0, 100, 0, false } });
      for (const Frame& a : drop)
         assert(fabsf(WireSweep::Len(a.r) - 1.f) < 1e-3f && fabsf(WireSweep::Len(a.u) - 1.f) < 1e-3f);
      // A loop over the top (the route goes up, past the vertical, and back): the right side stays on the same side, the rows never twist
      std::vector<PathPoint> loop;
      for (int i = 0; i <= 200; i++)
      {
         const float a = 3.4906585f * (float)i / 200.f; // 200 degrees
         loop.push_back({ 0.f, 100.f * sinf(a), 100.f * (1.f - cosf(a)), i == 0 });
      }
      const auto lf = Route(loop, { 0.f, 0.f }, 16);
      assert(lf.size() > 8);
      for (size_t i = 0; i < lf.size(); i++)
      {
         assert(lf[i].r.x > 0.99f); // Plan right stays +X all the way round
         if (i > 0)
            assert(WireSweep::Dot(lf[i].r, lf[i - 1].r) > 0.99f && WireSweep::Dot(lf[i].u, lf[i - 1].u) > 0.5f);
      }
   }
   // Rows: a straight stays two rows, a turn gets a row at most every 5.6 degrees
   {
      std::vector<PathPoint> arc;
      const int fine = 720;
      for (int i = 0; i <= fine; i++)
      {
         const float a = 1.5707963f * (float)i / (float)fine;
         arc.push_back({ 100.f * cosf(a), 100.f * sinf(a), 0.f, i == 0 });
      }
      const auto rows = BuildRows(arc, { 0.f, 0.f }, 8);
      assert(rows.size() > 10 && rows.size() < 40);
      for (size_t i = 2; i < rows.size(); i++)
      {
         const V3 a = WireSweep::Norm(rows[i - 1].p - rows[i - 2].p), b = WireSweep::Norm(rows[i].p - rows[i - 1].p);
         assert(acosf(std::clamp(WireSweep::Dot(a, b), -1.f, 1.f)) < 7.f * 3.14159f / 180.f);
      }
      assert(fabsf(MinRadius(BuildFrames(rows)) - 100.f) < 8.f);
      assert(BuildRows({ { 0, 0, 0, true }, { 0, 250, 0, true }, { 0, 500, 0, false } }, { 0.f, 0.f, 0.f }, 8).size() == 2);
      // Curvature Detail: more rows
      assert(BuildRows(arc, { 0.f, 0.f }, 16).size() > rows.size());
   }
   // Grade: straight by plan distance, ends and other spans untouched
   {
      std::vector<float> z { 10, 99, 99, 70, 5 };
      const std::vector<float> plan { 0, 100, 400, 500, 600 };
      assert(Grade(plan, z, 0, 3));
      assert(z[0] == 10.f && fabsf(z[1] - 22.f) < 1e-3f && fabsf(z[2] - 58.f) < 1e-3f && z[3] == 70.f && z[4] == 5.f);
      assert(!Grade(plan, z, 3, 3) && !Grade(plan, z, 3, 9));
   }

   // Tubes: outward winding, unit normals, indices in range, chamfered and closed ends
   {
      const auto frames = Route({ { 0, 0, 0, true }, { 0, 500, 0, false } });
      std::vector<Vx> v;
      std::vector<unsigned> idx;
      assert(BuildTubes<Vx>(frames, DefaultWires(4, true), wireR, kDefaultSides, kDefaultCapBevel, v, idx));
      for (unsigned k : idx)
         assert(k < v.size());
      for (const Vx& a : v)
         assert(fabsf(sqrtf(a.nx * a.nx + a.ny * a.ny + a.nz * a.nz) - 1.f) < 1e-3f);
      float minY = 1e9f, maxY = -1e9f;
      for (const Vx& a : v)
      {
         minY = fminf(minY, a.y);
         maxY = fmaxf(maxY, a.y);
      }
      assert(fabsf(minY) < 1e-3f && fabsf(maxY - 500.f) < 1e-3f); // The chamfer ends at the end of the route
      int bad = 0;
      for (size_t t = 0; t < idx.size(); t += 3)
      {
         const Vx &a = v[idx[t]], &b = v[idx[t + 1]], &c = v[idx[t + 2]];
         const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
         const float cx = uy * wz - uz * wy, cy = uz * wx - ux * wz, cz = ux * wy - uy * wx;
         const float area = sqrtf(cx * cx + cy * cy + cz * cz);
         if (area > 1e-5f && (cx * a.nx + cy * a.ny + cz * a.nz) <= 0.f)
            bad++;
      }
      if (bad)
         printf("%d triangles wound inward of %d\n", bad, (int)(idx.size() / 3));
      assert(bad == 0);
      // Same winding as the wire guide's sweep
      std::vector<Vx> sv;
      std::vector<unsigned> si;
      struct P { float x, y, z; };
      const std::vector<P> line { { 0, 0, 0 }, { 0, 500, 0 } };
      WireSweep::SweepTube<Vx>(line, wireR, 8, WireSweep::Caps::None, sv, si);
      const auto signOf = [](const std::vector<Vx>& vv, const std::vector<unsigned>& ii)
      {
         const Vx &a = vv[ii[0]], &b = vv[ii[1]], &c = vv[ii[2]];
         const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
         return ((uy * wz - uz * wy) * a.nx + (uz * wx - ux * wz) * a.ny + (ux * wy - uy * wx) * a.nz) > 0.f;
      };
      assert(signOf(sv, si) == signOf(v, idx));
      // Only the active wires
      v.clear();
      idx.clear();
      auto w = DefaultWires(4, true);
      w[2].active = w[3].active = false;
      BuildTubes<Vx>(frames, w, wireR, kDefaultSides, kDefaultCapBevel, v, idx);
      std::vector<Vx> v2;
      std::vector<unsigned> i2;
      BuildTubes<Vx>(frames, DefaultWires(2, true), wireR, kDefaultSides, kDefaultCapBevel, v2, i2);
      assert(v.size() == v2.size() && idx.size() == i2.size());
   }

   // Channel: rows of the profile, two sided, edges, the ball lying on the floor along a sloped banked turn
   {
      Profile p;
      assert(FitProfile(DefaultWires(4, true), wireR, ballR, p, err));
      std::vector<PathPoint> arc;
      for (int i = 0; i <= 720; i++)
      {
         const float a = 1.5707963f * (float)i / 720.f;
         arc.push_back({ 300.f * cosf(a), 300.f * sinf(a), 100.f - 40.f * (float)i / 720.f, i == 0 });
      }
      const auto frames = Route(arc, { 10.f, 10.f });
      const Channel c = BuildChannel(frames, p);
      assert(c.rows == (int)frames.size() && c.rowSize == (int)p.verts.size());
      assert((int)c.verts.size() == c.rows * c.rowSize);
      assert((int)c.tris.size() == (c.rows - 1) * (int)p.spans.size() * 12);
      for (unsigned k : c.tris)
         assert(k < c.verts.size());
      assert((int)c.edges.size() == (c.rows - 1) * c.rowSize);
      // The refresh decision of a support edit: the same inputs give an identical channel, a moved vertex does not
      Channel c2 = BuildChannel(frames, p);
      assert(SameChannel(c, c2) && !SameChannel(c, Channel {}));
      c2.verts[3].z += 0.01f;
      assert(!SameChannel(c, c2));
      for (const auto& e : c.edges)
         assert(e.first < c.verts.size() && e.second < c.verts.size());
      // A wire centreline stays a wire radius from its facet contact on every row
      const auto wire = WireCenterline(frames, DefaultWires(4, true)[0]);
      for (size_t i = 0; i < frames.size(); i++)
         assert(WireSweep::Len(wire[i] - frames[i].p) > 14.f && WireSweep::Len(wire[i] - frames[i].p) < 16.f);
      printf("channel: %d rows, %d verts, %d tris\n", c.rows, (int)c.verts.size(), (int)c.tris.size() / 3);
   }

   // Fixtures (phase 2a): freezy's supports and their solder blobs
   {
      std::vector<PathPoint> line;
      for (int i = 0; i <= 50; i++)
         line.push_back({ 0.f, 10.f * (float)i, 0.f, i == 0 });
      const auto frames = Route(line);
      const auto rowDist = RowDistances(frames);
      const auto wires = DefaultWires(4, true);
      Profile profile;
      assert(FitProfile(wires, wireR, ballR, profile, err));
      const auto build = [&](const std::vector<Fixture>& fx, std::vector<Vx>& v, std::vector<unsigned>& i, std::vector<FixtureShape>& sh, const std::vector<Wire>& w = DefaultWires(4, true))
      {
         v.clear();
         i.clear();
         BuildFixtures<Vx>(frames, rowDist, w, wireR, kDefaultSides, kDefaultCapBevel, ballR, profile, fx, v, i, sh);
      };
      const auto finite = [](const std::vector<Vx>& v)
      {
         for (const Vx& x : v)
            if (!std::isfinite(x.x) || !std::isfinite(x.y) || !std::isfinite(x.z) || !std::isfinite(x.nx) || !std::isfinite(x.ny) || !std::isfinite(x.nz))
               return false;
         return true;
      };
      std::vector<Vx> v;
      std::vector<unsigned> idx;
      std::vector<FixtureShape> sh;
      const auto section = [&](const V3& p) { return V2 { p.x, p.z }; }; // The route runs along +Y at z 0: x is right, z is up

      // Ring at the defaults: the circle round the wires (centre (0, 15), radius 40.04), touching wires 3 and 4 only
      build({ DefaultFixture(kFixtureRing, 250.f) }, v, idx, sh);
      assert(sh[0].built && sh[0].reason.empty());
      {
         V2 c { 0.f, 0.f };
         for (size_t k = 0; k + 1 < sh[0].centerline.size(); k++)
            c = c + section(sh[0].centerline[k]);
         c = c * (1.f / (float)(sh[0].centerline.size() - 1));
         assert(fabsf(c.x) < 0.01f && fabsf(c.y - 15.f) < 0.01f);
         for (const V3& p : sh[0].centerline)
            assert(fabsf(Len(section(p) - V2 { 0.f, 15.f }) - 40.04f) < 0.01f);
         // Blobs: the vertices after the tube (32 rings of 10), 108 each. Wires 3 and 4
         const int tubeVerts = 32 * kDefaultSides;
         assert((int)v.size() == tubeVerts + 2 * 3 * kTrianglesPerBlob);
         assert(sh[0].triangles == 640 + 2 * kTrianglesPerBlob); // Whole ring 640 triangles, no caps (a tube with caps has more vertices)
         bool hit3 = false, hit4 = false;
         for (int b = 0; b < 2; b++)
         {
            V2 cen { 0.f, 0.f };
            for (int k = 0; k < 3 * kTrianglesPerBlob; k++)
               cen = cen + V2 { v[tubeVerts + b * 3 * kTrianglesPerBlob + k].x, v[tubeVerts + b * 3 * kTrianglesPerBlob + k].z };
            cen = cen * (1.f / (float)(3 * kTrianglesPerBlob));
            assert(Len(cen - V2 { -30.f, 30.f }) < 8.f || Len(cen - V2 { 30.f, 30.f }) < 8.f);
            hit3 |= Len(cen - V2 { -30.f, 30.f }) < 8.f;
            hit4 |= Len(cen - V2 { 30.f, 30.f }) < 8.f;
         }
         assert(hit3 && hit4);
         // The ring clears wires 1 and 2 by 12.3
         const float gap = 40.04f - Len(V2 { -15.f, 0.f } - V2 { 0.f, 15.f }) - 2.f * wireR;
         assert(fabsf(gap - 12.33f) < 0.05f);
         assert(!sh[0].inBallPath); // 40 round the wires, the ball sits well inside
      }
      // An open arc (Cutout on) has caps on both ends: more vertices than its rings
      {
         Fixture f = DefaultFixture(kFixtureRing, 250.f);
         f.cutout = true;
         build({ f }, v, idx, sh);
         assert(sh[0].built);
         const int rings = (int)sh[0].centerline.size();
         assert((int)v.size() > rings * kDefaultSides + 2 * 3 * kTrianglesPerBlob - 1);
         // Straight Line: the chord between its two angles, so the centreline has points off the circle, never outside it
         Fixture g = DefaultFixture(kFixtureRing, 250.f);
         g.straight = true;
         build({ g }, v, idx, sh);
         assert(sh[0].built);
         bool inside = false;
         for (const V3& p : sh[0].centerline)
         {
            const float r = Len(section(p) - V2 { 0.f, 15.f });
            assert(r < 40.05f);
            inside |= r < 38.f;
         }
         assert(inside);
         // Level keeps the width and recentres on up or down
         const V2 l = LevelAngleRange(60.f, 120.f);
         assert(fabsf(l.x - 60.f) < 1e-3f && fabsf(l.y - 120.f) < 1e-3f);
         const V2 l2 = LevelAngleRange(200.f, 260.f);
         assert(fabsf(l2.x - 240.f) < 1e-2f && fabsf(l2.y - 300.f) < 1e-2f); // Centre 230 is nearer 270 than 90: 270 +- 30
      }
      // Rung: x from -11.75 to 11.75 at z 0, length 23.5; blobs on wires 1 and 2
      {
         build({ DefaultFixture(kFixtureRung, 100.f) }, v, idx, sh);
         assert(sh[0].built);
         const V2 a = section(sh[0].centerline[0]), b = section(sh[0].centerline[1]);
         assert(fabsf(a.x + 11.75f) < 0.001f && fabsf(b.x - 11.75f) < 0.001f && fabsf(a.y) < 0.001f && fabsf(b.y) < 0.001f);
         assert(fabsf(Len(b - a) - 23.5f) < 0.001f);
         assert(sh[0].inBallPath); // At the height of the wire centres: the ball sits 4.3 into it
         Fixture low = DefaultFixture(kFixtureRung, 100.f);
         low.offsetZ = -4.9f;
         build({ low }, v, idx, sh);
         assert(!sh[0].inBallPath);
         // Not built with wire 1 or 2 off
         auto w = DefaultWires(4, true);
         w[1].active = false;
         build({ DefaultFixture(kFixtureRung, 100.f) }, v, idx, sh, w);
         assert(!sh[0].built && !sh[0].reason.empty());
         build({ DefaultFixture(kFixtureStand, 100.f) }, v, idx, sh, w);
         assert(!sh[0].built && !sh[0].reason.empty());
      }
      // Cradle: its arms 6.5 from the centres of wires 1 to 4
      {
         build({ DefaultFixture(kFixtureCradle, 100.f) }, v, idx, sh);
         assert(sh[0].built);
         for (int w = 0; w < 4; w++)
         {
            const V2 wc { wires[w].x, wires[w].z };
            float best = 1e9f;
            for (size_t k = 0; k + 1 < sh[0].centerline.size(); k++)
               best = std::min(best, DistToSegment(wc, section(sh[0].centerline[k]), section(sh[0].centerline[k + 1])));
            assert(fabsf(best - 6.5f) < 0.01f);
         }
      }
      // Stand: no repeated points, its first bend at the Wire Diameter, a fold back refused
      {
         build({ DefaultFixture(kFixtureStand, 100.f) }, v, idx, sh);
         assert(sh[0].built);
         const auto& c = sh[0].centerline;
         for (size_t k = 0; k + 1 < c.size(); k++)
            assert(WireSweep::Len(c[k + 1] - c[k]) > 1e-3f);
         // The bend at the end of the attachment: the circle through three consecutive points of its arc
         const float want = 2.f * wireR;
         int checked = 0;
         for (size_t k = 1; k + 1 < c.size(); k++)
         {
            const V3 a = c[k - 1], b = c[k], d = c[k + 1];
            const float ab = WireSweep::Len(b - a), bd = WireSweep::Len(d - b), ad = WireSweep::Len(d - a);
            const float area = 0.5f * WireSweep::Len(WireSweep::Cross(b - a, d - a));
            if (area < 1e-4f || ab < 0.1f || bd < 0.1f)
               continue;
            const float radius = ab * bd * ad / (4.f * area);
            if (fabsf(radius - want) / want < 0.011f)
               checked++;
         }
         assert(checked >= 1); // An arc of the first bend is there at the Wire Diameter (6.5), within 1 percent
         Fixture f = DefaultFixture(kFixtureStand, 100.f);
         f.startVector = { -1.f, 0.f, 0.f }; // Out of the right end back through the attachment
         build({ f }, v, idx, sh);
         assert(!sh[0].built && !sh[0].reason.empty());
      }
      // Determinism, a Position past the end, a disabled fixture, an unknown type, clamps, mirror
      {
         std::vector<Fixture> fx { DefaultFixture(kFixtureRing, 100.f), DefaultFixture(kFixtureRung, 300.f), DefaultFixture(kFixtureCradle, 400.f), DefaultFixture(kFixtureStand, 450.f) };
         std::vector<Vx> v1, v2;
         std::vector<unsigned> i1, i2;
         build(fx, v1, i1, sh);
         build(fx, v2, i2, sh);
         assert(v1.size() == v2.size() && i1 == i2 && memcmp(v1.data(), v2.data(), v1.size() * sizeof(Vx)) == 0);
         assert(finite(v1));
         for (const FixtureShape& s : sh)
            assert(s.built);
         std::vector<Vx> ve;
         std::vector<unsigned> ie;
         build({ DefaultFixture(kFixtureRing, 99999.f) }, v1, i1, sh);
         const float builtAt = sh[0].position;
         build({ DefaultFixture(kFixtureRing, 500.f) }, ve, ie, sh);
         assert(v1.size() == ve.size() && memcmp(v1.data(), ve.data(), v1.size() * sizeof(Vx)) == 0 && builtAt == 500.f && sh[0].position == 500.f);
         Fixture off = DefaultFixture(kFixtureRing, 100.f);
         off.enabled = false;
         build({ off }, v1, i1, sh);
         assert(v1.empty() && sh[0].built && sh[0].triangles == 0 && !sh[0].centerline.empty());
         Fixture unk = DefaultFixture(5, 100.f);
         build({ unk }, v1, i1, sh);
         assert(v1.empty() && !sh[0].built && !sh[0].reason.empty());
         Fixture bad = DefaultFixture(kFixtureCradle, -5.f);
         bad.density = 1000;
         bad.armAngle = 500.f;
         bad.footWidth = -3.f;
         bad.startVector = { 0.f, 0.f, 0.f };
         bad.solderSize = 0.f;
         const uint32_t nanBits = 0x7FC00000u; // (made from its bits: also builds with -ffast-math, where NAN is not allowed)
         memcpy(&bad.scale, &nanBits, sizeof(nanBits));
         ClampFixture(bad);
         assert(bad.position == 0.f && bad.density == 128 && bad.armAngle == 179.f && bad.footWidth == 0.1f && bad.startVector.z == -1.f && bad.solderSize == 0.01f && bad.scale == 1.f);
         Fixture st = DefaultFixture(kFixtureStand, 0.f);
         st.footRotation = { 10.f, 20.f, 30.f };
         const Fixture orig = st;
         MirrorStand(st);
         assert(st.legLeft && st.footPosition.x == -15.f && st.footRotation.y == -20.f && st.footRotation.z == -30.f && st.footClockwise);
         MirrorStand(st);
         assert(!st.legLeft && st.footPosition.x == orig.footPosition.x && st.footRotation.z == orig.footRotation.z && !st.footClockwise);
      }
      // Object > Mirror of a rail: each fixture mirrored with the wires (x negated) is the mirror image of the original in the cross-section, and twice is the original
      {
         std::vector<Wire> w = DefaultWires(4, true), wm;
         w[2].x += 4.f; // Not symmetric, so a wrong side shows
         for (Wire x : w)
         {
            x.x = -x.x;
            wm.push_back(x);
         }
         Fixture ring = DefaultFixture(kFixtureRing, 250.f), rung = DefaultFixture(kFixtureRung, 250.f), cradle = DefaultFixture(kFixtureCradle, 250.f), stand = DefaultFixture(kFixtureStand, 250.f);
         ring.offsetX = 3.f;
         ring.cutout = ring.straight = true;
         ring.cutoutStart = 30.f;
         ring.cutoutEnd = 100.f;
         ring.straightStart = 200.f;
         ring.straightEnd = 300.f;
         rung.angle = 20.f;
         rung.offsetX = 2.f;
         cradle.leftArm = 60.f;
         cradle.rightArm = 90.f;
         cradle.rotation = 15.f;
         cradle.offsetX = 2.f;
         stand.offsetX = 3.f;
         stand.footRotation = { 10.f, 20.f, 30.f };
         for (const Fixture& f : { ring, rung, cradle, stand })
         {
            Fixture m = f;
            MirrorFixture(m);
            ClampFixture(m); // As a load or an edit of the fixture does: the mirror must already be in range (a rung at 20 gives 340, not 0)
            std::vector<FixtureShape> sa, sb;
            std::vector<Vx> va, vb;
            std::vector<unsigned> ia, ib;
            build({ f }, va, ia, sa, w);
            build({ m }, vb, ib, sb, wm);
            assert(sa.size() == 1 && sb.size() == 1 && sa[0].built && sb[0].built && sa[0].section.size() > 1 && sb[0].section.size() > 1);
            const auto near = [](const V2 p, const std::vector<V2>& line)
            {
               float best = 1e30f;
               for (size_t k = 0; k + 1 < line.size(); k++)
                  best = std::min(best, DistToSegment(p, line[k], line[k + 1]));
               return best;
            };
            for (const V2 p : sa[0].section)
               assert(near({ -p.x, p.y }, sb[0].section) < 0.5f);
            for (const V2 p : sb[0].section)
               assert(near({ -p.x, p.y }, sa[0].section) < 0.5f);
            assert(fabsf(sa[0].position - sb[0].position) < 1e-4f);
            MirrorFixture(m);
            assert(m.offsetX == f.offsetX && m.angle == f.angle && m.leftArm == f.leftArm && m.rightArm == f.rightArm && fabsf(m.rotation - f.rotation) < 1e-4f
               && fabsf(m.cutoutStart - f.cutoutStart) < 1e-4f && fabsf(m.cutoutEnd - f.cutoutEnd) < 1e-4f && fabsf(m.straightStart - f.straightStart) < 1e-4f
               && fabsf(m.straightEnd - f.straightEnd) < 1e-4f && m.legLeft == f.legLeft && m.footRotation.z == f.footRotation.z);
         }
         Fixture zero = DefaultFixture(kFixtureRing, 0.f), zeroStand = DefaultFixture(kFixtureStand, 0.f);
         Fixture reserved = DefaultFixture(5, 0.f);
         reserved.offsetX = 4.f;
         MirrorFixture(reserved);
         assert(reserved.offsetX == 4.f); // A type reserved for phase 2b is left as it is
         MirrorFixture(zero);
         MirrorFixture(zeroStand);
         assert(!std::signbit(zero.offsetX) && !std::signbit(zeroStand.offsetX) && !std::signbit(zeroStand.startVector.x) && !std::signbit(zeroStand.footRotation.y)
            && !std::signbit(zeroStand.footRotation.z)); // Also with -ffast-math, as the engine builds
      }
      // FrameAt is continuous across the rows of a bending, banked, falling route, and stays square
      {
         std::vector<PathPoint> arc;
         for (int i = 0; i <= 720; i++)
         {
            const float a = 1.5707963f * (float)i / 720.f;
            arc.push_back({ 300.f * cosf(a), 300.f * sinf(a), 100.f - 40.f * (float)i / 720.f, i == 0 });
         }
         const auto fr = Route(arc, { 10.f, 10.f });
         const auto rd = RowDistances(fr);
         Frame prev = FrameAt(fr, rd, 0.f);
         for (float d = 0.5f; d <= rd.back() + 1.f; d += 0.5f)
         {
            const Frame f = FrameAt(fr, rd, d);
            assert(WireSweep::Len(f.p - prev.p) < 1.f);
            assert(WireSweep::Dot(f.t, prev.t) > 0.99f && WireSweep::Dot(f.r, prev.r) > 0.99f && WireSweep::Dot(f.u, prev.u) > 0.99f);
            assert(fabsf(WireSweep::Dot(f.r, f.t)) < 1e-4f && fabsf(WireSweep::Dot(f.u, f.t)) < 1e-4f && fabsf(WireSweep::Dot(f.r, f.u)) < 1e-4f);
            assert(fabsf(WireSweep::Len(f.r) - 1.f) < 1e-4f && fabsf(WireSweep::Len(f.u) - 1.f) < 1e-4f);
            assert(std::isfinite(f.p.x) && std::isfinite(f.r.x) && std::isfinite(f.u.z));
            prev = f;
         }
         std::vector<Fixture> fx;
         for (int k = 0; k < 8; k++)
            fx.push_back(DefaultFixture(k % 4, rd.back() * (float)k / 7.f));
         std::vector<Vx> vv;
         std::vector<unsigned> ii;
         BuildFixtures<Vx>(fr, rd, wires, wireR, kDefaultSides, kDefaultCapBevel, ballR, profile, fx, vv, ii, sh);
         assert(finite(vv) && !ii.empty());
         for (const unsigned k : ii)
            assert(k < vv.size());
         for (const FixtureShape& s : sh)
            assert(s.built);
         printf("fixtures: 8 on a bent route, %d triangles\n", (int)ii.size() / 3);
      }
      printf("fixtures OK\n");
   }
   printf("wirerail_check OK\n");
   return 0;
}
