// license:GPLv3+

#pragma once

#include "common.h"
#include "B2SDataModel.h"
#include "Reel3D.h"
#include "Motion.h"
#include "Physics.h"
#include "DMDOverlay/DMDOverlay.h"
#include "seg/Dream7Display.h"
#include "seg/LEDBox.h"

#include "plugins/ControllerPlugin.h"
#include "plugins/ResURIResolver.h"
#include "plugins/VPXPlugin.h"

#include <future>
#include <chrono>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_dense.h>

namespace B2S
{

class B2SServer;

class B2SRenderer final
{
public:
   B2SRenderer(const MsgPluginAPI* const msgApi, const VPXPluginAPI* const vpxApi, const unsigned int endpointId, std::shared_ptr<B2STable> b2s);
   ~B2SRenderer();

   static void RegisterSettings(const MsgPluginAPI* const msgApi, unsigned int endpointId);
   // Fills the renderer owned entries (ShowGrill, DualMode, UpgradedSegmentRendering, KeepAspectRatio) of the plugin
   // declared "settings" sheet, returns the number of items filled
   static int FillSettingsItems(PagedUIItemDef* items);

   bool Render(VPXRenderContext2D* context, class B2SServer* server);

   // Fit transform of the current view (see SetViewTransform). Every source rectangle of a drawing call goes
   // through it: x' = m_ox + x * m_rx, y' = m_oy + y * m_ry. It is the identity when KeepAspectRatio is off
   struct ViewTransform
   {
      float m_ox = 0.f, m_oy = 0.f; // translation, in source units
      float m_rx = 1.f, m_ry = 1.f; // scale, source units per source unit of the unstretched view
   };

private:
   std::function<void()> ResolveRomPropUpdater(const std::vector<StateSrcId>& items, float* value, const B2SRomIDType romIdType, const int romId, const bool romInverted = false, const int romValue = 0, const bool holdSwitchOn = false) const;
   bool RenderBackglass(VPXRenderContext2D* context, class B2SServer* server);
   bool RenderScoreView(VPXRenderContext2D* context, class B2SServer* server);
   void RenderDisplays(VPXRenderContext2D* ctx, B2SServer* server, int target, VPXTexture backImage);
   // behindCanvas selects the pass: bulbs flagged SnippitBehindCanvas are drawn before the background image, the others after it
   void RenderBulbs(VPXRenderContext2D* ctx, const B2SServer* server, const vector<std::unique_ptr<B2SBulb>>& bulbs, float elapsed, B2SDualMode dualMode, bool behindCanvas);
   // Draws the part of the current background image (as the window shows it) under a rectangle, in output pixels
   void DrawBackgroundPatch(VPXRenderContext2D* ctx, float x, float y, float width, float height);
   bool DrawBoxedReelDigit(VPXRenderContext2D* ctx, const B2SScore& reel, int digitIndex, VPXTexture image, bool lit);
   // B2S Pro motion paths: the group and route movers (Motion.h) run once per render of either window, their ROM triggers are polled here
   void BuildMotion();
   double MotionNowMs() const;
   void UpdateMotion(VPXRenderContext2D* ctx, bool dmd);
   void RenderMotionBulb(VPXRenderContext2D* ctx, const B2SBulb& bulb, float alpha);
   // B2S Pro physics balls: the simulation (Physics.h) runs on its own 1 ms clock, advanced once per render of either window
   void BuildPhysics();
   double PhysicsNowSeconds() const;
   void UpdatePhysics(VPXRenderContext2D* ctx, bool dmd);
   void RenderPhysicsBall(VPXRenderContext2D* ctx, const B2SServer* server, B2SBulb& bulb);
   void RenderPivot(VPXRenderContext2D* ctx, const B2SServer* server, B2SBulb& bulb);
   void RenderNativeRotation(VPXRenderContext2D* ctx, const B2SServer* server, B2SBulb& bulb);
   // B2S Pro Dynamic Front Lighting (reference fB:655-685, 863-987): the base image of a light, then its layer and the overlays of the snippets under it
   void NoteSnippetDrawn(B2SBulb& bulb, float brightness, bool rotated);
   void DrawLightImage(VPXRenderContext2D* ctx, const B2SBulb& light);
   void RenderDynamicLight(VPXRenderContext2D* ctx, B2SBulb& light, B2SDualMode dualMode);
   void UpdatePositionGrid(B2SPositionGrid& grid, B2SBulb& bulb, float scaleX, float scaleY, const B2SServer* server);
   void DispatchNamedMoves(B2SServer* server);
   void RenderScores(VPXRenderContext2D* ctx, B2SServer* server, const B2SScores& scores, float elapsed);
   void RenderLegacyScoreDisplay(VPXRenderContext2D* ctx, B2SServer* server, const B2SScore& reel, bool dream7, const std::vector<SegElementType>& segTypes,
      const ankerl::unordered_dense::map<int, string>& playerScoreText, bool fromDigits);
   void UpdateAnimations(vector<B2SAnimation>& animations, float elapsed, B2SServer* server, B2SDualMode dualMode);
   void RenderAspectBars(VPXRenderContext2D* ctx); // Black borders in the area left over by the fit transform (kept clear by it)
   // Fit the whole view into the window without distorting it: the authored surface (refWidth x refHeight, in the
   // source units of the view) is scaled uniformly to the largest rectangle of that shape that fits the window,
   // centered, and the drawing is transformed into it, so nothing is cut and the leftover is left to the borders
   void SetViewTransform(VPXRenderContext2D* ctx, float refWidth, float refHeight);
   B2SDualMode ActiveDualMode() const;
   void OnRandomAnimationTrigger(vector<B2SAnimation>& animations, B2SRomIDType romIdType, int romId, bool start);
   void DispatchScriptTriggers(B2SServer* server);

