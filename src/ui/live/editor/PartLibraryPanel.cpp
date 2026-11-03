// license:GPLv3+

#include "core/stdafx.h"
#include "PartLibraryPanel.h"

#include "core/Settings.h"
#include "core/VPApp.h"
#include "core/SettingsService.h"
#include "core/VPApp.h"
#include "fonts/IconsForkAwesome.h"
#include "parts/Material.h"
#include "parts/ball.h"
#include "parts/bumper.h"
#include "parts/decal.h"
#include "parts/flasher.h"
#include "parts/flipper.h"
#include "parts/gate.h"
#include "parts/hittarget.h"
#include "parts/kicker.h"
#include "parts/plunger.h"
#include "parts/light.h"
#include "parts/pintable.h"
#include "parts/primitive.h"
#include "parts/ramp.h"
#include "parts/rubber.h"
#include "parts/spinner.h"
#include "parts/surface.h"
#include "parts/trigger.h"
#include "physics/PhysicsEngine.h"
#include "renderer/RenderDevice.h"
#include "ui/live/EditorUI.h"
#include "ui/live/editor/EditorUIPartRegistry.h"
#include "PropertyPane.h"
#include "CameraPreviewPanel.h"
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

// The lights read Player::m_time_msec (blink pattern), which does not advance while editing: library previews run them on their own clock
struct PlayerClock
{
   explicit PlayerClock(const double msec)
      : saved(g_pplayer->m_time_msec)
   {
      g_pplayer->m_time_msec = static_cast<uint32_t>(msec);
   }
   ~PlayerClock() { g_pplayer->m_time_msec = saved; }
   const uint32_t saved;
};

// The Light of an insert pack is a state driver: taken out of the parts to draw (the pass has no light either way)
static vector<Light *> ExtractLights(vector<IEditable *> &parts)
{
   vector<Light *> lights;
   std::erase_if(parts,
      [&lights](IEditable *const part)
      {
         if (part->GetItemType() != eItemLight)
            return false;
         lights.push_back(static_cast<Light *>(part));
         return true;
      });
   return lights;
}

static void SetLightsState(const vector<Light *> &lights, const double clock, const float state) // 0 unlit, 1 lit, 2 blinking
{
   const PlayerClock pc(clock);
   for (Light *const light : lights)
      light->setInPlayState(state);
}

// dt in milliseconds, as Player::PrepareFrame passes it to UpdateAnimation. True when an intensity changed
static bool StepLights(const vector<Light *> &lights, double &clock, const float dtMs)
{
   clock += dtMs;
   const PlayerClock pc(clock);
   bool changed = false;
   for (Light *const light : lights)
   {
      const float before = light->m_currentIntensity;
      light->UpdateAnimation(dtMs);
      changed |= light->m_currentIntensity != before;
   }
   return changed;
}

// Lit and settled at once (the fader run to its end)
static void SettleLit(const vector<Light *> &lights, double &clock)
{
   SetLightsState(lights, clock, 1.f);
   for (int i = 0; i < 100; i++)
      StepLights(lights, clock, 16.f);
}

PartLibraryPanel::PartLibraryPanel(EditorUI &editor)
   : m_editor(editor)
{
   if (const char *const folder = std::getenv("VPX_LIBRARY_THUMBS"); folder != nullptr && *folder != '\0')
   {
      m_hookFolder = PathFromUTF8(folder);
      m_hookActive = true;
   }
   if (const char *const place = std::getenv("VPX_LIBRARY_PLACE"); place != nullptr && *place != '\0')
   {
      // <pack path>[;x;y]: the position when the last two fields are numbers, else the double click rule
      string spec = place;
      const size_t semiY = spec.rfind(';'), semiX = semiY == string::npos || semiY == 0 ? string::npos : spec.rfind(';', semiY - 1);
      if (semiX != string::npos)
      {
         char *endX = nullptr, *endY = nullptr;
         const string xs = spec.substr(semiX + 1, semiY - semiX - 1), ys = spec.substr(semiY + 1);
         const float x = strtof(xs.c_str(), &endX), y = strtof(ys.c_str(), &endY);
         if (!xs.empty() && !ys.empty() && *endX == '\0' && *endY == '\0')
         {
            m_hookPlaceHasPos = true;
            m_hookPlacePos = Vertex2D(x, y);
            spec.resize(semiX);
         }
      }
      m_hookPlace = spec;
   }
   if (const char *const preview = std::getenv("VPX_LIBRARY_PREVIEW"); preview != nullptr && *preview != '\0')
   {
      // <pack path>;<state>;<yaw>, split from the right
      string spec = preview, tail[2];
      int n = 0;
      for (size_t semi; n < 2 && (semi = spec.rfind(';')) != string::npos; n++)
      {
         tail[n] = spec.substr(semi + 1);
         spec.resize(semi);
      }
      const string state = lowerCase(tail[n - 1 < 0 ? 0 : n - 1]);
      m_hookPreview = spec;
      m_hookState = state == "unlit" ? 0.f : state == "blink" ? 2.f : 1.f;
      if (n == 2)
         m_hookYaw = static_cast<float>(atof(tail[0].c_str()));
   }
}

