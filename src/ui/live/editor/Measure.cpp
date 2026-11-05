// license:GPLv3+

#include "core/stdafx.h"
#include "Measure.h"

#include "math/dragpoint.h"
#include "physics/PhysicsEngine.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

#include "EditorTheme.h"
#include "EditorUIPart.h"
#include "IconsLucide.h"

namespace VPX::EditorUI
{

string Measure::Format(const Length &length)
{
   char buf[96];
   snprintf(buf, sizeof(buf), "%.2f in \xC2\xB7 %.1f mm \xC2\xB7 %.1f VPU", length.inches(), length.mm(), length.vpu);
   return buf;
}

namespace
{
constexpr ImU32 kLine = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kLineSelected = IM_COL32(255, 214, 64, 255);
constexpr ImU32 kHalo = IM_COL32(0, 0, 0, 175);
constexpr ImU32 kHover = IM_COL32(255, 128, 0, 255);
constexpr float kNear = 0.5f; // Clip space w (depth in VPU) under which a point is behind the camera; legs are clipped to it before the divide (w is 1 in an orthographic view)

struct Clip
{
   float x, y, w;
};

Clip ToClip(const Matrix3D &mvp, const Vertex3Ds &v)
{
   return { mvp._11 * v.x + mvp._21 * v.y + mvp._31 * v.z + mvp._41, mvp._12 * v.x + mvp._22 * v.y + mvp._32 * v.z + mvp._42,
      mvp._14 * v.x + mvp._24 * v.y + mvp._34 * v.z + mvp._44 };
}

float Dist2(const ImVec2 &a, const ImVec2 &b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); }

float SegmentDist2(const ImVec2 &p, const ImVec2 &a, const ImVec2 &b)
{
   const float dx = b.x - a.x, dy = b.y - a.y;
   const float l2 = dx * dx + dy * dy;
   const float t = l2 > 1e-6f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
   return Dist2(p, ImVec2(a.x + t * dx, a.y + t * dy));
}

// Dashes along a segment already clipped to the view; `phase` is the distance of its start from the start of the whole segment (the dashes keep their place)
void AddDashed(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, const float dash, const float gap, const ImU32 color, const float thickness, const float phase)
{
   const float dx = b.x - a.x, dy = b.y - a.y;
   const float len = sqrtf(dx * dx + dy * dy);
   if (len < 1e-3f)
      return;
   const float ux = dx / len, uy = dy / len;
   const float period = dash + gap;
   for (float s = -fmodf(phase, period); s < len; s += period)
   {
      const float s0 = max(s, 0.f), s1 = min(s + dash, len);
      if (s1 > s0)
         dl->AddLine(ImVec2(a.x + ux * s0, a.y + uy * s0), ImVec2(a.x + ux * s1, a.y + uy * s1), color, thickness);
   }
}

// Liang-Barsky: the part [t0, t1] of the segment a-b inside the rect (false if none)
bool ClipToRect(const ImVec2 &a, const ImVec2 &b, const ImVec2 &mn, const ImVec2 &mx, float &t0, float &t1)
{
   t0 = 0.f;
   t1 = 1.f;
   const float d[2] = { b.x - a.x, b.y - a.y };
   const float lo[2] = { mn.x - a.x, mn.y - a.y }, hi[2] = { mx.x - a.x, mx.y - a.y };
   for (int k = 0; k < 2; k++)
   {
      if (fabsf(d[k]) < 1e-9f)
      {
         if (lo[k] > 0.f || hi[k] < 0.f)
            return false;
         continue;
      }
      float ta = lo[k] / d[k], tb = hi[k] / d[k];
      if (ta > tb)
         std::swap(ta, tb);
      t0 = max(t0, ta);
      t1 = min(t1, tb);
      if (t0 > t1)
         return false;
   }
   return true;
}

void DrawLabel(ImDrawList *dl, const ImVec2 &center, const char *text, const float dpi)
{
   const ImVec2 size = ImGui::CalcTextSize(text);
   const float padX = 6.f * dpi, padY = 3.f * dpi;
   const ImVec2 a(center.x - size.x * 0.5f - padX, center.y - size.y * 0.5f - padY), b(center.x + size.x * 0.5f + padX, center.y + size.y * 0.5f + padY);
   dl->AddRectFilled(a, b, IM_COL32(18, 18, 20, 220), 4.f * dpi);
   dl->AddRect(a, b, IM_COL32(255, 255, 255, 80), 4.f * dpi, 0, 1.f);
   dl->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), IM_COL32(255, 255, 255, 255), text);
}
}

