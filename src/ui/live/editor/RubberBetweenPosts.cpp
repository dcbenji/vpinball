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
// Footprint of a post: the Rubber Guide Radius if set, else half the mean of the X and Y extents of the mesh in table space; the centre is
// the centre of the XY bounds (a mesh post's origin is not always at its middle)
bool Footprint(Primitive *const prim, RubberFit::Post &post, string &error)
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
   const bool any = lox <= hix;
   post.x = any ? 0.5f * (lox + hix) : prim->m_d.m_vPosition.x;
   post.y = any ? 0.5f * (loy + hiy) : prim->m_d.m_vPosition.y;
   if (prim->m_d.m_rubberGuideRadius > 0.f)
      post.r = prim->m_d.m_rubberGuideRadius;
   else
   {
      post.r = any ? 0.25f * ((hix - lox) + (hiy - loy)) : 0.f;
      if (post.r > RubberBetweenPosts::kMaxAutoPostRadius)
      {
         error = prim->GetName() + " is too big to be a post (set its Rubber Guide Radius)";
         return false;
      }
   }
   if (!(post.r > 0.f))
   {
      error = prim->GetName() + " has no footprint (set its Rubber Guide Radius)";
      return false;
   }
   return true;
}

bool FitPosts(const vector<Primitive *> &prims, const float thickness, const float roundness, vector<RubberFit::Point> &pts, string &error)
{
   vector<RubberFit::Post> posts;
   for (Primitive *const prim : prims)
   {
      RubberFit::Post post;
      if (!Footprint(prim, post, error))
         return false;
      posts.push_back(post);
   }
   return RubberFit::Fit(posts, thickness, roundness, pts, error);
}

void SetPoints(Rubber *const rubber, const vector<RubberFit::Point> &pts, const float z)
{
   rubber->m_curve.ClearPoints();
   for (const RubberFit::Point &p : pts)
      rubber->m_curve.PushPoint(std::make_unique<DragPoint>(&rubber->m_curve, p.x, p.y, z, p.smooth));
}

}

bool RubberBetweenPosts::PostPosition(Primitive *const post, float &x, float &y, float *const radius)
{
   RubberFit::Post fp;
   string error;
   if (!Footprint(post, fp, error))
      return false;
   x = fp.x;
   y = fp.y;
   if (radius != nullptr)
      *radius = fp.r;
   return true;
}

