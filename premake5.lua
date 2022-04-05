include "./vendor/premake/premake_customization/solution_items.lua"

workspace "HEngine"
	architecture "x86_64"
	startproject "HEngineEditor"

	configurations
	{
		"Debug",
		"Release",
		"Dist"
	}

	solution_items
	{
		".editorconfig"
	}

	flags
	{
		"MultiProcessorCompile"
	}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories relative to root folder (solution directory)
IncludeDir = {}
IncludeDir["GLFW"] =		"%{wks.location}/HEngine/vendor/GLFW/include"
IncludeDir["Glad"] =		"%{wks.location}/HEngine/vendor/Glad/include"
IncludeDir["ImGui"] =		"%{wks.location}/HEngine/vendor/imgui"
IncludeDir["glm"] =			"%{wks.location}/HEngine/vendor/glm"
IncludeDir["stb_image"] =   "%{wks.location}/HEngine/vendor/stb_image"
IncludeDir["entt"] =		"%{wks.location}/HEngine/vendor/entt/include"
IncludeDir["yaml_cpp"] =	"%{wks.location}/HEngine/vendor/yaml-cpp/include"
IncludeDir["ImGuizmo"] =	"%{wks.location}/HEngine/vendor/ImGuizmo"

group "Dependencies"
	include "vendor/premake"
	include "HEngine/vendor/GLFW"
	include "HEngine/vendor/Glad"
	include "HEngine/vendor/imgui"
	include "HEngine/vendor/yaml-cpp"
group ""

include "HEngine"
include "Sandbox"
include "HEngineEditor"
