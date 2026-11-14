// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "core/VPApp.h"
#include "parts/light.h"
#include "parts/Material.h"
#include "parts/pintable.h"
#include "parts/primitive.h"
#include "parts/surface.h"
#include "parts/flipper.h"
#include "renderer/Renderer.h"

#include "doctest.h"

#include <filesystem>

// Builds a table exercising the PBR orthographic shadow map path: the playfield has a PBR material
// (the shadow receiver), one static and one dynamic-tier shadowed PBR light high above the table
// (ortho layout), plus static (wall) and dynamic (flipper) casters inside their footprint.
// With translucentCaster, a red translucent PBR plastic plate floats between the lights and the
// playfield: the receivers behind it get the transmitted (red) light instead of a plain shadow.
static std::filesystem::path CreatePBRShadowTable(const bool translucentCaster = false)
{
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(GetAssetPath() / "test000-default-table.vpx", feedback)));
   const float cx = (table->m_left + table->m_right) * 0.5f;
   const float cy = (table->m_top + table->m_bottom) * 0.5f;

   Material* const pfMat = new Material();
   pfMat->m_name = "pbr_playfield";
   pfMat->m_type = Material::MaterialType::PBR;
   pfMat->m_fRoughness = 0.5f;
   pfMat->m_fMetallic = 0.f;
   pfMat->m_fOcclusion = 1.f;
   pfMat->m_fEmissionIntensity = 0.f;
   pfMat->m_cBase = RGB(230, 230, 230);
   pfMat->m_bOpacityActive = false;
   table->AddMaterial(pfMat);
   table->m_playfieldMaterial = pfMat->m_name;

   // Static caster: a wall in the middle of the light footprint
   Surface* const wall = Surface::COMCreate();
   wall->Init(cx, cy, false);
   wall->SetName("caster_wall"s);
   wall->m_d.m_heighttop = 100.f;
   table->AddPart(wall);
   wall->Release();

   // Dynamic caster
   Flipper* const flipper = Flipper::COMCreate();
   flipper->Init(cx - 150.f, cy + 300.f, false);
   flipper->SetName("caster_flipper"s);
   table->AddPart(flipper);
   flipper->Release();

   // Two shadowed PBR lights far above the glass top => ortho shadow layout, one static tier, one static+dynamic tier
   for (int i = 0; i < 2; i++)
   {
      Light* const light = Light::COMCreate();
      light->Init(cx + (i == 0 ? -200.f : 200.f), cy - 400.f, false);
      light->SetName("pbr_shadow_light_" + std::to_string(i));
      light->m_d.m_type = LightType::PBR;
      light->m_d.m_state = 1.f;
      light->m_d.m_intensity = 5.f;
      light->m_d.m_falloff = 3000.f;
      light->m_d.m_falloff_power = 2.f;
      light->m_d.m_height = 500.f;
      light->m_d.m_color = i == 0 ? RGB(255, 220, 180) : RGB(200, 220, 255);
      light->m_d.m_showBulbMesh = false;
      light->m_d.m_visible = true;
      light->m_d.m_shadows = i == 0 ? ShadowMode::PBR_STATIC_SHADOWS : ShadowMode::PBR_STATIC_N_DYNAMIC_SHADOWS;
      table->AddPart(light);
      light->Release();
   }

   // A near light under the glass top: takes the cube layout (6 face layers in the near array).
   // Placed close to the wall so its shadow is unambiguous: light at (cx+150, cy), wall at (cx, cy)
   // should project the wall's shadow toward -X/+spread on the playfield.
   {
      Light* const light = Light::COMCreate();
      light->Init(cx + 200.f, cy - 100.f, false);
      light->SetName("pbr_shadow_light_near"s);
      light->m_d.m_type = LightType::PBR;
      light->m_d.m_state = 1.f;
      light->m_d.m_intensity = 12.f;
      light->m_d.m_falloff = 500.f;
      light->m_d.m_falloff_power = 2.f;
      light->m_d.m_height = 60.f;
      light->m_d.m_color = RGB(255, 255, 255);
      light->m_d.m_showBulbMesh = false;
      light->m_d.m_visible = true;
      light->m_d.m_shadows = ShadowMode::PBR_STATIC_SHADOWS;
      table->AddPart(light);
      light->Release();
   }

   // Translucent caster: a red PBR plastic plate floating between the lights (ortho at z=500, near
   // light at z=60) and the playfield. Wrap lighting is the material's thin-surface transmission.
   if (translucentCaster)
   {
      Material* const plasticMat = new Material();
      plasticMat->m_name = "pbr_plastic_red";
      plasticMat->m_type = Material::MaterialType::PBR;
      plasticMat->m_fRoughness = 0.4f;
      plasticMat->m_cBase = RGB(255, 40, 30);
      plasticMat->m_cRefractionTint = RGB(255, 60, 40);
      plasticMat->m_fThickness = 5.f;
      plasticMat->m_bOpacityActive = true;
      plasticMat->m_fOpacity = 0.65f;
      plasticMat->m_fWrapLighting = 1.0f;
      table->AddMaterial(plasticMat);

      Primitive* const plastic = Primitive::COMCreate();
      plastic->Init(cx + 80.f, cy - 200.f, false);
      plastic->SetName("caster_plastic"s);
      plastic->m_d.m_vPosition.z = 120.f;
      plastic->m_d.m_vSize = Vertex3Ds(400.f, 300.f, 12.f);
      plastic->m_d.m_szMaterial = plasticMat->m_name;
      plastic->m_d.m_visible = true;
      plastic->m_d.m_staticRendering = true;
      table->AddPart(plastic);
      plastic->Release();
   }

   const std::filesystem::path file = GetTestTmpDir() / (translucentCaster ? "pbr-translucency-table.vpx" : "pbr-shadow-table.vpx");
   table->m_filename = file;
   TestFileFeedback saveFeedback;
   REQUIRE(SUCCEEDED(table->Save(saveFeedback)));
   CHECK(std::filesystem::exists(file));
   table->Release();
   return file;
}

