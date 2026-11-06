// license:GPLv3+

#include "core/stdafx.h"
#include "core/player.h"
#include "ReferenceImage.h"
#include "EditorTheme.h"
#include "LiveRenderContext.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "renderer/Renderer.h"
#include "renderer/RenderCommand.h"
#include "renderer/RenderPass.h"
#include "core/SettingsService.h"
#include "renderer/Texture.h"

#include <cmath>
#include <format>

#ifdef __STANDALONE__
#define _WINDOWS_
#endif
#include "FreeImage.h"

namespace VPX::EditorUI
{

namespace
{
constexpr int kMaxSecondaryViews = 3;
constexpr int kMaxImageSide = 16384; // Decoded in full and uploaded: a 600 dpi playfield scan is beyond it

// The pixel size of an image file from its header (FreeImage's header only load; a plugin without it loads the whole image). False if unknown
bool ReadHeaderSize(const std::filesystem::path &path, unsigned int &width, unsigned int &height)
{
   vector<uint8_t> data = read_file(path);
   if (data.empty() || data.size() > 0x7FFFFFFFu)
      return false;
   FIMEMORY *const mem = FreeImage_OpenMemory(data.data(), static_cast<DWORD>(data.size()));
   if (mem == nullptr)
      return false;
   const FREE_IMAGE_FORMAT fif = FreeImage_GetFileTypeFromMemory(mem, static_cast<int>(data.size()));
   FIBITMAP *const dib = fif != FIF_UNKNOWN && FreeImage_FIFSupportsReading(fif) ? FreeImage_LoadFromMemory(fif, mem, FIF_LOAD_NOPIXELS) : nullptr;
   FreeImage_CloseMemory(mem);
   if (dib == nullptr)
      return false;
   width = FreeImage_GetWidth(dib);
   height = FreeImage_GetHeight(dib);
   FreeImage_Unload(dib);
   return width > 0 && height > 0;
}

string Escape(const string &s)
{
   string o;
   for (const char c : s)
      if (c == '%')
         o += "%25";
      else if (c == ';')
         o += "%3B";
      else if (c != '\n' && c != '\r')
         o += c;
   return o;
}

string Unescape(const string &s)
{
   string o;
   for (size_t i = 0; i < s.size(); i++)
      if (s[i] == '%' && i + 2 < s.size() + 0 && s.compare(i, 3, "%25") == 0)
      {
         o += '%';
         i += 2;
      }
      else if (s[i] == '%' && i + 2 < s.size() + 0 && s.compare(i, 3, "%3B") == 0)
      {
         o += ';';
         i += 2;
      }
      else
         o += s[i];
   return o;
}

// The scan resolution a file name tells ("playfield_300dpi.png"), else 150 (as the plastic atlas)
float DpiFromName(string s)
{
   for (char &c : s)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
   for (size_t pos = s.find("dpi"); pos != string::npos; pos = s.find("dpi", pos + 1))
   {
      size_t end = pos;
      while (end > 0 && isdigit(static_cast<unsigned char>(s[end - 1])))
         end--;
      if (end < pos)
      {
         const float v = static_cast<float>(atof(s.substr(end, pos - end).c_str()));
         if (v >= 10.f && v <= 4800.f)
            return v;
      }
   }
   return 150.f;
}
}

ReferenceImage::~ReferenceImage()
{
   // The engine's texture manager only keeps the pointer as a key until the editor is released (Release)
   Reap(true);
   delete m_tex;
}

void ReferenceImage::Retire(EditorUI &ed, Texture *const tex)
{
   ed.m_renderer->m_renderDevice->m_texMan.UnloadTexture(tex);
   m_retired.emplace_back(tex, ImGui::GetFrameCount());
}

void ReferenceImage::Reap(const bool all)
{
   // A recorded frame holds the sampler, not the texture, but the upload may still read the image data: a few frames is plenty
   for (size_t i = 0; i < m_retired.size();)
      if (all || ImGui::GetFrameCount() - m_retired[i].second >= 4)
      {
         delete m_retired[i].first;
         m_retired.erase(m_retired.begin() + static_cast<std::ptrdiff_t>(i));
      }
      else
         i++;
}

void ReferenceImage::FlushPending(EditorUI &ed)
{
   if (m_saveDue && m_loadedFor == ed.m_table && !ed.IsSaveInProgress() && !ed.IsInspectMode())
      Save(ed);
}

void ReferenceImage::Release(EditorUI &ed)
{
   if (m_saveDue && !ed.IsSaveInProgress() && !ed.IsInspectMode())
      Save(ed);
   if (ed.m_renderer != nullptr && ed.m_renderer->m_renderDevice != nullptr)
   {
      if (m_tex != nullptr)
         ed.m_renderer->m_renderDevice->m_texMan.UnloadTexture(m_tex);
   }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Storage: the table's own settings file (<table>.ini), key Editor/ReferenceImage. "v1;file;dpi;scale;x;y;rot;z;opacity;front;show;main;view1;view2;view3"

void ReferenceImage::Save(EditorUI &ed)
{
   m_saveDue = false;
   string value;
   if (m_tex != nullptr)
      value = std::format("v1;{};{:.6g};{:.8g};{:.6g};{:.6g};{:.6g};{:.6g};{:.4g};{};{};{};{};{};{}", Escape(m_p.file), m_p.dpi, m_p.scale, m_p.x, m_p.y, m_p.rot, m_p.z, m_p.opacity,
         m_p.front ? 1 : 0, m_p.show ? 1 : 0, m_p.showMain ? 1 : 0, m_p.showView[0] ? 1 : 0, m_p.showView[1] ? 1 : 0, m_p.showView[2] ? 1 : 0);
   Settings &settings = ed.m_table->GetSettings();
   settings.SetEditor_ReferenceImage(value, true);
   if (!settings.GetIniPath().empty()) // A table never saved has no settings file yet
      settings.Save();
}

void ReferenceImage::FlushSave(EditorUI &ed)
{
   // Not on every step of a slider drag, not while a table save writes the same settings on its worker thread (retried after it), not in a play test
   if (m_saveDue && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ed.IsSaveInProgress() && !ed.IsInspectMode())
      Save(ed);
}

void ReferenceImage::EnsureLoaded(EditorUI &ed)
{
   if (m_loadedFor == ed.m_table)
      return;
   m_loadedFor = ed.m_table;
   if (m_tex != nullptr)
   {
      Retire(ed, m_tex);
      m_tex = nullptr;
   }
   m_p = Params();
   m_error.clear();
   m_pick = 0;
   m_saveDue = false;

   const string stored = ed.m_table->GetSettings().GetEditor_ReferenceImage();
   if (stored.empty())
      return;
   vector<string> f;
   for (size_t start = 0;;)
   {
      const size_t end = stored.find(';', start);
      f.push_back(stored.substr(start, end == string::npos ? string::npos : end - start));
      if (end == string::npos)
         break;
      start = end + 1;
   }
   if (f.size() < 15 || f[0] != "v1")
      return;
   const auto num = [&](const size_t i, const float def) {
      char *e = nullptr;
      const float v = strtof(f[i].c_str(), &e);
      return e != f[i].c_str() && std::isfinite(v) ? v : def;
   };
   Params p;
   p.file = Unescape(f[1]);
   p.dpi = std::clamp(num(2, 150.f), 10.f, 4800.f);
   p.scale = num(3, 0.f);
   p.x = num(4, 0.f);
   p.y = num(5, 0.f);
   p.rot = num(6, 0.f);
   p.z = num(7, 0.1f);
   p.opacity = std::clamp(num(8, 0.6f), 0.f, 1.f);
   p.front = num(9, 0.f) != 0.f;
   p.show = num(10, 1.f) != 0.f;
   p.showMain = num(11, 1.f) != 0.f;
   for (int i = 0; i < kMaxSecondaryViews; i++)
      p.showView[i] = num(12 + i, 1.f) != 0.f;
   if (!(p.scale > 0.f) || kVPUPerInch / p.scale < 10.f || kVPUPerInch / p.scale > 4800.f) // A hand edited or stale scale: the DPI tells it
      p.scale = kVPUPerInch / p.dpi;
   else
      p.dpi = kVPUPerInch / p.scale;
   m_p = p;
   if (m_p.file.empty() || !LoadFromFile(ed, m_p.file, true))
   {
      const string err = m_error;
      m_p = Params();
      m_error = err; // The panel says why the image of the table is not there (the setting is kept until something is loaded)
   }
}

bool ReferenceImage::LoadFromFile(EditorUI &ed, const string &file, const bool keepPlacement)
{
   std::filesystem::path path = PathFromString(file);
   if (!FileExists(path) && !ed.m_table->m_filename.empty())
   {
      // Moved with the table: the same file name next to it
      const std::filesystem::path beside = ed.m_table->m_filename.parent_path() / path.filename();
      if (FileExists(beside))
         path = beside;
   }
   if (!FileExists(path))
   {
      m_error = "Reference image not found: " + file;
      return false;
   }
   const int maxSide = std::min(kMaxImageSide, ed.m_renderer->GetMaxTextureSize());
   unsigned int hw = 0, hh = 0;
   if (ReadHeaderSize(path, hw, hh) && std::max(hw, hh) > static_cast<unsigned int>(maxSide)) // From the header: a huge scan is not decoded
   {
      m_error = std::format("Reference image too large ({} x {} px, the limit is {} px on the long side): downscale it", hw, hh, maxSide);
      return false;
   }
   Texture *const tex = Texture::CreateFromFile(path);
   if (tex == nullptr)
   {
      m_error = "Reference image can not be read (PNG, JPG, ...): " + file;
      return false;
   }
   const int w = static_cast<int>(tex->m_width.load()), h = static_cast<int>(tex->m_height.load());
   if (std::max(w, h) > maxSide)
   {
      delete tex; // Never given to the renderer
      m_error = std::format("Reference image too large ({} x {} px, the limit is {} px on the long side): downscale it", w, h, maxSide);
      return false;
   }
   if (m_tex != nullptr)
      Retire(ed, m_tex); // A recorded frame may still sample it
   m_tex = tex;
   m_texW = w;
   m_texH = h;
   m_error.clear();
   const string resolved = PathToUTF8(path);
   if (keepPlacement && resolved != file)
      m_saveDue = true; // Found beside the table: the setting follows the file
   m_p.file = resolved;
   if (!keepPlacement)
   {
      m_p.rot = 0.f; // A new image starts fresh (Blender)
      m_p.dpi = DpiFromName(PathToUTF8(path.filename()));
      ApplyDpi();
      CenterOnTable(ed);
   }
   return true;
}

bool ReferenceImage::OpenFile(EditorUI &ed, const string &file)
{
   EnsureLoaded(ed);
   if (!LoadFromFile(ed, file, false))
   {
      ed.Report(m_error);
      return false;
   }
   m_saveDue = true;
   m_panelOpen = true;
   return true;
}

const string &ReferenceImage::GetFile(EditorUI &ed)
{
   EnsureLoaded(ed);
   return m_p.file;
}

void ReferenceImage::CenterOnTable(EditorUI &ed)
{
   m_p.x = 0.5f * (ed.m_table->m_left + ed.m_table->m_right);
   m_p.y = 0.5f * (ed.m_table->m_top + ed.m_table->m_bottom);
}

void ReferenceImage::PickFile(EditorUI &ed)
{
   if (ed.m_player->m_playfieldWnd == nullptr)
      return;
   m_pendingFile = std::make_shared<string>();
   std::filesystem::path location = PathFromString(ed.m_table->GetSettings().GetRecentDir_ImportDir());
   if (location.empty() && !ed.m_table->m_filename.empty())
      location = ed.m_table->m_filename.parent_path();
   const string loc = PathToUTF8(location);
   const SDL_DialogFileFilter filters[] = { { "Images", "png;jpg;jpeg;bmp;webp;tif;tiff" } };
   SDL_ShowOpenFileDialog(
      [](void *userdata, const char *const *filelist, int)
      {
         auto *res = static_cast<std::shared_ptr<string> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingFile), ed.m_player->m_playfieldWnd->GetCore(), filters, 1, loc.empty() ? nullptr : loc.c_str(), false);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Placement

void ReferenceImage::ToTable(const float px, const float py, float &tx, float &ty) const
{
   const float a = m_p.rot * static_cast<float>(M_PI / 180.), c = cosf(a), s = sinf(a);
   const float dx = (px - 0.5f * static_cast<float>(m_texW)) * m_p.scale, dy = (py - 0.5f * static_cast<float>(m_texH)) * m_p.scale;
   tx = m_p.x + dx * c + dy * s;
   ty = m_p.y - dx * s + dy * c;
}

void ReferenceImage::ToPixel(const float tx, const float ty, float &px, float &py) const
{
   const float a = m_p.rot * static_cast<float>(M_PI / 180.), c = cosf(a), s = sinf(a);
   const float ox = tx - m_p.x, oy = ty - m_p.y;
   px = (ox * c - oy * s) / m_p.scale + 0.5f * static_cast<float>(m_texW);
   py = (ox * s + oy * c) / m_p.scale + 0.5f * static_cast<float>(m_texH);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// 3D pass

bool ReferenceImage::Drawable(EditorUI &ed, const int viewOrdinal)
{
   if (ed.IsInspectMode() || ed.m_renderer == nullptr) // Not in a play test
      return false;
   if (viewOrdinal < 0 && ed.m_camMode != ViewMode::EditorCam) // Not in the player's view (the preview camera), nor the backdrop
      return false;
   if (viewOrdinal >= 0 && ed.m_renderer->IsEditorPreviewPlayerCam()) // Nor a secondary view showing the table as the player sees it
      return false;
   EnsureLoaded(ed);
   if (m_tex == nullptr || !m_p.show || m_texW <= 0 || m_texH <= 0 || m_p.scale <= 0.f)
      return false;
   return viewOrdinal < 0 ? m_p.showMain : m_p.showView[std::min(viewOrdinal, kMaxSecondaryViews - 1)];
}

void ReferenceImage::Render3D(EditorUI &ed, const int viewOrdinal)
{
   if (!Drawable(ed, viewOrdinal))
      return;
   Renderer *const renderer = ed.m_renderer.get();
   RenderDevice *const rd = renderer->m_renderDevice;
   Shader *const shader = rd->m_basicShader;

   Vertex3D_NoTex2 v[4]; // Triangle strip: top left, top right, bottom left, bottom right
   const float cornerX[4] = { 0.f, static_cast<float>(m_texW), 0.f, static_cast<float>(m_texW) };
   const float cornerY[4] = { 0.f, 0.f, static_cast<float>(m_texH), static_cast<float>(m_texH) };
   for (int i = 0; i < 4; i++)
   {
      float tx, ty;
      ToTable(cornerX[i], cornerY[i], tx, ty);
      v[i] = { tx, ty, m_p.z, 0.f, 0.f, 1.f, i & 1 ? 1.f : 0.f, i >> 1 ? 1.f : 0.f };
   }

   rd->ResetRenderState();
   const PartGroupData::SpaceReference prevSpace = renderer->GetSpaceReference();
   renderer->SelectPlayfieldSpace(); // The last part drawn may sit in the cabinet or the room space
   renderer->UpdateBasicShaderMatrix();
   rd->EnableAlphaBlend(false);
   // Behind: depth tested without writing, so the parts above the plane hide it and the ones below do not; In Front: over everything
   rd->SetRenderState(RenderState::ZENABLE, m_p.front ? RenderState::RS_FALSE : RenderState::RS_TRUE);
   rd->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
   rd->SetRenderState(RenderState::ZFUNC, RenderState::Z_LESSEQUAL);
   rd->SetRenderState(RenderState::CULLMODE, RenderState::CULL_NONE);
   shader->SetTechnique(ShaderTechnique::unshaded_with_texture);
   shader->SetTexture(ShaderUniform::tex_base_color, m_tex, false, SamplerFilter::SF_TRILINEAR, SamplerAddressMode::SA_CLAMP, SamplerAddressMode::SA_CLAMP);
   shader->SetVector(ShaderUniform::staticColor_Alpha, 1.f, 1.f, 1.f, m_p.opacity);
   rd->DrawTexturedQuad(shader, v);
   // Transparent commands run back to front after the opaque ones: Behind first among them, In Front last
   RenderCommand *const cmd = rd->GetCurrentPass()->m_commands.back();
   cmd->SetTransparent(true);
   cmd->SetDepth(m_p.front ? -1e9f : 1e9f);
   shader->SetVector(ShaderUniform::staticColor_Alpha, 1.f, 1.f, 1.f, 1.f);
   renderer->SetSpaceReferenceFor(prevSpace); // The draws after the image (selection edges, hidden overlay) keep the space they had
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Overlay and panel

void ReferenceImage::Click(EditorUI &ed, const ImVec2 &mouse)
{
   if (m_pick != 1 && m_pick != 2)
      return;
   const Vertex2D p = ed.UnprojectToPlane(mouse, m_p.z);
   const int i = m_pick - 1;
   m_pickPt[i][0] = p.x;
   m_pickPt[i][1] = p.y;
   if (i == 1 && hypotf(m_pickPt[1][0] - m_pickPt[0][0], m_pickPt[1][1] - m_pickPt[0][1]) < 1.f)
      return; // The same point twice
   m_pick++;
}

void ReferenceImage::DrawOverlay(EditorUI &ed, ImDrawList *const draw, const LiveRenderContext &ctx)
{
   if (!m_panelOpen || ed.IsInspectMode() || ed.m_camMode != ViewMode::EditorCam)
      return;
   EnsureLoaded(ed);
   if (m_tex == nullptr)
      return;
   const float dpi = ed.m_liveUI.GetDPI();
   const ImU32 col = IM_COL32(60, 200, 255, 255);
   if (m_p.show && m_p.showMain)
   {
      // The outline of the image while its panel is open
      ImVec2 c[4];
      const float cornerX[4] = { 0.f, static_cast<float>(m_texW), static_cast<float>(m_texW), 0.f };
      const float cornerY[4] = { 0.f, 0.f, static_cast<float>(m_texH), static_cast<float>(m_texH) };
      bool ok = true;
      for (int i = 0; i < 4; i++)
      {
         float tx, ty;
         ToTable(cornerX[i], cornerY[i], tx, ty);
         c[i] = ctx.Project(Vertex3Ds(tx, ty, m_p.z));
         ok &= c[i].x != FLT_MAX;
      }
      if (ok)
         for (int i = 0; i < 4; i++)
            draw->AddLine(c[i], c[(i + 1) % 4], IM_COL32(60, 200, 255, 110), dpi);
   }
   if (m_pick >= 2)
   {
      ImVec2 a = ctx.Project(Vertex3Ds(m_pickPt[0][0], m_pickPt[0][1], m_p.z)), b;
      if (m_pick >= 3)
         b = ctx.Project(Vertex3Ds(m_pickPt[1][0], m_pickPt[1][1], m_p.z));
      else if (!ImGui::GetIO().WantCaptureMouse)
         b = ImGui::GetMousePos();
      else
         b = ImVec2(FLT_MAX, FLT_MAX);
      if (a.x != FLT_MAX && b.x != FLT_MAX)
      {
         draw->AddLine(a, b, IM_COL32(0, 0, 0, 200), 3.5f * dpi);
         draw->AddLine(a, b, col, 1.5f * dpi);
      }
      for (int i = 0; i < (m_pick >= 3 ? 2 : 1); i++)
      {
         const ImVec2 p = i == 0 ? a : b;
         if (p.x != FLT_MAX)
         {
            draw->AddCircle(p, 6.f * dpi, IM_COL32(0, 0, 0, 200), 16, 3.5f * dpi);
            draw->AddCircle(p, 6.f * dpi, col, 16, 1.5f * dpi);
         }
      }
   }
}

void ReferenceImage::RenderPanel(EditorUI &ed)
{
   Reap(false);
   if (ed.IsInspectMode())
   {
      m_pendingFile.reset(); // An answer of the file dialog in a play test is dropped: the play test is view-only
      m_pick = 0;
      return;
   }
   if (m_pendingFile != nullptr && !m_pendingFile->empty())
   {
      const string file = *m_pendingFile;
      m_pendingFile.reset();
      EnsureLoaded(ed);
      g_settingsService.GetAppSettings().SetRecentDir_ImportDir(PathToString(PathFromString(file).parent_path()), false);
      if (LoadFromFile(ed, file, false))
      {
         m_saveDue = true;
         ed.Report("Reference image loaded");
      }
      else
         ed.Report(m_error);
   }
   FlushSave(ed);
   if (!m_panelOpen)
   {
      m_pick = 0;
      return;
   }
   EnsureLoaded(ed);

   const float dpi = ed.m_liveUI.GetDPI();
   ImGui::SetNextWindowPos(ImVec2(ed.m_viewportContentRect.x0 + 12.f * dpi, ed.m_viewportContentRect.y0 + 48.f * dpi), ImGuiCond_FirstUseEver);
   ImGui::SetNextWindowSize(ImVec2(340.f * dpi, 0.f), ImGuiCond_Always);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * dpi, 6.f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::V(EditorTheme::Border));
   if (ImGui::Begin("Reference Image", &m_panelOpen, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
   {
      bool changed = false;
      if (ImGui::Button("Load Image..."))
         PickFile(ed);
      ImGui::SetItemTooltip("A scan or blueprint (PNG, JPG) laid on the playfield plane. Kept in the table's settings file, not in the table");
      if (m_tex != nullptr)
      {
         ImGui::SameLine();
         if (ImGui::Button("Remove"))
         {
            Retire(ed, m_tex);
            m_tex = nullptr;
            m_pick = 0;
            changed = true;
         }
      }
      if (!m_error.empty())
      {
         ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.45f, 0.4f, 1.f));
         ImGui::PushTextWrapPos(0.f);
         ImGui::TextUnformatted(m_error.c_str());
         ImGui::PopTextWrapPos();
         ImGui::PopStyleColor();
      }
      if (m_tex == nullptr)
      {
         ImGui::TextDisabled("No reference image for this table.");
         ImGui::End();
         ImGui::PopStyleColor(2);
         ImGui::PopStyleVar(2);
         if (changed)
            m_saveDue = true;
         return;
      }
      ImGui::TextDisabled("%s  (%d x %d px)", PathToUTF8(PathFromString(m_p.file).filename()).c_str(), m_texW, m_texH);

      changed |= ImGui::Checkbox("Show", &m_p.show);
      ImGui::SameLine();
      int mode = m_p.front ? 1 : 0;
      if (ImGui::RadioButton("Behind", mode == 0))
         mode = 0, changed = true;
      ImGui::SetItemTooltip("Under the parts, depth tested like a playfield");
      ImGui::SameLine();
      if (ImGui::RadioButton("In Front", mode == 1))
         mode = 1, changed = true;
      ImGui::SetItemTooltip("Over everything, semi transparent");
      m_p.front = mode == 1;
      ImGui::SetNextItemWidth(-FLT_MIN);
      changed |= ImGui::SliderFloat("##opacity", &m_p.opacity, 0.f, 1.f, "Opacity  %.2f", ImGuiSliderFlags_AlwaysClamp);

      ImGui::SeparatorText("Scale");
      ImGui::SetNextItemWidth(120.f * dpi);
      if (ImGui::DragFloat("DPI", &m_p.dpi, 1.f, 10.f, 4800.f, "%.1f"))
      {
         m_p.dpi = std::clamp(m_p.dpi, 10.f, 4800.f);
         ApplyDpi();
         changed = true;
      }
      ImGui::SetItemTooltip("Scan resolution: %.2f VPU per inch", kVPUPerInch);
      ImGui::SameLine();
      ImGui::TextDisabled("%.2f x %.2f in", static_cast<float>(m_texW) / m_p.dpi, static_cast<float>(m_texH) / m_p.dpi);
      if (m_pick == 0)
      {
         ImGui::BeginDisabled(ed.m_measure.IsActive()); // Both take the clicks of the 3D views: one at a time
         if (ImGui::Button("Scale from 2 Points..."))
         {
            m_pick = 1;
            m_realDistance = 0.f;
         }
         ImGui::EndDisabled();
         ImGui::SetItemTooltip("%s", ed.m_measure.IsActive() ? "Not available while the Measure tool is on" : "Click two points on the image in the 3D Viewport, then type their real distance");
      }
      else if (m_pick < 3)
      {
         ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.f, 1.f), m_pick == 1 ? "Click the first point on the image (3D Viewport)" : "Click the second point");
         if (ImGui::Button("Cancel"))
            m_pick = 0;
      }
      else
      {
         const float picked = hypotf(m_pickPt[1][0] - m_pickPt[0][0], m_pickPt[1][1] - m_pickPt[0][1]);
         ImGui::Text("Picked: %.2f in (%.1f mm) at the current scale", picked / kVPUPerInch, picked / kVPUPerInch * 25.4f);
         ImGui::SetNextItemWidth(110.f * dpi);
         ImGui::InputFloat("##real", &m_realDistance, 0.f, 0.f, "%.3f");
         ImGui::SameLine();
         ImGui::SetNextItemWidth(70.f * dpi);
         const char *const units[2] = { "in", "mm" };
         ImGui::Combo("##unit", &m_unit, units, 2);
         ImGui::SameLine();
         ImGui::TextUnformatted("real distance");
         ImGui::BeginDisabled(m_realDistance <= 0.f);
         if (ImGui::Button("Apply"))
         {
            const float wanted = (m_unit == 0 ? m_realDistance : m_realDistance / 25.4f) * kVPUPerInch; // VPU
            float pxA, pyA, pxB, pyB;
            ToPixel(m_pickPt[0][0], m_pickPt[0][1], pxA, pyA);
            ToPixel(m_pickPt[1][0], m_pickPt[1][1], pxB, pyB);
            const float pixels = hypotf(pxB - pxA, pyB - pyA);
            const float newDpi = pixels > 0.01f ? kVPUPerInch * pixels / wanted : 0.f;
            if (!(newDpi >= 10.f && newDpi <= 4800.f))
               ed.Report(std::format("Reference image not scaled: that distance means {:.0f} DPI (10 to 4800 allowed)", newDpi));
            else
            {
               // The new scale, the first point staying where it is
               m_p.dpi = newDpi;
               m_p.scale = kVPUPerInch / m_p.dpi;
               float tx, ty;
               ToTable(pxA, pyA, tx, ty); // Where the first point goes with the new scale (centre unchanged)
               m_p.x += m_pickPt[0][0] - tx;
               m_p.y += m_pickPt[0][1] - ty;
               changed = true;
               ed.Report(std::format("Reference image scaled: {:.1f} DPI", m_p.dpi));
            }
            m_pick = 0;
         }
         ImGui::EndDisabled();
         ImGui::SameLine();
         if (ImGui::Button("Cancel"))
            m_pick = 0;
      }

      ImGui::SeparatorText("Position");
      ImGui::SetNextItemWidth(-FLT_MIN);
      float xy[2] = { m_p.x, m_p.y };
      if (ImGui::DragFloat2("##pos", xy, 0.5f, 0.f, 0.f, "%.2f"))
      {
         m_p.x = xy[0];
         m_p.y = xy[1];
         changed = true;
      }
      ImGui::SetItemTooltip("Offset X / Y: the centre of the image, table coordinates (VPU)");
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::DragFloat("##rot", &m_p.rot, 0.1f, -180.f, 180.f, "Rotation  %.2f deg", ImGuiSliderFlags_AlwaysClamp))
      {
         m_p.rot = std::clamp(m_p.rot, -180.f, 180.f);
         changed = true;
      }
      ImGui::SetNextItemWidth(-FLT_MIN);
      changed |= ImGui::DragFloat("##z", &m_p.z, 0.05f, -100.f, 500.f, "Height  %.2f VPU", ImGuiSliderFlags_AlwaysClamp);
      ImGui::SetItemTooltip("Above the playfield plane: a little clear of the playfield surface");
      if (ImGui::Button("Center on Table"))
      {
         CenterOnTable(ed);
         changed = true;
      }

      ImGui::SeparatorText("Shown in");
      changed |= ImGui::Checkbox("3D Viewport", &m_p.showMain);
      int views = 0;
      for (const LeafRect &leaf : ed.m_leafRects)
         if (leaf.type == AreaType::CameraPreview)
            views++;
      views = std::min(views, kMaxSecondaryViews);
      for (int i = 0; i < views; i++)
         changed |= ImGui::Checkbox(std::format("Secondary View {}", i + 1).c_str(), &m_p.showView[i]);
      if (views == 0)
         ImGui::TextDisabled("(no secondary view in this layout)");
      if (changed)
         m_saveDue = true;
   }
   ImGui::End();
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(2);
}

}
