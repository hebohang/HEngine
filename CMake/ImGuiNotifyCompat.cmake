include_guard(GLOBAL)

function(he_enable_imgui_notify_compat)
    if(NOT APPLE OR NOT TARGET imgui_notify)
        return()
    endif()

    set(vendorDirectory "${EngineSourceDir}/ThirdParty/imgui-notify-v2")
    set(vendorHeader "${vendorDirectory}/imgui_notify.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${vendorHeader}")
    file(READ "${vendorHeader}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 sourceHash "${source}")
    if(NOT sourceHash STREQUAL "60fd5708f52e77b5aa9d745a6353544c357e2fe57aa5549176ff052a5629d71c")
        message(FATAL_ERROR "The vendored imgui-notify header changed; review its macOS compatibility overlay.")
    endif()

    # Keep vendor sources intact. These narrowly scoped substitutions remove
    # Win32/MSVC dependencies and dangling temporary references from this pinned
    # header; all timekeeping remains standard C++ in the generated overlay.
    string(REPLACE "#include <vector>"
        "#include <chrono>\n#include <cstdarg>\n#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <vector>"
        source "${source}")
    string(REPLACE "GetTickCount64()"
        "static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())"
        source "${source}")
    string(REPLACE "NOTIFY_INLINE VOID " "NOTIFY_INLINE void " source "${source}")
    string(REPLACE "fn(format, args, __VA_ARGS__)"
        "fn(format, args __VA_OPT__(,) __VA_ARGS__)" source "${source}")
    string(REPLACE "sprintf_s(window_name, \"##TOAST%d\", i);"
        "std::snprintf(window_name, sizeof(window_name), \"##TOAST%d\", i);" source "${source}")
    string(REPLACE "get_color() -> const ImVec4&" "get_color() -> ImVec4" source "${source}")
    string(REPLACE "get_phase() -> const ImGuiToastPhase&"
        "get_phase() -> ImGuiToastPhase" source "${source}")
    string(REPLACE "\t\t\treturn { 0, 157, 255, 255 }; // Blue\n\t\t}\n\t}"
        "\t\t\treturn { 0, 157, 255, 255 }; // Blue\n\t\t}\n\t\treturn { 255, 255, 255, 255 };\n\t}"
        source "${source}")
    string(REPLACE "\t\t\treturn ICON_FA_INFO_CIRCLE;\n\t\t}\n\t}"
        "\t\t\treturn ICON_FA_INFO_CIRCLE;\n\t\t}\n\t\treturn nullptr;\n\t}"
        source "${source}")
    # The setters already format messages; render their results as plain text.
    string(REPLACE "TextColored(text_color, icon);"
        "TextColored(text_color, \"%s\", icon);" source "${source}")
    foreach(text title default_title content)
        string(REPLACE "Text(${text});" "TextUnformatted(${text});" source "${source}")
    endforeach()

    set(overlayRoot "${CMAKE_BINARY_DIR}/generated/thirdparty")
    set(overlayDirectory "${overlayRoot}/imgui-notify-v2")
    file(MAKE_DIRECTORY "${overlayDirectory}")
    file(CONFIGURE OUTPUT "${overlayDirectory}/imgui_notify.h" CONTENT "${source}" @ONLY)
    foreach(header font_awesome_5.h fa_solid_900.h)
        configure_file("${vendorDirectory}/${header}" "${overlayDirectory}/${header}" COPYONLY)
    endforeach()
    # imgui exports the broad ThirdParty include root before other libraries.
    # Prepend here as well so <imgui-notify-v2/imgui_notify.h> selects the overlay.
    target_include_directories(imgui BEFORE PUBLIC "$<BUILD_INTERFACE:${overlayRoot}>")
    target_include_directories(imgui_notify BEFORE INTERFACE
        "$<BUILD_INTERFACE:${overlayRoot}>" "$<BUILD_INTERFACE:${overlayDirectory}>")
    if(TARGET headers)
        target_include_directories(headers BEFORE INTERFACE "$<BUILD_INTERFACE:${overlayRoot}>")
    endif()
endfunction()
