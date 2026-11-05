// license:GPLv3+

#include "core/stdafx.h"

#include "ui/live/editor/AreaLayout.h"

#include "doctest.h"

#include <cmath>
#include <limits>

using json = nlohmann::json;

namespace
{
struct Legacy
{
   bool outliner, properties;
   float outlinerW, propertiesW;
   int left, right;
};

// The three default workspaces (Layout, Look-dev, Wiring) as legacy keys
constexpr Legacy kLayout { true, true, 200.f, 360.f, 0, 1 };
constexpr Legacy kLookDev { false, true, 200.f, 360.f, 0, 1 };

AreaLayout FromLegacy(const Legacy& l) { return AreaLayout::FromLegacy(l.outliner, l.properties, l.outlinerW, l.propertiesW, l.left, l.right); }

// Wiring: Outliner 200 | Viewport | right column H[fixed=second,size=380](Properties, Script)
AreaLayout MakeWiring()
{
   AreaLayout l = FromLegacy(kLayout);
   int script = -1;
   REQUIRE(l.Split(l.FindLeaf(AreaType::Properties), SplitAxis::Horizontal, true, 380.f, &script)); // a second Properties ...
   REQUIRE(l.SetType(script, AreaType::Script)); // ... made the Script
   return l;
}

void Legacy2(const AreaLayout& l, Legacy& out)
{
   l.ToLegacy(out.outliner, out.properties, out.outlinerW, out.propertiesW, out.left, out.right);
}

void CheckLegacy(const AreaLayout& l, const Legacy& expected)
{
   Legacy got {};
   Legacy2(l, got);
   CHECK(got.outliner == expected.outliner);
   CHECK(got.properties == expected.properties);
   CHECK(got.outlinerW == expected.outlinerW);
   CHECK(got.propertiesW == expected.propertiesW);
   CHECK(got.left == expected.left);
   CHECK(got.right == expected.right);
}

struct Computed
{
   std::vector<LeafRect> leaves;
   std::vector<SplitterRect> splitters;
   const LeafRect* Leaf(const AreaLayout& l, const AreaType t) const
   {
      const int id = l.FindLeaf(t);
      for (const LeafRect& r : leaves)
         if (r.id == id)
            return &r;
      return nullptr;
   }
};

Computed Compute(const AreaLayout& l, const float w = 1920.f, const float h = 1080.f, const float dpi = 1.f, const float sidebar = 0.f, const int maximized = -1)
{
   Computed c;
   l.Compute({ 0.f, 0.f, w, h }, dpi, sidebar, c.leaves, c.splitters, maximized);
   return c;
}

json Leaf(const char* name, const bool hidden = false)
{
   json j = { { "editor", name } };
   if (hidden)
      j["hidden"] = true;
   return j;
}

json Split(const char* axis, const char* fixed, const json& size, const json& a, const json& b)
{
   return { { "split", axis }, { "fixed", fixed }, { "size", size }, { "children", json::array({ a, b }) } };
}

// Valid base: Outliner 200 | Viewport | Properties 360
json ValidJson()
{
   return Split("vertical", "first", 200, Leaf("Outliner"), Split("vertical", "second", 360, Leaf("Viewport"), Leaf("Properties")));
}

void CheckRejected(const json& j, const char* msgPart = nullptr)
{
   AreaLayout l = FromLegacy(kLayout);
   const AreaLayout before = l;
   CHECK_FALSE(l.FromJson(j));
   CHECK(l == before); // unchanged on failure
   CHECK(l.Validate());
   if (msgPart)
      CHECK(l.LastError().find(msgPart) != std::string::npos);
}
}

