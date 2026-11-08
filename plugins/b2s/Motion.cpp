// license:GPLv3+

#include "Motion.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace B2S::Motion
{

static constexpr double kPi = 3.14159265358979323846;

static double Dist(Vec a, Vec b)
{
   const double dx = b.x - a.x, dy = b.y - a.y;
   return std::sqrt(dx * dx + dy * dy);
}

// Single.TryParse with NumberStyles.Float and the invariant culture: white space around, a sign, a decimal point, an exponent
bool ParseFloat(const std::string& text, float& value)
{
   const size_t first = text.find_first_not_of(" \t\r\n\v\f");
   if (first == std::string::npos)
      return false;
   std::string s = text.substr(first, text.find_last_not_of(" \t\r\n\v\f") - first + 1);
   if (!s.empty() && s[0] == '+')
      s.erase(0, 1);
   if (s.empty())
      return false;
   float result = 0.f;
   const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
   if (ec != std::errc() || ptr != s.data() + s.size() || !std::isfinite(result))
      return false;
   value = result;
   return true;
}

std::vector<Vec> ParsePoints(const std::string& text)
{
   std::vector<Vec> points;
   size_t start = 0;
   while (start <= text.size())
   {
      size_t end = text.find(';', start);
      if (end == std::string::npos)
         end = text.size();
      const std::string pair = text.substr(start, end - start);
      const size_t comma = pair.find(',');
      if (comma != std::string::npos && pair.find(',', comma + 1) == std::string::npos)
      {
         float x, y;
         if (ParseFloat(pair.substr(0, comma), x) && ParseFloat(pair.substr(comma + 1), y))
            points.push_back({ x, y });
      }
      if (end == text.size())
         break;
      start = end + 1;
   }
   return points;
}

double PathLength(const std::vector<Vec>& points)
{
   double total = 0.0;
   for (size_t i = 0; i + 1 < points.size(); i++)
      total += Dist(points[i], points[i + 1]);
   return total;
}

Vec PointAlong(const std::vector<Vec>& points, bool loop, double progress)
{
   const size_t n = points.size();
   if (n == 0)
      return {};
   const size_t segments = n - 1 + (loop ? 1 : 0);
   std::vector<double> lengths;
   double total = 0.0;
   for (size_t i = 0; i < segments; i++)
   {
      const double length = Dist(points[i % n], points[(i + 1) % n]);
      lengths.push_back(length);
      total += length;
   }
   if (total <= 0.0)
      return points[0];
   double target = std::clamp(progress, 0.0, 1.0) * total;
   for (size_t i = 0; i < lengths.size(); i++)
   {
      if (target <= lengths[i] || i == lengths.size() - 1)
      {
         const double part = lengths[i] <= 0.0 ? 0.0 : target / lengths[i];
         const Vec a = points[i % n], b = points[(i + 1) % n];
         return { a.x + (b.x - a.x) * part, a.y + (b.y - a.y) * part };
      }
      target -= lengths[i];
   }
   return points[n - 1];
}

double FirstSegmentCompletion(const std::vector<Vec>& points)
{
   if (points.size() < 2)
      return 1.0;
   double total = 0.0, firstDistinctEnd = -1.0;
   for (size_t i = 0; i + 1 < points.size(); i++)
   {
      const double length = Dist(points[i], points[i + 1]);
      total += length;
      if (firstDistinctEnd < 0.0 && length > 0.001)
         firstDistinctEnd = total;
   }
   if (total <= 0.001 || firstDistinctEnd < 0.0)
      return 1.0;
   return std::clamp(firstDistinctEnd / total, 0.0, 1.0);
}

void AdvanceRoll(Vec previous, Vec current, double width, double height, double& angle)
{
   const double dx = current.x - previous.x, dy = current.y - previous.y;
   const double distance = std::sqrt(dx * dx + dy * dy);
   if (distance <= 0.0001)
      return;
   const double dominant = std::abs(dx) >= std::abs(dy) ? dx : dy;
   const double direction = dominant > 0.0 ? 1.0 : (dominant < 0.0 ? -1.0 : 0.0);
   if (direction == 0.0)
      return;
   const double radius = std::max(1.0, std::min(width, height) / 2.0);
   angle = std::fmod(angle + direction * distance / radius * 180.0 / kPi, 360.0);
}

static int CompareNoCase(const std::string& a, const std::string& b)
{
   const size_t n = std::min(a.size(), b.size());
   for (size_t i = 0; i < n; i++)
   {
      const int x = std::toupper(static_cast<unsigned char>(a[i])), y = std::toupper(static_cast<unsigned char>(b[i]));
      if (x != y)
         return x < y ? -1 : 1;
   }
   return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

static std::string LowerKey(const std::string& s)
{
   std::string r = s;
   std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
   return r;
}


System::System(StampFn nextStamp)
   : m_nextStamp(std::move(nextStamp))
{
}

int System::AddItem(const Config& config, const std::string& key, bool dmd, Rect authored)
{
   Item item;
   item.cfg = config;
   item.key = key;
   item.dmd = dmd;
   item.authored = authored;
   item.image = static_cast<int>(m_items.size());
   m_items.push_back(std::move(item));
   return static_cast<int>(m_items.size()) - 1;
}

void System::Build()
{
   // Sequence groups, registered in file order while the bulbs load (reference fB:3425, BD:609-614)
   for (size_t i = 0; i < m_items.size(); i++)
   {
      Item& item = m_items[i];
      if (!item.cfg.IsSequenceMember())
         continue;
      const std::string key = LowerKey(item.cfg.group);
      auto it = m_groupKeys.find(key);
      if (it == m_groupKeys.end())
      {
         it = m_groupKeys.emplace(key, static_cast<int>(m_groups.size())).first;
         m_groups.emplace_back();
      }
      item.group = it->second;
      RegisterMember(it->second, static_cast<int>(i));
   }

   // Trigger lists. The load loop registers the B2S, lamp and solenoid starts, the B2S stops and resumes, and the removes in file
   // order (fB:3487-3528); the lamp and solenoid stops and resumes and the switch routes follow, in the reference's control name
   // order (fB:58-121)
   const auto add = [this](Role role, Source source, int item)
   {
      const int id = m_items[static_cast<size_t>(item)].cfg.Id(role, source);
      if (id > 0)
         m_lists[static_cast<size_t>(role)][static_cast<size_t>(source)][id].push_back(item);
   };
   for (size_t i = 0; i < m_items.size(); i++)
   {
      const Config& c = m_items[i].cfg;
      const int item = static_cast<int>(i);
      const bool removable = c.exitPoints.size() >= 2 || c.gravityDrop;
      if (c.Routed())
      {
         add(Role::Start, Source::Solenoid, item);
         add(Role::Start, Source::Lamp, item);
         add(Role::Start, Source::B2S, item);
         add(Role::Stop, Source::B2S, item);
         add(Role::Resume, Source::B2S, item);
      }
      if (removable)
      {
         add(Role::Remove, Source::Solenoid, item);
         add(Role::Remove, Source::Lamp, item);
         add(Role::Remove, Source::B2S, item);
      }
   }
   std::vector<int> byKey(m_items.size());
   for (size_t i = 0; i < byKey.size(); i++)
      byKey[i] = static_cast<int>(i);
   std::stable_sort(byKey.begin(), byKey.end(), [this](int a, int b) { return m_items[static_cast<size_t>(a)].key < m_items[static_cast<size_t>(b)].key; });
   for (const int item : byKey)
   {
      const Config& c = m_items[static_cast<size_t>(item)].cfg;
      if (c.Routed())
      {
         add(Role::Stop, Source::Lamp, item);
         add(Role::Resume, Source::Lamp, item);
         add(Role::Stop, Source::Solenoid, item);
         add(Role::Resume, Source::Solenoid, item);
         add(Role::Start, Source::Switch, item);
         add(Role::Stop, Source::Switch, item);
         add(Role::Resume, Source::Switch, item);
      }
      if (c.exitPoints.size() >= 2 || c.gravityDrop)
         add(Role::Remove, Source::Switch, item);
   }
   for (size_t role = 0; role < 4; role++)
      for (const auto& [id, items] : m_lists[role][static_cast<size_t>(Source::B2S)])
         if (id >= 0 && id < static_cast<int>(m_b2sUsed.size()))
            m_b2sUsed[static_cast<size_t>(id)] = true;
}

void System::SetScale(bool dmd, double sx, double sy)
{
   if (sx > 0.0 && sy > 0.0)
   {
      m_scale[dmd ? 1 : 0] = { sx, sy };
      m_scaleKnown[dmd ? 1 : 0] = true;
   }
}

std::vector<std::pair<Source, int>> System::RomSources() const
{
   std::set<std::pair<int, int>> ids;
   for (size_t role = 0; role < 4; role++)
      for (const Source source : { Source::Solenoid, Source::Lamp, Source::Switch })
         for (const auto& [id, items] : m_lists[role][static_cast<size_t>(source)])
            ids.insert({ static_cast<int>(source), id });
   std::vector<std::pair<Source, int>> result;
   for (const auto& [source, id] : ids)
      result.push_back({ static_cast<Source>(source), id });
   return result;
}

bool System::IsOccupied(int item) const
{
   const Item& it = It(item);
   return it.group >= 0 && m_groups[static_cast<size_t>(it.group)].occupied.contains(item);
}


// ------------------------------------------------------------------------------------------------------------------------
// Clock and appearance
// ------------------------------------------------------------------------------------------------------------------------

double System::Elapsed(const Item& item) const { return item.clockBase + (item.clockRunning ? m_now - item.clockStart : 0.0); }

void System::ClockStart(Item& item)
{
   if (!item.clockRunning)
   {
      item.clockRunning = true;
      item.clockStart = m_now;
   }
}

void System::ClockStop(Item& item)
{
   if (item.clockRunning)
   {
      item.clockBase += m_now - item.clockStart;
      item.clockRunning = false;
   }
}

void System::ClockReset(Item& item)
{
   item.clockBase = 0.0;
   item.clockRunning = false;
}

void System::ClockRestart(Item& item)
{
   ClockReset(item);
   ClockStart(item);
}

void System::SetVisible(int item, bool visible)
{
   Item& it = It(item);
   it.visible = visible;
   it.visibleStamp = m_nextStamp ? m_nextStamp() : it.visibleStamp + 1;
}

// The reference's RectangleF: the authored rectangle scaled to the window until the bulb is first moved
Rect System::CurrentRect(const Item& item) const
{
   if (item.prepared || item.rectSet)
      return item.rect;
   const Vec s = m_scale[item.dmd ? 1 : 0];
   return { item.authored.x * s.x, item.authored.y * s.y, item.authored.w * s.x, item.authored.h * s.y };
}

System::View System::GetView(int item) const
{
   const Item& it = It(item);
   const Vec s = it.prepared ? it.scale : m_scale[it.dmd ? 1 : 0];
   View v;
   const Vec c = CurrentRect(it).Center();
   v.center = { c.x / s.x, c.y / s.y };
   v.size = { it.authored.w, it.authored.h };
   v.roll = it.cfg.rollEnabled ? it.roll : 0.0;
   v.image = it.image;
   v.visible = it.visible;
   v.visibleStamp = it.visibleStamp;
   return v;
}

System::Feeder System::GetFeeder(int item, bool itemVisible) const
{
   Feeder f;
   const Item& it = It(item);
   if (!it.anchor || !it.cfg.Routed() || !it.sourceVisible)
      return f;
   const Rect me = CurrentRect(it);
   const Rect source = it.respawnActive ? it.respawnBounds : (!it.sourceBounds.IsEmpty() ? it.sourceBounds : me);
   const bool separate = !itemVisible || std::abs(source.x - me.x) > 0.5 || std::abs(source.y - me.y) > 0.5 || std::abs(source.w - me.w) > 0.5 || std::abs(source.h - me.h) > 0.5;
   if (!separate || source.IsEmpty())
      return f;
   const Vec s = it.prepared ? it.scale : m_scale[it.dmd ? 1 : 0];
   const Vec c = source.Center();
   f.draw = true;
   f.center = { c.x / s.x, c.y / s.y };
   f.size = { source.w / s.x, source.h / s.y };
   f.roll = it.cfg.rollEnabled ? it.sourceRoll : 0.0;
   f.image = it.sourceImage >= 0 ? it.sourceImage : it.image;
   return f;
}


// ------------------------------------------------------------------------------------------------------------------------
// Single bulb mover (reference PB:677-1000)
// ------------------------------------------------------------------------------------------------------------------------

// Converts the authored routes to window pixels once, around the bulb's rectangle at its first start (PB:830-857)
void System::Prepare(int item)
{
   Item& it = It(item);
   it.scale = m_scale[it.dmd ? 1 : 0];
   it.startRect = CurrentRect(it);
   it.rect = it.startRect;
   it.rectSet = true;
   if (it.anchor && it.sourceBounds.IsEmpty())
      it.sourceBounds = it.startRect; // Member 1 is also the authored source artwork (PB:832-836)
   const double originalWidth = std::max(1.0, it.authored.w), originalHeight = std::max(1.0, it.authored.h);
   const double scaleX = it.startRect.w / originalWidth, scaleY = it.startRect.h / originalHeight;
   const Vec originalCenter { it.authored.x + originalWidth / 2.0, it.authored.y + originalHeight / 2.0 };
   const Vec runtimeCenter = it.startRect.Center();
   const auto convert = [&](Vec p) { return Vec { runtimeCenter.x + (p.x - originalCenter.x) * scaleX, runtimeCenter.y + (p.y - originalCenter.y) * scaleY }; };
   it.entryRuntime.clear();
   for (const Vec& p : it.cfg.points)
      it.entryRuntime.push_back(convert(p));
   it.exitRuntime.clear();
   for (const Vec& p : it.cfg.exitPoints)
      it.exitRuntime.push_back(convert(p));
   it.respawnRuntime = convert(it.cfg.respawn);
   it.prepared = true;
}

void System::SetCenter(int item, Vec center, bool advanceRoll)
{
   Item& it = It(item);
   const Rect current = CurrentRect(it);
   if (advanceRoll && it.cfg.rollEnabled)
      AdvanceRoll(current.Center(), center, current.w, current.h, it.roll);
   it.rect = { center.x - it.startRect.w / 2.0, center.y - it.startRect.h / 2.0, it.startRect.w, it.startRect.h };
   it.rectSet = true;
}

void System::SetPosition(int item, Vec center, bool advanceRoll)
{
   Item& it = It(item);
   StopWobble(item, true);
   it.timerOn = false;
   ClockReset(it);
   it.paused = false;
   it.activeLoop = false;
   it.startDelay = 0.0;
   it.launchReported = true;
   it.startRect = CurrentRect(it);
   SetCenter(item, center, advanceRoll);
}

void System::StartCore(int item, bool exit, bool fromCurrentPosition)
{
   Item& it = It(item);
   const std::vector<Vec>& source = exit ? it.cfg.exitPoints : it.cfg.points;
   if (source.size() < 2)
      return;
   if (!exit && it.cfg.queueTriggers && (it.timerOn || it.paused))
   {
      it.queuedStarts = std::min(32, it.queuedStarts + 1);
      return;
   }
   StopWobble(item, true);
   it.timerOn = false;
   ClockReset(it);
   it.paused = false;
   it.activeDuration = std::clamp(exit ? it.cfg.exitDuration : it.cfg.duration, 250, 30000);
   it.startDelay = 0.0;
   it.activeLoop = !exit && it.cfg.loop;
   if (!it.prepared)
      Prepare(item);
   it.points = exit ? it.exitRuntime : it.entryRuntime;
   if (!exit && fromCurrentPosition)
   {
      std::vector<Vec> route { CurrentRect(it).Center() };
      for (const Vec& p : it.entryRuntime)
      {
         const Vec previous = route.back();
         const double dx = p.x - previous.x, dy = p.y - previous.y;
         if (dx * dx + dy * dy > 0.0001)
            route.push_back(p);
      }
      it.points = route;
   }
   if (!exit)
      it.arrivalSpeed = PathLength(it.points) / std::max(1.0, it.activeDuration);
   it.launchReported = exit;
   it.launchProgress = exit ? 1.0 : FirstSegmentCompletion(it.points);
   SetCenter(item, it.points[0], false);
   SetVisible(item, true);
   ClockStart(it);
   it.timerOn = true;
}

void System::StopPath(int item)
{
   Item& it = It(item);
   if (it.timerOn)
   {
      it.timerOn = false;
      ClockStop(it);
      it.paused = true;
   }
}

void System::ResumePath(int item)
{
   Item& it = It(item);
   if (!it.paused || it.points.size() < 2)
      return;
   it.paused = false;
   ClockStart(it);
   it.timerOn = true;
}

void System::StartShift(int item, Vec target, int duration, int delay)
{
   Item& it = It(item);
   StopWobble(item, true);
   it.timerOn = false;
   ClockReset(it);
   it.paused = false;
   it.launchReported = true;
   it.startRect = CurrentRect(it);
   it.points = { it.startRect.Center(), target };
   it.activeDuration = std::clamp(duration, 250, 30000);
   it.startDelay = std::max(0, delay);
   it.activeLoop = false;
   SetVisible(item, true);
   ClockStart(it);
   it.timerOn = true;
}

void System::StartDrainAll(int item, const std::vector<Vec>& route, int duration)
{
   if (route.size() < 2)
      return;
   Item& it = It(item);
   StopWobble(item, true);
   it.timerOn = false;
   ClockReset(it);
   it.paused = false;
   it.startRect = CurrentRect(it);
   it.points = route;
   it.activeDuration = std::clamp(duration, 250, 30000);
   it.startDelay = 0.0;
   it.activeLoop = false;
   it.launchReported = true;
   SetCenter(item, it.points[0], false);
   SetVisible(item, true);
   ClockStart(it);
   it.timerOn = true;
}

// The exit route relative to the bulb's current centre (PB:681-694)
std::vector<Vec> System::ExitRuntimeRoute(int item) const
{
   std::vector<Vec> route;
   const Item& it = It(item);
   if (it.cfg.exitPoints.size() < 2)
      return route;
   const Rect current = CurrentRect(it);
   const Vec start = current.Center();
   const Vec authoredStart = it.cfg.exitPoints[0];
   const double scaleX = it.authored.w > 0.0 ? current.w / it.authored.w : 1.0;
   const double scaleY = it.authored.h > 0.0 ? current.h / it.authored.h : 1.0;
   route.push_back(start);
   for (size_t i = 1; i < it.cfg.exitPoints.size(); i++)
      route.push_back({ start.x + (it.cfg.exitPoints[i].x - authoredStart.x) * scaleX, start.y + (it.cfg.exitPoints[i].y - authoredStart.y) * scaleY });
   return route;
}

void System::TickMover(int item)
{
   Item& it = It(item);
   if (!it.timerOn)
      return;
   if (it.points.size() < 2)
   {
      it.timerOn = false;
      ClockReset(it);
      return;
   }
   const double elapsed = Elapsed(it);
   if (elapsed < it.startDelay)
      return;
   const double progress = std::min(1.0, (elapsed - it.startDelay) / it.activeDuration);
   SetCenter(item, PointAlong(it.points, it.activeLoop, progress), true);
   if (!it.launchReported && progress >= it.launchProgress)
   {
      it.launchReported = true;
      RaiseLaunch(item);
   }
   if (progress >= 1.0)
   {
      if (it.activeLoop)
         ClockRestart(It(item));
      else
      {
         it.timerOn = false;
         ClockReset(it);
         it.paused = false;
         RaiseCompleted(item);
         if (It(item).cfg.queueTriggers && It(item).queuedStarts > 0)
         {
            It(item).queuedStarts--;
            StartCore(item, false, false);
         }
      }
   }
}

void System::StartWobble(int item, Vec direction, double amplitude, int delay)
{
   StopWobble(item, true);
   const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
   if (length <= 0.001 || amplitude <= 0.0)
      return;
   Item& it = It(item);
   it.wobbleBase = CurrentRect(it);
   it.wobbleDir = { direction.x / length, direction.y / length };
   it.wobbleAmplitude = amplitude;
   it.wobbleDelay = std::max(0, delay);
   it.wobbleStart = m_now;
   it.wobbleOn = true;
}

void System::StopWobble(int item, bool restore)
{
   Item& it = It(item);
   it.wobbleOn = false;
   if (restore && !it.wobbleBase.IsEmpty())
   {
      it.rect = it.wobbleBase;
      it.rectSet = true;
   }
   it.wobbleBase = {};
   it.wobbleDir = {};
   it.wobbleAmplitude = 0.0;
   it.wobbleDelay = 0.0;
}

// The wobble of a ball hit by an arriving one lasts 180 ms (PB:897-909)
void System::TickWobble(int item)
{
   Item& it = It(item);
   if (!it.wobbleOn)
      return;
   const double elapsed = m_now - it.wobbleStart;
   if (elapsed < it.wobbleDelay)
      return;
   const double progress = (elapsed - it.wobbleDelay) / 180.0;
   if (progress >= 1.0)
   {
      StopWobble(item, true);
      return;
   }
   const double offset = it.wobbleAmplitude * std::sin(kPi * progress) * std::exp(-1.2 * progress);
   const Vec base = it.wobbleBase.Center();
   const Vec center { base.x + it.wobbleDir.x * offset, base.y + it.wobbleDir.y * offset };
   if (it.cfg.rollEnabled)
      AdvanceRoll(CurrentRect(it).Center(), center, it.wobbleBase.w, it.wobbleBase.h, it.roll);
   it.rect = { center.x - it.wobbleBase.w / 2.0, center.y - it.wobbleBase.h / 2.0, it.wobbleBase.w, it.wobbleBase.h };
   it.rectSet = true;
}

bool System::StartSourceRespawn(int item)
{
   Item& it = It(item);
   if (!it.anchor || !it.cfg.respawnEnabled || !it.prepared || it.sourceBounds.IsEmpty())
      return false;
   const double width = it.sourceBounds.w, height = it.sourceBounds.h;
   const Rect from { it.respawnRuntime.x - width / 2.0, it.respawnRuntime.y - height / 2.0, width, height };
   if (std::abs(from.x - it.sourceBounds.x) < 0.5 && std::abs(from.y - it.sourceBounds.y) < 0.5)
      return false;
   StopSourceRespawn(item);
   it.respawnFrom = from;
   it.respawnTo = it.sourceBounds;
   it.respawnBounds = from;
   it.respawnActive = true;
   it.sourceVisible = true;
   it.respawnClockStart = m_now;
   return true;
}

void System::StopSourceRespawn(int item)
{
   Item& it = It(item);
   it.respawnActive = false;
   it.respawnBounds = {};
}

// The feeder slides back to its place with a cubic ease out, rolling on the way (PB:582-609)
void System::TickRespawn(int item)
{
   Item& it = It(item);
   if (!it.respawnActive)
      return;
   const double duration = std::clamp(it.cfg.respawnDuration, 50, 5000);
   const double progress = std::min(1.0, (m_now - it.respawnClockStart) / duration);
   const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
   const Rect next { it.respawnFrom.x + (it.respawnTo.x - it.respawnFrom.x) * eased, it.respawnFrom.y + (it.respawnTo.y - it.respawnFrom.y) * eased, it.respawnTo.w,
      it.respawnTo.h };
   if (it.cfg.rollEnabled)
      AdvanceRoll(it.respawnBounds.Center(), next.Center(), it.respawnTo.w, it.respawnTo.h, it.sourceRoll);
   it.respawnBounds = next;
   if (progress >= 1.0)
   {
      it.respawnBounds = it.respawnTo;
      it.respawnActive = false;
      RaiseRespawnCompleted(item);
   }
}

void System::SetSourceAnchor(int item, bool enabled)
{
   Item& it = It(item);
   if (it.anchor == enabled)
      return;
   it.anchor = enabled;
   it.sourceVisible = enabled;
   if (!enabled)
   {
      StopSourceRespawn(item);
      it.sourceBounds = {};
      it.sourceImage = -1;
   }
}

void System::SetSourceVisible(int item, bool visible)
{
   Item& it = It(item);
   if (!it.anchor || it.sourceVisible == visible)
      return;
   if (!visible)
      StopSourceRespawn(item);
   it.sourceVisible = visible;
}

void System::Tick(double nowMs)
{
   m_now = nowMs;
   for (size_t i = 0; i < m_items.size(); i++)
   {
      Item& it = m_items[i];
      if (!it.autostartDone && m_scaleKnown[it.dmd ? 1 : 0])
      {
         // A route without any start trigger runs from the start (reference fB:4556-4567), sequence members included
         it.autostartDone = true;
         if (it.cfg.Routed() && it.cfg.Id(Role::Start, Source::Solenoid) == 0 && it.cfg.Id(Role::Start, Source::Lamp) == 0 && it.cfg.Id(Role::Start, Source::B2S) == 0
            && it.cfg.Id(Role::Start, Source::Switch) == 0)
            StartCore(static_cast<int>(i), false, false);
      }
   }
   for (size_t i = 0; i < m_items.size(); i++)
   {
      const int item = static_cast<int>(i);
      TickMover(item);
      TickWobble(item);
      TickRespawn(item);
   }
}


// ------------------------------------------------------------------------------------------------------------------------
// Events of the mover reach the group that hooked them
// ------------------------------------------------------------------------------------------------------------------------

void System::RaiseLaunch(int item)
{
   Item& it = It(item);
   if (it.hookLaunch && it.group >= 0)
      LaunchSegmentCompleted(it.group, item);
}

void System::RaiseCompleted(int item)
{
   const int group = It(item).group;
   if (group < 0)
      return;
   if (It(item).hookEntry)
      EntryCompleted(group, item);
   if (It(item).hookRemoval)
      RemovalCompleted(group, item);
   if (It(item).hookDrain)
      DrainAllRemovalCompleted(group, item);
}

void System::RaiseRespawnCompleted(int item)
{
   Item& it = It(item);
   if (it.hookRespawn && it.group >= 0)
   {
      it.hookRespawn = false;
      CompleteSourceRespawn(it.group);
   }
}


// ------------------------------------------------------------------------------------------------------------------------
// Triggers (reference B2SData.StartMotionPaths / RemoveMotionPaths, BD:616-648; Server.MyB2SSetData, SV:2105-2134;
// formBackglass.ApplySwitchTriggers, fB:123-169)
// ------------------------------------------------------------------------------------------------------------------------

void System::DoStart(const std::vector<int>& items)
{
   std::set<int> requested;
   for (const int item : items)
   {
      if (It(item).group >= 0)
         requested.insert(It(item).group);
      else
         StartCore(item, false, false);
   }
   for (const int group : requested)
      GroupRequestStart(group);
}

void System::DoRemove(const std::vector<int>& items)
{
   std::set<int> requested;
   for (const int item : items)
   {
      if (It(item).group >= 0)
         requested.insert(It(item).group);
      else if (It(item).cfg.exitPoints.size() >= 2)
         StartCore(item, true, false);
   }
   for (const int group : requested)
      GroupRequestRemove(group);
}

void System::DoStop(const std::vector<int>& items)
{
   for (const int item : items)
      StopPath(item);
}

void System::DoResume(const std::vector<int>& items)
{
   for (const int item : items)
      ResumePath(item);
}

// startFirst: B2S ids and switches run start, remove, stop, resume; lamps and solenoids stop, resume, start, remove
void System::Fire(Source source, int id, bool startFirst)
{
   const auto list = [&](Role role) -> const std::vector<int>*
   {
      const auto& map = m_lists[static_cast<size_t>(role)][static_cast<size_t>(source)];
      const auto it = map.find(id);
      return it == map.end() ? nullptr : &it->second;
   };
   // The lists are copied: a start can run a group handler, which never changes them, but keep the walk independent of that
   const auto run = [&](Role role)
   {
      if (const auto* items = list(role))
      {
         const std::vector<int> copy = *items;
         switch (role)
         {
         case Role::Start: DoStart(copy); break;
         case Role::Remove: DoRemove(copy); break;
         case Role::Stop: DoStop(copy); break;
         case Role::Resume: DoResume(copy); break;
         }
      }
   };
   if (startFirst)
      for (const Role role : { Role::Start, Role::Remove, Role::Stop, Role::Resume })
         run(role);
   else
      for (const Role role : { Role::Stop, Role::Resume, Role::Start, Role::Remove })
         run(role);
}

void System::OnB2SData(int id, int value, double nowMs)
{
   if (id < 0 || id >= static_cast<int>(m_b2sState.size()) || !m_b2sUsed[static_cast<size_t>(id)])
      return;
   m_now = nowMs;
   const int previous = m_b2sState[static_cast<size_t>(id)];
   m_b2sState[static_cast<size_t>(id)] = value;
   if (previous == 0 && value != 0)
      Fire(Source::B2S, id, true);
}

void System::OnRomOn(Source source, int id, double nowMs)
{
   m_now = nowMs;
   Fire(source, id, source == Source::Switch);
}


// ------------------------------------------------------------------------------------------------------------------------
// Sequence groups (reference MotionPathSequenceState, BD:650-1160)
// ------------------------------------------------------------------------------------------------------------------------

void System::RegisterMember(int group, int item)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   if (std::find(g.members.begin(), g.members.end(), item) != g.members.end())
      return;
   Item& it = It(item);
   if (it.cfg.rollEnabled)
   {
      // A golden-angle phase keeps striped balls from stopping in lockstep (BD:667-671)
      it.roll = std::fmod((std::max(1, it.cfg.order) - 1) * 137.507764, 360.0);
   }
   g.members.push_back(item);
   std::sort(g.members.begin(), g.members.end(),
      [this](int a, int b)
      {
         const Item& l = It(a);
         const Item& r = It(b);
         if (l.cfg.order != r.cfg.order)
            return l.cfg.order < r.cfg.order;
         return CompareNoCase(l.key, r.key) < 0;
      });
   for (const int member : g.members)
      SetSourceAnchor(member, false);
   g.anchor = g.members.empty() ? -1 : g.members[0];
   if (g.anchor >= 0)
   {
      SetSourceAnchor(g.anchor, true);
      UpdateSourceAppearance(group);
      SetSourceVisible(g.anchor, g.sourceReady);
   }
}

bool System::QueueEnabled(const Group& g) const
{
   return std::any_of(g.members.begin(), g.members.end(), [this](int m) { return It(m).cfg.queueTriggers; });
}

bool System::DrainAllEnabled(const Group& g) const
{
   return std::any_of(g.members.begin(), g.members.end(), [this](int m) { return It(m).cfg.drainAll; });
}

bool System::GravityDropEnabled(const Group& g) const
{
   return std::any_of(g.members.begin(), g.members.end(), [this](int m) { return It(m).cfg.gravityDrop; });
}

void System::QueueOperation(Group& g, bool remove)
{
   if (QueueEnabled(g) && g.pending.size() < 32)
      g.pending.push_back(remove);
}

void System::GroupRequestStart(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   if (!g.pending.empty())
   {
      QueueOperation(g, false);
      ProcessPending(group);
      return;
   }
   if (g.activeRemoval >= 0 || !g.activeDrain.empty())
   {
      QueueOperation(g, false);
      return;
   }
   if (!g.sourceReady)
   {
      if (QueueEnabled(g))
         QueueOperation(g, false);
      else if (g.launching >= 0 && g.activeEntries.contains(g.launching))
         BeginEntry(group, g.launching, false);
      return;
   }
   StartEntry(group);
}

void System::GroupRequestRemove(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   if (!g.pending.empty() || !g.activeEntries.empty())
   {
      QueueOperation(g, true);
      ProcessPending(group);
      return;
   }
   if (!g.activeDrain.empty())
   {
      if (QueueEnabled(g))
         QueueOperation(g, true);
      return;
   }
   if (g.activeRemoval >= 0)
   {
      if (QueueEnabled(g))
         QueueOperation(g, true);
      else
         StartCore(g.activeRemoval, true, false);
      return;
   }
   if (GravityDropEnabled(g))
      StartGravityDrop(group);
   else if (DrainAllEnabled(g))
      StartAllRemovals(group);
   else
      StartRemoval(group);
}

// The member with the lowest order that is neither in the trough nor on its way enters next (BD:788-821)
bool System::StartEntry(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   if (!g.sourceReady || g.activeRemoval >= 0 || !g.activeDrain.empty())
      return false;
   int member = -1;
   for (const int m : g.members)
      if ((member < 0 || It(m).cfg.order < It(member).cfg.order) && !g.occupied.contains(m) && !g.activeEntries.contains(m))
         member = m;
   if (member < 0)
      return false;
   g.activeEntries.insert(member);
   It(member).hookEntry = true;
   BeginEntry(group, member, false);
   return true;
}

void System::BeginEntry(int group, int item, bool fromCurrent)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   g.launching = item;
   g.sourceReady = false;
   if (g.anchor >= 0)
   {
      It(g.anchor).sourceImage = It(item).image;
      SetSourceVisible(g.anchor, false);
   }
   It(item).hookLaunch = true;
   StartCore(item, false, fromCurrent);
}

