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
#include "renderer/editor/particle_editor.h"
#include "renderer/model.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "imgui/IconsFontAwesome5.h"
#include <math.h>


namespace Lumix
{

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
	SCENE_DEPTH,
	ONEMINUS,
	CODE,
	PIN,
	BACKFACE_SWITCH,
};

struct VertexOutput {
	ShaderEditorResource::ValueType type;
	StaticString<32> name;
};

static constexpr ShaderEditorResource::ValueType semanticToType(Mesh::AttributeSemantic semantic) {
	switch (semantic) {
		case Mesh::AttributeSemantic::POSITION: return ShaderEditorResource::ValueType::VEC3;
		case Mesh::AttributeSemantic::COLOR0: return ShaderEditorResource::ValueType::VEC4;
		case Mesh::AttributeSemantic::COLOR1: return ShaderEditorResource::ValueType::VEC4;
		case Mesh::AttributeSemantic::INDICES: return ShaderEditorResource::ValueType::IVEC4;
		case Mesh::AttributeSemantic::WEIGHTS: return ShaderEditorResource::ValueType::VEC4;
		case Mesh::AttributeSemantic::NORMAL: return ShaderEditorResource::ValueType::VEC4;
		case Mesh::AttributeSemantic::TANGENT: return ShaderEditorResource::ValueType::VEC4;
		case Mesh::AttributeSemantic::BITANGENT: return ShaderEditorResource::ValueType::VEC4;
		case Mesh::AttributeSemantic::TEXCOORD0: return ShaderEditorResource::ValueType::VEC2;
		case Mesh::AttributeSemantic::TEXCOORD1: return ShaderEditorResource::ValueType::VEC2;
		case Mesh::AttributeSemantic::INSTANCE0: return ShaderEditorResource::ValueType::VEC4;
		case Mesh::AttributeSemantic::INSTANCE1: return ShaderEditorResource::ValueType::VEC4;
		case Mesh::AttributeSemantic::INSTANCE2: return ShaderEditorResource::ValueType::VEC4;
		default: ASSERT(false); return ShaderEditorResource::ValueType::VEC4;
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

static constexpr const char* toString(ShaderEditorResource::ValueType type) {
	switch (type) {
		case ShaderEditorResource::ValueType::NONE: return "error";
		case ShaderEditorResource::ValueType::BOOL: return "bool";
		case ShaderEditorResource::ValueType::INT: return "int";
		case ShaderEditorResource::ValueType::FLOAT: return "float";
		case ShaderEditorResource::ValueType::VEC2: return "vec2";
		case ShaderEditorResource::ValueType::VEC3: return "vec3";
		case ShaderEditorResource::ValueType::VEC4: return "vec4";
		case ShaderEditorResource::ValueType::IVEC4: return "ivec4";
		case ShaderEditorResource::ValueType::MATRIX3: return "mat3";
		case ShaderEditorResource::ValueType::MATRIX4: return "mat4";
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
	
	{"Parameters", "Color", NodeType::COLOR_PARAM},
	{"Parameters", "Scalar", NodeType::SCALAR_PARAM},
	{"Parameters", "Vec4", NodeType::VEC4_PARAM},
	
	{"Constants", "Time", NodeType::TIME},
	{"Constants", "Vertex ID", NodeType::VERTEX_ID},
	{"Constants", "View direction", NodeType::VIEW_DIR},

	{"Utility", "Fresnel", NodeType::FRESNEL},
	{"Utility", "Custom code", NodeType::CODE},
	{"Utility", "Backface switch", NodeType::BACKFACE_SWITCH},
	{"Utility", "If", NodeType::IF},
	{"Utility", "Pixel depth", NodeType::PIXEL_DEPTH},
	{"Utility", "Scene depth", NodeType::SCENE_DEPTH},
	{"Utility", "Screen position", NodeType::SCREEN_POSITION},
	{"Utility", "Static switch", NodeType::STATIC_SWITCH},
	{"Utility", "Swizzle", NodeType::SWIZZLE},

	{"Math", "Abs", NodeType::ABS},
	{"Math", "All", NodeType::ALL},
	{"Math", "Any", NodeType::ANY},
	{"Math", "Ceil", NodeType::CEIL},
	{"Math", "Cos", NodeType::COS},
	{"Math", "Exp", NodeType::EXP},
	{"Math", "Exp2", NodeType::EXP2},
	{"Math", "Floor", NodeType::FLOOR},
	{"Math", "Fract", NodeType::FRACT},
	{"Math", "Log", NodeType::LOG},
	{"Math", "Log2", NodeType::LOG2},
	{"Math", "Normalize", NodeType::NORMALIZE},
	{"Math", "Not", NodeType::NOT},
	{"Math", "Round", NodeType::ROUND},
	{"Math", "Saturate", NodeType::SATURATE},
	{"Math", "Sin", NodeType::SIN},
	{"Math", "Sqrt", NodeType::SQRT},
	{"Math", "Tan", NodeType::TAN},
	{"Math", "Transpose", NodeType::TRANSPOSE},
	{"Math", "Trunc", NodeType::TRUNC},

	{"Math", "Cross", NodeType::CROSS},
	{"Math", "Distance", NodeType::DISTANCE},
	{"Math", "Dot", NodeType::DOT},
	{"Math", "Length", NodeType::LENGTH},
	{"Math", "Max", NodeType::MAX},
	{"Math", "Min", NodeType::MIN},
	{"Math", "Power", NodeType::POW},

	{"Math", "Add", NodeType::ADD},
	{"Math", "Append", NodeType::APPEND},
	{"Math", "Divide", NodeType::DIVIDE},
	{"Math", "Mix", NodeType::MIX},
	{"Math", "Multiply", NodeType::MULTIPLY},
	{"Math", "One minus", NodeType::ONEMINUS},
	{"Math", "Subtract", NodeType::SUBTRACT},

	{"Vertex", "Normal", NodeType::NORMAL},
	{"Vertex", "Position", NodeType::POSITION},
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

template <typename F>
static void	forEachInput(const ShaderEditorResource& resource, int node_id, const F& f) {
	for (const ShaderEditorResource::Link& link : resource.m_links) {
		if (link.getToNode() == node_id) {
			const int iter = resource.m_nodes.find([&](const ShaderEditorResource::Node* node) { return node->m_id == link.getFromNode(); }); 
			ShaderEditorResource::Node* from = resource.m_nodes[iter];
			const u16 from_attr = link.getFromPin();
			const u16 to_attr = link.getToPin();
			f(from, from_attr, to_attr, u32(&link - resource.m_links.begin()));
		}
	}
}

struct Input {
	ShaderEditorResource::Node* node = nullptr;
	u16 output_idx;

	void printReference(OutputMemoryStream& blob) const { node->printReference(blob, output_idx); }
	operator bool() const { return node != nullptr; }
};

static Input getInput(const ShaderEditorResource& resource, u16 node_id, u16 input_idx) {
	Input res;
	forEachInput(resource, node_id, [&](ShaderEditorResource::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
		if (to_attr == input_idx) {
			res.output_idx = from_attr;
			res.node = from;
		}
	});
	return res;
}

static bool isOutputConnected(const ShaderEditorResource& resource, u16 node_id, u16 input_idx) {
	for (const ShaderEditorResource::Link& link : resource.m_links) {
		if (link.getFromNode() == node_id) {
			const u16 from_attr = link.getFromPin();
			if (from_attr == input_idx) return true;
		}
	}
	return false;
}

static bool isInputConnected(const ShaderEditorResource& resource, u16 node_id, u16 input_idx) {
	return getInput(resource, node_id, input_idx).node;
}

void ShaderEditorResource::Node::printReference(OutputMemoryStream& blob, int output_idx) const
{
	blob << "v" << m_id;
}


ShaderEditorResource::Node::Node(NodeType type, ShaderEditorResource& resource)
	: m_type(type)
	, m_resource(resource)
{
	m_id = 0xffFF;
}

void ShaderEditorResource::Node::inputSlot() {
	ImGuiEx::Pin(m_id | (m_input_count << 16), true);
	++m_input_count;
}

void ShaderEditorResource::Node::outputSlot() {
	ImGuiEx::Pin(m_id | (m_output_count << 16) | OUTPUT_FLAG, false);
	++m_output_count;
}

void ShaderEditorResource::Node::generateOnce(OutputMemoryStream& blob) {
	if (m_generated) return;
	m_generated = true;
	generate(blob);
}

bool ShaderEditorResource::Node::nodeGUI() {
	ImGuiEx::BeginNode(m_id, m_pos, &m_selected);
	m_input_count = 0;
	m_output_count = 0;
	bool res = onGUI();
	ImGuiEx::EndNode();

	ASSERT((m_input_count > 0) == hasInputPins());
	ASSERT((m_output_count > 0) == hasOutputPins());

	return res;
}

struct MixNode : ShaderEditorResource::Node {
	explicit MixNode(ShaderEditorResource& resource)
		: Node(NodeType::MIX, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		ImGuiEx::NodeTitle("Mix");

		ImGui::BeginGroup();
		inputSlot(); ImGui::TextUnformatted("A");
		inputSlot(); ImGui::TextUnformatted("B");
		inputSlot(); ImGui::TextUnformatted("Weight");
		ImGui::EndGroup();

		ImGui::SameLine();
		outputSlot();
		return false;
	}

	void generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
		const Input input2 = getInput(m_resource, m_id, 2);
		if (!input0 || !input1 || !input2) return;
		input0.node->generateOnce(blob);
		input1.node->generateOnce(blob);
		input2.node->generateOnce(blob);

		
		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = mix(";
		input0.printReference(blob);
		blob << ", ";
		input1.printReference(blob);
		blob << ", ";
		input2.printReference(blob);
		blob << ");\n";
	}
};

struct CodeNode : ShaderEditorResource::Node {
	explicit CodeNode(ShaderEditorResource& resource, IAllocator& allocator)
		: Node(NodeType::CODE, resource)
		, m_allocator(allocator)
		, m_inputs(allocator)
		, m_outputs(allocator)
		, m_code(allocator)
	{}

	ShaderEditorResource::ValueType getOutputType(int index) const override { return m_outputs[index].type; }

	void serialize(OutputMemoryStream& blob) override {
		blob.writeString(m_code.c_str());
		blob.write(m_inputs.size());
		for (const Variable& var : m_inputs) {
			blob.write(var.type);
			blob.writeString(var.name.c_str());
		}
		blob.write(m_outputs.size());
		for (const Variable& var : m_outputs) {
			blob.write(var.type);
			blob.writeString(var.name.c_str());
		}
	}

	void deserialize(InputMemoryStream& blob) override {
		m_code = blob.readString();
		i32 size;
		blob.read(size);
		for (i32 i = 0; i < size; ++i) {
			Variable& var = m_inputs.emplace(m_allocator);
			blob.read(var.type);
			var.name = blob.readString();
		}

		blob.read(size);
		for (i32 i = 0; i < size; ++i) {
			Variable& var = m_outputs.emplace(m_allocator);
			blob.read(var.type);
			var.name = blob.readString();
		}
	}

	bool hasInputPins() const override { return !m_inputs.empty(); }
	bool hasOutputPins() const override { return !m_outputs.empty(); }

	bool edit(const char* label, ShaderEditorResource::ValueType* type) {
		bool changed = false;
		if (ImGui::BeginCombo(label, toString(*type))) {
			if (ImGui::Selectable("bool")) { *type = ShaderEditorResource::ValueType::BOOL; changed = true; }
			if (ImGui::Selectable("int")) { *type = ShaderEditorResource::ValueType::INT; changed = true; }
			if (ImGui::Selectable("float")) { *type = ShaderEditorResource::ValueType::FLOAT; changed = true; }
			if (ImGui::Selectable("vec2")) { *type = ShaderEditorResource::ValueType::VEC2; changed = true; }
			if (ImGui::Selectable("vec3")) { *type = ShaderEditorResource::ValueType::VEC3; changed = true; }
			if (ImGui::Selectable("vec4")) { *type = ShaderEditorResource::ValueType::VEC4; changed = true; }
			ImGui::EndCombo();
		}
		return changed;
	}

	void fixLinks(u32 deleted_idx, bool is_input) {
		const ShaderEditorResource::Link* to_del = nullptr;
		if (is_input) {
			for (ShaderEditorResource::Link& link : m_resource.m_links) {
				if (link.getToNode() == m_id) {
					const u16 to_attr = link.getToPin();
					if (to_attr == deleted_idx) to_del = &link;
					else if (to_attr > deleted_idx) {
						link.to = m_id | (u32(to_attr - 1) << 16);
					}
				}
			}
		}
		else {
			for (ShaderEditorResource::Link& link : m_resource.m_links) {
				if (link.getFromNode() == m_id) {
					const u16 from_attr = link.getFromPin();
					if (from_attr == deleted_idx) to_del = &link;
					else if (from_attr > deleted_idx) {
						link.from = m_id | (u32(from_attr - 1) << 16);
					}
				}
			}
		}
		if (to_del) m_resource.m_links.erase(u32(to_del - m_resource.m_links.begin()));
	}

	bool onGUI() override {
		bool changed = false;
		ImGuiEx::NodeTitle("Code");

		ImGui::BeginGroup();
		for (const Variable& input : m_inputs) {
			inputSlot();
			ImGui::TextUnformatted(input.name.c_str());
		}
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGui::BeginGroup();
	
		if (ImGui::Button(ICON_FA_PENCIL_ALT "Edit")) ImGui::OpenPopup("edit");

		if (ImGuiEx::BeginResizablePopup("edit", ImVec2(300, 300))) {
			auto edit_vars = [&](const char* label, Array<Variable>& vars, bool is_input){
				if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::PushID(&vars);
					
					if (ImGui::BeginTable("tab", 3)) {
			            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize);
						ImGui::TableSetupColumn("Type");
						ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
						ImGui::TableHeadersRow();

						for (Variable& var : vars) {
							ImGui::PushID(&var);
							ImGui::TableNextColumn();
							const u32 idx = u32(&var - vars.begin());
							bool del = ImGui::Button(ICON_FA_TRASH);
							ImGui::TableNextColumn();
							ImGui::SetNextItemWidth(-1);
							changed = edit("##type", &var.type) || changed;
							ImGui::TableNextColumn();
							char buf[128];
							copyString(buf, var.name.c_str());
							ImGui::SetNextItemWidth(-1);
							if (ImGui::InputText("##name", buf, sizeof(buf))) {
								var.name = buf;
								changed = true;
							}
							ImGui::PopID();
							if (del) {
								changed = true;
								fixLinks(idx, is_input);
								vars.erase(idx);
								break;
							}
						}

						ImGui::EndTable();
					}

					if (ImGui::Button("Add")) {
						vars.emplace(m_allocator);
						changed = true;
					}
					ImGui::PopID();
				}
			};

			edit_vars("Inputs", m_inputs, true);
			edit_vars("Outputs", m_outputs, false);

			if (ImGui::CollapsingHeader("Code", ImGuiTreeNodeFlags_DefaultOpen)) {
				char buf[4096];
				copyString(buf, m_code.c_str());
				if (ImGui::InputTextMultiline("##code", buf, sizeof(buf), ImVec2(-1, ImGui::GetContentRegionAvail().y))) {
					m_code = buf;
					changed = true;
				}
			}
			ImGui::EndPopup();
		}
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGui::BeginGroup();
		for (const Variable& output : m_outputs) {
			outputSlot();
			ImGui::TextUnformatted(output.name.c_str());
		}
		ImGui::EndGroup();

		return changed;
	}

	void generate(OutputMemoryStream& blob) override {
		for (const Variable& input_var : m_inputs) {
			const u32 idx = u32(&input_var - m_inputs.begin());
			const Input input = getInput(m_resource, m_id, idx);
			if (!input) continue;

			input.node->generateOnce(blob);
			blob << toString(input_var.type) << " " << input_var.name.c_str() << " = ";
			input.printReference(blob);
			blob << ";\n";
		}

		for (const Variable& var : m_outputs) {
			blob << toString(var.type) << " " << var.name.c_str() << ";";
		}

		blob << m_code.c_str();
	}
	
	void printReference(OutputMemoryStream& blob, int output_idx) const override {
		blob << m_outputs[output_idx].name.c_str();
	}

	struct Variable {
		Variable(IAllocator& allocator) : name(allocator) {}
		String name;
		ShaderEditorResource::ValueType type = ShaderEditorResource::ValueType::FLOAT;
	};

	IAllocator& m_allocator;
	Array<Variable> m_inputs;
	Array<Variable> m_outputs;
	String m_code;
};

template <NodeType Type>
struct OperatorNode : ShaderEditorResource::Node {
	explicit OperatorNode(ShaderEditorResource& resource)
		: Node(Type, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override { blob.write(b_val); }
	void deserialize(InputMemoryStream& blob) override { blob.read(b_val); }

	ShaderEditorResource::ValueType getOutputType(int) const override {
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
		if (input0) {
			const ShaderEditorResource::ValueType type0 = input0.node->getOutputType(input0.output_idx);
			if (input1) {
				const ShaderEditorResource::ValueType type1 = input1.node->getOutputType(input1.output_idx);
				return pickBiggerType(type0, type1);
			}
			return type0;
		}
		return ShaderEditorResource::ValueType::FLOAT;
	}

	void generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
		if (input0) input0.node->generateOnce(blob);
		if (input1) input1.node->generateOnce(blob);
	}

	void printReference(OutputMemoryStream& blob, int attr_idx) const override
	{
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
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
			default: 
				ASSERT(false);
				return "Error";
		}
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle(getName());

		outputSlot();
		inputSlot(); ImGui::Text("A");

		inputSlot();
		if (isInputConnected(m_resource, m_id, 1)) {
			ImGui::Text("B");
		}
		else {
			ImGui::DragFloat("B", &b_val);
		}

		return false;
	}

	float b_val = 2;
};

struct OneMinusNode : ShaderEditorResource::Node {
	explicit OneMinusNode(ShaderEditorResource& resource)
		: Node(NodeType::ONEMINUS, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	ShaderEditorResource::ValueType getOutputType(int index) const override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return ShaderEditorResource::ValueType::FLOAT;
		return input.node->getOutputType(input.output_idx);
	}

	void generate(OutputMemoryStream& blob) override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return;
		input.node->generateOnce(blob);
	}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return;
		
		switch(input.node->getOutputType(input.output_idx)) {
			default : blob << "(1 - "; break;
			case ShaderEditorResource::ValueType::VEC4:
				blob << "(vec4(1) - ";
				break;
			case ShaderEditorResource::ValueType::IVEC4:
				blob << "(ivec4(1) - ";
				break;
			case ShaderEditorResource::ValueType::VEC2:
				blob << "(vec2(1) - ";
				break;
			case ShaderEditorResource::ValueType::VEC3:
				blob << "(vec3(1) - ";
				break;
		}

		input.printReference(blob);
		blob << ")";
	}

	bool onGUI() override {
		inputSlot();
		ImGui::TextUnformatted("1 - X");
		ImGui::SameLine();
		outputSlot();
		return false;
	}
};

