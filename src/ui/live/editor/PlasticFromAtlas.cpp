// license:GPLv3+

#include "core/stdafx.h"
#include "core/editablereg.h"
#include "core/player.h"
#include "PlasticFromAtlas.h"
#include "EditorUIPart.h"
#include "EditorTheme.h"
#include "LiveRenderContext.h"
#include "Selection.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "math/PlasticMesh.h"
#include "parts/Material.h"
#include "parts/primitive.h"
#include "physics/PhysicsEngine.h"
#include "renderer/Renderer.h"
#include "renderer/Texture.h"

namespace VPX::EditorUI
{

namespace
{
string s_lastImage; // The atlas used last, offered first next time

bool IsHost(const IEditable *part) { return part != nullptr && part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->m_d.m_plasticGen; }

// Inverse of the table-to-UV mapping: the table position of an atlas UV
Vertex2D UvToTable(const float (&m)[6], const float u, const float v)
{
   const float det = m[0] * m[4] - m[1] * m[3];
   if (fabsf(det) < 1e-12f)
      return Vertex2D(0.f, 0.f);
   const float du = u - m[2], dv = v - m[5];
   return Vertex2D((m[4] * du - m[1] * dv) / det, (-m[3] * du + m[0] * dv) / det);
}

}

Primitive *PlasticFromAtlas::NewPrimitive(EditorUI &ed, const string &name, const float x, const float y)
{
   IEditable *const pie = EditableRegistry::CreateAndInit(eItemPrimitive, ed.m_table, x, y);
   if (pie == nullptr)
      return nullptr;
   if (auto *const scriptable = pie->GetIScriptable(); scriptable)
      scriptable->m_name = name;
   pie->m_desktopBackdrop = (ed.m_camMode == ViewMode::DesktopBackdrop);
   ed.m_table->AddPart(pie);
   pie->SetPartGroup(ed.GetPartGroupForNewPart());
   Primitive *const prim = static_cast<Primitive *>(pie);
   prim->m_d.m_toy = true;
   prim->m_d.m_collidable = false;
   prim->m_d.m_reflectionEnabled = false;
   return prim;
}

string PlasticFromAtlas::UniqueName(EditorUI &ed, const string &name) { return ed.m_table->IsNameUnique(name) ? name : ed.m_table->GetUniqueName(name); }

float PlasticFromAtlas::ParseDpi(const string &imageName)
{
   string s = imageName;
   for (char &c : s)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
   for (size_t pos = s.find("dpi"); pos != string::npos; pos = s.find("dpi", pos + 1))
   {
      size_t end = pos;
      while (end > 0 && isdigit(static_cast<unsigned char>(s[end - 1])))
         end--;
      if (end < pos)
      {
         const float v = static_cast<float>(atof(s.substr(end, pos - end).c_str()));
         if (v >= 10.f && v <= 4800.f)
            return v;
      }
   }
   return 150.f;
}

string PlasticFromAtlas::DefaultImage(const EditorUI &ed)
{
   const vector<Texture *> &images = ed.m_table->GetImageList();
   for (const Texture *const tex : images)
      if (tex->m_name == s_lastImage)
         return tex->m_name;
   // An image with "plastic" in its name; the biggest one if there are several (the atlas, not a part's texture)
   const Texture *best = nullptr;
   for (const Texture *const tex : images)
   {
      string s = tex->m_name;
      for (char &c : s)
         c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      if (s.find("plastic") != string::npos && (best == nullptr || static_cast<uint64_t>(tex->m_width) * tex->m_height > static_cast<uint64_t>(best->m_width) * best->m_height))
         best = tex;
   }
   if (best != nullptr)
      return best->m_name;
   return images.empty() ? string() : images.front()->m_name;
}

// The sheet centred on the table, at the scale of the dpi
void PlasticFromAtlas::SetSheet(EditorUI &ed)
{
   Trace &t = ed.m_plasticTrace;
   t.dpi = clamp(t.dpi, 10.f, 4800.f);
   const Texture *const tex = ed.m_table->GetImage(t.image);
   const float w = tex != nullptr && tex->m_width > 0 ? static_cast<float>(tex->m_width) : 1024.f;
   const float h = tex != nullptr && tex->m_height > 0 ? static_cast<float>(tex->m_height) : 1024.f;
   const float sw = w * kVPUPerInch / t.dpi, sh = h * kVPUPerInch / t.dpi;
   const float x0 = 0.5f * (ed.m_table->m_right + ed.m_table->m_left) - 0.5f * sw, y0 = 0.5f * (ed.m_table->m_bottom + ed.m_table->m_top) - 0.5f * sh;
   t.uvm[0] = 1.f / sw;
   t.uvm[1] = 0.f;
   t.uvm[2] = -x0 / sw;
   t.uvm[3] = 0.f;
   t.uvm[4] = 1.f / sh;
   t.uvm[5] = -y0 / sh;
}

bool PlasticFromAtlas::CanStart(const EditorUI &ed)
{
   return !ed.m_table->IsLocked() && !ed.IsInspectMode() && ed.m_pointEditPart == nullptr && !ed.m_plasticTrace.active;
}

void PlasticFromAtlas::Start(EditorUI &ed)
{
   if (!CanStart(ed))
      return;
   if (ed.m_table->GetImageList().empty())
   {
      ed.Report("Plastic from Atlas: the table has no image (import the atlas first)");
      return;
   }
   ed.m_addPartType = eItemInvalid;
   ed.ClearSelection();
   Trace t;
   t.active = true;
   t.image = DefaultImage(ed);
   t.dpi = ParseDpi(t.image);
   t.holeDia = 12.f;
   t.prevPerspective = ed.m_perspectiveCam;
   t.prevView = ed.m_camView;
   t.prevDistance = ed.m_camDistance;
   t.prevPredefined = static_cast<int>(ed.m_predefinedView);
   ed.m_plasticTrace = t;
   SetSheet(ed);
   // Top view, orthographic, framed on the sheet
   const Vertex2D a = UvToTable(ed.m_plasticTrace.uvm, 0.f, 0.f), b = UvToTable(ed.m_plasticTrace.uvm, 1.f, 1.f);
   ed.m_perspectiveCam = false;
   ed.SetView(EditorUI::PredefinedView::Top);
   ed.FrameBounds(FRect3D(a.x, b.x, a.y, b.y, 0.f, 0.f));
   ed.Report("Plastic from Atlas: LMB a corner where the line changes direction, Backspace removes the last, H holes, Enter closes, Esc cancels");
}

void PlasticFromAtlas::Cancel(EditorUI &ed)
{
   if (!ed.m_plasticTrace.active)
      return;
   ed.m_perspectiveCam = ed.m_plasticTrace.prevPerspective;
   ed.m_camView = ed.m_plasticTrace.prevView;
   ed.m_camDistance = ed.m_plasticTrace.prevDistance;
   ed.m_predefinedView = static_cast<EditorUI::PredefinedView>(ed.m_plasticTrace.prevPredefined);
   ed.m_plasticTrace = Trace();
   ed.Report("Plastic from Atlas cancelled");
}

void PlasticFromAtlas::Click(EditorUI &ed, const ImVec2 &mouse, const bool shift)
{
   Trace &t = ed.m_plasticTrace;
   const Vertex2D p = ed.UnprojectToPlane(mouse, 0.f);
   (void)shift;
   PLOGI << "Plastic from Atlas: " << (t.holes ? "hole" : "corner") << " at table " << p.x << ' ' << p.y;
   if (t.holes)
      t.holeList.push_back(Vertex3Ds(p.x, p.y, t.holeDia));
   else
   {
      t.pts.push_back(Vertex3Ds(p.x, p.y, 0.f));
      t.smooth.push_back(false);
   }
}

void PlasticFromAtlas::Keys(EditorUI &ed)
{
   Trace &t = ed.m_plasticTrace;
   const ImGuiIO &io = ImGui::GetIO();
   if (io.WantCaptureKeyboard)
      return;
   if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
      Finish(ed);
   else if (ImGui::IsKeyPressed(ImGuiKey_Backspace))
   {
      if (t.holes && !t.holeList.empty())
         t.holeList.pop_back();
      else if (!t.holes && !t.pts.empty())
      {
         t.pts.pop_back();
         t.smooth.pop_back();
      }
   }
   else if (ImGui::IsKeyPressed(ImGuiKey_H, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
   {
      t.holes = !t.holes;
      ed.Report(t.holes ? "Plastic from Atlas: placing holes (LMB), H back to the outline" : "Plastic from Atlas: tracing the outline, H for holes");
   }
}

Material *PlasticFromAtlas::EnsureMaterial(PinTable *const table, const int which)
{
   static constexpr const char *names[3] = { "PBR.Plastic_Print_Plastics", "PBR.Plastic_Clear1", "PBR.Plastic_Clear1_Bevel" };
   if (Material *const existing = table->GetMaterial(names[which]); !table->IsDummyMaterial(existing)) // GetMaterial never returns null
      return existing;
   // Police Force's values
   Material *const m = new Material();
   m->m_name = names[which];
   m->m_type = Material::PBR;
   m->m_fGlossyImageLerp = 1.f;
   m->m_fEdge = 1.f;
   m->m_cBase = 0xFFFFFF;
   m->m_cGlossy = 0;
   m->m_cClearcoat = 0;
   m->m_cRefractionTint = 0xFFFFFF;
   m->m_fMetallic = 0.f;
   m->m_fOcclusion = 1.f;
   m->m_cEmission = 0;
   m->m_fEmissionIntensity = 1.f;
   m->m_fElasticity = 0.f;
   m->m_fElasticityFalloff = 0.f;
   m->m_fFriction = 0.f;
   m->m_fScatterAngle = 0.f;
   if (which == 0)
   {
      m->m_fWrapLighting = 0.08f;
      m->m_fRoughness = 0.35f;
      m->m_fThickness = 0.05f;
      m->m_fEdgeAlpha = 0.f;
      m->m_fOpacity = 1.f;
      m->m_bOpacityActive = false;
      m->m_fClearcoat = 1.f;
      m->m_fClearcoatRoughness = 0.03f;
   }
   else
   {
      m->m_fWrapLighting = 0.f;
      m->m_fRoughness = which == 1 ? 0.07f : 0.045f;
      m->m_fThickness = 0.5f;
      m->m_fEdgeAlpha = 1.f;
      m->m_fOpacity = which == 1 ? 0.1f : 0.15f;
      m->m_bOpacityActive = true;
      m->m_fMetallic = which == 1 ? 0.f : 0.3f;
      m->m_fScatter = 0.19f;
      m->m_fAbsorption = 0.3f;
      m->m_cAbsorption = 14083821;
   }
   table->AddMaterial(m);
   table->SetNonUndoableDirty(SaveDirtyState::Dirty);
   return m;
}

void PlasticFromAtlas::Finish(EditorUI &ed)
{
   Trace &t = ed.m_plasticTrace;
   if (!t.active)
      return;
   if (t.pts.size() < 3)
   {
      ed.Report("Plastic from Atlas: the outline needs 3 points or more");
      return;
   }
   int crossing = 0;
   {
      // Build it first: a degenerate outline (collinear clicks) must not leave three default primitives behind
      vector<PlasticMesh::CurvePoint> outline;
      for (const Vertex3Ds &p : t.pts)
         outline.push_back({ p.x, p.y, false });
      vector<PlasticMesh::Hole> holes;
      for (const Vertex3Ds &h : t.holeList)
         holes.push_back({ h.x, h.y, h.z });
      PlasticMesh::Result probe;
      if (!PlasticMesh::Build(outline, holes, PlasticMesh::Params(), probe))
      {
         ed.Report("Plastic from Atlas: the outline has no area (Backspace to fix it)");
         return;
      }
      if (probe.skippedHoles > 0)
         ed.Report("Plastic from Atlas: " + std::to_string(probe.skippedHoles) + " hole(s) outside the outline are left out");
      crossing = probe.crossingHoles;
   }
   const float cx = 0.5f * (ed.m_table->m_left + ed.m_table->m_right), cy = 0.5f * (ed.m_table->m_top + ed.m_table->m_bottom);
   const string hostName = ed.m_table->GetUniqueName("Plastic"s);
   const string clearName = UniqueName(ed, hostName + "_Clear"), bevelName = UniqueName(ed, hostName + "_Bevel");

   Primitive *const host = NewPrimitive(ed, hostName, cx, cy);
   Primitive *const clear = host != nullptr ? NewPrimitive(ed, clearName, cx, cy) : nullptr;
   Primitive *const bevel = clear != nullptr ? NewPrimitive(ed, bevelName, cx, cy) : nullptr;
   if (bevel == nullptr)
   {
      ed.Report("Plastic from Atlas: could not create the parts");
      return;
   }
   host->m_d.m_szImage = t.image;
   host->m_d.m_szMaterial = EnsureMaterial(ed.m_table, 0)->m_name;
   clear->m_d.m_szMaterial = EnsureMaterial(ed.m_table, 1)->m_name;
   bevel->m_d.m_szMaterial = EnsureMaterial(ed.m_table, 2)->m_name;
   host->m_d.m_plasticDpi = t.dpi;
   host->m_d.m_plasticHoleDia = t.holeDia;
   for (int i = 0; i < 6; i++)
      host->m_d.m_plasticUvm[i] = t.uvm[i];
   host->InitPlastic(t.pts, t.smooth, t.holeList);
   // The part moves so that its centre sits at the table centre, ready for G and R (the art stays on the part)
   {
      const Vertex2D c = host->GetCenter();
      host->Translate(Vertex2D(cx - c.x, cy - c.y));
   }
   Primitive *const comps[2] = { clear, bevel };
   for (int i = 0; i < 2; i++)
   {
      comps[i]->m_d.m_plasticHost = hostName;
      comps[i]->m_d.m_plasticRole = i + 1;
      comps[i]->SetUILock(true); // A viewport click lands on the host
   }
   host->RegeneratePlastic(); // The companions are in the table now

   for (Primitive *const prim : { host, clear, bevel })
   {
      ed.m_player->TimerSetup(prim);
      prim->GetIRenderable()->RenderSetup(ed.m_renderer.get());
      if (prim->GetIHitable())
         ed.m_player->m_physics->Add(prim);
   }

   ed.m_undo.BeginUndo();
   for (Primitive *const prim : { host, clear, bevel })
      ed.m_undo.MarkForCreate(prim);
   ed.m_undo.SetLabel("Add Plastic from Atlas");
   ed.m_undo.EndUndo();
   ed.Report("Added " + hostName + " (with " + clearName + " and " + bevelName + ")"
      + (crossing > 0 ? "  Warning: " + std::to_string(crossing) + (crossing == 1 ? " hole crosses" : " holes cross") + " the outline edge" : string()));
   host->m_plasticCrossingWarn = false; // Said in the message above

   s_lastImage = t.image;
   ed.m_perspectiveCam = t.prevPerspective;
   ed.m_plasticTrace = Trace();

   ed.UpdateEditableList();
   if (const auto it = ed.m_editableMap.find(host); it != ed.m_editableMap.end())
   {
      ed.SetSelection(Selection(it->second));
      ed.m_outlinerAnchor = it->second;
      Matrix3D placed;
      if (!ed.GetSelectionTransform(placed))
         placed.SetIdentity();
      ed.SetLastOperation(EditorUI::LastOperation::Type::PlasticFromAtlas, placed);
      ed.m_lastOp.plHeight = host->m_d.m_plasticHeight;
      ed.m_lastOp.plThickness = host->m_d.m_plasticThickness;
      ed.m_lastOp.plBevel = host->m_d.m_plasticBevel;
      ed.m_lastOp.plSegs = host->m_d.m_plasticBevelSegs;
      ed.m_lastOp.plHoleDia = host->m_d.m_plasticHoleDia;
      ed.m_lastOp.plDpi = host->m_d.m_plasticDpi;
      ed.FrameSelection();
   }
}

void PlasticFromAtlas::Adjust(Primitive *const host, const float height, const float thickness, const float bevel, const int segs, const float holeDia, const float dpi)
{
   host->m_d.m_plasticHeight = clamp(height, -1000.f, 2000.f);
   host->m_d.m_plasticThickness = clamp(thickness, 0.1f, 100.f);
   host->m_d.m_plasticBevel = clamp(bevel, 0.f, host->m_d.m_plasticThickness);
   host->m_d.m_plasticBevelSegs = clamp(segs, 1, 16);
   host->m_d.m_plasticHoleDia = clamp(holeDia, 0.5f, 100.f);
   if (fabsf(dpi - host->m_d.m_plasticDpi) > 1e-3f)
      host->SetPlasticDpi(dpi);
   host->UpdatePlastic();
}

void PlasticFromAtlas::SyncCompanionVisibility(EditorUI &ed)
{
   for (IEditable *const part : ed.m_table->GetParts())
      if (IsHost(part))
         for (Primitive *const comp : static_cast<Primitive *>(part)->GetPlasticCompanions())
            if (comp->IsUIVisible(false) != part->IsUIVisible(false))
               comp->SetUIVisible(part->IsUIVisible(false));
}

void PlasticFromAtlas::AddCompanions(vector<IEditable *> &parts)
{
   const size_t n = parts.size();
   for (size_t i = 0; i < n; i++)
      if (IsHost(parts[i]))
         for (Primitive *const comp : static_cast<Primitive *>(parts[i])->GetPlasticCompanions())
            if (std::ranges::find(parts, static_cast<IEditable *>(comp)) == parts.end())
               parts.push_back(comp);
}

void PlasticFromAtlas::FixPasted(const vector<IEditable *> &pasted, const vector<string> &originalNames)
{
   for (IEditable *const part : pasted)
   {
      if (part->GetItemType() != eItemPrimitive || static_cast<Primitive *>(part)->m_d.m_plasticHost.empty())
         continue;
      Primitive *const comp = static_cast<Primitive *>(part);
      bool found = false;
      for (size_t i = 0; i < pasted.size() && !found; i++)
         if (IsHost(pasted[i]) && StrCompareNoCase(originalNames[i], comp->m_d.m_plasticHost))
         {
            comp->m_d.m_plasticHost = pasted[i]->GetName(); // Follows the copy of its host
            found = true;
         }
      if (!found) // A companion alone is never pasted live: a plain primitive
      {
         comp->m_d.m_plasticHost.clear();
         comp->m_d.m_plasticRole = 0;
         comp->SetUILock(false);
      }
   }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Bowing a line into an arc (point edit of the outline): drag a line between two points = an arc through the cursor, either side (the smooth peak
// point between the two corners is inserted or moved); double-click a curve = straight again

namespace
{
struct Span
{
   int a, peak, b; // corner, smooth peak (-1 = a straight line), corner
};

vector<Span> Spans(const DragPointCurve &curve)
{
   vector<Span> spans;
   const auto &pts = curve.GetPoints();
   const int n = static_cast<int>(pts.size());
   for (int i = 0; i < n; i++)
   {
      if (pts[i]->IsSmooth())
         continue;
      const int j = (i + 1) % n, k = (i + 2) % n;
      if (pts[j]->IsSmooth() && !pts[k]->IsSmooth() && n >= 3)
         spans.push_back({ i, j, k });
      else if (!pts[j]->IsSmooth())
         spans.push_back({ i, -1, j });
   }
   return spans;
}

// Distance from p to the span as drawn (the arc is flattened by PlasticMesh)
float SpanDistance(const DragPointCurve &curve, const Span &sp, const Vertex2D &p)
{
   const auto &pts = curve.GetPoints();
   vector<PlasticMesh::CurvePoint> poly;
   if (sp.peak < 0)
      poly = { { pts[sp.a]->GetX(), pts[sp.a]->GetY(), false }, { pts[sp.b]->GetX(), pts[sp.b]->GetY(), false } };
   else
   {
      const vector<PlasticMesh::CurvePoint> tri { { pts[sp.a]->GetX(), pts[sp.a]->GetY(), false }, { pts[sp.peak]->GetX(), pts[sp.peak]->GetY(), true }, { pts[sp.b]->GetX(), pts[sp.b]->GetY(), false } };
      poly = PlasticMesh::FlattenOutline(tri, 0.25f); // A, the arc, B (the closing edge B to A is not part of it)
   }
   float best = FLT_MAX;
   for (size_t i = 0; i + 1 < poly.size(); i++)
   {
      const float dx = poly[i + 1].x - poly[i].x, dy = poly[i + 1].y - poly[i].y, l2 = dx * dx + dy * dy;
      const float t = l2 > 0.f ? clamp(((p.x - poly[i].x) * dx + (p.y - poly[i].y) * dy) / l2, 0.f, 1.f) : 0.f;
      best = min(best, hypotf(p.x - (poly[i].x + t * dx), p.y - (poly[i].y + t * dy)));
   }
   return best;
}
}

bool PlasticFromAtlas::BowPress(EditorUI &ed, const ImVec2 &mouse, const bool doubleClick)
{
   ed.m_plasticBow = Bow();
   if (ed.m_pointEditPart == nullptr || ed.m_pointEditCenter || !IsHost(ed.m_pointEditPart->GetEditable()))
      return false;
   Primitive *const host = static_cast<Primitive *>(ed.m_pointEditPart->GetEditable());
   if (host->m_plasticEditHoles || ed.m_table->IsLocked())
      return false;
   const float z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
   const Vertex2D p = ed.UnprojectToPlane(mouse, z), q = ed.UnprojectToPlane(ImVec2(mouse.x + 8.f * ed.m_liveUI.GetDPI(), mouse.y), z);
   const float tol = hypotf(q.x - p.x, q.y - p.y);
   PLOGI << "Plastic from Atlas: press at table " << p.x << ' ' << p.y;
   const vector<Span> spans = Spans(host->m_plasticCurve);
   int best = -1;
   float bestDist = tol;
   for (size_t i = 0; i < spans.size(); i++)
      if (const float d = SpanDistance(host->m_plasticCurve, spans[i], p); d < bestDist)
      {
         bestDist = d;
         best = static_cast<int>(i);
      }
   if (best < 0)
      return false;
   const Span &sp = spans[best];
   if (doubleClick)
   {
      if (sp.peak >= 0 && host->m_plasticCurve.GetPoints().size() > 3) // An outline keeps 3 points
      {
         ed.m_undo.BeginUndo();
         ed.m_undo.MarkForUndo(host);
         ed.m_undo.SetFieldLabel("Straighten Line");
         ed.m_undo.EndUndo();
         host->m_plasticCurve.DeletePoint(host->m_plasticCurve.GetPoints()[sp.peak].get());
         ed.m_pointSel.clear();
         ed.m_renderer->ReinitRenderable(host->GetIRenderable());
         ed.m_renderer->InvalidateShadowContent();
      }
      return true;
   }
   ed.m_plasticBow.pending = true;
   ed.m_plasticBow.start = mouse;
   ed.m_plasticBow.corner = host->m_plasticCurve.GetPoints()[sp.a].get();
   ed.m_plasticBow.peak = sp.peak >= 0 ? host->m_plasticCurve.GetPoints()[sp.peak].get() : nullptr;
   ed.m_plasticBow.endCorner = host->m_plasticCurve.GetPoints()[sp.b].get();
   return true;
}

void PlasticFromAtlas::BowDrag(EditorUI &ed)
{
   Bow &bow = ed.m_plasticBow;
   if (!bow.pending && !bow.active)
      return;
   if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ed.m_pointEditPart == nullptr || !IsHost(ed.m_pointEditPart->GetEditable()))
   {
      bow = Bow();
      return;
   }
   Primitive *const host = static_cast<Primitive *>(ed.m_pointEditPart->GetEditable());
   DragPointCurve &curve = host->m_plasticCurve;
   const ImVec2 mouse = ImGui::GetMousePos();
   if (bow.pending)
   {
      if (fabsf(mouse.x - bow.start.x) < 3.f && fabsf(mouse.y - bow.start.y) < 3.f)
         return;
      bow.pending = false;
      bow.active = true;
      ed.m_undo.BeginUndo();
      ed.m_undo.MarkForUndo(host);
      ed.m_undo.SetFieldLabel("Bow Curve");
      ed.m_undo.EndUndo();
      if (bow.peak == nullptr)
      {
         const int n = static_cast<int>(curve.GetPoints().size());
         int at = 0;
         for (int i = 0; i < n; i++)
            if (curve.GetPoints()[i].get() == bow.corner)
               at = i + 1;
         curve.InsertPoint(at, std::make_unique<DragPoint>(&curve, bow.corner->GetX(), bow.corner->GetY(), 0.f, true));
         bow.peak = curve.GetPoints()[at].get();
      }
   }
   const Vertex2D p = ed.UnprojectToPlane(mouse, host->m_d.m_plasticHeight + host->m_d.m_plasticThickness);
   bow.peak->SetX(p.x);
   bow.peak->SetY(p.y);
   curve.OnPointsModified();
   ed.m_pointSel.clear();
   ed.m_renderer->ReinitRenderable(host->GetIRenderable());
   ed.m_renderer->InvalidateShadowContent();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing

void PlasticFromAtlas::DrawBackdrop(EditorUI &ed, ImDrawList *const draw, const LiveRenderContext &ctx)
{
   const float *uvm = nullptr;
   float z = 0.f; // the plane of the handles: the top of a placed plastic, so the backdrop lines up in perspective too
   string image;
   if (ed.m_plasticTrace.active)
   {
      uvm = ed.m_plasticTrace.uvm;
      image = ed.m_plasticTrace.image;
   }
   else if (ed.m_pointEditPart != nullptr && IsHost(ed.m_pointEditPart->GetEditable()))
   {
      const Primitive *const host = static_cast<const Primitive *>(ed.m_pointEditPart->GetEditable());
      uvm = host->m_d.m_plasticUvm;
      image = host->m_d.m_szImage;
      z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
   }
   if (uvm == nullptr || image.empty())
      return;
   Texture *const texture = ed.m_table->GetImage(image);
   if (texture == nullptr)
      return;
   float m[6];
   for (int i = 0; i < 6; i++)
      m[i] = uvm[i];
   const float us[4] = { 0.f, 1.f, 1.f, 0.f }, vs[4] = { 0.f, 0.f, 1.f, 1.f };
   ImVec2 p[4];
   for (int i = 0; i < 4; i++)
   {
      const Vertex2D t = UvToTable(m, us[i], vs[i]);
      p[i] = ctx.Project(Vertex3Ds(t.x, t.y, z));
      if (p[i].x == FLT_MAX)
         return;
   }
   // A dark sheet under the atlas (its alpha shows the cut-outs against it)
   draw->AddQuadFilled(p[0], p[1], p[2], p[3], IM_COL32(18, 18, 22, 235));
   const auto sampler = ed.m_renderer->m_renderDevice->m_texMan.LoadTexture(texture, false);
   if (sampler)
      draw->AddImageQuad(sampler, p[0], p[1], p[2], p[3], ImVec2(0.f, 0.f), ImVec2(1.f, 0.f), ImVec2(1.f, 1.f), ImVec2(0.f, 1.f));
}

void PlasticFromAtlas::DrawTrace(EditorUI &ed, ImDrawList *const draw, const LiveRenderContext &ctx)
{
   const Trace &t = ed.m_plasticTrace;
   if (!t.active)
      return;
   const float dpi = ed.m_liveUI.GetDPI();
   const auto project = [&ctx](const float x, const float y) { return ctx.Project(Vertex3Ds(x, y, 0.f)); };

   // The outline as it will be built (arcs flattened), closed
   if (t.pts.size() >= 2)
   {
      vector<PlasticMesh::CurvePoint> pts;
      for (size_t i = 0; i < t.pts.size(); i++)
         pts.push_back({ t.pts[i].x, t.pts[i].y, static_cast<bool>(t.smooth[i]) });
      vector<PlasticMesh::CurvePoint> flat = t.pts.size() >= 3 ? PlasticMesh::FlattenOutline(pts, 0.25f) : pts;
      for (size_t i = 0; i < flat.size(); i++)
      {
         if (i + 1 == flat.size() && t.pts.size() < 3)
            break;
         const ImVec2 a = project(flat[i].x, flat[i].y), b = project(flat[(i + 1) % flat.size()].x, flat[(i + 1) % flat.size()].y);
         if (a.x != FLT_MAX && b.x != FLT_MAX)
            draw->AddLine(a, b, IM_COL32(255, 210, 60, 255), 1.5f * dpi);
      }
   }
   for (size_t i = 0; i < t.pts.size(); i++)
   {
      const ImVec2 pos = project(t.pts[i].x, t.pts[i].y);
      if (pos.x == FLT_MAX)
         continue;
      const float radius = (i == 0 ? 5.f : 4.f) * dpi;
      draw->AddCircleFilled(pos, radius, t.smooth[i] ? IM_COL32(80, 160, 255, 255) : IM_COL32(255, 96, 96, 255), 12);
      draw->AddCircle(pos, radius + dpi, IM_COL32(0, 0, 0, 255), 12, 1.5f);
   }
   const auto circle = [&](const float x, const float y, const float dia, const ImU32 col, const float thickness)
   {
      const ImVec2 c = project(x, y), e = project(x + 0.5f * dia, y);
      if (c.x == FLT_MAX || e.x == FLT_MAX)
         return;
      draw->AddCircle(c, max(2.f, sqrtf((e.x - c.x) * (e.x - c.x) + (e.y - c.y) * (e.y - c.y))), col, 32, thickness);
   };
   for (const Vertex3Ds &h : t.holeList)
      circle(h.x, h.y, h.z, IM_COL32(90, 230, 120, 255), 1.5f * dpi);
   if (t.holes && !ImGui::GetIO().WantCaptureMouse)
   {
      const Vertex2D p = ed.UnprojectToPlane(ImGui::GetMousePos(), 0.f);
      circle(p.x, p.y, t.holeDia, IM_COL32(90, 230, 120, 140), dpi);
   }
   else if (!t.holes && !t.pts.empty() && !ImGui::GetIO().WantCaptureMouse)
   {
      const ImVec2 last = project(t.pts.back().x, t.pts.back().y);
      if (last.x != FLT_MAX)
         draw->AddLine(last, ImGui::GetMousePos(), IM_COL32(255, 210, 60, 110), dpi);
   }
}

void PlasticFromAtlas::RenderPanel(EditorUI &ed)
{
   Trace &t = ed.m_plasticTrace;
   if (!t.active || !ed.m_viewportVisible)
      return;
   const float dpi = ed.m_liveUI.GetDPI();
   ImGui::SetNextWindowPos(ImVec2(ed.m_viewportContentRect.x0 + 8.f * dpi, ed.m_viewportContentRect.y1 - 8.f * dpi), ImGuiCond_Always, ImVec2(0.f, 1.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * dpi, 6.f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::V(EditorTheme::Border));
   if (ImGui::Begin("Plastic from Atlas##Trace", nullptr,
          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
             | ImGuiWindowFlags_NoMove))
   {
      ImGui::Text("Plastic from Atlas: trace    %d point(s), %d hole(s)", static_cast<int>(t.pts.size()), static_cast<int>(t.holeList.size()));
      ImGui::SetNextItemWidth(300.f * dpi);
      if (ImGui::BeginCombo("Atlas", t.image.c_str()))
      {
         for (const Texture *const tex : ed.m_table->GetImageList())
            if (ImGui::Selectable(tex->m_name.c_str(), tex->m_name == t.image) && tex->m_name != t.image)
            {
               t.image = tex->m_name;
               t.dpi = ParseDpi(t.image);
               SetSheet(ed);
            }
         ImGui::EndCombo();
      }
      ImGui::SetNextItemWidth(90.f * dpi);
      if (ImGui::DragFloat("Sheet DPI", &t.dpi, 0.5f, 10.f, 2400.f, "%.1f"))
         SetSheet(ed);
      if (ImGui::IsItemDeactivatedAfterEdit()) // Keep the whole sheet in view at its new size
      {
         const Vertex2D a = UvToTable(t.uvm, 0.f, 0.f), b = UvToTable(t.uvm, 1.f, 1.f);
         ed.FrameBounds(FRect3D(a.x, b.x, a.y, b.y, 0.f, 0.f));
      }
      ImGui::SetItemTooltip("47.06 VPU per inch at this DPI. Traced points stay where they are, the sheet is rescaled about its centre");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.f * dpi);
      ImGui::DragFloat("Hole", &t.holeDia, 0.1f, 0.5f, 100.f, "%.1f");
      ImGui::SetItemTooltip("Hole diameter");
      ImGui::SameLine();
      if (ImGui::Button("M3 6.4"))
         t.holeDia = 6.4f;
      ImGui::SameLine();
      if (ImGui::Button("Post 12"))
         t.holeDia = 12.f;
      ImGui::SameLine();
      if (ImGui::Button("Large 16"))
         t.holeDia = 16.f;
      if (ImGui::RadioButton("Outline", !t.holes))
         t.holes = false;
      ImGui::SameLine();
      if (ImGui::RadioButton("Holes (H)", t.holes))
         t.holes = true;
      ImGui::SameLine();
      if (ImGui::Button("Close Loop (Enter)"))
         Finish(ed);
      ImGui::SameLine();
      if (ImGui::Button("Cancel (Esc)"))
         Cancel(ed);
   }
   ImGui::End();
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(2);
}

}
