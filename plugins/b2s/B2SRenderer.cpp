// license:GPLv3+

#include "common.h"

#include "B2SRenderer.h"
#include "B2SServer.h"

#include "pinmame/PinMAMEPlugin.h"

#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace B2S {

// sRGB to linear conversion (duplicate from the main VPX codebase, see utils/color.h)
static inline float InvsRGB(const float x) { return (x <= 0.04045f) ? (x * (float)(1.0 / 12.92)) : (powf(x * (float)(1.0 / 1.055) + (float)(0.055 / 1.055), 2.4f)); }

MSGPI_BOOL_VAL_SETTING(showGrillProp, "ShowGrill", "Show Grill", "Show Grill", true, false);
MSGPI_BOOL_VAL_SETTING(upgradedSegRenderingProp, "UpgradedSegmentRendering", "Upgraded segment rendering",
   "Render LED score displays with the upgraded segment renderer (turn off for the original B2S segment rendering)", true, true);
// The setting id is new on purpose: the switch this setting was before is stored as a bool under
// "KeepAspectRatio", and reading that 0/1 back as a percentage would silently give "off" (0 %) or
// "1 %" to everybody who used the switch.
MSGPI_INT_VAL_SETTING(keepAspectRatioProp, "KeepAspectRatioPercent", "Keep aspect ratio (%)",
   "Fit the backglass and score view images in their window at their authored aspect ratio, leaving the "
   "leftover area black: 100 keeps the aspect ratio fully (the image is never distorted, the black area is "
   "at its widest), 0 turns it off and stretches the images over the whole window, in between the padding "
   "and the leftover distortion are applied that part of the way (50: half the black area, half the "
   "distortion)", true, 0, 100, 100);
MSGPI_BOOL_VAL_SETTING(centerVerticallyProp, "CenterVertically", "Center vertically",
   "Center the images vertically in the black area left by 'Keep aspect ratio' (turn off to put them on the floor)", true, true);

static const char* dualModeValues[] = { "Authentic", "Fantasy" };
MSGPI_ENUM_VAL_SETTING(dualModeProp, "DualMode", "Dual Mode", "Active mode for dual backglasses (authentic or fantasy)", true, 1, 2, dualModeValues, 1);

B2SRenderer::B2SRenderer(const MsgPluginAPI* const msgApi, const VPXPluginAPI* const vpxApi, const unsigned int endpointId, std::shared_ptr<B2STable> b2s)
   : m_b2s(b2s)
   , m_msgApi(msgApi)
   , m_vpxApi(vpxApi)
   , m_endpointId(endpointId)
   , m_resURIResolver(*msgApi, endpointId, true, false, false)
   , m_pinmameControllers(
        msgApi, endpointId, CTLPI_CONTROLLERS_GET_MSG, CTLPI_CONTROLLERS_ON_CHG_MSG,
        [](std::vector<ControllerDef>& items)
        {
           const string pinmamePrefix(PMPI_GAMEID_PREFIX);
           std::erase_if(items, [&pinmamePrefix](const ControllerDef& src) { return !string(src.gameId).starts_with(pinmamePrefix); });
        },
        nullptr,
        [this]()
        {
           m_stateSources.Refresh();
           m_segSources.Refresh();
        })
   , m_segSources(
        msgApi, endpointId, CTLPI_SEG_GET_SRC_MSG, CTLPI_SEG_ON_SRC_CHG_MSG,
        [this](std::vector<SegSrcId>& items)
        {
           m_pinmameControllers.With(
              [&items](const std::vector<ControllerDef>& controllers)
              {
                 std::erase_if(items,
                    [&controllers](const SegSrcId& source)
                    {
                       return std::find_if(controllers.begin(), controllers.end(), [source](const ControllerDef& ctrl) { return ctrl.endpointId == source.id.endpointId; })
                          == controllers.end();
                    });
              });
        },
        nullptr, nullptr)
   , m_stateSources(
        msgApi, endpointId, CTLPI_STATE_GET_SRC_MSG, CTLPI_STATE_ON_SRC_CHG_MSG,
        [this](std::vector<StateSrcId>& items)
        {
           m_pinmameControllers.With(
              [&items](const std::vector<ControllerDef>& controllers)
              {
                 std::erase_if(items,
                    [&controllers](const StateSrcId& source)
                    {
                       return std::find_if(controllers.begin(), controllers.end(), [source](const ControllerDef& ctrl) { return ctrl.endpointId == source.id.endpointId; })
                          == controllers.end();
                    });
              });
        },
        [this]() { m_stateSources.With([this](const std::vector<StateSrcId>& items) { OnStateSrcChanged({}); }); },
        [this]() { m_stateSources.With([this](const std::vector<StateSrcId>& items) { OnStateSrcChanged(items); }); })
{
   BuildMotion(); // Before the subscriptions: they report the ROM sources at once
   BuildPhysics();

   const VPXTextureInfo* dmdTexInfo = nullptr;
   if (m_b2s->m_dmdImage.m_image)
      dmdTexInfo = GetTextureInfo(m_b2s->m_dmdImage.m_image);
   m_dmdWidth = dmdTexInfo ? static_cast<float>(dmdTexInfo->width) : 1024.f;
   m_dmdHeight = dmdTexInfo ? static_cast<float>(dmdTexInfo->height) : 768.f;

   m_segSources.Subscribe();
   m_stateSources.Subscribe();
   m_pinmameControllers.Subscribe();
}

B2SRenderer::~B2SRenderer()
{
   m_segSources.Unsubscribe();
   m_stateSources.Unsubscribe();
   m_pinmameControllers.Unsubscribe();
   DeleteTexture(m_canvasBackingTex);
   for (auto& [key, texture] : m_reelBoxTextures)
      DeleteTexture(texture);
   for (auto& [key, texture] : m_reelOverlayTextures)
      DeleteTexture(texture);
   for (auto& [bulb, frame] : m_physicsFrames)
      DeleteTexture(frame.texture);
   for (auto& [key, overlay] : m_dynamicOverlays)
      DeleteTexture(overlay.texture);
}

// Decoded RGBA pixels of a digit image (the VPX texture keeps them as sRGB8 or sRGBA8), null when the format is not supported
const Reel3D::Image* B2SRenderer::GetReelSource(VPXTexture texture)
{
   if (const auto it = m_reelSources.find(texture); it != m_reelSources.end())
      return it->second.Empty() ? nullptr : &it->second;
   Reel3D::Image& image = m_reelSources[texture];
   const VPXTextureInfo* const info = GetTextureInfo(texture);
   if (info == nullptr || info->data == nullptr || info->width == 0 || info->height == 0 || (info->format != VPXTEXFMT_sRGB8 && info->format != VPXTEXFMT_sRGBA8))
      return nullptr;
   const int bpp = info->format == VPXTEXFMT_sRGB8 ? 3 : 4;
   image = Reel3D::Image(static_cast<int>(info->width), static_cast<int>(info->height));
   const uint8_t* const src = static_cast<const uint8_t*>(info->data);
   for (size_t i = 0, n = static_cast<size_t>(image.w) * static_cast<size_t>(image.h); i < n; i++)
   {
      image.rgba[i * 4 + 0] = src[i * bpp + 0];
      image.rgba[i * 4 + 1] = src[i * bpp + 1];
      image.rgba[i * 4 + 2] = src[i * bpp + 2];
      image.rgba[i * 4 + 3] = bpp == 4 ? src[i * bpp + 3] : 255;
   }
   return &image;
}

// The box sized texture of a digit image: letterboxed when KeepDigitAspect is set, then with the 3D effect when enabled. A reel
// with a trigger is back lit only while illuminated, an unlit one has brightness 0 and no temperature grade (BRB:122-127)
VPXTexture B2SRenderer::GetReelBoxTexture(VPXTexture source, int boxW, int boxH, const B2SScore& reel, bool lit)
{
   const ReelTextureKey key { source, boxW, boxH, reel.m_keepDigitAspect ? reel.m_reelDigitWidth : 0, reel.m_keepDigitAspect, reel.m_reel3DEnabled,
      reel.m_reel3DEnabled ? (lit ? reel.m_reel3DBrightness : 0) : 0, reel.m_reel3DEnabled ? (lit ? reel.m_reel3DTemperature : 4000) : 0,
      reel.m_reel3DEnabled ? reel.m_reel3DDepth : 0 };
   if (const auto it = m_reelBoxTextures.find(key); it != m_reelBoxTextures.end())
      return it->second;
   VPXTexture texture = nullptr;
   if (const Reel3D::Image* const src = GetReelSource(source))
   {
      Reel3D::Image box = reel.m_keepDigitAspect ? Reel3D::FitToBox(*src, boxW, boxH, reel.m_reelDigitWidth) : Reel3D::Image();
      if (reel.m_reel3DEnabled)
         box = Reel3D::RenderReel(box.Empty() ? *src : box, boxW, boxH, key.brightness, key.temperature, key.depth);
      else if (box.Empty())
         box = Reel3D::Resample(*src, boxW, boxH);
      if (!box.Empty())
         UpdateTexture(&texture, box.w, box.h, VPXTEXFMT_sRGBA8, box.rgba.data());
   }
   m_reelBoxTextures[key] = texture;
   return texture;
}

VPXTexture B2SRenderer::GetReelOverlayTexture(const B2SScore& reel, int overlayW, int overlayH)
{
   const OverlayKey key { overlayW, overlayH, reel.m_reel3DDepth, reel.m_reel3DGlass };
   if (const auto it = m_reelOverlayTextures.find(key); it != m_reelOverlayTextures.end())
      return it->second;
   VPXTexture texture = nullptr;
   const Reel3D::Image overlay = Reel3D::CreateWindowOverlay(overlayW, overlayH, key.depth, key.glass);
   if (!overlay.Empty())
      UpdateTexture(&texture, overlay.w, overlay.h, VPXTEXFMT_sRGBA8, overlay.rgba.data());
   m_reelOverlayTextures[key] = texture;
   return texture;
}

void B2SRenderer::RegisterSettings(const MsgPluginAPI* const msgApi, unsigned int endpointId)
{
   msgApi->RegisterSetting(endpointId, &showGrillProp);
   msgApi->RegisterSetting(endpointId, &dualModeProp);
   msgApi->RegisterSetting(endpointId, &upgradedSegRenderingProp);
   msgApi->RegisterSetting(endpointId, &keepAspectRatioProp);
   msgApi->RegisterSetting(endpointId, &centerVerticallyProp);
}

namespace
{
   int MSGPIAPI ShowGrillGet(void*) { return showGrillProp_Get(); }
   void MSGPIAPI ShowGrillSet(void*, int v) { showGrillProp_Set(v); }
   int MSGPIAPI DualModeGet(void*) { return dualModeProp_Get(); }
   void MSGPIAPI DualModeSet(void*, int v) { dualModeProp_Set(v); }
   int MSGPIAPI UpgradedSegGet(void*) { return upgradedSegRenderingProp_Get(); }
   void MSGPIAPI UpgradedSegSet(void*, int v) { upgradedSegRenderingProp_Set(v); }
   int MSGPIAPI KeepAspectRatioGet(void*) { return keepAspectRatioProp_Get(); }
   void MSGPIAPI KeepAspectRatioSet(void*, int v) { keepAspectRatioProp_Set(std::max(0, std::min(100, v))); }
   int MSGPIAPI CenterVerticallyGet(void*) { return centerVerticallyProp_Get(); }
   void MSGPIAPI CenterVerticallySet(void*, int v) { centerVerticallyProp_Set(v); }
}

int B2SRenderer::FillSettingsItems(PagedUIItemDef* items)
{
   items[0] = {};
   items[0].type = PAGEDUI_ITEM_BOOL;
   items[0].id = "ShowGrill";
   items[0].label = "Show Grill";
   items[0].tooltip = "Show Grill";
   items[0].storage = PAGEDUI_STORAGE_HOST;
   items[0].boolProp = { 0, &ShowGrillGet, &ShowGrillSet };
   items[1] = {};
   items[1].type = PAGEDUI_ITEM_ENUM;
   items[1].id = "DualMode";
   items[1].label = "Dual Mode";
   items[1].tooltip = "Active mode for dual backglasses (authentic or fantasy)";
   items[1].storage = PAGEDUI_STORAGE_HOST;
   items[1].enumProp = { 1, 2, dualModeValues, 1, &DualModeGet, &DualModeSet };
   items[2] = {};
   items[2].type = PAGEDUI_ITEM_BOOL;
   items[2].id = "UpgradedSegmentRendering";
   items[2].label = "Upgraded segment rendering";
   items[2].tooltip = "Render LED score displays with the upgraded segment renderer (turn off for the original B2S segment rendering)";
   items[2].storage = PAGEDUI_STORAGE_HOST;
   items[2].boolProp = { 1, &UpgradedSegGet, &UpgradedSegSet };
   items[3] = {};
   items[3].type = PAGEDUI_ITEM_INT;
   items[3].id = "KeepAspectRatioPercent";
   items[3].label = "Keep aspect ratio";
   items[3].tooltip = "Fit the backglass and score view images in their window at their authored aspect ratio, leaving the "
      "leftover area black: 100 keeps the aspect ratio fully (the image is never distorted, the black area is at its widest), "
      "0 turns it off and stretches the images over the whole window, in between the black area and the leftover distortion "
      "are applied that part of the way (50: half the black area, half the distortion)";
   items[3].storage = PAGEDUI_STORAGE_HOST;
   items[3].intProp = { 0, 100, 100, "%d %%", &KeepAspectRatioGet, &KeepAspectRatioSet };
   items[4] = {};
   items[4].type = PAGEDUI_ITEM_BOOL;
   items[4].id = "CenterVertically";
   items[4].label = "Center vertically";
   items[4].tooltip = "Center the images vertically in the black area left by 'Keep aspect ratio' (turn off to put them on the floor). At 0 the black area is empty, at 50 its half width goes above the image";
   items[4].storage = PAGEDUI_STORAGE_HOST;
   items[4].boolProp = { 1, &CenterVerticallyGet, &CenterVerticallySet };
   return 5;
}

// KeepAspectRatio rendering: the host stretches the source surface (the fictitious screen the plugin draws
// on) over the whole window, each axis on its own. The surface is therefore padded to the window shape, so
// that both axes scale by the very same factor: the authored content keeps its shape (a round bulb stays
// round, the image is not stretched) and the padding stays black. Nothing else has to be known by the
// drawing code, it keeps working in the image authored units.
//
// The setting is a percentage of this fit: the padding the full fit needs is applied that part of the way,
// which takes both axes scale factors from the window/image ratio to 1 and the black area from nothing to
// its full width, the two ends met at 0 (plain stretch) and 100 (aspect ratio fully kept).
void B2SRenderer::FitSurfaceToWindow(VPXRenderContext2D* ctx, const float contentW, const float contentH)
{
   m_padW = m_padH = 0.f;
   const int keep = keepAspectRatioProp_Get();
   if (keep <= 0 || contentW <= 0.f || contentH <= 0.f || ctx->outWidth <= 0.f || ctx->outHeight <= 0.f)
   {
      ctx->srcWidth = contentW;
      ctx->srcHeight = contentH;
      return;
   }

   const float wndAspect = ctx->outWidth / ctx->outHeight;
   const bool wideContent = (contentW / contentH > wndAspect);
   const float fitW = wideContent ? contentW : contentH * wndAspect;
   const float fitH = wideContent ? contentW / wndAspect : contentH;
   const float part = (keep >= 100) ? 1.f : (0.01f * static_cast<float>(keep));
   ctx->srcWidth = m_padW = contentW + part * (fitW - contentW);
   ctx->srcHeight = m_padH = contentH + part * (fitH - contentH);
   m_contentW = contentW;
   m_contentH = contentH;
   // Vertical placement of the content in the surface: centered, or stuck on the floor. The content is drawn
   // from the top of the surface, so the placement is the black area taken above it
   m_layoutOY = centerVerticallyProp_Get() ? 0.5f * (m_padH - contentH) : m_padH - contentH;
   // Horizontal placement is centered, there is nothing to choose: the image is centered on its axis of
   // authoring, the only case where a horizontal black area shows is a window wider than the image
   m_layoutOX = 0.5f * (m_padW - contentW);
}

