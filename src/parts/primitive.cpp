// license:GPLv3+

#include "core/stdafx.h" 
#include "primitive.h"

#include "core/VPApp.h"
#include "math/matrix.h"
#include "parts/light.h"
#include "utils/bulb.h"
#include "parts/Collection.h"
#include "physics/collideex.h"
#include "physics/PhysicsEngine.h"
#include "renderer/Renderer.h"
#include "renderer/RenderCommand.h"
#include "renderer/Shader.h"
#include "renderer/trace.h"
#include "utils/objloader.h"
#include "utils/color.h"

#include "forsyth.h"
#include "progmesh.h"
#include "miniz/miniz.h"
#include "math/wiresweep.h"
#include "math/PlasticMesh.h"


Primitive::~Primitive()
{
   assert(m_renderer == nullptr); // RenderRelease must be explicitly called before deleting this object
}

Primitive *Primitive::CopyForPlay() const
{
   STANDARD_EDITABLE_COPY_FOR_PLAY_IMPL(Primitive)
   dst->m_mesh = m_mesh;
   for (const auto &dpt : m_wireCurve.GetPoints())
   {
      dst->m_wireCurve.PushPoint(std::make_unique<DragPoint>(&dst->m_wireCurve, dpt->GetX(), dpt->GetY(), dpt->GetZ(), dpt->IsSmooth()));
      dst->m_wireCurve.GetPoints().back()->m_roll = dpt->m_roll; // Wire Rail bank
   }
   for (const auto &dpt : m_plasticCurve.GetPoints())
      dst->m_plasticCurve.PushPoint(std::make_unique<DragPoint>(&dst->m_plasticCurve, dpt->GetX(), dpt->GetY(), dpt->GetZ(), dpt->IsSmooth()));
   for (const auto &dpt : m_plasticHoles.GetPoints())
      dst->m_plasticHoles.PushPoint(std::make_unique<DragPoint>(&dst->m_plasticHoles, dpt->GetX(), dpt->GetY(), dpt->GetZ(), dpt->IsSmooth()));
   if (dst->m_d.m_wireRail)
      dst->RegenerateRail(); // The play copy has its channel before PhysicSetup (RenderSetup comes later); the table is not set yet, so the channel part is left alone
   return dst;
}

HRESULT Primitive::Init(const float x, const float y, const bool fromMouseClick, const bool forPlay)
{
   SetDefaults(fromMouseClick);
   m_d.m_vPosition.x = x;
   m_d.m_vPosition.y = y;
   CalculateBuiltinOriginal();
   return S_OK;
}

#define LinkProp(field, prop) field = fromMouseClick ? g_settingsService.GetAppSettings().GetDefaultPropsPrimitive_##prop() : Settings::GetDefaultPropsPrimitive_##prop##_Default()
void Primitive::SetDefaults(const bool fromMouseClick)
{
   m_d.m_szNormalMap.clear();
   m_d.m_szORMMap.clear();
   m_d.m_szEmissionMap.clear();
   LinkProp(m_d.m_SideColor, SideColor);
   LinkProp(m_d.m_visible, Visible);
   LinkProp(m_d.m_staticRendering, StaticRendering);
   LinkProp(m_d.m_drawTexturesInside, DrawTexturesInside);

   LinkProp(m_d.m_vPosition.z, Position_Z);

   LinkProp(m_d.m_vSize.x, Size_X);
   LinkProp(m_d.m_vSize.y, Size_Y);
   LinkProp(m_d.m_vSize.z, Size_Z);

   LinkProp(m_d.m_aRotAndTra[0], RotAndTra0);
   LinkProp(m_d.m_aRotAndTra[1], RotAndTra1);
   LinkProp(m_d.m_aRotAndTra[2], RotAndTra2);
   LinkProp(m_d.m_aRotAndTra[3], RotAndTra3);
   LinkProp(m_d.m_aRotAndTra[4], RotAndTra4);
   LinkProp(m_d.m_aRotAndTra[5], RotAndTra5);
   LinkProp(m_d.m_aRotAndTra[6], RotAndTra6);
   LinkProp(m_d.m_aRotAndTra[7], RotAndTra7);
   LinkProp(m_d.m_aRotAndTra[8], RotAndTra8);

   LinkProp(m_d.m_szImage, Image);
   LinkProp(m_d.m_szNormalMap, NormalMap);
   LinkProp(m_d.m_hitEvent, HitEvent);
   LinkProp(m_d.m_threshold, HitThreshold);

   LinkProp(m_d.m_addBlend, AddBlend);
   LinkProp(m_d.m_useDepthMask, DepthMask);
   LinkProp(m_d.m_alpha, Opacity);
   LinkProp(m_d.m_color, Color);

   LinkProp(m_d.m_edgeFactorUI, EdgeFactorUI);
   LinkProp(m_d.m_collision_reductionFactor, CollisionReductionFactor);

   LinkProp(m_d.m_collidable, Collidable);
   LinkProp(m_d.m_toy, IsToy);
   LinkProp(m_d.m_disableLightingTop, DisableLighting);
   LinkProp(m_d.m_disableLightingBelow, DisableLightingBelow);
   LinkProp(m_d.m_backfacesEnabled, BackfacesEnabled);
   LinkProp(m_d.m_displayTexture, DisplayTexture);
   LinkProp(m_d.m_objectSpaceNormalMap, ObjectSpaceNormalMap);

   LinkProp(m_d.m_reflectionEnabled, ReflectionEnabled);
   LinkProp(m_timerEnabled, TimerEnabled);
   LinkProp(m_timerInterval, TimerInterval);

   SetDefaultPhysics(fromMouseClick);

   m_d.m_use3DMesh = false;
   m_d.m_meshFileName.clear();
   const PrimitiveData wireDefaults {}; // Wire generator state back to defaults (a Load into an existing object must not keep stale state)
   m_d.m_wireGen = wireDefaults.m_wireGen;
   m_d.m_wireHeight = wireDefaults.m_wireHeight;
   m_d.m_wireThickness = wireDefaults.m_wireThickness;
   m_d.m_wireStandHeight = wireDefaults.m_wireStandHeight;
   m_d.m_wireBendRadius = wireDefaults.m_wireBendRadius;
   m_d.m_wireEnds = wireDefaults.m_wireEnds;
   m_d.m_wireRails = wireDefaults.m_wireRails;
   m_d.m_wireSpacing = wireDefaults.m_wireSpacing;
   m_d.m_wireSides = wireDefaults.m_wireSides;
   m_d.m_wireRail = wireDefaults.m_wireRail;
   m_d.m_railCount = wireDefaults.m_railCount;
   for (int i = 0; i < WireRail::kMaxWires; i++)
      m_d.m_railWires[i] = wireDefaults.m_railWires[i];
   m_d.m_railThirdRight = wireDefaults.m_railThirdRight;
   m_d.m_railCapBevel = wireDefaults.m_railCapBevel;
   m_d.m_railBallDiameter = wireDefaults.m_railBallDiameter;
   m_d.m_railCurvatureDetail = wireDefaults.m_railCurvatureDetail;
   m_d.m_railShowCollider = wireDefaults.m_railShowCollider;
   m_d.m_railFixtures.clear();
   m_d.m_rubberGuideRadius = wireDefaults.m_rubberGuideRadius;
   m_d.m_plasticGen = wireDefaults.m_plasticGen;
   m_d.m_plasticHeight = wireDefaults.m_plasticHeight;
   m_d.m_plasticThickness = wireDefaults.m_plasticThickness;
   m_d.m_plasticBevel = wireDefaults.m_plasticBevel;
   m_d.m_plasticBevelSegs = wireDefaults.m_plasticBevelSegs;
   m_d.m_plasticDetail = wireDefaults.m_plasticDetail;
   m_d.m_plasticHoleDia = wireDefaults.m_plasticHoleDia;
   m_d.m_plasticDpi = wireDefaults.m_plasticDpi;
   for (int i = 0; i < 6; i++)
      m_d.m_plasticUvm[i] = wireDefaults.m_plasticUvm[i];
   m_d.m_plasticHost = wireDefaults.m_plasticHost;
   m_d.m_plasticRole = wireDefaults.m_plasticRole;
   m_plasticEditHoles = false;
   m_wireCurve.ClearPoints();
   m_plasticCurve.ClearPoints();
   m_plasticHoles.ClearPoints();
   m_inPlayState = m_d.m_visible;
}

void Primitive::SetDefaultPhysics(const bool fromMouseClick)
{
   LinkProp(m_d.m_elasticity, Elasticity);
   LinkProp(m_d.m_elasticityFalloff, ElasticityFalloff);
   LinkProp(m_d.m_friction, Friction);
   LinkProp(m_d.m_scatter, Scatter);
}
#undef LinkProp

void Primitive::WriteRegDefaults()
{
#define LinkProp(field, prop) g_settingsService.GetAppSettings().SetDefaultPropsPrimitive_##prop(field, false)
   LinkProp(m_d.m_SideColor, SideColor);
   LinkProp(m_d.m_visible, Visible);
   LinkProp(m_d.m_staticRendering, StaticRendering);
   LinkProp(m_d.m_drawTexturesInside, DrawTexturesInside);

   LinkProp(m_d.m_vPosition.z, Position_Z);

   LinkProp(m_d.m_vSize.x, Size_X);
   LinkProp(m_d.m_vSize.y, Size_Y);
   LinkProp(m_d.m_vSize.z, Size_Z);

   LinkProp(m_d.m_aRotAndTra[0], RotAndTra0);
   LinkProp(m_d.m_aRotAndTra[1], RotAndTra1);
   LinkProp(m_d.m_aRotAndTra[2], RotAndTra2);
   LinkProp(m_d.m_aRotAndTra[3], RotAndTra3);
   LinkProp(m_d.m_aRotAndTra[4], RotAndTra4);
   LinkProp(m_d.m_aRotAndTra[5], RotAndTra5);
   LinkProp(m_d.m_aRotAndTra[6], RotAndTra6);
   LinkProp(m_d.m_aRotAndTra[7], RotAndTra7);
   LinkProp(m_d.m_aRotAndTra[8], RotAndTra8);

   LinkProp(m_d.m_szImage, Image);
   LinkProp(m_d.m_szNormalMap, NormalMap);
   LinkProp(m_d.m_hitEvent, HitEvent);
   LinkProp(m_d.m_threshold, HitThreshold);

   LinkProp(m_d.m_addBlend, AddBlend);
   LinkProp(m_d.m_useDepthMask, DepthMask);
   LinkProp(m_d.m_alpha, Opacity);
   LinkProp(m_d.m_color, Color);

   LinkProp(m_d.m_edgeFactorUI, EdgeFactorUI);
   LinkProp(m_d.m_collision_reductionFactor, CollisionReductionFactor);

   LinkProp(m_d.m_collidable, Collidable);
   LinkProp(m_d.m_toy, IsToy);
   LinkProp(m_d.m_disableLightingTop, DisableLighting);
   LinkProp(m_d.m_disableLightingBelow, DisableLightingBelow);
   LinkProp(m_d.m_backfacesEnabled, BackfacesEnabled);
   LinkProp(m_d.m_displayTexture, DisplayTexture);
   LinkProp(m_d.m_objectSpaceNormalMap, ObjectSpaceNormalMap);

   LinkProp(m_d.m_elasticity, Elasticity);
   LinkProp(m_d.m_elasticityFalloff, ElasticityFalloff);
   LinkProp(m_d.m_friction, Friction);
   LinkProp(m_d.m_scatter, Scatter);

   LinkProp(m_d.m_reflectionEnabled, ReflectionEnabled);
   LinkProp(m_timerEnabled, TimerEnabled);
   LinkProp(m_timerInterval, TimerInterval);
#undef LinkProp
}


#pragma region physic

bool Primitive::PhysicUpdate(class PhysicsEngine *physics, const bool isUI)
{ 
   // If the transform did not change, then nothing to do
   RecalculateMatrices();
   if (m_physicMatrix == m_fullMatrix)
      return true;

   return false; // Request full physics release/setup
}

void Primitive::PhysicSetup(PhysicsEngine* physics, const bool isUI)
{
   if (!isUI && GetPartGroup() != nullptr && GetPartGroup()->GetReferenceSpace() != PartGroupData::SpaceReference::SR_PLAYFIELD)
      return;

   m_useAsPlayfield = IsPlayfield();

   //

   if (IsRailChannel()) // Not picked, and the rail itself collides (below)
      return;

   if (!isUI && m_d.m_toy && !m_d.m_wireRail)
      return;

   RecalculateMatrices();
   TransformVertices(); //!! could also only do this for the optional reduced variant!
   m_physicMatrix = m_fullMatrix; // Cache transform to avoid rebuilding everything if not needed

   //

   if (!isUI && m_d.m_wireRail)
   {
      // The ball channel, not the tubes: its facets, the long edges (see WireRail::BuildChannel), the row vertices
      const auto toV = [](const WireRail::V3 &v) { return Vertex3Ds(v.x, v.y, v.z); };
      const vector<WireRail::V3> &cv = m_railChannel.verts;
      for (size_t i = 0; i + 2 < m_railChannel.tris.size(); i += 3)
      {
         Vertex3Ds rgv3D[3] = { toV(cv[m_railChannel.tris[i]]), toV(cv[m_railChannel.tris[i + 2]]), toV(cv[m_railChannel.tris[i + 1]]) };
         HitTriangle *const ht = new HitTriangle(this, rgv3D);
         if (ht->IsDegenerate())
            delete ht;
         else
            SetupHitObject(physics, ht, isUI);
      }
      for (const auto &e : m_railChannel.edges)
         SetupHitObject(physics, new HitLine3D(this, toV(cv[e.first]), toV(cv[e.second])), isUI);
      for (const WireRail::V3 &v : cv)
         SetupHitObject(physics, new HitPoint(this, v.x, v.y, v.z), isUI);
      return;
   }

   // TODO limit vertex count for UI
   const unsigned int reduced_vertices = max((unsigned int)pow((double)m_vertices.size(), saturate(1.f - m_d.m_collision_reductionFactor) * 0.25f + 0.75f), 420u); //!! 420 = magic

   // Ported at: VisualPinball.Engine/VPT/Primitive/PrimitiveHitGenerator.cs

   if (reduced_vertices < m_vertices.size())
   {
      vector<ProgMesh::float3> prog_vertices(m_vertices.size());
      for (size_t i = 0; i < m_vertices.size(); ++i) //!! opt. use original data directly!
      {
         prog_vertices[i].x = m_vertices[i].x;
         prog_vertices[i].y = m_vertices[i].y;
         prog_vertices[i].z = m_vertices[i].z;
      }
      vector<ProgMesh::tridata> prog_indices(m_mesh.NumIndices() / 3);
      {
         size_t i2 = 0;
         for (size_t i = 0; i < m_mesh.NumIndices(); i += 3)
         {
            ProgMesh::tridata t;
            t.v[0] = m_mesh.m_indices[i];
            t.v[1] = m_mesh.m_indices[i + 1];
            t.v[2] = m_mesh.m_indices[i + 2];
            if (t.v[0] != t.v[1] && t.v[1] != t.v[2] && t.v[2] != t.v[0])
               prog_indices[i2++] = t;
         }
         if (i2 < prog_indices.size())
            prog_indices.resize(i2);
      }
      vector<unsigned int> prog_map;
      vector<unsigned int> prog_perm;
      ProgMesh::ProgressiveMesh(prog_vertices, prog_indices, prog_map, prog_perm);
      ProgMesh::PermuteVertices(prog_perm, prog_vertices, prog_indices);
      prog_perm.clear();

      vector<ProgMesh::tridata> prog_new_indices;
      ProgMesh::ReMapIndices(reduced_vertices, prog_indices, prog_new_indices, prog_map);
      prog_indices.clear();
      prog_map.clear();

      //

      ankerl::unordered_dense::set<std::pair<unsigned, unsigned>> addedEdges;

      // add collision triangles and edges
      for (size_t i = 0; i < prog_new_indices.size(); ++i)
      {
         const unsigned int i0 = prog_new_indices[i].v[0];
         const unsigned int i1 = prog_new_indices[i].v[1];
         const unsigned int i2 = prog_new_indices[i].v[2];

         Vertex3Ds rgv3D[3];
         // NB: HitTriangle wants CCW vertices, but for rendering we have them in CW order
         rgv3D[0].x = prog_vertices[i0].x;
         rgv3D[0].y = prog_vertices[i0].y;
         rgv3D[0].z = prog_vertices[i0].z;
         rgv3D[1].x = prog_vertices[i2].x;
         rgv3D[1].y = prog_vertices[i2].y;
         rgv3D[1].z = prog_vertices[i2].z;
         rgv3D[2].x = prog_vertices[i1].x;
         rgv3D[2].y = prog_vertices[i1].y;
         rgv3D[2].z = prog_vertices[i1].z;
         HitTriangle* ht = new HitTriangle(this, rgv3D);
         if (ht->IsDegenerate())
            delete ht;
         else
            SetupHitObject(physics, ht, isUI);

         AddHitEdge(physics, addedEdges, i0, i1, rgv3D[0], rgv3D[2], isUI);
         AddHitEdge(physics, addedEdges, i1, i2, rgv3D[2], rgv3D[1], isUI);
         AddHitEdge(physics, addedEdges, i2, i0, rgv3D[1], rgv3D[0], isUI);
      }

      prog_new_indices.clear();

      // add collision vertices
      if (!isUI)
         for (size_t i = 0; i < prog_vertices.size(); ++i)
            SetupHitObject(physics, new HitPoint(this, prog_vertices[i].x, prog_vertices[i].y, prog_vertices[i].z), isUI);
   }
   else
   {
      ankerl::unordered_dense::set<std::pair<unsigned, unsigned>> addedEdges;

      // add collision triangles and edges
      for (size_t i = 0; i < m_mesh.NumIndices(); i += 3)
      {
         const unsigned int i0 = m_mesh.m_indices[i];
         const unsigned int i1 = m_mesh.m_indices[i + 1];
         const unsigned int i2 = m_mesh.m_indices[i + 2];

         Vertex3Ds rgv3D[3];
         // NB: HitTriangle wants CCW vertices, but for rendering we have them in CW order
         rgv3D[0] = m_vertices[i0];
         rgv3D[1] = m_vertices[i2];
         rgv3D[2] = m_vertices[i1];
         HitTriangle* ht = new HitTriangle(this, rgv3D);
         if (ht->IsDegenerate())
            delete ht;
         else
            SetupHitObject(physics, ht, isUI);

         AddHitEdge(physics, addedEdges, i0, i1, rgv3D[0], rgv3D[2], isUI);
         AddHitEdge(physics, addedEdges, i1, i2, rgv3D[2], rgv3D[1], isUI);
         AddHitEdge(physics, addedEdges, i2, i0, rgv3D[1], rgv3D[0], isUI);
      }

      // add collision vertices
      if (!isUI)
         for (size_t i = 0; i < m_mesh.NumVertices(); ++i)
            SetupHitObject(physics, new HitPoint(this, m_vertices[i]), isUI);
   }
}

void Primitive::PhysicRelease(PhysicsEngine* physics, const bool isUI)
{
   if (!isUI)
      m_vhoCollidable.clear();
}

// Ported at: VisualPinball.Engine/Math/EdgeSet.cs

void Primitive::AddHitEdge(PhysicsEngine* physics, ankerl::unordered_dense::set< std::pair<unsigned, unsigned> >& addedEdges, const unsigned i, const unsigned j, const Vertex3Ds &vi, const Vertex3Ds &vj, const bool isUI)
{
   // create pair uniquely identifying the edge (i,j)
   const std::pair<unsigned, unsigned> p(std::min(i, j), std::max(i, j));
   if (!isUI && addedEdges.insert(p).second) // edge not yet added?
      SetupHitObject(physics, new HitLine3D(this, vi, vj), isUI);
}

void Primitive::SetupHitObject(PhysicsEngine* physics, HitObject *obj, const bool isUI)
{
   const Material * const mat = m_ptable->GetMaterial(m_d.m_szPhysicsMaterial);
   if (m_useAsPlayfield)
   {
      obj->SetFriction(m_ptable->m_overridePhysics ? m_ptable->m_fOverrideContactFriction : m_ptable->m_friction);
      obj->m_elasticity = m_ptable->m_overridePhysics ? m_ptable->m_fOverrideElasticity : m_ptable->m_elasticity;
      obj->m_elasticityFalloff = m_ptable->m_overridePhysics ? m_ptable->m_fOverrideElasticityFalloff : m_ptable->m_elasticityFalloff;
      obj->m_scatter = ANGTORAD(m_ptable->m_overridePhysics ? m_ptable->m_fOverrideScatterAngle : m_ptable->m_scatter);
   }
   else if (mat != nullptr && !m_d.m_overwritePhysics)
   {
       obj->m_elasticity = mat->m_fElasticity;
       obj->m_elasticityFalloff = mat->m_fElasticityFalloff;
       obj->SetFriction(mat->m_fFriction);
       obj->m_scatter = ANGTORAD(mat->m_fScatterAngle);
   }
   else
   {
       obj->m_elasticity = m_d.m_elasticity;
       obj->m_elasticityFalloff = m_d.m_elasticityFalloff;
       obj->SetFriction(m_d.m_friction);
       obj->m_scatter = ANGTORAD(m_d.m_scatter);
   }

   obj->m_enabled = isUI ? true : m_d.m_collidable;
   obj->m_threshold = m_d.m_threshold;
   obj->m_ObjType = ePrimitive;
   obj->m_obj = (IFireEvents *)this;
   obj->m_fe = m_d.m_hitEvent;

   physics->AddCollider(obj, isUI);
   
   if (!isUI)
      m_vhoCollidable.push_back(obj); // remember hit components of primitive
}

#pragma endregion

//////////////////////////////
// Calculation
//////////////////////////////

// Ported at: VisualPinball.Engine/VPT/Primitive/PrimitiveMeshGenerator.cs

const Matrix3D& Primitive::RecalculateMatrices()
{
   // scale * rotation * translation

   // translation + rotation matrix
   Matrix3D RTmatrix = ((Matrix3D::MatrixTranslate(m_d.m_aRotAndTra[3], m_d.m_aRotAndTra[4], m_d.m_aRotAndTra[5])
                       * Matrix3D::MatrixRotateZ(ANGTORAD(m_d.m_aRotAndTra[2])))
                       * Matrix3D::MatrixRotateY(ANGTORAD(m_d.m_aRotAndTra[1])))
                       * Matrix3D::MatrixRotateX(ANGTORAD(m_d.m_aRotAndTra[0]));
   RTmatrix = ((RTmatrix
              * Matrix3D::MatrixRotateZ(ANGTORAD(m_d.m_aRotAndTra[8])))
              * Matrix3D::MatrixRotateY(ANGTORAD(m_d.m_aRotAndTra[7])))
              * Matrix3D::MatrixRotateX(ANGTORAD(m_d.m_aRotAndTra[6]));
   m_fullMatrix = (Matrix3D::MatrixScale(m_d.m_vSize.x, m_d.m_vSize.y, m_d.m_vSize.z)
                 * RTmatrix)
                 * Matrix3D::MatrixTranslate(m_d.m_vPosition.x, m_d.m_vPosition.y, m_d.m_vPosition.z);
   if (m_genDragMoved)
      m_fullMatrix = m_fullMatrix * m_genDragMatrix;
   return m_fullMatrix;
}

void Primitive::OnMeshVerticesChanged()
{
   m_mesh.m_validBounds = false; // UpdateBounds keeps cached bounds otherwise
   m_mesh.UpdateBounds();
   m_dome.measured = false;
   m_physicMatrix.Scale(0.f, 0.f, 0.f); // The collider (and the editor pick triangles) come from the mesh: rebuild them even if the transform did not change
   // Not m_vertexBufferRegenerate: a static mesh has an immutable vertex buffer, the renderable reinit rebuilds it
}

// recalculate vertices for editor display & physics setup
void Primitive::TransformVertices()
{
   m_vertices.resize(m_mesh.NumVertices());
   m_normals.resize(m_mesh.NumVertices());

   Matrix3D fullMatrix = m_fullMatrix;
   if (GetPartGroup() != nullptr && GetPartGroup()->GetReferenceSpace() != PartGroupData::SpaceReference::SR_PLAYFIELD)
      fullMatrix = fullMatrix * m_ptable->GetDefaultPlayfieldToCabMatrix();

   for (size_t i = 0; i < m_mesh.NumVertices(); i++)
   {
      m_vertices[i] = fullMatrix * m_mesh.m_vertices[i];
      Vertex3Ds n = fullMatrix.MultiplyVectorNoTranslateNormal(m_mesh.m_vertices[i]);
      n.Normalize();
      m_normals[i] = n.z;
   }
}

void Primitive::GetEditorTriangles(vector<Vertex2D> &triangles) const
{
   triangles.reserve(m_mesh.NumIndices());
   for (size_t i = 0; i < m_mesh.NumIndices(); i += 3)
   {
      const Vertex3Ds &A = m_vertices[m_mesh.m_indices[i]];
      const Vertex3Ds &B = m_vertices[m_mesh.m_indices[i + 1]];
      const Vertex3Ds &C = m_vertices[m_mesh.m_indices[i + 2]];
      triangles.emplace_back(C.x, C.y);
      triangles.emplace_back(B.x, B.y);
      triangles.emplace_back(A.x, A.y);
   }
}