   std::shared_ptr<B2STable> m_b2s;

   const MsgPluginAPI* const m_msgApi;
   const VPXPluginAPI* const m_vpxApi;
   const unsigned int m_endpointId;
   PinballPlugin::Controller::CtrlItemConsumer<ControllerDef> m_pinmameControllers;
   mutable PinballPlugin::Controller::CtrlItemConsumer<StateSrcId> m_stateSources;
   void OnStateSrcChanged(const std::vector<StateSrcId>& items);
   mutable PinballPlugin::Controller::CtrlItemConsumer<SegSrcId> m_segSources;

   PinballPlugin::ResURIResolver m_resURIResolver;
   vector<std::unique_ptr<DMDOverlay::Renderer>> m_dmdOverlays; // One per <DMD> display of the edited document, by document index

   std::chrono::time_point<std::chrono::steady_clock> m_lastBackglassRenderTick;
   std::chrono::time_point<std::chrono::steady_clock> m_lastDmdRenderTick;

   B2SAnimation* m_lastRandomAnimation = nullptr; // Last animation started through a random trigger

   std::map<int, B2SReelDigit> m_reelDigits; // Rolling reel digit states, keyed by resolved digit index

   // Legacy segment renderer (opt-out of the upgraded segment rendering): one rasterized display per score
   // display, either a single multi-digit Dream7Display or one LEDBox per digit, keyed by the score element
   struct LegacySegDisplay
   {
      bool dream7 = false;
      bool built = false;
      std::unique_ptr<seg::Dream7Display> display;
      vector<std::unique_ptr<seg::LEDBox>> boxes;
   };
   std::map<const B2SScore*, LegacySegDisplay> m_legacySegDisplays;

   VPXTexture m_canvasBackingTex = nullptr; // B2S Pro canvas backing color (1x1), created on first use

   ViewTransform m_xf; // Transform of the view being rendered, applied by the drawing helpers
   float m_xfSx = 1.f; // Window pixels per authored pixel of the transformed view (x), what the motion, physics and grid scales take
   float m_xfSy = 1.f; // Idem (y)
   float m_xfBarX = 0.f; // Border width per side, in source units (0 when the window is the narrow side or the feature is off)
   float m_xfBarY = 0.f; // Border height per side, in source units

   // B2S Pro motion paths
   struct MotionProbe
   {
      Motion::Source source;
      int id;
      float value = -1.f; // Polled ROM state, -1 until observed
      std::function<void()> updater = []() { };
   };
   std::unique_ptr<Motion::System> m_motion;
   vector<B2SBulb*> m_motionBulbs; // By motion item
   vector<MotionProbe> m_motionProbes;
   std::mutex m_motionMutex; // The probes are rebuilt when the ROM sources change
   const B2SServer* m_motionServer = nullptr; // The write counter that orders motion events against script and ROM writes
   uint64_t m_motionLocalStamp = 0;
   std::chrono::steady_clock::time_point m_motionEpoch = std::chrono::steady_clock::now();

