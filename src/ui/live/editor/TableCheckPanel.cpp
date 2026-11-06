// license:GPLv3+

#include "core/stdafx.h"
#include "TableCheckPanel.h"
#include "IconsLucide.h"
#include "EditorTheme.h"
#include "EditorChrome.h"
#include "PlasticFromAtlas.h"

#include "math/MeshUtils.h"
#include "parts/Material.h"
#include "parts/ball.h"
#include "parts/bumper.h"
#include "parts/decal.h"
#include "parts/dispreel.h"
#include "parts/flasher.h"
#include "parts/flipper.h"
#include "parts/gate.h"
#include "parts/hittarget.h"
#include "parts/kicker.h"
#include "parts/light.h"
#include "parts/plunger.h"
#include "parts/primitive.h"
#include "parts/ramp.h"
#include "parts/rubber.h"
#include "parts/spinner.h"
#include "parts/surface.h"
#include "parts/trigger.h"
#include "renderer/Texture.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"

#include "imgui/imgui_internal.h"

namespace VPX::EditorUI
{

using Severity = TableCheckPanel::Severity;
using Finding = TableCheckPanel::Finding;

namespace
{

string Lower(const string &s)
{
   string r = s;
   for (char &c : r)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return r;
}

template <class... Args> string Fmt(const char *format, Args... args)
{
   char buf[256];
   snprintf(buf, sizeof(buf), format, args...);
   return buf;
}

// A reference from a part (or from the table settings) to an image or a material, by name
struct ResourceRef
{
   IEditable *part = nullptr; // null: a table setting
   string owner; // Part name, or the table name
   const char *field = "";
   string name;
   bool isImage = true;
   bool companion = false; // From a plastic's clear or bevel part: it counts as used, but a missing name is not listed on a part the author never sees
};

// What the checks work on, built once per scan
struct ScanContext
{
   PinTable &table;
   const vector<std::shared_ptr<EditorUIPart>> &uiParts;
   vector<IEditable *> parts; // Parts of the table listed in the editor, in outliner order (a plastic's clear and bevel parts and a rail's channel are not parts for the author)
   vector<IEditable *> companions; // Those, for the resources they use
   ankerl::unordered_dense::map<IEditable *, EditorUIPart *> uiOf;
   ankerl::unordered_dense::set<string> imageNames, materialNames; // Lower case
   vector<ResourceRef> refs;
   float marginX = 0.f, marginY = 0.f; // Allowed overshoot outside the playfield rectangle
};

using CheckFn = void (*)(const ScanContext &, vector<Finding> &);

struct CheckDef
{
   const char *title;
   CheckFn run;
};

Finding Make(const int check, const Severity severity, IEditable *part, string text)
{
   Finding f;
   f.check = check;
   f.severity = severity;
   f.part = part;
   f.name = part != nullptr ? part->GetName() : string();
   f.text = std::move(text);
   return f;
}

bool IsBackdropOrNonSpatial(const IEditable *part)
{
   if (part->m_desktopBackdrop)
      return true;
   switch (part->GetItemType())
   {
   case eItemPartGroup:
   case eItemLightSeq:
   case eItemTimer:
   case eItemBall:
   case eItemTextbox:
   case eItemDispReel: return true;
   default: return false;
   }
}

// ---------------------------------------------------------------------------------------------------------------------
// Resource references of the parts: every image and material name a part (or the table) stores

void CollectRefs(const PinTable &table, const ScanContext &ctx, vector<ResourceRef> &refs)
{
   const auto add = [&refs](IEditable *part, const string &owner, const char *field, const string &name, const bool isImage)
   {
      if (name.empty())
         return;
      ResourceRef r;
      r.part = part;
      r.owner = owner;
      r.field = field;
      r.name = name;
      r.isImage = isImage;
      r.companion = PlasticFromAtlas::IsCompanion(part);
      refs.push_back(std::move(r));
   };

   vector<IEditable *> withCompanions = ctx.parts;
   withCompanions.insert(withCompanions.end(), ctx.companions.begin(), ctx.companions.end());
   for (IEditable *const part : withCompanions)
   {
      const string &owner = part->GetName();
      const auto img = [&](const char *field, const string &name) { add(part, owner, field, name, true); };
      const auto mat = [&](const char *field, const string &name) { add(part, owner, field, name, false); };
      const auto base = [&](const BaseProperty &b)
      {
         img("Image", b.m_szImage);
         img("Normal Map", b.m_szNormalMap);
         img("ORM Map", b.m_szORMMap);
         img("Emission Map", b.m_szEmissionMap);
         mat("Material", b.m_szMaterial);
         mat("Physics Material", b.m_szPhysicsMaterial);
      };
      switch (part->GetItemType())
      {
      case eItemBall:
      {
         const auto &d = static_cast<Ball *>(part)->m_d;
         base(d);
         img("Decal Image", d.m_imageDecal);
         break;
      }
      case eItemBumper:
      {
         const auto &d = static_cast<Bumper *>(part)->m_d;
         base(d);
         mat("Cap Material", d.m_szCapMaterial);
         mat("Base Material", d.m_szBaseMaterial);
         mat("Skirt Material", d.m_szSkirtMaterial);
         mat("Ring Material", d.m_szRingMaterial);
         break;
      }
      case eItemDecal: base(static_cast<Decal *>(part)->m_d); break;
      case eItemDispReel: base(static_cast<DispReel *>(part)->m_d); break;
      case eItemFlasher:
      {
         const auto &d = static_cast<Flasher *>(part)->m_d;
         img("Image A", d.m_szImageA);
         img("Image B", d.m_szImageB);
         break;
      }
      case eItemFlipper:
      {
         const auto &d = static_cast<Flipper *>(part)->m_d;
         base(d);
         mat("Rubber Material", d.m_szRubberMaterial);
         break;
      }
      case eItemGate: base(static_cast<Gate *>(part)->m_d); break;
      case eItemHitTarget: base(static_cast<HitTarget *>(part)->m_d); break;
      case eItemKicker: base(static_cast<Kicker *>(part)->m_d); break;
      case eItemLight: base(static_cast<Light *>(part)->m_d); break;
      case eItemPlunger: base(static_cast<Plunger *>(part)->m_d); break;
      case eItemPrimitive:
      {
         const auto &d = static_cast<Primitive *>(part)->m_d;
         base(d);
         img("Dome Glow Map", d.m_domeGlowMap);
         img("Wood Mask", d.m_pfWoodMask); // Playfield Maker board (not drawn: Find Holes reads it)
         break;
      }
      case eItemRamp: base(static_cast<Ramp *>(part)->m_d); break;
      case eItemRubber: base(static_cast<Rubber *>(part)->m_d); break;
      case eItemSpinner: base(static_cast<Spinner *>(part)->m_d); break;
      case eItemSurface:
      {
         const auto &d = static_cast<Surface *>(part)->m_d;
         base(d);
         img("Side Image", d.m_szSideImage);
         mat("Top Material", d.m_szTopMaterial);
         mat("Side Material", d.m_szSideMaterial);
         mat("Slingshot Material", d.m_szSlingShotMaterial);
         break;
      }
      case eItemTrigger: base(static_cast<Trigger *>(part)->m_d); break;
      default: break;
      }
   }

   // The table settings
   const string tableLabel = "Table";
   add(nullptr, tableLabel, "Playfield Image", table.m_image, true);
   add(nullptr, tableLabel, "Playfield Material", table.m_playfieldMaterial, false);
   add(nullptr, tableLabel, "Ball Image", table.m_ballImage, true);
   add(nullptr, tableLabel, "Ball Decal", table.m_ballImageDecal, true);
   add(nullptr, tableLabel, "Environment Image", table.m_envImage, true);
   add(nullptr, tableLabel, "Color Grade Image", table.m_imageColorGrade, true);
   add(nullptr, tableLabel, "Screenshot", table.m_screenShot, true);
   for (const string &bg : table.m_BG_image)
      add(nullptr, tableLabel, "Backdrop Image", bg, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Checks. Each one appends its findings (with its own index in kChecks, passed as the check argument of Make)

constexpr int kCheckOutside = 0, kCheckMissing = 1, kCheckDuplicates = 2, kCheckColliders = 3, kCheckPoints = 4, kCheckCrossing = 5, kCheckUnused = 6;

// The outline a part really has, from its stored data (not the curve the editor is currently editing, see GetDragPointCurve): plastic hosts their
// outline (never the holes), metal wire guides their wire, lights only a custom shape (circle and PBR lights ignore their polygon)
const DragPointCurve *DataCurve(const ScanContext &ctx, IEditable *part)
{
   switch (part->GetItemType())
   {
   case eItemPrimitive:
   {
      Primitive *const prim = static_cast<Primitive *>(part);
      return prim->m_d.m_plasticGen ? &prim->m_plasticCurve : prim->m_d.m_wireGen ? &prim->m_wireCurve : nullptr;
   }
   case eItemLight:
   {
      Light *const light = static_cast<Light *>(part);
      return light->m_d.m_type != LightType::PBR && light->m_d.m_shape == ShapeCustom ? &light->m_curve : nullptr;
   }
   default:
   {
      const auto it = ctx.uiOf.find(part);
      return it != ctx.uiOf.end() ? it->second->GetDragPointCurve() : nullptr;
   }
   }
}

// Parts that are often put off the playfield on purpose (cabinet parts, toys, backglass lights and flashers): only informational when outside it
bool IsOftenOffPlayfield(const IEditable *part)
{
   switch (part->GetItemType())
   {
   case eItemPrimitive:
   case eItemLight:
   case eItemFlasher:
   case eItemDecal: return true;
   default: return false;
   }
}

// Parts outside the playfield rectangle (plus a margin). Backdrop parts (backglass, DMD, reels) are skipped
void CheckOutsidePlayfield(const ScanContext &ctx, vector<Finding> &out)
{
   const PinTable &t = ctx.table;
   if (t.m_right <= t.m_left || t.m_bottom <= t.m_top)
      return;
   for (IEditable *const part : ctx.parts)
   {
      if (IsBackdropOrNonSpatial(part))
         continue;
      float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
      const auto include = [&](const float x, const float y)
      {
         minX = min(minX, x);
         maxX = max(maxX, x);
         minY = min(minY, y);
         maxY = max(maxY, y);
      };
      const DragPointCurve *const curve = DataCurve(ctx, part);
      if (curve != nullptr && !curve->GetPoints().empty())
         for (const auto &p : curve->GetPoints())
            include(p->GetX(), p->GetY());
      else
      {
         const Vertex2D c = part->GetCenter();
         include(c.x, c.y);
      }
      string where;
      const auto side = [&where](const char *name, const float over)
      {
         where += (where.empty() ? "" : ", ") + Fmt("%.0f past the %s edge", over, name);
      };
      if (t.m_left - minX > ctx.marginX)
         side("left", t.m_left - minX);
      if (maxX - t.m_right > ctx.marginX)
         side("right", maxX - t.m_right);
      if (t.m_top - minY > ctx.marginY)
         side("top", t.m_top - minY);
      if (maxY - t.m_bottom > ctx.marginY)
         side("bottom", maxY - t.m_bottom);
      if (!where.empty())
         out.push_back(Make(kCheckOutside, IsOftenOffPlayfield(part) ? Severity::Info : Severity::Warning, part, "Outside the playfield: " + where));
   }
}

// An image or material name that does not resolve in the table
void CheckMissingResources(const ScanContext &ctx, vector<Finding> &out)
{
   for (const ResourceRef &r : ctx.refs)
   {
      if (r.companion)
         continue;
      const bool found = (r.isImage ? ctx.imageNames : ctx.materialNames).contains(Lower(r.name));
      if (found)
         continue;
      Finding f = Make(kCheckMissing, Severity::Warning, r.part, r.field + string(": ") + (r.isImage ? "no image named \"" : "no material named \"") + r.name + "\"");
      if (r.part == nullptr)
         f.name = r.owner;
      out.push_back(std::move(f));
   }
}

// Script names are case insensitive
void CheckDuplicateNames(const ScanContext &ctx, vector<Finding> &out)
{
   ankerl::unordered_dense::map<string, int> counts;
   for (IEditable *const part : ctx.parts)
      if (!part->GetName().empty())
         ++counts[Lower(part->GetName())];
   for (IEditable *const part : ctx.parts)
   {
      if (part->GetName().empty())
         continue;
      const int n = counts[Lower(part->GetName())];
      if (n > 1)
         out.push_back(Make(kCheckDuplicates, Severity::Error, part, Fmt("The name is shared with %d other part%s (scripts reach only one of them)", n - 1, n == 2 ? "" : "s")));
   }
}

// Collidable parts that cannot collide: no height, no thickness
void CheckColliders(const ScanContext &ctx, vector<Finding> &out)
{
   for (IEditable *const part : ctx.parts)
   {
      switch (part->GetItemType())
      {
      case eItemSurface:
      {
         const auto &d = static_cast<Surface *>(part)->m_d;
         if (d.m_collidable && d.m_heighttop <= d.m_heightbottom)
            out.push_back(Make(kCheckColliders, Severity::Error, part,
               Fmt("Collidable wall has no height: top %.1f is not above bottom %.1f", d.m_heighttop, d.m_heightbottom)));
         break;
      }
      case eItemRamp:
      {
         const auto &d = static_cast<Ramp *>(part)->m_d;
         // Only a flat ramp is judged by its widths (wire ramps use their wire), and only when both ends have none: the engine skips it then, not at a point
         if (d.m_collidable && d.m_type == RampTypeFlat && d.m_widthbottom <= 0.f && d.m_widthtop <= 0.f)
            out.push_back(Make(kCheckColliders, Severity::Error, part, "Collidable flat ramp has no width at either end"));
         break;
      }
      case eItemRubber:
      {
         const auto &d = static_cast<Rubber *>(part)->m_d;
         if (d.m_collidable && d.m_thickness <= 0)
            out.push_back(Make(kCheckColliders, Severity::Error, part, Fmt("Collidable rubber has no thickness (%d)", d.m_thickness)));
         break;
      }
      case eItemBumper:
         if (static_cast<Bumper *>(part)->m_d.m_radius <= 0.f)
            out.push_back(Make(kCheckColliders, Severity::Error, part, "Bumper has no radius"));
         break;
      case eItemKicker:
         if (static_cast<Kicker *>(part)->m_d.m_radius <= 0.f)
            out.push_back(Make(kCheckColliders, Severity::Error, part, "Kicker has no radius"));
         break;
      default: break;
      }
   }
}

// Closed outlines: the last drag point joins the first
bool IsClosedCurve(const IEditable *part)
{
   switch (part->GetItemType())
   {
   case eItemSurface:
   case eItemFlasher:
   case eItemLight:
   case eItemRubber:
   case eItemTrigger: return true;
   case eItemPrimitive: return static_cast<const Primitive *>(part)->m_d.m_plasticGen; // The plastic host's outline
   default: return false;
   }
}

// Drag points: too few, or consecutive points on the same spot (a zero length segment)
void CheckDragPoints(const ScanContext &ctx, vector<Finding> &out)
{
   for (IEditable *const part : ctx.parts)
   {
      const DragPointCurve *const curve = DataCurve(ctx, part);
      if (curve == nullptr)
         continue;
      const auto &pts = curve->GetPoints();
      const int n = static_cast<int>(pts.size());
      if (n < curve->GetMinimumPoints())
      {
         out.push_back(Make(kCheckPoints, Severity::Error, part, Fmt("Only %d drag point%s (at least %d needed)", n, n == 1 ? "" : "s", curve->GetMinimumPoints())));
         continue;
      }
      const bool closed = IsClosedCurve(part);
      int zero = 0, first = -1;
      for (int i = 0; i < (closed ? n : n - 1); ++i)
      {
         const int j = (i + 1) % n;
         if (pts[i]->GetX() == pts[j]->GetX() && pts[i]->GetY() == pts[j]->GetY() && pts[i]->GetZ() == pts[j]->GetZ()) // (a straight drop is not zero length)
         {
            if (first < 0)
               first = i;
            ++zero;
         }
      }
      if (zero > 0)
         out.push_back(Make(kCheckPoints, Severity::Warning, part,
            zero == 1 ? Fmt("Points %d and %d are on the same spot (zero length segment)", first + 1, (first + 1) % n + 1)
                      : Fmt("%d zero length segments (first: points %d and %d)", zero, first + 1, (first + 1) % n + 1)));
   }
}

// Proper crossing of two segments (shared end points and touching do not count)
bool SegmentsCross(const Vertex2D &a, const Vertex2D &b, const Vertex2D &c, const Vertex2D &d)
{
   if (max(a.x, b.x) < min(c.x, d.x) || max(c.x, d.x) < min(a.x, b.x) || max(a.y, b.y) < min(c.y, d.y) || max(c.y, d.y) < min(a.y, b.y))
      return false; // Boxes apart
   const auto cross = [](const Vertex2D &o, const Vertex2D &p, const Vertex2D &q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
   const float d1 = cross(a, b, c), d2 = cross(a, b, d), d3 = cross(c, d, a), d4 = cross(c, d, b);
   return ((d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f)) && ((d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f));
}

// Closed outlines that cross themselves. The curve is smoothed coarsely (a straight line between the points is enough to see a fold), so even long
// rubbers fit the vertex cap; an outline still over the cap gets an Info row instead of being skipped silently
void CheckSelfCrossing(const ScanContext &ctx, vector<Finding> &out)
{
   constexpr size_t kMaxVertices = 1200;
   for (IEditable *const part : ctx.parts)
   {
      if (!IsClosedCurve(part))
         continue;
      const DragPointCurve *const curve = DataCurve(ctx, part);
      if (curve == nullptr || static_cast<int>(curve->GetPoints().size()) < max(3, curve->GetMinimumPoints()))
         continue;
      vector<RenderVertex> v;
      for (const float accuracy : { 16.f, 256.f, 4096.f }) // (a higher value flattens more: the default is 4)
      {
         v.clear();
         curve->GetRgVertex(v, true, accuracy);
         if (v.size() <= kMaxVertices)
            break;
      }
      const size_t n = v.size();
      if (n > kMaxVertices)
      {
         out.push_back(Make(kCheckCrossing, Severity::Info, part, "Outline too long to check for crossings"));
         continue;
      }
      if (n < 4)
         continue;
      bool crossed = false;
      for (size_t i = 0; i < n && !crossed; ++i)
         for (size_t j = i + 2; j < n && !crossed; ++j)
         {
            if (i == 0 && j == n - 1)
               continue; // Neighbours across the closing segment
            crossed = SegmentsCross(v[i], v[(i + 1) % n], v[j], v[(j + 1) % n]);
         }
      if (crossed)
         out.push_back(Make(kCheckCrossing, Severity::Warning, part, "The outline crosses itself"));
   }
}

// Images and materials nothing refers to (a script may still use them by name)
void CheckUnused(const ScanContext &ctx, vector<Finding> &out)
{
   ankerl::unordered_dense::set<string> usedImages, usedMaterials;
   for (const ResourceRef &r : ctx.refs)
      (r.isImage ? usedImages : usedMaterials).insert(Lower(r.name));
   for (Texture *const image : ctx.table.GetImageList())
      if (!usedImages.contains(Lower(image->m_name)))
      {
         Finding f = Make(kCheckUnused, Severity::Info, nullptr, "Image not used by any part or table setting (a script may still use it)");
         f.name = image->m_name;
         f.image = image;
         out.push_back(std::move(f));
      }
   for (Material *const material : ctx.table.GetMaterialList())
      if (!usedMaterials.contains(Lower(material->m_name)))
      {
         Finding f = Make(kCheckUnused, Severity::Info, nullptr, "Material not used by any part or table setting");
         f.name = material->m_name;
         f.material = material;
         out.push_back(std::move(f));
      }
}

// THE CHECK LIST. To add a check: write a function like the ones above, give it a constant index here (the order of this list is the order of the
// groups in the area; keep the kCheck* constants matching) and add a line
constexpr CheckDef kChecks[] = {
   { "Outside the playfield", CheckOutsidePlayfield },
   { "Missing image or material", CheckMissingResources },
   { "Duplicate part names", CheckDuplicateNames },
   { "Colliders that cannot collide", CheckColliders },
   { "Drag points", CheckDragPoints },
   { "Self-crossing outlines", CheckSelfCrossing },
   { "Unused images and materials", CheckUnused },
};
static_assert(std::size(kChecks) == kCheckUnused + 1);

ImU32 SeverityColor(const Severity s)
{
   return s == Severity::Error ? EditorTheme::Rgb(224, 84, 84) : s == Severity::Warning ? EditorTheme::Rgb(232, 172, 52) : EditorTheme::Rgb(116, 164, 222);
}

const char *SeverityIcon(const Severity s) { return s == Severity::Error ? ICON_LC_X : s == Severity::Warning ? ICON_LC_TRIANGLE_ALERT : ICON_LC_CIRCLE_DOT; }

const char *SeverityName(const Severity s) { return s == Severity::Error ? "Errors" : s == Severity::Warning ? "Warnings" : "Info"; }

} // namespace

void TableCheckPanel::Scan()
{
   m_findings.clear();
   EditorUI &editor = m_editor;
   PinTable &table = *editor.m_table;
   ScanContext ctx { table, editor.m_editables };
   ankerl::unordered_dense::set<const IEditable *> live(table.GetParts().begin(), table.GetParts().end());
   for (const auto &uiPart : editor.m_editables)
   {
      IEditable *const edit = uiPart->GetEditable();
      if (edit == nullptr || !live.contains(edit))
         continue;
      if (PlasticFromAtlas::IsCompanion(edit))
      {
         ctx.companions.push_back(edit); // A plastic's clear and bevel parts, a Wire Rail's hidden channel: not listed by the Outliner either, and derived from the host
         continue;
      }
      ctx.parts.push_back(edit);
      ctx.uiOf[edit] = uiPart.get();
   }
   for (const Texture *const image : table.GetImageList())
      ctx.imageNames.insert(Lower(image->m_name));
   for (const Material *const material : table.GetMaterialList())
      ctx.materialNames.insert(Lower(material->m_name));
   ctx.marginX = 0.05f * (table.m_right - table.m_left);
   ctx.marginY = 0.05f * (table.m_bottom - table.m_top);
   CollectRefs(table, ctx, ctx.refs);
   for (const CheckDef &check : kChecks)
      check.run(ctx, m_findings);
   m_scanned = true;
}

void TableCheckPanel::SelectFinding(const Finding &f, const bool frame)
{
   EditorUI &editor = m_editor;
   PinTable &table = *editor.m_table;
   if (f.part != nullptr)
   {
      // The finding may be older than the table: only a part that is still there is selected
      const auto it = editor.m_editableMap.find(f.part);
      if (it == editor.m_editableMap.end() || !table.HasPart(f.part))
         return;
      // The editor drops the selected parts that are not of the camera mode (backdrop or not) at the next frame: keep the selection, say why instead
      if (f.part->m_desktopBackdrop != editor.IsBackdropEditMode())
      {
         // (one notification, a double click selects twice; a part group is on no side)
         const string text = f.part->GetItemType() == eItemPartGroup ? "\""s + f.name + "\" is a part group: it cannot be selected in the Desktop Backdrop view"s
            : f.part->m_desktopBackdrop                              ? "\""s + f.name + "\" is on the backdrop: switch to the Desktop Backdrop view to see it"s
                                                                     : "\""s + f.name + "\" is not on the backdrop: leave the Desktop Backdrop view to see it"s;
         m_notifyId = editor.m_liveUI.PushNotification(text, 5000, m_notifyId);
         return;
      }
      editor.SetSelection(Selection(it->second));
      editor.m_outlinerAnchor = it->second;
      if (frame)
         editor.FrameSelection();
   }
   else if (f.image != nullptr)
   {
      if (std::ranges::find(table.GetImageList(), f.image) != table.GetImageList().end())
         editor.SetSelection(Selection(f.image));
   }
   else if (f.material != nullptr)
   {
      if (std::ranges::find(table.GetMaterialList(), f.material) != table.GetMaterialList().end())
         editor.SetSelection(Selection(f.material));
   }
   else
      editor.SetSelection(Selection()); // A table setting: nothing selected shows the table's properties
}

void TableCheckPanel::RenderInArea(const ImVec2 &pos, const ImVec2 &size, const int leafId)
{
   EditorUI &editor = m_editor;

   // Change detection: cheap numbers every frame, the scan itself runs a moment after the last change and never during a drag. The end of an interaction
   // (item deactivated, mouse released) schedules a scan only if something changed since the last one: an edit coalesced into one undo record changes the
   // signature at its first frame, and its final value is only known when it ends. Only a held mouse button defers a scan: an item left active (a number
   // field in type mode) does not, so undo, redo and delete refresh the list at once
   const double now = ImGui::GetTime();
   const ImGuiID activeId = ImGui::GetActiveID();
   const bool endOfInteraction = (m_prevActiveId != 0 && activeId == 0) || ImGui::IsMouseReleased(ImGuiMouseButton_Left);
   m_prevActiveId = activeId;
   Signature sig;
   sig.table = editor.m_table;
   sig.topRecord = editor.m_undo.TopRecordId();
   sig.parts = editor.m_table->GetParts().size();
   sig.images = editor.m_table->GetImageList().size();
   sig.materials = editor.m_table->GetMaterialList().size();
   sig.dirty = static_cast<int>(editor.m_table->GetDirtyState());
   {
      const PinTable &t = *editor.m_table;
      sig.bounds[0] = t.m_left;
      sig.bounds[1] = t.m_top;
      sig.bounds[2] = t.m_right;
      sig.bounds[3] = t.m_bottom;
      sig.names = m_signature.names;
      // The names (image / material renames, table settings: edits with no undo record) are hashed only when something may have changed them: an
      // interaction ended or the cheap numbers moved
      Signature cheap = sig;
      cheap.names = m_signature.names;
      if (endOfInteraction || !m_scanned || !(cheap == m_signature))
      {
         std::hash<string> h;
         size_t acc = 0;
         const auto mix = [&acc, &h](const string &str) { acc = acc * 1099511628211ull + h(str) + 0x9e3779b9; };
         for (const Texture *const image : t.GetImageList())
            mix(image->m_name);
         for (const Material *const material : t.GetMaterialList())
            mix(material->m_name);
         for (const string *str : { &t.m_image, &t.m_playfieldMaterial, &t.m_ballImage, &t.m_ballImageDecal, &t.m_envImage, &t.m_imageColorGrade, &t.m_screenShot })
            mix(*str);
         for (const string &bg : t.m_BG_image)
            mix(bg);
         sig.names = acc;
      }
   }
   const bool dragging = editor.IsDragInProgress() || ImGui::IsMouseDown(ImGuiMouseButton_Left);
   if (!(sig == m_signature))
   {
      m_signature = sig;
      m_changedAt = now;
      m_pending = true;
      m_unsettled = true;
   }
   if (endOfInteraction && m_unsettled)
   {
      m_unsettled = false;
      m_pending = true;
      m_changedAt = now;
   }
   if (dragging)
      m_changedAt = now; // Settles a moment after the drag ends
   if (!m_scanned || (m_pending && !dragging && now - m_changedAt >= 0.35))
   {
      Scan();
      m_pending = false;
   }

   ImGui::SetNextWindowPos(pos);
   ImGui::SetNextWindowSize(size);
   constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
      | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
   const float dpi = editor.m_liveUI.GetDPI();
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(EditorTheme::Rgb(40, 40, 40)));
   ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::V(EditorTheme::Rgb(40, 40, 40)));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * dpi, 4.0f * dpi));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   ImGui::Begin(EditorUI::AreaWindowName("TABLECHECK", leafId).c_str(), nullptr, window_flags);
   const ImGuiStyle &style = ImGui::GetStyle();

