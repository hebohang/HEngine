#include "Hepch.h"

#include "Runtime/Resource/ResType/UIRes.h"

#include "_generated/serializer/UIRes.gen.h"
#include "Runtime/Core/Log/Log.h"
#include "Runtime/Platform/PlatformFileSystem.h"
#include "Runtime/Utils/FileUtils/AtomicFile.h"
#include "Runtime/Utils/JsonUtils/JsonUtils.h"
#include "Runtime/Utils/PathUtils/PathUtils.h"

#include <array>
#include <exception>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HEngine
{
    namespace
    {
        constexpr const char* kDefaultFontFamily = "Open Sans";

        static std::string ToCssLength(float value)
        {
            std::ostringstream stream;
            stream << std::round(value) << "dp";
            return stream.str();
        }

        static std::string SanitizeClassName(const std::string& className)
        {
            std::string result = className;
            if (result.empty())
                return result;

            for (char& c : result)
            {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
                    c = '_';
            }
            return result;
        }

        static std::string TrimCopy(const std::string& value);
        static std::string ToLowerCopy(std::string value);

        static std::string ResolveFontFamilyFromPath(const std::string& fontPath)
        {
            const std::string lowerFontPath = ToLowerCopy(fontPath);
            if (lowerFontPath.find("notosanssc") != std::string::npos || lowerFontPath.find("noto_sans_sc") != std::string::npos)
                return "Noto Sans SC";
            if (lowerFontPath.find("opensans") != std::string::npos || lowerFontPath.find("open_sans") != std::string::npos || lowerFontPath.find("open sans") != std::string::npos)
                return "Open Sans";
            return kDefaultFontFamily;
        }

        static const char* ResolveFontWeightFromPath(const std::string& fontPath)
        {
            const std::string lowerFontPath = ToLowerCopy(fontPath);
            if (lowerFontPath.find("bold") != std::string::npos || lowerFontPath.find("semibold") != std::string::npos || lowerFontPath.find("extrabold") != std::string::npos || lowerFontPath.find("black") != std::string::npos || lowerFontPath.find("medium") != std::string::npos)
                return "bold";
            return "normal";
        }

        static const char* ResolveFontStyleFromPath(const std::string& fontPath)
        {
            const std::string lowerFontPath = ToLowerCopy(fontPath);
            return lowerFontPath.find("italic") != std::string::npos ? "italic" : "normal";
        }

        static const char* MakeWidgetTypeName(UIWidgetType type)
        {
            switch (type)
            {
            case UIWidgetType::Canvas: return "Canvas";
            case UIWidgetType::Panel: return "Panel";
            case UIWidgetType::Text: return "Text";
            case UIWidgetType::Image: return "Image";
            case UIWidgetType::Button: return "Button";
            default: return "Panel";
            }
        }

        static std::string TrimCopy(const std::string& value)
        {
            size_t begin = 0;
            while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
                ++begin;

            size_t end = value.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
                --end;

            return value.substr(begin, end - begin);
        }

        static std::string ToLowerCopy(std::string value)
        {
            for (char& c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        }

        static std::string SanitizeExtraStyle(const std::string& extraStyle)
        {
            if (extraStyle.empty())
                return {};

            static const std::unordered_set<std::string> managedProperties = {
                "background-color",
                "background-image",
                "border",
                "border-radius",
                "color",
                "font-family",
                "font-size",
                "overflow",
                "pointer-events"
            };

            std::ostringstream sanitized;
            size_t start = 0;
            while (start < extraStyle.size())
            {
                const size_t end = extraStyle.find(';', start);
                const std::string declaration = TrimCopy(extraStyle.substr(start, end == std::string::npos ? std::string::npos : end - start));
                start = (end == std::string::npos) ? extraStyle.size() : end + 1;

                if (declaration.empty())
                    continue;

                const size_t colon = declaration.find(':');
                if (colon == std::string::npos)
                    continue;

                const std::string property = ToLowerCopy(TrimCopy(declaration.substr(0, colon)));
                if (managedProperties.contains(property))
                    continue;

                sanitized << declaration << "; ";
            }

            return sanitized.str();
        }

        static std::string MakeNodeTag(UIWidgetType type)
        {
            switch (type)
            {
            case UIWidgetType::Canvas:
            case UIWidgetType::Panel:
                return "div";
            case UIWidgetType::Text:
                return "p";
            case UIWidgetType::Image:
                return "img";
            case UIWidgetType::Button:
                return "button";
            default:
                return "div";
            }
        }

        static void AppendStyleRule(std::ostringstream& css, const UINodeRes& node)
        {
            const std::string elementId = node.ElementId.empty() ? ("node_" + std::to_string(node.Id)) : node.ElementId;
            const std::string fontFamily = ResolveFontFamilyFromPath(node.Style.FontPath);
            css << "#" << elementId << " {\n";
            css << "  position: absolute;\n";
            css << "  background-color: " << node.Style.BackgroundColor << ";\n";
            css << "  color: " << node.Style.TextColor << ";\n";
            if (node.Style.BorderWidth > 0.0f)
                css << "  border: " << ToCssLength(node.Style.BorderWidth) << " " << node.Style.BorderColor << ";\n";
            if (node.Style.BorderRadius > 0.0f)
                css << "  border-radius: " << ToCssLength(node.Style.BorderRadius) << ";\n";
            css << "  overflow: hidden;\n";
            css << "  pointer-events: " << (node.bInteractable ? "auto" : "none") << ";\n";
            if (!node.Style.BackgroundImage.empty())
                css << "  background-image: url('" << Utils::NormalizeAssetPath(node.Style.BackgroundImage) << "');\n";
            if (!node.Style.FontPath.empty() || node.Type == UIWidgetType::Text || node.Type == UIWidgetType::Button)
            {
                css << "  font-family: '" << fontFamily << "';\n";
                css << "  font-weight: " << ResolveFontWeightFromPath(node.Style.FontPath) << ";\n";
                css << "  font-style: " << ResolveFontStyleFromPath(node.Style.FontPath) << ";\n";
            }
            css << "  font-size: " << ToCssLength(node.Style.FontSize) << ";\n";
            if (node.Type == UIWidgetType::Text)
                css << "  margin: 0dp;\n";
            const std::string extraStyle = SanitizeExtraStyle(node.Style.ExtraStyle);
            if (!extraStyle.empty())
                css << "  " << extraStyle << "\n";
            css << "}\n\n";

            if (node.Type == UIWidgetType::Button)
            {
                css << "#" << elementId << ":hover {\n";
                css << "  filter: brightness(1.1);\n";
                css << "}\n\n";
            }
        }

        static void AppendNodeRml(std::ostringstream& rml, const UINodeRes& node, const std::unordered_map<int, std::vector<const UINodeRes*>>& childrenMap, int indentLevel, const glm::vec2& parentSize)
        {
            const std::string indent(indentLevel * 2, ' ');
            const std::string tag = MakeNodeTag(node.Type);
            const std::string className = SanitizeClassName(node.Style.ClassName.empty() ? ("hui-node-" + std::to_string(node.Id)) : node.Style.ClassName);
            const std::string elementId = node.ElementId.empty() ? ("node_" + std::to_string(node.Id)) : node.ElementId;

            glm::vec2 position(0.0f, 0.0f);
            glm::vec2 size = parentSize;
            if (node.Type != UIWidgetType::Canvas)
            {
                const UILayoutRect rect = ResolveUILayoutRect(node.Layout, parentSize);
                position = rect.Position;
                size = rect.Size;
            }

            rml << indent << "<" << tag
                << " id=\"" << elementId << "\""
                << " class=\"" << className << "\""
                << " data-he-widget=\"" << MakeWidgetTypeName(node.Type) << "\""
                << " data-he-anchor-min=\"" << node.Layout.AnchorMin.x << "," << node.Layout.AnchorMin.y << "\""
                << " data-he-anchor-max=\"" << node.Layout.AnchorMax.x << "," << node.Layout.AnchorMax.y << "\""
                << " data-he-pivot=\"" << node.Layout.Pivot.x << "," << node.Layout.Pivot.y << "\""
                << " data-he-offset-min=\"" << node.Layout.OffsetMin.x << "," << node.Layout.OffsetMin.y << "\""
                << " data-he-offset-max=\"" << node.Layout.OffsetMax.x << "," << node.Layout.OffsetMax.y << "\""
                << " style=\"";

            if (node.Type == UIWidgetType::Canvas)
            {
                rml << "left:0dp; top:0dp; width:100%; height:100%;";
            }
            else
            {
                rml << "left:" << ToCssLength(position.x)
                    << "; top:" << ToCssLength(position.y)
                    << "; width:" << ToCssLength(size.x)
                    << "; height:" << ToCssLength(size.y)
                    << ";";
            }

            if (!node.bVisible)
                rml << "display:none;";

            rml << "\"";

            if (node.Type == UIWidgetType::Image && !node.ImagePath.empty())
                rml << " src=\"" << Utils::NormalizeAssetPath(node.ImagePath) << "\"";

            const auto childIt = childrenMap.find(node.Id);
            const bool bHasChildren = childIt != childrenMap.end() && !childIt->second.empty();
            const bool bHasText = !node.Text.empty();
            if (!bHasChildren && !bHasText)
            {
                rml << " />\n";
                return;
            }

            rml << ">";
            if (bHasText)
                rml << node.Text;
            if (bHasChildren)
                rml << "\n";

            if (bHasChildren)
            {
                for (const UINodeRes* child : childIt->second)
                    AppendNodeRml(rml, *child, childrenMap, indentLevel + 1, size);
            }

            if (bHasChildren)
                rml << indent;
            rml << "</" << tag << ">\n";
        }

        struct RenderedUIFiles
        {
            std::filesystem::path RmlPath;
            std::filesystem::path RcssPath;
            std::string Rml;
            std::string Rcss;
        };

        static RenderedUIFiles RenderUIFiles(
            const UIRes& res,
            const std::filesystem::path& sourcePath)
        {
            RenderedUIFiles rendered;
            rendered.RmlPath = sourcePath.parent_path() / (sourcePath.stem().string() + ".rml");
            rendered.RcssPath = sourcePath.parent_path() / (sourcePath.stem().string() + ".rcss");

            std::ostringstream css;
            css << "body {\n";
            css << "  margin: 0px;\n";
            css << "  padding: 0px;\n";
            css << "  width: 100%;\n";
            css << "  height: 100%;\n";
            css << "  overflow: hidden;\n";
            css << "  font-family: '" << kDefaultFontFamily << "';\n";
            css << "  font-weight: normal;\n";
            css << "  font-style: normal;\n";
            css << "}\n\n";

            for (const UINodeRes& node : res.Nodes)
                AppendStyleRule(css, node);

            std::unordered_map<int, std::vector<const UINodeRes*>> childrenMap;
            const UINodeRes* root = nullptr;
            for (const UINodeRes& node : res.Nodes)
            {
                if (node.ParentId >= 0)
                    childrenMap[node.ParentId].push_back(&node);
                if (node.Type == UIWidgetType::Canvas && !root)
                    root = &node;
            }

            if (!root && !res.Nodes.empty())
                root = &res.Nodes.front();
            if (!root)
                throw std::runtime_error("UIRes has no nodes to export.");

            glm::vec2 rootSize = ResolveUILayoutSize(root->Layout);
            if (rootSize.x <= 0.0f || rootSize.y <= 0.0f)
                rootSize = glm::vec2(1280.0f, 720.0f);

            std::ostringstream rml;
            rml << "<rml>\n";
            rml << "  <head>\n";
            rml << "    <title>" << res.Name << "</title>\n";
            rml << "    <link type=\"text/rcss\" href=\"" << rendered.RcssPath.filename().generic_string() << "\" />\n";
            rml << "  </head>\n";
            rml << "  <body id=\"canvas\">\n";
            AppendNodeRml(rml, *root, childrenMap, 2, rootSize);
            rml << "  </body>\n";
            rml << "</rml>\n";

            if (!css || !rml)
                throw std::runtime_error("Failed to render RmlUi files in memory.");
            rendered.Rcss = css.str();
            rendered.Rml = rml.str();
            return rendered;
        }

        struct UIFileTransactionEntry
        {
            std::filesystem::path TargetPath;
            std::filesystem::path StagingPath;
            std::filesystem::path BackupPath;
            std::string_view Contents;
            bool bHadTarget = false;
            bool bCommitted = false;
            bool bPreserveBackup = false;
        };

        [[nodiscard]] std::vector<char> ReadFileBytes(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
                throw std::runtime_error("Failed to open UI resource backup source: " + path.string());

            const std::streamoff end = stream.tellg();
            if (end < 0 || static_cast<uintmax_t>(end) >
                static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            {
                throw std::runtime_error("UI resource backup source is too large: " + path.string());
            }

            std::vector<char> bytes(static_cast<size_t>(end));
            stream.seekg(0, std::ios::beg);
            if (!bytes.empty())
                stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!stream)
                throw std::runtime_error("Failed to read UI resource backup source: " + path.string());
            return bytes;
        }

        void AppendTransactionError(
            std::string& errors,
            std::string_view context,
            const std::filesystem::path& path,
            const std::error_code& error) noexcept
        {
            try
            {
                if (!errors.empty())
                    errors += "; ";
                errors += std::string(context) + " " + path.string() + ": " + error.message();
            }
            catch (...)
            {
            }
        }

        void CleanupTransactionFile(
            const std::filesystem::path& path,
            std::string& errors) noexcept
        {
            if (path.empty())
                return;

            std::error_code error;
            (void)std::filesystem::remove(path, error);
            if (error)
                AppendTransactionError(errors, "failed to remove", path, error);
        }

        [[nodiscard]] std::string DescribeFailure(const std::exception_ptr& failure)
        {
            try
            {
                std::rethrow_exception(failure);
            }
            catch (const std::exception& exception)
            {
                return exception.what();
            }
            catch (...)
            {
                return "unknown exception";
            }
        }

        template<size_t FileCount>
        void SaveUIFilesTransaction(std::array<UIFileTransactionEntry, FileCount>& files)
        {
            try
            {
                for (UIFileTransactionEntry& file : files)
                {
                    file.StagingPath = PlatformFileSystem::MakeTemporarySiblingPath(file.TargetPath);
                    Utils::WriteTextToFileAtomically(
                        file.StagingPath,
                        file.Contents,
                        Utils::AtomicWriteMode::CreateNew);
                }

                for (UIFileTransactionEntry& file : files)
                {
                    std::error_code statusError;
                    const std::filesystem::file_status status =
                        std::filesystem::symlink_status(file.TargetPath, statusError);
                    if (statusError == std::errc::no_such_file_or_directory)
                    {
                        statusError.clear();
                        continue;
                    }
                    if (statusError)
                    {
                        throw std::runtime_error(
                            "Failed to inspect existing UI resource file " + file.TargetPath.string() +
                            ": " + statusError.message());
                    }
                    if (!std::filesystem::exists(status))
                        continue;
                    if (!std::filesystem::is_regular_file(status))
                    {
                        throw std::runtime_error(
                            "UI resource save target is not a regular file: " + file.TargetPath.string());
                    }

                    const std::vector<char> bytes = ReadFileBytes(file.TargetPath);
                    file.BackupPath = PlatformFileSystem::MakeTemporarySiblingPath(file.TargetPath);
                    Utils::WriteBytesToFileAtomically(
                        file.BackupPath,
                        bytes.data(),
                        bytes.size(),
                        Utils::AtomicWriteMode::CreateNew);
                    file.bHadTarget = true;
                }

                for (UIFileTransactionEntry& file : files)
                {
                    std::error_code publishError;
                    if (!PlatformFileSystem::PublishFileAtomically(
                            file.StagingPath,
                            file.TargetPath,
                            true,
                            publishError))
                    {
                        throw std::runtime_error(
                            "Failed to publish UI resource file " + file.TargetPath.string() +
                            ": " + publishError.message());
                    }
                    file.bCommitted = true;
                    file.StagingPath.clear();
                }
            }
            catch (...)
            {
                const std::exception_ptr originalFailure = std::current_exception();
                std::string rollbackErrors;
                for (auto fileIt = files.rbegin(); fileIt != files.rend(); ++fileIt)
                {
                    UIFileTransactionEntry& file = *fileIt;
                    if (!file.bCommitted)
                        continue;

                    if (file.bHadTarget)
                    {
                        std::error_code restoreError;
                        if (PlatformFileSystem::PublishFileAtomically(
                                file.BackupPath,
                                file.TargetPath,
                                true,
                                restoreError))
                        {
                            file.BackupPath.clear();
                        }
                        else
                        {
                            file.bPreserveBackup = true;
                            AppendTransactionError(
                                rollbackErrors,
                                "failed to restore",
                                file.TargetPath,
                                restoreError);
                        }
                    }
                    else
                    {
                        std::error_code removeError;
                        (void)std::filesystem::remove(file.TargetPath, removeError);
                        if (removeError)
                        {
                            AppendTransactionError(
                                rollbackErrors,
                                "failed to remove newly published",
                                file.TargetPath,
                                removeError);
                        }
                    }
                }

                for (UIFileTransactionEntry& file : files)
                {
                    CleanupTransactionFile(file.StagingPath, rollbackErrors);
                    if (!file.bPreserveBackup)
                        CleanupTransactionFile(file.BackupPath, rollbackErrors);
                }

                if (!rollbackErrors.empty())
                {
                    throw std::runtime_error(
                        "UI resource save failed: " + DescribeFailure(originalFailure) +
                        "; rollback/cleanup also failed: " + rollbackErrors);
                }
                std::rethrow_exception(originalFailure);
            }

            std::string cleanupErrors;
            for (UIFileTransactionEntry& file : files)
            {
                CleanupTransactionFile(file.StagingPath, cleanupErrors);
                CleanupTransactionFile(file.BackupPath, cleanupErrors);
            }
            if (!cleanupErrors.empty())
            {
                Log::WriteFallbackError(
                    "UI resource save committed but temporary-file cleanup failed.",
                    cleanupErrors);
            }
        }
    }

    bool LoadUIRes(const std::filesystem::path& fullPath, UIRes& outRes, std::string* outError)
    {
        try
        {
            outRes = Utils::LoadJsonFile(fullPath).get<UIRes>();
            if (outRes.Name.empty())
                outRes.Name = fullPath.stem().string();
        }
        catch (const std::exception& ex)
        {
            if (outError)
                *outError = ex.what();
            return false;
        }

        return true;
    }

    bool SaveUIRes(const UIRes& res, const std::filesystem::path& fullPath, std::string* outError)
    {
        try
        {
            const RenderedUIFiles rendered = RenderUIFiles(res, fullPath);
            Json persistedJson;
            Serializer::Serialize(persistedJson, res);
            const std::string persistedText = persistedJson.dump(4);

            std::array<UIFileTransactionEntry, 3> files = {{
                { rendered.RcssPath, {}, {}, rendered.Rcss },
                { rendered.RmlPath, {}, {}, rendered.Rml },
                { fullPath, {}, {}, persistedText }
            }};
            SaveUIFilesTransaction(files);
        }
        catch (const std::exception& ex)
        {
            if (outError)
                *outError = ex.what();
            return false;
        }

        return true;
    }

    bool ExportUIRes(const UIRes& res, const std::filesystem::path& sourcePath, std::string* outError)
    {
        try
        {
            const RenderedUIFiles rendered = RenderUIFiles(res, sourcePath);
            Utils::WriteTextToFileAtomically(rendered.RcssPath, rendered.Rcss);
            Utils::WriteTextToFileAtomically(rendered.RmlPath, rendered.Rml);
        }
        catch (const std::exception& ex)
        {
            if (outError)
                *outError = ex.what();
            return false;
        }

        return true;
    }
}
