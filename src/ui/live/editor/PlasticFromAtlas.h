// license:GPLv3+

#pragma once

#include <map>
#include "imgui/imgui.h"
#include "math/matrix.h"
#include "math/vector.h"

class IEditable;
class DragPoint;
class Primitive;
class PinTable;
class Material;
class Texture;

namespace VPX::EditorUI
{

class EditorUI;
class LiveRenderContext;

// Plastic from Atlas: a plastic is traced on top of a print atlas image (a closed outline of straight runs and arcs, plus circular holes) and becomes
// three primitives built by PlasticMesh: the host (print: bottom cap with the atlas image, holds the curve and all parameters) and two companions,
// <host>_Clear (top cap) and <host>_Bevel (walls and bevel), whose meshes are always derived from the host (see Primitive::RegeneratePlastic).
// Add Plastic from Atlas starts a trace state (top view, the atlas drawn as a backdrop under the points): LMB corner, Backspace
// removes the last point, H switches between the outline and the holes, Enter closes the loop and creates the plastic, Esc cancels.
class PlasticFromAtlas final
{
public:
   static constexpr float kVPUPerInch = 47.06f;

   enum DpiSource { DpiFile, DpiName, DpiDefault, DpiMeasured, DpiTyped, DpiFitted };
   static const char *DpiSourceText(int source); // "from file", "from name", "default", "measured", "typed", "fitted"

   struct Trace
   {
      bool active = false;
      bool holes = false; // Placing holes instead of outline points
      string image; // The atlas
      float dpi = 150.f;
      int dpiSource = DpiDefault; // Where the Sheet DPI came from, shown beside it
      string atlasFilter; // The search box of the Atlas list
      float uvm[6] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f }; // table XY to the atlas' UV (sheet centred on the table)
      vector<Vertex3Ds> pts; // outline points (x, y), smooth = arc peak
      vector<bool> smooth;
      vector<Vertex3Ds> holeList; // x, y, diameter
      float holeDia = 12.f;
      bool prevPerspective = true;
      Matrix3D prevView; // The camera to come back to when the trace is cancelled
      float prevDistance = 0.f;
      int prevPredefined = 0;
      int measure = 0; // Sheet DPI from a known distance: 0 off, 1 first point next, 2 second point next, 3 both set
      float measureUv[4] = { 0.f, 0.f, 0.f, 0.f }; // The two points in atlas UV, so zoom, pan and a rescale of the sheet don't move them
      float measureIn = 0.f; // The real distance in inches
   };

   // Fit to Posts: click two holes of the plastic and the two posts under them; the plastic is scaled, turned and moved onto them in one step
   struct Fit
   {
      bool active = false;
      int step = 0; // 0 first hole, 1 second hole, 2 post under the first, 3 post under the second
      Primitive *host = nullptr;
      int hole[2] = { -1, -1 }; // Index in the plastic's holes (a pill is one)
      Vertex2D holePos[2] {};
      Vertex2D postPos[2] {};
      uint64_t undoId = 0; // The undo stack when it began: any change (an undo too) ends it
      bool editing = false; // Started inside the shape edit: leaving it ends the steps
      bool selectHost = false; // The host was the selection (not in point edit) and is selected again afterwards
   };
   // The Sheet DPI of the plastic last placed or fitted and where it came from, session only (shown in the edit panel while it still matches)
   struct DpiNote
   {
      const Primitive *host = nullptr;
      float dpi = 0.f;
      int source = DpiDefault;
   };
   static bool CanFit(const EditorUI &ed);
   static void FitStart(EditorUI &ed);
   static void FitCancel(EditorUI &ed, const char *message = nullptr);
   static void FitClick(EditorUI &ed, const ImVec2 &mouse);
   static void RenderFitPanel(EditorUI &ed);

   static bool CanStart(const EditorUI &ed);
   static void Start(EditorUI &ed); // Add Plastic from Atlas
   static void Cancel(EditorUI &ed);
   static void Finish(EditorUI &ed); // Enter: creates the plastic from the traced loop

   // Called from the viewport input of the editor while tracing
   static void Click(EditorUI &ed, const ImVec2 &mouse, bool shift);
   static void Keys(EditorUI &ed);
   // Point edit of an outline: press on a line (not on a point) = start bowing it, drag = arc through the cursor, double-click = straight. True if the press was taken
   struct Bow
   {
      bool pending = false, active = false;
      ImVec2 start {};
      DragPoint *corner = nullptr, *peak = nullptr, *endCorner = nullptr;
   };
   static bool BowPress(EditorUI &ed, const ImVec2 &mouse, bool doubleClick);
   static void BowDrag(EditorUI &ed);

