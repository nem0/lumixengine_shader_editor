#define LUMIX_NO_CUSTOM_CRT
#include "shader_editor.h"
#include "editor/utils.h"
#include "engine/crt.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/os.h"
#include "engine/path.h"
#include "engine/stream.h"
#include "engine/string.h"
#include "renderer/model.h"
#include "imgui/IconsFontAwesome5.h"
#include <math.h>


namespace Lumix
{

struct ShaderEditor::Undo {
	Undo(IAllocator& allocator) : blob(allocator) {}

	OutputMemoryStream blob;
	u16 id;
};

static constexpr u32 OUTPUT_FLAG = 1 << 31;
static constexpr char* PROLOGUE_VS = R"#(
include "pipelines/common.glsl"

define "ALPHA_CUTOUT"
define "VEGETATION"
uniform("Stiffness", "float", 10)

common [[
	#ifdef SKINNED
		layout(std140, binding = 4) uniform ModelState {
			float layer;
			float fur_scale;
			float fur_gravity;
			float padding;
			mat4 matrix;
			mat4 bones[256];
		} Model;
	#endif

	#if defined _HAS_ATTR6 && !defined GRASS
		#define HAS_LOD
	#endif
]]

vertex_shader [[
	layout(location = 0) in vec3 a_position;
	#ifdef _HAS_ATTR1
		layout(location = 1) in vec2 a_uv;
	#else
		const vec2 a_uv = vec2(0);
	#endif
	layout(location = 2) in vec3 a_normal;
	#ifdef _HAS_ATTR3
		layout(location = 3) in vec3 a_tangent;
	#else 
		const vec3 a_tangent = vec3(0, 1, 0);
	#endif
	#if defined SKINNED
		layout(location = 4) in ivec4 a_indices;
		layout(location = 5) in vec4 a_weights;
	#elif defined INSTANCED || defined GRASS
		layout(location = 4) in vec4 i_rot_quat;
		layout(location = 5) in vec4 i_pos_scale;
		#ifdef HAS_LOD
			layout(location = 6) in float i_lod;
			layout(location = 4) out float v_lod;
		#endif
		#ifdef _HAS_ATTR7
			layout(location = 7) in vec4 a_color;
			layout(location = 6) out vec4 v_color;
		#endif
		#ifdef _HAS_ATTR8
			layout(location = 8) in float a_ao;
			layout(location = 7) out float v_ao;
		#endif
	#else
		layout(std140, binding = 4) uniform ModelState {
			mat4 matrix;
		} Model;
	#endif
	
	layout(location = 0) out vec2 v_uv;
	layout(location = 1) out vec3 v_normal;
	layout(location = 2) out vec3 v_tangent;
	layout(location = 3) out vec4 v_wpos;
	#ifdef GRASS
		layout(location = 5) out float v_darken;
	#endif
	
	void main() {
		#ifdef HAS_LOD
			v_lod = 0;
		#endif
		v_uv = a_uv;
		#if defined INSTANCED || defined GRASS
			v_normal = rotateByQuat(i_rot_quat, a_normal);
			v_tangent = rotateByQuat(i_rot_quat, a_tangent);
			vec3 p = a_position * i_pos_scale.w;
			#ifdef HAS_LOD
				v_lod = i_lod;
			#endif
			#if defined GRASS && defined VEGETATION
				p = vegetationAnim(i_pos_scale.xyz, p, 1 / (1.0 + u_stiffness));
				v_darken = a_position.y > 0.1 ? 1 : 0.0;
			#elif defined VEGETATION
				#ifdef DEPTH
					p = vegetationAnim(i_pos_scale.xyz - Pass.shadow_to_camera.xyz, p, 0.001);
				#else
					p = vegetationAnim(i_pos_scale.xyz, p, 0.001);
				#endif
			#endif
			v_wpos = vec4(i_pos_scale.xyz + rotateByQuat(i_rot_quat, p), 1);
			#ifdef _HAS_ATTR7
				v_color = a_color;
			#endif
			#ifdef _HAS_ATTR8
				v_ao = a_ao;
			#endif
		#elif defined SKINNED
			mat4 model_mtx = Model.matrix * (a_weights.x * Model.bones[a_indices.x] + 
			a_weights.y * Model.bones[a_indices.y] +
			a_weights.z * Model.bones[a_indices.z] +
			a_weights.w * Model.bones[a_indices.w]);
			v_normal = mat3(model_mtx) * a_normal;
			v_tangent = mat3(model_mtx) * a_tangent;
			#ifdef FUR
				v_wpos = model_mtx * vec4(a_position + (a_normal + vec3(0, -Model.fur_gravity * Model.layer, 0)) * Model.layer * Model.fur_scale,  1);
			#else
				v_wpos = model_mtx * vec4(a_position,  1);
			#endif
		#else 
			mat4 model_mtx = Model.matrix;
			v_normal = mat3(model_mtx) * a_normal;
			v_tangent = mat3(model_mtx) * a_tangent;

			vec3 p = a_position;
			#ifdef VEGETATION
				p = vegetationAnim(Model.matrix[3].xyz, p, 0.001);
			#endif

			v_wpos = model_mtx * vec4(p,  1);
		#endif
)#";

static constexpr char* PROLOGUE_FS = R"#(
		gl_Position = Pass.view_projection * v_wpos;		
	}
]]