   int counts[3] = {};
   for (const Finding &f : m_findings)
      ++counts[static_cast<int>(f.severity)];

   // Header: editor type, refresh, severity filters
   ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(EditorTheme::Rgb(51, 51, 55)));
   editor.RenderAreaTypeButton(EditorUI::AreaEditor::TableCheck, leafId);
   ImGui::SameLine();
   if (ImGui::Button(ICON_LC_REFRESH_CW "##TableCheckRefresh"))
   {
      Scan();
      m_pending = false;
   }
   ImGui::SetItemTooltip("Check the table again (it also re-checks by itself a moment after the table changes)");
   for (int s = 0; s < 3; ++s)
   {
      ImGui::SameLine();
      const Severity sev = static_cast<Severity>(s);
      const string label = string(SeverityIcon(sev)) + " " + std::to_string(counts[s]) + "##TableCheckSev" + std::to_string(s);
      ImGui::PushStyleColor(ImGuiCol_Text, m_show[s] ? SeverityColor(sev) : EditorTheme::TextFaint);
      if (m_show[s])
         ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(EditorTheme::Raised));
      if (ImGui::Button(label.c_str()))
         m_show[s] = !m_show[s];
      if (m_show[s])
         ImGui::PopStyleColor();
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("%s: %d. Click to %s them", SeverityName(sev), counts[s], m_show[s] ? "hide" : "show");
   }
   ImGui::PopStyleColor();
   ImGui::Separator();

   // The list: one collapsible group per check
   // The list stops short of the bottom corners, as the Outliner's tree does: a corner drag cannot start over a scrollbar or a row, and the list's scrollbar
   // runs down to the area's corner otherwise (a drag from there would pull the scrollbar thumb)
   const float cornerMargin = 14.f * dpi - style.WindowPadding.y;
   ImGui::BeginChild("##TableCheckList", ImVec2(0, max(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetContentRegionAvail().y - cornerMargin)), ImGuiChildFlags_None);
   const float rowH = ImGui::GetTextLineHeightWithSpacing() + 2.f * dpi;
   const float iconW = 20.f * dpi;
   bool anyShown = false;
   // One pass groups the rows that pass the filter by check; only the rows in view are drawn (a table can have hundreds of unused images and materials)
   vector<const Finding *> groups[std::size(kChecks)];
   for (const Finding &f : m_findings)
      if (m_show[static_cast<int>(f.severity)])
         groups[f.check].push_back(&f);
   const float rowPitch = rowH + style.ItemSpacing.y;
   for (int c = 0; c < static_cast<int>(std::size(kChecks)); ++c)
   {
      const vector<const Finding *> &rows = groups[c];
      if (rows.empty())
         continue;
      anyShown = true;
      const string header = string(kChecks[c].title) + "  (" + std::to_string(rows.size()) + ")###TableCheckGroup" + std::to_string(c);
      if (!ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
         continue;
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(rows.size()), rowPitch);
      while (clipper.Step())
         for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
         {
            const Finding &f = *rows[index];
            ImGui::PushID(index);
            const ImVec2 rowPos = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            const bool selectedRow = f.part != nullptr && editor.m_editableMap.contains(f.part) && editor.IsPartSelected(editor.m_editableMap[f.part]);
            const bool clicked = ImGui::Selectable("##row", selectedRow, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowH));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList *const drawList = ImGui::GetWindowDrawList();
            const float textY = rowPos.y + 0.5f * (rowH - ImGui::GetTextLineHeight());
            drawList->AddText(ImVec2(rowPos.x + 4.f * dpi, textY), SeverityColor(f.severity), SeverityIcon(f.severity));
            const float nameW = min(rowW * 0.35f, 220.f * dpi);
            drawList->PushClipRect(ImVec2(rowPos.x + iconW, rowPos.y), ImVec2(rowPos.x + iconW + nameW - 6.f * dpi, rowPos.y + rowH), true);
            drawList->AddText(ImVec2(rowPos.x + iconW, textY), EditorTheme::Text, f.name.c_str());
            drawList->PopClipRect();
            drawList->PushClipRect(ImVec2(rowPos.x + iconW + nameW, rowPos.y), ImVec2(rowPos.x + rowW, rowPos.y + rowH), true);
            drawList->AddText(ImVec2(rowPos.x + iconW + nameW, textY), EditorTheme::TextDim, f.text.c_str());
            drawList->PopClipRect();
            if (hovered && ImGui::GetDragDropPayload() == nullptr)
               ImGui::SetTooltip("%s\n%s", f.name.c_str(), f.text.c_str());
            if (clicked)
               SelectFinding(f, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            ImGui::PopID();
         }
      ImGui::TreePop();
   }
   if (!anyShown)
   {
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
      ImGui::TextUnformatted(m_findings.empty() ? "No problems found" : "Everything found is hidden by the filters above");
      ImGui::PopStyleColor();
   }
   ImGui::EndChild();

   ImGui::End();
   ImGui::PopStyleVar(3);
   ImGui::PopStyleColor(2);
}

}
