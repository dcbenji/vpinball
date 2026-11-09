// license:GPLv3+

#include "core/stdafx.h"
#include "tablecheck.h"

#include "audio/AudioPlayer.h"
#include "math/MeshUtils.h"
#include "parts/ball.h"
#include "parts/bumper.h"
#include "parts/Collection.h"
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
   case TableCheckId::TimerSub: return "Timers without a sub";
   case TableCheckId::EventSub: return "Event subs of missing objects";
   case TableCheckId::ScriptReference: return "Missing names used by the script";
   case TableCheckId::Translucency: return "Translucency on opaque primitives";
   case TableCheckId::LightIntensity: return "Light intensity";
   case TableCheckId::VideoCap: return "Flasher VideoCap API";
   case TableCheckId::VPM: return "VPinMAME setup";
   case TableCheckId::OutsidePlayfield: return "Outside the playfield";
   case TableCheckId::MissingResource: return "Missing images and materials";
   case TableCheckId::DuplicateName: return "Duplicate part names";
   case TableCheckId::Collider: return "Colliders that can not collide";
   case TableCheckId::HeavyCollider: return "Heavy collidable primitives";
   case TableCheckId::DragPoints: return "Drag points";
   case TableCheckId::SelfCrossing: return "Self-crossing outlines";
   case TableCheckId::DepthBias: return "Depth bias";
   case TableCheckId::DuplicatePart: return "Duplicate parts";
   case TableCheckId::Kicker: return "Kickers";
   case TableCheckId::UnusedMaterial: return "Unused materials";
   case TableCheckId::UnusedImage: return "Unused images";
   case TableCheckId::LargeImage: return "Large images";
   case TableCheckId::EmptyCollection: return "Empty collections";
   case TableCheckId::CollectionEvents: return "Collection events";
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

// What the script says about resource names:
// - its string literals (lower case);
// - the literals it joins to other strings (names built at run time, e.g. "Digit" & n: the images whose name starts, or ends,
//   with such a part count as used);
// - whether it builds image names in a way these do not show (an image property set from a concatenation, or LoadTexture);
// - the names given as a literal to PlaySound / StopSound (and the script's own sound helpers named PlaySound...), to an image
//   property and to a material property (original letter case, once each; a literal joined to something else is skipped)
struct ScriptStrings
{
   ankerl::unordered_dense::set<string> strings;
   vector<string> prefixes, suffixes; // Literals followed, or preceded, by a concatenation (& or +)
   bool dynamicImageNames = false;
   vector<string> sounds, images, materials;
};

