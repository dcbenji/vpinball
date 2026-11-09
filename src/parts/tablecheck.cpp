// license:GPLv3+

#include "core/stdafx.h"
#include "tablecheck.h"

#include "audio/AudioPlayer.h"
#include "parts/flasher.h"
#include "parts/light.h"
#include "parts/primitive.h"
#include "parts/Sound.h"
#include "parts/surface.h"
#include "parts/textbox.h"
#include "parts/timer.h"
#include "renderer/Texture.h"

const char *TableCheckTitle(const TableCheckId check)
{
   switch (check)
   {
   case TableCheckId::ScriptDuplicate: return "Duplicate script declarations";
   case TableCheckId::ScriptExecute: return "Script 'Execute' command";
   case TableCheckId::Glass: return "Glass height";
   case TableCheckId::BallMapping: return "Ball mapping";
   case TableCheckId::LoadFix: return "Fixed when loaded";
   case TableCheckId::StaticReferenced: return "Static parts used by the script";
   case TableCheckId::TextboxDMD: return "Textbox used as DMD";
   case TableCheckId::FastTimer: return "Fast timers";
   case TableCheckId::Translucency: return "Translucency on opaque primitives";
   case TableCheckId::LightIntensity: return "Light intensity";
   case TableCheckId::VideoCap: return "Flasher VideoCap API";
   case TableCheckId::VPM: return "VPinMAME setup";
   case TableCheckId::Sound: return "Sounds";
   case TableCheckId::Backdrop: return "Backdrop images";
   case TableCheckId::Totals: return "Totals";
   case TableCheckId::Count: break;
   }
   return "";
}

string FormatTableAudit(const vector<TableIssue> &issues)
{
   std::stringstream ss;
   bool listed = false; // An issue was listed: "No issue identified." goes before the totals otherwise
   for (const TableIssue &issue : issues)
   {
      if (issue.check == TableCheckId::Totals)
      {
         if (!listed)
            ss << "No issue identified.\r\n";
         listed = true;
         ss << ". " << issue.message << "\r\n";
         continue;
      }
      listed = true;
      ss << (issue.severity == TableIssueSeverity::Error ? ". Error: " : issue.severity == TableIssueSeverity::Warning ? ". Warning: " : ". ") << issue.message << "\r\n";
   }
   if (!listed)
      ss << "No issue identified.\r\n";
   return "Table audit:\r\n" + ss.str();
}

