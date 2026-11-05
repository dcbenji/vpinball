// license:GPLv3+

#include "core/stdafx.h"
#include "MirrorParts.h"

#include "parts/bumper.h"
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
#include "parts/timer.h"
#include "parts/trigger.h"

#include "core/pinundo.h"

namespace VPX::EditorUI::MirrorParts
{

namespace
{
bool HasMeshToFlip(const IEditable *part);

// No negative zero (the mirrored value would be stored as -0 in the table file). The engine builds with -ffast-math (no signed zeros), under which the usual
// tests for zero (v == 0.f, or a select between 0.f and v, even one written on the bits) are dropped by the optimizer (checked with GCC and clang). So the
// sign bit is cleared by plain integer arithmetic: the mask keeps it only when the magnitude is not zero (magnitude + 0x7fffffff reaches bit 31 then)
float NoNegZero(const float v)
{
   const uint32_t bits = float_as_uint(v);
   const uint32_t magnitude = bits & 0x7fffffffu;
   return uint_as_float(bits & (0x7fffffffu | (((magnitude + 0x7fffffffu) >> 31) << 31)));
}
float Neg(const float v) { return NoNegZero(-v); }

// The coordinate along the axis
float Reflect(const float value, const float coord) { return NoNegZero(2.f * coord - value); }

void ReflectPoint(Vertex2D &v, const Axis axis, const float coord)
{
   if (axis == Axis::X)
      v.x = Reflect(v.x, coord);
   else
      v.y = Reflect(v.y, coord);
}

// A heading about z (degrees): the direction of a part's own axis, on the mirrored part. For a part symmetric about its own axis (all of them: the mirror image
// of a flipper is a flipper) the heading a is a -> -a across a vertical line (the axis of rotation stays, the sense reverses), and a -> 180 - a across a
// horizontal one (the same, then turned half a turn: the line's direction is perpendicular).
float Heading(const float a, const Axis axis) { return axis == Axis::X ? Neg(a) : NoNegZero(180.f - a); }

// The reflection F (x -> -x for Axis::X, y -> -y for Axis::Y) turns a rotation by an angle about the axis n into the rotation by the same angle about -F n
// (a rotation axis is an axial vector). So under F_x a rotation about x keeps its angle and the ones about y and z change sign; under F_y the ones about x
// and z change sign. For a part whose rotation is Rz * Ry * Rx (row vector convention, any order works) the Euler angles (rx, ry, rz) become (rx, -ry, -rz)
// under F_x and (-rx, ry, -rz) under F_y, as F (A B C) F = (F A F) (F B F) (F C F).
// Euler angles (x, y, z, degrees) of a 3D rotation
void MirrorEuler(float &rx, float &ry, float &rz, const Axis axis)
{
   if (axis == Axis::X)
   {
      ry = Neg(ry);
      rz = Neg(rz);
   }
   else
   {
      rx = Neg(rx);
      rz = Neg(rz);
   }
}

// The control points across the line. reverse: the closed outlines (walls, rubbers, lights, flashers, triggers, plastics) are listed the other way round, as a
// mirror turns their winding over (the slingshot flags move with the segments); open paths (ramps, wires) keep their direction
void MirrorCurve(DragPointCurve &curve, const Axis axis, const float coord, const bool reverse)
{
   for (const auto &point : curve.GetPoints())
   {
      if (axis == Axis::X)
         point->SetX(Reflect(point->GetX(), coord));
      else
         point->SetY(Reflect(point->GetY(), coord));
      point->m_roll = Neg(point->m_roll); // Wire Rail bank: positive raises the right side of the route
   }
   if (reverse)
      curve.ReverseOrder();
   curve.OnPointsModified();
}

template <class T> void MirrorAt(T &position, const Axis axis, const float coord)
{
   if (axis == Axis::X)
      position.x = Reflect(position.x, coord);
   else
      position.y = Reflect(position.y, coord);
}

void MirrorPrimitive(Primitive &prim, const Axis axis, const float coord)
{
   PrimitiveData &d = prim.m_d;
   if (d.m_plasticGen)
   {
      MirrorCurve(prim.m_plasticCurve, axis, coord, true);
      MirrorCurve(prim.m_plasticHoles, axis, coord, false); // Holes keep their order: a pill is two consecutive points, the first one smooth
      // The art stays on the part, mirrored with it (the atlas image is laid over the outline by the table to image map)
      if (axis == Axis::X)
         prim.PlasticMapTransform({ -1.f, 0.f, 2.f * coord, 0.f, 1.f, 0.f });
      else
         prim.PlasticMapTransform({ 1.f, 0.f, 0.f, 0.f, -1.f, 2.f * coord });
      prim.UpdatePlastic();
      return;
   }
   if (d.m_wireGen)
   {
      MirrorCurve(prim.m_wireCurve, axis, coord, false);
      if (d.m_wireRail)
      {
         // The route keeps its direction, so what was right of it is left of it now
         vector<WireRail::Wire> wires = prim.GetRailWires();
         for (WireRail::Wire &wire : wires)
            wire.x = Neg(wire.x);
         prim.SetRailWires(wires);
         d.m_railThirdRight = !d.m_railThirdRight;
         for (WireRail::Fixture &fixture : d.m_railFixtures) // Rings, rungs, cradles and stands keep their place along the route, mirrored across it
            WireRail::MirrorFixture(fixture);
      }
      prim.UpdateWire();
      return;
   }
   // A mesh primitive: its matrix is scale * (translate(a345) * rotate) * translate(position). Reflecting it gives the same with the object space offset
   // reflected, both Euler triples mirrored (one at [0..2], one at [6..8]) and the position reflected
   if (HasMeshToFlip(&prim))
   {
      // world' = mirror(world) is reached with the object space mesh mirrored along the axis (x for X, y for Y) and the transform mirrored as below
      // (M'(F v) = mirror(M(v))); the triangles are turned over so that the faces keep facing out. The scale stays positive
      Mesh &mesh = prim.m_mesh;
      const bool x = axis == Axis::X;
      for (Vertex3D_NoTex2 &v : mesh.m_vertices)
      {
         (x ? v.x : v.y) = Neg(x ? v.x : v.y);
         (x ? v.nx : v.ny) = Neg(x ? v.nx : v.ny);
      }
      for (Mesh::FrameData &frame : mesh.m_animationFrames)
         for (Mesh::VertData &v : frame.m_frameVerts)
         {
            (x ? v.x : v.y) = Neg(x ? v.x : v.y);
            (x ? v.nx : v.ny) = Neg(x ? v.nx : v.ny);
         }
      for (size_t i = 0; i + 2 < mesh.m_indices.size(); i += 3)
         std::swap(mesh.m_indices[i + 1], mesh.m_indices[i + 2]);
      prim.OnMeshVerticesChanged();
   }
   MirrorAt(d.m_vPosition, axis, coord);
   // A builtin primitive (a prism, its vertices come from the side count) is symmetric under object x -> -x for every side count, but under y -> -y only for
   // an even one. So for Y the object x flip stands in for the y one: the object space offset flips in x and the first rotation turns half a turn more
   // (F_x M F_y = S * T(a F_x) * Rz(180) * (F_y R F_y) * T(p F_y))
   const bool viaObjectX = axis == Axis::Y && !d.m_use3DMesh;
   if (axis == Axis::X || viaObjectX)
      d.m_aRotAndTra[3] = Neg(d.m_aRotAndTra[3]);
   else
      d.m_aRotAndTra[4] = Neg(d.m_aRotAndTra[4]);
   MirrorEuler(d.m_aRotAndTra[0], d.m_aRotAndTra[1], d.m_aRotAndTra[2], axis);
   MirrorEuler(d.m_aRotAndTra[6], d.m_aRotAndTra[7], d.m_aRotAndTra[8], axis);
   if (viaObjectX)
      d.m_aRotAndTra[2] = NoNegZero(d.m_aRotAndTra[2] + 180.f);
}
}

string Refusal(const IEditable *const part, const Axis axis)
{
   if (part->m_desktopBackdrop)
      return "backdrop element";
   switch (part->GetItemType())
   {
   case eItemSurface:
   case eItemFlipper:
   case eItemTimer:
   case eItemBumper:
   case eItemTrigger:
   case eItemLight:
   case eItemKicker:
   case eItemGate:
   case eItemSpinner:
   case eItemRamp:
   case eItemFlasher:
   case eItemRubber:
   case eItemHitTarget: return string();
   case eItemLightSeq: return "light sequencer (its centre belongs to its light collection, which mirror does not change)";
   case eItemPlunger: return axis == Axis::Y ? "plunger (it can only point at the player)" : string();
   case eItemPrimitive:
   {
      const Primitive *const prim = static_cast<const Primitive *>(part);
      if (prim->IsPlasticCompanion())
         return "part derived from its host";
      if (HasMeshToFlip(prim) && prim->m_d.m_objectSpaceNormalMap && !prim->m_d.m_szNormalMap.empty())
         return "object space normal map (its normals can not be mirrored)";
      return string();
   }
   case eItemDecal: return "decal (its image or text would read backwards)";
   case eItemTextbox:
   case eItemDispReel: return "display (its text would read backwards)";
   case eItemBall: return "ball";
   case eItemPartGroup: return "group (mirror its parts)";
   default: return "not a placed part";
   }
}

namespace
{
bool HasMeshToFlip(const IEditable *const part)
{
   if (part->GetItemType() != eItemPrimitive)
      return false;
   const Primitive *const prim = static_cast<const Primitive *>(part);
   return prim->m_d.m_use3DMesh && !prim->m_d.m_plasticGen && !prim->m_d.m_wireGen && prim->m_mesh.NumVertices() > 0;
}

struct MeshState
{
   Primitive *prim;
   vector<Vertex3D_NoTex2> vertices;
   vector<unsigned int> indices;
   vector<Mesh::FrameData> frames;
};
using MeshStates = vector<MeshState>;

std::shared_ptr<const MeshStates> Snapshot(const vector<Primitive *> &prims)
{
   auto states = std::make_shared<MeshStates>();
   for (Primitive *const prim : prims)
      states->push_back({ prim, prim->m_mesh.m_vertices, prim->m_mesh.m_indices, prim->m_mesh.m_animationFrames });
   return states;
}

void Restore(PinTable *const table, const MeshStates &states)
{
   for (const MeshState &state : states)
   {
      // A part deleted since is gone; a mesh that changed size is another mesh (reimported): leave it
      if (std::ranges::find(table->GetParts(), static_cast<IEditable *>(state.prim)) == table->GetParts().end() || state.prim->m_mesh.m_vertices.size() != state.vertices.size())
         continue;
      state.prim->m_mesh.m_vertices = state.vertices;
      state.prim->m_mesh.m_indices = state.indices;
      state.prim->m_mesh.m_animationFrames = state.frames;
      state.prim->OnMeshVerticesChanged();
      // The renderable and the colliders are refreshed by the part record of the same undo step
   }
}
}

void MarkMeshesForUndo(PinUndo &undo, PinTable *const table, const vector<IEditable *> &parts)
{
   vector<Primitive *> prims;
   for (IEditable *const part : parts)
      if (HasMeshToFlip(part))
         prims.push_back(static_cast<Primitive *>(part));
   if (prims.empty())
      return;
   undo.MarkForAction([table, before = Snapshot(prims)]() { Restore(table, *before); },
      [table, prims]() -> std::function<void()> { return [table, after = Snapshot(prims)]() { Restore(table, *after); }; });
}

float Position(const IEditable *const part, const Axis axis)
{
   const Vertex2D center = part->GetCenter();
   return axis == Axis::X ? center.x : center.y;
}

void Apply(IEditable *const part, const Axis axis, const float coord)
{
   switch (part->GetItemType())
   {
   case eItemSurface:
   {
      Surface &wall = *static_cast<Surface *>(part);
      MirrorCurve(wall.m_curve, axis, coord, true);
      break;
   }
   case eItemFlipper:
   {
      // The flipper is drawn and collides along its own y axis turned by the angle, and is symmetric about it: its mirror image is the same flipper at the
      // mirrored angle (a left flipper at 121 / 70 is a right flipper at -121 / -70). The sense of the stroke reverses with the sign, as it must.
      FlipperData &d = static_cast<Flipper *>(part)->m_d;
      MirrorAt(d.m_Center, axis, coord);
      d.m_StartAngle = Heading(d.m_StartAngle, axis);
      d.m_EndAngle = Heading(d.m_EndAngle, axis);
      break;
   }
   case eItemTimer: MirrorAt(static_cast<Timer *>(part)->m_d.m_v, axis, coord); break;
   case eItemPlunger: MirrorAt(static_cast<Plunger *>(part)->m_d.m_v, axis, coord); break;
   case eItemBumper:
   {
      BumperData &d = static_cast<Bumper *>(part)->m_d;
      MirrorAt(d.m_vCenter, axis, coord);
      d.m_orientation = Heading(d.m_orientation, axis);
      break;
   }
   case eItemTrigger:
   {
      Trigger &trigger = *static_cast<Trigger *>(part);
      MirrorAt(trigger.m_d.m_vCenter, axis, coord);
      trigger.m_d.m_rotation = Heading(trigger.m_d.m_rotation, axis);
      MirrorCurve(trigger.m_curve, axis, coord, true);
      break;
   }
   case eItemLight:
   {
      Light &light = *static_cast<Light *>(part);
      MirrorAt(light.m_d.m_vCenter, axis, coord);
      MirrorCurve(light.m_curve, axis, coord, true);
      break;
   }
   case eItemKicker:
   {
      KickerData &d = static_cast<Kicker *>(part)->m_d;
      MirrorAt(d.m_vCenter, axis, coord);
      d.m_orientation = Heading(d.m_orientation, axis);
      break;
   }
   case eItemGate:
   {
      // The gate's swing axis is its own x axis: the min and max angles are about it, and a mirror keeps their sense
      GateData &d = static_cast<Gate *>(part)->m_d;
      MirrorAt(d.m_vCenter, axis, coord);
      d.m_rotation = Heading(d.m_rotation, axis);
      break;
   }
   case eItemSpinner:
   {
      SpinnerData &d = static_cast<Spinner *>(part)->m_d;
      MirrorAt(d.m_vCenter, axis, coord);
      d.m_rotation = Heading(d.m_rotation, axis);
      break;
   }
   case eItemRamp:
   {
      // The path keeps its direction (the bottom and top heights and widths belong to its ends), so what was on its left is on its right now
      Ramp &ramp = *static_cast<Ramp *>(part);
      MirrorCurve(ramp.m_curve, axis, coord, false);
      std::swap(ramp.m_d.m_leftwallheight, ramp.m_d.m_rightwallheight);
      std::swap(ramp.m_d.m_leftwallheightvisible, ramp.m_d.m_rightwallheightvisible);
      if (ramp.m_d.m_type == RampType3WireLeft)
         ramp.m_d.m_type = RampType3WireRight;
      else if (ramp.m_d.m_type == RampType3WireRight)
         ramp.m_d.m_type = RampType3WireLeft;
      break;
   }
   case eItemFlasher:
   {
      Flasher &flasher = *static_cast<Flasher *>(part);
      MirrorCurve(flasher.m_curve, axis, coord, true);
      MirrorEuler(flasher.m_d.m_rotX, flasher.m_d.m_rotY, flasher.m_d.m_rotZ, axis); // About the centre of the outline, which is mirrored with it
      break;
   }
   case eItemRubber:
   {
      Rubber &rubber = *static_cast<Rubber *>(part);
      MirrorCurve(rubber.m_curve, axis, coord, true);
      MirrorEuler(rubber.m_d.m_rotX, rubber.m_d.m_rotY, rubber.m_d.m_rotZ, axis);
      break;
   }
   case eItemHitTarget:
   {
      HitTargetData &d = static_cast<HitTarget *>(part)->m_d;
      MirrorAt(d.m_vPosition, axis, coord);
      d.m_rotZ = Heading(d.m_rotZ, axis);
      break;
   }
   case eItemPrimitive: MirrorPrimitive(*static_cast<Primitive *>(part), axis, coord); break;
   default: break;
   }
}

}
