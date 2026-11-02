#include "core/stdafx.h"
#include "PropertyPane.h"
#include "IconsLucide.h"
#include "EditorTheme.h"

#include "core/pinundo.h"
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

MaterialEditUndo::MaterialEditUndo(PinUndo &undo, PinTable *table, Material *material, Source source)
   : m_undo(undo)
   , m_table(table)
   , m_material(material)
   , m_source(source)
{
   if (m_material != nullptr)
      m_before = std::make_unique<Material>(*m_material);
}

bool MaterialEditUndo::Commit(int modifiedField, const string &label)
{
   // The step being dragged / typed (same material, pane and field): only its first frame records a step
   static const Material *s_material = nullptr;
   static int s_field = 0;
   static uint64_t s_record = 0;
   if (m_material == nullptr)
      return false;
   const bool interacting = ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left);
   if (modifiedField <= 0)
   {
      if (!interacting) // Released: the next edit is a step of its own
      {
         s_material = nullptr;
         s_field = 0;
      }
      return false;
   }
   const int field = m_source | modifiedField;
   // Cancelled (Esc, also a typed value, which is no longer active on that frame): the value is back, the step it started is dropped if it is still the latest
   if (s_material == m_material && s_field == field && PropertyPane::ConsumeEditCancel())
   {
      if (m_undo.HasUndo() && m_undo.TopRecordId() == s_record)
         m_undo.Discard();
      s_material = nullptr;
      s_field = 0;
      return false;
   }
   if (interacting && s_material == m_material && s_field == field)
      return false;
   s_material = m_material;
   s_field = field;
   // The material may be deleted before the step is undone: check it is still in the table
   m_undo.BeginUndo();
   m_undo.SetFieldLabel(label);
   // Undoable and redoable: undoing captures the state it replaces so that the step can be redone. The material may be deleted in between
   // (a material created after a delete can reuse the freed address: the name must match too)
   const auto inTable = [table = m_table](const Material* material, const string& name)
   { return std::ranges::find(table->m_materials, material) != table->m_materials.end() && material->m_name == name; };
   const auto inTableAddress = [table = m_table](const Material* material) { return std::ranges::find(table->m_materials, material) != table->m_materials.end(); };
   const string nameAfter = m_material->m_name; // The material as the undo finds it
   const string nameBefore = m_before->m_name; // ... and as the redo finds it
   m_undo.MarkForAction(
      [inTable, nameAfter, material = m_material, before = std::shared_ptr<Material>(std::move(m_before))]()
      {
         if (inTable(material, nameAfter))
            *material = *before;
      },
      [inTable, inTableAddress, nameAfter, nameBefore, material = m_material]() -> std::function<void()>
      {
         // Also used for the inverse record: the material is in either state (edited or restored), the restore expects the other one
         if (!inTableAddress(material)) // (the address first: a deleted material must not be read)
            return []() {};
         const string cur = material->m_name;
         if (cur != nameAfter && cur != nameBefore)
            return []() {};
         const string expected = (cur == nameAfter) ? nameBefore : nameAfter;
         return [inTable, expected, material, state = std::make_shared<Material>(*material)]()
         {
            if (inTable(material, expected))
               *material = *state;
         };
      });
   m_undo.EndUndo();
   s_record = m_undo.TopRecordId();
   return true;
}

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

string PropertyPane::Ellipsize(const char* text, float maxWidth)
{
   const char* const textEnd = text + strlen(text);
   if (ImGui::CalcTextSize(text, textEnd).x <= maxWidth)
      return string(text);
   static const char* const ellipsis = "..."; // ASCII: the UI font may not carry U+2026
   const float dotsWidth = ImGui::CalcTextSize(ellipsis).x;
   const char* end = textEnd;
   while (end > text)
   {
      do // Step back one UTF-8 code point
         end--;
      while (end > text && (static_cast<unsigned char>(*end) & 0xC0) == 0x80);
      if (ImGui::CalcTextSize(text, end).x + dotsWidth <= maxWidth)
         break;
   }
   return trim_string(string(text, end)) + ellipsis;
}

