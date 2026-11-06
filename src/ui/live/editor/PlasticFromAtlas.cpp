// license:GPLv3+

#include "core/stdafx.h"
#include "core/editablereg.h"
#include "core/player.h"
#include "core/SettingsService.h"
#include "PlasticFromAtlas.h"
#include "EditorUIPart.h"
#include "RubberBetweenPosts.h"
#include "imgui/imgui_stdlib.h"
#include "EditorTheme.h"
#include "LiveRenderContext.h"
#include "Selection.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "math/ImageDpi.h"
#include "math/PlasticMesh.h"
#include "parts/Material.h"
#include "parts/primitive.h"
#include "physics/PhysicsEngine.h"
#include "renderer/Renderer.h"
#include "renderer/Texture.h"

namespace VPX::EditorUI
{

namespace
{
string s_lastImage; // The atlas used last, offered first next time

bool IsHost(const IEditable *part) { return part != nullptr && part->GetItemType() == eItemPrimitive && (static_cast<const Primitive *>(part)->m_d.m_plasticGen || static_cast<const Primitive *>(part)->m_d.m_wireRail || static_cast<const Primitive *>(part)->m_d.m_playfieldGen || static_cast<const Primitive *>(part)->m_d.m_rampCollider); } // A Wire Rail's channel part and the Playfield Maker's walls and floor are kept the same way
// The point edit tools (backdrop, bow, outline) are the plastic generator's only; the companion handling also keeps a Wire Rail's channel part
bool IsPointEditHost(const IEditable *part) { return IsHost(part) && static_cast<const Primitive *>(part)->m_d.m_plasticGen; }

// Inverse of the table-to-UV mapping: the table position of an atlas UV
Vertex2D UvToTable(const float (&m)[6], const float u, const float v)
{
   const float det = m[0] * m[4] - m[1] * m[3];
   if (fabsf(det) < 1e-12f)
      return Vertex2D(0.f, 0.f);
   const float du = u - m[2], dv = v - m[5];
   return Vertex2D((m[4] * du - m[1] * dv) / det, (-m[3] * du + m[0] * dv) / det);
}

}

Primitive *PlasticFromAtlas::NewPrimitive(EditorUI &ed, const string &name, const float x, const float y)
{
   IEditable *const pie = EditableRegistry::CreateAndInit(eItemPrimitive, ed.m_table, x, y);
   if (pie == nullptr)
      return nullptr;
   if (auto *const scriptable = pie->GetIScriptable(); scriptable)
      scriptable->m_name = name;
   pie->m_desktopBackdrop = (ed.m_camMode == ViewMode::DesktopBackdrop);
   ed.m_table->AddPart(pie);
   pie->SetPartGroup(ed.GetPartGroupForNewPart());
   Primitive *const prim = static_cast<Primitive *>(pie);
   prim->m_d.m_toy = true;
   prim->m_d.m_collidable = false;
   prim->m_d.m_reflectionEnabled = false;
   return prim;
}

string PlasticFromAtlas::UniqueName(EditorUI &ed, const string &name) { return ed.m_table->IsNameUnique(name) ? name : ed.m_table->GetUniqueName(name); }

float PlasticFromAtlas::ParseDpi(const string &imageName)
{
   string s = imageName;
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
   return 0.f;
}

const char *PlasticFromAtlas::DpiSourceText(const int source)
{
   static constexpr const char *text[] = { "from file", "from name", "default", "measured", "typed", "fitted" };
   return text[clamp(source, 0, 5)];
}

// The resolution the file states, else the number in the name, else 150
float PlasticFromAtlas::SheetDpi(const Texture *const tex, const string &imageName, int &source)
{
   if (tex != nullptr && tex->GetFileSize() > 0)
      if (const float dpi = ImageDpi::FromFile(tex->GetFileRaw(), tex->GetFileSize()); dpi >= 10.f && dpi <= 2400.f)
      {
         source = DpiFile;
         return dpi;
      }
   if (const float dpi = ParseDpi(imageName); dpi > 0.f)
   {
      source = DpiName;
      return dpi;
   }
   source = DpiDefault;
   return 150.f;
}

string PlasticFromAtlas::DefaultImage(const EditorUI &ed)
{
   const vector<Texture *> &images = ed.m_table->GetImageList();
   for (const Texture *const tex : images)
      if (tex->m_name == s_lastImage)
         return tex->m_name;
   // An image with "plastic" in its name; the biggest one if there are several (the atlas, not a part's texture)
   const Texture *best = nullptr;
   for (const Texture *const tex : images)
   {
      string s = tex->m_name;
      for (char &c : s)
         c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      if (s.find("plastic") != string::npos && (best == nullptr || static_cast<uint64_t>(tex->m_width) * tex->m_height > static_cast<uint64_t>(best->m_width) * best->m_height))
         best = tex;
   }
   if (best != nullptr)
      return best->m_name;
   return images.empty() ? string() : images.front()->m_name;
}

// The sheet centred on the table, at the scale of the dpi
void PlasticFromAtlas::SetSheet(EditorUI &ed)
{
   Trace &t = ed.m_plasticTrace;
   t.dpi = clamp(t.dpi, 10.f, 4800.f);
   const Texture *const tex = ed.m_table->GetImage(t.image);
   const float w = tex != nullptr && tex->m_width > 0 ? static_cast<float>(tex->m_width) : 1024.f;
   const float h = tex != nullptr && tex->m_height > 0 ? static_cast<float>(tex->m_height) : 1024.f;
   const float sw = w * kVPUPerInch / t.dpi, sh = h * kVPUPerInch / t.dpi;
   const float x0 = 0.5f * (ed.m_table->m_right + ed.m_table->m_left) - 0.5f * sw, y0 = 0.5f * (ed.m_table->m_bottom + ed.m_table->m_top) - 0.5f * sh;
   t.uvm[0] = 1.f / sw;
   t.uvm[1] = 0.f;
   t.uvm[2] = -x0 / sw;
   t.uvm[3] = 0.f;
   t.uvm[4] = 1.f / sh;
   t.uvm[5] = -y0 / sh;
}

// Use an image of the table as the atlas: its Sheet DPI comes from the file, the name or the default, and the sheet is framed
void PlasticFromAtlas::SelectAtlas(EditorUI &ed, const string &name)
{
   Trace &t = ed.m_plasticTrace;
   t.image = name;
   t.dpi = SheetDpi(ed.m_table->GetImage(name), name, t.dpiSource);
   t.measure = 0;
   SetSheet(ed);
   const Vertex2D a = UvToTable(t.uvm, 0.f, 0.f), b = UvToTable(t.uvm, 1.f, 1.f);
   ed.FrameBounds(FRect3D(a.x, b.x, a.y, b.y, 0.f, 0.f));
}

namespace
{
std::shared_ptr<vector<string>> s_pendingLoad; // The file dialog's answer (it comes back on another thread)
}

// Load Image: the system file dialog, the same one the Images list uses
void PlasticFromAtlas::LoadAtlasImage(EditorUI &ed)
{
   if (ed.m_player == nullptr || ed.m_player->m_playfieldWnd == nullptr || s_pendingLoad != nullptr)
      return;
   s_pendingLoad = std::make_shared<vector<string>>();
   const SDL_DialogFileFilter filters[] = { { "Image Files", "png;jpg;jpeg;webp" } };
   const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_ImageDir())); // SDL expects UTF-8
   SDL_ShowOpenFileDialog(
      [](void *userdata, const char *const *filelist, int)
      {
         auto *res = static_cast<std::shared_ptr<vector<string>> *>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            (**res).push_back(filelist[0]);
         else
            (**res).push_back(string()); // Cancelled: answered, nothing chosen
         delete res;
      },
      new std::shared_ptr<vector<string>>(s_pendingLoad), //
      ed.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
}

void PlasticFromAtlas::ApplyLoadedImage(EditorUI &ed)
{
   if (s_pendingLoad == nullptr || s_pendingLoad->empty())
      return;
   const string file = s_pendingLoad->front();
   s_pendingLoad = nullptr;
   if (file.empty() || !ed.m_plasticTrace.active)
      return;
   // A name the table already has gets a number
   const std::filesystem::path path = PathFromUTF8(file);
   const string base = PathToUTF8(path.stem());
   string name = base;
   for (int n = 1; ed.m_table->GetImage(name) != nullptr; n++)
      name = base + std::to_string(n);
   Texture *const tex = ed.m_table->ImportImage(path, name);
   if (tex == nullptr)
   {
      ed.Report("Plastic from Atlas: could not load " + file);
      return;
   }
   g_settingsService.GetAppSettings().SetRecentDir_ImageDir(PathToString(path.parent_path()), false);
   ed.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
   SelectAtlas(ed, tex->m_name);
   ed.Report("Plastic from Atlas: loaded " + tex->m_name + " (" + std::to_string(static_cast<unsigned>(tex->m_width)) + " x " + std::to_string(static_cast<unsigned>(tex->m_height)) + ", " + DpiSourceText(ed.m_plasticTrace.dpiSource) + " "
      + std::to_string(static_cast<int>(lroundf(ed.m_plasticTrace.dpi))) + " dpi)");
}

bool PlasticFromAtlas::CanStart(const EditorUI &ed)
{
   return !ed.m_table->IsLocked() && !ed.IsInspectMode() && ed.m_pointEditPart == nullptr && !ed.m_plasticTrace.active;
}

void PlasticFromAtlas::Start(EditorUI &ed)
{
   if (!CanStart(ed))
      return;
   if (ed.m_table->GetImageList().empty())
   {
      ed.Report("Plastic from Atlas: the table has no image (import the atlas first)");
      return;
   }
   ed.m_addPartType = eItemInvalid;
   ed.ClearSelection();
   Trace t;
   t.active = true;
   t.image = DefaultImage(ed);
   t.dpi = SheetDpi(ed.m_table->GetImage(t.image), t.image, t.dpiSource);
   t.holeDia = 12.f;
   t.prevPerspective = ed.m_perspectiveCam;
   t.prevView = ed.m_camView;
   t.prevDistance = ed.m_camDistance;
   t.prevPredefined = static_cast<int>(ed.m_predefinedView);
   ed.m_plasticTrace = t;
   SetSheet(ed);
   // Top view, orthographic, framed on the sheet
   const Vertex2D a = UvToTable(ed.m_plasticTrace.uvm, 0.f, 0.f), b = UvToTable(ed.m_plasticTrace.uvm, 1.f, 1.f);
   ed.m_perspectiveCam = false;
   ed.SetView(EditorUI::PredefinedView::Top);
   ed.FrameBounds(FRect3D(a.x, b.x, a.y, b.y, 0.f, 0.f));
   ed.Report("Plastic from Atlas: LMB a corner where the line changes direction, Backspace removes the last, H holes, Enter closes, Esc cancels");
}