   // Pill holes (editing holes): drag a round hole's ring outward and a second end follows so the ring end stays under the cursor
   struct Pill
   {
      bool pending = false, active = false;
      ImVec2 start {};
      ImVec2 last { -1.f, -1.f }; // The mouse at the last rebuild
      DragPoint *a = nullptr, *b = nullptr; // The first end, the second end (made when the drag starts)
   };
   static bool HolePress(EditorUI &ed, const ImVec2 &mouse, bool doubleClick); // Ring of a hole: start a pill (round) / round again on a double-click (pill); middle of a pill: select both ends. True if taken
   static void PillDrag(EditorUI &ed);
   static void AbortDrags(EditorUI &ed); // Delete, select all or H while a pill or bow drag holds points: the drag ends without touching them
   static void PillEnd(EditorUI &ed); // The button is up (or the mode ended): a pill dragged back to a point is round again
   static bool ScaleHoles(EditorUI &ed); // S on the selected holes: their diameters (each about its own centre). False when the modal is not a scale of holes
   static void BeforeDeletePoints(EditorUI &ed, const vector<DragPoint *> &points); // Hole points about to be deleted: the pill they leave reads as round
   static void SetHoleDiameter(EditorUI &ed, float dia); // The Hole Diameter of the panel: the next hole, and the selected holes

   // Editing the shape (Tab on a plastic)
   static Primitive *EditedHostOf(const EditorUI &ed) { return EditedHost(ed); }
   static bool IsEditing(const EditorUI &ed); // Point edit of a plastic host (its outline or its holes)
   static bool ZLocked(const EditorUI &ed); // Moves, turns and scales of a plastic stay in X and Y: no Z axis on its gizmo, Z constraints ignored
   static bool IsEditingHoles(const EditorUI &ed);
   static bool TopPlaneZ(const EditorUI &ed, float &z); // The edited (or the single selected) plastic: the height of its top, the plane its moves follow the cursor on
   static bool IsHostSelected(const EditorUI &ed); // A single plastic host is the selection
   static void RenderEditPanel(EditorUI &ed); // The floating panel while editing: Outline / Holes, Show Sheet, Hole Diameter, Curve Detail
   static void ViewportRightClick(EditorUI &ed); // RMB press and release without a drag: Edit Shape on a selected plastic, the edit menu while editing
   static void RenderMenu(EditorUI &ed);
   static void RenderPanel(EditorUI &ed); // The floating panel of the trace state

   // The atlas drawn as a backdrop on the playfield plane (trace state, and Tab on a placed plastic) and the traced points over it
   static void DrawBackdrop(EditorUI &ed, ImDrawList *draw, const LiveRenderContext &ctx);
   static void DrawTrace(EditorUI &ed, ImDrawList *draw, const LiveRenderContext &ctx);
   static void DrawPointEdit(EditorUI &ed, ImDrawList *draw, const LiveRenderContext &ctx); // Tab on a placed plastic: its exact outline and holes in colour

   // Companions follow their host through the editor operations (same undo record)
   static void SyncCompanionVisibility(const vector<IEditable *> &toggled); // The editor eye (H key, outliner) of a host reaches its companions (only for the parts that were toggled)
   // Undo / redo reload a host (its eye included) and not its companions: the companions of the hosts whose eye changed follow
   static std::map<const IEditable *, bool> SnapshotHostVisibility(const EditorUI &ed);
   static void SyncChangedHostVisibility(const EditorUI &ed, const std::map<const IEditable *, bool> &before);
   static void AddCompanions(vector<IEditable *> &parts); // Appends the companions of the plastic hosts in 'parts' that are not in it
   static void FixPasted(const vector<IEditable *> &pasted, const vector<string> &originalNames); // Pasted companions follow the pasted host copies, else become plain primitives

   // Re-apply from F9 (the host is the selection)
   static void Adjust(Primitive *host, float height, float thickness, float bevel, int segs, float holeDia, float dpi, int detail);

private:
   static bool FitInvalid(const EditorUI &ed); // The table changed under Fit to Posts (an undo, a delete, locked, left the edit): the steps end
   static Primitive *EditedHost(const EditorUI &ed); // The plastic host whose points are being edited, else null
   static Primitive *NewPrimitive(EditorUI &ed, const string &name, float x, float y);
   static string UniqueName(EditorUI &ed, const string &name);
   static Material *EnsureMaterial(PinTable *table, int which);
   static float ParseDpi(const string &imageName); // The number before "dpi" in the name, 0 when there is none
   static float SheetDpi(const Texture *tex, const string &imageName, int &source); // The file, else the name, else 150
   static void SelectAtlas(EditorUI &ed, const string &name);
   static void LoadAtlasImage(EditorUI &ed); // The Load Image button: the system file dialog
   static void ApplyLoadedImage(EditorUI &ed); // The dialog's result, once it came back
   static void SetSheet(EditorUI &ed); // Default mapping for the image and dpi, the sheet centred on the table
   static string DefaultImage(const EditorUI &ed);
};

}
