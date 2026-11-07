// license:GPLv3+

// Playfield Maker board builder: the board top, the walls, the backing and the floor collider from (outline, holes, params).
// No engine dependency (plain structs and floats in, meshes out), so it can be checked standalone
// (tools/playfieldboard_check.cpp: clang++ -std=c++20 -I src tools/playfieldboard_check.cpp src/math/PlayfieldBoard.cpp src/math/PlasticMesh.cpp).
// All coordinates are table units (VPU), the board top is at z = 0 and the wood is `thickness` deep.

#pragma once

#include "InsertPocket.h"
#include "PlasticMesh.h"

#include <string>
#include <vector>

namespace PlayfieldBoard
{

enum HoleKind { kRound = 0, kSlot = 1, kPolygon = 2 };
enum HoleFlags { kCut = 1, kFallsThrough = 2, kUnderInsert = 4, kFromMask = 8 }; // As the editor's PlayfieldHole::Flags
constexpr float kFloorBevelMin = 2.f; // A Falls Through hole's bevel goes into the floor collider (one chamfer) from this size: thinner would make sliver triangles
constexpr float kRimInset = 0.5f; // An insert pocket's rim takes the art this far (VPU) inside the hole outline

// One hole, in table coordinates. Round: centre (cx, cy), diameter. Slot: the capsule of that diameter between (cx, cy) and (ex, ey).
// Polygon: `polygon` is x0, y0, x1, y1, ... (3 or more points, either winding).
// bevel (VPU, the rounded rim at the top of the hole): -1 = the board's defaultBevel, 0 = off, > 0 = this value. kUnderInsert holes are always built
// with 0 (a real insert is sanded flush). The hole is the cut at the wall: a bevelled hole is `bevel` wider each side at the surface.
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
   std::vector<float> outline; // x0, y0, x1, y1, ... the board edge (the editor's rectangle: left top, right top, right bottom, left bottom, or a traced outline); either winding, the builder makes it counter-clockwise
   std::vector<float> floorOutline; // The floor collider's edge, same layout; empty = the outline. The table rectangle when the board follows a traced outline: the ball still rolls over an edge cut
   float thickness = 23.5f;
   float uvm[6] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f }; // u = a x + b y + c, v = d x + e y + f (table XY to UV)
   int detail = 1; // Curve Detail 1..5
   float defaultBevel = 0.f; // The bevel of holes at -1 (Default), VPU
   int bevelSegments = 3; // Rings of the rounded profile (1 = a chamfer); the floor collider always uses one
   // Built insert pockets: the insert body runs from z -0.1 down to -insertDepth (at least 0.2, at most thickness - 0.5: never below the wood), where an opaque floor (BoardMeshes::pocketFloors: facing up,
   // table UVs; the editor draws it with the board) closes the pocket, so the walls and backing below it never show through the body
   float insertDepth = 11.9f;
};

struct BoardMeshes
{
   PlasticMesh::Mesh top; // The board face at z = 0 with every built hole cut
   PlasticMesh::Mesh walls; // The board edge and the walls of every built hole except the insert pockets, with their bevels
   PlasticMesh::Mesh backing; // The face lowered below the wood (on the board's outline), on the dark rows of the plywood image (the editor appends it to the walls part)
   PlasticMesh::Mesh floor; // The flat collider face with only the kFallsThrough holes cut, then their walls down to -thickness (a chamfer when the bevel >= 2)
   // Built kUnderInsert holes (the caller sets kCut on them when the board cuts insert pockets): the insert bodies and their ink caps (InsertPocket.h), on the
   // contours the hole walls use, and the pockets' floors at the bodies' bottom with their rims (side walls facing in, the art's colour at the rim stretched
   // down; the walls part gets no plywood wall for a pocket). Empty when no pocket is built
   PlasticMesh::Mesh inserts;
   PlasticMesh::Mesh insertCaps;
   PlasticMesh::Mesh pocketFloors;
};

struct BoardReport
{
   std::vector<char> overlap; // Per input hole: 1 = a cut hole that leaves the board or touches another cut hole (left out), else 0
   int skipped = 0; // Cut holes left out as degenerate (fewer than 3 points in their loop)
   int overlapping = 0; // Holes flagged in `overlap`
   int bevelClamped = 0; // Holes flagged in `clamped`
   // Per input hole: 1 = built with a smaller bevel than set: near the board edge or another hole (the clamps allow for the mitre: a corner's grown
   // ring moves up to 2 x the bevel), too wide for a concave hole's shape (its grown ring would fold: halved until it does not), or over the thickness
   std::vector<char> clamped;
   std::vector<float> bevel; // Per input hole: the bevel it was built with (0 when not built)
   int built = 0; // Holes built
   int pockets = 0; // Insert pockets built with an insert body and cap
   size_t topTriangles = 0, wallTriangles = 0, backingTriangles = 0, floorTriangles = 0, insertTriangles = 0, capTriangles = 0;
   std::vector<std::string> warnings;
};

// False (meshes untouched) with an outline of fewer than 3 points or a degenerate board. `report.overlap` is filled before that check.
bool BuildBoard(const BoardParams& params, const std::vector<BoardHole>& holes, BoardMeshes& meshes, BoardReport& report);

}
