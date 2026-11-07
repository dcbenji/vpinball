// Standalone check of src/math/PrintBake.h: clang++ -std=c++20 -I src tools/printbake_check.cpp -o /tmp/pbc && /tmp/pbc  (also with -O2 -ffast-math, as the engine builds)
// argv[1] = "real <paint.raw> <print.raw> <mode> [out_prefix]": bake real maps (PF_art + PF_inkmask: mode 3, WW_Paint + WW_Print: mode 2) with the PFPK mode
//   (bit 0 alpha source, bit 1 invert), time it, compare random pixels with an independent bilinear in this file, and with out_prefix write <prefix>_print.raw / <prefix>_caps.raw ("w h 4\n" + RGBA). Raw dump format: "w h channels\n" then 8-bit pixels.
#include "math/PrintBake.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
using namespace PrintBake;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d %s: ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static BakeImage Img(const std::vector<uint8_t>& d, int w, int h, int ch) { BakeImage i; i.data = d.data(); i.w = w; i.h = h; i.channels = ch; return i; }
static std::vector<uint8_t> fill(int w, int h, int ch, const std::function<uint8_t(int x, int y, int c)>& f)
{
   std::vector<uint8_t> d((size_t)w * h * ch);
   for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) d[((size_t)y * w + x) * ch + c] = f(x, y, c);
   return d;
}

static void small()
{
   // 2 x 2 RGB paint, 2 x 2 RGBA print mask, same size: exact, no resampling
   const std::vector<uint8_t> paint = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120 };
   const std::vector<uint8_t> mask = { 255, 255, 255, 0,   0, 0, 0, 255,   90, 120, 150, 128,   30, 30, 30, 64 }; // lum: 255 0 120 30; alpha: 0 255 128 64
   BakeResult r;
   BakeParams p;
   CHECK(Bake(Img(paint, 2, 2, 3), Img(mask, 2, 2, 4), p, r), "bake");
   CHECK(r.w == 2 && r.h == 2 && r.print.size() == 16 && r.caps.empty(), "size %d x %d, caps %zu", r.w, r.h, r.caps.size());
   const int lumInk[4] = { 255, 0, 120, 30 }, alphaInk[4] = { 0, 255, 128, 64 };
   for (int i = 0; i < 4; i++)
   {
      CHECK(r.print[4 * i] == paint[3 * i] && r.print[4 * i + 1] == paint[3 * i + 1] && r.print[4 * i + 2] == paint[3 * i + 2], "rgb %d", i);
      CHECK(r.print[4 * i + 3] == 255 - lumInk[i], "luminance alpha %d: %d want %d", i, r.print[4 * i + 3], 255 - lumInk[i]);
   }
   p.printMode = kSourceAlpha;
   p.makeCaps = true;
   CHECK(Bake(Img(paint, 2, 2, 3), Img(mask, 2, 2, 4), p, r) && r.caps.size() == 16, "bake alpha");
   for (int i = 0; i < 4; i++)
   {
      CHECK(r.print[4 * i + 3] == 255 - alphaInk[i] && r.caps[4 * i + 3] == alphaInk[i], "alpha source %d", i);
      CHECK(r.caps[4 * i] == r.print[4 * i] && r.caps[4 * i + 1] == r.print[4 * i + 1] && r.caps[4 * i + 2] == r.print[4 * i + 2], "caps rgb %d", i);
   }
   CHECK(std::fabs(r.inkCoverage - (0 + 255 + 128 + 64) / (4. * 255)) < 1e-9, "coverage %g", r.inkCoverage);
   // PF's inkmask: alpha source + invert -> the baked alpha is the mask's own alpha
   p.printMode = kSourceAlpha | kInvert;
   CHECK(Bake(Img(paint, 2, 2, 3), Img(mask, 2, 2, 4), p, r), "bake alpha+invert");
   for (int i = 0; i < 4; i++) CHECK(r.print[4 * i + 3] == alphaInk[i] && r.caps[4 * i + 3] == 255 - alphaInk[i], "inkmask semantics %d: %d", i, r.print[4 * i + 3]);
   p.printMode = kInvert; // luminance inverted
   CHECK(Bake(Img(paint, 2, 2, 3), Img(mask, 2, 2, 4), p, r), "bake invert");
   for (int i = 0; i < 4; i++) CHECK(r.print[4 * i + 3] == lumInk[i], "luminance invert %d", i);
   // alpha source on an image without alpha: opaque = ink 1 (everything printed over, alpha 0); inverted: no ink, alpha 255
   const std::vector<uint8_t> gray = { 0, 100, 200, 255 };
   p.printMode = kSourceAlpha;
   CHECK(Bake(Img(paint, 2, 2, 3), Img(gray, 2, 2, 1), p, r), "bake");
   for (int i = 0; i < 4; i++) CHECK(r.print[4 * i + 3] == 0, "no alpha channel counts as opaque");
   p.printMode = kSourceAlpha | kInvert;
   CHECK(Bake(Img(paint, 2, 2, 3), Img(gray, 2, 2, 1), p, r), "bake");
   for (int i = 0; i < 4; i++) CHECK(r.print[4 * i + 3] == 255, "no alpha channel, inverted");
   // gray paint expands to r = g = b; luminance of a gray mask
   p.printMode = 0;
   const std::vector<uint8_t> gp = { 7, 8, 9, 10 };
   CHECK(Bake(Img(gp, 2, 2, 1), Img(gray, 2, 2, 1), p, r), "bake gray");
   for (int i = 0; i < 4; i++) CHECK(r.print[4 * i] == gp[i] && r.print[4 * i + 1] == gp[i] && r.print[4 * i + 2] == gp[i] && r.print[4 * i + 3] == 255 - gray[i], "gray %d", i);
   // RGBA paint: its alpha is ignored
   const std::vector<uint8_t> pa = { 1, 2, 3, 9, 4, 5, 6, 9, 7, 8, 9, 9, 10, 11, 12, 9 };
   CHECK(Bake(Img(pa, 2, 2, 4), Img(gray, 2, 2, 1), p, r) && r.print[0] == 1 && r.print[1] == 2 && r.print[2] == 3 && r.print[4] == 4, "rgba paint");
}

