// license:GPLv3+

#pragma once

#include "input/InputManager.h"
#include "core/pinundo.h"
#include "imgui/imgui.h"
#include "imguizmo/ImGuizmo.h"
#include "imgui_markdown/imgui_markdown.h"
#include "editor/AreaLayout.h"
#include "editor/CameraPreviewPanel.h"
#include "editor/EditorUIPart.h"
#include "editor/Selection.h"
#include "editor/EditorChrome.h"
#include "editor/Measure.h"
#include "editor/OutlinerPanel.h"
#include "editor/TableCheckPanel.h"
#include "editor/PartLibraryPanel.h"
#include "editor/PinnedPanel.h"
#include "editor/PropertiesPanel.h"
#include "editor/PlasticFromAtlas.h"
#include "editor/ReferenceImage.h"
#include "editor/RubberBetweenPosts.h"
#include "editor/RendererInspectionModal.h"
#include "editor/ConvertToPBRModal.h"
#include "editor/ScriptPanel.h"
#include "editor/SelectRename.h"
#include "editor/SearchPalette.h"
#include "math/matrix.h"
#include "renderer/Renderer.h"
#include "unordered_dense.h"
#include "ui/VPXFileFeedback.h"

#include <any>
#include <atomic>
#include <optional>
#include <thread>

class LiveUI;
class EngineMcp;
class PinTable;
class Player;
class InputManager;
class Renderer;
class DragPoint;
class PartGroup;
class FRect3D;
struct HitTestResult;
class IEditable;
class Rubber;

namespace VPX
{
class Sound;
}

