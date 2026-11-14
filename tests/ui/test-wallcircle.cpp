// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "ui/live/editor/ops/CircleMath.h"

#include "doctest.h"

using namespace VPX::EditorUI::CircleMath;

namespace
{
   // Outline of a 100 x 100 wall (positive winding), and the same outline the other way round
   const vector<Vertex2D> square { { 0.f, 0.f }, { 100.f, 0.f }, { 100.f, 100.f }, { 0.f, 100.f } };
   const vector<Vertex2D> squareReversed { { 0.f, 0.f }, { 0.f, 100.f }, { 100.f, 100.f }, { 100.f, 0.f } };
   const vector<int> all4 { 0, 1, 2, 3 };

   void CheckPoint(const Vertex2D &p, const float x, const float y)
   {
      CHECK(p.x == doctest::Approx(x).epsilon(1e-4));
      CHECK(p.y == doctest::Approx(y).epsilon(1e-4));
   }
}

TEST_CASE("Wall circle")
{
   SUBCASE("square to circle")
   {
      Fit fit;
      REQUIRE(FitCircle(square, all4, fit));
      CheckPoint(fit.center, 50.f, 50.f);
      CHECK(fit.radius == doctest::Approx(50.f * sqrtf(2.f)).epsilon(1e-5));
      CHECK(fit.startAngle == doctest::Approx(-0.75f * M_PIf).epsilon(1e-5));
      const vector<Vertex2D> circle = Circle(fit.center, fit.radius, 12, fit.startAngle, true);
      REQUIRE(circle.size() == 12);
      CheckPoint(circle[0], 0.f, 0.f); // First point stays at the first selected point
      for (const Vertex2D &p : circle)
         CHECK((p - fit.center).Length() == doctest::Approx(fit.radius).epsilon(1e-4));
      // 4 faces give the square back
      const vector<Vertex2D> four = Circle(fit.center, fit.radius, 4, fit.startAngle, true);
      REQUIRE(four.size() == 4);
      for (size_t i = 0; i < 4; i++)
         CheckPoint(four[i], square[i].x, square[i].y);
   }

   SUBCASE("regular spacing")
   {
      const vector<Vertex2D> circle = Circle({ 10.f, -20.f }, 30.f, 8, 0.f, true);
      REQUIRE(circle.size() == 8);
      CheckPoint(circle[0], 40.f, -20.f);
      CheckPoint(circle[2], 10.f, 10.f);
      for (size_t i = 0; i < circle.size(); i++)
         CHECK((circle[(i + 1) % circle.size()] - circle[i]).Length() == doctest::Approx(2.f * 30.f * sinf(M_PIf / 8.f)).epsilon(1e-4));
   }

   SUBCASE("winding is preserved")
   {
      CHECK(SignedArea(square) == doctest::Approx(10000.f));
      CHECK(SignedArea(squareReversed) == doctest::Approx(-10000.f));
      for (const vector<Vertex2D> *outline : { &square, &squareReversed })
      {
         Fit fit;
         REQUIRE(FitCircle(*outline, all4, fit));
         const bool positive = SignedArea(*outline) >= 0.f;
         const vector<Vertex2D> circle = Circle(fit.center, fit.radius, 12, fit.startAngle, positive);
         CHECK((SignedArea(circle) > 0.f) == positive);
         // Area of the regular dodecagon: 3 r^2
         CHECK(fabsf(SignedArea(circle)) == doctest::Approx(3.f * fit.radius * fit.radius).epsilon(1e-4));
      }
      const vector<Vertex2D> four = Circle({ 50.f, 50.f }, 50.f * sqrtf(2.f), 4, -0.75f * M_PIf, false);
      for (size_t i = 0; i < 4; i++)
         CheckPoint(four[i], squareReversed[i].x, squareReversed[i].y);
   }

   SUBCASE("face count is clamped")
   {
      CHECK(Circle({ 0.f, 0.f }, 10.f, 2, 0.f, true).size() == MIN_FACES);
      CHECK(Circle({ 0.f, 0.f }, 10.f, 0, 0.f, true).size() == MIN_FACES);
      CHECK(Circle({ 0.f, 0.f }, 10.f, 3, 0.f, true).size() == 3);
      CHECK(Circle({ 0.f, 0.f }, 10.f, 1000, 0.f, true).size() == MAX_FACES);
   }

   SUBCASE("fit to some of the points")
   {
      // 3 corners of the square: the circle through them, first point at the first of them
      Fit fit;
      REQUIRE(FitCircle(square, { 1, 2, 3 }, fit));
      CheckPoint(fit.center, 50.f, 50.f);
      CHECK(fit.radius == doctest::Approx(50.f * sqrtf(2.f)).epsilon(1e-5));
      CheckPoint(Circle(fit.center, fit.radius, 12, fit.startAngle, true)[0], 100.f, 0.f);
      // 3 points on a short arc of a circle (all on one side of its center)
      vector<Vertex2D> arc;
      for (const float a : { 0.f, 0.3f, 0.6f })
         arc.emplace_back(10.f + 300.f * cosf(a), -20.f + 300.f * sinf(a));
      REQUIRE(FitCircle(arc, { 0, 1, 2 }, fit));
      CheckPoint(fit.center, 10.f, -20.f);
      CHECK(fit.radius == doctest::Approx(300.f).epsilon(1e-4));
      // 5 points of an irregular outline on a circle
      vector<Vertex2D> outline;
      for (const float a : { 0.f, 1.f, 1.5f, 3.f, 4.5f })
         outline.emplace_back(-40.f + 25.f * cosf(a), 70.f + 25.f * sinf(a));
      REQUIRE(FitCircle(outline, { 0, 1, 2, 3, 4 }, fit));
      CheckPoint(fit.center, -40.f, 70.f);
      CHECK(fit.radius == doctest::Approx(25.f).epsilon(1e-4));
   }

   SUBCASE("points in line fall back to their average")
   {
      const vector<Vertex2D> line { { 0.f, 0.f }, { 10.f, 0.f }, { 20.f, 0.f } };
      Fit fit;
      REQUIRE(FitCircle(line, { 0, 1, 2 }, fit));
      CheckPoint(fit.center, 10.f, 0.f);
      CHECK(fit.radius == doctest::Approx(20.f / 3.f));
   }

   SUBCASE("degenerate input is refused")
   {
      Fit fit;
      const vector<Vertex2D> coincident { { 5.f, 5.f }, { 5.f, 5.f }, { 5.f, 5.f }, { 5.f, 5.f } };
      CHECK(!FitCircle(coincident, all4, fit));
      CHECK(!FitCircle(square, { 0, 1 }, fit)); // Fewer than 3 points
      CHECK(!FitCircle(square, {}, fit));
   }
}
