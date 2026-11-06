// Standalone check of src/math/MaskTrace.h: clang++ -std=c++20 -I src tools/masktrace_check.cpp -o /tmp/mtc && /tmp/mtc  (also with -O2 -ffast-math, as the engine builds)
// argv[1] = "bench": also times a real-size synthetic mask (3072 x 6781) on top of the checks.
// argv[1] = "real <file.raw> [holes round slots polygons]": Find Holes on a real mask (identity test: phase 1 gave 85 / 48 / 12 / 25 on PF_woodmask.png), and the outline trace on it.
// argv[1] = "strip <holes.raw> <expect: - | L | R | LR> [print.raw printMode]": the Fit Table to Paint strip warning on a real Holes mask (with the Print mask when given:
//   PF_inkmask is mode 3, WW_Print mode 2; without one the weaker one-sided hint rule runs) must fire on exactly the expected sides.
//   .raw = "w h channels\n" then the 8-bit pixels, row 0 first. python3: Image.open(png) -> np.asarray -> write f"{w} {h} {c}\n" + a.tobytes() (L = 1, RGB = 3, RGBA = 4 channels).
#include "math/MaskTrace.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
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

// ---- Phase 2: outline from the mask, strip detection, fit height ----

static bool outlineSimple(const OutlineResult& o) { return simple(o.polygon); }

