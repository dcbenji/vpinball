#include "core/stdafx.h"

#include "RampUIPart.h"

#include "../BlueprintRenderContext.h"
#include "math/matrix.h"
#include "utils/color.h"

namespace VPX::EditorUI
{

RampUIPart::RampUIPart(Ramp* ramp)
   : EditableUIPart(ramp, { &Data::m_visible })
{
}

RampUIPart::TransformMask RampUIPart::GetTransform(Matrix3D& transform) { return GetCurveTransform(transform, 0.5f * (m_part->m_d.m_heightbottom + m_part->m_d.m_heighttop)); }

void RampUIPart::SetTransform(const vec3& pos, const vec3& scale, const vec3& rot)
{
   SetCurveTransform(pos, scale, rot);
   const float pz = 0.5f * (m_part->m_d.m_heightbottom + m_part->m_d.m_heighttop);
   m_part->m_d.m_heightbottom += pos.z - pz;
   m_part->m_d.m_heighttop += pos.z - pz;
}

void RampUIPart::RenderOverlay(const EditorRenderContext& ctx)
{
   ctx.DrawWireframe(m_part);
}

void RampUIPart::RenderBlueprint(const BlueprintRenderContext& ctx)
{
   if (ctx.GetPass() == BlueprintRenderContext::Pass::Fill)
   {
      // make 1-wire ramps look unique in editor - uses ramp color
      ctx.SetFillColor(m_part->GetPTable()->RenderSolid() ? g_settingsService.GetAppSettings().GetEditor_FillColor() : -1);
      ctx.SetBorderColor(-1, false, 0);
      ctx.SetObject(this);

      int cvertex;
      const Vertex2D* const rgvLocal = m_part->GetRampVertex(cvertex, nullptr, nullptr, nullptr, nullptr, HIT_SHAPE_DETAIL_LEVEL, false);
      ctx.Polygon(rgvLocal, cvertex * 2);

      delete[] rgvLocal;
   }
   else
   {
      ctx.SetFillColor(-1);
      ctx.SetBorderColor(RGB(0, 0, 0), false, 0);
      ctx.SetLineColor(RGB(0, 0, 0), false, 0);
      ctx.SetObject(this);
      ctx.SetObject(nullptr); // nullptr so this won't be hit-tested

      bool* pfCross;
      Vertex2D* middlePoints;
      int cvertex;
      const Vertex2D* const rgvLocal = m_part->GetRampVertex(cvertex, nullptr, &pfCross, nullptr, &middlePoints, HIT_SHAPE_DETAIL_LEVEL, false);
      ctx.Polygon(rgvLocal, cvertex * 2);

      if (m_part->IsHabitrail())
      {
         ctx.Polyline(middlePoints, cvertex);
         if (m_part->m_d.m_type == RampType4Wire || m_part->m_d.m_type == RampType3WireRight)
         {
            ctx.SetLineColor(RGB(0, 0, 0), false, 3);
            ctx.Polyline(rgvLocal, cvertex);
         }
         if (m_part->m_d.m_type == RampType4Wire || m_part->m_d.m_type == RampType3WireLeft)
         {
            ctx.SetLineColor(RGB(0, 0, 0), false, 3);
            ctx.Polyline(&rgvLocal[cvertex], cvertex);
         }
      }
      else
      {
         for (int i = 0; i < cvertex; i++)
            if (pfCross[i])
               ctx.Line(rgvLocal[i].x, rgvLocal[i].y, rgvLocal[cvertex * 2 - i - 1].x, rgvLocal[cvertex * 2 - i - 1].y);
      }

      delete[] rgvLocal;
      delete[] pfCross;
      delete[] middlePoints;

      bool drawDragpoints = (ctx.m_partSelected || (this == ctx.m_pointEditPart) || g_settingsService.GetAppSettings().GetEditor_ShowDragPoints());
      // if the item is selected then draw the dragpoints (or if we are always to draw dragpoints)
      if (!drawDragpoints)
      {
         // if any of the drag points of this object are selected then draw all the dragpoints
         for (const auto& pdp : m_part->m_curve.GetPoints())
         {
            if (ctx.IsPointSelected(pdp.get()))
            {
               drawDragpoints = true;
               break;
            }
         }
      }

      if (drawDragpoints)
      {
         for (size_t i = 0; i < m_part->m_curve.GetPoints().size(); i++)
         {
            const auto& pdp = m_part->m_curve.GetPoints()[i];
            ctx.SetFillColor(-1);
            ctx.SetBorderColor((ctx.m_hotPoint == pdp.get()) ? RGB(0, 255, 0) : ((i == 0) ? RGB(0, 0, 255) : RGB(255, 0, 0)), false, 0);
            ctx.SetObject(this, pdp.get());

            ctx.Ellipse2(pdp->GetX(), pdp->GetY(), 8);
         }
      }
   }
}

void RampUIPart::InsertPointOnCurve(const Vertex2D& pos)
{
   m_part->AddPoint(pos, true); // Ramp points are usually always smooth
}

void RampUIPart::UpdatePropertyPane(PropertyPane& props)
{
   props.EditableHeader("Ramp"s, m_part);

   UpdateCurveSection(props);
   UpdateSectionsSection(props);

   if (props.BeginSection("Visuals"s))
   {
      props.Combo<Ramp>(
         m_part, "Type"s, vector<string> { "Flat"s, "4 Wires"s, "2 Wires"s, "3 Wires Left"s, "3 Wires Right"s, "1 Wire"s }, //
         [](const Ramp* ramp) { return static_cast<int>(ramp->m_d.m_type); }, //
         [](Ramp* ramp, int v) { ramp->m_d.m_type = static_cast<RampType>(v); });
      props.MaterialVisuals(m_part, "Material"s, &Data::m_szMaterial, "Image"s, &Data::m_szImage);
      props.Combo<Ramp>(
         m_part, "Image Mode"s, vector { "World"s, "Wrap"s }, //
         [](const Ramp* ramp) { return static_cast<int>(ramp->m_d.m_imagealignment); }, //
         [](Ramp* ramp, int v) { ramp->m_d.m_imagealignment = static_cast<RampImageAlignment>(v); }, "World maps the image in table space; Wrap stretches it along the part's shape");
      props.Checkbox<Ramp>(
         m_part, "Apply Image to Wall"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_imageWalls; }, //
         [](Ramp* ramp, bool v) { ramp->m_d.m_imageWalls = v; }, "Also stretch the image over the ramp's side walls");
      props.InputFloat<Ramp>(
         m_part, "Depth Bias"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_depthBias; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_depthBias = v; }, PropertyPane::Unit::None, 0, "Depth offset applied at render time to fix flickering between overlapping parts");
      props.Checkbox<Ramp>(
         m_part, "Visible"s, //
         [this](const Ramp* ramp) { return GetVisibility(ramp); }, //
         [this](Ramp* ramp, bool v) { SetVisibility(ramp, v); });
      props.Checkbox<Ramp>(
         m_part, "Reflection Enabled"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_reflectionEnabled; }, //
         [](Ramp* ramp, bool v) { ramp->m_d.m_reflectionEnabled = v; }, "Render this part in the table's reflections");
      props.EndSection();
   }

   if (props.BeginSection("Geometry"s))
   {
      props.InputFloat<Ramp>(
         m_part, "Top Height"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_heighttop; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_heighttop = v; }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat<Ramp>(
         m_part, "Bottom Height"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_heightbottom; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_heightbottom = v; }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat<Ramp>(
         m_part, "Top Width"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_widthtop; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_widthtop = v; }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat<Ramp>(
         m_part, "Bottom Width"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_widthbottom; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_widthbottom = v; }, PropertyPane::Unit::VPLength, 1);

      props.Separator("Visible Walls"s);
      props.InputFloat<Ramp>(
         m_part, "Left Wall"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_leftwallheightvisible; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_leftwallheightvisible = v; }, PropertyPane::Unit::VPLength, 1, "Height of the wall rendered on the left edge of the ramp (render only)");
      props.InputFloat<Ramp>(
         m_part, "Right Wall"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_rightwallheightvisible; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_rightwallheightvisible = v; }, PropertyPane::Unit::VPLength, 1, "Height of the wall rendered on the right edge of the ramp (render only)");

      props.Separator("Wire Ramp"s);
      props.InputFloat<Ramp>(
         m_part, "Diameter"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_wireDiameter; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_wireDiameter = v; }, PropertyPane::Unit::VPLength, 1, "Thickness of the ramp's wires");
      props.InputFloat<Ramp>(
         m_part, "Distance X"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_wireDistanceX; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_wireDistanceX = v; }, PropertyPane::Unit::VPLength, 1, "Horizontal spacing between the wires");
      props.InputFloat<Ramp>(
         m_part, "Distance Y"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_wireDistanceY; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_wireDistanceY = v; }, PropertyPane::Unit::VPLength, 1, "Vertical spacing between the wires");
      props.EndSection();
   }

   if (props.BeginSection("Physics"s))
   {
      props.Checkbox<Ramp>(
         m_part, "Has Hit Event"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_hitEvent; }, //
         [](Ramp* ramp, bool v) { ramp->m_d.m_hitEvent = v; }, "Fire a Hit event to the script when the ball hits this part");
      props.InputFloat<Ramp>(
         m_part, "Hit Threshold"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_threshold; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_threshold = v; }, PropertyPane::Unit::None, 3, "Minimum ball speed needed to trigger this part's hit event");
      props.InputFloat<Ramp>(
         m_part, "Left Wall"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_leftwallheight; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_leftwallheight = v; }, PropertyPane::Unit::VPLength, 1, "Height of the physical wall on the left edge (0 = no wall)");
      props.InputFloat<Ramp>(
         m_part, "Right Wall"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_rightwallheight; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_rightwallheight = v; }, PropertyPane::Unit::VPLength, 1, "Height of the physical wall on the right edge (0 = no wall)");
      props.MaterialCombo<Ramp>(
         m_part, "Physics Material"s, //
         [](const Ramp* surf) { return surf->m_d.m_szPhysicsMaterial; }, //
         [](Ramp* surf, const string& v) { surf->m_d.m_szPhysicsMaterial = v; }, "Material preset used for this part's physics values");
      props.Checkbox<Ramp>(
         m_part, "Overwrite Material"s, //
         [](const Ramp* surf) { return surf->m_d.m_overwritePhysics; }, //
         [](Ramp* surf, bool v) { surf->m_d.m_overwritePhysics = v; }, "Use the fields below instead of the physics material's values");
      props.InputFloat<Ramp>(
         m_part, "Elasticity"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_elasticity; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_elasticity = v; }, PropertyPane::Unit::None, 3);
      props.InputFloat<Ramp>(
         m_part, "Friction"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_friction; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_friction = max(v, 0.f); }, PropertyPane::Unit::None, 3);
      props.InputFloat<Ramp>(
         m_part, "Scatter Angle"s, //
         [](const Ramp* ramp) { return ramp->m_d.m_scatter; }, //
         [](Ramp* ramp, float v) { ramp->m_d.m_scatter = v; }, PropertyPane::Unit::None, 3, "Random angle added to the ball's bounce direction");
      props.Checkbox<Ramp>(
         m_part, "Collidable"s, //
         [](const Ramp* surf) { return surf->m_d.m_collidable; }, //
         [](Ramp* surf, bool v) { surf->m_d.m_collidable = v; });
      props.EndSection();
   }

   props.TimerSection(m_part);
}

