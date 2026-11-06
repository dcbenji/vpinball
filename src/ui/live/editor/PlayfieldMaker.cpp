// license:GPLv3+

#include "core/stdafx.h"
#include "core/editablereg.h"
#include "core/player.h"
#include "core/FileLocator.h"
#include "core/VPApp.h"
#include "core/SettingsService.h"
#include "PlayfieldMaker.h"
#include "PropertyPane.h"
#include "EditorUIPart.h"
#include "LiveRenderContext.h"
#include "Selection.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include "math/MaskTrace.h"
#include "math/PlasticMesh.h"
#include "parts/Material.h"
#include "parts/PartGroup.h"
#include "parts/primitive.h"
#include "physics/PhysicsEngine.h"
#include "renderer/Renderer.h"
#include "renderer/Texture.h"

namespace VPX::EditorUI
{

namespace
{
constexpr float kVPUPerInch = 47.06f;
constexpr const char *kPlywood = "Playfield Plywood"; // The image and the material of the walls

enum Slot { Paint, Shine, Bump, InsertPrint, WoodMask, Plywood, Glow, Blueprint, SlotCount };
constexpr const char *kSlotNames[SlotCount] = { "Paint", "Shine", "Bump", "Insert Print", "Wood Mask", "Plywood", "Glow", "Blueprint" };
constexpr const char *kSlotTips[SlotCount] = {
   "The table's playfield image (albedo)",
   "Occlusion, roughness and metal packed (the board's ORM map)",
   "The board's normal map",
   "The print over the traced inserts: rgb the art, alpha clear where the insert shows",
   "Holes as black on white (or white on black, or transparent): Find Holes reads it, it is not drawn",
   "The image of the hole walls; its last rows are the dark backing",
   "The board's emission map",
   "Opens Reference Image with this file (view only, not table data)",
};

struct Preset
{
   const char *name;
   bool slot;
   float a, b; // round: diameter; slot: length (end to end) and width, inches
};
constexpr Preset kPresets[] = { { "Flipper shaft 0.504 in", false, 0.504f, 0.f }, { "GI 0.58 in", false, 0.58f, 0.f }, { "Standup opening 1.1 in", false, 1.1f, 0.f },
   { "Rollover slot 1.69 x 0.185 in", true, 1.69f, 0.185f } };

// One editor per session: the maker's editor state
struct State
{
   int placing = 0; // 0 none, 1 round, 2 slot
   float roundDia = 0.58f * kVPUPerInch;
   float slotLength = 1.69f * kVPUPerInch, slotWidth = 0.185f * kVPUPerInch;
   const Primitive *board = nullptr; // The board the selected hole belongs to
   int selected = -1;
   bool grabbing = false;
   PlayfieldHole grabOrigin;
   Vertex2D grabStart { 0.f, 0.f };
   bool rebuildOnRelease = false; // A number field edit: the board is rebuilt once the field is let go, never per frame of a drag
   std::shared_ptr<vector<string>> pendingLoad; // The Load... dialog's answer (it comes back on another thread)
   int pendingSlot = -1;
   string dropPath;
   ImVec2 dropPos { 0.f, 0.f };
   int dropFrame = -1;
   struct SlotRect
   {
      int slot;
      ImVec2 a, b;
   };
   vector<SlotRect> rects; // The slot rows as drawn last frame (screen coordinates), for the drop test
   bool showGlow = false, showBlueprint = false;
   bool printAll = false;
   uint64_t floorsKey = ~0ull;
   size_t floorsParts = 0;
   bool floorsStale = true;
   vector<Primitive *> floors; // Collidable floor colliders (cached until the history or the part count changes)
   string findStatus; // The last Find Holes result line, shown only for the board and mask image it was made for
   bool findStatusBad = false;
   const Primitive *findBoard = nullptr;
   string findImage;
} s;


int SelectedHole(const Primitive *board)
{
   if (board == nullptr || s.board != board || s.selected < 0 || s.selected >= static_cast<int>(board->m_pfHoles.size()))
      return -1;
   return s.selected;
}

void SelectHole(const Primitive *board, const int index)
{
   s.board = board;
   s.selected = index;
}



Primitive *OldPlayfield(const PinTable *t)
{
   for (IEditable *const part : t->GetParts())
      if (part->GetItemType() == eItemPrimitive && !t->IsPlayerSessionObject(part) && Primitive::IsOldPlayfieldName(part->GetName()))
         return static_cast<Primitive *>(part);
   return nullptr;
}

// The values a floor collider plays with (its physics material unless overwritten)
void EffectivePhysics(const PinTable *t, const Primitive *p, float &friction, float &elasticity, float &falloff, float &scatter)
{
   const Material *const mat = t->GetMaterial(p->m_d.m_szPhysicsMaterial);
   if (!p->m_d.m_overwritePhysics && mat != nullptr && !t->IsDummyMaterial(mat))
   {
      friction = mat->m_fFriction;
      elasticity = mat->m_fElasticity;
      falloff = mat->m_fElasticityFalloff;
      scatter = mat->m_fScatterAngle;
   }
   else
   {
      friction = p->m_d.m_friction;
      elasticity = p->m_d.m_elasticity;
      falloff = p->m_d.m_elasticityFalloff;
      scatter = p->m_d.m_scatter;
   }
}

// Own floor: the values of the separate floor collider it replaces, else the table's effective playfield values, which it then follows. Decided here (when
// Own is chosen, and at Add) and kept on the board (m_pfFloorFollows): a later Turn Collision Off on that collider does not change it, and once the floor took
// a collider's values (at Add, while Keep Existing), choosing Own after that collider's collision was turned off keeps them
void CopyFloorPhysics(PinTable *const t, Primitive *const board, Primitive *const floor)
{
   if (board == nullptr || floor == nullptr)
      return;
   floor->m_d.m_overwritePhysics = true;
   for (const Primitive *const p : Primitive::FindPlayfieldFloors(t))
      if (!Primitive::IsOldPlayfieldName(p->GetName()))
      {
         EffectivePhysics(t, p, floor->m_d.m_friction, floor->m_d.m_elasticity, floor->m_d.m_elasticityFalloff, floor->m_d.m_scatter);
         board->m_d.m_pfFloorFollows = false;
         return;
      }
   if (!board->m_d.m_pfFloorFollows)
      return; // It replaced a separate floor collider (its collision off since): it keeps that one's values
   Primitive::GetTablePlayfieldPhysics(t, floor->m_d.m_friction, floor->m_d.m_elasticity, floor->m_d.m_elasticityFalloff, floor->m_d.m_scatter);
   board->m_d.m_pfFloorFollows = true;
}

// A tooltip that also shows on a disabled item (the greyed controls say why they are greyed)
void DisabledTooltip(const char *text)
{
   if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
      ImGui::SetTooltip("%s", text);
}


Material *EnsurePlywoodMaterial(PinTable *const t)
{
   if (Material *const existing = t->GetMaterial(kPlywood); !t->IsDummyMaterial(existing))
      return existing;
   Material *const m = new Material();
   m->m_name = kPlywood;
   m->m_type = Material::PBR;
   m->m_cBase = 0xFFFFFF;
   m->m_fRoughness = 0.75f;
   m->m_fMetallic = 0.f;
   m->m_fOpacity = 1.f;
   m->m_bOpacityActive = false;
   t->AddMaterial(m); // Not undone (a harmless leftover; a material is no part)
   t->SetNonUndoableDirty(SaveDirtyState::Dirty);
   return m;
}



vector<Primitive *> TracedCards(const PinTable *t)
{
   vector<Primitive *> res;
   for (IEditable *const part : t->GetParts())
      if (part->GetItemType() == eItemPrimitive && static_cast<Primitive *>(part)->IsTracedInsert())
         res.push_back(static_cast<Primitive *>(part));
   return res;
}

// The print image: the one most traced cards use
string CurrentPrint(const vector<Primitive *> &cards)
{
   std::map<string, int> count;
   for (const Primitive *const c : cards)
      if (!c->m_d.m_szORMMap.empty())
         count[c->m_d.m_szORMMap]++;
   string best;
   int n = 0;
   for (const auto &[name, k] : count)
      if (k > n)
      {
         best = name;
         n = k;
      }
   return best;
}





// Find Holes: traces the Wood Mask into holes. Replaces the holes a previous run made (FromMask), keeps the hand-placed ones. Returns the status line.
string FindHoles(const PinTable *const t, Primitive *const board, bool &ok)
{
   ok = false;
   const Texture *const tex = board->m_d.m_pfWoodMask.empty() ? nullptr : t->GetImage(board->m_d.m_pfWoodMask);
   if (tex == nullptr)
      return "The Wood Mask image is missing from the table";
   const auto t0 = std::chrono::steady_clock::now();
   std::shared_ptr<const BaseTexture> bmp = tex->GetRawBitmap(false, 0);
   if (bmp == nullptr || bmp->width() == 0 || bmp->height() == 0)
      return "The Wood Mask image could not be read";
   int channels = 4;
   vector<uint8_t> conv; // float and 565 masks become 8-bit (a mask is black and white: clamp to 0..1)
   const uint8_t *pixels = static_cast<const uint8_t *>(bmp->datac());
   size_t stride = bmp->pitch();
   const size_t w = bmp->width(), h = bmp->height();
   const auto half = [](const uint16_t v) -> float
   {
      const int e = (v >> 10) & 31, m = v & 1023;
      const float f = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? (m ? 0.f : 65504.f) : std::ldexp(static_cast<float>(m + 1024), e - 25);
      return (v & 0x8000) ? -f : f;
   };
   const auto to8 = [](const float f) { return static_cast<uint8_t>(clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
   switch (bmp->m_format)
   {
   case BaseTexture::BW: channels = 1; break;
   case BaseTexture::RGB:
   case BaseTexture::SRGB: channels = 3; break;
   case BaseTexture::RGBA:
   case BaseTexture::SRGBA: channels = 4; break;
   case BaseTexture::BW_FP32:
   case BaseTexture::RGB_FP32:
   case BaseTexture::RGBA_FP32:
   case BaseTexture::RGB_FP16:
   case BaseTexture::RGBA_FP16:
   {
      const bool fp16 = bmp->m_format == BaseTexture::RGB_FP16 || bmp->m_format == BaseTexture::RGBA_FP16;
      channels = bmp->m_format == BaseTexture::BW_FP32 ? 1 : (bmp->m_format == BaseTexture::RGB_FP32 || bmp->m_format == BaseTexture::RGB_FP16) ? 3 : 4;
      conv.resize(w * h * channels);
      for (size_t y = 0; y < h; y++)
         for (size_t i = 0; i < w * channels; i++)
         {
            const uint8_t *const src = pixels + y * stride;
            float f;
            if (fp16)
               f = half(reinterpret_cast<const uint16_t *>(src)[i]);
            else
               memcpy(&f, src + i * sizeof(float), sizeof(float));
            conv[y * w * channels + i] = to8(f);
         }
      pixels = conv.data();
      stride = w * channels;
      break;
   }
   case BaseTexture::SRGB565:
      channels = 3;
      conv.resize(w * h * 3);
      for (size_t y = 0; y < h; y++)
         for (size_t x = 0; x < w; x++)
         {
            const uint16_t v = reinterpret_cast<const uint16_t *>(pixels + y * stride)[x];
            uint8_t *const d = &conv[(y * w + x) * 3];
            d[0] = static_cast<uint8_t>(((v >> 11) & 31) * 255 / 31), d[1] = static_cast<uint8_t>(((v >> 5) & 63) * 255 / 63), d[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
         }
      pixels = conv.data();
      stride = w * 3;
      break;
   default: return "The Wood Mask image format is not supported (use PNG or JPG)";
   }
   MaskTrace::Image im;
   im.data = pixels;
   im.w = static_cast<int>(bmp->width());
   im.h = static_cast<int>(bmp->height());
   im.channels = channels;
   im.stride = stride;
   MaskTrace::Params prm;
   prm.left = t->m_left;
   prm.top = t->m_top;
   prm.right = t->m_right;
   prm.bottom = t->m_bottom;
   prm.mode = static_cast<MaskTrace::Mode>(clamp(board->m_d.m_pfMaskMode, 0, 3));
   prm.minSize = board->m_d.m_pfMinHole;
   prm.vpuPerInch = kVPUPerInch;
   MaskTrace::Result res;
   if (!MaskTrace::Trace(im, prm, res))
      return "The Wood Mask image could not be traced";

   // Footprints of the traced insert cards: the convex hull of their mesh vertices through the full matrix
   vector<vector<MaskTrace::Pt>> hulls;
   for (Primitive *const c : TracedCards(t))
   {
      if (c->m_mesh.NumVertices() == 0)
         continue;
      const Matrix3D &m = c->RecalculateMatrices();
      vector<MaskTrace::Pt> pts;
      pts.reserve(c->m_mesh.NumVertices());
      for (const Vertex3D_NoTex2 &v : c->m_mesh.m_vertices)
      {
         const Vertex3Ds w = m * Vertex3Ds(v.x, v.y, v.z);
         pts.push_back({ w.x, w.y });
      }
      hulls.push_back(MaskTrace::ConvexHull(std::move(pts)));
   }

   vector<PlayfieldHole> &holes = board->m_pfHoles;
   std::erase_if(holes, [](const PlayfieldHole &h) { return (h.flags & PlayfieldHole::FromMask) != 0; });
   int round = 0, slots = 0, under = 0, skipped = 0;
   for (const MaskTrace::Hole &th : res.holes)
   {
      if (holes.size() >= static_cast<size_t>(kMaxPlayfieldHoles) || (th.type == MaskTrace::HoleType::Polygon && th.polygon.size() > static_cast<size_t>(kMaxPlayfieldHolePoints)))
      {
         skipped++;
         continue;
      }
      PlayfieldHole h;
      h.centre = Vertex2D(th.x, th.y);
      h.diameter = th.diameter;
      h.flags = PlayfieldHole::Cut | PlayfieldHole::FromMask;
      if (th.type == MaskTrace::HoleType::Round)
         h.type = PlayfieldHole::Round, round++;
      else if (th.type == MaskTrace::HoleType::Slot)
      {
         h.type = PlayfieldHole::Slot;
         h.end = Vertex2D(th.x2, th.y2);
         slots++;
      }
      else
      {
         h.type = PlayfieldHole::Polygon;
         for (const MaskTrace::Pt &p : th.polygon)
            h.polygon.push_back(Vertex2D(p.x, p.y));
         h.diameter = max(th.maxX - th.minX, th.maxY - th.minY);
      }
      for (const vector<MaskTrace::Pt> &hull : hulls)
         if (MaskTrace::UnderInsert(th, hull))
         {
            h.flags = (h.flags & ~PlayfieldHole::Cut) | PlayfieldHole::UnderInsert;
            under++;
            break;
         }
      holes.push_back(std::move(h));
   }
   board->m_pfOverlap.clear();
   board->m_pfBevelClamped.clear();
   char line[256];
   const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   const int found = static_cast<int>(res.holes.size()) - skipped;
   const char *const names[] = { "Auto", "Dark", "Light", "Transparent" };
   const char *const used = names[static_cast<int>(res.usedMode)];
   const int asked = clamp(board->m_d.m_pfMaskMode, 0, 3);
   char how[96];
   if (res.noAlphaFallback)
      snprintf(how, sizeof(how), "Transparent -> Dark, the image has no alpha");
   else if (asked == 0)
      snprintf(how, sizeof(how), "Auto -> %s", used);
   else
      snprintf(how, sizeof(how), "%s", used);
   snprintf(line, sizeof(line), "%d holes found (%d round, %d slots), %d under inserts left uncut, %d outline cuts, %d under Min Size, mask read as %s, %.2f s%s", found, round, slots, under,
      static_cast<int>(res.outlineCuts.size()), res.droppedSmall, how, secs, skipped > 0 ? ", some skipped (hole limit)" : "");
   ok = true;
   return line;
}

vector<PlasticMesh::CurvePoint> HoleLoop(const PlayfieldHole &h, const float tol)
{
   PlasticMesh::Hole m { h.centre.x, h.centre.y, h.diameter, h.end.x, h.end.y, h.type == PlayfieldHole::Slot };
   if (h.type == PlayfieldHole::Polygon)
      for (const Vertex2D &v : h.polygon)
         m.polygon.push_back({ v.x, v.y, false });
   return PlasticMesh::HoleOutline(m, tol);
}

bool InsideLoop(const vector<PlasticMesh::CurvePoint> &loop, const Vertex2D &p)
{
   bool in = false;
   for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
      if (((loop[i].y > p.y) != (loop[j].y > p.y)) && (p.x < (loop[j].x - loop[i].x) * (p.y - loop[i].y) / (loop[j].y - loop[i].y) + loop[i].x))
         in = !in;
   return in;
}

PlayfieldHole NewHole(const Vertex2D &at)
{
   PlayfieldHole h;
   h.centre = at;
   if (s.placing == 2)
   {
      // Long axis along y, centred on the click
      h.type = PlayfieldHole::Slot;
      h.diameter = s.slotWidth;
      const float half = 0.5f * max(0.f, s.slotLength - s.slotWidth);
      h.centre = Vertex2D(at.x, at.y - half);
      h.end = Vertex2D(at.x, at.y + half);
   }
   else
      h.diameter = s.roundDia;
   return h;
}

void MoveHole(PlayfieldHole &h, const PlayfieldHole &from, const float dx, const float dy)
{
   h = from;
   h.flags &= ~PlayfieldHole::FromMask; // a moved mask hole is the author's now: Find Holes keeps it
   h.centre = Vertex2D(from.centre.x + dx, from.centre.y + dy);
   h.end = Vertex2D(from.end.x + dx, from.end.y + dy);
   for (Vertex2D &v : h.polygon)
      v = Vertex2D(v.x + dx, v.y + dy);
}


// The table's Add snapshot (see Add): names, the old playfield's physics fields and the ground plane, restored together
struct AddContext
{
   EditorUI *ed = nullptr;
   PinTable *table = nullptr;
   Primitive *old = nullptr; // The saved playfield, or the session quad (implicit playfield)
   bool quad = false;
   bool oldVisible = true; // Its authored visibility before the Add
   Primitive *board = nullptr, *walls = nullptr, *floor = nullptr; // Set once created
};

struct AddSnapshot
{
   bool boardIn = false; // The board was in the table (the state after the Add)
   string boardName, oldName;
   bool overwrite = false;
   float friction = 0.f, elasticity = 0.f, falloff = 0.f, scatter = 0.f;
   bool plane = false;
};





} // namespace

struct PlayfieldMaker::Impl
{
   // Bevel All Holes: every cut through-hole to Default (insert pockets are never bevelled). Remove All Bevels: every hole to Off. True if any changed
   // (the caller makes it one undo step and rebuilds). The holes keep their From Mask bit: a bulk setting is not a hand edit
   static bool SetAllBevels(Primitive *const board, const bool on)
   {
      bool changed = false;
      for (PlayfieldHole &h : board->m_pfHoles)
      {
         if (on && (!(h.flags & PlayfieldHole::Cut) || (h.flags & PlayfieldHole::UnderInsert)))
            continue;
         const float b = on ? -1.f : 0.f;
         changed |= h.bevel != b;
         h.bevel = b;
      }
      return changed;
   }

   static Primitive *SelectedBoard(const EditorUI &ed)
   {
      if (ed.m_selection.GetType() != Selection::S_EDITABLE || ed.m_selection.GetPart() == nullptr)
         return nullptr;
      IEditable *const part = ed.m_selection.GetPart()->GetEditable();
      return part != nullptr && part->GetItemType() == eItemPrimitive && static_cast<Primitive *>(part)->m_d.m_playfieldGen ? static_cast<Primitive *>(part) : nullptr;
   }

   // Authored visibility: m_d holds the editor display state, the UI part keeps the authored value (written back before a save)
   static void SetAuthoredVisible(EditorUI &ed, Primitive *const part, const bool visible)
   {
      if (part == nullptr)
         return;
      part->m_d.m_visible = visible;
      if (const auto it = ed.m_editableMap.find(part); it != ed.m_editableMap.end())
         it->second->SyncVisibilityOverrides();
   }

   static bool AuthoredVisible(EditorUI &ed, Primitive *const part)
   {
      ed.m_undo.SyncPartBeforeSave(part); // Writes the authored visibility into m_d
      return part->m_d.m_visible;
   }

   static const vector<Primitive *> &Floors(EditorUI &ed)
   {
      if (s.floorsStale)
      {
         s.floors = Primitive::FindPlayfieldFloors(ed.m_table);
         s.floorsStale = false;
      }
      return s.floors;
   }

   static string EnsurePlywoodImage(EditorUI &ed)
   {
      if (ed.m_table->GetImage(kPlywood) != nullptr)
         return kPlywood;
      Texture *const tex = ed.m_table->ImportImage(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets, "PlayfieldPlywood.png"), kPlywood);
      if (tex == nullptr)
         return string();
      ed.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      return tex->m_name;
   }

   // A file for a slot: imported under its file name, numbered when the table has that name already (never replaced: the old one may be in use)
   static Texture *ImportSlotImage(EditorUI &ed, const string &file)
   {
      const std::filesystem::path path = PathFromUTF8(file);
      const string base = PathToUTF8(path.stem());
      string name = base;
      for (int n = 1; ed.m_table->GetImage(name) != nullptr; n++)
         name = base + std::to_string(n);
      Texture *const tex = ed.m_table->ImportImage(path, name);
      if (tex == nullptr)
      {
         ed.Report("Playfield: could not load " + file);
         return nullptr;
      }
      g_settingsService.GetAppSettings().SetRecentDir_ImageDir(PathToString(path.parent_path()), false);
      ed.m_table->SetNonUndoableDirty(SaveDirtyState::Dirty);
      return tex;
   }

   static string GetSlot(EditorUI &ed, const Primitive *board, const int slot)
   {
      switch (slot)
      {
      case Paint: return ed.m_table->m_image;
      case Shine: return board->m_d.m_szORMMap;
      case Bump: return board->m_d.m_szNormalMap;
      case Glow: return board->m_d.m_szEmissionMap;
      case WoodMask: return board->m_d.m_pfWoodMask;
      case Plywood:
      {
         const Primitive *const walls = PlayfieldMaker::CompanionOf(board, kPlayfieldWallsRole);
         return walls != nullptr ? walls->m_d.m_szImage : string();
      }
      case InsertPrint: return CurrentPrint(TracedCards(ed.m_table));
      case Blueprint: return PathToUTF8(PathFromUTF8(ed.m_refImage.GetFile(ed)).filename());
      default: return string();
      }
   }

   // Writes a slot inside the panel's open undo record (the board, its companions and the table's playfield fields are in it already)
   static void SetSlot(EditorUI &ed, Primitive *const board, const int slot, const string &image)
   {
      switch (slot)
      {
      case Paint: ed.m_table->m_image = image; break;
      case Shine: board->m_d.m_szORMMap = image; break;
      case Bump: board->m_d.m_szNormalMap = image; break;
      case Glow: board->m_d.m_szEmissionMap = image; break;
      case WoodMask: board->m_d.m_pfWoodMask = image; break;
      case Plywood:
         if (Primitive *const walls = PlayfieldMaker::CompanionOf(board, kPlayfieldWallsRole); walls != nullptr)
            walls->m_d.m_szImage = image;
         break;
      case InsertPrint:
      {
         // Cards on the current print (or none) take the new one; cards on another image are left alone unless "All cards" is ticked
         const vector<Primitive *> cards = TracedCards(ed.m_table);
         const string current = CurrentPrint(cards);
         for (Primitive *const c : cards)
            if (s.printAll || c->m_d.m_szORMMap.empty() || c->m_d.m_szORMMap == current)
            {
               ed.m_undo.MarkForUndo(c, true, false);
               c->m_d.m_szORMMap = image;
            }
         break;
      }
      default: break;
      }
      if (ed.m_renderer)
         ed.m_renderer->MarkStaticPrepassDirty();
   }

   static bool ApplyFile(EditorUI &ed, Primitive *const board, const int slot, const string &file)
   {
      if (slot == Blueprint)
      {
         if (ed.m_refImage.OpenFile(ed, file))
            s.showBlueprint = true;
         return false; // Not table data
      }
      if (slot == InsertPrint && TracedCards(ed.m_table).empty())
      {
         ed.Report("Insert Print: this table has no traced inserts, nothing was imported");
         return false;
      }
      const Texture *const tex = ImportSlotImage(ed, file);
      if (tex == nullptr)
         return false;
      SetSlot(ed, board, slot, tex->m_name);
      if (slot == Glow)
         s.showGlow = true;
      ed.Report(string(kSlotNames[slot]) + ": " + tex->m_name);
      return true;
   }

   static void StartLoad(EditorUI &ed, const int slot)
   {
      if (ed.m_player == nullptr || ed.m_player->m_playfieldWnd == nullptr || s.pendingLoad != nullptr)
         return;
      s.pendingLoad = std::make_shared<vector<string>>();
      s.pendingSlot = slot;
      const SDL_DialogFileFilter filters[] = { { "Image Files", "png;jpg;jpeg;webp;exr;hdr" } };
      const string dir = PathToUTF8(PathFromString(g_settingsService.GetAppSettings().GetRecentDir_ImageDir()));
      SDL_ShowOpenFileDialog(
         [](void *userdata, const char *const *filelist, int)
         {
            auto *res = static_cast<std::shared_ptr<vector<string>> *>(userdata);
            (**res).push_back(filelist != nullptr && filelist[0] != nullptr ? string(filelist[0]) : string()); // Cancelled: answered, nothing chosen
            delete res;
         },
         new std::shared_ptr<vector<string>>(s.pendingLoad), ed.m_player->m_playfieldWnd->GetCore(), filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
   }

   static void EndGrab(EditorUI &ed, const bool confirm)
   {
      Primitive *const board = const_cast<Primitive *>(s.board);
      const int i = SelectedHole(board);
      s.grabbing = false;
      if (i < 0)
         return;
      if (!confirm)
      {
         board->m_pfHoles[i] = s.grabOrigin; // Only its outline moved: nothing to rebuild
         return;
      }
      const PlayfieldHole moved = board->m_pfHoles[i];
      board->m_pfHoles[i] = s.grabOrigin;
      ed.m_undo.BeginUndo();
      ed.m_undo.MarkForUndo(board);
      board->m_pfHoles[i] = moved;
      ed.m_undo.SetLabel("Move Hole");
      ed.m_undo.EndUndo();
      board->UpdatePlayfield(); // Once, at the release
   }

   static AddSnapshot TakeSnapshot(const AddContext &c)
   {
      AddSnapshot r;
      r.boardIn = c.board != nullptr && c.table->HasPart(c.board);
      if (c.board != nullptr)
         r.boardName = c.board->GetName();
      if (c.old != nullptr)
      {
         r.oldName = c.old->GetName();
         r.overwrite = c.old->m_d.m_overwritePhysics;
         r.friction = c.old->m_d.m_friction;
         r.elasticity = c.old->m_d.m_elasticity;
         r.falloff = c.old->m_d.m_elasticityFalloff;
         r.scatter = c.old->m_d.m_scatter;
      }
      r.plane = c.ed->m_player != nullptr && c.ed->m_player->m_physics != nullptr && c.ed->m_player->m_physics->HasImplicitPlayfieldMesh();
      return r;
   }

   static void Restore(const AddContext &c, const AddSnapshot &r)
   {
      PinTable *const t = c.table;
      EditorUI &ed = *c.ed;
      struct Target
      {
         Primitive *part;
         string name;
      };
      vector<Target> targets;
      if (c.old != nullptr && !r.oldName.empty())
         targets.push_back({ c.old, r.oldName });
      if (c.board != nullptr && !r.boardName.empty())
         targets.push_back({ c.board, r.boardName });
      // Pass 1: a part of the table holding a name about to be assigned (the board on undo, which the pre-Add snapshot does not list) steps aside first
      for (const Target &target : targets)
         for (IEditable *const part : t->GetParts())
            if (part != target.part && part->GetIScriptable() != nullptr && StrCompareNoCase(part->GetName(), target.name))
            {
               t->RenamePart(part, t->GetUniqueName("playfield_mesh_swap"s));
               break;
            }
      // Pass 2: the final names; a part out of the table (the board during redo, before it is put back) is not in the name registry
      for (const Target &target : targets)
         if (t->HasPart(target.part))
         {
            if (target.part->GetName() != target.name)
               t->RenamePart(target.part, target.name);
         }
         else
            target.part->GetIScriptable()->m_name = target.name;
      // Visibility: the Add state, or the state before it, whatever Show Old Playfield was set to
      if (r.boardIn)
      {
         SetAuthoredVisible(ed, c.board, true);
         SetAuthoredVisible(ed, c.walls, true);
         SetAuthoredVisible(ed, c.old, false);
      }
      else
         SetAuthoredVisible(ed, c.old, c.oldVisible);
      if (c.old != nullptr && !c.quad)
      {
         c.old->m_d.m_overwritePhysics = r.overwrite;
         c.old->m_d.m_friction = r.friction;
         c.old->m_d.m_elasticity = r.elasticity;
         c.old->m_d.m_elasticityFalloff = r.falloff;
         c.old->m_d.m_scatter = r.scatter;
      }
      if (ed.m_player != nullptr && ed.m_player->m_physics != nullptr)
      {
         for (Primitive *const p : { c.old, c.board, c.walls, c.floor })
            if (p != nullptr && t->HasPart(p))
               ed.m_player->m_physics->Update(p); // Clears the cached playfield status (PhysicSetup)
         ed.m_player->m_physics->SetImplicitPlayfieldMesh(r.plane);
      }
      if (ed.m_renderer)
      {
         if (c.old != nullptr)
            ed.m_renderer->ReinitRenderable(c.old);
         ed.m_renderer->MarkStaticPrepassDirty();
      }
      s.floorsStale = true;
   }

   static PartGroup *BoardGroup(EditorUI &ed, const Primitive *old)
   {
      // Never a group whose reference space is not the playfield: that skips PhysicSetup, so the board would stop being the playfield
      const auto ok = [](const PartGroup *g) { return g != nullptr && g->GetReferenceSpace() == PartGroupData::SpaceReference::SR_PLAYFIELD; };
      if (old != nullptr && ok(old->GetPartGroup()))
         return old->GetPartGroup();
      for (IEditable *const part : ed.m_table->GetParts())
         if (part->GetItemType() == eItemPartGroup && part->GetPartGroup() == nullptr && ok(static_cast<PartGroup *>(part)))
            return static_cast<PartGroup *>(part);
      PartGroup *const g = ed.GetPartGroupForNewPart();
      return ok(g) ? g : nullptr;
   }

   static Primitive *NewPart(EditorUI &ed, const string &name, PartGroup *group)
   {
      IEditable *const pie = EditableRegistry::CreateAndInit(eItemPrimitive, ed.m_table, 0.f, 0.f);
      if (pie == nullptr)
         return nullptr;
      pie->GetIScriptable()->m_name = name;
      pie->m_desktopBackdrop = false;
      ed.m_table->AddPart(pie);
      pie->SetPartGroup(group);
      return static_cast<Primitive *>(pie);
   }
};


Primitive *PlayfieldMaker::Board(const PinTable *table)
{
   if (table != nullptr)
      for (IEditable *const part : table->GetParts())
         if (part->GetItemType() == eItemPrimitive && static_cast<Primitive *>(part)->m_d.m_playfieldGen)
            return static_cast<Primitive *>(part);
   return nullptr;
}

Primitive *PlayfieldMaker::CompanionOf(const Primitive *board, const int role)
{
   return board != nullptr ? board->GetPlayfieldCompanion(role) : nullptr;
}

bool PlayfieldMaker::IsPart(const IEditable *part) { return part != nullptr && part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->IsPlayfieldPart(); }

bool PlayfieldMaker::StripBoard(EditorUI &ed, vector<IEditable *> &parts)
{
   const size_t n = parts.size();
   std::erase_if(parts, [](const IEditable *p) { return IsPart(p); });
   if (parts.size() == n)
      return false;
   ed.Report(kFollowsTable);
   return true;
}

bool PlayfieldMaker::StripBoardGroups(EditorUI &ed, vector<IEditable *> &parts)
{
   vector<const IEditable *> boardParts;
   for (const IEditable *const part : ed.m_table->GetParts())
      if (IsPart(part))
         boardParts.push_back(part);
   if (boardParts.empty())
      return false;
   const size_t n = parts.size();
   std::erase_if(parts,
      [&boardParts](const IEditable *p)
      {
         return p->GetItemType() == eItemPartGroup && std::ranges::any_of(boardParts, [p](const IEditable *b) { return b->IsChild(static_cast<const PartGroup *>(p)); });
      });
   if (parts.size() == n)
      return false;
   ed.Report(kFollowsTable);
   return true;
}

bool PlayfieldMaker::RefuseImport(EditorUI &ed, const PinTable *source)
{
   if (source == nullptr || std::ranges::none_of(source->GetParts(), [](const IEditable *p) { return IsPart(p); }))
      return false;
   ed.Report(kOneBoard);
   return true;
}

std::function<void()> PlayfieldMaker::KeepShown(EditorUI &ed)
{
   if (s.grabbing)
      Impl::EndGrab(ed, false); // The step reloads the holes: the grabbed one goes back first
   Primitive *const board = Board(ed.m_table);
   Primitive *const old = board != nullptr ? OldPlayfield(ed.m_table) : nullptr;
   if (old == nullptr) // Nothing to compare the board with
      return {};
   Primitive *const walls = CompanionOf(board, kPlayfieldWallsRole);
   const bool boardShown = Impl::AuthoredVisible(ed, board);
   const bool wallsShown = walls != nullptr && Impl::AuthoredVisible(ed, walls);
   const bool oldShown = Impl::AuthoredVisible(ed, old);
   if (oldShown && (boardShown || wallsShown)) // Both drawn already (the author's own Visible edit): the step is left alone, so it can undo that
      return {};
   return [&ed, board, walls, old, boardShown, wallsShown, oldShown]()
   {
      PinTable *const t = ed.m_table;
      if (Board(t) != board || !t->HasPart(old)) // The step took the board or the old playfield out (the undo of Add): its own restore applies
         return;
      // Only a step that would leave both playfields drawn (a board record taken on the other side of Show Old Playfield) gets the visibility back.
      // HasPart compares addresses: a part the step removed is never read
      const bool wallsIn = walls != nullptr && t->HasPart(walls);
      if (!Impl::AuthoredVisible(ed, old) || !(Impl::AuthoredVisible(ed, board) || (wallsIn && Impl::AuthoredVisible(ed, walls))))
         return;
      Impl::SetAuthoredVisible(ed, board, boardShown);
      if (wallsIn)
         Impl::SetAuthoredVisible(ed, walls, wallsShown);
      Impl::SetAuthoredVisible(ed, old, oldShown);
      if (ed.m_renderer)
         ed.m_renderer->MarkStaticPrepassDirty();
   };
}

bool PlayfieldMaker::CanAdd(const EditorUI &ed)
{
   return !ed.m_table->IsLocked() && !ed.IsInspectMode() && ed.m_player != nullptr && ed.m_player->m_physics != nullptr && ed.m_renderer != nullptr && ed.m_pointEditPart == nullptr;
}

void PlayfieldMaker::Add(EditorUI &ed)
{
   if (!CanAdd(ed))
      return;
   PinTable *const t = ed.m_table;
   if (Board(t) != nullptr)
   {
      ed.Report("Add Playfield: this table has a Playfield Maker board already");
      return;
   }
   auto ctx = std::make_shared<AddContext>();
   ctx->ed = &ed;
   ctx->table = t;
   for (IEditable *const part : t->GetParts()) // The saved playfield (session objects are not looked at)
      if (part->GetItemType() == eItemPrimitive && static_cast<Primitive *>(part)->IsPlayfield() && !t->IsPlayerSessionObject(part))
      {
         ctx->old = static_cast<Primitive *>(part);
         break;
      }
   if (ctx->old == nullptr && ed.m_player->m_implicitPlayfieldMesh != nullptr && t->HasPart(ed.m_player->m_implicitPlayfieldMesh))
   {
      ctx->old = ed.m_player->m_implicitPlayfieldMesh; // Kept as the session object it is: still never saved
      ctx->quad = true;
   }
   Primitive *const old = ctx->old;
   const bool quad = ctx->quad;
   if (old != nullptr)
      ctx->oldVisible = quad ? old->m_d.m_visible : Impl::AuthoredVisible(ed, old);

   ed.m_undo.BeginUndo();
   const AddSnapshot before = Impl::TakeSnapshot(*ctx);

   // The old playfield: renamed and hidden, never removed
   bool keepExisting = false;
   if (old != nullptr)
   {
      const string base = quad ? "playfield_mesh_implicit"s : "playfield_mesh_old"s;
      string name = base;
      for (int n = 2; !t->IsNameUnique(name); n++)
         name = base + std::to_string(n);
      t->RenamePart(old, name);
      if (!quad && old->m_d.m_collidable && !old->m_d.m_toy) // A toy has no colliders
      {
         // It plays with the table's values while it is the playfield, with its own once renamed: it keeps the table's
         old->m_d.m_overwritePhysics = true;
         Primitive::GetTablePlayfieldPhysics(t, old->m_d.m_friction, old->m_d.m_elasticity, old->m_d.m_elasticityFalloff, old->m_d.m_scatter);
         keepExisting = true;
      }
      Impl::SetAuthoredVisible(ed, old, false);
   }
   for (const Primitive *const f : Primitive::FindPlayfieldFloors(t))
      if (f->m_mesh.NumIndices() > 6)
         keepExisting = true; // A shaped floor collider (pockets, ramps into the floor): the board is look only

   // The board and its companions
   const auto unique = [t](const string &name)
   {
      string res = name;
      for (int n = 2; !t->IsNameUnique(res); n++)
         res = name + std::to_string(n);
      return res;
   };
   PartGroup *group = Impl::BoardGroup(ed, old);
   PartGroup *madeGroup = nullptr; // A table without a part group: parts with no group are live runtime objects to the editor (not listed, not selectable)
   if (group == nullptr)
   {
      if (IEditable *const pie = EditableRegistry::CreateAndInit(eItemPartGroup, t, 0.f, 0.f); pie != nullptr)
      {
         pie->GetIScriptable()->m_name = unique("Playfield"s);
         t->AddPart(pie);
         pie->SetPartGroup(nullptr); // A root group, playfield reference space (the default)
         madeGroup = group = static_cast<PartGroup *>(pie);
      }
   }
   const string boardName = "playfield_mesh"s;
   Primitive *const board = group != nullptr ? Impl::NewPart(ed, boardName, group) : nullptr; // No group to put it in (the root group could not be created): the failure path below
   Primitive *const walls = board != nullptr ? Impl::NewPart(ed, unique(boardName + "_Walls"), group) : nullptr;
   Primitive *const floor = walls != nullptr ? Impl::NewPart(ed, unique(boardName + "_Floor"), group) : nullptr;
   if (floor == nullptr)
   {
      for (Primitive *const made : { walls, board }) // What was made goes again (nothing set up yet: no renderable, no collider)
         if (made != nullptr)
         {
            t->RemovePart(made);
            made->Release(); // The creation reference
         }
      if (madeGroup != nullptr)
      {
         t->RemovePart(madeGroup);
         madeGroup->Release();
      }
      Impl::Restore(*ctx, before); // The names, the physics fields, the visibility and the plane as before
      ed.m_undo.EndUndo();
      ed.m_undo.Discard(); // Nothing was recorded
      ed.Report("Add Playfield: could not create the parts");
      return;
   }
   ctx->board = board;
   ctx->walls = walls;
   ctx->floor = floor;
   // As the implicit playfield (player.cpp) sets its quad
   board->m_d.m_playfieldGen = true;
   board->m_d.m_toy = true;
   board->m_d.m_collidable = false;
   board->m_d.m_visible = true;
   board->m_d.m_use3DMesh = true;
   board->m_d.m_depthBias = 100000.f;
   board->m_d.m_reflectionEnabled = true;
   board->m_d.m_disableLightingBelow = 1.f;
   board->m_d.m_displayTexture = t->m_winEditorBackdrop;
   board->m_desktopBackdrop = old != nullptr ? old->m_desktopBackdrop : false;
   board->m_d.m_staticRendering = true;
   if (old != nullptr && !quad)
   {
      board->m_d.m_szORMMap = old->m_d.m_szORMMap;
      board->m_d.m_szNormalMap = old->m_d.m_szNormalMap;
      board->m_d.m_szEmissionMap = old->m_d.m_szEmissionMap;
      board->m_d.m_objectSpaceNormalMap = old->m_d.m_objectSpaceNormalMap;
      board->m_d.m_staticRendering = old->m_d.m_staticRendering;
   }
   board->m_d.m_pfFloor = keepExisting ? 1 : 0;

   walls->m_d.m_plasticHost = boardName;
   walls->m_d.m_plasticRole = kPlayfieldWallsRole;
   walls->m_d.m_toy = true;
   walls->m_d.m_collidable = false;
   walls->m_d.m_visible = true;
   walls->m_d.m_reflectionEnabled = false; // Else every wall is mirrored into the playfield reflection as a ghost ring
   walls->m_d.m_disableLightingBelow = 1.f;
   walls->m_d.m_staticRendering = board->m_d.m_staticRendering;
   walls->m_d.m_szMaterial = EnsurePlywoodMaterial(t)->m_name;
   walls->m_d.m_szImage = Impl::EnsurePlywoodImage(ed);
   walls->SetUILock(true); // A viewport click lands on the board

   floor->m_d.m_plasticHost = boardName;
   floor->m_d.m_plasticRole = kPlayfieldFloorRole;
   floor->m_d.m_toy = false;
   floor->m_d.m_visible = false;
   floor->m_d.m_collision_reductionFactor = 0.f;
   floor->m_d.m_hitEvent = false;
   floor->SetUILock(true);
   CopyFloorPhysics(t, board, floor);

   board->RegeneratePlayfield(); // The companions are in the table now
   for (Primitive *const prim : { board, walls, floor })
   {
      ed.m_player->TimerSetup(prim);
      prim->GetIRenderable()->RenderSetup(ed.m_renderer.get());
      if (prim->GetIHitable())
         ed.m_player->m_physics->Add(prim);
   }
   if (old != nullptr)
   {
      ed.m_player->m_physics->Update(old); // Its cached playfield status clears: it stops being drawn as the playfield
      ed.m_renderer->ReinitRenderable(old);
   }
   if (quad)
      ed.m_player->m_physics->SetImplicitPlayfieldMesh(false); // The ground plane: the board's floor (or the existing one) takes over

   if (madeGroup != nullptr)
      ed.m_undo.MarkForCreate(madeGroup); // The same step: the undo takes the group out with the parts
   for (Primitive *const prim : { board, walls, floor })
      ed.m_undo.MarkForCreate(prim);
   ed.m_undo.MarkForAction([ctx, before]() { Impl::Restore(*ctx, before); },
      [ctx]() -> std::function<void()>
      {
         const AddSnapshot now = Impl::TakeSnapshot(*ctx);
         return [ctx, now]() { Impl::Restore(*ctx, now); };
      });
   ed.m_undo.SetLabel("Add Playfield");
   ed.m_renderer->MarkStaticPrepassDirty();
   ed.m_undo.EndUndo();
   s = State();

   ed.UpdateEditableList();
   SelectBoard(ed);
   ed.Report("Added the playfield board (" + string(old == nullptr ? "no playfield before" : quad ? "the built-in playfield is hidden" : old->GetName() + " is the old one, hidden")
      + "), Floor: " + (keepExisting ? "Keep Existing" : "Own"));
}

void PlayfieldMaker::SelectBoard(EditorUI &ed)
{
   Primitive *const board = Board(ed.m_table);
   if (board == nullptr)
   {
      ed.Report("This table has no Playfield Maker board (Add > Playfield)");
      return;
   }
   ed.m_selectionFilter = ed.m_selectionFilter | EditorUI::SelectionFilter::Playfield;
   if (const auto it = ed.m_editableMap.find(board); it != ed.m_editableMap.end())
   {
      ed.SetSelection(Selection(it->second));
      ed.m_outlinerAnchor = it->second;
   }
}

void PlayfieldMaker::OnFileDrop(EditorUI &ed, const string &path, const ImVec2 &pos)
{
   if (Impl::SelectedBoard(ed) == nullptr)
   {
      ed.Report("Drop an image on a playfield slot (select the playfield first)");
      return;
   }
   s.dropPath = path;
   s.dropPos = pos;
   s.dropFrame = ImGui::GetFrameCount();
}

void PlayfieldMaker::Update(EditorUI &ed)
{
   // The floor collider list follows the table (an edit, an undo, a part added or removed)
   if (const uint64_t key = ed.m_undo.TopRecordId(); key != s.floorsKey || ed.m_table->GetParts().size() != s.floorsParts)
   {
      s.floorsKey = key;
      s.floorsParts = ed.m_table->GetParts().size();
      s.floorsStale = true;
   }
   if (s.dropFrame >= 0 && ImGui::GetFrameCount() > s.dropFrame + 2) // Nobody took it
   {
      s.dropFrame = -1;
      ed.Report("Drop an image on a playfield slot");
   }
   Primitive *const board = Impl::SelectedBoard(ed);
   if (s.placing != 0 && (board == nullptr || ed.m_table->IsLocked()))
      s.placing = 0;
   if (s.grabbing)
   {
      if (board == nullptr || board != s.board || SelectedHole(board) < 0 || ed.m_table->IsLocked())
      {
         s.grabbing = false; // The board went (an undo, a selection change): the hole is the record's
         return;
      }
      const Vertex2D at = ed.UnprojectToPlane(ImGui::GetMousePos(), 0.f);
      MoveHole(board->m_pfHoles[s.selected], s.grabOrigin, at.x - s.grabStart.x, at.y - s.grabStart.y);
      if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
         Impl::EndGrab(ed, true);
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
         Impl::EndGrab(ed, false);
   }
}

bool PlayfieldMaker::IsBusy() { return s.placing != 0 || s.grabbing; }

bool PlayfieldMaker::Cancel(EditorUI &ed)
{
   if (s.grabbing)
   {
      Impl::EndGrab(ed, false);
      return true;
   }
   if (s.placing != 0)
   {
      s.placing = 0;
      ed.Report(string());
      return true;
   }
   return false;
}

bool PlayfieldMaker::BeginGrab(EditorUI &ed)
{
   Primitive *const board = Impl::SelectedBoard(ed);
   const int i = SelectedHole(board);
   if (i < 0 || s.grabbing || ed.m_table->IsLocked())
      return false;
   s.grabbing = true;
   s.grabOrigin = board->m_pfHoles[i];
   s.grabStart = ed.UnprojectToPlane(ImGui::GetMousePos(), 0.f);
   ed.Report("Move hole: click or Enter places it, Esc or right click cancels");
   return true;
}

bool PlayfieldMaker::Click(EditorUI &ed, const ImVec2 &mouse)
{
   Primitive *const board = Impl::SelectedBoard(ed);
   if (s.grabbing)
   {
      Impl::EndGrab(ed, true);
      return true;
   }
   if (board == nullptr)
      return false;
   const Vertex2D at = ed.UnprojectToPlane(mouse, 0.f);
   if (s.placing != 0)
   {
      if (board->m_pfHoles.size() >= static_cast<size_t>(kMaxPlayfieldHoles))
      {
         ed.Report("The board has the most holes it can hold");
         s.placing = 0;
         return true;
      }
      ed.m_undo.BeginUndo();
      ed.m_undo.MarkForUndo(board);
      board->m_pfHoles.push_back(NewHole(at));
      ed.m_undo.SetLabel(s.placing == 2 ? "Add Slot" : "Add Round Hole");
      ed.m_undo.EndUndo();
      SelectHole(board, static_cast<int>(board->m_pfHoles.size()) - 1);
      board->UpdatePlayfield();
      s.placing = 0;
      ed.Report(string());
      return true;
   }
   // A click inside a hole's outline selects the hole (the board stays the selection)
   const float tol = PlasticMesh::FlattenTolerance(board->m_d.m_pfDetail);
   for (int i = static_cast<int>(board->m_pfHoles.size()) - 1; i >= 0; i--)
   {
      const PlayfieldHole &h = board->m_pfHoles[i];
      const float r = h.type == PlayfieldHole::Polygon ? FLT_MAX : 0.5f * h.diameter + hypotf(h.end.x - h.centre.x, h.end.y - h.centre.y);
      if (r != FLT_MAX && hypotf(at.x - h.centre.x, at.y - h.centre.y) > r + 1.f && (h.type != PlayfieldHole::Slot || hypotf(at.x - h.end.x, at.y - h.end.y) > r + 1.f))
         continue;
      const vector<PlasticMesh::CurvePoint> loop = HoleLoop(h, tol);
      if (loop.size() >= 3 && InsideLoop(loop, at))
      {
         // A part in the hole (an insert, a lamp, a kicker) is selected first: the editor's own click takes it. Parts hidden in play (the old playfield,
         // an invisible floor collider) and session objects (the hidden built-in playfield) lie under every hole: they do not count
         vector<HitTestResult> hits;
         ed.RayCastParts(mouse, hits);
         for (const HitTestResult &hit : hits)
            if (IEditable *const part = ed.HitEditable(hit); part != nullptr && !IsPart(part) && !ed.m_table->IsPlayerSessionObject(part))
               if (const auto it = ed.m_editableMap.find(part); it != ed.m_editableMap.end() && !it->second->IsHiddenInPlay())
                  return false;
         SelectHole(board, i);
         return true;
      }
   }
   return false;
}

void PlayfieldMaker::Draw(EditorUI &ed, ImDrawList *const draw, const LiveRenderContext &ctx)
{
   Primitive *const board = Impl::SelectedBoard(ed);
   if (board == nullptr || !ed.m_viewportVisible || ed.m_viewportRect.Width() <= 0.f)
      return;
   draw->PushClipRect(ImVec2(ed.m_viewportRect.x0, ed.m_viewportRect.y0), ImVec2(ed.m_viewportRect.x1, ed.m_viewportRect.y1), true); // Never over the panels
   const float dpi = ed.m_liveUI.GetDPI();
   const float tol = max(PlasticMesh::FlattenTolerance(board->m_d.m_pfDetail), 0.5f);
   const int sel = SelectedHole(board);
   const auto outline = [&](const PlayfieldHole &h, const ImU32 color, const bool dashed, const float width)
   {
      const vector<PlasticMesh::CurvePoint> loop = HoleLoop(h, tol);
      for (size_t i = 0; i < loop.size(); i++)
      {
         if (dashed && (i & 1))
            continue;
         const PlasticMesh::CurvePoint &a = loop[i], &b = loop[(i + 1) % loop.size()];
         const ImVec2 pa = ctx.Project(Vertex3Ds(a.x, a.y, 0.1f)), pb = ctx.Project(Vertex3Ds(b.x, b.y, 0.1f));
         if (pa.x != FLT_MAX && pb.x != FLT_MAX)
            draw->AddLine(pa, pb, color, width * dpi);
      }
   };
   for (int i = 0; i < static_cast<int>(board->m_pfHoles.size()); i++)
   {
      const PlayfieldHole &h = board->m_pfHoles[i];
      const bool cut = (h.flags & PlayfieldHole::Cut) != 0;
      const bool overlap = cut && i < static_cast<int>(board->m_pfOverlap.size()) && board->m_pfOverlap[i];
      ImU32 color = !cut ? IM_COL32(150, 150, 150, 200) : overlap ? IM_COL32(255, 40, 40, 255)
         : (h.flags & PlayfieldHole::FallsThrough) && board->m_d.m_pfFloor == 0 ? IM_COL32(0, 220, 255, 255) : IM_COL32(255, 150, 0, 255);
      if (i == sel)
         color = overlap ? IM_COL32(255, 120, 120, 255) : !cut ? IM_COL32(230, 230, 230, 255) : IM_COL32(255, 230, 120, 255);
      outline(h, color, !cut, i == sel ? 2.5f : 1.5f);
      if (overlap)
         if (const ImVec2 p = ctx.Project(Vertex3Ds(h.centre.x, h.centre.y, 0.1f)); p.x != FLT_MAX)
            draw->AddText(ImVec2(p.x + 6.f * dpi, p.y - 6.f * dpi), IM_COL32(255, 60, 60, 255), "overlaps");
   }
   if (s.placing != 0 && ImGui::IsMousePosValid())
      outline(NewHole(ed.UnprojectToPlane(ImGui::GetMousePos(), 0.f)), IM_COL32(255, 230, 120, 200), false, 1.5f);
   draw->PopClipRect();
}

void PlayfieldMaker::RenderSection(EditorUI &ed, PropertyPane &props, Primitive *const board)
{
   if (board->GetPTable() != ed.m_table || ed.IsInspectMode())
   {
      props.FlushRow();
      ImGui::TextDisabled("The playfield is edited in the editor, not in a play test");
      return;
   }
   PinTable *const t = ed.m_table;
   bool layerEdited = false;

   // A file the Load... dialog or a drop brought (the drop is tested against the slot rows as drawn last frame)
   if (s.pendingLoad != nullptr && !s.pendingLoad->empty())
   {
      const string file = s.pendingLoad->front();
      const int slot = s.pendingSlot;
      s.pendingLoad = nullptr;
      if (!file.empty())
         layerEdited |= Impl::ApplyFile(ed, board, slot, file);
   }
   if (s.dropFrame >= 0)
      for (const State::SlotRect &r : s.rects)
         if (s.dropPos.x >= r.a.x && s.dropPos.x <= r.b.x && s.dropPos.y >= r.a.y && s.dropPos.y <= r.b.y)
         {
            s.dropFrame = -1;
            layerEdited |= Impl::ApplyFile(ed, board, r.slot, s.dropPath);
            break;
         }

   Primitive *const old = OldPlayfield(t);
   Primitive *const walls = CompanionOf(board, kPlayfieldWallsRole);
   Primitive *const floor = CompanionOf(board, kPlayfieldFloorRole);
   props.FlushRow();
   if (old != nullptr)
   {
      // Flips the authored visibility of the board, its walls and the old playfield together (not an undo step, saved as left)
      bool showOld = Impl::AuthoredVisible(ed, old);
      if (ImGui::Checkbox("Show Old Playfield", &showOld))
      {
         Impl::SetAuthoredVisible(ed, old, showOld);
         Impl::SetAuthoredVisible(ed, board, !showOld);
         Impl::SetAuthoredVisible(ed, walls, !showOld);
         if (ed.m_renderer)
            ed.m_renderer->MarkStaticPrepassDirty();
         t->SetNonUndoableDirty(SaveDirtyState::Dirty);
      }
      ImGui::SetItemTooltip("Draw %s instead of the board, to compare", old->GetName().c_str());
   }

   // Layer slots
   props.Separator("Layers"s);
   props.FlushRow();
   vector<State::SlotRect> rects;
   const float dpi = ed.m_liveUI.GetDPI();
   const float thumb = 28.f * dpi;
   bool findDone = false; // Find Holes ran in the layer loop
   const vector<Primitive *> cards = TracedCards(t);
   if (!board->m_d.m_szEmissionMap.empty())
      s.showGlow = true;
   if (!ed.m_refImage.GetFile(ed).empty())
      s.showBlueprint = true;
   for (int slot = 0; slot < SlotCount; slot++)
   {
      if ((slot == Glow && !s.showGlow) || (slot == Blueprint && !s.showBlueprint))
         continue;
      const string value = Impl::GetSlot(ed, board, slot);
      ImGui::PushID(slot);
      ImGui::BeginGroup();
      const Texture *const tex = slot == Blueprint || value.empty() ? nullptr : t->GetImage(value);
      ImTextureID image = tex != nullptr && ed.m_renderer ? ed.m_renderer->m_renderDevice->m_texMan.LoadTexture(const_cast<Texture *>(tex), false) : ImTextureID {};
      if (image)
         ImGui::Image(image, ImVec2(thumb, thumb));
      else
         ImGui::Dummy(ImVec2(thumb, thumb));
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::TextUnformatted(kSlotNames[slot]);
      if (slot == InsertPrint)
      {
         int users = 0;
         for (const Primitive *const c : cards)
            users += !value.empty() && c->m_d.m_szORMMap == value;
         if (cards.empty())
            ImGui::TextDisabled("no traced inserts");
         else
            ImGui::TextDisabled("%s, used by %d inserts", value.empty() ? "(none)" : value.c_str(), users);
      }
      else
         ImGui::TextDisabled("%s", value.empty() ? "(none)" : value.c_str());
      ImGui::EndGroup();
      ImGui::SameLine();
      ImGui::BeginDisabled(slot == InsertPrint && cards.empty()); // The slot's tooltip says why
      if (ImGui::Button("Load..."))
         Impl::StartLoad(ed, slot);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(value.empty() || slot == Blueprint || (slot == InsertPrint && cards.empty()));
      if (ImGui::Button("Clear"))
      {
         if (slot == InsertPrint)
            ImGui::OpenPopup("Clear Insert Print");
         else
         {
            Impl::SetSlot(ed, board, slot, string());
            layerEdited = true;
         }
      }
      ImGui::EndDisabled();
      if ((slot == Glow || slot == Blueprint) && value.empty())
      {
         ImGui::SameLine();
         if (ImGui::SmallButton("x"))
            (slot == Glow ? s.showGlow : s.showBlueprint) = false;
         ImGui::SetItemTooltip("Remove this layer");
      }
      if (ImGui::BeginPopupModal("Clear Insert Print", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
         int users = 0;
         for (const Primitive *const c : cards)
            users += c->m_d.m_szORMMap == value;
         ImGui::Text("This removes the print from %d inserts.", users);
         if (ImGui::Button("Remove"))
         {
            for (Primitive *const c : cards)
               if (c->m_d.m_szORMMap == value)
               {
                  ed.m_undo.MarkForUndo(c, true, false);
                  c->m_d.m_szORMMap.clear();
               }
            layerEdited = true;
            ImGui::CloseCurrentPopup();
         }
         ImGui::SameLine();
         if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
         ImGui::EndPopup();
      }
      ImGui::EndGroup();
      if (slot == InsertPrint && cards.empty())
         ImGui::SetItemTooltip("%s\nThis table has no traced inserts: nothing to load", kSlotTips[slot]);
      else
         ImGui::SetItemTooltip("%s\nDrop an image file here, or Load...", kSlotTips[slot]);
      rects.push_back({ slot, ImGui::GetItemRectMin(), ImGui::GetItemRectMax() });
      if (slot == InsertPrint && !cards.empty())
      {
         int others = 0;
         for (const Primitive *const c : cards)
            others += !c->m_d.m_szORMMap.empty() && c->m_d.m_szORMMap != value;
         if (others > 0)
         {
            // The cards a new print leaves alone, by name and image
            if (ImGui::TreeNode("##others", "%d inserts use another image", others))
            {
               for (const Primitive *const c : cards)
                  if (!c->m_d.m_szORMMap.empty() && c->m_d.m_szORMMap != value)
                     ImGui::BulletText("%s: %s", c->GetName().c_str(), c->m_d.m_szORMMap.c_str());
               ImGui::TreePop();
            }
            ImGui::Checkbox("All cards", &s.printAll);
            ImGui::SetItemTooltip("A new print goes on every traced insert, not only the ones on the current print");
         }
      }
      if (slot == WoodMask)
      {
         props.Combo<Primitive>(
            board, "Mask Mode"s, vector<string> { "Auto"s, "Dark"s, "Light"s, "Transparent"s }, //
            [](const Primitive *p) { return p->m_d.m_pfMaskMode; }, //
            [](Primitive *p, int v) { p->m_d.m_pfMaskMode = clamp(v, 0, 3); }, "Which pixels are holes: Auto reads the alpha if it has both values, else the minority of dark and light");
         props.InputFloat<Primitive>(
            board, "Min Size"s, //
            [](const Primitive *p) { return p->m_d.m_pfMinHole; }, //
            [](Primitive *p, float v) { p->m_d.m_pfMinHole = clamp(v, 0.f, 1000.f); }, PropertyPane::Unit::VPLength, 2, "Shapes smaller than this are ignored (0.05 in = 2.35)");
         ImGui::BeginDisabled(value.empty());
         if (ImGui::Button("Find Holes"))
         {
            bool ok = false;
            s.findStatus = FindHoles(t, board, ok);
            s.findStatusBad = !ok;
            s.findBoard = board;
            s.findImage = board->m_d.m_pfWoodMask;
            findDone = ok;
            if (ok)
               SelectHole(board, -1);
         }
         ImGui::EndDisabled();
         ImGui::SetItemTooltip("Cuts a hole for each shape in the Wood Mask, replacing the holes the last run made (a hole you moved, resized or toggled is kept). Holes under an insert are left uncut");
         if (!s.findStatus.empty() && s.findBoard == board && s.findImage == board->m_d.m_pfWoodMask)
         {
            ImGui::PushStyleColor(ImGuiCol_Text, s.findStatusBad ? ImVec4(1.f, 0.35f, 0.3f, 1.f) : ImVec4(0.6f, 0.9f, 0.6f, 1.f));
            ImGui::TextWrapped("%s", s.findStatus.c_str());
            ImGui::PopStyleColor();
         }
      }
      ImGui::PopID();
   }
   s.rects = std::move(rects);
   if (!s.showGlow || !s.showBlueprint)
   {
      if (ImGui::Button("+ Add Layer"))
         ImGui::OpenPopup("Add Layer");
      if (ImGui::BeginPopup("Add Layer"))
      {
         if (!s.showGlow && ImGui::Selectable("Glow"))
            s.showGlow = true;
         if (!s.showBlueprint && ImGui::Selectable("Blueprint"))
            s.showBlueprint = true;
         ImGui::EndPopup();
      }
   }
   props.CustomField(layerEdited, "Layer"s);

   // Board
   props.Separator("Board"s);
   props.InputFloat<Primitive>(
      board, "Thickness"s, //
      [](const Primitive *p) { return p->m_d.m_pfThickness; }, //
      [](Primitive *p, float v) { p->m_d.m_pfThickness = clamp(v, 1.f, 200.f); s.rebuildOnRelease = true; }, PropertyPane::Unit::VPLength, 2, "Plywood thickness (0.5 in = 23.5)");
   props.InputInt<Primitive>(
      board, "Curve Detail"s, //
      [](const Primitive *p) { return p->m_d.m_pfDetail; }, //
      [](Primitive *p, int v) { p->m_d.m_pfDetail = clamp(v, 1, 5); s.rebuildOnRelease = true; }, "How smooth the hole circles are (1 to 5, as plastics)");
   props.InputFloat<Primitive>(
      board, "Default Bevel"s, //
      [](const Primitive *p) { return p->m_d.m_pfDefaultBevel; }, //
      [](Primitive *p, float v) { p->m_d.m_pfDefaultBevel = clamp(v, 0.f, 200.f); s.rebuildOnRelease = true; }, PropertyPane::Unit::VPLength, 2,
      "The rounded rim of the holes set to Default (0 = none; 1/32 in is the Jurassic Park rim). The Wood Mask is the cut at the wall, as a router "
      "makes it: a bevelled hole is this much wider each side at the surface. Insert pockets never get one");
   props.InputInt<Primitive>(
      board, "Bevel Segments"s, //
      [](const Primitive *p) { return p->m_d.m_pfBevelSegments; }, //
      [](Primitive *p, int v) { p->m_d.m_pfBevelSegments = clamp(v, 1, 8); s.rebuildOnRelease = true; }, "Rings of the rounded bevel (1 = a chamfer)");
   {
      props.FlushRow();
      bool bevelEdited = false;
      const char *label = "";
      if (ImGui::Button("Bevel All Holes"))
      {
         bevelEdited = Impl::SetAllBevels(board, true);
         label = "Bevel All Holes";
      }
      ImGui::SetItemTooltip("Every cut through-hole to Default (Default Bevel), one undo step. Find Holes recreates traced holes at Default");
      ImGui::SameLine();
      if (ImGui::Button("Remove All Bevels"))
      {
         bevelEdited = Impl::SetAllBevels(board, false);
         label = "Remove All Bevels";
      }
      ImGui::SetItemTooltip("Every hole to Off, one undo step. Find Holes recreates traced holes at Default");
      if (bevelEdited)
         board->UpdatePlayfield();
      props.CustomField(bevelEdited, label);
   }

   // Floor
   props.FlushRow();
   const vector<Primitive *> &floors = Impl::Floors(ed);
   int mode = board->m_d.m_pfFloor;
   bool floorEdited = false;
   ImGui::TextUnformatted("Floor");
   ImGui::SameLine();
   if (ImGui::RadioButton("Own", mode == 0) && mode != 0)
   {
      board->m_d.m_pfFloor = 0;
      CopyFloorPhysics(t, board, floor);
      floorEdited = true;
   }
   ImGui::SetItemTooltip("The board's own floor collider: Falls Through holes drop the ball");
   ImGui::SameLine();
   ImGui::BeginDisabled(floors.empty() && mode != 1);
   if (ImGui::RadioButton("Keep Existing", mode == 1) && mode != 1)
   {
      board->m_d.m_pfFloor = 1;
      floorEdited = true;
   }
   DisabledTooltip(floors.empty() ? "No existing floor collider" : "The board is look only: the existing floor collider decides where the ball falls");
   ImGui::EndDisabled();
   if (board->m_d.m_pfFloor == 1 && floors.empty())
      ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "No floor: balls will fall through");
   for (Primitive *const f : floors)
   {
      ImGui::PushID(f);
      ImGui::TextDisabled("Floor collider: %s", f->GetName().c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Turn Collision Off"))
      {
         ed.m_undo.MarkForUndo(f, true, false);
         f->m_d.m_collidable = false;
         if (ed.m_player != nullptr && ed.m_player->m_physics != nullptr)
            ed.m_player->m_physics->Update(f);
         s.floorsStale = true;
         floorEdited = true;
      }
      ImGui::PopID();
   }
   if (floorEdited)
      board->UpdatePlayfield();
   props.CustomField(floorEdited, "Floor"s);

   // Holes
   props.Separator("Holes"s);
   props.FlushRow();
   bool holesEdited = findDone;
   vector<PlayfieldHole> &holes = board->m_pfHoles;
   int cut = 0, falls = 0, narrow = 0, overlap = 0, clamped = 0;
   const bool keep = board->m_d.m_pfFloor == 1;
   for (size_t i = 0; i < holes.size(); i++)
   {
      const PlayfieldHole &h = holes[i];
      const bool c = (h.flags & PlayfieldHole::Cut) != 0;
      cut += c;
      if (c && i < board->m_pfOverlap.size() && board->m_pfOverlap[i])
         overlap++;
      if (c && i < board->m_pfBevelClamped.size() && board->m_pfBevelClamped[i] >= 0.f)
         clamped++;
      if (c && (h.flags & PlayfieldHole::FallsThrough) && !keep)
      {
         falls++;
         if (h.type != PlayfieldHole::Polygon && h.diameter < 50.f) // The wall (cut) diameter, not the wider bevel at the surface: the ball rests on the wall's rim
            narrow++;
      }
   }
   ImGui::Text("%d holes, %d cut, %d fall through", static_cast<int>(holes.size()), cut, falls);
   if (overlap > 0)
      ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "%d holes overlap or leave the board: not cut until moved", overlap);
   if (clamped > 0)
   {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.8f, 0.3f, 1.f));
      ImGui::TextWrapped("%d bevels (*) are smaller than set: near the edge or another hole, too wide for the hole's shape, or over the thickness", clamped);
      ImGui::PopStyleColor();
   }
   if (falls > 0)
      ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "Make sure something below catches the ball");
   if (narrow > 0)
      ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%d Falls Through holes are narrower than the ball: it will rest on the rim", narrow);