static void outlineChecks()
{
   const double sx = 0.5;
   // A 400 x 600 px image: wood board from x 20..380, y 0..600 (reaches the top and bottom edges), plain background (hole) left and right of it.
   // On the board: a notch cut into the top-left edge of the board (touches the image's top), a notch in the right side (rectangle 40 x 100 from x 340), round holes.
   const auto build = [&](bool aa) {
      Canvas c(400, 600);
      c.draw([](double x, double y) { return inRect(x, y, 0, 0, 20, 600) || inRect(x, y, 380, 0, 400, 600); }, false);
      c.draw([](double x, double y) { return inRect(x, y, 100, 0, 160, 50); }, false);   // notch touching the top edge: a 60 x 50 bite
      c.draw([](double x, double y) { return inRect(x, y, 340, 200, 380, 300); }, false); // notch off the right side of the board: 40 x 100
      c.draw([](double x, double y) { return inDisc(x, y, 200, 300, 30); }, aa);          // interior holes must not change the outline
      c.draw([](double x, double y) { return inDisc(x, y, 250, 450, 20); }, aa);
      return c;
   };
   const Canvas c = build(false);
   const Params p = P(c, sx);
   Result hr;
   CHECK(Trace(Img(c.g, c.w, c.h, 1), p, hr), "trace");
   OutlineResult o;
   Params po = p;
   po.mode = hr.usedMode;
   CHECK(TraceOutline(Img(c.g, c.w, c.h, 1), po, o), "outline failed");
   // wood = board 360 x 600 minus bites: 60 x 50 + 40 x 100 px (interior holes are not subtracted: outer boundary only)
   const double want = (360. * 600 - 60 * 50 - 40 * 100) * sx * sx;
   printf("outline: %zu points, raw %zu, area %.1f want %.1f, wood components %d\n", o.polygon.size(), o.rawPoints, o.area, want, o.woodComponents);
   CHECK(std::fabs(o.area - want) < 0.003 * want, "outline area %g want %g", o.area, want);
   CHECK(area(o.polygon) > 0, "outline orientation: shoelace must be > 0");
   CHECK(outlineSimple(o), "outline self-intersects");
   CHECK(o.woodComponents == 1 && o.usedMode == Mode::Dark, "components %d mode %d", o.woodComponents, (int)o.usedMode);
   // 4 corners + the top notch (4 more) + the right notch (4 more) = 12 corners (the top notch cuts the straight top edge, the right one the straight right side)
   CHECK(o.polygon.size() == 12, "outline points %zu want 12", o.polygon.size());
   CHECK(std::fabs(o.minX - (100 + 20 * sx)) < 1e-3 && std::fabs(o.maxX - (100 + 380 * sx)) < 1e-3, "x bounds %g %g", o.minX, o.maxX);
   CHECK(std::fabs(o.minY - 50) < 1e-3 && std::fabs(o.maxY - (50 + 600 * sx)) < 1e-3, "contour must run along the image border: y %g %g", o.minY, o.maxY);
   for (const Pt& q : o.polygon) CHECK(!bad(q.x) && !bad(q.y), "NaN in outline");
   CHECK(!bad(o.area) && !bad(o.minX) && !bad(o.maxY), "NaN in outline fields");
   bool notch = false; // a vertex at the notch corner (x 160, y 50 px -> table 180, 75)
   for (const Pt& q : o.polygon) notch |= std::fabs(q.x - (100 + 160 * sx)) < 0.5 && std::fabs(q.y - (50 + 50 * sx)) < 0.5;
   CHECK(notch, "no vertex at the top notch's inner corner");
   // same board, anti-aliased holes: same polygon topology and area
   const Canvas ca = build(true);
   OutlineResult oa;
   CHECK(TraceOutline(Img(ca.g, ca.w, ca.h, 1), po, oa) && oa.polygon.size() == 12 && std::fabs(oa.area - o.area) < 0.003 * want, "aa outline differs");
   // Auto resolves like Trace; Light and Transparent versions of the same mask give the identical outline
   OutlineResult oauto;
   Params pa = p; pa.mode = Mode::Auto;
   CHECK(TraceOutline(Img(c.g, c.w, c.h, 1), pa, oauto) && oauto.polygon.size() == o.polygon.size() && oauto.area == o.area, "auto outline");
   std::vector<uint8_t> inv(c.g.size());
   for (size_t i = 0; i < inv.size(); i++) inv[i] = 255 - c.g[i];
   OutlineResult oi;
   Params pl = p; pl.mode = Mode::Light;
   CHECK(TraceOutline(Img(inv, c.w, c.h, 1), pl, oi) && oi.polygon.size() == o.polygon.size() && memcmp(oi.polygon.data(), o.polygon.data(), o.polygon.size() * sizeof(Pt)) == 0, "light outline");
   std::vector<uint8_t> rgba((size_t)c.w * c.h * 4, 255);
   for (size_t i = 0; i < c.g.size(); i++) rgba[4 * i + 3] = c.g[i] < 128 ? 0 : 255; // holes transparent
   OutlineResult ot;
   Params pt = p; pt.mode = Mode::Transparent;
   CHECK(TraceOutline(Img(rgba, c.w, c.h, 4), pt, ot) && ot.polygon.size() == o.polygon.size() && memcmp(ot.polygon.data(), o.polygon.data(), o.polygon.size() * sizeof(Pt)) == 0, "transparent outline");
   // stride and 3-channel input
   std::vector<uint8_t> padded((size_t)(c.w + 7) * c.h, 128);
   for (int y = 0; y < c.h; y++) memcpy(&padded[(size_t)y * (c.w + 7)], &c.g[(size_t)y * c.w], c.w);
   Image ip = Img(padded, c.w, c.h, 1); ip.stride = (size_t)c.w + 7;
   OutlineResult os;
   CHECK(TraceOutline(ip, po, os) && os.area == o.area && os.polygon.size() == o.polygon.size(), "strided outline");
   // the board touching no image edge: the contour is inside the image and a plain rectangle (4 points)
   Canvas m(300, 300);
   m.draw([](double x, double y) { return !inRect(x, y, 50, 40, 250, 260); }, false);
   OutlineResult om;
   Params pm2 = P(m, sx); pm2.mode = Mode::Dark;
   CHECK(TraceOutline(Img(m.g, m.w, m.h, 1), pm2, om) && om.polygon.size() == 4 && std::fabs(om.area - 200 * 220 * sx * sx) < 0.012 * 200 * 220 * sx * sx, "inset board outline %zu pts area %g", om.polygon.size(), om.area);
   // a fully wood image: the image rectangle; a fully hole image: false
   Canvas w(50, 40);
   OutlineResult ow;
   Params pw = P(w, sx); pw.mode = Mode::Dark;
   CHECK(TraceOutline(Img(w.g, w.w, w.h, 1), pw, ow) && ow.polygon.size() == 4 && std::fabs(ow.area - 50 * 40 * sx * sx) < 1e-3 * 50 * 40 * sx * sx && std::fabs(ow.minX - 100.f) < 1e-3f && std::fabs(ow.minY - 50.f) < 1e-3f && std::fabs(ow.maxX - 125.f) < 1e-3 && std::fabs(ow.maxY - 70.f) < 1e-3, "all wood: %zu pts area %g", ow.polygon.size(), ow.area);
   Canvas k(50, 40);
   k.draw([](double, double) { return true; }, false);
   OutlineResult ok2;
   CHECK(!TraceOutline(Img(k.g, k.w, k.h, 1), pw, ok2) && ok2.polygon.empty(), "all holes must fail");
   OutlineResult ob;
   CHECK(!TraceOutline(Image(), pw, ob), "null image");
   Params bp = pw; bp.right = bp.left;
   CHECK(!TraceOutline(Img(w.g, w.w, w.h, 1), bp, ob), "bad rect");
   // two wood components: the larger one wins
   Canvas t(200, 100);
   t.draw([](double x, double y) { return !(inRect(x, y, 10, 10, 60, 90) || inRect(x, y, 100, 10, 190, 90)); }, false);
   OutlineResult o2;
   Params p2 = P(t, sx); p2.mode = Mode::Dark;
   CHECK(TraceOutline(Img(t.g, t.w, t.h, 1), p2, o2) && o2.woodComponents == 2 && std::fabs(o2.area - 90 * 80 * sx * sx) < 0.02 * 90 * 80 * sx * sx, "largest wood component: %d comps, area %g", o2.woodComponents, o2.area);
   // Trace on the same image is unaffected by running the outline first (no shared state)
   Result h2;
   CHECK(Trace(Img(c.g, c.w, c.h, 1), p, h2) && same(hr, h2), "Trace changed");
}

