// license:GPLv3+

#include "core/stdafx.h"
#include "EditorChrome.h"
#include "SetOrigin.h"
#include "RubberBetweenPosts.h"
#include "IconsLucide.h"
#include "EditorTheme.h"
#include "PropertyPane.h"

#include "core/FileLocator.h"
#include "core/VPApp.h"
#include "renderer/Sampler.h"
#include "renderer/Texture.h"
#include "ui/EditorClipboard.h"
#include "ui/live/EditorUI.h"
#include "ui/live/LiveUI.h"
#include "parts/light.h"
#include "parts/primitive.h"
#include "parts/Collection.h"
#include "imgui/imgui_internal.h" // FindWindowByName (placement of the tool shelf relative to the Adjust Last Operation panel)

namespace VPX::EditorUI
{

namespace
{
constexpr float ToolButtonSize = 28.f; // Toolbar button (square), in pixels at 100% DPI
constexpr float ToolGroupGap = 8.f; // Gap between button groups (holds the 1 px separator)
constexpr float ToolSeparatorHeight = 16.f;
constexpr float StatusBarHeight = 22.f; // Bottom status bar, in pixels at 100% DPI
#ifdef __APPLE__
constexpr const char *ModKey = "Cmd"; // ImGui maps Cmd to Ctrl on macOS
#else
constexpr const char *ModKey = "Ctrl";
#endif
float s_toolDpi = 1.f;

// Toolbar icon button: transparent, hovered = Hover, active (mode on) = Fill (Blender's blue) background with a white icon
bool ToolButton(const char *icon, bool active = false)
{
   ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(active ? EditorTheme::Fill : 0u));
   ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(active ? EditorTheme::Rgb(85, 130, 190) : EditorTheme::Hover)); // An active tool stays blue under the mouse
   ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(active ? EditorTheme::Rgb(85, 130, 190) : EditorTheme::Hover));
   ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(active ? IM_COL32_WHITE : EditorTheme::Text));
   const bool pressed = ImGui::Button(icon, ImVec2(ToolButtonSize * s_toolDpi, ToolButtonSize * s_toolDpi));
   ImGui::PopStyleColor(4);
   return pressed;
}

// Text drawn over the render (view name, tool name): 1 px drop shadow so that it stays readable on light renders
void ShadowedText(const char *text)
{
   const ImVec2 pos = ImGui::GetCursorScreenPos();
   ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), EditorTheme::TextShadow, text);
   ImGui::TextUnformatted(text);
}

// Group separator: 1 px vertical line centered in the group gap
void ToolSeparator()
{
   const float half = 0.5f * (ToolGroupGap - 1.f) * s_toolDpi;
   ImGui::SameLine(0.f, half);
   const ImVec2 pos = ImGui::GetCursorScreenPos();
   const float inset = 0.5f * (ToolButtonSize - ToolSeparatorHeight) * s_toolDpi;
   ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + inset), ImVec2(pos.x, pos.y + inset + ToolSeparatorHeight * s_toolDpi), EditorTheme::Border, 1.f);
   ImGui::Dummy(ImVec2(1.f, ToolButtonSize * s_toolDpi));
   ImGui::SameLine(0.f, half);
}

// Chevron pointing down (open) or right (closed), drawn with lines
}

void EditorChrome::DrawChevron(ImDrawList *drawList, const ImVec2 &c, const ChevronDir dir, const float size, const ImU32 color)
{
   const float thickness = max(1.f, ImGui::GetFontSize() * 0.09f);
   const float h = size * 0.5f;
   if (dir == ChevronDir::Down)
   {
      drawList->AddLine(ImVec2(c.x - size, c.y - h), ImVec2(c.x, c.y + h), color, thickness);
      drawList->AddLine(ImVec2(c.x, c.y + h), ImVec2(c.x + size, c.y - h), color, thickness);
   }
   else
   {
      drawList->AddLine(ImVec2(c.x - h, c.y - size), ImVec2(c.x + h, c.y), color, thickness);
      drawList->AddLine(ImVec2(c.x + h, c.y), ImVec2(c.x - h, c.y + size), color, thickness);
   }
}

static float ShadeCaretWidth() { return 0.62f * ImGui::GetFrameHeight(); }

float EditorChrome::ShadeIconRowWidth(const bool withCaret)
{
   const float seam = std::max(1.f, ImGui::GetFrameHeight() / 22.f);
   return 3.f * ImGui::GetFrameHeight() * 1.1f + 2.f * seam + (withCaret ? seam + ShadeCaretWidth() : 0.f);
}

// The Viewport Shading popover: the view-only options of the shading mode (0 Wireframe, 1 Solid, 2 PBR). True if anything changed
static bool ShadeOptionsPopover(EditorUI *const editor, const int mode, ViewShadeOptions &o, bool &xray)
{
   static const ViewShadeOptions def;
   bool changed = false;
   const float fs = ImGui::GetFontSize();
   const ImVec4 dim = EditorTheme::V(EditorTheme::TextDim);
   const auto heading = [&](const char *text) {
      ImGui::PushStyleColor(ImGuiCol_Text, dim);
      ImGui::TextUnformatted(text);
      ImGui::PopStyleColor();
   };
   // Right click: Reset to Default for the last item
   const auto resetMenu = [&](auto reset) {
      if (ImGui::BeginPopupContextItem("##reset"))
      {
         if (ImGui::MenuItem("Reset to Default"))
         {
            reset();
            changed = true;
         }
         ImGui::EndPopup();
      }
   };
   // Joined segmented buttons (Blender's expanded enum), the active one blue
   const auto segmented = [&](const char *id, const char *const *labels, const int count, int &sel) {
      bool pressedAny = false;
      const float seam = std::max(1.f, fs / 14.f), w = (ImGui::GetContentRegionAvail().x - seam * (count - 1)) / count, h = ImGui::GetFrameHeight(), round = 0.22f * h;
      ImDrawList *const dl = ImGui::GetWindowDrawList();
      ImGui::PushID(id);
      for (int i = 0; i < count; i++)
      {
         if (i > 0)
            ImGui::SameLine(0.f, seam);
         ImGui::PushID(i);
         const bool pressed = ImGui::InvisibleButton("##seg", ImVec2(w, h));
         ImGui::PopID();
         const bool active = sel == i, hovered = ImGui::IsItemHovered();
         const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
         dl->AddRectFilled(a, b, active ? (hovered ? EditorTheme::FillActive : EditorTheme::Fill) : (hovered ? EditorTheme::Hover : EditorTheme::Raised), round,
            count == 1 ? ImDrawFlags_RoundCornersAll : i == 0 ? ImDrawFlags_RoundCornersLeft : i == count - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
         const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
         dl->AddText(ImVec2(0.5f * (a.x + b.x - ts.x), 0.5f * (a.y + b.y - ts.y)), ImGui::GetColorU32(ImGuiCol_Text), labels[i]);
         if (pressed && !active)
         {
            sel = i;
            pressedAny = true;
         }
      }
      ImGui::PopID();
      return pressedAny;
   };
   const auto checkbox = [&](const char *id, const char *label, bool &value, const bool def, const char *unavailable = nullptr) {
      ImGui::PushID(id);
      ImGui::BeginDisabled(unavailable != nullptr); // The renderer does not have what this option drives: shown disabled, with the reason
      bool toggled = PropertyPane::FlatCheckbox("##cb", &value);
      ImGui::SameLine();
      ImGui::TextUnformatted(label);
      if (ImGui::IsItemClicked() && unavailable == nullptr)
      {
         value = !value;
         toggled = true;
      }
      ImGui::EndDisabled();
      if (unavailable != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("%s", unavailable);
      resetMenu([&]() { value = def; });
      ImGui::PopID();
      changed |= toggled;
   };
   const auto slider = [&](const char *id, float &value, const float lo, const float hi, const char *format, const float def, const char *unavailable = nullptr) {
      ImGui::PushID(id);
      ImGui::BeginDisabled(unavailable != nullptr); // The renderer does not have what this slider drives: shown disabled, with the reason
      ImGui::SetNextItemWidth(-FLT_MIN);
      changed |= PropertyPane::FillSlider("##sl", &value, lo, hi, format);
      value = std::clamp(value, lo, hi);
      ImGui::EndDisabled();
      if (unavailable != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("%s", unavailable);
      resetMenu([&]() { value = def; });
      ImGui::PopID();
   };
   // The environment turn is a BGFX shader uniform: the GL and DX9 shaders do not have it
   // Outline and Shadow ride the AO shader's aoFx / aoLightDir uniforms: BGFX, but not its stereo / VR variants
   const char *const noScreenFx = g_pplayer != nullptr && g_pplayer->m_renderer != nullptr && !g_pplayer->m_renderer->HasScreenSpaceFx() ? "Not available with this renderer (the AO shader has no such effect, e.g. GL, DX9, stereo / VR)" : nullptr;
   const char *const noSolidShadow = g_pplayer != nullptr && g_pplayer->m_renderer != nullptr && !g_pplayer->m_renderer->HasSolidShadow() ? "Not available with this renderer (the AO shader has no such effect, e.g. GL, DX9, stereo / VR)" : nullptr;
   const char *const noEnvRotation = g_pplayer != nullptr && g_pplayer->m_renderer != nullptr && !g_pplayer->m_renderer->HasEnvRotation() ? "Not available with this renderer (the shaders have no environment rotation)" : nullptr;
   // Theme / Custom colour: the swatch opens the picker, picking a colour makes it Custom
   const auto themeOrCustom = [&](const char *id, bool &custom, float (&rgb)[3]) {
      static const char *const labels[2] = { "Theme", "Custom" };
      int sel = custom ? 1 : 0;
      if (segmented(id, labels, 2, sel))
      {
         custom = sel == 1;
         changed = true;
      }
      if (custom)
      {
         ImGui::PushID(id);
         if (ImGui::ColorButton("##swatch", ImVec4(rgb[0], rgb[1], rgb[2], 1.f), ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoBorder, ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight())))
            ImGui::OpenPopup("##picker");
         if (ImGui::BeginPopup("##picker"))
         {
            changed |= ImGui::ColorPicker3("##rgb", rgb, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_Float);
            ImGui::EndPopup();
         }
         ImGui::PopID();
      }
   };

   // Blender's order and names: Lighting, Color (Wireframe colour, Background), Options. Sections collapse
   const auto section = [&](const char *label) {
      ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.06f));
      ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1, 1, 1, 0.1f));
      ImGui::PushStyleColor(ImGuiCol_Text, dim);
      const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
      ImGui::PopStyleColor(4);
      return open;
   };
   ImGui::PushStyleColor(ImGuiCol_Text, dim);
   ImGui::TextUnformatted("Viewport Shading");
   ImGui::PopStyleColor();
   if (mode == 1)
   {
      if (section("Lighting"))
      {
         static const char *const lighting[2] = { "Studio", "Flat" };
         int sel = o.flat ? 1 : 0;
         if (segmented("lighting", lighting, 2, sel))
         {
            o.flat = sel == 1;
            changed = true;
         }
         slider("brightness", o.brightness, 0.f, 4.f, "Brightness  %.2f", def.brightness);
         ImGui::BeginDisabled(o.flat); // Flat has no direction
         slider("direction", o.lightAngle, -180.f, 180.f, "Light Direction  %.0f deg", def.lightAngle, noEnvRotation);
         slider("elevation", o.lightElevation, -90.f, 90.f, "Light Elevation  %.0f deg", def.lightElevation, noEnvRotation);
         ImGui::EndDisabled();
      }
      if (section("Color"))
      {
         heading("Background");
         themeOrCustom("background", o.bgCustom, o.bg);
      }
      if (section("Options"))
      {
         checkbox("cull", "Backface Culling", o.backfaceCulling, def.backfaceCulling);
         checkbox("cavity", "Cavity", o.cavity, def.cavity);
         checkbox("outline", "Outline", o.outline, def.outline, noScreenFx);
         checkbox("shadow", "Shadow", o.shadow, def.shadow, noSolidShadow);
         checkbox("xraySolid", "X-Ray", o.xraySolid, def.xraySolid);
         if (o.xraySolid)
            slider("xrayAlpha", o.xrayAlpha, 0.f, 1.f, "Opacity  %.2f", def.xrayAlpha);
         checkbox("hidden", "Hidden in Play", o.hiddenSolid, def.hiddenSolid);
      }
   }
   else if (mode == 0)
   {
      if (section("Color"))
      {
         heading("Wireframe");
         themeOrCustom("wire", o.wireCustom, o.wire);
         heading("Background");
         themeOrCustom("background", o.bgCustom, o.bg);
      }
      if (section("Options"))
      {
         checkbox("xray", "X-Ray", xray, false);
         checkbox("hidden", "Hidden in Play", o.hiddenWire, def.hiddenWire);
      }
   }
   else
   {
      if (section("Lighting"))
      {
         checkbox("lights", "Table Lights", o.tableLights, def.tableLights);
         slider("strength", o.envStrength, 0.f, 4.f, "Environment Strength  %.2f", def.envStrength);
         slider("rotation", o.envAngle, -180.f, 180.f, "Environment Rotation  %.0f deg", def.envAngle, noEnvRotation);
      }
      if (section("Options"))
         checkbox("hidden", "Hidden in Play", o.hiddenPbr, def.hiddenPbr);
   }
   ImGui::Spacing();
   if (ImGui::Button("Reset", ImVec2(-FLT_MIN, 0.f)))
   {
      if (mode == 1)
      {
         o.brightness = def.brightness;
         o.lightAngle = def.lightAngle;
         o.lightElevation = def.lightElevation;
         o.flat = def.flat;
         o.bgCustom = def.bgCustom;
         std::copy(def.bg, def.bg + 3, o.bg);
         o.backfaceCulling = def.backfaceCulling;
         o.cavity = def.cavity;
         o.outline = def.outline;
         o.shadow = def.shadow;
         o.xraySolid = def.xraySolid;
         o.xrayAlpha = def.xrayAlpha;
         o.hiddenSolid = def.hiddenSolid;
      }
      else if (mode == 0)
      {
         xray = false;
         o.bgCustom = def.bgCustom;
         std::copy(def.bg, def.bg + 3, o.bg);
         o.wireCustom = def.wireCustom;
         o.hiddenWire = def.hiddenWire;
         std::copy(def.wire, def.wire + 3, o.wire);
      }
      else
      {
         o.envStrength = def.envStrength;
         o.envAngle = def.envAngle;
         o.tableLights = def.tableLights;
         o.hiddenPbr = def.hiddenPbr;
      }
      changed = true;
   }
   ImGui::SetItemTooltip("Reset this mode's options to their defaults");
   return changed;
}

