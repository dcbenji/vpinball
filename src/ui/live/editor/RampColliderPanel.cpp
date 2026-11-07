// license:GPLv3+

// Ramp Collider in the editor: Add > Ramp Collider, the Properties section and the overlay of the selected collider (see math/rampcollider.h and
// SPEC-ramp-collider.md).

#include "core/stdafx.h"
#include "core/editablereg.h"
#include "core/player.h"
#include "PrimitiveUIPart.h"
#include "RampColliderPanel.h"
#include "EditorUIPart.h"
#include "Selection.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "parts/PartGroup.h"
#include "parts/ramp.h"
#include "physics/PhysicsEngine.h"
#include "renderer/Renderer.h"

namespace VPX::EditorUI
{

namespace
{
constexpr ImU32 kFloorFill = IM_COL32(40, 220, 90, 56);
constexpr ImU32 kFloorEdge = IM_COL32(40, 220, 90, 170);
constexpr ImU32 kWallFill = IM_COL32(40, 200, 210, 64);
constexpr ImU32 kRoute = IM_COL32(255, 255, 255, 200);
constexpr ImU32 kError = IM_COL32(255, 80, 70, 255);
constexpr ImU32 kAmber = IM_COL32(255, 190, 60, 255);

// A plain primitive: no generator, not a collider or one of its companions. What a collider can follow
bool IsPlainPrimitive(const IEditable *const part)
{
   if (part == nullptr || part->GetItemType() != eItemPrimitive)
      return false;
   const Primitive *const p = static_cast<const Primitive *>(part);
   return !p->IsPlasticCompanion() && !p->m_d.m_wireGen && !p->m_d.m_wireRail && !p->m_d.m_plasticGen && !p->m_d.m_playfieldGen && !p->m_d.m_rampCollider;
}

Primitive *FindReference(const Primitive &collider)
{
   if (collider.m_d.m_rampReference.empty() || collider.GetPTable() == nullptr)
      return nullptr;
   for (IEditable *const part : collider.GetPTable()->GetParts())
      if (IsPlainPrimitive(part) && StrCompareNoCase(part->GetName(), collider.m_d.m_rampReference))
         return static_cast<Primitive *>(part);
   return nullptr;
}

// The Ramps (the table's native ones) named by the Reference, which may list several names separated by commas: a route over two joined ramps.
// Names are trimmed and de-duplicated; 'names' counts them and 'missing' lists those that are no Ramp
vector<Ramp *> FindReferenceRamps(const Primitive &collider, int *const names = nullptr, string *const missing = nullptr)
{
   vector<Ramp *> ramps;
   if (names != nullptr)
      *names = 0;
   if (collider.m_d.m_rampReference.empty() || collider.GetPTable() == nullptr)
      return ramps;
   vector<string> seen;
   size_t from = 0;
   while (from <= collider.m_d.m_rampReference.size())
   {
      size_t to = collider.m_d.m_rampReference.find(',', from);
      if (to == string::npos)
         to = collider.m_d.m_rampReference.size();
      string name = collider.m_d.m_rampReference.substr(from, to - from);
      from = to + 1;
      const size_t a = name.find_first_not_of(" \t"), b = name.find_last_not_of(" \t");
      name = a == string::npos ? string() : name.substr(a, b - a + 1);
      if (name.empty() || std::any_of(seen.begin(), seen.end(), [&](const string &n) { return StrCompareNoCase(n, name); }))
         continue;
      seen.push_back(name);
      Ramp *found = nullptr;
      for (IEditable *const part : collider.GetPTable()->GetParts())
         if (part->GetItemType() == eItemRamp && StrCompareNoCase(part->GetName(), name))
            found = static_cast<Ramp *>(part);
      if (found != nullptr)
         ramps.push_back(found);
      else if (missing != nullptr)
         *missing += (missing->empty() ? "" : ", ") + name;
   }
   if (names != nullptr)
      *names = (int)seen.size();
   return ramps;
}

// Fit to Ramp: each point's height and width from the nearest of the Ramps' central curves, by the Ramp's own interpolation (bottom to top along its curve;
// a wire or habitrail Ramp gives its rail spacing as the width). With Flush Entry point 1's height is left as it is (the entry is flat on the playfield at
// 0), its width is still fitted
string FitToRamps(Primitive &collider, const vector<Ramp *> &ramps, const int names, const string &missing)
{
   const auto &points = collider.m_wireCurve.GetPoints();
   int fitted = 0;
   bool keptFirst = false;
   for (size_t k = 0; k < points.size(); k++)
   {
      vector<RampCollider::SurfaceFit> cands;
      for (const Ramp *const r : ramps)
      {
         RampCollider::SurfaceFit f;
         f.ok = r->GetSurfaceFit(points[k]->GetX(), points[k]->GetY(), f.height, f.width, f.dist);
         cands.push_back(f);
      }
      RampCollider::SurfaceFit best;
      if (!RampCollider::NearestFit(cands, best))
         continue;
      points[k]->m_width = best.width;
      if (k == 0 && collider.m_d.m_rampFlushEntry)
         keptFirst = true;
      else
         points[k]->SetZ(best.height);
      fitted++;
   }
   collider.UpdateRampCollider();
   string msg = "Fitted " + std::to_string(fitted) + " of " + std::to_string(points.size()) + " points (height and width) to " + std::to_string(ramps.size()) + (ramps.size() == 1 ? " ramp" : " ramps");
   if (!missing.empty())
      msg += "; " + std::to_string(ramps.size()) + " of " + std::to_string(names) + " names found: " + missing + " missing";
   if (keptFirst)
      msg += "; point 1 height left as it was (Flush Entry), its width fitted";
   return msg;
}

// Snap Heights to Reference: the height of the reference under each point, from the up-facing face nearest the point's own height (the closed shell of a
// ramp has an underside and may have a cover). Returns what it did, for the panel
string SnapHeights(Primitive &collider, Primitive &reference)
{
   vector<RampCollider::V3> verts;
   vector<unsigned> idx;
   reference.GetWorldMesh(verts, idx);
   const RampCollider::V3 up = collider.GetRampParams().up;
   const auto &points = collider.m_wireCurve.GetPoints();
   int snapped = 0;
   string missed, kept;
   for (size_t k = 0; k < points.size(); k++)
   {
      if (k == 0 && collider.m_d.m_rampFlushEntry)
      {
         kept = "point 1 kept at 0 (Flush Entry)";
         continue;
      }
      float z = 0.f;
      if (RampCollider::UpFacingHeight(verts, idx, points[k]->GetX(), points[k]->GetY(), points[k]->GetZ(), up, 0.7f, z))
      {
         points[k]->SetZ(z);
         snapped++;
      }
      else
         missed += (missed.empty() ? "" : ", ") + std::to_string(k + 1);
   }
   collider.UpdateRampCollider();
   string msg = "Snapped " + std::to_string(snapped) + " of " + std::to_string(points.size()) + " points";
   if (!kept.empty())
      msg += "; " + kept;
   if (!missed.empty())
      msg += "; no up-facing surface under point " + missed;
   return msg;
}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Add

bool RampColliderTool::CanAdd(const EditorUI &ed)
{
   return !ed.m_table->IsLocked() && !ed.IsInspectMode() && ed.m_player != nullptr && ed.m_player->m_physics != nullptr && ed.m_renderer != nullptr && ed.m_pointEditPart == nullptr;
}

bool RampColliderTool::HiddenInPlayShown(const EditorUI &ed) { return ed.IsHiddenInPlayPickable(nullptr); }

void RampColliderTool::Add(EditorUI &ed)
{
   if (!CanAdd(ed))
      return;
   PinTable *const t = ed.m_table;
   // The reference: the one selected plain primitive
   Primitive *reference = nullptr;
   if (ed.m_selection.GetType() == Selection::S_EDITABLE && ed.m_multiSel.size() <= 1 && ed.m_selection.GetPart() != nullptr && IsPlainPrimitive(ed.m_selection.GetPart()->GetEditable()))
      reference = static_cast<Primitive *>(ed.m_selection.GetPart()->GetEditable());

   // The route: along the longer of x and y of the reference's bounding box, from its lower end, 0 to its top less a wall, else 500 long at the 3D cursor
   vector<Vertex3Ds> route;
   if (reference != nullptr)
   {
      vector<RampCollider::V3> verts;
      vector<unsigned> idx;
      reference->GetWorldMesh(verts, idx);
      if (verts.empty())
         reference = nullptr;
      else
      {
         RampCollider::V3 lo = verts[0], hi = verts[0];
         for (const RampCollider::V3 &v : verts)
         {
            lo = { std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z) };
            hi = { std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z) };
         }
         const bool alongX = hi.x - lo.x > hi.y - lo.y;
         // The lower end: the mean height of the vertices in the outer fifth at each end
         const float a0 = alongX ? lo.x : lo.y, a1 = alongX ? hi.x : hi.y, fifth = (a1 - a0) * 0.2f;
         double sumLo = 0., sumHi = 0.;
         int nLo = 0, nHi = 0;
         for (const RampCollider::V3 &v : verts)
         {
            const float a = alongX ? v.x : v.y;
            if (a < a0 + fifth)
            {
               sumLo += v.z;
               nLo++;
            }
            if (a > a1 - fifth)
            {
               sumHi += v.z;
               nHi++;
            }
         }
         const bool startAtMin = nLo == 0 || nHi == 0 || sumLo / nLo <= sumHi / nHi;
         const float cx = 0.5f * (lo.x + hi.x), cy = 0.5f * (lo.y + hi.y);
         const float endZ = std::max(10.f, hi.z - RampCollider::kDefaultWall);
         const float s = startAtMin ? a0 : a1, e = startAtMin ? a1 : a0;
         route.push_back(alongX ? Vertex3Ds(s, cy, 0.f) : Vertex3Ds(cx, s, 0.f));
         route.push_back(alongX ? Vertex3Ds(e, cy, endZ) : Vertex3Ds(cx, e, endZ));
      }
   }
   if (reference == nullptr)
   {
      const vec3 &c = ed.Get3DCursor();
      route.clear();
      route.push_back(Vertex3Ds(c.x, c.y + WireRail::kDefaultLength * 0.5f, 0.f)); // The front end first: it climbs toward the back of the table
      route.push_back(Vertex3Ds(c.x, c.y - WireRail::kDefaultLength * 0.5f, 60.f));
   }

