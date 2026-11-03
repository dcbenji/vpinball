// license:GPLv3+

#include "core/stdafx.h"
#include "PartLibraryPanel.h"

#include "core/Settings.h"
#include "core/VPApp.h"
#include "core/SettingsService.h"
#include "core/VPApp.h"
#include "fonts/IconsForkAwesome.h"
#include "parts/ball.h"
#include "parts/pintable.h"
#include "parts/primitive.h"
#include "physics/PhysicsEngine.h"
#include "renderer/RenderDevice.h"
#include "ui/live/EditorUI.h"
#include "ui/live/editor/EditorUIPartRegistry.h"
#include "PropertyPane.h"
#include "EditorTheme.h"
#include "ui/live/LiveUI.h"
#include "ui/VPXFileFeedback.h"
#include "utils/JSONSerializer.h"

#include <miniz/miniz.h>
namespace
{

// Decompose a transform into the position/scale/rotation components used by the parts'
// SetTransform (same decomposition as EditorUI::SetSelectionTransform)
void ExtractTRS(Matrix3D transform, vec3 &pos, vec3 &scale, vec3 &rot)
{
   const Vertex3Ds right(transform._11, transform._12, transform._13);
   const Vertex3Ds up(transform._21, transform._22, transform._23);
   const Vertex3Ds dir(transform._31, transform._32, transform._33);
   scale.Set(max(right.Length(), 1e-8f), max(up.Length(), 1e-8f), max(dir.Length(), 1e-8f)); // Clamp to avoid division by zero

   transform._11 /= scale.x; // Normalize transform to evaluate rotation
   transform._12 /= scale.x;
   transform._13 /= scale.x;
   transform._21 /= scale.y;
   transform._22 /= scale.y;
   transform._23 /= scale.y;
   transform._31 /= scale.z;
   transform._32 /= scale.z;
   transform._33 /= scale.z;

   pos.Set(transform._41, transform._42, transform._43);

   // Derived from https://learnopencv.com/rotation-matrix-to-euler-angles/
   const float sy = sqrtf(transform._11 * transform._11 + transform._21 * transform._21);
   if (sy > 1e-6f)
   {
      rot.x = -RADTOANG(atan2f(transform._32, transform._33));
      rot.y = -RADTOANG(atan2f(-transform._31, sy));
      rot.z = -RADTOANG(atan2f(transform._21, transform._11));
   }
   else
   {
      rot.x = -RADTOANG(atan2f(transform._23, transform._22));
      rot.y = -RADTOANG(atan2f(-transform._31, sy));
      rot.z = 0.f;
   }
}

}

namespace VPX::EditorUI
{

// Full-width (or given width) button with a roomy frame
static bool PaddedButton(const char *label, const float width, const float dpi)
{
   ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f * dpi, 5.f * dpi));
   const bool pressed = ImGui::Button(label, ImVec2(width, 0.f));
   ImGui::PopStyleVar();
   return pressed;
}

PartLibraryPanel::PartLibraryPanel(EditorUI &editor)
   : m_editor(editor)
{
   if (const char *const folder = std::getenv("VPX_LIBRARY_THUMBS"); folder != nullptr && *folder != '\0')
   {
      m_hookFolder = PathFromUTF8(folder);
      m_hookActive = true;
   }
}

PartLibraryPanel::~PartLibraryPanel()
{
   ClearPreview();
   ReleaseScratch();
}


bool PartLibraryPanel::CanExportSelection() const
{
   // Only editable parts can be exported: drag points and light centers are edited inside their owning part
   return !m_editor.IsInspectMode() && !m_editor.m_table->IsLocked() && (m_editor.m_pointEditPart == nullptr) && !m_editor.m_multiSel.empty();
}

void PartLibraryPanel::ExportSelection()
{
   if (!CanExportSelection() || m_editor.m_player->m_playfieldWnd == nullptr)
      return;
   m_pendingExportPath = std::make_shared<string>();
   const std::filesystem::path defaultLocation
      = m_editor.m_table->m_filename.empty() ? PathFromString(m_editor.m_table->GetSettings().GetRecentDir_LoadDir()) : m_editor.m_table->m_filename.parent_path();
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   const SDL_DialogFileFilter filters[] = { { "Visual Pinball Packs", "vpz" } };
   SDL_ShowSaveFileDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingExportPath), //
      m_editor.m_player->m_playfieldWnd->GetCore(), filters, 1, location.empty() ? nullptr : location.c_str());
}

void PartLibraryPanel::Update()
{
   // Test hook: scan the given library folder and render its missing thumbnails without showing the area
   if (m_hookActive && m_editor.m_renderer != nullptr)
   {
      if (m_rescanLibrary)
      {
         m_rescanLibrary = false;
         RescanLibrary();
      }
      PumpThumbnails();
      if (m_thumbQueue.empty() && m_thumbStage == ThumbStage::Idle)
      {
         PLOGI << "LIBTHUMB rendered=" << m_thumbsRendered << " cached=" << m_thumbsCached << " failed=" << m_thumbsFailed;
         m_hookActive = false;
         m_hookFolder.clear();
         m_rescanLibrary = true; // The area, when shown, lists the user's folders
      }
   }
   // The area is no longer shown: stop rendering thumbnails and give the view slot back
   else if ((m_thumbSlot >= 0 || m_scratch != nullptr) && ImGui::GetFrameCount() > m_lastShownFrame + 1)
      AbortThumbnail();

   // Apply the file picked by the asynchronous 'Export' save dialog (deferred to the main thread)
   if (m_pendingExportPath && !m_pendingExportPath->empty())
   {
      std::filesystem::path file = PathFromUTF8(*m_pendingExportPath);
      m_pendingExportPath = nullptr;
      if (lowerCase(PathToUTF8(file.extension())) != ".vpz"s)
         file += ".vpz";
      vector<IEditable *> parts;
      for (const auto &part : m_editor.m_multiSel)
         parts.push_back(part->GetEditable());
      VPXFileFeedback feedback;
      m_editor.RestorePartsVisibility(); // Export the authored visibility, not the editor display state
      const HRESULT hr = m_editor.m_table->SavePartsToJSONPack(file, parts, feedback);
      if (SUCCEEDED(hr))
      {
         string exported;
         for (size_t i = 0; i < parts.size(); i++)
         {
            exported += (exported.empty() ? ""s : ", "s) + parts[i]->GetName();
            if (i == 4 && parts.size() > 5)
            {
               exported += ", ..."s;
               break;
            }
         }
         m_editor.m_liveUI.PushNotification("Exported "s + std::to_string(parts.size()) + " part(s) to '"s + PathToUTF8(file.filename()) + "': "s + exported, 10000);
      }
      else
         m_editor.m_liveUI.PushNotification("Failed to export selection to '"s + PathToUTF8(file) + '\'', 10000);
   }
   // Apply the file picked by the asynchronous 'Import' file dialog (deferred to the main thread)
   if (m_pendingImportPath && !m_pendingImportPath->empty())
   {
      m_selectedPack = PathFromUTF8(*m_pendingImportPath);
      m_pendingImportPath = nullptr;
      m_selectedPackInfo = ScanPack(m_selectedPack);
   }

   // Apply the folder picked by the asynchronous 'Add library folder' dialog (deferred to the main thread)
   if (m_pendingFolderPath && !m_pendingFolderPath->empty())
   {
      AddLibraryFolder(PathFromUTF8(*m_pendingFolderPath));
      m_pendingFolderPath = nullptr;
   }

   // Placement mode housekeeping: it is only available while the table is editable and the
   // playfield view is interactive (not in drag point edit mode, not hidden by the fly mode), and is
   // dropped while the part library area is hidden
   if (!m_editor.IsAreaEditorShown(EditorUI::AreaEditor::PartLibrary))
   {
      m_placementPack.clear();
      ClearPreview();
      return;
   }
   if (IsPlacementArmed() && (m_editor.IsInspectMode() || m_editor.m_table->IsLocked() || m_editor.m_pointEditPart != nullptr || m_editor.m_flyMode))
      CancelPlacement();
   if (IsPlacementArmed())
      UpdatePlacement(ImGui::GetIO().MousePos);
   else
      m_previewVisible = false;
}

