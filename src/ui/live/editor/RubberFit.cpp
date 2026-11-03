// license:GPLv3+

// Port of Visual Pinball Engine (freezy/VisualPinball.Engine, VisualPinball.Unity/VPT/Rubber/Guide): RubberCircleHullSolver, RubberPath
// (RubberPathMath), RubberPathSplineBaker. Computed in double, VPE's epsilons and tolerances kept.

#ifndef RUBBERFIT_STANDALONE
#include "core/stdafx.h"
#endif
#include "math/MeshUtils.h"
#include "RubberFit.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace VPX::EditorUI::RubberFit
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2. * kPi;
constexpr double kEps = 1e-4; // VPE solver epsilon
constexpr double kRoundnessEps = 1e-6;
constexpr int kMaxDepth = 12;
constexpr int kMaxSubdivisionsPerElement = 4096;
constexpr int kDeviationSamplesPerSegment = 16;

struct V2
{
   double x = 0., y = 0.;
};
V2 operator+(V2 a, V2 b) { return { a.x + b.x, a.y + b.y }; }
V2 operator-(V2 a, V2 b) { return { a.x - b.x, a.y - b.y }; }
V2 operator*(V2 a, double s) { return { a.x * s, a.y * s }; }
V2 operator*(double s, V2 a) { return a * s; }
V2 operator/(V2 a, double s) { return { a.x / s, a.y / s }; }
double Dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
double Cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
double Len(V2 a) { return std::hypot(a.x, a.y); }
double Dist(V2 a, V2 b) { return Len(a - b); }
bool Finite(V2 a) { return std::isfinite(a.x) && std::isfinite(a.y); }

enum class Type { FreeSpan, SupportedArc };

struct Element
{
   Type type = Type::FreeSpan;
   V2 start, end, c1, c2, center;
   double radius = 0., startAngle = 0., sweep = 0., length = 0.;
   int startBinding = 0;
};

// --- RubberPathMath

V2 PointAt(const Element &e, double t)
{
   t = std::clamp(t, 0., 1.);
   if (e.type == Type::SupportedArc)
   {
      const double a = e.startAngle + e.sweep * t;
      return e.center + V2 { std::cos(a), std::sin(a) } * e.radius;
   }
   const double u = 1. - t;
   return u * u * u * e.start + 3. * u * u * t * e.c1 + 3. * u * t * t * e.c2 + t * t * t * e.end;
}

V2 NormalizeSafe(V2 v, V2 fallback)
{
   const double l = Len(v);
   return l > 1e-12 ? v / l : fallback;
}

V2 TangentAt(const Element &e, double t)
{
   t = std::clamp(t, 0., 1.);
   if (e.type == Type::SupportedArc)
   {
      const double a = e.startAngle + e.sweep * t;
      const V2 d { -std::sin(a), std::cos(a) };
      return e.sweep < 0. ? d * -1. : d;
   }
   const double u = 1. - t;
   const V2 d = 3. * u * u * (e.c1 - e.start) + 6. * u * t * (e.c2 - e.c1) + 3. * t * t * (e.end - e.c2);
   return NormalizeSafe(d, NormalizeSafe(e.end - e.start, { 1., 0. }));
}

bool IsStraight(const Element &e)
{
   if (e.type != Type::FreeSpan)
      return false;
   const V2 d = e.end - e.start;
   return Dist(e.c1, e.start + d / 3.) <= kEps && Dist(e.c2, e.start + d * (2. / 3.)) <= kEps;
}

double DistanceToLine(V2 p, V2 a, V2 b)
{
   const V2 d = b - a;
   const double l = Len(d);
   if (l <= 1e-6)
      return Dist(p, a);
   return std::fabs(d.x * (p.y - a.y) - d.y * (p.x - a.x)) / l;
}

double DistanceToSegment(V2 p, V2 a, V2 b)
{
   const V2 d = b - a;
   const double l2 = Dot(d, d);
   if (l2 <= 1e-12)
      return Dist(p, a);
   const double t = std::clamp(Dot(p - a, d) / l2, 0., 1.);
   return Dist(p, a + d * t);
}

int RequiredDepth(V2 s, V2 c1, V2 c2, V2 e, double tol, int depth)
{
   if (depth >= kMaxDepth || std::max(DistanceToLine(c1, s, e), DistanceToLine(c2, s, e)) <= tol)
      return depth;
   const V2 sc = (s + c1) * 0.5, mc = (c1 + c2) * 0.5, ec = (c2 + e) * 0.5, lm = (sc + mc) * 0.5, rm = (mc + ec) * 0.5, mid = (lm + rm) * 0.5;
   return std::max(RequiredDepth(s, sc, lm, mid, tol, depth + 1), RequiredDepth(mid, rm, ec, e, tol, depth + 1));
}