void Primitive::GetEditorWireframe(vector<Vertex2D> &edges, vector<Vertex2D> &polyline) const
{
   if ((m_d.m_edgeFactorUI <= 0.0f) || (m_d.m_edgeFactorUI >= 1.0f) || !m_d.m_use3DMesh)
   {
      if (!m_d.m_use3DMesh || (m_d.m_edgeFactorUI >= 1.0f) || (m_mesh.NumVertices() <= 100)) // small mesh: draw all triangles
      {
         edges.reserve(m_mesh.NumIndices() * 2);
         for (size_t i = 0; i < m_mesh.NumIndices(); i += 3)
         {
            const Vertex3Ds &A = m_vertices[m_mesh.m_indices[i]];
            const Vertex3Ds &B = m_vertices[m_mesh.m_indices[i + 1]];
            const Vertex3Ds &C = m_vertices[m_mesh.m_indices[i + 2]];
            edges.emplace_back(A.x, A.y);
            edges.emplace_back(B.x, B.y);
            edges.emplace_back(B.x, B.y);
            edges.emplace_back(C.x, C.y);
            edges.emplace_back(C.x, C.y);
            edges.emplace_back(A.x, A.y);
         }
      }
      else if (m_mesh.NumIndices() > 0) // large mesh: draw a simplified mesh for performance reasons, does not approximate the shape well
      {
         polyline.reserve(m_mesh.NumIndices() / 3 + 1);
         const Vertex3Ds &A = m_vertices[m_mesh.m_indices[0]];
         polyline.emplace_back(A.x, A.y);
         for (size_t i = 0; i < m_mesh.NumIndices(); i += 3)
         {
            const Vertex3Ds &B = m_vertices[m_mesh.m_indices[i + 1]];
            polyline.emplace_back(B.x, B.y);
         }
      }
   }
   else
   {
      for (size_t i = 0; i < m_mesh.NumIndices(); i += 3)
      {
         const Vertex3Ds &A = m_vertices[m_mesh.m_indices[i]];
         const Vertex3Ds &B = m_vertices[m_mesh.m_indices[i + 1]];
         const Vertex3Ds &C = m_vertices[m_mesh.m_indices[i + 2]];
         const float An = m_normals[m_mesh.m_indices[i]];
         const float Bn = m_normals[m_mesh.m_indices[i + 1]];
         const float Cn = m_normals[m_mesh.m_indices[i + 2]];
         if (fabsf(An + Bn) < m_d.m_edgeFactorUI)
         {
            edges.emplace_back(A.x, A.y);
            edges.emplace_back(B.x, B.y);
         }
         if (fabsf(Bn + Cn) < m_d.m_edgeFactorUI)
         {
            edges.emplace_back(B.x, B.y);
            edges.emplace_back(C.x, C.y);
         }
         if (fabsf(Cn + An) < m_d.m_edgeFactorUI)
         {
            edges.emplace_back(C.x, C.y);
            edges.emplace_back(A.x, A.y);
         }
      }
   }
}

//////////////////////////////
// Rendering
//////////////////////////////

// VPX before 10.8 computed the viewer position based on a partial bounding volume that would not include primitives, so never fill legacy_bounds in here, only bounds
void Primitive::GetBoundingVertices(vector<Vertex3Ds> &bounds, vector<Vertex3Ds> *const legacy_bounds)
{
   if (!m_d.m_visible)
      return;

   RecalculateMatrices();
   if (m_d.m_use3DMesh)
   {
      m_mesh.UpdateBounds();
      if (m_mesh.m_minAABound.x != FLT_MAX)
      {
         const Vertex3Ds minBound = m_fullMatrix.MultiplyVectorNoPerspective(m_mesh.m_minAABound);
         const Vertex3Ds maxBound = m_fullMatrix.MultiplyVectorNoPerspective(m_mesh.m_maxAABound);
         bounds.emplace_back(minBound.x, minBound.y, minBound.z);
         bounds.emplace_back(minBound.x, minBound.y, maxBound.z);
         bounds.emplace_back(minBound.x, maxBound.y, minBound.z);
         bounds.emplace_back(minBound.x, maxBound.y, maxBound.z);
         bounds.emplace_back(maxBound.x, minBound.y, minBound.z);
         bounds.emplace_back(maxBound.x, minBound.y, maxBound.z);
         bounds.emplace_back(maxBound.x, maxBound.y, minBound.z);
         bounds.emplace_back(maxBound.x, maxBound.y, maxBound.z);
      }
   }
}

// Ported at: VisualPinball.Engine/VPT/Primitive/PrimitiveMeshGenerator.cs

void Primitive::CalculateBuiltinOriginal()
{
    if (m_d.m_use3DMesh)
        return;

   // this recalculates the Original Vertices -> should be only called, when sides are altered.
   const float outerRadius = -0.5f / (cosf((float)M_PI / (float)m_d.m_Sides));
   const float addAngle = (float)(2.0*M_PI) / (float)m_d.m_Sides;
   const float offsAngle = (float)M_PI / (float)m_d.m_Sides;
   float minX = FLT_MAX;
   float minY = FLT_MAX;
   float maxX = -FLT_MAX;
   float maxY = -FLT_MAX;

   m_mesh.m_vertices.resize(4 * m_d.m_Sides + 2);

   Vertex3D_NoTex2 *middle = m_mesh.m_vertices.data(); // middle point top
   middle->x = 0.0f;
   middle->y = 0.0f;
   middle->z = 0.5f;
   middle->nx = 0.0f;
   middle->ny = 0.0f;
   middle->nz = 1.0f;
   middle = &m_mesh.m_vertices[m_d.m_Sides + 1]; // middle point bottom
   middle->x = 0.0f;
   middle->y = 0.0f;
   middle->z = -0.5f;
   middle->nx = 0.0f;
   middle->ny = 0.0f;
   middle->nz = -1.0f;
   for (int i = 0; i < m_d.m_Sides; ++i)
   {
      // calculate top
      Vertex3D_NoTex2 * const topVert = &m_mesh.m_vertices[i + 1]; // top point at side
      const float currentAngle = addAngle*(float)i + offsAngle;
      topVert->x = sinf(currentAngle)*outerRadius;
      topVert->y = cosf(currentAngle)*outerRadius;
      topVert->z = 0.5f;
      topVert->nx = 0.0f;
      topVert->ny = 0.0f;
      topVert->nz = 1.0f;

      // calculate bottom
      Vertex3D_NoTex2 * const bottomVert = &m_mesh.m_vertices[i + 1 + m_d.m_Sides + 1]; // bottompoint at side
      bottomVert->x = topVert->x;
      bottomVert->y = topVert->y;
      bottomVert->z = -0.5f;
      bottomVert->nx = 0.0f;
      bottomVert->ny = 0.0f;
      bottomVert->nz = -1.0f;

      // calculate sides
      Vertex3D_NoTex2 &sideTopVert = m_mesh.m_vertices[m_d.m_Sides * 2 + 2 + i];
      sideTopVert = *topVert;
      sideTopVert.nx = sinf(currentAngle);
      sideTopVert.ny = cosf(currentAngle);
      sideTopVert.nz = 0.0f;
      Vertex3D_NoTex2 &sideBottomVert = m_mesh.m_vertices[m_d.m_Sides * 3 + 2 + i];
      sideBottomVert = *bottomVert;
      sideBottomVert.nx = sideTopVert.nx;
      sideBottomVert.ny = sideTopVert.ny;
      sideBottomVert.nz = 0.0f;

      // calculate bounds for X and Y
      if (topVert->x < minX)
         minX = topVert->x;
      if (topVert->x > maxX)
         maxX = topVert->x;
      if (topVert->y < minY)
         minY = topVert->y;
      if (topVert->y > maxY)
         maxY = topVert->y;
   }

   // these have to be replaced for image mapping
   middle = m_mesh.m_vertices.data(); // middle point top
   middle->tu = 0.25f;   // /4
   middle->tv = 0.25f;   // /4
   middle = &m_mesh.m_vertices[m_d.m_Sides + 1]; // middle point bottom
   middle->tu = (float)(0.25*3.); // /4*3
   middle->tv = 0.25f;   // /4
   const float invx = 0.5f / (maxX - minX);
   const float invy = 0.5f / (maxY - minY);
   const float invs = 1.0f / (float)m_d.m_Sides;
   for (int i = 0; i < m_d.m_Sides; i++)
   {
      Vertex3D_NoTex2 * const topVert = &m_mesh.m_vertices[i + 1]; // top point at side
      topVert->tu = (topVert->x - minX)*invx;
      topVert->tv = (topVert->y - minY)*invy;

      Vertex3D_NoTex2 * const bottomVert = &m_mesh.m_vertices[i + 1 + m_d.m_Sides + 1]; // bottom point at side
      bottomVert->tu = topVert->tu + 0.5f;
      bottomVert->tv = topVert->tv;

      Vertex3D_NoTex2 * const sideTopVert = &m_mesh.m_vertices[m_d.m_Sides * 2 + 2 + i];
      Vertex3D_NoTex2 * const sideBottomVert = &m_mesh.m_vertices[m_d.m_Sides * 3 + 2 + i];

      sideTopVert->tu = (float)i*invs;
      sideTopVert->tv = 0.5f;
      sideBottomVert->tu = sideTopVert->tu;
      sideBottomVert->tv = 1.0f;
   }

   // So how many indices are needed?
   // 3 per Triangle top - we have m_sides triangles -> 0, 1, 2, 0, 2, 3, 0, 3, 4, ...
   // 3 per Triangle bottom - we have m_sides triangles
   // 6 per Side at the side (two triangles form a rectangle) - we have m_sides sides
   // == 12 * m_sides
   // * 2 for both cullings (m_DrawTexturesInside == true)
   // == 24 * m_sides
   // this will also be the initial sorting, when depths, Vertices and Indices are recreated, because calculateRealTimeOriginal is called.

   // 2 restore indices
   //   check if anti culling is enabled:
   if (m_d.m_drawTexturesInside)
   {
      m_mesh.m_indices.resize(m_d.m_Sides * 24);
      // yes: draw everything twice
      // restore indices
      for (int i = 0; i < m_d.m_Sides; i++)
      {
         const int tmp = (i == m_d.m_Sides - 1) ? 1 : (i + 2); // wrapping around
         // top
         m_mesh.m_indices[i * 6] = 0;
         m_mesh.m_indices[i * 6 + 1] = i + 1;
         m_mesh.m_indices[i * 6 + 2] = tmp;
         m_mesh.m_indices[i * 6 + 3] = 0;
         m_mesh.m_indices[i * 6 + 4] = tmp;
         m_mesh.m_indices[i * 6 + 5] = i + 1;

         const int tmp2 = tmp + 1;
         // bottom
         m_mesh.m_indices[6 * (i + m_d.m_Sides)] = m_d.m_Sides + 1;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 1] = m_d.m_Sides + tmp2;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 2] = m_d.m_Sides + 2 + i;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 3] = m_d.m_Sides + 1;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 4] = m_d.m_Sides + 2 + i;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 5] = m_d.m_Sides + tmp2;

         // sides
         m_mesh.m_indices[12 * (i + m_d.m_Sides)] = m_d.m_Sides * 2 + tmp2;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 1] = m_d.m_Sides * 2 + 2 + i;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 2] = m_d.m_Sides * 3 + 2 + i;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 3] = m_d.m_Sides * 2 + tmp2;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 4] = m_d.m_Sides * 3 + 2 + i;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 5] = m_d.m_Sides * 3 + tmp2;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 6] = m_d.m_Sides * 2 + tmp2;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 7] = m_d.m_Sides * 3 + 2 + i;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 8] = m_d.m_Sides * 2 + 2 + i;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 9] = m_d.m_Sides * 2 + tmp2;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 10] = m_d.m_Sides * 3 + tmp2;
         m_mesh.m_indices[12 * (i + m_d.m_Sides) + 11] = m_d.m_Sides * 3 + 2 + i;
      }
   }
   else {
      // no: only out-facing polygons
      // restore indices
      m_mesh.m_indices.resize(m_d.m_Sides * 12);
      for (int i = 0; i < m_d.m_Sides; i++)
      {
         const int tmp = (i == m_d.m_Sides - 1) ? 1 : (i + 2); // wrapping around
         // top
         m_mesh.m_indices[i * 3] = 0;
         m_mesh.m_indices[i * 3 + 2] = i + 1;
         m_mesh.m_indices[i * 3 + 1] = tmp;

         //SetNormal(&m_mesh.m_vertices[0], &m_mesh.m_indices[i+3], 3); // see below

         const int tmp2 = tmp + 1;
         // bottom
         m_mesh.m_indices[3 * (i + m_d.m_Sides)] = m_d.m_Sides + 1;
         m_mesh.m_indices[3 * (i + m_d.m_Sides) + 1] = m_d.m_Sides + 2 + i;
         m_mesh.m_indices[3 * (i + m_d.m_Sides) + 2] = m_d.m_Sides + tmp2;

         //SetNormal(&m_mesh.m_vertices[0], &m_mesh.m_indices[3*(i+m_d.m_Sides)], 3); // see below

         // sides
         m_mesh.m_indices[6 * (i + m_d.m_Sides)] = m_d.m_Sides * 2 + tmp2;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 1] = m_d.m_Sides * 3 + 2 + i;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 2] = m_d.m_Sides * 2 + 2 + i;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 3] = m_d.m_Sides * 2 + tmp2;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 4] = m_d.m_Sides * 3 + tmp2;
         m_mesh.m_indices[6 * (i + m_d.m_Sides) + 5] = m_d.m_Sides * 3 + 2 + i;
      }
   }

   //SetNormal(&m_mesh.m_vertices[0], &m_mesh.m_indices[0], m_mesh.NumIndices()); // SetNormal only works for plane polygons
   //ComputeNormals(m_mesh.m_vertices, m_mesh.m_indices);
}

void Primitive::ExportMesh(ObjLoader& loader)
{
   if (m_d.m_visible)
   {
      const string& name = m_name;
      Vertex3D_NoTex2 *const buf = new Vertex3D_NoTex2[m_mesh.NumVertices()];
      RecalculateMatrices();
      for (size_t i = 0; i < m_mesh.NumVertices(); i++)
      {
         const Vertex3D_NoTex2 &v = m_mesh.m_vertices[i];
         Vertex3Ds vert = m_fullMatrix * Vertex3Ds{v.x, v.y, v.z};
         buf[i].x = vert.x;
         buf[i].y = vert.y;
         buf[i].z = vert.z;

         vert = Vertex3Ds(v.nx, v.ny, v.nz);
         vert = m_fullMatrix.MultiplyVectorNoTranslate(vert);
         buf[i].nx = vert.x;
         buf[i].ny = vert.y;
         buf[i].nz = vert.z;
         buf[i].tu = v.tu;
         buf[i].tv = v.tv;
      }
      loader.WriteObjectName(name);
      loader.WriteVertexInfo(buf, (unsigned int)m_mesh.NumVertices());
      const Material * const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
      loader.WriteMaterial(m_d.m_szMaterial, string(), mat);
      loader.UseTexture(m_d.m_szMaterial);
      loader.WriteFaceInfoLong(m_mesh.m_indices);
      loader.UpdateFaceOffset((unsigned int)m_mesh.NumVertices());
      delete[] buf;
   }
}

void Primitive::RenderSetup(Renderer *renderer)
{
   assert(m_renderer == nullptr);
   m_renderer = renderer;

   RegenerateWire(); // Wireform: every editor point edit reinitializes the renderable, so the mesh follows the points
   RegeneratePlastic(); // Plastic from Atlas host: same, and the companions' meshes

   // Check if we are part of a group, and if so if we are a child or the base
   m_groupdRendering = false;
   m_skipRendering = false;
   for (const auto collection : m_vCollection)
   {
      if (!collection->m_groupElements)
         continue;

      // The first primitive in the group is the base primitive
      // this element gets rendered by rendering all other group primitives
      // the rest of the group is marked as skipped rendering
      const Material * groupMaterial = nullptr;
      const Texture * groupTexel = nullptr;
      size_t overall_size = 0;
      bool partOfGroup = false;
      vector<Primitive *> prims;
      for (IEditable *const part : collection->GetParts())
      {
         if (part->GetItemType() != eItemPrimitive)
            continue;

         Primitive *const prim = (Primitive *)part;
         // only support dynamic mesh primitives for now
         if (!prim->m_d.m_use3DMesh || prim->m_d.m_staticRendering)
            continue;

         const Material *const mat = m_ptable->GetMaterial(prim->m_d.m_szMaterial);
         const Texture *const texel = m_ptable->GetImage(prim->m_d.m_szImage);
         if (groupMaterial == nullptr)
            groupMaterial = mat;
         if (groupTexel == nullptr)
            groupTexel = texel;
         if (mat == groupMaterial && texel == groupTexel)
         {
            prims.push_back(prim);
            partOfGroup |= prim == this;
            overall_size += prim->m_mesh.NumIndices();
         }
      }

      if (!partOfGroup || (prims.size() <= 1))
         continue;

      // We are part of a group, initialize as such
      if (prims[0] != this)
      {
         m_skipRendering = true;
      }
      else
      {
         m_groupdRendering = true;

         std::shared_ptr<IndexBuffer> indexBuffer
            = std::make_shared<IndexBuffer>(m_renderer->m_renderDevice, static_cast<unsigned int>(overall_size), false, overall_size < 65536 ? IndexBuffer::FMT_INDEX16 : IndexBuffer::FMT_INDEX32);
         void *indices;
         indexBuffer->Lock(indices);
         m_numGroupVertices = 0;
         m_numGroupIndices = 0;
         for (size_t i = 0; i < prims.size(); i++)
         {
            const Mesh &m = prims[i]->m_mesh;
            if (overall_size < 65536)
            {
               uint16_t * const __restrict idx16 = static_cast<uint16_t *>(indices);
               for (size_t k = 0; k < m.NumIndices(); k++)
                  idx16[m_numGroupIndices + k] = m_numGroupVertices + m.m_indices[k];
            }
            else
            {
               uint32_t * const __restrict idx32 = static_cast<uint32_t *>(indices);
               for (size_t k = 0; k < m.NumIndices(); k++)
                  idx32[m_numGroupIndices + k] = m_numGroupVertices + m.m_indices[k];
            }
            m_numGroupVertices += (int)m.NumVertices();
            m_numGroupIndices += (int)m.NumIndices();
         }
         indexBuffer->Unlock();

         std::shared_ptr<VertexBuffer> vertexBuffer = std::make_shared<VertexBuffer>(m_renderer->m_renderDevice, m_numGroupVertices);
         unsigned int ofs = 0;
         Vertex3D_NoTex2 *buf;
         vertexBuffer->Lock(buf);
         for (size_t i = 0; i < prims.size(); i++)
         {
            prims[i]->RecalculateMatrices();
            const Mesh &m = prims[i]->m_mesh;
            for (size_t t = 0; t < m.NumVertices(); t++)
            {
               Vertex3D_NoTex2 vt = m.m_vertices[t];
               prims[i]->m_fullMatrix.MultiplyVector(vt);

               const Vertex3Ds n = prims[i]->m_fullMatrix.MultiplyVectorNoTranslateNormal(vt);
               //!! Normalize?
               vt.nx = n.x;
               vt.ny = n.y;
               vt.nz = n.z;
               buf[ofs] = vt;
               ofs++;
            }
         }
         vertexBuffer->Unlock();
         m_meshBuffer = std::make_shared<MeshBuffer>(GetName() + ".RenderGroup"s, vertexBuffer, indexBuffer, true);
      }
      return;
   }

   // Normal mode
   m_lightmap = m_ptable->GetLight(m_d.m_szLightmap);

   m_currentFrame = -1.f;

   if (IsFlasherDome())
      MeasureFlasherDome(); // orders the index list (outer skin first) before it is uploaded
   if (m_mesh.NumVertices() > 0 && !m_mesh.m_indices.empty())
   {
      std::shared_ptr<VertexBuffer> vertexBuffer = std::make_shared<VertexBuffer>(m_renderer->m_renderDevice, (unsigned int)m_mesh.NumVertices(), nullptr, !(m_d.m_staticRendering || m_mesh.m_animationFrames.empty()));
      // Sorted parts get an updatable index buffer, all others keep the static one
      std::shared_ptr<IndexBuffer> indexBuffer = std::make_shared<IndexBuffer>(m_renderer->m_renderDevice, m_mesh.m_indices, WantsTriangleSort());
      m_meshBuffer = std::make_shared<MeshBuffer>(GetName(), vertexBuffer, indexBuffer, true);
      m_sortValid = false;
      m_sortCentroids.clear();

      // Compute and upload mesh to let a chance for renderdevice to share the buffers with other static objects
      RecalculateMatrices();
      // The PBR refraction options (curvature, anisotropy) read a smooth per-vertex curvature from the normal lengths
      const Material* const refrMat = m_ptable->GetMaterial(m_d.m_szMaterial);
      m_refrCurvatureEncoded = (m_d.m_refractionCurvature > 0.f || m_d.m_refractionAnisotropy > 0.f || m_d.m_refractionWave > 0.f || (refrMat && refrMat->m_fScatter > 0.f)) && !m_d.m_szRefractionProbe.empty() && m_mesh.m_animationFrames.empty();
      UploadMeshToVB(vertexBuffer);
   }
   else
   {
      m_meshBuffer = nullptr;
      m_skipRendering = true;
   }
   m_vertexBufferRegenerate = false;
}

// Uploads the mesh, with the per-vertex curvature for the PBR refraction options when m_refrCurvatureEncoded is set: each
// normal is stored with length 1 + k, k the surface curvature relative to a 10 VPU radius bend (vs_pbr passes length - 1 on
// as v_refrCurv, every vertex shader normalizes the normal). Per-face curvature comes from the face's 3 vertex normals and
// positions in table space (the largest singular value of the map from position to normal change over the face, the
// same estimate fs_pbr makes per triangle from the screen derivatives); a vertex takes the area-weighted mean over its faces.
// Area weighting keeps the term on narrow fillets (small faces) instead of spreading it across the big flat faces beside
// them, and interpolating it removes the per-triangle steps. Vertices split only for UVs (same position and normal) are
// welded first so a UV seam does not cut the average.
void Primitive::UploadMeshToVB(const std::shared_ptr<VertexBuffer>& vb)
{
   if (!m_refrCurvatureEncoded)
   {
      m_mesh.UploadToVB(vb, m_currentFrame);
      return;
   }
   const size_t nv = m_mesh.NumVertices();
   const size_t nt = m_mesh.NumIndices() / 3;
   vector<Vertex3Ds> pos(nv), nrm(nv);
   for (size_t i = 0; i < nv; i++)
   {
      const Vertex3D_NoTex2& v = m_mesh.m_vertices[i];
      pos[i] = m_fullMatrix * Vertex3Ds { v.x, v.y, v.z };
      nrm[i] = m_fullMatrix.MultiplyVectorNoTranslateNormal(v);
      nrm[i].Normalize();
   }
   // Weld vertices that share position (1e-3 VPU) and normal (~0.6 deg)
   vector<uint32_t> order(nv), weld(nv);
   const auto key = [&](size_t i) {
      return std::array<int64_t, 6> { (int64_t)std::lround(pos[i].x * 1000.f), (int64_t)std::lround(pos[i].y * 1000.f), (int64_t)std::lround(pos[i].z * 1000.f),
         (int64_t)std::lround(nrm[i].x * 100.f), (int64_t)std::lround(nrm[i].y * 100.f), (int64_t)std::lround(nrm[i].z * 100.f) };
   };
   vector<std::array<int64_t, 6>> keys(nv);
   for (size_t i = 0; i < nv; i++)
   {
      keys[i] = key(i);
      order[i] = (uint32_t)i;
   }
   std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
   for (size_t i = 0; i < nv; i++)
      weld[order[i]] = (i > 0 && keys[order[i]] == keys[order[i - 1]]) ? weld[order[i - 1]] : order[i];
   vector<double> kSum(nv, 0.), aSum(nv, 0.);
   for (size_t t = 0; t < nt; t++)
   {
      const unsigned int i0 = m_mesh.m_indices[t * 3], i1 = m_mesh.m_indices[t * 3 + 1], i2 = m_mesh.m_indices[t * 3 + 2];
      const Vertex3Ds e1 = pos[i1] - pos[i0], e2 = pos[i2] - pos[i0];
      Vertex3Ds fn = CrossProduct(e1, e2);
      const float area2 = fn.Length();
      if (area2 < 1e-6f)
         continue;
      fn = fn * (1.f / area2);
      Vertex3Ds t1 = e1;
      t1.Normalize();
      const Vertex3Ds t2 = CrossProduct(fn, t1);
      const Vertex3Ds d1 = nrm[i1] - nrm[i0], d2 = nrm[i2] - nrm[i0];
      // E: the edges in the face basis (columns), D: the normal changes along them; M = D.E^-1
      const float E00 = e1.Dot(t1), E01 = e2.Dot(t1), E10 = e1.Dot(t2), E11 = e2.Dot(t2);
      const float det = E00 * E11 - E01 * E10;
      if (fabsf(det) < 1e-9f)
         continue;
      const float i00 = E11 / det, i01 = -E01 / det, i10 = -E10 / det, i11 = E00 / det;
      const float D00 = d1.Dot(t1), D01 = d2.Dot(t1), D10 = d1.Dot(t2), D11 = d2.Dot(t2);
      const float a = D00 * i00 + D01 * i10, b = D00 * i01 + D01 * i11, c = D10 * i00 + D11 * i10, d = D10 * i01 + D11 * i11;
      const float s1 = a * a + b * b + c * c + d * d;
      const float s2 = sqrtf((a * a + b * b - c * c - d * d) * (a * a + b * b - c * c - d * d) + 4.f * (a * c + b * d) * (a * c + b * d));
      const float k = 10.f * sqrtf(max(0.5f * (s1 + s2), 0.f)); // max curvature relative to a 10 VPU radius
      const double area = 0.5 * area2;
      for (const unsigned int i : { i0, i1, i2 })
      {
         kSum[weld[i]] += area * k;
         aSum[weld[i]] += area;
      }
   }
   vector<Vertex3D_NoTex2> verts(m_mesh.m_vertices);
   for (size_t i = 0; i < nv; i++)
   {
      const size_t w = weld[i];
      const float k = aSum[w] > 0. ? (float)min(kSum[w] / aSum[w], 15.) : 0.f;
      Vertex3Ds n { verts[i].nx, verts[i].ny, verts[i].nz };
      n.Normalize();
      n = n * (1.f + k);
      verts[i].nx = n.x;
      verts[i].ny = n.y;
      verts[i].nz = n.z;
   }
   Vertex3D_NoTex2* buf;
   vb->Lock(buf);
   memcpy(buf, verts.data(), sizeof(Vertex3D_NoTex2) * nv);
   vb->Unlock();
}

