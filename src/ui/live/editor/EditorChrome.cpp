// license:GPLv3+

#include "core/stdafx.h"
#include "EditorChrome.h"
#include "IconsLucide.h"
#include "EditorTheme.h"

#include "core/FileLocator.h"
#include "core/VPApp.h"
#include "renderer/Sampler.h"
#include "renderer/Texture.h"
#include "ui/EditorClipboard.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "parts/light.h"

namespace VPX::EditorUI
{

namespace
{
constexpr float ToolButtonSize = 28.f; // Toolbar button (square), in pixels at 100% DPI
constexpr float ToolGroupGap = 8.f; // Gap between button groups (holds the 1 px separator)
constexpr float ToolSeparatorHeight = 16.f;
float s_toolDpi = 1.f;

// Toolbar icon button: transparent, hovered = Hover, active (mode on) = Raised background with Accent icon
bool ToolButton(const char *icon, bool active = false)
{
   ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(active ? EditorTheme::Raised : 0u));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(EditorTheme::Hover));
   ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(EditorTheme::Hover));
   ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(active ? EditorTheme::Accent : EditorTheme::Text));
   const bool pressed = ImGui::Button(icon, ImVec2(ToolButtonSize * s_toolDpi, ToolButtonSize * s_toolDpi));
   ImGui::PopStyleColor(4);
   return pressed;
}

// Text drawn over the render (view name, tool name): 1 px drop shadow so that it stays readable on light renders
void ShadowedText(const char *text)
{
   const ImVec2 pos = ImGui::GetCursorScreenPos();
   ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), EditorTheme::TextShadow, text);
   ImGui::TextUnformatted(text);
}

// Group separator: 1 px vertical line centered in the group gap
void ToolSeparator()
{
   const float half = 0.5f * (ToolGroupGap - 1.f) * s_toolDpi;
   ImGui::SameLine(0.f, half);
   const ImVec2 pos = ImGui::GetCursorScreenPos();
   const float inset = 0.5f * (ToolButtonSize - ToolSeparatorHeight) * s_toolDpi;
   ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + inset), ImVec2(pos.x, pos.y + inset + ToolSeparatorHeight * s_toolDpi), EditorTheme::Border, 1.f);
   ImGui::Dummy(ImVec2(1.f, ToolButtonSize * s_toolDpi));
   ImGui::SameLine(0.f, half);
}
}

void EditorChrome::Render(bool showFullUI)
{
   // Gives some transparency when positioning camera to better view camera view bounds
   // TODO for some reasons, this breaks the modal background behavior
   //SetupImGuiStyle(m_editor.m_selection.GetType() == Selection::S_CAMERA ? 0.3f : 1.0f);

   m_menuBarHeight = 0.0f;
   m_toolBarHeight = showFullUI ? (ToolButtonSize + 4.f) * m_editor.m_liveUI.GetDPI() + 1.f : 0.f; // + 1 px separator at the bottom, panels start below it

   if (!showFullUI)
      return;

   RenderMenuBar();
   RenderToolbar();
   RenderStatusOverlay();
}

