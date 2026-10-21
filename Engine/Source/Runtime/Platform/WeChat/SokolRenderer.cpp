#include "Runtime/Graphics/Renderer/LightRenderer.h"
#include "WeChatShader.h"

#include <emscripten.h>
#include <GLES3/gl3.h>

namespace HEngine { namespace { void TraceWebGLError(const char* operation, int line); } }
static void HE_GetSokolInteger(GLenum name, GLint* value);
// This context is exclusively owned by HE/Emscripten. Android WeChat rejects
// Sokol's CURRENT_PROGRAM query; use Emscripten's actual binding bookkeeping.
#define glGetIntegerv HE_GetSokolInteger
// The pinned Sokol header permits overriding its GL error checkpoints. Keep
// these active for initialization/first frame only, and retain the first error
// until the HE boundary can report it without unwinding through a Sokol call.
#define _SG_GL_CHECK_ERROR() HEngine::TraceWebGLError(__func__, __LINE__)
// Do not enable SOKOL_DEBUG here: its setup drains GL errors with an unbounded
// loop. A host returning a persistent error must reach HE's failure boundary.
#define SOKOL_IMPL
#define SOKOL_GLES3
#include <sokol_gfx.h>
#undef glGetIntegerv
#undef _SG_GL_CHECK_ERROR

#include <array>
#include <stdexcept>

EM_JS(int, HE_MakeWebGLCurrent, (), {
    const context = Module['preinitializedWebGLContext'];
    if (!context) return 0;
    const handle = GL.registerContext(context, {majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: true});
    if (!GL.makeContextCurrent(handle)) { GL.deleteContext(handle); return 0; }
    return handle;
});
EM_JS(void, HE_ReleaseWebGLContext, (int handle), { GL.deleteContext(handle); });
EM_JS(int, HE_CurrentWebGLProgram, (), {
    const program = GLctx.currentProgram;
    return program ? program.name : 0;
});
static void HE_GetSokolInteger(GLenum name, GLint* value)
{
    if (name == GL_CURRENT_PROGRAM) *value = HE_CurrentWebGLProgram();
    else glGetIntegerv(name, value);
}