// N2: the seam corner survives RDP (the ring used to start at the first raw point, 0.5 px from the corner)
static void cornerChecks()
{
   for (int n : { 1, 3, 10, 100 })
   {
      Canvas c(n, n);
      OutlineResult o;
      Params p = P(c, 1.0); p.mode = Mode::Dark;
      CHECK(TraceOutline(Img(c.g, n, n, 1), p, o), "%d x %d all wood", n, n);
      CHECK(o.polygon.size() == 4 && std::fabs(o.area - (double)n * n) < 1e-3 && o.minX == 100.f && o.minY == 50.f && o.maxX == 100.f + n && o.maxY == 50.f + n, "%d x %d all wood: %zu pts area %g (want %d)", n, n, o.polygon.size(), o.area, n * n);
      printf("corner check: %d x %d all wood -> %zu points, area %g\n", n, n, o.polygon.size(), o.area);
   }
   // a board that fills the image apart from a notch at the bottom right: the top-left seam corner is still exact
   Canvas c(100, 100);
   c.draw([](double x, double y) { return inRect(x, y, 60, 70, 100, 100); }, false);
   OutlineResult o;
   Params p = P(c, 1.0); p.mode = Mode::Dark;
   CHECK(TraceOutline(Img(c.g, 100, 100, 1), p, o) && o.polygon.size() == 6 && std::fabs(o.area - (10000. - 40 * 30)) < 0.002 * 10000. && o.minX == 100.f && o.minY == 50.f, "notched: %zu pts area %g", o.polygon.size(), o.area);
}

// S2/S3: a jagged board edge is capped at the load path's 4096 points
static void capChecks()
{
   const int W = 700, H = 4200;
   Canvas c(W, H);
   unsigned s = 12345;
   const auto rnd = [&] { s = s * 1664525u + 1013904223u; return (int)((s >> 16) % 7) - 3; }; // -3 .. +3
   std::vector<int> lx((size_t)H), rx((size_t)H);
   for (int y = 0; y < H; y++) lx[(size_t)y] = 40 + rnd(), rx[(size_t)y] = W - 40 + rnd();
   c.draw([&](double x, double y) { return x < lx[(size_t)y] || x >= rx[(size_t)y]; }, false);
   Params p = P(c, 1.0); p.mode = Mode::Dark;
   OutlineResult big, o;
   CHECK(TraceOutline(Img(c.g, W, H, 1), p, big, 1000000), "uncapped");
   CHECK(TraceOutline(Img(c.g, W, H, 1), p, o), "capped");
   printf("cap check: jagged edge, uncapped %zu points, capped %zu (tolerance doubled %d times), area %.0f vs %.0f\n", big.polygon.size(), o.polygon.size(), o.toleranceDoubled, big.area, o.area);
   CHECK(big.polygon.size() > 4096, "the test edge must exceed the cap uncapped (%zu)", big.polygon.size());
   CHECK(o.polygon.size() <= 4096 && o.polygon.size() >= 4 && o.toleranceDoubled >= 1 && !o.boundingBoxFallback, "capped: %zu points, doubled %d", o.polygon.size(), o.toleranceDoubled);
   CHECK(area(o.polygon) > 0 && outlineSimple(o), "capped outline orientation / simplicity");
   CHECK(std::fabs(o.area - big.area) < 0.01 * big.area, "capped area %g vs %g", o.area, big.area);
   CHECK(big.toleranceDoubled == 0, "no doubling when under the cap");
   OutlineResult tiny;
   CHECK(TraceOutline(Img(c.g, W, H, 1), p, tiny, 64) && tiny.polygon.size() <= 64 && area(tiny.polygon) > 0 && outlineSimple(tiny), "cap 64: %zu points", tiny.polygon.size());
}

