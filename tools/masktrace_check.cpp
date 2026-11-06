// Standalone check of src/math/MaskTrace.h: clang++ -std=c++20 -I src tools/masktrace_check.cpp -o /tmp/mtc && /tmp/mtc  (also with -O2 -ffast-math, as the engine builds)
// argv[1] = "bench": also times a real-size synthetic mask (3072 x 6781) on top of the checks.
#include "math/MaskTrace.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
using namespace MaskTrace;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d %s: ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

// NaN by bits: -ffast-math makes isnan() constant false.
static bool bad(float f) { uint32_t b; memcpy(&b, &f, 4); return (b & 0x7f800000u) == 0x7f800000u; }

struct Canvas
{
   int w, h;
   std::vector<uint8_t> g; // gray, 255 = white
   Canvas(int w_, int h_) : w(w_), h(h_), g((size_t)w_ * h_, 255) {}
   // black where pred(x, y) (pixel space); aa = 4x4 supersampling
   void draw(const std::function<bool(double, double)>& pred, bool aa)
   {
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w; x++)
         {
            int n = 0;
            const int s = aa ? 4 : 1;
            for (int j = 0; j < s; j++)
               for (int i = 0; i < s; i++)
                  n += pred(x + (i + 0.5) / s, y + (j + 0.5) / s);
            if (n) g[(size_t)y * w + x] = (uint8_t)std::min<int>(g[(size_t)y * w + x], 255 - 255 * n / (s * s));
         }
   }
};
static bool inRect(double x, double y, double x0, double y0, double x1, double y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }
static bool inDisc(double x, double y, double cx, double cy, double r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

static Params P(const Canvas& c, double sx) // table = pixels * sx, origin at (100, 50)
{
   Params p;
   p.left = 100.f, p.top = 50.f, p.right = (float)(100 + c.w * sx), p.bottom = (float)(50 + c.h * sx);
   return p;
}
static Image Img(const std::vector<uint8_t>& d, int w, int h, int ch) { Image i; i.data = d.data(); i.w = w; i.h = h; i.channels = ch; return i; }
static double area(const std::vector<Pt>& p) { double a = 0; for (size_t i = 0; i < p.size(); i++) a += (double)p[i].x * p[(i + 1) % p.size()].y - (double)p[(i + 1) % p.size()].x * p[i].y; return 0.5 * a; }
static bool same(const Result& a, const Result& b)
{
   if (a.holes.size() != b.holes.size() || a.components != b.components || a.outlineCuts.size() != b.outlineCuts.size()) return false;
   for (size_t i = 0; i < a.holes.size(); i++)
   {
      const Hole &p = a.holes[i], &q = b.holes[i];
      if (p.type != q.type || memcmp(&p.x, &q.x, 5 * sizeof(float)) || p.polygon.size() != q.polygon.size()) return false;
      if (p.polygon.size() && memcmp(p.polygon.data(), q.polygon.data(), p.polygon.size() * sizeof(Pt))) return false;
   }
   return true;
}
static void allFinite(const Result& r)
{
   for (const Hole& h : r.holes)
   {
      CHECK(!bad(h.x) && !bad(h.y) && !bad(h.diameter) && !bad(h.x2) && !bad(h.y2) && !bad(h.area) && !bad(h.minX) && !bad(h.maxY), "NaN/inf in a hole");
      for (const Pt& p : h.polygon) CHECK(!bad(p.x) && !bad(p.y), "NaN/inf in a polygon");
   }
}
static bool simple(const std::vector<Pt>& p) { std::vector<detail::D2> d; for (const Pt& q : p) d.push_back({ q.x, q.y }); return !detail::SelfIntersects(d); }
static bool polysTouch(const std::vector<Pt>& a, const std::vector<Pt>& b)
{
   for (size_t i = 0; i < a.size(); i++)
      for (size_t j = 0; j < b.size(); j++)
         if (detail::SegsTouch({ a[i].x, a[i].y }, { a[(i + 1) % a.size()].x, a[(i + 1) % a.size()].y }, { b[j].x, b[j].y }, { b[(j + 1) % b.size()].x, b[(j + 1) % b.size()].y })) return true;
   return false;
}

// L shape: a w x t bar plus a t x h bar sharing the corner (x0, y0); arms right and down
static bool inL(double x, double y, double x0, double y0, double len, double t) { return inRect(x, y, x0, y0, x0 + len, y0 + t) || inRect(x, y, x0, y0, x0 + t, y0 + len); }

static void scene(bool aa)
{
   const double sx = 0.5; // 1 px = 0.5 VPU
   Canvas c(800, 500);
   c.draw([](double x, double y) { return inDisc(x, y, 60, 60, 20); }, aa);       // circle d 40
   c.draw([](double x, double y) { return inDisc(x, y, 160, 70, 30); }, aa);      // circle d 60
   c.draw([](double x, double y) { return inDisc(x, y, 290, 80, 45); }, aa);      // circle d 90
   c.draw([](double x, double y) {                                                // slot: width 16, centre line (400,60)-(480,100)
      const double ax = 80, ay = 40, l2 = ax * ax + ay * ay, t = std::clamp(((x - 400) * ax + (y - 60) * ay) / l2, 0.0, 1.0);
      return std::hypot(x - 400 - t * ax, y - 60 - t * ay) <= 8; }, aa);
   c.draw([](double x, double y) { return inL(x, y, 40, 160, 100, 30); }, aa);    // L, 100 x 100, arm 30
   c.draw([](double x, double y) { return inL(x, y, 200, 160, 100, 30); }, aa);   // near-touching L pair: B nested inside A's elbow with a 1 px gap...
   c.draw([](double x, double y) { return inRect(x, y, 231, 191, 291, 221) || inRect(x, y, 231, 191, 261, 251); }, aa); // ...a smaller L, gap 1 px to A
   c.draw([](double x, double y) { return inDisc(x, y, 600, 60, 1.5); }, aa);     // pilot dot, 3 px = 1.5 VPU < 2.35
   c.draw([](double x, double y) { return inRect(x, y, 0, 300, 40, 360); }, aa);  // touches the left edge
   c.draw([](double x, double y) { return inRect(x, y, 400, 300, 430, 330) || inRect(x, y, 430, 330, 460, 360); }, false); // two squares touching at a corner

   const Params p = P(c, sx);
   Result r, r2;
   CHECK(Trace(Img(c.g, c.w, c.h, 1), p, r), "trace failed");
   CHECK(Trace(Img(c.g, c.w, c.h, 1), p, r2), "trace failed");
   printf("scene aa=%d: components %d, holes %zu, small %d, edge %zu, mode %d\n", aa, r.components, r.holes.size(), r.droppedSmall, r.outlineCuts.size(), (int)r.usedMode);
   CHECK(r.usedMode == Mode::Dark, "auto mode");
   CHECK(r.components == 11, "components %d", r.components); // 3 circles, slot, L, L pair (2), dot, edge shape, diagonal squares (2)
   CHECK(r.droppedSmall == 1, "dropped %d", r.droppedSmall);
   CHECK(r.outlineCuts.size() == 1, "outline cuts %zu", r.outlineCuts.size());
   CHECK(r.holes.size() == 9, "holes %zu", r.holes.size());
   CHECK(same(r, r2), "not the same twice");
   allFinite(r);
   int round = 0, slot = 0, poly = 0;
   for (const Hole& h : r.holes) { round += h.type == HoleType::Round; slot += h.type == HoleType::Slot; poly += h.type == HoleType::Polygon; }
   CHECK(round == 3 && slot == 1 && poly == 5, "round %d slot %d poly %d", round, slot, poly);
   if (r.holes.size() < 9) return;
   // raster order: three circles share no row start order; find by position
   const double want[3][3] = { { 60, 60, 40 }, { 160, 70, 60 }, { 290, 80, 90 } };
   for (auto& w : want)
      for (const Hole& h : r.holes)
         if (h.type == HoleType::Round && std::fabs(h.x - (100 + w[0] * sx)) < 2 && std::fabs(h.y - (50 + w[1] * sx)) < 2)
         {
            CHECK(std::fabs(h.diameter - w[2] * sx) <= 1.0 * sx, "circle d %g want %g", h.diameter, w[2] * sx);
            CHECK(std::fabs(h.x - (100 + w[0] * sx)) <= 0.5 * sx && std::fabs(h.y - (50 + w[1] * sx)) <= 0.5 * sx, "circle centre %g %g", h.x, h.y);
         }
   for (const Hole& h : r.holes)
   {
      if (h.type == HoleType::Slot)
      {
         CHECK(std::fabs(h.diameter - 16 * sx) <= 1.0 * sx, "slot d %g", h.diameter);
         const double l = std::hypot(h.x2 - h.x, h.y2 - h.y);
         CHECK(std::fabs(l - std::hypot(80, 40) * sx) <= 1.5 * sx, "slot length %g want %g", l, std::hypot(80, 40) * sx);
      }
      if (h.type == HoleType::Polygon)
      {
         CHECK(simple(h.polygon), "self-intersecting polygon");
         CHECK(area(h.polygon) < 0, "polygon not clockwise");
      }
   }
   // the L: 6 corners, area 100*30 + 70*30 = 5100 px^2
   int ls = 0;
   for (const Hole& h : r.holes)
      if (h.type == HoleType::Polygon && std::fabs(std::fabs(area(h.polygon)) - 5100 * sx * sx) < 40 * sx * sx * 5)
      {
         ls++;
         CHECK(h.polygon.size() == 6, "L has %zu points", h.polygon.size());
      }
   CHECK(ls >= 1, "no L found");
   // near-touching pair: both there, never touching
   std::vector<const Hole*> big;
   for (const Hole& h : r.holes)
      if (h.type == HoleType::Polygon && h.minY > 50 + 150 * sx && h.minX > 100 + 190 * sx && h.maxX < 100 + 310 * sx) big.push_back(&h);
   CHECK(big.size() == 2, "pair: %zu holes", big.size());
   if (big.size() == 2) CHECK(!polysTouch(big[0]->polygon, big[1]->polygon), "pair touches");
   // diagonal squares stayed two holes
   int sq = 0;
   for (const Hole& h : r.holes)
      if (h.type == HoleType::Polygon && h.minX > 100 + 395 * sx && h.minY > 50 + 295 * sx && h.maxX < 100 + 465 * sx) sq++;
   CHECK(sq == 2, "diagonal squares gave %d holes", sq);

   // other mask polarities and formats give the same holes: inverted (light), RGB, RGBA with opaque alpha, transparent holes, odd stride
   std::vector<uint8_t> inv(c.g), rgb(c.g.size() * 3), rgba(c.g.size() * 4), tr(c.g.size() * 4), strided((size_t)(c.w + 7) * c.h, 0);
   for (size_t i = 0; i < c.g.size(); i++)
   {
      inv[i] = 255 - c.g[i];
      rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] = c.g[i];
      rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = c.g[i], rgba[4 * i + 3] = 255;
      tr[4 * i] = tr[4 * i + 1] = tr[4 * i + 2] = 200, tr[4 * i + 3] = 255 - (255 - c.g[i]);
      strided[(i / c.w) * (c.w + 7) + i % c.w] = c.g[i];
   }
   // transparent: alpha 0 where hole -> alpha = c.g (white = opaque)
   Result a, b, d, e, f;
   Image si = Img(strided, c.w, c.h, 1); si.stride = (size_t)c.w + 7;
   CHECK(Trace(Img(inv, c.w, c.h, 1), p, a) && Trace(Img(rgb, c.w, c.h, 3), p, b) && Trace(Img(rgba, c.w, c.h, 4), p, d) && Trace(Img(tr, c.w, c.h, 4), p, e) && Trace(si, p, f), "trace");
   CHECK(a.usedMode == Mode::Light && e.usedMode == Mode::Transparent && b.usedMode == Mode::Dark && d.usedMode == Mode::Dark, "auto modes %d %d %d %d", (int)a.usedMode, (int)e.usedMode, (int)b.usedMode, (int)d.usedMode);
   CHECK(same(r, a) && same(r, b) && same(r, d) && same(r, e) && same(r, f), "mask formats disagree");
   Params po = p; po.mode = Mode::Light;
   Result forced; CHECK(Trace(Img(inv, c.w, c.h, 1), po, forced) && same(r, forced), "forced Light");

   // counter-clockwise input: the image flipped top to bottom reverses the trace direction; the output is clockwise all the same
   Canvas fl(c.w, c.h);
   for (int y = 0; y < c.h; y++) memcpy(&fl.g[(size_t)y * c.w], &c.g[(size_t)(c.h - 1 - y) * c.w], c.w);
   Result g; CHECK(Trace(Img(fl.g, c.w, c.h, 1), p, g), "trace");
   CHECK(g.holes.size() == r.holes.size(), "flipped holes %zu", g.holes.size());
   for (const Hole& h : g.holes) if (h.type == HoleType::Polygon) CHECK(area(h.polygon) < 0 && simple(h.polygon), "flipped polygon winding");
   // RDP itself on a counter-clockwise point list (a square with extra collinear points)
   std::vector<detail::D2> sqr; for (int i = 0; i < 10; i++) sqr.push_back({ (double)i, 0 }); for (int i = 0; i < 10; i++) sqr.push_back({ 10, (double)i }); for (int i = 10; i > 0; i--) sqr.push_back({ (double)i, 10 }); for (int i = 10; i > 0; i--) sqr.push_back({ 0, (double)i });
   CHECK(detail::SignedArea(sqr) > 0 && detail::Rdp(sqr, 0.1).size() == 4, "RDP of a ccw square: %zu", detail::Rdp(sqr, 0.1).size());
}