bool EditorChrome::ShadeIconRow(const char *id, int &mode, ViewShadeOptions *const options, EditorUI *const editor, bool *const xray)
{
   // Blender header look: 1.1 x 1 segments joined by a seam, rounded at the row ends only, idle Raised, active Fill; the spheres share one outer size
   static constexpr const char *names[3] = { "Wireframe", "Solid", "PBR" };
   static constexpr const char *descs[3] = { "The editor's hidden line wireframe", "PBR with the table lights, shadows, refraction, AO and bloom off", "Full shading, as the player renders it" };
   const float h = ImGui::GetFrameHeight(), w = h * 1.1f, seam = std::max(1.f, h / 22.f), round = 0.22f * h, th = std::max(1.f, h / 18.f), r = 0.34f * h;
   ImDrawList *const dl = ImGui::GetWindowDrawList();
   bool changed = false;
   ImGui::PushID(id);
   for (int i = 0; i < 3; i++)
   {
      if (i > 0)
         ImGui::SameLine(0.f, seam);
      ImGui::PushID(i);
      const bool pressed = ImGui::InvisibleButton("##shade", ImVec2(w, h));
      ImGui::PopID();
      const bool active = mode == i, hovered = ImGui::IsItemHovered();
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      dl->AddRectFilled(a, b, active ? (hovered ? EditorTheme::FillActive : EditorTheme::Fill) : (hovered ? EditorTheme::Hover : EditorTheme::Raised), round,
         i == 0 ? ImDrawFlags_RoundCornersLeft : (i == 2 && options == nullptr) ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
      const ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
      const auto tint = [&](const float k, const float alpha) { return ImGui::GetColorU32(ImVec4(tc.x * k, tc.y * k, tc.z * k, tc.w * alpha)); };
      const ImU32 col = active ? tint(1.f, 1.f) : tint(0.85f, 0.8f);
      const ImVec2 c(0.5f * (a.x + b.x), 0.5f * (a.y + b.y));
      if (i == 0)
      { // Circle, a meridian ellipse and the front halves of three parallels
         dl->AddCircle(c, r, col, 0, th);
         dl->AddEllipse(c, ImVec2(0.45f * r, r), col, 0.f, 0, th * 0.8f);
         for (int k = -1; k <= 1; k++)
         {
            const float y = 0.55f * r * k, half = sqrtf(r * r - y * y);
            for (int s = 0; s <= 12; s++)
            {
               const float t = IM_PI * s / 12.f;
               dl->PathLineTo(ImVec2(c.x + half * cosf(t), c.y + y + 0.25f * r * sinf(t)));
            }
            dl->PathStroke(col, 0, th * 0.8f);
         }
      }
      else if (i == 1)
      { // Flat disc, lighter toward the upper left in a few soft steps
         dl->AddCircleFilled(c, r, tint(0.8f, 1.f));
         for (int s = 0; s < 3; s++)
            dl->AddCircleFilled(ImVec2(c.x - 0.28f * r, c.y - 0.28f * r), r * (0.62f - 0.17f * s), tint(1.f, 0.35f));
      }
      else
      { // Ball: a smooth shade from the lit upper left to the dark lower right (stacked anti-aliased discs, each tangent to the rim at the upper left), small highlight, thin rim
         static constexpr int steps = 12;
         dl->AddCircleFilled(c, r, tint(0.32f, 1.f), 0);
         for (int s = 1; s <= steps; s++)
         {
            const float t = static_cast<float>(s) / steps, rad = r * (1.f - 0.78f * t), off = (r - rad) * 0.7071f;
            dl->AddCircleFilled(ImVec2(c.x - off, c.y - off), rad, tint(0.32f + 0.62f * t, 1.f), 0);
         }
         dl->AddCircleFilled(ImVec2(c.x - 0.36f * r, c.y - 0.38f * r), 0.12f * r + 0.3f, IM_COL32(255, 255, 255, 230));
         dl->AddCircle(c, r, col, 0, th * 0.8f);
      }
      Tip(names[i], xray != nullptr ? "Z" : nullptr, descs[i]); // Z cycles the 3D Viewport only
      if (pressed && !active)
      {
         mode = i;
         changed = true;
      }
   }
   if (options != nullptr)
   {
      // Blender's caret: a small joined button after the icons, opening the popover of the mode
      ImGui::SameLine(0.f, seam);
      const bool open = ImGui::IsPopupOpen("##ShadeOptions");
      const bool pressed = ImGui::InvisibleButton("##caret", ImVec2(ShadeCaretWidth(), h));
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      dl->AddRectFilled(a, b, open ? EditorTheme::Fill : (hovered ? EditorTheme::Hover : EditorTheme::Raised), round, ImDrawFlags_RoundCornersRight);
      const ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
      DrawChevron(dl, ImVec2(0.5f * (a.x + b.x), 0.5f * (a.y + b.y)), ChevronDir::Down, 0.14f * h, ImGui::GetColorU32(ImVec4(tc.x, tc.y, tc.z, open ? 1.f : 0.8f)));
      Tip("Viewport Shading", nullptr, "Options of the shading mode");
      if (pressed)
      {
         if (open)
            ImGui::CloseCurrentPopup();
         else
            ImGui::OpenPopup("##ShadeOptions");
      }
      ImGui::SetNextWindowPos(ImVec2(b.x, b.y + 2.f), ImGuiCond_Always, ImVec2(1.f, 0.f));
      ImGui::SetNextWindowSizeConstraints(ImVec2(15.f * ImGui::GetFontSize(), 0.f), ImVec2(15.f * ImGui::GetFontSize(), FLT_MAX));
      if (ImGui::BeginPopup("##ShadeOptions"))
      {
         if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
         {
            ImGui::CloseCurrentPopup();
            if (editor != nullptr)
               editor->m_areaEscPending = true; // The Esc that closed the popover is not an editor Esc (it would deselect)
         }
         bool localXray = options->xray;
         bool &x = xray != nullptr ? *xray : localXray;
         if (ShadeOptionsPopover(editor, mode, *options, x))
         {
            if (xray == nullptr)
               options->xray = localXray;
         }
         ImGui::EndPopup();
      }
   }
   ImGui::PopID();
   return changed;
}

// Icons of the transform dropdowns: our own vector drawings of what Blender's show, in a box of half size s around c
void EditorChrome::DrawPivotIcon(ImDrawList *const dl, const ImVec2 &c, const float s, const int pivot, const ImU32 col, const ImU32 dim)
{
   const float th = std::max(1.f, s / 8.f), rr = 0.42f * s, dot = 0.17f * s;
   const ImVec2 a(c.x - 0.32f * s, c.y + 0.32f * s), b(c.x + 0.3f * s, c.y - 0.3f * s); // The two rings: lower left, upper right
   const auto ring = [&](const ImVec2 &at, const float gapAt, const bool whole, const ImU32 color)
   {
      if (whole)
         dl->AddCircle(at, rr, color, 0, th);
      else
      {
         dl->PathArcTo(at, rr, gapAt + 0.9f, gapAt + 2.f * IM_PI - 0.9f, 20);
         dl->PathStroke(color, 0, th);
      }
   };
   const float towardsA = 0.75f * IM_PI, towardsB = -0.25f * IM_PI; // Screen angles (Y down) of the side of a ring which faces the other one
   switch (static_cast<EditorUI::TransformPivot>(pivot))
   {
   case EditorUI::TransformPivot::BoundingBoxCenter:
   {
      const float e = 0.78f * s, l = 0.34f * s;
      for (const float sx : { -1.f, 1.f })
         for (const float sy : { -1.f, 1.f })
         {
            const ImVec2 corner(c.x + sx * e, c.y + sy * e);
            dl->AddLine(corner, ImVec2(corner.x - sx * l, corner.y), col, th);
            dl->AddLine(corner, ImVec2(corner.x, corner.y - sy * l), col, th);
         }
      dl->AddCircleFilled(c, 1.3f * dot, col);
      break;
   }
   case EditorUI::TransformPivot::IndividualOrigins:
      ring(a, towardsB, true, col);
      dl->AddCircleFilled(a, dot, col);
      ring(b, towardsA, false, col);
      dl->AddCircleFilled(b, dot, col);
      break;
   case EditorUI::TransformPivot::MedianPoint:
      ring(a, towardsB, false, col);
      ring(b, towardsA, false, col);
      dl->AddCircleFilled(ImVec2(0.5f * (a.x + b.x), 0.5f * (a.y + b.y)), dot, col);
      break;
   case EditorUI::TransformPivot::ActiveElement:
      ring(a, towardsB, true, col);
      dl->AddCircleFilled(a, dot, col);
      ring(b, towardsA, false, dim);
      break;
   }
}

// The orientation axes, after Blender's icons: a small origin ring with arrows from it
void EditorChrome::DrawOrientationIcon(ImDrawList *const dl, const ImVec2 &c, const float s, const int orientation, const ImU32 col, const ImU32 dim)
{
   const float th = std::max(1.f, s / 8.f), ring = 0.2f * s, head = 0.3f * s;
   const auto arrow = [&](const ImVec2 &from, const ImVec2 &to, const ImU32 color)
   {
      const float dx = to.x - from.x, dy = to.y - from.y, len = std::max(hypotf(dx, dy), 1e-3f), ux = dx / len, uy = dy / len;
      dl->AddLine(ImVec2(from.x + ux * ring, from.y + uy * ring), to, color, th); // From the ring's edge
      dl->AddLine(to, ImVec2(to.x - head * (ux - 0.7f * uy), to.y - head * (uy + 0.7f * ux)), color, th);
      dl->AddLine(to, ImVec2(to.x - head * (ux + 0.7f * uy), to.y - head * (uy - 0.7f * ux)), color, th);
   };
   switch (static_cast<EditorUI::TransformOrientation>(orientation))
   {
   case EditorUI::TransformOrientation::Global:
   {
      const ImVec2 o(c.x - 0.6f * s, c.y + 0.6f * s);
      arrow(o, ImVec2(o.x, c.y - 0.9f * s), col);
      arrow(o, ImVec2(c.x + 0.9f * s, o.y), col);
      arrow(o, ImVec2(c.x + 0.55f * s, c.y - 0.55f * s), dim);
      dl->AddCircle(o, ring, col, 0, th);
      break;
   }
   case EditorUI::TransformOrientation::Local:
   {
      const ImVec2 o(c.x + 0.05f * s, c.y);
      arrow(o, ImVec2(c.x - 0.9f * s, c.y), col);
      arrow(o, ImVec2(c.x + 0.75f * s, c.y - 0.75f * s), col);
      arrow(o, ImVec2(c.x + 0.7f * s, c.y + 0.7f * s), dim);
      dl->AddCircle(o, ring, col, 0, th);
      break;
   }
   case EditorUI::TransformOrientation::Normal:
   {
      // A plane seen at an angle, its normal out of it
      dl->AddQuad(ImVec2(c.x - 0.85f * s, c.y - 0.55f * s), ImVec2(c.x - 0.1f * s, c.y - 0.85f * s), ImVec2(c.x - 0.1f * s, c.y + 0.6f * s), ImVec2(c.x - 0.85f * s, c.y + 0.9f * s),
         dim, th);
      const ImVec2 o(c.x - 0.4f * s, c.y);
      arrow(o, ImVec2(c.x + 0.9f * s, c.y), col);
      dl->AddCircle(o, ring, col, 0, th);
      break;
   }
   case EditorUI::TransformOrientation::View:
   {
      const ImVec2 o(c.x - 0.6f * s, c.y + 0.6f * s);
      arrow(o, ImVec2(o.x, c.y - 0.9f * s), col);
      arrow(o, ImVec2(c.x + 0.9f * s, o.y), col);
      dl->AddCircle(o, ring, col, 0, th);
      break;
   }
   }
}

// Orientation (icon, name, caret), then the pivot (icon, caret), as in Blender's header
float EditorChrome::TransformOrientationWidth()
{
   const float h = ImGui::GetFrameHeight();
   return 1.25f * h + ImGui::CalcTextSize("Normal").x + 0.85f * h;
}

float EditorChrome::TransformHeaderRowWidth() { return TransformOrientationWidth() + ImGui::GetStyle().ItemSpacing.x + 1.75f * ImGui::GetFrameHeight(); }

void EditorChrome::TransformHeaderRow(const char *id, EditorUI &editor)
{
   struct PivotItem
   {
      EditorUI::TransformPivot pivot;
      const char *name, *desc;
   };
   static constexpr PivotItem items[] = {
      { EditorUI::TransformPivot::BoundingBoxCenter, "Bounding Box Center", "Rotate and scale about the center of the box around the origins of the selected parts" },
      { EditorUI::TransformPivot::IndividualOrigins, "Individual Origins", "Each part rotates and scales about its own origin, staying where it is" },
      { EditorUI::TransformPivot::MedianPoint, "Median Point", "Rotate and scale about the average of the origins of the selected parts" },
      { EditorUI::TransformPivot::ActiveElement, "Active Element", "Rotate and scale about the origin of the active part" },
   };
   const float h = ImGui::GetFrameHeight(), round = 0.22f * h;
   ImDrawList *const dl = ImGui::GetWindowDrawList();
   const ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
   const ImU32 col = ImGui::GetColorU32(tc), dim = ImGui::GetColorU32(ImVec4(tc.x, tc.y, tc.z, tc.w * 0.5f));
   ImGui::PushID(id);
   {
      struct OrientationItem
      {
         EditorUI::TransformOrientation orientation;
         const char *name, *desc;
      };
      static constexpr OrientationItem orientations[] = {
         { EditorUI::TransformOrientation::Global, "Global", "Align the transformation axes to the table's (X right, Y towards the player, Z up)" },
         { EditorUI::TransformOrientation::Local, "Local", "Align the transformation axes to each selected part's own; several parts each move, turn and scale along their own" },
         { EditorUI::TransformOrientation::Normal, "Normal", "As Local for parts; Global when editing points (the editor has no surface normal)" },
         { EditorUI::TransformOrientation::View, "View", "Align the transformation axes to the view: X right, Y up, Z into the screen" },
      };
      const int current = static_cast<int>(editor.m_transformOrientation);
      const bool open = ImGui::IsPopupOpen("##OrientationMenu");
      const bool pressed = ImGui::InvisibleButton("##orientation", ImVec2(TransformOrientationWidth(), h));
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      dl->AddRectFilled(a, b, open ? EditorTheme::Fill : (hovered ? EditorTheme::Hover : EditorTheme::Raised), round);
      DrawOrientationIcon(dl, ImVec2(a.x + 0.6f * h, 0.5f * (a.y + b.y)), 0.36f * h, current, col, dim);
      dl->AddText(ImVec2(a.x + 1.25f * h, a.y + 0.5f * (h - ImGui::GetFontSize())), col, orientations[current].name);
      DrawChevron(dl, ImVec2(b.x - 0.38f * h, 0.5f * (a.y + b.y)), ChevronDir::Down, 0.14f * h, ImGui::GetColorU32(ImVec4(tc.x, tc.y, tc.z, open ? 1.f : 0.8f)));
      Tip("Transform Orientation", nullptr, "The axes of X / Y / Z in G / R / S and of the gizmo. Repeating G / R / S in the gizmo flips Global and Local");
      if (pressed && !open)
         ImGui::OpenPopup("##OrientationMenu");
      ImGui::SetNextWindowPos(ImVec2(a.x, b.y + 2.f));
      if (ImGui::BeginPopup("##OrientationMenu"))
      {
         if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
         {
            ImGui::CloseCurrentPopup();
            editor.m_areaEscPending = true; // The Esc that closed the menu is not an editor Esc (it would deselect)
         }
         ImGui::TextDisabled("Transform Orientation");
         ImGui::Separator();
         ImGui::PushStyleColor(ImGuiCol_Header, EditorTheme::V(EditorTheme::Fill));
         const float rowWidth = 12.f * ImGui::GetFontSize();
         for (const OrientationItem &item : orientations)
         {
            ImGui::PushID(static_cast<int>(item.orientation));
            if (ImGui::Selectable("##row", editor.m_transformOrientation == item.orientation, ImGuiSelectableFlags_None, ImVec2(rowWidth, h)))
               editor.SetTransformOrientation(item.orientation);
            const ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
            ImDrawList *const popupList = ImGui::GetWindowDrawList();
            DrawOrientationIcon(popupList, ImVec2(ra.x + 0.6f * h, 0.5f * (ra.y + rb.y)), 0.36f * h, static_cast<int>(item.orientation), col, dim);
            popupList->AddText(ImVec2(ra.x + 1.3f * h, ra.y + 0.5f * (h - ImGui::GetFontSize())), col, item.name);
            Tip(item.name, nullptr, item.desc);
            ImGui::PopID();
         }
         ImGui::PopStyleColor();
         ImGui::EndPopup();
      }
   }
   ImGui::SameLine();
   {
      const bool open = ImGui::IsPopupOpen("##PivotMenu");
      const bool pressed = ImGui::InvisibleButton("##pivot", ImVec2(1.75f * h, h));
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      dl->AddRectFilled(a, b, open ? EditorTheme::Fill : (hovered ? EditorTheme::Hover : EditorTheme::Raised), round);
      DrawPivotIcon(dl, ImVec2(a.x + 0.55f * h, 0.5f * (a.y + b.y)), 0.36f * h, static_cast<int>(editor.m_transformPivot), col, dim);
      DrawChevron(dl, ImVec2(b.x - 0.38f * h, 0.5f * (a.y + b.y)), ChevronDir::Down, 0.14f * h, ImGui::GetColorU32(ImVec4(tc.x, tc.y, tc.z, open ? 1.f : 0.8f)));
      Tip("Transform Pivot Point", nullptr, "Where R and S happen on a selection");
      if (pressed && !open)
         ImGui::OpenPopup("##PivotMenu");
      ImGui::SetNextWindowPos(ImVec2(a.x, b.y + 2.f));
      if (ImGui::BeginPopup("##PivotMenu"))
      {
         if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
         {
            ImGui::CloseCurrentPopup();
            editor.m_areaEscPending = true; // The Esc that closed the menu is not an editor Esc (it would deselect)
         }
         ImGui::TextDisabled("Transform Pivot Point");
         ImGui::Separator();
         ImGui::PushStyleColor(ImGuiCol_Header, EditorTheme::V(EditorTheme::Fill));
         const float rowWidth = 15.f * ImGui::GetFontSize();
         for (const PivotItem &item : items)
         {
            ImGui::PushID(static_cast<int>(item.pivot));
            const bool selected = editor.m_transformPivot == item.pivot;
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(rowWidth, h)))
               editor.SetTransformPivot(item.pivot);
            const ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
            ImDrawList *const popupList = ImGui::GetWindowDrawList();
            DrawPivotIcon(popupList, ImVec2(ra.x + 0.6f * h, 0.5f * (ra.y + rb.y)), 0.36f * h, static_cast<int>(item.pivot), col, dim);
            popupList->AddText(ImVec2(ra.x + 1.3f * h, ra.y + 0.5f * (h - ImGui::GetFontSize())), col, item.name);
            Tip(item.name, nullptr, item.desc);
            ImGui::PopID();
         }
         ImGui::PopStyleColor();
         ImGui::EndPopup();
      }
   }
   ImGui::PopID();
}

