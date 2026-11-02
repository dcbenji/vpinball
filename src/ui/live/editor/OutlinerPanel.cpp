// license:GPLv3+

#include "core/stdafx.h"
#include "OutlinerPanel.h"

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
   const ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Header);
   ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(color.x, color.y, color.z, color.w * 0.4f));
}

namespace
{
enum Tab
{
   TabParts,
   TabLights,
   TabMaterials,
   TabImages,
   TabProbes,
   TabCollections,
   TabMore,
   TabCount
};

constexpr int MaxRecentSelections = 5;

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

const char *TypeName(ItemTypeEnum type)
{
   const TypeInfo *info = FindTypeInfo(type);
   return info ? info->name : "Part";
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

bool IsLightLike(const IEditable *editable)
{
   return editable->GetItemType() == eItemLight || editable->GetItemType() == eItemFlasher;
}

string SelectionName(const Selection &sel)
{
   switch (sel.GetType())
   {
   case Selection::S_CAMERA:
   {
      static constexpr const char *names[] = { "Preview: Desktop", "Preview: Cabinet", "Preview: Full Single Screen" };
      const int cam = sel.GetCamera();
      return (cam >= 0 && cam < 3) ? names[cam] : "Preview";
   }
   case Selection::S_MATERIAL: return sel.GetMaterial()->m_name;
   case Selection::S_IMAGE: return sel.GetImage()->m_name;
   case Selection::S_EDITABLE: return sel.GetPart()->GetEditable()->GetName();
   case Selection::S_RENDERPROBE: return sel.GetProbe()->GetName();
   case Selection::S_SOUND: return sel.GetSound()->GetName();
   case Selection::S_COLLECTION: return sel.GetCollection()->m_name;
   default: return string();
   }
}

const char *SelectionKind(const Selection &sel)
{
   switch (sel.GetType())
   {
   case Selection::S_CAMERA: return "View Setup";
   case Selection::S_MATERIAL: return "Material";
   case Selection::S_IMAGE: return "Image";
   case Selection::S_EDITABLE: return TypeName(sel.GetPart()->GetEditable()->GetItemType());
   case Selection::S_RENDERPROBE: return "Render Probe";
   case Selection::S_SOUND: return "Sound";
   case Selection::S_COLLECTION: return "Collection";
   default: return "";
   }
}

// Tab showing the selected object, -1 to stay on the current one
int TabForSelection(const Selection &sel, int activeTab)
{
   switch (sel.GetType())
   {
   case Selection::S_MATERIAL: return TabMaterials;
   case Selection::S_IMAGE: return TabImages;
   case Selection::S_RENDERPROBE: return TabProbes;
   case Selection::S_COLLECTION: return TabCollections;
   case Selection::S_SOUND:
   case Selection::S_CAMERA: return TabMore;
   case Selection::S_EDITABLE:
      if (IsLightLike(sel.GetPart()->GetEditable()))
         return (activeTab == TabParts || activeTab == TabLights) ? -1 : TabLights;
      return activeTab == TabParts ? -1 : TabParts;
   default: return -1;
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
   m_openMatchesPending = !m_tokens.empty();
}

bool OutlinerPanel::MatchesFilter(const string &name) const
{
   if (m_tokens.names.empty())
      return true;
   const string name_lcase = lowerCase(name);
   return std::ranges::all_of(m_tokens.names, [&name_lcase](const string &token) { return name_lcase.find(token) != std::string::npos; });
}

bool OutlinerPanel::MatchesPart(const IEditable *editable) const
{
   if (m_tokens.empty())
      return true;
   if (!MatchesFilter(editable->GetName()))
      return false;
   if (!m_tokens.types.empty())
   {
      const TypeInfo *info = FindTypeInfo(editable->GetItemType());
      const char *name = info ? info->name : "Part";
      const char *alias = info ? info->alias : "";
      if (!std::ranges::all_of(m_tokens.types, [name, alias](const string &token) { return StartsWithNoCase(name, token) || StartsWithNoCase(alias, token); }))
         return false;
   }
   if (!m_tokens.materials.empty())
   {
      const string material = lowerCase(PropertiesPanel::PartMaterial(editable));
      if (!std::ranges::all_of(m_tokens.materials, [&material](const string &token) { return material.find(token) != std::string::npos; }))
         return false;
   }
   return true;
}

void OutlinerPanel::UpdateSelectionHistory()
{
   EditorUI &editor = m_editor;
   const PinTable *const table = editor.m_table;
   // Drop the objects that do not exist anymore (only compares pointers, nothing is dereferenced)
   std::erase_if(m_recentSelections,
      [&](const Selection &sel)
      {
         switch (sel.GetType())
         {
         case Selection::S_MATERIAL: return std::ranges::find(table->m_materials, sel.GetMaterial()) == table->m_materials.end();
         case Selection::S_IMAGE: return std::ranges::find(table->m_vimage, sel.GetImage()) == table->m_vimage.end();
         case Selection::S_RENDERPROBE: return std::ranges::find(table->m_vrenderprobe, sel.GetProbe()) == table->m_vrenderprobe.end();
         case Selection::S_SOUND: return std::ranges::find(table->m_vsound, sel.GetSound()) == table->m_vsound.end();
         case Selection::S_COLLECTION:
         {
            const Collection *const collection = sel.GetCollection();
            return std::ranges::none_of(table->GetCollections(), [collection](const Collection *c) { return c == collection; });
         }
         case Selection::S_EDITABLE:
         {
            const auto &part = std::get<std::shared_ptr<EditorUIPart>>(sel.payload);
            return std::ranges::find(editor.m_editables, part) == editor.m_editables.end();
         }
         default: return false;
         }
      });
   if (editor.m_selection == m_lastSeenSelection)
      return;
   m_lastSeenSelection = editor.m_selection;
   if (editor.m_selection.GetType() == Selection::S_NONE)
      return;
   std::erase(m_recentSelections, editor.m_selection);
   m_recentSelections.insert(m_recentSelections.begin(), editor.m_selection);
   if (m_recentSelections.size() > MaxRecentSelections + 1) // The current selection is part of the list but not displayed
      m_recentSelections.resize(MaxRecentSelections + 1);
   if (m_syncToSelection)
   {
      const int tab = TabForSelection(editor.m_selection, m_activeTab);
      if (tab >= 0)
         m_pendingTab = tab;
   }
}

void OutlinerPanel::RenderContextHeader()
{
   EditorUI &editor = m_editor;
   const Selection &selection = editor.m_selection;
   const ImGuiStyle &style = ImGui::GetStyle();

   // Breadcrumb of the selection
   m_crumbs.clear();
   const auto resourceCrumb = [this, &selection](const char *list, int tab)
   {
      m_crumbs.push_back({ list, 3, Selection(), nullptr, tab });
      m_crumbs.push_back({ SelectionName(selection), 0, Selection(), nullptr, 0 });
   };
   switch (selection.GetType())
   {
   case Selection::S_EDITABLE:
   {
      m_crumbs.push_back({ "Table", 1, Selection(), nullptr, 0 });
      const IEditable *const editable = selection.GetPart()->GetEditable();
      const size_t first = m_crumbs.size();
      for (const PartGroup *group = editable->GetPartGroup(); group != nullptr; group = static_cast<const IEditable *>(group)->GetPartGroup())
         m_crumbs.push_back({ static_cast<const IEditable *>(group)->GetName(), 4, Selection(), group, 0 });
      std::reverse(m_crumbs.begin() + first, m_crumbs.end());
      m_crumbs.push_back({ editable->GetName(), 0, Selection(), nullptr, 0 });
      break;
   }
   case Selection::S_MATERIAL: resourceCrumb("Materials", TabMaterials); break;
   case Selection::S_IMAGE: resourceCrumb("Images", TabImages); break;
   case Selection::S_RENDERPROBE: resourceCrumb("Render Probes", TabProbes); break;
   case Selection::S_COLLECTION: resourceCrumb("Collections", TabCollections); break;
   case Selection::S_SOUND: resourceCrumb("Sounds", TabMore); break;
   case Selection::S_CAMERA: resourceCrumb("View Setups", TabMore); break;
   default: break;
   }
   if (!m_crumbs.empty())
   {
      // Elide from the left when the breadcrumb does not fit
      const float gap = style.ItemInnerSpacing.x;
      const float sepWidth = ImGui::CalcTextSize(ICON_FK_ANGLE_RIGHT).x + gap * 2.f;
      const float ellipsisWidth = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2.f + gap;
      const auto crumbWidth = [&](const Crumb &crumb) { return ImGui::CalcTextSize(crumb.label.c_str()).x + style.FramePadding.x * 2.f + gap; };
      const float avail = ImGui::GetContentRegionAvail().x;
      size_t start = 0;
      float total = 0.f;
      for (const Crumb &crumb : m_crumbs)
         total += crumbWidth(crumb) + sepWidth;
      total -= sepWidth;
      while (start + 1 < m_crumbs.size() && total + (start > 0 ? ellipsisWidth + sepWidth : 0.f) > avail)
      {
         total -= crumbWidth(m_crumbs[start]) + sepWidth;
         ++start;
      }
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
      bool first = true;
      if (start > 0)
      {
         ImGui::TextDisabled("...");
         first = false;
      }
      for (size_t i = start; i < m_crumbs.size(); ++i)
      {
         const Crumb &crumb = m_crumbs[i];
         if (!first)
         {
            ImGui::SameLine(0.f, gap);
            ImGui::TextDisabled("%s", ICON_FK_ANGLE_RIGHT);
            ImGui::SameLine(0.f, gap);
         }
         first = false;
         if (crumb.kind == 0)
         {
            ImGui::TextUnformatted(crumb.label.c_str());
            continue;
         }
         ImGui::PushID((int)i);
         if (ImGui::SmallButton(crumb.label.c_str()))
         {
            if (crumb.kind == 1)
               editor.ClearSelection();
            else if (crumb.kind == 3)
               m_pendingTab = crumb.tab;
            else if (crumb.kind == 4)
            {
               const auto it = std::ranges::find_if(editor.m_editables, [&crumb](const std::shared_ptr<EditorUIPart> &part) { return part->GetEditable() == static_cast<const IEditable *>(crumb.group); });
               if (it != editor.m_editables.end())
                  editor.SetSelection(Selection(*it));
            }
         }
         ImGui::PopID();
      }
      ImGui::PopStyleColor();
   }

   // Recent selections, as a wrapping row of buttons
   const float maxX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
   int shown = 0;
   for (size_t i = 0; i < m_recentSelections.size() && shown < MaxRecentSelections; ++i)
   {
      const Selection &recent = m_recentSelections[i];
      if (recent == selection)
         continue;
      const string name = SelectionName(recent);
      const float width = ImGui::CalcTextSize(name.c_str()).x + style.FramePadding.x * 2.f;
      if (shown > 0 && ImGui::GetItemRectMax().x + style.ItemSpacing.x + width < maxX)
         ImGui::SameLine();
      ImGui::PushID((int)i);
      if (ImGui::SmallButton(name.c_str()))
         editor.SetSelection(recent);
      ImGui::PopID();
      ImGui::SetItemTooltip("%s", SelectionKind(recent));
      ++shown;
   }
   if (!m_crumbs.empty() || shown > 0)
      ImGui::Separator();
}

void OutlinerPanel::Render(float topBarHeight)
{
   EditorUI &editor = m_editor;
   if (editor.m_table->IsLocked())
      return;

   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   const float pane_width = PaneWidth * editor.m_liveUI.GetDPI();
   ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + topBarHeight));
   ImGui::SetNextWindowSize(ImVec2(pane_width, viewport->Size.y - topBarHeight));
   constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
      | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * editor.m_liveUI.GetDPI(), 4.0f * editor.m_liveUI.GetDPI()));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   ImGui::Begin("OUTLINER", nullptr, window_flags);
   {
      const bool popStyle = m_syncToSelection;
      if (popStyle)
         ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button(ICON_FK_LINK "##SyncTree"))
      {
         m_syncToSelection = !m_syncToSelection;
         m_lastSyncedPart = nullptr; // Reveal the current selection on next render
      }
      if (popStyle)
         ImGui::PopStyleColor();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Sync tree with selection");
   }
   {
      ImGui::SameLine();
      const float clearFilterWidth = ImGui::CalcTextSize(ICON_FK_TIMES).x + ImGui::GetStyle().FramePadding.x * 2.0f;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearFilterWidth - ImGui::GetStyle().ItemSpacing.x);
      ImGui::InputTextWithHint("##OutlinerFilter", "Filter: name t:type m:material", &m_filter);
   }
   {
      ImGui::SameLine();
      ImGui::BeginDisabled(m_filter.empty());
      if (ImGui::Button(ICON_FK_TIMES "##ClearOutlinerFilter"))
         m_filter.clear();
      ImGui::EndDisabled();
   }
   ParseFilter();
   UpdateSelectionHistory();
   const int forcedTab = m_pendingTab; // Requests made while drawing apply on next frame
   m_pendingTab = -1;

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
   // Part row shared by the parts tree and the lights list: selectable with ctrl/shift multi selection, drag source, drop target and eye toggle
   const auto renderPartLeaf = [&editor, &partDragSource, &partDropTarget](const std::shared_ptr<EditorUIPart> &edit, int &itemCounter, const std::shared_ptr<EditorUIPart> &revealPart, float eyeX)
   {
      const bool inactiveSel = editor.IsPartSelected(edit) && edit != editor.m_selection.GetPart();
      if (inactiveSel)
         PushInactiveSelectionColor();
      const bool clicked
         = ImGui::Selectable((edit->GetEditable()->GetName() + "##Outliner"s + std::to_string(itemCounter++)).c_str(), editor.IsPartSelected(edit), ImGuiSelectableFlags_AllowOverlap);
      if (inactiveSel)
         ImGui::PopStyleColor();
      if (clicked)
      {
         const ImGuiIO &selIO = ImGui::GetIO();
         if (selIO.KeyCtrl)
         {
            editor.TogglePartSelection(edit);
            editor.m_outlinerAnchor = edit;
         }
         else if (selIO.KeyShift)
            editor.SelectOutlinerRange(edit);
         else
         {
            editor.SetSelection(Selection(edit));
            editor.m_outlinerAnchor = edit;
         }
      }
      partDragSource(edit);
      partDropTarget(edit->GetEditable()->GetPartGroup()); // Dropping on a part moves to its group (same as the Win32 layer dialog)
      if (edit == revealPart && !ImGui::IsItemVisible())
         ImGui::SetScrollHereY();
      IEditable *editable = edit->GetEditable();
      if (editable && editor.m_table->m_liveBaseTable == nullptr)
      {
         ImGui::SameLine(eyeX);
         ImGui::PushStyleColor(ImGuiCol_Text, editable->IsUIVisible(false) ? IM_COL32_WHITE : IM_COL32(128, 128, 128, 255));
         if (ImGui::SmallButton(((editable->IsUIVisible(false) ? ICON_FK_EYE : ICON_FK_EYE_SLASH) + "##Eye__"s + editable->GetName()).c_str()))
         {
            editable->SetUIVisible(!editable->IsUIVisible(false));
            editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
         }
         ImGui::PopStyleColor();
      }
   };
   // When 'sync tree with selection' is enabled, the newly selected part is revealed in the tree (taken once the list is actually drawn)
   const auto takeRevealPart = [this, &editor]() -> std::shared_ptr<EditorUIPart>
   {
      std::shared_ptr<EditorUIPart> reveal = (m_syncToSelection && editor.m_selection.GetPart() != m_lastSyncedPart) ? editor.m_selection.GetPart() : nullptr;
      m_lastSyncedPart = editor.m_selection.GetPart();
      return reveal;
   };
   const auto tabFlags = [forcedTab](int tab) { return forcedTab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };

   if (ImGui::BeginTabBar("##OutlinerTabs", ImGuiTabBarFlags_FittingPolicyScroll))
   {
      if (ImGui::BeginTabItem("Parts", nullptr, tabFlags(TabParts)))
      {
         m_activeTab = TabParts;
         ImGui::PushID("parts");
         RenderContextHeader();
         const bool filterActive = !m_tokens.empty();
         const bool openMatches = m_openMatchesPending && filterActive;
         m_openMatchesPending = false;
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
            // One pass computing what is listed: when a filter is applied, only display groups that match it or contain a matching
            // part in their subtree, and count the listed parts of each group (the parts of the current view mode only)
            const bool backdropView = editor.m_camMode == ViewMode::DesktopBackdrop;
            m_matchedGroups.clear();
            m_visibleGroups.clear();
            m_groupCounts.clear();
            m_groupStack.clear();
            m_showPart.assign(editor.m_editables.size(), 0);
            for (size_t i = 0; i < editor.m_editables.size(); ++i)
            {
               const IEditable *const editable = editor.m_editables[i]->GetEditable();
               while (!m_groupStack.empty() && (editable->GetPartGroup() == nullptr || !editable->IsChild(m_groupStack.back())))
                  m_groupStack.pop_back();
               const bool isGroup = editable->GetItemType() == eItemPartGroup;
               const bool listed = MatchesPart(editable) || (!isGroup && std::ranges::any_of(m_groupStack, [this](const PartGroup *group) { return m_matchedGroups.contains(group); }));
               if (listed)
               {
                  if (filterActive)
                  {
                     for (const PartGroup *ancestor : m_groupStack)
                        m_visibleGroups.insert(ancestor);
                     if (isGroup)
                     {
                        m_matchedGroups.insert(static_cast<const PartGroup *>(editable));
                        m_visibleGroups.insert(static_cast<const PartGroup *>(editable));
                     }
                  }
                  if (!isGroup && editable->m_desktopBackdrop == backdropView)
                     for (const PartGroup *ancestor : m_groupStack)
                        ++m_groupCounts[ancestor];
               }
               m_showPart[i] = !isGroup && listed;
               if (isGroup)
                  m_groupStack.push_back(static_cast<const PartGroup *>(editable));
            }
            vector<Node> stack;
            int outlinerItem = 0;
            const float eyeX = ImGui::GetContentRegionAvail().x;
            // Highlight the group in which new and imported parts are added
            PartGroup *const newPartTarget = editor.GetPartGroupForNewPart();
            const ImVec4 newPartTargetColor(1.0f, 0.6f, 0.2f, 1.0f);
            const std::shared_ptr<EditorUIPart> revealPart = takeRevealPart();
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
               // TODO allow selection => ImGuiTreeNodeFlags_Selected
               // TODO support empty nodes => ImGuiTreeNodeFlags_Leaf
               if (edit->GetEditable()->GetItemType() == eItemPartGroup && filterActive && !m_visibleGroups.contains(static_cast<PartGroup *>(edit->GetEditable())))
                  continue;
               // In desktop backdrop mode, only display backdrop parts, and conversely in the other view modes
               if (edit->GetEditable()->GetItemType() != eItemPartGroup && (edit->GetEditable()->m_desktopBackdrop != backdropView))
                  continue;
               ImGui::AlignTextToFramePadding();
               if (edit->GetEditable()->GetItemType() == eItemPartGroup)
               {
                  PartGroup *group = static_cast<PartGroup *>(edit->GetEditable());
                  bool opened = false;
                  if (stack.empty() || stack.back().opened)
                  {
                     if ((revealPart && revealPart->GetEditable()->IsChild(group)) || (openMatches && m_visibleGroups.contains(group)))
                        ImGui::SetNextItemOpen(true);
                     const bool isNewPartTarget = (group == newPartTarget);
                     if (isNewPartTarget)
                        ImGui::PushStyleColor(ImGuiCol_Text, newPartTargetColor);
                     const bool inactiveSel = editor.IsPartSelected(edit) && edit != editor.m_selection.GetPart();
                     if (inactiveSel)
                        PushInactiveSelectionColor();
                     // The count is not part of the ID (after ###) so that the node keeps its open state when it changes
                     const auto countIt = m_groupCounts.find(group);
                     const string &groupName = edit->GetEditable()->GetName();
                     const string groupLabel = groupName + " (" + std::to_string(countIt != m_groupCounts.end() ? countIt->second : 0) + ")###" + groupName;
                     opened = ImGui::TreeNodeEx(groupLabel.c_str(), ImGuiTreeNodeFlags_AllowOverlap | (editor.IsPartSelected(edit) ? ImGuiTreeNodeFlags_Selected : 0));
                     if (inactiveSel)
                        ImGui::PopStyleColor();
                     if (isNewPartTarget)
                        ImGui::PopStyleColor();
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
                     if (isNewPartTarget)
                     {
                        ImGui::SameLine(eyeX - ImGui::CalcTextSize(ICON_FK_ARROW_CIRCLE_O_LEFT).x - ImGui::GetStyle().ItemSpacing.x);
                        ImGui::TextColored(newPartTargetColor, ICON_FK_ARROW_CIRCLE_O_LEFT);
                        if (ImGui::IsItemHovered())
                           ImGui::SetTooltip("New and imported parts are added to this group");
                     }
                     if (editor.m_table->m_liveBaseTable == nullptr)
                     {
                        ImGui::SameLine(eyeX);
                        ImGui::PushStyleColor(ImGuiCol_Text, group->IsUIVisible(false) ? IM_COL32_WHITE : IM_COL32(128, 128, 128, 255));
                        if (ImGui::SmallButton(((group->IsUIVisible(false) ? ICON_FK_EYE : ICON_FK_EYE_SLASH) + "##Eye__"s + edit->GetEditable()->GetName()).c_str()))
                        {
                           group->SetUIVisible(!group->IsUIVisible(false));
                           editor.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
                        }
                        ImGui::PopStyleColor();
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
         ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Lights", nullptr, tabFlags(TabLights)))
      {
         m_activeTab = TabLights;
         ImGui::PushID("lights");
         RenderContextHeader();
         const bool backdropView = editor.m_camMode == ViewMode::DesktopBackdrop;
         m_lightList.clear();
         for (const auto &edit : editor.m_editables)
         {
            const IEditable *const editable = edit->GetEditable();
            if (IsLightLike(editable) && editable->m_desktopBackdrop == backdropView && MatchesPart(editable))
               m_lightList.push_back(edit);
         }
         std::ranges::sort(m_lightList, [](const std::shared_ptr<EditorUIPart> &a, const std::shared_ptr<EditorUIPart> &b)
            { return StrLessNoCase(a->GetEditable()->GetName(), b->GetEditable()->GetName()); });
         const float eyeX = ImGui::GetContentRegionAvail().x;
         const std::shared_ptr<EditorUIPart> revealPart = takeRevealPart();
         int outlinerItem = 0;
         for (const auto &edit : m_lightList)
         {
            ImGui::AlignTextToFramePadding();
            renderPartLeaf(edit, outlinerItem, revealPart, eyeX);
         }
         ImGui::PopID();
         ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Materials", nullptr, tabFlags(TabMaterials)))
      {
         m_activeTab = TabMaterials;
         ImGui::PushID("materials");
         RenderContextHeader();
         const std::function<string(Material *)> materialName = [](Material *material) -> string { return material->m_name; };
         RenderResourceList("Materials", false, editor.m_table->m_materials, editor.m_multiSelMaterials, editor.m_outlinerMaterialAnchor, materialName);
         ImGui::PopID();
         ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Images", nullptr, tabFlags(TabImages)))
      {
         m_activeTab = TabImages;
         ImGui::PushID("images");
         RenderContextHeader();
         const std::function<string(Texture *)> imageName = [](Texture *image) -> string { return image->m_name; };
         RenderResourceList("Images", false, editor.m_table->m_vimage, editor.m_multiSelImages, editor.m_outlinerImageAnchor, imageName);
         ImGui::PopID();
         ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Probes", nullptr, tabFlags(TabProbes)))
      {
         m_activeTab = TabProbes;
         ImGui::PushID("probes");
         RenderContextHeader();
         int pos = 0;
         for (RenderProbe *probe : editor.m_table->m_vrenderprobe)
         {
            if (!MatchesFilter(probe->GetName()))
               continue;
            ImGui::PushID(pos++);
            Selection sel(probe);
            if (ImGui::Selectable(probe->GetName().c_str(), editor.m_selection == sel))
               editor.SetSelection(sel);
            ImGui::PopID();
         }
         ImGui::PopID();
         ImGui::EndTabItem();
      }

      const bool collectionsOpened = ImGui::BeginTabItem("Collections", nullptr, tabFlags(TabCollections));
      // Collections can only be edited on the base table (the inspected table is a live copy)
      const bool canEditCollections = editor.m_table->m_liveBaseTable == nullptr;
      const auto hasScriptableSelection = [&editor]()
      {
         return std::ranges::any_of(
            editor.m_multiSel, [](const std::shared_ptr<EditorUIPart> &part) { return part->GetEditable() != nullptr && part->GetEditable()->GetIScriptable() != nullptr; });
      };
      if (ImGui::BeginPopupContextItem())
      {
         ImGui::BeginDisabled(!canEditCollections);
         if (ImGui::MenuItem("New Collection"))
            editor.CreateCollection(false);
         ImGui::BeginDisabled(!hasScriptableSelection());
         if (ImGui::MenuItem("New Collection From Selection"))
            editor.CreateCollection(true);
         ImGui::EndDisabled();
         ImGui::EndDisabled();
         ImGui::EndPopup();
      }
      if (collectionsOpened)
      {
         m_activeTab = TabCollections;
         ImGui::PushID("collections");
         RenderContextHeader();
         ImGui::BeginDisabled(!canEditCollections);
         if (ImGui::SmallButton("New"))
            editor.CreateCollection(false);
         ImGui::SameLine();
         ImGui::BeginDisabled(!hasScriptableSelection());
         if (ImGui::SmallButton("New From Selection"))
            editor.CreateCollection(true);
         ImGui::EndDisabled();
         ImGui::EndDisabled();
         // Collections are listed in their storage order as it is user defined and relevant to scripts
         for (Collection *const collection : editor.m_table->GetCollections())
         {
            if (!MatchesFilter(collection->m_name))
               continue;
            const Selection sel(collection);
            if (ImGui::Selectable((collection->m_name + "##collection"s).c_str(), editor.m_selection == sel))
               editor.SetSelection(sel);
         }
         ImGui::PopID();
         ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("More", nullptr, tabFlags(TabMore)))
      {
         m_activeTab = TabMore;
         ImGui::PushID("more");
         RenderContextHeader();
         if (ImGui::TreeNodeEx("View Setups"))
         {
            if (ImGui::Selectable("Editor Camera"))
            {
               editor.ClearSelection();
               editor.m_table->SetViewSetupOverride(BG_DESKTOP);
               editor.m_table->SetViewSetupOverride(BG_INVALID);
               editor.m_camMode = ViewMode::EditorCam;
            }
            const Selection cam0 = Selection::Camera(0);
            if (ImGui::Selectable("Preview: Desktop", editor.m_selection == cam0))
            {
               editor.SetSelection(cam0);
               editor.m_camMode = ViewMode::PreviewCam;
               editor.m_table->SetViewSetupOverride(BG_DESKTOP);
            }
            const Selection cam1 = Selection::Camera(1);
            if (ImGui::Selectable("Preview: Cabinet", editor.m_selection == cam1))
            {
               editor.SetSelection(cam1);
               editor.m_camMode = ViewMode::PreviewCam;
               editor.m_table->SetViewSetupOverride(BG_FULLSCREEN);
            }
            const Selection cam2 = Selection::Camera(2);
            if (ImGui::Selectable("Preview: Full Single Screen", editor.m_selection == cam2))
            {
               editor.SetSelection(cam2);
               editor.m_camMode = ViewMode::PreviewCam;
               editor.m_table->SetViewSetupOverride(BG_FSS);
            }
            ImGui::TreePop();
         }
         const std::function<string(VPX::Sound *)> soundName = [](VPX::Sound *sound) -> string { return sound->GetName(); };
         RenderResourceList("Sounds", true, editor.m_table->m_vsound, editor.m_multiSelSounds, editor.m_outlinerSoundAnchor, soundName);
         ImGui::PopID();
         ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
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
