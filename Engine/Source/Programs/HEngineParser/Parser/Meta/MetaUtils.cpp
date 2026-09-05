#include "Common/Precompiled.h"

#include "MetaUtils.h"

static int parse_flag = 0;
namespace Utils
{

    void toString(const CXString& str, std::string& output)
    {
        auto cstr = clang_getCString(str);

        output = cstr != nullptr ? cstr : "";

        clang_disposeString(str);
    }

    std::string getQualifiedName(const CursorType& type) { return type.GetDisplayName(); }

    std::string getTypeNameWithoutNamespace(const CursorType& type)
    {
        std::string&& type_name = type.GetDisplayName();
        return type_name;
    }

    std::string getQualifiedName(const std::string& display_name, const Namespace& current_namespace)
    {
        std::string name = "";
        for (auto& iter_string : current_namespace)
        {
            name += (iter_string + "::");
        }

        name += display_name;

        return name;
    }

    std::string getQualifiedName(const Cursor& cursor, const Namespace& current_namespace)
    {
        return getQualifiedName(cursor.getSpelling(), current_namespace);
    }

    std::string formatQualifiedName(std::string& source_string)
    {
        Utils::replace(source_string, '<', 'L');
        Utils::replace(source_string, ':', 'S');
        Utils::replace(source_string, '>', 'R');
        Utils::replace(source_string, '*', 'P');
        return source_string;
    }

    /// <summary>
    ///
    /// </summary>
    /// <param name="from"></param>
    /// <param name="to"></param>
    /// <returns></returns>
    fs::path makeRelativePath(const fs::path& from, const fs::path& to)
    {
        // Start at the root path and while they are the same then do nothing then when they first
        // diverge take the remainder of the two path and replace the entire from path with ".."
        // segments.
        std::string form_complete_string;
        std::string to_complete_string;

        /*remove ".." and "."*/
        (void)formatPathString(from.string(), form_complete_string);
        (void)formatPathString(to.string(), to_complete_string);

        fs::path form_complete = form_complete_string;
        fs::path to_complete   = to_complete_string;

        auto iter_from = form_complete.begin();
        auto iter_to   = to_complete.begin();

        // Loop through both
        while (iter_from != form_complete.end() && iter_to != to_complete.end() && (*iter_to) == (*iter_from))
        {
            ++iter_to;
            ++iter_from;
        }

        fs::path final_path;
        while (iter_from != form_complete.end())
        {
            final_path /= "..";

            ++iter_from;
        }

        while (iter_to != to_complete.end())
        {
            final_path /= *iter_to;

            ++iter_to;
        }

        return final_path;
    }

    void fatalError(const std::string& error)
    {
        std::cerr << "Error: " << error << std::endl;

        exit(EXIT_FAILURE);
    }

    std::vector<std::string> split(std::string input, std::string pat)
    {
        std::vector<std::string> ret_list;
        if (pat.empty())
        {
            ret_list.emplace_back(std::move(input));
            return ret_list;
        }
        while (true)
        {
            size_t      index    = input.find(pat);
            std::string sub_list = input.substr(0, index);
            if (!sub_list.empty())
            {

                ret_list.push_back(sub_list);
            }
            if (index == std::string::npos)
            {
                break;
            }
            input.erase(0, index + pat.size());
        }
        return ret_list;
    }

    std::string getFileName(std::string path)
    {
        if (path.size() < 1)
        {
            return std::string();
        }
        return fs::path(path).filename().string();
    }

    std::string getNameWithoutFirstM(std::string& name)
    {
        std::string result = name;
        if (name.size() > 2 && name[0] == 'm' && name[1] == '_')
        {
            result = name.substr(2);
        }
        return result;
    }

    std::string getNameWithoutContainer(std::string name)
    {

        size_t left  = name.find_first_of('<') + 1;
        size_t right = name.find_last_of('>');
        if (left > 0 && right < name.size() && left < right)
        {
            return name.substr(left, right - left);
        }
        else
        {
            return {};
        }
    }

    std::string getStringWithoutQuot(std::string input)
    {
        size_t left  = input.find_first_of('\"') + 1;
        size_t right = input.find_last_of('\"');
        if (left > 0 && right < input.size() && left < right)
        {
            return input.substr(left, right - left);
        }
        else
        {
            return input;
        }
    }

    std::string trim(const std::string& context)
    {

        std::string ret_string = context;

        ret_string = ret_string.erase(0, ret_string.find_first_not_of(" "));
        ret_string = ret_string.erase(ret_string.find_last_not_of(" ") + 1);

        return ret_string;
    }

    std::string replace(std::string& source_string, std::string sub_string, const std::string new_string)
    {
        std::string::size_type pos = 0;
        while ((pos = source_string.find(sub_string)) != std::string::npos)
        {
            source_string.replace(pos, sub_string.length(), new_string);
        }
        return source_string;
    }

