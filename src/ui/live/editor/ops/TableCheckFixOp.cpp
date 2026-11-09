// license:GPLv3+

#include "core/stdafx.h"
#include "TableCheckFixOp.h"

#include "core/pinundo.h"
#include "parts/Collection.h"
#include "parts/light.h"
#include "physics/PhysicsEngine.h"
#include "renderer/Renderer.h"
#include "renderer/Texture.h"
#include "ui/live/EditorUI.h"
#include "ui/live/editor/TableCheckPane.h"

namespace VPX::EditorUI
{

const char *TableCheckFixOp::Label(const TableFix fix)
{
   switch (fix)
   {
   case TableFix::DisableTimer: return "Disable Timer";
   case TableFix::ClearLightIntensity: return "Set Intensity to 0";
   case TableFix::ClearBackdropImage: return "Clear Backdrop Image";
   case TableFix::DeleteCollection: return "Delete Collection";
   case TableFix::DeleteImage: return "Delete Image";
   case TableFix::None: break;
   }
   return "";
}

bool TableCheckFixOp::IsApplicable(const PinTable *table, const TableIssue &issue)
{
   switch (issue.fix)
   {
   case TableFix::DisableTimer: return issue.part != nullptr && table->HasPart(issue.part) && issue.part->m_timerEnabled;
   case TableFix::ClearLightIntensity:
      return issue.part != nullptr && table->HasPart(issue.part) && issue.part->GetItemType() == eItemLight && static_cast<const Light *>(issue.part)->m_d.m_intensity < 0.f;
   case TableFix::ClearBackdropImage: return issue.index >= 0 && issue.index < NUM_BG_SETS && !table->m_BG_image[issue.index].empty();
   // The issue may be older than the table: a collection or image is deleted only if it is still in the table and still unused
   case TableFix::DeleteCollection:
      return std::ranges::find(table->GetCollections(), issue.collection) != table->GetCollections().end() && issue.collection->GetParts().empty();
   case TableFix::DeleteImage:
   {
      if (std::ranges::find(table->GetImageList(), issue.image) == table->GetImageList().end())
         return false;
      bool used = false;
      ForEachResourceReference(*table, [&used, &issue](IEditable *, const char *, const string &name, const bool isImage) { used |= isImage && StrCompareNoCase(name, issue.image->m_name); });
      return !used;
   }
   case TableFix::None: break;
   }
   return false;
}

vector<IEditable *> TableCheckFixOp::Apply(PinTable *table, PinUndo &undo, const vector<const TableIssue *> &issues)
{
   vector<IEditable *> changed;
   if (table->m_liveBaseTable != nullptr)
      return changed; // The live copy of a play test: no undo, and its images and collections are owned by its base table
   for (const TableIssue *const issue : issues)
   {
      if (!IsApplicable(table, *issue))
         continue;
      switch (issue->fix)
      {
      case TableFix::DisableTimer:
         undo.MarkForUndo(issue->part);
         issue->part->m_timerEnabled = false;
         changed.push_back(issue->part);
         break;

      case TableFix::ClearLightIntensity:
      {
         Light *const light = static_cast<Light *>(issue->part);
         undo.MarkForUndo(light);
         light->put_Intensity(0.f); // Also updates the current intensity
         changed.push_back(light);
         break;
      }

      case TableFix::ClearBackdropImage:
      {
         const int set = issue->index;
         const auto snapshot = [table, set]() -> std::function<void()> { return [table, set, name = table->m_BG_image[set]]() { table->m_BG_image[set] = name; }; };
         undo.MarkForAction(snapshot(), snapshot);
         table->m_BG_image[set].clear();
         break;
      }

      case TableFix::DeleteCollection:
      {
         const auto &collections = table->GetCollections();
         const auto it = std::ranges::find(collections, issue->collection);
         CComObject<Collection> *const collection = *it;
         const size_t index = it - collections.begin();
         // The removed collection stays alive in the table (see RemoveCollection): undo puts it back at its place, redo removes it again
         const auto snapshot = [table, collection]() -> std::function<void()>
         {
            const auto &list = table->GetCollections();
            const auto pos = std::ranges::find(list, collection);
            if (pos == list.end())
               return [table, collection]() { table->RemoveCollection(collection); };
            return [table, collection, at = static_cast<size_t>(pos - list.begin())]() { table->RestoreCollection(collection, at); };
         };
         undo.MarkForAction([table, collection, index]() { table->RestoreCollection(collection, index); }, snapshot);
         table->RemoveCollection(collection);
         break;
      }

      case TableFix::DeleteImage:
      {
         // While the image is out of the table, the undo records own it: undo puts the same object back at its place (so the textures
         // the renderer made from it stay valid), redo takes it out again. It is deleted with the last record holding it, when out.
         struct Detached
         {
            Detached(PinTable *t, Texture *i)
               : table(t)
               , image(i)
            {
               s_ownedImages++;
            }
            PinTable *const table;
            Texture *const image;
            bool inTable = true;
            ~Detached()
            {
               s_ownedImages--;
               if (!inTable)
                  delete image;
            }
         };
         const auto state = std::make_shared<Detached>(table, issue->image);
         const auto snapshot = [state]() -> std::function<void()>
         {
            if (!state->inTable)
               return [state]()
               {
                  state->table->DetachImage(state->image);
                  state->inTable = false;
               };
            const auto &images = state->table->GetImageList();
            return [state, at = static_cast<size_t>(std::ranges::find(images, state->image) - images.begin())]()
            {
               state->table->AttachImage(state->image, at);
               state->inTable = true;
            };
         };
         const size_t index = table->DetachImage(issue->image);
         state->inTable = false;
         undo.MarkForAction(
            [state, index]()
            {
               state->table->AttachImage(state->image, index);
               state->inTable = true;
            },
            snapshot);
         break;
      }

      case TableFix::None: break;
      }
   }
   return changed;
}

vector<const TableIssue *> TableCheckFixOp::Confirmed(const PinTable *table, const vector<const TableIssue *> &issues)
{
   const vector<TableIssue> fresh = table->CheckTable(TableCheckPane::LiveOptions);
   vector<const TableIssue *> confirmed;
   for (const TableIssue *const issue : issues)
      if (std::ranges::any_of(fresh, [issue](const TableIssue &f)
             { return f.fix == issue->fix && f.part == issue->part && f.image == issue->image && f.collection == issue->collection && f.index == issue->index; }))
         confirmed.push_back(issue);
   return confirmed;
}

void TableCheckFixOp::Fix(EditorUI &editor, const vector<const TableIssue *> &issues)
{
   if (editor.IsInspectMode() || editor.m_table->IsLocked())
      return;
   vector<const TableIssue *> fixes;
   for (const TableIssue *const issue : Confirmed(editor.m_table, issues))
      if (IsApplicable(editor.m_table, *issue))
         fixes.push_back(issue);
   if (fixes.empty())
      return; // No empty undo step
   const string label = fixes.size() == 1 ? string(Label(fixes.front()->fix)) : string(Label(fixes.front()->fix)) + " (" + std::to_string(fixes.size()) + ')';
   editor.m_undo.BeginUndo();
   const vector<IEditable *> changed = Apply(editor.m_table, editor.m_undo, fixes);
   editor.m_undo.SetLabel(label);
   editor.m_undo.EndUndo();
   for (IEditable *const part : changed)
   {
      editor.m_renderer->ReinitRenderable(part->GetIRenderable());
      editor.m_player->m_physics->Update(part);
   }
   // A deleted image or collection must not stay selected
   const auto &images = editor.m_table->GetImageList();
   std::erase_if(editor.m_multiSelImages, [&images](const Texture *image) { return std::ranges::find(images, image) == images.end(); });
   if (editor.m_outlinerImageAnchor != nullptr && std::ranges::find(images, editor.m_outlinerImageAnchor) == images.end())
      editor.m_outlinerImageAnchor = nullptr;
   if ((editor.m_selection.GetType() == Selection::S_IMAGE && std::ranges::find(images, editor.m_selection.GetImage()) == images.end())
      || (editor.m_selection.GetType() == Selection::S_COLLECTION && std::ranges::find(editor.m_table->GetCollections(), editor.m_selection.GetCollection()) == editor.m_table->GetCollections().end()))
      editor.SetSelection(Selection());
   editor.Report(label);
}

}
