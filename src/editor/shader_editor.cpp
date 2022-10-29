#define LUMIX_NO_CUSTOM_CRT
#include "shader_editor.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "engine/crt.h"
#include "engine/engine.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/os.h"
#include "engine/path.h"
#include "engine/plugin.h"
#include "engine/stream.h"
#include "engine/string.h"
#include "renderer/model.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "imgui/IconsFontAwesome5.h"
#include <math.h>


namespace Lumix
{

struct ShaderEditor::Undo {
	Undo(IAllocator& allocator) : blob(allocator) {}

	OutputMemoryStream blob;
	u16 id;
};

enum class Version {
	FIRST,
	LAST
};

static constexpr u32 OUTPUT_FLAG = 1 << 31;

// serialized, do not change order
enum class NodeType {
	PBR,

	NUMBER,
	VEC2,
	VEC3,
	VEC4,
	SAMPLE,
	SWIZZLE,
	TIME,
	VERTEX_ID,
	POSITION,
	NORMAL,
	UV0,
	IF,
	APPEND,
	STATIC_SWITCH,
	MIX,

	SCALAR_PARAM,
	VEC4_PARAM,
	COLOR_PARAM,

	MULTIPLY,
	ADD,
	SUBTRACT,
	DIVIDE,

	DOT,
	CROSS,
	MIN,
	MAX,
	POW,
	DISTANCE,
	
	ABS,
	ALL,
	ANY,
	CEIL,
	COS,
	EXP,
	EXP2,
	FLOOR,
	FRACT,
	LOG,
	LOG2,
	NORMALIZE,
	NOT,
	ROUND,
	SATURATE,
	SIN,
	SQRT,
	TAN,
	TRANSPOSE,
	TRUNC,

	FRESNEL,
	LENGTH,
	VIEW_DIR,
	PIXEL_DEPTH,
	SCREEN_POSITION,
	SCENE_DEPTH
};

struct VertexOutput {
	ShaderEditor::ValueType type;
	StaticString<32> name;
};

static constexpr ShaderEditor::ValueType semanticToType(Mesh::AttributeSemantic semantic) {
	switch (semantic) {
		case Mesh::AttributeSemantic::POSITION: return ShaderEditor::ValueType::VEC3;
		case Mesh::AttributeSemantic::COLOR0: return ShaderEditor::ValueType::VEC4;
		case Mesh::AttributeSemantic::COLOR1: return ShaderEditor::ValueType::VEC4;
		case Mesh::AttributeSemantic::INDICES: return ShaderEditor::ValueType::IVEC4;
		case Mesh::AttributeSemantic::WEIGHTS: return ShaderEditor::ValueType::VEC4;
		case Mesh::AttributeSemantic::NORMAL: return ShaderEditor::ValueType::VEC4;
		case Mesh::AttributeSemantic::TANGENT: return ShaderEditor::ValueType::VEC4;
		case Mesh::AttributeSemantic::BITANGENT: return ShaderEditor::ValueType::VEC4;
		case Mesh::AttributeSemantic::TEXCOORD0: return ShaderEditor::ValueType::VEC2;
		case Mesh::AttributeSemantic::TEXCOORD1: return ShaderEditor::ValueType::VEC2;
		case Mesh::AttributeSemantic::INSTANCE0: return ShaderEditor::ValueType::VEC4;
		case Mesh::AttributeSemantic::INSTANCE1: return ShaderEditor::ValueType::VEC4;
		case Mesh::AttributeSemantic::INSTANCE2: return ShaderEditor::ValueType::VEC4;
		default: ASSERT(false); return ShaderEditor::ValueType::VEC4;
	}
}

static constexpr const char* toString(Mesh::AttributeSemantic sem) {

	struct {
		Mesh::AttributeSemantic semantic;
		const char* name;
	} table[] = {
		{ Mesh::AttributeSemantic::POSITION, "position" },
		{ Mesh::AttributeSemantic::NORMAL, "normal" },
		{ Mesh::AttributeSemantic::TANGENT, "tangent" },
		{ Mesh::AttributeSemantic::BITANGENT, "bitangent" },
		{ Mesh::AttributeSemantic::COLOR0, "color 0" },
		{ Mesh::AttributeSemantic::COLOR1, "color 1" },
		{ Mesh::AttributeSemantic::INDICES, "indices" },
		{ Mesh::AttributeSemantic::WEIGHTS, "weights" },
		{ Mesh::AttributeSemantic::TEXCOORD0, "tex coord 0" },
		{ Mesh::AttributeSemantic::TEXCOORD1, "tex coord 1" },
		{ Mesh::AttributeSemantic::INSTANCE0, "instance 0" },
		{ Mesh::AttributeSemantic::INSTANCE1, "instance 1" },
		{ Mesh::AttributeSemantic::INSTANCE2, "instance 2" }
	};

	for (const auto& i : table) {
		if (i.semantic == sem) return i.name;
	}
	ASSERT(false);
	return "Unknown";
}

static constexpr const char* toString(ShaderEditor::ValueType type) {
	switch (type) {
		case ShaderEditor::ValueType::NONE: return "error";
		case ShaderEditor::ValueType::BOOL: return "bool";
		case ShaderEditor::ValueType::INT: return "int";
		case ShaderEditor::ValueType::FLOAT: return "float";
		case ShaderEditor::ValueType::VEC2: return "vec2";
		case ShaderEditor::ValueType::VEC3: return "vec3";
		case ShaderEditor::ValueType::VEC4: return "vec4";
		case ShaderEditor::ValueType::IVEC4: return "ivec4";
		case ShaderEditor::ValueType::MATRIX3: return "mat3";
		case ShaderEditor::ValueType::MATRIX4: return "mat4";
		default: ASSERT(false); return "Unknown type";
	}
}

static const struct NodeTypeDesc {
	const char* group;
	const char* name;
	NodeType type;
} NODE_TYPES[] = {
	{nullptr, "Sample", NodeType::SAMPLE},
	{nullptr, "Vector 4", NodeType::VEC4},
	{nullptr, "Vector 3", NodeType::VEC3},
	{nullptr, "Vector 2", NodeType::VEC2},
	{nullptr, "Number", NodeType::NUMBER},
	{nullptr, "Swizzle", NodeType::SWIZZLE},
	{nullptr, "Time", NodeType::TIME},
	{nullptr, "View direction", NodeType::VIEW_DIR},
	{nullptr, "Pixel depth", NodeType::PIXEL_DEPTH},
	{nullptr, "Scene depth", NodeType::SCENE_DEPTH},
	{nullptr, "Screen position", NodeType::SCREEN_POSITION},
	{nullptr, "Vertex ID", NodeType::VERTEX_ID},
	{nullptr, "Mix", NodeType::MIX},
	{nullptr, "If", NodeType::IF},
	{nullptr, "Static switch", NodeType::STATIC_SWITCH},
	{nullptr, "Append", NodeType::APPEND},
	{nullptr, "Fresnel", NodeType::FRESNEL},
	{nullptr, "Scalar parameter", NodeType::SCALAR_PARAM},
	{nullptr, "Color parameter", NodeType::COLOR_PARAM},
	{nullptr, "Vec4 parameter", NodeType::VEC4_PARAM},

	{"Function", "Abs", NodeType::ABS},
	{"Function", "All", NodeType::ALL},
	{"Function", "Any", NodeType::ANY},
	{"Function", "Ceil", NodeType::CEIL},
	{"Function", "Cos", NodeType::COS},
	{"Function", "Exp", NodeType::EXP},
	{"Function", "Exp2", NodeType::EXP2},
	{"Function", "Floor", NodeType::FLOOR},
	{"Function", "Fract", NodeType::FRACT},
	{"Function", "Log", NodeType::LOG},
	{"Function", "Log2", NodeType::LOG2},
	{"Function", "Normalize", NodeType::NORMALIZE},
	{"Function", "Not", NodeType::NOT},
	{"Function", "Round", NodeType::ROUND},
	{"Function", "Saturate", NodeType::SATURATE},
	{"Function", "Sin", NodeType::SIN},
	{"Function", "Sqrt", NodeType::SQRT},
	{"Function", "Tan", NodeType::TAN},
	{"Function", "Transpose", NodeType::TRANSPOSE},
	{"Function", "Trunc", NodeType::TRUNC},

	{"Function", "Cross", NodeType::CROSS},
	{"Function", "Distance", NodeType::DISTANCE},
	{"Function", "Dot", NodeType::DOT},
	{"Function", "Length", NodeType::LENGTH},
	{"Function", "Max", NodeType::MAX},
	{"Function", "Min", NodeType::MIN},
	{"Function", "Power", NodeType::POW},

	{"Math", "Multiply", NodeType::MULTIPLY},
	{"Math", "Add", NodeType::ADD},
	{"Math", "Subtract", NodeType::SUBTRACT},
	{"Math", "Divide", NodeType::DIVIDE},

	{"Vertex", "Position", NodeType::POSITION},
	{"Vertex", "Normal", NodeType::NORMAL},
	{"Vertex", "UV0", NodeType::UV0}
};


static const struct {
	Mesh::AttributeSemantic semantics;
	const char* gui_name;
}
VERTEX_INPUTS[] = {
	{ Mesh::AttributeSemantic::POSITION,	"Position"			},
	{ Mesh::AttributeSemantic::NORMAL,		"Normal"			},
	{ Mesh::AttributeSemantic::COLOR0,		"Color 0"			},
	{ Mesh::AttributeSemantic::COLOR1,		"Color 1"			},
	{ Mesh::AttributeSemantic::TANGENT,		"Tangent"			},
	{ Mesh::AttributeSemantic::BITANGENT,	"Bitangent"			},
	{ Mesh::AttributeSemantic::INDICES,		"Indices"			},
	{ Mesh::AttributeSemantic::WEIGHTS,		"Weights"			},
	{ Mesh::AttributeSemantic::TEXCOORD0,	"Texture coord 0"	},
	{ Mesh::AttributeSemantic::TEXCOORD1,	"Texture coord 1"	},
	{ Mesh::AttributeSemantic::INSTANCE0,	"Instance data 0"	},
	{ Mesh::AttributeSemantic::INSTANCE1,	"Instance data 1"	},
	{ Mesh::AttributeSemantic::INSTANCE2,	"Instance data 2"	},
};


static constexpr char* FUNCTIONS[] = {
	"abs",
	"all",
	"any",
	"ceil",
	"cos",
	"exp",
	"exp2",
	"floor",
	"fract",
	"log",
	"log2",
	"normalize",
	"not",
	"round",
	"saturate",
	"sin",
	"sqrt",
	"tan",
	"transpose",
	"trunc"
};

static u16 toNodeId(int id) {
	return u16(id);
}

static u16 toAttrIdx(int id) {
	return u16(u32(id) >> 16);
}

template <typename F>
static void	forEachInput(const ShaderEditor& editor, int node_id, const F& f) {
	for (const ShaderEditor::Link& link : editor.m_links) {
		if (toNodeId(link.to) == node_id) {
			const int iter = editor.m_nodes.find([&](const ShaderEditor::Node* node) { return node->m_id == toNodeId(link.from); }); 
			ShaderEditor::Node* from = editor.m_nodes[iter];
			const u16 from_attr = toAttrIdx(link.from);
			const u16 to_attr = toAttrIdx(link.to);
			f(from, from_attr, to_attr, u32(&link - editor.m_links.begin()));
		}
	}
}

struct Input {
	const ShaderEditor::Node* node = nullptr;
	u16 output_idx;

