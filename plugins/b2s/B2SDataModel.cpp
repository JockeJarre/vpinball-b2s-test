// license:GPLv3+

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <format>
#include <set>

#include "common.h"
#include "B2SDataModel.h"
#include "B2SServer.h"

#include "tinyxml2/tinyxml2.h"

namespace B2S {

static const tinyxml2::XMLElement* GetNode(const tinyxml2::XMLNode& doc, const std::string& nodePath) noexcept
{
   const tinyxml2::XMLNode* node = &doc;
   size_t pos = 0;
   size_t nextPos = nodePath.find('/', pos);
   while (nextPos != std::string::npos)
   {
      const std::string elementName = nodePath.substr(pos, nextPos - pos);
      node = node->FirstChildElement(elementName.c_str());
      if (!node)
         return nullptr;
      pos = nextPos + 1;
      nextPos = nodePath.find('/', pos);
   }
   if (const std::string finalElementName = nodePath.substr(pos); !finalElementName.empty())
      node = node->FirstChildElement(finalElementName.c_str());
   return node ? node->ToElement() : nullptr;
}

static string GetStringAttribute(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& attributeName, const string& defVal) noexcept
{
   if (const tinyxml2::XMLElement* node = GetNode(doc, nodePath); node)
   {
      const char* value = node->Attribute(attributeName.c_str());
      if (value)
         return value;
   }
   return defVal;
}

static int GetIntAttribute(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& attributeName, const int& defVal) noexcept
{
   const tinyxml2::XMLElement* node = GetNode(doc, nodePath);
   return node ? node->IntAttribute(attributeName.c_str(), defVal) : defVal;
}

static bool GetBoolAttribute(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& attributeName, const bool& defVal) noexcept
{
   return GetIntAttribute(doc, nodePath, attributeName, defVal ? 1 : 0) != 0;
}

static vec4 GetColorAttribute(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& attributeName, const vec4& defVal) noexcept
{
   if (const tinyxml2::XMLElement* node = GetNode(doc, nodePath); node)
   {
      const char* value = node->Attribute(attributeName.c_str());
      if (value)
      {
         std::istringstream ss(value);
         string token;
         vector<int> colorValues;
         while (std::getline(ss, token, '.'))
         {
            int result = 0; // stays 0 when a colour part is not a number (from_chars leaves it untouched)
            std::from_chars(token.c_str(), token.c_str() + token.length(), result);
            colorValues.push_back(result);
         }
         if (colorValues.size() == 3)
            return vec4(static_cast<float>(colorValues[0]) / 255.f, static_cast<float>(colorValues[1]) / 255.f, static_cast<float>(colorValues[2]) / 255.f, 1.f);
      }
   }
   return defVal;
}

static VPXTexture GetTextureAttribute(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& attributeName) noexcept
{
   if (const tinyxml2::XMLElement* node = GetNode(doc, nodePath); node)
   {
      if (const char* value = node->Attribute(attributeName.c_str()))
      {
         std::string_view valueView { value };
         vector<uint8_t> decoded = base64_decode(valueView.data(), valueView.size());
         if (decoded.empty())
         {
            string path;
            const tinyxml2::XMLNode* parent = node;
            while (parent != nullptr)
            {
               if (parent->ToElement() != nullptr)
                  path = "/"s + parent->ToElement()->Name() + path;
               parent = parent->Parent();
            }
            LOGE(std::format("Failed to decode image at line {} ({})", node->GetLineNum(), path));
         }
         return CreateTexture(decoded.data(), static_cast<int>(decoded.size()));
      }
   }
   return nullptr;
}

static std::shared_ptr<vector<uint8_t>> GetSoundAttribute(const tinyxml2::XMLNode& doc, const std::string& nodePath) noexcept
{
   if (const tinyxml2::XMLElement* node = GetNode(doc, nodePath); node)
   {
      // The WAV stream is stored in the 'Stream' attribute ('Value' in some older files)
      const char* value = node->Attribute("Stream");
      if (value == nullptr)
         value = node->Attribute("Value");
      if (value != nullptr)
      {
         std::string_view valueView { value };
         vector<uint8_t> decoded_wav = base64_decode(valueView.data(), valueView.size());
         return std::make_shared<vector<uint8_t>>(std::move(decoded_wav));
      }
   }
   return nullptr;
}

static B2SImage GetImageAttribute(const tinyxml2::XMLNode& doc, const std::string& nodePath) noexcept
{
   const tinyxml2::XMLElement* node = GetNode(doc, nodePath);
   return node ? B2SImage(*node) : B2SImage();
}

template <class T> static vector<T> GetList(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& subNodeName) noexcept
{
   vector<T> list;
   const tinyxml2::XMLElement* node = GetNode(doc, nodePath);
   if (node == nullptr)
      return list;
   for (auto subNode = node->FirstChildElement(subNodeName.c_str()); subNode != nullptr; subNode = subNode->NextSiblingElement(subNodeName.c_str()))
      list.emplace_back(*subNode);
   return list;
}

template <class T> static vector<std::unique_ptr<T>> GetPtrList(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& subNodeName) noexcept
{
   vector<std::unique_ptr<T>> list;
   const tinyxml2::XMLElement* node = GetNode(doc, nodePath);
   if (node == nullptr)
      return list;
   for (auto subNode = node->FirstChildElement(subNodeName.c_str()); subNode != nullptr; subNode = subNode->NextSiblingElement(subNodeName.c_str()))
      list.emplace_back(std::make_unique<T>(*subNode));
   return list;
}

template <class T> static vector<T> GetFilteredList(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& subNodeName, bool isDMD) noexcept
{
   vector<T> list;
   const tinyxml2::XMLElement* node = GetNode(doc, nodePath);
   if (node == nullptr)
      return list;
   for (auto subNode = node->FirstChildElement(subNodeName.c_str()); subNode != nullptr; subNode = subNode->NextSiblingElement(subNodeName.c_str()))
   {
      bool isBackglass = subNode->Attribute("Parent", "Backglass") != nullptr;
      if (isBackglass == !isDMD)
         list.emplace_back(*subNode);
   }
   return list;
}

template <class T> static vector<std::unique_ptr<T>> GetFilteredPtrList(const tinyxml2::XMLNode& doc, const std::string& nodePath, const std::string& subNodeName, bool isDMD) noexcept
{
   vector<std::unique_ptr<T>> list;
   const tinyxml2::XMLElement* node = GetNode(doc, nodePath);
   if (node == nullptr)
      return list;
   for (auto subNode = node->FirstChildElement(subNodeName.c_str()); subNode != nullptr; subNode = subNode->NextSiblingElement(subNodeName.c_str()))
   {
      bool isBackglass = subNode->Attribute("Parent", "Backglass") != nullptr;
      if (isBackglass == !isDMD)
         list.emplace_back(std::make_unique<T>(*subNode));
   }
   return list;
}

static std::vector<std::string> GetStringList(const std::string& str, char delimiter) noexcept
{
   std::vector<std::string> tokens;
   std::istringstream tokenStream(str);
   std::string token;
   while (std::getline(tokenStream, token, delimiter))
      tokens.push_back(token);
   return tokens;
}

static bool StartsWithCaseInsensitive(const std::string_view& str, const std::string_view& prefix) noexcept
{
   if (prefix.length() > str.length()) return false;
   return std::equal(prefix.begin(), prefix.end(), str.begin(), [](char a, char b) { return cLower(a) == cLower(b); });
}

B2SSound::B2SSound(const tinyxml2::XMLNode& root) noexcept
   : m_name(GetStringAttribute(root, ""s, "Name"s, ""s))
   , m_wav(GetSoundAttribute(root, ""s))
{
}


// B2S Pro: a standard image snippit without native rotation is drawn from its full resolution
// RuntimeSnippitImage when the file provides one (reference fB:3309-3312)
static string BulbImageAttribute(const tinyxml2::XMLNode& root) noexcept
{
   const tinyxml2::XMLElement* node = root.ToElement();
   if (node == nullptr || node->Attribute("RuntimeSnippitImage") == nullptr || !node->Attribute("IsImageSnippit", "1"))
      return "Image"s;
   if (GetIntAttribute(root, ""s, "SnippitType"s, 0) != static_cast<int>(B2SSnippitType::StandardImage) || node->Attribute("AutomaticRotationEnabled", "1"))
      return "Image"s;
   return "RuntimeSnippitImage"s;
}

// B2S Pro: FlasherPulseDuration in ms, clamped to 50..5000 when set (reference fB:3012-3016)
static int GetFlasherPulseDuration(const tinyxml2::XMLNode& root) noexcept
{
   const int duration = GetIntAttribute(root, ""s, "FlasherPulseDuration"s, 0);
   return duration > 0 ? std::clamp(duration, 50, 5000) : 0;
}

// B2S Pro attributes follow the reference's .NET TryParse rules: an absent attribute keeps its default, a present but
// unparsable one reads as 0. Numbers may be surrounded by spaces and carry a sign; floats use the invariant culture
static string TrimNet(const char* value)
{
   string s(value);
   const size_t first = s.find_first_not_of(" \t\r\n");
   if (first == string::npos)
      return ""s;
   s = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
   if (!s.empty() && s[0] == '+')
      s.erase(0, 1);
   return s;
}

static float GetNetFloatAttribute(const tinyxml2::XMLNode& root, const char* name, float defVal) noexcept
{
   const tinyxml2::XMLElement* node = root.ToElement();
   const char* value = node ? node->Attribute(name) : nullptr;
   if (value == nullptr)
      return defVal;
   const string s = TrimNet(value);
   float result = 0.f;
   const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
   return (ec == std::errc() && ptr == s.data() + s.size() && std::isfinite(result)) ? result : 0.f;
}

static int GetNetIntAttribute(const tinyxml2::XMLNode& root, const char* name, int defVal) noexcept
{
   const tinyxml2::XMLElement* node = root.ToElement();
   const char* value = node ? node->Attribute(name) : nullptr;
   if (value == nullptr)
      return defVal;
   const string s = TrimNet(value);
   int result = 0;
   const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
   return (ec == std::errc() && ptr == s.data() + s.size()) ? result : 0;
}

// Unlike the pivot attributes, the Reel3D values keep their default when the attribute is absent or not a number
// (reference fB:3599-3602: Integer.TryParse inside the If)
static int GetIntOrDefault(const tinyxml2::XMLNode& root, const char* name, int defVal) noexcept
{
   const tinyxml2::XMLElement* node = root.ToElement();
   const char* value = node ? node->Attribute(name) : nullptr;
   if (value == nullptr)
      return defVal;
   const string s = TrimNet(value);
   int result = 0;
   const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
   return (ec == std::errc() && ptr == s.data() + s.size()) ? result : defVal;
}

static bool IsAttributeOne(const tinyxml2::XMLNode& root, const char* name) noexcept
{
   const tinyxml2::XMLElement* node = root.ToElement();
   return node != nullptr && node->Attribute(name, "1") != nullptr;
}

static string GetTrimmedAttribute(const tinyxml2::XMLNode& root, const char* name) noexcept
{
   const tinyxml2::XMLElement* node = root.ToElement();
   const char* value = node ? node->Attribute(name) : nullptr;
   if (value == nullptr)
      return ""s;
   string s(value);
   const size_t first = s.find_first_not_of(" \t\r\n");
   return first == string::npos ? ""s : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

static bool HasAttribute(const tinyxml2::XMLNode& root, const char* name) noexcept
{
   const tinyxml2::XMLElement* node = root.ToElement();
   return node != nullptr && node->Attribute(name) != nullptr;
}

// Native rotation is for standard image bulbs only (a SnippitType of 0, or none): AutomaticRotationEnabled is not even
// read for rotating or mech images (reference fB:3073-3076)
static bool IsNativeRotation(const tinyxml2::XMLNode& root) noexcept
{
   return GetNetIntAttribute(root, "SnippitType", 0) == 0 && IsAttributeOne(root, "AutomaticRotationEnabled");
}

// B2S Pro physics balls (reference fB:3151-3226, BD:465-497): not gated by IsImageSnippit. A present but unparsable scalar reads as 0
// and is clamped; a list item that does not parse is skipped. The floor points are the only boundary when there is no other
static Physics::Config ParsePhysicsConfig(const tinyxml2::XMLNode& root) noexcept
{
   Physics::Config c;
   c.ball = IsAttributeOne(root, "PhysicsBall");
   if (!c.ball)
      return c;
   const tinyxml2::XMLElement* node = root.ToElement();
   const auto text = [node](const char* name) { return node && node->Attribute(name) ? string(node->Attribute(name)) : string(); };
   c.bounds = Physics::ParseBounds(text("PhysicsBounds"));
   c.gravity = std::clamp(GetNetFloatAttribute(root, "PhysicsGravity", 650.f), 0.f, 10000.f);
   c.flipperStrength = std::clamp(GetNetFloatAttribute(root, "PhysicsFlipperStrength", 1.f), 0.f, 5.f);
   c.boundaryBounce = std::clamp(GetNetFloatAttribute(root, "PhysicsBoundaryBounce", 0.12f), 0.f, 1.f);
   c.flipperName = GetTrimmedAttribute(root, "PhysicsFlipperName");
   c.paths = Physics::ParsePaths(text("PhysicsBoundaries"));
   if (c.paths.empty())
   {
      std::vector<Physics::Vec2> floor;
      for (const Motion::Vec& p : Motion::ParsePoints(text("PhysicsFloorPoints")))
         floor.push_back({ static_cast<float>(p.x), static_cast<float>(p.y) });
      if (floor.size() >= 2)
         c.paths.push_back(std::move(floor));
   }
   c.segmentBounces = Physics::ParseSegmentBounces(text("PhysicsBoundarySegmentBounces"));
   c.obstacles = Physics::ParseObstacles(text("PhysicsObstacles"));
   c.obstacleBounces = Physics::ParseObstacleBounces(text("PhysicsObstacleBounces"));
   c.zones = Physics::ParseZones(text("PhysicsSwitchZones"));
   if (HasAttribute(root, "PhysicsSwitchZones"))
      Physics::ParseZoneAngles(text("PhysicsSwitchAngles"), c.zones);
   if (IsAttributeOne(root, "PhysicsLauncherEnabled"))
   {
      Physics::Launcher& l = c.launcher;
      l.enabled = true;
      l.followPivot = IsAttributeOne(root, "PhysicsLauncherFollowPivot");
      l.triggerType = GetNetIntAttribute(root, "PhysicsLauncherTriggerType", 1);
      l.triggerId = GetNetIntAttribute(root, "PhysicsLauncherTriggerID", 0);
      l.origin = { GetNetFloatAttribute(root, "PhysicsLauncherX", 0.f), GetNetFloatAttribute(root, "PhysicsLauncherY", 0.f) };
      l.angle = GetNetFloatAttribute(root, "PhysicsLauncherAngle", -90.f);
      l.strength = GetNetFloatAttribute(root, "PhysicsLauncherStrength", 900.f);
      l.randomAngle = GetNetFloatAttribute(root, "PhysicsLauncherRandomAngle", 0.f);
      l.randomStrength = GetNetFloatAttribute(root, "PhysicsLauncherRandomStrength", 0.f);
      l.captureRadius = GetNetFloatAttribute(root, "PhysicsLauncherCaptureRadius", 45.f);
   }
   c.rollEnabled = IsAttributeOne(root, "IsImageSnippit") && IsAttributeOne(root, "MotionPathRollEnabled");
   c.gravityDrop = IsAttributeOne(root, "IsImageSnippit") && IsAttributeOne(root, "MotionPathGravityDrop");
   return c;
}

// The alpha plane of a base64 PNG (the occlusion mask is stationary in backglass space); the texture itself is dropped. An image
// without an alpha channel is opaque, an undecodable one is ignored (the reference aborts the whole load)
static void DecodePhysicsMask(const tinyxml2::XMLNode& root, std::vector<uint8_t>& mask, int& width, int& height) noexcept
{
   if (!HasAttribute(root, "PhysicsSelectionMaskData"))
      return;
   const VPXTexture texture = GetTextureAttribute(root, ""s, "PhysicsSelectionMaskData"s);
   if (texture == nullptr)
      return;
   if (const VPXTextureInfo* info = GetTextureInfo(texture); info && info->width > 0 && info->height > 0 && info->data != nullptr)
   {
      width = static_cast<int>(info->width);
      height = static_cast<int>(info->height);
      const size_t count = static_cast<size_t>(width) * height;
      mask.assign(count, 255);
      if (info->format == VPXTEXFMT_sRGBA8)
      {
         const uint8_t* const pixels = static_cast<const uint8_t*>(info->data);
         for (size_t i = 0; i < count; i++)
            mask[i] = pixels[i * 4 + 3];
      }
   }
   DeleteTexture(texture);
}

// B2S Pro motion paths (reference fB:3225-3305, fB:3393-3425): image snippits only. A present but unparsable number reads as 0
// and is clamped like any other; the reference reads eight of the ids with CInt and aborts the load on non numeric text, which
// reads as 0 here. A route needs two points, and an id of 0 is none
static Motion::Config ParseMotionConfig(const tinyxml2::XMLNode& root) noexcept
{
   Motion::Config c;
   // A bulb that is also a pivot or a native rotation keeps that behavior only: the combination is in no sample file
   if (!IsAttributeOne(root, "IsImageSnippit") || IsAttributeOne(root, "PivotAnimation") || IsNativeRotation(root))
      return c;
   const tinyxml2::XMLElement* node = root.ToElement();
   const auto points = [node](const char* name)
   {
      std::vector<Motion::Vec> result = node && node->Attribute(name) ? Motion::ParsePoints(node->Attribute(name)) : std::vector<Motion::Vec>();
      if (result.size() < 2)
         result.clear();
      return result;
   };
   c.points = points("MotionPathPoints");
   c.exitPoints = points("MotionPathExitPoints");
   c.duration = std::clamp(GetNetIntAttribute(root, "MotionPathDuration", 3000), 250, 30000);
   c.exitDuration = std::clamp(GetNetIntAttribute(root, "MotionPathExitDuration", 3000), 250, 30000);
   c.loop = IsAttributeOne(root, "MotionPathLoop");
   c.queueTriggers = IsAttributeOne(root, "MotionPathQueueTriggers");
   c.rollEnabled = IsAttributeOne(root, "MotionPathRollEnabled");
   c.drainAll = IsAttributeOne(root, "MotionPathDrainAll");
   c.gravityDrop = IsAttributeOne(root, "MotionPathGravityDrop");
   c.respawnEnabled = IsAttributeOne(root, "MotionPathRespawnEnabled");
   if (c.respawnEnabled)
   {
      c.respawn = { GetNetFloatAttribute(root, "MotionPathRespawnX", 0.f), GetNetFloatAttribute(root, "MotionPathRespawnY", 0.f) };
      c.respawnDuration = std::clamp(GetNetIntAttribute(root, "MotionPathRespawnDuration", 350), 50, 5000);
   }
   c.group = GetTrimmedAttribute(root, "MotionPathSequenceGroup");
   c.order = std::clamp(GetNetIntAttribute(root, "MotionPathSequenceOrder", 0), 0, 999);
   using Motion::Role;
   using Motion::Source;
   const auto id = [&](Role role, Source source, const char* name, int maximum) { c.ids[static_cast<size_t>(role)][static_cast<size_t>(source)] = std::clamp(GetNetIntAttribute(root, name, 0), 0, maximum); };
   id(Role::Start, Source::Solenoid, "MotionPathSolenoidID", 255);
   id(Role::Start, Source::Lamp, "MotionPathLampID", 255);
   id(Role::Start, Source::B2S, "MotionPathB2SID", 250);
   id(Role::Start, Source::Switch, "MotionPathSwitchID", 255);
   id(Role::Stop, Source::Solenoid, "MotionPathStopSolenoidID", 255);
   id(Role::Stop, Source::Lamp, "MotionPathStopLampID", 255);
   id(Role::Stop, Source::B2S, "MotionPathStopB2SID", 250);
   id(Role::Stop, Source::Switch, "MotionPathStopSwitchID", 255);
   id(Role::Resume, Source::Solenoid, "MotionPathResumeSolenoidID", 255);
   id(Role::Resume, Source::Lamp, "MotionPathResumeLampID", 255);
   id(Role::Resume, Source::B2S, "MotionPathResumeB2SID", 250);
   id(Role::Resume, Source::Switch, "MotionPathResumeSwitchID", 255);
   id(Role::Remove, Source::Solenoid, "MotionPathRemoveSolenoidID", 255);
   id(Role::Remove, Source::Lamp, "MotionPathRemoveLampID", 255);
   id(Role::Remove, Source::B2S, "MotionPathRemoveB2SID", 250);
   id(Role::Remove, Source::Switch, "MotionPathRemoveSwitchID", 255);
   return c;
}

// The AutomaticRotation* attributes fall back to the SnippitRotating* ones when the bulb has a SnippitType attribute
// (hand edited files only, the Designer removes them), and to nothing otherwise (reference fB:3046-3106)
static int NativeSteps(const tinyxml2::XMLNode& root) noexcept
{
   int steps = 0;
   if (HasAttribute(root, "AutomaticRotationSteps"))
      steps = GetNetIntAttribute(root, "AutomaticRotationSteps", 0);
   else if (HasAttribute(root, "SnippitType"))
   {
      if (HasAttribute(root, "SnippitRotatingSteps"))
         steps = GetNetIntAttribute(root, "SnippitRotatingSteps", 0);
      else if (const int angle = GetNetIntAttribute(root, "SnippitRotatingAngle", 0); angle != 0)
         steps = 360 / angle;
   }
   return std::clamp(steps, 4, 360); // The tick uses at least 4 steps (PB:436)
}

static int NativeInterval(const tinyxml2::XMLNode& root) noexcept
{
   int interval = 0;
   if (HasAttribute(root, "AutomaticRotationInterval"))
      interval = GetNetIntAttribute(root, "AutomaticRotationInterval", 0);
   else if (HasAttribute(root, "SnippitType"))
      interval = GetNetIntAttribute(root, "SnippitRotatingInterval", 0);
   return std::clamp(interval, 10, 500);
}

static bool NativeAntiClockwise(const tinyxml2::XMLNode& root) noexcept
{
   if (HasAttribute(root, "AutomaticRotationDirection"))
      return GetNetIntAttribute(root, "AutomaticRotationDirection", 0) == 1;
   return HasAttribute(root, "SnippitType") && GetNetIntAttribute(root, "SnippitRotatingDirection", 0) == 1;
}

static int NativeStopBehaviour(const tinyxml2::XMLNode& root) noexcept
{
   if (HasAttribute(root, "AutomaticRotationStopBehaviour"))
      return GetNetIntAttribute(root, "AutomaticRotationStopBehaviour", 0);
   return HasAttribute(root, "SnippitType") ? GetNetIntAttribute(root, "SnippitRotatingStopBehaviour", 0) : 0;
}

// Trigger type: 2 is a solenoid, 5 a switch and every other value the lamp channel (so the Designer's B2S command, 6,
// is the lamp channel too); without the attribute the bulb keeps its own ROM type (reference fB:3084-3093)
static B2SRomIDType NativeTriggerType(const tinyxml2::XMLNode& root, const B2SRomIDType romType) noexcept
{
   if (IsAttributeOne(root, "AutomaticRotationContinuous"))
      return B2SRomIDType::NotDefined;
   if (!HasAttribute(root, "AutomaticRotationTriggerType"))
      return romType;
   const int type = GetNetIntAttribute(root, "AutomaticRotationTriggerType", 0);
   return type == 2 ? B2SRomIDType::Solenoid : type == 5 ? B2SRomIDType::Switch : B2SRomIDType::Lamp;
}

static int NativeTriggerId(const tinyxml2::XMLNode& root, const int romId) noexcept
{
   if (IsAttributeOne(root, "AutomaticRotationContinuous"))
      return 0;
   return std::max(0, HasAttribute(root, "AutomaticRotationTriggerID") ? GetNetIntAttribute(root, "AutomaticRotationTriggerID", 0) : romId);
}

// PivotX/PivotY/PivotDuration share the reference's variables with ExternalPivotX/ExternalPivotY/ExternalPivotDuration
// and are parsed after them, so a Pivot* value wins (fB:3177-3188)
static float GetPivotFloatAttribute(const tinyxml2::XMLNode& root, const char* pivotName, const char* externalName, float defVal) noexcept
{
   return GetNetFloatAttribute(root, pivotName, GetNetFloatAttribute(root, externalName, defVal));
}

// Bounding box of the pixels with a non zero alpha (reference GetBoundingRectangle, fB:5042-5082); false when there is none
static bool GetAlphaBounds(VPXTexture texture, int& x, int& y, int& w, int& h) noexcept
{
   const VPXTextureInfo* info = texture ? GetTextureInfo(texture) : nullptr;
   if (info == nullptr || info->width == 0 || info->height == 0)
      return false;
   x = 0;
   y = 0;
   w = static_cast<int>(info->width);
   h = static_cast<int>(info->height);
   if (info->format != VPXTEXFMT_sRGBA8 || info->data == nullptr)
      return true; // No alpha channel: every pixel is opaque
   const uint8_t* const pixels = static_cast<const uint8_t*>(info->data);
   int minX = w, minY = h, maxX = -1, maxY = -1;
   for (int py = 0; py < h; py++)
      for (int px = 0; px < w; px++)
         if (pixels[(static_cast<size_t>(py) * w + px) * 4 + 3] != 0)
         {
            minX = std::min(minX, px);
            minY = std::min(minY, py);
            maxX = std::max(maxX, px);
            maxY = std::max(maxY, py);
         }
   if (maxX < 0)
      return false;
   x = minX;
   y = minY;
   w = maxX - minX + 1;
   h = maxY - minY + 1;
   return true;
}

// B2S Pro Dynamic Front Lighting (reference fB:3032-3040, 3313-3316, 3355-3367). The seven Dynamic* parameters are read only for a light with a
// field, absent ones keep their defaults, a present but unparsable number reads as 0 and is clamped (the reference aborts the load there), a colour
// that is not R.G.B keeps white. A field is the alpha plane of the image: an image without alpha is fully opaque, other formats disable the light
static DynamicFront ParseDynamicFront(const tinyxml2::XMLNode& root) noexcept
{
   DynamicFront d;
   if (!IsAttributeOne(root, "DynamicFrontLighting") || IsAttributeOne(root, "IsImageSnippit"))
      return d;
   const VPXTexture texture = GetTextureAttribute(root, ""s, "DynamicFrontLightField"s);
   if (texture == nullptr)
      return d;
   const VPXTextureInfo* const info = GetTextureInfo(texture);
   if (info != nullptr && info->width > 0 && info->height > 0 && info->data != nullptr && (info->format == VPXTEXFMT_sRGBA8 || info->format == VPXTEXFMT_sRGB8))
   {
      d.fieldW = static_cast<int>(info->width);
      d.fieldH = static_cast<int>(info->height);
      const size_t count = static_cast<size_t>(d.fieldW) * static_cast<size_t>(d.fieldH);
      d.field.resize(count, uint8_t(255));
      if (info->format == VPXTEXFMT_sRGBA8)
         for (size_t i = 0; i < count; i++)
            d.field[i] = static_cast<const uint8_t*>(info->data)[i * 4 + 3];
      d.enabled = true;
   }
   DeleteTexture(texture);
   if (!d.enabled)
      return d;
   d.showLight = IsAttributeOne(root, "DynamicFrontShowLight");
   d.separate = IsAttributeOne(root, "SeparateLightLayer");
   d.targetId = std::max(0, GetNetIntAttribute(root, "DynamicFrontTargetSnippetID", 0));
   DynamicLight::Params& p = d.params;
   p.exposure = GetIntOrDefault(root, "DynamicLightExposure", 100);
   p.style = GetNetIntAttribute(root, "DynamicFlasherStyle", 1);
   p.saturation = GetNetIntAttribute(root, "DynamicSaturation", 100);
   p.protection = GetNetIntAttribute(root, "DynamicHighlightProtection", 0);
   p.lift = GetNetIntAttribute(root, "DynamicDarkAreaLift", 0);
   p.contrast = GetNetIntAttribute(root, "DynamicTransmissionContrast", 140);
   p.kelvin = GetNetIntAttribute(root, "DynamicLightTemperature", 4000);
   if (const tinyxml2::XMLElement* const node = root.ToElement(); node != nullptr && node->Attribute("DynamicLightColor") != nullptr)
   {
      int channel[3] = { 255, 255, 255 };
      int count = 0;
      const string text = node->Attribute("DynamicLightColor");
      size_t start = 0;
      while (count < 3 && start <= text.size())
      {
         const size_t dot = text.find('.', start);
         const string part = text.substr(start, dot == string::npos ? string::npos : dot - start);
         const string s = TrimNet(part.c_str());
         int value = 0;
         const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
         if (ec != std::errc() || ptr != s.data() + s.size())
            break;
         channel[count++] = std::clamp(value, 0, 255);
         if (dot == string::npos)
            break;
         start = dot + 1;
      }
      if (count == 3)
      {
         p.colorR = channel[0];
         p.colorG = channel[1];
         p.colorB = channel[2];
      }
   }
   p = DynamicLight::ClampParams(p);
   return d;
}

B2SBulb::B2SBulb(const tinyxml2::XMLNode& root) noexcept
   : m_id(GetIntAttribute(root, ""s, "RomID"s, 0))
   , m_name(GetStringAttribute(root, ""s, "Name"s, ""s))
   , m_b2sId(GetIntAttribute(root, ""s, "B2SID"s, -1))
   , m_b2sValue(GetIntAttribute(root, ""s, "B2SValue"s, 0))
   // A B2SID replaces the ROM binding entirely: it is a lamp ROM id with B2SValue as matching value (reference behaviour)
   // B2S Pro: a pivot's trigger owns its states, so it has no ROM visibility binding (reference fB:3339-3341)
   , m_romId(IsAttributeOne(root, "PivotAnimation") ? 0 : m_b2sId >= 0 ? m_b2sId : GetIntAttribute(root, ""s, "RomID"s, -1))
   , m_romIdType(m_b2sId >= 0 ? B2SRomIDType::Lamp : static_cast<B2SRomIDType>(GetIntAttribute(root, ""s, "RomIDType"s, 0)))
   , m_romInverted(m_b2sId >= 0 ? false : GetBoolAttribute(root, ""s, "RomInverted"s, false))
   , m_initialState(GetIntAttribute(root, ""s, "InitialState"s, 0) == 1) // the server lights a bulb at startup only for exactly 1 (fB:4743-4774)
   , m_initialStateOne(GetIntAttribute(root, ""s, "InitialState"s, 0) == 1)
   , m_bulbId(GetIntAttribute(root, ""s, "ID"s, 0))
   , m_dualMode(static_cast<B2SDualMode>(GetIntAttribute(root, ""s, "DualMode"s, 0)))
   , m_intensity(GetIntAttribute(root, ""s, "Intensity"s, 0))
   , m_zOrder(GetIntAttribute(root, ""s, "ZOrder"s, 0))
   , m_lightColor(GetColorAttribute(root, ""s, "LightColor"s, vec4(1.f, 1.f, 1.f, 1.f)))
   , m_dodgeColor(GetColorAttribute(root, ""s, "DodgeColor"s, vec4(0.f, 0.f, 0.f, 1.f)))
   , m_illuminationMode(GetIntAttribute(root, ""s, "IlluMode"s, 0))
   , m_visible(GetBoolAttribute(root, ""s, "Visible"s, true))
   , m_locationX(GetIntAttribute(root, ""s, "LocX"s, 0))
   , m_locationY(GetIntAttribute(root, ""s, "LocY"s, 0))
   , m_width(GetIntAttribute(root, ""s, "Width"s, 0))
   , m_height(GetIntAttribute(root, ""s, "Height"s, 0))
   , m_isImageSnippit(GetBoolAttribute(root, ""s, "IsImageSnippit"s, false))
   , m_snippitType(static_cast<B2SSnippitType>(GetIntAttribute(root, ""s, "SnippitType"s, 0)))
   , m_snippitRotatingSteps(GetIntAttribute(root, ""s, "SnippitRotatingSteps"s, -1) >= 0
           ? GetIntAttribute(root, ""s, "SnippitRotatingSteps"s, 0)
           // the server divides as floating point and rounds half to even: CInt(360 / angle) (fB:3055-3062), so 11 degrees give 33 steps
           : (GetIntAttribute(root, ""s, "SnippitRotatingAngle"s, 0) != 0 ? static_cast<int>(std::nearbyint(360.0 / GetIntAttribute(root, ""s, "SnippitRotatingAngle"s, 1))) : 0))
   , m_snippitRotatingInterval(GetIntAttribute(root, ""s, "SnippitRotatingInterval"s, 0))
   , m_snippitRotatingDirection(static_cast<B2SSnippitRotationDirection>(GetIntAttribute(root, ""s, "SnippitRotatingDirection"s, 0)))
   , m_snippitRotatingStopBehaviour(static_cast<B2SSnippitRotationStopBehaviour>(GetIntAttribute(root, ""s, "SnippitRotatingStopBehaviour"s, 0)))
   , m_image(GetTextureAttribute(root, ""s, BulbImageAttribute(root)))
   , m_offImage(GetTextureAttribute(root, ""s, "OffImage"s))
   , m_text(GetStringAttribute(root, ""s, "Text"s, ""s))
   , m_textAlignment(GetIntAttribute(root, ""s, "TextAlignment"s, 0))
   , m_fontName(GetStringAttribute(root, ""s, "FontName"s, ""s))
   , m_fontSize(GetIntAttribute(root, ""s, "FontSize"s, 0))
   , m_fontStyle(GetIntAttribute(root, ""s, "FontStyle"s, 0))
   , m_flasherPulseDuration(GetFlasherPulseDuration(root))
   , m_pivotAnimation(IsAttributeOne(root, "PivotAnimation"))
   , m_pivotTriggerType(GetNetIntAttribute(root, "PivotTriggerType", 1))
   , m_pivotTriggerId(GetNetIntAttribute(root, "PivotTriggerID", 0))
   , m_pivotAutomaticOscillation(m_pivotTriggerType != 0 && (m_pivotTriggerType == 4 || IsAttributeOne(root, "PivotAutomaticOscillation")))
   , m_pivotX(GetPivotFloatAttribute(root, "PivotX", "ExternalPivotX", 0.5f))
   , m_pivotY(GetPivotFloatAttribute(root, "PivotY", "ExternalPivotY", 0.5f))
   , m_pivotTipX(std::clamp(GetNetFloatAttribute(root, "PivotTipX", 0.9f), 0.f, 1.f))
   , m_pivotTipY(std::clamp(GetNetFloatAttribute(root, "PivotTipY", 0.5f), 0.f, 1.f))
   , m_pivotDownAngle(GetNetFloatAttribute(root, "PivotDownAngle", 0.f))
   , m_pivotUpAngle(GetNetFloatAttribute(root, "PivotUpAngle", -30.f))
   , m_pivotDuration(std::clamp(GetNetIntAttribute(root, "PivotDuration", GetNetIntAttribute(root, "ExternalPivotDuration", 80)), 10, 5000))
   , m_pivotRandomStrength(std::clamp(GetNetFloatAttribute(root, "PivotRandomStrength", 0.f), 0.f, 100.f))
   , m_pivotDownTrigger(GetTrimmedAttribute(root, "PivotDownTrigger"))
   , m_pivotUpTrigger(GetTrimmedAttribute(root, "PivotUpTrigger"))
   , m_externalPivotGroup(GetTrimmedAttribute(root, "ExternalPivotGroup"))
   , m_externalPivotRepresentative(IsAttributeOne(root, "ExternalPivotRepresentative"))
   , m_externalPivotAngle(GetNetFloatAttribute(root, "ExternalPivotAngle", 0.f))
   , m_externalGridName(GetTrimmedAttribute(root, "ExternalGridName"))
   , m_externalGridRepresentative(IsAttributeOne(root, "ExternalGridRepresentative"))
   , m_externalGridDuration(std::clamp(GetNetIntAttribute(root, "ExternalGridDuration", 45), 10, 1000))
   , m_nativeRotation(IsNativeRotation(root))
   , m_nativeContinuous(m_nativeRotation && IsAttributeOne(root, "AutomaticRotationContinuous"))
   , m_nativeTriggerType(m_nativeRotation ? NativeTriggerType(root, m_romIdType) : B2SRomIDType::NotDefined)
   , m_nativeTriggerId(m_nativeRotation ? NativeTriggerId(root, m_romId) : 0)
   , m_nativeSteps(NativeSteps(root))
   , m_nativeInterval(NativeInterval(root))
   , m_nativeAntiClockwise(NativeAntiClockwise(root))
   , m_nativeStopBehaviour(NativeStopBehaviour(root))
   , m_behindCanvas(IsAttributeOne(root, "SnippitBehindCanvas"))
   , m_motion(ParseMotionConfig(root))
   , m_physics(ParsePhysicsConfig(root))
   , m_dfl(ParseDynamicFront(root))
{
   if (m_physics.ball)
      DecodePhysicsMask(root, m_physicsMask, m_physicsMaskW, m_physicsMaskH);
   // Rotating images are always shown at startup; lamp writes only toggle their rotation (reference behavior)
   if (m_snippitType == B2SSnippitType::MechRotatingImage || m_snippitType == B2SSnippitType::SelfRotatingImage)
      m_brightness = 1.f;
   else
      m_brightness = m_initialState ? 1.f : 0.f;
   // B2S Pro: a Dynamic Front light is shown at startup only with InitialState 1 (ShowStartupDynamicFrontLights, reference fB:344, 4740-4750)
   if (m_dfl.enabled)
      m_brightness = m_initialStateOne ? 1.f : 0.f;

   // The reference crops standard images without native rotation, rolling or full resolution image to their non
   // transparent pixels at load (fB:3321-3325, fB:5084-5117)
   m_cropEligible = m_snippitType == B2SSnippitType::StandardImage && !IsAttributeOne(root, "AutomaticRotationEnabled")
      && !(m_isImageSnippit && IsAttributeOne(root, "MotionPathRollEnabled")) && BulbImageAttribute(root) == "Image"s;

   // B2S Pro: a bulb with a motion path or in a sequence group takes the same rectangle, whose centre the route moves (reference
   // PB:830-857); old files crop it to its alpha box first, like the reference
   if (m_pivotAnimation || !m_externalPivotGroup.empty() || !m_externalGridName.empty() || m_nativeRotation || m_motion.HasMotion() || m_physics.ball)
   {
      // The location moves by the crop offset in image pixels and the size scales by the crop ratio, so the hinge fractions apply to the
      // cropped rectangle, and position grids move between the centres of the cropped rectangles
      ComputeCrop();
      // Rest pose: the down angle, or the unrotated pose for automatic swings (BD:413). A pivot whose trigger is not
      // registered (no trigger id) stays unrotated (BD:408); named-command pivots get theirs at registration
      if (m_pivotAnimation && (m_pivotTriggerType == 4 || (m_pivotTriggerType != 0 && m_pivotTriggerId > 0)))
         m_pivotAngle = m_pivotAutomaticOscillation ? 0.f : m_pivotDownAngle;
   }
}

// The alpha cropped texture area and rectangle of the bulb (reference fB:3321-3325, 5084-5117): the location moves by the crop offset in image
// pixels and the size scales by the crop ratio
void B2SBulb::ComputeCrop() noexcept
{
   m_cropComputed = true;
   const VPXTextureInfo* const info = m_image ? GetTextureInfo(m_image) : nullptr;
   m_pivotTexW = info ? static_cast<int>(info->width) : 0;
   m_pivotTexH = info ? static_cast<int>(info->height) : 0;
   m_pivotDstX = m_locationX;
   m_pivotDstY = m_locationY;
   m_pivotDstW = m_width;
   m_pivotDstH = m_height;
   int x, y, w, h;
   if (m_cropEligible && info && GetAlphaBounds(m_image, x, y, w, h))
   {
      int offX, offY, offW, offH;
      bool apply = true;
      if (m_offImage)
      {
         // With an OffImage, the crop is the union of both boxes, and no crop when the off image is empty
         if (GetAlphaBounds(m_offImage, offX, offY, offW, offH))
         {
            const int right = std::max(x + w, offX + offW), bottom = std::max(y + h, offY + offH);
            x = std::min(x, offX);
            y = std::min(y, offY);
            w = right - x;
            h = bottom - y;
         }
         else
            apply = false;
      }
      if (apply)
      {
         m_pivotTexX = x;
         m_pivotTexY = y;
         m_pivotTexW = w;
         m_pivotTexH = h;
         // CInt rounds half to even, as nearbyint does in the default rounding mode
         m_pivotDstW = static_cast<int>(std::nearbyint(m_width * (static_cast<double>(w) / info->width)));
         m_pivotDstH = static_cast<int>(std::nearbyint(m_height * (static_cast<double>(h) / info->height)));
         m_pivotDstX = m_locationX + x;
         m_pivotDstY = m_locationY + y;
      }
   }
}

// The pixels of the cropped image area as straight RGBA (the Dynamic Front kernel reads the picture box image of the reference)
void B2SBulb::ExtractPixels() noexcept
{
   m_dflPixels.clear();
   const VPXTextureInfo* const info = m_image ? GetTextureInfo(m_image) : nullptr;
   if (info == nullptr || info->data == nullptr || (info->format != VPXTEXFMT_sRGBA8 && info->format != VPXTEXFMT_sRGB8))
      return;
   const int width = static_cast<int>(info->width), height = static_cast<int>(info->height);
   if (m_pivotTexW <= 0 || m_pivotTexH <= 0 || m_pivotTexX < 0 || m_pivotTexY < 0 || m_pivotTexX + m_pivotTexW > width || m_pivotTexY + m_pivotTexH > height)
      return;
   const bool alpha = info->format == VPXTEXFMT_sRGBA8;
   const uint8_t* const data = static_cast<const uint8_t*>(info->data);
   m_dflPixels.resize(static_cast<size_t>(m_pivotTexW) * static_cast<size_t>(m_pivotTexH) * 4);
   for (int y = 0; y < m_pivotTexH; y++)
      for (int x = 0; x < m_pivotTexW; x++)
      {
         const uint8_t* const s = data + (static_cast<size_t>(m_pivotTexY + y) * width + (m_pivotTexX + x)) * (alpha ? 4 : 3);
         uint8_t* const d = &m_dflPixels[(static_cast<size_t>(y) * m_pivotTexW + x) * 4];
         d[0] = s[0];
         d[1] = s[1];
         d[2] = s[2];
         d[3] = alpha ? s[3] : uint8_t(255);
         // The reference's picture box image is the cropped copy of the decoded image (a GDI+ copy re-quantises straight alpha colour)
         if (m_cropEligible)
            for (int k = 0; k < 3; k++)
               d[k] = DynamicLight::RequantizeChannel(d[k], d[3]);
      }
}

B2SBulb::~B2SBulb()
{
   DeleteTexture(m_image);
   DeleteTexture(m_offImage);
}

void B2SBulb::UpdateRotation(float elapsedInS)
{
   if (m_snippitType != B2SSnippitType::SelfRotatingImage)
      return;

   if (const int request = m_rotRequest.exchange(0); request != 0)
   {
      if (request == 1)
      {
         m_rotating = m_snippitRotatingSteps > 0 && m_snippitRotatingInterval > 0;
         m_rotateSlowDown = 0;
         m_slowdownAccMs = 0.f;
         m_rotateRunTillEnd = false;
         m_rotateRunToFirstStep = false;
         m_rotIntervalMs = static_cast<float>(m_snippitRotatingInterval);
      }
      else if (m_rotating)
      {
         switch (m_snippitRotatingStopBehaviour)
         {
         case B2SSnippitRotationStopBehaviour::SpinOff: m_rotateSlowDown = 1; break;
         case B2SSnippitRotationStopBehaviour::RunAnimationTillEnd: m_rotateRunTillEnd = true; break;
         case B2SSnippitRotationStopBehaviour::RunAnimationToFirstStep: m_rotateRunToFirstStep = true; break;
         default: m_rotating = false; break;
         }
      }
   }

   if (!m_rotating)
      return;

   const float direction = (m_snippitRotatingDirection == B2SSnippitRotationDirection::AntiClockwise) ? -1.f : 1.f;
   const float stepAngle = 360.f / static_cast<float>(m_snippitRotatingSteps);
   m_selfRotAngle += direction * stepAngle * (elapsedInS * 1000.f / m_rotIntervalMs);

   bool wrapped = false;
   while (m_selfRotAngle >= 360.f)
   {
      m_selfRotAngle -= 360.f;
      wrapped = true;
   }
   while (m_selfRotAngle < 0.f)
   {
      m_selfRotAngle += 360.f;
      wrapped = true;
   }

   if (m_rotateSlowDown > 0)
   {
      // Spin off: interval grows by 3ms per elapsed step for 25 steps then stops (reference behaviour)
      m_slowdownAccMs += elapsedInS * 1000.f;
      while (m_slowdownAccMs >= m_rotIntervalMs && m_rotateSlowDown <= 25)
      {
         m_slowdownAccMs -= m_rotIntervalMs;
         m_rotIntervalMs += 3.f;
         m_rotateSlowDown++;
      }
      if (m_rotateSlowDown > 25)
      {
         m_rotating = false;
         m_rotateSlowDown = 0;
      }
   }
   else if ((m_rotateRunTillEnd || m_rotateRunToFirstStep) && wrapped)
   {
      // Complete the current revolution then stop on the first step (angle 0)
      m_selfRotAngle = 0.f;
      m_rotating = false;
      m_rotateRunTillEnd = false;
      m_rotateRunToFirstStep = false;
   }
}


B2SImage::B2SImage()
   : m_image(nullptr)
   , m_filename(""s)
   , m_romId(0)
   , m_romIdType(B2SRomIDType::NotDefined)
   , m_romInverted(false)
{
}

B2SImage::B2SImage(const tinyxml2::XMLNode& root) noexcept
   : m_image(GetTextureAttribute(root, ""s, "Value"s))
   , m_filename(GetStringAttribute(root, ""s, "FileName"s, ""s))
   , m_romId(GetIntAttribute(root, ""s, "RomID"s, 0))
   , m_romIdType(static_cast<B2SRomIDType>(GetIntAttribute(root, ""s, "RomIDType"s, 0)))
   , m_romInverted(GetBoolAttribute(root, ""s, "RomInverted"s, false))
{
}

B2SImage::~B2SImage()
{
   DeleteTexture(m_image);
}


B2SReelImage::B2SReelImage(const tinyxml2::XMLNode& root, int setId) noexcept
   : m_name(GetStringAttribute(root, ""s, "Name"s, ""s) + (setId > 0 ? "_"s + std::to_string(setId) : ""s))
   , m_countOfIntermediate(GetIntAttribute(root, ""s, "CountOfIntermediates"s, 0))
   , m_image(GetTextureAttribute(root, ""s, "Image"s))
{
   for (int i = 1; i <= m_countOfIntermediate; i++)
      m_intermediates.push_back(GetTextureAttribute(root, ""s, "IntermediateImage"s + std::to_string(i)));
}

B2SReelImage::~B2SReelImage()
{
   DeleteTexture(m_image);
   for (const VPXTexture tex : m_intermediates)
      DeleteTexture(tex);
}


// The old flat reel format, <Reels><Image Name Image/>: the server reads these and then neither <Reels><Images> nor the illuminated
// images (fB:3944-3952)
static bool HasFlatReelImages(const tinyxml2::XMLNode& root) noexcept
{
   const tinyxml2::XMLElement* reels = GetNode(root, "Reels"s);
   return reels != nullptr && reels->FirstChildElement("Image") != nullptr;
}

static vector<std::unique_ptr<B2SReelImage>> GetIlluminatedReelImages(const tinyxml2::XMLNode& root) noexcept
{
   vector<std::unique_ptr<B2SReelImage>> list;
   const tinyxml2::XMLElement* node = HasFlatReelImages(root) ? nullptr : GetNode(root, "Reels/IlluminatedImages"s);
   if (node == nullptr)
      return list;
   // Flat illuminated images and per-set images (names suffixed with _setId)
   for (auto img = node->FirstChildElement("IlluminatedImage"); img != nullptr; img = img->NextSiblingElement("IlluminatedImage"))
      list.push_back(std::make_unique<B2SReelImage>(*img));
   for (auto set = node->FirstChildElement("Set"); set != nullptr; set = set->NextSiblingElement("Set"))
   {
      const int setId = set->IntAttribute("ID");
      for (auto img = set->FirstChildElement("IlluminatedImage"); img != nullptr; img = img->NextSiblingElement("IlluminatedImage"))
         list.push_back(std::make_unique<B2SReelImage>(*img, setId));
   }
   return list;
}

B2SReel::B2SReel(const tinyxml2::XMLNode& root) noexcept
   : m_images(!GetNode(root, "Reels"s)    ? vector<std::unique_ptr<B2SReelImage>>()
              : HasFlatReelImages(root) ? GetPtrList<B2SReelImage>(*GetNode(root, "Reels"s), ""s, "Image"s)
                                        : GetPtrList<B2SReelImage>(*GetNode(root, "Reels"s), "Images"s, "Image"s))
   , m_illuImages(GetIlluminatedReelImages(root))
{
}

const B2SReelImage* B2SReel::GetImage(const string& name, int index, bool illuminated, int setId) const
{
   // The reel type suffix defines the digit field width: "reel_00" -> 2 digits, "reel_0" -> 1 digit
   string base;
   int width = 0;
   if (name.ends_with("_00"))
   {
      width = 2;
      base = name.substr(0, name.length() - 2);
   }
   else if (name.ends_with("_0"))
   {
      width = 1;
      base = name.substr(0, name.length() - 1);
   }
   else
      return nullptr;
   string imgName;
   if (index < 0)
      imgName = std::format("{}Empty", base);
   else if (width == 2)
      imgName = std::format("{}{:02}", base, index);
   else
      imgName = std::format("{}{:01}", base, index);
   if (illuminated && setId > 0)
      imgName += "_"s + std::to_string(setId);
   const auto& pool = illuminated ? m_illuImages : m_images;
   for (const auto& img : pool)
   {
      if (img->m_name == imgName)
         return img.get();
   }
   return nullptr;
}

void B2SReelDigit::SetTarget(int value, bool animate)
{
   m_target = value;
   if (value < 0 || value >= 10 || m_current < 0 || m_current >= 10)
   {
      m_current = value;
      m_rolling = false;
      m_intermediate = 0;
      m_settle = 0;
      m_accMs = 0.f;
   }
   else if (!m_rolling)
   {
      if (animate && m_current != m_target)
      {
         m_rolling = true;
         m_intermediate = 0;
         m_settle = 0;
      }
      else
         m_current = value;
   }
   // While rolling, only the goal is updated (a roll in progress is never cancelled)
}

void B2SReelDigit::SetIlluminated(bool value, int intermediates)
{
   if (m_illuminated != value)
   {
      m_illuminated = value;
      if (m_rolling)
         m_intermediate = std::max(m_intermediate, intermediates);
   }
}

bool B2SReelDigit::Update(float elapsedInS, int rollingIntervalMs, int intermediates, bool rollUp)
{
   if (!m_rolling)
   {
      // No rolling animation: snap to the target
      m_current = m_target;
      m_intermediate = 0;
      m_settle = 0;
      m_accMs = 0.f;
      return false;
   }

   if (rollingIntervalMs < 10)
      rollingIntervalMs = 101; // B2SReelBox default rolling interval
   if (intermediates < 0)
      intermediates = 3; // Automatic intermediate count
   // Each digit step is spread over intermediate images + advance + settle ticks
   const float stepMs = static_cast<float>(rollingIntervalMs) / static_cast<float>(intermediates + 2);
   m_accMs += elapsedInS * 1000.f;
   bool advanced = false;
   while (m_rolling && m_accMs >= stepMs)
   {
      m_accMs -= stepMs;
      if (m_settle > 0)
      {
         if (--m_settle == 0 && m_current == m_target)
            m_rolling = false;
      }
      else if (m_intermediate < intermediates)
         m_intermediate++;
      else
      {
         m_current += rollUp ? 1 : -1;
         if (m_current > 9)
            m_current = 0;
         else if (m_current < 0)
            m_current = 9;
         m_intermediate = 0;
         m_settle = 2;
         advanced = true;
      }
   }
   return advanced;
}


B2SScore::B2SScore(const tinyxml2::XMLNode& root) noexcept
   : m_id(GetIntAttribute(root, ""s, "ID"s, 0))
   , m_b2sStartDigit(GetIntAttribute(root, ""s, "B2SStartDigit"s, 0))
   , m_b2sScoreType(static_cast<B2SScoreType>(GetIntAttribute(root, ""s, "B2SScoreType"s, 0)))
   , m_b2sPlayerNo(GetIntAttribute(root, ""s, "B2SPlayerNo"s, 0))
   , m_reelType(GetStringAttribute(root, ""s, "ReelType"s, ""s))
   , m_reelIlluLocation(GetIntAttribute(root, ""s, "ReelIlluLocation"s, 0))
   , m_reelIlluIntensity(GetIntAttribute(root, ""s, "ReelIlluIntensity"s, 0))
   , m_reelIlluB2SID(GetIntAttribute(root, ""s, "ReelIlluB2SID"s, 0))
   , m_reelIlluB2SIDType(GetIntAttribute(root, ""s, "ReelIlluB2SIDType"s, 0))
   , m_reelIlluB2SValue(GetIntAttribute(root, ""s, "ReelIlluB2SValue"s, 0))
   , m_reelIlluImageSet(GetIntAttribute(root, ""s, "ReelIlluImageSet"s, 0))
   , m_reelLitColor(GetColorAttribute(root, ""s, "ReelLitColor"s, vec4(1.f, 1.f, 1.f, 1.f)))
   , m_reelDarkColor(GetColorAttribute(root, ""s, "ReelDarkColor"s, vec4(0.f, 0.f, 0.f, 1.f)))
   , m_glow(GetIntAttribute(root, ""s, "Glow"s, 0))
   , m_thickness(GetIntAttribute(root, ""s, "Thickness"s, 0))
   , m_shear(GetIntAttribute(root, ""s, "Shear"s, 0))
   , m_digits(std::max(0, GetIntAttribute(root, ""s, "Digits"s, 0)))
   , m_spacing(GetIntAttribute(root, ""s, "Spacing"s, 0))
   , m_displayState(GetIntAttribute(root, ""s, "DisplayState"s, 0))
   , m_locX(GetIntAttribute(root, ""s, "LocX"s, 0))
   , m_locY(GetIntAttribute(root, ""s, "LocY"s, 0))
   , m_width(GetIntAttribute(root, ""s, "Width"s, 0))
   , m_height(GetIntAttribute(root, ""s, "Height"s, 0))
   , m_soundName(GetStringAttribute(root, ""s, "Sound"s, "stille"s))
   , m_soundNames(
        [&root, this]() -> vector<string>
        {
           vector<string> names;
           for (int i = 1; i <= m_digits; i++)
              names.push_back(GetStringAttribute(root, ""s, "Sound"s + std::to_string(i), ""s));
           return names;
        }())
   , m_ledSegments(
        [this]()
        {
           // ReelType suffix is the segment count: "Dream7LED7" -> 7, "RenderedLED14" -> 14
           size_t pos = m_reelType.find_last_not_of("0123456789");
           if (pos == string::npos || pos == m_reelType.length() - 1)
              return 0;
           const int count = atoi(m_reelType.c_str() + pos + 1);
           if (count == 7 || count == 8)
              return 7;
           if (count == 9 || count == 10)
              return 10;
           if (count == 14)
              return 14;
           return 0;
        }())
   , m_scoreType(StartsWithCaseInsensitive(m_reelType, "dream7"s)  ? B2SScoreRenderer::Dream7
           : StartsWithCaseInsensitive(m_reelType, "rendered"s)    ? B2SScoreRenderer::RenderedLED
           : StartsWithCaseInsensitive(m_reelType, "LED"s)         ? B2SScoreRenderer::LED
           : StartsWithCaseInsensitive(m_reelType, "ImportedLED"s) ? B2SScoreRenderer::ImportedLED
                                                                   : B2SScoreRenderer::Reel)
   , m_behindCanvas(IsAttributeOne(root, "BehindCanvas"))
   , m_keepDigitAspect(IsAttributeOne(root, "KeepDigitAspect"))
   , m_reelDigitWidth(std::max(0, GetNetIntAttribute(root, "ReelDigitWidth", 0)))
   , m_reel3DEnabled(IsAttributeOne(root, "Reel3DEnabled"))
   , m_reel3DBrightness(std::clamp(GetIntOrDefault(root, "Reel3DBrightness", 100), 0, 400))
   , m_reel3DTemperature(std::clamp(GetIntOrDefault(root, "Reel3DTemperature", 4000), 2000, 6500))
   , m_reel3DDepth(std::clamp(GetIntOrDefault(root, "Reel3DDepth", 100), 0, 200))
   , m_reel3DGlass(std::clamp(GetIntOrDefault(root, "Reel3DGlass", 55), 0, 200))
{
}

vector<int> B2SScore::DistributeScore(int value) const
{
   // Right aligned score on the display digits, keeping its rightmost digits if it does not fit.
   // Reels show leading zeros (matching B2SReelDisplay::SetScore) while LED displays show blanks.
   vector<int> result(static_cast<size_t>(std::max(0, m_digits)), -1);
   const string text = std::to_string(value);
   const size_t count = std::min(text.length(), result.size());
   for (size_t i = 0; i < count; i++)
   {
      const char c = text[text.length() - 1 - i];
      int digit = -1;
      if (c >= '0' && c <= '9')
         digit = c - '0';
      result[result.size() - 1 - i] = digit;
   }
   if (m_scoreType == B2SScoreRenderer::Reel || m_scoreType == B2SScoreRenderer::LED || m_scoreType == B2SScoreRenderer::ImportedLED)
      for (int& digit : result)
         if (digit < 0)
            digit = 0;
   return result;
}

B2SScores::B2SScores(const tinyxml2::XMLNode& root) noexcept
   : m_reelCountOfIntermediates(GetIntAttribute(root, "Scores"s, "ReelCountOfIntermediates"s, 0))
   , m_reelRollingDirection(GetStringAttribute(root, "Scores"s, "ReelRollingDirection"s, "Up"s) == "Up"s ? B2SReelRollingDirection::Up : B2SReelRollingDirection::Down)
   , m_reelRollingInterval(GetIntAttribute(root, "Scores"s, "ReelRollingInterval"s, 0))
{
}


B2SAnimationStep::B2SAnimationStep(const tinyxml2::XMLNode& root) noexcept
   : m_step(GetIntAttribute(root, ""s, "Step"s, 0))
   , m_on(GetStringList(GetStringAttribute(root, ""s, "On"s, ""s), ','))
   , m_waitLoopsAfterOn(GetIntAttribute(root, ""s, "WaitLoopsAfterOn"s, 0))
   , m_off(GetStringList(GetStringAttribute(root, ""s, "Off"s, ""s), ','))
   , m_waitLoopsAfterOff(GetIntAttribute(root, ""s, "WaitLoopsAfterOff"s, 0))
   , m_pulseSwitch(GetIntAttribute(root, ""s, "PulseSwitch"s, 0))
{
}


// An animation setting that also has an older spelling, resolved like the server (fB:4220-4234): the newer attribute wins, else the
// older attribute picks between two values (exactly "1" or anything else), else the default; a result of Undefined (0) becomes the default
template <class E> static E ResolveAnimationChoice(const tinyxml2::XMLNode& root, const char* name, const char* legacyName, E defVal, E legacyOne, E legacyOther) noexcept
{
   E value = defVal;
   if (HasAttribute(root, name))
      value = static_cast<E>(GetNetIntAttribute(root, name, static_cast<int>(defVal)));
   else if (HasAttribute(root, legacyName))
      value = IsAttributeOne(root, legacyName) ? legacyOne : legacyOther;
   return value == static_cast<E>(0) ? defVal : value;
}

B2SAnimation::B2SAnimation(const tinyxml2::XMLNode& root) noexcept
   : m_name(GetStringAttribute(root, ""s, "Name"s, ""s))
   , m_dualMode(static_cast<B2SDualMode>(GetIntAttribute(root, ""s, "DualMode"s, 0)))
   , m_interval(GetIntAttribute(root, ""s, "Interval"s, 0))
   , m_loops(GetIntAttribute(root, ""s, "Loops"s, 0))
   , m_idJoin(GetStringAttribute(root, ""s, "IDJoin"s, ""s))
   , m_startAnimationAtBackglassStartup(GetBoolAttribute(root, ""s, "StartAnimationAtBackglassStartup"s, false))
   , m_lightsStateAtAnimationStart(ResolveAnimationChoice(root, "LightsStateAtAnimationStart", "AllLightsOffAtAnimationStart", B2SLightsStateAtAnimationStart::NoChange,
        B2SLightsStateAtAnimationStart::LightsOff, B2SLightsStateAtAnimationStart::NoChange))
   , m_lightsStateAtAnimationEnd(ResolveAnimationChoice(root, "LightsStateAtAnimationEnd", "ResetLightsAtAnimationEnd", B2SLightsStateAtAnimationEnd::InvolvedLightsOff,
        B2SLightsStateAtAnimationEnd::LightsReseted, B2SLightsStateAtAnimationEnd::InvolvedLightsOff))
   , m_animationStopBehaviour(ResolveAnimationChoice(root, "AnimationStopBehaviour", "RunAnimationTilEnd", B2SAnimationStopBehaviour::StopImmediatelly,
        B2SAnimationStopBehaviour::RunAnimationTillEnd, B2SAnimationStopBehaviour::StopImmediatelly))
   , m_lockInvolvedLamps(GetBoolAttribute(root, ""s, "LockInvolvedLamps"s, false))
   , m_hideScoreDisplays(GetBoolAttribute(root, ""s, "HideScoreDisplays"s, false))
   , m_bringToFront(GetBoolAttribute(root, ""s, "BringToFront"s, false))
   , m_randomStart(GetBoolAttribute(root, ""s, "RandomStart"s, false))
   , m_randomQuality(GetIntAttribute(root, ""s, "RandomQuality"s, 0))
   , m_animationSteps(GetList<B2SAnimationStep>(root, ""s, "AnimationStep"s))
   , m_runtime(std::make_unique<Runtime>())
{
   // Expand each step into on/off entry actions (matches the reference EntryAction expansion)
   for (const B2SAnimationStep& step : m_animationSteps)
   {
      const bool isOnValid = (!step.m_on.empty() && !step.m_on[0].empty()) || step.m_waitLoopsAfterOn > 0;
      const bool isOffValid = (!step.m_off.empty() && !step.m_off[0].empty()) || step.m_waitLoopsAfterOff > 0;
      int pulseSwitch = step.m_pulseSwitch;
      if (isOnValid)
      {
         m_entryActions.push_back({ step.m_on, step.m_waitLoopsAfterOn, true, isOffValid ? 1 : 0, pulseSwitch });
         pulseSwitch = 0;
      }
      if (isOffValid)
      {
         m_entryActions.push_back({ step.m_off, step.m_waitLoopsAfterOff, false, isOnValid ? -1 : 0, pulseSwitch });
         pulseSwitch = 0;
      }
      if (pulseSwitch > 0)
         m_entryActions.push_back({ {}, 0, true, 0, pulseSwitch });
   }

   // Collect all involved light groups
   for (const EntryAction& entry : m_entryActions)
      for (const string& group : entry.groups)
         if (!group.empty() && std::find(m_lightsInvolved.begin(), m_lightsInvolved.end(), group) == m_lightsInvolved.end())
            m_lightsInvolved.push_back(group);

   // Parse the IDJoin ROM event triggers (lamp/solenoid/GI string/B2S Pro switch, I prefix inverts)
   for (const string& idJoin : GetStringList(m_idJoin, ','))
   {
      if (idJoin.empty())
         continue;
      const auto toInt = [&idJoin](size_t offset) -> int
      {
         int v = 0;
         for (size_t i = offset; i < idJoin.length(); i++)
         {
            if (idJoin[i] < '0' || idJoin[i] > '9')
               return 0;
            v = v * 10 + (idJoin[i] - '0');
         }
         return v;
      };
      const char c0 = static_cast<char>(std::toupper(static_cast<unsigned char>(idJoin[0])));
      const char c1 = idJoin.length() > 1 ? static_cast<char>(std::toupper(static_cast<unsigned char>(idJoin[1]))) : ' ';
      const char c2 = idJoin.length() > 2 ? static_cast<char>(std::toupper(static_cast<unsigned char>(idJoin[2]))) : ' ';
      switch (c0)
      {
      case 'L':
         if (toInt(1) > 0)
            m_romTriggers.push_back({ B2SRomIDType::Lamp, toInt(1), false });
         break;
      case 'S':
         if (toInt(1) > 0)
            m_romTriggers.push_back({ B2SRomIDType::Solenoid, toInt(1), false });
         break;
      case 'W':
         if (toInt(1) > 0)
            m_romTriggers.push_back({ B2SRomIDType::Switch, toInt(1), false });
         break;
      case 'G':
         if (c1 == 'I')
         {
            if (toInt(2) > 0)
               m_romTriggers.push_back({ B2SRomIDType::GIString, toInt(2), false });
         }
         else if (toInt(1) > 0)
            m_romTriggers.push_back({ B2SRomIDType::GIString, toInt(1), false });
         break;
      case 'I':
         if (c1 == 'L')
         {
            if (toInt(2) > 0)
               m_romTriggers.push_back({ B2SRomIDType::Lamp, toInt(2), true });
         }
         else if (c1 == 'S')
         {
            if (toInt(2) > 0)
               m_romTriggers.push_back({ B2SRomIDType::Solenoid, toInt(2), true });
         }
         else if (c1 == 'W')
         {
            if (toInt(2) > 0)
               m_romTriggers.push_back({ B2SRomIDType::Switch, toInt(2), true });
         }
         else if (c1 == 'G')
         {
            if (c2 == 'I')
            {
               if (toInt(3) > 0)
                  m_romTriggers.push_back({ B2SRomIDType::GIString, toInt(3), true });
            }
            else if (toInt(2) > 0)
               m_romTriggers.push_back({ B2SRomIDType::GIString, toInt(2), true });
         }
         else if (toInt(1) > 0)
            m_romTriggers.push_back({ B2SRomIDType::Lamp, toInt(1), true });
         break;
      default:
         if (toInt(0) > 0)
            m_romTriggers.push_back({ B2SRomIDType::Lamp, toInt(0), false });
         break;
      }
   }

   m_runtime->triggerValues.resize(m_romTriggers.size(), 0.f);
   m_runtime->triggerPrev.resize(m_romTriggers.size(), false);
   m_runtime->triggerUpdaters.resize(m_romTriggers.size());

   if (m_startAnimationAtBackglassStartup)
      m_runtime->request = 1;
}

void B2SAnimation::BindRomTriggers(const RomTriggerResolver& resolver)
{
   for (size_t i = 0; i < m_romTriggers.size(); i++)
      m_runtime->triggerUpdaters[i] = resolver(m_romTriggers[i].romIdType, m_romTriggers[i].romId, m_romTriggers[i].inverted, &m_runtime->triggerValues[i]);
}

bool B2SAnimation::IsRunning() const { return m_runtime->running; }

void B2SAnimation::Start(bool reverse) { m_runtime->request = reverse ? 2 : 1; }

void B2SAnimation::Stop() { m_runtime->request = 3; }

void B2SAnimation::Update(float elapsedInS, const B2SAnimationEffects& fx)
{
   Runtime& rt = *m_runtime;

   // Poll ROM event triggers (IDJoin): rising edge starts, falling edge stops (random-start animations go through the pool)
   for (size_t i = 0; i < m_romTriggers.size() && i < rt.triggerUpdaters.size(); i++)
   {
      if (rt.triggerUpdaters[i])
         rt.triggerUpdaters[i]();
      const bool on = rt.triggerValues[i] >= 0.5f;
      if (on != rt.triggerPrev[i])
      {
         rt.triggerPrev[i] = on;
         if (m_randomStart)
         {
            if (fx.randomTrigger)
               fx.randomTrigger(m_romTriggers[i].romIdType, m_romTriggers[i].romId, on, this);
         }
         else
         {
            // Trigger edges do not override a pending script request
            int expected = 0;
            rt.request.compare_exchange_strong(expected, on ? 1 : 3);
         }
      }
   }

   // Consume the pending script/trigger request
   if (const int request = rt.request.exchange(0); request != 0)
   {
      if (request == 3)
      {
         if (rt.running)
         {
            // Stop request honors the configured stop behaviour
            switch (m_animationStopBehaviour)
            {
            case B2SAnimationStopBehaviour::RunAnimationTillEnd:
            case B2SAnimationStopBehaviour::RunAnimationToFirstStep:
               if (!rt.stopMeLater)
                  rt.reachedThe0Point = false;
               rt.stopMeLater = true;
               break;
            default: EndRun(fx); break;
            }
         }
      }
      else if (!rt.running) // Starting an already running animation is ignored (reference behavior)
      {
         // Dual backglass: an animation only runs in its declared mode
         if (m_dualMode == B2SDualMode::Both || fx.dualMode == B2SDualMode::Both || m_dualMode == fx.dualMode)
            BeginRun(fx, request == 2);
      }
   }

   if (!rt.running)
      return;

   // Interval is in milliseconds, clamped to avoid a lock up on degenerate animations
   const float intervalInS = static_cast<float>(m_interval > 0 ? m_interval : 1) / 1000.f;
   rt.timeUntilNextStep -= elapsedInS;
   while (rt.running && rt.timeUntilNextStep <= 0.f)
      rt.timeUntilNextStep += static_cast<float>(Tick(fx)) * intervalInS;
}

int B2SAnimation::Tick(const B2SAnimationEffects& fx)
{
   Runtime& rt = *m_runtime;
   const int count = static_cast<int>(m_entryActions.size());
   int waitLoops = 1;

   while (true)
   {
      const int index = !rt.reverse ? rt.ticker + 1 : count - rt.ticker;
      if (index < 1 || index > count)
         break;
      const EntryAction* action = &m_entryActions[index - 1];
      if (action->corrector != 0 && rt.reverse)
      {
         const int corrected = index + action->corrector;
         if (corrected >= 1 && corrected <= count)
            action = &m_entryActions[corrected - 1];
      }
      for (const string& group : action->groups)
         if (!group.empty())
            fx.setGroup(group, action->on);
      if (action->pulseSwitch > 0)
         fx.pulseSwitch(action->pulseSwitch);
      if (action->waitLoops > 0)
      {
         waitLoops = action->waitLoops;
         break;
      }
      rt.ticker++;
      if (rt.ticker >= count)
         break;
   }

   rt.ticker++;
   bool finished = false;
   if (rt.ticker >= count)
   {
      rt.reachedThe0Point = true;
      rt.loopTicker++;
      rt.ticker = 0;
      if (m_loops > 0 && rt.loopTicker >= m_loops)
      {
         rt.loopTicker = 0;
         finished = true;
      }
   }

   if (finished || (rt.stopMeLater && m_animationStopBehaviour == B2SAnimationStopBehaviour::RunAnimationTillEnd && rt.ticker == 0)
      || (rt.stopMeLater && m_animationStopBehaviour == B2SAnimationStopBehaviour::RunAnimationToFirstStep && (rt.ticker == 1 || rt.ticker == 2) && rt.reachedThe0Point))
      EndRun(fx);

   return waitLoops;
}

void B2SAnimation::BeginRun(const B2SAnimationEffects& fx, bool reverse)
{
   Runtime& rt = *m_runtime;
   rt.stopMeLater = false;
   rt.reverse = reverse;
   rt.ticker = 0;
   rt.loopTicker = 0;
   rt.reachedThe0Point = false;
   rt.timeUntilNextStep = static_cast<float>(m_interval > 0 ? m_interval : 1) / 1000.f;

   // snapshot all lights before touching them (restored at animation end when reset is requested)
   if (m_lightsStateAtAnimationEnd == B2SLightsStateAtAnimationEnd::LightsReseted)
      rt.lightSnapshot = fx.snapshotAllLights();
   else
      rt.lightSnapshot.clear();

   // maybe switch off all lights or switch on/off the involved lights
   if (m_lightsStateAtAnimationStart == B2SLightsStateAtAnimationStart::LightsOff)
      fx.allLightsOff();
   else if (m_lightsStateAtAnimationStart == B2SLightsStateAtAnimationStart::InvolvedLightsOff || m_lightsStateAtAnimationStart == B2SLightsStateAtAnimationStart::InvolvedLightsOn)
      for (const string& group : m_lightsInvolved)
         fx.setGroup(group, m_lightsStateAtAnimationStart == B2SLightsStateAtAnimationStart::InvolvedLightsOn);

   if (m_lockInvolvedLamps)
      for (const string& group : m_lightsInvolved)
         fx.lockGroup(group);
   if (m_hideScoreDisplays)
      fx.setScoreDisplaysHidden(true);

   rt.running = true;
}

void B2SAnimation::EndRun(const B2SAnimationEffects& fx)
{
   Runtime& rt = *m_runtime;
   if (!rt.running)
      return;
   rt.running = false;
   rt.stopMeLater = false;

   if (m_hideScoreDisplays)
      fx.setScoreDisplaysHidden(false);
   if (m_lockInvolvedLamps)
      for (const string& group : m_lightsInvolved)
         fx.unlockGroup(group);
   if (m_lightsStateAtAnimationEnd == B2SLightsStateAtAnimationEnd::LightsReseted)
   {
      fx.restoreAllLights(rt.lightSnapshot);
      rt.lightSnapshot.clear();
   }
   else if (m_lightsStateAtAnimationEnd == B2SLightsStateAtAnimationEnd::InvolvedLightsOff || m_lightsStateAtAnimationEnd == B2SLightsStateAtAnimationEnd::InvolvedLightsOn)
      for (const string& group : m_lightsInvolved)
         fx.setGroup(group, m_lightsStateAtAnimationEnd == B2SLightsStateAtAnimationEnd::InvolvedLightsOn);
}

string B2SCommandKey(const string& name)
{
   const size_t first = name.find_first_not_of(" \t\r\n");
   if (first == string::npos)
      return ""s;
   string key = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
   for (char& c : key)
      c = cLower(c);
   return key;
}

// B2S Pro: registers the named-command targets in file order, as the reference does while loading bulbs (fB:3467-3481):
// position grid, then external pivot, then named-command pivot. A name registered again belongs to the later group
void B2STable::RegisterNamedTargets(const tinyxml2::XMLNode& root)
{
   const tinyxml2::XMLElement* illumination = GetNode(root, "Illumination"s);
   if (illumination == nullptr)
      return;
   std::unordered_map<string, size_t> gridsByName;
   std::unordered_map<string, size_t> pivotsByName;
   size_t backglassIndex = 0, dmdIndex = 0;
   for (auto node = illumination->FirstChildElement("Bulb"); node != nullptr; node = node->NextSiblingElement("Bulb"))
   {
      auto& list = node->Attribute("Parent", "Backglass") != nullptr ? m_backglassIlluminations : m_dmdIlluminations;
      size_t& index = node->Attribute("Parent", "Backglass") != nullptr ? backglassIndex : dmdIndex;
      if (index >= list.size())
         continue;
      B2SBulb* const bulb = list[index++].get();
      const string name = B2SCommandKey(bulb->m_name);

      if (!bulb->m_externalGridName.empty() && !name.empty())
      {
         const auto [it, added] = gridsByName.try_emplace(B2SCommandKey(bulb->m_externalGridName), m_positionGrids.size());
         if (added)
            m_positionGrids.emplace_back();
         B2SPositionGrid& grid = m_positionGrids[it->second];
         grid.targets[name] = bulb;
         if (bulb->m_externalGridRepresentative)
         {
            grid.representative = bulb;
            bulb->m_gridIndex = static_cast<int>(it->second);
         }
         grid.duration = bulb->m_externalGridDuration;
         m_positionGridTargets[name] = it->second;
      }

      if (!bulb->m_externalPivotGroup.empty() && !name.empty())
      {
         const auto [it, added] = pivotsByName.try_emplace(B2SCommandKey(bulb->m_externalPivotGroup), m_namedPivots.size());
         if (added)
            m_namedPivots.emplace_back();
         B2SNamedPivot& pivot = m_namedPivots[it->second];
         pivot.angles[name] = bulb->m_externalPivotAngle;
         if (bulb->m_externalPivotRepresentative)
         {
            pivot.representative = bulb;
            bulb->m_pivotRotation = true;
            bulb->m_pivotAngle = bulb->m_externalPivotAngle;
         }
         pivot.duration = std::clamp(bulb->m_pivotDuration, 10, 1000);
         m_namedPivotTargets[name] = it->second;
      }

      // Named-command pivot: a private group "Pivot:<name>" whose down and up commands turn the bulb (BD:562-574)
      const string down = B2SCommandKey(bulb->m_pivotDownTrigger), up = B2SCommandKey(bulb->m_pivotUpTrigger);
      if (bulb->m_pivotAnimation && bulb->m_pivotTriggerType == 0 && !down.empty() && !up.empty())
      {
         const size_t pivotIndex = m_namedPivots.size();
         B2SNamedPivot& pivot = m_namedPivots.emplace_back();
         pivot.representative = bulb;
         bulb->m_pivotRotation = true;
         bulb->m_pivotAngle = bulb->m_pivotDownAngle;
         pivot.angles[down] = bulb->m_pivotDownAngle;
         pivot.angles[up] = bulb->m_pivotUpAngle;
         pivot.duration = std::clamp(bulb->m_pivotDuration, 10, 1000);
         pivotsByName[B2SCommandKey("Pivot:"s + bulb->m_name)] = pivotIndex;
         m_namedPivotTargets[down] = pivotIndex;
         m_namedPivotTargets[up] = pivotIndex;
      }
   }
}

// B2S Pro: optional backing color behind transparent canvas pixels, "r.g.b" with 0..255 components
// (reference fB:219-236); legacy files have none and keep the black backing
static std::optional<vec4> GetCanvasBacking(const tinyxml2::XMLNode& root) noexcept
{
   const string value = GetStringAttribute(root, "Images"s, "B2SProTransparentCanvasBacking"s, ""s);
   if (value.empty())
      return std::nullopt;
   const vector<string> parts = GetStringList(value, '.');
   if (parts.size() != 3)
      return std::nullopt;
   float rgb[3];
   for (size_t i = 0; i < 3; i++)
   {
      const size_t first = parts[i].find_first_not_of(" \t");
      const size_t last = parts[i].find_last_not_of(" \t");
      if (first == string::npos)
         return std::nullopt;
      int component;
      const char* begin = parts[i].c_str() + first;
      const char* end = parts[i].c_str() + last + 1;
      if (*begin == '+')
         begin++;
      if (const auto [ptr, ec] = std::from_chars(begin, end, component); ec != std::errc() || ptr != end || component < 0 || component > 255)
         return std::nullopt;
      rgb[i] = static_cast<float>(component) / 255.f;
   }
   return vec4(rgb[0], rgb[1], rgb[2], 1.f);
}

B2STable::B2STable(const tinyxml2::XMLNode& root) noexcept
   : m_version(GetStringAttribute(root, ""s, "Version"s, ""s))
   , m_name(GetStringAttribute(root, "Name"s, "Value"s, ""s))
   , m_tableType(GetIntAttribute(root, "TableType"s, "Value"s, 0))
   , m_dmdType(static_cast<B2SDMDType>(GetIntAttribute(root, "DMDType"s, "Value"s, 0)))
   , m_dmdDefaultLocationX(GetIntAttribute(root, "DMDDefaultLocation"s, "LocX"s, 0))
   , m_dmdDefaultLocationY(GetIntAttribute(root, "DMDDefaultLocation"s, "LocY"s, 0))
   , m_grillHeight(GetIntAttribute(root, "GrillHeight"s, "Value"s, 0))
   , m_grillSmallHeight(GetIntAttribute(root, "GrillHeight"s, "Small"s, 0))
   , m_lampsDefaultSkipFrames(GetIntAttribute(root, "LampsDefaultSkipFrames"s, "Value"s, 0))
   , m_solenoidsDefaultSkipFrames(GetIntAttribute(root, "SolenoidsDefaultSkipFrames"s, "Value"s, 0))
   , m_giStringsDefaultSkipFrames(GetIntAttribute(root, "GIStringsDefaultSkipFrames"s, "Value"s, 0))
   , m_ledsDefaultSkipFrames(GetIntAttribute(root, "LEDsDefaultSkipFrames"s, "Value"s, 0))
   , m_projectGUID(GetStringAttribute(root, "ProjectGUID"s, "Value"s, ""s))
   , m_projectGUID2(GetStringAttribute(root, "ProjectGUID2"s, "Value"s, ""s))
   , m_assemblyGUID(GetStringAttribute(root, "AssemblyGUID"s, "Value"s, ""s))
   , m_vsName(GetStringAttribute(root, "VSName"s, "Value"s, ""s))
   , m_dualBackglass(GetIntAttribute(root, "DualBackglass"s, "Value"s, false))
   , m_author(GetStringAttribute(root, "Author"s, "Value"s, ""s))
   , m_artwork(GetStringAttribute(root, "Artwork"s, "Value"s, ""s))
   , m_gameName(GetStringAttribute(root, "GameName"s, "Value"s, ""s))
   , m_thumbnailImage(GetImageAttribute(root, "Images/ThumbnailImage"s))
   , m_backglassImage(GetImageAttribute(root, "Images/BackglassImage"s))
   // the server only reads the on image inside its BackglassOffImage branch (fB:4050-4091): without an off image it is ignored
   , m_backglassOnImage(GetNode(root, "Images/BackglassOffImage"s) != nullptr ? GetImageAttribute(root, "Images/BackglassOnImage"s) : B2SImage())
   , m_backglassOffImage(GetImageAttribute(root, "Images/BackglassOffImage"s))
   , m_dmdImage(GetImageAttribute(root, "Images/DMDImage"s))
   , m_canvasBacking(GetCanvasBacking(root))
   , m_sounds(GetList<B2SSound>(root, "Sounds"s, "Sound"s))
   , m_reels(root)
   , m_backglassScores(root)
   , m_dmdScores(root)
   , m_backglassIlluminations(GetFilteredPtrList<B2SBulb>(root, "Illumination"s, "Bulb"s, false))
   , m_backglassAnimations(GetFilteredList<B2SAnimation>(root, "Animations"s, "Animation"s, false))
   , m_dmdIlluminations(GetFilteredPtrList<B2SBulb>(root, "Illumination"s, "Bulb"s, true))
   , m_dmdAnimations(GetFilteredList<B2SAnimation>(root, "Animations"s, "Animation"s, true))
{
   RegisterNamedTargets(root);

   // Optional plugin-authored <Displays> element (ignored by other B2S tools): its <DMD> children
   // declare display overlays. Without a Source, the overlay shows the default dot matrix display, or
   // the default CRT/LCD screen of games that have no dot matrix display (see DMDOverlay::Renderer::Render)
   if (const tinyxml2::XMLElement* displays = GetNode(root, "Displays"s))
   {
      for (const tinyxml2::XMLElement* dmd = displays->FirstChildElement("DMD"); dmd != nullptr; dmd = dmd->NextSiblingElement("DMD"))
      {
         B2SDMDDisplay& display = m_dmdDisplays.emplace_back();
         const char* const target = dmd->Attribute("Target");
         display.target = target != nullptr && string_to_lower(target) == "scoreview"s ? 1 : 0;
         if (const char* const source = dmd->Attribute("Source"); source != nullptr)
            display.source = source;
         display.x = dmd->IntAttribute("X", 0);
         display.y = dmd->IntAttribute("Y", 0);
         display.w = dmd->IntAttribute("W", 0);
         display.h = dmd->IntAttribute("H", 0);
         display.options.style = PinballPlugin::OverlayDisplayStyleIndex(dmd->Attribute("Style"));
         float f4[4];
         if (PinballPlugin::OverlayParseFloats(dmd->Attribute("Dots"), f4, 2))
         {
            display.options.sizeW = static_cast<int>(f4[0]);
            display.options.sizeH = static_cast<int>(f4[1]);
         }
         if (PinballPlugin::OverlayParseFloats(dmd->Attribute("Tint"), f4, 3))
         {
            display.options.tintR = f4[0];
            display.options.tintG = f4[1];
            display.options.tintB = f4[2];
         }
         if (PinballPlugin::OverlayParseFloats(dmd->Attribute("GlassTint"), f4, 3))
         {
            display.options.glassTintR = f4[0];
            display.options.glassTintG = f4[1];
            display.options.glassTintB = f4[2];
         }
         display.options.glassRoughness = dmd->FloatAttribute("GlassRoughness", 0.f);
         if (PinballPlugin::OverlayParseFloats(dmd->Attribute("GlassAmbient"), f4, 3))
         {
            display.options.glassAmbientR = f4[0];
            display.options.glassAmbientG = f4[1];
            display.options.glassAmbientB = f4[2];
         }
         if (PinballPlugin::OverlayParseFloats(dmd->Attribute("GlassArea"), f4, 4))
         {
            display.options.glassAreaX = static_cast<int>(f4[0]);
            display.options.glassAreaY = static_cast<int>(f4[1]);
            display.options.glassAreaW = static_cast<int>(f4[2]);
            display.options.glassAreaH = static_cast<int>(f4[3]);
         }
         if (PinballPlugin::OverlayParseFloats(dmd->Attribute("GlassPad"), f4, 4))
         {
            display.options.glassPadL = f4[0];
            display.options.glassPadT = f4[1];
            display.options.glassPadR = f4[2];
            display.options.glassPadB = f4[3];
         }
         if (const tinyxml2::XMLElement* glassImage = dmd->FirstChildElement("GlassImage"))
            if (const char* const value = glassImage->Attribute("Value"))
               display.glassImage = base64_decode(value, strlen(value));
      }
   }

   // Score digit numbering: displays without an explicit B2SStartDigit are numbered sequentially
   // over all Score nodes in file order (both backglass and DMD parents share the same counter)
   if (const tinyxml2::XMLElement* scoresNode = GetNode(root, "Scores"s); scoresNode != nullptr)
   {
      int autoDigit = 1;
      int fileOrder = 0;
      for (const tinyxml2::XMLElement* node = scoresNode->FirstChildElement("Score"); node != nullptr; node = node->NextSiblingElement("Score"))
      {
         const bool isDMD = node->Attribute("Parent", "Backglass") == nullptr;
         B2SScore score(*node);
         score.m_resolvedStartDigit = (score.m_b2sStartDigit > 0) ? score.m_b2sStartDigit : autoDigit;
         score.m_fileOrder = fileOrder++;
         autoDigit += score.m_digits;
         (isDMD ? m_dmdScores : m_backglassScores).m_scores.push_back(std::move(score));
      }
   }

   // Player layout (reference B2SPlayer, BP:21-33): the displays of a player in file order across both windows. A display on a start digit that the
   // player already has is skipped (offset -1). Each other display keeps the offset of its digits in the player's score text
   {
      vector<B2SScore*> byFileOrder;
      for (B2SScores* scores : { &m_backglassScores, &m_dmdScores })
         for (B2SScore& score : scores->m_scores)
            if (score.m_b2sPlayerNo > 0)
               byFileOrder.push_back(&score);
      std::stable_sort(byFileOrder.begin(), byFileOrder.end(), [](const B2SScore* a, const B2SScore* b) { return a->m_fileOrder < b->m_fileOrder; });
      std::map<int, std::set<int>> startDigits;
      for (B2SScore* score : byFileOrder)
      {
         if (!startDigits[score->m_b2sPlayerNo].insert(score->m_resolvedStartDigit).second)
            continue; // m_playerOffset stays -1
         int& digits = m_playerDigits[score->m_b2sPlayerNo];
         score->m_playerOffset = digits;
         digits += std::max(0, score->m_digits);
      }
   }

   // Draw order, as the server does (fB:686-716, BD:1205-1320, formDMD.vb:44-72). Without any ZOrder above 0, every bulb is drawn in the order of
   // its control name "PictureBox<ID>" (an ordinal string compare: 10 comes before 2). With some, the backglass draws the ZOrder 0 bulbs by name first and
   // then the ZOrder groups upwards (equal ZOrder in file order); the DMD draws the groups first and the ZOrder 0 bulbs by name after them.
   // Known differences: a negative ZOrder is drawn with the ZOrder 0 bulbs where the server does not draw it (the Designer never writes one), and a bulb
   // whose ID repeats is drawn where the server drops it
   const auto drawOrder = [](vector<std::unique_ptr<B2SBulb>>& bulbs, const bool zGroupsFirst)
   {
      const bool useZOrder = std::any_of(bulbs.begin(), bulbs.end(), [](const std::unique_ptr<B2SBulb>& b) { return b->m_zOrder > 0; });
      const auto byName = [](const std::unique_ptr<B2SBulb>& a, const std::unique_ptr<B2SBulb>& b) { return std::to_string(a->m_bulbId) < std::to_string(b->m_bulbId); };
      std::stable_sort(bulbs.begin(), bulbs.end(),
         [&](const std::unique_ptr<B2SBulb>& a, const std::unique_ptr<B2SBulb>& b)
         {
            if (!useZOrder)
               return byName(a, b);
            const bool aZ = a->m_zOrder > 0, bZ = b->m_zOrder > 0;
            if (aZ != bZ)
               return zGroupsFirst ? aZ : bZ;
            return aZ ? a->m_zOrder < b->m_zOrder : byName(a, b);
         });
   };
   drawOrder(m_backglassIlluminations, false);
   drawOrder(m_dmdIlluminations, true);

   // B2S Pro Dynamic Front Lighting (backglass only, as in the reference): a file with lights crops the images of its lights and of its image
   // snippets like the reference does at load, and keeps the pixels of the snippets for the kernel. Other files are untouched
   for (const auto& bulb : m_backglassIlluminations)
      m_hasDynamicFront = m_hasDynamicFront || bulb->m_dfl.enabled;
   if (m_hasDynamicFront)
      for (const auto& bulb : m_backglassIlluminations)
      {
         const bool snippet = bulb->m_isImageSnippit && bulb->m_snippitType == B2SSnippitType::StandardImage;
         if (!bulb->m_dfl.enabled && !snippet)
            continue;
         if (!bulb->m_cropComputed)
            bulb->ComputeCrop();
         if (snippet)
            bulb->ExtractPixels();
      }

   // Animations without a positive interval or without any playable step are dropped (reference behavior)
   const auto dropEmptyAnims = [](vector<B2SAnimation>& animations)
   {
      vector<B2SAnimation> kept;
      for (B2SAnimation& animation : animations)
         if (animation.m_interval > 0 && !animation.IsEmpty())
            kept.push_back(std::move(animation));
      animations.swap(kept);
   };
   dropEmptyAnims(m_backglassAnimations);
   dropEmptyAnims(m_dmdAnimations);

   // Bulbs on the BackglassOnImage ROM channel are baked into the lit background and not drawn
   // separately, unless an animation drives their group (SetThruAnimation in the reference)
   if (m_backglassOnImage.m_romId > 0 && m_backglassOnImage.m_romIdType != B2SRomIDType::NotDefined)
   {
      std::unordered_map<string, bool> animatedGroups;
      for (const B2SAnimation& animation : m_backglassAnimations)
         for (const string& group : animation.GetLightsInvolved())
            animatedGroups[group] = true;
      for (const auto& bulb : m_backglassIlluminations)
      {
         // RomInverted is never applied to the lit background channel (reference forces TopRomInverted off)
         bulb->m_bakedIntoBackground = bulb->m_romId != 0 && bulb->m_romId == m_backglassOnImage.m_romId && bulb->m_romIdType == m_backglassOnImage.m_romIdType && !bulb->m_romInverted
            && animatedGroups.find(bulb->m_name) == animatedGroups.end();
         // An initially lit bulb on the lit-background channel lights the glass at startup (reference ShowStartupImages)
         if (bulb->m_bakedIntoBackground && bulb->m_initialState && !bulb->m_isImageSnippit)
            m_backglassOnImage.m_brightness = 1.f;
      }
   }
}

const B2SScore* B2STable::FindScoreDisplay(int displayId) const
{
   for (const B2SScore& score : m_backglassScores.m_scores)
      if (score.m_id == displayId)
         return &score;
   for (const B2SScore& score : m_dmdScores.m_scores)
      if (score.m_id == displayId)
         return &score;
   return nullptr;
}

const B2SScore* B2STable::FindScoreDigitDisplay(int digit) const
{
   for (const B2SScore& score : m_backglassScores.m_scores)
      if (digit >= score.m_resolvedStartDigit && digit < score.m_resolvedStartDigit + score.m_digits)
         return &score;
   for (const B2SScore& score : m_dmdScores.m_scores)
      if (digit >= score.m_resolvedStartDigit && digit < score.m_resolvedStartDigit + score.m_digits)
         return &score;
   return nullptr;
}

uint16_t B2SSegmentCharMask(char c, int ledSegments)
{
   // Character to segment mask, following the Dream7 DisplayCharacter tables (reference SegmentNumber.vb)
   // expressed in the CTLPI/PinMAME luminance bit order used by the modern segment renderer:
   //   7seg : a=0,b=1,c=2,d=3,e=4,f=5,g=6, dp/comma=7
   //   10seg: a..f=0..5, g1/g2 merged=6, i=8, l=9
   //   14seg: a=0,b=1,c=2,d=3,e=4,f=5,g1=6, h=8,i=9,j=10,g2=11,k=12,l=13,m=14, dp=15
   switch (ledSegments)
   {
   case 14:
   {
      switch (c)
      {
      case '0': return 0x143F;
      case '1': return 0x0406;
      case '2': return 0x085B;
      case '3': return 0x080F;
      case '4': return 0x0866;
      case '5': return 0x086D;
      case '6': return 0x087D;
      case '7': return 0x0007;
      case '8': return 0x087F;
      case '9': return 0x086F;
      case 'A':
      case 'a': return 0x0877;
      case 'B':
      case 'b': return 0x280F;
      case 'C':
      case 'c': return 0x0039;
      case 'D':
      case 'd': return 0x220F;
      case 'E':
      case 'e': return 0x0079;
      case 'F':
      case 'f': return 0x0071;
      case 'G':
      case 'g': return 0x083D;
      case 'H':
      case 'h': return 0x0876;
      case 'I':
      case 'i': return 0x2209;
      case 'J':
      case 'j': return 0x001E;
      case 'K':
      case 'k': return 0x4470;
      case 'L':
      case 'l': return 0x0038;
      case 'M':
      case 'm': return 0x0536;
      case 'N':
      case 'n': return 0x4136;
      case 'O':
      case 'o': return 0x003F;
      case 'P':
      case 'p': return 0x0873;
      case 'Q':
      case 'q': return 0x403F;
      case 'R':
      case 'r': return 0x4873;
      case 'S':
      case 's': return 0x086D;
      case 'T':
      case 't': return 0x2201;
      case 'U':
      case 'u': return 0x003E;
      case 'V':
      case 'v': return 0x1430;
      case 'W':
      case 'w': return 0x5036;
      case 'X':
      case 'x': return 0x5500;
      case 'Y':
      case 'y': return 0x2500;
      case 'Z':
      case 'z': return 0x1409;
      case '-': return 0x00C0;
      case '_': return 0x0008;
      case '=': return 0x0048;
      default: return 0;
      }
   }
   case 10:
   {
      switch (c)
      {
      case '0': return 0x003F;
      case '1': return 0x0300;
      case '2': return 0x005B;
      case '3': return 0x004F;
      case '4': return 0x0066;
      case '5': return 0x006D;
      case '6': return 0x007D;
      case '7': return 0x0007;
      case '8': return 0x007F;
      case '9': return 0x006F;
      case 'A':
      case 'a': return 0x0077;
      case 'B':
      case 'b': return 0x007C;
      case 'C':
      case 'c': return 0x0039;
      case 'D':
      case 'd': return 0x005E;
      case 'E':
      case 'e': return 0x0079;
      case 'F':
      case 'f': return 0x0071;
      case '-': return 0x0040;
      default: return 0;
      }
   }
   default:
   {
      switch (c)
      {
      case '0': return 0x003F;
      case '1': return 0x0006;
      case '2': return 0x005B;
      case '3': return 0x004F;
      case '4': return 0x0066;
      case '5': return 0x006D;
      case '6': return 0x007D;
      case '7': return 0x0007;
      case '8': return 0x007F;
      case '9': return 0x006F;
      case 'A':
      case 'a': return 0x0077;
      case 'B':
      case 'b': return 0x007C;
      case 'C':
      case 'c': return 0x0039;
      case 'D':
      case 'd': return 0x005E;
      case 'E':
      case 'e': return 0x0079;
      case 'F':
      case 'f': return 0x0071;
      case '-': return 0x0040;
      case '_': return 0x0008;
      default: return 0;
      }
   }
   }
}

uint16_t B2SSegmentTranslateBitCode(uint32_t bits, int ledSegments)
{
   // Dream7 DisplayBitCode iterates the display's segment list: segment index n reads bit n.
   // Translate from that (Dream7 layout order) to the CTLPI/PinMAME luminance bit order.
   switch (ledSegments)
   {
   case 14:
      // Dream7 order: a,b,c,d,e,f,g1,dot,h,i,j,g2,m,l,k,dot2 -> CTLPI: a,b,c,d,e,f,g1,comma,h,i,j,g2,m,l,k,dp
      // (segment positions are identical; the dot lights both comma and dp)
      return static_cast<uint16_t>(bits | ((bits & 0x0080) << 8));
   case 10:
      // Dream7 order: a,b,c,d,e,f,g1,g2,i,l with g2 also reading bit 6 -> CTLPI 9seg: a..g=0..6, i=8, l=9
      return static_cast<uint16_t>(bits & 0x037F);
   case 7:
   default: return static_cast<uint16_t>(bits & 0x00FF);
   }
}

uint16_t B2SSegmentUntranslateBitCode(uint32_t bits, int ledSegments)
{
   // Inverse of B2SSegmentTranslateBitCode: the stored CTLPI/PinMAME luminance mask back to
   // the Dream7 DisplayBitCode segment order expected by the legacy segment renderers.
   switch (ledSegments)
   {
   case 14:
      // CTLPI: a,b,c,d,e,f,g1,comma,h,i,j,g2,m,l,k,dp -> Dream7: a,b,c,d,e,f,g1,dot,h,i,j,g2,m,l,k,dot2
      // (positions are identical; the comma lights the dot, the dp lights dot + dot2)
      return static_cast<uint16_t>((bits & 0x7FFF) | ((bits & 0x8000) ? 0x8080 : 0));
   case 10:
      // CTLPI 9seg: a..g=0..6, i=8, l=9 -> Dream7 order is identical
      return static_cast<uint16_t>(bits & 0x037F);
   case 16:
      // CTLPI 16seg (split top/bottom): a1,a2,b,c,d2,d1,e,f,h,i,j,g2,m,l,k,g1 -> Dream7 14seg: a,b,c,d,e,f,g1,dot,h,i,j,g2,m,l,k,dot2
      return static_cast<uint16_t>(
         ((bits & 0x0003) ? 0x0001 : 0) | ((bits & 0x000C) >> 1) | ((bits & 0x0030) ? 0x0008 : 0) | ((bits & 0x00C0) >> 2) | ((bits & 0x8000) >> 9) | (bits & 0x0F00) | (bits & 0x7000));
   case 7:
   default: return static_cast<uint16_t>(bits & 0x00FF);
   }
}
}

