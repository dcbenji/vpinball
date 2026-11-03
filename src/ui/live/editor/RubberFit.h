// license:GPLv3+

#pragma once

#include <string>
#include <vector>

namespace VPX::EditorUI::RubberFit
{

// A post (guide): circle of its footprint in table XY
struct Post
{
   float x, y, r;
};

// A baked rubber drag point
struct Point
{
   float x, y;
   bool smooth;
};

// Rubber around circular posts, a port of Visual Pinball Engine's guided rubber (VisualPinball.Unity Rubber/Guide: RubberCircleHullSolver and
// RubberPathSplineBaker).
// The path is the convex hull of the post circles grown by thickness/2: supported arcs on the circles joined by free spans (cubic Beziers,
// straight at roundness 0, relaxed towards a rounded curve up to roundness 1). Posts inside the hull are ignored, a single post gives its full
// circle. The path may not cut a post. It is then baked to drag points sampled so that VPX's own Catmull-Rom curve through them stays within
// kRenderTolerance of the exact path.
// Returns false and sets error if no path can be fit (points untouched).
bool Fit(const std::vector<Post> &posts, float thickness, float roundness, std::vector<Point> &points, std::string &error);

constexpr float kRenderTolerance = 0.05f; // VPU

}
