#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace HEngine
{
    class AnimatorComponent;
    class Mesh;
    struct CharacterDeformationTick
    {
        bool Active = false;
        float DeltaSeconds = 0.0f;
    };
    // Pause freezes simulation; it is distinct from stop/distance opt-out.
    // Evaluate before TickEntity consumes the explicit paused single-step time.
    CharacterDeformationTick ResolveCharacterDeformationTick(const AnimatorComponent& animator,
        float frameDeltaSeconds, bool inRange);
    // Called once after final bone-pose constraints, also while paused so motion
    // vectors never reuse stale CPU deformation velocities.
    // Returns vertices actually published this tick, including one history-settle
    // upload. Cached unchanged sections contribute zero (not their active size).
    uint64_t UpdateCharacterDeformation(AnimatorComponent& animator, Mesh& mesh,
        const glm::mat4& world, float deltaSeconds, bool active);
    // Authoring preview: manual/default morphs only, no playback clock or simulation.
    // Restores any cloth deformation and publishes settled history once per edit.
    uint64_t PreviewCharacterDeformation(AnimatorComponent& animator, Mesh& mesh);
}