// Section i of the ramp when it is of the given kind (index based: co-edited ramps without the same section are left alone)
static RampSection* SectionAt(Ramp* ramp, size_t i, int kind) { return (i < ramp->m_d.m_sections.size() && ramp->m_d.m_sections[i].m_kind == kind) ? &ramp->m_d.m_sections[i] : nullptr; }
static const RampSection* SectionAt(const Ramp* ramp, size_t i, int kind)
{
   return (i < ramp->m_d.m_sections.size() && ramp->m_d.m_sections[i].m_kind == kind) ? &ramp->m_d.m_sections[i] : nullptr;
}

void RampUIPart::UpdateSectionsSection(PropertyPane& props)
{
   if (!props.BeginSection("Sections"s))
      return;
   const Ramp* const ramp = props.GetEditedPart(m_part);
   if (ramp->m_d.m_type != RampTypeFlat)
   {
      ImGui::TextWrapped("Sections (entry interface, hairpin) apply to flat ramps.");
      props.EndSection();
      return;
   }

   // Creation: the start edge of the ramp as an interface; the selected interior point(s) as a hairpin
   ImGui::BeginDisabled(!ramp->CanAddEntryInterface());
   props.Button<Ramp>(
      m_part, "Make Entry Interface"s, [](Ramp* r) { r->AddEntryInterface(); },
      "Mark the ramp's start edge (its first point) as an interface: a shallow flap leads from the playfield onto the ramp");
   ImGui::EndDisabled();
   int first = INT_MAX, last = -1;
   bool consecutive = false;
   if (GetPointEditContext() != nullptr && !GetPointEditContext()->IsCenterEditMode() && !GetPointEditContext()->GetSelectedPoints().empty())
   {
      for (const DragPoint* point : GetPointEditContext()->GetSelectedPoints())
      {
         const int index = ramp->m_curve.GetPointIndex(point) >= 0 ? ramp->m_curve.GetPointIndex(point) : m_part->m_curve.GetPointIndex(point);
         first = min(first, index);
         last = max(last, index);
      }
      consecutive = first >= 0 && (last - first + 1) == (int)GetPointEditContext()->GetSelectedPoints().size();
   }
   ImGui::BeginDisabled(!consecutive || !ramp->CanAddHairpin(first, last));
   props.Button<Ramp>(
      m_part, "Make Hairpin Section"s, [first, last](Ramp* r) { r->AddHairpin(first, last); },
      "Turn the selected interior point(s) of the path into a hairpin: each corner becomes an arc of one radius, with separate face counts for the outside and inside walls (point edit mode: select the corner point, or the consecutive corner points of a U-turn)");
   ImGui::EndDisabled();

   const vector<RampSectionStatus> status = ramp->GetSectionStatus();
   for (size_t i = 0; i < ramp->m_d.m_sections.size(); i++)
   {
      const RampSection& section = ramp->m_d.m_sections[i];
      const RampSectionStatus& st = status[i];
      ImGui::PushID((int)i);
      if (section.m_kind == RampSection::EntryFlap)
      {
         props.Separator("Entry Interface"s);
         props.SliderFloat<Ramp>(
            m_part, "Starts Ahead (VPU)"s, 0.f, 200.f, "%.0f", //
            [i](const Ramp* r) { const RampSection* s = SectionAt(r, i, RampSection::EntryFlap); return s ? s->m_flapAhead : 0.f; }, //
            [i](Ramp* r, float v) { if (RampSection* s = SectionAt(r, i, RampSection::EntryFlap)) s->m_flapAhead = max(0.f, v); },
            "How far ahead of the ramp foot the flap starts on the playfield");
         props.SliderFloat<Ramp>(
            m_part, "Lands Up Ramp (VPU)"s, 1.f, 300.f, "%.0f", //
            [i](const Ramp* r) { const RampSection* s = SectionAt(r, i, RampSection::EntryFlap); return s ? s->m_flapUp : 0.f; }, //
            [i](Ramp* r, float v) { if (RampSection* s = SectionAt(r, i, RampSection::EntryFlap)) s->m_flapUp = max(1.f, v); },
            "How far up the ramp (along its path) the flap meets the ramp floor; the flap replaces the ramp floor below that");
         props.SliderFloat<Ramp>(
            m_part, "Bow (VPU)"s, 0.f, 40.f, "%.1f", //
            [i](const Ramp* r) { const RampSection* s = SectionAt(r, i, RampSection::EntryFlap); return s ? s->m_flapBow : 0.f; }, //
            [i](Ramp* r, float v) { if (RampSection* s = SectionAt(r, i, RampSection::EntryFlap)) s->m_flapBow = max(0.f, v); },
            "How far the flap dips below the straight line between its two ends (0 = flat flap). Capped so the front edge starts at most about 6 deg downhill and the flap goes at most 5 VPU below the playfield (its front edge lies on the playfield)");
         props.SliderFloat<Ramp>(
            m_part, "Bow Position"s, 0.f, 1.f, "%.2f", //
            [i](const Ramp* r) { const RampSection* s = SectionAt(r, i, RampSection::EntryFlap); return s ? s->m_flapBowPos : 0.5f; }, //
            [i](Ramp* r, float v) { if (RampSection* s = SectionAt(r, i, RampSection::EntryFlap)) s->m_flapBowPos = clamp(v, 0.f, 1.f); },
            "Where along the flap its low point sits: 0 = at the front edge on the playfield, 1 = at the landing point (used range 0.1 to 0.9)");
         if (st.m_applied)
            ImGui::TextWrapped("Front edge %.1f deg %s, bow %.1f VPU, %.1f VPU below the playfield at most", fabsf(st.m_entryAngle), st.m_entryAngle < 0.f ? "downhill" : "uphill", st.m_bow, st.m_sink);
      }
      else if (section.m_kind == RampSection::Hairpin)
      {
         props.Separator(section.m_first == section.m_last ? "Hairpin at Point " + std::to_string(section.m_first) : "Hairpin, Points " + std::to_string(section.m_first) + " to " + std::to_string(section.m_last));
         const float minRadius = max(ramp->m_d.m_widthbottom, ramp->m_d.m_widthtop) * 0.5f + 1.f;
         props.SliderFloat<Ramp>(
            m_part, "Radius (VPU)"s, minRadius, max(minRadius * 4.f, 300.f), "%.0f", //
            [i](const Ramp* r) { const RampSection* s = SectionAt(r, i, RampSection::Hairpin); return s ? s->m_radius : 0.f; }, //
            [i, minRadius](Ramp* r, float v) { if (RampSection* s = SectionAt(r, i, RampSection::Hairpin)) s->m_radius = max(minRadius, v); },
            "Radius of the turn at the path centre. Its minimum, half the ramp width + 1, keeps the inside wall from folding");
         props.SliderInt<Ramp>(
            m_part, "Outside Faces"s, 1, 48, //
            [i](const Ramp* r) { const RampSection* s = SectionAt(r, i, RampSection::Hairpin); return s ? s->m_outerFaces : 0; }, //
            [i](Ramp* r, int v) { if (RampSection* s = SectionAt(r, i, RampSection::Hairpin)) s->m_outerFaces = clamp(v, 1, 256); },
            "Number of faces of the outside wall over the whole turn");
         props.SliderInt<Ramp>(
            m_part, "Inside Faces"s, 1, 48, //
            [i](const Ramp* r) { const RampSection* s = SectionAt(r, i, RampSection::Hairpin); return s ? s->m_innerFaces : 0; }, //
            [i](Ramp* r, int v) { if (RampSection* s = SectionAt(r, i, RampSection::Hairpin)) s->m_innerFaces = clamp(v, 1, 256); },
            "Number of faces of the inside wall over the whole turn (the inside is shorter: it usually takes fewer)");
         if (st.m_applied)
         {
            if (st.m_radius + 0.5f < section.m_radius)
               ImGui::TextWrapped("Radius limited to %.0f VPU by the neighbouring points", st.m_radius);
            else
               ImGui::TextWrapped("Inside wall radius %.0f VPU", st.m_radius - (st.m_minRadius - 1.f));
         }
      }
      if (!st.m_message.empty())
      {
         ImGui::PushStyleColor(ImGuiCol_Text, st.m_applied ? ImVec4(0.9f, 0.75f, 0.3f, 1.f) : ImVec4(1.f, 0.4f, 0.35f, 1.f));
         ImGui::TextWrapped("%s%s", st.m_applied ? "" : "Not applied: ", st.m_message.c_str());
         ImGui::PopStyleColor();
      }
      props.Button<Ramp>(
         m_part, "Remove Section"s,
         [i, kind = section.m_kind](Ramp* r)
         {
            if (SectionAt(r, i, kind))
               r->m_d.m_sections.erase(r->m_d.m_sections.begin() + i);
         });
      ImGui::PopID();
   }
   props.EndSection();
}

void RampUIPart::UpdatePointZField(PropertyPane& props, DragPoint* point)
{
   props.InputFloat<DragPoint>(
      point, "Height Offset"s, //
      [](const DragPoint* p) { return p->GetZ(); }, //
      [](DragPoint* p, float v) { p->SetZ(v); }, PropertyPane::Unit::VPLength, 1, "Height of this point relative to the height interpolated between the ramp's bottom and top");
   ImGui::BeginDisabled();
   props.InputFloat<DragPoint>(
      point, "Real Height"s, //
      [](const DragPoint* p) { return p->GetCalcHeight(); }, //
      [](DragPoint*, float) {}, PropertyPane::Unit::VPLength, 1, "Absolute height of this point after interpolation");
   ImGui::EndDisabled();
}
}
