#define LUMIX_NO_CUSTOM_CRT
#include "shader_editor.h"
#include "editor/utils.h"
#include "engine/crc32.h"
#include "engine/crt.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/os.h"
#include "engine/path.h"
#include "engine/stream.h"
#include "engine/string.h"
#include "renderer/model.h"
#include "imnodes.h"
#include "imgui/IconsFontAwesome5.h"
#include <math.h>


namespace Lumix
{

static constexpr u32 OUTPUT_FLAG = 1 << 31;

enum class NodeType
{
	VERTEX_INPUT,
	VERTEX_OUTPUT,

	FRAGMENT_INPUT,
	FRAGMENT_OUTPUT,

	CONSTANT,
	SAMPLE,
	MIX,
	UNIFORM,
	VEC4_MERGE,
	SWIZZLE,
	OPERATOR,
	BUILTIN_UNIFORM,
	VERTEX_ID,
	PASS,
	INSTANCE_MATRIX,
	FUNCTION_CALL,
	BINARY_FUNCTION_CALL,
	IF,
	VERTEX_PREFAB
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


static const struct { const char* name; NodeType type; bool is_frag; bool is_vert; bool can_user_create; } NODE_TYPES[] = {
	{"Mix",					NodeType::MIX,					true,		true,	true},
	{"Sample",				NodeType::SAMPLE,				true,		true,	true},
	{"Input",				NodeType::VERTEX_INPUT,			false,		true,	false},
	{"Output",				NodeType::VERTEX_OUTPUT,		false,		true,	false},
	{"Input",				NodeType::FRAGMENT_INPUT,		true,		false,	false},
	{"Output",				NodeType::FRAGMENT_OUTPUT,		true,		false,	false},
	{"Constant",			NodeType::CONSTANT,				true,		true,	true},
	{"Uniform",				NodeType::UNIFORM,				true,		true,	true},
	{"Vec4 merge",			NodeType::VEC4_MERGE,			true,		true,	true},
	{"Swizzle",				NodeType::SWIZZLE,				true,		true,	true},
	{"Operator",			NodeType::OPERATOR,				true,		true,	true},
	{"Builtin uniforms",	NodeType::BUILTIN_UNIFORM,		true,		true,	true},
	{"Vertex ID",			NodeType::VERTEX_ID,			false,		true,	true},
	{"Pass",				NodeType::PASS,					true,		true,	true},
	{"Instance matrix",		NodeType::INSTANCE_MATRIX,		false,		true,	true},
	{"Function",			NodeType::FUNCTION_CALL,		true,		true,	true},
	{"Binary function",		NodeType::BINARY_FUNCTION_CALL,	true,		true,	true},
	{"If",					NodeType::IF,					true,		true,	true},
	{"Vertex prefab",		NodeType::VERTEX_PREFAB,		false,		true,	true}
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


static const struct { const char* gui_name;  const char* name; ShaderEditor::ValueType type; } BUILTIN_UNIFORMS[] =
{
	{ "Model matrix",		"u_model[0]",				ShaderEditor::ValueType::MATRIX4 },
	{ "View & Projection",	"u_pass_view_projection",	ShaderEditor::ValueType::MATRIX4 },
	{ "Time",				"u_time",					ShaderEditor::ValueType::FLOAT },
};


static const struct
{
	const char* name;
	ShaderEditor::ValueType(*output_type)(const ShaderEditor::Node& node);
} BINARY_FUNCTIONS[] = {
	{ "dot",		[](const ShaderEditor::Node&){ return ShaderEditor::ValueType::FLOAT; } },
	{ "cross",		[](const ShaderEditor::Node& node){ return node.getInputType(0); } },
	{ "min",		[](const ShaderEditor::Node& node){ return node.getInputType(0); } },
	{ "max",		[](const ShaderEditor::Node& node){ return node.getInputType(0); } },
	{ "distance",	[](const ShaderEditor::Node&){ return ShaderEditor::ValueType::FLOAT; } }
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
	"inverse",
	"log",
	"log2",
	"normalize",
	"not",
	"round",
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

static void removeConnection(ShaderEditor::Stage& stage, ShaderEditor::Node* node, u16 attr_idx, bool is_input)
{
	for (i32 i = 0; i < stage.links.size(); ++i) {
		ShaderEditor::Link& link = stage.links[i];
		if (toNodeId(is_input ? link.to : link.from) == node->m_id) {
			int l = is_input ? link.to : link.from;
			const u32 x = toAttrIdx(l);
			if (x > attr_idx) {
				l = (l & 0xffFF) | ((x - 1) << 16);
				is_input ? link.to : link.from = l;
			}
			else if (x == attr_idx) {
				stage.links.swapAndPop(i);
				--i;
			}
		}
	}
}

template <typename F>
static void	forEachInput(const ShaderEditor::Stage& stage, int node_id, const F& f) {
	for (const ShaderEditor::Link& link : stage.links) {
		if (toNodeId(link.to) == node_id) {
			const int iter = stage.nodes.find([&](const ShaderEditor::Node* node) { return node->m_id == toNodeId(link.from); }); 
			const ShaderEditor::Node* from = stage.nodes[iter];
			const int from_attr = toAttrIdx(link.from);
			const int to_attr = toAttrIdx(link.to);
			f(from, from_attr, to_attr);
		}
	}
}

static const ShaderEditor::Node* getInput(const ShaderEditor::Stage& stage, u16 node_id, u16 input_idx, Ref<int> from_attr_idx) {
	const ShaderEditor::Node* res = nullptr;
	forEachInput(stage, node_id, [&](const ShaderEditor::Node* from, int from_attr, int to_attr){
		if (to_attr == input_idx) {
			from_attr_idx = from_attr;
			res = from;
		}
	});
	return res;
}

struct VertexOutputNode : public ShaderEditor::Node
{
	explicit VertexOutputNode(ShaderEditor& editor)
		: Node((int)NodeType::VERTEX_OUTPUT, editor)
		, m_varyings(editor.getAllocator())
	{
		m_varyings.push({ StaticString<32>("output"), ShaderEditor::ValueType::VEC4 });
	}

	void save(OutputMemoryStream& blob) override
	{
		blob.write(m_varyings.size());
		for (const Varying& v : m_varyings) {
			blob.write(v.type);
			blob.writeString(v.name);
		}
	}

	void load(InputMemoryStream& blob) override
	{
		/*int c;
		blob.read(c);
		m_varyings.resize(c);
		m_inputs.clear();
		m_inputs.push(nullptr); // gl_Position
		for (int i = 0; i < c; ++i) {
			blob.read(m_varyings[i].type);
			m_varyings[i].name = blob.readString();
			m_inputs.push(nullptr);
		}*/
	}

	void generateBeforeMain(OutputMemoryStream& blob) const override
	{
		// TODO handle ifdefs
		for (const Varying& v : m_varyings) {
			blob << "\tout " << toString(v.type) << " " << v.name << ";\n";
		}
	}

	void generate(OutputMemoryStream& blob, const ShaderEditor::Stage& stage) const override
	{
		forEachInput(stage, m_id, [&](const Node* from, int from_attr_idx, int to_attr_idx){
			from->generate(blob, stage);
		});

		forEachInput(stage, m_id, [&](const Node* from, int from_attr_idx, int to_attr_idx){
			if (to_attr_idx == 0) {
				blob << "\t\tgl_Position = ";
				from->printReference(blob, stage, from_attr_idx);
				blob << ";\n";
			}
			else {
				const Varying& v = m_varyings[to_attr_idx - 1];
				blob << "\t\t" << v.name << " = ";
				from->printReference(blob, stage, from_attr_idx);
				blob << ";\n";
			}
		});
	}

	ShaderEditor::ValueType getInputType(int index) const override {
		return ShaderEditor::ValueType::NONE;
	}

	void onGUI(ShaderEditor::Stage& stage) override
	{
		imnodes::BeginNodeTitleBar();
		ImGui::Text("Output");
		imnodes::EndNodeTitleBar();

		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("%s", "Vertex position");
		imnodes::EndInputAttribute();

		for (int i = 0, c = m_varyings.size(); i < c; ++i) {
			imnodes::BeginInputAttribute(m_id | (u32(i + 1) << 16));
			Varying& v = m_varyings[i];
			auto getter = [](void*, int idx, const char** out){
				*out = toString((ShaderEditor::ValueType)idx);
				return true;
			};
			if (ImGui::Button(ICON_FA_TRASH)) {
				removeConnection(stage, this, i + 1, true);
				m_varyings.erase(i);
				--i;
				--c;
				imnodes::EndInputAttribute();
				m_editor.saveUndo();
				continue;
			}
			ImGui::SameLine();
			ImGui::Combo(StaticString<32>("##t", i), (int*)&v.type, getter, nullptr, (int)ShaderEditor::ValueType::COUNT);
			ImGui::SameLine();
			ImGui::InputTextWithHint(StaticString<32>("##n", i), "Name", v.name.data, sizeof(v.name.data));
			imnodes::EndInputAttribute();
		}
		if (ImGui::Button("Add")) {
			m_varyings.push({ StaticString<32>("output"), ShaderEditor::ValueType::VEC4 });
		}
	}

	struct Varying {
		StaticString<32> name;
		ShaderEditor::ValueType type;
	};
	Array<Varying> m_varyings;
};


struct VertexInputNode : public ShaderEditor::Node
{
	explicit VertexInputNode(ShaderEditor& editor)
		: Node((int)NodeType::VERTEX_INPUT, editor)
		, m_semantics(editor.getAllocator())
	{
		m_semantics.push(Mesh::AttributeSemantic::POSITION);
	}


	void save(OutputMemoryStream& blob) override
	{
		blob.write(m_semantics.size()); 
		for(Mesh::AttributeSemantic i : m_semantics) {
			blob.write(i);
		}
	}


	void load(InputMemoryStream& blob) override
	{
		int c;
		blob.read(c);
		m_semantics.resize(c);
		for (int i = 0; i < c; ++i) {
			blob.read(m_semantics[i]);
		}
	}


	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const override
	{
		blob << "a" << output_idx;
	}


	ShaderEditor::ValueType getOutputType(int idx) const override
	{
		return semanticToType(m_semantics[idx]);
	}


	void generateBeforeMain(OutputMemoryStream& blob) const override
	{
		// TODO handle ifdefs
		for (int i = 0; i < m_semantics.size(); ++i) {
			blob << "\tin " << toString(getOutputType(0)) << " a" << i << ";\n";
		}
	}

	ShaderEditor::ValueType getInputType(int index) const override {
		return ShaderEditor::ValueType::NONE;
	}

	void onGUI(ShaderEditor::Stage& stage) override
	{
		imnodes::BeginNodeTitleBar();
		ImGui::TextUnformatted("Input");
		imnodes::EndNodeTitleBar();

		for (int i = 0; i < m_semantics.size(); ++i) {
			imnodes::BeginOutputAttribute(m_id | (i << 16) | OUTPUT_FLAG);
			if (ImGui::Button(ICON_FA_TRASH)) {
				m_semantics.erase(i);
				--i;
				imnodes::EndOutputAttribute();
				m_editor.saveUndo();
				continue;
			}
			ImGui::SameLine();
			auto getter = [](void*, int idx, const char** out){
				*out = toString((Mesh::AttributeSemantic)idx);
				return true;
			};
			int s = (int)m_semantics[i];
			if (ImGui::Combo(StaticString<32>("##cmb", i), &s, getter, nullptr, (int)Mesh::AttributeSemantic::COUNT)) {
				m_semantics[i] = (Mesh::AttributeSemantic)s;
			}
			imnodes::EndOutputAttribute();
		}
		if (ImGui::Button("Add")) {
			m_semantics.push(Mesh::AttributeSemantic::POSITION);
			m_editor.saveUndo();
		}
	}

	Array<Mesh::AttributeSemantic> m_semantics;
};


void ShaderEditor::Node::printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const
{
	blob << "v" << m_id;
}


ShaderEditor::Node::Node(int type, ShaderEditor& editor)
	: m_type(type)
	, m_editor(editor)
	, m_id(0xffFF)
{
}

struct OperatorNode : public ShaderEditor::Node
{
	enum Operation : int
	{
		ADD,
		SUB,
		MUL,
		DIV,
		LT,
		LTE,
		GT,
		GTE,
		EQ,
		NEQ,
		BIT_AND,
		BIT_OR,
		
		COUNT
	};

	explicit OperatorNode(ShaderEditor& editor)
		: Node((int)NodeType::OPERATOR, editor)
	{
		m_operation = MUL;
	}

	void save(OutputMemoryStream& blob) override { int o = m_operation; blob.write(o); }
	void load(InputMemoryStream& blob) override { int o; blob.read(o); m_operation = (Operation)o; }

	ShaderEditor::ValueType getOutputType(int) const override
	{
		switch (m_operation) {
			case LT:
			case LTE:
			case GT:
			case GTE:
			case EQ:
			case NEQ:
				// TODO bvec*
				return ShaderEditor::ValueType::BOOL;
				break;
		}
		// TODO float * vec4 and others
		return getInputType(0);
	}

	static const char* toString(Operation op) {
		switch (op) {
			case BIT_AND: return "&";
			case BIT_OR: return "|";
			case ADD: return "+";
			case MUL: return "*";
			case DIV: return "/";
			case SUB: return "-";
			case LT: return "<";
			case LTE: return "<=";
			case GT: return ">";
			case GTE: return ">=";
			case EQ: return "==";
			case NEQ: return "!=";
			default: ASSERT(false); return "Unknown";
		}
	}

	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int attr_idx) const override
	{
		int from_attr_idx0, from_attr_idx1;
		const Node* input0 = getInput(stage, m_id, 0, Ref(from_attr_idx0));
		const Node* input1 = getInput(stage, m_id, 1, Ref(from_attr_idx1));
		if (!input0 || !input1) return; 
		
		blob << "(";
		input0->printReference(blob, stage, from_attr_idx0);
		blob << ") " << toString(m_operation) << " (";
		input1->printReference(blob, stage, from_attr_idx1);
		blob << ")";
	}

	void onGUI(ShaderEditor::Stage& stage) override
	{
		int o = m_operation;
		auto getter = [](void*, int idx, const char** out){
			*out =  toString((Operation)idx);
			return true;
		};

		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("A");
		imnodes::EndInputAttribute();

		imnodes::BeginOutputAttribute(u32(m_id) | OUTPUT_FLAG);
		if (ImGui::Combo("Op", &o, getter, nullptr, (int)Operation::COUNT)) {
			m_operation = (Operation)o;
		}
		imnodes::EndOutputAttribute();

		imnodes::BeginInputAttribute(m_id | (1 << 16));
		ImGui::Text("B");
		imnodes::EndInputAttribute();

	}

	Operation m_operation;
};

struct SwizzleNode : public ShaderEditor::Node
{
	explicit SwizzleNode(ShaderEditor& editor)
		: Node((int)NodeType::SWIZZLE, editor)
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
	
	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const override {
		int from_attr_idx;
		const ShaderEditor::Node* input = getInput(stage, m_id, 0, Ref(from_attr_idx));
		if (!input) return;
		
		blob << "(";
		input->printReference(blob, stage, from_attr_idx);
		blob << ")." << m_swizzle;
	}

	void onGUI(ShaderEditor::Stage& stage) override {
		imnodes::BeginInputAttribute(m_id);
		ImGui::InputTextWithHint("", "swizzle", m_swizzle.data, sizeof(m_swizzle.data));
		imnodes::EndInputAttribute();
		ImGui::SameLine();
		imnodes::BeginOutputAttribute(u32(m_id) | OUTPUT_FLAG);
		imnodes::EndOutputAttribute();
	}

	StaticString<5> m_swizzle;
};

struct Vec4MergeNode : public ShaderEditor::Node
{
	explicit Vec4MergeNode(ShaderEditor& editor)
		: Node((int)NodeType::VEC4_MERGE, editor)
	{}


	void save(OutputMemoryStream&) override {}
	void load(InputMemoryStream&) override {}
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC4; }

	void generate(OutputMemoryStream&blob, const ShaderEditor::Stage& stage) const override {
		blob << "\t\tvec4 v" << m_id << ";\n";

		int from_attr;
		const Node* input = getInput(stage, m_id, 0, Ref(from_attr));
		if (input) {
			blob << "\t\tv" << m_id << ".xyz = ";
			input->printReference(blob, stage, from_attr);
			blob << ";\n";
		}

		input = getInput(stage, m_id, 1, Ref(from_attr));
		if (input) {
			blob << "\t\tv" << m_id << ".x = ";
			input->printReference(blob, stage, from_attr);
			blob << ";\n";
		}

		input = getInput(stage, m_id, 2, Ref(from_attr));
		if (input) {
			blob << "\t\tv" << m_id << ".y = ";
			input->printReference(blob, stage, from_attr);
			blob << ";\n";
		}

		input = getInput(stage, m_id, 3, Ref(from_attr));
		if (input) {
			blob << "\t\tv" << m_id << ".z = ";
			input->printReference(blob, stage, from_attr);
			blob << ";\n";
		}

		input = getInput(stage, m_id, 4, Ref(from_attr));
		if (input) {
			blob << "\t\tv" << m_id << ".w = ";
			input->printReference(blob, stage, from_attr);
			blob << ";\n";
		}
	}


	void onGUI(ShaderEditor::Stage& stage) override
	{
		imnodes::BeginInputAttribute(m_id);
		ImGui::TextUnformatted("xyz");
		imnodes::EndInputAttribute();

		imnodes::BeginInputAttribute(m_id | (1 << 16));
		ImGui::TextUnformatted("x");
		imnodes::EndInputAttribute();

		imnodes::BeginInputAttribute(m_id | (2 << 16));
		ImGui::TextUnformatted("y");
		imnodes::EndInputAttribute();

		imnodes::BeginInputAttribute(m_id | (3 << 16));
		ImGui::TextUnformatted("z");
		imnodes::EndInputAttribute();

		imnodes::BeginInputAttribute(m_id | (4 << 16));
		ImGui::TextUnformatted("w");
		imnodes::EndInputAttribute();

		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		ImGui::TextUnformatted("xyzw");
		imnodes::EndOutputAttribute();
	}
};

struct FunctionCallNode : public ShaderEditor::Node
{
	explicit FunctionCallNode(ShaderEditor& editor)
		: Node((int)NodeType::FUNCTION_CALL, editor)
	{
		m_function = 0;
	}