	void printReference(OutputMemoryStream& blob) const { node->printReference(blob, output_idx); }
	operator bool() const { return node != nullptr; }
};

static Input getInput(const ShaderEditor& editor, u16 node_id, u16 input_idx) {
	Input res;
	forEachInput(editor, node_id, [&](const ShaderEditor::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
		if (to_attr == input_idx) {
			res.output_idx = from_attr;
			res.node = from;
		}
	});
	return res;
}

static bool isInputConnected(const ShaderEditor& editor, u16 node_id, u16 input_idx) {
	return getInput(editor, node_id, input_idx).node;
}

void ShaderEditor::Node::printReference(OutputMemoryStream& blob, int output_idx) const
{
	blob << "v" << m_id;
}


ShaderEditor::Node::Node(NodeType type, ShaderEditor& editor)
	: m_type(type)
	, m_editor(editor)
	, m_id(0xffFF)
{
}

struct MixNode : public ShaderEditor::Node {
	explicit MixNode(ShaderEditor& editor)
		: Node(NodeType::MIX, editor)
	{}

	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		ImGui::TextUnformatted("Mix");
		ImGuiEx::EndNodeTitleBar();

		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true); ImGui::TextUnformatted("A");
		ImGuiEx::Pin(m_id | (1 << 16), true); ImGui::TextUnformatted("B");
		ImGuiEx::Pin(m_id | (2 << 16), true); ImGui::TextUnformatted("Weight");
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		return false;
	}

	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		const Input input2 = getInput(m_editor, m_id, 2);
		if (!input0 || !input1 || !input2) return;
		input0.node->generate(blob);
		input1.node->generate(blob);
		input2.node->generate(blob);

		
		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = mix(";
		input0.printReference(blob);
		blob << ", ";
		input1.printReference(blob);
		blob << ", ";
		input2.printReference(blob);
		blob << ");\n";
	}
};

template <NodeType Type>
struct OperatorNode : public ShaderEditor::Node {

	explicit OperatorNode(ShaderEditor& editor)
		: Node(Type, editor)
	{}

	void save(OutputMemoryStream& blob) override { blob.write(b_val); }
	void load(InputMemoryStream& blob) override { blob.read(b_val); }

	ShaderEditor::ValueType getOutputType(int) const override {
		// TODO float * vec4 and others
		return getInputType(0);
	}

	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		if (input0) input0.node->generate(blob);
		if (input1) input1.node->generate(blob);
	}

	void printReference(OutputMemoryStream& blob, int attr_idx) const override
	{
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		if (!input0) {
			blob << "0";
			return;
		}
		
		blob << "(";
		input0.printReference(blob);
		switch(Type) {
			case NodeType::MULTIPLY: blob << " * "; break;
			case NodeType::ADD: blob << " + "; break;
			case NodeType::DIVIDE: blob << " / "; break;
			case NodeType::SUBTRACT: blob << " - "; break;
			default: ASSERT(false); blob << " * "; break;
		}
		if (input1) {
			input1.printReference(blob);
		}
		else {
			blob << b_val;
		}
		blob << ")";
	}

	static const char* getName() {
		switch (Type) {
			case NodeType::ADD: return "Add";
			case NodeType::SUBTRACT: return "Subtract";
			case NodeType::MULTIPLY: return "Multiply";
			case NodeType::DIVIDE: return "Divide";
		}
		ASSERT(false);
		return "Error";
	}

	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		ImGui::TextUnformatted(getName());
		ImGuiEx::EndNodeTitleBar();

		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);

		ImGuiEx::Pin(m_id, true); ImGui::Text("A");

		ImGuiEx::Pin(m_id | (1 << 16), true);
		if (isInputConnected(m_editor, m_id, 1)) {
			ImGui::Text("B");
		}
		else {
			ImGui::DragFloat("B", &b_val);
		}

		return false;
	}

	float b_val = 2;
};

struct SwizzleNode : public ShaderEditor::Node
{
	explicit SwizzleNode(ShaderEditor& editor)
		: Node(NodeType::SWIZZLE, editor)
	{
		m_swizzle = "xyzw";
	}
	
	void save(OutputMemoryStream& blob) override { blob.write(m_swizzle); }
	void load(InputMemoryStream& blob) override { blob.read(m_swizzle); }
	ShaderEditor::ValueType getOutputType(int idx) const override { 
		// TODO other types, e.g. ivec4...
		switch(stringLength(m_swizzle)) {
			case 0: return ShaderEditor::ValueType::NONE;
			case 1: return ShaderEditor::ValueType::FLOAT;
			case 2: return ShaderEditor::ValueType::VEC2;
			case 3: return ShaderEditor::ValueType::VEC3;
			case 4: return ShaderEditor::ValueType::VEC4;
			default: ASSERT(false); return ShaderEditor::ValueType::NONE;
		}
	}
	
	void generate(OutputMemoryStream& blob) const override {
		const Input input = getInput(m_editor, m_id, 0);
		if (!input) return;
		input.node->generate(blob);
	}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
		const Input input = getInput(m_editor, m_id, 0);
		if (!input) return;
		
		input.printReference(blob);
		blob << "." << m_swizzle;
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id, true);
		bool res = ImGui::InputTextWithHint("", "swizzle", m_swizzle.data, sizeof(m_swizzle.data));

		ImGui::SameLine();
		ImGuiEx::Pin(u32(m_id) | OUTPUT_FLAG, false);
		return res;
	}

	StaticString<5> m_swizzle;
};

struct FresnelNode : public ShaderEditor::Node {
	explicit FresnelNode(ShaderEditor& editor)
		: Node(NodeType::FRESNEL, editor)
	{}

	void save(OutputMemoryStream&blob) override {
		blob.write(F0);
		blob.write(power);
	}

