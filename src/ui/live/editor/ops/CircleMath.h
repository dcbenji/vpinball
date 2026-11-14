// license:GPLv3+

#pragma once

#include "math/vector.h"

// Pure geometry of the circle tool (see CircleOp): a drag point outline (its control polygon) replaced by a regular polygon fitted to some of its points.
// Lengths are in VP units, angles in radians.
namespace VPX::EditorUI::CircleMath
{

constexpr int MIN_FACES = 3;
constexpr int MAX_FACES = 64;
constexpr float MIN_RADIUS = 1e-3f;

struct Fit
{
   Vertex2D center;
   float radius = 0.f;
   float startAngle = 0.f; // Angle of the first fitted point around the center
};

// Fits a circle to the listed points (indices in points): center by a least squares fit (the circle through them when they are on one, e.g.
// any 3 points), or their average position when they are in line, radius at their mean distance to it, start angle at the first of them.
// Returns false for fewer than 3 points or when they (nearly) coincide.
bool FitCircle(const vector<Vertex2D> &points, const vector<int> &sel, Fit &fit);

// Signed area of the closed outline (shoelace), positive when it turns towards increasing angles (atan2 of y over x)
float SignedArea(const vector<Vertex2D> &points);

// Regular polygon of faces points (clamped to MIN_FACES..MAX_FACES) on the circle, the first at startAngle, turning towards increasing angles
// if positive is set, decreasing otherwise
vector<Vertex2D> Circle(const Vertex2D &center, float radius, int faces, float startAngle, bool positive);

}
