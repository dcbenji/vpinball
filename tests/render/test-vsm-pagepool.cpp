// license:GPLv3+

// Headless tests of the VSM bookkeeping (renderer/VSMPagePool): shadow slot policy, per caster coverage cache, page culling, static caster toggles, shadow cache receivers

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "renderer/VSMPagePool.h"

#include "doctest.h"

#include <functional>
#include <random>

TEST_CASE("VSM slot policy: PickSlot")
{
   constexpr int n = 4;
   uint8_t freeSlot[n] = { 0, 0, 0, 0 };
   float parked[n] = { -1.f, -1.f, -1.f, -1.f };

   SUBCASE("a free slot first, lowest index")
   {
      freeSlot[2] = freeSlot[3] = 1;
      parked[0] = 0.5f;
      CHECK(VSM::PickSlot(n, freeSlot, parked, 10.f) == 2);
   }
   SUBCASE("never a lit light's slot")
   {
      CHECK(VSM::PickSlot(n, freeSlot, parked, 1000.f) == -1);
   }
   SUBCASE("the least important parked light, if less important than the taker")
   {
      parked[0] = 5.f;
      parked[1] = 2.f;
      parked[3] = 8.f;
      CHECK(VSM::PickSlot(n, freeSlot, parked, 10.f) == 1);
      CHECK(VSM::PickSlot(n, freeSlot, parked, 3.f) == 1);
      CHECK(VSM::PickSlot(n, freeSlot, parked, 2.f) == -1); // tie: no steal
      CHECK(VSM::PickSlot(n, freeSlot, parked, 1.f) == -1);
   }
   SUBCASE("an unlit light (importance < 0) only takes a free slot")
   {
      parked[1] = 0.f;
      CHECK(VSM::PickSlot(n, freeSlot, parked, -1.f) == -1);
      freeSlot[3] = 1;
      CHECK(VSM::PickSlot(n, freeSlot, parked, -1.f) == 3);
   }
   SUBCASE("importance is intensity x range")
   {
      CHECK(VSM::SlotImportance(2.f, 100.f) == doctest::Approx(200.f));
      CHECK(VSM::SlotImportance(-1.f, 100.f) == 0.f);
      CHECK(VSM::SlotImportance(3.f, 0.f) == doctest::Approx(3.f));
   }
}

