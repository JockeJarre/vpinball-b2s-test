// license:GPLv3+

#include <algorithm>
#include <cmath>

#include "Reel3D.h"

namespace B2S::Reel3D
{

namespace
{

inline uint8_t ClampByte(double v) { return static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(std::nearbyint(v))))); }

// Catmull-Rom (a = -0.5)
inline double CatmullRom(double t)
{
   t = std::fabs(t);
   if (t <= 1.0)
      return 1.5 * t * t * t - 2.5 * t * t + 1.0;
   if (t < 2.0)
      return -0.5 * t * t * t + 2.5 * t * t - 4.0 * t + 2.0;
   return 0.0;
}

struct Tap
{
   int index;
   double weight;
};

// Weights of the source pixels covered by the destination pixel centre at source coordinate pos (source pixel centres at i + 0.5)
void Taps(std::vector<Tap>& taps, double pos, double scale, int size)
{
   taps.clear();
   const double widen = std::max(1.0, scale);
   const double radius = 2.0 * widen;
   const int first = static_cast<int>(std::floor(pos - 0.5 - radius));
   const int last = static_cast<int>(std::ceil(pos - 0.5 + radius));
   double sum = 0.0;
   for (int i = first; i <= last; i++)
   {
      const double w = CatmullRom((static_cast<double>(i) + 0.5 - pos) / widen);
      if (w == 0.0)
         continue;
      taps.push_back({ std::max(0, std::min(size - 1, i)), w });
      sum += w;
   }
   if (sum != 0.0)
      for (Tap& tap : taps)
         tap.weight /= sum;
}

// Straight alpha SourceOver of a colour onto a pixel
void SourceOver(uint8_t* dst, int r, int g, int b, int a)
{
   if (a <= 0)
      return;
   const double sa = a / 255.0;
   const double da = dst[3] / 255.0;
   const double oa = sa + da * (1.0 - sa);
   if (oa <= 0.0)
      return;
   dst[0] = ClampByte((r * sa + dst[0] * da * (1.0 - sa)) / oa);
   dst[1] = ClampByte((g * sa + dst[1] * da * (1.0 - sa)) / oa);
   dst[2] = ClampByte((b * sa + dst[2] * da * (1.0 - sa)) / oa);
   dst[3] = ClampByte(oa * 255.0);
}

// Non antialiased wide line with butt caps (pixel centres on integer coordinates like GDI+), blended with SourceOver
void WideLine(Image& img, double x0, double y0, double x1, double y1, double width, int r, int g, int b, int a)
{
   const double dx = x1 - x0, dy = y1 - y0;
   const double len2 = dx * dx + dy * dy;
   if (len2 <= 0.0 || a <= 0)
      return;
   const double half = width * 0.5;
   const int minX = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - half - 1)));
   const int maxX = std::min(img.w - 1, static_cast<int>(std::ceil(std::max(x0, x1) + half + 1)));
   const int minY = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - half - 1)));
   const int maxY = std::min(img.h - 1, static_cast<int>(std::ceil(std::max(y0, y1) + half + 1)));
   const double len = std::sqrt(len2);
   for (int y = minY; y <= maxY; y++)
      for (int x = minX; x <= maxX; x++)
      {
         const double t = ((x - x0) * dx + (y - y0) * dy) / len2;
         if (t <= 0.0 || t > 1.0)
            continue;
         const double dist = std::fabs((x - x0) * dy - (y - y0) * dx) / len;
         if (dist <= half)
            SourceOver(img.Px(x, y), r, g, b, a);
      }
}

void TemperatureFactors(int kelvin, double& rf, double& gf, double& bf)
{
   const auto lerp = [](double a, double c, double t) { return a + (c - a) * t; };
   const auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
   if (kelvin <= 2700)
   {
      const double t = clamp01((kelvin - 2000.0) / 700.0);
      rf = lerp(1.22, 1.14, t);
      gf = lerp(0.92, 0.96, t);
      bf = lerp(0.58, 0.72, t);
   }
   else if (kelvin <= 3000)
   {
      const double t = clamp01((kelvin - 2700.0) / 300.0);
      rf = lerp(1.14, 1.10, t);
      gf = lerp(0.96, 0.98, t);
      bf = lerp(0.72, 0.80, t);
   }
   else if (kelvin <= 4000)
   {
      const double t = clamp01((kelvin - 3000.0) / 1000.0);
      rf = lerp(1.10, 1.00, t);
      gf = lerp(0.98, 1.00, t);
      bf = lerp(0.80, 1.00, t);
   }
   else if (kelvin <= 5000)
   {
      const double t = clamp01((kelvin - 4000.0) / 1000.0);
      rf = lerp(1.00, 0.96, t);
      gf = lerp(1.00, 1.01, t);
      bf = lerp(1.00, 1.10, t);
   }
   else
   {
      const double t = clamp01((kelvin - 5000.0) / 1500.0);
      rf = lerp(0.96, 0.90, t);
      gf = lerp(1.01, 1.00, t);
      bf = lerp(1.10, 1.22, t);
   }
}

}

