// license:GPLv3+

#pragma once

#include "imgui/imgui.h"
#include "math/matrix.h"

#include <cmath>

namespace VPX::EditorUI
{

class EditorUI;
struct LocalViewParts;

// Measuring math, in table coordinates (VP units). Free of any UI so that other features can reuse it (for example the scale of a reference image from two
// picked points: Measure::ScaleFactor). The unit factors are the engine's own (core/def.h: 50 VPU = 1 1/16 inch, so 47.06 VPU per inch)
namespace Measure
{

struct Length
{
   float vpu = 0.f;
   float inches() const { return VPUTOINCHES(vpu); }
   float mm() const { return VPUTOMM(vpu); }
};

inline Length FromVPU(const float vpu) { return Length { vpu }; }
inline Length FromInches(const float inches) { return Length { INCHESTOVPU(inches) }; }
inline Length FromMM(const float mm) { return Length { MMTOVPU(mm) }; }

// Distance between two points of the table
inline float DistanceVPU(const Vertex3Ds &a, const Vertex3Ds &b)
{
   const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
   return sqrtf(dx * dx + dy * dy + dz * dz);
}
inline Length Between(const Vertex3Ds &a, const Vertex3Ds &b) { return FromVPU(DistanceVPU(a, b)); }

// Angle at `vertex` between the legs to `a` and `b`, in degrees (0 when a leg is empty)
inline float AngleDegrees(const Vertex3Ds &a, const Vertex3Ds &vertex, const Vertex3Ds &b)
{
   const Vertex3Ds u = a - vertex, v = b - vertex;
   const float lu = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z), lv = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
   if (lu < 1e-6f || lv < 1e-6f)
      return 0.f;
   const float c = (u.x * v.x + u.y * v.y + u.z * v.z) / (lu * lv);
   return acosf(c < -1.f ? -1.f : c > 1.f ? 1.f : c) * (180.f / static_cast<float>(M_PI));
}

// Factor to apply to something measured as `measuredVPU` for it to measure `wantedVPU` (0 when nothing was measured)
inline float ScaleFactor(const float measuredVPU, const float wantedVPU) { return measuredVPU > 1e-6f ? wantedVPU / measuredVPU : 0.f; }

// "12.50 in · 317.5 mm · 588.3 VPU"
string Format(const Length &length);

}

// The Measure tool: Blender's Ruler. Click-drag in a 3D view to measure between two points of the surfaces (or of the playfield plane), drag an end to
// adjust, click-drag the midpoint of a ruler to add an angle leg. Ctrl while dragging snaps to the drag points and the origins of the parts.
// Rulers are editor state of the session: not saved with the table, no undo step, no dirty flag. They are drawn in the 3D Viewport and in the secondary
// 3D views (not in a play test)
class MeasureTool final
{
public:
   bool IsActive() const { return m_active; }
   static const char *Refusal(const EditorUI &ed); // Why the tool can not be entered now (null: it can)
   void SetActive(EditorUI &ed, bool active);
   bool HasRulers() const { return !m_rulers.empty(); }
   void Clear(EditorUI &ed, bool report = true); // All the rulers
   void SetTable(EditorUI &ed, const void *baseTable) // The rulers belong to a table: another one starts with none
   {
      if (m_table != nullptr && baseTable != m_table)
         Clear(ed, false);
      m_table = baseTable;
   }
   void ClearSelected(EditorUI &ed) { DeleteSelected(ed); }

   // Per frame of the 3D Viewport: draws the rulers into `dl` and, in the tool, handles the mouse (`canStart`: a press here is for the tool)
   void UpdateMain(EditorUI &ed, ImDrawList *dl, bool canStart);
   // Same in a secondary view's image: its camera as rendered (`mvp`), the screen rect of its image
   void UpdateImage(EditorUI &ed, int slot, const Matrix3D &mvp, const ImVec2 &rectMin, const ImVec2 &rectSize, unsigned int visibilityMask, const LocalViewParts *local, bool hiddenInPlay,
      ImDrawList *dl, bool canStart);
   // Once per frame, after the views: ends a drag whose view went away
   void EndFrame(EditorUI &ed);

   bool OnEscape(EditorUI &ed); // Cancels the drag in progress, else leaves the tool. True if it did something
   bool OnDelete(EditorUI &ed); // Deletes the selected ruler. True if there was one

private:
   struct Ruler
   {
      Vertex3Ds p[3];
      int count = 2; // 3: an angle ruler, the angle at p[1]
   };
   struct View;
   void Update(EditorUI &ed, const View &view, bool canStart);
   void Draw(const View &view, const Ruler &ruler, bool selected, int hoverPoint);
   bool PickPoint(EditorUI &ed, const View &view, const ImVec2 &mouse, bool snap, Vertex3Ds &out, bool *snapped) const;
   bool FindSnap(EditorUI &ed, const View &view, const ImVec2 &mouse, float radius, Vertex3Ds &out) const;
   void DeleteSelected(EditorUI &ed);
   void CancelDrag();

   bool m_active = false;
   const void *m_table = nullptr;
   vector<Ruler> m_rulers;
   int m_selected = -1;
   struct Drag
   {
      bool active = false;
      int view = -1; // -1: the 3D Viewport, else the slot of the secondary view it started in
      int ruler = -1;
      int point = 0;
      bool isNew = false;
      bool moved = false;
      bool toAngle = false; // A press on the midpoint of a two point ruler: it becomes an angle ruler when the drag starts
      ImVec2 pressPos;
      Ruler backup;
   } m_drag;
   bool m_waitRelease = false; // A drag was cancelled with the button still down
};

}