void PartLibraryPanel::RenderInArea(const ImVec2 &pos, const ImVec2 &size)
{
   if (m_rescanLibrary && !m_hookActive)
   {
      m_rescanLibrary = false;
      RescanLibrary();
   }
   m_lastShownFrame = ImGui::GetFrameCount();
   if (!m_hookActive)
      PumpThumbnails();

   if (m_editor.BeginAreaWindow("PARTLIBRARY", pos, size))
   {
      // Header: editor type button and title
      m_editor.RenderAreaTypeButton(EditorUI::AreaEditor::PartLibrary);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("Part Library");
      ImGui::Separator();

      // Destination of the import, same as for newly added parts: the group of the current selection
      const PartGroup *const target = m_editor.GetPartGroupForNewPart();
      ImGui::Dummy(ImVec2(0.f, 2.f));
      ImGui::TextDisabled("Import into part group:");
      ImGui::SameLine();
      ImGui::TextUnformatted((target != nullptr) ? target->GetName().c_str() : "(table root)");
      if (m_editor.m_table->IsLocked())
         ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "The table is locked: import is disabled");

      PropertyPane sections(m_editor.m_table);
      const float dpi = m_editor.m_liveUI.GetDPI();
      if (sections.BeginSection("Import Options"s))
      {
         int strategy = static_cast<int>(m_mergeStrategy);
         ImGui::RadioButton("Rename on conflict when assets differ", &strategy, static_cast<int>(PartImportMergeStrategy::RenameConflict));
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Imported assets sharing the name of a different table asset are renamed\n"
                              "and the references of the imported parts are updated to the new names.\n"
                              "Identical assets are shared.");
         ImGui::RadioButton("Newer wins on name conflict", &strategy, static_cast<int>(PartImportMergeStrategy::ExistingWins));
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On a name conflict, the imported parts reuse the asset already present in the table.");
         m_mergeStrategy = static_cast<PartImportMergeStrategy>(strategy);
         sections.EndSection();
      }

      if (sections.BeginSection("Import a Pack File"s))
      {
      if (PaddedButton(ICON_FK_FILE_O "  Select VPZ File...", ImGui::GetContentRegionAvail().x, dpi))
      {
         if (m_editor.m_player->m_playfieldWnd != nullptr)
         {
            m_pendingImportPath = std::make_shared<string>();
            const std::filesystem::path defaultLocation = PathFromString(m_editor.m_table->GetSettings().GetRecentDir_LoadDir());
            const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
            const SDL_DialogFileFilter filters[] = { { "Visual Pinball Packs", "vpz" } };
            SDL_ShowOpenFileDialog(
               [](void *userdata, const char *const *filelist, int filter)
               {
                  auto *res = static_cast<std::shared_ptr<string> *>(userdata);
                  if (filelist != nullptr && filelist[0] != nullptr)
                     **res = filelist[0];
                  delete res;
               },
               new std::shared_ptr<string>(m_pendingImportPath), //
               m_editor.m_player->m_playfieldWnd->GetCore(), filters, 1, location.empty() ? nullptr : location.c_str(), false);
         }
      }

      sections.EndSection();
      }

      if (sections.BeginSection("Library Folders"s))
      {
      const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
      if (PaddedButton(ICON_FK_FOLDER_OPEN "  Add Folder...", halfW, dpi))
      {
         if (m_editor.m_player->m_playfieldWnd != nullptr)
         {
            m_pendingFolderPath = std::make_shared<string>();
            const std::filesystem::path defaultLocation = PathFromString(m_editor.m_table->GetSettings().GetRecentDir_LoadDir());
            const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
            SDL_ShowOpenFolderDialog(
               [](void *userdata, const char *const *filelist, int filter)
               {
                  auto *res = static_cast<std::shared_ptr<string> *>(userdata);
                  if (filelist != nullptr && filelist[0] != nullptr)
                     **res = filelist[0];
                  delete res;
               },
               new std::shared_ptr<string>(m_pendingFolderPath), //
               m_editor.m_player->m_playfieldWnd->GetCore(), location.empty() ? nullptr : location.c_str(), false);
         }
      }
      ImGui::SameLine();
      if (PaddedButton(ICON_FK_REFRESH "  Refresh", ImGui::GetContentRegionAvail().x, dpi))
         m_rescanLibrary = true;

      // Folder tree, collapsible, as a filter of the tiles
      if (ImGui::TreeNodeEx("Folders", ImGuiTreeNodeFlags_SpanAvailWidth))
      {
         if (m_library.empty())
            ImGui::TextDisabled("No library folder defined");
         if (!m_library.empty() && ImGui::Selectable("(all)", m_filterFolder.empty()))
            m_filterFolder.clear();
         for (size_t i = 0; i < m_library.size(); i++)
         {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(ICON_FK_TRASH_O))
            {
               RemoveLibraryFolder(i);
               ImGui::PopID();
               break;
            }
            if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Remove this library folder");
            ImGui::SameLine();
            const LibraryFolder &root = m_library[i];
            const string label = root.path.filename().empty() ? PathToUTF8(root.path) : PathToUTF8(root.path.filename());
            RenderLibraryFolder(root, label);
            ImGui::PopID();
         }
         ImGui::TreePop();
      }

      const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 5.f + 40.f * dpi;
      if (ImGui::BeginChild("##LibraryGrid", ImVec2(0.f, -footerHeight), ImGuiChildFlags_Borders))
         RenderGrid(ImGui::GetContentRegionAvail().y);
      ImGui::EndChild();
      sections.EndSection();
      }

      // Selected pack preview and import action
      if (sections.BeginSection("Import"s))
      {
      if (m_selectedPack.empty())
         ImGui::TextDisabled("Select a VPZ pack file to import");
      else
      {
         ImGui::TextUnformatted(PathToUTF8(m_selectedPack.filename()).c_str());
         if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", PathToUTF8(m_selectedPack).c_str());
         if (!m_selectedPackInfo.valid)
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Failed to read the pack");
         else
            ImGui::Text("Content: %d part(s), %d collection(s), %d material(s), %d image(s), %d sound(s), %d font(s)%s", m_selectedPackInfo.parts, m_selectedPackInfo.collections,
               m_selectedPackInfo.materials, m_selectedPackInfo.images, m_selectedPackInfo.sounds, m_selectedPackInfo.fonts, m_selectedPackInfo.hasTable ? " + table" : "");
         ImGui::BeginDisabled(m_editor.m_table->IsLocked() || !m_selectedPackInfo.valid);
         if (PaddedButton(ICON_FK_DOWNLOAD "  Import", ImGui::GetContentRegionAvail().x, dpi))
            ImportPack(m_selectedPack);
         ImGui::EndDisabled();
      }
      sections.EndSection();
      }
   }
   m_editor.EndAreaWindow();
}
void PartLibraryPanel::RenderLibraryFolder(const LibraryFolder &folder, const string &label)
{
   // A folder node: clicking it filters the tiles to its packs (subfolders included)
   const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | (folder.subFolders.empty() ? ImGuiTreeNodeFlags_Leaf : 0)
         | (folder.path == m_filterFolder ? ImGuiTreeNodeFlags_Selected : 0));
   if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
      m_filterFolder = folder.path;
   if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", PathToUTF8(folder.path).c_str());
   if (open)
   {
      for (const LibraryFolder &sub : folder.subFolders)
         RenderLibraryFolder(sub, PathToUTF8(sub.path.filename()));
      ImGui::TreePop();
   }
}

