// license:GPLv3+

// Playfield Maker "Find Holes": traces a wood mask image (black shapes on white, white on black, or transparent) into hole outlines.
// No engine dependency (standard library only), so it can be checked standalone: clang++ -std=c++20 -I src tools/masktrace_check.cpp
// Pipeline (SPEC-playfield-maker.md, "Holes", Find Holes steps 2-6):
//   hole pixels (mask mode) -> 4-connected components -> drop those under Min Size -> components touching the image edge are outline cuts (listed, not traced)
//   -> outer contour by marching squares at the 50% level (crack following, foreground kept 4-connected at saddles; crossings interpolated between pixel centres)
//   -> circle test (least-squares circle) and slot test (capsule) on the unsimplified contour, else Ramer-Douglas-Peucker polygon.
// Phase 2 adds the outline trace (TraceOutline, P2-3 "outline from the mask") and the Fit Table to Paint helpers (DetectStrip, FitTableHeight) at the end.
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

inline int Lum(const uint8_t* q, int C) { return C == 1 ? (int)q[0] : ((int)q[0] + q[1] + q[2]) / 3; }

// Auto resolves to Transparent (alpha has both values), else the minority of dark vs light at 50% grey (a tie: dark). Transparent without alpha: Dark.
inline Mode ResolveMode(const Image& im, Mode mode, bool& noAlphaFallback)
{
   const int W = im.w, H = im.h, C = im.channels;
   const size_t stride = im.stride ? im.stride : (size_t)W * C;
   noAlphaFallback = false;
   if (mode == Mode::Auto)
   {
      size_t dark = 0, light = 0, clear = 0, opaque = 0;
      for (int y = 0; y < H; y++)
      {
         const uint8_t* row = im.data + (size_t)y * stride;
         for (int x = 0; x < W; x++)
         {
            const uint8_t* q = row + (size_t)x * C;
            (Lum(q, C) < 128 ? dark : light)++;
            if (C == 4)
               (q[3] < 128 ? clear : opaque)++;
         }
      }
      mode = (clear && opaque) ? Mode::Transparent : (dark <= light ? Mode::Dark : Mode::Light);
   }
   if (mode == Mode::Transparent && C != 4)
      mode = Mode::Dark, noAlphaFallback = true; // no alpha channel: nothing is transparent, fall back to the usual black shapes
   return mode;
}

// 0..255 "how much hole" of one pixel in a resolved mode; >= 128 is a hole pixel.
inline uint8_t CovAt(const uint8_t* q, int C, Mode mode)
{
   return mode == Mode::Transparent ? (uint8_t)(255 - q[3]) : mode == Mode::Light ? (uint8_t)Lum(q, C) : (uint8_t)(255 - Lum(q, C));
}

// Pixel (x, y) -> table units, as Trace maps them.
struct PixMap
{
   int W, H;
   double left, top, width, height; // the table rectangle
   double tx(double px) const { return left + px / W * width; }
   double ty(double py) const { return top + py / H * height; }
};

// Outer contour of the 4-connected foreground component whose first raster pixel is (x0, y0): crack following with the foreground on the left,
// foreground = plane >= 128, outside the image = 0, crossings interpolated at the 50% level between pixel centres. Directions E, S, W, N
// (clockwise on screen, y down). Table units. min/max: the component's pixel bounds (only to bound the loop).
inline std::vector<D2> CrackTrace(const uint8_t* plane, const PixMap& m, int x0, int y0, int minx, int miny, int maxx, int maxy)
{
   const int W = m.W, H = m.H;
   const auto fg = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && plane[(size_t)y * W + x] >= 128; };
   const auto val = [&](int x, int y) { return (x >= 0 && y >= 0 && x < W && y < H) ? (int)plane[(size_t)y * W + x] : 0; };
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
      const D2 q { m.tx(px), m.ty(py) };
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
   return raw;
}

