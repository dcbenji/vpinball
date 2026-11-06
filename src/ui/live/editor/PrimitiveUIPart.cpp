#include "core/stdafx.h"
#include "PrimitiveUIPart.h"

#include "core/extern.h"
#include "core/player.h"
#include "PlasticFromAtlas.h"
#include "math/matrix.h"
#include "utils/color.h"

namespace
{
struct TracedLookKey
{
   const char* label;
   const char* key;
   float def, vMin, vMax;
   int decimals;
   const char* tooltip;
};

// Position of the value of key in "a=1 b=2" text, npos when the key is not there
size_t FindTracedLookValue(const string& params, const char* key)
{
   const string token = string(key) + '=';
   for (size_t pos = params.find(token); pos != string::npos; pos = params.find(token, pos + 1))
      if (pos == 0 || params[pos - 1] == ' ')
         return pos + token.size();
   return string::npos;
}

float GetTracedLook(const string& params, const TracedLookKey& key)
{
   const size_t pos = FindTracedLookValue(params, key.key);
   return pos == string::npos ? key.def : static_cast<float>(atof(params.c_str() + pos));
}

void SetTracedLook(string& params, const char* key, float v)
{
   char value[32];
   snprintf(value, sizeof(value), "%.4g", v);
   const size_t pos = FindTracedLookValue(params, key);
   if (pos != string::npos)
   {
      const size_t end = params.find(' ', pos);
      params.replace(pos, end == string::npos ? string::npos : end - pos, value);
   }
   else
      params += (params.empty() ? ""s : " "s) + key + '=' + value;
}

constexpr TracedLookKey kLookKeys[] = {
   { "Glint Strength", "glint", 1.5f, 0.f, 6.f, 2, "Strength of the highlights in the facets" },
   { "Glint Sharpness", "sharp", 76.648f, 5.f, 400.f, 0, "Low = broad sheen, high = tight sparkle" },
   { "Glint Colour Fringe", "fringe", 1.62f, 0.f, 12.f, 2, "Degrees between the red, green and blue highlights: 0 = white glints, more = a wider colour split" },
   { "Glint Facet Tilt", "tilt", 50.938f, 10.f, 80.f, 1, "The facet angle the highlights are worked out for (deg)" },
   { "Glint Count", "keys", 5.f, 1.f, 8.f, 0, "Highlights around the insert" },
   { "Moulding Waviness", "wavy", 3.f, 0.f, 15.f, 2, "How far the moulding's surface leans (deg), so straight facets do not glint as one" },
   { "Moulding Wavelength (mm)", "wavelen", 6.f, 0.5f, 30.f, 1, "Size of the moulding's waves" },
   { "Glint Turn Jitter", "turnjit", 14.641f, 0.f, 90.f, 1, "How differently each insert's highlights are turned (deg)" },
   { "Glint View Turn", "glintturn", 0.25f, 0.f, 1.f, 2, "1 = the highlights turn with the camera, 0 = they hold still on the table and the glints only shift with the view through the facets" },
   { "Facet Blur", "facetblur", 0.f, 0.f, 6.f, 2, "Softens the facet map's edges for the highlights (mips)" },
   { "Room Glint", "room", 0.3f, 0.f, 3.f, 2, "The room's light glinting in the facets, lamp on or off" },
   { "Room Glint Tint", "roomtint", 0.f, 0.f, 1.f, 2, "0 = white room glints, 1 = the plastic's colour" },
   { "Lamp-Off Look", "env", 1.f, 0.f, 3.f, 2, "Strength of the baked lamp-off image" },
   { "Plastic Tint", "plastictint", 1.f, 0.f, 1.f, 2, "How much the plastic's colour tints the baked lamp-off image: 1 = fully, 0 = the image keeps its own baked colour" },
   { "Glow Spread", "spread", 0.67f, 0.2f, 4.f, 2, "Higher lifts the dim parts of the glow toward its peak" },
   { "White Leak", "leak", 0.f, 0.f, 1.f, 2, "How much white shows at the glow's hot core" },
   { "Frost", "frost", 0.f, 0.f, 1.f, 2, "A sanded cap: 0 = clear, more = the light under the cap blurs and the glints go soft, so the print over it reads more easily" },
};

// The traced fields a slider marks as touched in Primitive::m_tracedTouched: one bit per field, the bulb fields then the look keys
constexpr uint32_t kTouchBulbDepth = 1u << 0, kTouchBulbScale = 1u << 1, kTouchBulbBrightness = 1u << 2, kTouchBulbStretchX = 1u << 3, kTouchBulbStretchY = 1u << 4, kTouchBulbOffsetX = 1u << 5, kTouchBulbOffsetY = 1u << 6;
constexpr uint32_t LookKeyBit(size_t index) { return 1u << (7 + index); }
static_assert(7 + std::size(kLookKeys) <= 32, "Primitive::m_tracedTouched has a bit for each bulb field and look key");

struct TracedBulbField
{
   const char* label;
   float PrimitiveData::*field;
   uint32_t bit;
};

constexpr TracedBulbField kBulbFields[] = {
   { "Bulb Depth", &PrimitiveData::m_bulbDepth, kTouchBulbDepth },
   { "Bulb Spot Size", &PrimitiveData::m_bulbScale, kTouchBulbScale },
   { "Bulb Brightness", &PrimitiveData::m_bulbBrightness, kTouchBulbBrightness },
   { "Bulb Stretch X", &PrimitiveData::m_bulbStretchX, kTouchBulbStretchX },
   { "Bulb Stretch Y", &PrimitiveData::m_bulbStretchY, kTouchBulbStretchY },
   { "Bulb Offset X", &PrimitiveData::m_bulbOffsetX, kTouchBulbOffsetX },
   { "Bulb Offset Y", &PrimitiveData::m_bulbOffsetY, kTouchBulbOffsetY },
};
}


