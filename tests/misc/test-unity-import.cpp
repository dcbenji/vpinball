// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/pintable.h"
#include "parts/primitive.h"
#include "parts/Material.h"
#include "renderer/Texture.h"
#include "utils/UnityPrefabImporter.h"
#include "utils/UnityYaml.h"

#include <FreeImage.h>
#include <fstream>

#include "doctest.h"

namespace
{

void WriteFile(const std::filesystem::path& path, const string& content)
{
   std::filesystem::create_directories(path.parent_path());
   std::ofstream f(path, std::ios::binary | std::ios::trunc);
   f << content;
}

// Writes a solid color RGBA png through FreeImage
void WritePng(const std::filesystem::path& path, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
   std::filesystem::create_directories(path.parent_path());
   FIBITMAP* dib = FreeImage_AllocateT(FIT_BITMAP, 4, 4, 32, FI_RGBA_RED_MASK, FI_RGBA_GREEN_MASK, FI_RGBA_BLUE_MASK);
   REQUIRE(dib != nullptr);
   for (unsigned y = 0; y < 4; y++)
   {
      BYTE* row = FreeImage_GetScanLine(dib, y);
      for (unsigned x = 0; x < 4; x++, row += 4)
      {
         row[FI_RGBA_RED] = r;
         row[FI_RGBA_GREEN] = g;
         row[FI_RGBA_BLUE] = b;
         row[FI_RGBA_ALPHA] = a;
      }
   }
   FreeImage_Save(FIF_PNG, dib, PathToUTF8(path).c_str(), PNG_DEFAULT);
   FreeImage_Unload(dib);
}

// A minimal ASCII FBX: one "Cube" model node with a quad-mesh
const char* kCubeFbx = R"FBX(
FBXHeaderExtension: {
   FBXHeaderVersion: 1003
   FBXVersion: 7400
}
Objects: {
   Geometry: 1000001, "Geometry::", "Mesh" {
      Vertices: *24 {
         a: -0.5,-0.5,-0.5,0.5,-0.5,-0.5,0.5,0.5,-0.5,-0.5,0.5,-0.5,-0.5,-0.5,0.5,0.5,-0.5,0.5,0.5,0.5,0.5,-0.5,0.5,0.5
      }
      PolygonVertexIndex: *24 {
         a: 0,1,2,-4,4,5,6,-8,0,4,5,-2,1,5,6,-7,2,6,7,-4,3,7,4,-5
      }
      LayerElementUV: 0 {
         MappingInformationType: "ByPolygonVertex"
         ReferenceInformationType: "IndexToDirect"
         UV: *8 { a: 0,0,1,0,1,1,0,1 }
         UVIndex: *24 { a: 0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3 }
      }
   }
   Model: 1000002, "Model::Cube", "Mesh" {
      Version: 232
   }
}
Connections: {
   C: "OO",1000001,1000002
   C: "OO",1000002,0
}
)FBX";

} // namespace

TEST_CASE("UnityYaml")
{
   const string doc = R"YAML(
%YAML 1.1
%TAG !u! tag:unity3d.com,2011:
--- !u!1 &12345
GameObject:
  m_Name: Foo Bar (baz)
  m_IsActive: 1
  m_Component:
  - component: {fileID: 222, guid: abc, type: 3}
--- !u!4 &222
Transform:
  m_LocalPosition: {x: 1.5, y: -2, z: 3e2}
  m_Children: []
  m_Modifications:
  - target: {fileID: 1}
    propertyPath: m_LocalPosition.x
    value: 4.25
)YAML";
   const vector<UnityYaml::Document> docs = UnityYaml::Parse(doc);
   REQUIRE(docs.size() == 2);
   CHECK(docs[0].classId == 1);
   CHECK(docs[0].fileId == 12345);
   CHECK(docs[0].rootName == "GameObject");
   CHECK(docs[0].root["m_Name"].AsString() == "Foo Bar (baz)");
   UnityYaml::Ref r;
   CHECK(UnityYaml::AsRef(docs[0].root["m_Component"].At(0)["component"], r));
   CHECK(r.fileID == 222);
   CHECK(r.guid == "abc");
   float x, y, z;
   CHECK(UnityYaml::AsVec3(docs[1].root["m_LocalPosition"], x, y, z));
   CHECK(x == doctest::Approx(1.5f));
   CHECK(z == doctest::Approx(300.f));
   CHECK(docs[1].root["m_Modifications"].At(0)["value"].AsFloat() == doctest::Approx(4.25));
   CHECK(docs[1].root["m_Children"].IsList());
   // meta file (tagless)
   CHECK(UnityYaml::MetaGuid("fileFormatVersion: 2\nguid: 0123456789abcdef0123456789abcdef\nfoo: 1\n") == "0123456789abcdef0123456789abcdef");
}