struct SwizzleNode : ShaderEditorResource::Node {
	explicit SwizzleNode(ShaderEditorResource& resource)
		: Node(NodeType::SWIZZLE, resource)
	{
		m_swizzle = "xyzw";
	}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }
	
	void serialize(OutputMemoryStream& blob) override { blob.write(m_swizzle); }
	void deserialize(InputMemoryStream& blob) override { blob.read(m_swizzle); }
	ShaderEditorResource::ValueType getOutputType(int idx) const override { 
		// TODO other types, e.g. ivec4...
		switch(stringLength(m_swizzle)) {
			case 0: return ShaderEditorResource::ValueType::NONE;
			case 1: return ShaderEditorResource::ValueType::FLOAT;
			case 2: return ShaderEditorResource::ValueType::VEC2;
			case 3: return ShaderEditorResource::ValueType::VEC3;
			case 4: return ShaderEditorResource::ValueType::VEC4;
			default: ASSERT(false); return ShaderEditorResource::ValueType::NONE;
		}
	}
	
	void generate(OutputMemoryStream& blob) override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return;
		input.node->generateOnce(blob);
	}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return;
		
		input.printReference(blob);
		blob << "." << m_swizzle;
	}

	bool onGUI() override {
		inputSlot();
		ImGui::SetNextItemWidth(50);
		bool res = ImGui::InputTextWithHint("", "swizzle", m_swizzle.data, sizeof(m_swizzle.data));

		ImGui::SameLine();
		outputSlot();
		return res;
	}

	StaticString<5> m_swizzle;
};

