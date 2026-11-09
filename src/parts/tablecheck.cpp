// license:GPLv3+

#include "core/stdafx.h"
#include "tablecheck.h"

#include "audio/AudioPlayer.h"
#include "math/MeshUtils.h"
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
#include "parts/Material.h"
#include "parts/PartGroup.h"
#include "parts/plunger.h"
#include "parts/primitive.h"
#include "parts/ramp.h"
#include "parts/rubber.h"
#include "parts/Sound.h"
#include "parts/spinner.h"
#include "parts/surface.h"
#include "parts/textbox.h"
#include "parts/timer.h"
#include "parts/trigger.h"
#include "renderer/Texture.h"

const char *TableCheckTitle(const TableCheckId check)
{
   switch (check)
   {
   case TableCheckId::ScriptDuplicate: return "Duplicate script declarations";
   case TableCheckId::ScriptExecute: return "Script 'Execute' command";
   case TableCheckId::Glass: return "Glass height";
   case TableCheckId::BallMapping: return "Ball mapping";
   case TableCheckId::LoadFix: return "Fixed when loaded";
   case TableCheckId::StaticReferenced: return "Static parts used by the script";
   case TableCheckId::TextboxDMD: return "Textbox used as DMD";
   case TableCheckId::FastTimer: return "Fast timers";
   case TableCheckId::Translucency: return "Translucency on opaque primitives";
   case TableCheckId::LightIntensity: return "Light intensity";
   case TableCheckId::VideoCap: return "Flasher VideoCap API";
   case TableCheckId::VPM: return "VPinMAME setup";
   case TableCheckId::OutsidePlayfield: return "Outside the playfield";
   case TableCheckId::MissingResource: return "Missing images and materials";
   case TableCheckId::DuplicateName: return "Duplicate part names";
   case TableCheckId::Collider: return "Colliders that can not collide";
   case TableCheckId::DragPoints: return "Drag points";
   case TableCheckId::SelfCrossing: return "Self-crossing outlines";
   case TableCheckId::UnusedMaterial: return "Unused materials";
   case TableCheckId::Sound: return "Sounds";
   case TableCheckId::Backdrop: return "Backdrop images";
   case TableCheckId::Totals: return "Totals";
   case TableCheckId::Count: break;
   }
   return "";
}

string FormatTableAudit(const vector<TableIssue> &issues)
{
   std::stringstream ss;
   bool listed = false; // An issue was listed: "No issue identified." goes before the totals otherwise
   for (const TableIssue &issue : issues)
   {
      if (issue.check == TableCheckId::Totals)
      {
         if (!listed)
            ss << "No issue identified.\r\n";
         listed = true;
         ss << ". " << issue.message << "\r\n";
         continue;
      }
      listed = true;
      ss << (issue.severity == TableIssueSeverity::Error ? ". Error: " : issue.severity == TableIssueSeverity::Warning ? ". Warning: " : ". ") << issue.message << "\r\n";
   }
   if (!listed)
      ss << "No issue identified.\r\n";
   return "Table audit:\r\n" + ss.str();
}

