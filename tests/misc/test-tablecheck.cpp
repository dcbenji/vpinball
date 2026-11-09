// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/pintable.h"
#include "parts/ball.h"
#include "parts/bumper.h"
#include "parts/decal.h"
#include "parts/dispreel.h"
#include "parts/flipper.h"
#include "parts/hittarget.h"
#include "parts/plunger.h"
#include "parts/spinner.h"
#include "renderer/Texture.h"
#include "parts/kicker.h"
#include "parts/Material.h"
#include "parts/primitive.h"
#include "parts/ramp.h"
#include "parts/rubber.h"
#include "parts/surface.h"
#include "parts/light.h"
#include "parts/timer.h"
#include "parts/textbox.h"
#include "parts/flasher.h"
#include "parts/Sound.h"
#include "parts/tablecheck.h"
#include "parts/Collection.h"
#include "core/pinundo.h"
#include "ui/live/editor/ops/TableCheckFixOp.h"

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

// The issues of one check (live options)
vector<TableIssue> Issues(const PinTable *table, const TableCheckId check)
{
   vector<TableIssue> issues = table->CheckTable({ .extended = true });
   std::erase_if(issues, [check](const TableIssue &issue) { return issue.check != check; });
   return issues;
}

void SetPoints(DragPointCurve &curve, const vector<Vertex2D> &points)
{
   curve.ClearPoints();
   for (const Vertex2D &p : points)
      curve.PushPoint(std::make_unique<DragPoint>(&curve, p.x, p.y, 0.f, false));
}

// Applies the fixes of the issues as one undo record
void ApplyFixes(PinTable *table, PinUndo &undo, const vector<TableIssue> &issues)
{
   vector<const TableIssue *> fixes;
   for (const TableIssue &issue : issues)
      fixes.push_back(&issue);
   undo.BeginUndo();
   VPX::EditorUI::TableCheckFixOp::Apply(table, undo, fixes);
   undo.EndUndo();
}

