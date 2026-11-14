// license:GPLv3+

// Headless tests of the VSM bookkeeping (renderer/VSMPagePool): shadow slot policy

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
