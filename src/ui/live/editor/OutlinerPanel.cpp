// license:GPLv3+

#include "core/stdafx.h"
#include "OutlinerPanel.h"
#include "IconsLucide.h"
#include "EditorTheme.h"

#include "fonts/IconsForkAwesome.h"
#include "parts/Collection.h"
#include "parts/Material.h"
#include "parts/PartGroup.h"
#include "parts/Sound.h"
#include "PropertiesPanel.h"
#include "renderer/Texture.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

#include "imgui/imgui_stdlib.h"

namespace VPX::EditorUI
{

template <class T> static std::vector<T> SortedCaseInsensitive(std::vector<T> &list, const std::function<string(T)> &map)
{
   std::vector<T> sorted(list.begin(), list.end());
   std::ranges::sort(sorted, [&map](const T &a, const T &b) { return StrLessNoCase(map(a), map(b)); });
   return sorted;
}

// Push a dimmed selection color, used to highlight the non active elements of a multi selection
// less than the active one (the element edited in the property pane keeps the normal selection color)
static void PushInactiveSelectionColor()
{
   const ImVec4 color = EditorTheme::V(EditorTheme::RowSelected);
   ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x, color.y, color.z, color.w * 0.4f));
}

// Name shortened with "..." so that it fits in maxWidth (whole UTF-8 characters are removed)
static string EllipsizeToWidth(const string &text, float maxWidth)
{
   if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
      return text;
   string cut = text;
   while (!cut.empty())
   {
      while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
         cut.pop_back(); // Continuation bytes
      if (!cut.empty())
         cut.pop_back(); // Lead byte (or ASCII character)
      if (ImGui::CalcTextSize((cut + "...").c_str()).x <= maxWidth)
         break;
   }
   return cut + "...";
}

// Push the full row selection color of the outliner (active selection)
static void PushSelectionColor()
{
   ImGui::PushStyleColor(ImGuiCol_Header, EditorTheme::V(EditorTheme::RowSelected));
}

namespace
{
// Type display names, with an alias also accepted by the 't:' filter token
struct TypeInfo
{
   ItemTypeEnum type;
   const char *name;
   const char *alias;
};
constexpr TypeInfo TypeInfos[] = {
   { eItemSurface, "Surface", "wall" },
   { eItemFlipper, "Flipper", "" },
   { eItemTimer, "Timer", "" },
   { eItemPlunger, "Plunger", "" },
   { eItemTextbox, "Textbox", "" },
   { eItemBumper, "Bumper", "" },
   { eItemTrigger, "Trigger", "" },
   { eItemLight, "Light", "" },
   { eItemKicker, "Kicker", "" },
   { eItemDecal, "Decal", "" },
   { eItemGate, "Gate", "" },
   { eItemSpinner, "Spinner", "" },
   { eItemRamp, "Ramp", "" },
   { eItemDispReel, "Display Reel", "dispreel" },
   { eItemLightSeq, "Light Seq", "lightseq" },
   { eItemPrimitive, "Primitive", "" },
   { eItemFlasher, "Flasher", "" },
   { eItemRubber, "Rubber", "" },
   { eItemHitTarget, "Target", "hittarget" },
   { eItemBall, "Ball", "" },
   { eItemPartGroup, "Part Group", "partgroup" },
};

const TypeInfo *FindTypeInfo(ItemTypeEnum type)
{
   for (const TypeInfo &info : TypeInfos)
      if (info.type == type)
         return &info;
   return nullptr;
}

// prefix must already be lowercase
bool StartsWithNoCase(const char *text, const string &prefix)
{
   for (const char p : prefix)
   {
      if (*text == '\0' || cLower(*text) != p)
         return false;
      ++text;
   }
   return true;
}

// Object type categories of the filter popover (bit index in the type mask)
enum TypeCategory
{
   CatPrimitives,
   CatLights,
   CatFlashers,
   CatRamps,
   CatWalls,
   CatRubbers,
   CatFlippers,
   CatBumpers,
   CatTargets,
   CatGatesSpinners,
   CatTriggersKickers,
   CatOther,
   CatCount
};
static_assert(OutlinerPanel::AllTypesMask == (1u << CatCount) - 1u);
constexpr const char *CategoryLabels[CatCount] = { "Primitives", "Lights", "Flashers", "Ramps", "Walls", "Rubbers", "Flippers", "Bumpers", "Targets", "Gates / Spinners", "Triggers / Kickers", "Other" };

TypeCategory CategoryOf(ItemTypeEnum type)
{
   switch (type)
   {
   case eItemPrimitive: return CatPrimitives;
   case eItemLight: return CatLights;
   case eItemFlasher: return CatFlashers;
   case eItemRamp: return CatRamps;
   case eItemSurface: return CatWalls;
   case eItemRubber: return CatRubbers;
   case eItemFlipper: return CatFlippers;
   case eItemBumper: return CatBumpers;
   case eItemHitTarget: return CatTargets;
   case eItemGate:
   case eItemSpinner: return CatGatesSpinners;
   case eItemTrigger:
   case eItemKicker: return CatTriggersKickers;
   default: return CatOther;
   }
}

const char *TypeIcon(ItemTypeEnum type)
{
   switch (type)
   {
   case eItemPrimitive: return ICON_LC_BOX;
   case eItemLight: return ICON_LC_LIGHTBULB;
   case eItemFlasher: return ICON_LC_ZAP;
   case eItemRamp: return ICON_LC_ROUTE;
   case eItemSurface: return ICON_LC_SQUARE;
   case eItemRubber: return ICON_LC_CIRCLE;
   case eItemFlipper: return ICON_LC_POINTER;
   case eItemBumper: return ICON_LC_DISC;
   case eItemHitTarget: return ICON_LC_CIRCLE_DOT;
   case eItemGate: return ICON_LC_MOVE_HORIZONTAL;
   case eItemSpinner: return ICON_LC_REFRESH_CW;
   case eItemTrigger: return ICON_LC_MINUS;
   case eItemKicker: return ICON_LC_CIRCLE;
   case eItemPlunger: return ICON_LC_ARROW_UP;
   case eItemTimer: return ICON_LC_TIMER;
   case eItemTextbox: return ICON_LC_TYPE;
   case eItemDecal: return ICON_LC_IMAGE;
   default: return ICON_LC_TAG;
   }
}
}