// Cull Back Faces parts drawn blended (no depth mask, see-through material or refraction probe) are taken as closed
// double-walled transparent shells: their front faces are drawn back to front (SortTrianglesForCamera)
bool Primitive::WantsTriangleSort() const
{
   const Material *const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
   return m_d.m_cullBackfaces && !m_d.m_useDepthMask && !m_d.m_addBlend && !IsFlasherDome() && m_mesh.m_animationFrames.empty()
      && (mat->m_bOpacityActive || !m_d.m_szRefractionProbe.empty());
}

// Orders the index buffer far to near by triangle centroid distance from the camera (stereo: the eyes' midpoint, as both eyes
// are drawn by one instanced call). Distance does not depend on the camera rotation, so only a camera move of more than
// 1 VPU (or a part move) re-sorts, at most once every 4 frames: once per table load in play, throttled while the editor orbits.
// ponytail: runs inline on the render thread (35k triangles ~0.45 ms on an M-series core); a worker if parts get much bigger.
void Primitive::SortTrianglesForCamera()
{
   const ModelViewProj &mvp = m_renderer->GetMVP();
   const vec4 &c0 = mvp.GetCameraPos(0), &c1 = mvp.GetCameraPos(mvp.m_nEyes - 1);
   const Vertex3Ds cam(0.5f * (c0.x + c1.x), 0.5f * (c0.y + c1.y), 0.5f * (c0.z + c1.z));
   const bool partMoved = memcmp(m_sortMatrix.m, m_fullMatrix.m, sizeof(m_fullMatrix.m)) != 0;
   const unsigned int frame = g_pplayer->m_overall_frames;
   if (m_sortValid && (frame - m_sortFrame < 4 || (!partMoved && (cam - m_sortCameraPos).LengthSquared() < 1.f)))
      return;

   const auto start = std::chrono::high_resolution_clock::now();
   const size_t nt = m_mesh.NumIndices() / 3;
   if (partMoved || m_sortCentroids.size() != nt)
   {
      m_sortCentroids.resize(nt);
      for (size_t t = 0; t < nt; t++)
      {
         const Vertex3D_NoTex2 &a = m_mesh.m_vertices[m_mesh.m_indices[t * 3]], &b = m_mesh.m_vertices[m_mesh.m_indices[t * 3 + 1]], &c = m_mesh.m_vertices[m_mesh.m_indices[t * 3 + 2]];
         m_sortCentroids[t] = m_fullMatrix * Vertex3Ds((a.x + b.x + c.x) * (float)(1. / 3.), (a.y + b.y + c.y) * (float)(1. / 3.), (a.z + b.z + c.z) * (float)(1. / 3.));
      }
      m_sortMatrix = m_fullMatrix;
   }
   // Key: the squared distance's float bits (monotonic for positive floats), inverted for far first, over the triangle index
   vector<uint64_t> keys(nt);
   for (size_t t = 0; t < nt; t++)
   {
      const float d2 = (m_sortCentroids[t] - cam).LengthSquared();
      uint32_t bits;
      memcpy(&bits, &d2, sizeof(bits));
      keys[t] = (static_cast<uint64_t>(~bits) << 32) | static_cast<uint64_t>(t);
   }
   std::sort(keys.begin(), keys.end());
   const unsigned int ofs = m_meshBuffer->m_isVBOffsetApplied ? m_meshBuffer->m_vb->GetVertexOffset() : 0;
   uint32_t *idx;
   m_meshBuffer->m_ib->Lock(idx);
   for (size_t t = 0; t < nt; t++)
   {
      const unsigned int *const src = &m_mesh.m_indices[(keys[t] & 0xFFFFFFFFu) * 3];
      idx[t * 3] = src[0] + ofs;
      idx[t * 3 + 1] = src[1] + ofs;
      idx[t * 3 + 2] = src[2] + ofs;
   }
   m_meshBuffer->m_ib->Unlock();
   if (!m_sortValid)
   {
      PLOGI << "Primitive " << GetName() << ": " << nt << " triangles sorted back to front in "
            << std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start).count() << " ms";
   }
   m_sortCameraPos = cam;
   m_sortFrame = frame;
   m_sortValid = true;
}

void Primitive::RenderRelease()
{
   assert(m_renderer != nullptr);
   m_meshBuffer = nullptr;
   m_meshEdgeBuffer = nullptr;
   m_lightmap = nullptr;
   m_renderer = nullptr;
}

bool Primitive::IsFlasherDome() const
{
   static const bool enabled = std::getenv("VPX_T9") == nullptr || std::string(std::getenv("VPX_T9")) != "0";
   return enabled && !m_d.m_addBlend && lowerCase(m_d.m_szImage).rfind("dome_", 0) == 0;
}

// The dome is measured from its mesh: base centre and outer radius from the bounds (tabs stick out symmetrically along one
// axis only), the flute profile from the vertices facing the axis (the farthest are the groove roots, the nearest the ridge
// tips), the flute count as the smallest n for which the ridge tips' angles agree mod 2pi/n, and the cap underside
const Primitive::FlasherDomeShape &Primitive::MeasureFlasherDome()
{
   if (m_dome.measured)
      return m_dome;
   m_dome.measured = true;
   m_mesh.UpdateBounds();
   const Vertex3Ds lo = m_mesh.m_minAABound, hi = m_mesh.m_maxAABound;
   FlasherDomeShape d;
   d.measured = true;
   d.cx = 0.5f * (lo.x + hi.x);
   d.cy = 0.5f * (lo.y + hi.y);
   d.baseZ = lo.z;
   d.radius = 0.5f * min(hi.x - lo.x, hi.y - lo.y);
   d.height = hi.z - lo.z;
   d.rootR = 0.f;
   d.ridgeR = FLT_MAX;
   d.capZ = d.height;
   for (const Vertex3D_NoTex2 &v : m_mesh.m_vertices)
   {
      const float px = v.x - d.cx, py = v.y - d.cy, r = sqrtf(px * px + py * py);
      if (v.nz < -0.7f && v.z > lo.z + 0.5f * d.height)
         d.capZ = min(d.capZ, v.z - lo.z);
      if (v.z > lo.z + 0.9f * d.height)
         d.topR = max(d.topR, r);
      if (r < 0.5f * d.radius || r > 0.98f * d.radius || fabsf(v.nz) > 0.3f || v.nx * px + v.ny * py > -0.3f * r)
         continue;
      d.rootR = max(d.rootR, r);
      d.ridgeR = min(d.ridgeR, r);
   }
   if (d.rootR <= 0.f || d.ridgeR >= d.rootR)
   {
      PLOGW << "Flasher dome " << GetName() << ": no inner flutes found";
      return m_dome;
   }
   vector<float> tips;
   for (const Vertex3D_NoTex2 &v : m_mesh.m_vertices)
   {
      const float px = v.x - d.cx, py = v.y - d.cy, r = sqrtf(px * px + py * py);
      if (fabsf(v.nz) < 0.3f && v.nx * px + v.ny * py < -0.3f * r && r < d.ridgeR + 0.15f * (d.rootR - d.ridgeR))
         tips.push_back(atan2f(py, px));
   }
   for (int n = 6; n <= 64 && !tips.empty(); n++)
   {
      double cs = 0., sn = 0.;
      for (const float a : tips)
      {
         cs += cos(n * a);
         sn += sin(n * a);
      }
      if (sqrt(cs * cs + sn * sn) > 0.9 * (double)tips.size())
      {
         d.flutes = n;
         d.phase = (float)(atan2(sn, cs) / n + M_PI / n); // a groove root sits half a period from a ridge tip
         break;
      }
   }
   d.valid = d.flutes > 0 && d.capZ < d.height;
   if (d.valid)
   {
      // Outer skin triangles first: the dome layer reaches the inner flutes through the wall, so the body draws only its
      // outer skin (with the inner faces drawn too, their own white specular would read through a red wall)
      vector<unsigned int> outer, inner;
      const float midWall = 0.5f * (d.rootR + d.radius), capMid = d.capZ + 0.5f * (d.height - d.capZ);
      for (size_t t = 0; t + 2 < m_mesh.m_indices.size(); t += 3)
      {
         const Vertex3D_NoTex2 &a = m_mesh.m_vertices[m_mesh.m_indices[t]], &b = m_mesh.m_vertices[m_mesh.m_indices[t + 1]], &c = m_mesh.m_vertices[m_mesh.m_indices[t + 2]];
         const float px = (a.x + b.x + c.x) * (1.f / 3.f) - d.cx, py = (a.y + b.y + c.y) * (1.f / 3.f) - d.cy, pz = (a.z + b.z + c.z) * (1.f / 3.f) - d.baseZ;
         const float nz = a.nz + b.nz + c.nz;
         const bool isInner = (sqrtf(px * px + py * py) < midWall && pz < capMid) || nz < -1.5f;
         (isInner ? inner : outer).insert((isInner ? inner : outer).end(), { m_mesh.m_indices[t], m_mesh.m_indices[t + 1], m_mesh.m_indices[t + 2] });
      }
      d.outerIndices = (unsigned int)outer.size();
      outer.insert(outer.end(), inner.begin(), inner.end());
      m_mesh.m_indices = outer;
   }
   m_dome = d;
   PLOGI << "Flasher dome " << GetName() << ": " << (d.valid ? "" : "NOT measured, ") << d.flutes << " flutes, phase " << RADTOANG(d.phase) << " deg, R " << VPUTOMM(d.radius)
         << " mm, H " << VPUTOMM(d.height) << " mm, flutes " << VPUTOMM(d.ridgeR) << ".." << VPUTOMM(d.rootR) << " mm, cap underside " << VPUTOMM(d.capZ) << " mm";
   return m_dome;
}

static float FlasherDomeTune(const char *key, const float def)
{
   static const char *const tuning = std::getenv("VPX_T9_TUNE");
   float v;
   const char *p;
   if (tuning && (p = strstr(tuning, key)) && sscanf(p + strlen(key), "%f", &v) == 1)
      return v;
   return def;
}

bool Primitive::GetFlasherDomeBulb(Vertex3Ds &pos)
{
   const FlasherDomeShape &d = MeasureFlasherDome();
   if (!d.valid)
      return false;
   pos = RecalculateMatrices() * Vertex3Ds(d.cx, d.cy, d.baseZ + FlasherDomeTune("fil=", 0.47f) * d.height);
   return true;
}

bool Primitive::GetFlasherDomeCookie(vec3 &cookie)
{
   const FlasherDomeShape &d = MeasureFlasherDome();
   if (!d.valid)
      return false;
   const Vertex3Ds root = RecalculateMatrices().MultiplyVectorNoTranslate(Vertex3Ds(cosf(d.phase), sinf(d.phase), 0.f));
   cookie = vec3((float)d.flutes, atan2f(root.y, root.x), RADTOANG(atan2f(FlasherDomeTune("fil=", 0.47f) * d.height, d.rootR)));
   return true;
}

bool Primitive::IsTracedInsert() const { return m_d.m_addBlend && lowerCase(m_d.m_szImage).rfind("traced_", 0) == 0; }

#if defined(ENABLE_BGFX)
bool Primitive::GetShadowCacheBounds(Vertex3Ds &boundsMin, Vertex3Ds &boundsMax)
{
   if (m_mesh.NumIndices() == 0)
      return false;
   m_mesh.UpdateBounds();
   const Matrix3D &m = RecalculateMatrices();
   boundsMin = Vertex3Ds(FLT_MAX, FLT_MAX, FLT_MAX);
   boundsMax = Vertex3Ds(-FLT_MAX, -FLT_MAX, -FLT_MAX);
   for (int i = 0; i < 8; i++)
   {
      const Vertex3Ds p = m * Vertex3Ds(i & 1 ? m_mesh.m_maxAABound.x : m_mesh.m_minAABound.x, i & 2 ? m_mesh.m_maxAABound.y : m_mesh.m_minAABound.y, i & 4 ? m_mesh.m_maxAABound.z : m_mesh.m_minAABound.z);
      boundsMin = Vertex3Ds(min(boundsMin.x, p.x), min(boundsMin.y, p.y), min(boundsMin.z, p.z));
      boundsMax = Vertex3Ds(max(boundsMax.x, p.x), max(boundsMax.y, p.y), max(boundsMax.z, p.z));
   }
   return boundsMax.x > boundsMin.x && boundsMax.y > boundsMin.y;
}

bool Primitive::DrawShadowCacheMesh()
{
   if (m_renderer == nullptr || !m_d.m_visible || m_skipRendering || m_meshBuffer == nullptr || m_groupdRendering || m_vertexBufferRegenerate || m_mesh.NumIndices() == 0)
      return false;
   m_renderer->UpdateBasicShaderMatrix(RecalculateMatrices());
   m_renderer->m_renderDevice->DrawMesh(m_renderer->m_renderDevice->m_basicShader, false, m_d.m_vPosition, 0.f, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, (uint32_t)m_mesh.NumIndices());
   m_renderer->UpdateBasicShaderMatrix();
   return true;
}
#endif