static void resample()
{
   BakeParams p;
   BakeResult r;
   // known 1D bilinear: paint 2 x 1 = {0, 100}, mask 4 x 1 (the output grid): positions -0.25 -> clamp 0, 0.25, 0.75, 1.25 -> clamp 1
   const std::vector<uint8_t> paint = { 0, 0, 0, 100, 100, 100 };
   const std::vector<uint8_t> mask(4, 0);
   CHECK(Bake(Img(paint, 2, 1, 3), Img(mask, 4, 1, 1), p, r) && r.w == 4 && r.h == 1, "bake up");
   const int want[4] = { 0, 25, 75, 100 };
   for (int i = 0; i < 4; i++) CHECK(r.print[4 * i] == want[i], "upsampled %d: %d want %d", i, r.print[4 * i], want[i]);
   // downsample 4x4 paint to the 2 x 2 grid: each output pixel samples the centre of its 2 x 2 block: the mean of the four
   const std::vector<uint8_t> big = fill(4, 4, 1, [](int x, int y, int) { return (uint8_t)(10 * x + 40 * y); });
   const std::vector<uint8_t> m2(4, 0);
   CHECK(Bake(Img(big, 4, 4, 1), Img(m2, 2, 2, 1), p, r), "bake down");
   for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++)
   {
      const double mean = (10 * (2 * i) + 40 * (2 * j) + 10 * (2 * i + 1) + 40 * (2 * j) + 10 * (2 * i) + 40 * (2 * j + 1) + 10 * (2 * i + 1) + 40 * (2 * j + 1)) / 4.;
      CHECK(std::fabs(r.print[(2 * j + i) * 4] - mean) <= 0.5, "downsampled (%d,%d): %d want %g", i, j, r.print[(2 * j + i) * 4], mean);
   }
   // the Print mask is resampled too, to its own capped grid: 8 x 2 mask capped at 4 wide -> 4 x 1; ink 255 in the left half, 0 right half
   const std::vector<uint8_t> hm = fill(8, 2, 1, [](int x, int, int) { return (uint8_t)(x < 4 ? 255 : 0); });
   p.maxWidth = 4;
   CHECK(Bake(Img(big, 4, 4, 1), Img(hm, 8, 2, 1), p, r) && r.w == 4 && r.h == 1, "cap");
   CHECK(r.print[3] == 0 && r.print[7] == 0 && r.print[11] == 255 && r.print[15] == 255, "mask resampled: alpha %d %d %d %d", r.print[3], r.print[7], r.print[11], r.print[15]);
   p.maxWidth = 2048;
   // the size rules
   int w, h;
   CHECK(OutputSize(3072, 6781, 2048, w, h) && w == 2048 && h == 4521, "PF map size %d x %d", w, h);
   CHECK(OutputSize(2992, 6575, 2048, w, h) && w == 2048 && h == 4501, "WW map size %d x %d", w, h);
   CHECK(OutputSize(2048, 4524, 2048, w, h) && w == 2048 && h == 4524, "at the cap: unchanged");
   CHECK(OutputSize(1000, 3000, 2048, w, h) && w == 1000 && h == 3000, "never upscaled");
   CHECK(OutputSize(4096, 1, 2048, w, h) && w == 2048 && h == 1, "height at least 1");
   CHECK(!OutputSize(0, 5, 2048, w, h) && !OutputSize(5, 0, 2048, w, h) && !OutputSize(5, 5, 0, w, h), "bad sizes");
   // a real-shaped (small) cap: 4096 x 1000 -> 2048 x 500
   const std::vector<uint8_t> wide(4096 * 1000, 128), pw(64 * 16 * 3, 90);
   CHECK(Bake(Img(pw, 64, 16, 3), Img(wide, 4096, 1000, 1), p, r) && r.w == 2048 && r.h == 500 && r.print.size() == (size_t)2048 * 500 * 4, "cap 2048");
   CHECK(r.print[0] == 90 && r.print[3] == 127, "constant inputs stay constant: rgb %d alpha %d", r.print[0], r.print[3]);
   // stride
   std::vector<uint8_t> padded((size_t)(2 * 3 + 5) * 2, 77);
   const std::vector<uint8_t> src = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120 };
   for (int y = 0; y < 2; y++) memcpy(&padded[(size_t)y * 11], &src[(size_t)y * 6], 6);
   BakeImage ip = Img(padded, 2, 2, 3); ip.stride = 11;
   const std::vector<uint8_t> m(4, 0);
   CHECK(Bake(ip, Img(m, 2, 2, 1), p, r) && r.print[0] == 10 && r.print[4] == 40 && r.print[8] == 70 && r.print[12] == 100, "strided paint");
   std::vector<uint8_t> mpad((size_t)(2 + 3) * 2, 200);
   mpad[0] = 0; mpad[1] = 255; mpad[5] = 255; mpad[6] = 0;
   BakeImage imp = Img(mpad, 2, 2, 1); imp.stride = 5;
   CHECK(Bake(Img(src, 2, 2, 3), imp, p, r) && r.print[3] == 255 && r.print[7] == 0 && r.print[11] == 0 && r.print[15] == 255, "strided mask");
   // bad input
   BakeImage none;
   CHECK(!Bake(none, Img(m, 2, 2, 1), p, r) && r.print.empty(), "null paint");
   CHECK(!Bake(Img(src, 2, 2, 3), none, p, r), "null mask");
   CHECK(!Bake(Img(src, 2, 2, 2), Img(m, 2, 2, 1), p, r), "2 channels");
   BakeParams bp; bp.maxWidth = 0;
   CHECK(!Bake(Img(src, 2, 2, 3), Img(m, 2, 2, 1), bp, r), "maxWidth 0");
}