static void misc()
{
   // Min Size and the edge: a 4 px square at 0.5 VPU/px is 2 VPU, dropped at the default 2.35 and kept at 1.0
   Canvas c(100, 100);
   c.draw([](double x, double y) { return inRect(x, y, 40, 40, 44, 44); }, false);
   Params p = P(c, 0.5);
   Result r; CHECK(Trace(Img(c.g, 100, 100, 1), p, r) && r.holes.empty() && r.droppedSmall == 1, "min size drop");
   p.minSize = 1.f; CHECK(Trace(Img(c.g, 100, 100, 1), p, r) && r.holes.size() == 1, "min size keep");
   // a plain white or black image: no holes, no crash
   std::vector<uint8_t> white(100 * 100, 255), black(100 * 100, 0);
   CHECK(Trace(Img(white, 100, 100, 1), p, r) && r.holes.empty(), "white");
   CHECK(Trace(Img(black, 100, 100, 1), p, r) && r.holes.empty() && r.outlineCuts.size() <= 1, "black");
   CHECK(!Trace(Image(), p, r), "empty image accepted");
   // Under Insert
   Hole h; h.type = HoleType::Round; h.x = 10.f, h.y = 10.f, h.diameter = 4.f;
   const std::vector<Pt> hull = ConvexHull({ { 0, 0 }, { 20, 0 }, { 20, 20 }, { 0, 20 }, { 10, 10 } });
   CHECK(hull.size() == 4 && UnderInsert(h, hull), "centre inside");
   h.x = 21.5f; h.y = 10.f; CHECK(!UnderInsert(h, hull), "mostly outside (centre outside, a quarter of the area in)");
   h.x = 19.f; CHECK(UnderInsert(h, hull), "centre inside near the edge");
   Hole q; q.type = HoleType::Polygon; q.polygon = { { 18, 0 }, { 40, 0 }, { 40, 4 }, { 18, 4 } }; q.x = 29.f; q.y = 2.f; // 2 of 22 wide inside
   CHECK(!UnderInsert(q, hull), "polygon mostly outside");
   q.polygon = { { -10, 0 }, { 14, 0 }, { 14, 4 }, { -10, 4 } }; q.x = -5.f; // centre outside, 14 of 24 inside
   CHECK(UnderInsert(q, hull), "polygon more than half inside");
}

