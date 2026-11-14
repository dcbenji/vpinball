// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/pintable.h"
#include "parts/ramp.h"
#include "math/dragpoint.h"
#include "physics/collideex.h"
#include "physics/PhysicsEngine.h"
#include "utils/BiffReader.h"
#include "utils/BiffWriter.h"

#include "doctest.h"

TEST_CASE("Ramp part")
{
   PinTable* const table = CreateTestTable();

   SUBCASE("BIFF save/load round-trip")
   {
      Ramp* const ramp = Ramp::COMCreate();
      ramp->Init(100.f, 200.f, false);
      ramp->SetName("Ramp1");
      ramp->m_d.m_heightbottom = 10.f;
      ramp->m_d.m_heighttop = 90.f;
      ramp->m_d.m_widthbottom = 80.f;
      ramp->m_d.m_widthtop = 40.f;
      ramp->m_d.m_type = RampType4Wire;
      ramp->m_d.m_imagealignment = ImageModeWrap;
      ramp->m_d.m_leftwallheight = 55.f;
      ramp->m_d.m_rightwallheight = 65.f;
      ramp->m_d.m_leftwallheightvisible = 35.f;
      ramp->m_d.m_rightwallheightvisible = 45.f;
      ramp->m_d.m_wireDiameter = 6.f;
      ramp->m_d.m_wireDistanceX = 30.f;
      ramp->m_d.m_wireDistanceY = 60.f;
      ramp->m_d.m_depthBias = 2.f;
      ramp->m_d.m_imageWalls = false;
      ramp->m_d.m_szImage = "ramp.png";
      ramp->m_d.m_szMaterial = "rampMat";
      ramp->m_d.m_szPhysicsMaterial = "physMat";
      ramp->m_d.m_visible = false;
      ramp->m_d.m_collidable = false;
      ramp->m_d.m_hitEvent = true;
      ramp->m_d.m_reflectionEnabled = false;
      table->AddPart(ramp);
      ramp->Release();

      const InMemStream saved = SavePartToStream(ramp);

      Ramp* const copy = Ramp::COMCreate();
      LoadPartFromStream(copy, saved);

      CHECK(copy->GetName() == "Ramp1");
      CHECK(copy->m_d.m_heightbottom == 10.f);
      CHECK(copy->m_d.m_heighttop == 90.f);
      CHECK(copy->m_d.m_widthbottom == 80.f);
      CHECK(copy->m_d.m_widthtop == 40.f);
      CHECK(copy->m_d.m_type == RampType4Wire);
      CHECK(copy->m_d.m_imagealignment == ImageModeWrap);
      CHECK(copy->m_d.m_leftwallheight == 55.f);
      CHECK(copy->m_d.m_rightwallheight == 65.f);
      CHECK(copy->m_d.m_leftwallheightvisible == 35.f);
      CHECK(copy->m_d.m_rightwallheightvisible == 45.f);
      CHECK(copy->m_d.m_wireDiameter == 6.f);
      CHECK(copy->m_d.m_wireDistanceX == 30.f);
      CHECK(copy->m_d.m_wireDistanceY == 60.f);
      CHECK(copy->m_d.m_depthBias == 2.f);
      CHECK(copy->m_d.m_imageWalls == false);
      CHECK(copy->m_d.m_szImage == "ramp.png");
      CHECK(copy->m_d.m_szMaterial == "rampMat");
      CHECK(copy->m_d.m_szPhysicsMaterial == "physMat");
      CHECK(copy->m_d.m_visible == false);
      CHECK(copy->m_d.m_collidable == false);
      CHECK(copy->m_d.m_hitEvent == true);
      CHECK(copy->m_d.m_reflectionEnabled == false);

      // The default 2-point ramp curve round-trips through the stream
      REQUIRE(copy->m_curve.GetPoints().size() == ramp->m_curve.GetPoints().size());
      CHECK(copy->m_curve.GetPoints()[0]->GetX() == doctest::Approx(ramp->m_curve.GetPoints()[0]->GetX()));
      CHECK(copy->m_curve.GetPoints()[0]->GetY() == doctest::Approx(ramp->m_curve.GetPoints()[0]->GetY()));
      CHECK(copy->m_curve.GetPoints()[0]->GetCalcHeight() == doctest::Approx(ramp->m_curve.GetPoints()[0]->GetCalcHeight()));

      CHECK(StreamsEqual(saved, SavePartToStream(copy)));

      copy->Release();
   }

   SUBCASE("CopyForPlay preserves the serialized state")
   {
      Ramp* const ramp = Ramp::COMCreate();
      ramp->Init(50.f, 60.f, false);
      ramp->SetName("Ramp2");
      ramp->m_d.m_type = RampType1Wire;
      table->AddPart(ramp);
      ramp->Release();

      Ramp* const liveCopy = ramp->CopyForPlay();
      CHECK(StreamsEqual(SavePartToStream(ramp), SavePartToStream(liveCopy)));
      liveCopy->Release();
   }

   SUBCASE("surface height interpolates along the curve")
   {
      Ramp* const ramp = Ramp::COMCreate();
      ramp->Init(0.f, 0.f, false);
      ramp->SetName("Ramp3");
      table->AddPart(ramp);
      ramp->Release();

      // Default ramp is a straight 2-point path: height at each end is the bottom/top height
      ramp->m_d.m_heightbottom = 10.f;
      ramp->m_d.m_heighttop = 90.f;
      const auto& points = ramp->m_curve.GetPoints();
      REQUIRE(points.size() == 2);
      const Vertex2D start(points[0]->GetX(), points[0]->GetY());
      const Vertex2D end(points[1]->GetX(), points[1]->GetY());

      CHECK(ramp->GetSurfaceHeight(start.x, start.y) == doctest::Approx(10.f));
      CHECK(ramp->GetSurfaceHeight(end.x, end.y) == doctest::Approx(90.f));
      CHECK(ramp->GetSurfaceHeight((start.x + end.x) * 0.5f, (start.y + end.y) * 0.5f) == doctest::Approx(50.f).epsilon(0.05));
   }

   SUBCASE("degenerate curve is handled safely")
   {
      Ramp* const ramp = Ramp::COMCreate();
      ramp->Init(0.f, 0.f, false);
      ramp->SetName("Ramp5");
      ramp->m_d.m_heightbottom = 10.f;
      ramp->m_d.m_heighttop = 90.f;
      table->AddPart(ramp);
      ramp->Release();

      // Empty curve: height queries report "not on ramp" and AddPoint appends a control point
      ramp->m_curve.ClearPoints();
      CHECK(ramp->GetSurfaceHeight(0.f, 0.f) == 0.f);
      ramp->AddPoint(Vertex2D(50.f, 50.f), true);
      REQUIRE(ramp->m_curve.GetPoints().size() == 1);
      CHECK(ramp->m_curve.GetPoints()[0]->GetX() == doctest::Approx(50.f));
      CHECK(ramp->m_curve.GetPoints()[0]->GetY() == doctest::Approx(50.f));
      CHECK(ramp->m_curve.GetPoints()[0]->GetZ() == doctest::Approx(10.f));

      // Physics setup over an empty curve adds no collider and does not crash
      ramp->m_curve.ClearPoints();
      PhysicsEngine physics(table);
      ramp->PhysicSetup(&physics, false);

      // Coincident control points produce a zero-length curve: generated heights stay finite
      ramp->m_curve.PushPoint(std::make_unique<DragPoint>(&ramp->m_curve, 10.f, 10.f, 0.f, true));
      ramp->m_curve.PushPoint(std::make_unique<DragPoint>(&ramp->m_curve, 10.f, 10.f, 0.f, true));
      int cvertex = -1;
      float* heights = nullptr;
      const Vertex2D* const verts = ramp->GetRampVertex(cvertex, &heights, nullptr, nullptr, nullptr, -1.f, true);
      for (int i = 0; i < cvertex; i++)
         CHECK(std::isfinite(heights[i]));
      delete[] heights;
      delete[] verts;
   }

   SUBCASE("editor transforms move the curve")
   {
      Ramp* const ramp = Ramp::COMCreate();
      ramp->Init(100.f, 100.f, false);
      ramp->SetName("Ramp4");
      table->AddPart(ramp);
      ramp->Release();

      const Vertex2D center = ramp->GetCenter();
      ramp->Translate(Vertex2D(10.f, -10.f));
      CHECK(ramp->GetCenter().x == doctest::Approx(center.x + 10.f));
      CHECK(ramp->GetCenter().y == doctest::Approx(center.y - 10.f));
   }

   table->Release();
}

