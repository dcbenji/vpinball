// license:GPLv3+

#include "core/stdafx.h"

#include "AreaLayout.h"

#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>

namespace
{
std::atomic<uint32_t> g_nextNodeId { 1 };

uint32_t NewNodeId() { return g_nextNodeId.fetch_add(1, std::memory_order_relaxed); }

constexpr const char* kTypeNames[] = { "Outliner", "Properties", "Script", "PartLibrary", "Viewport", "CameraPreview" };
static_assert(std::size(kTypeNames) == static_cast<size_t>(AreaType::Count));

// Types a split may give to a new leaf, in order
// Panel types a split of a 3D view gives once the secondary views are at their cap (NewLeafType), in order
constexpr AreaType kNewLeafOrder[] = { AreaType::Outliner, AreaType::Properties, AreaType::Script, AreaType::PartLibrary };

AreaRect Empty(const AreaRect& r) { return { r.x0, r.y0, r.x0, r.y0 }; }

// Keys of a leaf's JSON object that belong to the layout (and to CameraPreview leaves): never read or written as a panel's state
constexpr const char* kLeafKeys[] = { "editor", "hidden", "preview_view", "view_camera", "view_shading", "live", "view_shade", "orbit", "split", "fixed", "size", "children" };
bool IsLeafKey(const std::string& key) { return std::any_of(std::begin(kLeafKeys), std::end(kLeafKeys), [&key](const char* k) { return key == k; }); }
bool HasPanelState(const AreaType type) { return type != AreaType::Viewport && type != AreaType::CameraPreview && type < AreaType::Count; }
}

const char* AreaTypeJsonName(const AreaType type)
{
   return static_cast<size_t>(type) < std::size(kTypeNames) ? kTypeNames[static_cast<size_t>(type)] : "";
}

bool AreaTypeFromJsonName(const std::string& name, AreaType& type)
{
   for (size_t i = 0; i < std::size(kTypeNames); ++i)
      if (name == kTypeNames[i])
      {
         type = static_cast<AreaType>(i);
         return true;
      }
   return false;
}

float SplitterRect::SizeForPos(const float p, const float dpi) const
{
   const float start = axis == SplitAxis::Vertical ? bounds.x0 : bounds.y0;
   const float end = axis == SplitAxis::Vertical ? bounds.x1 : bounds.y1;
   return (fixed == 1 ? end - p : p - start) / ((dpi > 0.f ? dpi : 1.f) * shrink);
}

// ---------------------------------------------------------------------------------------------------------------------
// Construction

AreaLayout::AreaLayout() : AreaLayout(nullptr) { *this = FromLegacy(true, true, 0.f, 0.f, 0, 1); }

AreaLayout AreaLayout::DefaultLayout()
{
   // The Outliner takes about 36% of a 1080p column (Blender's 189 px shows only 7 of our 23 px rows)
   static const nlohmann::json tree = nlohmann::json::parse(R"({ "split": "vertical", "fixed": "second", "size": 360, "children": [ { "editor": "Viewport" },
      { "split": "horizontal", "fixed": "first", "size": 360, "children": [ { "editor": "Outliner" }, { "editor": "Properties" } ] } ] })");
   AreaLayout layout;
   if (!layout.FromJson(tree))
      return AreaLayout(); // Cannot happen: the legacy shape
   return layout;
}

AreaLayout AreaLayout::FromLegacy(const bool outliner, const bool properties, float outlinerW, float propertiesW, const int leftEditor, const int rightEditor)
{
   if (!std::isfinite(outlinerW) || outlinerW <= 0.f)
      outlinerW = kDefaultOutlinerWidth;
   if (!std::isfinite(propertiesW) || propertiesW <= 0.f)
      propertiesW = kDefaultPropertiesWidth;
   const bool valid = leftEditor >= 0 && leftEditor < kPanelTypeCount && rightEditor >= 0 && rightEditor < kPanelTypeCount && leftEditor != rightEditor;

   AreaLayout l(nullptr);
   l.m_nodes.resize(5);
   AreaNode& root = l.m_nodes[0]; // V[first, outlinerW]
   root.isSplit = true;
   root.axis = SplitAxis::Vertical;
   root.child[0] = 1;
   root.child[1] = 2;
   root.fixed = 0;
   root.size = outlinerW;
   AreaNode& left = l.m_nodes[1];
   left.parent = 0;
   left.type = valid ? static_cast<AreaType>(leftEditor) : AreaType::Outliner;
   left.hidden = !outliner;
   AreaNode& inner = l.m_nodes[2]; // V[second, propertiesW]
   inner.parent = 0;
   inner.isSplit = true;
   inner.axis = SplitAxis::Vertical;
   inner.child[0] = 3;
   inner.child[1] = 4;
   inner.fixed = 1;
   inner.size = propertiesW;
   AreaNode& vp = l.m_nodes[3];
   vp.parent = 2;
   vp.type = AreaType::Viewport;
   AreaNode& right = l.m_nodes[4];
   right.parent = 2;
   right.type = valid ? static_cast<AreaType>(rightEditor) : AreaType::Properties;
   right.hidden = !properties;
   for (AreaNode& n : l.m_nodes)
      n.id = NewNodeId();
   l.m_root = 0;
   return l;
}

