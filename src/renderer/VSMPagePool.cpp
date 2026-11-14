// license:GPLv3+

#include "core/stdafx.h"
#include "VSMPagePool.h"

#include <algorithm>
#include <float.h>
#include <math.h>

namespace VSM
{
const Vertex3Ds CUBE_FACE_FWD[6] = { { 1.f, 0.f, 0.f }, { -1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, -1.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, -1.f } };
const Vertex3Ds CUBE_FACE_UP[6] = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0.f, -1.f, 0.f }, { 0.f, -1.f, 0.f } };

void LightMap::Reset(int layout_, int w, int h)
{
   layout = layout_;
   gridW = w;
   gridH = h;
   const int count = EntryCount();
   phys.assign(count, 0);
   needed.assign(count, 0);
   dirty.assign(count, 1); // a fresh map renders every marked page once
}

void PagePool::Reset(int layerCount)
{
   m_free.resize(layerCount);
   for (int i = 0; i < layerCount; i++)
      m_free[i] = layerCount - 1 - i; // pop order: layer 0 first
   m_ownerLight.assign(layerCount, -1);
   m_ownerPage.assign(layerCount, -1);
   m_stampOf.assign(layerCount, 0);
   m_protected.assign(layerCount, 0);
   m_stamp = 0;
}

int PagePool::Alloc(int light, int vpage, int* evictLight, int* evictVPage)
{
   int layer;
   if (!m_free.empty())
   {
      layer = m_free.back();
      m_free.pop_back();
   }
   else
   {
      // LRU eviction among pages not marked needed this frame
      int best = -1;
      for (int i = 0; i < (int)m_ownerLight.size(); i++)
         if (!m_protected[i] && (best < 0 || m_stampOf[i] < m_stampOf[best]))
            best = i;
      if (best < 0)
         return -1;
      layer = best;
      if (evictLight != nullptr)
         *evictLight = m_ownerLight[layer];
      if (evictVPage != nullptr)
         *evictVPage = m_ownerPage[layer];
   }
   m_ownerLight[layer] = light;
   m_ownerPage[layer] = vpage;
   Touch(layer);
   return layer;
}

void PagePool::Free(int layer)
{
   if (layer < 0 || layer >= (int)m_ownerLight.size() || m_ownerLight[layer] < 0)
      return;
   m_ownerLight[layer] = -1;
   m_ownerPage[layer] = -1;
   m_stampOf[layer] = 0;
   m_protected[layer] = 0;
   m_free.push_back(layer);
}

std::vector<uint8_t> SlotSetMembers(const std::vector<float>& importance, int cap)
{
   std::vector<int> order(importance.size());
   for (size_t i = 0; i < order.size(); i++)
      order[i] = (int)i;
   std::stable_sort(order.begin(), order.end(), [&importance](int a, int b) { return importance[a] > importance[b]; });
   std::vector<uint8_t> member(importance.size(), 0);
   for (int i = 0; i < cap && i < (int)order.size(); i++)
      member[order[i]] = 1;
   return member;
}

int PickSlot(int slotCount, const uint8_t* slotFree, const float* parkedImportance, float importance, const float* litImportance, const bool takerInSet)
{
   for (int i = 0; i < slotCount; i++)
      if (slotFree[i])
         return i;
   int best = -1;
   for (int i = 0; i < slotCount; i++)
      if (parkedImportance[i] >= 0.f && parkedImportance[i] < importance && (best < 0 || parkedImportance[i] < parkedImportance[best]))
         best = i;
   if (best >= 0 || litImportance == nullptr)
      return best;
   for (int i = 0; i < slotCount; i++)
      if (litImportance[i] >= 0.f && (takerInSet || litImportance[i] * SLOT_LIT_SWAP_RATIO <= importance) && (best < 0 || litImportance[i] < litImportance[best]))
         best = i;
   return best;
}

