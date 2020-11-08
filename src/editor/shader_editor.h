#pragma once


#include "engine/array.h"
#include "engine/path.h"
#include "engine/string.h"
#include "imgui/imgui.h"


namespace Lumix
{


struct InputMemoryStream;
struct OutputMemoryStream;


class ShaderEditor
{
public:
	struct Link {
		u32 from;
		u32 to;
	};

	struct Node;

	struct Stage {
		Stage(IAllocator& allocator) 
			: nodes(allocator)
			, links(allocator)
		{}

		Array<Node*> nodes;
		Array<Link> links;
	};

	enum class ShaderType
	{
		VERTEX,
		FRAGMENT,

		COUNT
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


	struct Node
	{
		Node(int type, ShaderEditor& editor);
		virtual ~Node() {}

		virtual void save(OutputMemoryStream&blob) {}
		virtual void load(InputMemoryStream&blob) {}
		virtual void generate(OutputMemoryStream&blob, const Stage& stage) const {}
		virtual void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const;
		virtual void generateBeforeMain(OutputMemoryStream&blob) const {}
		virtual ValueType getOutputType(int index) const { return ValueType::FLOAT; }
		virtual ValueType getInputType(int index) const { return ValueType::FLOAT; }
		virtual void onGUI(Stage& stage) = 0;

		u16 m_id;
		ImVec2 m_pos;

		int m_type;
		ShaderEditor& m_editor;

	protected:
	};

public:
	explicit ShaderEditor(IAllocator& allocator);
	~ShaderEditor();

	void onGUI();
	const char* getTextureName(int index) const { return m_textures[index]; }
	IAllocator& getAllocator() { return m_allocator; }
	Node* createNode(int type);
	void addNode(Node* node, const ImVec2& pos, ShaderType type);
	void destroyNode(Node* node);
	Node* getNodeByID(int id);
	Node& loadNode(InputMemoryStream& blob, ShaderType type);
	void loadNodeConnections(InputMemoryStream& blob, Node& node);
	void saveNode(OutputMemoryStream& blob, Node& node);
	void saveNodeConnections(OutputMemoryStream& blob, Node& node);
	bool hasFocus() const { return m_is_focused; }
	void undo();
	void redo();
	void saveUndo();

public:
	static const int MAX_TEXTURES_COUNT = 16;

	bool m_is_open;

private:
	void generatePasses(OutputMemoryStream& blob);
	void generate(const char* path, bool save_file);
	void newGraph();
	void save(const char* path);
	void load();
	bool canUndo() const;
	bool canRedo() const;

	void createConnection(Node* node, int pin_index, bool is_input);
	bool getSavePath();
	void clear();
	void onGUILeftColumn();
	void onGUIRightColumn();
	void onGUIMenu();

private:
	StaticString<50> m_textures[MAX_TEXTURES_COUNT];
	Path m_path;
	int m_last_node_id;
	int m_undo_stack_idx;
	Array<OutputMemoryStream> m_undo_stack;
	Stage m_vertex_stage;
	Stage m_fragment_stage;
	int m_context_link = -1;
	int m_hovered_link = -1;
	IAllocator& m_allocator;
	bool m_is_focused;
	float m_left_col_width = 100;
	String m_source;
	float m_scale = 1.f;
};


} // namespace Lumix
