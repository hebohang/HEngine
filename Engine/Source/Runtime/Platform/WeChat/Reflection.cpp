#include "Runtime/Core/Meta/Serializer/Serializer.h"
#include "_generated/serializer/TransformComponent.gen.hpp"
#include "_generated/serializer/SpriteRendererComponent.gen.hpp"
#include "_generated/serializer/TagComponent.gen.hpp"
#include "_generated/serializer/NativeScriptComponent.gen.hpp"
#include "_generated/serializer/TouchSample.gen.hpp"
#include "_generated/reflection/TransformComponent.reflection.gen.h"
#include "_generated/reflection/SpriteRendererComponent.reflection.gen.h"
#include "_generated/reflection/TagComponent.reflection.gen.h"
#include "_generated/reflection/NativeScriptComponent.reflection.gen.h"
#include "_generated/reflection/TouchSample.reflection.gen.h"
#if defined(HE_SCRIPT_CSHARP_WASM)
#include "_generated/serializer/Camera.gen.hpp"
#include "_generated/serializer/ScriptComponent.gen.hpp"
#include "_generated/serializer/SceneCamera.gen.hpp"
#include "_generated/serializer/CameraComponent.gen.hpp"
#include "_generated/reflection/ScriptComponent.reflection.gen.h"
#include "_generated/reflection/SceneCamera.reflection.gen.h"
#include "_generated/reflection/CameraComponent.reflection.gen.h"
#include "_generated/reflection/Camera.reflection.gen.h"
#endif

namespace HEngine
{
    void RegisterWeChatReflection()
    {
        static bool registered = false;
        if (registered) return;
        Reflection::TypeWrapperRegister_TransformComponent();
        Reflection::TypeWrapperRegister_SpriteRendererComponent();
        Reflection::TypeWrapperRegister_TagComponent();
        Reflection::TypeWrapperRegister_NativeScriptComponent();
        Reflection::TypeWrapperRegister_TouchSampleState();
#if defined(HE_SCRIPT_CSHARP_WASM)
        Reflection::TypeWrapperRegister_Camera();
        Reflection::TypeWrapperRegister_ScriptComponent();
        Reflection::TypeWrapperRegister_SceneCamera();
        Reflection::TypeWrapperRegister_CameraComponent();
#endif
        registered = true;
    }
}