TEST_CASE("AreaLayout legacy conversion and JSON round trip of the default workspaces")
{
   SUBCASE("Layout and Look-dev: FromLegacy -> ToJson -> text -> FromJson -> ToLegacy")
   {
      for (const Legacy& w : { kLayout, kLookDev })
      {
         const AreaLayout a = FromLegacy(w);
         CHECK(a.Validate());
         CHECK(a.LeafCount() == 3);

         const std::string text = a.ToJson().dump();
         AreaLayout b;
         REQUIRE(b.FromJson(json::parse(text)));
         CHECK(b == a);
         CHECK(b.Validate());
         CHECK(b.ToJson() == a.ToJson());
         CheckLegacy(b, w);
      }
   }

   SUBCASE("Wiring: H split in the right column")
   {
      const AreaLayout a = MakeWiring();
      CHECK(a.Validate());
      CHECK(a.LeafCount() == 4);
      CHECK(a.FindLeaf(AreaType::Script) >= 0);

      const json j = a.ToJson();
      CHECK(j["children"][1]["children"][1]["split"] == "horizontal");
      CHECK(j["children"][1]["children"][1]["fixed"] == "second");
      CHECK(j["children"][1]["children"][1]["size"] == 380.f);
      CHECK(j["children"][1]["children"][1]["children"][1]["editor"] == "Script");

      AreaLayout b;
      REQUIRE(b.FromJson(json::parse(j.dump())));
      CHECK(b == a);
      // older builds get the Properties leaf of the right column
      CheckLegacy(b, kLayout);

      const Computed c = Compute(b);
      CHECK(c.Leaf(b, AreaType::Properties)->rect.x0 == 1560.f);
      CHECK(c.Leaf(b, AreaType::Properties)->rect.y1 == 700.f);
      CHECK(c.Leaf(b, AreaType::Script)->rect.y0 == 700.f);
      CHECK(c.Leaf(b, AreaType::Script)->rect.y1 == 1080.f);
   }

   SUBCASE("non default widths, swapped editors and a hidden Properties")
   {
      const Legacy w { true, false, 250.f, 420.f, 3, 2 };
      const AreaLayout a = FromLegacy(w);
      AreaLayout b;
      REQUIRE(b.FromJson(json::parse(a.ToJson().dump())));
      CHECK(b == a);
      CheckLegacy(b, w);
   }

   SUBCASE("hidden flag and the whole Layout survive a JSON round trip")
   {
      AreaLayout a = FromLegacy(kLayout);
      REQUIRE(a.SetHidden(a.FindLeaf(AreaType::Outliner), true));
      AreaLayout b;
      REQUIRE(b.FromJson(a.ToJson()));
      CHECK(b == a);
      CHECK(b.Find(b.FindLeaf(AreaType::Outliner))->hidden);
      CHECK(AreaLayout().ToJson() == AreaLayout::FromLegacy(true, true, 0.f, 0.f, 0, 1).ToJson()); // default constructed == legacy default (stored widths absent)
   }

   SUBCASE("camera preview view setup round trips; Auto writes no key")
   if (AreaLayout::cameraPreviewEnabled)
   {
      AreaLayout a = FromLegacy(kLayout);
      REQUIRE(a.SetType(a.FindLeaf(AreaType::Outliner), AreaType::CameraPreview));
      const int leaf = a.FindLeaf(AreaType::CameraPreview);
      CHECK(a.ToJson().dump().find("preview_view") == std::string::npos);
      REQUIRE(a.SetPreviewView(leaf, 1));
      CHECK(a.ToJson().dump().find("\"cabinet\"") != std::string::npos);
      AreaLayout b;
      REQUIRE(b.FromJson(a.ToJson()));
      CHECK(b == a);
      CHECK(b.Find(b.FindLeaf(AreaType::CameraPreview))->previewView == 1);
      CHECK_FALSE(a.SetPreviewView(leaf, 3));
      REQUIRE(a.SetPreviewView(leaf, -1));
      CHECK(!(b == a));
   }

   SUBCASE("the preview's view setup and settings are dropped when it stops being one")
   if (AreaLayout::cameraPreviewEnabled)
   {
      AreaLayout a = FromLegacy(kLayout);
      const int outliner = a.FindLeaf(AreaType::Outliner), props = a.FindLeaf(AreaType::Properties);
      REQUIRE(a.SetType(outliner, AreaType::CameraPreview));
      REQUIRE(a.SetPreviewView(outliner, 1));
      AreaViewSettings v;
      v.freeOrbit = true;
      v.wireframe = true;
      REQUIRE(a.SetViewSettings(outliner, v));
      int swapped = 0;
      REQUIRE(a.SetType(outliner, AreaType::Properties, &swapped)); // a second Properties: no swap, the other Properties leaf stays one
      CHECK(swapped == -1);
      CHECK(a.Find(props)->type == AreaType::Properties);
      CHECK(a.Find(outliner)->type == AreaType::Properties);
      CHECK(a.Find(outliner)->previewView == -1);
      CHECK_FALSE(a.Find(outliner)->view.freeOrbit);
      CHECK_FALSE(a.Find(outliner)->view.wireframe);
      CHECK_FALSE(a.SetViewSettings(outliner, v)); // only for previews
      CHECK(a.Validate());
   }

   SUBCASE("view settings round trip; the defaults write no key; malformed values load as the defaults")
   if (AreaLayout::cameraPreviewEnabled)
   {
      AreaLayout a = FromLegacy(kLayout);
      const int leaf = a.FindLeaf(AreaType::Outliner);
      REQUIRE(a.SetType(leaf, AreaType::CameraPreview));
      const std::string plain = a.ToJson().dump();
      CHECK(plain.find("view_camera") == std::string::npos);
      CHECK(plain.find("view_shading") == std::string::npos);
      CHECK(plain.find("\"live\"") == std::string::npos);
      CHECK(plain.find("orbit") == std::string::npos);
      AreaViewSettings v;
      v.freeOrbit = true;
      v.wireframe = true;
      v.live = false;
      v.orbitValid = true;
      v.ortho = true;
      v.yaw = -30.f;
      v.pitch = 45.f;
      v.distance = 1234.f;
      v.target[0] = 10.f;
      v.target[1] = 20.f;
      v.target[2] = 30.f;
      REQUIRE(a.SetViewSettings(leaf, v));
      AreaLayout b;
      REQUIRE(b.FromJson(a.ToJson()));
      CHECK(b == a);
      const AreaViewSettings& r = b.Find(b.FindLeaf(AreaType::CameraPreview))->view;
      CHECK(r.freeOrbit);
      CHECK(r.wireframe);
      CHECK_FALSE(r.live);
      CHECK(r.orbitValid);
      CHECK(r.ortho);
      CHECK(r.yaw == -30.f);
      CHECK(r.pitch == 45.f);
      CHECK(r.distance == 1234.f);
      CHECK(r.target[2] == 30.f);

      // The orbit camera is not part of the structural equality (it moves with every drag), the choices are
      AreaViewSettings moved = v;
      moved.yaw = 10.f;
      REQUIRE(b.SetViewSettings(b.FindLeaf(AreaType::CameraPreview), moved));
      CHECK(b == a);
      moved.wireframe = false;
      REQUIRE(b.SetViewSettings(b.FindLeaf(AreaType::CameraPreview), moved));
      CHECK(!(b == a));

      AreaViewSettings bad = v;
      bad.distance = std::numeric_limits<float>::quiet_NaN();
      CHECK_FALSE(a.SetViewSettings(leaf, bad));

      // Unknown values, wrong types, a broken orbit: the layout loads, with the defaults for what is broken
      json j = Leaf("CameraPreview");
      j["view_camera"] = "drone";
      j["view_shading"] = 3;
      j["live"] = "no";
      j["orbit"] = { { "yaw", 1 }, { "pitch", 2 }, { "distance", -5 }, { "target", { 1, 2, 3 } } };
      AreaLayout c;
      REQUIRE(c.FromJson(Split("vertical", "first", 200, j, Leaf("Viewport"))));
      const AreaViewSettings& d = c.Find(c.FindLeaf(AreaType::CameraPreview))->view;
      CHECK_FALSE(d.freeOrbit);
      CHECK_FALSE(d.wireframe);
      CHECK(d.live);
      CHECK_FALSE(d.orbitValid);
   }

   SUBCASE("Close replaces the parent split by the sibling")
   {
      AreaLayout a = FromLegacy(kLayout);
      CHECK_FALSE(a.CanClose(a.FindLeaf(AreaType::Viewport)));
      REQUIRE(a.Close(a.FindLeaf(AreaType::Outliner)));
      CHECK(a.LeafCount() == 2);
      CHECK_FALSE(a.IsHosted(AreaType::Outliner));
      CHECK(a.Validate());
   }
}

