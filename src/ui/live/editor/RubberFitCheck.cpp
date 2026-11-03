// Standalone self-check: clang++ -std=c++20 RubberFit.cpp RubberFitCheck.cpp -o /tmp/rf && /tmp/rf
#include "RubberFit.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace VPX::EditorUI::RubberFit;

static float distTo(const Vec2 &p, const Post &c) { return std::hypot(p.x - c.x, p.y - c.y); }

// Every point lies on one of the grown circles
static void onCircles(const std::vector<Vec2> &pts, const std::vector<Post> &posts, float grow)
{
   for (const Vec2 &p : pts)
   {
      bool ok = false;
      for (const Post &c : posts)
         ok |= std::fabs(distTo(p, c) - (c.r + grow)) < 1e-2f;
      assert(ok);
   }
}

int main()
{
   // Two posts: a stadium, 14 = 10 + 8/2
   const std::vector<Post> two = { { 0, 0, 10 }, { 100, 0, 10 } };
   const auto a = Fit(two, 8.f, 0.f, Hull);
   assert(a.size() >= 4);
   onCircles(a, two, 4.f);
   // Three hull posts (unequal radii): the loop passes outside every post by at least the grown radius
   const std::vector<Post> three = { { 0, 0, 10 }, { 100, 0, 6 }, { 50, 80, 14 } };
   const auto b = Fit(three, 8.f, 2.f, Hull);
   assert(b.size() >= 3 * 3);
   onCircles(b, three, 6.f);
   for (const Vec2 &p : b)
      for (const Post &c : three)
         assert(distTo(p, c) >= c.r + 6.f - 1e-2f);
   // An inner post is ignored by the hull
   const std::vector<Post> inner = { { 0, 0, 10 }, { 100, 0, 10 }, { 50, 80, 10 }, { 50, 25, 5 } };
   assert(Fit(inner, 8.f, 0.f, Hull).size() == Fit({ inner.begin(), inner.begin() + 3 }, 8.f, 0.f, Hull).size());
   // Selection order with a concave corner: the post at (50, 20) is pushed in, the band passes on its inner side
   const std::vector<Post> dent = { { 0, 0, 10 }, { 100, 0, 10 }, { 100, 100, 10 }, { 0, 100, 10 }, { 50, 80, 10 } };
   // order 0,1,2,4,3: corner at post 4 is concave (the polygon goes 100,100 -> 50,80 -> 0,100)
   const std::vector<Post> poly = { dent[0], dent[1], dent[2], dent[4], dent[3] };
   const auto d = Fit(poly, 8.f, 0.f, SelectionOrder);
   onCircles(d, poly, 4.f);
   for (const Vec2 &p : d) // No point of the band falls inside a post
      for (const Post &c : poly)
         assert(distTo(p, c) >= c.r + 4.f - 1e-2f);
   std::puts("RubberFit ok");
   return 0;
}
