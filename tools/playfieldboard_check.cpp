// Standalone check of src/math/PlayfieldBoard:
//   clang++ -std=c++20 -I src tools/playfieldboard_check.cpp src/math/PlayfieldBoard.cpp src/math/PlasticMesh.cpp -o /tmp/pbc && /tmp/pbc [--exact] [fixture ...]
// Also build it with -O2 -ffast-math (the engine's flags).
// Each fixture (tools/fixtures/*.pfboard.txt) was dumped by the editor's board build before it moved into PlayfieldBoard: the table rectangle, the board
// settings, every hole and the meshes that build produced. The check rebuilds from the holes and compares with a 1e-5 tolerance (--exact: bit for bit; clang and gcc differ by ~1e-7 from float contraction, g++ -O2 -ffast-math gave 0) and reports the largest difference.
#include "math/PlayfieldBoard.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace PlayfieldBoard;
static int fails = 0;
static double gTol = 1e-5; // --exact: 0, a bit-for-bit comparison
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Fixture
{
   float rect[4] = {};
   float thick = 0.f;
   int detail = 1, floorMode = 0;
   float uvm[6] = {};
   std::vector<BoardHole> holes;
   std::vector<char> overlap;
   PlasticMesh::Mesh board, walls, floor;
};

static bool ReadMesh(FILE* f, PlasticMesh::Mesh& m)
{
   char tag[32], name[32];
   size_t nv = 0, ni = 0;
   if (fscanf(f, "%31s %31s %zu %zu", tag, name, &nv, &ni) != 4 || strcmp(tag, "mesh") != 0)
      return false;
   m.verts.resize(nv);
   for (PlasticMesh::Vertex& v : m.verts)
      if (fscanf(f, " V %f %f %f %f %f %f %f %f", &v.x, &v.y, &v.z, &v.nx, &v.ny, &v.nz, &v.tu, &v.tv) != 8)
         return false;
   m.idx.resize(ni);
   for (size_t i = 0; i < ni; i += 3)
      if (fscanf(f, " T %u %u %u", &m.idx[i], &m.idx[i + 1], &m.idx[i + 2]) != 3)
         return false;
   return true;
}

static bool Load(const char* path, Fixture& fx)
{
   FILE* f = fopen(path, "r");
   if (!f)
      return false;
   int ver = 0;
   size_t n = 0;
   bool ok = fscanf(f, " PFBOARD %d", &ver) == 1 && ver == 1 && fscanf(f, " rect %f %f %f %f", &fx.rect[0], &fx.rect[1], &fx.rect[2], &fx.rect[3]) == 4
      && fscanf(f, " params %f %d %d", &fx.thick, &fx.detail, &fx.floorMode) == 3 && fscanf(f, " uvm %f %f %f %f %f %f", &fx.uvm[0], &fx.uvm[1], &fx.uvm[2], &fx.uvm[3], &fx.uvm[4], &fx.uvm[5]) == 6
      && fscanf(f, " holes %zu", &n) == 1;
   for (size_t i = 0; ok && i < n; i++)
   {
      BoardHole h;
      int overlap = 0;
      size_t np = 0;
      ok = fscanf(f, " H %d %d %f %f %f %f %f %d %zu", &h.kind, &h.flags, &h.cx, &h.cy, &h.diameter, &h.ex, &h.ey, &overlap, &np) == 9;
      h.polygon.resize(np * 2);
      for (size_t k = 0; ok && k < np * 2; k++)
         ok = fscanf(f, " %f", &h.polygon[k]) == 1;
      fx.holes.push_back(std::move(h));
      fx.overlap.push_back((char)overlap);
   }
   ok = ok && ReadMesh(f, fx.board) && ReadMesh(f, fx.walls) && ReadMesh(f, fx.floor);
   fclose(f);
   return ok;
}

// Largest component difference between two meshes; -1 when the vertex or index counts or any index differ
static double Compare(const PlasticMesh::Mesh& a, const PlasticMesh::Mesh& b)
{
   if (a.verts.size() != b.verts.size() || a.idx != b.idx)
      return -1.;
   double worst = 0.;
   for (size_t i = 0; i < a.verts.size(); i++)
   {
      const float p[8] = { a.verts[i].x, a.verts[i].y, a.verts[i].z, a.verts[i].nx, a.verts[i].ny, a.verts[i].nz, a.verts[i].tu, a.verts[i].tv };
      const float q[8] = { b.verts[i].x, b.verts[i].y, b.verts[i].z, b.verts[i].nx, b.verts[i].ny, b.verts[i].nz, b.verts[i].tu, b.verts[i].tv };
      for (int k = 0; k < 8; k++)
         worst = std::max(worst, (double)fabsf(p[k] - q[k]));
   }
   return worst;
}

