// license:GPLv3+

// Wire Rail in the editor: the Properties sections with the cross-section view, and the overlay of the selected rail (see math/wirerail.h, freezy's VPE Wire Rail).

#include "core/stdafx.h"
#include "PrimitiveUIPart.h"
#include "parts/ramp.h"

namespace VPX::EditorUI
{

namespace
{
// freezy's wire colours, the sixth wire takes the first one's
constexpr ImU32 kWireColors[5] = { IM_COL32(13, 191, 255, 255), IM_COL32(255, 140, 13, 255), IM_COL32(115, 255, 51, 255), IM_COL32(255, 51, 166, 255), IM_COL32(166, 89, 255, 255) };
constexpr ImU32 kWireOff = IM_COL32(110, 115, 122, 255);
constexpr ImU32 kChannelFill = IM_COL32(40, 220, 90, 56);
constexpr ImU32 kChannelEdge = IM_COL32(40, 220, 90, 170);
constexpr ImU32 kError = IM_COL32(255, 80, 70, 255);

// A rail end whose floor sits proud of what feeds it (a ramp, or the end of another rail) kicks the ball out: "if the ball enters here". Warning only
string ProudLipWarning(const Primitive& rail)
{
   const PinTable* const table = rail.GetPTable();
   const float dia = max(rail.m_d.m_railBallDiameter, 1.f);
   // Plan position and lowest height (the floor) of a row of a channel
   const auto endOf = [](const WireRail::Channel& c, const int row)
   {
      Vertex3Ds e(0.f, 0.f, 1e9f);
      for (int k = 0; k < c.rowSize; k++)
      {
         const WireRail::V3& v = c.verts[(size_t)row * c.rowSize + k];
         e = Vertex3Ds(e.x + v.x / c.rowSize, e.y + v.y / c.rowSize, min(e.z, v.z));
      }
      return e;
   };
   const auto usable = [](const WireRail::Channel& c) { return c.rows >= 2 && c.rowSize >= 1 && c.verts.size() >= (size_t)(c.rows * c.rowSize); };
   string msg;
   if (table == nullptr || !usable(rail.GetRailChannel()))
      return msg;
   for (const int row : { 0, rail.GetRailChannel().rows - 1 })
      for (IEditable* const part : table->GetParts())
      {
         const Vertex3Ds e = endOf(rail.GetRailChannel(), row);
         float feed = -1e9f; // The height of what feeds this end, if anything is within a ball diameter of it
         if (part->GetItemType() == eItemRamp && static_cast<Ramp*>(part)->m_d.m_type == RampTypeFlat)
         {
            vector<RenderVertex3D> curve;
            static_cast<Ramp*>(part)->m_curve.GetRgVertex(curve, false, 4.f);
            if (!curve.empty() && min(hypotf(curve.front().x - e.x, curve.front().y - e.y), hypotf(curve.back().x - e.x, curve.back().y - e.y)) < dia)
               feed = static_cast<Ramp*>(part)->GetSurfaceHeight(e.x, e.y);
         }
         else if (part != &rail && part->GetItemType() == eItemPrimitive && static_cast<Primitive*>(part)->m_d.m_wireRail && usable(static_cast<Primitive*>(part)->GetRailChannel()))
            for (const int orow : { 0, static_cast<Primitive*>(part)->GetRailChannel().rows - 1 })
            {
               const Vertex3Ds o = endOf(static_cast<Primitive*>(part)->GetRailChannel(), orow);
               if (hypotf(o.x - e.x, o.y - e.y) < dia)
                  feed = o.z;
            }
         const float lip = e.z - feed;
         if (feed > -1e8f && lip > 0.25f && lip < dia * 0.5f) // A big step is a drop, not a lip
            msg += (msg.empty() ? "" : "\n") + std::format("Floor {:.1f} above {}: if the ball enters here it is kicked out, start the rail at or below it", lip, part->GetName());
      }
   return msg;
}
}

void PrimitiveUIPart::RenderRailOverlay(const EditorRenderContext& ctx)
{
   ImDrawList* const draw = ctx.GetDrawList();
   if (draw == nullptr)
      return;
   const vector<WireRail::Frame>& frames = m_part->GetRailFrames();
   if (frames.size() < 2)
      return;
   const auto project = [&ctx](const WireRail::V3& v) { return ctx.Project(Vertex3Ds(v.x, v.y, v.z)); };
   if (!m_part->GetRailError().empty())
   {
      // No collider: said over the rail at once, not only in the panel of a selected part
      const ImVec2 at = project(frames[frames.size() / 2].p);
      if (at.x != FLT_MAX)
         draw->AddText(at, kError, "No collider"); // Short: the view may be narrow, the panel says why
   }
   if (!ctx.IsSelected())
      return;
   if (m_part->m_d.m_railShowCollider)
   {
      const WireRail::Channel& c = m_part->GetRailChannel();
      for (size_t i = 0; i + 11 < c.tris.size(); i += 12) // A quad is 12 indices, two sided (see BuildChannel)
      {
         const ImVec2 a = project(c.verts[c.tris[i]]), b = project(c.verts[c.tris[i + 1]]), d = project(c.verts[c.tris[i + 2]]), e = project(c.verts[c.tris[i + 4]]);
         if (a.x != FLT_MAX && b.x != FLT_MAX && d.x != FLT_MAX && e.x != FLT_MAX)
            draw->AddQuadFilled(a, b, e, d, kChannelFill);
      }
      for (const auto& edge : c.edges)
      {
         const ImVec2 a = project(c.verts[edge.first]), b = project(c.verts[edge.second]);
         if (a.x != FLT_MAX && b.x != FLT_MAX)
            draw->AddLine(a, b, kChannelEdge);
      }
   }
   const vector<WireRail::Wire> wires = m_part->GetRailWires();
   for (size_t w = 0; w < wires.size(); w++)
   {
      if (!wires[w].active)
         continue;
      const vector<WireRail::V3> line = WireRail::WireCenterline(frames, wires[w]);
      for (size_t i = 0; i + 1 < line.size(); i++)
      {
         const ImVec2 a = project(line[i]), b = project(line[i + 1]);
         if (a.x != FLT_MAX && b.x != FLT_MAX)
            draw->AddLine(a, b, kWireColors[w % 5], 2.f);
      }
   }
}

void PrimitiveUIPart::UpdateRailPanel(PropertyPane& props)
{
   // A rail is made here: these sections stay open when the full sheet is collapsed under Advanced, as the point edit tools do
   const bool skipping = props.IsSkippingRest();
   if (!props.IsHarvest())
      props.SkipRest(false);
   const auto done = [&]() { props.SkipRest(skipping); };
   Primitive* const edited = props.GetEditedPart<Primitive>(m_part);
   const int count = clamp(edited->m_d.m_railCount, 1, WireRail::kMaxWires);
   m_railWire = clamp(m_railWire, 0, count - 1);

   if (props.BeginSection("Wire Rail"s))
   {
      props.InputInt<Primitive>(
         m_part, "Rails"s, //
         [](const Primitive* p) { return p->m_d.m_railCount; }, //
         [](Primitive* p, int v)
         {
            p->SetRailWires(WireRail::ResizeWires(p->GetRailWires(), clamp(v, 1, WireRail::kMaxWires), p->m_d.m_railThirdRight));
            p->UpdateWire();
         },
         "1 to 6 wires. A set still at its preset is laid out again, customized wires stay");
      if (count == 3)
         props.Combo<Primitive>(
            m_part, "Third Rail"s, vector<string> { "Left"s, "Right"s }, //
            [](const Primitive* p) { return p->m_d.m_railThirdRight ? 1 : 0; }, //
            [](Primitive* p, int v)
            {
               p->m_d.m_railThirdRight = v == 1;
               p->SetRailWires(WireRail::DefaultWires(3, p->m_d.m_railThirdRight)); // As VPE: switching resets the three wires
               p->UpdateWire();
            });
      props.InputFloat<Primitive>(
         m_part, "Wire Diameter"s, //
         [](const Primitive* p) { return p->m_d.m_wireThickness * 2.f; }, //
         [](Primitive* p, float v) { p->m_d.m_wireThickness = clamp(v, 0.1f, 50.f) * 0.5f; p->UpdateWire(); }, PropertyPane::Unit::VPLength, 2, "Thickness of every wire");
      props.InputInt<Primitive>(
         m_part, "Tube Sides"s, //
         [](const Primitive* p) { return p->m_d.m_wireSides; }, //
         [](Primitive* p, int v) { p->m_d.m_wireSides = clamp(v, 6, 16); p->UpdateWire(); }, "Polygons around each tube, 6 to 16");
      props.InputFloat<Primitive>(
         m_part, "Wire Cap Bevel"s, //
         [](const Primitive* p) { return p->m_d.m_railCapBevel; }, //
         [](Primitive* p, float v) { p->m_d.m_railCapBevel = clamp(v, 0.f, p->m_d.m_wireThickness); p->UpdateWire(); }, PropertyPane::Unit::VPLength, 2, "Chamfer on the wire ends");

      if (!props.IsHarvest())
      {
         props.FlushRow();
         // The cross-section, seen along the route: X to the right, Z up. Click a wire, drag it
         const float dpi = ImGui::GetFontSize() / 13.f;
         const float width = ImGui::GetContentRegionAvail().x;
         const float height = 170.f * dpi;
         const ImVec2 origin = ImGui::GetCursorScreenPos();
         ImGui::InvisibleButton("##RailSection", ImVec2(width, height));
         ImDrawList* const draw = ImGui::GetWindowDrawList();
         draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(27, 29, 33, 255), 3.f);
         draw->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
         // Frames the wires and the ball resting on them
         vector<WireRail::Wire> wires = edited->GetRailWires();
         const WireRail::Profile& profile = edited->GetRailProfile();
         const float ballR = max(edited->m_d.m_railBallDiameter, 1.f) * 0.5f;
         float top = profile.ball.y + ballR, side = ballR;
         for (const WireRail::Wire& w : wires)
         {
            top = max(top, w.z);
            side = max(side, fabsf(w.x));
         }
         const float scale = min(width / (2.f * (side + 16.f)), height / (top + 32.f));
         const ImVec2 center(origin.x + width * 0.5f, origin.y + height - 16.f * scale);
         const auto toScreen = [&](const float x, const float z) { return ImVec2(center.x + x * scale, center.y - z * scale); };
         for (float g = -100.f; g <= 100.f; g += 10.f)
         {
            const ImU32 c = g == 0.f ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 16);
            draw->AddLine(toScreen(g, -100.f), toScreen(g, 200.f), c);
            draw->AddLine(toScreen(-100.f, g), toScreen(100.f, g), c);
         }
         if (edited->GetRailError().empty())
         {
            draw->AddCircle(toScreen(profile.ball.x, profile.ball.y), ballR * scale, IM_COL32(255, 255, 255, 90), 48);
            if (edited->m_d.m_railShowCollider)
               for (const auto& span : profile.spans)
                  draw->AddLine(toScreen(profile.verts[span.first].x, profile.verts[span.first].y), toScreen(profile.verts[span.second].x, profile.verts[span.second].y), IM_COL32(40, 220, 90, 255), 2.f);
         }
         for (int i = 0; i < count; i++)
         {
            const ImVec2 at = toScreen(wires[i].x, wires[i].z);
            draw->AddCircleFilled(at, max(edited->m_d.m_wireThickness * scale, 3.f), wires[i].active ? kWireColors[i % 5] : kWireOff, 16);
            if (i == m_railWire)
               draw->AddCircle(at, max(edited->m_d.m_wireThickness * scale, 3.f) + 3.f, IM_COL32(255, 200, 20, 255), 16, 2.f);
         }
         draw->PopClipRect();
         bool moved = false;
         if (ImGui::IsItemActivated())
         {
            float best = 12.f * dpi;
            for (int i = 0; i < count; i++)
            {
               const ImVec2 at = toScreen(wires[i].x, wires[i].z);
               const float d = sqrtf((at.x - ImGui::GetMousePos().x) * (at.x - ImGui::GetMousePos().x) + (at.y - ImGui::GetMousePos().y) * (at.y - ImGui::GetMousePos().y));
               if (d < best)
               {
                  best = d;
                  m_railWire = i;
                  m_railDrag = true;
               }
            }
         }
         if (!ImGui::IsItemActive())
            m_railDrag = false;
         if (m_railDrag && (ImGui::GetIO().MouseDelta.x != 0.f || ImGui::GetIO().MouseDelta.y != 0.f))
         {
            wires[m_railWire].x += ImGui::GetIO().MouseDelta.x / scale;
            wires[m_railWire].z -= ImGui::GetIO().MouseDelta.y / scale;
            edited->SetRailWires(wires);
            edited->UpdateWire();
            moved = true;
         }
         props.CustomField(moved, "Move Wire"s); // One undo step for the whole drag
      }

