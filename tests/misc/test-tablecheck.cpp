// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/pintable.h"
#include "parts/primitive.h"
#include "parts/light.h"
#include "parts/timer.h"
#include "parts/textbox.h"
#include "parts/flasher.h"
#include "parts/Sound.h"

#include "doctest.h"

namespace
{

template <class T> T *AddTestPart(PinTable *table, const char *name)
{
   T *const part = T::COMCreate();
   part->Init(100.f, 200.f, false);
   part->SetName(name);
   table->AddPart(part);
   part->Release();
   return part;
}

// A minimal PCM WAV file with 2 channels
vector<uint8_t> MakeStereoWav()
{
   const auto append32 = [](vector<uint8_t> &out, uint32_t v) { for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); };
   const auto append16 = [](vector<uint8_t> &out, uint16_t v) { for (int i = 0; i < 2; i++) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); };
   const vector<uint8_t> samples(8, 0);
   vector<uint8_t> wav { 'R', 'I', 'F', 'F' };
   append32(wav, static_cast<uint32_t>(36 + samples.size()));
   wav.insert(wav.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
   append32(wav, 16);
   append16(wav, 1); // PCM
   append16(wav, 2); // stereo
   append32(wav, 44100);
   append32(wav, 44100 * 2 * 2);
   append16(wav, 4);
   append16(wav, 16);
   wav.insert(wav.end(), { 'd', 'a', 't', 'a' });
   append32(wav, static_cast<uint32_t>(samples.size()));
   wav.insert(wav.end(), samples.begin(), samples.end());
   return wav;
}

// A table which triggers every check of the audit
PinTable *CreateAuditTestTable()
{
   PinTable *const table = CreateTestTable();
   table->m_script_text = "Sub Dup\nEnd Sub\nSub Dup\nEnd Sub\nSub Table1_Init\nExecute \"x = 1\"\nLoadVPM \"01000000\", \"S11.VBS\", 1\nstaticprim.X = 1\nStaticMixed.X = 1\n"
                          "vpmTimer.Add 1, \"x\"\nFlasher1.VideoCapWidth = 320\nEnd Sub\n";
   table->m_glassBottomHeight = 300.f;
   table->m_glassTopHeight = 200.f;
   table->m_ballSphericalMapping = true;
   table->m_loadFixes.push_back("Playfield had an invalid friction (-1), it was set to 0 (save the table to keep the fix)");
   for (const char *name : { "staticprim", "StaticMixed", "TransPrim" })
   {
      Primitive *const prim = AddTestPart<Primitive>(table, name);
      prim->m_d.m_use3DMesh = false;
      prim->m_d.m_visible = true;
      prim->m_d.m_staticRendering = name != "TransPrim"s;
      prim->m_d.m_disableLightingBelow = name == "TransPrim"s ? 0.5f : 1.f;
   }
   AddTestPart<Textbox>(table, "DMDText")->m_d.m_isDMD = true;
   Timer *const timer = AddTestPart<Timer>(table, "FastTimer");
   timer->m_timerEnabled = true;
   timer->m_timerInterval = 10;
   Light *const light = AddTestPart<Light>(table, "NegLight");
   light->m_d.m_intensity = -1.f;
   light->m_d.m_intensity_scale = -1.f;
   AddTestPart<Flasher>(table, "Flasher1");
   table->m_vsound.push_back(new VPX::Sound("StereoHit", "stereo.wav", MakeStereoWav()));
   table->m_BG_image[0] = "MissingBG";
   return table;
}

}

TEST_CASE("Table check")
{
   SUBCASE("audit text of a table with every problem")
   {
      PinTable *const table = CreateAuditTestTable();
      const string expected = "Table audit:\r\n"
                              ". Error: Duplicate declaration of 'dup' in script at line 2\r\n"
                              ". Warning: Scripts seems to use the 'Execute' command. This command triggers computer security checks and will likely cause stutters during play.\r\n"
                              ". Warning: Glass height seems invalid: bottom is higher than top\r\n"
                              ". Warning: Ball uses legacy 'spherical mapping', it will be rendered like a 2D object and therefore will look bad in VR, stereo or headtracking\r\n"
                              ". Warning: Playfield had an invalid friction (-1), it was set to 0 (save the table to keep the fix)\r\n"
                              ". Warning: Primitive 'staticprim' seems to be referenced from the script while it is marked as static (most properties of a static object may not be modified at runtime).\r\n"
                              ". Warning: Primitive 'TransPrim' uses translucency (lighting from below) while it is fully opaque. Translucency will be discarded.\r\n"
                              ". Warning: legacy Textbox 'DMDText' is used for DMD rendering. It should be replaced by a flasher to get better rendering.\r\n"
                              ". Warning: Part 'FastTimer' uses a timer with a very short period of 10ms, below a 60FPS framerate. This will likely cause stutters and the table will not support 'frame pacing'.\r\n"
                              ". Error: Light 'NegLight' has a negative intensity.\r\n"
                              ". Error: Light 'NegLight' has a negative intensity scale.\r\n"
                              ". Error: Flasher 'Flasher1' uses the deprecated 'VideoCap' API (VideoCapWidth/VideoCapHeight/VideoCapUpdate). It is superseded by plugins and may be removed in a future version.\r\n"
                              ". Warning: VPM controller is used but table is missing a Timer object named 'PinMAMETimer'.\r\n"
                              ". Warning: VPM controller is used but vpmInit is not called. pause/resume/exit will likely exhibit bugs and physic outputs won't be supported.\r\n"
                              ". Error: sound 'StereoHit' is used for playfield physical sound but has multiple channels (not mono).\r\n"
                              " . Total sound size: 52 B\r\n"
                              ". Total image size: 0 B in VPX file, at least 0 B in GPU memory when played\r\n"
                              ". Warning: Desktop backdrop image 'MissingBG' is not present in the table (renders black)\r\n"
                              ". Total number of faces used in primitives: 0, needing 0 B in GPU memory when played\r\n";
      CHECK(table->AuditTable(false) == expected);
      table->Release();
   }

   SUBCASE("audit text of a clean table, with a missing backdrop image")
   {
      PinTable *const table = CreateTestTable();
      table->m_ballSphericalMapping = false;
      const string totals = ". Total sound size: 0 B\r\n"
                            ". Total image size: 0 B in VPX file, at least 0 B in GPU memory when played\r\n";
      const string faces = ". Total number of faces used in primitives: 0, needing 0 B in GPU memory when played\r\n";
      CHECK(table->AuditTable(false) == "Table audit:\r\nNo issue identified.\r\n" + totals + faces);

      // The backdrop check runs after the totals: the table is still said to have no issue
      table->m_BG_image[2] = "MissingFSS";
      CHECK(table->AuditTable(false)
         == "Table audit:\r\nNo issue identified.\r\n" + totals + ". Warning: Full Single Screen backdrop image 'MissingFSS' is not present in the table (renders black)\r\n" + faces);
      table->Release();
   }
}