TEST_CASE("PBR shadow map render [render]")
{
   ResetVPX();
   const std::filesystem::path tableFile = CreatePBRShadowTable();

#if defined(ENABLE_BGFX)
   g_settingsService.GetAppSettings().SetPlayer_GfxBackend("Direct3D11"s, false);
   g_settingsService.GetAppSettings().SetPlayer_DepthPrepass(2, false);
   CaptureRender(tableFile.string(), "pbr-shadow-Test.webp"s);
   CHECK(std::filesystem::exists(GetAssetPath() / "pbr-shadow-Test.webp"));
#endif

   ResetVPX();
}

// A red translucent plastic plate sits between the shadowed lights and the playfield: the light it
// transmits reaches the playfield tinted red (coloured transmittance shadow, ortho + cube layouts)
// while the plate itself glows from the near light behind it (thin surface translucency).
TEST_CASE("PBR transmittance shadow render [render]")
{
   ResetVPX();
   const std::filesystem::path tableFile = CreatePBRShadowTable(true);

#if defined(ENABLE_BGFX)
   g_settingsService.GetAppSettings().SetPlayer_GfxBackend("Direct3D11"s, false);
   g_settingsService.GetAppSettings().SetPlayer_DepthPrepass(2, false);
   CaptureRender(tableFile.string(), "pbr-translucency-Test.webp"s);
   CHECK(std::filesystem::exists(GetAssetPath() / "pbr-translucency-Test.webp"));
#endif

   ResetVPX();
}

