// license:GPLv3+

#include "core/stdafx.h"
#include "CircleMath.h"

namespace VPX::EditorUI::CircleMath
{

bool FitCircle(const vector<Vertex2D> &points, const vector<int> &sel, Fit &fit)
{
   if (sel.size() < 3)
      return false;
   Vertex2D mean(0.f, 0.f);
   for (const int i : sel)
      mean += points[i];
   mean /= static_cast<float>(sel.size());
   // Algebraic least squares fit (Kasa): minimizes the sum of (|p - c|^2 - r^2)^2, exact when the points are on a circle (any 3 points not
   // in line). Relative to the mean, the center c solves [Suu Suv; Suv Svv] c = 0.5 [Suuu + Suvv; Svvv + Suuv]
   double suu = 0., suv = 0., svv = 0., bu = 0., bv = 0.;
   for (const int i : sel)
   {
      const double u = points[i].x - mean.x, v = points[i].y - mean.y, w = u * u + v * v;
      suu += u * u;
      suv += u * v;
      svv += v * v;
      bu += 0.5 * u * w;
      bv += 0.5 * v * w;
   }
   const double det = suu * svv - suv * suv;
   Vertex2D center = mean; // Points in line (or coincident): their average position
   if (det > 1e-6 * (suu + svv) * (suu + svv))
   {
      const double cu = (bu * svv - bv * suv) / det, cv = (bv * suu - bu * suv) / det;
      if (fabs(cu) + fabs(cv) < 1e6)
         center = Vertex2D(mean.x + static_cast<float>(cu), mean.y + static_cast<float>(cv));
   }
   float radius = 0.f;
   for (const int i : sel)
      radius += (points[i] - center).Length();
   radius /= static_cast<float>(sel.size());
   if (radius < MIN_RADIUS)
      return false;
   const Vertex2D first = points[sel[0]] - center;
   fit.center = center;
   fit.radius = radius;
   fit.startAngle = first.Length() < MIN_RADIUS ? 0.f : atan2f(first.y, first.x);
   return true;
}

float SignedArea(const vector<Vertex2D> &points)
{
   float area = 0.f;
   for (size_t i = 0, n = points.size(); i < n; i++)
   {
      const Vertex2D &a = points[i], &b = points[(i + 1) % n];
      area += a.x * b.y - b.x * a.y;
   }
   return 0.5f * area;
}

vector<Vertex2D> Circle(const Vertex2D &center, const float radius, const int faces, const float startAngle, const bool positive)
{
   const int n = clamp(faces, MIN_FACES, MAX_FACES);
   const float step = (positive ? 2.f : -2.f) * M_PIf / static_cast<float>(n);
   vector<Vertex2D> result;
   result.reserve(n);
   for (int i = 0; i < n; i++)
   {
      const float a = startAngle + step * static_cast<float>(i);
      result.emplace_back(center.x + radius * cosf(a), center.y + radius * sinf(a));
   }
   return result;
}

}