void PartLibraryPanel::RenderGrid(const float height)
{
   const float dpi = m_editor.m_liveUI.GetDPI();
   const float tile = 96.f * dpi, labelH = ImGui::GetTextLineHeight() + 4.f * dpi;
   const ImGuiStyle &style = ImGui::GetStyle();
   const int columns = max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (tile + style.ItemSpacing.x)));
   const string filter = PathToUTF8(m_filterFolder);
   ImDrawList *const drawList = ImGui::GetWindowDrawList();
   int shown = 0, loads = 0;
   for (size_t i = 0; i < m_items.size(); i++)
   {
      LibraryItem &item = m_items[i];
      if (!filter.empty() && PathToUTF8(item.pack).rfind(filter, 0) != 0)
         continue;
      if (shown % columns != 0)
         ImGui::SameLine();
      shown++;
      ImGui::PushID(static_cast<int>(i));
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("##tile", ImVec2(tile, tile + labelH));
      const bool hovered = ImGui::IsItemHovered(), visible = ImGui::IsItemVisible();
      if (ImGui::IsItemClicked())
      {
         m_selectedPack = item.pack;
         m_selectedPackInfo = ScanPack(item.pack);
      }
      if (hovered)
         ImGui::SetTooltip("%s%s%s", PathToUTF8(item.pack).c_str(), item.material.empty() ? "" : "\nMaterial: ", item.material.c_str());
      if (visible)
      {
         // Thumbnails are loaded when their tile is first shown, a few per frame
         if (!item.thumbLoaded && loads < 4)
         {
            item.thumbLoaded = true;
            std::error_code ec;
            if (std::filesystem::exists(item.thumbFile, ec))
            {
               loads++;
               std::shared_ptr<const BaseTexture> tex = BaseTexture::CreateFromFile(item.thumbFile);
               item.thumb = tex ? std::make_shared<Sampler>(m_editor.m_renderer->m_renderDevice, "Library Thumbnail"s, tex, false) : nullptr;
            }
         }
         const ImVec2 p1(p0.x + tile, p0.y + tile);
         const bool selected = item.pack == m_selectedPack;
         drawList->AddRectFilled(p0, p1, IM_COL32(46, 46, 46, 255));
         if (item.thumb)
            drawList->AddImage(item.thumb, p0, p1);
         drawList->AddRect(p0, ImVec2(p1.x, p1.y + labelH), selected ? EditorTheme::Accent : hovered ? IM_COL32(120, 120, 120, 255) : IM_COL32(0, 0, 0, 0), 0.f, 0, selected ? 2.f : 1.f);
         static constexpr const char *kBadges[] = { "PART", "MAT", "INSERT" };
         static constexpr ImU32 kBadgeColors[] = { IM_COL32(70, 110, 160, 230), IM_COL32(150, 100, 50, 230), IM_COL32(150, 60, 120, 230) };
         const char *const badge = kBadges[static_cast<int>(item.kind)];
         const ImVec2 bs = ImGui::CalcTextSize(badge);
         drawList->AddRectFilled(ImVec2(p0.x + 2.f * dpi, p0.y + 2.f * dpi), ImVec2(p0.x + 6.f * dpi + bs.x, p0.y + 4.f * dpi + bs.y), kBadgeColors[static_cast<int>(item.kind)], 2.f * dpi);
         drawList->AddText(ImVec2(p0.x + 4.f * dpi, p0.y + 3.f * dpi), IM_COL32(255, 255, 255, 255), badge);
         const ImVec4 clip(p0.x, p1.y, p1.x, p1.y + labelH);
         drawList->AddText(nullptr, 0.f, ImVec2(p0.x + 2.f * dpi, p1.y + 2.f * dpi), ImGui::GetColorU32(ImGuiCol_Text), item.label.c_str(), nullptr, 0.f, &clip);
      }
      ImGui::PopID();
   }
   if (shown == 0)
   {
      const char *const msg = m_library.empty() ? "No library folder defined" : "No pack found";
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
      ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + max(0.f, (avail.x - ImGui::CalcTextSize(msg).x) * 0.5f), ImGui::GetCursorPosY() + max(0.f, height * 0.4f - ImGui::GetTextLineHeight())));
      ImGui::TextUnformatted(msg);
      ImGui::PopStyleColor();
   }
}