void Primitive::Render(const unsigned int renderMask)
{
   assert(m_renderer != nullptr);
   assert(!m_desktopBackdrop);

   // PBR shadow map pass: draw only the caster geometry with the light's view-projection
   if (renderMask & Renderer::SHADOW_MAP)
   {
#if defined(ENABLE_BGFX)
      // A flasher dome never shadows its own lamp (the lamp sits inside it)
      if (m_lightmap != nullptr && m_lightmap == m_renderer->m_vsmShadowLight && IsFlasherDome())
         return;
#endif
      if (m_d.m_visible && !m_skipRendering && m_meshBuffer != nullptr && ((renderMask & Renderer::STATIC_ONLY) == 0 || m_d.m_staticRendering))
      {
         // Translucent casters (PBR materials with a transmission, or with opacity) tint the light instead of blocking it:
         // they draw in the transmittance pass only, with their base colour, image and alpha
         const Material *const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
         const bool translucent = m_renderer->IsTranslucentShadowCaster(mat);
         if (translucent != ((renderMask & Renderer::SHADOW_TRANS) != 0))
            return;
         if (translucent)
            m_renderer->BindTranslucentShadowCaster(mat, m_ptable->GetImage(m_d.m_szImage), m_d.m_alpha / 100.f);
         m_renderer->UpdateBasicShaderMatrix(RecalculateMatrices());
         m_renderer->m_renderDevice->DrawMesh(
            m_renderer->m_renderDevice->m_basicShader, false, m_d.m_vPosition, m_d.m_depthBias, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, (uint32_t)m_mesh.NumIndices());
      }
      return;
   }

   if (!m_d.m_visible || m_skipRendering)
      return;

   const bool isStaticOnly = renderMask & Renderer::STATIC_ONLY;
   const bool isDynamicOnly = renderMask & Renderer::DYNAMIC_ONLY;
   const bool isReflectionPass = renderMask & Renderer::REFLECTION_PASS;
   const bool isUIPass = renderMask & Renderer::UI_EDGES || renderMask & Renderer::UI_FILL;
   TRACE_FUNCTION();

   // Update playfield primitive settings from table settings
   SamplerFilter pinf = SamplerFilter::SF_UNDEFINED; // Use the default filtering of the sampler (trilinear or anisotropic, depending on user choice)
   if (m_useAsPlayfield)
   {
      m_d.m_szMaterial = m_ptable->m_playfieldMaterial;
      m_d.m_szImage = m_ptable->m_image;
      m_d.m_szReflectionProbe = RenderProbe::PLAYFIELD_REFLECTION_RENDERPROBE_NAME;
      m_d.m_reflectionStrength = m_ptable->m_playfieldReflectionStrength;
      pinf = SamplerFilter::SF_ANISOTROPIC;
   }

   // Do not render ourself inside our reflection probe (no self reflection)
   RenderProbe * const reflection_probe = (isUIPass || m_d.m_reflectionStrength <= 0) ? nullptr : m_ptable->GetRenderProbe(m_d.m_szReflectionProbe);
   if (reflection_probe != nullptr && reflection_probe->IsRendering())
      return;
   
   if (isReflectionPass && !m_d.m_reflectionEnabled)
      return;

   const Material *const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
   // PBR materials are always dynamic: their shading state (lights, AO, specular resolve) can not be frozen in the static accumulation
   const bool isPBR = m_renderer->m_renderDevice->m_pbrActive && mat->m_type == Material::MaterialType::PBR;

   if (isStaticOnly && (!m_d.m_staticRendering || isPBR))
      return;
   
   // Don't render in LS when state off (visibility driven by light sequencer)
   if (!isUIPass && m_lockedByLS && !m_inPlayState)
      return;
   
   // only render if we have dynamic reflections to render above the statically prerendered primitive
   RenderTarget *const reflections = reflection_probe ? reflection_probe->Render(renderMask) : nullptr;
   if (isDynamicOnly && m_d.m_staticRendering && !isPBR && (reflections == nullptr))
      return;

   // Update lightmap before checking anything that uses alpha
   float alpha = m_d.m_alpha;
   float linkState;
   const bool flasherDome = IsFlasherDome();
   const uint32_t bodyIndices = m_groupdRendering ? m_numGroupIndices : (flasherDome && m_dome.valid) ? m_dome.outerIndices : (uint32_t)m_mesh.NumIndices();
   if (!m_d.m_stateSrcLink.empty() && g_pplayer->m_resURIResolver.TryGetFloatState(m_d.m_stateSrcLink, linkState))
   {
      m_renderer->LampTestLinkLevel(this, m_lightmap, linkState); // Lamp Test of the view being drawn, if any
      alpha *= linkState;
   }
   else if (m_lightmap && !flasherDome) // a flasher dome's lamp drives its dome layer, not its body
   {
      if (m_lightmap->m_d.m_intensity != 0.f && m_lightmap->m_d.m_intensity_scale != 0.f)
         alpha *= m_lightmap->m_currentIntensity / (m_lightmap->m_d.m_intensity * m_lightmap->m_d.m_intensity_scale);
      else
         alpha = 0.f;
   }

   // Flasher dome parameters (fs_flasher_dome.sc) for one of its passes: 0 the layer over an opaque body, 1 the transmittance
   // pass under a see-through body, 2 the layer over a see-through body
   const auto flasherDomeParams = [this](vec4 *params, const float pass)
   {
      const FlasherDomeShape &d = m_dome;
      const Matrix3D &mat3 = m_fullMatrix;
      const Vertex3Ds base = mat3 * Vertex3Ds(d.cx, d.cy, d.baseZ);
      Vertex3Ds axis = mat3.MultiplyVectorNoTranslate(Vertex3Ds(0.f, 0.f, 1.f));
      const float scale = axis.Length(); // object to table units
      axis.Normalize();
      // The coil lies along the dome's object x; the flute phase is measured from it
      Vertex3Ds coil = mat3.MultiplyVectorNoTranslate(Vertex3Ds(1.f, 0.f, 0.f));
      coil -= axis * coil.Dot(axis);
      coil.Normalize();
      const float mm = VPUTOMM(1.f);
      float level = 0.f;
      if (m_lightmap && m_lightmap->m_d.m_intensity * m_lightmap->m_d.m_intensity_scale > 0.f)
         level = m_lightmap->m_currentIntensity / (m_lightmap->m_d.m_intensity * m_lightmap->m_d.m_intensity_scale);
      if (FlasherDomeTune("lvl=", -1.f) >= 0.f) // VPX_T9_TUNE lvl=: a fixed lamp level (look checks)
         level = FlasherDomeTune("lvl=", -1.f);
      // dome_boost: the level the dome layer sees (a flash the ROM drives below full reads as a full one); capped at 1.5 (the
      // peak's headroom). 1 = unchanged, bit-identical
      if (m_d.m_domeBoost != 1.f)
         level = min(level * max(m_d.m_domeBoost, 0.f), max(level, 1.5f));
      // The OFF glints are lit by the room: they follow the scene lighting level, relative to the table's default level
      const float roomLevel = m_renderer->m_sceneLighting.GetGlobalEmissionScale() / max(m_ptable->m_globalEmissionScale, 0.01f);
      // The table's light scattered in the plastic (GI glow): the irradiance at the bulb through a soft knee (a dome right next
      // to a GI bulb glows more, not flat), as the radiance the dome layer emits (it bands it by the flutes)
      const auto domeGI = [this](const float leak)
      {
         const float gain = FlasherDomeTune("gi=", 0.035f), knee = FlasherDomeTune("giknee=", 15.f);
         const float lum = 0.2126f * m_domeGI.x + 0.7152f * m_domeGI.y + 0.0722f * m_domeGI.z;
         const float k = gain / (1.f + lum / max(knee, 1e-6f));
         static int logged = 0;
         if (logged < 6 && lum > 0.f)
         {
            logged++;
            PLOGI << "Flasher dome " << GetName() << ": GI irradiance " << m_domeGI.x << ", " << m_domeGI.y << ", " << m_domeGI.z << " (lum " << lum << ")";
            for (int i = 0; i < 4; i++)
               PLOGI << "Flasher dome " << GetName() << ": glint light " << i << " at " << m_domeLights[2 * i].x << ", " << m_domeLights[2 * i].y << ", " << m_domeLights[2 * i].z << " r " << m_domeLights[2 * i].w
                     << " E " << m_domeLights[2 * i + 1].x << ", " << m_domeLights[2 * i + 1].y << ", " << m_domeLights[2 * i + 1].z;
         }
         return vec4(leak, m_domeGI.x * k, m_domeGI.y * k, m_domeGI.z * k);
      };
      // The plastic's absorption sigma (1/mm): 0 (black) = the built-in red acrylic preset, else from the tint (its colour at 2 mm)
      vec4 sigma(0.100f, 1.400f, 1.500f, 0.f);
      if (m_d.m_domeTint != 0)
      {
         const vec3 c = convertColor(m_d.m_domeTint);
         sigma = vec4(-logf(max(c.x, 0.002f)) / 2.f, -logf(max(c.y, 0.002f)) / 2.f, -logf(max(c.z, 0.002f)) / 2.f, 0.f);
      }
      // The dome's lamp colour, normalised so its largest channel is 1
      vec4 lampColor(1.f, 1.f, 1.f, 0.f);
      if (m_lightmap)
      {
         const vec3 c = convertColor(m_lightmap->m_d.m_color);
         const float m = max(c.x, max(c.y, c.z));
         if (m > 0.f)
            lampColor = vec4(c.x / m, c.y / m, c.z / m, 0.f);
      }
      const float ridgeR = m_d.m_domeFluteDepth == 1.f ? d.ridgeR : d.rootR - m_d.m_domeFluteDepth * (d.rootR - d.ridgeR);
      const vec4 p[16] = {
         vec4(base.x, base.y, base.z, mm),
         vec4(axis.x, axis.y, axis.z, d.radius * scale * mm),
         vec4(coil.x, coil.y, coil.z, d.height * scale * mm),
         vec4(FlasherDomeTune("fil=", 0.47f) * d.height * scale * mm, m_d.m_domeFlutes > 0 ? (float)m_d.m_domeFlutes : (float)d.flutes, d.phase, level),
         vec4(d.rootR * scale * mm, ridgeR * scale * mm, d.capZ * scale * mm, FlasherDomeTune("room=", 1.5f) * (pass > 0.5f ? roomLevel : 1.f)),
         vec4(FlasherDomeTune("glow=", 300.f), FlasherDomeTune("bars=", 30000.f), FlasherDomeTune("needle=", 1.f), FlasherDomeTune("white=", 6000.f)),
         vec4(FlasherDomeTune("halo=", 2500.f), FlasherDomeTune("spot=", 2000.f), FlasherDomeTune("flash=", 8.f), FlasherDomeTune("bulb=", 1.f)),
         vec4(FlasherDomeTune("gain=", 1.f / 170.f), FlasherDomeTune("debug=", 0.f), FlasherDomeTune("sigma=", 1.f), pass),
         domeGI(FlasherDomeTune("leak=", 0.6f)),
         vec4(m_domeGIDir.x, m_domeGIDir.y, m_domeGIDir.z, m_domeGIDir.w * FlasherDomeTune("gidir=", 1.f)),
         vec4(clamp(m_d.m_domeBody, 0.f, 1.f), max(m_d.m_domeScatter, 0.f), max(m_d.m_domeCap, 0.f), FlasherDomeTune("interior=", 0.8f)),
         vec4(sigma.x, sigma.y, sigma.z, FlasherDomeTune("lscat=", 1.f)),
         vec4(lampColor.x, lampColor.y, lampColor.z, FlasherDomeTune("rim=", 1.f)),
         vec4(d.topR * scale * mm, m_ptable->m_envEmissionScale * m_renderer->m_sceneLighting.GetGlobalEmissionScale(), max(m_d.m_domeGlint, 0.f), max(m_d.m_domePeak, 0.f)),
         vec4(max(m_d.m_domeDepth, 0.f), clamp(m_d.m_domeCooldown, 0.f, 1.f), max(m_d.m_domeOrb, 0.f), FlasherDomeTune("orb=", 20000.f)),
         vec4(m_ptable->GetImage(m_d.m_domeGlowMap) ? 1.f : 0.f, FlasherDomeTune("gmap=", 2.5f), clamp(m_d.m_domeGlintLights, 0.f, 1.f), max(m_d.m_domeFrost, 0.f)) };
      std::copy(p, p + 16, params);
      params[24] = vec4(0.f, 0.f, 0.f, 0.f); // dome_lens: the dome's centre on screen (set by the layer draw)
      // dome_glint_lights: the table's lights for the OFF glints, irradiance relative to the room light (the shader scales the
      // glints by it)
      const float lightGain = FlasherDomeTune("tlg=", 1.f) / max(p[4].w, 1e-3f);
      for (int i = 0; i < 4; i++)
      {
         params[16 + 2 * i] = m_domeLights[2 * i];
         const vec4 &E = m_domeLights[2 * i + 1];
         params[17 + 2 * i] = m_d.m_domeGlintLights > 0.f ? vec4(E.x * lightGain, E.y * lightGain, E.z * lightGain, 0.f) : vec4(0.f, 0.f, 0.f, 0.f);
      }
      params[17].w = FlasherDomeTune("frost=", 1.f); // dome_frost's gain
      params[23].w = FlasherDomeTune("foot=", 1.f); // the feet's lit glow gain (dome_body)
   };

   // Traced insert prototype: an additive card whose image is a multi-view traced bake ('TRACED_' image name prefix). It is
   // drawn with the lamp off too, since the room still glints in its facets
   const bool tracedInsert = IsTracedInsert();

   // don't render additive primitive if there is nothing to add
   if (!isUIPass && m_d.m_addBlend && !tracedInsert && (m_d.m_color == 0 || alpha == 0.f))
      return;

   if (m_groupdRendering)
      m_fullMatrix.SetIdentity();
   else
   {
      RecalculateMatrices();
      if (m_vertexBufferRegenerate)
      {
         // Vertex (keyframe/regen) motion is not captured by matWorldPrev and may cause visual glitches in the depth prepass motion vectors
         UploadMeshToVB(m_meshBuffer->m_vb);
         m_vertexBufferRegenerate = false;
      }
   }

   // set transform
   m_renderer->UpdateBasicShaderMatrix(m_fullMatrix, m_prevFullMatrixValid ? &m_prevFullMatrix : nullptr);
   m_prevFullMatrix = m_fullMatrix;
   m_prevFullMatrixValid = true;

   if (isUIPass)
   {
      if (m_meshBuffer == nullptr)
      {
         m_renderer->UpdateBasicShaderMatrix();
         return;
      }
      // FIXME use correct point for depth sorting
      RenderMeshDesc body;
      body.mesh = m_meshBuffer;
      body.indexCount = m_groupdRendering ? m_numGroupIndices : (uint32_t)m_mesh.NumIndices();
      body.sortCenter = Vertex3Ds(0.f, 0.f, 0.f);
      body.depthBias = 10000.f;
      body.uiEdgeMesh = &m_meshEdgeBuffer;
      body.edgeMode = UIEdgeMode::Outline;
      body.edgeTriIndices = m_mesh.m_indices.data();
      body.edgeTriCount = m_mesh.m_indices.size();
      body.edgeVertices = m_mesh.m_vertices.data();
      m_renderer->RenderMesh(renderMask, body, {});
      m_renderer->UpdateBasicShaderMatrix();
      return;
   }

   m_renderer->m_renderDevice->ResetRenderState();

   // Request probes before setting up state since this can trigger a renderprobe update which modifies the render state
   RenderProbe *const refraction_probe = m_ptable->GetRenderProbe(m_d.m_szRefractionProbe);
   RenderTarget *const probeRT = refraction_probe ? refraction_probe->Render(renderMask) : nullptr;
   // dome_lens: the dome layer's lensed slices need a copy of the scene behind the dome (a screen space transparency probe).
   // Any table works: the dome's own refraction probe if one is set (its body then skips it), else any such probe the table
   // already has (ramps, plastics...; the sharpest), else one the engine adds, once, when a dome first needs it
   RenderProbe *lensProbe = nullptr;
   RenderTarget *domeLensRT = nullptr;
   if (flasherDome && m_d.m_domeLens > 0.f && mat->m_bOpacityActive)
   {
      lensProbe = (refraction_probe && refraction_probe->GetType() == RenderProbe::SCREEN_SPACE_TRANSPARENCY) ? refraction_probe : nullptr;
      if (lensProbe == nullptr)
      {
         for (RenderProbe *const probe : m_ptable->m_vrenderprobe)
            if (probe->GetType() == RenderProbe::SCREEN_SPACE_TRANSPARENCY && (lensProbe == nullptr || probe->GetRoughness() < lensProbe->GetRoughness()))
               lensProbe = probe;
         if (lensProbe == nullptr)
         {
            lensProbe = m_ptable->NewRenderProbe();
            lensProbe->SetType(RenderProbe::SCREEN_SPACE_TRANSPARENCY);
            lensProbe->SetName("Dome Lens"s);
            lensProbe->RenderSetup(m_renderer);
            PLOGI << "Flasher dome " << GetName() << ": no screen space transparency probe on the table, added 'Dome Lens' for dome_lens";
         }
      }
      domeLensRT = lensProbe == refraction_probe ? probeRT : lensProbe->Render(renderMask);
   }
   RenderTarget *const refractions = (domeLensRT && lensProbe == refraction_probe) ? nullptr : probeRT;

   const bool depthMask = m_d.m_useDepthMask && !m_d.m_addBlend;

   // Not in the reflection pass: its camera is mirrored below the playfield
   if (!m_meshBuffer->m_ib->m_isStatic && !isReflectionPass && WantsTriangleSort())
      SortTrianglesForCamera();

   RenderState::RenderStateValue cullMode = m_renderer->m_renderDevice->GetRenderState().GetRenderState(RenderState::CULLMODE);
   RenderState::RenderStateValue reversedCullMode = cullMode == RenderState::CULL_CCW ? RenderState::CULL_CW : RenderState::CULL_CCW;
   m_renderer->m_renderDevice->SetRenderState(RenderState::CULLMODE, depthMask ? ((m_d.m_backfacesEnabled && mat->m_bOpacityActive) ? reversedCullMode : cullMode) : (m_d.m_cullBackfaces ? cullMode : RenderState::CULL_NONE));

   // Force disable light from below for objects marked as static since there is no light from below during pre-render pass (to get the same result in dynamic mode & static mode)
   m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::fDisableLighting_top_below, m_d.m_disableLightingTop, m_d.m_staticRendering ? 1.0f : m_d.m_disableLightingBelow, 0.f, 0.f);

   // Select textures, replacing backglass image by capture if it is available
   Texture * const nMap = m_ptable->GetImage(m_d.m_szNormalMap);
   Texture * const img = m_ptable->GetImage(m_d.m_szImage);
   const float pinAlphaTest = img != nullptr ? img->m_alphaTestValue : -1.f;
   m_renderer->m_renderDevice->m_basicShader->SetAlphaTestValue(pinAlphaTest);

   // accommodate models with UV coords outside of [0,1] by using Repeat address mode
   if (img && nMap)
   {
      m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_base_color, img, false, pinf, SamplerAddressMode::SA_REPEAT, SamplerAddressMode::SA_REPEAT);
      m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_base_normalmap, nMap, true);
      m_renderer->m_renderDevice->m_basicShader->SetBool(ShaderUniform::objectSpaceNormalMap, m_d.m_objectSpaceNormalMap);
      m_renderer->m_renderDevice->m_basicShader->SetMaterial(mat, !img->IsOpaque() || alpha != 100.f);
   }
   else if (img)
   {
      m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_base_color, img, false, pinf, SamplerAddressMode::SA_REPEAT, SamplerAddressMode::SA_REPEAT);
      m_renderer->m_renderDevice->m_basicShader->SetMaterial(mat, !img->IsOpaque() || alpha != 100.f);
   }
   else
   {
      m_renderer->m_renderDevice->m_basicShader->SetMaterial(mat, alpha != 100.f);
   }

   // Check if this primitive is used as a lightmap and should be convoluted with the light shadows
   const bool lightmap = m_lightmap != nullptr && m_lightmap->m_d.m_shadows == ShadowMode::RAYTRACED_BALL_SHADOWS;
   if (lightmap)
      m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::lightCenter_doShadow, m_lightmap->m_d.m_vCenter.x, m_lightmap->m_d.m_vCenter.y, m_lightmap->GetCurrentHeight(), 1.0f);

   bool is_reflection_only_pass = false;
   if (m_d.m_addBlend)
   {
      // Additive blending is a special unlit mode with depth mask disabled
      m_renderer->m_renderDevice->EnableAlphaBlend(true);
      m_renderer->m_renderDevice->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
      const vec4 color = convertColor(m_d.m_color, alpha * (float)(1.0 / 100.0));
      m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::staticColor_Alpha, color.x * color.w, color.y * color.w, color.z * color.w, color.w);
      if (img && tracedInsert)
      {
         // Defaults: the look of the AFM 'round 1.19in starburst' type (l63). A part's bake type overrides them with its
         // traced_params, and VPX_TRACED overrides both for tuning without a rebuild ("gain=4 spread=0.67 glint=1.5 debug=1")
         const float hash = sinf(m_d.m_vPosition.x * 127.1f + m_d.m_vPosition.y * 311.7f) * 43758.5453f;
         const float seed = hash - floorf(hash);
         float turn = -78.201f, turnJitter = 14.641f, fadeTint = 1.f, depthK = 1.f;
         vec4 params[14] = {
            vec4(15.f, 45.f, 60.f, 70.f), // traced elevations
            vec4(4.f, 2.5f, 3.5f, 1.5f), // depths (mm): cover, facets, glow parallax, rim fade
            vec4(m_d.m_tracedGain, 0.67f, 1.6f, 0.f), // glow gain, glow spread, IOR, alignment view
            vec4(1.5f, 76.648f, 50.938f, 1.62f), // glints: strength, sharpness, facet tilt, fringe
            vec4(5.f, 0.f, 3.f, 6.f), // glints: keys, turn (below), waviness (deg), wavelength (mm)
            vec4(seed, nMap ? 1.f : 0.f, 0.5396875f, 0.f), // seed, has facets, mm per table unit, frost
            vec4(0.3f, 0.29f, 0.27f, 0.f), // room light glinting in the facets, lamp-off look gain
            convertColor(m_d.m_color, 0.f), // the plastic's colour
            vec4(0.f, m_d.m_tracedHaloSize, m_d.m_tracedHaloStrength, 0.f), // spill pass, size, strength
            vec4(0.f, 0.f, 0.f, 0.f), // table position to ink mask UV (scale, offset), 0 = no mask
            vec4(0.f, 0.f, 0.f, 0.f), // print map texel at the spill mip (u, v), white leak through the plastic, room glint tint
            vec4(0.f, 1.f, 0.f, 0.f), // print map tint on the glow, how much the plastic's colour tints the lamp-off look
            vec4(m_d.m_bulbDepth, m_d.m_bulbScale, m_d.m_bulbBrightness, 0.25f), // parallax hot spot: bulb depth (mm, 0 = off), size, brightness; how much the glints turn with the eye
            vec4(m_d.m_bulbStretchX, m_d.m_bulbStretchY, m_d.m_bulbOffsetX, m_d.m_bulbOffsetY) }; // hot spot stretch (u, v), offset (u, v, mm)
         // The lamp-off look atlas, in the Emission Map slot
         Texture *const envMap = m_ptable->GetImage(m_d.m_szEmissionMap);
         if (envMap)
         {
            m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_traced_env, envMap, true, SamplerFilter::SF_TRILINEAR, SamplerAddressMode::SA_CLAMP, SamplerAddressMode::SA_CLAMP);
            params[6].w = 1.f;
         }
         // The table's print map, in the ORM Map slot, over the whole table rectangle: alpha = the print covering the insert (0)
         // or not (1), so the lamp only shows through the unprinted window; rgb = the playfield's art, which the spill lights
         Texture *const inkMap = m_ptable->GetImage(m_d.m_szORMMap);
         float spillMm = 3.f;
         if (inkMap)
         {
            m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_pbr_orm, inkMap, false, SamplerFilter::SF_TRILINEAR, SamplerAddressMode::SA_CLAMP, SamplerAddressMode::SA_CLAMP);
            const float sx = 1.f / (m_ptable->m_right - m_ptable->m_left), sy = 1.f / (m_ptable->m_bottom - m_ptable->m_top);
            params[9] = vec4(sx, sy, -m_ptable->m_left * sx, -m_ptable->m_top * sy);
         }
         const auto tune = [&](const char *const tuning)
         {
            float v, e[4];
            const char *p;
            if ((p = strstr(tuning, "elev=")) && sscanf(p + 5, "%f,%f,%f,%f", &e[0], &e[1], &e[2], &e[3]) == 4) params[0] = vec4(e[0], e[1], e[2], e[3]);
            if ((p = strstr(tuning, "gain=")) && sscanf(p + 5, "%f", &v) == 1) params[2].x = v;
            if ((p = strstr(tuning, "spread=")) && sscanf(p + 7, "%f", &v) == 1) params[2].y = v;
            if ((p = strstr(tuning, "ior=")) && sscanf(p + 4, "%f", &v) == 1) params[2].z = v;
            if ((p = strstr(tuning, "debug=")) && sscanf(p + 6, "%f", &v) == 1) params[2].w = v;
            if ((p = strstr(tuning, "glint=")) && sscanf(p + 6, "%f", &v) == 1) params[3].x = v;
            if ((p = strstr(tuning, "sharp=")) && sscanf(p + 6, "%f", &v) == 1) params[3].y = v;
            if ((p = strstr(tuning, "tilt=")) && sscanf(p + 5, "%f", &v) == 1) params[3].z = v;
            if ((p = strstr(tuning, "fringe=")) && sscanf(p + 7, "%f", &v) == 1) params[3].w = v;
            if ((p = strstr(tuning, "keys=")) && sscanf(p + 5, "%f", &v) == 1) params[4].x = v;
            for (p = strstr(tuning, "turn="); p; p = strstr(p + 1, "turn=")) // Not the end of "glintturn="
               if ((p == tuning || p[-1] == ' ') && sscanf(p + 5, "%f", &v) == 1)
               {
                  turn = v;
                  break;
               }
            if ((p = strstr(tuning, "glintturn=")) && sscanf(p + 10, "%f", &v) == 1) params[12].w = saturate(v);
            if ((p = strstr(tuning, "turnjit=")) && sscanf(p + 8, "%f", &v) == 1) turnJitter = v;
            if ((p = strstr(tuning, "wavy=")) && sscanf(p + 5, "%f", &v) == 1) params[4].z = v;
            if ((p = strstr(tuning, "wavelen=")) && sscanf(p + 8, "%f", &v) == 1) params[4].w = v;
            if ((p = strstr(tuning, "frost=")) && sscanf(p + 6, "%f", &v) == 1) params[5].w = v;
            if ((p = strstr(tuning, "room=")) && sscanf(p + 5, "%f", &v) == 1) params[6] = vec4(v, v * 0.96f, v * 0.9f, params[6].w);
            if (envMap && (p = strstr(tuning, "env=")) && sscanf(p + 4, "%f", &v) == 1) params[6].w = v;
            if ((p = strstr(tuning, "facetblur=")) && sscanf(p + 10, "%f", &v) == 1) params[7].w = v;
            if ((p = strstr(tuning, "halo=")) && sscanf(p + 5, "%f", &v) == 1) params[8].y = v;
            if ((p = strstr(tuning, "halostr=")) && sscanf(p + 8, "%f", &v) == 1) params[8].z = v;
            if ((p = strstr(tuning, "fadetint=")) && sscanf(p + 9, "%f", &v) == 1) fadeTint = v;
            if ((p = strstr(tuning, "spillmm=")) && sscanf(p + 8, "%f", &v) == 1) spillMm = v;
            if ((p = strstr(tuning, "depthk=")) && sscanf(p + 7, "%f", &v) == 1) depthK = v;
            if ((p = strstr(tuning, "leak=")) && sscanf(p + 5, "%f", &v) == 1) params[10].z = v;
            if ((p = strstr(tuning, "roomtint=")) && sscanf(p + 9, "%f", &v) == 1) params[10].w = v;
            if ((p = strstr(tuning, "inktint=")) && sscanf(p + 8, "%f", &v) == 1) params[11].x = v;
            if ((p = strstr(tuning, "plastictint=")) && sscanf(p + 12, "%f", &v) == 1) params[11].y = saturate(v);
            if ((p = strstr(tuning, "bulbdepth=")) && sscanf(p + 10, "%f", &v) == 1) params[12].x = v;
            if ((p = strstr(tuning, "bulbscale=")) && sscanf(p + 10, "%f", &v) == 1) params[12].y = v;
            if ((p = strstr(tuning, "bulbbright=")) && sscanf(p + 11, "%f", &v) == 1) params[12].z = v;
            if ((p = strstr(tuning, "bulbstretch=")) && sscanf(p + 12, "%f,%f", &e[0], &e[1]) == 2) { params[13].x = e[0]; params[13].y = e[1]; }
            if ((p = strstr(tuning, "bulboffset=")) && sscanf(p + 11, "%f,%f", &e[0], &e[1]) == 2) { params[13].z = e[0]; params[13].w = e[1]; }
         };
         if (!m_d.m_tracedParams.empty())
            tune(m_d.m_tracedParams.c_str());
         static const char *const tuning = std::getenv("VPX_TRACED");
         if (tuning)
            tune(tuning);
         params[4].y = turn + (seed * 2.f - 1.f) * turnJitter;
         // The room's glints and the lamp-off look are lit by the room: they follow the scene lighting level, relative to the
         // table's default level (which they are tuned at)
         const float roomLevel = m_renderer->m_sceneLighting.GetGlobalEmissionScale() / max(m_ptable->m_globalEmissionScale, 0.01f);
         params[6] = vec4(params[6].x * roomLevel, params[6].y * roomLevel, params[6].z * roomLevel, params[6].w * roomLevel);
         // The spill's reach, as the print map mip whose texel is that wide (-1 = no print map: the old round falloff)
         params[8].w = (inkMap && inkMap->m_width > 0) ? log2f(max(spillMm / ((m_ptable->m_right - m_ptable->m_left) * params[5].z / (float)inkMap->m_width), 1.f)) : -1.f;
         if (params[8].w >= 0.f)
            params[10] = vec4(exp2f(params[8].w) / (float)inkMap->m_width, exp2f(params[8].w) / (float)max(inkMap->m_height.load(), 1u), params[10].z, params[10].w);
         // A bake scaled onto a smaller (or bigger) insert: its cover, facet and glow depths scale with it (depthk = the scale)
         params[1].x *= depthK;
         params[1].y *= depthK;
         params[1].z *= depthK;
         m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_base_color, img, true, SamplerFilter::SF_TRILINEAR, SamplerAddressMode::SA_CLAMP, SamplerAddressMode::SA_CLAMP);
         if (nMap)
            m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_base_normalmap, nMap, true, SamplerFilter::SF_TRILINEAR, SamplerAddressMode::SA_CLAMP, SamplerAddressMode::SA_CLAMP);
         // An incandescent lamp glows red-orange as its filament heats and cools, as the light's own draw does. A lamp with no
         // fader is faded by its driver (PinMAME's physical bulb model): its filament temperature follows from the card's level
         // (1 = the lamp fully on), as Light::get_FilamentTemperature does. fadetint=0 turns that off.
         const bool driverFaded = m_lightmap && m_lightmap->m_d.m_fader == FADER_NONE && fadeTint > 0.f && alpha > 0.f;
         if (m_lightmap && (m_lightmap->m_d.m_fader == FADER_INCANDESCENT || driverFaded))
         {
            float tint2700[3], tint[3];
            bulb_filament_temperature_to_tint(2700.f, tint2700);
            bulb_filament_temperature_to_tint(driverFaded ? (float)bulb_emission_to_filament_temperature(min(alpha * 0.01f, 2.f))
                                                          : (float)m_lightmap->m_currentFilamentTemperature, tint);
            m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::staticColor_Alpha, color.x * color.w * tint[0] / tint2700[0],
               color.y * color.w * tint[1] / tint2700[1], color.z * color.w * tint[2] / tint2700[2], color.w);
         }
         m_renderer->m_renderDevice->m_basicShader->SetFloat4v(ShaderUniform::tracedParams, params, 14);
         m_renderer->m_renderDevice->m_basicShader->SetTechnique(ShaderTechnique::insert_traced);
         // The lit insert's soft spill onto the art and coat around it: the same card, scaled up about its centre, drawn
         // first by the same technique in its spill mode. Not in reflections, and not with the lamp off.
         if (params[8].y > 0.f && params[8].z > 0.f && alpha > 0.f && !isReflectionPass)
         {
            params[8].x = 1.f;
            m_renderer->m_renderDevice->m_basicShader->SetFloat4v(ShaderUniform::tracedParams, params, 14);
            m_renderer->UpdateBasicShaderMatrix(Matrix3D::MatrixScale(params[8].y, params[8].y, 1.f) * m_fullMatrix);
            m_renderer->m_renderDevice->DrawMesh(m_renderer->m_renderDevice->m_basicShader, true, m_d.m_vPosition, m_d.m_depthBias + 0.1f, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, m_groupdRendering ? m_numGroupIndices : (uint32_t)m_mesh.NumIndices());
            m_renderer->UpdateBasicShaderMatrix(m_fullMatrix, m_prevFullMatrixValid ? &m_prevFullMatrix : nullptr);
            params[8].x = 0.f;
            m_renderer->m_renderDevice->m_basicShader->SetFloat4v(ShaderUniform::tracedParams, params, 14);
         }
      }
      else
      {
         m_renderer->m_renderDevice->m_basicShader->SetTechnique(lightmap ? (img ? ShaderTechnique::unshaded_with_texture_shadow : ShaderTechnique::unshaded_without_texture_shadow)
                                                    : (img ? ShaderTechnique::unshaded_with_texture : ShaderTechnique::unshaded_without_texture));
      }
      m_renderer->m_renderDevice->DrawMesh(m_renderer->m_renderDevice->m_basicShader, true, m_d.m_vPosition, m_d.m_depthBias, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, m_groupdRendering ? m_numGroupIndices : (uint32_t)m_mesh.NumIndices());
   }
   else
   {
      // Default lit primitive rendering
      // A flasher dome with a see-through body: first its transmittance pass, which filters what lies behind the dome through
      // the plastic (it multiplies the framebuffer: ZERO / INVSRC_COLOR blending), then the body over it (same sort key,
      // submitted after it)
      if (flasherDome && mat->m_bOpacityActive && isPBR && MeasureFlasherDome().valid)
      {
         vec4 params[25];
         flasherDomeParams(params, 1.f);
         const RenderState saved = m_renderer->m_renderDevice->GetRenderState();
         m_renderer->m_renderDevice->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_TRUE);
         m_renderer->m_renderDevice->SetRenderState(RenderState::SRCBLEND, RenderState::ZERO);
         m_renderer->m_renderDevice->SetRenderState(RenderState::DESTBLEND, RenderState::INVSRC_COLOR);
         m_renderer->m_renderDevice->SetRenderState(RenderState::BLENDOP, RenderState::BLENDOP_ADD);
         m_renderer->m_renderDevice->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
         m_renderer->m_renderDevice->m_basicShader->SetFloat4v(ShaderUniform::domeParams, params, 25);
         if (Texture *const glowMap = m_ptable->GetImage(m_d.m_domeGlowMap))
            m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_traced_env, glowMap, true, SamplerFilter::SF_TRILINEAR, SamplerAddressMode::SA_CLAMP, SamplerAddressMode::SA_CLAMP);
         m_renderer->m_renderDevice->m_basicShader->SetTechnique(ShaderTechnique::flasher_dome);
         m_renderer->m_renderDevice->DrawMesh(m_renderer->m_renderDevice->m_basicShader, true, m_d.m_vPosition, m_d.m_depthBias, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, bodyIndices);
         m_renderer->m_renderDevice->GetRenderState() = saved;
      }
      m_renderer->m_renderDevice->SetRenderState(RenderState::ZWRITEENABLE, depthMask ? RenderState::RS_TRUE : RenderState::RS_FALSE);
      // A thick dome body (dome_body): the milky interior drawn by the dome layer replaces the body's lit diffuse (the glossy
      // skin's reflections stay: the shader keeps the specular whatever the coverage)
      const float bodyAlpha = (flasherDome && mat->m_bOpacityActive && m_dome.valid) ? alpha * (1.f - clamp(m_d.m_domeBody, 0.f, 1.f)) : alpha;
      const vec4 color = convertColor(m_d.m_color, bodyAlpha * (float)(1.0 / 100.0));
      m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::staticColor_Alpha, &color);
      Shader::PBRTextureSet pbrMaps = isPBR
         ? m_renderer->GetPBRTextureSet(m_d, m_d.m_vPosition, -1.f, isReflectionPass, mat->m_bOpacityActive, renderMask, mat->m_bOpacityActive ? refractions : nullptr)
         : Shader::PBRTextureSet();
      // A flasher dome body is lit by the table's lights (a translucent red body glows from the GI bulbs behind it) but not
      // by its own lamp inside it: the dome layer draws that, and the lit look is tuned on it alone
      if (flasherDome && m_dome.valid && mat->m_bOpacityActive)
         pbrMaps.ownLampRadius = 1.5f * max(m_dome.height, m_dome.radius) * m_fullMatrix.MultiplyVectorNoTranslate(Vertex3Ds(0.f, 0.f, 1.f)).Length();
      // The playfield reads the baked static shadows of the lights that have one (Renderer::UpdatePBRShadowCache)
      pbrMaps.shadowCache = m_useAsPlayfield && m_renderer->IsPBRShadowCacheOn();
      m_renderer->m_renderDevice->m_basicShader->SetTechniqueMaterial(img ? ShaderTechnique::basic_with_texture : ShaderTechnique::basic_without_texture, *mat,
         img ? pinAlphaTest >= 0.f && !img->IsOpaque() : false, nMap, reflections, refractions, isPBR ? &pbrMaps : nullptr);

      // Handle render probes
      if (reflections || refractions)
      {
         m_mesh.UpdateBounds();
         float xMin = 1.f, yMin = 1.f, xMax = -1.f, yMax = -1.f;
         for (int eye = 0; eye < m_renderer->m_renderDevice->m_nEyes; eye++)
         {
            const Matrix3D & mvp = m_renderer->GetMVP().GetModelViewProj(eye);
            for (int i = 0; i < 8; i++)
            {
               Vertex3Ds p;
               p.x = (i & 1) ? m_mesh.m_minAABound.x : m_mesh.m_maxAABound.x;
               p.y = (i & 2) ? m_mesh.m_minAABound.y : m_mesh.m_maxAABound.y;
               p.z = (i & 4) ? m_mesh.m_minAABound.z : m_mesh.m_maxAABound.z;
                     float xp = mvp._11 * p.x + mvp._21 * p.y + mvp._31 * p.z + mvp._41;
                     float yp = mvp._12 * p.x + mvp._22 * p.y + mvp._32 * p.z + mvp._42;
               const float wp = mvp._14 * p.x + mvp._24 * p.y + mvp._34 * p.z + mvp._44;
               if (wp > 0.f)
               {
                  xp /= wp;
                  yp /= wp;
                  xMin = min(xMin, xp);
                  xMax = max(xMax, xp);
                  yMin = min(yMin, yp);
                  yMax = max(yMax, yp);
               }
               else
               {
                  xMin = yMin = -1.f;
                  xMax = yMax = 1.f;
                  break;
               }
            }
         }
         if (reflections)
         {
            reflection_probe->ExtendAreaOfInterest(xMin, xMax, yMin, yMax);
            m_renderer->m_renderDevice->AddRenderTargetDependency(reflections);
            Vertex3Ds plane_normal = reflection_probe->GetReflectionPlaneNormal();
            // In stereo, using a single view (from left eye) is ok enough as this is just to weight the probe
            plane_normal = m_renderer->GetMVP().GetModelViewInverseTranspose(0).MultiplyVectorNoTranslate(plane_normal);
            Vertex3Ds n(plane_normal.x, plane_normal.y, plane_normal.z);
            n.Normalize();
            m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::mirrorNormal_factor, n.x, n.y, n.z, m_d.m_reflectionStrength);
            m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_reflection, reflections->GetColorSampler());
            is_reflection_only_pass = m_d.m_staticRendering && !isPBR && isDynamicOnly;
            if (!is_reflection_only_pass && !m_renderer->m_renderDevice->GetRenderState().IsOpaque())
            { // Primitive uses alpha transparency => render in 2 passes, one for the texture with alpha blending, one for the reflections which can happen above a transparent part (like for a glass or insert plastic)
               m_renderer->m_renderDevice->m_basicShader->SetTechniqueMaterial(
                  img ? ShaderTechnique::basic_with_texture : ShaderTechnique::basic_without_texture, 
                  *mat, img ? pinAlphaTest >= 0.f && !img->IsOpaque() : false, nMap, false, false);
               // OpenGL & BGFX shaders derive doReflections from the reflection factor: null it so that only the reflection pass applies it
               m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::mirrorNormal_factor, n.x, n.y, n.z, 0.f);
               m_renderer->m_renderDevice->DrawMesh(m_renderer->m_renderDevice->m_basicShader, mat->m_bOpacityActive && (!m_d.m_staticRendering || isPBR), m_d.m_vPosition, m_d.m_depthBias,
                  m_meshBuffer, RenderDevice::TRIANGLELIST, 0, m_groupdRendering ? m_numGroupIndices : (uint32_t)m_mesh.NumIndices());
               m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::mirrorNormal_factor, n.x, n.y, n.z, m_d.m_reflectionStrength);
               is_reflection_only_pass = true;
            }
            if (is_reflection_only_pass)
            { // If the primitive is already rendered (dynamic pass after a static prepass, or multipass rendering due to alpha blending) => only render additive reflections
               m_renderer->m_renderDevice->EnableAlphaBlend(true);
               m_renderer->m_renderDevice->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
               m_renderer->m_renderDevice->m_basicShader->SetTechnique(ShaderTechnique::basic_reflection_only);
            }
         }
         if (refractions)
         {
            refraction_probe->ExtendAreaOfInterest(xMin, xMax, yMin, yMax);
            const vec4 colorR = convertColor(mat->m_cRefractionTint, m_d.m_refractionThickness);
            // Soft in-front rejection fallback in the PBR refraction (on by default, VPX_REFR_SOFT=0 restores the stepped one for A/B)
            static const float refrSoftFallback = std::getenv("VPX_REFR_SOFT") ? (float)atof(std::getenv("VPX_REFR_SOFT")) : 1.f;
            m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::refractionTint_thickness, &colorR);
            m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::refractionParams, m_d.m_refractionCurvature, clamp(m_d.m_refractionAnisotropy, 0.f, 1.f), refrSoftFallback, m_refrCurvatureEncoded ? 1.f : 0.f);
            m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::refractionParams2, max(m_d.m_refractionWave, 0.f), max(m_d.m_refractionNoise, 0.f), max(m_d.m_refractionNoiseScale, 0.01f), clamp(m_d.m_refractionWaveFlow, 0.f, 1.f));
            m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_refraction, refractions->GetColorSampler());
            m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_probe_depth, refractions->GetDepthSampler());
            m_renderer->m_renderDevice->SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_FALSE);
            if (!is_reflection_only_pass)
               m_renderer->m_renderDevice->AddRenderTargetDependencyOnNextRenderCommand(refractions); // Add a renderpass dependency on the render command (instead of in the renderframe) for the pass to be sorted with the command
         }
      }

      // draw the mesh (back of it if backface enabled)
      m_renderer->m_renderDevice->DrawMesh(m_renderer->m_renderDevice->m_basicShader,
         is_reflection_only_pass // The reflection pass is an additive (so transparent) pass to be drawn after the opaque one
            || refractions // Refractions must be rendered back to front since they rely on what is behind
            || (mat->m_bOpacityActive
               && (!m_d.m_staticRendering
                  || isPBR) /* && !m_renderer->m_renderDevice->GetRenderState().IsOpaque() */), // We can not use the real render state opaque state since Blood Machine and other tables use depth masks
         m_d.m_vPosition, m_d.m_depthBias, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, bodyIndices);
   }

   // Draw the front of the primitive if backface enabled
   if (depthMask && m_d.m_backfacesEnabled && mat->m_bOpacityActive)
   {
      m_renderer->m_renderDevice->SetRenderState(RenderState::CULLMODE, cullMode);
      m_renderer->m_renderDevice->DrawMesh(m_renderer->m_renderDevice->m_basicShader, mat->m_bOpacityActive, m_d.m_vPosition, m_d.m_depthBias, 
         m_meshBuffer, RenderDevice::TRIANGLELIST, 0, bodyIndices);
   }

   // Flasher dome layer: additive over the body just drawn (same sort key, submitted after it), outer skin only (depth equal)
   if (flasherDome && !is_reflection_only_pass && MeasureFlasherDome().valid)
   {
      vec4 params[25];
      flasherDomeParams(params, mat->m_bOpacityActive ? 2.f : 0.f);
      m_renderer->m_renderDevice->ResetRenderState();
      m_renderer->m_renderDevice->EnableAlphaBlend(true);
      m_renderer->m_renderDevice->SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
      m_renderer->m_renderDevice->SetRenderState(RenderState::CULLMODE, cullMode);
      // dome_lens: the probe (a copy of the scene behind the dome) for the lensed slices, over the dome's screen bounds
      if (domeLensRT)
      {
         params[19].w = clamp(m_d.m_domeLens, 0.f, 1.f);
         params[21].w = FlasherDomeTune("lensw=", 12.f); // the gather disc, in fifths of the dome radius
         m_mesh.UpdateBounds();
         float xMin = 1.f, yMin = 1.f, xMax = -1.f, yMax = -1.f;
         for (int eye = 0; eye < m_renderer->m_renderDevice->m_nEyes; eye++)
         {
            const Matrix3D &mvp = m_renderer->GetMVP().GetModelViewProj(eye);
            for (int i = 0; i < 8; i++)
            {
               const Vertex3Ds p((i & 1) ? m_mesh.m_minAABound.x : m_mesh.m_maxAABound.x, (i & 2) ? m_mesh.m_minAABound.y : m_mesh.m_maxAABound.y, (i & 4) ? m_mesh.m_minAABound.z : m_mesh.m_maxAABound.z);
               const float wp = mvp._14 * p.x + mvp._24 * p.y + mvp._34 * p.z + mvp._44;
               if (wp <= 0.f)
               {
                  xMin = yMin = -1.f;
                  xMax = yMax = 1.f;
                  break;
               }
               const float xp = (mvp._11 * p.x + mvp._21 * p.y + mvp._31 * p.z + mvp._41) / wp, yp = (mvp._12 * p.x + mvp._22 * p.y + mvp._32 * p.z + mvp._42) / wp;
               xMin = min(xMin, xp);
               xMax = max(xMax, xp);
               yMin = min(yMin, yp);
               yMax = max(yMax, yp);
            }
         }
         // the dome's centre on screen (render target uv, eye 0), for the gather disc
         {
            const Matrix3D &mvp = m_renderer->GetMVP().GetModelViewProj(0);
            const Vertex3Ds c(m_dome.cx, m_dome.cy, m_dome.baseZ + 0.5f * m_dome.height);
            const float wc = mvp._14 * c.x + mvp._24 * c.y + mvp._34 * c.z + mvp._44;
            if (wc > 0.f)
            {
               // as a render target uv: y follows the backend's framebuffer origin (bottom left on OpenGL), as gl_FragCoord does
               const float nx = (mvp._11 * c.x + mvp._21 * c.y + mvp._31 * c.z + mvp._41) / wc, ny = (mvp._12 * c.x + mvp._22 * c.y + mvp._32 * c.z + mvp._42) / wc;
               #if defined(ENABLE_BGFX)
               const bool originBottomLeft = bgfx::getCaps()->originBottomLeft;
               #else
               const bool originBottomLeft = true;
               #endif
               params[24] = vec4(0.5f + 0.5f * nx, originBottomLeft ? 0.5f + 0.5f * ny : 0.5f - 0.5f * ny, 1.f, 0.f);
            }
         }
         // the gather disc reaches past the dome a little: widen the area
         const float mx = 0.8f * (xMax - xMin), my = 0.8f * (yMax - yMin); // room for the gather disc and the face taps
         lensProbe->ExtendAreaOfInterest(xMin - mx, xMax + mx, yMin - my, yMax + my);
         params[24].w = (float)(domeLensRT->GetMipCount() - 1);
         m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_pbr_refr, domeLensRT->GetColorSampler());
         m_renderer->m_renderDevice->AddRenderTargetDependencyOnNextRenderCommand(domeLensRT);
      }
      m_renderer->m_renderDevice->m_basicShader->SetFloat4v(ShaderUniform::domeParams, params, 25);
         if (Texture *const glowMap = m_ptable->GetImage(m_d.m_domeGlowMap))
            m_renderer->m_renderDevice->m_basicShader->SetTexture(ShaderUniform::tex_traced_env, glowMap, true, SamplerFilter::SF_TRILINEAR, SamplerAddressMode::SA_CLAMP, SamplerAddressMode::SA_CLAMP);
      m_renderer->m_renderDevice->m_basicShader->SetTechnique(ShaderTechnique::flasher_dome);
      m_renderer->m_renderDevice->DrawMesh(m_renderer->m_renderDevice->m_basicShader, true, m_d.m_vPosition, m_d.m_depthBias, m_meshBuffer, RenderDevice::TRIANGLELIST, 0, bodyIndices);
   }

   // Restore state
   m_renderer->UpdateBasicShaderMatrix();
   m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::mirrorNormal_factor, 0.f, 0.f, 0.f, 0.f);
   m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::lightCenter_doShadow, 0.0f, 0.0f, 0.0f, 0.0f);
   m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::staticColor_Alpha, 1.0f, 1.0f, 1.0f, 1.0f);
   m_renderer->m_renderDevice->m_basicShader->SetVector(ShaderUniform::fDisableLighting_top_below, 0.f, 0.f, 0.f, 0.f);
}

