// license:GPLv3+

#pragma once

#include "imgui/imgui.h"

class IEditable;
class Texture;
class Material;

namespace VPX::EditorUI
{

class EditorUI;

// Table Check: an area editor listing the problems found in the table. Read-only, it never changes the table.
// The checks themselves (TableCheckPanel.cpp, kChecks) are a list of plain functions: adding a check is one function and one line in that list.
class TableCheckPanel
{
public:
   explicit TableCheckPanel(EditorUI &editor)
      : m_editor(editor)
   {
   }

   void RenderInArea(const ImVec2 &pos, const ImVec2 &size, int leafId); // (any number of areas show it: the findings and the severity filter are shared) // Window filling the given area rect, header included (runs the checks only while shown)

   enum class Severity : int
   {
      Error,
      Warning,
      Info
   };

   // One problem. The part / image / material pointers are only used to select on click, after checking they are still in the table
   struct Finding
   {
      Severity severity = Severity::Warning;
      int check = 0; // Index in the check list (groups the findings)
      string name; // Part (or image / material) name
      string text;
      IEditable *part = nullptr;
      Texture *image = nullptr;
      Material *material = nullptr;
   };

private:
   void Scan();
   void SelectFinding(const Finding &finding, bool frame);

   EditorUI &m_editor;
   vector<Finding> m_findings;
   bool m_scanned = false;
   bool m_show[3] = { true, true, true }; // Severity filter: errors, warnings, infos

   // Change detection: the table is rescanned a moment after something moved (undo history, part / resource counts, end of a drag), never while dragging
   struct Signature
   {
      const void *table = nullptr;
      uint64_t topRecord = 0;
      size_t parts = 0, images = 0, materials = 0;
      int dirty = 0; // The table's dirty state
      size_t names = 0; // Hash of the image and material names and of the table settings the checks read (non-undoable edits)
      float bounds[4] = {}; // Playfield rectangle
      bool operator==(const Signature &o) const = default;
   };
   Signature m_signature;
   bool m_unsettled = false; // The signature changed since the last interaction ended
   unsigned int m_notifyId = 0; // Last notification of a click on a row (reused, so a double click shows one)
   ImGuiID m_prevActiveId = 0; // To see an edit end: the end of an interaction (a coalesced edit keeps one undo record) is the last change to look at
   bool m_pending = false;
   double m_changedAt = 0.0;
};

}