vector<std::filesystem::path> PartLibraryPanel::GetLibraryFolders() const
{
   // The shipped 'assets/library' folder is the default part library folder: it is created when missing
   // (best effort, the install folder may be read only) and prepended to the user folders unless the user
   // removed it, which is persisted as an exclusion entry prefixed with '-' in the stored folder list
   const std::filesystem::path defaultFolder = g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets) / "library";
   std::error_code ec;
   std::filesystem::create_directories(defaultFolder, ec);

   vector<std::filesystem::path> folders;
   if (!m_hookFolder.empty())
   {
      folders.push_back(m_hookFolder);
      return folders;
   }
   ankerl::unordered_dense::set<string> excluded;
   const string stored = g_settingsService.GetAppSettings().GetEditor_PartLibraryFolders();
   size_t start = 0;
   while (start <= stored.size())
   {
      const size_t end = stored.find(';', start);
      const string entry = stored.substr(start, end == string::npos ? string::npos : end - start);
      if (!entry.empty())
      {
         if (entry[0] == '-')
            excluded.insert(lowerCase(PathToUTF8(PathFromString(entry.substr(1)).lexically_normal())));
         else
            folders.push_back(PathFromString(entry));
      }
      if (end == string::npos)
         break;
      start = end + 1;
   }

   const string defaultKey = lowerCase(PathToUTF8(defaultFolder.lexically_normal()));
   const bool listed = std::ranges::find_if(folders, [&defaultKey](const std::filesystem::path &f) { return lowerCase(PathToUTF8(f.lexically_normal())) == defaultKey; }) != folders.end();
   if (!listed && !excluded.contains(defaultKey))
      folders.insert(folders.begin(), defaultFolder.lexically_normal());
   return folders;
}

void PartLibraryPanel::SetLibraryFolders(const vector<std::filesystem::path> &folders)
{
   // Preserve the exclusion entries (prefixed with '-') of the stored list
   string stored;
   const string previous = g_settingsService.GetAppSettings().GetEditor_PartLibraryFolders();
   size_t start = 0;
   while (start <= previous.size())
   {
      const size_t end = previous.find(';', start);
      const string entry = previous.substr(start, end == string::npos ? string::npos : end - start);
      if (entry.starts_with('-'))
         stored += (stored.empty() ? ""s : ";"s) + entry;
      if (end == string::npos)
         break;
      start = end + 1;
   }
   for (const std::filesystem::path &folder : folders)
      stored += (stored.empty() ? ""s : ";"s) + PathToString(folder);
   g_settingsService.GetAppSettings().SetEditor_PartLibraryFolders(stored, false);
   m_rescanLibrary = true;
}

void PartLibraryPanel::AddLibraryFolder(const std::filesystem::path &folder)
{
   vector<std::filesystem::path> folders = GetLibraryFolders();
   const std::filesystem::path normalized = folder.lexically_normal();
   for (const std::filesystem::path &existing : folders)
      if (StrCompareNoCase(PathToUTF8(existing.lexically_normal()), PathToUTF8(normalized)))
         return;
   folders.push_back(normalized);
   SetLibraryFolders(folders);
}

void PartLibraryPanel::RemoveLibraryFolder(size_t index)
{
   vector<std::filesystem::path> folders = GetLibraryFolders();
   if (index >= folders.size())
      return;
   const std::filesystem::path removed = folders[index];
   folders.erase(folders.begin() + static_cast<ptrdiff_t>(index));
   // Removing the default library folder is persisted as an exclusion entry so it does not come back
   const std::filesystem::path defaultFolder = g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets) / "library";
   string stored;
   if (StrCompareNoCase(PathToUTF8(removed.lexically_normal()), PathToUTF8(defaultFolder.lexically_normal())))
      stored = "-"s + PathToString(defaultFolder.lexically_normal());
   for (const std::filesystem::path &folder : folders)
      stored += (stored.empty() ? ""s : ";"s) + PathToString(folder);
   g_settingsService.GetAppSettings().SetEditor_PartLibraryFolders(stored, false);
   m_rescanLibrary = true;
}

void PartLibraryPanel::RescanLibrary()
{
   AbortThumbnail();
   m_library.clear();
   m_items.clear();
   m_thumbQueue.clear();
   m_packKeys.clear();
   m_thumbsRendered = m_thumbsCached = m_thumbsFailed = 0;
   const std::function<void(LibraryFolder &)> scanFolder = [&scanFolder](LibraryFolder &node)
   {
      std::error_code ec;
      vector<std::filesystem::path> subdirs;
      for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(node.path, ec))
      {
         if (entry.is_directory(ec))
         {
            // A folder holding a manifest is a folder pack; thumbnail sidecars of zip packs and the built-in preview packs are not browsed
            const string name = PathToUTF8(entry.path().filename());
            if (std::filesystem::exists(entry.path() / "manifest.json", ec))
               node.packs.push_back(entry.path());
            else if (lowerCase(PathToUTF8(entry.path().extension())) != ".thumbs"s && name != "_preview"s)
               subdirs.push_back(entry.path());
         }
         else if (entry.is_regular_file(ec) && lowerCase(PathToUTF8(entry.path().extension())) == ".vpz"s)
            node.packs.push_back(entry.path());
      }
      std::ranges::sort(subdirs);
      std::ranges::sort(node.packs);
      for (const std::filesystem::path &subdir : subdirs)
      {
         node.subFolders.emplace_back().path = subdir;
         scanFolder(node.subFolders.back());
      }
   };
   // Tiles in tree order: a pack's kind, and its thumbnails' state (cached when the pack key matches and every thumbnail exists)
   const std::function<void(const LibraryFolder &)> addItems = [this, &addItems](const LibraryFolder &node)
   {
      for (const std::filesystem::path &pack : node.packs)
      {
         const PackInfo info = ScanPack(pack);
         if (!info.valid)
            continue;
         const std::filesystem::path thumbs = ThumbFolder(pack);
         const size_t first = m_items.size();
         const auto addItem = [&](const string &label, const string &material, const ItemKind kind)
         {
            LibraryItem &item = m_items.emplace_back();
            item.pack = pack;
            item.material = material;
            item.label = label;
            item.kind = kind;
            item.thumbFile = thumbs / (JSONSerializer::SanitizeFileName(label) + ".png"s);
         };
         if (info.kind == ItemKind::Material)
            for (const string &material : info.materialNames)
               addItem(material, material, ItemKind::Material);
         else
            addItem(PathToUTF8(pack.stem()), ""s, info.kind);
         const string key = std::format("{:016x}", PackKey(pack));
         string stored;
         std::ifstream keyFile(thumbs / "key.txt");
         std::getline(keyFile, stored);
         bool cached = stored == key;
         std::error_code ec;
         for (size_t i = first; i < m_items.size() && cached; i++)
            cached = std::filesystem::exists(m_items[i].thumbFile, ec);
         if (cached)
            m_thumbsCached += static_cast<int>(m_items.size() - first);
         else
         {
            m_packKeys[pack] = { key, static_cast<int>(m_items.size() - first) };
            for (size_t i = first; i < m_items.size(); i++)
               m_thumbQueue.push_back(i);
         }
      }
      for (const LibraryFolder &sub : node.subFolders)
         addItems(sub);
   };
   for (const std::filesystem::path &folder : GetLibraryFolders())
   {
      std::error_code ec;
      if (!std::filesystem::is_directory(folder, ec))
         continue;
      m_library.emplace_back().path = folder;
      scanFolder(m_library.back());
      addItems(m_library.back());
   }
   std::ranges::reverse(m_thumbQueue); // Rendered from the back: in tile order
}

