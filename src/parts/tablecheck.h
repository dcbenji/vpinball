// license:GPLv3+

// Table check: the problems found in a table (script, timers, lights, resources,...), as a list of issues.
// PinTable::CheckTable runs the checks (headless, the table is not modified). AuditTable formats them as text for
// the Win32 editor and the '-audit' command, the live editor lists them in its Table Check pane.

#pragma once

#include <functional>

class Collection;
class IEditable;
class Material;
class PinTable;
class Texture;

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
   TimerSub, // Enabled timer without a sub in the script
   EventSub, // Event sub of an object that is not in the table
   ScriptReference, // Sound, image or material named by the script that is not in the table
   Translucency, // Translucency on an opaque primitive
   LightIntensity, // Negative light intensity
   VideoCap, // Deprecated flasher VideoCap API
   VPM, // VPinMAME controller setup
   OutsidePlayfield, // Part outside the playfield
   MissingResource, // Image or material name that is not in the table
   DuplicateName, // Parts sharing a name
   Collider, // Collidable part that can not collide
   HeavyCollider, // Collidable primitive with many faces
   DragPoints, // Too few drag points, or two on the same spot
   SelfCrossing, // Outline that crosses itself
   UnusedMaterial, // Material not used by any part
   UnusedImage, // Image not used by any part, table setting, table description or script string
   LargeImage, // Image over 4096 pixels
   EmptyCollection, // Collection without parts
   Sound, // Sound format
   Backdrop, // Backdrop image not in the table
   Totals, // Size totals (sounds, images, primitives)
   Count
};

const char *TableCheckTitle(TableCheckId check);

// The one click fix of an issue, set by the check only when it is safe (the live editor applies it, see TableCheckFixOp)
enum class TableFix : uint8_t
{
   None,
   DisableTimer, // Untick the timer of the part
   ClearLightIntensity, // Set the negative intensity of the light to 0
   ClearBackdropImage, // Clear the backdrop image name of the view set 'index'
   DeleteCollection, // Delete the (empty) collection
   DeleteImage, // Delete the (unused) image
};

struct TableIssue
{
   TableIssueSeverity severity = TableIssueSeverity::Warning;
   TableCheckId check = TableCheckId::Count;
   string message; // Without the severity
   IEditable *part = nullptr; // Part the issue is about, if any
   Material *material = nullptr; // Material the issue is about, if any
   Texture *image = nullptr; // Image the issue is about, if any
   Collection *collection = nullptr; // Collection the issue is about, if any
   int index = -1; // Backdrop view set (ViewSetupID) of a backdrop issue
   TableFix fix = TableFix::None;
};

// Issue counts per severity
struct TableCheckSummary
{
   int counts[3] {}; // Errors, warnings, infos
   TableCheckId top = TableCheckId::Count; // The check with the most errors and warnings, and their number
   int topCount = 0;
   static TableCheckSummary Of(const vector<TableIssue> &issues)
   {
      TableCheckSummary summary;
      int perCheck[static_cast<int>(TableCheckId::Count)] {};
      for (const TableIssue &issue : issues)
      {
         ++summary.counts[static_cast<int>(issue.severity)];
         if (issue.severity != TableIssueSeverity::Info && ++perCheck[static_cast<int>(issue.check)] > summary.topCount)
         {
            summary.top = issue.check;
            summary.topCount = perCheck[static_cast<int>(issue.check)];
         }
      }
      return summary;
   }
};

struct TableCheckOptions
{
   bool sound = false; // Sound checks: they create an audio player (sound device settings) to read the sound formats
   bool totals = false; // Size totals of the sounds, images and primitives (Info issues)
   bool extended = false; // The table check's own checks (parts, resources,...), on top of the audit checks: AuditTable leaves them out
   bool script = true; // The checks that read the script (the live editor leaves them out for a very big script)
};

// Calls fn for each image and material name stored by the parts (part, field label, name) and by the table settings (part is null)
// (the script, which may name images in its strings, is not part of it)
void ForEachResourceReference(const PinTable &table, const std::function<void(IEditable *part, const char *field, const string &name, bool isImage)> &fn);

// The audit text of the Win32 editor and the '-audit' command: one line per issue, prefixed by ". Error: " or ". Warning: "
string FormatTableAudit(const vector<TableIssue> &issues);