	void save(OutputMemoryStream& blob) override { blob.write(m_function); }
	void load(InputMemoryStream& blob) override { blob.read(m_function); }

	void generate(OutputMemoryStream&blob, const ShaderEditor::Stage& stage) const override {
		int from_attr;
		const Node* input0 = getInput(stage, m_id, 0, Ref(from_attr));

		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = " << FUNCTIONS[m_function] << "(";
		if (input0) {
			input0->printReference(blob, stage, from_attr);
		}
		else {
			blob << "0";
		}
		blob << ");\n";
	}


	void onGUI(ShaderEditor::Stage& stage) override {
		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("argument");
		imnodes::EndInputAttribute();

		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		auto getter = [](void* data, int idx, const char** out_text) -> bool {
			*out_text = FUNCTIONS[idx];
			return true;
		};
		ImGui::Combo("Function", &m_function, getter, nullptr, lengthOf(FUNCTIONS));
		imnodes::EndOutputAttribute();
	}

	int m_function;
};


struct BinaryFunctionCallNode : public ShaderEditor::Node
{
	explicit BinaryFunctionCallNode(ShaderEditor& editor)
		: Node((int)NodeType::BINARY_FUNCTION_CALL, editor)
	{
		m_function = 0;
	}


	void save(OutputMemoryStream& blob) override { blob.write(m_function); }
	void load(InputMemoryStream& blob) override { blob.read(m_function); }