PartLibraryPanel::PackInfo PartLibraryPanel::ScanPack(const std::filesystem::path &path)
{
   PackInfo info;
   const auto pack = JSONSerializer::CreateReader(path);
   if (!pack || !pack->Exists("manifest.json"s))
      return info;
   info.valid = true;
   bool insert = false;
   for (const std::filesystem::path &file : pack->ListFiles())
   {
      const std::filesystem::path folder = file.parent_path();
      if (file == "table.json"s)
         info.hasTable = true;
      else if (file.extension() == ".json" && folder == "parts"s)
      {
         info.parts++;
         // A traced insert card (Primitive::IsTracedInsert): a primitive with add_blend and a TRACED_ image
         string content;
         if (!insert && pack->ReadTextFile(file, content))
            try
            {
               const nlohmann::json doc = nlohmann::json::parse(content);
               insert = doc.value("$type", ""s) == JSONSerializer::GetPartTypeName(eItemPrimitive) && doc.value("add_blend", false) && lowerCase(doc.value("image", ""s)).starts_with("traced_");
            }
            catch (const nlohmann::json::exception &) { }
      }
      else if (file.extension() == ".json" && folder == "collections"s)
         info.collections++;
      else if (file.extension() == ".json" && folder == "materials"s)
      {
         info.materials++;
         string content, name = PathToUTF8(file.stem());
         if (pack->ReadTextFile(file, content))
            try
            {
               name = nlohmann::json::parse(content).value("name", name);
            }
            catch (const nlohmann::json::exception &) { }
         info.materialNames.push_back(name);
      }
      else if (file.extension() == ".json" && folder == "images"s)
         info.images++;
      else if (file.extension() == ".json" && folder == "sounds"s)
         info.sounds++;
      else if (file.extension() == ".json" && folder == "fonts"s)
         info.fonts++;
   }
   info.kind = info.parts > 0 ? (insert ? ItemKind::Insert : ItemKind::Part) : info.materials > 0 ? ItemKind::Material : ItemKind::Part;
   return info;
}

std::filesystem::path PartLibraryPanel::ThumbFolder(const std::filesystem::path &pack)
{
   // Inside a folder pack (its own git can commit them); beside a zip pack, which is never rewritten
   std::error_code ec;
   if (std::filesystem::is_directory(pack, ec))
      return pack / "thumbs";
   std::filesystem::path sidecar = pack;
   sidecar += ".thumbs";
   return sidecar;
}

// 64 bit FNV-1a over the pack content (paths, JSON bytes, binary sizes; zip: entry names and CRC32, nothing decompressed), the rig version and the thumbnail size
uint64_t PartLibraryPanel::PackKey(const std::filesystem::path &pack)
{
   uint64_t h = 0xcbf29ce484222325ull;
   const auto hash = [&h](const void *data, const size_t size)
   {
      for (size_t i = 0; i < size; i++)
         h = (h ^ static_cast<const uint8_t *>(data)[i]) * 0x100000001b3ull;
   };
   const uint32_t rig[2] = { kRigVersion, static_cast<uint32_t>(kThumbSize) };
   hash(rig, sizeof(rig));
   std::error_code ec;
   if (std::filesystem::is_directory(pack, ec))
   {
      const auto reader = JSONSerializer::CreateReader(pack);
      for (const std::filesystem::path &file : reader->ListFiles())
      {
         const string name = file.generic_string();
         if (name.starts_with("thumbs/") || name.starts_with('.')) // The thumbnails themselves, VCS/OS files
            continue;
         hash(name.data(), name.size());
         if (file.extension() == ".json")
         {
            vector<uint8_t> data;
            if (reader->ReadBinaryFile(file, data))
               hash(data.data(), data.size());
         }
         else
         {
            const uint64_t size = std::filesystem::file_size(pack / file, ec);
            hash(&size, sizeof(size));
         }
      }
   }
   else
   {
      mz_zip_archive zip {};
      if (mz_zip_reader_init_file(&zip, PathToUTF8(pack).c_str(), 0))
      {
         for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); i++)
         {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&zip, i, &stat))
               continue;
            hash(stat.m_filename, strlen(stat.m_filename));
            hash(&stat.m_crc32, sizeof(stat.m_crc32));
         }
         mz_zip_reader_end(&zip);
      }
   }
   return h;
}

// Loads the item into a scratch table (never saved, the live table is not consulted) and sets its parts up with the live renderer, as the
// placement ghost does. Material items: the built-in preview sphere wearing the material
bool PartLibraryPanel::LoadScratch(const LibraryItem &item)
{
   ReleaseScratch();
   const auto load = [](const std::filesystem::path &path) -> CComObject<PinTable> *
   {
      CComObject<PinTable> *table;
      CComObject<PinTable>::CreateInstance(&table);
      table->AddRef();
      VPXFileFeedback feedback;
      const auto pack = JSONSerializer::CreateReader(path);
      const HRESULT hr = pack != nullptr ? table->LoadGameFromJSONPack(*pack, feedback) : E_FAIL;
      if (FAILED(hr) && (hr != APPX_E_BLOCK_HASH_INVALID) && (hr != APPX_E_CORRUPT_CONTENT))
      {
         table->Release();
         return nullptr;
      }
      return table;
   };
   m_scratch = load(item.pack);
   if (m_scratch == nullptr)
      return false;
   if (item.kind == ItemKind::Material)
   {
      CComObject<PinTable> *const sphere = load(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets) / "library" / "_preview" / "sphere.vpz");
      if (sphere == nullptr)
         return false;
      const vector<IEditable *> parts = m_scratch->ImportParts(sphere, nullptr);
      sphere->Release();
      for (IEditable *const part : parts)
         if (part->GetItemType() == eItemPrimitive)
            static_cast<Primitive *>(part)->m_d.m_szMaterial = item.material;
   }
   // ponytail: the scratch table's render probes are dropped (never set up with the renderer), probe reflection/refraction is not previewed
   for (RenderProbe *const probe : m_scratch->m_vrenderprobe)
      delete probe;
   m_scratch->m_vrenderprobe.clear();
   for (IEditable *const part : m_scratch->GetParts())
   {
      if (part->GetIRenderable() == nullptr)
         continue;
      part->GetIRenderable()->RenderSetup(m_editor.m_renderer.get());
      if (part->GetItemType() != eItemPartGroup)
         m_scratchParts.push_back(part);
   }
   return !m_scratchParts.empty();
}

