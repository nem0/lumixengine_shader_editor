#pragma once


#include "engine/array.h"
#include "engine/path.h"
#include "engine/string.h"
#include "imgui/imgui.h"


namespace Lumix
{


struct InputMemoryStream;
struct OutputMemoryStream;


struct ShaderEditor {
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
		Node(int type, ShaderEditor& editor);
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

		int m_type;
		ShaderEditor& m_editor;
	};

	explicit ShaderEditor(IAllocator& allocator);
	~ShaderEditor();

	void onGUI();
	const char* getTextureName(int index) const { return m_textures[index]; }
	IAllocator& getAllocator() { return m_allocator; }
	Node* createNode(int type);
	Node& loadNode(InputMemoryStream& blob);
	void saveNode(OutputMemoryStream& blob, Node& node);
	bool hasFocus() const { return m_is_focused; }
	void undo();
	void redo();
	void saveUndo(u16 id);

	static const int MAX_TEXTURES_COUNT = 16;
	bool m_is_open;
	Array<Link> m_links;
	Array<Node*> m_nodes;

private:
	void addNode(Node* node, const ImVec2& pos);
	void destroyNode(Node* node);
	void generate(const char* path, bool save_file);
	void newGraph();
	void save(OutputMemoryStream& blob);
	void save(const char* path);
	void load(InputMemoryStream& blob);
	void load();
	bool canUndo() const;
	bool canRedo() const;

	bool getSavePath();
	void clear();
	void onGUILeftColumn();
	void onGUIRightColumn();
	void onGUIMenu();

	struct Undo;

	IAllocator& m_allocator;
	StaticString<50> m_textures[MAX_TEXTURES_COUNT];
	Path m_path;
	int m_last_node_id;
	int m_undo_stack_idx;
	Array<Undo> m_undo_stack;
	int m_context_link = -1;
	int m_hovered_link = -1;
	bool m_is_focused;
	float m_left_col_width = 100;
	String m_source;
	float m_scale = 1.f;
};


} // namespace Lumix