// Same render, but the near light is moved mid-play: its baked shadow map must be invalidated and
// re-rendered, keeping the wall shadow anchored to the wall instead of following the light.
TEST_CASE("PBR shadow map follows light move [render]")
{
   ResetVPX();
   const std::filesystem::path tableFile = CreatePBRShadowTable();

#if defined(ENABLE_BGFX)
   g_settingsService.GetAppSettings().SetPlayer_GfxBackend("Direct3D11"s, false);
   g_settingsService.GetAppSettings().SetPlayer_DepthPrepass(2, false);

   struct MoveState
   {
      std::filesystem::path tmpScreenshotPath;
      bool done;
      bool captureRequested;
      bool moved;
   } state = { GetAssetPath() / "pbr-shadow-move-Test.webp"s, false, false, false };
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      MoveState* state = reinterpret_cast<MoveState*>(context);
      if (state->captureRequested || g_pplayer->m_overall_frames < 25)
         return;
      if (!state->moved)
      {
         // Move the near light once the first frames rendered (its shadow map was baked at the old spot)
         for (auto part : g_pplayer->m_ptable->GetParts())
         {
            if (part->GetItemType() != eItemLight)
               continue;
            Light* const light = static_cast<Light*>(part);
            if (light->GetName() == "pbr_shadow_light_near"s)
            {
               light->m_d.m_vCenter.y += 300.f;
               state->moved = true;
            }
         }
         return; // Give a few frames for the re-bake to land before capturing
      }
      if (g_pplayer->m_overall_frames < 40)
         return;
      if (g_pplayer->m_renderer->IsTemporalAccumulationInProgress() && g_pplayer->m_overall_frames < 512)
         return;
      state->captureRequested = true;
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { state->tmpScreenshotPath },
         [state](bool success)
         {
            g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
            state->done = true;
         });
   };

   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(tableFile, feedback)));
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   const unsigned int onPrepareFrameMsgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), onPrepareFrameMsgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(onPrepareFrameMsgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(onPrepareFrameMsgId);
   player = nullptr;
   table->Release();
   CHECK(state.done);
   CHECK(std::filesystem::exists(GetAssetPath() / "pbr-shadow-move-Test.webp"s));
#endif

   ResetVPX();
}

#if defined(ENABLE_BGFX)
// The shadow table with every shadowed light on a static tier: no dynamic caster re-dirties pages every frame, so the maps complete
// and the static shadow cache bakes them (the dynamic tier ortho light of the table keeps the page budget busy)
static void MakeShadowLightsStatic(PinTable* const table)
{
   for (IEditable* const part : table->GetParts())
      if (part->GetItemType() == eItemLight && static_cast<Light*>(part)->m_d.m_shadows == ShadowMode::PBR_STATIC_N_DYNAMIC_SHADOWS)
         static_cast<Light*>(part)->m_d.m_shadows = ShadowMode::PBR_STATIC_SHADOWS;
}

// Plays the shadow table with static shadowed lights and captures frame 240, with the static shadow cache on or off (or with no
// shadowed light at all, the unshadowed reference of the same frame). Returns the number of cache bakes submitted by then
static uint64_t CaptureStaticShadows(const std::filesystem::path& tableFile, const bool cacheOn, const string& shot, const bool noShadows = false)
{
   g_settingsService.GetAppSettings().SetPlayer_GfxBackend("Direct3D11"s, false);
   g_settingsService.GetAppSettings().SetPlayer_DepthPrepass(2, false);
   g_settingsService.GetAppSettings().SetPlayer_PBRShadowCache(cacheOn, false);
   struct CaptureState
   {
      std::filesystem::path path;
      bool requested = false, done = false;
      uint64_t bakes = 0;
   } state { GetAssetPath() / shot };
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      CaptureState* state = reinterpret_cast<CaptureState*>(context);
      if (state->requested || g_pplayer->m_overall_frames < 240)
         return;
      // The static parts are prerendered again after shadow work (pages, bakes): capture the accumulated image, not a partial one
      if (g_pplayer->m_renderer->IsTemporalAccumulationInProgress() && g_pplayer->m_overall_frames < 512)
         return;
      state->requested = true;
      state->bakes = g_pplayer->m_renderer->m_pbrShadowCacheBakes;
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { state->path },
         [state](bool success)
         {
            g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
            state->done = success;
         });
   };
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(tableFile, feedback)));
   MakeShadowLightsStatic(table);
   if (noShadows)
      for (IEditable* const part : table->GetParts())
         if (part->GetItemType() == eItemLight)
            static_cast<Light*>(part)->m_d.m_shadows = ShadowMode::NONE;
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   const unsigned int onPrepareFrameMsgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), onPrepareFrameMsgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(onPrepareFrameMsgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(onPrepareFrameMsgId);
   player = nullptr;
   table->Release();
   CHECK(state.done);
   return state.bakes;
}

