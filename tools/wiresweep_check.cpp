// Standalone check of src/math/wiresweep.h: clang++ -std=c++20 -I src tools/wiresweep_check.cpp -o /tmp/wsc && /tmp/wsc
#include <cassert>
#include <cstdio>
#include "math/wiresweep.h"

struct V { float x, y, z, nx, ny, nz, tu, tv; };
struct P { float x, y, z; };

int main()
{
   using namespace WireSweep;
   const std::vector<P> line { { 0, 0, 10 }, { 100, 0, 10 } };
   std::vector<V> v;
   std::vector<unsigned> i;
   assert(BuildWireform<V>(line, 3.f, 8, 1, 12.f, Caps::None, v, i));
   assert(v.size() == 16 && i.size() == 48);
   for (const V& a : v) // radius 3 around the axis y=0,z=10, unit outward normals, indices in range
   {
      assert(fabsf(sqrtf(a.y * a.y + (a.z - 10.f) * (a.z - 10.f)) - 3.f) < 1e-4f);
      assert(fabsf(sqrtf(a.nx * a.nx + a.ny * a.ny + a.nz * a.nz) - 1.f) < 1e-4f);
   }
   for (unsigned k : i)
      assert(k < v.size());
   // triangle winding is outward
   for (size_t t = 0; t < i.size(); t += 3)
   {
      const V &a = v[i[t]], &b = v[i[t + 1]], &c = v[i[t + 2]];
      const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
      const float cx = uy * wz - uz * wy, cy = uz * wx - ux * wz, cz = ux * wy - uy * wx;
      assert(cx * a.nx + cy * a.ny + cz * a.nz > 0.f);
   }
   v.clear(); i.clear();
   assert(BuildWireform<V>(line, 3.f, 8, 3, 12.f, Caps::None, v, i));
   assert(v.size() == 48 && i.size() == 144);
   // rails at y = -12, 0, +12 (horizontal normal of +X travel is +Y)
   float lo = 1e9f, hi = -1e9f;
   for (const V& a : v) { lo = fminf(lo, a.y); hi = fmaxf(hi, a.y); }
   assert(fabsf(lo + 15.f) < 1e-3f && fabsf(hi - 15.f) < 1e-3f);
   v.clear(); i.clear();
   assert(BuildWireform<V>(line, 3.f, 8, 1, 12.f, Caps::Flat, v, i));
   assert(v.size() == 16 + 2 * 9 && i.size() == 48 + 2 * 24);
   v.clear(); i.clear();
   assert(BuildWireform<V>(line, 3.f, 8, 1, 12.f, Caps::Round, v, i));
   assert(v.size() == 8 * (2 + 3 + 4) && i.size() == 6 * 8 * (4 + 3));
   for (unsigned k : i)
      assert(k < v.size());
   assert(!BuildWireform<V>(std::vector<P> { { 0, 0, 0 } }, 3.f, 8, 1, 12.f, Caps::None, v, i));
   // Path through an exactly vertical tangent: every ring must keep the full radius
   v.clear(); i.clear();
   const std::vector<P> bent { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 0, 10 }, { 10, 10, 10 } };
   assert(BuildWireform<V>(bent, 1.f, 8, 1, 12.f, Caps::None, v, i));
   assert(v.size() == 4 * 8);
   for (size_t r = 0; r < 4; r++)
   {
      float cx = 0, cy = 0, cz = 0;
      for (int j = 0; j < 8; j++) { cx += v[r * 8 + j].x; cy += v[r * 8 + j].y; cz += v[r * 8 + j].z; }
      cx /= 8; cy /= 8; cz /= 8;
      for (int j = 0; j < 8; j++)
      {
         const V& a = v[r * 8 + j];
         assert(fabsf(sqrtf((a.x - cx) * (a.x - cx) + (a.y - cy) * (a.y - cy) + (a.z - cz) * (a.z - cz)) - 1.f) < 1e-3f);
      }
   }
   puts("wiresweep ok");
}
