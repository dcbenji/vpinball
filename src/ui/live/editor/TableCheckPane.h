// license:GPLv3+

#pragma once

#include "EditorAreaView.h"
#include "parts/tablecheck.h"

namespace VPX::EditorUI
{

// Table Check pane: lists the problems found in the table (PinTable::CheckTable), grouped by check. A click on an issue
// selects its part, a double click frames it. The table is checked again when it changes (undo history, parts, assets,
// script) or on demand.
class TableCheckPane final : public EditorAreaView
{
public:
   explicit TableCheckPane(EditorUI &editor)
      : EditorAreaView(editor)
   {
   }

   AreaType GetType() const override { return AreaType::TableCheck; }
   void RenderPane(const AreaRect &rect, int leafId) override;
   void AppendStatusHints(vector<StatusHint> &hints) const override;
   nlohmann::json SaveSetup() const override;
   void LoadSetup(const nlohmann::json &j) override;

   // The checks run by the live editor: no audio player (sound checks), no size totals
   static constexpr TableCheckOptions LiveOptions {};

private:
   void Scan();
   void Select(const TableIssue &issue, bool frame);

   // What the issues depend on: a change of any of these rechecks the table
   struct Signature
   {
      const PinTable *table = nullptr;
      uint64_t undoId = 0;
      size_t parts = 0, images = 0, materials = 0, sounds = 0, collections = 0;
      int dirty = 0;
      uint32_t script = 0; // Script revision
      bool operator==(const Signature &) const = default;
   };
   Signature CurrentSignature() const;

   vector<TableIssue> m_issues;
   Signature m_signature;
   bool m_scanned = false;
   bool m_pending = false; // The table changed: checked again once the mouse is released
   bool m_show[3] = { true, true, true }; // Shown severities: errors, warnings, infos
};

}
