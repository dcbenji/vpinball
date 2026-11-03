// license:GPLv3+

#pragma once

#include <vector>

namespace VPX::EditorUI::RubberFit
{

struct Vec2
{
   float x, y;
};

// A post: centre and radius of its footprint (table XY)
struct Post
{
   float x, y, r;
};

enum Wrap
{
   Hull = 0, // Convex hull of the post circles (inner posts are ignored)
   SelectionOrder = 1 // Closed polygon through the posts in the given order, wrapped on the outside of convex corners and on the inside of concave ones
};

// Centreline of a rubber band stretched between circular posts: the posts' circles grown by thickness/2 + slack, tangent lines between
// consecutive circles, and one smooth point on each arc every 45 degrees at most (the two tangent points at least).
// Returns the closed loop of points (not repeating the first one), empty if no loop can be fit (fewer than 2 posts).
std::vector<Vec2> Fit(const std::vector<Post> &posts, float thickness, float slack, Wrap wrap);

}