TEST_CASE("AreaLayout migration of the old workspace keys")
{
   SUBCASE("missing or zero widths mean the defaults")
   {
      const AreaLayout a = AreaLayout::FromLegacy(true, true, 0.f, -5.f, 0, 1);
      const Legacy defaults { true, true, AreaLayout::kDefaultOutlinerWidth, AreaLayout::kDefaultPropertiesWidth, 0, 1 };
      CheckLegacy(a, defaults);
      const AreaLayout b = AreaLayout::FromLegacy(true, true, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0, 1);
      CheckLegacy(b, defaults);
      CHECK(a == b);
   }

   SUBCASE("invalid or duplicate editors mean Outliner/Properties")
   {
      for (const std::pair<int, int> bad : { std::pair { -1, 1 }, std::pair { 0, 9 }, std::pair { 2, 2 }, std::pair { 4, 1 }, std::pair { 0, 4 }, std::pair { 7, 7 } })
      {
         const AreaLayout a = AreaLayout::FromLegacy(true, true, 200.f, 360.f, bad.first, bad.second);
         CHECK(a.Validate());
         CHECK(a == FromLegacy(kLayout));
      }
   }

   SUBCASE("legacy outliner:false maps to a hidden left leaf, exactly reversible")
   {
      AreaLayout a = FromLegacy(kLookDev);
      CHECK(a.Find(a.FindLeaf(AreaType::Outliner))->hidden);
      REQUIRE(a.SetHidden(a.FindLeaf(AreaType::Outliner), false));
      CHECK(a == FromLegacy(kLayout));
   }

   SUBCASE("a workspace entry with legacy keys only and a bad layout falls back")
   {
      // Same decision as the loader: use "layout" only if FromJson succeeds, else the legacy keys
      const json ws = { { "outliner", false }, { "properties", true }, { "left_editor", 2 }, { "right_editor", 1 }, { "properties_width", 400 },
         { "layout", Split("vertical", "first", 200, Leaf("Outliner"), Leaf("Outliner")) } };
      AreaLayout l;
      if (!l.FromJson(ws["layout"]))
         l = AreaLayout::FromLegacy(ws.value("outliner", true), ws.value("properties", true), ws.value("outliner_width", 0.f), ws.value("properties_width", 0.f),
            ws.value("left_editor", 0), ws.value("right_editor", 1));
      CheckLegacy(l, { false, true, AreaLayout::kDefaultOutlinerWidth, 400.f, 2, 1 });
   }

   SUBCASE("ToLegacy with panels on one side only")
   {
      AreaLayout l;
      REQUIRE(l.FromJson(Split("vertical", "second", 300, Leaf("Viewport"), Leaf("Properties"))));
      CheckLegacy(l, { false, true, AreaLayout::kDefaultOutlinerWidth, 300.f, 0, 1 });
      REQUIRE(l.FromJson(Split("vertical", "first", 300, Leaf("Script"), Leaf("Viewport"))));
      CheckLegacy(l, { true, false, 300.f, 360.f, 2, 1 });
   }
}

TEST_CASE("AreaLayout compute")
{
   const AreaLayout l = FromLegacy(kLayout);

   SUBCASE("1920x1080 at dpi 1")
   {
      const Computed c = Compute(l);
      REQUIRE(c.leaves.size() == 3);
      const LeafRect& out = *c.Leaf(l, AreaType::Outliner);
      const LeafRect& vp = *c.Leaf(l, AreaType::Viewport);
      const LeafRect& props = *c.Leaf(l, AreaType::Properties);
      CHECK(out.rect.x0 == 0.f);
      CHECK(out.rect.x1 == 200.f);
      CHECK(vp.rect.x0 == 200.f);
      CHECK(vp.rect.x1 == 1560.f);
      CHECK(props.rect.x0 == 1560.f);
      CHECK(props.rect.x1 == 1920.f);
      for (const LeafRect& r : c.leaves)
      {
         CHECK(r.visible);
         CHECK(r.rect.y0 == 0.f);
         CHECK(r.rect.y1 == 1080.f);
      }
      REQUIRE(c.splitters.size() == 2);
      CHECK(c.splitters[0].pos == 200.f);
      CHECK(c.splitters[1].pos == 1560.f);
      CHECK(c.splitters[1].axis == SplitAxis::Vertical);
      CHECK(c.splitters[1].grab.x0 == 1557.f);
      CHECK(c.splitters[1].grab.x1 == 1563.f);
      CHECK(c.splitters[1].grab.y1 == 1080.f);
      // Dragging the Properties splitter 100 px left gives a stored width of 460
      CHECK(c.splitters[1].SizeForPos(1460.f, 1.f) == 460.f);
      CHECK(c.splitters[0].SizeForPos(250.f, 1.f) == 250.f);
      CHECK(c.splitters[1].minPos == 200.f + 240.f);
      CHECK(c.splitters[1].maxPos == 1920.f - 140.f);
   }

   SUBCASE("dpi scales sizes, positions are whole pixels")
   {
      const Computed c = Compute(l, 3840.f, 2160.f, 2.f);
      CHECK(c.Leaf(l, AreaType::Outliner)->rect.x1 == 400.f);
      CHECK(c.Leaf(l, AreaType::Properties)->rect.x0 == 3840.f - 720.f);
      const Computed odd = Compute(l, 1001.f, 700.f, 1.25f);
      for (const SplitterRect& s : odd.splitters)
         CHECK(s.pos == std::round(s.pos));
   }

   SUBCASE("hidden leaves take no space and their splitter is gone")
   {
      AreaLayout h = l;
      REQUIRE(h.SetHidden(h.FindLeaf(AreaType::Outliner), true));
      const Computed c = Compute(h);
      CHECK_FALSE(c.Leaf(h, AreaType::Outliner)->visible);
      CHECK(c.Leaf(h, AreaType::Viewport)->rect.x0 == 0.f);
      CHECK(c.splitters.size() == 1);
      REQUIRE(h.SetHidden(h.FindLeaf(AreaType::Properties), true));
      const Computed c2 = Compute(h);
      CHECK(c2.splitters.empty());
      CHECK(c2.Leaf(h, AreaType::Viewport)->rect.x1 == 1920.f);
      // exactly reversible
      REQUIRE(h.SetHidden(h.FindLeaf(AreaType::Outliner), false));
      REQUIRE(h.SetHidden(h.FindLeaf(AreaType::Properties), false));
      const Computed c3 = Compute(h);
      CHECK(c3.leaves.size() == Compute(l).leaves.size());
      CHECK(c3.Leaf(h, AreaType::Viewport)->rect.x0 == 200.f);
      CHECK(c3.Leaf(h, AreaType::Viewport)->rect.x1 == 1560.f);
   }

   SUBCASE("minimum sizes at 600x400")
   {
      const Computed c = Compute(l, 600.f, 400.f);
      const LeafRect& out = *c.Leaf(l, AreaType::Outliner);
      const LeafRect& vp = *c.Leaf(l, AreaType::Viewport);
      const LeafRect& props = *c.Leaf(l, AreaType::Properties);
      CHECK(out.rect.Width() >= AreaLayout::kPanelMinW);
      CHECK(vp.rect.Width() >= AreaLayout::kViewportMinW);
      CHECK(props.rect.Width() >= AreaLayout::kPanelMinW);
      CHECK(out.rect.Width() + vp.rect.Width() + props.rect.Width() == 600.f);
      CHECK(out.rect.x1 == 200.f);
      CHECK(vp.rect.x1 == 440.f); // Properties squeezed to its minimum, Viewport keeps 240
      CHECK(props.rect.Width() == 160.f);
      CHECK(vp.rect.Height() >= AreaLayout::kViewportMinH);
   }

   SUBCASE("the N-panel width counts towards the Viewport minimum")
   {
      const Computed c = Compute(l, 900.f, 600.f, 1.f, 300.f);
      const LeafRect& vp = *c.Leaf(l, AreaType::Viewport);
      CHECK(vp.rect.Width() >= 240.f + 300.f);
   }

   SUBCASE("smaller than all minimums: proportional shrink, never negative, covers the bounds")
   {
      for (const float w : { 500.f, 300.f, 100.f, 10.f, 0.f })
      {
         const Computed c = Compute(l, w, 50.f, 1.f, 300.f);
         float sum = 0.f, prevX1 = 0.f;
         for (const LeafRect& r : c.leaves)
         {
            CHECK(r.rect.Width() >= 0.f);
            CHECK(r.rect.Height() >= 0.f);
            CHECK(r.rect.x0 == prevX1); // no gaps, no seams
            prevX1 = r.rect.x1;
            sum += r.rect.Width();
         }
         CHECK(sum == w);
      }
   }

   SUBCASE("maximize shows only that leaf on the whole rect, without splitters")
   {
      const int propsId = l.FindLeaf(AreaType::Properties);
      const Computed c = Compute(l, 1920.f, 1080.f, 1.f, 0.f, propsId);
      CHECK(c.splitters.empty());
      REQUIRE(c.leaves.size() == 3);
      int visible = 0;
      for (const LeafRect& r : c.leaves)
      {
         if (r.visible)
         {
            ++visible;
            CHECK(r.id == propsId);
            CHECK(r.rect.x0 == 0.f);
            CHECK(r.rect.x1 == 1920.f);
         }
      }
      CHECK(visible == 1);
      // unknown / hidden id: ignored
      CHECK(Compute(l, 1920.f, 1080.f, 1.f, 0.f, 9999999).splitters.size() == 2);
      // restoring gives the same rects exactly
      const Computed a = Compute(l), b = Compute(l);
      CHECK(a.leaves[1].rect.x0 == b.leaves[1].rect.x0);
   }

   SUBCASE("LeafAt")
   {
      const Computed c = Compute(l);
      CHECK(AreaLayout::LeafAt(c.leaves, 10.f, 10.f) == l.FindLeaf(AreaType::Outliner));
      CHECK(AreaLayout::LeafAt(c.leaves, 200.f, 10.f) == l.FindLeaf(AreaType::Viewport)); // half open
      CHECK(AreaLayout::LeafAt(c.leaves, 1700.f, 500.f) == l.FindLeaf(AreaType::Properties));
      CHECK(AreaLayout::LeafAt(c.leaves, 2000.f, 500.f) == -1);
      CHECK(AreaLayout::LeafAt(c.leaves, 10.f, -1.f) == -1);
   }
}