void PropertyPane::DrawChevron(ImDrawList* drawList, const ImVec2& center, bool down, float size, ImU32 color)
{
   const float t = max(1.5f, 1.5f * size * 0.25f); // 1.5 px at the base font size
   if (down)
   {
      const ImVec2 pts[3] = { ImVec2(center.x - size, center.y - size * 0.5f), ImVec2(center.x, center.y + size * 0.5f), ImVec2(center.x + size, center.y - size * 0.5f) };
      drawList->AddPolyline(pts, 3, color, ImDrawFlags_None, t);
   }
   else
   {
      const ImVec2 pts[3] = { ImVec2(center.x - size * 0.5f, center.y - size), ImVec2(center.x + size * 0.5f, center.y), ImVec2(center.x - size * 0.5f, center.y + size) };
      drawList->AddPolyline(pts, 3, color, ImDrawFlags_None, t);
   }
}

bool PropertyPane::BeginFlatCombo(const char* id, const char* preview)
{
   // No ImGui arrow block (one tone field): a TextDim chevron is drawn inside the right end, and the preview is ellipsized to fit
   const ImGuiStyle& style = ImGui::GetStyle();
   const float width = ImGui::CalcItemWidth();
   const float height = ImGui::GetFrameHeight();
   const ImVec2 min = ImGui::GetCursorScreenPos();
   ImDrawList* const drawList = ImGui::GetWindowDrawList();
   const float chevronSize = ImGui::GetFontSize() * 0.3f;
   const string text = Ellipsize(preview, width - 2.f * style.FramePadding.x - 2.f * chevronSize - style.ItemInnerSpacing.x);
   const bool open = ImGui::BeginCombo(id, text.c_str(), ImGuiComboFlags_NoArrowButton);
   DrawChevron(drawList, ImVec2(min.x + width - style.FramePadding.x - chevronSize, min.y + height * 0.5f), true, chevronSize, EditorTheme::TextDim);
   return open;
}

bool PropertyPane::FlatCheckbox(const char* id, bool* value)
{
   // Blender look: unchecked = FrameBg grey, checked = Fill blue (the tick is the white CheckMark colour)
   const bool mixed = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_MixedValue) != 0;
   const bool fill = *value && !mixed;
   if (fill)
   {
      ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::Fill);
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, EditorTheme::FillHovered);
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, EditorTheme::FillActive);
   }
   const bool changed = ImGui::Checkbox(id, value);
   if (fill)
      ImGui::PopStyleColor(3);
   return changed;
}

int PropertyPane::FloatDecimals(Unit displayUnit, int requested, float shown)
{
   int decimals;
   switch (displayUnit)
   {
   case Unit::Percent:
   case Unit::PercentX100: decimals = 0; break;
   case Unit::Inches: decimals = 3; break;
   case Unit::VPLength:
   case Unit::VPMass:
   case Unit::VPSpeed:
   case Unit::Degree:
   case Unit::Millimeters: decimals = 1; break;
   default: decimals = requested == 0 ? 0 : 2; break; // Unitless factors; 0 stays for fields that are explicitly integral
   }
   if (requested >= 4) // Explicitly asked for more precision (tiny values)
      decimals = max(decimals, requested);
   // Never display a small non-zero value as zero
   while (decimals < 4 && shown != 0.f && fabsf(shown) < 0.5f * powf(10.f, -static_cast<float>(decimals)))
      decimals++;
   return decimals;
}

bool PropertyPane::NumberField(const char* id, float* value, int decimals)
{
   // Same frame as the fill sliders, value centred, no fill; press and drag changes the value, a click (or ctrl+click, double click) types it
   const string format = "%." + std::to_string(decimals) + 'f';
   return DragField(id, value, 0.f, 0.f, false, decimals, format.c_str());
}

void PropertyPane::RoundDragged(float* values, const float* before, int count, int decimals)
{
   // Drag results are rounded to the displayed precision; typed values (text input active) keep every digit
   if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::GetIO().WantTextInput)
      return;
   const float scale = powf(10.f, static_cast<float>(decimals));
   for (int i = 0; i < count; i++)
      if (values[i] != before[i])
         values[i] = roundf(values[i] * scale) / scale;
}