static bool load(const char* path, std::vector<uint8_t>& d, BakeImage& im)
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

// independent bilinear of a per-pixel value f(x, y) at source coordinates (fx, fy), clamped, for the spot check
static double sample(const BakeImage& im, double fx, double fy, const std::function<double(const uint8_t*)>& f)
{
   const size_t stride = im.stride ? im.stride : (size_t)im.w * im.channels;
   const auto at = [&](int x, int y) { return f(im.data + (size_t)y * stride + (size_t)x * im.channels); };
   fx = std::min(std::max(fx, 0.), (double)im.w - 1), fy = std::min(std::max(fy, 0.), (double)im.h - 1);
   const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, im.w - 1), y1 = std::min(y0 + 1, im.h - 1);
   const double ax = fx - x0, ay = fy - y0;
   return (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
}

static void real(int argc, char** argv)
{
   std::vector<uint8_t> dp, dm;
   BakeImage paint, mask;
   if (argc < 5 || !load(argv[2], dp, paint) || !load(argv[3], dm, mask)) { fails++; return; }
   BakeParams p;
   p.printMode = (unsigned)atoi(argv[4]);
   p.makeCaps = true;
   BakeResult r;
   double best = 1e9;
   for (int i = 0; i < 3; i++)
   {
      const auto t0 = std::chrono::steady_clock::now();
      CHECK(Bake(paint, mask, p, r), "bake");
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   }
   printf("real bake: paint %d x %d x%d, print %d x %d x%d (mode %u) -> %d x %d RGBA, ink coverage %.1f%%, best of 3 = %.3f s (print + caps)\n", paint.w, paint.h, paint.channels, mask.w, mask.h, mask.channels, p.printMode, r.w, r.h, 100. * r.inkCoverage, best);
   p.makeCaps = false;
   const auto t0 = std::chrono::steady_clock::now();
   BakeResult r1;
   Bake(paint, mask, p, r1);
   printf("  print only: %.3f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
   CHECK(r1.print == r.print && r1.caps.empty(), "print only differs from print + caps");
   unsigned s = 4242;
   const auto rnd = [&] { s = s * 1664525u + 1013904223u; return (s >> 8) & 0xffffff; };
   const bool useAlpha = (p.printMode & kSourceAlpha) != 0, inv = (p.printMode & kInvert) != 0;
   const int mc = mask.channels, pcn = paint.channels;
   const auto inkAt = [&](const uint8_t* q) { const double v = useAlpha ? (mc == 4 ? q[3] : 255) : (mc == 1 ? q[0] : (q[0] + q[1] + q[2]) / 3); return inv ? 255. - v : v; };
   double worst = 0., worstRgb = 0.;
   for (int n = 0; n < 4000; n++)
   {
      const int x = (int)(rnd() % (unsigned)r.w), y = (int)(rnd() % (unsigned)r.h);
      const double fxp = (x + 0.5) * paint.w / r.w - 0.5, fyp = (y + 0.5) * paint.h / r.h - 0.5, fxm = (x + 0.5) * mask.w / r.w - 0.5, fym = (y + 0.5) * mask.h / r.h - 0.5;
      const uint8_t* o = &r.print[((size_t)y * r.w + x) * 4];
      for (int c = 0; c < 3; c++) worstRgb = std::max(worstRgb, std::fabs(o[c] - sample(paint, fxp, fyp, [&](const uint8_t* q) { return (double)q[pcn == 1 ? 0 : c]; })));
      worst = std::max(worst, std::fabs(o[3] - (255. - sample(mask, fxm, fym, inkAt))));
   }
   printf("  4000 random pixels vs an independent bilinear: worst rgb difference %.3f, worst alpha difference %.3f levels\n", worstRgb, worst);
   CHECK(worst <= 0.51 && worstRgb <= 0.51, "worst difference %g / %g", worst, worstRgb);
   if (argc > 5)
   {
      const std::string base = argv[5];
      for (int k = 0; k < 2; k++)
      {
         FILE* f = fopen((base + (k ? "_caps.raw" : "_print.raw")).c_str(), "wb");
         if (!f) continue;
         fprintf(f, "%d %d 4\n", r.w, r.h);
         const std::vector<uint8_t>& b = k ? r.caps : r.print;
         fwrite(b.data(), 1, b.size(), f);
         fclose(f);
      }
   }
}

// S1: thin ink lines survive a big downscale (the box average, not bilinear samples that skip source pixels)
static void thinLines()
{
   for (int pw : { 2992, 4096, 6144, 9000 })
   {
      const int ph = 8;
      std::vector<uint8_t> m((size_t)pw * ph, 255), p((size_t)pw * ph * 3, 128);
      int lines = 0;
      for (int x = 3; x < pw; x += 37, lines++)
         for (int y = 0; y < ph; y++) m[(size_t)y * pw + x] = 0; // 1 px black lines on white; luminance + invert: the lines are ink
      BakeParams prm;
      prm.printMode = kInvert;
      BakeResult r;
      CHECK(Bake(Img(p, pw, ph, 3), Img(m, pw, ph, 1), prm, r), "bake %d", pw);
      int seen = 0;
      for (int x = 3; x < pw; x += 37)
      {
         const int ox = (int)((x + 0.5) * r.w / pw);
         bool hit = false;
         for (int dx = -1; dx <= 1; dx++) { const int q = ox + dx; if (q >= 0 && q < r.w && r.print[3 + 4 * (size_t)q] < 255) hit = true; }
         seen += hit;
      }
      printf("thin lines: mask %d -> %d wide (ratio %.2f): %d of %d lines survive, ink coverage %.5f (want %.5f)\n", pw, r.w, (double)pw / r.w, seen, lines, r.inkCoverage, (double)lines / pw);
      CHECK(seen == lines, "mask %d: only %d of %d 1 px lines survive", pw, seen, lines);
      CHECK(std::fabs(r.inkCoverage - (double)lines / pw) < 0.15 * lines / pw + 1e-4, "ink conserved: %g want %g", r.inkCoverage, (double)lines / pw);
   }
   // a downscaled flat paint stays flat, a 2-colour paint averages (box weights sum to 1)
   const int pw = 6144, ph = 12;
   std::vector<uint8_t> p((size_t)pw * ph * 3), m((size_t)pw * ph * 4, 0);
   for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) for (int c = 0; c < 3; c++) p[((size_t)y * pw + x) * 3 + c] = (x % 3) * 90 + 30; // 30, 120, 210: mean 120
   BakeParams prm;
   BakeResult r;
   CHECK(Bake(Img(p, pw, ph, 3), Img(m, pw, ph, 4), prm, r), "bake flat");
   int worst = 0;
   for (int i = 0; i < r.w * r.h; i++) worst = std::max(worst, std::abs((int)r.print[4 * (size_t)i] - 120));
   CHECK(worst <= 1, "box average of 3-pixel stripes at 3x: worst deviation from 120 is %d", worst);
}