      const int selected = m_railWire;
      props.InputFloat<Primitive>(
         m_part, "Wire X"s, //
         [selected](const Primitive* p) { return p->m_d.m_railWires[selected].x; }, //
         [selected](Primitive* p, float v) { p->m_d.m_railWires[selected].x = v; p->UpdateWire(); }, PropertyPane::Unit::VPLength, 2, "Selected wire: to the right of the route");
      props.InputFloat<Primitive>(
         m_part, "Wire Z"s, //
         [selected](const Primitive* p) { return p->m_d.m_railWires[selected].z; }, //
         [selected](Primitive* p, float v) { p->m_d.m_railWires[selected].z = v; p->UpdateWire(); }, PropertyPane::Unit::VPLength, 2, "Selected wire: above the route");
      props.Checkbox<Primitive>(
         m_part, "Wire On"s, //
         [selected](const Primitive* p) { return p->m_d.m_railWires[selected].active; }, //
         [selected](Primitive* p, bool v)
         {
            vector<WireRail::Wire> w = p->GetRailWires();
            if (!v && std::ranges::count_if(w, [](const WireRail::Wire& a) { return a.active; }) <= 1)
               return; // The last wire stays
            p->m_d.m_railWires[selected].active = v;
            p->UpdateWire();
         },
         "Selected wire: off keeps its position, it is not generated");
      if (!props.IsHarvest())
      {
         props.FlushRow();
         const bool reset = ImGui::Button("Reset Wires");
         if (reset)
         {
            edited->SetRailWires(WireRail::DefaultWires(count, edited->m_d.m_railThirdRight));
            edited->UpdateWire();
         }
         props.CustomField(reset, "Reset Wires"s);
         // Grade Heights: first to last, or between the two selected points. Only heights change
         DragPointEditContext* const pointCtx = GetPointEditContext();
         const auto& points = edited->m_wireCurve.GetPoints();
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
               edited->UpdateWire();
         }
         props.CustomField(graded, "Grade Heights"s);
         if (!edited->GetRailError().empty())
         {
            ImGui::PushStyleColor(ImGuiCol_Text, kError);
            ImGui::TextWrapped("No collider, the ball falls through: %s", edited->GetRailError().c_str());
            ImGui::PopStyleColor();
         }
         float reach = 0.f;
         for (const WireRail::Wire& w : edited->GetRailWires())
            if (w.active)
               reach = max(reach, max(fabsf(w.x), fabsf(w.z)));
         if (edited->GetRailMinRadius() < reach)
         {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 190, 60, 255));
            ImGui::TextWrapped("The route bends with radius %.0f, tighter than the wire %.0f out: the wires and the channel fold on the inside and the ball can jam or leave", edited->GetRailMinRadius(), reach);
            ImGui::PopStyleColor();
         }
         if (const string lip = ProudLipWarning(*edited); !lip.empty())
         {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 190, 60, 255));
            ImGui::TextWrapped("%s", lip.c_str());
            ImGui::PopStyleColor();
         }
      }
      props.EndSection();
   }

   if (props.BeginSection("Ball Channel Collider"s))
   {
      props.Checkbox<Primitive>(
         m_part, "Show Collider"s, //
         [](const Primitive* p) { return p->m_d.m_railShowCollider; }, //
         [](Primitive* p, bool v) { p->m_d.m_railShowCollider = v; }, "Draws the channel in green when the rail is selected");
      props.InputFloat<Primitive>(
         m_part, "Ball Diameter"s, //
         [](const Primitive* p) { return p->m_d.m_railBallDiameter; }, //
         [](Primitive* p, float v) { p->m_d.m_railBallDiameter = max(v, 1.f); p->UpdateWire(); }, PropertyPane::Unit::VPLength, 1, "The ball the channel is fitted to. Changes only this fit, not the ball in the game");
      props.InputInt<Primitive>(
         m_part, "Curvature Detail"s, //
         [](const Primitive* p) { return p->m_d.m_railCurvatureDetail; }, //
         [](Primitive* p, int v) { p->m_d.m_railCurvatureDetail = clamp(v, 2, 32); p->UpdateWire(); }, "How densely the route is sampled: raise it if a tight bend shows flats");
      props.FlushRow();
      if (!props.IsHarvest())
         ImGui::TextDisabled("%d rows, %d triangles", edited->GetRailChannel().rows, (int)edited->GetRailChannel().tris.size() / 3);
      props.EndSection();
   }
   done();
}

}