void OutlinerPanel::ParseFilter()
{
   if (m_filter == m_parsedFilter)
      return;
   m_parsedFilter = m_filter;
   m_tokens.names.clear();
   m_tokens.types.clear();
   m_tokens.materials.clear();
   const string lowered = lowerCase(m_filter);
   size_t pos = 0;
   while (pos < lowered.size())
   {
      const size_t end = std::min(lowered.find(' ', pos), lowered.size());
      string token = lowered.substr(pos, end - pos);
      pos = end + 1;
      if (token.empty())
         continue;
      vector<string> *target = &m_tokens.names;
      if (token.size() >= 2 && token[1] == ':' && (token[0] == 't' || token[0] == 'm' || token[0] == 'n'))
      {
         target = token[0] == 't' ? &m_tokens.types : token[0] == 'm' ? &m_tokens.materials : &m_tokens.names;
         token.erase(0, 2);
         if (token.empty())
            continue;
      }
      target->push_back(std::move(token));
   }
   m_openMatchesPending = true;
   ++m_filterGeneration;
}

bool OutlinerPanel::MatchesFilter(const string &name) const
{
   if (m_tokens.names.empty())
      return true;
   const string name_lcase = lowerCase(name);
   return std::ranges::all_of(m_tokens.names, [&name_lcase](const string &token) { return name_lcase.find(token) != std::string::npos; });
}

bool OutlinerPanel::MatchesPartCached(size_t index) const
{
   if (m_tokens.empty())
      return true;
   if (!std::ranges::all_of(m_tokens.names, [this, index](const string &token) { return m_lowerNames[index].find(token) != std::string::npos; }))
      return false;
   if (!m_tokens.types.empty())
   {
      const TypeInfo *info = FindTypeInfo(m_editor.m_editables[index]->GetEditable()->GetItemType());
      const char *name = info ? info->name : "Part";
      const char *alias = info ? info->alias : "";
      if (!std::ranges::all_of(m_tokens.types, [name, alias](const string &token) { return StartsWithNoCase(name, token) || StartsWithNoCase(alias, token); }))
         return false;
   }
   return std::ranges::all_of(m_tokens.materials, [this, index](const string &token) { return m_lowerMaterials[index].find(token) != std::string::npos; });
}

void OutlinerPanel::RefreshFilterCache()
{
   const auto &editables = m_editor.m_editables;
   const bool backdropView = m_editor.m_camMode == ViewMode::DesktopBackdrop;
   const double now = ImGui::GetTime();
   if (editables.size() == m_cacheSize && m_filterGeneration == m_cacheGeneration && backdropView == m_cacheBackdrop && now - m_cacheTime < 1.0)
   {
      // Reordered (drag & drop) or replaced (undo) parts keep the count: compare the content
      bool same = true;
      for (size_t i = 0; i < editables.size() && same; ++i)
         same = editables[i]->GetEditable() == m_cachePtrs[i];
      if (same)
         return;
   }
   m_cacheSize = editables.size();
   m_cacheGeneration = m_filterGeneration;
   m_cacheBackdrop = backdropView;
   m_cacheTime = now;
   m_cachePtrs.resize(editables.size());
   m_lowerNames.resize(editables.size());
   m_lowerMaterials.resize(editables.size());
   for (size_t i = 0; i < editables.size(); ++i)
   {
      const IEditable *const editable = editables[i]->GetEditable();
      m_cachePtrs[i] = editable;
      if (m_tokens.names.empty())
         m_lowerNames[i].clear();
      else
         m_lowerNames[i] = lowerCase(editable->GetName());
      if (m_tokens.materials.empty())
         m_lowerMaterials[i].clear();
      else
         m_lowerMaterials[i] = lowerCase(PropertiesPanel::PartMaterial(editable));
   }
}