void EditorChrome::Tip(const char *title, const char *shortcut, const char *description)
{
   // Own hover delay: ImGui's delay timers do not run on disabled items, which made their tooltips appear much later than the enabled ones
   static ImGuiID s_hoverId = 0;
   static double s_hoverSince = 0.0;
   const ImGuiID id = ImGui::GetItemID();
   if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_Stationary))
   {
      if (s_hoverId == id)
         s_hoverId = 0;
      return;
   }
   if (s_hoverId != id)
   {
      s_hoverId = id;
      s_hoverSince = ImGui::GetTime();
   }
   if (ImGui::GetTime() - s_hoverSince < ImGui::GetStyle().HoverDelayShort)
      return;
   if (!ImGui::BeginTooltip())
      return;
   ImGui::TextUnformatted(title);
   if (shortcut != nullptr && shortcut[0] != '\0')
   {
      const float padX = ImGui::GetStyle().FramePadding.x;
      ImGui::SameLine(0.f, 2.f * padX + 6.f);
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      const ImVec2 size = ImGui::CalcTextSize(shortcut);
      const ImVec2 chipMin(pos.x - padX, pos.y - 1.f), chipMax(pos.x + size.x + padX, pos.y + size.y + 1.f);
      ImDrawList *const drawList = ImGui::GetWindowDrawList();
      drawList->AddRectFilled(chipMin, chipMax, EditorTheme::Rgb(255, 255, 255, 20), 3.f);
      drawList->AddRect(chipMin, chipMax, EditorTheme::Hover, 3.f);
      drawList->AddText(pos, EditorTheme::TextDim, shortcut);
      ImGui::Dummy(size);
   }
   if (description != nullptr && description[0] != '\0')
   {
      ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
      ImGui::TextUnformatted(description);
      ImGui::PopStyleColor();
   }
   ImGui::EndTooltip();
}

namespace
{
// Edit mode selection (same actions as the Esc, G, R and S keyboard shortcuts), shown in the tool shelf or in the top toolbar when the shelf is hidden
const struct
{
   ImGuizmo::OPERATION operation;
   const char *icon;
   const char *title;
   const char *shortcut;
} editModes[] = {
   { (ImGuizmo::OPERATION)0, ICON_LC_MOUSE_POINTER_2, "Select", "Esc" }, //
   { ImGuizmo::TRANSLATE, ICON_LC_MOVE, "Move", "G" }, //
   { ImGuizmo::ROTATE, ICON_LC_ROTATE_3D, "Rotate", "R" }, //
   { ImGuizmo::SCALE, ICON_LC_SCALE_3D, "Scale", "S" }, //
};
}

void EditorChrome::Render(bool showFullUI)
{
   // Gives some transparency when positioning camera to better view camera view bounds
   // TODO for some reasons, this breaks the modal background behavior
   //SetupImGuiStyle(m_editor.m_selection.GetType() == Selection::S_CAMERA ? 0.3f : 1.0f);

   m_menuBarHeight = 0.0f;
   m_toolBarHeight = showFullUI ? (ToolButtonSize + 4.f) * m_editor.m_liveUI.GetDPI() + 1.f : 0.f; // + 1 px separator at the bottom, panels start below it
   m_statusBarHeight = (showFullUI && m_showStatusBar) ? StatusBarHeight * m_editor.m_liveUI.GetDPI() : 0.f;

   if (!showFullUI)
   {
      m_toolShelfDrawn = false;
      return;
   }

   RenderMenuBar();
   RenderToolbar();
   if (m_editor.IsViewportVisible()) // No visible viewport (maximized panel): nothing to overlay
      RenderStatusOverlay();
   RenderUndoHistory();
}