void Primitive::UpdateAnimation(const float diff_time_msec)
{
   if (m_currentFrame != -1.0f && m_doAnimation)
   {
      const float previousFrame = m_currentFrame;
      m_currentFrame += m_speed * (diff_time_msec * (float)(60. / 1000.));
      const float maxFrame = (float)(m_mesh.m_animationFrames.size()-1);
      if (m_currentFrame > maxFrame)
      {
         if (m_endless)
            m_currentFrame = fminf(m_currentFrame-maxFrame, maxFrame);
         else
         {
            m_currentFrame = maxFrame;
            m_doAnimation = false;
         }
      }
      m_vertexBufferRegenerate |= m_currentFrame != previousFrame;
   }
}

//////////////////////////////
// Positioning
//////////////////////////////

// x' = a x + b y + c, y' = d x + e y + f
static Matrix3D GenAffine(const float a, const float b, const float c, const float d, const float e, const float f)
{
   return Matrix3D { a, d, 0.f, 0.f, b, e, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, c, f, 0.f, 1.f };
}

// The drawn parts of a generated primitive: nothing can be deferred when one is not drawn under its own matrix (rendered in a group)
bool Primitive::CanDeferGenMesh() const
{
   if (m_renderer == nullptr || m_groupdRendering || m_skipRendering)
      return false;
   if (m_d.m_plasticGen)
      for (const Primitive *const comp : GetPlasticCompanions())
         if (comp->m_renderer == nullptr || comp->m_groupdRendering || comp->m_skipRendering)
            return false;
   return true;
}

// The wire or plastic points just moved by t. In an editor drag the live buffers are drawn under t until FlushGenDrag rebuilds the mesh at the drag end,
// like a plain primitive's matrix (no mesh build and no buffers per frame, and the vertex count can not change under a queued frame); else the mesh is rebuilt now
void Primitive::OnGenPointsMoved(const Matrix3D &t)
{
   if (!m_genDragDefer)
   {
      RegenerateWire();
      RegeneratePlastic();
      return;
   }
   const auto move = [&t](Primitive *const p)
   {
      p->m_genDragMatrix = p->m_genDragMoved ? p->m_genDragMatrix * t : t;
      p->m_genDragMoved = true;
   };
   move(this);
   if (m_d.m_plasticGen)
      for (Primitive *const comp : GetPlasticCompanions())
         move(comp);
}

void Primitive::FlushGenDrag()
{
   if (!m_genDragMoved)
      return;
   RegenerateWire();
   RegeneratePlastic();
   if (m_renderer)
      m_renderer->ReinitRenderable(this);
}

void Primitive::Translate(const Vertex2D &offset)
{
   if (IsPlasticCompanion()) // Derived from its host (identity transform, the host carries the curve)
      return;
   if (m_d.m_plasticGen)
   {
      m_plasticCurve.TranslatePoints(offset);
      m_plasticHoles.TranslatePoints(offset);
      PlasticMapTransform({ 1.f, 0.f, offset.x, 0.f, 1.f, offset.y });
      OnGenPointsMoved(Matrix3D::MatrixTranslate(offset.x, offset.y, 0.f));
      return;
   }
   if (m_d.m_wireGen)
   {
      // The callers (SetSelectionTransform) reinit the renderable and update the collider themselves
      m_wireCurve.TranslatePoints(offset);
      OnGenPointsMoved(Matrix3D::MatrixTranslate(offset.x, offset.y, 0.f));
      return;
   }
   m_d.m_vPosition.x += offset.x;
   m_d.m_vPosition.y += offset.y;
   CalculateBuiltinOriginal();
}

void Primitive::Rotate(const float ang, const Vertex2D &pvCenter, const bool useElementCenter)
{
   if (IsPlasticCompanion())
      return;
   if (m_d.m_plasticGen)
   {
      const Vertex2D c = useElementCenter ? GetCenter() : pvCenter;
      const float sn = sinf(ANGTORAD(ang)), cs = cosf(ANGTORAD(ang));
      m_plasticCurve.RotatePoints(ang, c);
      m_plasticHoles.RotatePoints(ang, c);
      PlasticMapTransform({ cs, -sn, c.x - cs * c.x + sn * c.y, sn, cs, c.y - sn * c.x - cs * c.y });
      OnGenPointsMoved(GenAffine(cs, -sn, c.x - cs * c.x + sn * c.y, sn, cs, c.y - sn * c.x - cs * c.y));
      return;
   }
   if (!m_d.m_wireGen)
      return IEditable::Rotate(ang, pvCenter, useElementCenter);
   const Vertex2D c = useElementCenter ? GetCenter() : pvCenter;
   const float sn = sinf(ANGTORAD(ang)), cs = cosf(ANGTORAD(ang));
   m_wireCurve.RotatePoints(ang, c);
   OnGenPointsMoved(GenAffine(cs, -sn, c.x - cs * c.x + sn * c.y, sn, cs, c.y - sn * c.x - cs * c.y));
}

void Primitive::Scale(const float scalex, const float scaley, const Vertex2D &pvCenter, const bool useElementCenter)
{
   if (IsPlasticCompanion())
      return;
   if (m_d.m_plasticGen)
   {
      const Vertex2D c = useElementCenter ? GetCenter() : pvCenter;
      m_plasticCurve.ScalePoints(scalex, scaley, c);
      m_plasticHoles.ScalePoints(scalex, scaley, c);
      for (const auto &h : m_plasticHoles.GetPoints()) // The diameter follows the scale
         h->SetZ(h->GetZ() * 0.5f * (fabsf(scalex) + fabsf(scaley)));
      PlasticMapTransform({ scalex, 0.f, c.x * (1.f - scalex), 0.f, scaley, c.y * (1.f - scaley) });
      OnGenPointsMoved(GenAffine(scalex, 0.f, c.x * (1.f - scalex), 0.f, scaley, c.y * (1.f - scaley)));
      return;
   }
   if (!m_d.m_wireGen)
      return IEditable::Scale(scalex, scaley, pvCenter, useElementCenter);
   const Vertex2D c = useElementCenter ? GetCenter() : pvCenter;
   m_wireCurve.ScalePoints(scalex, scaley, c);
   OnGenPointsMoved(GenAffine(scalex, 0.f, c.x * (1.f - scalex), 0.f, scaley, c.y * (1.f - scaley)));
}

Vertex2D Primitive::GetCenter() const
{
   if (m_d.m_plasticGen)
      return m_plasticCurve.GetCenter();
   if (m_d.m_wireGen)
      return m_wireCurve.GetCenter();
   return {m_d.m_vPosition.x, m_d.m_vPosition.y};
}

//////////////////////////////
// Metal Wire Guide generator
//////////////////////////////

void Primitive::RegenerateWire()
{
   if (!m_d.m_wireGen)
      return;
   if (m_d.m_wireRail)
   {
      RegenerateRail();
      return;
   }
   vector<RenderVertex3D> path;
   m_wireCurve.GetRgVertex(path, false, 4.f);
   for (RenderVertex3D &v : path)
      v.z += m_d.m_wireHeight; // point z is an offset from the wire height (0 = VPE)
   m_mesh.Clear();
   // ends: 0 stands, 1 round, 2 flat, 3 none -> WireSweep::Caps
   static constexpr WireSweep::Caps endCaps[4] = { WireSweep::Caps::Stands, WireSweep::Caps::Round, WireSweep::Caps::Flat, WireSweep::Caps::None };
   WireSweep::BuildWireform<Vertex3D_NoTex2>(path, max(m_d.m_wireThickness, 0.1f), m_d.m_wireSides, m_d.m_wireRails, m_d.m_wireSpacing, endCaps[clamp(m_d.m_wireEnds, 0, 3)],
      m_d.m_wireStandHeight, m_d.m_wireBendRadius, m_mesh.m_vertices, m_mesh.m_indices);
   // The mesh is in table coordinates: identity transform
   m_d.m_vPosition = Vertex3Ds(0.f, 0.f, 0.f);
   m_d.m_vSize = Vertex3Ds(1.f, 1.f, 1.f);
   for (float &v : m_d.m_aRotAndTra)
      v = 0.f;
   m_d.m_use3DMesh = true;
   m_genDragMoved = false;
   // No m_vertexBufferRegenerate: a static mesh has an immutable vertex buffer, the renderable reinit (RenderSetup) rebuilds it
   OnMeshVerticesChanged();
   m_physicMatrix.Scale(0.f, 0.f, 0.f); // Invalidate to force the collider rebuild (the transform itself did not change)
}

void Primitive::UpdateWire()
{
   if (m_d.m_wireRail)
      RegenerateRail(); // Before the physics update below, whatever the renderer does
   if (m_renderer)
      m_renderer->ReinitRenderable(this); // RenderSetup regenerates the mesh
   else
      RegenerateWire();
   if (g_pplayer && g_pplayer->m_physics)
      g_pplayer->m_physics->Update(this);
}

void Primitive::InitWireform(const Vertex2D &pos)
{
   m_d.m_wireGen = true;
   m_d.m_collidable = true;
   m_d.m_toy = false;
   m_d.m_overwritePhysics = true;
   m_d.m_elasticity = 0.f; // MetalWireGuideData defaults
   m_d.m_elasticityFalloff = 0.f;
   m_d.m_friction = 0.f;
   m_d.m_scatter = 0.f;
   m_wireCurve.ClearPoints();
   // 3 point arc, ~100 VP units span, z offset 0 (the wire sits at Height)
   m_wireCurve.PushPoint(std::make_unique<DragPoint>(&m_wireCurve, pos.x - 50.f, pos.y, 0.f, true));
   m_wireCurve.PushPoint(std::make_unique<DragPoint>(&m_wireCurve, pos.x, pos.y + 25.f, 0.f, true));
   m_wireCurve.PushPoint(std::make_unique<DragPoint>(&m_wireCurve, pos.x + 50.f, pos.y, 0.f, true));
   RegenerateWire();
}

//////////////////////////////
// Wire Rail generator
//////////////////////////////

vector<WireRail::Wire> Primitive::GetRailWires() const
{
   return vector<WireRail::Wire>(m_d.m_railWires, m_d.m_railWires + clamp(m_d.m_railCount, 1, WireRail::kMaxWires));
}

void Primitive::SetRailWires(const vector<WireRail::Wire> &wires)
{
   m_d.m_railCount = clamp((int)wires.size(), 1, WireRail::kMaxWires);
   for (int i = 0; i < m_d.m_railCount; i++)
      m_d.m_railWires[i] = wires[i];
}

void Primitive::InitWireRail(const Vertex2D &pos)
{
   m_d.m_wireGen = true;
   m_d.m_wireRail = true;
   m_d.m_collidable = true;
   m_d.m_toy = true; // For older engines, which would collide with the tubes: the channel part holds the collider there
   m_d.m_overwritePhysics = true;
   m_d.m_elasticity = 0.3f; // WireRailComponent defaults
   m_d.m_elasticityFalloff = 0.5f;
   m_d.m_friction = 0.15f;
   m_d.m_scatter = 0.f;
   m_d.m_wireHeight = 0.f; // A point's z is its height
   m_d.m_wireThickness = WireRail::kDefaultWireDiameter * 0.5f;
   m_d.m_wireSides = WireRail::kDefaultSides;
   m_d.m_wireEnds = 3;
   m_d.m_wireRails = 1;
   SetRailWires(WireRail::DefaultWires(4, true));
   m_wireCurve.ClearPoints();
   // VPE: a straight route of 500 on the playfield (z 0), you raise it. In VPX it is centred on the click, as a new ramp is
   m_wireCurve.PushPoint(std::make_unique<DragPoint>(&m_wireCurve, pos.x, pos.y - WireRail::kDefaultLength * 0.5f, 0.f, true));
   m_wireCurve.PushPoint(std::make_unique<DragPoint>(&m_wireCurve, pos.x, pos.y + WireRail::kDefaultLength * 0.5f, 0.f, true));
   RegenerateRail();
}

bool Primitive::IsRailChannel() const
{
   return m_d.m_plasticRole == kRailChannelRole && IsPlasticCompanion();
}

