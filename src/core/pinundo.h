// license:GPLv3+
#pragma once

#include <any>
#include <functional>

class IEditable;
class PinTable;
class UndoRecord;

class PinUndo final
{
public:
   PinUndo(PinTable* table);
   ~PinUndo();

   void BeginUndo();
   void MarkForUndo(IEditable * const pie, const bool saveForUndo = true);
   void MarkForCreate(IEditable *const pie);
   void MarkForDelete(IEditable *const pie);
   // Records a change that is not a part edit (asset, script, table property): the callback restores the previous state when the record is undone
   void MarkForAction(std::function<void()> undo);
   void EndUndo();
   // Undoes the last record, returning the editor state captured when it was begun (empty if none). The undone record can be redone
   // (until a new record is begun) unless keepForRedo is false, which is used to cancel the record that has just been begun
   std::any Undo(bool keepForRedo = true);
   // Redoes the last undone record, returning the editor state captured when it was undone (empty if none)
   std::any Redo();
   void Discard();

   // History labels for the undo history (index 0: the next record to undo / redo, then going away from it)
   void SetLabel(const string &label); // Complete label of the last record (being recorded, or just ended) (e.g. "Move 3 parts")
   void SetFieldLabel(const string &field); // Edited field, shown after the name of the part(s) of the record (e.g. "PBR_DOME_f125: Rim Width")
   size_t UndoCount() const;
   size_t RedoCount() const;
   string UndoLabel(size_t index) const;
   string RedoLabel(size_t index) const;

   bool HasUndo() const;
   bool HasRedo() const;
   bool IsUndoPastCleanPoint() const;
   void SetCleanPoint(const SaveDirtyState sds);

   // Registers the editor state capture callback: it is evaluated each time a new undo record is begun,
   // the returned state is stored in the record and given back by Undo() (e.g. to restore the selection)
   void SetEditorStateCapture(const std::function<std::any()> &capture) { m_editorStateCapture = capture; }

private:
   static constexpr size_t NoCleanPoint = static_cast<size_t>(-1);
   std::any ApplyRecord(UndoRecord &record, bool buildInverse, std::unique_ptr<UndoRecord> &inverse);
   void ClearRedo();
   void UpdateDirtyAfterMove();

   PinTable *const m_table;
   vector<std::unique_ptr<UndoRecord>> m_undoRecords;
   vector<std::unique_ptr<UndoRecord>> m_redoRecords; // Undone records (the last one is the next to redo)
   vector<std::unique_ptr<UndoRecord>> m_redoStash; // Redo records set aside while a new record is being recorded: restored if it is discarded
   size_t m_cleanpointAtBegin = 0;
   int m_nUndoLayer = 0;
   SaveDirtyState m_dirtyState = SaveDirtyState::Clean;
   SaveDirtyState m_dirtyStateAtBegin = SaveDirtyState::Clean; // Dirty state when the last record was begun, restored if it is discarded
   size_t m_cleanpoint = 0; // Undo record at which table is in a non-dirty state (if bigger than undo records size, clean state can not be reached)
   std::function<std::any()> m_editorStateCapture;
};
