#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace HEngine
{
    // Owned by each view's mesh cache, never by the shared CPU simulation asset.
    // Bone matrices are tracked separately by SubmittedMotionHistory.
    class SubmittedDeformationHistory
    {
    public:
        [[nodiscard]] bool NeedsUpload(uint64_t revision, uint64_t resetGeneration) const noexcept
        {
            return !bSubmitted || revision != mRevision || resetGeneration != mResetGeneration || bNeedsSettling;
        }

        template<class Vertex>
        void Stage(std::span<const Vertex> vertices, std::vector<Vertex>& upload,
            uint64_t revision, uint64_t resetGeneration, bool reset)
        {
            const bool useHistory = bSubmitted && !reset &&
                mResetGeneration == resetGeneration && mSubmitted.size() == vertices.size();
            upload.assign(vertices.begin(), vertices.end());
            mPending.resize(vertices.size());
            bPendingMotion = false;
            for (size_t index = 0; index < vertices.size(); ++index)
            {
                auto& vertex = upload[index];
                vertex.PreviousPosition = useHistory ? mSubmitted[index] : vertex.Pos;
                mPending[index] = vertex.Pos;
                bPendingMotion |= vertex.PreviousPosition != vertex.Pos;
            }
            mPendingRevision = revision;
            mPendingResetGeneration = resetGeneration;
            bPending = true;
        }

        void Commit() noexcept
        {
            if (!bPending) return;
            mSubmitted.swap(mPending);
            mRevision = mPendingRevision;
            mResetGeneration = mPendingResetGeneration;
            bNeedsSettling = bPendingMotion;
            bSubmitted = true;
            bPending = false;
        }
        // An aborted command list must not become the next frame's history.
        void Abort() noexcept { bPending = false; }

    private:
        std::vector<glm::vec3> mSubmitted, mPending;
        uint64_t mRevision = 0, mPendingRevision = 0;
        uint64_t mResetGeneration = 0, mPendingResetGeneration = 0;
        bool bSubmitted = false, bPending = false;
        bool bNeedsSettling = false, bPendingMotion = false;
    };
}