void PartLibraryPanel::ReleaseScratch()
{
   if (m_editor.m_renderer != nullptr)
      m_editor.m_renderer->CancelIsolatedEditorView(&m_scratchParts);
   m_scratchParts.clear();
   if (m_scratch == nullptr)
      return;
   if (m_editor.m_renderer != nullptr)
      for (IEditable *const edit : m_scratch->GetParts())
      {
         if (edit->GetIRenderable() == nullptr)
            continue;
         // Same deferred release as part deletion: the render thread may still use the render state
         m_editor.m_renderer->DropRenderable(edit->GetIRenderable());
         edit->AddRef();
         edit->m_renderReleasePending = true;
         m_editor.m_renderer->m_renderDevice->AddEndOfFrameCmd(
            [edit]()
            {
               edit->GetIRenderable()->RenderRelease();
               edit->m_renderReleasePending = false;
               edit->Release();
            });
      }
   // The table's images are deleted with it: drop them from the texture manager (recorded draws keep their samplers alive)
   if (m_editor.m_renderer != nullptr)
      for (Texture *const image : m_scratch->GetImageList())
         m_editor.m_renderer->m_renderDevice->m_texMan.UnloadTexture(image);
   m_scratch->Release();
   m_scratch = nullptr;
}

void PartLibraryPanel::AbortThumbnail()
{
   // An item in flight is queued again (still at the back of the queue)
   m_thumbStage = ThumbStage::Idle;
   ReleaseScratch();
   if (m_thumbSlot >= 0 && m_editor.m_renderer != nullptr && m_editor.m_cameraPreviews[m_thumbSlot].GetLeaf() < 0)
      m_editor.m_renderer->ReleaseEditorView(m_thumbSlot);
   m_thumbSlot = -1;
}