void RubberBetweenPosts::Refresh(EditorUI &ed, Rubber *const rubber, const bool physics)
{
   ed.m_renderer->ReinitRenderable(rubber);
   ed.m_renderer->InvalidateShadowContent();
   if (physics)
      ed.m_player->m_physics->Update(rubber);
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

vector<Rubber *> RubberBetweenPosts::SelectedLinkedRubbers(const EditorUI &ed)
{
   vector<Rubber *> rubbers;
   for (const auto &part : ed.m_multiSel)
      if (IEditable *const edit = part ? part->GetEditable() : nullptr; edit && edit->GetItemType() == eItemRubber && !static_cast<Rubber *>(edit)->m_d.m_postRefs.empty())
         rubbers.push_back(static_cast<Rubber *>(edit));
   return rubbers;
}

bool RubberBetweenPosts::CanCreate(const EditorUI &ed) { return !ed.m_table->IsLocked() && !ed.IsInspectMode() && ed.m_pointEditPart == nullptr && SelectedPosts(ed).size() >= 2; }

bool RubberBetweenPosts::CanBind(const EditorUI &ed)
{
   if (ed.m_table->IsLocked() || ed.IsInspectMode() || ed.m_pointEditPart != nullptr || SelectedPosts(ed).empty())
      return false;
   size_t rubbers = 0;
   for (const auto &part : ed.m_multiSel)
      if (IEditable *const edit = part ? part->GetEditable() : nullptr; edit && edit->GetItemType() == eItemRubber)
         rubbers++;
   return rubbers == 1;
}

bool RubberBetweenPosts::CanAutofit(const EditorUI &ed) { return !ed.m_table->IsLocked() && !ed.IsInspectMode() && ed.m_pointEditPart == nullptr && !SelectedLinkedRubbers(ed).empty(); }

RubberBetweenPosts::PostMap RubberBetweenPosts::PostsByName(const EditorUI &ed)
{
   PostMap posts;
   for (IEditable *const part : ed.m_table->GetParts())
      if (part->GetItemType() == eItemPrimitive)
         posts[lowerCase(part->GetName())] = static_cast<Primitive *>(part);
   return posts;
}

bool RubberBetweenPosts::Refit(EditorUI &ed, Rubber *const rubber, const PostMap *const posts, const bool force, const bool physics)
{
   if (ed.IsInspectMode()) // Not while playing: the live table is only inspected
      return false;
   vector<Primitive *> prims;
   for (const string &name : rubber->m_d.m_postRefs)
   {
      Primitive *prim = nullptr;
      if (posts != nullptr)
      {
         if (const auto it = posts->find(lowerCase(name)); it != posts->end())
            prim = it->second;
      }
      else if (IEditable *const edit = ed.m_table->GetElementByName(name.c_str()); edit != nullptr && edit->GetItemType() == eItemPrimitive)
         prim = static_cast<Primitive *>(edit);
      if (prim == nullptr)
      {
         ed.Report(rubber->GetName() + ": post " + name + " not found");
         return false;
      }
      prims.push_back(prim);
   }
   const float thickness = static_cast<float>(rubber->m_d.m_thickness);
   vector<RubberFit::Post> circles;
   string error;
   for (Primitive *const prim : prims)
   {
      RubberFit::Post post;
      if (!Footprint(prim, post, error))
      {
         ed.Report(rubber->GetName() + ": " + error + " (shape kept)");
         return false;
      }
      circles.push_back(post);
   }
   // Hash of everything the fit depends on (FNV-1a over the float bits)
   uint64_t hash = 1469598103934665603ull;
   const auto mix = [&hash](const float f)
   {
      uint32_t bits;
      memcpy(&bits, &f, sizeof(bits));
      hash = (hash ^ bits) * 1099511628211ull;
   };
   for (const RubberFit::Post &post : circles)
   {
      mix(post.x);
      mix(post.y);
      mix(post.r);
   }
   mix(thickness);
   mix(rubber->m_d.m_postRoundness);
   mix(rubber->m_d.m_height);
   if (!force)
      if (const auto it = ed.m_rubberFitHash.find(rubber); it != ed.m_rubberFitHash.end() && it->second == hash)
         return true; // Same inputs as the last fit
   vector<RubberFit::Point> pts;
   if (!RubberFit::Fit(circles, thickness, rubber->m_d.m_postRoundness, pts, error))
   {
      ed.Report(rubber->GetName() + ": " + error + " (shape kept)"); // The previous shape stays
      return false;
   }
   SetPoints(rubber, pts, rubber->m_d.m_height);
   Refresh(ed, rubber, physics);
   ed.m_rubberFitHash[rubber] = hash;
   return true;
}

void RubberBetweenPosts::Create(EditorUI &ed)
{
   if (!CanCreate(ed))
   {
      ed.Report("Create Rubber Around Selected Posts: select two or more primitives");
      return;
   }
   const vector<Primitive *> prims = SelectedPosts(ed);
   const int thickness = kDefaultThickness;
   const float height = kDefaultHeight;
   vector<RubberFit::Point> pts;
   string error;
   if (!FitPosts(prims, static_cast<float>(thickness), 0.f, pts, error))
   {
      ed.Report("Create Rubber Around Selected Posts: " + error);
      return;
   }
   RubberFit::Point centre { 0.f, 0.f, false };
   for (const RubberFit::Point &p : pts)
   {
      centre.x += p.x / static_cast<float>(pts.size());
      centre.y += p.y / static_cast<float>(pts.size());
   }

   // Same creation sequence as EditorUI::CreatePart
   IEditable *const pie = EditableRegistry::CreateAndInit(eItemRubber, ed.m_table, centre.x, centre.y);
   if (pie == nullptr)
      return;
   Rubber *const rubber = static_cast<Rubber *>(pie);
   ed.m_rubberFitHash.erase(rubber); // An address reused by the new part
   rubber->m_d.m_thickness = thickness;
   rubber->m_d.m_height = height;
   rubber->m_d.m_hitHeight = height;
   rubber->m_d.m_postRoundness = 0.f;
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
   ed.m_undo.SetLabel("Create Rubber Around Selected Posts");
   ed.m_undo.EndUndo();
   ed.Report("Added " + pie->GetName() + " around " + std::to_string(prims.size()) + " posts");

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
      ed.m_lastOp.rbRoundness = 0.f;
   }
}