	ShaderEditor::ValueType getOutputType(int) const override
	{
		return BINARY_FUNCTIONS[m_function].output_type(*this);
	}


	void generate(OutputMemoryStream& blob, const ShaderEditor::Stage& stage) const override {
		int from_attr0;
		int from_attr1;
		const Node* input0 = getInput(stage, m_id, 0, Ref(from_attr0));
		const Node* input1 = getInput(stage, m_id, 1, Ref(from_attr1));

		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = " << BINARY_FUNCTIONS[m_function].name << "(";
		if (input0) {
			input0->printReference(blob, stage, from_attr0);
		}
		else {
			blob << "0";
		}
		blob << ", ";
		if (input1) {
			input1->printReference(blob, stage, from_attr1);
		}
		else {
			blob << "0";
		}
		blob << ");\n";
	}


	void onGUI(ShaderEditor::Stage& stage) override {
		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("argument 1");
		imnodes::EndInputAttribute();
		
		imnodes::BeginInputAttribute(m_id | (1 << 16));
		ImGui::Text("argument 2");
		imnodes::EndInputAttribute();

		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		auto getter = [](void* data, int idx, const char** out_text) -> bool {
			*out_text = BINARY_FUNCTIONS[idx].name;
			return true;
		};
		ImGui::Combo("Function", &m_function, getter, nullptr, lengthOf(BINARY_FUNCTIONS));
		imnodes::EndOutputAttribute();
	}

	int m_function;
};

struct InstanceMatrixNode : public ShaderEditor::Node
{
	explicit InstanceMatrixNode(ShaderEditor& editor)
		: Node((int)NodeType::INSTANCE_MATRIX, editor)
	{}

	void save(OutputMemoryStream&) override {}
	void load(InputMemoryStream&) override {}
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::MATRIX4; }

	void generate(OutputMemoryStream&blob, const ShaderEditor::Stage& stage) const override {
		blob << "\tmat4 v" << m_id << ";\n";

		blob << "\tv" << m_id << "[0] = i_data0;\n";
		blob << "\tv" << m_id << "[1] = i_data1;\n";
		blob << "\tv" << m_id << "[2] = i_data2;\n";
		blob << "\tv" << m_id << "[3] = i_data3;\n";

		blob << "\tv" << m_id << " = transpose(v" << m_id << ");\n";
	}


