// license:GPLv3+

#include "core/stdafx.h"

#include "pinundo.h"

#include "vpversion.h"
#include "parts/ball.h"
#include "parts/PartGroup.h"
#include "parts/pintable.h"
#include "physics/PhysicsEngine.h"
#include "ui/live/LiveUI.h"
#include "utils/fileio.h"
#include "utils/BiffReader.h"
#include "utils/BiffWriter.h"

#define MAXUNDO 16 // Kept records, plus the one being recorded


class UndoRecord final
{
public:
   UndoRecord();
   ~UndoRecord();

   void MarkForUndo(IEditable *const pie, const bool saveForUndo);
   void MarkForCreate(IEditable *const pie);
   void MarkForDelete(IEditable *const pie);

   vector<std::function<void()>> m_actions;
   vector<std::unique_ptr<InMemStream>> m_vstm;
   vector<IEditable *> m_vieCreate;
   vector<IEditable *> m_vieDelete;
   vector<IEditable *> m_vieMark;
   bool m_ownsCreateRefs = false; // The record holds the creation reference of its created parts (revert of an undelete), released if it is dropped without being applied
   std::any m_editorState; // Editor provided state (e.g. selection) captured when the record was begun

   // Description shown by the undo history
   string m_label; // Complete label set by the editor (e.g. "Move 3 parts")
   string m_field; // Edited field/operation, composed with the subject part name (e.g. "PBR_DOME_f125: Rim Width")
   string m_subject; // Name of the first part involved in the record
   size_t m_subjectCount = 0; // Number of parts involved in the record
   void NoteSubject(IEditable *const pie);
   string Label() const;
};


UndoRecord::UndoRecord() { }

UndoRecord::~UndoRecord()
{
   for (IEditable *editable : m_vieDelete)
      editable->Release();
   if (m_ownsCreateRefs)
      for (IEditable *editable : m_vieCreate)
         editable->Release();
}

void UndoRecord::NoteSubject(IEditable *const pie)
{
   m_subjectCount++;
   if (m_subject.empty())
      if (const IScriptable *const scriptable = pie->GetIScriptable())
         m_subject = scriptable->m_name;
}

string UndoRecord::Label() const
{
   if (!m_label.empty())
      return m_label;
   if (!m_field.empty())
   {
      if (m_subjectCount > 1)
         return m_field + " (" + std::to_string(m_subjectCount) + " parts)";
      return m_subject.empty() ? m_field : m_subject + ": " + m_field;
   }
   if (m_subjectCount > 1)
      return std::to_string(m_subjectCount) + " parts";
   if (m_subjectCount == 1 && !m_subject.empty())
      return m_subject;
   return "Edit";
}

void UndoRecord::MarkForUndo(IEditable *const pie, const bool saveForUndo)
{
   if (FindIndexOf(m_vieMark, pie) != -1) // Been marked already
      return;

   if (FindIndexOf(m_vieCreate, pie) != -1) // Just created, so undo will delete it anyway
      return;

   m_vieMark.push_back(pie);
   NoteSubject(pie);

   auto pstm = std::make_unique<InMemStream>();

   pstm->Write(&pie, sizeof(IEditable *));

   BiffWriter writer(pstm.get(), nullptr);
   pie->Save(writer, saveForUndo);

   m_vstm.push_back(std::move(pstm));
}

void UndoRecord::MarkForCreate(IEditable *const pie)
{
   assert(FindIndexOf(m_vieCreate, pie) == -1); // Created twice?
   m_vieCreate.push_back(pie);
   NoteSubject(pie);
}

void UndoRecord::MarkForDelete(IEditable *const pie)
{
   assert(FindIndexOf(m_vieDelete, pie) == -1); // Already deleted - bad thing

   const int pie_pos = FindIndexOf(m_vieCreate, pie);
   if (pie_pos != -1) // Created and deleted in the same step
   {
      // Just forget about it
      m_vieCreate.erase(m_vieCreate.begin() + pie_pos);
      RemoveFromVectorSingle(m_vieMark, pie);
      return;
   }

   m_vieDelete.push_back(pie);
   pie->AddRef();
   NoteSubject(pie);
}



PinUndo::PinUndo(PinTable *table)
   : m_table(table)
{
}

PinUndo::~PinUndo() { }

bool PinUndo::HasUndo() const { return !m_undoRecords.empty(); }

bool PinUndo::HasRedo() const { return !m_redoRecords.empty(); }

size_t PinUndo::UndoCount() const { return m_undoRecords.size(); }

size_t PinUndo::RedoCount() const { return m_redoRecords.size(); }

string PinUndo::UndoLabel(const size_t index) const { return index < m_undoRecords.size() ? m_undoRecords[m_undoRecords.size() - 1 - index]->Label() : string(); }