// One 3D view the tool works in: the 3D Viewport (slot -1) or the image of a secondary view
struct MeasureTool::View
{
   int slot = -1;
   bool hasPick = false;
   EditorUI::PickView pick; // Secondary views (the 3D Viewport picks with a null PickView)
   Matrix3D mvp;
   ImVec2 rectMin, rectSize; // Screen rect the clip space maps to
   ImVec2 clipMin, clipMax; // Where the tool draws
   ImDrawList *dl = nullptr;
   float dpi = 1.f;

   const EditorUI::PickView *PickPtr() const { return hasPick ? &pick : nullptr; }
   ImVec2 ToScreen(const Clip &c) const { return ImVec2(rectMin.x + (c.w + c.x) * rectSize.x * 0.5f / c.w, rectMin.y + (c.w - c.y) * rectSize.y * 0.5f / c.w); }
   bool Project(const Vertex3Ds &v, ImVec2 &out) const
   {
      const Clip c = ToClip(mvp, v);
      if (c.w <= kNear)
         return false;
      out = ToScreen(c);
      return true;
   }
   // The part of a segment in front of the camera, in screen space
   bool ProjectSegment(const Vertex3Ds &a, const Vertex3Ds &b, ImVec2 &sa, ImVec2 &sb) const
   {
      Clip ca = ToClip(mvp, a), cb = ToClip(mvp, b);
      if (ca.w <= kNear && cb.w <= kNear)
         return false;
      if (ca.w <= kNear || cb.w <= kNear)
      {
         Clip &behind = ca.w <= kNear ? ca : cb;
         const Clip &front = ca.w <= kNear ? cb : ca;
         const float t = (kNear * 1.01f - behind.w) / (front.w - behind.w);
         behind = { behind.x + (front.x - behind.x) * t, behind.y + (front.y - behind.y) * t, kNear * 1.01f };
      }
      sa = ToScreen(ca);
      sb = ToScreen(cb);
      return true;
   }
};

const char *MeasureTool::Refusal(const EditorUI &ed)
{
   if (ed.IsInspectMode())
      return "Not available while playing";
   if (ed.m_camMode == ViewMode::DesktopBackdrop)
      return "Not available in the desktop backdrop view";
   if (ed.m_pointEditPart != nullptr)
      return "Not available while editing points";
   if (ed.m_plasticTrace.active)
      return "Not available while tracing a plastic";
   if (ed.m_plasticFit.active)
      return "Not available while fitting a plastic to its posts";
   if (ed.m_refImage.PickActive())
      return "Not available while picking the points of a reference image scale";
   return nullptr;
}

void MeasureTool::SetActive(EditorUI &ed, const bool active)
{
   if (active == m_active)
      return;
   if (active)
   {
      if (Refusal(ed) != nullptr)
         return; // (a plastic trace in progress is not discarded: the button says why)
      // Only one tool at a time: the gizmo, a part being added, a placement
      ed.m_addPartType = eItemInvalid;
      ed.m_partLibrary.CancelPlacement();
      ed.m_gizmoOperation = ImGuizmo::OPERATION(0);
      ed.m_boxSelectActive = false;
      m_active = true;
      m_waitRelease = ImGui::IsMouseDown(ImGuiMouseButton_Left); // The click on the tool button is not a ruler
      return;
   }
   CancelDrag();
   m_active = false;
   m_selected = -1;
}