fragment_shader [[
)#";

static constexpr char* PROLOGUE_FS2 = R"#(
	layout (binding=5) uniform sampler2D u_shadowmap;
	#if !defined DEPTH && !defined DEFERRED && !defined GRASS
		layout (binding=6) uniform sampler2D u_shadow_atlas;
		layout (binding=7) uniform samplerCubeArray u_reflection_probes;
	#endif
	
	layout(location = 0) in vec2 v_uv;
	layout(location = 1) in vec3 v_normal;
	layout(location = 2) in vec3 v_tangent;
	layout(location = 3) in vec4 v_wpos;
	#ifdef HAS_LOD
		layout(location = 4) in float v_lod;
	#endif
	#ifdef GRASS
		layout(location = 5) in float v_darken;
	#endif
	#ifdef _HAS_ATTR7
		layout(location = 6) in vec4 v_color;
	#endif
	#ifdef _HAS_ATTR8
		layout(location = 7) in float v_ao;
	#endif

	#if defined DEFERRED || defined GRASS
		layout(location = 0) out vec4 o_gbuffer0;
		layout(location = 1) out vec4 o_gbuffer1;
		layout(location = 2) out vec4 o_gbuffer2;
	#elif !defined DEPTH
		layout(location = 0) out vec4 o_color;
	#endif

	Surface getSurface()
	{

		Surface data;
)#";

static constexpr char* EPILOGUE = R"#(
		data.V = normalize(-data.wpos);
		return data;
	}
	
	#ifdef DEPTH
		void main()
		{
			#ifdef ALPHA_CUTOUT
				vec4 c = texture(u_albedomap, v_uv);
				if(c.a < 0.5) discard;
			#endif
		}
	#elif defined DEFERRED || defined GRASS
		void main()
		{
			Surface data = getSurface();
			#if defined ALPHA_CUTOUT && defined LUMIX_DX_SHADER
				if(data.alpha < 0.5) discard;
			#endif
			packSurface(data, o_gbuffer0, o_gbuffer1, o_gbuffer2);
		}
	#else 
		void main()
		{
			Surface data = getSurface();
			
			float linear_depth = dot(data.wpos.xyz, Pass.view_dir.xyz);
			Cluster cluster = getClusterLinearDepth(linear_depth);
			o_color.rgb = computeLighting(cluster, data, Global.light_dir.xyz, Global.light_color.rgb * Global.light_intensity, u_shadowmap, u_shadow_atlas, u_reflection_probes);

			#if defined ALPHA_CUTOUT
				if(data.alpha < 0.5) discard;
			#endif

			float ndotv = abs(dot(data.N , data.V)) + 1e-5f;
			o_color.a = mix(data.alpha, 1, pow(saturate(1 - ndotv), 5));
		}
	#endif
]]
)#";

enum class NodeType {
	PBR,