   const auto unique = [t](const string &name)
   {
      string res = name;
      for (int n = 2; !t->IsNameUnique(res); n++)
         res = name + std::to_string(n);
      return res;
   };
   if (const PartGroup *const g = ed.GetPartGroupForNewPart(); g != nullptr && g->GetReferenceSpace() != PartGroupData::SpaceReference::SR_PLAYFIELD)
   {
      ed.Report("Add Ramp Collider: the new part would go into a group that is not in playfield space, where it would not collide; select another group first");
      return;
   }
   IEditable *const hostPie = EditableRegistry::CreateAndInit(eItemPrimitive, t, route[0].x, route[0].y);
   if (hostPie == nullptr)
      return;
   Primitive *const host = static_cast<Primitive *>(hostPie);
   host->GetIScriptable()->m_name = unique("RampCollider"s);
   host->m_desktopBackdrop = false;
   host->InitRampCollider(route);
   if (reference != nullptr)
      host->m_d.m_rampReference = reference->GetName();
   t->AddPart(host);
   host->SetPartGroup(ed.GetPartGroupForNewPart());
   // The walls: a hidden part of their own (friction 0.02 against the floor's 0.3), not in the outliner, deleted, copied and moved with the collider
   Primitive *walls = static_cast<Primitive *>(EditableRegistry::CreateAndInit(eItemPrimitive, t, route[0].x, route[0].y));
   if (walls == nullptr)
   {
      t->RemovePart(host);
      host->Release();
      return;
   }
   walls->GetIScriptable()->m_name = unique(host->GetName() + "_Walls");
   walls->m_desktopBackdrop = false;
   t->AddPart(walls);
   walls->SetPartGroup(host->GetPartGroup());
   walls->m_d.m_plasticHost = host->GetName();
   walls->m_d.m_plasticRole = kRampWallsRole;
   walls->m_d.m_visible = false;
   walls->m_d.m_reflectionEnabled = false;
   walls->m_d.m_collision_reductionFactor = 0.f;
   walls->m_d.m_collidable = true;
   walls->m_d.m_toy = false;
   walls->m_d.m_overwritePhysics = true;
   walls->m_d.m_elasticity = 0.3f;
   walls->m_d.m_friction = 0.02f; // King Kong's rule: walls want much less friction than the floor
   walls->m_d.m_scatter = 0.f;
   walls->m_d.m_elasticityFalloff = 0.f;
   walls->SetUILock(true);
   host->RegenerateRampCollider(); // The walls part is in the table now: its mesh
   for (Primitive *const prim : { walls, host })
   {
      ed.m_player->TimerSetup(prim);
      prim->GetIRenderable()->RenderSetup(ed.m_renderer.get());
      if (prim->GetIHitable())
         ed.m_player->m_physics->Add(prim);
   }
   ed.m_undo.BeginUndo();
   ed.m_undo.MarkForCreate(host);
   ed.m_undo.MarkForCreate(walls);
   ed.m_undo.SetLabel("Add Ramp Collider"s);
   ed.m_undo.EndUndo();
   ed.UpdateEditableList();
   if (const auto it = ed.m_editableMap.find(host); it != ed.m_editableMap.end())
   {
      ed.SetSelection(Selection(it->second));
      ed.m_outlinerAnchor = it->second;
   }
   ed.m_renderer->InvalidateShadowContent();
   ed.Report("Added " + host->GetName() + (reference != nullptr ? " over " + reference->GetName() : string()) + (host->GetRampError().empty() ? string() : ": " + host->GetRampError()));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Overlay

void PrimitiveUIPart::RenderRampColliderOverlay(const EditorRenderContext &ctx)
{
   ImDrawList *const draw = ctx.GetDrawList();
   if (draw == nullptr)
      return;
   const RampCollider::Result &r = m_part->GetRampResult();
   if (!m_part->GetRampError().empty())
   {
      // No collider: said over the route at once, the panel says why
      const vector<WireRail::Frame> &frames = r.frames;
      if (!frames.empty())
      {
         const ImVec2 at = ctx.Project(Vertex3Ds(frames[frames.size() / 2].p.x, frames[frames.size() / 2].p.y, frames[frames.size() / 2].p.z));
         if (at.x != FLT_MAX)
            draw->AddText(at, kError, "No collider");
      }
   }
   if (!ctx.IsSelected() || !r.fits)
      return;
   const auto project = [&ctx](const RampCollider::V3 &v) { return ctx.Project(Vertex3Ds(v.x, v.y, v.z)); };
   const RampCollider::Mesh &fl = r.floor;
   for (size_t i = 0; i + 3 < fl.verts.size(); i += 2) // A quad per row span: left, right of this row, then the next row's
   {
      const ImVec2 a = project(fl.verts[i]), b = project(fl.verts[i + 1]), c = project(fl.verts[i + 3]), d = project(fl.verts[i + 2]);
      if (a.x != FLT_MAX && b.x != FLT_MAX && c.x != FLT_MAX && d.x != FLT_MAX)
      {
         draw->AddQuadFilled(a, b, c, d, kFloorFill);
         draw->AddLine(a, d, kFloorEdge);
         draw->AddLine(b, c, kFloorEdge);
      }
   }
   const RampCollider::Mesh &wl = r.walls; // Every triangle is there twice (both windings): the first of each pair
   for (size_t i = 0; i + 5 < wl.idx.size(); i += 6)
   {
      const ImVec2 a = project(wl.verts[wl.idx[i]]), b = project(wl.verts[wl.idx[i + 1]]), c = project(wl.verts[wl.idx[i + 2]]);
      if (a.x != FLT_MAX && b.x != FLT_MAX && c.x != FLT_MAX)
         draw->AddTriangleFilled(a, b, c, kWallFill);
   }
   for (size_t i = 0; i + 1 < r.frames.size(); i++)
   {
      const ImVec2 a = project(r.frames[i].p), b = project(r.frames[i + 1].p);
      if (a.x != FLT_MAX && b.x != FLT_MAX)
         draw->AddLine(a, b, kRoute, 1.5f);
   }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Properties

void PrimitiveUIPart::UpdateRampColliderPanel(PropertyPane &props)
{
   // The collider is made here: these sections stay open when the full sheet is collapsed under Advanced, as the point edit tools do
   const bool skipping = props.IsSkippingRest();
   if (!props.IsHarvest())
      props.SkipRest(false);
   const auto done = [&]() { props.SkipRest(skipping); };
   Primitive *const edited = props.GetEditedPart<Primitive>(m_part);

   if (!props.BeginSection("Ramp Collider"s))
   {
      done();
      return;
   }
   if (props.IsMultiEdit())
   {
      props.FlushRow();
      ImGui::TextDisabled("Select one ramp collider to edit it");
      props.EndSection();
      done();
      return;
   }

   // Reference
   props.Separator("Reference"s);
   {
      vector<string> names { "(none)"s };
      int current = 0;
      for (IEditable *const part : edited->GetPTable()->GetParts())
         if ((IsPlainPrimitive(part) || part->GetItemType() == eItemRamp) && !part->GetName().empty())
         {
            names.push_back(part->GetName());
            if (StrCompareNoCase(part->GetName(), edited->m_d.m_rampReference))
               current = (int)names.size() - 1;
         }
      if (current == 0 && !edited->m_d.m_rampReference.empty())
      {
         names.push_back(edited->m_d.m_rampReference); // A name that is no part: kept as it is, said below
         current = (int)names.size() - 1;
      }
      props.Combo<Primitive>(
         m_part, "Reference"s, names, //
         [current](const Primitive *) { return current; }, //
         [names](Primitive *p, int v) { p->m_d.m_rampReference = v > 0 && v < (int)names.size() ? names[v] : string(); },
         "The imported ramp whose look this collider follows (Snap Heights reads its surface), or a native Ramp part (Fit to Ramp reads its curve)");
   }
   props.InputString<Primitive>(
      m_part, "Reference Names"s, //
      [](const Primitive *p) { return p->m_d.m_rampReference; }, //
      [](Primitive *p, const string &v) { p->m_d.m_rampReference = v; },
      "The reference as text: for a route over several native Ramps, their names separated by commas (Fit to Ramp takes the nearest for each point). Renaming a part updates it, also inside the list");
   Primitive *const reference = FindReference(*edited);
   int refNames = 0;
   string refMissing;
   const vector<Ramp *> refRamps = FindReferenceRamps(*edited, &refNames, &refMissing);
   if (!edited->m_d.m_rampReference.empty() && reference == nullptr && refRamps.empty())
   {
      props.FlushRow();
      ImGui::PushStyleColor(ImGuiCol_Text, kError);
      ImGui::TextWrapped("Reference not found");
      ImGui::PopStyleColor();
   }
   if (!props.IsHarvest())
   {
      props.FlushRow();
      ImGui::BeginDisabled(reference == nullptr || edited->m_wireCurve.GetPoints().empty());
      const bool snap = ImGui::Button("Snap Heights to Reference");
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) // A disabled button still says why
         ImGui::SetTooltip("Each point's height from the reference's surface under it (the up-facing face nearest the point's own height)%s",
         reference == nullptr ? "\nDisabled: the Reference is not an imported ramp primitive" : "");
      if (snap && reference != nullptr)
         m_rampMessage = SnapHeights(*edited, *reference);
      props.CustomField(snap, "Snap Heights to Reference"s);
      ImGui::BeginDisabled(refRamps.empty() || edited->m_wireCurve.GetPoints().empty());
      const bool fit = ImGui::Button("Fit to Ramp");
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) // A disabled button still says why
         ImGui::SetTooltip("Each point's height and width from the nearest of the reference Ramps' central curves: the Ramp's own bottom-to-top interpolation, what its collider is%s",
         refRamps.empty() ? "\nDisabled: the Reference names no native Ramp" : "");
      if (fit && !refRamps.empty())
         m_rampMessage = FitToRamps(*edited, refRamps, refNames, refMissing);
      props.CustomField(fit, "Fit to Ramp"s);
      if (!m_rampMessage.empty())
         ImGui::TextWrapped("%s", m_rampMessage.c_str());
   }