TEST_CASE("AreaLayout split")
{
   AreaLayout l = FromLegacy(kLayout);

   SUBCASE("a split of the Viewport gives another 3D view (a CameraPreview with the default settings), on the requested side")
   if (AreaLayout::cameraPreviewEnabled)
   {
      const int vpId = l.FindLeaf(AreaType::Viewport);
      REQUIRE(l.CanSplit(vpId));
      int viewId = -1;
      REQUIRE(l.Split(vpId, SplitAxis::Vertical, true, 300.f, &viewId));
      CHECK(l.Validate());
      CHECK(l.FindLeaf(AreaType::Viewport) == vpId); // the split leaf keeps its id
      REQUIRE(viewId >= 0);
      CHECK(l.Find(viewId)->type == AreaType::CameraPreview);
      CHECK(l.Find(viewId)->previewView == -1);
      CHECK_FALSE(l.Find(viewId)->view.freeOrbit);
      const Computed c = Compute(l);
      CHECK(c.Leaf(l, AreaType::CameraPreview)->rect.Width() == 300.f);
      CHECK(c.Leaf(l, AreaType::CameraPreview)->rect.x1 == 1560.f);
      CHECK(c.Leaf(l, AreaType::Viewport)->rect.x1 == 1260.f);
      CHECK(l.ParentId(vpId) == l.ParentId(viewId));
   }

   SUBCASE("new leaf on the first (left/top) side keeps the given size")
   {
      int id = -1;
      REQUIRE(l.Split(l.FindLeaf(AreaType::Viewport), SplitAxis::Horizontal, false, 250.f, &id));
      const Computed c = Compute(l);
      const LeafRect* const r = c.Leaf(l, l.Find(id)->type);
      REQUIRE(r != nullptr);
      CHECK(r->rect.y0 == 0.f);
      CHECK(r->rect.Height() == 250.f);
      CHECK(c.Leaf(l, AreaType::Viewport)->rect.y0 == 250.f);
   }

   SUBCASE("split of a panel leaf: a copy of its editor and saved state")
   {
      const int out = l.FindLeaf(AreaType::Outliner);
      REQUIRE(l.SetState(out, json { { "display_mode", "table_data" }, { "types", 5 } }));
      int copy = -1;
      REQUIRE(l.Split(out, SplitAxis::Horizontal, true, 300.f, &copy));
      CHECK(l.Find(copy)->type == AreaType::Outliner);
      CHECK(l.Find(copy)->state == l.Find(out)->state);
      CHECK(l.CountLeaves(AreaType::Outliner) == 2);
      const Computed c = Compute(l);
      for (const LeafRect& r : c.leaves)
         if (r.id == copy)
         {
            CHECK(r.rect.Height() == 300.f);
            CHECK(r.rect.Width() == 200.f);
         }
         else if (r.id == out)
            CHECK(r.rect.Height() == 780.f);
   }

   SUBCASE("a split of a secondary 3D view copies its view setup and settings; at the cap a 3D view split gives the first editor not shown")
   if (AreaLayout::cameraPreviewEnabled)
   {
      const int out = l.FindLeaf(AreaType::Outliner);
      REQUIRE(l.SetType(out, AreaType::CameraPreview));
      REQUIRE(l.SetPreviewView(out, 2));
      AreaViewSettings v;
      v.wireframe = true;
      REQUIRE(l.SetViewSettings(out, v));
      int copy = -1;
      REQUIRE(l.Split(out, SplitAxis::Horizontal, true, 300.f, &copy));
      CHECK(l.Find(copy)->type == AreaType::CameraPreview);
      CHECK(l.Find(copy)->previewView == 2);
      CHECK(l.Find(copy)->view.wireframe);
      int third = -1;
      REQUIRE(l.Split(l.FindLeaf(AreaType::Viewport), SplitAxis::Vertical, true, 300.f, &third));
      CHECK(l.Find(third)->type == AreaType::CameraPreview);
      CHECK(l.CountLeaves(AreaType::CameraPreview) == AreaLayout::kMaxViewLeaves);
      CHECK(l.NewLeafType(l.FindLeaf(AreaType::Viewport)) == AreaType::Outliner); // the first editor not shown
      CHECK(l.NewLeafType(copy) == AreaType::Outliner);
      CHECK(l.Validate());
   }

   SUBCASE("a split copies the split area's editor and state, whatever is shown; refused at kMaxLeaves")
   {
      int a = -1, b = -1;
      REQUIRE(l.Split(l.FindLeaf(AreaType::Outliner), SplitAxis::Vertical, true, 150.f, &a));
      REQUIRE(l.SetType(a, AreaType::Script));
      REQUIRE(l.Split(a, SplitAxis::Horizontal, true, 200.f, &b));
      REQUIRE(l.SetType(b, AreaType::PartLibrary));
      CHECK(l.LeafCount() == 5);
      CHECK(l.Validate());
      const int props = l.FindLeaf(AreaType::Properties);
      REQUIRE(l.SetState(props, json { { "pin", { { "kind", "part" }, { "name", "Wall1" } } } }));
      CHECK(l.NewLeafType(props) == AreaType::Properties);
      REQUIRE(l.CanSplit(props));
      REQUIRE(l.Split(props, SplitAxis::Horizontal, true, 300.f));
      CHECK(l.CountLeaves(AreaType::Properties) == 2);
      for (const AreaNode& n : l.Nodes())
         if (!n.isSplit && n.type == AreaType::Properties)
            CHECK(n.state["pin"]["name"] == "Wall1");
      CHECK(l.Validate());
      CHECK(l.LeafCount() == AreaLayout::kMaxLeaves);
      const AreaLayout before = l;
      for (const AreaNode& n : l.Nodes())
         if (!n.isSplit)
         {
            CHECK_FALSE(l.CanSplit(static_cast<int>(n.id)));
            CHECK_FALSE(l.Split(static_cast<int>(n.id), SplitAxis::Vertical, true, 100.f));
         }
      CHECK(l == before);
   }

   SUBCASE("unknown ids and split nodes are refused")
   {
      CHECK_FALSE(l.CanSplit(-1));
      CHECK_FALSE(l.CanSplit(123456789));
      CHECK_FALSE(l.CanSplit(static_cast<int>(l.Nodes()[l.Root()].id))); // a split node
   }
}