bool IsIdentifierChar(const char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The image name properties of the parts and the table (the script can set them by name)
constexpr const char *imageProperties[] = { ".image", ".imagea", ".imageb", ".sideimage", ".normalmap", ".frontdecal", ".ballimage", ".ballfrontdecal",
   ".environmentimage", ".colorgradeimage", ".backdropimage_dt", ".backdropimage_fs", ".backdropimage_fss" };

ScriptStrings ScanScriptStrings(const string &script)
{
   ScriptStrings use;
   ankerl::unordered_dense::set<string> named[3]; // Sounds, images, materials already listed
   const string text = lowerCase(script);
   string code;
   vector<string> literals; // Of the line, in original letter case
   // The literal given at code position pos (spaces skipped), if it is not joined to something else
   const auto literalAt = [&code, &literals](size_t pos) -> const string *
   {
      while (pos < code.size() && (code[pos] == ' ' || code[pos] == '\t'))
         ++pos;
      if (code.compare(pos, 2, "\"\"") != 0)
         return nullptr;
      const size_t index = std::count(code.begin(), code.begin() + pos, '"') / 2;
      const size_t next = code.find_first_not_of(" \t", pos + 2);
      return next != string::npos && (code[next] == '&' || code[next] == '+') ? nullptr : &literals[index];
   };
   const auto addName = [&use, &named](const int kind, const string &name)
   {
      if (!name.empty() && named[kind].insert(lowerCase(name)).second)
         (kind == 0 ? use.sounds : kind == 1 ? use.images : use.materials).push_back(name);
   };
   // Property assignments '.<property> = ...' of the line: calls fn with the position after the '='
   const auto forEachAssignment = [&code](const char *property, const std::function<void(size_t)> &fn)
   {
      const size_t len = strlen(property);
      for (size_t pos = code.find(property); pos != string::npos; pos = code.find(property, pos + 1))
      {
         size_t next = pos + len;
         if (next < code.size() && IsIdentifierChar(code[next]))
            continue; // A longer name
         while (next < code.size() && (code[next] == ' ' || code[next] == '\t'))
            ++next;
         if (next < code.size() && code[next] == '=')
            fn(next + 1);
      }
   };
   for (size_t lineStart = 0; lineStart < text.size();)
   {
      size_t lineEnd = text.find('\n', lineStart);
      if (lineEnd == string::npos)
         lineEnd = text.size();
      // The code of the line up to a comment, its string literals ("" is a quote inside a literal) replaced by ""
      code.clear();
      literals.clear();
      for (size_t i = lineStart; i < lineEnd && text[i] != '\''; ++i)
      {
         if (text[i] != '"')
         {
            code += text[i];
            continue;
         }
         string literal;
         for (++i; i < lineEnd; ++i)
         {
            if (text[i] == '"' && i + 1 < lineEnd && text[i + 1] == '"')
               literal += script[++i];
            else if (text[i] == '"')
               break;
            else
               literal += script[i];
         }
         literals.push_back(std::move(literal));
         code += "\"\"";
      }
      // A REM comment (Rem then a space, a tab or nothing)
      if (const size_t first = code.find_first_not_of(" \t"); first != string::npos && code.compare(first, 3, "rem") == 0
         && (first + 3 == code.size() || code[first + 3] == ' ' || code[first + 3] == '\t'))
      {
         code.clear();
         literals.clear();
      }
      // The literals joined to something else (spaces skipped): parts of a built name
      for (size_t pos = code.find("\"\""), index = 0; pos != string::npos; pos = code.find("\"\"", pos + 2), ++index)
      {
         const string literal = lowerCase(literals[index]);
         use.strings.insert(literal);
         if (literal.empty())
            continue;
         const size_t next = code.find_first_not_of(" \t", pos + 2);
         const size_t prev = pos == 0 ? string::npos : code.find_last_not_of(" \t", pos - 1);
         if (next != string::npos && (code[next] == '&' || code[next] == '+'))
            use.prefixes.push_back(literal);
         if (prev != string::npos && (code[prev] == '&' || code[prev] == '+'))
            use.suffixes.push_back(literal);
      }
      for (size_t pos = code.find("loadtexture"); pos != string::npos && !use.dynamicImageNames; pos = code.find("loadtexture", pos + 1))
         use.dynamicImageNames = (pos == 0 || !IsIdentifierChar(code[pos - 1])) && (pos + 11 >= code.size() || !IsIdentifierChar(code[pos + 11]));
      for (const char *const property : imageProperties)
         forEachAssignment(property,
            [&](size_t pos)
            {
               // Anything but a single literal (a variable, a constant, a function result, a concatenation) is a name the script computes
               if (const string *const name = literalAt(pos))
                  addName(1, *name);
               else
                  use.dynamicImageNames = true;
            });
      forEachAssignment(".material", [&](size_t pos) { if (const string *const name = literalAt(pos)) addName(2, *name); });
      // Sound calls: an identifier starting with playsound or stopsound, then its first argument
      for (const char *const call : { "playsound", "stopsound" })
         for (size_t pos = code.find(call); pos != string::npos; pos = code.find(call, pos + 1))
         {
            if (pos > 0 && (IsIdentifierChar(code[pos - 1]) || code[pos - 1] == '.'))
               continue; // Part of another name, or a method of an object
            size_t next = pos;
            while (next < code.size() && IsIdentifierChar(code[next]))
               ++next;
            while (next < code.size() && (code[next] == ' ' || code[next] == '\t' || code[next] == '('))
               ++next;
            if (const string *const name = literalAt(next))
               addName(0, *name);
         }
      lineStart = lineEnd + 1;
   }
   return use;
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
// loadsCode: the script loads other code (LoadVPM, ExecuteGlobal,...), which may use anything by name
// scriptChecks: the checks that read the script (unused images, names used by the script)
void CheckParts(const PinTable &table, vector<TableIssue> &issues, const bool loadsCode, const bool scriptChecks)
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
   ankerl::unordered_dense::set<string> imageNames, materialNames, usedImages, usedMaterials;
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
         (isImage ? usedImages : usedMaterials).insert(key);
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

   // Collidable primitives with many faces: each face is a collider. The playfield mesh is the playfield itself. The physics reduce
   // the mesh by the primitive's collision reduction factor (as Primitive::PhysicSetup does): the faces are counted after it
   constexpr unsigned int maxColliderFaces = 5000;
   for (IEditable *const part : parts)
      if (part->GetItemType() == eItemPrimitive)
      {
         const Primitive *const prim = static_cast<const Primitive *>(part);
         if (!prim->m_d.m_use3DMesh || !prim->m_d.m_collidable || prim->m_d.m_toy || prim->IsPlayfield() || HasNoPlayfieldPosition(part))
            continue; // A part out of the playfield space (e.g. a backglass group) has no physics
         const size_t vertices = prim->m_mesh.NumVertices();
         double faces = static_cast<double>(prim->m_mesh.NumIndices() / 3);
         if (const double reduced = max(pow(static_cast<double>(vertices), saturate(1.f - prim->m_d.m_collision_reductionFactor) * 0.25f + 0.75f), 420.0);
             reduced < static_cast<double>(vertices))
            faces = faces * reduced / static_cast<double>(vertices);
         if (faces > maxColliderFaces)
            add(Warning, TableCheckId::HeavyCollider,
               std::format("Collidable primitive '{}' has {:.0f} collision faces: each face is a collider, which slows the physics (a simple collision mesh, more "
                           "collision reduction, or Toy, helps)", part->GetName(), faces), part);
      }

   // Collidable ramps so high that a ball on them would hit the glass (the glass is the ceiling of the table). A ramp entirely at
   // the glass is a ceiling itself (e.g. an invisible lid that keeps the ball down), not a ramp for the ball
   constexpr float ballDiameter = 50.f; // Standard ball
   const float glass = max(table.m_glassTopHeight, table.m_glassBottomHeight);
   for (IEditable *const part : parts)
      if (part->GetItemType() == eItemRamp && !HasNoPlayfieldPosition(part))
         if (const auto &d = static_cast<const Ramp *>(part)->m_d;
             d.m_collidable && max(d.m_heighttop, d.m_heightbottom) + ballDiameter > glass && min(d.m_heighttop, d.m_heightbottom) + ballDiameter <= glass)
            add(Warning, TableCheckId::Glass,
               std::format("Ramp '{}' goes up to {:.0f}: a ball on it would hit the glass (at {:.0f}), raise the glass", part->GetName(), max(d.m_heighttop, d.m_heightbottom), glass), part);

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

   // Visible flashers at the same height, with the same depth bias, that overlap: they are drawn in no defined order and may flicker
   // (z-fighting). Additive flashers are left out (their order does not change the result). There is no right value to set automatically
   struct FlasherBounds
   {
      Flasher *flasher;
      Vertex2D min, max;
   };
   vector<FlasherBounds> flashers;
   for (IEditable *const part : parts)
      if (part->GetItemType() == eItemFlasher && static_cast<const Flasher *>(part)->m_d.m_isVisible && static_cast<const Flasher *>(part)->m_d.m_addBlend == FlasherData::AB_NONE
         && !part->m_desktopBackdrop)
      {
         Flasher *const flasher = static_cast<Flasher *>(part);
         if (!flasher->m_curve.GetPoints().empty())
            flashers.push_back({ flasher, flasher->m_curve.GetMinBound(), flasher->m_curve.GetMaxBound() });
      }
   for (size_t i = 1; i < flashers.size(); ++i)
      for (size_t j = 0; j < i; ++j)
      {
         const FlasherBounds &a = flashers[i], &b = flashers[j];
         if (fabsf(a.flasher->m_d.m_height - b.flasher->m_d.m_height) < 0.01f && a.flasher->m_d.m_depthBias == b.flasher->m_d.m_depthBias && a.min.x < b.max.x
            && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y)
         {
            add(Info, TableCheckId::DepthBias,
               "Flasher '" + a.flasher->GetName() + "' overlaps flasher '" + b.flasher->GetName() + "' at the same height and depth bias: they may flicker, give one a different depth bias",
               a.flasher);
            break;
         }
      }

   // Collidable parts of the same type at the same place with the same shape: left over copies that collide twice (doubled bumpers kick
   // twice). Visual parts are often stacked on purpose (baked meshes, shadows), so only colliders are compared
   ankerl::unordered_dense::map<string, IEditable *> placed;
   for (IEditable *const part : parts)
   {
      const ItemTypeEnum type = part->GetItemType();
      const BaseProperty *const base = BaseOf(part);
      if (HasNoPlayfieldPosition(part) || base == nullptr || !base->m_collidable || type == eItemLight || type == eItemDecal
         || (type == eItemPrimitive && static_cast<const Primitive *>(part)->m_d.m_toy))
         continue;
      string key = std::format("{} {} {} {}", static_cast<int>(type), part->GetCenter().x, part->GetCenter().y, part->GetRotate());
      if (const DragPointCurve *const curve = OutlineOf(part))
         for (const auto &p : curve->GetPoints())
            key += std::format(" {} {} {}", p->GetX(), p->GetY(), p->GetZ());
      // The heights, sizes and surface: stacked walls, or a gate on an upper playfield above another one, are not duplicates
      switch (type)
      {
      case eItemSurface: key += std::format(" {} {}", static_cast<const Surface *>(part)->m_d.m_heightbottom, static_cast<const Surface *>(part)->m_d.m_heighttop); break;
      case eItemRubber: key += std::format(" {} {}", static_cast<const Rubber *>(part)->m_d.m_height, static_cast<const Rubber *>(part)->m_d.m_thickness); break;
      case eItemRamp:
      {
         const auto &d = static_cast<const Ramp *>(part)->m_d;
         key += std::format(" {} {} {} {} {}", d.m_heightbottom, d.m_heighttop, d.m_widthbottom, d.m_widthtop, static_cast<int>(d.m_type));
         break;
      }
      case eItemBumper: key += std::format(" {} {}", static_cast<const Bumper *>(part)->m_d.m_radius, static_cast<const Bumper *>(part)->m_d.m_szSurface); break;
      case eItemKicker: key += std::format(" {} {}", static_cast<const Kicker *>(part)->m_d.m_radius, static_cast<const Kicker *>(part)->m_d.m_szSurface); break;
      case eItemTrigger:
      {
         const auto &d = static_cast<const Trigger *>(part)->m_d;
         key += std::format(" {} {} {}", d.m_radius, static_cast<int>(d.m_shape), d.m_szSurface);
         break;
      }
      case eItemGate: key += std::format(" {} {} {}", static_cast<const Gate *>(part)->m_d.m_length, static_cast<const Gate *>(part)->m_d.m_height, static_cast<const Gate *>(part)->m_d.m_szSurface); break;
      case eItemSpinner:
         key += std::format(" {} {} {}", static_cast<const Spinner *>(part)->m_d.m_length, static_cast<const Spinner *>(part)->m_d.m_height, static_cast<const Spinner *>(part)->m_d.m_szSurface);
         break;
      case eItemFlipper:
      {
         const auto &d = static_cast<const Flipper *>(part)->m_d;
         key += std::format(" {} {} {} {} {} {} {}", d.m_height, d.m_BaseRadius, d.m_EndRadius, d.m_FlipperRadiusMax, d.m_StartAngle, d.m_EndAngle, d.m_szSurface);
         break;
      }
      case eItemPlunger: key += std::format(" {} {}", static_cast<const Plunger *>(part)->m_d.m_height, static_cast<const Plunger *>(part)->m_d.m_szSurface); break;
      case eItemHitTarget:
      {
         const auto &d = static_cast<const HitTarget *>(part)->m_d;
         key += std::format(" {} {} {} {} {}", d.m_vPosition.z, d.m_vSize.x, d.m_vSize.y, d.m_vSize.z, static_cast<int>(d.m_targetType));
         break;
      }
      default: break;
      }
      if (type == eItemPrimitive)
      {
         const auto &d = static_cast<const Primitive *>(part)->m_d;
         key += std::format(" {} {} {} {} {} {}", d.m_vPosition.x, d.m_vPosition.y, d.m_vPosition.z, d.m_vSize.x, d.m_vSize.y, d.m_vSize.z);
         for (const float v : d.m_aRotAndTra)
            key += std::format(" {}", v);
         // The mesh: its size and bounds (baked parts often share an identity transform, with different meshes)
         const Mesh &mesh = static_cast<const Primitive *>(part)->m_mesh;
         Vertex3Ds lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
         for (const Vertex3D_NoTex2 &v : mesh.m_vertices)
         {
            lo = Vertex3Ds(min(lo.x, v.x), min(lo.y, v.y), min(lo.z, v.z));
            hi = Vertex3Ds(max(hi.x, v.x), max(hi.y, v.y), max(hi.z, v.z));
         }
         key += std::format(" {} {} {} {} {} {} {} {} {}", d.m_use3DMesh, mesh.NumVertices(), mesh.NumIndices(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
      }
      if (const auto [it, inserted] = placed.try_emplace(std::move(key), part); !inserted)
         add(Warning, TableCheckId::DuplicatePart, "Part '" + part->GetName() + "' is at the same place, with the same shape, as '" + it->second->GetName() + "' (a duplicate?)", part);
   }

   // Kickers smaller than the ball, or overlapping a collidable wall or rubber (the ball is caught while it touches the other collider)
   constexpr float ballRadius = 25.f; // Standard ball
   vector<std::pair<const IEditable *, vector<RenderVertex>>> walls; // Closed outlines of the collidable walls and rubbers, made when a kicker needs them
   bool wallsMade = false;
   for (IEditable *const part : parts)
   {
      if (part->GetItemType() != eItemKicker || HasNoPlayfieldPosition(part))
         continue;
      const auto &d = static_cast<const Kicker *>(part)->m_d;
      if (d.m_radius > 0.f && d.m_radius < ballRadius)
         add(Info, TableCheckId::Kicker, std::format("Kicker '{}' (radius {:.1f}) is smaller than the ball (radius {:.0f})", part->GetName(), d.m_radius, ballRadius), part);
      if (!wallsMade)
         for (IEditable *const other : parts)
         {
            const ItemTypeEnum type = other->GetItemType();
            if ((type == eItemSurface && static_cast<const Surface *>(other)->m_d.m_collidable) || (type == eItemRubber && static_cast<const Rubber *>(other)->m_d.m_collidable))
            {
               walls.emplace_back(other, vector<RenderVertex>());
               OutlineOf(other)->GetRgVertex(walls.back().second, true, 16.f);
            }
         }
      wallsMade = true;
      for (const auto &[wall, outline] : walls)
      {
         const float reach = d.m_radius + (wall->GetItemType() == eItemRubber ? 0.5f * static_cast<float>(static_cast<const Rubber *>(wall)->m_d.m_thickness) : 0.f);
         bool overlaps = false;
         for (size_t i = 0; i < outline.size() && !overlaps; ++i)
         {
            // Distance from the kicker center to the segment
            const Vertex2D &a = outline[i], &b = outline[(i + 1) % outline.size()];
            const Vertex2D ab = b - a, ac = d.m_vCenter - a;
            const float len2 = ab.x * ab.x + ab.y * ab.y;
            const float t = len2 > 0.f ? clamp((ab.x * ac.x + ab.y * ac.y) / len2, 0.f, 1.f) : 0.f;
            const float dx = a.x + t * ab.x - d.m_vCenter.x, dy = a.y + t * ab.y - d.m_vCenter.y;
            overlaps = dx * dx + dy * dy < reach * reach;
         }
         if (overlaps)
            add(Info, TableCheckId::Kicker, "Kicker '" + part->GetName() + "' overlaps collidable " + (wall->GetItemType() == eItemRubber ? "rubber '" : "wall '") + wall->GetName() + '\'', part);
      }
   }

   const ScriptStrings script = scriptChecks ? ScanScriptStrings(table.m_script_text) : ScriptStrings();

   // Materials nothing refers to (no part, table setting or script string)
   for (Material *const material : table.GetMaterialList())
      if (const string key = lowerCase(material->m_name); !usedMaterials.contains(key) && !script.strings.contains(key))
         issues.push_back({ Info, TableCheckId::UnusedMaterial, "Material '" + material->m_name + "' is not used by any part or table setting (the script may still use it)", nullptr, material });

   // Images nothing refers to: no part, table setting, image link of the table description (Markdown '](name)'), script string or
   // part of a name the script builds
   const auto built = [&script](const string &key)
   {
      return std::ranges::any_of(script.prefixes, [&key](const string &p) { return key.starts_with(p); })
         || std::ranges::any_of(script.suffixes, [&key](const string &p) { return key.ends_with(p); });
   };
   const string notes = lowerCase(table.m_blurb + '\n' + table.m_description);
   const auto linked = [&notes](const string &key)
   {
      for (size_t pos = notes.find("](" + key); pos != string::npos; pos = notes.find("](" + key, pos + 1))
         if (const size_t end = pos + 2 + key.size(); end < notes.size() && (notes[end] == ')' || notes[end] == ' '))
            return true;
      return false;
   };
   for (Texture *const image : table.GetImageList())
   {
      if (!scriptChecks)
         break;
      const string key = lowerCase(image->m_name);
      if (usedImages.contains(key) || script.strings.contains(key) || linked(key) || built(key))
         continue;
      TableIssue issue { Info, TableCheckId::UnusedImage,
         "Image '" + image->m_name + "' is not used by any part, table setting, table description or script string"
            + (script.dynamicImageNames ? " (the script builds image names: check it before deleting the image)"s
               : loadsCode              ? " (the script loads other code, which may use it)"s
                                        : ""s) };
      issue.image = image;
      if (!script.dynamicImageNames && !loadsCode)
         issue.fix = TableFix::DeleteImage;
      issues.push_back(std::move(issue));
   }
   for (Texture *const image : table.GetImageList())
      if (image->m_width > 4096 || image->m_height > 4096)
      {
         TableIssue issue { Info, TableCheckId::LargeImage, std::format("Image '{}' is {}x{}: an image over 4096 pixels needs a lot of memory and may be downsized on some GPUs", image->m_name, image->m_width.load(), image->m_height.load()) };
         issue.image = image;
         issues.push_back(std::move(issue));
      }

   // Names the script gives as literals that are not in the table
   ankerl::unordered_dense::set<string> soundNames;
   for (const VPX::Sound *const sound : table.m_vsound)
      soundNames.insert(lowerCase(sound->GetName()));
   for (const string &name : script.sounds)
      if (!soundNames.contains(lowerCase(name)))
         issues.push_back({ Warning, TableCheckId::ScriptReference, "The script plays sound '" + name + "', which is not in the table" });
   for (const string &name : script.images)
      if (!imageNames.contains(lowerCase(name)))
         issues.push_back({ Warning, TableCheckId::ScriptReference, "The script sets image '" + name + "', which is not in the table" });
   for (const string &name : script.materials)
      if (!materialNames.contains(lowerCase(name)))
         issues.push_back({ Warning, TableCheckId::ScriptReference, "The script sets material '" + name + "', which is not in the table" });
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
   if (options.script)
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
      {
         add(Error, TableCheckId::LightIntensity, "Light '" + light->GetName() + "' has a negative intensity.", part);
         issues.back().fix = TableFix::ClearLightIntensity;
      }
      if (type == eItemLight && light->m_d.m_intensity_scale < 0.f) // Set by the script (it is not saved with the table): no fix
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

   // Enabled timers without a '<name>_Timer' sub: they run for nothing. A script that loads other code (LoadVPM, ExecuteGlobal,
   // Execute, GetRef) may define the sub there (e.g. the VPinMAME scripts): the issue is then an info, and the timer is not disabled
   // by the fix. The timer of a part in a collection that fires events also fires the collection's Timer event: not reported.
   const bool loadsCode = std::ranges::any_of(std::initializer_list<string> { "loadvpm"s, "loadvpmalt"s, "executeglobal"s, "execute"s, "getref"s },
      [&identifiers](const string &id) { return FindIndexOf(identifiers, id) != -1; });
   for (const auto part : m_vedit)
   {
      if (!options.extended || !options.script || !part->m_timerEnabled || IsPlayerSessionObject(part) || std::ranges::any_of(part->m_vCollection, [](const Collection *c) { return c->m_fireEvents; }))
         continue;
      if (FindIndexOf(functions, '.' + lowerCase(part->GetName()) + "_timer") != -1)
         continue;
      add(loadsCode ? Info : Warning, TableCheckId::TimerSub,
         "Part '" + part->GetName() + "' has an enabled timer but the script has no '" + part->GetName() + "_Timer' sub" + (loadsCode ? " (the script loads other code, which may have it)" : ""), part);
      if (!loadsCode)
         issues.back().fix = TableFix::DisableTimer;
   }

   // Event subs ('<object>_<event>') of objects that are not in the table: left behind by a renamed or deleted part, they never run.
   // ParseScript gives the subs in lower case
   static constexpr const char *partEvents[] = { "init", "hit", "unhit", "timer", "spin", "dropped", "raised", "slingshot", "animate", "limiteos", "limitbos", "collide" };
   ankerl::unordered_dense::set<string> objectNames { lowerCase(m_name) };
   for (const auto part : m_vedit)
      objectNames.insert(lowerCase(part->GetName()));
   for (const auto collection : m_vcollection)
      objectNames.insert(lowerCase(collection->m_name));
   for (const string &function : functions)
   {
      if (!options.extended)
         break;
      const size_t sep = function.rfind('_');
      if (function[0] != '.' || sep == string::npos || sep < 2) // Class methods start with their class name
         continue;
      const string object = function.substr(1, sep - 1), event = function.substr(sep + 1);
      // A sub the script calls by its name is a plain sub, not an event handler (e.g. a '<name>_Timer' sub called by a frame timer)
      if (std::find(std::begin(partEvents), std::end(partEvents), event) != std::end(partEvents) && !objectNames.contains(object) && FindIndexOf(identifiers, function.substr(1)) == -1)
         add(Warning, TableCheckId::EventSub, "The script has a sub '" + function.substr(1) + "' for the " + event + " event of '" + object + "', which is not in the table");
   }

   if (options.extended)
      CheckParts(*this, issues, loadsCode, options.script);

   // Collections without parts (leftovers). One whose name the script uses (as a name, 'For Each x In Collection' is valid on an empty
   // one, or in a string, e.g. for Eval) stays, and so does any when the script loads other code
   const string lowerScript = options.extended ? lowerCase(m_script_text) : string();
   for (CComObject<Collection> *const collection : m_vcollection)
      if (options.extended && options.script && collection->GetParts().empty())
      {
         const string name = lowerCase(collection->m_name);
         const bool used = FindIndexOf(identifiers, name) != -1 || lowerScript.find('"' + name + '"') != string::npos;
         add(Info, TableCheckId::EmptyCollection,
            "Collection '" + collection->m_name + "' is empty" + (used ? " (the script uses its name)" : loadsCode ? " (the script loads other code, which may use it)" : ""));
         issues.back().collection = collection;
         if (!used && !loadsCode)
            issues.back().fix = TableFix::DeleteCollection;
      }

   // Collections whose events the script handles ('<collection>_<event>' sub) but which do not fire events; for a Hit sub, the members
   // whose own hit events are off (their hits never reach the collection)
   static constexpr const char *collectionEvents[] = { "init", "hit", "slingshot", "timer", "spin", "unhit", "dropped", "raised" }; // ICollectionEvents
   for (CComObject<Collection> *const collection : m_vcollection)
   {
      if (!options.extended)
         break;
      const string prefix = '.' + lowerCase(collection->m_name) + '_';
      const auto sub = std::ranges::find_if(functions,
         [&prefix](const string &f)
         {
            return f.starts_with(prefix)
               && std::find(std::begin(collectionEvents), std::end(collectionEvents), f.substr(prefix.size())) != std::end(collectionEvents);
         });
      if (sub == functions.end())
         continue;
      if (!collection->m_fireEvents)
      {
         add(Warning, TableCheckId::CollectionEvents, "Collection '" + collection->m_name + "' does not fire events: the script's sub '" + sub->substr(1) + "' never runs");
         issues.back().collection = collection;
         continue;
      }
      if (FindIndexOf(functions, prefix + "hit") == -1)
         continue;
      for (IEditable *const part : collection->GetParts())
      {
         const ItemTypeEnum type = part->GetItemType();
         if (type != eItemSurface && type != eItemRubber && type != eItemPrimitive && type != eItemRamp)
            continue; // Parts that always fire their hit events, or have none
         if (const BaseProperty *const base = BaseOf(part); base != nullptr && !base->m_hitEvent)
            add(Info, TableCheckId::CollectionEvents, "Part '" + part->GetName() + "' is in collection '" + collection->m_name + "', whose Hit sub the script has, but its hit events are off", part);
      }
   }

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
      {
         add(Warning, TableCheckId::Backdrop, string(bgSetNames[i]) + " backdrop image '" + bg + "' is not present in the table (renders black)");
         issues.back().index = static_cast<int>(i);
         issues.back().fix = TableFix::ClearBackdropImage;
      }

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
