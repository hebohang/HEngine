#pragma once

#include "Runtime/Animation/Animator.h"
#include "Runtime/Resource/ResType/AnimController.h"
#include <limits>

namespace HEngine
{
    // Blend samples are phase-synchronized pose sources. The state's clip owns
    // duration and events, so changing weights never duplicates a footstep.
    inline void ApplyBaseMotionBlend(Animator& animator, int nodeId, const AnimControllerRes& controller,
        const std::map<int, int>& nodeToClip, std::vector<Animation>& clips,
        const std::map<std::string, Json>& parameters, bool loop, bool rootMotion = false, bool lockXZ = true)
    {
        const auto node = std::find_if(controller.nodes.begin(), controller.nodes.end(),
            [nodeId](const auto& item) { return item.id == nodeId; });
        if (node == controller.nodes.end() || node->blendParameter.empty()) return;
        const auto value = parameters.find(node->blendParameter);
        if (value == parameters.end() || !value->second.is_number())
            throw std::invalid_argument("Animation blend parameter must name a numeric controller parameter.");
        const float coordinate = value->second.get<float>();
        if (!std::isfinite(coordinate) || node->blendSamples.size() < 2)
            throw std::invalid_argument("Animation motion blend requires a finite coordinate and at least two samples.");
        const auto& samples = node->blendSamples;
        if (samples.size() > 32) throw std::invalid_argument("Animation blends support at most 32 samples.");
        if (!node->blendParameterY.empty())
        {
            const auto yValue = parameters.find(node->blendParameterY);
            if (yValue == parameters.end() || !yValue->second.is_number() || !std::isfinite(yValue->second.get<float>()))
                throw std::invalid_argument("Second animation blend parameter must be finite and numeric.");
            const double y = yValue->second.get<double>();
            std::vector<double> distances;
            double minimum = std::numeric_limits<double>::max();
            for (size_t i = 0; i < samples.size(); ++i)
            {
                const auto& sample = samples[i];
                if (!std::isfinite(sample.threshold) || !std::isfinite(sample.thresholdY) || !nodeToClip.contains(sample.nodeId))
                    throw std::invalid_argument("2D blend needs finite coordinates and loaded State node IDs.");
                for (size_t j = 0; j < i; ++j)
                    if (sample.threshold == samples[j].threshold && sample.thresholdY == samples[j].thresholdY)
                        throw std::invalid_argument("2D blend sample positions must be unique.");
                const double dx = double(coordinate) - sample.threshold, dy = y - sample.thresholdY;
                const double distance = dx * dx + dy * dy;
                distances.push_back(distance); minimum = std::min(minimum, distance);
            }
            std::vector<Animator> poses;
            std::vector<float> weights;
            for (size_t i = 0; i < samples.size(); ++i)
            {
                auto& clip = clips.at(nodeToClip.at(samples[i].nodeId));
                poses.emplace_back(&clip);
                auto& pose = poses.back();
                pose.SetLoop(loop); pose.SetRootMotionOptions(rootMotion, lockXZ);
                pose.UpdateAnimationAbsolute(animator.GetProgress() * clip.GetDuration());
                // Dividing by the minimum keeps finite, bounded weights even near a sample.
                weights.push_back(minimum == 0 ? (distances[i] == 0 ? 1.0f : 0.0f) : static_cast<float>(minimum / distances[i]));
            }
            animator.OverrideWeightedPose(poses, weights);
            return;
        }
        for (size_t index = 0; index < samples.size(); ++index)
            if (!std::isfinite(samples[index].threshold) ||
                (index && samples[index].threshold <= samples[index-1].threshold) ||
                !nodeToClip.contains(samples[index].nodeId))
                throw std::invalid_argument("Animation blend samples need increasing thresholds and loaded State node IDs.");
        size_t upper = 1;
        while (upper + 1 < samples.size() && coordinate > samples[upper].threshold) ++upper;
        const auto& low = samples[upper - 1];
        const auto& high = samples[upper];
        // Finite float endpoints can still overflow a float subtraction.
        const double fraction = (double(coordinate) - double(low.threshold)) /
            (double(high.threshold) - double(low.threshold));
        const float alpha = static_cast<float>(std::clamp(fraction, 0.0, 1.0));
        Animation& lowClip = clips.at(nodeToClip.at(low.nodeId));
        Animation& highClip = clips.at(nodeToClip.at(high.nodeId));
        Animator from(&lowClip), to(&highClip);
        from.SetLoop(loop); to.SetLoop(loop);
        from.SetRootMotionOptions(rootMotion, lockXZ); to.SetRootMotionOptions(rootMotion, lockXZ);
        from.UpdateAnimationAbsolute(animator.GetProgress() * lowClip.GetDuration());
        to.UpdateAnimationAbsolute(animator.GetProgress() * highClip.GetDuration());
        animator.OverrideBlendedPose(from, to, alpha);
    }

    inline void ApplyMotionBlend(Animator& animator, int nodeId, const AnimControllerRes& controller,
        const std::map<int, int>& nodeToClip, std::vector<Animation>& clips,
        const std::map<std::string, Json>& parameters, bool loop, bool rootMotion = false, bool lockXZ = true)
    {
        const auto node = std::find_if(controller.nodes.begin(), controller.nodes.end(),
            [nodeId](const auto& item) { return item.id == nodeId; });
        if (node == controller.nodes.end()) return;
        if (node->blendParameter.empty() && !node->blendParameterY.empty())
            throw std::invalid_argument("A 2D animation blend requires both X and Y parameters.");
        if (node->blendParameter.empty() && node->layers.empty()) return;
        animator.ResamplePose(); // Avoid accumulating additive layers on paused/zero-delta frames.
        ApplyBaseMotionBlend(animator, nodeId, controller, nodeToClip, clips, parameters, loop, rootMotion, lockXZ);
        if (node->layers.size() > 16) throw std::invalid_argument("Animation states support at most 16 layers.");
        for (const auto& layer : node->layers)
        {
            float weight = layer.weight;
            if (!layer.weightParameter.empty())
            {
                const auto value = parameters.find(layer.weightParameter);
                if (value == parameters.end() || !value->second.is_number())
                    throw std::invalid_argument("Animation layer weight parameter must be numeric.");
                weight *= value->second.get<float>();
            }
            if (!std::isfinite(weight) || weight < 0 || weight > 1 || layer.maskRoots.size() > 4096 || !nodeToClip.contains(layer.nodeId))
                throw std::invalid_argument("Invalid animation layer weight, mask size or State node ID.");
            if (weight == 0.0f) continue;
            float progress = animator.GetProgress();
            if (!layer.progressParameter.empty())
            {
                const auto value = parameters.find(layer.progressParameter);
                if (value == parameters.end() || !value->second.is_number())
                    throw std::invalid_argument("Animation layer progress parameter must be numeric.");
                progress = value->second.get<float>();
                if (!std::isfinite(progress) || progress < 0.0f || progress > 1.0f)
                    throw std::invalid_argument("Animation layer progress must be in [0,1].");
            }
            auto& clip = clips.at(nodeToClip.at(layer.nodeId));
            Animator pose(&clip);
            pose.SetLoop(layer.progressParameter.empty() && loop);
            pose.SetRootMotionOptions(rootMotion, lockXZ);
            pose.UpdateAnimationAbsolute(progress * clip.GetDuration());
            animator.ApplyLayer(pose, weight, layer.maskRoots, layer.additive);
        }
    }
}