	void load(InputMemoryStream&blob) override {
		blob.read(F0);
		blob.read(power);
	}

	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		ImGui::TextUnformatted("Fresnel");
		ImGuiEx::EndNodeTitleBar();

		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::DragFloat("F0", &F0);
		ImGui::DragFloat("Power", &power);
		return false;
	}

	void generate(OutputMemoryStream& blob) const override {
		// TODO use data.normal instead of v_normal
		blob << "float v" << m_id << " = mix(" << F0 << ", 1.0, pow(1 - saturate(dot(-normalize(v_wpos.xyz), v_normal)), " << power << "));\n";
	}

	float F0 = 0.04f;
	float power = 5.0f;
};

template <NodeType Type>
struct FunctionCallNode : public ShaderEditor::Node
{
	explicit FunctionCallNode(ShaderEditor& editor)
		: Node(Type, editor)
	{}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

	ShaderEditor::ValueType getOutputType(int) const override { 
		if (Type == NodeType::LENGTH) return ShaderEditor::ValueType::FLOAT;
		const Input input0 = getInput(m_editor, m_id, 0);
		if (input0) return input0.node->getOutputType(input0.output_idx);
		return ShaderEditor::ValueType::FLOAT;
	}

	static const char* getName() {
		switch (Type) {
			case NodeType::ABS: return "abs";
			case NodeType::ALL: return "all";
			case NodeType::ANY: return "any";
			case NodeType::CEIL: return "ceil";
			case NodeType::COS: return "cos";
			case NodeType::EXP: return "exp";
			case NodeType::EXP2: return "exp2";
			case NodeType::FLOOR: return "floor";
			case NodeType::FRACT: return "fract";
			case NodeType::LENGTH: return "length";
			case NodeType::LOG: return "log";
			case NodeType::LOG2: return "log2";
			case NodeType::NORMALIZE: return "normalize";
			case NodeType::NOT: return "not";
			case NodeType::ROUND: return "round";
			case NodeType::SATURATE: return "saturate";
			case NodeType::SIN: return "sin";
			case NodeType::SQRT: return "sqrt";
			case NodeType::TAN: return "tan";
			case NodeType::TRANSPOSE: return "transpose";
			case NodeType::TRUNC: return "trunc";
			default: ASSERT(false); return "error";
		}
	}

	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);

		if (input0) input0.node->generate(blob);

		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = " << getName() << "(";
		if (input0) {
			input0.printReference(blob);
		}
		else {
			blob << "0";
		}
		blob << ");\n";
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id, true);
		ImGui::TextUnformatted(getName());
		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		return false;
	}
};

template <NodeType Type>
struct BinaryFunctionCallNode : public ShaderEditor::Node
{
	explicit BinaryFunctionCallNode(ShaderEditor& editor)
		: Node(Type, editor)
	{
	}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

	ShaderEditor::ValueType getOutputType(int) const override { 
		switch (Type) {
			case NodeType::DISTANCE:
			case NodeType::DOT: return ShaderEditor::ValueType::FLOAT;
		}
		const Input input0 = getInput(m_editor, m_id, 0);
		if (input0) return input0.node->getOutputType(input0.output_idx);
		return ShaderEditor::ValueType::FLOAT;
	}

	static const char* getName() {
		switch (Type) {
			case NodeType::POW: return "pow";
			case NodeType::DOT: return "dot";
			case NodeType::CROSS: return "cross";
			case NodeType::MIN: return "min";
			case NodeType::MAX: return "max";
			case NodeType::DISTANCE: return "distance";
			default: ASSERT(false); return "error";
		}
	}

	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		if (input0) input0.node->generate(blob);
		if (input1) input1.node->generate(blob);

		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = " << getName() << "(";
		if (input0) {
			input0.printReference(blob);
		}
		else {
			blob << "0";
		}
		blob << ", ";
		if (input1) {
			input1.printReference(blob);
		}
		else {
			blob << "0";
		}
		blob << ");\n";
	}

	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		ImGui::TextUnformatted(getName());
		ImGuiEx::EndNodeTitleBar();
		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true); ImGui::Text("A");
		ImGuiEx::Pin(m_id | (1 << 16), true); ImGui::Text("B");
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		return false;
	}
};


template <NodeType Type>
struct VaryingNode : public ShaderEditor::Node {
	explicit VaryingNode(ShaderEditor& editor)
		: Node(Type, editor)
	{}

	void save(OutputMemoryStream&) override {}
	void load(InputMemoryStream&) override {}

	ShaderEditor::ValueType getOutputType(int) const override { 
		switch(Type) {
			case NodeType::POSITION: return ShaderEditor::ValueType::VEC3;
			case NodeType::NORMAL: return ShaderEditor::ValueType::VEC3;
			case NodeType::UV0: return ShaderEditor::ValueType::VEC2;
			default: ASSERT(false); return ShaderEditor::ValueType::VEC3;
		}
	}

	void printReference(OutputMemoryStream& blob, int output_idx) const {
		switch(Type) {
			case NodeType::POSITION: blob << "v_wpos.xyz"; break;
			case NodeType::NORMAL: blob << "v_normal"; break;
			case NodeType::UV0: blob << "v_uv"; break;
			default: ASSERT(false); break;
		}
		
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		switch(Type) {
			case NodeType::POSITION: ImGui::Text("Position"); break;
			case NodeType::NORMAL: ImGui::Text("Normal"); break;
			case NodeType::UV0: ImGui::Text("UV0"); break;
			default: ASSERT(false); break;
		}
		return false;
	}
};

template <ShaderEditor::ValueType TYPE>
struct ConstNode : public ShaderEditor::Node
{
	explicit ConstNode(ShaderEditor& editor)
		: Node(toNodeType(TYPE), editor)
	{
		m_type = TYPE;
		m_value[0] = m_value[1] = m_value[2] = m_value[3] = 0;
		m_int_value = 0;
	}

	static NodeType toNodeType(ShaderEditor::ValueType t) {
		switch(t) {
			case ShaderEditor::ValueType::VEC4: return NodeType::VEC4;
			case ShaderEditor::ValueType::VEC3: return NodeType::VEC3;
			case ShaderEditor::ValueType::VEC2: return NodeType::VEC2;
			case ShaderEditor::ValueType::FLOAT: return NodeType::NUMBER;
			default: ASSERT(false); return NodeType::NUMBER;
		}
	}

	void save(OutputMemoryStream& blob) override
	{
		blob.write(m_value);
		blob.write(m_is_color);
		blob.write(m_type);
		blob.write(m_int_value);
	}

	void load(InputMemoryStream& blob) override 
	{
		blob.read(m_value);
		blob.read(m_is_color);
		blob.read(m_type);
		blob.read(m_int_value);
	}

	ShaderEditor::ValueType getOutputType(int) const override { return m_type; }

	void printInputValue(u32 idx, OutputMemoryStream& blob) const {
		const Input input = getInput(m_editor, m_id, idx);
		if (input) {
			input.printReference(blob);
			return;
		}
		blob << m_value[idx];
	}

	void generate(OutputMemoryStream& blob) const override {
		for (u32 i = 0; i < 4; ++i) {
			const Input input = getInput(m_editor, m_id, i);
			if (input) input.node->generate(blob);
		}
	}

	void printReference(OutputMemoryStream& blob, int output_idx) const override {
		switch(m_type) {
			case ShaderEditor::ValueType::VEC4:
				blob << "vec4(";
				printInputValue(0, blob);
				blob << ", "; 
				printInputValue(1, blob);
				blob << ", "; 
				printInputValue(2, blob);
				blob << ", "; 
				printInputValue(3, blob);
				blob << ")";
				break;
			case ShaderEditor::ValueType::VEC3:
				blob << "vec3(";
				printInputValue(0, blob);
				blob << ", "; 
				printInputValue(1, blob);
				blob << ", "; 
				printInputValue(2, blob);
				blob << ")";
				break;
			case ShaderEditor::ValueType::VEC2:
				blob << "vec2(";
				printInputValue(0, blob);
				blob << ", "; 
				printInputValue(1, blob);
				blob << ")";
				break;
			case ShaderEditor::ValueType::INT:
				blob << m_int_value;
				break;
			case ShaderEditor::ValueType::FLOAT:
				blob << m_value[0];
				break;
			default: ASSERT(false); break;
		}
	}
	