// Mirrors the slot pass of Renderer::UpdateVirtualShadowMaps (lit lights by importance keep or pick a slot, parked lights keep
// theirs, one lit swap per second at most, the others release): Ripley's Redux layout, 36 GI lights always lit and 6 flashers,
// 32 slots, 60 frames a second. Modes: 0 flashers fire once then GI on, flashers firing at random; 1 GI on at power-up (the
// usual), flashers at random; 2 GI on at power-up, flashers in shows: groups of 2-3 firing together for ~1 s, resting 15-30 s;
// 3 the same with Flupper-style scripts: a flasher's IntensityScale (in the light's intensity, which the renderer's importance
// leaves out) is 0 between fires and a fraction during them. The simulation ranks by the authored importance (as the renderer),
// so the outcome must not change: flashers keep their slots
enum SlotSim { FLASHERS_FIRST, GI_FIRST, GI_FIRST_SHOWS, GI_FIRST_SHOWS_SCALED };
static void SlotSimulation(const SlotSim mode)
{
   const bool giFirst = mode != FLASHERS_FIRST;
   struct L
   {
      float importance;
      bool lit = false;
      int slot = -1;
   };
   std::vector<L> lights;
   for (int i = 0; i < 36; i++)
      lights.push_back({ 50.f + (float)i }); // GI
   for (int i = 0; i < 6; i++)
      lights.push_back({ 1000.f + (float)i }); // flashers
   constexpr int slots = 32;
   std::mt19937 rng(7);
   int rebuildsAfterWarmup = 0, litLosses = 0, starvedFrames = 0, litSwaps = 0, litSwapsAfterWarmup = 0, flasherLosses = 0, lastSwapFrame = -1;
   std::vector<int> pickFrames;
   bool hadSlot[6] = {};
   // The slot set: the 32 most important lights (here the 6 flashers and the 26 most important GI lights), never taken
   std::vector<float> imp;
   for (const L& l : lights)
      imp.push_back(l.importance);
   const std::vector<uint8_t> inSet = VSM::SlotSetMembers(imp, slots); // the renderer's own slot set function
   uint64_t lastSwap = 0;
   const bool shows = mode == GI_FIRST_SHOWS || mode == GI_FIRST_SHOWS_SCALED;
   const int frames = shows ? 60 * 300 : 3000, warmup = shows ? 60 * 120 : 600;
   int showEnd = 0, nextShow = 60 * 5;
   uint32_t showMask = 0;
   for (int frame = 0; frame < frames; frame++)
   {
      const uint64_t now = (uint64_t)frame * 16667 + 1; // usec() is never 0 (0 = lit in darkSince)
      if (shows && frame == nextShow)
      {
         showMask = 0; // 2 or 3 different flashers
         const int n = 2 + (int)(rng() % 2);
         for (int k = 0; k < n; showMask |= 1u << (rng() % 6))
         {
            k = 0;
            for (uint32_t m = showMask; m; m >>= 1)
               k += (int)(m & 1u);
            if (k >= n)
               break;
         }
         showEnd = frame + 60;
         nextShow = frame + 60 * (15 + (int)(rng() % 16));
      }
      for (size_t i = 0; i < lights.size(); i++)
         lights[i].lit = shows ? (i < 36 ? true : frame < showEnd && ((showMask >> (i - 36)) & 1u) != 0)
                       : giFirst ? (i < 36 ? true : frame >= 10 && (rng() % 100) < 15) // GI on at power-up, flashers later
                                 : (i < 36 ? frame >= 10 : frame < 6 ? (int)i - 36 == frame : (rng() % 100) < 15); // each flasher fires once, then GI on
      std::vector<int> order;
      for (size_t i = 0; i < lights.size(); i++)
         if (lights[i].lit)
            order.push_back((int)i);
      std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return lights[a].importance > lights[b].importance; });
      int holder[slots];
      std::fill(holder, holder + slots, -1);
      for (size_t i = 0; i < lights.size(); i++)
         if (lights[i].slot >= 0)
            holder[lights[i].slot] = (int)i;
      bool used[slots] = {};
      const std::vector<int> litBefore = [&]()
      {
         std::vector<int> v;
         for (size_t i = 0; i < lights.size(); i++)
            if (lights[i].lit && lights[i].slot >= 0)
               v.push_back((int)i);
         return v;
      }();
      for (const int li : order)
      {
         L& l = lights[li];
         if (l.slot >= 0)
         {
            used[l.slot] = true;
            continue;
         }
         uint8_t freeSlot[slots];
         float parked[slots], litImp[slots];
         for (int s = 0; s < slots; s++)
         {
            freeSlot[s] = !used[s] && holder[s] < 0;
            const L* const h = !used[s] && holder[s] >= 0 && !inSet[holder[s]] ? &lights[holder[s]] : nullptr;
            parked[s] = (h && !h->lit) ? h->importance : -1.f;
            litImp[s] = (h && h->lit) ? h->importance : -1.f;
         }
         const bool swapOk = now - lastSwap >= VSM::SLOT_LIT_SWAP_US;
         const int s = VSM::PickSlot(slots, freeSlot, parked, l.importance, swapOk ? litImp : nullptr, inSet[li]);
         if (s < 0)
         {
            starvedFrames++;
            continue;
         }
         if (holder[s] >= 0)
         {
            if (lights[holder[s]].lit)
            {
               lastSwap = now;
               litSwaps++;
               lastSwapFrame = frame;
               litSwapsAfterWarmup += !shows && frame > warmup;
            }
            lights[holder[s]].slot = -1;
         }
         holder[s] = li;
         l.slot = s;
         used[s] = true;
         pickFrames.push_back(frame);
      }
      for (const int li : litBefore)
         litLosses += lights[li].slot < 0;
      for (int i = 36; i < 42; i++) // a flasher that won a slot never loses it, lit or resting
      {
         flasherLosses += hadSlot[i - 36] && lights[i].slot < 0 ? 1 : 0;
         hadSlot[i - 36] = lights[i].slot >= 0 || hadSlot[i - 36];
         if (lights[i].slot < 0)
            hadSlot[i - 36] = false;
      }
   }
   // Warm-up: the fixed time, or in shows until the last flasher's first fire won its slot (a flasher may sit out the first shows)
   for (const int f : pickFrames)
      rebuildsAfterWarmup += f > std::max(shows ? lastSwapFrame : warmup, 0);
   CHECK(flasherLosses == 0);
   // A lit light loses its slot only to a light at least twice as important, one per second: the 6 flashers, once each, when
   // the GI came first (in shows too: a flasher that rests keeps its slot); never when the flashers came first
   CHECK(litLosses == litSwaps);
   CHECK(litSwaps == (giFirst ? 6 : 0));
   // Either way the end state is the same: the flashers hold slots, parked between fires (more important than any GI light),
   // the 10 least important GI lights stay unshadowed, and after the warm-up nothing changes hands (the old policy re-rendered
   // ~32 pages per second here)
   CHECK(litSwapsAfterWarmup == 0);
   CHECK(rebuildsAfterWarmup == 0);
   for (int i = 0; i < 10; i++)
      CHECK(lights[i].slot < 0);
   for (size_t i = 10; i < lights.size(); i++)
      CHECK(lights[i].slot >= 0);
   (void)starvedFrames;
}