// N5: output size caps
static void caps()
{
   int w = 0, h = 0;
   CHECK(!OutputSize(2000, 2000000000, 2048, w, h), "2000 x 2e9 must fail");
   CHECK(!OutputSize(20000, 100, 20000, w, h), "20000 wide output must fail");
   CHECK(OutputSize(3072, 6781, 2048, w, h) && w == 2048 && h > 4500 && h < 4600, "PF size %d x %d", w, h);
   CHECK(OutputSize(2048, 16384, 2048, w, h) && h == 16384, "16384 high is allowed");
   CHECK(!OutputSize(2048, 16385, 2048, w, h), "16385 high is not");
   std::vector<uint8_t> p(4 * 3, 0), m(4 * 4, 0);
   BakeResult r;
   // a mask that is "huge" cannot be built here; the Bake path is guarded by the same OutputSize call, and a bad image fails cleanly
   BakeParams prm;
   prm.maxWidth = 20000;
   CHECK(!Bake(Img(p, 2, 2, 3), BakeImage(), prm, r) && r.print.empty() && r.w == 0, "bad image");
}

// Soft insert bodies: a clear window (alpha mask 0 = ink 0) in black printed ink, with a sharp dark letter bar and a flat colour inside; after the blur the window is
// the flat colour (the black ink around does not tint it) and the letter's edge is soft; off by default; radius 0 = a plain copy of the print
static void bodies()
{
   const int W = 160, H = 120;
   // Paint: orange (200, 120, 40) inside a window x 40..119, y 30..89, with a black vertical bar x 78..81 inside it; black around. Mask alpha 255 = ink (printMode alpha)
   const std::vector<uint8_t> paint = fill(W, H, 3, [](int x, int y, int c) {
      const bool win = x >= 40 && x < 120 && y >= 30 && y < 90;
      if (!win || (x >= 78 && x < 82)) return (uint8_t)0;
      return (uint8_t)(c == 0 ? 200 : c == 1 ? 120 : 40); });
   const std::vector<uint8_t> mask = fill(W, H, 4, [](int x, int y, int c) {
      if (c < 3) return (uint8_t)0;
      const bool win = x >= 40 && x < 120 && y >= 30 && y < 90;
      return (uint8_t)(win ? 0 : 255); });
   BakeParams p;
   p.printMode = kSourceAlpha;
   p.maxWidth = W;
   BakeResult r;
   CHECK(Bake(Img(paint, W, H, 3), Img(mask, W, H, 4), p, r) && r.bodies.empty(), "bodies not made by default");
   p.makeBodies = true;
   p.bodyBlurRadius = 6;
   CHECK(Bake(Img(paint, W, H, 3), Img(mask, W, H, 4), p, r) && r.bodies.size() == (size_t)W * H * 4, "bodies size");
   const auto px = [&](int x, int y, int c) { return (int)r.bodies[((size_t)y * W + x) * 4 + c]; };
   CHECK(px(60, 60, 3) == 255, "alpha 255");
   // the bar (black, but it is clear: weight 1) is blurred to a soft dip, far from it (x 50, 10 px from the bar) the window colour is nearly flat
   CHECK(std::abs(px(50, 60, 0) - 200) <= 8 && std::abs(px(50, 60, 1) - 120) <= 5, "window colour %d %d", px(50, 60, 0), px(50, 60, 1));
   CHECK(px(80, 60, 0) > 60 && px(80, 60, 0) < 190, "the bar is a soft dip: %d (sharp would be 0)", px(80, 60, 0));
   // the black ink around the window barely tints the window next to its edge (the weights of the inked pixels are 0.002)
   CHECK(px(42, 60, 0) > 150, "ink does not leak into the window: %d", px(42, 60, 0));
   // a long line: the top half clear (value 200), the bottom half ink (value 20), far from the window the colour stays the ink's (no float running-sum bias)
   {
      const int w2 = 8, h2 = 4524;
      const std::vector<uint8_t> pa = fill(w2, h2, 3, [h2](int, int y, int) { return (uint8_t)(y < h2 / 2 ? 200 : 20); });
      const std::vector<uint8_t> ma = fill(w2, h2, 4, [h2](int, int y, int c) { return (uint8_t)(c < 3 ? 0 : (y < h2 / 2 ? 0 : 255)); });
      BakeParams q = p;
      q.maxWidth = w2;
      q.bodyBlurRadius = 24;
      BakeResult r2;
      CHECK(Bake(Img(pa, w2, h2, 3), Img(ma, w2, h2, 4), q, r2), "bake long");
      CHECK(std::abs((int)r2.bodies[((size_t)(h2 - 20) * w2 + 4) * 4] - 20) <= 1, "long line, deep in the ink: %d (want 20)", (int)r2.bodies[((size_t)(h2 - 20) * w2 + 4) * 4]);
   }
   p.bodyBlurRadius = 0;
   CHECK(Bake(Img(paint, W, H, 3), Img(mask, W, H, 4), p, r), "bake radius 0");
   CHECK(px(80, 60, 0) == 0 && px(60, 60, 0) == 200, "radius 0 keeps the pixels: %d %d", px(80, 60, 0), px(60, 60, 0));
}

int main(int argc, char** argv)
{
   small();
   resample();
   thinLines();
   caps();
   bodies();
   if (argc > 4 && !strcmp(argv[1], "real")) real(argc, argv);
   printf(fails ? "FAILED: %d\n" : "all checks passed\n", fails);
   return fails ? 1 : 0;
}
