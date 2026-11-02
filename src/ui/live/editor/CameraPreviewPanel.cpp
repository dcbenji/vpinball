// license:GPLv3+

#include "core/stdafx.h"
#include "CameraPreviewPanel.h"

#include "IconsLucide.h"
#include "EditorTheme.h"
#include "renderer/Renderer.h"
#include "renderer/RenderTarget.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

namespace VPX::EditorUI
{

CameraPreviewPanel::CameraPreviewPanel(EditorUI &editor)
   : m_editor(editor)
{
}

void CameraPreviewPanel::NotHosted()
{
   if (!m_requested)
      return;
   m_requested = false;
   m_refresh = true;
   m_width = m_height = 0;
   m_pendingWidth = m_pendingHeight = m_pendingFrames = 0;
   if (m_editor.m_renderer)
      m_editor.m_renderer->ReleaseEditorPreview();
}

void CameraPreviewPanel::RenderInArea(const ImVec2 &pos, const ImVec2 &size)
{
   m_frame++;
   if (m_editor.BeginAreaWindow("CAMERAPREVIEW", pos, size))
   {
      // Header: editor type button, title, Live toggle and Refresh
      m_editor.RenderAreaTypeButton(EditorUI::AreaEditor::CameraPreview);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("Camera Preview");
      // Header laid out against the area's right edge, keeping the corner zone (area split / join) free
      const float dpi = m_editor.m_liveUI.GetDPI();
      const float cornerPad = 12.f * dpi;
      const float refreshW = ImGui::CalcTextSize(ICON_LC_REFRESH_CW).x + 2.f * ImGui::GetStyle().FramePadding.x;
      const float rightEdge = ImGui::GetWindowContentRegionMax().x - cornerPad;
      ImGui::SameLine();
      ImGui::Checkbox("Live", &m_live);
      ImGui::SetItemTooltip("Render the player view continuously (every 2nd frame). Off: only after a change (click Refresh).");
      ImGui::SameLine(max(ImGui::GetCursorPosX(), rightEdge - refreshW));
      if (ImGui::Button(ICON_LC_REFRESH_CW "##CameraPreviewRefresh"))
         m_refresh = true;
      ImGui::SetItemTooltip("Refresh");
      ImGui::Separator();

      // Letterboxed at the main window's aspect: the framing the player view has on the playfield window
      const ImGuiIO &io = ImGui::GetIO();
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      float aspect = (io.DisplaySize.y > 0.f) ? io.DisplaySize.x / io.DisplaySize.y : 16.f / 9.f;
      if (avail.y > avail.x * 1.2f && avail.y > 0.f)
         aspect = avail.x / avail.y; // Portrait area: a portrait player view filling it, not a thumbnail
      float imgW = avail.x, imgH = avail.x / aspect;
      if (imgH > avail.y)
      {
         imgH = avail.y;
         imgW = imgH * aspect;
      }
      if (imgW >= 8.f && imgH >= 8.f)
      {
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

         // Re-render on changes: any new undo step (part edits, view setup edits), and at half rate while something is being dragged
         const uint64_t undoId = m_editor.m_undo.TopRecordId();
         if (undoId != m_lastUndoId)
         {
            m_lastUndoId = undoId;
            m_refresh = true;
         }
         // An interactive edit (gizmo drag, G/R/S modal transform, field drag; not a focused text field) keeps one undo record from its start: render while it runs, and once when it ends
         const bool dragging = ImGuizmo::IsUsingAny() || m_editor.m_modal.active || (ImGui::IsAnyItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left));
         if (m_wasDragging && !dragging)
            m_refresh = true;
         m_wasDragging = dragging;
         if (m_refresh || ((m_live || dragging) && (m_frame & 1u) == 0))
         {
            m_editor.m_renderer->RequestEditorPreview(m_width, m_height);
            m_requested = true;
            m_refresh = false;
         }

         ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - imgW) * 0.5f, ImGui::GetCursorPosY() + 4.f * dpi)); // Top-anchored under the header
         if (RenderTarget *const rt = m_editor.m_renderer->GetEditorPreview(); rt != nullptr)
            ImGui::Image(rt->GetColorSampler(), ImVec2(imgW, imgH));
         else
            ImGui::Dummy(ImVec2(imgW, imgH));
         ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), EditorTheme::WorkspaceOff, 0.f, 0, 1.f);
      }
   }
   m_editor.EndAreaWindow();
}

}
