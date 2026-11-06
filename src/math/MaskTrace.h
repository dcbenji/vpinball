// license:GPLv3+

// Playfield Maker "Find Holes": traces a wood mask image (black shapes on white, white on black, or transparent) into hole outlines.
// No engine dependency (standard library only), so it can be checked standalone: clang++ -std=c++20 -I src tools/masktrace_check.cpp
// Pipeline (SPEC-playfield-maker.md, "Holes", Find Holes steps 2-6):
//   hole pixels (mask mode) -> 4-connected components -> drop those under Min Size -> components touching the image edge are outline cuts (listed, not traced)
//   -> outer contour by marching squares at the 50% level (crack following, foreground kept 4-connected at saddles; crossings interpolated between pixel centres)
//   -> circle test (least-squares circle) and slot test (capsule) on the unsimplified contour, else Ramer-Douglas-Peucker polygon.
// All output coordinates are table units (VPU). The mask pixel (px, py) maps to (left + (px + 0.5) / W * (right - left), top + (py + 0.5) / H * (bottom - top)).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MaskTrace
{

// Matches Primitive::m_pfMaskMode (saved as PFMM).
enum class Mode { Auto = 0, Dark = 1, Light = 2, Transparent = 3 };

// Matches the saved hole type (PFHT).
enum class HoleType { Round = 0, Slot = 1, Polygon = 2 };

// 8-bit pixels, row 0 first (the top of the table). channels: 1 = BW, 3 = RGB/SRGB, 4 = RGBA/SRGBA (BaseTexture formats BW, RGB, SRGB, RGBA, SRGBA; the colour
// order does not matter, brightness is the channel average). Alpha is the 4th channel. Other BaseTexture formats: Convert(RGBA) first.
struct Image
{
   const uint8_t* data = nullptr;
   int w = 0, h = 0;
   int channels = 4;
   size_t stride = 0; // bytes per row, 0 = w * channels (BaseTexture::pitch())
};

struct Params
{
   float left = 0.f, top = 0.f, right = 1.f, bottom = 1.f; // the table rectangle the mask covers
   Mode mode = Mode::Auto;
   float minSize = 2.35f; // VPU: a component whose larger bounding box side is under this is dropped (0.05 in)
   float vpuPerInch = 47.f; // 0.5 in = 23.5 VPU
};

struct Pt
{
   float x, y;
};

// Same field meaning as PlasticMesh::Hole for Round and Slot, so a hole converts field by field.
//   Round:   (x, y) centre, diameter.
//   Slot:    capsule of `diameter` between the end centres (x, y) and (x2, y2).
//   Polygon: `polygon` = the simplified outer contour, clockwise (shoelace area < 0, no repeated first point), (x, y) = its area centroid, diameter 0.
struct Hole
{
   HoleType type = HoleType::Polygon;
   float x = 0.f, y = 0.f, diameter = 0.f;
   float x2 = 0.f, y2 = 0.f;
   std::vector<Pt> polygon;
   float area = 0.f; // VPU^2, of the traced contour
   float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f; // bounds of the traced contour
};

struct OutlineCut
{
   float minX, minY, maxX, maxY; // bounds of the component touching the image edge
};

struct Result
{
   std::vector<Hole> holes; // in raster order of each component's first pixel (top to bottom, left to right)
   std::vector<OutlineCut> outlineCuts; // listed and skipped in phase 1
   Mode usedMode = Mode::Dark; // what Auto resolved to (never Auto)
   int components = 0; // all 4-connected components, before any filtering
   int droppedSmall = 0; // under Min Size
   int rdpRetried = 0; // simplified contours that self-intersected and were redone at half the tolerance
   int rdpUnsimplified = 0; // ... and still did, so the raw contour was kept
   bool noAlphaFallback = false; // Transparent was asked for but the image has no alpha: Dark was used
};

namespace detail
{

struct D2
{
   double x, y;
};

inline double SignedArea(const std::vector<D2>& p)
{
   double a = 0.;
   for (size_t i = 0, n = p.size(); i < n; i++)
      a += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
   return 0.5 * a;
}

inline double SegDist(D2 p, D2 a, D2 b) // distance from p to the segment ab
{
   const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
   double t = l2 > 0. ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.;
   t = t < 0. ? 0. : t > 1. ? 1. : t;
   return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

inline double Orient(D2 a, D2 b, D2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

inline bool OnSeg(D2 a, D2 b, D2 p) // p collinear with ab: inside its box?
{
   return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= p.y && p.y <= std::max(a.y, b.y);
}

inline bool SegsTouch(D2 a, D2 b, D2 c, D2 d) // closed segments ab and cd share a point
{
   const double o1 = Orient(a, b, c), o2 = Orient(a, b, d), o3 = Orient(c, d, a), o4 = Orient(c, d, b);
   if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) && ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0)))
      return true;
   return (o1 == 0 && OnSeg(a, b, c)) || (o2 == 0 && OnSeg(a, b, d)) || (o3 == 0 && OnSeg(c, d, a)) || (o4 == 0 && OnSeg(c, d, b));
}