// Small L shapes stay polygons (a circle fit must also match the traced area; contours of squares under ~12 px are octagons and fit a circle, as they should), nested small L pairs with a 1 px gap never touch, Transparent on an RGB image reports its fallback
static void smallShapes()
{
   for (int arm = 8; arm <= 22; arm++)
      for (int t = 2; t <= 7; t++)
         for (int ot = 2; ot <= 6; ot++)
            for (int gap = 1; gap <= 2; gap++)
            {
               Canvas c(100, 100);
               c.draw([&](double x, double y) { return inL(x, y, 10, 10, arm + t + gap + ot + 4, ot) || inL(x, y, 10 + ot + gap, 10 + ot + gap, arm, t); }, false); // outer L with a smaller L nested in its elbow
               Params p = P(c, 1.0);
               p.minSize = 0.5f;
               Result r;
               CHECK(Trace(Img(c.g, c.w, c.h, 1), p, r), "small trace");
               bool ok = r.holes.size() == 2;
               for (const Hole& h : r.holes)
                  ok = ok && h.type == HoleType::Polygon;
               if (ok) ok = !polysTouch(r.holes[0].polygon, r.holes[1].polygon);
               CHECK(ok, "nested small Ls: arm %d t %d outer %d gap %d: %zu holes, first type %d", arm, t, ot, gap, r.holes.size(), r.holes.empty() ? -1 : (int)r.holes[0].type);
            }
   Canvas sq(60, 60);
   sq.draw([](double x, double y) { return inRect(x, y, 20, 20, 27, 27); }, false); // a 7 px square
   for (int n = 8; n <= 10; n++) // solid squares and diamonds are polygons, never zero-length slots
      for (int rot = 0; rot < 2; rot++)
      {
         Canvas k(60, 60);
         k.draw([&](double x, double y) { return rot ? std::fabs(x - 30) + std::fabs(y - 30) <= n * 0.75 : inRect(x, y, 20, 20, 20 + n, 20 + n); }, false);
         Result kr;
         CHECK(Trace(Img(k.g, k.w, k.h, 1), P(k, 1.0), kr) && kr.holes.size() == 1 && kr.holes[0].type != HoleType::Slot, "solid %d px square/diamond (rot %d) is not a slot", n, rot);
      }
   Result q;
   CHECK(Trace(Img(sq.g, sq.w, sq.h, 1), P(sq, 1.0), q) && q.holes.size() == 1 && q.holes[0].type == HoleType::Polygon, "small square is a polygon");
   Result r;
   Canvas d(60, 60);
   d.draw([](double x, double y) { return inDisc(x, y, 30, 30, 10); }, false);
   CHECK(Trace(Img(d.g, d.w, d.h, 1), P(d, 1.0), r) && r.holes.size() == 1 && r.holes[0].type == HoleType::Round, "disc still round");
   Params p = P(d, 1.0);
   p.mode = Mode::Transparent;
   CHECK(Trace(Img(d.g, d.w, d.h, 1), p, r) && r.noAlphaFallback && r.usedMode == Mode::Dark, "Transparent without alpha falls back to Dark");
}

