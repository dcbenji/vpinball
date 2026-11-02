#include "core/stdafx.h"
#include "PropertyPane.h"

#include "imgui/imgui_stdlib.h"
#include "parts/primitive.h"
#include "ui/live/LiveUI.h"

namespace VPX::EditorUI
{

PropertyPane::PropertyPane(PinTable* table)
   : m_table(table)
{
}

PropertyPane::~PropertyPane() { }

void PropertyPane::ArmBulbBroadcast(const IEditable* part)
{
   m_broadcast.clear();
   m_broadcastApplied = false;
   const auto isTraced = [](const IEditable* p) { return p->GetItemType() == eItemPrimitive && static_cast<const Primitive*>(p)->IsTracedInsert(); };
   if (!s_applyBulbToAll || part == nullptr || !isTraced(part))
      return;
   for (IEditable* other : m_table->GetParts())
      if (other != part && isTraced(other))
         m_broadcast.push_back(other);
}

void PropertyPane::SetShowStartup(bool showStartup)
{
   assert(!showStartup || m_table->m_liveBaseTable);
   m_showStartup = showStartup;
}

void PropertyPane::FlushRow()
{
   if (m_rowBottom <= 0.f)
      return;
   const float bottom = m_rowBottom;
   m_rowBottom = 0.f;
   const ImVec2 cursor = ImGui::GetCursorScreenPos();
   if (cursor.y >= bottom)
      return;
   // Submit an empty item so that the window bounds follow the moved cursor
   ImGui::SetCursorScreenPos(ImVec2(cursor.x, bottom - ImGui::GetStyle().ItemSpacing.y));
   ImGui::Dummy(ImVec2(0.f, 0.f));
}

void PropertyPane::PropertyLabel(const string& label, const char* tooltip)
{
   FlushRow();
   m_rowFocusable = false;
   const ImVec2 pos = ImGui::GetCursorScreenPos();
   m_rowMin = pos;
   const ImGuiStyle& style = ImGui::GetStyle();
   const float xSpacing = style.ItemSpacing.x;
   const float xWidth = ImGui::GetContentRegionAvail().x * 0.5f - m_syncWidth;
   // The panel keeps room for the hover pin toggle at the left of the label column
   const float pinWidth = m_pinMode == PinMode::Panel ? ImGui::GetFontSize() + xSpacing * 0.5f : 0.f;
   const float maxWidth = max(0.f, xWidth - xSpacing - pinWidth);

   const char* const text = label.c_str();
   const char* const textEnd = text + label.size();
   const auto drawLine = [&](const char* begin, const char* end, float y)
   {
      const float width = ImGui::CalcTextSize(begin, end).x;
      ImGui::SetCursorScreenPos(ImVec2(pos.x + max(pinWidth, xWidth - width - xSpacing), y));
      ImGui::TextUnformatted(begin, end);
      if (tooltip != nullptr)
         ImGui::SetItemTooltip("%s\n%s", text, tooltip);
      else
         ImGui::SetItemTooltip("%s", text);
   };
   const auto ellipsized = [&](const char* begin)
   {
      // Longest prefix of [begin, textEnd) that fits with an ellipsis
      const float dotsWidth = ImGui::CalcTextSize("...").x;
      const char* end = textEnd;
      while (end > begin + 1 && ImGui::CalcTextSize(begin, end).x + dotsWidth > maxWidth)
         end--;
      return trim_string(string(begin, end)) + "...";
   };

   if (ImGui::CalcTextSize(text, textEnd).x <= maxWidth)
   {
      ImGui::SetCursorScreenPos(ImVec2(pos.x + max(pinWidth, xWidth - ImGui::CalcTextSize(text, textEnd).x - xSpacing), pos.y + style.FramePadding.y));
      ImGui::TextUnformatted(text, textEnd);
      if (tooltip != nullptr)
         ImGui::SetItemTooltip("%s", tooltip);
   }
   else
   {
      // Wrap on two lines, at the space giving the most balanced lines; the second line gets an ellipsis if it still does not fit
      const char* split = nullptr;
      float best = FLT_MAX;
      for (const char* c = text; c < textEnd; c++)
      {
         if (*c != ' ')
            continue;
         const float w1 = ImGui::CalcTextSize(text, c).x;
         if (w1 > maxWidth)
            break;
         const float w = max(w1, ImGui::CalcTextSize(c + 1, textEnd).x);
         if (w < best)
         {
            best = w;
            split = c;
         }
      }
      if (split == nullptr)
      {
         const string line = ellipsized(text);
         drawLine(line.c_str(), line.c_str() + line.size(), pos.y + style.FramePadding.y);
      }
      else
      {
         const float lineHeight = ImGui::GetFontSize();
         drawLine(text, split, pos.y);
         if (ImGui::CalcTextSize(split + 1, textEnd).x <= maxWidth)
            drawLine(split + 1, textEnd, pos.y + lineHeight);
         else
         {
            const string line = ellipsized(split + 1);
            drawLine(line.c_str(), line.c_str() + line.size(), pos.y + lineHeight);
         }
         m_rowBottom = max(pos.y + 2.f * lineHeight, pos.y + ImGui::GetFrameHeight()) + style.ItemSpacing.y;
      }
   }

   ImGui::SetCursorScreenPos(ImVec2(pos.x + xWidth, pos.y));
   ImGui::SetNextItemWidth(xWidth);
   m_syncPos = ImVec2(pos.x + xWidth + xWidth, pos.y);
}

void PropertyPane::Header(const string& typeName, const std::function<string()>& getName, const std::function<void(const string&)>& setName)
{
   ImGui::NewLine();
   LiveUI::CenteredText(IsMultiEdit() ? typeName + " x" + std::to_string(GetEditCount()) : typeName);
   m_modifyFieldId++;
   // Do not edit name of live objects as it would break the script, nor of multi selections (no bulk renaming)
   ImGui::BeginDisabled(m_table->m_liveBaseTable || IsMultiEdit());
   PropertyLabel("Name"s);
   const string current = getName();
   string name = current;
   // Apply on Enter only: renaming on each keystroke would make each intermediate name unique, corrupting the name being typed
   if (ImGui::InputText("##Name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && name != current)
   {
      setName(name);
      if (getName() != current) // The setter may reject or adjust the name
         m_modified = m_modifyFieldId; // Makes the rename undoable and marks the table as modified
   }
   ImGui::EndDisabled();
   ImGui::Separator();
   if (m_afterHeader)
      m_afterHeader(*this);
}

void PropertyPane::EditableHeader(const string& typeName, IEditable* editable)
{
   Header(typeName, [editable]() { return editable->GetName(); }, [editable](const string& v) { editable->SetName(v); });
}

void PropertyPane::Separator(const string& label)
{
   m_pinGroup = label;
   if (m_skipRest)
      return;
   FlushRow();
   ImGui::TextUnformatted(label.c_str());
   // Underline the text
   const ImVec2 headerMin = ImGui::GetItemRectMin();
   const ImVec2 headerMax = ImGui::GetItemRectMax();
   ImDrawList* drawList = ImGui::GetWindowDrawList();
   const ImVec2 lineStart(headerMin.x, headerMax.y);
   const ImVec2 lineEnd(headerMax.x, headerMax.y);
   drawList->AddLine(lineStart, lineEnd, ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
}

bool PropertyPane::BeginSection(const string& name)
{
   assert(!m_inSection);
   if (m_skipRest)
      return false;
   FlushRow();
   if (s_focusPending && m_pinMode == PinMode::Panel)
   {
      if (ImGui::GetFrameCount() - s_focusRequestFrame > 10)
         s_focusPending = false; // Not found (field hidden for this object): give up
      else
         ImGui::SetNextItemOpen(true);
   }
   const bool opened = ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

   // Add a white line above
   const ImVec2 headerMin = ImGui::GetItemRectMin();
   const ImVec2 headerMax = ImGui::GetItemRectMax();
   ImDrawList* drawList = ImGui::GetWindowDrawList();
   const ImVec2 lineStart(headerMin.x, headerMin.y);
   const ImVec2 lineEnd(headerMax.x, headerMin.y);
   drawList->AddLine(lineStart, lineEnd, ImGui::GetColorU32(ImGuiCol_Text), 1.0f);

   if (!opened)
      return false;

   m_inSection = true;
   m_pinGroup = name;
   m_sectionHasSync = m_table->m_liveBaseTable;
   // for some reason I don't really get, the computed size is too large
   //m_syncWidth = m_sectionHasSync ? (ImGui::CalcTextSize(ICON_SAVE).x + ImGui::GetStyle().ItemSpacing.x) : 0.f;
   m_syncWidth = m_sectionHasSync ? (ImGui::CalcTextSize(ICON_SAVE).x * 0.5f) : 0.f;
   return true;
}

void PropertyPane::EndSection()
{
   assert(m_inSection);
   FlushRow();
   m_inSection = false;
}

bool PropertyPane::LookSliderRange(const string& label, Unit displayUnit, float& vMin, float& vMax)
{
   // Fields whose useful range is known (display units). Percentages are 0..100, scales up to 200 %
   struct Range
   {
      const char* label;
      float vMin, vMax;
   };
   static constexpr Range ranges[] = {
      { "Roughness", 0.f, 1.f }, { "Metallic", 0.f, 1.f }, { "Occlusion", 0.f, 1.f }, { "Clear Coat", 0.f, 1.f }, { "Clear Coat Roughness", 0.f, 1.f },
      { "Edge Brightness", 0.f, 1.f }, { "Wrap Lighting", 0.f, 1.f }, { "Glossy Image Lerp", 0.f, 1.f }, { "Opacity", 0.f, 1.f }, { "Edge Opacity", 0.f, 1.f },
      { "Thickness", 0.f, 1.f }, { "Falloff Power", 0.f, 10.f }, { "Reflection Strength", 0.f, 1.f }, { "Exposure", 0.f, 4.f },
   };
   for (const Range& range : ranges)
      if (label == range.label)
      {
         vMin = range.vMin;
         vMax = range.vMax;
         return true;
      }
   if (displayUnit == Unit::PercentX100)
   {
      vMin = 0.f;
      vMax = label.find("Scale") != string::npos ? 200.f : 100.f;
      return true;
   }
   return false;
}

void PropertyPane::RequestFocus(const PinKey& key)
{
   s_focusKey = key;
   s_focusPending = true;
   s_focusRequestFrame = ImGui::GetFrameCount();
   s_flashUntil = 0.0;
}

void PropertyPane::PinContext(const PinKey& key, const IEditable* part, const Material* material)
{
   // Right click anywhere on the field row (label or widget) opens the pin menu
   ImGuiWindow* const window = ImGui::GetCurrentWindow();
   const float rowBottom = m_rowBottom > 0.f ? m_rowBottom - ImGui::GetStyle().ItemSpacing.y : 0.f;
   const ImVec2 rowMax(window->WorkRect.Max.x, max(ImGui::GetItemRectMax().y, rowBottom));
   const bool rowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(m_rowMin, rowMax, false);
   // Search palette focus: scroll to the first panel row of the field, then flash it
   if (m_pinMode == PinMode::Panel && (s_focusPending || s_flashUntil > ImGui::GetTime()) && key == s_focusKey)
   {
      if (s_focusPending)
      {
         s_focusPending = false;
         s_flashUntil = ImGui::GetTime() + 1.2;
         ImGui::SetScrollHereY(0.3f);
         // Keyboard focus on the field, so a value can be typed right away (the last item is the field's widget, unless the
         // row ends with the copy-to-other-version button of an inspection session)
         if (m_rowFocusable && !m_sectionHasSync)
            ImGui::SetKeyboardFocusHere(-1);
      }
      else
      {
         const float alpha = static_cast<float>((s_flashUntil - ImGui::GetTime()) / 1.2);
         const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
         window->DrawList->AddRectFilled(m_rowMin, rowMax, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.35f * alpha)), 2.f);
      }
   }
   // Panel: a pin toggle at the left of the row, shown on hover (always when pinned)
   if (m_pinMode == PinMode::Panel)
   {
      const bool pinned = m_pins->IsPinned(key);
      if (pinned || rowHovered)
      {
         const float size = ImGui::GetFontSize();
         const ImVec2 iconMin(m_rowMin.x, m_rowMin.y + ImGui::GetStyle().FramePadding.y);
         const bool iconHovered = rowHovered && ImGui::IsMouseHoveringRect(iconMin, ImVec2(iconMin.x + size, iconMin.y + size), false);
         window->DrawList->AddText(iconMin, ImGui::GetColorU32(pinned ? ImGuiCol_CheckMark : iconHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled), ICON_FK_THUMB_TACK);
         if (iconHovered)
         {
            ImGui::SetTooltip(pinned ? "Unpin from overlay" : "Pin to overlay");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
               if (pinned)
                  m_pins->Unpin(key);
               else
                  m_pins->Pin(key);
            }
         }
      }
   }
   if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && rowHovered)
      ImGui::OpenPopup("##PinMenu");
   if (!ImGui::BeginPopup("##PinMenu"))
      return;
   if (m_pins->IsPinned(key))
   {
      if (ImGui::MenuItem(ICON_FK_THUMB_TACK " Unpin from overlay"))
         m_pins->Unpin(key);
   }
   else if (ImGui::MenuItem(ICON_FK_THUMB_TACK " Pin to overlay"))
      m_pins->Pin(key);
   if (part != nullptr && m_pins->HasSeeThroughSet(part))
   {
      if (ImGui::MenuItem("Pin see-through set"))
         m_pins->PinSeeThroughSet(part);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Pin the refraction fields of this part and the See-Through Surface fields of its material");
   }
   if (part != nullptr && m_pins->HasBulbSet(part))
   {
      if (ImGui::MenuItem("Pin bulb set"))
         m_pins->PinBulbSet(part);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Pin the Traced Insert Bulb fields of this part (depth, spot size, brightness, stretch, offset, apply to all)");
   }
   if (!(part != nullptr && m_pins->HasSeeThroughSet(part)) && material != nullptr && material->m_type == Material::PBR)
   {
      if (ImGui::MenuItem("Pin see-through set"))
         m_pins->PinMaterialSeeThrough(material->m_name);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Pin the See-Through Surface fields of this material");
   }
   ImGui::EndPopup();
}