	bool onGUI() override {
		bool res = false;

		const char* labels[] = { "X", "Y", "Z", "W" };

		ImGui::BeginGroup();
		u32 channels_count = 0;
		switch (m_type) {
			case ShaderEditor::ValueType::VEC4: channels_count = 4; break;
			case ShaderEditor::ValueType::VEC3: channels_count = 3; break;
			case ShaderEditor::ValueType::VEC2: channels_count = 2; break;
			default: channels_count = 1; break;
		}
			
		switch(m_type) {
			case ShaderEditor::ValueType::VEC4:
			case ShaderEditor::ValueType::VEC3:
			case ShaderEditor::ValueType::VEC2:
				for (u16 i = 0; i < channels_count; ++i) {
					ImGuiEx::Pin(m_id | (i << 16), true);
					if (isInputConnected(m_editor, m_id, i)) {
						ImGui::TextUnformatted(labels[i]);
					}
					else {
						res = ImGui::DragFloat(labels[i], &m_value[i]);
					}
				}
				switch (m_type) {
					case ShaderEditor::ValueType::VEC4:
					case ShaderEditor::ValueType::VEC3:
						ImGui::Checkbox("Color", &m_is_color);
						if (m_is_color) {
							res = ImGui::ColorPicker4("##col", m_value) || res; 
						}
						break;
				}
				break;
			case ShaderEditor::ValueType::FLOAT:
				ImGui::SetNextItemWidth(60);
				res = ImGui::DragFloat("##val", m_value) || res;
				break;
			case ShaderEditor::ValueType::INT:
				ImGui::SetNextItemWidth(60);
				res = ImGui::InputInt("##val", &m_int_value) || res;
				break;
			default: ASSERT(false); break;
		}
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);

		return res;
	}

	ShaderEditor::ValueType m_type;
	bool m_is_color = false;
	float m_value[4];
	int m_int_value;
};


struct SampleNode : public ShaderEditor::Node
{
	explicit SampleNode(ShaderEditor& editor)
		: Node(NodeType::SAMPLE, editor)
		, m_texture(editor.getAllocator())
	{}

	void save(OutputMemoryStream& blob) override { blob.writeString(m_texture.c_str()); }
	void load(InputMemoryStream& blob) override { m_texture = blob.readString(); }
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC4; }

	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		if (input0) input0.node->generate(blob);
		blob << "\t\tvec4 v" << m_id << " = ";
		char var_name[64];
		Shader::toTextureVarName(Span(var_name), m_texture.c_str());
		blob << "texture(" << var_name << ", ";
		if (input0) input0.printReference(blob);
		else blob << "v_uv";
		blob << ");\n";
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id, true);
		ImGui::Text("UV");

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		char tmp[128];
		copyString(tmp, m_texture.c_str());
		bool res = ImGui::InputText("Texture", tmp, sizeof(tmp));
		if (res) m_texture = tmp;
		return res;
	}

	String m_texture;
};

struct AppendNode : public ShaderEditor::Node {
	explicit AppendNode(ShaderEditor& editor)
	: Node(NodeType::APPEND, editor)
	{}
	
	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		ImGui::TextUnformatted("Append");
		ImGuiEx::EndNodeTitleBar();

		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true);
		ImGui::TextUnformatted("A");
		ImGuiEx::Pin(m_id | (1 << 16), true);
		ImGui::TextUnformatted("B");
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		return false;
	}

	static u32 getChannelCount(ShaderEditor::ValueType type) {
		switch (type) {
			case ShaderEditor::ValueType::FLOAT:
			case ShaderEditor::ValueType::BOOL:
			case ShaderEditor::ValueType::INT:
				return 1;
			case ShaderEditor::ValueType::VEC2:
				return 2;
			case ShaderEditor::ValueType::VEC3:
				return 3;
			case ShaderEditor::ValueType::IVEC4:
			case ShaderEditor::ValueType::VEC4:
				return 4;
			default:
				// TODO handle mat3 & co.
				ASSERT(false);
				return 1;
		}
	}

	ShaderEditor::ValueType getOutputType(int index) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		u32 count = 0;
		if (input0) count += getChannelCount(input0.node->getOutputType(input0.output_idx));
		if (input1) count += getChannelCount(input1.node->getOutputType(input1.output_idx));
		// TODO other types likes ivec4
		switch (count) {
			case 1: return ShaderEditor::ValueType::FLOAT;
			case 2: return ShaderEditor::ValueType::VEC2;
			case 3: return ShaderEditor::ValueType::VEC3;
			case 4: return ShaderEditor::ValueType::VEC4;
			default: ASSERT(false); return ShaderEditor::ValueType::FLOAT;
		}
	}

	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		if (input0) input0.node->generate(blob);
		const Input input1 = getInput(m_editor, m_id, 1);
		if (input1) input1.node->generate(blob);
	}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		if (!input0 && !input1) blob << "0";
		blob << toString(getOutputType(0)) << "(";
		if (input0) {
			input0.printReference(blob);
			if (input1) blob << ", ";
		}
		if (input1) {
			input1.printReference(blob);
		}
		blob << ")";
	}
};

struct StaticSwitchNode : public ShaderEditor::Node {
	explicit StaticSwitchNode(ShaderEditor& editor)
		: Node(NodeType::STATIC_SWITCH, editor)
		, m_define(editor.getAllocator())
	{}

	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		ImGui::TextUnformatted("Static switch");
		ImGuiEx::EndNodeTitleBar();
		
		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true);
		ImGui::TextUnformatted("True");
		ImGuiEx::Pin(m_id | (1 << 16), true);
		ImGui::TextUnformatted("False");
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		char tmp[128];
		copyString(tmp, m_define.c_str());
		ImGui::SetNextItemWidth(80);
		bool res = ImGui::InputText("##param", tmp, sizeof(tmp));
		if (res) m_define = tmp;
		return res;
	}

	void save(OutputMemoryStream& blob) override { blob.write(m_is_on); }
	
	void load(InputMemoryStream& blob) override { blob.read(m_is_on); }
	
	const char* getOutputTypeName() const {
		const Input input = getInput(m_editor, m_id, m_is_on ? 0 : 1);
		if (!input) return "float";
		return toString(input.node->getOutputType(input.output_idx));
	}

	void generate(OutputMemoryStream& blob) const override {
		blob << "#ifdef " << m_define.c_str() << "\n";
		const Input input0 = getInput(m_editor, m_id, 0);
		if (input0) {
			input0.node->generate(blob);
			blob << getOutputTypeName() << " v" << m_id << " = ";
			input0.printReference(blob);
			blob << ";\n";
		}
		blob << "#else\n";
		const Input input1 = getInput(m_editor, m_id, 1);
		if (input1) {
			input1.node->generate(blob);
			blob << getOutputTypeName() << " v" << m_id << " = "; 
			input1.printReference(blob);
			blob << ";\n";
		}
		blob << "#endif\n";
	}
	
	ShaderEditor::ValueType getOutputType(int) const override {
		const Input input = getInput(m_editor, m_id, m_is_on ? 0 : 1);
		if (input) return input.node->getOutputType(input.output_idx);
		return ShaderEditor::ValueType::FLOAT;
	}

	bool m_is_on = true;
	String m_define;
};

template <NodeType Type>
struct ParameterNode : public ShaderEditor::Node {
	explicit ParameterNode(ShaderEditor& editor)
		: Node(Type, editor)
		, m_name(editor.getAllocator())
	{}

	void save(OutputMemoryStream& blob) { blob.writeString(m_name.c_str()); }
	void load(InputMemoryStream& blob) { m_name = blob.readString(); }

	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		switch(Type) {
			case NodeType::SCALAR_PARAM: ImGui::TextUnformatted("Scalar param"); break;
			case NodeType::VEC4_PARAM: ImGui::TextUnformatted("Vec4 param"); break;
			case NodeType::COLOR_PARAM: ImGui::TextUnformatted("Color param"); break;
			default: ASSERT(false); ImGui::TextUnformatted("Error"); break;
		}
		ImGuiEx::EndNodeTitleBar();
		
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		char tmp[128];
		copyString(tmp, m_name.c_str());
		bool res = ImGui::InputText("##name", tmp, sizeof(tmp));
		if (res) m_name = tmp;
		return res;
	}

	void generate(OutputMemoryStream& blob) const override {
		switch(Type) {
			case NodeType::SCALAR_PARAM: blob << "\tfloat v"; break;
			case NodeType::VEC4_PARAM: blob << "\tvec4 v"; break;
			case NodeType::COLOR_PARAM: blob << "\tvec4 v"; break;
			default: ASSERT(false); blob << "\tfloat v"; break;
		}
		char var_name[64];
		Shader::toUniformVarName(Span(var_name), m_name.c_str());

		blob << m_id << " = " << var_name << ";";
	}

	String m_name;
};

struct PBRNode : public ShaderEditor::Node
{
	explicit PBRNode(ShaderEditor& editor)
		: Node(NodeType::PBR, editor)
	{}

	void save(OutputMemoryStream& blob) {}
	void load(InputMemoryStream& blob) {}

	static void generate(OutputMemoryStream& blob, const Node* node) {
		if (!node) return;
		forEachInput(node->m_editor, node->m_id, [&](const ShaderEditor::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
			generate(blob, from);
		});
		node->generate(blob);
	}

	void generateVS(OutputMemoryStream& blob) const {
		const Input input = getInput(m_editor, m_id, 0);
		generate(blob, input.node);
		if (!input) return;
		input.node->generate(blob);
		blob << "v_wpos = ";
		input.printReference(blob);
		blob << ";";
	}