void Primitive::RegenerateRail()
{
   vector<RenderVertex3D> fine;
   m_wireCurve.GetRgVertex(fine, false, 0.05f);
   if (fine.size() < 2)
      return;
   const auto &points = m_wireCurve.GetPoints();
   vector<WireRail::PathPoint> path;
   vector<float> rolls;
   size_t next = 0;
   for (const RenderVertex3D &v : fine)
   {
      path.push_back({ v.x, v.y, v.z, v.controlPoint });
      if (!v.controlPoint)
         continue;
      while (next < points.size() && (points[next]->GetX() != v.x || points[next]->GetY() != v.y || points[next]->GetZ() != v.z)) // Coincident points give no span
         next++;
      rolls.push_back(next < points.size() ? points[next]->m_roll : 0.f);
   }
   rolls.push_back(points.empty() ? 0.f : points.back()->m_roll);
   m_railFrames = WireRail::BuildFrames(WireRail::BuildRows(path, rolls, m_d.m_railCurvatureDetail));
   m_railMinRadius = WireRail::MinRadius(m_railFrames);

   const float radius = max(m_d.m_wireThickness, 0.05f);
   vector<Vertex3D_NoTex2> verts;
   vector<unsigned int> idx;
   const vector<WireRail::Wire> wires = GetRailWires();
   const bool tubes = WireRail::BuildTubes<Vertex3D_NoTex2>(m_railFrames, wires, radius, m_d.m_wireSides, m_d.m_railCapBevel, verts, idx);
   m_railRowDist = WireRail::RowDistances(m_railFrames);
   m_railError.clear();
   const bool fits = WireRail::FitProfile(wires, radius, max(m_d.m_railBallDiameter, 1.f) * 0.5f, m_railProfile, m_railError);
   // The supports and their solder, visual only: after the tubes, into the same mesh (the fit above is what the ball sits on, for the warning of a support in its path)
   WireRail::BuildFixtures<Vertex3D_NoTex2>(m_railFrames, m_railRowDist, wires, radius, m_d.m_wireSides, m_d.m_railCapBevel, max(m_d.m_railBallDiameter, 1.f) * 0.5f, fits ? m_railProfile : WireRail::Profile {},
      m_d.m_railFixtures, verts, idx, m_railFixtureShapes);
   if (tubes)
   {
      m_railFixtureTriangles = (int)(idx.size() / 3);
      m_mesh.Clear();
      m_mesh.m_vertices = std::move(verts);
      m_mesh.m_indices = std::move(idx);
   }
   // The mesh is in table coordinates: identity transform
   const auto identity = [](Primitive *const p)
   {
      p->m_d.m_vPosition = Vertex3Ds(0.f, 0.f, 0.f);
      p->m_d.m_vSize = Vertex3Ds(1.f, 1.f, 1.f);
      for (float &v : p->m_d.m_aRotAndTra)
         v = 0.f;
      p->m_d.m_use3DMesh = true;
      p->m_genDragMoved = false;
      p->OnMeshVerticesChanged(); // No m_vertexBufferRegenerate: a static mesh has an immutable vertex buffer, the renderable reinit (RenderSetup) rebuilds it
      p->m_physicMatrix.Scale(0.f, 0.f, 0.f); // Invalidate the collider (the transform itself did not change)
   };
   identity(this);

   WireRail::Channel channel = fits ? WireRail::BuildChannel(m_railFrames, m_railProfile) : WireRail::Channel {};
   const bool channelSame = WireRail::SameChannel(channel, m_railChannel);
   m_railChannel = std::move(channel);
   // The editor refreshes picking once the mesh has been rebuilt. A rebuild that left the channel as it was (a support edit) needs the picking quadtree only, not the colliders
   if (channelSame)
      m_railPickOnlyDirty = true;
   else
      m_plasticPickDirty = true; // Never cleared here: a pending full refresh stays
   // The channel parts are filled every time (a cheap copy): a channel part created after the first build, or restored by an undo or a paste, has no mesh of its own yet

   for (Primitive *const comp : GetPlasticCompanions())
   {
      if (m_railChannel.verts.empty())
         continue; // No fit: the channel part keeps its last mesh for older engines, this engine shows the error and has no collider
      comp->m_mesh.Clear();
      comp->m_mesh.m_vertices.resize(m_railChannel.verts.size());
      for (size_t i = 0; i < m_railChannel.verts.size(); i++)
      {
         const WireRail::V3 &v = m_railChannel.verts[i];
         comp->m_mesh.m_vertices[i] = Vertex3D_NoTex2 { v.x, v.y, v.z, 0.f, 0.f, 1.f, 0.f, 0.f };
      }
      comp->m_mesh.m_indices = m_railChannel.tris;
      identity(comp);
   }
}

void Primitive::UpdateRailFixtures()
{
   // RenderSetup rebuilds the mesh, fixtures included. RegenerateRail compares the channel with the last one: unchanged, it asks for the picking quadtree only (the supports are in the mesh a click picks)
   if (m_renderer)
      m_renderer->ReinitRenderable(this);
   else
      RegenerateRail();
}

bool Primitive::GradeRail(const int firstPoint, const int lastPoint)
{
   const auto &points = m_wireCurve.GetPoints();
   vector<RenderVertex3D> fine;
   m_wireCurve.GetRgVertex(fine, false, 0.05f);
   vector<float> plan(points.size(), 0.f), z;
   float along = 0.f;
   size_t next = 0;
   for (size_t i = 0; i < fine.size(); i++)
   {
      if (i > 0)
         along += sqrtf((fine[i].x - fine[i - 1].x) * (fine[i].x - fine[i - 1].x) + (fine[i].y - fine[i - 1].y) * (fine[i].y - fine[i - 1].y));
      if (fine[i].controlPoint)
      {
         while (next < points.size() && (points[next]->GetX() != fine[i].x || points[next]->GetY() != fine[i].y || points[next]->GetZ() != fine[i].z))
            plan[next++] = along;
         if (next < points.size())
            plan[next++] = along;
      }
   }
   while (next < points.size())
      plan[next++] = along;
   for (const auto &p : points)
      z.push_back(p->GetZ());
   if (!WireRail::Grade(plan, z, firstPoint, lastPoint))
      return false;
   for (int i = firstPoint + 1; i < lastPoint; i++)
      points[i]->SetZ(z[i]);
   return true;
}

//////////////////////////////
// Plastic from Atlas generator
//////////////////////////////

bool Primitive::IsPlasticCompanion() const
{
   if (m_d.m_plasticHost.empty())
      return false;
   if (m_ptable == nullptr) // Loading: the host may come later
      return true;
   for (const IEditable *const part : m_ptable->GetParts())
      if (part != static_cast<const IEditable *>(this) && part->GetItemType() == eItemPrimitive && (static_cast<const Primitive *>(part)->m_d.m_plasticGen || static_cast<const Primitive *>(part)->m_d.m_wireRail) && StrCompareNoCase(part->GetName(), m_d.m_plasticHost))
         return true;
   return false;
}

vector<Primitive *> Primitive::GetPlasticCompanions() const
{
   vector<Primitive *> res;
   if (m_ptable == nullptr || (!m_d.m_plasticGen && !m_d.m_wireRail) || GetName().empty())
      return res;
   const string name = GetName();
   for (IEditable *const part : m_ptable->GetParts())
      if (part != static_cast<const IEditable *>(this) && part->GetItemType() == eItemPrimitive)
      {
         Primitive *const p = static_cast<Primitive *>(part);
         if (p->IsPlasticCompanion() && StrCompareNoCase(p->m_d.m_plasticHost, name))
            res.push_back(p);
      }
   return res;
}

// m_plasticUvm = M, the points moved by T: the art stays on the part when M' = M o T^-1
void Primitive::PlasticMapTransform(const float (&t)[6])
{
   const float det = t[0] * t[4] - t[1] * t[3];
   if (fabsf(det) < 1e-9f)
      return;
   const float i0 = t[4] / det, i1 = -t[1] / det, i3 = -t[3] / det, i4 = t[0] / det;
   const float i2 = -(i0 * t[2] + i1 * t[5]), i5 = -(i3 * t[2] + i4 * t[5]);
   float *const m = m_d.m_plasticUvm;
   const float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5];
   m[0] = a * i0 + b * i3;
   m[1] = a * i1 + b * i4;
   m[2] = a * i2 + b * i5 + c;
   m[3] = d * i0 + e * i3;
   m[4] = d * i1 + e * i4;
   m[5] = d * i2 + e * i5 + f;
}

void Primitive::SetPlasticDpi(const float dpi)
{
   const float nd = clamp(dpi, 10.f, 4800.f);
   const float k = nd / m_d.m_plasticDpi;
   const Vertex2D c = m_plasticCurve.GetCentroid();
   float *const m = m_d.m_plasticUvm;
   const float u0 = m[0] * c.x + m[1] * c.y + m[2], v0 = m[3] * c.x + m[4] * c.y + m[5];
   m[0] *= k;
   m[1] *= k;
   m[3] *= k;
   m[4] *= k;
   m[2] = u0 - (m[0] * c.x + m[1] * c.y);
   m[5] = v0 - (m[3] * c.x + m[4] * c.y);
   m_d.m_plasticDpi = nd;
}

void Primitive::RegeneratePlastic()
{
   if (!m_d.m_plasticGen || m_plasticCurve.GetPoints().size() < 3)
      return;
   const auto setMesh = [](Primitive *const p, const PlasticMesh::Mesh &src)
      {
         p->m_mesh.Clear();
         p->m_mesh.m_vertices.resize(src.verts.size());
         for (size_t i = 0; i < src.verts.size(); i++)
         {
            const PlasticMesh::Vertex &v = src.verts[i];
            p->m_mesh.m_vertices[i] = Vertex3D_NoTex2 { v.x, v.y, v.z, v.nx, v.ny, v.nz, v.tu, v.tv };
         }
         p->m_mesh.m_indices = src.idx;
         // Table coordinates: identity transform
         p->m_d.m_vPosition = Vertex3Ds(0.f, 0.f, 0.f);
         p->m_d.m_vSize = Vertex3Ds(1.f, 1.f, 1.f);
         for (float &v : p->m_d.m_aRotAndTra)
            v = 0.f;
         p->m_d.m_use3DMesh = true;
         p->m_genDragMoved = false;
         // No m_vertexBufferRegenerate: a static mesh has an immutable vertex buffer, the renderable reinit (RenderSetup) rebuilds it
         p->OnMeshVerticesChanged();
         p->m_plasticPickDirty = true;
         p->m_physicMatrix.Scale(0.f, 0.f, 0.f); // Invalidate the collider (the transform itself did not change)
      };
   vector<PlasticMesh::CurvePoint> outline;
   for (const auto &pt : m_plasticCurve.GetPoints())
      outline.push_back({ pt->GetX(), pt->GetY(), pt->IsSmooth() });
   vector<PlasticMesh::Hole> holes;
   {
      const auto &hp = m_plasticHoles.GetPoints();
      const vector<int> partner = PlasticPillPartners(m_plasticHoles);
      for (int i = 0; i < static_cast<int>(hp.size()); i++)
      {
         if (partner[i] >= 0 && partner[i] < i) // The second end of a pill
            continue;
         PlasticMesh::Hole h { hp[i]->GetX(), hp[i]->GetY(), hp[i]->GetZ() };
         if (partner[i] > i)
         {
            h.x2 = hp[partner[i]]->GetX();
            h.y2 = hp[partner[i]]->GetY();
            h.pill = true;
         }
         holes.push_back(h);
      }
   }
   PlasticMesh::Params params;
   params.flatten = PlasticMesh::FlattenTolerance(m_d.m_plasticDetail);
   params.height = m_d.m_plasticHeight;
   params.thickness = m_d.m_plasticThickness;
   params.bevel = m_d.m_plasticBevel;
   params.bevelSegments = m_d.m_plasticBevelSegs;
   for (int i = 0; i < 6; i++)
      params.uvm[i] = m_d.m_plasticUvm[i];
   PlasticMesh::Result res;
   if (!PlasticMesh::Build(outline, holes, params, res))
      return; // Degenerate outline: keep the meshes as they are
   m_plasticCrossingHoles = res.crossingHoles;
   m_plasticSkippedHoles = res.skippedHoles;
   m_plasticCrossingWarn = res.crossingHoles != m_plasticReportedCrossing || res.skippedHoles != m_plasticReportedSkipped; // Said once per change of state, not on every rebuild
   setMesh(this, res.bottom);
   for (Primitive *const comp : GetPlasticCompanions())
   {
      // The author sees one plastic: the render flags of its Properties reach the clear and bevel parts too
      comp->m_d.m_visible = m_d.m_visible;
      comp->m_d.m_staticRendering = m_d.m_staticRendering;
      comp->m_d.m_depthBias = m_d.m_depthBias;
      comp->m_d.m_reflectionEnabled = m_d.m_reflectionEnabled;
      setMesh(comp, comp->m_d.m_plasticRole == 1 ? res.top : res.bevel);
      if (comp->m_renderer != nullptr)
         comp->m_renderer->ReinitRenderable(comp);
   }
}

vector<int> Primitive::PlasticPillPartners(const DragPointCurve &holes)
{
   const auto &pts = holes.GetPoints();
   vector<int> res(pts.size(), -1);
   for (int i = 0; i + 1 < static_cast<int>(pts.size()); i++)
      if (res[i] < 0 && pts[i]->IsSmooth() && fabsf(pts[i]->GetZ() - pts[i + 1]->GetZ()) < 1e-4f)
      {
         res[i] = i + 1;
         res[i + 1] = i;
         i++; // The second end is not the first end of another pill
      }
   return res;
}

void Primitive::SetPlasticHoleDiameter(const DragPoint *const hole, const float dia)
{
   const auto &pts = m_plasticHoles.GetPoints();
   const vector<int> partner = PlasticPillPartners(m_plasticHoles);
   const float d = clamp(dia, 0.5f, 100.f);
   for (int i = 0; i < static_cast<int>(pts.size()); i++)
      if (pts[i].get() == hole)
      {
         pts[i]->SetZ(d);
         if (partner[i] >= 0)
            pts[partner[i]]->SetZ(d);
      }
}

void Primitive::UpdatePlastic()
{
   if (m_renderer)
      m_renderer->ReinitRenderable(this); // RenderSetup regenerates the meshes (the live buffers are not touched before)
   else
      RegeneratePlastic();
}

void Primitive::InitPlastic(const vector<Vertex3Ds> &outline, const vector<bool> &smooth, const vector<Vertex3Ds> &holes)
{
   m_d.m_plasticGen = true;
   m_d.m_collidable = false;
   m_d.m_toy = true;
   m_plasticEditHoles = false;
   m_plasticCurve.ClearPoints();
   m_plasticHoles.ClearPoints();
   for (size_t i = 0; i < outline.size(); i++)
      m_plasticCurve.PushPoint(std::make_unique<DragPoint>(&m_plasticCurve, outline[i].x, outline[i].y, 0.f, i < smooth.size() && smooth[i]));
   for (const Vertex3Ds &h : holes)
      m_plasticHoles.PushPoint(std::make_unique<DragPoint>(&m_plasticHoles, h.x, h.y, h.z, false));
   RegeneratePlastic();
}

//////////////////////////////
// Save and Load
//////////////////////////////

// A Wire Rail fixture (RFIX, a skippable sub object: older versions skip it, still hashing it). All the fields of its type are written
static void SaveRailFixture(IObjectWriter &writer, const WireRail::Fixture &f)
{
   writer.BeginObject(FID(RFIX), true, true);
   writer.WriteInt(FID(RFTY), f.type);
   writer.WriteFloat(FID(RFPS), f.position);
   writer.WriteBool(FID(RFEN), f.enabled);
   writer.WriteFloat(FID(RFST), f.solderThreshold);
   writer.WriteFloat(FID(RFSS), f.solderSize);
   switch (f.type)
   {
   case WireRail::kFixtureRing:
      writer.WriteVector2(FID(RFOF), Vertex2D(f.offsetX, f.offsetZ));
      writer.WriteFloat(FID(RFSC), f.scale);
      writer.WriteInt(FID(RFDN), f.density);
      writer.WriteVector3(FID(RFCU), vec3(f.cutout ? 1.f : 0.f, f.cutoutStart, f.cutoutEnd));
      writer.WriteVector3(FID(RFSL), vec3(f.straight ? 1.f : 0.f, f.straightStart, f.straightEnd));
      break;
   case WireRail::kFixtureRung:
      writer.WriteVector2(FID(RFOF), Vertex2D(f.offsetX, f.offsetZ));
      writer.WriteFloat(FID(RFAN), f.angle);
      writer.WriteFloat(FID(RFLN), f.length);
      break;
   case WireRail::kFixtureCradle:
      writer.WriteVector2(FID(RFOF), Vertex2D(f.offsetX, f.offsetZ));
      writer.WriteInt(FID(RFDN), f.density);
      writer.WriteFloat(FID(RFBL), f.bottomLength);
      writer.WriteVector2(FID(RFAM), Vertex2D(f.leftArm, f.rightArm));
      writer.WriteFloat(FID(RFAA), f.armAngle);
      writer.WriteFloat(FID(RFRT), f.rotation);
      writer.WriteFloat(FID(RFCR), f.cornerRadius);
      break;
   case WireRail::kFixtureStand:
      writer.WriteVector2(FID(RFOF), Vertex2D(f.offsetX, f.offsetZ));
      writer.WriteFloat(FID(RFLN), f.length);
      writer.WriteInt(FID(RFLS), f.legLeft ? 0 : 1);
      writer.WriteVector3(FID(RFSV), vec3(f.startVector.x, f.startVector.y, f.startVector.z));
      writer.WriteFloat(FID(RFSD), f.startLength);
      writer.WriteVector3(FID(RFFP), vec3(f.footPosition.x, f.footPosition.y, f.footPosition.z));
      writer.WriteVector3(FID(RFFR), vec3(f.footRotation.x, f.footRotation.y, f.footRotation.z));
      writer.WriteBool(FID(RFFC), f.footClockwise);
      writer.WriteFloat(FID(RFFW), f.footWidth);
      writer.WriteFloat(FID(RFFL), f.footArm);
      writer.WriteFloat(FID(RFFA), f.footConnectedArm);
      break;
   default: break; // A type of a later version (hairpin, elbow, rail trim) has its fields where it was made: only the common ones are kept
   }
   writer.EndObject();
}

static WireRail::Fixture LoadRailFixture(IObjectReader &reader)
{
   WireRail::Fixture f;
   reader.AsObject(
      [&f](int tag, IObjectReader &r)
      {
         switch (tag)
         {
         case FID(RFTY): f.type = r.AsInt(); break;
         case FID(RFPS): f.position = r.AsFloat(); break;
         case FID(RFEN): f.enabled = r.AsBool(); break;
         case FID(RFST): f.solderThreshold = r.AsFloat(); break;
         case FID(RFSS): f.solderSize = r.AsFloat(); break;
         case FID(RFOF): { const Vertex2D v = r.AsVector2(); f.offsetX = v.x; f.offsetZ = v.y; break; }
         case FID(RFSC): f.scale = r.AsFloat(); break;
         case FID(RFDN): f.density = r.AsInt(); break;
         case FID(RFCU): { const vec3 v = r.AsVector3(); f.cutout = v.x != 0.f; f.cutoutStart = v.y; f.cutoutEnd = v.z; break; }
         case FID(RFSL): { const vec3 v = r.AsVector3(); f.straight = v.x != 0.f; f.straightStart = v.y; f.straightEnd = v.z; break; }
         case FID(RFAN): f.angle = r.AsFloat(); break;
         case FID(RFLN): f.length = r.AsFloat(); break;
         case FID(RFBL): f.bottomLength = r.AsFloat(); break;
         case FID(RFAM): { const Vertex2D v = r.AsVector2(); f.leftArm = v.x; f.rightArm = v.y; break; }
         case FID(RFAA): f.armAngle = r.AsFloat(); break;
         case FID(RFRT): f.rotation = r.AsFloat(); break;
         case FID(RFCR): f.cornerRadius = r.AsFloat(); break;
         case FID(RFLS): f.legLeft = r.AsInt() == 0; break;
         case FID(RFSV): { const vec3 v = r.AsVector3(); f.startVector = { v.x, v.y, v.z }; break; }
         case FID(RFSD): f.startLength = r.AsFloat(); break;
         case FID(RFFP): { const vec3 v = r.AsVector3(); f.footPosition = { v.x, v.y, v.z }; break; }
         case FID(RFFR): { const vec3 v = r.AsVector3(); f.footRotation = { v.x, v.y, v.z }; break; }
         case FID(RFFC): f.footClockwise = r.AsBool(); break;
         case FID(RFFW): f.footWidth = r.AsFloat(); break;
         case FID(RFFL): f.footArm = r.AsFloat(); break;
         case FID(RFFA): f.footConnectedArm = r.AsFloat(); break;
         }
         return true;
      },
      true);
   WireRail::ClampFixture(f);
   return f;
}