void System::LaunchSegmentCompleted(int group, int item)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   It(item).hookLaunch = false;
   if (item == g.launching && g.activeEntries.contains(item))
      BeginSourceRespawn(group);
}

void System::BeginSourceRespawn(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   g.launching = -1;
   if (g.anchor >= 0)
   {
      UpdateSourceAppearance(group);
      It(g.anchor).hookRespawn = true;
      if (StartSourceRespawn(g.anchor))
         return;
      It(g.anchor).hookRespawn = false;
   }
   CompleteSourceRespawn(group);
}

void System::CompleteSourceRespawn(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   g.sourceReady = true;
   if (g.anchor >= 0)
      SetSourceVisible(g.anchor, true);
   ProcessPending(group);
}

void System::ProcessPending(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   while (!g.pending.empty())
   {
      const bool remove = g.pending.front();
      if (remove)
      {
         if (g.activeRemoval >= 0 || !g.activeDrain.empty() || !g.activeEntries.empty())
            return;
         g.pending.pop_front();
         if (GravityDropEnabled(g) ? StartGravityDrop(group) : (DrainAllEnabled(g) ? StartAllRemovals(group) : StartRemoval(group)))
            return;
      }
      else
      {
         if (g.activeRemoval >= 0 || !g.activeDrain.empty() || !g.sourceReady)
            return;
         if (StartEntry(group))
         {
            g.pending.pop_front();
            return;
         }
         return; // Stays at the head and holds back the removals queued after it
      }
   }
}

