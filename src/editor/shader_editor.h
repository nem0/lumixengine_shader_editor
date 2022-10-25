#pragma once


#include "engine/array.h"
#include "engine/path.h"
#include "engine/string.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "renderer/gpu/gpu.h"
#include "imgui/imgui.h"


namespace Lumix
{


struct InputMemoryStream;
struct OutputMemoryStream;
enum class NodeType;


// TODO merge with ShaderEditorPlugin - this should make undo shortcut work
struct ShaderEditor : public StudioApp::GUIPlugin {
	struct Link {
		u32 from;
		u32 to;
	};

	enum class ValueType : int
	{
		BOOL,
		FLOAT,
		INT,
		VEC2,
		VEC3,
		VEC4,
		IVEC4,
		MATRIX3,
		MATRIX4,

		COUNT,
		NONE
	};

	struct Node {
		Node(NodeType type, ShaderEditor& editor);
		virtual ~Node() {}

		virtual void save(OutputMemoryStream&blob) {}
		virtual void load(InputMemoryStream&blob) {}
		virtual void generate(OutputMemoryStream&blob) const {}
		virtual void printReference(OutputMemoryStream& blob, int output_idx) const;
		virtual ValueType getOutputType(int index) const { return ValueType::FLOAT; }
		virtual ValueType getInputType(int index) const { return ValueType::FLOAT; }
		virtual bool onGUI() = 0;

		u16 m_id;
		ImVec2 m_pos;
		bool m_selected = false;
		bool m_reachable = false;

		NodeType m_type;
		ShaderEditor& m_editor;
	};

	explicit ShaderEditor(struct StudioApp& app);
	~ShaderEditor();

	void onWindowGUI();
	IAllocator& getAllocator() { return m_allocator; }
	Node* createNode(int type);
	Node& loadNode(InputMemoryStream& blob);
	void saveNode(OutputMemoryStream& blob, Node& node);
	bool hasFocus() override { return m_is_focused; }
	void undo();
	void redo();
	void saveUndo(u16 id);
	void addNode(NodeType node_type, ImVec2 pos);

	static const int MAX_TEXTURES_COUNT = 16;
	bool m_is_open;
	Array<Link> m_links;
	Array<Node*> m_nodes;

private:
	const char* getName() const override { return "shader_editor"; }
	bool isOpen() const { return m_is_open; }

	void destroyNode(Node * node);
	void generate(const char* path, bool save_file);
	void newGraph();
	void save(OutputMemoryStream& blob);
	void save(const char* path);
	bool load(InputMemoryStream& blob);
	void load();
	bool canUndo() const;
	bool canRedo() const;

	bool getSavePath();
	void clear();
	void onGUICanvas();
	void onGUIMenu();
	void onToggle();
	void deleteSelectedNode();
	void markReachableNodes() const;
	void markReachable(Node* node) const;

	void onSettingsLoaded() override;
	void onBeforeSettingsSaved() override;

	struct Undo;

	StudioApp& m_app;
	IAllocator& m_allocator;
	ImVec2 m_canvas_offset = ImVec2(0, 0);
	Path m_path;
	int m_last_node_id;
	int m_undo_stack_idx;
	Array<Undo> m_undo_stack;
	bool m_is_focused;
	String m_source;
	Action m_undo_action;
	Action m_redo_action;
	Action m_toggle_ui;
	Action m_delete_action;
	ImGuiEx::Canvas m_canvas;
	bool m_source_open = false;
};


} // namespace Lumix