	void onGUI(ShaderEditor::Stage&) override
	{
		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		ImGui::Text("Instance matrix");
		imnodes::EndOutputAttribute();
	}
};

struct ConstNode : public ShaderEditor::Node
{
	explicit ConstNode(ShaderEditor& editor)
		: Node((int)NodeType::CONSTANT, editor)
	{
		m_type = ShaderEditor::ValueType::VEC4;
		m_value[0] = m_value[1] = m_value[2] = m_value[3] = 0;
		m_int_value = 0;
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

	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const override {
		switch(m_type) {
			case ShaderEditor::ValueType::VEC4:
				blob << "vec4(" << m_value[0] << ", " << m_value[1] << ", "
					<< m_value[2] << ", " << m_value[3] << ")";
				break;
			case ShaderEditor::ValueType::VEC3:
				blob << "vec3(" << m_value[0] << ", " << m_value[1] << ", "
					<< m_value[2] << ")";
				break;
			case ShaderEditor::ValueType::VEC2:
				blob << "vec2(" << m_value[0] << ", " << m_value[1] << ")";
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
	
	void onGUI(ShaderEditor::Stage& stage) override {
		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		auto getter = [](void*, int idx, const char** out){
			*out = toString((ShaderEditor::ValueType)idx);
			return true;
		};
		ImGui::Combo("Type", (int*)&m_type, getter, nullptr, (int)ShaderEditor::ValueType::COUNT);

		switch(m_type) {
			case ShaderEditor::ValueType::VEC4:
				ImGui::Checkbox("Color", &m_is_color);
				if (m_is_color) {
					ImGui::ColorPicker4("", m_value); 
				}
				else {
					ImGui::InputFloat4("", m_value);
				}
				break;
			case ShaderEditor::ValueType::VEC3:
				ImGui::Checkbox("Color", &m_is_color);
				if (m_is_color) {
					ImGui::ColorPicker3("", m_value); 
				}
				else {
					ImGui::InputFloat3("", m_value);
				}
				break;
			case ShaderEditor::ValueType::VEC2:
				ImGui::InputFloat2("", m_value);
				break;
			case ShaderEditor::ValueType::FLOAT:
				ImGui::InputFloat("", m_value);
				break;
			case ShaderEditor::ValueType::INT:
				ImGui::InputInt("", &m_int_value);
				break;
			default: ASSERT(false); break;
		}
		imnodes::EndOutputAttribute();
	}

	ShaderEditor::ValueType m_type;
	bool m_is_color = false;
	float m_value[4];
	int m_int_value;
};


struct SampleNode : public ShaderEditor::Node
{
	explicit SampleNode(ShaderEditor& editor)
		: Node((int)NodeType::SAMPLE, editor)
	{
		m_texture = 0;
	}

	void save(OutputMemoryStream& blob) override { blob.write(m_texture); }
	void load(InputMemoryStream& blob) override { blob.read(m_texture); }
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC4; }

	void generate(OutputMemoryStream&blob, const ShaderEditor::Stage& stage) const override {
		blob << "\t\tvec4 v" << m_id << " = ";
		int from_attr;
		const Node* input0 = getInput(stage, m_id, 0, Ref(from_attr));
		if (!input0) {
			blob << "vec4(1, 0, 1, 1);\n";
			return;
		}

		blob << "texture(" << m_editor.getTextureName(m_texture) << ", ";
		input0->printReference(blob, stage, from_attr);
		blob << ");\n";
	}

	void onGUI(ShaderEditor::Stage& stage) override {
		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("UV");
		imnodes::EndInputAttribute();

		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		auto getter = [](void* data, int idx, const char** out) -> bool {
			*out = ((SampleNode*)data)->m_editor.getTextureName(idx);
			return true;
		};
		ImGui::Combo("Texture", &m_texture, getter, this, ShaderEditor::MAX_TEXTURES_COUNT);
		imnodes::EndOutputAttribute();
	}

	int m_texture;
};

struct FragmentInputNode : public ShaderEditor::Node
{
	explicit FragmentInputNode(ShaderEditor& editor)
		: Node((int)NodeType::FRAGMENT_INPUT, editor)
		, m_varyings(editor.getAllocator())
	{
		m_varyings.push({StaticString<32>(), ShaderEditor::ValueType::VEC4});
	}

	void save(OutputMemoryStream& blob) override
	{
		blob.write(m_varyings.size());
		for (const VertexOutputNode::Varying& v : m_varyings) {
			blob.write(v.type);
			blob.writeString(v.name);
		}
	}

	void load(InputMemoryStream& blob) override
	{
		int c;
		blob.read(c);
		m_varyings.resize(c);
		for (int i = 0; i < c; ++i) {
			blob.read(m_varyings[i].type);
			m_varyings[i].name = blob.readString();
		}
	}

	void generateBeforeMain(OutputMemoryStream& blob) const override {
		// TODO handle ifdefs
		for (const VertexOutputNode::Varying& v : m_varyings) {
			blob << "\tin " << toString(v.type) << " " << v.name << ";\n";
		}
	}

	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const {
		blob << m_varyings[output_idx].name;
	}

	void onGUI(ShaderEditor::Stage& stage) override
	{
		imnodes::BeginNodeTitleBar();
		ImGui::Text("Input");
		imnodes::EndNodeTitleBar();

		for (int i = 0, c = m_varyings.size(); i < c; ++i) {
			imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG | (i << 16));
			VertexOutputNode::Varying& v = m_varyings[i];
			auto getter = [](void*, int idx, const char** out){
				*out = toString((ShaderEditor::ValueType)idx);
				return true;
			};
			if (ImGui::Button(ICON_FA_TRASH)) {
				m_varyings.erase(i);
				--i;
				m_editor.saveUndo();
				imnodes::EndOutputAttribute();
				continue;
			}
			ImGui::SameLine();
			ImGui::Combo(StaticString<32>("##t", i), (int*)&v.type, getter, nullptr, (int)ShaderEditor::ValueType::COUNT);
			ImGui::SameLine();
			ImGui::InputTextWithHint(StaticString<32>("##n", i), "Name", v.name.data, sizeof(v.name.data));
			imnodes::EndOutputAttribute();
		}
	}

	Array<VertexOutputNode::Varying> m_varyings;
};


struct FragmentOutputNode : public ShaderEditor::Node
{
	explicit FragmentOutputNode(ShaderEditor& editor)
		: Node((int)NodeType::FRAGMENT_OUTPUT, editor)
	{
		m_count = 1;
	}

	void save(OutputMemoryStream& blob) { blob.write(m_count); }
	
	void load(InputMemoryStream& blob) {
		blob.read(m_count);
	}

	void generateBeforeMain(OutputMemoryStream& blob) const override
	{
		for (u32 i = 0; i < m_count; ++i) {
			blob << "\tlayout(location = " << i << ") out vec4 out" << i << ";\n";
		}
	}

	void generate(OutputMemoryStream& blob, const ShaderEditor::Stage& stage) const override {
		int from_attr;
		const Node* input0 = getInput(stage, m_id, 0, Ref(from_attr));
		
		if (input0) {
			blob << "\t\tif(";
			input0->printReference(blob, stage, from_attr);
			blob << ") discard;\n";
		}

		for (u32 i = 0; i < m_count; ++i) {
			const Node* input = getInput(stage, m_id, i + 1, Ref(from_attr));
			if (input) {
				blob << "\t\tout" << i << " = ";
				input->printReference(blob, stage, from_attr);
				blob << ";\n";
			}
			else {
				blob << "\t\tout" << i << " = vec4(0, 0, 0, 1);\n";
			}
		}
	}

	void onGUI(ShaderEditor::Stage& stage) override {
		imnodes::BeginNodeTitleBar();
		ImGui::Text("Output");
		imnodes::EndNodeTitleBar();
		
		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("Discard");
		imnodes::EndInputAttribute();

		for (u32 i = 0; i < m_count; ++i) {
			imnodes::BeginInputAttribute(m_id | ((i + 1) << 16));
			ImGui::Text("output %d ", i);
			imnodes::EndInputAttribute();
		}

		if (ImGui::Button("Add")) {
			++m_count;
			m_editor.saveUndo();
		}
		if (m_count > 0) {
			ImGui::SameLine();
			if (ImGui::Button("Remove")) {
				--m_count;
				m_editor.saveUndo();
			}
		}
	}

	u32 m_count = 0;
};


struct MixNode : public ShaderEditor::Node
{
	explicit MixNode(ShaderEditor& editor)
		: Node((int)NodeType::MIX, editor)
	{}