// A gravity drop releases the balls to the physics of step 8. Without physics balls it releases nothing, which is also what
// the reference does (BD:918-945: no member activates, nothing is available to reuse)
bool System::StartGravityDrop(int)
{
   return false;
}

bool System::StartRemoval(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   if (g.activeRemoval >= 0 || !g.activeDrain.empty() || !g.activeEntries.empty())
      return false;
   for (const int m : g.members)
      if (g.occupied.contains(m) && It(m).cfg.exitPoints.size() >= 2)
      {
         g.activeRemoval = m;
         break;
      }
   if (g.activeRemoval < 0)
      return false;
   It(g.activeRemoval).hookRemoval = true;
   StartCompactionShifts(group, g.activeRemoval);
   StartCore(g.activeRemoval, true, false);
   return true;
}

// Every ball leaves in one train, each following the one ahead at the lead's speed (BD:854-894)
bool System::StartAllRemovals(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   if (g.activeRemoval >= 0 || !g.activeDrain.empty() || !g.activeEntries.empty())
      return false;
   std::vector<int> removals;
   for (const int m : g.members)
      if (g.occupied.contains(m) && It(m).cfg.exitPoints.size() >= 2)
         removals.push_back(m);
   if (removals.empty())
      return false;
   std::stable_sort(removals.begin(), removals.end(), [this](int a, int b) { return It(a).cfg.order < It(b).cfg.order; });
   for (const int m : removals)
   {
      g.activeDrain.insert(m);
      It(m).hookDrain = true;
   }
   const int lead = removals[0];
   const std::vector<Vec> leadRoute = ExitRuntimeRoute(lead);
   const double leadLength = PathLength(leadRoute);
   const double leadSpeed = leadLength > 0.001 ? leadLength / std::max(250, It(lead).cfg.exitDuration) : 0.0;
   if (leadSpeed <= 0.000001)
   {
      for (const int m : removals)
         StartCore(m, true, false);
      return true;
   }
   const auto addDistinct = [](std::vector<Vec>& route, Vec point)
   {
      if (route.empty())
      {
         route.push_back(point);
         return;
      }
      const Vec previous = route.back();
      const double dx = point.x - previous.x, dy = point.y - previous.y;
      if (dx * dx + dy * dy > 0.0001)
         route.push_back(point);
   };
   for (size_t index = 0; index < removals.size(); index++)
   {
      const int m = removals[index];
      std::vector<Vec> train { CurrentRect(It(m)).Center() };
      for (int preceding = static_cast<int>(index) - 1; preceding >= 0; preceding--)
         addDistinct(train, CurrentRect(It(removals[static_cast<size_t>(preceding)])).Center());
      for (size_t k = 1; k < leadRoute.size(); k++)
         addDistinct(train, leadRoute[k]);
      const int duration = static_cast<int>(std::nearbyint(PathLength(train) / leadSpeed));
      StartDrainAll(m, train, duration);
   }
   return true;
}

