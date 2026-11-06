// license:GPLv3+

#include "core/stdafx.h"
#include "EditorUI.h"

#include "core/TableDB.h"
#include "core/VPXPluginAPIImpl.h"
#include "core/editablereg.h"
#include "core/extern.h"
#include "core/FileLocator.h"
#include "core/VPApp.h"
#include "core/vpversion.h"

#include "editor/EditorTheme.h"
#include "editor/EditorUIPart.h"
#include "editor/EditorUIPartRegistry.h"
#include "editor/IconsLucide.h"
#include "editor/LiveRenderContext.h"
#include "editor/MirrorParts.h"
#include "editor/PlayfieldMaker.h"
#include "editor/PrimitiveUIPart.h"
#include "editor/PropertyPane.h"
#include "editor/RubberBetweenPosts.h"

#include "parts/Collection.h"
#include "parts/PartGroup.h"
#include "parts/ball.h"
#include "parts/light.h"
#include "parts/Material.h"
#include "parts/primitive.h"
#include "parts/rubber.h"
#include "math/dragpoint.h"
#include "math/matrix.h"
#include "utils/UnityPrefabImporter.h"

#include "physics/PhysicsEngine.h"
#include "plugins/VPXPlugin.h"

#include "renderer/Anaglyph.h"
#include "renderer/Renderer.h"
#include "renderer/Sampler.h"
#include "renderer/Shader.h"
#include "renderer/VRDevice.h"

#include "ui/EditorClipboard.h"
#include "ui/VPXFileFeedback.h"
#include "ui/live/LiveUI.h"

#include "utils/BiffReader.h"
#include "utils/color.h"
#include "utils/JSONSerializer.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <cstdlib>
#include <map>
#include <set>
#include <span>
#include <unordered_set>

#if defined(ENABLE_DX9)
#include <shellapi.h>
#endif

#include "imgui/imgui_stdlib.h"
#include "imguizmo/ImGuizmo.h"
#include "input/SDLInputHandler.h"
#include "imgui_markdown/imgui_markdown.h"

namespace VPX::EditorUI
{

namespace
{
// A Ramp Collider's walls are a part of their own: their physics follows an edit of the host
void UpdateRampWallsPhysics(Player *const player, IEditable *const edit)
{
   if (edit == nullptr || edit->GetItemType() != eItemPrimitive || !static_cast<Primitive *>(edit)->m_d.m_rampCollider)
      return;
   for (Primitive *const comp : static_cast<Primitive *>(edit)->GetPlasticCompanions())
      player->m_physics->Update(comp);
}

   float YawOfRotation(const Matrix3D &l, float &step); // Defined with the transform helpers below
   constexpr float kMaxLastOpAngle = 36000.f; // Adjust Last Operation's Rotate Angle: a hundred turns either way
}

// Vertical field of view (in degrees) of the perspective editor camera


EditorUI::EditorUI(LiveUI &liveUI)
   : m_liveUI(liveUI)
   , m_player(g_pplayer)
   , m_renderer(m_player->m_renderer)
   , m_chrome(*this)
   , m_properties(*this)
   , m_inspectionModal(*this)
   , m_pbrConvertModal(*this)
   , m_selectRename(*this)
   , m_partLibrary(*this)
   , m_cameraPreviews { CameraPreviewPanel(*this, 0), CameraPreviewPanel(*this, 1), CameraPreviewPanel(*this, 2) }
   , m_scriptPanel(*this)
   , m_tableCheck(*this)
   , m_pinned(*this)
   , m_palette(*this)
   , m_undo(m_player->m_ptable)
{
   m_table = m_player->m_ptable;
   Reset3DCursor();
   m_pininput = &(m_player->m_pininput);

   EditorUIPartRegistry::InitRegistry();

   // Store the current selection in each undo record, so that undoing also restores it
   m_undo.SetEditorStateCapture([this]() { return std::any(CaptureUndoSelection()); });
   m_undo.SetPartRenamedHook(
      [this](const string &oldName, const string &newName, const bool restored)
      {
         if (restored)
            m_pinned.RenamePart(oldName, newName);
         else
            Report("Could not restore the name " + newName + " of " + oldName + ": another part, collection or script name uses it now");
      });
   // A part's m_d holds the editor display state (outliner hide, overlay) for the render visibility fields, the authored values live in the UI part: records hold
   // the authored values (written back to m_d before a save), and a loaded record becomes the new authored values
   m_undo.SetPartStateSync(
      [this](IEditable *part, const bool loaded)
      {
         for (const auto &uiPart : m_editables)
            if (uiPart->GetEditable() == part)
            {
               if (loaded)
                  uiPart->SyncVisibilityOverrides();
               else
                  uiPart->RestorePartVisibility();
               break;
            }
      });

   // The rotation & scale frame of a curve part is editor state outside the saved part: records keep it with the part
   m_undo.SetPartFrameHooks(
      [this](IEditable *part) -> std::any
      {
         for (const auto &uiPart : m_editables)
            if (uiPart->GetEditable() == part)
               return uiPart->GetCurveFrame();
         return {};
      },
      [this](IEditable *part, const std::any &frame)
      {
         for (const auto &uiPart : m_editables)
            if (uiPart->GetEditable() == part)
            {
               if (const auto *curveFrame = std::any_cast<EditorUIPart::CurveFrame>(&frame))
                  uiPart->SetCurveFrame(*curveFrame);
               break;
            }
      });

   ClearSelection();
   LoadNavPreferences();

   // Editor camera position. We use a right handed system for easy ImGuizmo integration while VPX renderer is left handed, so reverse X axis
   m_camDistance = m_table->m_bottom * 0.7f;
   const vec3 eye(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, -m_camDistance);
   const vec3 at(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, 0.f);
   constexpr vec3 up { 0.f, -1.f, 0.f };
   m_camView = Matrix3D::MatrixLookAtRH(eye, at, up);
   ImGuizmo::AllowAxisFlip(false);
}

void EditorUI::LoadNavPreferences()
{
   const Settings &settings = g_settingsService.GetAppSettings();
   m_navOrbitSelection = settings.GetEditor_NavOrbitSelection();
   m_navZoomToMouse = settings.GetEditor_NavZoomToMouse();
   m_navInvertZoom = settings.GetEditor_NavInvertZoom();
   m_navOrbitSensitivity = settings.GetEditor_NavOrbitSensitivity();
   m_navFovY = settings.GetEditor_NavFovY();
   m_navAutoDepth = settings.GetEditor_NavAutoDepth();
   m_navSelectionDisplay = settings.GetEditor_NavSelectionDisplay();
   m_navAltMiddleButton = settings.GetEditor_NavAltMiddleButton();
   m_navTrackpad = settings.GetEditor_NavTrackpad();
   m_navEmulateNumpad = settings.GetEditor_NavEmulateNumpad();
   m_navBlenderTransform = settings.GetEditor_NavBlenderTransform();
   m_transformPivot = static_cast<TransformPivot>(settings.GetEditor_TransformPivot());
   m_transformOrientation = static_cast<TransformOrientation>(settings.GetEditor_TransformOrientation());
   LoadSnapPreferences();
}

void EditorUI::LoadSnapPreferences()
{
   const Settings &settings = g_settingsService.GetAppSettings();
   m_snapEnabled = settings.GetEditor_SnapEnabled();
   m_snapTarget = settings.GetEditor_SnapTarget();
   m_snapMove = settings.GetEditor_SnapMove();
   m_snapRotate = settings.GetEditor_SnapRotate();
   m_snapScale = settings.GetEditor_SnapScale();
   m_snapAffectMove = settings.GetEditor_SnapAffectMove();
   m_snapAffectRotate = settings.GetEditor_SnapAffectRotate();
   m_snapAffectScale = settings.GetEditor_SnapAffectScale();
}

void EditorUI::SaveSnapPreferences() const
{
   Settings &settings = g_settingsService.GetAppSettings();
   settings.SetEditor_SnapEnabled(m_snapEnabled, false);
   settings.SetEditor_SnapTarget(m_snapTarget, false);
   settings.SetEditor_SnapMove(m_snapMove, false);
   settings.SetEditor_SnapRotate(m_snapRotate, false);
   settings.SetEditor_SnapScale(m_snapScale, false);
   settings.SetEditor_SnapAffectMove(m_snapAffectMove, false);
   settings.SetEditor_SnapAffectRotate(m_snapAffectRotate, false);
   settings.SetEditor_SnapAffectScale(m_snapAffectScale, false);
}

bool EditorUI::IsSnapActive(const SnapKind kind) const
{
   // Ctrl inverts the snapping while it is held (Blender)
   if (m_snapEnabled == ImGui::GetIO().KeyCtrl)
      return false;
   switch (kind)
   {
   case SnapKind::Move: return m_snapAffectMove;
   case SnapKind::Rotate: return m_snapAffectRotate;
   case SnapKind::Scale: return m_snapAffectScale;
   }
   return false;
}

void EditorUI::ToggleSnapping()
{
   m_snapEnabled = !m_snapEnabled;
   g_settingsService.GetAppSettings().SetEditor_SnapEnabled(m_snapEnabled, false);
   Report(m_snapEnabled ? "Snapping on" : "Snapping off");
}

EditorUI::~EditorUI()
{
   if (ImGuiContext *const context = ImGui::GetCurrentContext())
      context->ConfigNavEnableTabbing = true;
   // The table outlives the editor (Player deletes the UI first)
   m_fileOp.reset(); // Wait for a file operation in progress, its result being discarded with the session
   m_refImage.Release(*this);
   RestorePartsVisibility();
}

EditorUI::AsyncFileOp::~AsyncFileOp()
{
   if (worker.joinable())
      worker.join();
   if (table != nullptr)
      table->Release();
}

void EditorUI::Open()
{
   if (m_isOpened)
      return;
   m_isOpened = true;
   m_boxSelectActive = false;
   m_renderer->DisableStaticPrePass(true);
   m_player->SetPlayState(false); // Suspend play while editing

   // Drop input events queued while the editor was closed (e.g. the key press that ended a play test), so they don't trigger editor shortcuts
   ImGui::GetIO().ClearEventsQueue();
   ImGui::GetIO().ClearInputKeys();

   if (IsInspectMode())
   {
      // Inspecting a live copy (play testing): keep the game playing, use the normal player camera view with default shading
      m_camMode = ViewMode::PreviewCam;
      m_shadeMode = Renderer::ShadeMode::Default;
   }
   else
   {
      // Enter with the user camera setup as an orthographic top view fitted on the playfield bounds, rendered as masked wireframe
      m_camMode = ViewMode::EditorCam;
      m_perspectiveCam = false;
      m_predefinedView = PredefinedView::Top;
      m_shadeMode = Renderer::ShadeMode::Wireframe;
      // The playfield fit needs a valid display size and chrome height which may not be known yet (editor opened during startup): defer to the first rendered frame
      m_fitPlayfieldCamera = true;
   }
}

void EditorUI::Close()
{
   if (!m_isOpened)
      return;
   EndModalTransform(false); // A running modal ends here (its text input too)
   m_isOpened = false;
   m_flyMode = false;
   if (ImGuiContext *const context = ImGui::GetCurrentContext())
      context->ConfigNavEnableTabbing = true; // Overridden each frame while the editor is open (see RenderUI)
   ImGuizmo::Enable(false); // Clears a gizmo state left by a drag in progress
   ImGuizmo::Enable(true);
   SDLInputHandler::ClearSwallowedMouseButtons(); // The editor stops drawing: no nav cube press can stay swallowed
   m_deferredStop = -1;
   m_gizmoDragActive = false;
   FlushRubberDrag();
   FlushDrag();
   m_gizmoDragSelection.clear();
   ExitPointEditMode(false);
   RestorePartsVisibility();
   m_refImage.FlushPending(*this);
   m_inspectionModal.Close();
   m_pbrConvertModal.Close();
   m_selectRename.Close();
   m_partLibrary.CancelPlacement(); // An armed library placement does not survive the editor closing
   m_partLibrary.OnEditorClosed(); // The library's lightbox and thumbnails do not survive the editor closing (view slot, scratch tables)
   m_renderer->DisableStaticPrePass(false);
   m_renderer->SetShadeMode(Renderer::ShadeMode::Default);
   m_renderer->SetViewShade(ViewShadeOptions {});
   EndLocalViews(); // A play session (or the editor closing) ends the Local Views
   m_measure.SetActive(*this, false); // The rulers stay
}

void EditorUI::SetTable(PinTable *const table)
{
   SDLInputHandler::ClearSwallowedMouseButtons();
   assert(table != nullptr);
   // Only tables of a same base table / live copy pair are supported (guaranteed by the player's SetTable)
   PinTable *const baseTable = m_table->m_liveBaseTable ? m_table->m_liveBaseTable : m_table;
   assert(table == baseTable || table->m_liveBaseTable == baseTable);
   m_table = table;

   // Drop all the state bound to the previous table's editables (undo is bound to the base table which outlives the switch)
   if (m_pointEditPart)
      m_pointEditPart->SetPointEditContext(nullptr);
   m_pointEditPart.reset();
   m_pointEditCenter = false;
   m_centerSelected = false;
   m_pointSel.clear();
   m_pointDragPending = false;
   m_pointDragActive = false;
   m_savedSelection = Selection();
   m_savedMultiSel.clear();
   m_savedOutlinerAnchor = nullptr;
   ClearSelection();
   m_dynamicParts.clear();
   m_editables.clear();
   m_editableMap.clear();
   m_partGroupUseHistory.clear();
   m_lastUndoPart = nullptr;
   m_addPartType = eItemInvalid;
   m_partLibrary.CancelPlacement();
   m_boxSelectActive = false;
   m_selectRename.Close();
   m_measure.SetActive(*this, false);
   m_measure.SetTable(*this, table->m_liveBaseTable != nullptr ? table->m_liveBaseTable : table); // Rulers are positions in one table (they stay over a play test of it)
   EndLocalViews(); // They belong to the previous table's parts
}

void EditorUI::PlayTest()
{
   if (IsInspectMode())
      return;
   m_refImage.FlushPending(*this); // A reference image setting still due goes into the table's settings before the copy (Close() then has nothing left to write)
   // The editor visibility overrides must not leak into the duplicated table used for the play session
   RestorePartsVisibility();
   PinTable *const liveTable = m_table->CopyForPlay();
   m_player->SetTable(liveTable, Player::TableTransition::Stack);
}

void EditorUI::SaveTable()
{
   PLOGI << "SaveTable requested (inspect=" << IsInspectMode() << ", locked=" << m_table->IsLocked() << ", fileOpBusy=" << (m_fileOp != nullptr)
         << ", file=" << PathToUTF8(m_table->m_filename) << ')';
   if (m_table->IsLocked() || m_fileOp != nullptr)
   {
      PLOGW << "SaveTable ignored: " << (m_table->IsLocked() ? "table is locked" : "a file operation is in progress");
      m_liveUI.PushNotification(m_table->IsLocked() ? "Nothing saved: the table is locked"s : "Nothing saved: a file operation is still in progress"s, 8000);
      return;
   }
   if (IsDragInProgress())
   {
      // A drag or modal transform holds an uncommitted value in the parts: it would reach the file while Esc can still cancel it in the session
      m_liveUI.PushNotification("Nothing saved: finish the move first"s, 5000);
      return;
   }
   for (IEditable *const part : m_drag.pendingPhysics) // A save during a drag: the file gets meshes that match the points
      if (part->GetItemType() == eItemPrimitive && std::ranges::find(m_table->GetParts(), part) != m_table->GetParts().end())
         static_cast<Primitive *>(part)->FlushGenDrag();
   PinTable *saveTable = m_table;
   if (IsInspectMode())
   {
      // Inspection session: write the startup table, after copying back what was edited in the 'Live' tabs
      saveTable = m_table->m_liveBaseTable;
      if (saveTable->m_filename.empty())
      {
         m_liveUI.PushNotification("Nothing saved: the table has no file yet, use Save As from the editor"s, 8000);
         return;
      }
      int copied = 0, skipped = 0;
      for (Material *const live : m_liveEditedMaterials)
         if (Material *const startup = m_table->GetStartupFromLive<Material>(live))
         {
            const string name = startup->m_name;
            *startup = *live;
            startup->m_name = name;
            copied++;
         }
         for (IEditable *const live : m_liveEditedParts)
         {
            IEditable *const startup = m_table->GetStartupFromLive<IEditable>(live);
            if (startup == nullptr)
               continue;
            if (live->GetItemType() == eItemPrimitive && startup->GetItemType() == eItemPrimitive)
               static_cast<Primitive *>(startup)->m_d = static_cast<Primitive *>(live)->m_d;
            else if (live->GetItemType() == eItemLight && startup->GetItemType() == eItemLight)
               static_cast<Light *>(startup)->m_d = static_cast<Light *>(live)->m_d;
            else
            {
               skipped++;
               continue;
            }
            copied++;
         }
         m_liveEditedMaterials.clear();
         m_liveEditedParts.clear();
         string msg = std::format("Saved the startup table to '{}' ({} live edit(s) copied in", PathToUTF8(saveTable->m_filename), copied);
         if (skipped > 0)
            msg += std::format(", {} edited part(s) of other types NOT copied: use the save icon next to their fields", skipped);
         msg += ")";
         m_liveUI.PushNotification(msg, 10000);
      }
      else if (m_table->m_filename.empty())
      {
         SaveTableAs();
         return;
      }
   auto op = std::make_unique<AsyncFileOp>();
   op->type = AsyncFileOp::Type::Save;
   op->saveTable = saveTable;
   op->filename = saveTable->m_filename;
   op->displayPath = PathToUTF8(saveTable->m_filename);
   StartFileOp(std::move(op));
      }

void EditorUI::SaveTableAs()
{
   if (IsInspectMode() || m_table->IsLocked() || m_player->m_playfieldWnd == nullptr)
      return;
   m_pendingSaveAsPath = std::make_shared<string>();
   m_pendingSaveAsFolder = false;
   std::filesystem::path defaultLocation = m_table->m_filename;
   const bool isPack = !defaultLocation.empty() && JSONSerializer::IsPack(defaultLocation);
   const SDL_DialogFileFilter tableFilter = { "Visual Pinball Tables", "vpx" };
   const SDL_DialogFileFilter packFilter = { "Visual Pinball Packs", "vpz" };
   const SDL_DialogFileFilter filters[] = { isPack ? packFilter : tableFilter, isPack ? tableFilter : packFilter };
   if (defaultLocation.empty())
      defaultLocation = PathFromString(m_table->GetSettings().GetRecentDir_LoadDir()) / "new_table.vpx";
   else if (lowerCase(PathToUTF8(defaultLocation.extension())) != ".vpz"s)
   {
      std::error_code ec;
      if (!std::filesystem::is_directory(defaultLocation, ec))
         defaultLocation.replace_extension(".vpx");
   }
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   SDL_ShowSaveFileDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingSaveAsPath), //
      m_player->m_playfieldWnd->GetCore(), filters, static_cast<int>(std::size(filters)), location.empty() ? nullptr : location.c_str());
}

void EditorUI::SaveTableAsPackFolder()
{
   if (IsInspectMode() || m_table->IsLocked() || m_player->m_playfieldWnd == nullptr)
      return;
   m_pendingSaveAsPath = std::make_shared<string>();
   m_pendingSaveAsFolder = true;
   const std::filesystem::path defaultLocation = m_table->m_filename.empty() ? PathFromString(m_table->GetSettings().GetRecentDir_LoadDir()) : m_table->m_filename.parent_path();
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   SDL_ShowOpenFolderDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingSaveAsPath), //
      m_player->m_playfieldWnd->GetCore(), location.empty() ? nullptr : location.c_str(), false);
}

void EditorUI::LoadTable() { LoadTableDialog(false); }

void EditorUI::LoadTableFolder() { LoadTableDialog(true); }

void EditorUI::LoadTableDialog(const bool folder)
{
   if (IsInspectMode() || m_player->m_playfieldWnd == nullptr)
      return;
   // Loading another table discards the edited table's unsaved changes: ask for confirmation first
   m_pendingLoadFolder = folder;
   if (m_table->GetDirtyState() != SaveDirtyState::Clean)
   {
      m_pendingNewTable.reset();
      m_pendingClose.reset();
      m_confirmLoadTable = true;
      return;
   }
   ShowLoadTableDialog(folder);
}

void EditorUI::RequestClose(const int closeState)
{
   // Stopping a play test pops the table under the async save of the base table: refused for every caller (Stop, X, Shift+F5, OS window close)
   if (IsInspectMode() && m_fileOp != nullptr)
   {
      if (m_deferredStop != Player::CS_CLOSE_APP) // A Quit is the stronger request: a later Stop does not downgrade it
         m_deferredStop = closeState; // Replayed once the save has finished
      Report("Save in progress: the play test will stop when it finishes");
      return;
   }
   // Only closing a full editor session loses the edited table (a live edit started from the Win32 editor or a play test gets back to its editor)
   if (m_player->IsEditorMode() && !IsInspectMode() && m_table->GetDirtyState() != SaveDirtyState::Clean)
   {
      m_pendingNewTable.reset();
      m_pendingClose = closeState;
      m_confirmLoadTable = true;
      return;
   }
   m_player->SetCloseState(static_cast<Player::CloseState>(closeState));
}

void EditorUI::ShowLoadTableDialog(const bool folder)
{
   m_pendingLoadPath = std::make_shared<string>();
   // Start on the edited table's file (right folder, preselected file), falling back to the recent load dir
   std::filesystem::path defaultLocation = m_table->m_filename;
   if (defaultLocation.empty())
      defaultLocation = PathFromString(m_table->GetSettings().GetRecentDir_LoadDir()) / "";
   else if (folder)
      defaultLocation = defaultLocation.parent_path();
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   if (folder)
   {
      SDL_ShowOpenFolderDialog(
         [](void *userdata, const char *const *filelist, int filter)
         {
            auto *res = static_cast<std::shared_ptr<string> *>(userdata);
            if (filelist != nullptr && filelist[0] != nullptr)
               **res = filelist[0];
            delete res;
         },
         new std::shared_ptr<string>(m_pendingLoadPath), //
         m_player->m_playfieldWnd->GetCore(), location.empty() ? nullptr : location.c_str(), false);
      return;
   }
   const SDL_DialogFileFilter filters[] = { { "Visual Pinball Tables", "vpx;vpt;vpz" } };
   SDL_ShowOpenFileDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingLoadPath), //
      m_player->m_playfieldWnd->GetCore(), filters, 1, location.empty() ? nullptr : location.c_str(), false);
}

void EditorUI::NewTable(const NewTableTemplate templateType)
{
   if (IsInspectMode())
      return;
   // Creating a new table discards the edited table's unsaved changes: ask for confirmation first
   if (m_table->GetDirtyState() != SaveDirtyState::Clean)
   {
      m_pendingNewTable = templateType;
      m_pendingClose.reset();
      m_confirmLoadTable = true;
      return;
   }
   LoadTableTemplate(templateType);
}

void EditorUI::LoadTableTemplate(const NewTableTemplate templateType)
{
   if (m_fileOp != nullptr)
      return;
   string path;
   switch (templateType)
   {
   case NewTableTemplate::Stripped: path = "strippedTable.vpz"s; break;
   case NewTableTemplate::Example: path = "exampleTable.vpz"s; break;
   case NewTableTemplate::LightSeq: path = "lightSeqTable.vpz"s; break;
   case NewTableTemplate::Blank:
   default: path = "blankTable.vpz"s;
   }
   auto op = std::make_unique<AsyncFileOp>();
   op->type = AsyncFileOp::Type::NewTable;
   op->filename = g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets, path);
   op->displayPath = path;
   StartFileOp(std::move(op));
}

void EditorUI::StartFileOp(std::unique_ptr<AsyncFileOp> op)
{
   assert(m_fileOp == nullptr);
   switch (op->type)
   {
   case AsyncFileOp::Type::Save:
   {
      if (!IsInspectMode())
         RestorePartsVisibility(); // Save the authored visibility, not the editor display state
      AsyncFileOp *const fileOp = op.get();
      PinTable *const table = op->saveTable != nullptr ? op->saveTable : m_table;
      m_pinned.SaveTo(table); // The pinned properties list is saved with the table (table.json of packs)
      op->worker = std::thread(
         [fileOp, table]()
         {
            fileOp->result = table->Save(fileOp->feedback);
            fileOp->completed = true;
         });
      break;
   }
   case AsyncFileOp::Type::Load:
   case AsyncFileOp::Type::NewTable:
   {
      AsyncFileOp *const fileOp = op.get();
      op->worker = std::thread(
         [fileOp]()
         {
            CComObject<PinTable> *table;
            CComObject<PinTable>::CreateInstance(&table);
            table->AddRef();
            if (fileOp->type == AsyncFileOp::Type::NewTable)
            {
               // Same initial table setup as the Win32 editor (WinEditor::OpenNewTable)
               table->m_glassTopHeight = table->m_glassBottomHeight = 210;
               for (int i = 0; i < 16; i++)
                  table->m_rgcolorcustom[i] = RGB(0, 0, 0);
            }
            const HRESULT hr = table->LoadGameFromFilename(fileOp->filename, fileOp->feedback);
            // Same as the Win32 editor: hash/corrupt content errors still keep the loaded table
            if (SUCCEEDED(hr) || (hr == APPX_E_BLOCK_HASH_INVALID) || (hr == APPX_E_CORRUPT_CONTENT))
            {
               if (fileOp->type == AsyncFileOp::Type::NewTable)
               {
                  table->m_title = "New Table"s;
                  table->GetSettings().SetIniPath(std::filesystem::path());
                  table->m_filename.clear();
               }
               fileOp->table = table;
            }
            else
               table->Release();
            fileOp->result = hr;
            fileOp->completed = true;
         });
      break;
   }
   }
   m_fileOp = std::move(op);
}

void EditorUI::FinishFileOp()
{
   AsyncFileOp *const op = m_fileOp.get();
   const HRESULT result = op->result;
   switch (op->type)
   {
   case AsyncFileOp::Type::Load:
   case AsyncFileOp::Type::NewTable:
      if (op->table != nullptr)
      {
         if (op->type == AsyncFileOp::Type::Load)
            g_settingsService.GetAppSettings().SetRecentDir_LoadDir(PathToString(op->filename.parent_path()), false);
         PinTable *const table = op->table;
         op->table = nullptr;
         m_player->SetTable(table, Player::TableTransition::Replace); // The loaded table reference is adopted by the player
      }
      else
         m_liveUI.PushNotification(op->type == AsyncFileOp::Type::NewTable ? "Failed to create new table from '"s + op->displayPath : "Failed to load table '"s + op->displayPath, 10000);
      break;

   case AsyncFileOp::Type::Save:
      // Saves must always report their result: a failed save was previously silent (and looked like a successful one)
      if (FAILED(result))
      {
         PLOGE << "Save failed (hr=" << std::hex << static_cast<unsigned long>(result) << std::dec << ") for " << op->displayPath;
         m_liveUI.PushNotification("SAVE FAILED: '" + op->displayPath + "'", 15000);
         if (m_deferredStop >= 0)
         {
            m_deferredStop = -1; // Never quit or stop on top of a failed save
            Report("Save failed: close cancelled");
         }
      }
      else
      {
         PLOGI << "Save done: " << op->displayPath;
         Report("Saved " + PathToUTF8(op->filename.filename()));
         if (!IsInspectMode()) // inspect mode already reported when starting the save
            m_liveUI.PushNotification("Saved " + op->displayPath, 5000);
      }
      if (SUCCEEDED(result))
      {
         m_undo.SetCleanPoint(SaveDirtyState::Clean);
         m_lastOp.type = LastOperation::Type::None; // Adjusting after a save would change the table without marking it dirty
         // Start a new undo record on next edit (instead of merging it with the previous one), so that the table gets marked dirty again
         m_lastUndoPart = nullptr;
         m_lastUndoId = 0;
         if (op->restoreOnFailure)
            g_settingsService.GetAppSettings().SetRecentDir_LoadDir(PathToString(m_table->m_filename.parent_path()), false);
      }
      else if (op->restoreOnFailure)
      {
         m_table->m_filename = op->previousFilename;
         m_table->m_title = op->previousTitle;
      }
      break;
   }
   m_fileOp.reset();
}

void EditorUI::RenderFileOpModal()
{
   if (m_fileOp == nullptr)
      return;
   const char *action;
   switch (m_fileOp->type)
   {
   case AsyncFileOp::Type::Load: action = "Loading table"; break;
   case AsyncFileOp::Type::NewTable: action = "Creating table"; break;
   default: action = "Saving table"; break;
   }
   const string title = action + "###FileOp"s;
   ImGui::OpenPopup(title.c_str());
   if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
   {
      if (!m_fileOp->displayPath.empty())
         ImGui::TextDisabled("%s", m_fileOp->displayPath.c_str());
      const unsigned int length = m_fileOp->feedback.length.load();
      const unsigned int progress = m_fileOp->feedback.progress.load();
      ImGui::ProgressBar(length != 0 ? static_cast<float>(progress) / length : 0.f, ImVec2(280.f * m_liveUI.GetDPI(), 0.f));
      if (length != 0)
         ImGui::Text("%u / %u", progress, length);
      ImGui::EndPopup();
   }
}

void EditorUI::ResetCameraFromPlayer()
{
   // Try to setup editor camera to match the used one, but only mostly since the EditorUI does not have some view setup features like off-center, ...
   m_camView = Matrix3D::MatrixScale(1.f, 1.f, -1.f) * m_renderer->GetMVP().GetView(0) * Matrix3D::MatrixScale(1.f, -1.f, 1.f);
}

void EditorUI::SetBackdropCamera()
{
   // Setup the editor camera to frame the desktop backdrop (a flat rectangle in EDITOR_BG_WIDTH x EDITOR_BG_HEIGHT coordinates, Y axis going down)
   const ImGuiIO &io = ImGui::GetIO();
   m_camDistance = 0.5f * max((float)EDITOR_BG_HEIGHT, (float)EDITOR_BG_WIDTH * io.DisplaySize.y / io.DisplaySize.x);
   const vec3 eye((float)EDITOR_BG_WIDTH * 0.5f, (float)EDITOR_BG_HEIGHT * 0.5f, -m_camDistance);
   const vec3 at((float)EDITOR_BG_WIDTH * 0.5f, (float)EDITOR_BG_HEIGHT * 0.5f, 0.f);
   constexpr vec3 up { 0.f, -1.f, 0.f };
   m_camView = Matrix3D::MatrixLookAtRH(eye, at, up);
}

void EditorUI::ToggleSidebar() { m_showSidebar = !m_showSidebar; }

void EditorUI::RenderSplitters()
{
   // Draggable borders between the areas (Blender's area splitters): one grab strip per split of the layout tree whose two children are
   // visible, then the one of the N-panel at the left edge of the sidebar
   if (m_table->IsLocked())
      return;
   if (!m_areaRectsValid)
      ComputeAreaRects();
   const float dpi = m_liveUI.GetDPI();
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   const ImGuiIO &io = ImGui::GetIO();
   if (m_showSidebar && m_viewportVisible && !m_sidebarWasShown)
      m_raiseAreaWindows = true; // The N-panel window was just created, above the corner zones
   m_sidebarWasShown = m_showSidebar && m_viewportVisible;
   if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
      m_splitDragNode = -1; // Splitter drag over
   const bool raiseNow = ShouldRaiseAreaWindows(); // Deferred while a popup is open: the raised strips would cover it
   // Returns whether the strip is being dragged. The strip is a window like the former fixed splitters (same flags and cursor), the highlight
   // is drawn on hover / drag, and the seam line only between areas that are not the 3D view (the areas have no border of their own)
   const auto strip = [&](const char *id, const AreaRect &grab, const bool vertical, const bool seam, bool &activated, bool *rightClicked = nullptr) -> bool
   {
      ImGui::SetNextWindowPos(ImVec2(grab.x0, grab.y0));
      ImGui::SetNextWindowSize(ImVec2(grab.Width(), grab.Height()));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f)); // The default 32 px minimum widened the grab strip over the areas' edges (the Properties tab strip got no clicks)
      ImGui::Begin(id, nullptr,
         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus); // Never raised by a click: the corner zones, created after the strips, stay above them
      if (raiseNow)
         ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); // After a layout change (new windows are created on top): strips, then corners
      ImGui::InvisibleButton("##grab", ImVec2(grab.Width(), grab.Height()));
      activated = ImGui::IsItemActivated();
      if (rightClicked != nullptr)
         *rightClicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
      const bool active = ImGui::IsItemActive();
      const float mid = vertical ? 0.5f * (grab.x0 + grab.x1) : 0.5f * (grab.y0 + grab.y1);
      ImDrawList *const drawList = ImGui::GetWindowDrawList();
      if (seam)
      {
         if (vertical)
            drawList->AddLine(ImVec2(mid, grab.y0), ImVec2(mid, grab.y1), EditorTheme::Rgb(22, 22, 22), 3.f * dpi);
         else
            drawList->AddLine(ImVec2(grab.x0, mid), ImVec2(grab.x1, mid), EditorTheme::Rgb(22, 22, 22), 3.f * dpi);
      }
      if (ImGui::IsItemHovered() || active)
      {
         ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
         if (vertical)
            drawList->AddLine(ImVec2(mid, grab.y0), ImVec2(mid, grab.y1), ImGui::GetColorU32(ImGuiCol_SeparatorHovered), 2.f);
         else
            drawList->AddLine(ImVec2(grab.x0, mid), ImVec2(grab.x1, mid), ImGui::GetColorU32(ImGuiCol_SeparatorHovered), 2.f);
      }
      ImGui::End();
      ImGui::PopStyleVar(2);
      return active;
   };

   for (const SplitterRect &split : m_splitterRects)
   {
      const AreaNode *const node = m_areas.Find(split.nodeId);
      if (node == nullptr)
         continue;
      const bool vertical = split.axis == SplitAxis::Vertical;
      const bool seam = true; // Blender: a 3 px dark seam on every edge, beside the 3D view too
      char name[32];
      snprintf(name, sizeof(name), "##Split%d", split.nodeId);
      bool activated = false, menuRequested = false;
      const bool active = strip(name, split.grab, vertical, seam, activated, &menuRequested);
      if (menuRequested && m_areaModal == AreaModalKind::None && CanUseAreaTools())
      {
         // Right click on a divider: Blender's area edge menu (Vertical / Horizontal Split, Join Areas, Swap Areas)
         m_edgeMenuNode = split.nodeId;
         m_edgeMenuRequest = true;
         m_edgeMenuPos = io.MousePos;
      }
      if (activated)
      {
         m_splitDragNode = split.nodeId;
         m_splitDragPos = split.pos; // Unrounded: slow drags do not lose their sub pixel moves to the whole pixel dividers
      }
      if (active && m_splitDragNode == split.nodeId)
      {
         m_splitDragPos += vertical ? io.MouseDelta.x : io.MouseDelta.y;
         float lo = split.minPos, hi = split.maxPos;
         if (vertical)
         {
            // Same limit as before: the size of the area is at most 45% of the window
            const float cap = max(AreaLayout::kPanelMinW, 0.45f * viewport->Size.x / dpi) * dpi;
            if (split.fixed == 1)
               lo = max(lo, split.bounds.x1 - cap);
            else
               hi = min(hi, split.bounds.x0 + cap);
            if (lo > hi)
               lo = hi = split.pos;
         }
         m_splitDragPos = clamp(m_splitDragPos, lo, hi);
         m_areas.SetSplitSize(split.nodeId, split.SizeForPos(m_splitDragPos, dpi));
      }
   }
   if (m_showSidebar && m_viewportVisible)
   {
      const float half = 3.f * dpi;
      const AreaRect grab { m_viewportContentRect.x1 - half, m_viewportRect.y0, m_viewportContentRect.x1 + half, m_viewportRect.y1 };
      bool activated = false;
      if (strip("##SplitSidebar", grab, true, false, activated))
         m_sidebarWidth = clamp(m_sidebarWidth - io.MouseDelta.x / dpi, 140.f, 0.45f * viewport->Size.x / dpi);
   }
   // Corner zones above the strips, then the edge menu and the modes it starts (their input catcher is the last window)
   RenderAreaCorners();
   RenderAreaEdgeMenu();
   RenderAreaModal();
   if (raiseNow)
      m_raiseAreaWindows = false;
}

const char *EditorUI::AreaEditorName(const AreaEditor type)
{
   switch (type)
   {
   case AreaEditor::Outliner: return "Outliner";
   case AreaEditor::Properties: return "Properties";
   case AreaEditor::Script: return "Script";
   case AreaEditor::PartLibrary: return "Part Library";
   case AreaEditor::Viewport: return "3D Viewport";
   case AreaEditor::CameraPreview: return "Camera / 3D View";
   case AreaEditor::TableCheck: return "Table Check";
   case AreaEditor::Count: break;
   }
   return "";
}

const char *EditorUI::AreaEditorIcon(const AreaEditor type)
{
   switch (type)
   {
   case AreaEditor::Outliner: return ICON_LC_LIST;
   case AreaEditor::Properties: return ICON_LC_SLIDERS_HORIZONTAL;
   case AreaEditor::Script: return ICON_LC_CODE_XML;
   case AreaEditor::PartLibrary: return ICON_LC_BOX;
   case AreaEditor::CameraPreview: return ICON_LC_EYE;
   case AreaEditor::TableCheck: return ICON_LC_TRIANGLE_ALERT;
   case AreaEditor::Viewport:
   case AreaEditor::Count: break;
   }
   return "";
}

void EditorUI::SetAreaEditor(const int leafId, const AreaEditor type)
{
   if (type >= AreaEditor::Count || leafId < 0)
      return;
   PendingAreaOp op;
   op.kind = PendingAreaOp::Kind::SetType;
   op.leaf = leafId;
   op.type = type;
   QueueAreaOp(op);
}

void EditorUI::SetAreaLayout(const AreaLayout &layout)
{
   m_areas = layout;
   m_pendingAreaOps.clear();
   m_maximizedLeafId = -1;
   m_splitDragNode = -1;
   CancelAreaInteraction(false);
   m_hoverLeaf = -1;
   m_raiseAreaWindows = true;
   if (m_areas.IsHosted(AreaEditor::PartLibrary))
      m_partLibrary.Show(); // Rescan the library folders
   BindAreaPanels(); // The areas of the layout get their panels: a workspace shown again takes back its parked ones, a new area a panel loading the state saved with it
   ComputeAreaRects();
}

void EditorUI::BindAreaPanels()
{
   // Leaf ids of the areas another workspace holds (its saved tree): their panels are parked while that workspace is not shown, and come back
   // with all their state (search, tab, tree expansion, scroll: the same ImGui window) when it is. Leaf ids are never reused, so a parked panel
   // can only come back to its own area
   std::set<int> otherWorkspaces;
   for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
      if (i != m_pinned.GetActiveWorkspace())
         for (const AreaNode &node : m_pinned.GetWorkspace(i).layout.Nodes())
            if (!node.isSplit)
               otherWorkspaces.insert(static_cast<int>(node.id));
   // The panel of an area that left the tree is parked if another workspace holds it, else dropped (closed); a re-typed area's panel is dropped.
   // A new area gets its parked panel back, else a fresh one loading the state saved in its leaf
   const auto bind = [this, &otherWorkspaces](auto &panels, auto &parked, const AreaType type)
   {
      for (auto it = panels.begin(); it != panels.end();)
      {
         const AreaNode *const node = m_areas.Find(it->first);
         if (node != nullptr && !node->isSplit && node->type == type)
            ++it;
         else
         {
            if (node == nullptr && otherWorkspaces.contains(it->first))
               parked[it->first] = std::move(it->second);
            it = panels.erase(it);
         }
      }
      for (const AreaNode &node : m_areas.Nodes())
         if (!node.isSplit && node.type == type && !panels.contains(static_cast<int>(node.id)))
         {
            auto &panel = panels[static_cast<int>(node.id)];
            if (const auto p = parked.find(static_cast<int>(node.id)); p != parked.end())
            {
               panel = std::move(p->second);
               parked.erase(p);
               continue;
            }
            panel = std::make_unique<typename std::decay_t<decltype(panel)>::element_type>(*this);
            panel->BindArea(static_cast<int>(node.id), node.state);
         }
      // Then the parked panels no workspace holds any more are dropped (after the shown workspace took its own back)
      std::erase_if(parked, [&otherWorkspaces](const auto &entry) { return !otherWorkspaces.contains(entry.first); });
   };
   bind(m_outliners, m_parkedOutliners, AreaType::Outliner);
   bind(m_propertiesAreas, m_parkedProperties, AreaType::Properties);
   // A split's new area: the split area's live state (search, filters, tab, the pinned object itself), beyond what its leaf's saved state gave it
   for (const AreaCopy &copy : m_areaCopies)
   {
      if (OutlinerPanel *const to = GetOutlinerArea(copy.to))
         if (const OutlinerPanel *const from = GetOutlinerArea(copy.from))
            to->CopyAreaState(*from);
      if (PropertiesPanel *const to = GetPropertiesArea(copy.to))
         if (const PropertiesPanel *const from = GetPropertiesArea(copy.from))
            to->CopyAreaState(*from);
   }
   m_areaCopies.clear();
}

OutlinerPanel *EditorUI::GetOutlinerArea(const int leafId) const
{
   const auto it = m_outliners.find(leafId);
   return it != m_outliners.end() ? it->second.get() : nullptr;
}

void EditorUI::RefreshParkedStates(AreaLayout &layout) const
{
   // The saved tree of a workspace not shown has the states of when it was left: its parked panels may have changed since (a pinned object renamed)
   vector<int> leaves;
   for (const AreaNode &node : layout.Nodes())
      if (!node.isSplit)
         leaves.push_back(static_cast<int>(node.id));
   for (const int id : leaves)
   {
      if (const auto it = m_parkedOutliners.find(id); it != m_parkedOutliners.end())
         layout.SetState(id, it->second->SaveAreaState());
      // A pin to an object the current table does not have is kept as saved: during a play test the table shown is the live copy, while the
      // panel was parked pinned to an object of the startup table that is being saved (a pin to a deleted object resolves as not found on load)
      if (const auto it = m_parkedProperties.find(id); it != m_parkedProperties.end() && it->second->IsPinLive())
         layout.SetState(id, it->second->SaveAreaState());
   }
}

PropertiesPanel *EditorUI::GetPropertiesArea(const int leafId) const
{
   const auto it = m_propertiesAreas.find(leafId);
   return it != m_propertiesAreas.end() ? it->second.get() : nullptr;
}

void EditorUI::SyncAreaStates()
{
   for (const AreaNode &node : m_areas.Nodes())
   {
      if (node.isSplit)
         continue;
      if (node.type == AreaType::Outliner)
      {
         if (const OutlinerPanel *const panel = GetOutlinerArea(static_cast<int>(node.id)))
            m_areas.SetState(static_cast<int>(node.id), panel->SaveAreaState());
      }
      else if (node.type == AreaType::Properties)
      {
         if (const PropertiesPanel *const panel = GetPropertiesArea(static_cast<int>(node.id)))
            m_areas.SetState(static_cast<int>(node.id), panel->SaveAreaState());
      }
   }
}

AreaLayout EditorUI::GetAreaLayoutWithState() const
{
   AreaLayout layout = m_areas;
   for (const AreaNode &node : m_areas.Nodes())
   {
      if (node.isSplit)
         continue;
      if (node.type == AreaType::Outliner)
      {
         if (const OutlinerPanel *const panel = GetOutlinerArea(static_cast<int>(node.id)))
            layout.SetState(static_cast<int>(node.id), panel->SaveAreaState());
      }
      else if (node.type == AreaType::Properties)
      {
         if (const PropertiesPanel *const panel = GetPropertiesArea(static_cast<int>(node.id)))
            layout.SetState(static_cast<int>(node.id), panel->SaveAreaState());
      }
   }
   return layout;
}

const PropertiesPanel *EditorUI::GetJumpPropertiesPanel() const
{
   const PropertiesPanel *first = nullptr;
   for (const LeafRect &leaf : m_leafRects)
   {
      if (!leaf.visible || leaf.type != AreaType::Properties)
         continue;
      const PropertiesPanel *const panel = GetPropertiesArea(leaf.id);
      if (panel == nullptr)
         continue;
      if (!panel->IsPinned())
         return panel;
      if (first == nullptr)
         first = panel;
   }
   return first;
}

PropertiesPanel &EditorUI::GetSelectionProperties(const bool prepare)
{
   // A shown Properties area that follows the selection, prepared on it again here: it was drawn this frame (areas are drawn before the N-panel), except
   // in fly mode or under the inspection modal (areas not drawn) and for a new area, whose curation would be missing or of an older selection
   for (const LeafRect &leaf : m_leafRects)
      if (leaf.visible && leaf.type == AreaType::Properties)
         if (PropertiesPanel *const panel = GetPropertiesArea(leaf.id); panel != nullptr && !panel->IsPinned())
         {
            if (prepare)
               panel->PrepareCuration(); // On the current selection, also when the areas are not drawn (fly mode, inspection) or not yet (a new area); cheap when unchanged
            return *panel;
         }
   if (prepare)
      m_properties.PrepareCuration();
   return m_properties;
}

bool EditorUI::IsAreaEditorShown(const AreaEditor type) const
{
   for (const AreaNode &node : m_areas.Nodes())
      if (!node.isSplit && node.type == type && !node.hidden && (m_maximizedLeafId < 0 || m_maximizedLeafId == static_cast<int>(node.id)))
         return true;
   return false;
}

void EditorUI::ShowAreaEditor(const AreaEditor type)
{
   // Entry point of the former dialogs (script, part library) and of the jumps to the outliner / properties: the area hosting the type
   // is revealed, otherwise the type replaces the editor of its preferred area (Script and Properties: rightmost, Outliner and Part Library: leftmost)
   if (type >= AreaEditor::Count)
      return;
   PendingAreaOp op;
   op.kind = PendingAreaOp::Kind::Show;
   op.type = type;
   QueueAreaOp(op);
}

void EditorUI::ToggleAreaEditor(const AreaEditor type)
{
   if (type >= AreaEditor::Count)
      return;
   PendingAreaOp op;
   op.kind = PendingAreaOp::Kind::Toggle;
   op.type = type;
   QueueAreaOp(op);
}

void EditorUI::ToggleMaximizeArea()
{
   // Blender's Ctrl+Space: the area under the mouse fills the tree (the bars stay), a second press restores the layout. The View menu and the palette
   // call it with the mouse over their own popup: m_hoverLeaf is the area hovered before it opened. Nothing hovered (nor a viewport): the viewport area
   PendingAreaOp op;
   op.kind = PendingAreaOp::Kind::Maximize;
   if (m_maximizedLeafId < 0)
   {
      int leaf = m_hoverLeaf;
      if (leaf < 0 || m_areas.Find(leaf) == nullptr)
         leaf = m_areas.FindLeaf(AreaType::Viewport);
      const AreaNode *const node = leaf >= 0 ? m_areas.Find(leaf) : nullptr;
      if (node == nullptr || node->hidden)
      {
         Report("No area to maximize");
         return;
      }
      op.leaf = leaf;
   }
   QueueAreaOp(op); // leaf -1 restores
}

ImVec2 EditorUI::GetViewportContentCenter() const
{
   if (!m_areaRectsValid)
      ComputeAreaRects();
   if (!m_viewportVisible)
      return ImVec2(0.5f * ImGui::GetIO().DisplaySize.x, 0.5f * ImGui::GetIO().DisplaySize.y);
   return ImVec2(0.5f * (m_viewportContentRect.x0 + m_viewportContentRect.x1), 0.5f * (m_viewportContentRect.y0 + m_viewportContentRect.y1));
}

void EditorUI::WidenArea(const int leafId, const AreaType type)
{
   // Switching the type of an area that is too narrow for the new editor widens it (once, the splitter can shrink it again afterwards)
   const float minimum = type == AreaEditor::Properties ? 300.f : type == AreaEditor::Script ? 400.f : type == AreaEditor::TableCheck ? 320.f : 0.f;
   if (minimum <= 0.f)
      return;
   // The split whose stored size is the width of the area: the nearest vertical ancestor whose fixed child holds the area
   const auto &nodes = m_areas.Nodes();
   const AreaNode *split = nullptr;
   int child = leafId;
   for (int parent = m_areas.ParentId(child); parent >= 0; child = parent, parent = m_areas.ParentId(child))
   {
      const AreaNode *const candidate = m_areas.Find(parent);
      if (candidate != nullptr && candidate->axis == SplitAxis::Vertical && nodes[candidate->child[candidate->fixed]].id == static_cast<uint32_t>(child))
      {
         split = candidate;
         break;
      }
   }
   if (split == nullptr)
      return;
   // Same limit as the splitters (45% of the window), and what the other areas and the sidebar take is kept for the viewport
   const float dpi = m_liveUI.GetDPI();
   float mineX0 = 0.f, mineX1 = 0.f;
   for (const LeafRect &leaf : m_leafRects)
      if (leaf.id == leafId && leaf.visible)
      {
         mineX0 = leaf.rect.x0;
         mineX1 = leaf.rect.x1;
      }
   std::map<float, float> columns; // Left edge -> width of the other visible panel areas (stacked areas count once)
   for (const LeafRect &leaf : m_leafRects)
      if (leaf.visible && leaf.id != leafId && leaf.type != AreaType::Viewport && (mineX1 <= mineX0 || leaf.rect.x0 >= mineX1 || leaf.rect.x1 <= mineX0))
         columns[leaf.rect.x0] = max(columns[leaf.rect.x0], leaf.rect.Width() / dpi);
   float taken = m_showSidebar ? m_sidebarWidth : 0.f;
   for (const auto &column : columns)
      taken += column.second;
   const float limit = max(0.f, 0.45f * ImGui::GetIO().DisplaySize.x / dpi - taken);
   const float target = min(minimum, limit);
   if (split->size < target)
      m_areas.SetSplitSize(static_cast<int>(split->id), target);
}

bool EditorUI::ApplyAreaOp(const PendingAreaOp &op)
{
   switch (op.kind)
   {
   case PendingAreaOp::Kind::SetType:
   {
      const AreaNode *const node = m_areas.Find(op.leaf);
      if (node == nullptr || node->isSplit || node->type == op.type)
         return false;
      // Only this area changes, whether or not other areas show the type (Blender: any number of areas of a type). Only a secondary view
      // picked at its cap takes the type from the first one (AreaLayout::SetType), which then shows the editor this area leaves
      int other = -1;
      if (!m_areas.SetType(op.leaf, op.type, &other))
         return false;
      WidenArea(op.leaf, op.type);
      if (other >= 0)
         if (const AreaNode *const swapped = m_areas.Find(other))
            WidenArea(other, swapped->type);
      if (op.type == AreaEditor::PartLibrary)
         m_partLibrary.Show(); // Rescan the library folders
      return true;
   }
   case PendingAreaOp::Kind::Close:
   {
      if (m_maximizedLeafId == op.leaf)
         m_maximizedLeafId = -1;
      return m_areas.Close(op.leaf);
   }
   case PendingAreaOp::Kind::Show:
   case PendingAreaOp::Kind::Toggle:
   {
      // The areas hosting the type (any number): Toggle hides them all when one is shown, else shows them all; Show reveals the first one unless one is shown
      vector<int> hosting;
      int shownId = -1;
      for (const AreaNode &node : m_areas.Nodes())
         if (!node.isSplit && node.type == op.type)
         {
            hosting.push_back(static_cast<int>(node.id));
            if (!node.hidden && shownId < 0)
               shownId = static_cast<int>(node.id);
         }
      if (!hosting.empty())
      {
         if (op.kind == PendingAreaOp::Kind::Toggle)
         {
            const bool hide = shownId >= 0;
            bool changed = false;
            for (const int id : hosting)
            {
               if (hide && m_maximizedLeafId == id)
                  m_maximizedLeafId = -1;
               changed |= m_areas.SetHidden(id, hide);
            }
            return changed;
         }
         const int id = shownId >= 0 ? shownId : hosting.front();
         if (m_maximizedLeafId >= 0 && m_maximizedLeafId != id)
            m_maximizedLeafId = -1;
         return m_areas.SetHidden(id, false);
      }
      // Not hosted: it replaces the editor of the preferred area. Rects are computed with the hidden areas shown
      const bool right = op.type == AreaEditor::Script || op.type == AreaEditor::Properties || op.type == AreaEditor::TableCheck;
      const bool isTableCheck = op.type == AreaEditor::TableCheck;
      AreaLayout reference = m_areas;
      vector<int> leafIds;
      for (const AreaNode &node : reference.Nodes())
         if (!node.isSplit)
            leafIds.push_back(static_cast<int>(node.id));
      for (const int leafId : leafIds)
         reference.SetHidden(leafId, false);
      vector<LeafRect> leaves;
      vector<SplitterRect> splitters;
      reference.Compute(AreaRect { 0.f, 0.f, 1920.f, 1080.f }, 1.f, 0.f, leaves, splitters);
      int preferred = -1;
      float best = 0.f;
      for (const LeafRect &leaf : leaves)
      {
         if (leaf.type == AreaType::Viewport || (isTableCheck && leaf.type == AreaType::CameraPreview))
            continue; // (the Table Check goes by the right-most area that is not a 3D view)
         const float centre = 0.5f * (leaf.rect.x0 + leaf.rect.x1);
         if (preferred < 0 || (right ? centre > best : centre < best))
         {
            preferred = leaf.id;
            best = centre;
         }
      }
      if (isTableCheck)
      {
         // The Table Check is added beside the others, never in place of a working editor: the right-most area that is not a 3D view is split (new area
         // below it), else the 3D viewport (new area on its right). The new leaf is the one the split returns. With no room at all, the area of a
         // Part Library, Outliner or Script is replaced (in that order, never Properties nor a 3D view), and with none of those the request is refused
         const auto splitInto = [this, &op](const int leaf, const SplitAxis axis, const float size)
         {
            int newLeaf = -1;
            if (!m_areas.Split(leaf, axis, true, size, &newLeaf) || newLeaf < 0)
               return -1;
            return m_areas.SetType(newLeaf, op.type) ? newLeaf : -1; // (the split gave it a copy's type: it becomes the Table Check)
         };
         int fresh = -1;
         if (preferred >= 0)
            fresh = splitInto(preferred, SplitAxis::Horizontal, 320.f);
         if (fresh < 0)
            fresh = splitInto(m_areas.FindLeaf(AreaType::Viewport), SplitAxis::Vertical, AreaLayout::kDefaultPropertiesWidth);
         if (fresh < 0)
         {
            for (const AreaType type : { AreaType::PartLibrary, AreaType::Outliner, AreaType::Script })
            {
               const int target = m_areas.FindLeaf(type);
               if (target >= 0 && m_areas.SetType(target, op.type))
               {
                  fresh = target;
                  m_liveUI.PushNotification("No room for another area: Table Check replaced the "s + AreaEditorName(type) + " editor"s, 6000);
                  break;
               }
            }
         }
         if (fresh < 0)
         {
            m_liveUI.PushNotification("No room for another area: close or join one first"s, 6000);
            return false;
         }
         m_areas.SetHidden(fresh, false);
         if (m_maximizedLeafId >= 0 && m_maximizedLeafId != fresh)
            m_maximizedLeafId = -1;
         WidenArea(fresh, op.type);
         return true;
      }
      if (preferred < 0)
      {
         // No other area: the viewport area is split, the new area goes right (Script, Properties) or left (Outliner, Part Library)
         if (!m_areas.Split(m_areas.FindLeaf(AreaType::Viewport), SplitAxis::Vertical, right, right ? AreaLayout::kDefaultPropertiesWidth : AreaLayout::kDefaultOutlinerWidth))
            return false;
         for (const AreaNode &node : m_areas.Nodes())
            if (!node.isSplit && node.type != AreaType::Viewport)
               preferred = static_cast<int>(node.id);
         if (preferred < 0)
            return false;
      }
      if (!m_areas.SetType(preferred, op.type))
         return false;
      m_areas.SetHidden(preferred, false);
      if (m_maximizedLeafId >= 0 && m_maximizedLeafId != preferred)
         m_maximizedLeafId = -1;
      WidenArea(preferred, op.type);
      if (op.type == AreaEditor::PartLibrary)
         m_partLibrary.Show(); // Rescan the library folders
      return true;
   }
   case PendingAreaOp::Kind::Split:
   {
      m_maximizedLeafId = -1; // The new layout must be visible
      const bool hadPartLibrary = m_areas.IsHosted(AreaEditor::PartLibrary);
      int newLeaf = -1;
      if (!m_areas.Split(op.leaf, op.axis, op.newOnSecondSide, op.size, &newLeaf))
         return false;
      m_areaCopies.push_back({ op.leaf, newLeaf }); // Its panel copies the split area's live state once bound (BindAreaPanels)
      if (!hadPartLibrary && m_areas.IsHosted(AreaEditor::PartLibrary))
         m_partLibrary.Show();
      return true;
   }
   case PendingAreaOp::Kind::Join:
   {
      m_maximizedLeafId = -1;
      if (!m_areas.Join(op.leaf, op.leaf2))
         return false;
      if (m_maximizedLeafId >= 0 && m_areas.Find(m_maximizedLeafId) == nullptr)
         m_maximizedLeafId = -1;
      return true;
   }
   case PendingAreaOp::Kind::Swap: m_maximizedLeafId = -1; return m_areas.Swap(op.leaf);
   case PendingAreaOp::Kind::Maximize:
   {
      const AreaNode *const node = op.leaf >= 0 ? m_areas.Find(op.leaf) : nullptr;
      m_maximizedLeafId = (node != nullptr && !node->isSplit && !node->hidden) ? op.leaf : -1;
      return true;
   }
   }
   return false;
}

void EditorUI::UpdateAreaLayout()
{
   if (m_maximizedLeafId >= 0 && m_areas.Find(m_maximizedLeafId) == nullptr)
      m_maximizedLeafId = -1;
   // Queued layout changes are applied here, before any panel is drawn: no panel disappears between its BeginUndo and its EndUndo / Discard.
   // They wait while a field is being dragged or edited
   if (!m_pendingAreaOps.empty() && !ImGui::IsAnyItemActive())
   {
      const vector<PendingAreaOp> ops = std::move(m_pendingAreaOps);
      m_pendingAreaOps.clear();
      SyncAreaStates(); // A split copies the state of the area it splits
      ComputeAreaRects(); // Widths and sides are evaluated on the current rects
      for (const PendingAreaOp &op : ops)
      {
         const bool applied = ApplyAreaOp(op);
         m_raiseAreaWindows |= applied;
         if (!applied && op.kind == PendingAreaOp::Kind::Split)
            Report("Cannot split this area");
         else if (!applied && op.kind == PendingAreaOp::Kind::Join)
            Report("Cannot join these areas");
      }
      BindAreaPanels(); // New areas get their panels (with their saved state), the panels of the areas that are gone are parked or dropped
   }
   ComputeAreaRects();
   // Area under the mouse, for the maximize toggle: not while a popup (View menu, editor type) or the palette covers the areas
   if (!ImGui::IsPopupOpen(static_cast<ImGuiID>(0), ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && !m_palette.IsOpen())
   {
      const ImVec2 mouse = ImGui::GetMousePos();
      const int under = AreaLayout::LeafAt(m_leafRects, mouse.x, mouse.y);
      if (under >= 0)
         m_hoverLeaf = under;
   }
   if (m_hoverLeaf >= 0 && m_areas.Find(m_hoverLeaf) == nullptr)
      m_hoverLeaf = -1;
}

void EditorUI::ComputeAreaRects() const
{
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   const float dpi = m_liveUI.GetDPI();
   // The tree fills the window between the top bar and the status bar (heights of the last chrome render)
   const float top = viewport->Pos.y + m_chrome.GetTopBarHeight();
   const AreaRect tree { viewport->Pos.x, top, viewport->Pos.x + viewport->Size.x, max(top, viewport->Pos.y + viewport->Size.y - m_chrome.GetStatusBarHeight()) };
   const bool locked = m_table->IsLocked();
   const bool inspection = m_inspectionModal.IsVisible();
   const bool areasShown = !locked && !m_flyMode && !inspection;
   const bool sidebarShown = m_showSidebar && !locked && !inspection;
   m_areaRootH = tree.y1 - tree.y0;
   m_areas.Compute(tree, dpi, (sidebarShown && areasShown) ? m_sidebarWidth : 0.f, m_leafRects, m_splitterRects, m_maximizedLeafId);

   // The viewport area is the window onto the full window 3D render; when no area is drawn (locked table, fly mode, inspection) it is the whole tree
   m_viewportRect = tree;
   m_viewportVisible = true;
   if (areasShown)
   {
      m_viewportVisible = false;
      for (const LeafRect &leaf : m_leafRects)
         if (leaf.type == AreaType::Viewport && leaf.visible)
         {
            m_viewportRect = leaf.rect;
            m_viewportVisible = true;
            break;
         }
      if (!m_viewportVisible)
         m_viewportRect = AreaRect(); // A panel is maximized: nothing of the viewport is shown
   }
   // The N-panel is at the right edge of the viewport area
   m_viewportContentRect = m_viewportRect;
   if (sidebarShown && m_viewportVisible)
      m_viewportContentRect.x1 = max(m_viewportContentRect.x0, m_viewportContentRect.x1 - m_sidebarWidth * dpi);
   m_areaRectsValid = true;

   static const bool logRects = []()
   {
      const char *const value = std::getenv("VPX_AREA_LAYOUT_LOG");
      return value != nullptr && *value != '\0' && *value != '0';
   }();
   if (logRects)
   {
      string text;
      char buffer[160];
      for (const LeafRect &leaf : m_leafRects)
      {
         snprintf(buffer, sizeof(buffer), "%s%s=[%.0f %.0f %.0f %.0f] ", AreaTypeJsonName(leaf.type), leaf.visible ? "" : "(hidden)", leaf.rect.x0, leaf.rect.y0, leaf.rect.x1, leaf.rect.y1);
         text += buffer;
      }
      snprintf(buffer, sizeof(buffer), "content=[%.0f %.0f %.0f %.0f]", m_viewportContentRect.x0, m_viewportContentRect.y0, m_viewportContentRect.x1, m_viewportContentRect.y1);
      text += buffer;
      if (text != m_areaRectsLog)
      {
         m_areaRectsLog = text;
         PLOGI << "AreaLayout: " << text;
      }
   }
}

bool EditorUI::ShouldRaiseAreaWindows() const { return m_raiseAreaWindows && !ImGui::IsPopupOpen(static_cast<ImGuiID>(0), ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }

bool EditorUI::CanUseAreaTools() const
{
   // The corner zones, the edge menu and their modes only exist while nothing else owns the mouse
   if (m_table->IsLocked() || m_flyMode || m_inspectionModal.IsVisible() || m_fileOp != nullptr)
      return false;
   if (m_modal.active || IsDragInProgress() || m_pointEditPart != nullptr)
      return false;
   if (IsInspectMode() && m_player->IsGameRunning())
      return false; // The game owns the keys and the mouse
   return m_maximizedLeafId < 0 && !m_palette.IsOpen() && ImGui::GetTopMostPopupModal() == nullptr;
}

const LeafRect *EditorUI::FindLeafRect(const int leafId) const
{
   for (const LeafRect &leaf : m_leafRects)
      if (leaf.id == leafId && leaf.visible)
         return &leaf;
   return nullptr;
}

bool EditorUI::PlanAreaSplit(const int leafId, const SplitAxis axis, const bool newOnSecondSide, const ImVec2 &cursor, AreaSplitPlan &plan) const
{
   // Divider of a split of the area at the cursor: the new area is on the side of newOnSecondSide (right / bottom), both parts keep their minimum size
   plan = AreaSplitPlan();
   plan.axis = axis;
   const LeafRect *const leaf = FindLeafRect(leafId);
   if (leaf == nullptr)
   {
      plan.reason = "Area not found";
      return false;
   }
   plan.leaf = leaf->rect;
   const float dpi = m_liveUI.GetDPI();
   const bool vertical = axis == SplitAxis::Vertical;
   const float lo = vertical ? leaf->rect.x0 : leaf->rect.y0;
   const float hi = vertical ? leaf->rect.x1 : leaf->rect.y1;
   plan.line = clamp(roundf(vertical ? cursor.x : cursor.y), lo, hi);
   plan.newType = m_areas.NewLeafType(leafId); // What AreaLayout::Split gives the new area
   if (!m_areas.CanSplit(leafId))
   {
      plan.reason = "Cannot split this area";
      return false;
   }
   const float newMin = (vertical ? AreaLayout::kPanelMinW : AreaLayout::kPanelMinH) * dpi;
   // Same minimum as the layout solver (AreaLayout::Compute): the viewport keeps its own minimum plus the N-panel's width when it is open, so a preview is never squeezed on commit
   const float keepMin = (leaf->type == AreaType::Viewport ? (vertical ? AreaLayout::kViewportMinW + (m_showSidebar ? m_sidebarWidth : 0.f) : AreaLayout::kViewportMinH)
                                                           : (vertical ? AreaLayout::kPanelMinW : AreaLayout::kPanelMinH))
      * dpi;
   if (hi - lo < newMin + keepMin)
   {
      plan.reason = "Area too small to split";
      return false;
   }
   plan.line = roundf(clamp(plan.line, newOnSecondSide ? lo + keepMin : lo + newMin, newOnSecondSide ? hi - newMin : hi - keepMin));
   plan.size = (newOnSecondSide ? hi - plan.line : plan.line - lo) / dpi;
   if (!vertical) // A stacked split displays its fixed child at size * ratio of the layout height (see AreaLayout::StackedShrink)
      plan.size /= AreaLayout::StackedShrink(m_areaRootH, dpi);
   plan.newRect = leaf->rect;
   if (vertical)
   {
      if (newOnSecondSide)
         plan.newRect.x0 = plan.line;
      else
         plan.newRect.x1 = plan.line;
   }
   else if (newOnSecondSide)
      plan.newRect.y0 = plan.line;
   else
      plan.newRect.y1 = plan.line;
   plan.ok = true;
   return true;
}

const char *EditorUI::AreaJoinRefusal(const int keepId, const int consumeId) const
{
   const AreaNode *const keep = m_areas.Find(keepId);
   const AreaNode *const consume = m_areas.Find(consumeId);
   if (keep == nullptr || consume == nullptr)
      return "Area not found";
   if (!consume->isSplit && consume->type == AreaType::Viewport)
      return "The 3D Viewport cannot be joined away";
   if (keep->isSplit || consume->isSplit)
      return "Areas with sub-areas cannot be joined";
   if (m_areas.ParentId(keepId) < 0 || m_areas.ParentId(keepId) != m_areas.ParentId(consumeId))
      return "Areas must share a full edge to be joined";
   return "Cannot join these areas";
}

void EditorUI::DrawAreaSplitPreview(const AreaSplitPlan &plan) const
{
   ImDrawList *const drawList = ImGui::GetForegroundDrawList();
   const bool vertical = plan.axis == SplitAxis::Vertical;
   const ImVec2 a = vertical ? ImVec2(plan.line, plan.leaf.y0) : ImVec2(plan.leaf.x0, plan.line);
   const ImVec2 b = vertical ? ImVec2(plan.line, plan.leaf.y1) : ImVec2(plan.leaf.x1, plan.line);
   const auto centeredText = [&](const AreaRect &rect, const char *text, const ImU32 color)
   {
      const ImVec2 size = ImGui::CalcTextSize(text);
      drawList->AddText(ImVec2(0.5f * (rect.x0 + rect.x1) - 0.5f * size.x, 0.5f * (rect.y0 + rect.y1) - 0.5f * size.y), color, text);
   };
   if (plan.ok)
   {
      // The new area is tinted and named, the divider is where the split will be
      drawList->AddRectFilled(ImVec2(plan.newRect.x0, plan.newRect.y0), ImVec2(plan.newRect.x1, plan.newRect.y1), IM_COL32(237, 145, 62, 38));
      centeredText(plan.newRect, AreaEditorName(plan.newType), EditorTheme::Text);
      drawList->AddLine(a, b, EditorTheme::Accent, 2.f);
   }
   else
   {
      drawList->AddLine(a, b, IM_COL32(150, 150, 150, 220), 2.f);
      centeredText(plan.leaf, plan.reason, EditorTheme::Text);
      ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
   }
}

void EditorUI::DrawAreaJoinPreview(const int keepId, const int consumeId, const bool ok) const
{
   const LeafRect *const keep = FindLeafRect(keepId);
   const LeafRect *const consume = FindLeafRect(consumeId);
   if (keep == nullptr || consume == nullptr)
      return;
   ImDrawList *const drawList = ImGui::GetForegroundDrawList();
   const float dpi = m_liveUI.GetDPI();
   const ImVec2 cmin(consume->rect.x0, consume->rect.y0), cmax(consume->rect.x1, consume->rect.y1);
   if (!ok)
   {
      drawList->AddRect(cmin, cmax, IM_COL32(220, 60, 60, 255), 0.f, 0, 2.f * dpi);
      ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
      return;
   }
   // The consumed area is darkened and an arrow points into it from the kept one, which gets an outline
   drawList->AddRectFilled(cmin, cmax, IM_COL32(0, 0, 0, 115));
   drawList->AddRect(ImVec2(keep->rect.x0 + dpi, keep->rect.y0 + dpi), ImVec2(keep->rect.x1 - dpi, keep->rect.y1 - dpi), EditorTheme::Accent, 0.f, 0, 2.f * dpi);
   const ImVec2 from(0.5f * (keep->rect.x0 + keep->rect.x1), 0.5f * (keep->rect.y0 + keep->rect.y1));
   const ImVec2 centre(0.5f * (cmin.x + cmax.x), 0.5f * (cmin.y + cmax.y));
   ImVec2 dir(centre.x - from.x, centre.y - from.y);
   const float dirLength = sqrtf(dir.x * dir.x + dir.y * dir.y);
   if (dirLength < 1e-3f)
      return;
   dir = ImVec2(dir.x / dirLength, dir.y / dirLength);
   const ImVec2 perp(-dir.y, dir.x);
   const float length = 0.3f * min(consume->rect.Width(), consume->rect.Height());
   const float head = 0.5f * length; // Length of the triangle
   const float halfHead = 0.35f * length;
   const ImVec2 tail(centre.x - dir.x * 0.5f * length, centre.y - dir.y * 0.5f * length);
   const ImVec2 tip(centre.x + dir.x * 0.5f * length, centre.y + dir.y * 0.5f * length);
   const ImVec2 base(tip.x - dir.x * head, tip.y - dir.y * head);
   drawList->AddLine(tail, base, EditorTheme::Accent, max(3.f * dpi, 0.12f * length));
   drawList->AddTriangleFilled(tip, ImVec2(base.x + perp.x * halfHead, base.y + perp.y * halfHead), ImVec2(base.x - perp.x * halfHead, base.y - perp.y * halfHead), EditorTheme::Accent);
}

void EditorUI::CancelAreaInteraction(const bool consumeEscape)
{
   const bool wasActive = m_areaDrag.active || m_areaModal != AreaModalKind::None;
   m_areaDrag = AreaDrag();
   m_areaModal = AreaModalKind::None;
   m_areaModalNode = -1;
   m_edgeMenuNode = -1; // The menu closes itself when its node is gone
   m_edgeMenuRequest = false;
   if (consumeEscape)
      m_areaEscPending = true; // The Esc that cancelled is not an editor Esc (it would deselect)
   if (wasActive)
      ImGui::SetWindowFocus(nullptr); // The zone or catcher window must not keep the keyboard focus away from the viewport keys
}

void EditorUI::RenderAreaCorners()
{
   // Corner action zones (Blender): a small square flush in each corner of each visible area. Dragging from one into the same area splits it, into
   // another one joins them. They are created after the splitter strips and kept above them, and give way to any open popup
   if (!CanUseAreaTools() || m_areaModal != AreaModalKind::None)
   {
      if (m_areaDrag.active)
         CancelAreaInteraction(false);
      return;
   }
   if (!m_areaDrag.active && ImGui::IsPopupOpen(static_cast<ImGuiID>(0), ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
      return;
   const float dpi = m_liveUI.GetDPI();
   const bool raiseNow = ShouldRaiseAreaWindows();
   const vector<LeafRect> leaves = m_leafRects;
   for (const LeafRect &leaf : leaves)
   {
      if (!leaf.visible)
         continue;
      const float fullSide = min(14.f * dpi, 0.4f * min(leaf.rect.Width(), leaf.rect.Height()));
      if (fullSide < 6.f)
         continue;
      for (int corner = 0; corner < 4; ++corner)
      {
         const float side = fullSide;
         const float x = (corner & 1) ? leaf.rect.x1 - side : leaf.rect.x0;
         const float y = (corner & 2) ? leaf.rect.y1 - side : leaf.rect.y0;
         // Triangular hit zone: the half of the square touching the area's corner (Blender). Outside it the window takes no input, so the header
         // buttons and scrollbars next to the corner keep their clicks
         const ImVec2 mouse = ImGui::GetIO().MousePos;
         const float lx = (corner & 1) ? x + side - mouse.x : mouse.x - x; // Distance from the corner along x / y
         const float ly = (corner & 2) ? y + side - mouse.y : mouse.y - y;
         char name[40];
         snprintf(name, sizeof(name), "##Corner%d_%d", leaf.id, corner); // Keyed by the stable node id
         // Item ids of other windows are hashed from the window id (what GetID does right after Begin): ImGuiWindow::GetID reads the window's ID stack, which
         // ImGui's memory compaction empties for windows not drawn for a while (a hidden or maximized-away area): crash at IDStack.back() (11-02 18:57)
         // A widget under the pointer (header search field, buttons, scrollbar ends) keeps the click: ImGui's hovered item of the last frame, which is
         // the corner's own button while the zone is the one hovered
         ImGuiWindow *const cornerWindow = ImGui::FindWindowByName(name);
         const ImGuiID hoveredId = ImGui::GetHoveredID();
         bool overWidget = hoveredId != 0 && (cornerWindow == nullptr || hoveredId != ImHashStr("##corner", 0, cornerWindow->ID));
         ImGuiWindow *const hw = ImGui::GetCurrentContext()->HoveredWindow;
         const char *const hwName = hw != nullptr ? hw->RootWindow->Name : "";
         const bool overStrip = strncmp(hwName, "##Split", 7) == 0;
         // The area panels' windows: "<BASE>" for the first area of a type, "<BASE>##<slot>" for the others
         const auto isAreaWindow = [hwName](const char *base)
         {
            const size_t n = strlen(base);
            return strncmp(hwName, base, n) == 0 && (hwName[n] == '\0' || strncmp(hwName + n, "##", 2) == 0);
         };
         const bool overArea = hw == nullptr || isAreaWindow("OUTLINER") || isAreaWindow("PROPERTIES") || isAreaWindow("SCRIPT")
            || isAreaWindow("PARTLIBRARY") || isAreaWindow("TABLECHECK") || strncmp(hwName, "CAMERAPREVIEW", 13) == 0
            || strcmp(hwName, "SIDEBAR") == 0 || strcmp(hwName, "orbit lock toggle") == 0; // The N-panel and the orbit lock button belong to the viewport area
         // A floating window (popup, tooltip, palette, history, overlay) keeps all its clicks: no corner acts through it
         const bool overFloating = !overArea && !overStrip && strncmp(hwName, "##Corner", 8) != 0;
         if (overStrip)
            overWidget = false; // The splitter strip overlaps the corner's edge band: the corner wins inside its triangle
         if (overWidget && strncmp(hwName, "CAMERAPREVIEW", 13) == 0 && hoveredId == ImHashStr("##ViewImage", 0, hw->RootWindow->ID))
            overWidget = false; // The image of a secondary view is a button over the whole area (its mouse navigation): the corner wins inside its triangle
         else if (overWidget && (corner & 2) != 0 && overArea && hw != nullptr)
            overWidget = hoveredId == ImGui::GetWindowScrollbarID(hw, ImGuiAxis_X)
               || hoveredId == ImGui::GetWindowScrollbarID(hw, ImGuiAxis_Y); // Bottom, on an area window: list rows do not count, only scrollbars
         if (overFloating)
            overWidget = true;
         const bool inTriangle = lx >= 0.f && ly >= 0.f && lx + ly <= side && (!overWidget || m_areaDrag.active);
         ImGui::SetNextWindowPos(ImVec2(x, y));
         ImGui::SetNextWindowSize(ImVec2(side, side));
         ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
         ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
         ImGui::Begin(name, nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
               | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ((inTriangle || m_areaDrag.active) ? 0 : ImGuiWindowFlags_NoInputs));
         if (raiseNow)
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
         ImGui::InvisibleButton("##corner", ImVec2(side, side));
         const bool activated = ImGui::IsItemActivated() && inTriangle;
         const ImVec2 origin((corner & 1) ? x + side : x, (corner & 2) ? y + side : y); // The area's corner
         const float sx = (corner & 1) ? -1.f : 1.f, sy = (corner & 2) ? -1.f : 1.f;
         const bool hot = ImGui::IsItemHovered() && inTriangle && !m_areaDrag.active;
         ImDrawList *const dl = ImGui::GetWindowDrawList(); // Under tooltips and floating windows
         if (hot)
         {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            dl->AddTriangleFilled(
               origin, ImVec2(origin.x + sx * side, origin.y), ImVec2(origin.x, origin.y + sy * side), (EditorTheme::Accent & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT));
         }
         if (hot && !m_areaDrag.active)
         {
            // Hatch (two diagonal strokes) only while the corner is hovered, as in Blender
            for (const float f : { 0.85f, 0.45f })
               dl->AddLine(ImVec2(origin.x + sx * side * f, origin.y), ImVec2(origin.x, origin.y + sy * side * f), EditorTheme::Text, 1.5f);
         }
         ImGui::End();
         ImGui::PopStyleVar(2);
         if (activated && !m_areaDrag.active)
         {
            m_areaDrag = AreaDrag();
            m_areaDrag.active = true;
            m_areaDrag.leaf = leaf.id;
            m_areaDrag.corner = corner;
            m_areaDrag.start = ImGui::GetMousePos();
         }
      }
   }
   UpdateAreaDrag();
}

void EditorUI::UpdateAreaDrag()
{
   if (!m_areaDrag.active)
      return;
   if (FindLeafRect(m_areaDrag.leaf) == nullptr)
   {
      CancelAreaInteraction(false); // The area is gone
      return;
   }
   const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
   if (escape || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
   {
      CancelAreaInteraction(escape);
      return;
   }
   const ImVec2 mouse = ImGui::GetIO().MousePos;
   const float dx = mouse.x - m_areaDrag.start.x, dy = mouse.y - m_areaDrag.start.y;
   const float threshold = 8.f * m_liveUI.GetDPI();
   if (!m_areaDrag.decided && dx * dx + dy * dy >= threshold * threshold)
      m_areaDrag.decided = true;
   const bool released = !ImGui::IsMouseDown(ImGuiMouseButton_Left);
   ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
   if (m_areaDrag.decided)
   {
      // Inside the area: split (the axis follows the dominant movement for the first 20 px, then locks; the other axis when the chosen one cannot fit). Inside another area: join, the source takes over
      const int under = AreaLayout::LeafAt(m_leafRects, mouse.x, mouse.y);
      if (under == m_areaDrag.leaf)
      {
         // Blender: the axis follows the dominant direction of the drag until it has gone ~20 px, then it is kept
         if (!m_areaDrag.axisChosen)
         {
            m_areaDrag.axis = fabsf(dx) > fabsf(dy) ? SplitAxis::Vertical : SplitAxis::Horizontal;
            const float lockDistance = 20.f * m_liveUI.GetDPI();
            m_areaDrag.axisChosen = dx * dx + dy * dy >= lockDistance * lockDistance;
         }
         const auto sideOf
            = [this](const SplitAxis a) { return a == SplitAxis::Vertical ? (m_areaDrag.corner & 1) != 0 : (m_areaDrag.corner & 2) != 0; }; // The new area is on the side of the start corner
         AreaSplitPlan plan;
         PlanAreaSplit(m_areaDrag.leaf, m_areaDrag.axis, sideOf(m_areaDrag.axis), mouse, plan);
         if (!plan.ok)
         {
            // The chosen axis does not fit: the other one is used when it does
            const SplitAxis other = m_areaDrag.axis == SplitAxis::Vertical ? SplitAxis::Horizontal : SplitAxis::Vertical;
            AreaSplitPlan alt;
            if (PlanAreaSplit(m_areaDrag.leaf, other, sideOf(other), mouse, alt))
            {
               plan = alt;
               m_areaDrag.axis = other;
            }
         }
         const bool newOnSecondSide = sideOf(m_areaDrag.axis);
         DrawAreaSplitPreview(plan);
         if (released)
         {
            if (plan.ok)
            {
               PendingAreaOp op;
               op.kind = PendingAreaOp::Kind::Split;
               op.leaf = m_areaDrag.leaf;
               op.axis = m_areaDrag.axis;
               op.newOnSecondSide = newOnSecondSide;
               op.size = plan.size;
               QueueAreaOp(op);
            }
            else
               Report(plan.reason);
         }
      }
      else if (under >= 0)
      {
         const bool ok = m_areas.CanJoin(m_areaDrag.leaf, under);
         DrawAreaJoinPreview(m_areaDrag.leaf, under, ok);
         if (released)
         {
            if (ok)
            {
               PendingAreaOp op;
               op.kind = PendingAreaOp::Kind::Join;
               op.leaf = m_areaDrag.leaf;
               op.leaf2 = under;
               QueueAreaOp(op);
            }
            else
               Report(AreaJoinRefusal(m_areaDrag.leaf, under));
         }
      }
   }
   if (released)
   {
      m_areaDrag = AreaDrag();
      ImGui::SetWindowFocus(nullptr);
   }
}

void EditorUI::RenderAreaEdgeMenu()
{
   // Right click on a divider. The popup is opened and drawn from a window of its own (the strip windows are gone when the menu is shown)
   ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
   ImGui::SetNextWindowSize(ImVec2(1.f, 1.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
   ImGui::Begin("##AreaEdgeMenuHost", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
         | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);
   if (m_edgeMenuRequest)
   {
      m_edgeMenuRequest = false;
      ImGui::OpenPopup("##AreaEdgeMenu");
   }
   ImGui::SetNextWindowPos(m_edgeMenuPos, ImGuiCond_Appearing);
   if (ImGui::BeginPopup("##AreaEdgeMenu"))
   {
      const AreaNode *const split = m_areas.Find(m_edgeMenuNode);
      if (split == nullptr || !split->isSplit || m_areaModal != AreaModalKind::None || !CanUseAreaTools())
         ImGui::CloseCurrentPopup(); // The edge is gone (or something else took over)
      else
      {
         if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
         {
            ImGui::CloseCurrentPopup();
            m_areaEscPending = true;
         }
         const auto &nodes = m_areas.Nodes();
         const AreaNode &first = nodes[split->child[0]], &second = nodes[split->child[1]];
         const bool canJoin = !first.isSplit && !second.isSplit && !first.hidden && !second.hidden;
         const bool canSplit = m_areas.LeafCount() < AreaLayout::kMaxLeaves; // Any editor can be shown twice: only the area count limits a split
         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
         ImGui::TextUnformatted("Area Options");
         ImGui::PopStyleColor();
         ImGui::Separator();
         const auto startMode = [this](const AreaModalKind kind, const SplitAxis axis)
         {
            m_areaModal = kind;
            m_areaModalAxis = axis;
            m_areaModalNode = m_edgeMenuNode;
            m_areaModalFocus = true;
         };
         if (ImGui::MenuItem("Vertical Split", nullptr, false, canSplit))
            startMode(AreaModalKind::Split, SplitAxis::Vertical);
         if (ImGui::MenuItem("Horizontal Split", nullptr, false, canSplit))
            startMode(AreaModalKind::Split, SplitAxis::Horizontal);
         if (ImGui::MenuItem("Join Areas", nullptr, false, canJoin))
            startMode(AreaModalKind::Join, SplitAxis::Vertical);
         if (ImGui::MenuItem("Swap Areas"))
         {
            PendingAreaOp op;
            op.kind = PendingAreaOp::Kind::Swap;
            op.leaf = m_edgeMenuNode;
            QueueAreaOp(op);
         }
      }
      ImGui::EndPopup();
   }
   ImGui::End();
   ImGui::PopStyleVar(2);
}

void EditorUI::RenderAreaModal()
{
   // Interactive split / join started from the edge menu: a transparent window over the whole display takes the mouse (no click reaches a panel),
   // the preview follows the mouse and a click confirms. Esc or right click cancels
   if (m_areaModal == AreaModalKind::None)
      return;
   const AreaNode *const split = m_areas.Find(m_areaModalNode);
   if (split == nullptr || !split->isSplit || !CanUseAreaTools())
   {
      CancelAreaInteraction(false);
      return;
   }
   if (ImGui::GetTopMostPopupModal() != nullptr)
   {
      CancelAreaInteraction(false); // A modal dialog (discard changes,...) opened: it must stay clickable
      return;
   }
   const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
   if (escape || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
   {
      CancelAreaInteraction(escape);
      return;
   }
   const ImGuiIO &io = ImGui::GetIO();
   if (m_areaModalFocus)
   {
      ImGui::SetNextWindowFocus(); // On top of everything for the length of the mode
      m_areaModalFocus = false;
   }
   ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
   ImGui::SetNextWindowSize(io.DisplaySize);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
   ImGui::Begin(
      "##AreaModalCatcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
   ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); // Every frame: nothing (re-raised strips, popups) may end up above it
   ImGui::InvisibleButton("##catch", io.DisplaySize);
   const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
   ImGui::End();
   ImGui::PopStyleVar(2);
   ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

   const ImVec2 mouse = io.MousePos;
   const int under = AreaLayout::LeafAt(m_leafRects, mouse.x, mouse.y);
   bool done = false;
   if (m_areaModal == AreaModalKind::Split)
   {
      if (under >= 0)
      {
         AreaSplitPlan plan;
         PlanAreaSplit(under, m_areaModalAxis, true, mouse, plan); // The new area is the right / bottom part
         DrawAreaSplitPreview(plan);
         if (clicked)
         {
            done = true;
            if (plan.ok)
            {
               PendingAreaOp op;
               op.kind = PendingAreaOp::Kind::Split;
               op.leaf = under;
               op.axis = m_areaModalAxis;
               op.newOnSecondSide = true;
               op.size = plan.size;
               QueueAreaOp(op);
            }
            else
               Report(plan.reason);
         }
      }
   }
   else
   {
      // The area not under the mouse is consumed by the other one
      const auto &nodes = m_areas.Nodes();
      const int id0 = static_cast<int>(nodes[split->child[0]].id), id1 = static_cast<int>(nodes[split->child[1]].id);
      const int keep = under == id0 ? id0 : under == id1 ? id1 : -1;
      if (keep >= 0)
      {
         const int consume = keep == id0 ? id1 : id0;
         const bool ok = m_areas.CanJoin(keep, consume);
         DrawAreaJoinPreview(keep, consume, ok);
         if (clicked)
         {
            done = true;
            if (ok)
            {
               PendingAreaOp op;
               op.kind = PendingAreaOp::Kind::Join;
               op.leaf = keep;
               op.leaf2 = consume;
               QueueAreaOp(op);
            }
            else
               Report(AreaJoinRefusal(keep, consume));
         }
      }
   }
   if (done)
   {
      m_areaModal = AreaModalKind::None;
      m_areaModalNode = -1;
      ImGui::SetWindowFocus(nullptr);
   }
}

bool EditorUI::IsSecondaryViewEditDrag() const
{
   // An interactive edit (gizmo drag, G/R/S modal transform, field drag; not a focused text field, not the navigation of a secondary view: its image
   // or header is an active item of a CAMERAPREVIEW window) keeps one undo record from its start: the secondary views render while it runs
   const ImGuiWindow *const activeWindow = ImGui::GetCurrentContext()->ActiveIdWindow;
   const bool viewActive = activeWindow != nullptr && strncmp(activeWindow->RootWindow->Name, "CAMERAPREVIEW", 13) == 0;
   return ImGuizmo::IsUsingAny() || m_modal.active || (ImGui::IsAnyItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !viewActive);
}

bool EditorUI::BeginAreaWindow(const char *const name, const ImVec2 &pos, const ImVec2 &size)
{
   const float pad = 4.0f * m_liveUI.GetDPI();
   ImGui::SetNextWindowPos(pos);
   ImGui::SetNextWindowSize(size);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   return ImGui::Begin(name, nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
}

void EditorUI::EndAreaWindow()
{
   ImGui::End();
   ImGui::PopStyleVar(3);
}

void EditorUI::RenderAreas()
{
   if (m_table->IsLocked())
   {
      for (const auto &[leafId, panel] : m_propertiesAreas)
         panel->ClearMouseOver();
      for (CameraPreviewPanel &view : m_cameraPreviews)
         view.NotHosted();
      return;
   }
   // The leaves are read once: picking a type in the header of an area re-types them, which takes effect on the next frame. Each area of
   // a panel type has its slot (BindAreaPanels): the Outliner and Properties areas their own panel, the Script and Part Library areas their
   // own window of the one panel; the Camera Preview leaves (up to kMaxViewLeaves) each get one of the secondary view panels: the one bound
   // to that leaf, else one not bound to a shown leaf (it reads the leaf's settings)
   const vector<LeafRect> leaves = m_leafRects;
   BindAreaPanels(); // Normally already bound (every change of the tree binds), cheap when nothing changed
   vector<const PropertiesPanel *> propertiesDrawn;
   bool viewDrawn[AreaLayout::kMaxViewLeaves] = {};
   // Render schedule, one rule for every case: the shown views that want to render periodically this frame (Live on or an edit drag in progress, an image
   // shown, not refused by the 2 PBR view cap) are ranked within their class (PBR, wireframe) in leaf order; the rank sets the phase (CameraPreviewPanel).
   // One-shot renders (Refresh, end of an edit, undo, camera moves) are outside the schedule
   std::map<int, int> viewRanks;
   {
      const bool editDrag = IsSecondaryViewEditDrag();
      int classRank[2] = {};
      for (const LeafRect &l : leaves)
      {
         const AreaNode *const o = l.visible && l.type == AreaEditor::CameraPreview ? m_areas.Find(l.id) : nullptr;
         if (o == nullptr || !(o->view.live || editDrag))
            continue;
         int slot = -1;
         for (int i = 0; i < AreaLayout::kMaxViewLeaves; i++)
            if (m_cameraPreviews[i].GetLeaf() == l.id)
               slot = i;
         if (slot >= 0 && (!m_cameraPreviews[slot].HasImage() || (!o->view.wireframe && !m_renderer->CanRenderEditorPBRView(slot))))
            continue;
         viewRanks[l.id] = classRank[o->view.wireframe ? 1 : 0]++;
      }
   }
   const auto isShownViewLeaf = [&leaves](const int id) {
      return id >= 0 && std::any_of(leaves.begin(), leaves.end(), [id](const LeafRect &l) { return l.visible && l.id == id && l.type == AreaEditor::CameraPreview; });
   };
   for (const LeafRect &leaf : leaves)
   {
      if (!leaf.visible)
         continue;
      const ImVec2 pos(leaf.rect.x0, leaf.rect.y0);
      const ImVec2 size(leaf.rect.Width(), leaf.rect.Height());
      switch (leaf.type)
      {
      case AreaEditor::Outliner:
         if (OutlinerPanel *const panel = GetOutlinerArea(leaf.id))
            panel->RenderInArea(pos, size);
         break;
      case AreaEditor::Properties:
         if (PropertiesPanel *const panel = GetPropertiesArea(leaf.id))
         {
            panel->RenderInArea(pos, size);
            propertiesDrawn.push_back(panel);
         }
         break;
      case AreaEditor::Script:
         m_scriptPanel.RenderInArea(pos, size, leaf.id);
         break;
      case AreaEditor::PartLibrary:
         m_partLibrary.RenderInArea(pos, size, leaf.id);
         break;
      case AreaEditor::TableCheck: m_tableCheck.RenderInArea(pos, size, leaf.id); break;
      case AreaEditor::Viewport: break; // The 3D view shows through
      case AreaEditor::CameraPreview:
         // The view settings are saved in the layout leaf: read when a panel is bound to it, written back by the panel on every change
         if (const AreaNode *const node = m_areas.Find(leaf.id))
         {
            int k = -1;
            for (int i = 0; i < AreaLayout::kMaxViewLeaves && k < 0; i++)
               if (m_cameraPreviews[i].GetLeaf() == leaf.id)
                  k = i;
            for (int i = 0; i < AreaLayout::kMaxViewLeaves && k < 0; i++)
               if (!viewDrawn[i] && !isShownViewLeaf(m_cameraPreviews[i].GetLeaf()))
                  k = i;
            if (k >= 0 && !viewDrawn[k])
            {
               if (m_cameraPreviews[k].GetLeaf() != leaf.id)
                  m_cameraPreviews[k].Bind(leaf.id, node->previewView, node->view);
               const auto r = viewRanks.find(leaf.id);
               const int rank = r != viewRanks.end() ? r->second : 0;
               m_cameraPreviews[k].SetRank(rank);
               m_cameraPreviews[k].RenderInArea(pos, size);
               viewDrawn[k] = true;
            }
         }
         break;
      case AreaEditor::Count: break;
      }
   }
   for (const auto &[leafId, panel] : m_propertiesAreas)
      if (std::ranges::find(propertiesDrawn, panel.get()) == propertiesDrawn.end())
         panel->ClearMouseOver(); // A panel that is not hosted this frame must not keep last frame's hover
   for (int i = 0; i < AreaLayout::kMaxViewLeaves; i++)
      if (!viewDrawn[i])
         m_cameraPreviews[i].NotHosted(); // Zero cost when not shown: no render request, targets released
}

void EditorUI::RenderAreaTypeButton(const AreaEditor current, const int leafId)
{
   // Blender's editor type switcher: the editor icon and a down chevron at the far left of the area header, opening the list of the editor types
   const ImGuiStyle &style = ImGui::GetStyle();
   const char *const icon = AreaEditorIcon(current);
   const ImVec2 iconSize = ImGui::CalcTextSize(icon);
   const float chevronWidth = ImGui::GetFontSize() * 0.4f; // Small, drawn in TextDim
   const ImVec2 buttonSize(style.FramePadding.x * 2.f + iconSize.x + style.ItemInnerSpacing.x + chevronWidth, ImGui::GetFrameHeight());
   const int leaf = leafId >= 0 ? leafId : m_areas.FindLeaf(current);

   ImGui::PushID("##AreaType");
   const bool clicked = ImGui::Button("##AreaTypeButton", buttonSize);
   const ImVec2 itemMin = ImGui::GetItemRectMin();
   const ImVec2 itemMax = ImGui::GetItemRectMax();
   {
      ImDrawList *const drawList = ImGui::GetWindowDrawList();
      const float centerY = (itemMin.y + itemMax.y) * 0.5f;
      drawList->AddText(ImVec2(itemMin.x + style.FramePadding.x, centerY - iconSize.y * 0.5f), EditorTheme::Text, icon);
      const float half = chevronWidth * 0.5f;
      const float centerX = itemMax.x - style.FramePadding.x - half;
      drawList->AddTriangleFilled(ImVec2(centerX - half, centerY - half * 0.5f), ImVec2(centerX + half, centerY - half * 0.5f), ImVec2(centerX, centerY + half * 0.5f), EditorTheme::TextDim);
   }
   ImGui::SetItemTooltip("Editor Type");
   if (clicked)
      ImGui::OpenPopup("##AreaTypePopup");
   ImGui::SetNextWindowPos(ImVec2(itemMin.x, itemMax.y), ImGuiCond_Always);
   if (ImGui::BeginPopup("##AreaTypePopup"))
   {
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      {
         ImGui::CloseCurrentPopup();
         m_areaEscPending = true;
      }
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
      ImGui::TextUnformatted("Editor Type");
      ImGui::PopStyleColor();
      ImGui::Separator();
      for (int i = 0; i <= kPanelTypeCount + 1; ++i)
      {
         // The panels, then the camera preview, then the table check
         const AreaEditor type = i < kPanelTypeCount ? static_cast<AreaEditor>(i) : i == kPanelTypeCount ? AreaEditor::CameraPreview : AreaEditor::TableCheck;
         if (type == AreaEditor::CameraPreview && !AreaLayout::cameraPreviewEnabled)
            continue;
         const string label = string(AreaEditorIcon(type)) + "  " + AreaEditorName(type);
         if (ImGui::Selectable(label.c_str(), type == current))
            SetAreaEditor(leaf, type);
      }
      if (m_areas.CanClose(leaf))
      {
         ImGui::Separator();
         if (ImGui::Selectable(ICON_LC_X "  Close Area"))
         {
            PendingAreaOp op;
            op.kind = PendingAreaOp::Kind::Close;
            op.leaf = leaf;
            QueueAreaOp(op);
         }
      }
      ImGui::EndPopup();
   }
   ImGui::PopID();
}

bool EditorUI::ConsumedPaneEscape()
{
   // The Esc that cancelled a field drag or edit of a property pane is not an editor Esc (latched until its release, like the other consumed Esc)
   if (PropertyPane::ConsumeEscape())
      m_paneEscPending = true;
   if (!m_paneEscPending)
      return false;
   if (!ImGui::IsKeyDown(ImGuiKey_Escape))
      m_paneEscPending = false; // Released: this is the frame of the release, still consumed
   return true;
}

bool EditorUI::ConsumedAreaEscape()
{
   if (!m_areaEscPending)
      return false;
   if (!ImGui::IsKeyDown(ImGuiKey_Escape))
      m_areaEscPending = false; // Released: this is the frame of the release, still consumed
   return true;
}

void EditorUI::ShowSidebar(SidebarTab tab)
{
   m_showSidebar = true;
   m_sidebarTab = tab;
}

Vertex2D EditorUI::GetUIVisibleFraction() const
{
   // When the editor UI is not displayed (fly mode, inspection modal), nothing is drawn over the render
   if (m_flyMode || m_inspectionModal.IsVisible())
      return Vertex2D(1.f, 1.f);
   if (!m_areaRectsValid)
      ComputeAreaRects();
   if (!m_viewportVisible)
   {
      // A panel is maximized: frame for the layout that comes back on restore (its viewport area, N-panel excluded)
      const ImGuiViewport *const mainViewport = ImGui::GetMainViewport();
      const float top = mainViewport->Pos.y + m_chrome.GetTopBarHeight();
      const AreaRect tree { mainViewport->Pos.x, top, mainViewport->Pos.x + mainViewport->Size.x, max(top, mainViewport->Pos.y + mainViewport->Size.y - m_chrome.GetStatusBarHeight()) };
      const float dpi = m_liveUI.GetDPI();
      vector<LeafRect> leaves;
      vector<SplitterRect> splitters;
      m_areas.Compute(tree, dpi, (m_showSidebar && !m_table->IsLocked()) ? m_sidebarWidth : 0.f, leaves, splitters, -1);
      for (const LeafRect &leaf : leaves)
         if (leaf.type == AreaType::Viewport && leaf.visible)
         {
            const ImGuiIO &displayIo = ImGui::GetIO();
            const float x1 = max(leaf.rect.x0, leaf.rect.x1 - (m_showSidebar ? m_sidebarWidth * dpi : 0.f));
            const float marginX = max(leaf.rect.x0, displayIo.DisplaySize.x - x1);
            const float marginY = max(leaf.rect.y0, displayIo.DisplaySize.y - leaf.rect.y1);
            return Vertex2D(max(0.1f, 1.f - 2.f * marginX / displayIo.DisplaySize.x), max(0.1f, 1.f - 2.f * marginY / displayIo.DisplaySize.y));
         }
      return Vertex2D(1.f, 1.f);
   }
   // Otherwise, the top chrome (and the side panes, unless the table is locked) are drawn over the render:
   // evaluate the remaining visible fraction, applied as a symmetric margin so that fitted content stays centered. The area left to
   // the 3D view is the content of the viewport area (its N-panel excluded)
   if (!m_areaRectsValid)
      ComputeAreaRects();
   const ImGuiIO &io = ImGui::GetIO();
   const AreaRect &vc = m_viewportContentRect;
   const float marginX = max(vc.x0, io.DisplaySize.x - vc.x1);
   const float marginY = max(vc.y0, io.DisplaySize.y - vc.y1);
   return Vertex2D(max(0.1f, 1.f - 2.f * marginX / io.DisplaySize.x), max(0.1f, 1.f - 2.f * marginY / io.DisplaySize.y));
}

void EditorUI::SetPlayfieldCamera()
{
   // Setup the editor camera to frame the playfield bounds from a top view (table XY plane, Y axis going down)
   const ImGuiIO &io = ImGui::GetIO();
   const Vertex2D visible = GetUIVisibleFraction();
   const float aspect = io.DisplaySize.x / io.DisplaySize.y;
   m_camDistance = 0.5f * max(m_table->m_bottom / visible.y, m_table->m_right / (aspect * visible.x));
   const vec3 eye(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, -m_camDistance);
   const vec3 at(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, 0.f);
   constexpr vec3 up { 0.f, -1.f, 0.f };
   m_camView = Matrix3D::MatrixLookAtRH(eye, at, up);
}

// ImGuizmo sets its own clip rect (the full display, not intersected) when it draws: the commands it added are clipped to the viewport area afterwards, so the
// gizmo does not draw over the panels beside a narrow viewport
static void ClipDrawCommands(ImDrawList *const list, const int firstCmd, const AreaRect *const rect)
{
   if (list == nullptr || rect == nullptr)
      return;
   for (int i = firstCmd; i < list->CmdBuffer.Size; i++)
   {
      ImVec4 &clip = list->CmdBuffer[i].ClipRect;
      clip = ImVec4(max(clip.x, rect->x0), max(clip.y, rect->y0), min(clip.z, rect->x1), min(clip.w, rect->y1));
   }
}

void EditorUI::Render3D()
{
   // While a table save is running on the file operation worker thread, the table parts must not be
   // touched: the UI part renders write the editor visibility overrides into the parts' data (m_d),
   // racing with the serialization and saving the display state instead of the authored visibility
   // restored by RestorePartsVisibility when the save was started
   if (IsSaveInProgress())
      return;
   UpdateEditableList();
   m_refImage.Render3D(*this, -1); // Behind or over the parts, as the image is set
   LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, (m_table->m_liveBaseTable != nullptr) && m_player->IsPlaying());
   for (const auto &uiPart : m_editables)
   {
      if ((m_camMode == ViewMode::DesktopBackdrop && !uiPart->GetEditable()->m_desktopBackdrop) || (m_camMode != ViewMode::DesktopBackdrop && uiPart->GetEditable()->m_desktopBackdrop))
         continue;
      uiPart->Render(ctx);
   }
   m_partLibrary.Render3D();
}

void EditorUI::RenderReferenceImage3D(const int slot)
{
   if (IsSaveInProgress())
      return;
   m_refImage.Render3D(*this, SecondaryViewOrdinal(slot));
}

int EditorUI::SecondaryViewOrdinal(const int slot) const
{
   const int leafId = slot >= 0 && slot < AreaLayout::kMaxViewLeaves ? m_cameraPreviews[slot].GetLeaf() : -1;
   int ordinal = 0;
   for (const LeafRect &leaf : m_leafRects)
   {
      if (leaf.type != AreaEditor::CameraPreview)
         continue;
      if (leaf.id == leafId)
         return ordinal;
      ordinal++;
   }
   return 0;
}

void EditorUI::RenderHiddenOverlay3D()
{
   if (IsSaveInProgress() || IsInspectMode()) // (no overlay in a play test: writing the display state here would undo a script's visibility changes)
      return;
   // As Render3D for the parts which are not selected (the selection's edges follow), with the view's own shading and as an editor camera view
   LiveRenderContext ctx(m_player, nullptr, ViewMode::EditorCam, m_renderer->GetShadeMode(), false);
   const bool selectionDrawn = m_selectionOverlay && m_navSelectionDisplay != 2; // Else a selected hidden part would draw nothing in this view
   for (const auto &uiPart : m_editables)
   {
      if (uiPart->GetEditable()->m_desktopBackdrop || (selectionDrawn && IsPartSelected(uiPart)))
         continue;
      uiPart->Render(ctx);
   }
}

void EditorUI::RenderSelection3D()
{
   if (m_multiSel.empty() || !m_selectionOverlay || IsSaveInProgress())
      return;
   // As the 3D Viewport's selection overlay (RenderUI), without its 2D lines (no draw list): the edges of the selected parts over the scene, active part lighter
   if (m_renderer->m_renderDevice->GetCurrentRenderTarget()->HasDepth())
      m_renderer->m_renderDevice->Clear(clearType::ZBUFFER, 0);
   LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, (m_table->m_liveBaseTable != nullptr) && m_player->IsPlaying());
   ctx.m_selectionDisplay = m_navSelectionDisplay;
   ctx.m_isSelected = true;
   for (const auto &part : m_multiSel)
   {
      ctx.m_isActive = part == m_selection.GetPart();
      part->Render(ctx);
   }
}

void EditorUI::DrawSelectionLines(ImDrawList *drawList, const PickView &view)
{
   if (m_multiSel.empty() || !m_selectionOverlay || IsSaveInProgress())
      return;
   LiveRenderContext ctx(m_player, drawList, m_camMode, m_shadeMode, (m_table->m_liveBaseTable != nullptr) && m_player->IsPlaying());
   ctx.SetImageView(view.modelViewProj, view.rectMin, view.rectSize); // The edges are drawn by the view's own pass (RenderSelection3D)
   ctx.m_selectionDisplay = m_navSelectionDisplay;
   ctx.m_isSelected = true;
   drawList->PushClipRect(view.rectMin, ImVec2(view.rectMin.x + view.rectSize.x, view.rectMin.y + view.rectSize.y), true);
   for (const auto &part : m_multiSel)
   {
      if (view.local != nullptr && !view.local->Allows(part->GetEditable()))
         continue; // Selected, but not in the view's Local View
      ctx.m_isActive = part == m_selection.GetPart();
      part->Render(ctx);
   }
   drawList->PopClipRect();
}

void EditorUI::RenderUI()
{
   const ImGuiIO &io = ImGui::GetIO();
   ImGuizmo::SetOrthographic(m_camMode == ViewMode::DesktopBackdrop || (m_camMode == ViewMode::EditorCam && !m_perspectiveCam));
   ImGuizmo::BeginFrame();
   ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
   UpdateLocalViews(); // Before anything reads the sets (the gizmo, the overlays, the renderer)

   // In desktop backdrop mode, only backdrop parts can be selected and edited, and conversely
   {
      const bool backdropMode = m_camMode == ViewMode::DesktopBackdrop;
      std::erase_if(m_multiSel, [backdropMode](const std::shared_ptr<EditorUIPart> &part) { return part->GetEditable()->m_desktopBackdrop != backdropMode; });
      if (m_selection.GetType() == Selection::S_EDITABLE && !IsPartSelected(m_selection.GetPart()))
         m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
      if (m_outlinerAnchor && m_outlinerAnchor->GetEditable()->GetItemType() != eItemPartGroup && m_outlinerAnchor->GetEditable()->m_desktopBackdrop != backdropMode)
         m_outlinerAnchor.reset();
   }

   // Add part mode housekeeping: it is only available in edit mode, when the table is not locked, and
   // while not in drag point edit mode, so cancel it if one of these conditions is no longer met
   if (m_addPartType != eItemInvalid && (IsInspectMode() || m_table->IsLocked() || m_pointEditPart != nullptr))
      m_addPartType = eItemInvalid;
   else if (m_addPartType != eItemInvalid && m_camMode != ViewMode::DesktopBackdrop && m_fileOp == nullptr && !m_modal.active)
      PlaceNewPartAtCursor(); // As Blender's: a new part goes to the 3D cursor (the desktop backdrop, a 2D plane, keeps the click to place)
   if (m_plasticTrace.active && (IsInspectMode() || m_table->IsLocked() || m_pointEditPart != nullptr))
      PlasticFromAtlas::Cancel(*this);
   PlayfieldMaker::Update(*this);
   // Plastics whose meshes were rebuilt: their picking colliders follow
   if (m_player != nullptr && m_player->m_physics != nullptr && !IsInspectMode())
      for (IEditable *const part : m_table->GetParts())
      {
         if (part->GetItemType() == eItemPrimitive && static_cast<Primitive *>(part)->m_plasticCrossingWarn)
         {
            Primitive *const plastic = static_cast<Primitive *>(part);
            plastic->m_plasticCrossingWarn = false;
            if (plastic->m_d.m_plasticGen) // Warning only: the holes stay as they are. Said once per change of the two counts (and not when they drop to 0)
            {
               const int c = plastic->m_plasticCrossingHoles, o = plastic->m_plasticSkippedHoles;
               plastic->m_plasticReportedCrossing = c;
               plastic->m_plasticReportedSkipped = o;
               if (c > 0 || o > 0)
               {
                  string msg = "Warning: " + plastic->GetName() + ": ";
                  if (c > 0)
                     msg += std::to_string(c) + (c == 1 ? " hole is on or near" : " holes are on or near") + " the outline edge";
                  if (c > 0 && o > 0)
                     msg += ", ";
                  if (o > 0)
                     msg += std::to_string(o) + (o == 1 ? " hole is" : " holes are") + " outside the outline (left out)";
                  Report(msg);
               }
            }
         }
         // A drag waits for its end: the companions sit in the static UI quadtree, each update of one waits for a rebuild of that tree (~75 ms on a big table)
         if (part->GetItemType() == eItemPrimitive && static_cast<Primitive *>(part)->m_plasticPickDirty && !IsDragInProgress() && !ImGui::IsAnyItemActive())
         {
            Primitive *const plastic = static_cast<Primitive *>(part);
            plastic->m_plasticPickDirty = false;
            plastic->m_railPickOnlyDirty = false; // The full update refreshes the picking quadtree too
            m_player->m_physics->Update(plastic);
         }
         // A support edit of a Wire Rail: the picking quadtree only, the channel colliders are unchanged
         else if (part->GetItemType() == eItemPrimitive && static_cast<Primitive *>(part)->m_railPickOnlyDirty && !IsDragInProgress() && !ImGui::IsAnyItemActive())
         {
            Primitive *const rail = static_cast<Primitive *>(part);
            rail->m_railPickOnlyDirty = false;
            m_player->m_physics->UpdateUI(rail);
         }
      }

   // Drag point edit mode housekeeping: exit without restoring the selection if the edited part is no
   // longer the active selected part, and drop selected points that do not exist anymore (points are
   // recreated on undo, ...)
   if (m_pointEditPart)
   {
      DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve();
      if (IsInspectMode() || (curve == nullptr && !m_pointEditPart->HasEditCenter()) || m_selection.GetType() != Selection::S_EDITABLE || m_selection.GetPart() != m_pointEditPart)
         ExitPointEditMode(false);
      else if (curve != nullptr)
      {
         std::erase_if(m_pointSel, [curve](const DragPoint *point) { return curve->GetPointIndex(point) < 0; });
      }
   }

   // Apply the file picked by the asynchronous 'Save As' file dialog (deferred to the main thread, in edit mode)
   if (!IsInspectMode() && m_pendingSaveAsPath && !m_pendingSaveAsPath->empty() && m_fileOp == nullptr)
   {
      // The table is saved through its filename: set it for the save, and restore it if the save fails (which reports the error)
      auto op = std::make_unique<AsyncFileOp>();
      op->type = AsyncFileOp::Type::Save;
      op->restoreOnFailure = true;
      op->previousFilename = m_table->m_filename;
      op->previousTitle = m_table->m_title;
      m_table->m_filename = PathFromUTF8(*m_pendingSaveAsPath);
      m_table->m_title = TitleFromFilename(m_table->m_filename);
      op->filename = m_table->m_filename;
      op->displayPath = PathToUTF8(m_table->m_filename);
      m_pendingSaveAsPath = nullptr;
      // A folder selection is a VPZ folder pack: the directory must exist for the table to be saved as a folder (not a .vpx file)
      if (m_pendingSaveAsFolder)
      {
         m_pendingSaveAsFolder = false;
         std::filesystem::create_directories(m_table->m_filename);
      }
      StartFileOp(std::move(op));
   }

   // Apply the file picked by the asynchronous 'Load' file dialog (deferred to the main thread, in edit mode):
   // the loaded table replaces the edited one through a player session switch (see Player::SetTable)
   if (!IsInspectMode() && m_pendingLoadPath && !m_pendingLoadPath->empty() && m_fileOp == nullptr)
   {
      auto op = std::make_unique<AsyncFileOp>();
      op->type = AsyncFileOp::Type::Load;
      op->filename = PathFromUTF8(*m_pendingLoadPath);
      op->displayPath = PathToUTF8(op->filename);
      m_pendingLoadPath = nullptr;
      StartFileOp(std::move(op));
   }

   // Apply the .prefab picked by the asynchronous 'Import Unity Prefab' dialog (deferred to the main thread)
   if (!IsInspectMode() && m_pendingUnityPrefabPath && !m_pendingUnityPrefabPath->empty())
   {
      const std::filesystem::path path = PathFromUTF8(*m_pendingUnityPrefabPath);
      m_pendingUnityPrefabPath = nullptr;
      ImportUnityPrefab(path);
   }

   // Apply the result of a completed asynchronous file operation (running a worker thread load/save)
   if (m_fileOp && m_fileOp->completed)
      FinishFileOp();
   if (m_deferredStop >= 0 && m_fileOp == nullptr)
   {
      const int closeState = m_deferredStop;
      m_deferredStop = -1;
      RequestClose(closeState);
   }

   // Queued layout changes, then the rects of the areas (before any panel and the chrome overlays use them)
   UpdateAreaLayout();

#if !((defined(__APPLE__) && ((defined(TARGET_OS_IOS) && TARGET_OS_IOS) || (defined(TARGET_OS_TV) && TARGET_OS_TV))) || defined(__ANDROID__))

   const bool showFullUI = !m_inspectionModal.IsVisible() && !m_flyMode;
   m_chrome.Render(showFullUI);
   if (!showFullUI)
      CancelAreaInteraction(false); // Fly mode, inspection modal: no corner drag or edge mode survives it
   ComputeAreaRects(); // The bar heights may have changed with the chrome render
   m_secondaryViewHovered = false; // Set by the secondary views drawn by RenderAreas
   m_secondaryViewSlot = -1;
   for (SecondaryViewInfo &info : m_viewInfo)
      info = SecondaryViewInfo();
   if (showFullUI)
   {
      RenderAreas(); // Panels of the visible areas
      m_chrome.RenderToolShelf(m_viewportContentRect);
   }
   if (!m_inspectionModal.IsVisible())
      m_pinned.Render(); // N-panel
   if (!showFullUI)
      m_openQuickFavorites = false;
   if (showFullUI)
   {
      m_pinned.RenderQuickFavorites(); // Q
      RenderSplitters();
   }
   m_pinned.FlushPendingCopy(); // Copy to Selected requests queued by the panels, applied outside of any undo record
   if (showFullUI)
      m_palette.Render();
   if (showFullUI)
   {
      RenderAdjustPanel();
      PlasticFromAtlas::RenderPanel(*this);
      m_refImage.RenderPanel(*this);
      PlasticFromAtlas::RenderEditPanel(*this);
      PlasticFromAtlas::RenderFitPanel(*this);
      PlasticFromAtlas::RenderMenu(*this);
   }
   else
      m_refImage.UiHidden();
   m_chrome.RenderStatusBar(showFullUI);
   m_inspectionModal.Render();
   m_pbrConvertModal.Render();
   if (showFullUI)
      m_selectRename.Render();
   m_partLibrary.Update();
   m_scriptPanel.Update();

#endif

   // Deferred playfield fit on editor opening (display size and chrome height are valid once the UI has been rendered)
   if (m_fitPlayfieldCamera && io.DisplaySize.x > 0.f && io.DisplaySize.y > 0.f)
   {
      m_fitPlayfieldCamera = false;
      SetPlayfieldCamera();
   }

   // Invisible full frame window for overlays
   ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
   ImGui::SetNextWindowPos(ImVec2(0, 0));
   ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
   ImGui::PushStyleColor(ImGuiCol_Border, 0);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::Begin("overlays", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings
         | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
   ImDrawList *const overlayDrawList = ImGui::GetWindowDrawList();
   RenderCursorMenu();

   // Confirmation popup for discarding unsaved changes when loading another table or closing (OpenPopup must be
   // called in the same window scope as the matching BeginPopupModal)
   // All variants share one popup id, so that a request arriving while it is open retargets it
   const char *const confirmTitle = m_pendingClose ? "Quit###ConfirmDiscard" : m_pendingNewTable ? "New Table###ConfirmDiscard" : "Load Table###ConfirmDiscard";
   if (m_confirmLoadTable)
   {
      m_confirmLoadTable = false;
      ImGui::OpenPopup(confirmTitle);
   }
   if (ImGui::BeginPopupModal(confirmTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
   {
      ImGui::TextUnformatted("The current table has unsaved changes which will be lost.");
      ImGui::NewLine();
      if (ImGui::Button("Discard changes"))
      {
         ImGui::CloseCurrentPopup();
         if (m_pendingClose)
         {
            m_player->SetCloseState(static_cast<Player::CloseState>(*m_pendingClose));
            m_pendingClose.reset();
         }
         else if (m_pendingNewTable)
         {
            LoadTableTemplate(*m_pendingNewTable);
            m_pendingNewTable.reset();
         }
         else
            ShowLoadTableDialog(m_pendingLoadFolder);
      }
      ImGui::SameLine();
      ImGui::SetItemDefaultFocus();
      if (ImGui::Button("Cancel") || PropertyPane::ModalEscapePressed())
      {
         m_pendingNewTable.reset();
         m_pendingClose.reset();
         ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
   }

   // Progress feedback of the asynchronous file operation (load/save run on a worker thread): a modal popup
   // that blocks all interaction while the operation is running
   RenderFileOpModal();

   ImGui::End();
   ImGui::PopStyleVar();
   ImGui::PopStyleColor(2);

   // Update editor camera
   const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
   const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
   if (m_camMode == ViewMode::PreviewCam)
   {
      m_renderer->InitLayout();
      m_camView = RH2LH * m_renderer->GetMVP().GetView(0) * YAxis;
      m_camProj = YAxis * m_renderer->GetMVP().GetProj(0);
      // Keep the orbit distance consistent with the player camera, so that leaving the preview camera orbits around the table (not around a stale point)
      Matrix3D previewInv(m_camView);
      previewInv.Invert();
      const vec3 tableCenter(m_table->m_right * 0.5f, m_table->m_bottom * 0.5f, 0.f);
      m_camDistance = max((previewInv.GetOrthoNormalPos() - tableCenter).Dot(previewInv.GetOrthoNormalDir()), 100.f);
   }
   else
   {
      // Apply editor camera to renderer (move view/projection from right handed to left handed)
      // Convert from right handed (ImGuizmo view manipulate is right handed) to VPX's left handed coordinate system
      // Right Hand to Left Hand (note that RH2LH = inverse(RH2LH), so RH2LH.RH2LH is identity, which property is used below)
      const Matrix3D view = RH2LH * m_camView * YAxis;
      const Matrix3D proj = YAxis * m_camProj;
      m_renderer->SetViewProj(view, proj);

      if (m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop) // The desktop backdrop is a 2D view, always edited with an orthographic camera
      {
         // Convert from right handed (ImGuizmo view manipulate is right handed) to VPX's left handed coordinate system
         // Right Hand to Left Hand (note that RH2LH = inverse(RH2LH), so RH2LH.RH2LH is identity, which property is used below)
         //const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
         //const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, -1.f);
         //float zNear, zFar;
         //m_table->ComputeNearFarPlane(RH2LH * m_camView * YAxis, 1.f, zNear, zFar);
         // Near plane follows the orbit distance (a fixed 5 left a 10000:1 range, so near-coplanar surfaces z-fought when zoomed out: rails, lockdown bar, VLM lightmaps)
         const float zNear = clamp(m_camDistance * 0.05f, 5.f, 250.f);
         constexpr float zFar = 50000.f;
         m_camProj = Matrix3D::MatrixPerspectiveFovRH(m_navFovY, io.DisplaySize.x / io.DisplaySize.y, zNear, zFar);
      }
      else
      {
         constexpr float zNear = 0.5f;
         constexpr float zFar = 50000.f;
         const float viewHeight = m_camDistance;
         const float viewWidth = viewHeight * io.DisplaySize.x / io.DisplaySize.y;
         m_camProj = Matrix3D::MatrixOrthoOffCenterRH(-viewWidth, viewWidth, -viewHeight, viewHeight, zNear, -zFar);
      }
   }

   // Selection manipulator
   RailFixtureHandle(); // A support's handle on the route, ahead of the gizmo and the box select
   // A Wire Rail, as VPE: double-click the route to insert a point, double-click a point to delete it. Before the gizmo, which the first click of it put on the point
   if (m_pointEditPart != nullptr && !m_pointEditCenter && m_viewportVisible && (!io.WantCaptureMouse || ImGuizmo::IsOver()) && !io.KeyShift && !m_modal.active && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !m_railFixtureDrag.active
      && !ImGuizmo::IsViewManipulateHovered() && RailPointDoubleClick(HitTestDragPoint(ImGui::GetMousePos()), ImGui::GetMousePos()))
   {
      m_pointDragPending = false;
      m_boxSelectActive = false;
   }
   Matrix3D transform;
   const bool isSelectionTransformValid = GetSelectionTransform(transform);
   if (isSelectionTransformValid && !m_hadSelectionTransform && m_gizmoOperation == ImGuizmo::OPERATION(0) && !m_modal.active && !m_measure.IsActive())
      m_gizmoOperation = ImGuizmo::TRANSLATE; // A new selection shows the move gizmo (Esc hides it)
   m_hadSelectionTransform = isSelectionTransformValid;
   // A gizmo drag whose selection changed under the mouse (outliner click, undo,...) is over: the new selection must not follow it without an undo record
   bool gizmoLost = m_gizmoDragActive && (m_multiSel != m_gizmoDragSelection || m_undo.TopRecordId() != m_gizmoDragRecordId); // Selection or history changed under the drag
   if (gizmoLost)
   {
      ImGuizmo::Enable(false); // Clears ImGuizmo's using state
      ImGuizmo::Enable(true);
   }
   bool gizmoManipulated = false; // Manipulate called this frame with an operation
   // The view the gizmo works in (ImGuizmo has one active manipulation): the view under the mouse, a secondary view or the 3D Viewport; over a panel the last
   // one stays. A drag keeps the view it started in until it ends, wherever the mouse goes
   if (!ImGuizmo::IsUsingAny())
   {
      int hoveredSlot = -1;
      for (int i = 0; i < AreaLayout::kMaxViewLeaves; i++)
         if (m_viewInfo[i].valid && m_viewInfo[i].imageHovered)
            hoveredSlot = i;
      if (hoveredSlot >= 0)
         m_gizmoSlot = hoveredSlot;
      else if (m_viewportVisible && !io.WantCaptureMouse)
         m_gizmoSlot = -1; // Over the 3D Viewport
      if (m_gizmoSlot >= 0 && !m_viewInfo[m_gizmoSlot].valid)
         m_gizmoSlot = -1; // The view is gone
   }
   const SecondaryViewInfo *const gizmoView = m_gizmoSlot >= 0 && m_viewInfo[m_gizmoSlot].valid ? &m_viewInfo[m_gizmoSlot] : nullptr;
   if (isSelectionTransformValid && !m_table->IsLocked() && !m_modal.active && !gizmoLost && SelectionInView(m_gizmoSlot) && !m_railFixtureDrag.active
      && (m_gizmoSlot >= 0 ? (gizmoView != nullptr && m_pointEditPart == nullptr && m_camMode != ViewMode::DesktopBackdrop) : m_viewportVisible))
   {
      float camViewLH[16];
      Matrix3D secondaryProj;
      if (gizmoView != nullptr)
      {
         // A secondary view: the camera of its last render, factored the way the 3D Viewport's camera is (view = RH2LH * m_camView * YAxis, proj = YAxis * m_camProj;
         // ImGuizmo's view is RH2LH * m_camView)
         const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
         const Matrix3D secondaryView = gizmoView->view * YAxis;
         secondaryProj = YAxis * gizmoView->proj;
         memcpy(camViewLH, &secondaryView.m[0][0], sizeof(float) * 4 * 4);
         ImGuizmo::SetRect(gizmoView->rectMin.x, gizmoView->rectMin.y, gizmoView->rectSize.x, gizmoView->rectSize.y);
         ImGuizmo::SetDrawlist(gizmoView->drawList);
         ImGuizmo::SetOrthographic(fabsf(secondaryProj.m[2][3]) < 1e-6f);
      }
      else
      {
         memcpy(camViewLH, &m_camView.m[0][0], sizeof(float) * 4 * 4);
         for (int i = 8; i < 12; i++)
            camViewLH[i] = -camViewLH[i];
      }
      const bool groupGizmo = UsesGroupGizmo();
      Matrix3D groupMatrix;
      if (groupGizmo)
         groupMatrix = m_groupGizmo.active ? m_groupGizmo.current : GetGroupGizmoMatrix(m_gizmoSlot);
      Matrix3D &gizmoMatrix = groupGizmo ? groupMatrix : transform;
      const Matrix3D prevTransform(gizmoMatrix);
      ImGuizmo::OPERATION gizmoOperation = m_gizmoOperation;
      // A Wire Rail's route is 3D: its points keep the Z handle (the predefined views still restrict to their plane, below)
      const bool railPoints = m_pointEditPart && !m_pointEditCenter && gizmoOperation == ImGuizmo::TRANSLATE && m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive
         && static_cast<Primitive *>(m_pointEditPart->GetEditable())->IsRailRoute();
      if ((m_pointEditPart && !railPoints) || m_camMode == ViewMode::DesktopBackdrop || PlasticFromAtlas::ZLocked(*this))
      {
         // Drag point curves are 2D in the table XY plane, and backdrop parts are 2D in the backdrop XY plane: restrict gizmo operations to this plane
         if (gizmoOperation == ImGuizmo::TRANSLATE)
            gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y);
         else if (gizmoOperation == ImGuizmo::ROTATE)
            gizmoOperation = ImGuizmo::ROTATE_Z;
         else if (gizmoOperation == ImGuizmo::SCALE)
            gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y);
      }
      else if (m_camMode == ViewMode::EditorCam && gizmoView == nullptr && m_predefinedView != EditorUI::PredefinedView::None && groupGizmo
         && m_transformOrientation == TransformOrientation::View)
      {
         // The View orientation's X and Y are the screen's: a predefined view keeps those (its plane)
         if (gizmoOperation == ImGuizmo::TRANSLATE)
            gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y);
         else if (gizmoOperation == ImGuizmo::ROTATE)
            gizmoOperation = ImGuizmo::ROTATE_Z;
         else if (gizmoOperation == ImGuizmo::SCALE)
            gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y);
      }
      else if (m_camMode == ViewMode::EditorCam && gizmoView == nullptr)
      {
         // Predefined views are axis aligned 2D views: restrict gizmo operations to the view plane
         switch (m_predefinedView)
         {
         case EditorUI::PredefinedView::None: break;
         case EditorUI::PredefinedView::Left:
         case EditorUI::PredefinedView::Right:
            if (gizmoOperation == ImGuizmo::TRANSLATE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_Y | ImGuizmo::TRANSLATE_Z);
            else if (gizmoOperation == ImGuizmo::ROTATE)
               gizmoOperation = ImGuizmo::ROTATE_X;
            else if (gizmoOperation == ImGuizmo::SCALE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_Y | ImGuizmo::SCALE_Z);
            break;
         case EditorUI::PredefinedView::Top:
         case EditorUI::PredefinedView::Bottom:
            if (gizmoOperation == ImGuizmo::TRANSLATE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Y);
            else if (gizmoOperation == ImGuizmo::ROTATE)
               gizmoOperation = ImGuizmo::ROTATE_Z;
            else if (gizmoOperation == ImGuizmo::SCALE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Y);
            break;
         case EditorUI::PredefinedView::Front:
         case EditorUI::PredefinedView::Back:
            if (gizmoOperation == ImGuizmo::TRANSLATE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Z);
            else if (gizmoOperation == ImGuizmo::ROTATE)
               gizmoOperation = ImGuizmo::ROTATE_Y;
            else if (gizmoOperation == ImGuizmo::SCALE)
               gizmoOperation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Z);
            break;
         }
      }
      // Snapping: ImGuizmo takes the increment of the kind of operation (3 values for a translation, 1 for a rotation or a scale)
      float snapValues[3];
      if (PlasticFromAtlas::IsEditingHoles(*this) && (static_cast<int>(gizmoOperation) & (ImGuizmo::SCALE | ImGuizmo::SCALEU)) != 0)
         gizmoOperation = ImGuizmo::OPERATION(0); // S on holes is a diameter (the key S); the gizmo's scale would move the centres apart
      const float *snapPtr = nullptr;
      if ((static_cast<int>(gizmoOperation) & ImGuizmo::ROTATE) != 0)
      {
         if (IsSnapActive(SnapKind::Rotate) && m_snapRotate > 0.f)
         {
            snapValues[0] = m_snapRotate;
            snapPtr = snapValues;
         }
      }
      else if ((static_cast<int>(gizmoOperation) & ImGuizmo::SCALE) != 0)
      {
         if (IsSnapActive(SnapKind::Scale) && m_snapScale > 0.f)
         {
            snapValues[0] = m_snapScale;
            snapPtr = snapValues;
         }
      }
      else if (IsSnapActive(SnapKind::Move) && m_snapMove > 0.f)
      {
         snapValues[0] = snapValues[1] = snapValues[2] = m_snapMove;
         snapPtr = snapValues;
      }
      // Drawn inside the viewport area only (not over the panels beside it when the part is outside the narrow viewport)
      ImDrawList *const gizmoDrawList = ImGui::FindWindowByName("gizmo") != nullptr ? ImGui::FindWindowByName("gizmo")->DrawList : nullptr;
      const int gizmoFirstCmd = gizmoDrawList != nullptr ? max(0, gizmoDrawList->CmdBuffer.Size - 1) : 0;
      // ImGuizmo starts a drag only when no ImGui item is hovered or active: the image of a secondary view is one (its invisible button), so it is hidden from
      // ImGuizmo for the call (the 3D Viewport has no item under the mouse)
      ImGuiContext &imguiContext = *ImGui::GetCurrentContext();
      const ImGuiID savedIds[3] = { imguiContext.HoveredId, imguiContext.HoveredIdPreviousFrame, imguiContext.ActiveId };
      if (gizmoView != nullptr)
         for (ImGuiID *id : { &imguiContext.HoveredId, &imguiContext.HoveredIdPreviousFrame, &imguiContext.ActiveId })
            if (*id == gizmoView->itemId)
               *id = 0;
      // The letterbox and the header of the view's area are not the image: an item counts as hovered there, so ImGuizmo can not grab a handle nobody sees (clipped)
      if (gizmoView != nullptr && !ImGuizmo::IsUsingAny() && !(io.MousePos.x >= gizmoView->rectMin.x && io.MousePos.y >= gizmoView->rectMin.y
            && io.MousePos.x < gizmoView->rectMin.x + gizmoView->rectSize.x && io.MousePos.y < gizmoView->rectMin.y + gizmoView->rectSize.y))
         imguiContext.HoveredId = gizmoView->itemId;
      // Local: one part's own matrix, or a group's on the active part's axes. View: a group's matrix on the view's axes (see GetGroupGizmoMatrix)
      const bool gizmoLocal = IsOwnFrameOrientation(m_transformOrientation)
         || (groupGizmo && m_transformOrientation == TransformOrientation::View && m_pointEditPart == nullptr && m_camMode != ViewMode::DesktopBackdrop);
      ImGuizmo::Manipulate(camViewLH, gizmoView != nullptr ? (float *)(secondaryProj.m) : (float *)(m_camProj.m), gizmoOperation, gizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
         (float *)(gizmoMatrix.m), nullptr, snapPtr);
      imguiContext.HoveredId = savedIds[0];
      imguiContext.HoveredIdPreviousFrame = savedIds[1];
      imguiContext.ActiveId = savedIds[2];
      ClipDrawCommands(gizmoDrawList, gizmoFirstCmd, m_viewportVisible ? &m_viewportRect : nullptr);
      gizmoManipulated = gizmoOperation != ImGuizmo::OPERATION(0);
      if (gizmoView != nullptr)
      {
         // Back to the 3D Viewport's context for what follows (the nav cube)
         ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
         ImGuizmo::SetDrawlist(ImGui::FindWindowByName("gizmo")->DrawList);
         ImGuizmo::SetOrthographic(m_camMode == ViewMode::DesktopBackdrop || (m_camMode == ViewMode::EditorCam && !m_perspectiveCam));
      }
      if (memcmp(gizmoMatrix.m, prevTransform.m, 16 * sizeof(float)) != 0)
      {
         // Mark all selected parts for undo once per drag (m_lastUndoId is reset when the gizmo is released)
         if (!m_gizmoDragActive && m_table->m_liveBaseTable == nullptr)
         {
            m_gizmoDragActive = true;
            m_gizmoDragSelection = m_multiSel;
            m_undo.BeginUndo();
            for (const auto &part : m_multiSel)
               m_undo.MarkForUndo(part->GetEditable());
            MarkDependentsForUndo();
            m_undo.SetLabel(CountLabel(((static_cast<int>(m_gizmoOperation) & ImGuizmo::ROTATE) != 0) ? "Rotate"
                  : ((static_cast<int>(m_gizmoOperation) & ImGuizmo::SCALE) != 0)                     ? "Scale"
                                                                                                      : "Move",
               m_multiSel.size(), m_multiSel.empty() ? string() : m_multiSel.front()->GetEditable()->GetName()));
            m_undo.EndUndo();
            m_gizmoDragRecordId = m_undo.TopRecordId();
            m_lastUndoPart = nullptr;
            m_lastUndoId = 0x1000;
            m_gizmoStartTransform = groupGizmo ? transform : prevTransform;
            m_gizmoDragTranslate = (static_cast<int>(m_gizmoOperation) & (ImGuizmo::ROTATE | ImGuizmo::SCALE | ImGuizmo::SCALEU)) == 0 && !m_pointEditPart;
         }
         if (groupGizmo)
         {
            if (!m_groupGizmo.active)
            {
               m_groupGizmo.active = true;
               m_groupGizmo.start = prevTransform;
               BeginSelectionXform(m_groupGizmo.xform);
               m_groupGizmo.xform.pivot = vec3(prevTransform._41, prevTransform._42, prevTransform._43);
               m_groupGizmo.yaw = 0.f;
               m_groupGizmo.yawStep = 0.f;
               m_groupGizmo.ownFrame = m_pointEditPart == nullptr && IsOwnFrameOrientation(m_transformOrientation);
               m_groupGizmo.axis = vec3(0.f, 0.f, 0.f);
               m_groupGizmo.angle = 0.f;
            }
            m_groupGizmo.current = gizmoMatrix;
            // What the gizmo did since the drag began, as a move of its origin and a linear map about it
            Matrix3D inverse(m_groupGizmo.start);
            inverse.Invert();
            XformOp op;
            op.linear = inverse * gizmoMatrix;
            op.move = vec3(gizmoMatrix._41 - m_groupGizmo.start._41, gizmoMatrix._42 - m_groupGizmo.start._42, gizmoMatrix._43 - m_groupGizmo.start._43);
            op.linear._41 = op.linear._42 = op.linear._43 = 0.f;
            op.kind = (static_cast<int>(gizmoOperation) & ImGuizmo::ROTATE) != 0 ? XformKind::Rotate
               : (static_cast<int>(gizmoOperation) & (ImGuizmo::SCALE | ImGuizmo::SCALEU)) != 0                ? XformKind::Scale
                                                                                                                : XformKind::Move;
            if (m_groupGizmo.ownFrame)
            {
               // Local: the gizmo sits on the active part's axes (the rows of its start matrix), each part does in its own axes what it did in those
               // (L_local = F L F^T, move_local = move F^T)
               Matrix3D axes(m_groupGizmo.start), axesT;
               axes._41 = axes._42 = axes._43 = 0.f;
               for (int i = 0; i < 3; i++)
                  for (int j = 0; j < 3; j++)
                     axesT.m[i][j] = axes.m[j][i];
               axesT._41 = axesT._42 = axesT._43 = 0.f;
               op.linear = axes * op.linear * axesT;
               op.move = axesT.MultiplyVectorNoTranslate(op.move);
               op.ownFrame = true;
               if (op.kind == XformKind::Rotate)
               {
                  // A drag turns about one axis: take it on the first frame that has one, then the signed angle about it, continuous past a half turn
                  const Matrix3D &l = op.linear;
                  const vec3 sv(0.5f * (l._23 - l._32), 0.5f * (l._31 - l._13), 0.5f * (l._12 - l._21));
                  const float c = 0.5f * (l._11 + l._22 + l._33 - 1.f);
                  if (m_groupGizmo.axis.LengthSquared() < 0.5f && sv.Length() > 1e-4f)
                     m_groupGizmo.axis = sv * (1.f / sv.Length());
                  if (m_groupGizmo.axis.LengthSquared() > 0.5f)
                  {
                     float angle = atan2f(sv.Dot(m_groupGizmo.axis), c);
                     angle += roundf((m_groupGizmo.angle - angle) / (2.f * static_cast<float>(M_PI))) * 2.f * static_cast<float>(M_PI);
                     m_groupGizmo.angle = angle;
                     op.hasYaw = true;
                     op.angle = angle;
                     op.axis = m_groupGizmo.axis;
                  }
               }
            }
            else if (op.kind == XformKind::Rotate)
            {
               // The Z share read from the matrix is only known up to steps of 2 pi x the share: take the one nearest to the last frame, so a drag past a half turn goes on
               float step;
               op.yaw = YawOfRotation(op.linear, step);
               if (fabsf(step) > 1e-4f)
               {
                  op.yaw += roundf((m_groupGizmo.yaw - op.yaw) / step) * step;
                  m_groupGizmo.yawStep = step;
               }
               else if (step == 0.f) // On a full turn: a whole number of steps, the one nearest to the last frame
                  op.yaw = fabsf(m_groupGizmo.yawStep) > 1e-4f ? roundf(m_groupGizmo.yaw / m_groupGizmo.yawStep) * m_groupGizmo.yawStep : m_groupGizmo.yaw;
               op.hasYaw = true;
               m_groupGizmo.yaw = op.yaw;
            }
            ApplySelectionXform(m_groupGizmo.xform, op);
            m_groupGizmo.last = op;
         }
         else
            SetSelectionTransform(transform);
      }
   }
   // A drag interrupted with the mouse still held (Alt+A, Esc hiding the gizmo, a modal transform starting, the table getting locked,...) is not manipulated
   // anymore, so ImGuizmo would stay in its using state: reset it
   // Also when the handle was only pressed (no change yet, so no drag record): IsOver() would stay true and the viewport clicks would die
   if (!gizmoManipulated && ImGuizmo::IsUsingAny())
   {
      gizmoLost |= m_gizmoDragActive;
      ImGuizmo::Enable(false);
      ImGuizmo::Enable(true);
   }
   // Reset gizmo undo deduplication once the gizmo is released (each drag should create a single undo entry)
   if (m_gizmoDragActive && (gizmoLost || !ImGuizmo::IsUsing()))
   {
      m_gizmoDragActive = false;
      FlushRubberDrag();
      FlushDrag();
      m_gizmoDragSelection.clear();
      m_lastUndoPart = nullptr;
      m_lastUndoId = 0;
      // A finished translation can be adjusted (the rotation and scale gizmos are not: their axes can not be recovered from the matrices)
      Matrix3D released;
      if (!gizmoLost && m_gizmoDragTranslate && !m_multiSel.empty() && GetSelectionTransform(released)) // No adjustable operation for an interrupted drag
      {
         SetLastOperation(LastOperation::Type::Move, m_gizmoStartTransform);
         m_lastOp.offset = vec3(released._41 - m_gizmoStartTransform._41, released._42 - m_gizmoStartTransform._42, released._43 - m_gizmoStartTransform._43);
         if (m_groupGizmo.active && !m_groupGizmo.xform.parts.empty())
         {
            // A group: adjusting re-evaluates the move from the snapshot of the drag, by the gizmo's own move (a part may not take all of it, a Bumper's Z for one)
            m_lastOp.xform = m_groupGizmo.xform;
            m_lastOp.offset = m_groupGizmo.last.move; // In each part's own axes with Local
            m_lastOp.ownFrame = m_groupGizmo.last.ownFrame;
         }
      }
      m_gizmoDragTranslate = false;
   }
   if (m_groupGizmo.active && (gizmoLost || !ImGuizmo::IsUsing()))
   {
      m_groupGizmo.active = false;
      m_groupGizmo.xform = SelectionXform();
   }

   m_renderer->SetShadeMode(m_shadeMode);
   m_renderer->SetViewShade(m_camMode == ViewMode::EditorCam && !IsInspectMode() ? m_viewShade : ViewShadeOptions {}); // The options are the editor view's only
   m_renderer->SetLocalView(m_localView.ForRenderer());

   // Selection and physic colliders overlay (suspended while a save is running: the part renders
   // would write into the parts' data being serialized on the worker thread, see Render3D)
   {
      LiveRenderContext ctx(m_player, overlayDrawList, m_camMode, m_shadeMode, (m_table->m_liveBaseTable != nullptr) && m_player->IsPlaying());
      ctx.m_selectionDisplay = m_navSelectionDisplay;
      PlasticFromAtlas::DrawBackdrop(*this, overlayDrawList, ctx); // The atlas under the points (trace state, Tab on a placed plastic)
      m_refImage.DrawOverlay(*this, overlayDrawList, ctx); // The outline of the reference image, the points of a two point scale
      if (!m_multiSel.empty() && m_selectionOverlay && !IsSaveInProgress())
      {
         if (m_renderer->m_renderDevice->GetCurrentRenderTarget()->HasDepth())
            m_renderer->m_renderDevice->Clear(clearType::ZBUFFER, 0);
         ctx.m_isSelected = true;
         for (const auto &part : m_multiSel)
         {
            if (!m_localView.Allows(part->GetEditable()))
               continue; // Selected, but not in the Local View: nothing of it is drawn
            ctx.m_isActive = part == m_selection.GetPart();
            part->Render(ctx);
         }
         ctx.m_isActive = false;
         ctx.m_isSelected = false;
      }

      if (isSelectionTransformValid && SelectionInView(-1)) // Not for a selection the view does not show
      {
         const ImVec2 pos = ctx.Project(transform.GetOrthoNormalPos());
         // Not over the panels beside the viewport (the part may be outside the narrow viewport)
         if (!m_viewportVisible || (pos.x >= m_viewportRect.x0 && pos.x < m_viewportRect.x1 && pos.y >= m_viewportRect.y0 && pos.y < m_viewportRect.y1))
            overlayDrawList->AddCircleFilled(pos, 3.f * m_liveUI.GetDPI(), IM_COL32(255, 255, 255, 255), 16);
      }

      if (m_viewportVisible)
         DrawCursor3D(overlayDrawList, ctx, ImVec2(m_viewportRect.x0, m_viewportRect.y0), ImVec2(m_viewportRect.x1, m_viewportRect.y1));

      if (m_modal.active && m_modal.viewSlot < 0 && m_viewportVisible)
      {
         // Modal readout on the viewport header line (top centre, on a dark pill), like Blender's header text
         const float dpi = m_liveUI.GetDPI();
         const ImVec2 textSize = ImGui::CalcTextSize(m_modal.hud.c_str());
         const float x = (m_viewportContentRect.x0 + m_viewportContentRect.x1 - textSize.x) * 0.5f, y = m_viewportContentRect.y0 + 8.f * dpi;
         overlayDrawList->AddRectFilled(ImVec2(x - 10.f * dpi, y - 3.f * dpi), ImVec2(x + textSize.x + 10.f * dpi, y + textSize.y + 3.f * dpi), IM_COL32(20, 20, 22, 200), 4.f * dpi);
         overlayDrawList->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255), m_modal.hud.c_str());
      }

      if (m_modal.active && m_modal.viewSlot >= 0 && m_viewInfo[m_modal.viewSlot].valid)
      {
         // The same readout at the top of the secondary view the transform works in
         const SecondaryViewInfo &info = m_viewInfo[m_modal.viewSlot];
         const float dpi = m_liveUI.GetDPI();
         const ImVec2 textSize = ImGui::CalcTextSize(m_modal.hud.c_str());
         const float x = info.rectMin.x + (info.rectSize.x - textSize.x) * 0.5f, y = info.rectMin.y + 8.f * dpi;
         ImDrawList *const hudList = ImGui::GetForegroundDrawList();
         hudList->PushClipRect(info.rectMin, ImVec2(info.rectMin.x + info.rectSize.x, info.rectMin.y + info.rectSize.y), true);
         hudList->AddRectFilled(ImVec2(x - 10.f * dpi, y - 3.f * dpi), ImVec2(x + textSize.x + 10.f * dpi, y + textSize.y + 3.f * dpi), IM_COL32(20, 20, 22, 200), 4.f * dpi);
         hudList->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255), m_modal.hud.c_str());
         hudList->PopClipRect();
      }

      // In desktop backdrop mode, draw the bounds of the backdrop area
      if (m_camMode == ViewMode::DesktopBackdrop)
      {
         const Vertex3Ds bounds[4] = { Vertex3Ds(0.f, 0.f, 0.f), Vertex3Ds((float)EDITOR_BG_WIDTH, 0.f, 0.f), Vertex3Ds((float)EDITOR_BG_WIDTH, (float)EDITOR_BG_HEIGHT, 0.f),
            Vertex3Ds(0.f, (float)EDITOR_BG_HEIGHT, 0.f) };
         for (int i = 0; i < 4; i++)
            ctx.DrawLine(bounds[i], bounds[(i + 1) % 4], IM_COL32(255, 255, 255, 64));
      }

      // In drag point edit mode, render the drag points of the edited part's curve (or its center point)
      PlasticFromAtlas::DrawPointEdit(*this, overlayDrawList, ctx);
      if (m_pointEditPart && m_localView.Allows(m_pointEditPart->GetEditable())) // A part the Local View hides has no points drawn
      {
         if (m_pointEditCenter)
         {
            const ImVec2 pos = ctx.Project(m_pointEditPart->GetEditCenter());
            if (pos.x != FLT_MAX)
            {
               const ImU32 color = m_centerSelected ? IM_COL32(255, 128, 0, 255) : IM_COL32(80, 160, 255, 255);
               const float radius = 5.f * m_liveUI.GetDPI();
               overlayDrawList->AddCircleFilled(pos, radius, color, 12);
               overlayDrawList->AddCircle(pos, radius + m_liveUI.GetDPI(), IM_COL32(0, 0, 0, 255), 12, 1.5f);
            }
         }
         else
         {
            const auto &points = m_pointEditPart->GetDragPointCurve()->GetPoints();
            for (size_t i = 0; i < points.size(); i++)
            {
               const ImVec2 pos = ctx.Project(Vertex3Ds(points[i]->GetX(), points[i]->GetY(), m_pointEditPart->GetDragPointZ(points[i].get())));
               if (pos.x == FLT_MAX)
                  continue;
               const ImU32 color = IsPointSelected(points[i].get()) ? IM_COL32(255, 128, 0, 255) : (points[i]->IsSmooth() ? IM_COL32(80, 160, 255, 255) : IM_COL32(255, 96, 96, 255));
               const float radius = (i == 0 ? 5.f : 4.f) * m_liveUI.GetDPI(); // First point is drawn slightly larger to mark the curve start
               overlayDrawList->AddCircleFilled(pos, radius, color, 12);
               overlayDrawList->AddCircle(pos, radius + m_liveUI.GetDPI(), IM_COL32(0, 0, 0, 255), 12, 1.5f);
            }
         }
      }

      PlasticFromAtlas::DrawTrace(*this, overlayDrawList, ctx);
      PlayfieldMaker::Draw(*this, overlayDrawList, ctx);

      if (m_physOverlay == PhysicOverlay::All || (m_physOverlay == PhysicOverlay::Selected && !m_multiSel.empty()))
      {
         auto project = [ctx](Vertex3Ds v)
         {
            const ImVec2 pt = ctx.Project(v);
            return Vertex2D(pt.x, pt.y);
         };
         ImGui::PushStyleColor(ImGuiCol_PlotLines, IM_COL32(255, 0, 0, 255)); // We abuse ImGui colors to pass render colors
         ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(255, 0, 0, 64));
         // Gameplay colliders: the dragged parts' ones are rebuilt per frame while this overlay is on (see SetSelectionTransform), else at the drag end
         for (auto pho : m_player->m_physics->GetHitObjects())
            if (pho != nullptr && (pho->m_editable == nullptr || m_localView.Allows(pho->m_editable)) // Only the colliders of the parts the Local View shows
               && (m_physOverlay == PhysicOverlay::All || (m_physOverlay == PhysicOverlay::Selected && IsEditableSelected(pho->m_editable))))
               pho->DrawUI(project, overlayDrawList, true);
         ImGui::PopStyleColor(2);
      }
   }

   if (IsInspectMode() && m_viewportVisible)
   {
      // Play test: 2 px accent frame around the viewport area, so the running copy is not mistaken for the edit session
      const AreaRect &r = m_viewportRect;
      ImGui::GetBackgroundDrawList()->AddRect(ImVec2(r.x0 + 1.f, r.y0 + 1.f), ImVec2(r.x1 - 1.f, r.y1 - 1.f), EditorTheme::Accent, 0.f, 0, 2.f);
   }
   RenderNavCube();

   // Handle uncaught mouse & keyboard shortcuts (a modal transform in progress owns the mouse and keyboard)
   const bool modalWasActive = m_modal.active;
   if (modalWasActive && m_fileOp == nullptr)
      UpdateModalTransform();
   bool measureCanStart = false; // The Measure tool takes the press (the 3D Viewport's clicks are its own then)
   if (!io.WantCaptureMouse && m_viewportVisible && m_fileOp == nullptr && !modalWasActive) // Viewport mouse input: not with a panel maximized over it
   {
      // Trackpad: two finger scroll pans the view (Ctrl / Cmd + scroll zooms)
      if (m_navTrackpad && (io.MouseWheel != 0 || io.MouseWheelH != 0) && !io.KeyCtrl && !io.KeySuper)
      {
         if (m_camMode == ViewMode::PreviewCam)
            m_camMode = ViewMode::EditorCam;
         Matrix3D view(m_camView);
         view.Invert();
         const vec3 up = view.GetOrthoNormalUp(), dir = view.GetOrthoNormalDir(), right = view.GetOrthoNormalRight(), pos = view.GetOrthoNormalPos();
         const vec3 camTarget = pos - dir * m_camDistance;
         const float halfH = m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop ? m_camDistance * tanf(0.5f * ANGTORAD(m_navFovY)) : m_camDistance;
         const float unitsPerPixel = 2.f * halfH / io.DisplaySize.y;
         const vec3 offset = (right * -io.MouseWheelH + up * io.MouseWheel) * (20.f * unitsPerPixel);
         m_camView = Matrix3D::MatrixLookAtRH(pos + offset, camTarget + offset, up);
      }
      // Zoom in/out with mouse wheel
      else if (io.MouseWheel != 0)
      {
         if (m_camMode == ViewMode::PreviewCam)
            m_camMode = ViewMode::EditorCam;
         Matrix3D view(m_camView);
         view.Invert();
         const vec3 up = view.GetOrthoNormalUp(), dir = view.GetOrthoNormalDir(), pos = view.GetOrthoNormalPos();
         vec3 camTarget = pos - dir * m_camDistance;
         const float zoomFactor = powf(1.1f, (m_navInvertZoom ? 1.f : -1.f) * ImGui::GetIO().MouseWheel);
         vec3 newEye;
         if (m_navZoomToMouse && m_camMode != ViewMode::PreviewCam)
         {
            // Zoom toward the point under the cursor: keep the point of the focal plane under the mouse at the same screen position
            const vec3 right = view.GetOrthoNormalRight();
            const float halfH = m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop ? m_camDistance * tanf(0.5f * ANGTORAD(m_navFovY)) : m_camDistance;
            const ImVec2 mouse = ImGui::GetMousePos();
            const float ndcX = 2.f * mouse.x / io.DisplaySize.x - 1.f, ndcY = 1.f - 2.f * mouse.y / io.DisplaySize.y;
            const vec3 offset = right * (ndcX * halfH * io.DisplaySize.x / io.DisplaySize.y) + up * (ndcY * halfH);
            camTarget += offset * (1.f - zoomFactor);
            newEye = camTarget + dir * m_camDistance * zoomFactor;
         }
         else
         {
            // Zoom about the selection center (the eye moves along the line to it, the view direction is kept so nothing recenters)
            vec3 pivot = camTarget;
            GetNavPivot(pivot);
            newEye = pivot + (pos - pivot) * zoomFactor;
         }
         m_camDistance *= zoomFactor;
         m_camView = Matrix3D::MatrixLookAtRH(newEye, newEye - dir * m_camDistance, up);
      }

      // Mouse pan / orbit. Optionally (preference), Alt/Option + left drag acts as the middle button (no middle button on a trackpad / Magic Mouse)
      const ImGuiMouseButton navButton = (m_navAltMiddleButton && io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ? ImGuiMouseButton_Left : ImGuiMouseButton_Middle;
      if (!ImGui::IsMouseDown(navButton))
         m_orbitDragging = false;
      else
      {
         if (!m_orbitDragging)
         {
            // Orbit pivot, fixed for the whole drag: the selection center, else (auto depth) the 3D point under the cursor, else the view target
            m_orbitDragging = true;
            m_orbitPivotValid = m_camMode != ViewMode::PreviewCam && m_camMode != ViewMode::DesktopBackdrop
               && (GetNavPivot(m_orbitPivot) || (m_navAutoDepth && PickDepthPoint(ImGui::GetMousePos(), m_orbitPivot)));
         }
         ImVec2 drag = ImGui::GetMouseDragDelta(navButton);
         ImGui::ResetMouseDragDelta(navButton);
         if (m_camMode != ViewMode::PreviewCam && (drag.x != 0.f || drag.y != 0.f))
         {
            Matrix3D viewInverse(m_camView);
            viewInverse.Invert();
            vec3 dir = viewInverse.GetOrthoNormalDir();
            const vec3 up = viewInverse.GetOrthoNormalUp();
            const vec3 pos = viewInverse.GetOrthoNormalPos();
            const vec3 right = viewInverse.GetOrthoNormalRight();
            vec3 camTarget = pos - dir * m_camDistance;
            if (io.KeyShift || m_camMode == ViewMode::DesktopBackdrop || m_orbitLock)
            {
               // Pan so that the point under the cursor follows it (the view height at the orbit target depends on the projection)
               const float halfH = m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop ? m_camDistance * tanf(0.5f * ANGTORAD(m_navFovY)) : m_camDistance;
               drag.x *= 2.f * halfH / io.DisplaySize.y;
               drag.y *= 2.f * halfH / io.DisplaySize.y;
               camTarget = camTarget - right * drag.x + up * drag.y;
               m_camView = Matrix3D::MatrixLookAtRH(pos - right * drag.x + up * drag.y, camTarget, up);
            }
            else
            {
               m_predefinedView = PredefinedView::None;
               const Matrix3D rx = Matrix3D::MatrixRotate(drag.x * 0.01f * m_navOrbitSensitivity, up);
               const Matrix3D ry = Matrix3D::MatrixRotate(drag.y * 0.01f * m_navOrbitSensitivity, right);
               const Matrix3D roll = rx * ry;
               const vec3 pivot = m_orbitPivotValid ? m_orbitPivot : camTarget;
               const vec3 offset = roll.MultiplyVectorNoPerspective(pos - pivot); // Rigid rotation of the camera about the pivot (no recentering)
               dir = roll.MultiplyVectorNoPerspective(dir);
               dir.Normalize();

               // clamp
               vec3 planDir = CrossProduct(right, up);
               planDir.y = 0.f;
               planDir.Normalize();
               if (float dt = planDir.Dot(dir); dt < 0.0f)
               {
                  dir += planDir * dt;
                  dir.Normalize();
               }

               const vec3 newEye = pivot + offset;
               m_camView = Matrix3D::MatrixLookAtRH(newEye, newEye - dir * m_camDistance, up);
            }
         }
      }

      // 3D cursor: Shift+right click puts it on the surface under the mouse (a part, else the playfield plane)
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt && !ImGuizmo::IsOver() && !ImGuizmo::IsViewManipulateHovered()
         && !ImGuizmo::IsUsingViewManipulate() && m_camMode != ViewMode::DesktopBackdrop)
         Place3DCursor(ImGui::GetMousePos(), nullptr);

      // Select: click selects the front-most part (click again to cycle through overlapping parts),
      // shift+click toggles a part in the multi selection, drag box selects parts (shift+drag adds to the selection)
      // In drag point edit mode, click selects a drag point of the edited curve (shift toggles), dragging a
      // selected point moves the selected points in the table XY plane, drag box selects points
      measureCanStart = m_measure.IsActive() && !(m_navAltMiddleButton && io.KeyAlt) && !ImGuizmo::IsViewManipulateHovered() && !ImGuizmo::IsUsingViewManipulate();
      if (!m_measure.IsActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !(m_navAltMiddleButton && io.KeyAlt) && !ImGuizmo::IsOver() && !ImGuizmo::IsViewManipulateHovered()
         && !ImGuizmo::IsUsingViewManipulate() && !m_railFixtureDrag.active) // Alt+click is orbit when Alt acts as the middle button
      {
         m_boxSelectStart = ImGui::GetMousePos();
         if (m_refImage.PickActive())
            m_refImage.Click(*this, m_boxSelectStart); // Reference image: a point of the two point scale
         else if (!m_plasticTrace.active && !m_plasticFit.active && m_pointEditPart == nullptr && PlayfieldMaker::Click(*this, m_boxSelectStart))
         {
            // Playfield Maker: a hole placed, selected, or a hole grab confirmed
         }
         else if (m_plasticTrace.active)
            PlasticFromAtlas::Click(*this, m_boxSelectStart, io.KeyShift);
         else if (m_plasticFit.active)
            PlasticFromAtlas::FitClick(*this, m_boxSelectStart);
         else if (m_partLibrary.IsPlacementArmed())
         {
            // Part library placement mode: drop the previewed part at its ghost position
            m_partLibrary.CommitPlacement();
         }
         else if (m_addPartType != eItemInvalid)
         {
            // Add part mode: create the pending part at the picked point of the playfield XY plane
            const ItemTypeEnum type = m_addPartType;
            m_addPartType = eItemInvalid;
            CreatePart(type, UnprojectToPlane(m_boxSelectStart, 0.f), m_addWireform && type == eItemPrimitive, m_addWireRail && type == eItemPrimitive);
         }
         else if (m_pointEditPart != nullptr && m_pointEditCenter)
         {
            // Center point edit mode: clicking on the center selects it and starts a pending drag
            if (HitTestEditCenter(m_boxSelectStart))
            {
               if (io.KeyShift)
                  m_centerSelected = !m_centerSelected;
               else
                  m_centerSelected = true;
               m_pointDragPending = m_centerSelected;
               m_pointDragZ = m_pointEditPart->GetEditCenter().z;
               m_pointDragPos = UnprojectToPlane(m_boxSelectStart, m_pointDragZ);
            }
            else
               m_boxSelectActive = true;
         }
         else
         {
            DragPoint *const hitPoint = (m_pointEditPart != nullptr) ? HitTestDragPoint(m_boxSelectStart) : nullptr;
            if (hitPoint != nullptr)
            {
               if (io.KeyShift)
                  TogglePointSelection(hitPoint);
               else if (!IsPointSelected(hitPoint))
               {
                  m_pointSel.clear();
                  m_pointSel.push_back(hitPoint);
               }
               m_pointDragPending = IsPointSelected(hitPoint) && !hitPoint->m_uiLocked;
               m_pointDragZ = m_pointEditPart->GetDragPointZ(hitPoint);
               m_pointDragPos = UnprojectToPlane(m_boxSelectStart, m_pointDragZ);
            }
            else if (m_pointEditPart == nullptr
               || !(PlasticFromAtlas::HolePress(*this, m_boxSelectStart, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                  || PlasticFromAtlas::BowPress(*this, m_boxSelectStart, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))))
               m_boxSelectActive = true;
         }
      }
      if (m_boxSelectActive)
      {
         if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
         {
            const ImVec2 end = ImGui::GetMousePos();
            if (fabsf(end.x - m_boxSelectStart.x) > 2.f || fabsf(end.y - m_boxSelectStart.y) > 2.f)
            {
               overlayDrawList->AddRectFilled(m_boxSelectStart, end, IM_COL32(255, 128, 0, 32));
               overlayDrawList->AddRect(m_boxSelectStart, end, IM_COL32(255, 128, 0, 255));
            }
         }
         else
         {
            m_boxSelectActive = false;
            const ImVec2 end = ImGui::GetMousePos();
            if (m_pointEditPart)
            {
               if (fabsf(end.x - m_boxSelectStart.x) > 4.f || fabsf(end.y - m_boxSelectStart.y) > 4.f)
                  BoxSelectPoints(m_boxSelectStart, end, io.KeyShift);
               else if (!io.KeyShift)
               {
                  m_pointSel.clear();
                  m_centerSelected = false;
               }
            }
            else if (fabsf(end.x - m_boxSelectStart.x) > 4.f || fabsf(end.y - m_boxSelectStart.y) > 4.f)
               BoxSelectParts(m_boxSelectStart, end, io.KeyShift);
            else
            {
               SelectAtPoint(end, nullptr);
            }
         }
      }

      PlasticFromAtlas::BowDrag(*this);
      PlasticFromAtlas::PillDrag(*this);
      PlasticFromAtlas::ViewportRightClick(*this);

      // Drag the selected drag points in the table XY plane
      if (m_pointEditPart && (m_pointDragPending || m_pointDragActive))
      {
         if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
         {
            const Vertex2D pos = UnprojectToPlane(ImGui::GetMousePos(), m_pointDragZ);
            const Vertex2D delta(pos.x - m_pointDragPos.x, pos.y - m_pointDragPos.y);
            if (m_pointDragPending)
            {
               const ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
               if (fabsf(dragDelta.x) > 2.f || fabsf(dragDelta.y) > 2.f)
               {
                  // Drag actually starts: mark the edited part for undo once per drag
                  m_pointDragPending = false;
                  m_pointDragActive = m_pointEditCenter ? m_centerSelected : !m_pointSel.empty();
                  if (m_pointDragActive)
                  {
                     m_undo.BeginUndo();
                     m_undo.MarkForUndo(m_pointEditPart->GetEditable());
                     m_undo.SetFieldLabel("Move Points");
                     m_undo.EndUndo();
                  }
               }
            }
            if (m_pointDragActive && (delta.x != 0.f || delta.y != 0.f))
            {
               if (m_pointEditCenter)
               {
                  const Vertex3Ds center = m_pointEditPart->GetEditCenter();
                  m_pointEditPart->SetEditCenter(Vertex2D(center.x + delta.x, center.y + delta.y));
               }
               else
               {
                  DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve();
                  for (DragPoint *point : m_pointSel)
                     point->Translate(delta);
                  curve->OnPointsModified();
               }
               DetachRubberRefs(m_pointEditPart->GetEditable());
               m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
               m_renderer->InvalidateShadowContent();
               m_player->m_physics->Update(m_pointEditPart->GetEditable());
               UpdateRampWallsPhysics(m_player, m_pointEditPart->GetEditable());
            }
            m_pointDragPos = pos;
         }
         else
         {
            m_pointDragPending = false;
            m_pointDragActive = false;
         }
      }
   }
   if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
   {
      m_boxSelectActive = false; // Canceled: button released outside of the playfield
      m_pointDragPending = false; // Same for a point drag (it would keep undo / redo off until the mouse comes back to the viewport)
      m_pointDragActive = false;
      PlasticFromAtlas::PillEnd(*this); // Same for a pill drag
      m_plasticBow = PlasticFromAtlas::Bow(); // Same for a bow: it is only driven inside the gate, a stale one would jump to the cursor on the next press
   }
   m_measure.UpdateMain(*this, overlayDrawList, measureCanStart); // The rulers, and the tool's mouse
   m_measure.EndFrame(*this); // After the secondary views and the 3D Viewport had their turn
   // Ctrl+S must work while a widget (e.g. a slider) has keyboard focus; only a text input being edited takes precedence
   if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false) && io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
      SaveTable();
   // Search to field palette (Cmd+K on macOS, where ImGui maps Cmd to Ctrl)
   // Tab and Shift+Tab belong to the editor (drag point edit, snapping) while the mouse is over the viewport and no text is edited: ImGui must not use them
   // to tab into a field of a panel that kept the navigation focus. The setting is read by ImGui at the start of the next frame.
   ImGui::GetCurrentContext()->ConfigNavEnableTabbing = io.WantCaptureMouse || io.WantTextInput;
   // Inspecting a running game: the keys belong to the game (flippers, plunger, nudge,...), only Ctrl+S and the palette stay editor keys
   const bool gameKeys = IsInspectMode() && m_player->IsGameRunning();
   const bool editorPanels
      = !m_inspectionModal.IsVisible() && !m_flyMode && !modalWasActive; // The palette and workspaces need the editor panels (and not during a G / R / S modal transform)
   const bool panelsShown = editorPanels && !gameKeys;
   // Also while a field has keyboard focus (e.g. the one the palette just went to): no text field uses Ctrl+K
   if (editorPanels
      && ((ImGui::IsKeyPressed(ImGuiKey_K, false) && io.KeyCtrl && !io.KeyAlt && !io.KeyShift) || (ImGui::IsKeyPressed(ImGuiKey_F3, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)))
      m_palette.Toggle();
   // Workspaces: Ctrl+PgUp / PgDn cycle them (wrapping), Ctrl+1..3 pick one unless the digits emulate the numpad views. Handled before the keyboard capture
   // check: the workspaces showing a text editor (script) would otherwise never receive the keys, as the editor consumes them
   const bool workspaceKeysAllowed = panelsShown && m_fileOp == nullptr && !m_palette.IsOpen();
   if (!workspaceKeysAllowed && io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_PageUp, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false)))
      PLOGI << "Workspace key ignored: panelsShown=" << panelsShown << " fileOp=" << (m_fileOp != nullptr) << " palette=" << m_palette.IsOpen() << " modal=" << modalWasActive;
   if (workspaceKeysAllowed && io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
   {
      // Plain key test (owner Any): ImGui::Shortcut would lose the chord to a multiline text field (the script editor), which owns PageUp / PageDown.
      // While a text field is being edited the keys are its own (they page the script text), so they do not also switch the workspace
      const bool textKeys = io.WantTextInput;
      if (textKeys && (ImGui::IsKeyPressed(ImGuiKey_PageUp, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false)))
         PLOGI << "Workspace key left to the text field being edited";
      const bool nextWorkspace = !textKeys && ImGui::IsKeyPressed(ImGuiKey_PageDown, false);
      const bool previousWorkspace = !textKeys && ImGui::IsKeyPressed(ImGuiKey_PageUp, false);
      if (nextWorkspace || previousWorkspace)
         SwitchWorkspace(
            (max(m_pinned.GetActiveWorkspace(), 0) + (nextWorkspace ? 1 : PinnedPanel::WorkspaceCount - 1)) % PinnedPanel::WorkspaceCount); // Blender: Ctrl+PgUp/PgDn cycle workspaces
      else if (!m_navEmulateNumpad && !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_1, false) || ImGui::IsKeyPressed(ImGuiKey_2, false) || ImGui::IsKeyPressed(ImGuiKey_3, false)))
         SwitchWorkspace(ImGui::IsKeyPressed(ImGuiKey_1, false) ? 0 : ImGui::IsKeyPressed(ImGuiKey_2, false) ? 1 : 2); // Direct workspace keys (Cmd on macOS)
   }
   // Ctrl+Space (physical Ctrl: Cmd+Space is Spotlight on macOS): maximize the area under the mouse, again to restore. Like Ctrl+F it works while a panel has
   // the keyboard focus, but not while text is edited, nor in the situations where the areas are off (G / R / S, running game inspection,...)
   if (panelsShown && !m_table->IsLocked() && m_fileOp == nullptr && !m_palette.IsOpen() && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false)
      && (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) && !io.KeyAlt && !io.KeyShift && !m_areaDrag.active && m_areaModal == AreaModalKind::None
      && m_splitDragNode < 0) // Not during a corner drag, an edge mode or a splitter drag
   {
      const ImVec2 mouse = io.MousePos;
      if (m_maximizedLeafId >= 0 || AreaLayout::LeafAt(m_leafRects, mouse.x, mouse.y) >= 0) // Over an area (not the bars)
         ToggleMaximizeArea();
   }
   // Ctrl+F: search field of the Properties area under the mouse, otherwise the filter of the Outliner under the mouse, else of the first shown
   // Outliner (or the first hosted one, revealed). Not while a text field is being edited (script editor, fields,...), and an area is never re-typed from here
   if (panelsShown && !m_table->IsLocked() && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false) && io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
   {
      AreaEditor hovered = AreaEditor::Count;
      bool hoveredSidebar = false;
      int hoveredLeaf = -1;
      const bool overArea = GetAreaUnderMouse(hovered, hoveredSidebar, &hoveredLeaf) && !hoveredSidebar;
      PropertiesPanel *const properties = overArea && hovered == AreaEditor::Properties ? GetPropertiesArea(hoveredLeaf) : nullptr;
      OutlinerPanel *outliner = overArea && hovered == AreaEditor::Outliner ? GetOutlinerArea(hoveredLeaf) : nullptr;
      if (outliner == nullptr && properties == nullptr)
      {
         for (const LeafRect &leaf : m_leafRects)
            if (outliner == nullptr && leaf.visible && leaf.type == AreaEditor::Outliner)
               outliner = GetOutlinerArea(leaf.id);
         if (outliner == nullptr)
            outliner = GetOutlinerArea(m_areas.FindLeaf(AreaEditor::Outliner));
      }
      if (properties != nullptr)
      {
         if (properties->CanSearch())
            properties->FocusSearch();
         else
            Report("Search needs a single selection"); // The search is off for a multi selection: the next keys would otherwise be editor shortcuts
      }
      else if (outliner != nullptr)
      {
         if (FindLeafRect(outliner->GetLeaf()) == nullptr)
            ShowAreaEditor(AreaEditor::Outliner); // Reveals the area (next frame); never re-types one
         outliner->FocusFilter();
      }
      else
         Report("Outliner not shown");
   }
   // Shift+F5 stops the play test (as in IDEs), also while the game keys are routed to the game
   if (IsInspectMode() && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F5, false))
   {
      // RequestClose defers the stop while a save is running, like the Stop button and the X
      if (!modalWasActive)
         RequestClose(Player::CS_STOP_PLAY);
   }
   // Esc in a plain text field (part name, string properties, filters, search boxes) deactivates it on the press: the release must not clear the selection
   if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && io.WantTextInput && ImGui::GetCurrentContext()->ActiveIdPreviousFrame != 0)
      m_paneEscPending = true;
   const bool paletteEsc = m_palette.ConsumedEscape() | m_pinned.ConsumedEscape() | ConsumedAreaEscape() | ConsumedPaneEscape(); // The Esc that closed a popup is not an editor Esc
   // Trace state of Plastic from Atlas: Enter, Backspace and H are its own, and Esc is the only other editor key
   const bool tracing = m_plasticTrace.active;
   if (tracing && m_fileOp == nullptr && !modalWasActive && !gameKeys)
      PlasticFromAtlas::Keys(*this);
   // Undo, redo and F5 are global commands: a widget that still holds ImGui's active item (a number field clicked into type mode and left alone, an item
   // kept after a press) must not swallow them. They pass unless the item can use the key: a text field with an undo history of its own (the script editor,
   // renames and search boxes always keep it), a press still held (a drag in progress), or a modal popup.
   bool globalKeyOverItem = false;
   if (io.WantCaptureKeyboard && !io.KeyAlt && !gameKeys && ImGui::GetActiveID() != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
   {
      const bool redoKey = io.KeyCtrl && ((ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyShift)
#ifndef __APPLE__
         || (ImGui::IsKeyPressed(ImGuiKey_Y) && !io.KeyShift)
#endif
      );
      const bool undoKey = io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z);
      const bool playKey = !io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F5);
      if (undoKey || redoKey || playKey)
      {
         const ImGuiContext &g = *ImGui::GetCurrentContext();
         bool modalPopup = false;
         for (const ImGuiPopupData &popup : g.OpenPopupStack)
            modalPopup |= popup.Window != nullptr && (popup.Window->Flags & ImGuiWindowFlags_Modal) != 0;
         if (!modalPopup)
         {
            if (!io.WantTextInput)
               globalKeyOverItem = true;
            else if (ImGui::TempInputIsActive(g.ActiveId))
            {
               // A number field typed into: its own text undo (and redo) once its text was edited, the editor's while the text is untouched
               const ImGuiInputTextState *const text = ImGui::GetInputTextState(g.ActiveId);
               globalKeyOverItem = text == nullptr || !text->EditedBefore; // F5 included: typed text is applied only when the field deactivates, so it would be lost
            }
         }
      }
   }
   if (globalKeyOverItem)
      ImGui::ClearActiveID(); // The held item keeps its old text, which it would write back (maybe into another part after the undo) when it deactivates
   // The Ctrl+M prompt does not survive a mouse click (a view, a menu, a panel), a text field, a grab, a file operation or the game taking the keys
   if (m_mirrorPending
      && (modalWasActive || gameKeys || m_fileOp != nullptr || (io.WantCaptureKeyboard && !globalKeyOverItem) || ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)
         || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
   {
      m_mirrorPending = false;
      Report(string());
   }
   if ((!io.WantCaptureKeyboard || globalKeyOverItem) && m_fileOp == nullptr && !modalWasActive && !gameKeys && (!tracing || ImGui::IsKeyReleased(ImGuiKey_Escape)))
   {
      if (m_mirrorPending && HandleMirrorPrompt())
      {
         // Ctrl+M is waiting for its axis
      }
      else if (ImGui::IsKeyReleased(ImGuiKey_Escape) && !paletteEsc)
      {
         if (m_suppressNextEscape)
            m_suppressNextEscape = false; // Consumed by a field the Esc deactivated (e.g. the script editor)
         else if (m_refImage.PickActive())
            m_refImage.CancelPick();
         else if (m_measure.OnEscape(*this))
         {
            // Cancelled the ruler being dragged, or left the Measure tool
         }
         else if (PlayfieldMaker::Cancel(*this))
         {
            // Left hole placement, or a hole grab put back
         }
         else if (m_plasticFit.active)
            PlasticFromAtlas::FitCancel(*this, "Fit to Posts stopped"); // Leave the steps only
         else if (m_plasticTrace.active && m_plasticTrace.measure != 0)
            m_plasticTrace.measure = 0; // Leave measuring only, the add goes on
         else if (m_plasticTrace.active)
            PlasticFromAtlas::Cancel(*this); // Cancel the whole add
         else if (m_gizmoOperation != ImGuizmo::OPERATION(0))
            m_gizmoOperation = ImGuizmo::OPERATION(0); // Cancel current operation
         else if (m_boxSelectActive)
            m_boxSelectActive = false; // Cancel current box selection
         else if (m_addPartType != eItemInvalid)
            m_addPartType = eItemInvalid; // Cancel add part mode
         else if (m_partLibrary.IsPlacementArmed())
            m_partLibrary.CancelPlacement(); // Cancel part library placement mode
         else if (m_pointEditPart)
            ExitPointEditMode(true); // Exit drag point edit mode
         else if (m_selection.GetType() != Selection::S_NONE)
            ClearSelection(); // Cancel current selection
      }
      else if (m_secondaryViewHovered
         && ((!io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Z)
                || ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_Home) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal) || ImGui::IsKeyPressed(ImGuiKey_KeypadDivide)
                || ImGui::IsKeyPressed(ImGuiKey_Slash) || ImGui::IsKeyPressed(ImGuiKey_Backslash))) // The view's own: Local View
            || IsNumpadPressed(0) || IsNumpadPressed(1) || IsNumpadPressed(2) || IsNumpadPressed(3) || IsNumpadPressed(4) || IsNumpadPressed(5) || IsNumpadPressed(6)
            || IsNumpadPressed(7) || IsNumpadPressed(8) || IsNumpadPressed(9)))
      {
         // A secondary 3D view is under the mouse: the view keys are its own (no shading key; G / R / S work in it, see BeginModalTransform)
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_F5) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
      {
         // Play a shallow copy of the edited table
         if (!IsInspectMode() && !m_table->IsLocked())
            PlayTest();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_N))
      {
         // New table from the default blank template, N alone toggles the sidebar (Blender's N-panel)
         if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
            NewTable(NewTableTemplate::Blank);
         else if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false))
            ToggleSidebar();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_O))
      {
         // Open a table file, or a table pack folder with Shift
         if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
            LoadTable();
         else if (io.KeyCtrl && io.KeyShift && !io.KeyAlt)
            LoadTableFolder();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt)
         ToggleSnapping(); // Blender's snapping toggle (no ImGui keyboard navigation here)
      else if (ImGui::IsKeyPressed(ImGuiKey_F9, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
      {
         // Adjust Last Operation as a popup at the mouse (it would re-apply a transform to the selection: not under the Measure tool)
         if (m_measure.IsActive())
            Report("Measure tool active");
         else if (m_lastOp.type != LastOperation::Type::None)
         {
            m_adjustPopupOpen = true;
            m_adjustPopupJustOpened = true;
            m_adjustPopupPos = ImGui::GetMousePos();
         }
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
      {
         // Toggle drag point edit mode on the active selected part. For parts exposing an editable
         // center (light bulb, light sequencer animation center), Tab first switches the mode from
         // the drag point curve to the center point before exiting.
         if (m_pointEditPart)
         {
            if (!m_pointEditCenter && m_pointEditPart->HasEditCenter())
            {
               m_pointEditCenter = true;
               m_centerSelected = true;
               m_pointSel.clear();
               m_pointDragPending = false;
               m_pointDragActive = false;
            }
            else
               ExitPointEditMode(true);
         }
         else
            EnterPointEditMode();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_Q, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
         OpenQuickFavorites(); // Blender's Quick Favorites
      else if (ImGui::IsKeyPressed(ImGuiKey_F) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt) // Hide the UI (F frames the selection)
      {
         m_flyMode = !m_flyMode;
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_A))
      {
         if (io.KeyAlt && !io.KeyCtrl && !io.KeyShift)
            ClearSelection();
         else if (io.KeyShift && !io.KeyCtrl && !io.KeyAlt)
         {
            // Add a drag point on the curve segment nearest to the mouse position
            if (m_pointEditPart && !m_pointEditCenter && !io.WantCaptureMouse)
               AddPointOnNearestSegment();
            else if (!m_pointEditPart && !IsInspectMode() && !m_table->IsLocked())
            {
               // Request the part type picker popup at the mouse position (it is opened from the
               // toolbar window scope below, as OpenPopup must be called in the same window scope
               // as the matching BeginPopup)
               m_chrome.RequestAddPartPopup(ImGui::GetMousePos());
            }
         }
         else if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
         {
            SelectAllAction(); // Select all: all curve points in drag point edit mode, the center point in center edit mode, all pickable parts otherwise
         }
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_I, false) && io.KeyCtrl && !io.KeyAlt && !io.KeyShift) InvertSelection(); // Blender's Select > Invert (Ctrl+I)
      else if (ImGui::IsKeyPressed(ImGuiKey_M, false) && !io.KeyAlt && !io.KeyShift
         && (io.KeyCtrl
#ifdef __APPLE__
            || io.KeySuper // ImGui swaps Cmd and Ctrl on macOS: the physical Control key is KeySuper. Cmd+M is the system's Minimize (SDL's Window menu takes it before the editor)
#endif
            ))
         StartMirrorPrompt(); // Blender's Object > Mirror (Ctrl+M), then X or Y
      else if (ImGui::IsKeyPressed(ImGuiKey_C))
      {
         // Copy the selected parts (or the coordinates of the selected drag point in point edit mode)
         if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
            CopySelection();
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_V))
      {
         // Paste at the mouse position (or to the selected drag point in point edit mode)
         if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
            PasteSelection(ImGui::GetMousePos());
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_Delete) || (m_measure.IsActive() && ImGui::IsKeyPressed(ImGuiKey_X)))
      {
         if (!io.KeyAlt && !io.KeyCtrl && !io.KeyShift)
         {
            if (m_measure.IsActive())
               m_measure.OnDelete(*this); // In the Measure tool Del / X only ever act on rulers (nothing selected: nothing happens)
            else
               DeleteAction();
         }
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_D) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt)
      {
         DuplicateSelection(); // Duplicate the selected parts (through the editor clipboard) at the mouse position and grab them
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_H, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && m_pointEditPart != nullptr && m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive
         && static_cast<Primitive *>(m_pointEditPart->GetEditable())->m_d.m_plasticGen)
      {
         // Editing the points of a plastic: H switches between its outline and its holes
         Primitive *const plastic = static_cast<Primitive *>(m_pointEditPart->GetEditable());
         PlasticFromAtlas::AbortDrags(*this);
         plastic->m_plasticEditHoles = !plastic->m_plasticEditHoles;
         m_pointSel.clear();
         Report(plastic->m_plasticEditHoles ? "Editing the holes of " + plastic->GetName() + " (H: outline)" : "Editing the outline of " + plastic->GetName() + " (H: holes)");
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_H))
      {
         if (m_table->m_liveBaseTable == nullptr && !io.KeyCtrl) // No UI visibility in inspection mode
         {
            bool changed = false;
            vector<IEditable *> toggled; // Parts whose eye changed (a plastic host reaches its companions, a companion alone does not)
            if (io.KeyAlt)
            { // Unhide all
               for (auto &part : m_editables)
                  if (part->GetEditable()->GetItemType() != eItemPartGroup && (part->GetEditable()->m_desktopBackdrop == (m_camMode == ViewMode::DesktopBackdrop))
                     && !part->GetEditable()->IsUIVisible(false))
                  {
                     part->GetEditable()->SetUIVisible(true);
                     toggled.push_back(part->GetEditable());
                     changed = true;
                  }
            }
            else if (io.KeyShift)
            { // Hide unselected
               for (auto &part : m_editables)
                  if (part->GetEditable()->GetItemType() != eItemPartGroup && (part->GetEditable()->m_desktopBackdrop == (m_camMode == ViewMode::DesktopBackdrop)) && !IsPartSelected(part)
                     && part->GetEditable()->IsUIVisible(false))
                  {
                     part->GetEditable()->SetUIVisible(false);
                     toggled.push_back(part->GetEditable());
                     changed = true;
                  }
               for (const auto &part : m_multiSel) // Companions are never selected: a selected plastic keeps them
                  toggled.push_back(part->GetEditable());
            }
            else
            { // Hide selected
               for (const auto &part : m_multiSel)
                  if (part->GetEditable()->IsUIVisible(false))
                  {
                     part->GetEditable()->SetUIVisible(false);
                     toggled.push_back(part->GetEditable());
                     changed = true;
                  }
               ClearSelection();
            }
            if (changed)
            {
               PlasticFromAtlas::SyncCompanionVisibility(toggled); // A plastic's companions are hidden and shown with their host
               m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
            }
         }
      }
      else if ((ImGui::IsKeyPressed(ImGuiKey_W) || ImGui::IsKeyPressed(ImGuiKey_E) || ImGui::IsKeyPressed(ImGuiKey_T)) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
      {
         // Gizmo tools, always available: W move, E rotate, T scale (repeat toggles world / local)
         SetGizmoOperation(ImGui::IsKeyPressed(ImGuiKey_W) ? ImGuizmo::TRANSLATE : ImGui::IsKeyPressed(ImGuiKey_E) ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
         m_selectRename.OpenRename(); // Blender's Batch Rename
      else if (ImGui::IsKeyPressed(ImGuiKey_G, false) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt && m_navBlenderTransform && !m_pointEditPart)
         m_selectRename.OpenSimilar(); // Blender's Select Similar (with the Blender transform keys; otherwise Shift+G is the modal grab)
      else if (ImGui::IsKeyPressed(ImGuiKey_G))
      {
         // Grab (translate)
         if (m_camMode == ViewMode::PreviewCam && TransformViewSlot() == -1)
            m_camMode = ViewMode::EditorCam;
         if (io.KeyAlt && !m_pointEditPart)
         {
            Matrix3D tmp;
            if (GetSelectionTransform(tmp))
               ClearSelectionTransform(tmp, true, false, false, "Clear Location");
         }
         else if (!io.KeyCtrl && (m_navBlenderTransform != io.KeyShift)) // Shift picks the other scheme
            BeginModalTransform(ModalOp::Grab);
         else if (!io.KeyCtrl)
            SetGizmoOperation(ImGuizmo::TRANSLATE);
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_S))
      {
         // Ctrl+S (save) is handled earlier, before the WantCaptureKeyboard check
         if (!io.KeyCtrl)
         {
            const bool snapMenuKey = io.KeyShift && !io.KeyAlt && m_navBlenderTransform && m_camMode != ViewMode::DesktopBackdrop; // (without the Blender keys, and in the desktop backdrop view where the cursor is not shown, Shift+S is the modal scale: the menu is in Object > Snap)
            // Scale
            if (m_camMode == ViewMode::PreviewCam && TransformViewSlot() == -1 && !snapMenuKey)
               m_camMode = ViewMode::EditorCam;
            if (snapMenuKey)
            {
               // Shift+S: the snap menu of the 3D cursor (Blender's), for the view under the mouse
               if (!IsInspectMode() && m_viewportVisible && ImGui::IsKeyPressed(ImGuiKey_S, false))
               {
                  m_cursorMenuRequest = true;
                  m_cursorMenuPos = ImGui::GetMousePos();
                  m_cursorMenuSlot = std::max(TransformViewSlot(), -1);
               }
            }
            else if (io.KeyAlt && !m_pointEditPart)
            {
               Matrix3D tmp;
               if (GetSelectionTransform(tmp))
                  ClearSelectionTransform(tmp, false, true, false, "Clear Scale");
            }
            else if (m_navBlenderTransform != io.KeyShift)
               BeginModalTransform(ModalOp::Scale);
            else
               SetGizmoOperation(ImGuizmo::SCALE);
         }
      }
      else if (ImGui::IsKeyPressed(ImGuiKey_R))
      {
         // Rotate
         if (m_camMode == ViewMode::PreviewCam && TransformViewSlot() == -1)
            m_camMode = ViewMode::EditorCam;
         if (io.KeyAlt && !m_pointEditPart)
         {
            Matrix3D tmp;
            if (GetSelectionTransform(tmp))
               ClearSelectionTransform(tmp, false, false, true, "Clear Rotation");
         }
         else if (!io.KeyCtrl && (m_navBlenderTransform != io.KeyShift)) // Shift picks the other scheme
            BeginModalTransform(ModalOp::Rotate);
         else if (!io.KeyCtrl)
            SetGizmoOperation(ImGuizmo::ROTATE);
      }
#ifndef __APPLE__
      else if (ImGui::IsKeyPressed(ImGuiKey_Y) && io.KeyCtrl && !io.KeyAlt && !io.KeyShift) RedoSteps(1); // Ctrl+Y (on macOS ImGui maps Cmd to Ctrl and Cmd+Shift+Z is the redo)
#endif
      else if (ImGui::IsKeyPressed(ImGuiKey_Z))
      {
         if (io.KeyCtrl)
         { // Undo (Ctrl+Z), Redo (Ctrl+Shift+Z)
            if (io.KeyShift)
               RedoSteps(1);
            else
               UndoSteps(1);
         }
         else if (io.KeyAlt && !io.KeyShift)
            m_selectionOverlay = !m_selectionOverlay; // Alt+Z: show / hide the selection overlay
         else if (!io.KeyShift && !io.KeyAlt)
         { // Wireframe shade mode selection
            switch (m_shadeMode)
            {
            case Renderer::ShadeMode::Default: m_shadeMode = Renderer::ShadeMode::Wireframe; break;
            case Renderer::ShadeMode::Wireframe: m_shadeMode = Renderer::ShadeMode::NoDepthWireframe; break;
            case Renderer::ShadeMode::NoDepthWireframe: m_shadeMode = Renderer::ShadeMode::Solid; break;
            case Renderer::ShadeMode::Solid: m_shadeMode = Renderer::ShadeMode::Default; break;
            }
         }
      }
      else if (IsNumpadPressed(0))
         CycleCamera();
      else if (IsNumpadPressed(5, false)) // No key repeat: holding the key would toggle every few frames
         TogglePerspectiveCam();
      else if (ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal) || (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift))
         FrameSelection(); // Blender's View Selected
      else if (ImGui::IsKeyPressed(ImGuiKey_Home))
         FrameAll();
      else if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift
         && (ImGui::IsKeyPressed(ImGuiKey_Slash, false) || ImGui::IsKeyPressed(ImGuiKey_Backslash, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDivide, false)))
         ToggleLocalView(); // Blender's numpad /, also on / and \ (no numpad on a laptop); Lock Orbit is the header's lock button
      else if (IsNumpadPressed(2) || IsNumpadPressed(4) || IsNumpadPressed(6) || IsNumpadPressed(8))
         OrbitStep(IsNumpadPressed(4) || IsNumpadPressed(6), (IsNumpadPressed(4) || IsNumpadPressed(8)) ? 1.f : -1.f);
      else if (IsNumpadPressed(7))
         SetView(io.KeyCtrl ? PredefinedView::Bottom : PredefinedView::Top);
      else if (IsNumpadPressed(1))
         SetView(io.KeyCtrl ? PredefinedView::Back : PredefinedView::Front);
      else if (IsNumpadPressed(3))
         SetView(io.KeyCtrl ? PredefinedView::Right : PredefinedView::Left);
   }

   // Parts under the cursor list (Ctrl+click)
   if (m_pickListOpen)
   {
      ImGui::SetNextWindowPos(m_pickListPos, ImGuiCond_Appearing);
      ImGui::SetNextWindowSizeConstraints(ImVec2(220.f * m_liveUI.GetDPI(), 0.f), ImVec2(FLT_MAX, FLT_MAX));
      bool open = true;
      if (ImGui::Begin("Parts under cursor", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
      {
         for (const auto &part : m_pickList)
            if (ImGui::Selectable(part->GetEditable()->GetName().c_str(), IsPartSelected(part)))
            {
               SetSelection(Selection(part));
               m_outlinerAnchor = part;
               open = false;
            }
         if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            open = false;
      }
      ImGui::End();
      if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape))
         m_pickListOpen = false;
   }

   // Selected parts are kept dynamic in the UI quadtree so that they can be edited interactively. The selection also changes outside
   // of this pass (automation, undo, table switches) and parts get re-added as static (undelete), so the state is synced rather than
   // derived from selection changes
   std::erase_if(m_dynamicParts, [this](IEditable *edit) { return FindIndexOf(m_table->GetParts(), edit) == -1; });
   for (IEditable *const edit : m_dynamicParts)
      if (!IsEditableSelected(edit) && !m_player->m_physics->IsStatic(edit))
         m_player->m_physics->SetStatic(edit);
   std::erase_if(m_dynamicParts, [this](IEditable *edit) { return !IsEditableSelected(edit); });
   for (const auto &part : m_multiSel)
   {
      IEditable *const edit = part->GetEditable();
      if (edit->GetIHitable() == nullptr || edit->GetItemType() == eItemBall)
         continue;
      if (m_player->m_physics->IsStatic(edit))
         m_player->m_physics->SetDynamic(edit);
      if (FindIndexOf(m_dynamicParts, edit) == -1)
         m_dynamicParts.push_back(edit);
   }

   // Undo marks (and undo / redo) leave the authored visibility in the parts' m_d: put the display state back before the next frame renders them
   if (m_isOpened && !IsSaveInProgress() && !((m_table->m_liveBaseTable != nullptr) && m_player->IsPlaying())) // (closed this frame: Close restored the authored values for the game; a script may have written m_d of a playing live table)
   {
      const ankerl::unordered_dense::set<const IEditable *> liveParts(m_table->GetParts().begin(), m_table->GetParts().end());
      for (const auto &uiPart : m_editables)
         if (liveParts.contains(uiPart->GetEditable()))
            uiPart->ApplyDisplayVisibility();
   }
   if (m_isOpened)
      UpdateLocalViews(); // A part added or returned by an undo this frame is in the sets before the next render (which comes before the next frame's update)
}

bool EditorUI::GetViewportFraction(float (&rect)[4]) const
{
   const ImVec2 display = ImGui::GetIO().DisplaySize;
   if (!m_viewportVisible || m_viewportContentRect.Width() <= 0.f || m_viewportContentRect.Height() <= 0.f || display.x <= 0.f || display.y <= 0.f)
      return false;
   rect[0] = m_viewportContentRect.x0 / display.x;
   rect[1] = m_viewportContentRect.y0 / display.y;
   rect[2] = m_viewportContentRect.Width() / display.x;
   rect[3] = m_viewportContentRect.Height() / display.y;
   return true;
}

void EditorUI::SetLookAtCamera(const vec3 &eye, const vec3 &target, const float fovY, const Renderer::ShadeMode shading)
{
   if (fovY <= 0.f)
   {
      m_camMode = ViewMode::PreviewCam;
      return;
   }
   m_camMode = ViewMode::EditorCam;
   m_shadeMode = shading;
   m_perspectiveCam = true;
   m_navFovY = fovY;
   vec3 dir = target - eye;
   m_camDistance = max(dir.Length(), 1.f);
   dir.Normalize();
   // Same convention as Renderer::SetLookAtCamera: right handed look at in a table space mirrored along Z. The projection is rebuilt from these each frame
   const Vertex3Ds up = fabsf(dir.z) > 0.999f ? Vertex3Ds(0.f, -1.f, 0.f) : Vertex3Ds(0.f, 0.f, -1.f);
   m_camView = Matrix3D::MatrixLookAtRH(Vertex3Ds(eye.x, eye.y, -eye.z), Vertex3Ds(target.x, target.y, -target.z), up);
}

void EditorUI::PushUndo(IEditable * part, unsigned int undoId, const string &label)
{
   // No undo support in inspection mode (not implemented as it needs to handle both version of the table)
   if (m_table->m_liveBaseTable)
      return;

   // Filter out continuous modification after first one (dragging a part, editing values,...)
   if (part == m_lastUndoPart && undoId == m_lastUndoId)
      return;

   m_lastUndoPart = part;
   m_lastUndoId = undoId;
   m_undo.BeginUndo();
   m_undo.MarkForUndo(part);
   if (!label.empty())
      m_undo.SetFieldLabel(label);
   m_undo.EndUndo();
}

string EditorUI::CountLabel(const char *verb, const size_t count, const string &name)
{
   return count == 1 ? string(verb) + ": " + name : string(verb) + ": " + std::to_string(count) + " parts";
}

// Undo and redo are off while a drag is in progress (its record is the top one: undoing it would leave the drag going unrecorded)
bool EditorUI::IsDragInProgress() const { return m_modal.active || m_gizmoDragActive || ImGuizmo::IsUsingAny() || m_pointDragActive || m_pointDragPending || m_plasticBow.pending || m_plasticBow.active || m_plasticPill.pending || m_plasticPill.active || m_railFixtureDrag.active; }

bool EditorUI::CanUndo() const { return m_table->m_liveBaseTable == nullptr && m_undo.HasUndo() && !IsDragInProgress() && m_fileOp == nullptr; }

bool EditorUI::CanRedo() const { return m_table->m_liveBaseTable == nullptr && m_undo.HasRedo() && !IsDragInProgress() && m_fileOp == nullptr; }

// Gameplay colliders and static shadow pages of the parts moved during a drag (modal or gizmo), once at the drag end
void EditorUI::FlushDrag()
{
   const auto &parts = m_table->GetParts();
   for (IEditable *const part : m_drag.pendingPhysics)
      if (std::ranges::find(parts, part) != parts.end()) // Still in the table (a canceled drag restored it)
      {
         if (part->GetItemType() == eItemPrimitive)
            static_cast<Primitive *>(part)->FlushGenDrag(); // Before its colliders, which read the mesh
         m_player->m_physics->Update(part);
      }
   if (m_drag.shadowDirty)
      m_renderer->InvalidateShadowContent();
   m_drag = Drag();
}

void EditorUI::RestoreUndoState(const std::any &state)
{
   m_rubberFitHash.clear(); // The restored rubbers have other shapes than the last fit
   if (state.has_value())
      RestoreUndoSelection(std::any_cast<const UndoSelectionState &>(state));
}

void EditorUI::UndoSteps(const size_t count)
{
   if (!CanUndo() || count == 0)
      return;
   m_localUndoRan = true;
   m_lastUndoPart = nullptr;
   m_lastUndoId = 0;
   string label;
   std::any state;
   size_t done = 0;
   bool stoppedEarly = false;
   const auto hostEyes = PlasticFromAtlas::SnapshotHostVisibility(*this);
   // TODO handle IsUndoPastCleanPoint
   for (; done < count && m_undo.HasUndo(); ++done)
   {
      // A record that can not be redone would wipe the redo steps built so far: a multi-step undo stops before it (a single undo may clear redo)
      if (done > 0 && m_undo.NextUndoClearsRedo())
      {
         stoppedEarly = true;
         break;
      }
      label = m_undo.UndoLabel(0); // The label reported is the one of the last step actually undone
      const std::function<void()> keepShown = PlayfieldMaker::KeepShown(*this);
      if (std::any step = m_undo.Undo(); step.has_value())
         state = std::move(step);
      if (keepShown)
         keepShown();
   }
   RestoreUndoState(state);
   PlasticFromAtlas::SyncChangedHostVisibility(*this, hostEyes);
   Report((done == 1 ? "Undo: " + label : "Undo: " + std::to_string(done) + " steps (" + label + ")") + (stoppedEarly ? ", stopped before a step that can not be redone" : ""));
}

void EditorUI::RedoSteps(const size_t count)
{
   if (!CanRedo() || count == 0)
      return;
   m_localUndoRan = true;
   m_lastUndoPart = nullptr;
   m_lastUndoId = 0;
   string label;
   std::any state;
   size_t done = 0;
   const auto hostEyes = PlasticFromAtlas::SnapshotHostVisibility(*this);
   for (; done < count && m_undo.HasRedo(); ++done)
   {
      label = m_undo.RedoLabel(0);
      const std::function<void()> keepShown = PlayfieldMaker::KeepShown(*this);
      if (std::any step = m_undo.Redo(); step.has_value())
         state = std::move(step);
      if (keepShown)
         keepShown();
   }
   RestoreUndoState(state);
   PlasticFromAtlas::SyncChangedHostVisibility(*this, hostEyes);
   Report(done == 1 ? "Redo: " + label : "Redo: " + std::to_string(done) + " steps (" + label + ")");
}

void EditorUI::SwitchWorkspace(const int index)
{
   if (index < 0 || index >= PinnedPanel::WorkspaceCount)
      return;
   const bool changed = index != m_pinned.GetActiveWorkspace();
   m_pinned.ApplyWorkspace(index);
   if (changed)
      Report("Workspace: " + m_pinned.GetWorkspace(index).name);
}

void EditorUI::OnFileDrop(const string &path, const float x, const float y)
{
   if (m_table->IsLocked() || IsInspectMode())
   {
      Report("Dropped files are taken by the editor only (not locked, not in a play test)");
      return;
   }
   PlayfieldMaker::OnFileDrop(*this, path, ImVec2(x, y));
}

void EditorUI::Report(const string &message)
{
   m_report = message;
   m_reportTime = ImGui::GetTime();
}

bool EditorUI::CanCopy() const
{
   const bool can = m_pointEditPart ? (m_pointEditCenter ? m_centerSelected : m_pointSel.size() == 1) : (m_selection.GetType() == Selection::S_EDITABLE);
   return can && !m_table->IsLocked();
}

bool EditorUI::CanPaste() const
{
   const bool can = m_pointEditPart ? (m_pointEditCenter ? m_centerSelected && VPX::EditorClipboard::HasPoint() : m_pointSel.size() == 1 && VPX::EditorClipboard::HasPoint())
                                    : VPX::EditorClipboard::HasParts();
   return can && !m_table->IsLocked() && !IsInspectMode() && !m_measure.IsActive();
}

bool EditorUI::CanDuplicate() const { return !m_pointEditPart && !m_multiSel.empty() && !IsInspectMode() && !m_table->IsLocked() && !m_measure.IsActive(); }

bool EditorUI::CanDelete() const
{
   if (m_table->IsLocked() || m_measure.IsActive())
      return false;
   return m_pointEditPart ? !m_pointSel.empty() : m_selection.GetType() == Selection::S_EDITABLE;
}

bool EditorUI::CanSelect() const { return !m_table->IsLocked() && m_fileOp == nullptr; }

void EditorUI::DuplicateSelection()
{
   // Duplicate the selected parts (through the editor clipboard) at the mouse position and grab them
   if (m_measure.IsActive())
   {
      Report("Measure tool active");
      return;
   }
   if (!CanDuplicate())
      return;
   if (std::ranges::any_of(m_multiSel, [](const std::shared_ptr<EditorUIPart> &p) { return PlayfieldMaker::IsPart(p->GetEditable()); }))
   {
      Report(PlayfieldMaker::kFollowsTable);
      return;
   }
   const int grabSlot = TransformViewSlot();
   if (!SelectionInView(grabSlot)) // The grab which follows would be refused: no copy then
   {
      Report("Local View: the selection has parts this view does not show");
      return;
   }
   if (m_navBlenderTransform && (grabSlot == -2 || (grabSlot >= 0 && !CanStartModalIn(grabSlot)))) // The grab could not start in a view: no copy then (without the 3D Viewport shown it copies in place)
   {
      Report("Duplicate needs the 3D Viewport or a view that can be edited under the mouse");
      return;
   }
   const size_t count = m_multiSel.size();
   Matrix3D origin;
   const bool hasOrigin = GetSelectionTransform(origin);
   CopySelection();
   const uint64_t topBefore = m_undo.TopRecordId();
   PasteSelection(ImGui::GetMousePos(), true);
   if (m_undo.TopRecordId() == topBefore) // Nothing was pasted: the grab would fold into an unrelated step
   {
      Report("Nothing to duplicate");
      return;
   }
   m_undo.SetLabel(CountLabel("Duplicate", m_multiSel.size(), m_multiSel.empty() ? string() : m_multiSel.front()->GetEditable()->GetName())); // Not "Paste" (kept by the merge with the grab)
   Report(CountLabel("Duplicated", count, m_multiSel.empty() ? string() : m_multiSel.front()->GetEditable()->GetName()));
   if (m_navBlenderTransform)
   {
      BeginModalTransform(ModalOp::Grab);
      if (m_modal.active && hasOrigin)
      {
         m_modal.duplicate = true;
         m_modal.duplicateOrigin = vec3(origin._41, origin._42, origin._43);
      }
   }
   else
      SetGizmoOperation(ImGuizmo::TRANSLATE);
}

bool EditorUI::CanMirror() const { return CanDuplicate() && !m_measure.IsActive(); }

void EditorUI::StartMirrorPrompt()
{
   if (m_measure.IsActive())
   {
      Report("Measure tool active");
      return;
   }
   if (!CanMirror())
   {
      Report(m_table->IsLocked() ? "Mirror: not available (table locked)"
            : IsInspectMode()   ? "Mirror: not available (inspecting the game)"
            : m_pointEditPart   ? "Mirror: not available in point edit mode (Tab leaves it)"
                                : "Mirror: select the parts to mirror first");
      return;
   }
   m_mirrorPending = true;
   m_mirrorDuplicate = false;
}

// Ctrl+M is modal like Blender's: while armed every key is its own (X / Y pick the axis, Shift the median, D toggles the copy, Esc cancels, the modifiers
// pass), any other key, a mouse click, or a text field / grab / game taking over cancels it. It returns true while armed, so that the key chain does not run
bool EditorUI::HandleMirrorPrompt()
{
   ImGuiIO &io = ImGui::GetIO();
   const auto cancel = [this]()
   {
      m_mirrorPending = false;
      Report(string()); // The prompt text is shown only while armed
   };
   if (!CanMirror())
   {
      cancel();
      return false;
   }
   if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
   {
      cancel();
      m_paneEscPending = true; // The release of this Esc is not the editor's (it would clear the selection)
      return true;
   }
   if (!io.KeyAlt && (ImGui::IsKeyPressed(ImGuiKey_X, false) || ImGui::IsKeyPressed(ImGuiKey_Y, false)))
   {
      const int axis = ImGui::IsKeyPressed(ImGuiKey_X, false) ? 0 : 1;
      MirrorSelection(axis, io.KeyShift, m_mirrorDuplicate); // Disarms
      return true;
   }
   if (ImGui::IsKeyPressed(ImGuiKey_D, false))
      m_mirrorDuplicate = !m_mirrorDuplicate;
   else
   {
      static constexpr ImGuiKey kModifiers[] = { ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl, ImGuiKey_LeftShift, ImGuiKey_RightShift, ImGuiKey_LeftAlt, ImGuiKey_RightAlt, ImGuiKey_LeftSuper,
         ImGuiKey_RightSuper, ImGuiKey_Menu };
      for (int key = ImGuiKey_Tab; key < ImGuiKey_GamepadStart; key++)
         if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false) && std::ranges::find(kModifiers, static_cast<ImGuiKey>(key)) == std::end(kModifiers))
         {
            cancel(); // Any other key is not a Mirror key: it is swallowed, and the prompt is over
            return true;
         }
   }
   Report(string("Mirror") + (m_mirrorDuplicate ? " a copy" : "") + ": X = left / right, Y = front / back across the table centre, Shift+X / Shift+Y across the selection's median, D = "
      + (m_mirrorDuplicate ? "mirror in place instead" : "mirror a copy instead") + ", Esc cancels");
   return true;
}

// Object > Mirror: the selection across the vertical line x = the table's centre (Y: the horizontal line), or across the median of the selection. Parts that
// can not be mirrored honestly (MirrorParts::Refusal) are left out and named in the message; the rest is mirrored. One undo step for all of it.
void EditorUI::MirrorSelection(const int axisIndex, const bool aboutMedian, const bool duplicate)
{
   if (m_measure.IsActive())
   {
      Report("Measure tool active");
      return;
   }
   using MirrorParts::Axis;
   m_mirrorPending = false; // From the prompt, the menu or the palette
   if (!CanMirror())
      return;
   const Axis axis = axisIndex == 0 ? Axis::X : Axis::Y;
   const char *const axisName = axis == Axis::X ? "X" : "Y";
   vector<std::shared_ptr<EditorUIPart>> todo;
   string refused;
   for (const auto &part : m_multiSel)
   {
      const IEditable *const edit = part->GetEditable();
      if (edit->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(edit)->IsPlasticCompanion())
         continue; // Derived from its host: follows it
      const string reason = MirrorParts::Refusal(edit, axis);
      if (reason.empty())
         todo.push_back(part);
      else
         refused += (refused.empty() ? "" : ", ") + edit->GetName() + " (" + reason + ")";
   }
   if (todo.empty())
   {
      Report("Nothing mirrored" + (refused.empty() ? string() : ": " + refused));
      return;
   }
   // The mirror line: the table's centre line, or the median (mean of the centres) of the parts to mirror
   float coord;
   if (aboutMedian)
   {
      double sum = 0.;
      for (const auto &part : todo)
         sum += MirrorParts::Position(part->GetEditable(), axis);
      coord = static_cast<float>(sum / static_cast<double>(todo.size()));
   }
   else
      coord = axis == Axis::X ? 0.5f * (m_table->m_left + m_table->m_right) : 0.5f * (m_table->m_top + m_table->m_bottom);
   const string firstName = todo.front()->GetEditable()->GetName();
   const size_t count = todo.size();
   if (duplicate)
   {
      // Copies of the parts that can be mirrored, on the originals, then mirrored (one undo step with the paste)
      if (todo.size() != m_multiSel.size())
      {
         m_multiSel = todo;
         m_selection = Selection(todo.back());
      }
      CopySelection();
      const uint64_t topBefore = m_undo.TopRecordId();
      PasteSelection(ImGui::GetMousePos(), true);
      if (m_undo.TopRecordId() == topBefore || m_multiSel.empty())
      {
         Report("Nothing to duplicate");
         return;
      }
      m_undo.SetLabel(CountLabel("Duplicate Mirrored", m_multiSel.size(), m_multiSel.front()->GetEditable()->GetName()));
   }
   const vector<std::shared_ptr<EditorUIPart>> targets = duplicate ? m_multiSel : todo; // The copies, or the parts that can be mirrored
   vector<IEditable *> touched;
   for (const auto &part : targets)
      touched.push_back(part->GetEditable());
   m_undo.BeginUndo();
   for (IEditable *const edit : touched)
      m_undo.MarkForUndo(edit);
   MarkDependentsForUndo(touched);
   if (!duplicate)
      MirrorParts::MarkMeshesForUndo(m_undo, m_table, touched); // The copies of a duplicate are new parts: their undo removes them
   if (!duplicate)
      m_undo.SetLabel(CountLabel("Mirror", targets.size(), touched.front()->GetName()) + (axis == Axis::X ? " (X)" : " (Y)"));
   m_undo.EndUndo();
   if (duplicate)
      m_undo.MergeLastIntoPrevious();
   for (const auto &part : targets)
   {
      IEditable *const edit = part->GetEditable();
      MirrorParts::Apply(edit, axis, coord);
      part->OnMirrored();
      const Primitive *const prim = edit->GetItemType() == eItemPrimitive ? static_cast<const Primitive *>(edit) : nullptr;
      if (prim == nullptr || !(prim->m_d.m_plasticGen || prim->IsCurveHost())) // A wire, a plastic or a ramp collider refreshed itself
         if (edit->GetIRenderable() != nullptr)
            m_renderer->ReinitRenderable(edit->GetIRenderable());
      m_player->m_physics->Update(edit);
   }
   m_renderer->InvalidateShadowContent();
   DetachSelectedRubbersFromUnselectedPosts();
   RefitSelectionDependents();
   m_lastOp.type = LastOperation::Type::None; // The paste of a duplicate is not adjustable any more: its panel would put the copies back as pasted, unmirrored
   string message = string(duplicate ? "Duplicated and mirrored " : "Mirrored ") + (count == 1 ? firstName : std::to_string(count) + " parts") + " across the "
      + (aboutMedian ? "selection's median " : "table centre ") + axisName + " = " + std::to_string(static_cast<int>(lroundf(coord)));
   if (!refused.empty())
      message += "; left out: " + refused;
   Report(message);
}

void EditorUI::DeleteAction()
{
   if (m_measure.IsActive())
   {
      Report("Measure tool active");
      return;
   }
   if (m_pointEditPart)
      DeleteSelectedPoints();
   else
      DeleteSelection();
}

void EditorUI::SelectAllAction()
{
   if (m_pointEditPart)
   {
      if (m_pointEditCenter)
         m_centerSelected = true;
      else
      {
         PlasticFromAtlas::AbortDrags(*this);
         m_pointSel.clear();
         for (const auto &point : m_pointEditPart->GetDragPointCurve()->GetPoints())
            m_pointSel.push_back(point.get());
      }
   }
   else
      SelectAllParts();
}

void EditorUI::InvertSelection()
{
   if (m_pointEditPart)
   {
      if (m_pointEditCenter)
         m_centerSelected = !m_centerSelected;
      else
      {
         vector<DragPoint *> inverted;
         for (const auto &point : m_pointEditPart->GetDragPointCurve()->GetPoints())
            if (!IsPointSelected(point.get()))
               inverted.push_back(point.get());
         m_pointSel = std::move(inverted);
      }
      return;
   }
   // Same set as the select all: the pickable parts that are not selected
   const vector<std::shared_ptr<EditorUIPart>> previous = m_multiSel;
   SelectAllParts();
   std::erase_if(m_multiSel, [&previous](const auto &part) { return std::ranges::find(previous, part) != previous.end(); });
   m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   m_outlinerAnchor.reset();
}

bool EditorUI::GetAreaUnderMouse(AreaEditor & type, bool &sidebar, int *const leafId) const
{
   // Area geometry of RenderAreas / PinnedPanel::Render: the leaf rects of the layout tree, the N-panel being the right part of the viewport area
   sidebar = false;
   if (m_table->IsLocked() || m_flyMode || m_inspectionModal.IsVisible())
      return false;
   if (!m_areaRectsValid)
      ComputeAreaRects();
   const ImVec2 mouse = ImGui::GetMousePos();
   const int id = AreaLayout::LeafAt(m_leafRects, mouse.x, mouse.y);
   const AreaNode *const node = id >= 0 ? m_areas.Find(id) : nullptr;
   if (node == nullptr)
      return false;
   if (node->type != AreaType::Viewport)
   {
      type = node->type;
      if (leafId != nullptr)
         *leafId = id;
      return true;
   }
   if (m_showSidebar && mouse.x >= m_viewportContentRect.x1)
   {
      sidebar = true;
      return true;
   }
   return false;
}

bool EditorUI::IsPartSelected(const std::shared_ptr<EditorUIPart> &part) const { return part != nullptr && std::ranges::find(m_multiSel, part) != m_multiSel.end(); }

bool EditorUI::IsEditableSelected(const IEditable *editable) const
{
   for (const auto &part : m_multiSel)
      if (part->GetEditable() == editable)
         return true;
   return false;
}

void EditorUI::ClearSelection()
{
   m_selection = Selection();
   m_multiSel.clear();
   m_multiSelImages.clear();
   m_multiSelSounds.clear();
   m_multiSelMaterials.clear();
   m_outlinerAnchor.reset();
   m_outlinerImageAnchor = nullptr;
   m_outlinerSoundAnchor = nullptr;
   m_outlinerMaterialAnchor = nullptr;
}

EditorUI::UndoSelectionState EditorUI::CaptureUndoSelection() const
{
   UndoSelectionState state { m_selection, m_multiSel, m_outlinerAnchor, m_pointEditPart, {}, m_pointEditCenter };
   if (const Primitive *const plastic = PlasticFromAtlas::EditedHostOf(*this))
      state.plasticEditHoles = plastic->m_plasticEditHoles;
   // Drag points are deleted and recreated when their part is reloaded (undo, ...): store their index in the curve
   if (DragPointCurve *const curve = m_pointEditPart ? m_pointEditPart->GetDragPointCurve() : nullptr)
   {
      for (const DragPoint *point : m_pointSel)
         state.pointSel.push_back(curve->GetPointIndex(point));
   }
   return state;
}

void EditorUI::RestoreUndoSelection(const UndoSelectionState &state)
{
   // Ensure the editable map is up to date (undo may have deleted or undeleted parts)
   UpdateEditableList();
   // Parts are resolved through the editable map since undo may have removed and recreated them
   const auto resolve = [this](const std::shared_ptr<EditorUIPart> &part) -> std::shared_ptr<EditorUIPart>
   {
      if (part == nullptr)
         return nullptr;
      const auto it = m_editableMap.find(part->GetEditable());
      return it != m_editableMap.end() ? it->second : nullptr;
   };
   // Restore the drag point edit mode state (selected points are resolved by index since they are recreated on part reload)
   const std::shared_ptr<EditorUIPart> pointEditPart = resolve(state.pointEditPart);
   if (m_pointEditPart != pointEditPart)
   {
      if (m_pointEditPart)
         m_pointEditPart->SetPointEditContext(nullptr);
      m_pointEditPart = pointEditPart;
      if (m_pointEditPart)
         m_pointEditPart->SetPointEditContext(this);
      m_pointDragPending = false;
      m_pointDragActive = false;
   }
   m_pointEditCenter = state.pointEditCenter && m_pointEditPart && m_pointEditPart->HasEditCenter();
   m_centerSelected = m_pointEditCenter;
   if (Primitive *const plastic = PlasticFromAtlas::EditedHostOf(*this))
      plastic->m_plasticEditHoles = state.plasticEditHoles; // The part was reloaded: its holes / outline state is put back (the indices below are in that curve)
   m_pointSel.clear();
   if (DragPointCurve *const curve = m_pointEditPart ? m_pointEditPart->GetDragPointCurve() : nullptr)
   {
      const auto &points = curve->GetPoints();
      for (const int index : state.pointSel)
         if (index >= 0 && index < (int)points.size())
            m_pointSel.push_back(points[index].get());
   }
   m_multiSel.clear();
   for (const auto &part : state.multiSel)
      if (std::shared_ptr<EditorUIPart> resolved = resolve(part))
         m_multiSel.push_back(resolved);
   // Drop resources that no longer exist in the table
   std::erase_if(m_multiSelImages, [this](const Texture *image) { return std::ranges::find(m_table->m_vimage, image) == m_table->m_vimage.end(); });
   std::erase_if(m_multiSelSounds, [this](const VPX::Sound *sound) { return std::ranges::find(m_table->m_vsound, sound) == m_table->m_vsound.end(); });
   std::erase_if(m_multiSelMaterials, [this](const Material *material) { return std::ranges::find(m_table->m_materials, material) == m_table->m_materials.end(); });
   switch (state.selection.GetType())
   {
   case Selection::S_EDITABLE:
   {
      std::shared_ptr<EditorUIPart> part = resolve(state.selection.GetPart());
      if (part == nullptr || !IsPartSelected(part))
         part = m_multiSel.empty() ? nullptr : m_multiSel.back();
      m_selection = part ? Selection(part) : Selection();
      break;
   }
   case Selection::S_MATERIAL:
      m_selection = std::ranges::find(m_table->GetMaterialList(), state.selection.GetMaterial()) != m_table->GetMaterialList().end() ? state.selection : Selection();
      break;
   case Selection::S_IMAGE: m_selection = std::ranges::find(m_table->GetImageList(), state.selection.GetImage()) != m_table->GetImageList().end() ? state.selection : Selection(); break;
   case Selection::S_RENDERPROBE:
      m_selection = std::ranges::find(m_table->GetRenderProbeList(), state.selection.GetProbe()) != m_table->GetRenderProbeList().end() ? state.selection : Selection();
      break;
   case Selection::S_SOUND: m_selection = std::ranges::find(m_table->m_vsound, state.selection.GetSound()) != m_table->m_vsound.end() ? state.selection : Selection(); break;
   case Selection::S_COLLECTION:
      m_selection = std::ranges::find(m_table->GetCollections(), state.selection.GetCollection()) != m_table->GetCollections().end() ? state.selection : Selection();
      break;
   default: m_selection = state.selection; break; // S_NONE, S_CAMERA
   }
   m_outlinerAnchor = resolve(state.outlinerAnchor);
}

void EditorUI::SetSelection(const Selection &requested)
{
   Selection selection = requested;
   if (selection.GetType() == Selection::S_EDITABLE && selection.GetPart() != nullptr) // A plastic's clear or bevel part is never the selection (the Users lists, popups and collections name them): its host is
   {
      IEditable *const editable = selection.GetPart()->GetEditable();
      IEditable *const host = PlasticFromAtlas::HostOf(m_table, editable);
      if (host != editable)
         if (const auto it = m_editableMap.find(host); it != m_editableMap.end())
            selection = Selection(it->second);
   }
   m_selection = selection;
   m_multiSel.clear();
   m_multiSelImages.clear();
   m_multiSelSounds.clear();
   m_multiSelMaterials.clear();
   switch (selection.GetType())
   {
   case Selection::S_EDITABLE: m_multiSel.push_back(selection.GetPart()); break;
   case Selection::S_IMAGE: m_multiSelImages.push_back(selection.GetImage()); break;
   case Selection::S_SOUND: m_multiSelSounds.push_back(selection.GetSound()); break;
   case Selection::S_MATERIAL: m_multiSelMaterials.push_back(selection.GetMaterial()); break;
   default: break;
   }
}

void EditorUI::TogglePartSelection(const std::shared_ptr<EditorUIPart> &part)
{
   const auto it = std::ranges::find(m_multiSel, part);
   if (it == m_multiSel.end())
   {
      m_multiSel.push_back(part);
      m_selection = Selection(part);
   }
   else
   {
      const bool wasActive = m_selection.GetType() == Selection::S_EDITABLE && m_selection.GetPart() == part;
      m_multiSel.erase(it);
      if (m_multiSel.empty())
         m_selection = Selection();
      else if (wasActive)
         m_selection = Selection(m_multiSel.back());
   }
}

void EditorUI::SelectOutlinerRange(const std::shared_ptr<EditorUIPart> &part)
{
   // Range selection between the anchor part (last clicked one) and the given part
   if (m_outlinerAnchor == nullptr)
   {
      SetSelection(Selection(part));
      return;
   }
   int anchorPos = -1, partPos = -1, pos = 0;
   for (const auto &edit : m_editables)
   {
      if (edit->GetEditable()->GetItemType() == eItemPartGroup || (edit->GetEditable()->m_desktopBackdrop != (m_camMode == ViewMode::DesktopBackdrop)) || PlasticFromAtlas::IsCompanion(edit->GetEditable()))
         continue; // (a plastic's clear and bevel parts are not listed in the outliner)
      if (edit == m_outlinerAnchor)
         anchorPos = pos;
      if (edit == part)
         partPos = pos;
      pos++;
   }
   if (anchorPos < 0 || partPos < 0)
   {
      SetSelection(Selection(part));
      return;
   }
   if (partPos < anchorPos)
      std::swap(anchorPos, partPos);
   m_multiSel.clear();
   pos = 0;
   for (const auto &edit : m_editables)
   {
      if (edit->GetEditable()->GetItemType() == eItemPartGroup || (edit->GetEditable()->m_desktopBackdrop != (m_camMode == ViewMode::DesktopBackdrop)) || PlasticFromAtlas::IsCompanion(edit->GetEditable()))
         continue; // (a plastic's clear and bevel parts are not listed in the outliner)
      if (pos >= anchorPos && pos <= partPos)
         m_multiSel.push_back(edit);
      pos++;
   }
   m_selection = Selection(part);
}

void EditorUI::SelectPartsInGroup(const PartGroup *group)
{
   // Select all the parts contained in the given group (and in its sub groups, recursively)
   m_multiSel.clear();
   for (const auto &uiPart : m_editables)
   {
      IEditable *const editable = uiPart->GetEditable();
      if (editable->GetItemType() != eItemPartGroup && editable->IsChild(group) && (editable->m_desktopBackdrop == (m_camMode == ViewMode::DesktopBackdrop)) && !PlasticFromAtlas::IsCompanion(editable))
         m_multiSel.push_back(uiPart);
   }
   m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   m_outlinerAnchor.reset();
}

void EditorUI::MoveSelectionToPartGroup(PartGroup *group)
{
   // Gather the parts that can be moved to the target group: a part can not be moved to itself, and a
   // group can not be moved to itself or one of its descendants (this would create a cycle). Only part
   // groups may be moved to the root of the hierarchy (group-less parts are live objects).
   vector<IEditable *> moved;
   for (const auto &uiPart : m_multiSel)
   {
      IEditable *const editable = uiPart->GetEditable();
      if (editable == nullptr || editable == group || editable->GetPartGroup() == group)
         continue;
      if (editable->GetItemType() == eItemPartGroup && group != nullptr && group->IsChild(static_cast<PartGroup *>(editable)))
         continue;
      if (group == nullptr && editable->GetItemType() != eItemPartGroup)
         continue;
      moved.push_back(editable);
   }
   if (moved.empty())
      return;
   PlasticFromAtlas::AddCompanions(moved); // A plastic's companions change group with their host, in the same undo step
   PlayfieldMaker::StripBoard(*this, moved);
   if (group != nullptr && group->GetReferenceSpace() != PartGroupData::SpaceReference::SR_PLAYFIELD) // The board would stop being the playfield (PhysicSetup)
      PlayfieldMaker::StripBoardGroups(*this, moved);
   if (moved.empty())
      return;
   m_undo.BeginUndo();
   for (IEditable *const editable : moved)
   {
      m_undo.MarkForUndo(editable);
      editable->SetPartGroup(group);
   }
   m_undo.SetFieldLabel("Move to Part Group");
   m_undo.EndUndo();
   UpdateEditableList(); // Refresh the outliner paths and ordering
}

// The part a hit stands for: a click on a companion of a plastic lands on its host
IEditable *EditorUI::HitEditable(const HitTestResult &hit) const
{
   IEditable *const editable = hit.m_obj->m_editable;
   return editable != nullptr ? PlasticFromAtlas::HostOf(m_table, editable) : nullptr;
}

void EditorUI::SelectAtPoint(const ImVec2 &mousePos, const PickView *view)
{
   const ImGuiIO &io = ImGui::GetIO();
   vector<HitTestResult> vhoHit;
   RayCastParts(mousePos, vhoHit, view);
   if (io.KeyShift)
   {
      // Shift+click toggles the front-most hit part in the multi selection
      if (!vhoHit.empty())
      {
         const auto it = m_editableMap.find(HitEditable(vhoHit.front()));
         if (it != m_editableMap.end())
         {
            TogglePartSelection(it->second);
            m_outlinerAnchor = it->second;
         }
      }
   }
   else if (vhoHit.empty())
      ClearSelection();
   else
   {
      // Click selects the nearest hit part, subsequent clicks cycle to the parts behind it
      vector<std::shared_ptr<EditorUIPart>> hitParts;
      for (const HitTestResult &hr : vhoHit)
      {
         const auto it = m_editableMap.find(HitEditable(hr));
         if (it != m_editableMap.end() && std::ranges::find(hitParts, it->second) == hitParts.end())
            hitParts.push_back(it->second);
      }
      if (hitParts.empty())
         ClearSelection();
      else if (io.KeyCtrl)
      {
         // Ctrl+click lists all the parts under the cursor to pick from
         m_pickList = hitParts;
         m_pickListPos = mousePos;
         m_pickListOpen = true;
      }
      else
      {
         size_t selectionIndex = 0;
         const auto selIt = std::ranges::find(hitParts, m_selection.GetPart());
         if (selIt != hitParts.end())
            selectionIndex = static_cast<size_t>(selIt - hitParts.begin() + 1) % hitParts.size();
         SetSelection(Selection(hitParts[selectionIndex]));
         m_outlinerAnchor = hitParts[selectionIndex];
      }
      // TODO add debug action to make ball active: m_player->m_pactiveballDebug = m_pBall;
   }
}

void EditorUI::PickRay(const ImVec2 &mousePos, const PickView *view, Vertex3Ds &rayNear, Vertex3Ds &rayFar) const
{
   // Compute mouse position in clip space (the main view fills the playfield window, a secondary view its image rect)
   const float rClipWidth = view ? view->rectSize.x * 0.5f : (float)m_player->m_playfieldWnd->GetWidth() * 0.5f;
   const float rClipHeight = view ? view->rectSize.y * 0.5f : (float)m_player->m_playfieldWnd->GetHeight() * 0.5f;
   const float xcoord = (mousePos.x - ((view ? view->rectMin.x : 0.f) + rClipWidth)) / rClipWidth;
   const float ycoord = (((view ? view->rectMin.y : 0.f) + rClipHeight) - mousePos.y) / rClipHeight;

   // Use the inverse of our 3D transform to determine where in 3D space the
   // screen pixel the user clicked on is at.  Get the point at the near
   // clipping plane (z=0) and the far clipping plane (z=1) to get the whole
   // range we need to hit test
   Matrix3D invMVP = view ? view->modelViewProj : m_renderer->GetMVP().GetModelViewProj(0);
   invMVP.Invert();
   rayNear = invMVP * Vertex3Ds { xcoord, ycoord, 0.f };
   rayFar = invMVP * Vertex3Ds { xcoord, ycoord, 1.f };
}

bool EditorUI::IsHiddenInPlayPickable(const PickView *view) const
{
   // The Hidden in Play option of the view clicked (the 3D Viewport's only applies to its editor camera, as its options do)
   if (view != nullptr)
      return view->hiddenInPlay;
   return (m_camMode != ViewMode::EditorCam || IsInspectMode())
      || (Renderer::IsWireframeShade(m_shadeMode) ? m_viewShade.hiddenWire : m_shadeMode == Renderer::ShadeMode::Solid ? m_viewShade.hiddenSolid : m_viewShade.hiddenPbr);
}

bool EditorUI::IsPartPickableInView(const std::shared_ptr<EditorUIPart> &uiPart, bool hiddenInPlayPickable) const
{
   return hiddenInPlayPickable || !uiPart->IsHiddenInPlay();
}

void EditorUI::RayCastParts(const ImVec2 &mousePos, vector<HitTestResult> &vhoHit, const PickView *view) const
{
   Vertex3Ds v3d, v3d2;
   PickRay(mousePos, view, v3d, v3d2);

   // FIXME This is not really great as:
   // - picking depends on what was visible/enabled when quadtree was built (lazily at first pick), and also uses the physics quadtree for some parts
   // - primitives can have hit bug (Apron Top and Gottlieb arm of default table for example): degenerated geometry ?
   // We would need a dedicated quadtree for UI with all parts, and filter after picking by visibility
   vector<HitTestResult> vhoUnfilteredHit;
   m_player->m_physics->RayCast(v3d, v3d2, true, vhoUnfilteredHit);
   const bool hiddenInPlayPickable = IsHiddenInPlayPickable(view);

   // Filter out the colliders that should not be picked
   for (const auto &hr : vhoUnfilteredHit)
   {
      const auto editable = hr.m_obj->m_editable;
      if (editable && (!IsEditablePickable(editable, view != nullptr ? view->visibilityMask : 0) || !IsInLocalView(view, editable)))
         continue;
      if (editable && !hiddenInPlayPickable)
      {
         const auto it = m_editableMap.find(editable);
         if (it != m_editableMap.end() && it->second->IsHiddenInPlay())
            continue; // The view does not draw it: a click selects what is visible
      }
      vhoHit.push_back(hr);
   }
}

bool EditorUI::IsEditableVisible(const IEditable *editable, unsigned int visibilityMask) const
{
   // In desktop backdrop mode, only backdrop parts can be picked, and conversely in the other view modes
   if (editable->m_desktopBackdrop != (m_camMode == ViewMode::DesktopBackdrop))
      return false;
   if (visibilityMask == 0)
      visibilityMask = m_renderer->GetPlayerModeVisibilityMask(); // The 3D Viewport's; a secondary view brings its own
   const PartGroup *parent = editable->GetPartGroup();
   bool visible = editable->IsUIVisible(false);
   while (parent && visible)
   {
      if ((parent->GetPlayerModeVisibilityMask() & visibilityMask) == 0)
         visible = false;
      visible &= parent->IsUIVisible(false);
      parent = parent->GetPartGroup();
   }
   return visible;
}

bool EditorUI::IsEditablePickable(const IEditable *editable, unsigned int visibilityMask) const
{
   if (!IsEditableVisible(editable, visibilityMask))
      return false;
   const auto type = editable->GetItemType();
   if (type == ItemTypeEnum::eItemPrimitive && static_cast<const Primitive *>(editable)->IsRailChannel())
      return false;
   if (!HasFlag(m_selectionFilter, SelectionFilter::Playfield) && type == ItemTypeEnum::eItemPrimitive && static_cast<const Primitive *>(editable)->IsPlayfield())
      return false;
   if (!HasFlag(m_selectionFilter, SelectionFilter::Primitives) && type == ItemTypeEnum::eItemPrimitive)
      return false;
   if (!HasFlag(m_selectionFilter, SelectionFilter::Lights) && type == ItemTypeEnum::eItemLight)
      return false;
   if (!HasFlag(m_selectionFilter, SelectionFilter::Flashers) && type == ItemTypeEnum::eItemFlasher)
      return false;
   return true;
}

void EditorUI::SelectAllParts()
{
   // Select all the pickable parts
   const LocalViewParts &local = *KeyLocalView(); // What the view under the mouse shows (a secondary view's own set, else the 3D Viewport's)
   m_multiSel.clear();
   for (const auto &uiPart : m_editables)
   {
      IEditable *const editable = uiPart->GetEditable();
      if (editable != nullptr && editable->GetItemType() != eItemPartGroup && IsEditablePickable(editable) && local.Allows(editable) && !PlasticFromAtlas::IsCompanion(editable))
         m_multiSel.push_back(uiPart);
   }
   m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
}

void EditorUI::BoxSelectParts(const ImVec2 &cornerA, const ImVec2 &cornerB, bool add)
{
   // Select all the pickable parts whose transform position projects inside the given screen box
   if (!add)
   {
      m_multiSel.clear();
      m_selection = Selection();
   }
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   const ImVec2 boxMin(std::min(cornerA.x, cornerB.x), std::min(cornerA.y, cornerB.y));
   const ImVec2 boxMax(std::max(cornerA.x, cornerB.x), std::max(cornerA.y, cornerB.y));
   const bool hiddenInPlayPickable = IsHiddenInPlayPickable(nullptr); // Box select is the 3D Viewport's only
   for (const auto &uiPart : m_editables)
   {
      IEditable *const editable = uiPart->GetEditable();
      if (editable == nullptr || editable->GetItemType() == eItemPartGroup || !IsEditablePickable(editable) || !IsInLocalView(nullptr, editable) || !IsPartPickableInView(uiPart, hiddenInPlayPickable))
         continue;
      Matrix3D transform;
      if (uiPart->GetTransform(transform) == EditorUIPart::TM_None)
         continue;
      const ImVec2 pos = ctx.Project(transform.GetOrthoNormalPos());
      if (pos.x >= boxMin.x && pos.x <= boxMax.x && pos.y >= boxMin.y && pos.y <= boxMax.y && !IsPartSelected(uiPart))
         m_multiSel.push_back(uiPart);
   }
   if (!m_multiSel.empty() && (m_selection.GetType() != Selection::S_EDITABLE || !IsPartSelected(m_selection.GetPart())))
      m_selection = Selection(m_multiSel.back());
}

void EditorUI::CreatePart(const ItemTypeEnum type, const Vertex2D &pos, const bool wireform, const bool wireRail, const float *const z)
{
   IEditable *const pie = EditableRegistry::CreateAndInit(type, m_table, pos.x, pos.y);
   if (pie == nullptr)
      return;

   // Same initialization sequence as the WinUI editor
   if (auto *const scriptable = pie->GetIScriptable(); scriptable)
   {
      if (wireRail)
         scriptable->m_name = m_table->GetUniqueName("WireRail"s);
      else if (wireform)
         scriptable->m_name = m_table->GetUniqueName("MetalWireGuide"s);
      else
         m_table->GetUniqueName(type, scriptable->m_name);
   }
   if (wireRail)
      static_cast<Primitive *>(pie)->InitWireRail(pos);
   else if (wireform)
      static_cast<Primitive *>(pie)->InitWireform(pos);
   pie->m_desktopBackdrop = (m_camMode == ViewMode::DesktopBackdrop);
   m_table->AddPart(pie);
   pie->SetPartGroup(GetPartGroupForNewPart());
   // A Wire Rail's ball channel, a hidden part: not in the outliner, deleted, copied and moved with the rail, its mesh derived from it
   Primitive *channel = nullptr;
   if (wireRail)
   {
      const string channelName = pie->GetName() + "_Channel";
      channel = static_cast<Primitive *>(EditableRegistry::CreateAndInit(eItemPrimitive, m_table, pos.x, pos.y));
      if (channel != nullptr)
      {
         channel->GetIScriptable()->m_name = m_table->IsNameUnique(channelName) ? channelName : m_table->GetUniqueName(channelName);
         channel->m_desktopBackdrop = pie->m_desktopBackdrop;
         m_table->AddPart(channel);
         channel->SetPartGroup(pie->GetPartGroup());
         channel->m_d.m_plasticHost = pie->GetName();
         channel->m_d.m_plasticRole = kRailChannelRole;
         channel->m_d.m_visible = false;
         channel->m_d.m_reflectionEnabled = false;
         channel->m_d.m_collision_reductionFactor = 0.f;
         channel->SetUILock(true);
         static_cast<Primitive *>(pie)->RegenerateRail();
         m_player->TimerSetup(channel);
         channel->GetIRenderable()->RenderSetup(m_renderer.get());
         if (channel->GetIHitable())
            m_player->m_physics->Add(channel);
      }
   }

   // Same player side setup as player initialization
   if (type == eItemBall)
      m_player->m_vball.push_back(static_cast<Ball *>(pie));
   m_player->TimerSetup(pie);
   if (auto *const renderable = pie->GetIRenderable(); renderable)
      renderable->RenderSetup(m_renderer.get());
   if (pie->GetIHitable())
      m_player->m_physics->Add(pie);

   m_undo.BeginUndo();
   m_undo.MarkForCreate(pie);
   if (channel != nullptr)
      m_undo.MarkForCreate(channel);
   m_undo.SetLabel(wireRail ? "Add Wire Rail"s : wireform ? "Add Metal Wire Guide"s : "Add " + pie->GetName());
   m_undo.EndUndo();
   Report("Added " + pie->GetName());

   // Create the UI part now (it would otherwise be lazily added on next frame) and select the new part
   UpdateEditableList();
   if (const auto it = m_editableMap.find(pie); it != m_editableMap.end())
   {
      SetSelection(Selection(it->second));
      m_outlinerAnchor = it->second;
      if (z != nullptr && !wireRail && !wireform)
      {
         // The height of the 3D cursor, for a part type which has one (the others report no Z move, or ignore it)
         Matrix3D placed;
         if ((it->second->GetTransform(placed) & EditorUIPart::TM_TransZ) != 0)
         {
            placed._43 = *z;
            ApplyPartTransform(it->second, placed, false, false, false);
         }
      }
      if (wireRail)
      {
         // F9: applied in place (the undo record above already holds the state before the add)
         Matrix3D placed;
         GetSelectionTransform(placed);
         SetLastOperation(LastOperation::Type::WireRail, placed);
         const PrimitiveData &d = static_cast<Primitive *>(pie)->m_d;
         m_lastOp.rails = d.m_railCount;
         m_lastOp.railDiameter = d.m_wireThickness * 2.f;
         m_lastOp.wireHeight = 0.f;
      }
      else if (wireform)
      {
         // F9: the wire profile, applied in place (the undo record above already holds the state before the add)
         Matrix3D placed;
         GetSelectionTransform(placed);
         SetLastOperation(LastOperation::Type::Wireform, placed);
         const PrimitiveData &d = static_cast<Primitive *>(pie)->m_d;
         m_lastOp.wireHeight = d.m_wireHeight;
         m_lastOp.thickness = d.m_wireThickness;
         m_lastOp.standHeight = d.m_wireStandHeight;
         m_lastOp.bendRadius = d.m_wireBendRadius;
         m_lastOp.ends = d.m_wireEnds;
         m_lastOp.rails = d.m_wireRails;
         m_lastOp.spacing = d.m_wireSpacing;
      }
      else if (Matrix3D placed; GetSelectionTransform(placed))
      {
         SetLastOperation(LastOperation::Type::Add, placed);
         m_lastOp.base = vec3(0.f, 0.f, placed._43);
         m_lastOp.offset = vec3(placed._41, placed._42, 0.f);
      }
   }
}

void EditorUI::CreateCollection(bool fromSelection)
{
   // Same initialization sequence as the WinUI editor (PinTableWnd::NewCollection)
   CComObject<Collection> *pcol;
   CComObject<Collection>::CreateInstance(&pcol);
   pcol->AddRef();
   pcol->m_name = m_table->GetUniqueName("Collection"s);
   if (fromSelection)
   {
      vector<IEditable *> members;
      for (const auto &uiPart : m_multiSel)
         members.push_back(uiPart->GetEditable());
      PlasticFromAtlas::AddCompanions(members); // A script hiding the collection's plastics hides all three parts of each
      PlayfieldMaker::StripBoard(*this, members); // The playfield follows the table
      for (IEditable *const editable : members)
         if (editable && editable->GetIScriptable()) // Collections only reference scriptable parts (persisted by name)
            m_table->AddPartToCollection(pcol, editable);
   }
   m_table->AddCollection(pcol);
   pcol->Release();
   m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
   SetSelection(Selection(pcol));
}

PartGroup *EditorUI::GetPartGroupForNewPart()
{
   // Assign new parts to the group of the current selection. When the selection does not define a
   // group, favor the most recently used ones (still respecting UI visibility), then fall back to
   // the visibility mask (first UI visible root group, else first root group). The target is only
   // empty when the table does not have any part group at all.
   PartGroup *partGroup = nullptr;
   if (const std::shared_ptr<EditorUIPart> &part = m_selection.GetPart())
      partGroup = part->GetEditable()->GetItemType() == eItemPartGroup ? static_cast<PartGroup *>(part->GetEditable()) : part->GetEditable()->GetPartGroup();
   if (partGroup == nullptr)
   {
      const vector<IEditable *> &parts = m_table->GetParts();
      std::erase_if(m_partGroupUseHistory, [&parts](const PartGroup *group) { return std::ranges::find(parts, group) == parts.end(); });
      for (PartGroup *const group : m_partGroupUseHistory)
         if (group->IsUIVisible(true))
         {
            partGroup = group;
            break;
         }
   }
   if (partGroup == nullptr)
   {
      for (IEditable *const part : m_table->GetParts())
      {
         if (part->GetItemType() == eItemPartGroup && part->GetPartGroup() == nullptr)
         {
            if (part->IsUIVisible(false))
            {
               partGroup = static_cast<PartGroup *>(part);
               break;
            }
            if (partGroup == nullptr)
               partGroup = static_cast<PartGroup *>(part);
         }
      }
   }
   if (partGroup != nullptr)
   {
      std::erase(m_partGroupUseHistory, partGroup);
      m_partGroupUseHistory.insert(m_partGroupUseHistory.begin(), partGroup);
   }
   return partGroup;
}

void EditorUI::CopySelection()
{
   if (m_pointEditPart)
   {
      // Copy the coordinates of the selected drag point or center point
      if (m_pointEditCenter)
      {
         if (m_centerSelected)
            VPX::EditorClipboard::CopyPoint(m_pointEditPart->GetEditCenter());
      }
      else if (m_pointSel.size() == 1)
         VPX::EditorClipboard::CopyPoint(m_pointSel.front()->GetVertex());
      return;
   }
   vector<IEditable *> parts;
   for (const auto &part : m_multiSel)
      if (IEditable *const editable = part->GetEditable(); editable != nullptr)
         parts.push_back(editable);
   PlasticFromAtlas::AddCompanions(parts); // A plastic is copied with its companions
   if (PlayfieldMaker::StripBoard(*this, parts) && parts.empty())
      return;
   RestorePartsVisibility(); // Copy the authored visibility, not the editor display state
   VPX::EditorClipboard::CopyParts(parts);
   if (!parts.empty())
      Report(CountLabel("Copied", parts.size(), parts.front()->GetName()));
}

void EditorUI::PasteSelection(const ImVec2 &pos, const bool inPlace)
{
   if (m_measure.IsActive())
   {
      Report("Measure tool active");
      return;
   }
   if (m_table->IsLocked() || IsInspectMode())
      return;
   if (m_pointEditPart)
   {
      // Paste the copied coordinates to the selected drag point or center point
      Vertex3Ds pointPos;
      if (m_pointEditCenter)
      {
         if (m_centerSelected && VPX::EditorClipboard::GetPoint(pointPos))
         {
            BeginPointEdit();
            m_pointEditPart->SetEditCenter(Vertex2D(pointPos.x, pointPos.y));
            EndPointEdit();
         }
      }
      else if (m_pointSel.size() == 1 && VPX::EditorClipboard::GetPoint(pointPos))
      {
         BeginPointEdit();
         m_pointSel.front()->SetX(pointPos.x);
         m_pointSel.front()->SetY(pointPos.y);
         if (!PlasticFromAtlas::IsEditing(*this)) // On a plastic Z is a hole's diameter or the outline's zero: X and Y only
            m_pointSel.front()->SetZ(pointPos.z);
         EndPointEdit();
      }
      return;
   }
   const vector<vector<uint8_t>> parts = VPX::EditorClipboard::GetParts();
   if (parts.empty())
      return;
   // A board (or its companions) is never pasted: the table makes its one board by Add > Playfield (Copy leaves it out, older clipboards may hold it)
   for (const vector<uint8_t> &partData : parts)
   {
      if (partData.size() <= sizeof(int) || *reinterpret_cast<const ItemTypeEnum *>(partData.data()) != eItemPrimitive)
         continue;
      IEditable *const editable = EditableRegistry::Create(eItemPrimitive);
      if (editable == nullptr)
         continue;
      BiffReader reader(partData.data() + sizeof(int), static_cast<uint32_t>(partData.size() - sizeof(int)), CURRENT_FILE_FORMAT_VERSION, nullptr, 0);
      editable->Load(reader);
      const bool board = PlayfieldMaker::IsPart(editable);
      editable->Release();
      if (board)
      {
         Report(PlayfieldMaker::kOneBoard);
         return;
      }
   }
   const bool backdrop = (m_camMode == ViewMode::DesktopBackdrop);
   PartGroup *const partGroup = GetPartGroupForNewPart();
   vector<IEditable *> pasted;
   vector<string> originalNames; // Aligned with 'pasted': the names the parts were copied with (rubbers follow their posts by name)
   Vertex2D pasteMin(FLT_MAX, FLT_MAX), pasteMax(-FLT_MAX, -FLT_MAX);
   m_undo.BeginUndo();
   for (const vector<uint8_t> &partData : parts)
   {
      if (partData.size() <= sizeof(int))
         continue;
      const ItemTypeEnum type = *reinterpret_cast<const ItemTypeEnum *>(partData.data());
      IEditable *const editable = EditableRegistry::Create(type);
      if (editable == nullptr)
         continue;
      BiffReader reader(partData.data() + sizeof(int), static_cast<uint32_t>(partData.size() - sizeof(int)), CURRENT_FILE_FORMAT_VERSION, nullptr, 0);
      editable->Load(reader);
      editable->m_desktopBackdrop = backdrop;
      originalNames.push_back(editable->GetName());
      // If the original name is not yet used, use that one, otherwise add/increase the suffix until we find a name that's not used yet
      if (!m_table->IsNameUnique(editable->GetName()))
      {
         // First remove the existing suffix
         const string input = editable->GetName();
         size_t lastNonDigit = input.length();
         while (lastNonDigit > 0 && input[lastNonDigit - 1] >= '0' && input[lastNonDigit - 1] <= '9')
            --lastNonDigit;
         editable->SetName(m_table->GetUniqueName(input.substr(0, lastNonDigit)));
      }
      editable->SetPartGroup(partGroup);
      m_table->AddPart(editable);
      m_undo.MarkForCreate(editable);
      pasted.push_back(editable);
      if (editable->GetItemType() == eItemPrimitive && !static_cast<Primitive *>(editable)->m_d.m_plasticHost.empty())
         continue; // A companion sits at the origin: not part of the pasted bounds
      const Vertex2D center = editable->GetCenter();
      pasteMin.x = min(pasteMin.x, center.x);
      pasteMin.y = min(pasteMin.y, center.y);
      pasteMax.x = max(pasteMax.x, center.x);
      pasteMax.y = max(pasteMax.y, center.y);
   }
   // A pasted rubber that follows posts follows their copies if all of them were pasted with it, else it is detached (it keeps its shape)
   for (IEditable *const editable : pasted)
   {
      if (editable->GetItemType() != eItemRubber || static_cast<Rubber *>(editable)->m_d.m_postRefs.empty())
         continue;
      RubberData &d = static_cast<Rubber *>(editable)->m_d;
      vector<string> copies;
      for (const string &ref : d.m_postRefs)
      {
         for (size_t i = 0; i < pasted.size(); i++)
            if (pasted[i]->GetItemType() == eItemPrimitive && StrCompareNoCase(originalNames[i], ref))
            {
               copies.push_back(pasted[i]->GetName());
               break;
            }
      }
      if (copies.size() == d.m_postRefs.size())
         d.m_postRefs = copies;
      else
      {
         d.m_postRefs.clear();
         d.m_postRoundness = 0.f;
      }
   }
   PlasticFromAtlas::FixPasted(pasted, originalNames);
   if (!pasted.empty())
      m_undo.SetLabel(CountLabel("Paste", pasted.size(), pasted.front()->GetName()));
   m_undo.EndUndo();
   if (pasted.empty())
   {
      m_undo.Discard();
      return;
   }
   // Move the pasted parts so that their combined center lands at the given position (in the table XY plane, keeping their Z)
   // (a duplicate stays exactly on the original, its grab then follows the mouse movement from there)
   const Vertex2D target = inPlace ? Vertex2D(0.f, 0.f) : UnprojectToPlane(pos, 0.f);
   const Vertex2D offset = inPlace ? Vertex2D(0.f, 0.f) : Vertex2D(target.x - 0.5f * (pasteMin.x + pasteMax.x), target.y - 0.5f * (pasteMin.y + pasteMax.y));
   for (IEditable *const editable : pasted)
   {
      if (!inPlace)
         editable->Translate(offset);
      // Player side setup, same as part creation
      if (editable->GetItemType() == eItemBall)
         m_player->m_vball.push_back(static_cast<Ball *>(editable));
      m_player->TimerSetup(editable);
      if (auto *const renderable = editable->GetIRenderable(); renderable)
         renderable->RenderSetup(m_renderer.get());
      if (editable->GetIHitable())
         m_player->m_physics->Add(editable);
   }
   for (IEditable *const editable : pasted) // The copies of a plastic: companions rebuilt from their host copy
      if (editable->GetItemType() == eItemPrimitive && static_cast<Primitive *>(editable)->m_d.m_plasticGen)
         static_cast<Primitive *>(editable)->UpdatePlastic();
   // Select the pasted parts
   UpdateEditableList();
   m_multiSel.clear();
   for (IEditable *const editable : pasted)
      if (editable->GetItemType() == eItemPrimitive && !static_cast<Primitive *>(editable)->m_d.m_plasticHost.empty())
         continue; // Companions follow their host, they are not selected (the host stays the active part)
      else if (const auto it = m_editableMap.find(editable); it != m_editableMap.end())
         m_multiSel.push_back(it->second);
   m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   m_outlinerAnchor = m_multiSel.empty() ? nullptr : m_multiSel.back();
   if (Matrix3D placed; !m_multiSel.empty() && GetSelectionTransform(placed))
   {
      // The offset applied to the copied coordinates is the parameter (the original position is the pasted one minus it)
      SetLastOperation(LastOperation::Type::Paste, placed);
      m_lastOp.base = vec3(placed._41 - offset.x, placed._42 - offset.y, placed._43);
      m_lastOp.offset = vec3(offset.x, offset.y, 0.f);
   }
}

vector<IEditable *> EditorUI::SelectedEditables() const
{
   vector<IEditable *> parts;
   for (const auto &part : m_multiSel)
      if (IEditable *const edit = part->GetEditable(); edit != nullptr)
         parts.push_back(edit);
   return parts;
}

// Linked rubbers that follow one of the touched primitives, or are themselves touched (their own fields changed)
vector<Rubber *> EditorUI::DependentRubbers(const vector<IEditable *> &posts) const
{
   vector<Rubber *> rubbers;
   std::unordered_set<string> names; // Lowercase names of the touched primitives
   for (const IEditable *const post : posts)
      if (post->GetItemType() == eItemPrimitive)
         names.insert(lowerCase(post->GetName()));
   for (IEditable *const part : m_table->GetParts())
   {
      if (part->GetItemType() != eItemRubber || static_cast<Rubber *>(part)->m_d.m_postRefs.empty())
         continue;
      Rubber *const rubber = static_cast<Rubber *>(part);
      if (std::ranges::find(posts, part) != posts.end() || std::ranges::any_of(rubber->m_d.m_postRefs, [&names](const string &ref) { return names.contains(lowerCase(ref)); }))
         rubbers.push_back(rubber);
   }
   return rubbers;
}

void EditorUI::MarkDependentsForUndo() { MarkDependentsForUndo(SelectedEditables()); }

void EditorUI::MarkDependentsForUndo(const vector<IEditable *> &touched)
{
   for (Rubber *const rubber : DependentRubbers(touched))
      m_undo.MarkForUndo(rubber, true, false); // Not counted in the label: only the edited parts are
}

void EditorUI::DetachSelectedRubbersFromUnselectedPosts()
{
   const vector<IEditable *> selected = SelectedEditables();
   std::unordered_set<string> names;
   for (const IEditable *const part : selected)
      if (part->GetItemType() == eItemPrimitive)
         names.insert(lowerCase(part->GetName()));
   for (IEditable *const part : selected)
      if (part->GetItemType() == eItemRubber)
         if (const auto &refs = static_cast<Rubber *>(part)->m_d.m_postRefs; !refs.empty() && !std::ranges::all_of(refs, [&names](const string &ref) { return names.contains(lowerCase(ref)); }))
            DetachRubberRefs(part);
}

// The record of the edit already holds the rubbers (MarkDependentsForUndo before it, DropDeletedPostRefs for a deletion)
void EditorUI::RefitDependents(const vector<IEditable *> &touched)
{
   if (m_modal.active || m_gizmoDragActive)
   {
      // Per frame while dragging: no table scan, skip unchanged rubbers, and no physics update (a full collider rebuild) until FlushRubberDrag
      if (!m_rubberDrag.valid)
      {
         m_rubberDrag.rubbers = DependentRubbers(touched);
         if (!m_rubberDrag.rubbers.empty())
            m_rubberDrag.posts = RubberBetweenPosts::PostsByName(*this);
         m_rubberDrag.valid = true;
      }
      for (Rubber *const rubber : m_rubberDrag.rubbers)
         if (RubberBetweenPosts::Refit(*this, rubber, &m_rubberDrag.posts, false, false) && std::ranges::find(m_rubberDrag.pendingPhysics, rubber) == m_rubberDrag.pendingPhysics.end())
            m_rubberDrag.pendingPhysics.push_back(rubber);
      return;
   }
   const vector<Rubber *> rubbers = DependentRubbers(touched);
   if (rubbers.empty())
      return;
   const RubberBetweenPosts::PostMap posts = RubberBetweenPosts::PostsByName(*this);
   for (Rubber *const rubber : rubbers)
      RubberBetweenPosts::Refit(*this, rubber, &posts, false);
}

void EditorUI::FlushRubberDrag()
{
   if (!m_rubberDrag.pendingPhysics.empty())
   {
      const auto &parts = m_table->GetParts();
      for (Rubber *const rubber : m_rubberDrag.pendingPhysics)
         if (std::ranges::find(parts, static_cast<IEditable *>(rubber)) != parts.end()) // Still in the table (a canceled drag restored it)
            m_player->m_physics->Update(rubber);
   }
   m_rubberDrag = RubberDrag();
}

void EditorUI::DetachRubberRefs(IEditable *const part)
{
   if (part == nullptr || part->GetItemType() != eItemRubber || static_cast<Rubber *>(part)->m_d.m_postRefs.empty())
      return;
   RubberData &d = static_cast<Rubber *>(part)->m_d;
   d.m_postRefs.clear();
   d.m_postRoundness = 0.f;
   Report(part->GetName() + " no longer follows its posts");
}

void EditorUI::DropDeletedPostRefs(const vector<IEditable *> &deleted)
{
   m_rubberFitHash.clear(); // Addresses of deleted rubbers may be reused
   vector<string> names;
   for (const IEditable *const part : deleted)
      if (part->GetItemType() == eItemPrimitive)
         names.push_back(part->GetName());
   if (names.empty())
      return;
   const RubberBetweenPosts::PostMap posts = RubberBetweenPosts::PostsByName(*this);
   for (IEditable *const part : m_table->GetParts()) // The deleted parts are no longer in the table
   {
      if (part->GetItemType() != eItemRubber)
         continue;
      Rubber *const rubber = static_cast<Rubber *>(part);
      vector<string> &refs = rubber->m_d.m_postRefs;
      const auto gone = [&names](const string &ref) { return std::ranges::any_of(names, [&ref](const string &n) { return StrCompareNoCase(n, ref); }); };
      if (refs.empty() || !std::ranges::any_of(refs, gone))
         continue;
      m_undo.MarkForUndo(rubber);
      std::erase_if(refs, gone);
      if (refs.empty()) // Nothing left to follow: keeps its shape
         rubber->m_d.m_postRoundness = 0.f;
      else
         RubberBetweenPosts::Refit(*this, rubber, &posts);
   }
}

void EditorUI::DeleteSelection()
{
   if (m_measure.IsActive())
   {
      Report("Measure tool active");
      return;
   }
   if (m_table->IsLocked() || IsInspectMode())
      return;
   if (m_pointEditPart) // Toolbar delete while editing drag points: delete the points, the edited part must stay alive
   {
      DeleteSelectedPoints();
      return;
   }
   // Balls are runtime objects, parts without a part group are live objects or root groups: they can not be deleted (and stay selected)
   vector<IEditable *> partsToDelete;
   for (const auto &part : m_multiSel)
      if (IEditable *const edit = part->GetEditable(); edit->GetItemType() != eItemBall && edit->GetPartGroup() != nullptr && std::ranges::find(partsToDelete, edit) == partsToDelete.end())
         partsToDelete.push_back(edit);
   // Delete group content, nested groups included (partsToDelete grows while iterating)
   for (size_t i = 0; i < partsToDelete.size(); i++)
      if (partsToDelete[i]->GetItemType() == eItemPartGroup)
         for (IEditable *const edit : m_table->GetParts())
            if (edit->GetPartGroup() == partsToDelete[i] && edit->GetItemType() != eItemBall && std::ranges::find(partsToDelete, edit) == partsToDelete.end())
               partsToDelete.push_back(edit);
   PlasticFromAtlas::AddCompanions(partsToDelete); // A plastic goes with its companions
   PlayfieldMaker::StripBoard(*this, partsToDelete);
   PlayfieldMaker::StripBoardGroups(*this, partsToDelete); // A group holding the board stays, with the board in it (its other parts go)
   if (partsToDelete.empty())
      return;

   // Begun before the selection changes, so that undoing restores the deleted parts as selected
   m_undo.BeginUndo();
   for (IEditable *const edit : partsToDelete)
   {
      if (const auto it = m_editableMap.find(edit); it != m_editableMap.end())
      {
         const std::shared_ptr<EditorUIPart> uiPart = it->second;
         uiPart->RestorePartVisibility(); // While the part is alive, so that it is authored when undeleted
         m_editableMap.erase(it);
         RemoveFromVectorSingle(m_editables, uiPart);
      }
      // Release the player side resources (restored by PinUndo::Undo), the render ones at the end of the frame as the render thread may still use them.
      // The release runs on the render thread: an undelete that happens before it must not touch the render state (see PinUndo::Undo)
      if (edit->GetIHitable())
         m_player->m_physics->Remove(edit);
      if (edit->m_phittimer)
         m_player->TimerRelease(edit);
      m_renderer->DropRenderable(edit->GetIRenderable());
      edit->AddRef(); // Keep the part alive until its deferred render release has been processed
      edit->m_renderReleasePending = true;
      m_renderer->m_renderDevice->AddEndOfFrameCmd([edit]()
         {
            if (edit->GetIRenderable())
               edit->GetIRenderable()->RenderRelease();
            edit->m_renderReleasePending = false;
            edit->Release();
         });
      m_undo.MarkForDelete(edit); // The undo record keeps the part alive (until undone or dropped)
      for (Collection *const collection : edit->m_vCollection) // m_vCollection is kept for Undelete
         collection->RemovePart(edit);
      m_table->RemovePart(edit);
   }
   DropDeletedPostRefs(partsToDelete);
   m_undo.SetLabel(CountLabel("Delete", partsToDelete.size(), partsToDelete.front()->GetName()));
   m_undo.EndUndo();
   Report(CountLabel("Deleted", partsToDelete.size(), partsToDelete.front()->GetName()));
   m_lastUndoPart = nullptr; // Start a new record on next edit (a deleted part's address may be reused)
   m_lastUndoId = 0;

   // Remove the deleted parts from the selection (non deletable parts stay selected)
   std::erase_if(m_multiSel, [this](const auto &part) { return std::ranges::find(m_editables, part) == m_editables.end(); });
   if (m_selection.GetType() == Selection::S_EDITABLE && !IsPartSelected(m_selection.GetPart()))
      m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   if (m_outlinerAnchor && std::ranges::find(m_editables, m_outlinerAnchor) == m_editables.end())
      m_outlinerAnchor.reset();
}

void EditorUI::RestorePartsVisibility() const
{
   // The editor keeps overriding the parts' m_d visibility fields with the outliner state (see EditableUIPart::Render).
   // Only parts still in the table are known to be alive: UI parts of removed parts are only dropped by UpdateEditableList
   const ankerl::unordered_dense::set<const IEditable *> liveParts(m_table->GetParts().begin(), m_table->GetParts().end());
   for (const auto &uiPart : m_editables)
      if (liveParts.contains(uiPart->GetEditable()))
         uiPart->RestorePartVisibility();
}

void EditorUI::UpdateEditableList()
{
   if (m_plasticRepairedTable != m_table && m_renderer != nullptr && m_player != nullptr) // Once per table, before its parts are listed
   {
      m_plasticRepairedTable = m_table;
      PlasticFromAtlas::RepairCompanions(*this);
   }
   // In full edit mode, parts without a part group are live objects generated at runtime (implicit
   // playfield mesh, script created parts, ...): they are not part of the table file, so they are
   // not listed in the outliner and can not be selected
   const auto isLiveObject = [this](const IEditable *edit) { return (m_table->m_liveBaseTable == nullptr) && (edit->GetPartGroup() == nullptr) && (edit->GetItemType() != eItemPartGroup); };
   // Remove UI parts of removed editables (or of live objects in full edit mode)
   const ankerl::unordered_dense::set<const IEditable *> liveParts(m_table->GetParts().begin(), m_table->GetParts().end());
   std::erase_if(m_editables,
      [this, &liveParts, &isLiveObject](const auto &uiPart)
      {
         if (!liveParts.contains(uiPart->GetEditable()) || isLiveObject(uiPart->GetEditable()))
         {
            m_editableMap.erase(uiPart->GetEditable());
            return true;
         }
         return false;
      });
   // Drop removed parts from the multi selection, keeping a valid active part
   std::erase_if(m_multiSel, [&liveParts, &isLiveObject](const auto &part) { return !liveParts.contains(part->GetEditable()) || isLiveObject(part->GetEditable()); });
   if (m_selection.GetType() == Selection::S_EDITABLE && !IsPartSelected(m_selection.GetPart()))
      m_selection = m_multiSel.empty() ? Selection() : Selection(m_multiSel.back());
   if (m_outlinerAnchor && (!liveParts.contains(m_outlinerAnchor->GetEditable()) || isLiveObject(m_outlinerAnchor->GetEditable())))
      m_outlinerAnchor.reset();
   // Add UI parts for new editables
   bool needSort = false;
   ankerl::unordered_dense::set<PartGroup *> newGroups;
   for (const auto &edit : m_table->GetParts())
   {
      if (isLiveObject(edit))
         continue;
      const auto it = m_editableMap.find(edit);
      if (it == m_editableMap.end()) // New part
      {
         std::shared_ptr<EditorUIPart> uiPart = EditorUIPartRegistry::Create(edit);
         if (uiPart == nullptr) // eItemTable, eItemLightCenter, eItemDragPoint, eItemCollection
            uiPart = std::make_shared<BaseUIPart>(edit);
         if (m_table->m_liveBaseTable)
            edit->SetUIVisible(true);
         else if (edit->GetItemType() == eItemPartGroup)
            newGroups.insert(static_cast<PartGroup *>(edit));
         uiPart->SetOutlinerPath(edit->GetPathString(false));
         m_editables.push_back(uiPart);
         m_editableMap[edit] = std::move(uiPart);
         needSort = true;
      }
      else if (it->second->GetOutlinerPath() != edit->GetPathString(false)) // Name or parent group has changed
      {
         needSort = true;
         it->second->SetOutlinerPath(edit->GetPathString(false));
      }
   }
   // Win32 UI does not manage PartGroup UI hidden/shown state, so we lazily initialize new groups in a
   // single pass: hidden by default in edit mode, shown if they contain at least one visible part
   if (!newGroups.empty())
   {
      for (PartGroup *group : newGroups)
         group->SetUIVisible(false);
      for (const auto &edit : m_table->GetParts())
         if (edit->GetItemType() != eItemPartGroup && edit->IsUIVisible(false))
            for (PartGroup *group = edit->GetPartGroup(); group != nullptr; group = group->GetPartGroup())
               if (newGroups.contains(group))
                  group->SetUIVisible(true);
   }
   // Sort in tree order for the outliner (root live objects first, then each group directly followed by its content, siblings by name).
   // Paths are compared name by name: comparing the joined path strings would put "Lights GI/..." between "Lights" and "Lights/..."
   // (' ' sorts before '/'), and the outliner would then lose the parts of "Lights"
   if (needSort)
   {
      const auto pathOf = [](const IEditable *edit)
      {
         vector<const string *> path;
         for (const IEditable *part = edit; part != nullptr; part = part->GetPartGroup())
            path.push_back(&part->GetName());
         std::ranges::reverse(path);
         return path;
      };
      vector<std::pair<vector<const string *>, std::shared_ptr<EditorUIPart>>> sorted;
      sorted.reserve(m_editables.size());
      for (auto &uiPart : m_editables)
         sorted.emplace_back(pathOf(uiPart->GetEditable()), std::move(uiPart));
      std::ranges::sort(sorted,
         [](const auto &a, const auto &b)
         {
            const bool isRootA = a.second->GetEditable()->GetPartGroup() == nullptr && a.second->GetEditable()->GetItemType() != eItemPartGroup;
            const bool isRootB = b.second->GetEditable()->GetPartGroup() == nullptr && b.second->GetEditable()->GetItemType() != eItemPartGroup;
            if (isRootA != isRootB)
               return isRootA;
            return std::ranges::lexicographical_compare(a.first, b.first, [](const string *x, const string *y) { return StrLessNoCase(*x, *y); });
         });
      for (size_t i = 0; i < sorted.size(); i++)
         m_editables[i] = std::move(sorted[i].second);
   }
}

void EditorUI::RenderNavCube()
{
   // Not in the 2D desktop backdrop view, nor with the side panels hidden (it is kept in inspect mode to orbit the running game)
   if (!m_showNavCube || !m_viewportVisible || m_flyMode || m_table->IsLocked() || m_inspectionModal.IsVisible() || m_camMode == ViewMode::DesktopBackdrop)
   {
      SDLInputHandler::SetMouseOverEditorWidget(false); // Hidden cube: nothing is filtered
      m_navCubeFaceAnim = false;
      return;
   }

   ImGuiIO &io = ImGui::GetIO();
   const float dpi = m_liveUI.GetDPI();
   const float size = 128.f * dpi;
   const ImVec2 pos(m_viewportContentRect.x1 - size - 16.f * dpi, m_viewportContentRect.y0 + 32.f * dpi); // Below the shading row

   // The cube is computed in a right handed, Y up world, while the editor world is X right, Y down the table, Z down (height is -Z):
   // R (row vector convention) maps editor Z (down) to -Y and editor Y to Z, a pure rotation about X. viewG = R^-1 * viewV, viewV = R * viewG
   static constexpr float R[16] = { 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, -1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f };
   auto mul = [](const float *a, const float *b, float *o)
   {
      for (int i = 0; i < 4; i++)
         for (int j = 0; j < 4; j++)
            o[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
   };
   float Rinv[16];
   for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
         Rinv[i * 4 + j] = R[j * 4 + i];

   Matrix3D inv(m_camView);
   inv.Invert();
   const vec3 oldDir = inv.GetOrthoNormalDir();
   const vec3 camTarget = inv.GetOrthoNormalPos() - oldDir * m_camDistance; // Same orbit pivot as the mouse orbit

   float viewG[16], viewGIn[16];
   mul(Rinv, &m_camView.m[0][0], viewG);
   memcpy(viewGIn, viewG, sizeof(viewG));
   // Own right handed projection so that the cube handedness does not depend on the editor (possibly orthographic) projection
   const Matrix3D proj = Matrix3D::MatrixPerspectiveFovRH(m_navFovY, io.DisplaySize.x / io.DisplaySize.y, 1.f, 1000.f);
   const float identity[16] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };

   // Do not fight the selection gizmo: while it is dragged, the cube gets no mouse input (it is still drawn)
   const ImVec2 savedMousePos = io.MousePos;
   const bool savedMouseDown = io.MouseDown[0];
   const bool gizmoBusy = ImGuizmo::IsUsing();
   if (gizmoBusy)
   {
      io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
      io.MouseDown[0] = false;
   }
   ImDrawList *const cubeDrawList = ImGui::FindWindowByName("gizmo") != nullptr ? ImGui::FindWindowByName("gizmo")->DrawList : nullptr;
   const int cubeFirstCmd = cubeDrawList != nullptr ? max(0, cubeDrawList->CmdBuffer.Size - 1) : 0;
   ImGuizmo::ViewManipulate(viewG, proj.m[0], ImGuizmo::TRANSLATE, ImGuizmo::WORLD, const_cast<float *>(identity), m_camDistance, pos, ImVec2(size, size), 0x40101010);
   ClipDrawCommands(cubeDrawList, cubeFirstCmd, m_viewportVisible ? &m_viewportRect : nullptr);
   if (gizmoBusy)
   {
      io.MousePos = savedMousePos;
      io.MouseDown[0] = savedMouseDown;
   }
   SDLInputHandler::SetMouseOverEditorWidget(!gizmoBusy && ImGuizmo::IsViewManipulateHovered()); // The game must not get a press made on the cube

   if (memcmp(viewG, viewGIn, sizeof(viewG)) == 0)
   {
      m_navCubeFaceAnim = false;
      return;
   }
   // ImGuizmo's drag orbit builds a look at with a fixed Y up: from an axis view (default Top) the first drag frame has a zero mouse delta, the view direction
   // is parallel to up and the look at degenerates (zero or NaN basis), which would poison the editor camera for good. Skip such frames, the next ones recover.
   for (int axis = 0; axis < 3; axis++)
   {
      const float l2 = viewG[axis] * viewG[axis] + viewG[4 + axis] * viewG[4 + axis] + viewG[8 + axis] * viewG[8 + axis];
      if (!(l2 > 0.5f && l2 < 2.f)) // also rejects NaN
         return;
   }

   // Back to the editor camera: keep the pivot and distance, take the new direction and up
   float viewV[16];
   mul(R, viewG, viewV);
   Matrix3D newInv;
   memcpy(&newInv.m[0][0], viewV, sizeof(viewV));
   newInv.Invert();
   vec3 newDir = newInv.GetOrthoNormalDir();
   vec3 newUp = newInv.GetOrthoNormalUp();
   m_predefinedView = PredefinedView::None;

   if (!io.MouseDown[0]) // Face click: the cube animates the view toward an axis; use the editor predefined views up vectors and an orthographic camera
   {
      struct Axis
      {
         vec3 dir, up;
         PredefinedView view;
      };
      const Axis axes[6] = { { vec3(0.f, 0.f, -1.f), vec3(0.f, -1.f, 0.f), PredefinedView::Top }, { vec3(0.f, 0.f, 1.f), vec3(0.f, -1.f, 0.f), PredefinedView::Bottom },
         { vec3(0.f, 1.f, 0.f), vec3(0.f, 0.f, -1.f), PredefinedView::Front }, { vec3(0.f, -1.f, 0.f), vec3(0.f, 0.f, -1.f), PredefinedView::Back },
         { vec3(-1.f, 0.f, 0.f), vec3(0.f, 0.f, -1.f), PredefinedView::Left }, { vec3(1.f, 0.f, 0.f), vec3(0.f, 0.f, -1.f), PredefinedView::Right } };
      if (!m_navCubeFaceAnim) // Once, when the click starts: a numpad 5 during the animation must stay
         m_perspectiveCam = false;
      m_navCubeFaceAnim = true;
      for (const Axis &a : axes)
         if (newDir.Dot(a.dir) > 0.9999f)
         {
            newDir = a.dir;
            newUp = a.up;
            m_predefinedView = a.view;
            break;
         }
   }
   m_camMode = ViewMode::EditorCam;
   m_camView = Matrix3D::MatrixLookAtRH(camTarget + newDir * m_camDistance, camTarget, newUp);
}

void EditorUI::FrameBounds(const FRect3D &bounds)
{
   const ImGuiIO &io = ImGui::GetIO();
   m_camMode = ViewMode::EditorCam;
   Matrix3D view(m_camView);
   view.Invert();
   const vec3 up = view.GetOrthoNormalUp();
   const vec3 dir = view.GetOrthoNormalDir();
   const vec3 right = view.GetOrthoNormalRight();
   const vec3 newTarget(0.5f * (bounds.left + bounds.right), 0.5f * (bounds.top + bounds.bottom), -0.5f * (bounds.zlow + bounds.zhigh));

   // Evaluate the distance needed for all the bounds corners to be inside the view, keeping the camera orientation
   // and fitting inside the 3D viewport area (its content rect: the area the UI leaves to the render, which is drawn over the full window):
   // the half size of the area and its offset from the window center, as fractions of the half window (NDC)
   const float aspect = io.DisplaySize.x / io.DisplaySize.y;
   if (!m_areaRectsValid)
      ComputeAreaRects();
   const AreaRect &vc = m_viewportContentRect;
   const bool useRect = m_viewportVisible && !m_flyMode && !m_inspectionModal.IsVisible() && vc.x1 > vc.x0 && vc.y1 > vc.y0;
   Vertex2D visible = GetUIVisibleFraction(); // Maximized panel: frame for the layout that comes back on restore (centered)
   float offX = 0.f, offY = 0.f; // The area center in NDC (x right, y up)
   if (useRect)
   {
      visible = Vertex2D(max(0.1f, (vc.x1 - vc.x0) / io.DisplaySize.x), max(0.1f, (vc.y1 - vc.y0) / io.DisplaySize.y));
      offX = ((vc.x0 + vc.x1) / io.DisplaySize.x) - 1.f;
      offY = 1.f - ((vc.y0 + vc.y1) / io.DisplaySize.y);
   }
   const float tanHalfFovY = tanf(0.5f * ANGTORAD(m_navFovY));
   const float tx = tanHalfFovY * aspect;
   float distance = 0.f;
   for (int i = 0; i < 8; i++)
   {
      const vec3 toCorner = vec3((i & 1) ? bounds.right : bounds.left, (i & 2) ? bounds.bottom : bounds.top, (i & 4) ? -bounds.zlow : -bounds.zhigh) - newTarget;
      const float rx = right.Dot(toCorner), ry = up.Dot(toCorner), dz = dir.Dot(toCorner);
      // Perspective: the camera is panned so that the bounds center lands on the area center, which makes the corner's offset depend on its depth
      distance = max(distance,
         m_perspectiveCam ? dz + max(fabsf(ry + offY * tanHalfFovY * dz) / (tanHalfFovY * visible.y), fabsf(rx + offX * tx * dz) / (tx * visible.x))
                          : max(fabsf(ry) / visible.y, fabsf(rx) / (aspect * visible.x)));
   }
   if (distance > 0.f) // Keep the current distance for degenerate (point) selections
      m_camDistance = distance * 1.1f; // Small margin so that the selection does not exactly touch the view borders
   // Pan the camera (the orbit target follows) so the bounds center is at the area center
   const float halfW = m_perspectiveCam ? m_camDistance * tx : m_camDistance * aspect, halfH = m_perspectiveCam ? m_camDistance * tanHalfFovY : m_camDistance;
   const vec3 panTarget = newTarget - right * (offX * halfW) - up * (offY * halfH);
   const vec3 newEye = panTarget + dir * m_camDistance;
   m_camView = Matrix3D::MatrixLookAtRH(newEye, panTarget, up);
}

// Numpad key, or the matching top row digit when numpad emulation is enabled (laptops without a numpad)
bool EditorUI::IsNumpadPressed(int digit, bool repeat) const
{
   if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + digit), repeat))
      return true;
   return IsDigitNavKey(digit) && ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + digit), repeat);
}

void EditorUI::TogglePerspectiveCam()
{
   if (m_camMode == ViewMode::PreviewCam)
      m_perspectiveCam = true; // The preview camera is a perspective view
   else if (m_camMode == ViewMode::DesktopBackdrop)
      m_perspectiveCam = false; // The desktop backdrop is always orthographic
   m_camMode = ViewMode::EditorCam;
   m_perspectiveCam = !m_perspectiveCam;
}

// The digits are game keys (start, coin...) while the game runs and text for widgets: emulate only with the 3D view focused and the game not running (edit mode, or inspect mode while paused)
bool EditorUI::IsDigitNavKey(int digit) const
{
   const ImGuiIO &io = ImGui::GetIO();
   if (digit < 0 || digit > 9 || !m_navEmulateNumpad || (IsInspectMode() && m_player->IsGameRunning()) || io.WantTextInput || io.WantCaptureKeyboard)
      return false;
   return !io.KeyAlt;
}

namespace
{
   // The editor camera works in a right handed space where Z is the opposite of the table's one
   inline vec3 FlipZ(const vec3 &v) { return vec3(v.x, v.y, -v.z); }
   inline vec3 RotateAround(const vec3 &v, const vec3 &k, const float angle) { return v * cosf(angle) + CrossProduct(k, v) * sinf(angle) + k * (k.Dot(v) * (1.f - cosf(angle))); }
   // Table axes in the editor camera space
   inline vec3 TableAxis(const int axis) { return FlipZ(vec3(axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f, axis == 2 ? 1.f : 0.f)); }
   // Transform rotated by an angle (radians) around an axis given in the editor camera space, about its own position
   Matrix3D RotatedTransform(const Matrix3D &m0, const vec3 &k, const float angle)
   {
      Matrix3D result(m0);
      const vec3 rows[3] = { vec3(m0._11, m0._12, m0._13), vec3(m0._21, m0._22, m0._23), vec3(m0._31, m0._32, m0._33) };
      float *const out[3] = { &result._11, &result._21, &result._31 };
      for (int i = 0; i < 3; i++)
      {
         const vec3 r = FlipZ(RotateAround(FlipZ(rows[i]), k, angle));
         out[i][0] = r.x;
         out[i][1] = r.y;
         out[i][2] = r.z;
      }
      return result;
   }
   inline float WrapAngle(const float a) { return a - 2.f * static_cast<float>(M_PI) * floorf((a + static_cast<float>(M_PI)) / (2.f * static_cast<float>(M_PI))); } // To [-pi, pi)
   inline float CompatibleAngle(const float a, const float ref) { return ref + WrapAngle(a - ref); } // a + 2 pi n, nearest to ref
   // Rotation angles (degrees) of a rotation matrix in the parts' order, R = RotZ(z) * RotY(y) * RotX(x) (a Primitive's Rot X / Y / Z, see PrimitiveUIPart::GetTransform).
   // At y = +-90 (gimbal lock) the matrix only gives x + z (y = 90) or x - z (y = -90), and close to it z only up to the matrix' own precision. Of the angles which rebuild
   // the matrix, it takes the ones nearest to prev when given (Blender's compatible Euler: read back against the angles it stores, a typed angle does not move; against
   // the last frame's, a drag never jumps to the other solution (x + pi, pi - y, z + pi), though near the lock x and z turn up to 1 / cos y times as fast as the part),
   // else y within +-90 and, at the singularity, all of it on x and z 0 (Blender's convention)
   vec3 EulerFromRotation(const Matrix3D &m, const vec3 *prev)
   {
      const float cy = sqrtf(m._11 * m._11 + m._21 * m._21); // |cos y|
      const float det = m._11 * (m._22 * m._33 - m._23 * m._32) - m._12 * (m._21 * m._33 - m._23 * m._31) + m._13 * (m._21 * m._32 - m._22 * m._31);
      if (det < 0.f) // A mirror (a negative scale) is no rotation: read back as the editor always did (S X -1 on an unturned part gives Rot Z 180)
      {
         if (cy > 1e-6f)
            return vec3(-RADTOANG(atan2f(m._32, m._33)), -RADTOANG(atan2f(-m._31, cy)), -RADTOANG(atan2f(m._21, m._11)));
         return vec3(-RADTOANG(atan2f(m._23, m._22)), -RADTOANG(atan2f(-m._31, cy)), 0.f);
      }
      const float y1 = atan2f(m._31, cy);
      // t = x + k z, k the sign of sin y: defined at the singularity too, and well conditioned near it (these sums take out the factor 1 + |sin y|)
      const float k = m._31 >= 0.f ? 1.f : -1.f;
      const float t = k > 0.f ? atan2f(m._12 + m._23, m._22 - m._13) : atan2f(m._23 - m._12, m._22 + m._13);
      const float z1 = atan2f(-m._21, m._11);
      const float tol = 16.f * FLT_EPSILON / max(cy, 1e-30f); // Moving z by up to tol (x following on t) changes the matrix by ~16 epsilon
      if (prev == nullptr)
      {
         const float z = fabsf(z1) <= tol ? 0.f : z1;
         return vec3(RADTOANG(WrapAngle(t - k * z)), RADTOANG(y1), RADTOANG(z));
      }
      const vec3 p(ANGTORAD(prev->x), ANGTORAD(prev->y), ANGTORAD(prev->z));
      const float zp = p.z + k * 0.5f * WrapAngle(t - (p.x + k * p.z)); // The previous angles moved onto x + k z = t (the nearest point)
      vec3 best;
      float bestDist = FLT_MAX;
      for (int i = 0; i < 2; i++) // The two solutions: (x, y, z) and (x + pi, pi - y, z + pi)
      {
         const float y = CompatibleAngle(i == 0 ? y1 : static_cast<float>(M_PI) - y1, p.y);
         const float zr = CompatibleAngle(i == 0 ? z1 : z1 + static_cast<float>(M_PI), zp);
         const float z = clamp(zp, zr - tol, zr + tol);
         const float x = CompatibleAngle(t - k * z, p.x);
         const float dist = fabsf(x - p.x) + fabsf(y - p.y) + fabsf(z - p.z);
         if (dist < bestDist)
         {
            bestDist = dist;
            best = vec3(x, y, z);
         }
      }
      return vec3(RADTOANG(best.x), RADTOANG(best.y), RADTOANG(best.z));
   }
   // Decompose a transform into the position/scale/rotation components used by the parts' SetTransform. The rotation stays nearest to prevRot when given (see EulerFromRotation)
   void ExtractTRS(Matrix3D transform, vec3 &pos, vec3 &scale, vec3 &rot, const vec3 *prevRot = nullptr)
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
      rot = EulerFromRotation(transform, prevRot);
   }
   // The angles a part stores when it has all three (a Primitive's Rot X / Y / Z): its matrix is read back nearest to them
   bool GetPartEuler(EditorUIPart &part, vec3 &rot)
   {
      Matrix3D unused;
      constexpr int full = EditorUIPart::TM_RotX | EditorUIPart::TM_RotY;
      if ((static_cast<int>(part.GetTransform(unused)) & full) != full || part.GetEditable()->GetItemType() != eItemPrimitive)
         return false;
      const Primitive *const prim = static_cast<const Primitive *>(part.GetEditable());
      rot.Set(prim->m_d.m_aRotAndTra[0], prim->m_d.m_aRotAndTra[1], prim->m_d.m_aRotAndTra[2]);
      return true;
   }
   // The part of a rotation matrix about the table Z axis (radians): all of it for a rotation about Z, none for one about an axis in the table plane, the axis' Z share between.
   // A matrix only gives the angle up to a half turn: the same rotation is also angle + 2 pi m about the same axis, so the share is only known up to steps of 2 pi x the Z share
   // (step), and a drag picks the candidate nearest to its previous frame
   float YawOfRotation(const Matrix3D &l, float &step)
   {
      const float sx = 0.5f * (l._23 - l._32), sy = 0.5f * (l._31 - l._13), sz = 0.5f * (l._12 - l._21);
      const float s = sqrtf(sx * sx + sy * sy + sz * sz), c = 0.5f * (l._11 + l._22 + l._33 - 1.f);
      step = 2.f * static_cast<float>(M_PI);
      if (s > 1e-5f)
      {
         step *= sz / s;
         return atan2f(s, c) * sz / s;
      }
      if (c < 0.f) // A half turn: the axis comes from the diagonal
      {
         const float share = sqrtf(max(0.f, (l._33 - c) / (1.f - c)));
         step *= share;
         return static_cast<float>(M_PI) * share;
      }
      step = 0.f; // No turn, or a whole number of turns about an axis it does not give: the step is unknown
      return 0.f;
   }
   // Rotation about the table Z axis, in YawOfRotation's sense
   Matrix3D ZRotation(const float yaw)
   {
      Matrix3D result = Matrix3D::MatrixIdentity();
      result._11 = result._22 = cosf(yaw);
      result._12 = sinf(yaw);
      result._21 = -result._12;
      return result;
   }
   // Transform scaled by a factor along its own axes: all of them (axis < 0), the given one, or the two others (plane)
   Matrix3D ScaledTransform(const Matrix3D &m0, const int axis, const bool plane, const float factor)
   {
      Matrix3D result(m0);
      float *const out[3] = { &result._11, &result._21, &result._31 };
      for (int i = 0; i < 3; i++)
         if (axis < 0 || (plane ? i != axis : i == axis))
            for (int j = 0; j < 3; j++)
               out[i][j] *= factor;
      return result;
   }
}

// The view a G / R / S works in: a secondary view (its slot) under the mouse, else (over a panel) the last view the mouse was in, like the gizmo; -1 = the 3D
// Viewport; -2 = none (the mouse is over a secondary view that shows no image)
int EditorUI::TransformViewSlot() const
{
   if (m_secondaryViewHovered)
      return m_secondaryViewSlot >= 0 && m_viewInfo[m_secondaryViewSlot].valid ? m_secondaryViewSlot : -2;
   if (ImGui::GetIO().WantCaptureMouse && m_gizmoSlot >= 0 && m_viewInfo[m_gizmoSlot].valid && m_pointEditPart == nullptr && m_camMode != ViewMode::DesktopBackdrop)
      return m_gizmoSlot;
   return -1;
}

bool EditorUI::CanStartModalIn(const int viewSlot) const
{
   if (viewSlot == -2 || (viewSlot < 0 && !m_viewportVisible))
      return false;
   return viewSlot < 0 || (m_pointEditPart == nullptr && m_camMode != ViewMode::DesktopBackdrop); // Those edits are 2D in the 3D Viewport's plane (as for the gizmo)
}

void EditorUI::BeginModalTransform(ModalOp op)
{
   // G / R / S act on the 3D view under the mouse: a secondary view (its camera and image rect are used until the transform ends), else the 3D Viewport
   const int viewSlot = TransformViewSlot();
   if (!CanStartModalIn(viewSlot))
      return;
   UpdateLocalViews(); // A part just added (Duplicate) is already in the Local View
   if (!SelectionInView(viewSlot))
   {
      Report("Local View: the selection has parts this view does not show");
      return;
   }
   if (m_table->IsLocked() || m_modal.active)
      return;
   // The Playfield Maker board follows the table: G moves its selected hole instead (tested first: the board has no transform, so GetSelectionTransform fails for it)
   if (std::ranges::any_of(m_multiSel, [](const std::shared_ptr<EditorUIPart> &p) { return PlayfieldMaker::IsPart(p->GetEditable()); }))
   {
      const bool hole = op == ModalOp::Grab && m_multiSel.size() == 1;
      if (!(hole && PlayfieldMaker::BeginGrab(*this)))
         Report(hole ? "Select a hole to move it (G): the playfield follows the table"s : string(PlayfieldMaker::kFollowsTable));
      return;
   }
   Matrix3D initial;
   if (!GetSelectionTransform(initial))
      return;
   PlasticFromAtlas::AbortDrags(*this); // A pill or bow press holds points that a cancelled transform (an undo reload) would free
   if (viewSlot < 0 && m_camMode == ViewMode::PreviewCam)
      m_camMode = ViewMode::EditorCam;
   m_measure.SetActive(*this, false);
   const ImGuizmo::OPERATION savedGizmo = m_gizmoOperation;
   m_gizmoOperation = ImGuizmo::OPERATION(0);
   m_modal = ModalTransform();
   m_plasticHoleScaleInit.clear();
   m_modal.viewSlot = viewSlot;
   m_lastOp.type = LastOperation::Type::None;
   m_modal.savedGizmo = savedGizmo;
   m_modal.active = true;
   // Typed numbers: real characters on any keyboard layout (SDL sends text only while text input is on; no text field is active in a modal, so it is held open
   // here and closed when the transform ends)
   if (SDL_Window *const window = SDL_GetKeyboardFocus(); window != nullptr && !SDL_TextInputActive(window))
   {
      SDL_StartTextInput(window);
      m_modal.textWindow = window;
   }
   m_modal.op = op;
   BeginSelectionXform(m_modal.xform);
   m_modal.axisOrientation = m_transformOrientation;
   m_modal.activeAxes = GetActiveAxes();
   m_modal.viewFrame = GetViewOrientationFrame(viewSlot);
   m_modal.applied.kind = op == ModalOp::Rotate ? XformKind::Rotate : op == ModalOp::Scale ? XformKind::Scale : XformKind::Move;
   if (op == ModalOp::Rotate)
   {
      // Drag points and parts that keep no tilt can not follow an X or Y rotation: their positions still orbit the pivot, their own rotation stays (UpdateModalTransform
      // says so). What is left to do, so that the keys do not look broken: nothing can orbit (points in their plane, one part about its own origin or Individual Origins) means table Z only, and
      // with no part able to tilt the free rotation is about the table Z, not the view axis
      bool anyTilt = false;
      for (const SelectionXform::Part &part : m_modal.xform.parts)
         anyTilt |= (part.mask & (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY)) == (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY);
      // A plastic turns about the table Z only, also about the 3D cursor (it would orbit out of its plane)
      if (m_pointEditPart != nullptr || PlasticFromAtlas::ZLocked(*this) || (!anyTilt && ((m_modal.xform.parts.size() < 2 && m_transformPivot != TransformPivot::Cursor) || m_modal.xform.individual)))
      {
         m_modal.axis = 2;
         m_modal.zOnly = true;
         m_modal.axisOrientation = TransformOrientation::Global; // The table Z, whatever the orientation
      }
      m_modal.freeZ = !anyTilt;
   }
   m_modal.initial = initial;
   m_modal.startMouse = ImGui::GetMousePos();
   // Same undo record as a gizmo drag: one entry for the whole operation
   if (m_table->m_liveBaseTable == nullptr)
   {
      m_undo.BeginUndo();
      for (const auto &part : m_multiSel)
         m_undo.MarkForUndo(part->GetEditable());
      MarkDependentsForUndo();
      m_undo.SetLabel(CountLabel(op == ModalOp::Rotate ? "Rotate"
            : op == ModalOp::Scale                     ? "Scale"
                                                       : "Move",
         m_multiSel.size(), m_multiSel.empty() ? string() : m_multiSel.front()->GetEditable()->GetName()));
      m_undo.EndUndo();
      m_modal.hasUndo = true;
   }
   m_lastUndoPart = nullptr;
   m_lastUndoId = 0;
   UpdateModalTransform(); // Initialize the start angle/distance and the status text
}

void EditorUI::EndModalTransform(const bool confirm)
{
   if (!m_modal.active)
      return;
   // A confirmed transform that changed nothing (typed 0, the mouse did not move) leaves no undo step: it is dropped as a cancel (a duplicate is a change). Judged on
   // the operation's own parameters: the selection transform does not always carry the edit (drag points, translation only parts) and its precision shrinks far from the origin
   const bool unchanged = confirm && !m_modal.duplicate
      && (m_modal.op == ModalOp::Grab ? fabsf(m_modal.lastDelta.x) + fabsf(m_modal.lastDelta.y) + fabsf(m_modal.lastDelta.z) < 1e-5f
         : m_modal.op == ModalOp::Rotate ? fabsf(m_modal.lastAngle) < 1e-4f
         : fabsf(m_modal.lastFactor - 1.f) < 1e-5f);
   if (!confirm || unchanged)
   {
      if (m_modal.hasUndo)
         RestoreUndoState(m_undo.Undo(false)); // Canceled: nothing to redo
      else
      {
         XformOp restore;
         restore.kind = m_modal.applied.kind;
         ApplySelectionXform(m_modal.xform, restore);
      }
   }
   m_modal.active = false;
   FlushRubberDrag(); // After the restore of a canceled drag
   if (m_modal.textWindow != nullptr)
   {
      SDL_StopTextInput(m_modal.textWindow);
      m_modal.textWindow = nullptr;
   }
   FlushDrag(); // After the restore of a canceled drag
   m_gizmoOperation = m_modal.savedGizmo; // The gizmo comes back after the transform
   m_lastUndoPart = nullptr;
   m_lastUndoId = 0;

   // The confirmed operation can be adjusted (Adjust Last Operation)
   Matrix3D current;
   if (confirm && !unchanged && m_modal.hasUndo && !m_pointEditPart && !m_multiSel.empty() && GetSelectionTransform(current))
   {
      if (m_modal.duplicate && m_undo.MergeLastIntoPrevious()) // One undo step for the duplicate and its grab
      {
         SetLastOperation(LastOperation::Type::Duplicate, current);
         m_lastOp.base = m_modal.duplicateOrigin;
         m_lastOp.offset = vec3(current._41 - m_lastOp.base.x, current._42 - m_lastOp.base.y, current._43 - m_lastOp.base.z);
      }
      else
      {
         switch (m_modal.op)
         {
         case ModalOp::Grab:
            SetLastOperation(LastOperation::Type::Move, m_modal.initial);
            m_lastOp.offset = m_modal.lastDelta;
            m_lastOp.ownFrame = m_modal.lastOwnFrame;
            m_lastOp.xform = m_modal.xform;
            break;
         case ModalOp::Rotate:
            SetLastOperation(LastOperation::Type::Rotate, m_modal.initial);
            m_lastOp.axis = m_modal.axis < 0 && m_modal.freeZ ? 2 : m_modal.axis; // A free rotation that went about the table Z (no part can tilt) is shown as Z
            m_lastOp.axisView = m_modal.lastAxis;
            m_lastOp.angle = m_modal.lastAngle;
            m_lastOp.orientation = m_modal.axis < 0 ? TransformOrientation::Global : m_modal.axisOrientation;
            m_lastOp.viewFrame = m_modal.viewFrame;
            m_lastOp.xform = m_modal.xform;
            break;
         case ModalOp::Scale:
            SetLastOperation(LastOperation::Type::Scale, m_modal.initial);
            m_lastOp.axis = m_modal.axis;
            m_lastOp.plane = m_modal.plane;
            m_lastOp.factor = m_modal.lastFactor;
            m_lastOp.orientation = m_modal.axisOrientation;
            m_lastOp.viewFrame = m_modal.viewFrame;
            m_lastOp.xform = m_modal.xform;
            break;
         }
      }
   }
   m_modal.xform = SelectionXform();
}

void EditorUI::UpdateModalTransform()
{
   const ImGuiIO &io = ImGui::GetIO();
   const bool first = m_modal.hud.empty();
   Matrix3D current;
   if (!GetSelectionTransform(current) || m_table->IsLocked())
   {
      EndModalTransform(false);
      return;
   }
   if (!first)
   {
      if (ImGui::IsKeyReleased(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
      {
         EndModalTransform(false);
         return;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      {
         EndModalTransform(true);
         return;
      }
      // Axis constraint (pressing the same axis again removes it), Shift constrains to the plane instead. Not with Ctrl / Alt / Cmd held (Ctrl+Z is not Z)
      const bool chord = io.KeyCtrl || io.KeyAlt || io.KeySuper;
      const bool zLocked = PlasticFromAtlas::ZLocked(*this); // A plastic is edited in X and Y only
      const std::pair<ImGuiKey, int> axisKeys[] = { { ImGuiKey_X, 0 }, { ImGuiKey_Y, 1 }, { ImGuiKey_Z, 2 } };
      for (const auto &[key, axis] : axisKeys)
         if (!chord && !m_modal.zOnly && !(axis == 2 && zLocked) && ImGui::IsKeyPressed(key, false))
         {
            // Blender's cycle: the first press uses the scene's orientation, the second the other one of Global / Local (none when they are the same axes here,
            // as in point edit), the third removes the constraint
            const auto axesOf = [this](const TransformOrientation o)
            { return IsOwnFrameOrientation(o) ? TransformOrientation::Local : o == TransformOrientation::View ? o : TransformOrientation::Global; };
            const TransformOrientation alternate = m_transformOrientation == TransformOrientation::Global ? TransformOrientation::Local : TransformOrientation::Global;
            const bool same = m_modal.axis == axis && m_modal.plane == io.KeyShift;
            if (same && m_modal.axisOrientation == m_transformOrientation && axesOf(alternate) != axesOf(m_transformOrientation))
               m_modal.axisOrientation = alternate;
            else
            {
               m_modal.axis = same ? -1 : axis;
               m_modal.plane = !same && io.KeyShift;
               m_modal.axisOrientation = m_transformOrientation;
            }
         }
      // Numeric input. The keys are read directly (the digit row and the keypad, the minus and the period): SDL sends no text input here, as no text field is
      // active (the backend starts it only for those). Characters are used when the platform does send them (a layout whose digits are not on the digit keys)
      const auto typeChar = [this](const char c)
      {
         if (c == '-')
         {
            if (!m_modal.typed.empty() && m_modal.typed[0] == '-')
               m_modal.typed.erase(0, 1);
            else
               m_modal.typed.insert(0, "-");
         }
         else if (c != '.' || m_modal.typed.find('.') == string::npos)
            m_modal.typed += c;
      };
      if (io.InputQueueCharacters.Size > 0 || m_modal.textWindow != nullptr) // Text input is on: characters only (they follow the layout and the press order)
      {
         for (int i = 0; i < io.InputQueueCharacters.Size; i++)
         {
            const ImWchar c = io.InputQueueCharacters[i];
            if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-')
               typeChar(c == ',' ? '.' : static_cast<char>(c)); // A decimal comma (some locales' keypad)
         }
      }
      else if (!chord && !io.KeyShift)
      {
         for (int digit = 0; digit <= 9; digit++)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + digit), false) || ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + digit), false))
               typeChar(static_cast<char>('0' + digit));
         if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))
            typeChar('-');
         if (ImGui::IsKeyPressed(ImGuiKey_Period, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false))
            typeChar('.');
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !m_modal.typed.empty())
         m_modal.typed.pop_back();
   }
   const bool hasValue = !m_modal.typed.empty() && m_modal.typed != "-" && m_modal.typed != ".";
   const float value = hasValue ? static_cast<float>(atof(m_modal.typed.c_str())) : 0.f;

   // Camera basis & projection (in the camera's space): the 3D Viewport's (the whole window), or the view the transform started in (its last render's
   // camera, in its image rect: the editor camera is view = RH2LH * m_camView * YAxis, see RenderUI)
   Matrix3D view(m_camView);
   bool perspective = m_perspectiveCam && m_camMode != ViewMode::DesktopBackdrop;
   float tanHalfFov = tanf(0.5f * ANGTORAD(m_navFovY)), orthoHalfH = m_camDistance; // Half view height per unit of depth (perspective) / at any depth (orthographic)
   ImVec2 viewOrg(0.f, 0.f), viewSize = io.DisplaySize;
   if (m_modal.viewSlot >= 0)
   {
      const SecondaryViewInfo &info = m_viewInfo[m_modal.viewSlot];
      if (!info.valid) // The view is not shown anymore
      {
         EndModalTransform(false);
         return;
      }
      const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f), YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
      view = RH2LH * info.view * YAxis;
      perspective = fabsf(info.proj.m[2][3]) > 1e-6f;
      tanHalfFov = orthoHalfH = 1.f / max(fabsf(info.proj.m[1][1]), 1e-6f);
      viewOrg = info.rectMin;
      viewSize = info.rectSize;
   }
   view.Invert();
   const vec3 right = view.GetOrthoNormalRight(), up = view.GetOrthoNormalUp(), dir = view.GetOrthoNormalDir(), eye = view.GetOrthoNormalPos();
   const Matrix3D &m0 = m_modal.initial;
   const vec3 pivot = FlipZ(m_modal.xform.pivot);
   const float aspect = viewSize.x / viewSize.y;
   // Camera space point (right handed, see FlipZ) to the screen. The 3D Viewport's camera is a plain look at; a secondary view's can be any view setup of the table
   // (the player's: layback, off axis), so it projects with the model view projection its image was rendered with
   const auto toScreen = [&](const vec3 &p) -> ImVec2
   {
      if (m_modal.viewSlot >= 0)
      {
         const Matrix3D &mvp = m_viewInfo[m_modal.viewSlot].mvp;
         const vec3 v = FlipZ(p);
         const float xp = mvp._11 * v.x + mvp._21 * v.y + mvp._31 * v.z + mvp._41;
         const float yp = mvp._12 * v.x + mvp._22 * v.y + mvp._32 * v.z + mvp._42;
         const float wp = mvp._14 * v.x + mvp._24 * v.y + mvp._34 * v.z + mvp._44;
         if (wp <= 1e-10f) // Behind the camera
            return ImVec2(FLT_MAX, FLT_MAX);
         return ImVec2(viewOrg.x + (wp + xp) * viewSize.x * 0.5f / wp, viewOrg.y + (wp - yp) * viewSize.y * 0.5f / wp);
      }
      const float d = max((p - eye).Dot(dir), 1e-3f);
      const float h = perspective ? d * tanHalfFov : orthoHalfH;
      return ImVec2(viewOrg.x + 0.5f * viewSize.x * (1.f + (p - eye).Dot(right) / (h * aspect)), viewOrg.y + 0.5f * viewSize.y * (1.f - (p - eye).Dot(up) / h));
   };
   const ImVec2 screenPivot = toScreen(pivot);
   const ImVec2 mouse = ImGui::GetMousePos();
   const float curAngle = atan2f(mouse.y - screenPivot.y, mouse.x - screenPivot.x);
   const float curDist = hypotf(mouse.x - screenPivot.x, mouse.y - screenPivot.y);
   if (first)
   {
      m_modal.startAngle = curAngle;
      m_modal.startDist = max(curDist, 8.f);
   }

   XformOp xformOp;
   xformOp.kind = m_modal.applied.kind;
   string partNote; // Parts that can not follow, said plainly
   const char *axisNames[] = { "X", "Y", "Z" };
   // The constraint's axes: the frame's rows (table coordinates; Global = the table's), or each part's own (Local / Normal, the active part's following the mouse)
   const bool ownAxes = m_modal.axis >= 0 && IsOwnFrameOrientation(m_modal.axisOrientation);
   const bool viewAxes = m_modal.axis >= 0 && m_modal.axisOrientation == TransformOrientation::View;
   const Matrix3D &frame = viewAxes ? m_modal.viewFrame : ownAxes ? m_modal.activeAxes : Matrix3D::MatrixIdentity();
   const auto frameAxis = [&frame](const int a) { return FlipZ(vec3(frame.m[a][0], frame.m[a][1], frame.m[a][2])); }; // In the camera space; Global: TableAxis
   // As Blender's header says it: "along global X", "locking local Z" (Shift: the plane perpendicular to it)
   static constexpr const char *orientationNames[] = { "global", "local", "normal", "view" };
   const char *const orientationName = m_pointEditPart != nullptr && m_modal.axisOrientation != TransformOrientation::View ? "global" : orientationNames[static_cast<int>(m_modal.axisOrientation)];
   const string axisText = m_modal.axis < 0 ? (m_modal.freeZ && m_modal.op == ModalOp::Rotate ? " along global Z" : "")
                                            : string(m_modal.plane ? " locking " : " along ") + orientationName + ' ' + axisNames[m_modal.axis];
   char text[128];
   const char *snapText = "";
   switch (m_modal.op)
   {
   case ModalOp::Grab:
   {
      const float dx = mouse.x - m_modal.startMouse.x, dy = mouse.y - m_modal.startMouse.y;
      vec3 move(0.f, 0.f, 0.f);
      if (m_modal.viewSlot < 0)
      {
         const float depth = max((eye - pivot).Dot(dir), 1e-3f);
         const float unitsPerPixel = 2.f * (perspective ? depth * tanHalfFov : orthoHalfH) / viewSize.y;
         move = right * (dx * unitsPerPixel) - up * (dy * unitsPerPixel);
         if (float planeZ; PlasticFromAtlas::TopPlaneZ(*this, planeZ)) // A plastic follows the cursor on the plane of its top, as a drag does
         {
            const Vertex2D now = UnprojectToPlane(mouse, planeZ), start = UnprojectToPlane(m_modal.startMouse, planeZ);
            move = vec3(now.x - start.x, now.y - start.y, 0.f);
         }
      }
      else
      {
         // In the plane through the pivot facing the camera: the screen offsets of a step along its two axes give the move that follows the mouse
         constexpr float step = 10.f;
         const ImVec2 s0 = toScreen(pivot), sr = toScreen(pivot + right * step), su = toScreen(pivot + up * step);
         const float ax = (sr.x - s0.x) / step, ay = (sr.y - s0.y) / step, bx = (su.x - s0.x) / step, by = (su.y - s0.y) / step; // Pixels per unit along right / up
         const float det = ax * by - ay * bx;
         if (fabsf(det) > 1e-9f && fabsf(det) < 1e18f)
            move = right * ((dx * by - dy * bx) / det) + up * ((ax * dy - ay * dx) / det);
      }
      if (hasValue)
         move = m_modal.axis < 0 ? TableAxis(0) * value : frameAxis(m_modal.axis) * value; // No axis: along the table X, whatever the orientation
      else if (m_modal.axis >= 0)
      {
         const vec3 a = frameAxis(m_modal.axis);
         move = m_modal.plane ? move - a * move.Dot(a) : a * move.Dot(a);
      }
      vec3 delta = FlipZ(move);
      if (PlasticFromAtlas::ZLocked(*this))
         delta.z = 0.f; // Z never changes on a plastic
      if (!hasValue && IsSnapActive(SnapKind::Move))
      {
         bool snappedToPoint = false;
         if (m_snapTarget == 1 && m_modal.axis < 0)
         {
            // Snap to the center / drag points of the unselected parts: the closest one to the moved pivot on screen (X / Y only, Z is kept)
            if (!m_modal.snapPointsBuilt)
            {
               m_modal.snapPointsBuilt = true;
               m_modal.snapPoints.clear();
               for (const auto &uiPart : m_editables)
               {
                  IEditable *const editable = uiPart->GetEditable();
                  if (editable == nullptr || editable->GetItemType() == eItemPartGroup || editable->GetItemType() == eItemBall || IsPartSelected(uiPart) || !editable->IsUIVisible(false) || PlasticFromAtlas::IsCompanion(editable))
                     continue;
                  m_modal.snapPoints.push_back(editable->GetCenter());
                  if (DragPointCurve *const curve = uiPart->GetDragPointCurve())
                     for (const auto &point : curve->GetPoints())
                        m_modal.snapPoints.emplace_back(point->GetX(), point->GetY());
                  if (m_modal.snapPoints.size() > 20000)
                     break;
               }
            }
            const ImVec2 moved = toScreen(FlipZ(vec3(m0._41 + delta.x, m0._42 + delta.y, m0._43 + delta.z)));
            const float maxDist = 14.f * m_liveUI.GetDPI();
            float best = maxDist;
            for (const Vertex2D &candidate : m_modal.snapPoints)
            {
               const ImVec2 screen = toScreen(FlipZ(vec3(candidate.x, candidate.y, m0._43 + delta.z)));
               const float dist = hypotf(screen.x - moved.x, screen.y - moved.y);
               if (dist < best)
               {
                  best = dist;
                  delta.x = candidate.x - m0._41;
                  delta.y = candidate.y - m0._42;
                  snappedToPoint = true;
               }
            }
            if (snappedToPoint)
            {
               const ImVec2 marker = toScreen(FlipZ(vec3(m0._41 + delta.x, m0._42 + delta.y, m0._43 + delta.z)));
               ImGui::GetForegroundDrawList()->AddCircle(marker, 7.f * m_liveUI.GetDPI(), EditorTheme::Accent, 16, 2.f);
            }
         }
         if (!snappedToPoint && m_snapMove > 0.f && (ownAxes || viewAxes))
         {
            // Increments along the constraint's axes
            vec3 u(delta.Dot(vec3(frame._11, frame._12, frame._13)), delta.Dot(vec3(frame._21, frame._22, frame._23)), delta.Dot(vec3(frame._31, frame._32, frame._33)));
            u.Set(roundf(u.x / m_snapMove) * m_snapMove, roundf(u.y / m_snapMove) * m_snapMove, roundf(u.z / m_snapMove) * m_snapMove);
            delta = frame.MultiplyVectorNoTranslate(u);
         }
         else if (!snappedToPoint && m_snapMove > 0.f)
         {
            delta.x = roundf(delta.x / m_snapMove) * m_snapMove;
            delta.y = roundf(delta.y / m_snapMove) * m_snapMove;
            delta.z = roundf(delta.z / m_snapMove) * m_snapMove;
         }
         snapText = "  [snap]";
         if (PlasticFromAtlas::ZLocked(*this))
            delta.z = 0.f; // Increments along tilted (View) axes can bring Z back
      }
      if (ownAxes)
      {
         // Local: the move along the active part's axis, in its axes, done by each part along its own (Blender moves each object along its own X)
         delta = vec3(delta.Dot(vec3(frame._11, frame._12, frame._13)), delta.Dot(vec3(frame._21, frame._22, frame._23)), delta.Dot(vec3(frame._31, frame._32, frame._33)));
         xformOp.ownFrame = true;
      }
      xformOp.move = delta;
      m_modal.lastDelta = delta;
      m_modal.lastOwnFrame = ownAxes;
      snprintf(text, sizeof(text), "Move%s  %.2f  %.2f  %.2f vpu%s%s%s", axisText.c_str(), delta.x, delta.y, delta.z, snapText, hasValue ? "   [" : "",
         hasValue ? (m_modal.typed + "]").c_str() : "");
      break;
   }
   case ModalOp::Rotate:
   {
      const vec3 k = m_modal.axis >= 0 ? frameAxis(m_modal.axis) : m_modal.freeZ ? TableAxis(2) : dir; // Local: the active part's axis (the mouse turns about it)
      const float facing = k.Dot(dir) >= 0.f ? 1.f : -1.f;
      float angle = hasValue ? ANGTORAD(clamp(value, -kMaxLastOpAngle, kMaxLastOpAngle)) : -(curAngle - m_modal.startAngle) * facing; // Typed: what F9's Angle field holds
      if (!hasValue && IsSnapActive(SnapKind::Rotate) && m_snapRotate > 0.f)
      {
         angle = ANGTORAD(roundf(RADTOANG(angle) / m_snapRotate) * m_snapRotate);
         snapText = "  [snap]";
      }
      xformOp = OrientedXform(XformKind::Rotate, m_modal.axisOrientation, m_modal.viewFrame, m_modal.axis, false, angle, k, m_modal.xform.parts.size());
      m_modal.lastAxis = k;
      m_modal.lastAngle = RADTOANG(angle);
      snprintf(text, sizeof(text), "Rotate%s  %.1f deg%s%s", axisText.c_str(), RADTOANG(angle), snapText, hasValue ? "   [typed]" : "");
      if (fabsf(angle) > 1e-5f)
      {
         // Parts with only a Z rotation take the axis' table Z share of the turn: all of it about Z, none about an axis in the table plane
         int stay = 0, zOnly = 0;
         for (const SelectionXform::Part &part : m_modal.xform.parts)
         {
            const vec3 kp = ownAxes ? FlipZ(vec3(part.axes.m[m_modal.axis][0], part.axes.m[m_modal.axis][1], part.axes.m[m_modal.axis][2])) : k; // Local: the part's own axis
            const bool aboutZ = fabsf(kp.z) > 0.9999f, noZ = fabsf(kp.z) < 1e-4f;
            if ((part.mask & EditorUIPart::TM_RotAny) == 0)
               stay++;
            else if (!aboutZ && (part.mask & (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY)) != (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY))
               (noZ ? stay : zOnly)++;
         }
         const size_t count = m_modal.xform.parts.size();
         if (stay > 0)
            partNote = count == 1 ? "   (this part keeps its rotation)" : "   (" + std::to_string(stay) + " of " + std::to_string(count) + " keep their rotation)";
         if (zOnly > 0)
            partNote += count == 1 ? "   (this part only turns about Z)" : "   (" + std::to_string(zOnly) + " of " + std::to_string(count) + " only turn about Z)";
      }
      break;
   }
   case ModalOp::Scale:
   {
      float factor = hasValue ? value : curDist / m_modal.startDist;
      if (fabsf(factor) < 1e-3f)
         factor = hasValue ? 1.f : copysignf(1e-3f, factor); // Never singular (the next frame inverts it); "0" on the way to "0.5" holds 1: a zero scale would collapse curves and drag points for good
      if (!hasValue && IsSnapActive(SnapKind::Scale) && m_snapScale > 0.f)
      {
         factor = max(roundf(factor / m_snapScale) * m_snapScale, m_snapScale);
         snapText = "  [snap]";
      }
      xformOp = OrientedXform(XformKind::Scale, m_modal.axisOrientation, m_modal.viewFrame, m_modal.axis, m_modal.plane, factor, vec3(0.f, 0.f, 0.f), m_modal.xform.parts.size());
      m_modal.lastFactor = factor;
      snprintf(text, sizeof(text), "%s%s  %.3f%s%s", PlasticFromAtlas::IsEditingHoles(*this) ? "Scale diameter" : "Scale", axisText.c_str(), factor, snapText, hasValue ? "   [typed]" : "");
      if (fabsf(factor - 1.f) > 1e-5f)
      {
         // The scaled axes: all (a uniform scale also reaches the parts that only scale as a whole), the given one, or the two others (plane)
         int scaled = m_modal.axis < 0 ? EditorUIPart::TM_ScaleAll : 0;
         for (int i = 0; i < 3; i++)
            if (m_modal.axis < 0 || (m_modal.plane ? i != m_modal.axis : i == m_modal.axis))
               scaled |= EditorUIPart::TM_ScaleX << i;
         int stay = 0;
         for (const SelectionXform::Part &part : m_modal.xform.parts)
         {
            int partScaled = scaled;
            if (m_modal.axis >= 0 && m_modal.axisOrientation == TransformOrientation::View)
            {
               // Along the screen's axes: the part's own axes whose length the scale changes
               partScaled = 0;
               for (int i = 0; i < 3; i++)
                  if (fabsf(vec3(xformOp.linear.MultiplyVectorNoTranslate(vec3(part.axes.m[i][0], part.axes.m[i][1], part.axes.m[i][2]))).Length() - 1.f) > 1e-4f)
                     partScaled |= EditorUIPart::TM_ScaleX << i;
            }
            stay += (part.mask & partScaled) == 0;
         }
         if (stay > 0)
            partNote = m_modal.xform.parts.size() == 1 ? "   (this part keeps its size)" : "   (" + std::to_string(stay) + " of " + std::to_string(m_modal.xform.parts.size()) + " keep their size)";
      }
      break;
   }
   }
   m_modal.hud = text + partNote;
   if (!(xformOp == m_modal.applied)) // Nothing to apply while the mouse rests
   {
      ApplySelectionXform(m_modal.xform, xformOp);
      m_modal.applied = xformOp;
   }
}

// Cycle the camera: preview camera, editor camera, desktop backdrop
void EditorUI::CycleCamera()
{
   m_predefinedView = PredefinedView::None;
   switch (m_camMode)
   {
   case ViewMode::PreviewCam:
      m_camMode = ViewMode::EditorCam;
      ResetCameraFromPlayer();
      break;
   case ViewMode::EditorCam:
      m_camMode = ViewMode::DesktopBackdrop;
      SetBackdropCamera();
      break;
   case ViewMode::DesktopBackdrop: m_camMode = ViewMode::PreviewCam; break;
   }
}

// Editor Camera center on the whole selection, adjusting the zoom so that it fills the view
void EditorUI::FrameSelection()
{
   FRect3D bounds;
   if (GetSelectionBounds(bounds, &m_localView))
      FrameBounds(bounds);
}

void EditorUI::EndLocalViews()
{
   if (m_localView.active)
      ToggleLocalView(); // Gives the 3D Viewport's camera back
   for (CameraPreviewPanel &panel : m_cameraPreviews)
      panel.ExitLocalView();
   m_localKnown.clear();
   m_localHiddenLost.clear();
}

void EditorUI::ToggleLocalView()
{
   if (m_localView.active)
   {
      m_camMode = m_localViewCamera.camMode;
      m_perspectiveCam = m_localViewCamera.perspective;
      m_predefinedView = m_localViewCamera.predefined;
      m_camView = m_localViewCamera.view;
      m_camDistance = m_localViewCamera.distance;
      m_localView.Clear();
      m_renderer->SetLocalView(nullptr);
      Report("Local View off");
      return;
   }
   if (IsInspectMode() && m_player->IsGameRunning())
      return; // The keys are the game's while it runs
   if (!BuildLocalViewParts(m_localView))
   {
      Report("Local View: nothing selected");
      return;
   }
   m_localView.active = true;
   m_localViewCamera = { m_camMode, m_perspectiveCam, m_predefinedView, m_camView, m_camDistance };
   m_renderer->SetLocalView(m_localView.ForRenderer());
   if (FRect3D bounds; GetLocalViewBounds(m_localView, bounds)) // The set's, so that a part group alone (no bounds of its own) frames its parts
      FrameBounds(bounds);
   Report("Local View");
}

bool EditorUI::BuildLocalViewParts(LocalViewParts &parts) const
{
   parts.Clear();
   const auto add = [&parts](const IEditable *editable)
   {
      if (parts.set.insert(editable).second)
         parts.names[editable] = editable->GetName();
   };
   for (const auto &part : m_multiSel)
   {
      const IEditable *const editable = part->GetEditable();
      if (editable == nullptr)
         continue;
      add(editable);
      if (editable->GetItemType() == eItemPartGroup) // A part group stands for its parts
         for (const auto &other : m_editables)
            if (other->GetEditable() != nullptr)
               for (const PartGroup *group = other->GetEditable()->GetPartGroup(); group != nullptr; group = group->GetPartGroup())
                  if (static_cast<const IEditable *>(group) == editable)
                  {
                     add(other->GetEditable());
                     break;
                  }
   }
   // A plastic is shown whole (its clear and bevel parts), a Wire Rail with its channel part: the selection never holds those
   vector<IEditable *> withCompanions;
   for (const IEditable *const editable : parts.set)
      withCompanions.push_back(const_cast<IEditable *>(editable));
   PlasticFromAtlas::AddCompanions(withCompanions);
   for (const IEditable *const editable : withCompanions)
      add(editable);
   return !parts.set.empty();
}

bool EditorUI::GetLocalViewBounds(const LocalViewParts &parts, FRect3D &bounds) const
{
   bounds.Clear();
   for (const auto &uiPart : m_editables)
      if (parts.Allows(uiPart->GetEditable()))
         ExtendBoundsWithPart(bounds, uiPart);
   return bounds.left <= bounds.right;
}

const LocalViewParts &EditorUI::ViewLocalView(const int slot) const
{
   return slot >= 0 && slot < AreaLayout::kMaxViewLeaves ? m_cameraPreviews[slot].GetLocalView() : m_localView;
}

const LocalViewParts *EditorUI::KeyLocalView() const
{
   return &ViewLocalView(m_secondaryViewHovered ? m_secondaryViewSlot : -1);
}

bool EditorUI::SelectionInView(const int slot) const
{
   const LocalViewParts &local = ViewLocalView(slot);
   if (!local.active)
      return true;
   for (const auto &part : m_multiSel)
      if (!local.Allows(part->GetEditable()))
         return false;
   return m_selection.GetType() != Selection::S_EDITABLE || m_selection.GetPart() == nullptr || local.Allows(m_selection.GetPart()->GetEditable());
}

void EditorUI::UpdateLocalViews()
{
   LocalViewParts *views[1 + AreaLayout::kMaxViewLeaves];
   int n = 0;
   if (m_localView.active)
      views[n++] = &m_localView;
   for (CameraPreviewPanel &panel : m_cameraPreviews)
      if (panel.GetLocalView().active)
         views[n++] = &panel.GetLocalView();
   if (n == 0)
   {
      m_localKnown.clear();
      m_localHiddenLost.clear();
      m_localUndoRan = false;
      return; // Nothing to do when no view is local
   }
   if (IsInspectMode() && m_player->IsGameRunning())
   {
      EndLocalViews(); // The game runs again (resumed from a paused inspection): it is shown whole, and the keys are its own
      return;
   }
   // A part which left the table (deleted) drops out of the views, by address: it is not dereferenced; one an undo returns (as another object) is taken back by name
   const ankerl::unordered_dense::set<const IEditable *> liveParts(m_table->GetParts().begin(), m_table->GetParts().end());
   const bool firstFrame = m_localKnown.empty(); // The parts present when the first view went local are not new
   // A part hidden by every view which leaves the table: when an undo returns it, it stays hidden
   for (auto it = m_localKnown.begin(); it != m_localKnown.end();)
   {
      if (liveParts.contains(it->first))
      {
         ++it;
         continue;
      }
      const IEditable *const gonePart = it->first;
      if (std::ranges::none_of(std::span(views, static_cast<size_t>(n)), [gonePart](const LocalViewParts *view) { return view->set.contains(gonePart); }))
         m_localHiddenLost.push_back(it->second);
      it = m_localKnown.erase(it);
   }
   bool changed[1 + AreaLayout::kMaxViewLeaves] = {};
   for (int i = 0; i < n; i++)
   {
      LocalViewParts &view = *views[i];
      vector<const IEditable *> gone;
      for (const IEditable *const part : view.set)
         if (!liveParts.contains(part))
            gone.push_back(part);
      for (const IEditable *const part : gone)
      {
         view.lost.push_back(view.names[part]);
         view.names.erase(part);
         view.set.erase(part);
         changed[i] = true;
      }
      for (auto &[part, name] : view.names)
         if (const string &current = part->GetName(); current != name)
            name = current;
      for (size_t k = 0; k < view.lost.size();)
      {
         const auto it = std::ranges::find_if(m_table->GetParts(), [&](const IEditable *part) { return part->GetItemType() != eItemPartGroup && part->GetName() == view.lost[k] && !view.set.contains(part); });
         if (it == m_table->GetParts().end())
            k++;
         else
         {
            view.set.insert(*it);
            view.names[*it] = view.lost[k];
            view.lost.erase(view.lost.begin() + static_cast<ptrdiff_t>(k));
            changed[i] = true;
         }
      }
   }
   // A part new to the table (Duplicate, Paste, Add Part, a library placement, a redo) joins every Local View, so that it is drawn and can be clicked, as in Blender
   for (const IEditable *const part : m_table->GetParts())
   {
      const auto known = m_localKnown.find(part);
      if (known != m_localKnown.end())
      {
         if (known->second != part->GetName())
            known->second = part->GetName();
         continue;
      }
      m_localKnown[part] = part->GetName();
      if (firstFrame)
         continue;
      if (std::ranges::any_of(std::span(views, static_cast<size_t>(n)), [part](const LocalViewParts *view) { return view->set.contains(part); }))
         continue; // Taken back by name above
      if (const auto hidden = std::ranges::find(m_localHiddenLost, part->GetName()); hidden != m_localHiddenLost.end())
      {
         m_localHiddenLost.erase(hidden); // Whatever it is, the name is not a deleted part's any more
         if (m_localUndoRan)
            continue; // Returned by an undo, and it was not in the views (without an undo it is a new part which took the free name: it joins)
      }
      for (int i = 0; i < n; i++)
      {
         views[i]->set.insert(part);
         views[i]->names[part] = part->GetName();
         changed[i] = true;
      }
   }
   m_localUndoRan = false;
   for (int i = 0; i < n; i++)
   {
      if (changed[i])
         for (CameraPreviewPanel &panel : m_cameraPreviews)
            if (&panel.GetLocalView() == views[i])
               panel.LocalViewChanged();
      if (views[i]->set.empty()) // Every part it showed was deleted: nothing is left to show, it ends (the 3D Viewport's camera comes back)
      {
         if (views[i] == &m_localView)
            ToggleLocalView();
         else
            for (CameraPreviewPanel &panel : m_cameraPreviews)
               if (&panel.GetLocalView() == views[i])
                  panel.ExitLocalView();
      }
   }
}

void EditorUI::FrameAll()
{
   FRect3D bounds(0.f, m_table->m_right, 0.f, m_table->m_bottom, 0.f, m_table->m_glassTopHeight);
   if (FRect3D shown; m_localView.active && GetLocalViewBounds(m_localView, shown))
      bounds = shown; // What the Local View shows (the table's box if that has no size)
   FrameBounds(bounds);
}

// Editor Camera to an axis aligned view, keeping the orbit target and distance
// Change the vertical field of view keeping the framing of the orbit target (the distance is adjusted so the view does not jump)
// 3D point (camera space) under the cursor: the front-most pickable part hit by the mouse ray, else the playfield plane
bool EditorUI::PickSurfacePoint(const ImVec2 &mousePos, vec3 &point, const PickView *view) const
{
   Vertex3Ds a, b;
   PickRay(mousePos, view, a, b);
   float t = -1.f;
   vector<HitTestResult> hits;
   RayCastParts(mousePos, hits, view);
   if (!hits.empty())
      t = hits.front().m_time; // Sorted by distance from the viewer
   else if (fabsf(b.z - a.z) > 1e-10f)
      t = (0.f - a.z) / (b.z - a.z);
   if (!(t >= 0.f && t <= 1.f))
      return false;
   point = vec3(a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z));
   return true;
}

bool EditorUI::PickDepthPoint(const ImVec2 &mousePos, vec3 &point, const PickView *view) const
{
   vec3 surface;
   if (!PickSurfacePoint(mousePos, surface, view))
      return false;
   point = FlipZ(surface);
   return true;
}

void EditorUI::SetFov(const float fovY)
{
   const float newFov = clamp(fovY, 15.f, 100.f);
   if (m_perspectiveCam && m_camMode == ViewMode::EditorCam)
   {
      Matrix3D inv(m_camView);
      inv.Invert();
      const vec3 dir = inv.GetOrthoNormalDir(), up = inv.GetOrthoNormalUp();
      const vec3 camTarget = inv.GetOrthoNormalPos() - dir * m_camDistance;
      m_camDistance *= tanf(0.5f * ANGTORAD(m_navFovY)) / tanf(0.5f * ANGTORAD(newFov));
      m_camView = Matrix3D::MatrixLookAtRH(camTarget + dir * m_camDistance, camTarget, up);
   }
   m_navFovY = newFov;
}

void EditorUI::SetView(const PredefinedView view)
{
   m_predefinedView = view;
   m_camMode = ViewMode::EditorCam;
   Matrix3D inv(m_camView);
   inv.Invert();
   const vec3 camTarget = inv.GetOrthoNormalPos() - inv.GetOrthoNormalDir() * m_camDistance;
   vec3 newUp { 0.f, 0.f, -1.f }, newDir;
   switch (view)
   {
   case PredefinedView::Top:
      newUp = vec3(0.f, -1.f, 0.f);
      newDir = vec3(0.f, 0.f, -1.f);
      break;
   case PredefinedView::Bottom:
      newUp = vec3(0.f, -1.f, 0.f);
      newDir = vec3(0.f, 0.f, 1.f);
      break;
   case PredefinedView::Front: newDir = vec3(0.f, 1.f, 0.f); break;
   case PredefinedView::Back: newDir = vec3(0.f, -1.f, 0.f); break;
   case PredefinedView::Left: newDir = vec3(-1.f, 0.f, 0.f); break;
   case PredefinedView::Right: newDir = vec3(1.f, 0.f, 0.f); break;
   default: return;
   }
   m_camView = Matrix3D::MatrixLookAtRH(camTarget + newDir * m_camDistance, camTarget, newUp);
}

// Step the orbit by 15 degrees: around the vertical axis (yaw) or the view's horizontal axis
void EditorUI::OrbitStep(const bool yaw, const float sign)
{
   if (m_camMode == ViewMode::PreviewCam)
      m_camMode = ViewMode::EditorCam;
   m_predefinedView = PredefinedView::None;
   Matrix3D view(m_camView);
   view.Invert();
   const vec3 up = view.GetOrthoNormalUp(), dir = view.GetOrthoNormalDir(), right = view.GetOrthoNormalRight(), pos = view.GetOrthoNormalPos();
   vec3 camTarget = pos - dir * m_camDistance;
   GetNavPivot(camTarget);
   const Matrix3D rot = Matrix3D::MatrixRotate(sign * ANGTORAD(15.f), yaw ? vec3(0.f, 0.f, -1.f) : right);
   m_camMode = ViewMode::EditorCam;
   const vec3 newEye = camTarget + rot.MultiplyVectorNoPerspective(pos - camTarget); // Rigid rotation about the pivot
   m_camView = Matrix3D::MatrixLookAtRH(newEye, newEye - rot.MultiplyVectorNoPerspective(dir) * m_camDistance, rot.MultiplyVectorNoPerspective(up));
}

void EditorUI::SetGizmoOperation(ImGuizmo::OPERATION operation)
{
   m_measure.SetActive(*this, false); // Another tool (Select included) leaves the Measure tool
   if (operation == ImGuizmo::OPERATION(0))
   {
      m_gizmoOperation = operation;
      return;
   }
   if (m_camMode == ViewMode::PreviewCam)
      m_camMode = ViewMode::EditorCam;
   // Repeating the same operation flips the Transform Orientation between Global and Local (from Normal or View it goes to Global); it is a saved setting, so
   // switching to another operation keeps it
   if (m_gizmoOperation == operation)
      SetTransformOrientation(m_transformOrientation == TransformOrientation::Global ? TransformOrientation::Local : TransformOrientation::Global);
   m_gizmoOperation = operation;
}

bool EditorUI::GetSelectionTransform(Matrix3D &transform) const
{
   if (m_pointEditPart)
   {
      // In center edit mode, the gizmo operates on the part's center point
      if (m_pointEditCenter)
      {
         if (!m_centerSelected)
            return false;
         const Vertex3Ds center = m_pointEditPart->GetEditCenter();
         transform = Matrix3D::MatrixTranslate(center.x, center.y, center.z);
         return true;
      }
      // In drag point edit mode, the gizmo operates on the selected points (positioned at their centroid,
      // which stays fixed when rotating or scaling the points around it, unlike the bounding box center)
      if (m_pointSel.empty())
         return false;
      Vertex2D center(0.f, 0.f);
      float z = 0.f;
      for (const DragPoint *point : m_pointSel)
      {
         center += Vertex2D(point->GetX(), point->GetY());
         z += m_pointEditPart->GetDragPointZ(point);
      }
      const float invCount = 1.f / (float)m_pointSel.size();
      transform = Matrix3D::MatrixTranslate(center.x * invCount, center.y * invCount, z * invCount);
      return true;
   }
   if (m_selection.GetType() == Selection::S_EDITABLE)
   {
      const EditorUIPart::TransformMask mask = m_selection.GetPart()->GetTransform(transform);
      return mask != EditorUIPart::TransformMask::TM_None;
   }
   return false;
}

// Navigation pivot: the center of the selection bounds when 'orbit around selection' is enabled and something is selected (target left unchanged otherwise)
bool EditorUI::GetNavPivot(vec3 & target) const
{
   FRect3D bounds;
   if (!m_navOrbitSelection || m_camMode == ViewMode::DesktopBackdrop || !GetSelectionBounds(bounds, &m_localView))
      return false;
   target = vec3(0.5f * (bounds.left + bounds.right), 0.5f * (bounds.top + bounds.bottom), -0.5f * (bounds.zlow + bounds.zhigh));
   return true;
}

void EditorUI::ExtendBoundsWithPart(FRect3D &bounds, const std::shared_ptr<EditorUIPart> &part) const
{
   IEditable *const editable = part->GetEditable();
   if (editable->GetIHitable() != nullptr)
      for (const HitObject *const hitObject : m_player->m_physics->GetUIHitObjects(editable))
         bounds.Extend(hitObject->m_hitBBox);
   // Also include the part's position to cover parts without hit objects
   Matrix3D transform;
   if (part->GetTransform(transform) != EditorUIPart::TM_None)
   {
      const Vertex3Ds pos = transform.GetOrthoNormalPos();
      bounds.Extend(FRect3D(pos.x, pos.x, pos.y, pos.y, pos.z, pos.z));
   }
}

bool EditorUI::GetSelectionBounds(FRect3D &bounds, const LocalViewParts *local) const
{
   bounds.Clear();
   if (m_pointEditPart)
   {
      // In center edit mode, the bounds are the part's center point
      if (m_pointEditCenter)
      {
         if (m_centerSelected)
         {
            const Vertex3Ds center = m_pointEditPart->GetEditCenter();
            bounds.Extend(FRect3D(center.x, center.x, center.y, center.y, center.z, center.z));
         }
      }
      else
         // In drag point edit mode, the bounds are the ones of the selected points
         for (const DragPoint *point : m_pointSel)
         {
            const float z = m_pointEditPart->GetDragPointZ(point);
            bounds.Extend(FRect3D(point->GetX(), point->GetX(), point->GetY(), point->GetY(), z, z));
         }
   }
   else
   {
      for (const auto &part : m_multiSel)
         if (local == nullptr || local->Allows(part->GetEditable()))
            ExtendBoundsWithPart(bounds, part);
      if (m_multiSel.empty() && m_selection.GetPart() && (local == nullptr || local->Allows(m_selection.GetPart()->GetEditable())))
         ExtendBoundsWithPart(bounds, m_selection.GetPart());
   }
   return bounds.left <= bounds.right;
}

// Alt+G / Alt+S / Alt+R: one undo step for the reset (the transform funnel itself does not record)
void EditorUI::ClearSelectionTransform(const Matrix3D &transform, bool clearPosition, bool clearScale, bool clearRotation, const char *label)
{
   if (m_measure.IsActive())
   {
      Report("Measure tool active");
      return;
   }
   if (m_table->m_liveBaseTable == nullptr)
   {
      m_undo.BeginUndo();
      for (const auto &part : m_multiSel)
         m_undo.MarkForUndo(part->GetEditable());
      MarkDependentsForUndo();
      m_undo.SetLabel(CountLabel(label, m_multiSel.size(), m_multiSel.empty() ? string() : m_multiSel.front()->GetEditable()->GetName()));
      m_undo.EndUndo();
   }
   SetSelectionTransform(transform, clearPosition, clearScale, clearRotation);
}

void EditorUI::SetTransformPivot(const TransformPivot pivot)
{
   m_transformPivot = pivot;
   g_settingsService.GetAppSettings().SetEditor_TransformPivot(static_cast<int>(pivot), false);
}

vec3 EditorUI::GetTransformPivotPoint() const
{
   if (m_transformPivot == TransformPivot::Cursor && !(m_pointEditPart != nullptr && m_pointEditCenter)) // (a center point is its own pivot)
      return m_cursor3D;
   vec3 sum(0.f, 0.f, 0.f), low(FLT_MAX, FLT_MAX, FLT_MAX), high(-FLT_MAX, -FLT_MAX, -FLT_MAX), active(0.f, 0.f, 0.f);
   int count = 0;
   const auto add = [&](const vec3 &p)
   {
      sum = sum + p;
      low.Set(min(low.x, p.x), min(low.y, p.y), min(low.z, p.z));
      high.Set(max(high.x, p.x), max(high.y, p.y), max(high.z, p.z));
      count++;
   };
   if (m_pointEditPart != nullptr)
   {
      if (m_pointEditCenter)
         return m_pointEditPart->GetEditCenter();
      for (const DragPoint *point : m_pointSel)
         add(active = vec3(point->GetX(), point->GetY(), m_pointEditPart->GetDragPointZ(point))); // The active point is the last one selected
   }
   else
      for (const auto &part : m_multiSel)
      {
         Matrix3D transform;
         if (part->GetTransform(transform) == EditorUIPart::TM_None)
            continue;
         add(transform.GetOrthoNormalPos());
         if (part == m_selection.GetPart())
            active = transform.GetOrthoNormalPos();
      }
   if (count == 0)
      return active;
   switch (m_transformPivot)
   {
   case TransformPivot::ActiveElement: return active;
   case TransformPivot::BoundingBoxCenter: return (low + high) * 0.5f;
   default: return sum * (1.f / static_cast<float>(count));
   }
}

bool EditorUI::UsesGroupGizmo() const
{
   if (m_pointEditPart != nullptr)
      return true;
   // View: the gizmo lines up with the screen, which one part's own matrix can not do
   // (not on a plastic, which moves and turns in the table's X / Y only: its gizmo stays its own, as with Global)
   const int needed = m_transformOrientation == TransformOrientation::View && m_camMode != ViewMode::DesktopBackdrop && !PlasticFromAtlas::ZLocked(*this) ? 1 : 2;
   int count = 0;
   for (const auto &part : m_multiSel)
   {
      Matrix3D transform;
      count += part->GetTransform(transform) != EditorUIPart::TM_None;
      if (count >= needed)
         return true;
   }
   return count == 1 && m_transformPivot == TransformPivot::Cursor; // One part orbits the 3D cursor too: the gizmo sits on it (as Blender's)
}

EditorUI::XformOp EditorUI::OrientedXform(const XformKind kind, const TransformOrientation orientation, const Matrix3D &viewFrame, const int axis, const bool plane, const float value,
   const vec3 &freeAxis, const size_t partCount) const
{
   XformOp op;
   op.kind = kind;
   const bool own = axis >= 0 && IsOwnFrameOrientation(orientation);
   const Matrix3D frame = axis >= 0 && orientation == TransformOrientation::View ? viewFrame : Matrix3D::MatrixIdentity();
   if (kind == XformKind::Rotate)
   {
      op.hasYaw = true;
      if (own)
      {
         // About each part's own axis: L in the parts' axes, and the turn of a part that only stores a Z rotation from the table Z share of its own axis
         op.ownFrame = true;
         op.linear = RotatedTransform(Matrix3D::MatrixIdentity(), TableAxis(axis), value);
         op.angle = value;
         op.axis = vec3(axis == 0 ? -1.f : 0.f, axis == 1 ? -1.f : 0.f, axis == 2 ? -1.f : 0.f); // The matrix' own axis of a turn about TableAxis: minus FlipZ of it
      }
      else
      {
         const vec3 k = axis < 0 ? freeAxis : FlipZ(vec3(frame.m[axis][0], frame.m[axis][1], frame.m[axis][2])); // Global: TableAxis
         op.linear = RotatedTransform(Matrix3D::MatrixIdentity(), k, value);
         op.yaw = value * k.z; // k is in camera space (FlipZ): its Z is minus the table one, which matches YawOfRotation's sense
      }
   }
   else if (kind == XformKind::Scale)
   {
      // Global keeps one part along its own axes (as the editor always did; Blender would use the table's), several along the table's, each part's size following
      // what its axes become. Local / Normal: each part along its own. View: along the screen's
      const Matrix3D d = ScaledTransform(Matrix3D::MatrixIdentity(), axis, plane, value);
      op.negative = value < 0.f;
      if (axis >= 0 && orientation == TransformOrientation::View)
      {
         Matrix3D frameT = Matrix3D::MatrixIdentity();
         for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
               frameT.m[i][j] = frame.m[j][i];
         op.linear = frameT * d * frame; // F^T D F: into the frame's axes, scaled, back
      }
      else
      {
         op.linear = d;
         op.ownFrame = own || partCount == 1;
      }
   }
   return op;
}

void EditorUI::SetTransformOrientation(const TransformOrientation orientation)
{
   m_transformOrientation = orientation;
   g_settingsService.GetAppSettings().SetEditor_TransformOrientation(static_cast<int>(orientation), false);
}

bool EditorUI::IsOwnFrameOrientation(const TransformOrientation orientation) const
{
   // Drag points have no axes of their own (Blender's edit mode would use the object's, and a normal): Local and Normal act as Global there
   return (orientation == TransformOrientation::Local || orientation == TransformOrientation::Normal) && m_pointEditPart == nullptr;
}

Matrix3D EditorUI::GetActiveAxes() const
{
   Matrix3D result = Matrix3D::MatrixIdentity();
   if (m_pointEditPart != nullptr)
      return result; // Drag points are in the table's axes
   Matrix3D active;
   bool found = m_selection.GetType() == Selection::S_EDITABLE && m_selection.GetPart()->GetTransform(active) != EditorUIPart::TM_None;
   for (size_t i = 0; !found && i < m_multiSel.size(); i++)
      found = m_multiSel[i]->GetTransform(active) != EditorUIPart::TM_None;
   if (found)
   {
      vec3 pos, scale, rot;
      ExtractTRS(active, pos, scale, rot);
      const float s[3] = { 1.f / scale.x, 1.f / scale.y, 1.f / scale.z };
      for (int i = 0; i < 3; i++)
         for (int j = 0; j < 3; j++)
            result.m[i][j] = active.m[i][j] * s[i];
   }
   return result;
}

Matrix3D EditorUI::GetViewOrientationFrame(const int viewSlot) const
{
   // The camera of the view, as the modal transform reads it (see UpdateModalTransform): its right and up, from the editor camera space (FlipZ) to the table's
   Matrix3D view(m_camView);
   if (viewSlot >= 0 && m_viewInfo[viewSlot].valid)
   {
      const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f), YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
      view = RH2LH * m_viewInfo[viewSlot].view * YAxis;
   }
   view.Invert();
   vec3 x = FlipZ(view.GetOrthoNormalRight()), y = FlipZ(view.GetOrthoNormalUp());
   x.Normalize();
   y = y - x * y.Dot(x); // Orthonormal, whatever the precision of the camera matrix
   y.Normalize();
   // Z towards the viewer, as Blender's View Z (R Z 90 turns counterclockwise on screen, G Z moves towards the viewer). The table's frame is mirrored from the screen's
   // (Y towards the player), so in table coordinates this frame is mirrored too: the gizmo, which needs a proper matrix, takes the opposite Z (GetGroupGizmoMatrix)
   const vec3 z = CrossProduct(y, x);
   Matrix3D result = Matrix3D::MatrixIdentity();
   const vec3 rows[3] = { x, y, z };
   for (int i = 0; i < 3; i++)
   {
      result.m[i][0] = rows[i].x;
      result.m[i][1] = rows[i].y;
      result.m[i][2] = rows[i].z;
   }
   return result;
}

Matrix3D EditorUI::GetGroupGizmoMatrix(const int viewSlot) const
{
   Matrix3D result = Matrix3D::MatrixIdentity();
   // Drag points stay on the table's axes (their edits are in its plane), as does View in the desktop backdrop (a 2D view of its own)
   if (m_transformOrientation == TransformOrientation::View && m_pointEditPart == nullptr && m_camMode != ViewMode::DesktopBackdrop && !PlasticFromAtlas::ZLocked(*this))
   {
      result = GetViewOrientationFrame(viewSlot);
      result._31 = -result._31; // A proper matrix for ImGuizmo: Z into the screen (a drag is read back relative to it, the handle's sense changes nothing)
      result._32 = -result._32;
      result._33 = -result._33;
   }
   else if (IsOwnFrameOrientation(m_transformOrientation))
      result = GetActiveAxes();
   const vec3 pivot = GetTransformPivotPoint();
   result._41 = pivot.x;
   result._42 = pivot.y;
   result._43 = pivot.z;
   return result;
}

void EditorUI::BeginSelectionXform(SelectionXform &xform) const
{
   xform = SelectionXform();
   xform.pivot = GetTransformPivotPoint();
   if (m_pointEditPart != nullptr)
   {
      xform.centerEdit = m_pointEditCenter;
      xform.rail = m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive && static_cast<Primitive *>(m_pointEditPart->GetEditable())->IsRailRoute();
      if (!m_pointEditCenter)
         for (DragPoint *point : m_pointSel)
            xform.points.push_back({ point, point->GetX(), point->GetY(), point->GetZ(), m_pointEditPart->GetDragPointZ(point) });
      return;
   }
   xform.individual = m_transformPivot == TransformPivot::IndividualOrigins;
   for (const auto &uiPart : m_multiSel)
   {
      Matrix3D start;
      const int mask = static_cast<int>(uiPart->GetTransform(start));
      if (mask == EditorUIPart::TM_None)
         continue;
      SelectionXform::Part part;
      part.part = uiPart;
      part.mask = mask;
      ReadPartTransform(*uiPart, start, part.pos, part.scale, part.rot);
      part.axes = Matrix3D::MatrixIdentity();
      const float s[3] = { 1.f / part.scale.x, 1.f / part.scale.y, 1.f / part.scale.z };
      for (int i = 0; i < 3; i++)
         for (int j = 0; j < 3; j++)
            part.axes.m[i][j] = start.m[i][j] * s[i];
      xform.parts.push_back(part);
   }
}

Matrix3D EditorUI::TurnedAxes(const SelectionXform::Part &part, const XformOp &op)
{
   if (!op.ownFrame)
      return part.axes * op.linear;
   Matrix3D toFrame = Matrix3D::MatrixIdentity();
   for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
         toFrame.m[i][j] = part.axes.m[j][i];
   return part.axes * (toFrame * op.linear * part.axes);
}

void EditorUI::ApplySelectionXform(const SelectionXform &xform, const XformOp &op, const vector<vec3> *reference)
{
   if (m_pointEditPart != nullptr)
   {
      // Points keep X and Y (their Z is the part's, but for a Wire Rail): rotations are about the table Z here (see BeginModalTransform), and a point's data is its position
      if (xform.centerEdit)
      {
         const vec3 c = vec3(xform.pivot.x + op.move.x, xform.pivot.y + op.move.y, 0.f);
         m_pointEditPart->SetEditCenter(Vertex2D(c.x, c.y));
      }
      else
      {
         // S on the holes of a plastic changes their diameters, not their places (each hole about its own centre, both ends of a pill)
         if (op.kind != XformKind::Scale || !PlasticFromAtlas::ScaleHoles(*this))
         {
            for (const SelectionXform::Point &point : xform.points)
            {
               const float dz = xform.rail ? point.viewZ - xform.pivot.z : 0.f;
               const Vertex3Ds v = op.linear.MultiplyVectorNoTranslate(Vertex3Ds(point.x - xform.pivot.x, point.y - xform.pivot.y, dz));
               point.point->SetX(xform.pivot.x + v.x + op.move.x);
               point.point->SetY(xform.pivot.y + v.y + op.move.y);
               if (xform.rail)
                  point.point->SetZ(point.z + v.z - dz + op.move.z);
            }
         }
         m_pointEditPart->GetDragPointCurve()->OnPointsModified();
         DetachRubberRefs(m_pointEditPart->GetEditable());
      }
      m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
      m_renderer->InvalidateShadowContent();
      m_player->m_physics->Update(m_pointEditPart->GetEditable());
      UpdateRampWallsPhysics(m_player, m_pointEditPart->GetEditable());
      return;
   }
   const bool turns = memcmp(op.linear.m, Matrix3D::MatrixIdentity().m, sizeof(op.linear.m)) != 0;
   const bool zLocked = PlasticFromAtlas::ZLocked(*this);
   for (size_t index = 0; index < xform.parts.size(); index++)
   {
      const SelectionXform::Part &part = xform.parts[index];
      Matrix3D l = op.linear;
      vec3 move = op.move;
      if (op.ownFrame)
      {
         Matrix3D toFrame = Matrix3D::MatrixIdentity();
         for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
               toFrame.m[i][j] = part.axes.m[j][i];
         l = toFrame * op.linear * part.axes;
         move = part.axes.MultiplyVectorNoTranslate(op.move);
      }
      const vec3 c = xform.individual ? part.pos : xform.pivot;
      vec3 pos = c + vec3(l.MultiplyVectorNoTranslate(part.pos - c)) + move;
      if (zLocked)
         pos.z = part.pos.z; // A plastic moves in X and Y only, whatever the orientation's axes (View) or the pivot
      vec3 scale = part.scale, rot = part.rot;
      if (op.kind == XformKind::Rotate)
      {
         // Parts with a full rotation take L. A part with only a Z rotation takes what L has about the table Z (see YawOfRotation), a part with none keeps its own
         if ((part.mask & EditorUIPart::TM_RotAny) != 0)
         {
            float step;
            const bool full = (part.mask & (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY)) == (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY);
            const Matrix3D rotation = full ? l
               : ZRotation(!op.hasYaw ? YawOfRotation(l, step)
                       : op.ownFrame  ? op.angle * part.axes.MultiplyVectorNoTranslate(op.axis).z // The turn about this part's own axis: its table Z share
                                      : op.yaw);
            vec3 unusedPos, unusedScale, current;
            const vec3 *const ref = !full ? nullptr
               : reference != nullptr                                 ? &(*reference)[index]
               : turns && GetPartEuler(*part.part, current)           ? &current
                                                                      : &part.rot;
            ExtractTRS(part.axes * rotation, unusedPos, unusedScale, rot, ref);
         }
      }
      else if (op.kind == XformKind::Scale)
      {
         // The rotation is kept and the parts do not shear: a part's size along each of its axes is the length L gives that axis (Blender does the same to objects).
         // A negative factor is not stored as a mirror: the scaled axes are read back as a rotation and positive sizes, as the editor always did (a negative size
         // would render inside out with backface culling, and read back as a half turn by the next transform). Two reversed axes (a plane) are a half turn
         const float det = l._11 * (l._22 * l._33 - l._23 * l._32) - l._12 * (l._21 * l._33 - l._23 * l._31) + l._13 * (l._21 * l._32 - l._22 * l._31);
         if (op.negative || det < 0.f)
         {
            Matrix3D m = Matrix3D::MatrixIdentity();
            const float axisScale[3] = { part.scale.x, part.scale.y, part.scale.z };
            for (int i = 0; i < 3; i++)
            {
               const vec3 axis = vec3(l.MultiplyVectorNoTranslate(vec3(part.axes.m[i][0], part.axes.m[i][1], part.axes.m[i][2]))) * axisScale[i];
               m.m[i][0] = axis.x;
               m.m[i][1] = axis.y;
               m.m[i][2] = axis.z;
            }
            vec3 unusedPos;
            ExtractTRS(m, unusedPos, scale, rot);
         }
         else
         {
            const float axisScale[3] = { part.scale.x, part.scale.y, part.scale.z };
            float out[3];
            for (int i = 0; i < 3; i++)
               out[i] = axisScale[i] * max(vec3(l.MultiplyVectorNoTranslate(vec3(part.axes.m[i][0], part.axes.m[i][1], part.axes.m[i][2]))).Length(), 1e-8f);
            scale.Set(out[0], out[1], out[2]);
         }
      }
      ApplyPartTRS(part.part, pos, scale, rot);
   }
   DetachSelectedRubbersFromUnselectedPosts();
   RefitSelectionDependents();
}

void EditorUI::ApplyPartTRS(const std::shared_ptr<EditorUIPart> &part, const vec3 &pos, const vec3 &scale, const vec3 &rot)
{
   IEditable *const edit = part->GetEditable();
   Primitive *const prim = edit->GetItemType() == eItemPrimitive ? static_cast<Primitive *>(edit) : nullptr;
   const bool generated = prim != nullptr && (prim->m_d.m_plasticGen || prim->IsCurveHost());
   // In a drag, a wire or a plastic is drawn under a matrix too (with its companions), its mesh is rebuilt at the drag end (FlushDrag)
   const bool deferred = generated && (m_modal.active || m_gizmoDragActive) && prim->CanDeferGenMesh();
   if (generated)
      prim->m_genDragDefer = deferred;
   part->SetTransform(pos, scale, rot);
   if (generated)
      prim->m_genDragDefer = false;
   if (prim == nullptr || !prim->IsTransformedByMatrix() || (generated && !deferred)) // A primitive is drawn under its matrix (a generated one was rebuilt, its static buffer can only be replaced)
      m_renderer->ReinitRenderable(edit->GetIRenderable());
   if (m_modal.active || m_gizmoDragActive)
   {
      // Per frame while dragging: the static shadow pages and the gameplay colliders follow at the drag end (FlushDrag)
      m_drag.shadowDirty = true;
      // The collider overlay shows the gameplay colliders: while it is on, the dragged parts' colliders follow per frame (1-2 ms each)
      // Above kMaxLiveColliderParts dragged parts (~1.7 ms each) the per-frame rebuild is skipped: the overlay catches up at the drag end (FlushDrag)
      constexpr size_t kMaxLiveColliderParts = 32;
      if (m_physOverlay != PhysicOverlay::None && m_multiSel.size() <= kMaxLiveColliderParts)
         m_player->m_physics->Update(edit);
      else
         m_player->m_physics->UpdateUI(edit);
      if (std::ranges::find(m_drag.pendingPhysics, edit) == m_drag.pendingPhysics.end())
         m_drag.pendingPhysics.push_back(edit);
      if (prim != nullptr && prim->m_d.m_rampCollider) // The walls part: its drag flush and physics update at the drag end (not added in FlushDrag, which iterates this list)
         for (Primitive *const comp : prim->GetPlasticCompanions())
            if (std::ranges::find(m_drag.pendingPhysics, static_cast<IEditable *>(comp)) == m_drag.pendingPhysics.end())
               m_drag.pendingPhysics.push_back(comp);
   }
   else
   {
      m_renderer->InvalidateShadowContent(); // Cached (static) shadow pages and light maps follow the move
      m_player->m_physics->Update(edit);
      UpdateRampWallsPhysics(m_player, edit);
   }
}

void EditorUI::ReadPartTransform(EditorUIPart &part, const Matrix3D &transform, vec3 &pos, vec3 &scale, vec3 &rot)
{
   vec3 stored;
   ExtractTRS(transform, pos, scale, rot, GetPartEuler(part, stored) ? &stored : nullptr);
}

void EditorUI::ApplyPartTransform(const std::shared_ptr<EditorUIPart> &part, const Matrix3D &partTransform, const bool clearPos, const bool clearScale, const bool clearRot)
{
   vec3 pos, scale, rot;
   ReadPartTransform(*part, partTransform, pos, scale, rot);
   if (clearPos)
   {
      const float z = pos.z;
      pos.Set(0.f, 0.f, 0.f);
      const IEditable *const edit = part->GetEditable();
      if (m_multiSel.size() == 1 && edit != nullptr && edit->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(edit)->m_d.m_plasticGen)
         pos.z = z; // A single plastic keeps its height (it would sink by its thickness)
   }
   if (clearScale)
      scale.Set(1.f, 1.f, 1.f);
   if (clearRot)
      rot.Set(0.f, 0.f, 0.f);
   ApplyPartTRS(part, pos, scale, rot);
}

void EditorUI::SetSelectionTransform(const Matrix3D &newTransform, bool clearPosition, bool clearScale, bool clearRotation)
{
   if (m_pointEditPart)
   {
      // In point edit mode, apply the gizmo transform delta to the selected points in the table XY plane
      // (to the part's center point in center edit mode)
      if (m_pointEditCenter)
      {
         if (!m_centerSelected)
            return;
         Matrix3D oldTransform;
         GetSelectionTransform(oldTransform);
         Matrix3D invOldTransform(oldTransform);
         invOldTransform.Invert();
         const Matrix3D delta = invOldTransform * newTransform;
         const Vertex3Ds v = delta * m_pointEditPart->GetEditCenter();
         m_pointEditPart->SetEditCenter(Vertex2D(v.x, v.y));
      }
      else
      {
         if (m_pointSel.empty())
            return;
         Matrix3D oldTransform;
         GetSelectionTransform(oldTransform);
         Matrix3D invOldTransform(oldTransform);
         invOldTransform.Invert();
         const Matrix3D delta = invOldTransform * newTransform;
         const bool rail = m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive && static_cast<Primitive *>(m_pointEditPart->GetEditable())->IsRailRoute();
         for (DragPoint *point : m_pointSel)
         {
            const Vertex3Ds v = delta * point->GetVertex();
            point->SetX(v.x);
            point->SetY(v.y);
            if (rail)
               point->SetZ(v.z);
         }
         m_pointEditPart->GetDragPointCurve()->OnPointsModified();
         DetachRubberRefs(m_pointEditPart->GetEditable());
      }
      m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
      m_renderer->InvalidateShadowContent();
      m_player->m_physics->Update(m_pointEditPart->GetEditable());
      UpdateRampWallsPhysics(m_player, m_pointEditPart->GetEditable());
      return;
   }

   if (m_selection.GetType() != Selection::S_EDITABLE)
      return;

   const auto applyTransform = [this](const std::shared_ptr<EditorUIPart> &part, const Matrix3D &partTransform, bool clearPos, bool clearScale, bool clearRot)
   {
      ApplyPartTransform(part, partTransform, clearPos, clearScale, clearRot);
   };

   if (clearPosition || clearScale || clearRotation)
   {
      // Reset the requested transform components of each selected part
      for (const auto &part : m_multiSel)
      {
         Matrix3D partTransform;
         if (part->GetTransform(partTransform) == EditorUIPart::TM_None)
            continue;
         applyTransform(part, partTransform, clearPosition, clearScale, clearRotation);
      }
      DetachSelectedRubbersFromUnselectedPosts();
      RefitSelectionDependents();
      return;
   }

   // Apply the active part's transform delta to every selected part
   Matrix3D delta;
   m_selection.GetPart()->GetTransform(delta);
   delta.Invert();
   delta = delta * newTransform;
   for (const auto &part : m_multiSel)
   {
      Matrix3D partTransform;
      if (part->GetTransform(partTransform) == EditorUIPart::TM_None)
         continue;
      applyTransform(part, partTransform * delta, false, false, false);
   }
   DetachSelectedRubbersFromUnselectedPosts();
   RefitSelectionDependents();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Drag point edit mode: edit the points of the DragPointCurve of the active selected part.
// Entered/exited with Tab (or Escape). The selection is saved on entry and restored on exit.
//

bool EditorUI::CanEnterPointEdit() const
{
   return m_pointEditPart == nullptr && !IsInspectMode() && !m_table->IsLocked() && m_selection.GetType() == Selection::S_EDITABLE && m_selection.GetPart() != nullptr
      && (m_selection.GetPart()->GetDragPointCurve() != nullptr || m_selection.GetPart()->HasEditCenter());
}

void EditorUI::EnterPointEditMode()
{
   if (!CanEnterPointEdit())
      return;
   if (!m_localView.Allows(m_selection.GetPart()->GetEditable()))
   {
      Report("Local View: the part is not in this view");
      return;
   }
   m_savedSelection = m_selection;
   m_savedMultiSel = m_multiSel;
   m_savedOutlinerAnchor = m_outlinerAnchor;
   m_pointEditPart = m_selection.GetPart();
   m_plasticShowSheet = true; // Editing the shape of a plastic starts with the sheet shown
   m_pointEditPart->SetPointEditContext(this);
   // Parts without outline points but with an editable center (light sequencer, lights of circle shape) edit it directly
   const DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve();
   m_pointEditCenter = m_pointEditPart->HasEditCenter() && (curve == nullptr || curve->GetPoints().empty());
   m_centerSelected = m_pointEditCenter;
   m_pointSel.clear();
}

void EditorUI::ExitPointEditMode(bool restoreSelection)
{
   m_plasticBow = PlasticFromAtlas::Bow();
   m_plasticPill = PlasticFromAtlas::Pill();
   if (m_pointEditPart)
   {
      m_pointEditPart->SetPointEditContext(nullptr);
      if (m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive)
         static_cast<Primitive *>(m_pointEditPart->GetEditable())->m_plasticEditHoles = false; // The gizmo pivot is the outline's
   }
   m_pointEditPart.reset();
   m_pointEditCenter = false;
   m_centerSelected = false;
   m_pointSel.clear();
   m_pointDragPending = false;
   m_pointDragActive = false;
   if (!restoreSelection)
      return;
   // Restore the selection as it was when entering this mode, filtering out parts deleted since then
   const ankerl::unordered_dense::set<const IEditable *> liveParts(m_table->GetParts().begin(), m_table->GetParts().end());
   std::erase_if(m_savedMultiSel, [&liveParts](const std::shared_ptr<EditorUIPart> &part) { return !liveParts.contains(part->GetEditable()); });
   m_multiSel = m_savedMultiSel;
   m_selection = (m_savedSelection.GetType() == Selection::S_EDITABLE && (m_savedSelection.GetPart() == nullptr || !liveParts.contains(m_savedSelection.GetPart()->GetEditable())))
      ? (m_multiSel.empty() ? Selection() : Selection(m_multiSel.back()))
      : m_savedSelection;
   m_outlinerAnchor = (m_savedOutlinerAnchor == nullptr || liveParts.contains(m_savedOutlinerAnchor->GetEditable())) ? m_savedOutlinerAnchor : nullptr;
}

bool EditorUI::IsPointSelected(const DragPoint *point) const { return std::ranges::find(m_pointSel, point) != m_pointSel.end(); }

void EditorUI::TogglePointSelection(DragPoint *point)
{
   const auto it = std::ranges::find(m_pointSel, point);
   if (it == m_pointSel.end())
      m_pointSel.push_back(point);
   else
      m_pointSel.erase(it);
}

void EditorUI::BeginPointEdit()
{
   if (m_pointEditPart == nullptr)
      return;
   m_undo.BeginUndo();
   m_undo.MarkForUndo(m_pointEditPart->GetEditable());
   m_undo.SetFieldLabel("Edit Points");
   m_undo.EndUndo();
}

void EditorUI::EndPointEdit()
{
   if (m_pointEditPart == nullptr)
      return;
   if (DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve())
      curve->OnPointsModified();
   DetachRubberRefs(m_pointEditPart->GetEditable());
   m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
   m_renderer->InvalidateShadowContent();
   m_player->m_physics->Update(m_pointEditPart->GetEditable());
   UpdateRampWallsPhysics(m_player, m_pointEditPart->GetEditable());
}

// The point of a polyline nearest the mouse on the screen, within maxPx: its segment and how far along it. False: none (or all behind the camera)
template <class Point>
static bool NearestOnPolyline(const LiveRenderContext &ctx, const vector<Point> &pts, const ImVec2 &mouse, float maxPx, size_t &segment, float &along)
{
   bool found = false;
   for (size_t i = 0; i + 1 < pts.size(); i++)
   {
      const ImVec2 a = ctx.Project(Vertex3Ds(pts[i].x, pts[i].y, pts[i].z)), b = ctx.Project(Vertex3Ds(pts[i + 1].x, pts[i + 1].y, pts[i + 1].z));
      if (a.x == FLT_MAX || b.x == FLT_MAX)
         continue;
      const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
      const float t = len2 > 0.f ? clamp(((mouse.x - a.x) * dx + (mouse.y - a.y) * dy) / len2, 0.f, 1.f) : 0.f;
      const float d = sqrtf((a.x + t * dx - mouse.x) * (a.x + t * dx - mouse.x) + (a.y + t * dy - mouse.y) * (a.y + t * dy - mouse.y));
      if (d < maxPx)
      {
         maxPx = d;
         segment = i;
         along = t;
         found = true;
      }
   }
   return found;
}

// Wire Rail, as VPE: double-click the route to insert a point there, double-click a point to delete it. False: not a rail, or nothing near the mouse
bool EditorUI::RailPointDoubleClick(DragPoint *hitPoint, const ImVec2 &mousePos)
{
   if (m_table->IsLocked() || m_pointEditPart->GetEditable()->GetItemType() != eItemPrimitive || !static_cast<Primitive *>(m_pointEditPart->GetEditable())->IsRailRoute())
      return false;
   Primitive *const rail = static_cast<Primitive *>(m_pointEditPart->GetEditable());
   if (hitPoint != nullptr)
   {
      m_pointSel.clear();
      m_pointSel.push_back(hitPoint);
      DeleteSelectedPoints(); // Keeps the two points of an open route
      return true;
   }
   vector<RenderVertex3D> fine;
   rail->m_wireCurve.GetRgVertex(fine, false, 0.25f);
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   size_t seg = 0;
   float t = 0.f;
   if (!NearestOnPolyline(ctx, fine, mousePos, 8.f * m_liveUI.GetDPI(), seg, t))
      return false;
   const Vertex2D at(fine[seg].x + t * (fine[seg + 1].x - fine[seg].x), fine[seg].y + t * (fine[seg + 1].y - fine[seg].y));
   m_undo.BeginUndo();
   m_undo.MarkForUndo(rail);
   m_undo.SetFieldLabel("Add Point");
   m_undo.EndUndo();
   DragPoint *const point = m_pointEditPart->AddPointOnCurve(at);
   if (point == nullptr)
   {
      m_undo.Discard();
      return true;
   }
   m_pointSel.clear();
   m_pointSel.push_back(point);
   m_renderer->ReinitRenderable(rail->GetIRenderable());
   m_renderer->InvalidateShadowContent();
   m_player->m_physics->Update(rail);
   UpdateRampWallsPhysics(m_player, rail);
   return true;
}

// The handle of the selected Wire Rail support: a press on it drags its Position along the route (the nearest route point under the mouse), a render mesh edit per frame
void EditorUI::RailFixtureHandle()
{
   const ImGuiIO &io = ImGui::GetIO();
   RailFixtureDrag &drag = m_railFixtureDrag;
   Primitive *rail = nullptr;
   PrimitiveUIPart *ui = nullptr;
   if (m_selection.GetType() == Selection::S_EDITABLE && m_multiSel.size() <= 1 && m_selection.GetPart() != nullptr)
   {
      IEditable *const e = m_selection.GetPart()->GetEditable();
      if (e->GetItemType() == eItemPrimitive && static_cast<Primitive *>(e)->m_d.m_wireRail)
      {
         rail = static_cast<Primitive *>(e);
         ui = static_cast<PrimitiveUIPart *>(m_selection.GetPart().get());
      }
   }
   if (drag.active)
   {
      const bool valid = rail != nullptr && rail == drag.rail && ui->GetRailFixture() == drag.index && drag.index < (int)rail->m_d.m_railFixtures.size();
      const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
      if (valid && cancel)
         rail->m_d.m_railFixtures[drag.index].position = drag.startPosition; // Back where it was, and the record of the move dropped
      if (valid && !cancel && ImGui::IsMouseDown(ImGuiMouseButton_Left))
      {
         const vector<float> &dist = rail->GetRailRowDist();
         const vector<WireRail::Frame> &frames = rail->GetRailFrames();
         vector<Vertex3Ds> pts;
         for (const WireRail::Frame &f : frames)
            pts.emplace_back(f.p.x, f.p.y, f.p.z);
         const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
         size_t seg = 0;
         float t = 0.f;
         if (dist.size() == frames.size() && NearestOnPolyline(ctx, pts, ImGui::GetMousePos(), 1e9f, seg, t))
         {
            const float position = dist[seg] + t * (dist[seg + 1] - dist[seg]);
            WireRail::Fixture &fx = rail->m_d.m_railFixtures[drag.index];
            if (fabsf(position - fx.position) > 1e-4f)
            {
               fx.position = position;
               drag.moved = true;
               rail->UpdateRailFixtures(); // The render mesh only, the picking collider is refreshed after the release
            }
         }
         return;
      }
      // Released, canceled or lost: the record stays only for a move that ended with the release
      const bool keep = drag.moved && (!cancel || !valid); // Also when the selection changed mid-drag (A, Ctrl+I, Tab): the record holds the start Position, so the move stays undoable
      if (valid && cancel)
         rail->UpdateRailFixtures();
      if (!keep && m_undo.TopRecordId() == drag.recordId)
         m_undo.Discard();
      if (valid)
         m_renderer->InvalidateShadowContent();
      if (cancel)
         m_suppressNextEscape = true; // The Esc release is not an editor Esc (no deselect)
      drag = RailFixtureDrag();
      return;
   }
   if (rail == nullptr || ui == nullptr || m_table->IsLocked() || !m_viewportVisible || m_modal.active || io.KeyShift || m_plasticTrace.active || m_measure.IsActive() || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)
      || (io.WantCaptureMouse && !ImGuizmo::IsOver()) || ImGuizmo::IsViewManipulateHovered() || !SelectionInView(-1)) // (not on a rail the 3D Viewport's Local View hides)
      return;
   const int index = ui->GetRailFixture();
   const vector<WireRail::FixtureShape> &shapes = rail->GetRailFixtureShapes();
   if (index < 0 || index >= (int)shapes.size() || index >= (int)rail->m_d.m_railFixtures.size() || !shapes[index].built)
      return;
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   const ImVec2 at = ctx.Project(Vertex3Ds(shapes[index].anchor.x, shapes[index].anchor.y, shapes[index].anchor.z));
   const ImVec2 mouse = ImGui::GetMousePos();
   const float reach = 8.f * m_liveUI.GetDPI();
   if (at.x == FLT_MAX || (at.x - mouse.x) * (at.x - mouse.x) + (at.y - mouse.y) * (at.y - mouse.y) > reach * reach)
      return;
   m_undo.BeginUndo();
   m_undo.MarkForUndo(rail);
   m_undo.SetFieldLabel("Move Fixture");
   m_undo.EndUndo();
   drag.active = true;
   drag.rail = rail;
   drag.index = index;
   drag.startPosition = rail->m_d.m_railFixtures[index].position;
   drag.moved = false;
   drag.recordId = m_undo.TopRecordId();
   m_pointDragPending = false; // The press is the handle's, not a point's or the box select's
   m_boxSelectActive = false;
}

void EditorUI::AddPointOnNearestSegment()
{
   DragPointCurve *const curve = (m_pointEditPart != nullptr && !m_table->IsLocked()) ? m_pointEditPart->GetDragPointCurve() : nullptr;
   if (curve == nullptr)
      return;
   const auto &points = curve->GetPoints();
   const bool plasticHoles = m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive && static_cast<Primitive *>(m_pointEditPart->GetEditable())->m_d.m_plasticGen
      && static_cast<Primitive *>(m_pointEditPart->GetEditable())->m_plasticEditHoles;
   if (points.empty() && !plasticHoles) // The first hole of a plastic is added like the next ones
      return;
   // Unproject the mouse position on the drag plane of the edited curve
   const float z = points.empty() ? m_pointEditPart->GetDragPointZ(nullptr) : m_pointEditPart->GetDragPointZ(m_pointSel.empty() ? points.front().get() : m_pointSel.front());
   const Vertex2D pos = UnprojectToPlane(ImGui::GetMousePos(), z);
   m_undo.BeginUndo();
   m_undo.MarkForUndo(m_pointEditPart->GetEditable());
   m_undo.SetFieldLabel("Add Point");
   m_undo.EndUndo();
   DragPoint *const point = m_pointEditPart->AddPointOnCurve(pos);
   if (point == nullptr)
   {
      m_undo.Discard();
      return;
   }
   DetachRubberRefs(m_pointEditPart->GetEditable());
   // Select the newly created point
   m_pointSel.clear();
   m_pointSel.push_back(point);
   m_renderer->ReinitRenderable(m_pointEditPart->GetEditable()->GetIRenderable());
   m_renderer->InvalidateShadowContent();
   m_player->m_physics->Update(m_pointEditPart->GetEditable());
   UpdateRampWallsPhysics(m_player, m_pointEditPart->GetEditable());
}

Vertex2D EditorUI::UnprojectToPlane(const ImVec2 &mousePos, float z) const
{
   // Compute the mouse ray and intersect it with the table plane at the given Z coordinate
   const float rClipWidth = (float)m_player->m_playfieldWnd->GetWidth() * 0.5f;
   const float rClipHeight = (float)m_player->m_playfieldWnd->GetHeight() * 0.5f;
   const float xcoord = (mousePos.x - rClipWidth) / rClipWidth;
   const float ycoord = (rClipHeight - mousePos.y) / rClipHeight;
   Matrix3D invMVP = m_renderer->GetMVP().GetModelViewProj(0);
   invMVP.Invert();
   const Vertex3Ds v3d = invMVP * Vertex3Ds { xcoord, ycoord, 0.f };
   const Vertex3Ds v3d2 = invMVP * Vertex3Ds { xcoord, ycoord, 1.f };
   const float dz = v3d2.z - v3d.z;
   const float t = (fabsf(dz) > 1e-10f) ? (z - v3d.z) / dz : 0.f;
   return Vertex2D(v3d.x + t * (v3d2.x - v3d.x), v3d.y + t * (v3d2.y - v3d.y));
}

DragPoint *EditorUI::HitTestDragPoint(const ImVec2 &mousePos) const
{
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   const float maxDist = 10.f * m_liveUI.GetDPI();
   DragPoint *best = nullptr;
   float bestDist = maxDist;
   const auto &points = m_pointEditPart->GetDragPointCurve()->GetPoints();
   // On a plastic outline a handle's pick radius is capped by a third of the shorter neighbouring span, so a short line can still be bowed
   const IEditable *const edited = m_pointEditPart->GetEditable();
   const bool plasticOutline = edited->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(edited)->m_d.m_plasticGen && !static_cast<const Primitive *>(edited)->m_plasticEditHoles;
   const bool plasticHoles = edited->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(edited)->m_d.m_plasticGen && static_cast<const Primitive *>(edited)->m_plasticEditHoles;
   const int n = static_cast<int>(points.size());
   vector<ImVec2> proj;
   if (plasticOutline)
   {
      proj.reserve(n);
      for (const auto &point : points)
         proj.push_back(ctx.Project(Vertex3Ds(point->GetX(), point->GetY(), m_pointEditPart->GetDragPointZ(point.get()))));
   }
   for (int i = 0; i < n; i++)
   {
      const auto &point = points[i];
      const ImVec2 pos = plasticOutline ? proj[i] : ctx.Project(Vertex3Ds(point->GetX(), point->GetY(), m_pointEditPart->GetDragPointZ(point.get())));
      if (pos.x == FLT_MAX)
         continue;
      const float dx = pos.x - mousePos.x;
      const float dy = pos.y - mousePos.y;
      const float dist = sqrtf(dx * dx + dy * dy);
      float radius = maxDist;
      if (plasticOutline && n >= 3)
      {
         float shorter = FLT_MAX;
         for (const int j : { (i + n - 1) % n, (i + 1) % n })
            if (proj[j].x != FLT_MAX)
               shorter = min(shorter, hypotf(proj[j].x - pos.x, proj[j].y - pos.y));
         // Floor: the drawn handle can always be grabbed (4 px, coincident neighbours must not give 0); a line of 8 px or more can still be bowed from its middle
         radius = min(maxDist, max(shorter * (1.f / 3.f), 4.f * m_liveUI.GetDPI()));
      }
      if (plasticHoles)
      {
         // The ring beats the handle: the handle's pick radius is a third of the hole's radius on screen (floor 4 px), so the ring of a small hole can be pressed
         const float z = m_pointEditPart->GetDragPointZ(point.get());
         const ImVec2 edge = ctx.Project(Vertex3Ds(point->GetX() + 0.5f * point->GetZ(), point->GetY(), z));
         if (edge.x != FLT_MAX)
            radius = min(maxDist, max(hypotf(edge.x - pos.x, edge.y - pos.y) * (1.f / 3.f), 4.f * m_liveUI.GetDPI()));
      }
      if (dist < bestDist && dist < radius)
      {
         bestDist = dist;
         best = point.get();
      }
   }
   return best;
}

bool EditorUI::HitTestEditCenter(const ImVec2 &mousePos) const
{
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   const ImVec2 pos = ctx.Project(m_pointEditPart->GetEditCenter());
   if (pos.x == FLT_MAX)
      return false;
   const float maxDist = 10.f * m_liveUI.GetDPI();
   const float dx = pos.x - mousePos.x;
   const float dy = pos.y - mousePos.y;
   return dx * dx + dy * dy < maxDist * maxDist;
}

void EditorUI::BoxSelectPoints(const ImVec2 &cornerA, const ImVec2 &cornerB, bool add)
{
   // Select all the drag points of the edited curve projecting inside the given screen box
   if (!add)
      m_pointSel.clear();
   const LiveRenderContext ctx(m_player, nullptr, m_camMode, m_shadeMode, false);
   if (m_pointEditCenter)
   {
      // In center edit mode, select the part's center point if it projects inside the box
      const ImVec2 boxMin(std::min(cornerA.x, cornerB.x), std::min(cornerA.y, cornerB.y));
      const ImVec2 boxMax(std::max(cornerA.x, cornerB.x), std::max(cornerA.y, cornerB.y));
      const ImVec2 pos = ctx.Project(m_pointEditPart->GetEditCenter());
      if (!add)
         m_centerSelected = false;
      if (pos.x >= boxMin.x && pos.x <= boxMax.x && pos.y >= boxMin.y && pos.y <= boxMax.y)
         m_centerSelected = true;
      return;
   }
   const ImVec2 boxMin(std::min(cornerA.x, cornerB.x), std::min(cornerA.y, cornerB.y));
   const ImVec2 boxMax(std::max(cornerA.x, cornerB.x), std::max(cornerA.y, cornerB.y));
   for (const auto &point : m_pointEditPart->GetDragPointCurve()->GetPoints())
   {
      const ImVec2 pos = ctx.Project(Vertex3Ds(point->GetX(), point->GetY(), m_pointEditPart->GetDragPointZ(point.get())));
      if (pos.x >= boxMin.x && pos.x <= boxMax.x && pos.y >= boxMin.y && pos.y <= boxMax.y && !IsPointSelected(point.get()))
         m_pointSel.push_back(point.get());
   }
}

void EditorUI::DeleteSelectedPoints()
{
   if (m_pointEditPart == nullptr || m_pointSel.empty() || m_table->IsLocked() || IsInspectMode())
      return;
   PlasticFromAtlas::AbortDrags(*this); // A pill or bow drag in progress holds points that are about to go
   DragPointCurve *const curve = m_pointEditPart->GetDragPointCurve();
   vector<DragPoint *> deletable;
   for (DragPoint *point : m_pointSel)
      if (point->CanDelete())
         deletable.push_back(point);
   if (deletable.empty())
      return;
   BeginPointEdit();
   m_undo.SetFieldLabel("Delete Points");
   PlasticFromAtlas::BeforeDeletePoints(*this, deletable);
   for (DragPoint *point : deletable)
   {
      m_pointSel.erase(std::ranges::find(m_pointSel, point));
      curve->DeletePoint(point);
   }
   EndPointEdit();
}


void EditorUI::PickUnityPrefab()
{
   if (m_table->IsLocked() || IsInspectMode() || m_player->m_playfieldWnd == nullptr)
      return;
   m_pendingUnityPrefabPath = std::make_shared<string>();
   std::filesystem::path defaultLocation = PathFromString(m_table->GetSettings().GetRecentDir_ImportDir());
   if (defaultLocation.empty())
      defaultLocation = m_table->m_filename.empty() ? PathFromString(m_table->GetSettings().GetRecentDir_LoadDir()) : m_table->m_filename.parent_path();
   const string location = PathToUTF8(defaultLocation); // SDL expects UTF-8
   const SDL_DialogFileFilter filters[] = { { "Unity Prefab", "prefab" } };
   SDL_ShowOpenFileDialog(
      [](void *userdata, const char *const *filelist, int filter)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingUnityPrefabPath), //
      m_player->m_playfieldWnd->GetCore(), filters, 1, location.empty() ? nullptr : location.c_str(), false);
}

void EditorUI::ImportUnityPrefab(const std::filesystem::path &path)
{
   if (m_table->IsLocked() || IsInspectMode())
      return;
   g_settingsService.GetAppSettings().SetRecentDir_ImportDir(PathToString(path.parent_path()), false);
   vector<IEditable *> created;
   const UnityImportResult res = UnityImportPrefab(m_table, path, GetPartGroupForNewPart(), created);
   if (!res.success)
   {
      m_liveUI.PushNotification("Unity prefab import failed: "s + res.error, 10000);
      for (const string &w : res.warnings)
         PLOGW << "UnityPrefabImport: " << w;
      return;
   }
   for (const string &w : res.warnings)
      PLOGW << "UnityPrefabImport: " << w;

   // A running session looks assets up through hash maps built at its start: refresh them so the
   // imported materials and images resolve (same fixup as PinTable::ImportParts).
   if (m_table == m_player->m_ptable)
      m_table->SetupLookUpTables(true);

   m_undo.BeginUndo();
   for (IEditable *const part : created)
   {
      part->AddRef(); // Created parts keep a creation reference, released by the undo record (see PinTable::Uncreate)
      m_undo.MarkForCreate(part);
   }
   m_undo.SetLabel("Import Unity Prefab");
   m_undo.EndUndo();

   // Player side setup, same as part creation
   for (IEditable *const part : created)
   {
      if (part->GetItemType() == eItemBall)
         m_player->m_vball.push_back(static_cast<Ball *>(part));
      m_player->TimerSetup(part);
      if (auto *const renderable = part->GetIRenderable(); renderable)
         renderable->RenderSetup(m_renderer.get());
      if (part->GetIHitable())
         m_player->m_physics->Add(part);
   }

   // Select the imported parts
   UpdateEditableList();
   m_multiSel.clear();
   for (IEditable *const part : created)
      if (const auto it = m_editableMap.find(part); it != m_editableMap.end())
         m_multiSel.push_back(it->second);
   if (!m_multiSel.empty())
      m_selection = Selection(m_multiSel.back());

   m_liveUI.PushNotification("Imported "s + std::to_string(res.parts) + " part(s), "s + std::to_string(res.materials) + " material(s), "s + std::to_string(res.images) + " image(s) from '"s
         + PathToUTF8(path.filename()) + "'"s + (res.warnings.empty() ? ""s : " ("s + std::to_string(res.warnings.size()) + " warnings)"s),
      10000);
}

const char *EditorUI::LastOperationName(const LastOperation::Type type)
{
   switch (type)
   {
   case LastOperation::Type::Add: return "Add Part";
   case LastOperation::Type::Wireform: return "Add Metal Wire Guide";
   case LastOperation::Type::WireRail: return "Add Wire Rail";
   case LastOperation::Type::PlasticFromAtlas: return "Add Plastic from Atlas";
   case LastOperation::Type::Paste: return "Paste";
   case LastOperation::Type::Duplicate: return "Duplicate";
   case LastOperation::Type::Move: return "Move";
   case LastOperation::Type::Rotate: return "Rotate";
   case LastOperation::Type::Scale: return "Scale";
   case LastOperation::Type::RubberBetweenPosts: return "Rubber Around Posts";
   case LastOperation::Type::None: break;
   }
   return "";
}

void EditorUI::SetLastOperation(const LastOperation::Type type, const Matrix3D &initial)
{
   if (m_table->m_liveBaseTable != nullptr || m_pointEditPart != nullptr) // No undo record in inspection mode, and point edits are not operations on the selection
   {
      m_lastOp.type = LastOperation::Type::None;
      return;
   }
   const bool expanded = m_lastOp.expanded;
   m_lastOp = LastOperation();
   m_lastOp.expanded = expanded;
   m_lastOp.type = type;
   m_lastOp.initial = initial;
   m_lastOp.selection = m_multiSel;
   m_lastOp.undoId = m_undo.TopRecordId();
}

// Evaluates the operation again from the state it started from: the undo record holds the state before the operation, so it stays valid
void EditorUI::ApplyLastOperation()
{
   Matrix3D result(m_lastOp.initial);
   switch (m_lastOp.type)
   {
   case LastOperation::Type::Add:
   case LastOperation::Type::Paste:
   case LastOperation::Type::Duplicate:
      result._41 = m_lastOp.base.x + m_lastOp.offset.x;
      result._42 = m_lastOp.base.y + m_lastOp.offset.y;
      result._43 = m_lastOp.base.z + m_lastOp.offset.z;
      break;
   case LastOperation::Type::Move:
      if (!m_lastOp.xform.parts.empty())
      {
         XformOp op;
         op.move = m_lastOp.offset;
         op.ownFrame = m_lastOp.ownFrame;
         ApplySelectionXform(m_lastOp.xform, op);
         return;
      }
      result._41 += m_lastOp.offset.x;
      result._42 += m_lastOp.offset.y;
      result._43 += m_lastOp.offset.z;
      break;
   case LastOperation::Type::Rotate:
   {
      if (!std::isfinite(m_lastOp.angle))
         return;
      const float angle = clamp(m_lastOp.angle, -kMaxLastOpAngle, kMaxLastOpAngle);
      const auto turn = [this](const float a)
      { return OrientedXform(XformKind::Rotate, m_lastOp.orientation, m_lastOp.viewFrame, m_lastOp.axis, false, ANGTORAD(a), m_lastOp.axisView, m_lastOp.xform.parts.size()); };
      // From the snapshot: a Primitive's angles follow the turn in steps of at most a degree, from the snapshot's, as a slow drag takes them (not from the stored
      // ones, which hold the previous value). After the first whole turn, a part whose angles it moved by whole turns only takes the same again for each further
      // turn (the read-back gives the same against angles moved by whole turns), the others (a part at the lock, whose X and Z slide along X +- Z) walk every turn
      vector<vec3> reference;
      vector<char> walking; // The parts with all three angles that are still walked
      for (const SelectionXform::Part &part : m_lastOp.xform.parts)
      {
         reference.push_back(part.rot);
         walking.push_back((part.mask & (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY)) == (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY));
      }
      const auto walk = [&](const float from, const float to, const bool toEnd) // |to - from| <= 360; the last step is left to the caller unless toEnd
      {
         const int steps = max(1, static_cast<int>(ceilf(fabsf(to - from))));
         for (int i = 1; i < (toEnd ? steps + 1 : steps); i++)
         {
            const XformOp op = turn(from + (to - from) * static_cast<float>(i) / static_cast<float>(steps));
            for (size_t j = 0; j < m_lastOp.xform.parts.size(); j++)
               if (walking[j])
               {
                  vec3 unusedPos, unusedScale, rot;
                  ExtractTRS(TurnedAxes(m_lastOp.xform.parts[j], op), unusedPos, unusedScale, rot, &reference[j]);
                  reference[j] = rot;
               }
         }
      };
      const int turns = static_cast<int>(truncf(angle / 360.f)); // |turns| <= 100
      const float full = turns > 0 ? 360.f : -360.f;
      if (turns != 0)
      {
         const vector<vec3> start(reference);
         walk(0.f, full, true);
         for (size_t j = 0; j < reference.size(); j++)
         {
            if (!walking[j])
               continue;
            const vec3 d = reference[j] - start[j];
            const vec3 whole(360.f * roundf(d.x / 360.f), 360.f * roundf(d.y / 360.f), 360.f * roundf(d.z / 360.f));
            if (fabsf(d.x - whole.x) < 0.01f && fabsf(d.y - whole.y) < 0.01f && fabsf(d.z - whole.z) < 0.01f)
            {
               reference[j] = start[j] + whole * static_cast<float>(abs(turns));
               walking[j] = false;
            }
         }
         for (int t = 1; t < abs(turns); t++)
            walk(full * static_cast<float>(t), full * static_cast<float>(t + 1), true);
         for (size_t j = 0; j < walking.size(); j++)
            walking[j] = (m_lastOp.xform.parts[j].mask & (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY)) == (EditorUIPart::TM_RotX | EditorUIPart::TM_RotY);
      }
      walk(full * static_cast<float>(abs(turns)), angle, false);
      ApplySelectionXform(m_lastOp.xform, turn(angle), &reference);
      return;
   }
   case LastOperation::Type::Scale:
   {
      const XformOp op = OrientedXform(XformKind::Scale, m_lastOp.orientation, m_lastOp.viewFrame, m_lastOp.axis, m_lastOp.plane, m_lastOp.factor, vec3(0.f, 0.f, 0.f), m_lastOp.xform.parts.size());
      ApplySelectionXform(m_lastOp.xform, op);
      return;
   }
   case LastOperation::Type::RubberBetweenPosts:
   {
      // The created rubber is the selection: refit it in place (the undo record of its creation is unchanged)
      IEditable *const edit = m_lastOp.selection.empty() ? nullptr : m_lastOp.selection.front()->GetEditable();
      if (edit == nullptr || edit->GetItemType() != eItemRubber)
         return;
      Rubber *const rubber = static_cast<Rubber *>(edit);
      const RubberData before = rubber->m_d;
      rubber->m_d.m_thickness = m_lastOp.rbThickness;
      rubber->m_d.m_height = m_lastOp.rbHeight;
      rubber->m_d.m_hitHeight = m_lastOp.rbHeight;
      rubber->m_d.m_postRoundness = m_lastOp.rbRoundness;
      if (!RubberBetweenPosts::Refit(*this, rubber)) // Reports why if no path fits: keep the shape and the values it was fitted with
      {
         rubber->m_d.m_thickness = before.m_thickness;
         rubber->m_d.m_height = before.m_height;
         rubber->m_d.m_hitHeight = before.m_hitHeight;
         rubber->m_d.m_postRoundness = before.m_postRoundness;
      }
      return;
   }
   case LastOperation::Type::Wireform:
      if (m_lastOp.selection.empty() || m_lastOp.selection.front()->GetEditable()->GetItemType() != eItemPrimitive)
         return;
      {
         Primitive *const prim = static_cast<Primitive *>(m_lastOp.selection.front()->GetEditable());
         prim->m_d.m_wireHeight = m_lastOp.wireHeight;
         prim->m_d.m_wireThickness = m_lastOp.thickness;
         prim->m_d.m_wireStandHeight = m_lastOp.standHeight;
         prim->m_d.m_wireBendRadius = m_lastOp.bendRadius;
         prim->m_d.m_wireEnds = m_lastOp.ends;
         prim->m_d.m_wireRails = m_lastOp.rails;
         prim->m_d.m_wireSpacing = m_lastOp.spacing;
         prim->UpdateWire();
      }
      return;
   case LastOperation::Type::WireRail:
      if (m_lastOp.selection.empty() || m_lastOp.selection.front()->GetEditable()->GetItemType() != eItemPrimitive)
         return;
      {
         Primitive *const prim = static_cast<Primitive *>(m_lastOp.selection.front()->GetEditable());
         prim->SetRailWires(WireRail::ResizeWires(prim->GetRailWires(), clamp(m_lastOp.rails, 1, WireRail::kMaxWires), prim->m_d.m_railThirdRight));
         prim->m_d.m_wireThickness = clamp(m_lastOp.railDiameter, 0.1f, 50.f) * 0.5f;
         const auto &points = prim->m_wireCurve.GetPoints();
         const float dz = points.empty() ? 0.f : m_lastOp.wireHeight - points.front()->GetZ();
         for (const auto &point : points)
            point->SetZ(point->GetZ() + dz);
         prim->UpdateWire();
      }
      return;
   case LastOperation::Type::PlasticFromAtlas:
      if (m_lastOp.selection.empty() || m_lastOp.selection.front()->GetEditable()->GetItemType() != eItemPrimitive)
         return;
      PlasticFromAtlas::Adjust(static_cast<Primitive *>(m_lastOp.selection.front()->GetEditable()), m_lastOp.plHeight, m_lastOp.plThickness, m_lastOp.plBevel, m_lastOp.plSegs, m_lastOp.plHoleDia,
         m_lastOp.plDpi, m_lastOp.plDetail);
      return;
   case LastOperation::Type::None: return;
   }
   SetSelectionTransform(result);
}

bool EditorUI::RenderAdjustParameters()
{
   bool changed = false;
   const float width = 220.f * m_liveUI.GetDPI();
   // Esc while one of the fields is being edited cancels the edit (a typed field clears its own active id on Esc, so it is tested before the widgets),
   // it must not reach the editor (deselect)
   if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ImGui::IsAnyItemActive())
      m_paneEscPending = true;
   // Same colors as the editor fields
   ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::V(EditorTheme::Inactive));
   ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, EditorTheme::V(EditorTheme::Hover));
   ImGui::PushStyleColor(ImGuiCol_FrameBgActive, EditorTheme::V(EditorTheme::Raised));
   ImGui::PushStyleColor(ImGuiCol_SliderGrab, EditorTheme::V(EditorTheme::Fill));
   ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, EditorTheme::V(EditorTheme::FillActive));
   ImGui::PushItemWidth(width);
   const auto row = [dpi = m_liveUI.GetDPI()](const char *label)
   {
      ImGui::AlignTextToFramePadding();
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
      ImGui::TextUnformatted(label);
      ImGui::PopStyleColor();
      ImGui::SameLine(84.f * dpi);
   };
   // X / Y / Z drag fields with a letter cue; values that display as zero are shown as 0.00, never "-0.00" (the stored value is only written back when edited)
   const auto dragVec = [width](const char *id, float *v, const int n)
   {
      bool edited = false;
      ImGui::BeginGroup(); // One item for the tooltip
      static constexpr const char *formats[] = { "X  %.2f", "Y  %.2f", "Z  %.2f" };
      const float w = (width - ImGui::GetStyle().ItemSpacing.x * static_cast<float>(n - 1)) / static_cast<float>(n);
      for (int i = 0; i < n; i++)
      {
         float shown = fabsf(v[i]) < 0.005f ? 0.f : v[i];
         if (i > 0)
            ImGui::SameLine();
         ImGui::SetNextItemWidth(w);
         if (ImGui::DragFloat((string(id) + std::to_string(i)).c_str(), &shown, 1.f, 0.f, 0.f, formats[i]))
         {
            v[i] = shown;
            edited = true;
         }
      }
      ImGui::EndGroup();
      return edited;
   };
   // Whose axes X / Y / Z are (View: the screen's when the operation was done)
   const auto orientationCombo = [&]()
   {
      row("Orientation");
      static constexpr const char *orientations[] = { "Global", "Local", "Normal", "View" };
      int orientation = static_cast<int>(m_lastOp.orientation);
      if (!ImGui::Combo("##Orientation", &orientation, orientations, 4))
         return false;
      m_lastOp.orientation = static_cast<TransformOrientation>(orientation);
      return true;
   };
   switch (m_lastOp.type)
   {
   case LastOperation::Type::Move:
      row("Delta");
      changed |= dragVec("##Delta", &m_lastOp.offset.x, 3);
      ImGui::SetItemTooltip(m_lastOp.ownFrame ? "X, Y, Z in VP units, along each part's own axes (Local)" : "X, Y, Z in VP units");
      break;
   case LastOperation::Type::Paste:
   case LastOperation::Type::Duplicate:
      row("Offset");
      changed |= dragVec("##Offset", &m_lastOp.offset.x, 3);
      ImGui::SetItemTooltip("X, Y, Z in VP units, from the %s", m_lastOp.type == LastOperation::Type::Duplicate ? "duplicated parts" : "copied parts");
      break;
   case LastOperation::Type::Add:
      row("Position");
      changed |= dragVec("##Position", &m_lastOp.offset.x, 2);
      ImGui::SetItemTooltip("X, Y in VP units");
      break;
   case LastOperation::Type::Wireform:
   {
      row("Height");
      changed |= ImGui::DragFloat("##WHgt", &m_lastOp.wireHeight, 0.1f, -1000.f, 2000.f, "%.2f");
      row("Thickness");
      changed |= ImGui::DragFloat("##WThk", &m_lastOp.thickness, 0.1f, 0.1f, 50.f, "%.2f");
      row("Stand Height");
      changed |= ImGui::DragFloat("##WStH", &m_lastOp.standHeight, 0.1f, 0.f, 1000.f, "%.2f");
      row("Bend Radius");
      changed |= ImGui::DragFloat("##WBnd", &m_lastOp.bendRadius, 0.1f, 0.f, 200.f, "%.2f");
      row("Ends");
      static constexpr const char *ends[] = { "Stands", "Round", "Flat", "None" };
      changed |= ImGui::Combo("##WEnds", &m_lastOp.ends, ends, 4);
      row("Rails");
      changed |= ImGui::SliderInt("##WRails", &m_lastOp.rails, 1, 3);
      if (m_lastOp.rails > 1)
      {
         row("Spacing");
         changed |= ImGui::DragFloat("##WSpc", &m_lastOp.spacing, 0.1f, 0.f, 200.f, "%.2f");
      }
      break;
   }
   case LastOperation::Type::WireRail:
   {
      row("Height");
      changed |= ImGui::DragFloat("##RlHgt", &m_lastOp.wireHeight, 0.5f, -1000.f, 2000.f, "%.1f");
      row("Rails");
      changed |= ImGui::SliderInt("##RlRails", &m_lastOp.rails, 1, WireRail::kMaxWires);
      row("Wire Diameter");
      changed |= ImGui::DragFloat("##RlDia", &m_lastOp.railDiameter, 0.05f, 0.1f, 50.f, "%.2f");
      break;
   }
   case LastOperation::Type::PlasticFromAtlas:
   {
      row("Height");
      changed |= ImGui::DragFloat("##PlHgt", &m_lastOp.plHeight, 0.1f, -1000.f, 2000.f, "%.2f");
      row("Thickness");
      changed |= ImGui::DragFloat("##PlThk", &m_lastOp.plThickness, 0.05f, 0.1f, 100.f, "%.2f");
      row("Bevel");
      changed |= ImGui::DragFloat("##PlBvl", &m_lastOp.plBevel, 0.05f, 0.f, 100.f, "%.2f");
      row("Bevel Segments");
      changed |= ImGui::SliderInt("##PlSeg", &m_lastOp.plSegs, 1, 8);
      row("Curve Detail");
      changed |= ImGui::SliderInt("##PlDtl", &m_lastOp.plDetail, 1, 5);
      row("Hole Diameter");
      changed |= ImGui::DragFloat("##PlHole", &m_lastOp.plHoleDia, 0.1f, 0.5f, 100.f, "%.1f");
      row("Sheet DPI");
      changed |= ImGui::DragFloat("##PlDpi", &m_lastOp.plDpi, 0.5f, 10.f, 2400.f, "%.1f");
      break;
   }
   case LastOperation::Type::Rotate:
   {
      row("Angle");
      changed |= ImGui::DragFloat("##Angle", &m_lastOp.angle, 0.5f, -kMaxLastOpAngle, kMaxLastOpAngle, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
      row("Axis");
      static constexpr const char *axes[] = { "View", "X", "Y", "Z" };
      int axis = m_lastOp.axis + 1;
      if (ImGui::Combo("##Axis", &axis, axes, 4))
      {
         m_lastOp.axis = axis - 1;
         changed = true;
      }
      if (m_lastOp.axis >= 0)
         changed |= orientationCombo();
      break;
   }
   case LastOperation::Type::Scale:
   {
      row("Factor");
      changed |= ImGui::DragFloat("##Factor", &m_lastOp.factor, 0.01f, 0.001f, 1000.f, "%.3f");
      row("Axis");
      static constexpr const char *axes[] = { "All", "X", "Y", "Z" };
      int axis = m_lastOp.axis + 1;
      if (ImGui::Combo("##Axis", &axis, axes, 4))
      {
         m_lastOp.axis = axis - 1;
         changed = true;
      }
      if (m_lastOp.axis >= 0)
      {
         row("");
         changed |= ImGui::Checkbox("Perpendicular plane", &m_lastOp.plane);
         changed |= orientationCombo();
      }
      break;
   }
   case LastOperation::Type::RubberBetweenPosts:
   {
      row("Thickness");
      changed |= ImGui::DragInt("##RbThickness", &m_lastOp.rbThickness, 0.2f, 1, 200);
      row("Height");
      changed |= ImGui::DragFloat("##RbHeight", &m_lastOp.rbHeight, 0.5f, 0.f, 0.f, "%.1f");
      row("Roundness");
      changed |= ImGui::SliderFloat("##RbRoundness", &m_lastOp.rbRoundness, 0.f, 1.f, "%.2f");
      break;
   }
   case LastOperation::Type::None: break;
   }
   ImGui::PopItemWidth();
   ImGui::PopStyleColor(5);
   return changed;
}

void EditorUI::RenderAdjustPanel()
{
   if (m_measure.IsActive())
   {
      m_adjustPopupOpen = false; // Under the Measure tool the panel is off (a popup already open closes); the last operation stays adjustable afterwards
      return;
   }
   // The operation can be adjusted as long as it is the last undo step and the selection is the one it was done on
   if (m_lastOp.type != LastOperation::Type::None
      && (m_modal.active || m_pointEditPart != nullptr || m_table->IsLocked() || m_undo.TopRecordId() != m_lastOp.undoId || m_multiSel != m_lastOp.selection))
      m_lastOp.type = LastOperation::Type::None;
   if (m_lastOp.type == LastOperation::Type::None)
   {
      m_adjustPopupOpen = false;
      return;
   }
   bool changed = false;
   if (m_viewportVisible && !m_adjustPopupOpen) // The floating panel belongs to the viewport area; the F9 popup below does not (and replaces the panel while open)
   {
      const float dpi = m_liveUI.GetDPI();
      const char *const name = LastOperationName(m_lastOp.type);

      // Collapsible panel at the bottom left of the viewport area (above the status bar)
      ImGui::SetNextWindowPos(ImVec2(m_viewportContentRect.x0 + 8.f * dpi, m_viewportContentRect.y1 - 8.f * dpi), ImGuiCond_Always, ImVec2(0.f, 1.f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * dpi, 6.f * dpi));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
      ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::V(EditorTheme::Border));
      if (ImGui::Begin("Adjust Last Operation", nullptr,
             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
                | ImGuiWindowFlags_NoMove))
      {
         // Header: a triangle (right when collapsed, down when expanded) and the name of the operation
         const ImVec2 headerPos = ImGui::GetCursorScreenPos();
         const ImVec2 textSize = ImGui::CalcTextSize(name);
         const float headerWidth = max(220.f * dpi, textSize.x + 28.f * dpi);
         if (ImGui::InvisibleButton("##AdjustHeader", ImVec2(headerWidth, ImGui::GetFrameHeight())))
            m_lastOp.expanded = !m_lastOp.expanded;
         const bool hovered = ImGui::IsItemHovered();
         ImDrawList *const draw = ImGui::GetWindowDrawList();
         const float cy = headerPos.y + 0.5f * ImGui::GetFrameHeight();
         const float h = ImGui::GetFontSize() * 0.3f;
         if (m_lastOp.expanded)
            draw->AddTriangleFilled(ImVec2(headerPos.x + 2.f * dpi, cy - h * 0.6f), ImVec2(headerPos.x + 2.f * dpi + 2.f * h, cy - h * 0.6f),
               ImVec2(headerPos.x + 2.f * dpi + h, cy + h * 0.6f), EditorTheme::TextDim);
         else
            draw->AddTriangleFilled(ImVec2(headerPos.x + 2.f * dpi, cy - h), ImVec2(headerPos.x + 2.f * dpi, cy + h), ImVec2(headerPos.x + 2.f * dpi + h * 1.2f, cy), EditorTheme::TextDim);
         draw->AddText(ImVec2(headerPos.x + 20.f * dpi, cy - 0.5f * textSize.y), hovered ? EditorTheme::Text : EditorTheme::TextDim, name);
         ImGui::SetItemTooltip("Adjust Last Operation (F9)");
         if (m_lastOp.expanded)
         {
            ImGui::Separator();
            changed = RenderAdjustParameters();
         }
      }
      ImGui::End();
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar(2);
   }

   // F9 popup at the mouse
   if (m_adjustPopupOpen)
   {
      ImGui::SetNextWindowPos(m_adjustPopupPos, ImGuiCond_Appearing);
      bool open = true;
      ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
      if (ImGui::Begin("Adjust Last Operation##Popup", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
      {
         changed |= RenderAdjustParameters();
         if (!m_adjustPopupJustOpened && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            open = false;
      }
      ImGui::End();
      ImGui::PopStyleColor();
      m_adjustPopupJustOpened = false;
      if (!open)
         m_adjustPopupOpen = false;
   }

   if (changed)
      ApplyLastOperation();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// 3D cursor (Blender's): Shift+right click places it, Shift+S snaps the cursor to the selection / world origin / grid and the selection to the cursor / grid.
// View state of the editing session: not in the table file, no undo step, never drawn while the game plays.
//

void EditorUI::Place3DCursor(const ImVec2 &mousePos, const PickView *view)
{
   if (IsInspectMode())
      return;
   vec3 point;
   if (PickSurfacePoint(mousePos, point, view))
      m_cursor3D = point;
}

void EditorUI::DrawCursor3D(ImDrawList *const drawList, const EditorRenderContext &ctx, const ImVec2 &clipMin, const ImVec2 &clipMax) const
{
   if (IsInspectMode() || m_camMode == ViewMode::DesktopBackdrop)
      return;
   const ImVec2 c = ctx.Project(Vertex3Ds(m_cursor3D.x, m_cursor3D.y, m_cursor3D.z));
   if (c.x == FLT_MAX)
      return; // Behind the camera
   // Blender's look: a ring of red and white dashes with four crosshair ticks
   const float dpi = m_liveUI.GetDPI();
   const float radius = 10.f * dpi, tickIn = 4.f * dpi, tickOut = 7.f * dpi;
   constexpr int dashes = 24, steps = 3;
   drawList->PushClipRect(clipMin, clipMax, true);
   for (int pass = 0; pass < 2; pass++)
   {
      const float outlineWidth = pass == 0 ? 2.f * dpi : 0.f;
      for (int i = 0; i < dashes; i++)
      {
         ImVec2 points[steps + 1];
         for (int j = 0; j <= steps; j++)
         {
            const float angle = 2.f * static_cast<float>(M_PI) * (static_cast<float>(i * steps + j) / static_cast<float>(dashes * steps));
            points[j] = ImVec2(c.x + radius * cosf(angle), c.y + radius * sinf(angle));
         }
         const ImU32 color = pass == 0 ? IM_COL32(0, 0, 0, 200) : (i % 2 == 0 ? IM_COL32(235, 35, 35, 255) : IM_COL32(255, 255, 255, 255));
         drawList->AddPolyline(points, steps + 1, color, ImDrawFlags_None, 2.f * dpi + outlineWidth);
      }
   }
   for (int k = 0; k < 4; k++)
   {
      const float angle = static_cast<float>(M_PI) * 0.5f * static_cast<float>(k);
      const ImVec2 dir(cosf(angle), sinf(angle));
      const ImVec2 from(c.x + dir.x * (radius - tickIn), c.y + dir.y * (radius - tickIn)), to(c.x + dir.x * (radius + tickOut), c.y + dir.y * (radius + tickOut));
      drawList->AddLine(from, to, IM_COL32(0, 0, 0, 220), 3.5f * dpi);
      drawList->AddLine(from, to, IM_COL32(255, 255, 255, 255), 1.5f * dpi);
   }
   drawList->PopClipRect();
}

void EditorUI::DrawCursor3DInView(ImDrawList *const drawList, const PickView &view) const
{
   LiveRenderContext ctx(m_player, drawList, m_camMode, m_shadeMode, false);
   ctx.SetImageView(view.modelViewProj, view.rectMin, view.rectSize);
   DrawCursor3D(drawList, ctx, view.rectMin, ImVec2(view.rectMin.x + view.rectSize.x, view.rectMin.y + view.rectSize.y));
}

bool EditorUI::GetSelectionCenter(vec3 &center, const int viewSlot) const
{
   if (m_pointEditPart)
   {
      Matrix3D transform;
      if (!GetSelectionTransform(transform))
         return false;
      center = vec3(transform._41, transform._42, transform._43);
      return true;
   }
   const LocalViewParts &local = ViewLocalView(viewSlot);
   // Each axis is averaged over the parts which move along it (a part without Z reports a constant height: it must not pull the median)
   vec3 sum(0.f, 0.f, 0.f);
   int count[3] = { 0, 0, 0 };
   for (const auto &part : m_multiSel)
   {
      Matrix3D transform;
      if (!local.Allows(part->GetEditable()))
         continue;
      const int mask = static_cast<int>(part->GetTransform(transform));
      if ((mask & EditorUIPart::TM_TransX) != 0)
         sum.x += transform._41, count[0]++;
      if ((mask & EditorUIPart::TM_TransY) != 0)
         sum.y += transform._42, count[1]++;
      if ((mask & EditorUIPart::TM_TransZ) != 0)
         sum.z += transform._43, count[2]++;
   }
   if (count[0] + count[1] + count[2] == 0)
      return false;
   center = vec3(count[0] ? sum.x / static_cast<float>(count[0]) : 0.f, count[1] ? sum.y / static_cast<float>(count[1]) : 0.f, count[2] ? sum.z / static_cast<float>(count[2]) : 0.f);
   return true;
}

void EditorUI::Reset3DCursor()
{
   // The middle of the playfield, on it: a part added with an untouched cursor goes where the table centre was its default
   m_cursor3D = vec3(0.5f * (m_table->m_left + m_table->m_right), 0.5f * (m_table->m_top + m_table->m_bottom), 0.f);
}

bool EditorUI::CanRunCursorOp(const CursorOp op) const
{
   if (IsInspectMode())
      return false;
   if (m_measure.IsActive() && (op == CursorOp::SelectionToCursor || op == CursorOp::SelectionToCursorKeepOffset || op == CursorOp::SelectionToGrid))
      return false; // They move parts (the cursor itself can still be moved)
   switch (op)
   {
   case CursorOp::CursorToWorldOrigin:
   case CursorOp::CursorToGrid: return true;
   case CursorOp::CursorToSelected: return m_pointEditPart != nullptr || !m_multiSel.empty();
   case CursorOp::SelectionToCursor:
   case CursorOp::SelectionToCursorKeepOffset:
      if (m_camMode == ViewMode::DesktopBackdrop)
         return false; // The cursor is neither drawn nor placeable in that view: a move to it would be unseen
      [[fallthrough]];
   default: return !m_table->IsLocked() && m_fileOp == nullptr && !m_modal.active && m_pointEditPart == nullptr && !m_multiSel.empty();
   }
}

void EditorUI::RunCursorOp(const CursorOp op, const int viewSlot)
{
   if (!CanRunCursorOp(op))
   {
      if (m_measure.IsActive() && (op == CursorOp::SelectionToCursor || op == CursorOp::SelectionToCursorKeepOffset || op == CursorOp::SelectionToGrid))
         Report("Measure tool active");
      return;
   }
   const float grid = max(m_snapMove, 1e-3f); // The move increment of the snapping settings
   const auto snap = [grid](const float v) { return roundf(v / grid) * grid; };
   switch (op)
   {
   case CursorOp::CursorToWorldOrigin: m_cursor3D = vec3(0.f, 0.f, 0.f); return;
   case CursorOp::CursorToGrid: m_cursor3D = vec3(snap(m_cursor3D.x), snap(m_cursor3D.y), snap(m_cursor3D.z)); return;
   case CursorOp::CursorToSelected:
      if (vec3 center; GetSelectionCenter(center, viewSlot))
         m_cursor3D = center;
      else
         Report("3D cursor: nothing selected is shown in this view");
      return;
   default: break;
   }

   // The selection moves: refused, as G is, when the selection holds a part the view's Local View hides
   UpdateLocalViews();
   if (!SelectionInView(viewSlot))
   {
      Report("Local View: the selection has parts this view does not show");
      return;
   }
   vec3 center(0.f, 0.f, 0.f);
   if (op == CursorOp::SelectionToCursorKeepOffset && !GetSelectionCenter(center, viewSlot))
      return;
   struct Item
   {
      std::shared_ptr<EditorUIPart> part;
      Matrix3D transform;
   };
   vector<Item> items;
   for (const auto &part : m_multiSel)
   {
      Matrix3D transform;
      const int mask = static_cast<int>(part->GetTransform(transform));
      if (mask == EditorUIPart::TM_None)
         continue; // (a part group has no transform of its own)
      vec3 pos(transform._41, transform._42, transform._43), target = pos;
      switch (op)
      {
      case CursorOp::SelectionToCursor: target = m_cursor3D; break;
      case CursorOp::SelectionToCursorKeepOffset: target = pos + (m_cursor3D - center); break;
      default: target = vec3(snap(pos.x), snap(pos.y), snap(pos.z)); break; // Selection to Grid
      }
      // A part moves along the axes of its transform mask only (a part without Z keeps its height)
      if ((mask & EditorUIPart::TM_TransX) == 0)
         target.x = pos.x;
      if ((mask & EditorUIPart::TM_TransY) == 0)
         target.y = pos.y;
      if ((mask & EditorUIPart::TM_TransZ) == 0 || (part->GetEditable()->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part->GetEditable())->m_d.m_plasticGen))
         target.z = pos.z; // (a plastic keeps its Height: it moves in X and Y only)
      if (fabsf(target.x - pos.x) < 1e-4f && fabsf(target.y - pos.y) < 1e-4f && fabsf(target.z - pos.z) < 1e-4f)
         continue;
      transform._41 = target.x;
      transform._42 = target.y;
      transform._43 = target.z;
      items.push_back({ part, transform });
   }
   if (items.empty())
   {
      Report("Nothing to move");
      return;
   }
   const char *const label = op == CursorOp::SelectionToCursor ? "Selection to Cursor" : op == CursorOp::SelectionToCursorKeepOffset ? "Selection to Cursor (Keep Offset)" : "Selection to Grid";
   vector<IEditable *> touched;
   for (const Item &item : items)
      touched.push_back(item.part->GetEditable());
   m_undo.BeginUndo();
   for (const auto &part : m_multiSel) // All the selection: a selected rubber which did not move can still be detached from its posts below
      m_undo.MarkForUndo(part->GetEditable());
   MarkDependentsForUndo(SelectedEditables());
   m_undo.SetLabel(CountLabel(label, touched.size(), touched.front()->GetName()));
   m_undo.EndUndo();
   m_lastUndoPart = nullptr; // As the other paths which open a record: the next field edit starts its own
   m_lastUndoId = 0;
   m_lastOp.type = LastOperation::Type::None;
   for (const Item &item : items)
      ApplyPartTransform(item.part, item.transform, false, false, false);
   DetachSelectedRubbersFromUnselectedPosts();
   RefitSelectionDependents();
}

void EditorUI::RenderCursorMenu()
{
   // Called in the scope of the overlay window (OpenPopup and BeginPopup share it). A popup at the mouse rather than a pie: ImGui has none
   if (m_cursorMenuRequest)
   {
      m_cursorMenuRequest = false;
      ImGui::OpenPopup("3D Cursor Snap");
   }
   ImGui::SetNextWindowPos(m_cursorMenuPos, ImGuiCond_Appearing);
   if (!ImGui::BeginPopup("3D Cursor Snap"))
      return;
   if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      m_paneEscPending = true; // The Esc closing the menu is not an editor Esc (it would clear the selection)
   const int slot = m_cursorMenuSlot;
   const auto item = [this, slot](const char *name, const CursorOp op)
   {
      if (ImGui::MenuItem(name, nullptr, false, CanRunCursorOp(op)))
         RunCursorOp(op, slot);
   };
   ImGui::TextDisabled("Snap");
   ImGui::Separator();
   item("Cursor to Selected", CursorOp::CursorToSelected);
   item("Cursor to World Origin", CursorOp::CursorToWorldOrigin);
   item("Cursor to Grid", CursorOp::CursorToGrid);
   ImGui::Separator();
   item("Selection to Cursor", CursorOp::SelectionToCursor);
   item("Selection to Cursor (Keep Offset)", CursorOp::SelectionToCursorKeepOffset);
   item("Selection to Grid", CursorOp::SelectionToGrid);
   ImGui::EndPopup();
}

void EditorUI::PlaceNewPartAtCursor()
{
   const ItemTypeEnum type = m_addPartType;
   m_addPartType = eItemInvalid;
   const float z = m_cursor3D.z;
   CreatePart(type, Vertex2D(m_cursor3D.x, m_cursor3D.y), m_addWireform && type == eItemPrimitive, m_addWireRail && type == eItemPrimitive, z > 1e-4f ? &z : nullptr);
}

   }