	ShaderEditor::ValueType getOutputType(int) const override 
	{
		return getInputType(1);
	}

	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const override {
		int from_attr0, from_attr1, from_attr2;
		const Node* input0 = getInput(stage, m_id, 0, Ref(from_attr0));
		const Node* input1 = getInput(stage, m_id, 1, Ref(from_attr1));
		const Node* input2 = getInput(stage, m_id, 2, Ref(from_attr2));

		if (!input0 || !input1 || !input2) return;

		blob << "mix(";
		input0->printReference(blob, stage, from_attr0);
		blob << ", ";
		input1->printReference(blob, stage, from_attr1);
		blob << ", ";
		input2->printReference(blob, stage, from_attr2);
		blob << ")";
	}

	void onGUI(ShaderEditor::Stage& stage) override {
		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		imnodes::EndOutputAttribute();

		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("Input 1");
		imnodes::EndInputAttribute();

		imnodes::BeginInputAttribute(m_id | (1 << 16));
		ImGui::Text("Input 2");
		imnodes::EndInputAttribute();
		
		imnodes::BeginInputAttribute(m_id | (2 << 16));
		ImGui::Text("Weight");
		imnodes::EndInputAttribute();

	}
};

struct PassNode : public ShaderEditor::Node
{
	explicit PassNode(ShaderEditor& editor)
		: Node((int)NodeType::PASS, editor)
	{
		m_pass[0] = 0;
	}

	void save(OutputMemoryStream& blob) override { blob.writeString(m_pass); }
	void load(InputMemoryStream& blob) override { copyString(m_pass, blob.readString()); }
	ShaderEditor::ValueType getOutputType(int) const override { return getInputType(0); }

	void generate(OutputMemoryStream& blob, const ShaderEditor::Stage& stage) const override {
		const char* defs[] = { "\t\t#ifdef ", "\t\t#ifndef " };
		for (int i = 0; i < 2; ++i) {
			int from_attr;
			const Node* input = getInput(stage, m_id, 0, Ref(from_attr));
			
			if (!input) continue;

			blob << defs[i] << m_pass << "\n";
			blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = ";
			input->printReference(blob, stage, from_attr);
			blob << ";\n";
			blob << "\t\t#endif // " << m_pass << "\n\n";
		}
	}

	void onGUI(ShaderEditor::Stage& stage) override
	{
		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("if defined");
		imnodes::EndInputAttribute();

		imnodes::BeginInputAttribute(m_id | (1 << 16));
		ImGui::Text("if not defined");
		imnodes::EndInputAttribute();

		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		ImGui::InputText("Pass", m_pass, sizeof(m_pass));
		imnodes::EndOutputAttribute();
	}

	char m_pass[50];
};

struct IfNode : public ShaderEditor::Node
{
	explicit IfNode(ShaderEditor& editor)
		: Node((int)NodeType::IF, editor)
	{
	}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

	void generate(OutputMemoryStream& blob, const ShaderEditor::Stage& stage) const override {
		int from_attr_idx0, from_attr_idx1, from_attr_idx2;
		const Node* input0 = getInput(stage, m_id, 0, Ref(from_attr_idx0));
		const Node* input1 = getInput(stage, m_id, 1, Ref(from_attr_idx1));
		const Node* input2 = getInput(stage, m_id, 2, Ref(from_attr_idx2));
		if(!input0) return;
		
		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << ";\n";
		if (input1) {
			blob << "\t\tif(";
			input0->printReference(blob, stage, from_attr_idx0);
			blob << ") {\n";
			blob << "\t\t\tv" << m_id << " = ";
			input1->printReference(blob, stage, from_attr_idx1);
			blob << ";\n";
			blob << "\t\t}\n";

			if(input2) {
				blob << "\t\telse {\n";
				blob << "\t\t\tv" << m_id << " = ";
				input2->printReference(blob, stage, from_attr_idx2);
				blob << ";\n";
				blob << "\t\t}\n";
			}
		}
		else if(input2) {
			blob << "\t\tif(!(";
			input0->printReference(blob, stage, from_attr_idx0);
			blob << ")) {\n";
			blob << "\t\t\tv" << m_id << " = ";
			input2->printReference(blob, stage, from_attr_idx2);
			blob << ";\n";
			blob << "\t\t}\n";
		}
	}

	void onGUI(ShaderEditor::Stage& stage) override {
		imnodes::BeginInputAttribute(m_id);
		ImGui::Text("Condition");
		imnodes::EndInputAttribute();
		
		imnodes::BeginInputAttribute(m_id | (1 << 16));
		ImGui::Text("If");
		imnodes::EndInputAttribute();

		imnodes::BeginInputAttribute(m_id | (2 << 16));
		ImGui::Text("Else");
		imnodes::EndInputAttribute();

		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		ImGui::TextUnformatted("Output");
		imnodes::EndOutputAttribute();
	}
};

struct VertexPrefabNode : ShaderEditor::Node
{
	enum class Type : int {
		FULLSCREEN_POSITION,

		COUNT
	};

	explicit VertexPrefabNode(ShaderEditor& editor)
		: Node((int)NodeType::VERTEX_PREFAB, editor)
	{}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage&, int) const override {
		switch(m_type) {
			case Type::FULLSCREEN_POSITION: blob << "vec4((gl_VertexID & 1) * 2 - 1, (gl_VertexID & 2) - 1, 0, 1)"; break;
			default: ASSERT(false); break;
		}
	}

	ShaderEditor::ValueType getOutputType(int) const override
	{
		return ShaderEditor::ValueType::VEC4;
	}

	static const char* toString(Type type) {
		switch(type) {
			case Type::FULLSCREEN_POSITION: return "fullscreen position"; 
			default: ASSERT(false); return "Unknown";
		}
	}

	void onGUI(ShaderEditor::Stage&) override
	{
		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		auto getter = [](void*, int idx, const char** out){
			*out = toString((Type)idx);
			return true;
		};
		ImGui::Combo("", (int*)&m_type, getter, nullptr, (int)Type::COUNT);
		imnodes::EndOutputAttribute();
	}

	Type m_type = Type::FULLSCREEN_POSITION;
};


struct VertexIDNode : ShaderEditor::Node
{
	explicit VertexIDNode(ShaderEditor& editor)
		: Node((int)NodeType::VERTEX_ID, editor)
	{}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const override {
		blob << "gl_VertexID";
	}

	ShaderEditor::ValueType getOutputType(int) const override
	{
		return ShaderEditor::ValueType::INT;
	}

	void onGUI(ShaderEditor::Stage&) override
	{
		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		ImGui::Text("Vertex ID");
		imnodes::EndOutputAttribute();
	}
};

struct BuiltinUniformNode : ShaderEditor::Node
{
	explicit BuiltinUniformNode(ShaderEditor& editor)
		: Node((int)NodeType::BUILTIN_UNIFORM, editor)
	{
		m_uniform = 0;
	}


	void save(OutputMemoryStream& blob) override { blob.write(m_uniform); }
	void load(InputMemoryStream& blob) override { blob.read(m_uniform); }


	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage& stage, int output_idx) const override
	{
		blob << BUILTIN_UNIFORMS[m_uniform].name;
	}

	ShaderEditor::ValueType getOutputType(int) const override
	{
		return BUILTIN_UNIFORMS[m_uniform].type;
	}

	void onGUI(ShaderEditor::Stage& stage) override
	{
		auto getter = [](void* data, int index, const char** out_text) -> bool {
			*out_text = BUILTIN_UNIFORMS[index].gui_name;
			return true;
		};
		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		ImGui::Combo("Uniform", (int*)&m_uniform, getter, nullptr, lengthOf(BUILTIN_UNIFORMS));
		imnodes::EndOutputAttribute();
	}