void EditorChrome::RenderMenuBar()
{
   EditorUI &editor = m_editor;
   const float dpi = editor.m_liveUI.GetDPI();
   // The menu bar is tall enough for the workspace tabs, and shares the toolbar background
   constexpr float menuBarHeight = 26.f;
   constexpr float tabHeight = 22.f;
   ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, max(ImGui::GetStyle().FramePadding.y, 0.5f * (menuBarHeight * dpi - ImGui::GetFontSize()))));
   ImGui::PushStyleColor(ImGuiCol_MenuBarBg, EditorTheme::V(editor.IsInspectMode() ? EditorTheme::PlayBar : EditorTheme::Bar));
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(editor.IsInspectMode() ? EditorTheme::PlayBar : EditorTheme::Bar));
   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 7.f * dpi)); // Blender's 20 px menu row pitch
   if (ImGui::BeginMainMenuBar())
   {
      if (!editor.IsInspectMode() && ImGui::BeginMenu("File"))
      {
         if (ImGui::BeginMenu("New"))
         {
            if (ImGui::MenuItem("New Table", "Ctrl+N"))
               editor.NewTable(EditorUI::NewTableTemplate::Blank);
            if (ImGui::MenuItem("Completely Blank Table"))
               editor.NewTable(EditorUI::NewTableTemplate::Stripped);
            if (ImGui::MenuItem("Full Example Table"))
               editor.NewTable(EditorUI::NewTableTemplate::Example);
            if (ImGui::MenuItem("Light Sequence Demo Table"))
               editor.NewTable(EditorUI::NewTableTemplate::LightSeq);
            ImGui::EndMenu();
         }
         if (ImGui::MenuItem("Open...", "Ctrl+O"))
            editor.LoadTable();
         if (ImGui::MenuItem("Open Pack Folder...", "Ctrl+Shift+O"))
            editor.LoadTableFolder();
         if (ImGui::MenuItem("Save", "Ctrl+S"))
            editor.SaveTable();
         if (ImGui::MenuItem("Save As..."))
            editor.SaveTableAs();
         if (ImGui::MenuItem("Save As Pack Folder..."))
            editor.SaveTableAsPackFolder();
         ImGui::Separator();
         if (ImGui::MenuItem("Play", "F5", false, !editor.m_table->IsLocked()))
            editor.PlayTest();
         ImGui::Separator();
         if (ImGui::MenuItem("Quit"))
            editor.RequestClose(Player::CS_CLOSE_APP);
         ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Edit"))
      {
         // Blender style: the menu names the step ("Undo Move: Part1"), cut at 40 characters
         const auto stepTitle = [](const char *verb, const string &step)
         {
            size_t n = 37; // Cut on a code point boundary
            if (step.size() > 40)
               while (n > 0 && (static_cast<unsigned char>(step[n]) & 0xC0) == 0x80)
                  --n;
            return step.empty() ? string(verb) : string(verb) + ' ' + (step.size() > 40 ? step.substr(0, n) + "..." : step);
         };
         if (ImGui::MenuItem((stepTitle("Undo", editor.CanUndo() && editor.m_undo.UndoCount() > 0 ? editor.m_undo.UndoLabel(0) : string()) + "###EditUndo").c_str(), "Ctrl+Z", false, editor.CanUndo()))
            editor.UndoSteps(1);
         if (ImGui::MenuItem((stepTitle("Redo", editor.CanRedo() && editor.m_undo.RedoCount() > 0 ? editor.m_undo.RedoLabel(0) : string()) + "###EditRedo").c_str(), "Ctrl+Shift+Z", false, editor.CanRedo()))
            editor.RedoSteps(1);
         if (ImGui::MenuItem("Undo History...", nullptr, false, editor.m_table->m_liveBaseTable == nullptr && (editor.m_undo.HasUndo() || editor.m_undo.HasRedo())))
            OpenUndoHistory();
         if (ImGui::MenuItem("Adjust Last Operation...", "F9", false, editor.m_lastOp.type != EditorUI::LastOperation::Type::None))
         {
            editor.m_adjustPopupOpen = true;
            editor.m_adjustPopupJustOpened = true;
            editor.m_adjustPopupPos = ImGui::GetMousePos();
         }
         if (ImGui::MenuItem("Snapping", "Shift+Tab", editor.m_snapEnabled))
            editor.ToggleSnapping();
         ImGui::Separator();
         if (ImGui::MenuItem("Copy", "Ctrl+C", false, editor.CanCopy()))
            editor.CopySelection();
         if (ImGui::MenuItem("Paste", "Ctrl+V", false, editor.CanPaste()))
            editor.PasteSelection(editor.GetViewportContentCenter());
         ImGui::Separator();
         if (ImGui::MenuItem("Select All", "A", false, editor.CanSelect()))
            editor.SelectAllAction();
         if (ImGui::MenuItem("Deselect", "Alt+A", false, editor.CanSelect() && editor.m_selection.GetType() != Selection::S_NONE))
            editor.ClearSelection();
         if (ImGui::MenuItem("Invert Selection", "Ctrl+I", false, editor.CanSelect()))
            editor.InvertSelection();
         if (ImGui::MenuItem("Select Similar...", "Shift+G", false, editor.m_selectRename.CanSelectSimilar()))
            editor.m_selectRename.OpenSimilar();
         if (ImGui::MenuItem("Batch Rename...", "Ctrl+F2", false, editor.m_selectRename.CanRename()))
            editor.m_selectRename.OpenRename();
         ImGui::EndMenu();
      }
      if (!editor.IsInspectMode() && ImGui::BeginMenu("Object"))
      {
         const bool editablePart = editor.m_selection.GetType() == Selection::S_EDITABLE && !editor.m_table->IsLocked();
         if (ImGui::MenuItem("Duplicate", "Shift+D", false, editor.CanDuplicate()))
            editor.DuplicateSelection();
         if (ImGui::MenuItem("Delete", "Del", false, editor.CanDelete()))
            editor.DeleteAction();
         if (ImGui::MenuItem("Add Part...", "Shift+A", false, !editor.m_table->IsLocked() && editor.m_pointEditPart == nullptr))
            RequestAddPartPopup(ImGui::GetMousePos());
         if (ImGui::BeginMenu("Rubber", !editor.m_table->IsLocked() && !editor.IsInspectMode() && editor.m_pointEditPart == nullptr))
         {
            if (ImGui::MenuItem("Create Rubber Around Selected Posts", nullptr, false, RubberBetweenPosts::CanCreate(editor)))
               RubberBetweenPosts::Create(editor);
            if (ImGui::MenuItem("Bind Selected", nullptr, false, RubberBetweenPosts::CanBind(editor)))
               RubberBetweenPosts::Bind(editor);
            if (ImGui::MenuItem("Autofit Now", nullptr, false, RubberBetweenPosts::CanAutofit(editor)))
               RubberBetweenPosts::Autofit(editor);
            if (ImGui::MenuItem("Detach From Posts", nullptr, false, RubberBetweenPosts::CanDetach(editor)))
               RubberBetweenPosts::Detach(editor);
            ImGui::EndMenu();
         }
         if (ImGui::MenuItem("Add Metal Wire Guide", nullptr, false, !editor.m_table->IsLocked() && editor.m_pointEditPart == nullptr))
         {
            editor.m_addPartType = eItemPrimitive;
            editor.m_addWireform = true;
            editor.m_addWireRail = false;
         }
         if (ImGui::MenuItem("Add Wire Rail", nullptr, false, !editor.m_table->IsLocked() && editor.m_pointEditPart == nullptr))
         {
            editor.m_addPartType = eItemPrimitive;
            editor.m_addWireform = false;
            editor.m_addWireRail = true;
         }
         if (ImGui::MenuItem("Add Plastic from Atlas", nullptr, false, PlasticFromAtlas::CanStart(editor)))
            PlasticFromAtlas::Start(editor);
         if (ImGui::BeginMenu("Snap"))
         {
            // The 3D cursor (Shift+right click places it): see EditorUI::RunCursorOp
            using Op = EditorUI::CursorOp;
            if (ImGui::MenuItem("Cursor to Selected", nullptr, false, editor.CanRunCursorOp(Op::CursorToSelected)))
               editor.RunCursorOp(Op::CursorToSelected, -1);
            if (ImGui::MenuItem("Cursor to World Origin", nullptr, false, editor.CanRunCursorOp(Op::CursorToWorldOrigin)))
               editor.RunCursorOp(Op::CursorToWorldOrigin, -1);
            if (ImGui::MenuItem("Cursor to Grid", nullptr, false, editor.CanRunCursorOp(Op::CursorToGrid)))
               editor.RunCursorOp(Op::CursorToGrid, -1);
            ImGui::Separator();
            if (ImGui::MenuItem("Selection to Cursor", nullptr, false, editor.CanRunCursorOp(Op::SelectionToCursor)))
               editor.RunCursorOp(Op::SelectionToCursor, -1);
            if (ImGui::MenuItem("Selection to Cursor (Keep Offset)", nullptr, false, editor.CanRunCursorOp(Op::SelectionToCursorKeepOffset)))
               editor.RunCursorOp(Op::SelectionToCursorKeepOffset, -1);
            if (ImGui::MenuItem("Selection to Grid", nullptr, false, editor.CanRunCursorOp(Op::SelectionToGrid)))
               editor.RunCursorOp(Op::SelectionToGrid, -1);
            ImGui::EndMenu();
         }
         ImGui::Separator();
         if (ImGui::MenuItem("Move", "G", editor.m_gizmoOperation == ImGuizmo::TRANSLATE))
            editor.SetGizmoOperation(ImGuizmo::TRANSLATE);
         if (ImGui::MenuItem("Rotate", "R", editor.m_gizmoOperation == ImGuizmo::ROTATE))
            editor.SetGizmoOperation(ImGuizmo::ROTATE);
         if (ImGui::MenuItem("Scale", "S", editor.m_gizmoOperation == ImGuizmo::SCALE))
            editor.SetGizmoOperation(ImGuizmo::SCALE);
         if (ImGui::BeginMenu("Transform Orientation"))
         {
            static constexpr const char *names[] = { "Global", "Local", "Normal", "View" };
            for (int i = 0; i < 4; i++)
               if (ImGui::MenuItem(names[i], nullptr, static_cast<int>(editor.m_transformOrientation) == i))
                  editor.SetTransformOrientation(static_cast<EditorUI::TransformOrientation>(i));
            ImGui::EndMenu();
         }
         ImGui::Separator();
         if (ImGui::MenuItem("New Collection from Selection", nullptr, false, editablePart))
            editor.CreateCollection(true);
         if (ImGui::MenuItem("Edit Points", "Tab", false, editor.CanEnterPointEdit()))
            editor.EnterPointEditMode();
         ImGui::Separator();
         if (ImGui::BeginMenu("Set Origin"))
         {
            const bool canSetOrigin = SetOrigin::CanApply(editor);
            if (ImGui::MenuItem("Origin to Geometry", nullptr, false, canSetOrigin))
               SetOrigin::Apply(editor, SetOrigin::Mode::Geometry);
            if (ImGui::MenuItem("Origin to Geometry Bottom", nullptr, false, canSetOrigin))
               SetOrigin::Apply(editor, SetOrigin::Mode::GeometryBottom);
            ImGui::EndMenu();
         }
         ImGui::EndMenu();
      }
      if (!editor.IsInspectMode() && ImGui::BeginMenu("Tools"))
      {
         if (ImGui::MenuItem("Convert to PBR...", nullptr, false, !editor.m_table->IsLocked()))
            editor.m_pbrConvertModal.Show();
         if (ImGui::MenuItem("Import Unity Prefab...", nullptr, false, !editor.m_table->IsLocked()))
            editor.PickUnityPrefab();
         if (ImGui::BeginMenu("Light States"))
         {
            // The editor does not run the table script (which usually sets the lamp states): set the light state of the table or of a collection here
            const auto setStates = [&](const vector<IEditable *> &parts, const float state)
            {
               for (IEditable *const part : parts)
                  if (part->GetItemType() == eItemLight)
                     static_cast<Light *>(part)->put_State(state);
            };
            const float states[] = { 0.f, 0.706f, 1.f };
            const char *stateNames[] = { "Off", "0.706 (lamp top)", "On" };
            if (ImGui::BeginMenu("All Lights"))
            {
               for (int i = 0; i < 3; i++)
                  if (ImGui::MenuItem(stateNames[i]))
                     setStates(editor.m_table->GetParts(), states[i]);
               ImGui::EndMenu();
            }
            for (auto *const collection : editor.m_table->GetCollections())
            {
               bool hasLight = false;
               for (const IEditable *const part : collection->GetParts())
                  hasLight |= part->GetItemType() == eItemLight;
               if (hasLight && ImGui::BeginMenu(collection->m_name.c_str()))
               {
                  for (int i = 0; i < 3; i++)
                     if (ImGui::MenuItem(stateNames[i]))
                        setStates(collection->GetParts(), states[i]);
                  ImGui::EndMenu();
               }
            }
            ImGui::EndMenu();
         }
         if (ImGui::BeginMenu("Navigation"))
         {
            Settings &settings = g_settingsService.GetAppSettings();
            if (ImGui::MenuItem("Orbit Around Selection", nullptr, &editor.m_navOrbitSelection))
               settings.SetEditor_NavOrbitSelection(editor.m_navOrbitSelection, false);
            if (ImGui::MenuItem("Zoom To Mouse Position", nullptr, &editor.m_navZoomToMouse))
               settings.SetEditor_NavZoomToMouse(editor.m_navZoomToMouse, false);
            if (ImGui::MenuItem("Auto Depth (Orbit Around Point Under Cursor)", nullptr, &editor.m_navAutoDepth))
               settings.SetEditor_NavAutoDepth(editor.m_navAutoDepth, false);
            if (ImGui::MenuItem("Invert Zoom", nullptr, &editor.m_navInvertZoom))
               settings.SetEditor_NavInvertZoom(editor.m_navInvertZoom, false);
            if (ImGui::MenuItem("Alt + Drag Acts As Middle Button", nullptr, &editor.m_navAltMiddleButton))
               settings.SetEditor_NavAltMiddleButton(editor.m_navAltMiddleButton, false);
            if (ImGui::MenuItem("Trackpad (Scroll Pans, Ctrl/Cmd+Scroll Zooms)", nullptr, &editor.m_navTrackpad))
               settings.SetEditor_NavTrackpad(editor.m_navTrackpad, false);
            if (ImGui::MenuItem("Emulate Numpad (Top Row Digits)", nullptr, &editor.m_navEmulateNumpad))
               settings.SetEditor_NavEmulateNumpad(editor.m_navEmulateNumpad, false);
            if (ImGui::MenuItem("Blender Transform Keys (G / R / S)", nullptr, &editor.m_navBlenderTransform))
               settings.SetEditor_NavBlenderTransform(editor.m_navBlenderTransform, false);
            float fov = editor.m_navFovY;
            if (ImGui::SliderFloat("Field Of View", &fov, 15.f, 100.f, "%.1f deg"))
            {
               editor.SetFov(fov);
               settings.SetEditor_NavFovY(editor.m_navFovY, false);
            }
            const char *selectionModes[] = { "Selection: Edges", "Selection: Edges + Tint", "Selection: Hidden" };
            if (ImGui::BeginMenu(selectionModes[editor.m_navSelectionDisplay]))
            {
               for (int i = 0; i < 3; i++)
                  if (ImGui::MenuItem(selectionModes[i] + 11, nullptr, editor.m_navSelectionDisplay == i))
                  {
                     editor.m_navSelectionDisplay = i;
                     settings.SetEditor_NavSelectionDisplay(i, false);
                  }
               ImGui::EndMenu();
            }
            if (ImGui::SliderFloat("Orbit Sensitivity", &editor.m_navOrbitSensitivity, 0.1f, 5.f, "%.1f"))
               settings.SetEditor_NavOrbitSensitivity(editor.m_navOrbitSensitivity, false);
            ImGui::EndMenu();
         }
         ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("View"))
      {
         using PV = EditorUI::PredefinedView;
         const auto viewItem = [&](const char *label, const char *keys, PV view)
         {
            if (ImGui::MenuItem(label, keys, editor.m_camMode == ViewMode::EditorCam && editor.m_predefinedView == view))
               editor.SetView(view);
         };
         viewItem("Top", "Numpad 7", PV::Top);
         viewItem("Bottom", "Ctrl+Numpad 7", PV::Bottom);
         viewItem("Front", "Numpad 1", PV::Front);
         viewItem("Back", "Ctrl+Numpad 1", PV::Back);
         viewItem("Left", "Numpad 3", PV::Left);
         viewItem("Right", "Ctrl+Numpad 3", PV::Right);
         ImGui::Separator();
         if (ImGui::MenuItem("Perspective / Orthographic", "Numpad 5"))
         {
            editor.TogglePerspectiveCam();
         }
         if (ImGui::MenuItem("Cycle Camera (Preview / Editor / Backdrop)", "Numpad 0"))
            editor.CycleCamera();
         if (ImGui::MenuItem("Lock Orbit", nullptr, editor.m_orbitLock))
            editor.m_orbitLock = !editor.m_orbitLock;
         ImGui::Separator();
         if (ImGui::MenuItem("Orbit Left", "Numpad 4"))
            editor.OrbitStep(true, 1.f);
         if (ImGui::MenuItem("Orbit Right", "Numpad 6"))
            editor.OrbitStep(true, -1.f);
         if (ImGui::MenuItem("Orbit Up", "Numpad 8"))
            editor.OrbitStep(false, 1.f);
         if (ImGui::MenuItem("Orbit Down", "Numpad 2"))
            editor.OrbitStep(false, -1.f);
         ImGui::Separator();
         if (ImGui::MenuItem("Frame Selected", "F", false, editor.CanFrameSelection()))
            editor.FrameSelection();
         if (ImGui::MenuItem("Frame All", "Home"))
            editor.FrameAll();
         if (ImGui::MenuItem("Local View", "Numpad /  (US: / \\)", editor.IsLocalView())) // Blender's numpad /: the selection alone in the 3D Viewport (over a secondary view the key is its own)
            editor.ToggleLocalView();
         ImGui::Separator();
         static constexpr const char *workspaceKeys[PinnedPanel::WorkspaceCount] = { "Ctrl+1", "Ctrl+2", "Ctrl+3" };
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
            if (ImGui::MenuItem(("Workspace: " + editor.m_pinned.GetWorkspace(i).name).c_str(), workspaceKeys[i], editor.m_pinned.GetActiveWorkspace() == i))
               editor.SwitchWorkspace(i);
         ImGui::Separator();
         if (ImGui::MenuItem("Outliner", nullptr, editor.IsAreaEditorShown(EditorUI::AreaEditor::Outliner)))
            editor.ToggleAreaEditor(EditorUI::AreaEditor::Outliner);
         if (ImGui::MenuItem("Properties", nullptr, editor.IsAreaEditorShown(EditorUI::AreaEditor::Properties)))
            editor.ToggleAreaEditor(EditorUI::AreaEditor::Properties);
         // Reveals an editor, hosting it in its preferred area if no area shows it (the editor type button at the left of each area header re-types that area)
         if (ImGui::BeginMenu("Show Editor"))
         {
            for (int i = 0; i < kPanelTypeCount; ++i)
            {
               const EditorUI::AreaEditor type = static_cast<EditorUI::AreaEditor>(i);
               if (ImGui::MenuItem(EditorUI::AreaEditorName(type), nullptr, editor.IsAreaEditorShown(type)))
                  editor.ShowAreaEditor(type);
            }
            ImGui::EndMenu();
         }
         if (ImGui::MenuItem("Toggle Maximize Area", "Ctrl+Space"))
            editor.ToggleMaximizeArea();
         if (ImGui::MenuItem("Sidebar", "N", editor.m_showSidebar))
            editor.ToggleSidebar();
         if (ImGui::MenuItem("Quick Favorites", "Q"))
            editor.OpenQuickFavorites();
         if (ImGui::MenuItem("Menu Search...", "F3"))
            editor.m_palette.Open();
         if (ImGui::MenuItem("Navigation Cube", nullptr, editor.m_showNavCube))
            editor.m_showNavCube = !editor.m_showNavCube;
         if (ImGui::MenuItem("Tool Shelf", nullptr, m_showToolShelf))
            m_showToolShelf = !m_showToolShelf;
         if (ImGui::MenuItem("Status Bar", nullptr, m_showStatusBar))
            m_showStatusBar = !m_showStatusBar;
         ImGui::EndMenu();
      }
      if (editor.IsInspectMode() && !editor.m_table->IsLocked() && ImGui::BeginMenu("Debug"))
      {
         if (ImGui::MenuItem("Open debugger"))
            editor.m_player->m_showDebugger = true;
         if (ImGui::MenuItem("Renderer Inspection"))
            editor.m_inspectionModal.Show();
         if (ImGui::MenuItem(editor.m_player->IsPlaying() ? "Pause" : "Play"))
            editor.m_player->SetPlayState(!editor.m_player->IsPlaying());
         ImGui::EndMenu();
      }
      else if (!editor.IsInspectMode() && !editor.m_table->IsLocked() && ImGui::BeginMenu("Debug"))
      {
         if (ImGui::MenuItem("Renderer Inspection"))
            editor.m_inspectionModal.Show();
         ImGui::EndMenu();
      }
      // Workspace tabs (Blender's top bar), centered in the menu bar
      if (!editor.m_table->IsLocked())
      {
         const ImGuiStyle &tabStyle = ImGui::GetStyle();
         const float tabPadding = 10.f * dpi;
         const float tabGap = 0.f; // Tabs touch each other
         float tabsWidth = 0.f;
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
            tabsWidth += ImGui::CalcTextSize(editor.m_pinned.GetWorkspace(i).name.c_str()).x + tabPadding * 2.f + (i > 0 ? tabGap : 0.f);
         const float startX = ImGui::GetCursorPosX() + tabStyle.ItemSpacing.x; // Blender: straight after the menus
         const ImVec2 barPos = ImGui::GetWindowPos();
         const float barBottom = barPos.y + ImGui::GetWindowHeight();
         const float savedScreenY = ImGui::GetCursorScreenPos().y;
         ImDrawList *const drawList = ImGui::GetWindowDrawList();
         drawList->AddLine(ImVec2(barPos.x, barBottom - 1.f), ImVec2(barPos.x + ImGui::GetWindowWidth(), barBottom - 1.f), EditorTheme::Rgb(22, 22, 22), 1.f); // Bottom rule
         float x = barPos.x + startX;
         for (int i = 0; i < PinnedPanel::WorkspaceCount; i++)
         {
            const string &name = editor.m_pinned.GetWorkspace(i).name;
            const bool active = editor.m_pinned.GetActiveWorkspace() == i;
            const ImVec2 textSize = ImGui::CalcTextSize(name.c_str());
            const ImVec2 tabSize(textSize.x + tabPadding * 2.f, tabHeight * dpi);
            ImGui::SetCursorScreenPos(ImVec2(x, barBottom - tabSize.y));
            if (ImGui::InvisibleButton((name + "##Workspace").c_str(), tabSize))
               editor.SwitchWorkspace(i);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 tabMin = ImGui::GetItemRectMin();
            const ImVec2 tabMax = ImGui::GetItemRectMax();
            drawList->AddRectFilled(tabMin, tabMax, active ? EditorTheme::Workspace : (hovered ? EditorTheme::Rgb(46, 46, 46) : EditorTheme::WorkspaceOff), 3.f * dpi, ImDrawFlags_RoundCornersTop);
            drawList->AddText(ImVec2(tabMin.x + tabPadding, tabMin.y + 0.5f * (tabSize.y - textSize.y)), active ? IM_COL32_WHITE : EditorTheme::TextDim, name.c_str());
            {
               const string shortcut = "Ctrl+" + std::to_string(i + 1);
               Tip((name + " Workspace").c_str(), shortcut.c_str());
            }
            x += tabSize.x + tabGap;
         }
         ImGui::SetCursorScreenPos(ImVec2(x, savedScreenY));
      }
      // Right side buttons: 28x28, transparent, red when hovering the close one
      const float barButton = 28.f * dpi;
      const auto barIconButton = [barButton](const char *icon, ImU32 hoverColor)
      {
         ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
         ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(hoverColor));
         ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(hoverColor));
         const bool pressed = ImGui::Button(icon, ImVec2(barButton, barButton));
         ImGui::PopStyleColor(3);
         return pressed;
      };
      float buttonWidth = barButton;
      if (editor.IsInspectMode())
         buttonWidth += barButton + ImGui::GetStyle().ItemSpacing.x;
      ImVec2 padding = ImGui::GetCursorScreenPos();
      padding.x += ImGui::GetContentRegionAvail().x - buttonWidth;
      ImGui::SetCursorScreenPos(padding);
      if (editor.IsInspectMode())
      {
         if (barIconButton(ICON_LC_UNDO_2, EditorTheme::Hover)) // ICON_LC_SQUARE
            editor.Close();
         Tip("Back to Game");
      }
      if (barIconButton(ICON_LC_X, EditorTheme::Rgb(200, 60, 60)))
         editor.RequestClose(editor.IsInspectMode() ? Player::CS_STOP_PLAY : Player::CS_CLOSE_APP);
      if (editor.IsInspectMode())
         Tip("Stop Play Test", "Shift+F5");
      else
         Tip("Close Editor");
      m_menuBarHeight = ImGui::GetWindowSize().y;
      ImGui::EndMainMenuBar();
   }
   ImGui::PopStyleVar();
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar();
}