void DrawResampled(Image& dst, const Image& src, double x, double y, double w, double h)
{
   if (dst.Empty() || src.Empty() || w <= 0.0 || h <= 0.0)
      return;
   const double scaleX = src.w / w, scaleY = src.h / h;
   const int x0 = std::max(0, static_cast<int>(std::ceil(x - 0.5)));
   const int x1 = std::min(dst.w - 1, static_cast<int>(std::ceil(x + w - 0.5)) - 1);
   const int y0 = std::max(0, static_cast<int>(std::ceil(y - 0.5)));
   const int y1 = std::min(dst.h - 1, static_cast<int>(std::ceil(y + h - 0.5)) - 1);
   std::vector<std::vector<Tap>> columns(static_cast<size_t>(std::max(0, x1 - x0 + 1)));
   for (int dx = x0; dx <= x1; dx++)
      Taps(columns[static_cast<size_t>(dx - x0)], (dx + 0.5 - x) * scaleX, scaleX, src.w);
   std::vector<Tap> rows;
   for (int dy = y0; dy <= y1; dy++)
   {
      Taps(rows, (dy + 0.5 - y) * scaleY, scaleY, src.h);
      for (int dx = x0; dx <= x1; dx++)
      {
         const std::vector<Tap>& cols = columns[static_cast<size_t>(dx - x0)];
         double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
         for (const Tap& ty : rows)
            for (const Tap& tx : cols)
            {
               const uint8_t* p = src.Px(tx.index, ty.index);
               const double wgt = tx.weight * ty.weight;
               const double wa = wgt * p[3];
               r += wa * p[0];
               g += wa * p[1];
               b += wa * p[2];
               a += wa;
            }
         uint8_t* out = dst.Px(dx, dy);
         if (a <= 1e-9)
         {
            out[0] = out[1] = out[2] = out[3] = 0;
            continue;
         }
         out[0] = ClampByte(r / a);
         out[1] = ClampByte(g / a);
         out[2] = ClampByte(b / a);
         out[3] = ClampByte(a);
      }
   }
}

Image Resample(const Image& src, int w, int h)
{
   if (src.Empty() || w <= 0 || h <= 0)
      return {};
   Image out(w, h);
   DrawResampled(out, src, 0.0, 0.0, w, h);
   return out;
}

Image FitToBox(const Image& src, int boxW, int boxH, int digitWidth)
{
   if (src.Empty() || boxW <= 0 || boxH <= 0)
      return {};
   Image out(boxW, boxH);
   const uint8_t* bar = src.Px(0, 0);
   for (int y = 0; y < boxH; y++)
      for (int x = 0; x < boxW; x++)
         std::copy(bar, bar + 4, out.Px(x, y));
   if (digitWidth > 0)
   {
      const int w = std::max(1, std::min(boxW, digitWidth));
      DrawResampled(out, src, (boxW - w) / 2.0, 0.0, w, boxH);
   }
   else
   {
      const double scale = std::min(static_cast<double>(boxW) / src.w, static_cast<double>(boxH) / src.h);
      const int cw = std::max(1, static_cast<int>(std::nearbyint(src.w * scale)));
      const int ch = std::max(1, static_cast<int>(std::nearbyint(src.h * scale)));
      DrawResampled(out, src, (boxW - cw) / 2.0, (boxH - ch) / 2.0, cw, ch);
   }
   return out;
}

