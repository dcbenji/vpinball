// license:GPLv3+

// Playfield Maker phase 2 (P2-5, a prototype behind the board's Cut Insert Pockets switch): what fills a cut insert pocket, from the pocket's contour
// (the hole loop exactly as the board's walls use it, PlasticMesh::Result::contours).
//   The insert body (the "puck"): a closed shell, its top face at z = top (just under the board face), side walls down to top - depth and a bottom face.
//   The ink cap: the pocket's top face again at z = capZ (just over the board face), where the printed ink covers the insert (its image's alpha).
// Every vertex gets table UVs (u = a x + b y + c, v = d x + e y + f), so the insert's colour and the ink come from the table-wide art.
// No engine dependency (standard library and earcut only; PlasticMesh.h only for its plain structs), so it can be checked standalone:
//   clang++ -std=c++20 -I src tools/insertpocket_check.cpp
// Triangle winding follows PlasticMesh: the right-hand geometric normal of every triangle agrees with its vertex normals (up on the top faces, down on the
// bottom face, out of the insert on the walls); the shell is closed (every edge, welded by position, is shared by exactly two faces in opposite directions).

#pragma once

#include "PlasticMesh.h"
#include "earcut.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace InsertPocket
{

using PlasticMesh::CurvePoint;
using PlasticMesh::Mesh;
using PlasticMesh::Vertex;

struct Params
{
   float top = -0.1f; // z of the insert's top face (VPU): just under the board face, so the board's own top never fights with it
   float depth = 11.8f; // the frosted body, 0.25 in (VPU)
   float capZ = 0.05f; // z of the ink cap: just over the board face
   float uvm[6] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f }; // table XY to UV, as PlasticMesh::Params::uvm
};

// Twice the signed area (shoelace): > 0 counter-clockwise, as PlasticMesh's outline
inline double Area2(const std::vector<CurvePoint>& c)
{
   double a = 0.0;
   for (size_t i = 0; i < c.size(); i++)
   {
      const CurvePoint &u = c[i], &v = c[(i + 1) % c.size()];
      a += (double)u.x * v.y - (double)v.x * u.y;
   }
   return a;
}

// The contour as the builders use it: points closer than 1e-4 to the one before dropped (a repeated closing point too), counter-clockwise. Empty when fewer than
// 3 points are left or the loop has no area
inline std::vector<CurvePoint> Prepare(const std::vector<CurvePoint>& contour)
{
   std::vector<CurvePoint> p;
   p.reserve(contour.size());
   for (const CurvePoint& c : contour)
      if (p.empty() || std::fabs(c.x - p.back().x) + std::fabs(c.y - p.back().y) > 1e-4f)
         p.push_back(c);
   while (p.size() > 1 && std::fabs(p.front().x - p.back().x) + std::fabs(p.front().y - p.back().y) <= 1e-4f)
      p.pop_back();
   if (p.size() < 3)
      return {};
   const double a = Area2(p);
   if (!(std::fabs(a) > 1e-6))
      return {};
   if (a < 0.0)
      for (size_t i = 0, j = p.size() - 1; i < j; i++, j--)
         std::swap(p[i], p[j]);
   return p;
}

// The face of a prepared (counter-clockwise) contour: triangles as index triples into it, each counter-clockwise (up-facing), slivers of no area dropped
inline std::vector<std::array<uint32_t, 3>> Triangulate(const std::vector<CurvePoint>& p)
{
   std::vector<std::vector<std::array<double, 2>>> poly(1);
   poly[0].reserve(p.size());
   for (const CurvePoint& c : p)
      poly[0].push_back({ c.x, c.y });
   const std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
   std::vector<std::array<uint32_t, 3>> tris;
   tris.reserve(idx.size() / 3);
   for (size_t i = 0; i + 2 < idx.size(); i += 3)
   {
      std::array<uint32_t, 3> t { idx[i], idx[i + 1], idx[i + 2] };
      const CurvePoint &a = p[t[0]], &b = p[t[1]], &c = p[t[2]];
      const double cr = ((double)b.x - a.x) * ((double)c.y - a.y) - ((double)b.y - a.y) * ((double)c.x - a.x);
      if (!(std::fabs(cr) > 1e-9))
         continue;
      if (cr < 0.0)
         std::swap(t[1], t[2]);
      tris.push_back(t);
   }
   return tris;
}