   // The selected point's fields (Height, Width, walls, Roll) are in the point edit section above; the rest of the route is here
   props.Separator("Route"s);
   props.Checkbox<Primitive>(
      m_part, "Flush Entry"s, //
      [](const Primitive *p) { return p->m_d.m_rampFlushEntry; }, //
      [](Primitive *p, bool v) { p->m_d.m_rampFlushEntry = v; p->UpdateRampCollider(); }, "The floor leaves the playfield flat at height 0, eased to the route's height over the Entry Length: no step for the ball");
   props.InputFloat<Primitive>(
      m_part, "Entry Length"s, //
      [](const Primitive *p) { return p->m_d.m_rampEntryLength; }, //
      [](Primitive *p, float v) { p->m_d.m_rampEntryLength = clamp(v, 0.f, 2000.f); p->UpdateRampCollider(); }, PropertyPane::Unit::VPLength, 1, "How far the entry ease runs (clamped to the first span)");
   props.InputInt<Primitive>(
      m_part, "Curvature Detail"s, //
      [](const Primitive *p) { return p->m_d.m_rampCurvatureDetail; }, //
      [](Primitive *p, int v) { p->m_d.m_rampCurvatureDetail = clamp(v, 2, 32); p->UpdateRampCollider(); }, "How densely the route is sampled: a new row every 45 / this many degrees of turn (32 = 1.4 degrees)");
   if (!props.IsHarvest())
   {
      props.FlushRow();
      // Grade Heights: first to last, or between the two selected points. Only heights change
      DragPointEditContext *const pointCtx = GetPointEditContext();
      const auto &points = edited->m_wireCurve.GetPoints();
      const size_t nSel = pointCtx != nullptr ? pointCtx->GetSelectedPoints().size() : 0;
      ImGui::BeginDisabled(nSel == 1 || nSel > 2 || points.size() < 3);
      const bool grade = ImGui::Button(nSel == 2 ? "Grade Heights Between Selected Points" : "Grade Heights First to Last");
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("Every point between lands on a straight grade, by how far along the route it is in plan");
      bool graded = false;
      if (grade)
      {
         int a = 0, b = (int)points.size() - 1;
         if (nSel == 2)
         {
            a = edited->m_wireCurve.GetPointIndex(pointCtx->GetSelectedPoints()[0]);
            b = edited->m_wireCurve.GetPointIndex(pointCtx->GetSelectedPoints()[1]);
            if (a > b)
               std::swap(a, b);
         }
         graded = edited->GradeRail(a, b);
         if (graded)
            edited->UpdateRampCollider();
      }
      props.CustomField(graded, "Grade Heights"s);
   }