int FreeSpanSubdivisions(const Element &e, double tol)
{
   if (e.type != Type::FreeSpan || !std::isfinite(tol) || tol <= 0.)
      return 1;
   return 1 << RequiredDepth(e.start, e.c1, e.c2, e.end, tol, 0);
}

double FreeSpanLength(const Element &e)
{
   const int n = FreeSpanSubdivisions(e, 1e-4);
   double len = 0.;
   V2 prev = e.start;
   for (int i = 1; i <= n; i++)
   {
      const V2 p = PointAt(e, static_cast<double>(i) / n);
      len += Dist(prev, p);
      prev = p;
   }
   return len;
}

double DistanceToElement(V2 p, const Element &e)
{
   if (e.type == Type::SupportedArc)
      return std::fabs(Dist(p, e.center) - e.radius);
   const int n = FreeSpanSubdivisions(e, 1e-3);
   double m = INFINITY;
   V2 prev = e.start;
   for (int i = 1; i <= n; i++)
   {
      const V2 next = PointAt(e, static_cast<double>(i) / n);
      m = std::min(m, DistanceToSegment(p, prev, next));
      prev = next;
   }
   return m;
}

// --- RubberCircleHullSolver

struct Circle
{
   V2 c;
   double r;
   int binding;
};

struct Tangent
{
   int startCircle, endCircle;
   V2 start, end, inward;
   double Length() const { return Dist(start, end); }
};

struct ContactArc
{
   int circle;
   double startAngle, sweep;
   V2 start, end;
   double EndAngle() const { return startAngle + sweep; }
};

double PositiveAngle(double a)
{
   a = std::fmod(a, kTwoPi);
   return a < 0. ? a + kTwoPi : a;
}

V2 PointOnCircle(const Circle &c, double a) { return c.c + V2 { std::cos(a), std::sin(a) } * c.r; }
V2 TangentDirection(double a) { return { -std::sin(a), std::cos(a) }; }

std::vector<Circle> RemoveDuplicatesAndContained(const std::vector<Circle> &source)
{
   // source is already ordered by binding index
   std::vector<Circle> dedup;
   for (const Circle &c : source)
      if (std::ranges::none_of(dedup, [&c](const Circle &o) { return Dist(o.c, c.c) <= kEps && std::fabs(o.r - c.r) <= kEps; }))
         dedup.push_back(c);
   std::vector<Circle> retained;
   for (size_t i = 0; i < dedup.size(); i++)
   {
      bool contained = false;
      for (size_t j = 0; j < dedup.size() && !contained; j++)
         contained = i != j && Dist(dedup[i].c, dedup[j].c) + dedup[i].r <= dedup[j].r + kEps;
      if (!contained)
         retained.push_back(dedup[i]);
   }
   return retained;
}

std::vector<Tangent> FindHullTangents(const std::vector<Circle> &circles)
{
   std::vector<Tangent> tangents;
   for (size_t i = 0; i < circles.size(); i++)
      for (size_t j = 0; j < circles.size(); j++)
      {
         if (i == j)
            continue;
         const V2 delta = circles[j].c - circles[i].c;
         const double distance = Len(delta);
         if (distance <= kEps)
            continue;
         double ratio = (circles[j].r - circles[i].r) / distance;
         if (std::fabs(ratio) > 1. + kEps)
            continue;
         ratio = std::clamp(ratio, -1., 1.);
         const V2 dir = delta / distance;
         const V2 inward = dir * ratio + V2 { -dir.y, dir.x } * std::sqrt(std::max(0., 1. - ratio * ratio));
         const V2 start = circles[i].c - inward * circles[i].r;
         const V2 end = circles[j].c - inward * circles[j].r;
         if (Dist(start, end) <= kEps)
            continue;
         bool valid = true;
         for (size_t k = 0; k < circles.size() && valid; k++)
            valid = Dot(inward, circles[k].c - start) >= circles[k].r - kEps;
         if (valid)
            tangents.push_back({ static_cast<int>(i), static_cast<int>(j), start, end, inward });
      }
   return tangents;
}