void PropertyPane::PropertyLabel(const string& label, const char* tooltip)
{
   FlushRow();
   m_rowFocusable = false;
   const ImVec2 pos = ImGui::GetCursorScreenPos();
   m_rowMin = pos;
   const ImGuiStyle& style = ImGui::GetStyle();
   const float xSpacing = style.ItemSpacing.x;
   // Label column: ~42% of the pane, at least 120 px, while keeping a sane minimum for the value column
   const float availWidth = ImGui::GetContentRegionAvail().x;
   const float valueMin = 100.f;
   const float xWidth = max(60.f, min(max(availWidth * 0.5f, 120.f), availWidth - 2.f * m_syncWidth - valueMin));
   const float valueWidth = max(valueMin * 0.5f, availWidth - xWidth - 2.f * m_syncWidth);
   const float pinWidth = 0.f;
   const float fieldDy = 0.f;
   const float maxWidth = max(0.f, xWidth - xSpacing - pinWidth);

   // Single line, right aligned against the value column; ellipsized (full label in the tooltip) when it does not fit
   const string shown = Ellipsize(label.c_str(), maxWidth);
   const bool truncated = shown != label;
   const float width = ImGui::CalcTextSize(shown.c_str()).x;
   ImGui::SetCursorScreenPos(ImVec2(pos.x + max(pinWidth, xWidth - width - xSpacing), pos.y + style.FramePadding.y));
   ImGui::TextUnformatted(shown.c_str());
   if (m_highlight != nullptr && !m_highlight->empty())
   {
      // Search: a tint over the first occurrence of each term (the text stays readable under it)
      const ImVec2 textMin = ImGui::GetItemRectMin();
      string lower = shown;
      std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      for (const string &term : *m_highlight)
      {
         const size_t at = term.empty() ? string::npos : lower.find(term);
         if (at == string::npos)
            continue;
         const float x0 = textMin.x + ImGui::CalcTextSize(shown.c_str(), shown.c_str() + at).x;
         const float x1 = x0 + ImGui::CalcTextSize(shown.c_str() + at, shown.c_str() + at + term.size()).x;
         ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0 - 1.f, textMin.y), ImVec2(x1 + 1.f, textMin.y + ImGui::GetTextLineHeight()), EditorTheme::SearchMatch, 2.f);
      }
   }
   if (truncated && tooltip != nullptr)
      ImGui::SetItemTooltip("%s\n%s", label.c_str(), tooltip);
   else if (truncated)
      ImGui::SetItemTooltip("%s", label.c_str());
   else if (tooltip != nullptr)
      ImGui::SetItemTooltip("%s", tooltip);

   ImGui::SetCursorScreenPos(ImVec2(pos.x + xWidth, pos.y + fieldDy));
   ImGui::SetNextItemWidth(valueWidth);
   m_syncPos = ImVec2(pos.x + xWidth + valueWidth, pos.y + fieldDy);
}