void MeasureTool::Clear(EditorUI &ed, const bool report)
{
   const bool had = !m_rulers.empty();
   m_drag = Drag();
   m_rulers.clear();
   m_selected = -1;
   if (had && report)
      ed.Report("Rulers cleared");
}

void MeasureTool::CancelDrag()
{
   if (!m_drag.active)
      return;
   if (m_drag.ruler >= 0 && m_drag.ruler < static_cast<int>(m_rulers.size()))
   {
      if (m_drag.isNew)
      {
         m_rulers.erase(m_rulers.begin() + m_drag.ruler);
         m_selected = -1;
      }
      else
         m_rulers[m_drag.ruler] = m_drag.backup;
   }
   m_drag = Drag();
}

void MeasureTool::DeleteSelected(EditorUI &ed)
{
   if (m_selected < 0 || m_selected >= static_cast<int>(m_rulers.size()))
      return;
   m_drag = Drag();
   m_rulers.erase(m_rulers.begin() + m_selected);
   m_selected = -1;
   ed.Report("Ruler deleted");
}

bool MeasureTool::OnEscape(EditorUI &ed)
{
   if (m_drag.active)
   {
      CancelDrag();
      m_waitRelease = ImGui::IsMouseDown(ImGuiMouseButton_Left);
      return true;
   }
   if (m_active)
   {
      SetActive(ed, false); // Leaves the tool, the rulers stay
      return true;
   }
   return false;
}

bool MeasureTool::OnDelete(EditorUI &ed)
{
   if (!m_active || m_selected < 0 || m_selected >= static_cast<int>(m_rulers.size()))
      return false;
   DeleteSelected(ed);
   return true;
}

void MeasureTool::EndFrame(EditorUI &ed)
{
   const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
   if (!down)
      m_waitRelease = false;
   if (m_drag.active && !down)
   {
      // The view of the drag did not run this frame (it went away): the drag ends here
      if (m_drag.isNew && !m_drag.moved)
      {
         CancelDrag();
         m_selected = -1;
      }
      else
         m_drag = Drag();
   }
   if (m_active
      && (ed.IsInspectMode() || ed.m_camMode == ViewMode::DesktopBackdrop || ed.m_pointEditPart != nullptr || ed.m_addPartType != eItemInvalid || ed.m_plasticTrace.active || ed.m_plasticFit.active || ed.m_refImage.PickActive()
         || ed.m_partLibrary.IsPlacementArmed()))
      SetActive(ed, false);
}

void MeasureTool::UpdateMain(EditorUI &ed, ImDrawList *dl, const bool canStart)
{
   if (!ed.m_viewportVisible || ed.IsInspectMode() || ed.m_camMode == ViewMode::DesktopBackdrop || (!m_active && m_rulers.empty()))
      return;
   View view;
   view.slot = -1;
   view.mvp = ed.m_renderer->GetMVP().GetModelViewProj(0);
   view.rectMin = ImVec2(0.f, 0.f);
   view.rectSize = ImVec2(static_cast<float>(ed.m_player->m_playfieldWnd->GetWidth()), static_cast<float>(ed.m_player->m_playfieldWnd->GetHeight()));
   view.clipMin = ImVec2(ed.m_viewportRect.x0, ed.m_viewportRect.y0);
   view.clipMax = ImVec2(ed.m_viewportRect.x1, ed.m_viewportRect.y1);
   view.dl = dl;
   view.dpi = ed.m_liveUI.GetDPI();
   Update(ed, view, canStart);
}