std::vector<Tangent> RemoveSubsegmentTangents(const std::vector<Tangent> &source)
{
   std::vector<Tangent> retained;
   for (size_t i = 0; i < source.size(); i++)
   {
      const Tangent &t = source[i];
      const V2 dir = NormalizeSafe(t.end - t.start, { 1., 0. });
      const double sd = Dot(dir, t.start), ed = Dot(dir, t.end);
      bool contained = false;
      for (size_t j = 0; j < source.size() && !contained; j++)
      {
         if (i == j)
            continue;
         const Tangent &c = source[j];
         if (Dot(t.inward, c.inward) < 1. - kEps || std::fabs(Dot(t.inward, c.start - t.start)) > kEps)
            continue;
         contained = Dot(dir, c.start) <= sd + kEps && Dot(dir, c.end) >= ed - kEps && c.Length() > t.Length() + kEps;
      }
      if (!contained)
         retained.push_back(t);
   }
   return retained;
}

bool TryOrderCycle(const std::vector<Circle> &circles, const std::vector<Tangent> &tangents, std::vector<Tangent> &cycle, std::string &error)
{
   if (tangents.size() < 2)
   {
      error = "The posts do not produce a closed outer tangent cycle.";
      return false;
   }
   std::map<int, std::vector<Tangent>> outgoing;
   for (const Tangent &t : tangents)
      outgoing[t.startCircle].push_back(t);
   for (const auto &pair : outgoing)
      if (pair.second.size() > 1)
      {
         error = "Post " + std::to_string(circles[static_cast<size_t>(pair.first)].binding) + " has an ambiguous hull tangent.";
         return false;
      }
   const Tangent *first = &tangents[0];
   for (const Tangent &t : tangents)
      if (std::tie(t.start.x, t.start.y, circles[static_cast<size_t>(t.startCircle)].binding, circles[static_cast<size_t>(t.endCircle)].binding)
         < std::tie(first->start.x, first->start.y, circles[static_cast<size_t>(first->startCircle)].binding, circles[static_cast<size_t>(first->endCircle)].binding))
         first = &t;
   Tangent current = *first;
   std::set<std::pair<int, int>> visited;
   while (visited.insert({ current.startCircle, current.endCircle }).second)
   {
      cycle.push_back(current);
      const auto it = outgoing.find(current.endCircle);
      if (it == outgoing.end())
      {
         error = "Post " + std::to_string(circles[static_cast<size_t>(current.endCircle)].binding) + " breaks the tangent cycle.";
         return false;
      }
      current = it->second[0];
      if (current.startCircle == first->startCircle && current.endCircle == first->endCircle)
         break;
      if (cycle.size() > tangents.size())
      {
         error = "The hull tangent traversal did not close.";
         return false;
      }
   }
   if (current.startCircle != first->startCircle || current.endCircle != first->endCircle || cycle.size() < 2)
   {
      error = "The hull tangents form multiple or open cycles.";
      return false;
   }
   return true;
}

std::vector<ContactArc> BuildContactArcs(const std::vector<Circle> &circles, const std::vector<Tangent> &cycle, double roundness)
{
   std::vector<ContactArc> arcs(cycle.size());
   for (size_t i = 0; i < cycle.size(); i++)
   {
      const Tangent &t = cycle[i], &next = cycle[(i + 1) % cycle.size()];
      const Circle &c = circles[static_cast<size_t>(t.endCircle)];
      const double a0 = std::atan2(t.end.y - c.c.y, t.end.x - c.c.x);
      const double a1 = std::atan2(next.start.y - c.c.y, next.start.x - c.c.x);
      const double originalSweep = PositiveAngle(a1 - a0);
      const double middle = a0 + originalSweep * 0.5;
      // Ease the contact anchors toward the midpoint so they stay apart through most of the slider range, then converge at full roundness
      const double sweep = originalSweep * (1. - roundness * roundness);
      const double sa = middle - sweep * 0.5, ea = middle + sweep * 0.5;
      arcs[i] = { t.endCircle, sa, sweep, PointOnCircle(c, sa), PointOnCircle(c, ea) };
   }
   return arcs;
}

double HandleLength(double chord, V2 startTangent, V2 endTangent, double roundness)
{
   if (roundness <= kRoundnessEps)
      return chord / 3.;
   const double turn = std::clamp(std::atan2(Cross(startTangent, endTangent), Dot(startTangent, endTangent)), 0., kPi);
   const double cs = std::cos(turn * 0.25);
   return chord / (3. * cs * cs);
}