static void stripChecks()
{
   // w x h wood board (white) with a full-height hole column of 3 px, a px from the left edge and b px from the right edge, and a disc in the middle
   // of each side, so Auto sees holes as the minority and the wood margins left and right are exactly a and b columns wide.
   const auto make = [](int w, int h, int a, int b) {
      Canvas c(w, h);
      if (a > 0) c.draw([&](double x, double y) { (void)y; return x >= a && x < a + 3; }, false);
      if (b > 0) c.draw([&](double x, double y) { (void)y; return x >= w - b - 3 && x < w - b; }, false);
      return c;
   };
   StripParams sp;
   StripScan s;
   Canvas c = make(400, 400, 12, 1); // 12 px = 3% of the width: a baked-in strip; 1 px (0.25%): the other side has no margin to speak of
   CHECK(DetectStrip(Img(c.g, c.w, c.h, 1), sp, s), "strip");
   printf("strip check: wood band 12 px left, 1 px right: L%d R%d fires %d hint %d\n", s.leftCols, s.rightCols, (int)s.fires, (int)s.hint);
   CHECK(s.fires && s.hint && s.leftCols == 12 && !s.leftIsHole && s.rightCols == 0 && s.usedMode == Mode::Dark, "wood band L%d R%d", s.leftCols, s.rightCols);
   c = make(400, 400, 6, 6); // 1.5%: a margin
   CHECK(DetectStrip(Img(c.g, c.w, c.h, 1), sp, s) && !s.fires && s.leftCols == 0 && s.rightCols == 0, "6 px margins must not fire");
   c = make(400, 400, 0, 20); // right wood band, left column 0 is a hole column right away (a = 0 draws nothing: column 0 stays wood)
   c.draw([](double x, double y) { (void)y; return x < 1; }, false); // hole column on the left edge: 1 px (0.25%, under the 0.5% opposite limit)
   CHECK(DetectStrip(Img(c.g, c.w, c.h, 1), sp, s) && s.fires && s.rightCols == 20 && !s.rightIsHole && s.leftCols == 0, "mixed: L%d R%d", s.leftCols, s.rightCols);
   // a margin on both sides: the mask alone cannot tell, and a two-sided pair of bands is a real margin, so the hint rule stays quiet
   c = make(400, 400, 12, 12);
   CHECK(DetectStrip(Img(c.g, c.w, c.h, 1), sp, s) && !s.fires && s.leftCols == 0 && s.rightCols == 0 && s.leftMaskCols == 12 && s.rightMaskCols == 12, "two-sided: L%d R%d", s.leftCols, s.rightCols);
   // an all-hole band: 12 px on the right edge (and a wood margin on the left): needs the Print mask to say so
   c = make(400, 400, 30, 0);
   c.draw([](double x, double y) { (void)y; return x >= 388; }, false);
   c.draw([](double x, double y) { return inDisc(x, y, 150, 200, 40); }, false);
   CHECK(DetectStrip(Img(c.g, c.w, c.h, 1), sp, s) && !s.fires, "mask only, bands on both sides: no fire");
   {
      const Canvas c2 = c;
      // Print masks (ink = dark with mode 2): `artFrom`..`artTo` columns carry art (a dotted pattern, never a uniform column), the rest is blank
      const auto printMask = [&](int pw, int ph, int artFrom, int artTo) {
         std::vector<uint8_t> d((size_t)pw * ph, 255);
         for (int y = 0; y < ph; y++) for (int x = artFrom; x < artTo; x++) if ((x * 7 + y * 13) % 5 == 0) d[(size_t)y * pw + x] = 0;
         return d;
      };
      const std::vector<uint8_t> blankRight = printMask(800, 800, 0, 770), artRight = printMask(800, 800, 0, 800), blankLeft = printMask(800, 800, 40, 800);
      Image pr = Img(blankRight, 800, 800, 1);
      CHECK(DetectStrip(Img(c2.g, c2.w, c2.h, 1), sp, s, &pr, 2) && s.fires && !s.hint && s.rightCols == 12 && s.rightIsHole && s.leftCols == 0, "hole band, blank print right: L%d R%d", s.leftCols, s.rightCols);
      pr = Img(artRight, 800, 800, 1); // a different size from the mask (fractions are compared)
      CHECK(DetectStrip(Img(c2.g, c2.w, c2.h, 1), sp, s, &pr, 2) && !s.fires && s.rightMaskCols == 12, "art to the edge: a real margin");
      pr = Img(blankLeft, 800, 800, 1);
      CHECK(DetectStrip(Img(c2.g, c2.w, c2.h, 1), sp, s, &pr, 2) && s.fires && s.leftCols == 30 && s.rightCols == 0 && !s.hint, "wood band 30 left, blank print left only: L%d R%d", s.leftCols, s.rightCols);
      // the print's own band must also be >= 2% of ITS width: 12 blank columns of 800 is 1.5%
      const std::vector<uint8_t> narrow = printMask(800, 800, 12, 800);
      pr = Img(narrow, 800, 800, 1);
      CHECK(DetectStrip(Img(c2.g, c2.w, c2.h, 1), sp, s, &pr, 2) && !s.fires, "blank print band under 2%% must not fire");
      // alpha source (mode 1/3): RGBA print, alpha = ink; PF's inkmask is alpha + invert
      std::vector<uint8_t> rgba(800 * 800 * 4, 255);
      for (int y = 0; y < 800; y++) for (int x = 0; x < 800; x++) rgba[((size_t)y * 800 + x) * 4 + 3] = (x >= 40 && (x * 7 + y * 13) % 5 == 0) ? 0 : 255; // alpha 1 where clear (PF convention)
      pr = Img(rgba, 800, 800, 4);
      CHECK(DetectStrip(Img(c2.g, c2.w, c2.h, 1), sp, s, &pr, 3) && s.fires && s.leftCols == 30, "alpha + invert print: L%d R%d fires %d", s.leftCols, s.rightCols, (int)s.fires);
   }
   // a band as wide as half the image or more is not a strip (nothing to scan against)
   Canvas blank(100, 100);
   CHECK(DetectStrip(Img(blank.g, blank.w, blank.h, 1), sp, s) && !s.fires && s.leftCols == 0 && s.rightCols == 0, "blank image must not fire");
   CHECK(!DetectStrip(Image(), sp, s), "null image");
   // 0.995 uniformity: a stray pixel per band column (1 of 400 rows = 0.25%) still counts as the band
   c = make(400, 400, 12, 1);
   for (int x = 0; x < 12; x++) c.g[(size_t)(x * 31 % 400) * c.w + x] = 0;
   CHECK(DetectStrip(Img(c.g, c.w, c.h, 1), sp, s) && s.fires && s.leftCols == 12, "stray pixels: L%d", s.leftCols);
   // the strip warning follows the resolved mode: a Light-mode mask (holes white) of the same board
   c = make(400, 400, 12, 1);
   std::vector<uint8_t> inv(c.g.size());
   for (size_t i = 0; i < inv.size(); i++) inv[i] = 255 - c.g[i];
   StripParams spl; spl.mode = Mode::Light;
   CHECK(DetectStrip(Img(inv, c.w, c.h, 1), spl, s) && s.fires && s.leftCols == 12 && !s.leftIsHole, "light mode: L%d", s.leftCols);
   CHECK(FitTableHeight(952.94f, 2992, 6575) > 2094.0f && FitTableHeight(952.94f, 2992, 6575) < 2094.3f, "fit height %g", FitTableHeight(952.94f, 2992, 6575));
   CHECK(FitTableHeight(100.f, 100, 200) == 200.f && FitTableHeight(0.f, 100, 200) == 0.f && FitTableHeight(100.f, 0, 200) == 0.f && FitTableHeight(100.f, 100, -1) == 0.f, "fit height edge cases");
}

