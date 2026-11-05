// license:GPLv3+

#pragma once

#include "imgui/imgui.h"

#include <map>
#include <memory>
#include <unordered_set>

class IEditable;
class PinTable;

namespace VPX::EditorUI
{

class EditorUI;
struct LocalViewParts;

// Edit > Select Similar (Shift+G) and Edit > Batch Rename (Ctrl+F2), after Blender's.
//
// Select Similar extends the selection with the parts which share a property with the active part: the type, the material, the image, a collection,
// the part group, or the name prefix (the name without its trailing number). It takes the parts Select All takes: the pickable ones (not hidden), and
// when the view under the mouse is a Local View only those it shows.
//
// Batch Rename renames the selected parts with a stack of operations (find / replace, set name, add or strip prefix / suffix, numbering, case), shows
// the old and new names and refuses to apply while a result is not usable (a duplicate, a name another part, a collection, the table or the script
// already uses, a name which is not a script identifier). It applies as ONE undo step. The parts which hold names of other parts (rubbers following
// posts, plastic companions, parts placed on a wall or ramp) follow in the same step. The table script refers to parts by name: the dialog lists the
// lines which use the names being changed and can replace them (whole words, and the event handlers such as Bumper1_Hit) in the same undo step.
class SelectRename final
{
public:
   explicit SelectRename(EditorUI &editor);

   bool CanSelectSimilar() const;
   void OpenSimilar(); // The popup of the modes at the mouse
   bool CanRename() const;
   void OpenRename();
   void Close(); // The editor closes or the table changes
   bool IsRenameOpen() const { return m_renameOpen; }

   void Render(); // Every frame: the popup and the dialog

private:
   enum class Similar
   {
      Type,
      Material,
      Image,
      Collection,
      PartGroup,
      NamePrefix
   };
   void SelectSimilar(Similar mode);
   void RenderSimilarPopup();

   struct Op
   {
      enum Type
      {
         FindReplace,
         SetName,
         PrefixSuffix,
         Numbering,
         Case
      };
      Type type = FindReplace;
      string find, replace; // Find / Replace
      bool regex = false;
      bool matchCase = true;
      string text; // Set Name; Prefix / Suffix
      bool strip = false; // Prefix / Suffix: strip the text instead of adding it
      bool suffix = false; // Prefix / Suffix: at the end
      int start = 1, step = 1, padding = 3; // Numbering
      string separator = "_";
      bool replaceTrailing = true; // Numbering: the name loses its trailing digits first
      int caseMode = 0; // Case: upper, lower, title
      string error; // Regex that does not compile
   };
   struct Row
   {
      IEditable *part = nullptr;
      string oldName, newName, problem;
      bool changed = false;
   };
   struct ScriptUse
   {
      int line = 0;
      string text;
      bool handler = false; // A Sub Name_Event declaration
   };
   struct ScriptUses
   {
      int count = 0;
      vector<ScriptUse> lines; // The first few
   };

   string Signature() const;
   void Refresh();
   void BuildRows();
   void RenderRename();
   void ApplyRename();
   struct ScriptRename
   {
      PinTable *table = nullptr;
      EditorUI *editor = nullptr;
      string before, after; // The script text before the rename and the text it produced
      vector<std::pair<string, string>> names; // (old, new) of the renamed parts
   };
   void SwapScript(const std::shared_ptr<const ScriptRename> &rename, bool toBefore);

   EditorUI &m_editor;

   bool m_similarOpen = false;
   bool m_similarJustOpened = false;
   ImVec2 m_similarPos;
   const LocalViewParts *m_similarLocal = nullptr; // The Local View of the view the mouse was over when the popup opened

   bool m_renameOpen = false;
   bool m_renameJustOpened = false;
   vector<Op> m_ops;
   bool m_numberByName = false; // Numbering order: the selection order, or the sorted old names
   bool m_replaceInScript = false;
   vector<Row> m_rows;
   int m_nChanged = 0, m_nProblems = 0;
   string m_sig, m_pendingSig; // What the rows were computed for; what the dialog holds now
   bool m_sigValid = false;
   bool m_stale = false; // The inputs changed and the preview waits for the typing to pause
   double m_pendingSince = 0.0;
   size_t m_declaredSig = 0;
   std::unordered_set<string> m_declared; // Names the script declares (lower case)
   std::map<string, ScriptUses> m_scriptUses; // By old name (lower case)
   int m_scriptUseLines = 0;
};

}
