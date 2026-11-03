// license:GPLv3+

#pragma once

#include "imgui/imgui.h"
#include "parts/pintable.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct ImVec2;
class Sampler;

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
//
// Pack placement: selecting a pack in the library loads its whole content in a temporary
// table and previews it as ghost parts following the mouse over the 3D view (resting on the
// topmost surface under the pointer). Clicking commits the pack content to the edited table;
// unselecting or closing the dialog discards it.
class PartLibraryPanel
{
public:
   explicit PartLibraryPanel(EditorUI &editor);
   ~PartLibraryPanel();

   void Show() { m_rescanLibrary = true; } // The editor was just switched to this type: rescan the library folders

   // Export of the current selection to a VPZ pack file, disabled when there is no exportable selection
   bool CanExportSelection() const;
   void ExportSelection();

   void Update(); // Every frame, whether the editor is shown or not: applies the results of the asynchronous file dialogs
   void RenderInArea(const ImVec2 &pos, const ImVec2 &size); // Window filling the given area rect, header included

   // Pack placement mode: a pack of the library is selected for preview & placement
   bool IsPlacementArmed() const { return !m_previewParts.empty(); }
   void CancelPlacement();
   void CommitPlacement();

   void Render3D(); // Renders the ghost preview parts into the 3D scene (called by EditorUI::Render3D)

   // Drops the preview parts, e.g. when the edited table is switched or the editor is closed
   void ClearPreview();

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
      string material; // Material items: the material name
      string label;
      ItemKind kind = ItemKind::Part;
      std::filesystem::path thumbFile;
      std::shared_ptr<Sampler> thumb;
      bool thumbLoaded = false; // Load attempted
   };

   static constexpr unsigned int kRigVersion = 1; // Bump when the thumbnail rig changes: every thumbnail is rendered again
   static constexpr int kThumbSize = 256;

   void RescanLibrary();
   void RenderGrid(float height);
   void PumpThumbnails(); // Renders the missing thumbnails, one at a time
   bool LoadScratch(const LibraryItem &item);
   void ReleaseScratch();
   void AbortThumbnail();
   static uint64_t PackKey(const std::filesystem::path &pack);
   static std::filesystem::path ThumbFolder(const std::filesystem::path &pack);
   void RenderLibraryFolder(const LibraryFolder &folder, const string &label);
   vector<std::filesystem::path> GetLibraryFolders() const;
   void SetLibraryFolders(const vector<std::filesystem::path> &folders);
   void AddLibraryFolder(const std::filesystem::path &folder);
   void RemoveLibraryFolder(size_t index);
   PackInfo ScanPack(const std::filesystem::path &path);
   void ImportPack(const std::filesystem::path &path);

   // Pack preview: the selected pack is loaded in a scratch table so that it never pollutes the
   // edited one (no outliner entry, no undo, not in the picking quadtree) until it is committed.
   // Its authored positions are overridden by UpdatePlacement (moved as a rigid group)
   void StartPlacement();
   void UpdatePlacement(const ImVec2 &mousePos);
   void SetupPlayerSide(IEditable *part, bool initRenderable);

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
   std::filesystem::path m_selectedPack;
   PackInfo m_selectedPackInfo;

   // Pack placement preview (see class comment): m_placementPack is the armed pack, the preview
   // table holds its loaded content (non group parts are tracked for translation & rendering)
   std::filesystem::path m_placementPack;
   CComObject<PinTable> *m_previewTable = nullptr;
   vector<IEditable *> m_previewParts;
   vector<std::shared_ptr<EditorUIPart>> m_previewUIParts;
   bool m_previewVisible = false; // Ghost currently rendered (mouse over the 3D view)
   Vertex2D m_previewPos;
   float m_previewZ = 0.f;
   vector<LibraryItem> m_items;
   std::filesystem::path m_filterFolder; // Tiles shown: the packs under this folder, all if empty

   // Thumbnail rendering: the queue of items to render, the pack keys to write once their last item is rendered
   vector<size_t> m_thumbQueue;
   std::map<std::filesystem::path, std::pair<string, int>> m_packKeys; // Pack -> (key, items left to render)
   enum class ThumbStage { Idle, Requested, ReadBack } m_thumbStage = ThumbStage::Idle;
   int m_thumbSlot = -1; // Renderer view slot held while thumbnails are rendered
   uint32_t m_thumbReadCount = 0;
   CComObject<PinTable> *m_scratch = nullptr; // Never saved
   vector<IEditable *> m_scratchParts;
   int m_lastShownFrame = -1;
   int m_thumbsRendered = 0, m_thumbsCached = 0, m_thumbsFailed = 0;
   bool m_hookActive = false; // VPX_LIBRARY_THUMBS test hook running
   std::filesystem::path m_hookFolder;
};

}