std::vector<Element> EmitElements(const std::vector<Circle> &circles, const std::vector<Tangent> &cycle, double roundness)
{
   const std::vector<ContactArc> arcs = BuildContactArcs(circles, cycle, roundness);
   std::vector<Element> elements;
   for (size_t i = 0; i < cycle.size(); i++)
   {
      const Tangent &t = cycle[i];
      const Circle &sc = circles[static_cast<size_t>(t.startCircle)], &ec = circles[static_cast<size_t>(t.endCircle)];
      const ContactArc &prev = arcs[(i + cycle.size() - 1) % cycle.size()], &next = arcs[i];
      if (prev.circle != t.startCircle || next.circle != t.endCircle)
         return {};
      const V2 st = TangentDirection(prev.EndAngle()), et = TangentDirection(next.startAngle);
      const V2 chord = next.start - prev.end;
      const double chordLength = Len(chord);
      if (chordLength > kEps)
      {
         const double h = HandleLength(chordLength, st, et, roundness);
         Element e;
         e.type = Type::FreeSpan;
         e.start = prev.end;
         e.end = next.start;
         e.c1 = roundness <= kRoundnessEps ? prev.end + chord / 3. : prev.end + st * h;
         e.c2 = roundness <= kRoundnessEps ? prev.end + chord * (2. / 3.) : next.start - et * h;
         e.startBinding = sc.binding;
         e.length = FreeSpanLength(e);
         elements.push_back(e);
      }
      if (next.sweep * ec.r <= kEps)
         continue;
      Element a;
      a.type = Type::SupportedArc;
      a.start = next.start;
      a.end = next.end;
      a.center = ec.c;
      a.radius = ec.r;
      a.startAngle = next.startAngle;
      a.sweep = next.sweep;
      a.startBinding = ec.binding;
      a.length = next.sweep * ec.r;
      elements.push_back(a);
   }
   return elements;
}

// Rotates the loop to start at the lexicographically smallest start point (deterministic first drag point)
void Normalize(std::vector<Element> &elements)
{
   const auto less = [](const Element &a, const Element &b)
   {
      if (a.start.x < b.start.x - kEps)
         return true;
      if (a.start.x > b.start.x + kEps)
         return false;
      if (a.start.y < b.start.y - kEps)
         return true;
      if (a.start.y > b.start.y + kEps)
         return false;
      return a.startBinding != b.startBinding ? a.startBinding < b.startBinding : a.type < b.type;
   };
   size_t first = 0;
   for (size_t i = 1; i < elements.size(); i++)
      if (less(elements[i], elements[first]))
         first = i;
   std::rotate(elements.begin(), elements.begin() + static_cast<long>(first), elements.end());
}

bool SegmentsIntersect(V2 a, V2 b, V2 c, V2 d)
{
   const V2 ab = b - a, cd = d - c;
   const double den = Cross(ab, cd);
   if (std::fabs(den) <= kEps)
      return false;
   const V2 ac = c - a;
   const double t = Cross(ac, cd) / den, u = Cross(ac, ab) / den;
   return t > kEps && t < 1. - kEps && u > kEps && u < 1. - kEps;
}

bool TryValidate(const std::vector<Element> &elements, std::string &error)
{
   if (elements.empty())
   {
      error = "The rubber path is empty.";
      return false;
   }
   for (size_t i = 0; i < elements.size(); i++)
   {
      const Element &e = elements[i];
      const std::string name = "Path element " + std::to_string(i);
      if (!Finite(e.start) || !Finite(e.end) || !std::isfinite(e.length) || e.length <= kEps)
      {
         error = name + " has invalid geometry or length.";
         return false;
      }
      if (e.type == Type::FreeSpan)
      {
         if (!Finite(e.c1) || !Finite(e.c2))
         {
            error = name + " has invalid control points.";
            return false;
         }
      }
      else if (!Finite(e.center) || !std::isfinite(e.radius) || e.radius <= 0. || !std::isfinite(e.sweep) || e.sweep <= 0.
         || std::fabs(Dist(e.start, e.center) - e.radius) > kEps || std::fabs(Dist(e.end, e.center) - e.radius) > kEps)
      {
         error = name + " has inconsistent circle geometry.";
         return false;
      }
      if (Dist(e.end, elements[(i + 1) % elements.size()].start) > kEps)
      {
         error = name + " is not connected to the next element.";
         return false;
      }
   }
   std::vector<V2> pts;
   for (const Element &e : elements)
   {
      pts.push_back(e.start);
      const int samples = e.type == Type::SupportedArc ? std::max(2, static_cast<int>(std::ceil(e.sweep / (kPi / 32.)))) : std::max(1, FreeSpanSubdivisions(e, std::max(0.05, kEps)));
      for (int i = 1; i < samples; i++)
         pts.push_back(PointAt(e, static_cast<double>(i) / samples));
   }
   for (size_t i = 0; i < pts.size(); i++)
   {
      const size_t in = (i + 1) % pts.size();
      for (size_t j = i + 2; j < pts.size(); j++)
      {
         const size_t jn = (j + 1) % pts.size();
         if (i == jn || in == j)
            continue;
         if (SegmentsIntersect(pts[i], pts[in], pts[j], pts[jn]))
         {
            error = "The rubber path intersects itself.";
            return false;
         }
      }
   }
   return true;
}

