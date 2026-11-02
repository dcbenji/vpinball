// license:GPLv3+

#include "core/stdafx.h"
#include "SearchPalette.h"

#include "core/editablereg.h"
#include "parts/Material.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

#include "EditorUIPart.h"
#include "PropertyPane.h"
#include "Selection.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_stdlib.h"

#include <algorithm>
#include <cctype>

namespace VPX::EditorUI
{

namespace
{

constexpr const char *kSep = "\xE2\x80\xBA"; // Single right-pointing angle quotation mark
constexpr const char *kDot = "\xC2\xB7"; // Middle dot
constexpr size_t kMaxResults = 60;
constexpr size_t kMaxRecent = 8;
constexpr size_t kMaxWords = 8;
constexpr int kHarvestsPerFrame = 3;

string Lower(const string &s)
{
   string r = s;
   for (char &c : r)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return r;
}

// 0: no match, 1: substring, 2: substring starting a word (both lowercase)
int MatchWord(const string &hay, const string &word)
{
   size_t pos = hay.find(word);
   if (pos == string::npos)
      return 0;
   while (pos != string::npos)
   {
      if (pos == 0 || !std::isalnum(static_cast<unsigned char>(hay[pos - 1])))
         return 2;
      pos = hay.find(word, pos + 1);
   }
   return 1;
}

}

SearchPalette::SearchPalette(EditorUI &editor)
   : m_editor(editor)
{
}

SearchPalette::~SearchPalette() = default;

void SearchPalette::Open()
{
   m_open = true;
   m_focusInput = true;
   m_firstFrame = true;
   m_indexStale = true;
   m_dirty = true;
   m_harvestSelection = true;
   m_scrollToSel = true;
   m_hasPending = false;
   m_sel = 0;
   m_query.clear();
   m_lastQuery.clear();
   // Retry types whose harvest failed earlier
   m_queue.clear();
   for (TypeInfo &type : m_types)
      type.queued = 0;
}

int SearchPalette::TypeIndex(const string &key, const string &name)
{
   const auto it = m_typeByKey.find(key);
   if (it != m_typeByKey.end())
      return it->second;
   TypeInfo info;
   info.name = name;
   info.lower = Lower(name);
   m_types.push_back(std::move(info));
   const int idx = static_cast<int>(m_types.size()) - 1;
   m_typeByKey[key] = idx;
   return idx;
}

// Rebuilds the object list (names and lowercase copies); the field catalogs are kept
void SearchPalette::BuildIndex()
{
   const PinTable *const table = m_editor.m_table;
   m_objects.clear();
   m_partType.clear();
   m_tableType = TypeIndex("table"s, "Table"s);
   m_matType = TypeIndex("material"s, "Material"s);

   m_objects.push_back({ PinKind::Table, string(), "Table"s, "table"s, m_tableType });
   for (const Material *const mat : table->m_materials)
      m_objects.push_back({ PinKind::Material, mat->m_name, mat->m_name, Lower(mat->m_name), m_matType });
   for (IEditable *const part : table->GetParts())
   {
      const ItemTypeEnum itemType = part->GetItemType();
      if (itemType == eItemTable)
         continue;
      int type;
      if (const auto cached = m_itemTypeIdx.find(static_cast<int>(itemType)); cached != m_itemTypeIdx.end())
         type = cached->second;
      else
      {
         type = TypeIndex("p"s + std::to_string(static_cast<int>(itemType)), string(LocalString(EditableRegistry::GetTypeNameStringID(itemType)).m_szbuffer));
         m_itemTypeIdx[static_cast<int>(itemType)] = type;
      }
      const string &name = part->GetName();
      m_partType[name] = type;
      m_objects.push_back({ PinKind::Part, name, name, Lower(name), type });
   }
   m_indexParts = table->GetParts().size();
   m_indexMats = table->m_materials.size();
   m_indexStale = false;
   m_dirty = true;
   RefreshCatalog();
   QueueMissingTypes();
}

// Learns the field list of each type from the registered definitions, in draw order
void SearchPalette::RefreshCatalog()
{
   struct Entry
   {
      int seq;
      string group;
   };
   vector<std::unordered_map<string, Entry>> found(m_types.size());
   for (const auto &[id, def] : m_editor.m_pinned.GetDefs())
   {
      int type = -1;
      switch (def.key.kind)
      {
      case PinKind::Table: type = m_tableType; break;
      case PinKind::Material: type = m_matType; break;
      case PinKind::Part:
      {
         const auto it = m_partType.find(def.key.name);
         if (it != m_partType.end())
            type = it->second;
         break;
      }
      }
      if (type < 0 || type >= static_cast<int>(found.size()))
         continue;
      const auto [it, added] = found[type].try_emplace(def.key.prop, Entry { def.seq, def.group });
      if (!added && def.seq < it->second.seq)
         it->second = Entry { def.seq, def.group };
   }
   for (size_t t = 0; t < found.size(); ++t)
   {
      if (found[t].empty())
         continue;
      vector<std::pair<int, const string *>> order;
      order.reserve(found[t].size());
      for (const auto &[label, entry] : found[t])
         order.emplace_back(entry.seq, &label);
      std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first < b.first : *a.second < *b.second; });
      vector<Field> &fields = m_types[t].fields;
      fields.clear();
      fields.reserve(order.size());
      for (const auto &o : order)
      {
         const string &group = found[t][*o.second].group;
         fields.push_back({ *o.second, group, Lower(*o.second), Lower(group) });
      }
   }
}

