// license:GPLv3+

#pragma once

#include "PinnedPanel.h"

#include <unordered_map>

namespace VPX::EditorUI
{

class EditorUI;

// Cmd/Ctrl+K palette: type part of an object name and/or a field label, Enter selects the object and scrolls the properties
// panel to the field (flashing it), Cmd/Ctrl+Enter pins the field to the overlay. The index is the pin field registry: field
// labels per object type, learned from harvests of the objects' panes.
class SearchPalette
{
public:
   explicit SearchPalette(EditorUI &editor);
   ~SearchPalette();

   void Open();
   void Close() { m_open = false; }
   void Toggle() { m_open ? Close() : Open(); }
   bool IsOpen() const { return m_open; }

   void Render();

   // True while the Esc press that closed the palette is still held, and on the frame of its release: the editor's own Esc
   // handler (which acts on the release) must skip that Esc. Call it every frame the editor checks for Esc.
   bool ConsumedEscape();

private:
   struct Field
   {
      string label, group;
      string labelLower, groupLower;
   };
   // Object type (table, material or part item type) with the fields learned from the pin registry
   struct TypeInfo
   {
      string name, lower;
      vector<Field> fields;
      int queued = 0; // Representative objects queued for harvest (conditional fields differ per object)
   };
   struct Object
   {
      PinKind kind;
      string name; // Registry name (empty for the table)
      string display, lower;
      int type;
   };
   struct Result
   {
      int obj; // Index in m_objects
      int field; // Index in the type's fields, -1: object-only or recent field not in the catalog
      int score;
      bool isField;
      string prop; // Field label (field rows)
      string label, group; // Cached display strings
      string right;
   };
   struct Recent
   {
      PinKind kind;
      string name;
      string prop;
      bool isField;
   };
   struct Candidate
   {
      int obj, field, score;
   };

   void BuildIndex();
   int TypeIndex(const string &key, const string &name);
   void RefreshCatalog();
   void QueueMissingTypes();
   void Harvest();
   void Recompute();
   void MakeResult(Result &r) const;
   void Activate(const Result &r, bool pin);
   void Remember(const Result &r);

   EditorUI &m_editor;
   bool m_open = false;
   bool m_focusInput = false;
   bool m_firstFrame = false;
   bool m_indexStale = true;
   bool m_dirty = true;
   bool m_harvestSelection = false;
   bool m_scrollToSel = false;
   bool m_hasPending = false;
   bool m_pendingPin = false;
   size_t m_pending = 0;
   int m_sel = 0;
   size_t m_indexParts = 0, m_indexMats = 0;
   string m_query;
   string m_lastQuery;

   vector<TypeInfo> m_types;
   std::unordered_map<string, int> m_typeByKey;
   std::unordered_map<string, int> m_partType; // Part name -> type
   std::unordered_map<int, int> m_itemTypeIdx; // Item type -> type index
   bool m_escPending = false;
   int m_tableType = -1, m_matType = -1;
   vector<Object> m_objects;
   struct QueueItem
   {
      PinKind kind;
      string name;
   };
   vector<QueueItem> m_queue;
   vector<Result> m_results;
   vector<Recent> m_recent;
};

}