TEST_CASE("VSM slot policy: over the cap, no cascade, flashers first") { SlotSimulation(FLASHERS_FIRST); }
TEST_CASE("VSM slot policy: over the cap, no cascade, GI first") { SlotSimulation(GI_FIRST); }
TEST_CASE("VSM slot policy: over the cap, flasher shows with long rests keep their slots") { SlotSimulation(GI_FIRST_SHOWS); }
TEST_CASE("VSM slot policy: over the cap, flashers whose script scales them to 0 between fires keep their slots") { SlotSimulation(GI_FIRST_SHOWS_SCALED); }

// The importance the renderer ranks by: authored intensity x range (Renderer passes the authored intensity, not one scaled by the
// script's IntensityScale, which Flupper-style flasher scripts drive to 0 between fires): a bright wide flasher stays a member of
// the slot set over a dim GI bulb of a similar range
TEST_CASE("VSM slot set ranks by authored intensity x range")
{
   CHECK(VSM::SlotImportance(24.f, 600.f) == doctest::Approx(14400.f));
   const std::vector<float> imp { VSM::SlotImportance(1.f, 360.f), VSM::SlotImportance(24.f, 600.f), VSM::SlotImportance(1.f, 350.f) };
   const std::vector<uint8_t> member = VSM::SlotSetMembers(imp, 2);
   CHECK(member[1] == 1);
   CHECK(member[0] == 1);
   CHECK(member[2] == 0);
}

TEST_CASE("VSM slot policy: lit swap")
{
   SUBCASE("a slot-set member takes a lit holder outside the set even within 2x (review round 2 R2-3)")
   {
      const uint8_t none[2] = { 0, 0 };
      const float noPark[2] = { -1.f, -1.f };
      const float litOut[2] = { 60.f, 55.f };
      CHECK(VSM::PickSlot(2, none, noPark, 70.f, litOut) == -1); // under 2x: a light outside the set does not
      CHECK(VSM::PickSlot(2, none, noPark, 70.f, litOut, true) == 1); // a set member does, the least important holder
   }
   constexpr int n = 3;
   const uint8_t freeSlot[n] = { 0, 0, 0 };
   const float noParked[n] = { -1.f, -1.f, -1.f };
   const float lit[n] = { 40.f, 20.f, 30.f };
   CHECK(VSM::PickSlot(n, freeSlot, noParked, 100.f) == -1); // no lit swap allowed this frame
   CHECK(VSM::PickSlot(n, freeSlot, noParked, 100.f, lit) == 1); // the least important lit holder
   CHECK(VSM::PickSlot(n, freeSlot, noParked, 39.f, lit) == -1); // under 2x: kept
   CHECK(VSM::PickSlot(n, freeSlot, noParked, 40.f, lit) == 1);
   const float parked[n] = { -1.f, -1.f, 0.f }; // a parked light less important than the taker goes before any lit swap
   CHECK(VSM::PickSlot(n, freeSlot, parked, 1.f, lit) == 2);
}

static std::vector<VSM::Caster> RandomCasters(std::mt19937& rng, int count, const Vertex3Ds& around, float spread)
{
   std::uniform_real_distribution<float> pos(-spread, spread), size(1.f, spread * 0.3f);
   std::vector<VSM::Caster> casters(count);
   for (int i = 0; i < count; i++)
   {
      VSM::Caster& c = casters[i];
      c.isStatic = (i % 4) != 0; // every 4th one dynamic
      const Vertex3Ds p(around.x + pos(rng), around.y + pos(rng), std::max(0.f, around.z * 0.5f + pos(rng) * 0.3f));
      const Vertex3Ds h(size(rng), size(rng), size(rng) * 0.5f);
      c.boundsMin = p - h;
      c.boundsMax = p + h;
   }
   return casters;
}