// Up to 3 objects per type without catalog get harvested (first, middle, last: conditional fields differ per object)
void SearchPalette::QueueMissingTypes()
{
   vector<vector<size_t>> perType(m_types.size());
   for (size_t i = 0; i < m_objects.size(); ++i)
      if (m_types[m_objects[i].type].queued == 0 && m_types[m_objects[i].type].fields.empty())
         perType[m_objects[i].type].push_back(i);
   for (size_t t = 0; t < perType.size(); ++t)
   {
      const vector<size_t> &list = perType[t];
      if (list.empty())
         continue;
      m_types[t].queued = 3;
      size_t picks[3] = { 0, list.size() / 2, list.size() - 1 };
      for (size_t k = 0; k < 3; ++k)
         if (k == 0 || picks[k] != picks[k - 1])
            m_queue.push_back({ m_objects[list[picks[k]]].kind, m_objects[list[picks[k]]].name });
   }
}

void SearchPalette::Harvest()
{
   int budget = kHarvestsPerFrame;
   bool any = false;
   while (budget-- > 0 && !m_queue.empty())
   {
      const QueueItem item = m_queue.front();
      m_queue.erase(m_queue.begin());
      m_editor.m_pinned.EnsureHarvested(item.kind, item.name);
      any = true;
   }
   if (any)
   {
      RefreshCatalog();
      m_dirty = true;
   }
}

void SearchPalette::MakeResult(Result &r) const
{
   const Object &o = m_objects[r.obj];
   const TypeInfo &type = m_types[o.type];
   if (r.isField)
   {
      if (r.field >= 0)
      {
         r.prop = type.fields[r.field].label;
         r.group = type.fields[r.field].group;
      }
      else
      {
         const PinnedPanel::Def *def = m_editor.m_pinned.FindDef(PinKey { o.kind, o.name, r.prop });
         r.group = def ? def->group : string();
      }
      r.label = o.display + " " + kSep + " " + r.prop;
      r.right = r.group.empty() ? type.name : r.group + " " + kDot + " " + type.name;
   }
   else
   {
      r.label = o.display + "  (select)";
      r.right = type.name;
   }
}