void OutlinerPanel::SelectViewSetup(int viewSetup)
{
   EditorUI &editor = m_editor;
   editor.SetSelection(Selection::Camera(viewSetup));
   editor.m_camMode = ViewMode::PreviewCam;
   editor.m_table->SetViewSetupOverride(viewSetup == 0 ? BG_DESKTOP : viewSetup == 1 ? BG_FULLSCREEN : BG_FSS);
}

void OutlinerPanel::Render(float topBarHeight)
{
   EditorUI &editor = m_editor;
   if (editor.m_table->IsLocked())
      return;

   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   const float pane_width = editor.m_outlinerWidth * editor.m_liveUI.GetDPI();
   ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + topBarHeight));
   ImGui::SetNextWindowSize(ImVec2(pane_width, viewport->Size.y - topBarHeight));
   constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
      | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * editor.m_liveUI.GetDPI(), 4.0f * editor.m_liveUI.GetDPI()));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   ImGui::Begin("OUTLINER", nullptr, window_flags);
   const ImGuiStyle &style = ImGui::GetStyle();

   // Show the selection made elsewhere (viewport, property pane, search): resources live in Table Data and parts in View Layer
   if (editor.m_selection != m_lastSeenSelection)
   {
      m_lastSeenSelection = editor.m_selection;
      switch (editor.m_selection.GetType())
      {
      case Selection::S_MATERIAL:
      case Selection::S_IMAGE:
      case Selection::S_SOUND:
      case Selection::S_RENDERPROBE:
      case Selection::S_COLLECTION:
      case Selection::S_CAMERA:
         m_revealResource = m_syncToSelection || m_displayMode != 1;
         m_displayMode = 1;
         break;
      case Selection::S_EDITABLE: m_displayMode = 0; break;
      default: break;
      }
   }

   // Header, first row: display mode (Blender's display mode combo) and sync button
   {
      const bool popStyle = m_syncToSelection;
      if (popStyle)
         ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button(ICON_LC_LINK "##SyncTree"))
      {
         m_syncToSelection = !m_syncToSelection;
         m_lastSyncedPart = nullptr; // Reveal the current selection on next render
      }
      if (popStyle)
         ImGui::PopStyleColor();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Sync tree with selection");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
      static constexpr const char *modeNames[] = { "View Layer", "Table Data" };
      if (ImGui::BeginCombo("##OutlinerMode", modeNames[m_displayMode]))
      {
         for (int mode = 0; mode < 2; ++mode)
            if (ImGui::Selectable(modeNames[mode], m_displayMode == mode))
               m_displayMode = mode;
         ImGui::EndCombo();
      }
   }
   // Second row: search, clear and the filter popover
   {
      const float buttonWidth = ImGui::CalcTextSize(ICON_LC_X).x + style.FramePadding.x * 2.0f;
      const float filterWidth = ImGui::CalcTextSize(ICON_LC_LIST_FILTER).x + style.FramePadding.x * 2.0f;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - filterWidth - style.ItemSpacing.x * 2.0f);
      if (m_focusFilter)
      {
         ImGui::SetKeyboardFocusHere();
         m_focusFilter = false;
      }
      ImGui::InputTextWithHint("##OutlinerFilter", "Search", &m_filter);
      ImGui::SetItemTooltip("Name words, t:light, m:<material>, n:<name> (Ctrl+F)");
      ImGui::SameLine();
      ImGui::BeginDisabled(m_filter.empty());
      if (ImGui::Button(ICON_LC_X "##ClearOutlinerFilter"))
         m_filter.clear();
      ImGui::EndDisabled();
      ImGui::SameLine();
      const bool typeFilterActive = m_typeMask != AllTypesMask || m_selectedOnly;
      if (typeFilterActive)
         ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button(ICON_LC_LIST_FILTER "##OutlinerFilterButton"))
         ImGui::OpenPopup("##OutlinerFilterPopup");
      if (typeFilterActive)
         ImGui::PopStyleColor();
      ImGui::SetItemTooltip("Filters");
      ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y), ImGuiCond_Always, ImVec2(1.f, 0.f));
      if (ImGui::BeginPopup("##OutlinerFilterPopup"))
      {
         bool changed = false;
         if (ImGui::SmallButton("All"))
         {
            m_typeMask = AllTypesMask;
            changed = true;
         }
         ImGui::SameLine();
         if (ImGui::SmallButton("None"))
         {
            m_typeMask = 0;
            changed = true;
         }
         ImGui::Separator();
         for (int cat = 0; cat < CatCount; ++cat)
            changed |= ImGui::CheckboxFlags(CategoryLabels[cat], &m_typeMask, 1u << cat);
         ImGui::Separator();
         changed |= ImGui::Checkbox("Selected only", &m_selectedOnly);
         if (changed)
            m_openMatchesPending = true;
         ImGui::EndPopup();
      }
   }
   ImGui::Separator();
   ParseFilter();
   RefreshFilterCache();

   // Drag & drop of parts and part groups to move them between groups (not supported in inspection
   // mode as the inspected table is a live copy). The drag payload is just a marker: the moved parts
   // are the multi selection (the dragged part is selected first if it was not part of it). The move
   // is applied after the tree iteration since it mutates the ordering of the iterated part list.
   const bool canReparent = editor.m_table->m_liveBaseTable == nullptr;
   PartGroup *dropGroup = nullptr;
   bool dropRequested = false;
   const auto partDragSource = [&editor, canReparent](const std::shared_ptr<EditorUIPart> &uiPart)
   {
      if (!canReparent || !ImGui::BeginDragDropSource())
         return;
      if (!editor.IsPartSelected(uiPart))
         editor.SetSelection(Selection(uiPart));
      const IEditable *editable = uiPart->GetEditable();
      ImGui::SetDragDropPayload("VPX_PART", &editable, sizeof(editable));
      ImGui::Text("Move %d part%s", (int)editor.m_multiSel.size(), editor.m_multiSel.size() > 1 ? "s" : "");
      ImGui::EndDragDropSource();
   };
   const auto partDropTarget = [&dropGroup, &dropRequested, canReparent](PartGroup *group)
   {
      if (!canReparent || !ImGui::BeginDragDropTarget())
         return;
      if (ImGui::AcceptDragDropPayload("VPX_PART") != nullptr)
      {
         dropGroup = group;
         dropRequested = true;
      }
      ImGui::EndDragDropTarget();
   };
   // Part row: type icon, selectable with ctrl/shift multi selection (double click frames the part), drag source, drop target and eye toggle
   std::shared_ptr<EditorUIPart> rangeTarget; // Shift+click: applied once all the rows are known
   const auto renderPartLeaf = [this, &editor, &partDragSource, &partDropTarget, &rangeTarget](const std::shared_ptr<EditorUIPart> &edit, int &itemCounter, const std::shared_ptr<EditorUIPart> &revealPart, float eyeX)
   {
      IEditable *const editable = edit->GetEditable();
      m_listedParts.push_back(edit);
      const bool inactiveSel = editor.IsPartSelected(edit) && edit != editor.m_selection.GetPart();
      if (inactiveSel)
         PushInactiveSelectionColor();
      else
         PushSelectionColor();
      const bool clicked = ImGui::Selectable(("##Outliner"s + std::to_string(itemCounter++)).c_str(),
         editor.IsPartSelected(edit), ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick);
      ImGui::PopStyleColor();
      {
         // Type icon (dim) and name (normal text) drawn over the selectable
         const char *const icon = TypeIcon(editable->GetItemType());
         const ImVec2 textPos = ImGui::GetItemRectMin();
         ImDrawList *const drawList = ImGui::GetWindowDrawList();
         drawList->AddText(textPos, EditorTheme::TextDim, icon);
         const float nameX = textPos.x + ImGui::CalcTextSize(icon).x + 6.f;
         const float eyeScreenX = ImGui::GetWindowPos().x - ImGui::GetScrollX() + eyeX; // Where the eye button is placed by SameLine(eyeX)
         drawList->AddText(ImVec2(nameX, textPos.y), EditorTheme::Text, EllipsizeToWidth(editable->GetName(), eyeScreenX - ImGui::GetStyle().ItemSpacing.x - nameX).c_str());
      }
      if (clicked)
      {
         const ImGuiIO &selIO = ImGui::GetIO();
         if (selIO.KeyCtrl)
         {
            editor.TogglePartSelection(edit);
            editor.m_outlinerAnchor = edit;
         }
         else if (selIO.KeyShift)
            rangeTarget = edit;
         else
         {
            editor.SetSelection(Selection(edit));
            editor.m_outlinerAnchor = edit;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
               editor.FrameSelection(); // Blender's View Selected
         }
      }
      partDragSource(edit);
      partDropTarget(editable->GetPartGroup()); // Dropping on a part moves to its group (same as the Win32 layer dialog)
      if (edit == revealPart && !ImGui::IsItemVisible())
         ImGui::SetScrollHereY();
      if (editor.m_table->m_liveBaseTable == nullptr)
      {
         ImGui::SameLine(eyeX);
         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(editable->IsUIVisible(false) ? EditorTheme::TextDim : EditorTheme::TextFaint));
         ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
         if (ImGui::SmallButton(((editable->IsUIVisible(false) ? ICON_LC_EYE : ICON_LC_EYE_OFF) + "##Eye__"s + editable->GetName()).c_str()))
         {
            editable->SetUIVisible(!editable->IsUIVisible(false));
            editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         }
         ImGui::PopStyleColor(2);
      }
   };

   if (m_displayMode == 0)
   {
      // View Layer: the part groups (Blender's collections) tree
      const bool filterActive = !m_tokens.empty() || m_typeMask != AllTypesMask || m_selectedOnly;
      const bool openMatches = m_openMatchesPending && filterActive;
      m_openMatchesPending = false;
      m_listedParts.clear();
      ImGui::PushID("parts");
      const bool scenePartsOpened = ImGui::TreeNodeEx("Scene Parts", ImGuiTreeNodeFlags_DefaultOpen);
      partDropTarget(nullptr); // Dropping on the scene root ungroups part groups
      if (scenePartsOpened)
      {
         // Table definition parts
         struct Node
         {
            PartGroup *group;
            bool opened;
         };
         // One pass computing what is listed: parts must match the type mask, the selected only option and the search tokens (or be in a group
         // matching them). With any filter, groups without listed part are hidden. Also counts the listed parts of each group (current view mode only)
         const bool backdropView = editor.m_camMode == ViewMode::DesktopBackdrop;
         const bool hasTokens = !m_tokens.empty();
         m_matchedGroups.clear();
         m_visibleGroups.clear();
         m_groupCounts.clear();
         m_groupStack.clear();
         m_showPart.assign(editor.m_editables.size(), 0);
         for (size_t i = 0; i < editor.m_editables.size(); ++i)
         {
            const std::shared_ptr<EditorUIPart> &uiPart = editor.m_editables[i];
            const IEditable *const editable = uiPart->GetEditable();
            while (!m_groupStack.empty() && (editable->GetPartGroup() == nullptr || !editable->IsChild(m_groupStack.back())))
               m_groupStack.pop_back();
            if (editable->GetItemType() == eItemPartGroup)
            {
               if (hasTokens && MatchesPartCached(i))
               {
                  m_matchedGroups.insert(static_cast<const PartGroup *>(editable));
                  for (const PartGroup *ancestor : m_groupStack)
                     m_visibleGroups.insert(ancestor);
                  m_visibleGroups.insert(static_cast<const PartGroup *>(editable));
               }
               m_groupStack.push_back(static_cast<const PartGroup *>(editable));
               continue;
            }
            const bool listed = editable->m_desktopBackdrop == backdropView && (m_typeMask & (1u << CategoryOf(editable->GetItemType()))) != 0 && (!m_selectedOnly || editor.IsPartSelected(uiPart))
               && (!hasTokens || MatchesPartCached(i) || std::ranges::any_of(m_groupStack, [this](const PartGroup *group) { return m_matchedGroups.contains(group); }));
            m_showPart[i] = listed;
            if (!listed)
               continue;
            for (const PartGroup *ancestor : m_groupStack)
            {
               m_visibleGroups.insert(ancestor);
               ++m_groupCounts[ancestor];
            }
         }
         vector<Node> stack;
         int outlinerItem = 0;
         const float eyeX = ImGui::GetContentRegionAvail().x;
         // Highlight the group in which new and imported parts are added
         PartGroup *const newPartTarget = editor.GetPartGroupForNewPart();
         // When 'sync tree with selection' is enabled, reveal the newly selected part in the tree
         const std::shared_ptr<EditorUIPart> revealPart = (m_syncToSelection && editor.m_selection.GetPart() != m_lastSyncedPart) ? editor.m_selection.GetPart() : nullptr;
         m_lastSyncedPart = editor.m_selection.GetPart();
         for (size_t idx = 0; idx < editor.m_editables.size(); ++idx)
         {
            const auto &edit = editor.m_editables[idx];
            const PartGroup *parent = edit->GetEditable()->GetPartGroup();
            while (!stack.empty()
               && ((parent == nullptr && (edit->GetEditable()->GetItemType() == eItemPartGroup)) // Root partgroup: pop all
                  || (parent == nullptr && (edit->GetEditable()->GetItemType() != eItemPartGroup) && (stack.back().group != nullptr)) // Live object: pop all unless in 'Live Object' group
                  || (parent != nullptr && (!edit->GetEditable()->IsChild(stack.back().group))))) // Child object: pop up to the parent group
            {
               if (stack.back().opened)
                  ImGui::TreePop();
               stack.pop_back();
            }
            const bool isGroup = edit->GetEditable()->GetItemType() == eItemPartGroup;
            // With a filter, only display groups containing a listed part
            if (isGroup && filterActive && !m_visibleGroups.contains(static_cast<PartGroup *>(edit->GetEditable())))
               continue;
            // In desktop backdrop mode, only display backdrop parts, and conversely in the other view modes
            if (!isGroup && (edit->GetEditable()->m_desktopBackdrop != backdropView))
               continue;
            ImGui::AlignTextToFramePadding();
            if (isGroup)
            {
               PartGroup *group = static_cast<PartGroup *>(edit->GetEditable());
               bool opened = false;
               if (stack.empty() || stack.back().opened)
               {
                  if ((revealPart && revealPart->GetEditable()->IsChild(group)) || (openMatches && m_visibleGroups.contains(group)))
                     ImGui::SetNextItemOpen(true);
                  const bool isNewPartTarget = (group == newPartTarget);
                  const bool inactiveSel = editor.IsPartSelected(edit) && edit != editor.m_selection.GetPart();
                  if (inactiveSel)
                     PushInactiveSelectionColor();
                  else
                     PushSelectionColor();
                  // Folder icon and count are outside the ID (after ###) so that the node keeps its open state when they change
                  const auto countIt = m_groupCounts.find(group);
                  const string &groupName = edit->GetEditable()->GetName();
                  const string idPart = "###" + groupName;
                  const bool wasOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetID(idPart.c_str()), 0) != 0;
                  // Name shortened so that it ends before the count, the new part marker and the eye button
                  const string countText = std::to_string(countIt != m_groupCounts.end() ? countIt->second : 0);
                  const string iconPart = wasOpen ? ICON_LC_FOLDER_OPEN "  " : ICON_LC_FOLDER "  ";
                  const float arrowWidth = ImGui::CalcTextSize(ICON_LC_CIRCLE_ARROW_LEFT).x;
                  const float labelStartX = ImGui::GetCursorScreenPos().x + ImGui::GetTreeNodeToLabelSpacing();
                  const float labelEndX = ImGui::GetWindowPos().x - ImGui::GetScrollX() + eyeX - arrowWidth - style.ItemSpacing.x * 3.f - ImGui::CalcTextSize(countText.c_str()).x;
                  const string groupLabel = iconPart + EllipsizeToWidth(groupName, labelEndX - labelStartX - ImGui::CalcTextSize(iconPart.c_str()).x) + idPart;
                  opened = ImGui::TreeNodeEx(groupLabel.c_str(), ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanFullWidth | (editor.IsPartSelected(edit) ? ImGuiTreeNodeFlags_Selected : 0));
                  ImGui::PopStyleColor();
                  const bool rowHovered = ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                  partDragSource(edit);
                  partDropTarget(group);
                  if (ImGui::BeginPopupContextItem())
                  {
                     if (ImGui::MenuItem("Select"))
                        editor.SetSelection(Selection(edit));
                     if (ImGui::MenuItem("Select Contents"))
                        editor.SelectPartsInGroup(group);
                     ImGui::EndPopup();
                  }
                  {
                     // Part count (dim, right aligned before the eye column); the new part target marker is only shown on row hover
                     ImGui::SameLine(eyeX - arrowWidth - style.ItemSpacing.x * 2.f - ImGui::CalcTextSize(countText.c_str()).x);
                     ImGui::TextColored(EditorTheme::V(EditorTheme::TextDim), "%s", countText.c_str());
                     if (isNewPartTarget && rowHovered)
                     {
                        ImGui::SameLine(eyeX - arrowWidth - style.ItemSpacing.x);
                        ImGui::TextColored(EditorTheme::V(EditorTheme::TextDim), ICON_LC_CIRCLE_ARROW_LEFT);
                        if (ImGui::IsItemHovered())
                           ImGui::SetTooltip("New and imported parts are added to this group");
                     }
                  }
                  if (editor.m_table->m_liveBaseTable == nullptr)
                  {
                     ImGui::SameLine(eyeX);
                     ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(group->IsUIVisible(false) ? EditorTheme::TextDim : EditorTheme::TextFaint));
                     ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
                     if (ImGui::SmallButton(((group->IsUIVisible(false) ? ICON_LC_EYE : ICON_LC_EYE_OFF) + "##Eye__"s + edit->GetEditable()->GetName()).c_str()))
                     {
                        group->SetUIVisible(!group->IsUIVisible(false));
                        editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
                     }
                     ImGui::PopStyleColor(2);
                  }
                  if (edit == revealPart && !ImGui::IsItemVisible())
                     ImGui::SetScrollHereY();
               }
               stack.emplace_back(group, opened);
            }
            else
            {
               const bool show = m_showPart[idx] != 0;
               if (parent == nullptr && stack.empty() && show)
               {
                  if (revealPart && revealPart->GetEditable()->GetPartGroup() == nullptr)
                     ImGui::SetNextItemOpen(true);
                  stack.push_back({ nullptr, ImGui::TreeNodeEx("[Live Objects]", ImGuiTreeNodeFlags_AllowOverlap) });
                  partDropTarget(nullptr);
               }
               if (!stack.empty() && stack.back().opened && show)
                  renderPartLeaf(edit, outlinerItem, revealPart, eyeX);
            }
         }
         while (!stack.empty())
         {
            if (stack.back().opened)
               ImGui::TreePop();
            stack.pop_back();
         }
         ImGui::TreePop();
      }
      ImGui::PopID();
      if (rangeTarget)
      {
         // Range between the anchor and the clicked row, restricted to the rows listed (type filter, search, open groups)
         const auto anchorIt = std::ranges::find(m_listedParts, editor.m_outlinerAnchor);
         const auto targetIt = std::ranges::find(m_listedParts, rangeTarget);
         if (editor.m_outlinerAnchor == nullptr || anchorIt == m_listedParts.end() || targetIt == m_listedParts.end())
            editor.SetSelection(Selection(rangeTarget));
         else
         {
            editor.m_multiSel.assign(std::min(anchorIt, targetIt), std::max(anchorIt, targetIt) + 1);
            editor.m_selection = Selection(rangeTarget);
         }
      }
   }
   else
   {
      // Table Data: the table resources as twirl-down categories (Blender's 'Blender File' mode)
      ImGui::PushID("data");
      // Category label with the item count, the count is outside the ID
      char label[64];
      const auto categoryLabel = [&label](const char *name, size_t count) -> const char *
      {
         snprintf(label, sizeof(label), "%s (%d)###%s", name, (int)count, name);
         return label;
      };
      const std::function<string(Material *)> materialName = [](Material *material) -> string { return material->m_name; };
      const Selection::SelectionType selType = editor.m_selection.GetType();
      if (m_revealResource && selType == Selection::S_MATERIAL)
         ImGui::SetNextItemOpen(true);
      RenderResourceList(categoryLabel("Materials", editor.m_table->m_materials.size()), true, editor.m_table->m_materials, editor.m_multiSelMaterials, editor.m_outlinerMaterialAnchor, materialName);
      const std::function<string(Texture *)> imageName = [](Texture *image) -> string { return image->m_name; };
      if (m_revealResource && selType == Selection::S_IMAGE)
         ImGui::SetNextItemOpen(true);
      RenderResourceList(categoryLabel("Images", editor.m_table->m_vimage.size()), true, editor.m_table->m_vimage, editor.m_multiSelImages, editor.m_outlinerImageAnchor, imageName);
      const std::function<string(VPX::Sound *)> soundName = [](VPX::Sound *sound) -> string { return sound->GetName(); };
      if (m_revealResource && selType == Selection::S_SOUND)
         ImGui::SetNextItemOpen(true);
      RenderResourceList(categoryLabel("Sounds", editor.m_table->m_vsound.size()), true, editor.m_table->m_vsound, editor.m_multiSelSounds, editor.m_outlinerSoundAnchor, soundName);
      if (m_revealResource && selType == Selection::S_RENDERPROBE)
         ImGui::SetNextItemOpen(true);
      if (ImGui::TreeNode(categoryLabel("Render Probes", editor.m_table->m_vrenderprobe.size())))
      {
         int pos = 0;
         for (RenderProbe *probe : editor.m_table->m_vrenderprobe)
         {
            if (!MatchesFilter(probe->GetName()))
               continue;
            ImGui::PushID(pos++);
            Selection sel(probe);
            if (ImGui::Selectable(probe->GetName().c_str(), editor.m_selection == sel))
               editor.SetSelection(sel);
            if (m_revealResource && editor.m_selection == sel && !ImGui::IsItemVisible())
               ImGui::SetScrollHereY();
            ImGui::PopID();
         }
         ImGui::TreePop();
      }
      // Collections can only be edited on the base table (the inspected table is a live copy)
      const bool canEditCollections = editor.m_table->m_liveBaseTable == nullptr;
      if (m_revealResource && selType == Selection::S_COLLECTION)
         ImGui::SetNextItemOpen(true);
      const bool collectionsOpened = ImGui::TreeNode(categoryLabel("Collections", editor.m_table->GetCollections().size()));
      if (ImGui::BeginPopupContextItem())
      {
         ImGui::BeginDisabled(!canEditCollections);
         if (ImGui::MenuItem("New Collection"))
            editor.CreateCollection(false);
         const bool hasScriptableSel = std::ranges::any_of(
            editor.m_multiSel, [](const std::shared_ptr<EditorUIPart> &part) { return part->GetEditable() != nullptr && part->GetEditable()->GetIScriptable() != nullptr; });
         ImGui::BeginDisabled(!hasScriptableSel);
         if (ImGui::MenuItem("New Collection From Selection"))
            editor.CreateCollection(true);
         ImGui::EndDisabled();
         ImGui::EndDisabled();
         ImGui::EndPopup();
      }
      if (collectionsOpened)
      {
         // Collections are listed in their storage order as it is user defined and relevant to scripts
         for (Collection *const collection : editor.m_table->GetCollections())
         {
            if (!MatchesFilter(collection->m_name))
               continue;
            const Selection sel(collection);
            if (ImGui::Selectable((collection->m_name + "##collection"s).c_str(), editor.m_selection == sel))
               editor.SetSelection(sel);
            if (m_revealResource && editor.m_selection == sel && !ImGui::IsItemVisible())
               ImGui::SetScrollHereY();
         }
         ImGui::TreePop();
      }
      if (m_revealResource && selType == Selection::S_CAMERA)
         ImGui::SetNextItemOpen(true);
      if (ImGui::TreeNodeEx("View Setups"))
      {
         if (ImGui::Selectable("Editor Camera"))
         {
            editor.ClearSelection();
            editor.m_table->SetViewSetupOverride(BG_DESKTOP);
            editor.m_table->SetViewSetupOverride(BG_INVALID);
            editor.m_camMode = ViewMode::EditorCam;
         }
         static constexpr const char *previewNames[] = { "Preview: Desktop", "Preview: Cabinet", "Preview: Full Single Screen" };
         for (int cam = 0; cam < 3; ++cam)
            if (ImGui::Selectable(previewNames[cam], editor.m_selection == Selection::Camera(cam)))
               SelectViewSetup(cam);
         ImGui::TreePop();
      }
      m_revealResource = false;
      ImGui::PopID();
   }
   if (dropRequested)
      editor.MoveSelectionToPartGroup(dropGroup);

   ImGui::End();
   ImGui::PopStyleVar(3);
}