// True if two non-adjacent edges of the closed polygon touch or cross. (Adjacent edges share a vertex by construction.)
inline bool SelfIntersects(const std::vector<D2>& p)
{
   const size_t n = p.size();
   for (size_t i = 0; i < n; i++)
   {
      const D2 a = p[i], b = p[(i + 1) % n];
      for (size_t j = i + 2; j < n; j++)
      {
         if (i == 0 && j == n - 1)
            continue;
         const D2 c = p[j], d = p[(j + 1) % n];
         if (std::max(a.x, b.x) < std::min(c.x, d.x) || std::max(c.x, d.x) < std::min(a.x, b.x) || std::max(a.y, b.y) < std::min(c.y, d.y)
            || std::max(c.y, d.y) < std::min(a.y, b.y))
            continue;
         if (SegsTouch(a, b, c, d))
            return true;
      }
   }
   return false;
}

// Ramer-Douglas-Peucker on a closed polygon: split at point 0 and the point farthest from it, simplify both chains. Deterministic.
inline std::vector<D2> Rdp(const std::vector<D2>& p, double tol)
{
   const size_t n = p.size();
   if (n < 4)
      return p;
   size_t far = 0;
   double fd = -1.;
   for (size_t i = 1; i < n; i++)
   {
      const double d = std::hypot(p[i].x - p[0].x, p[i].y - p[0].y);
      if (d > fd)
         fd = d, far = i;
   }
   std::vector<char> keep(n, 0);
   keep[0] = keep[far] = 1;
   std::vector<std::pair<size_t, size_t>> st { { 0, far }, { far, n } }; // chain [a, b] with index b taken modulo n
   while (!st.empty())
   {
      const auto [a, b] = st.back();
      st.pop_back();
      double md = -1.;
      size_t mi = 0;
      for (size_t i = a + 1; i < b; i++)
      {
         const double d = SegDist(p[i], p[a], p[b % n]);
         if (d > md)
            md = d, mi = i;
      }
      if (md > tol)
      {
         keep[mi] = 1;
         st.push_back({ a, mi });
         st.push_back({ mi, b });
      }
   }
   std::vector<D2> out;
   for (size_t i = 0; i < n; i++)
      if (keep[i])
         out.push_back(p[i]);
   return out;
}

// Least-squares (Kasa) circle. False if the points are (nearly) collinear.
inline bool FitCircle(const std::vector<D2>& p, double& cx, double& cy, double& r)
{
   const size_t n = p.size();
   double mx = 0., my = 0.;
   for (const D2& q : p)
      mx += q.x, my += q.y;
   mx /= (double)n, my /= (double)n;
   double sxx = 0., sxy = 0., syy = 0., sxz = 0., syz = 0., sz = 0.;
   for (const D2& q : p)
   {
      const double x = q.x - mx, y = q.y - my, z = x * x + y * y;
      sxx += x * x, sxy += x * y, syy += y * y, sxz += x * z, syz += y * z, sz += z;
   }
   const double det = sxx * syy - sxy * sxy;
   if (!(det > 1e-12 * (sxx + syy) * (sxx + syy)))
      return false;
   const double a = (sxz * syy - syz * sxy) / det * 0.5, b = (syz * sxx - sxz * sxy) / det * 0.5; // centre offset
   const double r2 = a * a + b * b + sz / (double)n;
   if (!(r2 > 0.))
      return false;
   cx = mx + a, cy = my + b, r = std::sqrt(r2);
   return true;
}

