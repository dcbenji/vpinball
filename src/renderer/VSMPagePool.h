// license:GPLv3+

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "math/vector.h"
#include "math/matrix.h"

// PBR Virtual Shadow Map bookkeeping (UE5-style paged shadow maps).
//
// Each shadowed PBR light owns a large *virtual* map tiled into fixed-size pages; only pages
// containing casters are allocated a physical layer of the pool depth texture array. Pages are
// persistent spatial caches: a page renders once when allocated, then re-renders only when marked
// dirty (dynamic caster coverage every frame, light move, content edit). No temporal accumulation.
//
// This unit is deliberately free of GPU/renderer types so the coverage marking, allocation and
// eviction logic can be unit-tested headless. The renderer mirrors the same conventions on the
// GPU side (one RenderTarget per physical layer, tex_vsm_pool / tex_vsm_pagetables samplers).
namespace VSM
{
constexpr int PAGE_DIM = 128; // physical page edge in texels (one array layer per page); named PAGE_DIM since PAGE_SIZE is a system macro on some platforms
constexpr int MAX_LIGHTS = 32; // max shadowed lights (index+1 packed in 8 bits of the light record)
constexpr int CUBE_FACE_GRID = 2; // virtual pages per cube-face edge -> 4 pages per face, 24 per light
constexpr int MAX_ORTHO_GRID = 64; // max pages per edge of an ortho virtual map (64 pages = 8192 texels)
constexpr int PAGETABLE_STRIDE = MAX_ORTHO_GRID * MAX_ORTHO_GRID; // page-table texels reserved per light
constexpr int PAGETABLE_W = 512; // tex_vsm_pagetables texture width in texels (height 256 holds 32 lights)
constexpr int LAYOUT_ORTHO = 0; // ortho virtual map: gridW x gridH pages over the fitted light-space box
constexpr int LAYOUT_CUBE = 2; // cube virtual map: 6 faces of CUBE_FACE_GRID^2 pages (layout ids kept for fs_pbr)

// Clip-space transform zooming a whole-map projection into a single page: page (px, py) of a gridW x gridH virtual map becomes
// the full NDC box. Composed on the right of the light's projection (pageVP = view * proj * PageZoomMatrix), so page depth is
// the map's depth unchanged. Page (0, 0) is the top left (+y NDC) one, as the coverage marking and fs_pbr index them.
inline Matrix3D PageZoomMatrix(const int px, const int py, const int gridW, const int gridH)
{
   Matrix3D zoom;
   zoom.SetIdentity();
   zoom._11 = (float)gridW;
   zoom._22 = (float)gridH;
   zoom._41 = (float)(gridW - 1 - 2 * px);
   zoom._42 = (float)(1 - gridH + 2 * py);
   return zoom;
}

// Forward/up axes of the 6 cube faces, matching the rasterization lookAt calls and the
// pbr_shadow_cube_lookup face order in fs_pbr (+X, -X, +Y, -Y, +Z, -Z)
extern const Vertex3Ds CUBE_FACE_FWD[6];
extern const Vertex3Ds CUBE_FACE_UP[6];

// Virtual page state of one shadowed light. For LAYOUT_ORTHO the virtual map is a gridW x gridH
// page grid over the light's fitted ortho box; for LAYOUT_CUBE it is 6 faces of gridW x gridH
// pages each (gridW == gridH == CUBE_FACE_GRID), face f's entries starting at f * gridW * gridH.
struct LightMap
{
   int layout = LAYOUT_ORTHO;
   int gridW = 0; // virtual map width in pages (per face for cube)
   int gridH = 0; // virtual map height in pages (per face for cube)
   int ptBase = 0; // first page-table texel of this light in tex_vsm_pagetables (shadowIndex * PAGETABLE_STRIDE)

   std::vector<int> phys; // per virtual page: physical pool layer + 1 (0 = not resident)
   std::vector<uint8_t> needed; // caster coverage marked this frame
   std::vector<uint8_t> dirty; // stale content: page must be (re)rendered (bit 1: freshly allocated, kept out of the page table until rendered)

   int EntryCount() const { return gridW * gridH * (layout == LAYOUT_CUBE ? 6 : 1); }
   void Reset(int layout_, int w, int h);
};

// Physical page pool: a fixed number of layers of the shared depth texture array. Allocation pops
// the free list; when empty, the coldest page not marked needed this frame is evicted (LRU).
class PagePool
{
public:
   explicit PagePool(int layerCount) { Reset(layerCount); }