TEST_CASE("Unity prefab import")
{
   // Synthesized Unity asset tree (ASCII FBX + YAML + PNG textures)
   const std::filesystem::path root = GetAssetPath() / "unity-fixture";
   std::filesystem::remove_all(root);
   const std::filesystem::path dir = root / "Assets" / "Test";

   const string fbxGuid = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1";
   const string matGuid = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb2";
   const string baseGuid = "ccccccccccccccccccccccccccccccc3";
   const string maskGuid = "ddddddddddddddddddddddddddddddd4";
   const string prefabGuid = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeee5";

   WriteFile(dir / "cube.fbx", kCubeFbx);
   WriteFile(dir / "cube.fbx.meta", "fileFormatVersion: 2\nguid: " + fbxGuid + "\n");
   WritePng(dir / "BaseMap.png", 200, 30, 30, 255);
   WriteFile(dir / "BaseMap.png.meta", "fileFormatVersion: 2\nguid: " + baseGuid + "\n");
   WritePng(dir / "MaskMap.png", 255, 128, 0, 204); // metal=1, ao=0.5, smooth=0.8
   WriteFile(dir / "MaskMap.png.meta", "fileFormatVersion: 2\nguid: " + maskGuid + "\n");
   WriteFile(dir / "test.mat",
      "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n"
      "--- !u!21 &2100000\n"
      "Material:\n"
      "  m_Name: testmat\n"
      "  m_ValidKeywords:\n"
      "  - _MASKMAP\n"
      "  m_SavedProperties:\n"
      "    m_TexEnvs:\n"
      "    - _BaseColorMap:\n"
      "        m_Texture: {fileID: 2800000, guid: "
         + baseGuid
         + ", type: 3}\n"
           "    - _MaskMap:\n"
           "        m_Texture: {fileID: 2800000, guid: "
         + maskGuid
         + ", type: 3}\n"
           "    m_Floats:\n"
           "    - _Metallic: 0.9\n"
           "    - _Smoothness: 0.8\n"
           "    - _SurfaceType: 0\n"
           "    m_Colors:\n"
           "    - _BaseColor: {r: 1, g: 0, b: 0, a: 1}\n");
   WriteFile(dir / "test.mat.meta", "fileFormatVersion: 2\nguid: " + matGuid + "\n");
   // fileID for mesh "Cube" = XXH64("Type:Mesh->Cube0") = -5495902117074765545
   WriteFile(dir / "test.prefab",
      "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n"
      "--- !u!1 &1001\n"
      "GameObject:\n"
      "  m_Component:\n"
      "  - component: {fileID: 1002}\n"
      "  - component: {fileID: 1003}\n"
      "  - component: {fileID: 1004}\n"
      "  m_Name: Cube\n"
      "  m_IsActive: 1\n"
      "--- !u!4 &1002\n"
      "Transform:\n"
      "  m_GameObject: {fileID: 1001}\n"
      "  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}\n"
      "  m_LocalPosition: {x: 0, y: 0, z: 0}\n"
      "  m_LocalScale: {x: 1, y: 1, z: 1}\n"
      "  m_Father: {fileID: 0}\n"
      "--- !u!33 &1003\n"
      "MeshFilter:\n"
      "  m_GameObject: {fileID: 1001}\n"
      "  m_Mesh: {fileID: -5495902117074765545, guid: "
         + fbxGuid
         + ", type: 3}\n"
           "--- !u!23 &1004\n"
           "MeshRenderer:\n"
           "  m_GameObject: {fileID: 1001}\n"
           "  m_Enabled: 1\n"
           "  m_Materials:\n"
           "  - {fileID: 2100000, guid: "
         + matGuid + ", type: 2}\n");
   WriteFile(dir / "test.prefab.meta", "fileFormatVersion: 2\nguid: " + prefabGuid + "\n");

   CComObject<PinTable>* table = nullptr;
   CComObject<PinTable>::CreateInstance(&table);
   REQUIRE(table != nullptr);
   table->AddRef();

   vector<IEditable*> created;
   const UnityImportResult res = UnityImportPrefab(table, dir / "test.prefab", nullptr, created);

   INFO("error: ", res.error);
   for (const string& w : res.warnings)
      MESSAGE("warning: ", w);
   REQUIRE(res.success);
   CHECK(res.parts == 1);
   CHECK(res.materials == 1);
   CHECK(res.images == 2); // base map + ORM (repacked from mask map)
   REQUIRE(created.size() == 1);

   Primitive* prim = static_cast<Primitive*>(created[0]);
   CHECK(prim->m_mesh.m_vertices.size() > 0);
   CHECK(prim->m_mesh.m_indices.size() > 0);
   CHECK(prim->m_d.m_szORMMap.empty() == false);
   CHECK(prim->m_d.m_szImage.empty() == false);
   // scale sanity: VPU magnitudes (1cm-ish cube => tens of VPU)
   const float m = prim->m_mesh.m_vertices[0].x;
   CHECK(fabsf(m) > 1.f);
   CHECK(fabsf(m) < 5000.f);

   Material* mat = table->GetMaterial(prim->m_d.m_szMaterial);
   REQUIRE(mat != nullptr);
   CHECK(mat->m_type == Material::PBR);
   CHECK(mat->m_fMetallic == doctest::Approx(1.f));
   CHECK(mat->m_fRoughness == doctest::Approx(1.f));
   CHECK(!mat->m_bOpacityActive);

   table->Release();
}