// The padding is painted over once the frame is drawn: animations, illuminations and score displays are
// authored on the image and may spill in the area kept for the borders
void B2SRenderer::RenderAspectPadding(VPXRenderContext2D* ctx)
{
   const float left = m_layoutOX;
   const float top = m_layoutOY;
   const float bottom = m_padH - m_layoutOY - m_contentH;
   const float right = m_padW - m_layoutOX - m_contentW;
   if (left <= 0.f && right <= 0.f && top <= 0.f && bottom <= 0.f)
      return;

   if (m_blackTexture == nullptr)
   {
      static const uint8_t black[4] = { 0, 0, 0, 255 };
      m_vpxApi->UpdateTexture(&m_blackTexture, 1, 1, VPXTextureFormat::VPXTEXFMT_sRGBA8, black);
      if (m_blackTexture == nullptr)
         return; // Texture upload failed, keep the padding as it is
   }

   const auto drawBar = [this, ctx](const float x, const float y, const float w, const float h)
   {
      ctx->DrawImage(ctx, m_blackTexture, 0.f, 0.f, 0.f, 1.f, // Opaque black
         0.f, 0.f, 1.f, 1.f, // The 1x1 texture, fully sampled
         0.f, 0.f, 0.f, // No pivot, no rotation
         x, y, w, h);
   };

   if (top > 0.f)
      drawBar(0.f, 0.f, ctx->srcWidth, top);
   if (bottom > 0.f)
      drawBar(0.f, m_layoutOY + m_contentH, ctx->srcWidth, bottom);
   if (left > 0.f)
      drawBar(0.f, m_layoutOY, left, m_contentH);
   if (right > 0.f)
      drawBar(m_layoutOX + m_contentW, m_layoutOY, right, m_contentH);
}

void B2SRenderer::OnStateSrcChanged(const std::vector<StateSrcId>& items)
{
   if (m_b2s->m_backglassOnImage.m_image)
      m_b2s->m_backglassOnImage.m_romUpdater = []() { /* No ROM source */ };
   for (auto* bulbs : { &m_b2s->m_backglassIlluminations, &m_b2s->m_dmdIlluminations })
      for (auto& bulb : *bulbs)
         bulb->m_romUpdater = []() { /* No ROM source */ };


   // The reference forces TopRomInverted to False for the BackglassOnImage channel
   if (m_b2s->m_backglassOnImage.m_image)
      m_b2s->m_backglassOnImage.m_romUpdater
         = ResolveRomPropUpdater(items, &m_b2s->m_backglassOnImage.m_romProbe, m_b2s->m_backglassOnImage.m_romIdType, m_b2s->m_backglassOnImage.m_romId, false);

   // The reference only defines a matching value (B2SValue as RomIDValue) on B2SID bulbs; ROM events and
   // script writes share the same lamp channel there, so the value filter applies to both paths
   for (auto* bulbs : { &m_b2s->m_backglassIlluminations, &m_b2s->m_dmdIlluminations })
      for (auto& bulb : *bulbs)
      {
         // B2S Pro: a pivot has no ROM visibility binding; its ROM trigger (solenoid, lamp or switch) drives its swing
         // (reference BD:405-422, numbering 1 solenoid, 2 lamp, 5 switch)
         if (bulb->m_pivotAnimation)
         {
            bulb->m_pivotUpdater = []() { /* No ROM source */ };
            if (bulb->m_pivotTriggerId > 0)
               switch (bulb->m_pivotTriggerType)
               {
               case 1: bulb->m_pivotUpdater = ResolveRomPropUpdater(items, &bulb->m_pivotProbe, B2SRomIDType::Solenoid, bulb->m_pivotTriggerId); break;
               case 2: bulb->m_pivotUpdater = ResolveRomPropUpdater(items, &bulb->m_pivotProbe, B2SRomIDType::Lamp, bulb->m_pivotTriggerId); break;
               case 5: bulb->m_pivotUpdater = ResolveRomPropUpdater(items, &bulb->m_pivotProbe, B2SRomIDType::Switch, bulb->m_pivotTriggerId); break;
               default: break;
               }
            continue;
         }
         // B2S Pro: a native rotation bulb is started and stopped by its trigger; the channel value is read raw, the
         // inversion is applied to the on/off decision (reference SV:949-966, fB:123-131). Mech ids never reach it
         if (bulb->m_nativeRotation)
         {
            bulb->m_nativeUpdater = []() { /* No ROM source */ };
            if (bulb->m_nativeTriggerId > 0 && bulb->m_nativeTriggerType != B2SRomIDType::Mech)
               bulb->m_nativeUpdater = ResolveRomPropUpdater(items, &bulb->m_nativeProbe, bulb->m_nativeTriggerType, bulb->m_nativeTriggerId);
            continue;
         }
         const int romValue = bulb->m_b2sId >= 0 ? bulb->m_b2sValue : 0;
         // B2S Pro: the reference shows/hides rotating images on a switch trigger instead of rotating them.
         // Not ported yet: they keep no ROM source
         if (bulb->m_romIdType == B2SRomIDType::Switch && bulb->m_snippitType != B2SSnippitType::StandardImage)
            continue;
         switch (bulb->m_snippitType)
         {
         case B2SSnippitType::StandardImage: bulb->m_romUpdater = ResolveRomPropUpdater(items, &bulb->m_romProbe, bulb->m_romIdType, bulb->m_romId, bulb->m_romInverted, romValue, true); break;
         case B2SSnippitType::MechRotatingImage: bulb->m_romUpdater = ResolveRomPropUpdater(items, &bulb->m_mechProbe, bulb->m_romIdType, bulb->m_romId); break;
         case B2SSnippitType::SelfRotatingImage: bulb->m_romUpdater = ResolveRomPropUpdater(items, &bulb->m_romOn, bulb->m_romIdType, bulb->m_romId, bulb->m_romInverted, romValue); break;
         }
      }

   // Reel illumination lamps (ReelIlluB2SID is always a lamp in the reference implementation)
   for (auto* scores : { &m_b2s->m_backglassScores, &m_b2s->m_dmdScores })
      for (auto& score : scores->m_scores)
         score.m_reelIlluUpdater = score.m_reelIlluB2SID > 0 ? ResolveRomPropUpdater(items, &score.m_reelIllu, B2SRomIDType::Lamp, score.m_reelIlluB2SID) : []() { };

   // Rebind the ROM event triggers of all animations (IDJoin)
   const auto resolver = [this, &items](B2SRomIDType romIdType, int romId, bool inverted, float* target) { return ResolveRomPropUpdater(items, target, romIdType, romId, inverted); };
   for (auto& animation : m_b2s->m_backglassAnimations)
      animation.BindRomTriggers(resolver);
   for (auto& animation : m_b2s->m_dmdAnimations)
      animation.BindRomTriggers(resolver);

   // B2S Pro motion paths: one probe per solenoid, lamp or switch that a bulb starts, stops, resumes or removes on. The raw state is
   // read, without inversion or a minimum on time (reference SV:788-822, fB:123-169)
   {
      std::lock_guard lock(m_motionMutex);
      m_motionProbes.clear();
      if (m_motion)
      {
         for (const auto& [source, id] : m_motion->RomSources())
            m_motionProbes.push_back({ source, id });
         for (MotionProbe& probe : m_motionProbes) // The storage no longer moves: the updaters keep pointers into it
         {
            const B2SRomIDType type = probe.source == Motion::Source::Solenoid ? B2SRomIDType::Solenoid : probe.source == Motion::Source::Lamp ? B2SRomIDType::Lamp : B2SRomIDType::Switch;
            probe.updater = ResolveRomPropUpdater(items, &probe.value, type, probe.id);
         }
      }
   }

   // B2S Pro physics launchers: a solenoid or lamp reporting on, or a switch closing, fires the launcher of the ball (B2S ids come with
   // the script writes). The raw state is read, like the reference (SV:805, SV:1044, fB:132)
   {
      std::lock_guard lock(m_motionMutex);
      m_physicsProbes.clear();
      if (m_physics)
      {
         for (const auto& [source, id] : m_physics->LauncherSources())
            if (source != Motion::Source::B2S)
               m_physicsProbes.push_back({ source, id });
         for (PhysicsProbe& probe : m_physicsProbes)
         {
            const B2SRomIDType type = probe.source == Motion::Source::Solenoid ? B2SRomIDType::Solenoid : probe.source == Motion::Source::Lamp ? B2SRomIDType::Lamp : B2SRomIDType::Switch;
            probe.updater = ResolveRomPropUpdater(items, &probe.value, type, probe.id);
         }
      }
   }
}

// B2S Pro motion paths: every bulb with a route or in a sequence group becomes an item of one system shared by both windows
void B2SRenderer::BuildMotion()
{
   m_motion = std::make_unique<Motion::System>([this]() -> uint64_t { return m_motionServer ? m_motionServer->NextStamp() : ++m_motionLocalStamp; });
   for (const bool dmd : { false, true })
      for (auto& bulb : dmd ? m_b2s->m_dmdIlluminations : m_b2s->m_backglassIlluminations)
      {
         bulb->m_motionIndex = -1;
         if (!bulb->m_motion.HasMotion())
            continue;
         // The rectangle the route moves is the one the bulb is drawn at (alpha cropped for old files)
         const Motion::Rect authored { static_cast<double>(bulb->m_pivotDstX), static_cast<double>(bulb->m_pivotDstY), static_cast<double>(bulb->m_pivotDstW),
            static_cast<double>(bulb->m_pivotDstH) };
         bulb->m_motionIndex = m_motion->AddItem(bulb->m_motion, "PictureBox" + std::to_string(bulb->m_bulbId), dmd, authored);
         m_motionBulbs.push_back(bulb.get());
      }
   m_motion->Build();
}

double B2SRenderer::MotionNowMs() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_motionEpoch).count(); }

// Once per render of a window: its scale (output pixels per authored pixel) is what the bulbs of that window measure their routes
// with, the ROM triggers are polled (a solenoid or lamp reporting on, a switch closing, the first observation counting), and the
// movers advance
void B2SRenderer::UpdateMotion(VPXRenderContext2D* ctx, bool dmd)
{
   if (!m_motion || m_motion->Size() == 0)
      return;
   m_motion->SetScale(dmd, ctx->outWidth > 0.f && ctx->srcWidth > 0.f ? ctx->outWidth / ctx->srcWidth : 1.f, ctx->outHeight > 0.f && ctx->srcHeight > 0.f ? ctx->outHeight / ctx->srcHeight : 1.f);
   const double now = MotionNowMs();
   {
      std::lock_guard lock(m_motionMutex);
      for (MotionProbe& probe : m_motionProbes)
      {
         const float previous = probe.value;
         probe.updater();
         if (probe.value > 0.f && !(previous > 0.f))
            m_motion->OnRomOn(probe.source, probe.id, now);
      }
   }
   m_motion->Tick(now);
}

std::function<void()> B2SRenderer::ResolveRomPropUpdater(const std::vector<StateSrcId>& items, float* value, const B2SRomIDType romIdType, const int romId, const bool romInverted, const int romValue, const bool holdSwitchOn) const
{
   int groupId;
   switch (romIdType)
   {
   case B2SRomIDType::Solenoid: groupId = PMPI_GROUP_SOLENOID; break;
   case B2SRomIDType::GIString: groupId = PMPI_GROUP_GI; break;
   case B2SRomIDType::Lamp: groupId = PMPI_GROUP_LAMP; break;
   case B2SRomIDType::Mech: groupId = PMPI_GROUP_MECH; break;
   case B2SRomIDType::Switch: groupId = PMPI_GROUP_SWITCH; break;
   default: return []() { /* No ROM source */ };
   }
   for (const StateSrcId& src : items)
   {
      if (src.id.resId != groupId)
         continue;
      for (unsigned int i = 0; i < src.nStates; i++)
      {
         const auto& def = src.stateDefs[i];
         if (def.mappingId == romId && def.dataFormat == CTLPI_STATE_FORMAT_FLOAT && def.GetState)
         {
            return [this, value, def, romInverted, romValue]()
            {
               m_stateSources.With([this, value, &def](const std::vector<StateSrcId>&) { def.GetState(def.callContext, value); });
               if (romValue > 0)
                  *value = (static_cast<int>(*value) == romValue) ? 1.f : 0.f;
               else if (romInverted)
                  *value = 1.f - *value;
            };
         }
         // B2S Pro: switches are on/off states. Like the reference, a bulb lit by a switch keeps the closed
         // state for at least 150 ms after the switch closes, so that short pulses stay visible
         if (romIdType == B2SRomIDType::Switch && def.mappingId == romId && def.dataFormat == CTLPI_STATE_FORMAT_UINT8 && def.GetState)
         {
            return [this, value, def, romInverted, holdSwitchOn, wasClosed = false, holdUntil = std::chrono::steady_clock::time_point()]() mutable
            {
               uint8_t state = 0;
               m_stateSources.With([&state, &def](const std::vector<StateSrcId>&) { def.GetState(def.callContext, &state); });
               bool closed = state != 0;
               const auto now = std::chrono::steady_clock::now();
               if (closed && !wasClosed)
                  holdUntil = now + std::chrono::milliseconds(150);
               wasClosed = closed;
               if (!closed && holdSwitchOn && now < holdUntil)
                  closed = true;
               *value = (closed != romInverted) ? 1.f : 0.f;
            };
         }
      }
   }
   return []() { /* No ROM source */ };
}

bool B2SRenderer::Render(VPXRenderContext2D* ctx, B2SServer* server)
{
   switch (ctx->window)
   {
   case VPXWindowId::VPXWINDOW_Backglass: return RenderBackglass(ctx, server);
   case VPXWindowId::VPXWINDOW_ScoreView: return RenderScoreView(ctx, server);
   default: return false;
   }
}

B2SDualMode B2SRenderer::ActiveDualMode() const { return m_b2s->m_dualBackglass ? static_cast<B2SDualMode>(dualModeProp_Get()) : B2SDualMode::Both; }

// B2S Pro physics balls (reference BD:336-390, 465-528): first the pivots a ball can name as its flipper, then the balls. Only a pivot
// with a registered trigger is a flipper (BD:405-422), so the named command pivots and the external pivots cannot be
void B2SRenderer::BuildPhysics()
{
   m_physics = std::make_unique<Physics::System>();
   for (const bool dmd : { false, true })
      for (auto& bulb : dmd ? m_b2s->m_dmdIlluminations : m_b2s->m_backglassIlluminations)
      {
         bulb->m_physicsPivot = -1;
         if (!bulb->m_pivotAnimation || bulb->m_name.empty() || !(bulb->m_pivotTriggerType == 4 || (bulb->m_pivotTriggerType != 0 && bulb->m_pivotTriggerId > 0)))
            continue;
         Physics::PivotParams p;
         p.name = bulb->m_name;
         p.rect = { static_cast<float>(bulb->m_pivotDstX), static_cast<float>(bulb->m_pivotDstY), static_cast<float>(bulb->m_pivotDstW), static_cast<float>(bulb->m_pivotDstH) };
         p.pivotX = bulb->m_pivotX;
         p.pivotY = bulb->m_pivotY;
         p.tipX = bulb->m_pivotTipX;
         p.tipY = bulb->m_pivotTipY;
         p.randomStrength = bulb->m_pivotRandomStrength;
         p.automatic = bulb->m_pivotAutomaticOscillation;
         p.upAngle = bulb->m_pivotUpAngle;
         p.downAngle = bulb->m_pivotDownAngle;
         p.moveDuration = bulb->m_pivotDuration;
         p.initialAngle = bulb->m_pivotAngle;
         bulb->m_physicsPivot = m_physics->AddPivot(p);
         m_physicsPivots.push_back(bulb.get());
      }
   for (const bool dmd : { false, true })
      for (auto& bulb : dmd ? m_b2s->m_dmdIlluminations : m_b2s->m_backglassIlluminations)
      {
         bulb->m_physicsIndex = -1;
         // A ball that is also a motion path member keeps its motion path: the link between them (gravity drop) is not ported
         if (!bulb->m_physics.ball || bulb->m_motion.HasMotion())
            continue;
         if (bulb->m_physics.Simulated())
            bulb->m_physicsIndex = m_physics->AddBall(bulb->m_physics,
               { static_cast<float>(bulb->m_pivotDstX), static_cast<float>(bulb->m_pivotDstY), static_cast<float>(bulb->m_pivotDstW), static_cast<float>(bulb->m_pivotDstH) }, dmd);
         else
            bulb->m_physicsIndex = -2; // Drawn with the mask and aspect rules at its authored position, not simulated (fB:3482)
      }
   m_physics->Build();
   m_physics->SetPulseSwitch(
      [this](int switchId)
      {
         if (m_pulseServer != nullptr)
            m_pulseServer->HoldSwitch(switchId);
      });
}

double B2SRenderer::PhysicsNowSeconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_motionEpoch).count(); }