	int m_uniform;
};

struct UniformNode : public ShaderEditor::Node
{
	explicit UniformNode(ShaderEditor& editor)
		: Node((int)NodeType::UNIFORM, editor)
	{
		m_value_type = ShaderEditor::ValueType::VEC4;
	}

	void save(OutputMemoryStream& blob) override { blob.write(m_type); blob.writeString(m_name); }
	void load(InputMemoryStream& blob) override { blob.read(m_type); m_name = blob.readString(); }
	ShaderEditor::ValueType getOutputType(int) const override { return m_value_type; }

	void printReference(OutputMemoryStream& blob, const ShaderEditor::Stage&, int) const override {
		blob << m_name;
	}

	void generateBeforeMain(OutputMemoryStream& blob) const override {
		blob << "\tuniform " << toString(m_value_type) << " " << m_name << ";\n";
	}


	void onGUI(ShaderEditor::Stage& stage) override
	{
		imnodes::BeginOutputAttribute(m_id | OUTPUT_FLAG);
		auto getter = [](void*, int idx, const char** out){
			*out = toString((ShaderEditor::ValueType)idx);
			return true;
		};
		ImGui::Combo("Type", (int*)&m_value_type, getter, nullptr, (int)ShaderEditor::ValueType::COUNT);
		ImGui::InputText("Name", m_name.data, sizeof(m_name.data));
		imnodes::EndOutputAttribute();
	}