namespace
{

// A straight or bent flat test ramp through the given points (bottom height 0, top 100, width 80)
Ramp* MakeSectionTestRamp(PinTable* table, const char* name, std::initializer_list<Vertex2D> points)
{
   Ramp* const ramp = Ramp::COMCreate();
   ramp->Init(0.f, 0.f, false);
   ramp->SetName(name);
   ramp->m_d.m_type = RampTypeFlat;
   ramp->m_d.m_heightbottom = 0.f;
   ramp->m_d.m_heighttop = 100.f;
   ramp->m_d.m_widthbottom = 80.f;
   ramp->m_d.m_widthtop = 80.f;
   ramp->m_d.m_leftwallheight = 62.f;
   ramp->m_d.m_rightwallheight = 62.f;
   ramp->m_d.m_leftwallheightvisible = 62.f;
   ramp->m_d.m_rightwallheightvisible = 62.f;
   ramp->m_curve.ClearPoints();
   for (const Vertex2D& p : points)
      ramp->m_curve.PushPoint(std::make_unique<DragPoint>(&ramp->m_curve, p.x, p.y, 0.f, false));
   table->AddPart(ramp);
   ramp->Release();
   return ramp;
}

struct Rows
{
   int n = 0;
   vector<Vertex2D> right, left;
   vector<float> height;
   vector<bool> wall; // empty: all rows have walls
};

Rows GetRows(const Ramp* ramp)
{
   Rows r;
   float* heights = nullptr;
   bool* walls = nullptr;
   const Vertex2D* const v = ramp->GetRampVertex(r.n, &heights, nullptr, nullptr, nullptr, HIT_SHAPE_DETAIL_LEVEL, true, &walls);
   for (int i = 0; i < r.n; i++)
   {
      r.right.push_back(v[i]);
      r.left.push_back(v[r.n * 2 - i - 1]);
      r.height.push_back(heights[i]);
      if (walls)
         r.wall.push_back(walls[i]);
   }
   delete[] v;
   delete[] heights;
   delete[] walls;
   return r;
}

bool HasTag(const InMemStream& stream, const char tag[4])
{
   const uint8_t* const d = stream.Data();
   for (size_t i = 0; i + 4 <= stream.Size(); i++)
      if (memcmp(d + i, tag, 4) == 0)
         return true;
   return false;
}

// Distinct points at the radius from the centre, on the given side of x = centre.x (sideX -1 / +1)
int CountDistinct(const vector<Vertex2D>& pts, const Vertex2D& centre, float radius, float sideX)
{
   vector<Vertex2D> found;
   for (const Vertex2D& p : pts)
      if (fabsf((p - centre).Length() - radius) < 0.01f && (p.x - centre.x) * sideX >= -0.01f
         && std::ranges::none_of(found, [&p](const Vertex2D& q) { return (p - q).Length() < 0.001f; }))
         found.push_back(p);
   return (int)found.size();
}

} // namespace