void PropertyPane::ConvertUnit(Unit from, Unit& to, float& value, int& nDecimalAdjust)
{
   nDecimalAdjust = 0;
   if (from == Unit::Percent) // All percent values are shown as 0..100 range
      to = Unit::PercentX100;
   switch (to)
   {
   case Unit::Percent:
      switch (from)
      {
      case Unit::PercentX100:
         value = value / 100.f;
         nDecimalAdjust = 2;
         break;
      default: to = from; break;
      }
      break;

   case Unit::VPLength:
      switch (from)
      {
      case Unit::Inches:
         value = INCHESTOVPU(value);
         nDecimalAdjust = -2;
         break;
      case Unit::Millimeters:
         value = MMTOVPU(value);
         nDecimalAdjust = -2;
         break;
      default: to = from; break;
      }
      break;
   
   case Unit::Inches:
      switch (from)
      {
      case Unit::VPLength:
         value = VPUTOINCHES(value);
         nDecimalAdjust = 2;
         break;
      default: to = from; break;
      }
      break;

   case Unit::Millimeters:
      switch (from)
      {
      case Unit::VPLength:
         value = VPUTOMM(value);
         nDecimalAdjust = 2;
         break;
      default: to = from; break;
      }
      break;

   case Unit::PercentX100:
      switch (from)
      {
      case Unit::Percent:
         value = value * 100.f;
         nDecimalAdjust = -2;
         break;
      default: to = from; break;
      }
      break;

   default: to = from; break;
   }
}

void PropertyPane::ResolveUnit(Unit from, Unit& to, int& nDecimalAdjust)
{
   float value = 0.f;
   ConvertUnit(from, to, value, nDecimalAdjust);
}

const char* PropertyPane::GetUnitLabel(Unit unit)
{
   switch (unit)
   {
   case Unit::Inches: return "inches";
   case Unit::Millimeters: return "mm";
   case Unit::Degree: return "deg";
   case Unit::Percent:
   case Unit::PercentX100: return "%";
   case Unit::VPLength:
   case Unit::VPMass:
   case Unit::VPSpeed: return "vpu";
   default: return nullptr;
   }
}

}