TEST_CASE("VSM coverage cache")
{
   std::mt19937 rng(11);
   const Vertex3Ds lightPos(500.f, 1000.f, 60.f);
   std::vector<VSM::Caster> casters = RandomCasters(rng, 200, lightPos, 300.f);
   constexpr int entries = 6 * VSM::CUBE_FACE_GRID * VSM::CUBE_FACE_GRID;
   VSM::Coverage cov;
   int light = 0;

   // Same pages as marking every caster with MarkCubePages (the per frame marking it replaces)
   const auto reference = [&](bool dynamicTier, std::vector<uint8_t>& needed, std::vector<uint8_t>& dirty)
   {
      needed.assign(entries, 0);
      dirty.assign(entries, 0);
      std::vector<uint8_t> tmp(entries);
      for (const VSM::Caster& c : casters)
      {
         if (!c.active)
            continue;
         std::fill(tmp.begin(), tmp.end(), uint8_t(0));
         VSM::MarkCubePages(lightPos, 2.f, c.boundsMin, c.boundsMax, VSM::CUBE_FACE_GRID, 1, tmp.data());
         for (int i = 0; i < entries; i++)
         {
            needed[i] |= tmp[i];
            if (dynamicTier && !c.isStatic)
               dirty[i] |= tmp[i];
         }
      }
   };
   const auto check = [&](bool dynamicTier)
   {
      std::vector<uint8_t> refNeeded, refDirty, needed(entries, 7), dirty(entries, 0);
      reference(dynamicTier, refNeeded, refDirty);
      cov.Mark(casters, dynamicTier, needed.data(), dirty.data());
      CHECK(needed == refNeeded);
      CHECK(dirty == refDirty);
   };

   CHECK(cov.UpdateCube(&light, lightPos, 2.f, 1, casters) == 200); // first use: every caster marked
   check(false);
   check(true);
   CHECK(cov.UpdateCube(&light, lightPos, 2.f, 1, casters) == 0); // nothing moved: nothing re-marked

   // A dynamic caster moves: only it is re-marked, and the result still matches a full marking
   casters[4].boundsMin.x += 150.f;
   casters[4].boundsMax.x += 150.f;
   casters[4].version++;
   CHECK(cov.UpdateCube(&light, lightPos, 2.f, 1, casters) == 1);
   check(true);

   // A hidden caster (invisible wall) marks nothing, without a re-mark
   casters[5].active = false;
   CHECK(cov.UpdateCube(&light, lightPos, 2.f, 1, casters) == 0);
   check(false);

   // The dirty fresh bit (2) of a page survives the dynamic dirty marking
   {
      std::vector<uint8_t> needed(entries), dirty(entries, 2);
      cov.Mark(casters, true, needed.data(), dirty.data());
      for (int i = 0; i < entries; i++)
         CHECK((dirty[i] & 2) != 0);
   }

   // Key changes re-mark everything: light moved, other light in the slot, caster list rebuilt
   CHECK(cov.UpdateCube(&light, lightPos + Vertex3Ds(1.f, 0.f, 0.f), 2.f, 1, casters) == 200);
   CHECK(cov.UpdateCube(&light, lightPos, 2.f, 1, casters) == 200);
   int other = 0;
   CHECK(cov.UpdateCube(&other, lightPos, 2.f, 1, casters) == 200);
   CHECK(cov.UpdateCube(&other, lightPos, 2.f, 2, casters) == 200);
   check(true);

   // Ortho layout: same pages as MarkOrthoPages
   const Vertex3Ds center(500.f, 1000.f, 0.f);
   const Vertex3Ds keyPos(500.f, 1000.f, 2400.f);
   const Matrix3D view = Matrix3D::MatrixLookAtRH(keyPos, center, Vertex3Ds(0.f, -1.f, 0.f));
   const float l = -700.f, r = 700.f, b = -1300.f, t = 1300.f;
   constexpr int gw = 9, gh = 16;
   CHECK(cov.UpdateOrtho(&light, keyPos, view, l, r, b, t, gw, gh, 2, 2, casters) == 200);
   CHECK(cov.UpdateOrtho(&light, keyPos, view, l, r, b, t, gw, gh, 2, 2, casters) == 0);
   std::vector<uint8_t> refNeeded(gw * gh, 0), refDirty(gw * gh, 0), needed(gw * gh, 5), dirty(gw * gh, 0);
   for (const VSM::Caster& c : casters)
      if (c.active)
      {
         VSM::MarkOrthoPages(view, l, r, b, t, c.boundsMin, c.boundsMax, gw, gh, 2, refNeeded.data());
         if (!c.isStatic)
            VSM::MarkOrthoPages(view, l, r, b, t, c.boundsMin, c.boundsMax, gw, gh, 2, refDirty.data());
      }
   cov.Mark(casters, true, needed.data(), dirty.data());
   CHECK(needed == refNeeded);
   CHECK(dirty == refDirty);
}