bool TryValidateClearance(const std::vector<Element> &elements, const std::vector<Circle> &circles, std::string &error)
{
   const double tol = std::max(0.005, kEps * 4.);
   for (const Element &e : elements)
   {
      const int samples = e.type == Type::SupportedArc ? std::max(2, static_cast<int>(std::ceil(e.sweep / (kPi / 64.)))) : std::max(2, FreeSpanSubdivisions(e, tol));
      V2 prev = e.start;
      for (int s = 0; s <= samples; s++)
      {
         const V2 p = PointAt(e, static_cast<double>(s) / samples);
         for (const Circle &c : circles)
            if (Dist(p, c.c) < c.r - tol || (e.type == Type::FreeSpan && s > 0 && DistanceToSegment(c.c, prev, p) < c.r - 2. * tol))
            {
               error = "The rounded path intersects post " + std::to_string(c.binding) + ".";
               return false;
            }
         prev = p;
      }
   }
   return true;
}

bool Solve(const std::vector<Post> &posts, double cordRadius, double roundness, std::vector<Element> &elements, std::string &error)
{
   if (posts.empty())
   {
      error = "At least one post is required.";
      return false;
   }
   if (!std::isfinite(cordRadius) || cordRadius < 0.)
   {
      error = "Thickness must be finite and non-negative.";
      return false;
   }
   if (!std::isfinite(roundness) || roundness < 0. || roundness > 1.)
   {
      error = "Roundness must be between zero and one.";
      return false;
   }
   std::vector<Circle> circles;
   for (size_t i = 0; i < posts.size(); i++)
   {
      const Post &p = posts[i];
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.r) || p.r <= 0.f)
      {
         error = "Post " + std::to_string(i) + " has invalid circle geometry.";
         return false;
      }
      circles.push_back({ { p.x, p.y }, p.r + cordRadius, static_cast<int>(i) });
   }
   circles = RemoveDuplicatesAndContained(circles);
   if (circles.size() == 1)
   {
      // No free spans, roundness has nothing to relax
      Element e;
      e.type = Type::SupportedArc;
      e.start = e.end = circles[0].c + V2 { -circles[0].r, 0. };
      e.center = circles[0].c;
      e.radius = circles[0].r;
      e.startAngle = kPi;
      e.sweep = kTwoPi;
      e.startBinding = circles[0].binding;
      e.length = kTwoPi * circles[0].r;
      elements = { e };
      return true;
   }
   std::vector<Tangent> cycle;
   if (!TryOrderCycle(circles, RemoveSubsegmentTangents(FindHullTangents(circles)), cycle, error))
      return false;
   elements = EmitElements(circles, cycle, roundness);
   if (elements.empty())
   {
      error = "The post hull produced no path elements.";
      return false;
   }
   Normalize(elements);
   return TryValidate(elements, error) && TryValidateClearance(elements, circles, error);
}

// --- RubberPathSplineBaker

struct Node
{
   V2 pos;
   bool smooth;
   int outgoing;
};

bool IsFullCircle(const std::vector<Element> &el)
{
   double sweep = 0.;
   for (const Element &e : el)
   {
      if (e.type != Type::SupportedArc || Dist(e.center, el[0].center) > kEps || std::fabs(e.radius - el[0].radius) > kEps)
         return false;
      sweep += e.sweep;
   }
   return sweep >= kTwoPi - kEps;
}

bool IsSmoothJunction(const Element &a, const Element &b)
{
   if (IsStraight(a) || IsStraight(b))
      return false;
   return Dot(TangentAt(a, 1.), TangentAt(b, 0.)) >= 1. - kEps;
}