// Capsule through the points: axis = principal axis of the contour points, width and ends from their extent, verified against every point.
inline bool FitSlot(const std::vector<D2>& p, double tolPx, D2& a, D2& b, double& r)
{
   const size_t n = p.size();
   double mx = 0., my = 0.;
   for (const D2& q : p)
      mx += q.x, my += q.y;
   mx /= (double)n, my /= (double)n;
   double sxx = 0., sxy = 0., syy = 0.;
   for (const D2& q : p)
      sxx += (q.x - mx) * (q.x - mx), sxy += (q.x - mx) * (q.y - my), syy += (q.y - my) * (q.y - my);
   const double ang = 0.5 * std::atan2(2. * sxy, sxx - syy);
   const double ax = std::cos(ang), ay = std::sin(ang); // unit axis, perpendicular (-ay, ax)
   double t0 = 1e300, t1 = -1e300, u0 = 1e300, u1 = -1e300;
   for (const D2& q : p)
   {
      const double t = (q.x - mx) * ax + (q.y - my) * ay, u = -(q.x - mx) * ay + (q.y - my) * ax;
      t0 = std::min(t0, t), t1 = std::max(t1, t), u0 = std::min(u0, u), u1 = std::max(u1, u);
   }
   r = 0.5 * (u1 - u0);
   if (!(r > 0.) || t1 - t0 < 2. * r)
      return false;
   const double uc = 0.5 * (u0 + u1);
   a = { mx + (t0 + r) * ax - uc * ay, my + (t0 + r) * ay + uc * ax };
   b = { mx + (t1 - r) * ax - uc * ay, my + (t1 - r) * ay + uc * ax };
   const double tol = std::max(0.02 * r, tolPx);
   for (const D2& q : p)
      if (std::fabs(SegDist(q, a, b) - r) > tol)
         return false;
   return true;
}

} // namespace detail