void SearchPalette::Recompute()
{
   m_results.clear();
   m_sel = 0;
   m_scrollToSel = true;

   vector<string> words;
   {
      const string lowered = Lower(m_lastQuery);
      size_t i = 0;
      while (i < lowered.size() && words.size() < kMaxWords)
      {
         while (i < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[i])))
            ++i;
         const size_t start = i;
         while (i < lowered.size() && !std::isspace(static_cast<unsigned char>(lowered[i])))
            ++i;
         if (i > start)
            words.push_back(lowered.substr(start, i - start));
      }
   }

   if (words.empty())
   {
      // Last activated results, dropping the ones whose object is gone
      for (size_t k = 0; k < m_recent.size();)
      {
         const Recent &rec = m_recent[k];
         int found = -1;
         for (size_t oi = 0; oi < m_objects.size(); ++oi)
            if (m_objects[oi].kind == rec.kind && m_objects[oi].name == rec.name)
            {
               found = static_cast<int>(oi);
               break;
            }
         if (found < 0)
         {
            m_recent.erase(m_recent.begin() + k);
            continue;
         }
         Result r { found, -1, 0, rec.isField, rec.prop, {}, {}, {} };
         if (rec.isField)
         {
            const vector<Field> &fields = m_types[m_objects[found].type].fields;
            for (size_t f = 0; f < fields.size(); ++f)
               if (fields[f].label == rec.prop)
               {
                  r.field = static_cast<int>(f);
                  break;
               }
         }
         MakeResult(r);
         m_results.push_back(std::move(r));
         ++k;
      }
      return;
   }

   // Current selection ranks first
   bool selIsTable = false;
   const Material *selMat = nullptr;
   const IEditable *selPart = nullptr;
   {
      const Selection &sel = m_editor.m_selection;
      switch (sel.GetType())
      {
      case Selection::S_NONE: selIsTable = true; break;
      case Selection::S_MATERIAL: selMat = sel.GetMaterial(); break;
      case Selection::S_EDITABLE:
      {
         const auto part = sel.GetPart();
         selPart = part ? part->GetEditable() : nullptr;
         break;
      }
      default: break;
      }
   }
   const string selName = selMat ? selMat->m_name : (selPart ? selPart->GetName() : string());

   const unsigned full = (1u << words.size()) - 1u;
   vector<Candidate> cands;
   for (size_t oi = 0; oi < m_objects.size(); ++oi)
   {
      const Object &o = m_objects[oi];
      const TypeInfo &type = m_types[o.type];
      // Words the object name or type already satisfy
      unsigned mask = 0;
      int base = 0;
      for (size_t w = 0; w < words.size(); ++w)
      {
         const int nameMatch = MatchWord(o.lower, words[w]);
         if (nameMatch != 0)
         {
            mask |= 1u << w;
            base += nameMatch == 2 ? 30 : 20;
         }
         else if (MatchWord(type.lower, words[w]) != 0)
         {
            mask |= 1u << w;
            base += 10;
         }
      }
      const bool isSel = (o.kind == PinKind::Table && selIsTable) || (o.kind == PinKind::Material && selMat != nullptr && o.name == selName)
         || (o.kind == PinKind::Part && selPart != nullptr && o.name == selName);
      base += (isSel ? 100000 : 0) - static_cast<int>(std::min<size_t>(o.name.size(), 100));

      if (mask == full)
      {
         cands.push_back({ static_cast<int>(oi), -1, base });
         continue;
      }
      // Remaining words must be found in a field label or group
      for (size_t f = 0; f < type.fields.size(); ++f)
      {
         const Field &field = type.fields[f];
         int total = base - static_cast<int>(std::min<size_t>(field.label.size(), 100) / 2);
         bool ok = true;
         for (size_t w = 0; w < words.size() && ok; ++w)
         {
            if (mask & (1u << w))
               continue;
            if (const int m = MatchWord(field.labelLower, words[w]); m != 0)
               total += m == 2 ? 40 : 30;
            else if (const int g = MatchWord(field.groupLower, words[w]); g != 0)
               total += g == 2 ? 20 : 15;
            else
               ok = false;
         }
         if (ok)
            cands.push_back({ static_cast<int>(oi), static_cast<int>(f), total });
      }
   }

   const auto better = [](const Candidate &a, const Candidate &b)
   {
      if (a.score != b.score)
         return a.score > b.score;
      return a.obj != b.obj ? a.obj < b.obj : a.field < b.field;
   };
   if (cands.size() > kMaxResults)
   {
      std::partial_sort(cands.begin(), cands.begin() + kMaxResults, cands.end(), better);
      cands.resize(kMaxResults);
   }
   else
      std::sort(cands.begin(), cands.end(), better);

   m_results.reserve(cands.size());
   for (const Candidate &c : cands)
   {
      Result r { c.obj, c.field, c.score, c.field >= 0, {}, {}, {}, {} };
      MakeResult(r);
      m_results.push_back(std::move(r));
   }
}