void EditorChrome::RenderMenuBar()
{
   EditorUI &editor = m_editor;
   const float dpi = editor.m_liveUI.GetDPI();
   // The menu bar is tall enough for the workspace tabs, and shares the toolbar background
   constexpr float menuBarHeight = 28.f;
   constexpr float tabHeight = 24.f;
   ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, max(ImGui::GetStyle().FramePadding.y, 0.5f * (menuBarHeight * dpi - ImGui::GetFontSize()))));
   ImGui::PushStyleColor(ImGuiCol_MenuBarBg, EditorTheme::V(EditorTheme::Bar));
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   if (ImGui::BeginMainMenuBar())
   {
      if (!editor.IsInspectMode() && ImGui::BeginMenu("File"))
      {
         if (ImGui::BeginMenu("New"))
         {
            if (ImGui::MenuItem("New Table", "Ctrl+N"))
               editor.NewTable(EditorUI::NewTableTemplate::Blank);
            if (ImGui::MenuItem("Completely Blank Table"))
               editor.NewTable(EditorUI::NewTableTemplate::Stripped);
            if (ImGui::MenuItem("Full Example Table"))
               editor.NewTable(EditorUI::NewTableTemplate::Example);
            if (ImGui::MenuItem("Light Sequence Demo Table"))
               editor.NewTable(EditorUI::NewTableTemplate::LightSeq);
            ImGui::EndMenu();
         }
         if (ImGui::MenuItem("Load..."))
            editor.LoadTable();
         if (ImGui::MenuItem("Load Pack Folder..."))
            editor.LoadTableFolder();
         if (ImGui::MenuItem("Save", "Ctrl+S"))
            editor.SaveTable();
         if (ImGui::MenuItem("Save As..."))
            editor.SaveTableAs();
         if (ImGui::MenuItem("Save As Pack Folder..."))
            editor.SaveTableAsPackFolder();
         ImGui::Separator();
         if (ImGui::MenuItem("Play", "F5", false, !editor.m_table->IsLocked()))
            editor.PlayTest();
         ImGui::Separator();
         if (ImGui::MenuItem("Quit"))
            editor.RequestClose(Player::CS_CLOSE_APP);
         ImGui::EndMenu();
      }
      if (!editor.IsInspectMode() && ImGui::BeginMenu("Tools"))
      {
         if (ImGui::MenuItem("Convert to PBR...", nullptr, false, !editor.m_table->IsLocked()))
            editor.m_pbrConvertModal.Show();
         if (ImGui::MenuItem("Import Unity Prefab...", nullptr, false, !editor.m_table->IsLocked()))
            editor.PickUnityPrefab();
         if (ImGui::BeginMenu("Light States"))
         {
            // The editor does not run the table script (which usually sets the lamp states): set the light state of the table or of a collection here
            const auto setStates = [&](const vector<IEditable *> &parts, const float state)
            {
               for (IEditable *const part : parts)
                  if (part->GetItemType() == eItemLight)
                     static_cast<Light *>(part)->put_State(state);
            };
            const float states[] = { 0.f, 0.706f, 1.f };
            const char *stateNames[] = { "Off", "0.706 (lamp top)", "On" };
            if (ImGui::BeginMenu("All Lights"))
            {
               for (int i = 0; i < 3; i++)
                  if (ImGui::MenuItem(stateNames[i]))
                     setStates(editor.m_table->GetParts(), states[i]);
               ImGui::EndMenu();
            }
            for (auto *const collection : editor.m_table->GetCollections())
            {
               bool hasLight = false;
               for (const IEditable *const part : collection->GetParts())
                  hasLight |= part->GetItemType() == eItemLight;
               if (hasLight && ImGui::BeginMenu(collection->m_name.c_str()))
               {
                  for (int i = 0; i < 3; i++)
                     if (ImGui::MenuItem(stateNames[i]))
                        setStates(collection->GetParts(), states[i]);
                  ImGui::EndMenu();
               }
            }
            ImGui::EndMenu();
         }
         if (ImGui::BeginMenu("Navigation"))
         {
            Settings &settings = g_settingsService.GetAppSettings();
            if (ImGui::MenuItem("Orbit Around Selection", nullptr, &editor.m_navOrbitSelection))
               settings.SetEditor_NavOrbitSelection(editor.m_navOrbitSelection, false);
            if (ImGui::MenuItem("Zoom To Mouse Position", nullptr, &editor.m_navZoomToMouse))
               settings.SetEditor_NavZoomToMouse(editor.m_navZoomToMouse, false);
            if (ImGui::MenuItem("Auto Depth (Orbit Around Point Under Cursor)", nullptr, &editor.m_navAutoDepth))
               settings.SetEditor_NavAutoDepth(editor.m_navAutoDepth, false);
            if (ImGui::MenuItem("Invert Zoom", nullptr, &editor.m_navInvertZoom))
               settings.SetEditor_NavInvertZoom(editor.m_navInvertZoom, false);
            if (ImGui::MenuItem("Alt + Drag Acts As Middle Button", nullptr, &editor.m_navAltMiddleButton))
               settings.SetEditor_NavAltMiddleButton(editor.m_navAltMiddleButton, false);
            if (ImGui::MenuItem("Trackpad (Scroll Pans, Ctrl/Cmd+Scroll Zooms)", nullptr, &editor.m_navTrackpad))
               settings.SetEditor_NavTrackpad(editor.m_navTrackpad, false);
            if (ImGui::MenuItem("Emulate Numpad (Top Row Digits)", nullptr, &editor.m_navEmulateNumpad))
               settings.SetEditor_NavEmulateNumpad(editor.m_navEmulateNumpad, false);
            if (ImGui::MenuItem("Blender Transform Keys (G / R / S)", nullptr, &editor.m_navBlenderTransform))
               settings.SetEditor_NavBlenderTransform(editor.m_navBlenderTransform, false);
            float fov = editor.m_navFovY;
            if (ImGui::SliderFloat("Field Of View", &fov, 15.f, 100.f, "%.1f deg"))
            {
               editor.SetFov(fov);
               settings.SetEditor_NavFovY(editor.m_navFovY, false);
            }
            const char *selectionModes[] = { "Selection: Edges", "Selection: Edges + Tint", "Selection: Hidden" };
            if (ImGui::BeginMenu(selectionModes[editor.m_navSelectionDisplay]))
            {
               for (int i = 0; i < 3; i++)
                  if (ImGui::MenuItem(selectionModes[i] + 11, nullptr, editor.m_navSelectionDisplay == i))
                  {
                     editor.m_navSelectionDisplay = i;
                     settings.SetEditor_NavSelectionDisplay(i, false);
                  }
               ImGui::EndMenu();
            }
            if (ImGui::SliderFloat("Orbit Sensitivity", &editor.m_navOrbitSensitivity, 0.1f, 5.f, "%.1f"))
               settings.SetEditor_NavOrbitSensitivity(editor.m_navOrbitSensitivity, false);
            ImGui::EndMenu();
         }
         ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("View"))
      {
         using PV = EditorUI::PredefinedView;
         const auto viewItem = [&](const char *label, const char *keys, PV view)
         {
            if (ImGui::MenuItem(label, keys, editor.m_camMode == ViewMode::EditorCam && editor.m_predefinedView == view))
               editor.SetView(view);
         };
         viewItem("Top", "7", PV::Top);
         viewItem("Bottom", "Ctrl+7", PV::Bottom);
         viewItem("Front", "1", PV::Front);
         viewItem("Back", "Ctrl+1", PV::Back);
         viewItem("Left", "3", PV::Left);
         viewItem("Right", "Ctrl+3", PV::Right);
         ImGui::Separator();
         if (ImGui::MenuItem("Perspective / Orthographic", "5"))
         {
            editor.m_camMode = ViewMode::EditorCam;
            editor.m_perspectiveCam = !editor.m_perspectiveCam;
         }
         if (ImGui::MenuItem("Cycle Camera (Preview / Editor / Backdrop)", "0"))
            editor.CycleCamera();
         if (ImGui::MenuItem("Lock Orbit", "Numpad /", editor.m_orbitLock))
            editor.m_orbitLock = !editor.m_orbitLock;
         ImGui::Separator();
         if (ImGui::MenuItem("Orbit Left", "4"))
            editor.OrbitStep(true, 1.f);
         if (ImGui::MenuItem("Orbit Right", "6"))
            editor.OrbitStep(true, -1.f);
         if (ImGui::MenuItem("Orbit Up", "8"))
            editor.OrbitStep(false, 1.f);
         if (ImGui::MenuItem("Orbit Down", "2"))
            editor.OrbitStep(false, -1.f);
         ImGui::Separator();
         if (ImGui::MenuItem("Frame Selected", "F"))
            editor.FrameSelection();
         if (ImGui::MenuItem("Frame All", "Home"))
            editor.FrameAll();
         ImGui::Separator();
         static constexpr const char *workspaceKeys[PinnedPanel::WorkspaceCount] = { "Ctrl+1", "Ctrl+2", "Ctrl+3" };
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
            if (ImGui::MenuItem(("Workspace: " + editor.m_pinned.GetWorkspace(i).name).c_str(), workspaceKeys[i], editor.m_pinned.GetActiveWorkspace() == i))
               editor.m_pinned.ApplyWorkspace(i);
         ImGui::Separator();
         if (ImGui::MenuItem("Outliner", nullptr, editor.IsAreaEditorShown(EditorUI::AreaEditor::Outliner)))
            editor.ToggleAreaEditor(EditorUI::AreaEditor::Outliner);
         if (ImGui::MenuItem("Properties", nullptr, editor.IsAreaEditorShown(EditorUI::AreaEditor::Properties)))
            editor.ToggleAreaEditor(EditorUI::AreaEditor::Properties);
         // Editor type of the left and right areas (Blender's editor type switcher, also the button at the left of each area header)
         for (int side = 0; side < 2; ++side)
         {
            const bool right = side == 1;
            if (ImGui::BeginMenu(right ? "Right Area" : "Left Area"))
            {
               bool &shown = right ? editor.m_showProperties : editor.m_showOutliner;
               if (ImGui::MenuItem("Visible", nullptr, shown))
                  shown = !shown;
               ImGui::Separator();
               const EditorUI::AreaEditor current = right ? editor.m_rightEditor : editor.m_leftEditor;
               for (int i = 0; i < static_cast<int>(EditorUI::AreaEditor::Count); ++i)
               {
                  const EditorUI::AreaEditor type = static_cast<EditorUI::AreaEditor>(i);
                  if (ImGui::MenuItem(EditorUI::AreaEditorName(type), nullptr, current == type))
                  {
                     editor.SetAreaEditor(right, type);
                     shown = true;
                  }
               }
               ImGui::EndMenu();
            }
         }
         if (ImGui::MenuItem("Sidebar", "N", editor.m_showSidebar))
            editor.ToggleSidebar();
         if (ImGui::MenuItem("Quick Favorites", "Q"))
            editor.OpenQuickFavorites();
         if (ImGui::MenuItem("Menu Search...", "F3"))
            editor.m_palette.Open();
         if (ImGui::MenuItem("Navigation Cube", nullptr, editor.m_showNavCube))
            editor.m_showNavCube = !editor.m_showNavCube;
         ImGui::EndMenu();
      }
      if (editor.IsInspectMode() && !editor.m_table->IsLocked() && ImGui::BeginMenu("Debug"))
      {
         if (ImGui::MenuItem("Open debugger"))
            editor.m_player->m_showDebugger = true;
         if (ImGui::MenuItem("Renderer Inspection"))
            editor.m_inspectionModal.Show();
         if (ImGui::MenuItem(editor.m_player->IsPlaying() ? "Pause" : "Play"))
            editor.m_player->SetPlayState(!editor.m_player->IsPlaying());
         ImGui::EndMenu();
      }
      else if (!editor.IsInspectMode() && !editor.m_table->IsLocked() && ImGui::BeginMenu("Debug"))
      {
         if (ImGui::MenuItem("Renderer Inspection"))
            editor.m_inspectionModal.Show();
         ImGui::EndMenu();
      }
      // Workspace tabs (Blender's top bar), centered in the menu bar
      if (!editor.m_table->IsLocked())
      {
         const ImGuiStyle &tabStyle = ImGui::GetStyle();
         const float tabPadding = 12.f * dpi;
         const float tabGap = 0.f; // Tabs touch each other
         float tabsWidth = 0.f;
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
            tabsWidth += ImGui::CalcTextSize(editor.m_pinned.GetWorkspace(i).name.c_str()).x + tabPadding * 2.f + (i > 0 ? tabGap : 0.f);
         const float startX = max(ImGui::GetCursorPosX() + tabStyle.ItemSpacing.x, 0.5f * (ImGui::GetWindowWidth() - tabsWidth));
         const ImVec2 barPos = ImGui::GetWindowPos();
         const float barBottom = barPos.y + ImGui::GetWindowHeight();
         const float savedScreenY = ImGui::GetCursorScreenPos().y;
         ImDrawList *const drawList = ImGui::GetWindowDrawList();
         drawList->AddLine(ImVec2(barPos.x, barBottom - 1.f), ImVec2(barPos.x + ImGui::GetWindowWidth(), barBottom - 1.f), EditorTheme::TabActive, 1.f); // Bottom rule
         float x = barPos.x + startX;
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
         {
            const string &name = editor.m_pinned.GetWorkspace(i).name;
            const bool active = editor.m_pinned.GetActiveWorkspace() == i;
            const ImVec2 textSize = ImGui::CalcTextSize(name.c_str());
            const ImVec2 tabSize(textSize.x + tabPadding * 2.f, tabHeight * dpi);
            ImGui::SetCursorScreenPos(ImVec2(x, barBottom - tabSize.y));
            if (ImGui::InvisibleButton((name + "##Workspace").c_str(), tabSize))
               editor.m_pinned.ApplyWorkspace(i);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 tabMin = ImGui::GetItemRectMin();
            const ImVec2 tabMax = ImGui::GetItemRectMax();
            drawList->AddRectFilled(tabMin, tabMax, active ? EditorTheme::TabActive : (hovered ? EditorTheme::TabHover : 0u), 5.f * dpi, ImDrawFlags_RoundCornersTop);
            drawList->AddText(ImVec2(tabMin.x + tabPadding, tabMin.y + 0.5f * (tabSize.y - textSize.y)), active ? IM_COL32_WHITE : EditorTheme::TextDim, name.c_str());
            ImGui::SetItemTooltip("Workspace (Ctrl+%d)", i + 1);
            x += tabSize.x + tabGap;
         }
         ImGui::SetCursorScreenPos(ImVec2(x, savedScreenY));
      }
      // Right side buttons: 28x28, transparent, red when hovering the close one
      const float barButton = 28.f * dpi;
      const auto barIconButton = [barButton](const char *icon, ImU32 hoverColor)
      {
         ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
         ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(hoverColor));
         ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(hoverColor));
         const bool pressed = ImGui::Button(icon, ImVec2(barButton, barButton));
         ImGui::PopStyleColor(3);
         return pressed;
      };
      float buttonWidth = barButton;
      if (editor.IsInspectMode())
         buttonWidth += barButton + ImGui::GetStyle().ItemSpacing.x;
      ImVec2 padding = ImGui::GetCursorScreenPos();
      padding.x += ImGui::GetContentRegionAvail().x - buttonWidth;
      ImGui::SetCursorScreenPos(padding);
      if (editor.IsInspectMode())
      {
         if (barIconButton(ICON_LC_UNDO_2, EditorTheme::Hover)) // ICON_LC_SQUARE
            editor.Close();
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Get back to player");
      }
      if (barIconButton(ICON_LC_X, EditorTheme::Rgb(200, 60, 60)))
         editor.RequestClose(editor.IsInspectMode() ? Player::CS_STOP_PLAY : Player::CS_CLOSE_APP);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Close editor");
      m_menuBarHeight = ImGui::GetWindowSize().y;
      ImGui::EndMainMenuBar();
   }
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar();
}