// Per page caster culling is only safe if a caster that rasterizes into a page (projected with the page's view and zoomed
// projection, as the page render does) always has that page in its undilated coverage. Checked with point casters.
TEST_CASE("VSM page culling matches the page projections")
{
   std::mt19937 rng(23);
   std::uniform_real_distribution<float> d(-400.f, 400.f);
   const float zNear = 2.f, zFar = 500.f;
   const Vertex3Ds lightPos(500.f, 1000.f, 60.f);
   VSM::Coverage cov;
   int light = 0;
   std::vector<VSM::Caster> casters(1);
   int insideCube = 0;
   for (int n = 0; n < 4000; n++)
   {
      const Vertex3Ds p(lightPos.x + d(rng), lightPos.y + d(rng), lightPos.z + d(rng) * 0.3f);
      casters[0].boundsMin = p - Vertex3Ds(0.01f, 0.01f, 0.01f);
      casters[0].boundsMax = p + Vertex3Ds(0.01f, 0.01f, 0.01f);
      casters[0].version++;
      cov.UpdateCube(&light, lightPos, zNear, 1, casters);
      for (int face = 0; face < 6; face++)
      {
         const Matrix3D view = Matrix3D::MatrixLookAtRH(lightPos, lightPos + VSM::CUBE_FACE_FWD[face], VSM::CUBE_FACE_UP[face]);
         const Vertex3Ds v = view.MultiplyVectorNoPerspective(p);
         if (-v.z <= zNear * 1.01f)
            continue; // behind the face's near plane
         for (int py = 0; py < VSM::CUBE_FACE_GRID; py++)
            for (int px = 0; px < VSM::CUBE_FACE_GRID; px++)
            {
               const Matrix3D vp = view * Matrix3D::MatrixPerspectiveFovRH(90.f, 1.f, zNear, zFar) * VSM::PageZoomMatrix(px, py, VSM::CUBE_FACE_GRID, VSM::CUBE_FACE_GRID);
               const Vertex3Ds ndc = vp * p;
               if (fabsf(ndc.x) < 0.98f && fabsf(ndc.y) < 0.98f)
               {
                  insideCube++;
                  const int vpage = face * VSM::CUBE_FACE_GRID * VSM::CUBE_FACE_GRID + py * VSM::CUBE_FACE_GRID + px;
                  CHECK(cov.InPage(0, vpage));
               }
            }
      }
   }
   CHECK(insideCube > 3000); // nearly every point lands in some page

   // Ortho (light above the glass): view * fitted ortho box * page zoom
   const Vertex3Ds keyPos(500.f, 1000.f, 2400.f);
   const Matrix3D view = Matrix3D::MatrixLookAtRH(keyPos, Vertex3Ds(500.f, 1000.f, 0.f), Vertex3Ds(0.f, -1.f, 0.f));
   const float l = -700.f, r = 700.f, b = -1300.f, t = 1300.f;
   constexpr int gw = 9, gh = 16;
   const Matrix3D proj = Matrix3D::MatrixOrthoOffCenterRH(l, r, b, t, -100.f, -3000.f);
   int insideOrtho = 0;
   for (int n = 0; n < 2000; n++)
   {
      const Vertex3Ds p(500.f + d(rng) * 1.7f, 1000.f + d(rng) * 3.f, fabsf(d(rng)) * 0.5f);
      casters[0].boundsMin = p - Vertex3Ds(0.01f, 0.01f, 0.01f);
      casters[0].boundsMax = p + Vertex3Ds(0.01f, 0.01f, 0.01f);
      casters[0].version++;
      cov.UpdateOrtho(&light, keyPos, view, l, r, b, t, gw, gh, 2, 1, casters);
      for (int py = 0; py < gh; py++)
         for (int px = 0; px < gw; px++)
         {
            const Vertex3Ds ndc = (view * proj * VSM::PageZoomMatrix(px, py, gw, gh)) * p;
            if (fabsf(ndc.x) < 0.98f && fabsf(ndc.y) < 0.98f)
            {
               insideOrtho++;
               CHECK(cov.InPage(0, py * gw + px));
            }
         }
   }
   CHECK(insideOrtho > 1000);

   // A caster without bounds is drawn into every page
   casters[0].boundsMin = casters[0].boundsMax = Vertex3Ds(0.f, 0.f, 0.f);
   casters[0].noBounds = true;
   casters[0].version++;
   cov.UpdateCube(&light, lightPos, zNear, 1, casters);
   for (int vpage = 0; vpage < 24; vpage++)
      CHECK(cov.InPage(0, vpage));
}