void Primitive::Save(IObjectWriter& writer, const bool saveForUndo)
{
   if (!saveForUndo && m_ptable != nullptr && m_d.m_plasticRole == kRailChannelRole) // The channel part collides in older engines: with the physics of its rail
      for (const IEditable *const part : m_ptable->GetParts())
         if (part->GetItemType() == eItemPrimitive && static_cast<const Primitive *>(part)->m_d.m_wireRail && StrCompareNoCase(part->GetName(), m_d.m_plasticHost))
         {
            const PrimitiveData &rail = static_cast<const Primitive *>(part)->m_d;
            m_d.m_elasticity = rail.m_elasticity;
            m_d.m_elasticityFalloff = rail.m_elasticityFalloff;
            m_d.m_friction = rail.m_friction;
            m_d.m_scatter = rail.m_scatter;
            m_d.m_overwritePhysics = rail.m_overwritePhysics;
            m_d.m_szPhysicsMaterial = rail.m_szPhysicsMaterial;
            m_d.m_collidable = rail.m_collidable;
            m_d.m_hitEvent = rail.m_hitEvent;
            m_d.m_threshold = rail.m_threshold;
            m_d.m_toy = false; // A default Toy would give the older engine no collider
         }
   writer.WriteVector4(FID(VPOS), vec4(m_d.m_vPosition.x, m_d.m_vPosition.y, m_d.m_vPosition.z, 0.f));
   writer.WriteVector4(FID(VSIZ), vec4(m_d.m_vSize.x, m_d.m_vSize.y, m_d.m_vSize.z, 0.f));
   writer.WriteFloat(FID(RTV0), m_d.m_aRotAndTra[0]);
   writer.WriteFloat(FID(RTV1), m_d.m_aRotAndTra[1]);
   writer.WriteFloat(FID(RTV2), m_d.m_aRotAndTra[2]);
   writer.WriteFloat(FID(RTV3), m_d.m_aRotAndTra[3]);
   writer.WriteFloat(FID(RTV4), m_d.m_aRotAndTra[4]);
   writer.WriteFloat(FID(RTV5), m_d.m_aRotAndTra[5]);
   writer.WriteFloat(FID(RTV6), m_d.m_aRotAndTra[6]);
   writer.WriteFloat(FID(RTV7), m_d.m_aRotAndTra[7]);
   writer.WriteFloat(FID(RTV8), m_d.m_aRotAndTra[8]);
   writer.WriteString(FID(IMAG), m_d.m_szImage);
   writer.WriteString(FID(NRMA), m_d.m_szNormalMap);
   writer.WriteString(FID(ORMA), m_d.m_szORMMap);
   writer.WriteString(FID(EMIS), m_d.m_szEmissionMap);
   writer.WriteString(FID(DMGM), m_d.m_domeGlowMap);
   writer.WriteInt(FID(SIDS), m_d.m_Sides);
   writer.WriteWideString(FID(NAME), MakeWString(m_name));
   writer.WriteString(FID(MATR), m_d.m_szMaterial);
   writer.WriteInt(FID(SCOL), m_d.m_SideColor);
   writer.WriteBool(FID(TVIS), m_d.m_visible);
   writer.WriteBool(FID(DTXI), m_d.m_drawTexturesInside);
   writer.WriteBool(FID(HTEV), m_d.m_hitEvent);
   writer.WriteFloat(FID(THRS), m_d.m_threshold);
   writer.WriteFloat(FID(ELAS), m_d.m_elasticity);
   writer.WriteFloat(FID(ELFO), m_d.m_elasticityFalloff);
   writer.WriteFloat(FID(RFCT), m_d.m_friction);
   writer.WriteFloat(FID(RSCT), m_d.m_scatter);
   writer.WriteFloat(FID(EFUI), m_d.m_edgeFactorUI);
   writer.WriteFloat(FID(CORF), m_d.m_collision_reductionFactor);
   writer.WriteBool(FID(CLDR), m_d.m_collidable);
   writer.WriteBool(FID(ISTO), m_d.m_toy);
   writer.WriteBool(FID(U3DM), m_d.m_use3DMesh);
   writer.WriteBool(FID(STRE), m_d.m_staticRendering);
   writer.WriteFloat(FID(DILT), m_d.m_disableLightingTop);
   writer.WriteFloat(FID(DILB), m_d.m_disableLightingBelow);
   writer.WriteBool(FID(REEN), m_d.m_reflectionEnabled);
   writer.WriteBool(FID(EBFC), m_d.m_backfacesEnabled);
   writer.WriteBool(FID(CLBF), m_d.m_cullBackfaces);
   writer.WriteString(FID(MAPH), m_d.m_szPhysicsMaterial);
   writer.WriteBool(FID(OVPH), m_d.m_overwritePhysics);
   writer.WriteBool(FID(DIPT), m_d.m_displayTexture);
   writer.WriteBool(FID(OSNM), m_d.m_objectSpaceNormalMap);

   // Don't save the meshes for undo/redo
   if (m_d.m_use3DMesh && !saveForUndo)
   {
      writer.WriteString(FID(M3DN), m_d.m_meshFileName);
      writer.WriteInt(FID(M3VN), (int)m_mesh.NumVertices());

#ifndef COMPRESS_MESHES
      writer.WriteRaw(FID(M3DX), m_mesh.m_vertices.data(), (int)(sizeof(Vertex3D_NoTex2)*m_mesh.NumVertices()));
#else
      {
      const mz_ulong slen = (mz_ulong)(sizeof(Vertex3D_NoTex2)*m_mesh.NumVertices());
      mz_ulong clen = compressBound(slen);
      mz_uint8 * const c = new mz_uint8[clen];
      if (compress2(c, &clen, (const unsigned char *)m_mesh.m_vertices.data(), slen, MZ_BEST_COMPRESSION) != Z_OK)
         ShowError("Could not compress primitive vertex data");
      writer.WriteInt(FID(M3CY), (int)clen);
      writer.WriteRaw(FID(M3CX), c, clen);
      delete [] c;
      }
#endif

      writer.WriteInt(FID(M3FN), (int)m_mesh.NumIndices());
      if (m_mesh.NumVertices() > 65535)
      {
#ifndef COMPRESS_MESHES
         writer.WriteRaw(FID(M3DI), m_mesh.m_indices.data(), (int)(sizeof(unsigned int)*m_mesh.NumIndices()));
#else
         const mz_ulong slen = (mz_ulong)(sizeof(unsigned int)*m_mesh.NumIndices());
         mz_ulong clen = compressBound(slen);
         mz_uint8 * const c = new mz_uint8[clen];
         if (compress2(c, &clen, (const unsigned char *)m_mesh.m_indices.data(), slen, MZ_BEST_COMPRESSION) != Z_OK)
            ShowError("Could not compress primitive index data");
         writer.WriteInt(FID(M3CJ), (int)clen);
         writer.WriteRaw(FID(M3CI), c, clen);
         delete [] c;
#endif
      }
      else
      {
         vector<WORD> tmp(m_mesh.NumIndices());
         for (size_t i = 0; i < m_mesh.NumIndices(); ++i)
            tmp[i] = m_mesh.m_indices[i];
#ifndef COMPRESS_MESHES
         writer.WriteRaw(FID(M3DI), tmp.data(), (int)(sizeof(WORD)*m_mesh.NumIndices()));
#else
         const mz_ulong slen = (mz_ulong)(sizeof(WORD)*m_mesh.NumIndices());
         mz_ulong clen = compressBound(slen);
         mz_uint8 * const c = new mz_uint8[clen];
         if (compress2(c, &clen, (const unsigned char *)tmp.data(), slen, MZ_BEST_COMPRESSION) != Z_OK)
            ShowError("Could not compress primitive index data");
         writer.WriteInt(FID(M3CJ), (int)clen);
         writer.WriteRaw(FID(M3CI), c, clen);
         delete [] c;
#endif
      }
      
      if (!m_mesh.m_animationFrames.empty())
      {
         const mz_ulong slen = (mz_ulong)(sizeof(Mesh::VertData)*m_mesh.NumVertices());
         for (size_t i = 0; i < m_mesh.m_animationFrames.size(); i++)
         {
            mz_ulong clen = compressBound(slen);
            mz_uint8 * const c = new mz_uint8[clen];
            if (compress2(c, &clen, (const unsigned char *)m_mesh.m_animationFrames[i].m_frameVerts.data(), slen, MZ_BEST_COMPRESSION) != Z_OK)
               ShowError("Could not compress primitive animation vertex data");
            writer.WriteInt(FID(M3AY), (int)clen);
            writer.WriteRaw(FID(M3AX), c, clen);
            delete [] c;
         }

      }
   }
   if (m_d.m_wireGen) // Optional fields: only written for wireforms, so other primitives save byte-identical
   {
      writer.WriteBool(FID(WGEN), true);
      writer.WriteFloat(FID(WHGT), m_d.m_wireHeight);
      writer.WriteFloat(FID(WTHK), m_d.m_wireThickness);
      writer.WriteFloat(FID(WSTH), m_d.m_wireStandHeight);
      writer.WriteFloat(FID(WBND), m_d.m_wireBendRadius);
      writer.WriteInt(FID(WEND), m_d.m_wireEnds);
      writer.WriteInt(FID(WRLS), m_d.m_wireRails);
      writer.WriteFloat(FID(WSPC), m_d.m_wireSpacing);
      writer.WriteInt(FID(WSEG), m_d.m_wireSides);
      if (m_d.m_wireRail) // Optional fields: only written for rails, so wire guides save byte-identical
      {
         writer.WriteBool(FID(WRAL), true);
         writer.WriteInt(FID(RLCT), m_d.m_railCount);
         static constexpr int wireFields[WireRail::kMaxWires] = { FID(RLW1), FID(RLW2), FID(RLW3), FID(RLW4), FID(RLW5), FID(RLW6) };
         for (int i = 0; i < m_d.m_railCount; i++)
            writer.WriteVector3(wireFields[i], vec3(m_d.m_railWires[i].x, m_d.m_railWires[i].z, m_d.m_railWires[i].active ? 1.f : 0.f));
         writer.WriteBool(FID(RLTH), m_d.m_railThirdRight);
         writer.WriteFloat(FID(RLBV), m_d.m_railCapBevel);
         writer.WriteFloat(FID(RLBD), m_d.m_railBallDiameter);
         writer.WriteInt(FID(RLCD), m_d.m_railCurvatureDetail);
         writer.WriteBool(FID(RLSC), m_d.m_railShowCollider);
         for (const WireRail::Fixture &f : m_d.m_railFixtures) // None for a rail without supports: it saves byte-identical to one that never had them
            SaveRailFixture(writer, f);
      }
      m_wireCurve.SavePoints(writer);
   }
   if (m_d.m_plasticGen) // Optional fields: only written for plastic hosts and companions, so other primitives save byte-identical
   {
      writer.WriteBool(FID(PGEN), true);
      writer.WriteFloat(FID(PHGT), m_d.m_plasticHeight);
      writer.WriteFloat(FID(PTHK), m_d.m_plasticThickness);
      writer.WriteFloat(FID(PBVL), m_d.m_plasticBevel);
      writer.WriteInt(FID(PBSG), m_d.m_plasticBevelSegs);
      if (m_d.m_plasticDetail > 1) // Only when above the default: plastics without it save as before
         writer.WriteInt(FID(PDTL), m_d.m_plasticDetail);
      writer.WriteFloat(FID(PHDI), m_d.m_plasticHoleDia);
      writer.WriteFloat(FID(PDPI), m_d.m_plasticDpi);
      writer.WriteVector3(FID(PUVM), vec3(m_d.m_plasticUvm[0], m_d.m_plasticUvm[1], m_d.m_plasticUvm[2]));
      writer.WriteVector3(FID(PUVV), vec3(m_d.m_plasticUvm[3], m_d.m_plasticUvm[4], m_d.m_plasticUvm[5]));
      m_plasticCurve.SavePoints(writer);
      m_plasticHoles.SavePoints(writer, FID(PHLS));
   }
   if (!m_d.m_plasticHost.empty())
   {
      writer.WriteString(FID(PHST), m_d.m_plasticHost);
      writer.WriteInt(FID(PHRL), m_d.m_plasticRole);
   }
   if (m_d.m_rubberGuideRadius > 0.f) // Optional field, other primitives save byte-identical
      writer.WriteFloat(FID(RGRD), m_d.m_rubberGuideRadius);
   writer.WriteFloat(FID(PIDB), m_d.m_depthBias);
   writer.WriteFloat(FID(TRGN), m_d.m_tracedGain);
   writer.WriteFloat(FID(TRHS), m_d.m_tracedHaloSize);
   writer.WriteFloat(FID(TRHG), m_d.m_tracedHaloStrength);
   writer.WriteString(FID(TRPS), m_d.m_tracedParams);
   writer.WriteFloat(FID(BLBD), m_d.m_bulbDepth);
   writer.WriteFloat(FID(BLBS), m_d.m_bulbScale);
   writer.WriteFloat(FID(BLBB), m_d.m_bulbBrightness);
   writer.WriteFloat(FID(BLSX), m_d.m_bulbStretchX);
   writer.WriteFloat(FID(BLSY), m_d.m_bulbStretchY);
   writer.WriteFloat(FID(BLOX), m_d.m_bulbOffsetX);
   writer.WriteFloat(FID(BLOY), m_d.m_bulbOffsetY);
   writer.WriteBool(FID(ADDB), m_d.m_addBlend);
   writer.WriteBool(FID(ZMSK), m_d.m_useDepthMask);
   writer.WriteFloat(FID(FALP), m_d.m_alpha);
   writer.WriteInt(FID(COLR), m_d.m_color);

   writer.WriteString(FID(LMAP), m_d.m_szLightmap);
   writer.WriteString(FID(SLNK), m_d.m_stateSrcLink);

   writer.WriteString(FID(REFL), m_d.m_szReflectionProbe);
   writer.WriteFloat(FID(RSTR), m_d.m_reflectionStrength);
   writer.WriteString(FID(REFR), m_d.m_szRefractionProbe);
   writer.WriteFloat(FID(RTHI), m_d.m_refractionThickness);
   writer.WriteFloat(FID(RCRV), m_d.m_refractionCurvature);
   writer.WriteFloat(FID(RANI), m_d.m_refractionAnisotropy);
   writer.WriteFloat(FID(RWAV), m_d.m_refractionWave);
   writer.WriteFloat(FID(RWFL), m_d.m_refractionWaveFlow);
   writer.WriteFloat(FID(RNOI), m_d.m_refractionNoise);
   writer.WriteFloat(FID(RNSC), m_d.m_refractionNoiseScale);
   writer.WriteFloat(FID(DMBD), m_d.m_domeBody);
   writer.WriteFloat(FID(DMSC), m_d.m_domeScatter);
   writer.WriteFloat(FID(DMCP), m_d.m_domeCap);
   writer.WriteInt(FID(DMFL), m_d.m_domeFlutes);
   writer.WriteFloat(FID(DMFD), m_d.m_domeFluteDepth);
   writer.WriteInt(FID(DMTN), m_d.m_domeTint);
   writer.WriteFloat(FID(DMGL), m_d.m_domeGlint);
   writer.WriteFloat(FID(DMGT), m_d.m_domeGlintLights);
   writer.WriteFloat(FID(DMFR), m_d.m_domeFrost);
   writer.WriteFloat(FID(DMLN), m_d.m_domeLens);
   writer.WriteFloat(FID(DMPK), m_d.m_domePeak);
   writer.WriteFloat(FID(DMDP), m_d.m_domeDepth);
   writer.WriteFloat(FID(DMCD), m_d.m_domeCooldown);
   writer.WriteFloat(FID(DMOR), m_d.m_domeOrb);
   writer.WriteFloat(FID(DMBO), m_d.m_domeBoost);

   SaveSharedEditableFields(writer);

   writer.EndObject();
}

void Primitive::Load(IObjectReader& reader)
{
   SetDefaults(false);
   m_wireCurve.ClearPoints();
   m_plasticCurve.ClearPoints();
   m_plasticHoles.ClearPoints();
   m_mesh.m_validBounds = false;
   reader.AsObject(
      [this](int tag, IObjectReader& reader)
      {
         switch (tag)
         {
         case FID(PIID): reader.AsInt(); break; // Deprecated (unique part id, now the name is guaranteed to be unique)
         case FID(BMIN): reader.AsVector3(); break; // Deprecated (bounding box min)
         case FID(BMAX): reader.AsVector3(); break; // Deprecated (bounding box max)
         case FID(VPOS): m_d.m_vPosition = reader.AsVector4().xyz(); break;
         case FID(VSIZ): m_d.m_vSize = reader.AsVector4().xyz(); break;
         case FID(RTV0): m_d.m_aRotAndTra[0] = reader.AsFloat(); break;
         case FID(RTV1): m_d.m_aRotAndTra[1] = reader.AsFloat(); break;
         case FID(RTV2): m_d.m_aRotAndTra[2] = reader.AsFloat(); break;
         case FID(RTV3): m_d.m_aRotAndTra[3] = reader.AsFloat(); break;
         case FID(RTV4): m_d.m_aRotAndTra[4] = reader.AsFloat(); break;
         case FID(RTV5): m_d.m_aRotAndTra[5] = reader.AsFloat(); break;
         case FID(RTV6): m_d.m_aRotAndTra[6] = reader.AsFloat(); break;
         case FID(RTV7): m_d.m_aRotAndTra[7] = reader.AsFloat(); break;
         case FID(RTV8): m_d.m_aRotAndTra[8] = reader.AsFloat(); break;
         case FID(IMAG): m_d.m_szImage = reader.AsString(); break;
         case FID(NRMA): m_d.m_szNormalMap = reader.AsString(); break;
         case FID(ORMA): m_d.m_szORMMap = reader.AsString(); break;
         case FID(EMIS): m_d.m_szEmissionMap = reader.AsString(); break;
         case FID(DMGM): m_d.m_domeGlowMap = reader.AsString(); break;
         case FID(SIDS): m_d.m_Sides = clamp(reader.AsInt(), 3, Max_Primitive_Sides); break;
         case FID(NAME): m_name = MakeString(reader.AsWideString()); break;
         case FID(MATR): m_d.m_szMaterial = reader.AsString(); break;
         case FID(SCOL): m_d.m_SideColor = reader.AsInt(); break;
         case FID(TVIS): m_d.m_visible = reader.AsBool(); break;
         case FID(REEN): m_d.m_reflectionEnabled = reader.AsBool(); break;
         case FID(DTXI): m_d.m_drawTexturesInside = reader.AsBool(); break;
         case FID(HTEV): m_d.m_hitEvent = reader.AsBool(); break;
         case FID(THRS): m_d.m_threshold = reader.AsFloat(); break;
         case FID(ELAS): m_d.m_elasticity = reader.AsFloat(); break;
         case FID(ELFO): m_d.m_elasticityFalloff = reader.AsFloat(); break;
         case FID(RFCT): m_d.m_friction = reader.AsFloat(); break;
         case FID(RSCT): m_d.m_scatter = reader.AsFloat(); break;
         case FID(EFUI): m_d.m_edgeFactorUI = reader.AsFloat(); break;
         case FID(CORF): m_d.m_collision_reductionFactor = reader.AsFloat(); break;
         case FID(CLDR): m_d.m_collidable = reader.AsBool(); break;
         case FID(ISTO): m_d.m_toy = reader.AsBool(); break;
         case FID(MAPH): m_d.m_szPhysicsMaterial = reader.AsString(); break;
         case FID(OVPH): m_d.m_overwritePhysics = reader.AsBool(); break;
         case FID(STRE): m_d.m_staticRendering = reader.AsBool(); break;
         case FID(DILI):
         {
            int tmp;
            tmp = reader.AsInt();
            m_d.m_disableLightingTop = (tmp == 1) ? 1.f : dequantizeUnsigned<8>(tmp);
            break;
         } // Pre 10.8 compatible hacky loading!
         case FID(DILT): m_d.m_disableLightingTop = reader.AsFloat(); break;
         case FID(DILB): m_d.m_disableLightingBelow = reader.AsFloat(); break;
         case FID(U3DM): m_d.m_use3DMesh = reader.AsBool(); break;
         case FID(EBFC): m_d.m_backfacesEnabled = reader.AsBool(); break;
         case FID(CLBF): m_d.m_cullBackfaces = reader.AsBool(); break;
         case FID(RGRD): { const float v = reader.AsFloat(); m_d.m_rubberGuideRadius = v > 0.f ? v : 0.f; break; }
         case FID(WGEN): m_d.m_wireGen = reader.AsBool(); break;
         case FID(WHGT): m_d.m_wireHeight = reader.AsFloat(); break;
         case FID(WTHK): m_d.m_wireThickness = reader.AsFloat(); break;
         case FID(WSTH): m_d.m_wireStandHeight = reader.AsFloat(); break;
         case FID(WBND): m_d.m_wireBendRadius = reader.AsFloat(); break;
         case FID(WEND): m_d.m_wireEnds = clamp(reader.AsInt(), 0, 3); break;
         case FID(WRLS): m_d.m_wireRails = clamp(reader.AsInt(), 1, 3); break;
         case FID(WSPC): m_d.m_wireSpacing = reader.AsFloat(); break;
         case FID(WSEG): m_d.m_wireSides = clamp(reader.AsInt(), 3, 32); break;
         case FID(WRAL): m_d.m_wireRail = reader.AsBool(); break;
         case FID(RLCT): m_d.m_railCount = clamp(reader.AsInt(), 1, WireRail::kMaxWires); break;
         case FID(RLW1): case FID(RLW2): case FID(RLW3): case FID(RLW4): case FID(RLW5): case FID(RLW6):
         {
            const vec3 v = reader.AsVector3();
            const int i = tag == FID(RLW1) ? 0 : tag == FID(RLW2) ? 1 : tag == FID(RLW3) ? 2 : tag == FID(RLW4) ? 3 : tag == FID(RLW5) ? 4 : 5;
            m_d.m_railWires[i] = { v.x, v.y, v.z != 0.f };
            break;
         }
         case FID(RLTH): m_d.m_railThirdRight = reader.AsBool(); break;
         case FID(RLBV): m_d.m_railCapBevel = reader.AsFloat(); break;
         case FID(RLBD): m_d.m_railBallDiameter = reader.AsFloat(); break;
         case FID(RLCD): m_d.m_railCurvatureDetail = clamp(reader.AsInt(), 2, 32); break;
         case FID(RLSC): m_d.m_railShowCollider = reader.AsBool(); break;
         case FID(RFIX):
         {
            WireRail::Fixture f = LoadRailFixture(reader);
            if (m_d.m_railFixtures.size() < WireRail::kMaxFixtures)
               m_d.m_railFixtures.push_back(f);
            break;
         }
         case FID(DPNT): m_wireCurve.LoadPointToken(reader); break; // Moved to the plastic outline after the load if this is a plastic host
         case FID(PGEN): m_d.m_plasticGen = reader.AsBool(); break;
         case FID(PHGT): m_d.m_plasticHeight = reader.AsFloat(); break;
         case FID(PTHK): m_d.m_plasticThickness = reader.AsFloat(); break;
         case FID(PBVL): m_d.m_plasticBevel = reader.AsFloat(); break;
         case FID(PBSG): m_d.m_plasticBevelSegs = clamp(reader.AsInt(), 1, 16); break;
         case FID(PDTL): m_d.m_plasticDetail = clamp(reader.AsInt(), 1, 5); break;
         case FID(PHDI): m_d.m_plasticHoleDia = reader.AsFloat(); break;
         case FID(PDPI): m_d.m_plasticDpi = clamp(reader.AsFloat(), 10.f, 4800.f); break;
         case FID(PUVM): { const vec3 v = reader.AsVector3(); m_d.m_plasticUvm[0] = v.x; m_d.m_plasticUvm[1] = v.y; m_d.m_plasticUvm[2] = v.z; break; }
         case FID(PUVV): { const vec3 v = reader.AsVector3(); m_d.m_plasticUvm[3] = v.x; m_d.m_plasticUvm[4] = v.y; m_d.m_plasticUvm[5] = v.z; break; }
         case FID(PHLS): m_plasticHoles.LoadPointToken(reader); break;
         case FID(PHST): m_d.m_plasticHost = reader.AsString(); break;
         case FID(PHRL): m_d.m_plasticRole = clamp(reader.AsInt(), 0, kRailChannelRole); break;
         case FID(DIPT): m_d.m_displayTexture = reader.AsBool(); break;
         case FID(M3DN): m_d.m_meshFileName = reader.AsString(); break;
         case FID(M3VN):
         {
            const int numVertices = reader.AsInt();
            if (numVertices < 0 || numVertices > 0x400000) // arbitrary sanity bound, largest real meshes are far smaller
            {
               ShowError("Invalid vertex count in primitive mesh data");
               break;
            }
            m_numVertices = numVertices;
            if (!m_mesh.m_animationFrames.empty())
            {
               for (size_t i = 0; i < m_mesh.m_animationFrames.size(); i++)
                  m_mesh.m_animationFrames[i].m_frameVerts.clear();
               m_mesh.m_animationFrames.clear();
            }
            break;
         }
         case FID(M3DX):
         {
            m_mesh.m_vertices.clear();
            m_mesh.m_vertices.resize(m_numVertices);
            reader.AsRaw(m_mesh.m_vertices.data(), (int)sizeof(Vertex3D_NoTex2) * m_numVertices);
            break;
         }
#ifdef COMPRESS_MESHES
         case FID(M3AY):
         {
            const int size = reader.AsInt();
            if (size < 0 || size > 0x4000000) // arbitrary sanity bound on compressed blob size
            {
               ShowError("Invalid animation data size in primitive mesh data");
               break;
            }
            m_compressedAnimationVertices = size;
            break;
         }
         case FID(M3AX):
         {
            Mesh::FrameData frameData;
            frameData.m_frameVerts.clear();
            frameData.m_frameVerts.resize(m_numVertices);

            mz_ulong uclen = (mz_ulong)(sizeof(Mesh::VertData) * frameData.m_frameVerts.size());
            mz_uint8 *const c = new mz_uint8[m_compressedAnimationVertices];
            reader.AsRaw(c, m_compressedAnimationVertices);
            const int error = uncompress((unsigned char *)frameData.m_frameVerts.data(), &uclen, c, m_compressedAnimationVertices);
            if (error != Z_OK)
               ShowError("Could not uncompress primitive animation vertex data, error " + std::to_string(error));
            delete[] c;
            m_mesh.m_animationFrames.push_back(frameData);
            break;
         }
         case FID(M3CY):
         {
            const int size = reader.AsInt();
            if (size < 0 || size > 0x4000000) // arbitrary sanity bound on compressed blob size
            {
               ShowError("Invalid vertex data size in primitive mesh data");
               break;
            }
            m_compressedVertices = size;
            break;
         }
         case FID(M3CX):
         {
            m_mesh.m_vertices.clear();
            m_mesh.m_vertices.resize(m_numVertices);
            const mz_ulong uclen = (mz_ulong)(sizeof(Vertex3D_NoTex2) * m_mesh.NumVertices());
            mz_uint8 *const c = new mz_uint8[m_compressedVertices];
            reader.AsRaw(c, m_compressedVertices);
            mz_ulong uclen2 = uclen;
            const int error = uncompress((unsigned char *)m_mesh.m_vertices.data(), &uclen2, c, m_compressedVertices);
            if (error != Z_OK)
               ShowError("Could not uncompress primitive vertex data, error " + std::to_string(error));
            delete[] c;
            break;
         }
#endif
         case FID(M3FN):
         {
            const int numIndices = reader.AsInt();
            if (numIndices < 0 || numIndices > 0x1000000) // arbitrary sanity bound, largest real meshes are far smaller
            {
               ShowError("Invalid index count in primitive mesh data");
               break;
            }
            m_numIndices = numIndices;
            break;
         }
         case FID(M3DI):
         {
            m_mesh.m_indices.resize(m_numIndices);
            if (m_numVertices > 65535)
               reader.AsRaw(m_mesh.m_indices.data(), (int)sizeof(unsigned int) * m_numIndices);
            else
            {
               vector<WORD> tmp(m_numIndices);
               reader.AsRaw(tmp.data(), (int)sizeof(WORD) * m_numIndices);
               for (int i = 0; i < m_numIndices; ++i)
                  m_mesh.m_indices[i] = tmp[i];
            }
            break;
         }
#ifdef COMPRESS_MESHES
         case FID(M3CJ):
         {
            const int size = reader.AsInt();
            if (size < 0 || size > 0x4000000) // arbitrary sanity bound on compressed blob size
            {
               ShowError("Invalid index data size in primitive mesh data");
               break;
            }
            m_compressedIndices = size;
            break;
         }
         case FID(M3CI):
         {
            m_mesh.m_indices.resize(m_numIndices);
            if (m_numVertices > 65535)
            {
               const mz_ulong uclen = (mz_ulong)(sizeof(unsigned int) * m_mesh.NumIndices());
               mz_uint8 *const c = new mz_uint8[m_compressedIndices];
               reader.AsRaw(c, m_compressedIndices);
               mz_ulong uclen2 = uclen;
               const int error = uncompress((unsigned char *)m_mesh.m_indices.data(), &uclen2, c, m_compressedIndices);
               if (error != Z_OK)
                  ShowError("Could not uncompress (large) primitive index data, error " + std::to_string(error));
               delete[] c;
            }
            else
            {
               const mz_ulong uclen = (mz_ulong)(sizeof(WORD) * m_mesh.NumIndices());
               mz_uint8 *const c = new mz_uint8[m_compressedIndices];
               reader.AsRaw(c, m_compressedIndices);
               vector<WORD> tmp(m_numIndices);

               mz_ulong uclen2 = uclen;
               const int error = uncompress((unsigned char *)tmp.data(), &uclen2, c, m_compressedIndices);
               if (error != Z_OK)
                  ShowError("Could not uncompress (small) primitive index data, error " + std::to_string(error));
               delete[] c;
               for (int i = 0; i < m_numIndices; ++i)
                  m_mesh.m_indices[i] = tmp[i];
            }
            break;
         }
#endif
         case FID(PIDB): m_d.m_depthBias = reader.AsFloat(); break;
         case FID(TRGN): m_d.m_tracedGain = reader.AsFloat(); break;
         case FID(TRHS): m_d.m_tracedHaloSize = reader.AsFloat(); break;
         case FID(TRHG): m_d.m_tracedHaloStrength = reader.AsFloat(); break;
         case FID(TRPS): m_d.m_tracedParams = reader.AsString(); break;
         case FID(BLBD): m_d.m_bulbDepth = reader.AsFloat(); break;
         case FID(BLBS): m_d.m_bulbScale = reader.AsFloat(); break;
         case FID(BLBB): m_d.m_bulbBrightness = reader.AsFloat(); break;
         case FID(BLSX): m_d.m_bulbStretchX = reader.AsFloat(); break;
         case FID(BLSY): m_d.m_bulbStretchY = reader.AsFloat(); break;
         case FID(BLOX): m_d.m_bulbOffsetX = reader.AsFloat(); break;
         case FID(BLOY): m_d.m_bulbOffsetY = reader.AsFloat(); break;
         case FID(OSNM): m_d.m_objectSpaceNormalMap = reader.AsBool(); break;
         case FID(ADDB): m_d.m_addBlend = reader.AsBool(); break;
         case FID(ZMSK): m_d.m_useDepthMask = reader.AsBool(); break;
         case FID(FALP): m_d.m_alpha = reader.AsFloat(); break;
         case FID(COLR): m_d.m_color = reader.AsInt(); break;

         case FID(LMAP): m_d.m_szLightmap = reader.AsString(); break;
         case FID(SLNK): m_d.m_stateSrcLink = reader.AsString(); break;

         case FID(REFL): m_d.m_szReflectionProbe = reader.AsString(); break;
         case FID(RSTR): m_d.m_reflectionStrength = reader.AsFloat(); break;
         case FID(REFR): m_d.m_szRefractionProbe = reader.AsString(); break;
         case FID(RTHI): m_d.m_refractionThickness = reader.AsFloat(); break;
         case FID(RCRV): m_d.m_refractionCurvature = reader.AsFloat(); break;
         case FID(RANI): m_d.m_refractionAnisotropy = reader.AsFloat(); break;
         case FID(RWAV): m_d.m_refractionWave = reader.AsFloat(); break;
         case FID(RWFL): m_d.m_refractionWaveFlow = reader.AsFloat(); break;
         case FID(DMBD): m_d.m_domeBody = reader.AsFloat(); break;
         case FID(DMSC): m_d.m_domeScatter = reader.AsFloat(); break;
         case FID(DMCP): m_d.m_domeCap = reader.AsFloat(); break;
         case FID(DMFL): m_d.m_domeFlutes = reader.AsInt(); break;
         case FID(DMFD): m_d.m_domeFluteDepth = reader.AsFloat(); break;
         case FID(DMTN): m_d.m_domeTint = reader.AsInt(); break;
         case FID(DMGL): m_d.m_domeGlint = reader.AsFloat(); break;
         case FID(DMGT): m_d.m_domeGlintLights = reader.AsFloat(); break;
         case FID(DMFR): m_d.m_domeFrost = reader.AsFloat(); break;
         case FID(DMLN): m_d.m_domeLens = reader.AsFloat(); break;
         case FID(DMPK): m_d.m_domePeak = reader.AsFloat(); break;
         case FID(DMDP): m_d.m_domeDepth = reader.AsFloat(); break;
         case FID(DMCD): m_d.m_domeCooldown = reader.AsFloat(); break;
         case FID(DMOR): m_d.m_domeOrb = reader.AsFloat(); break;
         case FID(DMBO): m_d.m_domeBoost = reader.AsFloat(); break;
         case FID(RNOI): m_d.m_refractionNoise = reader.AsFloat(); break;
         case FID(RNSC): m_d.m_refractionNoiseScale = reader.AsFloat(); break;

         default: LoadSharedEditableField(tag, reader); break;
         }
         return true;
      });

   if (reader.GetVersion() < 1011) // so that old tables do the reorderForsyth on each load, new tables only on mesh import, so a simple resave of a old table will also skip this step
   {
      unsigned int *const tmp = reorderForsyth(m_mesh.m_indices, (int)m_mesh.NumVertices());
      if (tmp != nullptr)
      {
         memcpy(m_mesh.m_indices.data(), tmp, m_mesh.NumIndices() * sizeof(unsigned int));
         delete[] tmp;
      }
   }
   m_inPlayState = m_d.m_visible;
   CalculateBuiltinOriginal();
   if (m_d.m_wireGen) // The mesh is derived from the points: rebuild it (deterministic, also covers undo whose records skip meshes)
      RegenerateWire();
   if (m_d.m_plasticGen)
   {
      // The outline was read as the generic drag points (DPNT, whatever the order of the fields)
      for (const auto &dpt : m_wireCurve.GetPoints())
         m_plasticCurve.PushPoint(std::make_unique<DragPoint>(&m_plasticCurve, dpt->GetX(), dpt->GetY(), dpt->GetZ(), dpt->IsSmooth()));
      m_wireCurve.ClearPoints();
      // Live (an undo): the reinit rebuilds the meshes of the host and its companions; a file load has no renderer, the saved companion meshes stand
      if (m_renderer)
         m_renderer->ReinitRenderable(this);
      else
         RegeneratePlastic();
   }
}

