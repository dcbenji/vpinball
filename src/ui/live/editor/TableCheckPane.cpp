// license:GPLv3+

#include "core/stdafx.h"
#include "TableCheckPane.h"
#include "EditorTheme.h"
#include "ViewportPane.h"
#include "OutlinerPanel.h"
#include "ui/live/EditorUI.h"
#include "ui/live/IconsLucide.h"
#include "ui/live/ImGuiHelpers.h"
#include "ui/live/LiveUI.h"

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

TableCheckPane::Signature TableCheckPane::CurrentSignature() const
{
   const PinTable &table = *m_editor.m_table;
   Signature sig;
   sig.table = &table;
   sig.undoId = m_editor.m_undo.TopRecordId();
   sig.parts = table.GetParts().size();
   sig.images = table.GetImageList().size();
   sig.materials = table.GetMaterialList().size();
   sig.sounds = table.m_vsound.size();
   sig.collections = table.GetCollections().size();
   sig.dirty = static_cast<int>(table.GetDirtyState());
   sig.script = table.GetScriptRevision();
   return sig;
}

void TableCheckPane::Scan()
{
   const PinTable &table = *m_editor.m_table;
   m_issues = table.CheckTable(LiveOptions);
   // Objects made by the player session (implicit playfield, runtime balls,...) are not part of the authored table
   std::erase_if(m_issues, [&table](const TableIssue &issue) { return issue.part != nullptr && table.IsPlayerSessionObject(issue.part); });
   std::ranges::stable_sort(m_issues, {}, [](const TableIssue &issue) { return static_cast<int>(issue.check); });
   m_signature = CurrentSignature();
   m_scanned = true;
   m_pending = false;
}

void TableCheckPane::Select(const TableIssue &issue, const bool frame)
{
   EditorUI &editor = m_editor;
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

   // Checked again when the table changed, but not in the middle of a drag (the undo record of a drag is made when it starts)
   if (!m_scanned || !(CurrentSignature() == m_signature))
      m_pending = true;
   if (m_pending && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
      Scan();

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
   for (const TableIssue &issue : m_issues)
      ++counts[static_cast<int>(issue.severity)];

   // Header: editor type, check again, severity filters with their counts
   editor.RenderAreaTypeButton(leafId);
   ImGui::SameLine();
   if (IconButtonInk(ICON_LC_REFRESH_CW, "##TableCheckRescan"))
      Scan();
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

   // The issues, one collapsible group per check. Only the rows in view are drawn (a table may have hundreds of issues)
   ImGui::BeginChild("##TableCheckList", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
   const float rowH = ImGui::GetTextLineHeightWithSpacing() + 2.f * dpi;
   const float iconW = 20.f * dpi;
   bool anyShown = false;
   vector<const TableIssue *> rows;
   for (size_t first = 0; first < m_issues.size();)
   {
      const TableCheckId check = m_issues[first].check;
      size_t end = first;
      rows.clear();
      for (; end < m_issues.size() && m_issues[end].check == check; ++end)
         if (m_show[static_cast<int>(m_issues[end].severity)])
            rows.push_back(&m_issues[end]);
      first = end;
      if (rows.empty())
         continue;
      anyShown = true;
      const string header = string(TableCheckTitle(check)) + "  (" + std::to_string(rows.size()) + ")###TableCheckGroup" + std::to_string(static_cast<int>(check));
      // A group starts open when it has an error, or warnings and only a few rows: a long list, or infos alone, is a summary row to expand
      const bool hasError = std::ranges::any_of(rows, [](const TableIssue *issue) { return issue->severity == TableIssueSeverity::Error; });
      const bool hasWarning = std::ranges::any_of(rows, [](const TableIssue *issue) { return issue->severity == TableIssueSeverity::Warning; });
      const bool openByDefault = hasError || (hasWarning && rows.size() <= 10);
      if (!ImGui::TreeNodeEx(header.c_str(), (openByDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0) | ImGuiTreeNodeFlags_SpanAvailWidth))
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
            const bool clicked = ImGui::Selectable("##row", selectedRow, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.f, rowH));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList *const drawList = ImGui::GetWindowDrawList();
            const float textY = rowPos.y + 0.5f * (rowH - ImGui::GetTextLineHeight());
            drawList->AddText(ImVec2(rowPos.x + 4.f * dpi, textY), SeverityColor(issue.severity), SeverityIcon(issue.severity));
            drawList->PushClipRect(ImVec2(rowPos.x + iconW, rowPos.y), ImVec2(rowPos.x + rowW, rowPos.y + rowH), true);
            drawList->AddText(ImVec2(rowPos.x + iconW, textY), issue.part != nullptr ? EditorTheme::Text : EditorTheme::TextDim, issue.message.c_str());
            drawList->PopClipRect();
            if (hovered && ImGui::GetDragDropPayload() == nullptr)
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
      ImGui::TextUnformatted(m_issues.empty() ? "No issue found" : "All issues are hidden by the filters above");
      ImGui::PopStyleColor();
   }
   ImGui::EndChild();

   ImGui::PopID();
   ImGui::End();
   ImGui::PopStyleVar(3);
}

}
