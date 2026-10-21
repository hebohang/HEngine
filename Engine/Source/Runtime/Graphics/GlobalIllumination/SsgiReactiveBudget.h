#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace HEngine
{
    struct SsgiReactiveRegion
    {
        glm::uvec4 Pixels{ 0u }; // min x/y inclusive, max x/y exclusive, trace pixels
        uint32_t RemainingFrames = 0;
        uint32_t Reasons = 0; // 1 geometry, 2 reset, 4 unknown, 8 overflow, 16 projection
        bool bEnabled = false;
    };

    struct SsgiReactiveObject
    {
        int EntityId = -1;
        uint64_t Revision = 0;
        glm::vec3 Minimum{ 0.f }, Maximum{ 0.f };
        bool bTrustedBounds = false;
        bool bGlobalOnChange = false; // shadows/emissive transport not localized by geometry radius
        bool bContinuousDeformation = false; // explicit animation path, independent of bounds trust
    };

    // CPU-only prototype: union all affected tiles into one conservative rect.
    // World-space windows survive camera motion; Prepare never ages committed
    // history. Commit advances exactly one successfully submitted GI frame.
    class SsgiReactiveBudget
    {
    public:
        static constexpr uint32_t WindowFrames = 16;
        static constexpr size_t MaxObjects = 16384;
        static constexpr size_t MaxChangedObjects = 256;

        SsgiReactiveRegion Prepare(std::span<const SsgiReactiveObject> objects,
            uint64_t sceneGeneration, uint64_t geometryFingerprint,
            const glm::mat4& viewProjection, uint32_t width, uint32_t height,
            float maximumRayDistance, uint32_t maxBounces, float thickness, bool bReset)
        {
            mPendingObjects.clear();
            mPendingWindows = mWindows;
            mPendingScene = sceneGeneration;
            mPendingFingerprint = geometryFingerprint;
            mPrepared = true;
            Window changed;
            changed.Remaining = WindowFrames;
            if (!mInitialized || sceneGeneration != mScene || bReset) changed.Reasons |= 2u;
            if (objects.size() > MaxObjects) changed.Reasons |= 8u;
            size_t changes = 0;
            const auto include = [&](const SsgiReactiveObject& object) {
                ++changes;
                changed.Reasons |= 1u;
                if (object.bGlobalOnChange) changed.Reasons |= 4u;
                if (!object.bTrustedBounds || !Finite(object.Minimum) || !Finite(object.Maximum) ||
                    glm::any(glm::greaterThan(object.Minimum, object.Maximum)))
                {
                    changed.Reasons |= 4u;
                    return;
                }
                if (!changed.bBounds) { changed.Minimum = object.Minimum; changed.Maximum = object.Maximum; }
                else { changed.Minimum = glm::min(changed.Minimum, object.Minimum); changed.Maximum = glm::max(changed.Maximum, object.Maximum); }
                changed.bBounds = true;
            };
            if (objects.size() <= MaxObjects)
            {
                for (const auto& object : objects)
                {
                    if (!mPendingObjects.emplace(object.EntityId, object).second) changed.Reasons |= 4u;
                    // Bounds trust determines changed-object scope, not whether a
                    // static object changes. Only explicit dynamic paths renew.
                    if (object.bContinuousDeformation) changed.Reasons |= 4u;
                    const auto old = mObjects.find(object.EntityId);
                    if (old == mObjects.end()) include(object);
                    else if (old->second.Revision != object.Revision ||
                        old->second.bTrustedBounds != object.bTrustedBounds ||
                        old->second.bGlobalOnChange != object.bGlobalOnChange ||
                        old->second.bContinuousDeformation != object.bContinuousDeformation ||
                        old->second.Minimum != object.Minimum || old->second.Maximum != object.Maximum)
                    { include(old->second); include(object); }
                }
                for (const auto& [id, object] : mObjects)
                    if (!mPendingObjects.contains(id)) include(object);
            }
            if (changes > MaxChangedObjects) changed.Reasons |= 8u;
            if (mInitialized && geometryFingerprint != mFingerprint && changes == 0) changed.Reasons |= 4u;
            if (changed.Reasons)
            {
                const float radius = (maximumRayDistance + std::max(thickness, 0.f) * 1.1f + 1e-4f) *
                    float(std::max(maxBounces, 1u));
                if (!std::isfinite(radius) || radius <= 0.f) changed.Reasons |= 4u;
                else if (changed.bBounds) { changed.Minimum -= radius; changed.Maximum += radius; }
                mPendingWindows.push_back(changed);
            }
            SsgiReactiveRegion result;
            result.bEnabled = true;
            if (!width || !height) { result.Reasons = 16u; result.RemainingFrames = WindowFrames; return result; }
            glm::vec2 uvMin(1.f), uvMax(0.f);
            bool bAny = false;
            for (const auto& window : mPendingWindows)
            {
                result.Reasons |= window.Reasons;
                result.RemainingFrames = std::max(result.RemainingFrames, window.Remaining);
            }
            for (const auto& window : mPendingWindows)
            {
                if ((window.Reasons & ~1u) != 0u)
                { result.Pixels = {0u, 0u, width, height}; return result; }
                for (uint32_t corner = 0; corner < 8; ++corner)
                {
                    const glm::vec3 point{
                        corner & 1u ? window.Maximum.x : window.Minimum.x,
                        corner & 2u ? window.Maximum.y : window.Minimum.y,
                        corner & 4u ? window.Maximum.z : window.Minimum.z };
                    const glm::vec4 clip = viewProjection * glm::vec4(point, 1.f);
                    // Near-plane intersections deliberately fall back, rather
                    // than projecting only surviving corners and underestimating.
                    if (!Finite(glm::vec3(clip)) || !std::isfinite(clip.w) || clip.w <= 1e-6f || clip.z < 0.f)
                    { result.Pixels = {0u, 0u, width, height}; result.Reasons |= 16u; return result; }
                    const glm::vec2 uv = glm::vec2(clip) / clip.w * glm::vec2(.5f, -.5f) + .5f;
                    if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
                    { result.Pixels = {0u, 0u, width, height}; result.Reasons |= 16u; return result; }
                    uvMin = glm::min(uvMin, uv); uvMax = glm::max(uvMax, uv); bAny = true;
                }
            }
            if (!bAny || uvMax.x < 0.f || uvMax.y < 0.f || uvMin.x > 1.f || uvMin.y > 1.f) return result;
            uvMin = glm::clamp(uvMin, glm::vec2(0.f), glm::vec2(1.f));
            uvMax = glm::clamp(uvMax, glm::vec2(0.f), glm::vec2(1.f));
            const auto lower = [](float uv, uint32_t size) { const uint32_t p = uint32_t(std::floor(uv * size)); return p > 8u ? ((p - 8u) / 8u) * 8u : 0u; };
            const auto upper = [](float uv, uint32_t size) { return std::min(size, ((uint32_t(std::ceil(uv * size)) + 15u) / 8u) * 8u); };
            result.Pixels = { lower(uvMin.x, width), lower(uvMin.y, height), upper(uvMax.x, width), upper(uvMax.y, height) };
            return result;
        }

        void Commit() noexcept
        {
            if (!mPrepared) return;
            for (auto& window : mPendingWindows) if (window.Remaining) --window.Remaining;
            std::erase_if(mPendingWindows, [](const Window& window) { return window.Remaining == 0; });
            mObjects.swap(mPendingObjects); mWindows.swap(mPendingWindows);
            mScene = mPendingScene; mFingerprint = mPendingFingerprint;
            mInitialized = true; mPrepared = false;
        }
        void Abort() noexcept { mPrepared = false; }

    private:
        static bool Finite(const glm::vec3& value) noexcept
        { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
        struct Window
        {
            glm::vec3 Minimum{0.f}, Maximum{0.f};
            uint32_t Remaining = 0, Reasons = 0;
            bool bBounds = false;
        };
        std::unordered_map<int, SsgiReactiveObject> mObjects, mPendingObjects;
        std::vector<Window> mWindows, mPendingWindows;
        uint64_t mScene = 0, mFingerprint = 0, mPendingScene = 0, mPendingFingerprint = 0;
        bool mInitialized = false, mPrepared = false;
    };
}