void PropertyPane::Header(const string& typeName, const std::function<string()>& getName, const std::function<void(const string&)>& setName)
{
   ImGui::Dummy(ImVec2(0.f, 2.f));
   ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
   LiveUI::CenteredText(IsMultiEdit() ? typeName + " x" + std::to_string(GetEditCount()) : typeName);
   ImGui::PopStyleColor();
   ImGui::Dummy(ImVec2(0.f, 2.f));
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
   ImGui::Dummy(ImVec2(0.f, 4.f));
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
   // ImGui's filled triangle clashes with the line icons: the header is submitted with invisible text (same ID, click, keyboard
   // and open-state behaviour), then the label and a TextDim chevron are drawn over it
   const ImU32 labelColor = ImGui::GetColorU32(ImGuiCol_Text);
   ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 0.f));
   const bool opened = ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
   ImGui::PopStyleColor();

   // Thin accent rule on the left of the header
   const ImVec2 headerMin = ImGui::GetItemRectMin();
   const ImVec2 headerMax = ImGui::GetItemRectMax();
   ImDrawList* drawList = ImGui::GetWindowDrawList();
   {
      const ImGuiStyle& style = ImGui::GetStyle();
      const float fontSize = ImGui::GetFontSize();
      const float chevronSize = fontSize * 0.3f;
      DrawChevron(drawList, ImVec2(headerMin.x + style.FramePadding.x + fontSize * 0.5f, (headerMin.y + headerMax.y) * 0.5f), opened, chevronSize, EditorTheme::TextDim);
      drawList->AddText(ImVec2(headerMin.x + fontSize + style.FramePadding.x * 3.f, headerMin.y + style.FramePadding.y), labelColor, name.c_str(), ImGui::FindRenderedTextEnd(name.c_str()));
   }
   drawList->AddRectFilled(headerMin, ImVec2(headerMin.x + 2.f, headerMax.y), EditorTheme::Accent);
   drawList->AddLine(ImVec2(headerMin.x, headerMax.y), ImVec2(headerMax.x, headerMax.y), EditorTheme::WorkspaceOff, 1.f); // Separates stacked headers

   if (!opened)
      return false;

   ImGui::Dummy(ImVec2(0.f, 4.f));
   m_inSection = true;
   m_pinGroup = name;
   m_pinSection = name;
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

namespace
{
// State of the field being pressed or typed in (only one field has the mouse at a time)
struct DragFieldState
{
   ImGuiID id = 0; // Field pressed or dragged
   ImVec2 press; // Mouse position at the press
   double start = 0.0; // Value at the press
   double acc = 0.0; // Value change accumulated since the drag started
   float speed = 0.f; // Value per pixel of a number field
   bool moved = false; // The mouse went past the click slop: this is a drag
   ImGuiID editId = 0; // Field in text edit mode
   double editStart = 0.0; // Value when the text edit started (Esc restores it exactly, not its rounded text)
};
DragFieldState s_dragField;
}

bool PropertyPane::DragField(const char* id, float* value, float vMin, float vMax, bool fill, int decimals, const char* format)
{
   return DragFieldImpl(id, value, nullptr, vMin, vMax, fill, decimals, format);
}

bool PropertyPane::DragFieldInt(const char* id, int* value)
{
   return DragFieldImpl(id, nullptr, value, 0.f, 0.f, false, 0, "%d");
}

