#include "AssimpImporter.h"

#include "Runtime/Core/Log/Log.h"
#include "Runtime/Core/Meta/Json.h"
#include "Runtime/Resource/ResType/Mesh.h"
#include "Runtime/Resource/ResType/Material.h"
#include "Runtime/Resource/ResType/BasePbrMaterialBuilder.h"
#include "Runtime/Resource/ResType/Animation.h"
#include "Runtime/Resource/ResType/Skeleton.h"
#include "Runtime/Resource/AssetManager/AssetManager.h"
#include "Runtime/Resource/ConfigManager/ConfigManager.h"
#include "Runtime/Animation/Animdata.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include "Mesh.gen.h"
#include "Material.gen.h"
#include "Animation.gen.h"
#include "all_serializer.h"

#include <assimp/Importer.hpp>
#include <assimp/GltfMaterial.h>
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/material.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

// ---------------------------------------------------------------------------
// Windows Unicode path helpers
// Assimp's default IOSystem uses fopen() internally, which on Windows only
// accepts ANSI-encoded paths. Any path containing characters outside the
// current system code page (e.g. Chinese on a non-CJK locale, or any non-
// ASCII path when the process code-page is UTF-8) will fail.
// The fix: give Assimp a custom IOSystem that converts the UTF-8 path strings
// Assimp works with internally into wide strings and calls _wfopen().
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>

namespace {

static std::wstring Utf8ToWide(const char* utf8)
{
    if (!utf8 || !*utf8) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), len);
    return wide;
}

// Convert a std::filesystem::path to a UTF-8 std::string.
// We explicitly go through the native wstring so we never hit the
// narrow-string conversion that depends on the current locale/code-page.
static std::string PathToUtf8(const std::filesystem::path& p)
{
    std::wstring wide = p.wstring();
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                  nullptr, 0, nullptr, nullptr);
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                        utf8.data(), len, nullptr, nullptr);
    return utf8;
}

class WideCharIOStream final : public Assimp::IOStream
{
public:
    explicit WideCharIOStream(FILE* f) : mFile(f) {}
    ~WideCharIOStream() override
    {
        if (mFile) { ::fclose(mFile); mFile = nullptr; }
    }

    size_t Read(void* buf, size_t sz, size_t cnt) override
    { return ::fread(buf, sz, cnt, mFile); }

    size_t Write(const void* buf, size_t sz, size_t cnt) override
    { return ::fwrite(buf, sz, cnt, mFile); }

    aiReturn Seek(size_t offset, aiOrigin origin) override
    {
        return (::_fseeki64(mFile, static_cast<long long>(offset), origin) == 0)
               ? AI_SUCCESS : AI_FAILURE;
    }
    size_t Tell() const override
    { return static_cast<size_t>(::_ftelli64(mFile)); }

    size_t FileSize() const override
    {
        auto cur = ::_ftelli64(mFile);
        ::_fseeki64(mFile, 0, SEEK_END);
        auto sz = ::_ftelli64(mFile);
        ::_fseeki64(mFile, cur, SEEK_SET);
        return static_cast<size_t>(sz);
    }
    void Flush() override { ::fflush(mFile); }

private:
    FILE* mFile = nullptr;
};

// Assimp takes ownership of an IOSystem set via SetIOHandler() and will
// delete it when the Importer is destroyed — so always heap-allocate it.
class WideCharIOSystem final : public Assimp::IOSystem
{
public:
    bool Exists(const char* path) const override
    {
        return std::filesystem::exists(std::filesystem::path(Utf8ToWide(path)));
    }
    char getOsSeparator() const override { return '\\'; }

    Assimp::IOStream* Open(const char* path, const char* mode = "rb") override
    {
        std::wstring wpath = Utf8ToWide(path);
        std::wstring wmode(mode, mode + ::strlen(mode));
        FILE* f = ::_wfopen(wpath.c_str(), wmode.c_str());
        return f ? new WideCharIOStream(f) : nullptr;
    }
    void Close(Assimp::IOStream* stream) override { delete stream; }
};

} // anonymous namespace
#else
namespace {
static std::string PathToUtf8(const std::filesystem::path& p) { return p.string(); }
} // anonymous namespace
#endif // _WIN32

namespace HEngine
{
    // Convert aiMatrix4x4 (row-major) to a 16-float column-major array for glm::make_mat4
    static std::vector<float> AiMatToVec16(const aiMatrix4x4& m)
    {
        std::vector<float> v(16);
        v[0]=m.a1; v[1]=m.b1; v[2]=m.c1; v[3]=m.d1;
        v[4]=m.a2; v[5]=m.b2; v[6]=m.c2; v[7]=m.d2;
        v[8]=m.a3; v[9]=m.b3; v[10]=m.c3; v[11]=m.d3;
        v[12]=m.a4; v[13]=m.b4; v[14]=m.c4; v[15]=m.d4;
        return v;
    }

    static glm::mat4 AiMatToGlm(const aiMatrix4x4& m)
    {
        const std::vector<float> values = AiMatToVec16(m);
        return glm::make_mat4(values.data());
    }

    struct BoneNodeInfo
    {
        const aiNode* node = nullptr;
        glm::mat4 worldTransform = glm::mat4(1.0f);
    };

    static void BuildAnimNode(const aiNode* node, AnimNodeRes& animNode)
    {
        animNode.name           = node->mName.C_Str();
        animNode.localTransform = AiMatToVec16(node->mTransformation);
        animNode.children.reserve(node->mNumChildren);
        for (uint32_t i = 0; i < node->mNumChildren; ++i)
        {
            AnimNodeRes child;
            BuildAnimNode(node->mChildren[i], child);
            animNode.children.push_back(std::move(child));
        }
    }

    static void CollectBoneNodeInfos(
        const aiNode* node,
        const std::unordered_set<std::string>& boneNames,
        const glm::mat4& parentWorldTransform,
        std::unordered_map<std::string, BoneNodeInfo>& outBoneNodeInfos)
    {
        if (!node)
            return;

        const glm::mat4 localTransform = AiMatToGlm(node->mTransformation);
        const glm::mat4 worldTransform = parentWorldTransform * localTransform;
        const std::string nodeName = node->mName.C_Str();

        if (boneNames.find(nodeName) != boneNames.end() && outBoneNodeInfos.find(nodeName) == outBoneNodeInfos.end())
            outBoneNodeInfos.emplace(nodeName, BoneNodeInfo{ node, worldTransform });

        for (uint32_t i = 0; i < node->mNumChildren; ++i)
            CollectBoneNodeInfos(node->mChildren[i], boneNames, worldTransform, outBoneNodeInfos);
    }

    static void FillBoneLocalBindFromTransform(const glm::mat4& localBindTransform, SkeletonBoneRes& outBone)
    {
        glm::vec3 scale(1.0f);
        glm::quat orientation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 translation(0.0f);
        glm::vec3 skew(0.0f);
        glm::vec4 perspective(0.0f);

        if (glm::decompose(localBindTransform, scale, orientation, translation, skew, perspective))
        {
            outBone.localBindT = translation;
            outBone.localBindR = glm::normalize(orientation);
            outBone.localBindS = scale;
            return;
        }

        outBone.localBindT = glm::vec3(localBindTransform[3]);
        outBone.localBindR = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        outBone.localBindS = glm::vec3(1.0f);
    }

