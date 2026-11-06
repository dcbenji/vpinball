// Standalone check of src/math/HoleClassify.h: clang++ -std=c++20 -I src tools/holeclassify_check.cpp -o /tmp/hcc && /tmp/hcc  (also with -O2 -ffast-math, as the engine builds)
// argv[1] = "real <file.raw>": Find Holes on a real mask (raw dump: "w h channels\n" then 8-bit pixels, see masktrace_check.cpp) and the class counts, with no
//   footprints (everything through) and with synthetic insert cards (a hull around every small round hole), to see the classifier on real shapes.
#include "math/HoleClassify.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace MaskTrace;
using namespace HoleClassify;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d %s: ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Hole round_(float x, float y, float d)
{
   Hole h; h.type = HoleType::Round; h.x = x; h.y = y; h.diameter = d;
   return h;
}
static Hole slot_(float x, float y, float x2, float y2, float d)
{
   Hole h; h.type = HoleType::Slot; h.x = x; h.y = y; h.x2 = x2; h.y2 = y2; h.diameter = d;
   return h;
}
static Hole poly_(std::vector<Pt> p) // as Trace gives it: clockwise (shoelace < 0)
{
   double a = 0;
   for (size_t i = 0; i < p.size(); i++) a += (double)p[i].x * p[(i + 1) % p.size()].y - (double)p[(i + 1) % p.size()].x * p[i].y;
   if (a > 0) std::reverse(p.begin(), p.end());
   Hole h; h.type = HoleType::Polygon; h.polygon = p;
   double cx = 0, cy = 0;
   for (const Pt& q : p) cx += q.x, cy += q.y;
   h.x = (float)(cx / p.size()), h.y = (float)(cy / p.size());
   return h;
}
static std::vector<Pt> box(float x0, float y0, float x1, float y1) { return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } }; } // shoelace > 0 (counter-clockwise by the board's convention)