namespace VPX::EditorUI
{

vector<string> PrimitiveUIPart::TouchedTracedLabels(const Primitive* primitive)
{
   vector<string> labels;
   for (const TracedBulbField& bulb : kBulbFields)
      if (primitive->m_tracedTouched & bulb.bit)
         labels.emplace_back(bulb.label);
   for (size_t i = 0; i < std::size(kLookKeys); i++)
      if (primitive->m_tracedTouched & LookKeyBit(i))
         labels.emplace_back(kLookKeys[i].label);
   return labels;
}

void PrimitiveUIPart::CopyTouchedTraced(const Primitive* source, Primitive* target)
{
   for (const TracedBulbField& bulb : kBulbFields)
      if (source->m_tracedTouched & bulb.bit)
         target->m_d.*bulb.field = source->m_d.*bulb.field;
   for (size_t i = 0; i < std::size(kLookKeys); i++)
      if (source->m_tracedTouched & LookKeyBit(i))
         SetTracedLook(target->m_d.m_tracedParams, kLookKeys[i].key, GetTracedLook(source->m_d.m_tracedParams, kLookKeys[i]));
}

PrimitiveUIPart::PrimitiveUIPart(Primitive* primitive)
   : EditableUIPart(primitive, { &Data::m_visible })
{
}

PrimitiveUIPart::TransformMask PrimitiveUIPart::GetTransform(Matrix3D& transform)
{
   if (m_part->IsPlasticCompanion())
      return TM_None; // Derived from its host: not moved, and not picked by a box select
   if (m_part->m_d.m_plasticGen)
      return GetCurveTransform(transform, m_part->m_d.m_plasticHeight + m_part->m_d.m_plasticThickness);
   if (m_part->m_d.m_wireGen)
   {
      // Metal Wire Guide: the wire points are in table coordinates, the gizmo works on them like on a ramp curve
      const auto& pts = m_part->m_wireCurve.GetPoints();
      float z = 0.f;
      for (const auto& pt : pts)
         z += pt->GetZ();
      return GetCurveTransform(transform, (pts.empty() ? 0.f : z / (float)pts.size()) + m_part->m_d.m_wireHeight);
   }
   const Matrix3D Smatrix = Matrix3D::MatrixScale(m_part->m_d.m_vSize.x, m_part->m_d.m_vSize.y, m_part->m_d.m_vSize.z);
   const Matrix3D Tmatrix = Matrix3D::MatrixTranslate(m_part->m_d.m_vPosition);
   const Matrix3D Rmatrix = (Matrix3D::MatrixRotateZ(ANGTORAD(m_part->m_d.m_aRotAndTra[2])) //
                               * Matrix3D::MatrixRotateY(ANGTORAD(m_part->m_d.m_aRotAndTra[1]))) //
      * Matrix3D::MatrixRotateX(ANGTORAD(m_part->m_d.m_aRotAndTra[0]));
   transform = (Smatrix * Rmatrix) * Tmatrix; // fullMatrix = Scale * Rotate * Translate
   return TM_TRSAny;
}

void PrimitiveUIPart::SetTransform(const vec3& pos, const vec3& scale, const vec3& rot)
{
   if (m_part->IsPlasticCompanion())
      return;
   if (m_part->m_d.m_plasticGen)
   {
      const float z = m_part->m_d.m_plasticHeight + m_part->m_d.m_plasticThickness;
      SetCurveTransform(pos, scale, rot);
      if (fabsf(pos.z - z) > 1e-4f)
      {
         const float dz = pos.z - z;
         m_part->m_d.m_plasticHeight += dz;
         m_part->OnGenPointsMoved(Matrix3D::MatrixTranslate(0.f, 0.f, dz));
      }
      return;
   }
   if (m_part->m_d.m_wireGen)
   {
      const auto& pts = m_part->m_wireCurve.GetPoints();
      float z = 0.f;
      for (const auto& pt : pts)
         z += pt->GetZ();
      z = (pts.empty() ? 0.f : z / (float)pts.size()) + m_part->m_d.m_wireHeight;
      SetCurveTransform(pos, scale, rot);
      if (fabsf(pos.z - z) > 1e-4f)
      {
         for (const auto& pt : pts)
            pt->SetZ(pt->GetZ() + pos.z - z);
         m_part->OnGenPointsMoved(Matrix3D::MatrixTranslate(0.f, 0.f, pos.z - z));
      }
      return;
   }
   m_part->m_d.m_vPosition = pos;
   m_part->m_d.m_aRotAndTra[0] = rot.x;
   m_part->m_d.m_aRotAndTra[1] = rot.y;
   m_part->m_d.m_aRotAndTra[2] = rot.z;
   m_part->m_d.m_vSize = scale;
}

void PrimitiveUIPart::RenderOverlay(const EditorRenderContext& ctx)
{
   if (m_part->IsRailChannel())
      return;
   ctx.DrawWireframe(m_part);
   if (m_part->m_d.m_wireRail)
      RenderRailOverlay(ctx);
}

void PrimitiveUIPart::InsertPointOnCurve(const Vertex2D& pos)
{
   if (m_part->m_d.m_plasticGen)
   {
      DragPointCurve* const curve = GetDragPointCurve();
      if (m_part->m_plasticEditHoles)
      {
         // A new hole at the position, with the current hole diameter
         curve->PushPoint(std::make_unique<DragPoint>(curve, pos.x, pos.y, m_part->m_d.m_plasticHoleDia, false));
         return;
      }
      // A corner on the nearest chord of the control polygon (the closest point of the chord, not of an arc: an arc next to it becomes a new circle)
      const auto& pts = curve->GetPoints();
      const int n = (int)pts.size();
      float best = FLT_MAX;
      int at = 0;
      Vertex2D out(pos);
      for (int i = 0; i < n; i++)
      {
         const DragPoint* const a = pts[i].get();
         const DragPoint* const b = pts[(i + 1) % n].get();
         const float dx = b->GetX() - a->GetX(), dy = b->GetY() - a->GetY();
         const float len2 = dx * dx + dy * dy;
         const float t = len2 > 0.f ? clamp(((pos.x - a->GetX()) * dx + (pos.y - a->GetY()) * dy) / len2, 0.f, 1.f) : 0.f;
         const float px = a->GetX() + t * dx, py = a->GetY() + t * dy;
         const float d2 = (px - pos.x) * (px - pos.x) + (py - pos.y) * (py - pos.y);
         if (d2 < best)
         {
            best = d2;
            at = i + 1;
            out = Vertex2D(px, py);
         }
      }
      curve->InsertPoint(at, std::make_unique<DragPoint>(curve, out.x, out.y, 0.f, false));
      return;
   }
   // Open path: insert on the nearest segment (the default implementation assumes a closed 2D curve), z interpolated
   DragPointCurve* const curve = &m_part->m_wireCurve;
   vector<RenderVertex3D> vv;
   curve->GetRgVertex(vv, false, 4.f);
   Vertex2D out;
   int seg = -1;
   ClosestPointOnPolygon(vv, pos, out, seg, false);
   if (seg < 0 || seg + 1 >= (int)vv.size())
      return;
   int icp = 0;
   for (int i = 0; i < seg + 1; i++)
      if (vv[i].controlPoint)
         icp++;
   const float dx = vv[seg + 1].x - vv[seg].x, dy = vv[seg + 1].y - vv[seg].y;
   const float len2 = dx * dx + dy * dy;
   const float t = len2 > 0.f ? clamp(((out.x - vv[seg].x) * dx + (out.y - vv[seg].y) * dy) / len2, 0.f, 1.f) : 0.f;
   curve->InsertPoint(icp, std::make_unique<DragPoint>(curve, out.x, out.y, vv[seg].z + t * (vv[seg + 1].z - vv[seg].z), true));
   if (m_part->m_d.m_wireRail && icp > 0 && icp + 1 < (int)curve->GetPoints().size())
   {
      // On a banked rail the new point takes the roll between its neighbours, by plan distance
      const auto &pts = curve->GetPoints();
      const DragPoint *const a = pts[icp - 1].get();
      const DragPoint *const b = pts[icp + 1].get();
      const float da = sqrtf((out.x - a->GetX()) * (out.x - a->GetX()) + (out.y - a->GetY()) * (out.y - a->GetY()));
      const float db = sqrtf((out.x - b->GetX()) * (out.x - b->GetX()) + (out.y - b->GetY()) * (out.y - b->GetY()));
      pts[icp]->m_roll = da + db > 0.f ? a->m_roll + (b->m_roll - a->m_roll) * da / (da + db) : a->m_roll;
   }
}

void PrimitiveUIPart::UpdatePointZField(PropertyPane& props, DragPoint* point)
{
   if (m_part->m_d.m_plasticGen)
   {
      if (m_part->m_plasticEditHoles) // A hole keeps its diameter in z
         props.InputFloat<DragPoint>(
            point, "Diameter"s, //
            [](const DragPoint* p) { return p->GetZ(); }, //
            [](DragPoint* p, float v) { static_cast<Primitive*>(p->GetIEditable())->SetPlasticHoleDiameter(p, v); }, PropertyPane::Unit::VPLength, 2, "Diameter of this hole (both ends of a pill)");
      return;
   }
   if (m_part->m_d.m_wireRail)
   {
      props.InputFloat<DragPoint>(
         point, "Height"s, //
         [](const DragPoint* p) { return p->GetZ(); }, //
         [](DragPoint* p, float v) { p->SetZ(v); }, PropertyPane::Unit::VPLength, 1, "Height of the route at this point (the Z handle of the gizmo moves it too)");
      props.InputFloat<DragPoint>(
         point, "Roll"s, //
         [](const DragPoint* p) { return p->m_roll; }, //
         [](DragPoint* p, float v) { p->m_roll = clamp(v, -180.f, 180.f); }, PropertyPane::Unit::Degree, 1, "Rolls the wires around the route at this point, positive raises the right side: bank a turn");
      return;
   }
   props.InputFloat<DragPoint>(
      point, "Z Offset"s, //
      [](const DragPoint* p) { return p->GetZ(); }, //
      [](DragPoint* p, float v) { p->SetZ(v); }, PropertyPane::Unit::VPLength, 1, "Height of this point of the wire above the wire Height (the gizmo moves points in the XY plane only)");
}

void PrimitiveUIPart::UpdatePropertyPane(PropertyPane& props)
{
   if (m_part->m_d.m_wireRail) // A rail is made here: above the tabs
      props.PrependAfterHeader(
         [this](PropertyPane& p)
         {
            UpdateCurveSection(p);
            UpdateRailPanel(p);
         });
   props.EditableHeader(m_part->m_d.m_plasticGen ? "Plastic from Atlas"s : m_part->m_d.m_wireRail ? "Wire Rail"s : m_part->m_d.m_wireGen ? "Metal Wire Guide"s : "Primitive"s, m_part);

   if (!m_part->m_d.m_wireRail)
      UpdateCurveSection(props);

   if (m_part->m_d.m_wireGen && !m_part->m_d.m_wireRail && props.BeginSection("Metal Wire Guide"s))
   {
      props.InputFloat<Primitive>(
         m_part, "Height"s, //
         [](const Primitive* p) { return p->m_d.m_wireHeight; }, //
         [](Primitive* p, float v) { p->m_d.m_wireHeight = clamp(v, -1000.f, 2000.f); p->UpdateWire(); }, PropertyPane::Unit::VPLength, 2, "Wire height above the playfield (point Z values are offsets from it)");
      props.InputFloat<Primitive>(
         m_part, "Thickness"s, //
         [](const Primitive* p) { return p->m_d.m_wireThickness; }, //
         [](Primitive* p, float v) { p->m_d.m_wireThickness = clamp(v, 0.1f, 50.f); p->UpdateWire(); }, PropertyPane::Unit::VPLength, 2, "Wire radius");
      props.InputFloat<Primitive>(
         m_part, "Stand Height"s, //
         [](const Primitive* p) { return p->m_d.m_wireStandHeight; }, //
         [](Primitive* p, float v) { p->m_d.m_wireStandHeight = clamp(v, 0.f, 1000.f); p->UpdateWire(); }, PropertyPane::Unit::VPLength, 2, "Stands: length of the leg below the wire");
      props.InputFloat<Primitive>(
         m_part, "Bend Radius"s, //
         [](const Primitive* p) { return p->m_d.m_wireBendRadius; }, //
         [](Primitive* p, float v) { p->m_d.m_wireBendRadius = clamp(v, 0.f, 200.f); p->UpdateWire(); }, PropertyPane::Unit::VPLength, 2, "Stands: radius of the bend into the leg");
      props.Combo<Primitive>(
         m_part, "Ends"s, vector<string> { "Stands"s, "Round"s, "Flat"s, "None"s }, //
         [](const Primitive* p) { return p->m_d.m_wireEnds; }, //
         [](Primitive* p, int v) { p->m_d.m_wireEnds = clamp(v, 0, 3); p->UpdateWire(); });
      props.InputInt<Primitive>(
         m_part, "Rails"s, //
         [](const Primitive* p) { return p->m_d.m_wireRails; }, //
         [](Primitive* p, int v) { p->m_d.m_wireRails = clamp(v, 1, 3); p->UpdateWire(); }, "1 to 3 parallel wires");
      props.InputFloat<Primitive>(
         m_part, "Rail Spacing"s, //
         [](const Primitive* p) { return p->m_d.m_wireSpacing; }, //
         [](Primitive* p, float v) { p->m_d.m_wireSpacing = max(v, 0.f); p->UpdateWire(); }, PropertyPane::Unit::VPLength, 1);
      props.InputInt<Primitive>(
         m_part, "Sides"s, //
         [](const Primitive* p) { return p->m_d.m_wireSides; }, //
         [](Primitive* p, int v) { p->m_d.m_wireSides = clamp(v, 3, 32); p->UpdateWire(); }, "Segments around the wire");
      props.EndSection();
   }

   if (m_part->m_d.m_plasticGen && props.BeginSection("Plastic from Atlas"s))
   {
      props.InputFloat<Primitive>(
         m_part, "Height"s, //
         [](const Primitive* p) { return p->m_d.m_plasticHeight; }, //
         [](Primitive* p, float v) { p->m_d.m_plasticHeight = clamp(v, -1000.f, 2000.f); p->UpdatePlastic(); }, PropertyPane::Unit::VPLength, 2, "Underside of the plastic above the playfield");
      props.InputFloat<Primitive>(
         m_part, "Thickness"s, //
         [](const Primitive* p) { return p->m_d.m_plasticThickness; }, //
         [](Primitive* p, float v) { p->m_d.m_plasticThickness = clamp(v, 0.1f, 100.f); p->UpdatePlastic(); }, PropertyPane::Unit::VPLength, 2, "Plastic thickness (3 mm = 5.6)");
      props.InputFloat<Primitive>(
         m_part, "Bevel"s, //
         [](const Primitive* p) { return p->m_d.m_plasticBevel; }, //
         [](Primitive* p, float v) { p->m_d.m_plasticBevel = clamp(v, 0.f, p->m_d.m_plasticThickness); p->UpdatePlastic(); }, PropertyPane::Unit::VPLength, 2, "Bevel on the top side, offset inward (1 mm = 1.9)");
      props.InputInt<Primitive>(
         m_part, "Bevel Segments"s, //
         [](const Primitive* p) { return p->m_d.m_plasticBevelSegs; }, //
         [](Primitive* p, int v) { p->m_d.m_plasticBevelSegs = clamp(v, 1, 16); p->UpdatePlastic(); }, "1 = chamfer, more = rounded");
      props.InputInt<Primitive>(
         m_part, "Curve Detail"s, //
         [](const Primitive* p) { return p->m_d.m_plasticDetail; }, //
         [](Primitive* p, int v) { p->m_d.m_plasticDetail = clamp(v, 1, 5); p->UpdatePlastic(); }, "How smooth arcs and hole circles are (1 to 5, 1 = default; at most 96 segments on any one arc)");
      props.InputFloat<Primitive>(
         m_part, "Hole Diameter"s, //
         [](const Primitive* p) { return p->m_d.m_plasticHoleDia; }, //
         [](Primitive* p, float v) { p->m_d.m_plasticHoleDia = clamp(v, 0.5f, 100.f); }, PropertyPane::Unit::VPLength, 2, "Diameter of the next hole placed (M3 clearance 6.4, post 12, large post 16)");
      props.InputFloat<Primitive>(
         m_part, "Sheet DPI"s, //
         [](const Primitive* p) { return p->m_d.m_plasticDpi; }, //
         [](Primitive* p, float v) { p->SetPlasticDpi(v); p->UpdatePlastic(); }, PropertyPane::Unit::None, 1, "Scale of the atlas: 47.06 VPU per inch at this DPI (the art scales about the plastic)");
      if (GetPointEditContext() != nullptr)
         props.Combo<Primitive>(
            m_part, "Edit"s, vector<string> { "Outline"s, "Holes"s }, //
            [](const Primitive* p) { return p->m_plasticEditHoles ? 1 : 0; }, //
            [](Primitive* p, int v) { p->m_plasticEditHoles = v == 1; }, "H switches while editing points");
      props.EndSection();
   }
   else if (m_part->IsPlasticCompanion() && props.BeginSection("Plastic from Atlas"s))
   {
      ImGui::TextDisabled("Part of %s: its mesh is derived from the plastic, edit it there", m_part->m_d.m_plasticHost.c_str());
      props.EndSection();
   }
   // The clear and bevel parts are engine details of a plastic: their materials and refraction are edited here, on the plastic, in the same undo step
   if (m_part->m_d.m_plasticGen)
   {
      Primitive *const clear = PlasticFromAtlas::CompanionOf(props.GetEditedPart<Primitive>(m_part), 1);
      Primitive *const bevel = PlasticFromAtlas::CompanionOf(props.GetEditedPart<Primitive>(m_part), 2);
      if (clear != nullptr && bevel != nullptr && props.BeginSection("Clear Plastic"s))
      {
         // Several plastics selected: a Clear field edits the clear part of each of them, a Bevel field the bevel part (never a print, never a part that is no
         // plastic: those have no companion and are skipped). The other selected parts are swapped for those companions while the fields are drawn
         const vector<IEditable *> selectedHosts = props.GetCoEditedParts();
         const auto companionsOf = [&props, &selectedHosts](const int role)
         {
            const PropertyPane::CompanionLookup &lookup = props.GetCompanionLookup();
            vector<IEditable *> comps;
            for (IEditable *const other : selectedHosts)
               if (other->GetItemType() == eItemPrimitive)
               {
                  const Primitive *const host = props.GetEditedPart<Primitive>(static_cast<Primitive *>(other));
                  if (Primitive *const comp = lookup ? lookup(host, role) : PlasticFromAtlas::CompanionOf(host, role); comp != nullptr)
                     comps.push_back(comp);
               }
            return comps;
         };
         const auto refraction = [&props](Primitive *const part, const string &prefix)
         {
            props.RenderProbeCombo<Primitive>(
               part, prefix + "Refraction Probe"s, //
               [](const Primitive* p) { return p->m_d.m_szRefractionProbe; }, //
               [](Primitive* p, const string& v) { p->m_d.m_szRefractionProbe = v; }, "Render probe providing the image seen through the clear plastic");
            props.InputFloat<Primitive>(
               part, prefix + "Refraction Thickness"s, //
               [](const Primitive* p) { return p->m_d.m_refractionThickness; }, //
               [](Primitive* p, float v) { p->m_d.m_refractionThickness = v; }, PropertyPane::Unit::None, 3, "Simulated thickness used to refract light through the plastic");
            props.SliderFloat<Primitive>(
               part, prefix + "Refraction Curvature"s, //
               [](const Primitive* p) { return p->m_d.m_refractionCurvature; }, //
               [](Primitive* p, float v) { p->m_d.m_refractionCurvature = max(v, 0.f); }, 0.f, 8.f, 2, "Curvature-driven refraction strength (0 = off)");
            props.SliderFloat<Primitive>(
               part, prefix + "Refraction Anisotropy"s, //
               [](const Primitive* p) { return p->m_d.m_refractionAnisotropy; }, //
               [](Primitive* p, float v) { p->m_d.m_refractionAnisotropy = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
            props.SliderFloat<Primitive>(
               part, prefix + "Refraction Wave"s, //
               [](const Primitive* p) { return p->m_d.m_refractionWave; }, //
               [](Primitive* p, float v) { p->m_d.m_refractionWave = max(v, 0.f); }, 0.f, 3.f, 2);
            props.SliderFloat<Primitive>(
               part, prefix + "Refraction Wave Flow"s, //
               [](const Primitive* p) { return p->m_d.m_refractionWaveFlow; }, //
               [](Primitive* p, float v) { p->m_d.m_refractionWaveFlow = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
            props.SliderFloat<Primitive>(
               part, prefix + "Refraction Noise"s, //
               [](const Primitive* p) { return p->m_d.m_refractionNoise; }, //
               [](Primitive* p, float v) { p->m_d.m_refractionNoise = max(v, 0.f); }, 0.f, 10.f, 2);
            props.SliderFloat<Primitive>(
               part, prefix + "Refraction Noise Scale (mm)"s, //
               [](const Primitive* p) { return p->m_d.m_refractionNoiseScale; }, //
               [](Primitive* p, float v) { p->m_d.m_refractionNoiseScale = max(v, 0.01f); }, 0.5f, 10.f, 2);
         };
         {
            PropertyPane::CoEditScope scope(props, companionsOf(1));
            props.MaterialCombo<Primitive>(
               clear, "Clear Material"s, //
               [](const Primitive* p) { return p->m_d.m_szMaterial; }, //
               [](Primitive* p, const string& v) { p->m_d.m_szMaterial = v; }, "Material of the clear top");
            refraction(clear, "Clear "s);
         }
         {
            PropertyPane::CoEditScope scope(props, companionsOf(2));
            props.MaterialCombo<Primitive>(
               bevel, "Bevel Material"s, //
               [](const Primitive* p) { return p->m_d.m_szMaterial; }, //
               [](Primitive* p, const string& v) { p->m_d.m_szMaterial = v; }, "Material of the walls and bevel");
            refraction(bevel, "Bevel "s);
         }
         props.EndSection(); // (RefreshEditedParts reinitializes both parts after an edit)
      }
   }

   // Apply asynchronous mesh file dialog results (not in the hidden pin harvest pane, which would open the dialogs out of sight)
   if (!props.IsHarvest() && m_pendingMeshImport && !m_pendingMeshImport->empty())
   {
      // A file was selected: reset the options to their defaults and show the import option dialog
      m_meshImportFileName = *m_pendingMeshImport;
      m_pendingMeshImport = nullptr;
      m_meshUnitsMeters = false;
      m_meshImportAbsolutePosition = false;
      m_meshImportCenterMesh = false;
      m_meshImportMaterial = false;
      m_meshImportAnimation = false;
      m_meshImportNoForsyth = false;
      m_meshImportFailed = false;
      ImGui::OpenPopup("Wavefront OBJ Importer");
   }
   if (!props.IsHarvest() && m_pendingMeshExport && !m_pendingMeshExport->empty())
   {
      // A file was selected: reset the options to their defaults and show the export option dialog
      m_meshExportFileName = *m_pendingMeshExport;
      m_pendingMeshExport = nullptr;
      m_meshUnitsMeters = false;
      m_meshExportFailed = false;
      ImGui::OpenPopup("Wavefront OBJ Exporter");
   }

   // Import option dialog (same options as the WinUI 'Wavefront OBJ Importer' dialog)
   if (!m_meshImportFileName.empty())
   {
      if (ImGui::BeginPopupModal("Wavefront OBJ Importer", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
         ImGui::TextWrapped("%s", m_meshImportFileName.c_str());
         ImGui::Spacing();
         UpdateMeshUnitsUI();
         ImGui::SeparatorText("Options");
         ImGui::Columns(2, nullptr, true);
         ImGui::Checkbox("Center mesh to its midpoint", &m_meshImportCenterMesh);
         int position = m_meshImportAbsolutePosition ? 1 : 0;
         ImGui::RadioButton("Place at primitive's position", &position, 0);
         ImGui::RadioButton("Place at mesh's absolute position (use mesh's midpoint)", &position, 1);
         m_meshImportAbsolutePosition = position != 0;
         ImGui::NextColumn();
         ImGui::Checkbox("Import mesh's material", &m_meshImportMaterial);
         ImGui::Checkbox("Import Animation Sequence", &m_meshImportAnimation);
         ImGui::Checkbox("Do not reorder/optimize data", &m_meshImportNoForsyth);
         ImGui::Columns(1);
         if (m_meshImportFailed)
            ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), "Failed to import file!");
         ImGui::Separator();
         if (ImGui::Button("Import"))
         {
            const std::filesystem::path meshPath = PathFromUTF8(m_meshImportFileName); // The dialog and the table use UTF-8
            m_part->m_d.m_meshFileName = PathToUTF8(meshPath.filename());
            m_meshImportFailed = !m_part->LoadMesh(meshPath, m_meshUnitsMeters ? MeshUnits::Meters : MeshUnits::VPUnits, m_meshImportAbsolutePosition, m_meshImportCenterMesh,
               m_meshImportMaterial, m_meshImportAnimation, !m_meshImportNoForsyth);
            if (!m_meshImportFailed)
            {
               m_meshImportFileName.clear();
               ImGui::CloseCurrentPopup();
            }
         }
         ImGui::SameLine();
         if (ImGui::Button("Cancel") || PropertyPane::ModalEscapePressed())
         {
            m_meshImportFileName.clear();
            ImGui::CloseCurrentPopup();
         }
         ImGui::EndPopup();
      }
      else
         m_meshImportFileName.clear();
   }

   // Export option dialog (same options as the WinUI 'Wavefront OBJ Exporter' dialog)
   if (!m_meshExportFileName.empty())
   {
      if (ImGui::BeginPopupModal("Wavefront OBJ Exporter", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
         ImGui::TextWrapped("%s", m_meshExportFileName.c_str());
         ImGui::Spacing();
         UpdateMeshUnitsUI();
         if (m_meshExportFailed)
            ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), "Failed to export file!");
         ImGui::Separator();
         if (ImGui::Button("Export"))
         {
            m_meshExportFailed = !m_part->m_mesh.SaveWavefrontObj(
               PathFromUTF8(m_meshExportFileName), m_part->m_d.m_use3DMesh ? m_part->m_name : "Primitive"s, m_meshUnitsMeters ? MeshUnits::Meters : MeshUnits::VPUnits);
            if (!m_meshExportFailed)
            {
               m_meshExportFileName.clear();
               ImGui::CloseCurrentPopup();
            }
         }
         ImGui::SameLine();
         if (ImGui::Button("Cancel") || PropertyPane::ModalEscapePressed())
         {
            m_meshExportFileName.clear();
            ImGui::CloseCurrentPopup();
         }
         ImGui::EndPopup();
      }
      else
         m_meshExportFileName.clear();
   }

   if (props.BeginSection("Visuals"s))
   {
      props.Separator("Geometry"s);
      if (ImGui::Button("Import Mesh"))
         ImportMesh();
      ImGui::SameLine();
      ImGui::BeginDisabled(!props.GetEditedPart<Primitive>(m_part)->m_d.m_use3DMesh);
      if (ImGui::Button("Export Mesh"))
         ExportMesh();
      ImGui::EndDisabled();
      if (props.GetEditedPart<Primitive>(m_part)->m_d.m_use3DMesh)
      {
         ImGui::BeginDisabled();
         props.InputString<Primitive>(
            m_part, "Mesh File"s, //
            [](const Primitive* primitive) { return primitive->m_d.m_meshFileName; }, //
            [](Primitive*, const string&) {});
         ImGui::EndDisabled();
      }
      else
      {
         props.InputInt<Primitive>(
            m_part, "Sides"s, //
            [](const Primitive* primitive) { return primitive->m_d.m_Sides; }, //
            [](Primitive* primitive, int v) { primitive->SetSides(v); });
         props.Checkbox<Primitive>(
            m_part, "Draw Textures Inside"s, //
            [](const Primitive* primitive) { return primitive->m_d.m_drawTexturesInside; }, //
            [](Primitive* primitive, bool v) { primitive->m_d.m_drawTexturesInside = v; }, "Render the image on the inside faces of the primitive");
      }

      props.Separator("Render Options"s);
      props.Checkbox<Primitive>(
         m_part, "Visible"s, //
         [this](const Primitive* primitive) { return GetVisibility(primitive); }, //
         [this](Primitive* primitive, bool v) { SetVisibility(primitive, v); });
      props.Checkbox<Primitive>(
         m_part, "Static Rendering"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_staticRendering; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_staticRendering = v; }, "Bake this part into the static prerender pass (faster, but it cannot move or change at runtime)");
      props.Checkbox<Primitive>(
         m_part, "Reflection Enabled"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_reflectionEnabled; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_reflectionEnabled = v; }, "Render this part in the table's reflections");
      props.InputFloat<Primitive>(
         m_part, "Depth Bias"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_depthBias; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_depthBias = v; }, PropertyPane::Unit::None, 0,
         "Depth offset applied at render time to fix flickering between overlapping parts");
      props.Checkbox<Primitive>(
         m_part, "Depth Mask"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_useDepthMask; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_useDepthMask = v; },
         "Write this part to the depth buffer (off lets parts behind it show through, for hollow or transparent meshes)");
      props.Checkbox<Primitive>(
         m_part, "Render Backfaces"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_backfacesEnabled; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_backfacesEnabled = v; }, "Draw the back faces of the mesh as well as the front ones");
      props.Checkbox<Primitive>(
         m_part, "Cull Back Faces"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_cullBackfaces; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_cullBackfaces = v; },
         "Draw front faces only when Depth Mask is off; use for closed, double-walled transparent meshes. Also sorts its triangles back to front for the camera.");
      props.Checkbox<Primitive>(
         m_part, "Additive Blend"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_addBlend; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_addBlend = v; }, "Add this part's light over the scene instead of drawing it opaque");
      props.LightmapCombo<Primitive>(
         m_part, "Lightmap"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szLightmap; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szLightmap = v; }, "Use this light's illumination as the part's baked lighting");
      props.InputResURI<Primitive>(
         m_part, "State Link"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_stateSrcLink; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_stateSrcLink = v; }, ResURIKind::State,
         "URI linking this part's state to a shared state source (plugin or external controller)", DisplayFamily::Any, -1, false, -1);

      props.Separator("Material"s);
      if (props.GetEditedPart<Primitive>(m_part)->IsPlayfield())
      {
         // The playfield mesh's material and image are overridden by the table's playfield definition
         props.MaterialCombo<Primitive>(
            m_part, "Material"s, //
            [](const Primitive* primitive) { return primitive->m_ptable->m_playfieldMaterial; }, //
            [](Primitive* primitive, const string& v) { primitive->m_ptable->m_playfieldMaterial = v; });
         props.ImageCombo<Primitive>(
            m_part, "Image"s, //
            [](const Primitive* primitive) { return primitive->m_ptable->m_image; }, //
            [](Primitive* primitive, const string& v) { primitive->m_ptable->m_image = v; });
         ImGui::BeginDisabled(m_part->m_ptable->m_image.empty());
         props.Checkbox<Primitive>(
            m_part, "Show in Editor"s, //
            [](const Primitive* primitive) { return primitive->m_ptable->m_winEditorBackdrop; }, //
            [](Primitive* primitive, bool v) { primitive->m_ptable->m_winEditorBackdrop = v; });
         ImGui::EndDisabled();
         // The implicit playfield mesh does not support normal mapping
         ImGui::BeginDisabled(m_part == g_pplayer->m_implicitPlayfieldMesh);
         props.MaterialVisuals(m_part, {}, nullptr, {}, nullptr, nullptr, &Data::m_szNormalMap, &Data::m_objectSpaceNormalMap);
         ImGui::EndDisabled();
      }
      else
         props.MaterialVisuals(m_part, "Material"s, &Data::m_szMaterial, "Image"s, &Data::m_szImage, &Data::m_displayTexture, &Data::m_szNormalMap, &Data::m_objectSpaceNormalMap);
      props.ImageCombo<Primitive>(
         m_part, "ORM Map"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szORMMap; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szORMMap = v; });
      props.ImageCombo<Primitive>(
         m_part, "Emission Map"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szEmissionMap; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szEmissionMap = v; });
      props.InputFloat<Primitive>(
         m_part, "Disable Spot Lights"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_disableLightingTop; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_disableLightingTop = v; }, PropertyPane::Unit::None, 3,
         "Reduce the effect of dynamic light sources on the top of this part (0 = fully lit)");
      props.InputFloat<Primitive>(
         m_part, "Translucency"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_disableLightingBelow; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_disableLightingBelow = v; }, PropertyPane::Unit::None, 3, "Let light pass through the part from below, faking translucency");
      props.InputFloat<Primitive>(
         m_part, "Modulate Opacity"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_alpha; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_alpha = v; }, PropertyPane::Unit::None, 3, "Scale the part's opacity on top of the image's alpha");
      props.InputRGB<Primitive>(
         m_part, "Modulate Color"s, //
         [](const Primitive* primitive) { return convertColor(primitive->m_d.m_color); }, //
         [](Primitive* primitive, const vec3& v) { primitive->m_d.m_color = convertColorRGB(v); }, "Color multiplied with the part's image and material");

      props.Separator("Reflections");
      props.RenderProbeCombo<Primitive>(
         m_part, "Reflection Probe"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szReflectionProbe; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szReflectionProbe = v; }, "Render probe providing this part's reflection image");
      props.InputFloat<Primitive>(
         m_part, "Reflection Strength"s, //
         [](const Primitive* primitive) { return primitive->IsPlayfield() ? primitive->m_ptable->m_playfieldReflectionStrength : primitive->m_d.m_reflectionStrength; }, //
         [](Primitive* primitive, float v)
         {
            if (primitive->IsPlayfield())
               primitive->m_ptable->m_playfieldReflectionStrength = v;
            else
               primitive->m_d.m_reflectionStrength = v;
         },
         PropertyPane::Unit::None, 3);

      props.Separator("Refractions"s);
      props.RenderProbeCombo<Primitive>(
         m_part, "Refraction Probe"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szRefractionProbe; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szRefractionProbe = v; }, "Render probe providing the image seen through this part");
      props.InputFloat<Primitive>(
         m_part, "Refraction Thickness"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionThickness; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionThickness = v; }, PropertyPane::Unit::None, 3, "Simulated thickness used to refract light through this part");
      props.SliderFloat<Primitive>(
         m_part, "Refraction Curvature"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionCurvature; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionCurvature = max(v, 0.f); }, 0.f, 8.f, 2, "Curvature-driven refraction strength (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Refraction Anisotropy"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionAnisotropy; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionAnisotropy = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
      props.SliderFloat<Primitive>(
         m_part, "Refraction Wave"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionWave; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionWave = max(v, 0.f); }, 0.f, 3.f, 2);
      props.SliderFloat<Primitive>(
         m_part, "Refraction Wave Flow"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionWaveFlow; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionWaveFlow = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2);
      props.SliderFloat<Primitive>(
         m_part, "Refraction Noise"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionNoise; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionNoise = max(v, 0.f); }, 0.f, 10.f, 2); // 1 = ~0.7 deg tilt; range far past "sensible" on purpose (Ben: dial past, then back)
      props.SliderFloat<Primitive>(
         m_part, "Refraction Noise Scale (mm)"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_refractionNoiseScale; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_refractionNoiseScale = max(v, 0.01f); }, 0.5f, 10.f, 2);

      // Traced insert (an additive card with a TRACED_ image): the lit insert's hot spot over the bulb, parallaxing with the view
      props.Separator("Traced Insert Bulb"s);
      props.SliderFloat<Primitive>(
         m_part, "Bulb Depth (mm)"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbDepth; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbDepth = max(v, 0.f); primitive->m_tracedTouched |= kTouchBulbDepth; }, 0.f, 30.f, 1, "A soft hot spot in the lit insert over the bulb, this far below the top, shifting with the view (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Spot Size"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbScale; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbScale = max(v, 0.05f); primitive->m_tracedTouched |= kTouchBulbScale; }, 0.05f, 0.4f, 2, "Hot spot size (1 = about 19 mm across at half height)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Brightness"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbBrightness; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbBrightness = max(v, 0.f); primitive->m_tracedTouched |= kTouchBulbBrightness; }, 0.f, 4.f, 2, "Hot spot brightness (1 = +80 % of the glow's peak at its centre)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Stretch X"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbStretchX; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbStretchX = max(v, 0.05f); primitive->m_tracedTouched |= kTouchBulbStretchX; }, 0.2f, 4.f, 2, "Hot spot stretch along the card's X (1 = round)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Stretch Y"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbStretchY; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbStretchY = max(v, 0.05f); primitive->m_tracedTouched |= kTouchBulbStretchY; }, 0.2f, 4.f, 2, "Hot spot stretch along the card's Y (1 = round)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Offset X (mm)"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbOffsetX; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbOffsetX = v; primitive->m_tracedTouched |= kTouchBulbOffsetX; }, -25.f, 25.f, 1, "Moves the hot spot along the card's X (the bulb is not under the insert's centre)");
      props.SliderFloat<Primitive>(
         m_part, "Bulb Offset Y (mm)"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_bulbOffsetY; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_bulbOffsetY = v; primitive->m_tracedTouched |= kTouchBulbOffsetY; }, -25.f, 25.f, 1, "Moves the hot spot along the card's Y");

      // The rest of a traced insert's look lives in its traced_params text: each slider reads and writes one key of it, and the renderer reads the text at draw time
      props.Separator("Traced Insert Look"s);
      for (size_t i = 0; i < std::size(kLookKeys); i++)
      {
         const TracedLookKey& key = kLookKeys[i];
         const uint32_t bit = LookKeyBit(i);
         props.SliderFloat<Primitive>(
            m_part, string(key.label), //
            [key](const Primitive* primitive) { return GetTracedLook(primitive->m_d.m_tracedParams, key); }, //
            [key, bit](Primitive* primitive, float v)
            {
               SetTracedLook(primitive->m_d.m_tracedParams, key.key, v);
               primitive->m_tracedTouched |= bit;
            },
            key.vMin, key.vMax, key.decimals, key.tooltip);
      }
      if (!props.IsSkippingRest() && props.GetEditedPart<Primitive>(m_part)->IsTracedInsert())
         props.ApplyBulbToAllButton(m_part);

      props.Separator("Flasher Dome"s);
      props.SliderFloat<Primitive>(
         m_part, "Dome Body"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeBody; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeBody = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2,
         "Thick body: 0 = see-through (the scene behind filters through the plastic), 1 = the scene behind is replaced by the scattered interior");
      props.SliderFloat<Primitive>(
         m_part, "Dome Scatter"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeScatter; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeScatter = max(v, 0.f); }, 0.f, 4.f, 2,
         "Body scatter driven by the dome's own lamp (milky glow in the lamp's colour; 0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Cap Highlight"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeCap; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeCap = max(v, 0.f); }, 0.f, 4.f, 2, "Highlight on the cap seam/bevel at the top (0 = off)");
      props.InputInt<Primitive>(
         m_part, "Dome Flutes"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeFlutes; }, //
         [](Primitive* primitive, int v) { primitive->m_d.m_domeFlutes = clamp(v, 0, 96); }, "Flute count override, 0..96 (0 = measured from the mesh)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Flute Depth"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeFluteDepth; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeFluteDepth = clamp(v, 0.f, 3.f); }, 0.f, 3.f, 2, "Flute depth scale (1 = measured from the mesh)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Peak White"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domePeak; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domePeak = clamp(v, 0.f, 3.f); }, 0.f, 3.f, 2,
         "At full power the lit body and the top go white-hot like a real flash (0 = stays red at the peak); also a hot spot in the middle of the cap");
      props.SliderFloat<Primitive>(
         m_part, "Dome Orb"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeOrb; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeOrb = clamp(v, 0.f, 4.f); }, 0.f, 4.f, 2,
         "The lit bulb seen through the ridges as a hot glowing ball that moves with the view (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Cooldown"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeCooldown; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeCooldown = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2,
         "How the white-hot peak follows a flash: 0 = it switches on only near full power (short flashes snap), 1 = a smooth blackbody pulse that cools from white through yellow and orange to red");
      props.SliderFloat<Primitive>(
         m_part, "Dome Flash Boost"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeBoost; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeBoost = clamp(v, 0.f, 5.f); }, 0.f, 5.f, 2,
         "Gain on the lamp level the dome sees: a flash the game drives below full (short pulses) reads hotter; the lamp's own light on the table is unchanged (1 = as is)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Base Ramp"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeBaseRamp; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeBaseRamp = clamp(roundf(v), 0.f, 60.f); }, 0.f, 60.f, 0,
         "Degrees: the dome's base lets its lamp's light out gradually instead of at a sharp edge, so the brightest light just past the base's shadow on the playfield (a round hot spot under a raised dome) spreads out (0 = sharp, as before)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Depth"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeDepth; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeDepth = clamp(v, 0.f, 3.f); }, 0.f, 3.f, 2,
         "Lamp off: the far wall's ridges seen through the cavity, each flute lensing its own slice, so something moves inside as you move (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Glints"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeGlint; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeGlint = clamp(v, 0.f, 5.f); }, 0.f, 5.f, 2,
         "The lamp-off sparkle: facet glints, ridge glints and needles along the flutes, the room seen in the plastic (1 = as tuned, 0 = none)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Glint Lights"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeGlintLights; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeGlintLights = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2,
         "What the lamp-off sparkle reflects: 0 = a fixed studio room, 1 = the table's own lights near the dome (GI bulbs, inserts, other flashers), so the glints move and change as they light up");
      props.SliderFloat<Primitive>(
         m_part, "Dome Lens"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeLens; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeLens = clamp(v, 0.f, 1.f); }, 0.f, 1.f, 2,
         "See-through walls: each ridge shows a mirrored, squeezed slice of what is behind the dome, drawing fine vertical lines (uses the table's screen copy for transparent parts, or adds one; 0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Frost"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeFrost; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeFrost = clamp(v, 0.f, 3.f); }, 0.f, 3.f, 2,
         "Clear and pale domes: the thick parts of the plastic (ridges, top, base, rim) scatter the room's light to a milky white so the dome reads frosted, not grey (0 = off)");
      props.SliderFloat<Primitive>(
         m_part, "Dome Spill"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_domeSpill; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_domeSpill = clamp(v, 0.f, 4.f); }, 0.f, 4.f, 2,
         "A faint tint of the dome's colour on the clear plastic it stands on, round the base: the table's light coming through the plastic (0 = off)");
      props.InputRGB<Primitive>(
         m_part, "Dome Tint"s, //
         [](const Primitive* primitive) { return convertColor(primitive->m_d.m_domeTint); }, //
         [](Primitive* primitive, const vec3& v) { primitive->m_d.m_domeTint = convertColorRGB(v); },
         "Plastic colour at 2 mm thickness; black = the built-in red acrylic preset");

      props.EndSection();
   }

   if (!props.GetEditedPart<Primitive>(m_part)->m_d.m_wireGen && !props.GetEditedPart<Primitive>(m_part)->m_d.m_plasticGen && !m_part->IsPlasticCompanion() && props.BeginSection("Position"s))
   {
      props.Separator("Position, Rotation & Size"s);
      props.InputFloat3<Primitive>(
         m_part, "Position"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_vPosition; }, //
         [](Primitive* primitive, const vec3& v) { primitive->m_d.m_vPosition = v; }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat3<Primitive>(
         m_part, "Orientation"s, //
         [](const Primitive* primitive) { return vec3(primitive->m_d.m_aRotAndTra[0], primitive->m_d.m_aRotAndTra[1], primitive->m_d.m_aRotAndTra[2]); }, //
         [](Primitive* primitive, const vec3& v) { 
            primitive->m_d.m_aRotAndTra[0] = v.x; 
            primitive->m_d.m_aRotAndTra[1] = v.y;
            primitive->m_d.m_aRotAndTra[2] = v.z;
         },
         PropertyPane::Unit::Degree, 2);
      props.InputFloat3<Primitive>(
         m_part, "Scale"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_vSize; }, //
         [](Primitive* primitive, const vec3& v) { primitive->m_d.m_vSize = v; }, PropertyPane::Unit::PercentX100, 0); // Default primitives have a size of 100

      props.Separator("Additional Transform"s);
      props.InputFloat3<Primitive>(
         m_part, "Translation"s, //
         [](const Primitive* primitive) { return vec3(primitive->m_d.m_aRotAndTra[3], primitive->m_d.m_aRotAndTra[4], primitive->m_d.m_aRotAndTra[5]); }, //
         [](Primitive* primitive, const vec3& v)
         {
            primitive->m_d.m_aRotAndTra[3] = v.x;
            primitive->m_d.m_aRotAndTra[4] = v.y;
            primitive->m_d.m_aRotAndTra[5] = v.z;
         },
         PropertyPane::Unit::VPLength, 1, "Additional offset applied after the base transform (can be animated by script without changing Position)");
      props.InputFloat3<Primitive>(
         m_part, "Rotation"s, //
         [](const Primitive* primitive) { return vec3(primitive->m_d.m_aRotAndTra[6], primitive->m_d.m_aRotAndTra[7], primitive->m_d.m_aRotAndTra[8]); }, //
         [](Primitive* primitive, const vec3& v)
         {
            primitive->m_d.m_aRotAndTra[6] = v.x;
            primitive->m_d.m_aRotAndTra[7] = v.y;
            primitive->m_d.m_aRotAndTra[8] = v.z;
         },
         PropertyPane::Unit::Degree, 2, "Additional rotation applied on top of the base orientation");
      props.EndSection();
   }

   if (props.BeginSection("Physics"s))
   {
      props.Checkbox<Primitive>(
         m_part, "Has Hit Event"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_hitEvent; }, //
         [](Primitive* primitive, bool v) { primitive->m_d.m_hitEvent = v; }, "Fire a Hit event to the script when the ball hits this part");
      props.InputFloat<Primitive>(
         m_part, "Hit Threshold"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_threshold; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_threshold = v; }, PropertyPane::Unit::None, 3, "Minimum ball speed needed to trigger this part's hit event");
      props.MaterialCombo<Primitive>(
         m_part, "Physics Material"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_szPhysicsMaterial; }, //
         [](Primitive* primitive, const string& v) { primitive->m_d.m_szPhysicsMaterial = v; }, "Material preset used for this part's physics values");
      props.Checkbox<Primitive>(
         m_part, "Overwrite Material"s, //
         [](const Primitive* surf) { return surf->m_d.m_overwritePhysics; }, //
         [](Primitive* surf, bool v) { surf->m_d.m_overwritePhysics = v; }, "Use the fields below instead of the physics material's values");
      props.InputFloat<Primitive>(
         m_part, "Elasticity"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_elasticity; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_elasticity = v; }, PropertyPane::Unit::None, 3);
      props.InputFloat<Primitive>(
         m_part, "Elasticity Falloff"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_elasticityFalloff; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_elasticityFalloff = v; }, PropertyPane::Unit::None, 3, "Reduce elasticity for slower impacts (0 = constant elasticity)");
      props.InputFloat<Primitive>(
         m_part, "Friction"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_friction; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_friction = max(v, 0.f); }, PropertyPane::Unit::None, 3);
      props.InputFloat<Primitive>(
         m_part, "Scatter Angle"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_scatter; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_scatter = v; }, PropertyPane::Unit::None, 3, "Random angle added to the ball's bounce direction");
      props.Checkbox<Primitive>(
         m_part, "Collidable"s, //
         [](const Primitive* surf) { return surf->m_d.m_collidable; }, //
         [](Primitive* surf, bool v) { surf->m_d.m_collidable = v; });
      if (!m_part->m_d.m_wireRail) // A rail's collider is its ball channel
      {
      props.Checkbox<Primitive>(
         m_part, "Toy"s, //
         [](const Primitive* surf) { return surf->m_d.m_toy; }, //
         [](Primitive* surf, bool v) { surf->m_d.m_toy = v; }, "Never collidable");
      props.InputFloat<Primitive>(
         m_part, "Reduction Factor"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_collision_reductionFactor; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_collision_reductionFactor = v; }, PropertyPane::Unit::None, 3,
         "Simplify the physics collision mesh (higher = fewer collision points, faster but less accurate)");
      }
      props.EndSection();
   }

   if (!m_part->m_d.m_wireGen && props.BeginSection("Rubber Guide"s))
   {
      props.InputFloat<Primitive>(
         m_part, "Rubber Guide Radius"s, //
         [](const Primitive* primitive) { return primitive->m_d.m_rubberGuideRadius; }, //
         [](Primitive* primitive, float v) { primitive->m_d.m_rubberGuideRadius = v > 0.f ? v : 0.f; }, PropertyPane::Unit::VPLength, 2,
         "Radius of this primitive as a post of a guided rubber (0 = from the size of the mesh). Rubbers around it are refit");
      props.EndSection();
   }

   //props.TimerSection(m_part);
}

