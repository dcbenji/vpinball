// Standalone self-check (one translation unit, with the minimal stand-ins the real headers need):
//   clang++ -std=c++20 -I src -I third-party/include src/ui/live/editor/RubberFitCheck.cpp -o /tmp/rf && /tmp/rf
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
using std::vector;
using std::min;
using std::max;
#define __forceinline inline
struct Vertex3D_NoTex2
{
   float x, y, z, nx, ny, nz, tu, tv;
};
#define RUBBERFIT_STANDALONE
#include "math/vector.h"
#include "math/MeshUtils.h"
#include "RubberFit.cpp"

using namespace VPX::EditorUI::RubberFit;

// The curve VPX renders for the drag points (Catmull-Rom with the smooth flags, as DragPoint::GetRgVertex)
static vector<Vertex2D> Render(const vector<Point> &pts)
{
   vector<Vertex2D> out;
   const size_t n = pts.size();
   const auto v = [&pts](size_t i) { return Vertex2D(pts[i].x, pts[i].y); };
   for (size_t i = 0; i < n; i++)
   {
      const size_t next = (i + 1) % n, prev = pts[i].smooth ? (i + n - 1) % n : i, foll = pts[next].smooth ? (i + 2) % n : next;
      CatmullCurve<2> cc;
      cc.SetCurve(v(prev), v(i), v(next), v(foll));
      for (int s = 0; s < 32; s++)
      {
         Vertex2D p;
         cc.GetPointAt(static_cast<float>(s) / 32.f, &p);
         out.push_back(p);
      }
   }
   return out;
}

// Smallest distance of the rendered band to a post, beyond the post radius plus half the thickness (>= -tolerance: clears every post)
static float MinClearance(const vector<Point> &pts, const vector<Post> &posts, float thickness)
{
   float m = 1e9f;
   for (const Vertex2D &p : Render(pts))
      for (const Post &c : posts)
         m = min(m, std::hypot(p.x - c.x, p.y - c.y) - (c.r + 0.5f * thickness));
   return m;
}

static vector<Point> Ok(const vector<Post> &posts, float thickness, float roundness)
{
   vector<Point> pts;
   std::string err;
   const bool ok = Fit(posts, thickness, roundness, pts, err);
   if (!ok)
      std::printf("unexpected error: %s\n", err.c_str());
   assert(ok && pts.size() >= 3);
   return pts;
}

int main()
{
   constexpr float tol = kRenderTolerance + 0.01f;
   // Two posts: a stadium, clear of both posts and touching them, at any roundness
   const vector<Post> two = { { 0, 0, 10 }, { 100, 0, 10 } };
   for (const float rn : { 0.f, 0.5f, 1.f })
   {
      const float c = MinClearance(Ok(two, 8.f, rn), two, 8.f);
      std::printf("two posts roundness %.1f: clearance %.4f\n", rn, c);
      assert(c > -tol && c < tol);
   }
   // Three posts, plus a small one inside the hull: the enclosed post is ignored (same points)
   const vector<Post> three = { { 0, 0, 10 }, { 100, 0, 6 }, { 50, 80, 14 } };
   vector<Post> four = three;
   four.push_back({ 50, 25, 5 });
   const auto a = Ok(three, 8.f, 0.f), b = Ok(four, 8.f, 0.f);
   assert(a.size() == b.size());
   for (size_t i = 0; i < a.size(); i++)
      assert(a[i].x == b[i].x && a[i].y == b[i].y && a[i].smooth == b[i].smooth);
   assert(MinClearance(a, three, 8.f) < tol);
   // Roundness 0 and 0.5 both clear every post, including the enclosed one, and 0.5 pulls the free spans in
   for (const float rn : { 0.f, 0.25f, 0.5f, 1.f })
      assert(MinClearance(Ok(four, 8.f, rn), four, 8.f) > -tol);
   assert(MinClearance(Ok(three, 8.f, 0.5f), three, 8.f) > MinClearance(a, three, 8.f) - 1e-3f);
   // Single post: the full circle (smooth points on the grown circle)
   const vector<Post> one = { { 30, 40, 10 } };
   const auto c = Ok(one, 8.f, 0.f);
   for (const Point &p : c)
      assert(p.smooth && std::fabs(std::hypot(p.x - 30.f, p.y - 40.f) - 14.f) < 1e-3f);
   assert(std::fabs(MinClearance(c, one, 8.f)) < tol);
   // Coincident posts are one, a smaller one stacked inside a bigger one is enclosed
   const auto d = Ok({ { 0, 0, 10 }, { 0, 0, 10 }, { 100, 0, 10 } }, 8.f, 0.f), e = Ok({ { 0, 0, 10 }, { 100, 0, 10 }, { 0, 0, 4 } }, 8.f, 0.f);
   const auto f = Ok(two, 8.f, 0.f);
   assert(d.size() == f.size() && e.size() == f.size());
   // Bad input is an error and leaves the points alone
   vector<Point> keep = { { 1.f, 2.f, true } };
   std::string err;
   assert(!Fit({ { 0, 0, 10 }, { NAN, 0, 10 } }, 8.f, 0.f, keep, err) && !err.empty() && keep.size() == 1);
   assert(!Fit({ { 0, 0, 10 }, { 50, 0, INFINITY } }, 8.f, 0.f, keep, err));
   assert(!Fit({ { 0, 0, 10 }, { 50, 0, 0 } }, 8.f, 0.f, keep, err));
   assert(!Fit(two, NAN, 0.f, keep, err));
   assert(!Fit(two, 8.f, 1.5f, keep, err));
   assert(!Fit({}, 8.f, 0.f, keep, err));
   std::puts("RubberFit ok");
   return 0;
}