    std::string replace(std::string& source_string, char taget_char, const char new_char)
    {
        std::replace(source_string.begin(), source_string.end(), taget_char, new_char);
        return source_string;
    }

    std::string toUpper(std::string& source_string)
    {
        transform(source_string.begin(), source_string.end(), source_string.begin(), ::toupper);
        return source_string;
    }

    std::string join(std::vector<std::string> context_list, std::string separator)
    {
        std::string ret_string;
        if (context_list.size() == 0)
        {
            return ret_string;
        }
        ret_string = context_list[0];
        for (int index = 1; index < context_list.size(); ++index)
        {
            ret_string += separator + context_list[index];
        }

        return ret_string;
    }

    std::string trim(std::string& source_string, const std::string trim_chars)
    {
        size_t left_pos = source_string.find_first_not_of(trim_chars);
        if (left_pos == std::string::npos)
        {
            source_string = std::string();
        }
        else
        {
            size_t right_pos = source_string.find_last_not_of(trim_chars);
            source_string    = source_string.substr(left_pos, right_pos - left_pos + 1);
        }
        return source_string;
    }

    std::string loadFile(std::string path)
    {
        std::string output;
        std::string error;
        return loadFile(fs::path(std::move(path)), output, error) ? output : std::string();
    }

    bool loadFile(const fs::path& path, std::string& output, std::string& error)
    {
        output.clear();
        error.clear();

        std::ifstream input(path, std::ios::in | std::ios::binary);
        if (!input.is_open())
        {
            error = "could not open file for reading: " + path.generic_string();
            return false;
        }

        std::ostringstream stream;
        stream << input.rdbuf();
        if (input.bad())
        {
            error = "could not read file: " + path.generic_string();
            input.close();
            return false;
        }

        input.close();
        if (input.fail())
        {
            error = "could not close file after reading: " + path.generic_string();
            return false;
        }
        output = stream.str();
        return true;
    }

    bool saveFile(const std::string& outputString, const fs::path& outputFile, std::string& error)
    {
        error.clear();
        const auto parentPath = outputFile.parent_path();
        std::error_code fileError;

        if (!parentPath.empty() && !fs::exists(parentPath, fileError))
        {
            if (fileError || !fs::create_directories(parentPath, fileError))
            {
                if (fileError || !fs::exists(parentPath))
                {
                    error = "could not create output directory: " + parentPath.generic_string();
                    return false;
                }
            }
        }

        std::ofstream output(outputFile, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            error = "could not open output file: " + outputFile.generic_string();
            return false;
        }

        output.write(outputString.data(), static_cast<std::streamsize>(outputString.size()));
        if (!output.good())
        {
            error = "could not write output file: " + outputFile.generic_string();
            output.close();
            return false;
        }

        output.flush();
        if (!output.good())
        {
            error = "could not flush output file: " + outputFile.generic_string();
            output.close();
            return false;
        }

        output.close();
        if (output.fail())
        {
            error = "could not close output file: " + outputFile.generic_string();
            return false;
        }
        return true;
    }

    void replaceAll(std::string& resource_str, std::string sub_str, std::string new_str)
    {
        std::string::size_type pos = 0;
        while ((pos = resource_str.find(sub_str)) != std::string::npos)
        {
            resource_str = resource_str.replace(pos, sub_str.length(), new_str);
        }
        return;
    }

    unsigned long formatPathString(const std::string& path_string, std::string& out_string)
    {
        unsigned int ulRet             = 0;
        auto         local_path_string = path_string;
        fs::path     local_path;

        local_path = local_path_string;
        if (local_path.is_relative())
        {
            local_path_string = fs::current_path().string() + "/" + local_path_string;
        }

        replaceAll(local_path_string, "\\", "/");
        std::vector<std::string> subString = split(local_path_string, "/");
        std::vector<std::string> out_sub_string;
        for (auto p : subString)
        {
            if (p == "..")
            {
                out_sub_string.pop_back();
            }
            else if (p != ".")
            {
                out_sub_string.push_back(p);
            }
        }
        for (int i = 0; i < out_sub_string.size() - 1; i++)
        {
            out_string.append(out_sub_string[i] + "/");
        }
        out_string.append(out_sub_string[out_sub_string.size() - 1]);
        return 0;
    }
    std::string convertNameToUpperCamelCase(const std::string& name, std::string pat)
    {
        std::string ret_string;
        auto&& name_spilts = split(name, pat);
        for (auto& split_string : name_spilts)
        {
            split_string[0] = toupper(split_string[0]);
            ret_string.append(split_string);
        }
        return ret_string;
    }
} // namespace Utils