// The per light VSM params live in the light row of the page table texture (fs_pbr pbr_vsm_light), no longer in uniform arrays:
// the layout fs_pbr reads, the row vector product it evaluates (r0 * x + r1 * y + r2 * z + r3 = the CPU's view * proj applied to
// p), and the change detection that gates the upload
TEST_CASE("VSM light row: layout, ortho projection, upload only on change")
{
   std::mt19937 rng(7);
   std::uniform_real_distribution<float> u(-2.f, 2.f);
   std::vector<Matrix3D> vp(VSM::MAX_LIGHTS);
   std::vector<vec4> params(VSM::MAX_LIGHTS), box(VSM::MAX_LIGHTS), cache(VSM::MAX_LIGHTS);
   for (int s = 0; s < VSM::MAX_LIGHTS; s++)
   {
      for (int r = 0; r < 4; r++)
         for (int c = 0; c < 4; c++)
            vp[s].m[r][c] = u(rng);
      params[s] = vec4((float)(s * VSM::PAGETABLE_STRIDE), (float)(1 + s % VSM::MAX_ORTHO_GRID), (float)(VSM::MAX_ORTHO_GRID - s % 7), u(rng));
      box[s] = vec4(u(rng), u(rng), u(rng), 0.f);
      cache[s] = vec4(u(rng), u(rng), u(rng), (float)(s % 3));
   }
   std::vector<float> row;
   REQUIRE(VSM::PackLightRow(row, vp.data(), params.data(), box.data(), cache.data()));
   REQUIRE(row.size() == (size_t)VSM::PAGETABLE_W * 4);
   const auto texel = [&row](int s, int k) { const float* t = row.data() + (s * VSM::LIGHT_ROW_TEXELS + k) * 4; return vec4(t[0], t[1], t[2], t[3]); };
   for (int s = 0; s < VSM::MAX_LIGHTS; s++)
   {
      for (int i = 0; i < 8; i++)
      {
         const Vertex3Ds p(u(rng) * 500.f, u(rng) * 500.f, u(rng) * 100.f);
         const vec4 c0 = texel(s, 0), c1 = texel(s, 1), c2 = texel(s, 2);
         const auto dot1 = [&p](const vec4& c) { return c.x * p.x + c.y * p.y + c.z * p.z + c.w; };
         const Vertex3Ds sc(dot1(c0), dot1(c1), dot1(c2));
         const Vertex3Ds ref = vp[s].MultiplyVectorNoPerspective(p);
         CHECK(sc.x == doctest::Approx(ref.x).epsilon(1e-5));
         CHECK(sc.y == doctest::Approx(ref.y).epsilon(1e-5));
         CHECK(sc.z == doctest::Approx(ref.z).epsilon(1e-5));
      }
      // fs_pbr's unpacking of the ortho grid (floor(x / 128), x - 128 * that) gives the grid back exactly
      const float gx = texel(s, 3).x, gw = floorf(gx * (1.f / 128.f));
      CHECK(gw == params[s].y);
      CHECK(gx - gw * 128.f == params[s].z);
      CHECK(texel(s, 3).y == params[s].w);
      CHECK(texel(s, 3).z == box[s].x);
      CHECK(texel(s, 3).w == box[s].y);
      CHECK(texel(s, 4).z == cache[s].z);
      CHECK(texel(s, 4).w == cache[s].w);
      CHECK(texel(s, 5).x == 0.f);
   }
   // unchanged -> no upload; one changed entry -> upload
   CHECK_FALSE(VSM::PackLightRow(row, vp.data(), params.data(), box.data(), cache.data()));
   cache[VSM::MAX_LIGHTS - 1].w = 5.f;
   CHECK(VSM::PackLightRow(row, vp.data(), params.data(), box.data(), cache.data()));
   CHECK(texel(VSM::MAX_LIGHTS - 1, 4).w == 5.f);
   CHECK(VSM::PAGETABLE_ROWS == VSM::MAX_LIGHTS * VSM::PAGETABLE_STRIDE / VSM::PAGETABLE_W);
   static_assert(VSM::MAX_LIGHTS <= 64, "Renderer::m_vsmPageTableDirtySlots is a 64 bit slot mask");
   static_assert(VSM::PAGETABLE_STRIDE % VSM::PAGETABLE_W == 0, "a slot's page table entries are whole rows (partial uploads)");
}

