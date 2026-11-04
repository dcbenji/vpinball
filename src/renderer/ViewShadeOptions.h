// license:GPLv3+

#pragma once

// View-only shading options of the live editor's 3D views (the Viewport Shading popover next to the shading icon row).
// One set per view (the main 3D Viewport, each secondary view). They only change how that view is drawn: nothing here is a table setting,
// and the defaults are exactly the look the views had before the options existed.
struct ViewShadeOptions
{
   // Solid: the studio environment (assets/library/_preview/env.hdr) is the only light
   float brightness = 1.f; // Level of the studio light (0 .. 4, 1 = as shipped)
   float lightAngle = 0.f; // Rotation of the studio environment about the table's vertical axis, degrees (-180 .. 180)
   float lightElevation = 0.f; // Raising of the studio environment about its horizontal axis, degrees (-90 .. 90)
   bool flat = false; // Lighting Flat (uniform light, the base colours as they are) instead of Studio
   bool bgCustom = false; // Background: custom colour instead of the theme's
   float bg[3] = { 13.f / 255.f, 13.f / 255.f, 13.f / 255.f }; // ... the colour (the theme's: 0x0D0D0D)
   bool backfaceCulling = false; // Faces seen from behind are not drawn, also for double sided parts
   bool cavity = false; // Ambient occlusion (the renderer's dynamic AO), off in Solid otherwise
   // Wireframe
   bool xray = false; // Secondary views only (the main view's is its NoDepthWireframe shade mode): wireframe seen through the parts
   bool wireCustom = false; // Wire colour: custom instead of black
   float wire[3] = { 0.f, 0.f, 0.f };
   // Overlay: parts hidden in play but shown in the editor (physics posts, walls, baked meshes) drawn as a grey fill with black edges over the scene, per mode
   bool hiddenWire = true; // Wireframe (as before the option existed)
   bool hiddenSolid = false; // Solid: only what the player renders
   bool hiddenPbr = false; // PBR: as the player renders it
   // PBR
   float envStrength = 1.f; // Level of the table's environment light in this view (0 .. 4, 1 = the table's)
   bool tableLights = true; // The table's lights (and their shadows) light this view

   bool operator==(const ViewShadeOptions &o) const
   {
      return brightness == o.brightness
         && lightAngle == o.lightAngle
         && lightElevation == o.lightElevation
         && flat == o.flat
         && bgCustom == o.bgCustom
         && bg[0] == o.bg[0]
         && bg[1] == o.bg[1]
         && bg[2] == o.bg[2]
         && backfaceCulling == o.backfaceCulling
         && cavity == o.cavity
         && xray == o.xray
         && wireCustom == o.wireCustom
         && wire[0] == o.wire[0]
         && wire[1] == o.wire[1]
         && wire[2] == o.wire[2]
         && envStrength == o.envStrength
         && tableLights == o.tableLights
         && hiddenWire == o.hiddenWire
         && hiddenSolid == o.hiddenSolid
         && hiddenPbr == o.hiddenPbr;
   }
   bool operator!=(const ViewShadeOptions &o) const { return !(*this == o); }
};
