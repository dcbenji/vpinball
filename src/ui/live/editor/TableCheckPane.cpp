// license:GPLv3+

#include "core/stdafx.h"
#include "TableCheckPane.h"
#include "EditorTheme.h"
#include "ViewportPane.h"
#include "OutlinerPanel.h"
#include "ops/TableCheckFixOp.h"
#include "ui/live/EditorUI.h"
#include "ui/live/IconsLucide.h"
#include "ui/live/ImGuiHelpers.h"
#include "ui/live/LiveUI.h"

#include <chrono>

namespace VPX::EditorUI
{

namespace
{

ImU32 SeverityColor(const TableIssueSeverity s)
{
   return s == TableIssueSeverity::Error ? EditorTheme::Rgb(224, 84, 84) : s == TableIssueSeverity::Warning ? EditorTheme::Rgb(232, 172, 52) : EditorTheme::Rgb(116, 164, 222);
}

const char *SeverityIcon(const TableIssueSeverity s) { return s == TableIssueSeverity::Error ? ICON_LC_X : s == TableIssueSeverity::Warning ? ICON_LC_TRIANGLE_ALERT : ICON_LC_CIRCLE_DOT; }

const char *SeverityName(const TableIssueSeverity s) { return s == TableIssueSeverity::Error ? "Errors" : s == TableIssueSeverity::Warning ? "Warnings" : "Infos"; }

}

TableCheckStatus::Signature TableCheckStatus::CurrentSignature(const EditorUI &editor)
{
   const PinTable &table = *editor.m_table;
   Signature sig;
   sig.table = &table;
   sig.undoId = editor.m_undo.TopRecordId();
   sig.parts = table.GetParts().size();
   sig.images = table.GetImageList().size();
   sig.materials = table.GetMaterialList().size();
   sig.sounds = table.m_vsound.size();
   sig.collections = table.GetCollections().size();
   sig.dirty = static_cast<int>(table.GetDirtyState());
   sig.script = table.GetScriptRevision();
   return sig;
}

vector<TableIssue> TableCheckPane::LiveCheck(const PinTable &table, const bool log, const bool script)
{
   const auto start = std::chrono::steady_clock::now();
   TableCheckOptions options = LiveOptions;
   options.script = script;
   vector<TableIssue> issues = table.CheckTable(options);
   // Objects made by the player session (implicit playfield, runtime balls,...) are not part of the authored table
   std::erase_if(issues, [&table](const TableIssue &issue) { return issue.part != nullptr && table.IsPlayerSessionObject(issue.part); });
   std::ranges::stable_sort(issues, {}, [](const TableIssue &issue) { return static_cast<int>(issue.check); });
   if (log)
      PLOGI << "Table check: " << issues.size() << " issues" << (script ? "" : " (without the script checks)") << ", checked in " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << " ms";
   return issues;
}

void TableCheckPane::Select(const TableIssue &issue, const bool frame)
{
   EditorUI &editor = m_editor;
   if (issue.image != nullptr)
   {
      if (std::ranges::find(editor.m_table->GetImageList(), issue.image) != editor.m_table->GetImageList().end())
         editor.SetSelection(Selection(issue.image));
      return;
   }
   if (issue.material != nullptr)
   {
      if (std::ranges::find(editor.m_table->GetMaterialList(), issue.material) != editor.m_table->GetMaterialList().end())
         editor.SetSelection(Selection(issue.material));
      return;
   }
   if (issue.part == nullptr)
      return;
   // The issue may be older than the table: only a part that is still in it is selected
   const auto it = editor.m_editableMap.find(issue.part);
   if (it == editor.m_editableMap.end() || !editor.m_table->HasPart(issue.part))
      return;
   editor.SetSelection(Selection(it->second));
   OutlinerPanel::m_anchor = it->second;
   if (frame)
      if (ViewportPane *const vp = editor.GetTargetViewport(); vp != nullptr)
         vp->FrameSelection();
}

void TableCheckStatus::SetLoaded(const PinTable *table, const TableCheckSummary &summary)
{
   s_loadedTable = table;
   s_loadedSummary = summary;
}

void TableCheckStatus::Rescan(const EditorUI &editor, const bool log, const bool script)
{
   m_issues = TableCheckPane::LiveCheck(*editor.m_table, log, script);
   m_summary = TableCheckSummary::Of(m_issues);
   m_haveIssues = script; // Without the script checks, a pane asking for the issues checks the table again
   m_pending = false;
   m_signature = CurrentSignature(editor);
}

void TableCheckStatus::Update(const EditorUI &editor)
{
   const PinTable *const table = editor.m_table;
   const PinTable *const base = table->m_liveBaseTable ? table->m_liveBaseTable : table;
   if (m_table != base)
   {
      // A new table: the counts of the loading thread, or a first check
      m_table = base;
      if (s_loadedTable == base)
      {
         m_summary = s_loadedSummary;
         m_issues.clear();
         m_haveIssues = false;
         m_pending = false;
         m_signature = CurrentSignature(editor);
         s_loadedTable = nullptr;
      }
      else
         Rescan(editor, true, editor.m_table->m_script_text.size() <= TableCheckPane::MaxStartupScriptSize);
      return;
   }
   // Checked again when the table changed, but not in the middle of a drag (the undo record of a drag is made when it starts), not
   // while the script is being typed (once it paused for a while: a script revision per key), and not while a play test runs (the
   // balls it makes and destroys change the parts). A big script is checked without the script checks, as at start-up
   if (const Signature sig = CurrentSignature(editor); !(sig == m_signature))
   {
      if (sig.script != m_signature.script && sig.script != m_lastScriptRevision)
      {
         m_lastScriptRevision = sig.script;
         m_lastScriptEdit = ImGui::GetTime();
      }
      m_pending = true;
   }
   if (m_pending && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::GetTime() - m_lastScriptEdit >= ScriptEditPause && !editor.m_player->IsPlaying(false))
      Rescan(editor, false, editor.m_table->m_script_text.size() <= TableCheckPane::MaxStartupScriptSize);
}

const vector<TableIssue> &TableCheckStatus::GetIssues(const EditorUI &editor)
{
   if (!m_haveIssues)
      Rescan(editor);
   return m_issues;
}

string TableCheckStatus::Text() const
{
   string text;
   static constexpr const char *names[2][2] = { { " error", " errors" }, { " warning", " warnings" } };
   for (int s = 0; s < 2; ++s)
      if (const int n = m_summary.counts[s]; n > 0)
         text += (text.empty() ? "" : ", ") + std::to_string(n) + names[s][n == 1 ? 0 : 1];
   return text.empty() ? "No errors or warnings"s : text;
}

ImU32 TableCheckStatus::Color() const
{
   return m_summary.counts[0] > 0 ? SeverityColor(TableIssueSeverity::Error) : m_summary.counts[1] > 0 ? SeverityColor(TableIssueSeverity::Warning) : EditorTheme::TextDim;
}

nlohmann::json TableCheckPane::SaveSetup() const { return { { "show", { m_show[0], m_show[1], m_show[2] } } }; }

void TableCheckPane::LoadSetup(const nlohmann::json &j)
{
   if (!j.is_object() || !j.contains("show") || !j["show"].is_array() || j["show"].size() != 3)
      return;
   for (int i = 0; i < 3; i++)
      m_show[i] = j["show"][i].is_boolean() ? j["show"][i].get<bool>() : true;
}

void TableCheckPane::AppendStatusHints(vector<StatusHint> &hints) const
{
   hints.push_back({ ICON_LC_MOUSE_LEFT, "Select Part" });
   hints.push_back({ "Double " ICON_LC_MOUSE_LEFT, "Frame Part" });
}

void TableCheckPane::RenderPane(const AreaRect &rect, const int leafId)
{
   EditorUI &editor = m_editor;
   if (editor.m_table->IsLocked())
      return;

   TableCheckStatus &status = editor.m_tableCheckStatus;
   status.Update(editor);
   const vector<TableIssue> &issues = status.GetIssues(editor);

   const float dpi = editor.m_liveUI.GetDPI();
   char name[32];
   snprintf(name, sizeof(name), "##Pane%d", leafId);
   ImGui::SetNextWindowPos(ImVec2(rect.x0, rect.y0));
   ImGui::SetNextWindowSize(ImVec2(rect.Width(), rect.Height()));
   constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
      | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * dpi, 4.0f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   ImGui::Begin(name, nullptr, window_flags);
   ImGui::PushID(leafId);

   int counts[3] = {};
   for (const TableIssue &issue : issues)
      ++counts[static_cast<int>(issue.severity)];

   // Header: editor type, check again, severity filters with their counts
   editor.RenderAreaTypeButton(leafId);
   ImGui::SameLine();
   if (IconButtonInk(ICON_LC_REFRESH_CW, "##TableCheckRescan"))
      status.Rescan(editor);
   ImGui::SetItemTooltip("Check the table again (it is also checked again when it changes)");
   for (int s = 0; s < 3; ++s)
   {
      ImGui::SameLine();
      const TableIssueSeverity sev = static_cast<TableIssueSeverity>(s);
      const string label = string(SeverityIcon(sev)) + ' ' + std::to_string(counts[s]) + "##TableCheckSeverity" + std::to_string(s);
      ImGui::PushStyleColor(ImGuiCol_Text, m_show[s] ? SeverityColor(sev) : EditorTheme::TextFaint);
      ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(m_show[s] ? EditorTheme::Raised : EditorTheme::Inactive));
      if (ImGui::Button(label.c_str()))
         m_show[s] = !m_show[s];
      ImGui::PopStyleColor(2);
      ImGui::SetItemTooltip("%s: %d. Click to %s them", SeverityName(sev), counts[s], m_show[s] ? "hide" : "show");
   }