namespace HEngine
{
    namespace
    {
        std::string sGraphicsError;
        bool sTraceGraphics = false;
        const char* WebGLErrorName(GLenum error)
        {
            switch (error)
            {
            case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
            case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
            case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
            case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
            case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
            case 0x9242: return "CONTEXT_LOST_WEBGL";
            default: return "Unknown WebGL error";
            }
        }
        void RecordWebGLError(const char* operation, int line)
        {
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR && sGraphicsError.empty())
            {
                sGraphicsError = std::string(WebGLErrorName(error)) + " (" + std::to_string(error) + ") at " +
                    operation + ":" + std::to_string(line);
                HE_CORE_ERROR("{}", sGraphicsError);
            }
        }
        void TraceWebGLError(const char* operation, int line)
        {
            if (sTraceGraphics) RecordWebGLError(operation, line);
        }
        void CheckGraphics(const char* stage)
        {
            RecordWebGLError(stage, 0);
            if (!sGraphicsError.empty()) throw std::runtime_error(std::string(stage) + ": " + sGraphicsError);
        }
        void GraphicsLog(const char* tag, uint32_t level, uint32_t item, const char* message, uint32_t line, const char* file, void*)
        {
            if (level <= 1 && sGraphicsError.empty())
                sGraphicsError = "Sokol[" + std::to_string(item) + "] " + (message ? message : "error") + ":" + std::to_string(line);
            HE_CORE_INFO("Sokol[{}:{}] {} {}:{}: {}", level, item, tag ? tag : "gfx", file ? file : "", line, message ? message : "(message stripped in release)");
        }
        class SokolRenderer final : public LightRenderer
        {
        public:
            ~SokolRenderer() override { Shutdown(); }
            void Init(const LightTexture& texture) override
            {
                sGraphicsError.clear();
                sTraceGraphics = true;
                mTexturePath = texture.AssetPath;
                mContext = HE_MakeWebGLCurrent();
                if (!mContext) throw std::runtime_error("Cannot bind the host WebGL 2 context");
                // Establish the tracked binding before Sokol reads its cache.
                glUseProgram(0);
                CheckGraphics("host context registration");
                sg_desc setup{};
                setup.logger.func = GraphicsLog;
                setup.environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
                setup.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
                setup.environment.defaults.sample_count = 1;
                sg_setup(setup);
                mInitialized = true;
                CheckGraphics("Sokol setup");
                sg_shader_desc shader{};
                shader.vertex_func.source = WeChatShader::Vertex;
                shader.fragment_func.source = WeChatShader::Fragment;
                shader.attrs[0].glsl_name = WeChatShader::Attribute0;
                shader.attrs[1].glsl_name = WeChatShader::Attribute1;
                shader.uniform_blocks[0].stage = SG_SHADERSTAGE_VERTEX;
                shader.uniform_blocks[0].size = 80;
                shader.uniform_blocks[0].layout = SG_UNIFORMLAYOUT_STD140;
                shader.uniform_blocks[0].glsl_uniforms[0] = { SG_UNIFORMTYPE_FLOAT4, 5, WeChatShader::Uniform };
                shader.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
                shader.views[0].texture.image_type = SG_IMAGETYPE_2D;
                shader.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
                shader.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
                shader.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
                shader.texture_sampler_pairs[0] = { SG_SHADERSTAGE_FRAGMENT, 0, 0, WeChatShader::Sampler };
                mShader = sg_make_shader(shader);
                CheckGraphics("shader creation");
                sg_pipeline_desc pipeline{};
                pipeline.shader = mShader;
                pipeline.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
                pipeline.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
                pipeline.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
                pipeline.depth.write_enabled = true;
                pipeline.colors[0].blend.enabled = true;
                pipeline.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
                pipeline.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                pipeline.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
                pipeline.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                mPipeline = sg_make_pipeline(pipeline);
                CheckGraphics("pipeline creation");
                constexpr float vertices[] = { -.5f,-.5f,0,1, .5f,-.5f,1,1, .5f,.5f,1,0,
                                              -.5f,-.5f,0,1, .5f,.5f,1,0, -.5f,.5f,0,0 };
                sg_buffer_desc buffer{};
                buffer.data = SG_RANGE(vertices);
                mBindings.vertex_buffers[0] = sg_make_buffer(buffer);
                CheckGraphics("vertex buffer creation");
                sg_image_desc image{};
                image.width = texture.Width;
                image.height = texture.Height;
                image.pixel_format = SG_PIXELFORMAT_RGBA8;
                image.data.mip_levels[0] = { texture.Pixels.data(), texture.Pixels.size() };
                mImage = sg_make_image(image);
                CheckGraphics("texture creation");
                sg_view_desc view{};
                view.texture.image = mImage;
                mBindings.views[0] = sg_make_view(view);
                CheckGraphics("texture view creation");
                sg_sampler_desc sampler{};
                sampler.min_filter = SG_FILTER_NEAREST;
                sampler.mag_filter = SG_FILTER_NEAREST;
                sampler.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
                sampler.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
                mBindings.samplers[0] = sg_make_sampler(sampler);
                CheckGraphics("sampler creation");
                if (sg_query_shader_state(mShader) != SG_RESOURCESTATE_VALID ||
                    sg_query_pipeline_state(mPipeline) != SG_RESOURCESTATE_VALID ||
                    sg_query_buffer_state(mBindings.vertex_buffers[0]) != SG_RESOURCESTATE_VALID ||
                    sg_query_image_state(mImage) != SG_RESOURCESTATE_VALID ||
                    sg_query_view_state(mBindings.views[0]) != SG_RESOURCESTATE_VALID ||
                    sg_query_sampler_state(mBindings.samplers[0]) != SG_RESOURCESTATE_VALID)
                    throw std::runtime_error("Sokol initialization failed; see complete shader/driver log.");
            }
            void Resize(int width, int height) override { mWidth = width; mHeight = height; }
            LightRenderCapabilities Capabilities() const override { return {}; }
            void Submit(const RenderCamera& camera, std::span<const RenderPrimitive2D> primitives) override
            {
                for (const auto& primitive : primitives)
                    if (primitive.Kind != RenderPrimitive2DKind::Quad || primitive.TextureAssetPath != mTexturePath)
                        throw std::runtime_error("Unsupported primitive or texture in the WeChat sprite profile");
                CheckGraphics("before frame");
                sg_pass pass{};
                pass.swapchain.width = mWidth;
                pass.swapchain.height = mHeight;
                pass.swapchain.sample_count = 1;
                pass.swapchain.color_format = SG_PIXELFORMAT_RGBA8;
                pass.swapchain.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
                pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
                pass.action.colors[0].clear_value = { .025f, .04f, .07f, 1 };
                pass.action.depth.load_action = SG_LOADACTION_CLEAR;
                pass.action.depth.clear_value = 1;
                sg_begin_pass(pass);
                TraceWebGLError("sg_begin_pass", 0);
                sg_apply_pipeline(mPipeline);
                TraceWebGLError("sg_apply_pipeline", 0);
                sg_apply_bindings(mBindings);
                TraceWebGLError("sg_apply_bindings", 0);
                for (const auto& primitive : primitives)
                {
                    std::array<glm::vec4, 5> uniforms;
                    const auto matrix = camera.ViewProjection * primitive.Transform;
                    for (int row = 0; row < 4; ++row)
                        uniforms[row] = { matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row] };
                    uniforms[4] = primitive.Color;
                    sg_apply_uniforms(0, { uniforms.data(), sizeof(uniforms) });
                    TraceWebGLError("sg_apply_uniforms", 0);
                    sg_draw(0, 6, 1);
                    TraceWebGLError("sg_draw", 0);
                }
                sg_end_pass();
                sg_commit();
                CheckGraphics("frame submission");
                sTraceGraphics = false;
            }
            void Shutdown() override
            {
                sTraceGraphics = false;
                if (mInitialized) { sg_shutdown(); mInitialized = false; }
                if (mContext) { HE_ReleaseWebGLContext(mContext); mContext = 0; }
                sGraphicsError.clear();
            }
        private:
            sg_shader mShader{};
            sg_pipeline mPipeline{};
            sg_image mImage{};
            sg_bindings mBindings{};
            std::string mTexturePath;
            int mWidth = 1, mHeight = 1;
            int mContext = 0;
            bool mInitialized = false;
        };
    }
    Scope<LightRenderer> CreateLightRenderer() { return CreateScope<SokolRenderer>(); }
}
