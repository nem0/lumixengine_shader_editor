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


struct ShaderEditorResource {
	using Link = NodeEditorLink;

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

	struct Node : NodeEditorNode {
		Node(NodeType type, ShaderEditorResource& resource);
		virtual ~Node() {}

		virtual void serialize(OutputMemoryStream&blob) {}
		virtual void deserialize(InputMemoryStream&blob) {}
		virtual void printReference(OutputMemoryStream& blob, int output_idx) const;
		virtual ValueType getOutputType(int index) const { return ValueType::FLOAT; }

		bool nodeGUI() override;
		void generateOnce(OutputMemoryStream& blob);

		void inputSlot();
		void outputSlot();

		bool m_selected = false;
		bool m_reachable = false;
		bool m_generated = false;
		u32 m_input_count = 0;
		u32 m_output_count = 0;

		NodeType m_type;
		ShaderEditorResource& m_resource;

	protected:
		virtual void generate(OutputMemoryStream& blob) {}
		virtual bool onGUI() = 0;
	};

	ShaderEditorResource(StudioApp& app);
	~ShaderEditorResource();

	Node* createNode(int type);
	Node& deserializeNode(InputMemoryStream& blob);
	static void serializeNode(OutputMemoryStream& blob, Node& node);

	void markReachable(Node* node) const;
	void destroyNode(Node * node);
	void deleteSelectedNodes();
	void colorLinks();
	void colorLinks(ImU32 color, u32 link_ix);
	void markReachableNodes() const;
	void serialize(OutputMemoryStream& blob);
	bool deserialize(InputMemoryStream& blob);
	void deleteUnreachable();
	String generate();

	StudioApp& m_app;
	IAllocator& m_allocator;
	Array<Link> m_links;
	Array<Node*> m_nodes;
	int m_last_node_id = 0;
};

struct ShaderEditor : public StudioApp::GUIPlugin, NodeEditor {
	using Node = ShaderEditorResource::Node;
	using Link = NodeEditorLink;

	explicit ShaderEditor(struct StudioApp& app);
	~ShaderEditor();

	void onWindowGUI();
	IAllocator& getAllocator() { return m_allocator; }
	bool hasFocus() override { return m_is_focused; }
	void pushUndo(u32 tag) override;
	ShaderEditorResource::Node* addNode(NodeType node_type, ImVec2 pos, bool save_undo);
	ShaderEditorResource* getResource() { return m_resource; }

	static const int MAX_TEXTURES_COUNT = 16;
	bool m_is_open;

private:
	const char* getName() const override { return "shader_editor"; }
	bool isOpen() const { return m_is_open; }

	void onSettingsLoaded() override;
	void onBeforeSettingsSaved() override;
	
	void saveSource();
	void generateAndSaveSource();
	void newGraph(bool is_particle_shader);
	void saveAs(const char* path);
	void save();
	void load(const char* path);

	void deserialize(InputMemoryStream& blob) override;
	void serialize(OutputMemoryStream& blob) override;

	void onCanvasClicked(ImVec2 pos, i32 hovered_link) override;
	void onLinkDoubleClicked(Link& link, ImVec2 pos) override;
	void onContextMenu(ImVec2 pos) override;

	void clear();
	void onGUIMenu();
	void onToggle();
	void deleteSelectedNodes();
	void deleteUnreachable();
	void pushRecent(const char* path);

	StudioApp& m_app;
	IAllocator& m_allocator;
	ImVec2 m_canvas_offset = ImVec2(0, 0);
	Path m_path;
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
	Array<String> m_recent_paths;
	bool m_show_save_as = false;
	bool m_show_open = false;

	ShaderEditorResource* m_resource = nullptr;
};


} // namespace Lumix
