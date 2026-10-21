#include "GltfSceneImporter.h"
#include "NativeMeshOutput.h"
#include "Runtime/Core/UUID.h"
#include "Runtime/EcsFramework/Component/ComponentGroup.h"
#include "Runtime/EcsFramework/Component/Light/SpotLightComponent.h"
#include "Runtime/Graphics/Mesh/MeshResourceValidator.h"
#include "Runtime/Resource/ResType/BasePbrMaterialBuilder.h"
#include "Runtime/Utils/MathUtils/MathUtils.h"
#include "Runtime/Platform/PhysicsMeshCooker.h"
#include "all_serializer.h"

#define CGLTF_IMPLEMENTATION
#include "ThirdParty/cgltf/cgltf.h"
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace HEngine
{
    namespace
    {
        void Require(bool condition, const std::string& message)
        {
            if (!condition) throw std::runtime_error(message);
        }
        template<class T> Json Native(const T& value)
        {
            Json result; Serializer::Serialize(result, value); return result;
        }
        void Write(const std::filesystem::path& path, const void* data, size_t size)
        {
            std::ofstream stream(path, std::ios::binary);
            stream.write(static_cast<const char*>(data), size);
            stream.close();
            Require(bool(stream), "output_write: Cannot write staged resource.");
        }
        void WriteJson(const std::filesystem::path& path, const Json& value)
        {
            const auto bytes = value.dump(2); Write(path, bytes.data(), bytes.size());
        }
        Json Extras(const cgltf_extras& extras, const cgltf_data* data = nullptr)
        {
            // cgltf 1.15 stores mesh extras as offsets (targetNames handling).
            if (!extras.data && data && extras.end_offset > extras.start_offset)
                return Json::parse(data->json + extras.start_offset, data->json + extras.end_offset);
            return extras.data ? Json::parse(extras.data) : Json::object();
        }
        std::string Id(const cgltf_extras& extras, const cgltf_data* data = nullptr)
        {
            const auto metadata = Extras(extras, data);
            Require(metadata.value("he_schema", 0) == 1, "identity_schema: Set he_schema=1 and persistent he_id UUID extras.");
            const auto id = metadata.value("he_id", std::string());
            Require(UUID(id).isValid(), "identity_missing: Persist a he_id UUID in the authoring document before import.");
            return id;
        }
        std::filesystem::path Dependency(const std::filesystem::path& source, const char* uri)
        {
            std::string decoded = uri ? uri : "";
            Require(decoded.find(':') == std::string::npos && decoded.find('\\') == std::string::npos,
                "dependency_uri: Use a relative local URI inside the source directory.");
            cgltf_decode_uri(decoded.data()); decoded.resize(std::strlen(decoded.c_str()));
            const auto root = std::filesystem::canonical(source.parent_path());
            const auto path = std::filesystem::weakly_canonical(root / std::filesystem::u8path(decoded));
            const auto relative = path.lexically_relative(root);
            Require(!relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
                "dependency_escape: Dependency escapes source directory.");
            Require(std::filesystem::is_regular_file(path), "dependency_missing: " + path.string());
            return path;
        }
        TransformComponent Transform(const glm::mat4& matrix)
        {
            TransformComponent value;
            Require(Math::DecomposeTransform(matrix, value.Translation, value.Rotation, value.Scale),
                "transform_singular: Apply or repair the source transform.");
            auto rebuilt = value.GetTransform();
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
                Require(std::isfinite(matrix[c][r]) && std::abs(matrix[c][r] - rebuilt[c][r]) < 1e-4f * std::max(1.f, std::abs(matrix[c][r])),
                    "transform_shear: Transform cannot be represented by engine TRS; apply shear in Blender.");
            Require(glm::all(glm::greaterThan(glm::abs(value.Scale), glm::vec3(1e-6f))), "transform_zero_scale: Repair zero scale.");
            return value;
        }
        std::string CookMesh(const MeshRes& mesh, const std::vector<uint8_t>& bytes)
        {
            std::vector<std::array<float, 3>> triangles;
            for (const auto& primitive : mesh.primitives)
            {
                const auto& positions = mesh.accessorVec.at(primitive.attributes.at("POSITION"));
                const auto& indices = mesh.accessorVec.at(primitive.indices);
                const auto positionOffset = mesh.bufferViewVec.at(positions.bufferViewIndex).byteOffset;
                const auto indexOffset = mesh.bufferViewVec.at(indices.bufferViewIndex).byteOffset;
                for (size_t i = 0; i < indices.count; i += 3)
                {
                    std::array<float, 3> vertices[3];
                    for (size_t v = 0; v < 3; ++v)
                    {
                        uint32_t index; std::memcpy(&index, bytes.data()+indexOffset+(i+v)*4, 4);
                        std::memcpy(&vertices[v], bytes.data()+positionOffset+size_t(index)*12, 12);
                    }
                    triangles.insert(triangles.end(), std::begin(vertices), std::end(vertices));
                }
            }
#if HE_ENABLE_PHYSICS_3D
            return CookTriangleCollision(triangles);
#else
            RequireBuildFeature(BuildFeature::Physics3D, "Import collision mesh");
            return {};
#endif
        }
    }

    Json GltfSceneImporter::Convert(const std::filesystem::path& source, const std::filesystem::path& staging)
    {
        Json result = {{"schema", 1}, {"tool", "cgltf-scene/1"}, {"ok", false},
            {"diagnostics", Json::array()}, {"nodes", Json::object()}, {"dependencies", Json::array()}};
        std::string objectId, objectName, property = "document";
        try
        {
            cgltf_options options{};
            cgltf_data* raw = nullptr;
            // Read through filesystem::path for Unicode Windows filenames.
            std::ifstream input(source, std::ios::binary | std::ios::ate);
            Require(bool(input) && input.tellg() > 0 && input.tellg() <= 512ll * 1024 * 1024, "source_size: Invalid or oversized source (512 MiB limit).");
            std::vector<char> file(static_cast<size_t>(input.tellg())); input.seekg(0); input.read(file.data(), file.size());
            Require(cgltf_parse(&options, file.data(), file.size(), &raw) == cgltf_result_success, "gltf_parse: Invalid glTF/GLB.");
            std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
            for (size_t i = 0; i < data->extensions_used_count; ++i)
            {
                const std::string extension = data->extensions_used[i];
                Require(extension == "KHR_lights_punctual" || extension == "KHR_materials_emissive_strength" || extension == "KHR_texture_transform",
                    "extension_unsupported: Remove/bake " + extension + ". No Assimp fallback.");
            }
            for (size_t i = 0; i < data->extensions_required_count; ++i)
            {
                const std::string extension = data->extensions_required[i];
                Require(extension == "KHR_lights_punctual" || extension == "KHR_materials_emissive_strength" || extension == "KHR_texture_transform",
                    "required_extension_unsupported: Cannot import " + extension + ".");
            }
            Require(data->skins_count == 0 && data->animations_count == 0, "animation_unsupported: Scene mode accepts static content; use legacy baked-model import for animation.");
            Require(data->scene != nullptr, "scene_missing: Select an explicit default scene.");
            result["documentId"] = Id(data->scene->extras);
            // Load external buffers ourselves for path confinement and Unicode support.
            std::vector<std::vector<char>> external(data->buffers_count);
            for (size_t i = 0; i < data->buffers_count; ++i)
            {
                auto& buffer = data->buffers[i];
                if (!buffer.uri || std::string(buffer.uri).starts_with("data:")) continue;
                const auto path = Dependency(source, buffer.uri);
                Require(std::filesystem::file_size(path) == buffer.size && buffer.size <= 512u * 1024u * 1024u, "buffer_size: External buffer size mismatch or limit exceeded.");
                external[i].resize(buffer.size);
                std::ifstream stream(path, std::ios::binary); stream.read(external[i].data(), external[i].size());
                Require(bool(stream), "buffer_read: Cannot read external buffer.");
                buffer.data = external[i].data(); buffer.data_free_method = cgltf_data_free_method_none;
                result["dependencies"].push_back(path.generic_string());
            }
            Require(cgltf_load_buffers(&options, data.get(), nullptr) == cgltf_result_success, "buffer_load: Invalid embedded buffer.");
            Require(cgltf_validate(data.get()) == cgltf_result_success, "gltf_validate: Invalid accessor, index or node graph.");
            for (size_t i = 0; i < data->nodes_count; ++i)
            {
                size_t depth = 0;
                for (auto* parent = data->nodes[i].parent; parent; parent = parent->parent)
                    Require(++depth <= 256, "hierarchy_depth: Flatten hierarchy deeper than 256 levels.");
            }
            std::filesystem::create_directories(staging);
            std::unordered_map<const cgltf_image*, std::string> images;
            auto texture = [&](const cgltf_texture_view& view) -> std::string
            {
                if (!view.texture) return {};
                auto* image = view.texture->image;
                Require(image != nullptr, "texture_image: Missing standard PNG/JPEG image.");
                if (images.contains(image)) return images.at(image);
                std::string ext;
                std::vector<char> bytes;
                if (image->buffer_view)
                {
                    ext = image->mime_type && std::string(image->mime_type) == "image/png" ? ".png" :
                        image->mime_type && std::string(image->mime_type) == "image/jpeg" ? ".jpg" : "";
                    const auto* ptr = cgltf_buffer_view_data(image->buffer_view);
                    Require(ptr != nullptr, "texture_buffer: Missing image buffer.");
                    bytes.assign(ptr, ptr + image->buffer_view->size);
                }
                else
                {
                    Require(image->uri && !std::string(image->uri).starts_with("data:"), "texture_uri: Use external PNG/JPEG or GLB bufferView images; data URI images are unsupported.");
                    const auto path = Dependency(source, image->uri); ext = path.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {return char(std::tolower(c));});
                    Require(std::filesystem::file_size(path) <= 128u*1024u*1024u, "texture_size: Image exceeds 128 MiB.");
                    std::ifstream stream(path, std::ios::binary);
                    bytes.assign(std::istreambuf_iterator<char>(stream), {});
                    result["dependencies"].push_back(path.generic_string());
                }
                Require(ext == ".png" || ext == ".jpg" || ext == ".jpeg", "texture_format: Only PNG/JPEG are supported in scene v1.");
                const auto name = "image-" + std::to_string(images.size()) + ext;
                Write(staging / name, bytes.data(), bytes.size());
                return images[image] = "__content__/" + name;
            };
            PbrMaterialRes defaultPbr; defaultPbr.pbrMetallicRoughness.metallicFactor = 0;
            WriteJson(staging / "default.hmat", Native(BuildBasePbrMaterialResV2(defaultPbr)));
            std::unordered_map<const cgltf_material*, std::string> materials;
            std::set<std::string> materialIds;
            for (size_t i = 0; i < data->materials_count; ++i)
            {
                auto& sourceMat = data->materials[i]; property = "material"; objectName = sourceMat.name ? sourceMat.name : "";
                objectId = Id(sourceMat.extras);
                Require(materialIds.insert(objectId).second, "identity_duplicate: Material IDs must be unique.");
                const auto& pbr = sourceMat.pbr_metallic_roughness;
                PbrMaterialRes mat;
                mat.pbrMetallicRoughness.baseColorFactor = glm::make_vec4(pbr.base_color_factor);
                mat.pbrMetallicRoughness.metallicFactor = pbr.metallic_factor;
                mat.pbrMetallicRoughness.roughnessFactor = pbr.roughness_factor;
                for (float factor : pbr.base_color_factor)
                    Require(std::isfinite(factor) && factor >= 0 && factor <= 1, "material_factor: Base color factor must be finite in [0,1].");
                Require(std::isfinite(pbr.metallic_factor) && pbr.metallic_factor >= 0 && pbr.metallic_factor <= 1 &&
                    std::isfinite(pbr.roughness_factor) && pbr.roughness_factor >= 0 && pbr.roughness_factor <= 1,
                    "material_factor: Metallic/roughness factors must be finite in [0,1].");
                mat.emissiveFactor = glm::make_vec3(sourceMat.emissive_factor) * (sourceMat.has_emissive_strength ? sourceMat.emissive_strength.emissive_strength : 1.f);
                mat.alphaMode = sourceMat.alpha_mode == cgltf_alpha_mode_mask ? "MASK" : sourceMat.alpha_mode == cgltf_alpha_mode_blend ? "BLEND" : "OPAQUE";
                mat.alphaCutoff = sourceMat.alpha_cutoff; mat.doubleSided = sourceMat.double_sided;
                auto bind = [&](auto& target, bool& enabled, const auto& view) {
                    target.textureRes.textureSourcePath = texture(view); enabled = !target.textureRes.textureSourcePath.empty();
                    if (!enabled) return;
                    auto& sampling = target.sampling;
                    sampling.texCoord = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
                    target.texCoord = sampling.texCoord;
                    if (view.has_transform) {
                        sampling.offset = glm::make_vec2(view.transform.offset);
                        sampling.scale = glm::make_vec2(view.transform.scale);
                        sampling.rotation = view.transform.rotation;
                    }
                    if (view.texture->sampler) {
                        const auto& source = *view.texture->sampler;
                        sampling.wrapS = source.wrap_s; sampling.wrapT = source.wrap_t;
                        sampling.minFilter = source.min_filter ? source.min_filter : 9987;
                        sampling.magFilter = source.mag_filter ? source.mag_filter : 9729;
                    }
                    (void)PackTextureSampling(sampling);
                };
                bind(mat.pbrMetallicRoughness.baseColorTexture, mat.bUseAlbedoMap, pbr.base_color_texture);
                bind(mat.pbrMetallicRoughness.metallicRoughnessTexture, mat.bUseMetallicRoughnessMap, pbr.metallic_roughness_texture);
                bind(mat.normalTexture, mat.bUseNormalMap, sourceMat.normal_texture);
                bind(mat.occlusionTexture, mat.bUseAoMap, sourceMat.occlusion_texture);
                bind(mat.emissiveTexture, mat.bUseEmissiveMap, sourceMat.emissive_texture);
                mat.normalTexture.scale = sourceMat.normal_texture.scale;
                mat.occlusionTexture.strength = sourceMat.occlusion_texture.scale;
                Require(std::isfinite(mat.normalTexture.scale) && mat.normalTexture.scale >= 0 &&
                    std::isfinite(mat.occlusionTexture.strength) && mat.occlusionTexture.strength >= 0 && mat.occlusionTexture.strength <= 1,
                    "material_factor: Invalid normal strength or AO strength.");
                auto name = "material-" + objectId + ".hmat";
                WriteJson(staging / name, Native(BuildBasePbrMaterialResV2(mat)));
                materials[&sourceMat] = "__content__/" + name;
                result["materials"][objectId] = materials[&sourceMat];
            }
            std::unordered_map<const cgltf_mesh*, std::string> meshes;
            std::unordered_map<const cgltf_mesh*, std::string> mirroredMeshes;
            std::set<const cgltf_mesh*> needsMirroredWinding;
            for (size_t i = 0; i < data->nodes_count; ++i)
            {
                glm::mat4 world; cgltf_node_transform_world(&data->nodes[i], glm::value_ptr(world));
                if (data->nodes[i].mesh && glm::determinant(world) < 0)
                    needsMirroredWinding.insert(data->nodes[i].mesh);
            }
            std::unordered_map<const cgltf_mesh*, std::string> collisions;
            std::set<const cgltf_mesh*> collisionMeshes;
            for (size_t i = 0; i < data->nodes_count; ++i)
                if (Extras(data->nodes[i].extras).value("he_collision", std::string()) == "mesh")
                    collisionMeshes.insert(data->nodes[i].mesh);
            std::unordered_map<const cgltf_mesh*, Json> slots, bindings;
            for (size_t i = 0; i < data->meshes_count; ++i)
            {
                const auto& sourceMesh = data->meshes[i]; property = "mesh"; objectName = sourceMesh.name ? sourceMesh.name : "";
                objectId = Id(sourceMesh.extras, data.get());
                MeshRes mesh; mesh.name = "ImportedGeometry";
                std::vector<uint8_t> bytes;
                auto append = [&](const void* ptr, size_t size, const std::string& type, size_t count, int component) {
                    const size_t offset = bytes.size(); const auto* first = static_cast<const uint8_t*>(ptr);
                    bytes.insert(bytes.end(), first, first + size);
                    return AddAccessor(mesh, 0, offset, size, component, type, count);
                };
                Json keys = Json::array(), paths = Json::array(); std::set<std::string> uniqueSlots;
                for (size_t j = 0; j < sourceMesh.primitives_count; ++j)
                {
                    const auto& prim = sourceMesh.primitives[j];
                    Require(prim.type == cgltf_primitive_type_triangles && !prim.has_draco_mesh_compression && !prim.targets_count,
                        "primitive_unsupported: Use uncompressed static triangles without morph targets.");
                    const auto key = objectId + "/" + (prim.material ? Id(prim.material->extras) : "default");
                    Require(uniqueSlots.insert(key).second, "slot_identity: Multiple primitives with the same material require consolidation in the source mesh.");
                    keys.push_back(key); paths.push_back(prim.material ? materials.at(prim.material) : "__content__/default.hmat");
                    // MeshLoader assigns slots by unique native material path. Neutral
                    // slot placeholders keep geometry independent of authored materials.
                    auto placeholder = BuildBasePbrMaterialResV2(defaultPbr);
                    placeholder.OrphanedParameters["he_import_slot"] = j;
                    const auto slotName = "slot-" + std::to_string(j) + ".hmat";
                    WriteJson(staging / slotName, Native(placeholder));
                    Primitive native; native.materialResPath = "__content__/" + slotName;
                    size_t vertices = 0;
                    for (size_t a = 0; a < prim.attributes_count; ++a)
                    {
                        const auto& attribute = prim.attributes[a]; const auto* accessor = attribute.data;
                        const std::string name = attribute.name;
                        Require(name == "POSITION" || name == "NORMAL" || name == "TANGENT" || name == "TEXCOORD_0" || name == "TEXCOORD_1" || name == "COLOR_0",
                            "attribute_unsupported: Bake/remove attribute " + name + ".");
                        const size_t components = cgltf_num_components(accessor->type);
                        Require(accessor->count <= 10000000 && components >= 2 && components <= 4, "accessor_limit: Invalid vertex accessor.");
                        const size_t expected = name == "POSITION" || name == "NORMAL" ? 3 : name == "TANGENT" ? 4 : 2;
                        Require(name == "COLOR_0" ? components == 3 || components == 4 : components == expected,
                            "attribute_shape: Invalid component count for " + name + ".");
                        std::vector<float> values(accessor->count * components);
                        Require(cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) == values.size(), "accessor_decode: Cannot decode attribute.");
                        for (float value : values) Require(std::isfinite(value), "attribute_nonfinite: Vertex attributes must be finite.");
                        native.attributes[name] = append(values.data(), values.size()*sizeof(float), "VEC"+std::to_string(components), accessor->count, 5126);
                        if (name == "POSITION") vertices = accessor->count;
                    }
                    Require(vertices && native.attributes.contains("NORMAL"), "mesh_normals: Export positions and normals.");
                    for (size_t a = 0; a < prim.attributes_count; ++a)
                        Require(prim.attributes[a].data->count == vertices, "attribute_count: Vertex attribute counts must match POSITION.");
                    if (prim.material && prim.material->normal_texture.texture)
                        Require(native.attributes.contains("TANGENT") && native.attributes.contains("TEXCOORD_0"), "mesh_tangents: Normal maps require exported UV0 and tangents.");
                    if (prim.material) {
                        const auto checkUv = [&](const cgltf_texture_view& view) {
                            if (!view.texture) return;
                            const int uv = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
                            Require(native.attributes.contains(uv == 1 ? "TEXCOORD_1" : "TEXCOORD_0"), "texture_uv_missing: Material selects a missing UV channel.");
                        };
                        checkUv(prim.material->pbr_metallic_roughness.base_color_texture);
                        checkUv(prim.material->pbr_metallic_roughness.metallic_roughness_texture);
                        checkUv(prim.material->normal_texture); checkUv(prim.material->occlusion_texture); checkUv(prim.material->emissive_texture);
                    }
                    const size_t count = prim.indices ? prim.indices->count : vertices;
                    Require(count <= 30000000 && count % 3 == 0, "mesh_indices: Triangle index count invalid.");
                    std::vector<uint32_t> indices(count);
                    std::vector<float> sparseIndices;
                    if (prim.indices)
                    {
                        Require(prim.indices->type == cgltf_type_scalar && !prim.indices->normalized &&
                            (prim.indices->component_type == cgltf_component_type_r_8u || prim.indices->component_type == cgltf_component_type_r_16u || prim.indices->component_type == cgltf_component_type_r_32u),
                            "index_type: Triangle indices must be unnormalized unsigned integer scalars.");
                        if (prim.indices->is_sparse)
                        {
                            // cgltf_accessor_read_index silently returns zero for sparse data.
                            // Valid indices are <10M (vertex limit), exactly representable as floats.
                            sparseIndices.resize(count);
                            Require(cgltf_accessor_unpack_floats(prim.indices, sparseIndices.data(), count) == count,
                                "index_sparse: Cannot decode sparse triangle indices.");
                        }
                    }
                    for (size_t n = 0; n < count; ++n)
                    {
                        const size_t index = !sparseIndices.empty() ? static_cast<size_t>(sparseIndices[n]) : prim.indices ? cgltf_accessor_read_index(prim.indices, n) : n;
                        Require(index < vertices, "mesh_index_range: Index exceeds vertex count."); indices[n] = static_cast<uint32_t>(index);
                    }
                    native.indices = append(indices.data(), indices.size()*sizeof(uint32_t), "SCALAR", count, 5125);
                    mesh.primitives.push_back(std::move(native));
                }
                // Temporary ordinal filenames never serve as persistent identity. Publication
                // hashes the native dependency graph, deduplicating evaluated equal geometry.
                const auto name = "mesh-" + std::to_string(i);
                MeshBuffer buffer; buffer.byteLength = static_cast<int>(bytes.size()); buffer.data = bytes; buffer.uri = "__content__/"+name+".bin";
                mesh.bufferVec.push_back(std::move(buffer));
                const auto validation = MeshResourceValidator::ValidateLoaded(mesh);
                Require(bool(validation), "native_mesh: " + validation.Message);
                if (collisionMeshes.contains(&sourceMesh))
                {
                    const auto cooked = CookMesh(mesh, bytes);
                    Write(staging / (name+".hcollision"), cooked.data(), cooked.size());
                    collisions[&sourceMesh] = "__content__/"+name+".hcollision";
                }
                mesh.bufferVec[0].data.clear();
                Write(staging / (name+".bin"), bytes.data(), bytes.size());
                WriteJson(staging / (name+".hmesh"), Native(mesh));
                meshes[&sourceMesh] = "__content__/"+name+".hmesh"; slots[&sourceMesh] = keys; bindings[&sourceMesh] = paths;
                if (needsMirroredWinding.contains(&sourceMesh))
                {
                    // Renderer uses fixed CCW culling. Keep source TRS/hierarchy and
                    // share one reversed-index variant among mirrored instances.
                    for (const auto& primitive : mesh.primitives)
                    {
                        const auto& accessor = mesh.accessorVec.at(primitive.indices);
                        const auto offset = mesh.bufferViewVec.at(accessor.bufferViewIndex).byteOffset;
                        for (size_t index = 0; index < accessor.count; index += 3)
                            for (size_t b = 0; b < 4; ++b)
                                std::swap(bytes[offset+(index+1)*4+b], bytes[offset+(index+2)*4+b]);
                    }
                    mesh.bufferVec[0].uri = "__content__/"+name+"-mirror.bin";
                    Write(staging / (name+"-mirror.bin"), bytes.data(), bytes.size());
                    WriteJson(staging / (name+"-mirror.hmesh"), Native(mesh));
                    mirroredMeshes[&sourceMesh] = "__content__/"+name+"-mirror.hmesh";
                }
            }
            std::set<std::string> nodeIds;
            bool primary = false;
            std::function<void(const cgltf_node*)> visit = [&](const cgltf_node* node)
            {
                property = "node"; objectName = node->name ? node->name : ""; objectId = Id(node->extras);
                Require(nodeIds.insert(objectId).second, "identity_duplicate: Node IDs must be unique; repair copied custom properties in Blender.");
                Require(!node->has_mesh_gpu_instancing && !node->skin, "node_unsupported: Realize instances and export static meshes.");
                glm::mat4 matrix; cgltf_node_transform_local(node, glm::value_ptr(matrix));
                auto transform = Transform(matrix);
                if (node->parent) transform.ParentUuid = Id(node->parent->extras);
                Json entity; TagComponent tag; tag.Tag = objectName; entity["TagComponent"] = Native(tag);
                entity["TransformComponent"] = Native(transform);
                Json entry = {{"id", objectId}, {"name", objectName}, {"slots", Json::array()}};
                glm::mat4 sourceWorld; cgltf_node_transform_world(node, glm::value_ptr(sourceWorld));
                entry["worldMatrix"] = Json::array();
                for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) entry["worldMatrix"].push_back(sourceWorld[c][r]);
                const auto metadata = Extras(node->extras);
                if (metadata.value("he_character", false))
                {
                    Require(!node->parent && metadata.value("he_collision", std::string("none")) == "none",
                        "character_root: Character marker requires an unparented object without rigid collision.");
                    CharacterMovementComponent movement; movement.MaxSpeed = 2.0f;
                    CapsuleCollider3DComponent capsule;
                    entity["CharacterMovementComponent"] = Native(movement);
                    entity["CapsuleCollider3DComponent"] = Native(capsule);
                }
                if (node->mesh && !metadata.value("he_proxy", false))
                {
                    glm::mat4 world; cgltf_node_transform_world(node, glm::value_ptr(world));
                    MeshComponent mesh; mesh.Path = glm::determinant(world) < 0 ? mirroredMeshes.at(node->mesh) : meshes.at(node->mesh); mesh.meshResPath = mesh.Path;
                    mesh.MaterialOverrides = bindings.at(node->mesh).get<std::vector<std::string>>();
                    entity["MeshComponent"] = Native(mesh); entry["slots"] = slots.at(node->mesh); entry["meshId"] = Id(node->mesh->extras, data.get());
                }
                if (node->camera)
                {
                    CameraComponent camera; camera.Primary = metadata.value("he_primary", false);
                    camera.bOverrideExposure = metadata.contains("he_exposure_ev100");
                    camera.ExposureEV100 = metadata.value("he_exposure_ev100", 14.f);
                    Require(!camera.Primary || !primary, "camera_primary: Select only one primary camera."); primary |= camera.Primary;
                    camera.FixedAspectRatio = true;
                    if (node->camera->type == cgltf_camera_type_perspective)
                    {
                        const auto& p = node->camera->data.perspective;
                        Require(p.has_zfar, "camera_infinite: Engine requires a finite far clip.");
                        camera.Camera.SetPerspective(p.yfov, p.znear, p.zfar);
                        camera.Camera.mAspectRatio = p.has_aspect_ratio ? p.aspect_ratio : 16.f/9.f;
                    }
                    else
                    {
                        const auto& p = node->camera->data.orthographic;
                        camera.Camera.SetOrthographic(p.ymag*2.f, p.znear, p.zfar); camera.Camera.mAspectRatio = p.xmag/p.ymag;
                    }
                    entity["CameraComponent"] = Native(camera);
                }
                if (node->light)
                {
                    auto& light = *node->light;
                    if (light.type == cgltf_light_type_directional)
                    {
                        DirectionalLightComponent component; component.Intensity = light.intensity; component.Color = glm::make_vec3(light.color);
                        component.LocalDirection = { 0.0f, 0.0f, -1.0f };
                        entity["DirectionalLightComponent"] = Native(component);
                    }
                    else if (light.type == cgltf_light_type_point)
                    {
                        PointLightComponent component; component.Intensity = light.intensity; component.IntensityUnits = 1; component.LightColor = glm::make_vec3(light.color);
                        Require(light.range > 0, "light_range: Set a finite point light range in authoring metadata/export."); component.Radius = light.range;
                        entity["PointLightComponent"] = Native(component);
                    }
                    else if (light.type == cgltf_light_type_spot)
                    {
                        Require(light.range > 0, "light_range: Set a finite spot light range in authoring metadata/export.");
                        Require(light.spot_inner_cone_angle >= 0 && light.spot_inner_cone_angle < light.spot_outer_cone_angle &&
                                light.spot_outer_cone_angle <= glm::half_pi<float>(),
                            "light_cone: Spot cone angles must be ordered half-angles below 90 degrees.");
                        SpotLightComponent component;
                        component.Intensity = light.intensity;
                        component.IntensityUnits = 1;
                        component.LightColor = glm::make_vec3(light.color);
                        component.Radius = light.range;
                        component.InnerConeAngle = glm::degrees(light.spot_inner_cone_angle);
                        component.OuterConeAngle = glm::degrees(light.spot_outer_cone_angle);
                        entity["SpotLightComponent"] = Native(component);
                    }
                    else Require(false, "light_type: Unsupported glTF light type.");
                }
                const auto collision = metadata.value("he_collision", std::string("none"));
                if (collision != "none")
                {
                    RequireBuildFeature(BuildFeature::Physics3D, "Import rigid collision");
                    Require(collision == "box" || collision == "sphere" || collision == "mesh", "collision_shape: Use box/sphere proxies or a static triangle mesh.");
                    Require(collision == "mesh" || metadata.value("he_proxy", false), "collision_proxy: Use a separate explicit proxy to avoid runtime render-mesh bounds construction.");
                    glm::mat4 world; cgltf_node_transform_world(node, glm::value_ptr(world)); auto worldTrs = Transform(world);
                    Require(glm::determinant(world) > 0, "collision_mirror: Apply mirrored proxy transforms.");
                    if (collision == "sphere") Require(std::abs(worldTrs.Scale.x-worldTrs.Scale.y)<1e-5f && std::abs(worldTrs.Scale.x-worldTrs.Scale.z)<1e-5f,
                        "collision_sphere_scale: Sphere requires uniform world scale.");
                    Rigidbody3DComponent body; body.Shape = collision == "box" ? CollisionShape::Box : CollisionShape::Sphere;
                    const auto motion = metadata.value("he_motion", std::string("static"));
                    Require(motion == "static" || motion == "dynamic", "collision_motion: Use static or dynamic.");
                    Require(motion != "dynamic" || !node->parent, "collision_parent: Dynamic proxies must be roots.");
                    if (collision == "mesh")
                    {
                        Require(motion == "static" && node->mesh && collisions.contains(node->mesh), "collision_static: Triangle collision requires a static mesh object.");
                        body.Shape = CollisionShape::TriangleMesh;
                        body.CollisionAssetPath = collisions.at(node->mesh);
                    }
                    body.Type = motion == "dynamic" ? Rigidbody3DComponent::Body3DType::Dynamic : Rigidbody3DComponent::Body3DType::Static;
                    body.restitution = 0; body.EnableCCD = motion == "dynamic";
                    entity["Rigidbody3DComponent"] = Native(body);
                }
                entry["entity"] = entity; result["nodes"][objectId] = entry;
                for (size_t c = 0; c < node->children_count; ++c) visit(node->children[c]);
            };
            Require(data->nodes_count <= 65000, "scene_limit: Scene exceeds 65000 nodes.");
            for (size_t n = 0; n < data->scene->nodes_count; ++n) visit(data->scene->nodes[n]);
            if (!primary) result["diagnostics"].push_back({{"severity","warning"},{"code","camera_primary_missing"},{"message","Set he_primary on a camera before visual verification."}});
            result["ok"] = true;
        }
        catch (const std::exception& error)
        {
            const std::string message = error.what();
            result["diagnostics"].push_back({{"severity","error"},{"code",message.substr(0,message.find(':'))},
                {"objectId",objectId},{"name",objectName},{"property",property},{"message",message}});
        }
        return result;
    }
}
