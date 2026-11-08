// license:GPLv3+
//
// B2S Pro motion paths (MotionPath* attributes of image snippits), free of any VPX dependency: the route mover of a single
// bulb (reference B2SPictureBox.vb, PB:677-1157) and the sequence groups that turn a set of bulbs into a managed queue,
// the trough (reference B2SData.vb, BD:609-1160). All positions are in screen pixels of the window the bulb belongs to,
// like the reference: the scale (output pixels per authored pixel) is captured when a bulb first starts, and the lengths,
// the roll and the collision wobble are computed with it. The renderer converts back to authored pixels to draw.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace B2S::Motion
{

struct Vec
{
   double x = 0.0;
   double y = 0.0;
};

struct Rect
{
   double x = 0.0; // left
   double y = 0.0; // top
   double w = 0.0;
   double h = 0.0;
   Vec Center() const { return { x + w * 0.5, y + h * 0.5 }; }
   bool IsEmpty() const { return w <= 0.0 || h <= 0.0; }
};

// Single.TryParse with NumberStyles.Float and the invariant culture: white space around, a sign, a decimal point, an exponent
bool ParseFloat(const std::string& text, float& value);
// "x,y;x,y;..." (reference ParseMotionPathPoints, fB:4571-4585): a pair that is not two numbers is skipped
std::vector<Vec> ParsePoints(const std::string& text);

double PathLength(const std::vector<Vec>& points); // open length
// The point at a fraction of the route's length (reference PointAlongMotionPath, PB:1042-1068); a loop adds the closing segment
Vec PointAlong(const std::vector<Vec>& points, bool loop, double progress);
// The fraction of the open length at the end of the first segment longer than 0.001 (PB:1081-1094)
double FirstSegmentCompletion(const std::vector<Vec>& points);
// Accumulates the roll of a ball moving from one centre to the next (reference AdvanceMotionPathRoll, PB:1111-1122)
void AdvanceRoll(Vec previous, Vec current, double width, double height, double& angle);

enum class Source
{
   Solenoid = 0,
   Lamp = 1,
   B2S = 2,
   Switch = 3
};

enum class Role
{
   Start = 0,
   Stop = 1,
   Resume = 2,
   Remove = 3
};

// What a bulb's XML says, in authored pixels (reference fB:3225-3305, fB:3393-3425)
struct Config
{
   std::vector<Vec> points; // Entry route, kept only with at least two points
   std::vector<Vec> exitPoints;
   int duration = 3000; // ms, 250..30000
   int exitDuration = 3000;
   bool loop = false;
   bool queueTriggers = false;
   bool rollEnabled = false;
   bool drainAll = false;
   bool gravityDrop = false;
   bool respawnEnabled = false;
   Vec respawn; // Authored centre of the feeder's re-entry start
   int respawnDuration = 350; // ms, 50..5000
   std::string group; // Sequence group, trimmed
   int order = 0; // 0..999, the group needs more than 0
   // Trigger ids [role][source]: solenoid and lamp 0..255, B2S 0..250, switch 0..255; 0 = none
   std::array<std::array<int, 4>, 4> ids {};

   int Id(Role role, Source source) const { return ids[static_cast<size_t>(role)][static_cast<size_t>(source)]; }
   bool Routed() const { return points.size() >= 2; }
   bool IsSequenceMember() const { return !group.empty() && order > 0; }
   bool HasMotion() const { return Routed() || IsSequenceMember(); }
};

class System
{
public:
   using StampFn = std::function<uint64_t()>;

   // The stamp function numbers every visibility change, so that the most recent of a motion event and a script or ROM write wins
   explicit System(StampFn nextStamp);

   // key is the reference's control name ("PictureBox<ID>"), which orders members and routes; authored is the bulb rectangle
   // (the alpha cropped one for old files, which the reference moves by its centre); returns the item index
   int AddItem(const Config& config, const std::string& key, bool dmd, Rect authored);
   // Registers the sequence groups and the trigger tables once every item is added
   void Build();

   // The scale (output pixels per authored pixel) of a window, set before it is used; a bulb waits for its window
   void SetScale(bool dmd, double sx, double sy);
   // Advances every mover, wobble and respawn; starts the routes that have no trigger the first time its window is known
   void Tick(double nowMs);
   // Every B2SSetData write in order (reference MyB2SSetData, SV:2105-2134): a rising edge from 0 acts
   void OnB2SData(int id, int value, double nowMs);
   // A solenoid or lamp reported on, or a switch closed (the first observation counts), the ROM routes of the reference
   void OnRomOn(Source source, int id, double nowMs);
   // The solenoid, lamp and switch ids some item reacts to
   std::vector<std::pair<Source, int>> RomSources() const;

   size_t Size() const { return m_items.size(); }
   const Config& GetConfig(int item) const { return m_items[static_cast<size_t>(item)].cfg; }

   struct View
   {
      Vec center; // Authored pixels
      Vec size;
      double roll = 0.0; // Degrees, positive clockwise; 0 unless rolling
      int image = 0; // The item whose image to draw (the trough swaps images between members)
      bool visible = false; // The reference's Visible, with the stamp of its last change
      uint64_t visibleStamp = 0;
   };
   View GetView(int item) const;

   struct Feeder
   {
      bool draw = false;
      Vec center; // Authored pixels
      Vec size;
      double roll = 0.0;
      int image = 0;
   };
   // The persistent source of a sequence anchor, drawn before the anchor (reference fB:1114-1156); itemVisible is the anchor's own
   Feeder GetFeeder(int item, bool itemVisible) const;

   // Testing and diagnostics
   bool IsActive(int item) const { return m_items[static_cast<size_t>(item)].timerOn || m_items[static_cast<size_t>(item)].paused; }
   bool IsOccupied(int item) const;
   int GroupOf(int item) const { return m_items[static_cast<size_t>(item)].group; }

private:
   struct Item
   {
      Config cfg;
      std::string key;
      bool dmd = false;
      Rect authored;
      int group = -1;
      bool autostartDone = false;

      // Screen geometry, captured when the bulb first starts
      bool prepared = false;
      Vec scale { 1.0, 1.0 };
      bool rectSet = false; // The rectangle was moved; until then it is the authored one scaled to the window
      Rect rect; // The reference's RectangleF
      Rect startRect;
      std::vector<Vec> entryRuntime;
      std::vector<Vec> exitRuntime;
      Vec respawnRuntime;

      // Mover: a Timer and a Stopwatch in the reference
      bool timerOn = false;
      bool paused = false;
      bool clockRunning = false;
      double clockBase = 0.0; // ms accumulated while stopped
      double clockStart = 0.0;
      std::vector<Vec> points; // The active route, screen pixels
      double activeDuration = 3000.0;
      double startDelay = 0.0;
      bool activeLoop = false;
      bool launchReported = true;
      double launchProgress = 1.0;
      double arrivalSpeed = 0.0; // px per ms
      int queuedStarts = 0;

      // Appearance
      double roll = 0.0;
      int image = 0;
      bool visible = false;
      uint64_t visibleStamp = 0;

      // Collision wobble (reference StartTroughCollisionImpulse, PB:880-942)
      bool wobbleOn = false;
      Rect wobbleBase;
      Vec wobbleDir;
      double wobbleAmplitude = 0.0;
      double wobbleDelay = 0.0;
      double wobbleStart = 0.0;

      // Sequence source (the feeder) of the anchor member (PB:513-609)
      bool anchor = false;
      bool sourceVisible = false;
      int sourceImage = -1; // -1: the item's own image
      double sourceRoll = 0.0;
      Rect sourceBounds;
      bool respawnActive = false;
      Rect respawnFrom, respawnTo, respawnBounds;
      double respawnClockStart = 0.0;

      // Handlers the group hooks (AddHandler / RemoveHandler in the reference)
      bool hookEntry = false;
      bool hookLaunch = false;
      bool hookRemoval = false;
      bool hookDrain = false;
      bool hookRespawn = false;
   };

   struct Group
   {
      std::vector<int> members; // Sorted by order, then key
      std::set<int> activeEntries;
      std::set<int> occupied;
      std::set<int> activeDrain;
      std::deque<bool> pending; // true = remove
      std::vector<Vec> compactionTargets;
      int activeRemoval = -1;
      int launching = -1;
      int anchor = -1;
      bool sourceReady = true;
   };

   Item& It(int item) { return m_items[static_cast<size_t>(item)]; }
   const Item& It(int item) const { return m_items[static_cast<size_t>(item)]; }

   // Clock
   double Elapsed(const Item& item) const;
   void ClockStart(Item& item);
   void ClockStop(Item& item);
   void ClockReset(Item& item);
   void ClockRestart(Item& item);

   // Appearance
   void SetVisible(int item, bool visible);
   Rect CurrentRect(const Item& item) const;

   // Single bulb mover (PB:677-1000)
   void Prepare(int item);
   void StartCore(int item, bool exit, bool fromCurrentPosition);
   void StopPath(int item);
   void ResumePath(int item);
   void SetCenter(int item, Vec center, bool advanceRoll);
   void SetPosition(int item, Vec center, bool advanceRoll);
   void StartShift(int item, Vec target, int duration, int delay);
   void StartDrainAll(int item, const std::vector<Vec>& route, int duration);
   std::vector<Vec> ExitRuntimeRoute(int item) const;
   void TickMover(int item);
   void TickWobble(int item);
   void TickRespawn(int item);
   void StartWobble(int item, Vec direction, double amplitude, int delay);
   void StopWobble(int item, bool restore);
   bool StartSourceRespawn(int item);
   void StopSourceRespawn(int item);
   void SetSourceAnchor(int item, bool enabled);
   void SetSourceVisible(int item, bool visible);

   // Events of the mover
   void RaiseLaunch(int item);
   void RaiseCompleted(int item);
   void RaiseRespawnCompleted(int item);

   // Triggers
   void DoStart(const std::vector<int>& items);
   void DoRemove(const std::vector<int>& items);
   void DoStop(const std::vector<int>& items);
   void DoResume(const std::vector<int>& items);
   void Fire(Source source, int id, bool startFirst);

   // Sequence groups (BD:650-1160)
   void RegisterMember(int group, int item);
   bool QueueEnabled(const Group& g) const;
   bool DrainAllEnabled(const Group& g) const;
   bool GravityDropEnabled(const Group& g) const;
   void QueueOperation(Group& g, bool remove);
   void GroupRequestStart(int group);
   void GroupRequestRemove(int group);
   bool StartEntry(int group);
   void BeginEntry(int group, int item, bool fromCurrent);
   bool StartRemoval(int group);
   bool StartAllRemovals(int group);
   bool StartGravityDrop(int group);
   void StartCompactionShifts(int group, int removing);
   void NormalizeCompactedSlots(int group, int remaining);
   void UpdateSourceAppearance(int group);
   void BeginSourceRespawn(int group);
   void CompleteSourceRespawn(int group);
   void ProcessPending(int group);
   void ApplyEntryCollision(int group, int incoming);
   void EntryCompleted(int group, int item);
   void LaunchSegmentCompleted(int group, int item);
   void RemovalCompleted(int group, int item);
   void DrainAllRemovalCompleted(int group, int item);

   StampFn m_nextStamp;
   std::vector<Item> m_items;
   std::vector<Group> m_groups;
   std::map<std::string, int> m_groupKeys;
   // Trigger lists [role][source]: id -> items, in registration order
   std::array<std::array<std::map<int, std::vector<int>>, 4>, 4> m_lists;
   std::array<int, 251> m_b2sState {};
   std::array<bool, 251> m_b2sUsed {};
   std::array<bool, 2> m_scaleKnown { false, false };
   std::array<Vec, 2> m_scale { Vec { 1.0, 1.0 }, Vec { 1.0, 1.0 } };
   double m_now = 0.0;
};

}

