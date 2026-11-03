// license:GPLv3+

// Tube sweep along a 3D centerline (parallel-transported ring frame), used by the Wireform primitive generator.
// Header only and templated on the vertex / point types so it can be checked standalone (tools/wiresweep_check.cpp).
// Vertex needs x,y,z,nx,ny,nz,tu,tv floats (Vertex3D_NoTex2), Point needs x,y,z floats (Vertex3Ds, RenderVertex3D).
// The ring/quad layout follows Ramp::CreateWire / Ramp::GenerateWireMesh (Ramp itself does not call this).

#pragma once

#include <cmath>
#include <vector>

namespace WireSweep
{

enum class Caps { None = 0, Flat = 1, Round = 2 };

struct V3
{
   float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Len(V3 a) { return sqrtf(Dot(a, a)); }
inline V3 Norm(V3 a)
{
   const float l = Len(a);
   return l > 1e-12f ? a * (1.f / l) : V3 { 0.f, 0.f, 0.f };
}

// Appends one tube of the given radius along the centerline (coincident points are dropped), optionally capped.
// Returns false (nothing appended) if fewer than 2 distinct points remain.
template <class Vertex, class Point>
bool SweepTube(const std::vector<Point>& centerline, const float radius, int sides, const Caps caps, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
{
   sides = sides < 3 ? 3 : sides;
   std::vector<V3> p;
   for (const Point& c : centerline)
   {
      const V3 v { c.x, c.y, c.z };
      if (p.empty() || Len(v - p.back()) > 1e-4f)
         p.push_back(v);
   }
   const int n = (int)p.size();
   if (n < 2)
      return false;

   // Arc length parameter for u
   std::vector<float> s(n, 0.f);
   for (int i = 1; i < n; i++)
      s[i] = s[i - 1] + Len(p[i] - p[i - 1]);
   const float total = s[n - 1] > 0.f ? s[n - 1] : 1.f;

   const unsigned base = (unsigned)verts.size();
   const float twoPi = 6.28318530717958647692f;
   V3 prevN { 0.f, 0.f, 0.f };
   V3 firstT { 0.f, 0.f, 0.f }, lastT { 0.f, 0.f, 0.f };
   V3 firstN { 0.f, 0.f, 0.f }, lastN { 0.f, 0.f, 0.f };
   V3 firstB { 0.f, 0.f, 0.f }, lastB { 0.f, 0.f, 0.f };

   // Emits a ring of 'sides' vertices around 'c' (radius r), the normal tilted by 'tilt' along the tangent for round caps
   const auto ring = [&](const V3& c, const V3& t, const V3& nrm, const V3& bin, const float r, const float tilt, const float u)
   {
      const float ct = cosf(tilt), st = sinf(tilt);
      for (int j = 0; j < sides; j++)
      {
         const float a = twoPi * (float)j / (float)sides;
         const V3 radial = nrm * cosf(a) + bin * sinf(a);
         const V3 pos = c + radial * r;
         const V3 nn = radial * ct + t * st;
         Vertex vtx;
         vtx.x = pos.x;
         vtx.y = pos.y;
         vtx.z = pos.z;
         vtx.nx = nn.x;
         vtx.ny = nn.y;
         vtx.nz = nn.z;
         vtx.tu = u;
         vtx.tv = (float)j / (float)sides;
         verts.push_back(vtx);
      }
   };

   int rings = 0;
   for (int i = 0; i < n; i++)
   {
      V3 t = Norm(i == n - 1 ? p[i] - p[i - 1] : p[i + 1] - p[i]);
      V3 nrm, bin;
      if (i == 0)
      {
         // Start frame: normal as horizontal as possible (vertical start tangents fall back to X)
         const V3 up = fabsf(t.z) > 0.999f ? V3 { 1.f, 0.f, 0.f } : V3 { 0.f, 0.f, 1.f };
         nrm = Norm(Cross(t, up));
         bin = Norm(Cross(t, nrm));
      }
      else
      {
         // Parallel transport (same update as Ramp::CreateWire)
         // Parallel transport: previous normal projected onto the new ring plane (stays valid through vertical tangents)
         nrm = Norm(prevN - t * Dot(prevN, t));
         if (Len(nrm) < 0.5f)
            nrm = Norm(Cross(t, fabsf(t.z) > 0.999f ? V3 { 1.f, 0.f, 0.f } : V3 { 0.f, 0.f, 1.f }));
         bin = Norm(Cross(t, nrm));
      }
      prevN = nrm;
      if (i == 0)
      {
         firstT = t;
         firstN = nrm;
         firstB = bin;
      }
      lastT = t;
      lastN = nrm;
      lastB = bin;
      ring(p[i], t, nrm, bin, radius, 0.f, s[i] / total);
      rings++;
   }

   // Round caps: extra rings closing a hemisphere, the last one collapses to the tip
   constexpr int capRings = 3;
   int startCapRings = 0, endCapRings = 0;
   if (caps == Caps::Round)
   {
      // End cap rings go after the sweep rings
      for (int k = 1; k <= capRings; k++)
      {
         const float phi = 1.57079632679489661923f * (float)k / (float)capRings;
         ring(p[n - 1] + lastT * (radius * sinf(phi)), lastT, lastN, lastB, radius * cosf(phi), phi, 1.f);
         endCapRings++;
      }
   }

   const auto strip = [&](const unsigned first, const int count)
   {
      for (int i = 0; i < count - 1; i++)
         for (int j = 0; j < sides; j++)
         {
            const unsigned q0 = first + i * sides + j;
            const unsigned q1 = first + i * sides + ((j + 1) % sides);
            const unsigned q2 = first + (i + 1) * sides + j;
            const unsigned q3 = first + (i + 1) * sides + ((j + 1) % sides);
            idx.insert(idx.end(), { q0, q1, q2, q3, q2, q1 }); // same quad split as Ramp::GenerateWireMesh
         }
   };
   strip(base, rings + endCapRings);

   if (caps == Caps::Round)
   {
      // Start cap: its own strip, built from the tip towards the first ring so the winding stays outward
      const unsigned first = (unsigned)verts.size();
      for (int k = capRings; k >= 1; k--)
      {
         const float phi = 1.57079632679489661923f * (float)k / (float)capRings;
         ring(p[0] - firstT * (radius * sinf(phi)), firstT * -1.f, firstN, firstB, radius * cosf(phi), phi, 0.f);
         startCapRings++;
      }
      // Closing ring at the equator (same radial frame as the sweep rings, so the strip winds like the main one)
      ring(p[0], firstT * -1.f, firstN, firstB, radius, 0.f, 0.f);
      startCapRings++;
      strip(first, startCapRings);
   }
   else if (caps == Caps::Flat)
   {
      for (int end = 0; end < 2; end++)
      {
         const V3 c = end ? p[n - 1] : p[0];
         const V3 t = end ? lastT : firstT * -1.f;
         const V3 nrm = end ? lastN : firstN;
         const V3 bin = end ? lastB : firstB;
         const unsigned first = (unsigned)verts.size();
         Vertex cv;
         cv.x = c.x;
         cv.y = c.y;
         cv.z = c.z;
         cv.nx = t.x;
         cv.ny = t.y;
         cv.nz = t.z;
         cv.tu = end ? 1.f : 0.f;
         cv.tv = 0.5f;
         verts.push_back(cv);
         for (int j = 0; j < sides; j++)
         {
            const float a = twoPi * (float)j / (float)sides;
            const V3 pos = c + (nrm * cosf(a) + bin * sinf(a)) * radius;
            Vertex vtx;
            vtx.x = pos.x;
            vtx.y = pos.y;
            vtx.z = pos.z;
            vtx.nx = t.x;
            vtx.ny = t.y;
            vtx.nz = t.z;
            vtx.tu = cv.tu;
            vtx.tv = (float)j / (float)sides;
            verts.push_back(vtx);
         }
         for (int j = 0; j < sides; j++)
         {
            const unsigned a0 = first + 1 + j, a1 = first + 1 + ((j + 1) % sides);
            if (end)
               idx.insert(idx.end(), { first, a0, a1 });
            else
               idx.insert(idx.end(), { first, a1, a0 });
         }
      }
   }
   return true;
}

// Wireform mesh: 'rails' parallel tubes (1: offset 0, 2: +-spacing/2, 3: -spacing, 0, +spacing) offset along the horizontal normal of the path
template <class Vertex, class Point>
bool BuildWireform(const std::vector<Point>& path, const float radius, int sides, const int rails, const float spacing, const Caps caps, std::vector<Vertex>& verts, std::vector<unsigned>& idx)
{
   const int numRails = rails < 1 ? 1 : (rails > 3 ? 3 : rails);
   const int n = (int)path.size();
   if (n < 2)
      return false;
   // Horizontal (XY plane) normal per point from the neighbours; vertical stretches keep the previous one
   std::vector<V3> hn(n);
   V3 prev { 1.f, 0.f, 0.f };
   for (int i = 0; i < n; i++)
   {
      const Point& a = path[i == 0 ? 0 : i - 1];
      const Point& b = path[i == n - 1 ? n - 1 : i + 1];
      V3 h { -(b.y - a.y), b.x - a.x, 0.f };
      if (Len(h) < 1e-6f)
         h = prev;
      hn[i] = prev = Norm(h);
   }
   bool any = false;
   for (int r = 0; r < numRails; r++)
   {
      const float off = numRails == 1 ? 0.f : (numRails == 2 ? (r == 0 ? -0.5f : 0.5f) * spacing : (float)(r - 1) * spacing);
      std::vector<V3> line(n);
      for (int i = 0; i < n; i++)
         line[i] = V3 { path[i].x, path[i].y, path[i].z } + hn[i] * off;
      any |= SweepTube<Vertex>(line, radius, sides, caps, verts, idx);
   }
   return any;
}

}