// Once per render of a window: its scale (balls in that window wait for it), the launcher triggers of the ROM, the clock, and the
// angles of the flippers, which the physics clock moves (reference PB:637-653)
void B2SRenderer::UpdatePhysics(VPXRenderContext2D* ctx, bool dmd)
{
   if (!m_physics || (m_physics->BallCount() == 0 && m_physics->PivotCount() == 0))
      return;
   m_physics->SetScale(dmd, ctx->outWidth > 0.f && ctx->srcWidth > 0.f ? ctx->outWidth / ctx->srcWidth : 1.f, ctx->outHeight > 0.f && ctx->srcHeight > 0.f ? ctx->outHeight / ctx->srcHeight : 1.f);
   const double now = PhysicsNowSeconds();
   {
      std::lock_guard lock(m_motionMutex);
      for (PhysicsProbe& probe : m_physicsProbes)
      {
         const float previous = probe.value;
         probe.updater();
         if (probe.value > 0.f && !(previous > 0.f))
            m_physics->FireLaunchers(probe.source, probe.id, now);
      }
   }
   m_physics->Advance(now);
   for (size_t i = 0; i < m_physicsPivots.size(); i++)
      if (m_physics->IsPhysicsPivot(static_cast<int>(i)))
         m_physicsPivots[i]->m_pivotAngle = m_physics->PivotAngle(static_cast<int>(i));
}

// The ball artwork, drawn into its authored aspect (scaled by the smaller axis scale, so it stays round) about the physics centre, rolled,
// and multiplied by the selection mask, which is stationary in backglass space: the ball passes behind what the mask clears (reference
// CreatePhysicsMaskedFrame, fB:1060-1112, BallRollDestinationPoints, fB:1285-1304). It is composed at authored resolution (the window
// scales it) with 2x2 supersampling, and drawn 1:1 at its bounding box
void B2SRenderer::RenderPhysicsBall(VPXRenderContext2D* ctx, const B2SServer* server, B2SBulb& bulb)
{
   // Shown from load; a later write on the bulb's group hides or shows it
   float state = 0.f;
   uint64_t writeStamp = 0;
   if (server && server->GetBulbState(bulb, state, nullptr, &writeStamp) && state == 0.f)
      return;
   if (bulb.m_image == nullptr || bulb.m_pivotDstW <= 0 || bulb.m_pivotDstH <= 0 || ctx->outWidth <= 0.f || ctx->srcWidth <= 0.f || ctx->outHeight <= 0.f || ctx->srcHeight <= 0.f)
      return;
   PhysicsBallFrame& frame = m_physicsFrames[&bulb];
   if (frame.source.empty())
   {
      const VPXTextureInfo* const info = GetTextureInfo(bulb.m_image);
      if (info == nullptr || info->data == nullptr || info->width == 0 || info->height == 0 || (info->format != VPXTEXFMT_sRGBA8 && info->format != VPXTEXFMT_sRGB8))
         return;
      frame.sourceW = static_cast<int>(info->width);
      frame.sourceH = static_cast<int>(info->height);
      const size_t count = static_cast<size_t>(frame.sourceW) * frame.sourceH;
      frame.source.resize(count * 4);
      const uint8_t* const data = static_cast<const uint8_t*>(info->data);
      for (size_t i = 0; i < count; i++)
      {
         if (info->format == VPXTEXFMT_sRGBA8)
            std::copy(data + i * 4, data + i * 4 + 4, frame.source.begin() + static_cast<ptrdiff_t>(i * 4));
         else
         {
            std::copy(data + i * 3, data + i * 3 + 3, frame.source.begin() + static_cast<ptrdiff_t>(i * 4));
            frame.source[i * 4 + 3] = 255;
         }
      }
   }

   const float sx = ctx->outWidth / ctx->srcWidth, sy = ctx->outHeight / ctx->srcHeight;
   const float rectW = static_cast<float>(bulb.m_pivotDstW), rectH = static_cast<float>(bulb.m_pivotDstH);
   float cx = static_cast<float>(bulb.m_pivotDstX) + rectW * 0.5f, cy = static_cast<float>(bulb.m_pivotDstY) + rectH * 0.5f, roll = 0.f;
   if (bulb.m_physicsIndex >= 0)
   {
      const Physics::System::View view = m_physics->GetView(bulb.m_physicsIndex);
      cx = view.center.x;
      cy = view.center.y;
      roll = view.roll;
   }
   const auto same = [](float a, float b) { return std::abs(a - b) < 0.0005f; };
   if (!frame.valid || !same(frame.cx, cx) || !same(frame.cy, cy) || !same(frame.roll, roll) || !same(frame.sx, sx) || !same(frame.sy, sy))
   {
      // The artwork keeps its aspect and takes the smaller axis scale; a gravity drop ball keeps its area instead (PB:105-125)
      const float s = bulb.m_physics.gravityDrop ? std::sqrt(sx * sy) : std::min(sx, sy);
      const float wA = rectW * s / sx, hA = rectH * s / sy;
      const double a = static_cast<double>(roll) * 3.14159265358979323846 / 180.0;
      const float cs = static_cast<float>(std::cos(a)), sn = static_cast<float>(std::sin(a));
      const float ex = wA * 0.5f * (std::abs(cs) + std::abs(sn)), ey = hA * 0.5f * (std::abs(cs) + std::abs(sn));
      const int left = static_cast<int>(std::floor(cx - ex)), top = static_cast<int>(std::floor(cy - ey));
      const int w = std::clamp(static_cast<int>(std::ceil(cx + ex)) - left, 1, 2048), h = std::clamp(static_cast<int>(std::ceil(cy + ey)) - top, 1, 2048);
      frame.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
      const uint8_t* const src = frame.source.data();
      const int tw = frame.sourceW, th = frame.sourceH;
      // Bilinear sample of the premultiplied artwork at texture coordinates in [0, size]; returns premultiplied RGB and alpha
      const auto sample = [&](float u, float v, float out[4])
      {
         const float fx = std::clamp(u - 0.5f, 0.f, static_cast<float>(tw - 1)), fy = std::clamp(v - 0.5f, 0.f, static_cast<float>(th - 1));
         const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy), x1 = std::min(x0 + 1, tw - 1), y1 = std::min(y0 + 1, th - 1);
         const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
         out[0] = out[1] = out[2] = out[3] = 0.f;
         const int xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
         const float ws[4] = { (1.f - tx) * (1.f - ty), tx * (1.f - ty), (1.f - tx) * ty, tx * ty };
         for (int k = 0; k < 4; k++)
         {
            const uint8_t* const p = src + (static_cast<size_t>(ys[k]) * tw + xs[k]) * 4;
            const float alpha = static_cast<float>(p[3]) / 255.f;
            out[0] += ws[k] * alpha * static_cast<float>(p[0]);
            out[1] += ws[k] * alpha * static_cast<float>(p[1]);
            out[2] += ws[k] * alpha * static_cast<float>(p[2]);
            out[3] += ws[k] * alpha;
         }
      };
      const bool masked = !bulb.m_physicsMask.empty();
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w; x++)
         {
            float acc[4] = { 0.f, 0.f, 0.f, 0.f };
            for (int k = 0; k < 4; k++)
            {
               // The inverse of the rolled, scaled unit square: window pixel -> unit coordinates -> artwork
               const float px = static_cast<float>(left + x) + (k & 1 ? 0.75f : 0.25f), py = static_cast<float>(top + y) + (k & 2 ? 0.75f : 0.25f);
               const float qx = (px - cx) / wA, qy = (py - cy) / hA;
               const float ux = qx * cs + qy * sn, uy = -qx * sn + qy * cs;
               if (ux < -0.5f || ux > 0.5f || uy < -0.5f || uy > 0.5f)
                  continue;
               float t[4];
               sample((ux + 0.5f) * static_cast<float>(tw), (uy + 0.5f) * static_cast<float>(th), t);
               acc[0] += t[0];
               acc[1] += t[1];
               acc[2] += t[2];
               acc[3] += t[3];
            }
            if (acc[3] <= 0.f)
               continue;
            float alpha = acc[3] / 4.f;
            if (masked)
            {
               // The mask is indexed by backglass pixel; outside it the ball is not seen
               const int mx = left + x, my = top + y;
               const int m = (mx >= 0 && my >= 0 && mx < bulb.m_physicsMaskW && my < bulb.m_physicsMaskH) ? bulb.m_physicsMask[static_cast<size_t>(my) * bulb.m_physicsMaskW + mx] : 0;
               alpha = alpha * static_cast<float>(m) / 255.f;
            }
            uint8_t* const o = &frame.pixels[(static_cast<size_t>(y) * w + x) * 4];
            o[0] = static_cast<uint8_t>(std::clamp(acc[0] / acc[3] + 0.5f, 0.f, 255.f));
            o[1] = static_cast<uint8_t>(std::clamp(acc[1] / acc[3] + 0.5f, 0.f, 255.f));
            o[2] = static_cast<uint8_t>(std::clamp(acc[2] / acc[3] + 0.5f, 0.f, 255.f));
            o[3] = static_cast<uint8_t>(std::clamp(alpha * 255.f + 0.5f, 0.f, 255.f));
         }
      UpdateTexture(&frame.texture, w, h, VPXTEXFMT_sRGBA8, frame.pixels.data());
      frame.left = left;
      frame.top = top;
      frame.w = w;
      frame.h = h;
      frame.cx = cx;
      frame.cy = cy;
      frame.roll = roll;
      frame.sx = sx;
      frame.sy = sy;
      frame.valid = true;
   }
   if (frame.texture != nullptr)
      ctx->DrawImage(ctx, frame.texture, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, static_cast<float>(frame.w), static_cast<float>(frame.h), 0.f, 0.f, 0.f, ContentX(static_cast<float>(frame.left)), ContentY(static_cast<float>(frame.top)), static_cast<float>(frame.w),
         static_cast<float>(frame.h));
}

void B2SRenderer::UpdateAnimations(vector<B2SAnimation>& animations, float elapsed, B2SServer* server, B2SDualMode dualMode)
{
   B2SAnimationEffects fx;
   if (server)
      fx = server->GetAnimationEffects();
   fx.dualMode = dualMode;
   fx.randomTrigger = [this, &animations](B2SRomIDType romIdType, int romId, bool start, B2SAnimation*) { OnRandomAnimationTrigger(animations, romIdType, romId, start); };
   for (auto& animation : animations)
   {
      // Animation slow downs from the plugin settings (AllAnimationSlowDown takes precedence over per-animation)
      float scaledElapsed = elapsed;
      if (server)
      {
         int slowDown = server->GetAllAnimationSlowDown();
         if (slowDown <= 1)
            slowDown = server->GetAnimationSlowDown(animation.m_name);
         if (slowDown > 1)
            scaledElapsed /= static_cast<float>(slowDown);
      }
      animation.Update(scaledElapsed, fx);
   }
}

// B2S Pro native rotation start and stop (reference StartNativeRotation, StopNativeRotation and SetNativeRotationState,
// PB:341-433). A start cancels a pending stop and resumes from the stored progress; a stop applies the stop behavior;
// repeated reports of the same state do nothing
static void NativeStart(B2SBulb& bulb, std::chrono::steady_clock::time_point now, uint64_t stamp)
{
   bulb.m_nativeCommandedOn = true;
   bulb.m_nativeSlowDown = 0.0;
   bulb.m_nativeRunTillEnd = false;
   bulb.m_nativeRunToFirstStep = false;
   if (!bulb.m_nativeRunning)
   {
      bulb.m_nativeRunning = true;
      bulb.m_nativeLastTick = now;
   }
   bulb.m_nativeShowStamp = stamp; // Starting shows the bulb (PB:408)
}

static void NativeStop(B2SBulb& bulb)
{
   bulb.m_nativeCommandedOn = false;
   if (!bulb.m_nativeRunning)
      return;
   switch (bulb.m_nativeStopBehaviour)
   {
   case 0: bulb.m_nativeSlowDown = 1.0; break;
   case 2: bulb.m_nativeRunTillEnd = true; break;
   case 3: bulb.m_nativeRunToFirstStep = true; break;
   default: bulb.m_nativeRunning = false; break;
   }
}

static void NativeSetState(B2SBulb& bulb, bool enabled, std::chrono::steady_clock::time_point now, uint64_t stamp)
{
   if (bulb.m_nativeCommandedOn == enabled)
   {
      if (enabled && !bulb.m_nativeRunning)
         NativeStart(bulb, now, stamp);
      return;
   }
   if (enabled)
      NativeStart(bulb, now, stamp);
   else
      NativeStop(bulb);
}

// Script B2SSetData writes in the lamp-id space trigger animations bound to that lamp through IDJoin
// (mirrors the reference's MyB2SSetData which treats the B2SID space as lamp space). B2S Pro: they also start and stop
// native rotation bulbs on that lamp, each write in order, so that a B2SPulseData (on, then off) still starts the bulb
void B2SRenderer::DispatchScriptTriggers(B2SServer* server)
{
   const auto now = std::chrono::steady_clock::now();
   for (const auto& [lampId, value] : server->DrainAnimationTriggers())
   {
      // B2S Pro motion paths: every write in order, a rising edge from 0 acts (SV:2105-2134)
      if (m_motion)
         m_motion->OnB2SData(lampId, value, MotionNowMs());
      // B2S Pro physics launchers on a B2S id: every write that is not 0 (SV:2110)
      if (m_physics && value != 0)
         m_physics->FireLaunchers(Motion::Source::B2S, lampId, PhysicsNowSeconds());
      for (auto* animations : { &m_b2s->m_backglassAnimations, &m_b2s->m_dmdAnimations })
         for (auto& animation : *animations)
            for (const auto& trigger : animation.GetRomTriggers())
               if (trigger.romIdType == B2SRomIDType::Lamp && trigger.romId == lampId)
               {
                  const bool start = (value != 0) != trigger.inverted;
                  if (animation.m_randomStart)
                     OnRandomAnimationTrigger(*animations, B2SRomIDType::Lamp, lampId, start);
                  else if (start)
                     animation.Start();
                  else
                     animation.Stop();
                  break;
               }
      for (auto* bulbs : { &m_b2s->m_backglassIlluminations, &m_b2s->m_dmdIlluminations })
         for (auto& bulb : *bulbs)
            if (bulb->m_nativeRotation && bulb->m_nativeTriggerType == B2SRomIDType::Lamp && bulb->m_nativeTriggerId == lampId
               && !(!bulb->m_name.empty() && server->IsIlluminationLocked(bulb->m_name)))
               NativeSetState(*bulb, (value != 0) != bulb->m_romInverted, now, server->NextStamp());
   }
}

void B2SRenderer::OnRandomAnimationTrigger(vector<B2SAnimation>& animations, B2SRomIDType romIdType, int romId, bool start)
{
   if (start)
   {
      // Pick one animation at random among all the random-start animations sharing this ROM trigger,
      // weighted by RandomQuality (the reference adds the animation once per quality point to the pool)
      vector<B2SAnimation*> pool;
      int totalWeight = 0;
      for (B2SAnimation& animation : animations)
         if (animation.m_randomStart
            && std::ranges::any_of(
               animation.GetRomTriggers(), [romIdType, romId](const B2SAnimation::RomTrigger& trigger) { return trigger.romIdType == romIdType && trigger.romId == romId; }))
         {
            pool.push_back(&animation);
            totalWeight += std::max(1, animation.m_randomQuality);
         }
      const bool anyRunning = std::ranges::any_of(pool, [](const B2SAnimation* animation) { return animation->IsRunning(); });
      if (!anyRunning && !pool.empty())
      {
         int pick = std::rand() % totalWeight;
         for (B2SAnimation* animation : pool)
         {
            pick -= std::max(1, animation->m_randomQuality);
            if (pick < 0)
            {
               m_lastRandomAnimation = animation;
               break;
            }
         }
         m_lastRandomAnimation->Start(false);
      }
   }
   else if (m_lastRandomAnimation != nullptr)
   {
      m_lastRandomAnimation->Stop();
      m_lastRandomAnimation = nullptr;
   }
}

static void UpdatePivotMove(B2SBulb& bulb, std::chrono::steady_clock::time_point now, uint64_t nextLegStamp);
static void DrawRotatedBulb(VPXRenderContext2D* ctx, const B2SBulb& bulb, float alpha, float offsetX, float offsetY);

// B2S Pro motion path: the image of imageOwner (the trough swaps images between its members) drawn with its centre at the
// given authored position, rolled about its centre by the angle (clockwise positive), untinted. The rectangle is the bulb's own;
// its alpha crop only applies to its own image. VPX measures the texture rectangle from the bottom of the texture
static void DrawMotionImage(VPXRenderContext2D* ctx, const B2SBulb& imageOwner, const B2SBulb& shape, Motion::Vec center, Motion::Vec size, double roll, float alpha, const float offsetX, const float offsetY)
{
   if (imageOwner.m_image == nullptr || alpha <= 0.f || size.x <= 0.0 || size.y <= 0.0)
      return;
   const VPXTextureInfo* const info = GetTextureInfo(imageOwner.m_image);
   if (info == nullptr)
      return;
   float texX = 0.f, texTop = 0.f, texW = static_cast<float>(info->width), texH = static_cast<float>(info->height);
   if (&imageOwner == &shape && shape.m_pivotTexW > 0 && shape.m_pivotTexH > 0)
   {
      texX = static_cast<float>(shape.m_pivotTexX);
      texTop = static_cast<float>(shape.m_pivotTexY);
      texW = static_cast<float>(shape.m_pivotTexW);
      texH = static_cast<float>(shape.m_pivotTexH);
   }
   const float texY = static_cast<float>(info->height) - texTop - texH;
   ctx->DrawImage(ctx, imageOwner.m_image, 1.f, 1.f, 1.f, alpha, texX, texY, texW, texH, texX + texW * 0.5f, texY + texH * 0.5f, static_cast<float>(roll),
      static_cast<float>(center.x - size.x * 0.5) + offsetX, static_cast<float>(center.y - size.y * 0.5) + offsetY, static_cast<float>(size.x), static_cast<float>(size.y));
}