void MeasureTool::UpdateImage(EditorUI &ed, const int slot, const Matrix3D &mvp, const ImVec2 &rectMin, const ImVec2 &rectSize, const unsigned int visibilityMask, const LocalViewParts *local,
   const bool hiddenInPlay, ImDrawList *dl, const bool canStart)
{
   if (ed.IsInspectMode() || ed.m_camMode == ViewMode::DesktopBackdrop || (!m_active && m_rulers.empty()))
      return;
   View view;
   view.slot = slot;
   view.hasPick = true;
   view.pick.modelViewProj = mvp;
   view.pick.rectMin = rectMin;
   view.pick.rectSize = rectSize;
   view.pick.visibilityMask = visibilityMask;
   view.pick.hiddenInPlay = hiddenInPlay;
   view.pick.local = local;
   view.mvp = mvp;
   view.rectMin = rectMin;
   view.rectSize = rectSize;
   view.clipMin = rectMin;
   view.clipMax = ImVec2(rectMin.x + rectSize.x, rectMin.y + rectSize.y);
   view.dl = dl;
   view.dpi = ed.m_liveUI.GetDPI();
   Update(ed, view, canStart);
}

// The drag points and the origins of the parts the view shows, the nearest to the mouse within `radius` pixels
bool MeasureTool::FindSnap(EditorUI &ed, const View &view, const ImVec2 &mouse, const float radius, Vertex3Ds &out) const
{
   float best = radius * radius;
   bool found = false;
   const auto consider = [&](const Vertex3Ds &v)
   {
      ImVec2 s;
      if (!view.Project(v, s))
         return;
      const float d = Dist2(s, mouse);
      if (d < best)
      {
         best = d;
         out = v;
         found = true;
      }
   };
   for (const auto &part : ed.m_editables)
   {
      IEditable *const editable = part->GetEditable();
      if (!ed.IsEditablePickable(editable, view.pick.visibilityMask) || !ed.IsInLocalView(view.PickPtr(), editable))
         continue;
      if (!ed.IsPartPickableInView(part, ed.IsHiddenInPlayPickable(view.PickPtr())))
         continue;
      Matrix3D m;
      if ((part->GetTransform(m) & EditorUIPart::TM_TransAny) == EditorUIPart::TM_TransAny)
         consider(Vertex3Ds(m._41, m._42, m._43));
      if (const DragPointCurve *const curve = part->GetDragPointCurve())
         for (const auto &point : curve->GetPoints())
            consider(Vertex3Ds(point->GetX(), point->GetY(), part->GetDragPointZ(point.get())));
   }
   return found;
}

// The point of the surface under the mouse (the nearest part the ray hits, else the playfield plane); Ctrl snaps to the drag points and part origins
bool MeasureTool::PickPoint(EditorUI &ed, const View &view, const ImVec2 &mouse, const bool snap, Vertex3Ds &out, bool *snapped) const
{
   if (snapped != nullptr)
      *snapped = false;
   if (snap && FindSnap(ed, view, mouse, 14.f * view.dpi, out))
   {
      if (snapped != nullptr)
         *snapped = true;
      return true;
   }
   Vertex3Ds a, b;
   ed.PickRay(mouse, view.PickPtr(), a, b);
   float t = -1.f;
   vector<HitTestResult> hits;
   ed.RayCastParts(mouse, hits, view.PickPtr());
   if (!hits.empty())
      t = hits.front().m_time;
   else if (fabsf(b.z - a.z) > 1e-10f)
      t = (0.f - a.z) / (b.z - a.z); // The playfield plane
   if (!(t >= 0.f && t <= 1.f))
      return false;
   out = Vertex3Ds(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z));
   return true;
}