// 64 shadow slots: the slot index + 1 is packed in 8 bits of the light record (fs_pbr: (shadowInfo >> 4) & 255) and the page
// table texture is 512 texels wide, one 4096 texel stride per slot (fs_pbr: ivec2(idx & 511, idx >> 9))
TEST_CASE("VSM 64 slots fit the light record and the page table")
{
   CHECK(VSM::MAX_LIGHTS == 64);
   CHECK(VSM::MAX_LIGHTS + 1 <= 255);
   const int rows = VSM::MAX_LIGHTS * VSM::PAGETABLE_STRIDE / VSM::PAGETABLE_W;
   CHECK(rows == 512);
   const int lastIdx = (VSM::MAX_LIGHTS - 1) * VSM::PAGETABLE_STRIDE + VSM::PAGETABLE_STRIDE - 1;
   CHECK((lastIdx >> 9) < rows);
   // light record w (a float), decoded as fs_pbr does (int(w), exact: w < 2^24 since the penumbra code is <= 255): every slot,
   // mode and layout survives with every penumbra code. The former decode int(floor(w + 0.5)) read an odd mode back one higher
   // once w >= 2^23 (code >= 128): w + 0.5 rounds to even in fp32
   for (int code = 0; code <= 255; code++)
      for (const int slot : { 0, VSM::MAX_LIGHTS - 1 })
         for (int mode = 0; mode <= 4; mode++)
         {
            const int shadowInfo = mode | ((slot + 1) << 4) | (VSM::LAYOUT_CUBE << 12) | (code << 16);
            const float w = (float)shadowInfo;
            REQUIRE((double)w == (double)shadowInfo);
            const int back = (int)w;
            CHECK(((back >> 4) & 255) - 1 == slot);
            CHECK((back & 15) == mode);
            CHECK(((back >> 12) & 15) == VSM::LAYOUT_CUBE);
            CHECK((back >> 16) == code);
         }
   volatile float half = 0.5f; // the old decode, on a case it got wrong (code 128, mode 1)
   const int odd = 1 | (1 << 4) | (VSM::LAYOUT_CUBE << 12) | (128 << 16);
   CHECK(((int)floorf((float)odd + half) & 15) == 2);
}

// The static shadow cache bakes over and is read by the board surface only: playfield space, shown in the player mode, flat on the
// table and covering at least half of it (the playfield mesh always)
TEST_CASE("VSM static shadow cache receivers: the board only")
{
   const float table[4] = { 0.f, 0.f, 952.f, 2116.f };
   const Vertex3Ds boardLo(0.f, 0.f, -0.5f), boardHi(952.f, 2116.f, 0.5f);
   // The board (Ripley's Redux: the playfield art primitive), static and flat over the table
   CHECK(VSM::IsShadowCacheReceiver(false, true, true, true, table, boardLo, boardHi));
   // A VR room floor: flat at z 0 and much larger than the table, in a Room part group (room space), shown in VR only
   const Vertex3Ds floorLo(-10000.f, -10000.f, 0.f), floorHi(10000.f, 10000.f, 0.f);
   CHECK_FALSE(VSM::IsShadowCacheReceiver(false, true, false, true, table, floorLo, floorHi)); // room space, VR mode
   CHECK_FALSE(VSM::IsShadowCacheReceiver(false, true, false, false, table, floorLo, floorHi)); // room space, desktop mode
   // Even in playfield space, the mode's visibility mask hides it on desktop
   CHECK_FALSE(VSM::IsShadowCacheReceiver(false, true, true, false, table, floorLo, floorHi));
   // A playfield space floor shown in the mode lies over the whole table once clipped: it qualifies as a board, its bounds no
   // longer matter (the tiles cover the table rect, Renderer::UpdatePBRShadowCache)
   CHECK(VSM::IsShadowCacheReceiver(false, true, true, true, table, floorLo, floorHi));
   // The half-table test applies to the clipped bounds: a huge primitive barely overlapping the table is not the board
   CHECK_FALSE(VSM::IsShadowCacheReceiver(false, true, true, true, table, Vertex3Ds(-10000.f, 1900.f, 0.f), Vertex3Ds(10000.f, 10000.f, 0.f)));
   // Cabinet space parts are not on the board
   CHECK_FALSE(VSM::IsShadowCacheReceiver(false, true, false, true, table, boardLo, boardHi));
   // Not flat, not static, under half of the table
   CHECK_FALSE(VSM::IsShadowCacheReceiver(false, true, true, true, table, boardLo, Vertex3Ds(952.f, 2116.f, 30.f)));
   CHECK_FALSE(VSM::IsShadowCacheReceiver(false, false, true, true, table, boardLo, boardHi));
   CHECK_FALSE(VSM::IsShadowCacheReceiver(false, true, true, true, table, boardLo, Vertex3Ds(952.f, 1000.f, 0.5f)));
   // The playfield mesh counts static or not, but not outside playfield space or hidden in the mode
   CHECK(VSM::IsShadowCacheReceiver(true, false, true, true, table, boardLo, boardHi));
   CHECK_FALSE(VSM::IsShadowCacheReceiver(true, false, true, false, table, boardLo, boardHi));
}