void EditorChrome::RenderToolbar()
{
   EditorUI &editor = m_editor;
   s_toolDpi = editor.m_liveUI.GetDPI();
   const float toolGap = 2.f * s_toolDpi;
   const float oldIconSize = max(0.f, 20.f * s_toolDpi - 2.f * ImGui::GetStyle().WindowPadding.y); // Size of the add part popup icons
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + m_menuBarHeight));
   ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, m_toolBarHeight));
   constexpr ImGuiWindowFlags window_flags
      = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f * s_toolDpi, 2.f * s_toolDpi));
   // Blender's second row is an area header on 48 (the content grey), so the active workspace tab above connects to it; revert to Bar for the old black band
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(editor.IsInspectMode() ? EditorTheme::PlayBar : EditorTheme::Rgb(48, 48, 48)));
   ImGui::Begin("TOOLBAR", nullptr, window_flags);
   ImGui::PopStyleColor();
   ImGui::PopStyleVar(2);
   // 1 px separator between the toolbar and the area panels below
   ImGui::GetWindowDrawList()->AddLine(ImVec2(viewport->Pos.x, viewport->Pos.y + m_menuBarHeight + m_toolBarHeight - 1.f), ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + m_menuBarHeight + m_toolBarHeight - 1.f), EditorTheme::Rgb(22, 22, 22), 1.f);
   if (editor.IsInspectMode())
   {
      if (ToolButton(editor.m_player->IsPlaying() ? ICON_LC_PAUSE : ICON_LC_PLAY))
         editor.m_player->SetPlayState(!editor.m_player->IsPlaying());
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(editor.m_player->IsPlaying());
      if (ToolButton(ICON_LC_SKIP_FORWARD))
         editor.m_player->m_step = true;
      ImGui::EndDisabled();
      ImGui::SameLine(0.f, toolGap);
      if (ToolButton(ICON_LC_SQUARE))
         editor.RequestClose(Player::CS_STOP_PLAY);
      Tip("Stop Play Test", "Shift+F5");
   }
   else
   {
      const float iconSize = oldIconSize;

      // File operations: same actions as the File menu, each button opening a sub menu
      if (ToolButton(ICON_LC_FILE_PLUS))
         ImGui::OpenPopup("New table Popup");
      Tip("New Table", "Ctrl+N");
      if (ImGui::BeginPopup("New table Popup"))
      {
         if (ImGui::MenuItem("New Table", "Ctrl+N"))
            editor.NewTable(EditorUI::NewTableTemplate::Blank);
         if (ImGui::MenuItem("Completely Blank Table"))
            editor.NewTable(EditorUI::NewTableTemplate::Stripped);
         if (ImGui::MenuItem("Full Example Table"))
            editor.NewTable(EditorUI::NewTableTemplate::Example);
         if (ImGui::MenuItem("Light Sequence Demo Table"))
            editor.NewTable(EditorUI::NewTableTemplate::LightSeq);
         ImGui::EndPopup();
      }
      ImGui::SameLine(0.f, toolGap);
      if (ToolButton(ICON_LC_FOLDER_OPEN))
         ImGui::OpenPopup("Open table Popup");
      Tip("Open Table", "Ctrl+O");
      if (ImGui::BeginPopup("Open table Popup"))
      {
         if (ImGui::MenuItem("Table file..."))
            editor.LoadTable();
         if (ImGui::MenuItem("Table pack folder..."))
            editor.LoadTableFolder();
         ImGui::EndPopup();
      }
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_SAVE))
         ImGui::OpenPopup("Save table Popup");
      Tip("Save Table", "Ctrl+S", editor.m_table->IsLocked() ? "The table is locked" : nullptr);
      if (ImGui::BeginPopup("Save table Popup"))
      {
         if (ImGui::MenuItem("Save", "Ctrl+S"))
            editor.SaveTable();
         if (ImGui::MenuItem("Save As File..."))
            editor.SaveTableAs();
         if (ImGui::MenuItem("Save As Pack Folder..."))
            editor.SaveTableAsPackFolder();
         ImGui::EndPopup();
      }
      ImGui::EndDisabled();
      ToolSeparator();

      // Play a shallow copy of the edited table
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_PLAY))
         editor.PlayTest();
      Tip("Play Table", "F5", editor.m_table->IsLocked() ? "The table is locked" : nullptr);
      ImGui::EndDisabled();
      ToolSeparator();

      // Edit mode selection: moved to the tool shelf when it is shown
      if (!m_toolShelfDrawn) // Edit modes: in the tool shelf when it was drawn last frame
      {
         for (const auto &mode : editModes)
         {
            const bool active = (editor.m_gizmoOperation == mode.operation);
            if (ToolButton(mode.icon, active))
               editor.SetGizmoOperation(mode.operation);
            Tip(mode.title, mode.shortcut);
            if (&mode != std::end(editModes) - 1)
               ImGui::SameLine(0.f, toolGap);
         }
      }

      // Snapping: magnet toggle (Shift+Tab) and a small chevron button opening the snapping popover
      ImGui::SameLine(0.f, toolGap);
      {
         const bool snapOn = editor.m_snapEnabled;
         if (ToolButton(ICON_LC_MAGNET, snapOn))
            editor.ToggleSnapping();
         const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
         Tip(snapOn ? "Snapping: On" : "Snapping: Off", "Shift+Tab", "Hold Ctrl to invert while transforming.");
         ImGui::SameLine(0.f, 0.f);
         ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(0u));
         ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(EditorTheme::Hover));
         ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(EditorTheme::Hover));
         const bool openPopover = ImGui::Button("##SnapOptions", ImVec2(0.5f * ToolButtonSize * s_toolDpi, ToolButtonSize * s_toolDpi));
         ImGui::PopStyleColor(3);
         {
            const ImVec2 cmin = ImGui::GetItemRectMin(), cmax = ImGui::GetItemRectMax();
            DrawChevron(ImGui::GetWindowDrawList(), ImVec2(0.5f * (cmin.x + cmax.x), 0.5f * (cmin.y + cmax.y)), ChevronDir::Down, 3.f * s_toolDpi, EditorTheme::TextDim);
         }
         Tip("Snapping Options");
         if (openPopover)
            ImGui::OpenPopup("Snap Popover");
         ImGui::SetNextWindowPos(ImVec2(bmin.x, bmax.y), ImGuiCond_Appearing);
         if (ImGui::BeginPopup("Snap Popover"))
         {
            bool changed = false;
            const float labelWidth = 84.f * s_toolDpi;
            const float fieldWidth = 120.f * s_toolDpi;
            // Same section style as the Properties editor: accent rule on the left of the title
            const auto section = [](const char *title)
            {
               const ImVec2 pos = ImGui::GetCursorScreenPos();
               ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + 2.f, pos.y + ImGui::GetTextLineHeight()), EditorTheme::Accent);
               ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.f * s_toolDpi);
               ImGui::TextUnformatted(title);
            };
            // Label on the left, field on the right (Properties convention)
            const auto rowLabel = [labelWidth](const char *label)
            {
               ImGui::AlignTextToFramePadding();
               ImGui::TextUnformatted(label);
               ImGui::SameLine(labelWidth + ImGui::GetStyle().WindowPadding.x);
            };
            section("Snap");
            rowLabel("Snap To");
            // Two chip segmented control
            for (int target = 0; target < 2; ++target)
            {
               const bool active = editor.m_snapTarget == target;
               ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(active ? EditorTheme::Fill : EditorTheme::Raised));
               ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::V(active ? EditorTheme::FillHovered : EditorTheme::Hover));
               ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::V(active ? EditorTheme::FillActive : EditorTheme::Hover));
               ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * s_toolDpi);
               if (ImGui::Button(target == 0 ? "Increment" : "Points", ImVec2(0.5f * fieldWidth, 0.f)) && !active)
               {
                  editor.m_snapTarget = target;
                  changed = true;
               }
               ImGui::PopStyleVar();
               ImGui::PopStyleColor(3);
               if (target == 0)
               {
                  ImGui::SetItemTooltip("Snap to multiples of the increments.");
                  ImGui::SameLine(0.f, 2.f * s_toolDpi);
               }
               else
                  ImGui::SetItemTooltip("Snap to the centers and drag points of the other parts (G move, without axis constraint; the gizmo uses the increment).");
            }
            ImGui::Spacing();
            section("Increments");
            rowLabel("Move (vpu)");
            ImGui::SetNextItemWidth(fieldWidth);
            changed |= ImGui::DragFloat("##SnapMove", &editor.m_snapMove, 0.5f, 0.01f, 1000.f, "%.2f");
            rowLabel("Rotate (deg)");
            ImGui::SetNextItemWidth(fieldWidth);
            changed |= ImGui::DragFloat("##SnapRotate", &editor.m_snapRotate, 0.5f, 0.1f, 180.f, "%.1f");
            rowLabel("Scale");
            ImGui::SetNextItemWidth(fieldWidth);
            changed |= ImGui::DragFloat("##SnapScale", &editor.m_snapScale, 0.005f, 0.001f, 10.f, "%.3f");
            ImGui::Spacing();
            section("Affect");
            changed |= ImGui::Checkbox("Move", &editor.m_snapAffectMove);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Rotate", &editor.m_snapAffectRotate);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Scale", &editor.m_snapAffectScale);
            if (changed)
               editor.SaveSnapPreferences();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) // Esc closes the popover (keyboard navigation is off: ImGui does not), it is not an editor Esc
            {
               editor.m_paneEscPending = true;
               ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
         }
      }
      ToolSeparator();

      // Add part buttons (same parts and icons as the WinUI toolbar)
      if (m_addPartButtons.empty())
      {
         static const struct
         {
            ItemTypeEnum type;
            const char *name;
            const char *file;
            bool wire = false;
            bool rail = false;
         } addParts[] = {
            { eItemSurface, "Surface", "editor/wall.png" }, //
            { eItemGate, "Gate", "editor/gate.png" }, //
            { eItemRamp, "Ramp", "editor/ramp.png" }, //
            { eItemFlipper, "Flipper", "editor/flipper.png" }, //
            { eItemPlunger, "Plunger", "editor/plunger.png" }, //
            { eItemBall, "Ball", "editor/ball.png" }, //
            { eItemBumper, "Bumper", "editor/bumper.png" }, //
            { eItemSpinner, "Spinner", "editor/spinner.png" }, //
            { eItemTimer, "Timer", "editor/timer.png" }, //
            { eItemTrigger, "Trigger", "editor/trigger.png" }, //
            { eItemLight, "Light", "editor/light.png" }, //
            { eItemKicker, "Kicker", "editor/kicker.png" }, //
            { eItemHitTarget, "Target", "editor/target.png" }, //
            { eItemDecal, "Decal", "editor/decal.png" }, //
            { eItemTextbox, "Textbox", "editor/textbox.png" }, //
            { eItemDispReel, "Display Reel", "editor/reel.png" }, //
            { eItemLightSeq, "Light Seq", "editor/lightseq.png" }, //
            { eItemPrimitive, "Primitive", "editor/primitive.png" }, //
            { eItemFlasher, "Flasher", "editor/flasher.png" }, //
            { eItemRubber, "Rubber", "editor/rubber.png" }, //
            { eItemPrimitive, "Metal Wire Guide", "editor/ramp.png", true }, // Primitive with the wire generator (no dedicated icon yet)
            { eItemPrimitive, "Wire Rail", "editor/ramp.png", false, true }, // Primitive with the wire and rail generators (no dedicated icon yet)
         };
         for (const auto &def : addParts)
         {
            std::shared_ptr<const BaseTexture> tex = BaseTexture::CreateFromFile(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets, def.file));
            m_addPartButtons.push_back({ def.type, def.name, tex ? std::make_shared<Sampler>(editor.m_renderer->m_renderDevice, def.name, tex, false) : nullptr, def.wire, def.rail });
         }
      }
      // Add part button, opening the part type picker popup (same as the Shift+A shortcut)
      if (!m_toolShelfDrawn) // Moved to the tool shelf when it is shown (the popup below is shared)
      {
         ImGui::BeginDisabled(editor.m_pointEditPart != nullptr || editor.m_table->IsLocked());
         const bool addPartActive = (editor.m_addPartType != eItemInvalid);
         if (ToolButton(ICON_LC_PLUS, addPartActive))
            RequestAddPartPopup(ImGui::GetMousePos());
         Tip("Add Part", "Shift+A", editor.m_table->IsLocked() ? "The table is locked" : editor.m_pointEditPart != nullptr ? "Not available while editing points" : nullptr);
         ImGui::EndDisabled();
      }

      // Copy/Paste buttons (same actions as the Ctrl+C and Ctrl+V keyboard shortcuts)
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(!editor.CanCopy());
      if (ToolButton(ICON_LC_COPY))
         editor.CopySelection();
      Tip("Copy Selection", "Ctrl+C", editor.CanCopy() ? nullptr : editor.m_table->IsLocked() ? "The table is locked" : "Nothing selected");
      ImGui::EndDisabled();
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(!editor.CanPaste());
      if (ToolButton(ICON_LC_CLIPBOARD_PASTE))
         editor.PasteSelection(editor.GetViewportContentCenter());
      Tip("Paste", "Ctrl+V", editor.CanPaste() ? nullptr : editor.m_table->IsLocked() ? "The table is locked" : editor.IsInspectMode() ? "Not available while inspecting" : "The clipboard is empty");
      ImGui::EndDisabled();

      // Delete selection button, always visible but only enabled when there are selected parts
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(editor.m_selection.GetType() != Selection::S_EDITABLE || editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_TRASH_2))
         editor.DeleteSelection();
      Tip("Delete Selection", "Delete", editor.m_table->IsLocked() ? "The table is locked" : editor.m_selection.GetType() != Selection::S_EDITABLE ? "Nothing selected" : nullptr);
      ImGui::EndDisabled();

      // Part library import/export buttons
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_DOWNLOAD))
         editor.OpenPartLibrary();
      Tip("Import Parts", nullptr, editor.m_table->IsLocked() ? "The table is locked" : "Open the part library.");
      ImGui::EndDisabled();
      ImGui::SameLine(0.f, toolGap);
      ImGui::BeginDisabled(!editor.CanExportPartSelection());
      if (ToolButton(ICON_LC_UPLOAD))
         editor.ExportPartSelection();
      Tip("Export Selection", nullptr, editor.CanExportPartSelection() ? "Export the selected parts to a VPZ pack." : "Nothing selected");
      ImGui::EndDisabled();

      // Script editor button
      ToolSeparator();
      const bool scriptActive = editor.IsAreaEditorShown(EditorUI::AreaEditor::Script);
      if (ToolButton(ICON_LC_CODE_XML, scriptActive))
         editor.ShowAreaEditor(EditorUI::AreaEditor::Script);
      Tip("Edit Table Script");

      // Part type picker popup (Shift+A in standard mode): picking a type arms add part mode,
      // clicking outside of the popup dismisses it without arming add part mode
      if (m_openAddPartPopup)
      {
         m_openAddPartPopup = false;
         ImGui::OpenPopup("Add Part");
      }
      ImGui::SetNextWindowPos(m_addPartPopupPos, ImGuiCond_Appearing);
      if (ImGui::BeginPopup("Add Part"))
      {
         int column = 0;
         for (const auto &button : m_addPartButtons)
         {
            if (button.icon == nullptr)
               continue;
            if (column > 0)
               ImGui::SameLine();
            if (ImGui::ImageButton(button.name, button.icon, ImVec2(iconSize, iconSize)))
            {
               editor.m_addPartType = button.type;
               editor.m_addWireform = button.wire;
               editor.m_addWireRail = button.rail;
               ImGui::CloseCurrentPopup();
            }
            Tip((string("Add ") + button.name).c_str());
            column = (column + 1) % 5;
         }
         ImGui::Separator();
         const bool canRubber = RubberBetweenPosts::CanCreate(editor);
         ImGui::BeginDisabled(!canRubber);
         if (ImGui::Selectable("Create Rubber Around Selected Posts"))
         {
            RubberBetweenPosts::Create(editor);
            ImGui::CloseCurrentPopup();
         }
         ImGui::EndDisabled();
         if (!canRubber)
            ImGui::SetItemTooltip("Select two or more primitives (the posts) first");
         if (ImGui::Selectable("Plastic from Atlas"))
         {
            PlasticFromAtlas::Start(editor);
            ImGui::CloseCurrentPopup();
         }
         ImGui::SetItemTooltip("Trace a plastic on a print atlas image");
         ImGui::EndPopup();
      }
   }
   const float buttonWidth = 3.f * ToolButtonSize * s_toolDpi + 2.f * toolGap;
   ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttonWidth);
   if (ToolButton(ICON_LC_RULER)) // Unit selection menu
      ImGui::OpenPopup("Unit Popup");
   Tip("Units", nullptr, "Display units of the property fields.");
   if (ImGui::BeginPopup("Unit Popup"))
   {
      ImGui::TextUnformatted("Units");
      ImGui::Separator();
      if (ImGui::RadioButton("VP Units", editor.m_units == EditorUI::Units::VPX))
         editor.m_units = EditorUI::Units::VPX;
      if (ImGui::RadioButton("Metric", editor.m_units == EditorUI::Units::Metric))
         editor.m_units = EditorUI::Units::Metric;
      if (ImGui::RadioButton("Imperial", editor.m_units == EditorUI::Units::Imperial))
         editor.m_units = EditorUI::Units::Imperial;
      ImGui::EndPopup();
   }
   ImGui::SameLine(0.f, toolGap);
   if (ToolButton(ICON_LC_LAYERS)) // Overlay option menu
      ImGui::OpenPopup("Overlay Popup");
   Tip("Overlays", nullptr, "Viewport overlays.");
   if (ImGui::BeginPopup("Overlay Popup"))
   {
      ImGui::TextUnformatted("Overlays");
      ImGui::Separator();
      ImGui::Checkbox("Selection Overlay", &editor.m_selectionOverlay);
      ImGui::Separator();
      ImGui::TextUnformatted("Physics Overlay");
      if (ImGui::RadioButton("None", editor.m_physOverlay == EditorUI::PhysicOverlay::None))
         editor.m_physOverlay = EditorUI::PhysicOverlay::None;
      if (ImGui::RadioButton("Selected", editor.m_physOverlay == EditorUI::PhysicOverlay::Selected))
         editor.m_physOverlay = EditorUI::PhysicOverlay::Selected;
      if (ImGui::RadioButton("All", editor.m_physOverlay == EditorUI::PhysicOverlay::All))
         editor.m_physOverlay = EditorUI::PhysicOverlay::All;
      ImGui::EndPopup();
   }
   ImGui::SameLine(0.f, toolGap);
   if (ToolButton(ICON_LC_LIST_FILTER)) // Selection filter
      ImGui::OpenPopup("Selection filter Popup");
   Tip("Selection Filter", nullptr, "Choose which kinds of parts can be selected in the viewport.");
   if (ImGui::BeginPopup("Selection filter Popup"))
   {
      bool pf = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Playfield);
      bool prims = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Primitives);
      bool lights = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Lights);
      bool flashers = EditorUI::HasFlag(editor.m_selectionFilter, EditorUI::SelectionFilter::Flashers);
      ImGui::TextUnformatted("Selection Filter");
      ImGui::Separator();
      if (ImGui::Checkbox("Playfield", &pf))
         editor.m_selectionFilter = pf ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Playfield) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Playfield);
      if (ImGui::Checkbox("Primitives", &prims))
         editor.m_selectionFilter = prims ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Primitives) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Primitives);
      if (ImGui::Checkbox("Lights", &lights))
         editor.m_selectionFilter = lights ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Lights) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Lights);
      if (ImGui::Checkbox("Flashers", &flashers))
         editor.m_selectionFilter = flashers ? (editor.m_selectionFilter | EditorUI::SelectionFilter::Flashers) : (editor.m_selectionFilter & ~EditorUI::SelectionFilter::Flashers);
      ImGui::EndPopup();
   }
   ImGui::End();
}

