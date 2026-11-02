// license:GPLv3+

#pragma once

#include "Selection.h"
#include "unordered_dense.h"

class PartGroup;
class IEditable;

namespace VPX::EditorUI
{

class EditorUI;
class EditorUIPart;

// Left side panel with one tab per object kind (parts tree, lights, materials, images, render probes,
// collections, view setups & sounds), used to select the edited object.
class OutlinerPanel
{
public:
   explicit OutlinerPanel(EditorUI &editor)
      : m_editor(editor)
   {
   }

   void Render(float topBarHeight);

   // Width of the panel in logical pixels (scaled by the UI DPI)
   static constexpr float PaneWidth = 200.f;

private:
   // Filter text split in lowercase tokens: 'n:x' or bare words (name contains), 't:x' (type starts with), 'm:x' (material contains)
   struct FilterTokens
   {
      vector<string> names;
      vector<string> types;
      vector<string> materials;
      bool empty() const { return names.empty() && types.empty() && materials.empty(); }
   };
   // Breadcrumb element: kind 0 plain text, 1 clears the selection, 2 selects sel, 3 brings tab to front, 4 selects the part of group
   struct Crumb
   {
      string label;
      int kind;
      Selection sel;
      const PartGroup *group;
      int tab;
   };

   void ParseFilter();
   bool MatchesFilter(const string &name) const; // Name tokens only (non part lists)
   bool MatchesPartCached(size_t index) const; // All tokens, for m_editables[index] (needs RefreshFilterCache)
   void RefreshFilterCache();
   void SelectViewSetup(int viewSetup); // Select a preview camera and switch the view to it
   void UpdateSelectionHistory();
   void RenderContextHeader(); // Breadcrumb and recent selections, drawn at the top of each tab

   // Render a resource list (images, sounds, materials) with ctrl/shift multi selection.
   // multiSel holds all the selected items, anchor is the last clicked one for range selection.
   // When asTree is set, the list is wrapped in a twirl-down node labelled label.
   template <class T>
   void RenderResourceList(const char *label, bool asTree, vector<T *> &items, vector<T *> &multiSel, T *&anchor, const std::function<string(T *)> &nameOf);

   EditorUI &m_editor;
   string m_filter;
   bool m_syncToSelection = false; // When enabled, the tree is expanded & scrolled to reveal the selected part on selection change
   std::shared_ptr<EditorUIPart> m_lastSyncedPart; // Last selection revealed in the tree while syncing

   string m_parsedFilter; // Filter text the tokens were parsed from
   FilterTokens m_tokens;
   bool m_openMatchesPending = false; // Filter changed: open the groups containing matches once the tree is drawn

   int m_pendingTab = -1; // Tab to bring to front on next render (-1: none)
   int m_activeTab = 0; // Tab drawn last frame
   Selection m_lastSeenSelection;
   vector<Selection> m_recentSelections; // Most recent first, includes the current selection

   // Per frame scratch data of the parts tree (members to avoid reallocating)
   ankerl::unordered_dense::set<const PartGroup *> m_matchedGroups;
   ankerl::unordered_dense::set<const PartGroup *> m_visibleGroups;
   ankerl::unordered_dense::map<const PartGroup *, int> m_groupCounts;
   vector<const PartGroup *> m_groupStack;
   vector<uint8_t> m_showPart; // Per m_editables entry: part is listed (matches the filter or is in a matching group)
   vector<std::shared_ptr<EditorUIPart>> m_lightList; // Name sorted lights and flashers matching the filter, rebuilt with the cache

   // Lowercase names/materials of m_editables (only filled when a matching token exists), refreshed when the part count,
   // filter or view mode changes, or every second to catch renames
   vector<string> m_lowerNames;
   vector<string> m_lowerMaterials;
   size_t m_cacheSize = 0;
   int m_filterGeneration = 0;
   int m_cacheGeneration = -1;
   bool m_cacheBackdrop = false;
   double m_cacheTime = -10.0;
   vector<Crumb> m_crumbs;
};

}