// Float or integer field (exactly one of value / intValue): an integer is dragged and typed as an integer, so that large values stay exact
bool PropertyPane::DragFieldImpl(const char* id, float* value, int* intValue, float vMin, float vMax, bool fill, int decimals, const char* format)
{
   ImGuiWindow* const window = ImGui::GetCurrentWindow();
   if (window->SkipItems)
      return false;
   ImGuiContext& g = *ImGui::GetCurrentContext();
   const ImGuiStyle& style = g.Style;
   const ImGuiID fieldId = window->GetID(id);
   const ImRect bb(window->DC.CursorPos, window->DC.CursorPos + ImVec2(ImGui::CalcItemWidth(), ImGui::GetFrameHeight()));
   ImGui::ItemSize(bb, style.FramePadding.y);
   if (!ImGui::ItemAdd(bb, fieldId, &bb, ImGuiItemFlags_Inputable))
      return false;
   const auto current = [&]() -> double { return intValue ? static_cast<double>(*intValue) : static_cast<double>(*value); };
   const auto put = [&](double v)
   {
      if (intValue)
         *intValue = static_cast<int>(clamp(llround(v), static_cast<long long>(INT_MIN), static_cast<long long>(INT_MAX)));
      else
         *value = static_cast<float>(v);
   };
   const bool mixed = (g.LastItemData.ItemFlags & ImGuiItemFlags_MixedValue) != 0;
   const bool hovered = ImGui::ItemHoverable(bb, fieldId, g.LastItemData.ItemFlags);
   DragFieldState& st = s_dragField;
   bool changed = false;
   bool textMode = ImGui::TempInputIsActive(fieldId);
   if (!textMode && st.editId == fieldId)
      st.editId = 0;
   if (st.id == fieldId && g.ActiveId != fieldId)
      st.id = 0; // The press was cancelled from elsewhere
   if (!textMode)
   {
      const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left, ImGuiInputFlags_None, fieldId);
      const bool doubleClicked = hovered && g.IO.MouseClickedCount[0] == 2 && ImGui::TestKeyOwner(ImGuiKey_MouseLeft, fieldId);
      if (clicked || doubleClicked)
         ImGui::SetKeyOwner(ImGuiKey_MouseLeft, fieldId);
      if ((clicked && g.IO.KeyCtrl) || doubleClicked || g.NavActivateId == fieldId)
         textMode = true;
      else if (clicked)
      {
         // Press: nothing changes yet (no jump to the cursor, unlike ImGui sliders), the value is edited relative to the grab
         ImGui::SetActiveID(fieldId, window);
         ImGui::SetFocusID(fieldId, window);
         ImGui::FocusWindow(window);
         g.ActiveIdUsingNavDirMask = (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
         st = DragFieldState();
         st.id = fieldId;
         st.press = g.IO.MousePos;
         st.start = current();
         st.speed = DragSpeed(decimals, static_cast<float>(fabs(st.start)));
      }
      if (!textMode && st.id == fieldId && g.ActiveId == fieldId)
      {
         if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) // Cancels the drag
         {
            if (st.moved && current() != st.start)
            {
               put(st.start);
               changed = true;
               s_editCancelled = true;
            }
            s_escConsumed = true;
            ImGui::ClearActiveID();
            st.id = 0;
         }
         else if (g.IO.MouseDown[0])
         {
            if (!st.moved)
               st.moved = fabsf(g.IO.MousePos.x - st.press.x) >= 3.f; // Click slop; the drag starts from here, without a jump
            else
            {
               const bool fine = g.IO.KeyShift;
               double speed = fill ? (vMax - vMin) / max(bb.GetWidth(), 1.f) : st.speed;
               if (fine)
                  speed *= 0.1;
               st.acc += g.IO.MouseDelta.x * speed;
               if (fill)
                  st.acc = clamp(st.start + st.acc, static_cast<double>(vMin), static_cast<double>(vMax)) - st.start; // Soft range: the drag stays inside it
               double v = st.start + st.acc;
               if (g.IO.KeyCtrl)
               {
                  // Snap to tenths of the range (number fields: ten units of the last displayed digit)
                  const double base = fill ? vMin : 0.0;
                  const double step = fill ? (vMax - vMin) * 0.1 : pow(10.0, 1.0 - decimals);
                  if (step > 0.0)
                     v = base + round((v - base) / step) * step;
                  if (fill)
                     v = clamp(v, static_cast<double>(vMin), static_cast<double>(vMax));
               }
               else
               {
                  const double scale = pow(10.0, decimals + (fine ? 1 : 0));
                  v = round(v * scale) / scale;
               }
               const double before = current();
               put(v);
               changed = current() != before;
            }
         }
         else
         {
            // Released: a click that did not drag types the value (the text field takes the activation over on its first frame)
            const bool click = !st.moved;
            st.id = 0;
            if (click)
            {
               g.NavActivateId = fieldId;
               g.NavActivateFlags = ImGuiActivateFlags_PreferInput;
               textMode = true;
            }
            else
               ImGui::ClearActiveID();
         }
      }
   }

   if (textMode)
   {
      // Typing: the value follows the text; Enter or a click away keeps it, Esc puts the exact value back
      if (st.editId != fieldId)
      {
         st.editId = fieldId;
         st.editStart = current();
      }
      const bool cancel = ImGui::TempInputIsActive(fieldId) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
      const double before = current();
      bool edited = intValue ? ImGui::TempInputScalar(bb, fieldId, id, ImGuiDataType_S32, intValue, "%d", nullptr, nullptr)
                             : ImGui::TempInputScalar(bb, fieldId, id, ImGuiDataType_Float, value, "%.6g", nullptr, nullptr);
      if (cancel)
      {
         s_escConsumed = true;
         put(st.editStart);
         edited = current() != before;
         if (edited)
            s_editCancelled = true;
      }
      else if (hovered && g.IO.MouseClickedCount[0] == 2)
      {
         // Double click types the whole number (the word selection of a double click stops at the decimal point)
         if (ImGuiInputTextState* const state = ImGui::GetInputTextState(fieldId))
            state->SelectAll();
      }
      return edited;
   }

   if (changed)
      ImGui::MarkItemEdited(fieldId);
   const bool active = g.ActiveId == fieldId;
   if (hovered || active)
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
   // Frame (neutral grey on hover), then the value bar (Blender style, brighter while dragged; only the left corners are rounded unless full), then the number
   ImDrawList* const drawList = window->DrawList;
   const float rounding = style.FrameRounding;
   ImGui::RenderNavCursor(bb, fieldId);
   drawList->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rounding);
   if (fill && vMax > vMin && value != nullptr)
   {
      const float t = clamp((*value - vMin) / (vMax - vMin), 0.f, 1.f);
      if (t > 0.f)
      {
         const ImU32 color = active ? EditorTheme::FillActive : hovered ? EditorTheme::FillHovered : EditorTheme::Fill;
         drawList->AddRectFilled(bb.Min, ImVec2(bb.Min.x + bb.GetWidth() * t, bb.Max.y), color, rounding, t >= 1.f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
      }
   }
   char text[64];
   const char* shown = text;
   if (mixed && g.MixedValueLabel != nullptr)
      shown = g.MixedValueLabel;
   else
      intValue ? ImFormatString(text, IM_COUNTOF(text), format, *intValue) : ImFormatString(text, IM_COUNTOF(text), format, *value);
   ImGui::PushStyleColor(ImGuiCol_Text, mixed ? g.Style.Colors[ImGuiCol_TextMixedValue] : (fill ? ImVec4(1.f, 1.f, 1.f, 1.f) : g.Style.Colors[ImGuiCol_Text]));
   ImGui::RenderTextClipped(bb.Min, bb.Max, shown, nullptr, nullptr, ImVec2(0.5f, 0.5f));
   ImGui::PopStyleColor();
   return changed;
}

