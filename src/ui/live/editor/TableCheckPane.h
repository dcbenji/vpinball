// license:GPLv3+

#pragma once

#include "imgui/imgui.h"

#include "EditorAreaView.h"
#include "parts/tablecheck.h"

namespace VPX::EditorUI
{

// Table Check pane: lists the problems found in the table (PinTable::CheckTable, kept by the editor's TableCheckStatus),
// grouped by check. A click on an issue selects its part, a double click frames it.
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

   // The checks run by the live editor: no audio player (sound checks), no size totals, no objects made by a player session,
   // sorted by check. With log, the time it took is logged (table loading).
   static constexpr TableCheckOptions LiveOptions { .extended = true };
   static vector<TableIssue> LiveCheck(const PinTable &table, bool log = false, bool script = true);
   // The checks of a table on the editor thread (at start-up, after a change) run the script checks only for a script up to this size:
   // a bigger one is checked fully when a Table Check pane is shown
   static constexpr size_t MaxStartupScriptSize = 512 * 1024;

private:
   void Select(const TableIssue &issue, bool frame);

   bool m_show[3] = { true, true, true }; // Shown severities: errors, warnings, infos
};

// The issues of the edited table, their counts shown in the status bar and listed by the Table Check panes. A table loaded by
// the editor is checked by the loading thread before the editor of its session gets it (counts only, the issues are listed
// when a pane asks for them); a table the player started with, or the live copy of a play test, is checked on its first editor
// frame. Then the table is checked again when it changes (undo history, number of parts, images, materials, sounds and
// collections, dirty state, script revision), once the mouse button is released, the script edits paused and the play test is
// paused.
class TableCheckStatus
{
public:
   static void SetLoaded(const PinTable *table, const TableCheckSummary &summary); // Counts of a loaded table, for the editor of its session
   void Update(const EditorUI &editor); // Once per frame: checks the table again if it changed
   void Rescan(const EditorUI &editor, bool log = false, bool script = true);
   const vector<TableIssue> &GetIssues(const EditorUI &editor); // Checks the table if only its counts are known
   string Text() const; // Errors and warnings, e.g. "2 errors, 5 warnings"
   int InfoCount() const { return m_summary.counts[2]; }
   const TableCheckSummary &Summary() const { return m_summary; }
   bool IsPending() const { return m_pending; } // The table changed since the issues were made: it will be checked again
   ImU32 Color() const; // Of the most severe issue

private:
   // What the issues depend on: a change of any of these checks the table again
   struct Signature
   {
      const PinTable *table = nullptr;
      uint64_t undoId = 0;
      size_t parts = 0, images = 0, materials = 0, sounds = 0, collections = 0;
      int dirty = 0;
      uint32_t script = 0; // Script revision
      bool operator==(const Signature &) const = default;
   };
   static Signature CurrentSignature(const EditorUI &editor);

   const PinTable *m_table = nullptr; // Base table of the counts
   Signature m_signature;
   TableCheckSummary m_summary;
   vector<TableIssue> m_issues;
   bool m_haveIssues = false; // Only the counts are known (table checked by the loading thread)
   bool m_pending = false; // The table changed: checked again once the mouse is released
   static constexpr double ScriptEditPause = 1.0; // Seconds without a script edit before the table is checked again
   uint32_t m_lastScriptRevision = 0;
   double m_lastScriptEdit = -ScriptEditPause; // ImGui time of the last script revision seen
   static inline const PinTable *s_loadedTable = nullptr;
   static inline TableCheckSummary s_loadedSummary;
};

}