static double CheckFixture(const char* path)
{
   Fixture fx;
   if (FILE* const probe = fopen(path, "r"))
      fclose(probe);
   else
   {
      printf("%s: not present, skipped (the large fixtures are fork-only)\n", path);
      return 0.;
   }
   if (!Load(path, fx))
   {
      CHECK(false, "cannot read fixture %s", path);
      return 0.;
   }
   BoardParams p;
   p.outline = { fx.rect[0], fx.rect[1], fx.rect[2], fx.rect[1], fx.rect[2], fx.rect[3], fx.rect[0], fx.rect[3] };
   p.thickness = fx.thick;
   p.detail = fx.detail;
   for (int i = 0; i < 6; i++)
      p.uvm[i] = fx.uvm[i];
   std::vector<BoardHole> holes = fx.holes;
   if (fx.floorMode != 0)
      for (BoardHole& h : holes)
         h.flags &= ~kFallsThrough; // Keep Existing floor: what the editor does
   BoardMeshes m;
   BoardReport r;
   CHECK(BuildBoard(p, holes, m, r), "%s: BuildBoard failed", path);
   PlasticMesh::Mesh walls = m.walls; // The editor appends the backing to the walls part
   const unsigned base = (unsigned)walls.verts.size();
   walls.verts.insert(walls.verts.end(), m.backing.verts.begin(), m.backing.verts.end());
   for (unsigned i : m.backing.idx)
      walls.idx.push_back(base + i);
   const double dt = Compare(m.top, fx.board), dw = Compare(walls, fx.walls), df = Compare(m.floor, fx.floor);
   CHECK(dt >= 0., "%s: top differs in vertex or index counts (%zu/%zu verts, %zu/%zu idx)", path, m.top.verts.size(), fx.board.verts.size(), m.top.idx.size(), fx.board.idx.size());
   CHECK(dw >= 0., "%s: walls+backing differ in vertex or index counts (%zu/%zu verts)", path, walls.verts.size(), fx.walls.verts.size());
   CHECK(df >= 0., "%s: floor differs in vertex or index counts (%zu/%zu verts)", path, m.floor.verts.size(), fx.floor.verts.size());
   const double worst = std::max({ dt, dw, df });
   CHECK(worst <= gTol, "%s: largest difference %g > %g", path, worst, gTol);
   CHECK(r.overlap == fx.overlap, "%s: overlap flags differ", path);
   printf("%s: %zu holes, %d built, %d overlapping; top %zu, walls %zu, backing %zu, floor %zu triangles; largest difference top %.3g walls+backing %.3g floor %.3g\n", path, holes.size(), r.built, r.overlapping, r.topTriangles,
      r.wallTriangles, r.backingTriangles, r.floorTriangles, dt, dw, df);
   return worst;
}

static BoardHole Round(float x, float y, float d, int flags = kCut)
{
   BoardHole h;
   h.kind = kRound;
   h.flags = flags;
   h.cx = x;
   h.cy = y;
   h.diameter = d;
   return h;
}

// Synthetic cases that need no fixture: counts, the overlap rules, a hole beyond the edge, a non-rectangular outline, degenerate input
static void Synthetic()
{
   BoardParams p;
   p.outline = { 0, 0, 100, 0, 100, 200, 0, 200 };
   p.thickness = 10.f;
   p.uvm[0] = 0.01f;
   p.uvm[4] = 0.005f;
   std::vector<BoardHole> holes = { Round(30, 30, 10), Round(34, 30, 10), Round(70, 100, 12, kCut | kFallsThrough), Round(3, 100, 10), Round(50, 150, 8, 0) };
   BoardMeshes m;
   BoardReport r;
   CHECK(BuildBoard(p, holes, m, r), "synthetic build");
   CHECK(r.overlap == std::vector<char>({ 1, 1, 0, 1, 0 }), "overlap flags: two touching holes, one over the edge, one uncut");
   CHECK(r.built == 1 && r.overlapping == 3, "built %d overlapping %d", r.built, r.overlapping);
   CHECK(!m.top.idx.empty() && !m.walls.idx.empty() && !m.backing.idx.empty() && !m.floor.idx.empty(), "meshes empty");
   CHECK(m.floor.idx.size() > 6, "the Falls Through hole is cut from the floor");
   CHECK(m.backing.idx == m.floor.idx && m.backing.verts.size() == m.floor.verts.size(), "backing follows the floor");
   for (const PlasticMesh::Vertex& v : m.backing.verts)
      CHECK(v.z == -p.thickness - 0.5f, "backing depth");

   // A hexagonal board: a hole inside builds, one reaching out through the slanted edge is left out
   BoardParams hex;
   hex.outline = { 50, 0, 100, 25, 100, 75, 50, 100, 0, 75, 0, 25 };
   hex.thickness = 10.f;
   hex.uvm[0] = 0.01f;
   hex.uvm[4] = 0.01f;
   std::vector<BoardHole> hh = { Round(50, 50, 20), Round(8, 12, 14) };
   BoardMeshes hm;
   BoardReport hr;
   CHECK(BuildBoard(hex, hh, hm, hr), "hexagon build");
   CHECK(hr.overlap == std::vector<char>({ 0, 1 }), "hexagon: the hole through the slanted edge is left out (%d %d)", hr.overlap[0], hr.overlap[1]);
   CHECK(hr.built == 1, "hexagon built %d", hr.built);

   BoardParams bad = p;
   bad.outline = { 0, 0, 100, 0 };
   BoardMeshes bm;
   BoardReport br;
   CHECK(!BuildBoard(bad, holes, bm, br), "an outline of two points fails");
   bad.outline = { 0, 0, 0, 0, 0, 200, 0, 200 };
   CHECK(!BuildBoard(bad, holes, bm, br), "a zero-width rectangle fails");
   CHECK(bm.top.verts.empty(), "meshes untouched on failure");
}

int main(int argc, char** argv)
{
   std::vector<const char*> files;
   for (int i = 1; i < argc; i++)
      if (strcmp(argv[i], "--exact") == 0)
         gTol = 0.;
      else
         files.push_back(argv[i]);
   if (files.empty())
   {
      files.push_back("tools/fixtures/pf-maker.pfboard.txt");
      files.push_back("tools/fixtures/blank-maker.pfboard.txt");
      files.push_back("tools/fixtures/pf-maker-allcut.pfboard.txt");
   }
   double worst = 0.;
   for (const char* f : files)
      worst = std::max(worst, CheckFixture(f));
   printf("largest difference over all fixtures: %.3g (tolerance %g)\n", worst, gTol);
   Synthetic();
   printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
   return fails ? 1 : 0;
}