bool PropertyPane::ConsumeEscape()
{
   if (!s_escConsumed)
      return false;
   if (!ImGui::IsKeyDown(ImGuiKey_Escape))
      s_escConsumed = false; // Released: this is the frame of the release, still consumed
   return true;
}

bool PropertyPane::DragFieldN(const char* id, float* values, int count, int decimals, const char* format)
{
   const ImGuiStyle& style = ImGui::GetStyle();
   const float width = IM_TRUNC((ImGui::CalcItemWidth() - static_cast<float>(count - 1) * style.ItemInnerSpacing.x) / static_cast<float>(count));
   bool changed = false;
   ImGui::BeginGroup();
   for (int i = 0; i < count; i++)
   {
      if (i > 0)
         ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
      ImGui::SetNextItemWidth(width);
      changed |= DragField((string(id) + std::to_string(i)).c_str(), &values[i], 0.f, 0.f, false, decimals, format);
   }
   ImGui::EndGroup();
   return changed;
}

bool PropertyPane::FillSlider(const char* id, float* value, float vMin, float vMax, const char* format)
{
   // Soft range: a value beyond it extends the displayed range instead of being pinned at the end
   vMin = min(vMin, *value);
   vMax = max(vMax, *value);
   return DragField(id, value, vMin, vMax, true, ImParseFormatPrecision(format, 3), format);
}

