#pragma once

#include "EditorUIPart.h"
#include "math/matrix.h"
#include "parts/primitive.h"

namespace VPX::EditorUI
{

class PrimitiveUIPart final : public EditableUIPart<Primitive>
{
public:
   explicit PrimitiveUIPart(Primitive* primitive);

   TransformMask GetTransform(Matrix3D& transform) override;
   void SetTransform(const vec3& pos, const vec3& scale, const vec3& rot) override;

   void RenderOverlay(const EditorRenderContext& ctx) override;

   // Metal Wire Guides (primitives with the wire generator on) are edited through their wire points, in table coordinates
   // Plastic hosts (Plastic from Atlas) are edited through their outline, or their holes while the Outline / Holes state says so
   DragPointCurve* GetDragPointCurve() const override
   {
      if (m_part->m_d.m_plasticGen)
         return m_part->m_plasticEditHoles ? &m_part->m_plasticHoles : &m_part->m_plasticCurve;
      return m_part->IsCurveHost() ? &m_part->m_wireCurve : nullptr;
   }
   float GetDragPointZ(const DragPoint* point) const override
   {
      if (m_part->m_d.m_plasticGen)
         return m_part->m_d.m_plasticHeight + m_part->m_d.m_plasticThickness;
      return point->GetZ() + (m_part->m_d.m_wireGen ? m_part->m_d.m_wireHeight : 0.f);
   }

protected:
   void InsertPointOnCurve(const Vertex2D& pos) override;
   bool HasPointSmooth() const override { return !(m_part->m_d.m_plasticGen && m_part->m_plasticEditHoles); }
   bool IsNewPointSmooth() const override { return !m_part->m_d.m_plasticGen; }
   void UpdatePointZField(PropertyPane& props, DragPoint* point) override;

public:

   void UpdatePropertyPane(PropertyPane& props) override;
   int GetRailFixture() const { return m_railFixture; } // The Wire Rail support selected in its list (-1 none)
   void SetRailFixture(const int i) { m_railFixture = i; }

   // Traced insert apply button: the fields a slider touched on this primitive (Primitive::m_tracedTouched), and their copy to another traced insert.
   // A look key is written alone in the target's traced_params, its other keys stay
   static vector<string> TouchedTracedLabels(const Primitive* primitive);
   static void CopyTouchedTraced(const Primitive* source, Primitive* target);

private:
   void UpdateRampColliderPanel(PropertyPane& props); // RampColliderPanel.cpp
   void RenderRampColliderOverlay(const EditorRenderContext& ctx);
   string m_rampMessage; // The outcome of the last Snap Heights to Reference
   void UpdateRailPanel(PropertyPane& props); // WireRailPanel.cpp
   void UpdateRailFixturePanel(PropertyPane& props);
   void RenderRailOverlay(const EditorRenderContext& ctx);
   int m_railWire = 0; // The wire selected in the cross-section
   bool m_railDrag = false;
   int m_railFixture = -1; // The support selected in the list of the Fixtures section

   void ImportMesh();
   void ExportMesh();
   void UpdateMeshUnitsUI();

   bool m_meshUnitsMeters = false;

   // Import option dialog state, mirroring the WinUI 'Wavefront OBJ Importer' dialog
   std::shared_ptr<string> m_pendingMeshImport;
   string m_meshImportFileName;
   bool m_meshImportAbsolutePosition = false;
   bool m_meshImportCenterMesh = false;
   bool m_meshImportMaterial = false;
   bool m_meshImportAnimation = false;
   bool m_meshImportNoForsyth = false;
   bool m_meshImportFailed = false;

   // Export option dialog state, mirroring the WinUI 'Wavefront OBJ Exporter' dialog
   std::shared_ptr<string> m_pendingMeshExport;
   string m_meshExportFileName;
   bool m_meshExportFailed = false;
};

}
