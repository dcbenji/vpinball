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


// Shading of a built mesh: every normal of unit length and finite, UVs finite, every triangle wound as its vertex normals face, and on the tube surface (a vertex at the
// tube radius from 'centerline') the normal along the radial direction from the nearest centreline point. Returns the faults found, printing the first few with 'what'
static bool BitsFinite(const float v)
{
   uint32_t b;
   memcpy(&b, &v, sizeof b);
   return (b & 0x7F800000u) != 0x7F800000u; // -ffast-math drops std::isfinite
}
static int CheckShading(const char* what, const std::vector<Vx>& v, const std::vector<unsigned>& idx, const size_t v0, const size_t i0, const std::vector<V3>& centerline, const float radius, size_t tubeEnd = 0)
{
   int bad = 0, badUnit = 0, badOut = 0, badRadial = 0, badWind = 0, tubeVerts = 0;
   const auto report = [&](const char* kind, const size_t k, const float val)
   {
      if (bad++ < 6)
         printf("  %s: %s at vertex %d (%.2f, %.2f, %.2f) n (%.3f, %.3f, %.3f): %.4f\n", what, kind, (int)k, v[k].x, v[k].y, v[k].z, v[k].nx, v[k].ny, v[k].nz, val);
   };
   for (size_t k = v0; k < v.size(); k++)
   {
      const Vx& x = v[k];
      if (!BitsFinite(x.x) || !BitsFinite(x.y) || !BitsFinite(x.z) || !BitsFinite(x.nx) || !BitsFinite(x.ny) || !BitsFinite(x.nz) || !BitsFinite(x.tu) || !BitsFinite(x.tv))
      {
         report("not finite", k, 0.f);
         badUnit++;
         continue;
      }
      const V3 n { x.nx, x.ny, x.nz }, p { x.x, x.y, x.z };
      const float l = WireSweep::Len(n);
      if (fabsf(l - 1.f) > 1e-3f)
      {
         report("normal not unit", k, l);
         badUnit++;
      }
      if (centerline.size() < 2 || (tubeEnd && k >= tubeEnd))
         continue; // The blobs after the tube: normals and winding only
      // The nearest point of the centreline polyline
      float best = 1e30f;
      V3 near {};
      for (size_t s = 0; s + 1 < centerline.size(); s++)
      {
         const V3 a = centerline[s], d = centerline[s + 1] - a;
         const float l2 = WireSweep::Dot(d, d);
         const float t = l2 > 0.f ? std::clamp(WireSweep::Dot(p - a, d) / l2, 0.f, 1.f) : 0.f;
         const V3 q = a + d * t;
         const float dist = WireSweep::Len(p - q);
         if (dist < best)
         {
            best = dist;
            near = q;
         }
      }
      if (fabsf(best - radius) > radius * 0.05f)
         continue; // Not on the tube surface: a cap, a chamfer or a blob
      tubeVerts++;
      const V3 radial = (p - near) * (1.f / best);
      const float d = WireSweep::Dot(n, radial);
      if (d <= 0.f)
      {
         report("normal points into the tube", k, d);
         badOut++;
      }
      else if (d < 0.7f) // A chamfer ring at the tube radius is blended half way (cos 45 degrees)
      {
         report("normal off the radial", k, d);
         badRadial++;
      }
   }
   for (size_t t = i0; t + 2 < idx.size(); t += 3)
   {
      const Vx &a = v[idx[t]], &b = v[idx[t + 1]], &c = v[idx[t + 2]];
      const V3 face = WireSweep::Cross(V3 { b.x - a.x, b.y - a.y, b.z - a.z }, V3 { c.x - a.x, c.y - a.y, c.z - a.z });
      if (WireSweep::Len(face) < 1e-8f)
         continue;
      const V3 vn { a.nx + b.nx + c.nx, a.ny + b.ny + c.ny, a.nz + b.nz + c.nz };
      const float d = WireSweep::Dot(WireSweep::Norm(face), WireSweep::Norm(vn));
      if (d <= 0.f)
      {
         report("winding against the normals", idx[t], d);
         badWind++;
      }
   }
   printf("shading %s: %d verts (%d on the tube), %d tris: unit/finite %d, inward %d, off radial %d, winding %d\n", what, (int)(v.size() - v0), tubeVerts, (int)(idx.size() - i0) / 3, badUnit, badOut, badRadial, badWind);
   return bad;
}