bool Primitive::LoadMesh(
   const std::filesystem::path &filename, const MeshUnits units, const bool importAbsolutePosition, const bool centerMesh, const bool importMaterial, const bool importAnimation, const bool doForsyth)
{
   m_mesh.Clear();
   m_d.m_use3DMesh = false;
   m_meshBuffer = nullptr;
   m_vertexBufferRegenerate = true;

   if (importMaterial)
   {
      Material *const mat = new Material();
      if (ObjLoader::LoadMaterial(std::filesystem::path(filename).replace_extension(".mtl"), mat))
      {
         GetPTable()->AddMaterial(mat);
         m_d.m_szMaterial = mat->m_name;
      }
      else
         delete mat;
   }
   if (!m_mesh.LoadWavefrontObj(filename, units))
      return false;

   // Meshes imported in meters are converted to VP units, so the primitive scale is reset to 1
   if (units == MeshUnits::Meters)
   {
      m_d.m_vSize.x = 1.0f;
      m_d.m_vSize.y = 1.0f;
      m_d.m_vSize.z = 1.0f;
   }

   if (importAbsolutePosition || centerMesh)
   {
      for (size_t i = 0; i < m_mesh.m_vertices.size(); i++)
      {
         m_mesh.m_vertices[i].x -= m_mesh.middlePoint.x;
         m_mesh.m_vertices[i].y -= m_mesh.middlePoint.y;
         m_mesh.m_vertices[i].z -= m_mesh.middlePoint.z;
      }
      if (importAbsolutePosition)
      {
         m_d.m_vPosition.x = m_mesh.middlePoint.x;
         m_d.m_vPosition.y = m_mesh.middlePoint.y;
         m_d.m_vPosition.z = m_mesh.middlePoint.z;
         m_d.m_vSize.x = 1.0f;
         m_d.m_vSize.y = 1.0f;
         m_d.m_vSize.z = 1.0f;
      }
   }
   if (importAnimation)
   {
      if (m_mesh.LoadAnimation(filename, units))
      {
         if (centerMesh)
         {
            for (size_t t = 0; t < m_mesh.m_animationFrames.size(); t++)
            {
               for (size_t i = 0; i < m_mesh.m_vertices.size(); i++)
               {
                  m_mesh.m_animationFrames[t].m_frameVerts[i].x -= m_mesh.middlePoint.x;
                  m_mesh.m_animationFrames[t].m_frameVerts[i].y -= m_mesh.middlePoint.y;
                  m_mesh.m_animationFrames[t].m_frameVerts[i].z -= m_mesh.middlePoint.z;
               }
            }
         }
      }
   }
   m_d.m_use3DMesh = true;
   if (doForsyth)
   {
      unsigned int *const tmp = reorderForsyth(m_mesh.m_indices, (int)m_mesh.NumVertices());
      if (tmp != nullptr)
      {
         memcpy(m_mesh.m_indices.data(), tmp, m_mesh.NumIndices() * sizeof(unsigned int));
         delete[] tmp;
      }
   }
   if (auto renderer = m_renderer)
   {
      RenderRelease();
      RenderSetup(renderer);
   }
   if (g_pplayer && g_pplayer->m_physics)
   {
      m_physicMatrix.Scale(0.f, 0.f, 0.f); // Invalidate to force update
      g_pplayer->m_physics->Update(this);
   }
   return true;
}

//////////////////////////////
// Standard methods
//////////////////////////////

STDMETHODIMP Primitive::get_Image(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szImage);
   return S_OK;
}

STDMETHODIMP Primitive::put_Image(BSTR newVal)
{
   m_d.m_szImage = MakeString(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_NormalMap(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szNormalMap);
   return S_OK;
}

STDMETHODIMP Primitive::put_NormalMap(BSTR newVal)
{
   const string szImage = MakeString(newVal);
   const Texture * const tex = m_ptable->GetImage(szImage);
   if (tex && tex->IsHDR())
   {
      ShowError("Cannot use a HDR image (.exr/.hdr) here");
      return E_FAIL;
   }
   m_d.m_szNormalMap = szImage;

   return S_OK;
}

float Primitive::GetDepth(const Vertex3Ds& viewDir) const
{
   return m_d.m_depthBias + m_d.m_vPosition.Dot(viewDir);
}

STDMETHODIMP Primitive::get_Sides(int *pVal)
{
   *pVal = m_d.m_Sides;
   return S_OK;
}

STDMETHODIMP Primitive::put_Sides(int newVal)
{
   SetSides(newVal);
   return S_OK;
}

// The built-in mesh topology depends on the side count: rebuild it and the render/physics state (a regenerate flag is not enough, the vertex buffer is static and sized for the old mesh)
void Primitive::SetSides(int sides)
{
   sides = clamp(sides, 3, Max_Primitive_Sides);
   if (m_d.m_Sides == sides)
      return;
   m_d.m_Sides = sides;
   if (m_d.m_use3DMesh)
      return;
   CalculateBuiltinOriginal();
   if (m_renderer)
      m_renderer->ReinitRenderable(this);
   if (g_pplayer && g_pplayer->m_physics)
   {
      m_physicMatrix.Scale(0.f, 0.f, 0.f); // Invalidate to force update
      g_pplayer->m_physics->Update(this);
   }
}

STDMETHODIMP Primitive::get_Material(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szMaterial);
   return S_OK;
}

STDMETHODIMP Primitive::put_Material(BSTR newVal)
{
   m_d.m_szMaterial = MakeString(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_SideColor(OLE_COLOR *pVal)
{
   *pVal = m_d.m_SideColor;
   return S_OK;
}

STDMETHODIMP Primitive::put_SideColor(OLE_COLOR newVal)
{
   m_d.m_SideColor = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Visible(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_visible);
   return S_OK;
}

STDMETHODIMP Primitive::put_Visible(VARIANT_BOOL newVal)
{
   m_d.m_visible = VBTOF(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_DrawTexturesInside(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_drawTexturesInside);
   return S_OK;
}

STDMETHODIMP Primitive::put_DrawTexturesInside(VARIANT_BOOL newVal)
{
   if (m_d.m_drawTexturesInside != VBTOb(newVal))
   {
      m_d.m_drawTexturesInside = VBTOb(newVal);
      m_vertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Primitive::get_X(float *pVal)
{
   *pVal = m_d.m_vPosition.x;
   return S_OK;
}

STDMETHODIMP Primitive::put_X(float newVal)
{
   m_d.m_vPosition.x = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Y(float *pVal)
{
   *pVal = m_d.m_vPosition.y;
   return S_OK;
}

STDMETHODIMP Primitive::put_Y(float newVal)
{
   m_d.m_vPosition.y = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Z(float *pVal)
{
   *pVal = m_d.m_vPosition.z;
   return S_OK;
}

STDMETHODIMP Primitive::put_Z(float newVal)
{
   m_d.m_vPosition.z = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Size_X(float *pVal)
{
   *pVal = m_d.m_vSize.x;
   return S_OK;
}

STDMETHODIMP Primitive::put_Size_X(float newVal)
{
   m_d.m_vSize.x = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Size_Y(float *pVal)
{
   *pVal = m_d.m_vSize.y;
   return S_OK;
}

STDMETHODIMP Primitive::put_Size_Y(float newVal)
{
   m_d.m_vSize.y = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Size_Z(float *pVal)
{
   *pVal = m_d.m_vSize.z;
   return S_OK;
}

STDMETHODIMP Primitive::put_Size_Z(float newVal)
{
   m_d.m_vSize.z = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra0(float *pVal)
{
    return get_RotX(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra0(float newVal)
{
   return put_RotX(newVal);
}

STDMETHODIMP Primitive::get_RotX(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[0];
   return S_OK;
}

STDMETHODIMP Primitive::put_RotX(float newVal)
{
   m_d.m_aRotAndTra[0] = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra1(float *pVal)
{
   return get_RotY(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra1(float newVal)
{
   return put_RotY(newVal);
}

STDMETHODIMP Primitive::get_RotY(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[1];
   return S_OK;
}

STDMETHODIMP Primitive::put_RotY(float newVal)
{
   m_d.m_aRotAndTra[1] = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra2(float *pVal)
{
   return get_RotZ(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra2(float newVal)
{
   return put_RotZ(newVal);
}

STDMETHODIMP Primitive::get_RotZ(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[2];
   return S_OK;
}

STDMETHODIMP Primitive::put_RotZ(float newVal)
{
   m_d.m_aRotAndTra[2] = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra3(float *pVal)
{
   return get_TransX(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra3(float newVal)
{
   return put_TransX(newVal);
}

STDMETHODIMP Primitive::get_TransX(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[3];
   return S_OK;
}

STDMETHODIMP Primitive::put_TransX(float newVal)
{
   m_d.m_aRotAndTra[3] = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra4(float *pVal)
{
   return get_TransY(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra4(float newVal)
{
   return put_TransY(newVal);
}

STDMETHODIMP Primitive::get_TransY(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[4];
   return S_OK;
}

STDMETHODIMP Primitive::put_TransY(float newVal)
{
   m_d.m_aRotAndTra[4] = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra5(float *pVal)
{
   return get_TransZ(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra5(float newVal)
{
   return put_TransZ(newVal);
}

STDMETHODIMP Primitive::get_TransZ(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[5];
   return S_OK;
}

STDMETHODIMP Primitive::put_TransZ(float newVal)
{
   m_d.m_aRotAndTra[5] = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra6(float *pVal)
{
   return get_ObjRotX(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra6(float newVal)
{
   return put_ObjRotX(newVal);
}

STDMETHODIMP Primitive::get_ObjRotX(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[6];
   return S_OK;
}

STDMETHODIMP Primitive::put_ObjRotX(float newVal)
{
   m_d.m_aRotAndTra[6] = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra7(float *pVal)
{
   return get_ObjRotY(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra7(float newVal)
{
   return put_ObjRotY(newVal);
}

STDMETHODIMP Primitive::get_ObjRotY(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[7];
   return S_OK;
}

STDMETHODIMP Primitive::put_ObjRotY(float newVal)
{
   m_d.m_aRotAndTra[7] = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_RotAndTra8(float *pVal)
{
   return get_ObjRotZ(pVal);
}

STDMETHODIMP Primitive::put_RotAndTra8(float newVal)
{
   return put_ObjRotZ(newVal);
}

STDMETHODIMP Primitive::get_ObjRotZ(float *pVal)
{
   *pVal = m_d.m_aRotAndTra[8];
   return S_OK;
}

STDMETHODIMP Primitive::put_ObjRotZ(float newVal)
{
   m_d.m_aRotAndTra[8] = newVal;
   return S_OK;
}


STDMETHODIMP Primitive::get_Opacity(float *pVal)
{
   *pVal = m_d.m_alpha;
   return S_OK;
}

STDMETHODIMP Primitive::put_Opacity(float newVal)
{
   SetAlpha(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_Color(OLE_COLOR *pVal)
{
   *pVal = m_d.m_color;
   return S_OK;
}

STDMETHODIMP Primitive::put_Color(OLE_COLOR newVal)
{
   m_d.m_color = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_AddBlend(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_addBlend);
   return S_OK;
}

STDMETHODIMP Primitive::put_AddBlend(VARIANT_BOOL newVal)
{
   m_d.m_addBlend = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_EnableDepthMask(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_useDepthMask);
   return S_OK;
}

STDMETHODIMP Primitive::put_EnableDepthMask(VARIANT_BOOL newVal)
{
   m_d.m_useDepthMask = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_EdgeFactorUI(float *pVal)
{
   *pVal = m_d.m_edgeFactorUI;
   return S_OK;
}

STDMETHODIMP Primitive::put_EdgeFactorUI(float newVal)
{
   m_d.m_edgeFactorUI = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_CollisionReductionFactor(float *pVal)
{
   *pVal = m_d.m_collision_reductionFactor;
   return S_OK;
}

STDMETHODIMP Primitive::put_CollisionReductionFactor(float newVal)
{
   m_d.m_collision_reductionFactor = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_EnableStaticRendering(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_staticRendering);
   return S_OK;
}

STDMETHODIMP Primitive::put_EnableStaticRendering(VARIANT_BOOL newVal)
{
   m_d.m_staticRendering = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_HasHitEvent(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_hitEvent);
   return S_OK;
}

STDMETHODIMP Primitive::put_HasHitEvent(VARIANT_BOOL newVal)
{
   m_d.m_hitEvent = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_Threshold(float *pVal)
{
   *pVal = m_d.m_threshold;
   return S_OK;
}

STDMETHODIMP Primitive::put_Threshold(float newVal)
{
   m_d.m_threshold = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Elasticity(float *pVal)
{
   *pVal = m_d.m_elasticity;
   return S_OK;
}

STDMETHODIMP Primitive::put_Elasticity(float newVal)
{
   m_d.m_elasticity = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_ElasticityFalloff(float *pVal)
{
   *pVal = m_d.m_elasticityFalloff;
   return S_OK;
}

STDMETHODIMP Primitive::put_ElasticityFalloff(float newVal)
{
   m_d.m_elasticityFalloff = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Friction(float *pVal)
{
   *pVal = m_d.m_friction;
   return S_OK;
}

STDMETHODIMP Primitive::put_Friction(float newVal)
{
   m_d.m_friction = max(newVal, 0.f); // Friction can not be negative, but may exceed 1
   return S_OK;
}

STDMETHODIMP Primitive::get_Scatter(float *pVal)
{
   *pVal = m_d.m_scatter;
   return S_OK;
}

STDMETHODIMP Primitive::put_Scatter(float newVal)
{
   m_d.m_scatter = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_Collidable(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_collidable);
   return S_OK;
}

STDMETHODIMP Primitive::put_Collidable(VARIANT_BOOL newVal)
{
   const bool val = VBTOb(newVal);
   m_d.m_collidable = val;
      if (!m_vhoCollidable.empty() && m_vhoCollidable[0]->m_enabled != val)
         for (size_t i = 0; i < m_vhoCollidable.size(); i++) //!! costly
            m_vhoCollidable[i]->m_enabled = val; //copy to hit-testing on entities composing the object
   return S_OK;
}

STDMETHODIMP Primitive::get_IsToy(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_toy);
   return S_OK;
}

STDMETHODIMP Primitive::put_IsToy(VARIANT_BOOL newVal)
{
   m_d.m_toy = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_BackfacesEnabled(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_backfacesEnabled);
   return S_OK;
}

STDMETHODIMP Primitive::put_BackfacesEnabled(VARIANT_BOOL newVal)
{
   m_d.m_backfacesEnabled = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_ObjectSpaceNormalMap(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_objectSpaceNormalMap);
   return S_OK;
}

STDMETHODIMP Primitive::put_ObjectSpaceNormalMap(VARIANT_BOOL newVal)
{
   m_d.m_objectSpaceNormalMap = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_DisableLighting(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_disableLightingTop != 0.f);
   return S_OK;
}

STDMETHODIMP Primitive::put_DisableLighting(VARIANT_BOOL newVal)
{
   m_d.m_disableLightingTop = VBTOb(newVal) ? 1.f : 0;
   return S_OK;
}

STDMETHODIMP Primitive::get_BlendDisableLighting(float *pVal)
{
   *pVal = m_d.m_disableLightingTop;
   return S_OK;
}

STDMETHODIMP Primitive::put_BlendDisableLighting(float newVal)
{
   m_d.m_disableLightingTop = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_BlendDisableLightingFromBelow(float *pVal)
{
   *pVal = m_d.m_disableLightingBelow;
   return S_OK;
}

STDMETHODIMP Primitive::put_BlendDisableLightingFromBelow(float newVal)
{
   m_d.m_disableLightingBelow = newVal;
   return S_OK;
}

STDMETHODIMP Primitive::get_ReflectionEnabled(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_reflectionEnabled);
   return S_OK;
}

STDMETHODIMP Primitive::put_ReflectionEnabled(VARIANT_BOOL newVal)
{
   m_d.m_reflectionEnabled = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_PhysicsMaterial(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szPhysicsMaterial);
   return S_OK;
}

STDMETHODIMP Primitive::put_PhysicsMaterial(BSTR newVal)
{
   m_d.m_szPhysicsMaterial = MakeString(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_OverwritePhysics(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_overwritePhysics);
   return S_OK;
}

STDMETHODIMP Primitive::put_OverwritePhysics(VARIANT_BOOL newVal)
{
   m_d.m_overwritePhysics = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_HitThreshold(float *pVal)
{
   *pVal = m_currentHitThreshold;
   return S_OK;
}

STDMETHODIMP Primitive::get_DisplayTexture(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_displayTexture);
   return S_OK;
}

STDMETHODIMP Primitive::put_DisplayTexture(VARIANT_BOOL newVal)
{
   m_d.m_displayTexture = VBTOb(newVal);
   return S_OK;
}


STDMETHODIMP Primitive::PlayAnim(float startFrame, float speed)
{
   if (!m_mesh.m_animationFrames.empty() && !m_d.m_staticRendering)
   {
      if ((size_t)startFrame >= m_mesh.m_animationFrames.size())
         startFrame = 0.0f;
      //if (startFrame < 0.0f)
      //   startFrame = -startFrame;
      if (speed < 0.0f) speed = -speed;

      m_vertexBufferRegenerate = m_vertexBufferRegenerate || (m_currentFrame != startFrame) || (m_speed != speed) || !m_doAnimation || m_endless;

      m_currentFrame = startFrame;
      m_speed = speed;
      m_doAnimation = true;
      m_endless = false;
   }

   return S_OK;
}

STDMETHODIMP Primitive::PlayAnimEndless(float speed)
{
   if (!m_mesh.m_animationFrames.empty() && !m_d.m_staticRendering)
   {
      if (speed < 0.0f) speed = -speed;

      m_vertexBufferRegenerate = m_vertexBufferRegenerate || (m_currentFrame != 0.f) || (m_speed != speed) || !m_doAnimation || !m_endless;

      m_currentFrame = 0.0f;
      m_speed = speed;
      m_doAnimation = true;
      m_endless = true;
   }

   return S_OK;
}

STDMETHODIMP Primitive::StopAnim()
{
   m_doAnimation = false;
   m_vertexBufferRegenerate = false;
   return S_OK;
}

STDMETHODIMP Primitive::ContinueAnim(float speed)
{
   if (m_currentFrame > 0.0f && !m_d.m_staticRendering)
   {
      if (speed < 0.0f) speed = -speed;

      m_vertexBufferRegenerate = m_vertexBufferRegenerate || (m_speed != speed) || !m_doAnimation;

      m_speed = speed;
      m_doAnimation = true;
   }

   return S_OK;
}

STDMETHODIMP Primitive::ShowFrame(float frame)
{
   if (!m_mesh.m_animationFrames.empty() && frame >= 0.f && !m_d.m_staticRendering)
   {
      if ((size_t)frame >= m_mesh.m_animationFrames.size())
         frame = (float)(m_mesh.m_animationFrames.size() - 1);

      m_vertexBufferRegenerate = m_vertexBufferRegenerate || (m_currentFrame != frame) || m_doAnimation;

      m_currentFrame = frame;
      m_doAnimation = false;
   }

   return S_OK;
}

STDMETHODIMP Primitive::get_DepthBias(float *pVal)
{
   *pVal = m_d.m_depthBias;
   return S_OK;
}

STDMETHODIMP Primitive::put_DepthBias(float newVal)
{
   m_d.m_depthBias = newVal;
   return S_OK;
}

//Sets the in play state for light sequencing rendering
void Primitive::setInPlayState(const bool newVal)
{
    m_inPlayState = newVal;
}

STDMETHODIMP Primitive::get_ReflectionProbe(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szReflectionProbe);
   return S_OK;
}

STDMETHODIMP Primitive::put_ReflectionProbe(BSTR newVal)
{
   m_d.m_szReflectionProbe = MakeString(newVal);
   return S_OK;
}

STDMETHODIMP Primitive::get_RefractionProbe(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szRefractionProbe);
   return S_OK;
}

STDMETHODIMP Primitive::put_RefractionProbe(BSTR newVal)
{
   m_d.m_szRefractionProbe = MakeString(newVal);
   return S_OK;
}