string PinUndo::RedoLabel(const size_t index) const { return index < m_redoRecords.size() ? m_redoRecords[m_redoRecords.size() - 1 - index]->Label() : string(); }

void PinUndo::SetLabel(const string &label)
{
   if (!m_undoRecords.empty())
      m_undoRecords.back()->m_label = label;
}

void PinUndo::SetFieldLabel(const string &field)
{
   if (!m_undoRecords.empty())
      m_undoRecords.back()->m_field = field;
}

void PinUndo::ClearRedo()
{
   m_redoRecords.clear();
   m_redoStash.clear();
   if (m_cleanpoint != NoCleanPoint && m_cleanpoint > m_undoRecords.size()) // The clean state was in the dropped redo records
      m_cleanpoint = NoCleanPoint;
}

void PinUndo::UpdateDirtyAfterMove()
{
   if ((m_undoRecords.size() == m_cleanpoint) && (m_dirtyState > SaveDirtyState::Clean))
   {
      m_dirtyState = SaveDirtyState::Clean;
      m_table->SetDirty(SaveDirtyState::Clean);
   }
   else if ((m_undoRecords.size() != m_cleanpoint) && (m_dirtyState < SaveDirtyState::Dirty))
   {
      // If we're not clean, we must be dirty (always process this case for autosave, even though to the user it wouldn't appear to matter)
      m_dirtyState = SaveDirtyState::Dirty;
      m_table->SetDirty(SaveDirtyState::Dirty);
   }
}

bool PinUndo::IsUndoPastCleanPoint() const { return !m_undoRecords.empty() && m_undoRecords.size() == m_cleanpoint; }

void PinUndo::SetCleanPoint(const SaveDirtyState sds)
{
   if (sds == SaveDirtyState::Clean)
      m_cleanpoint = m_undoRecords.size();
   m_dirtyState = sds;
   m_table->SetDirty(sds);
}

void PinUndo::BeginUndo()
{
   m_nUndoLayer++;
   if (m_nUndoLayer == 1)
   {
      // Only drop the oldest record once the history exceeds its size, so that a record that ends up being discarded (see Discard) does not cost a real one
      if (m_undoRecords.size() > MAXUNDO)
      {
         m_undoRecords.erase(m_undoRecords.begin());
         m_cleanpoint = (m_cleanpoint == 0 || m_cleanpoint == NoCleanPoint) ? NoCleanPoint : m_cleanpoint - 1;
      }
      // A new edit invalidates the redo records. They are only stashed until the record is ended: if it ends up discarded (see Discard, the editor
      // begins and discards a record every frame while the mouse is over a property pane) they are restored
      m_redoStash.clear();
      m_cleanpointAtBegin = m_cleanpoint;
      if (!m_redoRecords.empty())
      {
         m_redoStash = std::move(m_redoRecords);
         m_redoRecords.clear();
         if (m_cleanpoint != NoCleanPoint && m_cleanpoint > m_undoRecords.size()) // The clean state was in the redo records: no longer reachable
            m_cleanpoint = NoCleanPoint;
      }
      m_dirtyStateAtBegin = m_dirtyState;
      m_undoRecords.push_back(std::make_unique<UndoRecord>());
      if (m_editorStateCapture)
         m_undoRecords.back()->m_editorState = m_editorStateCapture();
   }
}

void PinUndo::MarkForUndo(IEditable *const pie, const bool saveForUndo)
{
   assert(!m_undoRecords.empty());
   assert(m_nUndoLayer > 0);
   if (m_undoRecords.empty())
      return;
   if (m_nUndoLayer <= 0)
      BeginUndo();

   m_undoRecords.back()->MarkForUndo(pie, saveForUndo);
}

void PinUndo::MarkForCreate(IEditable *const pie)
{
   assert(!m_undoRecords.empty());
   assert(m_nUndoLayer > 0);
   if (m_undoRecords.empty())
      return;
   if (m_nUndoLayer <= 0)
      BeginUndo();

   m_undoRecords.back()->MarkForCreate(pie);
}

void PinUndo::MarkForDelete(IEditable *const pie)
{
   assert(!m_undoRecords.empty());
   assert(m_nUndoLayer > 0);
   if (m_undoRecords.empty())
      return;
   if (m_nUndoLayer <= 0)
      BeginUndo();

   m_undoRecords.back()->MarkForDelete(pie);
}

void PinUndo::MarkForAction(std::function<void()> undo)
{
   assert(!m_undoRecords.empty());
   assert(m_nUndoLayer > 0);
   if (m_undoRecords.empty())
      return;
   if (m_nUndoLayer <= 0)
      BeginUndo();

   m_undoRecords.back()->m_actions.push_back(std::move(undo));
}