// The persistent source of a sequence anchor comes first, at the anchor's own place in the draw order, even while the anchor is hidden
void B2SRenderer::RenderMotionBulb(VPXRenderContext2D* ctx, const B2SBulb& bulb, float alpha)
{
   const Motion::System::Feeder feeder = m_motion->GetFeeder(bulb.m_motionIndex, alpha > 0.f);
   if (feeder.draw && feeder.image >= 0 && static_cast<size_t>(feeder.image) < m_motionBulbs.size())
      DrawMotionImage(ctx, *m_motionBulbs[static_cast<size_t>(feeder.image)], bulb, feeder.center, feeder.size, feeder.roll, 1.f, m_layoutOX, m_layoutOY);
   if (alpha > 0.f)
   {
      const Motion::System::View view = m_motion->GetView(bulb.m_motionIndex);
      if (view.image >= 0 && static_cast<size_t>(view.image) < m_motionBulbs.size())
         DrawMotionImage(ctx, *m_motionBulbs[static_cast<size_t>(view.image)], bulb, view.center, view.size, view.roll, alpha, m_layoutOX, m_layoutOY);
   }
}

void B2SRenderer::RenderBulbs(VPXRenderContext2D* ctx, const B2SServer* server, const vector<std::unique_ptr<B2SBulb>>& bulbs, float elapsed, B2SDualMode dualMode, bool behindCanvas)
{
   for (const auto& bulb : bulbs)
   {
      if (bulb->m_behindCanvas != behindCanvas)
         continue; // B2S Pro: behind-canvas bulbs have their own pass before the background image
      if (dualMode != B2SDualMode::Both && bulb->m_dualMode != B2SDualMode::Both && bulb->m_dualMode != dualMode)
         continue; // Dual backglass: only render bulbs matching the active mode
      if (bulb->m_bakedIntoBackground)
         continue; // Drawn as part of the lit background image
      // Dynamic Front Lighting exists on the backglass only (reference: formDMD has none), a light of the DMD is an ordinary bulb there
      const bool dynamicLight = bulb->m_dfl.enabled && &bulbs == &m_b2s->m_backglassIlluminations;
      if (m_dflPhase == 1 && !dynamicLight)
         continue; // The base pass of the Dynamic Front lights only handles the lights
      if (m_dflPhase == 2 && dynamicLight)
      {
         RenderDynamicLight(ctx, *bulb, dualMode); // Its brightness is known since the base pass
         continue;
      }
      // B2S Pro position grid representative: moved between its grid markers by named commands, on screen like the reference
      if (bulb->m_gridIndex >= 0 && static_cast<size_t>(bulb->m_gridIndex) < m_b2s->m_positionGrids.size())
         UpdatePositionGrid(m_b2s->m_positionGrids[bulb->m_gridIndex], *bulb, ctx->outWidth > 0.f && ctx->srcWidth > 0.f ? ctx->outWidth / ctx->srcWidth : 1.f,
            ctx->outHeight > 0.f && ctx->srcHeight > 0.f ? ctx->outHeight / ctx->srcHeight : 1.f, server);
      if (bulb->m_pivotAnimation)
      {
         RenderPivot(ctx, server, *bulb);
         continue;
      }
      if (bulb->m_nativeRotation)
      {
         RenderNativeRotation(ctx, server, *bulb);
         continue;
      }
      if (bulb->m_physicsIndex != -1 && m_physics)
      {
         // B2S Pro physics ball: drawn at its simulated position, rolled and clipped by its selection mask, untinted (fB:885-889)
         RenderPhysicsBall(ctx, server, *bulb);
         continue;
      }
      const bool locked = server && !bulb->m_name.empty() && server->IsIlluminationLocked(bulb->m_name);
      // ROM polling: a ROM state change counts as a write into the same channel as script writes
      // (the reference routes both through the same shared state, the most recent one wins), so the
      // probe keeps updating even while a script write is in control. Probes are suspended while the
      // illumination group is locked by an animation, and the first observation does not count as a
      // change. Mech values are rotation steps, not visibility writes, and never take the channel.
      // B2S Pro: the reference applies the first switch state it polls at once, so for switch bulbs
      // the first observation counts (a switch closed at startup lights its bulbs).
      if (!locked)
      {
         float* const probe = bulb->m_snippitType == B2SSnippitType::MechRotatingImage ? &bulb->m_mechProbe
            : bulb->m_snippitType == B2SSnippitType::SelfRotatingImage                 ? &bulb->m_romOn
                                                                                       : &bulb->m_romProbe;
         const float prev = *probe;
         bulb->m_romUpdater();
         if (server != nullptr && (prev >= 0.f || bulb->m_romIdType == B2SRomIDType::Switch) && *probe != prev && bulb->m_romIdType != B2SRomIDType::Mech)
            bulb->m_romStamp = server->NextStamp();
      }
      float state = 0.f;
      bool fromLamp = false;
      uint64_t writeStamp = 0;
      const bool scripted = server && server->GetBulbState(*bulb, state, &fromLamp, &writeStamp);
      // Script writes on the lamp channel of a rotating image drive its rotation instead of its
      // visibility (reference): for mech images the value selects the rotation step, for self
      // rotating images it toggles the spinning, most recent write between script and ROM wins
      const int lampId = (bulb->m_romIdType == B2SRomIDType::Lamp && bulb->m_romId > 0) ? bulb->m_romId : -1;
      uint64_t lampWriteStamp = 0;
      float lampWriteState = 0.f;
      const bool lampWinsRotation = server != nullptr && !locked && lampId > 0 && server->GetScriptedLampState(lampId, lampWriteState, &lampWriteStamp) && lampWriteStamp > bulb->m_romStamp;
      float rotation = 0.f;
      if (bulb->m_snippitType == B2SSnippitType::MechRotatingImage)
      {
         // Mech rotating images stay visible (the channel value only selects the rotation step);
         // illumination group writes still drive their visibility
         if (scripted && !fromLamp)
            bulb->m_brightness = state;
         if (lampWinsRotation)
            bulb->m_mechRot = lampWriteState;
         else if (bulb->m_mechProbe >= 0.f)
            bulb->m_mechRot = bulb->m_mechProbe;
         rotation = 360.f * (bulb->m_mechRot / static_cast<float>(bulb->m_snippitRotatingSteps));
      }
      else if (bulb->m_snippitType == B2SSnippitType::SelfRotatingImage)
      {
         if (scripted && !fromLamp)
            bulb->m_brightness = state;
         float spin = std::max(0.f, bulb->m_romOn);
         if (lampWinsRotation)
            spin = (bulb->m_b2sId >= 0 && bulb->m_b2sValue > 0) ? (static_cast<int>(lampWriteState) == bulb->m_b2sValue ? 1.f : 0.f)
                                                                : ((lampWriteState != 0.f) != bulb->m_romInverted ? 1.f : 0.f);
         // Rotation only toggles on state transitions so that explicit B2SStartRotation calls keep
         // spinning until B2SStopRotation or a state write (reference behaviour)
         const bool spinOn = spin >= 0.5f;
         if (spinOn != bulb->m_spinDriverOn)
         {
            bulb->m_spinDriverOn = spinOn;
            if (spinOn)
               bulb->StartRotation();
            else
               bulb->StopRotation();
         }
         bulb->UpdateRotation(elapsed);
         rotation = bulb->GetRotationAngle();
      }
      else if (scripted)
         bulb->m_brightness = state;
      else if (bulb->m_romStamp > 0 || (server == nullptr && bulb->m_romProbe >= 0.f))
         // The probe only takes over once a ROM change was actually observed: the first observation
         // is the initial state read, not a write, so InitialState survives until the first event
         bulb->m_brightness = bulb->m_romProbe;
      // B2S Pro: an external pivot move or a position grid shift shows the bulb, until a later write (PB:625, PB:798)
      if (const uint64_t showStamp = std::max(bulb->m_pivotShowStamp, bulb->m_gridShowStamp); showStamp > std::max(scripted ? writeStamp : uint64_t(0), bulb->m_romStamp))
         bulb->m_brightness = 1.f;
      // B2S Pro motion paths: a start, a shift or a sequence step shows the bulb, a finished removal hides it (reference PB:803-878,
      // BD:1135-1145), until a later write
      uint64_t motionStamp = 0;
      if (bulb->m_motionIndex >= 0 && m_motion)
      {
         const Motion::System::View view = m_motion->GetView(bulb->m_motionIndex);
         motionStamp = view.visibleStamp;
         if (motionStamp > std::max(scripted ? writeStamp : uint64_t(0), bulb->m_romStamp))
            bulb->m_brightness = view.visible ? 1.f : 0.f;
      }
      // B2S Pro: a flasher pulse shows the bulb for FlasherPulseDuration after each ON write, ignores OFF writes
      // meanwhile, then hides it until the next ON write, even while its state stays ON (reference PB:1205-1257)
      float brightness = bulb->m_brightness;
      if (bulb->m_flasherPulseDuration > 0 && bulb->m_snippitType != B2SSnippitType::MechRotatingImage && bulb->m_snippitType != B2SSnippitType::SelfRotatingImage)
      {
         const bool sourceOn = bulb->m_brightness > 0.f;
         const uint64_t stamp = std::max(scripted ? writeStamp : bulb->m_romStamp, motionStamp);
         const auto now = std::chrono::steady_clock::now();
         if (sourceOn && (!bulb->m_pulseSourceOn || stamp != bulb->m_pulseWriteStamp))
            bulb->m_pulseEnd = now + std::chrono::milliseconds(bulb->m_flasherPulseDuration);
         bulb->m_pulseSourceOn = sourceOn;
         bulb->m_pulseWriteStamp = stamp;
         brightness = now < bulb->m_pulseEnd ? 1.f : 0.f;
      }
      if (dynamicLight)
      {
         // B2S Pro Dynamic Front light (reference fB:655-685, 863-878): its image is never drawn the usual way. The base pass (phase 1) draws it under
         // every bulb when ShowLight is on, and keeps the brightness for the main pass; outside the two passes (behind canvas) it does both at once
         bulb->m_dflBrightness = brightness;
         if (m_dflPhase == 1)
         {
            if (brightness > 0.f && bulb->m_dfl.showLight && !bulb->m_dfl.separate)
               DrawLightImage(ctx, *bulb);
         }
         else
            RenderDynamicLight(ctx, *bulb, dualMode); // Behind the canvas: no base image, the layer and the overlays go onto the bare surface
         continue;
      }
      if (bulb->m_motionIndex >= 0 && m_motion)
      {
         // B2S Pro motion path: drawn at the position its route has reached, rolled, untinted (reference fB:817-929, fB:1114-1156)
         RenderMotionBulb(ctx, *bulb, brightness);
         continue;
      }
      if (bulb->m_pivotRotation)
      {
         // B2S Pro external pivot representative: turned by named commands, drawn rotated about its hinge
         UpdatePivotMove(*bulb, std::chrono::steady_clock::now(), 0);
         DrawRotatedBulb(ctx, *bulb, brightness, m_layoutOX, m_layoutOY);
         NoteSnippetDrawn(*bulb, brightness, true);
         continue;
      }
      if (bulb->m_offImage && brightness < 1.f)
      {
         const VPXTextureInfo* const bulbTex = GetTextureInfo(bulb->m_offImage);
         // Drawn untinted like the server (plain DrawImage, fB:848-926): the Designer already bakes LightColor into the exported image
         ctx->DrawImage(ctx, bulb->m_offImage, 1.f, 1.f, 1.f, 1.f,
            0.f, 0.f, static_cast<float>(bulbTex->width), static_cast<float>(bulbTex->height),
            static_cast<float>(bulbTex->width) * 0.5f, static_cast<float>(bulbTex->height) * 0.5f, rotation,
            ContentX(static_cast<float>(bulb->m_locationX)) + bulb->m_gridOffsetX, static_cast<float>(bulb->m_locationY) + bulb->m_gridOffsetY + m_layoutOY, static_cast<float>(bulb->m_width),
            static_cast<float>(bulb->m_height));
      }
      if (bulb->m_image)
      {
         const VPXTextureInfo* const bulbTex = GetTextureInfo(bulb->m_image);
         ctx->DrawImage(ctx, bulb->m_image, 1.f, 1.f, 1.f, brightness,
            0.f, 0.f, static_cast<float>(bulbTex->width), static_cast<float>(bulbTex->height),
            static_cast<float>(bulbTex->width) * 0.5f, static_cast<float>(bulbTex->height) * 0.5f, rotation,
            ContentX(static_cast<float>(bulb->m_locationX)) + bulb->m_gridOffsetX, static_cast<float>(bulb->m_locationY) + bulb->m_gridOffsetY + m_layoutOY, static_cast<float>(bulb->m_width),
            static_cast<float>(bulb->m_height));
      }
      if (m_b2s->m_hasDynamicFront)
         NoteSnippetDrawn(*bulb, bulb->m_image ? brightness : 0.f, false);
   }
}

// B2S Pro pivot animation. Each trigger event restarts a smoothstep swing from the current angle over the pivot
// duration and shows the bulb (reference PB:143-157, PB:611-675); automatic pivots swing up and down while
// triggered and return to 0 when released. The image is drawn rotated about its hinge, untinted as in the reference
// (fB:879-906, fB:4417-4442). Group writes on the bulb name still show or hide it, the most recent write winning
// Starts a pivot swing from the current angle (reference SetPivotRotationTarget, PB:611-635), which also shows the bulb
static void StartPivotMove(B2SBulb& bulb, float angle, int duration, std::chrono::steady_clock::time_point start, uint64_t stamp)
{
   bulb.m_pivotStartAngle = bulb.m_pivotAngle;
   bulb.m_pivotTargetAngle = angle;
   bulb.m_pivotMoveDuration = std::clamp(duration, 10, bulb.m_pivotAutomaticOscillation ? 5000 : 1000);
   bulb.m_pivotMoveStart = start;
   bulb.m_pivotMoving = true;
   bulb.m_pivotShowStamp = stamp;
}

// Advances a pivot swing; an automatic pivot starts its next leg when a leg ends while triggered (PB:661-675)
static void UpdatePivotMove(B2SBulb& bulb, std::chrono::steady_clock::time_point now, uint64_t nextLegStamp)
{
   if (!bulb.m_pivotMoving)
      return;
   const float elapsedMs = std::chrono::duration<float, std::milli>(now - bulb.m_pivotMoveStart).count();
   const float p = std::min(1.f, elapsedMs / static_cast<float>(bulb.m_pivotMoveDuration));
   bulb.m_pivotAngle = bulb.m_pivotStartAngle + (bulb.m_pivotTargetAngle - bulb.m_pivotStartAngle) * (p * p * (3.f - 2.f * p));
   if (p >= 1.f)
   {
      bulb.m_pivotAngle = bulb.m_pivotTargetAngle;
      bulb.m_pivotMoving = false;
      if (bulb.m_pivotAutomaticOscillation && bulb.m_pivotAutomaticActive)
         StartPivotMove(bulb, std::abs(bulb.m_pivotTargetAngle - bulb.m_pivotUpAngle) < 0.01f ? bulb.m_pivotDownAngle : bulb.m_pivotUpAngle, bulb.m_pivotDuration, now,
            nextLegStamp);
   }
}

// Draws a bulb rotated about its hinge, from its alpha-cropped area, untinted as in the reference (fB:879-906, fB:4417-4442).
// The hinge of a bulb that is not a pivot animation is clamped to its rectangle (fB:3388-3389)
static void DrawRotatedBulb(VPXRenderContext2D* ctx, const B2SBulb& bulb, float alpha, const float offsetX, const float offsetY)
{
   if (alpha <= 0.f || bulb.m_image == nullptr || bulb.m_pivotTexW <= 0 || bulb.m_pivotTexH <= 0)
      return;
   const float hingeX = bulb.m_pivotAnimation ? bulb.m_pivotX : std::clamp(bulb.m_pivotX, 0.f, 1.f);
   const float hingeY = bulb.m_pivotAnimation ? bulb.m_pivotY : std::clamp(bulb.m_pivotY, 0.f, 1.f);
   // VPX measures the texture rectangle of DrawImage from the bottom of the texture, so the cropped area (measured from the
   // top, like the reference) is converted; the pivot stays at the same fraction of the rectangle
   const VPXTextureInfo* const info = GetTextureInfo(bulb.m_image);
   const float texX = static_cast<float>(bulb.m_pivotTexX);
   const float texW = static_cast<float>(bulb.m_pivotTexW), texH = static_cast<float>(bulb.m_pivotTexH);
   const float texY = info ? static_cast<float>(info->height) - static_cast<float>(bulb.m_pivotTexY) - texH : static_cast<float>(bulb.m_pivotTexY);
   ctx->DrawImage(ctx, bulb.m_image, 1.f, 1.f, 1.f, alpha, texX, texY, texW, texH, texX + texW * hingeX, texY + texH * hingeY, bulb.m_pivotAngle,
      static_cast<float>(bulb.m_pivotDstX) + bulb.m_gridOffsetX + offsetX, static_cast<float>(bulb.m_pivotDstY) + bulb.m_gridOffsetY + offsetY, static_cast<float>(bulb.m_pivotDstW),
      static_cast<float>(bulb.m_pivotDstH));
}