   // B2S Pro physics balls: the ball is composed on the CPU (artwork rolled and sized to its aspect, then clipped by the selection mask,
   // reference CreatePhysicsMaskedFrame fB:1060-1112) into an authored resolution texture drawn 1:1 at its bounding box
   struct PhysicsBallFrame
   {
      VPXTexture texture = nullptr;
      std::vector<uint8_t> source; // The ball artwork, RGBA
      int sourceW = 0, sourceH = 0;
      std::vector<uint8_t> pixels; // The composed frame
      int left = 0, top = 0, w = 0, h = 0;
      float cx = 0.f, cy = 0.f, roll = 0.f, sx = 0.f, sy = 0.f; // The pose the frame was composed for
      bool valid = false;
   };
   struct PhysicsProbe
   {
      Motion::Source source;
      int id;
      float value = -1.f; // Polled ROM state, -1 until observed
      std::function<void()> updater = []() { };
   };
   std::unique_ptr<Physics::System> m_physics;
   vector<B2SBulb*> m_physicsPivots; // By physics pivot index
   vector<PhysicsProbe> m_physicsProbes;
   std::map<const B2SBulb*, PhysicsBallFrame> m_physicsFrames;
   B2SServer* m_pulseServer = nullptr; // Switch zones write back to the game through it

   // B2S Pro Dynamic Front Lighting: the overlay of a (light, snippet) pair is recomputed only when its geometry changes (the light being on
   // does not enter it, reference section 3.6 of the step 9 spec); it is a CPU built texture the size of the overlap, drawn 1:1
   struct DynamicOverlay
   {
      VPXTexture texture = nullptr;
      int ox = 0, oy = 0, ow = 0, oh = 0; // The overlap on the backglass
      int lx = 0, ly = 0, lw = 0, lh = 0; // The light's rounded rectangle
      int sx = 0, sy = 0, sw = 0, sh = 0; // The snippet's rounded rectangle
      float angle = 0.f;
      bool rotated = false;
      bool empty = false; // Nothing is lit in the overlap
      bool valid = false;
   };
   std::map<std::pair<const B2SBulb*, const B2SBulb*>, DynamicOverlay> m_dynamicOverlays;
   uint64_t m_dflFrameId = 0; // Counts the renders of the backglass; a snippet drawn in the current one has this stamp
   int m_dflPhase = 0; // 1: the base pass of the lights, 2: the main pass with the lights' brightness already known

   // B2S Pro reel displays with KeepDigitAspect or 3D: digit images decoded once, then CPU built box and glass overlay textures
   // (reference ReelImageSizing.vb, Reel3DEffect.vb), cached per source image and settings
   struct ReelTextureKey
   {
      VPXTexture source;
      int w, h, digitWidth;
      bool aspect, effect3d;
      int brightness, temperature, depth;
      bool operator<(const ReelTextureKey& o) const
      {
         return std::tie(source, w, h, digitWidth, aspect, effect3d, brightness, temperature, depth)
            < std::tie(o.source, o.w, o.h, o.digitWidth, o.aspect, o.effect3d, o.brightness, o.temperature, o.depth);
      }
   };
   struct OverlayKey
   {
      int w, h, depth, glass;
      bool operator<(const OverlayKey& o) const { return std::tie(w, h, depth, glass) < std::tie(o.w, o.h, o.depth, o.glass); }
   };
   std::map<VPXTexture, Reel3D::Image> m_reelSources;
   std::map<ReelTextureKey, VPXTexture> m_reelBoxTextures;
   std::map<OverlayKey, VPXTexture> m_reelOverlayTextures;
   const Reel3D::Image* GetReelSource(VPXTexture texture);
   VPXTexture GetReelBoxTexture(VPXTexture source, int boxW, int boxH, const B2SScore& reel, bool lit);
   VPXTexture GetReelOverlayTexture(const B2SScore& reel, int overlayW, int overlayH);

   bool m_showGrill = false;
   float m_b2sWidth = 0.f;
   float m_b2sHeight = 0.f;
   float m_dmdWidth = 0.f;
   float m_dmdHeight = 0.f;
   float m_grillCut = 0.f;

   VPXTexture m_blackTexture = nullptr; // 1x1 opaque black, KeepAspectRatio borders
};

}
