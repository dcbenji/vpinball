// license:GPLv3+

// Playfield Maker board builder: the board top, the walls, the backing and the floor collider from (outline, holes, params).
// No engine dependency (plain structs and floats in, meshes out), so it can be checked standalone
// (tools/playfieldboard_check.cpp: clang++ -std=c++20 -I src tools/playfieldboard_check.cpp src/math/PlayfieldBoard.cpp src/math/PlasticMesh.cpp).
// All coordinates are table units (VPU), the board top is at z = 0 and the wood is `thickness` deep.

#pragma once

#include "PlasticMesh.h"

#include <string>
#include <vector>

namespace PlayfieldBoard
{

enum HoleKind { kRound = 0, kSlot = 1, kPolygon = 2 };
enum HoleFlags { kCut = 1, kFallsThrough = 2, kUnderInsert = 4, kFromMask = 8 }; // As the editor's PlayfieldHole::Flags

// One hole, in table coordinates. Round: centre (cx, cy), diameter. Slot: the capsule of that diameter between (cx, cy) and (ex, ey).
// Polygon: `polygon` is x0, y0, x1, y1, ... (3 or more points, either winding). `bevel` is -1 (the board's default) until the bevel work uses it.
// Only holes with kCut set are built; the caller clears kFallsThrough when the existing floor decides where the ball falls.
struct BoardHole
{
   int kind = kRound;
   int flags = kCut;
   float cx = 0.f, cy = 0.f, diameter = 0.f;
   float ex = 0.f, ey = 0.f;
   std::vector<float> polygon;
   float bevel = -1.f;
};

struct BoardParams
{
   std::vector<float> outline; // x0, y0, x1, y1, ... the board edge as the editor's counter-clockwise rectangle (left top, right top, right bottom, left bottom)
   float thickness = 23.5f;
   float uvm[6] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f }; // u = a x + b y + c, v = d x + e y + f (table XY to UV)
   int detail = 1; // Curve Detail 1..5
   float defaultBevel = 0.f; // Not used yet
   int bevelSegments = 3; // Not used yet
};

struct BoardMeshes
{
   PlasticMesh::Mesh top; // The board face at z = 0 with every built hole cut
   PlasticMesh::Mesh walls; // The board edge and the walls of every built hole
   PlasticMesh::Mesh backing; // The floor face lowered below the wood, on the dark rows of the plywood image (the editor appends it to the walls part)
   PlasticMesh::Mesh floor; // The flat collider face with only the kFallsThrough holes cut
};

struct BoardReport
{
   std::vector<char> overlap; // Per input hole: 1 = a cut hole that leaves the board or touches another cut hole (left out), else 0
   int skipped = 0; // Cut holes left out as degenerate (fewer than 3 points in their loop)
   int overlapping = 0; // Holes flagged in `overlap`
   int bevelClamped = 0; // Not used yet
   int built = 0; // Holes built
   size_t topTriangles = 0, wallTriangles = 0, backingTriangles = 0, floorTriangles = 0;
   std::vector<std::string> warnings;
};

// False (meshes untouched) with an outline of fewer than 3 points or a degenerate board. `report.overlap` is filled before that check.
bool BuildBoard(const BoardParams& params, const std::vector<BoardHole>& holes, BoardMeshes& meshes, BoardReport& report);

}
