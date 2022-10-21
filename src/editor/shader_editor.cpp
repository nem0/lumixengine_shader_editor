#define LUMIX_NO_CUSTOM_CRT
#include "shader_editor.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
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

enum class Version {
	FIRST,
	LAST
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

	NUMBER,
	VEC2,
	VEC3,
	VEC4,
	SAMPLE,
	SWIZZLE,
	TIME,
	VERTEX_ID,
	PASS,
	POSITION,
	NORMAL,
	UV0,
	IF,

	// operators
	MULTIPLY,
	ADD,

	// binary functions
	MIX,
	DOT,
	CROSS,
	MIN,
	MAX,
	DISTANCE,
	
	// unary functions
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
	TRUNC
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
	{nullptr, "Vertex ID", NodeType::VERTEX_ID},
	{nullptr, "Pass", NodeType::PASS},
	{nullptr, "If", NodeType::IF},

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
	{"Function", "Mix", NodeType::MIX},
	{"Function", "Normalize", NodeType::NORMALIZE},
	{"Function", "Not", NodeType::NOT},
	{"Function", "Round", NodeType::ROUND},
	{"Function", "Saturate", NodeType::SATURATE},
	{"Function", "Sin", NodeType::SIN},
	{"Function", "Sqrt", NodeType::SQRT},
	{"Function", "Tan", NodeType::TAN},
	{"Function", "Transpose", NodeType::TRANSPOSE},
	{"Function", "Trunc", NodeType::TRUNC},

	{"Function", "Dot", NodeType::DOT},
	{"Function", "Cross", NodeType::CROSS},
	{"Function", "Min", NodeType::MIN},
	{"Function", "Max", NodeType::MAX},
	{"Function", "Distance", NodeType::DISTANCE},

	{"Math", "Multiply", NodeType::MULTIPLY},
	{"Math", "Add", NodeType::ADD},

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


ShaderEditor::Node::Node(NodeType type, ShaderEditor& editor)
	: m_type(type)
	, m_editor(editor)
	, m_id(0xffFF)
{
}

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