// ---- real masks (raw dumps, see the header) ----

static bool loadRaw(const char* path, std::vector<uint8_t>& d, Image& im)
{
   FILE* f = fopen(path, "rb");
   if (!f) { printf("cannot open %s\n", path); return false; }
   int w = 0, h = 0, c = 0;
   if (fscanf(f, "%d %d %d\n", &w, &h, &c) != 3) { fclose(f); return false; }
   d.resize((size_t)w * h * c);
   const bool ok = fread(d.data(), 1, d.size(), f) == d.size();
   fclose(f);
   im = Img(d, w, h, c);
   return ok;
}

static void realChecks(int argc, char** argv)
{
   std::vector<uint8_t> d;
   Image im;
   if (!loadRaw(argv[2], d, im)) { fails++; return; }
   Params p; p.left = 0.f, p.top = 0.f, p.right = 952.94f, p.bottom = FitTableHeight(952.94f, im.w, im.h);
   Result r;
   CHECK(Trace(im, p, r), "trace");
   int round = 0, slot = 0, poly = 0;
   for (const Hole& h : r.holes) { round += h.type == HoleType::Round; slot += h.type == HoleType::Slot; poly += h.type == HoleType::Polygon; }
   printf("%s (%d x %d): holes %zu (round %d, slot %d, polygon %d), edge cuts %zu, components %d, dropped small %d, mode %d, rdp retried %d / unsimplified %d\n", argv[2], im.w, im.h, r.holes.size(), round, slot, poly, r.outlineCuts.size(), r.components, r.droppedSmall, (int)r.usedMode, r.rdpRetried, r.rdpUnsimplified);
   if (argc >= 7)
      CHECK(r.holes.size() == (size_t)atoi(argv[3]) && round == atoi(argv[4]) && slot == atoi(argv[5]) && poly == atoi(argv[6]), "identity: want %s / %s / %s / %s", argv[3], argv[4], argv[5], argv[6]);
   Params po = p;
   po.mode = r.usedMode;
   OutlineResult o;
   const auto t0 = std::chrono::steady_clock::now();
   CHECK(TraceOutline(im, po, o), "outline");
   const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   printf("outline: %zu points (raw contour %zu), area %.0f of %.0f VPU^2 (%.1f%%), bounds %.1f %.1f - %.1f %.1f, wood components %d, simplify retried %d / unsimplified %d, %.3f s\n", o.polygon.size(), o.rawPoints, o.area, (double)p.right * p.bottom, 100. * o.area / ((double)p.right * p.bottom), o.minX, o.minY, o.maxX, o.maxY, o.woodComponents, o.rdpRetried, o.rdpUnsimplified, dt);
   CHECK(area(o.polygon) > 0 && outlineSimple(o), "outline orientation / simplicity");
   for (const Pt& q : o.polygon) CHECK(!bad(q.x) && !bad(q.y), "NaN in outline");
}