   // Placing: a preset or typed sizes, then a click on the playfield
   {
      static int preset = 1;
      ImGui::SetNextItemWidth(220.f * dpi);
      if (ImGui::BeginCombo("##preset", preset >= 0 ? kPresets[preset].name : "Custom"))
      {
         for (int i = 0; i < static_cast<int>(std::size(kPresets)); i++)
            if (ImGui::Selectable(kPresets[i].name, preset == i))
            {
               preset = i;
               if (kPresets[i].slot)
               {
                  s.slotLength = kPresets[i].a * kVPUPerInch;
                  s.slotWidth = kPresets[i].b * kVPUPerInch;
               }
               else
                  s.roundDia = kPresets[i].a * kVPUPerInch;
            }
         ImGui::EndCombo();
      }
      // Typed sizes in inches, as the presets (kept in VPU)
      const auto inches = [&](const char *label, float &vpu, const float maxVpu)
      {
         float v = vpu / kVPUPerInch;
         ImGui::SetNextItemWidth(90.f * dpi);
         if (ImGui::InputFloat(label, &v, 0.f, 0.f, "%.3f in"))
         {
            vpu = clamp(v * kVPUPerInch, 1.f, maxVpu);
            preset = -1;
         }
      };
      inches("Diameter", s.roundDia, 1000.f);
      inches("Slot Length", s.slotLength, 2000.f);
      ImGui::SameLine();
      inches("Width", s.slotWidth, 1000.f);
      if (ImGui::Button(s.placing == 1 ? "Click to place..." : "Add Round Hole"))
      {
         s.placing = s.placing == 1 ? 0 : 1;
         ed.Report(s.placing ? "Click on the playfield to place the hole (Esc cancels)" : "");
      }
      ImGui::SameLine();
      if (ImGui::Button(s.placing == 2 ? "Click to place..." : "Add Slot"))
      {
         s.placing = s.placing == 2 ? 0 : 2;
         ed.Report(s.placing ? "Click on the playfield to place the slot (Esc cancels)" : "");
      }
   }

