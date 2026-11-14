// license:GPLv3+

#pragma once

#include "core/resourceid.h"
#include "math/dragpoint.h"
#include "parts/Material.h"
#include "parts/pintable.h"
#include "physics/hitable.h"
#include "renderer/Renderable.h"
#include "utils/eventproxy.h"


class HitObject;
class HitTriangle;
class MeshBuffer;

// A typed stretch of a flat ramp whose generated geometry replaces the plain ramp's there (render and physics alike).
// Saved as one skippable RSEC record each, only when present: a ramp without sections saves and builds exactly as before.
struct RampSection
{
   enum Kind : int
   {
      EntryFlap = 1, // range starts at point 0: a shallow lead-in flap from the playfield onto the ramp
      Hairpin = 2, // range of interior points: each corner becomes an arc of the section radius
   };
   static constexpr int kVersion = 1;

   int m_kind = EntryFlap;
   int m_first = 0; // first and last control point index of the range the section replaces
   int m_last = 0;

   // Entry flap
   float m_flapAhead = 60.f; // how far ahead of the ramp foot it starts on the playfield (VPU, >= 0)
   float m_flapUp = 60.f; // how far up the ramp it lands (VPU along the path)
   float m_flapBow = 0.f; // how far the flap dips below the straight line between its two ends (VPU, >= 0)
   float m_flapBowPos = 0.5f; // where its low point sits, 0 = front edge .. 1 = landing (clamped to 0.1 .. 0.9)

   // Hairpin
   float m_radius = 50.f; // radius at the path centre (VPU)
   int m_outerFaces = 12; // faces of the outside wall over the whole section
   int m_innerFaces = 9; // faces of the inside wall over the whole section

   bool operator==(const RampSection &other) const = default;
};

// What the geometry builder made of one section (for the editor): whether it applies, the values it used after its clamps, and why not
struct RampSectionStatus
{
   bool m_applied = false;
   float m_radius = 0.f; // hairpin: radius used (limited by the leg lengths)
   float m_minRadius = 0.f; // hairpin: smallest radius that does not fold the inside (half the widest ramp width + 1)
   float m_legGap = 0.f; // hairpin: clear gap between the two legs' walls (VPU)
   float m_bow = 0.f; // flap: bow used after the caps
   float m_flapUp = 0.f; // flap: landing distance used
   float m_entryAngle = 0.f; // flap: slope of its front edge (degrees, negative = downhill)
   float m_sink = 0.f; // flap: deepest point below the ramp foot (VPU)
   string m_message;
};

class RampData final : public BaseProperty
{
public:
   float m_heightbottom;
   float m_heighttop;
   float m_widthbottom;
   float m_widthtop;
   RampType m_type;
   RampImageAlignment m_imagealignment;
   float m_leftwallheight;
   float m_rightwallheight;
   float m_leftwallheightvisible;
   float m_rightwallheightvisible;

   float m_wireDiameter;
   float m_wireDistanceX;
   float m_wireDistanceY;

   float m_depthBias;      // for determining depth sorting

   bool m_imageWalls;

   vector<RampSection> m_sections; // only applied to flat ramps
};