    static void BuildSkeletonResFromScene(
        const aiScene* scene,
        const std::map<std::string, BoneInfo>& boneMap,
        const std::string& skeletonName,
        SkeletonRes& outSkeleton)
    {
        outSkeleton = {};
        outSkeleton.name = skeletonName;
        if (!scene || !scene->mRootNode || boneMap.empty())
            return;

        std::unordered_set<std::string> boneNames;
        boneNames.reserve(boneMap.size());
        for (const auto& [boneName, info] : boneMap)
        {
            (void)info;
            boneNames.insert(boneName);
        }

        std::unordered_map<std::string, BoneNodeInfo> boneNodeInfos;
        boneNodeInfos.reserve(boneMap.size());
        CollectBoneNodeInfos(scene->mRootNode, boneNames, glm::mat4(1.0f), boneNodeInfos);

        std::vector<std::pair<std::string, BoneInfo>> sortedBones;
        sortedBones.reserve(boneMap.size());
        for (const auto& kv : boneMap)
            sortedBones.push_back(kv);
        std::sort(sortedBones.begin(), sortedBones.end(), [](const auto& a, const auto& b)
            {
                return a.second.id < b.second.id;
            });

        outSkeleton.bones.reserve(sortedBones.size());
        outSkeleton.inverseBindMatrices.reserve(sortedBones.size());

        for (const auto& [boneName, info] : sortedBones)
        {
            SkeletonBoneRes bone;
            bone.index = info.id;
            bone.name = boneName;

            auto boneNodeIt = boneNodeInfos.find(boneName);
            if (boneNodeIt != boneNodeInfos.end())
            {
                const aiNode* parentBoneNode = boneNodeIt->second.node ? boneNodeIt->second.node->mParent : nullptr;
                glm::mat4 parentBoneWorldTransform(1.0f);

                while (parentBoneNode)
                {
                    auto parentBoneIt = boneMap.find(parentBoneNode->mName.C_Str());
                    if (parentBoneIt != boneMap.end())
                    {
                        bone.parentIndex = parentBoneIt->second.id;

                        auto parentNodeInfoIt = boneNodeInfos.find(parentBoneNode->mName.C_Str());
                        if (parentNodeInfoIt != boneNodeInfos.end())
                            parentBoneWorldTransform = parentNodeInfoIt->second.worldTransform;
                        break;
                    }
                    parentBoneNode = parentBoneNode->mParent;
                }

                const glm::mat4 localBindTransform = bone.parentIndex >= 0
                    ? glm::inverse(parentBoneWorldTransform) * boneNodeIt->second.worldTransform
                    : boneNodeIt->second.worldTransform;
                FillBoneLocalBindFromTransform(localBindTransform, bone);
            }
            else
            {
                HE_CORE_WARN("AssimpImporter: bone node '{0}' not found in scene hierarchy while building skeleton '{1}'", boneName, skeletonName);
            }

            outSkeleton.bones.push_back(bone);

            std::vector<float> mat(16);
            const float* ptr = glm::value_ptr(info.offset);
            std::copy(ptr, ptr + 16, mat.data());
            outSkeleton.inverseBindMatrices.push_back(std::move(mat));
        }
    }

    static Json ToJsonAnimNode(const AnimNodeRes& node)
    {
        Json j;
        j["name"] = node.name;
        j["localTransform"] = node.localTransform;

        j["children"] = Json::array();
        for (const auto& child : node.children)
            j["children"].push_back(ToJsonAnimNode(child));
        return j;
    }

    static bool SaveAnimResJson(const AnimRes& animRes, const std::filesystem::path& outPath)
    {
        Json j;
        j["version"] = animRes.version;
        j["name"] = animRes.name;
        j["meshResPath"] = animRes.meshResPath;
        j["targetSkeletonPath"] = animRes.targetSkeletonPath;

        j["boneInfos"] = Json::array();
        for (const auto& bi : animRes.boneInfos)
        {
            Json jb;
            jb["name"] = bi.name;
            jb["id"] = bi.id;
            jb["offsetMatrix"] = bi.offsetMatrix;
            j["boneInfos"].push_back(std::move(jb));
        }

        j["clips"] = Json::array();
        for (const auto& clip : animRes.clips)
        {
            Json jc;
            jc["name"] = clip.name;
            jc["duration"] = clip.duration;
            jc["ticksPerSecond"] = clip.ticksPerSecond;
            jc["channels"] = Json::array();

            for (const auto& ch : clip.channels)
            {
                Json jch;
                jch["name"] = ch.name;

                jch["positionKeys"] = Json::array();
                for (const auto& k : ch.positionKeys)
                {
                    Json jk;
                    jk["px"] = k.px;
                    jk["py"] = k.py;
                    jk["pz"] = k.pz;
                    jk["timeStamp"] = k.timeStamp;
                    jch["positionKeys"].push_back(std::move(jk));
                }

                jch["rotationKeys"] = Json::array();
                for (const auto& k : ch.rotationKeys)
                {
                    Json jk;
                    jk["qw"] = k.qw;
                    jk["qx"] = k.qx;
                    jk["qy"] = k.qy;
                    jk["qz"] = k.qz;
                    jk["timeStamp"] = k.timeStamp;
                    jch["rotationKeys"].push_back(std::move(jk));
                }

                jch["scaleKeys"] = Json::array();
                for (const auto& k : ch.scaleKeys)
                {
                    Json jk;
                    jk["sx"] = k.sx;
                    jk["sy"] = k.sy;
                    jk["sz"] = k.sz;
                    jk["timeStamp"] = k.timeStamp;
                    jch["scaleKeys"].push_back(std::move(jk));
                }

                jc["channels"].push_back(std::move(jch));
            }

            j["clips"].push_back(std::move(jc));
        }

        j["rootNode"] = ToJsonAnimNode(animRes.rootNode);

        std::ofstream fout(outPath);
        if (!fout)
        {
            HE_CORE_ERROR("AssimpImporter: failed to open animation output '{0}'", outPath.string());
            return false;
        }
        fout << std::setw(4) << j;
        fout.flush();
        if (!fout)
        {
            HE_CORE_ERROR("AssimpImporter: failed to write animation output '{0}'", outPath.string());
            return false;
        }
        return true;
    }

    // Collect every bone from every mesh into a shared map
    static void CollectBones(const aiScene* scene,
                             std::map<std::string, BoneInfo>& boneMap,
                             int& boneCounter)
    {
        for (uint32_t m = 0; m < scene->mNumMeshes; ++m)
        {
            aiMesh* mesh = scene->mMeshes[m];
            for (uint32_t b = 0; b < mesh->mNumBones; ++b)
            {
                std::string name = mesh->mBones[b]->mName.C_Str();
                if (boneMap.find(name) == boneMap.end())
                {
                    BoneInfo bi;
                    bi.id     = boneCounter++;
                    bi.offset = glm::make_mat4(AiMatToVec16(mesh->mBones[b]->mOffsetMatrix).data());
                    boneMap[name] = bi;
                }
            }
        }
    }

    // Append raw bytes to a buffer
    template<typename T>
    static void AppendData(std::vector<uint8_t>& buf, const T* data, size_t count)
    {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        buf.insert(buf.end(), bytes, bytes + sizeof(T) * count);
    }

    // Create an Accessor + BufferView pointing at [offset, offset+bytes) inside bufferIndex
    static int AddAccessor(MeshRes& meshRes, int bufferIndex,
                           size_t byteOffset, size_t byteLength,
                           int componentType, const std::string& type,
                           size_t count)
    {
        MeshBufferView bv;
        bv.bufferIndex = bufferIndex;
        bv.byteOffset  = byteOffset;
        bv.byteLength  = byteLength;
        bv.byteStride  = 0;
        int bvIdx = (int)meshRes.bufferViewVec.size();
        meshRes.bufferViewVec.push_back(bv);

        Accessor acc;
        acc.bufferViewIndex = bvIdx;
        acc.byteOffset      = 0;
        acc.componentType   = componentType;
        acc.count           = count;
        acc.type            = type;
        acc.normalized      = false;
        int accIdx = (int)meshRes.accessorVec.size();
        meshRes.accessorVec.push_back(acc);
        return accIdx;
    }

    static std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    static std::string TrimDot(std::string extension)
    {
        if (!extension.empty() && extension.front() == '.')
            extension.erase(extension.begin());
        return extension;
    }