// B2S Pro position grid shift: a cubic Hermite curve from the current centre to the target in output pixels, starting with
// the current velocity limited to twice the average speed, and ending with the velocity toward the look-ahead target
// (reference StartExternalGridShift, PB:769-801, PB:1002-1040). Each shift shows the bulb
static void StartGridShift(B2SBulb& bulb, float targetX, float targetY, int duration, float lookX, float lookY, std::chrono::steady_clock::time_point start, uint64_t stamp)
{
   float startX = bulb.m_gridCenterX, startY = bulb.m_gridCenterY;
   float velX = bulb.m_gridLastVX, velY = bulb.m_gridLastVY;
   if (bulb.m_gridRunning)
   {
      // Restart from the point and velocity of the running shift
      const float d = bulb.m_gridDuration;
      const float t = std::clamp(std::chrono::duration<float, std::milli>(start - bulb.m_gridStart).count() / d, 0.f, 1.f);
      const float t2 = t * t, t3 = t2 * t;
      const float h00 = 2.f * t3 - 3.f * t2 + 1.f, h10 = t3 - 2.f * t2 + t, h01 = -2.f * t3 + 3.f * t2, h11 = t3 - t2;
      const float dh00 = 6.f * t2 - 6.f * t, dh10 = 3.f * t2 - 4.f * t + 1.f, dh01 = -6.f * t2 + 6.f * t, dh11 = 3.f * t2 - 2.f * t;
      startX = h00 * bulb.m_gridP0X + h10 * d * bulb.m_gridV0X + h01 * bulb.m_gridP1X + h11 * d * bulb.m_gridV1X;
      startY = h00 * bulb.m_gridP0Y + h10 * d * bulb.m_gridV0Y + h01 * bulb.m_gridP1Y + h11 * d * bulb.m_gridV1Y;
      velX = (dh00 * bulb.m_gridP0X + dh10 * d * bulb.m_gridV0X + dh01 * bulb.m_gridP1X + dh11 * d * bulb.m_gridV1X) / d;
      velY = (dh00 * bulb.m_gridP0Y + dh10 * d * bulb.m_gridV0Y + dh01 * bulb.m_gridP1Y + dh11 * d * bulb.m_gridV1Y) / d;
      bulb.m_gridCenterX = startX;
      bulb.m_gridCenterY = startY;
   }
   const int d = std::clamp(duration, 10, 1000);
   const double dx = targetX - startX, dy = targetY - startY;
   const double maximum = std::max(0.01, std::sqrt(dx * dx + dy * dy) * 2.0 / d);
   const double speed = std::sqrt(static_cast<double>(velX) * velX + static_cast<double>(velY) * velY);
   if (speed > maximum && speed > 0.0)
   {
      const float scale = static_cast<float>(maximum / speed);
      velX *= scale;
      velY *= scale;
   }
   bulb.m_gridP0X = startX;
   bulb.m_gridP0Y = startY;
   bulb.m_gridP1X = targetX;
   bulb.m_gridP1Y = targetY;
   bulb.m_gridV0X = velX;
   bulb.m_gridV0Y = velY;
   bulb.m_gridV1X = (lookX - targetX) / static_cast<float>(d);
   bulb.m_gridV1Y = (lookY - targetY) / static_cast<float>(d);
   bulb.m_gridLastVX = velX;
   bulb.m_gridLastVY = velY;
   bulb.m_gridDuration = static_cast<float>(d);
   bulb.m_gridStart = start;
   bulb.m_gridRunning = true;
   bulb.m_gridShowStamp = stamp;
}

// B2S Pro: applies the queued position grid commands with the one-step look-ahead and the flush timer of the reference
// (ExternalPositionGridState, BD:1704-1727), then advances the representative's shift. Centres are those of the
// alpha-cropped rectangles, converted to output pixels with the current scale
void B2SRenderer::UpdatePositionGrid(B2SPositionGrid& grid, B2SBulb& bulb, float scaleX, float scaleY, const B2SServer* server)
{
   const auto center = [scaleX, scaleY](const B2SBulb& b, float& x, float& y)
   {
      x = (static_cast<float>(b.m_pivotDstX) + static_cast<float>(b.m_pivotDstW) * 0.5f) * scaleX;
      y = (static_cast<float>(b.m_pivotDstY) + static_cast<float>(b.m_pivotDstH) * 0.5f) * scaleY;
   };
   const auto nextStamp = [server, &bulb]() { return server ? server->NextStamp() : std::max(bulb.m_gridShowStamp, bulb.m_pivotShowStamp) + 1; };
   if (!bulb.m_gridCenterSet)
   {
      center(bulb, bulb.m_gridCenterX, bulb.m_gridCenterY);
      bulb.m_gridCenterSet = true;
   }
   const auto flushInterval = std::chrono::milliseconds(std::clamp(grid.duration * 2, 20, 2000));
   const auto flush = [&]()
   {
      if (const auto it = grid.targets.find(grid.pending); it != grid.targets.end())
      {
         float x, y;
         center(*it->second, x, y);
         StartGridShift(bulb, x, y, grid.duration, x, y, grid.flushAt, nextStamp());
      }
      grid.pending.clear();
   };
   for (const B2SPositionGrid::Command& command : grid.commands)
   {
      const auto target = grid.targets.find(command.target);
      if (target == grid.targets.end())
         continue;
      if (!grid.pending.empty() && command.time >= grid.flushAt)
         flush();
      if (const auto pending = grid.targets.find(grid.pending); !grid.pending.empty() && pending != grid.targets.end())
      {
         float x, y, lookX, lookY;
         center(*pending->second, x, y);
         center(*target->second, lookX, lookY);
         StartGridShift(bulb, x, y, grid.duration, lookX, lookY, command.time, nextStamp());
      }
      grid.pending = command.target;
      grid.flushAt = command.time + flushInterval;
   }
   grid.commands.clear();
   const auto now = std::chrono::steady_clock::now();
   if (!grid.pending.empty() && now >= grid.flushAt)
      flush();

   if (bulb.m_gridRunning)
   {
      const float d = bulb.m_gridDuration;
      const float t = std::clamp(std::chrono::duration<float, std::milli>(now - bulb.m_gridStart).count() / d, 0.f, 1.f);
      const float t2 = t * t, t3 = t2 * t;
      const float h00 = 2.f * t3 - 3.f * t2 + 1.f, h10 = t3 - 2.f * t2 + t, h01 = -2.f * t3 + 3.f * t2, h11 = t3 - t2;
      const float dh00 = 6.f * t2 - 6.f * t, dh10 = 3.f * t2 - 4.f * t + 1.f, dh01 = -6.f * t2 + 6.f * t, dh11 = 3.f * t2 - 2.f * t;
      bulb.m_gridCenterX = h00 * bulb.m_gridP0X + h10 * d * bulb.m_gridV0X + h01 * bulb.m_gridP1X + h11 * d * bulb.m_gridV1X;
      bulb.m_gridCenterY = h00 * bulb.m_gridP0Y + h10 * d * bulb.m_gridV0Y + h01 * bulb.m_gridP1Y + h11 * d * bulb.m_gridV1Y;
      bulb.m_gridLastVX = (dh00 * bulb.m_gridP0X + dh10 * d * bulb.m_gridV0X + dh01 * bulb.m_gridP1X + dh11 * d * bulb.m_gridV1X) / d;
      bulb.m_gridLastVY = (dh00 * bulb.m_gridP0Y + dh10 * d * bulb.m_gridV0Y + dh01 * bulb.m_gridP1Y + dh11 * d * bulb.m_gridV1Y) / d;
      if (t >= 1.f)
         bulb.m_gridRunning = false;
   }
   float homeX, homeY;
   center(bulb, homeX, homeY);
   bulb.m_gridOffsetX = (bulb.m_gridCenterX - homeX) / scaleX;
   bulb.m_gridOffsetY = (bulb.m_gridCenterY - homeY) / scaleY;
}

// B2S Pro: applies the external pivot moves requested by the script (B2SSetData on a member name) and queues the
// position grid moves for their representative
void B2SRenderer::DispatchNamedMoves(B2SServer* server)
{
   if (server == nullptr)
      return;
   for (const B2SServer::NamedMove& move : server->DrainNamedMoves())
   {
      if (move.grid)
      {
         if (move.index < m_b2s->m_positionGrids.size())
         {
            B2SPositionGrid& grid = m_b2s->m_positionGrids[move.index];
            if (grid.representative != nullptr && grid.commands.size() < 1024)
               grid.commands.push_back({ move.target, move.time });
         }
      }
      else if (move.index < m_b2s->m_namedPivots.size())
      {
         const B2SNamedPivot& pivot = m_b2s->m_namedPivots[move.index];
         if (const auto angle = pivot.angles.find(move.target); pivot.representative != nullptr && angle != pivot.angles.end())
            StartPivotMove(*pivot.representative, angle->second, pivot.duration, move.time, server->NextStamp());
      }
   }
}

// B2S Pro one-image native rotation (reference PB:341-473, fB:1340-1388, fB:4556-4559). The trigger starts the spin on
// its ON edge and applies the stop behavior on its OFF edge; repeated reports of the same state do nothing. The angle
// advances continuously by (360 / steps) per interval, with no acceleration, and a stop never hides the image: it is
// hidden until the first start unless InitialState is 1 or the bulb is continuous. Continuous bulbs start 2 s after the
// backglass loads. The image is drawn whole, rotated about its hinge, untinted (fB:879-906, fB:4417-4442)
void B2SRenderer::RenderNativeRotation(VPXRenderContext2D* ctx, const B2SServer* server, B2SBulb& bulb)
{
   const auto now = std::chrono::steady_clock::now();
   if (!bulb.m_nativeInitialized)
   {
      bulb.m_nativeInitialized = true;
      bulb.m_nativeLoadTime = now;
      bulb.m_nativeLastTick = now;
   }
   const auto nextStamp = [server, &bulb]() { return server ? server->NextStamp() : bulb.m_nativeShowStamp + 1; };

   if (bulb.m_nativeContinuous)
   {
      if (!bulb.m_nativeContinuousStarted && now >= bulb.m_nativeLoadTime + std::chrono::milliseconds(2000))
      {
         bulb.m_nativeContinuousStarted = true;
         NativeStart(bulb, now, nextStamp());
      }
   }
   else if (bulb.m_nativeTriggerId > 0 && bulb.m_nativeTriggerType != B2SRomIDType::Mech && bulb.m_nativeTriggerType != B2SRomIDType::NotDefined)
   {
      // ROM triggers are skipped while an animation locks the bulb's group (SV:951); the ROM polling keeps the probe
      // current so that a change during a lock is lost, as in the reference. ROM outputs only report changes, so an
      // output found off at startup is no event, except for switches whose first poll applies (SV:274-277)
      const bool locked = server && !bulb.m_name.empty() && server->IsIlluminationLocked(bulb.m_name);
      const float previous = bulb.m_nativeProbe;
      bulb.m_nativeUpdater();
      const float value = bulb.m_nativeProbe;
      if (value >= 0.f && !locked)
      {
         const bool raw = value > 0.f;
         if (previous < 0.f ? (raw || bulb.m_nativeTriggerType == B2SRomIDType::Switch) : raw != (previous > 0.f))
            NativeSetState(bulb, raw != bulb.m_romInverted, now, nextStamp());
      }
      // B2SSetData(id) and B2SPulseData(id) on the lamp channel arrive through DispatchScriptTriggers (SV:2184-2200)
   }

   // Tick: progress advances by stepAngle * elapsed / interval, scaled down while spinning off (PB:435-473)
   if (bulb.m_nativeRunning)
   {
      const double elapsedMs = std::chrono::duration<double, std::milli>(now - bulb.m_nativeLastTick).count();
      bulb.m_nativeLastTick = now;
      const float stepAngle = 360.f / static_cast<float>(std::max(4, bulb.m_nativeSteps));
      double speedFactor = 1.0;
      int slowDownIntervals = 0;
      if (bulb.m_nativeSlowDown > 0.0)
      {
         slowDownIntervals = 25 + static_cast<int>(std::ceil(900.0 / std::max(1, bulb.m_nativeInterval)));
         speedFactor = std::max(0.0, 1.0 - (bulb.m_nativeSlowDown - 1.0) / std::max(1, slowDownIntervals));
      }
      const float next = bulb.m_nativeProgress + static_cast<float>(stepAngle * elapsedMs / std::max(1, bulb.m_nativeInterval) * speedFactor);
      const bool completed = next >= 360.f;
      bulb.m_nativeProgress = std::fmod(next, 360.f);
      bulb.m_pivotAngle = bulb.m_nativeAntiClockwise ? -bulb.m_nativeProgress : bulb.m_nativeProgress;
      bulb.m_nativeShowStamp = nextStamp(); // Every tick shows the bulb (PB:452)
      if (bulb.m_nativeSlowDown > 0.0)
      {
         bulb.m_nativeSlowDown += elapsedMs / std::max(1, bulb.m_nativeInterval);
         if (bulb.m_nativeSlowDown >= slowDownIntervals + 1.0)
         {
            bulb.m_nativeRunning = false;
            bulb.m_nativeSlowDown = 0.0;
         }
      }
      else if (bulb.m_nativeRunTillEnd && completed)
      {
         bulb.m_nativeRunTillEnd = false;
         bulb.m_nativeRunning = false;
      }
      else if (bulb.m_nativeRunToFirstStep && completed)
      {
         // The shown angle snaps to 0 while the stored progress keeps the overshoot (PB:466-472)
         bulb.m_nativeRunToFirstStep = false;
         bulb.m_pivotAngle = 0.f;
         bulb.m_nativeRunning = false;
      }
   }

   // Visibility: InitialState or continuous, then the most recent of a start or spinning frame and a group write
   float state = 0.f;
   uint64_t writeStamp = 0;
   const bool scripted = server && server->GetBulbState(bulb, state, nullptr, &writeStamp);
   bool visible = bulb.m_initialStateOne || bulb.m_nativeContinuous || bulb.m_nativeShowStamp > 0;
   if (scripted && writeStamp > bulb.m_nativeShowStamp)
      visible = state != 0.f;
   float alpha = visible ? 1.f : 0.f;
   if (bulb.m_flasherPulseDuration > 0)
   {
      // Flasher pulse (PB:1205-1257): every start and spinning frame restarts it
      const uint64_t stamp = std::max(bulb.m_nativeShowStamp, scripted ? writeStamp : uint64_t(0));
      if (visible && (!bulb.m_pulseSourceOn || stamp != bulb.m_pulseWriteStamp))
         bulb.m_pulseEnd = now + std::chrono::milliseconds(bulb.m_flasherPulseDuration);
      bulb.m_pulseSourceOn = visible;
      bulb.m_pulseWriteStamp = stamp;
      alpha = now < bulb.m_pulseEnd ? 1.f : 0.f;
   }
   DrawRotatedBulb(ctx, bulb, alpha, m_layoutOX, m_layoutOY);
   NoteSnippetDrawn(bulb, alpha, true);
}

