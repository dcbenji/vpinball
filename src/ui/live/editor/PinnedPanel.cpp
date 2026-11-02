// license:GPLv3+

#include "core/stdafx.h"
#include "PinnedPanel.h"

#include "core/player.h"
#include "parts/Material.h"
#include "parts/primitive.h"
#include "physics/PhysicsEngine.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

#include "PropertyPane.h"

namespace VPX::EditorUI
{

// Field groups pinned by the see-through quick set (property pane separators)
static const string kRefractionGroup = "Refractions"s;
static const string kSeeThroughGroup = "See-Through Surface"s;
static const string kBulbGroup = "Traced Insert Bulb"s;

static bool IsTracedInsert(const IEditable *part)
{
   return part != nullptr && part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->IsTracedInsert();
}

static const char *PinKindName(PinKind kind)
{
   switch (kind)
   {
   case PinKind::Table: return "table";
   case PinKind::Material: return "material";
   default: return "part";
   }
}

static bool PinKindFromName(const string &name, PinKind &kind)
{
   if (name == "table")
      kind = PinKind::Table;
   else if (name == "material")
      kind = PinKind::Material;
   else if (name == "part")
      kind = PinKind::Part;
   else
      return false;
   return true;
}

static string ObjectId(PinKind kind, const string &name) { return string(PinKindName(kind)) + '\x1f' + name; }

string PinKey::Id() const { return ObjectId(kind, name) + '\x1f' + prop; }

PinnedPanel::PinnedPanel(EditorUI &editor)
   : m_editor(editor)
{
}

PinnedPanel::~PinnedPanel() = default;

bool PinnedPanel::IsPinned(const PinKey &key) const { return std::ranges::find(m_pins, key) != m_pins.end(); }

void PinnedPanel::Pin(const PinKey &key)
{
   if (!IsPinned(key))
      m_pins.push_back(key);
   m_visible = true;
   m_collapsed = false;
}

void PinnedPanel::Unpin(const PinKey &key) { std::erase(m_pins, key); }

bool PinnedPanel::NeedsDef(const string &id, const void *owner) const
{
   const auto it = m_defs.find(id);
   return it == m_defs.end() || it->second.owner != owner;
}

void PinnedPanel::SetDef(const PinKey &key, const string &group, const void *owner, PinDrawFn &&draw, bool common)
{
   Def &def = m_defs[key.Id()];
   def.key = key;
   def.group = group;
   def.owner = owner;
   def.draw = std::move(draw);
   def.common = common;
   // Only a harvest records which fields an object shows: a panel registration (e.g. from the startup copy in an inspection
   // session, whose owner differs) keeps the def in the object's curated sections
   if (m_harvestStamp != 0)
   {
      def.stamp = m_harvestStamp;
      def.seq = m_harvestSeq++;
   }
}

unsigned PinnedPanel::EnsureHarvested(PinKind kind, const string &name, void **resolved)
{
   const void *owner = nullptr;
   void *const obj = Resolve(kind, name, &owner);
   if (resolved != nullptr)
      *resolved = obj;
   if (obj == nullptr || owner == nullptr)
      return 0;
   const string objId = ObjectId(kind, name);
   const auto harvested = m_harvested.find(objId);
   if (harvested == m_harvested.end() || harvested->second != owner)
      Harvest(kind, name);
   const auto stamp = m_harvestStamps.find(objId);
   return stamp == m_harvestStamps.end() ? 0 : stamp->second;
}

void PinnedPanel::InvalidateHarvest(PinKind kind, const string &name) { m_harvested.erase(ObjectId(kind, name)); }

void PinnedPanel::CollectDefs(PinKind kind, const string &name, unsigned stamp, vector<const Def *> &out) const
{
   out.clear();
   if (stamp == 0)
      return;
   const string prefix = ObjectId(kind, name) + '\x1f';
   for (auto it = m_defs.lower_bound(prefix); it != m_defs.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
      if (it->second.stamp == stamp && it->second.draw)
         out.push_back(&it->second);
   std::ranges::sort(out, [](const Def *a, const Def *b) { return a->seq < b->seq; });
}

bool PinnedPanel::HasBulbSet(const IEditable *part) const { return IsTracedInsert(part); }

bool PinnedPanel::HasSeeThroughSet(const IEditable *part) const
{
   if (part == nullptr || part->GetItemType() != eItemPrimitive)
      return false;
   if (IsTracedInsert(part)) // The quick set of a traced insert also holds its bulb fields
      return true;
   const Primitive *const prim = static_cast<const Primitive *>(part);
   if (!prim->m_d.m_szRefractionProbe.empty())
      return true;
   const Material *const mat = prim->m_d.m_szMaterial.empty() ? nullptr : m_editor.m_table->GetMaterial(prim->m_d.m_szMaterial);
   return mat != nullptr && !m_editor.m_table->IsDummyMaterial(mat) && mat->m_type == Material::PBR;
}

void PinnedPanel::PinSeeThroughSet(const IEditable *part)
{
   if (!HasSeeThroughSet(part))
      return;
   const Primitive *const prim = static_cast<const Primitive *>(part);
   m_groupRequests.push_back({ PinKind::Part, part->GetName(), kRefractionGroup });
   if (prim->IsTracedInsert())
      m_groupRequests.push_back({ PinKind::Part, part->GetName(), kBulbGroup });
   if (!prim->m_d.m_szMaterial.empty())
      PinMaterialSeeThrough(prim->m_d.m_szMaterial);
   m_visible = true;
   m_collapsed = false;
}

void PinnedPanel::PinBulbSet(const IEditable *part)
{
   if (!HasBulbSet(part))
      return;
   m_groupRequests.push_back({ PinKind::Part, part->GetName(), kBulbGroup });
   m_visible = true;
   m_collapsed = false;
}

void PinnedPanel::PinMaterialSeeThrough(const string &materialName)
{
   const Material *const mat = m_editor.m_table->GetMaterial(materialName);
   if (mat == nullptr || m_editor.m_table->IsDummyMaterial(mat) || mat->m_type != Material::PBR)
      return;
   m_groupRequests.push_back({ PinKind::Material, materialName, kSeeThroughGroup });
   m_visible = true;
   m_collapsed = false;
}

void PinnedPanel::SaveTo(PinTable *table) const
{
   if (table == nullptr)
      return;
   table->m_editorPins.clear();
   for (const PinKey &pin : m_pins)
      table->m_editorPins.push_back({ PinKindName(pin.kind), pin.name, pin.prop });
}

void PinnedPanel::SyncWithTable()
{
   // Pins belong to the base (startup) table: they are kept across the switch to its live copy in an inspection session
   const PinTable *const base = m_editor.m_table->m_liveBaseTable ? m_editor.m_table->m_liveBaseTable : m_editor.m_table;
   if (base == m_sourceTable)
      return;
   m_sourceTable = base;
   m_pins.clear();
   m_defs.clear();
   m_harvested.clear();
   m_groupRequests.clear();
   for (const PinTable::EditorPin &pin : base->m_editorPins)
   {
      PinKey key;
      if (!PinKindFromName(pin.kind, key.kind) || pin.prop.empty())
         continue;
      key.name = pin.name;
      key.prop = pin.prop;
      if (!IsPinned(key))
         m_pins.push_back(key);
   }
   if (!m_pins.empty())
      m_visible = true;
}

// Resolves a pinned object by name on the displayed table (the live copy in an inspection session, like the panel's 'Live' tab)
void *PinnedPanel::Resolve(PinKind kind, const string &name, const void **owner) const
{
   *owner = nullptr;
   PinTable *const table = m_editor.m_table;
   switch (kind)
   {
   case PinKind::Table: *owner = table; return static_cast<void *>(table);
   case PinKind::Material:
      for (Material *const mat : table->m_materials)
         if (mat->m_name == name)
         {
            *owner = mat;
            return static_cast<void *>(mat);
         }
      return nullptr;
   case PinKind::Part:
      for (IEditable *const part : table->GetParts())
         if (part->GetName() == name)
         {
            const auto it = m_editor.m_editableMap.find(part);
            *owner = it != m_editor.m_editableMap.end() ? it->second.get() : nullptr;
            return static_cast<void *>(part);
         }
      return nullptr;
   }
   return nullptr;
}

// Registers the field definitions of an object by drawing its property pane in a hidden window
bool PinnedPanel::Harvest(PinKind kind, const string &name)
{
   const void *owner = nullptr;
   void *const obj = Resolve(kind, name, &owner);
   m_harvested[ObjectId(kind, name)] = owner;
   if (obj == nullptr || owner == nullptr)
      return false;
   m_harvestStamp = ++m_harvestCounter;
   m_harvestSeq = 0;
   m_harvestStamps[ObjectId(kind, name)] = m_harvestStamp;
   PropertyPane props(m_editor.m_table);
   ApplyUnits(props);
   props.SetPinning(this, PropertyPane::PinMode::Harvest, kind == PinKind::Part ? owner : nullptr);
   ImGui::SetNextWindowPos(ImVec2(-100000.f, -100000.f));
   ImGui::SetNextWindowSize(ImVec2(PropertiesPanel::PaneWidth * m_editor.m_liveUI.GetDPI(), 400.f));
   ImGui::Begin("##PinHarvest", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
         | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
   switch (kind)
   {
   case PinKind::Table: m_editor.m_properties.TableProperties(props); break;
   case PinKind::Material: m_editor.m_properties.MaterialProperties(props, static_cast<Material *>(obj)); break;
   case PinKind::Part: static_cast<EditorUIPart *>(const_cast<void *>(owner))->UpdatePropertyPane(props); break;
   }
   ImGui::End();
   m_harvestStamp = 0;
   return true;
}

void PinnedPanel::ProcessGroupRequests()
{
   if (m_groupRequests.empty())
      return;
   const vector<GroupRequest> requests = std::move(m_groupRequests);
   m_groupRequests.clear();
   int found = 0;
   for (const GroupRequest &req : requests)
   {
      // Always refresh the object's definitions: the group may have fields never drawn yet (other material type, collapsed section,...)
      Harvest(req.kind, req.name);
      for (const auto &[id, def] : m_defs)
         if (def.key.kind == req.kind && def.key.name == req.name && def.group == req.group)
         {
            if (!IsPinned(def.key))
               m_pins.push_back(def.key);
            found++;
         }
   }
   if (found == 0)
      m_editor.m_liveUI.PushNotification("No fields found to pin"s, 4000);
}

void PinnedPanel::ApplyUnits(PropertyPane &props) const
{
   switch (m_editor.m_units)
   {
   case EditorUI::Units::VPX: props.SetLengthUnit(PropertyPane::Unit::VPLength); break;
   case EditorUI::Units::Metric: props.SetLengthUnit(PropertyPane::Unit::Millimeters); break;
   case EditorUI::Units::Imperial: props.SetLengthUnit(PropertyPane::Unit::Inches); break;
   }
}

void PinnedPanel::PinSelectionSeeThrough()
{
   const Selection &sel = m_editor.m_selection;
   if (sel.GetType() == Selection::S_EDITABLE)
      PinSeeThroughSet(sel.GetPart()->GetEditable());
   else if (sel.GetType() == Selection::S_MATERIAL && sel.GetMaterial() != nullptr)
      PinMaterialSeeThrough(sel.GetMaterial()->m_name);
}

void PinnedPanel::RenderHeader()
{
   const ImGuiStyle &style = ImGui::GetStyle();
   if (ImGui::SmallButton(m_collapsed ? ICON_FK_CARET_RIGHT : ICON_FK_CARET_DOWN))
      m_collapsed = !m_collapsed;
   ImGui::SameLine();
   ImGui::TextUnformatted(ICON_FK_THUMB_TACK " Pinned");

   const auto buttonWidth = [&style](const char *label) { return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.f; };
   const float buttonsWidth = buttonWidth(ICON_FK_TINT) + buttonWidth(ICON_FK_TRASH) + buttonWidth(ICON_FK_TIMES) + style.ItemSpacing.x * 2.f;
   ImGui::SameLine(max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttonsWidth));

   const Selection &sel = m_editor.m_selection;
   const bool canPinSel = (sel.GetType() == Selection::S_EDITABLE && HasSeeThroughSet(sel.GetPart()->GetEditable()))
      || (sel.GetType() == Selection::S_MATERIAL && sel.GetMaterial() != nullptr && sel.GetMaterial()->m_type == Material::PBR);
   ImGui::BeginDisabled(!canPinSel);
   if (ImGui::SmallButton(ICON_FK_TINT))
      PinSelectionSeeThrough();
   ImGui::EndDisabled();
   ImGui::SetItemTooltip("Pin see-through set for selection (refraction fields of the selected part, its Traced Insert Bulb fields if it is a traced insert, and the See-Through Surface fields of its material)");
   ImGui::SameLine();
   ImGui::BeginDisabled(m_pins.empty());
   if (ImGui::SmallButton(ICON_FK_TRASH))
      ImGui::OpenPopup("##PinClearAll");
   ImGui::EndDisabled();
   ImGui::SetItemTooltip("Unpin all");
   ImGui::SameLine();
   if (ImGui::SmallButton(ICON_FK_TIMES))
      m_visible = false;
   ImGui::SetItemTooltip("Hide the overlay (View > Pinned Properties, or P)");
   if (ImGui::BeginPopup("##PinClearAll"))
   {
      ImGui::Text("Unpin all %d properties?", static_cast<int>(m_pins.size()));
      if (ImGui::Button("Unpin all"))
      {
         ClearPins();
         ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel"))
         ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
   }
}

void PinnedPanel::Render(float topBarHeight)
{
   EditorUI &editor = m_editor;
   if (editor.m_table->IsLocked())
      return;
   SyncWithTable();
   if (!m_visible)
      return;

   ProcessGroupRequests();

   // Objects of the pinned fields, in order of first appearance
   vector<std::pair<PinKind, string>> objects;
   for (const PinKey &pin : m_pins)
      if (std::ranges::find(objects, std::make_pair(pin.kind, pin.name)) == objects.end())
         objects.emplace_back(pin.kind, pin.name);

   // Load the missing or stale definitions (part re-init, live copy of an inspection session), one object per frame
   for (const auto &[kind, name] : objects)
   {
      const void *owner = nullptr;
      if (Resolve(kind, name, &owner) == nullptr || owner == nullptr)
         continue;
      const bool stale = std::ranges::any_of(m_pins,
         [&, kind = kind, &name = name](const PinKey &pin)
         {
            if (pin.kind != kind || pin.name != name)
               return false;
            const auto it = m_defs.find(pin.Id());
            return it == m_defs.end() || (kind == PinKind::Part && it->second.owner != owner);
         });
      if (!stale)
         continue;
      const auto harvested = m_harvested.find(ObjectId(kind, name));
      if (harvested != m_harvested.end() && harvested->second == owner)
         continue; // Already harvested from this owner: the remaining fields are not available (e.g. hidden for this material type)
      Harvest(kind, name);
      break;
   }

   const float dpi = editor.m_liveUI.GetDPI();
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   const float width = 300.f * dpi;
   ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - (PropertiesPanel::PaneWidth + 8.f) * dpi, viewport->Pos.y + topBarHeight + (8.f + 128.f + 16.f) * dpi), ImGuiCond_FirstUseEver,
      ImVec2(1.f, 0.f));
   ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, viewport->Size.y * 0.8f));
   ImGui::SetNextWindowSize(ImVec2(width, 0.f)); // Fixed width, height fitted to the rows
   ImGui::SetNextWindowBgAlpha(m_hovered ? 0.92f : 0.6f);
   ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_hovered ? 1.f : 0.85f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f * dpi, 4.f * dpi));
   ImGui::Begin("Pinned###PinnedOverlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
   m_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_AllowWhenBlockedByPopup)
      || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsAnyItemActive());

   RenderHeader();

   if (!m_collapsed)
   {
      if (m_pins.empty())
         ImGui::TextDisabled("Right click a property in the Properties\npanel and pick 'Pin to overlay'");

      PropertyPane props(editor.m_table);
      ApplyUnits(props);
      props.SetPinning(this, PropertyPane::PinMode::Overlay);
      const bool inspect = editor.IsInspectMode();
      const vector<PinKey> pins = m_pins; // Rows may unpin while drawn
      for (const auto &[kind, name] : objects)
      {
         const void *owner = nullptr;
         void *const obj = Resolve(kind, name, &owner);
         ImGui::PushID(ObjectId(kind, name).c_str());
         const string header = (kind == PinKind::Table ? "Table"s : name) + (kind == PinKind::Material ? " (material)"s : string()) + "###obj";
         if (!props.BeginSection(header))
         {
            ImGui::PopID();
            continue;
         }
         IEditable *const part = (kind == PinKind::Part && obj != nullptr) ? static_cast<IEditable *>(obj) : nullptr;
         // "Apply to all traced inserts": the pinned bulb sliders of a traced insert also write to the other traced inserts
         props.ArmBulbBroadcast(part);
         // Undo of part edits outside of an inspection session, recorded like the properties panel (only while interacting, as it snapshots the part)
         const bool withUndo = part != nullptr && !inspect && m_hovered;
         if (withUndo)
         {
            editor.m_undo.BeginUndo();
            editor.m_undo.MarkForUndo(part);
            for (IEditable *const other : props.GetBroadcastParts())
               editor.m_undo.MarkForUndo(other);
         }
         props.SetPinOwner(owner);
         const int modifiedBefore = props.GetModifiedField();
         for (const PinKey &pin : pins)
         {
            if (pin.kind != kind || pin.name != name)
               continue;
            props.FlushRow(); // Below the previous row when its label wrapped
            ImGui::PushID(pin.prop.c_str());
            if (ImGui::SmallButton(ICON_FK_TIMES))
               Unpin(pin);
            ImGui::SetItemTooltip("Unpin");
            ImGui::SameLine();
            const auto it = m_defs.find(pin.Id());
            if (obj == nullptr)
               ImGui::TextDisabled("%s (object not found)", pin.prop.c_str());
            else if (it == m_defs.end() || !it->second.draw || (kind == PinKind::Part && it->second.owner != owner))
               ImGui::TextDisabled("%s (not available)", pin.prop.c_str());
            else
               it->second.draw(props, obj);
            ImGui::PopID();
         }
         props.EndSection();
         const int modified = props.GetModifiedField() != modifiedBefore ? props.GetModifiedField() : 0;
         if (withUndo)
         {
            editor.m_undo.EndUndo();
            if (modified > 0 && (editor.m_lastUndoPart != part || editor.m_lastUndoId != static_cast<unsigned int>(0x4000 | modified)))
            {
               editor.m_lastUndoPart = part;
               editor.m_lastUndoId = 0x4000 | modified;
            }
            else
               editor.m_undo.Discard();
         }
         if (modified > 0)
         {
            if (part != nullptr)
            {
               editor.m_renderer->ReinitRenderable(part->GetIRenderable());
               editor.m_player->m_physics->Update(part);
            }
            if (props.WasBroadcast())
               for (IEditable *const other : props.GetBroadcastParts())
                  editor.m_renderer->ReinitRenderable(other->GetIRenderable());
            if (inspect)
            {
               // Same tracking as the panel's 'Live' tab: 'Save' copies these edits back to the startup table
               if (kind == PinKind::Material)
                  editor.m_liveEditedMaterials.insert(static_cast<Material *>(obj));
               else if (part != nullptr)
                  editor.m_liveEditedParts.insert(part);
               if (props.WasBroadcast())
                  for (IEditable *const other : props.GetBroadcastParts())
                     editor.m_liveEditedParts.insert(other);
            }
            else if (part == nullptr)
               editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         }
         ImGui::PopID();
      }
   }

   ImGui::End();
   ImGui::PopStyleVar(2);
}

}