// RDP at `tol`; a simplification that self-intersects is redone at half the tolerance, then falls back to the raw contour.
inline std::vector<D2> Simplify(const std::vector<D2>& raw, double tol, int& retried, int& unsimplified)
{
   std::vector<D2> poly = Rdp(raw, tol);
   if (poly.size() < 3 || SelfIntersects(poly))
   {
      retried++;
      poly = Rdp(raw, tol * 0.5);
      if (poly.size() < 3 || SelfIntersects(poly))
      {
         unsimplified++;
         poly = raw;
      }
   }
   return poly;
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

   // 2. Hole pixels (mode resolution and coverage: detail::ResolveMode / CovAt).
   const Mode mode = ResolveMode(im, prm.mode, out.noAlphaFallback);
   out.usedMode = mode;
   std::vector<uint8_t> cov((size_t)W * H); // 0..255 "how much hole"; >= 128 is a hole pixel
   for (int y = 0; y < H; y++)
   {
      const uint8_t* row = im.data + (size_t)y * stride;
      uint8_t* c = &cov[(size_t)y * W];
      for (int x = 0; x < W; x++)
         c[x] = CovAt(row + (size_t)x * C, C, mode);
   }
   const auto fg = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && cov[(size_t)y * W + x] >= 128; };

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

         // 4. Outer contour by crack following (detail::CrackTrace).
         const PixMap pm { W, H, prm.left, prm.top, (double)prm.right - prm.left, (double)prm.bottom - prm.top };
         const std::vector<D2> raw = CrackTrace(cov.data(), pm, x0, y0, minx, miny, maxx, maxy);
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
            std::vector<D2> poly = Simplify(raw, tolRdp, out.rdpRetried, out.rdpUnsimplified);
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

// ---- Phase 2 (P2-3): the board outline from the Wood Mask. ----

struct OutlineResult
{
   // The outer boundary of the largest WOOD component (the board where the mask is intact), out-of-image pixels counting as background, so where
   // the board reaches the image edge the contour runs along the image border and edge cuts (shooter lane, notches) cut into it.
   // RDP-simplified at Trace's tolerance, no circle or slot fitting, table units, shoelace area > 0 (counter-clockwise by the board's convention;
   // hole polygons are the opposite, < 0), no repeated first point. Interior holes are ignored (they are not part of the outer boundary).
   std::vector<Pt> polygon;
   float area = 0.f; // VPU^2 of the simplified polygon
   float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
   Mode usedMode = Mode::Dark; // the resolved hole mode that was inverted into wood
   bool noAlphaFallback = false;
   int woodComponents = 0; // 4-connected wood components found
   size_t woodPixels = 0; // pixels in the one traced
   size_t rawPoints = 0; // contour points before simplification
   int rdpRetried = 0, rdpUnsimplified = 0; // as Result
   int toleranceDoubled = 0; // times the RDP tolerance was doubled to get under maxPoints (0 = the first simplification fit)
   bool boundingBoxFallback = false; // no tolerance gave a valid polygon under maxPoints (not seen in practice): the polygon is the bounding rectangle of the contour
};

