// license:GPLv3+

#pragma once

#include "imgui/imgui.h"
#include "AreaLayout.h"
#include "math/bbox.h"
#include "parts/pintable.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct ImVec2;
class Sampler;
class Light;

namespace VPX::EditorUI
{

class EditorUI;
class EditorUIPart;

// Part library, hosted by an editor area (Blender's editor type switcher): export of the current part selection to a partial VPZ pack, and import of
// parts from a specific VPZ file or from a browsable list of library folders holding VPZ
// packs (zip files, or folders holding a manifest.json) organized in subfolders, shown as a grid of thumbnail tiles. The import merge strategy
// defines how conflicts between imported assets and same named table assets are resolved.
// Thumbnails are rendered while the area is shown, one at a time, from a scratch table drawn alone through a renderer secondary view slot
// (Renderer::EditorViewRequest::isolated), and cached beside the pack: thumbs/ inside a folder pack, a '<name>.vpz.thumbs' folder beside a zip.
// Test hook: VPX_LIBRARY_THUMBS=<folder> renders the missing thumbnails of that library folder at startup, without showing the area.
// Clicking a tile opens a lightbox (modal, resizable): the item live in its own view slot (its own scratch table, drawn alone), orbited with the left
// button, wheel zoom, double click to frame; inserts have Lit / Unlit / Blink buttons driving the pack's own Light (a state driver only: the pass has no light).
// Test hook: VPX_LIBRARY_PREVIEW=<pack path>;<lit|unlit|blink>;<yaw> opens the lightbox on that pack at startup, logging 'LIBPREVIEW open <pack>' once rendered.
class PartLibraryPanel
{
public:
   explicit PartLibraryPanel(EditorUI &editor);
   ~PartLibraryPanel();

   void Show() { m_rescanLibrary = true; } // The editor was just switched to this type: rescan the library folders

   // Export of the current selection to a VPZ pack file, disabled when there is no exportable selection
   bool CanExportSelection() const;
   void ExportSelection();

   void OnEditorClosed(); // EditorUI::Close: drops the lightbox and the thumbnail in flight (scratch tables, view slots)
   // Pack placement (a tile dragged into the 3D view, or double clicked): the pack is loaded in a scratch table and previewed as ghost parts resting on the
   // topmost surface under the pointer; commit moves its content to the edited table (one undo step, 'Place <pack>').
   bool IsPlacementArmed() const { return !m_previewParts.empty(); }
   void CancelPlacement();
   void CommitPlacement();
   void Render3D(); // Renders the ghost preview parts into the 3D scene (called by EditorUI::Render3D)
   void UnloadImages(PinTable *table); // The table's images are deleted with it: out of the texture manager first
   void ClearPreview(); // Drops the preview parts, e.g. when the edited table is switched or the editor is closed

   void Update(); // Every frame, whether the editor is shown or not: applies the results of the asynchronous file dialogs
   void RenderInArea(const ImVec2 &pos, const ImVec2 &size); // Window filling the given area rect, header included

private:
   // A library folder, recursively scanned for importable VPZ packs
   struct LibraryFolder
   {
      std::filesystem::path path;
      vector<LibraryFolder> subFolders;
      vector<std::filesystem::path> packs;
   };

   enum class ItemKind
   {
      Part, // A pack with parts
      Material, // A material of a pack without parts
      Insert, // A pack holding a traced insert card (a primitive with add_blend and a TRACED_ image)
   };

   // Summary of the importable content of a VPZ pack
   struct PackInfo
   {
      bool valid = false;
      ItemKind kind = ItemKind::Part;
      vector<string> materialNames;
      bool hasTable = false;
      int parts = 0;
      int collections = 0;
      int materials = 0;
      int images = 0;
      int sounds = 0;
      int fonts = 0;
   };

   // A library tile: a pack, or a material of a pack without parts
   struct LibraryItem
   {
      std::filesystem::path pack;
      string catalog; // The first level library folder it was found in (lower case), empty: at the root
      string material; // Material items: the material name
      string label;
      ItemKind kind = ItemKind::Part;
      std::filesystem::path thumbFile;
      float norm = 1.f; // Inserts: the cached normalization factor
      std::shared_ptr<Sampler> thumb;
      bool thumbLoaded = false; // Load attempted
   };