namespace VPX::EditorUI
{

class EditorUI final
   : private DragPointEditContext
{
   // The editor's UI is split between sub components which, for the time being, access
   // the shared editor state directly (to be refined as domain objects are extracted)
   friend class ::EngineMcp;
   friend class EditorChrome;
   friend class OutlinerPanel;
   friend class PropertiesPanel;
   friend class RendererInspectionModal;
   friend class ConvertToPBRModal;
   friend class PartLibraryPanel;
   friend class CameraPreviewPanel;
   friend class ScriptPanel;
   friend class TableCheckPanel;
   friend class PinnedPanel;
   friend class SearchPalette;
   friend class SetOrigin;
   friend class SelectRename;
   friend class RubberBetweenPosts;
   friend class PlasticFromAtlas;
   friend class ReferenceImage;
   friend class MeasureTool;

public:
   EditorUI(LiveUI &liveUI);
   ~EditorUI();

   void Open();
   bool IsOpened() const { return m_isOpened; }
   void Render3D();
   void RenderHiddenOverlay3D(); // The parts hidden in play, as the 3D Viewport draws them, in a secondary view's 3D pass (the view's Hidden in Play option is checked by the caller)
   void RenderReferenceImage3D(int slot); // The reference image in the 3D pass of a secondary view (its panel slot)
   void RenderSelection3D(); // The selection's edges in the 3D pass being recorded (the secondary views; the 3D Viewport draws them in RenderUI)
   void RenderUI();
   void Close();

   // Switch the editor to a new table (either the base edited table or one of its live copies)
   void SetTable(PinTable *table);

   // Start playing a shallow copy of the edited table (edition is suspended, restored when the play session ends)
   void PlayTest();

   bool IsPreview() const { return m_camMode == ViewMode::PreviewCam; }
   bool IsBackdropEditMode() const { return m_camMode == ViewMode::DesktopBackdrop; }

   bool IsInspectMode() const { return m_table->m_liveBaseTable != nullptr; }
   bool IsLocalView() const { return m_localView.active; } // The 3D Viewport shows a Local View (View > Local View)

   // Posts a message in the status bar (last operation report, fades after a few seconds)
   void Report(const string &message);
   // Development only (EngineMcp): look at camera in table space (VPU, Z up), vertical FOV in degrees; a null fov goes back to the preview camera
   void SetLookAtCamera(const vec3 &eye, const vec3 &target, float fovY, Renderer::ShadeMode shading);
   // Development only: the part of the display showing the 3D view, as fractions of its size; false when it is hidden
   bool GetViewportFraction(float (&rect)[4]) const;

   // True if the top row digit (0..9) is a view navigation key (numpad emulation) rather than a game key: editor UI up and game not running (edit mode, or inspect mode while paused)
   bool IsDigitNavKey(int digit) const;
   // Numpad 5: toggles between the perspective and the orthographic editor camera. From the preview camera (perspective) or the desktop backdrop (orthographic)
   // the flag is stale, so it is first set to what the view showed, which makes the first press visibly change the view.
   void TogglePerspectiveCam();

   // ImGui window name of an area panel: "<base>##<id>", the id being the area's leaf id (Script, Part Library) or a serial number per panel created
   // (Outliner, Properties: an area re-typed away and back keeps its leaf id, but gets a new panel), so ImGui state (scroll, tree open state) is not inherited
   static string AreaWindowName(const char *base, int id) { return string(base) + "##" + std::to_string(id); }
   static int NewAreaPanelSerial()
   {
      static int serial = 0;
      return ++serial;
   }

private:
   // UI Context
   LiveUI &m_liveUI;
   Player *m_player;
   PinTable *m_table; // The table displayed by the player
   InputManager *m_pininput;
   std::unique_ptr<Renderer>& m_renderer;

   // Sub components (UI panels & modals)
   EditorChrome m_chrome;
   // Properties of the selection that no area shows: the N-panel's Item tab when no Properties area follows the selection, and the panes the
   // field harvests draw. The Properties areas have their own instances (m_propertiesAreas)
   PropertiesPanel m_properties;
   RendererInspectionModal m_inspectionModal;
   ConvertToPBRModal m_pbrConvertModal;
   SelectRename m_selectRename; // Select Similar (Shift+G) and Batch Rename (Ctrl+F2)
   PartLibraryPanel m_partLibrary;
   CameraPreviewPanel m_cameraPreviews[AreaLayout::kMaxViewLeaves]; // Secondary 3D views, one per renderer view slot, bound to the Camera Preview leaves shown (see RenderAreas)
   bool IsSecondaryViewEditDrag() const; // An interactive edit runs: the secondary views render on their schedule (see RenderAreas)
   bool m_secondaryViewHovered = false; // A secondary view is under the mouse: the 3D Viewport leaves it the view keys (numpad, F, Home, Z)
   int m_secondaryViewSlot = -1; // ... its slot (G / R / S started over it work in that view)
   // What the editing code needs of a secondary view shown this frame (filled by its CameraPreviewPanel, reset every frame): its image rect and the camera
   // of its last render, the draw list of its area window, whether the mouse is over the image
   struct SecondaryViewInfo
   {
      bool valid = false;
      bool imageHovered = false;
      ImVec2 rectMin, rectSize;
      Matrix3D view, proj; // The renderer's convention (see EditorUI::RenderUI)
      Matrix3D mvp; // Model view projection of the image (Renderer::GetEditorViewMVP)
      ImDrawList *drawList = nullptr;
      ImGuiID itemId = 0; // The image's invisible button
   };
   SecondaryViewInfo m_viewInfo[AreaLayout::kMaxViewLeaves];
   int m_gizmoSlot = -1; // The view the gizmo is drawn in and works in: -1 = the 3D Viewport, else a secondary view slot. The mouse picks it; a drag keeps it until it ends
   ScriptPanel m_scriptPanel;
   TableCheckPanel m_tableCheck;
   PinnedPanel m_pinned; // Floating overlay of pinned properties
   SearchPalette m_palette; // Cmd/Ctrl+K search to field

   Selection m_selection;

   // Multi selection of editable parts: all currently selected parts, with m_selection.GetPart() being the
   // active one (property pane target and gizmo pivot). Empty unless m_selection.GetType() == S_EDITABLE.
   vector<std::shared_ptr<EditorUIPart>> m_multiSel;
   vector<IEditable *> m_dynamicParts; // Parts made dynamic in the UI quadtree because they are selected
   std::shared_ptr<EditorUIPart> m_outlinerAnchor; // Anchor part for shift+click range selection in the outliner
   // Multi selection of shared resources (images, sounds, materials): all selected items, with
   // m_selection's payload being the active one. Empty unless m_selection has the matching type.
   vector<Texture *> m_multiSelImages;
   vector<VPX::Sound *> m_multiSelSounds;
   vector<Material *> m_multiSelMaterials;
   Texture *m_outlinerImageAnchor = nullptr; // Anchors for shift+click range selection in the outliner
   VPX::Sound *m_outlinerSoundAnchor = nullptr;
   Material *m_outlinerMaterialAnchor = nullptr;
   bool m_pickListOpen = false; // Ctrl+click list of the parts under the cursor
   ImVec2 m_pickListPos;
   vector<std::shared_ptr<EditorUIPart>> m_pickList;
   bool m_boxSelectActive = false;
   ImVec2 m_boxSelectStart;
   bool IsPartSelected(const std::shared_ptr<EditorUIPart> &part) const;
   bool IsEditableSelected(const IEditable *editable) const;
   void ClearSelection();
   void SetSelection(const Selection &selection);
   void TogglePartSelection(const std::shared_ptr<EditorUIPart> &part);
   void SelectOutlinerRange(const std::shared_ptr<EditorUIPart> &part);
   void SelectPartsInGroup(const PartGroup *group);
   void MoveSelectionToPartGroup(PartGroup *group); // Move all selected parts/groups to the given group (nullptr = root, part groups only)
   IEditable *HitEditable(const HitTestResult &hit) const;
   bool IsEditableVisible(const IEditable *editable, unsigned int visibilityMask = 0) const; // The view mode, the part and its groups are visible: the first half of IsEditablePickable
   // The view a pick is made in: null (the default) is the main 3D view (its renderer MVP, the playfield window), else a secondary view's
   // image (its model view projection as rendered, and the screen rect of its image)
   struct PickView
   {
      Matrix3D modelViewProj;
      ImVec2 rectMin, rectSize;
      unsigned int visibilityMask = 0; // Player mode visibility of a secondary view (0: the 3D Viewport's)
      bool hiddenInPlay = true; // The view shows the parts hidden in play: they can be picked (else they can not, they are not drawn)
      const LocalViewParts *local = nullptr; // The view's Local View: only its parts can be picked (null: the 3D Viewport's)
   };
   bool IsInLocalView(const PickView *view, const IEditable *editable) const { return (view != nullptr ? view->local : &m_localView) == nullptr || (view != nullptr ? view->local : &m_localView)->Allows(editable); }
   void PickRay(const ImVec2 &mousePos, const PickView *view, Vertex3Ds &rayNear, Vertex3Ds &rayFar) const; // World space segment under the mouse
   void RayCastParts(const ImVec2 &mousePos, vector<HitTestResult> &vhoHit, const PickView *view = nullptr) const;
   void DrawSelectionLines(ImDrawList *drawList, const PickView &view); // The 2D part of the selection overlay (lines, colliders) in a secondary view's image
   void SelectAtPoint(const ImVec2 &mousePos, const PickView *view); // A click (no drag) at the mouse position: shift toggles, ctrl lists, else selects / cycles / clears, as in the 3D Viewport
   bool IsEditablePickable(const IEditable *editable, unsigned int visibilityMask = 0) const; // Mask 0: the 3D Viewport's player mode visibility
   bool IsHiddenInPlayPickable(const PickView *view) const; // Whether the view clicked (null: the 3D Viewport) lets the parts hidden in play be picked
   bool IsPartPickableInView(const std::shared_ptr<EditorUIPart> &uiPart, bool hiddenInPlayPickable) const;
   void BoxSelectParts(const ImVec2 &cornerA, const ImVec2 &cornerB, bool add);
   void SelectAllParts();

   // Selection snapshot stored in undo records and restored on undo
   struct UndoSelectionState
   {
      Selection selection;
      vector<std::shared_ptr<EditorUIPart>> multiSel;
      std::shared_ptr<EditorUIPart> outlinerAnchor;
      std::shared_ptr<EditorUIPart> pointEditPart; // Part in drag point edit mode, nullptr when not in that mode
      vector<int> pointSel; // Indices of the selected points in the edited part's curve
      bool pointEditCenter = false; // Whether the part's center point is being edited instead of its drag point curve
   };
   UndoSelectionState CaptureUndoSelection() const;
   void RestoreUndoSelection(const UndoSelectionState &state);

   // Drag point edit mode (entered/exited with Tab when the active selected part has a DragPointCurve):
   // while active, the part's curve points are rendered and can be selected & transformed in the table XY
   // plane. For parts exposing an editable center (light bulb, light sequencer animation center), Tab
   // switches the mode between the drag point curve and the center point before exiting.
   std::shared_ptr<EditorUIPart> m_pointEditPart; // Part whose DragPointCurve is being edited (nullptr when not in point edit mode)
   bool m_pointEditCenter = false; // Edit the part's center point instead of its drag point curve
   bool m_centerSelected = false; // Whether the part's center point is selected in center edit mode
   vector<DragPoint *> m_pointSel; // Selected drag points of the edited part's curve
   Selection m_savedSelection; // Selection state saved on mode entry, restored on exit
   vector<std::shared_ptr<EditorUIPart>> m_savedMultiSel;
   std::shared_ptr<EditorUIPart> m_savedOutlinerAnchor;
   bool m_pointDragPending = false; // Left button is down on a selected point (drag not started yet)
   bool m_pointDragActive = false; // Left button drag is moving the selected points
   float m_pointDragZ = 0.f; // Table Z coordinate of the plane in which points are dragged
   Vertex2D m_pointDragPos; // Last drag position in table coordinates
   bool CanEnterPointEdit() const; // A part with points (or an edit center) is selected and point edit is not active
   void EnterPointEditMode();
   void ExitPointEditMode(bool restoreSelection);
   bool IsPointSelected(const DragPoint *point) const;
   void TogglePointSelection(DragPoint *point);
   DragPoint *HitTestDragPoint(const ImVec2 &mousePos) const;
   bool HitTestEditCenter(const ImVec2 &mousePos) const;
   void BoxSelectPoints(const ImVec2 &cornerA, const ImVec2 &cornerB, bool add);
   Vertex2D UnprojectToPlane(const ImVec2 &mousePos, float z) const;
   void AddPointOnNearestSegment();
   bool RailPointDoubleClick(DragPoint *hitPoint, const ImVec2 &mousePos);
   void DeleteSelectedPoints();

   // DragPointEditContext implementation
   const vector<DragPoint *> &GetSelectedPoints() const override { return m_pointSel; }
   bool IsCenterEditMode() const override { return m_pointEditCenter; }
   void BeginPointEdit() override;
   void EndPointEdit() override;

   // Decorated editable parts (kept sorted for the outliner, and indexed by editable)
   vector<std::shared_ptr<EditorUIPart>> m_editables;
   ankerl::unordered_dense::map<const IEditable *, std::shared_ptr<EditorUIPart>> m_editableMap;
   void UpdateEditableList();
   void RestorePartsVisibility() const; // Revert the editor visibility overrides of the table parts (reapplied on next render)

   // Enter/Exit edit mode (manage table backup, dynamic mode,...)
   void ResetCameraFromPlayer();
   void SetBackdropCamera();
   void SetPlayfieldCamera();
   Vertex2D GetUIVisibleFraction() const;

   // Undo support
   void PushUndo(IEditable *part, unsigned int editId, const string &label = string());
   PinUndo m_undo;
   static string CountLabel(const char *verb, size_t count, const string &name); // "Move PartName" / "Move 3 parts"
   bool IsDragInProgress() const; // Gizmo, point or modal drag: undo and redo are off
   bool CanUndo() const;
   bool CanRedo() const;
   void UndoSteps(size_t count); // Edit > Undo (Ctrl+Z), count > 1 for the undo history
   void RedoSteps(size_t count); // Edit > Redo (Ctrl+Shift+Z / Ctrl+Y)
   void RestoreUndoState(const std::any &state);
   void SwitchWorkspace(int index); // Applies a workspace and reports it in the status bar

   // Edit menu actions (also the keyboard shortcuts and the F3 operators)
   bool CanFrameSelection() const { return m_selection.GetType() == Selection::S_EDITABLE; }
   bool CanCopy() const;
   bool CanPaste() const;
   bool CanDuplicate() const;
   bool CanDelete() const;
   bool CanSelect() const; // Select all / invert
   void DuplicateSelection();
   // Object > Mirror (Ctrl+M, then X or Y): the selection across the table centre line or across the median of the selection, optionally mirrored copies
   bool CanMirror() const;
   void MirrorSelection(int axis, bool aboutMedian, bool duplicate); // axis 0 = X (left / right), 1 = Y (front / back)
   void StartMirrorPrompt(); // Ctrl+M: waits for X or Y (Shift: about the median, D: duplicate mirrored), Esc cancels
   bool HandleMirrorPrompt(); // True when it used the keys of the frame
   bool m_mirrorPending = false;
   bool m_mirrorDuplicate = false;
   void DeleteAction(); // Delete key: the selected drag points in point edit mode, the selected parts otherwise
   void SelectAllAction(); // A key: all curve points in drag point edit mode, the center point in center edit mode, all pickable parts otherwise
   void InvertSelection();

   // Snapping (magnet button of the toolbar), editor preferences: the increments apply to the gizmo and to the G / R / S modal transforms,
   // Ctrl inverts the snapping while transforming
   bool m_snapEnabled = false;
   int m_snapTarget = 0; // 0 = increment (grid), 1 = points (G modal transform, unconstrained: centers and drag points of the unselected parts)
   float m_snapMove = 10.f; // VP units
   float m_snapRotate = 15.f; // Degrees
   float m_snapScale = 0.1f;
   bool m_snapAffectMove = true;
   bool m_snapAffectRotate = true;
   bool m_snapAffectScale = true;
   enum class SnapKind { Move, Rotate, Scale };
   void LoadSnapPreferences();
   void SaveSnapPreferences() const;
   bool IsSnapActive(SnapKind kind) const; // Snapping enabled (inverted while Ctrl is held) and applying to the kind of transform
   void ToggleSnapping();

   // A transform of the selection evaluated from a snapshot taken when it began, not from the state of the previous frame: parts that lose what they can not store (the Z of
   // drag points, the tilt of a Wall, the Z of a Bumper) can not drift, and every frame's result only depends on the operation's current value. p' = c + (p - c) * L + move, with
   // c the pivot (or the part's own origin with Individual Origins), and the part's own rotation or scale following L as far as it can store them (see ApplySelectionXform)
   enum class XformKind { Move, Rotate, Scale };
   // Transform Orientation (Blender's header dropdown): the axes X / Y / Z of G / R / S and of the gizmo. Global = the table's, Local = each part's own (a multi-selection
   // moves, turns and scales each part along its own axes), Normal = Local for parts and Global in point edit (the editor has no surface normal), View = the screen's
   // right, up and depth in the view the transform happens in
   enum class TransformOrientation { Global, Local, Normal, View };
   struct XformOp
   {
      XformKind kind = XformKind::Move;
      Matrix3D linear = Matrix3D::MatrixIdentity(); // L, in table axes
      vec3 move { 0.f, 0.f, 0.f };
      bool ownFrame = false; // linear and move are given in each part's own axes
      // Rotate: the turn about the table Z (radians) of the parts that only store a Z rotation, angle x the axis' table Z share. Read back from linear when not given (the
      // gizmo), which only holds it up to a half turn
      bool hasYaw = false;
      float yaw = 0.f;
      // Rotate with ownFrame: the turn (radians, not limited to a half turn) about axis (the matrix' own sense, in the parts' axes); each part with only a Z rotation
      // takes angle x the table Z share of its own axis
      float angle = 0.f;
      vec3 axis { 0.f, 0.f, 0.f };
      bool negative = false; // Scale: a negative factor (it may reverse two axes, which no determinant shows): the scaled axes are read back as a rotation
      bool operator==(const XformOp &o) const
      {
         return kind == o.kind && ownFrame == o.ownFrame && hasYaw == o.hasYaw && yaw == o.yaw && angle == o.angle && axis.x == o.axis.x && axis.y == o.axis.y && axis.z == o.axis.z && negative == o.negative && move.x == o.move.x && move.y == o.move.y && move.z == o.move.z
            && memcmp(linear.m, o.linear.m, sizeof(linear.m)) == 0;
      }
   };
   struct SelectionXform
   {
      struct Part
      {
         std::shared_ptr<EditorUIPart> part;
         int mask = 0;
         vec3 pos, scale, rot;
         Matrix3D axes; // Unit rows: the part's own axes in table coordinates
      };
      vector<Part> parts;
      struct Point
      {
         DragPoint *point;
         float x, y, z; // z: the point's own Z (a Wire Rail's route is 3D)
         float viewZ; // The height it is drawn at (GetDragPointZ)
      };
      vector<Point> points;
      bool rail = false; // A Wire Rail's points also take Z
      bool centerEdit = false;
      vec3 pivot { 0.f, 0.f, 0.f };
      bool individual = false; // Each part about its own origin
   };

   // Adjust Last Operation (Blender's redo panel, F9): the parameters of the last move / rotate / scale / duplicate / paste / add part,
   // editable as long as nothing else happened (no new undo step, same selection). Adjusting re-evaluates the operation from the state it
   // started from, the undo record (which holds the state before the operation) is unchanged: one Ctrl+Z still reverts all.
   struct LastOperation
   {
      enum class Type { None, Add, Paste, Duplicate, Move, Rotate, Scale, RubberBetweenPosts, Wireform, PlasticFromAtlas, WireRail };
      Type type = Type::None;
      uint64_t undoId = 0; // PinUndo::TopRecordId() after the operation
      vector<std::shared_ptr<EditorUIPart>> selection;
      Matrix3D initial; // Transform of the selection before (move, rotate, scale) or after (placements) the operation
      vec3 base { 0.f, 0.f, 0.f }; // Placements: position the offset is relative to (table coordinates)
      vec3 offset { 0.f, 0.f, 0.f }; // Move: delta, placements: offset from the base (table coordinates)
      vec3 axisView { 0.f, 0.f, 1.f }; // Rotation about the view axis: the axis when the operation was done (camera space)
      int axis = -1; // Rotate / scale: -1 view axis (rotate) or all axes (scale), 0..2 for X / Y / Z
      bool plane = false; // Scale: the axes perpendicular to the axis
      float angle = 0.f; // Degrees
      float factor = 1.f;
      SelectionXform xform; // Move, rotate, scale from G / R / S: the selection the operation is evaluated from
      TransformOrientation orientation = TransformOrientation::Global; // Rotate / scale along an axis: whose axes
      Matrix3D viewFrame = Matrix3D::MatrixIdentity(); // The View orientation's axes (rows, table coordinates) of the view the operation was done in
      bool ownFrame = false; // Move: the offset is in each part's own axes (Local along an axis)
      int rbThickness = 8; // Guided rubber: the created or bound rubber is the selection, adjusting refits it in place
      float rbHeight = 25.f;
      float rbRoundness = 0.f;
      int rails = 1; // Metal Wire Guide profile
      float wireHeight = 25.f;
      float thickness = 3.f;
      float standHeight = 30.f;
      float bendRadius = 8.f;
      float spacing = 12.f;
      int ends = 0;
      float railDiameter = 6.5f; // Wire Rail (rails, wireHeight = height of the route's first point)
      float plHeight = 50.f; // Plastic from Atlas: the created plastic (host) is the selection, adjusting rebuilds it in place
      float plThickness = 5.6f;
      float plBevel = 1.9f;
      int plSegs = 2;
      float plHoleDia = 12.f;
      float plDpi = 150.f;
      bool expanded = true;
   } m_lastOp;
   bool m_adjustPopupOpen = false; // F9
   bool m_adjustPopupJustOpened = false;
   ImVec2 m_adjustPopupPos {};
   void SetLastOperation(LastOperation::Type type, const Matrix3D &initial);
   void ApplyLastOperation();
   bool RenderAdjustParameters(); // Widgets of the parameters, true if one changed
   void RenderAdjustPanel(); // Panel at the bottom left of the viewport, and the F9 popup
   static const char *LastOperationName(LastOperation::Type type);

   // Status bar report (see Report)
   string m_report;
   double m_reportTime = -1000.0;
   IEditable *m_lastUndoPart = nullptr;
   unsigned int m_lastUndoId = 0;
   bool m_suppressNextEscape = false; // The Esc release of an Esc that deactivated an edited field (e.g. the script editor) is not an editor Esc

   // Guided rubbers (see RubberBetweenPosts): the rubbers that follow primitives by name. A record that moves posts must hold the rubbers too
   // (MarkDependentsForUndo, with the record open, before the move), the move then refits them (RefitDependents), so that one undo reverts both.
   vector<Rubber *> DependentRubbers(const vector<IEditable *> &posts) const;
   vector<IEditable *> SelectedEditables() const;
   void MarkDependentsForUndo(); // Rubbers following the selected parts, in the open undo record
   void MarkDependentsForUndo(const vector<IEditable *> &touched);
   void DetachSelectedRubbersFromUnselectedPosts(); // A linked rubber moved without its posts stops following them
   void RefitDependents(const vector<IEditable *> &touched);
   // Hash of the inputs of the last fit of each rubber (a refit with the same inputs is skipped), dropped when an undo or a deletion may have changed the shapes
   std::unordered_map<const Rubber *, uint64_t> m_rubberFitHash;
   // During a drag (modal or gizmo) the rubbers and the post names are resolved once, and the physics of the refit rubbers is updated when the drag ends
   struct RubberDrag
   {
      bool valid = false;
      vector<Rubber *> rubbers;
      RubberBetweenPosts::PostMap posts;
      vector<Rubber *> pendingPhysics;
   } m_rubberDrag;
   void FlushRubberDrag(); // Physics of the rubbers refit during the drag, and forgets the drag caches
   void ClearSelectionTransform(const Matrix3D &transform, bool clearPosition, bool clearScale, bool clearRotation, const char *label);
   void RefitSelectionDependents() { RefitDependents(SelectedEditables()); }
   void DetachRubberRefs(IEditable *part); // The points of a linked rubber were edited by hand: it stops following its posts
   void DropDeletedPostRefs(const vector<IEditable *> &deleted); // In the open undo record of the deletion

   // Add/Remove parts
   void DeleteSelection();
   ItemTypeEnum m_addPartType = eItemInvalid; // Part type pending placement (eItemInvalid when not in add part mode)
   PlasticFromAtlas::Bow m_plasticBow;
   ReferenceImage m_refImage; // View > Reference Image: a scan under or over the playfield in the 3D views (view-only, kept in the table's settings file)
   int SecondaryViewOrdinal(int slot) const; // The secondary view of a panel slot among the view areas of the layout (0 = the first), for per view options
   PlasticFromAtlas::Trace m_plasticTrace; // Add Plastic from Atlas: the trace state (the plastic is created when the loop is closed)
   MeasureTool m_measure; // The Measure tool (Blender's Ruler): view-only rulers kept for the session
   bool m_addWireform = false; // With m_addPartType == eItemPrimitive: the pending part is a Wireform (set whenever add part mode is armed)
   bool m_addWireRail = false; // Same: a Wire Rail
   void CreatePart(ItemTypeEnum type, const Vertex2D &pos, bool wireform = false, bool wireRail = false, const float *z = nullptr); // z: height of the part when its type has one
   void CreateCollection(bool fromSelection); // Create a collection, optionally populated with the selected parts
   PartGroup *GetPartGroupForNewPart();
   vector<PartGroup *> m_partGroupUseHistory; // Recently used insertion target part groups, most recent first

   enum class NewTableTemplate
   {
      Blank,
      Stripped,
      Example,
      LightSeq
   };

   // File operations (the 'Save As' and 'Load' file dialogs are asynchronous: their result is applied in RenderUI)
   void SaveTable();
   void SaveTableAs();
   void SaveTableAsPackFolder(); // Save as a VPZ folder pack
   void LoadTable();
   void LoadTableFolder(); // Load a VPZ folder pack
   void NewTable(NewTableTemplate templateType);
   void ShowLoadTableDialog(bool folder = false);
   void LoadTableDialog(bool folder);
   void LoadTableTemplate(NewTableTemplate templateType);
   std::shared_ptr<string> m_pendingSaveAsPath;
   std::shared_ptr<string> m_pendingLoadPath;
   std::shared_ptr<string> m_pendingUnityPrefabPath;
   bool m_pendingSaveAsFolder = false; // Pending 'Save As' result is a folder pack, not a file
   bool m_pendingLoadFolder = false; // Pending 'Load' result is a folder pack, not a file
   std::optional<NewTableTemplate> m_pendingNewTable; // New table template awaiting the 'discard unsaved changes' confirmation
   bool m_confirmLoadTable = false; // Request the 'discard unsaved changes' confirmation popup in RenderUI

   // Asynchronous file operations: table loading and saving run on a worker thread while a modal progress
   // feedback blocks all user interaction. The result is applied on the main thread in RenderUI.
   struct FileOpFeedback final : public VPXFileFeedback
   {
      std::atomic<unsigned int> progress { 0 };
      std::atomic<unsigned int> length { 0 };
      void SetProgress(const unsigned int value) override { progress = value; }
      void SetLength(const unsigned int value) override { length = value; }
   };
   struct AsyncFileOp
   {
      enum class Type
      {
         Load,
         NewTable,
         Save
      };
      ~AsyncFileOp();
      Type type = Type::Load;
      std::filesystem::path filename; // File being loaded or saved
      string displayPath; // Path displayed in the progress modal
      FileOpFeedback feedback;
      HRESULT result = E_FAIL; // Written by the worker thread before flagging the operation as completed
      std::atomic<bool> completed { false };
      std::thread worker;
      PinTable *table = nullptr; // Loaded table (load ops only): reference adopted by the player on success, released on failure
      PinTable *saveTable = nullptr; // Table written by a save op (the startup table when saving from an inspection session)
      // 'Save As' restore data: filename and title to restore when the save fails
      bool restoreOnFailure = false;
      std::filesystem::path previousFilename;
      string previousTitle;
   };
   // Inspection session: live copies edited from the 'Live' tabs, copied back to the startup table by 'Save'
   // (the startup table is the one written to disk; live values are otherwise discarded when the session ends)
   ankerl::unordered_dense::set<Material *> m_liveEditedMaterials;
   ankerl::unordered_dense::set<IEditable *> m_liveEditedParts;
   std::unique_ptr<AsyncFileOp> m_fileOp; // Running asynchronous file operation (all editor interaction is blocked while set)
   void StartFileOp(std::unique_ptr<AsyncFileOp> op);
   void FinishFileOp();
   void RenderFileOpModal();
   // Whether a save of the edited table is running on the file operation worker thread: while it is, the
   // UI parts must not write into the parts' data which is being serialized (see EditorUI::Render3D)
   bool IsSaveInProgress() const { return m_fileOp != nullptr && m_fileOp->type == AsyncFileOp::Type::Save && (m_fileOp->saveTable == nullptr || m_fileOp->saveTable == m_table); } // Saving this session's table (a save from a play test writes the startup table)

   // Part library (import/export of parts through partial VPZ packs, handled by the PartLibraryPanel, hosted by an area)
   void OpenPartLibrary() { ShowAreaEditor(AreaEditor::PartLibrary); }
   bool CanExportPartSelection() const { return m_partLibrary.CanExportSelection(); }
   void ExportPartSelection() { m_partLibrary.ExportSelection(); }

   // Unity prefab import (Tools > Import Unity Prefab...)
   void PickUnityPrefab();
   void ImportUnityPrefab(const std::filesystem::path &path);

public:
   // Closes the session with the given Player::CloseState, first asking to discard unsaved changes when they would be lost
   void RequestClose(int closeState);
private:
   int m_deferredStop = -1; // Close state of a stop request refused while a save was running, replayed when it finishes
   std::optional<int> m_pendingClose; // Close state awaiting the 'discard unsaved changes' confirmation

   // Clipboard (copy/paste of parts through the OS clipboard, and of drag point coordinates in point edit mode)
   void CopySelection();
   void PasteSelection(const ImVec2 &pos, bool inPlace = false); // inPlace: the parts keep their coordinates (Duplicate), pos is ignored

   // Rendering
   enum class PhysicOverlay
   {
      None,
      Selected,
      All
   };
   PhysicOverlay m_physOverlay = PhysicOverlay::None;
   bool m_selectionOverlay = true;
   enum class SelectionFilter : uint32_t
   {
      None = 0,
      Playfield = 0x0002,
      Primitives = 0x0004,
      Lights = 0x0008,
      Flashers = 0x0010,
      All = 0x0002 | 0x0004 | 0x0008 | 0x0010
   };
   friend constexpr SelectionFilter operator|(const SelectionFilter a, const SelectionFilter b) { return static_cast<SelectionFilter>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
   friend constexpr SelectionFilter operator&(const SelectionFilter a, const SelectionFilter b) { return static_cast<SelectionFilter>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); }
   friend constexpr SelectionFilter operator~(const SelectionFilter a) { return static_cast<SelectionFilter>(~static_cast<uint32_t>(a)); }
   static constexpr bool HasFlag(const SelectionFilter flags, const SelectionFilter flag) { return (flags & flag) != SelectionFilter::None; }
   SelectionFilter m_selectionFilter = SelectionFilter::All;

   // UI state
   bool m_isOpened = false;
   bool m_flyMode = false;
   enum class Units
   {
      VPX, Metric, Imperial
   } m_units = Units::VPX;

   // 3D editor
   ImGuizmo::OPERATION m_gizmoOperation = (ImGuizmo::OPERATION)0;
   bool m_hadSelectionTransform = false; // Whether the previous frame had a selection to transform
   void SetGizmoOperation(ImGuizmo::OPERATION operation); // Edit mode actions of the Esc (select), G (grab), S (scale) and R (rotate) shortcuts (repeating one flips Global / Local)
   TransformOrientation m_transformOrientation = TransformOrientation::Global; // Saved as Editor.TransformOrientation
   void SetTransformOrientation(TransformOrientation orientation);
   bool IsOwnFrameOrientation(TransformOrientation orientation) const; // Local or Normal, outside point edit: each part along its own axes
   Matrix3D GetViewOrientationFrame(int viewSlot) const; // The View orientation's axes (rows, table coordinates): right, up, and towards the viewer (Blender's View Z)
   Matrix3D GetActiveAxes() const; // The active part's own axes (unit rows, table coordinates), identity in point edit or with no part
   // R (value: the angle in radians, freeAxis: the axis with no constraint, camera space) or S (value: the factor) along an axis of an orientation, as an operation on a
   // selection snapshot of partCount parts (G, F9 and the gizmo build theirs from their own move)
   XformOp OrientedXform(XformKind kind, TransformOrientation orientation, const Matrix3D &viewFrame, int axis, bool plane, float value, const vec3 &freeAxis, size_t partCount) const;
   bool GetSelectionTransform(Matrix3D &transform) const;
   bool GetSelectionBounds(FRect3D &bounds, const LocalViewParts *local = nullptr) const; // With a Local View, only the selected parts it shows

   // Transform Pivot Point (Blender's header dropdown): where R and S happen on the selection. All modes use the parts' origins (their reported transforms), never their
   // geometry, as Blender does in Object Mode; with one part they are all the part's origin. In point edit they use the selected points (Individual Origins = Median)
   enum class TransformPivot { MedianPoint, ActiveElement, IndividualOrigins, BoundingBoxCenter, Cursor }; // (the numbers are saved in the settings: new ones go last)
   TransformPivot m_transformPivot = TransformPivot::MedianPoint;
   void SetTransformPivot(TransformPivot pivot);
   vec3 GetTransformPivotPoint() const; // Table coordinates; the gizmo and the mouse of a modal transform work around it (with Individual Origins, the median)

   void BeginSelectionXform(SelectionXform &xform) const;
   // reference: per part, the angles a Primitive's turned matrix is read back nearest to (see EulerFromRotation). By default the ones it stores now (the last frame of a
   // drag, Blender's behaviour), the snapshot's for an op that does not turn (the restore of a cancel)
   void ApplySelectionXform(const SelectionXform &xform, const XformOp &op, const vector<vec3> *reference = nullptr);
   static Matrix3D TurnedAxes(const SelectionXform::Part &part, const XformOp &op); // A Primitive's unit axes under a Rotate op
   void ApplyPartTRS(const std::shared_ptr<EditorUIPart> &part, const vec3 &pos, const vec3 &scale, const vec3 &rot);
   // The gizmo of a selection that does not transform as one part (several parts, drag points): it sits on the pivot and has no scale, what it does is read back as an XformOp
   struct GroupGizmo
   {
      bool active = false; // A drag runs: its matrix lives here
      Matrix3D start, current;
      SelectionXform xform;
      float yaw = 0.f; // The table Z turn of the last frame: the next one is taken nearest to it (continuous past a half turn)
      float yawStep = 0.f; // The last known step between the candidates (2 pi x the Z share), for a frame on a full turn, which does not give it
      bool ownFrame = false; // Local: what the gizmo does in the active part's axes is done by each part in its own
      vec3 axis { 0.f, 0.f, 0.f }; // ownFrame rotation: the axis of the drag (in the active part's axes), its signed angle unwrapped from frame to frame
      float angle = 0.f;
      XformOp last; // The last one set (Adjust Last Operation of a move)
   } m_groupGizmo;
   bool UsesGroupGizmo() const;
   Matrix3D GetGroupGizmoMatrix(int viewSlot) const; // At the pivot, on the axes of the orientation (View: of that view, -1 = the 3D Viewport)

   // Blender style modal transform (G / R / S then move the mouse, X / Y / Z to constrain to an axis (Shift for the plane
   // perpendicular to it), type a number for an exact value, Enter / left click to confirm, Esc / right click to cancel)
   enum class ModalOp { Grab, Rotate, Scale };
   struct ModalTransform
   {
      bool active = false;
      int viewSlot = -1; // The view it works in (its camera, its image rect): -1 = the 3D Viewport, else a secondary view slot
      ModalOp op = ModalOp::Grab;
      int axis = -1; // Constraint axis (0..2 for X/Y/Z), -1 for none (view plane / view axis / uniform)
      bool plane = false; // Constrain to the plane perpendicular to the axis
      bool hasUndo = false; // An undo record was started when the transform began (canceling reverts it)
      string typed; // Typed numeric value
      string hud; // Status text drawn over the render
      ImGuizmo::OPERATION savedGizmo = ImGuizmo::OPERATION(0); // Gizmo tool to restore when the transform ends
      ImVec2 startMouse;
      float startAngle = 0.f, startDist = 1.f; // Angle and distance of the start mouse position around the pivot on screen
      Matrix3D initial; // Selection transform when the operation began
      bool zOnly = false; // Rotation fixed to the table Z axis: an X or Y one would change nothing (drag points, a part that can not tilt turning on its own)
      bool freeZ = false; // No axis means the table Z, not the view axis: no part in the selection takes a tilt
      TransformOrientation axisOrientation = TransformOrientation::Global; // Whose axes the constraint uses: pressing the same axis key again goes from the scene's
      // orientation to the other one of Global / Local, then to no constraint (Blender's G X X)
      Matrix3D activeAxes = Matrix3D::MatrixIdentity(); // The active part's axes (Local: a constrained move follows the mouse along the active part's axis)
      Matrix3D viewFrame = Matrix3D::MatrixIdentity(); // The View orientation's axes in the view the transform works in
      bool lastOwnFrame = false; // lastDelta is in each part's own axes
      SelectionXform xform; // The selection when the operation began
      XformOp applied; // The last one set
      struct SDL_Window *textWindow = nullptr; // The window whose text input this modal turned on
      vec3 duplicateOrigin { 0.f, 0.f, 0.f }; // Position of the duplicated selection
      bool duplicate = false; // Grab following a duplicate: its undo record is folded into the one of the paste on confirmation
      vec3 lastDelta { 0.f, 0.f, 0.f }; // Parameters of the last evaluation (kept as the adjustable parameters of the operation when confirmed)
      vec3 lastAxis { 0.f, 0.f, 1.f }; // Rotation axis (in the editor camera space, see FlipZ)
      float lastAngle = 0.f; // Degrees
      float lastFactor = 1.f;
      vector<Vertex2D> snapPoints; // Snap targets of the 'points' snapping (centers and drag points of the unselected parts), built on first use
      bool snapPointsBuilt = false;
   } m_modal;
   vector<std::shared_ptr<EditorUIPart>> m_gizmoDragSelection; // Selection the current gizmo drag started on
   uint64_t m_gizmoDragRecordId = 0; // Undo record pushed when the current gizmo drag began
   // Parts moved during a drag: the picking tree follows per frame, the gameplay colliders and static shadow pages at the drag end
   struct Drag
   {
      vector<IEditable *> pendingPhysics;
      bool shadowDirty = false;
   };
   mutable Drag m_drag; // Filled by SetSelectionTransform (const)
   void FlushDrag();
   bool m_gizmoDragActive = false; // A gizmo drag is in progress (its undo record was pushed)
   Matrix3D m_gizmoStartTransform; // Selection transform when the current gizmo drag began
   bool m_gizmoDragTranslate = false; // The current gizmo drag is a translation
   int TransformViewSlot() const;
   bool CanStartModalIn(int viewSlot) const;
   void BeginModalTransform(ModalOp op);
   void UpdateModalTransform();
   void EndModalTransform(bool confirm);
   void FrameBounds(const FRect3D &bounds); // Center the editor camera on the bounds and adjust the zoom to fit them
   bool IsNumpadPressed(int digit, bool repeat = true) const;
   bool GetNavPivot(vec3 &target) const; // Orbit / zoom pivot (selection center), false if none
   void SetSelectionTransform(const Matrix3D &transform, bool clearPosition = false, bool clearScale = false, bool clearRotation = false);

   // Editor camera
   ViewMode m_camMode = ViewMode::PreviewCam;
   bool m_perspectiveCam = true;
   bool m_navCubeFaceAnim = false; // The nav cube is animating the view toward a clicked face (orthographic is forced once, when it starts)
   Renderer::ShadeMode m_shadeMode = Renderer::ShadeMode::Default;
   ViewShadeOptions m_viewShade; // The 3D Viewport's view-only shading options (Viewport Shading popover), not saved
   enum class PredefinedView
   {
      None, Front, Back, Right, Left, Top, Bottom
   } m_predefinedView = PredefinedView::None;
   bool m_navZoomToMouse = true; // Navigation preference: wheel zoom goes toward the cursor
   int m_navSelectionDisplay = 0; // 0 = edges only, 1 = edges and tint, 2 = hidden
   bool m_navAutoDepth = true; // Navigation preference: with no selection, orbit around the 3D point under the cursor
   bool m_orbitDragging = false; // Orbit / pan drag in progress
   bool m_orbitPivotValid = false; // m_orbitPivot (camera space) is the orbit pivot of the current drag
   vec3 m_orbitPivot;
   bool PickDepthPoint(const ImVec2 &mousePos, vec3 &point, const PickView *view = nullptr) const;
   bool PickSurfacePoint(const ImVec2 &mousePos, vec3 &point, const PickView *view = nullptr) const; // The table space point under the mouse: the front-most part hit, else the playfield plane (z = 0)

   // 3D cursor (Blender's): a point of the table space (view state of the editing session, not in the table file, never drawn in play). Shift+right click places it
   // on the surface under the mouse, Shift+S opens the snap menu. Add Part places the new part at its X / Y (and Z where the part type has one).
public:
   const vec3 &Get3DCursor() const { return m_cursor3D; } // Table space (x, y, z up)
   void Set3DCursor(const vec3 &position) { m_cursor3D = position; }
private:
   enum class CursorOp { CursorToSelected, CursorToWorldOrigin, CursorToGrid, SelectionToCursor, SelectionToCursorKeepOffset, SelectionToGrid };
   vec3 m_cursor3D { 0.f, 0.f, 0.f }; // Set by Reset3DCursor
   void Reset3DCursor(); // To the playfield centre at z = 0
   bool m_cursorMenuRequest = false; // Shift+S pressed: the menu is opened from the overlay window scope (see RenderUI)
   ImVec2 m_cursorMenuPos;
   int m_cursorMenuSlot = -1; // The view the menu was asked in (a secondary view's slot, else -1): its Local View decides what can be moved
   void Place3DCursor(const ImVec2 &mousePos, const PickView *view); // Shift+right click
   void DrawCursor3D(ImDrawList *drawList, const EditorRenderContext &ctx, const ImVec2 &clipMin, const ImVec2 &clipMax) const;
   void DrawCursor3DInView(ImDrawList *drawList, const PickView &view) const; // In a secondary view's image
   bool CanRunCursorOp(CursorOp op) const;
   void RunCursorOp(CursorOp op, int viewSlot);
   void RenderCursorMenu();
   bool GetSelectionCenter(vec3 &center, int viewSlot) const; // Median of the origins of the selected parts (or the points) the view shows
   void ApplyPartTransform(const std::shared_ptr<EditorUIPart> &part, const Matrix3D &partTransform, bool clearPos, bool clearScale, bool clearRot); // One part under a transform, with the redraw and physics follow-up
   // A part's matrix (as its GetTransform gives it) read back as the position, size and rotation its SetTransform takes. A Primitive keeps the Rot X / Y / Z nearest
   // to the ones it stores, also at the gimbal lock (Rot Y +-90)
   static void ReadPartTransform(EditorUIPart &part, const Matrix3D &transform, vec3 &pos, vec3 &scale, vec3 &rot);
   void PlaceNewPartAtCursor(); // Add part mode armed (outside the desktop backdrop): the part is created at once at the 3D cursor
   float m_navFovY = 39.6f; // Editor perspective camera vertical field of view in degrees (50 mm lens equivalent)
   void SetFov(float fovY);
   bool m_navInvertZoom = false; // Navigation preferences (stored in the Editor section of the settings)
   bool m_navAltMiddleButton = true;
   bool m_navTrackpad = true;
   float m_navOrbitSensitivity = 1.f;
   void LoadNavPreferences();
   bool m_navBlenderTransform = true; // Navigation preference: G / R / S start a modal transform instead of the gizmo
   bool m_navEmulateNumpad = true; // Navigation preference: top row digits act as the numpad view keys
   bool m_navOrbitSelection = true; // Navigation preference: orbit and zoom around the selection
   void SetView(PredefinedView view); // Axis aligned view (also reachable from the View menu)
   void CycleCamera();
   void FrameSelection(); // Editor camera centered on the selection, zoomed to fill the view (Blender's View Selected, F / numpad .)
   void FrameAll(); // Same for the whole table (Home)
   // Local View (Blender's numpad /, View > Local View): a view limited to the parts selected when it was entered, hiding all the others in that view only, framed;
   // again, everything and the camera it was entered from. View-only: no part, undo step, table change or file is touched; it ends with a play session or a table change.
   // Each view (the 3D Viewport, each secondary view) has its own, see LocalViewParts
   LocalViewParts m_localView;
   struct LocalViewCamera
   {
      ViewMode camMode;
      bool perspective;
      PredefinedView predefined;
      Matrix3D view;
      float distance;
   } m_localViewCamera;
   void ToggleLocalView(); // The 3D Viewport's
   void EndLocalViews(); // All of them, cameras given back (a play session or a table change)
   bool BuildLocalViewParts(LocalViewParts &parts) const; // The selection (a part group's parts too) as a Local View's parts; false when nothing is selected
   bool GetLocalViewBounds(const LocalViewParts &parts, FRect3D &bounds) const; // The bounds of the parts a Local View shows (what it frames); false when it has none
   void ExtendBoundsWithPart(FRect3D &bounds, const std::shared_ptr<EditorUIPart> &part) const; // Hit boxes and position of one part
   const LocalViewParts *KeyLocalView() const; // The Local View a view key works on: the hovered secondary view's, else the 3D Viewport's
   const LocalViewParts &ViewLocalView(int slot) const; // The Local View of a view: a secondary view's slot, or -1 for the 3D Viewport
   bool SelectionInView(int slot) const; // Every selected part is in the view's Local View (always, when it is not local): the gizmo, G / R / S and the pivot work only then, nothing is transformed unseen
   // A part added while a view is local joins it (as Blender's): UpdateLocalViews finds the new parts of the table, by the table's parts of the previous frame
   std::map<const IEditable *, string> m_localKnown; // Parts of the table (and their names) as of the last frame a view was local
   bool m_localUndoRan = false; // An undo / redo ran since the Local Views were last updated: a part which then appears may be one it returned
   vector<string> m_localHiddenLost; // Names of the parts which left the table (deleted) while hidden by every view: an undo bringing one back does not show it
   void UpdateLocalViews(); // Every frame: drops the parts which left the table from the Local Views, takes them back if an undo returns them
   void OrbitStep(bool yaw, float sign); // 15 degree orbit step
   bool m_orbitLock = false; // When locked, camera drags pan instead of orbiting
   bool m_fitPlayfieldCamera = false;
   Matrix3D m_camView, m_camProj;
   float m_camDistance;
   bool m_showNavCube = true; // View > Navigation Cube
   // Areas: Blender-style split tree (see editor/AreaLayout.h): Outliner | Viewport + N-panel | Properties by default. Hidden leaves are the
   // View menu show / hide toggles. Widths are logical pixels stored in the tree, resized by the splitters, saved per workspace.
   float m_sidebarWidth = 300.f;
   // N-panel (Blender's sidebar at the viewport's right edge): tabs Item (selection transform), Favorites (the Quick Favorites set), View
   enum class SidebarTab
   {
      Item,
      Favorites,
      View
   };
   bool m_showSidebar = false;
   SidebarTab m_sidebarTab = SidebarTab::Item;
   void ToggleSidebar(); // N
   void RenderSplitters();

   // Editor types of the areas (Blender's editor type switcher, the button at the left of each area header). Any number of areas may show a
   // type, as in Blender: picking a type re-types only that area. Saved per workspace by name in the "layout" tree, with each panel's own state
   // (the legacy left_editor / right_editor ints keep the AreaType order).
   using AreaEditor = AreaType;
   static const char *AreaEditorName(AreaEditor type);
   static const char *AreaEditorIcon(AreaEditor type);
   // Layout changes (type switch, show / hide, split, join, swap, maximize) are queued and applied by UpdateAreaLayout at the start of the
   // next UI frame, before any panel is drawn: a panel never disappears between its BeginUndo and its EndUndo / Discard
   struct PendingAreaOp
   {
      enum class Kind
      {
         SetType, // leaf, type: re-type the area, widening it if too narrow for the new editor
         Show, // type: reveal the editor, hosting it in its preferred area if needed
         Toggle, // type: hide / show the area hosting the editor, or host it
         Split, // leaf, axis, newOnSecondSide, size
         Join, // leaf (kept), leaf2 (consumed)
         Swap, // leaf: id of the split
         Close, // leaf: close the area, its sibling takes the space
         Maximize // leaf: leaf to maximize, -1 to restore
      };
      Kind kind = Kind::SetType;
      int leaf = -1;
      int leaf2 = -1;
      AreaType type = AreaType::Outliner;
      SplitAxis axis = SplitAxis::Vertical;
      bool newOnSecondSide = true;
      float size = 0.f;
   };
   void QueueAreaOp(const PendingAreaOp &op) { m_pendingAreaOps.push_back(op); }
   void SetAreaEditor(int leafId, AreaEditor type); // Editor type of the area (leaf id) (queued)
   void SetAreaLayout(const AreaLayout &layout); // Workspace restore (immediate): clears the pending ops, the maximized area and the drags
   bool IsAreaEditorHosted(AreaEditor type) const { return m_areas.IsHosted(type); }
   bool IsAreaEditorShown(AreaEditor type) const; // Hosted by a visible area (any of them)
   void ShowAreaEditor(AreaEditor type); // Make the type visible (queued): nothing if an area shows it, else reveals the first area hosting it, or hosts it in its preferred area (Script, Properties: rightmost, others: leftmost)
   bool GetAreaUnderMouse(AreaEditor &type, bool &sidebar, int *leafId = nullptr) const; // Area editor (and its leaf) under the mouse (or the N-panel), false over the viewport
   void ToggleAreaEditor(AreaEditor type); // View menu (queued): hide every area of the type when one is shown, else show them all, or host it
   void ToggleMaximizeArea(); // Ctrl+Space and View menu (queued): maximize the area under the mouse (the last one hovered, for the menu), or restore
   ImVec2 GetViewportContentCenter() const; // Centre of the 3D view's visible part (paste position)
   void UpdateAreaLayout(); // Start of the UI frame: applies the pending ops, then computes the area rects
   void ComputeAreaRects() const; // Tree rect from the bar heights, then the cached leaf / splitter / viewport rects
   void RenderAreas(); // Hosts the panels of the visible areas
   void RenderAreaTypeButton(AreaEditor current, int leafId = -1); // Header button of the area hosting the given type (or of the given leaf), and its editor type popup
   bool ApplyAreaOp(const PendingAreaOp &op);
   void WidenArea(int leafId, AreaType type); // Switching the editor of a too narrow area widens it (once, the splitter can shrink it again afterwards)
   AreaLayout m_areas;
   // Any number of areas of a panel type (Blender). Each Outliner and Properties area has its own panel (display mode, filters, search, pin, tab),
   // created when the area appears with the state saved in its leaf and dropped with the area (closed or re-typed): a new area always starts clean.
   // A workspace not shown keeps its areas' panels (parked). A split's new area gets a copy of the split area's live panel state (m_areaCopies).
   // Script and Part Library are one panel each drawn in every area of the type (one script, one library), each area being its own window
   std::map<int, std::unique_ptr<OutlinerPanel>> m_outliners; // By leaf id
   std::map<int, std::unique_ptr<PropertiesPanel>> m_propertiesAreas; // By leaf id
   // Panels of the areas of the workspaces not shown, by leaf id: given back when their workspace is shown again (BindAreaPanels)
   std::map<int, std::unique_ptr<OutlinerPanel>> m_parkedOutliners;
   std::map<int, std::unique_ptr<PropertiesPanel>> m_parkedProperties;
   struct AreaCopy
   {
      int from = -1, to = -1; // Leaf ids: split area, new area
   };
   vector<AreaCopy> m_areaCopies; // Splits applied this frame, copied once the new areas have their panels
   void BindAreaPanels(); // Panels of the areas of the tree (after any change of m_areas)
   void RefreshParkedStates(AreaLayout &layout) const; // A hidden workspace's saved tree: its areas' states from their parked panels (table save)
   void SyncAreaStates(); // The panels' states written to their leaves of m_areas (before a split copies one, a save or a workspace capture)
   AreaLayout GetAreaLayoutWithState() const; // m_areas with the panels' current states
   OutlinerPanel *GetOutlinerArea(int leafId) const;
   PropertiesPanel *GetPropertiesArea(int leafId) const;
   // The first shown Properties area following the selection, else m_properties (prepare: its curation is first prepared on the current selection; it
   // may harvest in a hidden window, so it is called before a window is begun)
   PropertiesPanel &GetSelectionProperties(bool prepare = true);
   const PropertiesPanel *GetJumpPropertiesPanel() const; // Properties area a search palette jump goes to: the first shown one following the selection, else the first shown one
   std::vector<PendingAreaOp> m_pendingAreaOps;
   int m_maximizedLeafId = -1; // Transient (not saved): only this leaf is laid out, on the whole tree rect
   // Rects of the last ComputeAreaRects (physical pixels): leaves, splitters, the viewport leaf and its content (the leaf without the N-panel).
   // The 3D view is the full window, the viewport leaf is a window onto it.
   mutable std::vector<LeafRect> m_leafRects;
   mutable std::vector<SplitterRect> m_splitterRects;
   mutable float m_areaRootH = 1080.f; // Height of the area tree bounds of the last layout
   mutable AreaRect m_viewportRect, m_viewportContentRect;
   mutable bool m_areaRectsValid = false;
   mutable bool m_viewportVisible = true; // False while a panel is maximized: the viewport rects are then empty and no viewport overlay is drawn
   mutable string m_areaRectsLog;
   int m_splitDragNode = -1; // Splitter drag: node id and unrounded divider position
   float m_splitDragPos = 0.f;
   // Corner drags (Blender's area split / join), the edge menu and its interactive split / join modes. Everything is drawn on the foreground
   // draw list, the tree is only changed through the queued ops. Nodes are kept by id and re-resolved every frame: a vanished node cancels
   struct AreaSplitPlan
   {
      bool ok = false;
      const char *reason = ""; // Why not, when !ok
      SplitAxis axis = SplitAxis::Vertical;
      AreaRect leaf; // Area to split
      AreaRect newRect; // Where the new area would be (ok only)
      float line = 0.f; // Divider coordinate (x for a Vertical split, y for a Horizontal one), whole pixels, inside the area
      float size = 0.f; // Logical extent of the new area (ok only)
      AreaType newType = AreaType::Count; // Editor of the new area (AreaLayout::NewLeafType)
   };
   struct AreaDrag
   {
      bool active = false;
      int leaf = -1; // Area the drag started in
      int corner = 0; // 0 top left, 1 top right, 2 bottom left, 3 bottom right
      ImVec2 start = ImVec2(0.f, 0.f);
      bool decided = false; // Moved far enough: split inside the area, join in another one
      bool axisChosen = false; // The split axis is re-evaluated until the drag passes 20 px, then stays locked
      SplitAxis axis = SplitAxis::Vertical;
   };
   enum class AreaModalKind
   {
      None,
      Split, // Edge menu 'Vertical / Horizontal Split': the preview follows the mouse, click confirms
      Join // Edge menu 'Join Areas': the area not under the mouse is consumed, click confirms
   };
   AreaDrag m_areaDrag;
   AreaModalKind m_areaModal = AreaModalKind::None;
   int m_areaModalNode = -1; // Split node (edge) the mode was started from
   SplitAxis m_areaModalAxis = SplitAxis::Vertical;
   bool m_areaModalFocus = false; // Bring the input catcher window to the front (first frame of the mode)
   int m_edgeMenuNode = -1;
   bool m_edgeMenuRequest = false;
   ImVec2 m_edgeMenuPos = ImVec2(0.f, 0.f);
   int m_hoverLeaf = -1; // Area under the mouse at the start of the frame (not while a popup or the palette covers the areas)
   bool m_sidebarWasShown = false; // The N-panel window is created above the corner zones the first time it is shown: raise them then
   bool m_raiseAreaWindows = false; // After a layout change: splitter strips and corner zones are brought to the front again, in that order (corners win)
   bool IsViewportVisible() const { return m_viewportVisible; } // The 3D view is shown (not hidden behind a maximized panel): gate for the viewport overlays
   bool ShouldRaiseAreaWindows() const; // A layout change is pending a raise, and no popup is open (it would be covered)
   bool CanUseAreaTools() const; // Corner zones, edge menu and their modes: off during G / R / S, gizmo / point drags, point edit, running game inspection, file ops, palette, maximize
   const LeafRect *FindLeafRect(int leafId) const; // Visible leaf of the last ComputeAreaRects
   bool PlanAreaSplit(int leafId, SplitAxis axis, bool newOnSecondSide, const ImVec2 &cursor, AreaSplitPlan &plan) const;
   const char *AreaJoinRefusal(int keepId, int consumeId) const;
   void DrawAreaSplitPreview(const AreaSplitPlan &plan) const;
   void DrawAreaJoinPreview(int keepId, int consumeId, bool ok) const;
   void RenderAreaCorners(); // Corner action zones of the visible areas, then the drag in progress
   void UpdateAreaDrag();
   void RenderAreaEdgeMenu(); // Right click menu of the splitters, and the modes it starts
   void RenderAreaModal();
   void CancelAreaInteraction(bool consumeEscape); // Drag, mode and menu
   bool ConsumedAreaEscape(); // The Esc that closed the editor type popup is not an editor Esc
   bool m_areaEscPending = false;
   bool ConsumedPaneEscape(); // The Esc that cancelled a property pane field edit is not an editor Esc
   bool m_paneEscPending = false;
   bool BeginAreaWindow(const char *name, const ImVec2 &pos, const ImVec2 &size); // Window filling an area rect (Script and Part Library), always pair with EndAreaWindow
   void EndAreaWindow();
   void ShowSidebar(SidebarTab tab);
   void OpenQuickFavorites() { m_openQuickFavorites = true; } // Q: popup of the Quick Favorites at the mouse
   bool m_openQuickFavorites = false;
   void RenderNavCube(); // ImGuizmo view cube: drag orbits around the orbit pivot, click a face snaps to the axis view
};

}
