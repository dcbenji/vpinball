// license:GPLv3+

#include "core/stdafx.h"
#include "CircleOp.h"
#include "imgui/imgui.h"

namespace VPX::EditorUI
{

namespace
{
   CircleOp circleOp;
   [[maybe_unused]] const bool registered = CurveEditOp::Register(&circleOp);
}

int CircleOp::GetShortcut() const { return ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_C; }

bool CircleOp::Start(const EditorUIPart &, const vector<CurveEditOp::PointState> &points, const vector<int> &sel, const bool closed, string &error)
{
   if (!closed)
   {
      error = "only closed outlines (walls) can be made round";
      return false;
   }
   // The whole outline is replaced, so locked points would be moved
   if (std::ranges::any_of(points, [](const CurveEditOp::PointState &point) { return point.uiLocked; }))
   {
      error = "the outline has locked points";
      return false;
   }
   vector<Vertex2D> pos;
   for (const CurveEditOp::PointState &point : points)
      pos.emplace_back(point.pos.x, point.pos.y);
   if (!CircleMath::FitCircle(pos, sel, m_fit))
   {
      error = sel.size() < 3 ? "select at least 3 points" : "the selected points coincide";
      return false;
   }
   return true;
}

bool CircleOp::Generate(const EditorUIPart &, const vector<CurveEditOp::PointState> &points, const vector<int> &, const bool closed,
   vector<CurveEditOp::PointState> &out, vector<int> &outSel)
{
   if (!closed || points.empty() || m_fit.radius < CircleMath::MIN_RADIUS)
      return false;
   vector<Vertex2D> pos;
   for (const CurveEditOp::PointState &point : points)
      pos.emplace_back(point.pos.x, point.pos.y);
   const vector<Vertex2D> circle = CircleMath::Circle(m_fit.center, m_fit.radius, m_faces, m_fit.startAngle, CircleMath::SignedArea(pos) >= 0.f);
   CurveEditOp::PointState point = points[0];
   point.smooth = false;
   point.autoTexture = true; // The texture coordinate of the first point would be shared by all points
   point.slingshot = std::ranges::all_of(points, [](const CurveEditOp::PointState &p) { return p.slingshot; });
   out.clear();
   outSel.clear();
   for (const Vertex2D &p : circle)
   {
      point.pos.x = p.x;
      point.pos.y = p.y;
      outSel.push_back(static_cast<int>(out.size()));
      out.push_back(point);
   }
   return true;
}

bool CircleOp::RenderParameters(const EditorUIPart &, const std::function<void(const char *)> &row)
{
   bool changed = false;
   row("Radius");
   changed |= ImGui::DragFloat("##CircleRadius", &m_fit.radius, 0.1f, 0.01f, 10000.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
   ImGui::SetItemTooltip("Radius in VP units, from the mean distance of the selected points to their average position");
   row("Faces");
   changed |= ImGui::SliderInt("##CircleFaces", &m_faces, CircleMath::MIN_FACES, CircleMath::MAX_FACES, "%d", ImGuiSliderFlags_AlwaysClamp);
   ImGui::SetItemTooltip("Number of points of the new wall outline");
   row("Center");
   changed |= ImGui::DragFloat2("##CircleCenter", &m_fit.center.x, 0.1f, 0.f, 0.f, "%.2f");
   row("Start Angle");
   changed |= ImGui::SliderAngle("##CircleStart", &m_fit.startAngle, -180.f, 180.f);
   ImGui::SetItemTooltip("Direction of the first point from the center");
   return changed;
}

}