// The tube and channel builders of phase 2a (702132ad7), frozen as they were: a rail without end fittings must come out of the phase 2b builders byte for byte the same
namespace Ref2a
{
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
   p[0] = endS + tS * bevel;
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
         if (atEnd)
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
template <class Vertex>
bool BuildTubes(const std::vector<Frame>& frames, const std::vector<Wire>& wires, const float radius, const int sides, const float bevel, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
{
   bool any = false;
   for (const Wire& w : wires)
      if (w.active)
         any |= AppendTube<Vertex>(WireCenterline(frames, w), radius, sides, bevel, verts, idx);
   return any;
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
   return c;
}
}

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
         Fixture unk = DefaultFixture(9, 100.f); // A type of a later version
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
         Fixture reserved = DefaultFixture(9, 0.f);
         reserved.offsetX = 4.f;
         MirrorFixture(reserved);
         assert(reserved.offsetX == 4.f); // A type of a later version is left as it is
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
      // Shading: each fixture type at its defaults, the stand with a non-default foot too, and the rail tubes' own caps
      {
         int faults = 0;
         std::vector<Fixture> kinds { DefaultFixture(kFixtureRing, 250.f), DefaultFixture(kFixtureRung, 250.f), DefaultFixture(kFixtureCradle, 250.f), DefaultFixture(kFixtureStand, 250.f) };
         Fixture foot = DefaultFixture(kFixtureStand, 250.f);
         foot.legLeft = true;
         foot.startVector = WireSweep::Norm(V3 { 0.3f, 0.2f, -1.f });
         foot.startLength = 25.f;
         foot.footPosition = { -10.f, 5.f, -70.f };
         foot.footRotation = { 20.f, 35.f, 60.f };
         foot.footClockwise = true;
         foot.footWidth = 22.f;
         foot.footArm = 18.f;
         foot.footConnectedArm = 40.f;
         kinds.push_back(foot);
         const char* names[] = { "ring", "rung", "cradle", "stand", "stand (foot moved)" };
         for (size_t k = 0; k < kinds.size(); k++)
         {
            build({ kinds[k] }, v, idx, sh);
            assert(sh[0].built);
            std::vector<Vx> tube;
            std::vector<unsigned> tubeIdx;
            if (kinds[k].type == kFixtureRing)
               AppendClosedTube<Vx>(std::vector<V3>(sh[0].centerline.begin(), sh[0].centerline.end() - 1), wireR, kDefaultSides, tube, tubeIdx);
            else
               AppendTube<Vx>(sh[0].centerline, wireR, kDefaultSides, kDefaultCapBevel, tube, tubeIdx);
            faults += CheckShading(names[k], v, idx, 0, 0, sh[0].centerline, wireR, tube.size());
         }
         std::vector<Vx> tv;
         std::vector<unsigned> ti;
         assert(BuildTubes<Vx>(frames, wires, wireR, kDefaultSides, kDefaultCapBevel, tv, ti));
         faults += CheckShading("rail tubes", tv, ti, 0, 0, WireCenterline(frames, wires[0]), -1.f); // Normals and winding only (the radial test needs one centreline)
         assert(faults == 0);
      }
      printf("fixtures OK\n");
   }

   // End fittings (phase 2b): freezy's Hairpin, Elbow and Rail Trim
   {
      std::vector<PathPoint> line;
      for (int i = 0; i <= 50; i++)
         line.push_back({ 0.f, 10.f * (float)i, 0.f, i == 0 });
      const auto frames = Route(line);
      const auto rowDist = RowDistances(frames);
      const float total = rowDist.back();
      assert(fabsf(total - 500.f) < 0.01f);
      const auto wires = DefaultWires(4, true);
      Profile profile;
      assert(FitProfile(wires, wireR, ballR, profile, err));
      const auto fin = [](const float v) { return BitsFinite(v); };
      const auto allFinite = [&](const std::vector<Vx>& v)
      {
         for (const Vx& x : v)
            if (!fin(x.x) || !fin(x.y) || !fin(x.z) || !fin(x.nx) || !fin(x.ny) || !fin(x.nz) || !fin(x.tu) || !fin(x.tv))
               return false;
         return true;
      };
      const auto vfinite = [&](const V3& p) { return fin(p.x) && fin(p.y) && fin(p.z); };
      const auto at = [](const std::vector<V3>& pts, const size_t i) { return pts[i]; };
      // What the render builds with a list of fittings: tubes, supports, fittings. Returns the triangle count
      const auto render = [&](const std::vector<Fixture>& fx, const std::vector<Wire>& w, std::vector<Vx>& v, std::vector<unsigned>& idx, std::vector<FixtureShape>& sh, FittingPlan* planOut = nullptr)
      {
         v.clear();
         idx.clear();
         const FittingPlan renderPlan = PlanFittings(fx, w, total, true), all = PlanFittings(fx, w, total, false);
         TubeRings rings;
         BuildTubesTrimmed<Vx>(frames, rowDist, w, wireR, kDefaultSides, kDefaultCapBevel, renderPlan.trims, renderPlan.fitted, &rings, v, idx);
         BuildFixtures<Vx>(frames, rowDist, w, wireR, kDefaultSides, kDefaultCapBevel, ballR, profile, fx, v, idx, sh, &renderPlan.trims);
         BuildEndFittings<Vx>(frames, rowDist, w, wireR, kDefaultSides, kDefaultCapBevel, fx, all, renderPlan, rings, v, idx, sh);
         if (planOut != nullptr)
            *planOut = all;
         return (int)(idx.size() / 3);
      };
      std::vector<Vx> v;
      std::vector<unsigned> idx;
      std::vector<FixtureShape> sh;

      // Hairpin at the defaults (4 wires, 6.5, density 24, End): rails at x +-15 z 0, arc ends (+-30, +40 along the route), apex +70, 25 points; 480 triangles
      {
         const std::vector<Fixture> fx { DefaultFixture(kFixtureHairpin, total) };
         assert(fx[0].density == 24 && fx[0].railA == 0 && fx[0].railB == 1 && fx[0].fitOffset == 0.f && fx[0].endpoint == kEndEnd);
         FittingPlan plan = PlanFittings(fx, wires, total, true);
         assert(plan.ok[0] && plan.why[0].empty());
         HairpinPath hp;
         assert(HairpinPoints(plan.eff[0], frames, rowDist, wires, total, 6, 12, hp));
         assert(hp.pts.size() == 25 && hp.terminalStartSpan == 6 && hp.terminalEndSpan == 18);
         assert(fabsf(hp.pts[0].x + 15.f) < 1e-3f && fabsf(hp.pts[0].y - 500.f) < 1e-3f && fabsf(hp.pts[24].x - 15.f) < 1e-3f);
         assert(fabsf(hp.pts[6].x + 30.f) < 1e-3f && fabsf(hp.pts[6].y - 540.f) < 1e-3f && fabsf(hp.pts[6].z) < 1e-3f); // The arc ends
         assert(fabsf(hp.pts[18].x - 30.f) < 1e-3f && fabsf(hp.pts[18].y - 540.f) < 1e-3f);
         assert(fabsf(hp.pts[12].x) < 1e-3f && fabsf(hp.pts[12].y - 570.f) < 1e-3f); // The apex, 70 beyond the endpoint
         for (const V3& q : hp.pts)
            assert(vfinite(q) && fabsf(q.z) < 1e-3f);
         // The Bezier handle: 15 (the chord is 42.72 and 0.49 of it 20.9 is the cap)
         {
            const V3 a = hp.pts[0], arcA = hp.pts[6];
            const float chord = WireSweep::Len(arcA - a);
            assert(fabsf(chord - 42.72f) < 0.01f);
            const auto lead = [&](const float handle) { return Detail::BuildLead(a, arcA, { 0.f, 1.f, 0.f }, handle, 6); };
            const auto capped = lead(100.f), atCap = lead(0.49f * chord), fifteen = lead(15.f), tiny = lead(1.f);
            for (size_t k = 0; k < capped.size(); k++)
               assert(WireSweep::Len(capped[k] - atCap[k]) < 1e-3f);
            assert(WireSweep::Len(fifteen[2] - tiny[2]) > 1.f && WireSweep::Len(fifteen[2] - atCap[2]) > 0.1f && fabsf(0.49f * chord - 20.9f) < 0.05f);
            assert(WireSweep::Len(fifteen[0] - a) < 1e-4f && WireSweep::Len(fifteen[6] - arcA) < 1e-4f);
         }
         // Render: 24 spans x 10 sides x 2 = 480 triangles, no caps, plus the bridge bands (2 x 20); the rails lose their end caps (2 x 30 triangles)
         std::vector<Vx> tube;
         std::vector<unsigned> ti;
         TubeOpts open;
         open.capStart = open.capEnd = false;
         assert(AppendTube<Vx>(hp.pts, wireR, kDefaultSides, kDefaultCapBevel, tube, ti, &open));
         assert(ti.size() / 3 == 480 && tube.size() == 25 * kDefaultSides);
         std::vector<Vx> v0, vh;
         std::vector<unsigned> i0, ih;
         std::vector<FixtureShape> s0, sH;
         const int none = render({}, wires, v0, i0, s0);
         const int tris = render(fx, wires, vh, ih, sH);
         assert(tris == none - 2 * 30 + 480 + 2 * 20);
         assert(sH[0].built && sH[0].triangles == 480 && sH[0].railA == 0 && sH[0].railB == 1 && fabsf(sH[0].position - 500.f) < 1e-3f && allFinite(vh));
         // The mouths are welded: every triangle of the bridge bands is short
         for (size_t t = ih.size() - 2 * 20 * 3; t < ih.size(); t += 3)
         {
            const V3 a { vh[ih[t]].x, vh[ih[t]].y, vh[ih[t]].z }, b { vh[ih[t + 1]].x, vh[ih[t + 1]].y, vh[ih[t + 1]].z }, c { vh[ih[t + 2]].x, vh[ih[t + 2]].y, vh[ih[t + 2]].z };
            assert(WireSweep::Len(a - b) < 6.f && WireSweep::Len(b - c) < 6.f && WireSweep::Len(a - c) < 6.f); // The ring chord is 3.9, a twisted band would cross the tube
            assert(WireSweep::Len(a - b) < 4.5f || WireSweep::Len(a - c) < 4.5f);
         }
         // Collider: a square tube, 4 + 12 + 4 spans, 20 x 4 faces x 2 = 160 triangles, 96 on the arc; 80 corner lines, 84 points
         Channel box;
         BoxTerminal term;
         assert(AppendHairpinBox(box, plan.eff[0], frames, rowDist, wires, total, wireR * 2.f, term));
         assert(box.tris.size() / 3 == 160 && term.tris.size() == 96 && box.edges.size() == 80 && box.verts.size() == 84 && term.edges.size() == 48);
         for (const V3& q : box.verts)
            assert(vfinite(q));
         // Faces outward: the normal (as HitTriangle takes it) points away from the path, every box corner is half the wire diameter out on both axes
         HairpinPath cp;
         assert(HairpinPoints(plan.eff[0], frames, rowDist, wires, total, 4, 12, cp) && cp.pts.size() == 21);
         for (size_t t = 0; t + 2 < box.tris.size(); t += 3)
         {
            const V3 a = box.verts[box.tris[t]], b = box.verts[box.tris[t + 1]], c = box.verts[box.tris[t + 2]];
            const V3 n = WireSweep::Cross(b - a, c - a);
            const V3 centre = (a + b + c) * (1.f / 3.f);
            float best = 1e30f;
            V3 nearest {};
            for (size_t k = 0; k + 1 < cp.pts.size(); k++)
            {
               const V3 d = cp.pts[k + 1] - cp.pts[k];
               const float l2 = WireSweep::Dot(d, d);
               const float u = l2 > 0.f ? std::clamp(WireSweep::Dot(centre - cp.pts[k], d) / l2, 0.f, 1.f) : 0.f;
               const V3 q = cp.pts[k] + d * u;
               if (WireSweep::Len(centre - q) < best)
               {
                  best = WireSweep::Len(centre - q);
                  nearest = q;
               }
            }
            assert(WireSweep::Dot(n, centre - nearest) > 0.f && best > 2.5f && best < 4.7f); // 3.25 to the face, up to 4.6 to a corner
         }
         // Side of the box = the wire diameter
         assert(fabsf(WireSweep::Len(box.verts[0] - box.verts[1]) - 6.5f) < 0.02f && fabsf(WireSweep::Len(box.verts[1] - box.verts[2]) - 6.5f) < 0.02f);
         // A rail's ball channel with the hairpin: the box and the terminal flags
         std::vector<ChannelGap> gaps;
         const Channel ch = BuildFittingChannel(frames, rowDist, wires, wireR, ballR, profile, fx, plan, &gaps);
         assert(gaps.empty() && ch.pieces.size() == 1 && ch.quadIndices == BuildChannel(frames, profile).tris.size());
         size_t terminalTris = 0, terminalEdges = 0, terminalVerts = 0;
         for (const unsigned char f : ch.triTerminal)
            terminalTris += f;
         for (const unsigned char f : ch.edgeTerminal)
            terminalEdges += f;
         for (const unsigned char f : ch.vertTerminal)
            terminalVerts += f;
         assert(ch.triTerminal.size() == ch.tris.size() / 3 && terminalTris == 96 && terminalEdges == 48 && terminalVerts == 13 * 4);
         // A hairpin at Offset 60 trims rails 1 and 2 by 60 in the collider and the render
         Fixture off = fx[0];
         off.fitOffset = 60.f;
         plan = PlanFittings({ off }, wires, total, false);
         assert(plan.ok[0] && fabsf(plan.trims.end[0] - 60.f) < 1e-4f && fabsf(plan.trims.end[1] - 60.f) < 1e-4f && plan.trims.end[2] == 0.f && plan.trims.end[3] == 0.f && plan.trims.start[0] == 0.f);
         const Trims coll = ColliderTrims({ off }, wires, plan);
         assert(coll.end[0] == 60.f && coll.end[1] == 60.f && coll.end[2] == 0.f);
         assert(HairpinPoints(plan.eff[0], frames, rowDist, wires, total, 6, 12, hp) && fabsf(hp.pts[0].y - 440.f) < 1e-3f && fabsf(hp.pts[12].y - 510.f) < 1e-3f);
         // Rotation 90: the loop stands up (its diameter turned about the route)
         Fixture rot = fx[0];
         rot.rotation = 90.f;
         plan = PlanFittings({ rot }, wires, total, false);
         assert(HairpinPoints(plan.eff[0], frames, rowDist, wires, total, 6, 12, hp));
         assert(fabsf(hp.pts[6].x) < 1e-3f && fabsf(fabsf(hp.pts[6].z) - 30.f) < 1e-3f && fabsf(hp.pts[12].y - 570.f) < 1e-3f);
         // At the Start: the loop leaves the other way, the rails' first rings meet its mouths
         Fixture st = fx[0];
         st.endpoint = kEndStart;
         st.position = 0.f;
         plan = PlanFittings({ st }, wires, total, false);
         assert(HairpinPoints(plan.eff[0], frames, rowDist, wires, total, 6, 12, hp) && fabsf(hp.pts[0].y) < 1e-3f && fabsf(hp.pts[12].y + 70.f) < 1e-3f);
         assert(render({ st }, wires, vh, ih, sH) == none - 2 * 30 + 480 + 2 * 20 && allFinite(vh));
         for (size_t t = ih.size() - 2 * 20 * 3; t < ih.size(); t += 3)
         {
            const V3 a { vh[ih[t]].x, vh[ih[t]].y, vh[ih[t]].z }, b { vh[ih[t + 1]].x, vh[ih[t + 1]].y, vh[ih[t + 1]].z };
            assert(WireSweep::Len(a - b) < 6.f);
         }
      }

      // Elbow at the defaults: rails 1 and 2 end 40 short, 3 and 4 run to the end; path of 8 points, the bend at the Wire Diameter, the bottom 80 down
      {
         const Fixture elbow = DefaultFixture(kFixtureElbow, total, &wires);
         assert(elbow.fitOffset == 40.f && elbow.railA == 0 && elbow.railB == 1 && elbow.dropLength == 80.f);
         const std::vector<Fixture> fx { elbow };
         FittingPlan plan = PlanFittings(fx, wires, total, false);
         assert(plan.ok[0]);
         assert(fabsf(plan.trims.end[0] - 40.f) < 1e-4f && fabsf(plan.trims.end[1] - 40.f) < 1e-4f && plan.trims.end[2] == 0.f && plan.trims.end[3] == 0.f);
         ElbowPath ep;
         assert(ElbowPoints(plan.eff[0], frames, rowDist, wires, total, 2.f * wireR, ep));
         assert(ep.a.size() == 8 && ep.b.size() == 8);
         assert(fabsf(ep.a[0].y - 460.f) < 1e-3f && fabsf(ep.a[0].x + 15.f) < 1e-3f && fabsf(ep.b[0].x - 15.f) < 1e-3f);
         const V3 centreA = ep.a[0] + V3 { 0.f, 0.f, -6.5f };
         for (size_t k = 1; k <= 6; k++)
            assert(fabsf(WireSweep::Len(ep.a[k] - centreA) - 6.5f) / 6.5f < 0.01f); // The bend is a circle of 6.5, the wire's diameter
         assert(fabsf(ep.a.back().z + 80.f) < 1e-3f && fabsf(ep.a.back().y - 466.5f) < 1e-3f && fabsf(ep.b.back().z + 80.f) < 1e-3f);
         assert(fabsf(ep.a[6].z + 6.5f) < 1e-3f && fabsf(ep.a[6].y - 466.5f) < 1e-3f); // The bend ends going straight down, 6.5 out and 6.5 down
         // A shorter drop tightens the bend (the radius is at most the drop)
         Fixture shortDrop = elbow;
         shortDrop.dropLength = 2.f;
         plan = PlanFittings({ shortDrop }, wires, total, false);
         assert(ElbowPoints(plan.eff[0], frames, rowDist, wires, total, 2.f * wireR, ep) && fabsf(ep.a.back().z + 2.f) < 1e-3f);
         // Z Angle turns the horizontal leg about the route's up
         Fixture twist = elbow;
         twist.zAngle = 90.f;
         plan = PlanFittings({ twist }, wires, total, false);
         assert(ElbowPoints(plan.eff[0], frames, rowDist, wires, total, 2.f * wireR, ep) && fabsf(fabsf(ep.a.back().x - ep.a[0].x) - 6.5f) < 1e-3f && fabsf(ep.a.back().y - 460.f) < 1e-3f);
         // Other Rail Cutoffs shorten the other rails in the render only
         Fixture cut = elbow;
         cut.cutoff[2] = 25.f;
         cut.cutoff[0] = 99.f; // The attached rail's own: fixed at 0
         plan = PlanFittings({ cut }, wires, total, false);
         assert(plan.ok[0] && plan.trims.end[2] == 25.f && plan.trims.end[0] == 40.f);
         const Trims coll = ColliderTrims({ cut }, wires, plan);
         assert(coll.end[0] == 40.f && coll.end[1] == 40.f && coll.end[2] == 0.f && coll.end[3] == 0.f);
         // Rendered: two tubes open at the rails, bevelled at the bottom, welded
         std::vector<Vx> v0, ve;
         std::vector<unsigned> i0, ie;
         std::vector<FixtureShape> s0, sE;
         const int none = render({}, wires, v0, i0, s0);
         const int tris = render(fx, wires, ve, ie, sE);
         assert(sE[0].built && sE[0].centerline.size() == 8 && sE[0].centerline2.size() == 8 && allFinite(ve) && tris > none);
         // The channel: [0, 460] with the four wires, then wires 3 and 4 alone, 60 apart, which a 50 ball falls between: no channel there, said
         std::vector<ChannelGap> gaps;
         plan = PlanFittings(fx, wires, total, false);
         const Channel ch = BuildFittingChannel(frames, rowDist, wires, wireR, ballR, profile, fx, plan, &gaps);
         assert(ch.pieces.size() == 1 && gaps.size() == 1 && fabsf(gaps[0].from - 460.f) < 1e-3f && fabsf(gaps[0].to - 500.f) < 1e-3f);
         // Two drop faces (the floor spans meeting at the lowest vertex), under it, 80 down; 8 triangles; the last vertex of the floor stays the profile's own
         assert(ch.tris.size() == ch.quadIndices && ch.verts.size() == (size_t)ch.pieces[0].rows * ch.pieces[0].rowSize + 8);
         float lowest = 1e9f;
         for (const auto& span : profile.spans)
            if (IsFloorSpan(profile, span))
               lowest = std::min(lowest, std::min(profile.verts[span.first].y, profile.verts[span.second].y));
         const size_t n0 = (size_t)ch.pieces[0].rows * ch.pieces[0].rowSize;
         float lowestFace = 1e9f;
         for (int f = 0; f < 2; f++)
         {
            const V3 t0 = ch.verts[n0 + f * 4], b0 = ch.verts[n0 + f * 4 + 1], t1 = ch.verts[n0 + f * 4 + 2], b1 = ch.verts[n0 + f * 4 + 3];
            assert(fabsf(t0.z - b0.z - 80.f) < 1e-3f && fabsf(t1.z - b1.z - 80.f) < 1e-3f && fabsf(t0.x - b0.x) < 1e-4f && fabsf(t0.y - 460.f) < 1e-3f);
            lowestFace = std::min({ lowestFace, t0.z, t1.z });
            assert(t0.z >= lowest - 1e-3f && t1.z >= lowest - 1e-3f);
         }
         assert(fabsf(lowestFace - lowest) < 1e-3f); // The faces hang from the lowest vertex of the profile
         assert(ch.edges.size() == (size_t)(ch.pieces[0].rows - 1) * ch.pieces[0].rowSize + 6);
      }

      // Rail Trim: the largest value per rail, the order does not matter; a fitting that is refused trims nothing and is said
      {
         Fixture a = DefaultFixture(kFixtureTrim, total), b = DefaultFixture(kFixtureTrim, total);
         a.cutoff[0] = 10.f;
         a.cutoff[2] = 30.f;
         b.cutoff[0] = 5.f;
         b.cutoff[1] = 20.f;
         b.endpoint = kEndStart;
         Fixture c = DefaultFixture(kFixtureTrim, total);
         c.cutoff[0] = 12.f;
         c.cutoff[3] = 40.f;
         FittingPlan p1 = PlanFittings({ a, b, c }, wires, total, false), p2 = PlanFittings({ c, b, a }, wires, total, false);
         for (int w = 0; w < 4; w++)
            assert(p1.trims.start[w] == p2.trims.start[w] && p1.trims.end[w] == p2.trims.end[w]);
         assert(p1.trims.end[0] == 12.f && p1.trims.end[1] == 0.f && p1.trims.end[2] == 30.f && p1.trims.end[3] == 40.f && p1.trims.start[0] == 5.f && p1.trims.start[1] == 20.f);
         // A trim at the other endpoint is no conflict
         Fixture e = DefaultFixture(kFixtureElbow, total, &wires);
         Fixture t = DefaultFixture(kFixtureTrim, total);
         t.cutoff[0] = 30.f;
         t.endpoint = kEndStart;
         FittingPlan p = PlanFittings({ e, t }, wires, total, false);
         assert(p.ok[0] && p.trims.end[0] == 40.f && p.trims.start[0] == 30.f);
         // At the same endpoint on one of its rails: the elbow is not built, says why, trims nothing (its Offset 40 is not on rail 2)
         t.endpoint = kEndEnd;
         p = PlanFittings({ e, t }, wires, total, false);
         assert(!p.ok[0] && p.why[0].find("Another endpoint cutoff") != std::string::npos && p.trims.end[0] == 30.f && p.trims.end[1] == 0.f);
         Fixture h = DefaultFixture(kFixtureHairpin, total);
         h.fitOffset = 50.f;
         p = PlanFittings({ h, t }, wires, total, false);
         assert(!p.ok[0] && p.why[0].find("Rail Trim") != std::string::npos && p.trims.end[1] == 0.f && p.trims.end[0] == 30.f && !p.fitted.Any());
         // A Rail Trim on a rail that is not one of the pair is no conflict
         t.cutoff[0] = 0.f;
         t.cutoff[2] = 30.f;
         p = PlanFittings({ e, t }, wires, total, false);
         assert(p.ok[0] && p.trims.end[2] == 30.f && p.trims.end[0] == 40.f && p.fitted.end[0] && p.fitted.end[1] && !p.fitted.end[2]);
         // Two elbows on the same pair: each refuses the other
         p = PlanFittings({ e, e }, wires, total, false);
         assert(!p.ok[0] && !p.ok[1] && !p.trims.Any());
         // Switched off: out of the render plan (no trim, no cap change), still in the full plan
         e.enabled = false;
         p = PlanFittings({ e }, wires, total, true);
         assert(!p.trims.Any() && !p.fitted.Any());
         p = PlanFittings({ e }, wires, total, false);
         assert(p.trims.end[0] == 40.f && p.fitted.end[0]);
         // The refusals
         Fixture eq = DefaultFixture(kFixtureHairpin, total);
         eq.railB = 0;
         p = PlanFittings({ eq }, wires, total, false);
         assert(!p.ok[0] && p.why[0] == "Select two different rails. Invalid Hairpins are not generated.");
         eq = DefaultFixture(kFixtureHairpin, total);
         eq.fitOffset = 500.f;
         p = PlanFittings({ eq }, wires, total, false);
         assert(!p.ok[0] && p.why[0].find("consumes the whole route") != std::string::npos && !p.trims.Any());
         auto off = wires;
         off[1].active = false;
         p = PlanFittings({ DefaultFixture(kFixtureElbow, total, &off) }, off, total, false);
         assert(p.ok[0] && !(DefaultFixture(kFixtureElbow, total, &off).railB == 1)); // The elbow's default pair skips the wire that is off
         p = PlanFittings({ DefaultFixture(kFixtureElbow, total, &wires) }, off, total, false);
         assert(!p.ok[0] && p.why[0].find("must be active") != std::string::npos);
         // A rail beyond Rails: an elbow's is clamped as freezy's Ensure would, a hairpin's is not built
         Fixture far = DefaultFixture(kFixtureElbow, total, &wires);
         far.railB = 5;
         p = PlanFittings({ far }, wires, total, false);
         assert(p.ok[0] && p.eff[0].railB == 3);
         far = DefaultFixture(kFixtureHairpin, total);
         far.railB = 5;
         p = PlanFittings({ far }, wires, total, false);
         assert(p.ok[0] && p.eff[0].railB == 3); // Clamped to Rails - 1 like the elbow
      }

      // A trimmed channel: the route cut at the trims, a profile fitted from the wires that are left in each piece; what can't hold a ball has no collider
      {
         Fixture top = DefaultFixture(kFixtureTrim, total);
         top.cutoff[2] = 100.f;
         top.cutoff[3] = 100.f;
         std::vector<ChannelGap> gaps;
         FittingPlan plan = PlanFittings({ top }, wires, total, false);
         Channel ch = BuildFittingChannel(frames, rowDist, wires, wireR, ballR, profile, { top }, plan, &gaps);
         assert(ch.pieces.size() == 2 && gaps.empty());
         Profile two;
         auto bottom = wires;
         bottom[2].active = bottom[3].active = false;
         assert(FitProfile(bottom, wireR, ballR, two, err));
         assert(ch.pieces[0].rowSize == (int)profile.verts.size() && ch.pieces[1].rowSize == (int)two.verts.size() && ch.pieces[1].rows == 2 && ch.pieces[0].rows == 2);
         // The first piece is the full rail's channel over [0, 400]: its rows
         const Frame cutFrame = FrameAt(frames, rowDist, 400.f);
         const V3 want = cutFrame.p + cutFrame.r * profile.verts[0].x + cutFrame.u * profile.verts[0].y;
         assert(WireSweep::Len(ch.verts[ch.pieces[0].rowSize] - want) < 1e-3f && WireSweep::Len(ch.verts[ch.pieces[1].firstVert] - (cutFrame.p + cutFrame.r * two.verts[0].x + cutFrame.u * two.verts[0].y)) < 1e-3f);
         for (const auto& e : ch.edges)
            assert(e.first < ch.verts.size() && e.second < ch.verts.size());
         // A floor wire trimmed 50: the wires left from 450
         Fixture floor = DefaultFixture(kFixtureTrim, total);
         floor.cutoff[0] = 50.f;
         plan = PlanFittings({ floor }, wires, total, false);
         ch = BuildFittingChannel(frames, rowDist, wires, wireR, ballR, profile, { floor }, plan, &gaps);
         Profile three;
         auto rest = wires;
         rest[0].active = false;
         const bool fitsRest = FitProfile(rest, wireR, ballR, three, err);
         assert(ch.pieces.size() == (fitsRest ? 2u : 1u) && gaps.size() == (fitsRest ? 0u : 1u));
         printf("floor wire trimmed 50: %s\n", fitsRest ? "wires 2-4 hold a channel" : "no channel from 450");
         // The remaining wires 3 and 4 only: nothing fits, the piece is empty and the gap is said
         Fixture lower = DefaultFixture(kFixtureTrim, total);
         lower.cutoff[0] = 50.f;
         lower.cutoff[1] = 50.f;
         plan = PlanFittings({ lower }, wires, total, false);
         ch = BuildFittingChannel(frames, rowDist, wires, wireR, ballR, profile, { lower }, plan, &gaps);
         assert(ch.pieces.size() == 1 && gaps.size() == 1 && fabsf(gaps[0].from - 450.f) < 1e-3f && fabsf(gaps[0].to - total) < 1e-3f);
         // Trimmed at the start too, and a wire cut away completely
         Fixture both = DefaultFixture(kFixtureTrim, total);
         both.cutoff[3] = 600.f; // Past the route: clamped at the build, nothing is left of the wire
         plan = PlanFittings({ both }, wires, total, false);
         assert(plan.eff[0].cutoff[3] == total);
         // Terminal flags are absent when there is no hairpin
         assert(ch.triTerminal.empty() && ch.edgeTerminal.empty() && ch.vertTerminal.empty());
      }

      // A rail without end fittings: the tubes and the channel come out as the phase 2a builders gave them
      {
         std::vector<PathPoint> arc;
         for (int i = 0; i <= 720; i++)
         {
            const float a = 1.5707963f * (float)i / 720.f;
            arc.push_back({ 300.f * cosf(a), 300.f * sinf(a), 100.f - 40.f * (float)i / 720.f, i == 0 });
         }
         const auto bent = Route(arc, { 10.f, 10.f });
         const auto bentDist = RowDistances(bent);
#ifdef __FAST_MATH__
         const float tol = 2e-3f; // The compiler may fuse and reorder the two instantiations differently
#else
         const float tol = 0.f;
#endif
         const auto sameFloats = [tol](const void* a, const void* b, const size_t floats)
         {
            const float *fa = static_cast<const float*>(a), *fb = static_cast<const float*>(b);
            for (size_t k = 0; k < floats; k++)
               if (fabsf(fa[k] - fb[k]) > tol)
                  return false;
            return true;
         };
         for (int pass = 0; pass < 3; pass++)
         {
            const auto& fr = pass == 0 ? frames : bent;
            const auto& rd = pass == 0 ? rowDist : bentDist;
            const auto w = pass == 2 ? DefaultWires(6, true) : (pass == 1 ? DefaultWires(4, true) : DefaultWires(1, true));
            std::vector<Vx> vo, vn;
            std::vector<unsigned> io, in;
            Ref2a::BuildTubes<Vx>(fr, w, wireR, kDefaultSides, kDefaultCapBevel, vo, io);
            const FittingPlan none = PlanFittings({}, w, rd.back(), true);
            assert(!none.trims.Any() && !none.fitted.Any());
            BuildTubesTrimmed<Vx>(fr, rd, w, wireR, kDefaultSides, kDefaultCapBevel, none.trims, none.fitted, nullptr, vn, in);
            assert(vo.size() == vn.size() && io == in && sameFloats(vo.data(), vn.data(), vo.size() * 8));
            Profile pr;
            assert(FitProfile(w, wireR, ballR, pr, err));
            const Channel co = Ref2a::BuildChannel(fr, pr);
            std::vector<ChannelGap> gaps;
            const Channel cn = BuildFittingChannel(fr, rd, w, wireR, ballR, pr, {}, none, &gaps);
            assert(co.verts.size() == cn.verts.size() && co.tris == cn.tris && co.edges == cn.edges && co.rowSize == cn.rowSize && co.rows == cn.rows && gaps.empty());
            assert(co.verts.empty() || memcmp(co.verts.data(), cn.verts.data(), co.verts.size() * sizeof(V3)) == 0);
            assert(cn.pieces.size() == 1 && cn.quadIndices == cn.tris.size() && cn.triTerminal.empty());
            // The same with supports on, tubes first
            std::vector<FixtureShape> shapes;
            const std::vector<Fixture> supports { DefaultFixture(kFixtureRing, 100.f), DefaultFixture(kFixtureCradle, 150.f) };
            std::vector<Vx> v1, v2;
            std::vector<unsigned> i1, i2;
            BuildFixtures<Vx>(fr, rd, w, wireR, kDefaultSides, kDefaultCapBevel, ballR, pr, supports, v1, i1, shapes);
            BuildFixtures<Vx>(fr, rd, w, wireR, kDefaultSides, kDefaultCapBevel, ballR, pr, supports, v2, i2, shapes, &none.trims);
            assert(v1.size() == v2.size() && i1 == i2 && sameFloats(v1.data(), v2.data(), v1.size() * 8));
         }
      }

      // Mirror: twice gives the fitting back, no negative zero; a mirrored hairpin's centreline is the mirrored centreline
      {
         Fixture h = DefaultFixture(kFixtureHairpin, total), e = DefaultFixture(kFixtureElbow, total, &wires), t = DefaultFixture(kFixtureTrim, total);
         h.rotation = 40.f;
         h.fitOffset = 25.f;
         e.zAngle = 30.f;
         t.cutoff[1] = 12.f;
         for (Fixture* f : { &h, &e, &t })
         {
            Fixture m = *f;
            MirrorFixture(m);
            MirrorFixture(m);
            assert(memcmp(&m, f, sizeof(Fixture)) == 0 || (m.rotation == f->rotation && m.zAngle == f->zAngle && m.railA == f->railA && m.cutoff[1] == f->cutoff[1] && m.fitOffset == f->fitOffset));
         }
         Fixture zero = DefaultFixture(kFixtureElbow, total, &wires), zeroH = DefaultFixture(kFixtureHairpin, total);
         MirrorFixture(zero);
         MirrorFixture(zeroH);
         assert(!std::signbit(zero.zAngle) && !std::signbit(zeroH.rotation) && zeroH.rotation == 0.f);
         // The route runs along +Y: mirroring across it flips the wires' x only
         auto mw = wires;
         for (Wire& w : mw)
            w.x = -w.x;
         Fixture mh = h, me = e;
         MirrorFixture(mh);
         MirrorFixture(me);
         assert(fabsf(mh.rotation - 320.f) < 1e-3f && fabsf(me.zAngle + 30.f) < 1e-3f && mh.railA == h.railA && mh.fitOffset == h.fitOffset);
         FittingPlan p0 = PlanFittings({ h, e }, wires, total, false), p1 = PlanFittings({ mh, me }, mw, total, false);
         HairpinPath a, b;
         assert(HairpinPoints(p0.eff[0], frames, rowDist, wires, total, 6, 12, a) && HairpinPoints(p1.eff[0], frames, rowDist, mw, total, 6, 12, b) && a.pts.size() == b.pts.size());
         for (size_t k = 0; k < a.pts.size(); k++)
            assert(WireSweep::Len(V3 { -a.pts[k].x, a.pts[k].y, a.pts[k].z } - b.pts[k]) < 2e-3f);
         ElbowPath ea, eb;
         assert(ElbowPoints(p0.eff[1], frames, rowDist, wires, total, 2.f * wireR, ea) && ElbowPoints(p1.eff[1], frames, rowDist, mw, total, 2.f * wireR, eb) && ea.a.size() == eb.a.size());
         for (size_t k = 0; k < ea.a.size(); k++)
            assert(WireSweep::Len(V3 { -ea.a[k].x, ea.a[k].y, ea.a[k].z } - eb.a[k]) < 2e-3f && WireSweep::Len(V3 { -ea.b[k].x, ea.b[k].y, ea.b[k].z } - eb.b[k]) < 2e-3f);
      }

      // Everything together on a bent, banked, falling route, at both ends, 4 and 6 wires: nothing is NaN, every index in range
      {
         std::vector<PathPoint> arc;
         for (int i = 0; i <= 720; i++)
         {
            const float a = 1.5707963f * (float)i / 720.f;
            arc.push_back({ 300.f * cosf(a), 300.f * sinf(a), 100.f - 40.f * (float)i / 720.f, i == 0 });
         }
         const auto bent = Route(arc, { 10.f, 10.f });
         const auto bentDist = RowDistances(bent);
         const float len = bentDist.back();
         for (const int count : { 4, 6 })
         {
            const auto w = DefaultWires(count, true);
            Profile pr;
            assert(FitProfile(w, wireR, ballR, pr, err));
            for (const int end : { kEndStart, kEndEnd })
            {
               Fixture h = DefaultFixture(kFixtureHairpin, len), e = DefaultFixture(kFixtureElbow, len, &w), t = DefaultFixture(kFixtureTrim, len);
               h.endpoint = end;
               h.rotation = 25.f;
               e.endpoint = 1 - end;
               e.railA = 2;
               e.railB = 3;
               e.zAngle = -20.f;
               t.endpoint = end;
               t.cutoff[2] = 33.f;
               t.cutoff[3] = 20.f;
               const std::vector<Fixture> fx { DefaultFixture(kFixtureRing, 100.f), h, e, t, DefaultFixture(kFixtureStand, len * 0.5f) };
               const FittingPlan renderPlan = PlanFittings(fx, w, len, true), all = PlanFittings(fx, w, len, false);
               TubeRings rings;
               std::vector<Vx> v2;
               std::vector<unsigned> i2;
               std::vector<FixtureShape> shapes;
               BuildTubesTrimmed<Vx>(bent, bentDist, w, wireR, kDefaultSides, kDefaultCapBevel, renderPlan.trims, renderPlan.fitted, &rings, v2, i2);
               BuildFixtures<Vx>(bent, bentDist, w, wireR, kDefaultSides, kDefaultCapBevel, ballR, pr, fx, v2, i2, shapes, &renderPlan.trims);
               BuildEndFittings<Vx>(bent, bentDist, w, wireR, kDefaultSides, kDefaultCapBevel, fx, all, renderPlan, rings, v2, i2, shapes);
               assert(allFinite(v2));
               for (const unsigned ix : i2)
                  assert(ix < v2.size());
               std::vector<ChannelGap> gaps;
               const Channel ch = BuildFittingChannel(bent, bentDist, w, wireR, ballR, pr, fx, all, &gaps);
               for (const V3& q : ch.verts)
                  assert(vfinite(q));
               for (const unsigned ix : ch.tris)
                  assert(ix < ch.verts.size());
               for (const auto& ed : ch.edges)
                  assert(ed.first < ch.verts.size() && ed.second < ch.verts.size());
               for (const FixtureShape& s : shapes)
                  for (const V3& q : s.centerline)
                     assert(vfinite(q));
               assert(shapes[1].built && shapes[2].built && shapes[3].built);
               printf("fittings on a bent route, %d wires, hairpin at %s: %d render triangles, channel %d pieces %d triangles, %d gaps\n", count, end == kEndStart ? "start" : "end",
                  (int)(i2.size() / 3), (int)ch.pieces.size(), (int)(ch.tris.size() / 3), (int)gaps.size());
            }
         }
      }
      (void)at;
      printf("end fittings OK\n");
   }
   printf("wirerail_check OK\n");
   return 0;
}