void AreaLayout::ToLegacy(bool& outliner, bool& properties, float& outlinerW, float& propertiesW, int& leftEditor, int& rightEditor) const
{
   AreaLayout ref = *this; // hidden leaves laid out as visible
   std::vector<LeafRect> leaves;
   std::vector<SplitterRect> splitters;
   ref.Layout(ref.m_root, { 0.f, 0.f, 1920.f, 1080.f }, 1.f, 0.f, 1080.f, true, leaves, &splitters);

   float vcx = 960.f;
   for (const LeafRect& lr : leaves)
      if (lr.type == AreaType::Viewport)
         vcx = (lr.rect.x0 + lr.rect.x1) * 0.5f;

   const LeafRect* left = nullptr;
   const LeafRect* right = nullptr;
   float leftCx = 0.f, rightCx = 0.f;
   for (const LeafRect& lr : leaves)
   {
      if (lr.type == AreaType::Viewport || lr.type == AreaType::CameraPreview) // Legacy keys only know the 4 panel types
         continue;
      const float cx = (lr.rect.x0 + lr.rect.x1) * 0.5f;
      if (cx < vcx && (!left || cx > leftCx))
      {
         left = &lr;
         leftCx = cx;
      }
      else if (cx > vcx && (!right || cx < rightCx))
      {
         right = &lr;
         rightCx = cx;
      }
   }

   auto hiddenOf = [this](const LeafRect* lr) {
      const AreaNode* n = Find(lr->id);
      return n && n->hidden;
   };
   outliner = left != nullptr && !hiddenOf(left);
   properties = right != nullptr && !hiddenOf(right);
   outlinerW = left ? left->rect.Width() : kDefaultOutlinerWidth;
   propertiesW = right ? right->rect.Width() : kDefaultPropertiesWidth;
   leftEditor = left ? static_cast<int>(left->type) : -1;
   rightEditor = right ? static_cast<int>(right->type) : -1;
   if (leftEditor >= 0 && rightEditor == leftEditor)
      rightEditor = -1; // Two areas of one editor: older builds want two different editors, the right side takes the first other one below
   if (leftEditor < 0)
   {
      for (const AreaType t : { AreaType::Outliner, AreaType::Properties, AreaType::Script, AreaType::PartLibrary })
         if (static_cast<int>(t) != rightEditor)
         {
            leftEditor = static_cast<int>(t);
            break;
         }
   }
   if (rightEditor < 0)
   {
      for (const AreaType t : { AreaType::Properties, AreaType::Outliner, AreaType::Script, AreaType::PartLibrary })
         if (static_cast<int>(t) != leftEditor)
         {
            rightEditor = static_cast<int>(t);
            break;
         }
   }
}

// ---------------------------------------------------------------------------------------------------------------------
// JSON

