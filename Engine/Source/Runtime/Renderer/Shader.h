#pragma once

#include <string>
#include <unordered_map>
#include <filesystem>

#include <glm/glm.hpp>

namespace HEngine
{
	// from https://github.dev/dtrajko/MoravaEngine/tree/bca0095d04d18f4ec8f04c670a74aaa1339f000a/MoravaEngine/src/Hazel/Renderer
	enum class ShaderUniformType
	{
		None = 0, Bool, Int, UInt, Float, Vec2, Vec3, Vec4, Mat3, Mat4
	};

	class ShaderUniform
	{
	public:
		ShaderUniform() = default;
		ShaderUniform(const std::string& name, ShaderUniformType type, uint32_t size, uint32_t offset);

		const std::string& GetName() const { return mName; }
		ShaderUniformType GetType() const { return mType; }
		uint32_t GetSize() const { return mSize; }
		uint32_t GetOffset() const { return mOffset; }

		static const std::string& UniformTypeToString(ShaderUniformType type);

	private:
		std::string mName;
		ShaderUniformType mType = ShaderUniformType::None;
		uint32_t mSize = 0;
		uint32_t mOffset = 0;
	};

	struct ShaderUniformBuffer
	{
		std::string Name;
		uint32_t Index;
		uint32_t BindingPoint;
		uint32_t Size;
		uint32_t RendererID;
		std::vector<ShaderUniform> Uniforms;
	};

	struct ShaderBuffer
	{
		std::string Name;
		uint32_t Size = 0;
		std::unordered_map<std::string, ShaderUniform> Uniforms;
	};

    class Shader
    {
    public:
        virtual ~Shader() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void SetBool(const std::string& name, bool value) = 0;
        virtual void SetInt(const std::string& name, int value) = 0;
        virtual void SetIntArray(const std::string& name, int* values, uint32_t count) = 0;
        virtual void SetFloat(const std::string& name, float value) = 0;
        virtual void SetFloat3(const std::string& name, const glm::vec3& value) = 0;
        virtual void SetFloat4(const std::string& name, const glm::vec4& value) = 0;
        virtual void SetMat4(const std::string& name, const glm::mat4& value) = 0;

		virtual void SetUniform(const std::string& fullname, const glm::mat4& value) {};

        [[nodiscard]] virtual const std::string& GetName() const = 0;

        static Ref<Shader> Create(const std::filesystem::path& filepath);
        static Ref<Shader> Create(const std::string& filepath);
        static Ref<Shader> Create(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);
		
		// Use native shader language instead of spir-v
		static Ref<Shader> CreateNative(const std::filesystem::path& filepath);
        static Ref<Shader> CreateNative(const std::string& filepath);
        static Ref<Shader> CreateNative(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);
    };
}