   // The list: a table that fits the panel (hole, Cut, Falls Through, Bevel, delete)
   int sel = SelectedHole(board);
   int toDelete = -1;
   const ImGuiStyle &style = ImGui::GetStyle();
   const float rowH = ImGui::GetFrameHeight() + style.CellPadding.y * 2.f;
   if (!holes.empty()
      && ImGui::BeginTable("##holes", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingFixedFit,
         ImVec2(0.f, (min(static_cast<float>(holes.size()), 8.f) + 1.f) * rowH + 4.f * dpi)))
   {
      const float box = ImGui::GetFrameHeight();
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Hole", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Cut", ImGuiTableColumnFlags_WidthFixed, max(box, ImGui::CalcTextSize("Cut").x));
      ImGui::TableSetupColumn("Falls", ImGuiTableColumnFlags_WidthFixed, max(box, ImGui::CalcTextSize("Falls").x));
      ImGui::TableSetupColumn("Bevel", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00.00").x + style.FramePadding.x * 2.f);
      ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("x").x + style.FramePadding.x * 2.f);
      ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
      static constexpr const char *kHeaderTips[] = { "Click a hole to select it (G moves it in the viewport)", "Cut through the board", "Falls Through: the floor collider has the hole, the ball drops",
         "Bevel of the hole's rim: Def = the board's Default Bevel, Off = none, a number = its own size (Hole Bevel Size)", nullptr };
      for (int col = 0; col < 5; col++)
      {
         ImGui::TableSetColumnIndex(col);
         ImGui::TableHeader(ImGui::TableGetColumnName(col));
         if (kHeaderTips[col] != nullptr)
            ImGui::SetItemTooltip("%s", kHeaderTips[col]);
      }
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(holes.size()), rowH);
      while (clipper.Step())
         for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
         {
            PlayfieldHole &h = holes[i];
            ImGui::PushID(i);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
            const bool pocket = (h.flags & PlayfieldHole::UnderInsert) != 0;
            const float clampedTo = (h.flags & PlayfieldHole::Cut) && i < static_cast<int>(board->m_pfBevelClamped.size()) ? board->m_pfBevelClamped[i] : -1.f;

            // Hole: "1 Round 0.580 in", ellipsis when the column is narrow, then a marker: * (amber) bevel clamped, ins = insert pocket
            ImGui::TableSetColumnIndex(0);
            char label[96];
            const char *const type = h.type == PlayfieldHole::Slot ? "Slot" : h.type == PlayfieldHole::Polygon ? "Shape" : "Round";
            if (h.type == PlayfieldHole::Slot) // Length (end to end) x width, as the preset
               snprintf(label, sizeof(label), "%d %s %.2f x %.3f in", i + 1, type, (hypotf(h.end.x - h.centre.x, h.end.y - h.centre.y) + h.diameter) / kVPUPerInch, h.diameter / kVPUPerInch);
            else
               snprintf(label, sizeof(label), "%d %s %.3f in", i + 1, type, h.diameter / kVPUPerInch);
            const char *const marker = pocket ? "ins" : clampedTo >= 0.f ? "*" : nullptr;
            const float markerW = marker ? ImGui::CalcTextSize(marker).x + style.ItemSpacing.x : 0.f;
            const float avail = max(ImGui::GetContentRegionAvail().x - markerW, 1.f);
            string text = label;
            if (ImGui::CalcTextSize(text.c_str()).x > avail)
            {
               while (!text.empty() && ImGui::CalcTextSize((text + "...").c_str()).x > avail)
                  text.pop_back();
               text += "...";
            }
            text += "##hole";
            if (ImGui::Selectable(text.c_str(), i == sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(avail, 0.f)))
            {
               SelectHole(board, i);
               sel = i;
            }
            const auto holeTip = [&]()
            {
               if (pocket)
                  ImGui::SetItemTooltip("%s: an insert pocket (under an insert), never bevelled", label);
               else if (clampedTo >= 0.f)
                  ImGui::SetItemTooltip("%s: bevel clamped to %.3f in (%.2f VPU): near the edge or another hole, too wide for the hole's shape, or over the thickness", label,
                     clampedTo / kVPUPerInch, clampedTo);
               else
                  ImGui::SetItemTooltip("%s", label);
            };
            holeTip();
            if (marker)
            {
               ImGui::SameLine();
               if (pocket)
                  ImGui::TextDisabled("%s", marker);
               else
                  ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", marker);
               holeTip();
            }

            ImGui::TableSetColumnIndex(1);
            bool c = (h.flags & PlayfieldHole::Cut) != 0;
            if (ImGui::Checkbox("##cut", &c))
            {
               h.flags = (c ? (h.flags | PlayfieldHole::Cut) : (h.flags & ~PlayfieldHole::Cut)) & ~PlayfieldHole::FromMask;
               holesEdited = true;
            }
            ImGui::SetItemTooltip("Cut");

            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(keep);
            bool f = (h.flags & PlayfieldHole::FallsThrough) != 0;
            if (ImGui::Checkbox("##falls", &f))
            {
               h.flags = (f ? (h.flags | PlayfieldHole::FallsThrough) : (h.flags & ~PlayfieldHole::FallsThrough)) & ~PlayfieldHole::FromMask;
               holesEdited = true;
            }
            DisabledTooltip(keep ? "The existing floor decides where the ball falls" : "Falls Through");
            ImGui::EndDisabled();

            // Bevel: Def / Off / its own size (set in the selected hole's fields)
            ImGui::TableSetColumnIndex(3);
            {
               char preview[16];
               if (pocket || h.bevel == 0.f)
                  snprintf(preview, sizeof(preview), "Off");
               else if (h.bevel < 0.f)
                  snprintf(preview, sizeof(preview), "Def");
               else
                  snprintf(preview, sizeof(preview), "%.2f", h.bevel);
               ImGui::SetNextItemWidth(-FLT_MIN);
               ImGui::BeginDisabled(pocket);
               if (ImGui::BeginCombo("##bevel", preview, ImGuiComboFlags_NoArrowButton))
               {
                  const int mode = h.bevel < 0.f ? 0 : h.bevel == 0.f ? 1 : 2;
                  static constexpr const char *kModes[] = { "Default", "Off", "Custom" };
                  for (int m = 0; m < 3; m++)
                     if (ImGui::Selectable(kModes[m], m == mode) && m != mode)
                     {
                        h.bevel = m == 0 ? -1.f : m == 1 ? 0.f : (board->m_d.m_pfDefaultBevel > 0.f ? board->m_d.m_pfDefaultBevel : 1.47f);
                        h.flags &= ~PlayfieldHole::FromMask;
                        holesEdited = true;
                     }
                  ImGui::EndCombo();
               }
               DisabledTooltip(pocket ? "Insert pockets are built without a bevel (a real insert is sanded flush)" : "Bevel of the hole's rim: the board's Default Bevel, none, or its own (Hole Bevel Size, VPU)");
               ImGui::EndDisabled();
            }

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("x"))
               toDelete = i;
            ImGui::SetItemTooltip("Delete the hole");
            ImGui::PopID();
         }
      ImGui::EndTable();
   }
   if (toDelete >= 0)
   {
      holes.erase(holes.begin() + toDelete);
      if (sel == toDelete)
         SelectHole(board, -1);
      else if (sel > toDelete)
         SelectHole(board, sel - 1);
      holesEdited = true;
   }
   if (holesEdited)
      board->UpdatePlayfield();
   props.CustomField(holesEdited, "Holes"s);

   // The selected hole's fields (G moves it in the viewport)
   if (SelectedHole(board) >= 0)
   {
      const auto holeAt = [](const Primitive *p) -> const PlayfieldHole * { const int i = SelectedHole(p); return i >= 0 ? &p->m_pfHoles[i] : nullptr; };
      const auto holeEdit = [](Primitive *p, const std::function<void(PlayfieldHole &)> &fn)
      {
         if (const int i = SelectedHole(p); i >= 0)
         {
            fn(p->m_pfHoles[i]);
            p->m_pfHoles[i].flags &= ~PlayfieldHole::FromMask; // edited by hand: the next Find Holes keeps it
            s.rebuildOnRelease = true;
         }
      };
      const PlayfieldHole *const h = holeAt(board);
      props.InputFloat<Primitive>(
         board, "Hole X"s, [holeAt](const Primitive *p) { const PlayfieldHole *h = holeAt(p); return h ? h->centre.x : 0.f; }, //
         [holeEdit](Primitive *p, float v) { holeEdit(p, [v](PlayfieldHole &h) { MoveHole(h, PlayfieldHole(h), v - h.centre.x, 0.f); }); }, PropertyPane::Unit::VPLength, 2, "G moves the hole in the viewport");
      props.InputFloat<Primitive>(
         board, "Hole Y"s, [holeAt](const Primitive *p) { const PlayfieldHole *h = holeAt(p); return h ? h->centre.y : 0.f; }, //
         [holeEdit](Primitive *p, float v) { holeEdit(p, [v](PlayfieldHole &h) { MoveHole(h, PlayfieldHole(h), 0.f, v - h.centre.y); }); }, PropertyPane::Unit::VPLength, 2);
      if (h != nullptr && h->type != PlayfieldHole::Polygon)
         props.InputFloat<Primitive>(
            board, "Hole Diameter"s, [holeAt](const Primitive *p) { const PlayfieldHole *h = holeAt(p); return h ? h->diameter : 0.f; }, //
            [holeEdit](Primitive *p, float v) { holeEdit(p, [v](PlayfieldHole &h) { h.diameter = clamp(v, 1.f, 1000.f); }); }, PropertyPane::Unit::VPLength, 2);
      if (h != nullptr && !(h->flags & PlayfieldHole::UnderInsert))
      {
         props.Combo<Primitive>(
            board, "Hole Bevel"s, { "Default"s, "Off"s, "Custom"s }, [holeAt](const Primitive *p) { const PlayfieldHole *h = holeAt(p); return !h || h->bevel < 0.f ? 0 : h->bevel == 0.f ? 1 : 2; }, //
            [holeEdit](Primitive *p, int v)
            {
               const float def = p->m_d.m_pfDefaultBevel;
               holeEdit(p, [v, def](PlayfieldHole &h) { h.bevel = v == 0 ? -1.f : v == 1 ? 0.f : (h.bevel > 0.f ? h.bevel : def > 0.f ? def : 1.47f); });
            },
            "Default: the board's Default Bevel. Off: none. Custom: the size below");
         if (h->bevel > 0.f)
            props.InputFloat<Primitive>(
               board, "Hole Bevel Size"s, [holeAt](const Primitive *p) { const PlayfieldHole *h = holeAt(p); return h ? max(h->bevel, 0.f) : 0.f; }, //
               [holeEdit](Primitive *p, float v) { holeEdit(p, [v](PlayfieldHole &h) { h.bevel = clamp(v, 0.f, 200.f); }); }, PropertyPane::Unit::VPLength, 2,
               "The rounded rim at the top of this hole: the hole is this much wider each side at the surface than its cut");
      }
      if (h != nullptr && h->type == PlayfieldHole::Slot)
      {
         props.InputFloat<Primitive>(
            board, "Hole End X"s, [holeAt](const Primitive *p) { const PlayfieldHole *h = holeAt(p); return h ? h->end.x : 0.f; }, //
            [holeEdit](Primitive *p, float v) { holeEdit(p, [v](PlayfieldHole &h) { h.end.x = v; }); }, PropertyPane::Unit::VPLength, 2);
         props.InputFloat<Primitive>(
            board, "Hole End Y"s, [holeAt](const Primitive *p) { const PlayfieldHole *h = holeAt(p); return h ? h->end.y : 0.f; }, //
            [holeEdit](Primitive *p, float v) { holeEdit(p, [v](PlayfieldHole &h) { h.end.y = v; }); }, PropertyPane::Unit::VPLength, 2);
      }
   }
   if (s.rebuildOnRelease && !ImGui::IsAnyItemActive())
   {
      s.rebuildOnRelease = false;
      board->UpdatePlayfield(); // Once the field is let go
   }
}

}