PartLibraryPanel::~PartLibraryPanel()
{
   ClearPreview();
   ReleaseScratch();
   ReleaseScratch(m_lbScratch, m_lbParts);
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

void PartLibraryPanel::OnEditorClosed()
{
   CloseLightbox();
   AbortThumbnail();
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

   // Placement mode housekeeping: it is only available while the table is editable and the playfield view is interactive (not in drag point edit mode,
   // not hidden by the fly mode), and is dropped while the part library area is hidden
   if (!m_editor.IsAreaEditorShown(EditorUI::AreaEditor::PartLibrary))
   {
      m_placementPack.clear();
      ClearPreview();
   }
   if (IsPlacementArmed() && (m_editor.IsInspectMode() || m_editor.m_table->IsLocked() || m_editor.m_pointEditPart != nullptr || m_editor.m_flyMode))
      CancelPlacement();
   if (m_dragActive && !IsPlacementArmed())
      m_dragActive = false;
   if (IsPlacementArmed())
   {
      UpdatePlacement(ImGui::GetIO().MousePos);
      if (m_dragActive)
      {
         ImGui::SetTooltip("Place %s", PathToUTF8(m_placementPack.stem()).c_str());
         if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
         {
            m_dragActive = false;
            if (m_previewVisible)
               CommitPlacement();
            else
               CancelPlacement(); // Released over the UI
         }
      }
   }
   else
      m_previewVisible = false;

   // Tile clicks: a double click places (a material is applied), a single click opens the lightbox once the double click interval passed
   if (m_doubleClicked)
   {
      m_doubleClicked = false;
      if (m_doubleItem.kind == ItemKind::Material)
         ApplyMaterialItem(m_doubleItem);
      else
         PlaceItem(m_doubleItem);
   }
   if (m_pendingOpen && !m_dragActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::GetTime() - m_pendingTime > ImGui::GetIO().MouseDoubleClickTime + 0.03)
   {
      m_pendingOpen = false;
      OpenLightbox(m_pendingItem);
   }

   // Test hook: place a pack once the editor is up
   if (!m_hookPlace.empty() && m_editor.m_renderer != nullptr && m_editor.m_player->m_physics != nullptr && ++m_hookPlaceFrames > 30)
   {
      LibraryItem item;
      item.pack = PathFromUTF8(m_hookPlace);
      const PackInfo info = ScanPack(item.pack);
      item.kind = info.kind;
      m_hookPlace.clear();
      if (!info.valid || !PlaceItem(item))
         PLOGW << "LIBPLACE failed " << PathToUTF8(item.pack);
   }

   // Test hook: open the lightbox on a pack
   if (!m_hookPreview.empty() && m_editor.m_renderer != nullptr)
   {
      LibraryItem item;
      item.pack = PathFromUTF8(m_hookPreview);
      m_hookPreviewLog = m_hookPreview;
      m_hookPreview.clear();
      const PackInfo info = ScanPack(item.pack);
      if (info.valid)
      {
         item.kind = info.kind;
         item.label = PathToUTF8(item.pack.stem());
         if (info.kind == ItemKind::Material && !info.materialNames.empty())
            item.material = item.label = info.materialNames[0];
         item.thumbFile = ThumbFolder(item.pack) / (JSONSerializer::SanitizeFileName(item.label) + (info.kind == ItemKind::Material ? "_0"s : ""s) + ".png"s);
         OpenLightbox(item);
         m_lbWantYaw = m_hookYaw;
         m_lbWantState = m_hookState;
      }
      else
         PLOGW << "LIBPREVIEW failed " << PathToUTF8(item.pack);
   }
   RenderLightbox();

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
               RemoveLibraryFolder(m_library[i].path); // By path: m_library skips the stored folders missing on disk
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

      // The grid is the last section: it takes the area height down to the bottom edge (at least a row and a half of tiles)
      const float gridHeight = max(ImGui::GetContentRegionAvail().y - 4.f * dpi, 150.f * dpi);
      if (ImGui::BeginChild("##LibraryGrid", ImVec2(0.f, gridHeight), ImGuiChildFlags_Borders))
         RenderGrid(ImGui::GetContentRegionAvail().y);
      ImGui::EndChild();
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
   const float tile = 96.f * dpi, labelH = ImGui::GetTextLineHeight() + 6.f * dpi;
   const ImGuiStyle &style = ImGui::GetStyle();
   const int columns = max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (tile + style.ItemSpacing.x)));
   const string filter = m_filterFolder.empty() ? ""s : PathToUTF8(m_filterFolder / ""); // Trailing separator: /lib/foo does not match /lib/foobar
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
         if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
         {
            // Double click places (no lightbox), once the panels are done
            m_pendingOpen = false;
            m_doubleItem = item;
            m_doubleClicked = true;
         }
         else
         {
            // Single click: the lightbox, unless a second click follows within the double click interval (see Update)
            m_pendingItem = item;
            m_pendingOpen = true;
            m_pendingTime = ImGui::GetTime();
         }
      }
      // Dragging a tile starts the placement: released over the 3D view commits it, over the UI or Esc cancels it
      if (ImGui::IsItemActive() && !m_dragActive && item.kind != ItemKind::Material && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.f * dpi) && !m_editor.m_table->IsLocked() && !m_editor.IsInspectMode())
      {
         m_pendingOpen = false;
         m_placementPack = item.pack;
         StartPlacement();
         m_dragActive = IsPlacementArmed();
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

void PartLibraryPanel::RemoveLibraryFolder(const std::filesystem::path &folder)
{
   vector<std::filesystem::path> folders = GetLibraryFolders();
   std::erase(folders, folder);
   // Removing the default library folder is persisted as an exclusion entry so it does not come back
   const std::filesystem::path defaultFolder = g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets) / "library";
   string stored;
   if (StrCompareNoCase(PathToUTF8(folder.lexically_normal()), PathToUTF8(defaultFolder.lexically_normal())))
      stored = "-"s + PathToString(defaultFolder.lexically_normal());
   for (const std::filesystem::path &kept : folders)
      stored += (stored.empty() ? ""s : ";"s) + PathToString(kept);
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
         const auto addItem = [&](const string &label, const string &material, const ItemKind kind, const string &fileStem)
         {
            LibraryItem &item = m_items.emplace_back();
            item.pack = pack;
            item.material = material;
            item.label = label;
            item.kind = kind;
            item.thumbFile = thumbs / (fileStem + ".png"s);
         };
         if (info.kind == ItemKind::Material)
            for (size_t i = 0; i < info.materialNames.size(); i++) // Indexed: sanitized names may collide (or differ by case only)
               addItem(info.materialNames[i], info.materialNames[i], ItemKind::Material, JSONSerializer::SanitizeFileName(info.materialNames[i]) + '_' + std::to_string(i));
         else
            addItem(PathToUTF8(pack.stem()), ""s, info.kind, JSONSerializer::SanitizeFileName(PathToUTF8(pack.stem())));
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
         if (name.starts_with("thumbs/") || std::ranges::any_of(file, [](const std::filesystem::path &c) { return PathToUTF8(c).starts_with('.'); })) // The thumbnails, VCS/OS files
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
            // Size and modification time: an edited uncompressed image keeps its size
            const uint64_t size = std::filesystem::file_size(pack / file, ec);
            const int64_t time = std::filesystem::last_write_time(pack / file, ec).time_since_epoch().count();
            hash(&size, sizeof(size));
            hash(&time, sizeof(time));
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

// Item bounds: the primitives' mesh bounds; only when no primitive has any, the other parts by their position +/- 100 VPU
FRect3D PartLibraryPanel::ScratchBounds(const vector<IEditable *> &parts)
{
   FRect3D bounds(FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
   bool any = false;
#if defined(ENABLE_BGFX)
   for (IEditable *const part : parts)
   {
      Vertex3Ds bMin, bMax;
      if (part->GetItemType() == eItemPrimitive && static_cast<Primitive *>(part)->GetShadowCacheBounds(bMin, bMax))
      {
         bounds.Extend(FRect3D(bMin.x, bMax.x, bMin.y, bMax.y, bMin.z, bMax.z));
         any = true;
      }
   }
#endif
   if (!any)
      for (IEditable *const part : parts)
      {
         const Vertex2D c = part->GetCenter();
         bounds.Extend(FRect3D(c.x - 100.f, c.x + 100.f, c.y - 100.f, c.y + 100.f, 0.f, 100.f));
      }
   return bounds;
}

// Loads the item into a scratch table (never saved, the live table is not consulted) and sets its parts up with the live renderer, as the
// placement ghost does. Material items: the built-in preview sphere wearing the material
bool PartLibraryPanel::LoadScratch(const LibraryItem &item, CComObject<PinTable> *&scratch, vector<IEditable *> &scratchParts)
{
   ReleaseScratch(scratch, scratchParts);
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
   scratch = load(item.pack);
   if (scratch == nullptr)
      return false;
   if (item.kind == ItemKind::Material)
   {
      CComObject<PinTable> *const sphere = load(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets) / "library" / "_preview" / "sphere.vpz");
      if (sphere == nullptr)
         return false;
      const vector<IEditable *> parts = scratch->ImportParts(sphere, nullptr);
      sphere->Release();
      for (IEditable *const part : parts)
         if (part->GetItemType() == eItemPrimitive)
            static_cast<Primitive *>(part)->m_d.m_szMaterial = item.material;
   }
   // ponytail: the scratch table's render probes are dropped (never set up with the renderer), probe reflection/refraction is not previewed
   for (RenderProbe *const probe : scratch->m_vrenderprobe)
      delete probe;
   scratch->m_vrenderprobe.clear();
   for (IEditable *const part : scratch->GetParts())
   {
      // A state link follows the live controller (Light::UpdateAnimation, the primitive's alpha): the previews run on their own state
      if (part->GetItemType() == eItemLight)
         static_cast<Light *>(part)->m_d.m_stateSrcLink.clear();
      else if (part->GetItemType() == eItemPrimitive)
         static_cast<Primitive *>(part)->m_d.m_stateSrcLink.clear();
      else if (part->GetItemType() == eItemFlasher)
         static_cast<Flasher *>(part)->m_d.m_stateSrcLink.clear();
      if (part->GetIRenderable() == nullptr)
         continue;
      part->GetIRenderable()->RenderSetup(m_editor.m_renderer.get());
      if (part->GetItemType() != eItemPartGroup)
         scratchParts.push_back(part);
   }
   return !scratchParts.empty();
}

void PartLibraryPanel::ReleaseScratch(CComObject<PinTable> *&scratch, vector<IEditable *> &scratchParts)
{
   if (m_editor.m_renderer != nullptr)
      m_editor.m_renderer->CancelIsolatedEditorView(&scratchParts);
   scratchParts.clear();
   if (scratch == nullptr)
      return;
   if (m_editor.m_renderer != nullptr)
      for (IEditable *const edit : scratch->GetParts())
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
      for (Texture *const image : scratch->GetImageList())
         m_editor.m_renderer->m_renderDevice->m_texMan.UnloadTexture(image);
   scratch->Release();
   scratch = nullptr;
}

bool PartLibraryPanel::LoadScratch(const LibraryItem &item) { return LoadScratch(item, m_scratch, m_scratchParts); }

void PartLibraryPanel::ReleaseScratch() { ReleaseScratch(m_scratch, m_scratchParts); }

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
   if (renderer == nullptr || m_lbOpen || (m_thumbQueue.empty() && m_thumbStage == ThumbStage::Idle && m_thumbSlot < 0))
      return; // The lightbox has the view slot and the editor's attention: the thumbnails wait (OpenLightbox aborted the one in flight)
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
      // Checked before loading the item: a refused request would reload it every frame
      if (m_thumbSlot < 0 || g_pplayer->GetInfoMode() != IF_NONE || !renderer->CanRenderEditorPBRView(m_thumbSlot))
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
      if (item.kind == ItemKind::Insert)
      {
         double clock = 1000.0;
         SettleLit(ExtractLights(m_scratchParts), clock); // Lit, not drawn
      }
      const FRect3D bounds = ScratchBounds(m_scratchParts);
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
      m_thumbTicket = ++m_readSeq == 0 ? ++m_readSeq : m_readSeq; // Never 0 (the VR screenshot ticket)
      rd->AddEndOfFrameCmd([rd, rt, file = item.thumbFile, ticket = m_thumbTicket]() { rd->RequestVRScreenshot(rt, file, ticket); });
      ReleaseScratch();
      m_thumbStage = ThumbStage::ReadBack;
      break;
   }

   case ThumbStage::ReadBack:
   {
      RenderDevice *const rd = renderer->m_renderDevice;
      if (rd->m_directScreenshotDone != m_thumbTicket) // A read back aborted earlier completes with its own ticket
         return;
      LibraryItem &item = m_items[m_thumbQueue.back()];
      m_thumbQueue.pop_back();
      m_thumbStage = ThumbStage::Idle;
      item.thumb = nullptr;
      item.thumbLoaded = false;
      if (!rd->m_directScreenshotSaved)
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

void PartLibraryPanel::OpenLightbox(const LibraryItem &item)
{
   CloseLightbox();
   AbortThumbnail(); // The thumbnails pause while the lightbox is open
   m_lbItem = item;
   m_lbInfo = ScanPack(item.pack);
   m_lbOpen = m_lbOpening = true;
   m_lbDirty = true;
   m_lbRequested = m_lbLogged = m_lbFailed = false;
   m_lbWidth = m_lbHeight = m_lbPendingFrames = 0;
   m_lbWantYaw = 35.f;
   m_lbWantState = 1.f;
   m_lbState = 1.f;
   m_lbClock = 1000.0;
   m_lbAnimUntil = 0.0;
}

void PartLibraryPanel::CloseLightbox()
{
   ReleaseScratch(m_lbScratch, m_lbParts); // Also drops the pending request and unloads the scratch images
   m_lbLights.clear();
   if (m_lbSlot >= 0 && m_editor.m_renderer != nullptr && m_editor.m_cameraPreviews[m_lbSlot].GetLeaf() < 0)
      m_editor.m_renderer->ReleaseEditorView(m_lbSlot);
   m_lbSlot = -1;
   m_lbOpen = m_lbOpening = false;
   m_lbItem = LibraryItem();
}

void PartLibraryPanel::FrameLightbox(const bool resetAngles)
{
   if (resetAngles)
   {
      m_lbCam.yaw = 35.f;
      m_lbCam.pitch = 30.f;
   }
   // On the bounding sphere: the distance does not change with the yaw (the projected box would)
   const float aspect = m_lbHeight > 0 ? static_cast<float>(m_lbWidth) / static_cast<float>(m_lbHeight) : 1.f;
   const float dx = m_lbBounds.right - m_lbBounds.left, dy = m_lbBounds.bottom - m_lbBounds.top, dz = m_lbBounds.zhigh - m_lbBounds.zlow;
   const float radius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
   const float halfV = 0.5f * ANGTORAD(m_editor.m_navFovY), halfH = atanf(tanf(halfV) * aspect);
   m_lbCam.target[0] = 0.5f * (m_lbBounds.left + m_lbBounds.right);
   m_lbCam.target[1] = 0.5f * (m_lbBounds.top + m_lbBounds.bottom);
   m_lbCam.target[2] = 0.5f * (m_lbBounds.zlow + m_lbBounds.zhigh);
   m_lbCam.distance = max(1.f, 1.1f * radius / sinf(min(halfV, halfH)));
   m_lbCam.orbitValid = true;
   m_lbDirty = true;
}

void PartLibraryPanel::SetLightboxState(const float state)
{
   m_lbState = state;
   SetLightsState(m_lbLights, m_lbClock, state);
   m_lbAnimUntil = m_lbClock + 4000.0; // The fade settles in well under a second; the cap stops a float that never quite converges
   m_lbDirty = true;
}

bool PartLibraryPanel::AnimateLights(const float dtMs) { return StepLights(m_lbLights, m_lbClock, dtMs); }

void PartLibraryPanel::RenderLightbox()
{
   if (!m_lbOpen)
      return;
   Renderer *const renderer = m_editor.m_renderer.get();
   if (renderer == nullptr)
   {
      CloseLightbox();
      return;
   }
   const float dpi = m_editor.m_liveUI.GetDPI();
   const ImGuiIO &io = ImGui::GetIO();
   const bool justOpened = m_lbOpening; // The click that opened it is still this frame's click
   if (m_lbOpening)
   {
      ImGui::OpenPopup("###PartLibLightbox");
      m_lbOpening = false;
      ImGui::SetNextWindowSize(ImVec2(480.f * dpi, 520.f * dpi), ImGuiCond_Appearing);
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
   }
   ImGui::SetNextWindowSizeConstraints(ImVec2(240.f * dpi, 260.f * dpi), ImVec2(FLT_MAX, FLT_MAX));
   bool open = true;
   if (!ImGui::BeginPopupModal((m_lbItem.label + "###PartLibLightbox"s).c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
   {
      CloseLightbox();
      return;
   }
   // Esc or a click outside closes it
   if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape) || (!justOpened && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)))
   {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      CloseLightbox();
      return;
   }

   // View slot (one no Camera Preview area uses, with PBR room under the cap), then the scratch table of the item
   bool refused = false;
   const bool desktop = g_pplayer->m_vrDevice == nullptr && renderer->m_stereo3D == STEREO_OFF;
   if (m_lbSlot >= 0 && m_editor.m_cameraPreviews[m_lbSlot].GetLeaf() >= 0)
   {
      m_lbSlot = -1; // A Camera Preview area took it over
      m_lbDirty = true;
   }
   if (m_lbSlot < 0 && desktop && !m_lbFailed)
      for (int i = AreaLayout::kMaxViewLeaves - 1; i >= 0 && m_lbSlot < 0; i--)
         if (m_editor.m_cameraPreviews[i].GetLeaf() < 0 && renderer->CanRenderEditorPBRView(i))
            m_lbSlot = i;
   if (m_lbSlot >= 0 && m_lbScratch == nullptr && !m_lbFailed)
   {
      vector<IEditable *> all;
      if (!LoadScratch(m_lbItem, m_lbScratch, all))
      {
         ReleaseScratch(m_lbScratch, all);
         m_lbFailed = true;
         PLOGW << "LIBPREVIEW failed " << PathToUTF8(m_lbItem.pack);
      }
      else
      {
         if (m_lbItem.kind == ItemKind::Insert)
            m_lbLights = ExtractLights(all);
         m_lbParts = all;
         if (m_lbParts.empty())
         {
            ReleaseScratch(m_lbScratch, all);
            m_lbLights.clear();
            m_lbFailed = true;
         }
         else
         {
            m_lbBounds = ScratchBounds(m_lbParts);
            m_lbCam = AreaViewSettings();
            FrameLightbox(true);
            m_lbCam.yaw = m_lbWantYaw;
            if (!m_lbLights.empty())
            {
               SettleLit(m_lbLights, m_lbClock); // A click opens an insert lit, not fading in
               if (m_lbWantState != 1.f)
               {
                  SetLightboxState(m_lbWantState);
                  for (int i = 0; i < 100 && m_lbWantState == 0.f; i++) // Unlit settled too; blinking starts from its own pattern
                     AnimateLights(16.f);
               }
               m_lbState = m_lbWantState;
               m_lbAnimUntil = 0.0;
            }
         }
      }
   }

   // Header: name, kind and counts
   static constexpr const char *kKinds[] = { "Part", "Material", "Insert" };
   ImGui::TextUnformatted(m_lbItem.label.c_str());
   ImGui::SameLine();
   ImGui::TextDisabled("%s", kKinds[static_cast<int>(m_lbItem.kind)]);
   ImGui::TextDisabled("%d part(s), %d material(s), %d image(s)%s%s", m_lbInfo.parts, m_lbInfo.materials, m_lbInfo.images, m_lbItem.material.empty() ? "" : ", material ", m_lbItem.material.c_str());

   // Insert states, through the pack's own Light
   if (m_lbItem.kind == ItemKind::Insert)
   {
      ImGui::BeginDisabled(m_lbLights.empty());
      static constexpr struct { const char *label; float state; } kStates[] = { { "Lit", 1.f }, { "Unlit", 0.f }, { "Blink", 2.f } };
      for (const auto &st : kStates)
      {
         const bool active = m_lbState == st.state;
         if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
         if (ImGui::Button(st.label) && !active)
            SetLightboxState(st.state);
         if (active)
            ImGui::PopStyleColor();
         ImGui::SameLine();
      }
      ImGui::EndDisabled();
      if (m_lbLights.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("This pack has no Light");
   }
   if (ImGui::Button("Reset View") && m_lbScratch != nullptr)
      FrameLightbox(true);

   // The light fades are stepped while the lightbox is open, and re-rendered only while they move (blinking: always)
   if (m_lbScratch != nullptr && !m_lbLights.empty())
   {
      const bool changed = AnimateLights(min(io.DeltaTime * 1000.f, 100.f));
      if (m_lbState == 2.f || (changed && m_lbClock < m_lbAnimUntil))
         m_lbDirty = true;
   }

   // Image area, orbited like a Sketchfab model: left drag, wheel zoom, double click frames
   const float hintH = ImGui::GetTextLineHeightWithSpacing();
   const ImVec2 avail = ImGui::GetContentRegionAvail();
   const ImVec2 size(max(32.f, avail.x), max(32.f, avail.y - hintH));
   const ImVec2 imgMin = ImGui::GetCursorScreenPos(), imgMax(imgMin.x + size.x, imgMin.y + size.y);
   ImGui::InvisibleButton("##view", size);
   const bool hovered = ImGui::IsItemHovered();
   if (m_lbScratch != nullptr)
   {
      if (ImGui::IsItemActive() && (io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f))
      {
         constexpr float radPerPixel = 0.01f * (180.f / static_cast<float>(M_PI));
         m_lbCam.yaw = fmodf(m_lbCam.yaw - io.MouseDelta.x * radPerPixel * m_editor.m_navOrbitSensitivity, 360.f);
         m_lbCam.pitch = clamp(m_lbCam.pitch + io.MouseDelta.y * radPerPixel * m_editor.m_navOrbitSensitivity, -89.f, 89.f);
         m_lbDirty = true;
      }
      if (hovered && io.MouseWheel != 0.f)
      {
         m_lbCam.distance = clamp(m_lbCam.distance * powf(1.1f, (m_editor.m_navInvertZoom ? 1.f : -1.f) * io.MouseWheel), 1.f, 40000.f);
         m_lbDirty = true;
      }
      if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
         FrameLightbox(true); // Distance and orbit angles
   }

   // Render size: the image in pixels, a new size adopted once stable (no target churn while the window is resized)
   const int w = clamp(static_cast<int>(size.x * io.DisplayFramebufferScale.x) & ~15, 64, 1024), h = clamp(static_cast<int>(size.y * io.DisplayFramebufferScale.y) & ~15, 64, 1024);
   if (m_lbWidth == 0)
   {
      m_lbWidth = w;
      m_lbHeight = h;
      if (m_lbScratch != nullptr)
         FrameLightbox(false); // Framed for the real aspect
   }
   else if (w != m_lbWidth || h != m_lbHeight)
   {
      if (w != m_lbPendingW || h != m_lbPendingH)
         m_lbPendingFrames = 0;
      m_lbPendingW = w;
      m_lbPendingH = h;
      if (++m_lbPendingFrames >= 10)
      {
         m_lbWidth = w;
         m_lbHeight = h;
         m_lbDirty = true;
      }
   }
   else
      m_lbPendingFrames = 0;

   // The slot's targets were released under us after the request (a Camera Preview area dropping the slot): render again
   if (m_lbRequested && m_lbSlot >= 0 && !renderer->IsEditorViewPending(m_lbSlot) && renderer->GetEditorView(m_lbSlot) == nullptr)
      m_lbDirty = true;
   if (m_lbSlot >= 0 && m_lbScratch != nullptr && m_lbDirty && !renderer->IsEditorViewPending(m_lbSlot))
   {
      Renderer::EditorViewRequest req;
      req.width = m_lbWidth;
      req.height = m_lbHeight;
      req.playerCamera = false;
      CameraPreviewPanel::ComputeOrbit(m_lbCam, m_editor.m_navFovY, static_cast<float>(m_lbWidth) / static_cast<float>(m_lbHeight), req.view, req.proj);
      req.isolated = &m_lbParts;
      req.isolatedTable = m_lbScratch;
      if (renderer->RequestEditorView(m_lbSlot, req))
      {
         m_lbDirty = false;
         m_lbRequested = true;
      }
      else
         refused = true; // An info mode is on: retried every frame
   }

   ImDrawList *const drawList = ImGui::GetWindowDrawList();
   RenderTarget *const rt = m_lbSlot >= 0 && m_lbScratch != nullptr ? renderer->GetEditorView(m_lbSlot) : nullptr;
   const char *note = nullptr;
   drawList->AddRectFilled(imgMin, imgMax, IM_COL32(46, 46, 46, 255));
   if (rt != nullptr)
   {
#if defined(ENABLE_BGFX)
      const bool flipV = bgfx::getCaps()->originBottomLeft;
#elif defined(ENABLE_OPENGL)
      const bool flipV = true;
#else
      const bool flipV = false;
#endif
      // Aspect kept: the render size is the image size rounded down to 16 px (capped at 1024)
      const float fit = min(size.x / static_cast<float>(m_lbWidth), size.y / static_cast<float>(m_lbHeight));
      const ImVec2 half(0.5f * fit * static_cast<float>(m_lbWidth), 0.5f * fit * static_cast<float>(m_lbHeight)), mid(0.5f * (imgMin.x + imgMax.x), 0.5f * (imgMin.y + imgMax.y));
      drawList->AddImage(rt->GetColorSampler(), ImVec2(mid.x - half.x, mid.y - half.y), ImVec2(mid.x + half.x, mid.y + half.y), ImVec2(0.f, flipV ? 1.f : 0.f), ImVec2(1.f, flipV ? 0.f : 1.f));
      if (refused)
         note = "Not available while an info mode is on";
      else if (m_lbRequested && !m_lbLogged && !renderer->IsEditorViewPending(m_lbSlot))
      {
         m_lbLogged = true;
         PLOGI << "LIBPREVIEW open " << (m_hookPreviewLog.empty() ? PathToUTF8(m_lbItem.pack) : m_hookPreviewLog);
      }
   }
   else
   {
      // No live render: the cached thumbnail
      if (!m_lbItem.thumb && !m_lbItem.thumbLoaded)
      {
         m_lbItem.thumbLoaded = true;
         std::error_code ec;
         if (std::filesystem::exists(m_lbItem.thumbFile, ec))
            if (std::shared_ptr<const BaseTexture> tex = BaseTexture::CreateFromFile(m_lbItem.thumbFile))
               m_lbItem.thumb = std::make_shared<Sampler>(renderer->m_renderDevice, "Library Thumbnail"s, tex, false);
      }
      if (m_lbItem.thumb)
      {
         const float side = min(size.x, size.y);
         const ImVec2 c(0.5f * (imgMin.x + imgMax.x), 0.5f * (imgMin.y + imgMax.y));
         drawList->AddImage(m_lbItem.thumb, ImVec2(c.x - 0.5f * side, c.y - 0.5f * side), ImVec2(c.x + 0.5f * side, c.y + 0.5f * side));
      }
      note = m_lbFailed ? "Nothing to render in this pack" : !desktop ? "Live view needs the desktop renderer" : m_lbSlot < 0 ? "No free PBR view: showing the thumbnail" : nullptr;
   }
   if (note != nullptr)
      drawList->AddText(ImVec2(imgMin.x + 6.f * dpi, imgMax.y - ImGui::GetTextLineHeight() - 4.f * dpi), EditorTheme::TextDim, note);
   drawList->AddRect(imgMin, imgMax, hovered ? EditorTheme::Accent : EditorTheme::WorkspaceOff, 0.f, 0, 1.f);
   ImGui::TextDisabled("Drag: orbit   Wheel: zoom   Double click: frame   Esc: close");
   ImGui::EndPopup();
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
// Render3D while the pointer hovers the 3D playfield view, positioned by UpdatePlacement.
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
      m_placementPack.clear();
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
   m_dragActive = false;
   m_placementPack.clear();
   ClearPreview();
}

void PartLibraryPanel::UpdatePlacement(const ImVec2 &mousePos)
{
   const ImGuiIO &io = ImGui::GetIO();
   // While a tile is dragged the pointer is captured by the panel: the 3D view is then the viewport area
   const AreaRect &view = m_editor.m_viewportContentRect;
   const bool overView = m_dragActive ? (m_editor.m_viewportVisible && mousePos.x >= view.x0 && mousePos.x < view.x1 && mousePos.y >= view.y0 && mousePos.y < view.y1) : !io.WantCaptureMouse;
   const bool hovering = overView && m_editor.m_camMode != ViewMode::DesktopBackdrop && m_editor.m_fileOp == nullptr && m_editor.m_player->m_playfieldWnd != nullptr;
   if (!hovering)
   {
      // Re-hide the ghost parts when the pointer leaves the 3D view (or the view is not 3D)
      m_previewVisible = false;
      return;
   }
   PlaceAt(m_editor.UnprojectToPlane(mousePos, 0.f));
}

// Moves the pack content to the given playfield XY point, resting on the topmost surface under it (a vertical ray cast: the UI quadtree holds all
// parts, collidable or not)
void PartLibraryPanel::PlaceAt(const Vertex2D &pos)
{
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
   // (the lowest bound is taken over the raisable parts only: a part that never moves would be raised again at every pointer move)
   float minZ = FLT_MAX;
   for (size_t i = 0; i < m_previewParts.size(); i++)
   {
      Matrix3D transform;
      if ((m_previewUIParts[i]->GetTransform(transform) & EditorUIPart::TM_TransZ) == 0)
         continue;
      vector<Vertex3Ds> bounds;
      m_previewParts[i]->GetBoundingVertices(bounds, nullptr);
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
   m_editor.m_undo.SetLabel("Place " + PathToUTF8(m_placementPack.stem()));
   m_editor.m_undo.EndUndo();
   UnloadImages(m_previewTable); // Only the images the merge left in it (the merged ones moved to the edited table and stay loaded)
   m_previewTable->Release();
   m_previewTable = nullptr;
   m_previewVisible = false;
   m_placedCount = static_cast<int>(imported.size());

   // Select the imported parts
   m_editor.UpdateEditableList();
   vector<std::shared_ptr<EditorUIPart>> placedParts;
   for (IEditable *const part : imported)
      if (part->GetItemType() != eItemPartGroup)
         if (const auto it = m_editor.m_editableMap.find(part); it != m_editor.m_editableMap.end())
            placedParts.push_back(it->second);
   if (!placedParts.empty())
   {
      m_editor.SetSelection(Selection(placedParts.back()));
      m_editor.m_multiSel = placedParts;
      m_editor.m_outlinerAnchor = placedParts.back();
   }

   // F9 repeats the placement with an offset, as for an added part (the offset is relative to the base: Z of the surface it rests on)
   if (Matrix3D placed; m_editor.GetSelectionTransform(placed))
   {
      m_editor.SetLastOperation(EditorUI::LastOperation::Type::Add, placed);
      m_editor.m_lastOp.base = vec3(0.f, 0.f, placed._43);
      m_editor.m_lastOp.offset = vec3(placed._41, placed._42, 0.f);
   }

   m_editor.m_liveUI.PushNotification("Placed "s + std::to_string(imported.size()) + " part(s) from '"s + PathToUTF8(m_placementPack.filename()) + "' into '"s
         + (target != nullptr ? target->GetName() : "(table root)"s) + '\'',
      10000);
   m_placementPack.clear(); // ponytail: not armed for another copy, the next placement is a new drag or double click
   m_dragActive = false;
}

void PartLibraryPanel::UnloadImages(PinTable *const table)
{
   if (m_editor.m_renderer != nullptr)
      for (Texture *const image : table->GetImageList())
         m_editor.m_renderer->m_renderDevice->m_texMan.UnloadTexture(image);
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
      UnloadImages(m_previewTable);
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
            m_editor.m_renderer->RenderItemOfTable(part, m_previewTable, false);
}

// The double click position: the centre of the selection bounds if there is a selection, else the table centre
Vertex2D PartLibraryPanel::DefaultPlacementPos() const
{
   if (FRect3D bounds; !m_editor.m_multiSel.empty() && m_editor.GetSelectionBounds(bounds))
      return Vertex2D(0.5f * (bounds.left + bounds.right), 0.5f * (bounds.top + bounds.bottom));
   return Vertex2D(0.5f * (m_editor.m_table->m_left + m_editor.m_table->m_right), 0.5f * (m_editor.m_table->m_top + m_editor.m_table->m_bottom));
}

bool PartLibraryPanel::PlaceItem(const LibraryItem &item)
{
   if (m_editor.m_table->IsLocked() || m_editor.IsInspectMode() || m_editor.m_pointEditPart != nullptr)
      return false;
   // The default position is read before the placement replaces the selection
   const Vertex2D pos = m_hookPlaceHasPos ? m_hookPlacePos : DefaultPlacementPos();
   m_hookPlaceHasPos = false;
   CancelPlacement();
   m_placementPack = item.pack;
   StartPlacement();
   if (!IsPlacementArmed())
      return false;
   PlaceAt(pos);
   CommitPlacement();
   if (Matrix3D placed; m_editor.GetSelectionTransform(placed) && getenv("VPX_LIBRARY_PLACE") != nullptr)
      PLOGI << "LIBPLACE " << PathToUTF8(item.pack) << " parts=" << m_placedCount << " at=" << placed._41 << ',' << placed._42 << ',' << placed._43;
   return true;
}

// Sets (or, with apply false, only tests) the part's material: the types with one material field
static bool SetPartMaterial(IEditable *const part, const string &name, const bool apply = true)
{
   const auto set = [apply, &name](string &field)
   {
      if (apply)
         field = name;
      return true;
   };
   switch (part->GetItemType())
   {
   case eItemPrimitive: return set(static_cast<Primitive *>(part)->m_d.m_szMaterial);
   case eItemRamp: return set(static_cast<Ramp *>(part)->m_d.m_szMaterial);
   case eItemSurface: return set(static_cast<Surface *>(part)->m_d.m_szTopMaterial);
   case eItemDecal: return set(static_cast<Decal *>(part)->m_d.m_szMaterial);
   case eItemFlipper: return set(static_cast<Flipper *>(part)->m_d.m_szMaterial);
   case eItemHitTarget: return set(static_cast<HitTarget *>(part)->m_d.m_szMaterial);
   case eItemPlunger: return set(static_cast<Plunger *>(part)->m_d.m_szMaterial);
   case eItemRubber: return set(static_cast<Rubber *>(part)->m_d.m_szMaterial);
   case eItemSpinner: return set(static_cast<Spinner *>(part)->m_d.m_szMaterial);
   case eItemKicker: return set(static_cast<Kicker *>(part)->m_d.m_szMaterial);
   case eItemTrigger: return set(static_cast<Trigger *>(part)->m_d.m_szMaterial);
   case eItemGate: return set(static_cast<Gate *>(part)->m_d.m_szMaterial);
   case eItemBumper: return set(static_cast<Bumper *>(part)->m_d.m_szCapMaterial);
   default: return false;
   }
}

// Imports the material pack through the same merge path as a part pack, then assigns the material to the selected parts, in the same undo step.
// Nothing is imported (and no undo step made) when no selected part can take a material
void PartLibraryPanel::ApplyMaterialItem(const LibraryItem &item)
{
   if (m_editor.m_table->IsLocked() || m_editor.IsInspectMode() || m_editor.m_pointEditPart != nullptr)
      return;
   vector<IEditable *> targets;
   for (const std::shared_ptr<EditorUIPart> &uiPart : m_editor.m_multiSel)
      if (IEditable *const part = uiPart->GetEditable(); !part->IsUILocked() && SetPartMaterial(part, string(), false))
         targets.push_back(part);
   if (targets.empty())
   {
      m_editor.m_liveUI.PushNotification("Select the parts to apply the material to"s, 4000);
      return;
   }
   CComObject<PinTable> *temp;
   CComObject<PinTable>::CreateInstance(&temp);
   temp->AddRef();
   VPXFileFeedback feedback;
   const std::unique_ptr<JSONSerializer::Deserializer> pack = m_editor.m_table->CreateImportDeserializer(item.pack, m_mergeStrategy);
   const HRESULT hr = pack != nullptr ? temp->LoadGameFromJSONPack(*pack, feedback) : E_FAIL;
   if (FAILED(hr) && (hr != APPX_E_BLOCK_HASH_INVALID) && (hr != APPX_E_CORRUPT_CONTENT))
   {
      temp->Release();
      m_editor.m_liveUI.PushNotification("Failed to import '"s + PathToUTF8(item.pack) + '\'', 10000);
      return;
   }
   // The name the material has after the merge: the deserializer's rename map (keyed by the file stem), else it kept its name
   const string stem = JSONSerializer::SanitizeFileName(item.material), renamed = pack->RenamedAsset("materials", stem);
   const string name = StrCompareNoCase(renamed, stem) ? item.material : renamed;
   PinTable *const table = m_editor.m_table;
   m_editor.m_undo.BeginUndo();
   table->ImportParts(temp, nullptr);
   temp->Release();
   int assigned = 0;
   if (table->GetMaterial(name) != nullptr)
      for (IEditable *const part : targets)
      {
         m_editor.m_undo.MarkForUndo(part);
         SetPartMaterial(part, name);
         assigned++;
      }
   m_editor.m_undo.SetLabel("Apply material " + name);
   m_editor.m_undo.EndUndo();
   for (IEditable *const part : targets)
      m_editor.m_renderer->ReinitRenderable(part->GetIRenderable());
   m_editor.m_liveUI.PushNotification("Imported material '"s + name + "', applied to "s + std::to_string(assigned) + " part(s)"s, 10000);
}

}
