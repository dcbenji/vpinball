// license:GPLv3+

// Playfield Maker phase 2 (P2-4) "Bake Insert Print": composes the Paint image and the Print (ink) mask into the RGBA buffers the traced inserts read.
// No engine dependency (standard library only), so it can be checked standalone: clang++ -std=c++20 -I src tools/printbake_check.cpp
//   Insert Print: rgb = Paint, alpha = 1 - ink (1 where the insert shows clear, 0 where printed over: fs_insert_traced.sc:311-318).
//   Insert Caps (P2-5, only when Cut Insert Pockets is on): the same rgb, alpha = ink.
// Needs no traced inserts: the inputs are the two images only. The output has the Ink Mask's size, capped at maxWidth (2048) wide, aspect kept; both
// images cover the whole table, so both are resampled to that grid: bilinear (sampled at pixel centres, edges clamped) while the downscale ratio is at most 1.5
// (exact at the same size), an area-weighted box average above that, so a 1 px ink line survives a 3x downscale instead of falling between the samples.
// The output is at most kMaxSide (16384) pixels on a side; larger asks fail (Bake returns false, OutputSize returns false), they never allocate.
// The names BakeImage / BakeParams / BakeResult differ from MaskTrace's Image / Params / Result so one .cpp can use both namespaces.
// The Ink Mask's ink: ink = 1 after the source and the invert are applied. The source is the alpha channel (an image without alpha counts as opaque, ink 1
// before the invert) or the luminance (channel average, as MaskTrace); invert flips it. PF's PF_inkmask is alpha + invert (its alpha is 1 where clear), so
// the baked Insert Print alpha is exactly that mask's alpha. Output is straight (not premultiplied) 8-bit RGBA, row 0 first.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace PrintBake
{

// 8-bit pixels, row 0 first (the top of the table). channels: 1 = gray, 3 = RGB, 4 = RGBA (colour order does not matter here: Paint is copied channel for channel,
// the mask's luminance is the channel average). stride: bytes per row, 0 = w * channels.
struct BakeImage
{
   const uint8_t* data = nullptr;
   int w = 0, h = 0;
   int channels = 4;
   size_t stride = 0;
};

// The saved Ink Mask slot mode (PFPK).
enum PrintMode : unsigned { kSourceAlpha = 1, kInvert = 2 }; // bit 0: source = alpha (else luminance); bit 1: invert

struct BakeParams
{
   unsigned printMode = 0;
   int maxWidth = 2048; // output width cap; the Ink Mask is never upscaled
   bool makeCaps = false; // also fill BakeResult::caps (Insert Caps)
};

struct BakeResult
{
   int w = 0, h = 0;
   std::vector<uint8_t> print; // w * h * 4 RGBA: rgb = Paint, a = 255 - ink
   std::vector<uint8_t> caps; // same size, only if BakeParams::makeCaps: rgb = Paint, a = ink
   double inkCoverage = 0.; // mean ink, 0..1 (a quick sanity number: 0 = nothing printed, 1 = all ink)
};

constexpr int kMaxSide = 16384; // output cap per side (a 16384 x 16384 RGBA buffer is 1 GiB; real maps are 2048 x ~4500)

// The output size for an Ink Mask of pw x ph pixels: min(pw, maxWidth) wide, the height rounded to keep the aspect (at least 1). False for bad input or an
// output over kMaxSide on either side (call this first to know before baking).
inline bool OutputSize(int pw, int ph, int maxWidth, int& w, int& h)
{
   if (pw < 1 || ph < 1 || maxWidth < 1)
      return false;
   w = std::min(pw, maxWidth);
   const double hh = (w == pw) ? (double)ph : std::max(1., std::round((double)ph * w / pw));
   if (w > kMaxSide || hh > kMaxSide)
      return false;
   h = (int)hh;
   return true;
}

namespace detail
{

struct Axis // one bilinear sample position along an axis: pixels i0 and i1 with weight f on i1
{
   int i0, i1;
   float f;
};

inline std::vector<Axis> MakeAxis(int dst, int src)
{
   std::vector<Axis> a((size_t)dst);
   for (int i = 0; i < dst; i++)
   {
      double s = ((double)i + 0.5) * src / dst - 0.5;
      s = s < 0. ? 0. : s > (double)(src - 1) ? (double)(src - 1) : s;
      const int i0 = (int)s;
      a[(size_t)i] = { i0, std::min(i0 + 1, src - 1), (float)(s - i0) };
   }
   return a;
}

constexpr double kBoxRatio = 1.5; // a source/destination ratio over this on either axis switches that image from bilinear to box averaging

// Area-weighted box taps for one axis: output pixel i averages the source interval [i*src/dst, (i+1)*src/dst), each source pixel weighted by its overlap,
// weights summing to 1. An axis that upscales or downscales by <= kBoxRatio gets the two bilinear taps in the same form (the other axis may be boxed).
struct Taps
{
   std::vector<int> start, count;
   std::vector<size_t> off; // index of an output pixel's first weight in w
   std::vector<float> w;
};

inline Taps MakeTaps(int dst, int src)
{
   Taps t;
   t.start.resize((size_t)dst), t.count.resize((size_t)dst), t.off.resize((size_t)dst);
   const bool box = (double)src / dst > kBoxRatio;
   const std::vector<Axis> ax = box ? std::vector<Axis>() : MakeAxis(dst, src);
   for (int i = 0; i < dst; i++)
   {
      t.off[(size_t)i] = t.w.size();
      if (!box)
      {
         const Axis& a = ax[(size_t)i];
         t.start[(size_t)i] = a.i0;
         t.count[(size_t)i] = a.i1 > a.i0 ? 2 : 1;
         t.w.push_back(a.i1 > a.i0 ? 1.f - a.f : 1.f);
         if (a.i1 > a.i0)
            t.w.push_back(a.f);
         continue;
      }
      const double s0 = (double)i * src / dst, s1 = (double)(i + 1) * src / dst;
      const int j0 = (int)s0, j1 = std::min(src, (int)std::ceil(s1));
      t.start[(size_t)i] = j0;
      t.count[(size_t)i] = std::max(1, j1 - j0);
      double sum = 0.;
      const size_t first = t.w.size();
      for (int j = j0; j < std::max(j1, j0 + 1); j++)
      {
         const double wt = std::max(0., std::min((double)(j + 1), s1) - std::max((double)j, s0));
         t.w.push_back((float)wt), sum += wt;
      }
      for (size_t k = first; k < t.w.size(); k++)
         t.w[k] = (float)(t.w[k] / sum);
   }
   return t;
}

inline bool Usable(const BakeImage& im)
{
   return im.data && im.w >= 1 && im.h >= 1 && (im.channels == 1 || im.channels == 3 || im.channels == 4);
}

} // namespace detail

// False (out cleared) for an unusable image or parameter, or an output over kMaxSide on a side, or when the buffers cannot be allocated.
inline bool Bake(const BakeImage& paint, const BakeImage& printMask, const BakeParams& prm, BakeResult& out)
{
   using namespace detail;
   out = BakeResult();
   int W, H;
   if (!Usable(paint) || !Usable(printMask) || !OutputSize(printMask.w, printMask.h, prm.maxWidth, W, H))
      return false;
   const int pc = paint.channels, mc = printMask.channels;
   const size_t pstride = paint.stride ? paint.stride : (size_t)paint.w * pc, mstride = printMask.stride ? printMask.stride : (size_t)printMask.w * mc;
   const bool useAlpha = (prm.printMode & kSourceAlpha) != 0, invert = (prm.printMode & kInvert) != 0;

   // The mask's ink at one source pixel, 0..255.
   const auto ink = [&](const uint8_t* q) -> float {
      int v;
      if (useAlpha)
         v = mc == 4 ? q[3] : 255;
      else
         v = mc == 1 ? (int)q[0] : ((int)q[0] + q[1] + q[2]) / 3;
      return (float)(invert ? 255 - v : v);
   };

   // Each image is bilinear unless it is downscaled by more than kBoxRatio on an axis (then box on both axes, upscaling axes keeping their bilinear taps).
   const bool boxP = (double)paint.w / W > kBoxRatio || (double)paint.h / H > kBoxRatio, boxM = (double)printMask.w / W > kBoxRatio || (double)printMask.h / H > kBoxRatio;
   const std::vector<Axis> px = boxP ? std::vector<Axis>() : MakeAxis(W, paint.w), py = boxP ? std::vector<Axis>() : MakeAxis(H, paint.h);
   const std::vector<Axis> mx = boxM ? std::vector<Axis>() : MakeAxis(W, printMask.w), my = boxM ? std::vector<Axis>() : MakeAxis(H, printMask.h);
   const Taps tpx = boxP ? MakeTaps(W, paint.w) : Taps(), tpy = boxP ? MakeTaps(H, paint.h) : Taps();
   const Taps tmx = boxM ? MakeTaps(W, printMask.w) : Taps(), tmy = boxM ? MakeTaps(H, printMask.h) : Taps();

   try
   {
      out.print.assign((size_t)W * H * 4, 255);
      if (prm.makeCaps)
         out.caps.assign((size_t)W * H * 4, 255);
      std::vector<float> rgbRow((size_t)W * 3), inkRow((size_t)W), accP, accM;
      if (boxP)
         accP.resize((size_t)W * 3);
      if (boxM)
         accM.resize((size_t)W);
      out.w = W, out.h = H;
      double inkSum = 0.;
      for (int y = 0; y < H; y++)
      {
         // rgbRow: Paint resampled to output row y (unrounded); inkRow: the same for the ink.
         if (!boxP)
         {
            const uint8_t* p0 = paint.data + (size_t)py[(size_t)y].i0 * pstride;
            const uint8_t* p1 = paint.data + (size_t)py[(size_t)y].i1 * pstride;
            const float fyp = py[(size_t)y].f;
            for (int x = 0; x < W; x++)
            {
               const Axis& a = px[(size_t)x];
               const uint8_t* q00 = p0 + (size_t)a.i0 * pc, *q10 = p0 + (size_t)a.i1 * pc, *q01 = p1 + (size_t)a.i0 * pc, *q11 = p1 + (size_t)a.i1 * pc;
               for (int c = 0; c < 3; c++)
               {
                  const int k = pc == 1 ? 0 : c; // gray: the one channel for r, g and b
                  const float top = q00[k] + (q10[k] - q00[k]) * a.f, bot = q01[k] + (q11[k] - q01[k]) * a.f;
                  rgbRow[(size_t)x * 3 + c] = top + (bot - top) * fyp;
               }
            }
         }
         else
         {
            std::fill(accP.begin(), accP.end(), 0.f);
            const size_t yi = (size_t)y;
            for (int r = 0; r < tpy.count[yi]; r++)
            {
               const float wy = tpy.w[tpy.off[yi] + (size_t)r];
               const uint8_t* row = paint.data + (size_t)(tpy.start[yi] + r) * pstride;
               for (int x = 0; x < W; x++)
               {
                  const size_t xi = (size_t)x;
                  float s[3] = { 0.f, 0.f, 0.f };
                  for (int t = 0; t < tpx.count[xi]; t++)
                  {
                     const uint8_t* q = row + (size_t)(tpx.start[xi] + t) * pc;
                     const float wx = tpx.w[tpx.off[xi] + (size_t)t];
                     for (int c = 0; c < 3; c++)
                        s[c] += wx * q[pc == 1 ? 0 : c];
                  }
                  for (int c = 0; c < 3; c++)
                     accP[xi * 3 + c] += wy * s[c];
               }
            }
            std::copy(accP.begin(), accP.end(), rgbRow.begin());
         }
         if (!boxM)
         {
            const uint8_t* m0 = printMask.data + (size_t)my[(size_t)y].i0 * mstride;
            const uint8_t* m1 = printMask.data + (size_t)my[(size_t)y].i1 * mstride;
            const float fym = my[(size_t)y].f;
            for (int x = 0; x < W; x++)
            {
               const Axis& b = mx[(size_t)x];
               const float i00 = ink(m0 + (size_t)b.i0 * mc), i10 = ink(m0 + (size_t)b.i1 * mc), i01 = ink(m1 + (size_t)b.i0 * mc), i11 = ink(m1 + (size_t)b.i1 * mc);
               const float itop = i00 + (i10 - i00) * b.f, ibot = i01 + (i11 - i01) * b.f;
               inkRow[(size_t)x] = itop + (ibot - itop) * fym;
            }
         }
         else
         {
            std::fill(accM.begin(), accM.end(), 0.f);
            const size_t yi = (size_t)y;
            for (int r = 0; r < tmy.count[yi]; r++)
            {
               const float wy = tmy.w[tmy.off[yi] + (size_t)r];
               const uint8_t* row = printMask.data + (size_t)(tmy.start[yi] + r) * mstride;
               for (int x = 0; x < W; x++)
               {
                  const size_t xi = (size_t)x;
                  float s = 0.f;
                  for (int t = 0; t < tmx.count[xi]; t++)
                     s += tmx.w[tmx.off[xi] + (size_t)t] * ink(row + (size_t)(tmx.start[xi] + t) * mc);
                  accM[xi] += wy * s;
               }
            }
            std::copy(accM.begin(), accM.end(), inkRow.begin());
         }
         uint8_t* o = &out.print[(size_t)y * W * 4];
         uint8_t* oc = prm.makeCaps ? &out.caps[(size_t)y * W * 4] : nullptr;
         for (int x = 0; x < W; x++)
         {
            // the mix of values in 0..255 with weights summing to 1 cannot leave 0..255 by more than float error; clamp so the +0.5 truncation cannot wrap
            uint8_t rgb[3];
            for (int c = 0; c < 3; c++)
               rgb[c] = (uint8_t)std::min(255.f, std::max(0.f, rgbRow[(size_t)x * 3 + c] + 0.5f));
            const int ink8 = (int)std::min(255.f, std::max(0.f, inkRow[(size_t)x] + 0.5f));
            inkSum += ink8;
            o[4 * x] = rgb[0], o[4 * x + 1] = rgb[1], o[4 * x + 2] = rgb[2], o[4 * x + 3] = (uint8_t)(255 - ink8);
            if (oc)
               oc[4 * x] = rgb[0], oc[4 * x + 1] = rgb[1], oc[4 * x + 2] = rgb[2], oc[4 * x + 3] = (uint8_t)ink8;
         }
      }
      out.inkCoverage = inkSum / (255. * W * H);
   }
   catch (const std::bad_alloc&)
   {
      out = BakeResult();
      return false;
   }
   return true;
}

} // namespace PrintBake