struct FresnelNode : ShaderEditorResource::Node {
	explicit FresnelNode(ShaderEditorResource& resource)
		: Node(NodeType::FRESNEL, resource)
	{}

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream&blob) override {
		blob.write(F0);
		blob.write(power);
	}

	void deserialize(InputMemoryStream&blob) override {
		blob.read(F0);
		blob.read(power);
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle("Fresnel");

		outputSlot();
		ImGui::DragFloat("F0", &F0);
		ImGui::DragFloat("Power", &power);
		return false;
	}

	void generate(OutputMemoryStream& blob) override {
		// TODO use data.normal instead of v_normal
		blob << "float v" << m_id << " = mix(" << F0 << ", 1.0, pow(1 - saturate(dot(-normalize(v_wpos.xyz), v_normal)), " << power << "));\n";
	}

	float F0 = 0.04f;
	float power = 5.0f;
};

template <NodeType Type>
struct FunctionCallNode : ShaderEditorResource::Node
{
	explicit FunctionCallNode(ShaderEditorResource& resource)
		: Node(Type, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override { 
		if constexpr (Type == NodeType::LENGTH) return ShaderEditorResource::ValueType::FLOAT;
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) return input0.node->getOutputType(input0.output_idx);
		return ShaderEditorResource::ValueType::FLOAT;
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

	void generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);

		if (input0) input0.node->generateOnce(blob);

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
		inputSlot();
		ImGui::TextUnformatted(getName());
		ImGui::SameLine();
		outputSlot();
		return false;
	}
};

static u32 getChannelsCount(ShaderEditorResource::ValueType type) {
	switch (type) {
		case ShaderEditorResource::ValueType::BOOL:
		case ShaderEditorResource::ValueType::INT:
		case ShaderEditorResource::ValueType::FLOAT:
			return 1;
		case ShaderEditorResource::ValueType::VEC2:
			return 2;
		case ShaderEditorResource::ValueType::VEC3:
			return 3;
		case ShaderEditorResource::ValueType::IVEC4:
		case ShaderEditorResource::ValueType::VEC4:
			return 4;
		default:
			ASSERT(false);
			return 0;
	}
}

static ShaderEditorResource::ValueType pickBiggerType(ShaderEditorResource::ValueType t0, ShaderEditorResource::ValueType t1) {
	if (getChannelsCount(t0) > getChannelsCount(t1)) return t0;
	return t1;
}

static void makeSafeCast(OutputMemoryStream& blob, ShaderEditorResource::ValueType t0, ShaderEditorResource::ValueType t1) {
	const u32 c0 = getChannelsCount(t0);
	const u32 c1 = getChannelsCount(t1);
	if (c0 == c1) return;
	
	if (c1 == 1) {
		blob << ".";
		for (u32 i = 0; i < c0; ++i) blob << "x";
	}
	else if (c0 == 1){
		blob << ".x";
	}
}

struct PowerNode : ShaderEditorResource::Node {
	explicit PowerNode(ShaderEditorResource& resource)
		: Node(NodeType::POW, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {
		blob.write(m_exponent);
	}

	void deserialize(InputMemoryStream& blob) override {
		blob.read(m_exponent);
	}
	
	ShaderEditorResource::ValueType getOutputType(int) const override { 
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) return input0.node->getOutputType(input0.output_idx);
		return ShaderEditorResource::ValueType::FLOAT;
	}

	void generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		if (!input0) return;
		input0.node->generateOnce(blob);

		const Input input1 = getInput(m_resource, m_id, 1);
		if (input1) input1.node->generateOnce(blob);

		const char* type_str = toString(getOutputType(0));
		blob << "\t\t" << type_str << " v" << m_id << " = pow(";
		input0.printReference(blob);
		blob << ", ";
		if (input1) {
			input1.printReference(blob);
			makeSafeCast(blob
				, input0.node->getOutputType(input0.output_idx)
				, input1.node->getOutputType(input1.output_idx));
		}
		else {
			blob << type_str << "(" << m_exponent << ")";
		}
		blob << ");\n";
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle("Power");
		ImGui::BeginGroup();
		inputSlot(); ImGui::Text("Base");
		inputSlot(); 
		if (getInput(m_resource, m_id, 1)) {
			ImGui::Text("Exponent");
		}
		else {
			ImGui::DragFloat("Exponent", &m_exponent);
		}

		ImGui::EndGroup();

		ImGui::SameLine();
		outputSlot();
		return false;
	}

	float m_exponent = 2.f;
};

template <NodeType Type>
struct BinaryFunctionCallNode : ShaderEditorResource::Node
{
	explicit BinaryFunctionCallNode(ShaderEditorResource& resource)
		: Node(Type, resource)
	{
	}
	
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override { 
		switch (Type) {
			case NodeType::DISTANCE:
			case NodeType::DOT: return ShaderEditorResource::ValueType::FLOAT;
			default: break;
		}
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) return input0.node->getOutputType(input0.output_idx);
		return ShaderEditorResource::ValueType::FLOAT;
	}

	static const char* getName() {
		switch (Type) {
			case NodeType::DOT: return "dot";
			case NodeType::CROSS: return "cross";
			case NodeType::MIN: return "min";
			case NodeType::MAX: return "max";
			case NodeType::DISTANCE: return "distance";
			default: ASSERT(false); return "error";
		}
	}

	void generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
		if (input0) input0.node->generateOnce(blob);
		if (input1) input1.node->generateOnce(blob);

		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = " << getName() << "(";
		if (input0) {
			input0.printReference(blob);
		}
		else {
			blob << "1";
		}
		blob << ", ";
		if (input1) {
			input1.printReference(blob);
			if (input0) {
				makeSafeCast(blob
					, input0.node->getOutputType(input0.output_idx)
					, input1.node->getOutputType(input1.output_idx));
			}
		}
		else {
			blob << "1";
		}
		blob << ");\n";
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle(getName());
		ImGui::BeginGroup();
		inputSlot(); ImGui::Text("A");
		inputSlot(); ImGui::Text("B");
		ImGui::EndGroup();

		ImGui::SameLine();
		outputSlot();
		return false;
	}
};