static void synthetic()
{
   MaskTrace::Result t;
   t.holes.push_back(round_(50, 50, 20));                                   // 0: centre inside card A
   t.holes.push_back(round_(200, 50, 20));                                  // 1: well away from every card
   t.holes.push_back(poly_({ { 100, 100 }, { 160, 100 }, { 160, 112 }, { 112, 112 }, { 112, 160 }, { 100, 160 } })); // 2: L, arms 12 wide, 1152 area: card B covers the horizontal arm (62%) but not the shape's centre point
   t.holes.push_back(slot_(300, 50, 340, 50, 12));                          // 3: slot, centre (320, 50) inside card C
   t.holes.push_back(round_(500, 500, 30));                                 // 4: a light's centre lies inside
   t.holes.push_back(round_(600, 500, 30));                                 // 5: a light just outside its edge
   t.holes.push_back(poly_({ { 700, 500 }, { 760, 500 }, { 760, 560 }, { 740, 560 }, { 740, 520 }, { 720, 520 }, { 720, 560 }, { 700, 560 } })); // 6: U shape, light in the gap
   t.holes.push_back(round_(50, 300, 20));                                  // 7: half-covered by a card (centre outside, exactly under half) -> through
   t.holes.push_back(round_(250, 300, 20));                                 // 8: no card near it
   t.outlineCuts.push_back({ 0.f, 0.f, 10.f, 10.f });
   t.outlineCuts.push_back({ 900.f, 0.f, 950.f, 30.f });

   Footprints fp;
   fp.hulls.push_back(box(30, 30, 70, 70));                                 // A covers hole 0's centre (and all of it)
   fp.hulls.push_back(box(95, 95, 165, 140));                               // B: covers the L's horizontal arm and 28 of the 48 of its vertical arm below it: well over half
   fp.hulls.push_back(box(310, 40, 330, 60));                               // C over the slot's centre only
   // D: hole 7 spans x 40..60; a hull from x = 51 covers a sliver under half of it and not its centre -> through
   fp.hulls.push_back(box(51, 280, 70, 320));
   fp.lightCentres.push_back({ 500, 500 });                                 // inside hole 4
   fp.lightCentres.push_back({ 618, 500 });                                 // hole 5 spans x 585..615: outside
   fp.lightCentres.push_back({ 730, 540 });                                 // in the U's gap (between x 720 and 740, y > 520): outside the polygon
   Classes r = Classify(t, fp);
   CHECK(r.kind.size() == 9 && r.why.size() == 9 && r.card.size() == 9, "sizes");
   CHECK(r.kind[0] == Kind::InsertPocket && r.why[0] == Why::Card && r.card[0] == 0, "centre under a card");
   CHECK(r.kind[1] == Kind::Through && r.why[1] == Why::None && r.card[1] == -1, "far from everything");
   CHECK(r.kind[2] == Kind::InsertPocket && r.why[2] == Why::Card && r.card[2] == 1, "more than half under a card, centre outside it: kind %d", (int)r.kind[2]);
   CHECK(r.kind[3] == Kind::InsertPocket && r.card[3] == 2, "slot centre under a card");
   CHECK(r.kind[4] == Kind::InsertPocket && r.why[4] == Why::Light && r.card[4] == -1, "light centre inside a round hole");
   CHECK(r.kind[5] == Kind::Through, "light just outside");
   CHECK(r.kind[6] == Kind::Through, "light in the gap of a U-shaped hole must not count (even-odd)");
   CHECK(r.kind[7] == Kind::Through, "just under half covered: through, got %d", (int)r.kind[7]);
   CHECK(r.kind[8] == Kind::Through, "hole 8");
   CHECK(r.pockets == 4 && r.throughs == 5 && r.byCard == 3 && r.byLight == 1 && r.edgeCuts == 2 && r.haveEvidence, "counts %d %d %d %d %d", r.pockets, r.throughs, r.byCard, r.byLight, r.edgeCuts);
   CHECK(r.pockets + r.throughs == (int)t.holes.size(), "every hole is one or the other");

   // a card winding in either direction gives the same answer
   Footprints cw = fp;
   for (auto& h : cw.hulls) std::reverse(h.begin(), h.end());
   Classes rc = Classify(t, cw);
   CHECK(rc.kind == r.kind && rc.card == r.card, "clockwise hulls differ");

   // card beats light; a shape under a card and over a light is a Card pocket
   Footprints both;
   both.hulls.push_back(box(30, 30, 70, 70));
   both.lightCentres.push_back({ 50, 50 });
   Classes rb = Classify(t, both);
   CHECK(rb.kind[0] == Kind::InsertPocket && rb.why[0] == Why::Card && rb.byCard == 1 && rb.byLight == 0, "card should win");

   // the first hull that matches is reported
   Footprints two;
   two.hulls.push_back(box(900, 900, 910, 910));
   two.hulls.push_back(box(30, 30, 70, 70));
   two.hulls.push_back(box(20, 20, 80, 80));
   CHECK(Classify(t, two).card[0] == 1, "first matching hull");

   // nothing given: everything is a through hole, no evidence
   Classes r0 = Classify(t, Footprints());
   CHECK(r0.pockets == 0 && r0.throughs == 9 && !r0.haveEvidence && r0.edgeCuts == 2, "no footprints");
   // degenerate and non-finite input is ignored, never crashes, never matches
   Footprints junk;
   junk.hulls.push_back({});
   junk.hulls.push_back({ { 0, 0 }, { 10, 10 } });
   junk.hulls.push_back({ { 0, 0 }, { 10, 10 }, { 20, 20 } }); // a line: zero area
   volatile uint32_t infBits = 0x7f800000u, nanBits = 0x7fc00000u; // built from bits: -ffast-math makes the usual NaN / inf spellings undefined
   float inf, nan;
   { const uint32_t a = infBits, b = nanBits; memcpy(&inf, &a, 4); memcpy(&nan, &b, 4); }
   junk.hulls.push_back({ { 30, 30 }, { inf, 30 }, { 70, 70 } });
   junk.hulls.push_back({ { 30, 30 }, { 70, nan }, { 70, 70 } });
   junk.lightCentres.push_back({ nan, 50 });
   junk.lightCentres.push_back({ 50, inf });
   Classes rj = Classify(t, junk);
   CHECK(rj.pockets == 0 && !rj.haveEvidence, "junk input: %d pockets, evidence %d", rj.pockets, (int)rj.haveEvidence);
   // empty trace
   Classes re = Classify(MaskTrace::Result(), fp);
   CHECK(re.kind.empty() && re.pockets == 0 && re.throughs == 0 && re.edgeCuts == 0, "empty trace");
   // a polygon with fewer than 3 points never contains a light
   MaskTrace::Result bad; bad.holes.push_back(poly_({ { 0, 0 }, { 5, 5 } }));
   Footprints l; l.lightCentres.push_back({ 1, 1 });
   CHECK(Classify(bad, l).pockets == 0, "degenerate polygon");

   // PointInHole directly
   CHECK(PointInHole(round_(0, 0, 10), 3, 4) && !PointInHole(round_(0, 0, 10), 3.1f, 4.1f), "round boundary");
   CHECK(PointInHole(slot_(0, 0, 100, 0, 10), 50, 4.9f) && !PointInHole(slot_(0, 0, 100, 0, 10), 50, 5.1f) && PointInHole(slot_(0, 0, 100, 0, 10), -4.9f, 0) && !PointInHole(slot_(0, 0, 100, 0, 10), -5.1f, 0), "slot");
   CHECK(Finite(1.f) && !Finite(inf) && !Finite(nan), "Finite is a bit test");
}

