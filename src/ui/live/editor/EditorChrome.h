// license:GPLv3+

#pragma once

#include "AreaLayout.h"
#include "core/ieditable.h"
#include "imgui/imgui.h"
#include "renderer/ViewShadeOptions.h"

#include <memory>
#include <vector>

class Sampler;

namespace VPX::EditorUI
{

class EditorUI;

// Top chrome of the editor: main menubar, toolbar (with the add part picker popup)
// and the status overlay text. Owns the heights of the top bars used to position
// the side panels.
class EditorChrome
{
public:
   explicit EditorChrome(EditorUI &editor)
      : m_editor(editor)
   {
   }

   enum class ChevronDir
   {
      Down,
      Right
   };
   // Line chevron (Lucide style, replaces ImGui's filled triangles), centered at c, half width 'size'
   static void DrawChevron(ImDrawList *drawList, const ImVec2 &center, ChevronDir dir, float size, ImU32 color);
   // Blender style tooltip for the last item (also when it is disabled): Title Case name, dim shortcut chip, optional dim sentence case description
   static void Tip(const char *title, const char *shortcut = nullptr, const char *description = nullptr);
   // Blender's shading switch: joined row of 3 sphere icon buttons (mode 0 Wireframe, 1 Solid, 2 PBR, the active one blue), true if the mode was changed. Width: ShadeIconRowWidth
   // With `options` the row ends in Blender's caret button, which opens the Viewport Shading popover of the mode: its view-only options (the main view passes
   // xray = its wireframe X-Ray, kept in its shade mode; a secondary view leaves it null: options->xray). The popover changes `options` (and *xray) in place
   static bool ShadeIconRow(const char *id, int &mode, ViewShadeOptions *options = nullptr, EditorUI *editor = nullptr, bool *xray = nullptr);
   static float ShadeIconRowWidth(bool withCaret = false); // Width of the row, for placing it
   // Blender's transform controls of the 3D view header: the Transform Orientation and Transform Pivot Point dropdowns (editor settings, each view's header shows the same ones)
   static void TransformHeaderRow(const char *id, EditorUI &editor);
   static float TransformHeaderRowWidth(); // Width of the row, for placing it
   static float TransformOrientationWidth(); // Width of its orientation dropdown
   static void DrawOrientationIcon(ImDrawList *dl, const ImVec2 &center, float size, int orientation, ImU32 color, ImU32 dim); // orientation: EditorUI::TransformOrientation
   static void DrawPivotIcon(ImDrawList *dl, const ImVec2 &center, float size, int pivot, ImU32 color, ImU32 dim); // pivot: EditorUI::TransformPivot

   void Render(bool showFullUI);
   float GetTopBarHeight() const { return m_menuBarHeight + m_toolBarHeight; }
   // Height of the status bar at the bottom of the window (0 when hidden, including when the full UI is hidden): the areas end above it
   float GetStatusBarHeight() const { return m_statusBarHeight; }
   // Draws the status bar (context key hints, last operation report). Called once the areas have been rendered so that it stays on top of them
   void RenderStatusBar(bool showFullUI);
   void ToggleStatusBar() { m_showStatusBar = !m_showStatusBar; }
   // Tool shelf overlay inside the viewport area (View > Tool Shelf): drawn right after the areas by the tool shelf task, the rect being the
   // visible part of the 3D view
   void RenderToolShelf(const AreaRect &viewportContent);
   bool IsToolShelfShown() const { return m_showToolShelf; }
   void ToggleToolShelf() { m_showToolShelf = !m_showToolShelf; }
   // Opens the Edit > Undo History window at the mouse position
   void OpenUndoHistory()
   {
      m_undoHistoryOpen = true;
      m_undoHistoryJustOpened = true;
      m_undoHistoryPos = ImGui::GetMousePos();
   }

   // Requests the part type picker popup to be opened at the given position (Shift+A shortcut).
   // The popup is opened from the toolbar window scope, as OpenPopup must be called in the same
   // window scope as the matching BeginPopup.
   void RequestAddPartPopup(const ImVec2 &pos)
   {
      m_addPartPopupPos = pos;
      m_openAddPartPopup = true;
   }

private:
   void RenderMenuBar();
   void RenderToolbar();
   bool ToolShelfVisible() const; // Shelf enabled and allowed in the current state (it may still not fit, see m_toolShelfDrawn)
   void RenderStatusOverlay();
   void MeasureToolButton(); // The Measure tool's button (tool shelf, or top toolbar when the shelf is not drawn)
   void RenderUndoHistory();

   EditorUI &m_editor;
   float m_menuBarHeight = 0.0f;
   float m_toolBarHeight = 0.0f;
   float m_statusBarHeight = 0.0f;
   bool m_showStatusBar = true; // View > Status Bar
   // Scene statistics of the status bar, recomputed when the undo history or the part count changes
   uint64_t m_statsUndoId = ~0ull;
   size_t m_statsParts = ~size_t(0), m_statsVerts = 0, m_statsFaces = 0;
   bool m_toolShelfDrawn = false; // The shelf was drawn last frame: the top toolbar then omits the tools it hosts
   bool m_showToolShelf = true; // View > Tool Shelf (global, not per workspace)
   bool m_undoHistoryOpen = false;
   bool m_undoHistoryJustOpened = false;
   ImVec2 m_undoHistoryPos {};
   struct AddPartButton
   {
      ItemTypeEnum type;
      const char *name;
      std::shared_ptr<Sampler> icon;
      bool wire = false; // The Wireform entry (a primitive armed with the wire generator)
      bool rail = false; // The Wire Rail entry
   };
   std::vector<AddPartButton> m_addPartButtons; // Lazily initialized add part toolbar buttons
   ImVec2 m_addPartPopupPos {}; // Position at which the Shift+A part type picker popup was opened
   bool m_openAddPartPopup = false; // Request to open the part type picker popup (consumed in the toolbar window scope)
};

}