struct PositionNode : ShaderEditorResource::Node {
	explicit PositionNode(ShaderEditorResource& resource)
		: Node(NodeType::POSITION, resource)
	{}

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override { blob.write(m_space); }
	void deserialize(InputMemoryStream& blob) override { blob.read(m_space); }

	ShaderEditorResource::ValueType getOutputType(int) const override { return ShaderEditorResource::ValueType::VEC3; }

	void printReference(OutputMemoryStream& blob, int output_idx) const {
		switch (m_space) {
			case CAMERA: blob << "v_wpos"; break;
			case LOCAL: blob << "v_local_position"; break;
		}
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle("Position");
		outputSlot();
		return ImGui::Combo("Space", (i32*)&m_space, "Camera\0Local\0");
	}

	enum Space : u32 {
		CAMERA,
		LOCAL
	};

	Space m_space = CAMERA;
};

template <NodeType Type>
struct VaryingNode : ShaderEditorResource::Node {
	explicit VaryingNode(ShaderEditorResource& resource)
		: Node(Type, resource)
	{}

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream&) override {}
	void deserialize(InputMemoryStream&) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override { 
		switch(Type) {
			case NodeType::NORMAL: return ShaderEditorResource::ValueType::VEC3;
			case NodeType::UV0: return ShaderEditorResource::ValueType::VEC2;
			default: ASSERT(false); return ShaderEditorResource::ValueType::VEC3;
		}
	}

	void printReference(OutputMemoryStream& blob, int output_idx) const {
		switch(Type) {
			case NodeType::NORMAL: blob << "v_normal"; break;
			case NodeType::UV0: blob << "v_uv"; break;
			default: ASSERT(false); break;
		}
		
	}

	bool onGUI() override {
		outputSlot();
		switch(Type) {
			case NodeType::NORMAL: ImGui::Text("Normal"); break;
			case NodeType::UV0: ImGui::Text("UV0"); break;
			default: ASSERT(false); break;
		}
		return false;
	}
};

template <ShaderEditorResource::ValueType TYPE>
struct ConstNode : ShaderEditorResource::Node
{
	explicit ConstNode(ShaderEditorResource& resource)
		: Node(toNodeType(TYPE), resource)
	{
		m_type = TYPE;
		m_value[0] = m_value[1] = m_value[2] = m_value[3] = 0;
		m_int_value = 0;
	}

	static NodeType toNodeType(ShaderEditorResource::ValueType t) {
		switch(t) {
			case ShaderEditorResource::ValueType::VEC4: return NodeType::VEC4;
			case ShaderEditorResource::ValueType::VEC3: return NodeType::VEC3;
			case ShaderEditorResource::ValueType::VEC2: return NodeType::VEC2;
			case ShaderEditorResource::ValueType::FLOAT: return NodeType::NUMBER;
			default: ASSERT(false); return NodeType::NUMBER;
		}
	}

	void serialize(OutputMemoryStream& blob) override
	{
		blob.write(m_value);
		blob.write(m_type);
		blob.write(m_int_value);
	}

	void deserialize(InputMemoryStream& blob) override 
	{
		blob.read(m_value);
		blob.read(m_type);
		blob.read(m_int_value);
	}

	ShaderEditorResource::ValueType getOutputType(int) const override { return m_type; }

	void printInputValue(u32 idx, OutputMemoryStream& blob) const {
		const Input input = getInput(m_resource, m_id, idx);
		if (input) {
			input.printReference(blob);
			return;
		}
		blob << m_value[idx];
	}

	void generate(OutputMemoryStream& blob) override {
		for (u32 i = 0; i < 4; ++i) {
			const Input input = getInput(m_resource, m_id, i);
			if (input) input.node->generateOnce(blob);
		}
	}

	void printReference(OutputMemoryStream& blob, int output_idx) const override {
		switch(m_type) {
			case ShaderEditorResource::ValueType::VEC4:
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
			case ShaderEditorResource::ValueType::VEC3:
				blob << "vec3(";
				printInputValue(0, blob);
				blob << ", "; 
				printInputValue(1, blob);
				blob << ", "; 
				printInputValue(2, blob);
				blob << ")";
				break;
			case ShaderEditorResource::ValueType::VEC2:
				blob << "vec2(";
				printInputValue(0, blob);
				blob << ", "; 
				printInputValue(1, blob);
				blob << ")";
				break;
			case ShaderEditorResource::ValueType::INT:
				blob << m_int_value;
				break;
			case ShaderEditorResource::ValueType::FLOAT:
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
			case ShaderEditorResource::ValueType::VEC4: channels_count = 4; break;
			case ShaderEditorResource::ValueType::VEC3: channels_count = 3; break;
			case ShaderEditorResource::ValueType::VEC2: channels_count = 2; break;
			default: channels_count = 1; break;
		}
			
		switch(m_type) {
			case ShaderEditorResource::ValueType::VEC4:
			case ShaderEditorResource::ValueType::VEC3:
			case ShaderEditorResource::ValueType::VEC2:
				for (u16 i = 0; i < channels_count; ++i) {
					inputSlot();
					if (isInputConnected(m_resource, m_id, i)) {
						ImGui::TextUnformatted(labels[i]);
					}
					else {
						res = ImGui::DragFloat(labels[i], &m_value[i]);
					}
				}
				switch (m_type) {
					case ShaderEditorResource::ValueType::VEC4:
						res = ImGui::ColorEdit4("##col", m_value, ImGuiColorEditFlags_NoInputs) || res; 
						break;
					case ShaderEditorResource::ValueType::VEC3:
						res = ImGui::ColorEdit3("##col", m_value, ImGuiColorEditFlags_NoInputs) || res; 
						break;
					default: break;
				}
				break;
			case ShaderEditorResource::ValueType::FLOAT:
				ImGui::SetNextItemWidth(60);
				res = ImGui::DragFloat("##val", m_value) || res;
				break;
			case ShaderEditorResource::ValueType::INT:
				ImGui::SetNextItemWidth(60);
				res = ImGui::InputInt("##val", &m_int_value) || res;
				break;
			default: ASSERT(false); break;
		}
		ImGui::EndGroup();

		ImGui::SameLine();
		outputSlot();

		return res;
	}

	bool hasInputPins() const override { 
		switch (m_type) {
			case ShaderEditorResource::ValueType::VEC4:
			case ShaderEditorResource::ValueType::VEC3:
			case ShaderEditorResource::ValueType::VEC2:
				return true;
			default: return false;
		}
	}

	bool hasOutputPins() const override { return true; }

	ShaderEditorResource::ValueType m_type;
	float m_value[4];
	int m_int_value;
};


struct SampleNode : ShaderEditorResource::Node
{
	explicit SampleNode(ShaderEditorResource& resource, IAllocator& allocator)
		: Node(NodeType::SAMPLE, resource)
		, m_texture(allocator)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override { blob.writeString(m_texture.c_str()); }
	void deserialize(InputMemoryStream& blob) override { m_texture = blob.readString(); }
	ShaderEditorResource::ValueType getOutputType(int) const override { return ShaderEditorResource::ValueType::VEC4; }

	void generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) input0.node->generateOnce(blob);
		blob << "\t\tvec4 v" << m_id << " = ";
		char var_name[64];
		Shader::toTextureVarName(Span(var_name), m_texture.c_str());
		blob << "texture(" << var_name << ", ";
		if (input0) input0.printReference(blob);
		else blob << "v_uv";
		blob << ");\n";
	}

	bool onGUI() override {
		inputSlot();
		ImGui::Text("UV");

		ImGui::SameLine();
		outputSlot();
		char tmp[128];
		copyString(tmp, m_texture.c_str());
		bool res = ImGui::InputText("Texture", tmp, sizeof(tmp));
		if (res) m_texture = tmp;
		return res;
	}

	String m_texture;
};

struct AppendNode : ShaderEditorResource::Node {
	explicit AppendNode(ShaderEditorResource& resource)
	: Node(NodeType::APPEND, resource)
	{}
	
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		ImGuiEx::NodeTitle("Append");

		ImGui::BeginGroup();
		inputSlot();
		ImGui::TextUnformatted("A");
		inputSlot();
		ImGui::TextUnformatted("B");
		ImGui::EndGroup();

