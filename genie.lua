if plugin "shader_editor" then
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	defines { "BUILDING_SHADER_EDITOR" }
	dynamic_link_plugin { "editor", "engine", "renderer", "core" }
	if build_studio then
		dynamic_link_plugin { "editor" }
	end
end