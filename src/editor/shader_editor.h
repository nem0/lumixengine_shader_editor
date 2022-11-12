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
		ImU32 color;
	};

	enum class ValueType : i32
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

		virtual void serialize(OutputMemoryStream&blob) {}
		virtual void deserialize(InputMemoryStream&blob) {}
		virtual void printReference(OutputMemoryStream& blob, int output_idx) const;
		virtual ValueType getOutputType(int index) const { return ValueType::FLOAT; }
		virtual bool hasInputPins() const = 0;
		virtual bool hasOutputPins() const = 0;

		bool onNodeGUI();
		void generateOnce(OutputMemoryStream& blob);

		void inputSlot();
		void outputSlot();

		u16 m_id;
		ImVec2 m_pos;
		bool m_selected = false;
		bool m_reachable = false;
		bool m_generated = false;
		u32 m_input_count = 0;
		u32 m_output_count = 0;

		NodeType m_type;
		ShaderEditor& m_editor;

	protected:
		virtual void generate(OutputMemoryStream& blob) {}
		virtual bool onGUI() = 0;
	};

	explicit ShaderEditor(struct StudioApp& app);
	~ShaderEditor();

	void onWindowGUI();
	IAllocator& getAllocator() { return m_allocator; }
	Node* createNode(int type);
	Node& deserializeNode(InputMemoryStream& blob);
	void serializeNode(OutputMemoryStream& blob, Node& node);
	bool hasFocus() override { return m_is_focused; }
	void undo();
	void redo();
	void saveUndo(u16 id);
	Node* addNode(NodeType node_type, ImVec2 pos, bool save_undo);

	static const int MAX_TEXTURES_COUNT = 16;
	bool m_is_open;
	Array<Link> m_links;
	Array<Node*> m_nodes;

private:
	const char* getName() const override { return "shader_editor"; }
	bool isOpen() const { return m_is_open; }

	void onSettingsLoaded() override;
	void onBeforeSettingsSaved() override;
	void destroyNode(Node * node);
	
	void saveSource();
	void generate();
	void generateAndSaveSource();
	void newGraph();
	void serialize(OutputMemoryStream& blob);
	void saveAs(const char* path);
	void save();
	bool load(InputMemoryStream& blob);
	void load();
	void load(const char* path);
	bool canUndo() const;
	bool canRedo() const;

	bool getSavePath();
	void clear();
	void onGUICanvas();
	void onGUIMenu();
	void onToggle();
	void deleteSelectedNodes();
	void markReachableNodes() const;
	void markReachable(Node* node) const;
	void deleteUnreachable();
	void pushRecent(const char* path);
	void colorLinks();
	void colorLinks(ImU32 color, u32 link_ix);

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
	Action m_save_action;
	Action m_undo_action;
	Action m_redo_action;
	Action m_toggle_ui;
	Action m_delete_action;
	Action m_generate_action;
	ImGuiEx::Canvas m_canvas;
	bool m_source_open = false;
	bool m_is_any_item_active = false;
	Array<String> m_recent_paths;
	ImGuiID m_half_link_start = 0;
	ImVec2 m_context_pos;
};


} // namespace Lumix