// A script toggling a static caster re-renders only the pages it covered and covers, in every map: no table wide blink
TEST_CASE("VSM static caster toggled every frame: only its pages re-render, shadows elsewhere stay")
{
   // Two static casters on opposite sides of the light; a script toggles A every frame (Caster::visSerial + version, as the
   // renderer does). Each frame: Mark, then the dirty pages 'render' (cleared). B's pages must never be dirtied (they stay
   // resident and visible), A's are, and Mark reports the static change (the light's cache bake is stale)
   const auto box = [](float x, float y, float z, float s)
   {
      VSM::Caster c;
      c.isStatic = true;
      c.boundsMin = Vertex3Ds(x - s, y - s, z);
      c.boundsMax = Vertex3Ds(x + s, y + s, z + 30.f);
      return c;
   };
   const Vertex3Ds lightPos(500.f, 1000.f, 60.f);
   std::vector<VSM::Caster> casters { box(900.f, 1000.f, 0.f, 10.f), box(100.f, 1000.f, 0.f, 10.f) }; // A (+x), B (-x)
   int light = 0;
   const auto run = [&](VSM::Coverage& cov, int entries, const std::function<void()>& update)
   {
      std::vector<uint8_t> needed(entries), dirty(entries, 0), pagesA(entries, 0), pagesB(entries, 0);
      update();
      CHECK_FALSE(cov.Mark(casters, true, needed.data(), dirty.data())); // first marking: no static change
      // Each caster's own pages (its coverage alone)
      std::vector<VSM::Caster> only = casters;
      only[1].active = false;
      cov.Mark(only, false, pagesA.data(), dirty.data());
      only[0].active = false;
      only[1].active = true;
      cov.Mark(only, false, pagesB.data(), dirty.data());
      int bOnly = 0;
      for (int vp = 0; vp < entries; vp++)
         bOnly += pagesB[vp] && !pagesA[vp];
      REQUIRE(bOnly > 0);
      for (int frame = 0; frame < 60; frame++)
      {
         casters[0].shown ^= 1;
         casters[0].visSerial++;
         casters[0].version++;
         update();
         std::fill(dirty.begin(), dirty.end(), uint8_t(0)); // last frame's dirty pages rendered
         CHECK(cov.Mark(casters, false, needed.data(), dirty.data()));
         int dirtyA = 0;
         for (int vp = 0; vp < entries; vp++)
         {
            if (pagesB[vp] && !pagesA[vp])
            {
               CHECK(dirty[vp] == 0); // B's shadow stays
               CHECK(needed[vp] == 1);
            }
            dirtyA += dirty[vp] != 0;
            if (dirty[vp])
               CHECK(pagesA[vp] == 1); // only A's pages (dilated coverage) re-render
         }
         CHECK(dirtyA > 0);
      }
      // A frame without a toggle: nothing static dirtied
      update();
      std::fill(dirty.begin(), dirty.end(), uint8_t(0));
      CHECK_FALSE(cov.Mark(casters, false, needed.data(), dirty.data()));
      CHECK(std::count(dirty.begin(), dirty.end(), uint8_t(0)) == entries);
   };

   SUBCASE("cube")
   {
      VSM::Coverage cov;
      run(cov, 6 * VSM::CUBE_FACE_GRID * VSM::CUBE_FACE_GRID, [&]() { cov.UpdateCube(&light, lightPos, 2.f, 1, casters); });
   }
   SUBCASE("ortho")
   {
      VSM::Coverage cov;
      const Vertex3Ds keyPos(500.f, 1000.f, 2400.f);
      const Matrix3D view = Matrix3D::MatrixLookAtRH(keyPos, Vertex3Ds(500.f, 1000.f, 0.f), Vertex3Ds(0.f, -1.f, 0.f));
      constexpr int gw = 9, gh = 16;
      run(cov, gw * gh, [&]() { cov.UpdateOrtho(&light, keyPos, view, -700.f, 700.f, -1300.f, 1300.f, gw, gh, 1, 1, casters); });
   }
}