class Ramp :
   public IDispatchImpl<IRamp, &IID_IRamp, &LIBID_VPinballLib>,
   public ISupportErrorInfo,
   public CComObjectRoot,
   public CComCoClass<Ramp, &CLSID_Ramp>,
   public EventProxy<Ramp, &DIID_IRampEvents>,
   public IConnectionPointContainerImpl<Ramp>,
   public IProvideClassInfo2Impl<&CLSID_Ramp, &DIID_IRampEvents, &LIBID_VPinballLib>,
   public IEditable,
   public IHitable,
   public IRenderable,
   public IScriptable,
   public IFireEvents,
   public IPerPropertyBrowsing // Ability to fill in dropdown in property browser
{
public:
#ifdef VPX_MANUAL_SCRIPT_DISPATCH
   STDMETHOD(GetIDsOfNames)(REFIID /*riid*/, LPOLESTR* rgszNames, UINT cNames, LCID lcid,DISPID* rgDispId);
   STDMETHOD(Invoke)(DISPID dispIdMember, REFIID /*riid*/, LCID lcid, WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr);
   STDMETHOD(GetDocumentation)(MEMBERID index, BSTR *pBstrName, BSTR *pBstrDocString, DWORD *pdwHelpContext, BSTR *pBstrHelpFile);
   HRESULT FireDispID(const DISPID dispid, DISPPARAMS * const pdispparams) final;
#endif
   Ramp()
      : m_curve(this, 2)
   {
      m_curve.SetIndexShiftListener([this](int index, int delta) { ShiftSectionAnchors(index, delta); });
      m_d.m_collidable = true;
      m_d.m_visible = true;
      m_d.m_depthBias = 0.0f;
      m_d.m_wireDiameter = 6.0f;
      m_d.m_wireDistanceX = 38.0f;
      m_d.m_wireDistanceY = 88.0f;
   }
   virtual ~Ramp();

   BEGIN_COM_MAP(Ramp)
      COM_INTERFACE_ENTRY(IDispatch)
      COM_INTERFACE_ENTRY(IRamp)
      //COM_INTERFACE_ENTRY(ISupportErrorInfo)
      COM_INTERFACE_ENTRY_IMPL(IConnectionPointContainer)
      COM_INTERFACE_ENTRY(IPerPropertyBrowsing)
      COM_INTERFACE_ENTRY(IProvideClassInfo)
      COM_INTERFACE_ENTRY(IProvideClassInfo2)
   END_COM_MAP()

   BEGIN_CONNECTION_POINT_MAP(Ramp)
      CONNECTION_POINT_ENTRY(DIID_IRampEvents)
   END_CONNECTION_POINT_MAP()

   STANDARD_EDITABLE_DECLARES(Ramp, eItemRamp, RAMP)

   //DECLARE_NOT_AGGREGATABLE(Ramp)
   // Remove the comment from the line above if you don't want your object to
   // support aggregation.

   DECLARE_REGISTRY_RESOURCEID(IDR_RAMP)

   // ISupportsErrorInfo
   STDMETHOD(InterfaceSupportsErrorInfo)(REFIID riid);

   void ClearForOverwrite() final;

   void FlipY(const Vertex2D& pvCenter) final;
   void FlipX(const Vertex2D& pvCenter) final;
   void Rotate(const float ang, const Vertex2D &pvCenter, const bool useElementCenter) final;
   void Scale(const float scalex, const float scaley, const Vertex2D &pvCenter, const bool useElementCenter) final;
   void Translate(const Vertex2D &offset) final;

   Vertex2D GetCenter() const final { return m_curve.GetCenter(); }

   void GetBoundingVertices(vector<Vertex3Ds> &bounds, vector<Vertex3Ds> *const legacy_bounds) final;

   float GetDepth(const Vertex3Ds &viewDir) const final;
   void SetDefaultPhysics(const bool fromMouseClick) final;
   void ExportMesh(ObjLoader &loader) final;

   void WriteRegDefaults() final;

   void AddPoint(const Vertex2D &v, const bool smooth);

   float GetSurfaceHeight(float x, float y) const;
   bool IsHabitrail() const;

   // Computes the vertices and additional information for the ramp shape.
   // Also refreshes the drag points' m_calcHeight display cache (derived value, written through the owned DragPoint pointers)
   // ppWall (optional) receives, for a ramp built with sections, a per vertex flag: a wall segment exists between two consecutive
   // vertices only when both are flagged. It is set to nullptr when every segment has walls (all ramps without sections).
   Vertex2D *GetRampVertex(int &pcvertex, float **const ppheight, bool **const ppfCross, float **const ppratio, Vertex2D **const pMiddlePoints, const float _accuracy,
      const bool inc_width, bool **const ppWall = nullptr) const;

   // Sections are applied only to flat ramps; with any section, render and physics use the same fixed sampling
   bool HasActiveSections() const { return !m_d.m_sections.empty() && m_d.m_type == RampTypeFlat; }
   // Runs the section geometry builder and reports, per section, what it did
   vector<RampSectionStatus> GetSectionStatus() const;
   // Keeps section ranges on the same points when a point is inserted (delta +1) or deleted (delta -1) at index
   void ShiftSectionAnchors(int index, int delta);
   // Section creation (editor): false when refused (not a flat ramp, already present, not interior / consecutive / free points)
   bool CanAddEntryInterface() const;
   bool AddEntryInterface();
   bool CanAddHairpin(int first, int last) const;
   bool AddHairpin(int first, int last);

   RampData m_d;

   // The ramp center curve (drag points defining the ramp path)
   DragPointCurve m_curve;

private:
   Renderer *m_renderer = nullptr;

   int m_rampVertex;
   float *m_rgheightInit = nullptr;

   int m_numVertices = 0;      // this goes along with dynamicVertexBuffer
   int m_numIndices = 0;
   Vertex3D_NoTex2* m_vertBuffer = nullptr;
   Vertex3D_NoTex2* m_vertBuffer2 = nullptr;
   vector<WORD> m_meshIndices;
   std::shared_ptr<MeshBuffer> m_meshBuffer;
   std::shared_ptr<MeshBuffer> m_meshEdgeBuffer;
   bool m_dynamicVertexBufferRegenerate = false;

   vector<HitObject*> m_vhoCollidable; // Objects to that may be collide selectable

   // Get an approximation of the curve described by the control points of this ramp.
   template <typename T>
   void GetCentralCurve(vector<T> &vv, const float _accuracy = -1.f) const
   {
      float accuracy;

      // as solid ramps are rendered into the static buffer, always use maximum precision
      if (_accuracy != -1.f)
         accuracy = _accuracy; // used for hit shape calculation, always!
      else
      {
         const Material * const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
         if (!mat->m_bOpacityActive)
            accuracy = 10.f;
         else
            accuracy = (float)m_ptable->GetDetailLevel();
      }

      accuracy = 4.0f*powf(10.0f, (10.0f - accuracy)*(float)(1.0 / 1.5)); // min = 4 (highest accuracy/detail level), max = 4 * 10^(10/1.5) = ~18.000.000 (lowest accuracy/detail level)

      m_curve.GetRgVertex(vv, false, accuracy);
   }

   void PrepareHabitrail();

   struct SectionRows
   {
      vector<Vertex2D> right, left, middle; // right = middle + width/2 * normal (first half of GetRampVertex's outline)
      vector<float> height, ratio;
      vector<bool> cross, wall;
   };
   bool BuildSectionedRows(SectionRows &rows, vector<RampSectionStatus> *status) const;
   bool m_physicsSectioned = false; // set while PhysicSetup builds a sectioned ramp: zero length joints are skipped

   // Updates the m_calcHeight display cache of matching drag points (derived value, written through the owned DragPoint pointers)
   void AssignHeightToControlPoint(const RenderVertex3D &v, const float height) const;

   void AddJoint(class PhysicsEngine *physics, const Vertex3Ds &v1, const Vertex3Ds &v2, const bool isUI);
   void AddJoint2D(class PhysicsEngine *physics, const Vertex2D &p, const float zlow, const float zhigh, const bool isUI);
   void CheckJoint(class PhysicsEngine *physics, const HitTriangle *const ph3d1, const HitTriangle *const ph3d2, const bool isUI);
   void AddWallLineSeg(class PhysicsEngine* physics, const Vertex2D &pv1, const Vertex2D &pv2, const bool pv3_exists, const float height1, const float height2, const float wallheight, const bool isUI);
   Vertex3Ds AddWallQuad(class PhysicsEngine *physics, const Vertex2D &pv1, const float height1, const Vertex2D &pv2, const float height2, const float wallheight,
      const Vertex3Ds &prevNormal, const bool addStartJoint, const bool addEndJoint, const bool isUI);
   void SetupHitObject(class PhysicsEngine* physics, HitObject *obj, const bool isUI);

   void GenerateVertexBuffer();
   void CreateWire(const int numRings, const int numSegments, const Vertex2D * const midPoints, Vertex3D_NoTex2 * const rgvBuf) const;
   void GenerateWireMesh(Vertex3D_NoTex2 **meshBuf, Vertex3D_NoTex2 **meshBuf2);
   void GenerateRampMesh(Vertex3D_NoTex2 **meshBuf);

   Vertex3Ds m_boundingSphereCenter;
   void UpdateBounds();

   // IRamp
public:
   STDMETHOD(get_Elasticity)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_Elasticity)(/*[in]*/ float newVal);
   STDMETHOD(get_VisibleLeftWallHeight)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_VisibleLeftWallHeight)(/*[in]*/ float newVal);
   STDMETHOD(get_VisibleRightWallHeight)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_VisibleRightWallHeight)(/*[in]*/ float newVal);
   STDMETHOD(get_RightWallHeight)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_RightWallHeight)(/*[in]*/ float newVal);
   STDMETHOD(get_LeftWallHeight)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_LeftWallHeight)(/*[in]*/ float newVal);
   STDMETHOD(get_HasWallImage)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_HasWallImage)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ImageAlignment)(/*[out, retval]*/ RampImageAlignment *pVal);
   STDMETHOD(put_ImageAlignment)(/*[in]*/ RampImageAlignment newVal);
   STDMETHOD(get_Image)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_Image)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_Type)(/*[out, retval]*/ RampType *pVal);
   STDMETHOD(put_Type)(/*[in]*/ RampType newVal);
   STDMETHOD(get_Material)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_Material)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_WidthTop)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_WidthTop)(/*[in]*/ float newVal);
   STDMETHOD(get_WidthBottom)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_WidthBottom)(/*[in]*/ float newVal);
   STDMETHOD(get_HeightTop)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_HeightTop)(/*[in]*/ float newVal);
   STDMETHOD(get_HeightBottom)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_HeightBottom)(/*[in]*/ float newVal);

   STDMETHOD(get_HasHitEvent)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_HasHitEvent)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_Threshold)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_Threshold)(/*[in]*/ float newVal);
   STDMETHOD(get_Collidable)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_Collidable)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_Visible)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_Visible)(/*[in]*/ VARIANT_BOOL newVal);
   STDMETHOD(get_ReflectionEnabled)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_ReflectionEnabled)(/*[in]*/ VARIANT_BOOL newVal);

   STDMETHOD(get_Friction)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_Friction)(/*[in]*/ float newVal);
   STDMETHOD(get_Scatter)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_Scatter)(/*[in]*/ float newVal);

   STDMETHOD(get_DepthBias)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_DepthBias)(/*[in]*/ float newVal);
   STDMETHOD(get_WireDiameter)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_WireDiameter)(/*[in]*/ float newVal);
   STDMETHOD(get_WireDistanceX)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_WireDistanceX)(/*[in]*/ float newVal);
   STDMETHOD(get_WireDistanceY)(/*[out, retval]*/ float *pVal);
   STDMETHOD(put_WireDistanceY)(/*[in]*/ float newVal);
   STDMETHOD(get_PhysicsMaterial)(/*[out, retval]*/ BSTR *pVal);
   STDMETHOD(put_PhysicsMaterial)(/*[in]*/ BSTR newVal);
   STDMETHOD(get_OverwritePhysics)(/*[out, retval]*/ VARIANT_BOOL *pVal);
   STDMETHOD(put_OverwritePhysics)(/*[in]*/ VARIANT_BOOL newVal);
};
