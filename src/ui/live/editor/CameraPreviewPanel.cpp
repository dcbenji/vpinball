// license:GPLv3+

#include "core/stdafx.h"
#include "CameraPreviewPanel.h"

#include "IconsLucide.h"
#include "EditorTheme.h"
#include "math/bbox.h"
#include "math/matrix.h"
#include "renderer/Renderer.h"
#include "renderer/RenderTarget.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "imgui/imgui_internal.h"
#if defined(ENABLE_BGFX)
#include "bgfx/bgfx.h"
#endif

namespace VPX::EditorUI
{

static_assert(AreaLayout::kMaxViewLeaves == Renderer::kMaxEditorViews, "one renderer view slot per Camera Preview leaf");

CameraPreviewPanel::CameraPreviewPanel(EditorUI &editor, const int slot)
   : m_editor(editor)
   , m_slot(slot)
{
}

void CameraPreviewPanel::NotHosted()
{
   m_leafId = -1; // Rebound (settings read from the leaf) when shown again
   if (!m_requested)
      return;
   m_requested = false;
   m_refresh = true;
   m_width = m_height = 0;
   m_pendingWidth = m_pendingHeight = m_pendingFrames = 0;
   if (m_editor.m_renderer)
      m_editor.m_renderer->ReleaseEditorView(m_slot);
}

void CameraPreviewPanel::Bind(const int leafId, const int previewView, const AreaViewSettings &settings)
{
   m_leafId = leafId;
   m_view = (previewView >= BG_DESKTOP && previewView < BG_INVALID) ? previewView : -1;
   m_settings = settings;
   m_refresh = true;
}

void CameraPreviewPanel::WriteBack()
{
   if (m_leafId < 0)
      return;
   m_editor.m_areas.SetPreviewView(m_leafId, m_view);
   m_editor.m_areas.SetViewSettings(m_leafId, m_settings);
}

namespace
{
   constexpr const char *kViewNames[] = { "Desktop", "Cabinet", "FSS" }; // Indexed by ViewSetupID (BG_DESKTOP, BG_FULLSCREEN, BG_FSS)
   constexpr const char *kViewJsonNames[] = { "desktop", "cabinet", "fss" };
}

void CameraPreviewPanel::SetView(const int view)
{
   const int v = (view >= BG_DESKTOP && view < BG_INVALID) ? view : -1;
   if (v != m_view)
   {
      m_view = v;
      m_refresh = true;
      WriteBack();
   }
}

const char *CameraPreviewPanel::ViewJsonName(const int view)
{
   return (view >= BG_DESKTOP && view < BG_INVALID) ? kViewJsonNames[view] : nullptr;
}

int CameraPreviewPanel::ViewFromJsonName(const char *const name)
{
   if (name != nullptr)
      for (int i = 0; i < BG_INVALID; i++)
         if (strcmp(name, kViewJsonNames[i]) == 0)
            return i;
   return -1;
}

// Turntable orbit camera, table space (VPX units, Z up): yaw 0 looks from the front (the player side, +Y), pitch 90 from the top
void CameraPreviewPanel::CameraVectors(float dir[3], float up[3], float right[3]) const
{
   const float yaw = ANGTORAD(m_settings.yaw), pitch = ANGTORAD(m_settings.pitch);
   dir[0] = sinf(yaw) * cosf(pitch);
   dir[1] = cosf(yaw) * cosf(pitch);
   dir[2] = sinf(pitch);
   up[0] = -sinf(yaw) * sinf(pitch);
   up[1] = -cosf(yaw) * sinf(pitch);
   up[2] = cosf(pitch);
   right[0] = cosf(yaw);
   right[1] = -sinf(yaw);
   right[2] = 0.f;
}

void CameraPreviewPanel::ComputeOrbit(const float aspect, Matrix3D &view, Matrix3D &proj) const
{
   float dir[3], up[3], right[3];
   CameraVectors(dir, up, right);
   const float d = m_settings.distance;
   const float *const t = m_settings.target;
   // The live editor camera's convention (EditorUI): right handed look at in a table space mirrored along Z, then converted to VPX's left
   // handed view / projection pair (as Renderer::SetLookAtCamera)
   const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
   const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
   const Matrix3D camView = Matrix3D::MatrixLookAtRH(Vertex3Ds(t[0] + dir[0] * d, t[1] + dir[1] * d, -(t[2] + dir[2] * d)), Vertex3Ds(t[0], t[1], -t[2]), Vertex3Ds(up[0], up[1], -up[2]));
   Matrix3D camProj;
   if (!m_settings.ortho)
   {
      const float zNear = clamp(d * 0.05f, 5.f, 250.f); // As the editor view
      camProj = Matrix3D::MatrixPerspectiveFovRH(m_editor.m_navFovY, aspect, zNear, 50000.f);
   }
   else
   {
      // Same view height at the target as the perspective camera, so numpad 5 keeps the framing
      const float viewHeight = d * tanf(0.5f * ANGTORAD(m_editor.m_navFovY)), viewWidth = viewHeight * aspect;
      camProj = Matrix3D::MatrixOrthoOffCenterRH(-viewWidth, viewWidth, -viewHeight, viewHeight, 0.5f, -50000.f);
   }
   view = RH2LH * camView * YAxis;
   proj = YAxis * camProj;
}

void CameraPreviewPanel::FrameBounds(const FRect3D &bounds, const float aspect)
{
   // As the editor view's FrameBounds: the camera orientation is kept, the distance fits the bounds corners (whole image, no UI margin)
   float dir[3], up[3], right[3];
   CameraVectors(dir, up, right);
   const float c[3] = { 0.5f * (bounds.left + bounds.right), 0.5f * (bounds.top + bounds.bottom), 0.5f * (bounds.zlow + bounds.zhigh) };
   const float tanHalfFovY = tanf(0.5f * ANGTORAD(m_editor.m_navFovY));
   float distance = 0.f;
   for (int i = 0; i < 8; i++)
   {
      const float v[3] = { ((i & 1) ? bounds.right : bounds.left) - c[0], ((i & 2) ? bounds.bottom : bounds.top) - c[1], ((i & 4) ? bounds.zhigh : bounds.zlow) - c[2] };
      const float dx = fabsf(right[0] * v[0] + right[1] * v[1] + right[2] * v[2]);
      const float dy = fabsf(up[0] * v[0] + up[1] * v[1] + up[2] * v[2]);
      const float dz = dir[0] * v[0] + dir[1] * v[1] + dir[2] * v[2];
      distance = max(distance, m_settings.ortho ? max(dy, dx / aspect) / tanHalfFovY : dz + max(dy / tanHalfFovY, dx / (tanHalfFovY * aspect)));
   }
   for (int i = 0; i < 3; i++)
      m_settings.target[i] = c[i];
   if (distance > 0.f) // Keep the current distance for degenerate (point) selections
      m_settings.distance = distance * 1.1f;
   if (!(m_settings.distance > 0.f))
      m_settings.distance = 1000.f;
   m_settings.orbitValid = true;
   m_camMoved = true;
}

void CameraPreviewPanel::FrameAll(const float aspect)
{
   const PinTable *const table = m_editor.m_table;
   FrameBounds(FRect3D(0.f, table->m_right, 0.f, table->m_bottom, 0.f, table->m_glassTopHeight), aspect);
}

bool CameraPreviewPanel::GameOwnsInput() const
{
   // As the editor's game keys: inspecting the running game, keys and mouse are the game's
   return m_editor.IsInspectMode() && m_editor.m_player->IsGameRunning();
}

void CameraPreviewPanel::HandleOrbitInput(const ImVec2 &imgMin, const ImVec2 &imgSize, const bool hovered, const bool active)
{
   (void)imgMin;
   if (GameOwnsInput())
   {
      m_navDragging = false;
      return;
   }
   const ImGuiIO &io = ImGui::GetIO();
   const EditorUI &e = m_editor;
   const float halfH = m_settings.distance * tanf(0.5f * ANGTORAD(e.m_navFovY)); // View half height at the target (perspective and orthographic)
   const float unitsPerPixel = 2.f * halfH / max(1.f, imgSize.y);
   float dir[3], up[3], right[3];
   CameraVectors(dir, up, right);
   const auto pan = [&](const float dx, const float dy) {
      for (int i = 0; i < 3; i++)
         m_settings.target[i] += (-right[i] * dx + up[i] * dy) * unitsPerPixel;
      m_camMoved = true;
   };

   // Wheel: the editor view's bindings (trackpad preference: two finger scroll pans, Ctrl / Cmd + scroll zooms), zoom about the view target
   if (hovered && !active && (io.MouseWheel != 0.f || io.MouseWheelH != 0.f))
   {
      if (e.m_navTrackpad && !io.KeyCtrl && !io.KeySuper)
         pan(-io.MouseWheelH * 20.f, io.MouseWheel * 20.f);
      else if (io.MouseWheel != 0.f)
      {
         m_settings.distance *= powf(1.1f, (e.m_navInvertZoom ? 1.f : -1.f) * io.MouseWheel);
         m_settings.distance = clamp(m_settings.distance, 1.f, 40000.f);
         m_camMoved = true;
      }
   }

   // Middle drag orbits, Shift + middle drag pans (Alt / Option + left drag acts as the middle button with the preference on, the orbit lock pans)
   const ImGuiMouseButton navButton = (e.m_navAltMiddleButton && io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ? ImGuiMouseButton_Left : ImGuiMouseButton_Middle;
   if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
      m_navDragging = false;
   else if (active && !m_navDragging && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (navButton == ImGuiMouseButton_Left && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
      m_navDragging = true;
   if (m_navDragging && ImGui::IsMouseDown(navButton))
   {
      const ImVec2 drag = io.MouseDelta;
      if (drag.x != 0.f || drag.y != 0.f)
      {
         if (io.KeyShift || e.m_orbitLock)
            pan(drag.x, drag.y);
         else
         {
            m_settings.yaw -= drag.x * 0.01f * e.m_navOrbitSensitivity * (180.f / static_cast<float>(M_PI));
            m_settings.pitch = clamp(m_settings.pitch + drag.y * 0.01f * e.m_navOrbitSensitivity * (180.f / static_cast<float>(M_PI)), -90.f, 90.f);
            m_settings.yaw = fmodf(m_settings.yaw, 360.f);
            m_camMoved = true;
         }
      }
   }
}

void CameraPreviewPanel::HandleViewKeys(const float aspect)
{
   // The view commands of the editor view, for the view under the mouse (the editor view ignores them meanwhile, see m_secondaryViewHovered)
   const ImGuiIO &io = ImGui::GetIO();
   if (io.WantTextInput || m_editor.m_fileOp != nullptr || m_editor.m_modal.active || GameOwnsInput())
      return;
   EditorUI &e = m_editor;
   if (e.IsNumpadPressed(0))
   {
      m_settings.freeOrbit = !m_settings.freeOrbit; // Blender's camera view toggle: the Player view, or back to the orbit camera
      m_refresh = true;
      return;
   }
   if (!m_settings.freeOrbit)
      return;
   const auto setView = [this](const float yaw, const float pitch) {
      m_settings.yaw = yaw;
      m_settings.pitch = pitch;
      m_camMoved = true;
   };
   if (e.IsNumpadPressed(5))
   {
      m_settings.ortho = !m_settings.ortho;
      m_camMoved = true;
   }
   else if (e.IsNumpadPressed(7))
      setView(0.f, io.KeyCtrl ? -90.f : 90.f);
   else if (e.IsNumpadPressed(1))
      setView(io.KeyCtrl ? 180.f : 0.f, 0.f);
   else if (e.IsNumpadPressed(3))
      setView(io.KeyCtrl ? 90.f : -90.f, 0.f);
   else if (e.IsNumpadPressed(4) || e.IsNumpadPressed(6))
      setView(fmodf(m_settings.yaw + (e.IsNumpadPressed(4) ? -15.f : 15.f), 360.f), m_settings.pitch);
   else if (e.IsNumpadPressed(8) || e.IsNumpadPressed(2))
      setView(m_settings.yaw, clamp(m_settings.pitch + (e.IsNumpadPressed(8) ? 15.f : -15.f), -90.f, 90.f));
   else if (ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal) || (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift))
   {
      FRect3D bounds;
      if (e.GetSelectionBounds(bounds))
         FrameBounds(bounds, aspect); // Frame Selected
   }
   else if (ImGui::IsKeyPressed(ImGuiKey_Home))
      FrameAll(aspect);
}

void CameraPreviewPanel::RenderInArea(const ImVec2 &pos, const ImVec2 &size)
{
   m_camMoved = false;
   m_hasImage = false;
   const AreaViewSettings before = m_settings;
   const int viewBefore = m_view;
   char windowName[32];
   snprintf(windowName, sizeof(windowName), "CAMERAPREVIEW%d", m_slot);
   if (m_editor.BeginAreaWindow(windowName, pos, size))
   {
      // Header: editor type button, camera, view setup (Player camera), shading, Live toggle and Refresh
      m_editor.RenderAreaTypeButton(EditorUI::AreaEditor::CameraPreview, m_leafId);
      const float dpi = m_editor.m_liveUI.GetDPI();
      const ImGuiStyle &style = ImGui::GetStyle();
      const float cornerPad = 12.f * dpi; // Header laid out against the area's right edge, keeping the corner zone (area split / join) free
      const float refreshW = ImGui::CalcTextSize(ICON_LC_REFRESH_CW).x + 2.f * style.FramePadding.x;
      const float rightEdge = ImGui::GetWindowContentRegionMax().x - cornerPad;
      const auto comboWidth = [&](const char *widest) { return ImGui::CalcTextSize(widest).x + ImGui::GetFrameHeight() + 2.f * style.FramePadding.x; };
      // Next header item: on the same row when it fits before the Refresh button, else on a new row (narrow areas)
      const auto nextItem = [&](const float width) {
         ImGui::SameLine();
         if (ImGui::GetCursorPosX() + width > rightEdge - refreshW - style.ItemSpacing.x)
            ImGui::NewLine();
      };

      nextItem(comboWidth("Free Orbit"));
      ImGui::SetNextItemWidth(comboWidth("Free Orbit"));
      if (ImGui::BeginCombo("##ViewCamera", m_settings.freeOrbit ? "Free Orbit" : "Player"))
      {
         if (ImGui::Selectable("Player", !m_settings.freeOrbit))
            m_settings.freeOrbit = false;
         ImGui::SetItemTooltip("The player view: a view setup of the table, as the player shows it");
         if (ImGui::Selectable("Free Orbit", m_settings.freeOrbit))
            m_settings.freeOrbit = true;
         ImGui::SetItemTooltip("Its own orbit camera: middle drag orbits, Shift + middle drag pans, wheel zooms, numpad views, F frames the selection, Home the table");
         ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("Camera (numpad 0 over the view toggles it)");

      // View setup: Auto (the player's) or a fixed one of the table's three view setups
      const int activeView = static_cast<int>(m_editor.m_table->GetViewMode());
      const int shownView = m_view >= 0 ? m_view : activeView;
      if (!m_settings.freeOrbit)
      {
         const string autoLabel = "Auto ("s + kViewNames[clamp(activeView, 0, 2)] + ')';
         const char *const comboLabel = m_view >= 0 ? kViewNames[m_view] : autoLabel.c_str();
         nextItem(comboWidth(autoLabel.c_str()));
         ImGui::SetNextItemWidth(comboWidth(autoLabel.c_str()));
         if (ImGui::BeginCombo("##CameraPreviewView", comboLabel))
         {
            if (ImGui::Selectable(autoLabel.c_str(), m_view < 0))
               SetView(-1);
            ImGui::SetItemTooltip("The view setup the player uses (the table's active view mode)");
            for (int i = 0; i < BG_INVALID; i++)
               if (ImGui::Selectable(kViewNames[i], m_view == i))
                  SetView(i);
            ImGui::EndCombo();
         }
         ImGui::SetItemTooltip("View setup shown: Desktop, Cabinet (fullscreen) or FSS. Does not change the table's or the player's view mode.");
      }
      if (shownView != m_lastShownView)
      {
         m_lastShownView = shownView;
         m_refresh = true;
      }

      nextItem(comboWidth("Wireframe"));
      ImGui::SetNextItemWidth(comboWidth("Wireframe"));
      if (ImGui::BeginCombo("##ViewShading", m_settings.wireframe ? "Wireframe" : "Shaded"))
      {
         if (ImGui::Selectable("Shaded", !m_settings.wireframe))
            m_settings.wireframe = false;
         ImGui::SetItemTooltip("Full shading, as the player renders it (at most 2 shaded views)");
         if (ImGui::Selectable("Wireframe", m_settings.wireframe))
            m_settings.wireframe = true;
         ImGui::SetItemTooltip("The editor's hidden line wireframe, without post processing (cheap)");
         ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("Shading");
      nextItem(ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Live").x);
      ImGui::Checkbox("Live", &m_settings.live);
      ImGui::SetItemTooltip("Render continuously (every 2nd frame, every 4th for Wireframe). Off: only after a change (click Refresh).");
      ImGui::SameLine(max(ImGui::GetCursorPosX(), rightEdge - refreshW));
      if (ImGui::Button(ICON_LC_REFRESH_CW "##CameraPreviewRefresh"))
         m_refresh = true;
      ImGui::SetItemTooltip("Refresh");
      ImGui::Separator();

      // Player view: always the player window's aspect, letterboxed in the area (top-anchored): the view setup frames (and, in window mode, rotates)
      // the table after the aspect, so any other aspect (e.g. a portrait one for a tall area) is not what the player shows and crops the table.
      // Free orbit: the whole area
      const ImGuiIO &io = ImGui::GetIO();
      ImVec2 avail = ImGui::GetContentRegionAvail();
      avail.y -= 4.f * dpi; // The image starts 4 px down: keep it inside the area
      float imgW = avail.x, imgH = avail.y;
      if (!m_settings.freeOrbit)
      {
         const float aspect = m_editor.m_renderer->GetDisplayAspectRatio();
         imgW = avail.x;
         imgH = avail.x / aspect;
         if (imgH > avail.y)
         {
            imgH = avail.y;
            imgW = imgH * aspect;
         }
      }
      const bool hoveredWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
      if (imgW >= 8.f && imgH >= 8.f)
      {
         const float aspect = imgW / imgH;
         if (m_settings.freeOrbit && !m_settings.orbitValid)
         {
            // First shown: the table seen from the front, 35 degrees up, framed
            m_settings.yaw = 0.f;
            m_settings.pitch = 35.f;
            FrameAll(aspect);
         }

         // Render size: the image size in framebuffer pixels, capped
         int w = static_cast<int>(roundf(imgW * io.DisplayFramebufferScale.x));
         int h = static_cast<int>(roundf(imgH * io.DisplayFramebufferScale.y));
         if (w > kMaxWidth)
         {
            h = static_cast<int>(roundf(static_cast<float>(h) * static_cast<float>(kMaxWidth) / static_cast<float>(w)));
            w = kMaxWidth;
         }
         if (w != m_pendingWidth || h != m_pendingHeight)
         {
            m_pendingWidth = w;
            m_pendingHeight = h;
            m_pendingFrames = 0;
         }
         else
            m_pendingFrames++;
         if ((m_pendingWidth != m_width || m_pendingHeight != m_height) && (m_width == 0 || m_pendingFrames >= kStableFrames))
         {
            m_width = m_pendingWidth;
            m_height = m_pendingHeight;
            m_refresh = true;
         }

         // The image, under an invisible button that takes this view's mouse input (no picking, no gizmo: P2 / P3)
         ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - imgW) * 0.5f, ImGui::GetCursorPosY() + 4.f * dpi)); // Top-anchored under the header
         ImGui::InvisibleButton("##ViewImage", ImVec2(imgW, imgH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
         const ImVec2 imgMin = ImGui::GetItemRectMin(), imgMax = ImGui::GetItemRectMax();
         const bool imgHovered = ImGui::IsItemHovered(), imgActive = ImGui::IsItemActive();
         if (m_settings.freeOrbit)
            HandleOrbitInput(imgMin, ImVec2(imgW, imgH), imgHovered, imgActive);
         if (hoveredWindow)
            HandleViewKeys(aspect);
         if (m_settings.freeOrbit && m_settings.orbitValid && aspect != m_lastAspect)
            m_refresh = true;
         m_lastAspect = aspect;

         // Re-render on changes: any new undo step (part edits, view setup edits), and at half rate while something is being dragged
         const uint64_t undoId = m_editor.m_undo.TopRecordId();
         if (undoId != m_lastUndoId)
         {
            m_lastUndoId = undoId;
            m_refresh = true;
         }
         // An interactive edit keeps one undo record from its start: render while it runs (on the schedule), and once when it ends
         const bool dragging = m_editor.IsSecondaryViewEditDrag(); // Same rule as the schedule's ranking (EditorUI::RenderAreas)
         if (m_wasDragging && !dragging)
            m_refresh = true;
         m_wasDragging = dragging;
         if (!m_settings.SameChoices(before) || m_view != viewBefore)
            m_refresh = true;
         Renderer *const renderer = m_editor.m_renderer.get();
         const bool pbrRefused = !m_settings.wireframe && !renderer->CanRenderEditorPBRView(m_slot);
         m_hasImage = true;
         // The views alternate on the half rate frames (2 shaded views cost one view per frame); a wireframe view renders every 4th frame (the editor's
         // wireframe draws every part twice, CPU side it costs more than a shaded view: ~8 ms a render on Police Force); the camera of this view
         // being moved renders every frame
         // One global frame counter, each view phased by its rank among the shown views of its shading (set by EditorUI::RenderAreas): shaded rank r on
         // the frames (F + r) even, so 2 shaded views never render on the same frame; wireframe rank w on the phases 1, 3 (mod 4, off the first shaded view's
         // frames), then 0, 2, so no two wireframe views share a frame
         static constexpr unsigned int kWirePhases[4] = { 1u, 3u, 0u, 2u };
         const unsigned int frame = static_cast<unsigned int>(ImGui::GetFrameCount());
         const bool scheduled = m_settings.wireframe ? (frame % 4u) == kWirePhases[static_cast<unsigned int>(m_rank) % 4u] : ((frame + static_cast<unsigned int>(m_rank)) % 2u) == 0;
         if (!pbrRefused && (m_refresh || m_camMoved || ((m_settings.live || dragging) && scheduled)))
         {
            Renderer::EditorViewRequest req;
            req.width = m_width;
            req.height = m_height;
            req.playerCamera = !m_settings.freeOrbit;
            req.viewSetup = m_view >= 0 ? static_cast<ViewSetupID>(m_view) : BG_INVALID;
            req.wireframe = m_settings.wireframe;
            if (m_settings.freeOrbit)
               ComputeOrbit(static_cast<float>(m_width) / static_cast<float>(max(1, m_height)), req.view, req.proj);
            if (renderer->RequestEditorView(m_slot, req))
            {
               m_requested = true;
               m_refresh = false;
            }
         }

         ImDrawList *const drawList = ImGui::GetWindowDrawList();
         if (RenderTarget *const rt = renderer->GetEditorView(m_slot); rt != nullptr && !pbrRefused)
         {
            // Render targets are stored bottom-left first on OpenGL / GLES (the scene shaders' TEX_V_IS_UP): flip them for ImGui there only
#if defined(ENABLE_BGFX)
            const bool flipV = bgfx::getCaps()->originBottomLeft;
#elif defined(ENABLE_OPENGL)
            const bool flipV = true;
#else
            const bool flipV = false;
#endif
            drawList->AddImage(rt->GetColorSampler(), imgMin, imgMax, ImVec2(0.f, flipV ? 1.f : 0.f), ImVec2(1.f, flipV ? 0.f : 1.f));
         }
         else
            drawList->AddRectFilled(imgMin, imgMax, IM_COL32(20, 20, 20, 255));
         if (pbrRefused)
         {
            const char *const msg = "2 shaded views at most: set this one to Wireframe";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            drawList->AddText(ImVec2(0.5f * (imgMin.x + imgMax.x - ts.x), 0.5f * (imgMin.y + imgMax.y - ts.y)), EditorTheme::TextDim, msg);
         }
         drawList->AddRect(imgMin, imgMax, imgHovered && m_settings.freeOrbit ? EditorTheme::Accent : EditorTheme::WorkspaceOff, 0.f, 0, 1.f);
         if (imgHovered && m_settings.freeOrbit)
            ImGui::SetMouseCursor(m_navDragging ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Arrow);
      }
      if (hoveredWindow)
         m_editor.m_secondaryViewHovered = true; // Keys go to the view under the mouse
   }
   m_editor.EndAreaWindow();
   if (!m_settings.SameChoices(before) || m_view != viewBefore || m_camMoved || before.orbitValid != m_settings.orbitValid)
      WriteBack();
}

}