// Pass Params.mode = Result::usedMode of the last Find Holes (resolved, so Auto is only re-resolved if you pass it). Only vpuPerInch is used from Params
// besides the mode and the rectangle (minSize does not apply: the outline is the one largest component). The polygon has at most maxPoints points (default
// 4096, the load path's limit): while it has more, the RDP tolerance is doubled (toleranceDoubled), so a jagged hand-drawn edge still reloads.
// False (out cleared) for an unusable image or rectangle or a mask with no wood at all.
inline bool TraceOutline(const Image& im, const Params& prm, OutlineResult& out, size_t maxPoints = 4096)
{
   using namespace detail;
   out = OutlineResult();
   if (!im.data || im.w < 1 || im.h < 1 || (im.channels != 1 && im.channels != 3 && im.channels != 4) || !(prm.right > prm.left) || !(prm.bottom > prm.top))
      return false;
   const int W = im.w, H = im.h, C = im.channels;
   const size_t stride = im.stride ? im.stride : (size_t)W * C;
   const Mode mode = ResolveMode(im, prm.mode, out.noAlphaFallback);
   out.usedMode = mode;
   std::vector<uint8_t> wood((size_t)W * H); // 255 - hole coverage: >= 128 is wood (hole = cov >= 128, so wood = cov <= 127: the same 50% level)
   for (int y = 0; y < H; y++)
   {
      const uint8_t* row = im.data + (size_t)y * stride;
      uint8_t* c = &wood[(size_t)y * W];
      for (int x = 0; x < W; x++)
         c[x] = (uint8_t)(255 - CovAt(row + (size_t)x * C, C, mode));
   }
   // Largest 4-connected wood component (the first one in raster order wins a tie).
   std::vector<uint8_t> seen((size_t)W * H, 0);
   std::vector<uint32_t> stack;
   size_t bestN = 0;
   int bx0 = 0, by0 = 0, bminx = 0, bminy = 0, bmaxx = 0, bmaxy = 0;
   for (int y0 = 0; y0 < H; y0++)
      for (int x0 = 0; x0 < W; x0++)
      {
         const size_t i0 = (size_t)y0 * W + x0;
         if (seen[i0] || wood[i0] < 128)
            continue;
         int minx = x0, maxx = x0, miny = y0, maxy = y0;
         size_t n = 0;
         seen[i0] = 1;
         stack.assign(1, (uint32_t)i0);
         while (!stack.empty())
         {
            const uint32_t i = stack.back();
            stack.pop_back();
            n++;
            const int x = (int)(i % (uint32_t)W), y = (int)(i / (uint32_t)W);
            minx = std::min(minx, x), maxx = std::max(maxx, x), miny = std::min(miny, y), maxy = std::max(maxy, y);
            const int nx[4] = { x + 1, x - 1, x, x }, ny[4] = { y, y, y + 1, y - 1 };
            for (int k = 0; k < 4; k++)
               if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < W && ny[k] < H)
               {
                  const size_t j = (size_t)ny[k] * W + nx[k];
                  if (!seen[j] && wood[j] >= 128)
                  {
                     seen[j] = 1;
                     stack.push_back((uint32_t)j);
                  }
               }
         }
         out.woodComponents++;
         if (n > bestN)
            bestN = n, bx0 = x0, by0 = y0, bminx = minx, bminy = miny, bmaxx = maxx, bmaxy = maxy;
      }
   if (!bestN)
      return false;
   out.woodPixels = bestN;
   const PixMap pm { W, H, prm.left, prm.top, (double)prm.right - prm.left, (double)prm.bottom - prm.top };
   const std::vector<D2> raw = CrackTrace(wood.data(), pm, bx0, by0, bminx, bminy, bmaxx, bmaxy);
   // Crack midpoints cut the image corners (the contour passes (0.5, 0) then (0, 0.5)): where two consecutive points sit on different image borders,
   // the board owns the corner pixel, so put the exact corner between them. Then a board that fills the image keeps its corners through RDP.
   const double eps = 1e-6 * std::max(pm.width, pm.height), rl = pm.left, rt = pm.top, rr = pm.left + pm.width, rb = pm.top + pm.height;
   const auto sideOf = [&](const D2& q, double& cx, double& cy) { // 1 left, 2 right, 4 top, 8 bottom
      int m = 0;
      if (std::fabs(q.x - rl) < eps) m |= 1, cx = rl;
      if (std::fabs(q.x - rr) < eps) m |= 2, cx = rr;
      if (std::fabs(q.y - rt) < eps) m |= 4, cy = rt;
      if (std::fabs(q.y - rb) < eps) m |= 8, cy = rb;
      return m;
   };
   std::vector<D2> filled;
   filled.reserve(raw.size() + 8);
   size_t firstCorner = (size_t)-1;
   for (size_t i = 0; i < raw.size(); i++)
   {
      filled.push_back(raw[i]);
      const D2& a = raw[i];
      const D2& b = raw[(i + 1) % raw.size()];
      double ax = 0., ay = 0., bx = 0., by = 0.;
      const int ma = sideOf(a, ax, ay), mb = sideOf(b, bx, by);
      if (ma && mb && !(ma & mb) && ((ma & 3) != 0) != ((mb & 3) != 0)) // one on a left/right border, the other on a top/bottom border
      {
         if (firstCorner == (size_t)-1)
            firstCorner = filled.size();
         filled.push_back((ma & 3) ? D2 { ax, by } : D2 { bx, ay });
      }
   }
   out.rawPoints = filled.size();
   if (filled.size() < 3)
      return false;
   // RDP always keeps point 0, so start the ring at a corner fill point: a corner at the seam (the ring starts at (0.5, 0) for a board touching the top-left
   // pixel) would otherwise lie within tolerance of the chord across it and be dropped.
   if (firstCorner != (size_t)-1 && firstCorner > 0)
      std::rotate(filled.begin(), filled.begin() + (std::ptrdiff_t)firstCorner, filled.end());
   const double pxSize = std::max(pm.width / W, pm.height / H);
   double tol = std::max(pxSize, 0.005 * prm.vpuPerInch);
   std::vector<D2> poly = Simplify(filled, tol, out.rdpRetried, out.rdpUnsimplified);
   if (poly.size() > maxPoints)
   {
      // Too many points (a jagged edge, or the unsimplified fallback returned the raw ring): double the tolerance until the simplification is valid and fits.
      std::vector<D2> fit;
      for (int k = 0; k < 40 && fit.empty(); k++)
      {
         tol *= 2.;
         out.toleranceDoubled++;
         std::vector<D2> t = Rdp(filled, tol);
         if (t.size() < 3)
            break; // collapsed: larger tolerances only collapse more
         if (t.size() <= maxPoints && !SelfIntersects(t))
            fit = std::move(t);
      }
      if (fit.empty())
      {
         double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
         for (const D2& q : filled)
            x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
         fit = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
         out.boundingBoxFallback = true;
      }
      poly = std::move(fit);
   }
   if (SignedArea(poly) < 0.)
      std::reverse(poly.begin(), poly.end());
   out.area = (float)std::fabs(SignedArea(poly));
   out.minX = out.minY = 1e30f, out.maxX = out.maxY = -1e30f;
   out.polygon.reserve(poly.size());
   for (const D2& p : poly)
   {
      out.polygon.push_back({ (float)p.x, (float)p.y });
      out.minX = std::min(out.minX, (float)p.x), out.maxX = std::max(out.maxX, (float)p.x), out.minY = std::min(out.minY, (float)p.y), out.maxY = std::max(out.maxY, (float)p.y);
   }
   return true;
}

