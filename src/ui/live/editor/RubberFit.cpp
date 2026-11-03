// license:GPLv3+

#include "RubberFit.h"

#include <algorithm>
#include <cmath>

namespace VPX::EditorUI::RubberFit
{

namespace
{
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 2.f * kPi;

struct Circle
{
   float x, y, r;
   int side; // +1: the band goes counter clockwise around it (the post is on its left), -1: clockwise
};

float Mod2Pi(float a)
{
   a = std::fmod(a, kTwoPi);
   return a < 0.f ? a + kTwoPi : a;
}

float Cross(const Vec2 &o, const Vec2 &a, const Vec2 &b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Indices of the circles on the convex hull, counter clockwise: hull of points sampled on each circle (which circles contribute, and in
// which order), the tangents themselves are computed exactly afterwards
std::vector<int> HullOrder(const std::vector<Circle> &c)
{
   struct P
   {
      Vec2 p;
      int id;
   };
   std::vector<P> pts;
   constexpr int kSamples = 72;
   for (size_t i = 0; i < c.size(); i++)
      for (int k = 0; k < kSamples; k++)
      {
         const float a = kTwoPi * static_cast<float>(k) / kSamples;
         pts.push_back({ { c[i].x + c[i].r * std::cos(a), c[i].y + c[i].r * std::sin(a) }, static_cast<int>(i) });
      }
   std::sort(pts.begin(), pts.end(), [](const P &a, const P &b) { return a.p.x < b.p.x || (a.p.x == b.p.x && a.p.y < b.p.y); });
   std::vector<P> h(2 * pts.size());
   size_t n = 0;
   for (size_t i = 0; i < pts.size(); i++) // Andrew's monotone chain
   {
      while (n >= 2 && Cross(h[n - 2].p, h[n - 1].p, pts[i].p) <= 0.f)
         n--;
      h[n++] = pts[i];
   }
   for (size_t i = pts.size() - 1, lo = n + 1; i > 0; i--)
   {
      while (n >= lo && Cross(h[n - 2].p, h[n - 1].p, pts[i - 1].p) <= 0.f)
         n--;
      h[n++] = pts[i - 1];
   }
   std::vector<int> order;
   for (size_t i = 0; i + 1 < n; i++)
      if (order.empty() || order.back() != h[i].id)
         order.push_back(h[i].id);
   while (order.size() > 1 && order.front() == order.back())
      order.pop_back();
   return order;
}

// Walks the circles in order (a closed loop) and emits the arcs joined by tangents
std::vector<Vec2> Walk(const std::vector<Circle> &c, const float maxStep)
{
   const size_t n = c.size();
   std::vector<Vec2> out;
   if (n == 1)
   {
      const int steps = std::max(3, static_cast<int>(std::ceil(kTwoPi / maxStep)));
      for (int k = 0; k < steps; k++)
      {
         const float a = kTwoPi * static_cast<float>(k) / static_cast<float>(steps);
         out.push_back({ c[0].x + c[0].r * std::cos(a), c[0].y + c[0].r * std::sin(a) });
      }
      return out;
   }
   // Angle (around its centre) of the contact point of the tangent leaving circle i towards circle j. The path travels from i to j with the
   // normal n on its left; the contact on a counter clockwise circle is centre - r n, on a clockwise one centre + r n
   const auto tangentAngle = [&c](size_t i, size_t j, float &angleOnI, float &angleOnJ)
   {
      const float dx = c[j].x - c[i].x, dy = c[j].y - c[i].y;
      const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-6f);
      const float k = std::clamp((static_cast<float>(c[j].side) * c[j].r - static_cast<float>(c[i].side) * c[i].r) / len, -1.f, 1.f);
      const float na = std::atan2(dy, dx) + std::acos(k); // Angle of the normal
      angleOnI = na + (c[i].side > 0 ? kPi : 0.f); // Direction centre -> contact: -n for a counter clockwise circle
      angleOnJ = na + (c[j].side > 0 ? kPi : 0.f);
   };
   for (size_t i = 0; i < n; i++)
   {
      const size_t prev = (i + n - 1) % n, next = (i + 1) % n;
      float in = 0.f, out2 = 0.f, unused = 0.f;
      tangentAngle(prev, i, unused, in);
      tangentAngle(i, next, out2, unused);
      float sweep = c[i].side > 0 ? Mod2Pi(out2 - in) : Mod2Pi(in - out2);
      if (sweep > kTwoPi - 1e-4f)
         sweep = 0.f;
      const int steps = sweep < 1e-3f ? 0 : std::max(1, static_cast<int>(std::ceil(sweep / maxStep - 1e-4f)));
      for (int k = 0; k <= steps; k++)
      {
         const float a = in + static_cast<float>(c[i].side) * (steps == 0 ? 0.f : sweep * static_cast<float>(k) / static_cast<float>(steps));
         out.push_back({ c[i].x + c[i].r * std::cos(a), c[i].y + c[i].r * std::sin(a) });
         if (steps == 0)
            break;
      }
   }
   return out;
}
}

std::vector<Vec2> Fit(const std::vector<Post> &posts, const float thickness, const float slack, const Wrap wrap)
{
   if (posts.size() < 2 || !std::isfinite(thickness) || !std::isfinite(slack))
      return {};
   std::vector<Circle> c;
   for (const Post &p : posts)
   {
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.r))
         return {};
      const Circle n { p.x, p.y, std::max(p.r + 0.5f * thickness + slack, 0.01f), 1 };
      // Near identical circles (posts stacked on each other) are one: keep the bigger
      const auto same = std::find_if(c.begin(), c.end(), [&n](const Circle &o) { return std::fabs(o.x - n.x) < 0.01f && std::fabs(o.y - n.y) < 0.01f; });
      if (same == c.end())
         c.push_back(n);
      else
         same->r = std::max(same->r, n.r);
   }
   if (c.size() < 2)
      return {};
   // The selection order polygon can not wrap a post inside another one's circle: use the hull
   const auto contains = [](const Circle &a, const Circle &b) { return std::hypot(a.x - b.x, a.y - b.y) + b.r <= a.r; };
   Wrap mode = wrap;
   for (size_t i = 0; i < c.size() && mode == SelectionOrder; i++)
      for (size_t j = 0; j < c.size(); j++)
         if (i != j && contains(c[i], c[j]))
         {
            mode = Hull;
            break;
         }

