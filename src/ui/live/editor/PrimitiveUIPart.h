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
   DragPointCurve* GetDragPointCurve() const override { return m_part->m_d.m_wireGen ? &m_part->m_wireCurve : nullptr; }
   float GetDragPointZ(const DragPoint* point) const override { return point->GetZ() + (m_part->m_d.m_wireGen ? m_part->m_d.m_wireHeight : 0.f); }

protected:
   void InsertPointOnCurve(const Vertex2D& pos) override;
   bool IsNewPointSmooth() const override { return true; }
   void UpdatePointZField(PropertyPane& props, DragPoint* point) override;

public:

   void UpdatePropertyPane(PropertyPane& props) override;

private:
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