// Trace a drawn mask and classify it: the P2-3 "Done" scene in miniature (rectangle board, two notches touching the edge, round holes, polygons, shapes under cards).
static void endToEnd()
{
   const int W = 400, H = 600;
   std::vector<uint8_t> g((size_t)W * H, 255);
   const auto rect = [&](int x0, int y0, int x1, int y1) { for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) g[(size_t)y * W + x] = 0; };
   const auto disc = [&](double cx, double cy, double r) { for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if ((x + .5 - cx) * (x + .5 - cx) + (y + .5 - cy) * (y + .5 - cy) <= r * r) g[(size_t)y * W + x] = 0; };
   rect(0, 0, 60, 40);      // notch touching the top-left corner
   rect(340, 300, 400, 380); // notch off the right edge
   for (int i = 0; i < 6; i++) disc(80 + 50 * i, 100, 15); // 6 round holes
   rect(100, 200, 140, 240); // 2 polygons (squares)
   rect(200, 200, 260, 220);
   disc(100, 400, 18); disc(200, 420, 18); disc(300, 440, 18); // 3 shapes under cards
   Image im; im.data = g.data(); im.w = W; im.h = H; im.channels = 1;
   Params p; p.left = 0.f, p.top = 0.f, p.right = 400.f, p.bottom = 600.f;
   MaskTrace::Result t;
   CHECK(Trace(im, p, t), "trace");
   Footprints fp;
   fp.hulls.push_back(ConvexHull(box(80, 380, 120, 420)));
   fp.hulls.push_back(ConvexHull(box(180, 400, 220, 440)));
   fp.hulls.push_back(ConvexHull(box(280, 420, 320, 460)));
   Classes r = Classify(t, fp);
   int round = 0;
   for (const MaskTrace::Hole& h : t.holes) round += h.type == HoleType::Round;
   printf("end to end: %zu traced shapes (%d round), pockets %d, through %d, edge cuts %d\n", t.holes.size(), round, r.pockets, r.throughs, r.edgeCuts);
   CHECK(t.holes.size() == 11 && round == 9, "traced %zu / round %d", t.holes.size(), round);
   CHECK(r.pockets == 3 && r.throughs == 8 && r.edgeCuts == 2 && r.byCard == 3 && r.haveEvidence, "counts %d %d %d", r.pockets, r.throughs, r.edgeCuts);
   for (size_t i = 0; i < t.holes.size(); i++)
      CHECK((r.kind[i] == Kind::InsertPocket) == (t.holes[i].y > 380.f), "hole %zu at (%g, %g) kind %d", i, t.holes[i].x, t.holes[i].y, (int)r.kind[i]);
}

static void real(const char* path)
{
   FILE* f = fopen(path, "rb");
   if (!f) { printf("cannot open %s\n", path); fails++; return; }
   int w = 0, h = 0, c = 0;
   if (fscanf(f, "%d %d %d\n", &w, &h, &c) != 3) { fclose(f); fails++; return; }
   std::vector<uint8_t> d((size_t)w * h * c);
   if (fread(d.data(), 1, d.size(), f) != d.size()) { fclose(f); fails++; return; }
   fclose(f);
   Image im; im.data = d.data(); im.w = w; im.h = h; im.channels = c;
   Params p; p.left = 0.f, p.top = 0.f, p.right = 952.94f, p.bottom = (float)(952.94 * h / w);
   MaskTrace::Result t;
   CHECK(Trace(im, p, t), "trace");
   Classes none = Classify(t, Footprints());
   printf("%s: %zu shapes, no footprints: pockets %d through %d edge cuts %d\n", path, t.holes.size(), none.pockets, none.throughs, none.edgeCuts);
   CHECK(none.pockets == 0 && none.throughs == (int)t.holes.size(), "no footprints must give all through holes");
   // synthetic cards: a square hull 1.3x the size around every round hole under 40 VPU across, and an Insert Light centre on every polygon under 40 VPU
   Footprints fp;
   int cards = 0, lights = 0;
   for (const MaskTrace::Hole& hl : t.holes)
   {
      if (hl.type == HoleType::Round && hl.diameter < 40.f)
      {
         const float r = 0.65f * hl.diameter;
         fp.hulls.push_back(ConvexHull(box(hl.x - r, hl.y - r, hl.x + r, hl.y + r)));
         cards++;
      }
      else if (hl.type == HoleType::Polygon && hl.maxX - hl.minX < 40.f && hl.maxY - hl.minY < 40.f && PointInHole(hl, hl.x, hl.y))
      {
         fp.lightCentres.push_back({ hl.x, hl.y });
         lights++;
      }
   }
   Classes r = Classify(t, fp);
   printf("  synthetic evidence: %d cards (hull around every round hole < 40 VPU), %d lights (centroid of every small polygon whose centroid is inside it): pockets %d (card %d, light %d) through %d\n", cards, lights, r.pockets, r.byCard, r.byLight, r.throughs);
   CHECK(r.pockets >= cards + lights && r.pockets + r.throughs == (int)t.holes.size(), "every synthetic card and light must make a pocket");
}

int main(int argc, char** argv)
{
   synthetic();
   endToEnd();
   if (argc > 2 && !strcmp(argv[1], "real")) real(argv[2]);
   printf(fails ? "FAILED: %d\n" : "all checks passed\n", fails);
   return fails ? 1 : 0;
}