std::any PinUndo::Undo(const bool keepForRedo)
{
   assert(m_nUndoLayer == 0);
   while (m_nUndoLayer > 0)
      EndUndo();

   if (!HasUndo())
      return {};

   std::unique_ptr<UndoRecord> record = std::move(m_undoRecords.back());
   m_undoRecords.pop_back();

   std::unique_ptr<UndoRecord> inverse;
   std::any editorState = ApplyRecord(*record, keepForRedo, inverse);
   if (inverse)
   {
      m_redoStash.clear();
      m_redoRecords.push_back(std::move(inverse));
   }
   else if (!keepForRedo && !m_redoStash.empty())
   {
      // Cancels the record that has just been begun (e.g. a canceled modal transform): the redo records it stashed are back
      m_redoRecords = std::move(m_redoStash);
      m_redoStash.clear();
      m_cleanpoint = m_cleanpointAtBegin;
   }
   else
      ClearRedo(); // Not redoable (non part change): the redo records may depend on the state before it
   record.reset();

   UpdateDirtyAfterMove();
   return editorState;
}

std::any PinUndo::Redo()
{
   assert(m_nUndoLayer == 0);
   while (m_nUndoLayer > 0)
      EndUndo();

   if (!HasRedo())
      return {};

   m_redoStash.clear();
   std::unique_ptr<UndoRecord> record = std::move(m_redoRecords.back());
   m_redoRecords.pop_back();

   std::unique_ptr<UndoRecord> inverse;
   std::any editorState = ApplyRecord(*record, true, inverse);
   assert(inverse); // Redo records never hold actions, they are always invertible
   if (inverse)
      m_undoRecords.push_back(std::move(inverse));
   record.reset();

   UpdateDirtyAfterMove();
   return editorState;
}

