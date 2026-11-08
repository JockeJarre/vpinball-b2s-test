// license:GPLv3+

#include "Physics.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <memory>
#include <random>

namespace B2S::Physics
{

static constexpr double kPi = 3.14159265358979323846;
static constexpr float kDt = 0.001f; // PhysicsSubstepSeconds
static constexpr float kShallowSlopeLimit = 0.35f;
static constexpr float kShallowRollMultiplier = 2.0f;
static constexpr float kScatterDegrees = 10.0f;
static constexpr float kScatterMinImpact = 150.0f; // px/s of incoming normal speed
static constexpr float kBoundsRestitution = 0.55f;
static constexpr float kFlipperRestitution = 0.58f;
static constexpr float kBallRadiusFactor = 0.43f;
static constexpr float kFlipperRadiusFactor = 0.07f;


// ------------------------------------------------------------------------------------------------------------------------
// Attribute parsers
// ------------------------------------------------------------------------------------------------------------------------

static std::vector<std::string> Split(const std::string& text, char separator)
{
   std::vector<std::string> parts;
   size_t start = 0;
   while (true)
   {
      const size_t end = text.find(separator, start);
      if (end == std::string::npos)
      {
         parts.push_back(text.substr(start));
         break;
      }
      parts.push_back(text.substr(start, end - start));
      start = end + 1;
   }
   return parts;
}

static bool IsBlank(const std::string& text) { return text.find_first_not_of(" \t\r\n\v\f") == std::string::npos; }

// Integer.TryParse: white space around, a sign, digits only
static bool ParseInt(const std::string& text, int& value)
{
   const size_t first = text.find_first_not_of(" \t\r\n\v\f");
   if (first == std::string::npos)
      return false;
   std::string s = text.substr(first, text.find_last_not_of(" \t\r\n\v\f") - first + 1);
   if (!s.empty() && s[0] == '+')
      s.erase(0, 1);
   int result = 0;
   const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
   if (ec != std::errc() || ptr != s.data() + s.size())
      return false;
   value = result;
   return true;
}

RectF ParseBounds(const std::string& text)
{
   const std::vector<std::string> parts = Split(text, ',');
   if (parts.size() != 4)
      return {};
   float v[4];
   for (size_t i = 0; i < 4; i++)
      if (!Motion::ParseFloat(parts[i], v[i]))
         return {};
   if (v[2] <= v[0] || v[3] <= v[1])
      return {};
   return { v[0], v[1], v[2] - v[0], v[3] - v[1] };
}

std::vector<std::vector<Vec2>> ParsePaths(const std::string& text)
{
   std::vector<std::vector<Vec2>> paths;
   if (IsBlank(text))
      return paths;
   for (const std::string& part : Split(text, '|'))
   {
      std::vector<Vec2> path;
      for (const Motion::Vec& p : Motion::ParsePoints(part))
         path.push_back({ static_cast<float>(p.x), static_cast<float>(p.y) });
      if (path.size() >= 2)
         paths.push_back(std::move(path));
   }
   return paths;
}

std::vector<std::vector<float>> ParseSegmentBounces(const std::string& text)
{
   std::vector<std::vector<float>> groups;
   if (IsBlank(text))
      return groups;
   for (const std::string& part : Split(text, '|'))
   {
      std::vector<float> values;
      for (const std::string& item : Split(part, ','))
      {
         float v = -1.f;
         if (!Motion::ParseFloat(item, v))
            v = -1.f;
         values.push_back(std::clamp(v, -1.f, 3.f));
      }
      groups.push_back(std::move(values));
   }
   return groups;
}

std::vector<RectF> ParseObstacles(const std::string& text)
{
   std::vector<RectF> obstacles;
   if (IsBlank(text))
      return obstacles;
   for (const std::string& part : Split(text, '|'))
   {
      const std::vector<std::string> items = Split(part, ',');
      float v[4];
      if (items.size() != 4 || !Motion::ParseFloat(items[0], v[0]) || !Motion::ParseFloat(items[1], v[1]) || !Motion::ParseFloat(items[2], v[2]) || !Motion::ParseFloat(items[3], v[3]))
         continue;
      if (v[2] > 0.f && v[3] > 0.f)
         obstacles.push_back({ v[0], v[1], v[2], v[3] });
   }
   return obstacles;
}

std::vector<float> ParseObstacleBounces(const std::string& text)
{
   std::vector<float> bounces;
   if (IsBlank(text))
      return bounces;
   for (const std::string& item : Split(text, ','))
   {
      float v = -1.f;
      if (!Motion::ParseFloat(item, v) || std::isnan(v) || std::isinf(v) || v < 0.f)
         v = -1.f;
      bounces.push_back(v < 0.f ? -1.f : std::min(3.f, v));
   }
   return bounces;
}

std::vector<Zone> ParseZones(const std::string& text)
{
   std::vector<Zone> zones;
   if (IsBlank(text))
      return zones;
   for (const std::string& part : Split(text, '|'))
   {
      const std::vector<std::string> items = Split(part, ',');
      float v[4];
      int id = 0;
      if (items.size() != 5 || !Motion::ParseFloat(items[0], v[0]) || !Motion::ParseFloat(items[1], v[1]) || !Motion::ParseFloat(items[2], v[2]) || !Motion::ParseFloat(items[3], v[3])
         || !ParseInt(items[4], id))
         continue;
      if (v[2] > 0.f && v[3] > 0.f && id > 0)
         zones.push_back({ { v[0], v[1], v[2], v[3] }, std::min(255, id), 0.f });
   }
   return zones;
}

float NormalizeAngle(float angle)
{
   float normalized = std::fmod(angle, 360.f);
   if (normalized > 180.f)
      normalized -= 360.f;
   if (normalized <= -180.f)
      normalized += 360.f;
   return normalized;
}

// The angle of the i-th zone that survived parsing (an angle for a skipped zone shifts every later one, like the reference)
void ParseZoneAngles(const std::string& text, std::vector<Zone>& zones)
{
   if (IsBlank(text))
      return;
   const std::vector<std::string> parts = Split(text, '|');
   for (size_t i = 0; i < parts.size() && i < zones.size(); i++)
   {
      float v = 0.f;
      if (!Motion::ParseFloat(parts[i], v))
         v = 0.f;
      zones[i].angle = NormalizeAngle(v);
   }
}


// ------------------------------------------------------------------------------------------------------------------------
// System
// ------------------------------------------------------------------------------------------------------------------------

System::System()
{
   // Time seeded and process wide like System.Random
   auto engine = std::make_shared<std::mt19937_64>(std::random_device {}());
   m_random = [engine]() { return std::uniform_real_distribution<double>(0.0, 1.0)(*engine); };
}

int System::AddBall(const Config& config, RectF authored, bool dmd)
{
   Ball ball;
   ball.cfg = config;
   ball.authored = authored;
   ball.dmd = dmd;
   ball.authW = authored.w;
   ball.authH = authored.h;
   ball.authoredCenter = { authored.x + authored.w / 2.0f, authored.y + authored.h / 2.0f };
   ball.center = ball.authoredCenter;
   // A gravity drop ball has no flipper and no launcher, and waits inactive for its release (BD:475-496)
   if (ball.cfg.gravityDrop)
   {
      ball.cfg.flipperName.clear();
      ball.cfg.launcher.enabled = false;
      ball.active = false;
   }
   ball.cfg.gravity = std::clamp(ball.cfg.gravity, 0.f, 10000.f);
   ball.cfg.flipperStrength = std::clamp(ball.cfg.flipperStrength, 0.f, 5.f);
   ball.cfg.boundaryBounce = std::clamp(ball.cfg.boundaryBounce, 0.f, 1.f);
   // Paths, obstacles and zones as the state's constructor keeps them (PBS:91-125)
   for (size_t p = 0; p < config.paths.size(); p++)
   {
      if (config.paths[p].size() < 2)
         continue;
      ball.paths.push_back(config.paths[p]);
      std::vector<float> values;
      const std::vector<float>* saved = p < config.segmentBounces.size() ? &config.segmentBounces[p] : nullptr;
      for (size_t s = 0; s + 1 < config.paths[p].size(); s++)
         values.push_back(std::clamp(saved && s < saved->size() ? (*saved)[s] : -1.f, -1.f, 3.f));
      ball.segBounce.push_back(std::move(values));
   }
   for (size_t i = 0; i < config.obstacles.size(); i++)
   {
      const RectF& o = config.obstacles[i];
      if (o.w <= 0.f || o.h <= 0.f)
         continue;
      ball.obstacles.push_back(o);
      const float v = i < config.obstacleBounces.size() ? config.obstacleBounces[i] : -1.f;
      ball.obstacleBounce.push_back(std::isnan(v) || std::isinf(v) || v < 0.f ? ball.cfg.boundaryBounce : std::min(3.f, v));
   }
   for (const Zone& z : config.zones)
   {
      if (z.bounds.w > 0.f && z.bounds.h > 0.f && z.switchId > 0)
      {
         ball.zones.push_back({ z.bounds, z.switchId, NormalizeAngle(z.angle) });
         ball.zoneInside.push_back(false);
      }
   }
   if (ball.cfg.launcher.enabled)
   {
      ball.launcher = ball.cfg.launcher;
      ball.launcher.captureRadius = std::max(5.f, ball.launcher.captureRadius);
      ball.hasLauncher = true;
      ball.holding = true;
      ball.armed = true;
   }
   m_balls.push_back(std::move(ball));
   return static_cast<int>(m_balls.size()) - 1;
}

int System::AddPivot(const PivotParams& params)
{
   Pivot pivot;
   pivot.params = params;
   pivot.duration = params.moveDuration;
   pivot.angle = params.initialAngle;
   pivot.prevAngle = params.initialAngle;
   m_pivots.push_back(std::move(pivot));
   return static_cast<int>(m_pivots.size()) - 1;
}

static std::string LowerTrim(const std::string& s)
{
   const size_t first = s.find_first_not_of(" \t\r\n");
   if (first == std::string::npos)
      return {};
   std::string r = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
   std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
   return r;
}

void System::Build()
{
   for (Ball& ball : m_balls)
   {
      ball.flipper = -1;
      const std::string name = LowerTrim(ball.cfg.flipperName);
      if (name.empty())
         continue;
      for (size_t i = 0; i < m_pivots.size(); i++)
         if (LowerTrim(m_pivots[i].params.name) == name)
         {
            ball.flipper = static_cast<int>(i);
            m_pivots[i].used = true;
            break;
         }
   }
}

void System::SetScale(bool dmd, float sx, float sy)
{
   if (sx > 0.f && sy > 0.f)
   {
      m_scale[dmd ? 1 : 0] = { sx, sy };
      m_scaleKnown[dmd ? 1 : 0] = true;
   }
}

float System::Radius(int ball) const
{
   const Ball& b = m_balls[static_cast<size_t>(ball)];
   return std::max(2.0f, std::min(b.authW, b.authH) * kBallRadiusFactor);
}

System::View System::GetView(int ball) const
{
   const Ball& b = m_balls[static_cast<size_t>(ball)];
   return { b.center, b.roll, b.active };
}

void System::SetState(int ball, Vec2 center, Vec2 velocity)
{
   Ball& b = m_balls[static_cast<size_t>(ball)];
   b.center = center;
   b.vel = velocity;
}


// ------------------------------------------------------------------------------------------------------------------------
// Clock and pivots (reference BD:336-390, PB:611-659)
// ------------------------------------------------------------------------------------------------------------------------

void System::Advance(double nowSeconds)
{
   if (!m_hasLast)
   {
      m_last = nowSeconds;
      m_hasLast = true;
      return;
   }
   const double elapsed = nowSeconds - m_last;
   m_last = nowSeconds;
   if (!m_prerolled)
   {
      // The reference starts its clock at load, so the first tick consumes the whole load time and the ball is settled when it is
      // first seen. A plugin clock starts at the first frame: give the balls a second to settle (the flipper does not move meanwhile)
      for (const Ball& ball : m_balls)
         if (ball.active && ScaleKnown(ball))
         {
            m_pending += 1.0;
            m_prerolled = true;
            break;
         }
   }
   if (elapsed > 0.0 && std::isfinite(elapsed))
      m_pending += elapsed;
   const int steps = static_cast<int>(std::floor((m_pending + 0.000000001) / 0.001));
   if (steps > 0)
   {
      Substeps(steps);
      m_pending = std::max(0.0, m_pending - steps * 0.001);
   }
   for (Ball& ball : m_balls)
      if (ball.active && ScaleKnown(ball))
         Present(ball, true);
}

void System::Substeps(int n)
{
   for (int i = 0; i < n; i++)
   {
      for (Pivot& pivot : m_pivots)
         if (pivot.used)
            AdvancePivot(pivot);
      for (Ball& ball : m_balls)
         StepBall(ball);
   }
}

// Each substep the pivot remembers its angle, then moves its smoothstep by 1 ms (PB:637-653)
void System::AdvancePivot(Pivot& p)
{
   p.prevAngle = p.angle;
   if (!p.moving)
      return;
   p.elapsedMs += 1.0;
   const float progress = static_cast<float>(std::min(1.0, p.elapsedMs / p.duration));
   const float eased = progress * progress * (3.0f - 2.0f * progress);
   p.angle = p.startAngle + (p.targetAngle - p.startAngle) * eased;
   if (progress >= 1.0f)
   {
      p.angle = p.targetAngle;
      p.moving = false;
      if (p.params.automatic && p.automaticActive)
      {
         p.startAngle = p.angle;
         p.targetAngle = std::abs(p.targetAngle - p.params.upAngle) < 0.01f ? p.params.downAngle : p.params.upAngle;
         p.duration = std::clamp(p.params.moveDuration, 10, 5000);
         p.elapsedMs = 0.0;
         p.moving = true;
      }
   }
}

void System::StartPivotSwing(int pivot, float angle, int durationMs, double nowSeconds)
{
   Synchronize(nowSeconds);
   Pivot& p = m_pivots[static_cast<size_t>(pivot)];
   p.startAngle = p.angle;
   p.targetAngle = angle;
   p.duration = std::clamp(durationMs, 10, p.params.automatic ? 5000 : 1000);
   p.elapsedMs = 0.0;
   p.moving = true;
}

void System::SetPivotAutomaticActive(int pivot, bool active) { m_pivots[static_cast<size_t>(pivot)].automaticActive = active; }


// ------------------------------------------------------------------------------------------------------------------------
// One ball (reference PBS:176-221, 281-342)
// ------------------------------------------------------------------------------------------------------------------------

void System::StepBall(Ball& ball)
{
   if (!ball.active || !ScaleKnown(ball) || ball.authored.IsEmpty())
      return;
   const Pivot* flipper = ball.flipper >= 0 ? &m_pivots[static_cast<size_t>(ball.flipper)] : nullptr;
   float omega = 0.f;
   if (flipper)
   {
      if (!ball.hasLastAngle)
      {
         ball.lastAngle = flipper->prevAngle;
         ball.hasLastAngle = true;
      }
      // The angular velocity of this substep: the angle change since the previous one, in rad per 0.001 s
      omega = static_cast<float>(static_cast<double>(flipper->angle - ball.lastAngle) * kPi / 180.0 / 0.001);
      ball.lastAngle = flipper->angle;
   }

   Vec2 c = ball.center;
   if (ball.holding && ball.hasLauncher)
   {
      Vec2 hold;
      float angle;
      GetLauncherPose(ball, hold, angle);
      ball.center = hold;
      ball.vel = {};
      return;
   }
   const Vec2 prev = c;
   ball.vel.y += ball.cfg.gravity * kDt;
   c.x += ball.vel.x * kDt;
   c.y += ball.vel.y * kDt;
   const float r = std::max(2.0f, std::min(ball.authW, ball.authH) * kBallRadiusFactor);
   const RectF& b = ball.cfg.bounds;
   const float right = b.x + b.w, bottom = b.y + b.h;
   if (c.x - r < b.x)
   {
      c.x = b.x + r;
      ball.vel.x = std::abs(ball.vel.x) * kBoundsRestitution;
   }
   if (c.x + r > right)
   {
      c.x = right - r;
      ball.vel.x = -std::abs(ball.vel.x) * kBoundsRestitution;
   }
   if (c.y - r < b.y)
   {
      c.y = b.y + r;
      ball.vel.y = std::abs(ball.vel.y) * kBoundsRestitution;
   }
   if (c.y + r > bottom)
   {
      c.y = bottom - r;
      ball.vel.y = -std::abs(ball.vel.y) * kBoundsRestitution;
   }
   // An attached launcher uses a rotating snippet, not a flipper collision surface
   if (flipper && (!ball.hasLauncher || !ball.launcher.followPivot))
      ResolveFlipper(ball, c, r, ball.vel, *flipper, omega);
   for (size_t p = 0; p < ball.paths.size(); p++)
   {
      const std::vector<Vec2>& path = ball.paths[p];
      for (size_t s = 0; s + 1 < path.size(); s++)
      {
         float bounce = ball.cfg.boundaryBounce;
         if (p < ball.segBounce.size() && s < ball.segBounce[p].size() && ball.segBounce[p][s] >= 0.f)
            bounce = ball.segBounce[p][s];
         const Vec2 c0 = c, v0 = ball.vel;
         ResolveBoundary(c, prev, r, ball.vel, path[s], path[s + 1], bounce);
         const bool resolved = std::abs(c.x - c0.x) > 0.0001f || std::abs(c.y - c0.y) > 0.0001f || std::abs(ball.vel.x - v0.x) > 0.0001f || std::abs(ball.vel.y - v0.y) > 0.0001f;
         if (resolved)
            ShallowBoost(ball.vel, path[s], path[s + 1], kDt, ball.cfg.gravity);
      }
   }
   for (size_t i = 0; i < ball.obstacles.size(); i++)
      ResolveObstacle(c, r, ball.vel, ball.obstacles[i], ball.obstacleBounce[i]);
   CheckLauncherCapture(ball, c);
   CheckSwitchZones(ball, prev, c);
   ball.center = c;
}

// The flipper: a one sided plate from the hinge to the tip, rotated by the pivot's angle, whose surface moves with the swing
// (reference ResolveFlipperCollision, PBS:734-796)
void System::ResolveFlipper(const Ball& ball, Vec2& c, float r, Vec2& v, const Pivot& f, float omega)
{
   const RectF fb = f.params.rect;
   const Vec2 hinge { fb.x + fb.w * f.params.pivotX, fb.y + fb.h * f.params.pivotY };
   const Vec2 baseTip { fb.x + fb.w * f.params.tipX, fb.y + fb.h * f.params.tipY };
   const double a = f.angle * kPi / 180.0;
   const double sx = baseTip.x - hinge.x, sy = baseTip.y - hinge.y;
   const Vec2 tip { static_cast<float>(hinge.x + sx * std::cos(a) - sy * std::sin(a)), static_cast<float>(hinge.y + sx * std::sin(a) + sy * std::cos(a)) };
   const float segX = tip.x - hinge.x, segY = tip.y - hinge.y;
   const float len2 = segX * segX + segY * segY;
   if (len2 <= 0.01f)
      return;
   const float raw = ((c.x - hinge.x) * segX + (c.y - hinge.y) * segY) / len2;
   const float frac = std::clamp(raw, 0.0f, 1.0f);
   const Vec2 contact { hinge.x + segX * frac, hinge.y + segY * frac };
   const float segLen = std::sqrt(len2);
   float nx = segY / segLen, ny = -segX / segLen;
   if (ny > 0.0f)
   {
      nx = -nx;
      ny = -ny;
   }
   const float flipperRadius = std::max(3.0f, std::min(fb.w, fb.h) * kFlipperRadiusFactor);
   const float minDist = r + flipperRadius;
   if (raw < 0.0f || raw > 1.0f)
   {
      const float ex = c.x - contact.x, ey = c.y - contact.y;
      const float ed = std::sqrt(ex * ex + ey * ey);
      if (ed >= minDist || ed < 0.001f)
         return;
      nx = ex / ed;
      ny = ey / ed;
      if (ny > 0.0f)
         return; // only a ball above the end point
      c.x += nx * (minDist - ed);
      c.y += ny * (minDist - ed);
   }
   else
   {
      const float signedDist = (c.x - contact.x) * nx + (c.y - contact.y) * ny;
      if (signedDist >= minDist)
         return;
      c.x += nx * (minDist - signedDist);
      c.y += ny * (minDist - signedDist);
   }
   const float armX = contact.x - hinge.x, armY = contact.y - hinge.y;
   float impact = ball.cfg.flipperStrength;
   float variation = f.params.randomStrength;
   if (omega != 0.0f && variation > 0.0f && !std::isinf(variation))
   {
      variation = std::min(100.0f, variation);
      impact *= 1.0f + static_cast<float>((Random() * 2.0 - 1.0) * variation / 100.0);
   }
   const float surfX = -omega * armY * impact, surfY = omega * armX * impact;
   float relX = v.x - surfX, relY = v.y - surfY;
   const float toward = relX * nx + relY * ny;
   if (toward < 0.0f)
   {
      relX -= (1.0f + kFlipperRestitution) * toward * nx;
      relY -= (1.0f + kFlipperRestitution) * toward * ny;
      v.x = surfX + relX;
      v.y = surfY + relY;
   }
}

// Reflects the velocity about the normal; fast impacts scatter by up to 10 degrees (reference PBS:588-616)
void System::Reflect(Vec2& v, float nx, float ny, float bounce, float incoming)
{
   v.x -= (1.0f + bounce) * incoming * nx;
   v.y -= (1.0f + bounce) * incoming * ny;
   if (-incoming < kScatterMinImpact)
      return;
   const float deg = static_cast<float>((Random() * 2.0 - 1.0) * kScatterDegrees);
   const double rad = deg * kPi / 180.0;
   const float cs = static_cast<float>(std::cos(rad)), sn = static_cast<float>(std::sin(rad));
   const float rx = v.x * cs - v.y * sn, ry = v.x * sn + v.y * cs;
   if (rx * nx + ry * ny <= 0.0f)
      return; // the rotated velocity would point into the surface: keep the unrotated one
   v.x = rx;
   v.y = ry;
}

// A moving ball against a segment, as a capsule of the ball's radius: the first contact between the start and the end of the step
// (reference PBS:618-732)
bool System::SweptHit(Vec2 prev, Vec2 cur, float r, Vec2 a, Vec2 b, Vec2& ic, Vec2& in) const
{
   const float mx = cur.x - prev.x, my = cur.y - prev.y, mLen2 = mx * mx + my * my;
   if (mLen2 <= 0.000001f)
      return false;
   const float sx = b.x - a.x, sy = b.y - a.y, sLen2 = sx * sx + sy * sy;
   if (sLen2 <= 0.01f)
      return false;
   const float sLen = std::sqrt(sLen2), tx = sx / sLen, ty = sy / sLen, bnx = ty, bny = -tx;
   const float psd = (prev.x - a.x) * bnx + (prev.y - a.y) * bny;
   const float csd = (cur.x - a.x) * bnx + (cur.y - a.y) * bny;
   const float chg = csd - psd;
   float best = FLT_MAX;
   bool found = false;
   const float pp = (prev.x - a.x) * tx + (prev.y - a.y) * ty, cp = (cur.x - a.x) * tx + (cur.y - a.y) * ty;
   // Embedded in the segment's sides at the start and past the other side at the end: back to the side it came from
   if (pp >= 0 && pp <= sLen && cp >= 0 && cp <= sLen && std::abs(psd) < r && psd * csd <= 0.0f && std::abs(csd) >= r)
   {
      const float side = (std::abs(psd) >= 0.000001f) ? (psd > 0 ? 1.f : -1.f) : (chg < 0 ? 1.f : -1.f);
      ic = { a.x + tx * pp + bnx * side * r, a.y + ty * pp + bny * side * r };
      in = { bnx * side, bny * side };
      return true;
   }
   for (int i = 0; i < 2; ++i) // the two parallel sides of the capsule
   {
      const float side = i == 0 ? 1.f : -1.f;
      if (side * chg < -0.000001f)
      {
         const float t = (side * r - psd) / chg;
         if (t >= 0 && t <= 1 && t < best)
         {
            const float hx = prev.x + mx * t, hy = prev.y + my * t;
            const float proj = (hx - a.x) * tx + (hy - a.y) * ty;
            if (proj >= 0 && proj <= sLen)
            {
               best = t;
               ic = { hx, hy };
               in = { bnx * side, bny * side };
               found = true;
            }
         }
      }
   }
   for (int i = 0; i < 2; ++i) // the round end caps
   {
      const Vec2 e = i == 0 ? a : b;
      const float rx = prev.x - e.x, ry = prev.y - e.y, cc = rx * rx + ry * ry - r * r;
      if (cc >= 0.0f)
      {
         const float lin = 2.0f * (rx * mx + ry * my);
         const double disc = static_cast<double>(lin) * lin - 4.0 * static_cast<double>(mLen2) * static_cast<double>(cc);
         if (disc >= 0.0)
         {
            const float t = static_cast<float>((-static_cast<double>(lin) - std::sqrt(disc)) / (2.0 * static_cast<double>(mLen2)));
            if (t >= 0 && t <= 1 && t < best)
            {
               const float hx = prev.x + mx * t, hy = prev.y + my * t, nx = hx - e.x, ny = hy - e.y;
               const float nl = std::sqrt(nx * nx + ny * ny);
               if (nl >= 0.001f)
               {
                  best = t;
                  ic = { hx, hy };
                  in = { nx / nl, ny / nl };
                  found = true;
               }
            }
         }
      }
   }
   return found;
}

// A boundary segment: the ball is pushed back out to a distance of its radius on the side it came from and its velocity reflected
// (reference ResolveStaticFloorCollision, PBS:506-586)
void System::ResolveBoundary(Vec2& c, Vec2 prev, float r, Vec2& v, Vec2 a, Vec2 b, float bounce)
{
   const float sx = b.x - a.x, sy = b.y - a.y, len2 = sx * sx + sy * sy;
   if (len2 <= 0.01f)
      return;
   const float raw = ((c.x - a.x) * sx + (c.y - a.y) * sy) / len2;
   const float f = std::clamp(raw, 0.0f, 1.0f);
   const float cx = a.x + sx * f, cy = a.y + sy * f;
   const float ox = c.x - cx, oy = c.y - cy, d2 = ox * ox + oy * oy;
   if (d2 >= r * r)
   {
      // Not overlapping at the end of the step: a fast ball may have gone through since the start of it
      Vec2 hitC, hitN;
      if (!SweptHit(prev, c, r, a, b, hitC, hitN))
         return;
      c = { hitC.x + hitN.x * 0.01f, hitC.y + hitN.y * 0.01f };
      const float ns = v.x * hitN.x + v.y * hitN.y;
      if (ns < 0.0f)
         Reflect(v, hitN.x, hitN.y, bounce, ns);
      return;
   }
   const float d = std::sqrt(d2);
   float nx, ny;
   if (raw < 0.0f || raw > 1.0f)
   {
      // An end cap: the normal points away from the end point
      const float pox = prev.x - cx, poy = prev.y - cy, pd = std::sqrt(pox * pox + poy * poy);
      if (d >= 0.001f)
      {
         nx = ox / d;
         ny = oy / d;
      }
      else if (pd >= 0.001f)
      {
         nx = pox / pd;
         ny = poy / pd;
      }
      else
         return;
   }
   else
   {
      // The side: the normal is perpendicular to the segment, on the side of the start of the step
      const float sl = std::sqrt(len2);
      nx = sy / sl;
      ny = -sx / sl;
      float side = (prev.x - cx) * nx + (prev.y - cy) * ny;
      if (std::abs(side) < 0.001f)
         side = ox * nx + oy * ny;
      if (std::abs(side) < 0.001f)
      {
         if (v.x * nx + v.y * ny > 0.0f)
         {
            nx = -nx;
            ny = -ny;
         }
      }
      else if (side < 0.0f)
      {
         nx = -nx;
         ny = -ny;
      }
   }
   c = { cx + nx * r, cy + ny * r };
   const float ns = v.x * nx + v.y * ny;
   if (ns < 0.0f)
      Reflect(v, nx, ny, bounce, ns);
}

// On a segment flatter than 0.35 the tangential part of gravity counts twice (reference PBS:344-363)
void System::ShallowBoost(Vec2& v, Vec2 a, Vec2 b, float dt, float g)
{
   const float sx = b.x - a.x, sy = b.y - a.y, len = std::sqrt(sx * sx + sy * sy);
   if (len <= 0.01f)
      return;
   const float tx = sx / len, ty = sy / len, slope = std::abs(ty);
   if (slope <= 0.001f || slope > kShallowSlopeLimit)
      return;
   const float extra = g * ty * (kShallowRollMultiplier - 1.0f);
   v.x += tx * extra * dt;
   v.y += ty * extra * dt;
}

// An obstacle is a circle of the mean half size of its rectangle; no swept test, no scatter (reference PBS:475-504)
void System::ResolveObstacle(Vec2& c, float r, Vec2& v, RectF o, float bounce)
{
   const float ox = o.x + o.w / 2.0f, oy = o.y + o.h / 2.0f;
   const float orad = std::max(1.0f, (o.w + o.h) / 4.0f);
   const float dx = c.x - ox, dy = c.y - oy, minDist = r + orad, d2 = dx * dx + dy * dy;
   if (d2 >= minDist * minDist)
      return;
   float d = std::sqrt(d2), nx, ny;
   if (d < 0.001f)
   {
      nx = 0.f;
      ny = -1.f;
      d = 0.f;
   }
   else
   {
      nx = dx / d;
      ny = dy / d;
   }
   c.x += nx * (minDist - d);
   c.y += ny * (minDist - d);
   const float ns = v.x * nx + v.y * ny;
   if (ns < 0.0f)
   {
      v.x -= (1.0f + bounce) * ns * nx;
      v.y -= (1.0f + bounce) * ns * ny;
   }
}


// ------------------------------------------------------------------------------------------------------------------------
// Launcher and switch zones
// ------------------------------------------------------------------------------------------------------------------------

void System::GetLauncherPose(const Ball& ball, Vec2& origin, float& angle) const
{
   origin = ball.launcher.origin;
   angle = ball.launcher.angle;
   if (!ball.launcher.followPivot || ball.flipper < 0)
      return;
   const Pivot& pivot = m_pivots[static_cast<size_t>(ball.flipper)];
   if (pivot.params.automatic)
      origin = ball.authoredCenter;
   const RectF& pb = pivot.params.rect;
   const Vec2 hinge { pb.x + pb.w * pivot.params.pivotX, pb.y + pb.h * pivot.params.pivotY };
   const float rest = pivot.params.automatic ? 0.f : pivot.params.downAngle;
   const float delta = pivot.angle - rest;
   const double radians = delta * kPi / 180.0;
   const float dx = origin.x - hinge.x, dy = origin.y - hinge.y;
   origin = { hinge.x + static_cast<float>(dx * std::cos(radians) - dy * std::sin(radians)), hinge.y + static_cast<float>(dx * std::sin(radians) + dy * std::cos(radians)) };
   angle += delta;
}

void System::Launch(Ball& ball)
{
   if (!ball.hasLauncher || !ball.armed)
      return;
   const float randomAngleOffset = static_cast<float>((Random() * 2.0 - 1.0) * ball.launcher.randomAngle);
   const float randomStrengthFactor = 1.0f + static_cast<float>((Random() * 2.0 - 1.0) * ball.launcher.randomStrength / 100.0);
   Vec2 origin;
   float angle = 0.f;
   GetLauncherPose(ball, origin, angle);
   const double radians = (angle + randomAngleOffset) * kPi / 180.0;
   const float strength = std::max(0.0f, ball.launcher.strength * randomStrengthFactor);
   ball.center = origin;
   // Presented at once, without rolling
   const Vec2 s = m_scale[ball.dmd ? 1 : 0];
   ball.presented = { origin.x * s.x, origin.y * s.y };
   ball.presentedSet = true;
   ball.vel = { static_cast<float>(std::cos(radians)) * strength, static_cast<float>(std::sin(radians)) * strength };
   ball.armed = false;
   ball.holding = false;
   ball.exitedCapture = false;
   std::fill(ball.zoneInside.begin(), ball.zoneInside.end(), false);
}

// After a launch the ball is held again when it comes back into the capture circle (it must have left it first)
void System::CheckLauncherCapture(Ball& ball, Vec2& c)
{
   if (!ball.hasLauncher || ball.armed)
      return;
   Vec2 origin;
   float angle;
   GetLauncherPose(ball, origin, angle);
   const float radius = ball.launcher.captureRadius;
   const float dx = c.x - origin.x, dy = c.y - origin.y;
   const bool inside = dx * dx + dy * dy <= radius * radius;
   if (!ball.exitedCapture)
      ball.exitedCapture = !inside;
   else if (inside)
   {
      c = origin;
      ball.vel = {};
      ball.armed = true;
      ball.holding = true;
   }
}

static bool ZoneContains(const RectF& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static Vec2 ZoneLocal(Vec2 p, const Zone& zone)
{
   const float angle = NormalizeAngle(zone.angle);
   if (std::abs(angle) < 0.001f)
      return p;
   const float cx = zone.bounds.x + zone.bounds.w / 2.0f, cy = zone.bounds.y + zone.bounds.h / 2.0f;
   const double radians = -angle * kPi / 180.0;
   const double cs = std::cos(radians), sn = std::sin(radians);
   const double x = p.x - cx, y = p.y - cy;
   return { static_cast<float>(cx + x * cs - y * sn), static_cast<float>(cy + x * sn + y * cs) };
}

static bool ClipSegment(double direction, double distance, double& first, double& last)
{
   if (std::abs(direction) < 0.0000001)
      return distance >= 0.0;
   const double ratio = distance / direction;
   if (direction < 0.0)
   {
      if (ratio > last)
         return false;
      if (ratio > first)
         first = ratio;
   }
   else
   {
      if (ratio < first)
         return false;
      if (ratio < last)
         last = ratio;
   }
   return true;
}

static bool SegmentIntersectsZone(Vec2 from, Vec2 to, const Zone& zone)
{
   const Vec2 lf = ZoneLocal(from, zone), lt = ZoneLocal(to, zone);
   if (ZoneContains(zone.bounds, lf.x, lf.y) || ZoneContains(zone.bounds, lt.x, lt.y))
      return true;
   const double dx = lt.x - lf.x, dy = lt.y - lf.y;
   double first = 0.0, last = 1.0;
   return ClipSegment(-dx, lf.x - zone.bounds.x, first, last) && ClipSegment(dx, (zone.bounds.x + zone.bounds.w) - lf.x, first, last)
      && ClipSegment(-dy, lf.y - zone.bounds.y, first, last) && ClipSegment(dy, (zone.bounds.y + zone.bounds.h) - lf.y, first, last);
}

// The ball centre entering a zone closes its PinMAME switch; the zone is armed again once a step ends outside (PBS:410-473)
void System::CheckSwitchZones(Ball& ball, Vec2 prev, Vec2 cur)
{
   for (size_t i = 0; i < ball.zones.size(); i++)
   {
      const Zone& zone = ball.zones[i];
      const Vec2 l = ZoneLocal(cur, zone);
      const bool inside = ZoneContains(zone.bounds, l.x, l.y);
      const bool crossed = !inside && SegmentIntersectsZone(prev, cur, zone);
      if (!ball.zoneInside[i] && (inside || crossed) && m_pulseSwitch)
         m_pulseSwitch(zone.switchId);
      ball.zoneInside[i] = inside;
   }
}


// ------------------------------------------------------------------------------------------------------------------------
// Triggers and presentation
// ------------------------------------------------------------------------------------------------------------------------

static Motion::Source SourceOf(int triggerType)
{
   switch (triggerType)
   {
   case 2: return Motion::Source::Lamp;
   case 3: return Motion::Source::B2S;
   case 5: return Motion::Source::Switch;
   default: return Motion::Source::Solenoid;
   }
}

bool System::FireLaunchers(Motion::Source source, int id, double nowSeconds)
{
   bool routed = false;
   for (Ball& ball : m_balls)
      if (ball.hasLauncher && ball.launcher.triggerId > 0 && ball.launcher.triggerId == id && SourceOf(ball.launcher.triggerType) == source)
      {
         routed = true;
         Synchronize(nowSeconds);
         Launch(ball);
      }
   return routed;
}

std::vector<std::pair<Motion::Source, int>> System::LauncherSources() const
{
   std::vector<std::pair<Motion::Source, int>> result;
   for (const Ball& ball : m_balls)
      if (ball.hasLauncher && ball.launcher.triggerId > 0)
      {
         const std::pair<Motion::Source, int> entry { SourceOf(ball.launcher.triggerType), ball.launcher.triggerId };
         if (std::find(result.begin(), result.end(), entry) == result.end())
            result.push_back(entry);
      }
   return result;
}

// Once per clock tick, from the previously presented centre, in window pixels (reference PresentServerFrame, PBS:186-191)
void System::Present(Ball& ball, bool advanceRoll)
{
   const Vec2 s = m_scale[ball.dmd ? 1 : 0];
   const Vec2 cur { ball.center.x * s.x, ball.center.y * s.y };
   if (!ball.presentedSet)
   {
      ball.presented = { ball.authoredCenter.x * s.x, ball.authoredCenter.y * s.y };
      ball.presentedSet = true;
   }
   if (advanceRoll && ball.cfg.rollEnabled)
   {
      double angle = ball.roll;
      Motion::AdvanceRoll({ ball.presented.x, ball.presented.y }, { cur.x, cur.y }, ball.authW * s.x, ball.authH * s.y, angle);
      ball.roll = static_cast<float>(angle);
   }
   ball.presented = cur;
}

}