namespace
{
constexpr const char* kPreviewViewNames[3] = { "desktop", "cabinet", "fss" }; // Indexed by ViewSetupID

nlohmann::json ShadeOptionsToJson(const ViewShadeOptions& o)
{
   // Only what differs from the default: a layout stays small, and an option added later keeps its default in layouts saved before
   const ViewShadeOptions d;
   nlohmann::json j = nlohmann::json::object();
   if (o.brightness != d.brightness)
      j["brightness"] = o.brightness;
   if (o.lightAngle != d.lightAngle)
      j["light_angle"] = o.lightAngle;
   if (o.lightElevation != d.lightElevation)
      j["light_elevation"] = o.lightElevation;
   if (o.flat)
      j["flat"] = true;
   if (o.bgCustom)
      j["background"] = { o.bg[0], o.bg[1], o.bg[2] };
   if (o.backfaceCulling)
      j["backface_culling"] = true;
   if (o.cavity)
      j["cavity"] = true;
   if (o.outline)
      j["outline"] = true;
   if (o.shadow)
      j["shadow"] = true;
   if (o.xraySolid)
      j["xray_solid"] = true;
   if (o.xrayAlpha != d.xrayAlpha)
      j["xray_alpha"] = o.xrayAlpha;
   if (o.xray)
      j["xray"] = true;
   if (o.wireCustom)
      j["wire_color"] = { o.wire[0], o.wire[1], o.wire[2] };
   if (o.envStrength != d.envStrength)
      j["env_strength"] = o.envStrength;
   if (o.envAngle != d.envAngle)
      j["env_angle"] = o.envAngle;
   if (!o.tableLights)
      j["table_lights"] = false;
   const auto lampToJson = [&](const LampTestSettings& l, const char* const suffix) {
      if (l.mode != LampTestMode::Off)
         j[std::string("lamp_test") + suffix] = l.mode == LampTestMode::AllOn ? "all_on" : l.mode == LampTestMode::AllOff ? "all_off" : l.mode == LampTestMode::Blink ? "blink" : "collection";
      if (l.blinkHz != d.lampPbr.blinkHz)
         j[std::string("lamp_blink_hz") + suffix] = l.blinkHz;
      if (!l.collection.empty())
         j[std::string("lamp_collection") + suffix] = l.collection;
   };
   lampToJson(o.lampPbr, "");
   lampToJson(o.lampSolid, "_solid");
   if (o.hiddenWire != d.hiddenWire)
      j["hidden_wire"] = o.hiddenWire;
   if (o.hiddenSolid != d.hiddenSolid)
      j["hidden_solid"] = o.hiddenSolid;
   if (o.hiddenPbr != d.hiddenPbr)
      j["hidden_pbr"] = o.hiddenPbr;
   return j;
}

// Malformed or out of range values mean the default
ViewShadeOptions ShadeOptionsFromJson(const nlohmann::json& j)
{
   ViewShadeOptions o;
   if (!j.is_object())
      return o;
   const auto num = [&](const char* key, float& out, const float lo, const float hi) {
      const auto it = j.find(key);
      if (it != j.end() && it->is_number() && std::isfinite(it->get<double>()))
         out = std::clamp(static_cast<float>(it->get<double>()), lo, hi);
   };
   const auto flag = [&](const char* key, bool& out) {
      const auto it = j.find(key);
      if (it != j.end() && it->is_boolean())
         out = it->get<bool>();
   };
   const auto color = [&](const char* key, bool& custom, float (&rgb)[3]) {
      const auto it = j.find(key);
      if (it == j.end() || !it->is_array() || it->size() != 3)
         return;
      float c[3];
      for (int i = 0; i < 3; i++)
      {
         if (!(*it)[i].is_number() || !std::isfinite((*it)[i].get<double>()))
            return;
         c[i] = std::clamp(static_cast<float>((*it)[i].get<double>()), 0.f, 1.f);
      }
      custom = true;
      std::copy(c, c + 3, rgb);
   };
   num("brightness", o.brightness, 0.f, 4.f);
   num("light_angle", o.lightAngle, -180.f, 180.f);
   num("light_elevation", o.lightElevation, -90.f, 90.f);
   flag("flat", o.flat);
   color("background", o.bgCustom, o.bg);
   flag("backface_culling", o.backfaceCulling);
   flag("cavity", o.cavity);
   flag("outline", o.outline);
   flag("shadow", o.shadow);
   flag("xray_solid", o.xraySolid);
   num("xray_alpha", o.xrayAlpha, 0.f, 1.f);
   flag("xray", o.xray);
   color("wire_color", o.wireCustom, o.wire);
   num("env_strength", o.envStrength, 0.f, 4.f);
   num("env_angle", o.envAngle, -180.f, 180.f);
   flag("table_lights", o.tableLights);
   const auto lampFromJson = [&](LampTestSettings& l, const char* const suffix) {
      if (const auto it = j.find(std::string("lamp_test") + suffix); it != j.end() && it->is_string())
      {
         const std::string v = it->get<std::string>();
         l.mode = v == "all_on" ? LampTestMode::AllOn : v == "all_off" ? LampTestMode::AllOff : v == "blink" ? LampTestMode::Blink : v == "collection" ? LampTestMode::Collection : LampTestMode::Off;
      }
      if (const auto it = j.find(std::string("lamp_blink_hz") + suffix); it != j.end() && it->is_number() && std::isfinite(it->get<double>()))
         l.blinkHz = std::clamp(static_cast<float>(it->get<double>()), 0.25f, 8.f);
      if (const auto it = j.find(std::string("lamp_collection") + suffix); it != j.end() && it->is_string())
         l.collection = it->get<std::string>();
   };
   lampFromJson(o.lampPbr, "");
   lampFromJson(o.lampSolid, "_solid");
   flag("hidden_wire", o.hiddenWire);
   flag("hidden_solid", o.hiddenSolid);
   flag("hidden_pbr", o.hiddenPbr);
   return o;
}

nlohmann::json NodeToJson(const std::vector<AreaNode>& nodes, const int idx)
{
   const AreaNode& n = nodes[idx];
   nlohmann::json j = nlohmann::json::object();
   if (n.isSplit)
   {
      j["split"] = n.axis == SplitAxis::Vertical ? "vertical" : "horizontal";
      j["fixed"] = n.fixed == 0 ? "first" : "second";
      j["size"] = n.size;
      j["children"] = nlohmann::json::array({ NodeToJson(nodes, n.child[0]), NodeToJson(nodes, n.child[1]) });
   }
   else
   {
      j["editor"] = AreaTypeJsonName(n.type);
      if (n.hidden)
         j["hidden"] = true;
      if (HasPanelState(n.type) && n.state.is_object())
         for (const auto& [key, value] : n.state.items())
            if (!IsLeafKey(key))
               j[key] = value;
      if (n.type == AreaType::CameraPreview && n.previewView >= 0 && n.previewView < 3)
         j["preview_view"] = kPreviewViewNames[n.previewView];
      if (n.type == AreaType::CameraPreview)
      {
         const AreaViewSettings& v = n.view;
         if (v.freeOrbit)
            j["view_camera"] = "orbit";
         if (v.wireframe)
            j["view_shading"] = "wireframe";
         else if (v.solid)
            j["view_shading"] = "solid";
         if (!v.live)
            j["live"] = false;
         if (v.shade != ViewShadeOptions {})
            j["view_shade"] = ShadeOptionsToJson(v.shade);
         if (v.orbitValid)
            j["orbit"] = { { "yaw", v.yaw }, { "pitch", v.pitch }, { "distance", v.distance }, { "target", { v.target[0], v.target[1], v.target[2] } }, { "ortho", v.ortho } };
      }
   }
   return j;
}
}

nlohmann::json AreaLayout::ToJson() const { return m_root >= 0 ? NodeToJson(m_nodes, m_root) : nlohmann::json::object(); }

