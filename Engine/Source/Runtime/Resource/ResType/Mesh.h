#pragma once
#include "Runtime/Core/Meta/Reflection/Reflection.h"
#include "Texture.h"
#include <glm/glm.hpp>
#include <map>

#define HENGINE_COMPONENT_TYPE_BYTE (5120)
#define HENGINE_COMPONENT_TYPE_UNSIGNED_BYTE (5121)
#define HENGINE_COMPONENT_TYPE_SHORT (5122)
#define HENGINE_COMPONENT_TYPE_UNSIGNED_SHORT (5123)
#define HENGINE_COMPONENT_TYPE_INT (5124)
#define HENGINE_COMPONENT_TYPE_UNSIGNED_INT (5125)
#define HENGINE_COMPONENT_TYPE_FLOAT (5126)
#define HENGINE_COMPONENT_TYPE_DOUBLE \
  (5130)  // OpenGL double type. Note that some of glTF 2.0 validator does not
// support double type even the schema seems allow any value of
// integer:
// https://github.com/KhronosGroup/glTF/blob/b9884a2fd45130b4d673dd6c8a706ee21ee5c5f7/specification/2.0/schema/accessor.schema.json#L22

namespace HEngine
{
    namespace Utils
    {
        static inline int32_t GetComponentSizeInBytes(uint32_t componentType)
        {
            if (componentType == HENGINE_COMPONENT_TYPE_BYTE) 
            {
                return 1;
            }
            else if (componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_BYTE) 
            {
                return 1;
            }
            else if (componentType == HENGINE_COMPONENT_TYPE_SHORT) 
            {
                return 2;
            }
            else if (componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_SHORT) 
            {
                return 2;
            }
            else if (componentType == HENGINE_COMPONENT_TYPE_INT) 
            {
                return 4;
            }
            else if (componentType == HENGINE_COMPONENT_TYPE_UNSIGNED_INT) 
            {
                return 4;
            }
            else if (componentType == HENGINE_COMPONENT_TYPE_FLOAT) 
            {
                return 4;
            }
            else if (componentType == HENGINE_COMPONENT_TYPE_DOUBLE) 
            {
                return 8;
            }
            else 
            {
                HE_CORE_ASSERT(false, "Unknown component type");
                return -1;
            }
        }

        static inline int32_t GetNumComponentsInType(const std::string& type) 
        {
            if (type.compare("SCALAR") == 0) 
            {
                return 1;
            }
            else if (type.compare("VEC2") == 0) 
            {
                return 2;
            }
            else if (type.compare("VEC3") == 0) 
            {
                return 3;
            }
            else if (type.compare("VEC4") == 0) 
            {
                return 4;
            }
            else if (type.compare("MAT2") == 0) 
            {
                return 4;
            }
            else if (type.compare("MAT3") == 0) 
            {
                return 9;
            }
            else if (type.compare("MAT4") == 0) 
            {
                return 16;
            }
            else 
            {
                HE_CORE_ASSERT(false, "Unknown type");
                return -1;
            }
        }
    }

    REFLECTION_TYPE(MeshBuffer)
    CLASS(MeshBuffer, Fields)
    {
        REFLECTION_BODY(MeshBuffer);
    public:
        std::string name;
        int byteLength = 0;
        std::vector<uint8_t> data;
        std::string uri;            // index in MeshRes.uriVec
                                    // considered as required here but not in the spec (need to clarify)
                                    // uri is not decoded(e.g. whitespace may be represented as %20)
    };

    REFLECTION_TYPE(MeshBufferView)
    CLASS(MeshBufferView, Fields)
    {
        REFLECTION_BODY(MeshBufferView);
    public:
        std::string name;
        int bufferIndex = -1;;   // Required
        size_t byteOffset{ 0 };  // minimum 0, default 0
        size_t byteLength{ 0 };  // required, minimum 1. 0 = invalid
        size_t byteStride{ 0 };  // minimum 4, maximum 252 (multiple of 4), default 0 =
                                 // understood to be tightly packed
        int target{ 0 };         // ["ARRAY_BUFFER", "ELEMENT_ARRAY_BUFFER"] for vertex indices
                                 // or attribs. Could be 0 for other data
    };

    // TODO: support Sparse
    REFLECTION_TYPE(Accessor)
    CLASS(Accessor, Fields)
    {
        REFLECTION_BODY(Accessor);
    public:
        int bufferViewIndex = -1;  // optional in spec but required here since sparse accessor
                                   // are not supported
        std::string name;
        size_t byteOffset = 0;
        bool normalized = false;    // optional.
        int componentType = 0;      // (required) One of TINYGLTF_COMPONENT_TYPE_***
        size_t count = 0;           // required
        std::string type;           // (required) One of TINYGLTF_TYPE_***   ..

        std::vector<float> min;  // optional. integer value is promoted to double
        std::vector<float> max;  // optional. integer value is promoted to double
    };

    REFLECTION_TYPE(Primitive)
    CLASS(Primitive, Fields)
    {
        REFLECTION_BODY(Primitive);
    public:
        std::string materialResPath;
        std::map<std::string, int> attributes;  // Required. type to accessor index in MeshRes.accessorVec
        int indices = -1;   // The index of the accessor that contains the indices.
        int mode = 4;  // The topology type of primitives to render.
        std::vector<std::map<std::string, int>> targets;
    };

    REFLECTION_TYPE(MeshRes)
    CLASS(MeshRes, Fields)
    {
        REFLECTION_BODY(MeshRes);
    public:
        static std::string GetExtName() { return "hmesh"; }
    public:
        int version = 1;
        std::string name;
        std::string skeletonPath;
        std::vector<Primitive> primitives;  // Required
        std::vector<float> weights;

        std::vector<Accessor> accessorVec;
        std::vector<MeshBufferView> bufferViewVec;
        std::vector<MeshBuffer> bufferVec;
    };
}
