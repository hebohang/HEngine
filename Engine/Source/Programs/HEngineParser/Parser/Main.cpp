#include "Common/Precompiled.h"
#include "Parser/Parser.h"

namespace
{
    int argumentError(const std::string& message)
    {
        std::string sanitized = message;
        std::replace(sanitized.begin(), sanitized.end(), '|', '/');
        std::replace(sanitized.begin(), sanitized.end(), '\r', ' ');
        std::replace(sanitized.begin(), sanitized.end(), '\n', ' ');
        std::cerr << "HE_CODEGEN_DIAGNOSTIC|error|<arguments>|0|0|" << sanitized << '\n';
        return 2;
    }

    bool assignOption(const std::string& name,
                      const std::string& value,
                      std::set<std::string>& seen,
                      ParserOptions& options)
    {
        if (value.empty() || !seen.emplace(name).second)
        {
            return false;
        }

        if (name == "--input-list")
        {
            options.inputList = value;
        }
        else if (name == "--input-header")
        {
            options.inputHeader = value;
        }
        else if (name == "--output-root")
        {
            options.outputRoot = value;
        }
        else if (name == "--context")
        {
            options.contextFile = value;
        }
        else if (name == "--templates")
        {
            options.templatesDirectory = value;
        }
        else if (name == "--index")
        {
            options.indexFile = value;
        }
        else if (name == "--module")
        {
            options.moduleName = value;
        }
        else
        {
            return false;
        }
        return true;
    }

    int run(ParserOptions options)
    {
        const auto startTime = std::chrono::steady_clock::now();
        const bool quiet = options.quiet;
        const std::string moduleName = options.moduleName;

        MetaParser::prepare();
        MetaParser parser(std::move(options));
        int result = parser.parse();
        if (result == 0)
        {
            result = parser.generateFiles();
        }

        if (result == 0 && !quiet)
        {
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime);
            std::cout << "HE_CODEGEN|status=success|module=" << moduleName << "|libclang="
                      << parser.getClangVersion() << "|durationMs=" << duration.count() << '\n';
        }
        return result;
    }
}

int main(int argc, char* argv[])
{
    try
    {
        if (argc == 7)
        {
            for (int index = 1; index < argc; ++index)
            {
                if (argv[index] == nullptr || std::string(argv[index]).empty())
                {
                    return argumentError("legacy positional arguments must not be empty");
                }
            }
            const std::string showErrors = argv[6];
            if (showErrors != "0" && showErrors != "1")
            {
                return argumentError("legacy showErrors must be 0 or 1");
            }

            MetaParser::prepare();
            const auto startTime = std::chrono::steady_clock::now();
            MetaParser parser(argv[1], argv[2], argv[3], argv[4], argv[5], showErrors == "1");
            int result = parser.parse();
            if (result == 0)
            {
                result = parser.generateFiles();
            }
            if (result == 0 && showErrors == "1")
            {
                const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startTime);
                std::cout << "HE_CODEGEN|status=success|module=" << argv[5] << "|libclang="
                          << parser.getClangVersion() << "|durationMs=" << duration.count() << '\n';
            }
            return result;
        }

        ParserOptions options;
        std::set<std::string> seen;
        for (int index = 1; index < argc; ++index)
        {
            if (argv[index] == nullptr)
            {
                return argumentError("null command-line argument");
            }
            const std::string argument = argv[index];
            if (argument == "--quiet" || argument == "--validate-generated")
            {
                if (!seen.emplace(argument).second)
                {
                    return argumentError("duplicate --quiet option");
                }
                if (argument == "--quiet")
                {
                    options.quiet = true;
                }
                else
                {
                    options.validateGenerated = true;
                }
                continue;
            }
            if (!argument.starts_with("--"))
            {
                return argumentError("unexpected positional argument in flag mode: " + argument);
            }
            if (index + 1 >= argc || argv[index + 1] == nullptr)
            {
                return argumentError("missing value for " + argument);
            }
            const std::string value = argv[++index];
            if (!assignOption(argument, value, seen, options))
            {
                return argumentError("unknown, duplicate, or empty option: " + argument);
            }
        }

        const std::vector<std::string> required = {"--input-list", "--input-header", "--output-root", "--context",
                                                   "--templates", "--index", "--module"};
        for (const auto& option : required)
        {
            if (!seen.contains(option))
            {
                return argumentError("missing required option: " + option);
            }
        }
        return run(std::move(options));
    }
    catch (const std::exception& exception)
    {
        return argumentError(std::string("unhandled parser exception: ") + exception.what());
    }
    catch (...)
    {
        return argumentError("unhandled non-standard parser exception");
    }
}