namespace
{

// The data block of the parts that have the common properties (image, material, physics material)
const BaseProperty *BaseOf(const IEditable *part)
{
   switch (part->GetItemType())
   {
   case eItemBall: return &static_cast<const Ball *>(part)->m_d;
   case eItemBumper: return &static_cast<const Bumper *>(part)->m_d;
   case eItemDecal: return &static_cast<const Decal *>(part)->m_d;
   case eItemDispReel: return &static_cast<const DispReel *>(part)->m_d;
   case eItemFlipper: return &static_cast<const Flipper *>(part)->m_d;
   case eItemGate: return &static_cast<const Gate *>(part)->m_d;
   case eItemHitTarget: return &static_cast<const HitTarget *>(part)->m_d;
   case eItemKicker: return &static_cast<const Kicker *>(part)->m_d;
   case eItemLight: return &static_cast<const Light *>(part)->m_d;
   case eItemPlunger: return &static_cast<const Plunger *>(part)->m_d;
   case eItemPrimitive: return &static_cast<const Primitive *>(part)->m_d;
   case eItemRamp: return &static_cast<const Ramp *>(part)->m_d;
   case eItemRubber: return &static_cast<const Rubber *>(part)->m_d;
   case eItemSpinner: return &static_cast<const Spinner *>(part)->m_d;
   case eItemSurface: return &static_cast<const Surface *>(part)->m_d;
   case eItemTrigger: return &static_cast<const Trigger *>(part)->m_d;
   default: return nullptr;
   }
}

// The outline a part uses (its drag points), if any
const DragPointCurve *OutlineOf(const IEditable *part)
{
   switch (part->GetItemType())
   {
   case eItemSurface: return &static_cast<const Surface *>(part)->m_curve;
   case eItemRamp: return &static_cast<const Ramp *>(part)->m_curve;
   case eItemRubber: return &static_cast<const Rubber *>(part)->m_curve;
   case eItemFlasher: return &static_cast<const Flasher *>(part)->m_curve;
   case eItemLight: return static_cast<const Light *>(part)->m_d.m_shape == ShapeCustom ? &static_cast<const Light *>(part)->m_curve : nullptr;
   case eItemTrigger:
   {
      const TriggerShape shape = static_cast<const Trigger *>(part)->m_d.m_shape;
      return shape != TriggerStar && shape != TriggerButton ? &static_cast<const Trigger *>(part)->m_curve : nullptr; // Star and button triggers are round
   }
   default: return nullptr;
   }
}

// Closed outlines: the last drag point joins the first
bool IsClosedOutline(const IEditable *part) { return part->GetItemType() != eItemRamp; }

// Parts that are often put off the playfield on purpose (cabinet parts, toys, backglass lights and flashers)
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

bool HasNoPlayfieldPosition(const IEditable *part)
{
   if (part->m_desktopBackdrop || (part->GetPartGroup() != nullptr && part->GetPartGroup()->GetReferenceSpace() != PartGroupData::SpaceReference::SR_PLAYFIELD))
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

// Proper crossing of two segments (shared end points and touching do not count)
bool SegmentsCross(const Vertex2D &a, const Vertex2D &b, const Vertex2D &c, const Vertex2D &d)
{
   if (max(a.x, b.x) < min(c.x, d.x) || max(c.x, d.x) < min(a.x, b.x) || max(a.y, b.y) < min(c.y, d.y) || max(c.y, d.y) < min(a.y, b.y))
      return false;
   const auto cross = [](const Vertex2D &o, const Vertex2D &p, const Vertex2D &q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
   const float d1 = cross(a, b, c), d2 = cross(a, b, d), d3 = cross(c, d, a), d4 = cross(c, d, b);
   return ((d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f)) && ((d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f));
}

string PartLabel(const IEditable *part) { return part != nullptr ? "Part '" + part->GetName() + '\'' : "Table"s; }

}

void ForEachResourceReference(const PinTable &table, const std::function<void(IEditable *part, const char *field, const string &name, bool isImage)> &fn)
{
   for (IEditable *const part : table.GetParts())
   {
      const auto img = [&](const char *field, const string &name) { if (!name.empty()) fn(part, field, name, true); };
      const auto mat = [&](const char *field, const string &name) { if (!name.empty()) fn(part, field, name, false); };
      if (const BaseProperty *const base = BaseOf(part))
      {
         img(part->GetItemType() == eItemSurface ? "Top Image" : "Image", base->m_szImage);
         mat("Material", base->m_szMaterial);
         if (!base->m_overwritePhysics) // Else the part's own physics values are used, not the material's
            mat("Physics Material", base->m_szPhysicsMaterial);
      }
      switch (part->GetItemType())
      {
      case eItemBall: img("Decal Image", static_cast<const Ball *>(part)->m_d.m_imageDecal); break;
      case eItemBumper:
      {
         const auto &d = static_cast<const Bumper *>(part)->m_d;
         mat("Cap Material", d.m_szCapMaterial);
         mat("Base Material", d.m_szBaseMaterial);
         mat("Skirt Material", d.m_szSkirtMaterial);
         mat("Ring Material", d.m_szRingMaterial);
         break;
      }
      case eItemFlasher:
         img("Image A", static_cast<const Flasher *>(part)->m_d.m_szImageA);
         img("Image B", static_cast<const Flasher *>(part)->m_d.m_szImageB);
         break;
      case eItemFlipper: mat("Rubber Material", static_cast<const Flipper *>(part)->m_d.m_szRubberMaterial); break;
      case eItemPrimitive: img("Normal Map", static_cast<const Primitive *>(part)->m_d.m_szNormalMap); break;
      case eItemSurface:
      {
         const auto &d = static_cast<const Surface *>(part)->m_d;
         img("Side Image", d.m_szSideImage);
         mat("Top Material", d.m_szTopMaterial);
         mat("Side Material", d.m_szSideMaterial);
         mat("Slingshot Material", d.m_szSlingShotMaterial);
         break;
      }
      default: break;
      }
   }
   const auto img = [&](const char *field, const string &name) { if (!name.empty()) fn(nullptr, field, name, true); };
   img("Playfield Image", table.m_image);
   img("Ball Image", table.m_ballImage);
   img("Ball Decal", table.m_ballImageDecal);
   img("Environment Image", table.m_envImage);
   img("Color Grade Image", table.m_imageColorGrade);
   img("Screenshot", table.m_screenShot);
   img("Desktop Backdrop Image", table.m_BG_image[BG_DESKTOP]);
   img("Cabinet Backdrop Image", table.m_BG_image[BG_FULLSCREEN]);
   img("Full Single Screen Backdrop Image", table.m_BG_image[BG_FSS]);
   if (!table.m_playfieldMaterial.empty())
      fn(nullptr, "Playfield Material", table.m_playfieldMaterial, false);
}

namespace
{

// The checks of the parts
void CheckParts(const PinTable &table, vector<TableIssue> &issues)
{
   const auto add = [&issues](const TableIssueSeverity severity, const TableCheckId check, string message, IEditable *part = nullptr)
   { issues.push_back({ severity, check, std::move(message), part }); };
   constexpr TableIssueSeverity Error = TableIssueSeverity::Error, Warning = TableIssueSeverity::Warning, Info = TableIssueSeverity::Info;

   // The parts of the authored table (not the objects made by a running player session)
   vector<IEditable *> parts;
   parts.reserve(table.GetParts().size());
   for (IEditable *const part : table.GetParts())
      if (!table.IsPlayerSessionObject(part))
         parts.push_back(part);

   // Parts outside the playfield rectangle, with a 5% margin
   if (table.m_right > table.m_left && table.m_bottom > table.m_top)
   {
      const float marginX = 0.05f * (table.m_right - table.m_left), marginY = 0.05f * (table.m_bottom - table.m_top);
      for (IEditable *const part : parts)
      {
         if (HasNoPlayfieldPosition(part))
            continue;
         // Visual parts the ball never touches (lights, flashers, decals, primitives that do not collide) are often put off the
         // playfield on purpose (backbox, backglass, cabinet, room), also in playfield space: left out
         if (const BaseProperty *const base = BaseOf(part); IsOftenOffPlayfield(part) && (base == nullptr || !base->m_collidable))
            continue;
         float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
         const auto include = [&](const float x, const float y)
         {
            minX = min(minX, x);
            maxX = max(maxX, x);
            minY = min(minY, y);
            maxY = max(maxY, y);
         };
         const DragPointCurve *const curve = OutlineOf(part);
         if (curve != nullptr && !curve->GetPoints().empty())
            for (const auto &p : curve->GetPoints())
               include(p->GetX(), p->GetY());
         else
            include(part->GetCenter().x, part->GetCenter().y);
         string where;
         const auto side = [&where](const char *edge, const float over) { where += (where.empty() ? "" : ", ") + std::format("{:.0f} past the {} edge", over, edge); };
         if (table.m_left - minX > marginX)
            side("left", table.m_left - minX);
         if (maxX - table.m_right > marginX)
            side("right", maxX - table.m_right);
         if (table.m_top - minY > marginY)
            side("top", table.m_top - minY);
         if (maxY - table.m_bottom > marginY)
            side("bottom", maxY - table.m_bottom);
         if (!where.empty())
            add(IsOftenOffPlayfield(part) ? Info : Warning, TableCheckId::OutsidePlayfield, "Part '" + part->GetName() + "' is outside the playfield: " + where, part);
      }
   }

   // Image and material names that are not in the table (backdrop images are reported by the backdrop check)
   ankerl::unordered_dense::set<string> imageNames, materialNames, usedMaterials;
   for (const Texture *const image : table.GetImageList())
      imageNames.insert(lowerCase(image->m_name));
   for (const Material *const material : table.GetMaterialList())
      materialNames.insert(lowerCase(material->m_name));
   ForEachResourceReference(table,
      [&](IEditable *part, const char *field, const string &name, const bool isImage)
      {
         if (part != nullptr && table.IsPlayerSessionObject(part))
            return;
         const string key = lowerCase(name);
         if (!isImage)
            usedMaterials.insert(key);
         if (StrCompareNoCase(name, g_szNoneSelection) || (isImage && string(field).ends_with("Backdrop Image")) || (isImage ? imageNames : materialNames).contains(key))
            return;
         add(Warning, TableCheckId::MissingResource, PartLabel(part) + " uses " + (isImage ? "image '" : "material '") + name + "' (" + field + "), which is not in the table", part);
      });

   // Script names are case insensitive: parts sharing a name can not all be reached from the script
   ankerl::unordered_dense::map<string, int> nameCounts;
   for (IEditable *const part : parts)
      if (!part->GetName().empty())
         ++nameCounts[lowerCase(part->GetName())];
   for (IEditable *const part : parts)
      if (const int n = part->GetName().empty() ? 0 : nameCounts[lowerCase(part->GetName())]; n > 1)
         add(Error, TableCheckId::DuplicateName, std::format("Part '{}' has the same name as {} other part{} (the script reaches only one of them)", part->GetName(), n - 1, n == 2 ? "" : "s"), part);

   // Collidable parts that can not collide: no width, no thickness, no radius (a wall with no height still collides with its top)
   for (IEditable *const part : parts)
   {
      const string name = part->GetName();
      switch (part->GetItemType())
      {
      case eItemRamp:
         // Only a flat ramp is judged by its widths (wire ramps use their wire), and only when both ends have none
         if (const auto &d = static_cast<const Ramp *>(part)->m_d; d.m_collidable && d.m_type == RampTypeFlat && d.m_widthbottom <= 0.f && d.m_widthtop <= 0.f)
            add(Error, TableCheckId::Collider, "Collidable flat ramp '" + name + "' has no width at either end", part);
         break;
      case eItemRubber:
         if (const auto &d = static_cast<const Rubber *>(part)->m_d; d.m_collidable && d.m_thickness <= 0)
            add(Error, TableCheckId::Collider, "Collidable rubber '" + name + "' has no thickness", part);
         break;
      case eItemBumper:
         if (static_cast<const Bumper *>(part)->m_d.m_radius <= 0.f)
            add(Error, TableCheckId::Collider, "Bumper '" + name + "' has no radius", part);
         break;
      case eItemKicker:
         if (static_cast<const Kicker *>(part)->m_d.m_radius <= 0.f)
            add(Error, TableCheckId::Collider, "Kicker '" + name + "' has no radius", part);
         break;
      default: break;
      }
   }

   // Drag points: too few, or two consecutive points on the same spot (a zero length segment, harmless: info)
   for (IEditable *const part : parts)
   {
      const DragPointCurve *const curve = OutlineOf(part);
      if (curve == nullptr)
         continue;
      const auto &pts = curve->GetPoints();
      const int n = static_cast<int>(pts.size());
      if (n < curve->GetMinimumPoints())
      {
         add(Error, TableCheckId::DragPoints, std::format("Part '{}' has only {} drag point{} (at least {} needed)", part->GetName(), n, n == 1 ? "" : "s", curve->GetMinimumPoints()), part);
         continue;
      }
      int zero = 0, first = -1;
      for (int i = 0; i < (IsClosedOutline(part) ? n : n - 1); ++i)
      {
         const int j = (i + 1) % n;
         if (pts[i]->GetX() == pts[j]->GetX() && pts[i]->GetY() == pts[j]->GetY() && pts[i]->GetZ() == pts[j]->GetZ())
         {
            if (first < 0)
               first = i;
            ++zero;
         }
      }
      if (zero == 1)
         add(Info, TableCheckId::DragPoints, std::format("Part '{}': drag points {} and {} are on the same spot (zero length segment)", part->GetName(), first + 1, (first + 1) % n + 1), part);
      else if (zero > 1)
         add(Info, TableCheckId::DragPoints, std::format("Part '{}' has {} zero length segments (first: drag points {} and {})", part->GetName(), zero, first + 1, (first + 1) % n + 1), part);
   }

   // Closed outlines that cross themselves. The curve is smoothed coarsely (straight lines between the points show a fold), an outline
   // still too long to check is reported as such
   constexpr size_t maxVertices = 1200;
   vector<RenderVertex> v;
   for (IEditable *const part : parts)
   {
      const DragPointCurve *const curve = OutlineOf(part);
      if (curve == nullptr || !IsClosedOutline(part) || static_cast<int>(curve->GetPoints().size()) < max(3, curve->GetMinimumPoints()))
         continue;
      for (const float accuracy : { 16.f, 256.f, 4096.f }) // A higher value flattens more (the default is 4)
      {
         v.clear();
         curve->GetRgVertex(v, true, accuracy);
         if (v.size() <= maxVertices)
            break;
      }
      const size_t n = v.size();
      if (n > maxVertices)
      {
         add(Info, TableCheckId::SelfCrossing, "Part '" + part->GetName() + "': outline too long to check for crossings", part);
         continue;
      }
      bool crossed = false;
      for (size_t i = 0; n >= 4 && i < n && !crossed; ++i)
         for (size_t j = i + 2; j < n && !crossed; ++j)
            if (!(i == 0 && j == n - 1)) // Neighbours across the closing segment
               crossed = SegmentsCross(v[i], v[(i + 1) % n], v[j], v[(j + 1) % n]);
      if (crossed)
         add(Warning, TableCheckId::SelfCrossing, "The outline of part '" + part->GetName() + "' crosses itself", part);
   }

   // Materials nothing refers to
   for (Material *const material : table.GetMaterialList())
      if (!usedMaterials.contains(lowerCase(material->m_name)))
         issues.push_back({ Info, TableCheckId::UnusedMaterial, "Material '" + material->m_name + "' is not used by any part or table setting (the script may still use it)", nullptr, material });
}

}

vector<TableIssue> PinTable::CheckTable(const TableCheckOptions &options) const
{
   // Perform a simple table audit (disable lighting vs static, script reference of static parts, png vs webp, hdr vs exr,...)
   vector<TableIssue> issues;
   const auto add = [&issues](const TableIssueSeverity severity, const TableCheckId check, string message, IEditable *part = nullptr)
   { issues.push_back({ severity, check, std::move(message), part }); };
   constexpr TableIssueSeverity Error = TableIssueSeverity::Error, Warning = TableIssueSeverity::Warning, Info = TableIssueSeverity::Info;

   vector<string> functions;
   vector<string> identifiers;
   ParseScript(m_script_text, functions, identifiers,
      [&add](const string &functionName, int line) { add(Error, TableCheckId::ScriptDuplicate, "Duplicate declaration of '" + functionName + "' in script at line " + std::to_string(line)); });

   if (FindIndexOf(identifiers, "execute"s) != -1)
      add(Warning, TableCheckId::ScriptExecute, "Scripts seems to use the 'Execute' command. This command triggers computer security checks and will likely cause stutters during play.");

   if (m_glassBottomHeight > m_glassTopHeight)
      add(Warning, TableCheckId::Glass, "Glass height seems invalid: bottom is higher than top");

   if (m_glassBottomHeight < INCHESTOVPU(2) || m_glassTopHeight < INCHESTOVPU(2))
      add(Warning, TableCheckId::Glass, "Glass height seems invalid: glass is below 2\"");

   if (m_ballSphericalMapping)
      add(Warning, TableCheckId::BallMapping, "Ball uses legacy 'spherical mapping', it will be rendered like a 2D object and therefore will look bad in VR, stereo or headtracking");

   for (const string &fix : m_loadFixes)
      add(Warning, TableCheckId::LoadFix, fix);

   // Search for inconsistencies in the table parts
   bool hasPulseTimer = false, hasPinMameTimer = false;
   for (const auto part : m_vedit)
   {
      auto type = part->GetItemType();
      Primitive *const prim = type == eItemPrimitive ? (Primitive *)part : nullptr;
      Light *const light = type == eItemLight ? (Light *)part : nullptr;
      Surface *const surf = type == eItemSurface ? (Surface *)part : nullptr;
      Textbox *const textbox = type == eItemTextbox ? (Textbox *)part : nullptr;

      // Referencing a static object from script (ok if it is for reading properties, not for writing)
      if (type == eItemPrimitive && prim->m_d.m_staticRendering && FindIndexOf(identifiers, lowerCase(prim->GetName())) != -1)
         add(Warning, TableCheckId::StaticReferenced, "Primitive '" + prim->GetName() + "' seems to be referenced from the script while it is marked as static (most properties of a static object may not be modified at runtime).", part);

      if (type == eItemTextbox && (textbox->m_d.m_isDMD || StrFindNoCase(textbox->m_d.m_text, "DMD"s) != string::npos))
         add(Warning, TableCheckId::TextboxDMD, "legacy Textbox '" + textbox->GetName() + "' is used for DMD rendering. It should be replaced by a flasher to get better rendering.", part);

      if (type == eItemTimer) {
         const string name = lowerCase(((Timer *)part)->GetName());
         hasPulseTimer |= name == "pulsetimer";
         hasPinMameTimer |= name == "pinmametimer";
      }

      // Warning on very fast timers (lower than 5ms)
      switch (type)
      {
      case eItemBall:
      // eItemDecal
      case eItemPartGroup:
      // eItemPrimitive
      case eItemTextbox:
      case eItemDispReel:
      case eItemSurface:
      case eItemTimer:
      case eItemLight:
      case eItemRamp:
      case eItemPlunger:
      case eItemSpinner:
      case eItemTrigger:
      case eItemKicker:
      case eItemRubber:
      case eItemFlasher:
      case eItemLightSeq:
      case eItemHitTarget:
      case eItemBumper:
      case eItemFlipper:
      case eItemGate:
         if (part->m_timerEnabled && part->m_timerInterval != -1 && part->m_timerInterval != -2 && part->m_timerInterval < 17)
            add(Warning, TableCheckId::FastTimer, "Part '" + part->GetName() + "' uses a timer with a very short period of " + std::to_string(part->m_timerInterval) + "ms, below a 60FPS framerate. This will likely cause stutters and the table will not support 'frame pacing'.", part);
         break;
      // eItemTable
      // eItemLightCenter
      // eItemDragPoint
      // eItemCollection
      default: break;
      }

      if (type == eItemPrimitive && prim->m_d.m_visible
         && prim->m_d.m_disableLightingBelow != 1.f && !prim->m_d.m_staticRendering
         && (!GetMaterial(prim->m_d.m_szMaterial)->m_bOpacityActive || GetMaterial(prim->m_d.m_szMaterial)->m_fOpacity == 1.f)
         && (GetImage(prim->m_d.m_szImage) == nullptr || GetImage(prim->m_d.m_szImage)->IsOpaque()))
         add(Warning, TableCheckId::Translucency, "Primitive '" + prim->GetName() + "' uses translucency (lighting from below) while it is fully opaque. Translucency will be discarded.", part);

      if (type == eItemLight && light->m_d.m_intensity < 0.f)
         add(Error, TableCheckId::LightIntensity, "Light '" + light->GetName() + "' has a negative intensity.", part);
      if (type == eItemLight && light->m_d.m_intensity_scale < 0.f)
         add(Error, TableCheckId::LightIntensity, "Light '" + light->GetName() + "' has a negative intensity scale.", part);

      // Disabled as this is now enforced in the rendering
      // Enabling translucency (light from below) won't work with static parts: otherwise the rendering will be different in VR/Headtracked vs desktop modes. It also needs a non opaque alpha.
      //if (type == eItemPrimitive && prim->m_d.m_disableLightingBelow != 1.f && prim->m_d.m_staticRendering)
      //   ss << ". Warning: Primitive '" << prim->GetName() << "' has translucency enabled but is also marked as static. Translucency will not be applied on desktop, and it will look different between VR/headtracked and desktop.\r\n";
      //if (type == eItemSurface && surf->m_d.m_disableLightingBelow != 1.f && surf->StaticRendering())
      //   ss << ". Warning: Wall '" << surf->GetName() << "' has translucency enabled but will be statically rendered (not droppable with opaque materials). Translucency will not be applied on desktop, and it will look different between VR/headtracked and desktop.\r\n";
   }

   // The flasher VideoCap API (capturing the window of an external application) is deprecated in favor of plugins and may be removed in a future version
   if (FindIndexOf(identifiers, "videocapwidth"s) != -1 || FindIndexOf(identifiers, "videocapheight"s) != -1 || FindIndexOf(identifiers, "videocapupdate"s) != -1)
   {
      // Name the flashers referenced as '<name>.VideoCap...' (whole name, case insensitive like VBScript), or the script for an indirect use
      const string script = lowerCase(m_script_text);
      bool attributed = false;
      for (const auto part : m_vedit)
      {
         if (part->GetItemType() != eItemFlasher)
            continue;
         const string ref = lowerCase(string(part->GetName())) + ".videocap";
         for (size_t pos = script.find(ref); pos != string::npos; pos = script.find(ref, pos + 1))
         {
            if (pos > 0 && (isalnum(static_cast<unsigned char>(script[pos - 1])) || script[pos - 1] == '_'))
               continue; // Part of a longer name
            add(Error, TableCheckId::VideoCap, "Flasher '" + part->GetName()
               + "' uses the deprecated 'VideoCap' API (VideoCapWidth/VideoCapHeight/VideoCapUpdate). It is superseded by plugins and may be removed in a future version.", part);
            attributed = true;
            break;
         }
      }
      if (!attributed)
         add(Error, TableCheckId::VideoCap, "script uses the deprecated Flasher 'VideoCap' API (VideoCapWidth/VideoCapHeight/VideoCapUpdate). It is superseded by plugins and may be removed in a future version.");
   }

   if ((FindIndexOf(identifiers, "loadvpm"s) != -1) || (FindIndexOf(identifiers, "loadvpmalt"s) != -1))
   {
      if (!hasPinMameTimer)
         add(Warning, TableCheckId::VPM, "VPM controller is used but table is missing a Timer object named 'PinMAMETimer'.");
      if (FindIndexOf(identifiers, "vpminit"s) == -1)
         add(Warning, TableCheckId::VPM, "VPM controller is used but vpmInit is not called. pause/resume/exit will likely exhibit bugs and physic outputs won't be supported.");
   }

   if (!hasPulseTimer && (FindIndexOf(identifiers, "vpmtimer"s) != -1))
      add(Warning, TableCheckId::VPM, "script uses 'vpmTimer' but table is missing a Timer object named 'PulseTimer'. vpmTimer will not work as expected.");

   if (options.extended)
      CheckParts(*this, issues);

   if (options.sound)
   {
      // Only used to query sound informations: no need for the optional haptic output
      auto audioPlayer = std::make_unique<VPX::AudioPlayer>(m_settings.GetPlayer_SoundDeviceBG(), m_settings.GetPlayer_SoundDevice(),
         static_cast<VPX::SoundConfigTypes>(m_settings.GetPlayer_Sound3D()), static_cast<VPX::BackglassConfigTypes>(m_settings.GetPlayer_Sound3DBackglass()), ""s, VPX::SNDCFG_SND3D2CH);
      for (const auto sound : m_vsound)
      {
         auto specs = audioPlayer->GetSoundInformations(sound);
         if (specs.nChannels > 1 && sound->GetOutputTarget() == VPX::SNDOUT_TABLE)
            add(Error, TableCheckId::Sound, "sound '" + sound->GetName() + "' is used for playfield physical sound but has multiple channels (not mono).");
      }
      audioPlayer = nullptr;
   }

   // Also output a log of the table file content to allow easier size optimization
   if (options.totals)
   {
      size_t totalSize = 0, totalGpuSize = 0;
      for (const auto sound : m_vsound)
      {
         //ss << "  . Sound: '" << sound->m_name << "', size: " << SizeToReadable(sound->m_cdata) << "\r\n";
         totalSize += sound->GetFileSize();
      }
      add(Info, TableCheckId::Totals, "Total sound size: " + SizeToReadable(totalSize));

      totalSize = 0;
      for (const auto image : m_vimage)
      {
         size_t imageSize = image->GetFileSize();
         size_t gpuSize = image->GetEstimatedGPUSize();
         //ss << "  . Image: '" << image->m_name << "', size: " << SizeToReadable(imageSize) << ", GPU mem size: " << SizeToReadable(gpuSize) << "\r\n";
         totalSize += imageSize;
         totalGpuSize += gpuSize;
      }
      add(Info, TableCheckId::Totals, "Total image size: " + SizeToReadable(totalSize) + " in VPX file, at least " + SizeToReadable(totalGpuSize) + " in GPU memory when played");
   }

   // Backdrop images referenced per view mode but not present in the table would render black.
   // An empty name or the "<None>" sentinel means no backdrop is set.
   static const char* const bgSetNames[NUM_BG_SETS] = { "Desktop", "Cabinet", "Full Single Screen" };
   for (unsigned int i = 0; i < NUM_BG_SETS; ++i)
      if (const string& bg = m_BG_image[i]; !bg.empty() && !StrCompareNoCase(bg, g_szNoneSelection) && GetImage(bg) == nullptr)
         add(Warning, TableCheckId::Backdrop, string(bgSetNames[i]) + " backdrop image '" + bg + "' is not present in the table (renders black)");

   if (options.totals)
   {
      int nPrimTris = 0, primMemSize = 0;
      for (const auto part : m_vedit)
         if (part->GetItemType() == eItemPrimitive && ((Primitive *)part)->m_d.m_use3DMesh /* && ((Primitive *)part)->m_d.m_visible */ )
         {
            primMemSize += (((Primitive *)part)->m_mesh.NumIndices() > 65536 ? 4 : 2) * (int) ((Primitive *)part)->m_mesh.NumIndices();
            primMemSize += (int) ((Primitive *)part)->m_mesh.NumVertices() * sizeof(Vertex3D_NoTex2);
            nPrimTris += (int) ((Primitive *)part)->m_mesh.NumIndices() / 3;
         }
      add(Info, TableCheckId::Totals, "Total number of faces used in primitives: " + std::to_string(nPrimTris) + ", needing " + SizeToReadable(primMemSize) + " in GPU memory when played");
   }

   return issues;
}