std::vector<Node> CreateNodes(const std::vector<Element> &el, const std::vector<int> &subdivisions)
{
   std::vector<Node> nodes;
   const bool full = IsFullCircle(el);
   for (size_t ei = 0; ei < el.size(); ei++)
   {
      const Element &e = el[ei];
      if (nodes.empty())
         nodes.push_back({ e.start, full, static_cast<int>(ei) });
      else
      {
         nodes.back().outgoing = static_cast<int>(ei);
         if (!full)
            nodes.back().smooth = IsSmoothJunction(el[ei - 1], e);
      }
      const int count = subdivisions[ei];
      for (int s = 1; s <= count; s++)
      {
         const bool last = s == count;
         const V2 p = PointAt(e, static_cast<double>(s) / count);
         if (ei == el.size() - 1 && last && Dist(p, nodes[0].pos) <= kEps)
            continue;
         nodes.push_back({ p, full || (!last && (e.type == Type::SupportedArc || !IsStraight(e))), last && ei + 1 < el.size() ? static_cast<int>(ei) + 1 : static_cast<int>(ei) });
      }
   }
   if (!full && !nodes.empty())
      nodes[0].smooth = IsSmoothJunction(el.back(), el[0]);
   return nodes;
}

// Largest distance of VPX's Catmull-Rom through the nodes (same neighbour rule as DragPoint::GetRgVertex) from the exact element each segment follows
double MeasureDeviation(const std::vector<Element> &el, const std::vector<Node> &nodes)
{
   const size_t n = nodes.size();
   const auto vtx = [&nodes](size_t i) { return Vertex2D(static_cast<float>(nodes[i].pos.x), static_cast<float>(nodes[i].pos.y)); };
   double maxDev = 0.;
   for (size_t i = 0; i < n; i++)
   {
      const size_t next = (i + 1) % n;
      const size_t prev = nodes[i].smooth ? (i + n - 1) % n : i;
      const size_t following = nodes[next].smooth ? (i + 2) % n : next;
      CatmullCurve<2> cc;
      cc.SetCurve(vtx(prev), vtx(i), vtx(next), vtx(following));
      for (int s = 0; s <= kDeviationSamplesPerSegment; s++)
      {
         Vertex2D v;
         cc.GetPointAt(static_cast<float>(s) / kDeviationSamplesPerSegment, &v);
         maxDev = std::max(maxDev, DistanceToElement({ v.x, v.y }, el[static_cast<size_t>(nodes[i].outgoing)]));
      }
   }
   return maxDev;
}

bool Bake(const std::vector<Element> &el, double tol, std::vector<Point> &out, std::string &error)
{
   std::vector<int> sub(el.size());
   for (size_t i = 0; i < el.size(); i++)
   {
      if (el[i].type == Type::FreeSpan)
      {
         sub[i] = FreeSpanSubdivisions(el[i], tol);
         continue;
      }
      double maxAngle = 2. * std::acos(std::clamp(1. - tol / el[i].radius, -1., 1.));
      if (!std::isfinite(maxAngle) || maxAngle <= 1e-5)
         maxAngle = kPi / 16.;
      sub[i] = std::max(3, static_cast<int>(std::ceil(el[i].sweep / maxAngle)));
   }
   for (;;)
   {
      const std::vector<Node> nodes = CreateNodes(el, sub);
      if (nodes.size() < 3)
      {
         error = "The sampled rubber path contains fewer than three distinct points.";
         return false;
      }
      if (MeasureDeviation(el, nodes) <= tol)
      {
         out.clear();
         for (const Node &n : nodes)
            out.push_back({ static_cast<float>(n.pos.x), static_cast<float>(n.pos.y), n.smooth });
         return true;
      }
      bool refined = false;
      for (size_t i = 0; i < el.size(); i++)
      {
         if (IsStraight(el[i]))
            continue;
         if (sub[i] >= kMaxSubdivisionsPerElement)
         {
            error = "The sampled spline exceeds its render tolerance.";
            return false;
         }
         sub[i] = std::min(kMaxSubdivisionsPerElement, sub[i] * 2);
         refined = true;
      }
      if (!refined)
      {
         error = "The sampled spline exceeds its render tolerance.";
         return false;
      }
   }
}
}

bool Fit(const std::vector<Post> &posts, const float thickness, const float roundness, std::vector<Point> &points, std::string &error)
{
   std::vector<Element> elements;
   if (!Solve(posts, 0.5 * static_cast<double>(thickness), roundness, elements, error))
      return false;
   return Bake(elements, kRenderTolerance, points, error);
}

}