		ImGui::SameLine();
		outputSlot();
		return false;
	}

	static u32 getChannelCount(ShaderEditorResource::ValueType type) {
		switch (type) {
			case ShaderEditorResource::ValueType::FLOAT:
			case ShaderEditorResource::ValueType::BOOL:
			case ShaderEditorResource::ValueType::INT:
				return 1;
			case ShaderEditorResource::ValueType::VEC2:
				return 2;
			case ShaderEditorResource::ValueType::VEC3:
				return 3;
			case ShaderEditorResource::ValueType::IVEC4:
			case ShaderEditorResource::ValueType::VEC4:
				return 4;
			default:
				// TODO handle mat3 & co.
				ASSERT(false);
				return 1;
		}
	}

	ShaderEditorResource::ValueType getOutputType(int index) const override {
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
		u32 count = 0;
		if (input0) count += getChannelCount(input0.node->getOutputType(input0.output_idx));
		if (input1) count += getChannelCount(input1.node->getOutputType(input1.output_idx));
		// TODO other types likes ivec4
		switch (count) {
			case 1: return ShaderEditorResource::ValueType::FLOAT;
			case 2: return ShaderEditorResource::ValueType::VEC2;
			case 3: return ShaderEditorResource::ValueType::VEC3;
			case 4: return ShaderEditorResource::ValueType::VEC4;
			default: ASSERT(false); return ShaderEditorResource::ValueType::FLOAT;
		}
	}

	void generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) input0.node->generateOnce(blob);
		const Input input1 = getInput(m_resource, m_id, 1);
		if (input1) input1.node->generateOnce(blob);
	}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
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

struct StaticSwitchNode : ShaderEditorResource::Node {
	explicit StaticSwitchNode(ShaderEditorResource& resource, IAllocator& allocator)
		: Node(NodeType::STATIC_SWITCH, resource)
		, m_define(allocator)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		ImGuiEx::NodeTitle("Static switch");
		
		ImGui::BeginGroup();
		inputSlot();
		ImGui::TextUnformatted("True");
		inputSlot();
		ImGui::TextUnformatted("False");
		ImGui::EndGroup();

		ImGui::SameLine();
		outputSlot();
		char tmp[128];
		copyString(tmp, m_define.c_str());
		ImGui::SetNextItemWidth(80);
		bool res = ImGui::InputText("##param", tmp, sizeof(tmp));
		if (res) m_define = tmp;
		return res;
	}

	void serialize(OutputMemoryStream& blob) override { blob.writeString(m_define.c_str()); }
	void deserialize(InputMemoryStream& blob) override { m_define = blob.readString(); }
	
	const char* getOutputTypeName() const {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return "float";
		return toString(input.node->getOutputType(input.output_idx));
	}

	void generate(OutputMemoryStream& blob) override {
		blob << "#ifdef " << m_define.c_str() << "\n";
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) {
			input0.node->generateOnce(blob);
			blob << getOutputTypeName() << " v" << m_id << " = ";
			input0.printReference(blob);
			blob << ";\n";
		}
		blob << "#else\n";
		const Input input1 = getInput(m_resource, m_id, 1);
		if (input1) {
			input1.node->generateOnce(blob);
			blob << getOutputTypeName() << " v" << m_id << " = "; 
			input1.printReference(blob);
			blob << ";\n";
		}
		blob << "#endif\n";
	}
	
	ShaderEditorResource::ValueType getOutputType(int) const override {
		const Input input = getInput(m_resource, m_id, 0);
		if (input) return input.node->getOutputType(input.output_idx);
		return ShaderEditorResource::ValueType::FLOAT;
	}

	String m_define;
};

template <NodeType Type>
struct ParameterNode : ShaderEditorResource::Node {
	explicit ParameterNode(ShaderEditorResource& resource, IAllocator& allocator)
		: Node(Type, resource)
		, m_name(allocator)
	{}

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override { blob.writeString(m_name.c_str()); }
	void deserialize(InputMemoryStream& blob) override { m_name = blob.readString(); }

	bool onGUI() override {
		const ImU32 color = ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
		switch(Type) {
			case NodeType::SCALAR_PARAM: ImGuiEx::NodeTitle("Scalar param", color); break;
			case NodeType::VEC4_PARAM: ImGuiEx::NodeTitle("Vec4 param", color); break;
			case NodeType::COLOR_PARAM: ImGuiEx::NodeTitle("Color param", color); break;
			default: ASSERT(false); ImGuiEx::NodeTitle("Error"); break;
		}
		
		outputSlot();
		char tmp[128];
		copyString(tmp, m_name.c_str());
		bool res = ImGui::InputText("##name", tmp, sizeof(tmp));
		if (res) m_name = tmp;
		return res;
	}

	void generate(OutputMemoryStream& blob) override {
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

struct PinNode : ShaderEditorResource::Node {
	explicit PinNode(ShaderEditorResource& resource)
		: Node(NodeType::PIN, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }
	
	void generate(OutputMemoryStream& blob) override {
		const Input input = getInput(m_resource, m_id, 0);
		if (input) input.node->generateOnce(blob);
	}

	void printReference(OutputMemoryStream& blob, int output_idx) const override {
		const Input input = getInput(m_resource, m_id, 0);
		if (input) input.printReference(blob);
	}

	bool onGUI() override { 
		inputSlot();
		ImGui::TextUnformatted(" ");
		ImGui::SameLine();
		outputSlot();
		return false;
	}
};

struct PBRNode : ShaderEditorResource::Node
{
	explicit PBRNode(ShaderEditorResource& resource)
		: Node(NodeType::PBR, resource)
		, m_vertex_decl(gpu::PrimitiveType::TRIANGLE_STRIP)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return false; }

	void serialize(OutputMemoryStream& blob) override {
		blob.write(m_type);
		blob.write(m_vertex_decl);
	}

	void deserialize(InputMemoryStream& blob) override {
		blob.read(m_type);
		blob.read(m_vertex_decl);
	}


	static void generate(OutputMemoryStream& blob, Node* node) {
		if (!node) return;
		forEachInput(node->m_resource, node->m_id, [&](ShaderEditorResource::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
			generate(blob, from);
		});
		node->generateOnce(blob);
	}

	static const char* typeToString(const gpu::Attribute& attr) {
		switch (attr.type) {
			case gpu::AttributeType::FLOAT:
				break;
			case gpu::AttributeType::I16:
			case gpu::AttributeType::I8:
				if (attr.flags & gpu::Attribute::AS_INT) {
					switch (attr.components_count) {
						case 1: return "int";
						case 2: return "ivec2";
						case 3: return "ivec3";
						case 4: return "ivec4";
					}
					ASSERT(false);
					return "int";
				}
				break;
			case gpu::AttributeType::U8:
				if (attr.flags & gpu::Attribute::AS_INT) {
					switch (attr.components_count) {
						case 1: return "uint";
						case 2: return "uvec2";
						case 3: return "uvec3";
						case 4: return "uvec4";
					}
					ASSERT(false);
					return "int";
				}
				break;
		}
		switch (attr.components_count) {
			case 1: return "float";
			case 2: return "vec2";
			case 3: return "vec3";
			case 4: return "vec4";
		}
		ASSERT(false);
		return "float";
	}

	void generate(OutputMemoryStream& blob) override {
		blob << "import \"pipelines/surface_base.inc\"\n\n";
	
		IAllocator& allocator = m_resource.m_allocator;
		Array<String> uniforms(allocator);
		Array<String> defines(allocator);
		Array<String> textures(allocator);
	
		auto add_uniform = [&](auto* n, const char* type) {
			const i32 idx = uniforms.find([&](const String& u) { return u == n->m_name; });
			if (idx < 0) {
				uniforms.emplace(n->m_name.c_str(), allocator);
				blob << "uniform(\"" << n->m_name.c_str() << "\", \"" << type << "\")\n";
			}
		};
	
		auto add_define = [&](StaticSwitchNode* n){
			const i32 idx = defines.find([&](const String& u) { return u == n->m_define; });
			if (idx < 0) {
				defines.emplace(n->m_define.c_str(), allocator);
				blob << "define(\"" << n->m_define.c_str() << "\")\n";
			}
		};
	
		auto add_texture = [&](SampleNode* n){
			const i32 idx = textures.find([&](const String& u) { return u == n->m_texture; });
			if (idx < 0) {
				textures.emplace(n->m_texture.c_str(), allocator);
				blob << "{\n"
					<< "\tname = \"" << n->m_texture.c_str() << "\",\n"
					<< "\tdefault_texture = \"textures/common/white.tga\"\n"
					<< "}\n";
			}
		};
	
		for (Node* n : m_resource.m_nodes) {
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
				default: break;
			}
		}
	
		if (m_type == Type::PARTICLES) {
			blob << "common(\"#define PARTICLES\\n\")\n";
		}

		blob << "surface_shader_ex({\n";
		blob << "texture_slots = {\n";
		for (Node* n : m_resource.m_nodes) {
			if (!n->m_reachable) continue;
			if (n->m_type == NodeType::SAMPLE) add_texture((SampleNode*)n);
		}
		blob << "},\n";
	
		if (m_type == Type::PARTICLES) {
			blob << "vertex_preface = [[\n";
			for (u32 i = 0; i < m_vertex_decl.attributes_count; ++i) {
				blob << "\tlayout(location = " << i << ") in " << typeToString(m_vertex_decl.attributes[i]) << " i_" << i << ";\n";
			}
			blob << R"#(
					layout (location = 0) out vec2 v_uv;
				]],
				vertex = [[
					vec2 pos = vec2(gl_VertexID & 1, (gl_VertexID & 2) * 0.5);
					v_uv = pos;
					pos = pos * 2 - 1;
					gl_Position = Pass.projection * ((Pass.view * u_model * vec4(i_0.xyz, 1)) + vec4(pos.xy, 0, 0));
				]],
				fragment_preface = [[
					layout (location = 0) in vec2 v_uv;
				]],
			)#";
		}

		blob << "fragment = [[\n";
	
		bool need_local_position = false;
		for (Node* n : m_resource.m_nodes) {
			if (n->m_type == NodeType::POSITION) {
				need_local_position = need_local_position || ((PositionNode*)n)->m_space == PositionNode::LOCAL;
			}
			n->m_generated = false;
		}

		const struct {
			const char* name;
			const char* default_value;
			const char* particle_default = nullptr;
		}
		fields[] = { 
			{ "albedo", "vec3(1, 0, 1)" },
			{ "N", "v_normal", "vec3(0, 1, 0)" },
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
			Input input = getInput(m_resource, m_id, i);
			if (input) {
				input.node->generateOnce(blob);
				blob << "\tdata." << field.name << " = ";
				if (i < 2) blob << "vec3(";
				input.printReference(blob);
				const ShaderEditorResource::ValueType type = input.node->getOutputType(input.output_idx);
				if (i == 0) {
					switch(type) {
						case ShaderEditorResource::ValueType::IVEC4:
						case ShaderEditorResource::ValueType::VEC4:
							blob << ".rgb"; break;
						case ShaderEditorResource::ValueType::VEC3:
							break;
						case ShaderEditorResource::ValueType::VEC2:
							blob << ".rgr"; break;
						case ShaderEditorResource::ValueType::BOOL:
						case ShaderEditorResource::ValueType::INT:
						case ShaderEditorResource::ValueType::FLOAT:
							break;
						case ShaderEditorResource::ValueType::COUNT:
						case ShaderEditorResource::ValueType::NONE:
						case ShaderEditorResource::ValueType::MATRIX3:
						case ShaderEditorResource::ValueType::MATRIX4: 
							// invalid data
							break;
					}
				}
				else if (type != ShaderEditorResource::ValueType::VEC3 && i < 2) blob << ".rgb";
				else if (type != ShaderEditorResource::ValueType::FLOAT && i >= 2) blob << ".x";
				if (i < 2) blob << ")";
				blob << ";\n";
			}
			else {
				if (m_type == Type::PARTICLES && field.particle_default) {
					blob << "\tdata." << field.name << " = " << field.particle_default << ";\n";
				}
				else {
					blob << "\tdata." << field.name << " = " << field.default_value << ";\n";
				}
			}
		}

		blob << "data.V = vec3(0);\n";
		blob << "data.wpos = vec3(0);\n";
		blob << "]]\n";
		if (need_local_position) blob << ",\nneed_local_position = true\n";
		blob << "})\n";
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle(m_type == Type::SURFACE ? "PBR Surface" : "PBR Particles");
		
		inputSlot();
		ImGui::TextUnformatted("Albedo");

		inputSlot();
		ImGui::TextUnformatted("Normal");

		inputSlot();
		ImGui::TextUnformatted("Opacity");

		inputSlot();
		ImGui::TextUnformatted("Roughness");

		inputSlot();
		ImGui::TextUnformatted("Metallic");

		inputSlot();
		ImGui::TextUnformatted("Emission");

		inputSlot();
		ImGui::TextUnformatted("AO");

		inputSlot();
		ImGui::TextUnformatted("Translucency");

		inputSlot();
		ImGui::TextUnformatted("Shadow");

		if (m_type == Type::PARTICLES && ImGui::Button("Copy vertex declaration")) {
			m_show_fs = true;
		}

		FileSelector& fs = m_resource.m_app.getFileSelector();
		if (fs.gui("Select particle", &m_show_fs, "par", false)) {
			m_vertex_decl = ParticleEditor::getVertexDecl(fs.getPath(), m_resource.m_app);
		}

		return false;
	}

	enum class Type : u32 {
		SURFACE,
		PARTICLES
	};

	gpu::VertexDecl m_vertex_decl;
	Type m_type = Type::SURFACE;
	bool m_show_fs = false;
};