template <class T>
void OutlinerPanel::RenderResourceList(const char *label, bool asTree, vector<T *> &items, vector<T *> &multiSel, T *&anchor, const std::function<string(T *)> &nameOf)
{
   if (asTree && !ImGui::TreeNode(label))
      return;
   EditorUI &editor = m_editor;
   const std::vector<T *> sorted = SortedCaseInsensitive(items, nameOf);
   int pos = 0;
   for (T *const item : sorted)
   {
      if (!MatchesFilter(nameOf(item)))
         continue;
      ImGui::PushID(pos++);
      const bool selected = std::ranges::find(multiSel, item) != multiSel.end();
      const bool inactiveSel = selected && editor.m_selection != Selection(item);
      if (inactiveSel)
         PushInactiveSelectionColor();
      const bool clicked = ImGui::Selectable(nameOf(item).c_str(), selected);
      if (inactiveSel)
         ImGui::PopStyleColor();
      if (m_revealResource && editor.m_selection == Selection(item) && !ImGui::IsItemVisible())
         ImGui::SetScrollHereY();
      ImGui::PopID();
      if (!clicked)
         continue;
      const ImGuiIO &selIO = ImGui::GetIO();
      if (selIO.KeyCtrl)
      {
         const auto it = std::ranges::find(multiSel, item);
         if (it == multiSel.end())
         {
            multiSel.push_back(item);
            editor.m_selection = Selection(item);
         }
         else
         {
            const bool wasActive = editor.m_selection == Selection(item);
            multiSel.erase(it);
            if (multiSel.empty())
               editor.m_selection = Selection();
            else if (wasActive)
               editor.m_selection = Selection(multiSel.back());
         }
         anchor = item;
      }
      else if (selIO.KeyShift && anchor != nullptr)
      {
         // Range selection between the anchor and the clicked item, restricted to the visible items
         vector<T *> visible;
         for (T *const v : sorted)
            if (MatchesFilter(nameOf(v)))
               visible.push_back(v);
         const auto anchorIt = std::ranges::find(visible, anchor);
         const auto itemIt = std::ranges::find(visible, item);
         if (anchorIt == visible.end() || itemIt == visible.end())
         {
            editor.SetSelection(Selection(item));
            anchor = item;
         }
         else
         {
            multiSel.clear();
            for (auto it = std::min(anchorIt, itemIt); it != std::max(anchorIt, itemIt) + 1; ++it)
               multiSel.push_back(*it);
            editor.m_selection = Selection(item);
         }
      }
      else
      {
         editor.SetSelection(Selection(item));
         anchor = item;
      }
   }
   if (asTree)
      ImGui::TreePop();
}
}