void B2SRenderer::RenderPivot(VPXRenderContext2D* ctx, const B2SServer* server, B2SBulb& bulb)
{
   const auto now = std::chrono::steady_clock::now();
   const bool automatic = bulb.m_pivotAutomaticOscillation;
   const auto nextStamp = [server, &bulb]() { return server ? server->NextStamp() : bulb.m_pivotShowStamp + 1; };
   // B2S Pro: a pivot that a physics ball names as its flipper swings on the physics clock (1 ms substeps, so the ball sees its angular
   // velocity), which is first brought up to date so that the swing starts at the physics time of the event (PB:611-635)
   const bool physics = bulb.m_physicsPivot >= 0 && m_physics && m_physics->IsPhysicsPivot(bulb.m_physicsPivot);
   const auto moveTo = [&](float angle)
   {
      if (physics)
      {
         m_physics->StartPivotSwing(bulb.m_physicsPivot, angle, bulb.m_pivotDuration, PhysicsNowSeconds());
         bulb.m_pivotShowStamp = nextStamp();
      }
      else
         StartPivotMove(bulb, angle, bulb.m_pivotDuration, now, nextStamp());
   };
   const auto trigger = [&](bool enabled)
   {
      if (automatic)
      {
         if (enabled == bulb.m_pivotAutomaticActive)
            return;
         bulb.m_pivotAutomaticActive = enabled;
         if (physics)
            m_physics->SetPivotAutomaticActive(bulb.m_physicsPivot, enabled);
         moveTo(enabled ? bulb.m_pivotUpAngle : 0.f);
      }
      else
         moveTo(enabled ? bulb.m_pivotUpAngle : bulb.m_pivotDownAngle);
   };

   // Trigger events (BD:405-422: type 4 always, other types need a trigger id). ROM outputs only report changes, so
   // an output found off at startup is no event; the first switch poll applies even when open (fB:117-131)
   if (bulb.m_pivotTriggerType == 4)
   {
      if (!bulb.m_pivotStarted)
      {
         bulb.m_pivotStarted = true;
         trigger(true);
      }
   }
   else if (bulb.m_pivotTriggerType != 0 && bulb.m_pivotTriggerId > 0)
   {
      switch (bulb.m_pivotTriggerType)
      {
      case 1:
      case 2:
      case 5:
      {
         const float prev = bulb.m_pivotProbe;
         bulb.m_pivotUpdater();
         const float value = bulb.m_pivotProbe;
         if (value >= 0.f)
         {
            const bool on = value > 0.f;
            if (prev < 0.f ? (on || bulb.m_pivotTriggerType == 5) : on != (prev > 0.f))
               trigger(on);
         }
         break;
      }
      default:
      {
         // B2S id: every B2SSetData call is an event (SV:2109)
         float state = 0.f;
         uint64_t stamp = 0;
         if (server && server->GetScriptedLampState(bulb.m_pivotTriggerId, state, &stamp) && stamp != bulb.m_pivotScriptStamp)
         {
            bulb.m_pivotScriptStamp = stamp;
            trigger(state != 0.f);
         }
         break;
      }
      }
   }

   // Swing: smoothstep over the duration (PB:661-675), on the physics clock for a flipper
   if (physics)
      bulb.m_pivotAngle = m_physics->PivotAngle(bulb.m_physicsPivot);
   else if (bulb.m_pivotMoving)
      UpdatePivotMove(bulb, now, server ? server->NextStamp() : bulb.m_pivotShowStamp + 1);

   // Visibility: InitialState, then the most recent of a trigger, command or grid event (always shows, PB:625, PB:798)
   // and a group write
   float state = 0.f;
   uint64_t writeStamp = 0;
   const bool scripted = server && server->GetBulbState(bulb, state, nullptr, &writeStamp);
   const uint64_t showStamp = std::max(bulb.m_pivotShowStamp, bulb.m_gridShowStamp);
   bool visible = bulb.m_initialStateOne || showStamp > 0;
   if (scripted && writeStamp > showStamp)
      visible = state != 0.f;
   float alpha = visible ? 1.f : 0.f;
   if (bulb.m_flasherPulseDuration > 0)
   {
      // Flasher pulse (PB:1205-1257): every event that shows the pivot restarts it
      const uint64_t stamp = std::max(showStamp, scripted ? writeStamp : uint64_t(0));
      if (visible && (!bulb.m_pulseSourceOn || stamp != bulb.m_pulseWriteStamp))
         bulb.m_pulseEnd = now + std::chrono::milliseconds(bulb.m_flasherPulseDuration);
      bulb.m_pulseSourceOn = visible;
      bulb.m_pulseWriteStamp = stamp;
      alpha = now < bulb.m_pulseEnd ? 1.f : 0.f;
   }
   DrawRotatedBulb(ctx, bulb, alpha, m_layoutOX, m_layoutOY);
   NoteSnippetDrawn(bulb, alpha, true);
}

// B2S Pro Dynamic Front Lighting: an image snippet leaves a record of the frame in which it was drawn (shown, rectangle, pose), which the lights
// above it read. Moving or physics driven snippets are not recorded, so they are not lit
void B2SRenderer::NoteSnippetDrawn(B2SBulb& bulb, float brightness, bool rotated)
{
   if (!m_b2s->m_hasDynamicFront || !bulb.m_isImageSnippit || bulb.m_dflPixels.empty())
      return;
   bulb.m_dflFrame = brightness > 0.f ? m_dflFrameId : 0;
   bulb.m_dflX = static_cast<float>(bulb.m_pivotDstX) + bulb.m_gridOffsetX;
   bulb.m_dflY = static_cast<float>(bulb.m_pivotDstY) + bulb.m_gridOffsetY;
   bulb.m_dflAngle = rotated ? bulb.m_pivotAngle : 0.f;
   bulb.m_dflRotated = rotated;
}

// The light's own picture: the cropped image drawn whole and untinted at its rectangle (reference fB:849-861, 1192-1268)
void B2SRenderer::DrawLightImage(VPXRenderContext2D* ctx, const B2SBulb& light)
{
   if (light.m_image == nullptr || light.m_pivotTexW <= 0 || light.m_pivotTexH <= 0)
      return;
   const VPXTextureInfo* const info = GetTextureInfo(light.m_image);
   if (info == nullptr)
      return;
   const float texW = static_cast<float>(light.m_pivotTexW), texH = static_cast<float>(light.m_pivotTexH);
   const float texY = static_cast<float>(info->height) - static_cast<float>(light.m_pivotTexY) - texH; // VPX measures the rectangle from the bottom
   ctx->DrawImage(ctx, light.m_image, 1.f, 1.f, 1.f, 1.f, static_cast<float>(light.m_pivotTexX), texY, texW, texH, 0.f, 0.f, 0.f, ContentX(static_cast<float>(light.m_pivotDstX)),
      ContentY(static_cast<float>(light.m_pivotDstY)), texW, texH);
}

// The main pass of a Dynamic Front light (reference fB:863-878, 934-987): while it is on, its separate layer, then for every visible image snippet
// of lower Z that it overlaps the snippet's current pixels lit by the field, composited over everything drawn so far
void B2SRenderer::RenderDynamicLight(VPXRenderContext2D* ctx, B2SBulb& light, B2SDualMode dualMode)
{
   if (light.m_dflBrightness <= 0.f)
      return; // Binary like the reference: a light that is off draws nothing
   const DynamicFront& d = light.m_dfl;
   if (d.separate)
      DrawLightImage(ctx, light);
   const int lx = light.m_pivotDstX, ly = light.m_pivotDstY, lw = light.m_pivotDstW, lh = light.m_pivotDstH;
   if (lw <= 0 || lh <= 0)
      return;
   for (const auto& entry : m_b2s->m_backglassIlluminations)
   {
      const B2SBulb& snippet = *entry;
      if (&snippet == &light || !snippet.m_isImageSnippit || snippet.m_dflPixels.empty() || snippet.m_dflFrame != m_dflFrameId)
         continue; // Not an image snippet that was drawn in this frame (hidden snippets leave no record)
      if (snippet.m_zOrder >= light.m_zOrder)
         continue; // Equal or higher: not under the light (fB:943)
      if (d.targetId != 0 && snippet.m_bulbId != d.targetId)
         continue;
      if (dualMode != B2SDualMode::Both && snippet.m_dualMode != B2SDualMode::Both && snippet.m_dualMode != dualMode)
         continue;
      const int sx = static_cast<int>(std::nearbyint(snippet.m_dflX)), sy = static_cast<int>(std::nearbyint(snippet.m_dflY));
      const int sw = snippet.m_pivotDstW, sh = snippet.m_pivotDstH;
      const int ox = std::max(lx, sx), oy = std::max(ly, sy);
      const int ow = std::min(lx + lw, sx + sw) - ox, oh = std::min(ly + lh, sy + sh) - oy;
      if (ow <= 0 || oh <= 0 || sw <= 0 || sh <= 0)
         continue;
      DynamicOverlay& pair = m_dynamicOverlays[{ &light, &snippet }];
      if (!pair.valid || pair.ox != ox || pair.oy != oy || pair.ow != ow || pair.oh != oh || pair.lx != lx || pair.ly != ly || pair.lw != lw || pair.lh != lh || pair.sx != sx
         || pair.sy != sy || pair.sw != sw || pair.sh != sh || pair.angle != snippet.m_dflAngle || pair.rotated != snippet.m_dflRotated)
      {
         const size_t count = static_cast<size_t>(ow) * static_cast<size_t>(oh);
         std::vector<uint8_t> field(count), source(count * 4), lit(count * 4);
         DynamicLight::CropField(d.field.data(), d.fieldW, d.fieldH, lw, lh, ox - lx, oy - ly, ow, oh, field.data());
         if (snippet.m_dflRotated)
         {
            const float hingeX = snippet.m_pivotAnimation ? snippet.m_pivotX : std::clamp(snippet.m_pivotX, 0.f, 1.f);
            const float hingeY = snippet.m_pivotAnimation ? snippet.m_pivotY : std::clamp(snippet.m_pivotY, 0.f, 1.f);
            DynamicLight::RotatedFrame(snippet.m_dflPixels.data(), snippet.m_pivotTexW, snippet.m_pivotTexH, static_cast<float>(snippet.m_pivotDstW),
               static_cast<float>(snippet.m_pivotDstH), sw, sh, hingeX, hingeY, snippet.m_dflAngle, ox - sx, oy - sy, ow, oh, source.data());
         }
         else
            DynamicLight::StandardFrame(snippet.m_dflPixels.data(), snippet.m_pivotTexW, snippet.m_pivotTexH, sw, sh, ox - sx, oy - sy, ow, oh, source.data());
         DynamicLight::Render(source.data(), field.data(), ow, oh, d.params, lit.data());
         pair.empty = true;
         for (size_t i = 3; i < lit.size() && pair.empty; i += 4)
            pair.empty = lit[i] == 0;
         if (!pair.empty)
            UpdateTexture(&pair.texture, ow, oh, VPXTEXFMT_sRGBA8, lit.data());
         pair.ox = ox;
         pair.oy = oy;
         pair.ow = ow;
         pair.oh = oh;
         pair.lx = lx;
         pair.ly = ly;
         pair.lw = lw;
         pair.lh = lh;
         pair.sx = sx;
         pair.sy = sy;
         pair.sw = sw;
         pair.sh = sh;
         pair.angle = snippet.m_dflAngle;
         pair.rotated = snippet.m_dflRotated;
         pair.valid = true;
      }
      if (!pair.empty && pair.texture != nullptr)
         ctx->DrawImage(ctx, pair.texture, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, static_cast<float>(ow), static_cast<float>(oh), 0.f, 0.f, 0.f, ContentX(static_cast<float>(ox)), ContentY(static_cast<float>(oy)),
            static_cast<float>(ow), static_cast<float>(oh));
   }
}

// B2S Pro reel digit with KeepDigitAspect and/or the 3D effect. The box follows the reference's integer layout (LocX - 1, box
// width rounded half to even, each box x rounded half to even, fB:3607, 3775-3777), the box texture is built on the CPU in
// authored pixels and drawn 1:1, then the matching slice of the display wide glass overlay, then (behind canvas) the canvas
// patch (B2SReelBox.vb:117-177). Returns false when the digit texture cannot be decoded, the caller then draws the plain way
bool B2SRenderer::DrawBoxedReelDigit(VPXRenderContext2D* ctx, const B2SScore& reel, int digitIndex, VPXTexture image, bool lit)
{
   const int boxW = static_cast<int>(std::nearbyint((static_cast<double>(reel.m_width) - (reel.m_digits - 1) * reel.m_spacing / 2.0) / reel.m_digits));
   const int boxH = reel.m_height;
   if (boxW <= 0 || boxH <= 0)
      return true; // The reference draws nothing in an empty box
   const int boxX = static_cast<int>(std::nearbyint(reel.m_locX - 1 + digitIndex * (boxW + reel.m_spacing / 2.0)));
   const float x = ContentX(static_cast<float>(boxX));
   const float y = ContentY(static_cast<float>(reel.m_locY));
   const VPXTexture box = GetReelBoxTexture(image, boxW, boxH, reel, lit);
   if (box == nullptr)
      return false;
   ctx->DrawImage(ctx, box, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, static_cast<float>(boxW), static_cast<float>(boxH), 0.f, 0.f, 0.f, x, y, static_cast<float>(boxW),
      static_cast<float>(boxH));
   if (reel.m_reel3DEnabled && reel.m_width > 2)
   {
      // The overlay is display wide and each digit takes its slice at its offset in the display (BRB:145-165)
      if (const VPXTexture overlay = GetReelOverlayTexture(reel, reel.m_width, boxH))
      {
         const int offset = boxX - (reel.m_locX - 1);
         const int sourceX = std::max(0, std::min(reel.m_width - 1, offset));
         const int sourceW = std::min(boxW, reel.m_width - sourceX);
         if (sourceW > 0)
            ctx->DrawImage(ctx, overlay, 1.f, 1.f, 1.f, 1.f, static_cast<float>(sourceX), 0.f, static_cast<float>(sourceW), static_cast<float>(boxH), 0.f, 0.f, 0.f,
               x, y, static_cast<float>(sourceW), static_cast<float>(boxH));
      }
   }
   if (reel.m_behindCanvas)
      DrawBackgroundPatch(ctx, static_cast<float>(boxX), static_cast<float>(reel.m_locY), static_cast<float>(boxW), static_cast<float>(boxH));
   return true;
}

