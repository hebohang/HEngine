#pragma once

#include "Runtime/Core/Meta/Reflection/Reflection.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace HEngine
{
    REFLECTION_TYPE(FacialKey)
    STRUCT(FacialKey, Fields)
    {
        REFLECTION_BODY(FacialKey);
        float Time = 0.0f;
        float Value = 0.0f;
    };

    REFLECTION_TYPE(FacialCurve)
    STRUCT(FacialCurve, Fields)
    {
        REFLECTION_BODY(FacialCurve);
        std::string Target;
        std::vector<FacialKey> Keys;
        bool Smooth = true;
    };

    // Curves are absolute named morph weights. The same file can drive any
    // compatible character; no mesh indices or hero-specific names are stored.
    REFLECTION_TYPE(FacialPerformanceRes)
    STRUCT(FacialPerformanceRes, Fields)
    {
        REFLECTION_BODY(FacialPerformanceRes);
        static std::string GetExtName() { return "hface"; }
        float Duration = 1.0f;
        bool Loop = false;
        std::vector<FacialCurve> Curves;
    };

    class FacialPerformance
    {
    public:
        static void Validate(const FacialPerformanceRes& performance)
        {
            if (!std::isfinite(performance.Duration) || performance.Duration <= 0.0f)
                throw std::invalid_argument("Facial performance requires a finite positive duration");
            for (const auto& curve : performance.Curves)
            {
                if (curve.Target.empty() || curve.Keys.empty())
                    throw std::invalid_argument("Facial curve requires a target and keys");
                float previous = -1.0f;
                for (const auto& key : curve.Keys)
                {
                    if (!std::isfinite(key.Time) || !std::isfinite(key.Value) || key.Time < 0.0f ||
                        key.Time <= previous || key.Time > performance.Duration || std::abs(key.Value) > 2.0f)
                        throw std::invalid_argument("Invalid facial key time/value or ordering");
                    previous = key.Time;
                }
            }
        }

        static float Sample(const FacialCurve& curve, float time)
        {
            if (curve.Keys.empty() || !std::isfinite(time)) return 0.0f;
            if (time <= curve.Keys.front().Time) return curve.Keys.front().Value;
            if (time >= curve.Keys.back().Time) return curve.Keys.back().Value;
            const auto right = std::upper_bound(curve.Keys.begin(), curve.Keys.end(), time,
                [](float value, const FacialKey& key) { return value < key.Time; });
            const auto& left = *(right - 1);
            float t = (time - left.Time) / (right->Time - left.Time);
            if (curve.Smooth) t = t * t * (3.0f - 2.0f * t);
            return std::lerp(left.Value, right->Value, t);
        }

        static void Evaluate(const FacialPerformanceRes& performance, float time,
            std::map<std::string, float>& weights)
        {
            if (!std::isfinite(time) || !std::isfinite(performance.Duration) || performance.Duration <= 0.0f) return;
            time = std::max(time, 0.0f);
            if (performance.Loop) time = std::fmod(time, performance.Duration);
            else time = std::min(time, performance.Duration);
            for (const auto& curve : performance.Curves) weights[curve.Target] = Sample(curve, time);
        }

        // An asymmetric close/open envelope, deterministic under seek and replay.
        // Disabled with interval <= 0. Author the eyelid shape in the asset.
        static float Blink(float time, float interval, float duration = 0.18f)
        {
            if (!std::isfinite(time) || !std::isfinite(interval) || !std::isfinite(duration) ||
                time < 0.0f || interval <= 0.0f || duration <= 0.0f) return 0.0f;
            duration = std::min(duration, interval);
            const float phase = std::fmod(time, interval);
            if (phase > duration) return 0.0f;
            const float close = duration * 0.35f;
            const float linear = phase <= close ? phase / close : (duration - phase) / (duration - close);
            return linear * linear * (3.0f - 2.0f * linear);
        }
    };
    class FacialPlayer
    {
    public:
        void Reset();
        void Seek(float seconds);
        [[nodiscard]] float GetTime() const { return mTime; }
        [[nodiscard]] const std::string& GetError() const { return mError; }
        // Explicit weights override authored curves; procedural blink takes max.
        const std::map<std::string, float>& Update(float deltaSeconds,
            const std::string& resourcePath, const FacialPerformanceRes& inlineData,
            const std::map<std::string, float>& weights,
            const std::string& blinkLeft = {}, const std::string& blinkRight = {},
            float blinkInterval = 0.0f);
    private:
        float mTime = 0.0f;
        std::string mLoadedPath;
        std::string mError;
        FacialPerformanceRes mLoadedData;
        std::map<std::string, float> mWeights;
    };

}