Image RenderReel(const Image& source, int w, int h, int brightness, int temperature, int depth)
{
   if (source.Empty() || w <= 0 || h <= 0)
      return {};
   brightness = std::max(0, std::min(400, brightness));
   temperature = std::max(2000, std::min(6500, temperature));
   depth = std::max(0, std::min(200, depth));

   Image img = (source.w == w && source.h == h) ? source : Resample(source, w, h);
   // Fully transparent pixels carry no colour (GDI+ SourceCopy turns them into 0,0,0,0)
   for (size_t i = 0; i + 3 < img.rgba.size(); i += 4)
      if (img.rgba[i + 3] == 0)
         img.rgba[i] = img.rgba[i + 1] = img.rgba[i + 2] = 0;

   // Drum shade per row, back light per pixel (reference Reel3DEffect.vb:40-67)
   const double depthAmount = depth / 100.0;
   const double lightAmount = brightness / 100.0;
   for (int y = 0; y < h; y++)
   {
      const double normalY = h <= 1 ? 0.0 : 2.0 * y / (h - 1.0) - 1.0;
      const double roundness = std::sqrt(std::max(0.0, 1.0 - normalY * normalY));
      const double drumShade = std::max(0.18, 1.0 - depthAmount * 0.5 * (1.0 - roundness));
      for (int x = 0; x < w; x++)
      {
         uint8_t* p = img.Px(x, y);
         const double lum = (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0;
         const double material = std::pow(std::max(0.0, std::min(1.0, lum)), 1.35);
         const double normalX = w <= 1 ? 0.0 : 2.0 * x / (w - 1.0) - 1.0;
         const double bulbFalloff = 0.68 + 0.32 * std::max(0.0, 1.0 - std::sqrt(normalX * normalX * 0.38 + normalY * normalY * 0.72));
         const double addedLight = material * bulbFalloff * lightAmount * 0.54;
         for (int c = 0; c < 3; c++)
            p[c] = ClampByte(p[c] * drumShade * 0.67 + 255.0 * addedLight);
      }
   }

   // Side shadows (reference Reel3DEffect.vb:75-85)
   const int sideWidth = std::min(std::max(1, w / 3), 12 + static_cast<int>(std::nearbyint(8.0 * depthAmount)));
   for (int i = 0; i < sideWidth; i++)
   {
      const int alpha = std::max(0, std::min(255, static_cast<int>(std::nearbyint(105.0 * depthAmount * (1.0 - i / static_cast<double>(sideWidth))))));
      if (alpha <= 0)
         continue;
      for (int y = 0; y < h; y++)
      {
         SourceOver(img.Px(i, y), 0, 0, 0, alpha);
         SourceOver(img.Px(w - 1 - i, y), 0, 0, 0, alpha);
      }
   }

   // Colour temperature grade, luminance preserving (reference Reel3DEffect.vb:147-216)
   if (temperature != 4000)
   {
      double rf, gf, bf;
      TemperatureFactors(temperature, rf, gf, bf);
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w; x++)
         {
            uint8_t* p = img.Px(x, y);
            if (p[3] == 0)
               continue;
            const double r = p[0], g = p[1], b = p[2];
            const double oldLuma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            double gr = r * rf, gg = g * gf, gb = b * bf;
            const double newLuma = 0.2126 * gr + 0.7152 * gg + 0.0722 * gb;
            if (newLuma > 0.001)
            {
               const double lumaScale = std::max(0.82, std::min(1.18, oldLuma / newLuma));
               gr *= lumaScale;
               gg *= lumaScale;
               gb *= lumaScale;
            }
            const double strength = 0.55 + 0.3 * (oldLuma / 255.0);
            p[0] = ClampByte(r + (gr - r) * strength);
            p[1] = ClampByte(g + (gg - g) * strength);
            p[2] = ClampByte(b + (gb - b) * strength);
         }
   }
   return img;
}

Image CreateWindowOverlay(int w, int h, int depth, int glass)
{
   if (w <= 2 || h <= 2)
      return {};
   depth = std::max(0, std::min(200, depth));
   glass = std::max(0, std::min(200, glass));
   Image img(w, h);
   const double depthAmount = depth / 100.0;

   // Depth rings (reference Reel3DEffect.vb:105-118)
   int insetWidth = 18 + static_cast<int>(std::nearbyint(7.0 * depthAmount));
   insetWidth = std::min(insetWidth, std::max(1, std::min(w, h) / 2 - 1));
   for (int i = 0; i < insetWidth; i++)
   {
      const int alpha = std::max(0, std::min(255, static_cast<int>(std::nearbyint(150.0 * depthAmount * (1.0 - i / static_cast<double>(insetWidth))))));
      if (alpha <= 0)
         continue;
      const int rw = std::max(1, w - 1 - 2 * i), rh = std::max(1, h - 1 - 2 * i);
      for (int x = i; x <= i + rw && x < w; x++)
      {
         SourceOver(img.Px(x, i), 0, 0, 0, alpha);
         if (i + rh != i)
            SourceOver(img.Px(x, std::min(h - 1, i + rh)), 0, 0, 0, alpha);
      }
      for (int y = i + 1; y < i + rh && y < h; y++)
      {
         SourceOver(img.Px(i, y), 0, 0, 0, alpha);
         SourceOver(img.Px(std::min(w - 1, i + rw), y), 0, 0, 0, alpha);
      }
   }

   // Glass reflection (reference Reel3DEffect.vb:120-145)
   const int reflectionAlpha = std::max(0, std::min(255, static_cast<int>(std::nearbyint(150.0 * glass / 100.0))));
   if (reflectionAlpha <= 0)
      return img;
   const int shineX = 16, shineY = 8, shineW = std::max(2, w - 32), shineH = std::max(16, h / 3);
   for (int y = shineY; y < shineY + shineH && y < h; y++)
   {
      const int alpha = std::max(0, std::min(255, static_cast<int>(std::nearbyint(reflectionAlpha * (1.0 - static_cast<double>(y - shineY) / shineH)))));
      for (int x = shineX; x < shineX + shineW && x < w; x++)
         SourceOver(img.Px(x, y), 235, 248, 255, alpha);
   }
   WideLine(img, 35.0, 19.0, w - 85.0, 7.0, 2.5, 255, 255, 255, reflectionAlpha);
   WideLine(img, w / 3, 10.0, w / 2, h / 3, 5.0, 210, 238, 255, reflectionAlpha / 2);
   return img;
}

}