void SearchPalette::Remember(const Result &r)
{
   const Object &o = m_objects[r.obj];
   const string prop = r.isField ? r.prop : string();
   for (size_t k = 0; k < m_recent.size(); ++k)
      if (m_recent[k].kind == o.kind && m_recent[k].name == o.name && m_recent[k].isField == r.isField && m_recent[k].prop == prop)
      {
         m_recent.erase(m_recent.begin() + k);
         break;
      }
   m_recent.insert(m_recent.begin(), Recent { o.kind, o.name, prop, r.isField });
   if (m_recent.size() > kMaxRecent)
      m_recent.resize(kMaxRecent);
}

void SearchPalette::Activate(const Result &r, bool pin)
{
   if (r.obj < 0 || r.obj >= static_cast<int>(m_objects.size()))
      return;
   const Object &o = m_objects[r.obj];
   const PinKey key { o.kind, o.name, r.isField ? r.prop : string() };

   if (pin)
   {
      if (!r.isField)
         return;
      m_editor.m_pinned.Pin(key);
      m_editor.m_liveUI.PushNotification("Pinned " + r.label, 2000);
      Remember(r);
      m_focusInput = true;
      return;
   }

   Close();
   PinTable *const table = m_editor.m_table;
   bool found = false;
   switch (o.kind)
   {
   case PinKind::Table:
      m_editor.ClearSelection();
      found = true;
      break;
   case PinKind::Material:
      for (Material *const mat : table->m_materials)
         if (mat->m_name == o.name)
         {
            m_editor.SetSelection(Selection(mat));
            found = true;
            break;
         }
      break;
   case PinKind::Part:
      for (IEditable *const part : table->GetParts())
         if (part->GetName() == o.name)
         {
            const auto it = m_editor.m_editableMap.find(part);
            if (it != m_editor.m_editableMap.end())
            {
               m_editor.SetSelection(Selection(it->second));
               found = true;
            }
            break;
         }
      break;
   }
   if (!found)
   {
      m_editor.m_liveUI.PushNotification(o.display + " no longer exists", 2000);
      return;
   }
   if (r.isField)
   {
      m_editor.m_showProperties = true; // A workspace may hide the panel, which would never consume the request
      PropertyPane::RequestFocus(key);
   }
   Remember(r);
}

bool SearchPalette::ConsumedEscape()
{
   if (!m_escPending)
      return false;
   if (!ImGui::IsKeyDown(ImGuiKey_Escape))
      m_escPending = false; // Released: this is the frame of the release, still consumed
   return true;
}

