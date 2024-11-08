project "shader_editor"
	libType()
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	defines { "BUILDING_SHADER_EDITOR" }
	links { "editor", "engine", "renderer", "core" }
	if build_studio then
		links { "editor" }
	end
	defaultConfigurations()
	
linkPlugin("shader_editor")
