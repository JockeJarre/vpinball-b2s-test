// license:GPLv3+

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// B2S Pro reel display image processing, free of any VPX dependency: digit image sizing (KeepDigitAspect and
// ReelDigitWidth, reference src/shared/ReelImageSizing.vb) and the 3D reel effect (reference Classes/Reel3DEffect.vb).
// All images are straight alpha RGBA, 8 bits per channel, top row first.
namespace B2S::Reel3D
{

struct Image
{
   int w = 0;
   int h = 0;
   std::vector<uint8_t> rgba;

   bool Empty() const { return w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4; }
   Image() = default;
   Image(int width, int height) : w(width), h(height), rgba(static_cast<size_t>(std::max(0, width)) * static_cast<size_t>(std::max(0, height)) * 4, 0) {}
   uint8_t* Px(int x, int y) { return rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4; }
   const uint8_t* Px(int x, int y) const { return rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4; }
};

// Draws src stretched into the destination rectangle (destination pixels whose centre lies inside it are replaced) with a
// Catmull-Rom resampler whose kernel is widened by the downscale factor, which was the closest to GDI+ high quality bicubic
void DrawResampled(Image& dst, const Image& src, double x, double y, double w, double h);

// The image stretched to the given size over a transparent bitmap (GDI+ SourceCopy)
Image Resample(const Image& src, int w, int h);

// ReelImageSizing.Fit (digitWidth 0: the whole image letterboxed, centred) or StretchToWidth (digitWidth > 0: full height, centred
// width). The bars take the colour of the source pixel (0,0)
Image FitToBox(const Image& src, int boxW, int boxH, int digitWidth);

// Reel3DEffect.RenderReel: the image resampled to the box (when it is not that size already), drum shading, back light, side
// shadows and colour temperature grade. brightness 0..400, temperature 2000..6500 K, depth 0..200 (clamped like the reference)
Image RenderReel(const Image& source, int w, int h, int brightness, int temperature, int depth);

// Reel3DEffect.CreateWindowOverlay: depth rings and glass reflection of a whole display, cropped per digit by the caller.
// depth 0..200, glass 0..200. Empty when the size is 2 pixels or less
Image CreateWindowOverlay(int w, int h, int depth, int glass);

}