struct BackfaceSwitchNode : ShaderEditorResource::Node {
	explicit BackfaceSwitchNode(ShaderEditorResource& resource)
		: Node(NodeType::BACKFACE_SWITCH, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override {
		const Input inputA = getInput(m_resource, m_id, 0);
		if (inputA) {
			return inputA.node->getOutputType(inputA.output_idx);
		}
		const Input inputB = getInput(m_resource, m_id, 1);
		if (inputB) {
			return inputB.node->getOutputType(inputB.output_idx);
		}
		return ShaderEditorResource::ValueType::FLOAT;
	}

	void generate(OutputMemoryStream& blob) override {
		const Input inputA = getInput(m_resource, m_id, 0);
		const Input inputB = getInput(m_resource, m_id, 1);
		if (!inputA && !inputB) return;
		
		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << ";\n";
		if (inputA) {
			blob << "\tif (gl_FrontFacing) {\n";
					inputA.node->generateOnce(blob);
					blob << "\t\tv" << m_id << " = ";
					inputA.printReference(blob);
					blob << ";\n\t}\n";
		}
		if (inputB) {
			blob << "\tif (!gl_FrontFacing) {\n";
					inputB.node->generateOnce(blob);
					blob << "\t\tv" << m_id << " = ";
					inputB.printReference(blob);
					blob << ";\n\t}\n";
		}
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle("Backface switch");
		outputSlot();
		inputSlot(); ImGui::TextUnformatted("Front");
		inputSlot(); ImGui::TextUnformatted("Back");
		return false;
	}
};

struct IfNode : ShaderEditorResource::Node {
	explicit IfNode(ShaderEditorResource& resource)
		: Node(NodeType::IF, resource)
	{
	}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	void generate(OutputMemoryStream& blob) override {
		const Input inputA = getInput(m_resource, m_id, 0);
		const Input inputB = getInput(m_resource, m_id, 1);
		const Input inputGT = getInput(m_resource, m_id, 2);
		const Input inputEQ = getInput(m_resource, m_id, 3);
		const Input inputLT = getInput(m_resource, m_id, 4);
		if (!inputA || !inputB) return;
		if (!inputGT && !inputEQ && !inputLT) return;
		
		inputA.node->generateOnce(blob);
		inputB.node->generateOnce(blob);

		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << ";\n";
		if (inputGT) {
			inputGT.node->generateOnce(blob);
			
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
			inputEQ.node->generateOnce(blob);

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
			inputLT.node->generateOnce(blob);

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
		inputSlot();
		ImGui::Text("A");
		
		inputSlot();
		ImGui::Text("B");

		inputSlot();
		ImGui::Text("A > B");

		inputSlot();
		ImGui::Text("A == B");

		inputSlot();
		ImGui::Text("A < B");
		ImGui::EndGroup();

		ImGui::SameLine();

		outputSlot();
		ImGui::TextUnformatted("Output");

		return false;
	}
};

struct VertexIDNode : ShaderEditorResource::Node
{
	explicit VertexIDNode(ShaderEditorResource& resource)
		: Node(NodeType::VERTEX_ID, resource)
	{}

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
		blob << "gl_VertexID";
	}

	ShaderEditorResource::ValueType getOutputType(int) const override
	{
		return ShaderEditorResource::ValueType::INT;
	}

	bool onGUI() override {
		outputSlot();
		ImGui::Text("Vertex ID");
		return false;
	}
};

template <NodeType Type>
struct UniformNode : ShaderEditorResource::Node
{
	explicit UniformNode(ShaderEditorResource& resource)
		: Node(Type, resource)
	{}

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	void printReference(OutputMemoryStream& blob,  int output_idx) const override
	{
		blob << getVarName();
	}

