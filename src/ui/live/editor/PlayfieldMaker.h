// license:GPLv3+

#pragma once

#include "imgui/imgui.h"

#include <functional>

class IEditable;
class Primitive;
class PinTable;

namespace VPX::EditorUI
{

class EditorUI;
class LiveRenderContext;
class PropertyPane;

// Playfield Maker: Add > Playfield turns the table's playfield into a board with holes (a Primitive named playfield_mesh, m_d.m_playfieldGen) and two
// companions: <board>_Walls (the hole walls and a dark backing, plywood image) and <board>_Floor (an invisible floor collider with the Falls Through holes cut,
// collidable when Floor = Own). The old playfield is renamed and hidden, never removed (one undo step). The board's Properties hold the Playfield section:
// layer slots (images dropped from the desktop, or Load...), the floor setting and the holes.
class PlayfieldMaker final
{
public:
   static constexpr const char *kFollowsTable = "The playfield follows the table; undo the Add to remove it";
   static constexpr const char *kOneBoard = "A table has one playfield board, made by Add > Playfield: a board is not pasted or imported";

   static Primitive *Board(const PinTable *table); // The board of the table, else null
   static Primitive *CompanionOf(const Primitive *board, int role); // kPlayfieldWallsRole, kPlayfieldFloorRole, kPlayfieldInsertsRole or kPlayfieldInsertCapsRole
   static bool IsPart(const IEditable *part); // The board or one of its companions
   // Removes the board and its companions from parts (copy, delete, group, collection, pack export): reports kFollowsTable when it removed any
   static bool StripBoard(EditorUI &ed, vector<IEditable *> &parts);
   // Removes the part groups that hold the board or a companion (Delete keeps such a group, with the board in it; a move to a group that is not in
   // playfield space refuses it): reports kFollowsTable when it removed any
   static bool StripBoardGroups(EditorUI &ed, vector<IEditable *> &parts);
   // A pack about to be imported holds a board or a companion (Part Library): reports kOneBoard
   static bool RefuseImport(EditorUI &ed, const PinTable *source);
   // Undo and redo, around each step: Show Old Playfield is no undo step, so a board record taken on its other side would bring the board back over the
   // shown old playfield. A step that would leave both playfields drawn, when they were not before it, gets the visibility of the board, its walls and
   // the old playfield back; any other step (the undo of Add, an undo of the author's own Visible edit) is left alone. A hole grab is put back first.
   // Returns what to run after the step (empty when nothing can change)
   static std::function<void()> KeepShown(EditorUI &ed);

   static bool CanAdd(const EditorUI &ed);
   static void Add(EditorUI &ed); // Add > Playfield, one undo step
   static void SelectBoard(EditorUI &ed); // The table's Playfield... button

   // The Playfield section of the board's Properties (drawn inside the panel's open undo record)
   static void RenderSection(EditorUI &ed, PropertyPane &props, Primitive *board);

   // Per frame (the editor's update): the hole grab follows the mouse, a dropped file nobody took is reported
   static void Update(EditorUI &ed);
   static void OnFileDrop(EditorUI &ed, const string &path, const ImVec2 &pos);

   // Viewport, while the board is the selection: hole outlines, Add Round Hole / Add Slot placement, click on a hole selects it, G moves it
   static void Draw(EditorUI &ed, ImDrawList *draw, const LiveRenderContext &ctx);
   static bool Click(EditorUI &ed, const ImVec2 &mouse); // True if the click was taken
   static bool BeginGrab(EditorUI &ed); // G: true if a hole grab started
   static bool Cancel(EditorUI &ed); // Esc: true if placing or a grab was cancelled
   static bool IsBusy(); // Placing a hole or grabbing one

private:
   struct Impl; // The helpers (they reach the editor's state as this class does)
};

}