// Cache on against cache off, and both against the same frame without shadows. The shadowed pixels are those of the live render
// (cache off) at least 16 luminance levels darker than without shadows: the cache must darken them as much (the sums of the
// darkening over them), so a missing or empty bake fails however small its share of the frame. Elsewhere only capture noise
// and the unrotated kernel of the bakes may differ: the mean and the share of pixels more than 24 levels apart are bounded.
struct ShadowCaptureCompare
{
   double meanDif = 0.; // mean absolute difference on vs off, levels per channel
   int maxDif = 0;
   double farPct = 0.; // % of pixels with a channel more than 24 levels apart, on vs off
   uint64_t shadowPx = 0; // shadowed pixels of the live render
   double darkOn = 0., darkOff = 0.; // summed luminance darkening of the shadowed pixels, cache on and off
};
static ShadowCaptureCompare CompareShadowCaptures(const string& onShot, const string& offShot, const string& noneShot)
{
   ShadowCaptureCompare r;
   const std::shared_ptr<BaseTexture> on = BaseTexture::CreateFromFile(GetAssetPath() / onShot);
   const std::shared_ptr<BaseTexture> off = BaseTexture::CreateFromFile(GetAssetPath() / offShot);
   const std::shared_ptr<BaseTexture> none = BaseTexture::CreateFromFile(GetAssetPath() / noneShot);
   REQUIRE(on != nullptr);
   REQUIRE(off != nullptr);
   REQUIRE(none != nullptr);
   REQUIRE(on->width() == off->width());
   REQUIRE(on->height() == off->height());
   REQUIRE(none->width() == off->width());
   REQUIRE(none->height() == off->height());
   const unsigned int bppOn = on->pitch() / on->width(), bppOff = off->pitch() / off->width(), bppNone = none->pitch() / none->width();
   const uint8_t* const a = static_cast<const uint8_t*>(on->datac());
   const uint8_t* const b = static_cast<const uint8_t*>(off->datac());
   const uint8_t* const n = static_cast<const uint8_t*>(none->datac());
   const auto lum = [](const uint8_t* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; };
   double sum = 0.;
   uint64_t far = 0;
   for (unsigned int y = 0; y < on->height(); y++)
      for (unsigned int x = 0; x < on->width(); x++)
      {
         const uint8_t* const pa = a + y * on->pitch() + x * bppOn;
         const uint8_t* const pb = b + y * off->pitch() + x * bppOff;
         const uint8_t* const pn = n + y * none->pitch() + x * bppNone;
         int pxMax = 0;
         for (int c = 0; c < 3; c++)
         {
            const int d = abs((int)pa[c] - (int)pb[c]);
            sum += d;
            pxMax = max(pxMax, d);
         }
         r.maxDif = max(r.maxDif, pxMax);
         if (pxMax > 24)
            far++;
         const double dOff = lum(pn) - lum(pb);
         if (dOff > 16.)
         {
            r.shadowPx++;
            r.darkOff += dOff;
            r.darkOn += lum(pn) - lum(pa);
         }
      }
   const double px = (double)on->width() * (double)on->height();
   r.meanDif = sum / (px * 3.);
   r.farPct = 100. * (double)far / px;
   return r;
}
#endif

// The static shadow cache bakes the playfield shadow of a static light once its map is complete, and the playfield then reads the
// bake instead of the live filter: the picture must not change (the bake has no per pixel kernel rotation, hence the small margin),
// and the shadowed pixels must be as dark with the cache as without it
TEST_CASE("PBR static shadow cache bakes the live shadow [render]")
{
   ResetVPX();
   const std::filesystem::path tableFile = CreatePBRShadowTable();

#if defined(ENABLE_BGFX)
   const uint64_t bakesOn = CaptureStaticShadows(tableFile, true, "pbr-shadow-cache-on-Test.webp"s);
   ResetVPX();
   const uint64_t bakesOff = CaptureStaticShadows(tableFile, false, "pbr-shadow-cache-off-Test.webp"s);
   ResetVPX();
   CaptureStaticShadows(tableFile, false, "pbr-shadow-cache-none-Test.webp"s, true);
   ResetVPX();
   CHECK(bakesOn >= 1);
   CHECK(bakesOff == 0);
   const ShadowCaptureCompare r = CompareShadowCaptures("pbr-shadow-cache-on-Test.webp"s, "pbr-shadow-cache-off-Test.webp"s, "pbr-shadow-cache-none-Test.webp"s);
   MESSAGE("static shadow cache: " << bakesOn << " bake(s), cache on vs off mean abs " << r.meanDif << ", max " << r.maxDif << " levels, " << r.farPct
                                   << "% of pixels > 24 apart; " << r.shadowPx << " shadowed pixels, darkening on/off " << r.darkOn / max(r.darkOff, 1.));
   CHECK(r.meanDif <= 0.5);
   CHECK(r.farPct <= 0.1);
   CHECK(r.shadowPx >= 1000);
   CHECK(r.darkOn >= 0.8 * r.darkOff);
   CHECK(r.darkOn <= 1.2 * r.darkOff);
#endif

   ResetVPX();
}