void MeasureTool::Update(EditorUI &ed, const View &view, const bool canStart)
{
   const ImGuiIO &io = ImGui::GetIO();
   const ImVec2 mouse = io.MousePos;
   const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
   const float handleR = 9.f * view.dpi;
   // What the pointer is over, with the tool on: a point of a ruler (index), the midpoint of a two point ruler (-2), or its line (-3)
   int hoverRuler = -1, hoverPoint = -1;

   if (m_active && canStart && !m_drag.active && !m_waitRelease && !ed.m_modal.active)
   {
      float best = handleR * handleR;
      for (int i = static_cast<int>(m_rulers.size()) - 1; i >= 0; i--)
         for (int k = 0; k < m_rulers[i].count; k++)
            if (ImVec2 s; view.Project(m_rulers[i].p[k], s) && Dist2(s, mouse) <= best)
            {
               best = Dist2(s, mouse);
               hoverRuler = i;
               hoverPoint = k;
            }
      if (hoverRuler < 0)
         for (int i = static_cast<int>(m_rulers.size()) - 1; i >= 0 && hoverRuler < 0; i--)
            if (m_rulers[i].count == 2)
            {
               ImVec2 s;
               const Vertex3Ds mid = (m_rulers[i].p[0] + m_rulers[i].p[1]) * 0.5f;
               if (view.Project(mid, s) && Dist2(s, mouse) <= handleR * handleR)
               {
                  hoverRuler = i;
                  hoverPoint = -2;
               }
            }
      if (hoverRuler < 0)
         for (int i = static_cast<int>(m_rulers.size()) - 1; i >= 0 && hoverRuler < 0; i--)
            for (int k = 0; k + 1 < m_rulers[i].count; k++)
               if (ImVec2 sa, sb; view.ProjectSegment(m_rulers[i].p[k], m_rulers[i].p[k + 1], sa, sb) && SegmentDist2(mouse, sa, sb) <= 25.f * view.dpi * view.dpi)
               {
                  hoverRuler = i;
                  hoverPoint = -3;
                  break;
               }
      if (io.KeyShift)
         hoverRuler = -1, hoverPoint = -1; // Shift: a new ruler wherever the press is
      if (hoverRuler >= 0 && hoverPoint != -3)
         ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
   }

   // A press starts a drag (or selects a ruler)
   if (m_active && canStart && !m_drag.active && !m_waitRelease && !ed.m_modal.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
   {
      if (hoverRuler >= 0)
      {
         m_selected = hoverRuler;
         if (hoverPoint != -3)
         {
            Ruler &r = m_rulers[hoverRuler];
            m_drag = Drag();
            m_drag.active = true;
            m_drag.view = view.slot;
            m_drag.ruler = hoverRuler;
            m_drag.pressPos = mouse;
            m_drag.backup = r;
            if (hoverPoint == -2)
            {
               // Click-drag from the midpoint: a third point, the angle is read at it (Blender's Ctrl+click on the middle of a ruler). The ruler only
               // becomes an angle ruler once the drag starts: a plain click leaves it as it is
               m_drag.point = 1;
               m_drag.toAngle = true;
            }
            else
               m_drag.point = hoverPoint;
         }
      }
      else
      {
         Vertex3Ds p;
         if (PickPoint(ed, view, mouse, io.KeyCtrl, p, nullptr))
         {
            Ruler r;
            r.p[0] = r.p[1] = r.p[2] = p;
            m_rulers.push_back(r);
            m_table = ed.m_table;
            m_selected = static_cast<int>(m_rulers.size()) - 1;
            m_drag = Drag();
            m_drag.active = true;
            m_drag.view = view.slot;
            m_drag.ruler = m_selected;
            m_drag.point = 1;
            m_drag.isNew = true;
            m_drag.pressPos = mouse;
            m_drag.backup = r;
         }
         else
            m_selected = -1;
      }
   }

   // The drag in progress, in the view it started in
   bool snapShown = false;
   Vertex3Ds snapAt;
   if (m_drag.active && m_drag.view == view.slot)
   {
      if (m_drag.ruler < 0 || m_drag.ruler >= static_cast<int>(m_rulers.size()))
         m_drag = Drag();
      else if (!down)
      {
         if (m_drag.isNew && !m_drag.moved)
         {
            CancelDrag(); // A click on nothing: no ruler
            m_selected = -1;
         }
         else
            m_drag = Drag();
      }
      else
      {
         if (!m_drag.moved && Dist2(mouse, m_drag.pressPos) > 9.f * view.dpi * view.dpi)
            m_drag.moved = true;
         if (m_drag.moved && m_drag.toAngle)
         {
            Ruler &r = m_rulers[m_drag.ruler];
            r.p[2] = r.p[1];
            r.p[1] = (r.p[0] + r.p[2]) * 0.5f;
            r.count = 3;
            m_drag.toAngle = false;
         }
         if (m_drag.moved)
         {
            Vertex3Ds p;
            bool snapped = false;
            if (PickPoint(ed, view, mouse, io.KeyCtrl, p, &snapped))
            {
               m_rulers[m_drag.ruler].p[m_drag.point] = p;
               snapShown = snapped;
               snapAt = p;
            }
         }
      }
   }
   else if (m_active && canStart && !m_drag.active && io.KeyCtrl && !down)
   {
      Vertex3Ds p;
      snapShown = FindSnap(ed, view, mouse, 14.f * view.dpi, p); // Where Ctrl would snap
      snapAt = p;
   }

   // Draw
   view.dl->PushClipRect(view.clipMin, view.clipMax, true);
   for (int i = 0; i < static_cast<int>(m_rulers.size()); i++)
      Draw(view, m_rulers[i], m_active && i == m_selected, i == hoverRuler ? hoverPoint : -1);
   if (snapShown)
      if (ImVec2 s; view.Project(snapAt, s))
      {
         view.dl->AddCircle(s, 8.f * view.dpi, kHalo, 20, 4.f * view.dpi);
         view.dl->AddCircle(s, 8.f * view.dpi, IM_COL32(80, 220, 255, 255), 20, 2.f * view.dpi);
      }
   view.dl->PopClipRect();
}

void MeasureTool::Draw(const View &view, const Ruler &ruler, const bool selected, const int hoverPoint)
{
   ImDrawList *const dl = view.dl;
   const float dpi = view.dpi;
   const ImU32 color = selected ? kLineSelected : kLine;
   ImVec2 sp[3];
   bool visible[3];
   for (int k = 0; k < ruler.count; k++)
      visible[k] = view.Project(ruler.p[k], sp[k]);

   for (int k = 0; k + 1 < ruler.count; k++)
   {
      ImVec2 a, b;
      if (!view.ProjectSegment(ruler.p[k], ruler.p[k + 1], a, b))
         continue;
      const float dx = b.x - a.x, dy = b.y - a.y;
      const float len = sqrtf(dx * dx + dy * dy);
      // Only the part in the view (and a margin) is dashed and labelled: a long ruler of a zoomed in view has ends far off screen
      float t0, t1;
      const float margin = 50.f * dpi;
      if (!ClipToRect(a, b, ImVec2(view.clipMin.x - margin, view.clipMin.y - margin), ImVec2(view.clipMax.x + margin, view.clipMax.y + margin), t0, t1))
         continue;
      const ImVec2 ca(a.x + dx * t0, a.y + dy * t0), cb(a.x + dx * t1, a.y + dy * t1);
      AddDashed(dl, ca, cb, 7.f * dpi, 4.f * dpi, kHalo, 4.f * dpi, t0 * len);
      AddDashed(dl, ca, cb, 7.f * dpi, 4.f * dpi, color, 1.75f * dpi, t0 * len);
      // Ticks across the ends of the leg
      if (len > 1e-3f)
      {
         const ImVec2 n(-dy / len * 6.f * dpi, dx / len * 6.f * dpi);
         for (const bool first : { true, false })
            if (visible[first ? k : k + 1] && (first ? t0 <= 0.f : t1 >= 1.f))
            {
               const ImVec2 &e = first ? a : b;
               dl->AddLine(ImVec2(e.x - n.x, e.y - n.y), ImVec2(e.x + n.x, e.y + n.y), kHalo, 4.f * dpi);
               dl->AddLine(ImVec2(e.x - n.x, e.y - n.y), ImVec2(e.x + n.x, e.y + n.y), color, 1.75f * dpi);
            }
      }
      // The length at the middle of the visible part of the leg, above it
      const string text = Measure::Format(Measure::Between(ruler.p[k], ruler.p[k + 1]));
      const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
      DrawLabel(dl, ImVec2(0.5f * (ca.x + cb.x), 0.5f * (ca.y + cb.y) - textSize.y * 0.5f - 13.f * dpi), text.c_str(), dpi);
   }

   // The angle at the middle point of an angle ruler: an arc between the legs, and the degrees
   if (ruler.count == 3 && visible[0] && visible[1] && visible[2])
   {
      const auto unit = [](const ImVec2 &from, const ImVec2 &to, float &len)
      {
         const float dx = to.x - from.x, dy = to.y - from.y;
         len = sqrtf(dx * dx + dy * dy);
         return len > 1e-3f ? ImVec2(dx / len, dy / len) : ImVec2(1.f, 0.f);
      };
      float l0, l2;
      const ImVec2 d0 = unit(sp[1], sp[0], l0), d2 = unit(sp[1], sp[2], l2);
      const float radius = min(30.f * dpi, 0.4f * min(l0, l2));
      if (radius > 3.f * dpi)
      {
         const float a0 = atan2f(d0.y, d0.x);
         float delta = atan2f(d2.y, d2.x) - a0;
         while (delta > static_cast<float>(M_PI))
            delta -= 2.f * static_cast<float>(M_PI);
         while (delta < -static_cast<float>(M_PI))
            delta += 2.f * static_cast<float>(M_PI);
         constexpr int steps = 24;
         ImVec2 prev(sp[1].x + cosf(a0) * radius, sp[1].y + sinf(a0) * radius);
         for (int i = 1; i <= steps; i++)
         {
            const float a = a0 + delta * static_cast<float>(i) / steps;
            const ImVec2 cur(sp[1].x + cosf(a) * radius, sp[1].y + sinf(a) * radius);
            dl->AddLine(prev, cur, kHalo, 4.f * dpi);
            prev = cur;
         }
         prev = ImVec2(sp[1].x + cosf(a0) * radius, sp[1].y + sinf(a0) * radius);
         for (int i = 1; i <= steps; i++)
         {
            const float a = a0 + delta * static_cast<float>(i) / steps;
            const ImVec2 cur(sp[1].x + cosf(a) * radius, sp[1].y + sinf(a) * radius);
            dl->AddLine(prev, cur, color, 1.75f * dpi);
            prev = cur;
         }
         const float mid = a0 + 0.5f * delta;
         char buf[32];
         snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", Measure::AngleDegrees(ruler.p[0], ruler.p[1], ruler.p[2]));
         const ImVec2 textSize = ImGui::CalcTextSize(buf);
         const float away = radius + 10.f * dpi + 0.5f * textSize.x;
         DrawLabel(dl, ImVec2(sp[1].x + cosf(mid) * away, sp[1].y + sinf(mid) * away), buf, dpi);
      }
   }

   // The handles, with the tool on: the points, and the midpoint of a two point ruler (where an angle starts)
   if (m_active)
   {
      for (int k = 0; k < ruler.count; k++)
         if (visible[k])
         {
            const bool hot = hoverPoint == k;
            dl->AddCircleFilled(sp[k], (hot ? 6.5f : 4.5f) * dpi, hot ? kHover : color, 16);
            dl->AddCircle(sp[k], (hot ? 6.5f : 4.5f) * dpi, IM_COL32(0, 0, 0, 255), 16, 1.5f * dpi);
         }
      if (ruler.count == 2 && visible[0] && visible[1])
      {
         ImVec2 mid((sp[0].x + sp[1].x) * 0.5f, (sp[0].y + sp[1].y) * 0.5f);
         const bool hot = hoverPoint == -2;
         dl->AddCircle(mid, (hot ? 6.5f : 4.f) * dpi, kHalo, 16, 3.5f * dpi);
         dl->AddCircle(mid, (hot ? 6.5f : 4.f) * dpi, hot ? kHover : color, 16, 1.75f * dpi);
      }
   }
}

}