void PlasticFromAtlas::Cancel(EditorUI &ed)
{
   if (!ed.m_plasticTrace.active)
      return;
   ed.m_perspectiveCam = ed.m_plasticTrace.prevPerspective;
   ed.m_camView = ed.m_plasticTrace.prevView;
   ed.m_camDistance = ed.m_plasticTrace.prevDistance;
   ed.m_predefinedView = static_cast<EditorUI::PredefinedView>(ed.m_plasticTrace.prevPredefined);
   ed.m_plasticTrace = Trace();
   s_pendingLoad = nullptr; // A file picked in the dialog later is not for the next trace (the dialog's callback keeps its own reference)
   ed.Report("Plastic from Atlas cancelled");
}

void PlasticFromAtlas::Click(EditorUI &ed, const ImVec2 &mouse, const bool shift)
{
   Trace &t = ed.m_plasticTrace;
   const Vertex2D p = ed.UnprojectToPlane(mouse, 0.f);
   (void)shift;
   if (t.measure != 0)
   {
      // Measuring takes the click: no corner, no hole
      if (t.measure <= 2)
      {
         const int i = t.measure - 1;
         t.measureUv[2 * i] = t.uvm[0] * p.x + t.uvm[1] * p.y + t.uvm[2];
         t.measureUv[2 * i + 1] = t.uvm[3] * p.x + t.uvm[4] * p.y + t.uvm[5];
         PLOGI << "Plastic from Atlas: measure point " << t.measure << " at table " << p.x << ' ' << p.y << " uv " << t.measureUv[2 * i] << ' ' << t.measureUv[2 * i + 1];
         t.measure++;
      }
      return;
   }
   PLOGI << "Plastic from Atlas: " << (t.holes ? "hole" : "corner") << " at table " << p.x << ' ' << p.y;
   if (t.holes)
      t.holeList.push_back(Vertex3Ds(p.x, p.y, t.holeDia));
   else
   {
      t.pts.push_back(Vertex3Ds(p.x, p.y, 0.f));
      t.smooth.push_back(false);
   }
}

