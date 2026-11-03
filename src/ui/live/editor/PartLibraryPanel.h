// license:GPLv3+

#pragma once

#include "imgui/imgui.h"
#include "parts/pintable.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace VPX::EditorUI
{

class EditorUI;

// Part library, hosted by an editor area (Blender's editor type switcher): export of the current part selection to a partial VPZ pack, and import of
// parts from a specific VPZ file or from a browsable list of library folders holding VPZ
// packs organized in subfolders. The import merge strategy defines how conflicts between
// imported assets and same named table assets are resolved.
class PartLibraryPanel
{
public:
   explicit PartLibraryPanel(EditorUI &editor);

   void Show() { m_rescanLibrary = true; } // The editor was just switched to this type: rescan the library folders

   // Export of the current selection to a VPZ pack file, disabled when there is no exportable selection
   bool CanExportSelection() const;
   void ExportSelection();

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

   // Summary of the importable content of a VPZ pack
   struct PackInfo
   {
      bool valid = false;
      bool hasTable = false;
      int parts = 0;
      int collections = 0;
      int materials = 0;
      int images = 0;
      int sounds = 0;
      int fonts = 0;
   };

   void RescanLibrary();
   void RenderLibraryFolder(const LibraryFolder &folder, const string &label);
   vector<std::filesystem::path> GetLibraryFolders() const;
   void SetLibraryFolders(const vector<std::filesystem::path> &folders);
   void AddLibraryFolder(const std::filesystem::path &folder);
   void RemoveLibraryFolder(size_t index);
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
   std::filesystem::path m_selectedPack;
   PackInfo m_selectedPackInfo;
};

}
