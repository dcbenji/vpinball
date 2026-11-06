// license:GPLv3+

// Plastic from Atlas mesh builder: a closed outline (corner points and arc peaks) with circular drill holes becomes three meshes,
// the bottom cap (the printed side, normals up), the top cap, and the walls with a bevel on the top side.
// No engine dependency, so it can be checked standalone (tools/plasticmesh_check.cpp: clang++ -std=c++20 -I src tools/plasticmesh_check.cpp src/math/PlasticMesh.cpp).
// Triangle winding follows WireSweep: the right-hand geometric normal of every triangle agrees with its vertex normals.

#pragma once

#include <vector>

namespace PlasticMesh
{

// One traced point. Corner (smooth = false): a corner or a tangent point. Smooth: the peak of an arc through its two neighbours
// (the circle through 3 points); between two consecutive smooth points the arc ends at their midpoint.
struct CurvePoint
{
   float x, y;
   bool smooth;
};

// A drill hole: circle centre and diameter (VPU). A pill (slot) is the capsule of that diameter between (x, y) and (x2, y2).
// A polygon hole: with `polygon` of 3 or more points set, the hole is that polygon (x, y, diameter, x2, y2 and pill are ignored; its points are
// straight corners, the smooth flag is ignored too, flatten curves first). Either winding is accepted. A polygon with fewer than 3 distinct points,
// or with any point outside the outline, is skipped and counted in skippedHoles. Plastics never set it.
// Holes (round, pill or polygon) must neither overlap each other nor nest, and must stay clear of the outline; Build does not check that between holes
// (crossingHoles only warns about the outline) and the cap of an overlapping or nested hole comes out wrong (partly filled). The caller drops those.
struct Hole
{
   float x = 0.f, y = 0.f, diameter = 0.f;
   float x2 = 0.f, y2 = 0.f;
   bool pill = false;
   std::vector<CurvePoint> polygon;
};

struct Params
{
   float height = 50.f; // z of the underside (the bottom cap)
   float thickness = 5.6f;
   float bevel = 1.9f; // top side only, offset inward so the part does not grow (and into the holes, away from the material)
   int bevelSegments = 2; // 1 = chamfer, more = rounded
   float uvm[6] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f }; // u = a x + b y + c, v = d x + e y + f (table XY to UV)
   bool bottomCap = true; // false: Result.bottom stays empty (no bottom face is built); the top cap and the walls are unchanged
   float flatten = 0.25f; // max chord error of flattened arcs and holes, VPU (FlattenTolerance of the Curve Detail)
};

constexpr int kMaxArcSegments = 96; // Hard cap on the segments of any one arc or hole circle (a pill loop is 96 points at most)

// Flatten tolerance of the Curve Detail 1..5 (1 = 0.25 VPU, the default)
inline float FlattenTolerance(int detail)
{
   detail = detail < 1 ? 1 : detail > 5 ? 5 : detail;
   return 0.25f / (float)(detail * detail);
}

struct Vertex
{
   float x, y, z, nx, ny, nz, tu, tv;
};

struct Mesh
{
   std::vector<Vertex> verts;
   std::vector<unsigned> idx;
};

struct Result
{
   Mesh bottom; // the printed side: z = height, normals up (empty with Params::bottomCap = false)
   Mesh top; // the cap at z = height + thickness, inset by the bevel
   Mesh bevel; // walls plus bevel rings
   int skippedHoles = 0; // holes whose centre lies outside the outline
   int crossingHoles = 0; // holes (kept as they are) whose circle reaches over the outline edge or into the band r + 2 * bevel of it: a warning only
};

// The closed outline flattened to a polyline (no repeated first point). Arcs deviate from the true circle by at most `tol`.
std::vector<CurvePoint> FlattenOutline(const std::vector<CurvePoint>& outline, float tol);

// The polygon a hole is built as (a circle, a capsule for a pill, or the polygon itself), clockwise, no repeated first point; flattened to the chord error `tol`.
std::vector<CurvePoint> HoleOutline(const Hole& hole, float tol);

// False (result untouched) with fewer than 3 outline points or a degenerate outline.
bool Build(const std::vector<CurvePoint>& outline, const std::vector<Hole>& holes, const Params& params, Result& out);

}