void EditorChrome::RenderToolbar()
{
   EditorUI &editor = m_editor;
   s_toolDpi = editor.m_liveUI.GetDPI();
   const float toolGap = 2.f * s_toolDpi;
   const float oldIconSize = max(0.f, 20.f * s_toolDpi - 2.f * ImGui::GetStyle().WindowPadding.y); // Size of the add part popup icons
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + m_menuBarHeight));
   ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, m_toolBarHeight));
   constexpr ImGuiWindowFlags window_flags
      = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f * s_toolDpi, 2.f * s_toolDpi));
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   ImGui::Begin("TOOLBAR", nullptr, window_flags);
   ImGui::PopStyleColor();
   ImGui::PopStyleVar(2);
   // 1 px separator between the toolbar and the area panels below
   ImGui::GetWindowDrawList()->AddLine(ImVec2(viewport->Pos.x, viewport->Pos.y + m_menuBarHeight + m_toolBarHeight - 1.f), ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + m_menuBarHeight + m_toolBarHeight - 1.f), EditorTheme::TabActive, 1.f);
   if (editor.IsInspectMode())
   {
      if (ToolButton(editor.m_player->IsPlaying() ? ICON_LC_PAUSE : ICON_LC_PLAY))
         editor.m_player->SetPlayState(!editor.m_player->IsPlaying());
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(editor.m_player->IsPlaying());
      if (ToolButton(ICON_LC_SKIP_FORWARD))
         editor.m_player->m_step = true;
      ImGui::EndDisabled();
   }
   else
   {
      const float iconSize = oldIconSize;

      // File operations: same actions as the File menu, each button opening a sub menu
      if (ToolButton(ICON_LC_FILE_PLUS))
         ImGui::OpenPopup("New table Popup");
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("New table\n[Ctrl+N]");
      if (ImGui::BeginPopup("New table Popup"))
      {
         if (ImGui::MenuItem("New Table", "Ctrl+N"))
            editor.NewTable(EditorUI::NewTableTemplate::Blank);
         if (ImGui::MenuItem("Completely Blank Table"))
            editor.NewTable(EditorUI::NewTableTemplate::Stripped);
         if (ImGui::MenuItem("Full Example Table"))
            editor.NewTable(EditorUI::NewTableTemplate::Example);
         if (ImGui::MenuItem("Light Sequence Demo Table"))
            editor.NewTable(EditorUI::NewTableTemplate::LightSeq);
         ImGui::EndPopup();
      }
      ImGui::SameLine(0.f, toolGap);
      if (ToolButton(ICON_LC_FOLDER_OPEN))
         ImGui::OpenPopup("Open table Popup");
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Open table");
      if (ImGui::BeginPopup("Open table Popup"))
      {
         if (ImGui::MenuItem("Table file..."))
            editor.LoadTable();
         if (ImGui::MenuItem("Table pack folder..."))
            editor.LoadTableFolder();
         ImGui::EndPopup();
      }
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_SAVE))
         ImGui::OpenPopup("Save table Popup");
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Save table\n[Ctrl+S]");
      if (ImGui::BeginPopup("Save table Popup"))
      {
         if (ImGui::MenuItem("Save", "Ctrl+S"))
            editor.SaveTable();
         if (ImGui::MenuItem("Save As File..."))
            editor.SaveTableAs();
         if (ImGui::MenuItem("Save As Pack Folder..."))
            editor.SaveTableAsPackFolder();
         ImGui::EndPopup();
      }
      ImGui::EndDisabled();
      ToolSeparator();

      // Play a shallow copy of the edited table
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_PLAY))
         editor.PlayTest();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Play table\n[F5]");
      ImGui::EndDisabled();
      ToolSeparator();

      // Edit mode selection (same actions as the Esc, G, S and R keyboard shortcuts)
      static const struct
      {
         ImGuizmo::OPERATION operation;
         const char *icon;
         const char *tooltip;
      } editModes[] = {
         { (ImGuizmo::OPERATION)0, ICON_LC_MOUSE_POINTER_2, "Select\n[Esc]" }, //
         { ImGuizmo::TRANSLATE, ICON_LC_MOVE, "Grab\n[G]" }, //
         { ImGuizmo::SCALE, ICON_LC_SCALE_3D, "Scale\n[S]" }, //
         { ImGuizmo::ROTATE, ICON_LC_ROTATE_3D, "Rotate\n[R]" }, //
      };
      for (const auto &mode : editModes)
      {
         const bool active = (editor.m_gizmoOperation == mode.operation);
         if (ToolButton(mode.icon, active))
            editor.SetGizmoOperation(mode.operation);
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", mode.tooltip);
         if (&mode != std::end(editModes) - 1)
            ImGui::SameLine(0.f, toolGap);
      }
      ToolSeparator();

      // Add part buttons (same parts and icons as the WinUI toolbar)
      if (m_addPartButtons.empty())
      {
         static const struct
         {
            ItemTypeEnum type;
            const char *name;
            const char *file;
         } addParts[] = {
            { eItemSurface, "Surface", "editor/wall.png" }, //
            { eItemGate, "Gate", "editor/gate.png" }, //
            { eItemRamp, "Ramp", "editor/ramp.png" }, //
            { eItemFlipper, "Flipper", "editor/flipper.png" }, //
            { eItemPlunger, "Plunger", "editor/plunger.png" }, //
            { eItemBall, "Ball", "editor/ball.png" }, //
            { eItemBumper, "Bumper", "editor/bumper.png" }, //
            { eItemSpinner, "Spinner", "editor/spinner.png" }, //
            { eItemTimer, "Timer", "editor/timer.png" }, //
            { eItemTrigger, "Trigger", "editor/trigger.png" }, //
            { eItemLight, "Light", "editor/light.png" }, //
            { eItemKicker, "Kicker", "editor/kicker.png" }, //
            { eItemHitTarget, "Target", "editor/target.png" }, //
            { eItemDecal, "Decal", "editor/decal.png" }, //
            { eItemTextbox, "Textbox", "editor/textbox.png" }, //
            { eItemDispReel, "Display Reel", "editor/reel.png" }, //
            { eItemLightSeq, "Light Seq", "editor/lightseq.png" }, //
            { eItemPrimitive, "Primitive", "editor/primitive.png" }, //
            { eItemFlasher, "Flasher", "editor/flasher.png" }, //
            { eItemRubber, "Rubber", "editor/rubber.png" }, //
         };
         for (const auto &def : addParts)
         {
            std::shared_ptr<const BaseTexture> tex = BaseTexture::CreateFromFile(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets, def.file));
            m_addPartButtons.push_back({ def.type, def.name, tex ? std::make_shared<Sampler>(editor.m_renderer->m_renderDevice, def.name, tex, false) : nullptr });
         }
      }
      // Add part button, opening the part type picker popup (same as the Shift+A shortcut)
      ImGui::BeginDisabled(editor.m_pointEditPart != nullptr || editor.m_table->IsLocked());
      const bool addPartActive = (editor.m_addPartType != eItemInvalid);
      if (ToolButton(ICON_LC_PLUS, addPartActive))
         RequestAddPartPopup(ImGui::GetMousePos());
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Add part\n[Shift+A]");
      ImGui::EndDisabled();

      // Copy/Paste buttons (same actions as the Ctrl+C and Ctrl+V keyboard shortcuts)
      ImGui::SameLine(0.f, toolGap);
      const bool canCopy
         = editor.m_pointEditPart ? (editor.m_pointEditCenter ? editor.m_centerSelected : editor.m_pointSel.size() == 1) : (editor.m_selection.GetType() == Selection::S_EDITABLE);
      ImGui::BeginDisabled(!canCopy || editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_COPY))
         editor.CopySelection();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Copy selection\n[Ctrl+C]");
      ImGui::EndDisabled();
      ImGui::SameLine(0.f, toolGap);
      const bool canPaste = editor.m_pointEditPart
         ? (editor.m_pointEditCenter ? editor.m_centerSelected && VPX::EditorClipboard::HasPoint() : editor.m_pointSel.size() == 1 && VPX::EditorClipboard::HasPoint())
         : VPX::EditorClipboard::HasParts();
      ImGui::BeginDisabled(!canPaste || editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_CLIPBOARD_PASTE))
         editor.PasteSelection(ImVec2(viewport->GetCenter().x, viewport->GetCenter().y));
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Paste\n[Ctrl+V]");
      ImGui::EndDisabled();

      // Delete selection button, always visible but only enabled when there are selected parts
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(editor.m_selection.GetType() != Selection::S_EDITABLE || editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_TRASH_2))
         editor.DeleteSelection();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Delete selection\n[Delete]");
      ImGui::EndDisabled();

      // Part library import/export buttons
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_DOWNLOAD))
         editor.OpenPartLibrary();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Import parts (part library)");
      ImGui::EndDisabled();
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(!editor.CanExportPartSelection());
      if (ToolButton(ICON_LC_UPLOAD))
         editor.ExportPartSelection();
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Export selection to a VPZ pack");
      ImGui::EndDisabled();

      // Script editor button
      ToolSeparator();
      const bool scriptActive = editor.IsAreaEditorShown(EditorUI::AreaEditor::Script);
      if (ToolButton(ICON_LC_CODE_XML, scriptActive))
         editor.ShowAreaEditor(EditorUI::AreaEditor::Script);
      if (ImGui::IsItemHovered())
         ImGui::SetTooltip("Edit table script");

      // Part type picker popup (Shift+A in standard mode): picking a type arms add part mode,
      // clicking outside of the popup dismisses it without arming add part mode
      if (m_openAddPartPopup)
      {
         m_openAddPartPopup = false;
         ImGui::OpenPopup("Add Part");
      }
      ImGui::SetNextWindowPos(m_addPartPopupPos, ImGuiCond_Appearing);
      if (ImGui::BeginPopup("Add Part"))
      {
         int column = 0;
         for (const auto &button : m_addPartButtons)
         {
            if (button.icon == nullptr)
               continue;
            if (column > 0)
               ImGui::SameLine();
            if (ImGui::ImageButton(button.name, button.icon, ImVec2(iconSize, iconSize)))
            {
               editor.m_addPartType = button.type;
               ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Add %s", button.name);
            column = (column + 1) % 5;
         }
         ImGui::EndPopup();
      }
   }
   const float buttonWidth = 3.f * ToolButtonSize * s_toolDpi + 2.f * toolGap;
   ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttonWidth);
   if (ToolButton(ICON_LC_RULER)) // Unit selection menu
      ImGui::OpenPopup("Unit Popup");
   if (ImGui::BeginPopup("Unit Popup"))
   {
      ImGui::TextUnformatted("Units:");
      ImGui::Separator();
      if (ImGui::RadioButton("VP Units", editor.m_units == EditorUI::Units::VPX))
         editor.m_units = EditorUI::Units::VPX;
      if (ImGui::RadioButton("Metric", editor.m_units == EditorUI::Units::Metric))
         editor.m_units = EditorUI::Units::Metric;
      if (ImGui::RadioButton("Imperial", editor.m_units == EditorUI::Units::Imperial))
         editor.m_units = EditorUI::Units::Imperial;
      ImGui::EndPopup();
   }
   ImGui::SameLine(0.f, toolGap);
   if (ToolButton(ICON_LC_STICKY_NOTE)) // Overlay option menu
      ImGui::OpenPopup("Overlay Popup");
   if (ImGui::BeginPopup("Overlay Popup"))
   {
      ImGui::TextUnformatted("Overlays:");
      ImGui::Separator();
      ImGui::Checkbox("Overlay selection", &editor.m_selectionOverlay);
      ImGui::Separator();
      ImGui::TextUnformatted("Physic Overlay:");
      if (ImGui::RadioButton("None", editor.m_physOverlay == EditorUI::PhysicOverlay::None))
         editor.m_physOverlay = EditorUI::PhysicOverlay::None;
      if (ImGui::RadioButton("Selected", editor.m_physOverlay == EditorUI::PhysicOverlay::Selected))
         editor.m_physOverlay = EditorUI::PhysicOverlay::Selected;
      if (ImGui::RadioButton("All", editor.m_physOverlay == EditorUI::PhysicOverlay::All))
         editor.m_physOverlay = EditorUI::PhysicOverlay::All;
      ImGui::EndPopup();
   }
   ImGui::SameLine(0.f, toolGap);
   if (ToolButton(ICON_LC_LIST_FILTER)) // Selection filter
      ImGui::OpenPopup("Selection filter Popup");
   if (ImGui::BeginPopup("Selection filter Popup"))
   {
      bool pf = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Playfield);
      bool prims = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Primitives);
      bool lights = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Lights);
      bool flashers = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Flashers);
      ImGui::TextUnformatted("Selection filters:");
      ImGui::Separator();
      if (ImGui::Checkbox("Playfield", &pf))
         editor.m_selectionFilter = pf ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Playfield) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Playfield);
      if (ImGui::Checkbox("Primitives", &prims))
         editor.m_selectionFilter = prims ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Primitives) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Primitives);
      if (ImGui::Checkbox("Lights", &lights))
         editor.m_selectionFilter = lights ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Lights) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Lights);
      if (ImGui::Checkbox("Flashers", &flashers))
         editor.m_selectionFilter = flashers ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Flashers) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Flashers);
      ImGui::EndPopup();
   }
   ImGui::End();
}