void PrimitiveUIPart::UpdateMeshUnitsUI()
{
   ImGui::SeparatorText("Units and axes");
   int units = m_meshUnitsMeters ? 1 : 0;
   ImGui::RadioButton("VPUnits (Up -Z, Visual Pinball default)", &units, 0);
   ImGui::Indent();
   ImGui::TextDisabled("Recommended Blender Import/Export Transformations:\nScale 0.00054, Forward -Y, Up -Z");
   ImGui::Unindent();
   ImGui::RadioButton("Meters (Up Y, Blender default)", &units, 1);
   ImGui::Indent();
   ImGui::TextDisabled("Matches Blender's default Import/Export Transformations:\nScale 1.0, Forward -Z, Up Y. Primitive scale stays 1.");
   ImGui::Unindent();
   m_meshUnitsMeters = units != 0;
}

void PrimitiveUIPart::ImportMesh()
{
   if (g_pplayer == nullptr || g_pplayer->m_playfieldWnd == nullptr)
      return;
   m_pendingMeshImport = std::make_shared<string>();
   const SDL_DialogFileFilter filters[] = { { "Wavefront obj file", "obj" } };
   SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* filelist, int filter)
      {
         auto* res = static_cast<std::shared_ptr<string>*>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingMeshImport), //
      g_pplayer->m_playfieldWnd->GetCore(), filters, 1, nullptr, false);
}

void PrimitiveUIPart::ExportMesh()
{
   if (g_pplayer == nullptr || g_pplayer->m_playfieldWnd == nullptr)
      return;
   m_pendingMeshExport = std::make_shared<string>();
   const SDL_DialogFileFilter filters[] = { { "Wavefront obj file", "obj" } };
   SDL_ShowSaveFileDialog(
      [](void* userdata, const char* const* filelist, int filter)
      {
         auto* res = static_cast<std::shared_ptr<string>*>(userdata);
         if (filelist != nullptr && filelist[0] != nullptr)
            **res = filelist[0];
         delete res;
      },
      new std::shared_ptr<string>(m_pendingMeshExport), //
      g_pplayer->m_playfieldWnd->GetCore(), filters, 1, nullptr);
}
}