// The balls behind the leaving one roll up into its place, one after the other (BD:1014-1027)
void System::StartCompactionShifts(int group, int removing)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   g.compactionTargets.clear();
   std::vector<int> occupiedInOrder;
   for (const int m : g.members)
      if (g.occupied.contains(m))
         occupiedInOrder.push_back(m);
   if (occupiedInOrder.empty() || occupiedInOrder[0] != removing)
      return;
   for (size_t index = 0; index + 1 < occupiedInOrder.size(); index++)
   {
      g.compactionTargets.push_back(CurrentRect(It(occupiedInOrder[index])).Center());
      StartShift(occupiedInOrder[index + 1], g.compactionTargets[index], It(removing).cfg.exitDuration, static_cast<int>(index) * 48);
   }
}

// When the leaving ball is gone the survivors swap their images and roll angles onto the lower members and snap into their
// slots, which cuts a late compaction shift short (BD:1029-1075)
void System::NormalizeCompactedSlots(int group, int remaining)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   std::vector<int> survivorImages, unusedImages;
   std::vector<double> survivorRolls, unusedRolls;
   for (const int m : g.members)
   {
      if (g.occupied.contains(m))
      {
         survivorImages.push_back(It(m).image);
         survivorRolls.push_back(It(m).roll);
      }
      else
      {
         unusedImages.push_back(It(m).image);
         unusedRolls.push_back(It(m).roll);
      }
   }
   g.occupied.clear();
   for (size_t index = 0; index < g.members.size(); index++)
   {
      const int m = g.members[index];
      if (static_cast<int>(index) < remaining && index < survivorRolls.size())
      {
         It(m).image = survivorImages[index];
         It(m).roll = survivorRolls[index];
      }
      else
      {
         const int unusedIndex = static_cast<int>(index) - remaining;
         if (unusedIndex >= 0 && unusedIndex < static_cast<int>(unusedRolls.size()))
         {
            It(m).image = unusedImages[static_cast<size_t>(unusedIndex)];
            It(m).roll = unusedRolls[static_cast<size_t>(unusedIndex)];
         }
      }
      SetPosition(m, CurrentRect(It(m)).Center(), false);
      if (static_cast<int>(index) < remaining)
      {
         if (index < g.compactionTargets.size())
            SetPosition(m, g.compactionTargets[index], false);
         SetVisible(m, true);
         g.occupied.insert(m);
      }
      else
         SetVisible(m, false);
   }
   g.compactionTargets.clear();
   UpdateSourceAppearance(group);
}