#if defined(ENABLE_BGFX)
// Plays the shadow table (static shadowed lights, so the static shadow cache bakes) with the primary view plus extraViews editor views
// on the playfield window. Counts over 200 frames how many times the scene level PBR work (shadow map pages, static shadow cache, light
// list) ran, and how many cache bakes were submitted by then
static void CountPBRFrameBuilds(const std::filesystem::path& tableFile, const int extraViews, uint64_t& frames, uint64_t& builds, uint64_t& viewPasses, uint64_t& bakes)
{
   struct CountState
   {
      uint64_t frame0 = 0, builds0 = 0, passes0 = 0, frames = 0, builds = 0, passes = 0, bakes = 0;
      bool started = false, done = false;
   } state;
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      CountState* state = reinterpret_cast<CountState*>(context);
      if (state->done || g_pplayer->m_overall_frames < 30)
         return;
      if (!state->started)
      {
         state->started = true;
         state->frame0 = g_pplayer->m_overall_frames;
         state->builds0 = g_pplayer->m_renderer->m_pbrFrameBuilds;
         state->passes0 = g_pplayer->m_renderer->m_pbrViewPasses;
         return;
      }
      if (g_pplayer->m_overall_frames - state->frame0 < 200)
         return;
      state->frames = g_pplayer->m_overall_frames - state->frame0;
      state->builds = g_pplayer->m_renderer->m_pbrFrameBuilds - state->builds0;
      state->passes = g_pplayer->m_renderer->m_pbrViewPasses - state->passes0;
      state->bakes = g_pplayer->m_renderer->m_pbrShadowCacheBakes;
      state->done = true;
      g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
   };

   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(tableFile, feedback)));
   MakeShadowLightsStatic(table);
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   for (int i = 0; i < extraViews; i++)
   {
      RenderView* const view = player->m_renderer->CreateEditorView();
      view->m_outputWnd = player->m_playfieldWnd;
      view->SetOutputArea(vec4(0.5f, 0.5f * (float)i, 1.f, 0.5f * (float)(i + 1)));
   }
   const unsigned int onPrepareFrameMsgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), onPrepareFrameMsgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(onPrepareFrameMsgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(onPrepareFrameMsgId);
   player = nullptr;
   table->Release();
   REQUIRE(state.done);
   frames = state.frames;
   builds = state.builds;
   viewPasses = state.passes;
   bakes = state.bakes;
}
#endif

// The shadow pages, the static shadow cache and the light list are scene level: a frame with editor panes (or the VR mirror) builds
// them once, for all its views, not once per view, and the cache bakes each light once whatever the number of views
TEST_CASE("PBR shadows and light list built once per frame with two views [render]")
{
   ResetVPX();
   const std::filesystem::path tableFile = CreatePBRShadowTable();

#if defined(ENABLE_BGFX)
   uint64_t singleViewBakes = 0;
   for (int extraViews = 0; extraViews <= 2; extraViews++)
   {
      INFO("views: " << extraViews + 1);
      g_settingsService.GetAppSettings().SetPlayer_GfxBackend("Direct3D11"s, false);
      g_settingsService.GetAppSettings().SetPlayer_DepthPrepass(2, false);
      uint64_t frames = 0, builds = 0, viewPasses = 0, bakes = 0;
      CountPBRFrameBuilds(tableFile, extraViews, frames, builds, viewPasses, bakes);
      MESSAGE("views " << extraViews + 1 << ": " << builds << " PBR frame builds in " << frames << " frames, " << viewPasses << " views with PBR passes, " << bakes << " shadow cache bake(s)");
      CHECK(frames >= 200);
      CHECK(builds == frames);
      // Every view recorded PBR passes in every frame: the extra views reused the frame's build, they did not skip PBR
      CHECK(viewPasses == frames * (uint64_t)(extraViews + 1));
      CHECK(bakes >= 1);
      if (extraViews == 0)
         singleViewBakes = bakes;
      else
         CHECK(bakes == singleViewBakes);
      ResetVPX();
   }
#endif

   ResetVPX();
}