   // Fixes run on the edited table with undo: not on the live copy of an inspected play session (no undo, only some changes are saved back)
   // Fixes also wait for a list as recent as the table: after a change (e.g. a script edit, checked again once the typing paused)
   const bool canFix = !editor.IsInspectMode() && !status.IsPending();
   const char *const noFixTooltip = editor.IsInspectMode() ? "Fixes are only available when editing the table, not while inspecting a play session"
                                                           : "The table changed: fixes are available once it is checked again";
   vector<TableIssue> fixRequest; // Applied after the list is drawn (the fix checks the table again, which rebuilds the issues)
   const auto fixButton = [&](const char *label, const char *id, const float right, const vector<const TableIssue *> &fixes, const string &tooltip)
   {
      const float width = ImGui::CalcTextSize(label).x + 2.f * ImGui::GetStyle().FramePadding.x;
      ImGui::SameLine(right - width);
      ImGui::BeginDisabled(!canFix);
      if (ImGui::SmallButton((string(label) + id).c_str()))
         for (const TableIssue *const issue : fixes)
            fixRequest.push_back(*issue);
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("%s", canFix ? tooltip.c_str() : noFixTooltip);
      return width;
   };

   // The issues, one collapsible group per check. Only the rows in view are drawn (a table may have hundreds of issues)
   ImGui::BeginChild("##TableCheckList", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
   const float rowH = ImGui::GetTextLineHeightWithSpacing() + 2.f * dpi;
   const float iconW = 20.f * dpi;
   bool anyShown = false;
   vector<const TableIssue *> rows;
   for (size_t first = 0; first < issues.size();)
   {
      const TableCheckId check = issues[first].check;
      size_t end = first;
      rows.clear();
      for (; end < issues.size() && issues[end].check == check; ++end)
         if (m_show[static_cast<int>(issues[end].severity)])
            rows.push_back(&issues[end]);
      first = end;
      if (rows.empty())
         continue;
      anyShown = true;
      const string header = string(TableCheckTitle(check)) + "  (" + std::to_string(rows.size()) + ")###TableCheckGroup" + std::to_string(static_cast<int>(check));
      // A group starts open when it has an error, or warnings and only a few rows: a long list, or infos alone, is a summary row to expand
      const bool hasError = std::ranges::any_of(rows, [](const TableIssue *issue) { return issue->severity == TableIssueSeverity::Error; });
      const bool hasWarning = std::ranges::any_of(rows, [](const TableIssue *issue) { return issue->severity == TableIssueSeverity::Warning; });
      const bool openByDefault = hasError || (hasWarning && rows.size() <= 10);
      const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
      const bool open = ImGui::TreeNodeEx(header.c_str(), (openByDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0) | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap);
      // Fix All: the issues of the group with the same fix, as one undo step
      vector<const TableIssue *> groupFixes;
      for (const TableIssue *const issue : rows)
         if (issue->fix != TableFix::None && (groupFixes.empty() || issue->fix == groupFixes.front()->fix))
            groupFixes.push_back(issue);
      if (groupFixes.size() > 1)
      {
         ImGui::PushID(static_cast<int>(check));
         fixButton("Fix All", "##TableCheckFixAll", right, groupFixes, string(TableCheckFixOp::Label(groupFixes.front()->fix)) + ": the " + std::to_string(groupFixes.size()) + " issues of this group, as one undo step");
         ImGui::PopID();
      }
      if (!open)
         continue;
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(rows.size()), rowH + ImGui::GetStyle().ItemSpacing.y);
      while (clipper.Step())
         for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
         {
            const TableIssue &issue = *rows[index];
            ImGui::PushID(index);
            const ImVec2 rowPos = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            const auto part = issue.part != nullptr ? editor.m_editableMap.find(issue.part) : editor.m_editableMap.end();
            const bool selectedRow = part != editor.m_editableMap.end() && editor.IsPartSelected(part->second);
            const bool clicked = ImGui::Selectable("##row", selectedRow, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.f, rowH));
            const bool hovered = ImGui::IsItemHovered();
            float textRight = rowPos.x + rowW;
            bool fixHovered = false; // The button shows its own tooltip, not the row's
            if (issue.fix != TableFix::None)
            {
               textRight -= fixButton("Fix", "##TableCheckFix", ImGui::GetCursorPosX() + rowW, { &issue }, TableCheckFixOp::Label(issue.fix)) + 4.f * dpi;
               fixHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            }
            ImDrawList *const drawList = ImGui::GetWindowDrawList();
            const float textY = rowPos.y + 0.5f * (rowH - ImGui::GetTextLineHeight());
            drawList->AddText(ImVec2(rowPos.x + 4.f * dpi, textY), SeverityColor(issue.severity), SeverityIcon(issue.severity));
            drawList->PushClipRect(ImVec2(rowPos.x + iconW, rowPos.y), ImVec2(textRight, rowPos.y + rowH), true);
            drawList->AddText(ImVec2(rowPos.x + iconW, textY), issue.part != nullptr ? EditorTheme::Text : EditorTheme::TextDim, issue.message.c_str());
            drawList->PopClipRect();
            if (hovered && !fixHovered && ImGui::GetDragDropPayload() == nullptr)
               ImGui::SetTooltip("%s", issue.message.c_str());
            if (clicked)
               Select(issue, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            ImGui::PopID();
         }
      ImGui::TreePop();
   }
   if (!anyShown)
   {
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
      ImGui::TextUnformatted(issues.empty() ? "No issue found" : "All issues are hidden by the filters above");
      ImGui::PopStyleColor();
   }
   ImGui::EndChild();

   ImGui::PopID();
   ImGui::End();
   ImGui::PopStyleVar(3);

   if (!fixRequest.empty())
   {
      vector<const TableIssue *> fixes;
      for (const TableIssue &issue : fixRequest)
         fixes.push_back(&issue);
      TableCheckFixOp::Fix(editor, fixes);
      status.Rescan(editor);
   }
}

}
