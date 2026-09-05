#include "Hepch.h"

#include "Runtime/Platform/Embree/EditorEmbreeContext.h"

#if defined(WITH_EDITOR)

#include <embree4/rtcore.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace HEngine
{
	namespace
	{
		struct SharedEditorEmbreeState
		{
			~SharedEditorEmbreeState()
			{
				if (Scene)
					rtcReleaseScene(Scene);
				if (Device)
					rtcReleaseDevice(Device);
			}

			std::mutex Mutex;
			RTCDevice Device = nullptr;
			RTCScene Scene = nullptr;
			std::uint64_t Generation = 0;
			bool bSceneDirty = false;
		};

		SharedEditorEmbreeState& GetSharedState()
		{
			static SharedEditorEmbreeState state;
			return state;
		}

		std::uint64_t NextGeneration(std::uint64_t generation) noexcept
		{
			++generation;
			return generation == 0 ? 1 : generation;
		}

		RTCError ConsumeDeviceError(RTCDevice device) noexcept
		{
			return device ? rtcGetDeviceError(device) : RTC_ERROR_UNKNOWN;
		}

		bool ConsumeDeviceSuccess(RTCDevice device) noexcept
		{
			return ConsumeDeviceError(device) == RTC_ERROR_NONE;
		}

		void ThrowIfDeviceFailed(RTCDevice device, const char* operation)
		{
			if (!device)
			{
				(void)rtcGetDeviceError(nullptr);
				throw std::runtime_error(operation);
			}

			if (!ConsumeDeviceSuccess(device))
			{
				rtcReleaseDevice(device);
				throw std::runtime_error(operation);
			}
		}

		void ThrowOnDeviceError(RTCDevice device, const char* operation)
		{
			if (!ConsumeDeviceSuccess(device))
				throw std::runtime_error(operation);
		}
	}

	struct EditorEmbreeInstance::Impl
	{
		RTCScene GeometryScene = nullptr;
		RTCGeometry InstanceGeometry = nullptr;
		std::uint32_t InstanceID = RTC_INVALID_GEOMETRY_ID;
		std::uint64_t ContextGeneration = 0;
	};

	EditorEmbreeInstance::EditorEmbreeInstance(Scope<Impl> impl)
		: mImpl(std::move(impl))
	{
	}

	EditorEmbreeInstance::~EditorEmbreeInstance()
	{
		if (!mImpl)
			return;

		SharedEditorEmbreeState& state = GetSharedState();
		{
			std::scoped_lock lock(state.Mutex);
			if (state.Scene && state.Generation == mImpl->ContextGeneration &&
				mImpl->InstanceID != RTC_INVALID_GEOMETRY_ID)
			{
				rtcDetachGeometry(state.Scene, mImpl->InstanceID);
				(void)ConsumeDeviceSuccess(state.Device);
				state.bSceneDirty = true;
			}

			if (mImpl->InstanceGeometry)
				rtcReleaseGeometry(mImpl->InstanceGeometry);
			if (mImpl->GeometryScene)
				rtcReleaseScene(mImpl->GeometryScene);
			(void)ConsumeDeviceError(state.Device);
		}
	}

	std::uint32_t EditorEmbreeInstance::GetInstanceID() const noexcept
	{
		return mImpl ? mImpl->InstanceID : RTC_INVALID_GEOMETRY_ID;
	}

	std::uint64_t EditorEmbreeInstance::GetContextGeneration() const noexcept
	{
		return mImpl ? mImpl->ContextGeneration : 0;
	}

	void EditorEmbreeContext::Initialize()
	{
		SharedEditorEmbreeState& state = GetSharedState();
		std::scoped_lock lock(state.Mutex);
		if (state.Device && state.Scene)
			return;
		if (state.Device || state.Scene)
			throw std::logic_error("The editor Embree context is partially initialized.");

		RTCDevice device = rtcNewDevice(nullptr);
		ThrowIfDeviceFailed(device, "Failed to create the editor Embree device.");

		RTCScene scene = rtcNewScene(device);
		const RTCError sceneError = ConsumeDeviceError(device);
		if (!scene || sceneError != RTC_ERROR_NONE)
		{
			if (scene)
				rtcReleaseScene(scene);
			(void)ConsumeDeviceError(device);
			rtcReleaseDevice(device);
			throw std::runtime_error("Failed to create the editor Embree scene.");
		}

		state.Device = device;
		state.Scene = scene;
		state.Generation = NextGeneration(state.Generation);
		state.bSceneDirty = true;
	}

	void EditorEmbreeContext::Reset()
	{
		SharedEditorEmbreeState& state = GetSharedState();
		std::scoped_lock lock(state.Mutex);
		if (!state.Device || !state.Scene)
			throw std::logic_error("Cannot reset an uninitialized editor Embree context.");

		RTCScene replacement = rtcNewScene(state.Device);
		const RTCError replacementError = ConsumeDeviceError(state.Device);
		if (!replacement || replacementError != RTC_ERROR_NONE)
		{
			if (replacement)
				rtcReleaseScene(replacement);
			(void)ConsumeDeviceError(state.Device);
			throw std::runtime_error("Failed to reset the editor Embree scene.");
		}

		RTCScene previous = std::exchange(state.Scene, replacement);
		state.Generation = NextGeneration(state.Generation);
		state.bSceneDirty = true;
		rtcReleaseScene(previous);
		(void)ConsumeDeviceError(state.Device);
	}

	void EditorEmbreeContext::Shutdown() noexcept
	{
		SharedEditorEmbreeState& state = GetSharedState();
		std::scoped_lock lock(state.Mutex);

		RTCScene scene = std::exchange(state.Scene, nullptr);
		RTCDevice device = std::exchange(state.Device, nullptr);
		state.Generation = NextGeneration(state.Generation);
		state.bSceneDirty = false;

		if (scene)
			rtcReleaseScene(scene);
		(void)ConsumeDeviceError(device);
		if (device)
			rtcReleaseDevice(device);
	}

	bool EditorEmbreeContext::IsInitialized() noexcept
	{
		SharedEditorEmbreeState& state = GetSharedState();
		std::scoped_lock lock(state.Mutex);
		return state.Device != nullptr && state.Scene != nullptr;
	}

	std::uint64_t EditorEmbreeContext::GetGeneration() noexcept
	{
		SharedEditorEmbreeState& state = GetSharedState();
		std::scoped_lock lock(state.Mutex);
		return state.Generation;
	}

	Scope<EditorEmbreeInstance> EditorEmbreeContext::CreateInstance(
		std::span<const glm::vec3> vertices,
		std::span<const std::uint32_t> triangleIndices,
		const glm::mat4& transform)
	{
		if (vertices.empty() || triangleIndices.empty() || triangleIndices.size() % 3 != 0)
			throw std::invalid_argument("Editor Embree geometry requires vertices and complete triangles.");

		SharedEditorEmbreeState& state = GetSharedState();
		std::scoped_lock lock(state.Mutex);
		if (!state.Device || !state.Scene)
			throw std::logic_error("The editor Embree context is not initialized.");

		RTCScene geometryScene = rtcNewScene(state.Device);
		const RTCError geometrySceneError = ConsumeDeviceError(state.Device);
		RTCGeometry triangleGeometry = nullptr;
		RTCGeometry instanceGeometry = nullptr;
		std::uint32_t attachedInstanceID = RTC_INVALID_GEOMETRY_ID;
		try
		{
			if (!geometryScene || geometrySceneError != RTC_ERROR_NONE)
				throw std::runtime_error("Failed to create an editor Embree geometry scene.");

			triangleGeometry = rtcNewGeometry(state.Device, RTC_GEOMETRY_TYPE_TRIANGLE);
			const RTCError triangleGeometryError = ConsumeDeviceError(state.Device);
			if (!triangleGeometry || triangleGeometryError != RTC_ERROR_NONE)
				throw std::runtime_error("Failed to create editor Embree triangle geometry.");

			void* vertexDestination = rtcSetNewGeometryBuffer(
				triangleGeometry,
				RTC_BUFFER_TYPE_VERTEX,
				0,
				RTC_FORMAT_FLOAT3,
				sizeof(glm::vec3),
				vertices.size());
			const RTCError vertexBufferError = ConsumeDeviceError(state.Device);
			if (!vertexDestination || vertexBufferError != RTC_ERROR_NONE)
				throw std::runtime_error("Failed to allocate the editor Embree vertex buffer.");

			void* indexDestination = rtcSetNewGeometryBuffer(
				triangleGeometry,
				RTC_BUFFER_TYPE_INDEX,
				0,
				RTC_FORMAT_UINT3,
				3 * sizeof(std::uint32_t),
				triangleIndices.size() / 3);
			const RTCError indexBufferError = ConsumeDeviceError(state.Device);
			if (!indexDestination || indexBufferError != RTC_ERROR_NONE)
				throw std::runtime_error("Failed to allocate the editor Embree index buffer.");

			std::memcpy(vertexDestination, vertices.data(), vertices.size_bytes());
			std::memcpy(indexDestination, triangleIndices.data(), triangleIndices.size_bytes());

			rtcCommitGeometry(triangleGeometry);
			ThrowOnDeviceError(state.Device, "Failed to commit editor Embree triangle geometry.");
			const std::uint32_t triangleGeometryID = rtcAttachGeometry(geometryScene, triangleGeometry);
			const RTCError triangleAttachError = ConsumeDeviceError(state.Device);
			if (triangleGeometryID == RTC_INVALID_GEOMETRY_ID || triangleAttachError != RTC_ERROR_NONE)
				throw std::runtime_error("Failed to attach editor Embree triangle geometry.");
			rtcReleaseGeometry(std::exchange(triangleGeometry, nullptr));
			(void)ConsumeDeviceError(state.Device);
			rtcCommitScene(geometryScene);
			ThrowOnDeviceError(state.Device, "Failed to commit the editor Embree geometry scene.");

			instanceGeometry = rtcNewGeometry(state.Device, RTC_GEOMETRY_TYPE_INSTANCE);
			const RTCError instanceGeometryError = ConsumeDeviceError(state.Device);
			if (!instanceGeometry || instanceGeometryError != RTC_ERROR_NONE)
				throw std::runtime_error("Failed to create editor Embree instance geometry.");

			rtcSetGeometryInstancedScene(instanceGeometry, geometryScene);
			ThrowOnDeviceError(state.Device, "Failed to assign the editor Embree instanced scene.");
			rtcSetGeometryTimeStepCount(instanceGeometry, 1);
			ThrowOnDeviceError(state.Device, "Failed to configure editor Embree instance time steps.");
			rtcSetGeometryTransform(
				instanceGeometry,
				0,
				RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
				glm::value_ptr(transform));
			ThrowOnDeviceError(state.Device, "Failed to set the editor Embree instance transform.");
			rtcCommitGeometry(instanceGeometry);
			ThrowOnDeviceError(state.Device, "Failed to commit editor Embree instance geometry.");

			attachedInstanceID = rtcAttachGeometry(state.Scene, instanceGeometry);
			const RTCError instanceAttachError = ConsumeDeviceError(state.Device);
			if (attachedInstanceID == RTC_INVALID_GEOMETRY_ID || instanceAttachError != RTC_ERROR_NONE)
				throw std::runtime_error("Failed to attach editor Embree instance geometry.");

			auto impl = CreateScope<EditorEmbreeInstance::Impl>();
			impl->GeometryScene = geometryScene;
			impl->InstanceGeometry = instanceGeometry;
			impl->InstanceID = attachedInstanceID;
			impl->ContextGeneration = state.Generation;
			state.bSceneDirty = true;

			return Scope<EditorEmbreeInstance>(new EditorEmbreeInstance(std::move(impl)));
		}
		catch (...)
		{
			if (attachedInstanceID != RTC_INVALID_GEOMETRY_ID)
			{
				rtcDetachGeometry(state.Scene, attachedInstanceID);
				(void)ConsumeDeviceSuccess(state.Device);
				state.bSceneDirty = true;
			}
			if (instanceGeometry)
				rtcReleaseGeometry(instanceGeometry);
			if (triangleGeometry)
				rtcReleaseGeometry(triangleGeometry);
			if (geometryScene)
				rtcReleaseScene(geometryScene);
			(void)ConsumeDeviceError(state.Device);
			throw;
		}
	}

	bool EditorEmbreeContext::UpdateInstanceTransform(
		EditorEmbreeInstance& instance,
		const glm::mat4& transform)
	{
		if (!instance.mImpl || !instance.mImpl->InstanceGeometry)
			return false;

		SharedEditorEmbreeState& state = GetSharedState();
		std::scoped_lock lock(state.Mutex);
		if (!state.Scene || state.Generation != instance.mImpl->ContextGeneration)
			return false;

		rtcSetGeometryTransform(
			instance.mImpl->InstanceGeometry,
			0,
			RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
			glm::value_ptr(transform));
		if (!ConsumeDeviceSuccess(state.Device))
			return false;
		rtcCommitGeometry(instance.mImpl->InstanceGeometry);
		if (!ConsumeDeviceSuccess(state.Device))
			return false;
		state.bSceneDirty = true;
		return true;
	}

	bool EditorEmbreeContext::Commit() noexcept
	{
		try
		{
			SharedEditorEmbreeState& state = GetSharedState();
			std::scoped_lock lock(state.Mutex);
			if (!state.Scene || !state.bSceneDirty)
				return state.Scene != nullptr;

			rtcCommitScene(state.Scene);
			if (!ConsumeDeviceSuccess(state.Device))
				return false;
			state.bSceneDirty = false;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	std::optional<EditorEmbreeRayHit> EditorEmbreeContext::Raycast(
		const glm::vec3& origin,
		const glm::vec3& direction,
		float nearDistance,
		float farDistance)
	{
		const auto bFiniteVector = [](const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		};
		const float clampedNearDistance = std::max(0.0f, nearDistance);
		if (!bFiniteVector(origin) || !bFiniteVector(direction) ||
			glm::dot(direction, direction) <= std::numeric_limits<float>::epsilon() ||
			std::isnan(nearDistance) || std::isnan(farDistance) ||
			farDistance <= clampedNearDistance)
		{
			return std::nullopt;
		}

		SharedEditorEmbreeState& state = GetSharedState();
		std::scoped_lock lock(state.Mutex);
		if (!state.Scene)
			return std::nullopt;
		if (state.bSceneDirty)
		{
			rtcCommitScene(state.Scene);
			if (!ConsumeDeviceSuccess(state.Device))
				return std::nullopt;
			state.bSceneDirty = false;
		}

		RTCRayHit rayHit{};
		rayHit.ray.org_x = origin.x;
		rayHit.ray.org_y = origin.y;
		rayHit.ray.org_z = origin.z;
		rayHit.ray.dir_x = direction.x;
		rayHit.ray.dir_y = direction.y;
		rayHit.ray.dir_z = direction.z;
		rayHit.ray.tnear = clampedNearDistance;
		rayHit.ray.tfar = farDistance;
		rayHit.ray.mask = std::numeric_limits<std::uint32_t>::max();
		rayHit.ray.flags = 0;
		rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
		for (std::uint32_t& instanceID : rayHit.hit.instID)
			instanceID = RTC_INVALID_GEOMETRY_ID;

		rtcIntersect1(state.Scene, &rayHit);
		if (!ConsumeDeviceSuccess(state.Device))
			return std::nullopt;
		if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID ||
			rayHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID)
		{
			return std::nullopt;
		}

		return EditorEmbreeRayHit{ rayHit.hit.instID[0], rayHit.hit.primID };
	}
}

#endif