   void Reset(int layerCount);

   // Clears the per-frame eviction protection; stamp identifies this marking frame
   void BeginFrame(uint64_t stamp)
   {
      m_stamp = stamp;
      m_protected.assign(m_ownerLight.size(), uint8_t(0));
   }

   // Allocates a physical layer for (light, vpage): free list first, else LRU-evicts the coldest
   // unprotected page (the evicted owner's entry is reported through the out params). Returns the
   // layer index, or -1 when every page is protected this frame.
   int Alloc(int light, int vpage, int* evictLight = nullptr, int* evictVPage = nullptr);

   void Free(int layer);

   // Marks a resident layer as needed for the current frame (LRU stamp + eviction protection)
   void Touch(int layer)
   {
      m_stampOf[layer] = m_stamp;
      m_protected[layer] = 1;
   }

   int LayerCount() const { return (int)m_ownerLight.size(); }
   int FreeCount() const { return (int)m_free.size(); }

private:
   std::vector<int> m_free; // free physical layers
   std::vector<int> m_ownerLight; // layer -> owner light index (-1 = free)
   std::vector<int> m_ownerPage; // layer -> owner virtual page
   std::vector<uint64_t> m_stampOf; // layer -> last-needed frame stamp
   std::vector<uint8_t> m_protected; // layer -> marked needed this frame (no eviction)
   uint64_t m_stamp = 0;
};

// Shadow slot policy. More shadow-capable lights than slots (PBRShadowMaxLights) must not cascade: a lit light without a slot
// takes a free slot, else the slot of the least important *parked* (unlit) light that is less important than itself, else, at
// most once per SLOT_LIT_SWAP_US over the whole table, the slot of the least important *lit* light when it is at least
// SLOT_LIT_SWAP_RATIO times less important, else it stays slot-starved (unshadowed) this frame. Free lit-to-lit taking used
// to cascade (the loser took the next one's slot, down the part order, each re-rendering its whole map: Ripley's Redux
// 2026-11-13, 42 lights for 32 slots); the ratio and the rate limit keep the swap one-way and one at a time, so the important
// lights end up shadowed whichever lit first (GI at power-up before the first flasher fire).
// Importance is the light's authored intensity x range (stable: not its current fade level, nor the script's IntensityScale,
// which Flupper-style flasher scripts drive to 0 between fires), so which lights lose their
// shadows under the cap is chosen, not arrival order: the slots end up with the most important shadow-capable lights, lit or
// parked, and stay there. A parked light keeps its slot however long it rests (a flasher between shows keeps its warm map and
// bake; an expiry handed resting flashers' slots to GI lights, so every show knocked a GI shadow out for the expiry time and
// flashers firing together went unshadowed). A light that stays dark for good keeps its slot too: hide it to free it.
// The renderer passes the authored intensity (Light m_d.m_intensity), never one scaled by the script's IntensityScale
inline float SlotImportance(float intensity, float range) { return std::max(intensity, 0.f) * std::max(range, 1.f); }

// The slot set: member[i] = 1 for the 'cap' most important of these lights (importance, then index order: the caller's part order).
// The renderer never offers a member's slot to PickSlot (see below).
std::vector<uint8_t> SlotSetMembers(const std::vector<float>& importance, int cap);

constexpr float SLOT_LIT_SWAP_RATIO = 2.f;
constexpr uint64_t SLOT_LIT_SWAP_US = 1000000; // one lit-to-lit swap per second at most

// slotFree[i]: no light holds slot i; parkedImportance[i]: importance of the parked (unlit) light holding slot i, < 0 when the
// slot is held by a lit light or already assigned this frame; litImportance[i] (nullptr = no lit swap this frame): importance
// of the lit light holding slot i not yet confirmed this frame, else < 0. Returns the slot for a lit light of the given
// importance (free slots first, lowest index), or -1.
// takerInSet: the taker belongs to the slot set (the caller's PBRShadowMaxLights most important lights, whose holders it never
// offers): it may take a lit holder's slot without the 2x ratio (every offered holder is outside the set, ranked below it), else a
// set member within 2x of a lit holder outside the set would starve for good.
int PickSlot(int slotCount, const uint8_t* slotFree, const float* parkedImportance, float importance, const float* litImportance = nullptr, bool takerInSet = false);

// Marks the virtual pages overlapped by a caster AABB for an ortho light. boundsMin/boundsMax are
// world space; the box (l, r, b, t) is the fitted ortho extent in light view space as returned by
// the ortho fit (light view matrix + box fit over the table bounds). Coverage matches the shader's
// uv convention: px = u * gridW with u = (lx - l) / (r - l), py = v * gridH with v = (t - ly) / (t - b).
// 'dilate' inflates the marked page rect on each side (PCF penumbra bleed). Sets needed[py*gridW+px].
void MarkOrthoPages(const Matrix3D& lightView, float l, float r, float b, float t, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int gridW, int gridH, int dilate, uint8_t* needed);

// Page coverage of one caster, as a rect of an ortho map's page grid (empty when x1 < x0) or a bit mask of a cube map's
// 6 * CUBE_FACE_GRID^2 pages (bit = virtual page index). Same pages as MarkOrthoPages / MarkCubePages.
struct PageRect
{
   int16_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
   bool Empty() const { return x1 < x0 || y1 < y0; }
   bool Contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
   void Fill(uint8_t* pages, int gridW) const
   {
      for (int y = y0; y <= y1; y++)
         for (int x = x0; x <= x1; x++)
            pages[y * gridW + x] |= 1; // |=: keeps a dirty page's fresh bit (2)
   }
};
static_assert(6 * CUBE_FACE_GRID * CUBE_FACE_GRID <= 32, "cube page masks are 32 bit");
PageRect OrthoPageRect(const Matrix3D& lightView, float l, float r, float b, float t, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int gridW, int gridH, int dilate);
uint32_t CubePageMask(const Vertex3Ds& lightPos, float zNear, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int dilate);

// A shadow caster as the coverage marking sees it: world bounds and a version bumped when they change (dynamic casters)
struct Caster
{
   void* part = nullptr;
   bool isStatic = false;
   bool active = true; // false: never drawn in the shadow pass (invisible wall): marks no page
   bool noBounds = false; // not culled per page (no bounds, or bounds that may not hold what it draws): drawn into every page
   Vertex3Ds boundsMin, boundsMax;
   unsigned int version = 0;
};

// Per light page coverage of every caster, cached: static casters are marked once per map (light, position, layout, grid,
// caster list) instead of every frame (~0.1 ms per shadowed light per frame on Ripley's Redux), only casters whose version
// moved are re-marked. Holds the PCF-dilated coverage (page allocation) and the undilated one (the pages a caster can
// rasterize into: per page caster culling).
class Coverage
{
public:
   // Both return the number of casters (re)marked
   int UpdateCube(const void* light, const Vertex3Ds& lightPos, float zNear, unsigned int casterSerial, const std::vector<Caster>& casters);
   int UpdateOrtho(const void* light, const Vertex3Ds& lightPos, const Matrix3D& lightView, float l, float r, float b, float t, int gridW, int gridH, int dilate,
      unsigned int casterSerial, const std::vector<Caster>& casters);