	ShaderEditorResource::ValueType getOutputType(int) const override
	{
		switch (Type) {
			case NodeType::SCREEN_POSITION: return ShaderEditorResource::ValueType::VEC2;
			case NodeType::VIEW_DIR: return ShaderEditorResource::ValueType::VEC3;
			case NodeType::SCENE_DEPTH:
			case NodeType::PIXEL_DEPTH:
			case NodeType::TIME:
				return ShaderEditorResource::ValueType::FLOAT;
			default: 
				ASSERT(false);
				return ShaderEditorResource::ValueType::FLOAT;
		}
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
		outputSlot();
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
		const u32 i = u32(&p - m_recent_paths.begin());		const StaticString<32> key("shader_editor_recent_", i);
		settings.setValue(Settings::LOCAL, key, p.c_str());
	}
}

ShaderEditor::ShaderEditor(StudioApp& app)
	: m_allocator(app.getAllocator())
	, m_app(app)
	, m_source(app.getAllocator())
	, m_is_focused(false)
	, m_is_open(false)
	, m_recent_paths(app.getAllocator())
	, NodeEditor(app.getAllocator())
{
	newGraph(false);
	m_save_action.init(ICON_FA_SAVE "Save", "Shader editor save", "shader_editor_save", ICON_FA_SAVE, os::Keycode::S, Action::Modifiers::CTRL, true);
	m_save_action.func.bind<&ShaderEditor::save>(this);
	m_save_action.plugin = this;

	m_generate_action.init("Generate shader", "Shader editor generate", "shader_editor_generate", ICON_FA_CHECK, os::Keycode::E, Action::Modifiers::CTRL, true);
	m_generate_action.func.bind<&ShaderEditor::generateAndSaveSource>(this);
	m_generate_action.plugin = this;


	m_undo_action.init(ICON_FA_UNDO "Undo", "Shader editor undo", "shader_editor_undo", ICON_FA_UNDO, os::Keycode::Z, Action::Modifiers::CTRL, true);
	m_undo_action.func.bind<&ShaderEditor::undo>((SimpleUndoRedo*)this);
	m_undo_action.plugin = this;

	m_redo_action.init(ICON_FA_REDO "Redo", "Shader editor redo", "shader_editor_redo", ICON_FA_REDO, os::Keycode::Z, Action::Modifiers::CTRL | Action::Modifiers::SHIFT, true);
	m_redo_action.func.bind<&ShaderEditor::redo>((SimpleUndoRedo*)this);
	m_redo_action.plugin = this;

	m_delete_action.init(ICON_FA_TRASH "Delete", "Shader editor delete", "shader_editor_delete", ICON_FA_TRASH, os::Keycode::DEL, Action::Modifiers::NONE, true);
	m_delete_action.func.bind<&ShaderEditor::deleteSelectedNodes>(this);
	m_delete_action.plugin = this;

	m_toggle_ui.init("Shader Editor", "Toggle shader editor", "shaderEditor", "", true);
	m_toggle_ui.func.bind<&ShaderEditor::onToggle>(this);
	m_toggle_ui.is_selected.bind<&ShaderEditor::isOpen>(this);

	m_app.addWindowAction(&m_toggle_ui);
	m_app.addAction(&m_generate_action);
	m_app.addAction(&m_save_action);
	m_app.addAction(&m_undo_action);
	m_app.addAction(&m_redo_action);
	m_app.addAction(&m_delete_action);
}

void ShaderEditor::deleteSelectedNodes() {
	if (m_is_any_item_active) return;
	m_resource->deleteSelectedNodes();
	pushUndo(NO_MERGE_UNDO);
}

void ShaderEditor::onToggle() { 
	m_is_open = !m_is_open;
}

ShaderEditor::~ShaderEditor()
{
	m_app.removeAction(&m_toggle_ui);
	m_app.removeAction(&m_generate_action);
	m_app.removeAction(&m_save_action);
	m_app.removeAction(&m_undo_action);
	m_app.removeAction(&m_redo_action);
	m_app.removeAction(&m_delete_action);
	LUMIX_DELETE(m_allocator, m_resource);
}

void ShaderEditorResource::deleteSelectedNodes() {
	for (i32 i = m_nodes.size() - 1; i > 0; --i) { // we really don't want to delete node 0 (output)
		Node* node = m_nodes[i];
		if (node->m_selected) {
			for (i32 j = m_links.size() - 1; j >= 0; --j) {
				if (m_links[j].getFromNode() == node->m_id || m_links[j].getToNode() == node->m_id) {
					m_links.erase(j);
				}
			}

			LUMIX_DELETE(m_allocator, node);
			m_nodes.swapAndPop(i);
		}
	}
}

void ShaderEditorResource::markReachable(Node* node) const {
	node->m_reachable = true;

	forEachInput(*this, node->m_id, [&](ShaderEditorResource::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
		markReachable(from);
	});
}

void ShaderEditorResource::colorLinks(ImU32 color, u32 link_idx) {
	m_links[link_idx].color = color;
	const u32 from_node_id = m_links[link_idx].getFromNode();
	for (u32 i = 0, c = m_links.size(); i < c; ++i) {
		if (m_links[i].getToNode() == from_node_id) colorLinks(color, i);
	}
}

void ShaderEditorResource::colorLinks() {
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

	forEachInput(*this, m_nodes[0]->m_id, [&](ShaderEditorResource::Node* from, u16 from_attr, u16 to_attr, u32 link_idx) {
		colorLinks(colors[to_attr % lengthOf(colors)], link_idx);
	});
}

void ShaderEditorResource::markReachableNodes() const {
	for (Node* n : m_nodes) {
		n->m_reachable = false;
	}
	markReachable(m_nodes[0]);
}

void ShaderEditor::saveSource() {
	PathInfo fi(m_path.c_str());
	StaticString<LUMIX_MAX_PATH> path(fi.m_dir, fi.m_basename, ".shd");
	os::OutputFile file;
	if (!file.open(path)) {
		logError("Could not create file ", path);
		return;
	}

	if (!file.write(m_source.c_str(), m_source.length())) {
		file.close();
		logError("Could not write ", path);
		return;
	}
	file.close();
}

void ShaderEditor::generateAndSaveSource() {
	m_source = m_resource->generate();
	saveSource();
}

String ShaderEditorResource::generate() {
	markReachableNodes();
	colorLinks();

	OutputMemoryStream blob(m_allocator);
	blob.reserve(32 * 1024);

	m_nodes[0]->generateOnce(blob);

	String res(m_allocator);
	res.resize((u32)blob.size());
	memcpy(res.getData(), blob.data(), res.length());
	res.getData()[res.length()] = '\0';
	return static_cast<String&&>(res);
}


void ShaderEditorResource::serializeNode(OutputMemoryStream& blob, Node& node)
{
	int type = (int)node.m_type;
	blob.write(node.m_id);
	blob.write(type);
	blob.write(node.m_pos);

	node.serialize(blob);
}

void ShaderEditor::saveAs(const char* path) {
	os::OutputFile file;
	FileSystem& fs = m_app.getEngine().getFileSystem();
	
	OutputMemoryStream blob(m_allocator);
	m_resource->serialize(blob);
	if (!fs.saveContentSync(Path(path), blob)) {
		logError("Could not save ", path);
		return;
	}

	pushRecent(path);
	m_path = path;
}

void ShaderEditorResource::serialize(OutputMemoryStream& blob) {
	blob.reserve(4096);
	blob.write(u32('_LSE'));
	blob.write(Version::LAST);
	blob.write(m_last_node_id);

	const i32 nodes_count = m_nodes.size();
	blob.write(nodes_count);
	for(auto* node : m_nodes) {
		serializeNode(blob, *node);
	}

	const i32 links_count = m_links.size();
	blob.write(links_count);
	for (Link& l : m_links) {
		blob.write(l.from);
		blob.write(l.to);
	}

	generate();
	colorLinks();
}

void ShaderEditor::clear() {
	LUMIX_DELETE(m_allocator, m_resource);
	m_resource = LUMIX_NEW(m_allocator, ShaderEditorResource)(m_app);
	clearUndoStack();
}

ShaderEditorResource::Node* ShaderEditorResource::createNode(int type) {
	switch ((NodeType)type) {
		case NodeType::PBR:							return LUMIX_NEW(m_allocator, PBRNode)(*this);
		case NodeType::PIN:							return LUMIX_NEW(m_allocator, PinNode)(*this);
		case NodeType::VEC4:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC4>)(*this);
		case NodeType::VEC3:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC3>)(*this);
		case NodeType::VEC2:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC2>)(*this);
		case NodeType::NUMBER:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::FLOAT>)(*this);
		case NodeType::SAMPLE:						return LUMIX_NEW(m_allocator, SampleNode)(*this, m_allocator);
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
		case NodeType::BACKFACE_SWITCH:				return LUMIX_NEW(m_allocator, BackfaceSwitchNode)(*this);
		case NodeType::IF:							return LUMIX_NEW(m_allocator, IfNode)(*this);
		case NodeType::STATIC_SWITCH:				return LUMIX_NEW(m_allocator, StaticSwitchNode)(*this, m_allocator);
		case NodeType::ONEMINUS:					return LUMIX_NEW(m_allocator, OneMinusNode)(*this);
		case NodeType::CODE:						return LUMIX_NEW(m_allocator, CodeNode)(*this, m_allocator);
		case NodeType::APPEND:						return LUMIX_NEW(m_allocator, AppendNode)(*this);
		case NodeType::FRESNEL:						return LUMIX_NEW(m_allocator, FresnelNode)(*this);
		case NodeType::POSITION:					return LUMIX_NEW(m_allocator, PositionNode)(*this);
		case NodeType::NORMAL:						return LUMIX_NEW(m_allocator, VaryingNode<NodeType::NORMAL>)(*this);
		case NodeType::UV0:							return LUMIX_NEW(m_allocator, VaryingNode<NodeType::UV0>)(*this);
		case NodeType::SCALAR_PARAM:				return LUMIX_NEW(m_allocator, ParameterNode<NodeType::SCALAR_PARAM>)(*this, m_allocator);
		case NodeType::COLOR_PARAM:					return LUMIX_NEW(m_allocator, ParameterNode<NodeType::COLOR_PARAM>)(*this, m_allocator);
		case NodeType::VEC4_PARAM:					return LUMIX_NEW(m_allocator, ParameterNode<NodeType::VEC4_PARAM>)(*this, m_allocator);
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
		case NodeType::POW:							return LUMIX_NEW(m_allocator, PowerNode)(*this);
		case NodeType::DISTANCE:					return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::DISTANCE>)(*this);
	}

	ASSERT(false);
	return nullptr;
}