   if (mode == Hull)
   {
      const std::vector<int> order = HullOrder(c);
      std::vector<Circle> ring;
      for (const int i : order)
         ring.push_back(c[static_cast<size_t>(i)]);
      // A hull circle whose arc comes out negative (it only grazes the hull, sampling put it there) does not belong: drop it and walk again
      for (bool again = true; again && ring.size() > 2;)
      {
         again = false;
         for (size_t i = 0; i < ring.size() && !again; i++)
         {
            const Circle &a = ring[(i + ring.size() - 1) % ring.size()], &b = ring[i], &d = ring[(i + 1) % ring.size()];
            const auto ang = [](const Circle &p, const Circle &q)
            {
               const float dx = q.x - p.x, dy = q.y - p.y;
               return std::atan2(dy, dx) + std::acos(std::clamp((q.r - p.r) / std::max(std::sqrt(dx * dx + dy * dy), 1e-6f), -1.f, 1.f)) + kPi;
            };
            if (Mod2Pi(ang(b, d) - ang(a, b)) > kTwoPi - 1e-3f)
            {
               ring.erase(ring.begin() + static_cast<long>(i));
               again = true;
            }
         }
      }
      c = ring;
   }
   else
   {
      // Counter clockwise polygon, then the side of each post from the turn at its corner
      float area = 0.f;
      for (size_t i = 0; i < c.size(); i++)
      {
         const Circle &a = c[i], &b = c[(i + 1) % c.size()];
         area += a.x * b.y - b.x * a.y;
      }
      if (area < 0.f)
         std::reverse(c.begin(), c.end());
      for (size_t i = 0; i < c.size(); i++)
      {
         const Circle &a = c[(i + c.size() - 1) % c.size()], &b = c[i], &d = c[(i + 1) % c.size()];
         const float turn = (b.x - a.x) * (d.y - b.y) - (b.y - a.y) * (d.x - b.x);
         c[i].side = turn < 0.f ? -1 : 1;
      }
   }
   if (c.empty())
      return {};
   std::vector<Vec2> pts = Walk(c, kPi / 4.f);
   std::reverse(pts.begin(), pts.end()); // Same winding as a rubber made by Add (decreasing angle)
   return pts;
}

}
