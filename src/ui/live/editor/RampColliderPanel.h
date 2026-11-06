// license:GPLv3+

#pragma once

class Primitive;

namespace VPX::EditorUI
{

class EditorUI;

// Ramp Collider (math/rampcollider.h): Add > Ramp Collider makes the collider of a ramp whose look is an imported part. The selected plain primitive becomes its
// Reference and the route starts along it; the host (the floor) and its <name>_Walls companion are made in one undo step. The Properties section and the
// overlay are PrimitiveUIPart members (RampColliderPanel.cpp).
class RampColliderTool final
{
public:
   static bool CanAdd(const EditorUI &ed);
   static void Add(EditorUI &ed); // One undo step, "Add Ramp Collider"
   static bool HiddenInPlayShown(const EditorUI &ed); // The 3D Viewport lets parts hidden in play be seen and picked
};

}