void PlasticFromAtlas::Keys(EditorUI &ed)
{
   Trace &t = ed.m_plasticTrace;
   const ImGuiIO &io = ImGui::GetIO();
   if (io.WantCaptureKeyboard || t.measure != 0 || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) // Typing in a field, measuring, or the Atlas list open: the outline keys are not its own
      return;
   if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
      Finish(ed);
   else if (ImGui::IsKeyPressed(ImGuiKey_Backspace))
   {
      if (t.holes && !t.holeList.empty())
         t.holeList.pop_back();
      else if (!t.holes && !t.pts.empty())
      {
         t.pts.pop_back();
         t.smooth.pop_back();
      }
   }
   else if (ImGui::IsKeyPressed(ImGuiKey_H, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift)
   {
      t.holes = !t.holes;
      ed.Report(t.holes ? "Plastic from Atlas: placing holes (LMB), H back to the outline" : "Plastic from Atlas: tracing the outline, H for holes");
   }
}

Material *PlasticFromAtlas::EnsureMaterial(PinTable *const table, const int which)
{
   static constexpr const char *names[3] = { "PBR.Plastic_Print_Plastics", "PBR.Plastic_Clear1", "PBR.Plastic_Clear1_Bevel" };
   if (Material *const existing = table->GetMaterial(names[which]); !table->IsDummyMaterial(existing)) // GetMaterial never returns null
      return existing;
   // Police Force's values
   Material *const m = new Material();
   m->m_name = names[which];
   m->m_type = Material::PBR;
   m->m_fGlossyImageLerp = 1.f;
   m->m_fEdge = 1.f;
   m->m_cBase = 0xFFFFFF;
   m->m_cGlossy = 0;
   m->m_cClearcoat = 0;
   m->m_cRefractionTint = 0xFFFFFF;
   m->m_fMetallic = 0.f;
   m->m_fOcclusion = 1.f;
   m->m_cEmission = 0;
   m->m_fEmissionIntensity = 1.f;
   m->m_fElasticity = 0.f;
   m->m_fElasticityFalloff = 0.f;
   m->m_fFriction = 0.f;
   m->m_fScatterAngle = 0.f;
   if (which == 0)
   {
      m->m_fWrapLighting = 0.08f;
      m->m_fRoughness = 0.35f;
      m->m_fThickness = 0.05f;
      m->m_fEdgeAlpha = 0.f;
      m->m_fOpacity = 1.f;
      m->m_bOpacityActive = false;
      m->m_fClearcoat = 1.f;
      m->m_fClearcoatRoughness = 0.03f;
   }
   else
   {
      m->m_fWrapLighting = 0.f;
      m->m_fRoughness = which == 1 ? 0.07f : 0.045f;
      m->m_fThickness = 0.5f;
      m->m_fEdgeAlpha = 1.f;
      m->m_fOpacity = which == 1 ? 0.1f : 0.15f;
      m->m_bOpacityActive = true;
      m->m_fMetallic = which == 1 ? 0.f : 0.3f;
      m->m_fScatter = 0.19f;
      m->m_fAbsorption = 0.3f;
      m->m_cAbsorption = 14083821;
   }
   table->AddMaterial(m);
   table->SetNonUndoableDirty(SaveDirtyState::Dirty);
   return m;
}

void PlasticFromAtlas::Finish(EditorUI &ed)
{
   Trace &t = ed.m_plasticTrace;
   if (!t.active)
      return;
   if (t.pts.size() < 3)
   {
      ed.Report("Plastic from Atlas: the outline needs 3 points or more");
      return;
   }
   int crossing = 0, skipped = 0;
   {
      // Build it first: a degenerate outline (collinear clicks) must not leave three default primitives behind
      vector<PlasticMesh::CurvePoint> outline;
      for (const Vertex3Ds &p : t.pts)
         outline.push_back({ p.x, p.y, false });
      vector<PlasticMesh::Hole> holes;
      for (const Vertex3Ds &h : t.holeList)
         holes.push_back({ h.x, h.y, h.z });
      PlasticMesh::Result probe;
      if (!PlasticMesh::Build(outline, holes, PlasticMesh::Params(), probe))
      {
         ed.Report("Plastic from Atlas: the outline has no area (Backspace to fix it)");
         return;
      }
      crossing = probe.crossingHoles;
      skipped = probe.skippedHoles;
   }
   const float cx = 0.5f * (ed.m_table->m_left + ed.m_table->m_right), cy = 0.5f * (ed.m_table->m_top + ed.m_table->m_bottom);
   const string hostName = ed.m_table->GetUniqueName("Plastic"s);
   const string clearName = UniqueName(ed, hostName + "_Clear"), bevelName = UniqueName(ed, hostName + "_Bevel");

   Primitive *const host = NewPrimitive(ed, hostName, cx, cy);
   Primitive *const clear = host != nullptr ? NewPrimitive(ed, clearName, cx, cy) : nullptr;
   Primitive *const bevel = clear != nullptr ? NewPrimitive(ed, bevelName, cx, cy) : nullptr;
   if (bevel == nullptr)
   {
      ed.Report("Plastic from Atlas: could not create the parts");
      return;
   }
   host->m_d.m_szImage = t.image;
   host->m_d.m_szMaterial = EnsureMaterial(ed.m_table, 0)->m_name;
   clear->m_d.m_szMaterial = EnsureMaterial(ed.m_table, 1)->m_name;
   bevel->m_d.m_szMaterial = EnsureMaterial(ed.m_table, 2)->m_name;
   host->m_d.m_plasticDpi = t.dpi;
   host->m_d.m_plasticHoleDia = t.holeDia;
   for (int i = 0; i < 6; i++)
      host->m_d.m_plasticUvm[i] = t.uvm[i];
   host->InitPlastic(t.pts, t.smooth, t.holeList);
   // The part moves so that its centre sits at the table centre, ready for G and R (the art stays on the part)
   {
      const Vertex2D c = host->GetCenter();
      host->Translate(Vertex2D(cx - c.x, cy - c.y));
   }
   Primitive *const comps[2] = { clear, bevel };
   for (int i = 0; i < 2; i++)
   {
      comps[i]->m_d.m_plasticHost = hostName;
      comps[i]->m_d.m_plasticRole = i + 1;
      comps[i]->SetUILock(true); // A viewport click lands on the host
   }
   host->RegeneratePlastic(); // The companions are in the table now

   for (Primitive *const prim : { host, clear, bevel })
   {
      ed.m_player->TimerSetup(prim);
      prim->GetIRenderable()->RenderSetup(ed.m_renderer.get());
      if (prim->GetIHitable())
         ed.m_player->m_physics->Add(prim);
   }

   ed.m_undo.BeginUndo();
   for (Primitive *const prim : { host, clear, bevel })
      ed.m_undo.MarkForCreate(prim);
   ed.m_undo.SetLabel("Add Plastic from Atlas");
   ed.m_undo.EndUndo();
   // A plastic much bigger than 0.7 of the table's width, or smaller than an inch, almost always has the wrong Sheet DPI
   string sizeWarning;
   {
      float lox = FLT_MAX, hix = -FLT_MAX, loy = FLT_MAX, hiy = -FLT_MAX;
      for (const Vertex3D_NoTex2 &v : host->m_mesh.m_vertices)
      {
         lox = min(lox, v.x);
         hix = max(hix, v.x);
         loy = min(loy, v.y);
         hiy = max(hiy, v.y);
      }
      const float w = (hix - lox) / kVPUPerInch, l = (hiy - loy) / kVPUPerInch, tableWidth = (ed.m_table->m_right - ed.m_table->m_left) / kVPUPerInch;
      char text[256];
      if (lox <= hix && (w > 0.7f * tableWidth || l > 0.7f * tableWidth)) // 0.7 of the table's width: the 2.4x lion of the acceptance case is 14.4 in on a 20.2 in table
         snprintf(text, sizeof(text), "Warning: the plastic is %.1f x %.1f in, that looks large for this table (%.1f in wide): if the Sheet DPI is wrong, use Measure (before Enter) or Fit to Posts (right-click it)  ", w, l, tableWidth);
      else if (lox <= hix && w < 1.f && l < 1.f)
         snprintf(text, sizeof(text), "Warning: the plastic is %.2f x %.2f in, under 1 inch, Sheet DPI is probably wrong: use Measure (before Enter) or Fit to Posts (right-click it)  ", w, l);
      else
         text[0] = '\0';
      sizeWarning = text;
   }
   ed.m_plasticDpiNote = { host, host->m_d.m_plasticDpi, t.dpiSource };
   ed.Report(sizeWarning + "Added " + hostName + " (with " + clearName + " and " + bevelName + ")"
      + (crossing > 0 ? "  Warning: " + std::to_string(crossing) + (crossing == 1 ? " hole is on or near" : " holes are on or near") + " the outline edge" : string())
      + (skipped > 0 ? (crossing > 0 ? ", " : "  Warning: ") + std::to_string(skipped) + (skipped == 1 ? " hole is" : " holes are") + " outside the outline (left out)" : string()));
   host->m_plasticReportedCrossing = host->m_plasticCrossingHoles; // Said in the message above
   host->m_plasticReportedSkipped = host->m_plasticSkippedHoles;
   host->m_plasticCrossingWarn = false;

   s_lastImage = t.image;
   ed.m_perspectiveCam = t.prevPerspective;
   ed.m_plasticTrace = Trace();
   s_pendingLoad = nullptr;

   ed.UpdateEditableList();
   if (const auto it = ed.m_editableMap.find(host); it != ed.m_editableMap.end())
   {
      ed.SetSelection(Selection(it->second));
      ed.m_outlinerAnchor = it->second;
      Matrix3D placed;
      if (!ed.GetSelectionTransform(placed))
         placed.SetIdentity();
      ed.SetLastOperation(EditorUI::LastOperation::Type::PlasticFromAtlas, placed);
      ed.m_lastOp.plHeight = host->m_d.m_plasticHeight;
      ed.m_lastOp.plThickness = host->m_d.m_plasticThickness;
      ed.m_lastOp.plBevel = host->m_d.m_plasticBevel;
      ed.m_lastOp.plSegs = host->m_d.m_plasticBevelSegs;
      ed.m_lastOp.plDetail = host->m_d.m_plasticDetail;
      ed.m_lastOp.plHoleDia = host->m_d.m_plasticHoleDia;
      ed.m_lastOp.plDpi = host->m_d.m_plasticDpi;
      ed.FrameSelection();
   }
}

void PlasticFromAtlas::Adjust(Primitive *const host, const float height, const float thickness, const float bevel, const int segs, const float holeDia, const float dpi, const int detail)
{
   host->m_d.m_plasticHeight = clamp(height, -1000.f, 2000.f);
   host->m_d.m_plasticThickness = clamp(thickness, 0.1f, 100.f);
   host->m_d.m_plasticBevel = clamp(bevel, 0.f, host->m_d.m_plasticThickness);
   host->m_d.m_plasticBevelSegs = clamp(segs, 1, 16);
   host->m_d.m_plasticDetail = clamp(detail, 1, 5);
   host->m_d.m_plasticHoleDia = clamp(holeDia, 0.5f, 100.f);
   if (fabsf(dpi - host->m_d.m_plasticDpi) > 1e-3f)
      host->SetPlasticDpi(dpi);
   host->UpdatePlastic();
}

void PlasticFromAtlas::SyncCompanionVisibility(const vector<IEditable *> &toggled)
{
   for (IEditable *const part : toggled)
      if (IsHost(part))
         for (Primitive *const comp : static_cast<Primitive *>(part)->GetPlasticCompanions())
            if (comp->IsUIVisible(false) != part->IsUIVisible(false))
               comp->SetUIVisible(part->IsUIVisible(false));
}

std::map<const IEditable *, bool> PlasticFromAtlas::SnapshotHostVisibility(const EditorUI &ed)
{
   std::map<const IEditable *, bool> res;
   for (IEditable *const part : ed.m_table->GetParts())
      if (IsHost(part))
         res[part] = part->IsUIVisible(false);
   return res;
}

void PlasticFromAtlas::SyncChangedHostVisibility(const EditorUI &ed, const std::map<const IEditable *, bool> &before)
{
   vector<IEditable *> changed;
   for (IEditable *const part : ed.m_table->GetParts()) // The live parts, looked up by address (a host deleted by the undo is never dereferenced)
      if (IsHost(part))
         if (const auto it = before.find(part); it != before.end() && it->second != part->IsUIVisible(false))
            changed.push_back(part);
   SyncCompanionVisibility(changed);
}

bool PlasticFromAtlas::IsCompanion(const IEditable *part)
{
   return part != nullptr && part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->IsPlasticCompanion();
}

void PlasticFromAtlas::RepairCompanions(EditorUI &ed, const vector<IEditable *> *const only)
{
   if (ed.m_table->m_liveBaseTable != nullptr || ed.m_renderer == nullptr || ed.m_player == nullptr) // Not on the live copy of an inspected table
      return;
   const vector<IEditable *> parts = only != nullptr ? *only : ed.m_table->GetParts(); // Parts are added below
   for (IEditable *const part : parts)
   {
      if (!IsPointEditHost(part) || static_cast<const Primitive *>(part)->GetName().empty()) // A Wire Rail's companion is its channel, made with the rail
         continue;
      Primitive *const host = static_cast<Primitive *>(part);
      vector<Primitive *> created;
      string made;
      for (const int role : { 1, 2 })
      {
         if (CompanionOf(host, role) != nullptr)
            continue;
         const string name = UniqueName(ed, host->GetName() + (role == 1 ? "_Clear" : "_Bevel"));
         Primitive *const comp = NewPrimitive(ed, name, 0.f, 0.f);
         if (comp == nullptr)
            continue;
         comp->SetPartGroup(host->GetPartGroup());
         comp->m_d.m_szMaterial = EnsureMaterial(ed.m_table, role)->m_name;
         comp->m_d.m_plasticHost = host->GetName();
         comp->m_d.m_plasticRole = role;
         comp->SetUILock(true);
         if (only != nullptr)
            ed.m_undo.MarkForCreate(comp);
         created.push_back(comp);
         made += (made.empty() ? "" : " and ") + name;
      }
      if (created.empty())
         continue;
      host->RegeneratePlastic(); // Fills the new parts' meshes
      for (Primitive *const comp : created)
      {
         ed.m_player->TimerSetup(comp);
         comp->GetIRenderable()->RenderSetup(ed.m_renderer.get());
         if (comp->GetIHitable())
            ed.m_player->m_physics->Add(comp);
      }
      ed.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      PLOGW << "Plastic " << host->GetName() << " lacked part(s): recreated " << made << " from defaults";
   }
}

IEditable *PlasticFromAtlas::HostOf(const PinTable *table, const IEditable *companion)
{
   if (IsCompanion(companion))
      for (IEditable *const part : table->GetParts())
         if (IsHost(part) && StrCompareNoCase(part->GetName(), static_cast<const Primitive *>(companion)->m_d.m_plasticHost))
            return part;
   return const_cast<IEditable *>(companion);
}

Primitive *PlasticFromAtlas::CompanionOf(const Primitive *host, const int role)
{
   if (host == nullptr || !host->m_d.m_plasticGen)
      return nullptr;
   for (Primitive *const comp : host->GetPlasticCompanions())
      if (comp->m_d.m_plasticRole == role)
         return comp;
   return nullptr;
}

PlasticFromAtlas::CompanionMap::CompanionMap(const PinTable *const table)
   : m_table(table)
{
   const auto key = [](string name)
   {
      for (char &c : name)
         c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
      return name;
   };
   std::unordered_map<string, Primitive *> hosts;
   const vector<IEditable *> &parts = table->GetParts();
   for (IEditable *const part : parts)
      if (part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->m_d.m_plasticGen && !part->GetName().empty())
         hosts.emplace(key(part->GetName()), static_cast<Primitive *>(part));
   for (IEditable *const part : parts)
   {
      if (part->GetItemType() != eItemPrimitive)
         continue;
      Primitive *const comp = static_cast<Primitive *>(part);
      if (comp->m_d.m_plasticHost.empty() || (comp->m_d.m_plasticRole != 1 && comp->m_d.m_plasticRole != 2))
         continue;
      const auto host = hosts.find(key(comp->m_d.m_plasticHost));
      if (host != hosts.end() && host->second != comp)
      {
         Primitive *&slot = m_map[host->second][comp->m_d.m_plasticRole - 1];
         if (slot == nullptr)
            slot = comp;
      }
   }
}

Primitive *PlasticFromAtlas::CompanionMap::Get(const Primitive *const host, const int role) const
{
   if (host == nullptr || (role != 1 && role != 2))
      return nullptr;
   if (host->GetPTable() != m_table) // A part of another table (the startup copy during a play test): looked up directly
      return CompanionOf(host, role);
   const auto it = m_map.find(host);
   return it == m_map.end() ? nullptr : it->second[role - 1];
}

void PlasticFromAtlas::AddCompanions(vector<IEditable *> &parts)
{
   const size_t n = parts.size();
   for (size_t i = 0; i < n; i++)
      if (IsHost(parts[i]))
         for (Primitive *const comp : static_cast<Primitive *>(parts[i])->GetPlasticCompanions())
            if (std::ranges::find(parts, static_cast<IEditable *>(comp)) == parts.end())
               parts.push_back(comp);
}

void PlasticFromAtlas::FixPasted(const vector<IEditable *> &pasted, const vector<string> &originalNames)
{
   for (IEditable *const part : pasted)
   {
      if (part->GetItemType() != eItemPrimitive || static_cast<Primitive *>(part)->m_d.m_plasticHost.empty())
         continue;
      Primitive *const comp = static_cast<Primitive *>(part);
      bool found = false;
      for (size_t i = 0; i < pasted.size() && !found; i++)
         if (IsHost(pasted[i]) && StrCompareNoCase(originalNames[i], comp->m_d.m_plasticHost))
         {
            comp->m_d.m_plasticHost = pasted[i]->GetName(); // Follows the copy of its host
            found = true;
         }
      if (!found) // A companion alone is never pasted live: a plain primitive
      {
         comp->m_d.m_plasticHost.clear();
         comp->m_d.m_plasticRole = 0;
         comp->SetUILock(false);
      }
   }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Bowing a line into an arc (point edit of the outline): drag a line between two points = an arc through the cursor, either side (the smooth peak
// point between the two corners is inserted or moved); double-click a curve = straight again

namespace
{
struct Span
{
   int a, peak, b; // corner, smooth peak (-1 = a straight line), corner
};

vector<Span> Spans(const DragPointCurve &curve)
{
   vector<Span> spans;
   const auto &pts = curve.GetPoints();
   const int n = static_cast<int>(pts.size());
   for (int i = 0; i < n; i++)
   {
      if (pts[i]->IsSmooth())
         continue;
      const int j = (i + 1) % n, k = (i + 2) % n;
      if (pts[j]->IsSmooth() && !pts[k]->IsSmooth() && n >= 3)
         spans.push_back({ i, j, k });
      else if (!pts[j]->IsSmooth())
         spans.push_back({ i, -1, j });
   }
   return spans;
}

// Distance from p to the span as drawn (the arc is flattened by PlasticMesh)
float SpanDistance(const DragPointCurve &curve, const Span &sp, const Vertex2D &p)
{
   const auto &pts = curve.GetPoints();
   vector<PlasticMesh::CurvePoint> poly;
   if (sp.peak < 0)
      poly = { { pts[sp.a]->GetX(), pts[sp.a]->GetY(), false }, { pts[sp.b]->GetX(), pts[sp.b]->GetY(), false } };
   else
   {
      const vector<PlasticMesh::CurvePoint> tri { { pts[sp.a]->GetX(), pts[sp.a]->GetY(), false }, { pts[sp.peak]->GetX(), pts[sp.peak]->GetY(), true }, { pts[sp.b]->GetX(), pts[sp.b]->GetY(), false } };
      poly = PlasticMesh::FlattenOutline(tri, 0.25f); // A, the arc, B (the closing edge B to A is not part of it)
   }
   float best = FLT_MAX;
   for (size_t i = 0; i + 1 < poly.size(); i++)
   {
      const float dx = poly[i + 1].x - poly[i].x, dy = poly[i + 1].y - poly[i].y, l2 = dx * dx + dy * dy;
      const float t = l2 > 0.f ? clamp(((p.x - poly[i].x) * dx + (p.y - poly[i].y) * dy) / l2, 0.f, 1.f) : 0.f;
      best = min(best, hypotf(p.x - (poly[i].x + t * dx), p.y - (poly[i].y + t * dy)));
   }
   return best;
}
}

bool PlasticFromAtlas::BowPress(EditorUI &ed, const ImVec2 &mouse, const bool doubleClick)
{
   ed.m_plasticBow = Bow();
   if (ed.m_pointEditPart == nullptr || ed.m_pointEditCenter || !IsPointEditHost(ed.m_pointEditPart->GetEditable()))
      return false;
   Primitive *const host = static_cast<Primitive *>(ed.m_pointEditPart->GetEditable());
   if (host->m_plasticEditHoles || ed.m_table->IsLocked())
      return false;
   const float z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
   const Vertex2D p = ed.UnprojectToPlane(mouse, z), q = ed.UnprojectToPlane(ImVec2(mouse.x + 8.f * ed.m_liveUI.GetDPI(), mouse.y), z);
   const float tol = hypotf(q.x - p.x, q.y - p.y);
   PLOGI << "Plastic from Atlas: press at table " << p.x << ' ' << p.y;
   const vector<Span> spans = Spans(host->m_plasticCurve);
   int best = -1;
   float bestDist = tol;
   for (size_t i = 0; i < spans.size(); i++)
      if (const float d = SpanDistance(host->m_plasticCurve, spans[i], p); d < bestDist)
      {
         bestDist = d;
         best = static_cast<int>(i);
      }
   if (best < 0)
      return false;
   const Span &sp = spans[best];
   if (doubleClick)
   {
      if (sp.peak >= 0 && host->m_plasticCurve.GetPoints().size() > 3) // An outline keeps 3 points
      {
         ed.m_undo.BeginUndo();
         ed.m_undo.MarkForUndo(host);
         ed.m_undo.SetFieldLabel("Straighten Line");
         ed.m_undo.EndUndo();
         host->m_plasticCurve.DeletePoint(host->m_plasticCurve.GetPoints()[sp.peak].get());
         ed.m_pointSel.clear();
         ed.m_renderer->ReinitRenderable(host->GetIRenderable());
         ed.m_renderer->InvalidateShadowContent();
      }
      return true;
   }
   ed.m_plasticBow.pending = true;
   ed.m_plasticBow.start = mouse;
   ed.m_plasticBow.corner = host->m_plasticCurve.GetPoints()[sp.a].get();
   ed.m_plasticBow.peak = sp.peak >= 0 ? host->m_plasticCurve.GetPoints()[sp.peak].get() : nullptr;
   ed.m_plasticBow.endCorner = host->m_plasticCurve.GetPoints()[sp.b].get();
   return true;
}

void PlasticFromAtlas::BowDrag(EditorUI &ed)
{
   Bow &bow = ed.m_plasticBow;
   if (!bow.pending && !bow.active)
      return;
   if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ed.m_pointEditPart == nullptr || !IsPointEditHost(ed.m_pointEditPart->GetEditable()))
   {
      bow = Bow();
      return;
   }
   Primitive *const host = static_cast<Primitive *>(ed.m_pointEditPart->GetEditable());
   DragPointCurve &curve = host->m_plasticCurve;
   const ImVec2 mouse = ImGui::GetMousePos();
   if (bow.pending)
   {
      if (fabsf(mouse.x - bow.start.x) < 3.f && fabsf(mouse.y - bow.start.y) < 3.f)
         return;
      bow.pending = false;
      bow.active = true;
      ed.m_undo.BeginUndo();
      ed.m_undo.MarkForUndo(host);
      ed.m_undo.SetFieldLabel("Bow Curve");
      ed.m_undo.EndUndo();
      if (bow.peak == nullptr)
      {
         const int n = static_cast<int>(curve.GetPoints().size());
         int at = 0;
         for (int i = 0; i < n; i++)
            if (curve.GetPoints()[i].get() == bow.corner)
               at = i + 1;
         curve.InsertPoint(at, std::make_unique<DragPoint>(&curve, bow.corner->GetX(), bow.corner->GetY(), 0.f, true));
         bow.peak = curve.GetPoints()[at].get();
      }
   }
   const Vertex2D p = ed.UnprojectToPlane(mouse, host->m_d.m_plasticHeight + host->m_d.m_plasticThickness);
   bow.peak->SetX(p.x);
   bow.peak->SetY(p.y);
   curve.OnPointsModified();
   ed.m_pointSel.clear();
   ed.m_renderer->ReinitRenderable(host->GetIRenderable());
   ed.m_renderer->InvalidateShadowContent();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Editing the shape: pill holes, S on holes, the panel and the right-click menu

Primitive *PlasticFromAtlas::EditedHost(const EditorUI &ed)
{
   if (ed.m_pointEditPart == nullptr || ed.m_pointEditCenter)
      return nullptr;
   IEditable *const e = ed.m_pointEditPart->GetEditable();
   return IsPointEditHost(e) ? static_cast<Primitive *>(e) : nullptr; // the plastic generator only: a Wire Rail keeps its own point editing
}

bool PlasticFromAtlas::IsEditing(const EditorUI &ed) { return EditedHost(ed) != nullptr; }

bool PlasticFromAtlas::IsHostSelected(const EditorUI &ed) { return ed.m_selection.GetType() == Selection::S_EDITABLE && ed.m_selection.GetPart() != nullptr && IsPointEditHost(ed.m_selection.GetPart()->GetEditable()); }

bool PlasticFromAtlas::ZLocked(const EditorUI &ed) { return IsEditing(ed) || (ed.m_pointEditPart == nullptr && ed.m_multiSel.size() == 1 && IsHostSelected(ed)); }

bool PlasticFromAtlas::TopPlaneZ(const EditorUI &ed, float &z)
{
   if (!ZLocked(ed))
      return false;
   const Primitive *host = EditedHost(ed);
   if (host == nullptr && ed.m_selection.GetPart() != nullptr)
      host = static_cast<const Primitive *>(ed.m_selection.GetPart()->GetEditable());
   if (host == nullptr)
      return false;
   z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
   return true;
}

bool PlasticFromAtlas::IsEditingHoles(const EditorUI &ed)
{
   const Primitive *const host = EditedHost(ed);
   return host != nullptr && host->m_plasticEditHoles;
}

namespace
{
float SegDist(const Vertex2D &p, const DragPoint *a, const DragPoint *b)
{
   const float dx = b->GetX() - a->GetX(), dy = b->GetY() - a->GetY(), l2 = dx * dx + dy * dy;
   const float t = l2 > 1e-12f ? clamp(((p.x - a->GetX()) * dx + (p.y - a->GetY()) * dy) / l2, 0.f, 1.f) : 0.f;
   return hypotf(p.x - (a->GetX() + t * dx), p.y - (a->GetY() + t * dy));
}
}

bool PlasticFromAtlas::HolePress(EditorUI &ed, const ImVec2 &mouse, const bool doubleClick)
{
   ed.m_plasticPill = Pill();
   Primitive *const host = EditedHost(ed);
   if (host == nullptr || !host->m_plasticEditHoles || ed.m_table->IsLocked())
      return false;
   const float z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
   const Vertex2D p = ed.UnprojectToPlane(mouse, z), q = ed.UnprojectToPlane(ImVec2(mouse.x + 8.f * ed.m_liveUI.GetDPI(), mouse.y), z);
   const float tol = hypotf(q.x - p.x, q.y - p.y);
   DragPointCurve &curve = host->m_plasticHoles;
   const auto &pts = curve.GetPoints();
   const vector<int> partner = Primitive::PlasticPillPartners(curve);
   int ring = -1, middle = -1;
   float ringErr = tol;
   for (int i = 0; i < static_cast<int>(pts.size()); i++)
   {
      if (partner[i] >= 0 && partner[i] < i)
         continue; // The second end stands for its pill
      const DragPoint *const b = partner[i] > i ? pts[partner[i]].get() : pts[i].get();
      const float r = 0.5f * pts[i]->GetZ(), d = SegDist(p, pts[i].get(), b);
      if (fabsf(d - r) < ringErr)
      {
         ringErr = fabsf(d - r);
         ring = i;
      }
      else if (partner[i] > i && d < r - tol && middle < 0)
         middle = i;
   }
   const int hit = ring >= 0 ? ring : middle;
   if (hit < 0)
      return false;
   if (partner[hit] >= 0) // A pill
   {
      if (ring >= 0 && doubleClick)
      {
         // Round again, keeping the first end
         ed.m_undo.BeginUndo();
         ed.m_undo.MarkForUndo(host);
         ed.m_undo.SetFieldLabel("Round Hole");
         ed.m_undo.EndUndo();
         DragPoint *const first = pts[hit].get(), *const second = pts[partner[hit]].get();
         first->SetSmooth(false);
         curve.DeletePoint(second);
         ed.m_pointSel.clear();
         ed.m_renderer->ReinitRenderable(host->GetIRenderable());
         ed.m_renderer->InvalidateShadowContent();
         return true;
      }
      // The ring or the middle of a pill: both ends are selected and a drag moves the whole hole
      DragPoint *const a = pts[hit].get(), *const b = pts[partner[hit]].get();
      if (ed.m_pointSel.size() != 2 || !ed.IsPointSelected(a) || !ed.IsPointSelected(b))
         ed.m_pointSel = { a, b };
      ed.m_pointDragPending = !a->m_uiLocked;
      ed.m_pointDragZ = z;
      ed.m_pointDragPos = ed.UnprojectToPlane(mouse, z);
      return true;
   }
   if (ring < 0 || doubleClick)
      return ring >= 0; // The inside of a round hole is nothing; a double-click on a round hole's ring has nothing to make round
   ed.m_plasticPill.pending = true;
   ed.m_plasticPill.start = mouse;
   ed.m_plasticPill.a = pts[hit].get();
   return true;
}

void PlasticFromAtlas::PillEnd(EditorUI &ed)
{
   Pill &pill = ed.m_plasticPill;
   if (pill.active && pill.a != nullptr && pill.b != nullptr)
      if (Primitive *const host = EditedHost(ed))
         if (hypotf(pill.b->GetX() - pill.a->GetX(), pill.b->GetY() - pill.a->GetY()) < 0.05f) // Dragged back onto the first end: round again
         {
            pill.a->SetSmooth(false);
            host->m_plasticHoles.DeletePoint(pill.b);
            ed.m_pointSel.clear();
            ed.m_renderer->ReinitRenderable(host->GetIRenderable());
            ed.m_renderer->InvalidateShadowContent();
            pill = Pill();
            return;
         }
   if (pill.active && pill.a != nullptr && pill.b != nullptr)
      ed.m_pointSel = { pill.a, pill.b }; // Both ends selected (not during the drag: the gizmo would be under the cursor)
   pill = Pill();
}

void PlasticFromAtlas::AbortDrags(EditorUI &ed)
{
   ed.m_plasticPill = Pill();
   ed.m_plasticBow = Bow();
}

void PlasticFromAtlas::PillDrag(EditorUI &ed)
{
   Pill &pill = ed.m_plasticPill;
   if (!pill.pending && !pill.active)
      return;
   Primitive *const host = EditedHost(ed);
   if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || host == nullptr || !host->m_plasticEditHoles)
   {
      PillEnd(ed);
      return;
   }
   DragPointCurve &curve = host->m_plasticHoles;
   const ImVec2 mouse = ImGui::GetMousePos();
   if (pill.pending)
   {
      if (fabsf(mouse.x - pill.start.x) < 3.f && fabsf(mouse.y - pill.start.y) < 3.f)
         return; // A click and release changes nothing
      pill.pending = false;
      pill.active = true;
      ed.m_undo.BeginUndo();
      ed.m_undo.MarkForUndo(host);
      ed.m_undo.SetFieldLabel("Make Pill");
      ed.m_undo.EndUndo();
      const int n = static_cast<int>(curve.GetPoints().size());
      int at = n;
      for (int i = 0; i < n; i++)
         if (curve.GetPoints()[i].get() == pill.a)
            at = i + 1;
      ed.m_pointSel.clear();
      pill.a->SetSmooth(true); // Joined to the next point, which is the second end, with the same diameter
      curve.InsertPoint(at, std::make_unique<DragPoint>(&curve, pill.a->GetX(), pill.a->GetY(), pill.a->GetZ(), false));
      pill.b = curve.GetPoints()[at].get();
   }
   if (mouse.x == pill.last.x && mouse.y == pill.last.y)
      return; // Nothing moved: no rebuild
   pill.last = mouse;
   // The far end of the ring stays under the cursor
   const float z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
   const Vertex2D p = ed.UnprojectToPlane(mouse, z);
   const float r = 0.5f * pill.a->GetZ(), dx = p.x - pill.a->GetX(), dy = p.y - pill.a->GetY(), len = hypotf(dx, dy);
   if (len <= r + 0.05f)
   {
      pill.b->SetX(pill.a->GetX());
      pill.b->SetY(pill.a->GetY());
   }
   else
   {
      pill.b->SetX(p.x - dx / len * r);
      pill.b->SetY(p.y - dy / len * r);
   }
   curve.OnPointsModified();
   ed.m_renderer->ReinitRenderable(host->GetIRenderable());
   ed.m_renderer->InvalidateShadowContent();
}

bool PlasticFromAtlas::ScaleHoles(EditorUI &ed)
{
   Primitive *const host = EditedHost(ed);
   if (host == nullptr || !host->m_plasticEditHoles || !ed.m_modal.active || ed.m_modal.op != EditorUI::ModalOp::Scale)
      return false;
   const auto &pts = host->m_plasticHoles.GetPoints();
   if (ed.m_plasticHoleScaleInit.empty()) // The first evaluation: the diameters as they were
   {
      const vector<int> partner = Primitive::PlasticPillPartners(host->m_plasticHoles);
      for (int i = 0; i < static_cast<int>(pts.size()); i++)
         if (ed.IsPointSelected(pts[i].get()) || (partner[i] >= 0 && ed.IsPointSelected(pts[partner[i]].get())))
            ed.m_plasticHoleScaleInit.emplace_back(pts[i].get(), pts[i]->GetZ());
   }
   const float k = fabsf(ed.m_modal.lastFactor);
   for (const auto &[point, dia] : ed.m_plasticHoleScaleInit)
      point->SetZ(clamp(dia * k, 0.5f, 100.f));
   host->m_plasticHoles.OnPointsModified();
   return true;
}

void PlasticFromAtlas::BeforeDeletePoints(EditorUI &ed, const vector<DragPoint *> &points)
{
   Primitive *const host = EditedHost(ed);
   if (host == nullptr || !host->m_plasticEditHoles)
      return;
   const auto &pts = host->m_plasticHoles.GetPoints();
   const vector<int> partner = Primitive::PlasticPillPartners(host->m_plasticHoles);
   vector<DragPoint *> others;
   for (int i = 0; i < static_cast<int>(pts.size()); i++)
      if (partner[i] >= 0 && std::ranges::find(points, pts[i].get()) != points.end())
         others.push_back(pts[partner[i]].get());
   for (DragPoint *const o : others) // The pill loses an end: the end that stays is a round hole, whatever follows it in the list
      o->SetSmooth(false);
}

void PlasticFromAtlas::SetHoleDiameter(EditorUI &ed, const float dia)
{
   Primitive *const host = EditedHost(ed);
   if (host == nullptr)
      return;
   host->m_d.m_plasticHoleDia = clamp(dia, 0.5f, 100.f); // Always the next hole
   if (host->m_plasticEditHoles)
      for (const DragPoint *const p : ed.m_pointSel) // And the selected holes (both ends of a pill)
         host->SetPlasticHoleDiameter(p, dia);
   host->m_plasticHoles.OnPointsModified();
   ed.m_renderer->ReinitRenderable(host->GetIRenderable());
   ed.m_renderer->InvalidateShadowContent();
}

void PlasticFromAtlas::RenderEditPanel(EditorUI &ed)
{
   Primitive *const host = EditedHost(ed);
   if (host == nullptr || !ed.m_viewportVisible || ed.m_plasticTrace.active || ed.m_plasticFit.active)
      return;
   const float dpi = ed.m_liveUI.GetDPI();
   ImGui::SetNextWindowPos(ImVec2(ed.m_viewportContentRect.x0 + 8.f * dpi, ed.m_viewportContentRect.y1 - 8.f * dpi), ImGuiCond_Always, ImVec2(0.f, 1.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * dpi, 6.f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::V(EditorTheme::Border));
   if (ImGui::Begin("Plastic from Atlas##Edit", nullptr,
          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
             | ImGuiWindowFlags_NoMove))
   {
      ImGui::Text("Plastic from Atlas: edit shape (%s)", host->m_plasticEditHoles ? "holes" : "outline");
      ImGui::SameLine();
      if (ImGui::RadioButton("Outline", !host->m_plasticEditHoles) && host->m_plasticEditHoles)
      {
         host->m_plasticEditHoles = false;
         ed.m_pointSel.clear();
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Holes (H)", host->m_plasticEditHoles) && !host->m_plasticEditHoles)
      {
         host->m_plasticEditHoles = true;
         ed.m_pointSel.clear();
      }
      ImGui::SameLine();
      ImGui::Checkbox("Show Sheet", &ed.m_plasticShowSheet);
      ImGui::SetItemTooltip("The atlas drawn over the view. Off: the table and the plastic as they are");
      // Hole Diameter: the selected holes if holes are selected, and always the next hole
      float dia = host->m_d.m_plasticHoleDia;
      if (host->m_plasticEditHoles && !ed.m_pointSel.empty())
         dia = ed.m_pointSel.front()->GetZ();
      ImGui::SetNextItemWidth(70.f * dpi);
      const bool changed = ImGui::DragFloat("Hole Diameter", &dia, 0.1f, 0.5f, 100.f, "%.1f");
      if (ImGui::IsItemActivated())
      {
         ed.m_undo.BeginUndo();
         ed.m_undo.MarkForUndo(host);
         ed.m_undo.SetFieldLabel("Hole Diameter");
         ed.m_undo.EndUndo();
      }
      if (changed)
         SetHoleDiameter(ed, dia);
      ImGui::SetItemTooltip("Diameter of the selected holes (both ends of a pill), and of the next hole placed");
      int detail = host->m_d.m_plasticDetail;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110.f * dpi);
      const bool detailChanged = ImGui::SliderInt("Curve Detail", &detail, 1, 5);
      if (ImGui::IsItemActivated())
      {
         ed.m_undo.BeginUndo();
         ed.m_undo.MarkForUndo(host);
         ed.m_undo.SetFieldLabel("Curve Detail");
         ed.m_undo.EndUndo();
      }
      if (detailChanged)
      {
         host->m_d.m_plasticDetail = clamp(detail, 1, 5);
         ed.m_renderer->ReinitRenderable(host->GetIRenderable());
         ed.m_renderer->InvalidateShadowContent();
      }
      ImGui::SetItemTooltip("How smooth arcs and hole circles are (1 = default, at most 96 segments on any one arc)");
      // The Sheet DPI, and where it came from while that is known
      if (ed.m_plasticDpiNote.host == host && fabsf(ed.m_plasticDpiNote.dpi - host->m_d.m_plasticDpi) < 0.05f)
         ImGui::Text("Sheet DPI %.1f (%s)", host->m_d.m_plasticDpi, DpiSourceText(ed.m_plasticDpiNote.source));
      else
         ImGui::Text("Sheet DPI %.1f", host->m_d.m_plasticDpi);
      ImGui::SameLine();
      if (ImGui::Button("Fit to Posts"))
         FitStart(ed);
      ImGui::SetItemTooltip("Click two holes of the plastic, then the two posts under them: the plastic is scaled, turned and moved onto the posts");
   }
   ImGui::End();
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(2);
}

void PlasticFromAtlas::ViewportRightClick(EditorUI &ed)
{
   if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
   {
      ed.m_plasticRmbDown = true;
      ed.m_plasticRmbStart = ImGui::GetMousePos();
   }
   if (!ed.m_plasticRmbDown)
      return;
   if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
      return;
   ed.m_plasticRmbDown = false;
   const ImVec2 mouse = ImGui::GetMousePos();
   if (fabsf(mouse.x - ed.m_plasticRmbStart.x) > 4.f || fabsf(mouse.y - ed.m_plasticRmbStart.y) > 4.f) // A drag is not a click
      return;
   if (ed.m_plasticTrace.active || ed.m_plasticFit.active || ed.m_table->IsLocked() || ed.IsInspectMode() || ed.m_addPartType != eItemInvalid || ed.m_partLibrary.IsPlacementArmed() || ed.IsDragInProgress()
      || ed.m_boxSelectActive)
      return;
   if (ed.m_pointEditPart != nullptr)
   {
      if (EditedHost(ed) == nullptr)
         return;
      ed.m_plasticMenu = 1;
   }
   else
   {
      // On the selected plastic (any part under the cursor that stands for it)
      if (ed.m_multiSel.size() != 1 || !IsHostSelected(ed))
         return;
      IEditable *const host = ed.m_selection.GetPart()->GetEditable();
      vector<HitTestResult> hits;
      ed.RayCastParts(ed.m_plasticRmbStart, hits, nullptr);
      bool onIt = false;
      for (const auto &hit : hits)
         onIt |= ed.HitEditable(hit) == host;
      if (!onIt)
         return;
      ed.m_plasticMenu = 0;
   }
   ed.m_plasticMenuPos = mouse;
}

void PlasticFromAtlas::RenderMenu(EditorUI &ed)
{
   if (ed.m_plasticMenu >= 0 && ed.m_plasticMenuPos.x >= 0.f)
   {
      ImGui::SetNextWindowPos(ed.m_plasticMenuPos);
      ImGui::OpenPopup("##PlasticEditMenu");
      ed.m_plasticMenuPos = ImVec2(-1.f, -1.f); // Opened
   }
   if (!ImGui::BeginPopup("##PlasticEditMenu"))
      return;
   if (ed.m_plasticMenu == 0)
   {
      if (ImGui::MenuItem("Edit Shape", "Tab", false, ed.CanEnterPointEdit()))
         ed.EnterPointEditMode();
      if (ImGui::MenuItem("Fit to Posts", nullptr, false, CanFit(ed)))
         FitStart(ed);
   }
   else if (Primitive *const host = EditedHost(ed))
   {
      if (ImGui::MenuItem("Outline", "H", !host->m_plasticEditHoles) && host->m_plasticEditHoles)
      {
         host->m_plasticEditHoles = false;
         ed.m_pointSel.clear();
      }
      if (ImGui::MenuItem("Holes", "H", host->m_plasticEditHoles) && !host->m_plasticEditHoles)
      {
         host->m_plasticEditHoles = true;
         ed.m_pointSel.clear();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Show Sheet", nullptr, ed.m_plasticShowSheet))
         ed.m_plasticShowSheet = !ed.m_plasticShowSheet;
      ImGui::Separator();
      if (ImGui::MenuItem("Fit to Posts", nullptr, false, CanFit(ed)))
         FitStart(ed);
      ImGui::Separator();
      if (ImGui::MenuItem("Done Editing", "Tab"))
         ed.ExitPointEditMode(true);
   }
   ImGui::EndPopup();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Fit to Posts: two holes of the plastic, the two posts under them

namespace
{
struct HoleSpot
{
   const DragPoint *a, *b; // The centre (a == b), or the two ends of a pill
   float r;
   Vertex2D centre;
};

vector<HoleSpot> HoleSpots(const Primitive *const host)
{
   vector<HoleSpot> spots;
   const auto &pts = host->m_plasticHoles.GetPoints();
   const vector<int> partner = Primitive::PlasticPillPartners(host->m_plasticHoles);
   for (int i = 0; i < static_cast<int>(pts.size()); i++)
   {
      if (partner[i] >= 0 && partner[i] < i)
         continue; // The second end of a pill
      const DragPoint *const a = pts[i].get(), *const b = partner[i] > i ? pts[partner[i]].get() : a;
      spots.push_back({ a, b, 0.5f * a->GetZ(), Vertex2D(0.5f * (a->GetX() + b->GetX()), 0.5f * (a->GetY() + b->GetY())) }); // A pill counts as its middle
   }
   return spots;
}
}

bool PlasticFromAtlas::CanFit(const EditorUI &ed)
{
   if (ed.m_table->IsLocked() || ed.IsInspectMode() || ed.m_plasticTrace.active || ed.m_plasticFit.active || ed.m_modal.active || ed.m_addPartType != eItemInvalid)
      return false;
   return IsEditing(ed) || (ed.m_pointEditPart == nullptr && ed.m_multiSel.size() == 1 && IsHostSelected(ed));
}

void PlasticFromAtlas::FitStart(EditorUI &ed)
{
   if (!CanFit(ed))
      return;
   const bool editing = IsEditing(ed);
   Primitive *const host = editing ? EditedHost(ed) : static_cast<Primitive *>(ed.m_selection.GetPart()->GetEditable());
   const size_t holes = HoleSpots(host).size();
   if (holes < 2)
   {
      ed.Report("Fit to Posts: " + host->GetName() + " has " + (holes == 0 ? "no holes" : "1 hole") + ", it needs 2 to fit onto posts");
      return;
   }
   Fit f;
   f.active = true;
   f.host = host;
   f.editing = editing;
   f.selectHost = !editing;
   f.undoId = ed.m_undo.TopRecordId();
   ed.m_pointSel.clear(); // No gizmo over the holes
   ed.m_plasticBow = Bow();
   ed.m_plasticPill = Pill();
   ed.m_boxSelectActive = false;
   if (!editing)
      ed.ClearSelection();
   ed.m_plasticFit = f;
   ed.Report("Fit to Posts 1/4: click a hole of the plastic (Esc stops)");
}

void PlasticFromAtlas::FitCancel(EditorUI &ed, const char *const message)
{
   Fit &f = ed.m_plasticFit;
   if (!f.active)
      return;
   Primitive *const host = f.host;
   const bool reselect = f.selectHost;
   f = Fit();
   if (reselect && host != nullptr && !ed.m_plasticTrace.active)
      if (const auto it = ed.m_editableMap.find(host); it != ed.m_editableMap.end())
         ed.SetSelection(Selection(it->second));
   if (message != nullptr)
      ed.Report(message);
}

namespace
{
const char *const kFitPrompt[4] = { "click a hole of the plastic", "click a second hole", "click the post under the first hole (or its place on the playfield)",
   "click the post under the second hole" };
}

bool PlasticFromAtlas::FitInvalid(const EditorUI &ed)
{
   const Fit &f = ed.m_plasticFit;
   // Anything that changed the table (an undo, a delete), or left the shape edit, ends the steps
   return ed.m_table->IsLocked() || ed.IsInspectMode() || ed.m_plasticTrace.active || ed.m_undo.TopRecordId() != f.undoId || IsEditing(ed) != f.editing
      || std::ranges::find(ed.m_table->GetParts(), static_cast<IEditable *>(f.host)) == ed.m_table->GetParts().end() || (f.editing && EditedHost(ed) != f.host);
}

void PlasticFromAtlas::FitClick(EditorUI &ed, const ImVec2 &mouse)
{
   Fit &f = ed.m_plasticFit;
   Primitive *const host = f.host;
   if (!f.active || host == nullptr)
      return;
   if (FitInvalid(ed)) // Checked here too: the panel's check does not run in fly mode or with the full UI off
   {
      FitCancel(ed, "Fit to Posts stopped");
      return;
   }
   if (f.step < 2)
   {
      const float z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
      const Vertex2D p = ed.UnprojectToPlane(mouse, z), q = ed.UnprojectToPlane(ImVec2(mouse.x + 8.f * ed.m_liveUI.GetDPI(), mouse.y), z);
      const float tol = hypotf(q.x - p.x, q.y - p.y);
      const vector<HoleSpot> spots = HoleSpots(host);
      int best = -1;
      float bestDist = FLT_MAX;
      for (int i = 0; i < static_cast<int>(spots.size()); i++)
      {
         const float d = SegDist(p, spots[i].a, spots[i].b);
         if (d <= spots[i].r + tol && d < bestDist)
         {
            bestDist = d;
            best = i;
         }
      }
      if (best < 0)
      {
         ed.Report(string("Fit to Posts ") + std::to_string(f.step + 1) + "/4: that is not on a hole, " + kFitPrompt[f.step]);
         return;
      }
      if (f.step == 1 && (best == f.hole[0] || hypotf(spots[best].centre.x - f.holePos[0].x, spots[best].centre.y - f.holePos[0].y) < 0.05f))
      {
         ed.Report("Fit to Posts 2/4: that is the first hole again, click a different hole");
         return;
      }
      PLOGI << "Plastic from Atlas: fit hole " << f.step + 1 << " at table " << spots[best].centre.x << ' ' << spots[best].centre.y;
      f.hole[f.step] = best;
      f.holePos[f.step] = spots[best].centre;
      f.step++;
      ed.Report(string("Fit to Posts ") + std::to_string(f.step + 1) + "/4: " + kFitPrompt[f.step]);
      return;
   }

   // A post: the centre of the primitive under the cursor (as a guided rubber takes it), else that point of the playfield
   Vertex2D pos = ed.UnprojectToPlane(mouse, 0.f);
   string what = "the playfield";
   {
      vector<HitTestResult> hits;
      ed.RayCastParts(mouse, hits, nullptr);
      Vertex3Ds rayNear, rayFar;
      ed.PickRay(mouse, nullptr, rayNear, rayFar);
      for (const HitTestResult &hit : hits)
      {
         IEditable *const e = hit.m_obj != nullptr ? hit.m_obj->m_editable : nullptr;
         if (e == nullptr)
            break;
         if (e->GetItemType() == eItemPrimitive && (IsHost(e) || static_cast<Primitive *>(e)->IsPlasticCompanion()))
            continue; // A plastic is in the way (this one, or another): the post is under it
         if (e->GetItemType() == eItemPrimitive)
         {
            float x, y, r;
            // A post is clicked on its body: a click farther from the centre than its footprint is on something big (an insert, a plate) and means the playfield
            // The footprint is tested where the ray meets the part (hit.m_time runs 0..1 from the near to the far point), not at the playfield behind the click:
            // on a post standing 50 VPU high that point is far off in a tilted view
            const float hitX = rayNear.x + hit.m_time * (rayFar.x - rayNear.x), hitY = rayNear.y + hit.m_time * (rayFar.y - rayNear.y);
            if (RubberBetweenPosts::PostPosition(static_cast<Primitive *>(e), x, y, &r) && hypotf(hitX - x, hitY - y) <= r + 2.f)
            {
               pos = Vertex2D(x, y);
               what = e->GetName();
            }
         }
         break;
      }
   }
   const int i = f.step - 2;
   if (i == 1 && hypotf(pos.x - f.postPos[0].x, pos.y - f.postPos[0].y) < 0.05f)
   {
      ed.Report("Fit to Posts 4/4: that is the first post again, click a different one");
      return;
   }
   PLOGI << "Plastic from Atlas: fit post " << i + 1 << " at table " << pos.x << ' ' << pos.y << " (" << what << ")";
   f.postPos[i] = pos;
   if (i == 0)
   {
      f.step++;
      char text[160];
      snprintf(text, sizeof(text), "Fit to Posts 4/4: %s (first post: %s at %.1f %.1f)", kFitPrompt[3], what.c_str(), pos.x, pos.y);
      ed.Report(text);
      return;
   }

   // Both posts known: one uniform scale, a turn and a move that bring the two holes onto the posts, in one undo step
   const Vertex2D h1 = f.holePos[0], h2 = f.holePos[1], p1 = f.postPos[0], p2 = f.postPos[1];
   const float dh = hypotf(h2.x - h1.x, h2.y - h1.y), dp = hypotf(p2.x - p1.x, p2.y - p1.y);
   const float scale = dp / dh;
   float angle = RADTOANG(atan2f(p2.y - p1.y, p2.x - p1.x) - atan2f(h2.y - h1.y, h2.x - h1.x));
   while (angle > 180.f)
      angle -= 360.f;
   while (angle <= -180.f)
      angle += 360.f;
   const float oldDpi = host->m_d.m_plasticDpi;
   ed.m_undo.BeginUndo();
   ed.m_undo.MarkForUndo(host);
   ed.m_undo.SetFieldLabel("Fit to Posts");
   ed.m_undo.EndUndo();
   host->Scale(scale, scale, h1, false); // The existing transforms: the art stays on the part, the holes scale with it
   host->Rotate(angle, h1, false);
   host->Translate(Vertex2D(p1.x - h1.x, p1.y - h1.y));
   host->m_d.m_plasticDpi = clamp(oldDpi / scale, 10.f, 4800.f); // The sheet's pixels per inch follow the scale
   ed.m_plasticDpiNote = { host, host->m_d.m_plasticDpi, DpiFitted };
   ed.m_renderer->ReinitRenderable(host->GetIRenderable());
   ed.m_renderer->InvalidateShadowContent();
   ed.m_player->m_physics->Update(host);
   {
      const vector<HoleSpot> after = HoleSpots(host);
      float r1 = -1.f, r2 = -1.f;
      if (f.hole[0] < static_cast<int>(after.size()) && f.hole[1] < static_cast<int>(after.size()))
      {
         r1 = hypotf(after[f.hole[0]].centre.x - p1.x, after[f.hole[0]].centre.y - p1.y);
         r2 = hypotf(after[f.hole[1]].centre.x - p2.x, after[f.hole[1]].centre.y - p2.y);
      }
      PLOGI << "Plastic from Atlas: fit to posts scale " << scale << " turn " << angle << " deg, holes now " << r1 << " and " << r2 << " VPU from the posts, Sheet DPI " << oldDpi << " -> " << host->m_d.m_plasticDpi;
   }
   char text[256];
   snprintf(text, sizeof(text), "Fit to Posts: scaled %.3fx, turned %.1f deg, Sheet DPI %.1f (fitted)%s", scale, angle, host->m_d.m_plasticDpi,
      scale > 1.5f || scale < 0.67f ? "  Warning: a big change of size, check the two holes and the two posts" : "");
   FitCancel(ed); // Done: back to the selection (or the shape edit)
   ed.Report(text);
}

void PlasticFromAtlas::RenderFitPanel(EditorUI &ed)
{
   Fit &f = ed.m_plasticFit;
   if (!f.active || !ed.m_viewportVisible)
      return;
   if (FitInvalid(ed))
   {
      FitCancel(ed, "Fit to Posts stopped");
      return;
   }
   const float dpi = ed.m_liveUI.GetDPI();
   ImGui::SetNextWindowPos(ImVec2(ed.m_viewportContentRect.x0 + 8.f * dpi, ed.m_viewportContentRect.y1 - 8.f * dpi), ImGuiCond_Always, ImVec2(0.f, 1.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * dpi, 6.f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::V(EditorTheme::Border));
   if (ImGui::Begin("Plastic from Atlas##Fit", nullptr,
          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
             | ImGuiWindowFlags_NoMove))
   {
      ImGui::Text("Fit to Posts %d/4: %s", f.step + 1, kFitPrompt[f.step]);
      ImGui::SameLine();
      if (ImGui::Button("Stop (Esc)"))
         FitCancel(ed, "Fit to Posts stopped");
   }
   ImGui::End();
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(2);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing

void PlasticFromAtlas::DrawBackdrop(EditorUI &ed, ImDrawList *const draw, const LiveRenderContext &ctx)
{
   const float *uvm = nullptr;
   float z = 0.f; // the plane of the handles: the top of a placed plastic, so the backdrop lines up in perspective too
   string image;
   if (ed.m_plasticTrace.active)
   {
      uvm = ed.m_plasticTrace.uvm;
      image = ed.m_plasticTrace.image;
   }
   else if (const Primitive *const host = EditedHost(ed); host != nullptr && ed.m_plasticShowSheet)
   {
      uvm = host->m_d.m_plasticUvm;
      image = host->m_d.m_szImage;
      z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
   }
   if (uvm == nullptr || image.empty())
      return;
   Texture *const texture = ed.m_table->GetImage(image);
   if (texture == nullptr)
      return;
   float m[6];
   for (int i = 0; i < 6; i++)
      m[i] = uvm[i];
   const auto project = [&](const float u, const float v)
   {
      const Vertex2D t = UvToTable(m, u, v);
      return ctx.Project(Vertex3Ds(t.x, t.y, z));
   };
   // Orthographic like the gizmo's test: the desktop backdrop, or the editor camera with perspective off (the preview camera is always perspective)
   const bool ortho = ed.m_camMode == ViewMode::DesktopBackdrop || (ed.m_camMode == ViewMode::EditorCam && !ed.m_perspectiveCam);
   const auto sampler = ed.m_renderer->m_renderDevice->m_texMan.LoadTexture(texture, false);
   if (ortho)
   {
      const float us[4] = { 0.f, 1.f, 1.f, 0.f }, vs[4] = { 0.f, 0.f, 1.f, 1.f };
      ImVec2 p[4];
      for (int i = 0; i < 4; i++)
      {
         p[i] = project(us[i], vs[i]);
         if (p[i].x == FLT_MAX)
            return;
      }
      // A dark sheet under the atlas (its alpha shows the cut-outs against it)
      draw->AddQuadFilled(p[0], p[1], p[2], p[3], IM_COL32(18, 18, 22, 235));
      if (sampler)
         draw->AddImageQuad(sampler, p[0], p[1], p[2], p[3], ImVec2(0.f, 0.f), ImVec2(1.f, 0.f), ImVec2(1.f, 1.f), ImVec2(0.f, 1.f));
      return;
   }
   // Perspective: a quad is only affine, so the sheet is a grid of projected cells and stays on the points. A cell with a corner behind the camera is
   // skipped, the others are drawn (the dark sheet too)
   constexpr int N = 16;
   ImVec2 g[(N + 1) * (N + 1)];
   for (int j = 0; j <= N; j++)
      for (int i = 0; i <= N; i++)
         g[j * (N + 1) + i] = project(static_cast<float>(i) / N, static_cast<float>(j) / N);
   for (int j = 0; j < N; j++)
      for (int i = 0; i < N; i++)
      {
         const ImVec2 &a = g[j * (N + 1) + i], &b = g[j * (N + 1) + i + 1], &c = g[(j + 1) * (N + 1) + i + 1], &d = g[(j + 1) * (N + 1) + i];
         if (a.x == FLT_MAX || b.x == FLT_MAX || c.x == FLT_MAX || d.x == FLT_MAX)
            continue;
         draw->AddQuadFilled(a, b, c, d, IM_COL32(18, 18, 22, 235));
      }
   if (!sampler)
      return;
   for (int j = 0; j < N; j++)
      for (int i = 0; i < N; i++)
      {
         const ImVec2 &a = g[j * (N + 1) + i], &b = g[j * (N + 1) + i + 1], &c = g[(j + 1) * (N + 1) + i + 1], &d = g[(j + 1) * (N + 1) + i];
         if (a.x == FLT_MAX || b.x == FLT_MAX || c.x == FLT_MAX || d.x == FLT_MAX)
            continue;
         const float u0 = static_cast<float>(i) / N, u1 = static_cast<float>(i + 1) / N, v0 = static_cast<float>(j) / N, v1 = static_cast<float>(j + 1) / N;
         draw->AddImageQuad(sampler, a, b, c, d, ImVec2(u0, v0), ImVec2(u1, v0), ImVec2(u1, v1), ImVec2(u0, v1));
      }
}

static constexpr ImU32 kOutlineColor = IM_COL32(255, 60, 220, 255);
static constexpr ImU32 kSelectedColor = IM_COL32(255, 128, 0, 255);

// A line that stays readable over any art: the colour on a dark edge
static void DrawOutlineSegment(ImDrawList *const draw, const ImVec2 &a, const ImVec2 &b, const float dpi, const ImU32 color = kOutlineColor)
{
   draw->AddLine(a, b, IM_COL32(0, 0, 0, 200), 3.5f * dpi);
   draw->AddLine(a, b, color, 1.5f * dpi);
}

void PlasticFromAtlas::DrawPointEdit(EditorUI &ed, ImDrawList *const draw, const LiveRenderContext &ctx)
{
   const Primitive *const host = EditedHost(ed);
   if (host == nullptr)
      return;
   const float dpi = ed.m_liveUI.GetDPI();
   const float z = host->m_d.m_plasticHeight + host->m_d.m_plasticThickness;
   const float tol = PlasticMesh::FlattenTolerance(host->m_d.m_plasticDetail); // The flatten of the mesh
   vector<PlasticMesh::CurvePoint> pts;
   for (const auto &point : host->m_plasticCurve.GetPoints())
      pts.push_back({ point->GetX(), point->GetY(), point->IsSmooth() });
   if (pts.size() >= 3)
   {
      const vector<PlasticMesh::CurvePoint> flat = PlasticMesh::FlattenOutline(pts, tol);
      for (size_t i = 0; i < flat.size(); i++)
      {
         const PlasticMesh::CurvePoint &n = flat[(i + 1) % flat.size()];
         const ImVec2 a = ctx.Project(Vertex3Ds(flat[i].x, flat[i].y, z)), b = ctx.Project(Vertex3Ds(n.x, n.y, z));
         if (a.x != FLT_MAX && b.x != FLT_MAX)
            DrawOutlineSegment(draw, a, b, dpi);
      }
   }
   // Holes: the loop the mesh is built from (a circle, or a capsule for a pill), a light dark fill so it reads as a hole over the art, orange when selected
   const auto &hp = host->m_plasticHoles.GetPoints();
   const vector<int> partner = Primitive::PlasticPillPartners(host->m_plasticHoles);
   for (int i = 0; i < static_cast<int>(hp.size()); i++)
   {
      if (partner[i] >= 0 && partner[i] < i)
         continue;
      PlasticMesh::Hole h { hp[i]->GetX(), hp[i]->GetY(), hp[i]->GetZ() };
      bool selected = ed.IsPointSelected(hp[i].get());
      if (partner[i] > i)
      {
         h.x2 = hp[partner[i]]->GetX();
         h.y2 = hp[partner[i]]->GetY();
         h.pill = true;
         selected |= ed.IsPointSelected(hp[partner[i]].get());
      }
      const vector<PlasticMesh::CurvePoint> loop = PlasticMesh::HoleOutline(h, tol);
      vector<ImVec2> sp;
      for (const PlasticMesh::CurvePoint &v : loop)
      {
         const ImVec2 s = ctx.Project(Vertex3Ds(v.x, v.y, z));
         if (s.x == FLT_MAX)
         {
            sp.clear();
            break;
         }
         sp.push_back(s);
      }
      if (sp.size() < 3)
         continue;
      float area = 0.f; // Same winding for the fill's edge
      for (size_t k = 0; k < sp.size(); k++)
         area += sp[k].x * sp[(k + 1) % sp.size()].y - sp[(k + 1) % sp.size()].x * sp[k].y;
      if (area < 0.f)
         std::reverse(sp.begin(), sp.end());
      draw->AddConvexPolyFilled(sp.data(), static_cast<int>(sp.size()), IM_COL32(8, 8, 12, 90));
      for (size_t k = 0; k < sp.size(); k++)
         DrawOutlineSegment(draw, sp[k], sp[(k + 1) % sp.size()], dpi, selected ? kSelectedColor : kOutlineColor);
   }
}

void PlasticFromAtlas::DrawTrace(EditorUI &ed, ImDrawList *const draw, const LiveRenderContext &ctx)
{
   const Trace &t = ed.m_plasticTrace;
   if (!t.active)
      return;
   const float dpi = ed.m_liveUI.GetDPI();
   const auto project = [&ctx](const float x, const float y) { return ctx.Project(Vertex3Ds(x, y, 0.f)); };

   // The outline as it will be built (arcs flattened), closed
   if (t.pts.size() >= 2)
   {
      vector<PlasticMesh::CurvePoint> pts;
      for (size_t i = 0; i < t.pts.size(); i++)
         pts.push_back({ t.pts[i].x, t.pts[i].y, static_cast<bool>(t.smooth[i]) });
      vector<PlasticMesh::CurvePoint> flat = t.pts.size() >= 3 ? PlasticMesh::FlattenOutline(pts, 0.25f) : pts;
      for (size_t i = 0; i < flat.size(); i++)
      {
         if (i + 1 == flat.size() && t.pts.size() < 3)
            break;
         const ImVec2 a = project(flat[i].x, flat[i].y), b = project(flat[(i + 1) % flat.size()].x, flat[(i + 1) % flat.size()].y);
         if (a.x != FLT_MAX && b.x != FLT_MAX)
            DrawOutlineSegment(draw, a, b, dpi);
      }
   }
   for (size_t i = 0; i < t.pts.size(); i++)
   {
      const ImVec2 pos = project(t.pts[i].x, t.pts[i].y);
      if (pos.x == FLT_MAX)
         continue;
      const float radius = (i == 0 ? 5.f : 4.f) * dpi;
      draw->AddCircleFilled(pos, radius, t.smooth[i] ? IM_COL32(80, 160, 255, 255) : IM_COL32(255, 96, 96, 255), 12);
      draw->AddCircle(pos, radius + dpi, IM_COL32(0, 0, 0, 255), 12, 1.5f);
   }
   const auto circle = [&](const float x, const float y, const float dia, const ImU32 col, const float thickness)
   {
      const ImVec2 c = project(x, y), e = project(x + 0.5f * dia, y);
      if (c.x == FLT_MAX || e.x == FLT_MAX)
         return;
      draw->AddCircle(c, max(2.f, sqrtf((e.x - c.x) * (e.x - c.x) + (e.y - c.y) * (e.y - c.y))), col, 32, thickness);
   };
   for (const Vertex3Ds &h : t.holeList)
      circle(h.x, h.y, h.z, IM_COL32(90, 230, 120, 255), 1.5f * dpi);
   if (t.measure != 0)
   {
      ImVec2 mp[2];
      for (int i = 0; i < t.measure - 1 && i < 2; i++)
      {
         const Vertex2D q = UvToTable(t.uvm, t.measureUv[2 * i], t.measureUv[2 * i + 1]);
         mp[i] = project(q.x, q.y);
      }
      const ImU32 col = IM_COL32(255, 60, 220, 255);
      if (t.measure == 3 && mp[0].x != FLT_MAX && mp[1].x != FLT_MAX)
         draw->AddLine(mp[0], mp[1], col, 1.5f * dpi);
      else if (t.measure == 2 && mp[0].x != FLT_MAX && !ImGui::GetIO().WantCaptureMouse)
         draw->AddLine(mp[0], ImGui::GetMousePos(), IM_COL32(255, 60, 220, 140), dpi);
      for (int i = 0; i < t.measure - 1 && i < 2; i++)
         if (mp[i].x != FLT_MAX)
         {
            draw->AddCircle(mp[i], 5.f * dpi, col, 12, 1.5f * dpi);
            draw->AddCircleFilled(mp[i], 1.5f * dpi, col, 6);
         }
   }
   if (t.holes && !ImGui::GetIO().WantCaptureMouse)
   {
      const Vertex2D p = ed.UnprojectToPlane(ImGui::GetMousePos(), 0.f);
      circle(p.x, p.y, t.holeDia, IM_COL32(90, 230, 120, 140), dpi);
   }
   else if (!t.holes && !t.pts.empty() && !ImGui::GetIO().WantCaptureMouse)
   {
      const ImVec2 last = project(t.pts.back().x, t.pts.back().y);
      if (last.x != FLT_MAX)
         draw->AddLine(last, ImGui::GetMousePos(), IM_COL32(255, 60, 220, 110), dpi);
   }
}

void PlasticFromAtlas::RenderPanel(EditorUI &ed)
{
   Trace &t = ed.m_plasticTrace;
   if (!t.active || !ed.m_viewportVisible)
      return;
   const float dpi = ed.m_liveUI.GetDPI();
   ImGui::SetNextWindowPos(ImVec2(ed.m_viewportContentRect.x0 + 8.f * dpi, ed.m_viewportContentRect.y1 - 8.f * dpi), ImGuiCond_Always, ImVec2(0.f, 1.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * dpi, 6.f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Bar));
   ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::V(EditorTheme::Border));
   if (ImGui::Begin("Plastic from Atlas##Trace", nullptr,
          ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
             | ImGuiWindowFlags_NoMove))
   {
      ImGui::Text("Plastic from Atlas: trace    %d point(s), %d hole(s)", static_cast<int>(t.pts.size()), static_cast<int>(t.holeList.size()));
      ApplyLoadedImage(ed);
      ImGui::SetNextItemWidth(300.f * dpi);
      const bool comboOpen = ImGui::BeginCombo("Atlas", t.image.c_str());
      if (comboOpen)
      {
         // A search box on top (Enter takes the first match), the images sorted by name
         if (ImGui::IsWindowAppearing())
         {
            t.atlasFilter.clear();
            ImGui::SetKeyboardFocusHere();
         }
         ImGui::SetNextItemWidth(-FLT_MIN);
         const bool enter = ImGui::InputTextWithHint("##AtlasSearch", "Search by name", &t.atlasFilter, ImGuiInputTextFlags_EnterReturnsTrue);
         if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) // The Esc that closes the list is not the Esc that cancels the add
            ed.m_suppressNextEscape = true;
         vector<const Texture *> images;
         const string needle = lowerCase(t.atlasFilter);
         for (const Texture *const tex : ed.m_table->GetImageList())
            if (needle.empty() || lowerCase(tex->m_name).find(needle) != string::npos)
               images.push_back(tex);
         std::ranges::sort(images, [](const Texture *a, const Texture *b) { return lowerCase(a->m_name) < lowerCase(b->m_name); });
         const Texture *chosen = nullptr;
         if (enter && !images.empty())
            chosen = images.front();
         else if (enter)
            ImGui::SetKeyboardFocusHere(-1); // No match: the search box keeps the focus
         for (const Texture *const tex : images)
            if (ImGui::Selectable(tex->m_name.c_str(), tex->m_name == t.image))
               chosen = tex;
         if (images.empty())
            ImGui::TextDisabled("No image matches");
         if (chosen != nullptr)
         {
            if (chosen->m_name != t.image)
               SelectAtlas(ed, chosen->m_name);
            ImGui::CloseCurrentPopup();
         }
         ImGui::EndCombo();
      }
      ImGui::SameLine();
      if (ImGui::Button("Load Image"))
         LoadAtlasImage(ed);
      ImGui::SetItemTooltip("Add an image file (png, jpg, webp) to the table and use it as the atlas");
      ImGui::SetNextItemWidth(90.f * dpi);
      if (ImGui::DragFloat("Sheet DPI", &t.dpi, 0.5f, 10.f, 2400.f, "%.1f"))
      {
         t.dpiSource = DpiTyped;
         SetSheet(ed);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) // Keep the whole sheet in view at its new size
      {
         const Vertex2D a = UvToTable(t.uvm, 0.f, 0.f), b = UvToTable(t.uvm, 1.f, 1.f);
         ed.FrameBounds(FRect3D(a.x, b.x, a.y, b.y, 0.f, 0.f));
      }
      ImGui::SetItemTooltip("47.06 VPU per inch at this DPI. Traced points stay where they are, the sheet is rescaled about its centre");
      ImGui::SameLine();
      ImGui::TextDisabled("(%s)", DpiSourceText(t.dpiSource));
      ImGui::SetItemTooltip("Where the Sheet DPI came from: the image file, the number in its name, the default 150, a measured distance, or typed");
      ImGui::SameLine();
      if (ImGui::Button(t.measure != 0 ? "Measuring" : "Measure"))
         t.measure = t.measure != 0 ? 0 : 1;
      ImGui::SetItemTooltip("Click two points on the sheet that are a known distance apart (two drill holes), then type the distance in inches and Apply");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.f * dpi);
      ImGui::DragFloat("Hole", &t.holeDia, 0.1f, 0.5f, 100.f, "%.1f");
      ImGui::SetItemTooltip("Hole diameter");
      ImGui::SameLine();
      if (ImGui::Button("M3 6.4"))
         t.holeDia = 6.4f;
      ImGui::SameLine();
      if (ImGui::Button("Post 12"))
         t.holeDia = 12.f;
      ImGui::SameLine();
      if (ImGui::Button("Large 16"))
         t.holeDia = 16.f;
      if (t.measure != 0)
      {
         if (t.measure < 3)
            ImGui::Text("Measure: click %s point on the sheet (Esc to stop measuring)", t.measure == 1 ? "the first" : "the second");
         else
         {
            const Texture *const tex = ed.m_table->GetImage(t.image);
            const float w = tex != nullptr && tex->m_width > 0 ? static_cast<float>(tex->m_width) : 1024.f;
            const float h = tex != nullptr && tex->m_height > 0 ? static_cast<float>(tex->m_height) : 1024.f;
            const float px = hypotf((t.measureUv[2] - t.measureUv[0]) * w, (t.measureUv[3] - t.measureUv[1]) * h);
            ImGui::Text("Measure: %.1f px apart", px);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.f * dpi);
            ImGui::InputFloat("Distance (in)", &t.measureIn, 0.f, 0.f, "%.3f");
            ImGui::SetItemTooltip("The real distance between the two points in inches (47.06 VPU per inch)");
            if (t.measureIn < 0.f)
               t.measureIn = 0.f;
            // After the field, so a value committed by clicking Apply is already in
            const float newDpi = t.measureIn > 0.f ? px / t.measureIn : 0.f;
            const bool ok = px >= 0.5f && newDpi >= 10.f && newDpi <= 2400.f;
            ImGui::SameLine();
            ImGui::BeginDisabled(!ok);
            if (ImGui::Button("Apply"))
            {
               PLOGI << "Plastic from Atlas: measure apply " << px << " px / " << t.measureIn << " in = " << newDpi << " dpi";
               t.dpi = newDpi;
               t.dpiSource = DpiMeasured;
               t.measure = 0;
               SetSheet(ed);
               const Vertex2D a = UvToTable(t.uvm, 0.f, 0.f), b = UvToTable(t.uvm, 1.f, 1.f);
               ed.FrameBounds(FRect3D(a.x, b.x, a.y, b.y, 0.f, 0.f));
               ed.Report("Plastic from Atlas: Sheet DPI set from the measured distance");
            }
            ImGui::EndDisabled();
            if (ok)
               ImGui::SetItemTooltip("Sheet DPI becomes %.1f", newDpi);
            ImGui::SameLine();
            if (ImGui::Button("Redo"))
               t.measure = 1;
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
               t.measure = 0;
         }
      }
      if (ImGui::RadioButton("Outline", !t.holes))
         t.holes = false;
      ImGui::SameLine();
      if (ImGui::RadioButton("Holes (H)", t.holes))
         t.holes = true;
      ImGui::SameLine();
      if (ImGui::Button("Close Loop (Enter)"))
         Finish(ed);
      ImGui::SameLine();
      if (ImGui::Button("Cancel (Esc)"))
         Cancel(ed);
   }
   ImGui::End();
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(2);
}

}
