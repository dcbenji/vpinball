// license:GPLv3+

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "imgui/imgui.h"

class Texture;

namespace VPX::EditorUI
{

class EditorUI;
class LiveRenderContext;

// Reference image: a scan or blueprint lying on the playfield plane of the 3D views, as Blender's reference / background images. View-only: it is not a part,
// not in the table data, adds no undo step and never marks the table dirty; it is not drawn in play. One image per table, kept in the table's own settings
// file (<table>.ini, key Editor/ReferenceImage), not in the table.
// The image is placed by its centre (table coordinates, VPU), a rotation (degrees, counter clockwise seen from above) and a scale (VPU per pixel, set by
// the scan DPI or by two picked points and their real distance). It is drawn either Behind (depth tested like a playfield: the parts above it hide it)
// or In Front (over everything, semi transparent), in the 3D Viewport and in each secondary view that shows it (View > Reference Image...).
class ReferenceImage final
{
public:
   ~ReferenceImage();
   void FlushPending(EditorUI &ed); // A settings write still due goes out now (the editor closes, the table may be replaced next)
   void Release(EditorUI &ed); // Before the editor goes (the texture is unloaded from the renderer)

   // The 3D pass of a view being recorded: -1 = the 3D Viewport, else the ordinal of the secondary view among the view areas of the layout
   void Render3D(EditorUI &ed, int viewOrdinal);
   // The 2D overlay of the 3D Viewport: the outline of the image while the panel is open, the points of a two point scale
   void DrawOverlay(EditorUI &ed, ImDrawList *draw, const LiveRenderContext &ctx);
   void RenderPanel(EditorUI &ed);

   void TogglePanel() { m_panelOpen = !m_panelOpen; }
   bool IsPanelOpen() const { return m_panelOpen; }

   // Two point scale: a click in the 3D Viewport picks a point on the image plane
   bool PickActive() const { return m_pick != 0 && m_panelOpen; }
   void Click(EditorUI &ed, const ImVec2 &mouse);
   void CancelPick() { m_pick = 0; }
   void UiHidden() { m_pick = 0; } // The editor panels are hidden: a pick in progress is dropped

private:
   struct Params
   {
      string file; // Absolute path of the image
      float dpi = 150.f; // Scan resolution; scale = kVPUPerInch / dpi
      float scale = 0.f; // VPU per pixel
      float x = 0.f, y = 0.f; // Centre of the image, table coordinates (VPU)
      float rot = 0.f; // Degrees, counter clockwise seen from above
      float z = 0.1f; // Height above the playfield plane (VPU): clear of the playfield surface
      float opacity = 0.6f;
      bool front = false; // In Front instead of Behind
      bool show = true; // The image is shown at all
      bool showMain = true; // ... in the 3D Viewport
      bool showView[3] = { true, true, true }; // ... in the secondary view areas, in layout order
   };

   void EnsureLoaded(EditorUI &ed);
   bool LoadFromFile(EditorUI &ed, const string &file, bool keepPlacement);
   void Save(EditorUI &ed);
   void FlushSave(EditorUI &ed);
   bool Drawable(EditorUI &ed, int viewOrdinal);
   void ToTable(float px, float py, float &tx, float &ty) const; // Pixel to table
   void ToPixel(float tx, float ty, float &px, float &py) const; // Table to pixel
   void ApplyDpi() { m_p.scale = m_p.dpi > 0.f ? kVPUPerInch / m_p.dpi : 1.f; }
   void CenterOnTable(EditorUI &ed);
   void PickFile(EditorUI &ed);

   static constexpr float kVPUPerInch = 47.06f; // The engine's own (PlasticFromAtlas)

   Params m_p;
   Texture *m_tex = nullptr; // Loaded from m_p.file; kept (not deleted) when replaced: a recorded frame may still sample it
   std::vector<std::pair<Texture *, int>> m_retired; // With the frame they were retired in: freed a few frames later, once the GPU is done with them
   void Retire(EditorUI &ed, Texture *tex);
   void Reap(bool all);
   int m_texW = 0, m_texH = 0;
   const void *m_loadedFor = nullptr; // The table the parameters were read for
   string m_error;
   bool m_panelOpen = false;
   bool m_saveDue = false;
   std::shared_ptr<string> m_pendingFile; // The file dialog's answer (written by its callback)

   int m_pick = 0; // 0 = off, 1 = waiting for the first point, 2 = for the second, 3 = both picked (distance entry)
   float m_pickPt[2][2] = {};
   float m_realDistance = 0.f; // In the unit below
   int m_unit = 0; // 0 = inches, 1 = millimetres
};

}
