// license:GPLv3+

#include "DynamicLight.h"

#include <algorithm>
#include <cmath>

namespace B2S::DynamicLight
{

namespace
{

inline int ClampInt(int value, int low, int high) { return std::max(low, std::min(high, value)); }
inline double Clamp01(double value) { return std::max(0.0, std::min(1.0, value)); }
inline double SmoothStep(double value)
{
   const double t = Clamp01(value);
   return t * t * (3.0 - 2.0 * t);
}
inline int ClampByte(int value) { return ClampInt(value, 0, 255); }
// VB CInt(Double) and Math.Round(Double) round half to even, which nearbyint does in the default rounding mode
inline int Round(double value) { return static_cast<int>(std::nearbyint(value)); }
inline double Lerp(double a, double b, double amount)
{
   amount = Clamp01(amount);
   return a + (b - a) * amount;
}

// The sliding window mean of the reference (DAL:346-381): the window is clamped to the image and the divisor is the number of pixels in it
std::vector<double> BlurDoubleMap(const std::vector<double>& values, int width, int height, int radius)
{
   std::vector<double> horizontal(values.size());
   std::vector<double> output(values.size());
   for (int y = 0; y < height; y++)
   {
      const int row = y * width;
      double sum = 0.0;
      int left = 0;
      int right = std::min(width - 1, radius);
      for (int x = left; x <= right; x++)
         sum += values[static_cast<size_t>(row + x)];
      for (int x = 0; x < width; x++)
      {
         horizontal[static_cast<size_t>(row + x)] = sum / (right - left + 1);
         const int removeX = x - radius;
         const int addX = x + radius + 1;
         if (removeX >= 0)
         {
            sum -= values[static_cast<size_t>(row + removeX)];
            left++;
         }
         if (addX < width)
         {
            sum += values[static_cast<size_t>(row + addX)];
            right++;
         }
      }
   }
   for (int x = 0; x < width; x++)
   {
      double sum = 0.0;
      int top = 0;
      int bottom = std::min(height - 1, radius);
      for (int y = top; y <= bottom; y++)
         sum += horizontal[static_cast<size_t>(y * width + x)];
      for (int y = 0; y < height; y++)
      {
         output[static_cast<size_t>(y * width + x)] = sum / (bottom - top + 1);
         const int removeY = y - radius;
         const int addY = y + radius + 1;
         if (removeY >= 0)
         {
            sum -= horizontal[static_cast<size_t>(removeY * width + x)];
            top++;
         }
         if (addY < height)
         {
            sum += horizontal[static_cast<size_t>(addY * width + x)];
            bottom++;
         }
      }
   }
   return output;
}

void ApplyTemperature(uint8_t* pixels, int width, int height, int kelvin)
{
   kelvin = ClampInt(kelvin, 2000, 6500);
   if (kelvin == 4000)
      return;
   double rf, gf, bf;
   if (kelvin <= 2700)
   {
      const double t = (kelvin - 2000.0) / 700.0;
      rf = Lerp(1.22, 1.14, t);
      gf = Lerp(0.92, 0.96, t);
      bf = Lerp(0.58, 0.72, t);
   }
   else if (kelvin <= 3000)
   {
      const double t = (kelvin - 2700.0) / 300.0;
      rf = Lerp(1.14, 1.1, t);
      gf = Lerp(0.96, 0.98, t);
      bf = Lerp(0.72, 0.8, t);
   }
   else if (kelvin <= 4000)
   {
      const double t = (kelvin - 3000.0) / 1000.0;
      rf = Lerp(1.1, 1.0, t);
      gf = Lerp(0.98, 1.0, t);
      bf = Lerp(0.8, 1.0, t);
   }
   else if (kelvin <= 5000)
   {
      const double t = (kelvin - 4000.0) / 1000.0;
      rf = Lerp(1.0, 0.96, t);
      gf = Lerp(1.0, 1.01, t);
      bf = Lerp(1.0, 1.1, t);
   }
   else
   {
      const double t = (kelvin - 5000.0) / 1500.0;
      rf = Lerp(0.96, 0.9, t);
      gf = Lerp(1.01, 1.0, t);
      bf = Lerp(1.1, 1.22, t);
   }
   for (size_t i = 0, n = static_cast<size_t>(width) * static_cast<size_t>(height); i < n; i++)
   {
      uint8_t* const p = pixels + i * 4;
      if (p[3] == 0)
         continue;
      const double orr = p[0], og = p[1], ob = p[2];
      const double oldLuma = orr * 0.2126 + og * 0.7152 + ob * 0.0722;
      double nr = orr * rf, ng = og * gf, nb = ob * bf;
      const double newLuma = nr * 0.2126 + ng * 0.7152 + nb * 0.0722;
      if (newLuma > 0.001)
      {
         const double scale = std::max(0.82, std::min(1.18, oldLuma / newLuma));
         nr *= scale;
         ng *= scale;
         nb *= scale;
      }
      const double strength = 0.55 + 0.3 * (oldLuma / 255.0);
      p[2] = static_cast<uint8_t>(ClampByte(Round(ob + (nb - ob) * strength)));
      p[1] = static_cast<uint8_t>(ClampByte(Round(og + (ng - og) * strength)));
      p[0] = static_cast<uint8_t>(ClampByte(Round(orr + (nr - orr) * strength)));
   }
}

// One texel of an image; outside the image GDI+ blends with transparent black (measured on the far edge of an upscaled crop), so there is none
inline const uint8_t* Texel(const uint8_t* image, int imageW, int imageH, int x, int y)
{
   if (x < 0 || y < 0 || x >= imageW || y >= imageH)
      return nullptr;
   return image + (static_cast<size_t>(y) * static_cast<size_t>(imageW) + static_cast<size_t>(x)) * 4;
}

inline void CopyRequantized(const uint8_t* texel, uint8_t* out)
{
   const int a = texel[3];
   if (a == 0)
   {
      out[0] = out[1] = out[2] = out[3] = 0;
      return;
   }
   out[0] = RequantizeChannel(texel[0], a);
   out[1] = RequantizeChannel(texel[1], a);
   out[2] = RequantizeChannel(texel[2], a);
   out[3] = static_cast<uint8_t>(a);
}

// Bilinear sample with texel k at coordinate k (probed GDI+ rule), interpolating the premultiplied colour; an exact texel is copied
void SampleBilinear(const uint8_t* image, int imageW, int imageH, double u, double v, uint8_t* out)
{
   const double fu = std::floor(u), fv = std::floor(v);
   const int x0 = static_cast<int>(fu), y0 = static_cast<int>(fv);
   const double fx = u - fu, fy = v - fv;
   if (fx == 0.0 && fy == 0.0)
   {
      if (const uint8_t* const texel = Texel(image, imageW, imageH, x0, y0); texel != nullptr)
         CopyRequantized(texel, out);
      else
         out[0] = out[1] = out[2] = out[3] = 0;
      return;
   }
   const uint8_t* const t[4] = { Texel(image, imageW, imageH, x0, y0), Texel(image, imageW, imageH, x0 + 1, y0), Texel(image, imageW, imageH, x0, y0 + 1),
      Texel(image, imageW, imageH, x0 + 1, y0 + 1) };
   const double w[4] = { (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy, fx * fy };
   // GDI+ interpolates the premultiplied 8 bit colour of the texels and un-premultiplies the result
   double alpha = 0.0, r = 0.0, g = 0.0, b = 0.0;
   for (int k = 0; k < 4; k++)
   {
      if (t[k] == nullptr)
         continue;
      const int ta = t[k][3];
      alpha += w[k] * ta;
      r += w[k] * ((t[k][0] * ta + 127) / 255);
      g += w[k] * ((t[k][1] * ta + 127) / 255);
      b += w[k] * ((t[k][2] * ta + 127) / 255);
   }
   const int a = ClampByte(static_cast<int>(std::floor(alpha + 0.5)));
   if (a == 0)
   {
      out[0] = out[1] = out[2] = out[3] = 0;
      return;
   }
   // un-premultiply with the rounded alpha, like the 8 bit operation
   const double scale = 255.0 / a;
   out[0] = static_cast<uint8_t>(ClampByte(static_cast<int>(std::floor(r * scale + 0.5))));
   out[1] = static_cast<uint8_t>(ClampByte(static_cast<int>(std::floor(g * scale + 0.5))));
   out[2] = static_cast<uint8_t>(ClampByte(static_cast<int>(std::floor(b * scale + 0.5))));
   out[3] = static_cast<uint8_t>(a);
}

} // namespace

Params ClampParams(Params params)
{
   params.exposure = ClampInt(params.exposure, 0, 1600);
   params.saturation = ClampInt(params.saturation, 0, 200);
   params.protection = ClampInt(params.protection, 0, 100);
   params.lift = ClampInt(params.lift, 0, 100);
   params.contrast = ClampInt(params.contrast, 0, 300);
   params.kelvin = ClampInt(params.kelvin, 2000, 6500);
   return params;
}

uint8_t RequantizeChannel(int channel, int alpha)
{
   if (alpha <= 0)
      return 0;
   const int premultiplied = (channel * alpha + 127) / 255;
   const int value = (premultiplied * ((255 << 16) / alpha)) >> 16;
   return static_cast<uint8_t>(std::min(255, value));
}

void Render(const uint8_t* source, const uint8_t* field, int width, int height, const Params& params, uint8_t* out)
{
   const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
   std::fill(out, out + count * 4, uint8_t(0));

   // Transmission map (DAL:313-344): the luminance of every pixel, its 9 x 9 local mean
   std::vector<double> luminance(count);
   for (size_t i = 0; i < count; i++)
      luminance[i] = std::max(0.0, std::min(1.0, (source[i * 4] * 0.299 + source[i * 4 + 1] * 0.587 + source[i * 4 + 2] * 0.114) / 255.0));
   const std::vector<double> localAverage = BlurDoubleMap(luminance, width, height, 4);
   const double contrast = std::max(0.0, std::min(3.0, params.contrast / 100.0));
   const double detailStrength = 0.45 + contrast * 1.15;
   const double midpointStrength = 0.72 + contrast * 0.28;

   // Per pixel lighting (DAL:383-457)
   const double normalizedIntensity = std::max(0.0, std::min(16.0, params.exposure / 100.0));
   const double exposureStrength = normalizedIntensity * 1.18 + std::pow(std::max(0.0, normalizedIntensity - 3.0), 1.28) * 0.58;
   const double protect = std::max(0.0, std::min(1.0, params.protection / 100.0));
   const double shadowLift = std::max(0.0, std::min(1.0, params.lift / 100.0));
   const double sat = std::max(0.0, std::min(2.0, params.saturation / 100.0));
   for (size_t i = 0; i < count; i++)
   {
      const double f = field[i] / 255.0;
      if (f <= 0.0)
         continue;
      const uint8_t* const s = source + i * 4;
      const double r = s[0], g = s[1], b = s[2];
      const double sourceAlpha = s[3] / 255.0;
      const double lum = (r * 0.299 + g * 0.587 + b * 0.114) / 255.0;
      const double blackProtection = SmoothStep((std::max(r, std::max(g, b)) - 18.0) / 30.0);
      if (blackProtection <= 0.0)
         continue;
      const double transmitted = std::pow(std::max(0.0, luminance[i]), 0.82) * midpointStrength + (luminance[i] - localAverage[i]) * detailStrength;
      const double surfaceDetail = std::max(0.025, std::min(1.0, transmitted));
      const double response = 0.72 + shadowLift * (1.0 - lum) * 0.28;
      const double detailResponse = 0.8 + surfaceDetail * 0.4;
      double localEnergy = exposureStrength * f * response * detailResponse;
      localEnergy *= 1.0 - protect * lum * 0.8;
      const double gain = 1.0 + localEnergy * 0.38;
      double rr = r * (1.0 + (gain - 1.0) * params.colorR / 255.0);
      double gg = g * (1.0 + (gain - 1.0) * params.colorG / 255.0);
      double bb = b * (1.0 + (gain - 1.0) * params.colorB / 255.0);
      const double burstMix = SmoothStep((normalizedIntensity - 8.0) / 8.0) * f * SmoothStep((lum - 0.55) / 0.45);
      rr += (255.0 - rr) * burstMix;
      gg += (255.0 - gg) * burstMix;
      bb += (255.0 - bb) * burstMix;
      const double litLum = rr * 0.299 + gg * 0.587 + bb * 0.114;
      rr = litLum + (rr - litLum) * sat;
      gg = litLum + (gg - litLum) * sat;
      bb = litLum + (bb - litLum) * sat;
      if (params.style == 2)
      {
         rr = params.colorR;
         gg = params.colorG;
         bb = params.colorB;
      }
      uint8_t* const o = out + i * 4;
      o[0] = static_cast<uint8_t>(ClampByte(Round(rr)));
      o[1] = static_cast<uint8_t>(ClampByte(Round(gg)));
      o[2] = static_cast<uint8_t>(ClampByte(Round(bb)));
      o[3] = static_cast<uint8_t>(ClampByte(Round(f * sourceAlpha * blackProtection * 255.0)));
   }
   ApplyTemperature(out, width, height, params.kelvin);
}

void CropField(const uint8_t* field, int fieldW, int fieldH, int lightW, int lightH, int relX, int relY, int outW, int outH, uint8_t* out)
{
   // The source rectangle in field pixels, computed in single precision like the reference's RectangleF
   const float srcX = static_cast<float>(relX * fieldW) / static_cast<float>(lightW);
   const float srcY = static_cast<float>(relY * fieldH) / static_cast<float>(lightH);
   const float srcW = static_cast<float>(outW * fieldW) / static_cast<float>(lightW);
   const float srcH = static_cast<float>(outH * fieldH) / static_cast<float>(lightH);
   const float stepX = srcW / static_cast<float>(outW);
   const float stepY = srcH / static_cast<float>(outH);
   for (int j = 0; j < outH; j++)
      for (int i = 0; i < outW; i++)
      {
         const double u = srcX + i * static_cast<double>(stepX), v = srcY + j * static_cast<double>(stepY);
         const double fu = std::floor(u), fv = std::floor(v);
         const int x0 = static_cast<int>(fu), y0 = static_cast<int>(fv), x1 = x0 + 1, y1 = y0 + 1;
         const double fx = u - fu, fy = v - fv;
         // The texels beyond the far edge of the field are transparent
         const auto at = [&](int x, int y) -> double { return x < 0 || y < 0 || x >= fieldW || y >= fieldH ? 0.0 : field[static_cast<size_t>(y) * fieldW + x]; };
         const double value = (1.0 - fx) * (1.0 - fy) * at(x0, y0) + fx * (1.0 - fy) * at(x1, y0) + (1.0 - fx) * fy * at(x0, y1) + fx * fy * at(x1, y1);
         out[static_cast<size_t>(j) * outW + i] = static_cast<uint8_t>(ClampByte(static_cast<int>(std::floor(value + 0.5))));
      }
}

void StandardFrame(const uint8_t* image, int imageW, int imageH, int rectW, int rectH, int cropX, int cropY, int outW, int outH, uint8_t* out)
{
   const bool oneToOne = imageW == rectW && imageH == rectH;
   const double stepX = static_cast<double>(imageW) / rectW, stepY = static_cast<double>(imageH) / rectH;
   for (int j = 0; j < outH; j++)
      for (int i = 0; i < outW; i++)
      {
         uint8_t* const o = out + (static_cast<size_t>(j) * outW + i) * 4;
         const int x = cropX + i, y = cropY + j;
         if (oneToOne)
         {
            // The reference copies the snippet twice (image to frame, frame to crop) and each copy re-quantises
            CopyRequantized(Texel(image, imageW, imageH, x, y), o);
            CopyRequantized(o, o);
         }
         else
         {
            SampleBilinear(image, imageW, imageH, x * stepX, y * stepY, o);
            CopyRequantized(o, o);
         }
      }
}

void RotatedFrame(const uint8_t* image, int imageW, int imageH, float authoredW, float authoredH, int rectW, int rectH, float pivotX, float pivotY, float angleDeg,
   int cropX, int cropY, int outW, int outH, uint8_t* out)
{
   const double angle = angleDeg * 3.14159265358979323846 / 180.0;
   const double cs = std::cos(angle), sn = std::sin(angle);
   const double sx = rectW / static_cast<double>(authoredW), sy = rectH / static_cast<double>(authoredH);
   const double hingeX = rectW * static_cast<double>(pivotX), hingeY = rectH * static_cast<double>(pivotY);
   const auto corner = [&](double dx, double dy, double& x, double& y)
   {
      x = hingeX + (dx * cs - dy * sn) * sx - cropX;
      y = hingeY + (dx * sn + dy * cs) * sy - cropY;
   };
   double p0x, p0y, p1x, p1y, p2x, p2y;
   corner(-authoredW * static_cast<double>(pivotX), -authoredH * static_cast<double>(pivotY), p0x, p0y);
   corner(authoredW * (1.0 - static_cast<double>(pivotX)), -authoredH * static_cast<double>(pivotY), p1x, p1y);
   corner(-authoredW * static_cast<double>(pivotX), authoredH * (1.0 - static_cast<double>(pivotY)), p2x, p2y);
   // GDI+ takes the three points as single precision floats and maps in single precision; at an angle like 90 degrees the edges of the image fall
   // exactly on pixel coordinates, where the inclusive test depends on that rounding
   const float fp0x = static_cast<float>(p0x), fp0y = static_cast<float>(p0y);
   const float ax = static_cast<float>(p1x) - fp0x, ay = static_cast<float>(p1y) - fp0y, bx = static_cast<float>(p2x) - fp0x, by = static_cast<float>(p2y) - fp0y;
   const float det = ax * by - ay * bx;
   std::fill(out, out + static_cast<size_t>(outW) * outH * 4, uint8_t(0));
   if (std::fabs(det) < 1e-9f)
      return;
   for (int j = 0; j < outH; j++)
      for (int i = 0; i < outW; i++)
      {
         // The integer pixel coordinate (not the centre) and an inclusive test, as measured against GDI+ HighSpeed
         const float dx = static_cast<float>(i) - fp0x, dy = static_cast<float>(j) - fp0y;
         const float u = ((dx * by - dy * bx) / det) * static_cast<float>(imageW);
         const float v = ((ax * dy - ay * dx) / det) * static_cast<float>(imageH);
         if (u < 0.f || u > static_cast<float>(imageW) || v < 0.f || v > static_cast<float>(imageH))
            continue;
         // A sample exactly on an edge of the parallelogram (quarter turns put them on integer coordinates) belongs to it only on a left or a top
         // edge, as measured against GDI+: the outward normal of the edge points left, or straight up
         const auto owned = [&](float nx, float ny) { return nx < 0.f || (nx == 0.f && ny < 0.f); };
         if ((u == 0.f && !owned(-by / det, bx / det)) || (u == static_cast<float>(imageW) && !owned(by / det, -bx / det))
            || (v == 0.f && !owned(ay / det, -ax / det)) || (v == static_cast<float>(imageH) && !owned(-ay / det, ax / det)))
            continue;
         SampleBilinear(image, imageW, imageH, u, v, out + (static_cast<size_t>(j) * outW + i) * 4);
      }
}

} // namespace B2S::DynamicLight
