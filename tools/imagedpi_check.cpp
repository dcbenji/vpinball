// Prints the resolution each file states: clang++ -std=c++20 -I src tools/imagedpi_check.cpp src/math/ImageDpi.cpp -o /tmp/idc && /tmp/idc a.png b.jpg ...
#include "math/ImageDpi.h"
#include <cstdio>
#include <vector>
int main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++)
   {
      FILE *f = fopen(argv[i], "rb");
      if (!f) { printf("%s: cannot open\n", argv[i]); continue; }
      std::vector<uint8_t> d;
      uint8_t buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
      fclose(f);
      printf("%s: %.3f\n", argv[i], ImageDpi::FromFile(d.data(), d.size()));
   }
}