void PartLibraryPanel::PumpThumbnails()
{
#if !defined(ENABLE_BGFX)
   m_thumbQueue.clear(); // ponytail: the read back (RenderDevice::RequestVRScreenshot) is BGFX only
#else
   Renderer *const renderer = m_editor.m_renderer.get();
   if (renderer == nullptr || (m_thumbQueue.empty() && m_thumbStage == ThumbStage::Idle && m_thumbSlot < 0))
      return;
   // A Camera Preview area took the slot over: start again with another one
   if (m_thumbSlot >= 0 && m_editor.m_cameraPreviews[m_thumbSlot].GetLeaf() >= 0)
   {
      m_thumbSlot = -1;
      AbortThumbnail();
   }
   switch (m_thumbStage)
   {
   case ThumbStage::Idle:
   {
      if (m_thumbQueue.empty())
      {
         AbortThumbnail(); // Done: give the slot back
         return;
      }
      if (g_pplayer->m_vrDevice != nullptr || renderer->m_stereo3D != STEREO_OFF)
         return; // Secondary views are only rendered on desktop
      // A view slot no Camera Preview area uses (the last ones first), kept until the queue is empty
      for (int i = AreaLayout::kMaxViewLeaves - 1; i >= 0 && m_thumbSlot < 0; i--)
         if (m_editor.m_cameraPreviews[i].GetLeaf() < 0 && renderer->CanRenderEditorPBRView(i))
            m_thumbSlot = i;
      if (m_thumbSlot < 0)
         return;
      const LibraryItem &item = m_items[m_thumbQueue.back()];
      if (!LoadScratch(item))
      {
         ReleaseScratch();
         PLOGW << "Library thumbnail: nothing to render in '" << PathToUTF8(item.pack) << '\'';
         m_thumbsFailed++;
         m_thumbQueue.pop_back();
         return;
      }
      // Fixed 3/4 camera framed on the item bounds: primitives by their mesh bounds, other parts by their position +/- 100 VPU
      FRect3D bounds(FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
      for (IEditable *const part : m_scratchParts)
      {
         Vertex3Ds bMin, bMax;
#if defined(ENABLE_BGFX)
         if (part->GetItemType() != eItemPrimitive || !static_cast<Primitive *>(part)->GetShadowCacheBounds(bMin, bMax))
#endif
         {
            const Vertex2D c = part->GetCenter();
            bMin = Vertex3Ds(c.x - 100.f, c.y - 100.f, 0.f);
            bMax = Vertex3Ds(c.x + 100.f, c.y + 100.f, 100.f);
         }
         bounds.Extend(FRect3D(bMin.x, bMax.x, bMin.y, bMax.y, bMin.z, bMax.z));
      }
      AreaViewSettings cam;
      cam.yaw = 35.f;
      cam.pitch = 30.f;
      CameraPreviewPanel::FrameBounds(cam, bounds, m_editor.m_navFovY, 1.f);
      Renderer::EditorViewRequest req;
      req.width = req.height = kThumbSize;
      req.playerCamera = false;
      CameraPreviewPanel::ComputeOrbit(cam, m_editor.m_navFovY, 1.f, req.view, req.proj);
      req.isolated = &m_scratchParts;
      req.isolatedTable = m_scratch;
      if (renderer->RequestEditorView(m_thumbSlot, req))
         m_thumbStage = ThumbStage::Requested;
      else
         ReleaseScratch(); // Retried on a later frame
      break;
   }

   case ThumbStage::Requested:
   {
      // Once a scene render recorded the request (this frame's): read the view back at the end of this frame (render thread), then save it
      if (renderer->IsEditorViewPending(m_thumbSlot))
         return;
      RenderTarget *const rt = renderer->GetEditorView(m_thumbSlot);
      if (rt == nullptr)
      {
         AbortThumbnail();
         return;
      }
      const LibraryItem &item = m_items[m_thumbQueue.back()];
      std::error_code ec;
      std::filesystem::create_directories(item.thumbFile.parent_path(), ec);
      RenderDevice *const rd = renderer->m_renderDevice;
      m_thumbReadCount = rd->m_directScreenshotCount;
      rd->AddEndOfFrameCmd([rd, rt, file = item.thumbFile]() { rd->RequestVRScreenshot(rt, file, true); });
      ReleaseScratch();
      m_thumbStage = ThumbStage::ReadBack;
      break;
   }

   case ThumbStage::ReadBack:
   {
      RenderDevice *const rd = renderer->m_renderDevice;
      if (rd->m_directScreenshotCount == m_thumbReadCount)
         return;
      LibraryItem &item = m_items[m_thumbQueue.back()];
      m_thumbQueue.pop_back();
      m_thumbStage = ThumbStage::Idle;
      item.thumb = nullptr;
      item.thumbLoaded = false;
      std::error_code ec;
      if (!std::filesystem::exists(item.thumbFile, ec))
      {
         m_thumbsFailed++;
         return;
      }
      m_thumbsRendered++;
      // The pack key is written once all its thumbnails are rendered
      if (auto it = m_packKeys.find(item.pack); it != m_packKeys.end() && --it->second.second == 0)
      {
         std::ofstream(ThumbFolder(item.pack) / "key.txt", std::ios::trunc) << it->second.first << '\n';
         m_packKeys.erase(it);
      }
      break;
   }
   }
#endif
}

// Imports the parts and referenced assets of a VPZ pack into the edited table, merging them
// with the existing content: assets are shared by name, part and collection names are made unique
void PartLibraryPanel::ImportPack(const std::filesystem::path &path)
{
   if (m_editor.m_table->IsLocked() || m_editor.IsInspectMode())
      return;

   CComObject<PinTable> *temp;
   CComObject<PinTable>::CreateInstance(&temp);
   temp->AddRef();
   VPXFileFeedback feedback;
   // The merge strategy may rename the assets of the pack on load (conflicting and not equal ones)
   const std::unique_ptr<JSONSerializer::Deserializer> pack = m_editor.m_table->CreateImportDeserializer(path, m_mergeStrategy);
   const HRESULT hr = pack != nullptr ? temp->LoadGameFromJSONPack(*pack, feedback) : E_FAIL;
   // Same as the table load: hash/corrupt content errors still keep the loaded parts
   if (FAILED(hr) && (hr != APPX_E_BLOCK_HASH_INVALID) && (hr != APPX_E_CORRUPT_CONTENT))
   {
      temp->Release();
      m_editor.m_liveUI.PushNotification("Failed to import '"s + PathToUTF8(path) + '\'', 10000);
      return;
   }

   PinTable *const table = m_editor.m_table;
   PartGroup *const target = m_editor.GetPartGroupForNewPart();

   // Move the parts, collections and assets of the pack to the edited table (merged by name)
   m_editor.m_undo.BeginUndo();
   const vector<IEditable *> imported = table->ImportParts(temp, target);
   temp->Release();
   for (IEditable *const part : imported)
   {
      part->AddRef(); // Created parts keep a creation reference, released by the undo record (see PinTable::Uncreate)
      m_editor.m_undo.MarkForCreate(part);
   }

   // Player side setup, same as part creation
   for (IEditable *const part : imported)
      SetupPlayerSide(part, true);
   m_editor.m_undo.EndUndo();

   // Select the imported parts
   m_editor.UpdateEditableList();
   m_editor.m_multiSel.clear();
   for (IEditable *const part : imported)
      if (const auto it = m_editor.m_editableMap.find(part); it != m_editor.m_editableMap.end())
         m_editor.m_multiSel.push_back(it->second);
   if (!m_editor.m_multiSel.empty())
      m_editor.m_selection = Selection(m_editor.m_multiSel.back());

   m_editor.m_liveUI.PushNotification(
      "Imported "s + std::to_string(imported.size()) + " part(s) from '"s + PathToUTF8(path.filename()) + "' into '"s + (target != nullptr ? target->GetName() : "(table root)"s) + '\'',
      10000);
}

void PartLibraryPanel::SetupPlayerSide(IEditable *part, const bool initRenderable)
{
   if (part->GetItemType() == eItemBall)
      m_editor.m_player->m_vball.push_back(static_cast<Ball *>(part));
   m_editor.m_player->TimerSetup(part);
   if (initRenderable)
   {
      if (auto *const renderable = part->GetIRenderable(); renderable)
         renderable->RenderSetup(m_editor.m_renderer.get());
   }
   else
      m_editor.m_renderer->ReinitRenderable(part->GetIRenderable());
   if (part->GetIHitable())
      m_editor.m_player->m_physics->Add(part);
}

// Pack placement: the selected pack is loaded in a scratch table so that it never pollutes the
// edited one until it is committed. Its parts are kept invisible and only rendered as ghosts by
// Render3D while the mouse hovers the 3D playfield view, positioned by UpdatePlacement.
void PartLibraryPanel::StartPlacement()
{
   ClearPreview();
   if (m_placementPack.empty() || m_editor.m_table->IsLocked() || m_editor.IsInspectMode())
      return;

   CComObject<PinTable> *temp;
   CComObject<PinTable>::CreateInstance(&temp);
   temp->AddRef();
   VPXFileFeedback feedback;
   // The merge strategy is applied now so that the previewed parts already carry the asset names they will get when committed
   const std::unique_ptr<JSONSerializer::Deserializer> pack = m_editor.m_table->CreateImportDeserializer(m_placementPack, m_mergeStrategy);
   const HRESULT hr = pack != nullptr ? temp->LoadGameFromJSONPack(*pack, feedback) : E_FAIL;
   if ((FAILED(hr) && (hr != APPX_E_BLOCK_HASH_INVALID) && (hr != APPX_E_CORRUPT_CONTENT)) || temp->GetParts().empty())
   {
      temp->Release();
      m_editor.m_liveUI.PushNotification("Failed to load '"s + PathToUTF8(m_placementPack) + '\'', 10000);
      return;
   }

   m_previewTable = temp;
   for (IEditable *const part : temp->GetParts())
   {
      if (part->GetItemType() == eItemPartGroup)
         continue;
      m_previewParts.push_back(part);
      std::shared_ptr<EditorUIPart> uiPart = EditorUIPartRegistry::Create(part);
      m_previewUIParts.push_back(uiPart != nullptr ? uiPart : std::make_shared<BaseUIPart>(part));
      if (auto *const renderable = part->GetIRenderable(); renderable)
         renderable->RenderSetup(m_editor.m_renderer.get());
   }
   m_previewPos = Vertex2D(FLT_MAX, FLT_MAX); // Force the first placement update
   m_previewZ = FLT_MAX;
}

void PartLibraryPanel::CancelPlacement()
{
   m_placementPack.clear();
   ClearPreview();
}

void PartLibraryPanel::UpdatePlacement(const ImVec2 &mousePos)
{
   const ImGuiIO &io = ImGui::GetIO();
   const bool hovering = !io.WantCaptureMouse && m_editor.m_camMode != ViewMode::DesktopBackdrop && m_editor.m_fileOp == nullptr && m_editor.m_player->m_playfieldWnd != nullptr;
   if (!hovering)
   {
      // Re-hide the ghost parts when the mouse leaves the 3D view (or the view is not 3D)
      m_previewVisible = false;
      return;
   }

   // Project the pointed point on the playfield XY plane, then drop a vertical ray on it: the parts
   // rest on the topmost surface it hits (the UI quadtree holds all parts, collidable or not)
   const Vertex2D pos = m_editor.UnprojectToPlane(mousePos, 0.f);
   const float zTop = m_editor.m_table->m_glassTopHeight + 500.f;
   const float zBottom = -500.f;
   vector<HitTestResult> hits;
   m_editor.m_player->m_physics->RayCast(Vertex3Ds(pos.x, pos.y, zTop), Vertex3Ds(pos.x, pos.y, zBottom), true, hits);
   const float restZ = hits.empty() ? 0.f : zTop + hits.front().m_time * (zBottom - zTop);

   if (pos.x == m_previewPos.x && pos.y == m_previewPos.y && restZ == m_previewZ)
   {
      m_previewVisible = true;
      return;
   }
   m_previewPos = pos;
   m_previewZ = restZ;

   // Move the whole pack content rigidly: the centroid of its part centers tracks the pointed point
   Vertex2D centroid(0.f, 0.f);
   for (IEditable *const part : m_previewParts)
   {
      const Vertex2D c = part->GetCenter();
      centroid.x += c.x;
      centroid.y += c.y;
   }
   if (!m_previewParts.empty())
   {
      centroid.x /= static_cast<float>(m_previewParts.size());
      centroid.y /= static_cast<float>(m_previewParts.size());
   }
   const Vertex2D delta(pos.x - centroid.x, pos.y - centroid.y);
   for (IEditable *const part : m_previewParts)
      part->Translate(delta);

   // Raise the content on the Z axis so its lowest bound lies on the hit surface. Only parts exposing
   // a Z translation get raised; curve based parts (ramps, surfaces, ...) keep their authored Z
   float minZ = FLT_MAX;
   for (IEditable *const part : m_previewParts)
   {
      vector<Vertex3Ds> bounds;
      part->GetBoundingVertices(bounds, nullptr);
      for (const Vertex3Ds &v : bounds)
         minZ = min(minZ, v.z);
   }
   if (minZ != FLT_MAX)
      for (size_t i = 0; i < m_previewParts.size(); i++)
      {
         Matrix3D transform;
         if ((m_previewUIParts[i]->GetTransform(transform) & EditorUIPart::TM_TransZ) != 0)
         {
            vec3 p, s, r;
            ExtractTRS(transform, p, s, r);
            p.z += restZ - minZ;
            m_previewUIParts[i]->SetTransform(p, s, r);
         }
      }
   for (IEditable *const part : m_previewParts)
      m_editor.m_renderer->ReinitRenderable(part->GetIRenderable());
   m_previewVisible = true;
}

void PartLibraryPanel::CommitPlacement()
{
   if (!IsPlacementArmed() || !m_previewVisible || m_editor.m_table->IsLocked() || m_editor.IsInspectMode())
      return;

   PinTable *const table = m_editor.m_table;
   PartGroup *const target = m_editor.GetPartGroupForNewPart();

   // Move the preview parts to the edited table: they are already at the pointed position. Their
   // assets (already merged by name at preview load) and authored part groups follow them
   m_previewParts.clear();
   m_previewUIParts.clear();
   m_editor.m_undo.BeginUndo();
   const vector<IEditable *> imported = table->ImportParts(m_previewTable, target);
   for (IEditable *const part : imported)
   {
      part->AddRef(); // Created parts keep a creation reference, released by the undo record (see PinTable::Uncreate)
      m_editor.m_undo.MarkForCreate(part);
   }
   // Player side setup, same as part creation; the renderables are re-initialized so that they resolve
   // the assets of the edited table (pack assets skipped by the merge are released with the scratch table)
   for (IEditable *const part : imported)
      SetupPlayerSide(part, false);
   m_editor.m_undo.EndUndo();
   m_previewTable->Release();
   m_previewTable = nullptr;

   // Select the imported parts
   m_editor.UpdateEditableList();
   m_editor.m_multiSel.clear();
   for (IEditable *const part : imported)
      if (part->GetItemType() != eItemPartGroup)
         if (const auto it = m_editor.m_editableMap.find(part); it != m_editor.m_editableMap.end())
            m_editor.m_multiSel.push_back(it->second);
   if (!m_editor.m_multiSel.empty())
      m_editor.m_selection = Selection(m_editor.m_multiSel.back());

   m_editor.m_liveUI.PushNotification("Placed "s + std::to_string(imported.size()) + " part(s) from '"s + PathToUTF8(m_placementPack.filename()) + "' into '"s
         + (target != nullptr ? target->GetName() : "(table root)"s) + '\'',
      10000);

   // Stay armed: reload the preview so that another copy can be placed on the next click
   if (!m_placementPack.empty())
      StartPlacement();
}

void PartLibraryPanel::ClearPreview()
{
   m_previewParts.clear();
   m_previewUIParts.clear();
   if (m_previewTable != nullptr)
   {
      if (m_editor.m_renderer != nullptr)
         for (IEditable *const edit : m_previewTable->GetParts())
         {
            if (edit->GetIRenderable() == nullptr)
               continue;
            // Same deferred release as part deletion: the render thread may still use the render state
            m_editor.m_renderer->DropRenderable(edit->GetIRenderable());
            edit->AddRef();
            edit->m_renderReleasePending = true;
            m_editor.m_renderer->m_renderDevice->AddEndOfFrameCmd(
               [edit]()
               {
                  edit->GetIRenderable()->RenderRelease();
                  edit->m_renderReleasePending = false;
                  edit->Release();
               });
         }
      m_previewTable->Release();
      m_previewTable = nullptr;
   }
   m_previewVisible = false;
}

void PartLibraryPanel::Render3D()
{
   if (m_previewVisible)
      for (IEditable *const part : m_previewParts)
         if (part->GetIRenderable() != nullptr)
            m_editor.m_renderer->RenderItem(part, false);
}
}
