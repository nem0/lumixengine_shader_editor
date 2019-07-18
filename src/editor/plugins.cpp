#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "shader_editor.h"

namespace Lumix {

	
struct ShaderEditorPlugin final : public StudioApp::GUIPlugin
{
	explicit ShaderEditorPlugin(StudioApp& app)
		: m_shader_editor(app.getWorldEditor().getAllocator())
		, m_app(app)
	{
		Action* action = LUMIX_NEW(app.getWorldEditor().getAllocator(), Action)(
			"Shader Editor", "Toggle shader editor", "shaderEditor");
		action->func.bind<ShaderEditorPlugin, &ShaderEditorPlugin::onAction>(this);
		action->is_selected.bind<ShaderEditorPlugin, &ShaderEditorPlugin::isOpen>(this);
		app.addWindowAction(action);
		m_shader_editor.m_is_open = false;
	}


	const char* getName() const override { return "shader_editor"; }
	void onAction() { m_shader_editor.m_is_open = !m_shader_editor.m_is_open; }
	void onWindowGUI() override { m_shader_editor.onGUI(); }
	bool hasFocus() override { return m_shader_editor.hasFocus(); }
	bool isOpen() const { return m_shader_editor.m_is_open; }
	void onSettingsLoaded() override {
		m_shader_editor.m_is_open = m_app.getSettings().getValue("is_shader_editor_open", false);
	}
	void onBeforeSettingsSaved() override {
		m_app.getSettings().setValue("is_shader_editor_open", m_shader_editor.m_is_open);
	}


	StudioApp& m_app;
	ShaderEditor m_shader_editor;
};


LUMIX_STUDIO_ENTRY(shader_editor)
{
	WorldEditor& editor = app.getWorldEditor();

	auto* plugin = LUMIX_NEW(editor.getAllocator(), ShaderEditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}


}