void RubberBetweenPosts::Bind(EditorUI &ed)
{
   if (!CanBind(ed))
   {
      ed.Report("Bind Selected: select one rubber and one or more primitives");
      return;
   }
   Rubber *rubber = nullptr;
   for (const auto &part : ed.m_multiSel)
      if (IEditable *const edit = part ? part->GetEditable() : nullptr; edit && edit->GetItemType() == eItemRubber)
         rubber = static_cast<Rubber *>(edit);
   const vector<Primitive *> prims = SelectedPosts(ed);
   vector<RubberFit::Point> pts;
   string error;
   if (!FitPosts(prims, static_cast<float>(rubber->m_d.m_thickness), rubber->m_d.m_postRoundness, pts, error))
   {
      ed.Report("Bind Selected: " + error + " (shape kept)");
      return;
   }
   ed.m_undo.BeginUndo();
   ed.m_undo.MarkForUndo(rubber);
   rubber->m_d.m_postRefs.clear();
   for (const Primitive *const prim : prims)
      rubber->m_d.m_postRefs.push_back(prim->GetName());
   SetPoints(rubber, pts, rubber->m_d.m_height);
   Refresh(ed, rubber);
   ed.m_rubberFitHash.erase(rubber);
   ed.m_undo.SetLabel("Bind Selected: " + rubber->GetName());
   ed.m_undo.EndUndo();
   ed.Report(rubber->GetName() + " follows " + std::to_string(prims.size()) + (prims.size() == 1 ? " post" : " posts"));

   if (const auto it = ed.m_editableMap.find(rubber); it != ed.m_editableMap.end())
   {
      ed.SetSelection(Selection(it->second));
      ed.m_outlinerAnchor = it->second;
      Matrix3D placed;
      if (!ed.GetSelectionTransform(placed))
         placed.SetIdentity();
      ed.SetLastOperation(EditorUI::LastOperation::Type::RubberBetweenPosts, placed);
      ed.m_lastOp.rbThickness = rubber->m_d.m_thickness;
      ed.m_lastOp.rbHeight = rubber->m_d.m_height;
      ed.m_lastOp.rbRoundness = rubber->m_d.m_postRoundness;
   }
}

void RubberBetweenPosts::Autofit(EditorUI &ed)
{
   const vector<Rubber *> rubbers = SelectedLinkedRubbers(ed);
   if (!CanAutofit(ed))
   {
      ed.Report("Autofit Now: select a rubber that follows posts");
      return;
   }
   ed.m_undo.BeginUndo();
   for (Rubber *const rubber : rubbers)
      ed.m_undo.MarkForUndo(rubber);
   const PostMap posts = PostsByName(ed);
   size_t fitted = 0;
   for (Rubber *const rubber : rubbers)
      fitted += Refit(ed, rubber, &posts) ? 1 : 0; // A failure reports and keeps the shape
   if (fitted == 0)
   {
      ed.m_undo.EndUndo();
      ed.m_undo.Discard();
      return;
   }
   ed.m_undo.SetLabel(EditorUI::CountLabel("Autofit", rubbers.size(), rubbers.front()->GetName()));
   ed.m_undo.EndUndo();
}

void RubberBetweenPosts::Detach(EditorUI &ed)
{
   const vector<Rubber *> rubbers = SelectedLinkedRubbers(ed);
   if (!CanDetach(ed))
   {
      ed.Report("Detach From Posts: select a rubber that follows posts");
      return;
   }
   ed.m_undo.BeginUndo();
   for (Rubber *const rubber : rubbers)
   {
      ed.m_undo.MarkForUndo(rubber);
      ed.DetachRubberRefs(rubber);
   }
   ed.m_undo.SetLabel(EditorUI::CountLabel("Detach From Posts", rubbers.size(), rubbers.front()->GetName()));
   ed.m_undo.EndUndo();
}

}