void EditorChrome::RenderStatusOverlay()
{
   EditorUI &editor = m_editor;
   const ImGuiIO &io = ImGui::GetIO();

   // Overlay Info Text
   const ImVec2 overlayPos((editor.m_showOutliner ? editor.m_outlinerWidth : 0.f) * editor.m_liveUI.GetDPI(), m_toolBarHeight + m_menuBarHeight + 5.f * editor.m_liveUI.GetDPI());

   // Camera orbit lock toggle (drawn in its own window as the status overlay does not capture inputs)
   ImGui::SetNextWindowPos(overlayPos);
   ImGui::Begin("orbit lock toggle", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
         | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
   const bool stylePushed = editor.m_orbitLock;
   if (stylePushed)
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
   if (ImGui::Button(editor.m_orbitLock ? ICON_LC_LOCK : ICON_LC_LOCK_OPEN))
      editor.m_orbitLock = !editor.m_orbitLock;
   if (stylePushed)
      ImGui::PopStyleColor();
   if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Lock camera orbit\n[Keypad /]");
   const float lockToggleWidth = ImGui::GetItemRectSize().x;
   ImGui::End();

   ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - (editor.m_showOutliner ? editor.m_outlinerWidth : 0.f) * editor.m_liveUI.GetDPI(),
      io.DisplaySize.y - m_toolBarHeight - m_menuBarHeight - 5.f * editor.m_liveUI.GetDPI())); // Fixed outliner width (to be adjusted when moving ImGui to the docking branch)
   ImGui::SetNextWindowPos(overlayPos);
   ImGui::Begin("text overlay", nullptr, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
   const float labelAlign = ImGui::GetCursorPosX() + lockToggleWidth + ImGui::GetStyle().ItemSpacing.x;
   ImGui::SetCursorPos(ImVec2(labelAlign, ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y));
   switch (editor.m_camMode)
   {
   case ViewMode::PreviewCam: ShadowedText("Preview Camera"); break;
   case ViewMode::EditorCam:
   {
      string text;
      switch (editor.m_predefinedView)
      {
      case EditorUI::PredefinedView::None: text = "User"sv; break;
      case EditorUI::PredefinedView::Left: text = "Left"sv; break;
      case EditorUI::PredefinedView::Right: text = "Right"sv; break;
      case EditorUI::PredefinedView::Top: text = "Top"sv; break;
      case EditorUI::PredefinedView::Bottom: text = "Bottom"sv; break;
      case EditorUI::PredefinedView::Front: text = "Front"sv; break;
      case EditorUI::PredefinedView::Back: text = "Back"sv; break;
      }
      ShadowedText((text + (editor.m_perspectiveCam ? " Perspective" : " Orthographic")).c_str());
      break;
   }
   case ViewMode::DesktopBackdrop: ShadowedText("Desktop Backdrop"); break;
   }
   ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + ImGui::GetStyle().FramePadding.x, ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y));
   const char *gizmoIcon = nullptr;
   const char *gizmoLabel = nullptr;
   if (editor.m_gizmoOperation == ImGuizmo::OPERATION(0))
   {
      gizmoIcon = ICON_LC_MOUSE_POINTER_2;
      gizmoLabel = "Select";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::TRANSLATE)
   {
      gizmoIcon = ICON_LC_MOVE;
      gizmoLabel = "Grab";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::SCALE)
   {
      gizmoIcon = ICON_LC_SCALE_3D;
      gizmoLabel = "Scale";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::ROTATE)
   {
      gizmoIcon = ICON_LC_ROTATE_3D;
      gizmoLabel = "Rotate";
   }
   if (gizmoIcon)
   {
      ShadowedText(gizmoIcon);
      ImGui::SameLine(labelAlign);
      ShadowedText(gizmoLabel);
   }
   if (editor.m_pointEditPart)
      ShadowedText(editor.m_pointEditCenter ? "Center Edit (Tab to exit)" : "Drag Point Edit (Tab to exit)");
   ImGui::End();
}

}