	bool onGUI() override {
		ImGui::TextUnformatted("Multiply");
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

template <NodeType Type>
struct FunctionCallNode : public ShaderEditor::Node
{
	explicit FunctionCallNode(ShaderEditor& editor)
		: Node(Type, editor)
	{}

	void save(OutputMemoryStream& blob) override {}
	void load(InputMemoryStream& blob) override {}

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

	ShaderEditor::ValueType getOutputType(int) const override
	{
		// TODO
		return ShaderEditor::ValueType::FLOAT;
	}

	static const char* getName() {
		switch (Type) {
			case NodeType::MIX: return "mix";
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
		ImGui::TextUnformatted(getName());
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
struct UVNode : public ShaderEditor::Node {
	explicit UVNode(ShaderEditor& editor)
		: Node(Type, editor)
	{}

	void save(OutputMemoryStream&) override {}
	void load(InputMemoryStream&) override {}
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC3; }

	void printReference(OutputMemoryStream& blob, int output_idx) const {
		switch(Type) {
			case NodeType::UV0: blob << "v_uv"; break;
			default: ASSERT(false); break;
		}
		
	}

	bool onGUI() override {
		ImGuiEx::Pin(m_id | OUTPUT_FLAG, false);
		switch(Type) {
			case NodeType::UV0: ImGui::Text("UV0"); break;
			default: ASSERT(false); break;
		}
		return false;
	}
};

struct NormalNode : public ShaderEditor::Node {
	explicit NormalNode(ShaderEditor& editor)
		: Node(NodeType::NORMAL, editor)
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
		: Node(NodeType::POSITION, editor)
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
		bool res = false;

		const char* labels[] = { "X", "Y", "Z", "W" };

		ImGui::BeginGroup();
		switch(m_type) {
			case ShaderEditor::ValueType::VEC4:
				for (u16 i = 0; i < 4; ++i) {
					ImGuiEx::Pin(m_id | (i << 16), true);
					if (isInputConnected(m_editor, m_id, i)) {
						ImGui::TextUnformatted(labels[i]);
					}
					else {
						res = ImGui::DragFloat(labels[i], &m_value[i]);
					}
				}
				ImGui::Checkbox("Color", &m_is_color);
				if (m_is_color) {
					res = ImGui::ColorPicker4("##col", m_value) || res; 
				}
				break;
			case ShaderEditor::ValueType::VEC3:
				ImGuiEx::Pin(m_id | (1 << 16), true);
				res = ImGui::DragFloat("x", &m_value[0]);
				ImGuiEx::Pin(m_id | (2 << 16), true);
				res = ImGui::DragFloat("y", &m_value[1]) || res;
				ImGuiEx::Pin(m_id | (3 << 16), true);
				res = ImGui::DragFloat("z", &m_value[2]) || res;
				ImGui::Checkbox("Color", &m_is_color);
				if (m_is_color) {
					res = ImGui::ColorPicker3("##col", m_value) || res; 
				}
				break;
			case ShaderEditor::ValueType::VEC2:
				ImGuiEx::Pin(m_id | (1 << 16), true);
				res = ImGui::DragFloat("x", &m_value[0]);
				ImGuiEx::Pin(m_id | (2 << 16), true);
				res = ImGui::DragFloat("y", &m_value[1]) || res;
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
	{
		m_texture = 0;
	}

	void save(OutputMemoryStream& blob) override { blob.write(m_texture); }
	void load(InputMemoryStream& blob) override { blob.read(m_texture); }
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC4; }

	void generate(OutputMemoryStream& blob) const override {
		blob << "\t\tvec4 v" << m_id << " = ";
		const Input input0 = getInput(m_editor, m_id, 0);
		blob << "texture(" << m_editor.getTextureName(m_texture) << ", ";
		if (input0) input0.printReference(blob);
		else blob << "v_uv";
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
		: Node(NodeType::PBR, editor)
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
		const struct {
			const char* name;
			const char* default_value;
		}
		fields[] = { 
			{ "wpos", "vec4(0, 0, 0, 1)" },
			{ "albedo", "vec3(1, 0, 1)" },
			{ "alpha", "1" }, 
			{ "N", "v_normal" },
			{ "roughness", "1" },
			{ "metallic", "0" },
			{ "emission", "0" },
			{ "ao", "1" },
			{ "translucency", "1" },
			{ "shadow", "1" }
		};

		for (const auto& field : fields) {
			const int i = int(&field - fields);
			Input input = getInput(m_editor, m_id, i);
			if (input) {
				input.node->generate(blob);
				blob << "\t\tdata." << field.name << " = ";
				input.printReference(blob);
				const ShaderEditor::ValueType type = input.node->getOutputType(input.output_idx);
				if (type == ShaderEditor::ValueType::VEC4 && i <= 1) blob << ".xyz";
				blob << ";\n";
			}
			else if (i > 0) {
				blob << "\t\tdata." << field.name << " = " << field.default_value << ";\n";
			}
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
		ImGui::TextUnformatted("Shadow");

		return false;
	}
};


struct PassNode : public ShaderEditor::Node
{
	explicit PassNode(ShaderEditor& editor)
		: Node(NodeType::PASS, editor)
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
		return ShaderEditor::ValueType::FLOAT;
	}

	static const char* getVarName() {
		switch (Type) {
			case NodeType::TIME: return "Global.time";
			default: ASSERT(false); return "Error";
		}
	}

	static const char* getName() {
		switch (Type) {
			case NodeType::TIME: return "Time";
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
	m_is_open = m_app.getSettings().getValue(Settings::GLOBAL, "is_shader_editor_open", false);
}

void ShaderEditor::onBeforeSettingsSaved() {
	m_app.getSettings().setValue(Settings::GLOBAL, "is_shader_editor_open", m_is_open);
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
{
	newGraph();
	m_undo_action.init(ICON_FA_UNDO "Undo", "Shader editor undo", "shader_editor_undo", ICON_FA_UNDO, os::Keycode::Z, Action::Modifiers::CTRL, true);
	m_undo_action.func.bind<&ShaderEditor::undo>(this);
	m_undo_action.plugin = this;

	m_redo_action.init(ICON_FA_REDO "Redo", "Shader editor redo", "shader_editor_redo", ICON_FA_REDO, os::Keycode::Z, Action::Modifiers::CTRL | Action::Modifiers::SHIFT, true);
	m_redo_action.func.bind<&ShaderEditor::redo>(this);
	m_redo_action.plugin = this;

	m_delete_action.init(ICON_FA_TRASH "Delete", "Shader editor delete", "shader_editor_delete", ICON_FA_TRASH, os::Keycode::DEL, Action::Modifiers::NONE, true);
	m_delete_action.func.bind<&ShaderEditor::deleteSelectedNode>(this);
	m_delete_action.plugin = this;

	m_toggle_ui.init("Shader Editor", "Toggle shader editor", "shaderEditor", "", true);
	m_toggle_ui.func.bind<&ShaderEditor::onToggle>(this);
	m_toggle_ui.is_selected.bind<&ShaderEditor::isOpen>(this);

	m_app.addWindowAction(&m_toggle_ui);
	m_app.addAction(&m_undo_action);
	m_app.addAction(&m_redo_action);
	m_app.addAction(&m_delete_action);
}

void ShaderEditor::deleteSelectedNode() {
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
	blob.write(Version::LAST);
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
		case NodeType::VEC4:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC4>)(*this);
		case NodeType::VEC3:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC3>)(*this);
		case NodeType::VEC2:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC2>)(*this);
		case NodeType::NUMBER:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::FLOAT>)(*this);
		case NodeType::SAMPLE:						return LUMIX_NEW(m_allocator, SampleNode)(*this);
		case NodeType::MULTIPLY:					return LUMIX_NEW(m_allocator, OperatorNode<NodeType::MULTIPLY>)(*this);
		case NodeType::ADD:							return LUMIX_NEW(m_allocator, OperatorNode<NodeType::ADD>)(*this);
		case NodeType::SWIZZLE:						return LUMIX_NEW(m_allocator, SwizzleNode)(*this);
		case NodeType::TIME:						return LUMIX_NEW(m_allocator, UniformNode<NodeType::TIME>)(*this);
		case NodeType::VERTEX_ID:					return LUMIX_NEW(m_allocator, VertexIDNode)(*this);
		case NodeType::PASS:						return LUMIX_NEW(m_allocator, PassNode)(*this);
		case NodeType::IF:							return LUMIX_NEW(m_allocator, IfNode)(*this);
		case NodeType::POSITION:					return LUMIX_NEW(m_allocator, PositionNode)(*this);
		case NodeType::NORMAL:						return LUMIX_NEW(m_allocator, NormalNode)(*this);
		case NodeType::UV0:							return LUMIX_NEW(m_allocator, UVNode<NodeType::UV0>)(*this);
		
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

		case NodeType::MIX:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::MIX>)(*this);
		case NodeType::DOT:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::DOT>)(*this);
		case NodeType::CROSS:						return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::CROSS>)(*this);
		case NodeType::MIN:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::MIN>)(*this);
		case NodeType::MAX:							return LUMIX_NEW(m_allocator, BinaryFunctionCallNode<NodeType::MAX>)(*this);
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
	Version version;
	blob.read(version);
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

