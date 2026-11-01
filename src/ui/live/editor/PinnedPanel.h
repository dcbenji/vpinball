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
   bool NeedsDef(const string &id, const void *owner) const;
   void SetDef(const PinKey &key, const string &group, const void *owner, PinDrawFn &&draw);

   // "See-through set": a primitive's refraction fields and its material's See-Through Surface fields
   bool HasSeeThroughSet(const IEditable *part) const;
   void PinSeeThroughSet(const IEditable *part);
   void PinMaterialSeeThrough(const string &materialName);

   // Persistence: the pin list is stored on the (startup) table, written to its table.json
   void SaveTo(PinTable *table) const;

private:
   struct Def
   {
      PinKey key;
      string group; // Section or separator under which the field is drawn in the panel
      const void *owner = nullptr; // Part: its EditorUIPart (definitions may capture it), otherwise the object itself
      PinDrawFn draw;
   };
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
   const PinTable *m_sourceTable = nullptr; // Base table the pin list was loaded from
};

}