// Find Holes, steps 2-5. False (result cleared) for an unusable image or rectangle.
inline bool Trace(const Image& im, const Params& prm, Result& out)
{
   using namespace detail;
   out = Result();
   if (!im.data || im.w < 1 || im.h < 1 || (im.channels != 1 && im.channels != 3 && im.channels != 4) || !(prm.right > prm.left) || !(prm.bottom > prm.top))
      return false;
   const int W = im.w, H = im.h, C = im.channels;
   const size_t stride = im.stride ? im.stride : (size_t)W * C;

   // 2. Hole pixels. Auto: the alpha has both values -> transparent is the hole; else the minority of dark vs light at 50% grey (a tie: dark).
   const auto lum = [&](const uint8_t* q) { return C == 1 ? (int)q[0] : ((int)q[0] + q[1] + q[2]) / 3; };
   Mode mode = prm.mode;
   if (mode == Mode::Auto)
   {
      size_t dark = 0, light = 0, clear = 0, opaque = 0;
      for (int y = 0; y < H; y++)
      {
         const uint8_t* row = im.data + (size_t)y * stride;
         for (int x = 0; x < W; x++)
         {
            const uint8_t* q = row + (size_t)x * C;
            (lum(q) < 128 ? dark : light)++;
            if (C == 4)
               (q[3] < 128 ? clear : opaque)++;
         }
      }
      mode = (clear && opaque) ? Mode::Transparent : (dark <= light ? Mode::Dark : Mode::Light);
   }
   if (mode == Mode::Transparent && C != 4)
      mode = Mode::Dark, out.noAlphaFallback = true; // no alpha channel: nothing is transparent, fall back to the usual black shapes
   out.usedMode = mode;
   std::vector<uint8_t> cov((size_t)W * H); // 0..255 "how much hole"; >= 128 is a hole pixel
   for (int y = 0; y < H; y++)
   {
      const uint8_t* row = im.data + (size_t)y * stride;
      uint8_t* c = &cov[(size_t)y * W];
      for (int x = 0; x < W; x++)
      {
         const uint8_t* q = row + (size_t)x * C;
         c[x] = mode == Mode::Transparent ? (uint8_t)(255 - q[3]) : mode == Mode::Light ? (uint8_t)lum(q) : (uint8_t)(255 - lum(q));
      }
   }
   const auto fg = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && cov[(size_t)y * W + x] >= 128; };
   const auto val = [&](int x, int y) { return (x >= 0 && y >= 0 && x < W && y < H) ? (int)cov[(size_t)y * W + x] : 0; };

   const double sx = ((double)prm.right - prm.left) / W, sy = ((double)prm.bottom - prm.top) / H, pxSize = std::max(sx, sy);
   const double tolRdp = std::max(pxSize, 0.005 * prm.vpuPerInch);
   const double tolCircle = 1.5 * pxSize;

   // 3. 4-connected components in raster order.
   std::vector<uint8_t> seen((size_t)W * H, 0);
   std::vector<uint32_t> stack;
   for (int y0 = 0; y0 < H; y0++)
      for (int x0 = 0; x0 < W; x0++)
      {
         const size_t i0 = (size_t)y0 * W + x0;
         if (seen[i0] || cov[i0] < 128)
            continue;
         int minx = x0, maxx = x0, miny = y0, maxy = y0;
         seen[i0] = 1;
         stack.assign(1, (uint32_t)i0);
         while (!stack.empty())
         {
            const uint32_t i = stack.back();
            stack.pop_back();
            const int x = (int)(i % (uint32_t)W), y = (int)(i / (uint32_t)W);
            minx = std::min(minx, x), maxx = std::max(maxx, x), miny = std::min(miny, y), maxy = std::max(maxy, y);
            const int nx[4] = { x + 1, x - 1, x, x }, ny[4] = { y, y, y + 1, y - 1 };
            for (int k = 0; k < 4; k++)
               if (fg(nx[k], ny[k]) && !seen[(size_t)ny[k] * W + nx[k]])
               {
                  seen[(size_t)ny[k] * W + nx[k]] = 1;
                  stack.push_back((uint32_t)((size_t)ny[k] * W + nx[k]));
               }
         }
         out.components++;
         const double bw = (maxx - minx + 1) * sx, bh = (maxy - miny + 1) * sy;
         if (std::max(bw, bh) < prm.minSize)
         {
            out.droppedSmall++;
            continue;
         }
         const auto tx = [&](double px) { return prm.left + px / W * ((double)prm.right - prm.left); };
         const auto ty = [&](double py) { return prm.top + py / H * ((double)prm.bottom - prm.top); };
         if (minx == 0 || miny == 0 || maxx == W - 1 || maxy == H - 1)
         {
            out.outlineCuts.push_back({ (float)tx(minx), (float)ty(miny), (float)tx(maxx + 1), (float)ty(maxy + 1) });
            continue;
         }

         // 4. Outer contour by crack following with the foreground on the left. Directions E, S, W, N (clockwise on screen, y down).
         static const int DX[4] = { 1, 0, -1, 0 }, DY[4] = { 0, 1, 0, -1 };
         const auto sides = [&](int vx, int vy, int d, int& lx, int& ly, int& rx, int& ry) { // the pixels left and right of the edge leaving vertex (vx, vy)
            switch (d)
            {
            case 0: lx = vx, ly = vy - 1, rx = vx, ry = vy; break;
            case 1: lx = vx, ly = vy, rx = vx - 1, ry = vy; break;
            case 2: lx = vx - 1, ly = vy, rx = vx - 1, ry = vy - 1; break;
            default: lx = vx - 1, ly = vy - 1, rx = vx, ry = vy - 1; break;
            }
         };
         std::vector<D2> raw; // table units
         int vx = x0 + 1, vy = y0, d = 2;
         const int svx = vx, svy = vy;
         for (size_t guard = 0, maxSteps = 4 * (size_t)(maxx - minx + 3) * (size_t)(maxy - miny + 3); guard < maxSteps; guard++)
         {
            int lx, ly, rx, ry;
            sides(vx, vy, d, lx, ly, rx, ry);
            const double vl = val(lx, ly), vr = val(rx, ry), t = (vl - 127.5) / (vl - vr); // vl >= 128 > vr, so vl - vr >= 1
            const double px = lx + 0.5 + t * (rx - lx), py = ly + 0.5 + t * (ry - ly);
            const D2 q { tx(px), ty(py) };
            if (raw.empty() || std::hypot(q.x - raw.back().x, q.y - raw.back().y) > 1e-6)
               raw.push_back(q);
            vx += DX[d], vy += DY[d];
            int alx, aly, arx, ary;
            sides(vx, vy, d, alx, aly, arx, ary); // the pixels ahead
            const bool al = fg(alx, aly), ar = fg(arx, ary);
            if (!al)
               d = (d + 3) & 3; // left (also the saddle: diagonal foreground pixels stay separate)
            else if (ar)
               d = (d + 1) & 3;
            if (vx == svx && vy == svy && d == 2)
               break;
         }
         if (raw.size() > 1 && std::hypot(raw.front().x - raw.back().x, raw.front().y - raw.back().y) <= 1e-6)
            raw.pop_back();
         if (raw.size() < 3)
            continue;

         Hole h;
         double cx, cy, r;
         D2 sa, sb;
         double sr;
         if (raw.size() >= 8 && FitCircle(raw, cx, cy, r) && [&] {
                const double tol = std::max(0.02 * r, tolCircle);
                for (const D2& q : raw)
                   if (std::fabs(std::hypot(q.x - cx, q.y - cy) - r) > tol)
                      return false;
                const double a = std::fabs(SignedArea(raw)); // a small square or L must stay a polygon: the circle's area has to match the shape's
                double x0 = raw[0].x, x1 = x0, y0 = raw[0].y, y1 = y0; // a disc fills 78% of its bounding box, a square (its octagonal contour included) 97%
                for (const D2& q : raw)
                   x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
                return a > 0. && std::fabs(3.14159265358979323846 * r * r - a) < 0.1 * a && a < 0.9 * (x1 - x0) * (y1 - y0);
             }())
         {
            h.type = HoleType::Round;
            h.x = (float)cx, h.y = (float)cy, h.diameter = (float)(2. * r);
         }
         else if (raw.size() >= 8 && FitSlot(raw, tolCircle, sa, sb, sr)
            && [&] {
                  const double a = std::fabs(SignedArea(raw)), cap = 3.14159265358979323846 * sr * sr + 2. * sr * std::hypot(sb.x - sa.x, sb.y - sa.y);
                  const double len = std::hypot(sb.x - sa.x, sb.y - sa.y); // not a zero-length capsule (a small square on a diagonal axis)
                  return a > 0. && len >= sr && std::fabs(cap - a) < 0.1 * a;
               }())
         {
            h.type = HoleType::Slot;
            h.x = (float)sa.x, h.y = (float)sa.y, h.x2 = (float)sb.x, h.y2 = (float)sb.y, h.diameter = (float)(2. * sr);
         }
         else
         {
            std::vector<D2> poly = Rdp(raw, tolRdp);
            if (poly.size() < 3 || SelfIntersects(poly))
            {
               out.rdpRetried++;
               poly = Rdp(raw, tolRdp * 0.5);
               if (poly.size() < 3 || SelfIntersects(poly))
               {
                  out.rdpUnsimplified++;
                  poly = raw;
               }
            }
            if (SignedArea(poly) > 0.)
               std::reverse(poly.begin(), poly.end());
            h.type = HoleType::Polygon;
            double a2 = 0., gx = 0., gy = 0.;
            for (size_t i = 0, n = poly.size(); i < n; i++)
            {
               const D2 p = poly[i], q = poly[(i + 1) % n];
               const double c = p.x * q.y - q.x * p.y;
               a2 += c, gx += (p.x + q.x) * c, gy += (p.y + q.y) * c;
            }
            if (std::fabs(a2) > 1e-12)
               h.x = (float)(gx / (3. * a2)), h.y = (float)(gy / (3. * a2));
            else
               h.x = (float)poly[0].x, h.y = (float)poly[0].y;
            h.polygon.reserve(poly.size());
            for (const D2& p : poly)
               h.polygon.push_back({ (float)p.x, (float)p.y });
         }
         h.area = (float)std::fabs(SignedArea(raw));
         h.minX = h.minY = 1e30f, h.maxX = h.maxY = -1e30f;
         for (const D2& p : raw)
            h.minX = std::min(h.minX, (float)p.x), h.maxX = std::max(h.maxX, (float)p.x), h.minY = std::min(h.minY, (float)p.y), h.maxY = std::max(h.maxY, (float)p.y);
         out.holes.push_back(std::move(h));
      }
   return true;
}