// ---- Phase 2 (P2-3): Fit Table to Paint support. The spec lists neither in the core table; they are pure, so they live here and the wiring just calls them. ----

// Table height (VPU) for a table `width` VPU wide that the Paint image (imgW x imgH pixels) covers exactly: width * imgH / imgW. 0 for an unusable input.
inline float FitTableHeight(float width, int imgW, int imgH)
{
   return (imgW > 0 && imgH > 0 && width > 0.f) ? (float)((double)width * imgH / imgW) : 0.f;
}

struct StripParams
{
   Mode mode = Mode::Auto; // pass the resolved mode of the last Find Holes if there is one
   float minFraction = 0.02f; // a band narrower than this share of the image width is a normal margin, not a baked-in strip (PF/WW: 80 px of 3072 = 2.6%; a real margin: 13 px of 2992 = 0.4%)
   float uniformity = 0.995f; // a column belongs to the band when this share of its pixels have the band's kind
   float oppositeMax = 0.005f; // no Ink Mask: the side opposite a band must be narrower than this share of the width
};

struct StripScan
{
   // A band of columns along the left or right image edge where every column is (almost) all one kind: all hole or all wood. The Wood Mask of a
   // paint that still carries freezy's plywood side strip has such a band (the strip is uniform: in WW's mask it is all wood, 80 px plus the margin).
   // Only columns are scanned: a hole-free band along the top is just the board's top (WW: the first 363 rows).
   // The Wood Mask alone cannot tell the strip from a real wood margin (PF has 88 px of wood on the left, 2.9%, the WW strip 93 px, 3.0%), so a side only
   // fires when the Ink Mask is ALSO uniform over the same share of its own width: a real margin is printed (art and ink run into it), the baked-in strip is not.
   int leftCols = 0, rightCols = 0; // mask band widths in pixels of the sides that fired, 0 = that side did not fire
   int leftMaskCols = 0, rightMaskCols = 0; // the raw mask bands (>= minFraction of the width, else 0), fired or not, for diagnostics
   bool leftIsHole = false, rightIsHole = false; // the band's kind
   bool fires = false; // either side is a baked-in strip: warn before Fit Table to Paint
   bool hint = false; // fires from the weaker mask-only rule (no Ink Mask given): show "check this", do not block
   Mode usedMode = Mode::Dark;
};