bool EditorChrome::ToolShelfVisible() const
{
   // Same states as the nav cube (the fly mode and the renderer inspection are also handled by the caller, which only draws the chrome in the full UI)
   const EditorUI &editor = m_editor;
   return m_showToolShelf && !editor.IsInspectMode() && !editor.m_table->IsLocked() && !editor.m_flyMode && !editor.m_inspectionModal.IsVisible()
      && editor.m_camMode != ViewMode::DesktopBackdrop;
}

void EditorChrome::RenderToolShelf(const AreaRect &viewportContent)
{
   m_toolShelfDrawn = false; // Set once the shelf is really drawn: the toolbar keeps the tools whenever the shelf is hidden (too small, overlap, no viewport)
   if (!ToolShelfVisible() || !m_editor.IsViewportVisible())
      return;
   EditorUI &editor = m_editor;
   const float dpi = editor.m_liveUI.GetDPI();
   s_toolDpi = dpi;
   const ImGuiStyle &style = ImGui::GetStyle();

   // Below the status overlay texts (orbit lock + view name, tool name, optional point edit line): same metrics as RenderStatusOverlay
   const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
   float textStackHeight = style.WindowPadding.y + 2.f * style.FramePadding.y + 2.f * lineHeight;
   if (editor.m_pointEditPart)
      textStackHeight += lineHeight;
   const ImVec2 pos(viewportContent.x0 + 6.f * dpi, viewportContent.y0 + 5.f * dpi + textStackHeight + 4.f * dpi);

   // Estimated size (the window auto resizes): 4 edit modes, a separator, add, orientation toggle
   const float pad = 3.f * dpi;
   const float gap = 2.f * dpi;
   const float button = ToolButtonSize * dpi;
   const float sepHeight = ToolGroupGap * dpi;
   const float estWidth = button + 2.f * pad;
   const float estHeight = 6.f * button + 4.f * gap + 2.f * sepHeight + 2.f * pad;
   // Do not overlap the nav cube / N-panel side (too narrow viewport) nor the F9 Adjust Last panel at the bottom left (last frame rect, if shown)
   if (viewportContent.Width() < estWidth + 220.f * dpi || pos.y + estHeight > viewportContent.y1 - 8.f * dpi)
      return;
   if (const ImGuiWindow *adjust = ImGui::FindWindowByName("Adjust Last Operation"); adjust != nullptr && adjust->WasActive && pos.y + estHeight + 4.f * dpi > adjust->Pos.y)
      return;

   m_toolShelfDrawn = true;
   ImGui::SetNextWindowPos(pos);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f * dpi);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V((EditorTheme::Strip & 0x00FFFFFFu) | (217u << 24))); // ~85% alpha
   ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::V(EditorTheme::Border));
   const bool open = ImGui::Begin("##ToolShelf", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
         | ImGuiWindowFlags_NoNav);
   ImGui::PopStyleColor(2);
   ImGui::PopStyleVar(4);
   if (open)
   {
      // Edit modes
      for (const auto &mode : editModes)
      {
         const bool active = (editor.m_gizmoOperation == mode.operation);
         if (ToolButton(mode.icon, active))
            editor.SetGizmoOperation(mode.operation);
         Tip(mode.title, mode.shortcut);
      }

      // Separator
      {
         const ImVec2 sp = ImGui::GetCursorScreenPos();
         ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x + 3.f * dpi, sp.y + 0.5f * sepHeight), ImVec2(sp.x + button - 3.f * dpi, sp.y + 0.5f * sepHeight), EditorTheme::TabActive, 1.f);
         ImGui::Dummy(ImVec2(button, sepHeight));
      }

      // Add part: opens the part type picker popup of the top toolbar (consumed there next frame), anchored at the bottom right of the button
      ImGui::BeginDisabled(editor.m_pointEditPart != nullptr || editor.m_table->IsLocked());
      if (ToolButton(ICON_LC_PLUS, editor.m_addPartType != eItemInvalid))
         RequestAddPartPopup(ImVec2(ImGui::GetItemRectMax().x + 2.f * dpi, ImGui::GetItemRectMax().y));
      Tip("Add Part", "Shift+A", editor.m_table->IsLocked() ? "The table is locked" : editor.m_pointEditPart != nullptr ? "Not available while editing points" : nullptr);
      ImGui::EndDisabled();

      // Transform Orientation, Global / Local (the header dropdown has all of them): the axes of the gizmo and of X / Y / Z in G / R / S
      {
         using Orientation = EditorUI::TransformOrientation;
         const Orientation orientation = editor.m_transformOrientation;
         const bool global = orientation == Orientation::Global;
         if (ToolButton(ICON_LC_BOX, !global))
            editor.SetTransformOrientation(global ? Orientation::Local : Orientation::Global);
         static constexpr const char *titles[] = { "Orientation: Global", "Orientation: Local", "Orientation: Normal", "Orientation: View" };
         Tip(titles[static_cast<int>(orientation)], nullptr, global ? "Click to switch to Local. Repeating G / R / S in the gizmo also switches." : "Click to switch to Global. Repeating G / R / S in the gizmo also switches.");
      }
   }
   ImGui::End();
}

