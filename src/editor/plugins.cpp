#define LUMIX_NO_CUSTOM_CRT
#include "imgui/imgui.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "shader_editor.h"

namespace Lumix {

	
LUMIX_STUDIO_ENTRY(shader_editor)
{
	auto* plugin = LUMIX_NEW(app.getAllocator(), ShaderEditor)(app);
	app.addPlugin(*plugin);
	return nullptr;
}


}