void ShaderEditor::onGUIRightColumn()
{
	ImGui::BeginChild("right_col");
	
	static ImVec2 offset = ImVec2(0, 0);
	ImGuiEx::BeginNodeEditor("shader_editor", &offset);
	const ImVec2 origin = ImGui::GetCursorScreenPos();
		
	for (Node*& node : m_nodes) {
		ImGuiEx::BeginNode(node->m_id, node->m_pos, &node->m_selected);
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
		m_context_link = hovered_link;
		open_context = true;
	}

	if(ImGui::BeginPopup("context_menu")) {
		if (m_context_link == -1) {
			static char filter[64] = "";
			ImGui::SetNextItemWidth(150);
			if (open_context) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
			if (filter[0]) {
				for (const auto& node_type : NODE_TYPES) {
					if (stristr(node_type.name, filter)) {
						if (ImGui::MenuItem(node_type.name)) {
							addNode(node_type.type, mp);
						}
					}
				}
			}
			else {
				nodeGroupUI(*this, Span(NODE_TYPES), mp);
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
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit")) {
			menuItem(m_undo_action, canUndo());
			menuItem(m_redo_action, canRedo());
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Generate & save", nullptr, false, !m_path.isEmpty())) {
			generate(m_path.c_str(), true);
		}

		ImGui::EndMenuBar();
	}
}

void ShaderEditor::onWindowGUI()
{
	m_is_focused = false;
	if (!m_is_open) return;

	StaticString<LUMIX_MAX_PATH + 25> title("Shader Editor");
	if (!m_path.isEmpty()) title << " - " << m_path.c_str();
	title << "###Shader Editor";

	if (ImGui::Begin(title, &m_is_open, ImGuiWindowFlags_MenuBar))
	{
		m_is_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		onGUIMenu();
		onGUILeftColumn();
		ImVec2 size(m_left_col_width, 0);
		ImGui::SameLine();
		ImGuiEx::VSplitter("vsplit", &size);
		m_left_col_width = size.x;
		ImGui::SameLine();
		onGUIRightColumn();
	}
	ImGui::End();
}


} // namespace Lumix