void EditorChrome::RenderStatusOverlay()
{
   EditorUI &editor = m_editor;

   // Overlay Info Text: at the top left of the viewport area
   const float dpi = editor.m_liveUI.GetDPI();
   const AreaRect &vc = editor.m_viewportContentRect;
   const ImVec2 overlayPos(vc.x0, vc.y0 + 5.f * dpi);

   // Camera orbit lock toggle (drawn in its own window as the status overlay does not capture inputs)
   ImGui::SetNextWindowPos(overlayPos);
   ImGui::Begin("orbit lock toggle", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
         | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
   const bool stylePushed = editor.m_orbitLock;
   if (stylePushed)
      ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::V(EditorTheme::Fill)); // On reads brighter than the 84 default (Raised 66 read darker)
   if (ImGui::Button(editor.m_orbitLock ? ICON_LC_LOCK : ICON_LC_LOCK_OPEN))
      editor.m_orbitLock = !editor.m_orbitLock;
   if (stylePushed)
      ImGui::PopStyleColor();
   Tip("Lock Camera Orbit");
   const float lockToggleWidth = ImGui::GetItemRectSize().x;
   ImGui::End();

   // Shading switch at the right end of the viewport's top row (hidden when too narrow for it beside the texts)
   if (vc.Width() > 560.f * dpi)
   {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
      const bool shadeOptions = editor.m_camMode == ViewMode::EditorCam && !editor.IsInspectMode(); // (none in inspect mode either, it draws with the defaults) The options are the editor view's: none for the Preview Camera and the Desktop Backdrop
      ImGui::SetNextWindowPos(ImVec2(vc.x1 - 16.f * dpi - ShadeIconRowWidth(shadeOptions), overlayPos.y), 0, ImVec2(0.f, 0.f));
      ImGui::Begin("shading row", nullptr,
         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
      int mode = Renderer::IsWireframeShade(editor.m_shadeMode) ? 0 : editor.m_shadeMode == Renderer::ShadeMode::Solid ? 1 : 2;
      bool xray = editor.m_shadeMode == Renderer::ShadeMode::NoDepthWireframe; // The main view's X-Ray is its NoDepthWireframe shade mode (the Z cycle's third step)
      const bool xrayBefore = xray;
      if (ShadeIconRow("##MainShading", mode, shadeOptions ? &editor.m_viewShade : nullptr, &editor, &xray))
         editor.m_shadeMode = mode == 0 ? (xray ? Renderer::ShadeMode::NoDepthWireframe : Renderer::ShadeMode::Wireframe) : mode == 1 ? Renderer::ShadeMode::Solid : Renderer::ShadeMode::Default;
      else if (xray != xrayBefore && Renderer::IsWireframeShade(editor.m_shadeMode))
         editor.m_shadeMode = xray ? Renderer::ShadeMode::NoDepthWireframe : Renderer::ShadeMode::Wireframe;
      ImGui::End();
      // The transform dropdowns on the left of it, as in Blender's header (hidden first when it gets narrow)
      if (vc.Width() > 720.f * dpi)
      {
         ImGui::SetNextWindowPos(ImVec2(vc.x1 - 16.f * dpi - ShadeIconRowWidth(shadeOptions) - 12.f * dpi - TransformHeaderRowWidth(), overlayPos.y), 0, ImVec2(0.f, 0.f));
         ImGui::Begin("transform row", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
               | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
         TransformHeaderRow("##MainTransform", editor);
         ImGui::End();
      }
      ImGui::PopStyleVar();
   }

   ImGui::SetNextWindowSize(ImVec2(max(1.f, vc.Width()), max(1.f, vc.Height() - 5.f * dpi)));
   ImGui::SetNextWindowPos(overlayPos);
   ImGui::Begin("text overlay", nullptr, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
   const float labelAlign = ImGui::GetCursorPosX() + lockToggleWidth + ImGui::GetStyle().ItemSpacing.x;
   ImGui::SetCursorPos(ImVec2(labelAlign, ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y));
   // Area header: what this window is, and the state of the play test being inspected
   ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
   ShadowedText("3D Viewport");
   ImGui::PopStyleColor();
   ImGui::SameLine(0.f, 8.f * dpi);
   if (editor.IsInspectMode())
   {
      const char *const badge = editor.m_player->IsPlaying() ? "Playing" : "Paused";
      const ImVec2 textSize = ImGui::CalcTextSize(badge);
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float padX = 6.f * dpi;
      ImDrawList *const dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(ImVec2(at.x, at.y - 1.f * dpi), ImVec2(at.x + textSize.x + 2.f * padX, at.y + textSize.y + 1.f * dpi), EditorTheme::Accent, 3.f * dpi);
      dl->AddText(ImVec2(at.x + padX, at.y), IM_COL32(20, 20, 22, 255), badge);
      ImGui::Dummy(ImVec2(textSize.x + 2.f * padX, textSize.y));
      ImGui::SameLine(0.f, 8.f * dpi);
   }
   ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
   ShadowedText("|");
   ImGui::PopStyleColor();
   ImGui::SameLine(0.f, 8.f * dpi);
   switch (editor.m_camMode)
   {
   case ViewMode::PreviewCam: ShadowedText("Preview Camera"); break;
   case ViewMode::EditorCam:
   {
      string text;
      switch (editor.m_predefinedView)
      {
      case EditorUI::PredefinedView::None: text = "User"sv; break;
      case EditorUI::PredefinedView::Left: text = "Left"sv; break;
      case EditorUI::PredefinedView::Right: text = "Right"sv; break;
      case EditorUI::PredefinedView::Top: text = "Top"sv; break;
      case EditorUI::PredefinedView::Bottom: text = "Bottom"sv; break;
      case EditorUI::PredefinedView::Front: text = "Front"sv; break;
      case EditorUI::PredefinedView::Back: text = "Back"sv; break;
      }
      ShadowedText((text + (editor.m_perspectiveCam ? " Perspective" : " Orthographic")).c_str());
      break;
   }
   case ViewMode::DesktopBackdrop: ShadowedText("Desktop Backdrop"); break;
   }
   const ImVec2 viewLabelMin = ImGui::GetItemRectMin(), viewLabelMax = ImGui::GetItemRectMax(); // The view name just drawn, without "(Local)" (the window is NoInputs: its click target is a window of its own, below)
   if (editor.IsLocalView()) // Blender's viewport name, "(Local)" after it
   {
      ImGui::SameLine(0.f, ImGui::CalcTextSize(" ").x);
      ShadowedText("(Local)");
   }
   ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + ImGui::GetStyle().FramePadding.x, ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y));
   const char *gizmoIcon = nullptr;
   const char *gizmoLabel = nullptr;
   if (editor.m_gizmoOperation == ImGuizmo::OPERATION(0))
   {
      gizmoIcon = ICON_LC_MOUSE_POINTER_2;
      gizmoLabel = "Select";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::TRANSLATE)
   {
      gizmoIcon = ICON_LC_MOVE;
      gizmoLabel = "Move";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::SCALE)
   {
      gizmoIcon = ICON_LC_SCALE_3D;
      gizmoLabel = "Scale";
   }
   else if (editor.m_gizmoOperation == ImGuizmo::ROTATE)
   {
      gizmoIcon = ICON_LC_ROTATE_3D;
      gizmoLabel = "Rotate";
   }
   if (gizmoIcon)
   {
      ShadowedText(gizmoIcon);
      ImGui::SameLine(labelAlign);
      ShadowedText(gizmoLabel);
   }
   if (editor.m_pointEditPart)
      ShadowedText(editor.m_pointEditCenter ? "Center Edit (Tab to exit)" : "Drag Point Edit (Tab to exit)");
   ImGui::End();

   // Click target over the view name: Blender's View > Viewpoint menu, same actions as the numpad keys and the View menu
   {
      const float pad = 3.f * dpi;
      ImGui::SetNextWindowPos(ImVec2(viewLabelMin.x - pad, viewLabelMin.y - pad));
      ImGui::SetNextWindowSize(ImVec2(viewLabelMax.x - viewLabelMin.x + 2.f * pad, viewLabelMax.y - viewLabelMin.y + 2.f * pad));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
      ImGui::Begin("view label menu", nullptr,
         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar();
      // A click that confirms a G / R / S transform (the modal ends on the press) must not also open the menu
      static bool s_modalLastFrame = false;
      static bool s_ignoreClick = false;
      const bool pressed = ImGui::InvisibleButton("##viewlabel", ImGui::GetContentRegionAvail());
      if (ImGui::IsItemActivated())
         s_ignoreClick = editor.m_modal.active || s_modalLastFrame;
      s_modalLastFrame = editor.m_modal.active;
      if (pressed && !s_ignoreClick && !editor.m_modal.active)
         ImGui::OpenPopup("##viewpoint");
      if (ImGui::IsItemHovered() || ImGui::IsPopupOpen("##viewpoint"))
         ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 0.5f), 3.f * dpi);
      ImGui::SetNextWindowPos(ImVec2(viewLabelMin.x - pad, viewLabelMax.y + pad)); // Drops down from the label, which stays readable
      if (ImGui::BeginPopup("##viewpoint"))
      {
         using PV = EditorUI::PredefinedView;
         const bool editorCam = editor.m_camMode == ViewMode::EditorCam;
         // The projection the view really has: the Preview Camera is a perspective view, the Desktop Backdrop an orthographic one
         const bool perspective = editor.m_camMode == ViewMode::PreviewCam || (editorCam && editor.m_perspectiveCam);
         const bool orthographic = editor.m_camMode == ViewMode::DesktopBackdrop || (editorCam && !editor.m_perspectiveCam);
         // Picking the other projection is what Numpad 5 does (it starts from that real projection in every camera mode)
         if (ImGui::MenuItem("Perspective", nullptr, perspective) && !perspective)
            editor.TogglePerspectiveCam();
         if (ImGui::MenuItem("Orthographic", "Numpad 5", orthographic) && !orthographic)
            editor.TogglePerspectiveCam();
         ImGui::Separator();
         const auto viewItem = [&](const char *label, const char *keys, PV view)
         {
            if (ImGui::MenuItem(label, keys, editorCam && editor.m_predefinedView == view))
               editor.SetView(view);
         };
         viewItem("Top", "Numpad 7", PV::Top);
         viewItem("Bottom", "Ctrl+Numpad 7", PV::Bottom);
         viewItem("Front", "Numpad 1", PV::Front);
         viewItem("Back", "Ctrl+Numpad 1", PV::Back);
         viewItem("Left", "Numpad 3", PV::Left);
         viewItem("Right", "Ctrl+Numpad 3", PV::Right);
         ImGui::Separator();
         if (ImGui::MenuItem("Frame All", "Home"))
            editor.FrameAll();
         ImGui::Separator();
         // The cameras Numpad 0 cycles through, picked directly
         if (ImGui::MenuItem("Preview Camera", nullptr, editor.m_camMode == ViewMode::PreviewCam) && editor.m_camMode != ViewMode::PreviewCam)
         {
            editor.m_predefinedView = EditorUI::PredefinedView::None;
            editor.m_camMode = ViewMode::PreviewCam;
         }
         if (ImGui::MenuItem("Desktop Backdrop", nullptr, editor.m_camMode == ViewMode::DesktopBackdrop) && editor.m_camMode != ViewMode::DesktopBackdrop)
         {
            editor.m_predefinedView = EditorUI::PredefinedView::None;
            editor.m_camMode = ViewMode::DesktopBackdrop;
            editor.SetBackdropCamera();
         }
         if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) // Esc closes the menu (keyboard navigation is off: ImGui does not), it is not an editor Esc
         {
            editor.m_paneEscPending = true;
            ImGui::CloseCurrentPopup();
         }
         ImGui::EndPopup();
      }
      ImGui::End();
   }
}

