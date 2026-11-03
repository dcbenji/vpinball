// license:GPLv3+

#pragma once

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
   static bool Refit(EditorUI &ed, Rubber *rubber);

private:
   static vector<::Primitive *> SelectedPosts(const EditorUI &ed);
};

}