CComObject<Collection> *AddTestCollection(PinTable *table, const char *name)
{
   CComObject<Collection> *collection;
   CComObject<Collection>::CreateInstance(&collection);
   collection->AddRef();
   collection->m_name = name;
   table->AddCollection(collection);
   collection->Release();
   return collection;
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
                              ". Warning: Primitive 'StaticMixed' seems to be referenced from the script while it is marked as static (most properties of a static object may not be modified at runtime).\r\n"
                              ". Warning: Primitive 'TransPrim' uses translucency (lighting from below) while it is fully opaque. Translucency will be discarded.\r\n"
                              ". Warning: legacy Textbox 'DMDText' is used for DMD rendering. It should be replaced by a flasher to get better rendering.\r\n"
                              ". Warning: Part 'FastTimer' uses a timer with a very short period of 10ms, below a 60FPS framerate. This will likely cause stutters and the table will not support 'frame pacing'.\r\n"
                              ". Error: Light 'NegLight' has a negative intensity.\r\n"
                              ". Error: Light 'NegLight' has a negative intensity scale.\r\n"
                              ". Error: Flasher 'Flasher1' uses the deprecated 'VideoCap' API (VideoCapWidth/VideoCapHeight/VideoCapUpdate). It is superseded by plugins and may be removed in a future version.\r\n"
                              ". Warning: VPM controller is used but table is missing a Timer object named 'PinMAMETimer'.\r\n"
                              ". Warning: VPM controller is used but vpmInit is not called. pause/resume/exit will likely exhibit bugs and physic outputs won't be supported.\r\n"
                              ". Warning: script uses 'vpmTimer' but table is missing a Timer object named 'PulseTimer'. vpmTimer will not work as expected.\r\n"
                              ". Error: sound 'StereoHit' is used for playfield physical sound but has multiple channels (not mono).\r\n"
                              ". Total sound size: 52 B\r\n"
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

   SUBCASE("issues name their check, severity and part")
   {
      PinTable *const table = CreateAuditTestTable();
      const vector<TableIssue> issues = table->CheckTable({});
      const auto find = [&issues](TableCheckId check) { return std::ranges::find_if(issues, [check](const TableIssue &i) { return i.check == check; }); };

      // The live scan leaves out the sound checks (they create an audio player) and the size totals
      CHECK(find(TableCheckId::Sound) == issues.end());
      CHECK(find(TableCheckId::Totals) == issues.end());
      CHECK(issues.size() == 17);

      const auto light = find(TableCheckId::LightIntensity);
      REQUIRE(light != issues.end());
      CHECK(light->severity == TableIssueSeverity::Error);
      CHECK(light->part == table->GetParts()[5]);
      CHECK(light->message == "Light 'NegLight' has a negative intensity.");
      CHECK(std::ranges::count_if(issues, [](const TableIssue &i) { return i.check == TableCheckId::LightIntensity; }) == 2);

      const auto prim = find(TableCheckId::StaticReferenced);
      REQUIRE(prim != issues.end());
      CHECK(prim->severity == TableIssueSeverity::Warning);
      CHECK(prim->part == table->GetParts()[0]);
      CHECK(find(TableCheckId::FastTimer)->part == table->GetParts()[4]);
      CHECK(find(TableCheckId::VideoCap)->part == table->GetParts()[6]);
      CHECK(find(TableCheckId::ScriptDuplicate)->part == nullptr);
      CHECK(find(TableCheckId::Backdrop)->message == "Desktop backdrop image 'MissingBG' is not present in the table (renders black)");

      const vector<TableIssue> withSound = table->CheckTable({ .sound = true });
      const auto sound = std::ranges::find_if(withSound, [](const TableIssue &i) { return i.check == TableCheckId::Sound; });
      REQUIRE(sound != withSound.end());
      CHECK(sound->severity == TableIssueSeverity::Error);
      CHECK(withSound.size() == issues.size() + 1);

      const vector<TableIssue> withTotals = table->CheckTable({ .totals = true });
      CHECK(std::ranges::count_if(withTotals, [](const TableIssue &i) { return i.check == TableCheckId::Totals && i.severity == TableIssueSeverity::Info; }) == 3);
      table->Release();
   }

   SUBCASE("script parsing reads the given script, identifiers are lower case")
   {
      PinTable *const table = CreateTestTable();
      table->m_script_text = "Sub Other\nEnd Sub\n";
      vector<string> functions, identifiers;
      int duplicates = 0;
      table->ParseScript("Sub Dup\nEnd Sub\nSub Dup\nEnd Sub\nMyPart.X = 1\n", functions, identifiers, [&duplicates](const string &, int) { duplicates++; });
      CHECK(duplicates == 1);
      CHECK(FindIndexOf(functions, ".dup"s) != -1);
      CHECK(FindIndexOf(functions, ".other"s) == -1);
      CHECK(FindIndexOf(identifiers, "mypart"s) != -1);
      table->Release();
   }

   SUBCASE("parts outside the playfield")
   {
      PinTable *const table = CreateTestTable();
      table->m_right = 1000.f;
      table->m_bottom = 2000.f;
      Bumper *const inside = AddTestPart<Bumper>(table, "Inside");
      Bumper *const outside = AddTestPart<Bumper>(table, "Outside");
      outside->m_d.m_vCenter = Vertex2D(-200.f, 500.f);
      // Visual parts the ball never touches (here a backbox light and a cabinet mesh, in playfield space): left out
      Light *const light = AddTestPart<Light>(table, "BackLight");
      light->m_d.m_vCenter = Vertex2D(500.f, 3000.f);
      light->m_d.m_shape = ShapeCircle;
      Primitive *const cabinet = AddTestPart<Primitive>(table, "CabinetMesh");
      cabinet->m_d.m_vPosition = Vertex3Ds(-500.f, 500.f, 0.f);
      cabinet->m_d.m_collidable = false;
      Primitive *const block = AddTestPart<Primitive>(table, "OffBlock");
      block->m_d.m_vPosition = Vertex3Ds(-500.f, 500.f, 0.f);
      block->m_d.m_collidable = true;
      const vector<TableIssue> issues = Issues(table, TableCheckId::OutsidePlayfield);
      REQUIRE(issues.size() == 2);
      CHECK(issues[0].part == outside);
      CHECK(issues[0].severity == TableIssueSeverity::Warning);
      CHECK(issues[0].message == "Part 'Outside' is outside the playfield: 200 past the left edge");
      CHECK(issues[1].part == block);
      CHECK(issues[1].severity == TableIssueSeverity::Info); // A collidable primitive: also often off the playfield on purpose
      CHECK(inside != nullptr);
      table->Release();
   }

   SUBCASE("missing and unused images and materials")
   {
      PinTable *const table = CreateTestTable();
      Surface *const wall = AddTestPart<Surface>(table, "Wall1");
      wall->m_d.m_szSideImage = "NoSuchImage";
      wall->m_d.m_szTopMaterial = "UsedMat";
      wall->m_d.m_szPhysicsMaterial = "NoPhysicsMat"; // Not used: the wall overwrites the physics of its material
      table->m_playfieldMaterial = "NoSuchMat";
      table->m_script_text = "Sub Table1_Init\nWall1.Material = \"ScriptMat\"\nEnd Sub\n";
      for (const char *name : { "UsedMat", "UnusedMat", "ScriptMat" })
      {
         Material *const mat = new Material();
         mat->m_name = name;
         table->AddMaterial(mat);
      }
      const vector<TableIssue> missing = Issues(table, TableCheckId::MissingResource);
      REQUIRE(missing.size() == 2);
      CHECK(missing[0].part == wall);
      CHECK(missing[0].message == "Part 'Wall1' uses image 'NoSuchImage' (Side Image), which is not in the table");
      CHECK(missing[1].part == nullptr);
      CHECK(missing[1].message == "Table uses material 'NoSuchMat' (Playfield Material), which is not in the table");
      const vector<TableIssue> unused = Issues(table, TableCheckId::UnusedMaterial);
      REQUIRE(unused.size() == 1);
      CHECK(unused[0].severity == TableIssueSeverity::Info);
      CHECK(unused[0].material == table->GetMaterialList()[1]);
      wall->m_d.m_overwritePhysics = false;
      CHECK(Issues(table, TableCheckId::MissingResource).size() == 3);
      table->Release();
   }

   SUBCASE("duplicate part names")
   {
      PinTable *const table = CreateTestTable();
      AddTestPart<Bumper>(table, "Bumper1");
      Bumper *const second = AddTestPart<Bumper>(table, "Bumper2");
      second->GetIScriptable()->m_name = "BUMPER1"; // As loaded from an old file: names are not case sensitive for the script
      const vector<TableIssue> issues = Issues(table, TableCheckId::DuplicateName);
      REQUIRE(issues.size() == 2);
      CHECK(issues[0].severity == TableIssueSeverity::Error);
      CHECK(issues[1].part == second);
      table->Release();
   }

   SUBCASE("colliders that can not collide")
   {
      PinTable *const table = CreateTestTable();
      Surface *const wall = AddTestPart<Surface>(table, "FlatWall"); // Still collides with its top: not reported
      wall->m_d.m_collidable = true;
      wall->m_d.m_heighttop = wall->m_d.m_heightbottom = 0.f;
      Rubber *const rubber = AddTestPart<Rubber>(table, "ThinRubber");
      rubber->m_d.m_collidable = true;
      rubber->m_d.m_thickness = 0;
      AddTestPart<Bumper>(table, "NoBumper")->m_d.m_radius = 0.f;
      AddTestPart<Kicker>(table, "NoKicker")->m_d.m_radius = 0.f;
      Ramp *const ramp = AddTestPart<Ramp>(table, "NoRamp");
      ramp->m_d.m_collidable = true;
      ramp->m_d.m_type = RampTypeFlat;
      ramp->m_d.m_widthbottom = ramp->m_d.m_widthtop = 0.f;
      AddTestPart<Bumper>(table, "GoodBumper");
      const vector<TableIssue> issues = Issues(table, TableCheckId::Collider);
      REQUIRE(issues.size() == 4);
      for (const TableIssue &issue : issues)
         CHECK(issue.severity == TableIssueSeverity::Error);
      CHECK(issues[0].message == "Collidable rubber 'ThinRubber' has no thickness");
      table->Release();
   }

   SUBCASE("Where used lists every image reference of the parts and the table settings")
   {
      PinTable *const table = CreateTestTable();
      table->m_name = "Table1";
      REQUIRE(table->ImportImage(GetAssetPath() / "test000-default-table-Ref.webp", "Shared"s) != nullptr);
      AddTestPart<Ball>(table, "Ball1")->m_d.m_imageDecal = "Shared";
      AddTestPart<Surface>(table, "Wall1")->m_d.m_szSideImage = "shared"; // Names are not case sensitive
      AddTestPart<Bumper>(table, "Bumper1")->m_d.m_szImage = "Other";
      table->m_imageColorGrade = "Shared";
      table->m_BG_image[BG_FSS] = "Shared";
      vector<WhereUsedInfo> users;
      table->ShowWhereImageUsed(users, table->GetImageList()[0]);
      REQUIRE(users.size() == 4);
      CHECK(users[0].whereUsedObjectname == "Ball1");
      CHECK(users[0].whereUsedPropertyName == "Decal Image");
      CHECK(users[1].whereUsedObjectname == "Wall1");
      CHECK(users[1].whereUsedPropertyName == "Side Image");
      CHECK(users[2].whereUsedObjectname == "Table1");
      CHECK(users[2].whereUsedPropertyName == "Color Grade Image");
      CHECK(users[3].whereUsedObjectname == "Table1");
      CHECK(users[3].whereUsedPropertyName == "Full Single Screen Backdrop Image");
      table->Release();
   }

   SUBCASE("drag points and self crossing outlines")
   {
      PinTable *const table = CreateTestTable();
      Ramp *const ramp = AddTestPart<Ramp>(table, "OnePoint");
      ramp->m_curve.ClearPoints();
      ramp->m_curve.PushPoint(std::make_unique<DragPoint>(&ramp->m_curve, 0.f, 0.f, 0.f, false));
      Surface *const twin = AddTestPart<Surface>(table, "TwinPoints");
      SetPoints(twin->m_curve, { { 0.f, 0.f }, { 100.f, 0.f }, { 100.f, 0.f }, { 100.f, 100.f }, { 0.f, 100.f } });
      Surface *const bowtie = AddTestPart<Surface>(table, "Bowtie");
      SetPoints(bowtie->m_curve, { { 0.f, 0.f }, { 100.f, 100.f }, { 100.f, 0.f }, { 0.f, 100.f } });
      Surface *const square = AddTestPart<Surface>(table, "Square");
      SetPoints(square->m_curve, { { 0.f, 0.f }, { 100.f, 0.f }, { 100.f, 100.f }, { 0.f, 100.f } });

      const vector<TableIssue> points = Issues(table, TableCheckId::DragPoints);
      REQUIRE(points.size() == 2);
      CHECK(points[0].part == ramp);
      CHECK(points[0].severity == TableIssueSeverity::Error);
      CHECK(points[0].message == "Part 'OnePoint' has only 1 drag point (at least 2 needed)");
      CHECK(points[1].part == twin);
      CHECK(points[1].severity == TableIssueSeverity::Info); // Harmless
      CHECK(points[1].message == "Part 'TwinPoints': drag points 2 and 3 are on the same spot (zero length segment)");

      const vector<TableIssue> crossing = Issues(table, TableCheckId::SelfCrossing);
      REQUIRE(crossing.size() == 1);
      CHECK(crossing[0].part == bowtie);
      table->Release();
   }

   SUBCASE("the audit text leaves out the table check's own checks")
   {
      PinTable *const table = CreateTestTable();
      table->m_ballSphericalMapping = false;
      AddTestPart<Bumper>(table, "Bumper1");
      Bumper *const second = AddTestPart<Bumper>(table, "Bumper2");
      second->GetIScriptable()->m_name = "BUMPER1";
      CHECK(table->AuditTable(false).find("same name") == string::npos);
      CHECK(table->AuditTable(false).find("No issue identified.") != string::npos);
      CHECK(Issues(table, TableCheckId::DuplicateName).size() == 2);
      table->Release();
   }

   SUBCASE("fix: disable an enabled timer without a sub, undo and redo")
   {
      using VPX::EditorUI::TableCheckFixOp;
      PinTable *const table = CreateTestTable();
      table->m_script_text = "Sub Busy_Timer\nEnd Sub\n";
      Timer *const lonely = AddTestPart<Timer>(table, "Lonely");
      Timer *const busy = AddTestPart<Timer>(table, "Busy");
      for (Timer *timer : { lonely, busy })
      {
         timer->m_timerEnabled = true;
         timer->m_timerInterval = 100;
      }
      AddTestPart<Timer>(table, "Off")->m_timerEnabled = false;

      vector<TableIssue> issues = Issues(table, TableCheckId::TimerSub);
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].part == lonely);
      CHECK(issues[0].fix == TableFix::DisableTimer);
      CHECK(issues[0].message == "Part 'Lonely' has an enabled timer but the script has no 'Lonely_Timer' sub");

      // An issue older than the table is not fixed: a fresh check must still offer the fix (here the script got the sub meanwhile)
      CHECK(TableCheckFixOp::Confirmed(table, { &issues[0] }).size() == 1);
      table->m_script_text = "Sub Busy_Timer\nEnd Sub\nSub Lonely_Timer\nEnd Sub\n";
      CHECK(TableCheckFixOp::Confirmed(table, { &issues[0] }).empty());
      table->m_script_text = "Sub Busy_Timer\nEnd Sub\n";

      // Never applied to the live copy of a play test (no undo, its objects belong to its base table)
      PinTable *const base = CreateTestTable();
      table->m_liveBaseTable = base;
      {
         PinUndo liveUndo(table);
         ApplyFixes(table, liveUndo, issues);
      }
      CHECK(lonely->m_timerEnabled);
      table->m_liveBaseTable = nullptr;
      base->Release();

      PinUndo undo(table);
      ApplyFixes(table, undo, issues);
      CHECK_FALSE(lonely->m_timerEnabled);
      CHECK(busy->m_timerEnabled);
      CHECK(Issues(table, TableCheckId::TimerSub).empty());
      CHECK_FALSE(TableCheckFixOp::IsApplicable(table, issues[0])); // Already fixed
      undo.Undo();
      CHECK(lonely->m_timerEnabled);
      undo.Redo();
      CHECK_FALSE(lonely->m_timerEnabled);

      // A script that loads other code may define the sub there (e.g. the VPinMAME scripts): an info, not fixed
      busy->m_timerEnabled = false;
      lonely->m_timerEnabled = true;
      for (const char *script : { "LoadVPM \"01000000\", \"S11.VBS\", 1\n", "ExecuteGlobal code\n", "Execute code\n", "Set f = GetRef(\"Other\")\n" })
      {
         table->m_script_text = script;
         issues = Issues(table, TableCheckId::TimerSub);
         REQUIRE(issues.size() == 1);
         CHECK(issues[0].fix == TableFix::None);
         CHECK(issues[0].severity == TableIssueSeverity::Info);
      }

      // The timer of a part in a collection that fires events also fires the collection's Timer event (e.g. Coll_Timer(idx))
      table->m_script_text = "Sub Table1_Init\nEnd Sub\n";
      CComObject<Collection> *const timers = AddTestCollection(table, "Timers");
      table->AddPartToCollection(timers, lonely);
      timers->m_fireEvents = true;
      CHECK(Issues(table, TableCheckId::TimerSub).empty());
      timers->m_fireEvents = false;
      CHECK(Issues(table, TableCheckId::TimerSub).size() == 1);
      table->Release();
   }

   SUBCASE("fix: negative light intensity to 0, undo and redo")
   {
      PinTable *const table = CreateTestTable();
      Light *const light = AddTestPart<Light>(table, "NegLight");
      light->m_d.m_intensity = -2.f;
      light->m_d.m_intensity_scale = -1.f;
      const vector<TableIssue> issues = Issues(table, TableCheckId::LightIntensity);
      REQUIRE(issues.size() == 2);
      CHECK(issues[0].fix == TableFix::ClearLightIntensity);
      CHECK(issues[1].fix == TableFix::None); // The intensity scale is set by the script, it is not saved with the table
      PinUndo undo(table);
      ApplyFixes(table, undo, issues);
      CHECK(light->m_d.m_intensity == 0.f);
      undo.Undo();
      CHECK(light->m_d.m_intensity == -2.f);
      CHECK_FALSE(undo.HasUndo());
      undo.Redo();
      CHECK(light->m_d.m_intensity == 0.f);
      table->Release();
   }

   SUBCASE("fix: clear a missing backdrop image, undo and redo")
   {
      PinTable *const table = CreateTestTable();
      table->m_BG_image[BG_FULLSCREEN] = "MissingCabinet";
      const vector<TableIssue> issues = Issues(table, TableCheckId::Backdrop);
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].index == BG_FULLSCREEN);
      CHECK(issues[0].fix == TableFix::ClearBackdropImage);
      PinUndo undo(table);
      ApplyFixes(table, undo, issues);
      CHECK(table->m_BG_image[BG_FULLSCREEN].empty());
      CHECK(Issues(table, TableCheckId::Backdrop).empty());
      undo.Undo();
      CHECK(table->m_BG_image[BG_FULLSCREEN] == "MissingCabinet");
      undo.Redo();
      CHECK(table->m_BG_image[BG_FULLSCREEN].empty());
      undo.Undo();
      CHECK(table->m_BG_image[BG_FULLSCREEN] == "MissingCabinet");
      table->Release();
   }

   SUBCASE("fix: delete an empty collection, undo and redo")
   {
      PinTable *const table = CreateTestTable();
      table->m_script_text = "Sub Table1_Init\nFor Each x In Used\nNext\nEnd Sub\n";
      CComObject<Collection> *const leftover = AddTestCollection(table, "Leftover");
      CComObject<Collection> *const full = AddTestCollection(table, "Full");
      CComObject<Collection> *const used = AddTestCollection(table, "Used");
      table->AddPartToCollection(full, AddTestPart<Bumper>(table, "Bumper1"));

      const vector<TableIssue> issues = Issues(table, TableCheckId::EmptyCollection);
      REQUIRE(issues.size() == 2);
      CHECK(issues[0].collection == leftover);
      CHECK(issues[0].fix == TableFix::DeleteCollection);
      CHECK(issues[1].collection == used);
      CHECK(issues[1].fix == TableFix::None); // The script uses its name

      PinUndo undo(table);
      ApplyFixes(table, undo, issues);
      REQUIRE(table->GetCollections().size() == 2);
      CHECK(table->GetCollections()[0] == full);
      CHECK(table->IsNameUnique("Leftover"));
      undo.Undo();
      REQUIRE(table->GetCollections().size() == 3);
      CHECK(table->GetCollections()[0] == leftover); // Back at its place, with its name
      CHECK_FALSE(table->IsNameUnique("Leftover"));
      undo.Redo();
      CHECK(table->GetCollections().size() == 2);
      undo.Undo();
      CHECK(table->GetCollections()[0] == leftover);

      // Deleted again outside the undo system (as the properties panel does): the redo record would remove it a second time
      table->RemoveCollection(leftover);
      undo.ClearRedo();
      CHECK_FALSE(undo.HasRedo());
      CHECK(table->GetCollections().size() == 2);

      // Not offered when the script names the collection in a string, or loads other code
      CComObject<Collection> *const evaled = AddTestCollection(table, "Evaled");
      table->m_script_text = "Sub Table1_Init\nSet c = Eval(\"Evaled\")\nEnd Sub\n";
      for (const TableIssue &issue : Issues(table, TableCheckId::EmptyCollection))
         CHECK(issue.fix == (issue.collection == evaled ? TableFix::None : TableFix::DeleteCollection));
      table->m_script_text = "ExecuteGlobal code\n";
      for (const TableIssue &issue : Issues(table, TableCheckId::EmptyCollection))
         CHECK(issue.fix == TableFix::None);
      table->Release();
   }

   SUBCASE("unused images: every reference site counts")
   {
      PinTable *const table = CreateTestTable();
      // One image per place that can name an image: each one is used, except 'Unused'
      vector<std::pair<string, std::function<void(const string &)>>> sites;
      const auto site = [&sites](const string &name, std::function<void(const string &)> set) { sites.emplace_back(name, std::move(set)); };
      site("BallImage", [table](const string &n) { AddTestPart<Ball>(table, "Ball1")->m_d.m_szImage = n; });
      site("BallDecal", [table](const string &n) { static_cast<Ball *>(table->GetParts().back())->m_d.m_imageDecal = n; });
      site("DecalImage", [table](const string &n) { AddTestPart<Decal>(table, "Decal1")->m_d.m_szImage = n; });
      site("ReelImage", [table](const string &n) { AddTestPart<DispReel>(table, "Reel1")->m_d.m_szImage = n; });
      site("FlasherA", [table](const string &n) { AddTestPart<Flasher>(table, "Flasher1")->m_d.m_szImageA = n; });
      site("FlasherB", [table](const string &n) { static_cast<Flasher *>(table->GetParts().back())->m_d.m_szImageB = n; });
      site("FlipperImage", [table](const string &n) { AddTestPart<Flipper>(table, "Flipper1")->m_d.m_szImage = n; });
      site("TargetImage", [table](const string &n) { AddTestPart<HitTarget>(table, "Target1")->m_d.m_szImage = n; });
      site("LightImage", [table](const string &n) { AddTestPart<Light>(table, "Light1")->m_d.m_szImage = n; });
      site("PlungerImage", [table](const string &n) { AddTestPart<Plunger>(table, "Plunger1")->m_d.m_szImage = n; });
      site("PrimImage", [table](const string &n) { AddTestPart<Primitive>(table, "Prim1")->m_d.m_szImage = n; });
      site("PrimNormal", [table](const string &n) { static_cast<Primitive *>(table->GetParts().back())->m_d.m_szNormalMap = n; });
      site("RampImage", [table](const string &n) { AddTestPart<Ramp>(table, "Ramp1")->m_d.m_szImage = n; });
      site("RubberImage", [table](const string &n) { AddTestPart<Rubber>(table, "Rubber1")->m_d.m_szImage = n; });
      site("SpinnerImage", [table](const string &n) { AddTestPart<Spinner>(table, "Spinner1")->m_d.m_szImage = n; });
      site("WallTop", [table](const string &n) { AddTestPart<Surface>(table, "Wall1")->m_d.m_szImage = n; });
      site("WallSide", [table](const string &n) { static_cast<Surface *>(table->GetParts().back())->m_d.m_szSideImage = n; });
      site("Playfield", [table](const string &n) { table->m_image = n; });
      site("TableBall", [table](const string &n) { table->m_ballImage = n; });
      site("TableBallDecal", [table](const string &n) { table->m_ballImageDecal = n; });
      site("Environment", [table](const string &n) { table->m_envImage = n; });
      site("ColorGrade", [table](const string &n) { table->m_imageColorGrade = n; });
      site("Screenshot", [table](const string &n) { table->m_screenShot = n; });
      site("BackdropDesktop", [table](const string &n) { table->m_BG_image[BG_DESKTOP] = n; });
      site("BackdropCabinet", [table](const string &n) { table->m_BG_image[BG_FULLSCREEN] = n; });
      site("BackdropFSS", [table](const string &n) { table->m_BG_image[BG_FSS] = n; });
      site("InBlurb", [table](const string &n) { table->m_blurb = "Rules: ![](" + n + ")"; });
      site("InDescription", [table](const string &n) { table->m_description = "![shot](" + n + " \"title\")"; });
      site("InScript", [table](const string &n) { table->m_script_text = "Sub Table1_Init\nPrim1.Image = \"" + n + "\" ' a comment\nEnd Sub\n"; });
      const std::filesystem::path file = GetAssetPath() / "test000-default-table-Ref.webp";
      for (const auto &[name, set] : sites)
      {
         REQUIRE(table->ImportImage(file, name) != nullptr);
         set(name);
      }
      Texture *const unused = table->ImportImage(file, "Unused"s);
      REQUIRE(unused != nullptr);
      table->m_blurb += " ](Unused2) Unused"; // Not a link to 'Unused'
      table->m_script_text += "' \"Unused\" in a comment\n";

      vector<TableIssue> issues = Issues(table, TableCheckId::UnusedImage);
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].image == unused);
      CHECK(issues[0].severity == TableIssueSeverity::Info);
      CHECK(issues[0].message == "Image 'Unused' is not used by any part, table setting, table description or script string");

      // The image users listed by the Win32 and live editor 'Where used' come from the same list
      for (Texture *const image : table->GetImageList())
      {
         vector<WhereUsedInfo> users;
         table->ShowWhereImageUsed(users, image);
         const bool listed = image->m_name == "InBlurb" || image->m_name == "InDescription" || image->m_name == "InScript" || image == unused;
         CHECK_MESSAGE(users.empty() == listed, image->m_name);
      }

      CHECK(Issues(table, TableCheckId::LargeImage).empty());
      unused->m_width = 8192;
      issues = Issues(table, TableCheckId::LargeImage);
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].image == unused);
      table->Release();
   }

   SUBCASE("unused images: names the script builds")
   {
      PinTable *const table = CreateTestTable();
      const std::filesystem::path file = GetAssetPath() / "test000-default-table-Ref.webp";
      for (const char *name : { "Digit3", "Ball2", "Lamp_on", "Plain" })
         REQUIRE(table->ImportImage(file, name) != nullptr);
      const auto unused = [table]()
      {
         vector<string> names;
         for (const TableIssue &issue : Issues(table, TableCheckId::UnusedImage))
            names.push_back(issue.image->m_name);
         return names;
      };

      // A literal joined to something else is the start or the end of a name: the images it may build count as used
      table->m_script_text = "Sub Table1_Init\ns = \"Digit\" & n : Reel.Image = s\nTable1.BallImage = \"ball\" & n\nLight1.Image = n & \"_on\"\nEnd Sub\n";
      CHECK(unused() == vector<string> { "Plain" });
      table->m_script_text = "Function DigitImg(n)\nDigitImg = \"Digit\" + CStr(n)\nEnd Function\nSub Table1_Init\nReel.Image = DigitImg(n)\nEnd Sub\n";
      CHECK(unused() == vector<string> { "Ball2", "Lamp_on", "Plain" });

      // Names built from variables only, LoadTexture: any image may be used, the issue says so
      for (const char *script : { "Sub Table1_Init\nPrim1.SideImage=prefix & n\nEnd Sub\n", "Table1.EnvironmentImage = a + b\n", "Set img = LoadTexture(\"x\", path)\n",
              "\tREM nothing\nSet img = LoadTexture(\"x\", path)\n" })
      {
         table->m_script_text = script;
         const vector<TableIssue> issues = Issues(table, TableCheckId::UnusedImage);
         REQUIRE(issues.size() == 4);
         CHECK(issues[0].message.find("the script builds image names") != string::npos);
      }
      // An image property set from anything but a single literal (a constant, a variable, Split, Replace, a function result): any image
      // may be used, no delete is offered
      for (const char *script : { "Const P = \"Digit\"\nSub Show(reel, n)\nDim img : img = P & n\nreel.Image = img\nEnd Sub\n",
              "imgs = Split(\"d0,d1,d2\", \",\")\nobj.Image = imgs(n)\n", "obj.Image = Replace(base, \"off\", \"on\")\n", "Reel.Image = DigitImg(n)\n" })
      {
         table->m_script_text = script;
         const vector<TableIssue> issues = Issues(table, TableCheckId::UnusedImage);
         REQUIRE(issues.size() == 4);
         for (const TableIssue &issue : issues)
         {
            CHECK(issue.fix == TableFix::None);
            CHECK(issue.message.find("the script builds image names") != string::npos);
         }
      }
      // A script that loads other code
      table->m_script_text = "ExecuteGlobal GetTextFile(\"core.vbs\")\n";
      REQUIRE(Issues(table, TableCheckId::UnusedImage).size() == 4);
      CHECK(Issues(table, TableCheckId::UnusedImage)[0].message.find("the script loads other code") != string::npos);

      // Not image names: a concatenation in a comment or a REM line, an image property of a longer name
      for (const char *script : { "' Light1.Image = \"Digit\" & i\n", "Light1.ImageCount = a & b\n", "  Rem LoadTexture x\n", "\tREM\tLoadTexture x\n", "REM\n" })
      {
         table->m_script_text = script;
         const vector<TableIssue> issues = Issues(table, TableCheckId::UnusedImage);
         REQUIRE(issues.size() == 4);
         CHECK(issues[0].message.find("the script") == string::npos);
      }
      table->Release();
   }

   SUBCASE("without the script checks")
   {
      PinTable *const table = CreateTestTable();
      table->m_name = "Table1";
      Timer *const timer = AddTestPart<Timer>(table, "Lonely");
      timer->m_timerEnabled = true;
      AddTestCollection(table, "Leftover");
      REQUIRE(table->ImportImage(GetAssetPath() / "test000-default-table-Ref.webp", "Unused"s) != nullptr);
      table->m_script_text = "Sub Dup\nEnd Sub\nSub Dup\nEnd Sub\nSub Gone_Hit\nEnd Sub\nSub Table1_Init\nPlaySound \"Missing\"\nEnd Sub\n";
      const auto count = [table](const bool script, const TableCheckId check)
      {
         const vector<TableIssue> issues = table->CheckTable({ .extended = true, .script = script });
         return std::ranges::count_if(issues, [check](const TableIssue &issue) { return issue.check == check; });
      };
      for (const TableCheckId check : { TableCheckId::ScriptDuplicate, TableCheckId::TimerSub, TableCheckId::EmptyCollection, TableCheckId::UnusedImage })
      {
         CHECK(count(true, check) == 1);
         CHECK(count(false, check) == 0); // Not reported: they need the script
      }
      table->Release();
   }

   SUBCASE("summary: the counts and the check behind most errors and warnings")
   {
      const TableCheckSummary summary = TableCheckSummary::Of({ { TableIssueSeverity::Warning, TableCheckId::Glass, "a" }, { TableIssueSeverity::Warning, TableCheckId::Translucency, "b" },
         { TableIssueSeverity::Warning, TableCheckId::Translucency, "c" }, { TableIssueSeverity::Info, TableCheckId::OutsidePlayfield, "d" },
         { TableIssueSeverity::Info, TableCheckId::OutsidePlayfield, "e" }, { TableIssueSeverity::Info, TableCheckId::OutsidePlayfield, "f" } });
      CHECK(summary.counts[1] == 3);
      CHECK(summary.counts[2] == 3);
      CHECK(summary.top == TableCheckId::Translucency); // Infos are not counted
      CHECK(summary.topCount == 2);
      CHECK(TableCheckSummary::Of({}).topCount == 0);
   }

   SUBCASE("every check has a title")
   {
      for (int i = 0; i < static_cast<int>(TableCheckId::Count); i++)
         CHECK(string(TableCheckTitle(static_cast<TableCheckId>(i))) != "");
   }
}