	FLOAT_CONSTANT,
	VEC4_CONSTANT,
	SAMPLE,
	MIX,
	VEC4,
	SWIZZLE,
	MULTIPLY,
	OPERATOR,
	BUILTIN_UNIFORM,
	VERTEX_ID,
	PASS,
	POSITION,
	NORMAL,
	FUNCTION_CALL,
	BINARY_FUNCTION_CALL,
	IF
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

static const struct {
	const char* name;
	NodeType type;
} NODE_TYPES[] = {
	{"Position", NodeType::POSITION},
	{"Normal", NodeType::NORMAL},
	{"Mix", NodeType::MIX},
	{"Sample", NodeType::SAMPLE},
	{"Constant", NodeType::FLOAT_CONSTANT},
	{"Vec4 constant", NodeType::VEC4_CONSTANT},
	{"Vec4", NodeType::VEC4},
	{"Swizzle", NodeType::SWIZZLE},
	{"Multiply", NodeType::MULTIPLY},
	{"Operator", NodeType::OPERATOR},
	{"Builtin uniforms", NodeType::BUILTIN_UNIFORM},
	{"Vertex ID", NodeType::VERTEX_ID},
	{"Pass", NodeType::PASS},
	{"Function", NodeType::FUNCTION_CALL},
	{"Binary function", NodeType::BINARY_FUNCTION_CALL},
	{"If", NodeType::IF}};


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
	{ "View & Projection",	"Pass.pass_view_projection",	ShaderEditor::ValueType::MATRIX4 },
	{ "Time",				"Global.time",					ShaderEditor::ValueType::FLOAT },
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

template <typename F>
static void	forEachInput(const ShaderEditor& editor, int node_id, const F& f) {
	for (const ShaderEditor::Link& link : editor.m_links) {
		if (toNodeId(link.to) == node_id) {
			const int iter = editor.m_nodes.find([&](const ShaderEditor::Node* node) { return node->m_id == toNodeId(link.from); }); 
			const ShaderEditor::Node* from = editor.m_nodes[iter];
			const u16 from_attr = toAttrIdx(link.from);
			const u16 to_attr = toAttrIdx(link.to);
			f(from, from_attr, to_attr);
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
	forEachInput(editor, node_id, [&](const ShaderEditor::Node* from, u16 from_attr, u16 to_attr){
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


ShaderEditor::Node::Node(int type, ShaderEditor& editor)
	: m_type(type)
	, m_editor(editor)
	, m_id(0xffFF)
{
}

struct MultiplyNode : public ShaderEditor::Node {

	explicit MultiplyNode(ShaderEditor& editor)
		: Node((int)NodeType::MULTIPLY, editor)
	{}

	void save(OutputMemoryStream& blob) override { blob.write(b_val); }
	void load(InputMemoryStream& blob) override { blob.read(b_val); }

	ShaderEditor::ValueType getOutputType(int) const override {
		// TODO float * vec4 and others
		return getInputType(0);
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
		blob << " * ";
		if (input1) {
			input1.printReference(blob);
		}
		else {
			blob << b_val;
		}
		blob << ")";
	}

	bool onGUI() override {
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

struct OperatorNode : public ShaderEditor::Node {
	enum Operation : i32 {
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

	void printReference(OutputMemoryStream& blob, int attr_idx) const override
	{
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		if (!input0 || !input1) return; 
		
		blob << "(";
		input0.printReference(blob);
		blob << " " << toString(m_operation) << " ";
		input1.printReference(blob);
		blob << ")";
	}

	bool onGUI() override
	{
		int o = m_operation;
		auto getter = [](void*, int idx, const char** out){
			*out =  toString((Operation)idx);
			return true;
		};

		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true); ImGui::Text("A");
		ImGuiEx::Pin(m_id | (1 << 16), true); ImGui::Text("B");
		ImGui::EndGroup();

		ImGui::SameLine();
		bool res = false;
		ImGuiEx::Pin(u32(m_id) | OUTPUT_FLAG, false);
		if (ImGui::Combo("Op", &o, getter, nullptr, (int)Operation::COUNT)) {
			m_operation = (Operation)o;
			res = true;
		}

		return res;
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

struct Vec4Node : public ShaderEditor::Node
{
	explicit Vec4Node(ShaderEditor& editor)
		: Node((int)NodeType::VEC4, editor)
	{}

	void save(OutputMemoryStream& blob) override { blob.write(value); }
	void load(InputMemoryStream& blob) override { blob.read(value); }
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC4; }

	void generate(OutputMemoryStream& blob) const override {
		blob << "\t\tvec4 v" << m_id << ";\n";

		Input input = getInput(m_editor, m_id, 0);
		blob << "\t\tv" << m_id << ".x = ";
		if (input) {
			input.printReference(blob);
		}
		else {
			blob << value.x;
		}
		blob << ";\n";

		input = getInput(m_editor, m_id, 1);
		blob << "\t\tv" << m_id << ".y = ";
		if (input) {
			input.printReference(blob);
		}
		else {
			blob << value.y;
		}
		blob << ";\n";

		input = getInput(m_editor, m_id, 2);
		blob << "\t\tv" << m_id << ".z = ";
		if (input) {
			input.printReference(blob);
		}
		else {
			blob << value.z;
		}
		blob << ";\n";

		input = getInput(m_editor, m_id, 3);
		blob << "\t\tv" << m_id << ".w = ";
		if (input) {
			input.printReference(blob);
		}
		else {
			blob << value.w;
		}
		blob << ";\n";
	}


	bool onGUI() override
	{
		ImGui::BeginGroup();
		for (i32 i = 0; i < 4; ++i) {
			ImGuiEx::Pin(m_id | (i << 16), true);
			const char* labels[] = { "x", "y", "z", "w" };
			if (isInputConnected(m_editor, m_id, i)) {
				ImGui::TextUnformatted(labels[i]);
			}
			else {
				ImGui::DragFloat(labels[i], &value.x + i);	
			}
		}
		ImGui::EndGroup();

		ImGui::SameLine();

		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::TextUnformatted("xyzw");
		return false;
	}

	Vec4 value = Vec4(0);
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

	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);

		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = " << FUNCTIONS[m_function] << "(";
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
		auto getter = [](void* data, int idx, const char** out_text) -> bool {
			*out_text = FUNCTIONS[idx];
			return true;
		};
		ImGui::SetNextItemWidth(120);
		bool res = ImGui::Combo("##fn", &m_function, getter, nullptr, lengthOf(FUNCTIONS));
		
		ImGui::SameLine();

		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		return res;
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


	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);

		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = " << BINARY_FUNCTIONS[m_function].name << "(";
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
		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true); ImGui::Text("argument 1");
		ImGuiEx::Pin(m_id | (1 << 16), true); ImGui::Text("argument 2");
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		auto getter = [](void* data, int idx, const char** out_text) -> bool {
			*out_text = BINARY_FUNCTIONS[idx].name;
			return true;
		};
		bool res = ImGui::Combo("Function", &m_function, getter, nullptr, lengthOf(BINARY_FUNCTIONS));
		return res;
	}

	int m_function;
};

struct NormalNode : public ShaderEditor::Node {
	explicit NormalNode(ShaderEditor& editor)
		: Node((int)NodeType::NORMAL, editor)
	{}

	void save(OutputMemoryStream&) override {}
	void load(InputMemoryStream&) override {}
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC3; }

	void printReference(OutputMemoryStream& blob, int output_idx) const {
		blob << "v_normal";
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::Text("Normal");
		return false;
	}
};

struct PositionNode : public ShaderEditor::Node {
	explicit PositionNode(ShaderEditor& editor)
		: Node((int)NodeType::POSITION, editor)
	{}

	void save(OutputMemoryStream&) override {}
	void load(InputMemoryStream&) override {}
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC4; }

	void printReference(OutputMemoryStream& blob, int output_idx) const {
		blob << "v_wpos";
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::Text("Position");
		return false;
	}
};

template <ShaderEditor::ValueType TYPE>
struct ConstNode : public ShaderEditor::Node
{
	explicit ConstNode(ShaderEditor& editor)
		: Node((int)toNodeType(TYPE), editor)
	{
		m_type = TYPE;
		m_value[0] = m_value[1] = m_value[2] = m_value[3] = 0;
		m_int_value = 0;
	}

	static NodeType toNodeType(ShaderEditor::ValueType t) {
		switch(t) {
			case ShaderEditor::ValueType::FLOAT: return NodeType::FLOAT_CONSTANT;
			case ShaderEditor::ValueType::VEC4: return NodeType::VEC4_CONSTANT;
			default: ASSERT(false); return NodeType::FLOAT_CONSTANT;
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

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
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
	
	bool onGUI() override {
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		auto getter = [](void*, int idx, const char** out){
			*out = toString((ShaderEditor::ValueType)idx);
			return true;
		};
		bool res = false;

		switch(m_type) {
			case ShaderEditor::ValueType::VEC4:
				ImGui::Checkbox("Color", &m_is_color);
				if (m_is_color) {
					res = ImGui::ColorPicker4("", m_value) || res; 
				}
				else {
					res = ImGui::InputFloat4("", m_value) || res;
				}
				break;
			case ShaderEditor::ValueType::VEC3:
				res = ImGui::Checkbox("Color", &m_is_color) || res;
				if (m_is_color) {
					res = ImGui::ColorPicker3("", m_value) || res; 
				}
				else {
					res = ImGui::InputFloat3("", m_value) || res;
				}
				break;
			case ShaderEditor::ValueType::VEC2:
				res =  ImGui::InputFloat2("", m_value) || res;
				break;
			case ShaderEditor::ValueType::FLOAT:
				ImGui::SetNextItemWidth(60);
				res = ImGui::InputFloat("", m_value) || res;
				break;
			case ShaderEditor::ValueType::INT:
				ImGui::SetNextItemWidth(60);
				res = ImGui::InputInt("", &m_int_value) || res;
				break;
			default: ASSERT(false); break;
		}
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
		: Node((int)NodeType::SAMPLE, editor)
	{
		m_texture = 0;
	}

	void save(OutputMemoryStream& blob) override { blob.write(m_texture); }
	void load(InputMemoryStream& blob) override { blob.read(m_texture); }
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC4; }

	void generate(OutputMemoryStream& blob) const override {
		blob << "\t\tvec4 v" << m_id << " = ";
		const Input input0 = getInput(m_editor, m_id, 0);
		if (!input0) {
			blob << "vec4(1, 0, 1, 1);\n";
			return;
		}

		blob << "texture(" << m_editor.getTextureName(m_texture) << ", ";
		input0.printReference(blob);
		blob << ");\n";
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id, true);
		ImGui::Text("UV");

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		auto getter = [](void* data, int idx, const char** out) -> bool {
			*out = ((SampleNode*)data)->m_editor.getTextureName(idx);
			return true;
		};
		bool res = ImGui::Combo("Texture", &m_texture, getter, this, ShaderEditor::MAX_TEXTURES_COUNT);
		return res;
	}

	int m_texture;
};



struct PBRNode : public ShaderEditor::Node
{
	explicit PBRNode(ShaderEditor& editor)
		: Node((int)NodeType::PBR, editor)
	{}

	void save(OutputMemoryStream& blob) {}
	void load(InputMemoryStream& blob) {}

	static void generate(OutputMemoryStream& blob, const Node* node) {
		if (!node) return;
		forEachInput(node->m_editor, node->m_id, [&](const ShaderEditor::Node* from, u16 from_attr, u16 to_attr){
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
		const char* fields[] = {
			"wpos", "albedo", "alpha", "N", "roughness", "metallic", "emission", "ao", "translucency"
		};

		for (const char*& field : fields) {
			const int i = int(&field - fields);
			Input input = getInput(m_editor, m_id, i);
			if (input) {
				input.node->generate(blob);

				blob << "\t\tdata." << field << " = ";
				input.printReference(blob);
				const ShaderEditor::ValueType type = input.node->getOutputType(input.output_idx);
				if (type == ShaderEditor::ValueType::VEC4 && i <= 1) blob << ".xyz";
				blob << ";\n";
			}
		}

		const Input input = getInput(m_editor, m_id, 9);
		if (input) {
			input.node->generate(blob);
			
			blob << "if (";
			input.printReference(blob);
			blob << ") discard;";
		}
	}

	bool onGUI() override {
		ImGui::Text("PBR");
		
		ImGuiEx::Pin(m_id, true);
		ImGui::TextUnformatted("Vertex position");

		ImGuiEx::Pin(m_id | (1 << 16), true);
		ImGui::TextUnformatted("Albedo");

		ImGuiEx::Pin(m_id | (2 << 16), true);
		ImGui::TextUnformatted("Alpha");

		ImGuiEx::Pin(m_id | (3 << 16), true);
		ImGui::TextUnformatted("Normal");

		ImGuiEx::Pin(m_id | (4 << 16), true);
		ImGui::TextUnformatted("Roughness");

		ImGuiEx::Pin(m_id | (5 << 16), true);
		ImGui::TextUnformatted("Metallic");

		ImGuiEx::Pin(m_id | (6 << 16), true);
		ImGui::TextUnformatted("Emission");

		ImGuiEx::Pin(m_id | (7 << 16), true);
		ImGui::TextUnformatted("AO");

		ImGuiEx::Pin(m_id | (8 << 16), true);
		ImGui::TextUnformatted("Translucency");

		ImGuiEx::Pin(m_id | (9 << 16), true);
		ImGui::TextUnformatted("Discard");

		return false;
	}
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

	void printReference(OutputMemoryStream& blob,  int output_idx) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		const Input input2 = getInput(m_editor, m_id, 2);

		if (!input0 || !input1 || !input2) return;

		blob << "mix(";
		input0.printReference(blob);
		blob << ", ";
		input1.printReference(blob);
		blob << ", ";
		input2.printReference(blob);
		blob << ")";
	}

	bool onGUI() override {
		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true);
		ImGui::Text("Input 1");

		ImGuiEx::Pin(m_id | (1 << 16), true);
		ImGui::Text("Input 2");
		
		ImGuiEx::Pin(m_id | (2 << 16), true);
		ImGui::Text("Weight");
		ImGui::EndGroup();

		ImGui::SameLine();
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		return false;
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

	void generate(OutputMemoryStream& blob) const override {
		const char* defs[] = { "\t\t#ifdef ", "\t\t#ifndef " };
		for (int i = 0; i < 2; ++i) {
			const Input input = getInput(m_editor, m_id, 0);
			
			if (!input) continue;

			blob << defs[i] << m_pass << "\n";
			blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << " = ";
			input.printReference(blob);
			blob << ";\n";
			blob << "\t\t#endif // " << m_pass << "\n\n";
		}
	}

	bool onGUI() override {
		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true);
		ImGui::Text("if defined");

		ImGuiEx::Pin(m_id | (1 << 16), true);
		ImGui::Text("if not defined");
		ImGui::EndGroup();
		ImGui::SameLine();

		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::InputText("Pass", m_pass, sizeof(m_pass));

		return false;
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

	void generate(OutputMemoryStream& blob) const override {
		const Input input0 = getInput(m_editor, m_id, 0);
		const Input input1 = getInput(m_editor, m_id, 1);
		const Input input2 = getInput(m_editor, m_id, 2);
		if(!input0) return;
		
		blob << "\t\t" << toString(getOutputType(0)) << " v" << m_id << ";\n";
		if (input1) {
			blob << "\t\tif(";
			input0.printReference(blob);
			blob << ") {\n";
			blob << "\t\t\tv" << m_id << " = ";
			input1.printReference(blob);
			blob << ";\n";
			blob << "\t\t}\n";

			if(input2) {
				blob << "\t\telse {\n";
				blob << "\t\t\tv" << m_id << " = ";
				input2.printReference(blob);
				blob << ";\n";
				blob << "\t\t}\n";
			}
		}
		else if(input2) {
			blob << "\t\tif(!(";
			input0.printReference(blob);
			blob << ")) {\n";
			blob << "\t\t\tv" << m_id << " = ";
			input2.printReference(blob);
			blob << ";\n";
			blob << "\t\t}\n";
		}
	}

	bool onGUI() override {
		ImGui::BeginGroup();
		ImGuiEx::Pin(m_id, true);
		ImGui::Text("Condition");
		
		ImGuiEx::Pin(m_id | (1 << 16), true);
		ImGui::Text("If");

		ImGuiEx::Pin(m_id | (2 << 16), true);
		ImGui::Text("Else");
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
		: Node((int)NodeType::VERTEX_ID, editor)
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

struct BuiltinUniformNode : ShaderEditor::Node
{
	explicit BuiltinUniformNode(ShaderEditor& editor)
		: Node((int)NodeType::BUILTIN_UNIFORM, editor)
	{
		m_uniform = 0;
	}


	void save(OutputMemoryStream& blob) override { blob.write(m_uniform); }
	void load(InputMemoryStream& blob) override { blob.read(m_uniform); }


	void printReference(OutputMemoryStream& blob,  int output_idx) const override
	{
		blob << BUILTIN_UNIFORMS[m_uniform].name;
	}

	ShaderEditor::ValueType getOutputType(int) const override
	{
		return BUILTIN_UNIFORMS[m_uniform].type;
	}

	bool onGUI() override
	{
		auto getter = [](void* data, int index, const char** out_text) -> bool {
			*out_text = BUILTIN_UNIFORMS[index].gui_name;
			return true;
		};
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		ImGui::SetNextItemWidth(120);
		bool res = ImGui::Combo("##uniform", (int*)&m_uniform, getter, nullptr, lengthOf(BUILTIN_UNIFORMS));
		return res;
	}

	int m_uniform;
};

ShaderEditor::ShaderEditor(IAllocator& allocator)
	: m_allocator(allocator)
	, m_undo_stack(allocator)
	, m_source(allocator)
	, m_links(allocator)
	, m_nodes(allocator)
	, m_undo_stack_idx(-1)
	, m_is_focused(false)
	, m_is_open(false)
{
	newGraph();
}


ShaderEditor::~ShaderEditor()
{
	clear();
}


void ShaderEditor::generate(const char* sed_path, bool save_file)
{
	OutputMemoryStream blob(m_allocator);
	blob.reserve(32*1024);

	for (u32 i = 0; i < lengthOf(m_textures); ++i) {
		if (m_textures[i].empty()) continue;

		blob << "texture_slot {\n"
			 << "\tname = \"" << m_textures[i] << "\",\n"
			 << "\tdefault_texture = \"textures/common/white.tga\"\n"
			 << "}\n";
	}

	blob << PROLOGUE_VS;
	((PBRNode*)m_nodes[0])->generateVS(blob);
	blob << PROLOGUE_FS;

	u32 binding = 0;
	for (u32 i = 0; i < lengthOf(m_textures); ++i) {
		if (m_textures[i].empty()) continue;
		blob << "\tlayout (binding=" << binding << ") uniform sampler2D " << m_textures[i] << ";\n";
		++binding;
	}
	
	blob << PROLOGUE_FS2;

	m_nodes[0]->generate(blob);
	blob << EPILOGUE;

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
	memcpy(m_source.getData(), blob.data(), m_source.length() + 1);
}


void ShaderEditor::addNode(Node* node, const ImVec2& pos)
{
	m_nodes.push(node);
	node->m_pos = pos;
	node->m_id = ++m_last_node_id;
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
}

void ShaderEditor::save(OutputMemoryStream& blob) {
	blob.reserve(4096);
	blob.write(m_last_node_id);
	for (const StaticString<50>& t : m_textures) {
		blob.writeString(t);
	}

	const i32 nodes_count = m_nodes.size();
	blob.write(nodes_count);
	for(auto* node : m_nodes) {
		saveNode(blob, *node);
	}

	const i32 links_count = m_links.size();
	blob.write(links_count);
	blob.write(m_links.begin(), m_links.byte_size());
}

void ShaderEditor::clear()
{
	for (auto* node : m_nodes) {
		LUMIX_DELETE(m_allocator, node);
	}
	m_nodes.clear();

	m_undo_stack.clear();
	m_undo_stack_idx = -1;

	m_last_node_id = 0;
}

ShaderEditor::Node* ShaderEditor::createNode(int type) {
	switch ((NodeType)type) {
		case NodeType::PBR:							return LUMIX_NEW(m_allocator, PBRNode)(*this);
		case NodeType::FLOAT_CONSTANT:				return LUMIX_NEW(m_allocator, ConstNode<ValueType::FLOAT>)(*this);
		case NodeType::VEC4_CONSTANT:				return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC4>)(*this);
		case NodeType::MIX:							return LUMIX_NEW(m_allocator, MixNode)(*this);
		case NodeType::SAMPLE:						return LUMIX_NEW(m_allocator, SampleNode)(*this);
		case NodeType::MULTIPLY:					return LUMIX_NEW(m_allocator, MultiplyNode)(*this);
		case NodeType::SWIZZLE:						return LUMIX_NEW(m_allocator, SwizzleNode)(*this);
		case NodeType::VEC4:						return LUMIX_NEW(m_allocator, Vec4Node)(*this);
		case NodeType::OPERATOR:					return LUMIX_NEW(m_allocator, OperatorNode)(*this);
		case NodeType::BUILTIN_UNIFORM:				return LUMIX_NEW(m_allocator, BuiltinUniformNode)(*this);
		case NodeType::VERTEX_ID:					return LUMIX_NEW(m_allocator, VertexIDNode)(*this);
		case NodeType::PASS:						return LUMIX_NEW(m_allocator, PassNode)(*this);
		case NodeType::IF:							return LUMIX_NEW(m_allocator, IfNode)(*this);
		case NodeType::POSITION:					return LUMIX_NEW(m_allocator, PositionNode)(*this);
		case NodeType::NORMAL:						return LUMIX_NEW(m_allocator, NormalNode)(*this);
		case NodeType::FUNCTION_CALL:				return LUMIX_NEW(m_allocator, FunctionCallNode)(*this);
		case NodeType::BINARY_FUNCTION_CALL:		return LUMIX_NEW(m_allocator, BinaryFunctionCallNode)(*this);
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
}

void ShaderEditor::load(InputMemoryStream& blob) {
	blob.read(m_last_node_id);
	for (StaticString<50>& t : m_textures) {
		t = blob.readString();
	}

	int size;
	blob.read(size);
	for(int i = 0; i < size; ++i) {
		loadNode(blob);
	}

	blob.read(size);
	m_links.resize(size);
	blob.read(m_links.begin(), m_links.byte_size());
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


void ShaderEditor::onGUIRightColumn()
{
	ImGui::BeginChild("right_col");
	
	static ImVec2 offset = ImVec2(0, 0);
	ImGuiEx::BeginNodeEditor("shader_editor", &offset);
		
	for (Node*& node : m_nodes) {
		ImGuiEx::BeginNode(node->m_id, node->m_pos);
		if (node->onGUI()) {
			saveUndo(node->m_id);
		}
		ImGuiEx::EndNode();
	}

	i32 hovered_link = -1;
	for (i32 i = 0, c = m_links.size(); i < c; ++i) {
		ImGuiEx::NodeLink(m_links[i].from | OUTPUT_FLAG, m_links[i].to);
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

	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0) && ImGui::GetIO().KeyCtrl) {
		m_links.erase(hovered_link);
		saveUndo(0xffFF);
	}

	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
		ImGui::OpenPopup("context_menu");
		m_context_link = hovered_link;
	}

	if(ImGui::BeginPopup("context_menu")) {
		if (m_context_link == -1) {
			static char filter[64] = "";
			ImGui::SetNextItemWidth(150);
			ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
			for (const auto& node_type : NODE_TYPES) {
				if (!filter[0] || stristr(node_type.name, filter)) {
					if (ImGui::MenuItem(node_type.name)) {
						Node* n = createNode((int)node_type.type);
						n->m_id = ++m_last_node_id;
						m_nodes.push(n);
						saveUndo(0xffFF);
					}
					}
			}
		}

		if (m_context_link != -1 && ImGui::Selectable("Remove link")) {
			m_links.erase(m_context_link);
			m_context_link = -1;
			saveUndo(0xffFF);
		}

		ImGui::EndPopup();
	}		

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
			ImGui::InputTextMultiline("##src", m_source.getData(), m_source.length(), ImVec2(0, 300), ImGuiInputTextFlags_ReadOnly);
		}
	}

	ImGui::PopItemWidth();
	ImGui::EndChild();
}

void ShaderEditor::saveUndo(u16 id) {
	while (m_undo_stack.size() > m_undo_stack_idx + 1) m_undo_stack.pop();
	++m_undo_stack_idx;

	Undo u(m_allocator);
	u.id = id;
	save(u.blob);
	if (id == 0xffFF || m_undo_stack.back().id != id) {
		m_undo_stack.push(static_cast<Undo&&>(u));
	}
	else {
		m_undo_stack.back() = static_cast<Undo&&>(u);
	}
	generate("", false);
}

bool ShaderEditor::canUndo() const { return m_undo_stack_idx > 0; }
bool ShaderEditor::canRedo() const { return m_undo_stack_idx < m_undo_stack.size() - 1; }

void ShaderEditor::undo() {
	if (m_undo_stack_idx < 0) return;
	
	load(InputMemoryStream(m_undo_stack[m_undo_stack_idx].blob));
	--m_undo_stack_idx;
}

void ShaderEditor::redo() {
	if (m_undo_stack_idx + 1 >= m_undo_stack.size()) return;
	
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

	for (auto& t : m_textures) t = "";
	m_last_node_id = 0;
	m_path = "";
	
	m_nodes.push(LUMIX_NEW(m_allocator, PBRNode)(*this));
	m_nodes.back()->m_pos.x = 50;
	m_nodes.back()->m_pos.y = 50;
	m_nodes.back()->m_id = ++m_last_node_id;

	m_textures[0] = "Albedo";
	m_textures[1] = "Normal";
	m_textures[2] = "Roughness";
	m_textures[3] = "Metallic";

	m_undo_stack.clear();
	m_undo_stack_idx = -1;
	saveUndo(0xffFF);
}

void ShaderEditor::onGUIMenu()
{
	if(ImGui::BeginMenuBar()) {
		if(ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("New")) newGraph();
			if (ImGui::MenuItem("Open")) load();
			if (ImGui::MenuItem("Save", nullptr, false, !m_path.isEmpty())) save(m_path.c_str());
			if (ImGui::MenuItem("Save as")) {
				if(getSavePath() && !m_path.isEmpty()) save(m_path.c_str());
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit")) {
			if (ImGui::MenuItem("Undo", nullptr, false, canUndo())) undo();
			if (ImGui::MenuItem("Redo", nullptr, false, canRedo())) redo();
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Generate & save", nullptr, false, !m_path.isEmpty())) {
			generate(m_path.c_str(), true);
		}

		ImGui::EndMenuBar();
	}
}

void ShaderEditor::onGUI()
{
	if (!m_is_open) return;
	StaticString<LUMIX_MAX_PATH + 25> title("Shader Editor");
	if (!m_path.isEmpty()) title << " - " << m_path.c_str();
	title << "###Shader Editor";
	if (ImGui::Begin(title, &m_is_open, ImGuiWindowFlags_MenuBar))
	{
		m_is_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow);

		onGUIMenu();
		onGUILeftColumn();
		ImVec2 size(m_left_col_width, 0);
		ImGui::SameLine();
		ImGuiEx::VSplitter("vsplit", &size);
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