inline void TableUv(const Params& prm, Vertex& v)
{
   v.tu = prm.uvm[0] * v.x + prm.uvm[1] * v.y + prm.uvm[2];
   v.tv = prm.uvm[3] * v.x + prm.uvm[4] * v.y + prm.uvm[5];
}

// A flat face of the prepared contour at height z, facing up (up = true) or down
inline void AddFace(const std::vector<CurvePoint>& p, const std::vector<std::array<uint32_t, 3>>& tris, const float z, const bool up, const Params& prm, Mesh& m)
{
   const uint32_t base = static_cast<uint32_t>(m.verts.size());
   for (const CurvePoint& c : p)
   {
      Vertex v { c.x, c.y, z, 0.f, 0.f, up ? 1.f : -1.f, 0.f, 0.f };
      TableUv(prm, v);
      m.verts.push_back(v);
   }
   for (const std::array<uint32_t, 3>& t : tris)
      if (up)
         m.idx.insert(m.idx.end(), { base + t[0], base + t[1], base + t[2] });
      else
         m.idx.insert(m.idx.end(), { base + t[0], base + t[2], base + t[1] });
}

// The insert body over one pocket. False (mesh untouched) for a contour of fewer than 3 points, no area, or a depth that is not positive
inline bool AddPuck(const std::vector<CurvePoint>& contour, const Params& prm, Mesh& m)
{
   if (!(prm.depth > 0.f))
      return false;
   const std::vector<CurvePoint> p = Prepare(contour);
   if (p.empty())
      return false;
   const std::vector<std::array<uint32_t, 3>> tris = Triangulate(p);
   if (tris.empty())
      return false;
   const float zTop = prm.top, zBot = prm.top - prm.depth;
   AddFace(p, tris, zTop, true, prm, m);
   AddFace(p, tris, zBot, false, prm, m);
   // The walls: the right-hand normal (dy, -dx) of a counter-clockwise loop points out of the insert. Smooth across shallow turns (a round insert), sharp at
   // corners, as the board's hole walls
   const size_t n = p.size();
   std::vector<std::array<float, 2>> en(n);
   for (size_t i = 0; i < n; i++)
   {
      const CurvePoint &a = p[i], &b = p[(i + 1) % n];
      const float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
      en[i] = l > 1e-6f ? std::array<float, 2> { dy / l, -dx / l } : std::array<float, 2> { 0.f, 0.f };
   }
   const auto vertexNormal = [&](const size_t i, const size_t edge)
   {
      const std::array<float, 2> &u = en[(i + n - 1) % n], &w = en[i % n];
      if (u[0] * w[0] + u[1] * w[1] > 0.866f)
      {
         const float sx = u[0] + w[0], sy = u[1] + w[1], l = std::sqrt(sx * sx + sy * sy);
         return l > 1e-6f ? std::array<float, 2> { sx / l, sy / l } : en[edge];
      }
      return en[edge];
   };
   for (size_t i = 0; i < n; i++)
   {
      const size_t j = (i + 1) % n;
      const CurvePoint &a = p[i], &b = p[j];
      const std::array<float, 2> na = vertexNormal(i, i), nb = vertexNormal(j, i);
      const uint32_t base = static_cast<uint32_t>(m.verts.size());
      Vertex q[4] = { { a.x, a.y, zBot, na[0], na[1], 0.f, 0.f, 0.f }, { b.x, b.y, zBot, nb[0], nb[1], 0.f, 0.f, 0.f }, { b.x, b.y, zTop, nb[0], nb[1], 0.f, 0.f, 0.f },
         { a.x, a.y, zTop, na[0], na[1], 0.f, 0.f, 0.f } };
      for (Vertex& v : q)
      {
         TableUv(prm, v);
         m.verts.push_back(v);
      }
      m.idx.insert(m.idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 }); // As the board's walls: (a0, b0, b1), (a0, b1, a1)
   }
   return true;
}

// The ink cap over one pocket: its face at capZ, facing up. False (mesh untouched) as AddPuck
inline bool AddCap(const std::vector<CurvePoint>& contour, const Params& prm, Mesh& m)
{
   const std::vector<CurvePoint> p = Prepare(contour);
   if (p.empty())
      return false;
   const std::vector<std::array<uint32_t, 3>> tris = Triangulate(p);
   if (tris.empty())
      return false;
   AddFace(p, tris, prm.capZ, true, prm, m);
   return true;
}

}