	void generate(OutputMemoryStream& blob) const override {
		const struct {
			const char* name;
			const char* default_value;
		}
		fields[] = { 
			{ "albedo", "vec3(1, 0, 1)" },
			{ "N", "v_normal" },
			{ "alpha", "1" }, 
			{ "roughness", "1" },
			{ "metallic", "0" },
			{ "emission", "0" },
			{ "ao", "1" },
			{ "translucency", "0" },
			{ "shadow", "1" }
		};

		for (const auto& field : fields) {
			const int i = int(&field - fields);
			Input input = getInput(m_editor, m_id, i);
			if (input) {
				input.node->generate(blob);
				blob << "\tdata." << field.name << " = ";
				if (i < 2) blob << "vec3(";
				input.printReference(blob);
				const ShaderEditor::ValueType type = input.node->getOutputType(input.output_idx);
				if (i == 0) {
					switch(type) {
						case ShaderEditor::ValueType::VEC4: blob << ".rgb"; break;
						case ShaderEditor::ValueType::VEC3: break;
						case ShaderEditor::ValueType::VEC2: blob << ".rgr"; break;
						case ShaderEditor::ValueType::FLOAT: break;
					}
				}
				else if (type != ShaderEditor::ValueType::VEC3 && i < 2) blob << ".rgb";
				else if (type != ShaderEditor::ValueType::FLOAT && i >= 2) blob << ".x";
				if (i < 2) blob << ")";
				blob << ";\n";
			}
			else {
				blob << "\tdata." << field.name << " = " << field.default_value << ";\n";
			}
		}
	}

	bool onGUI() override {
		ImGuiEx::BeginNodeTitleBar();
		ImGui::Text("PBR");
		ImGuiEx::EndNodeTitleBar();
		
		ImGuiEx::Pin(m_id, true);
		ImGui::TextUnformatted("Albedo");

		ImGuiEx::Pin(m_id | (1 << 16), true);
		ImGui::TextUnformatted("Normal");

		ImGuiEx::Pin(m_id | (2 << 16), true);
		ImGui::TextUnformatted("Opacity");

		ImGuiEx::Pin(m_id | (3 << 16), true);
		ImGui::TextUnformatted("Roughness");

		ImGuiEx::Pin(m_id | (4 << 16), true);
		ImGui::TextUnformatted("Metallic");

		ImGuiEx::Pin(m_id | (5 << 16), true);
		ImGui::TextUnformatted("Emission");

		ImGuiEx::Pin(m_id | (6 << 16), true);
		ImGui::TextUnformatted("AO");

		ImGuiEx::Pin(m_id | (7 << 16), true);
		ImGui::TextUnformatted("Translucency");

		ImGuiEx::Pin(m_id | (8 << 16), true);
		ImGui::TextUnformatted("Shadow");

		return false;
	}
};

struct IfNode : public ShaderEditor::Node
{
	explicit IfNode(ShaderEditor& editor)
		: Node(NodeType::IF, editor)
	{
	}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

	void generate(OutputMemoryStream& blob) const override {
		const Input inputA = getInput(m_editor, m_id, 0);
		const Input inputB = getInput(m_editor, m_id, 1);
		const Input inputGT = getInput(m_editor, m_id, 2);
		const Input inputEQ = getInput(m_editor, m_id, 3);
		const Input inputLT = getInput(m_editor, m_id, 4);
		if (!inputA || !inputB) return;
		if (!inputGT && !inputEQ && !inputLT) return;
		
		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << ";\n";
		if (inputGT) {
			blob << "\t\tif(";
			inputA.printReference(blob);
			blob << " > ";
			inputB.printReference(blob);
			blob << ") {\n";
			blob << "\t\t\tv" << m_id << " = ";
			inputGT.printReference(blob);
			blob << ";\n";
			blob << "\t\t}\n";
		}
		if (inputEQ) {
			blob << "\t\tif(";
			inputA.printReference(blob);
			blob << " == ";
			inputB.printReference(blob);
			blob << ") {\n";
			blob << "\t\t\tv" << m_id << " = ";
			inputEQ.printReference(blob);
			blob << ";\n";
			blob << "\t\t}\n";
		}
		if (inputLT) {
			blob << "\t\tif(";
			inputA.printReference(blob);
			blob << " < ";
			inputB.printReference(blob);
			blob << ") {\n";
			blob << "\t\t\tv" << m_id << " = ";
			inputLT.printReference(blob);
			blob << ";\n";
			blob << "\t\t}\n";
		}
	}

	bool onGUI() override {
		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true);
		ImGui::Text("A");
		
		ImGuiEx::Pin(m_id | (1 << 16), true);
		ImGui::Text("B");

		ImGuiEx::Pin(m_id | (2 << 16), true);
		ImGui::Text("A > B");

		ImGuiEx::Pin(m_id | (3 << 16), true);
		ImGui::Text("A == B");

		ImGuiEx::Pin(m_id | (4 << 16), true);
		ImGui::Text("A < B");
		ImGui::EndGroup();

		ImGui::SameLine();

		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::TextUnformatted("Output");

		return false;
	}
};

struct VertexIDNode : ShaderEditor::Node
{
	explicit VertexIDNode(ShaderEditor& editor)
		: Node(NodeType::VERTEX_ID, editor)
	{}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
		blob << "gl_VertexID";
	}

	ShaderEditor::ValueType getOutputType(int) const override
	{
		return ShaderEditor::ValueType::INT;
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::Text("Vertex ID");
		return false;
	}
};

template <NodeType Type>
struct UniformNode : ShaderEditor::Node
{
	explicit UniformNode(ShaderEditor& editor)
		: Node(Type, editor)
	{}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override
	{
		blob << getVarName();
	}

	ShaderEditor::ValueType getOutputType(int) const override
	{
		switch (Type) {
			case NodeType::SCREEN_POSITION: return ShaderEditor::ValueType::VEC2;
			case NodeType::VIEW_DIR: return ShaderEditor::ValueType::VEC3;
		}
		return ShaderEditor::ValueType::FLOAT;
	}

	static const char* getVarName() {
		switch (Type) {
			case NodeType::TIME: return "Global.time";
			case NodeType::VIEW_DIR: return "Pass.view_dir.xyz";
			case NodeType::PIXEL_DEPTH: return "toLinearDepth(Pass.inv_projection, gl_FragCoord.z)";
			case NodeType::SCENE_DEPTH: return "toLinearDepth(Pass.inv_projection, texture(u_depthbuffer, gl_FragCoord.xy / Global.framebuffer_size).x)";
			case NodeType::SCREEN_POSITION: return "(gl_FragCoord.xy / Global.framebuffer_size)";
			default: ASSERT(false); return "Error";
		}
	}

	static const char* getName() {
		switch (Type) {
			case NodeType::TIME: return "Time";
			case NodeType::VIEW_DIR: return "View direction";
			case NodeType::PIXEL_DEPTH: return "Pixel depth";
			case NodeType::SCENE_DEPTH: return "Scene depth";
			case NodeType::SCREEN_POSITION: return "Screen position";
			default: ASSERT(false); return "Error";
		}
	}

	bool onGUI() override
	{
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::TextUnformatted(getName());
		return false;
	}
};

void ShaderEditor::onSettingsLoaded() {
	Settings& settings = m_app.getSettings();
	m_is_open = settings.getValue(Settings::GLOBAL, "is_shader_editor_open", false);
	char tmp[LUMIX_MAX_PATH];
	m_recent_paths.clear();
	for (u32 i = 0; ; ++i) {
		const StaticString<32> key("shader_editor_recent_", i);
		const u32 len = settings.getValue(Settings::LOCAL, key, Span(tmp));
		if (len == 0) break;
		m_recent_paths.emplace(tmp, m_app.getAllocator());
	}
}

void ShaderEditor::onBeforeSettingsSaved() {
	Settings& settings = m_app.getSettings();
	settings.setValue(Settings::GLOBAL, "is_shader_editor_open", m_is_open);
	for (const String& p : m_recent_paths) {
		const u32 i = u32(&p - m_recent_paths.begin());
		const StaticString<32> key("shader_editor_recent_", i);
		settings.setValue(Settings::LOCAL, key, p.c_str());
	}
}

