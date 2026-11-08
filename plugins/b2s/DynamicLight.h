// license:GPLv3+
//
// B2S Pro Dynamic Front Lighting (DynamicFront* / DynamicLight* attributes of a light bulb), free of any VPX dependency: the lighting
// kernel of the reference (src/server/.../Classes/DynamicArtworkLighting.vb, DAL), the light field crop and the "live pose" snippet
// frames of formBackglass.vb (fB:955-1058). Everything works on straight alpha RGBA bytes, row 0 on top, in authored backglass pixels.

#pragma once

#include <cstdint>
#include <vector>

namespace B2S::DynamicLight
{

// The static parameters of a light (reference fB:3360-3367, clamped by the caller's ClampParams)
struct Params
{
   int exposure = 100; // percent, 0..1600
   int colorR = 255;
   int colorG = 255;
   int colorB = 255;
   int style = 1; // 2 = solid light colour, anything else lights the artwork
   int saturation = 100; // 0..200
   int protection = 0; // highlight protection, 0..100
   int lift = 0; // dark area lift, 0..100
   int contrast = 140; // transmission contrast, 0..300
   int kelvin = 4000; // 2000..6500
};

// The clamps the reference applies when it loads a light
Params ClampParams(Params params);

// DynamicArtworkLighting.Render: source and out are width * height * 4 bytes (R, G, B, A; the RGB of a pixel with alpha 0 must be 0), field is
// width * height alpha bytes. out gets the straight alpha overlay (reference DAL:44-77, 313-502)
void Render(const uint8_t* source, const uint8_t* field, int width, int height, const Params& params, uint8_t* out);

// The part of the light field under the overlap of light and snippet (fB:955-965): the whole field is stretched over the light rectangle
// (lightW x lightH) and the overlap, whose top left is (relX, relY) inside that rectangle, is sampled bilinearly into outW x outH bytes
void CropField(const uint8_t* field, int fieldW, int fieldH, int lightW, int lightH, int relX, int relY, int outW, int outH, uint8_t* out);

// A snippet drawn stretched to its rounded rectangle (rectW x rectH) and cut at (cropX, cropY) into outW x outH RGBA bytes
// (fB:995-1011, 1034-1058). image is imageW x imageH straight RGBA
void StandardFrame(const uint8_t* image, int imageW, int imageH, int rectW, int rectH, int cropX, int cropY, int outW, int outH, uint8_t* out);

// A pivot or natively rotated snippet at its current angle, drawn through three corner points (fB:1013-1031, 4417-4442): authoredW x authoredH is
// the snippet's authored size, (rectW, rectH) its rounded rectangle, (pivotX, pivotY) the hinge as fractions, angleDeg positive clockwise
void RotatedFrame(const uint8_t* image, int imageW, int imageH, float authoredW, float authoredH, int rectW, int rectH, float pivotX, float pivotY,
   float angleDeg, int cropX, int cropY, int outW, int outH, uint8_t* out);

// What GDI+ makes of a straight alpha texel copied 1:1 (premultiplied to 8 bit, then un-premultiplied; probed over all 65,536 pairs)
uint8_t RequantizeChannel(int channel, int alpha);

// Source over: the overlay (straight alpha) over a destination colour byte, in gamma space (reference DrawImageUnscaled)
inline uint8_t BlendChannel(int source, int sourceAlpha, int destination)
{
   return static_cast<uint8_t>((source * sourceAlpha + destination * (255 - sourceAlpha) + 127) / 255);
}

} // namespace B2S::DynamicLight
