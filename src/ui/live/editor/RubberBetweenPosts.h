// license:GPLv3+

#pragma once

#include <unordered_map>

class Primitive;
class Rubber;

namespace VPX::EditorUI
{

class EditorUI;

// Object > Rubber Between Posts: a stock rubber whose drag points are computed to wrap the selected primitives (the posts), either on the
// convex hull of the posts or on the polygon of the selection order. The rubber keeps the names of its posts (Rubber post_refs) and is
// refit when they move, are deleted or renamed (EditorUI::RefitDependents), until its own points are edited by hand.
class RubberBetweenPosts final
{
public:
   static constexpr int kDefaultThickness = 8;
   static constexpr float kDefaultHeight = 25.f;

   // At least 2 primitives (other than the playfield mesh) selected, table editable
   static bool CanApply(const EditorUI &ed);
   // Creates the rubber (one undo step), selects it and arms Adjust Last Operation
   static void Apply(EditorUI &ed);
   // Fits the drag points of the rubber to the posts it references and refreshes its renderable and physics (no undo handling: the caller
   // has the rubber in the record). False (rubber untouched) if a post is missing or no loop can be fit.
   // posts: the primitives of the table by lowercase name, built once by the caller for several refits (null: looked up in the table)
   using PostMap = std::unordered_map<string, ::Primitive *>;
   static bool Refit(EditorUI &ed, Rubber *rubber, const PostMap *posts = nullptr);
   static PostMap PostsByName(const EditorUI &ed);
   static constexpr float kMaxPostRadius = 150.f; // VPU, bigger primitives are not posts

private:
   static vector<::Primitive *> SelectedPosts(const EditorUI &ed);
};

}
