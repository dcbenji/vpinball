// license:GPLv3+

#pragma once

#include "imgui/imgui.h"
#include "math/matrix.h"
#include "math/vector.h"

class IEditable;
class DragPoint;
class Primitive;
class PinTable;
class Material;

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

   struct Trace
   {
      bool active = false;
      bool holes = false; // Placing holes instead of outline points
      string image; // The atlas
      float dpi = 150.f;
      float uvm[6] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f }; // table XY to the atlas' UV (sheet centred on the table)
      vector<Vertex3Ds> pts; // outline points (x, y), smooth = arc peak
      vector<bool> smooth;
      vector<Vertex3Ds> holeList; // x, y, diameter
      float holeDia = 12.f;
      bool prevPerspective = true;
      Matrix3D prevView; // The camera to come back to when the trace is cancelled
      float prevDistance = 0.f;
      int prevPredefined = 0;
   };

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
   static void RenderPanel(EditorUI &ed); // The floating panel of the trace state

   // The atlas drawn as a backdrop on the playfield plane (trace state, and Tab on a placed plastic) and the traced points over it
   static void DrawBackdrop(EditorUI &ed, ImDrawList *draw, const LiveRenderContext &ctx);
   static void DrawTrace(EditorUI &ed, ImDrawList *draw, const LiveRenderContext &ctx);

   // Companions follow their host through the editor operations (same undo record)
   static void SyncCompanionVisibility(EditorUI &ed); // The editor eye (H key, outliner) of a host reaches its companions
   static void AddCompanions(vector<IEditable *> &parts); // Appends the companions of the plastic hosts in 'parts' that are not in it
   static void FixPasted(const vector<IEditable *> &pasted, const vector<string> &originalNames); // Pasted companions follow the pasted host copies, else become plain primitives

   // Re-apply from F9 (the host is the selection)
   static void Adjust(Primitive *host, float height, float thickness, float bevel, int segs, float holeDia, float dpi);

private:
   static Primitive *NewPrimitive(EditorUI &ed, const string &name, float x, float y);
   static string UniqueName(EditorUI &ed, const string &name);
   static Material *EnsureMaterial(PinTable *table, int which);
   static float ParseDpi(const string &imageName);
   static void SetSheet(EditorUI &ed); // Default mapping for the image and dpi, the sheet centred on the table
   static string DefaultImage(const EditorUI &ed);
};

}