   // Walls: their own part, so their own friction. Written to the companion in this step
   props.Separator("Walls"s);
   const auto walls = [](const Primitive *p) { return p->GetRampWalls(); };
   props.InputFloat<Primitive>(
      m_part, "Wall Elasticity"s, //
      [walls](const Primitive *p) { const Primitive *w = walls(p); return w != nullptr ? w->m_d.m_elasticity : 0.3f; }, //
      [walls](Primitive *p, float v) { if (Primitive *w = walls(p)) { w->m_d.m_elasticity = v; p->UpdateRampCollider(); } }, PropertyPane::Unit::None, 3);
   props.InputFloat<Primitive>(
      m_part, "Wall Friction"s, //
      [walls](const Primitive *p) { const Primitive *w = walls(p); return w != nullptr ? w->m_d.m_friction : 0.02f; }, //
      [walls](Primitive *p, float v) { if (Primitive *w = walls(p)) { w->m_d.m_friction = max(v, 0.f); p->UpdateRampCollider(); } }, PropertyPane::Unit::None, 3,
      "Walls want much less friction than the floor (0.15 on a 180 degree U costs a quarter of the speed; King Kong's run at 0.02)");
   props.InputFloat<Primitive>(
      m_part, "Wall Scatter"s, //
      [walls](const Primitive *p) { const Primitive *w = walls(p); return w != nullptr ? w->m_d.m_scatter : 0.f; }, //
      [walls](Primitive *p, float v) { if (Primitive *w = walls(p)) { w->m_d.m_scatter = v; p->UpdateRampCollider(); } }, PropertyPane::Unit::None, 3);