// ---- Find Holes step 6 support: Under Insert. The caller builds each traced insert card's footprint (the convex hull of its mesh vertices through its full matrix). ----

// Convex hull (Andrew monotone chain), counter-clockwise (shoelace area > 0). Fewer than 3 distinct points: returned as they are.
inline std::vector<Pt> ConvexHull(std::vector<Pt> p)
{
   std::sort(p.begin(), p.end(), [](Pt a, Pt b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
   p.erase(std::unique(p.begin(), p.end(), [](Pt a, Pt b) { return a.x == b.x && a.y == b.y; }), p.end());
   if (p.size() < 3)
      return p;
   const auto cr = [](Pt o, Pt a, Pt b) { return ((double)a.x - o.x) * ((double)b.y - o.y) - ((double)a.y - o.y) * ((double)b.x - o.x); };
   std::vector<Pt> h(2 * p.size());
   size_t k = 0;
   for (size_t i = 0; i < p.size(); i++)
   {
      while (k >= 2 && cr(h[k - 2], h[k - 1], p[i]) <= 0.)
         k--;
      h[k++] = p[i];
   }
   for (size_t i = p.size() - 1, t = k + 1; i > 0; i--)
   {
      while (k >= t && cr(h[k - 2], h[k - 1], p[i - 1]) <= 0.)
         k--;
      h[k++] = p[i - 1];
   }
   h.resize(k - 1);
   return h;
}

// The hole as a polygon (a circle or capsule flattened to `segments` points around).
inline std::vector<Pt> Outline(const Hole& h, int segments = 48)
{
   if (h.type == HoleType::Polygon)
      return h.polygon;
   std::vector<Pt> o;
   const double r = 0.5 * h.diameter, pi = 3.14159265358979323846;
   const double ax = h.type == HoleType::Slot ? h.x2 - h.x : 0., ay = h.type == HoleType::Slot ? h.y2 - h.y : 0., al = std::hypot(ax, ay);
   const double base = al > 0. ? std::atan2(ay, ax) : 0.;
   const int half = std::max(segments / 2, 4);
   for (int s = 0; s < 2; s++) // end cap at B (angles base - 90..+90 degrees), then at A
   {
      const double cx = s == 0 ? (h.type == HoleType::Slot ? h.x2 : h.x) : h.x, cy = s == 0 ? (h.type == HoleType::Slot ? h.y2 : h.y) : h.y;
      for (int i = 0; i < half; i++)
      {
         const double a = base + (s == 0 ? -0.5 : 0.5) * pi + pi * i / half;
         o.push_back({ (float)(cx + r * std::cos(a)), (float)(cy + r * std::sin(a)) });
      }
   }
   return o;
}

// A hole is under an insert when its centre is inside the footprint or more than half of its area is. `hull` is counter-clockwise (ConvexHull).
inline bool UnderInsert(const Hole& hole, const std::vector<Pt>& hull)
{
   if (hull.size() < 3)
      return false;
   using namespace detail;
   std::vector<D2> subj;
   for (const Pt& p : Outline(hole))
      subj.push_back({ p.x, p.y });
   if (subj.size() < 3)
      return false;
   const double cx = hole.type == HoleType::Slot ? 0.5 * ((double)hole.x + hole.x2) : hole.x, cy = hole.type == HoleType::Slot ? 0.5 * ((double)hole.y + hole.y2) : hole.y;
   bool inside = true;
   for (size_t i = 0; i < hull.size() && inside; i++)
      inside = Orient({ hull[i].x, hull[i].y }, { hull[(i + 1) % hull.size()].x, hull[(i + 1) % hull.size()].y }, { cx, cy }) >= 0.;
   if (inside)
      return true;
   for (size_t i = 0; i < hull.size() && !subj.empty(); i++) // Sutherland-Hodgman against each hull edge
   {
      const D2 a { hull[i].x, hull[i].y }, b { hull[(i + 1) % hull.size()].x, hull[(i + 1) % hull.size()].y };
      std::vector<D2> in;
      in.swap(subj);
      for (size_t j = 0; j < in.size(); j++)
      {
         const D2 p = in[j], q = in[(j + 1) % in.size()];
         const double op = Orient(a, b, p), oq = Orient(a, b, q);
         if (op >= 0.)
            subj.push_back(p);
         if ((op > 0. && oq < 0.) || (op < 0. && oq > 0.))
         {
            const double t = op / (op - oq);
            subj.push_back({ p.x + t * (q.x - p.x), p.y + t * (q.y - p.y) });
         }
      }
   }
   std::vector<D2> full;
   for (const Pt& p : Outline(hole))
      full.push_back({ p.x, p.y });
   const double total = std::fabs(SignedArea(full));
   return subj.size() >= 3 && total > 0. && std::fabs(SignedArea(subj)) > 0.5 * total;
}

} // namespace MaskTrace