bool PropertyPane::LookSliderRange(const string& label, Unit displayUnit, float& vMin, float& vMax)
{
   // Fields whose useful range is known (display units); the ranges are soft (FillSlider extends them to include the value).
   // Percentages are 0..100, scales up to 200 %
   struct Range
   {
      const char* label;
      float vMin, vMax;
   };
   static constexpr Range ranges[] = {
      // Exact labels
      { "Roughness", 0.f, 1.f }, { "Metallic", 0.f, 1.f }, { "Occlusion", 0.f, 1.f }, { "Clear Coat", 0.f, 1.f }, { "Clear Coat Roughness", 0.f, 1.f },
      { "Edge Brightness", 0.f, 1.f }, { "Wrap Lighting", 0.f, 1.f }, { "Glossy Image Lerp", 0.f, 1.f }, { "Opacity", 0.f, 1.f }, { "Edge Opacity", 0.f, 1.f },
      { "Thickness", 0.f, 1.f }, { "Falloff Power", 0.f, 10.f }, { "Reflection Strength", 0.f, 1.f }, { "Exposure", 0.f, 4.f },
      // Known full labels of the part panes (no partial matching: "Thickness" must not catch "Refraction Thickness")
      { "Modulate Opacity", 0.f, 100.f }, { "Intensity", 0.f, 10.f }, { "Env. Reflection Scale", 0.f, 2.f }, { "Translucency", 0.f, 1.f },
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

void PropertyPane::RequestOverlayFlash(const PinKey& key)
{
   s_overlayKey = key;
   s_overlayPending = true;
   s_overlayRequestTime = ImGui::GetTime();
   s_overlayFlashUntil = 0.0;
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
   // A favorite was just added: its row in the N-panel / Q popup scrolls into view and flashes
   if (m_pinMode == PinMode::Overlay && (s_overlayPending || s_overlayFlashUntil > ImGui::GetTime()) && key == s_overlayKey)
   {
      const double now = ImGui::GetTime();
      if (s_overlayPending)
      {
         s_overlayPending = false;
         if (now - s_overlayRequestTime < 6.0) // Not drawn within a few seconds (panel hidden): forget it
         {
            s_overlayFlashUntil = now + 1.2;
            ImGui::SetScrollHereY(0.3f);
         }
      }
      else
      {
         const float alpha = static_cast<float>((s_overlayFlashUntil - now) / 1.2);
         const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
         window->DrawList->AddRectFilled(m_rowMin, rowMax, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.35f * alpha)), 2.f);
      }
   }
   if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && rowHovered)
      ImGui::OpenPopup("##PinMenu");
   if (!ImGui::BeginPopup("##PinMenu"))
      return;
   if (m_pins->IsPinned(key))
   {
      if (ImGui::MenuItem(ICON_LC_STAR " Remove from Quick Favorites"))
         m_pins->Unpin(key);
   }
   else if (ImGui::MenuItem(ICON_LC_STAR " Add to Quick Favorites"))
   {
      m_pins->Pin(key);
      RequestOverlayFlash(key);
   }
   if (part != nullptr && m_pins->HasSeeThroughSet(part))
   {
      if (ImGui::MenuItem("Add see-through set to Quick Favorites"))
         m_pins->PinSeeThroughSet(part);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Add the refraction fields of this part and the See-Through Surface fields of its material");
   }
   if (part != nullptr && m_pins->HasBulbSet(part))
   {
      if (ImGui::MenuItem("Add bulb set to Quick Favorites"))
         m_pins->PinBulbSet(part);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Add the Traced Insert Bulb fields of this part (depth, spot size, brightness, stretch, offset, apply to all)");
   }
   if (!(part != nullptr && m_pins->HasSeeThroughSet(part)) && material != nullptr && material->m_type == Material::PBR)
   {
      if (ImGui::MenuItem("Add see-through set to Quick Favorites"))
         m_pins->PinMaterialSeeThrough(material->m_name);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Add the See-Through Surface fields of this material");
   }
   if (part != nullptr)
   {
      // Blender's "Copy to Selected": this field's value goes to the other selected parts that have the same field (Alt while editing does it live)
      ImGui::Separator();
      const bool canCopy = m_pins->CanCopyToSelected(key);
      if (ImGui::MenuItem(ICON_LC_COPY " Copy to Selected", nullptr, false, canCopy))
         m_pins->CopyToSelected(key);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("%s", canCopy ? "Write this value to the other selected parts that have the same field (hold Alt while editing a field to do it live)"
                                         : "Select other parts that have this field (Ctrl+click in the outliner or viewport)");
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