// The feeder shows the next ball to enter (BD:1077-1086)
void System::UpdateSourceAppearance(int group)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   if (g.anchor < 0)
      return;
   for (const int m : g.members)
      if (!g.occupied.contains(m) && !g.activeEntries.contains(m) && m != g.activeRemoval)
      {
         It(g.anchor).sourceImage = It(m).image;
         return;
      }
}

void System::EntryCompleted(int group, int item)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   It(item).hookEntry = false;
   It(item).hookLaunch = false;
   if (g.activeEntries.erase(item) > 0)
   {
      ApplyEntryCollision(group, item);
      g.occupied.insert(item);
      if (item == g.launching)
         BeginSourceRespawn(group);
   }
   ProcessPending(group);
}

// The arriving ball bounces back a little and passes a decaying impulse down the balls already in the trough (BD:1104-1133)
void System::ApplyEntryCollision(int group, int incoming)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   if (g.occupied.empty())
      return;
   std::vector<int> impacted;
   for (const int m : g.members)
      if (g.occupied.contains(m))
         impacted.push_back(m);
   std::stable_sort(impacted.begin(), impacted.end(), [this](int a, int b) { return It(a).cfg.order > It(b).cfg.order; });
   if (impacted.empty())
      return;
   const Vec incomingCenter = CurrentRect(It(incoming)).Center();
   const Vec nearest = CurrentRect(It(impacted[0])).Center();
   Vec direction { nearest.x - incomingCenter.x, nearest.y - incomingCenter.y };
   const double directionLength = std::sqrt(direction.x * direction.x + direction.y * direction.y);
   if (directionLength <= 0.001)
      return;
   direction = { direction.x / directionLength, direction.y / directionLength };
   const Rect incomingRect = CurrentRect(It(incoming));
   const double ballDiameter = std::max(1.0, std::min(incomingRect.w, incomingRect.h));
   double impactAmplitude = It(incoming).arrivalSpeed * 4.5;
   impactAmplitude = std::max(ballDiameter * 0.012, std::min(ballDiameter * 0.04, impactAmplitude)) * 30.0;
   StartWobble(incoming, { -direction.x, -direction.y }, impactAmplitude * 0.35, 0);
   double transferred = impactAmplitude;
   for (size_t index = 0; index < impacted.size(); index++)
   {
      StartWobble(impacted[index], direction, transferred, static_cast<int>(index) * 18);
      transferred *= 0.58;
      if (transferred < 0.25)
         break;
   }
}

void System::RemovalCompleted(int group, int item)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   It(item).hookRemoval = false;
   if (item == g.activeRemoval)
   {
      g.occupied.erase(item);
      SetVisible(item, false);
      NormalizeCompactedSlots(group, static_cast<int>(g.occupied.size()));
      g.activeRemoval = -1;
   }
   ProcessPending(group);
}

void System::DrainAllRemovalCompleted(int group, int item)
{
   Group& g = m_groups[static_cast<size_t>(group)];
   It(item).hookDrain = false;
   if (g.activeDrain.erase(item) == 0)
      return;
   g.occupied.erase(item);
   SetVisible(item, false);
   if (g.activeDrain.empty())
   {
      g.compactionTargets.clear();
      UpdateSourceAppearance(group);
      ProcessPending(group);
   }
}

}

