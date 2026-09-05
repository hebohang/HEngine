#pragma once

#include <functional>
#include <string>
#include <vector>

namespace HEngine
{
    class EditorHistoryStack
    {
    public:
        struct Entry
        {
            std::string Snapshot;
            std::string Label;
        };

        void Clear();
        void Reset(const std::string& snapshot, const std::string& label = "Initial State");
        void CaptureSnapshot(const std::string& snapshot, const std::string& label, bool bInteractive);
        void CommitPendingInteractiveSnapshot();

        [[nodiscard]] bool CanUndo() const;
        [[nodiscard]] bool CanRedo() const;
        [[nodiscard]] bool IsInitialized() const;
        [[nodiscard]] const char* GetUndoLabel() const;
        [[nodiscard]] const char* GetRedoLabel() const;

        bool Undo(const std::function<bool(const std::string&)>& applySnapshot);
        bool Redo(const std::function<bool(const std::string&)>& applySnapshot);

    private:
        void PushSnapshot(const std::string& snapshot, const std::string& label);
        void ClearPendingSnapshot();

    private:
        std::vector<Entry> mEntries;
        size_t mCursor = 0;
        bool mHasPendingInteractiveSnapshot = false;
        std::string mPendingSnapshot;
        std::string mPendingLabel;
        size_t mMaxEntries = 256;
    };

    class IEditorUndoClient
    {
    public:
        virtual ~IEditorUndoClient() = default;

        [[nodiscard]] virtual bool CanUndo() const = 0;
        [[nodiscard]] virtual bool CanRedo() const = 0;
        virtual bool Undo() = 0;
        virtual bool Redo() = 0;
        [[nodiscard]] virtual const char* GetUndoContextName() const = 0;
        [[nodiscard]] virtual bool IsUndoContextDirty() const { return false; }
    };
}