ShaderEditorResource::Node& ShaderEditorResource::deserializeNode(InputMemoryStream& blob) {
	int type;
	u16 id;
	blob.read(id);
	blob.read(type);
	Node* node = createNode(type);
	node->m_id = id;
	m_nodes.push(node);
	blob.read(node->m_pos);

	node->deserialize(blob);
	return *node;
}

void ShaderEditor::load(const char* path) {
	m_path = path;

	clear();

	FileSystem& fs = m_app.getEngine().getFileSystem();
	OutputMemoryStream data(m_allocator);
	if (!fs.getContentSync(Path(path), data)) {
		logError("Failed to load ", path);
		return;
	}

	InputMemoryStream blob(data);
	m_resource->deserialize(blob);

	clearUndoStack();
	pushUndo(NO_MERGE_UNDO);
	pushRecent(path);
}

void ShaderEditor::pushRecent(const char* path) {
	String p(path, m_app.getAllocator());
	m_recent_paths.eraseItems([&](const String& s) { return s == path; });
	m_recent_paths.push(static_cast<String&&>(p));
}

bool ShaderEditorResource::deserialize(InputMemoryStream& blob) {
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
		deserializeNode(blob);
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

static ImVec2 operator+(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x + b.x, a.y + b.y);
}


static ImVec2 operator-(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x - b.x, a.y - b.y);
}

ShaderEditorResource::Node* ShaderEditor::addNode(NodeType node_type, ImVec2 pos, bool save_undo) {
	Node* n = m_resource->createNode((int)node_type);
	n->m_id = ++m_resource->m_last_node_id;
	n->m_pos = pos;
	m_resource->m_nodes.push(n);
	if (m_half_link_start) {
		if (m_half_link_start & OUTPUT_FLAG) {
			if (n->hasInputPins()) m_resource->m_links.push({u32(m_half_link_start) & ~OUTPUT_FLAG, u32(n->m_id)});
		}
		else {
			if (n->hasOutputPins()) m_resource->m_links.push({u32(n->m_id), u32(m_half_link_start)});
		}
		m_half_link_start = 0;
	}
	if (save_undo) pushUndo(NO_MERGE_UNDO);
	return n;
}

static void nodeGroupUI(ShaderEditor& editor, Span<const NodeTypeDesc> nodes, ImVec2 pos) {
	if (nodes.length() == 0) return;

	const NodeTypeDesc* n = nodes.begin();
	const char* group = n->group;

	bool open = !group || ImGui::BeginMenu(group);
	while (n != nodes.end() && n->group == nodes.begin()->group) {
		if (open && ImGui::MenuItem(n->name)) {
			editor.addNode(n->type, pos, true);
		}
		++n;
	}
	if (open && group) ImGui::EndMenu();

	nodeGroupUI(editor, Span(n, nodes.end()), pos);
}

void ShaderEditor::onCanvasClicked(ImVec2 pos, i32 hovered_link) {
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
			addNode(t.type, pos, false);
			if (hovered_link >= 0) splitLink(m_resource->m_nodes.back(), m_resource->m_links, hovered_link);
			pushUndo(NO_MERGE_UNDO);
			break;
		}
	}
}

void ShaderEditor::onLinkDoubleClicked(ShaderEditor::Link& link, ImVec2 pos) {
	ShaderEditorResource::Node* n = addNode(NodeType::PIN, pos, false);
	ShaderEditorResource::Link new_link;
	new_link.color = link.color;
	new_link.from = n->m_id | OUTPUT_FLAG; 
	new_link.to = link.to;
	link.to = n->m_id;
	getResource()->m_links.push(new_link);
	pushUndo(SimpleUndoRedo::NO_MERGE_UNDO);
}

void ShaderEditor::onContextMenu(ImVec2 pos) {
	static char filter[64] = "";
	ImGui::SetNextItemWidth(150);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
	if (filter[0]) {
		for (const auto& node_type : NODE_TYPES) {
			if (stristr(node_type.name, filter)) {
				if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::MenuItem(node_type.name)) {
					addNode(node_type.type, pos, true);
					filter[0] = '\0';
					ImGui::CloseCurrentPopup();
					break;
				}
			}
		}
	}
	else {
		nodeGroupUI(*this, Span(NODE_TYPES), pos);
	}
}

void ShaderEditor::pushUndo(u32 tag) {
	SimpleUndoRedo::pushUndo(tag);
	m_source = m_resource->generate();
}

void ShaderEditor::serialize(OutputMemoryStream& blob) {
	m_resource->serialize(blob);
}

void ShaderEditor::deserialize(InputMemoryStream& blob) {
	LUMIX_DELETE(m_allocator, m_resource);
	m_resource = LUMIX_NEW(m_allocator, ShaderEditorResource)(m_app);
	m_resource->deserialize(blob);
}

void ShaderEditorResource::destroyNode(Node* node) {
	for (i32 i = m_links.size() - 1; i >= 0; --i) {
		if (m_links[i].getFromNode() == node->m_id || m_links[i].getToNode() == node->m_id) {
			m_links.swapAndPop(i);
		}
	}

	LUMIX_DELETE(m_allocator, node);
	m_nodes.eraseItem(node);
}

ShaderEditorResource::ShaderEditorResource(StudioApp& app)
	: m_app(app)
	, m_allocator(app.getAllocator())
	, m_links(m_allocator)
	, m_nodes(m_allocator)
{}

ShaderEditorResource::~ShaderEditorResource() {
	for (auto* node : m_nodes) {
		LUMIX_DELETE(m_allocator, node);
	}
}

void ShaderEditor::newGraph(bool is_particle_shader) {
	clear();

	m_path = "";
	
	PBRNode* output = LUMIX_NEW(m_allocator, PBRNode)(*m_resource);
	output->m_type = is_particle_shader ? PBRNode::Type::PARTICLES : PBRNode::Type::SURFACE;
	m_resource->m_nodes.push(output);
	output->m_pos.x = 50;
	output->m_pos.y = 50;
	output->m_id = ++m_resource->m_last_node_id;

	clearUndoStack();
	pushUndo(NO_MERGE_UNDO);
}

void ShaderEditor::save() {
	if (m_path.isEmpty()) m_show_save_as = true;
	else saveAs(m_path.c_str());
}

void ShaderEditor::onGUIMenu() {
	if(ImGui::BeginMenuBar()) {
		if(ImGui::BeginMenu("File")) {
			if (ImGui::BeginMenu("New")) {
				if (ImGui::MenuItem("Particles")) newGraph(true);
				if (ImGui::MenuItem("Surface")) newGraph(false);
				ImGui::EndMenu();
			}
			menuItem(m_generate_action, !m_path.isEmpty());
			ImGui::MenuItem("View source", nullptr, &m_source_open);
			if (ImGui::MenuItem("Open")) m_show_open = true;
			menuItem(m_save_action, true);
			if (ImGui::MenuItem("Save as")) m_show_save_as = true;
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

		ImGui::EndMenuBar();
	}
}

void ShaderEditorResource::deleteUnreachable() {
	markReachableNodes();
	colorLinks();
	for (i32 i = m_nodes.size() - 1; i >= 0; --i) {
		Node* node = m_nodes[i];
		if (!node->m_reachable) {
			for (i32 j = m_links.size() - 1; j >= 0; --j) {
				if (m_links[j].getFromNode() == node->m_id || m_links[j].getToNode() == node->m_id) {
					m_links.erase(j);
				}
			}

			LUMIX_DELETE(m_allocator, node);
			m_nodes.swapAndPop(i);
		}
	}
}

void ShaderEditor::deleteUnreachable() {
	m_resource->deleteUnreachable();
	pushUndo(NO_MERGE_UNDO);
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

	ImGui::SetNextWindowSize(ImVec2(300, 300), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(title, &m_is_open, ImGuiWindowFlags_MenuBar))
	{
		m_is_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		onGUIMenu();
		
		FileSelector& fs = m_app.getFileSelector();
		if (fs.gui("Open", &m_show_open, "sed", false)) load(fs.getPath());
		if (fs.gui("Save As", &m_show_save_as, "sed", true)) saveAs(fs.getPath());

		ImGui::BeginChild("canvas");
		nodeEditorGUI(m_resource->m_nodes, m_resource->m_links);
		ImGui::EndChild();
	}
	ImGui::End();
}


} // namespace Lumix