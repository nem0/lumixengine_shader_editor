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
	PIN
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


static u16 toNodeId(int id) {
	return u16(id);
}

static u16 toAttrIdx(int id) {
	return u16(u32(id) >> 16);
}

template <typename F>
static void	forEachInput(const ShaderEditorResource& resource, int node_id, const F& f) {
	for (const ShaderEditorResource::Link& link : resource.m_links) {
		if (toNodeId(link.to) == node_id) {
			const int iter = resource.m_nodes.find([&](const ShaderEditorResource::Node* node) { return node->m_id == toNodeId(link.from); }); 
			ShaderEditorResource::Node* from = resource.m_nodes[iter];
			const u16 from_attr = toAttrIdx(link.from);
			const u16 to_attr = toAttrIdx(link.to);
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
		if (toNodeId(link.from) == node_id) {
			const u16 from_attr = toAttrIdx(link.from);
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
	, m_id(0xffFF)
{
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

bool ShaderEditorResource::Node::onNodeGUI() {
	ImGuiEx::BeginNode(m_id, m_pos, &m_selected);
	m_input_count = 0;
	m_output_count = 0;
	bool res = onGUI();
	ImGuiEx::EndNode();

	ASSERT((m_input_count > 0) == hasInputPins());
	ASSERT((m_output_count > 0) == hasOutputPins());

	return res;
}

struct MixNode : public ShaderEditorResource::Node {
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

struct CodeNode : public ShaderEditorResource::Node {
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
				if (toNodeId(link.to) == m_id) {
					const u16 to_attr = toAttrIdx(link.to);
					if (to_attr == deleted_idx) to_del = &link;
					else if (to_attr > deleted_idx) {
						link.to = m_id | (u32(to_attr - 1) << 16);
					}
				}
			}
		}
		else {
			for (ShaderEditorResource::Link& link : m_resource.m_links) {
				if (toNodeId(link.from) == m_id) {
					const u16 from_attr = toAttrIdx(link.from);
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
struct OperatorNode : public ShaderEditorResource::Node {
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
		}
		ASSERT(false);
		return "Error";
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

struct OneMinusNode : public ShaderEditorResource::Node {
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

struct SwizzleNode : public ShaderEditorResource::Node {
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

struct FresnelNode : public ShaderEditorResource::Node {
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
struct FunctionCallNode : public ShaderEditorResource::Node
{
	explicit FunctionCallNode(ShaderEditorResource& resource)
		: Node(Type, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override { 
		if (Type == NodeType::LENGTH) return ShaderEditorResource::ValueType::FLOAT;
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

template <NodeType Type>
struct BinaryFunctionCallNode : public ShaderEditorResource::Node
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
		}
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) return input0.node->getOutputType(input0.output_idx);
		return ShaderEditorResource::ValueType::FLOAT;
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


template <NodeType Type>
struct VaryingNode : public ShaderEditorResource::Node {
	explicit VaryingNode(ShaderEditorResource& resource)
		: Node(Type, resource)
	{}

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream&) override {}
	void deserialize(InputMemoryStream&) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override { 
		switch(Type) {
			case NodeType::POSITION: return ShaderEditorResource::ValueType::VEC3;
			case NodeType::NORMAL: return ShaderEditorResource::ValueType::VEC3;
			case NodeType::UV0: return ShaderEditorResource::ValueType::VEC2;
			default: ASSERT(false); return ShaderEditorResource::ValueType::VEC3;
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
		outputSlot();
		switch(Type) {
			case NodeType::POSITION: ImGui::Text("Position"); break;
			case NodeType::NORMAL: ImGui::Text("Normal"); break;
			case NodeType::UV0: ImGui::Text("UV0"); break;
			default: ASSERT(false); break;
		}
		return false;
	}
};

template <ShaderEditorResource::ValueType TYPE>
struct ConstNode : public ShaderEditorResource::Node
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
		}
		return false;
	}

	bool hasOutputPins() const override { return true; }

	ShaderEditorResource::ValueType m_type;
	float m_value[4];
	int m_int_value;
};


struct SampleNode : public ShaderEditorResource::Node
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

struct AppendNode : public ShaderEditorResource::Node {
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

struct StaticSwitchNode : public ShaderEditorResource::Node {
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
struct ParameterNode : public ShaderEditorResource::Node {
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

struct PinNode : public ShaderEditorResource::Node {
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

struct PBRNode : public ShaderEditorResource::Node
{
	explicit PBRNode(ShaderEditorResource& resource)
		: Node(NodeType::PBR, resource)
	{}

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return false; }

	static void generate(OutputMemoryStream& blob, Node* node) {
		if (!node) return;
		forEachInput(node->m_resource, node->m_id, [&](ShaderEditorResource::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
			generate(blob, from);
		});
		node->generateOnce(blob);
	}

	void generateVS(OutputMemoryStream& blob) const {
		const Input input = getInput(m_resource, m_id, 0);
		generate(blob, input.node);
		if (!input) return;
		input.node->generateOnce(blob);
		blob << "v_wpos = ";
		input.printReference(blob);
		blob << ";";
	}

	void generate(OutputMemoryStream& blob) override {
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
			Input input = getInput(m_resource, m_id, i);
			if (input) {
				input.node->generateOnce(blob);
				blob << "\tdata." << field.name << " = ";
				if (i < 2) blob << "vec3(";
				input.printReference(blob);
				const ShaderEditorResource::ValueType type = input.node->getOutputType(input.output_idx);
				if (i == 0) {
					switch(type) {
						case ShaderEditorResource::ValueType::VEC4: blob << ".rgb"; break;
						case ShaderEditorResource::ValueType::VEC3: break;
						case ShaderEditorResource::ValueType::VEC2: blob << ".rgr"; break;
						case ShaderEditorResource::ValueType::FLOAT: break;
					}
				}
				else if (type != ShaderEditorResource::ValueType::VEC3 && i < 2) blob << ".rgb";
				else if (type != ShaderEditorResource::ValueType::FLOAT && i >= 2) blob << ".x";
				if (i < 2) blob << ")";
				blob << ";\n";
			}
			else {
				blob << "\tdata." << field.name << " = " << field.default_value << ";\n";
			}
		}
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle("PBR");
		
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

		return false;
	}
};

struct IfNode : public ShaderEditorResource::Node
{
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
		}
		return ShaderEditorResource::ValueType::FLOAT;
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
	, SimpleUndoRedo(app.getAllocator())
{
	newGraph();
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
	saveUndo(NO_MERGE_UNDO);
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
	clear();
}

void ShaderEditorResource::deleteSelectedNodes() {
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
}

void ShaderEditorResource::markReachable(Node* node) const {
	node->m_reachable = true;

	forEachInput(*this, node->m_id, [&](ShaderEditorResource::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
		markReachable(from);
	});
}

void ShaderEditorResource::colorLinks(ImU32 color, u32 link_idx) {
	m_links[link_idx].color = color;
	const u32 from_node_id = toNodeId(m_links[link_idx].from);
	for (u32 i = 0, c = m_links.size(); i < c; ++i) {
		if (toNodeId(m_links[i].to) == from_node_id) colorLinks(color, i);
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

	for (Node* n : m_nodes) {
		n->m_generated = false;
	}

	m_nodes[0]->generateOnce(blob);

	blob << "]]\n})\n";

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
	if(!file.open(path)) {
		logError("Could not save shader ", path);
		return;
	}

	OutputMemoryStream blob(m_allocator);
	m_resource->serialize(blob);

	bool success = file.write(blob.data(), blob.size());
	file.close();
	if (!success) {
		logError("Could not save shader ", path);
	}

	pushRecent(path);
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
}

void ShaderEditor::clear() {
	LUMIX_DELETE(m_allocator, m_resource);
	m_resource = LUMIX_NEW(m_allocator, ShaderEditorResource)(m_allocator);
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
		case NodeType::IF:							return LUMIX_NEW(m_allocator, IfNode)(*this);
		case NodeType::STATIC_SWITCH:				return LUMIX_NEW(m_allocator, StaticSwitchNode)(*this, m_allocator);
		case NodeType::ONEMINUS:					return LUMIX_NEW(m_allocator, OneMinusNode)(*this);
		case NodeType::CODE:						return LUMIX_NEW(m_allocator, CodeNode)(*this, m_allocator);
		case NodeType::APPEND:						return LUMIX_NEW(m_allocator, AppendNode)(*this);
		case NodeType::FRESNEL:						return LUMIX_NEW(m_allocator, FresnelNode)(*this);
		case NodeType::POSITION:					return LUMIX_NEW(m_allocator, VaryingNode<NodeType::POSITION>)(*this);
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
		case NodeType::POW:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::POW>)(*this);
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
	m_resource->deserialize(blob);

	clearUndoStack();
	saveUndo(NO_MERGE_UNDO);
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
	if (save_undo) saveUndo(NO_MERGE_UNDO);
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

void ShaderEditor::onGUICanvas()
{
	ImGui::BeginChild("canvas");

	m_canvas.begin();

	static ImVec2 offset = ImVec2(0, 0);
	ImGuiEx::BeginNodeEditor("shader_editor", &offset);
	const ImVec2 origin = ImGui::GetCursorScreenPos();

	ImGuiID moved = 0;
	u32 moved_count = 0;
	for (Node*& node : m_resource->m_nodes) {
		const bool reachable = node->m_reachable;
		if (!reachable) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
		
		const ImVec2 old_pos = node->m_pos;
		if (node->onNodeGUI()) {
			saveUndo(node->m_id);
		}
		if (old_pos.x != node->m_pos.x || old_pos.y != node->m_pos.y) {
			moved = node->m_id;
			++moved_count;
		}
		if (!reachable) ImGui::PopStyleVar();
	}

	if (moved_count > 0) {
		if (moved_count > 1) saveUndo(0xffFE);
		else saveUndo(moved);
	}

	bool open_context = false;

	const ImVec2 mp = ImGui::GetMousePos() - origin - offset;
	i32 hovered_link = -1;
	for (i32 i = 0, c = m_resource->m_links.size(); i < c; ++i) {
		Link& link = m_resource->m_links[i];
		ImGuiEx::NodeLinkEx(link.from | OUTPUT_FLAG, link.to, link.color, ImGui::GetColorU32(ImGuiCol_TabActive));
		if (ImGuiEx::IsLinkHovered()) {
			if (ImGui::IsMouseClicked(0) && ImGui::GetIO().KeyCtrl) {
				if (ImGuiEx::IsLinkStartHovered()) {
					ImGuiEx::StartNewLink(link.to, true);
				}
				else {
					ImGuiEx::StartNewLink(link.from | OUTPUT_FLAG, false);
				}
				m_resource->m_links.erase(i);
				--c;
			}
			if (ImGui::IsMouseDoubleClicked(0)) {
				Node* n = addNode(NodeType::PIN, mp, false);
				Link new_link;
				new_link.color = link.color;
				new_link.from = n->m_id | OUTPUT_FLAG; 
				new_link.to = link.to;
				link.to = n->m_id;
				m_resource->m_links.push(new_link);
				saveUndo(NO_MERGE_UNDO);
			}
			else {
				hovered_link = i;
			}
		}
	}

	{
		ImGuiID start_attr, end_attr;
		if (ImGuiEx::GetHalfLink(&start_attr)) {
			open_context = true;
			m_context_pos = ImGui::GetMousePos() - offset;
			m_half_link_start = start_attr;
		}

		if (ImGuiEx::GetNewLink(&start_attr, &end_attr)) {
			ASSERT(start_attr & OUTPUT_FLAG);
			m_resource->m_links.eraseItems([&](const Link& link) { return link.to == end_attr; });
			m_resource->m_links.push({u32(start_attr) & ~OUTPUT_FLAG, u32(end_attr)});
			
			saveUndo(NO_MERGE_UNDO);
			m_resource->colorLinks();
		}
	}

	ImGuiEx::EndNodeEditor();
 
	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
		if (ImGui::GetIO().KeyAlt && hovered_link != -1) {
			m_resource->m_links.erase(hovered_link);
			saveUndo(NO_MERGE_UNDO);
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
					addNode(t.type, mp, true);
					break;
				}
			}
		}
	}

	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
		open_context = true;
		m_context_pos = ImGui::GetMousePos() - offset;
		m_half_link_start = 0;
	}

	if (open_context) ImGui::OpenPopup("context_menu");

	if(ImGui::BeginPopup("context_menu")) {
		static char filter[64] = "";
		if (ImGui::MenuItem("Reset zoom")) m_canvas.m_scale = ImVec2(1, 1);
		ImGui::SetNextItemWidth(150);
		if (open_context) ImGui::SetKeyboardFocusHere();
		ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
		if (filter[0]) {
			for (const auto& node_type : NODE_TYPES) {
				if (stristr(node_type.name, filter)) {
					if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::MenuItem(node_type.name)) {
						addNode(node_type.type, m_context_pos, true);
						filter[0] = '\0';
						ImGui::CloseCurrentPopup();
						break;
					}
				}
			}
		}
		else {
			nodeGroupUI(*this, Span(NODE_TYPES), m_context_pos);
		}

		ImGui::EndPopup();
	}		

	m_is_any_item_active = ImGui::IsAnyItemActive();

	m_canvas.end();

	ImGui::EndChild();
}

void ShaderEditor::saveUndo(u32 tag) {
	pushUndo(tag);
	m_source = m_resource->generate();
}

void ShaderEditor::serialize(OutputMemoryStream& blob) {
	m_resource->serialize(blob);
}

void ShaderEditor::deserialize(InputMemoryStream& blob) {
	LUMIX_DELETE(m_allocator, m_resource);
	m_resource = LUMIX_NEW(m_allocator, ShaderEditorResource)(m_allocator);
	m_resource->deserialize(blob);
}

void ShaderEditorResource::destroyNode(Node* node) {
	for (i32 i = m_links.size() - 1; i >= 0; --i) {
		if (toNodeId(m_links[i].from) == node->m_id || toNodeId(m_links[i].to) == node->m_id) {
			m_links.swapAndPop(i);
		}
	}

	LUMIX_DELETE(m_allocator, node);
	m_nodes.eraseItem(node);
}

ShaderEditorResource::ShaderEditorResource(IAllocator& allocator)
	: m_links(allocator)
	, m_nodes(allocator)
	, m_allocator(allocator)
{}

ShaderEditorResource::~ShaderEditorResource() {
	for (auto* node : m_nodes) {
		LUMIX_DELETE(m_allocator, node);
	}
}

void ShaderEditor::newGraph() {
	clear();

	m_path = "";
	
	m_resource->m_nodes.push(LUMIX_NEW(m_allocator, PBRNode)(*m_resource));
	m_resource->m_nodes.back()->m_pos.x = 50;
	m_resource->m_nodes.back()->m_pos.y = 50;
	m_resource->m_nodes.back()->m_id = ++m_resource->m_last_node_id;

	clearUndoStack();
	saveUndo(NO_MERGE_UNDO);
}

void ShaderEditor::save() {
	if (m_path.isEmpty()) {
		if(getSavePath() && !m_path.isEmpty()) saveAs(m_path.c_str());
	}
	else {
		saveAs(m_path.c_str());
	}
}

void ShaderEditor::onGUIMenu()
{
	if(ImGui::BeginMenuBar()) {
		if(ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New")) newGraph();
			menuItem(m_generate_action, !m_path.isEmpty());
			ImGui::MenuItem("View source", nullptr, &m_source_open);
			if (ImGui::MenuItem("Open")) load();
			menuItem(m_save_action, true);
			if (ImGui::MenuItem("Save as")) {
				if(getSavePath() && !m_path.isEmpty()) saveAs(m_path.c_str());
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
				if (toNodeId(m_links[j].from) == node->m_id || toNodeId(m_links[j].to) == node->m_id) {
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
	saveUndo(NO_MERGE_UNDO);
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