   // Status
   if (!props.IsHarvest())
   {
      props.Separator("Status"s);
      props.FlushRow();
      const RampCollider::Result &r = edited->GetRampResult();
      if (!edited->GetRampError().empty())
      {
         ImGui::PushStyleColor(ImGuiCol_Text, kError);
         ImGui::TextWrapped("No collider, the parts keep their last meshes: %s", edited->GetRampError().c_str());
         ImGui::PopStyleColor();
      }
      else if (r.fits)
      {
         const int total = r.floorTris + r.wallTris;
         ImGui::PushStyleColor(ImGuiCol_Text, total > RampCollider::kTriangleWarning ? kAmber : ImGui::GetColorU32(ImGuiCol_Text));
         ImGui::TextWrapped("Collider: %d triangles (floor %d, walls %d)%s", total, r.floorTris, r.wallTris, total > RampCollider::kTriangleWarning ? ", a lot: a lower Curvature Detail gives fewer rows on a gentle route" : "");
         ImGui::PopStyleColor();
         if (edited->m_d.m_rampFlushEntry && r.entryUsed > 0.f)
         {
            ImGui::TextWrapped("Entry: steepest grade %.1f degrees over %.0f units (a straight line would be %.1f)", r.peakEntryGrade, r.entryUsed, r.straightEntryGrade);
            if (r.entryShortened)
               ImGui::TextWrapped("Entry length shortened to %.0f", r.entryUsed);
         }
         ImGui::TextWrapped("Real grade against gravity: %.1f to %.1f degrees, %.0f units long", r.minGrade, r.maxGrade, r.dist.empty() ? 0.f : r.dist.back());
         if (r.maxGrade > 25.f)
         {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextWrapped("Steeper than 25 degrees: a slow ball may not make it");
            ImGui::PopStyleColor();
         }
         for (size_t i = 0; i < r.dips.size() && i < 3; i++)
         {
            ImGui::PushStyleColor(ImGuiCol_Text, kError);
            ImGui::TextWrapped("Ball trap: a dip at %.0f units along the route", r.dips[i].dist);
            ImGui::PopStyleColor();
         }
      }
      if (!RampColliderTool::HiddenInPlayShown(g_pplayer->m_liveUI->m_editorUI))
         ImGui::TextDisabled("Turn on Hidden in Play in the view options to see and pick ramp colliders");
   }
   props.EndSection();
   done();
}

}