vector<TableIssue> PinTable::CheckTable(const TableCheckOptions &options) const
{
   // Perform a simple table audit (disable lighting vs static, script reference of static parts, png vs webp, hdr vs exr,...)
   vector<TableIssue> issues;
   const auto add = [&issues](const TableIssueSeverity severity, const TableCheckId check, string message, IEditable *part = nullptr)
   { issues.push_back({ severity, check, std::move(message), part }); };
   constexpr TableIssueSeverity Error = TableIssueSeverity::Error, Warning = TableIssueSeverity::Warning, Info = TableIssueSeverity::Info;

   vector<string> functions;
   vector<string> identifiers;
   ParseScript(m_script_text, functions, identifiers,
      [&add](const string &functionName, int line) { add(Error, TableCheckId::ScriptDuplicate, "Duplicate declaration of '" + functionName + "' in script at line " + std::to_string(line)); });

   if (FindIndexOf(identifiers, "execute"s) != -1)
      add(Warning, TableCheckId::ScriptExecute, "Scripts seems to use the 'Execute' command. This command triggers computer security checks and will likely cause stutters during play.");

   if (m_glassBottomHeight > m_glassTopHeight)
      add(Warning, TableCheckId::Glass, "Glass height seems invalid: bottom is higher than top");

   if (m_glassBottomHeight < INCHESTOVPU(2) || m_glassTopHeight < INCHESTOVPU(2))
      add(Warning, TableCheckId::Glass, "Glass height seems invalid: glass is below 2\"");

   if (m_ballSphericalMapping)
      add(Warning, TableCheckId::BallMapping, "Ball uses legacy 'spherical mapping', it will be rendered like a 2D object and therefore will look bad in VR, stereo or headtracking");

   for (const string &fix : m_loadFixes)
      add(Warning, TableCheckId::LoadFix, fix);

   // Search for inconsistencies in the table parts
   bool hasPulseTimer = false, hasPinMameTimer = false;
   for (const auto part : m_vedit)
   {
      auto type = part->GetItemType();
      Primitive *const prim = type == eItemPrimitive ? (Primitive *)part : nullptr;
      Light *const light = type == eItemLight ? (Light *)part : nullptr;
      Surface *const surf = type == eItemSurface ? (Surface *)part : nullptr;
      Textbox *const textbox = type == eItemTextbox ? (Textbox *)part : nullptr;

      // Referencing a static object from script (ok if it is for reading properties, not for writing)
      if (type == eItemPrimitive && prim->m_d.m_staticRendering && FindIndexOf(identifiers, lowerCase(prim->GetName())) != -1)
         add(Warning, TableCheckId::StaticReferenced, "Primitive '" + prim->GetName() + "' seems to be referenced from the script while it is marked as static (most properties of a static object may not be modified at runtime).", part);

      if (type == eItemTextbox && (textbox->m_d.m_isDMD || StrFindNoCase(textbox->m_d.m_text, "DMD"s) != string::npos))
         add(Warning, TableCheckId::TextboxDMD, "legacy Textbox '" + textbox->GetName() + "' is used for DMD rendering. It should be replaced by a flasher to get better rendering.", part);

      if (type == eItemTimer) {
         const string name = lowerCase(((Timer *)part)->GetName());
         hasPulseTimer |= name == "pulsetimer";
         hasPinMameTimer |= name == "pinmametimer";
      }

      // Warning on very fast timers (lower than 5ms)
      switch (type)
      {
      case eItemBall:
      // eItemDecal
      case eItemPartGroup:
      // eItemPrimitive
      case eItemTextbox:
      case eItemDispReel:
      case eItemSurface:
      case eItemTimer:
      case eItemLight:
      case eItemRamp:
      case eItemPlunger:
      case eItemSpinner:
      case eItemTrigger:
      case eItemKicker:
      case eItemRubber:
      case eItemFlasher:
      case eItemLightSeq:
      case eItemHitTarget:
      case eItemBumper:
      case eItemFlipper:
      case eItemGate:
         if (part->m_timerEnabled && part->m_timerInterval != -1 && part->m_timerInterval != -2 && part->m_timerInterval < 17)
            add(Warning, TableCheckId::FastTimer, "Part '" + part->GetName() + "' uses a timer with a very short period of " + std::to_string(part->m_timerInterval) + "ms, below a 60FPS framerate. This will likely cause stutters and the table will not support 'frame pacing'.", part);
         break;
      // eItemTable
      // eItemLightCenter
      // eItemDragPoint
      // eItemCollection
      default: break;
      }

      if (type == eItemPrimitive && prim->m_d.m_visible
         && prim->m_d.m_disableLightingBelow != 1.f && !prim->m_d.m_staticRendering
         && (!GetMaterial(prim->m_d.m_szMaterial)->m_bOpacityActive || GetMaterial(prim->m_d.m_szMaterial)->m_fOpacity == 1.f)
         && (GetImage(prim->m_d.m_szImage) == nullptr || GetImage(prim->m_d.m_szImage)->IsOpaque()))
         add(Warning, TableCheckId::Translucency, "Primitive '" + prim->GetName() + "' uses translucency (lighting from below) while it is fully opaque. Translucency will be discarded.", part);

      if (type == eItemLight && light->m_d.m_intensity < 0.f)
         add(Error, TableCheckId::LightIntensity, "Light '" + light->GetName() + "' has a negative intensity.", part);
      if (type == eItemLight && light->m_d.m_intensity_scale < 0.f)
         add(Error, TableCheckId::LightIntensity, "Light '" + light->GetName() + "' has a negative intensity scale.", part);

      // Disabled as this is now enforced in the rendering
      // Enabling translucency (light from below) won't work with static parts: otherwise the rendering will be different in VR/Headtracked vs desktop modes. It also needs a non opaque alpha.
      //if (type == eItemPrimitive && prim->m_d.m_disableLightingBelow != 1.f && prim->m_d.m_staticRendering)
      //   ss << ". Warning: Primitive '" << prim->GetName() << "' has translucency enabled but is also marked as static. Translucency will not be applied on desktop, and it will look different between VR/headtracked and desktop.\r\n";
      //if (type == eItemSurface && surf->m_d.m_disableLightingBelow != 1.f && surf->StaticRendering())
      //   ss << ". Warning: Wall '" << surf->GetName() << "' has translucency enabled but will be statically rendered (not droppable with opaque materials). Translucency will not be applied on desktop, and it will look different between VR/headtracked and desktop.\r\n";
   }

   // The flasher VideoCap API (capturing the window of an external application) is deprecated in favor of plugins and may be removed in a future version
   if (FindIndexOf(identifiers, "videocapwidth"s) != -1 || FindIndexOf(identifiers, "videocapheight"s) != -1 || FindIndexOf(identifiers, "videocapupdate"s) != -1)
   {
      // Name the flashers referenced as '<name>.VideoCap...' (whole name, case insensitive like VBScript), or the script for an indirect use
      const string script = lowerCase(m_script_text);
      bool attributed = false;
      for (const auto part : m_vedit)
      {
         if (part->GetItemType() != eItemFlasher)
            continue;
         const string ref = lowerCase(string(part->GetName())) + ".videocap";
         for (size_t pos = script.find(ref); pos != string::npos; pos = script.find(ref, pos + 1))
         {
            if (pos > 0 && (isalnum(static_cast<unsigned char>(script[pos - 1])) || script[pos - 1] == '_'))
               continue; // Part of a longer name
            add(Error, TableCheckId::VideoCap, "Flasher '" + part->GetName()
               + "' uses the deprecated 'VideoCap' API (VideoCapWidth/VideoCapHeight/VideoCapUpdate). It is superseded by plugins and may be removed in a future version.", part);
            attributed = true;
            break;
         }
      }
      if (!attributed)
         add(Error, TableCheckId::VideoCap, "script uses the deprecated Flasher 'VideoCap' API (VideoCapWidth/VideoCapHeight/VideoCapUpdate). It is superseded by plugins and may be removed in a future version.");
   }

   if ((FindIndexOf(identifiers, "loadvpm"s) != -1) || (FindIndexOf(identifiers, "loadvpmalt"s) != -1))
   {
      if (!hasPinMameTimer)
         add(Warning, TableCheckId::VPM, "VPM controller is used but table is missing a Timer object named 'PinMAMETimer'.");
      if (FindIndexOf(identifiers, "vpminit"s) == -1)
         add(Warning, TableCheckId::VPM, "VPM controller is used but vpmInit is not called. pause/resume/exit will likely exhibit bugs and physic outputs won't be supported.");
   }

   if (!hasPulseTimer && (FindIndexOf(identifiers, "vpmtimer"s) != -1))
      add(Warning, TableCheckId::VPM, "script uses 'vpmTimer' but table is missing a Timer object named 'PulseTimer'. vpmTimer will not work as expected.");

   if (options.sound)
   {
      // Only used to query sound informations: no need for the optional haptic output
      auto audioPlayer = std::make_unique<VPX::AudioPlayer>(m_settings.GetPlayer_SoundDeviceBG(), m_settings.GetPlayer_SoundDevice(),
         static_cast<VPX::SoundConfigTypes>(m_settings.GetPlayer_Sound3D()), static_cast<VPX::BackglassConfigTypes>(m_settings.GetPlayer_Sound3DBackglass()), ""s, VPX::SNDCFG_SND3D2CH);
      for (const auto sound : m_vsound)
      {
         auto specs = audioPlayer->GetSoundInformations(sound);
         if (specs.nChannels > 1 && sound->GetOutputTarget() == VPX::SNDOUT_TABLE)
            add(Error, TableCheckId::Sound, "sound '" + sound->GetName() + "' is used for playfield physical sound but has multiple channels (not mono).");
      }
      audioPlayer = nullptr;
   }

   // Also output a log of the table file content to allow easier size optimization
   if (options.totals)
   {
      size_t totalSize = 0, totalGpuSize = 0;
      for (const auto sound : m_vsound)
      {
         //ss << "  . Sound: '" << sound->m_name << "', size: " << SizeToReadable(sound->m_cdata) << "\r\n";
         totalSize += sound->GetFileSize();
      }
      add(Info, TableCheckId::Totals, "Total sound size: " + SizeToReadable(totalSize));

      totalSize = 0;
      for (const auto image : m_vimage)
      {
         size_t imageSize = image->GetFileSize();
         size_t gpuSize = image->GetEstimatedGPUSize();
         //ss << "  . Image: '" << image->m_name << "', size: " << SizeToReadable(imageSize) << ", GPU mem size: " << SizeToReadable(gpuSize) << "\r\n";
         totalSize += imageSize;
         totalGpuSize += gpuSize;
      }
      add(Info, TableCheckId::Totals, "Total image size: " + SizeToReadable(totalSize) + " in VPX file, at least " + SizeToReadable(totalGpuSize) + " in GPU memory when played");
   }

   // Backdrop images referenced per view mode but not present in the table would render black.
   // An empty name or the "<None>" sentinel means no backdrop is set.
   static const char* const bgSetNames[NUM_BG_SETS] = { "Desktop", "Cabinet", "Full Single Screen" };
   for (unsigned int i = 0; i < NUM_BG_SETS; ++i)
      if (const string& bg = m_BG_image[i]; !bg.empty() && !StrCompareNoCase(bg, g_szNoneSelection) && GetImage(bg) == nullptr)
         add(Warning, TableCheckId::Backdrop, string(bgSetNames[i]) + " backdrop image '" + bg + "' is not present in the table (renders black)");

   if (options.totals)
   {
      int nPrimTris = 0, primMemSize = 0;
      for (const auto part : m_vedit)
         if (part->GetItemType() == eItemPrimitive && ((Primitive *)part)->m_d.m_use3DMesh /* && ((Primitive *)part)->m_d.m_visible */ )
         {
            primMemSize += (((Primitive *)part)->m_mesh.NumIndices() > 65536 ? 4 : 2) * (int) ((Primitive *)part)->m_mesh.NumIndices();
            primMemSize += (int) ((Primitive *)part)->m_mesh.NumVertices() * sizeof(Vertex3D_NoTex2);
            nPrimTris += (int) ((Primitive *)part)->m_mesh.NumIndices() / 3;
         }
      add(Info, TableCheckId::Totals, "Total number of faces used in primitives: " + std::to_string(nPrimTris) + ", needing " + SizeToReadable(primMemSize) + " in GPU memory when played");
   }

   return issues;
}
