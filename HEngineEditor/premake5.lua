project "HEngineEditor"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")
	
	files
	{
		"src/**.h",
		"src/**.cpp",
	}

	includedirs 
	{
		"%{wks.location}/HEngine/vendor/spdlog/include",
		"%{wks.location}/HEngine/src",
		"%{wks.location}/HEngine/vendor",
		"%{IncludeDir.glm}",
		"%{IncludeDir.entt}",
		"%{IncludeDir.ImGuizmo}",
	}

	links
	{
		"HEngine",
	}

	filter "system:windows"
		systemversion "latest"

		defines
		{
			"HE_PLATFORM_WINDOWS",
			"HE_ENABLE_ASSERTS",
		}

	filter "configurations:Debug"
		defines "HE_DEBUG"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		defines "HE_RELEASE"
		runtime "Release"
		optimize "on"

	filter "configurations:Dist"
		defines "HE_DIST"
		runtime "Release"
		optimize "on"