TEST_CASE("AreaLayout join")
{
   const AreaLayout original = FromLegacy(kLayout);

   SUBCASE("refused for the Viewport, non-siblings, subtrees, hidden and self")
   {
      AreaLayout l = original;
      const int out = l.FindLeaf(AreaType::Outliner);
      const int vp = l.FindLeaf(AreaType::Viewport);
      const int props = l.FindLeaf(AreaType::Properties);
      const int innerSplit = l.ParentId(vp);
      REQUIRE(innerSplit >= 0);

      CHECK_FALSE(l.CanJoin(props, vp)); // would consume the Viewport
      CHECK_FALSE(l.Join(props, vp));
      CHECK_FALSE(l.CanJoin(out, props)); // not siblings
      CHECK_FALSE(l.CanJoin(out, vp));
      CHECK_FALSE(l.CanJoin(out, innerSplit)); // a subtree
      CHECK_FALSE(l.CanJoin(innerSplit, out));
      CHECK_FALSE(l.CanJoin(props, props));
      CHECK_FALSE(l.CanJoin(props, 99999999));
      CHECK_FALSE(l.Join(out, props));
      CHECK(l == original);

      REQUIRE(l.SetHidden(props, true));
      CHECK_FALSE(l.CanJoin(vp, props)); // hidden
      REQUIRE(l.SetHidden(props, false));
      CHECK(l.CanJoin(vp, props));
   }

   SUBCASE("a split then a join restores the layout")
   {
      AreaLayout l = original;
      const int vp = l.FindLeaf(AreaType::Viewport);
      int script = -1;
      REQUIRE(l.Split(vp, SplitAxis::Vertical, true, 300.f, &script));
      CHECK_FALSE(l.CanJoin(script, vp));
      REQUIRE(l.CanJoin(vp, script));
      REQUIRE(l.Join(vp, script));
      CHECK(l.Find(script) == nullptr);
      CHECK(l.FindLeaf(AreaType::Viewport) == vp);
      CHECK(l.Validate());
      CHECK(l == original);
   }

   SUBCASE("joining panels siblings")
   {
      AreaLayout l = MakeWiring();
      const int props = l.FindLeaf(AreaType::Properties);
      const int script = l.FindLeaf(AreaType::Script);
      REQUIRE(l.CanJoin(props, script));
      REQUIRE(l.Join(script, props)); // either side may win
      CHECK(l.FindLeaf(AreaType::Properties) < 0);
      CHECK(l.Validate());
      CHECK(l.LeafCount() == 3);
      const Computed c = Compute(l);
      CHECK(c.Leaf(l, AreaType::Script)->rect.Height() == 1080.f);
   }

   SUBCASE("the Viewport can take over a panel")
   {
      AreaLayout l = original;
      const int vp = l.FindLeaf(AreaType::Viewport);
      REQUIRE(l.Join(vp, l.FindLeaf(AreaType::Properties)));
      CHECK(l.Validate());
      CHECK(l.LeafCount() == 2);
      CHECK(Compute(l).Leaf(l, AreaType::Viewport)->rect.x1 == 1920.f);
   }
}