ShaderEditor::ShaderEditor(StudioApp& app)
	: m_allocator(app.getAllocator())
	, m_app(app)
	, m_undo_stack(app.getAllocator())
	, m_source(app.getAllocator())
	, m_links(app.getAllocator())
	, m_nodes(app.getAllocator())
	, m_undo_stack_idx(-1)
	, m_is_focused(false)
	, m_is_open(false)
	, m_recent_paths(app.getAllocator())
{
	newGraph();
	m_undo_action.init(ICON_FA_UNDO "Undo", "Shader editor undo", "shader_editor_undo", ICON_FA_UNDO, os::Keycode::Z, Action::Modifiers::CTRL, true);
	m_undo_action.func.bind<&ShaderEditor::undo>(this);
	m_undo_action.plugin = this;

	m_redo_action.init(ICON_FA_REDO "Redo", "Shader editor redo", "shader_editor_redo", ICON_FA_REDO, os::Keycode::Z, Action::Modifiers::CTRL | Action::Modifiers::SHIFT, true);
	m_redo_action.func.bind<&ShaderEditor::redo>(this);
	m_redo_action.plugin = this;

	m_delete_action.init(ICON_FA_TRASH "Delete", "Shader editor delete", "shader_editor_delete", ICON_FA_TRASH, os::Keycode::DEL, Action::Modifiers::NONE, true);
	m_delete_action.func.bind<&ShaderEditor::deleteSelectedNodes>(this);
	m_delete_action.plugin = this;

	m_toggle_ui.init("Shader Editor", "Toggle shader editor", "shaderEditor", "", true);
	m_toggle_ui.func.bind<&ShaderEditor::onToggle>(this);
	m_toggle_ui.is_selected.bind<&ShaderEditor::isOpen>(this);

	m_app.addWindowAction(&m_toggle_ui);
	m_app.addAction(&m_undo_action);
	m_app.addAction(&m_redo_action);
	m_app.addAction(&m_delete_action);
}

void ShaderEditor::deleteSelectedNodes() {
	if (m_is_any_item_active) return;

	for (i32 i = m_nodes.size() - 1; i >= 0; --i) {
		Node* node = m_nodes[i];
		if (node->m_selected) {
			for (i32 j = m_links.size() - 1; j >= 0; --j) {
				if (toNodeId(m_links[j].from) == node->m_id || toNodeId(m_links[j].to) == node->m_id) {
					m_links.erase(j);
				}
			}

			LUMIX_DELETE(m_allocator, node);
			m_nodes.swapAndPop(i);
		}
	}
	saveUndo(0xffFF);
}

void ShaderEditor::onToggle() { 
	m_is_open = !m_is_open;
}

ShaderEditor::~ShaderEditor()
{
	m_app.removeAction(&m_toggle_ui);
	m_app.removeAction(&m_undo_action);
	m_app.removeAction(&m_redo_action);
	m_app.removeAction(&m_delete_action);
	clear();
}

void ShaderEditor::markReachable(Node* node) const {
	node->m_reachable = true;

	forEachInput(*this, node->m_id, [&](ShaderEditor::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
		markReachable(from);
	});
}

void ShaderEditor::colorLinks(ImU32 color, u32 link_idx) {
	m_links[link_idx].color = color;
	const u32 from_node_id = toNodeId(m_links[link_idx].from);
	for (u32 i = 0, c = m_links.size(); i < c; ++i) {
		if (toNodeId(m_links[i].to) == from_node_id) colorLinks(color, i);
	}
}

void ShaderEditor::colorLinks() {
	const ImU32 colors[] = {
		IM_COL32(0x20, 0x20, 0xA0, 255),
		IM_COL32(0x20, 0xA0, 0x20, 255),
		IM_COL32(0x20, 0xA0, 0xA0, 255),
		IM_COL32(0xA0, 0x20, 0x20, 255),
		IM_COL32(0xA0, 0x20, 0xA0, 255),
		IM_COL32(0xA0, 0xA0, 0x20, 255),
		IM_COL32(0xA0, 0xA0, 0xA0, 255),
	};
	
	for (Link& l : m_links) {
		l.color = IM_COL32(0xA0, 0xA0, 0xA0, 0xFF);
	}

	forEachInput(*this, m_nodes[0]->m_id, [&](ShaderEditor::Node* from, u16 from_attr, u16 to_attr, u32 link_idx) {
		colorLinks(colors[to_attr % lengthOf(colors)], link_idx);
	});
}

void ShaderEditor::markReachableNodes() const {
	for (Node* n : m_nodes) {
		n->m_reachable = false;
	}
	markReachable(m_nodes[0]);
}

void ShaderEditor::generate(const char* sed_path, bool save_file)
{
	markReachableNodes();
	colorLinks();

	OutputMemoryStream blob(m_allocator);
	blob.reserve(32*1024);

	blob << "import \"pipelines/surface_base.inc\"\n\n";

	Array<String> uniforms(m_allocator);
	Array<String> defines(m_allocator);
	Array<String> textures(m_allocator);

	auto add_uniform = [&](auto* n, const char* type) {
		const i32 idx = uniforms.find([&](const String& u) { return u == n->m_name; });
		if (idx < 0) {
			uniforms.emplace(n->m_name.c_str(), m_allocator);
			blob << "uniform(\"" << n->m_name.c_str() << "\", \"" << type << "\")\n";
		}
	};

	auto add_define = [&](StaticSwitchNode* n){
		const i32 idx = defines.find([&](const String& u) { return u == n->m_define; });
		if (idx < 0) {
			defines.emplace(n->m_define.c_str(), m_allocator);
			blob << "define(\"" << n->m_define.c_str() << "\")\n";
		}
	};

	auto add_texture = [&](SampleNode* n){
		const i32 idx = textures.find([&](const String& u) { return u == n->m_texture; });
		if (idx < 0) {
			textures.emplace(n->m_texture.c_str(), m_allocator);
			blob << "{\n"
				<< "\tname = \"" << n->m_texture.c_str() << "\",\n"
				<< "\tdefault_texture = \"textures/common/white.tga\"\n"
				<< "}\n";
		}
	};

	for (Node* n : m_nodes) {
		if (!n->m_reachable) continue;
		switch(n->m_type) {
			case NodeType::SCALAR_PARAM:
				add_uniform((ParameterNode<NodeType::SCALAR_PARAM>*)n, "float");
				break;
			case NodeType::VEC4_PARAM:
				add_uniform((ParameterNode<NodeType::VEC4_PARAM>*)n, "vec4");
				break;
			case NodeType::COLOR_PARAM:
				add_uniform((ParameterNode<NodeType::COLOR_PARAM>*)n, "color");
				break;
			case NodeType::STATIC_SWITCH:
				add_define((StaticSwitchNode*)n);
				break;
		}
	}

	blob << "surface_shader_ex({\n";
	blob << "texture_slots = {\n";
	for (Node* n : m_nodes) {
		if (!n->m_reachable) continue;
		switch(n->m_type) {
			case NodeType::SAMPLE:
				add_texture((SampleNode*)n);
				break;
		}
	}
	blob << "},\n";

	blob << "fragment = [[\n";

	#if 0 // TODO
		((PBRNode*)m_nodes[0])->generateVS(blob);
	#endif	

	m_nodes[0]->generate(blob);

	blob << "]]\n})\n";

	if (save_file) {
		PathInfo fi(sed_path);
		StaticString<LUMIX_MAX_PATH> path(fi.m_dir, fi.m_basename, ".shd");
		os::OutputFile file;
		if (!file.open(path)) {
			logError("Could not create file ", path);
			return;
		}

		if (!file.write(blob.data(), blob.size())) {
			file.close();
			logError("Could not write ", path);
			return;
		}
		file.close();
	}

	m_source.resize((u32)blob.size());
	memcpy(m_source.getData(), blob.data(), m_source.length());
	m_source.getData()[m_source.length()] = '\0';
}


void ShaderEditor::saveNode(OutputMemoryStream& blob, Node& node)
{
	int type = (int)node.m_type;
	blob.write(node.m_id);
	blob.write(type);
	blob.write(node.m_pos);

	node.save(blob);
}

void ShaderEditor::save(const char* path) {
	os::OutputFile file;
	if(!file.open(path)) {
		logError("Could not save shader ", path);
		return;
	}

	OutputMemoryStream blob(m_allocator);
	save(blob);

	bool success = file.write(blob.data(), blob.size());
	file.close();
	if (!success) {
		logError("Could not save shader ", path);
	}

	pushRecent(path);
}

void ShaderEditor::save(OutputMemoryStream& blob) {
	blob.reserve(4096);
	blob.write(u32('_LSE'));
	blob.write(Version::LAST);
	blob.write(m_last_node_id);

	const i32 nodes_count = m_nodes.size();
	blob.write(nodes_count);
	for(auto* node : m_nodes) {
		saveNode(blob, *node);
	}

	const i32 links_count = m_links.size();
	blob.write(links_count);
	for (Link& l : m_links) {
		blob.write(l.from);
		blob.write(l.to);
	}
}

void ShaderEditor::clear()
{
	for (auto* node : m_nodes) {
		LUMIX_DELETE(m_allocator, node);
	}
	m_nodes.clear();
	m_links.clear();

	m_undo_stack.clear();
	m_undo_stack_idx = -1;

	m_last_node_id = 0;
}