void B2SRenderer::RenderScores(VPXRenderContext2D* ctx, B2SServer* server, const B2SScores& scores, float elapsed)
{
   if (server == nullptr)
      return;
   // B2SHideScoreDisplays/animations only hide LED score displays, reel displays keep rendering
   const bool ledDisplaysHidden = server->AreScoreDisplaysHidden();

   vector<SegElementType> segTypes;
   vector<float> luminances;
   vector<VPXSegDisplayRenderStyle> styles;
   vector<VPXSegDisplayHint> hints;
   m_segSources.With([&segTypes, &styles, &hints, &luminances, server](const std::vector<SegSrcId>& segDisplays){
      int digitIndex = 1;
      for (const auto& display : segDisplays)
      {
         SegDisplayFrame state = display.GetState(display.callContext);
         for (unsigned int i = 0; i < display.nElements; i++)
         {
            VPXSegDisplayHint hint = VPXSegDisplayHint::Generic;
            VPXSegDisplayRenderStyle style = VPXSegDisplayRenderStyle::VPXSegStyle_Plasma;
            if ((display.hardware & CTLPI_SEG_HARDWARE_FAMILY_MASK) == CTLPI_SEG_HARDWARE_NEON_PLASMA)
               style = VPXSegDisplayRenderStyle::VPXSegStyle_Plasma;
            else if ((display.hardware & CTLPI_SEG_HARDWARE_FAMILY_MASK) == CTLPI_SEG_HARDWARE_VFD_GREEN)
               style = VPXSegDisplayRenderStyle::VPXSegStyle_GreenVFD;
            else if ((display.hardware & CTLPI_SEG_HARDWARE_FAMILY_MASK) == CTLPI_SEG_HARDWARE_VFD_BLUE)
            {
               style = VPXSegDisplayRenderStyle::VPXSegStyle_BlueVFD;
               if (display.hardware == CTLPI_SEG_HARDWARE_GTS1_4DIGIT //
                  || display.hardware == CTLPI_SEG_HARDWARE_GTS1_6DIGIT //
                  || display.hardware == CTLPI_SEG_HARDWARE_GTS80A_7DIGIT //
                  || display.hardware == CTLPI_SEG_HARDWARE_GTS80B_20DIGIT //
               )
                  hint = VPXSegDisplayHint::Gottlieb;
            }
            segTypes.push_back(display.elementType[i]);
            styles.push_back(style);
            hints.push_back(hint);

            int bitState = 0;
            for (int j = 0; j < 16; j++)
            {
               luminances.push_back(state.frame[i * 16 + j]);
               if (state.frame[i * 16 + j] > 0.5f)
                  bitState |= 1 << j;
            }
            int ret;
            switch (bitState & ~0x80) // Remove the comma
            {
            // 7-segment stuff
            case 0x003F: ret = 0; break;
            case 0x0006: ret = 1; break;
            case 0x005B: ret = 2; break;
            case 0x004F: ret = 3; break;
            case 0x0066: ret = 4; break;
            case 0x006D: ret = 5; break;
            case 0x007D: ret = 6; break;
            case 0x0007: ret = 7; break;
            case 0x007F: ret = 8; break;
            case 0x006F: ret = 9; break;
            // Additional 10-segment stuff
            case 0x0300: ret = 1; break;
            case 0x007C: ret = 6; break;
            case 0x0067: ret = 9; break;
            // Default is empty
            default: ret = -1; break;
            }
            server->SetRomScoreDigit(digitIndex, ret);
            // Keep the raw segment mask: script digits decode to value-only, but frame-fed displays
            // need the full mask (alphanumeric glyphs do not decode to a digit). Must be applied
            // after SetRomScoreDigit which clears the mask.
            server->ApplyScoreSegments(digitIndex, bitState);
            digitIndex++;
         }
      }
   });

   // Build the per-player score strings: right aligned, space padded on the left, over all the displays of the player in both
   // windows (file order, the layout is fixed at load: B2STable::m_playerDigits and B2SScore::m_playerOffset). A display takes its
   // slice of the text at its offset, also when it is hidden. A player nobody has written yet shows nothing (reference: LEDs dark)
   ankerl::unordered_dense::map<int, string> playerScoreText;
   for (const auto& [player, digits] : m_b2s->m_playerDigits)
   {
      string& text = playerScoreText[player];
      text.assign(static_cast<size_t>(digits), ' ');
      if (!server->HasPlayerScore(player))
         continue;
      const string scoreText = std::to_string(abs(server->GetPlayerScore(player)));
      if (scoreText.length() >= text.length())
         text = scoreText.substr(scoreText.length() - text.length());
      else
         text.replace(text.length() - scoreText.length(), scoreText.length(), scoreText);
   }

   for (const auto& reel : scores.m_scores)
   {
      // Skip hidden displays (DisplayState=1), and LED displays when score displays are hidden
      if (reel.IsHidden() || (ledDisplaysHidden && (reel.m_scoreType == B2SScoreRenderer::Dream7 || reel.m_scoreType == B2SScoreRenderer::RenderedLED)))
         continue;

      // Nothing to draw, and the digit width below would divide by zero
      if (reel.m_digits == 0)
         continue;

      // Skip digits located on the grill when the grill is hidden
      if (static_cast<float>(reel.m_locY) > ctx->srcHeight)
         continue;

      // A player display shows its most recent writer: the player score (B2SSetScorePlayer), or the digits written by the script
      // (B2SSetScore/ScoreDigit/Reel/Credits). The reference has one digit store that both routes write, so the last writer wins.
      // PinMAME frames never take a player display over. A display the player already has on this start digit (offset -1) is
      // only reached through the digit calls
      bool fromDigits = reel.m_b2sPlayerNo == 0 || reel.m_playerOffset < 0;
      if (!fromDigits)
      {
         uint64_t digitStamp = 0;
         for (int i = 0; i < reel.m_digits; i++)
            digitStamp = std::max(digitStamp, server->GetScoreDigitStamp(reel.m_resolvedStartDigit + i));
         fromDigits = digitStamp > server->GetPlayerScoreStamp(reel.m_b2sPlayerNo);
      }

      // Legacy segment rendering (opt-out of the upgraded renderer): rasterize like the original
      // B2S server — one Dream7Display per LED display, or one simple LED box per digit
      if (!upgradedSegRenderingProp_Get() && (reel.m_scoreType == B2SScoreRenderer::Dream7 || reel.m_scoreType == B2SScoreRenderer::RenderedLED))
      {
         const int usedLEDType = server->GetUsedLEDType();
         const bool dream7 = (usedLEDType == 2) || (usedLEDType != 1 && reel.m_scoreType == B2SScoreRenderer::Dream7);
         RenderLegacyScoreDisplay(ctx, server, reel, dream7, segTypes, playerScoreText, fromDigits);
         continue;
      }

      const float width = (static_cast<float>(reel.m_width) - 0.5f * static_cast<float>((reel.m_digits - 1) * reel.m_spacing)) / static_cast<float>(reel.m_digits);

      // Reel rolling: resolve the rolling direction, the illuminated image selection and the per-digit
      // hold flags. A digit waits while a less significant digit has a pending 9->0 / 0->9 wrap (carry).
      const bool rollUp = scores.m_reelRollingDirection != B2SReelRollingDirection::Down;
      // ReelIlluB2SID is in lamp-id space: script B2SSetData writes and ROM lamp states share the
      // channel; the most recent change wins
      {
         const float prevIllu = reel.m_reelIllu;
         reel.m_reelIlluUpdater();
         if (server != nullptr && prevIllu >= 0.f && reel.m_reelIllu != prevIllu)
            reel.m_reelIlluStamp = server->NextStamp();
      }
      float illuState = std::max(0.f, reel.m_reelIllu);
      uint64_t illuLampStamp = 0;
      float scriptedIllu = 0.f;
      if (server != nullptr && server->GetScriptedLampState(reel.m_reelIlluB2SID, scriptedIllu, &illuLampStamp) && illuLampStamp > reel.m_reelIlluStamp)
         illuState = scriptedIllu;
      const bool illuminated = reel.m_reelIlluB2SID > 0 && (reel.m_reelIlluB2SValue > 0 ? (static_cast<int>(illuState) == reel.m_reelIlluB2SValue) : (illuState != 0.f));
      vector<char> hold(static_cast<size_t>(std::max(0, reel.m_digits)), 0);
      bool lowerWrapPending = false;
      for (int i = reel.m_digits - 1; i >= 0; i--)
      {
         hold[static_cast<size_t>(i)] = lowerWrapPending ? 1 : 0;
         if (const auto it = m_reelDigits.find(reel.m_resolvedStartDigit + i); it != m_reelDigits.end() && it->second.HasPendingWrap(rollUp))
            lowerWrapPending = true;
      }

      for (int i = 0; i < reel.m_digits; i++)
      {
         const float x = static_cast<float>(reel.m_locX) + static_cast<float>(i) * (width + 0.5f * static_cast<float>(reel.m_spacing));
         int digit = 0;
         const int index = reel.m_resolvedStartDigit + i;
         if (!fromDigits)
         {
            const string& text = playerScoreText[reel.m_b2sPlayerNo];
            const size_t offset = static_cast<size_t>(reel.m_playerOffset) + static_cast<size_t>(i);
            const char c = offset < text.length() ? text[offset] : ' ';
            const int raw = (c >= '0' && c <= '9') ? c - '0' : -1;
            digit = (reel.m_scoreType == B2SScoreRenderer::Dream7 || reel.m_scoreType == B2SScoreRenderer::RenderedLED) ? raw : (raw < 0 ? 0 : raw);
         }
         else
            digit = server->GetScoreDigit(index);

         switch (reel.m_scoreType)
         {
         case B2SScoreRenderer::LED:
            // Black Pyramid (Bally 1984)
         case B2SScoreRenderer::ImportedLED:
            // Apache! (Taito 1978)
            // LED & ImportedLED seem to be the same as reel but without animation
         case B2SScoreRenderer::Reel:
            // Volkan Steel and Metal (Original 2023), Hang Glider (Bally 1976) => Simple reels, no animations
            // ? => Simple reels with animations & sounds
            {
               int shownDigit = digit;
               int intermediate = 0;
               B2SReelDigit& st = m_reelDigits[index];
               const B2SReelImage* cur = m_b2s->m_reels.GetImage(reel.m_reelType, st.Current(), illuminated, reel.m_reelIlluImageSet);
               const int intermediates = cur ? cur->m_countOfIntermediate : 0;
               st.SetIlluminated(illuminated, intermediates);
               // Player displays are updated from decoded pinmame frames so they always roll;
               // script driven displays roll when B2SSetScore/B2SSetReel raised the roll flag.
               // LED image displays never roll (B2SReelBox::isLED).
               const bool rollRequested = server->ConsumeScoreDigitRoll(index);
               st.SetTarget(digit, reel.m_scoreType == B2SScoreRenderer::Reel && (!fromDigits || rollRequested));
               if (!hold[static_cast<size_t>(i)] && st.Update(elapsed, scores.m_reelRollingInterval, intermediates, rollUp))
               {
                  // Reel roll sound: per-digit Sound1..N attribute, or the display wide Sound attribute
                  const string& soundName
                     = (i < static_cast<int>(reel.m_soundNames.size()) && !reel.m_soundNames[static_cast<size_t>(i)].empty()) ? reel.m_soundNames[static_cast<size_t>(i)] : reel.m_soundName;
                  if (!soundName.empty() && soundName != "stille"sv)
                     server->B2SPlaySound(soundName);
               }
               shownDigit = st.Current();
               intermediate = st.Intermediate();
               const B2SReelImage* reelImage = m_b2s->m_reels.GetImage(reel.m_reelType, shownDigit, illuminated, reel.m_reelIlluImageSet);
               // B2S Pro: displays using KeepDigitAspect or the 3D effect are drawn by DrawBoxedReelDigit; the reference falls back
               // to the regular image when the illuminated one is missing (B2SReelBox.vb:94-101)
               const bool boxed = reel.m_keepDigitAspect || reel.m_reel3DEnabled;
               if (boxed && reelImage == nullptr && illuminated)
                  reelImage = m_b2s->m_reels.GetImage(reel.m_reelType, shownDigit, false, reel.m_reelIlluImageSet);
               const VPXTexture texture = (reelImage && intermediate > 0 && intermediate <= static_cast<int>(reelImage->m_intermediates.size()))
                  ? reelImage->m_intermediates[static_cast<size_t>(intermediate - 1)]
                  : nullptr;
               if (reelImage && (texture != nullptr || reelImage->m_image != nullptr))
               {
                  const VPXTexture img = texture != nullptr ? texture : reelImage->m_image;
                  // A reel with a trigger is back lit only while illuminated, one without always (BRB:125)
                  if (boxed && DrawBoxedReelDigit(ctx, reel, i, img, reel.m_reelIlluB2SID <= 0 || illuminated))
                     break;
                  const VPXTextureInfo* texInfo = GetTextureInfo(img);
                  ctx->DrawImage(ctx, img, 1.f, 1.f, 1.f, 1.f, //
                     0.f, 0.f, static_cast<float>(texInfo->width), static_cast<float>(texInfo->height), //
                     0.f, 0.f, 0.f, // No rotation
                     ContentX(x), ContentY(static_cast<float>(reel.m_locY)), width, static_cast<float>(reel.m_height));
                  // B2S Pro: a behind-canvas reel gets the matching patch of the background drawn over it, so the reel
                  // shows only through the canvas openings (reference B2SReelBox.vb:167-176)
                  if (reel.m_behindCanvas)
                     DrawBackgroundPatch(ctx, x, static_cast<float>(reel.m_locY), width, static_cast<float>(reel.m_height));
               }
            }
            break;

         case B2SScoreRenderer::Dream7:
            // Asteroid Annie (Gottlieb 1980)
         case B2SScoreRenderer::RenderedLED:
         {
            std::array<float, 16> brightness;
            // Segment layout declared by the ReelType suffix (Dream7LEDx/RenderedLEDx)
            SegElementType segType = reel.m_ledSegments == 7 ? SegElementType::CTLPI_SEG_LAYOUT_7
               : reel.m_ledSegments == 10                    ? SegElementType::CTLPI_SEG_LAYOUT_9
               : reel.m_ledSegments == 14                    ? SegElementType::CTLPI_SEG_LAYOUT_14
                                                             : SegElementType::CTLPI_SEG_LAYOUT_14;
            VPXSegDisplayRenderStyle style = VPXSegDisplayRenderStyle::VPXSegStyle_Plasma;
            VPXSegDisplayHint hint = VPXSegDisplayHint::Generic;
            if (index > 0 && index * 16 <= (int)luminances.size())
            {
               segType = segTypes[index - 1];
               style = styles[index - 1];
               hint = hints[index - 1];
               memcpy(brightness.data(), luminances.data() + (index - 1) * 16, 16 * sizeof(float));
            }
            else
            {
               // Script driven digit: an explicit B2SSetLED segment mask takes precedence,
               // otherwise the value is mapped to its standard 7 segment pattern
               const int segMask = server->GetScoreDigitSegments(index);
               static constexpr uint16_t digitSegments[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
               const uint16_t bits = segMask >= 0 ? static_cast<uint16_t>(segMask) : (digit >= 0 && digit < 10) ? digitSegments[digit] : 0;
               for (int j = 0; j < 16; j++)
                  brightness[j] = ((bits >> j) & 1) ? 1.f : 0.f;
               if (reel.m_scoreType == B2SScoreRenderer::RenderedLED)
                  style = VPXSegDisplayRenderStyle::VPXSegStyle_GenLED;
            }
            // UsedLEDType plugin setting forces Dream7 (2) or rendered LEDs (1) whatever the table declares
            const int usedLEDType = server->GetUsedLEDType();
            if (usedLEDType == 1)
               style = VPXSegDisplayRenderStyle::VPXSegStyle_GenLED;
            else if (usedLEDType == 2)
               style = VPXSegDisplayRenderStyle::VPXSegStyle_Plasma;
            // The styled profiles embed their own display color so the emitter tint must stay neutral
            // for them (it is multiplied over the profile color, tinting twice); only the generic
            // styles take the authored lit color. Same for the glass: the B2S dark color is the unlit
            // segment color, not a glass tint, and multiplied over the output it darkens the display.
            const vec4 litColor = (style == VPXSegDisplayRenderStyle::VPXSegStyle_GenPlasma || style == VPXSegDisplayRenderStyle::VPXSegStyle_GenLED)
               ? vec4(InvsRGB(reel.m_reelLitColor.x), InvsRGB(reel.m_reelLitColor.y), InvsRGB(reel.m_reelLitColor.z), 1.f)
               : vec4(1.f, 1.f, 1.f, 1.f);
            ctx->DrawSegDisplay(ctx, style, hint,
               // First layer: neutral glass, there is no glass texture
               nullptr, 1.f, 1.f, 1.f, 0.15f, // Glass texture, tint and roughness
               0.f, 0.f, 0.f, 0.f, // Glass texture coordinates (inside overall glass texture, cut for each element)
               0.f, 0.f, 0.f, // Glass lighting from room
               // Second layer: emitter tinted with the lit segment color
               segType, brightness.data(), // Segment emitter type and brightness array
               litColor.x, litColor.y, litColor.z, 1.f, 1.f, // Emitter tint, emitter brightness, emitter alpha
               0.f, 0.f, 0.f, 0.f, // Emitter padding (from glass border)
               // Render quad
               ContentX(x), m_layoutOY + m_contentH - static_cast<float>(reel.m_locY + reel.m_height), width, static_cast<float>(reel.m_height));
         }
            break;
         }
      }
   }
}


// Legacy segment rendering (UpgradedSegmentRendering opt-out), matching the original B2S server
// implementation: a Dream7Display spans the whole score display, rendered LED boxes are one box
// per digit. Both rasterize the segments into a texture which is redrawn only on changes
// Segment count of a CTLPI_SEG_LAYOUT_xxx element type, matching B2SSegmentUntranslateBitCode's 'ledSegments' values
static int SegLayoutSegments(SegElementType type)
{
   switch (type)
   {
   case CTLPI_SEG_LAYOUT_7:
   case CTLPI_SEG_LAYOUT_7C:
   case CTLPI_SEG_LAYOUT_7D: return 7;
   case CTLPI_SEG_LAYOUT_9:
   case CTLPI_SEG_LAYOUT_9C: return 10;
   case CTLPI_SEG_LAYOUT_16: return 16;
   default: return 14;
   }
}

void B2SRenderer::RenderLegacyScoreDisplay(VPXRenderContext2D* ctx, B2SServer* server, const B2SScore& reel, bool dream7, const std::vector<SegElementType>& segTypes,
   const ankerl::unordered_dense::map<int, string>& playerScoreText, bool fromDigits)
{
   LegacySegDisplay& state = m_legacySegDisplays[&reel];
   if (!state.built || state.dream7 != dream7)
   {
      // (Re)build the display, also on a UsedLEDType mode change
      state = LegacySegDisplay {};
      state.dream7 = dream7;
      const uint8_t litR = static_cast<uint8_t>(std::clamp(reel.m_reelLitColor.x * 255.f + 0.5f, 0.f, 255.f));
      const uint8_t litG = static_cast<uint8_t>(std::clamp(reel.m_reelLitColor.y * 255.f + 0.5f, 0.f, 255.f));
      const uint8_t litB = static_cast<uint8_t>(std::clamp(reel.m_reelLitColor.z * 255.f + 0.5f, 0.f, 255.f));
      const uint8_t darkR = static_cast<uint8_t>(std::clamp(reel.m_reelDarkColor.x * 255.f + 0.5f, 0.f, 255.f));
      const uint8_t darkG = static_cast<uint8_t>(std::clamp(reel.m_reelDarkColor.y * 255.f + 0.5f, 0.f, 255.f));
      const uint8_t darkB = static_cast<uint8_t>(std::clamp(reel.m_reelDarkColor.z * 255.f + 0.5f, 0.f, 255.f));
      const uint32_t litColor = seg::RGB(litR, litG, litB);
      const uint32_t darkColor = seg::RGB(darkR, darkG, darkB);
      if (dream7)
      {
         auto d7 = std::make_unique<seg::Dream7Display>(m_vpxApi);
         d7->SetRect({ reel.m_locX - 1, reel.m_locY, reel.m_width, reel.m_height });
         d7->SetType(reel.m_ledSegments == 10 ? seg::SegmentNumberType_TenSegment : reel.m_ledSegments == 14 ? seg::SegmentNumberType_FourteenSegment : seg::SegmentNumberType_SevenSegment);
         d7->SetScaleMode(seg::ScaleMode_Stretch);
         d7->SetDigits(reel.m_digits);
         d7->SetSpacing(static_cast<float>(reel.m_spacing) * 5.f);
         // Reference color wiring: the glass colors derive from the authored lit segment color
         d7->SetLightColor(litColor);
         d7->SetGlassColor(seg::RGB(static_cast<uint8_t>(std::min(255, litR + 50)), static_cast<uint8_t>(std::min(255, litG + 50)), static_cast<uint8_t>(std::min(255, litB + 50))));
         d7->SetGlassColorCenter(seg::RGB(static_cast<uint8_t>(std::min(255, litR + 70)), static_cast<uint8_t>(std::min(255, litG + 70)), static_cast<uint8_t>(std::min(255, litB + 70))));
         d7->SetOffColor(darkColor);
         d7->SetGlassAlpha(140);
         d7->SetGlassAlphaCenter(255);
         d7->SetThickness(static_cast<float>(reel.m_thickness) / 100.f * 1.2f);
         d7->SetShear(static_cast<float>(reel.m_shear) / 100.f);
         d7->SetGlow(static_cast<float>(std::lround(static_cast<float>(reel.m_glow) / 100.f)));
         // 'Taxi' patch to shear the third LED display (reference FormBackglass)
         if (reel.m_id == 3 && string_to_lower(m_b2s->m_name) == "taxi"s)
         {
            d7->SetAngle(4.f);
            d7->SetShear(d7->GetShear() / 2.f);
         }
         state.display = std::move(d7);
      }
      else
      {
         const int width = static_cast<int>(std::lround((reel.m_width - (reel.m_digits - 1) * reel.m_spacing / 2.f) / reel.m_digits));
         state.boxes.reserve(static_cast<size_t>(reel.m_digits));
         for (int i = 0; i < reel.m_digits; i++)
         {
            auto box = std::make_unique<seg::LEDBox>(m_vpxApi);
            const int x = static_cast<int>(std::lround((reel.m_locX - 1) + i * (width + reel.m_spacing / 2.f)));
            box->SetRect({ x, reel.m_locY, width, reel.m_height });
            box->SetLEDType(reel.m_ledSegments == 10 ? seg::eLEDType_LED10 : reel.m_ledSegments == 14 ? seg::eLEDType_LED14 : seg::eLEDType_LED8);
            box->SetLitLEDSegmentColor(litColor);
            box->SetDarkLEDSegmentColor(darkColor);
            state.boxes.push_back(std::move(box));
         }
      }
      state.built = true;
   }

   // Feed the digits: an explicit script segment mask (B2SSetLED/B2SSetLEDDisplay, untranslated
   // back to the Dream7 bit order these renderers expect) wins, else the digit glyph
   for (int i = 0; i < reel.m_digits; i++)
   {
      const int index = reel.m_resolvedStartDigit + i;
      string text;
      if (!fromDigits)
      {
         const auto it = playerScoreText.find(reel.m_b2sPlayerNo);
         const size_t offset = static_cast<size_t>(reel.m_playerOffset) + static_cast<size_t>(i);
         text = string(1, (it != playerScoreText.end() && offset < it->second.length()) ? it->second[offset] : ' ');
      }
      else
      {
         // TODO As the original B2SSetScoreDigit: decimal text, of which only the first character is drawn (10-15 show '1').
         // TODO Unlike the original, a digit the script never set shows '0' (the original left it blank)
         const int value = server->GetScoreDigit(index);
         text = value < 0 ? " "s : std::to_string(value); // -1 is a blank digit: the leading padding of B2SSetScore on an LED display
      }
      const int segMask = server->GetScoreDigitSegments(index);
      // Source segment layout for the mask untranslation: the PinMAME frame element's own layout
      // when the digit is frame-fed (it may differ from the display's declared type, e.g. a Williams
      // 16 segment alphanum source driving a Dream7LED14 display), else the declared segment count.
      const int srcSegs = (index > 0 && index <= static_cast<int>(segTypes.size())) ? SegLayoutSegments(segTypes[static_cast<size_t>(index - 1)]) : reel.m_ledSegments;
      if (dream7)
      {
         if (segMask >= 0)
            state.display->SetValue(i, B2SSegmentUntranslateBitCode(static_cast<uint32_t>(segMask), srcSegs));
         else
            state.display->SetValue(i, text);
      }
      else if (i < static_cast<int>(state.boxes.size()))
      {
         if (segMask >= 0)
            state.boxes[static_cast<size_t>(i)]->SetValue(B2SSegmentUntranslateBitCode(static_cast<uint32_t>(segMask), srcSegs));
         else
            state.boxes[static_cast<size_t>(i)]->SetText(text);
      }
   }
   if (dream7)
   {
      state.display->SetViewOffset(m_layoutOX, m_layoutOY); // Placed with the KeepAspectRatio placement
      state.display->Render(ctx);
   }
   else
      for (auto& box : state.boxes)
      {
         box->SetViewOffset(m_layoutOX, m_layoutOY);
         box->Render(ctx);
      }
}


bool B2SRenderer::RenderBackglass(VPXRenderContext2D* ctx, B2SServer* server)
{
   // Update to latest settings state
   m_grillCut = showGrillProp_Get() ? 0.f : static_cast<float>(m_b2s->m_grillHeight);

   const VPXTextureInfo* bgTexInfo = nullptr;
   if (m_b2s->m_backglassOffImage.m_image)
      bgTexInfo = GetTextureInfo(m_b2s->m_backglassOffImage.m_image);
   else if (m_b2s->m_backglassImage.m_image)
      bgTexInfo = GetTextureInfo(m_b2s->m_backglassImage.m_image);
   m_b2sWidth = bgTexInfo ? static_cast<float>(bgTexInfo->width) : 1024.f;
   m_b2sHeight = (bgTexInfo ? static_cast<float>(bgTexInfo->height) : 768.f) - m_grillCut;

   FitSurfaceToWindow(ctx, m_b2sWidth, m_b2sHeight);

   // Update animations
   auto now = std::chrono::steady_clock::now();
   float elapsed = static_cast<float>(static_cast<double>((now - m_lastBackglassRenderTick).count()) / 1000000000.0);
   m_lastBackglassRenderTick = now;
   const B2SDualMode dualMode = ActiveDualMode();
   m_motionServer = server;
   m_pulseServer = server;
   DispatchScriptTriggers(server);
   if (m_b2s->m_hasDynamicFront)
      ++m_dflFrameId; // Image snippets drawn from here on belong to this frame
   UpdateMotion(ctx, false);
   UpdatePhysics(ctx, false);
   DispatchNamedMoves(server);
   UpdateAnimations(m_b2s->m_backglassAnimations, elapsed, server, dualMode);

   // B2S Pro: transparent canvas pixels composite over the authored backing color instead of black (reference fB:617-627).
   // The color is stored in a 1x1 texture so it is drawn untinted, exactly as authored
   if (m_b2s->m_canvasBacking.has_value())
   {
      if (m_canvasBackingTex == nullptr)
      {
         const vec4& color = *m_b2s->m_canvasBacking;
         const uint8_t pixel[4] = { static_cast<uint8_t>(std::lround(color.x * 255.f)), static_cast<uint8_t>(std::lround(color.y * 255.f)), static_cast<uint8_t>(std::lround(color.z * 255.f)), 255 };
         UpdateTexture(&m_canvasBackingTex, 1, 1, VPXTEXFMT_sRGBA8, pixel);
      }
      ctx->DrawImage(ctx, m_canvasBackingTex, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, // No rotation
         0.f, 0.f, ctx->srcWidth, ctx->srcHeight); // The whole padded surface, so the border area takes the backing color
   }

   // B2S Pro: behind-canvas snippets are drawn first and the stationary backglass is composited over them, so its
   // transparent openings are the only places where they show (reference fB:643-650)
   RenderBulbs(ctx, server, m_b2s->m_backglassIlluminations, elapsed, dualMode, true);

   // Draw background
   B2SImage& onImage = m_b2s->m_backglassOnImage;
   {
      const float prev = onImage.m_romProbe;
      onImage.m_romUpdater();
      if (server != nullptr && prev >= 0.f && onImage.m_romProbe != prev)
         onImage.m_romStamp = server->NextStamp();
   }
   // The lit background is also driven by script writes on its lamp channel (reference MyB2SSetData):
   // the most recent write between script and ROM wins, RomInverted is never applied to this channel
   uint64_t onLampStamp = 0;
   float onLampState = 0.f;
   if (server != nullptr && onImage.m_romIdType == B2SRomIDType::Lamp && onImage.m_romId > 0 && server->GetScriptedLampState(onImage.m_romId, onLampState, &onLampStamp)
      && onLampStamp > onImage.m_romStamp)
      onImage.m_brightness = (onLampState != 0.f) ? 1.f : 0.f;
   else if (onImage.m_romProbe >= 0.f)
      onImage.m_brightness = onImage.m_romProbe;

   if (onImage.m_image == nullptr || onImage.m_brightness < 1.f)
   {
      // The unlit variant is the OffImage when the file provides one (the reference's DarkImage)
      const B2SImage& darkImage = m_b2s->m_backglassOffImage.m_image ? m_b2s->m_backglassOffImage : m_b2s->m_backglassImage;
      if (darkImage.m_image)
      {
         const VPXTextureInfo* texInfo = GetTextureInfo(darkImage.m_image);
         ctx->DrawImage(ctx, darkImage.m_image, 1.f, 1.f, 1.f, 1.f, 0.f, m_grillCut, static_cast<float>(texInfo->width), static_cast<float>(texInfo->height) - m_grillCut, 0.f, 0.f,
            0.f, // No rotation
            ContentX(0.f), m_layoutOY, static_cast<float>(texInfo->width), static_cast<float>(texInfo->height) - m_grillCut);
      }
   }
   if (onImage.m_image)
   {
      const VPXTextureInfo* texInfo = GetTextureInfo(onImage.m_image);
      ctx->DrawImage(ctx, onImage.m_image, 1.f, 1.f, 1.f, onImage.m_brightness, 0.f, m_grillCut, static_cast<float>(texInfo->width), static_cast<float>(texInfo->height) - m_grillCut, 0.f,
         0.f, 0.f, // No rotation
         ContentX(0.f), m_layoutOY, static_cast<float>(texInfo->width), static_cast<float>(texInfo->height) - m_grillCut);
   }

   // Draw illuminations, scores and DMD overlays. B2S Pro Dynamic Front Lighting: the lights' base images go right after the background, below
   // every bulb (reference fB:655-685), so the lights get their own pass first and the main pass then reuses their brightness
   if (m_b2s->m_hasDynamicFront)
   {
      m_dflPhase = 1;
      RenderBulbs(ctx, server, m_b2s->m_backglassIlluminations, elapsed, dualMode, false);
      m_dflPhase = 2;
      RenderBulbs(ctx, server, m_b2s->m_backglassIlluminations, elapsed, dualMode, false);
      m_dflPhase = 0;
   }
   else
      RenderBulbs(ctx, server, m_b2s->m_backglassIlluminations, elapsed, dualMode, false);
   RenderScores(ctx, server, m_b2s->m_backglassScores, elapsed);
   const VPXTexture backImage = m_b2s->m_backglassImage.m_image ? m_b2s->m_backglassImage.m_image
      : m_b2s->m_backglassOffImage.m_image                      ? m_b2s->m_backglassOffImage.m_image
                                                                : m_b2s->m_backglassOnImage.m_image;
   RenderDisplays(ctx, server, 0, backImage);

   RenderAspectPadding(ctx);

   return true;
}

void B2SRenderer::DrawBackgroundPatch(VPXRenderContext2D* ctx, float x, float y, float width, float height)
{
   if (ctx->window == VPXWindowId::VPXWINDOW_ScoreView)
   {
      if (m_b2s->m_dmdImage.m_image)
         if (const VPXTextureInfo* const info = GetTextureInfo(m_b2s->m_dmdImage.m_image))
            ctx->DrawImage(ctx, m_b2s->m_dmdImage.m_image, 1.f, 1.f, 1.f, 1.f, x, static_cast<float>(info->height) - y - height, width, height, 0.f, 0.f, 0.f, ContentX(x), ContentY(y), width, height);
      return;
   }
   // The window shows the unlit (OffImage or backglass) image with the lit image blended over it, the hidden grill being
   // the bottom rows: the same two layers, restricted to the rectangle, give the patch. VPX measures the texture rectangle of
   // DrawImage from the bottom of the texture
   const auto drawPatch = [ctx, x, y, width, height, this](VPXTexture texture, float alpha)
   {
      const VPXTextureInfo* const info = GetTextureInfo(texture);
      if (info == nullptr)
         return;
      ctx->DrawImage(ctx, texture, 1.f, 1.f, 1.f, alpha, x, static_cast<float>(info->height) - y - height, width, height, 0.f, 0.f, 0.f, ContentX(x), ContentY(y), width, height);
   };
   const B2SImage& onImage = m_b2s->m_backglassOnImage;
   if (onImage.m_image == nullptr || onImage.m_brightness < 1.f)
   {
      const B2SImage& darkImage = m_b2s->m_backglassOffImage.m_image ? m_b2s->m_backglassOffImage : m_b2s->m_backglassImage;
      if (darkImage.m_image)
         drawPatch(darkImage.m_image, 1.f);
   }
   if (onImage.m_image)
      drawPatch(onImage.m_image, onImage.m_brightness);
}

bool B2SRenderer::RenderScoreView(VPXRenderContext2D* ctx, B2SServer* server)
{
   if (m_b2s->m_dmdImage.m_image == nullptr && m_b2s->m_dmdIlluminations.empty())
      return false;

   // Update to latest settings state
   FitSurfaceToWindow(ctx, m_dmdWidth, m_dmdHeight);

   // Update animations
   auto now = std::chrono::steady_clock::now();
   float elapsed = static_cast<float>(static_cast<double>((now - m_lastDmdRenderTick).count()) / 1000000000.0);
   m_lastDmdRenderTick = now;
   m_motionServer = server;
   m_pulseServer = server;
   DispatchScriptTriggers(server);
   UpdateMotion(ctx, true);
   UpdatePhysics(ctx, true);
   DispatchNamedMoves(server);
   UpdateAnimations(m_b2s->m_dmdAnimations, elapsed, server, ActiveDualMode());
   // B2S Pro: behind-canvas snippets first, the background image is then composited over them (reference formDMD.vb:77-98)
   RenderBulbs(ctx, server, m_b2s->m_dmdIlluminations, elapsed, B2SDualMode::Both, true);
   // Draw background
   if (m_b2s->m_dmdImage.m_image)
      ctx->DrawImage(ctx, m_b2s->m_dmdImage.m_image, 1.f, 1.f, 1.f, 1.f,
         0.f, 0.f, m_dmdWidth, m_dmdHeight,
         0.f, 0.f, 0.f, // No rotation
         ContentX(0.f), m_layoutOY, m_dmdWidth, m_dmdHeight);

   // Draw illuminations, scores and DMD overlays
   RenderBulbs(ctx, server, m_b2s->m_dmdIlluminations, elapsed, B2SDualMode::Both, false); // Reference does not dual-filter score view bulbs
   RenderScores(ctx, server, m_b2s->m_dmdScores, elapsed);
   RenderDisplays(ctx, server, 1, m_b2s->m_dmdImage.m_image);

   RenderAspectPadding(ctx);

   return true;
}

// Renders the <DMD> displays of the document targeted at this view, growing the overlay pool on
// demand (a display presence in the document means enabled). Each display keeps the overlay of its
// document index whatever its target, so that the overlay state (glass texture, uploaded frame,
// bounds search) is not shared between the backglass and score view displays
void B2SRenderer::RenderDisplays(VPXRenderContext2D* ctx, B2SServer* server, int target, VPXTexture backImage)
{
   vector<B2SDMDDisplay>& displays = server->GetOverlayDoc().displays;
   for (size_t i = 0; i < displays.size(); i++)
   {
      if (displays[i].target != target)
         continue;
      while (i >= m_dmdOverlays.size())
         m_dmdOverlays.emplace_back(std::make_unique<DMDOverlay::Renderer>(m_vpxApi, m_resURIResolver));
      m_dmdOverlays[i]->SetViewOffset(m_layoutOX, m_layoutOY); // Placed with the KeepAspectRatio placement
      m_dmdOverlays[i]->Render(ctx, displays[i], backImage);
   }
}
}
