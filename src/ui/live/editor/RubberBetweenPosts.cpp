// license:GPLv3+

#include "core/stdafx.h"
#include "core/editablereg.h"
#include "RubberBetweenPosts.h"
#include "RubberFit.h"
#include "EditorUIPart.h"
#include "Selection.h"
#include "ui/live/EditorUI.h"
#include "renderer/Renderer.h"
#include "parts/primitive.h"
#include "parts/rubber.h"
#include "physics/PhysicsEngine.h"

namespace VPX::EditorUI
{

namespace
{
// Footprint of a post: centre = position XY, radius = half the mean of the X and Y extents of the mesh in table space
bool Footprint(Primitive *const prim, RubberFit::Post &post)
{
   const Matrix3D &m = prim->RecalculateMatrices();
   float lox = FLT_MAX, loy = FLT_MAX, hix = -FLT_MAX, hiy = -FLT_MAX;
   for (const Vertex3D_NoTex2 &v : prim->m_mesh.m_vertices)
   {
      const Vertex3Ds w = m * v;
      lox = min(lox, w.x);
      hix = max(hix, w.x);
      loy = min(loy, w.y);
      hiy = max(hiy, w.y);
   }
   post.x = prim->m_d.m_vPosition.x;
   post.y = prim->m_d.m_vPosition.y;
   post.r = lox <= hix ? 0.25f * ((hix - lox) + (hiy - loy)) : 0.f;
   return true;
}

}

vector<Primitive *> RubberBetweenPosts::SelectedPosts(const EditorUI &ed)
{
   vector<Primitive *> posts;
   for (const auto &part : ed.m_multiSel)
      if (IEditable *const edit = part ? part->GetEditable() : nullptr; edit && edit->GetItemType() == eItemPrimitive && !static_cast<Primitive *>(edit)->IsPlayfield()
         && std::ranges::find(posts, static_cast<Primitive *>(edit)) == posts.end())
         posts.push_back(static_cast<Primitive *>(edit));
   return posts;
}

namespace
{
// Drag points of the band around the posts, empty if no loop can be fit
vector<RubberFit::Vec2> FitPosts(const vector<Primitive *> &prims, const int thickness, const float slack, const int wrap)
{
   vector<RubberFit::Post> posts;
   for (Primitive *const prim : prims)
   {
      RubberFit::Post post;
      Footprint(prim, post);
      posts.push_back(post);
   }
   vector<RubberFit::Vec2> pts = RubberFit::Fit(posts, static_cast<float>(thickness), slack, wrap == 1 ? RubberFit::SelectionOrder : RubberFit::Hull);
   return pts.size() >= 3 ? pts : vector<RubberFit::Vec2>();
}

void SetPoints(Rubber *const rubber, const vector<RubberFit::Vec2> &pts, const float z)
{
   rubber->m_curve.ClearPoints();
   for (const RubberFit::Vec2 &p : pts)
      rubber->m_curve.PushPoint(std::make_unique<DragPoint>(&rubber->m_curve, p.x, p.y, z, true));
}
}

bool RubberBetweenPosts::CanApply(const EditorUI &ed)
{
   return !ed.m_table->IsLocked() && !ed.IsInspectMode() && ed.m_pointEditPart == nullptr && SelectedPosts(ed).size() >= 2;
}

bool RubberBetweenPosts::Refit(EditorUI &ed, Rubber *const rubber)
{
   vector<Primitive *> prims;
   for (const string &name : rubber->m_d.m_postRefs)
   {
      IEditable *const edit = ed.m_table->GetElementByName(name.c_str());
      if (edit == nullptr || edit->GetItemType() != eItemPrimitive)
         return false;
      prims.push_back(static_cast<Primitive *>(edit));
   }
   const vector<RubberFit::Vec2> pts = FitPosts(prims, rubber->m_d.m_thickness, rubber->m_d.m_postSlack, rubber->m_d.m_postWrap);
   if (pts.empty())
      return false;
   SetPoints(rubber, pts, rubber->m_d.m_height);
   ed.m_renderer->ReinitRenderable(rubber);
   ed.m_renderer->InvalidateShadowContent();
   ed.m_player->m_physics->Update(rubber);
   return true;
}

void RubberBetweenPosts::Apply(EditorUI &ed)
{
   if (!CanApply(ed))
   {
      ed.Report("Rubber Between Posts: select two or more primitives");
      return;
   }
   const vector<Primitive *> prims = SelectedPosts(ed);
   const int thickness = kDefaultThickness;
   const float height = kDefaultHeight;
   const vector<RubberFit::Vec2> pts = FitPosts(prims, thickness, 0.f, 0);
   if (pts.empty())
   {
      ed.Report("Rubber Between Posts: no loop fits these posts");
      return;
   }
   RubberFit::Vec2 centre { 0.f, 0.f };
   for (const RubberFit::Vec2 &p : pts)
   {
      centre.x += p.x / static_cast<float>(pts.size());
      centre.y += p.y / static_cast<float>(pts.size());
   }

   // Same creation sequence as EditorUI::CreatePart
   IEditable *const pie = EditableRegistry::CreateAndInit(eItemRubber, ed.m_table, centre.x, centre.y);
   if (pie == nullptr)
      return;
   Rubber *const rubber = static_cast<Rubber *>(pie);
   rubber->m_d.m_thickness = thickness;
   rubber->m_d.m_height = height;
   rubber->m_d.m_hitHeight = height;
   rubber->m_d.m_postWrap = 0;
   rubber->m_d.m_postSlack = 0.f;
   for (const Primitive *const prim : prims)
      rubber->m_d.m_postRefs.push_back(prim->GetName());
   SetPoints(rubber, pts, height);
   if (auto *const scriptable = pie->GetIScriptable(); scriptable)
      ed.m_table->GetUniqueName(eItemRubber, scriptable->m_name);
   pie->m_desktopBackdrop = (ed.m_camMode == ViewMode::DesktopBackdrop);
   ed.m_table->AddPart(pie);
   pie->SetPartGroup(ed.GetPartGroupForNewPart());
   ed.m_player->TimerSetup(pie);
   if (auto *const renderable = pie->GetIRenderable(); renderable)
      renderable->RenderSetup(ed.m_renderer.get());
   if (pie->GetIHitable())
      ed.m_player->m_physics->Add(pie);

   ed.m_undo.BeginUndo();
   ed.m_undo.MarkForCreate(pie);
   ed.m_undo.SetLabel("Add Rubber Between Posts");
   ed.m_undo.EndUndo();
   ed.Report("Added " + pie->GetName() + " between " + std::to_string(prims.size()) + " posts");

   ed.UpdateEditableList();
   if (const auto it = ed.m_editableMap.find(pie); it != ed.m_editableMap.end())
   {
      ed.SetSelection(Selection(it->second));
      ed.m_outlinerAnchor = it->second;
      Matrix3D placed;
      if (!ed.GetSelectionTransform(placed))
         placed.SetIdentity();
      ed.SetLastOperation(EditorUI::LastOperation::Type::RubberBetweenPosts, placed);
      ed.m_lastOp.rbThickness = thickness;
      ed.m_lastOp.rbHeight = height;
      ed.m_lastOp.rbWrap = 0;
      ed.m_lastOp.rbSlack = 0.f;
   }
}

}