static void stripReal(int argc, char** argv)
{
   std::vector<uint8_t> d1, d2;
   Image a, b;
   if (!loadRaw(argv[2], d1, a)) { fails++; return; }
   const bool withPrint = argc > 5;
   if (withPrint && !loadRaw(argv[4], d2, b)) { fails++; return; }
   StripScan sa;
   DetectStrip(a, StripParams(), sa, withPrint ? &b : nullptr, withPrint ? (unsigned)atoi(argv[5]) : 0u);
   const std::string want = argv[3];
   printf("strip scan: %s%s%s -> mask bands left %d right %d, fired left %d right %d, fires %d hint %d (want %s)\n", argv[2], withPrint ? " + " : "", withPrint ? argv[4] : "", sa.leftMaskCols, sa.rightMaskCols, sa.leftCols, sa.rightCols, (int)sa.fires, (int)sa.hint, want.c_str());
   CHECK((sa.leftCols > 0) == (want.find('L') != std::string::npos), "left side: fired %d want %s", sa.leftCols, want.c_str());
   CHECK((sa.rightCols > 0) == (want.find('R') != std::string::npos), "right side: fired %d want %s", sa.rightCols, want.c_str());
   CHECK(sa.fires == (want != "-") && (!withPrint || !sa.hint), "fires %d hint %d", (int)sa.fires, (int)sa.hint);
}

int main(int argc, char** argv)
{
   scene(false);
   scene(true);
   misc();
   smallShapes();
   outlineChecks();
   cornerChecks();
   capChecks();
   stripChecks();
   if (argc > 1 && !strcmp(argv[1], "bench")) bench();
   if (argc > 2 && !strcmp(argv[1], "real")) realChecks(argc, argv);
   if (argc > 3 && !strcmp(argv[1], "strip")) stripReal(argc, argv);
   printf(fails ? "FAILED: %d\n" : "all checks passed\n", fails);
   return fails ? 1 : 0;
}
