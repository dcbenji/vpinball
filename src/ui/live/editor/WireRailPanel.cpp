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
constexpr ImU32 kAmber = IM_COL32(255, 190, 60, 255);

// A rail end whose floor sits proud of what feeds it (a ramp, or the end of another rail) kicks the ball out: "if the ball enters here". Warning only
string ProudLipWarning(const Primitive& rail)
{
   const PinTable* const table = rail.GetPTable();
   const float dia = max(rail.m_d.m_railBallDiameter, 1.f);
   // Plan position and lowest height (the floor) of the first row (!last) of the first piece of a channel, or the last row of its last piece
   const auto endOf = [](const WireRail::Channel& c, const bool last)
   {
      const WireRail::ChannelPiece& piece = last ? c.pieces.back() : c.pieces.front();
      const size_t row = piece.firstVert + (last ? (size_t)(piece.rows - 1) * piece.rowSize : 0);
      Vertex3Ds e(0.f, 0.f, 1e9f);
      for (int k = 0; k < piece.rowSize; k++)
      {
         const WireRail::V3& v = c.verts[row + k];
         e = Vertex3Ds(e.x + v.x / piece.rowSize, e.y + v.y / piece.rowSize, min(e.z, v.z));
      }
      return e;
   };
   const auto usable = [](const WireRail::Channel& c) { return !c.pieces.empty() && c.pieces.front().rows >= 2 && c.pieces.back().rows >= 2 && c.pieces.front().rowSize >= 1 && c.pieces.back().rowSize >= 1; };
   string msg;
   if (table == nullptr || !usable(rail.GetRailChannel()))
      return msg;
   for (const bool lastRow : { false, true })
      for (IEditable* const part : table->GetParts())
      {
         const Vertex3Ds e = endOf(rail.GetRailChannel(), lastRow);
         float feed = -1e9f; // The height of what feeds this end, if anything is within a ball diameter of it
         if (part->GetItemType() == eItemRamp && static_cast<Ramp*>(part)->m_d.m_type == RampTypeFlat)
         {
            vector<RenderVertex3D> curve;
            static_cast<Ramp*>(part)->m_curve.GetRgVertex(curve, false, 4.f);
            if (!curve.empty() && min(hypotf(curve.front().x - e.x, curve.front().y - e.y), hypotf(curve.back().x - e.x, curve.back().y - e.y)) < dia)
               feed = static_cast<Ramp*>(part)->GetSurfaceHeight(e.x, e.y);
         }
         else if (part != &rail && part->GetItemType() == eItemPrimitive && static_cast<Primitive*>(part)->m_d.m_wireRail && usable(static_cast<Primitive*>(part)->GetRailChannel()))
            for (const bool otherLast : { false, true })
            {
               const Vertex3Ds o = endOf(static_cast<Primitive*>(part)->GetRailChannel(), otherLast);
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
      for (size_t i = 0; i + 11 < std::min(c.tris.size(), c.quadIndices); i += 12) // A quad is 12 indices, two sided (see BuildChannel); a hairpin's box after them is single triangles
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
      const WireRail::Trims& trims = m_part->GetRailTrims(); // What the end fittings cut off, as rendered
      const vector<WireRail::V3> line = WireRail::TrimmedWireCenterline(frames, m_part->GetRailRowDist(), wires[w], trims.start[w], trims.end[w]);
      for (size_t i = 0; i + 1 < line.size(); i++)
      {
         const ImVec2 a = project(line[i]), b = project(line[i + 1]);
         if (a.x != FLT_MAX && b.x != FLT_MAX)
            draw->AddLine(a, b, kWireColors[w % 5], 2.f);
      }
   }
   // The supports: each centreline in amber (grey when off), the selected one thick with its name and a handle at its Position on the route
   const vector<WireRail::FixtureShape>& shapes = m_part->GetRailFixtureShapes();
   const vector<WireRail::Fixture>& list = m_part->m_d.m_railFixtures;
   for (size_t i = 0; i < shapes.size() && i < list.size(); i++)
   {
      if (!shapes[i].built)
         continue;
      const bool sel = (int)i == m_railFixture;
      const ImU32 color = list[i].enabled ? kAmber : kWireOff;
      for (const vector<WireRail::V3>* line : { &shapes[i].centerline, &shapes[i].centerline2 })
         for (size_t k = 0; k + 1 < line->size(); k++)
         {
            const ImVec2 a = project((*line)[k]), b = project((*line)[k + 1]);
            if (a.x != FLT_MAX && b.x != FLT_MAX)
               draw->AddLine(a, b, color, sel ? 3.f : 1.5f);
         }
      if (sel)
      {
         const ImVec2 at = project(shapes[i].anchor);
         if (at.x != FLT_MAX)
         {
            if (list[i].type != WireRail::kFixtureTrim) // A rail trim has no handle
               draw->AddCircleFilled(at, 5.f * ImGui::GetFontSize() / 13.f, color, 16);
            draw->AddText(ImVec2(at.x + 8.f, at.y - 16.f), color, WireRail::FixtureName(list[i], (int)i).c_str());
         }
      }
   }
}

void PrimitiveUIPart::UpdateRailFixturePanel(PropertyPane& props)
{
   if (props.IsHarvest() || !props.BeginSection("Fixtures"s)) // Not in the F3 palette
      return;
   using WireRail::Fixture;
   Primitive* const edited = props.GetEditedPart<Primitive>(m_part);
   if (props.IsMultiEdit()) // A list index means nothing across rails: the fields would write into the same index of the other selected rails
   {
      props.FlushRow();
      ImGui::TextDisabled("Select one rail to edit its fixtures");
      props.EndSection();
      return;
   }
   vector<Fixture>& list = edited->m_d.m_railFixtures;
   const int count = clamp(edited->m_d.m_railCount, 1, WireRail::kMaxWires);
   const float routeLength = edited->GetRailLength();
   m_railFixture = clamp(m_railFixture, -1, (int)list.size() - 1);
   if (m_railFixture < 0 && !list.empty())
      m_railFixture = 0;
   const auto& shapes = edited->GetRailFixtureShapes();

   const vector<WireRail::Wire> railWires = edited->GetRailWires();
   props.FlushRow();
   ImGui::TextDisabled("Supports sit at a Position along the route, independent of its points.");
   // Add: at half the route, freezy's defaults, last in the list, selected. Rung and Stand need two wires
   struct AddButton { const char* label; int type; bool needsTwo; };
   static constexpr AddButton kAdd[4] = { { "Ring", WireRail::kFixtureRing, false }, { "Cradle", WireRail::kFixtureCradle, false }, { "Rung", WireRail::kFixtureRung, true }, { "Stand", WireRail::kFixtureStand, true } };
   for (int b = 0; b < 4; b++)
   {
      if (b > 0)
         ImGui::SameLine();
      ImGui::BeginDisabled(routeLength <= 0.f || (kAdd[b].needsTwo && count < 2) || list.size() >= WireRail::kMaxFixtures);
      const bool add = ImGui::Button(kAdd[b].label);
      ImGui::EndDisabled();
      if (add)
      {
         list.push_back(WireRail::DefaultFixture(kAdd[b].type, routeLength * 0.5f));
         m_railFixture = (int)list.size() - 1;
         edited->UpdateRailFixtures();
      }
      props.CustomField(add, "Add "s + kAdd[b].label);
   }
   // End fittings (phase 2b): at the start or the end of the route, they trim rails and the hairpin and the elbow have colliders. Added at the End
   props.FlushRow();
   ImGui::TextDisabled("End Fittings sit at the start or the end of the route and reshape the rails there.");
   static constexpr AddButton kAddEnd[3] = { { "Hairpin", WireRail::kFixtureHairpin, true }, { "Elbow", WireRail::kFixtureElbow, true }, { "Rail Trim", WireRail::kFixtureTrim, false } };
   for (int b = 0; b < 3; b++)
   {
      if (b > 0)
         ImGui::SameLine();
      ImGui::BeginDisabled(routeLength <= 0.f || (kAddEnd[b].needsTwo && count < 2) || list.size() >= WireRail::kMaxFixtures);
      const bool add = ImGui::Button(kAddEnd[b].label);
      ImGui::EndDisabled();
      if (add)
      {
         list.push_back(WireRail::DefaultFixture(kAddEnd[b].type, routeLength, &railWires));
         m_railFixture = (int)list.size() - 1;
         edited->UpdateRailFixtures();
      }
      props.CustomField(add, "Add "s + kAddEnd[b].label);
   }

   // The list: enabled, name (select, drag onto another row to move), duplicate, delete
   bool changed = false;
   string changeLabel;
   int dropFrom = -1, dropTo = -1;
   for (int i = 0; i < (int)list.size() && !changed; i++)
   {
      ImGui::PushID(i);
      bool on = list[i].enabled;
      if (ImGui::Checkbox("##on", &on))
      {
         list[i].enabled = on;
         changed = true;
         changeLabel = "Toggle Fixture";
      }
      ImGui::SetItemTooltip("Enabled: off leaves it out of the render mesh (an end fitting keeps its trims and colliders for the ball)");
      ImGui::SameLine();
      const bool built = i < (int)shapes.size() && shapes[i].built;
      string name = WireRail::FixtureName(list[i], i);
      if (!built)
         name += " (not built)";
      else if (!list[i].enabled)
         name += " (off)";
      ImGui::PushStyleColor(ImGuiCol_Text, !built ? kError : list[i].enabled ? ImGui::GetColorU32(ImGuiCol_Text) : kWireOff);
      if (ImGui::Selectable(name.c_str(), i == m_railFixture, 0, ImVec2(ImGui::GetContentRegionAvail().x - 150.f * (ImGui::GetFontSize() / 13.f), 0.f)))
         m_railFixture = i;
      ImGui::PopStyleColor();
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
      {
         ImGui::SetDragDropPayload("RAILFIXTURE", &i, sizeof(int));
         ImGui::TextUnformatted(name.c_str());
         ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget())
      {
         if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload("RAILFIXTURE"))
         {
            dropFrom = *static_cast<const int*>(payload->Data);
            dropTo = i;
         }
         ImGui::EndDragDropTarget();
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(list.size() >= WireRail::kMaxFixtures);
      const bool duplicate = ImGui::SmallButton("Duplicate");
      ImGui::EndDisabled();
      if (duplicate)
      {
         Fixture copy = list[i]; // Right after it, at the same Position
         list.insert(list.begin() + i + 1, copy);
         m_railFixture = i + 1;
         changed = true;
         changeLabel = "Duplicate "s + WireRail::FixtureTypeName(copy.type);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Delete") && !changed)
      {
         changeLabel = "Delete "s + WireRail::FixtureTypeName(list[i].type);
         list.erase(list.begin() + i);
         m_railFixture = min(m_railFixture, (int)list.size() - 1);
         changed = true;
      }
      ImGui::PopID();
   }
   if (!changed && dropFrom >= 0 && dropTo >= 0 && dropFrom != dropTo && dropFrom < (int)list.size() && dropTo < (int)list.size())
   {
      // Renumbers only: the Position stays
      const Fixture moved = list[dropFrom];
      list.erase(list.begin() + dropFrom);
      list.insert(list.begin() + dropTo, moved);
      m_railFixture = dropTo;
      changed = true;
      changeLabel = "Move Fixture in List";
   }
   if (changed)
      edited->UpdateRailFixtures();
   props.CustomField(changed, changeLabel.empty() ? "Fixture"s : changeLabel);

   if (list.empty())
   {
      props.FlushRow();
      ImGui::TextDisabled("No fixtures yet: Ring, Cradle, Rung and Stand add one at the middle of the route; Hairpin, Elbow and Rail Trim at its end.");
      props.EndSection();
      return;
   }
   const int idx = m_railFixture;
   const int type = list[idx].type;
   if (type < 0 || type >= WireRail::kFixtureTypeCount)
   {
      props.FlushRow();
      ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
      ImGui::TextWrapped("%s: made by a newer version, kept as it is and not built here", WireRail::FixtureName(list[idx], idx).c_str());
      ImGui::PopStyleColor();
      props.EndSection();
      return;
   }

   // The fields of the selected support. The lambdas take the index and check it, a record of the undo may have shortened the list
   const auto fxAt = [idx](const Primitive* p) -> const Fixture* { return idx < (int)p->m_d.m_railFixtures.size() ? &p->m_d.m_railFixtures[idx] : nullptr; };
   const auto fxEdit = [idx](Primitive* p, const std::function<void(Fixture&)>& fn)
   {
      if (idx >= (int)p->m_d.m_railFixtures.size())
         return;
      fn(p->m_d.m_railFixtures[idx]);
      WireRail::ClampFixture(p->m_d.m_railFixtures[idx]);
      p->UpdateRailFixtures();
   };
   const auto number = [&](const char* label, float Fixture::* field, PropertyPane::Unit unit, int decimals, const char* tip)
   {
      props.InputFloat<Primitive>(
         m_part, label, //
         [fxAt, field](const Primitive* p) { const Fixture* f = fxAt(p); return f ? f->*field : 0.f; }, //
         [fxEdit, field](Primitive* p, float v) { fxEdit(p, [&](Fixture& f) { f.*field = v; }); }, unit, decimals, tip);
   };
   const auto integer = [&](const char* label, int Fixture::* field, const char* tip)
   {
      props.InputInt<Primitive>(
         m_part, label, //
         [fxAt, field](const Primitive* p) { const Fixture* f = fxAt(p); return f ? f->*field : 0; }, //
         [fxEdit, field](Primitive* p, int v) { fxEdit(p, [&](Fixture& f) { f.*field = v; }); }, tip);
   };
   const auto flag = [&](const char* label, bool Fixture::* field, const char* tip)
   {
      props.Checkbox<Primitive>(
         m_part, label, //
         [fxAt, field](const Primitive* p) { const Fixture* f = fxAt(p); return f ? f->*field : false; }, //
         [fxEdit, field](Primitive* p, bool v) { fxEdit(p, [&](Fixture& f) { f.*field = v; }); }, tip);
   };
   const auto vector3 = [&](const char* label, WireRail::V3 Fixture::* field, PropertyPane::Unit unit, const char* tip)
   {
      props.InputFloat3<Primitive>(
         m_part, label, //
         [fxAt, field](const Primitive* p) { const Fixture* f = fxAt(p); return f ? vec3((f->*field).x, (f->*field).y, (f->*field).z) : vec3(0.f, 0.f, 0.f); }, //
         [fxEdit, field](Primitive* p, const vec3& v) { fxEdit(p, [&](Fixture& f) { f.*field = { v.x, v.y, v.z }; }); }, unit, 2, tip);
   };
   // A button that edits the selected fixture in place: one undo step
   const auto button = [&](const char* label, const char* undoLabel, const char* tip, const std::function<void(Fixture&)>& fn)
   {
      props.FlushRow();
      const bool pressed = ImGui::Button(label);
      if (tip != nullptr)
         ImGui::SetItemTooltip("%s", tip);
      if (pressed)
         fxEdit(edited, fn);
      props.CustomField(pressed, undoLabel);
   };
   const auto position = [&]()
   {
      props.SliderFloat<Primitive>(
         m_part, "Position"s, //
         [fxAt](const Primitive* p) { const Fixture* f = fxAt(p); return f ? f->position : 0.f; }, //
         [fxEdit](Primitive* p, float v) { fxEdit(p, [&](Fixture& f) { f.position = v; }); }, 0.f, max(routeLength, 1.f), 1, "Distance along the route (the handle in the viewport drags it)");
   };
   const auto offsets = [&]()
   {
      number("Offset X", &Fixture::offsetX, PropertyPane::Unit::VPLength, 2, "Sideways, to the right of the route");
      number("Offset Z", &Fixture::offsetZ, PropertyPane::Unit::VPLength, 2, "Up from the route");
      button("Reset Offset", "Reset Offset", nullptr, [](Fixture& f) { f.offsetX = f.offsetZ = 0.f; });
   };
   const auto solder = [&]()
   {
      number("Solder Size", &Fixture::solderSize, PropertyPane::Unit::None, 2, "Scales the blobs where this support's wire meets a rail");
      number("Solder Threshold", &Fixture::solderThreshold, PropertyPane::Unit::VPLength, 2, "A blob wherever a wire of this support comes this close to a rail");
   };
   // Two angles and the Level button, a range of the ring
   const auto range = [&](const char* title, bool Fixture::* on, float Fixture::* start, float Fixture::* end)
   {
      flag(title, on, nullptr);
      if (fxAt(edited) != nullptr && fxAt(edited)->*on)
      {
         number((string(title) + " Start").c_str(), start, PropertyPane::Unit::Degree, 1, "Degrees anticlockwise from the right: 90 is up, 180 left, 270 down");
         number((string(title) + " End").c_str(), end, PropertyPane::Unit::Degree, 1, nullptr);
         button(("Level " + string(title)).c_str(), "Level Range", "Keeps the width and recentres the range on straight up or down, whichever is nearer",
            [start, end](Fixture& f) { const WireRail::V2 r = WireRail::LevelAngleRange(f.*start, f.*end); f.*start = r.x; f.*end = r.y; });
      }
   };

   props.Separator(WireRail::FixtureName(list[idx], idx));
   if (!WireRail::IsEndFitting(type)) // An end fitting is at the start or the end of the route: it has no Position
      position();
   // End fittings: the endpoint, the wire pair and the wires as combos
   vector<string> wireNames;
   for (int w = 0; w < count; w++)
      wireNames.push_back("Wire "s + std::to_string(w + 1));
   const auto endpoint = [&]()
   {
      props.Combo<Primitive>(
         m_part, "Endpoint"s, vector<string> { "Start"s, "End"s }, //
         [fxAt](const Primitive* p) { const Fixture* f = fxAt(p); return f ? f->endpoint : 1; }, //
         [fxEdit](Primitive* p, int v)
         {
            fxEdit(p,
               [&](Fixture& f)
               {
                  const bool changed = f.endpoint != v;
                  f.endpoint = v;
                  f.position = v == WireRail::kEndStart ? 0.f : p->GetRailLength();
                  const vector<WireRail::Wire> ws = p->GetRailWires();
                  // As freezy's elbow: moving to the other end re-picks the pair if the chosen one is not on there
                  if (changed && f.type == WireRail::kFixtureElbow && !(f.railA != f.railB && f.railA < (int)ws.size() && f.railB < (int)ws.size() && ws[f.railA].active && ws[f.railB].active))
                     WireRail::DefaultRailPair(ws, f.railA, f.railB);
               });
         },
         "Route end where the fitting is attached");
   };
   const auto railCombo = [&](const char* label, int Fixture::* field, const char* tip)
   {
      props.Combo<Primitive>(
         m_part, label, wireNames, //
         [fxAt, field, count](const Primitive* p) { const Fixture* f = fxAt(p); return f ? clamp(f->*field, 0, count - 1) : 0; }, //
         [fxEdit, field](Primitive* p, int v) { fxEdit(p, [&](Fixture& f) { f.*field = v; }); }, tip);
   };
   // One cutoff per wire. 'attached': the wire continues into the fitting and is not cut off (an elbow's own pair)
   const auto cutoffs = [&](const bool elbow)
   {
      for (int w = 0; w < count; w++)
      {
         const Fixture* const cur = fxAt(edited);
         const bool attached = elbow && cur != nullptr && (w == clamp(cur->railA, 0, count - 1) || w == clamp(cur->railB, 0, count - 1));
         ImGui::BeginDisabled(attached);
         props.InputFloat<Primitive>(
            m_part, "Wire "s + std::to_string(w + 1) + (attached ? " (attached)" : ""), //
            [fxAt, w, attached](const Primitive* p) { const Fixture* f = fxAt(p); return f && !attached ? f->cutoff[w] : 0.f; }, //
            [fxEdit, w](Primitive* p, float v) { fxEdit(p, [&](Fixture& f) { f.cutoff[w] = v; }); }, PropertyPane::Unit::VPLength, 2,
            attached ? "Attached rails continue into the elbow and are not cut off" : "Distance measured inward from the selected endpoint");
         ImGui::EndDisabled();
      }
   };
   switch (type)
   {
   case WireRail::kFixtureRing:
      number("Scale", &Fixture::scale, PropertyPane::Unit::None, 2, "1 wraps the wires; below 1 is inside their envelope");
      integer("Ring Density", &Fixture::density, "Segments of a whole turn, 3 to 128");
      offsets();
      range("Cutout", &Fixture::cutout, &Fixture::cutoutStart, &Fixture::cutoutEnd);
      range("Straight Line", &Fixture::straight, &Fixture::straightStart, &Fixture::straightEnd);
      solder();
      button("Apply to All", "Apply to All Rings", "Copies everything but the Position, solder included, to every other ring",
         [edited, idx](Fixture& src)
         {
            for (int k = 0; k < (int)edited->m_d.m_railFixtures.size(); k++)
               if (k != idx && edited->m_d.m_railFixtures[k].type == WireRail::kFixtureRing)
               {
                  Fixture& t = edited->m_d.m_railFixtures[k];
                  const float pos = t.position;
                  const bool en = t.enabled;
                  t = src;
                  t.position = pos;
                  t.enabled = en;
               }
         });
      break;
   case WireRail::kFixtureCradle:
      integer("Ring Density", &Fixture::density, "Segments of a whole turn, 3 to 128 (the rounded corners)");
      offsets();
      number("Bottom Length", &Fixture::bottomLength, PropertyPane::Unit::VPLength, 2, nullptr);
      number("Left Arm Length", &Fixture::leftArm, PropertyPane::Unit::VPLength, 2, "0 leaves the arm out");
      number("Right Arm Length", &Fixture::rightArm, PropertyPane::Unit::VPLength, 2, "0 leaves the arm out");
      number("Arm Angle", &Fixture::armAngle, PropertyPane::Unit::Degree, 2, "Between the arms, 1 to 179");
      number("Rotation", &Fixture::rotation, PropertyPane::Unit::Degree, 1, nullptr);
      number("Corner Radius", &Fixture::cornerRadius, PropertyPane::Unit::VPLength, 2, "Never under the tube radius");
      solder();
      break;
   case WireRail::kFixtureRung:
      offsets();
      number("Angle", &Fixture::angle, PropertyPane::Unit::Degree, 1, "About the middle of the wires");
      number("Length", &Fixture::length, PropertyPane::Unit::VPLength, 2, "Added to the length between wires 1 and 2 (signed)");
      solder();
      break;
   case WireRail::kFixtureStand:
      props.Separator("Rail Attachment"s);
      offsets();
      number("Length", &Fixture::length, PropertyPane::Unit::VPLength, 2, "Added to the length of the attachment rung between wires 1 and 2");
      props.Separator("Leg"s);
      props.Combo<Primitive>(
         m_part, "Side"s, vector<string> { "Left"s, "Right"s }, //
         [fxAt](const Primitive* p) { const Fixture* f = fxAt(p); return f && f->legLeft ? 0 : 1; }, //
         [fxEdit](Primitive* p, int v) { fxEdit(p, [&](Fixture& f) { f.legLeft = v == 0; }); }, "Which end of the attachment the leg leaves from");
      button("Mirror", "Mirror Stand", "Flips the side, the start vector, the foot and the attachment offset across the route", [](Fixture& f) { WireRail::MirrorStand(f); });
      vector3("Start Vector", &Fixture::startVector, PropertyPane::Unit::None, "Direction the leg leaves in, x right, y along, z up of the route. Normalised");
      number("Start Length", &Fixture::startLength, PropertyPane::Unit::VPLength, 2, nullptr);
      props.Separator("U-Hook Foot"s);
      vector3("Foot Position", &Fixture::footPosition, PropertyPane::Unit::VPLength, "From the leg's start, in the route's frame");
      vector3("Foot Rotation", &Fixture::footRotation, PropertyPane::Unit::Degree, "Euler XYZ, x first");
      flag("Clockwise", &Fixture::footClockwise, nullptr);
      number("Width", &Fixture::footWidth, PropertyPane::Unit::VPLength, 2, nullptr);
      number("Arm Length", &Fixture::footArm, PropertyPane::Unit::VPLength, 2, nullptr);
      number("Connected Arm Length", &Fixture::footConnectedArm, PropertyPane::Unit::VPLength, 2, nullptr);
      solder();
      break;
   case WireRail::kFixtureHairpin:
      endpoint();
      railCombo("Rail A", &Fixture::railA, "First attached rail");
      railCombo("Rail B", &Fixture::railB, "Second attached rail");
      integer("Ring Density", &Fixture::density, "Longitudinal sampling density around the complete circular loop, 4 to 128");
      number("Offset", &Fixture::fitOffset, PropertyPane::Unit::VPLength, 2, "How far the loop sits back from the endpoint along the rails. The two connected rails shorten to follow it, so the loop stays attached");
      number("Loop Diameter", &Fixture::loopDiameter, PropertyPane::Unit::VPLength, 2, "Centerline diameter of the terminal semicircle");
      number("Lead Length", &Fixture::leadLength, PropertyPane::Unit::VPLength, 2, "Distance the loop center extends beyond the route endpoint");
      number("Tangent Length", &Fixture::tangentLength, PropertyPane::Unit::VPLength, 2, "Bezier handle length used to blend each rail into the loop");
      number("Rotation", &Fixture::rotation, PropertyPane::Unit::Degree, 1, "Rotate the loop diameter around the route tangent");
      break;
   case WireRail::kFixtureElbow:
      endpoint();
      railCombo("Rail A", &Fixture::railA, "First rail that continues into the hole");
      railCombo("Rail B", &Fixture::railB, "Second rail that continues into the hole");
      number("Offset", &Fixture::fitOffset, PropertyPane::Unit::VPLength, 2, "Moves the elbow inward from the route endpoint, shortening the two rails. Zero bends at the endpoint");
      number("Drop Length", &Fixture::dropLength, PropertyPane::Unit::VPLength, 2, "Vertical length from the bend down into the hole");
      number("Z Angle", &Fixture::zAngle, PropertyPane::Unit::Degree, 1, "Rotate the horizontal leg around the route-local vertical axis");
      props.Separator("Other Rail Cutoffs"s);
      cutoffs(true);
      break;
   case WireRail::kFixtureTrim:
      endpoint();
      cutoffs(false);
      break;
   default: break;
   }

   props.FlushRow();
   if (!props.IsHarvest())
   {
      if (idx < (int)shapes.size() && !shapes[idx].built && !shapes[idx].reason.empty())
      {
         ImGui::PushStyleColor(ImGuiCol_Text, kError);
         ImGui::TextWrapped("%s is not built: %s", WireRail::FixtureName(list[idx], idx).c_str(), shapes[idx].reason.c_str());
         ImGui::PopStyleColor();
      }
      else if (idx < (int)shapes.size() && shapes[idx].built && !shapes[idx].info.empty())
         ImGui::TextWrapped("%s", shapes[idx].info.c_str());
      for (const WireRail::ChannelGap& gap : edited->GetRailChannelGaps()) // Where a trim leaves wires that can't hold a ball (freezy is silent here)
      {
         ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
         ImGui::TextWrapped("No ball channel from %.0f to %.0f: the remaining wires can't hold a ball", gap.from, gap.to);
         ImGui::PopStyleColor();
      }
      for (int k = 0; k < (int)list.size() && k < (int)shapes.size(); k++)
         if (shapes[k].inBallPath && list[k].enabled)
         {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextWrapped("%s is in the ball's path. It doesn't collide, the ball goes through it: move it with Offset Z", WireRail::FixtureName(list[k], k).c_str());
            ImGui::PopStyleColor();
         }
      ImGui::TextDisabled("Render mesh: %d triangles", edited->GetRailFixtureTriangles());
   }
   props.EndSection();
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
         const vector<WireRail::FixtureShape>& fxShapes = edited->GetRailFixtureShapes();
         if (m_railFixture >= 0 && m_railFixture < (int)fxShapes.size() && fxShapes[m_railFixture].built) // The selected support, at the wire's thickness
            for (size_t k = 0; k + 1 < fxShapes[m_railFixture].section.size(); k++)
            {
               const WireRail::V2 a = fxShapes[m_railFixture].section[k], b = fxShapes[m_railFixture].section[k + 1];
               draw->AddLine(toScreen(a.x, a.y), toScreen(b.x, b.y), kAmber, max(edited->m_d.m_wireThickness * 2.f * scale, 2.f));
            }
         // An end fitting selected: the wires trimmed away at its attachment in grey, Rail A and B outlined in amber
         const bool fitSel = m_railFixture >= 0 && m_railFixture < (int)fxShapes.size() && fxShapes[m_railFixture].built && WireRail::IsEndFitting(edited->m_d.m_railFixtures[m_railFixture].type);
         for (int i = 0; i < count; i++)
         {
            const ImVec2 at = toScreen(wires[i].x, wires[i].z);
            const bool trimmed = fitSel && (fxShapes[m_railFixture].trimmedMask >> i & 1u);
            draw->AddCircleFilled(at, max(edited->m_d.m_wireThickness * scale, 3.f), wires[i].active && !trimmed ? kWireColors[i % 5] : kWireOff, 16);
            if (fitSel && (i == fxShapes[m_railFixture].railA || i == fxShapes[m_railFixture].railB))
               draw->AddCircle(at, max(edited->m_d.m_wireThickness * scale, 3.f) + 5.f, kAmber, 16, 2.f);
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

   UpdateRailFixturePanel(props);

   if (props.BeginSection("Ball Channel Collider"s))
   {
      props.Checkbox<Primitive>(
         m_part, "Show Collider"s, //
         [](const Primitive* p) { return p->m_d.m_railShowCollider; }, //
         [](Primitive* p, bool v) { p->m_d.m_railShowCollider = v; }, "Draws the channel in green when the rail is selected");
      props.Checkbox<Primitive>(
         m_part, "Terminal Impact"s, //
         [](const Primitive* p) { return p->m_d.m_railTerminal; }, //
         [](Primitive* p, bool v) { p->m_d.m_railTerminal = v; p->m_plasticPickDirty = true; p->UpdateWire(); }, "Physics of a hairpin's end loop only, the last hit as the ball leaves the rail");
      if (edited->m_d.m_railTerminal)
      {
         props.InputFloat<Primitive>(
            m_part, "Terminal Elasticity"s, //
            [](const Primitive* p) { return p->m_d.m_railTerminalElasticity; }, //
            [](Primitive* p, float v) { p->m_d.m_railTerminalElasticity = clamp(v, 0.f, 1.f); p->UpdateWire(); }, PropertyPane::Unit::None, 3, "Elasticity of the hairpin's end loop");
         props.InputFloat<Primitive>(
            m_part, "Terminal Falloff"s, //
            [](const Primitive* p) { return p->m_d.m_railTerminalFalloff; }, //
            [](Primitive* p, float v) { p->m_d.m_railTerminalFalloff = clamp(v, 0.f, 1.f); p->UpdateWire(); }, PropertyPane::Unit::None, 3, "Elasticity falloff of the hairpin's end loop");
         props.InputFloat<Primitive>(
            m_part, "Terminal Friction"s, //
            [](const Primitive* p) { return p->m_d.m_railTerminalFriction; }, //
            [](Primitive* p, float v) { p->m_d.m_railTerminalFriction = clamp(v, 0.f, 1.f); p->UpdateWire(); }, PropertyPane::Unit::None, 3, "Friction of the hairpin's end loop");
         props.InputFloat<Primitive>(
            m_part, "Terminal Scatter"s, //
            [](const Primitive* p) { return p->m_d.m_railTerminalScatter; }, //
            [](Primitive* p, float v) { p->m_d.m_railTerminalScatter = clamp(v, 0.f, 90.f); p->UpdateWire(); }, PropertyPane::Unit::Degree, 1, "Scatter angle of the hairpin's end loop");
      }
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
         ImGui::TextDisabled("%d rows in %d pieces, %d triangles", edited->GetRailChannel().rows, (int)edited->GetRailChannel().pieces.size(), (int)edited->GetRailChannel().tris.size() / 3);
      props.EndSection();
   }
   done();
}

}