void EditorChrome::RenderUndoHistory()
{
   if (!m_undoHistoryOpen)
      return;
   EditorUI &editor = m_editor;
   const float dpi = editor.m_liveUI.GetDPI();
   // The history can not change under the window: it is closed with the editor state it lists
   if (editor.m_table->m_liveBaseTable != nullptr || editor.m_modal.active)
   {
      m_undoHistoryOpen = false;
      return;
   }
   ImGui::SetNextWindowPos(m_undoHistoryPos, ImGuiCond_Appearing);
   ImGui::SetNextWindowSizeConstraints(ImVec2(260.f * dpi, 0.f), ImVec2(420.f * dpi, 420.f * dpi));
   bool open = true;
   int undoSteps = 0, redoSteps = 0; // Requested move in the history, applied once the window is done
   if (ImGui::Begin("Undo History", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
   {
      const size_t undoCount = editor.m_undo.UndoCount();
      const size_t redoCount = editor.m_undo.RedoCount();
      if (undoCount == 0 && redoCount == 0)
         ImGui::TextDisabled("No history");
      // Steps that can be redone, dimmed, the furthest first (so that the list reads newest first)
      for (size_t i = redoCount; i-- > 0;)
      {
         ImGui::PushID(static_cast<int>(i) + 1000);
         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextFaint));
         if (ImGui::Selectable(editor.m_undo.RedoLabel(i).c_str()))
            redoSteps = static_cast<int>(i) + 1;
         ImGui::PopStyleColor();
         ImGui::PopID();
      }
      // Done steps, the newest first: the first one is the current state, picking an older one undoes down to the state it left
      for (size_t i = 0; i < undoCount; i++)
      {
         ImGui::PushID(static_cast<int>(i));
         if (ImGui::Selectable(editor.m_undo.UndoLabel(i).c_str(), i == 0))
            undoSteps = static_cast<int>(i);
         ImGui::PopID();
      }
      if (undoCount > 0 || redoCount > 0)
      {
         ImGui::PushID("original");
         ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::V(EditorTheme::TextDim));
         if (ImGui::Selectable("Original", undoCount == 0))
            undoSteps = static_cast<int>(undoCount);
         ImGui::PopStyleColor();
         ImGui::PopID();
      }
      // Clicking outside of the window dismisses it (not on the frame it opened, as the click that opened it may be still pending)
      // (a press on a row makes it the active item, which would read as not hovered and close the window before the row is released)
      if (!m_undoHistoryJustOpened && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
         && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
         open = false;
   }
   ImGui::End();
   m_undoHistoryJustOpened = false;
   if (redoSteps > 0)
   {
      editor.RedoSteps(static_cast<size_t>(redoSteps));
      open = false;
   }
   else if (undoSteps > 0)
   {
      editor.UndoSteps(static_cast<size_t>(undoSteps));
      open = false;
   }
   if (!open)
      m_undoHistoryOpen = false;
}

void EditorChrome::RenderStatusBar(const bool showFullUI)
{
   if (!showFullUI || m_statusBarHeight <= 0.f)
      return;
   EditorUI &editor = m_editor;
   const ImGuiIO &io = ImGui::GetIO();
   const float dpi = editor.m_liveUI.GetDPI();
   const ImGuiViewport *const viewport = ImGui::GetMainViewport();
   const float height = m_statusBarHeight;

   // Key hints of the current context: [key chip] label
   struct Hint
   {
      string key, label;
   };
   vector<Hint> hints;
   const string mod = ModKey;
   EditorUI::AreaEditor areaUnderMouse = EditorUI::AreaEditor::Count;
   bool sidebarUnderMouse = false;
   const bool overArea = editor.GetAreaUnderMouse(areaUnderMouse, sidebarUnderMouse);
   if (editor.m_palette.IsOpen())
      hints = { { "Type", "Search" }, { "Enter", "Run" }, { mod + "+Enter", "Add to Quick Favorites" }, { "Esc", "Close" } };
   else if (io.WantTextInput)
      hints = { { "Enter", "Confirm" }, { "Esc", "Cancel" }, { mod + "+A", "Select All Text" } };
   else if (editor.m_modal.active)
      hints = { { "LMB / Enter", "Confirm" }, { "RMB / Esc", "Cancel" }, { "X Y Z", "Axis" }, { "Shift+X Y Z", "Plane" }, { "0-9", "Exact Value" },
         { mod, editor.m_snapEnabled ? "Hold: Snap Off" : "Hold: Snap On" } };
   else if (editor.m_areaDrag.active)
      hints = { { "Drag inside the area", "Split" }, { "Drag into a neighbour", "Join" }, { "Esc", "Cancel" } };
   else if (editor.m_areaModal != EditorUI::AreaModalKind::None)
      hints = { { "Move Mouse", editor.m_areaModal == EditorUI::AreaModalKind::Split ? "Place Split" : "Choose Area to Remove" }, { "LMB", "Confirm" }, { "RMB / Esc", "Cancel" } };
   else if (editor.m_splitDragNode >= 0)
      hints = { { "Drag", "Resize Areas" }, { "Release LMB", "Confirm" } };
   else if (ImGuizmo::IsUsing())
      hints = { { "Drag", "Transform" }, { mod, editor.m_snapEnabled ? "Hold: Snap Off" : "Hold: Snap On" }, { "Release LMB", "Confirm" } };
   else if (editor.m_adjustPopupOpen)
      hints = { { "Drag", "Adjust" }, { "Double-click", "Type Value" }, { "Click Outside", "Close" } };
   else if (editor.m_boxSelectActive && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !overArea)
      hints = { { "Release LMB", "Select" }, { "Shift", "Add to Selection" }, { "Esc", "Cancel" } };
   else if (overArea && sidebarUnderMouse)
      hints = { { "Drag", "Adjust" }, { "Double-click", "Type Value" }, { "N", "Hide Sidebar" } };
   else if (overArea)
   {
      switch (areaUnderMouse)
      {
      case EditorUI::AreaEditor::Outliner:
         hints = { { "LMB", "Select" }, { mod + "+LMB", "Toggle" }, { "Shift+LMB", "Range" }, { "Double-click", "Frame" }, { mod + "+F", "Filter" } };
         break;
      case EditorUI::AreaEditor::Properties:
         hints = { { "Wheel", "Scroll" }, { "Double-click", "Type Value" }, { mod + "+F", "Search Fields" }, { mod + "+K", "Search to Field" } };
         break;
      case EditorUI::AreaEditor::Script: hints = { { mod + "+S", "Save Table" }, { "F3", "Menu Search" } }; break;
      case EditorUI::AreaEditor::PartLibrary: hints = { { "Wheel", "Scroll" } }; break;
      case EditorUI::AreaEditor::CameraPreview: hints = { { "Live", "Render Continuously" }, { "Refresh", "Update Once" } }; break;
      default: hints = { { "LMB", "Select" }, { "Wheel", "Scroll" } }; break;
      }
   }
   else if (editor.m_plasticTrace.active)
      hints = { { "LMB", editor.m_plasticTrace.holes ? "Place Hole" : "Corner" }, { "Backspace", "Remove Last" }, { "H", editor.m_plasticTrace.holes ? "Outline" : "Holes" },
         { "Enter", "Close Loop" }, { "Esc", "Cancel" } };
   else if (editor.m_addPartType != eItemInvalid)
      hints = { { "LMB", editor.m_addWireRail ? "Place Wire Rail" : editor.m_addWireform ? "Place Metal Wire Guide" : "Place Part" }, { "Esc", "Cancel" } };
   else if (editor.m_pointEditPart && editor.m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive && static_cast<Primitive *>(editor.m_pointEditPart->GetEditable())->m_d.m_plasticGen)
      hints = { { "LMB", "Select Point" }, { "Drag Line", "Bow" }, { "Dbl-click", "Straight" }, { "G", "Move" }, { "Shift+A", "Add Point" }, { "H", static_cast<Primitive *>(editor.m_pointEditPart->GetEditable())->m_plasticEditHoles ? "Outline" : "Holes" },
         { "Del", "Delete" }, { "Tab", "Exit" }, { "F9", "Profile" } };
   else if (editor.m_pointEditPart && editor.m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive && static_cast<Primitive *>(editor.m_pointEditPart->GetEditable())->m_d.m_wireRail)
      hints = { { "LMB", "Select Point" }, { "Dbl-click", "Insert or Delete" }, { "G", "Move" }, { "Gizmo Z", "Height" }, { "Del", "Delete" }, { "Tab", "Exit" }, { "Properties", "Grade, Roll" } };
   else if (editor.m_pointEditPart && editor.m_pointEditPart->GetEditable()->GetItemType() == eItemPrimitive && static_cast<Primitive *>(editor.m_pointEditPart->GetEditable())->m_d.m_wireGen)
      hints = { { "LMB", "Select Point" }, { "G", "Move" }, { "Shift+A", "Add Point" }, { "Del", "Delete" }, { "Tab", "Exit" }, { "Properties", "Metal Wire Guide" } };
   else if (editor.m_pointEditPart)
      hints = { { "LMB", "Select Point" }, { "Shift+LMB", "Toggle" }, { "G", "Move" }, { "Shift+A", "Add Point" }, { "Del", "Delete" }, { "Tab", "Exit" } };
   else
   {
      // Navigation hints follow the navigation preferences
      hints.push_back({ "LMB", "Select" });
      if (!editor.IsInspectMode() && RubberBetweenPosts::CanCreate(editor))
         hints.push_back({ "Shift+A", "Create Rubber Around Selected Posts" });
      hints.push_back({ editor.m_navAltMiddleButton ? "MMB / Alt+LMB" : "MMB", "Orbit" });
      hints.push_back({ "Shift+MMB", "Pan" });
      if (editor.m_navTrackpad)
      {
         hints.push_back({ "Scroll", "Pan" });
         hints.push_back({ mod + "+Scroll", "Zoom" });
      }
      else
         hints.push_back({ "Wheel", "Zoom" });
      if (editor.IsInspectMode())
      {
         hints.push_back({ mod + "+S", "Save Changes" });
         hints.push_back({ "F3", "Menu Search" });
         hints.push_back({ "Shift+F5", "Stop Play Test" });
      }
      else
      {
         hints.push_back({ "G", "Move" });
         hints.push_back({ "R", "Rotate" });
         hints.push_back({ "S", "Scale" });
         hints.push_back({ "Shift+Tab", editor.m_snapEnabled ? "Snap: On" : "Snap: Off" });
         if (editor.m_lastOp.type != EditorUI::LastOperation::Type::None)
            hints.push_back({ "F9", "Adjust Last Operation" });
      }
   }

   // Last operation report on the right, fading out
   const double age = ImGui::GetTime() - editor.m_reportTime;
   const float reportAlpha = age < 3.5 ? 1.f : age < 4.5 ? static_cast<float>(4.5 - age) : 0.f;
   const bool hasReport = reportAlpha > 0.f && !editor.m_report.empty();

   const float pad = 8.f * dpi;
   ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - height));
   ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
   ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
   ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::V(editor.IsInspectMode() ? EditorTheme::PlayBar : EditorTheme::Bar));
   ImGui::Begin("STATUSBAR", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav
         | ImGuiWindowFlags_NoFocusOnAppearing);
   ImGui::PopStyleColor();
   ImGui::PopStyleVar(3);
   ImDrawList *const draw = ImGui::GetWindowDrawList();
   const ImVec2 barMin = ImGui::GetWindowPos();
   const ImVec2 barMax(barMin.x + viewport->Size.x, barMin.y + height);
   draw->AddLine(barMin, ImVec2(barMax.x, barMin.y), EditorTheme::Border, 1.f); // 1 px top border

   const float textH = ImGui::GetFontSize();
   const float textY = barMin.y + 0.5f * (height - textH);
   float rightLimit = barMax.x - pad;
   {
      // Scene statistics at the right end: the collection, the vertices and faces of the meshes, the parts. Recomputed when the table changed
      const PinTable *const table = editor.m_table;
      const uint64_t undoId = editor.m_undo.TopRecordId();
      if (undoId != m_statsUndoId || table->GetParts().size() != m_statsParts)
      {
         m_statsUndoId = undoId;
         m_statsParts = table->GetParts().size();
         m_statsVerts = m_statsFaces = 0;
         for (const IEditable *const part : table->GetParts())
            if (part->GetItemType() == eItemPrimitive)
            {
               const Primitive *const prim = static_cast<const Primitive *>(part);
               m_statsVerts += prim->m_mesh.NumVertices();
               m_statsFaces += prim->m_mesh.NumIndices() / 3;
            }
      }
      const Collection *const collection = editor.m_selection.GetType() == Selection::S_COLLECTION ? editor.m_selection.GetCollection() : nullptr;
      const auto grouped = [](size_t n)
      {
         string digits = std::to_string(n), out;
         for (size_t i = 0; i < digits.size(); i++)
            out += (i > 0 && (digits.size() - i) % 3 == 0 ? ","s : ""s) + digits[i];
         return out;
      };
      const string stats = std::format("{} | Mesh Verts: {} | Mesh Faces: {} | Objects: {}", collection != nullptr ? collection->m_name : "Scene Collection"s, grouped(m_statsVerts), grouped(m_statsFaces), grouped(m_statsParts));
      const float x = barMax.x - pad - ImGui::CalcTextSize(stats.c_str()).x;
      draw->AddText(ImVec2(x, textY), EditorTheme::TextDim, stats.c_str());
      rightLimit = x - 2.f * pad;
   }
   if (hasReport)
   {
      const ImVec2 size = ImGui::CalcTextSize(editor.m_report.c_str());
      const float x = rightLimit - size.x;
      const auto fade = [reportAlpha](ImU32 color) { return (color & 0x00FFFFFFu) | (static_cast<ImU32>(static_cast<float>((color >> 24) & 0xFFu) * reportAlpha) << 24); };
      draw->AddText(ImVec2(x, textY), fade(EditorTheme::Text), editor.m_report.c_str());
      rightLimit = x - 2.f * pad;
   }

   // Hints from the left, as many as fit before the report
   const float chipPadX = 4.f * dpi, chipPadY = 1.f * dpi, gap = 4.f * dpi, hintGap = 12.f * dpi;
   float x = barMin.x + pad;
   for (const Hint &hint : hints)
   {
      const ImVec2 keySize = ImGui::CalcTextSize(hint.key.c_str());
      const ImVec2 labelSize = ImGui::CalcTextSize(hint.label.c_str());
      const float width = keySize.x + 2.f * chipPadX + gap + labelSize.x;
      if (x + width > rightLimit)
         break;
      const ImVec2 chipMin(x, barMin.y + 0.5f * (height - (keySize.y + 2.f * chipPadY)));
      const ImVec2 chipMax(x + keySize.x + 2.f * chipPadX, chipMin.y + keySize.y + 2.f * chipPadY);
      draw->AddRectFilled(chipMin, chipMax, EditorTheme::Rgb(255, 255, 255, 20), 3.f * dpi);
      draw->AddRect(chipMin, chipMax, EditorTheme::Hover, 3.f * dpi);
      draw->AddText(ImVec2(chipMin.x + chipPadX, chipMin.y + chipPadY), EditorTheme::Rgb(135, 135, 135), hint.key.c_str());
      draw->AddText(ImVec2(chipMax.x + gap, textY), EditorTheme::Rgb(135, 135, 135), hint.label.c_str());
      x += width + hintGap;
   }
   ImGui::End();
}

}