   // needed[vp] = 1 for the pages of the active casters; dirty[vp] |= 1 for those of the active dynamic casters when dynamicTier
   void Mark(const std::vector<Caster>& casters, bool dynamicTier, uint8_t* needed, uint8_t* dirty) const;

   // True when the caster's (undilated) coverage includes the virtual page
   bool InPage(size_t caster, int vpage) const
   {
      if (caster >= m_version.size())
         return true;
      return m_layout == LAYOUT_CUBE ? ((m_cubeTight[caster] >> vpage) & 1u) != 0 : m_orthoTight[caster].Contains(vpage % m_gridW, vpage / m_gridW);
   }

private:
   bool Rekey(const void* light, const Vertex3Ds& pos, int layout, int gridW, int gridH, unsigned int casterSerial, size_t casterCount);

   const void* m_light = nullptr;
   Vertex3Ds m_pos;
   int m_layout = -1, m_gridW = 0, m_gridH = 0;
   unsigned int m_casterSerial = 0;
   std::vector<unsigned int> m_version; // caster version each entry was marked at
   std::vector<uint32_t> m_cube, m_cubeTight; // cube layout: page bit masks, PCF dilated / undilated
   std::vector<PageRect> m_ortho, m_orthoTight; // ortho layout: page rects, PCF dilated / undilated
};

// Marks the virtual pages of the 6 cube faces for a near light. A caster crossing a face plane
// conservatively marks that whole face; otherwise each corner projects with the same formulas as
// pbr_shadow_cube_lookup and the covered page range is marked. Sets needed[f*g*g + py*g + px].
void MarkCubePages(const Vertex3Ds& lightPos, float zNear, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int faceGrid, int dilate, uint8_t* needed);
}