// Applies a record (restores the state it was started from) and builds the record that reverts this application (the redo record when undoing, the undo record when redoing)
std::any PinUndo::ApplyRecord(UndoRecord &record, const bool buildInverse, std::unique_ptr<UndoRecord> &inverseOut)
{
   // Non part changes (actions) can not be reverted
   const bool invertible = buildInverse && record.m_actions.empty();
   std::unique_ptr<UndoRecord> inverse;
   if (invertible)
   {
      inverse = std::make_unique<UndoRecord>();
      inverse->m_ownsCreateRefs = true;
      if (m_editorStateCapture)
         inverse->m_editorState = m_editorStateCapture(); // Editor state before the revert, given back when the inverse is applied
      inverse->m_label = record.m_label;
      inverse->m_field = record.m_field;
      inverse->m_subject = record.m_subject;
      inverse->m_subjectCount = record.m_subjectCount;
   }

   // Non part changes first, most recent first: a part restored below may refer to an asset they bring back
   for (auto it = record.m_actions.rbegin(); it != record.m_actions.rend(); ++it)
      (*it)();

   for (IEditable *editable : record.m_vieDelete)
   {
      m_table->Undelete(editable); // Adds the table reference; the record's one is released with the record below
      if (inverse)
      {
         // Reverting this undelete is a creation undo: Uncreate drops two references (the creation one and the table one)
         editable->AddRef();
         inverse->m_vieCreate.push_back(editable);
      }
      if (g_pplayer && (g_pplayer->m_ptable == m_table))
      {
         // Deleted through the LiveUI: restore the player side resources it released (same setup as part creation). The render release is
         // deferred to the end of the frame and runs on the render thread: when it is still pending, the render setup is deferred to the
         // next frame preparation, which runs after the frame (and its deferred commands) has been fully processed
         g_pplayer->TimerSetup(editable);
         if (IRenderable *const renderable = editable->GetIRenderable())
         {
            if (editable->m_renderReleasePending)
               g_pplayer->m_renderer->SetupRenderable(renderable);
            else
               renderable->RenderSetup(g_pplayer->m_renderer.get());
         }
         if (editable->GetIHitable())
            g_pplayer->m_physics->Add(editable);
      }
   }

   for (const auto &pstm : record.m_vstm)
   {
      IEditable *const pie = *reinterpret_cast<IEditable *const *>(pstm->Data());
      if (inverse && FindIndexOf(inverse->m_vieCreate, pie) == -1) // Parts that are deleted again by the inverse need no snapshot
      {
         // Snapshot of the part before it is overwritten: restored when the inverse is applied
         auto inverseStm = std::make_unique<InMemStream>();
         inverseStm->Write(&pie, sizeof(IEditable *));
         BiffWriter inverseWriter(inverseStm.get(), nullptr);
         pie->Save(inverseWriter, true);
         inverse->m_vieMark.push_back(pie);
         inverse->m_vstm.push_back(std::move(inverseStm));
      }
      IScriptable *const scriptable = pie->GetIScriptable();
      const string nameBefore = scriptable ? scriptable->m_name : string();
      pie->ClearForOverwrite();

      // Process the loaded PartGroup parenting to support undoing reparenting
      pie->m_onLoadExpectedPartGroup.clear();
      BiffReader reader(pstm->Data() + sizeof(IEditable *), static_cast<uint32_t>(pstm->Size() - sizeof(IEditable *)), CURRENT_FILE_FORMAT_VERSION, nullptr, 0);
      pie->Load(reader);

      // Load writes the name directly: apply a restored name through the table to keep its name registry (and code view) in sync
      if (scriptable && scriptable->m_name != nameBefore && m_table->HasRegisteredName(pie))
      {
         const string restoredName = scriptable->m_name;
         scriptable->m_name = nameBefore;
         if (lowerCase(restoredName) == lowerCase(nameBefore) || m_table->IsNameUnique(restoredName))
            m_table->RenamePart(pie, restoredName);
         else
            PLOGW << "Undo could not restore the name '" << restoredName << "' of '" << nameBefore << "' as it is now used by another part";
      }
      // The record holds the name of the part's group (empty when it had none, which is only valid for part groups)
      if (!pie->m_onLoadExpectedPartGroup.empty())
      {
         const string groupName = pie->m_onLoadExpectedPartGroup;
         for (IEditable *const edit : m_table->GetParts())
            if (edit->GetItemType() == eItemPartGroup && StrCompareNoCase(edit->GetIScriptable()->m_name, groupName))
            {
               pie->SetPartGroup(static_cast<PartGroup *>(edit));
               break;
            }
      }
      else if (pie->GetItemType() == eItemPartGroup)
         pie->SetPartGroup(nullptr);
      if (g_pplayer)
      {
         if (pie->GetIRenderable())
            g_pplayer->m_renderer->ReinitRenderable(pie->GetIRenderable());
         if (pie->GetIHitable())
            g_pplayer->m_physics->Update(pie);
      }
   }

   record.m_ownsCreateRefs = false; // Uncreate drops the creation references
   for (size_t i = 0; i < record.m_vieCreate.size(); i++)
   {
      IEditable *const pie = record.m_vieCreate[i];
      if (inverse)
      {
         // Reverting this creation undo is a delete: the inverse keeps the part alive, to put it back in the table (Undelete) when applied
         inverse->m_vieDelete.push_back(pie);
         pie->AddRef();
      }
      if (g_pplayer && (g_pplayer->m_ptable == m_table))
      {
         // The part was created through the LiveUI and owns player side resources: release them before removing it
         if (pie->GetItemType() == eItemBall)
            RemoveFromVectorSingle(g_pplayer->m_vball, static_cast<Ball *>(pie));
         if (pie->GetIHitable())
            g_pplayer->m_physics->Remove(pie);
         if (pie->m_phittimer)
            g_pplayer->TimerRelease(pie);
         g_pplayer->m_renderer->DropRenderable(pie->GetIRenderable());
         pie->AddRef(); // Keep the part alive until its deferred render release has been processed
         pie->m_renderReleasePending = true;
         g_pplayer->m_renderer->m_renderDevice->AddEndOfFrameCmd(
            [pie]()
            {
               if (pie->GetIRenderable())
                  pie->GetIRenderable()->RenderRelease();
               pie->m_renderReleasePending = false;
               pie->Release();
            });
      }
      m_table->Uncreate(pie);
   }

   inverseOut = std::move(inverse);
   return std::move(record.m_editorState);
}

void PinUndo::Discard()
{
   assert(m_nUndoLayer == 0);
   while (m_nUndoLayer > 0)
      EndUndo();

   if (!HasUndo())
      return;

   m_undoRecords.pop_back();

   // The redo records stashed when the record was begun are back
   if (!m_redoStash.empty())
   {
      m_redoRecords = std::move(m_redoStash);
      m_redoStash.clear();
      m_cleanpoint = m_cleanpointAtBegin;
   }

   // A discarded record leaves no trace: restore the dirty state it changed when ended
   if (m_dirtyState != m_dirtyStateAtBegin)
   {
      m_dirtyState = m_dirtyStateAtBegin;
      m_table->SetDirty(m_dirtyState);
   }
}


void PinUndo::EndUndo()
{
   assert(m_nUndoLayer > 0);
   if (m_nUndoLayer > 0)
      m_nUndoLayer--;
   if (m_nUndoLayer == 0 && (m_dirtyState < SaveDirtyState::Dirty))
   {
      m_dirtyState = SaveDirtyState::Dirty;
      m_table->SetDirty(SaveDirtyState::Dirty);
   }
}

