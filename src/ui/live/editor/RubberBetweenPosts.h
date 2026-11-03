// license:GPLv3+

#pragma once

#include <unordered_map>

class Primitive;
class Rubber;

namespace VPX::EditorUI
{

class EditorUI;

// Guided rubber (after Visual Pinball Engine's): a stock rubber whose drag points are computed to wrap the selected primitives (the posts),
// see RubberFit. The rubber keeps the names of its posts (Rubber post_refs) and its roundness, and is refit when they move, are resized,
// deleted or renamed (EditorUI::RefitDependents), until its own points are edited by hand or it is detached.
// Object > Rubber: Create Rubber Around Selected Posts, Bind Selected, Autofit Now, Detach From Posts.
class RubberBetweenPosts final
{
public:
   static constexpr int kDefaultThickness = 8;
   static constexpr float kDefaultHeight = 25.f;
   static constexpr float kMaxAutoPostRadius = 150.f; // VPU, bigger primitives are not posts unless they have a Rubber Guide Radius

   // 2+ primitives (other than the playfield mesh) selected, table editable
   static bool CanCreate(const EditorUI &ed);
   // Exactly one rubber and 1+ primitives selected
   static bool CanBind(const EditorUI &ed);
   // 1+ linked rubbers selected
   static bool CanAutofit(const EditorUI &ed);
   static bool CanDetach(const EditorUI &ed) { return CanAutofit(ed); }

   // Each is one labelled undo step. Create selects the new rubber and arms Adjust Last Operation (Thickness, Height, Roundness), Bind too.
   static void Create(EditorUI &ed);
   static void Bind(EditorUI &ed);
   static void Autofit(EditorUI &ed);
   static void Detach(EditorUI &ed);

   // Fits the drag points of the rubber to the posts it references and refreshes its renderable and physics (no undo handling: the caller
   // has the rubber in the record). False (rubber untouched, the solver error reported) if a post is missing or no path can be fit; also
   // false, silently, while inspecting the live table during play (no refit in play mode).
   // posts: the primitives of the table by lowercase name, built once by the caller for several refits (null: looked up in the table)
   using PostMap = std::unordered_map<string, ::Primitive *>;
   static bool Refit(EditorUI &ed, Rubber *rubber, const PostMap *posts = nullptr);
   static PostMap PostsByName(const EditorUI &ed);

private:
   static void Refresh(EditorUI &ed, Rubber *rubber); // Renderable, shadows and physics after a change of the points
   static vector<::Primitive *> SelectedPosts(const EditorUI &ed);
   static vector<Rubber *> SelectedLinkedRubbers(const EditorUI &ed);
};

}