PageRect OrthoPageRect(const Matrix3D& lightView, float l, float r, float b, float t, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int gridW, int gridH, int dilate)
{
   float px0 = FLT_MAX, px1 = -FLT_MAX, py0 = FLT_MAX, py1 = -FLT_MAX;
   for (int c = 0; c < 8; c++)
   {
      const Vertex3Ds corner(c & 1 ? boundsMax.x : boundsMin.x, c & 2 ? boundsMax.y : boundsMin.y, c & 4 ? boundsMax.z : boundsMin.z);
      const Vertex3Ds v = lightView.MultiplyVectorNoPerspective(corner);
      px0 = std::min(px0, (v.x - l) / (r - l) * gridW);
      px1 = std::max(px1, (v.x - l) / (r - l) * gridW);
      py0 = std::min(py0, (t - v.y) / (t - b) * gridH);
      py1 = std::max(py1, (t - v.y) / (t - b) * gridH);
   }
   PageRect rect;
   rect.x0 = (int16_t)std::max(0, (int)floorf(px0) - dilate);
   rect.x1 = (int16_t)std::min(gridW - 1, (int)ceilf(px1) - 1 + dilate);
   rect.y0 = (int16_t)std::max(0, (int)floorf(py0) - dilate);
   rect.y1 = (int16_t)std::min(gridH - 1, (int)ceilf(py1) - 1 + dilate);
   return rect;
}

void MarkOrthoPages(const Matrix3D& lightView, float l, float r, float b, float t, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int gridW, int gridH, int dilate, uint8_t* needed)
{
   OrthoPageRect(lightView, l, r, b, t, boundsMin, boundsMax, gridW, gridH, dilate).Fill(needed, gridW);
}

uint32_t CubePageMask(const Vertex3Ds& lightPos, float zNear, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int dilate)
{
   constexpr int count = 6 * CUBE_FACE_GRID * CUBE_FACE_GRID;
   uint8_t pages[count] = {};
   MarkCubePages(lightPos, zNear, boundsMin, boundsMax, CUBE_FACE_GRID, dilate, pages);
   uint32_t mask = 0;
   for (int i = 0; i < count; i++)
      mask |= (uint32_t)pages[i] << i;
   return mask;
}

bool Coverage::Rekey(const void* light, const Vertex3Ds& pos, int layout, int gridW, int gridH, unsigned int casterSerial, size_t casterCount)
{
   if (m_light == light && m_pos.x == pos.x && m_pos.y == pos.y && m_pos.z == pos.z && m_layout == layout && m_gridW == gridW && m_gridH == gridH
      && m_casterSerial == casterSerial && m_version.size() == casterCount)
      return false;
   m_light = light;
   m_pos = pos;
   m_layout = layout;
   m_gridW = gridW;
   m_gridH = gridH;
   m_casterSerial = casterSerial;
   m_version.assign(casterCount, ~0u);
   m_cube.assign(layout == LAYOUT_CUBE ? casterCount : 0, 0u);
   m_cubeTight.assign(layout == LAYOUT_CUBE ? casterCount : 0, 0u);
   m_ortho.assign(layout == LAYOUT_CUBE ? 0 : casterCount, PageRect {});
   m_orthoTight.assign(layout == LAYOUT_CUBE ? 0 : casterCount, PageRect {});
   return true;
}

int Coverage::UpdateCube(const void* light, const Vertex3Ds& lightPos, float zNear, unsigned int casterSerial, const std::vector<Caster>& casters)
{
   Rekey(light, lightPos, LAYOUT_CUBE, CUBE_FACE_GRID, CUBE_FACE_GRID, casterSerial, casters.size());
   int marked = 0;
   for (size_t i = 0; i < casters.size(); i++)
   {
      const Caster& c = casters[i];
      if (m_version[i] == c.version)
         continue;
      // Allocation: one page ring of PCF bleed (taps reach up to 8/(grid*PAGE_DIM) of a face, under a page, see fs_pbr filterRadius clamp)
      m_cube[i] = CubePageMask(lightPos, zNear, c.boundsMin, c.boundsMax, 1);
      m_cubeTight[i] = c.noBounds ? ~0u : CubePageMask(lightPos, zNear, c.boundsMin, c.boundsMax, 0);
      m_version[i] = c.version;
      marked++;
   }
   return marked;
}

int Coverage::UpdateOrtho(const void* light, const Vertex3Ds& lightPos, const Matrix3D& lightView, float l, float r, float b, float t, int gridW, int gridH, int dilate,
   unsigned int casterSerial, const std::vector<Caster>& casters)
{
   Rekey(light, lightPos, LAYOUT_ORTHO, gridW, gridH, casterSerial, casters.size());
   int marked = 0;
   for (size_t i = 0; i < casters.size(); i++)
   {
      const Caster& c = casters[i];
      if (m_version[i] == c.version)
         continue;
      m_ortho[i] = PageRect {};
      m_orthoTight[i] = PageRect { 0, 0, (int16_t)(gridW - 1), (int16_t)(gridH - 1) };
      if (c.boundsMin.x <= c.boundsMax.x && c.boundsMin.y <= c.boundsMax.y && c.boundsMin.z <= c.boundsMax.z)
      {
         m_ortho[i] = OrthoPageRect(lightView, l, r, b, t, c.boundsMin, c.boundsMax, gridW, gridH, dilate);
         if (!c.noBounds)
            m_orthoTight[i] = OrthoPageRect(lightView, l, r, b, t, c.boundsMin, c.boundsMax, gridW, gridH, 0);
      }
      m_version[i] = c.version;
      marked++;
   }
   return marked;
}