TEST_CASE("Ramp sections")
{
   PinTable* const table = CreateTestTable();

   SUBCASE("a 10.8 ramp record loads without sections and re-saves unchanged")
   {
      // The fields of a ramp as 10.8 writes them (Ramp::SaveData, ISelect::SaveData, then the points), no section record
      InMemStream stream10_8;
      {
         const ItemTypeEnum type = eItemRamp;
         stream10_8.Write(&type, sizeof(int));
         BiffWriter w(&stream10_8, nullptr);
         w.WriteFloat(FID(HTBT), 5.f);
         w.WriteFloat(FID(HTTP), 75.f);
         w.WriteFloat(FID(WDBT), 70.f);
         w.WriteFloat(FID(WDTP), 60.f);
         w.WriteString(FID(MATR), "Plastic"s);
         w.WriteBool(FID(TMON), false);
         w.WriteInt(FID(TMIN), 100);
         w.WriteInt(FID(TYPE), RampTypeFlat);
         w.WriteWideString(FID(NAME), L"Ramp108"s);
         w.WriteString(FID(IMAG), ""s);
         w.WriteInt(FID(ALGN), ImageModeWorld);
         w.WriteBool(FID(IMGW), true);
         w.WriteFloat(FID(WLHL), 62.f);
         w.WriteFloat(FID(WLHR), 62.f);
         w.WriteFloat(FID(WVHL), 30.f);
         w.WriteFloat(FID(WVHR), 30.f);
         w.WriteBool(FID(HTEV), false);
         w.WriteFloat(FID(THRS), 2.f);
         w.WriteFloat(FID(ELAS), 0.3f);
         w.WriteFloat(FID(RFCT), 0.3f);
         w.WriteFloat(FID(RSCT), 0.f);
         w.WriteBool(FID(CLDR), true);
         w.WriteBool(FID(RVIS), true);
         w.WriteFloat(FID(RADB), 0.f);
         w.WriteFloat(FID(RADI), 8.f);
         w.WriteFloat(FID(RADX), 38.f);
         w.WriteFloat(FID(RADY), 88.f);
         w.WriteBool(FID(REEN), true);
         w.WriteString(FID(MAPH), ""s);
         w.WriteBool(FID(OVPH), true);
         w.WriteBool(FID(LOCK), false);
         w.WriteInt(FID(LAYR), 0);
         w.WriteString(FID(LANR), "Layer_1"s);
         w.WriteBool(FID(LVIS), true);
         w.WriteInt(FID(PNTS), 0);
         const Vertex2D pts[3] = { { 500.f, 1500.f }, { 500.f, 1100.f }, { 700.f, 900.f } };
         for (const Vertex2D& p : pts)
         {
            w.BeginObject(FID(DPNT), true, false);
            w.WriteVector2(FID(VCEN), p);
            w.WriteFloat(FID(POSZ), 0.f);
            w.WriteBool(FID(SMTH), true);
            w.WriteBool(FID(SLNG), false);
            w.WriteBool(FID(ATEX), true);
            w.WriteFloat(FID(TEXC), 0.f);
            w.WriteBool(FID(LOCK), false);
            w.WriteInt(FID(LAYR), 0);
            w.WriteString(FID(LANR), "Layer_1"s);
            w.EndObject();
         }
         w.EndObject();
      }
      Ramp* const loaded = Ramp::COMCreate();
      LoadPartFromStream(loaded, stream10_8);
      CHECK(loaded->m_d.m_sections.empty());
      CHECK(loaded->m_curve.GetPoints().size() == 3);
      CHECK(loaded->m_d.m_heighttop == 75.f);
      table->AddPart(loaded);
      loaded->Release();

      // Plain geometry path: no wall flags, and the rows are those of the curve as before
      int cvertex = 0;
      bool* walls = reinterpret_cast<bool*>(1);
      const Vertex2D* const v = loaded->GetRampVertex(cvertex, nullptr, nullptr, nullptr, nullptr, HIT_SHAPE_DETAIL_LEVEL, true, &walls);
      CHECK(walls == nullptr);
      CHECK(cvertex > 3);
      delete[] v;

      // Re-save: no section record, and a second load/save generation is byte-identical (the 10.9 save itself adds
      // master's own format changes over the 10.8 bytes: shared fields, no PNTS / per point layer tags)
      const InMemStream saved1 = SavePartToStream(loaded);
      CHECK_FALSE(HasTag(saved1, "RSEC"));
      Ramp* const reloaded = Ramp::COMCreate();
      LoadPartFromStream(reloaded, saved1);
      CHECK(reloaded->m_d.m_sections.empty());
      CHECK(StreamsEqual(saved1, SavePartToStream(reloaded)));
      reloaded->Release();
   }

   SUBCASE("sections round-trip, copy for play, and are skipped by a reader that does not know them")
   {
      Ramp* const ramp = MakeSectionTestRamp(table, "RampSec1", { { 500.f, 1500.f }, { 500.f, 1000.f }, { 700.f, 1000.f }, { 700.f, 1500.f } });
      REQUIRE(ramp->AddEntryInterface());
      ramp->m_d.m_sections.back().m_flapBow = 7.f;
      ramp->m_d.m_sections.back().m_flapBowPos = 0.3f;
      REQUIRE(ramp->AddHairpin(1, 2));
      ramp->m_d.m_sections.back().m_radius = 55.f;
      ramp->m_d.m_sections.back().m_outerFaces = 12;
      ramp->m_d.m_sections.back().m_innerFaces = 9;
      const InMemStream saved = SavePartToStream(ramp);
      CHECK(HasTag(saved, "RSEC"));

      Ramp* const copy = Ramp::COMCreate();
      LoadPartFromStream(copy, saved);
      REQUIRE(copy->m_d.m_sections.size() == 2);
      CHECK(copy->m_d.m_sections == ramp->m_d.m_sections);
      CHECK(StreamsEqual(saved, SavePartToStream(copy)));
      copy->Release();

      Ramp* const live = ramp->CopyForPlay();
      CHECK(live->m_d.m_sections == ramp->m_d.m_sections);
      live->Release();

      // A reader without the RSEC case (an older build) skips each whole record and still reads the fields around it
      BiffReader reader(saved.Data() + sizeof(int), static_cast<uint32_t>(saved.Size() - sizeof(int)), CURRENT_FILE_FORMAT_VERSION, nullptr, 0);
      int points = 0, tagsAfter = 0;
      float top = 0.f;
      bool afterSections = false;
      reader.AsObject(
         [&](int tag, IObjectReader& r)
         {
            if (tag == FID(HTTP))
               top = r.AsFloat();
            else if (tag == FID(DPNT))
            {
               points++;
               r.AsObject([](int, IObjectReader&) { return true; });
            }
            else if (tag == FID(RSEC))
               afterSections = true; // not read: the reader skips the record
            else if (afterSections)
               tagsAfter++;
            return true;
         });
      CHECK_FALSE(reader.HasError());
      CHECK(top == 100.f);
      CHECK(points == 4);
      CHECK(tagsAfter == 0); // ENDB of the part ends the loop: nothing of the section leaked out as fields
   }

   SUBCASE("entry flap: replaces the ramp foot, starts ahead without walls, respects its caps")
   {
      Ramp* const ramp = MakeSectionTestRamp(table, "RampSec2", { { 500.f, 1500.f }, { 500.f, 1100.f } }); // 400 long, 100 high: 14 deg
      const Rows plain = GetRows(ramp);
      CHECK(plain.wall.empty());
      REQUIRE(ramp->AddEntryInterface());
      RampSection& flap = ramp->m_d.m_sections.back();
      flap.m_flapAhead = 60.f;
      flap.m_flapUp = 80.f;
      flap.m_flapBow = 6.f;
      flap.m_flapBowPos = 0.5f;
      Rows r = GetRows(ramp);
      REQUIRE(r.wall.size() == (size_t)r.n);
      // Front edge 60 ahead of the foot, on the foot level, no walls ahead of the foot
      CHECK(r.right[0].y == doctest::Approx(1560.f).epsilon(1e-4));
      CHECK(r.height[0] == doctest::Approx(0.f));
      CHECK_FALSE(r.wall[0]);
      CHECK(r.wall[r.n - 1]);
      // Lands 80 up the ramp on the ramp's own height there; past the landing the rows are the plain ramp's
      const vector<RampSectionStatus> st = ramp->GetSectionStatus();
      REQUIRE(st.size() == 1);
      CHECK(st[0].m_applied);
      CHECK(st[0].m_flapUp == doctest::Approx(80.f));
      CHECK(st[0].m_bow == doctest::Approx(6.f));
      for (int i = 0; i < r.n; i++)
         if (r.right[i].y < 1500.f - 80.f - 0.01f)
            CHECK(r.height[i] == doctest::Approx((1500.f - r.right[i].y) / 4.f).epsilon(1e-3));
      // Bow below the straight line between the flap ends, low point near the middle; never more than 5 below the foot
      float zMin = 0.f;
      for (int i = 0; i < r.n; i++)
         zMin = min(zMin, r.height[i]);
      CHECK(zMin >= -5.f - 1e-3f);

      // Parts placed on the ramp follow the flap (it fills the corner at the foot), and the plain ramp past the landing
      const RampSection flapCopy = flap;
      ramp->m_d.m_sections.clear();
      const float plainFoot = ramp->GetSurfaceHeight(500.f, 1500.f), plainUp = ramp->GetSurfaceHeight(500.f, 1300.f);
      REQUIRE(ramp->AddEntryInterface());
      ramp->m_d.m_sections.back() = flapCopy;
      const Rows flapRows = GetRows(ramp);
      float footRow = 0.f;
      for (int i = 0; i < flapRows.n; i++)
         if (fabsf(flapRows.right[i].y - 1500.f) < 0.01f)
            footRow = flapRows.height[i];
      CHECK(footRow > plainFoot + 1.f);
      CHECK(ramp->GetSurfaceHeight(500.f, 1500.f) == doctest::Approx(footRow).epsilon(1e-3));
      CHECK(ramp->GetSurfaceHeight(500.f, 1300.f) == doctest::Approx(plainUp).epsilon(1e-3));
      CHECK(ramp->GetSurfaceHeight(500.f, 1540.f) <= 0.5f);
      CHECK(ramp->GetSurfaceHeight(500.f, 1540.f) >= -5.f);

      // The front edge lies on the playfield even when the ramp starts above it
      ramp->m_d.m_heightbottom = 3.f;
      CHECK(GetRows(ramp).height[0] == doctest::Approx(0.f));
      ramp->m_d.m_heightbottom = 0.f;

      // A large bow is capped: front edge at most ~6 deg downhill, at most 5 VPU below the playfield
      ramp->m_d.m_sections.back().m_flapBow = 40.f;
      ramp->m_d.m_sections.back().m_flapBowPos = 0.1f;
      r = GetRows(ramp);
      const RampSectionStatus capped = ramp->GetSectionStatus()[0];
      CHECK(capped.m_bow < 40.f);
      CHECK(capped.m_entryAngle >= -6.f - 0.01f);
      CHECK(capped.m_sink <= 5.f + 1e-3f);
      for (int i = 0; i < r.n; i++)
         CHECK(r.height[i] >= -5.f - 1e-3f);
      const float slope0 = (r.height[1] - r.height[0]) / (r.right[0] - r.right[1]).Length();
      CHECK(slope0 >= -tanf(ANGTORAD(6.f)) - 0.01f);
   }

   SUBCASE("hairpin: concentric walls with their own face counts, no fold, legs apart, ranges follow inserts and deletes")
   {
      // A U-turn: two legs 200 apart, corners at points 1 and 2
      Ramp* const ramp = MakeSectionTestRamp(table, "RampSec3", { { 500.f, 1500.f }, { 500.f, 1000.f }, { 700.f, 1000.f }, { 700.f, 1500.f } });
      REQUIRE(ramp->AddHairpin(1, 2));
      RampSection& hp = ramp->m_d.m_sections.back();
      hp.m_radius = 60.f;
      hp.m_outerFaces = 12;
      hp.m_innerFaces = 9;
      vector<RampSectionStatus> st = ramp->GetSectionStatus();
      REQUIRE(st[0].m_applied);
      CHECK(st[0].m_radius == doctest::Approx(60.f));
      CHECK(st[0].m_legGap == doctest::Approx(200.f - 80.f).epsilon(1e-3));
      const Rows r = GetRows(ramp);
      vector<Vertex2D> all = r.right;
      all.insert(all.end(), r.left.begin(), r.left.end());
      // Arc centres: (560, 1060) and (640, 1060) (y down the table: the path turns at y = 1000)
      const Vertex2D c1(560.f, 1060.f), c2(640.f, 1060.f);
      // 12 outside faces over the two 90 deg corners = 6 + 6 (7 distinct points each), 9 inside = 5 + 4 (or 4 + 5)
      CHECK(CountDistinct(all, c1, 100.f, -1.f) == 7);
      CHECK(CountDistinct(all, c2, 100.f, 1.f) == 7);
      CHECK(CountDistinct(all, c1, 20.f, -1.f) + CountDistinct(all, c2, 20.f, 1.f) == 9 + 2);
      // No fold: every floor triangle keeps the orientation of the first one (or is degenerate where the inside repeats a point)
      float sign = 0.f;
      bool folded = false;
      for (int i = 0; i < r.n - 1; i++)
      {
         const Vertex2D tri[2][3] = { { r.left[i], r.right[i], r.left[i + 1] }, { r.left[i + 1], r.right[i], r.right[i + 1] } };
         for (const auto& t : tri)
         {
            const float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[1].y - t[0].y) * (t[2].x - t[0].x);
            if (fabsf(area) < 1e-3f)
               continue;
            if (sign == 0.f)
               sign = area > 0.f ? 1.f : -1.f;
            else if (area * sign < 0.f)
               folded = true;
         }
      }
      CHECK_FALSE(folded);

      // Heights outside the hairpin are the plain ramp's (the section only changes its own stretch), for the rows and for parts on the ramp
      {
         const vector<RampSection> keep = ramp->m_d.m_sections;
         ramp->m_d.m_sections.clear();
         const float plainLeg = ramp->GetSurfaceHeight(700.f, 1300.f), plainFoot = ramp->GetSurfaceHeight(500.f, 1300.f);
         ramp->m_d.m_sections = keep;
         CHECK(ramp->GetSurfaceHeight(700.f, 1300.f) == doctest::Approx(plainLeg).epsilon(1e-3));
         CHECK(ramp->GetSurfaceHeight(500.f, 1300.f) == doctest::Approx(plainFoot).epsilon(1e-3));
      }

      // Radius under half the width + 1 is refused; a radius larger than the legs allow is limited, then refused once under that
      hp.m_radius = 30.f;
      st = ramp->GetSectionStatus();
      CHECK_FALSE(st[0].m_applied);
      hp.m_radius = 150.f;
      st = ramp->GetSectionStatus();
      CHECK(st[0].m_applied);
      CHECK(st[0].m_radius == doctest::Approx(100.f)); // half the 200 between the corners
      ramp->m_curve.GetPoints()[2]->SetX(560.f); // legs 60 apart: under the ramp width
      st = ramp->GetSectionStatus();
      CHECK_FALSE(st[0].m_applied);
      CHECK_FALSE(st[0].m_message.empty());
      ramp->m_curve.GetPoints()[2]->SetX(700.f);

      // A point inserted on the bridge of the U joins the range and is a straight run, not a refusal
      ramp->m_curve.InsertPoint(2, std::make_unique<DragPoint>(&ramp->m_curve, 600.f, 1000.f, 0.f, false));
      CHECK(hp.m_first == 1);
      CHECK(hp.m_last == 3);
      CHECK(ramp->GetSectionStatus()[0].m_applied);
      ramp->m_curve.DeletePoint(ramp->m_curve.GetPoints()[2].get());
      CHECK(hp.m_last == 2);

      // Inserting a point before the hairpin moves its range with its points; deleting one of its points shrinks it
      ramp->m_curve.InsertPoint(1, std::make_unique<DragPoint>(&ramp->m_curve, 500.f, 1250.f, 0.f, false));
      CHECK(hp.m_first == 2);
      CHECK(hp.m_last == 3);
      ramp->m_curve.DeletePoint(ramp->m_curve.GetPoints()[3].get());
      REQUIRE(ramp->m_d.m_sections.size() == 1);
      CHECK(ramp->m_d.m_sections[0].m_first == 2);
      CHECK(ramp->m_d.m_sections[0].m_last == 2);
      ramp->m_curve.DeletePoint(ramp->m_curve.GetPoints()[2].get());
      CHECK(ramp->m_d.m_sections.empty());
   }

   SUBCASE("hairpin: a coarse outside wall is refined until the floor does not fold over the inside wall")
   {
      // Width 50, radius 150, one 90 deg corner, 1 outside face / 9 inside: the single outside chord would pass 123.7 from the
      // centre, inside the 125 inside wall
      Ramp* const ramp = MakeSectionTestRamp(table, "RampSec5", { { 500.f, 1500.f }, { 500.f, 1000.f }, { 1000.f, 1000.f } });
      ramp->m_d.m_widthbottom = ramp->m_d.m_widthtop = 50.f;
      REQUIRE(ramp->AddHairpin(1, 1));
      RampSection& hp = ramp->m_d.m_sections.back();
      hp.m_radius = 150.f;
      hp.m_outerFaces = 1;
      hp.m_innerFaces = 9;
      const vector<RampSectionStatus> st = ramp->GetSectionStatus();
      REQUIRE(st[0].m_applied);
      CHECK(st[0].m_outerFaces > 1);
      CHECK_FALSE(st[0].m_message.empty());
      const Rows r = GetRows(ramp);
      bool folded = false;
      for (int i = 0; i < r.n - 1; i++)
      {
         // the turn is clockwise or anticlockwise: every non degenerate floor triangle must have the same orientation
         const Vertex2D tri[4][3] = { { r.left[i], r.right[i], r.left[i + 1] }, { r.left[i + 1], r.right[i], r.right[i + 1] },
            { r.left[i], r.right[i], r.right[i + 1] }, { r.left[i], r.right[i + 1], r.left[i + 1] } };
         for (const auto& t : tri)
         {
            const float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[1].y - t[0].y) * (t[2].x - t[0].x);
            const float ref = (r.right[0].x - r.left[0].x) * (r.left[1].y - r.left[0].y) - (r.right[0].y - r.left[0].y) * (r.left[1].x - r.left[0].x);
            if (fabsf(area) > 1e-2f && area * ref < 0.f)
               folded = true;
         }
      }
      CHECK_FALSE(folded);
      // Before the fix: 1 outside face over the corner gives an inverted floor quad
      hp.m_outerFaces = 40;
      CHECK(ramp->GetSectionStatus()[0].m_outerFaces == 40);
   }

   SUBCASE("collision comes from the drawn rows: every hit triangle vertex is a row point at floor or wall top height")
   {
      Ramp* const ramp = MakeSectionTestRamp(table, "RampSec4", { { 500.f, 1500.f }, { 500.f, 1000.f }, { 700.f, 1000.f }, { 700.f, 1500.f } });
      REQUIRE(ramp->AddEntryInterface());
      ramp->m_d.m_sections.back().m_flapBow = 5.f;
      REQUIRE(ramp->AddHairpin(1, 2));
      ramp->m_d.m_sections.back().m_radius = 60.f;
      const Rows r = GetRows(ramp);
      for (const PhysicProfile profile : { PhysicProfile::V10_8, PhysicProfile::V10_9 })
      {
         table->m_physicProfile = profile;
         PhysicsEngine physics(table);
         vector<HitObject*> hits;
         physics.CollectColliders(ramp, &hits, false);
         int triangles = 0, wallTriangles = 0, outside = 0;
         for (HitObject* ho : hits)
         {
            if (const HitTriangle* tri = dynamic_cast<HitTriangle*>(ho))
            {
               triangles++;
               const bool wall = fabsf(tri->m_normal.z) < 1e-3f;
               wallTriangles += wall ? 1 : 0;
               for (const Vertex3Ds& p : tri->m_rgv)
               {
                  bool found = false;
                  for (int i = 0; i < r.n && !found; i++)
                     for (const Vertex2D& s : { r.right[i], r.left[i] })
                        if ((Vertex2D(p.x, p.y) - s).Length() < 1e-3f
                           && (fabsf(p.z - r.height[i]) < 1e-3f || (r.wall[i] && fabsf(p.z - r.height[i] - 62.f) < 1e-3f)))
                           found = true;
                  outside += found ? 0 : 1;
               }
            }
            delete ho;
         }
         ramp->PhysicRelease(&physics, false);
         CHECK(triangles > 0);
         CHECK(outside == 0);
         if (profile == PhysicProfile::V10_9)
            CHECK(wallTriangles > 0);
      }
   }

   table->Release();
}
