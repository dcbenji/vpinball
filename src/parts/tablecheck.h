// license:GPLv3+

// Table check: the problems found in a table (script, timers, lights, resources,...), as a list of issues.
// PinTable::CheckTable runs the checks (headless, the table is not modified). AuditTable formats them as text for
// the Win32 editor and the '-audit' command, the live editor lists them in its Table Check pane.

#pragma once

#include <functional>

class IEditable;
class Material;
class PinTable;

enum class TableIssueSeverity : uint8_t
{
   Error,
   Warning,
   Info
};

// The check that reported an issue (a list of issues can be grouped by check, in this order)
enum class TableCheckId : uint8_t
{
   ScriptDuplicate, // Same sub or function declared twice
   ScriptExecute, // Script uses 'Execute'
   Glass, // Glass height
   BallMapping, // Legacy ball spherical mapping
   LoadFix, // Invalid data fixed when the table was loaded
   StaticReferenced, // Static part referenced from the script
   TextboxDMD, // Legacy textbox used as a DMD
   FastTimer, // Timer faster than 60 FPS
   Translucency, // Translucency on an opaque primitive
   LightIntensity, // Negative light intensity
   VideoCap, // Deprecated flasher VideoCap API
   VPM, // VPinMAME controller setup
   OutsidePlayfield, // Part outside the playfield
   MissingResource, // Image or material name that is not in the table
   DuplicateName, // Parts sharing a name
   Collider, // Collidable part that can not collide
   DragPoints, // Too few drag points, or two on the same spot
   SelfCrossing, // Outline that crosses itself
   UnusedMaterial, // Material not used by any part
   Sound, // Sound format
   Backdrop, // Backdrop image not in the table
   Totals, // Size totals (sounds, images, primitives)
   Count
};

const char *TableCheckTitle(TableCheckId check);

struct TableIssue
{
   TableIssueSeverity severity = TableIssueSeverity::Warning;
   TableCheckId check = TableCheckId::Count;
   string message; // Without the severity
   IEditable *part = nullptr; // Part the issue is about, if any
   Material *material = nullptr; // Material the issue is about, if any
};

struct TableCheckOptions
{
   bool sound = false; // Sound checks: they create an audio player (sound device settings) to read the sound formats
   bool totals = false; // Size totals of the sounds, images and primitives (Info issues)
   bool extended = false; // The table check's own checks (parts, resources,...), on top of the audit checks: AuditTable leaves them out
};

// Calls fn for each image and material name stored by the parts (part, field label, name) and by the table settings (part is null)
void ForEachResourceReference(const PinTable &table, const std::function<void(IEditable *part, const char *field, const string &name, bool isImage)> &fn);

// The audit text of the Win32 editor and the '-audit' command: one line per issue, prefixed by ". Error: " or ". Warning: "
string FormatTableAudit(const vector<TableIssue> &issues);
