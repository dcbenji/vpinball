// license:GPLv3+

#pragma once

#include "imgui/imgui.h"

#include <SDL3/SDL_process.h>

#include <filesystem>

class PinTable;

namespace VPX::EditorUI
{

class EditorUI;

// Table script editor, hosted by an editor area (Blender's editor type switcher): a basic embedded text editor by
// default. Alternatively, when an external editor command is configured, the script
// can be written to a temporary file opened in that editor, and synchronized back to
// the table whenever the file changes on disk.
// There is one script (the table's) and one external editing session, so a single instance draws every Script area: each area is its own
// ImGui window (own scroll; the text cursor is the one of the area being typed in), and each keystroke is applied to the table at once, so
// the other Script areas show it on the next frame.
class ScriptPanel
{
public:
   explicit ScriptPanel(EditorUI &editor);
   ~ScriptPanel();

   void Update(); // Every frame, whether the editor is shown or not: external editing session housekeeping
   // Window filling the given area rect, header included; leafId: the area (its window and its editor type button)
   void RenderInArea(const ImVec2 &pos, const ImVec2 &size, int leafId);

private:
   void OpenInExternalEditor();
   void SyncExternalScript();
   void StopExternalSession();

   EditorUI &m_editor;

   string m_extEditorCmd; // External editor command, mirrored to the Editor.ExternalScriptEditor app setting
   bool m_wrap = true; // Word wrap on (off for very long scripts); decided while the editor is not active
   bool m_extCmdLoaded = false;

   // External editing session: m_script_text of the armed table is written to a temporary file,
   // opened in the configured external editor, then written back whenever the file changes on disk
   PinTable *m_extTable = nullptr;
   std::filesystem::path m_extFile;
   std::filesystem::file_time_type m_extFileTime {};
   SDL_Process *m_extProcess = nullptr;
};

}