    static std::string SanitizeToken(std::string token)
    {
        for (char& ch : token)
        {
            const bool bValid =
                (ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '_' ||
                ch == '-';
            if (!bValid)
                ch = '_';
        }
        return token;
    }

    static std::string DetectCompressedImageExtension(const aiTexture* embeddedTex)
    {
        if (!embeddedTex || embeddedTex->mHeight != 0 || !embeddedTex->pcData || embeddedTex->mWidth == 0)
            return {};

        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(embeddedTex->pcData);
        const size_t size = static_cast<size_t>(embeddedTex->mWidth);

        if (size >= 8 && std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0)
            return "png";
        if (size >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
            return "jpg";
        if (size >= 2 && bytes[0] == 'B' && bytes[1] == 'M')
            return "bmp";
        if (size >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ')
            return "dds";
        if (size >= 6 && (std::memcmp(bytes, "GIF87a", 6) == 0 || std::memcmp(bytes, "GIF89a", 6) == 0))
            return "gif";
        if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0)
            return "webp";
        if (size >= 4 &&
            ((bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == 42 && bytes[3] == 0) ||
             (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0 && bytes[3] == 42)))
            return "tif";

        return {};
    }

    static std::string GetEmbeddedTextureExtension(const aiTexture* embeddedTex, const aiString& textureRef)
    {
        if (!embeddedTex)
            return "bin";

        if (embeddedTex->mHeight != 0)
            return "tga";

        std::string hint = ToLowerAscii(std::string(embeddedTex->achFormatHint));
        hint = TrimDot(hint);
        if (!hint.empty())
        {
            const bool bAlphaNumeric = std::all_of(hint.begin(), hint.end(),
                [](unsigned char ch) { return std::isalnum(ch) != 0; });
            if (bAlphaNumeric)
            {
                if (hint == "jpeg")
                    return "jpg";
                return hint;
            }
        }

        std::string refExt = TrimDot(ToLowerAscii(std::filesystem::path(textureRef.C_Str()).extension().string()));
        if (!refExt.empty())
            return refExt;

        std::string embeddedExt = TrimDot(ToLowerAscii(std::filesystem::path(embeddedTex->mFilename.C_Str()).extension().string()));
        if (!embeddedExt.empty())
            return embeddedExt;

        std::string detectedExt = DetectCompressedImageExtension(embeddedTex);
        if (!detectedExt.empty())
            return detectedExt;

        return "bin";
    }

    static bool WriteEmbeddedTexture(const aiTexture* embeddedTex, const std::filesystem::path& outPath)
    {
        if (!embeddedTex || !embeddedTex->pcData)
            return false;

        std::ofstream fout(outPath, std::ios::binary);
        if (!fout)
            return false;

        if (embeddedTex->mHeight == 0)
        {
            const size_t byteSize = static_cast<size_t>(embeddedTex->mWidth);
            fout.write(reinterpret_cast<const char*>(embeddedTex->pcData), static_cast<std::streamsize>(byteSize));
            return static_cast<bool>(fout);
        }

        const uint32_t width = embeddedTex->mWidth;
        const uint32_t height = embeddedTex->mHeight;
        if (width == 0 || height == 0)
            return false;

        uint8_t tgaHeader[18] = {};
        tgaHeader[2] = 2; // uncompressed true-color image
        tgaHeader[12] = static_cast<uint8_t>(width & 0xFF);
        tgaHeader[13] = static_cast<uint8_t>((width >> 8) & 0xFF);
        tgaHeader[14] = static_cast<uint8_t>(height & 0xFF);
        tgaHeader[15] = static_cast<uint8_t>((height >> 8) & 0xFF);
        tgaHeader[16] = 32;
        tgaHeader[17] = 8;

        fout.write(reinterpret_cast<const char*>(tgaHeader), sizeof(tgaHeader));
        const size_t pixelByteSize = static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(aiTexel);
        fout.write(reinterpret_cast<const char*>(embeddedTex->pcData), static_cast<std::streamsize>(pixelByteSize));

        return static_cast<bool>(fout);
    }

    static std::string BuildEmbeddedTextureFileName(const std::string& modelStem,
                                                    uint32_t materialIndex,
                                                    aiTextureType texType,
                                                    int embeddedIndex,
                                                    const aiTexture* embeddedTex,
                                                    const std::string& extension)
    {
        std::string baseName;
        if (embeddedTex && embeddedTex->mFilename.length > 0)
            baseName = std::filesystem::path(embeddedTex->mFilename.C_Str()).stem().string();

        if (baseName.empty())
        {
            const char* typeName = aiTextureTypeToString(texType);
            baseName = modelStem + "_mat" + std::to_string(materialIndex) + "_" +
                (typeName ? typeName : "texture");
            if (embeddedIndex >= 0)
                baseName += "_" + std::to_string(embeddedIndex);
        }

        baseName = SanitizeToken(baseName);
        return baseName + "." + extension;
    }

    enum class MaterialTextureSlot
    {
        Albedo,
        Normal,
        MetallicRoughness,
        Ao,
        Emissive,
        Unknown
    };

    static bool ContainsAnyToken(const std::string& text,
                                 std::initializer_list<const char*> tokens)
    {
        for (const char* token : tokens)
        {
            if (token && text.find(token) != std::string::npos)
                return true;
        }
        return false;
    }

    static MaterialTextureSlot ClassifyTextureSlotByType(aiTextureType texType)
    {
        switch (texType)
        {
        case aiTextureType_BASE_COLOR:
        case aiTextureType_DIFFUSE:
            return MaterialTextureSlot::Albedo;
        case aiTextureType_NORMAL_CAMERA:
        case aiTextureType_NORMALS:
        case aiTextureType_HEIGHT:
            return MaterialTextureSlot::Normal;
        case aiTextureType_GLTF_METALLIC_ROUGHNESS:
        case aiTextureType_METALNESS:
        case aiTextureType_DIFFUSE_ROUGHNESS:
        case aiTextureType_SPECULAR:
        case aiTextureType_SHININESS:
            return MaterialTextureSlot::MetallicRoughness;
        case aiTextureType_AMBIENT_OCCLUSION:
        case aiTextureType_LIGHTMAP:
        case aiTextureType_AMBIENT:
            return MaterialTextureSlot::Ao;
        case aiTextureType_EMISSION_COLOR:
        case aiTextureType_EMISSIVE:
            return MaterialTextureSlot::Emissive;
        default:
            return MaterialTextureSlot::Unknown;
        }
    }

    static MaterialTextureSlot ClassifyTextureSlotByText(const std::string& lowerText)
    {
        if (ContainsAnyToken(lowerText, { "normal", "_nrm", "_nor", "_normal", "bump" }))
            return MaterialTextureSlot::Normal;
        if (ContainsAnyToken(lowerText, { "metalrough", "metallicroughness", "roughness", "metallic", "orm", "rma", "mra", "specular" }))
            return MaterialTextureSlot::MetallicRoughness;
        if (ContainsAnyToken(lowerText, { "occlusion", "ambientocclusion", "_ao", "ao_", "lightmap" }))
            return MaterialTextureSlot::Ao;
        if (ContainsAnyToken(lowerText, { "emissive", "emission", "emit", "glow", "selfillum" }))
            return MaterialTextureSlot::Emissive;
        if (ContainsAnyToken(lowerText, { "basecolor", "base_color", "albedo", "diffuse", "color", "col" }))
            return MaterialTextureSlot::Albedo;
        return MaterialTextureSlot::Unknown;
    }

	static int ConvertTextureWrapMode(aiTextureMapMode mode)
	{
		switch (mode)
		{
		case aiTextureMapMode_Wrap:
			return HE_TEXTURE_WRAP_REPEAT;
		case aiTextureMapMode_Mirror:
			return HE_TEXTURE_WRAP_MIRRORED_REPEAT;
		case aiTextureMapMode_Clamp:
		case aiTextureMapMode_Decal:
		default:
			return HE_TEXTURE_WRAP_CLAMP_TO_EDGE;
		}
	}

	static void ApplyImportedTextureSampler(
		const aiMaterial& material,
		aiTextureType textureType,
		uint32_t textureIndex,
		TextureRes& texture)
	{
		aiTextureMapMode wrapU = aiTextureMapMode_Clamp;
		if (material.Get(
				AI_MATKEY_MAPPINGMODE_U(textureType, textureIndex),
				wrapU) == AI_SUCCESS)
		{
			texture.sampler.wrapS = ConvertTextureWrapMode(wrapU);
		}
		aiTextureMapMode wrapV = aiTextureMapMode_Clamp;
		if (material.Get(
				AI_MATKEY_MAPPINGMODE_V(textureType, textureIndex),
				wrapV) == AI_SUCCESS)
		{
			texture.sampler.wrapT = ConvertTextureWrapMode(wrapV);
		}
		uint32_t magnificationFilter = 0;
		if (material.Get(
				AI_MATKEY_GLTF_MAPPINGFILTER_MAG(textureType, textureIndex),
				magnificationFilter) == AI_SUCCESS)
		{
			texture.sampler.magFilter =
				static_cast<int>(magnificationFilter);
		}
		uint32_t minificationFilter = 0;
		if (material.Get(
				AI_MATKEY_GLTF_MAPPINGFILTER_MIN(textureType, textureIndex),
				minificationFilter) == AI_SUCCESS)
		{
			texture.sampler.minFilter =
				static_cast<int>(minificationFilter);
		}
	}

    static bool IsKnownImageExtension(const std::string& extensionLower)
    {
        return ContainsAnyToken(extensionLower,
            { "png", "jpg", "jpeg", "tga", "tif", "tiff", "bmp", "dds", "webp", "exr", "hdr", "ktx", "ktx2", "psd" });
    }

    static bool ExtractMaterialStringValue(const aiMaterialProperty* prop, std::string& outValue)
    {
        if (!prop || prop->mType != aiPTI_String || !prop->mData || prop->mDataLength < 5)
            return false;

        const uint32_t strLen = *reinterpret_cast<const uint32_t*>(prop->mData);
        const size_t valueOffset = 4;
        const size_t valueBytes = static_cast<size_t>(strLen);
        if (valueOffset + valueBytes > static_cast<size_t>(prop->mDataLength))
            return false;

        outValue.assign(prop->mData + valueOffset, prop->mData + valueOffset + valueBytes);
        return true;
    }

    static bool LooksLikeTextureReference(const std::string& value, const aiScene* scene)
    {
        if (value.empty())
            return false;
        if (value.front() == '*')
            return true;

        if (scene)
        {
            const aiTexture* embedded = scene->GetEmbeddedTexture(value.c_str());
            if (embedded)
                return true;
        }

        const std::string lower = ToLowerAscii(value);
        const std::string ext = TrimDot(ToLowerAscii(std::filesystem::path(value).extension().string()));
        if (!ext.empty() && IsKnownImageExtension(ext))
            return true;

        return lower.find('\\') != std::string::npos || lower.find('/') != std::string::npos;
    }

    class CreatedMeshPairGuard final
    {
    public:
        ~CreatedMeshPairGuard()
        {
            if (!bKeepFiles)
                (void)Cleanup();
        }

        void SetPaths(const std::filesystem::path& binPath, const std::filesystem::path& meshPath)
        {
            mBinPath = binPath;
            mMeshPath = meshPath;
        }

        void MarkBinCreated() { bBinCreated = true; }
        void MarkMeshCreated() { bMeshCreated = true; }
        [[nodiscard]] bool HasBin() const { return bBinCreated; }
        void KeepFiles() { bKeepFiles = true; }

        [[nodiscard]] bool Cleanup() noexcept
        {
            bool bCleanupSucceeded = true;
            const auto removeCreated = [&bCleanupSucceeded](const std::filesystem::path& path, bool& bCreated)
            {
                if (!bCreated)
                    return;

                std::error_code removeError;
                std::filesystem::remove(path, removeError);
                if (removeError)
                {
                    bCleanupSucceeded = false;
                    return;
                }
                bCreated = false;
            };

            removeCreated(mMeshPath, bMeshCreated);
            removeCreated(mBinPath, bBinCreated);
            return bCleanupSucceeded;
        }

    private:
        std::filesystem::path mBinPath;
        std::filesystem::path mMeshPath;
        bool bBinCreated = false;
        bool bMeshCreated = false;
        bool bKeepFiles = false;
    };

    bool AssimpImporter::Import(const std::filesystem::path& assetPath,
                                const std::filesystem::path& outDir)
    {
        // PathToUtf8 is always safe: on Windows it goes wstring→UTF-8 via
        // WideCharToMultiByte, bypassing the locale-dependent narrow conversion.
        const std::string utf8Path = PathToUtf8(assetPath);

        Assimp::Importer importer;

#ifdef _WIN32
        // Hand Assimp a custom IOSystem so that every internal fopen() call
        // goes through _wfopen(), making Unicode (e.g. Chinese) paths work.
        // SetIOHandler transfers ownership — Assimp deletes it on ~Importer().
        importer.SetIOHandler(new WideCharIOSystem());
#endif

		const std::string sourceExtension = ToLowerAscii(
			assetPath.extension().string());
		const bool bGltfSource =
			sourceExtension == ".gltf" || sourceExtension == ".glb";
		unsigned int postProcessFlags =
			aiProcess_Triangulate |
			aiProcess_GenSmoothNormals |
			aiProcess_CalcTangentSpace |
			aiProcess_LimitBoneWeights |
			aiProcess_FlipUVs |
			aiProcess_JoinIdenticalVertices;
		// Assimp's glTF reader converts raw glTF UVs to Assimp's lower-left
		// convention internally. HEngine uploads images without a vertical flip,
		// so this post-process conversion restores the source glTF UVs. It also
		// keeps the generated tangent basis paired with the final coordinates.

		const aiScene* scene = importer.ReadFile(utf8Path, postProcessFlags);

        if (!scene || !scene->mRootNode)
        {
            HE_CORE_ERROR("AssimpImporter: failed to load '{0}': {1}",
                          utf8Path, importer.GetErrorString());
            return false;
        }

        std::string stem = assetPath.stem().string();

        // ---- Collect global bone info ----
        std::map<std::string, BoneInfo> boneMap;
        int boneCounter = 0;
        bool hasSkinning = false;
        bool hasRenderableMesh = false;
        for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            aiMesh* mesh = scene->mMeshes[mi];
            if (mesh->mNumBones > 0)
                hasSkinning = true;
            if (mesh->mNumVertices > 0 && mesh->mNumFaces > 0)
                hasRenderableMesh = true;
        }
        if (hasSkinning)
            CollectBones(scene, boneMap, boneCounter);
		else if (bGltfSource && scene->mNumAnimations == 0)
		{
			// Static glTF nodes may carry the model's authored unit conversion or
			// may instance one mesh multiple times. Bake that hierarchy so dropping
			// an imported .hmesh into a scene at identity reproduces the source.
			const aiScene* transformedScene = importer.ApplyPostProcessing(
				aiProcess_PreTransformVertices);
			if (!transformedScene || !transformedScene->mRootNode)
			{
				HE_CORE_ERROR(
					"AssimpImporter: failed to bake static node transforms for '{0}': {1}",
					utf8Path,
					importer.GetErrorString());
				return false;
			}
			scene = transformedScene;
		}

		std::unordered_set<uint32_t> usedMaterialIndices;
		for (uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
		{
			const aiMesh* mesh = scene->mMeshes[meshIndex];
			if (mesh->mNumVertices > 0 && mesh->mNumFaces > 0)
				usedMaterialIndices.insert(mesh->mMaterialIndex);
		}

        std::string meshPathRelative;
        std::string skeletonPathRelative;
        bool bImportSucceeded = true;
        CreatedMeshPairGuard createdMeshPair;

        if (hasRenderableMesh)
        {
            // ---- Process materials ----
            std::unordered_map<uint32_t, std::string> matResPathMap;
			std::unordered_map<std::string, std::string> importedTextureCache;
			uint32_t importedMaterialOrdinal = 0;
			for (uint32_t mi = 0; mi < scene->mNumMaterials; ++mi)
			{
				if (!usedMaterialIndices.contains(mi))
					continue;
				aiMaterial* mat = scene->mMaterials[mi];
				PbrMaterialRes pbrMat;

            struct TextureCandidate
            {
                aiTextureType semantic = aiTextureType_UNKNOWN;
                MaterialTextureSlot keyHint = MaterialTextureSlot::Unknown;
                std::string textureRef;
                std::string textureRelPath;
            };

            aiString name;
            if (mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS)
                pbrMat.name = name.C_Str();
            const std::string materialName = pbrMat.name.empty() ? std::string("<unnamed>") : pbrMat.name;

            HE_CORE_INFO("AssimpImporter: material '{0}' texture probe start (properties={1}, embeddedTextures={2})",
                         materialName,
                         mat->mNumProperties,
                         scene->mNumTextures);

            auto importTextureRef = [&](aiTextureType texType,
                                        const std::string& textureRef,
                                        std::string& outRelPath) -> bool
            {
                if (textureRef.empty())
                    return false;

                const std::string cacheKey = ToLowerAscii(textureRef);
                if (auto cached = importedTextureCache.find(cacheKey); cached != importedTextureCache.end())
                {
                    outRelPath = cached->second;
                    return !outRelPath.empty();
                }

                const auto [embeddedTex, embeddedIndex] = scene->GetEmbeddedTextureAndIndex(textureRef.c_str());
                if (embeddedTex)
                {
                    aiString refString;
                    refString.Set(textureRef);
                    const std::string extension = GetEmbeddedTextureExtension(embeddedTex, refString);
                    const std::string fileName = BuildEmbeddedTextureFileName(
                        stem, mi, texType, embeddedIndex, embeddedTex, extension);

                    auto dstTex = AssetManager::GetAvailableFileName(fileName.c_str(), outDir);
                    if (!WriteEmbeddedTexture(embeddedTex, dstTex))
                    {
                        HE_CORE_WARN("AssimpImporter: failed to export embedded texture '{0}' ({1}) from '{2}'",
                                     textureRef, aiTextureTypeToString(texType), utf8Path);
                        bImportSucceeded = false;
                        return false;
                    }

                    outRelPath = ConfigManager::GetRelativePath(dstTex);
                    if (outRelPath.empty())
                    {
                        bImportSucceeded = false;
                        return false;
                    }

                    importedTextureCache[cacheKey] = outRelPath;
                    HE_CORE_INFO("AssimpImporter: material '{0}' imported embedded {1} '{2}' -> '{3}'",
                                 materialName, aiTextureTypeToString(texType), textureRef, outRelPath);
                    return true;
                }

                std::filesystem::path srcTex = std::filesystem::path(textureRef);
                if (!srcTex.is_absolute())
                    srcTex = assetPath.parent_path() / srcTex;

                if (!std::filesystem::exists(srcTex))
                {
                    std::filesystem::path fallbackTex = assetPath.parent_path() / std::filesystem::path(textureRef).filename();
                    if (std::filesystem::exists(fallbackTex))
                        srcTex = fallbackTex;
                }

                if (!std::filesystem::exists(srcTex))
                {
                    HE_CORE_WARN("AssimpImporter: missing texture '{0}' ({1}) for material '{2}' in '{3}'",
                                 textureRef, aiTextureTypeToString(texType), materialName, utf8Path);
                    bImportSucceeded = false;
                    return false;
                }

                auto dstTex = AssetManager::GetAvailableFileName(
                    srcTex.filename().string().c_str(), outDir);
                std::error_code copyEc;
                std::filesystem::copy_file(srcTex, dstTex, copyEc);
                if (copyEc)
                {
                    HE_CORE_WARN("AssimpImporter: failed to copy texture '{0}' -> '{1}', reason: {2}",
                                 srcTex.string(), dstTex.string(), copyEc.message());
                    bImportSucceeded = false;
                    return false;
                }

                outRelPath = ConfigManager::GetRelativePath(dstTex);
                if (outRelPath.empty())
                {
                    bImportSucceeded = false;
                    return false;
                }

                importedTextureCache[cacheKey] = outRelPath;
                HE_CORE_INFO("AssimpImporter: material '{0}' imported external {1} '{2}' -> '{3}'",
                             materialName, aiTextureTypeToString(texType), textureRef, outRelPath);
                return true;
            };

			auto importTextureWithSampler = [&](
				aiTextureType texType,
				std::string& outRelPath,
				TextureRes& textureRes) -> bool
			{
				const unsigned int textureCount = mat->GetTextureCount(texType);
				for (unsigned int textureIndex = 0;
					textureIndex < textureCount;
					++textureIndex)
				{
					aiString texturePath;
					if (mat->GetTexture(
							texType,
							textureIndex,
							&texturePath) != AI_SUCCESS)
					{
						continue;
					}
					if (!importTextureRef(
							texType,
							texturePath.C_Str(),
							outRelPath))
					{
						continue;
					}
					ApplyImportedTextureSampler(
						*mat,
						texType,
						textureIndex,
						textureRes);
					return true;
				}
				return false;
			};

            auto assignSlotIfMissing = [&](MaterialTextureSlot slot,
                                           const std::string& textureRelPath) -> bool
            {
                if (textureRelPath.empty())
                    return false;

                switch (slot)
                {
                case MaterialTextureSlot::Albedo:
                    if (pbrMat.bUseAlbedoMap)
                        return false;
                    pbrMat.pbrMetallicRoughness.baseColorTexture.textureRes.textureSourcePath = textureRelPath;
                    pbrMat.bUseAlbedoMap = true;
                    return true;
                case MaterialTextureSlot::Normal:
                    if (pbrMat.bUseNormalMap)
                        return false;
                    pbrMat.normalTexture.textureRes.textureSourcePath = textureRelPath;
                    pbrMat.bUseNormalMap = true;
                    return true;
                case MaterialTextureSlot::MetallicRoughness:
                    if (pbrMat.bUseMetallicRoughnessMap)
                        return false;
                    pbrMat.pbrMetallicRoughness.metallicRoughnessTexture.textureRes.textureSourcePath = textureRelPath;
                    pbrMat.bUseMetallicRoughnessMap = true;
                    return true;
                case MaterialTextureSlot::Ao:
                    if (pbrMat.bUseAoMap)
                        return false;
                    pbrMat.occlusionTexture.textureRes.textureSourcePath = textureRelPath;
                    pbrMat.bUseAoMap = true;
                    return true;
                case MaterialTextureSlot::Emissive:
                    if (pbrMat.bUseEmissiveMap)
                        return false;
                    pbrMat.emissiveTexture.textureRes.textureSourcePath = textureRelPath;
                    pbrMat.bUseEmissiveMap = true;
                    return true;
                default:
                    return false;
                }
            };

            auto collectTextureCandidatesFromProperties = [&](std::vector<TextureCandidate>& outCandidates)
            {
                std::unordered_set<std::string> seenRefs;
                for (uint32_t pi = 0; pi < mat->mNumProperties; ++pi)
                {
                    const aiMaterialProperty* prop = mat->mProperties[pi];
                    if (!prop)
                        continue;

                    const char* keyRaw = prop->mKey.C_Str();
                    if (!keyRaw || keyRaw[0] == '\0')
                        continue;

                    const std::string propKey = keyRaw;
                    const std::string propKeyLower = ToLowerAscii(propKey);
                    const bool bIsTextureKey = propKey == "$tex.file";
                    const bool bLooksLikeRawTextureKey =
                        !bIsTextureKey &&
                        prop->mType == aiPTI_String &&
                        (propKeyLower.find("texture") != std::string::npos ||
                         propKeyLower.find("file") != std::string::npos);
                    if (!bIsTextureKey && !bLooksLikeRawTextureKey)
                        continue;

                    std::string textureRef;
                    if (!ExtractMaterialStringValue(prop, textureRef))
                    {
                        aiString textureValue;
                        if (mat->Get(prop->mKey.C_Str(), prop->mSemantic, prop->mIndex, textureValue) != AI_SUCCESS)
                            continue;
                        textureRef = textureValue.C_Str();
                    }

                    if (!LooksLikeTextureReference(textureRef, scene))
                        continue;

                    const std::string dedupeKey = ToLowerAscii(textureRef);
                    if (!seenRefs.insert(dedupeKey).second)
                        continue;

                    TextureCandidate candidate;
                    candidate.semantic = static_cast<aiTextureType>(prop->mSemantic);
                    candidate.keyHint = ClassifyTextureSlotByText(propKeyLower);
                    candidate.textureRef = textureRef;
                    outCandidates.push_back(std::move(candidate));

                    HE_CORE_INFO("AssimpImporter: material '{0}' candidate texture key='{1}', semantic={2}, index={3}, ref='{4}'",
                                 materialName,
                                 propKey,
                                 prop->mSemantic,
                                 prop->mIndex,
                                 textureRef);
                }
            };

            auto applyTextureFallbackFromCandidates = [&]()
            {
                std::vector<TextureCandidate> candidates;
                collectTextureCandidatesFromProperties(candidates);
                if (candidates.empty())
                    return;

                HE_CORE_INFO("AssimpImporter: material '{0}' fallback texture scan found {1} candidate(s)",
                             materialName, static_cast<uint32_t>(candidates.size()));

                for (auto& candidate : candidates)
                {
                    std::string relPath;
                    if (!importTextureRef(candidate.semantic, candidate.textureRef, relPath))
                        continue;
                    candidate.textureRelPath = relPath;
                }

                for (const auto& candidate : candidates)
                {
                    if (candidate.textureRelPath.empty())
                        continue;

                    MaterialTextureSlot slot = ClassifyTextureSlotByType(candidate.semantic);
                    if (slot == MaterialTextureSlot::Unknown)
                        slot = candidate.keyHint;
                    if (slot == MaterialTextureSlot::Unknown)
                    {
                        const std::string nameLower = ToLowerAscii(std::filesystem::path(candidate.textureRef).stem().string());
                        slot = ClassifyTextureSlotByText(nameLower);
                    }

                    assignSlotIfMissing(slot, candidate.textureRelPath);
                }

                for (const auto& candidate : candidates)
                {
                    if (candidate.textureRelPath.empty())
                        continue;

                    if (assignSlotIfMissing(MaterialTextureSlot::Albedo, candidate.textureRelPath))
                        break;
                }
            };

            // Diffuse/albedo texture
            {
                std::string albedoPath;
				TextureRes& texture = pbrMat.pbrMetallicRoughness
					.baseColorTexture.textureRes;
				if (importTextureWithSampler(
						aiTextureType_BASE_COLOR,
						albedoPath,
						texture) ||
					importTextureWithSampler(
						aiTextureType_DIFFUSE,
						albedoPath,
						texture))
                {
					texture.textureSourcePath = albedoPath;
                    pbrMat.bUseAlbedoMap = true;
                }
            }

            // Normal texture
            {
                std::string normalPath;
				TextureRes& texture = pbrMat.normalTexture.textureRes;
				if (importTextureWithSampler(
						aiTextureType_NORMAL_CAMERA,
						normalPath,
						texture) ||
					importTextureWithSampler(
						aiTextureType_NORMALS,
						normalPath,
						texture))
                {
					texture.textureSourcePath = normalPath;
                    pbrMat.bUseNormalMap = true;
                }
            }

            // glTF metallicRoughnessTexture may map to GLTF_METALLIC_ROUGHNESS,
            // METALNESS or DIFFUSE_ROUGHNESS.
            {
                std::string metallicRoughnessPath;
				TextureRes& texture = pbrMat.pbrMetallicRoughness
					.metallicRoughnessTexture.textureRes;
				if (importTextureWithSampler(
						aiTextureType_GLTF_METALLIC_ROUGHNESS,
						metallicRoughnessPath,
						texture) ||
					importTextureWithSampler(
						aiTextureType_METALNESS,
						metallicRoughnessPath,
						texture) ||
					importTextureWithSampler(
						aiTextureType_DIFFUSE_ROUGHNESS,
						metallicRoughnessPath,
						texture))
                {
					texture.textureSourcePath = metallicRoughnessPath;
                    pbrMat.bUseMetallicRoughnessMap = true;
                }
            }

            // glTF occlusionTexture is usually mapped as LIGHTMAP in Assimp.
            {
                std::string aoPath;
				TextureRes& texture = pbrMat.occlusionTexture.textureRes;
				if (importTextureWithSampler(
						aiTextureType_AMBIENT_OCCLUSION,
						aoPath,
						texture) ||
					importTextureWithSampler(
						aiTextureType_LIGHTMAP,
						aoPath,
						texture) ||
					importTextureWithSampler(
						aiTextureType_AMBIENT,
						aoPath,
						texture))
                {
					texture.textureSourcePath = aoPath;
                    pbrMat.bUseAoMap = true;
                }
            }

            // Emissive texture
            {
                std::string emissivePath;
				TextureRes& texture = pbrMat.emissiveTexture.textureRes;
				if (importTextureWithSampler(
						aiTextureType_EMISSION_COLOR,
						emissivePath,
						texture) ||
					importTextureWithSampler(
						aiTextureType_EMISSIVE,
						emissivePath,
						texture))
                {
					texture.textureSourcePath = emissivePath;
                    pbrMat.bUseEmissiveMap = true;
                }
            }

            if (!pbrMat.bUseAlbedoMap ||
                !pbrMat.bUseNormalMap ||
                !pbrMat.bUseMetallicRoughnessMap ||
                !pbrMat.bUseAoMap ||
                !pbrMat.bUseEmissiveMap)
            {
                applyTextureFallbackFromCandidates();
            }

            // Material factors/colors fallback for assets with no texture maps.
            {
                aiString alphaMode;
                if (mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS)
                {
                    pbrMat.alphaMode = alphaMode.C_Str();
                }

                float alphaCutoff = pbrMat.alphaCutoff;
                if (mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alphaCutoff) == AI_SUCCESS)
                {
                    pbrMat.alphaCutoff = std::clamp(alphaCutoff, 0.0f, 1.0f);
                }

                int doubleSided = pbrMat.doubleSided ? 1 : 0;
                if (mat->Get(AI_MATKEY_TWOSIDED, doubleSided) == AI_SUCCESS)
                {
                    pbrMat.doubleSided = doubleSided != 0;
                }

                aiColor4D baseColor4;
                if (mat->Get(AI_MATKEY_BASE_COLOR, baseColor4) == AI_SUCCESS)
                {
                    pbrMat.pbrMetallicRoughness.baseColorFactor = {
                        std::clamp(baseColor4.r, 0.0f, 1.0f),
                        std::clamp(baseColor4.g, 0.0f, 1.0f),
                        std::clamp(baseColor4.b, 0.0f, 1.0f),
                        std::clamp(baseColor4.a, 0.0f, 1.0f)
                    };
                }
                else
                {
                    aiColor3D diffuseColor;
                    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor) == AI_SUCCESS)
                    {
                        float opacity = 1.0f;
                        mat->Get(AI_MATKEY_OPACITY, opacity);
                        pbrMat.pbrMetallicRoughness.baseColorFactor = {
                            std::clamp(diffuseColor.r, 0.0f, 1.0f),
                            std::clamp(diffuseColor.g, 0.0f, 1.0f),
                            std::clamp(diffuseColor.b, 0.0f, 1.0f),
                            std::clamp(opacity, 0.0f, 1.0f)
                        };
                    }
                }

                float metallicFactor = pbrMat.pbrMetallicRoughness.metallicFactor;
                if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor) == AI_SUCCESS)
                {
                    pbrMat.pbrMetallicRoughness.metallicFactor = std::clamp(metallicFactor, 0.0f, 1.0f);
                }

                float roughnessFactor = pbrMat.pbrMetallicRoughness.roughnessFactor;
                if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == AI_SUCCESS)
                {
                    pbrMat.pbrMetallicRoughness.roughnessFactor = std::clamp(roughnessFactor, 0.0f, 1.0f);
                }
                else
                {
                    float shininess = 0.0f;
                    if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess >= 0.0f)
                    {
                        const float convertedRoughness = std::sqrt(2.0f / (shininess + 2.0f));
                        pbrMat.pbrMetallicRoughness.roughnessFactor = std::clamp(convertedRoughness, 0.0f, 1.0f);
                    }
                }

                float emissiveStrength = 1.0f;
                if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveStrength) == AI_SUCCESS &&
                    (!std::isfinite(emissiveStrength) || emissiveStrength < 0.0f))
                {
                    HE_CORE_WARN(
                        "AssimpImporter: material '{0}' has invalid emissive strength {1}; using 1.0",
                        materialName,
                        emissiveStrength);
                    emissiveStrength = 1.0f;
                }

                aiColor3D emissiveColor;
                if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColor) == AI_SUCCESS)
                {
                    pbrMat.emissiveFactor = {
                        std::clamp(emissiveColor.r, 0.0f, 1.0f) * emissiveStrength,
                        std::clamp(emissiveColor.g, 0.0f, 1.0f) * emissiveStrength,
                        std::clamp(emissiveColor.b, 0.0f, 1.0f) * emissiveStrength
                    };
                }

                HE_CORE_INFO("AssimpImporter: material '{0}' factors base=({1:.3f},{2:.3f},{3:.3f},{4:.3f}) m={5:.3f} r={6:.3f} emissive=({7:.3f},{8:.3f},{9:.3f})",
                             materialName,
                             pbrMat.pbrMetallicRoughness.baseColorFactor.r,
                             pbrMat.pbrMetallicRoughness.baseColorFactor.g,
                             pbrMat.pbrMetallicRoughness.baseColorFactor.b,
                             pbrMat.pbrMetallicRoughness.baseColorFactor.a,
                             pbrMat.pbrMetallicRoughness.metallicFactor,
                             pbrMat.pbrMetallicRoughness.roughnessFactor,
                             pbrMat.emissiveFactor.r,
                             pbrMat.emissiveFactor.g,
                             pbrMat.emissiveFactor.b);
            }

			const MaterialRes matRes = BuildBasePbrMaterialResV2(pbrMat);

				std::string materialStem = pbrMat.name;
				if (materialStem.empty())
				{
					materialStem = stem;
					if (importedMaterialOrdinal != 0)
						materialStem += std::to_string(importedMaterialOrdinal);
				}
				++importedMaterialOrdinal;
				const std::string materialFileName =
					materialStem + "." + MaterialRes::GetExtName();
				auto matPath = AssetManager::GetAvailableFileName(materialFileName.c_str(), outDir);
				if (!AssetManager::SaveAsset(matRes, matPath))
				{
					bImportSucceeded = false;
					continue;
				}
				matResPathMap[mi] = ConfigManager::GetRelativePath(matPath);
			}

            // ---- Build one MeshRes containing all aiMesh primitives ----
            MeshRes meshRes;
            meshRes.name = stem;

            std::vector<uint8_t> meshBinData;
            constexpr int sharedBufferIndex = 0;

        for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            aiMesh* mesh = scene->mMeshes[mi];
            if (mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
                continue;

            bool skinned = mesh->mNumBones > 0 && hasSkinning;

            size_t numVerts  = mesh->mNumVertices;
            Primitive prim;

            // ---- POSITION (VEC3 FLOAT) ----
            {
                size_t off = meshBinData.size();
                for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
                {
                    float pos[3] = { mesh->mVertices[v].x,
                                     mesh->mVertices[v].y,
                                     mesh->mVertices[v].z };
                    AppendData(meshBinData, pos, 3);
                }
                size_t bytes = meshBinData.size() - off;
                int accIdx = AddAccessor(meshRes, sharedBufferIndex,
                                         off, bytes, HENGINE_COMPONENT_TYPE_FLOAT,
                                         "VEC3", numVerts);
                prim.attributes["POSITION"] = accIdx;
            }

            // ---- NORMAL (VEC3 FLOAT) ----
            if (mesh->HasNormals())
            {
                size_t off = meshBinData.size();
                for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
                {
                    float n[3] = { mesh->mNormals[v].x,
                                   mesh->mNormals[v].y,
                                   mesh->mNormals[v].z };
                    AppendData(meshBinData, n, 3);
                }
                size_t bytes = meshBinData.size() - off;
                int accIdx = AddAccessor(meshRes, sharedBufferIndex,
                                         off, bytes, HENGINE_COMPONENT_TYPE_FLOAT,
                                         "VEC3", numVerts);
                prim.attributes["NORMAL"] = accIdx;
            }

            // ---- TEXCOORD_0 (VEC2 FLOAT) ----
            {
                size_t off = meshBinData.size();
                for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
                {
                    float uv[2] = { 0.0f, 0.0f };
                    if (mesh->mTextureCoords[0])
                    {
                        uv[0] = mesh->mTextureCoords[0][v].x;
                        uv[1] = mesh->mTextureCoords[0][v].y;
                    }
                    AppendData(meshBinData, uv, 2);
                }
                size_t bytes = meshBinData.size() - off;
                int accIdx = AddAccessor(meshRes, sharedBufferIndex,
                                         off, bytes, HENGINE_COMPONENT_TYPE_FLOAT,
                                         "VEC2", numVerts);
                prim.attributes["TEXCOORD_0"] = accIdx;
            }

            // ---- TANGENT (VEC3 FLOAT) ----
            if (mesh->HasTangentsAndBitangents())
            {
                size_t off = meshBinData.size();
                for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
                {
                    float t[3] = { mesh->mTangents[v].x,
                                   mesh->mTangents[v].y,
                                   mesh->mTangents[v].z };
                    AppendData(meshBinData, t, 3);
                }
                size_t bytes = meshBinData.size() - off;
                int accIdx = AddAccessor(meshRes, sharedBufferIndex,
                                         off, bytes, HENGINE_COMPONENT_TYPE_FLOAT,
                                         "VEC3", numVerts);
                prim.attributes["TANGENT"] = accIdx;

                off = meshBinData.size();
                for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
                {
                    float b[3] = { mesh->mBitangents[v].x,
                                   mesh->mBitangents[v].y,
                                   mesh->mBitangents[v].z };
                    AppendData(meshBinData, b, 3);
                }
                bytes = meshBinData.size() - off;
                accIdx = AddAccessor(meshRes, sharedBufferIndex,
                                     off, bytes, HENGINE_COMPONENT_TYPE_FLOAT,
                                     "VEC3", numVerts);
                prim.attributes["BITANGENT"] = accIdx;
            }

            // ---- JOINTS_0 & WEIGHTS_0 (VEC4 FLOAT) ----
            if (skinned)
            {
                // Build per-vertex bone arrays
                std::vector<std::array<float, 4>> joints(numVerts, {-1.0f,-1.0f,-1.0f,-1.0f});
                std::vector<std::array<float, 4>> weights(numVerts, {0.0f,0.0f,0.0f,0.0f});

                for (uint32_t b = 0; b < mesh->mNumBones; ++b)
                {
                    aiBone* bone = mesh->mBones[b];
                    std::string boneName = bone->mName.C_Str();
                    int boneId = -1;
                    auto it = boneMap.find(boneName);
                    if (it != boneMap.end()) boneId = it->second.id;
                    if (boneId < 0) continue;

                    for (uint32_t wi = 0; wi < bone->mNumWeights; ++wi)
                    {
                        uint32_t vId    = bone->mWeights[wi].mVertexId;
                        float    weight = bone->mWeights[wi].mWeight;
                        for (int slot = 0; slot < 4; ++slot)
                        {
                            if (joints[vId][slot] < 0.0f)
                            {
                                joints[vId][slot]  = (float)boneId;
                                weights[vId][slot] = weight;
                                break;
                            }
                        }
                    }
                }
                // Normalize after limiting the influences. Some FBX exporters keep
                // overlapping vertex groups whose accumulated weight is greater
                // than one; feeding those values directly to linear blend skinning
                // transforms the affected vertices more than once.
                for (uint32_t vertex = 0; vertex < numVerts; ++vertex)
                {
                    float totalWeight = 0.0f;
                    for (float weight : weights[vertex])
                        totalWeight += std::max(0.0f, weight);

                    if (totalWeight > 1.0e-6f)
                    {
                        const float inverseTotalWeight = 1.0f / totalWeight;
                        for (float& weight : weights[vertex])
                            weight = std::max(0.0f, weight) * inverseTotalWeight;
                    }
                    else
                    {
                        joints[vertex][0] = 0.0f;
                        weights[vertex][0] = 1.0f;
                    }

                    for (float& joint : joints[vertex])
                    {
                        if (joint < 0.0f)
                            joint = 0.0f;
                    }
                }

                // JOINTS_0
                {
                    size_t off = meshBinData.size();
                    for (uint32_t v = 0; v < numVerts; ++v)
                        AppendData(meshBinData, joints[v].data(), 4);
                    size_t bytes = meshBinData.size() - off;
                    int accIdx = AddAccessor(meshRes, sharedBufferIndex,
                                             off, bytes, HENGINE_COMPONENT_TYPE_FLOAT,
                                             "VEC4", numVerts);
                    prim.attributes["JOINTS_0"] = accIdx;
                }
                // WEIGHTS_0
                {
                    size_t off = meshBinData.size();
                    for (uint32_t v = 0; v < numVerts; ++v)
                        AppendData(meshBinData, weights[v].data(), 4);
                    size_t bytes = meshBinData.size() - off;
                    int accIdx = AddAccessor(meshRes, sharedBufferIndex,
                                             off, bytes, HENGINE_COMPONENT_TYPE_FLOAT,
                                             "VEC4", numVerts);
                    prim.attributes["WEIGHTS_0"] = accIdx;
                }
            }

            // ---- INDEX (SCALAR UNSIGNED_INT) ----
            {
                size_t off = meshBinData.size();
                size_t numIndices = 0;
                for (uint32_t f = 0; f < mesh->mNumFaces; ++f)
                {
                    aiFace& face = mesh->mFaces[f];
                    for (uint32_t k = 0; k < face.mNumIndices; ++k)
                    {
                        uint32_t idx = face.mIndices[k];
                        AppendData(meshBinData, &idx, 1);
                        numIndices++;
                    }
                }
                size_t bytes = meshBinData.size() - off;
                int accIdx = AddAccessor(meshRes, sharedBufferIndex,
                                         off, bytes, HENGINE_COMPONENT_TYPE_UNSIGNED_INT,
                                         "SCALAR", numIndices);
                prim.indices = accIdx;
            }

            // Material
            prim.materialResPath = matResPathMap.count(mesh->mMaterialIndex) ?
                matResPathMap[mesh->mMaterialIndex] : "";
            prim.mode = 4; // TRIANGLES

            meshRes.primitives.push_back(prim);
        }

            // ---- Save one shared .bin file for all primitives ----
            const std::string meshExtension = "." + MeshRes::GetExtName();
            const std::filesystem::path meshStemPath = AssetManager::GetAvailableFileStem(
                stem,
                outDir,
                { ".bin", meshExtension });
            const std::string meshOutputStem = meshStemPath.filename().string();
            std::filesystem::path binPath = meshStemPath;
            binPath += ".bin";
            std::filesystem::path meshPath = meshStemPath;
            meshPath += meshExtension;
            createdMeshPair.SetPaths(binPath, meshPath);
            meshRes.name = meshOutputStem;

            int meshBufferByteLength = 0;
            try
            {
                constexpr std::size_t MaxCookedMeshBufferBytes = std::size_t{ 1 } << 28;
                if (meshBinData.empty() || meshBinData.size() > MaxCookedMeshBufferBytes ||
                    meshBinData.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    throw std::length_error("mesh binary exceeds the runtime resource limit");
                }
                meshBufferByteLength = static_cast<int>(meshBinData.size());
                Utils::WriteBytesToFileAtomically(
                    binPath,
                    meshBinData.data(),
                    meshBinData.size(),
                    Utils::AtomicWriteMode::CreateNew);
                createdMeshPair.MarkBinCreated();
            }
            catch (const std::exception& exception)
            {
                HE_CORE_ERROR(
                    "AssimpImporter: failed to save mesh binary output '{0}': {1}",
                    binPath.string(),
                    exception.what());
                bImportSucceeded = false;
            }

            MeshBuffer sharedBuf;
            sharedBuf.name       = meshOutputStem;
            sharedBuf.byteLength = meshBufferByteLength;
            sharedBuf.uri        = ConfigManager::GetRelativePath(binPath);
            meshRes.bufferVec.push_back(sharedBuf);

            if (!boneMap.empty())
            {
                SkeletonRes skeleton;
                BuildSkeletonResFromScene(scene, boneMap, stem + "_Skeleton", skeleton);

                auto skeletonOutPath = AssetManager::GetAvailableFileName((stem + "_Skeleton.hskel").c_str(), outDir);
                std::string skeletonSaveError;
                if (SaveSkeletonRes(skeleton, skeletonOutPath, &skeletonSaveError))
                    skeletonPathRelative = Utils::NormalizeAssetPath(ConfigManager::GetRelativePath(skeletonOutPath));
                else
                {
                    HE_CORE_ERROR(
                        "AssimpImporter: failed to save skeleton output '{0}': {1}",
                        skeletonOutPath.string(),
                        skeletonSaveError);
                    bImportSucceeded = false;
                }
            }

            meshRes.version = 1;
            meshRes.skeletonPath = skeletonPathRelative;
            if (createdMeshPair.HasBin() && AssetManager::SaveAsset(
                    meshRes,
                    meshPath,
                    Utils::AtomicWriteMode::CreateNew))
            {
                createdMeshPair.MarkMeshCreated();
                meshPathRelative = ConfigManager::GetRelativePath(meshPath);
                createdMeshPair.KeepFiles();
            }
            else
            {
                bImportSucceeded = false;
                if (!createdMeshPair.Cleanup())
                    HE_CORE_ERROR("AssimpImporter: failed to clean newly-created mesh pair for '{0}'", utf8Path);
            }
        }

        // ---- Build AnimRes for each animation ----
        if (scene->HasAnimations())
        {
            // Build boneInfos list
            std::vector<AnimBoneInfoRes> boneInfos;
            boneInfos.reserve(boneMap.size());
            for (const auto& [name, bi] : boneMap)
            {
                AnimBoneInfoRes bir;
                bir.name = name;
                bir.id   = bi.id;
                // Store offset matrix column-major
                bir.offsetMatrix.resize(16);
                const float* raw = glm::value_ptr(bi.offset);
                std::copy(raw, raw + 16, bir.offsetMatrix.data());
                boneInfos.push_back(bir);
            }

            for (uint32_t ai = 0; ai < scene->mNumAnimations; ++ai)
            {
                aiAnimation* anim = scene->mAnimations[ai];

                AnimClipRes clip;
                clip.name          = anim->mName.C_Str();
                clip.duration      = (float)anim->mDuration;
                clip.ticksPerSecond = (float)(anim->mTicksPerSecond > 0.0 ?
                                               anim->mTicksPerSecond : 24.0);

                for (uint32_t ch = 0; ch < anim->mNumChannels; ++ch)
                {
                    aiNodeAnim* chan = anim->mChannels[ch];
                    AnimBoneChannelRes chanRes;
                    chanRes.name = chan->mNodeName.C_Str();

                    for (uint32_t k = 0; k < chan->mNumPositionKeys; ++k)
                    {
                        AnimKeyPositionRes kp;
                        kp.px = chan->mPositionKeys[k].mValue.x;
                        kp.py = chan->mPositionKeys[k].mValue.y;
                        kp.pz = chan->mPositionKeys[k].mValue.z;
                        kp.timeStamp = (float)chan->mPositionKeys[k].mTime;
                        chanRes.positionKeys.push_back(kp);
                    }
                    for (uint32_t k = 0; k < chan->mNumRotationKeys; ++k)
                    {
                        AnimKeyRotationRes kr;
                        kr.qw = chan->mRotationKeys[k].mValue.w;
                        kr.qx = chan->mRotationKeys[k].mValue.x;
                        kr.qy = chan->mRotationKeys[k].mValue.y;
                        kr.qz = chan->mRotationKeys[k].mValue.z;
                        kr.timeStamp = (float)chan->mRotationKeys[k].mTime;
                        chanRes.rotationKeys.push_back(kr);
                    }
                    for (uint32_t k = 0; k < chan->mNumScalingKeys; ++k)
                    {
                        AnimKeyScaleRes ks;
                        ks.sx = chan->mScalingKeys[k].mValue.x;
                        ks.sy = chan->mScalingKeys[k].mValue.y;
                        ks.sz = chan->mScalingKeys[k].mValue.z;
                        ks.timeStamp = (float)chan->mScalingKeys[k].mTime;
                        chanRes.scaleKeys.push_back(ks);
                    }
                    clip.channels.push_back(chanRes);
                }

                AnimRes animRes;
                animRes.version = 1;
                animRes.name        = clip.name.empty() ? stem : clip.name;
                animRes.meshResPath = meshPathRelative;
                animRes.targetSkeletonPath = skeletonPathRelative;
                animRes.boneInfos   = boneInfos;
                BuildAnimNode(scene->mRootNode, animRes.rootNode);
                animRes.clips.push_back(clip);

                std::string animName = animRes.name.empty() ? stem : animRes.name;
                for (auto& c : animName) if (c == ' ' || c == '|') c = '_';
                auto animPath = AssetManager::GetAvailableFileName((animName + ".hanim").c_str(), outDir);
                if (!SaveAnimResJson(animRes, animPath))
                    bImportSucceeded = false;
            }
        }

        if (!bImportSucceeded)
        {
            HE_CORE_ERROR("AssimpImporter: one or more outputs failed while importing '{0}'", utf8Path);
            return false;
        }

        HE_CORE_INFO("AssimpImporter: imported '{0}' -> {1} mesh(es), {2} anim(s)",
                     utf8Path, scene->mNumMeshes, scene->mNumAnimations);
        return true;
    }
}