   // The Traced card's look parameters the lightbox edits (per pack, kept for the session, applied to the placed card)
   struct InsertLook
   {
      float gain = 8.f, depth = 0.f, spot = 1.f, bright = 1.f, ox = 0.f, oy = 0.f;
      bool operator==(const InsertLook &) const = default;
   };
   // A session override: only the edited fields (mask bits gain, depth, spot, bright, ox, oy = 1..32) are written, to every Traced card (cards keep their other values)
   struct LookOverride
   {
      InsertLook look;
      unsigned mask = 0;
   };
   static bool PackLook(PinTable *table, InsertLook &look); // The first Traced card's values
   static vector<Primitive *> TracedCards(PinTable *table);
   static void ApplyLook(PinTable *table, const LookOverride &override);
   LookOverride ResolveLook(const std::filesystem::path &pack, PinTable *table); // The session override of the pack (mask 0: none)
   void ApplyLightboxLook(); // The cards' own values, then the override

   static constexpr unsigned int kRigVersion = 7; // Bump when the thumbnail rig changes: every thumbnail is rendered again
   static constexpr int kThumbSize = 256;
   // Insert previews are normalized: the card's alpha is scaled (never up) until the 90th percentile display luminance of the item's pixels reaches this value
   // (the red round insert, the least blown of the set, as it read at factor 1); the factor is cached beside the thumbnail ('<name>.norm')
   static constexpr float kNormTarget = 0.70f;

   void RescanLibrary();
   void RenderGrid(float height);
   void RenderGridControls(); // Kind filter, name search and sort of the tiles (kept in the app settings)
   void SaveGridView() const;
   void PumpThumbnails(); // Renders the missing thumbnails, one at a time
   bool LoadScratch(const LibraryItem &item, float factor = 1.f);
   void ReleaseScratch();
   bool LoadScratch(const LibraryItem &item, CComObject<PinTable> *&scratch, vector<IEditable *> &scratchParts, float factor = 1.f);
   void ReleaseScratch(CComObject<PinTable> *&scratch, vector<IEditable *> &scratchParts);
   static FRect3D ScratchBounds(const vector<IEditable *> &parts);
   // Pack placement: the pack is loaded in a scratch table so that it never pollutes the edited one (no outliner entry, no undo, not in the picking
   // quadtree) until it is committed. Its authored positions are overridden by PlaceAt (moved as a rigid group)
   void StartPlacement();
   void UpdatePlacement(const ImVec2 &mousePos);
   void PlaceAt(const Vertex2D &pos);
   Vertex2D DefaultPlacementPos() const; // Centre of the selection bounds, else the table centre
   bool PlaceItem(const LibraryItem &item); // Double click: placed at the default position
   void ApplyMaterialItem(const LibraryItem &item); // Double click on a material: imported, and assigned to the selected parts in the same undo step
   void SetupPlayerSide(IEditable *part, bool initRenderable);
   void OpenLightbox(const LibraryItem &item);
   void CloseLightbox();
   void RenderLightbox(); // Every frame (Update): the modal while open
   void SetLightboxState(float state); // 0 unlit, 1 lit, 2 blinking (Light::setInPlayState)
   bool AnimateLights(float dtMs); // True when a light's intensity changed
   void FrameLightbox(bool resetAngles);
   void AbortThumbnail();
   static uint64_t PackKey(const std::filesystem::path &pack);
   static std::filesystem::path ThumbFolder(const std::filesystem::path &pack);
   void RenderLibraryMenu(); // The header menu
   void RenderCatalogs(float height); // The catalog column left of the grid
   vector<std::filesystem::path> GetLibraryFolders() const;
   void SetLibraryFolders(const vector<std::filesystem::path> &folders);
   void AddLibraryFolder(const std::filesystem::path &folder);
   void RemoveLibraryFolder(const std::filesystem::path &folder);
   PackInfo ScanPack(const std::filesystem::path &path);
   void ImportPack(const std::filesystem::path &path);

   EditorUI &m_editor;

   // Results of the asynchronous SDL file dialogs, applied on the main thread in Update
   std::shared_ptr<string> m_pendingExportPath;
   std::shared_ptr<string> m_pendingImportPath;
   std::shared_ptr<string> m_pendingFolderPath;

   // Import options
   PartImportMergeStrategy m_mergeStrategy = PartImportMergeStrategy::RenameConflict;

   // Library content, rebuilt on Show and whenever the folder list changes
   bool m_rescanLibrary = true;
   vector<LibraryFolder> m_library;
   string m_selectedLabel; // With the pack: the last clicked tile
   std::filesystem::path m_selectedPack;
   PackInfo m_selectedPackInfo;
   vector<LibraryItem> m_items;
   string m_catalog; // Active catalog (a first level folder of the library folders, lower case), empty: all
   vector<string> m_catalogs; // The catalogs found (display names), sorted
   int m_sortKind = 0; // 0 name, 1 kind then name
   char m_search[64] = {};
   bool m_viewLoaded = false;

