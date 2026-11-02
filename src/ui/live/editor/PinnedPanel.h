// license:GPLv3+

#pragma once

#include <functional>
#include <map>

class IEditable;
class PinTable;

namespace VPX::EditorUI
{

class EditorUI;
class PropertyPane;

// Object a pinned property belongs to
enum class PinKind
{
   Table,
   Material,
   Part
};

// A pinned property: the object it belongs to (by kind and name, resolved on the displayed table each frame) and its
// property pane label. Names are used instead of pointers so that pins survive selection changes, part re-inits and
// the switch between the edited table and its live copy in an inspection session.
struct PinKey
{
   PinKind kind = PinKind::Part;
   string name; // Empty for the table
   string prop; // Property pane label of the field
   string Id() const;
   bool operator==(const PinKey &other) const { return kind == other.kind && name == other.name && prop == other.prop; }
};

// Draws a pinned field with a property pane on the resolved object (Material*, PinTable* or IEditable*)
using PinDrawFn = std::function<void(PropertyPane &, void *)>;

// "Pinned" floating overlay: properties pinned from the properties panel (right click on a field), drawn with the
// same property pane widgets and getter/setter definitions as the panel. The definitions are registered by the
// property pane when it draws a field (and cached by key); for objects never shown in the panel this session (pins
// loaded with the table, quick sets), they are harvested by running the object's pane in a hidden window.
class PinnedPanel
{
public:
   explicit PinnedPanel(EditorUI &editor);
   ~PinnedPanel();

   void Render(float topBarHeight);

   bool IsVisible() const { return m_visible; }
   void SetVisible(bool visible) { m_visible = visible; }
   void ToggleVisible() { m_visible = !m_visible; }

   bool IsPinned(const PinKey &key) const;
   void Pin(const PinKey &key);
   void Unpin(const PinKey &key);
   void ClearPins() { m_pins.clear(); }
   bool HasPins() const { return !m_pins.empty(); }

   // Field definitions, registered by the property panes (owner identifies the definition's provider, see PropertyPane::SetPinning)
   struct Def
   {
      PinKey key;
      string group; // Section or separator under which the field is drawn in the panel
      const void *owner = nullptr; // Part: its EditorUIPart (definitions may capture it), otherwise the object itself
      PinDrawFn draw;
      bool common = false; // Registered while the pane's tier was Common (PropertyPane::SetTier)
      int seq = 0; // Draw order in the harvest that registered it
      unsigned stamp = 0; // Harvest that registered it (0: registered by the panel outside of a harvest)
   };
   bool NeedsDef(const string &id, const void *owner) const;
   void SetDef(const PinKey &key, const string &group, const void *owner, PinDrawFn &&draw, bool common);
   bool IsHarvesting() const { return m_harvestStamp != 0; }

   // Curated (Look / Transform) sections of the properties panel: the fields of an object, as registered by its latest harvest
   // Harvests the object if it was never harvested from its current owner; returns the stamp of its latest harvest (0 if unresolved)
   unsigned EnsureHarvested(PinKind kind, const string &name, void **resolved = nullptr);
   void InvalidateHarvest(PinKind kind, const string &name);
   // Definitions registered by the given harvest of an object, in draw order
   void CollectDefs(PinKind kind, const string &name, unsigned stamp, vector<const Def *> &out) const;
   // Every known definition (search palette index)
   const std::map<string, Def> &GetDefs() const { return m_defs; }
   const Def *FindDef(const PinKey &key) const
   {
      const auto it = m_defs.find(key.Id());
      return it == m_defs.end() ? nullptr : &it->second;
   }

   // "See-through set": a primitive's refraction fields and its material's See-Through Surface fields
   bool HasSeeThroughSet(const IEditable *part) const;
   void PinSeeThroughSet(const IEditable *part);
   void PinMaterialSeeThrough(const string &materialName);
   // "Bulb set": the Traced Insert Bulb fields (sliders and the apply-to-all toggle) of a traced insert; also part of its see-through set
   bool HasBulbSet(const IEditable *part) const;
   void PinBulbSet(const IEditable *part);

   // Persistence: the pin list is stored on the (startup) table, written to its table.json
   void SaveTo(PinTable *table) const;

private:
   struct GroupRequest
   {
      PinKind kind;
      string name;
      string group;
   };

   void SyncWithTable();
   void *Resolve(PinKind kind, const string &name, const void **owner) const;
   bool Harvest(PinKind kind, const string &name);
   void ProcessGroupRequests();
   void RenderHeader();
   void ApplyUnits(PropertyPane &props) const;
   void PinSelectionSeeThrough();

   EditorUI &m_editor;
   bool m_visible = false;
   bool m_collapsed = false;
   bool m_hovered = false;
   vector<PinKey> m_pins; // In pinning order (rows are grouped by object, in order of first appearance)
   std::map<string, Def> m_defs; // By PinKey::Id()
   vector<GroupRequest> m_groupRequests; // Quick set requests, applied once the object's definitions are known
   std::map<string, const void *> m_harvested; // Objects already harvested (by kind + name) with the owner they were harvested from
   std::map<string, unsigned> m_harvestStamps; // Stamp of the latest harvest of each object (by kind + name)
   unsigned m_harvestCounter = 0;
   unsigned m_harvestStamp = 0; // Stamp of the harvest in progress (0 when not harvesting)
   int m_harvestSeq = 0;
   const PinTable *m_sourceTable = nullptr; // Base table the pin list was loaded from
};

}