TEST_CASE("AreaLayout swap")
{
   const AreaLayout original = MakeWiring();

   SUBCASE("swap twice is the identity")
   {
      AreaLayout l = original;
      const int vp = l.FindLeaf(AreaType::Viewport);
      const int inner = l.ParentId(vp);
      REQUIRE(l.Swap(inner));
      CHECK(l != original);
      CHECK(l.Validate());
      REQUIRE(l.Swap(inner));
      CHECK(l == original);
      CHECK(l.ToJson() == original.ToJson());
   }

   SUBCASE("swap mirrors the rects and keeps the sizes")
   {
      AreaLayout l = original;
      const int vp = l.FindLeaf(AreaType::Viewport);
      REQUIRE(l.Swap(l.ParentId(vp)));
      const Computed c = Compute(l);
      CHECK(c.Leaf(l, AreaType::Properties)->rect.x0 == 200.f);
      CHECK(c.Leaf(l, AreaType::Properties)->rect.Width() == 360.f);
      CHECK(c.Leaf(l, AreaType::Viewport)->rect.x1 == 1920.f);
   }

   SUBCASE("swap of a split of leaves and of subtrees; leaves are refused")
   {
      AreaLayout l = original;
      CHECK_FALSE(l.Swap(l.FindLeaf(AreaType::Viewport)));
      CHECK_FALSE(l.Swap(-1));
      const int colSplit = l.ParentId(l.FindLeaf(AreaType::Script));
      REQUIRE(l.Swap(colSplit));
      const Computed c = Compute(l);
      CHECK(c.Leaf(l, AreaType::Script)->rect.y0 == 0.f);
      CHECK(c.Leaf(l, AreaType::Script)->rect.Height() == 380.f);
      CHECK(c.Leaf(l, AreaType::Properties)->rect.y0 == 380.f);
      // root: swaps the Outliner and the whole right subtree
      AreaLayout r = original;
      REQUIRE(r.Swap(static_cast<int>(r.Nodes()[r.Root()].id)));
      CHECK(r.Validate());
      const Computed rc = Compute(r);
      CHECK(rc.Leaf(r, AreaType::Outliner)->rect.x0 == 1720.f);
      CHECK(rc.Leaf(r, AreaType::Outliner)->rect.Width() == 200.f);
   }
}

TEST_CASE("AreaLayout set type and hidden")
{
   SUBCASE("a type another leaf shows: both leaves show it (any number of areas of a type), the new one with the default state")
   {
      AreaLayout l = FromLegacy(kLayout);
      const int out = l.FindLeaf(AreaType::Outliner);
      const int props = l.FindLeaf(AreaType::Properties);
      REQUIRE(l.SetState(out, json { { "display_mode", "table_data" } }));
      REQUIRE(l.SetState(props, json { { "pin", { { "kind", "table" } } } }));
      int swapped = 0;
      REQUIRE(l.SetType(out, AreaType::Properties, &swapped));
      CHECK(swapped == -1);
      CHECK(l.Find(out)->type == AreaType::Properties);
      CHECK(l.Find(props)->type == AreaType::Properties);
      CHECK(l.Find(out)->state.empty()); // not the Outliner's state, not the other Properties' pin
      CHECK(l.Find(props)->state["pin"]["kind"] == "table");
      CHECK_FALSE(l.IsHosted(AreaType::Outliner));
      CHECK(l.CountLeaves(AreaType::Properties) == 2);
      CHECK(l.Validate());
      REQUIRE(l.SetType(props, AreaType::Script));
      CHECK(l.Find(props)->state.empty());
      CHECK_FALSE(l.SetState(l.FindLeaf(AreaType::Viewport), json::object()));
      CHECK_FALSE(l.SetState(out, json::array()));
   }

   SUBCASE("a free type is just taken; the hidden flag stays with the slot")
   {
      AreaLayout l = FromLegacy(kLookDev);
      const int out = l.FindLeaf(AreaType::Outliner);
      REQUIRE(l.SetType(out, AreaType::Script));
      CHECK(l.FindLeaf(AreaType::Outliner) < 0);
      CHECK(l.Find(out)->type == AreaType::Script);
      CHECK(l.Find(out)->hidden);
      REQUIRE(l.SetType(out, AreaType::Script)); // no-op
      CHECK(l.Validate());
   }

   SUBCASE("each area keeps its own hidden flag")
   {
      AreaLayout l = FromLegacy(kLookDev);
      const int out = l.FindLeaf(AreaType::Outliner);
      const int props = l.FindLeaf(AreaType::Properties);
      REQUIRE(l.SetType(props, AreaType::Outliner));
      CHECK(l.Find(out)->type == AreaType::Outliner);
      CHECK(l.Find(props)->type == AreaType::Outliner);
      CHECK(l.Find(out)->hidden);
      CHECK_FALSE(l.Find(props)->hidden);
   }

   SUBCASE("the Viewport can't be re-typed, hidden, chosen, or given to a panel")
   {
      AreaLayout l = FromLegacy(kLayout);
      const AreaLayout before = l;
      const int vp = l.FindLeaf(AreaType::Viewport);
      const int out = l.FindLeaf(AreaType::Outliner);
      CHECK_FALSE(l.SetType(vp, AreaType::Script));
      CHECK_FALSE(l.SetType(out, AreaType::Viewport));
      CHECK_FALSE(l.SetType(out, AreaType::Count));
      CHECK_FALSE(l.SetType(static_cast<int>(l.Nodes()[l.Root()].id), AreaType::Script)); // a split
      CHECK_FALSE(l.SetHidden(vp, true));
      CHECK_FALSE(l.SetHidden(-1, true));
      CHECK(l == before);
      CHECK(l.Validate());
   }

   SUBCASE("CameraPreview: chosen by type only, up to kMaxViewLeaves leaves, never in the legacy keys")
   {
      AreaLayout l = FromLegacy(kLayout);
      const int out = l.FindLeaf(AreaType::Outliner);
      const int props = l.FindLeaf(AreaType::Properties);
      REQUIRE(l.SetType(out, AreaType::CameraPreview));
      CHECK(l.Validate());
      CHECK(l.FindLeaf(AreaType::CameraPreview) == out);
      CHECK_FALSE(l.IsHosted(AreaType::Outliner));
      Legacy legacy {};
      Legacy2(l, legacy);
      CHECK(legacy.left != static_cast<int>(AreaType::CameraPreview));
      CHECK(legacy.right == static_cast<int>(AreaType::Properties));
      CHECK(legacy.left >= 0);
      CHECK(legacy.left < kPanelTypeCount);
      REQUIRE(l.SetType(props, AreaType::CameraPreview)); // a second view: no swap below the cap
      CHECK(l.CountLeaves(AreaType::CameraPreview) == 2);
      CHECK(l.Find(out)->type == AreaType::CameraPreview);
      CHECK(l.Validate());
      // A third one (a split of the Viewport gives a 3D view), then a fourth request swaps with the first view instead
      int third = -1;
      REQUIRE(l.Split(l.FindLeaf(AreaType::Viewport), SplitAxis::Horizontal, true, 200.f, &third));
      CHECK(l.Find(third)->type == AreaType::CameraPreview);
      CHECK(l.CountLeaves(AreaType::CameraPreview) == AreaLayout::kMaxViewLeaves);
      CHECK(l.Validate());
      int fourth = -1;
      REQUIRE(l.Split(l.FindLeaf(AreaType::Viewport), SplitAxis::Vertical, true, 200.f, &fourth));
      CHECK(l.Find(fourth)->type == AreaType::Outliner); // at the cap: the first editor not shown
      REQUIRE(l.SetState(fourth, json { { "display_mode", "table_data" } }));
      int swapped = -1;
      REQUIRE(l.SetType(fourth, AreaType::CameraPreview, &swapped));
      CHECK(swapped == out); // the first view
      CHECK(l.CountLeaves(AreaType::CameraPreview) == AreaLayout::kMaxViewLeaves);
      CHECK(l.Find(fourth)->type == AreaType::CameraPreview);
      CHECK(l.Find(fourth)->state.empty());
      CHECK(l.Find(out)->type == AreaType::Outliner); // the view took the type the fourth leaf left, with its panel state
      CHECK(l.Find(out)->state["display_mode"] == "table_data");
      CHECK(l.CountLeaves(AreaType::Outliner) == 1);
      CHECK(l.Validate());
   }
}

