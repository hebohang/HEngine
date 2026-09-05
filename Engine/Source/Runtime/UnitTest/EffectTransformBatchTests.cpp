#include "Hepch.h"

#include "Runtime/UnitTest/EffectTransformBatchTests.h"

#include "Runtime/Graphics/VFX/EffectTransformBatch.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace HEngine::Tests
{
    namespace
    {
        struct RecordingDispatcher
        {
            std::vector<EffectTransformCommand> Commands;
        };

        void Require(bool condition, std::string_view message)
        {
            if (!condition)
                throw std::runtime_error(std::string(message));
        }

        bool EqualFloatBits(float left, float right)
        {
            return std::bit_cast<uint32_t>(left) ==
                std::bit_cast<uint32_t>(right);
        }

        bool EqualCommand(
            const EffectTransformCommand& left,
            const EffectTransformCommand& right)
        {
            return left.Handle == right.Handle &&
                EqualFloatBits(left.PositionX, right.PositionX) &&
                EqualFloatBits(left.PositionY, right.PositionY) &&
                EqualFloatBits(left.PositionZ, right.PositionZ) &&
                EqualFloatBits(left.ForwardX, right.ForwardX) &&
                EqualFloatBits(left.ForwardY, right.ForwardY) &&
                EqualFloatBits(left.ForwardZ, right.ForwardZ);
        }

        bool IsValidHandle(int32_t handle)
        {
            if (handle == 7)
                return true;
            if (handle < 1000 || handle >= 1064)
                return false;
            return (handle - 1000) % 3 != 1;
        }

        bool RecordAndDispatch(
            void* context,
            const EffectTransformCommand& command)
        {
            auto& recording = *static_cast<RecordingDispatcher*>(context);
            recording.Commands.push_back(command);
            return IsValidHandle(command.Handle);
        }

        std::vector<EffectTransformCommand> MakeMaximumWorkload()
        {
            std::vector<EffectTransformCommand> commands;
            commands.reserve(67);
            for (int32_t index = 0; index < 64; ++index)
            {
                EffectTransformCommand command{
                    .Handle = 1000 + index,
                    .PositionX = static_cast<float>(index) * 0.25f,
                    .PositionY = -static_cast<float>(index) * 0.125f,
                    .PositionZ = 8.0f - static_cast<float>(index),
                    .ForwardX = 1.0f + static_cast<float>(index),
                    .ForwardY = -0.5f,
                    .ForwardZ = -2.0f
                };
                if (index == 6)
                {
                    command.ForwardX = 0.0f;
                    command.ForwardY = -0.0f;
                    command.ForwardZ = 0.0f;
                }
                commands.push_back(command);
            }

            commands.push_back({
                .Handle = -1,
                .PositionX = -10.0f,
                .PositionY = 2.0f,
                .PositionZ = 4.0f,
                .ForwardX = 0.0f,
                .ForwardY = 0.0f,
                .ForwardZ = -1.0f
            });
            commands.push_back({
                .Handle = 9000,
                .PositionX = 3.0f,
                .PositionY = 4.0f,
                .PositionZ = 5.0f,
                .ForwardX = -1.0f,
                .ForwardY = 0.25f,
                .ForwardZ = 2.0f
            });
            commands.push_back({
                .Handle = 1005,
                .PositionX = 99.0f,
                .PositionY = 98.0f,
                .PositionZ = 97.0f,
                .ForwardX = -3.0f,
                .ForwardY = -2.0f,
                .ForwardZ = -1.0f
            });
            return commands;
        }
    }

    void EffectTransformBatchContract()
    {
        RecordingDispatcher recording;
        std::vector<uint8_t> results = { 0xA5, 0xA5, 0xA5 };

        DispatchEffectTransformBatch(
            std::span<const EffectTransformCommand>(),
            &recording,
            &RecordAndDispatch,
            results);
        Require(results.empty(), "An empty effect transform batch retained stale results.");
        Require(recording.Commands.empty(), "An empty effect transform batch invoked the dispatcher.");

        const EffectTransformCommand single{
            .Handle = 7,
            .PositionX = 1.0f,
            .PositionY = 2.0f,
            .PositionZ = 3.0f,
            .ForwardX = 0.25f,
            .ForwardY = -0.0f,
            .ForwardZ = -1.0f
        };
        DispatchEffectTransformBatch(
            std::span<const EffectTransformCommand>(&single, 1),
            &recording,
            &RecordAndDispatch,
            results);
        Require(results.size() == 1 && results.front() == uint8_t{ 1 },
            "A valid single-command batch returned the wrong result.");
        Require(recording.Commands.size() == 1 &&
            EqualCommand(recording.Commands.front(), single),
            "A single-command batch changed the dispatched command.");

        const std::vector<EffectTransformCommand> commands = MakeMaximumWorkload();
        Require(commands.size() == 67, "The effect transform workload fixture changed size.");
        recording.Commands.clear();
        results.assign(80, uint8_t{ 0xA5 });
        DispatchEffectTransformBatch(
            commands,
            &recording,
            &RecordAndDispatch,
            results);

        Require(results.size() == commands.size(),
            "The batch did not overwrite the scratch result size.");
        Require(recording.Commands.size() == commands.size(),
            "The batch skipped or duplicated a dispatcher invocation.");
        for (std::size_t index = 0; index < commands.size(); ++index)
        {
            Require(EqualCommand(recording.Commands[index], commands[index]),
                "The batch changed dispatcher input order or command data.");
            Require(results[index] == static_cast<uint8_t>(
                IsValidHandle(commands[index].Handle) ? 1 : 0),
                "The batch changed dispatcher result order or value.");
        }
        Require(EqualFloatBits(recording.Commands[6].ForwardX, 0.0f) &&
            EqualFloatBits(recording.Commands[6].ForwardY, -0.0f) &&
            EqualFloatBits(recording.Commands[6].ForwardZ, 0.0f),
            "The batch normalized or changed a zero forward vector.");
        Require(recording.Commands[5].Handle == 1005 &&
            recording.Commands.back().Handle == 1005 &&
            !EqualFloatBits(
                recording.Commands[5].PositionX,
                recording.Commands.back().PositionX),
            "The batch collapsed duplicate handles or changed their order.");

        const std::size_t callCountBeforeNullDispatcher = recording.Commands.size();
        results.assign(4, uint8_t{ 0xA5 });
        DispatchEffectTransformBatch(commands, &recording, nullptr, results);
        Require(results.size() == commands.size() &&
            std::all_of(results.begin(), results.end(), [](uint8_t result)
            {
                return result == uint8_t{ 0 };
            }),
            "A null dispatcher did not return one false result per command.");
        Require(recording.Commands.size() == callCountBeforeNullDispatcher,
            "A null dispatcher unexpectedly invoked a prior callback.");

        DispatchEffectTransformBatch(
            std::span<const EffectTransformCommand>(),
            nullptr,
            nullptr,
            results);
        Require(results.empty(),
            "An empty null-dispatch batch did not clear scratch results.");
    }
}