// printMask: optional (null = the weaker mask-only rule). printMode: the saved Ink Mask slot mode, bit 0 = source is alpha (an image without alpha counts as
// opaque), bit 1 = invert; the ink is >= 128 after both (as PrintBake). Without an Ink Mask a side fires only when exactly one side has a band >= minFraction
// and the opposite side's band is < oppositeMax (the strip is one-sided, a real margin is two-sided), and `hint` is set.
inline bool DetectStrip(const Image& im, const StripParams& prm, StripScan& out, const Image* printMask = nullptr, unsigned printMode = 0)
{
   using namespace detail;
   out = StripScan();
   if (!im.data || im.w < 2 || im.h < 1 || (im.channels != 1 && im.channels != 3 && im.channels != 4))
      return false;
   const int W = im.w, H = im.h, C = im.channels;
   const size_t stride = im.stride ? im.stride : (size_t)W * C;
   bool noAlpha;
   const Mode mode = ResolveMode(im, prm.mode, noAlpha);
   out.usedMode = mode;
   // The band of uniform columns from `from` stepping by `step`; stops scanning at half the width (a wider band is rejected anyway). kind(x, y) is the pixel kind.
   const auto band = [&](int w, int h, int from, int step, bool& isKind, const auto& kind) {
      const auto frac = [&](int x) {
         size_t n = 0;
         for (int y = 0; y < h; y++)
            n += kind(x, y);
         return (double)n / h;
      };
      isKind = frac(from) >= 0.5;
      int n = 0;
      for (int x = from; x >= 0 && x < w && n <= w / 2; x += step, n++)
      {
         const double f = frac(x);
         if ((isKind ? f : 1. - f) < prm.uniformity)
            break;
      }
      return n;
   };
   const auto holeKind = [&](int x, int y) { return CovAt(im.data + (size_t)y * stride + (size_t)x * C, C, mode) >= 128; };
   const auto bandCols = [&](int w, int n, float minFrac) { // the width in pixels, or 0 under minFrac of w or past half the width
      const int minCols = std::max(2, (int)std::ceil(minFrac * w));
      return (n < minCols || n > w / 2) ? 0 : n;
   };
   const int rawL = band(W, H, 0, 1, out.leftIsHole, holeKind), rawR = band(W, H, W - 1, -1, out.rightIsHole, holeKind);
   const int l = bandCols(W, rawL, prm.minFraction), r = bandCols(W, rawR, prm.minFraction);
   out.leftMaskCols = l, out.rightMaskCols = r;

   bool fireL = false, fireR = false;
   if (printMask && printMask->data && printMask->w >= 2 && printMask->h >= 1 && (printMask->channels == 1 || printMask->channels == 3 || printMask->channels == 4))
   {
      const Image& pm = *printMask;
      const int pc = pm.channels;
      const size_t ps = pm.stride ? pm.stride : (size_t)pm.w * pc;
      const bool useAlpha = (printMode & 1u) != 0, invert = (printMode & 2u) != 0;
      const auto inkKind = [&](int x, int y) {
         const uint8_t* q = pm.data + (size_t)y * ps + (size_t)x * pc;
         const int v = useAlpha ? (pc == 4 ? q[3] : 255) : Lum(q, pc);
         return (invert ? 255 - v : v) >= 128;
      };
      bool k;
      const int pl = l > 0 ? bandCols(pm.w, band(pm.w, pm.h, 0, 1, k, inkKind), prm.minFraction) : 0;
      const int pr = r > 0 ? bandCols(pm.w, band(pm.w, pm.h, pm.w - 1, -1, k, inkKind), prm.minFraction) : 0;
      fireL = l > 0 && pl > 0, fireR = r > 0 && pr > 0;
   }
   else
   {
      // exactly one side has a band and the opposite side's raw band (not minFraction-gated) is under oppositeMax of the width
      const double lim = (double)prm.oppositeMax * W;
      if (l > 0 && r == 0 && rawR < lim)
         fireL = true, out.hint = true;
      else if (r > 0 && l == 0 && rawL < lim)
         fireR = true, out.hint = true;
   }
   out.leftCols = fireL ? l : 0, out.rightCols = fireR ? r : 0;
   out.fires = fireL || fireR;
   return true;
}

} // namespace MaskTrace
