project "shader_editor"
	libType()
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	defines { "BUILDING_SHADER_EDITOR" }
	links { "editor", "engine", "renderer" }
	useLua()
	defaultConfigurations()
	
linkPlugin("shader_editor")
