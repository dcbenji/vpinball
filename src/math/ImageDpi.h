// license:GPLv3+

#pragma once

#include <cstddef>
#include <cstdint>

namespace ImageDpi
{

// The resolution an image file states, in pixels per inch: PNG pHYs, JPEG JFIF density or EXIF resolution (x direction). 0 when the file says nothing
// usable, or only a screen default (72 and 96 count as no information), or something outside 10..4800.
float FromFile(const uint8_t *data, size_t size);

}
