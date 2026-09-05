#include "EditorHistory.h"

#include <cstddef>

namespace HEngine
{
    void EditorHistoryStack::Clear()
    {
        mEntries.clear();
        mCursor = 0;
        ClearPendingSnapshot();
    }

    void EditorHistoryStack::Reset(const std::string& snapshot, const std::string& label)
    {
        mEntries.clear();
        mEntries.push_back({ snapshot, label });
        mCursor = 0;
        ClearPendingSnapshot();
    }

    void EditorHistoryStack::CaptureSnapshot(const std::string& snapshot, const std::string& label, bool bInteractive)
    {
        if (!IsInitialized())
        {
            Reset(snapshot, label);
            return;
        }

        const std::string& currentSnapshot = mEntries[mCursor].Snapshot;
        if (snapshot == currentSnapshot)
        {
            if (!bInteractive)
                ClearPendingSnapshot();
            return;
        }

        if (bInteractive)
        {
            mHasPendingInteractiveSnapshot = true;
            mPendingSnapshot = snapshot;
            mPendingLabel = label;
            return;
        }

        if (mHasPendingInteractiveSnapshot)
        {
            if (mPendingSnapshot != currentSnapshot)
                PushSnapshot(mPendingSnapshot, mPendingLabel.empty() ? label : mPendingLabel);

            ClearPendingSnapshot();
            if (snapshot == mEntries[mCursor].Snapshot)
                return;
        }

        PushSnapshot(snapshot, label);
    }

    void EditorHistoryStack::CommitPendingInteractiveSnapshot()
    {
        if (!IsInitialized() || !mHasPendingInteractiveSnapshot)
            return;

        const std::string& currentSnapshot = mEntries[mCursor].Snapshot;
        if (mPendingSnapshot != currentSnapshot)
            PushSnapshot(mPendingSnapshot, mPendingLabel);

        ClearPendingSnapshot();
    }

    bool EditorHistoryStack::CanUndo() const
    {
        return mEntries.size() > 1 && mCursor > 0;
    }

    bool EditorHistoryStack::CanRedo() const
    {
        return !mEntries.empty() && (mCursor + 1) < mEntries.size();
    }

    bool EditorHistoryStack::IsInitialized() const
    {
        return !mEntries.empty();
    }

    const char* EditorHistoryStack::GetUndoLabel() const
    {
        return CanUndo() ? mEntries[mCursor].Label.c_str() : nullptr;
    }

    const char* EditorHistoryStack::GetRedoLabel() const
    {
        return CanRedo() ? mEntries[mCursor + 1].Label.c_str() : nullptr;
    }

    bool EditorHistoryStack::Undo(const std::function<bool(const std::string&)>& applySnapshot)
    {
        if (!CanUndo())
            return false;

        const size_t targetCursor = mCursor - 1;
        if (!applySnapshot || !applySnapshot(mEntries[targetCursor].Snapshot))
            return false;

        mCursor = targetCursor;
        ClearPendingSnapshot();
        return true;
    }

    bool EditorHistoryStack::Redo(const std::function<bool(const std::string&)>& applySnapshot)
    {
        if (!CanRedo())
            return false;

        const size_t targetCursor = mCursor + 1;
        if (!applySnapshot || !applySnapshot(mEntries[targetCursor].Snapshot))
            return false;

        mCursor = targetCursor;
        ClearPendingSnapshot();
        return true;
    }

    void EditorHistoryStack::PushSnapshot(const std::string& snapshot, const std::string& label)
    {
        if ((mCursor + 1) < mEntries.size())
            mEntries.erase(mEntries.begin() + static_cast<ptrdiff_t>(mCursor + 1), mEntries.end());

        mEntries.push_back({ snapshot, label });
        if (mEntries.size() > mMaxEntries)
            mEntries.erase(mEntries.begin());

        mCursor = mEntries.empty() ? 0 : (mEntries.size() - 1);
    }

    void EditorHistoryStack::ClearPendingSnapshot()
    {
        mHasPendingInteractiveSnapshot = false;
        mPendingSnapshot.clear();
        mPendingLabel.clear();
    }
}