bool AreaLayout::FromJson(const nlohmann::json& j)
{
   AreaLayout tmp(nullptr);
   std::string why;
   int leafCount = 0;

   // Returns the node index or -1 (why set)
   auto parse = [&](auto&& self, const nlohmann::json& n, const int depth, const int parent) -> int {
      if (depth > kMaxDepth)
      {
         why = "layout deeper than " + std::to_string(kMaxDepth) + " levels";
         return -1;
      }
      if (!n.is_object())
      {
         why = "node is not an object";
         return -1;
      }
      const int idx = static_cast<int>(tmp.m_nodes.size());
      tmp.m_nodes.emplace_back();
      tmp.m_nodes[idx].id = NewNodeId();
      tmp.m_nodes[idx].parent = parent;
      if (n.contains("split"))
      {
         if (!n["split"].is_string() || !n.contains("fixed") || !n["fixed"].is_string() || !n.contains("size") || !n["size"].is_number()
            || !n.contains("children") || !n["children"].is_array() || n["children"].size() != 2)
         {
            why = "malformed split node";
            return -1;
         }
         const std::string axis = n["split"].get<std::string>();
         const std::string fixed = n["fixed"].get<std::string>();
         if ((axis != "vertical" && axis != "horizontal") || (fixed != "first" && fixed != "second"))
         {
            why = "unknown split axis or fixed child";
            return -1;
         }
         tmp.m_nodes[idx].isSplit = true;
         tmp.m_nodes[idx].axis = axis == "vertical" ? SplitAxis::Vertical : SplitAxis::Horizontal;
         tmp.m_nodes[idx].fixed = fixed == "first" ? 0 : 1;
         tmp.m_nodes[idx].size = static_cast<float>(n["size"].get<double>());
         for (int c = 0; c < 2; ++c)
         {
            const int ci = self(self, n["children"][c], depth + 1, idx);
            if (ci < 0)
               return -1;
            tmp.m_nodes[idx].child[c] = ci; // (not a reference: the vector grows while parsing)
         }
      }
      else
      {
         AreaType type;
         if (!n.contains("editor") || !n["editor"].is_string() || !AreaTypeFromJsonName(n["editor"].get<std::string>(), type))
         {
            why = "unknown or missing editor name";
            return -1;
         }
         if (n.contains("hidden") && !n["hidden"].is_boolean())
         {
            why = "hidden is not a boolean";
            return -1;
         }
         if (++leafCount > kMaxLeaves)
         {
            why = "more than " + std::to_string(kMaxLeaves) + " leaves";
            return -1;
         }
         tmp.m_nodes[idx].type = type;
         tmp.m_nodes[idx].hidden = n.value("hidden", false);
         if (HasPanelState(type)) // The other keys are the panel's state (its own validation: unknown or malformed values mean its defaults)
            for (const auto& [key, value] : n.items())
               if (!IsLeafKey(key))
                  tmp.m_nodes[idx].state[key] = value;
         tmp.m_nodes[idx].previewView = -1; // Unknown or missing names mean Auto
         if (type == AreaType::CameraPreview && n.contains("preview_view") && n["preview_view"].is_string())
            for (int v = 0; v < 3; v++)
               if (n["preview_view"].get<std::string>() == kPreviewViewNames[v])
                  tmp.m_nodes[idx].previewView = v;
         if (type == AreaType::CameraPreview)
         {
            // View settings: malformed or unknown values mean the default (never a reason to reject the layout)
            AreaViewSettings& v = tmp.m_nodes[idx].view;
            v.freeOrbit = n.contains("view_camera") && n["view_camera"].is_string() && n["view_camera"].get<std::string>() == "orbit";
            const std::string shading = n.contains("view_shading") && n["view_shading"].is_string() ? n["view_shading"].get<std::string>() : std::string();
            v.wireframe = shading == "wireframe";
            v.solid = shading == "solid";
            v.live = !(n.contains("live") && n["live"].is_boolean() && !n["live"].get<bool>());
            if (const auto so = n.find("view_shade"); so != n.end())
               v.shade = ShadeOptionsFromJson(*so);
            if (const auto o = n.find("orbit"); o != n.end() && o->is_object())
            {
               const auto num = [&](const char* key, float& out) {
                  const auto it = o->find(key);
                  if (it == o->end() || !it->is_number() || !std::isfinite(it->get<double>()))
                     return false;
                  out = static_cast<float>(it->get<double>());
                  return true;
               };
               const auto t = o->find("target");
               bool ok = num("yaw", v.yaw) && num("pitch", v.pitch) && num("distance", v.distance) && v.distance > 0.f && t != o->end() && t->is_array() && t->size() == 3;
               for (int c = 0; ok && c < 3; c++)
               {
                  ok = (*t)[c].is_number() && std::isfinite((*t)[c].get<double>());
                  if (ok)
                     v.target[c] = static_cast<float>((*t)[c].get<double>());
               }
               v.orbitValid = ok;
               v.ortho = ok && o->contains("ortho") && (*o)["ortho"].is_boolean() && (*o)["ortho"].get<bool>();
               if (!ok)
                  v = AreaViewSettings { v.freeOrbit, v.wireframe, v.solid, v.live, v.shade };
            }
         }
      }
      return idx;
   };

   const int root = parse(parse, j, 0, -1);
   if (root >= 0)
   {
      tmp.m_root = root;
      tmp.ValidateImpl(why);
   }
   else
      tmp.m_root = -1;

   if (root < 0 || !why.empty())
   {
      m_lastError = why.empty() ? "invalid layout" : why;
      PLOGW << "Editor layout rejected: " << m_lastError;
      return false;
   }
   *this = std::move(tmp);
   m_lastError.clear();
   return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Validation

bool AreaLayout::Validate() const
{
   std::string why;
   return ValidateImpl(why);
}

bool AreaLayout::Validate(std::string& why) const { return ValidateImpl(why); }

// Returns true if valid; otherwise false with `why` set (non empty). When valid `why` is left untouched (empty).
bool AreaLayout::ValidateImpl(std::string& why) const
{
   const int count = static_cast<int>(m_nodes.size());
   if (m_root < 0 || m_root >= count || m_nodes[m_root].parent != -1)
   {
      why = "no root";
      return false;
   }
   std::vector<uint8_t> seen(count, 0);
   int visited = 0, leaves = 0, viewports = 0, views = 0;

   auto walk = [&](auto&& self, const int idx, const int depth) -> bool {
      if (depth > kMaxDepth)
      {
         why = "layout deeper than " + std::to_string(kMaxDepth) + " levels";
         return false;
      }
      if (idx < 0 || idx >= count || seen[idx])
      {
         why = "broken node links";
         return false;
      }
      seen[idx] = 1;
      ++visited;
      const AreaNode& n = m_nodes[idx];
      if (n.isSplit)
      {
         if (n.fixed > 1 || !std::isfinite(n.size) || n.size <= 0.f)
         {
            why = "split size is not a finite positive number";
            return false;
         }
         for (int c = 0; c < 2; ++c)
         {
            if (n.child[c] < 0 || n.child[c] >= count || m_nodes[n.child[c]].parent != idx)
            {
               why = "broken node links";
               return false;
            }
            if (!self(self, n.child[c], depth + 1))
               return false;
         }
         return true;
      }
      if (static_cast<size_t>(n.type) >= static_cast<size_t>(AreaType::Count) || (n.type == AreaType::CameraPreview && !cameraPreviewEnabled))
      {
         why = "unknown or disabled editor type";
         return false;
      }
      if (++leaves > kMaxLeaves)
      {
         why = "more than " + std::to_string(kMaxLeaves) + " leaves";
         return false;
      }
      if (n.type == AreaType::Viewport)
      {
         ++viewports;
         if (n.hidden)
         {
            why = "the Viewport cannot be hidden";
            return false;
         }
      }
      // Any number of areas of a panel type (as in Blender); the secondary 3D views have a cap (render targets)
      if (n.type == AreaType::CameraPreview && ++views > kMaxViewLeaves)
      {
         why = "more than " + std::to_string(kMaxViewLeaves) + " CameraPreview editors";
         return false;
      }
      return true;
   };

   if (!walk(walk, m_root, 0))
      return false;
   if (viewports != 1)
   {
      why = "layout needs exactly one Viewport";
      return false;
   }
   if (visited != count)
   {
      why = "unreachable nodes";
      return false;
   }
   return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Queries

int AreaLayout::IndexOf(const int id) const
{
   if (id < 0)
      return -1;
   for (size_t i = 0; i < m_nodes.size(); ++i)
      if (m_nodes[i].id == static_cast<uint32_t>(id))
         return static_cast<int>(i);
   return -1;
}

const AreaNode* AreaLayout::Find(const int id) const
{
   const int i = IndexOf(id);
   return i >= 0 ? &m_nodes[i] : nullptr;
}

int AreaLayout::FindLeaf(const AreaType type) const
{
   for (const AreaNode& n : m_nodes)
      if (!n.isSplit && n.type == type)
         return static_cast<int>(n.id);
   return -1;
}

int AreaLayout::CountLeaves(const AreaType type) const
{
   int c = 0;
   for (const AreaNode& n : m_nodes)
      c += !n.isSplit && n.type == type ? 1 : 0;
   return c;
}

int AreaLayout::ParentId(const int id) const
{
   const int i = IndexOf(id);
   return i >= 0 && m_nodes[i].parent >= 0 ? static_cast<int>(m_nodes[m_nodes[i].parent].id) : -1;
}

int AreaLayout::LeafCount() const
{
   int c = 0;
   for (const AreaNode& n : m_nodes)
      c += n.isSplit ? 0 : 1;
   return c;
}

bool AreaLayout::operator==(const AreaLayout& other) const
{
   auto eq = [&](auto&& self, const int a, const int b) -> bool {
      if (a < 0 || b < 0)
         return a == b;
      const AreaNode& x = m_nodes[a];
      const AreaNode& y = other.m_nodes[b];
      if (x.isSplit != y.isSplit)
         return false;
      if (!x.isSplit)
         return x.type == y.type && x.hidden == y.hidden && (x.type != AreaType::CameraPreview || (x.previewView == y.previewView && x.view.SameChoices(y.view)))
            && (!HasPanelState(x.type) || x.state == y.state);
      return x.axis == y.axis && x.fixed == y.fixed && x.size == y.size && self(self, x.child[0], y.child[0]) && self(self, x.child[1], y.child[1]);
   };
   return eq(eq, m_root, other.m_root);
}

int AreaLayout::Depth(const int idx) const
{
   int d = 0;
   for (int p = m_nodes[idx].parent; p >= 0; p = m_nodes[p].parent)
      ++d;
   return d;
}

bool AreaLayout::Contains(const int idx, const AreaType type) const
{
   const AreaNode& n = m_nodes[idx];
   return n.isSplit ? (Contains(n.child[0], type) || Contains(n.child[1], type)) : n.type == type;
}

bool AreaLayout::Vis(const int idx, const bool ignoreHidden) const
{
   const AreaNode& n = m_nodes[idx];
   return n.isSplit ? (Vis(n.child[0], ignoreHidden) || Vis(n.child[1], ignoreHidden)) : (ignoreHidden || !n.hidden);
}

void AreaLayout::CollectLeaves(const int idx, std::vector<int>& out) const
{
   const AreaNode& n = m_nodes[idx];
   if (n.isSplit)
   {
      CollectLeaves(n.child[0], out);
      CollectLeaves(n.child[1], out);
   }
   else
      out.push_back(idx);
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout

void AreaLayout::MinSize(const int idx, const float dpi, const float sidebarPx, const bool ignoreHidden, float& w, float& h) const
{
   const AreaNode& n = m_nodes[idx];
   w = h = 0.f;
   if (!Vis(idx, ignoreHidden))
      return;
   if (!n.isSplit)
   {
      if (n.type == AreaType::Viewport)
      {
         w = (kViewportMinW + sidebarPx) * dpi;
         h = kViewportMinH * dpi;
      }
      else
      {
         w = kPanelMinW * dpi;
         h = kPanelMinH * dpi;
      }
      return;
   }
   float w0, h0, w1, h1;
   MinSize(n.child[0], dpi, sidebarPx, ignoreHidden, w0, h0);
   MinSize(n.child[1], dpi, sidebarPx, ignoreHidden, w1, h1);
   const bool v0 = Vis(n.child[0], ignoreHidden), v1 = Vis(n.child[1], ignoreHidden);
   if (v0 && v1)
   {
      if (n.axis == SplitAxis::Vertical)
      {
         w = w0 + w1;
         h = std::max(h0, h1);
      }
      else
      {
         w = std::max(w0, w1);
         h = h0 + h1;
      }
   }
   else
   {
      w = v0 ? w0 : w1;
      h = v0 ? h0 : h1;
   }
}

void AreaLayout::Layout(const int idx, const AreaRect r, const float dpi, const float sidebarPx, const float rootH, const bool ignoreHidden,
   std::vector<LeafRect>& leaves, std::vector<SplitterRect>* splitters) const
{
   const AreaNode& n = m_nodes[idx];
   if (!n.isSplit)
   {
      const bool vis = Vis(idx, ignoreHidden);
      leaves.push_back({ static_cast<int>(n.id), n.type, vis ? r : Empty(r), vis });
      return;
   }
   const bool v0 = Vis(n.child[0], ignoreHidden), v1 = Vis(n.child[1], ignoreHidden);
   if (!v0 || !v1)
   {
      // One child (or none) takes everything; the other takes no space and draws no splitter
      Layout(n.child[0], v0 ? r : Empty(r), dpi, sidebarPx, rootH, ignoreHidden, leaves, splitters);
      Layout(n.child[1], v1 ? r : Empty(r), dpi, sidebarPx, rootH, ignoreHidden, leaves, splitters);
      return;
   }

   const bool vertical = n.axis == SplitAxis::Vertical;
   const float a0 = vertical ? r.x0 : r.y0;
   const float a1 = vertical ? r.x1 : r.y1;
   const float total = std::max(0.f, a1 - a0);
   const int f = n.fixed, o = 1 - n.fixed;
   float w, h;
   MinSize(n.child[f], dpi, sidebarPx, ignoreHidden, w, h);
   const float minF = vertical ? w : h;
   MinSize(n.child[o], dpi, sidebarPx, ignoreHidden, w, h);
   const float minO = vertical ? w : h;

   const float shrink = vertical ? 1.f : StackedShrink(rootH, dpi);
   float fixedPx = n.size * dpi * shrink;
   const bool squeezed = total <= minF + minO;
   if (squeezed) // smaller than the minimums: shrink proportionally
      fixedPx = (minF + minO) > 0.f ? total * minF / (minF + minO) : total * 0.5f;
   else
      fixedPx = std::clamp(fixedPx, minF, total - minO);

   float pos = f == 1 ? a1 - fixedPx : a0 + fixedPx;
   pos = std::max(a0, std::min(std::round(pos), a1));

   AreaRect c0 = r, c1 = r;
   if (vertical)
   {
      c0.x1 = pos;
      c1.x0 = pos;
   }
   else
   {
      c0.y1 = pos;
      c1.y0 = pos;
   }

   if (splitters)
   {
      SplitterRect s;
      s.nodeId = static_cast<int>(n.id);
      s.axis = n.axis;
      s.fixed = n.fixed;
      s.bounds = r;
      s.pos = pos;
      s.shrink = shrink;
      const float half = kGrabWidth * dpi * 0.5f;
      s.grab = vertical ? AreaRect { pos - half, r.y0, pos + half, r.y1 } : AreaRect { r.x0, pos - half, r.x1, pos + half };
      if (squeezed)
         s.minPos = s.maxPos = pos;
      else
      {
         s.minPos = f == 1 ? a0 + minO : a0 + minF;
         s.maxPos = f == 1 ? a1 - minF : a1 - minO;
         if (s.minPos > s.maxPos)
            s.minPos = s.maxPos = pos;
      }
      splitters->push_back(s);
   }

   Layout(n.child[0], c0, dpi, sidebarPx, rootH, ignoreHidden, leaves, splitters);
   Layout(n.child[1], c1, dpi, sidebarPx, rootH, ignoreHidden, leaves, splitters);
}

void AreaLayout::Compute(const AreaRect bounds, const float dpi, const float sidebarPx, std::vector<LeafRect>& leaves,
   std::vector<SplitterRect>& splitters, const int maximizedLeafId) const
{
   leaves.clear();
   splitters.clear();
   if (m_root < 0)
      return;

   const int maxIdx = IndexOf(maximizedLeafId);
   if (maxIdx >= 0 && !m_nodes[maxIdx].isSplit && !m_nodes[maxIdx].hidden)
   {
      std::vector<int> all;
      CollectLeaves(m_root, all);
      for (const int i : all)
      {
         const AreaNode& n = m_nodes[i];
         const bool vis = i == maxIdx;
         leaves.push_back({ static_cast<int>(n.id), n.type, vis ? bounds : Empty(bounds), vis });
      }
      return;
   }
   Layout(m_root, bounds, dpi > 0.f ? dpi : 1.f, sidebarPx, std::max(0.f, bounds.y1 - bounds.y0), false, leaves, &splitters);
}

int AreaLayout::LeafAt(const std::vector<LeafRect>& leaves, const float x, const float y)
{
   for (const LeafRect& l : leaves)
      if (l.visible && x >= l.rect.x0 && x < l.rect.x1 && y >= l.rect.y0 && y < l.rect.y1)
         return l.id;
   return -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mutations

void AreaLayout::Compact()
{
   if (m_root < 0)
      return;
   std::vector<int> remap(m_nodes.size(), -1);
   std::vector<int> order; // preorder: parents before children
   auto walk = [&](auto&& self, const int idx) -> void {
      remap[idx] = static_cast<int>(order.size());
      order.push_back(idx);
      if (m_nodes[idx].isSplit)
      {
         self(self, m_nodes[idx].child[0]);
         self(self, m_nodes[idx].child[1]);
      }
   };
   walk(walk, m_root);
   std::vector<AreaNode> out;
   out.reserve(order.size());
   for (const int old : order)
   {
      AreaNode n = m_nodes[old];
      n.parent = n.parent >= 0 ? remap[n.parent] : -1;
      if (n.isSplit)
      {
         n.child[0] = remap[n.child[0]];
         n.child[1] = remap[n.child[1]];
      }
      out.push_back(n);
   }
   m_nodes = std::move(out);
   m_root = 0;
}

int AreaLayout::FirstFreeType() const
{
   for (const AreaType t : kNewLeafOrder)
   {
      if (FindLeaf(t) < 0)
         return static_cast<int>(t);
   }
   return -1;
}

AreaType AreaLayout::NewLeafType(const int leaf) const
{
   const AreaNode* const n = Find(leaf);
   if (n != nullptr && !n->isSplit && HasPanelState(n->type))
      return n->type;
   if (cameraPreviewEnabled && CountLeaves(AreaType::CameraPreview) < kMaxViewLeaves)
      return AreaType::CameraPreview;
   const int free = FirstFreeType();
   return free >= 0 ? static_cast<AreaType>(free) : AreaType::Outliner;
}

bool AreaLayout::CanSplit(const int leaf) const
{
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit)
      return false;
   return LeafCount() < kMaxLeaves && Depth(i) + 1 <= kMaxDepth;
}

bool AreaLayout::Split(const int leaf, const SplitAxis axis, const bool newOnSecondSide, float newSizePx, int* const newLeafId)
{
   if (newLeafId != nullptr)
      *newLeafId = -1;
   if (!CanSplit(leaf))
      return false;
   if (!std::isfinite(newSizePx) || newSizePx <= 0.f)
      newSizePx = 1.f;
   const int li = IndexOf(leaf);
   const int parent = m_nodes[li].parent;
   const AreaType newType = NewLeafType(leaf);
   // A copy of the split area's editor starts with its state (Blender's split duplicates the area)
   const bool copy = newType == m_nodes[li].type;
   const nlohmann::json newState = copy ? m_nodes[li].state : nlohmann::json::object();
   const int newPreviewView = copy ? m_nodes[li].previewView : -1;
   const AreaViewSettings newView = copy ? m_nodes[li].view : AreaViewSettings {};

   const int si = static_cast<int>(m_nodes.size());
   const int ni = si + 1;
   m_nodes.emplace_back();
   m_nodes.emplace_back();
   AreaNode& s = m_nodes[si];
   AreaNode& n = m_nodes[ni];
   s.id = NewNodeId();
   s.isSplit = true;
   s.axis = axis;
   s.parent = parent;
   s.child[newOnSecondSide ? 0 : 1] = li;
   s.child[newOnSecondSide ? 1 : 0] = ni;
   s.fixed = newOnSecondSide ? 1 : 0; // the new leaf keeps newSizePx
   s.size = newSizePx;
   n.id = NewNodeId();
   n.parent = si;
   n.type = newType;
   n.state = newState;
   n.previewView = newPreviewView;
   n.view = newView;
   if (newLeafId != nullptr)
      *newLeafId = static_cast<int>(n.id);
   m_nodes[li].parent = si;
   if (parent >= 0)
   {
      AreaNode& p = m_nodes[parent];
      p.child[p.child[0] == li ? 0 : 1] = si;
   }
   else
      m_root = si;
   Compact();
   return true;
}

bool AreaLayout::CanJoin(const int keepLeaf, const int consumeLeaf) const
{
   const int k = IndexOf(keepLeaf), c = IndexOf(consumeLeaf);
   if (k < 0 || c < 0 || k == c)
      return false;
   const AreaNode& a = m_nodes[k];
   const AreaNode& b = m_nodes[c];
   if (a.isSplit || b.isSplit || a.parent < 0 || a.parent != b.parent)
      return false;
   return !a.hidden && !b.hidden && b.type != AreaType::Viewport;
}

bool AreaLayout::Join(const int keep, const int consume)
{
   if (!CanJoin(keep, consume))
      return false;
   const int k = IndexOf(keep);
   const int p = m_nodes[k].parent;
   const int g = m_nodes[p].parent;
   m_nodes[k].parent = g;
   if (g >= 0)
   {
      AreaNode& gn = m_nodes[g];
      gn.child[gn.child[0] == p ? 0 : 1] = k;
   }
   else
      m_root = k;
   Compact(); // drops the old parent split and the consumed leaf
   return true;
}

bool AreaLayout::CanClose(const int leaf) const
{
   const int i = IndexOf(leaf);
   return i >= 0 && !m_nodes[i].isSplit && m_nodes[i].type != AreaType::Viewport && m_nodes[i].parent >= 0;
}

bool AreaLayout::Close(const int leaf)
{
   if (!CanClose(leaf))
      return false;
   const int i = IndexOf(leaf);
   const int p = m_nodes[i].parent;
   const int g = m_nodes[p].parent;
   const int sib = m_nodes[p].child[0] == i ? m_nodes[p].child[1] : m_nodes[p].child[0];
   m_nodes[sib].parent = g;
   if (g >= 0)
   {
      AreaNode& gn = m_nodes[g];
      gn.child[gn.child[0] == p ? 0 : 1] = sib;
   }
   else
      m_root = sib;
   Compact(); // drops the old parent split and the closed leaf
   return true;
}

bool AreaLayout::Swap(const int split)
{
   const int i = IndexOf(split);
   if (i < 0 || !m_nodes[i].isSplit)
      return false;
   AreaNode& n = m_nodes[i];
   std::swap(n.child[0], n.child[1]);
   n.fixed = static_cast<uint8_t>(1 - n.fixed);
   return true;
}

bool AreaLayout::SetType(const int leaf, const AreaType type, int* const swappedWith)
{
   if (swappedWith != nullptr)
      *swappedWith = -1;
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit || m_nodes[i].type == AreaType::Viewport)
      return false;
   if (type == AreaType::Viewport || type >= AreaType::Count || (type == AreaType::CameraPreview && !cameraPreviewEnabled))
      return false;
   AreaNode& n = m_nodes[i];
   if (n.type == type)
      return true;
   // Any number of areas of a panel type; up to kMaxViewLeaves secondary views: at the cap, a leaf becomes one by taking it from the first one
   const int o = type == AreaType::CameraPreview && CountLeaves(AreaType::CameraPreview) >= kMaxViewLeaves ? IndexOf(FindLeaf(type)) : -1;
   if (o >= 0)
   {
      m_nodes[o].type = n.type; // the other leaf takes the type this leaf leaves
      std::swap(m_nodes[o].previewView, n.previewView); // the preview's view setup and view settings follow the preview
      std::swap(m_nodes[o].view, n.view);
      std::swap(m_nodes[o].state, n.state); // and the panel's state follows the panel
      if (m_nodes[o].type != AreaType::CameraPreview)
      {
         m_nodes[o].previewView = -1;
         m_nodes[o].view = AreaViewSettings {};
      }
      if (swappedWith != nullptr)
         *swappedWith = static_cast<int>(m_nodes[o].id);
   }
   else if (type == AreaType::CameraPreview)
   {
      n.previewView = -1; // a new view: the default settings
      n.view = AreaViewSettings {};
   }
   if (o < 0)
      n.state = nlohmann::json::object(); // a new editor in the area: its defaults
   n.type = type;
   if (n.type != AreaType::CameraPreview)
   {
      n.previewView = -1;
      n.view = AreaViewSettings {};
   }
   return true;
}

bool AreaLayout::SetState(const int leaf, const nlohmann::json& state)
{
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit || !HasPanelState(m_nodes[i].type) || !state.is_object())
      return false;
   m_nodes[i].state = state;
   return true;
}

bool AreaLayout::SetPreviewView(const int leaf, const int view)
{
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit || view < -1 || view > 2)
      return false;
   m_nodes[i].previewView = view;
   return true;
}

bool AreaLayout::SetViewSettings(const int leaf, const AreaViewSettings& settings)
{
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit || m_nodes[i].type != AreaType::CameraPreview)
      return false;
   if (settings.orbitValid
      && (!std::isfinite(settings.yaw) || !std::isfinite(settings.pitch) || !std::isfinite(settings.distance) || settings.distance <= 0.f || !std::isfinite(settings.target[0])
         || !std::isfinite(settings.target[1]) || !std::isfinite(settings.target[2])))
      return false;
   m_nodes[i].view = settings;
   return true;
}

bool AreaLayout::SetHidden(const int leaf, const bool hidden)
{
   const int i = IndexOf(leaf);
   if (i < 0 || m_nodes[i].isSplit || m_nodes[i].type == AreaType::Viewport)
      return false;
   m_nodes[i].hidden = hidden;
   return true;
}

bool AreaLayout::SetSplitSize(const int split, const float logicalPx)
{
   const int i = IndexOf(split);
   if (i < 0 || !m_nodes[i].isSplit || !std::isfinite(logicalPx))
      return false;
   m_nodes[i].size = std::max(1.f, logicalPx);
   return true;
}
