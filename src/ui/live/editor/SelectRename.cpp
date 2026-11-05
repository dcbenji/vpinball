// license:GPLv3+

#include "core/stdafx.h"

#include "SelectRename.h"
#include "EditorUIPart.h"
#include "Selection.h"
#include "EditorTheme.h"
#include "PropertiesPanel.h"
#include "parts/Collection.h"
#include "parts/decal.h"
#include "parts/ball.h"
#include "parts/dispreel.h"
#include "parts/flipper.h"
#include "parts/flasher.h"
#include "parts/hittarget.h"
#include "parts/light.h"
#include "parts/PartGroup.h"
#include "parts/plunger.h"
#include "parts/primitive.h"
#include "parts/ramp.h"
#include "parts/rubber.h"
#include "parts/spinner.h"
#include "parts/surface.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

#include <numeric>
#include <optional>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace VPX::EditorUI
{

namespace
{

constexpr size_t kMaxListedLines = 3; // Script lines shown per renamed part

bool IsAlpha(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsDigit(const char c) { return c >= '0' && c <= '9'; }
bool IsWordChar(const char c) { return IsAlpha(c) || IsDigit(c) || c == '_'; }

string Lower(const string &s)
{
   string r = s;
   for (char &c : r)
      if (c >= 'A' && c <= 'Z')
         c = static_cast<char>(c - 'A' + 'a');
   return r;
}

// The image of a part (empty if it has none), as PropertiesPanel::PartMaterial does for the material
string PartImage(const IEditable *part)
{
   switch (part->GetItemType())
   {
   case eItemPrimitive: return static_cast<const Primitive *>(part)->m_d.m_szImage;
   case eItemRamp: return static_cast<const Ramp *>(part)->m_d.m_szImage;
   case eItemSurface: return static_cast<const Surface *>(part)->m_d.m_szImage;
   case eItemDecal: return static_cast<const Decal *>(part)->m_d.m_szImage;
   case eItemHitTarget: return static_cast<const HitTarget *>(part)->m_d.m_szImage;
   case eItemPlunger: return static_cast<const Plunger *>(part)->m_d.m_szImage;
   case eItemRubber: return static_cast<const Rubber *>(part)->m_d.m_szImage;
   case eItemSpinner: return static_cast<const Spinner *>(part)->m_d.m_szImage;
   case eItemLight: return static_cast<const Light *>(part)->m_d.m_szImage;
   case eItemFlasher: return static_cast<const Flasher *>(part)->m_d.m_szImageA;
   case eItemDispReel: return static_cast<const DispReel *>(part)->m_d.m_szImage;
   case eItemFlipper: return static_cast<const Flipper *>(part)->m_d.m_szImage;
   case eItemBall: return static_cast<const Ball *>(part)->m_d.m_szImage;
   default: return string();
   }
}

// The name without its trailing number and separator ("Post_012" and "Post3" -> "Post"); a name made of digits only stays whole
string NamePrefix(const string &name)
{
   size_t end = name.size();
   while (end > 0 && IsDigit(name[end - 1]))
      --end;
   while (end > 0 && (name[end - 1] == '_' || name[end - 1] == '.' || name[end - 1] == '-' || name[end - 1] == ' '))
      --end;
   return end == 0 ? name : name.substr(0, end);
}

// A part name is a script identifier: ASCII letters, digits and underscores, not starting with a digit
bool IsScriptIdentifier(const string &name)
{
   if (name.empty() || name.size() >= MAXNAMEBUFFER || IsDigit(name[0]))
      return false;
   return std::ranges::all_of(name, IsWordChar);
}

// The part which the editor keeps its name: the implicit playfield is found by it
bool IsNameLocked(const IEditable *part) { return part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->IsPlayfield(); }

// Uses of part names in the table script: every word equal to a part's old name (case insensitive, as VBScript), and every word of the form
// <Old>_<Suffix> (event handlers Sub Old_Hit and their calls, GetRef("Old_Hit"), timer strings): the part is the longest prefix of the word which
// is the name of a part (or collection). In code, comments and strings alike.
struct ScriptHit
{
   size_t begin = 0, length = 0; // The part's name in the script text
   int line = 0;
   size_t lineStart = 0;
   string key; // Lower case old name
   string suffix; // Lower case, with its underscore, for the <Old>_<Suffix> form (empty for the name alone)
   bool decl = false; // Sub <Old>_<Suffix>
};

vector<ScriptHit> FindScriptHits(const string &text, const std::unordered_set<string> &allNames, const std::unordered_set<string> &keys)
{
   vector<ScriptHit> hits;
   const size_t n = text.size();
   size_t i = 0, lineStart = 0;
   int line = 1;
   string prev1, prev2; // The identifiers before, on this line
   while (i < n)
   {
      const char c = text[i];
      if (c == '\n')
      {
         line++;
         lineStart = ++i;
         prev1.clear();
         prev2.clear();
         continue;
      }
      if (IsAlpha(c) || c == '_')
      {
         size_t j = i + 1;
         while (j < n && IsWordChar(text[j]))
            ++j;
         const string word = Lower(text.substr(i, j - i));
         const bool decl = prev1 == "sub" && prev2 != "end";
         if (keys.contains(word))
            hits.push_back({ i, j - i, line, lineStart, word, string(), false });
         else if (!allNames.contains(word)) // The full name of another part (or collection) is never split: Bumper_Reset is not Bumper + _Reset
         {
            size_t best = 0;
            for (size_t k = 1; k + 1 < word.size(); k++)
               if (word[k] == '_' && allNames.contains(word.substr(0, k)))
                  best = k;
            if (best > 0 && keys.contains(word.substr(0, best)))
               hits.push_back({ i, best, line, lineStart, word.substr(0, best), word.substr(best), decl });
         }
         prev2 = prev1;
         prev1 = word;
         i = j;
         continue;
      }
      if (IsDigit(c)) // A number, or a word which starts with a digit: not a name
      {
         while (i < n && IsWordChar(text[i]))
            ++i;
         continue;
      }
      ++i;
   }
   return hits;
}

// A quantified group which contains a quantifier ((a+)+, (.*)*, (\\w+\\s?)*) can backtrack without end on a name that does not match
bool HasNestedQuantifier(const string &pattern)
{
   vector<bool> groupHasQuantifier, groupHasAlternation;
   const size_t n = pattern.size();
   bool inClass = false;
   const auto isQuantifier = [&](const size_t at) { return pattern[at] == '*' || pattern[at] == '+' || pattern[at] == '?' || (pattern[at] == '{' && at + 1 < n && IsDigit(pattern[at + 1])); };
   for (size_t i = 0; i < n; i++)
   {
      const char c = pattern[i];
      if (c == '\\')
      {
         ++i;
         continue;
      }
      if (inClass)
      {
         inClass = c != ']';
         continue;
      }
      if (c == '[')
         inClass = true;
      else if (c == '|')
      {
         if (!groupHasAlternation.empty())
            groupHasAlternation.back() = true;
      }
      else if (c == '(')
      {
         groupHasQuantifier.push_back(false);
         groupHasAlternation.push_back(false);
         if (i + 2 < n && pattern[i + 1] == '?') // (?: (?= (?!
            i += 2;
      }
      else if (c == ')')
      {
         if (groupHasQuantifier.empty())
            continue;
         const bool inner = groupHasQuantifier.back();
         const bool alternation = groupHasAlternation.back();
         groupHasQuantifier.pop_back();
         groupHasAlternation.pop_back();
         if (alternation && !groupHasAlternation.empty())
            groupHasAlternation.back() = true;
         if (i + 1 < n && isQuantifier(i + 1) && pattern[i + 1] != '?')
         {
            if (inner || alternation) // (a+)+  and  (a|aa)+
               return true;
            if (!groupHasQuantifier.empty())
               groupHasQuantifier.back() = true;
         }
         else if (inner && !groupHasQuantifier.empty())
            groupHasQuantifier.back() = true; // An unquantified group inside another one: its quantifiers count for the parent
      }
      else if (isQuantifier(i) && !groupHasQuantifier.empty())
         groupHasQuantifier.back() = true;
   }
   return false;
}

// VBScript words a part cannot be named
const std::unordered_set<string> &VbsKeywords()
{
   static const std::unordered_set<string> words = { "and", "as", "byref", "byval", "call", "case", "class", "const", "default", "dim", "do", "each", "else", "elseif", "empty", "end", "eqv",
      "erase", "error", "execute", "executeglobal", "exit", "explicit", "false", "for", "function", "get", "goto", "if", "imp", "in", "is", "let", "loop", "me", "mod", "new", "next", "not",
      "nothing", "null", "on", "option", "or", "preserve", "private", "property", "public", "randomize", "redim", "rem", "resume", "select", "set", "step", "stop", "sub", "then", "to", "true",
      "until", "wend", "while", "with", "xor", "eval", "err", "typename", "vartype", "msgbox", "inputbox", "createobject", "getobject", "wscript" };
   return words;
}

// The names the script declares (lower case): Sub, Function, Class, Property, Dim, ReDim, Const, Public, Private. Comments and strings are skipped
std::unordered_set<string> CollectDeclared(const string &text)
{
   std::unordered_set<string> declared;
   bool continued = false; // The line ended with " _": the statement goes on
   vector<string> tokens; // The statement: identifiers (lower case), punctuation as single characters, "#" for a number or a string
   const auto flush = [&]()
   {
      size_t i = 0;
      while (i < tokens.size() && (tokens[i] == "public" || tokens[i] == "private" || tokens[i] == "static" || tokens[i] == "default"))
         ++i;
      const bool modifier = i > 0 && (tokens[0] == "public" || tokens[0] == "private");
      if (i < tokens.size())
      {
         const string &head = tokens[i];
         if ((head == "sub" || head == "function" || head == "class") && i + 1 < tokens.size() && IsWordChar(tokens[i + 1][0]))
            declared.insert(tokens[i + 1]);
         else if (head == "property" && i + 2 < tokens.size() && IsWordChar(tokens[i + 2][0]))
            declared.insert(tokens[i + 2]);
         else if (head == "dim" || head == "redim" || head == "const" || (modifier && head != "end" && IsWordChar(head[0])))
         {
            size_t k = (head == "dim" || head == "redim" || head == "const") ? i + 1 : i;
            if (k < tokens.size() && tokens[k] == "preserve")
               ++k;
            const bool isConst = head == "const";
            while (k < tokens.size())
            {
               if (IsWordChar(tokens[k][0]) && !IsDigit(tokens[k][0]))
                  declared.insert(tokens[k]);
               ++k;
               int depth = 0; // Array bounds, and a Const's value, up to the next comma at depth 0
               while (k < tokens.size() && !(depth == 0 && tokens[k] == ","))
               {
                  if (tokens[k] == "(")
                     depth++;
                  else if (tokens[k] == ")")
                     depth--;
                  else if (!isConst && depth == 0 && tokens[k] != "(" )
                     break;
                  ++k;
               }
               if (k < tokens.size() && tokens[k] == ",")
                  ++k;
               else
                  break;
            }
         }
      }
      tokens.clear();
   };
   const size_t n = text.size();
   size_t i = 0;
   while (i < n)
   {
      const char c = text[i];
      if (c == '\n' && continued)
      {
         continued = false; // The statement goes on the next line
         ++i;
      }
      else if (c == '\n' || c == ':')
      {
         flush();
         ++i;
      }
      else if (c == '\'')
         while (i < n && text[i] != '\n')
            ++i;
      else if (c == '"')
      {
         ++i;
         while (i < n && !(text[i] == '"' && (i + 1 >= n || text[i + 1] != '"')))
            i += (text[i] == '"') ? 2 : 1;
         ++i;
         tokens.push_back("#");
      }
      else if (IsWordChar(c))
      {
         size_t j = i;
         while (j < n && IsWordChar(text[j]))
            ++j;
         string word = Lower(text.substr(i, j - i));
         if (word == "_") // A line continuation when only blanks follow
         {
            size_t k = j;
            while (k < n && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r'))
               ++k;
            if (k >= n || text[k] == '\n')
            {
               continued = true;
               word.clear();
            }
         }
         if (word == "rem" && tokens.empty()) // A comment statement
         {
            while (j < n && text[j] != '\n')
               ++j;
            word.clear();
         }
         if (!word.empty())
            tokens.push_back(IsDigit(word[0]) ? string("#") : word);
         i = j;
      }
      else
      {
         if (c != ' ' && c != '\t' && c != '\r')
            tokens.push_back(string(1, c));
         ++i;
      }
   }
   flush();
   return declared;
}

}

SelectRename::SelectRename(EditorUI &editor)
   : m_editor(editor)
{
}

void SelectRename::Close()
{
   m_similarOpen = false;
   m_renameOpen = false;
   m_rows.clear();
   m_scriptUses.clear();
   m_sigValid = false;
   m_stale = false;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// Select Similar
// ---------------------------------------------------------------------------------------------------------------------------------------------------

bool SelectRename::CanSelectSimilar() const
{
   const EditorUI &ed = m_editor;
   return ed.CanSelect() && ed.m_pointEditPart == nullptr && ed.m_selection.GetType() == Selection::S_EDITABLE && ed.m_selection.GetPart() != nullptr
      && ed.m_selection.GetPart()->GetEditable() != nullptr;
}

void SelectRename::OpenSimilar()
{
   if (!CanSelectSimilar())
      return;
   m_similarOpen = true;
   m_similarJustOpened = true;
   m_similarPos = ImGui::GetMousePos();
   m_similarLocal = m_editor.KeyLocalView(); // The view under the mouse now (the popup covers it later)
}

void SelectRename::SelectSimilar(const Similar mode)
{
   EditorUI &ed = m_editor;
   if (!CanSelectSimilar())
      return;
   const std::shared_ptr<EditorUIPart> active = ed.m_selection.GetPart();
   const IEditable *const ref = active->GetEditable();
   const LocalViewParts &local = m_similarLocal != nullptr ? *m_similarLocal : *ed.KeyLocalView();
   const PinTable *const table = ed.m_table;

   const string refMaterial = Lower(PropertiesPanel::PartMaterial(ref));
   const string refImage = Lower(PartImage(ref));
   const string refPrefix = Lower(NamePrefix(ref->GetName()));
   std::unordered_set<const IEditable *> inCollection; // The parts of the collections which hold the active part
   if (mode == Similar::Collection)
      for (const Collection *const collection : table->GetCollections())
         if (std::ranges::find(collection->GetParts(), const_cast<IEditable *>(ref)) != collection->GetParts().end())
            inCollection.insert(collection->GetParts().begin(), collection->GetParts().end());

   const char *what = "Type";
   switch (mode)
   {
   case Similar::Type: what = "Type"; break;
   case Similar::Material: what = "Material"; break;
   case Similar::Image: what = "Image"; break;
   case Similar::Collection: what = "Collection"; break;
   case Similar::PartGroup: what = "Part Group"; break;
   case Similar::NamePrefix: what = "Name Prefix"; break;
   }
   if ((mode == Similar::Material && refMaterial.empty()) || (mode == Similar::Image && refImage.empty()) || (mode == Similar::Collection && inCollection.empty())
      || (mode == Similar::PartGroup && ref->GetPartGroup() == nullptr))
   {
      ed.Report("Select Similar ("s + what + "): " + ref->GetName() + " has none");
      return;
   }

   int added = 0;
   for (const auto &uiPart : ed.m_editables)
   {
      IEditable *const editable = uiPart->GetEditable();
      // The parts Select All takes: not the part groups, the pickable ones (not hidden), those the view shows
      if (editable == nullptr || editable->GetItemType() == eItemPartGroup || !ed.IsEditablePickable(editable) || !local.Allows(editable) || ed.IsPartSelected(uiPart))
         continue;
      bool similar = false;
      switch (mode)
      {
      case Similar::Type: similar = editable->GetItemType() == ref->GetItemType(); break;
      case Similar::Material: similar = Lower(PropertiesPanel::PartMaterial(editable)) == refMaterial; break;
      case Similar::Image: similar = Lower(PartImage(editable)) == refImage; break;
      case Similar::Collection: similar = inCollection.contains(editable); break;
      case Similar::PartGroup: similar = editable->GetPartGroup() == ref->GetPartGroup(); break;
      case Similar::NamePrefix: similar = Lower(NamePrefix(editable->GetName())) == refPrefix; break;
      }
      if (similar)
      {
         ed.m_multiSel.push_back(uiPart);
         added++;
      }
   }
   ed.m_outlinerAnchor.reset();
   ed.Report("Select Similar ("s + what + "): " + std::to_string(added) + " added, " + std::to_string(ed.m_multiSel.size()) + " selected");
}

void SelectRename::RenderSimilarPopup()
{
   if (!m_similarOpen)
      return;
   if (!CanSelectSimilar())
   {
      m_similarOpen = false;
      return;
   }
   EditorUI &ed = m_editor;
   const IEditable *const ref = ed.m_selection.GetPart()->GetEditable();
   const bool hasMaterial = !PropertiesPanel::PartMaterial(ref).empty();
   const bool hasImage = !PartImage(ref).empty();
   const bool hasGroup = ref->GetPartGroup() != nullptr;
   bool hasCollection = false;
   for (const Collection *const collection : ed.m_table->GetCollections())
      if (std::ranges::find(collection->GetParts(), const_cast<IEditable *>(ref)) != collection->GetParts().end())
         hasCollection = true;

   ImGui::SetNextWindowPos(m_similarPos, ImGuiCond_Appearing);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   bool open = true;
   if (ImGui::Begin("Select Similar##Popup", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav))
   {
      ImGui::TextDisabled("Select Similar to %s", ref->GetName().c_str());
      ImGui::Separator();
      struct Entry
      {
         const char *label;
         Similar mode;
         bool enabled;
      };
      const Entry entries[] = { { "Type", Similar::Type, true }, { "Material", Similar::Material, hasMaterial }, { "Image", Similar::Image, hasImage },
         { "Collection", Similar::Collection, hasCollection }, { "Part Group", Similar::PartGroup, hasGroup }, { "Name Prefix", Similar::NamePrefix, true } };
      for (const Entry &entry : entries)
      {
         ImGui::BeginDisabled(!entry.enabled);
         if (ImGui::Selectable(entry.label))
         {
            SelectSimilar(entry.mode);
            open = false;
         }
         ImGui::EndDisabled();
      }
      if (!m_similarJustOpened && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
         && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
         open = false;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
         open = false;
         ed.m_paneEscPending = true; // The Esc is the popup's, not an editor Esc
      }
   }
   ImGui::End();
   ImGui::PopStyleColor();
   m_similarJustOpened = false;
   if (!open)
      m_similarOpen = false;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------
// Batch Rename
// ---------------------------------------------------------------------------------------------------------------------------------------------------

bool SelectRename::CanRename() const
{
   const EditorUI &ed = m_editor;
   return !ed.IsInspectMode() && !ed.m_table->IsLocked() && ed.m_fileOp == nullptr && ed.m_pointEditPart == nullptr && !ed.m_multiSel.empty();
}

void SelectRename::OpenRename()
{
   if (!CanRename())
      return;
   if (m_ops.empty())
      m_ops.push_back(Op {});
   m_renameOpen = true;
   m_renameJustOpened = true;
   m_sigValid = false;
   m_replaceInScript = false;
}

string SelectRename::Signature() const
{
   const EditorUI &ed = m_editor;
   const PinTable *const table = ed.m_table;
   string sig;
   for (const Op &op : m_ops)
   {
      sig += std::to_string(op.type) + '\x1f' + op.find + '\x1f' + op.replace + '\x1f' + op.text + '\x1f' + op.separator + '\x1f';
      sig += std::to_string(op.regex) + std::to_string(op.matchCase) + std::to_string(op.strip) + std::to_string(op.suffix) + std::to_string(op.replaceTrailing);
      sig += std::to_string(op.start) + ',' + std::to_string(op.step) + ',' + std::to_string(op.padding) + ',' + std::to_string(op.caseMode) + '\x1e';
   }
   sig += std::to_string(m_numberByName) + std::to_string(m_replaceInScript) + '|';
   for (const auto &uiPart : ed.m_multiSel)
      if (uiPart != nullptr && uiPart->GetEditable() != nullptr)
         sig += std::to_string(reinterpret_cast<uintptr_t>(uiPart->GetEditable())) + ':' + uiPart->GetEditable()->GetName() + ';';
   sig += '|' + std::to_string(table->m_script_text.size()) + ':' + std::to_string(std::hash<std::string_view>()(table->m_script_text));
   sig += '|' + std::to_string(table->GetParts().size()) + ',' + std::to_string(table->GetCollections().size()) + ',' + std::to_string(ed.m_undo.TopRecordId());
   return sig;
}

// The rows and the script uses are computed again only when what they depend on changed (not every frame). A regex is evaluated once the typing paused
void SelectRename::Refresh()
{
   const string sig = Signature();
   if (m_sigValid && sig == m_sig)
      return;
   const double now = ImGui::GetTime();
   if (sig != m_pendingSig)
   {
      m_pendingSig = sig;
      m_pendingSince = now;
   }
   const bool hasRegex = std::ranges::any_of(m_ops, [](const Op &op) { return op.type == Op::FindReplace && op.regex && !op.find.empty(); });
   if (m_sigValid && hasRegex && now - m_pendingSince < 0.4)
   {
      m_stale = true; // Typing: the preview below is of the previous input
      return;
   }
   m_stale = false;
   m_sig = sig;
   m_sigValid = true;
   BuildRows();
}

void SelectRename::BuildRows()
{
   EditorUI &ed = m_editor;
   const PinTable *const table = ed.m_table;
   m_rows.clear();
   for (Op &op : m_ops)
      op.error.clear();

   // Compiled once for all the names
   vector<std::optional<std::regex>> regexes(m_ops.size());
   for (size_t k = 0; k < m_ops.size(); k++)
   {
      Op &op = m_ops[k];
      if (op.type == Op::FindReplace && op.regex && !op.find.empty())
      {
         if (HasNestedQuantifier(op.find))
         {
            op.error = "pattern could be too slow (a quantified group contains a quantifier or an alternation)";
            continue;
         }
         try
         {
            regexes[k].emplace(op.find, op.matchCase ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase));
         }
         catch (const std::regex_error &e)
         {
            op.error = e.what();
         }
         catch (...)
         {
            op.error = "invalid pattern";
         }
      }
   }

   for (const auto &uiPart : ed.m_multiSel)
   {
      IEditable *const part = uiPart != nullptr ? uiPart->GetEditable() : nullptr;
      if (part == nullptr || part->GetIScriptable() == nullptr)
         continue;
      if (std::ranges::any_of(m_rows, [part](const Row &row) { return row.part == part; }))
         continue;
      Row row;
      row.part = part;
      row.oldName = part->GetName();
      m_rows.push_back(std::move(row));
   }

   // The order of the numbering: the selection order, or the old names sorted
   vector<size_t> order(m_rows.size());
   std::iota(order.begin(), order.end(), size_t(0));
   if (m_numberByName)
      std::ranges::stable_sort(order, [this](const size_t a, const size_t b) { return Lower(m_rows[a].oldName) < Lower(m_rows[b].oldName); });
   vector<int> numberIndex(m_rows.size(), 0);
   {
      int index = 0;
      for (const size_t r : order)
         if (!IsNameLocked(m_rows[r].part))
            numberIndex[r] = index++;
   }

   for (size_t r = 0; r < m_rows.size(); r++)
   {
      Row &row = m_rows[r];
      if (IsNameLocked(row.part))
      {
         row.newName = row.oldName;
         row.problem = "kept: the playfield is found by its name";
         continue;
      }
      string name = row.oldName;
      for (size_t k = 0; k < m_ops.size(); k++)
      {
         Op &op = m_ops[k];
         switch (op.type)
         {
         case Op::FindReplace:
            if (op.find.empty())
               break;
            if (op.regex)
            {
               if (regexes[k].has_value() && op.error.empty())
               {
                  if (name.size() > 128) // A long result (a Set Name before) is not given to the regex engine
                  {
                     op.error = "name too long for a pattern: " + row.oldName;
                     continue;
                  }
                  try
                  {
                     name = std::regex_replace(name, *regexes[k], op.replace);
                  }
                  catch (const std::exception &)
                  {
                     op.error = "pattern too complex for " + row.oldName; // Blocks the rename
                  }
               }
            }
            else
            {
               const string haystack = op.matchCase ? name : Lower(name);
               const string needle = op.matchCase ? op.find : Lower(op.find);
               string out;
               size_t pos = 0;
               for (size_t at; (at = haystack.find(needle, pos)) != string::npos; pos = at + needle.size())
               {
                  out.append(name, pos, at - pos);
                  out += op.replace;
               }
               out.append(name, pos, string::npos);
               name = std::move(out);
            }
            break;
         case Op::SetName: name = op.text; break;
         case Op::PrefixSuffix:
            if (op.text.empty())
               break;
            if (!op.strip)
               name = op.suffix ? name + op.text : op.text + name;
            else if (name.size() >= op.text.size())
            {
               if (!op.suffix && Lower(name.substr(0, op.text.size())) == Lower(op.text))
                  name.erase(0, op.text.size());
               else if (op.suffix && Lower(name.substr(name.size() - op.text.size())) == Lower(op.text))
                  name.erase(name.size() - op.text.size());
            }
            break;
         case Op::Numbering:
         {
            if (op.replaceTrailing)
            {
               size_t end = name.size();
               while (end > 0 && IsDigit(name[end - 1]))
                  --end;
               if (end > 0) // Keeps a name made of digits only
                  name.erase(end);
               if (!name.empty() && !op.separator.empty() && name.size() >= op.separator.size() && name.compare(name.size() - op.separator.size(), op.separator.size(), op.separator) == 0)
                  name.erase(name.size() - op.separator.size());
            }
            const long long value = std::max<long long>(0, static_cast<long long>(op.start) + static_cast<long long>(numberIndex[r]) * op.step);
            string digits = std::to_string(value);
            if (digits.size() < static_cast<size_t>(op.padding))
               digits.insert(0, static_cast<size_t>(op.padding) - digits.size(), '0');
            name += op.separator + digits;
            break;
         }
         case Op::Case:
            if (op.caseMode == 0)
               for (char &c : name)
                  c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
            else if (op.caseMode == 1)
               name = Lower(name);
            else
            {
               bool wordStart = true;
               for (char &c : name)
               {
                  if (IsAlpha(c))
                     c = wordStart ? ((c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c) : ((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
                  wordStart = !IsAlpha(c) && !IsDigit(c);
               }
            }
            break;
         }
      }
      row.newName = name;
      row.changed = name != row.oldName;
   }

   // The script: what it declares, and its uses of the names which change
   const string &text = table->m_script_text;
   if (m_declaredSig != text.size() * 31 + std::hash<std::string_view>()(text))
   {
      m_declaredSig = text.size() * 31 + std::hash<std::string_view>()(text);
      m_declared = CollectDeclared(text);
   }
   std::unordered_set<string> keys, allNames;
   for (const Row &row : m_rows)
      if (row.changed)
         keys.insert(Lower(row.oldName));
   m_scriptUses.clear();
   m_scriptUseLines = 0;
   vector<ScriptHit> hits;
   if (!keys.empty())
   {
      for (const IEditable *const part : table->GetParts())
         if (part->GetIScriptable() != nullptr)
            allNames.insert(Lower(part->GetName()));
      for (const Collection *const collection : table->GetCollections())
         allNames.insert(Lower(collection->m_name));
      hits = FindScriptHits(text, allNames, keys);
      int lastLine = 0;
      for (const ScriptHit &hit : hits)
      {
         ScriptUses &uses = m_scriptUses[hit.key];
         uses.count++;
         if (uses.lines.size() < kMaxListedLines && (uses.lines.empty() || uses.lines.back().line != hit.line))
         {
            const size_t end = text.find('\n', hit.lineStart);
            string lineText = text.substr(hit.lineStart, end == string::npos ? string::npos : end - hit.lineStart);
            while (!lineText.empty() && (lineText.back() == '\r' || lineText.back() == ' ' || lineText.back() == '\t'))
               lineText.pop_back();
            const size_t first = lineText.find_first_not_of(" \t");
            lineText = first == string::npos ? string() : lineText.substr(first);
            if (lineText.size() > 100)
               lineText = lineText.substr(0, 100) + "...";
            uses.lines.push_back({ hit.line, lineText, hit.decl });
         }
         if (hit.line != lastLine)
            m_scriptUseLines++;
         lastLine = hit.line;
      }
   }

   // Conflicts: only the parts which change are checked
   std::unordered_map<string, int> results; // Lower case new names of the parts which change
   for (const Row &row : m_rows)
      if (row.changed)
         results[Lower(row.newName)]++;
   std::unordered_map<string, string> handlerClash; // Old name (lower) -> a handler the script declares already under the new name (when the script is replaced)
   if (m_replaceInScript)
      for (const Row &row : m_rows)
         if (row.changed)
            for (const ScriptHit &hit : hits)
               if (hit.decl && hit.key == Lower(row.oldName) && Lower(row.newName) != Lower(row.oldName) && m_declared.contains(Lower(row.newName) + hit.suffix))
                  handlerClash[hit.key] = row.newName + hit.suffix;
   m_nChanged = 0;
   m_nProblems = 0;
   for (Row &row : m_rows)
   {
      if (!row.changed)
         continue;
      m_nChanged++;
      const string lowerName = Lower(row.newName);
      if (row.newName.empty())
         row.problem = "empty name";
      else if (!IsScriptIdentifier(row.newName))
         row.problem = row.newName.size() >= MAXNAMEBUFFER ? "longer than 31 characters" : "not a script name (letters, digits and _, not starting with a digit)";
      else if (VbsKeywords().contains(lowerName))
         row.problem = "a VBScript keyword";
      else if (results[lowerName] > 1)
         row.problem = "same result as another part";
      else if (lowerName != Lower(row.oldName) && !table->IsNameUnique(row.newName))
         row.problem = "already used (a part, a collection, the table, a global; any case)";
      else if (lowerName != Lower(row.oldName) && m_declared.contains(lowerName))
         row.problem = "already declared in the script (Dim, Const, Sub, Function, Class)";
      else if (const auto clash = handlerClash.find(Lower(row.oldName)); clash != handlerClash.end())
         row.problem = "the script already declares " + clash->second;
      if (!row.problem.empty())
         m_nProblems++;
   }
   for (const Op &op : m_ops)
      if (!op.error.empty())
         m_nProblems++;
}

// Restores or applies the script text of a rename record, when it is safe: the names it needs are free, and the script is still as the record left it
void SelectRename::SwapScript(const std::shared_ptr<const ScriptRename> &rename, const bool toBefore)
{
   PinTable *const table = rename->table;
   const string &from = toBefore ? rename->after : rename->before;
   const string &to = toBefore ? rename->before : rename->after;
   vector<string> blocked; // Names the script needs which another part, a collection or the script now holds
   for (const auto &[oldName, newName] : rename->names)
   {
      if (Lower(oldName) == Lower(newName))
         continue; // A change of case only: the part holds the name itself
      const string &needed = toBefore ? oldName : newName;
      if (!table->IsNameUnique(needed))
         blocked.push_back(needed);
   }
   if (!blocked.empty())
   {
      string list;
      for (const string &name : blocked)
         list += (list.empty() ? "" : ", ") + name;
      rename->editor->Report(string("Batch Rename: ") + (toBefore ? "undo" : "redo") + " could not restore " + list + " (name in use): the script was left as it is");
      PLOGW << "Batch Rename " << (toBefore ? "undo" : "redo") << ": names in use: " << list;
      return;
   }
   if (table->m_script_text == to)
      return; // Already as it should be (an undo that was blocked left the script renamed)
   if (table->m_script_text != from)
   {
      rename->editor->Report(string("Batch Rename: the script was edited since the rename, it was not ") + (toBefore ? "reverted" : "re-applied"));
      return;
   }
   table->m_script_text = to;
   table->SetDirtyScript(SaveDirtyState::Dirty);
}

void SelectRename::ApplyRename()
{
   EditorUI &ed = m_editor;
   m_sigValid = false;
   BuildRows();
   if (!CanRename() || m_nProblems > 0 || m_nChanged == 0)
      return;
   PinTable *const table = ed.m_table;

   vector<const Row *> changed;
   for (const Row &row : m_rows)
      if (row.changed)
         changed.push_back(&row);

   // The script uses are found with the old names, before anything changes
   std::unordered_map<string, string> renames; // Lower case old name -> new name
   for (const Row *const row : changed)
      renames[Lower(row->oldName)] = row->newName;
   vector<ScriptHit> hits;
   if (m_replaceInScript)
   {
      std::unordered_set<string> keys, allNames;
      for (const auto &entry : renames)
         keys.insert(entry.first);
      for (const IEditable *const part : table->GetParts())
         if (part->GetIScriptable() != nullptr)
            allNames.insert(Lower(part->GetName()));
      for (const Collection *const collection : table->GetCollections())
         allNames.insert(Lower(collection->m_name));
      hits = FindScriptHits(table->m_script_text, allNames, keys);
   }

   ed.m_undo.BeginUndo();
   for (const Row *const row : changed)
      ed.m_undo.MarkForUndo(row->part);
   for (const Row *const row : changed)
   {
      row->part->SetName(row->newName);
      // Rubbers following a post, plastic companions, parts placed on a wall or ramp and the lightmaps of a light hold its name: they follow, in the same record
      table->UpdateSurfaceReferences(row->part, row->oldName, [&ed](IEditable *dependent) { ed.m_undo.MarkForUndo(dependent, true, false); });
      ed.m_pinned.RenamePart(row->oldName, row->newName); // Pinned fields and favorites of the part
   }
   int scriptEdits = 0;
   if (!hits.empty())
   {
      auto rename = std::make_shared<ScriptRename>();
      rename->table = table;
      rename->editor = &ed;
      rename->before = table->m_script_text;
      string after;
      after.reserve(rename->before.size());
      size_t pos = 0;
      for (const ScriptHit &hit : hits) // In text order
      {
         after.append(rename->before, pos, hit.begin - pos);
         after += renames[hit.key];
         pos = hit.begin + hit.length;
      }
      after.append(rename->before, pos, string::npos);
      rename->after = after;
      for (const Row *const row : changed)
         rename->names.emplace_back(row->oldName, row->newName);
      table->m_script_text = after;
      table->SetDirtyScript(SaveDirtyState::Dirty);
      scriptEdits = static_cast<int>(hits.size());
      const std::shared_ptr<const ScriptRename> record = rename;
      ed.m_undo.MarkForAction([this, record]() { SwapScript(record, true); },
         // The snapshot of the state before an undo or a redo, symmetric: it returns what takes the script back to where it is now, if it is still the
         // renamed text or the text before the rename (a script edited since is left as it is)
         [this, record]() -> std::function<void()>
         {
            const string &now = record->table->m_script_text;
            if (now == record->after)
               return [this, record]() { SwapScript(record, false); };
            if (now == record->before)
               return [this, record]() { SwapScript(record, true); };
            return []() {};
         });
   }
   ed.m_undo.SetLabel(EditorUI::CountLabel("Batch Rename", changed.size(), changed.front()->oldName + " to " + changed.front()->newName));
   ed.m_undo.EndUndo(); // The record marks the table dirty (and undoing it back to the saved state makes it clean again)
   ed.Report("Renamed " + std::to_string(changed.size()) + (changed.size() == 1 ? " part" : " parts") + (scriptEdits > 0 ? ", " + std::to_string(scriptEdits) + " uses in the script" : string()));
   m_renameOpen = false;
   m_sigValid = false;
}

void SelectRename::RenderRename()
{
   if (!m_renameOpen)
      return;
   EditorUI &ed = m_editor;
   if (!CanRename())
   {
      m_renameOpen = false;
      return;
   }
   const float dpi = ed.m_liveUI.GetDPI();
   Refresh();

   ImGui::SetNextWindowSize(ImVec2(620.f * dpi, 0.f), ImGuiCond_Appearing);
   if (m_renameJustOpened)
   {
      const ImGuiViewport *const viewport = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(viewport->GetCenter().x, viewport->Pos.y + 90.f * dpi), ImGuiCond_Always, ImVec2(0.5f, 0.f));
   }
   bool open = true;
   if (ImGui::Begin("Batch Rename", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
   {
      if (m_renameJustOpened)
         ImGui::SetWindowFocus();
      ImGui::TextDisabled("%d selected, %d will change (Ctrl+F2)", static_cast<int>(m_rows.size()), m_nChanged);

      // The operations, applied in order to every name
      int removeAt = -1;
      for (size_t k = 0; k < m_ops.size(); k++)
      {
         Op &op = m_ops[k];
         ImGui::PushID(static_cast<int>(k));
         ImGui::Separator();
         static const char *const typeNames[] = { "Find / Replace", "Set Name", "Prefix / Suffix", "Numbering", "Case" };
         ImGui::SetNextItemWidth(150.f * dpi);
         if (ImGui::BeginCombo("##type", typeNames[op.type]))
         {
            for (int t = 0; t < 5; t++)
               if (ImGui::Selectable(typeNames[t], op.type == t))
                  op.type = static_cast<Op::Type>(t);
            ImGui::EndCombo();
         }
         ImGui::SameLine();
         if (ImGui::SmallButton("Remove"))
            removeAt = static_cast<int>(k);
         const auto fullWidth = []() { ImGui::SetNextItemWidth(-FLT_MIN); };
         switch (op.type)
         {
         case Op::FindReplace:
            fullWidth();
            EditorTheme::WithTextField([&] { return ImGui::InputTextWithHint("##find", "Find", &op.find); });
            fullWidth();
            EditorTheme::WithTextField([&] { return ImGui::InputTextWithHint("##replace", op.regex ? "Replace with (use $1 for groups)" : "Replace with", &op.replace); });
            ImGui::Checkbox("Regular expression", &op.regex);
            ImGui::SameLine();
            ImGui::Checkbox("Match case", &op.matchCase);
            if (!op.error.empty())
               ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "Pattern: %s", op.error.c_str());
            break;
         case Op::SetName:
            fullWidth();
            EditorTheme::WithTextField([&] { return ImGui::InputTextWithHint("##text", "New name", &op.text); });
            break;
         case Op::PrefixSuffix:
            fullWidth();
            EditorTheme::WithTextField([&] { return ImGui::InputTextWithHint("##text", op.strip ? "Text to strip" : "Text to add", &op.text); });
            if (ImGui::RadioButton("Add", !op.strip))
               op.strip = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Strip", op.strip))
               op.strip = true;
            ImGui::SameLine(0.f, 24.f * dpi);
            if (ImGui::RadioButton("Prefix", !op.suffix))
               op.suffix = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Suffix", op.suffix))
               op.suffix = true;
            break;
         case Op::Numbering:
            ImGui::SetNextItemWidth(90.f * dpi);
            ImGui::InputInt("Start", &op.start, 1, 10);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.f * dpi);
            ImGui::InputInt("Step", &op.step, 1, 10);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.f * dpi);
            ImGui::InputInt("Padding", &op.padding, 1, 1);
            op.start = std::clamp(op.start, 0, 999999);
            op.step = std::clamp(op.step, 1, 9999);
            op.padding = std::clamp(op.padding, 0, 9);
            ImGui::SetNextItemWidth(60.f * dpi);
            EditorTheme::WithTextField([&] { return ImGui::InputText("Separator", &op.separator); });
            ImGui::SameLine();
            ImGui::Checkbox("Replace trailing number", &op.replaceTrailing);
            ImGui::SameLine();
            ImGui::Checkbox("In name order", &m_numberByName);
            ImGui::SetItemTooltip("Number the parts in the order of their old names instead of the order they were selected in");
            break;
         case Op::Case:
            if (ImGui::RadioButton("UPPER", op.caseMode == 0))
               op.caseMode = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("lower", op.caseMode == 1))
               op.caseMode = 1;
            ImGui::SameLine();
            if (ImGui::RadioButton("Title", op.caseMode == 2))
               op.caseMode = 2;
            break;
         }
         ImGui::PopID();
      }
      if (removeAt >= 0)
         m_ops.erase(m_ops.begin() + removeAt);
      ImGui::Separator();
      if (m_ops.size() < 8 && ImGui::Button("Add Operation"))
         m_ops.push_back(Op {});

      // Old -> new
      ImGui::Spacing();
      ImGui::TextUnformatted("Preview");
      const float rowsHeight = std::min(10.f, static_cast<float>(m_rows.size()) + 1.f) * ImGui::GetTextLineHeightWithSpacing() + 4.f * dpi;
      if (ImGui::BeginTable("##rows", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg, ImVec2(0.f, rowsHeight)))
      {
         ImGui::TableSetupScrollFreeze(0, 1);
         ImGui::TableSetupColumn("Old name");
         ImGui::TableSetupColumn("New name");
         ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 1.4f);
         ImGui::TableHeadersRow();
         for (const Row &row : m_rows)
         {
            ImGui::TableNextRow();
            const bool bad = !row.problem.empty() && row.changed;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.oldName.c_str());
            ImGui::TableNextColumn();
            if (bad)
               ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", row.newName.empty() ? "(empty)" : row.newName.c_str());
            else if (row.changed)
               ImGui::TextUnformatted(row.newName.c_str());
            else
               ImGui::TextDisabled("%s", row.newName.c_str());
            ImGui::TableNextColumn();
            if (bad)
               ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", row.problem.c_str());
            else if (!row.problem.empty())
               ImGui::TextDisabled("%s", row.problem.c_str());
            else if (!row.changed)
               ImGui::TextDisabled("unchanged");
         }
         ImGui::EndTable();
      }

      // The table script refers to parts by name
      ImGui::Spacing();
      const string scriptTitle = m_scriptUses.empty() ? string("Table script: no use of these names") : "Table script: " + std::to_string(m_scriptUseLines) + " lines use these names";
      if (ImGui::CollapsingHeader((scriptTitle + "###ScriptUses").c_str(), m_scriptUses.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen) && !m_scriptUses.empty())
      {
         if (ImGui::BeginChild("##uses", ImVec2(0.f, 150.f * dpi), ImGuiChildFlags_Borders))
         {
            for (const Row &row : m_rows)
            {
               const auto it = m_scriptUses.find(Lower(row.oldName));
               if (!row.changed || it == m_scriptUses.end())
                  continue;
               ImGui::Text("%s -> %s: %d use%s", row.oldName.c_str(), row.newName.c_str(), it->second.count, it->second.count == 1 ? "" : "s");
               ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
               for (const ScriptUse &use : it->second.lines)
                  ImGui::Text("   %d: %s%s", use.line, use.text.c_str(), use.handler ? "   (handler declaration)" : "");
               if (static_cast<size_t>(it->second.count) > it->second.lines.size())
                  ImGui::Text("   ...");
               ImGui::PopStyleColor();
            }
         }
         ImGui::EndChild();
      }
      ImGui::Checkbox("Also replace in script", &m_replaceInScript);
      ImGui::SetItemTooltip("Whole words and Name_Suffix words (Sub Name_Hit, calls, GetRef(\"Name_Hit\")), any case, in code, comments and strings. Part of the same undo step.");
      if (m_replaceInScript && m_scriptUses.empty())
         ImGui::SameLine(), ImGui::TextDisabled("(nothing to replace)");
      ImGui::TextDisabled("Followed by the rename: collections, part groups, rubbers on posts, plastic companions, parts on a wall or ramp, lightmaps of a light, pinned fields, Local Views.");

      // Refuses while a result is not usable
      ImGui::Spacing();
      if (m_nProblems > 0)
         ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%d conflict%s: fix them to apply", m_nProblems, m_nProblems == 1 ? "" : "s");
      if (m_stale)
         ImGui::TextDisabled("Updating the preview...");
      ImGui::BeginDisabled(m_nProblems > 0 || m_nChanged == 0 || m_stale);
      const bool apply = ImGui::Button("Rename");
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel"))
         open = false;
      if (apply)
         ApplyRename();
      else if (!m_renameJustOpened && !ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
         open = false;
         ed.m_paneEscPending = true;
      }
   }
   ImGui::End();
   m_renameJustOpened = false;
   if (!open)
      m_renameOpen = false;
}

void SelectRename::Render()
{
   RenderSimilarPopup();
   RenderRename();
}

}
