// license:GPLv3+

#include "ImageDpi.h"

#include <cmath>

namespace ImageDpi
{

namespace
{
uint32_t Be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Be16(const uint8_t *p) { return (uint32_t(p[0]) << 8) | p[1]; }

// 72 and 96 are what screens and most software write when nobody set a resolution
float Informative(const double dpi)
{
   if (!(dpi >= 10.0 && dpi <= 4800.0) || fabs(dpi - 72.0) < 0.6 || fabs(dpi - 96.0) < 0.6)
      return 0.f;
   return static_cast<float>(dpi);
}

float Png(const uint8_t *d, const size_t n)
{
   size_t pos = 8;
   while (pos + 12 <= n)
   {
      const uint32_t len = Be32(d + pos);
      const uint8_t *const type = d + pos + 4;
      if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T')
         break; // pHYs comes before the image data
      if (type[0] == 'p' && type[1] == 'H' && type[2] == 'Y' && type[3] == 's' && len >= 9 && pos + 8 + 9 <= n)
      {
         const uint8_t *const p = d + pos + 8;
         return p[8] == 1 ? Informative(Be32(p) * 0.0254) : 0.f; // pixels per metre
      }
      if (len > n || pos + 12 + len > n)
         break;
      pos += 12 + static_cast<size_t>(len);
   }
   return 0.f;
}

// EXIF: a TIFF structure. XResolution (0x011A) is a rational, ResolutionUnit (0x0128) 2 = inch (the default), 3 = cm
float Exif(const uint8_t *t, const size_t n)
{
   if (n < 8)
      return 0.f;
   const bool le = t[0] == 'I' && t[1] == 'I';
   if (!le && !(t[0] == 'M' && t[1] == 'M'))
      return 0.f;
   const auto u16 = [&](const size_t o) { return le ? uint32_t(t[o]) | (uint32_t(t[o + 1]) << 8) : Be16(t + o); };
   const auto u32 = [&](const size_t o) { return le ? uint32_t(t[o]) | (uint32_t(t[o + 1]) << 8) | (uint32_t(t[o + 2]) << 16) | (uint32_t(t[o + 3]) << 24) : Be32(t + o); };
   const size_t ifd = u32(4);
   if (ifd + 2 > n)
      return 0.f;
   const uint32_t count = u16(ifd);
   double x = 0.0;
   uint32_t unit = 2;
   for (uint32_t i = 0; i < count; i++)
   {
      const size_t e = ifd + 2 + 12 * static_cast<size_t>(i);
      if (e + 12 > n)
         break;
      const uint32_t tag = u16(e);
      if (tag == 0x011A)
      {
         const size_t off = u32(e + 8);
         if (off + 8 <= n && u32(off + 4) != 0)
            x = static_cast<double>(u32(off)) / u32(off + 4);
      }
      else if (tag == 0x0128)
         unit = u16(e + 8);
   }
   return unit == 2 ? Informative(x) : unit == 3 ? Informative(x * 2.54) : 0.f;
}

float Jpeg(const uint8_t *d, const size_t n)
{
   size_t pos = 2;
   while (pos + 4 <= n)
   {
      if (d[pos] != 0xFF)
         break;
      const uint8_t marker = d[pos + 1];
      if (marker == 0xFF)
      {
         pos++;
         continue;
      }
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
      {
         pos += 2;
         continue;
      }
      if (marker == 0xDA || marker == 0xD9) // the headers are over
         break;
      const size_t len = Be16(d + pos + 2);
      if (len < 2 || pos + 2 + len > n)
         break;
      const uint8_t *const p = d + pos + 4;
      const size_t plen = len - 2;
      if (marker == 0xE0 && plen >= 12 && p[0] == 'J' && p[1] == 'F' && p[2] == 'I' && p[3] == 'F' && p[4] == 0)
      {
         const uint32_t units = p[7], xd = Be16(p + 8);
         if (const float v = units == 1 ? Informative(xd) : units == 2 ? Informative(xd * 2.54) : 0.f; v > 0.f)
            return v;
      }
      else if (marker == 0xE1 && plen >= 14 && p[0] == 'E' && p[1] == 'x' && p[2] == 'i' && p[3] == 'f' && p[4] == 0 && p[5] == 0)
      {
         if (const float v = Exif(p + 6, plen - 6); v > 0.f)
            return v;
      }
      pos += 2 + len;
   }
   return 0.f;
}
}

float FromFile(const uint8_t *data, const size_t size)
{
   if (data == nullptr || size < 12)
      return 0.f;
   static constexpr uint8_t kPng[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
   bool png = true;
   for (int i = 0; i < 8; i++)
      png &= data[i] == kPng[i];
   if (png)
      return Png(data, size);
   if (data[0] == 0xFF && data[1] == 0xD8)
      return Jpeg(data, size);
   return 0.f;
}

}