void Coverage::Mark(const std::vector<Caster>& casters, bool dynamicTier, uint8_t* needed, uint8_t* dirty) const
{
   if (m_layout == LAYOUT_CUBE)
   {
      uint32_t neededMask = 0, dirtyMask = 0;
      for (size_t i = 0; i < casters.size() && i < m_cube.size(); i++)
         if (casters[i].active)
         {
            neededMask |= m_cube[i];
            if (dynamicTier && !casters[i].isStatic)
               dirtyMask |= m_cube[i];
         }
      for (int vp = 0; vp < 6 * CUBE_FACE_GRID * CUBE_FACE_GRID; vp++)
      {
         needed[vp] = (neededMask >> vp) & 1u;
         dirty[vp] |= (dirtyMask >> vp) & 1u;
      }
   }
   else
   {
      std::fill(needed, needed + m_gridW * m_gridH, uint8_t(0));
      for (size_t i = 0; i < casters.size() && i < m_ortho.size(); i++)
         if (casters[i].active)
         {
            m_ortho[i].Fill(needed, m_gridW);
            if (dynamicTier && !casters[i].isStatic)
               m_ortho[i].Fill(dirty, m_gridW);
         }
   }
}

void MarkCubePages(const Vertex3Ds& lightPos, float zNear, const Vertex3Ds& boundsMin, const Vertex3Ds& boundsMax, int faceGrid, int dilate, uint8_t* needed)
{
   for (int f = 0; f < 6; f++)
   {
      // Face selection and uv formulas matching pbr_shadow_cube_lookup in fs_pbr
      int inFront = 0;
      float u0 = FLT_MAX, u1 = -FLT_MAX, v0 = FLT_MAX, v1 = -FLT_MAX;
      for (int c = 0; c < 8; c++)
      {
         const Vertex3Ds corner(c & 1 ? boundsMax.x : boundsMin.x, c & 2 ? boundsMax.y : boundsMin.y, c & 4 ? boundsMax.z : boundsMin.z);
         const float dx = corner.x - lightPos.x, dy = corner.y - lightPos.y, dz = corner.z - lightPos.z;
         float fw, fu, fv;
         switch (f)
         {
         case 0:
            fw = dx;
            fu = -dy / fw;
            fv = dz / fw;
            break;
         case 1:
            fw = -dx;
            fu = dy / fw;
            fv = dz / fw;
            break;
         case 2:
            fw = dy;
            fu = dx / fw;
            fv = dz / fw;
            break;
         case 3:
            fw = -dy;
            fu = -dx / fw;
            fv = dz / fw;
            break;
         case 4:
            fw = dz;
            fu = dx / fw;
            fv = -dy / fw;
            break;
         default:
            fw = -dz;
            fu = -dx / fw;
            fv = -dy / fw;
            break;
         }
         if (fw <= zNear)
            continue;
         inFront++;
         const float su = fu * 0.5f + 0.5f, sv = fv * -0.5f + 0.5f;
         u0 = std::min(u0, su);
         u1 = std::max(u1, su);
         v0 = std::min(v0, sv);
         v1 = std::max(v1, sv);
      }
      if (inFront == 0)
         continue; // caster entirely behind this face
      int x0, x1, y0, y1;
      if (inFront < 8)
      {
         // AABB straddles the face plane: conservatively mark the whole face
         x0 = 0;
         x1 = faceGrid - 1;
         y0 = 0;
         y1 = faceGrid - 1;
      }
      else
      {
         x0 = std::max(0, (int)floorf(u0 * faceGrid) - dilate);
         x1 = std::min(faceGrid - 1, (int)ceilf(u1 * faceGrid) - 1 + dilate);
         y0 = std::max(0, (int)floorf(v0 * faceGrid) - dilate);
         y1 = std::min(faceGrid - 1, (int)ceilf(v1 * faceGrid) - 1 + dilate);
      }
      uint8_t* const face = needed + f * faceGrid * faceGrid;
      for (int y = y0; y <= y1; y++)
         for (int x = x0; x <= x1; x++)
            face[y * faceGrid + x] = 1;
   }
}
}
