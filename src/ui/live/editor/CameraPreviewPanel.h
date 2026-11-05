// license:GPLv3+

#pragma once

#include "imgui/imgui.h"
#include "AreaLayout.h"
#include "renderer/Renderer.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Matrix3D;
class FRect3D;

namespace VPX::EditorUI
{

class EditorUI;

// A view's Local View (Blender's numpad /): the parts the view is limited to, as they were selected when it was entered. View-only: nothing of the parts, the
// table, the undo history or a file changes, and none of it is saved or kept over a table change or a play session (see EditorUI::ToggleLocalView)
struct LocalViewParts
{
   bool active = false;
   LocalViewSet set; // The parts of the table it is limited to, by address: what the renderer draws and the picking accepts
   std::map<const IEditable *, std::string> names; // Their names, to take a part back that an undo returns as another object
   std::vector<std::string> lost; // Names of the parts which left the table (deleted)
   void Clear()
   {
      active = false;
      set.clear();
      names.clear();
      lost.clear();
   }
   const LocalViewSet *ForRenderer() const { return active ? &set : nullptr; } // Null (the renderer's nil check) when the view is not local
   bool Allows(const IEditable *part) const { return !active || set.contains(part); }
};

// Secondary 3D view, hosted by a Camera Preview editor area (up to AreaLayout::kMaxViewLeaves, one panel per renderer view slot): not the
// editing viewport (click selects as in the 3D Viewport, see HandleSelectInput; the gizmo and G / R / S work in the view under the mouse, see EditorUI::RenderUI), the scene rendered by the Renderer into an offscreen target beside it, with
// - camera: the Player view (the table's view setup, as the editor's Preview Camera shows it, Auto/Desktop/Cabinet/FSS) or its own Free orbit
//   camera (turntable orbit, pan, zoom, numpad views, Frame Selected / Frame All, with the main viewport's mouse bindings, while hovered),
// - shading: full PBR, or Wireframe (the editor's hidden line wireframe on the renderer's lite path: no post processing, no HDR targets).
// The settings are the layout leaf's (AreaViewSettings), saved with the workspace.
//
// Cost: nothing is rendered while no area shows it. When shown, it is rendered at the displayed image size (capped at 1280 px wide)
// and only when something changed (undo history, size, camera, Refresh, end of an interactive edit), at half rate (every 2nd frame,
// the views alternating by rank on one global frame counter; every 4th frame for Wireframe, whose draw is the more expensive one CPU side) while a gizmo, a G/R/S modal transform
// or a field drag runs and all the time when Live is on, and every frame while its own camera is being moved.
class CameraPreviewPanel
{
public:
   CameraPreviewPanel(EditorUI &editor, int slot);

   // Turntable orbit camera helpers over view settings (also used by the part library thumbnails)
   static void CameraVectors(const AreaViewSettings &s, float dir[3], float up[3], float right[3]); // Table space (Z up): view target to eye, screen up, screen right
   static void ComputeOrbit(const AreaViewSettings &s, float fovY, float aspect, Matrix3D &view, Matrix3D &proj);
   static void FrameBounds(AreaViewSettings &s, const FRect3D &bounds, float fovY, float aspect); // Fits target and distance, orientation kept

   void RenderInArea(const ImVec2 &pos, const ImVec2 &size); // Window filling the given area rect, header included
   void NotHosted(); // Every frame the area is not shown: releases the view's render targets (once), and the leaf binding

   // Layout leaf whose settings the panel shows (and writes back on every change), -1 if none
   int GetLeaf() const { return m_leafId; }
   void Bind(int leafId, int previewView, const AreaViewSettings &settings);
   bool HasImage() const { return m_hasImage; } // Last frame: the area was big enough for an image (see EditorUI::RenderAreas, the render schedule)
   void SetRank(int rank) { m_rank = rank; } // Rank among the views on the render schedule of the same shading (PBR / Wireframe), sets the render phase

   // Local View of this view (Blender's numpad /, see EditorUI::ToggleLocalView); the camera it was entered from comes back on exit
   const LocalViewParts &GetLocalView() const { return m_local; }
   LocalViewParts &GetLocalView() { return m_local; }
   void ExitLocalView(); // Ends it, giving the camera back (no report)
   void LocalViewChanged() { m_refresh = true; } // Its parts changed (some left the table or came back): re-render

   // View setup shown by the Player camera: -1 = Auto (the one the player uses, the table's active view mode), else BG_DESKTOP / BG_FULLSCREEN / BG_FSS.
   // Rendering another one never changes the table's or the player's view mode. JSON names (per workspace layout leaf): "desktop",
   // "cabinet", "fss"; Auto is written as no key.
   int GetView() const { return m_view; }
   void SetView(int view);
   static const char *ViewJsonName(int view); // nullptr for Auto
   static int ViewFromJsonName(const char *name); // -1 (Auto) for null or unknown names

private:
   static constexpr int kMaxWidth = 1280;
   static constexpr int kStableFrames = 10; // A new size is adopted after being stable this many frames (no target churn while a splitter is dragged)

   void WriteBack(); // Settings to the layout leaf
   void HandleOrbitInput(const ImVec2 &imgMin, const ImVec2 &imgSize, bool hovered, bool active);
   void HandleSelectInput(const ImVec2 &imgMin, const ImVec2 &imgSize, bool hovered);
   unsigned int PickVisibilityMask() const; // Player mode visibility of what the view shows (0: the editor view's)
   bool PickHiddenInPlay() const; // The view's Hidden in Play option
   void DrawMeasure(const Matrix3D &mvp, const ImVec2 &imgMin, const ImVec2 &imgSize, bool hovered, ImDrawList *drawList); // The Measure tool's rulers and mouse in the view
   void HandleViewKeys(float aspect);
   bool GameOwnsInput() const;
   void FrameBounds(const FRect3D &bounds, float aspect);
   void FrameAll(float aspect);
   void ToggleLocalView(float aspect);

   EditorUI &m_editor;
   const int m_slot; // Renderer view slot
   int m_leafId = -1;
   AreaViewSettings m_settings;
   int m_view = -1; // See GetView
   int m_lastShownView = -1; // Effective view setup of the last request (Auto follows the player's mode changes)
   bool m_refresh = true;
   bool m_wasDragging = false;
   bool m_requested = false; // A view was requested since the last release
   bool m_navDragging = false; // Orbit / pan drag of this view in progress
   bool m_clickPending = false; // A left press in the image, not released yet (a release within the click threshold selects)
   ImVec2 m_clickStart;
   bool m_camMoved = false; // The orbit camera moved this frame (rendered this frame)
   float m_lastAspect = 1.f;
   uint64_t m_lastUndoId = ~0ull;
   std::vector<const void *> m_lastSelection; // Selected parts (active last) of the last frame: a change re-renders (the selection edges are drawn in the view)
   int m_width = 0, m_height = 0; // Requested render size (px)
   int m_pendingWidth = 0, m_pendingHeight = 0, m_pendingFrames = 0;
   int m_rank = 0;
   bool m_hasImage = true;
   LocalViewParts m_local;
   AreaViewSettings m_localSaved; // The settings (camera) the Local View was entered from
};

}