ShaderEditor::Node* ShaderEditor::createNode(int type) {
	switch ((NodeType)type) {
		case NodeType::PBR:							return LUMIX_NEW(m_allocator, PBRNode)(*this);
		case NodeType::VEC4:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC4>)(*this);
		case NodeType::VEC3:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC3>)(*this);
		case NodeType::VEC2:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC2>)(*this);
		case NodeType::NUMBER:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::FLOAT>)(*this);
		case NodeType::SAMPLE:						return LUMIX_NEW(m_allocator, SampleNode)(*this);
		case NodeType::MULTIPLY:					return LUMIX_NEW(m_allocator, OperatorNode<NodeType::MULTIPLY>)(*this);
		case NodeType::ADD:							return LUMIX_NEW(m_allocator, OperatorNode<NodeType::ADD>)(*this);
		case NodeType::DIVIDE:						return LUMIX_NEW(m_allocator, OperatorNode<NodeType::DIVIDE>)(*this);
		case NodeType::SUBTRACT:					return LUMIX_NEW(m_allocator, OperatorNode<NodeType::SUBTRACT>)(*this);
		case NodeType::SWIZZLE:						return LUMIX_NEW(m_allocator, SwizzleNode)(*this);
		case NodeType::TIME:						return LUMIX_NEW(m_allocator, UniformNode<NodeType::TIME>)(*this);
		case NodeType::VIEW_DIR:					return LUMIX_NEW(m_allocator, UniformNode<NodeType::VIEW_DIR>)(*this);
		case NodeType::PIXEL_DEPTH:					return LUMIX_NEW(m_allocator, UniformNode<NodeType::PIXEL_DEPTH>)(*this);
		case NodeType::SCENE_DEPTH:					return LUMIX_NEW(m_allocator, UniformNode<NodeType::SCENE_DEPTH>)(*this);
		case NodeType::SCREEN_POSITION:				return LUMIX_NEW(m_allocator, UniformNode<NodeType::SCREEN_POSITION>)(*this);
		case NodeType::VERTEX_ID:					return LUMIX_NEW(m_allocator, VertexIDNode)(*this);
		case NodeType::IF:							return LUMIX_NEW(m_allocator, IfNode)(*this);
		case NodeType::STATIC_SWITCH:				return LUMIX_NEW(m_allocator, StaticSwitchNode)(*this);
		case NodeType::APPEND:						return LUMIX_NEW(m_allocator, AppendNode)(*this);
		case NodeType::FRESNEL:						return LUMIX_NEW(m_allocator, FresnelNode)(*this);
		case NodeType::POSITION:					return LUMIX_NEW(m_allocator, VaryingNode<NodeType::POSITION>)(*this);
		case NodeType::NORMAL:						return LUMIX_NEW(m_allocator, VaryingNode<NodeType::NORMAL>)(*this);
		case NodeType::UV0:							return LUMIX_NEW(m_allocator, VaryingNode<NodeType::UV0>)(*this);
		case NodeType::SCALAR_PARAM:				return LUMIX_NEW(m_allocator, ParameterNode<NodeType::SCALAR_PARAM>)(*this);
		case NodeType::COLOR_PARAM:					return LUMIX_NEW(m_allocator, ParameterNode<NodeType::COLOR_PARAM>)(*this);
		case NodeType::VEC4_PARAM:					return LUMIX_NEW(m_allocator, ParameterNode<NodeType::VEC4_PARAM>)(*this);
		case NodeType::MIX:							return LUMIX_NEW(m_allocator, MixNode)(*this);
		
		case NodeType::ABS:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::ABS>)(*this);
		case NodeType::ALL:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::ALL>)(*this);
		case NodeType::ANY:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::ANY>)(*this);
		case NodeType::CEIL:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::CEIL>)(*this);
		case NodeType::COS:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::COS>)(*this);
		case NodeType::EXP:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::EXP>)(*this);
		case NodeType::EXP2:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::EXP2>)(*this);
		case NodeType::FLOOR:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::FLOOR>)(*this);
		case NodeType::FRACT:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::FRACT>)(*this);
		case NodeType::LOG:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::LOG>)(*this);
		case NodeType::LOG2:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::LOG2>)(*this);
		case NodeType::NORMALIZE:					return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::NORMALIZE>)(*this);
		case NodeType::NOT:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::NOT>)(*this);
		case NodeType::ROUND:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::ROUND>)(*this);
		case NodeType::SATURATE:					return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::SATURATE>)(*this);
		case NodeType::SIN:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::SIN>)(*this);
		case NodeType::SQRT:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::SQRT>)(*this);
		case NodeType::TAN:							return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::TAN>)(*this);
		case NodeType::TRANSPOSE:					return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::TRANSPOSE>)(*this);
		case NodeType::TRUNC:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::TRUNC>)(*this);
		case NodeType::LENGTH:						return LUMIX_NEW(m_allocator, FunctionCallNode<NodeType::LENGTH>)(*this);

		case NodeType::DOT:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::DOT>)(*this);
		case NodeType::CROSS:						return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::CROSS>)(*this);
		case NodeType::MIN:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::MIN>)(*this);
		case NodeType::MAX:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::MAX>)(*this);
		case NodeType::POW:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::POW>)(*this);
		case NodeType::DISTANCE:					return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::DISTANCE>)(*this);
	}

	ASSERT(false);
	return nullptr;
}

ShaderEditor::Node& ShaderEditor::loadNode(InputMemoryStream& blob) {
	int type;
	u16 id;
	blob.read(id);
	blob.read(type);
	Node* node = createNode(type);
	node->m_id = id;
	m_nodes.push(node);
	blob.read(node->m_pos);

	node->load(blob);
	return *node;
}

void ShaderEditor::load() {
	char path[LUMIX_MAX_PATH];
	if (!os::getOpenFilename(Span(path), "Shader edit data\0*.sed\0", nullptr)) return;
	load(path);
}

void ShaderEditor::load(const char* path) {
	m_path = path;

	clear();

	os::InputFile file;
	if (!file.open(path)) {
		logError("Failed to load shader ", path);
		return;
	}

	const u32 data_size = (u32)file.size();
	Array<u8> data(m_allocator);
	data.resize(data_size);
	if (!file.read(&data[0], data_size)) {
		logError("Failed to load shader ", path);
		file.close();
		return;
	}
	file.close();

	InputMemoryStream blob(&data[0], data_size);
	load(blob);

	m_undo_stack.clear();
	m_undo_stack_idx = -1;
	saveUndo(0xffFF);
	pushRecent(path);
}

void ShaderEditor::pushRecent(const char* path) {
	String p(path, m_app.getAllocator());
	m_recent_paths.eraseItems([&](const String& s) { return s == path; });
	m_recent_paths.push(static_cast<String&&>(p));
}

bool ShaderEditor::load(InputMemoryStream& blob) {
	Version version;
	u32 magic;
	blob.read(magic);
	if (magic != '_LSE') return false;
	blob.read(version);
	if (version > Version::LAST) return false;
	blob.read(m_last_node_id);

	int size;
	blob.read(size);
	for(int i = 0; i < size; ++i) {
		loadNode(blob);
	}

	blob.read(size);
	m_links.resize(size);
	for (Link& l : m_links) {
		blob.read(l.from);
		blob.read(l.to);
	}
	markReachableNodes();
	colorLinks();

	return true;
}


bool ShaderEditor::getSavePath()
{
	char path[LUMIX_MAX_PATH];
	if (os::getSaveFilename(Span(path), "Shader edit data\0*.sed\0", "sed"))
	{
		m_path = path;
		return true;
	}
	return false;
}


static ImVec2 operator+(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x + b.x, a.y + b.y);
}


static ImVec2 operator-(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x - b.x, a.y - b.y);
}

void ShaderEditor::addNode(NodeType node_type, ImVec2 pos) {
	Node* n = createNode((int)node_type);
	n->m_id = ++m_last_node_id;
	n->m_pos = pos;
	m_nodes.push(n);
	saveUndo(0xffFF);
}

static void nodeGroupUI(ShaderEditor& editor, Span<const NodeTypeDesc> nodes, ImVec2 pos) {
	if (nodes.length() == 0) return;

	const NodeTypeDesc* n = nodes.begin();
	const char* group = n->group;

	bool open = !group || ImGui::BeginMenu(group);
	while (n != nodes.end() && n->group == nodes.begin()->group) {
		if (open && ImGui::MenuItem(n->name)) {
			editor.addNode(n->type, pos);
		}
		++n;
	}
	if (open && group) ImGui::EndMenu();

	nodeGroupUI(editor, Span(n, nodes.end()), pos);
}

