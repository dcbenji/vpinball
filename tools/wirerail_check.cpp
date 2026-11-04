// Standalone check of src/math/wirerail.h: clang++ -std=c++20 -I src tools/wirerail_check.cpp -o /tmp/wrc && /tmp/wrc
#include <cassert>
#include <cstdio>
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
      for (const auto& e : c.edges)
         assert(e.first < c.verts.size() && e.second < c.verts.size());
      // A wire centreline stays a wire radius from its facet contact on every row
      const auto wire = WireCenterline(frames, DefaultWires(4, true)[0]);
      for (size_t i = 0; i < frames.size(); i++)
         assert(WireSweep::Len(wire[i] - frames[i].p) > 14.f && WireSweep::Len(wire[i] - frames[i].p) < 16.f);
      printf("channel: %d rows, %d verts, %d tris\n", c.rows, (int)c.verts.size(), (int)c.tris.size() / 3);
   }
   printf("wirerail_check OK\n");
   return 0;
}