   // Thumbnail rendering: the queue of items to render, the pack keys to write once their last item is rendered
   vector<size_t> m_thumbQueue;
   std::map<std::filesystem::path, std::pair<string, int>> m_packKeys; // Pack -> (key, items left to render)
   enum class ThumbStage { Idle, Requested, ReadBack } m_thumbStage = ThumbStage::Idle;
   int m_thumbIter = 0; // Insert normalization: renders done for the item in flight, and the factor of the current one
   float m_thumbFactor = 1.f;
   int m_thumbSlot = -1; // Renderer view slot held while thumbnails are rendered
   uint32_t m_thumbTicket = 0, m_readSeq = 0; // Read back of the item in flight (RenderDevice::RequestVRScreenshot ticket)
   CComObject<PinTable> *m_scratch = nullptr; // Never saved
   vector<IEditable *> m_scratchParts;
   int m_lastShownFrame = -1;
   int m_thumbsRendered = 0, m_thumbsCached = 0, m_thumbsFailed = 0;
   // Placement preview (see above): m_placementPack is the armed pack, the preview table holds its loaded content (non group parts are tracked for
   // translation & rendering)
   std::filesystem::path m_placementPack;
   CComObject<PinTable> *m_previewTable = nullptr;
   vector<IEditable *> m_previewParts;
   vector<std::shared_ptr<EditorUIPart>> m_previewUIParts;
   bool m_previewVisible = false; // Ghost currently rendered (pointer over the 3D view)
   Vertex2D m_previewPos;
   float m_previewZ = 0.f;
   int m_placedCount = 0; // Parts of the last commit
   bool m_dragActive = false; // A tile is being dragged: released over the 3D view commits
   bool m_dragCancelled = false; // Esc pressed during the drag: the release cancels
   bool m_dragLatch = false; // A drag started with this mouse press (no second one before the release)
   // Tile clicks: a single click opens the lightbox once the double click interval passed, a double click places instead
   LibraryItem m_pendingItem;
   bool m_pendingOpen = false;
   double m_pendingTime = 0.0;
   LibraryItem m_doubleItem;
   bool m_doubleClicked = false; // Handled in Update, outside of the panels' undo records
   string m_hookPlace; // VPX_LIBRARY_PLACE=<pack path>[;x;y]
   bool m_hookPlaceHasPos = false;
   Vertex2D m_hookPlacePos;
   int m_hookPlaceFrames = 0;

   // Lightbox: own scratch table and view slot (independent of the thumbnails, which pause while it is open)
   bool m_lbOpen = false;
   LibraryItem m_lbItem;
   CComObject<PinTable> *m_lbScratch = nullptr;
   vector<IEditable *> m_lbParts; // Rendered parts (the lights are state drivers, not drawn)
   vector<Light *> m_lbLights;
   FRect3D m_lbBounds;
   AreaViewSettings m_lbCam;
   int m_lbSlot = -1;
   int m_lbWidth = 0, m_lbHeight = 0, m_lbPendingW = 0, m_lbPendingH = 0, m_lbPendingFrames = 0; // Render size (px), a new size adopted once stable
   bool m_lbDirty = true; // A render is wanted
   bool m_lbRequested = false, m_lbLogged = false, m_lbFailed = false;
   PackInfo m_lbInfo;
   float m_lbWantYaw = 35.f, m_lbWantState = 1.f; // Applied when the scratch table is set up
   float m_lbState = 1.f; // 0..1 lit level, 2 blinking
   double m_lbClock = 0.0; // ms, the lights' time base (Light blinks on Player::m_time_msec, which does not run while editing)
   double m_lbAnimUntil = 0.0; // Lit / Unlit: animate until the fade settled or this time
   InsertLook m_lbLook, m_lbPackLook; // Edited look, and the pack's own
   bool m_lbHasCard = false; // The scratch table has a Traced card
   string m_lbLayoutLog; // The last LIBLAYOUT line
   bool m_lbShowLook = false; // The look column shown by the gear toggle (a narrow panel)
   bool m_lbReal = false; // 'Table brightness': the normalization is off
   vector<InsertLook> m_lbOrig; // Each Traced card's own values
   std::map<std::filesystem::path, LookOverride> m_looks; // Overrides of the packs, for the session (never written to a pack)
   std::map<string, float> m_hookLook; // VPX_LIBRARY_PREVIEW look fields (gain, depth, spot, bright, ox, oy)
   std::filesystem::path m_hookLookPack;
   bool m_lbOpening = false; // Open the popup this frame
   string m_hookPreview, m_hookPreviewLog; // VPX_LIBRARY_PREVIEW: the pack path as given
   float m_hookYaw = 35.f;
   float m_hookState = -1.f;
   bool m_hookActive = false; // VPX_LIBRARY_THUMBS test hook running
   std::filesystem::path m_hookFolder;
};

}
