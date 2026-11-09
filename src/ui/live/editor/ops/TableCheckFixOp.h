// license:GPLv3+

#pragma once

#include "parts/tablecheck.h"

class PinTable;
class PinUndo;

namespace VPX::EditorUI
{

class EditorUI;

// One click fixes of the Table Check pane. Only the safe, deterministic fixes the checks offer (TableIssue::fix): the
// table check decides when a fix is safe (for example, a timer is only disabled when the script loads no other code).
// Each fix is undoable, several fixes applied together are one undo step.
class TableCheckFixOp final
{
public:
   static const char *Label(TableFix fix); // Short action label, e.g. "Disable Timer"
   static bool IsApplicable(const PinTable *table, const TableIssue &issue); // The issue has a fix and its target is still in the table, unfixed

   // Applies the fixes of the issues to the table, recording them in the undo record being built (the caller begins and ends it).
   // Returns the parts that changed (their renderables and physics need an update). Issues without a fix, or whose target is no
   // longer in the table, are skipped.
   static vector<IEditable *> Apply(PinTable *table, PinUndo &undo, const vector<const TableIssue *> &issues);

   // The issues for which a fresh check of the table still offers the same fix: a list of issues may be older than the table (e.g.
   // a script edit not checked yet). Fix only applies these.
   static vector<const TableIssue *> Confirmed(const PinTable *table, const vector<const TableIssue *> &issues);

   // Editor op: applies the fixes as one labelled undo step (full edit mode only: an inspected live copy is not undoable)
   static void Fix(EditorUI &editor, const vector<const TableIssue *> &issues);
};

}