TEST_CASE("AreaLayout invalid JSON is rejected")
{
   REQUIRE(AreaLayout().FromJson(ValidJson()));

   SUBCASE("two Viewports")
   {
      CheckRejected(Split("vertical", "first", 200, Leaf("Viewport"), Split("vertical", "second", 360, Leaf("Viewport"), Leaf("Properties"))), "Viewport");
   }
   SUBCASE("no Viewport")
   {
      CheckRejected(Split("vertical", "first", 200, Leaf("Outliner"), Leaf("Properties")), "Viewport");
   }
   SUBCASE("duplicate editors are accepted and round trip with their own states")
   {
      json two = Split("vertical", "first", 200, Leaf("Outliner"), Split("vertical", "second", 360, Leaf("Viewport"), Split("horizontal", "first", 300, Leaf("Properties"), Leaf("Properties"))));
      two["children"][0]["display_mode"] = "table_data";
      two["children"][1]["children"][1]["children"][0]["pin"] = { { "kind", "part" }, { "name", "Wall1" } };
      AreaLayout l;
      REQUIRE(l.FromJson(two));
      CHECK(l.Validate());
      CHECK(l.CountLeaves(AreaType::Properties) == 2);
      const int first = l.FindLeaf(AreaType::Properties);
      CHECK(l.Find(first)->state["pin"]["name"] == "Wall1");
      AreaLayout b;
      REQUIRE(b.FromJson(l.ToJson()));
      CHECK(b == l);
      CHECK(b.ToJson() == l.ToJson());
      CHECK(l.ToJson()["children"][0]["display_mode"] == "table_data");
      CHECK_FALSE(l.ToJson()["children"][1]["children"][1]["children"][1].contains("pin"));
      // The layout's own keys are never state keys
      json clash = Split("vertical", "first", 200, Leaf("Outliner"), Leaf("Viewport"));
      clash["children"][0]["hidden"] = true;
      clash["children"][0]["size"] = 12; // a split key on a leaf
      clash["children"][0]["live"] = false; // a CameraPreview key
      clash["children"][0]["display_mode"] = "table_data";
      REQUIRE(l.FromJson(clash));
      CHECK(l.Find(l.FindLeaf(AreaType::Outliner))->state == json { { "display_mode", "table_data" } });
      AreaLayout round;
      REQUIRE(round.FromJson(l.ToJson()));
      CHECK(round == l);
      CHECK_FALSE(l.ToJson()["children"][0].contains("size"));
      // The legacy keys of a layout with two areas of one editor stay two different editors (what older builds accept)
      json both = Split("vertical", "first", 200, Leaf("Properties"), Split("vertical", "second", 360, Leaf("Viewport"), Leaf("Properties")));
      REQUIRE(l.FromJson(both));
      bool outliner = false, properties = false;
      float ow = 0.f, pw = 0.f;
      int left = -1, right = -1;
      l.ToLegacy(outliner, properties, ow, pw, left, right);
      CHECK(left == static_cast<int>(AreaType::Properties));
      CHECK(right != left);
      CHECK(right >= 0);
      CHECK(right < kPanelTypeCount);
   }
   SUBCASE("NaN, infinite, zero, negative and non numeric sizes")
   {
      CheckRejected(Split("vertical", "first", std::numeric_limits<double>::quiet_NaN(), Leaf("Outliner"), Leaf("Viewport")));
      CheckRejected(Split("vertical", "first", std::numeric_limits<double>::infinity(), Leaf("Outliner"), Leaf("Viewport")));
      CheckRejected(Split("vertical", "first", 0, Leaf("Outliner"), Leaf("Viewport")));
      CheckRejected(Split("vertical", "first", -10, Leaf("Outliner"), Leaf("Viewport")));
      CheckRejected(Split("vertical", "first", "200", Leaf("Outliner"), Leaf("Viewport")));
      CheckRejected(Split("vertical", "first", nullptr, Leaf("Outliner"), Leaf("Viewport")));
   }
   SUBCASE("depth 9")
   {
      // 9 nested splits on the first child, so the depth limit is hit before the leaf count
      json chain = Leaf("Viewport");
      for (int i = 0; i < 9; ++i)
         chain = Split("vertical", "second", 200, chain, Leaf("Outliner"));
      CheckRejected(chain, "deeper");
   }
   SUBCASE("depth 8 is still fine for the depth check")
   {
      json chain = Leaf("Viewport");
      const char* names[] = { "Outliner", "Properties", "Script", "PartLibrary" };
      for (int i = 0; i < 4; ++i)
         chain = Split(i % 2 ? "horizontal" : "vertical", "second", 200, chain, Leaf(names[i]));
      AreaLayout l;
      CHECK(l.FromJson(chain));
      CHECK(l.LeafCount() == 5);
   }
   SUBCASE("more than 6 leaves")
   {
      json j = Leaf("Viewport");
      for (const char* n : { "Outliner", "Properties", "Script", "PartLibrary", "CameraPreview", "Outliner" })
         j = Split("vertical", "second", 100, j, Leaf(n));
      CheckRejected(j);
   }
   SUBCASE("CameraPreview is accepted up to 3 times (rejected by builds where it is disabled)")
   {
      if (!AreaLayout::cameraPreviewEnabled)
         CheckRejected(Split("vertical", "first", 200, Leaf("CameraPreview"), Leaf("Viewport")));
      else
      {
         AreaLayout l;
         CHECK(l.FromJson(Split("vertical", "first", 200, Leaf("CameraPreview"), Leaf("Viewport"))));
         CHECK(l.Validate());
         CHECK(l.FindLeaf(AreaType::CameraPreview) >= 0);
         json three = Leaf("Viewport");
         for (int i = 0; i < 3; ++i)
            three = Split("vertical", "second", 200, three, Leaf("CameraPreview"));
         CHECK(l.FromJson(three));
         CHECK(l.CountLeaves(AreaType::CameraPreview) == 3);
         CheckRejected(Split("vertical", "second", 200, three, Leaf("CameraPreview")), "CameraPreview");
      }
   }
   SUBCASE("hidden Viewport")
   {
      CheckRejected(Split("vertical", "first", 200, Leaf("Outliner"), Leaf("Viewport", true)), "hidden");
   }
   SUBCASE("malformed nodes")
   {
      CheckRejected(json::array());
      CheckRejected(nullptr);
      CheckRejected(Leaf("Nonsense"));
      CheckRejected(json { { "editor", 3 } });
      CheckRejected(json::object());
      CheckRejected(Split("diagonal", "first", 200, Leaf("Outliner"), Leaf("Viewport")));
      CheckRejected(Split("vertical", "third", 200, Leaf("Outliner"), Leaf("Viewport")));
      CheckRejected(json { { "split", "vertical" }, { "fixed", "first" }, { "size", 200 }, { "children", json::array({ Leaf("Viewport") }) } });
      CheckRejected(json { { "split", "vertical" }, { "fixed", "first" }, { "size", 200 }, { "children", json::array({ Leaf("Viewport"), Leaf("Outliner"), Leaf("Script") }) } });
      json badHidden = Leaf("Outliner");
      badHidden["hidden"] = "yes";
      CheckRejected(Split("vertical", "first", 200, badHidden, Leaf("Viewport")));
   }
   SUBCASE("a single Viewport leaf is a valid layout")
   {
      AreaLayout l;
      REQUIRE(l.FromJson(Leaf("Viewport")));
      CHECK(l.LeafCount() == 1);
      CHECK(Compute(l).Leaf(l, AreaType::Viewport)->rect.Width() == 1920.f);
   }
}

