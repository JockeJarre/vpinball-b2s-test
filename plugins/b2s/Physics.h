// license:GPLv3+
//
// B2S Pro physics balls (Physics* attributes of image snippits), free of any VPX dependency: the simulation of one ball (reference
// src/shared/PhysicsBallState.vb, PBS), the shared 1 ms physics clock and the physics pivot (the flipper, B2SPictureBox.vb PB:611-659,
// B2SData.vb BD:336-390, 465-528). Everything runs in authored backglass pixels with 32 bit floats like the reference (VB Single);
// the window scale only enters when a ball is presented and rolled.

#pragma once

#include "Motion.h"

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace B2S::Physics
{

struct Vec2
{
   float x = 0.f;
   float y = 0.f;
};

struct RectF
{
   float x = 0.f; // left
   float y = 0.f; // top
   float w = 0.f;
   float h = 0.f;
   bool IsEmpty() const { return w <= 0.f || h <= 0.f; }
};

struct Zone
{
   RectF bounds;
   int switchId = 0;
   float angle = 0.f; // degrees about the zone centre, clockwise, (-180, 180]
};

struct Launcher
{
   bool enabled = false;
   bool followPivot = false;
   int triggerType = 1; // 2 lamp, 3 B2S id, 5 switch, anything else solenoid
   int triggerId = 0;
   Vec2 origin;
   float angle = -90.f; // degrees, y down: -90 is straight up
   float strength = 900.f; // px/s
   float randomAngle = 0.f;
   float randomStrength = 0.f; // percent
   float captureRadius = 45.f;
};

// What a bulb's XML says (reference fB:3151-3226, BD:465-497)
struct Config
{
   bool ball = false; // PhysicsBall="1"
   RectF bounds; // empty bounds: the bulb is not simulated
   float gravity = 650.f; // px/s^2, 0..10000
   float flipperStrength = 1.f; // 0..5
   float boundaryBounce = 0.12f; // 0..1
   std::string flipperName; // trimmed
   std::vector<std::vector<Vec2>> paths; // open polylines
   std::vector<std::vector<float>> segmentBounces; // per path, per segment: -1 = the global bounce
   std::vector<RectF> obstacles;
   std::vector<float> obstacleBounces; // index aligned with the parsed obstacles, -1 = the global bounce
   std::vector<Zone> zones;
   Launcher launcher;
   bool rollEnabled = false; // MotionPathRollEnabled: the presented ball rolls
   bool gravityDrop = false; // MotionPathGravityDrop: the ball starts inactive, without flipper or launcher (step 7 link, not ported)

   bool Simulated() const { return ball && !bounds.IsEmpty(); }
};

// Attribute text parsers (reference fB:4587-4719): an unparsable list item is skipped (or -1), never 0
RectF ParseBounds(const std::string& text);
std::vector<std::vector<Vec2>> ParsePaths(const std::string& text);
std::vector<std::vector<float>> ParseSegmentBounces(const std::string& text);
std::vector<RectF> ParseObstacles(const std::string& text);
std::vector<float> ParseObstacleBounces(const std::string& text);
std::vector<Zone> ParseZones(const std::string& text);
void ParseZoneAngles(const std::string& text, std::vector<Zone>& zones);
float NormalizeAngle(float angle);

// A pivot bulb that a ball names as its flipper: the same data as a normal pivot, advanced on the physics clock
struct PivotParams
{
   std::string name; // trimmed
   RectF rect; // alpha cropped rectangle in authored pixels
   float pivotX = 0.5f; // hinge, fractions of the rectangle (unclamped)
   float pivotY = 0.5f;
   float tipX = 0.9f; // flipper tip at angle 0, clamped fractions
   float tipY = 0.5f;
   float randomStrength = 0.f; // percent, 0..100
   bool automatic = false; // swings up and down while triggered
   float upAngle = -30.f;
   float downAngle = 0.f;
   int moveDuration = 80; // ms
   float initialAngle = 0.f; // the pose at load: the down angle, or 0 for an automatic swing
};

class System
{
public:
   System();

   // The random source of the scatter, the flipper variation and the launcher: NextDouble() in [0, 1)
   void SetRandom(std::function<double()> next) { m_random = std::move(next); }
   // Called when a ball centre enters a switch zone
   void SetPulseSwitch(std::function<void(int)> pulse) { m_pulseSwitch = std::move(pulse); }

   // authored is the bulb rectangle (Left, Top, Width, Height); returns the ball index
   int AddBall(const Config& config, RectF authored, bool dmd);
   int AddPivot(const PivotParams& params);
   // Resolves the flipper names to pivots (case insensitive, trimmed)
   void Build();

   // The scale (output pixels per authored pixel) of a window: a ball waits for it, like the reference waits for its screen rectangle
   void SetScale(bool dmd, float sx, float sy);
   // Brings the clock to the given time: whole 1 ms substeps, the remainder carried over, then every ball is presented once
   void Advance(double nowSeconds);
   // The same, before a pivot starts a swing or a launcher fires, so that the event applies at the physics time it happened
   void Synchronize(double nowSeconds) { Advance(nowSeconds); }

   // Pivot control
   void StartPivotSwing(int pivot, float angle, int durationMs, double nowSeconds);
   void SetPivotAutomaticActive(int pivot, bool active);
   float PivotAngle(int pivot) const { return m_pivots[static_cast<size_t>(pivot)].angle; }
   bool IsPhysicsPivot(int pivot) const { return m_pivots[static_cast<size_t>(pivot)].used; }

   // A launcher trigger: solenoid or lamp reported on, a B2S id written non zero, a switch closed. True when some ball has a route
   bool FireLaunchers(Motion::Source source, int id, double nowSeconds);
   // The ids that launchers react to, by source (the renderer polls them)
   std::vector<std::pair<Motion::Source, int>> LauncherSources() const;

   struct View
   {
      Vec2 center; // physics centre, authored pixels
      float roll = 0.f; // degrees, positive clockwise
      bool active = false;
   };
   View GetView(int ball) const;
   size_t BallCount() const { return m_balls.size(); }
   size_t PivotCount() const { return m_pivots.size(); }
   const Config& GetConfig(int ball) const { return m_balls[static_cast<size_t>(ball)].cfg; }
   float Radius(int ball) const;

   // Testing: n substeps of 1 ms without presenting
   void Substeps(int n);
   // Testing: puts the ball at a centre with a velocity
   void SetState(int ball, Vec2 center, Vec2 velocity);
   Vec2 Velocity(int ball) const { return m_balls[static_cast<size_t>(ball)].vel; }
   int FlipperOf(int ball) const { return m_balls[static_cast<size_t>(ball)].flipper; }

private:
   struct Pivot
   {
      PivotParams params;
      bool used = false; // some ball names it
      float angle = 0.f;
      float prevAngle = 0.f; // the angle at the start of the substep (PB:638)
      bool moving = false;
      float startAngle = 0.f;
      float targetAngle = 0.f;
      int duration = 80;
      double elapsedMs = 0.0;
      bool automaticActive = false;
   };

   struct Ball
   {
      Config cfg;
      RectF authored;
      bool dmd = false;
      float authW = 0.f, authH = 0.f;
      Vec2 authoredCenter;
      Vec2 center; // the physics centre
      Vec2 vel;
      int flipper = -1;
      bool active = true;
      bool hasLastAngle = false;
      float lastAngle = 0.f;
      bool holding = false;
      bool armed = false;
      bool exitedCapture = false;
      std::vector<bool> zoneInside;
      std::vector<std::vector<float>> segBounce;
      std::vector<RectF> obstacles;
      std::vector<float> obstacleBounce;
      std::vector<std::vector<Vec2>> paths;
      std::vector<Zone> zones;
      Launcher launcher;
      bool hasLauncher = false;
      // Presentation (render thread)
      Vec2 presented; // window pixels
      bool presentedSet = false;
      float roll = 0.f;
   };

   void AdvancePivot(Pivot& pivot);
   void StepBall(Ball& ball);
   void ResolveFlipper(const Ball& ball, Vec2& c, float r, Vec2& v, const Pivot& f, float omega);
   void ResolveBoundary(Vec2& c, Vec2 prev, float r, Vec2& v, Vec2 a, Vec2 b, float bounce);
   bool SweptHit(Vec2 prev, Vec2 cur, float r, Vec2 a, Vec2 b, Vec2& impactCenter, Vec2& impactNormal) const;
   void Reflect(Vec2& v, float nx, float ny, float bounce, float incoming);
   static void ShallowBoost(Vec2& v, Vec2 a, Vec2 b, float dt, float g);
   static void ResolveObstacle(Vec2& c, float r, Vec2& v, RectF o, float bounce);
   void CheckLauncherCapture(Ball& ball, Vec2& c);
   void CheckSwitchZones(Ball& ball, Vec2 prev, Vec2 cur);
   void GetLauncherPose(const Ball& ball, Vec2& origin, float& angle) const;
   void Launch(Ball& ball);
   void Present(Ball& ball, bool advanceRoll);
   bool ScaleKnown(const Ball& ball) const { return m_scaleKnown[ball.dmd ? 1 : 0]; }
   double Random() { return m_random(); }

   std::vector<Ball> m_balls;
   std::vector<Pivot> m_pivots;
   std::function<double()> m_random;
   std::function<void(int)> m_pulseSwitch;
   std::array<bool, 2> m_scaleKnown { false, false };
   std::array<Vec2, 2> m_scale { Vec2 { 1.f, 1.f }, Vec2 { 1.f, 1.f } };
   double m_pending = 0.0;
   double m_last = 0.0;
   bool m_hasLast = false;
   bool m_prerolled = false;
};

}