	StaticString<50> m_name;
	ShaderEditor::ValueType m_value_type;
};

ShaderEditor::ShaderEditor(IAllocator& allocator)
	: m_vertex_stage(allocator)
	, m_fragment_stage(allocator)
	, m_allocator(allocator)
	, m_undo_stack(allocator)
	, m_source(allocator)
	, m_undo_stack_idx(-1)
	, m_is_focused(false)
	, m_is_open(false)
{
	newGraph();
	imnodes::Initialize();
}


ShaderEditor::~ShaderEditor()
{
	clear();
}


ShaderEditor::Node* ShaderEditor::getNodeByID(int id)
{
	for(auto* node : m_vertex_stage.nodes)
	{
		if(node->m_id == id) return node;
	}

	for(auto* node : m_fragment_stage.nodes)
	{
		if(node->m_id == id) return node;
	}

	return nullptr;
}


void ShaderEditor::generate(const char* sed_path, bool save_file)
{
	OutputMemoryStream blob(m_allocator);
	blob.reserve(8192);

	for (u32 i = 0; i < lengthOf(m_textures); ++i) {
		if (!m_textures[i][0]) continue;

		blob << "texture_slot {\n";
		blob << "\tname = \"" << m_textures[i] << "\",\n";
		blob << "\tdefault_texture = \"textures/common/white.tga\"\n";
		blob << "}\n\n";
	}

	// TODO
	/*
	for (const Attribute& attr : m_attributes) {
		blob << "attribute { name = \"" << attr.name << "\", semantic = " << toString((Mesh::AttributeSemantic)attr.semantic) << " }\n";
	}
	*/

	blob << "include \"pipelines/common.glsl\"\n\n";


	auto writeShader = [&](const char* shader_type, const Stage& stage){
		blob << shader_type << "_shader [[\n";

		for (u32 i = 0; i < lengthOf(m_textures); ++i) {
			if (!m_textures[i][0]) continue;

			blob << "\tlayout (binding=" << i << ") uniform sampler2D " << m_textures[i] << ";\n";
		}

		for (auto* node : stage.nodes) {
			node->generateBeforeMain(blob);
		}

		blob << "\tvoid main() {\n";
		for(auto& node : stage.nodes)
		{
			if (node->m_type == (int)NodeType::FRAGMENT_OUTPUT ||
				node->m_type == (int)NodeType::VERTEX_OUTPUT)
			{
				node->generate(blob, stage);
			}
		}
		blob << "\t}\n";

		blob << "]]\n\n";
	};

	writeShader("fragment", m_fragment_stage);
	writeShader("vertex", m_vertex_stage);

	if (save_file) {
		PathInfo fi(sed_path);
		StaticString<MAX_PATH_LENGTH> path(fi.m_dir, fi.m_basename, ".shd");
		OS::OutputFile file;
		if (!file.open(path)) {
			logError("Editor") << "Could not create file " << path;
			return;
		}

		file.write(blob.data(), blob.size());
		file.close();
	}

	m_source.resize((u32)blob.size());
	memcpy(m_source.getData(), blob.data(), m_source.length() + 1);
}


void ShaderEditor::addNode(Node* node, const ImVec2& pos, ShaderType type)
{
	if(type == ShaderType::FRAGMENT)
	{
		m_fragment_stage.nodes.push(node);
	}
	else
	{
		m_vertex_stage.nodes.push(node);
	}

	node->m_pos = pos;
	node->m_id = ++m_last_node_id;
}


void ShaderEditor::createConnection(Node* node, int pin_index, bool is_input)
{
	/*if (!m_new_link_info.is_active) return;
	if (m_new_link_info.is_from_input == is_input) return;

	if (is_input)
	{
		execute(LUMIX_NEW(m_allocator, CreateConnectionCommand)(
			m_new_link_info.from->m_id, m_new_link_info.from_pin_index, node->m_id, pin_index, *this));
	}
	else
	{
		execute(LUMIX_NEW(m_allocator, CreateConnectionCommand)(
			node->m_id, pin_index, m_new_link_info.from->m_id, m_new_link_info.from_pin_index, *this));
	}*/
}


void ShaderEditor::saveNode(OutputMemoryStream& blob, Node& node)
{
	int type = (int)node.m_type;
	blob.write(node.m_id);
	blob.write(type);
	blob.write(node.m_pos);

	node.save(blob);
}


void ShaderEditor::saveNodeConnections(OutputMemoryStream& blob, Node& node)
{
	/*int inputs_count = node.m_inputs.size();
	blob.write(inputs_count);
	for(int i = 0; i < inputs_count; ++i)
	{
		int tmp = node.m_inputs[i] ? node.m_inputs[i]->m_id : -1;
		blob.write(tmp);
		tmp = node.m_inputs[i] ? node.m_inputs[i]->m_outputs.indexOf(&node) : -1;
		blob.write(tmp);
	}

	int outputs_count = node.m_outputs.size();
	blob.write(outputs_count);
	for(int i = 0; i < outputs_count; ++i)
	{
		int tmp = node.m_outputs[i] ? node.m_outputs[i]->m_id : -1;
		blob.write(tmp);
		tmp = node.m_outputs[i] ? node.m_outputs[i]->m_inputs.indexOf(&node) : -1;
		blob.write(tmp);
	}*/
}


void ShaderEditor::save(const char* path)
{
	OS::OutputFile file;
	if(!file.open(path)) 
	{
		logError("Editor") << "Could not save shader " << path;
		return;
	}

	OutputMemoryStream blob(m_allocator);
	blob.reserve(4096);
	for (u32 i = 0; i < lengthOf(m_textures); ++i)
	{
		blob.writeString(m_textures[i]);
	}

	int nodes_count = m_vertex_stage.nodes.size();
	blob.write(nodes_count);
	for(auto* node : m_vertex_stage.nodes)
	{
		saveNode(blob, *node);
	}

	for(auto* node : m_vertex_stage.nodes)
	{
		saveNodeConnections(blob, *node);
	}

	nodes_count = m_fragment_stage.nodes.size();
	blob.write(nodes_count);
	for (auto* node : m_fragment_stage.nodes)
	{
		saveNode(blob, *node);
	}

	for (auto* node : m_fragment_stage.nodes)
	{
		saveNodeConnections(blob, *node);
	}

	bool success = file.write(blob.data(), blob.size());
	file.close();
	if (!success)
	{
		logError("Editor") << "Could not save shader " << path;
	}
}


void ShaderEditor::clear()
{
	for (auto* node : m_fragment_stage.nodes)
	{
		LUMIX_DELETE(m_allocator, node);
	}
	m_fragment_stage.nodes.clear();

	for(auto* node : m_vertex_stage.nodes)
	{
		LUMIX_DELETE(m_allocator, node);
	}
	m_vertex_stage.nodes.clear();

	m_undo_stack.clear();
	m_undo_stack_idx = -1;

	m_last_node_id = 0;
}


ShaderEditor::Node* ShaderEditor::createNode(int type)
{
	switch ((NodeType)type) {
		case NodeType::FRAGMENT_OUTPUT:				return LUMIX_NEW(m_allocator, FragmentOutputNode)(*this);
		case NodeType::VERTEX_OUTPUT:				return LUMIX_NEW(m_allocator, VertexOutputNode)(*this);
		case NodeType::FRAGMENT_INPUT:				return LUMIX_NEW(m_allocator, FragmentInputNode)(*this);
		case NodeType::VERTEX_INPUT:				return LUMIX_NEW(m_allocator, VertexInputNode)(*this);
		case NodeType::CONSTANT:					return LUMIX_NEW(m_allocator, ConstNode)(*this);
		case NodeType::MIX:							return LUMIX_NEW(m_allocator, MixNode)(*this);
		case NodeType::SAMPLE:						return LUMIX_NEW(m_allocator, SampleNode)(*this);
		case NodeType::UNIFORM:						return LUMIX_NEW(m_allocator, UniformNode)(*this);
		case NodeType::SWIZZLE:						return LUMIX_NEW(m_allocator, SwizzleNode)(*this);
		case NodeType::VEC4_MERGE:					return LUMIX_NEW(m_allocator, Vec4MergeNode)(*this);
		case NodeType::OPERATOR:					return LUMIX_NEW(m_allocator, OperatorNode)(*this);
		case NodeType::BUILTIN_UNIFORM:				return LUMIX_NEW(m_allocator, BuiltinUniformNode)(*this);
		case NodeType::VERTEX_ID:					return LUMIX_NEW(m_allocator, VertexIDNode)(*this);
		case NodeType::PASS:						return LUMIX_NEW(m_allocator, PassNode)(*this);
		case NodeType::IF:							return LUMIX_NEW(m_allocator, IfNode)(*this);
		case NodeType::INSTANCE_MATRIX:				return LUMIX_NEW(m_allocator, InstanceMatrixNode)(*this);
		case NodeType::FUNCTION_CALL:				return LUMIX_NEW(m_allocator, FunctionCallNode)(*this);
		case NodeType::BINARY_FUNCTION_CALL:		return LUMIX_NEW(m_allocator, BinaryFunctionCallNode)(*this);
		case NodeType::VERTEX_PREFAB:				return LUMIX_NEW(m_allocator, VertexPrefabNode)(*this);
	}

	ASSERT(false);
	return nullptr;
}


ShaderEditor::Node& ShaderEditor::loadNode(InputMemoryStream& blob, ShaderType shader_type)
{
	int type;
	int id;
	blob.read(id);
	blob.read(type);
	Node* node = createNode(type);
	node->m_id = id;
	if(shader_type == ShaderType::FRAGMENT) {
		m_fragment_stage.nodes.push(node);
	}
	else {
		m_vertex_stage.nodes.push(node);
	}
	blob.read(node->m_pos);

	node->load(blob);
	return *node;
}


void ShaderEditor::loadNodeConnections(InputMemoryStream& blob, Node& node)
{
	/*int size;
	blob.read(size);
	for(int i = 0; i < size; ++i)
	{
		int tmp;
		blob.read(tmp);
		node.m_inputs[i] = tmp < 0 ? nullptr : getNodeByID(tmp);
		blob.read(tmp);
		if(node.m_inputs[i]) node.m_inputs[i]->m_outputs[tmp] = &node;
	}

	blob.read(size);
	for(int i = 0; i < size; ++i)
	{
		int tmp;
		blob.read(tmp);
		node.m_outputs[i] = tmp < 0 ? nullptr : getNodeByID(tmp);
		blob.read(tmp);
		if(node.m_outputs[i]) node.m_outputs[i]->m_inputs[tmp] = &node;
	}*/
}


void ShaderEditor::load()
{
	char path[MAX_PATH_LENGTH];
	if (!OS::getOpenFilename(Span(path), "Shader edit data\0*.sed\0", nullptr))
	{
		return;
	}
	m_path = path;

	clear();

	OS::InputFile file;
	if (!file.open(path))
	{
		logError("Editor") << "Failed to load shader " << path;
		return;
	}

	int data_size = (int)file.size();
	Array<u8> data(m_allocator);
	data.resize(data_size);
	if (!file.read(&data[0], data_size))
	{
		logError("Editor") << "Failed to load shader " << path;
		file.close();
		return;
	}
	file.close();

	InputMemoryStream blob(&data[0], data_size);
	for (u32 i = 0; i < lengthOf(m_textures); ++i)
	{
		m_textures[i] = blob.readString();
	}

	int size;
	blob.read(size);
	for(int i = 0; i < size; ++i)
	{
		loadNode(blob, ShaderType::VERTEX);
	}

	for(auto* node : m_vertex_stage.nodes)
	{
		loadNodeConnections(blob, *node);
		m_last_node_id = maximum(int(node->m_id + 1), int(m_last_node_id));
	}

	blob.read(size);
	for (int i = 0; i < size; ++i)
	{
		loadNode(blob, ShaderType::FRAGMENT);
	}

	for (auto* node : m_fragment_stage.nodes)
	{
		loadNodeConnections(blob, *node);
		m_last_node_id = maximum(int(node->m_id + 1), int(m_last_node_id));
	}
}


bool ShaderEditor::getSavePath()
{
	char path[MAX_PATH_LENGTH];
	if (OS::getSaveFilename(Span(path), "Shader edit data\0*.sed\0", "sed"))
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


void ShaderEditor::onGUIRightColumn()
{
	ImGui::BeginChild("right_col");
	
	if (!ImGui::BeginTabBar("##shader_type")) {
		ImGui::EndChild();
		return;
	}

	for (i32 i = 0; i < 2; ++i) {
		if (!ImGui::BeginTabItem(i == 0 ? "Vertex" : "Fragment")) continue;

		ShaderType current_shader_type = (ShaderType)i;
		Stage& stage = current_shader_type == ShaderType::FRAGMENT ? m_fragment_stage : m_vertex_stage; 

		imnodes::BeginNodeEditor();
		
		if (imnodes::IsEditorHovered() && ImGui::IsMouseClicked(1)) {
			ImGui::OpenPopup("context_menu");
			m_context_link = m_hovered_link;
		}

		if(ImGui::BeginPopup("context_menu")) {
			if (m_context_link != -1 && ImGui::Selectable("Remove link")) {
				stage.links.erase(m_context_link);
				m_context_link = -1;
			}


			if (ImGui::BeginMenu("Add")) {
				for (auto node_type : NODE_TYPES) {
					if (!node_type.is_frag && current_shader_type == ShaderType::FRAGMENT) continue;
					if (!node_type.is_vert && current_shader_type == ShaderType::VERTEX) continue;
					if (!node_type.can_user_create) continue;

					if (ImGui::MenuItem(node_type.name)) {
						Node* n = createNode((int)node_type.type);
						n->m_id = ++m_last_node_id;
						stage.nodes.push(n);
					}
				}
				ImGui::EndMenu();
			}

			ImGui::EndPopup();
		}

		const ImGuiStyle normal_style = ImGui::GetStyle();
		ImGuiIO& io = ImGui::GetIO();
		if (io.MouseWheel != 0.0f && io.KeyCtrl) {
			m_scale = clamp(m_scale + io.MouseWheel * 0.10f, 0.20f, 2.0f);
		}
		ImGui::GetStyle().ScaleAllSizes(m_scale);
		ImGui::SetWindowFontScale(m_scale);

		const ImVec2 cursor_screen_pos = ImGui::GetCursorScreenPos();

		for (Node*& node : stage.nodes) {
			imnodes::SetNodeEditorSpacePos(node->m_id, node->m_pos);
			imnodes::BeginNode(node->m_id);
			ImGui::PushItemWidth(120 * m_scale);
			node->onGUI(stage);
			ImGui::PopItemWidth();
			imnodes::EndNode();
		}

		for (i32 i = 0, c = stage.links.size(); i < c; ++i) {
			imnodes::Link(i, stage.links[i].from | OUTPUT_FLAG, stage.links[i].to);
		}

		ImGui::GetStyle() = normal_style;
		imnodes::EndNodeEditor();
		
		for (Node* node : stage.nodes) {
			node->m_pos = imnodes::GetNodeEditorSpacePos(node->m_id);
		}

		m_hovered_link = -1;
		imnodes::IsLinkHovered(&m_hovered_link);

		{
			int start_attr, end_attr;
			if (imnodes::IsLinkCreated(&start_attr, &end_attr)) {
				Array<Link>& links = current_shader_type == ShaderType::FRAGMENT ? m_fragment_stage.links : m_vertex_stage.links;
				if (start_attr & OUTPUT_FLAG) {
					links.push({u32(start_attr) & ~OUTPUT_FLAG, u32(end_attr)});
				}
				else {
					links.push({u32(start_attr), u32(end_attr) & ~OUTPUT_FLAG});
				}
				saveUndo();
			}
		}

		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
	
	ImGui::EndChild();
}


void ShaderEditor::onGUILeftColumn()
{
	ImGui::BeginChild("left_col", ImVec2(m_left_col_width, 0));
	ImGui::PushItemWidth(m_left_col_width);

	if (ImGui::CollapsingHeader("Textures")) {
		for (u32 i = 0; i < lengthOf(m_textures); ++i) {
			ImGui::InputText(StaticString<10>("###tex", i), m_textures[i].data, sizeof(m_textures[i]));
		}
	}

	if (ImGui::CollapsingHeader("Source")) {
		if (m_source.length() == 0) {
			ImGui::Text("Empty");
		}
		else {
			ImGui::InputTextMultiline("", m_source.getData(), m_source.length(), ImVec2(0, 300), ImGuiInputTextFlags_ReadOnly);
		}
	}

	ImGui::PopItemWidth();
	ImGui::EndChild();
}


void ShaderEditor::saveUndo()
{
	// TODO
	generate("", false);
}


bool ShaderEditor::canUndo() const
{
	return m_undo_stack_idx >= 0;
}


bool ShaderEditor::canRedo() const
{
	return m_undo_stack_idx < m_undo_stack.size() - 1;
}


void ShaderEditor::undo()
{
	if (m_undo_stack_idx < 0) return;
	/*
	m_undo_stack[m_undo_stack_idx]->undo();
	--m_undo_stack_idx;*/
}


void ShaderEditor::redo()
{
	if (m_undo_stack_idx + 1 >= m_undo_stack.size()) return;
	/*
	m_undo_stack[m_undo_stack_idx + 1]->execute();
	++m_undo_stack_idx;
	*/
}


void ShaderEditor::destroyNode(Node* node)
{
	/*for(auto* input : node->m_inputs)
	{
		if(!input) continue;
		input->m_outputs[input->m_outputs.indexOf(node)] = nullptr;
	}

	for(auto* output : node->m_outputs)
	{
		if(!output) continue;
		output->m_inputs[output->m_inputs.indexOf(node)] = nullptr;
	}

	LUMIX_DELETE(m_allocator, node);
	m_fragment_stage.nodes.eraseItem(node);
	m_vertex_stage.nodes.eraseItem(node);*/
}


void ShaderEditor::newGraph()
{
	clear();

	for (auto& t : m_textures) t = "";
	m_last_node_id = 0;
	m_path = "";
	
	m_fragment_stage.nodes.push(LUMIX_NEW(m_allocator, FragmentOutputNode)(*this));
	m_fragment_stage.nodes.back()->m_pos.x = 50;
	m_fragment_stage.nodes.back()->m_pos.y = 50;
	m_fragment_stage.nodes.back()->m_id = ++m_last_node_id;

	m_fragment_stage.nodes.push(LUMIX_NEW(m_allocator, FragmentInputNode)(*this));
	m_fragment_stage.nodes.back()->m_pos.x = 50;
	m_fragment_stage.nodes.back()->m_pos.y = 150;
	m_fragment_stage.nodes.back()->m_id = ++m_last_node_id;
	
	m_vertex_stage.nodes.push(LUMIX_NEW(m_allocator, VertexOutputNode)(*this));
	m_vertex_stage.nodes.back()->m_pos.x = 50;
	m_vertex_stage.nodes.back()->m_pos.y = 50;
	m_vertex_stage.nodes.back()->m_id = ++m_last_node_id;
	
	m_vertex_stage.nodes.push(LUMIX_NEW(m_allocator, VertexInputNode)(*this));
	m_vertex_stage.nodes.back()->m_pos.x = 50;
	m_vertex_stage.nodes.back()->m_pos.y = 150;
	m_vertex_stage.nodes.back()->m_id = ++m_last_node_id;
}


void ShaderEditor::generatePasses(OutputMemoryStream& blob)
{
	/*const char* passes[32];
	int pass = 0;

	auto process = [&](Array<Node*>& nodes){
		for (auto* node : nodes)
		{
			if (node->m_type != (int)NodeType::PASS) continue;

			auto* pass_node = static_cast<PassNode*>(node);
			passes[pass] = pass_node->m_pass;
			++pass;
		}
	};

	process(m_vertex_stage.nodes);
	process(m_fragment_stage.nodes);

	if (pass == 0)
	{
		passes[0] = "MAIN";
		++pass;
	}

	for (int i = 0; i < pass; ++i)
	{
		blob << "pass \"" << passes[i] << "\"\n";
	}*/
}


void ShaderEditor::onGUIMenu()
{
	if(ImGui::BeginMenuBar()) {
		if(ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New")) newGraph();
			if (ImGui::MenuItem("Open")) load();
			if (ImGui::MenuItem("Save", nullptr, false, m_path.isValid())) save(m_path.c_str());
			if (ImGui::MenuItem("Save as")) {
				if(getSavePath() && m_path.isValid()) save(m_path.c_str());
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit")) {
			if (ImGui::MenuItem("Undo", nullptr, false, canUndo())) undo();
			if (ImGui::MenuItem("Redo", nullptr, false, canRedo())) redo();
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Generate & save", nullptr, false, m_path.isValid())) {
			generate(m_path.c_str(), true);
		}

		ImGui::EndMenuBar();
	}
}


void ShaderEditor::onGUI()
{
	if (!m_is_open) return;
	StaticString<MAX_PATH_LENGTH + 25> title("Shader Editor");
	if (m_path.isValid()) title << " - " << m_path.c_str();
	title << "###Shader Editor";
	if (ImGui::Begin(title, &m_is_open, ImGuiWindowFlags_MenuBar))
	{
		m_is_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow);

		onGUIMenu();
		onGUILeftColumn();
		ImVec2 size(m_left_col_width, 0);
		ImGui::SameLine();
		ImGui::VSplitter("vsplit", &size);
		m_left_col_width = size.x;
		ImGui::SameLine();
		onGUIRightColumn();
	}
	else
	{
		m_is_focused = false;
	}
	ImGui::End();
}


} // namespace Lumix