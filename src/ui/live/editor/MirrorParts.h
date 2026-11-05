// license:GPLv3+

#pragma once

class IEditable;
class PinUndo;
class PinTable;

namespace VPX::EditorUI
{

// Object > Mirror (Blender's Ctrl+M): the per type mirroring of a part across a line of the playfield plane.
//   X: the vertical line x = coord (left / right), Y: the horizontal line y = coord (front / back).
// A mirror is its own inverse: applying it twice gives the part back (up to float rounding of the coordinates).
namespace MirrorParts
{
enum class Axis
{
   X,
   Y
};

// Empty when the part can be mirrored, else the reason it is refused (a few words, for the status bar)
string Refusal(const IEditable *part, Axis axis);

// A mesh primitive is mirrored with its mesh (the vertices, normals and triangle winding are flipped in object space: no negative scale, which would draw
// and collide inside out). The meshes are not part of the part undo records: this adds the action that restores them to the open record. It does nothing when
// none of the parts has a mesh to flip
void MarkMeshesForUndo(PinUndo &undo, PinTable *table, const vector<IEditable *> &parts);

// Mirrors the part across the line (only for a part Refusal accepts). The caller refreshes the renderable and the colliders (primitives with a generated
// mesh refresh themselves) and marks the undo record
void Apply(IEditable *part, Axis axis, float coord);

// Position of the part along the axis (the median of the selection is the mean of these)
float Position(const IEditable *part, Axis axis);
}

}