TEST_CASE("AreaLayout node ids are stable across compaction")
{
   AreaLayout l = FromLegacy(kLayout);
   const int out = l.FindLeaf(AreaType::Outliner);
   const int vp = l.FindLeaf(AreaType::Viewport);
   const int props = l.FindLeaf(AreaType::Properties);

   // Index of a node in the vector
   auto indexOf = [&](const int id) {
      for (size_t i = 0; i < l.Nodes().size(); ++i)
         if (static_cast<int>(l.Nodes()[i].id) == id)
            return static_cast<int>(i);
      return -1;
   };
   const int propsIdxBefore = indexOf(props);

   int script = -1;
   REQUIRE(l.Split(vp, SplitAxis::Vertical, true, 300.f, &script));
   REQUIRE(script >= 0);
   const int splitId = l.ParentId(script);
   REQUIRE(l.Join(vp, script));

   // The split node and the Script leaf are gone, so the vector was compacted and indices moved ...
   CHECK(l.Find(script) == nullptr);
   CHECK(l.Find(splitId) == nullptr);
   CHECK(l.Nodes().size() == 5);
   // ... but ids of the surviving nodes are unchanged
   CHECK(l.FindLeaf(AreaType::Outliner) == out);
   CHECK(l.FindLeaf(AreaType::Viewport) == vp);
   CHECK(l.FindLeaf(AreaType::Properties) == props);
   CHECK(l.Find(vp)->type == AreaType::Viewport);
   CHECK(l.ParentId(vp) == l.ParentId(props));
   (void)propsIdxBefore;

   // Compaction really moves indices: join the Properties away from a Wiring layout
   AreaLayout w = MakeWiring();
   const int wOut = w.FindLeaf(AreaType::Outliner);
   const int wVp = w.FindLeaf(AreaType::Viewport);
   const int wScript = w.FindLeaf(AreaType::Script);
   const int wProps = w.FindLeaf(AreaType::Properties);
   int scriptIdxBefore = -1;
   for (size_t i = 0; i < w.Nodes().size(); ++i)
      if (static_cast<int>(w.Nodes()[i].id) == wScript)
         scriptIdxBefore = static_cast<int>(i);
   REQUIRE(w.Join(wScript, wProps));
   int scriptIdxAfter = -1;
   for (size_t i = 0; i < w.Nodes().size(); ++i)
      if (static_cast<int>(w.Nodes()[i].id) == wScript)
         scriptIdxAfter = static_cast<int>(i);
   CHECK(scriptIdxAfter >= 0);
   CHECK(scriptIdxAfter != scriptIdxBefore);
   CHECK(w.FindLeaf(AreaType::Outliner) == wOut);
   CHECK(w.FindLeaf(AreaType::Viewport) == wVp);
   CHECK(w.Find(wScript) == &w.Nodes()[scriptIdxAfter]);
   CHECK(w.Find(wProps) == nullptr);
   CHECK(w.Validate());

   // Ids are never reused, also not by a new layout
   const AreaLayout other = FromLegacy(kLayout);
   for (const AreaNode& n : other.Nodes())
   {
      CHECK(n.id != static_cast<uint32_t>(wOut));
      CHECK(n.id != static_cast<uint32_t>(wProps));
   }
}

TEST_CASE("AreaLayout split size edits")
{
   AreaLayout l = FromLegacy(kLayout);
   const int innerSplit = l.ParentId(l.FindLeaf(AreaType::Viewport));
   REQUIRE(l.SetSplitSize(innerSplit, 460.f));
   CHECK(Compute(l).Leaf(l, AreaType::Properties)->rect.x0 == 1460.f);
   CHECK_FALSE(l.SetSplitSize(innerSplit, std::numeric_limits<float>::quiet_NaN()));
   CHECK_FALSE(l.SetSplitSize(l.FindLeaf(AreaType::Viewport), 100.f)); // a leaf
   REQUIRE(l.SetSplitSize(innerSplit, -4.f));
   CHECK(l.Validate()); // clamped to a positive value
}