static void bench()
{
   const int W = 3072, H = 6781;
   std::vector<uint8_t> img((size_t)W * H, 255);
   unsigned s = 12345;
   auto rnd = [&] { s = s * 1664525u + 1013904223u; return (s >> 8) & 0xffff; };
   int n = 0;
   for (int k = 0; k < 120; k++) // ~120 shapes, a mix like PF: small insert polygons, round holes, slots
   {
      const int cx = 150 + (int)(rnd() % (W - 300)), cy = 150 + (int)(rnd() % (H - 300)), kind = k % 3, r = 20 + (int)(rnd() % 80);
      for (int y = cy - r - 10; y <= cy + r + 10; y++)
         for (int x = cx - r - 10; x <= cx + r + 10; x++)
         {
            const bool in = kind == 0 ? (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r : kind == 1 ? (std::abs(x - cx) <= r / 4 && std::abs(y - cy) <= r) : (std::abs(x - cx) + std::abs(y - cy) <= r || (x > cx && x < cx + r && y > cy - r / 3 && y < cy + r / 3));
            if (in) img[(size_t)y * W + x] = 0;
         }
      n++;
   }
   Params p; p.left = 0.f, p.top = 0.f, p.right = 952.f, p.bottom = 2103.f;
   Image im = Img(img, W, H, 1);
   Result r;
   double best = 1e9;
   for (int i = 0; i < 3; i++)
   {
      const auto t0 = std::chrono::steady_clock::now();
      Trace(im, p, r);
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   }
   printf("bench %d x %d (BW, %d shapes drawn): %zu holes of %d components, best of 3 = %.3f s\n", W, H, n, r.holes.size(), r.components, best);
   std::vector<uint8_t> rgba((size_t)W * H * 4, 255);
   for (size_t i = 0; i < img.size(); i++) rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = img[i];
   im = Img(rgba, W, H, 4);
   const auto t0 = std::chrono::steady_clock::now();
   Trace(im, p, r);
   printf("bench RGBA: %.3f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

int main(int argc, char** argv)
{
   scene(false);
   scene(true);
   misc();
   smallShapes();
   if (argc > 1 && !strcmp(argv[1], "bench")) bench();
   printf(fails ? "FAILED: %d\n" : "all checks passed\n", fails);
   return fails ? 1 : 0;
}