void ShaderEditor::onGUICanvas()
{
	ImGui::BeginChild("canvas");

	m_canvas.begin();

	static ImVec2 offset = ImVec2(0, 0);
	ImGuiEx::BeginNodeEditor("shader_editor", &offset);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
		
	for (Node*& node : m_nodes) {
		const bool reachable = node->m_reachable;
		if (!reachable) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
		
		const ImVec2 old_pos = node->m_pos;
		ImGuiEx::BeginNode(node->m_id, node->m_pos, &node->m_selected);
		if (node->onGUI()) {
			saveUndo(node->m_id);
		}
		ImGuiEx::EndNode();
		if (old_pos.x != node->m_pos.x || old_pos.y != node->m_pos.y) {
			saveUndo(node->m_id);
		}
		if (!reachable) ImGui::PopStyleVar();
	}

	i32 hovered_link = -1;
	for (i32 i = 0, c = m_links.size(); i < c; ++i) {
		const Link& link = m_links[i];
		ImGuiEx::NodeLinkEx(link.from | OUTPUT_FLAG, link.to, link.color, ImGui::GetColorU32(ImGuiCol_TabActive));
		if (ImGuiEx::IsLinkHovered()) {
			hovered_link = i;
		}
	}

	{
		ImGuiID start_attr, end_attr;
		if (ImGuiEx::GetNewLink(&start_attr, &end_attr)) {
			if (start_attr & OUTPUT_FLAG) {
				m_links.push({u32(start_attr) & ~OUTPUT_FLAG, u32(end_attr)});
			} else {
				m_links.push({u32(start_attr), u32(end_attr) & ~OUTPUT_FLAG});
			}
			saveUndo(0xffFF);
		}
	}

	ImGuiEx::EndNodeEditor();
 
	const ImVec2 mp = ImGui::GetMousePos() - origin - offset;
	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
		if (ImGui::GetIO().KeyAlt && hovered_link != -1) {
			m_links.erase(hovered_link);
			saveUndo(0xffFF);
		}
		else {
			static const struct {
				char key;
				NodeType type;
			} types[] = {
				{ 'A', NodeType::ADD },
				{ 'C', NodeType::CROSS},
				{ 'D', NodeType::DOT },
				{ 'F', NodeType::FRACT },
				{ 'I', NodeType::IF },
				{ 'N', NodeType::NORMALIZE },
				{ 'M', NodeType::MULTIPLY },
				{ 'P', NodeType::SCALAR_PARAM },
				{ 'S', NodeType::SATURATE },
				{ 'T', NodeType::SAMPLE },
				{ 'U', NodeType::UV0 },
				{ '1', NodeType::NUMBER},
				{ '2', NodeType::VEC2 },
				{ '3', NodeType::VEC3 },
				{ '4', NodeType::VEC4 },
			};
			for (const auto& t : types) {
				if (os::isKeyDown((os::Keycode)t.key)) {
					addNode(t.type, mp);
					break;
				}
			}
		}
	}

	bool open_context = false;
	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
		ImGui::OpenPopup("context_menu");
		open_context = true;
	}

	if(ImGui::BeginPopup("context_menu")) {
		static char filter[64] = "";
		if (ImGui::MenuItem("Reset zoom")) m_canvas.m_scale = ImVec2(1, 1);
		ImGui::SetNextItemWidth(150);
		if (open_context) ImGui::SetKeyboardFocusHere();
		ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
		if (filter[0]) {
			for (const auto& node_type : NODE_TYPES) {
				if (stristr(node_type.name, filter)) {
					if (ImGui::MenuItem(node_type.name)) {
						addNode(node_type.type, mp);
						filter[0] = '\0';
					}
				}
			}
		}
		else {
			nodeGroupUI(*this, Span(NODE_TYPES), mp);
		}

		ImGui::EndPopup();
	}		

	m_is_any_item_active = ImGui::IsAnyItemActive();

	m_canvas.end();

	ImGui::EndChild();
}


void ShaderEditor::saveUndo(u16 id) {
	while (m_undo_stack.size() > m_undo_stack_idx + 1) m_undo_stack.pop();

	Undo u(m_allocator);
	u.id = id;
	save(u.blob);
	if (id == 0xffFF || m_undo_stack.back().id != id) {
		m_undo_stack.push(static_cast<Undo&&>(u));
		++m_undo_stack_idx;
	}
	else {
		m_undo_stack.back() = static_cast<Undo&&>(u);
	}
	generate("", false);
}

bool ShaderEditor::canUndo() const { return m_undo_stack_idx > 0; }
bool ShaderEditor::canRedo() const { return m_undo_stack_idx < m_undo_stack.size() - 1; }

void ShaderEditor::undo() {
	if (m_undo_stack_idx <= 0) return;
	
	for (auto* node : m_nodes) {
		LUMIX_DELETE(m_allocator, node);
	}
	m_nodes.clear();

	load(InputMemoryStream(m_undo_stack[m_undo_stack_idx - 1].blob));
	--m_undo_stack_idx;
}

void ShaderEditor::redo() {
	if (m_undo_stack_idx + 1 >= m_undo_stack.size()) return;
	
	for (auto* node : m_nodes) {
		LUMIX_DELETE(m_allocator, node);
	}
	m_nodes.clear();

	load(InputMemoryStream(m_undo_stack[m_undo_stack_idx + 1].blob));
	++m_undo_stack_idx;
}

void ShaderEditor::destroyNode(Node* node) {
	for (i32 i = m_links.size() - 1; i >= 0; --i) {
		if (toNodeId(m_links[i].from) == node->m_id || toNodeId(m_links[i].to) == node->m_id) {
			m_links.swapAndPop(i);
		}
	}

	LUMIX_DELETE(m_allocator, node);
	m_nodes.eraseItem(node);
}

void ShaderEditor::newGraph() {
	clear();

	m_last_node_id = 0;
	m_path = "";
	
	m_nodes.push(LUMIX_NEW(m_allocator, PBRNode)(*this));
	m_nodes.back()->m_pos.x = 50;
	m_nodes.back()->m_pos.y = 50;
	m_nodes.back()->m_id = ++m_last_node_id;

	m_undo_stack.clear();
	m_undo_stack_idx = -1;
	saveUndo(0xffFF);
}

void ShaderEditor::onGUIMenu()
{
	if(ImGui::BeginMenuBar()) {
		if(ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New")) newGraph();
			ImGui::MenuItem("View source", nullptr, &m_source_open);
			if (ImGui::MenuItem("Open")) load();
			if (ImGui::MenuItem("Save")) {
				if (m_path.isEmpty()) {
					if(getSavePath() && !m_path.isEmpty()) save(m_path.c_str());
				}
				else {
					save(m_path.c_str());
				}
			}
			if (ImGui::MenuItem("Save as")) {
				if(getSavePath() && !m_path.isEmpty()) save(m_path.c_str());
			}

			if (ImGui::BeginMenu("Recent", !m_recent_paths.empty())) {
				for (const String& s : m_recent_paths) {
					if (ImGui::MenuItem(s.c_str())) load(s.c_str());
				}
				ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit")) {
			menuItem(m_undo_action, canUndo());
			menuItem(m_redo_action, canRedo());
			if (ImGui::MenuItem(ICON_FA_BRUSH "Clean")) deleteUnreachable();
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Generate & save", nullptr, false, !m_path.isEmpty())) {
			generate(m_path.c_str(), true);
		}

		ImGui::EndMenuBar();
	}
}

void ShaderEditor::deleteUnreachable() {
	markReachableNodes();
	colorLinks();
	for (i32 i = m_nodes.size() - 1; i >= 0; --i) {
		Node* node = m_nodes[i];
		if (!node->m_reachable) {
			for (i32 j = m_links.size() - 1; j >= 0; --j) {
				if (toNodeId(m_links[j].from) == node->m_id || toNodeId(m_links[j].to) == node->m_id) {
					m_links.erase(j);
				}
			}

			LUMIX_DELETE(m_allocator, node);
			m_nodes.swapAndPop(i);
		}
	}
	saveUndo(0xffFF);
}

void ShaderEditor::onWindowGUI()
{
	if (m_source_open) {
		ImGui::SetNextWindowSize(ImVec2(300, 300), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Shader source", &m_source_open)) {
			if (m_source.length() == 0) {
				ImGui::Text("Empty");
			} else {
				ImGui::SetNextItemWidth(-1);
				ImGui::InputTextMultiline("##src", m_source.getData(), m_source.length(), ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiInputTextFlags_ReadOnly);
			}
		}
		ImGui::End();
	}

	m_is_focused = false;
	if (!m_is_open) return;

	StaticString<LUMIX_MAX_PATH + 25> title("Shader Editor");
	if (!m_path.isEmpty()) title << " - " << m_path.c_str();
	title << "###Shader Editor";

	if (ImGui::Begin(title, &m_is_open, ImGuiWindowFlags_MenuBar))
	{
		m_is_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		onGUIMenu();
		onGUICanvas();
	}
	ImGui::End();
}


} // namespace Lumix