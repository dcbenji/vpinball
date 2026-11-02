// license:GPLv3+

#pragma once

#include <optional>

#include "audio/AudioPlayer.h"
#include "PinnedPanel.h"

class Collection;
class IEditable;
class Material;
class Texture;
class RenderProbe;
struct WhereUsedInfo;
namespace VPX
{
class Sound;
}

namespace VPX::EditorUI
{

class EditorUI;
class PropertyPane;

// Right side panel displaying the properties of the current selection (table,
// camera view setup, image, material, render probe, sound or editable part).
class PropertiesPanel
{
   friend class PinnedPanel; // Harvests the field definitions of pinned objects through the panes below

public:
   explicit PropertiesPanel(EditorUI &editor);
   ~PropertiesPanel();

   void Render(float topBarHeight);

   // Width of the panel in logical pixels (scaled by the UI DPI)
   static constexpr float PaneWidth = 280.f;

   // Main visual material of a part (the one its header chip opens), empty if none
   static string PartMaterial(const IEditable *part);

private:
   void TableProperties(PropertyPane &props);
   void ImageProperties(PropertyPane &props, Texture *image);
   void RenderProbeProperties(PropertyPane &props, RenderProbe *probe);
   void CameraProperties(PropertyPane &props, int bgSet);
   void MaterialProperties(PropertyPane &props, Material *material);
   void SoundProperties(PropertyPane &props, VPX::Sound *sound);
   void CollectionProperties(PropertyPane &props, Collection *collection);

   // Rebuilds renderables & physics of the parts modified through the pane: the edited one, plus all
   // co-edited parts of a multi-edit selection (also resyncing their editor visibility overrides)
   void RefreshEditedParts(PropertyPane &props);

   // Resource actions offered at the top of the image/sound/material property panes,
   // matching the actions of the corresponding Win32 manager dialogs
   void ImageActions(PropertyPane &props);
   void SoundActions(PropertyPane &props);
   void MaterialActions(PropertyPane &props);
   void RequestConfirm(const string &message, const std::function<void()> &action);
   void UsersSection(PropertyPane &props, const vector<WhereUsedInfo> &whereUsed);
   void RenderPopups();

   // Curated sections: the object's Look and Transform fields (drawn from the pin field registry, filled by a harvest of its pane)
   // right after the header, then the full sheet collapsed under an "Advanced" header. Only for the object types with curation rules
   struct Curation
   {
      PinKind kind = PinKind::Table;
      string name;
      void *obj = nullptr; // Object resolved for the definitions (PinTable*, Material* or IEditable*)
      unsigned stamp = 0; // Harvest the lists below were built from
      vector<const PinnedPanel::Def *> look;
      vector<const PinnedPanel::Def *> transform;
      int advancedCount = 0; // Fields of the full sheet not in the curated sections
      bool active = false;
   };
   void PrepareCuration();
   void DrawCuration(PropertyPane &props);
   void EndCuration(PropertyPane &props);
   Curation m_curation;
   bool m_reharvest = false; // An edit may have changed the fields the pane shows (e.g. material type): harvest again once idle
   bool m_advancedIndent = false;
   Material *m_pendingMaterialSel = nullptr; // Header material chip clicked: selected once the panel is drawn

   EditorUI &m_editor;
   bool m_selectLiveTab = true;

   // Results of the asynchronous SDL file dialogs used by the resource actions. Since the dialogs are
   // asynchronous, the item selection at the time the action was requested is captured along the way.
   std::shared_ptr<vector<string>> m_pendingImageImport;
   std::shared_ptr<string> m_pendingImageReimport;
   Texture *m_pendingImageReimportTarget = nullptr;
   std::shared_ptr<string> m_pendingImageExport;
   vector<Texture *> m_pendingImageExportSel;
   std::shared_ptr<vector<string>> m_pendingSoundImport;
   std::shared_ptr<string> m_pendingSoundReimport;
   VPX::Sound *m_pendingSoundReimportTarget = nullptr;
   std::shared_ptr<string> m_pendingSoundExport;
   vector<VPX::Sound *> m_pendingSoundExportSel;
   std::shared_ptr<string> m_pendingMaterialImport;
   std::shared_ptr<string> m_pendingMaterialExport;
   vector<Material *> m_pendingMaterialExportSel;

   // Pending destructive action confirmation popup
   string m_confirmMessage;
   std::function<void()> m_confirmAction;

   // When enabled, exported resource files are named after the object name instead of the import file name
   bool m_exportUseNames = false;
   // Last resource action result displayed in the actions section (e.g. files not found on reimport)
   string m_actionStatus;

   // Sound preview state: the sound whose playback was started from the pane, and whether
   // playback was actually observed (PlaySound commands are dispatched asynchronously)
   VPX::Sound *m_playingSound = nullptr;
   bool m_playingSoundObserved = false;
   VPX::Sound *m_soundInfoFor = nullptr;
   std::optional<VPX::SoundSpec> m_soundInfo;
};

}