void SearchPalette::Render()
{
   if (!m_open)
      return;

   // Index and harvests run before the palette window: harvesting opens its own hidden windows
   const PinTable *const table = m_editor.m_table;
   if (m_indexStale || table->GetParts().size() != m_indexParts || table->m_materials.size() != m_indexMats)
      BuildIndex();
   if (m_harvestSelection)
   {
      m_harvestSelection = false;
      const Selection &sel = m_editor.m_selection;
      if (sel.GetType() == Selection::S_MATERIAL && sel.GetMaterial() != nullptr)
         m_queue.insert(m_queue.begin(), QueueItem { PinKind::Material, sel.GetMaterial()->m_name });
      else if (sel.GetType() == Selection::S_EDITABLE && sel.GetPart() != nullptr)
         m_queue.insert(m_queue.begin(), QueueItem { PinKind::Part, sel.GetPart()->GetEditable()->GetName() });
      else if (sel.GetType() == Selection::S_NONE)
         m_queue.insert(m_queue.begin(), QueueItem { PinKind::Table, string() });
   }
   const bool harvested = !m_queue.empty();
   Harvest();

   if (m_dirty || m_query != m_lastQuery)
   {
      m_lastQuery = m_query;
      m_dirty = false;
      Recompute();
   }

   const ImGuiViewport *const vp = ImGui::GetMainViewport();
   const ImGuiStyle &style = ImGui::GetStyle();
   const float width = 560.f * m_editor.m_liveUI.GetDPI();
   const float innerWidth = width - 2.f * style.WindowPadding.x;
   const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
   const float chrome = 2.f * style.WindowPadding.y + ImGui::GetFrameHeight() + 4.f * style.ItemSpacing.y + ImGui::GetTextLineHeight() + 1.f;
   const float maxList = std::max(3.f * rowHeight, vp->Size.y * 0.6f - chrome);
   size_t count = m_results.size();
   const float listHeight = count == 0 ? ImGui::GetTextLineHeight() : std::min(static_cast<float>(count) * rowHeight, maxList);

   ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.2f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
   ImGui::SetNextWindowSize(ImVec2(width, chrome + listHeight), ImGuiCond_Always);
   ImGui::SetNextWindowBgAlpha(0.96f);
   if (m_firstFrame || harvested)
      ImGui::SetNextWindowFocus();

   bool close = false;
   bool hasActivation = false;
   size_t activateIdx = 0;
   bool activatePin = false;

   constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
      | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
   if (ImGui::Begin("##SearchPalette", nullptr, flags))
   {
      if (!m_firstFrame && !harvested && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
         close = true;
      m_firstFrame = false;

      if (m_focusInput)
      {
         ImGui::SetKeyboardFocusHere();
         m_focusInput = false;
      }
      ImGui::SetNextItemWidth(innerWidth);
      ImGui::InputTextWithHint("##query", "Search: object, field, type  (e.g. ramp refraction, l31 bulb depth)", &m_query);
      if (m_query != m_lastQuery)
      {
         // Recompute now so that Enter acts on the list matching the text
         m_lastQuery = m_query;
         m_dirty = false;
         Recompute();
         count = m_results.size();
      }

      if (count > 0)
      {
         const int last = static_cast<int>(count) - 1;
         if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && m_sel < last)
         {
            ++m_sel;
            m_scrollToSel = true;
         }
         if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && m_sel > 0)
         {
            --m_sel;
            m_scrollToSel = true;
         }
         if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
         {
            hasActivation = true;
            activateIdx = static_cast<size_t>(m_sel);
            activatePin = ImGui::GetIO().KeyCtrl;
         }
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
         close = true;
         m_escPending = true;
      }

      ImGui::Separator();
      if (count == 0)
         ImGui::TextDisabled("%s", m_lastQuery.empty() ? "Type to search" : "No matches");
      else if (ImGui::BeginChild("##results", ImVec2(innerWidth, listHeight), ImGuiChildFlags_None, ImGuiWindowFlags_None))
      {
         const ImGuiIO &io = ImGui::GetIO();
         const bool mouseMoved = io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f;
         ImDrawList *const draw = ImGui::GetWindowDrawList();
         const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
         for (size_t i = 0; i < count; ++i)
         {
            const Result &r = m_results[i];
            const bool selected = static_cast<int>(i) == m_sel;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(r.label.c_str(), selected))
            {
               hasActivation = true;
               activateIdx = i;
               activatePin = false;
            }
            if (selected && m_scrollToSel)
               ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            if (ImGui::IsItemHovered() && mouseMoved)
               m_sel = static_cast<int>(i);
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float rightWidth = ImGui::CalcTextSize(r.right.c_str()).x;
            // Skip the dim column when it would overlap the label
            if (ImGui::CalcTextSize(r.label.c_str()).x + rightWidth + 3.f * style.ItemSpacing.x < itemMax.x - itemMin.x)
               draw->AddText(ImVec2(itemMax.x - rightWidth - style.ItemSpacing.x, itemMin.y), dim, r.right.c_str());
            ImGui::PopID();
         }
         m_scrollToSel = false;
      }
      if (count != 0)
         ImGui::EndChild(); // Always paired with BeginChild, whatever it returned
      ImGui::TextDisabled("Enter go to field %s Cmd/Ctrl+Enter pin %s Esc close", kDot, kDot);
   }
   ImGui::End();

   if (close)
      Close();
   else if (hasActivation && activateIdx < m_results.size())
   {
      const Result r = m_results[activateIdx]; // Copy: activation may change the palette state
      Activate(r, activatePin);
   }
}

}
