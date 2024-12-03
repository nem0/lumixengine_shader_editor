#include "core/crt.h"
#include "core/log.h"
#include "core/math.h"
#include "core/os.h"
#include "core/path.h"
#include "core/profiler.h"
#include "core/stream.h"
#include "core/string.h"
#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/editor_asset.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "engine/component_uid.h"
#include "engine/engine.h"
#include "engine/plugin.h"
#include "engine/world.h"
#include "renderer/editor/particle_editor.h"
#include "renderer/model.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "imgui/IconsFontAwesome5.h"


namespace Lumix {

namespace {

struct ShaderEditor;

enum class Version {
	FIRST,
	LAST
};

enum class ShaderResourceEditorType : u32 {
	SURFACE,
	PARTICLE,
	FUNCTION
};

// serialized, do not change order
enum class ShaderNodeType {
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

	FUNCTION_INPUT,
	FUNCTION_OUTPUT,
	FUNCTION_CALL,

	PARTICLE_STREAM
};

struct ShaderEditorResource {
	using Link = NodeEditorLink;

	enum class ValueType : i32 {
		BOOL,
		FLOAT,
		INT,
		VEC2,
		VEC3,
		VEC4,
		IVEC4,

		COUNT,
		NONE
	};

	struct Node : NodeEditorNode {
		Node(ShaderEditorResource& resource);
		virtual ~Node() {}

		virtual void serialize(OutputMemoryStream&blob) {}
		virtual void deserialize(InputMemoryStream&blob) {}
		virtual void printReference(OutputMemoryStream& blob, int output_idx) const;
		virtual ValueType getOutputType(int index) const { return ValueType::FLOAT; }
		virtual ShaderNodeType getType() const = 0;

		bool nodeGUI() override;
		bool generateOnce(OutputMemoryStream& blob);

		void inputSlot();
		void outputSlot();

		bool m_selected = false;
		bool m_reachable = false;
		bool m_generated = false;
		u32 m_input_count = 0;
		u32 m_output_count = 0;

		ShaderEditorResource& m_resource;
		String m_error;

	protected:
		virtual bool generate(OutputMemoryStream& blob) { return true; }
		virtual bool onGUI() = 0;
		bool error(const char* msg) { m_error = msg; return false; }
	};

	ShaderEditorResource(const Path& path, ShaderEditor& editor, IAllocator& allocator)
		: m_editor(editor)
		, m_allocator(allocator)
		, m_links(m_allocator)
		, m_nodes(m_allocator)
		, m_path(path)
	{}

	~ShaderEditorResource() {
		for (auto* node : m_nodes) {
			LUMIX_DELETE(m_allocator, node);
		}
	}

	bool load(StudioApp& app) {
		OutputMemoryStream content(m_allocator);
		if (!app.getEngine().getFileSystem().getContentSync(m_path, content)) {
			logError("Failed to read ", m_path);
			return false;
		}
		
		InputMemoryStream blob(content);
		if (!deserialize(blob)) logError("Failed to deserialize ", m_path);
		return true;
	}

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

	void colorLinks() {
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

	void markReachableNodes() const {
		for (Node* n : m_nodes) {
			n->m_reachable = false;
		}
		markReachable(m_nodes[0]);
	}

	void clearGeneratedFlags() {
		for (Node* n : m_nodes) n->m_generated = false;
	}

	void destroyNode(Node* node) {
		for (i32 i = m_links.size() - 1; i >= 0; --i) {
			if (m_links[i].getFromNode() == node->m_id || m_links[i].getToNode() == node->m_id) {
				m_links.swapAndPop(i);
			}
		}

		LUMIX_DELETE(m_allocator, node);
		m_nodes.eraseItem(node);
	}

	void clear() {
		m_last_node_id = 0;
		m_links.clear();
		for (Node* n : m_nodes) {
			LUMIX_DELETE(m_allocator, n);
		}
		m_nodes.clear();
	}

	void markReachable(Node* node) const {
		node->m_reachable = true;

		forEachInput(*this, node->m_id, [&](ShaderEditorResource::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
			markReachable(from);
		});
	}

	void colorLinks(ImU32 color, u32 link_idx) {
		m_links[link_idx].color = color;
		const u32 from_node_id = m_links[link_idx].getFromNode();
		for (u32 i = 0, c = m_links.size(); i < c; ++i) {
			if (m_links[i].getToNode() == from_node_id) colorLinks(color, i);
		}
	}

	void deleteSelectedNodes() {
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
	
	void deleteUnreachable() {
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

	static void serializeNode(OutputMemoryStream& blob, Node& node) {
		int type = (int)node.getType();
		blob.write(node.m_id);
		blob.write(type);
		blob.write(node.m_pos);

		node.serialize(blob);
	}
	
	Node& deserializeNode(InputMemoryStream& blob) {
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

	bool generate(String* source) {
		markReachableNodes();
		colorLinks();

		OutputMemoryStream blob(m_allocator);
		blob.reserve(32 * 1024);

		for (Node* n : m_nodes) n->m_error = "";
		if (!m_nodes[0]->generateOnce(blob)) return false;

		if (source) {
			source->resize((u32)blob.size());
			memcpy(source->getMutableData(), blob.data(), source->length());
			source->getMutableData()[source->length()] = '\0';
		}
		return true;
	}

	void serialize(OutputMemoryStream& blob) {
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

		generate(nullptr);
	}

	bool deserialize(InputMemoryStream& blob) {
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

	Node* createNode(int type);
	void init(ShaderResourceEditorType type);
	ShaderResourceEditorType getShaderType() const;
	ValueType getFunctionOutputType() const;

	IAllocator& m_allocator;
	ShaderEditor& m_editor;
	Path m_path;
	Array<Link> m_links;
	Array<Node*> m_nodes;
	int m_last_node_id = 0;

	static ResourceType TYPE;
};

ResourceType ShaderEditorResource::TYPE("shader_graph");

struct ShaderEditor final : StudioApp::IPlugin {
	struct FunctionPlugin : EditorAssetPlugin {
		FunctionPlugin(ShaderEditor& editor) 
			: EditorAssetPlugin("Shader graph function", "sfn", TYPE, editor.m_app, editor.m_allocator)
			, m_editor(editor)
		{}
		
		void addSubresources(AssetCompiler& compiler, const Path& path) override {
			compiler.addResource(TYPE, path);
			m_editor.addFunction(path);
		}

		void openEditor(const Path& path) override { m_editor.open(path); }

		void createResource(OutputMemoryStream& blob) override {
			ShaderEditorResource res(Path("new shader function"), m_editor, m_editor.m_allocator);
			res.init(ShaderResourceEditorType::FUNCTION);
			res.serialize(blob);
		}

		ShaderEditor& m_editor;
		static ResourceType TYPE;
	};

	struct AssetPlugin : EditorAssetPlugin {
		AssetPlugin(ShaderEditor& editor)
			: EditorAssetPlugin("Shader graph", "sed", Shader::TYPE, editor.m_app, editor.m_allocator)
			, m_editor(editor)
		{}

		bool compile(const Path& src) override {
			ShaderEditorResource res(src, m_editor, m_editor.m_allocator);
			if (!res.load(m_app)) {
				logError("Failed to load ", src);
				return false;
			}

			String source(m_editor.m_allocator);
			if (!res.generate(&source)) return false;

			Span<const u8> span((const u8*)source.c_str(), source.length());
			
			m_editor.registerDependencies(res);
			
			return m_editor.m_app.getAssetCompiler().writeCompiledResource(src, span);
		}

		void createResource(OutputMemoryStream& blob) override {
			ShaderEditorResource res(Path("new surface shader"), m_editor, m_editor.m_allocator);
			res.init(ShaderResourceEditorType::SURFACE);
			res.serialize(blob);
		}

		void openEditor(const Path& path) override { m_editor.open(path); }

		void listLoaded() override {
			auto& resources = m_editor.m_app.getAssetCompiler().lockResources();
			for (const AssetCompiler::ResourceItem& res : resources) {
				if (res.type != FunctionPlugin::TYPE) continue;
				m_editor.addFunction(res.path);
			}
			m_editor.m_app.getAssetCompiler().unlockResources();
		}

		ShaderEditor& m_editor;
	};

	ShaderEditor(StudioApp& app)
		: m_allocator(app.getAllocator(), "shader editor")
		, m_app(app)
		, m_functions(m_allocator)
		, m_function_plugin(*this)
		, m_asset_plugin(*this)
	{}

	void registerDependencies(const ShaderEditorResource& res);

	void init() override {}
	const char* getName() const override { return "shader editor"; }
	bool showGizmo(WorldView&, ComponentUID) override { return false; }

	void addFunction(const Path& path) {
		FileSystem& fs = m_app.getEngine().getFileSystem();
		OutputMemoryStream data(m_allocator);
		UniquePtr<ShaderEditorResource> shd = UniquePtr<ShaderEditorResource>::create(m_allocator, path, *this, m_allocator);
		if (!fs.getContentSync(path, data)) {
			logError("Failed to load ", path);
			return;
		}

		m_functions.eraseItems([&](const UniquePtr<ShaderEditorResource>& f){ return f->m_path == path; });
		InputMemoryStream blob(data);
		shd->deserialize(blob);
		shd->m_path = path;
		ASSERT(shd->getShaderType() == ShaderResourceEditorType::FUNCTION);
		m_functions.emplace(shd.move());
	}

	void open(const Path& path);

	TagAllocator m_allocator;
	StudioApp& m_app;
	Array<UniquePtr<ShaderEditorResource>> m_functions;
	FunctionPlugin m_function_plugin;
	AssetPlugin m_asset_plugin;
};

ResourceType ShaderEditor::FunctionPlugin::TYPE("shader_graph_function");

struct VertexOutput {
	ShaderEditorResource::ValueType type;
	StaticString<32> name;
};

static ShaderEditorResource::ValueType toType(const gpu::Attribute& attr) {
	switch (attr.type) {
		case gpu::AttributeType::FLOAT:
			break;
		case gpu::AttributeType::I16:
		case gpu::AttributeType::I8:
			if (attr.flags & gpu::Attribute::AS_INT) {
				switch (attr.components_count) {
					case 1: return ShaderEditorResource::ValueType::INT;
					case 2: ASSERT(false); break;
					case 3: ASSERT(false); break;
					case 4: return ShaderEditorResource::ValueType::IVEC4;
				}
				ASSERT(false);
				return ShaderEditorResource::ValueType::NONE;
			}
			break;
		case gpu::AttributeType::U16:
		case gpu::AttributeType::U8:
			if (attr.flags & gpu::Attribute::AS_INT) {
				switch (attr.components_count) {
					case 1: ASSERT(false); break;
					case 2: ASSERT(false); break;
					case 3: ASSERT(false); break;
					case 4: ASSERT(false); break;
				}
				ASSERT(false);
				return ShaderEditorResource::ValueType::NONE;
			}
			break;
	}
	switch (attr.components_count) {
		case 1: return ShaderEditorResource::ValueType::FLOAT;
		case 2: return ShaderEditorResource::ValueType::VEC2;
		case 3: return ShaderEditorResource::ValueType::VEC3;
		case 4: return ShaderEditorResource::ValueType::VEC4;
	}
	ASSERT(false);
	return ShaderEditorResource::ValueType::NONE;
}

static constexpr const char* toString(ShaderEditorResource::ValueType type) {
	switch (type) {
		case ShaderEditorResource::ValueType::COUNT:
		case ShaderEditorResource::ValueType::NONE:
			return "error";
		case ShaderEditorResource::ValueType::BOOL: return "bool";
		case ShaderEditorResource::ValueType::INT: return "int";
		case ShaderEditorResource::ValueType::FLOAT: return "float";
		case ShaderEditorResource::ValueType::VEC2: return "vec2";
		case ShaderEditorResource::ValueType::VEC3: return "vec3";
		case ShaderEditorResource::ValueType::VEC4: return "vec4";
		case ShaderEditorResource::ValueType::IVEC4: return "ivec4";
	}
	ASSERT(false);
	return "Unknown type";
}

static bool edit(const char* label, ShaderEditorResource::ValueType* type) {
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

struct Input {
	ShaderEditorResource::Node* node = nullptr;
	u16 output_idx;

	void printReference(OutputMemoryStream& blob) const { node->printReference(blob, output_idx); }
	operator bool() const { return node != nullptr; }
};

static Input getInput(const ShaderEditorResource& resource, u16 node_id, u16 input_idx) {
	Input res;
	ShaderEditorResource::forEachInput(resource, node_id, [&](ShaderEditorResource::Node* from, u16 from_attr, u16 to_attr, u32 link_idx){
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

ShaderEditorResource::Node::Node(ShaderEditorResource& resource)
	: m_resource(resource)
	, m_error(resource.m_allocator)
{
	m_id = 0xffFF;
}

void ShaderEditorResource::Node::inputSlot() {
	ImGuiEx::Pin(m_id | (m_input_count << 16), true);
	++m_input_count;
}

void ShaderEditorResource::Node::outputSlot() {
	ImGuiEx::Pin(m_id | (m_output_count << 16) | NodeEditor::OUTPUT_FLAG, false);
	++m_output_count;
}

bool ShaderEditorResource::Node::generateOnce(OutputMemoryStream& blob) {
	if (m_generated) return true;
	m_generated = true;
	return generate(blob);
}

bool ShaderEditorResource::Node::nodeGUI() {
	ImGuiEx::BeginNode(m_id, m_pos, &m_selected);
	m_input_count = 0;
	m_output_count = 0;
	bool res = onGUI();

	if (m_error.length() > 0) {
		ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0xff, 0, 0, 0xff));
	}
	ImGuiEx::EndNode();
	if (m_error.length() > 0) {
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_error.c_str());
	}

	ASSERT((m_input_count > 0) == hasInputPins());
	ASSERT((m_output_count > 0) == hasOutputPins());

	return res;
}

struct MixNode : ShaderEditorResource::Node {
	explicit MixNode(ShaderEditorResource& resource)
		: Node(resource)
	{}
	
	ShaderNodeType getType() const override { return ShaderNodeType::MIX; }

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

	bool generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
		const Input input2 = getInput(m_resource, m_id, 2);
		if (!input0 || !input1 || !input2) {
			return error("Missing input");
		}
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
		return true;
	}
};

struct CodeNode : ShaderEditorResource::Node {
	explicit CodeNode(ShaderEditorResource& resource, IAllocator& allocator)
		: Node(resource)
		, m_allocator(allocator)
		, m_inputs(allocator)
		, m_outputs(allocator)
		, m_code(allocator)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::CODE; }

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
							ImGui::SetNextItemWidth(-1);
							changed = inputString("##name", &var.name) || changed;
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
				if (inputStringMultiline("##code", &m_code, ImVec2(-1, ImGui::GetContentRegionAvail().y))) {
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

	bool generate(OutputMemoryStream& blob) override {
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
		return true;
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

template <ShaderNodeType Type>
struct OperatorNode : ShaderEditorResource::Node {
	explicit OperatorNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return Type; }

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

	bool generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		const Input input1 = getInput(m_resource, m_id, 1);
		if (input0) input0.node->generateOnce(blob);
		if (input1) input1.node->generateOnce(blob);
		return true;
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
			case ShaderNodeType::MULTIPLY: blob << " * "; break;
			case ShaderNodeType::ADD: blob << " + "; break;
			case ShaderNodeType::DIVIDE: blob << " / "; break;
			case ShaderNodeType::SUBTRACT: blob << " - "; break;
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
			case ShaderNodeType::ADD: return "Add";
			case ShaderNodeType::SUBTRACT: return "Subtract";
			case ShaderNodeType::MULTIPLY: return "Multiply";
			case ShaderNodeType::DIVIDE: return "Divide";
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
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::ONEMINUS; }

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	ShaderEditorResource::ValueType getOutputType(int index) const override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return ShaderEditorResource::ValueType::FLOAT;
		return input.node->getOutputType(input.output_idx);
	}

	bool generate(OutputMemoryStream& blob) override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return error("Missing input");
		input.node->generateOnce(blob);
		return true;
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
		: Node(resource)
	{
		m_swizzle = "xyzw";
	}
	
	ShaderNodeType getType() const override { return ShaderNodeType::SWIZZLE; }

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
	
	bool generate(OutputMemoryStream& blob) override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return error("Missing input");
		input.node->generateOnce(blob);
		return true;
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
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::FRESNEL; }

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

	bool generate(OutputMemoryStream& blob) override {
		// TODO use data.normal instead of v_normal
		blob << "float v" << m_id << " = mix(" << F0 << ", 1.0, pow(1 - saturate(dot(-normalize(v_wpos.xyz), v_normal)), " << power << "));\n";
		return true;
	}

	float F0 = 0.04f;
	float power = 5.0f;
};

struct FunctionInputNode : ShaderEditorResource::Node {
	explicit FunctionInputNode(ShaderEditorResource& resource, IAllocator& allocator)
		: Node(resource)
		, m_name(allocator)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::FUNCTION_INPUT; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	bool onGUI() override {
		ImColor color(ImGui::GetStyle().Colors[ImGuiCol_Tab]);
		ImGuiEx::BeginNodeTitleBar(color);
		ImGui::Text("Input %s (%s)", m_name.c_str(), toString(m_type));
		ImGuiEx::EndNodeTitleBar();

		outputSlot();
		bool res = inputString("Name", &m_name);
		res = edit("Type", &m_type) || res;
		return res;
	}

	void serialize(OutputMemoryStream& blob) override {
		blob.writeString(m_name.c_str());
		blob.write(m_type);
	}
	void deserialize(InputMemoryStream& blob) override {
		m_name = blob.readString();
		blob.read(m_type);
	}
	
	bool generate(OutputMemoryStream& blob) override { return true; }
	
	void printReference(OutputMemoryStream& blob, int attr_idx) const override {
		blob << m_name.c_str();
	}

	ShaderEditorResource::ValueType getOutputType(i32) const override { return m_type; }

	String m_name;
	ShaderEditorResource::ValueType m_type = ShaderEditorResource::ValueType::FLOAT;
};

struct FunctionOutputNode : ShaderEditorResource::Node {
	explicit FunctionOutputNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::FUNCTION_OUTPUT; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return false; }

	bool onGUI() override {
		ImGuiEx::NodeTitle("Function output");
		inputSlot();
		ImGui::TextUnformatted(" ");
		return false;
	}

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}
	
	bool generate(OutputMemoryStream& blob) override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return error("Missing input");

		const ShaderEditorResource::ValueType output_type = input.node->getOutputType(input.output_idx);
		StringView name = Path::getBasename(m_resource.m_path.c_str());
		blob << toString(output_type) << " " << name << "(";

		bool first_arg = true;
		for (const Node* node : m_resource.m_nodes) {
			if (node->getType() != ShaderNodeType::FUNCTION_INPUT) continue;
			
			FunctionInputNode* n = (FunctionInputNode*)node;
			if (!first_arg) blob << ", ";
			blob << toString(n->m_type) << " " << n->m_name.c_str();
			first_arg = false;
		}

		blob << ") {\n";
		if (input) {
			input.node->generateOnce(blob);
			blob << "\treturn ";
			input.printReference(blob);
			blob << ";\n";
		}
		blob << "}";
		return true;
	}
};

struct FunctionCallNode : ShaderEditorResource::Node {
	explicit FunctionCallNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::FUNCTION_CALL; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override { blob.writeString(m_function_resource->m_path.c_str()); }
	void deserialize(InputMemoryStream& blob) override {
		const char* path = blob.readString();
		for (const auto& f : m_resource.m_editor.m_functions) {
			if (f->m_path == path) {
				m_function_resource = f.get();
				break;
			}
		}
	}

	ShaderEditorResource::ValueType getOutputType(int) const override { return m_function_resource->getFunctionOutputType(); }
	
	bool generate(OutputMemoryStream& blob) override {
		StringView fn_name = Path::getBasename(m_function_resource->m_path.c_str());
		ShaderEditorResource::ValueType type = m_function_resource->getFunctionOutputType();
		blob << "\t" << toString(type) << " v" << m_id << " = " << fn_name << "(";
		u32 input_count = 0;
		for (const Node* n : m_function_resource->m_nodes) {
			if (n->getType() == ShaderNodeType::FUNCTION_INPUT) ++input_count;
		}

		for (u32 i = 0; i < input_count; ++i) {
			const Input input = getInput(m_resource, m_id, i);
			if (!input) {
				return error("Input not connected");
			}
			if (i > 0) blob << ", ";
			input.printReference(blob);
		}
		blob << ");\n";
		return true;
	}

	bool onGUI() override {
		StringView basename = Path::getBasename(m_function_resource->m_path.c_str());
		StaticString<MAX_PATH> name(basename);
		ImGuiEx::NodeTitle(name);
		outputSlot();
		for (const Node* node : m_function_resource->m_nodes) {
			if (node->getType() != ShaderNodeType::FUNCTION_INPUT) continue;

			FunctionInputNode* n = (FunctionInputNode*)node;
			inputSlot();
			ImGui::TextUnformatted(n->m_name.c_str());
		}
		return false;
	}

	ShaderEditorResource* m_function_resource;
};

template <ShaderNodeType Type>
struct BuiltinFunctionCallNode : ShaderEditorResource::Node
{
	explicit BuiltinFunctionCallNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return Type; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override { 
		if constexpr (Type == ShaderNodeType::LENGTH) return ShaderEditorResource::ValueType::FLOAT;
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) return input0.node->getOutputType(input0.output_idx);
		return ShaderEditorResource::ValueType::FLOAT;
	}

	static const char* getName() {
		switch (Type) {
			case ShaderNodeType::ABS: return "abs";
			case ShaderNodeType::ALL: return "all";
			case ShaderNodeType::ANY: return "any";
			case ShaderNodeType::CEIL: return "ceil";
			case ShaderNodeType::COS: return "cos";
			case ShaderNodeType::EXP: return "exp";
			case ShaderNodeType::EXP2: return "exp2";
			case ShaderNodeType::FLOOR: return "floor";
			case ShaderNodeType::FRACT: return "fract";
			case ShaderNodeType::LENGTH: return "length";
			case ShaderNodeType::LOG: return "log";
			case ShaderNodeType::LOG2: return "log2";
			case ShaderNodeType::NORMALIZE: return "normalize";
			case ShaderNodeType::NOT: return "not";
			case ShaderNodeType::ROUND: return "round";
			case ShaderNodeType::SATURATE: return "saturate";
			case ShaderNodeType::SIN: return "sin";
			case ShaderNodeType::SQRT: return "sqrt";
			case ShaderNodeType::TAN: return "tan";
			case ShaderNodeType::TRANSPOSE: return "transpose";
			case ShaderNodeType::TRUNC: return "trunc";
			default: ASSERT(false); return "error";
		}
	}

	bool generate(OutputMemoryStream& blob) override {
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
		return true;
	}

	bool onGUI() override {
		inputSlot();
		ImGui::TextUnformatted(getName());
		ImGui::SameLine();
		outputSlot();
		return false;
	}
};

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
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::POW; }

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

	bool generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		if (!input0) return error("Missing input");
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
		return true;
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

template <ShaderNodeType Type>
struct BinaryBuiltinFunctionCallNode : ShaderEditorResource::Node
{
	explicit BinaryBuiltinFunctionCallNode(ShaderEditorResource& resource)
		: Node(resource)
	{
	}
	
	ShaderNodeType getType() const override { return Type; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override { 
		switch (Type) {
			case ShaderNodeType::DISTANCE:
			case ShaderNodeType::DOT: return ShaderEditorResource::ValueType::FLOAT;
			default: break;
		}
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) return input0.node->getOutputType(input0.output_idx);
		return ShaderEditorResource::ValueType::FLOAT;
	}

	static const char* getName() {
		switch (Type) {
			case ShaderNodeType::DOT: return "dot";
			case ShaderNodeType::CROSS: return "cross";
			case ShaderNodeType::MIN: return "min";
			case ShaderNodeType::MAX: return "max";
			case ShaderNodeType::DISTANCE: return "distance";
			default: ASSERT(false); return "error";
		}
	}

	bool generate(OutputMemoryStream& blob) override {
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
		return true;
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
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::POSITION; }

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override { blob.write(m_space); }
	void deserialize(InputMemoryStream& blob) override { blob.read(m_space); }

	ShaderEditorResource::ValueType getOutputType(int) const override { return ShaderEditorResource::ValueType::VEC3; }

	void printReference(OutputMemoryStream& blob, int output_idx) const override {
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

template <ShaderNodeType Type>
struct VaryingNode : ShaderEditorResource::Node {
	explicit VaryingNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return Type; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream&) override {}
	void deserialize(InputMemoryStream&) override {}

	ShaderEditorResource::ValueType getOutputType(int) const override { 
		switch(Type) {
			case ShaderNodeType::NORMAL: return ShaderEditorResource::ValueType::VEC3;
			case ShaderNodeType::UV0: return ShaderEditorResource::ValueType::VEC2;
			default: ASSERT(false); return ShaderEditorResource::ValueType::VEC3;
		}
	}

	void printReference(OutputMemoryStream& blob, int output_idx) const override {
		switch(Type) {
			case ShaderNodeType::NORMAL: blob << "v_normal"; break;
			case ShaderNodeType::UV0: blob << "v_uv"; break;
			default: ASSERT(false); break;
		}
		
	}

	bool onGUI() override {
		outputSlot();
		switch(Type) {
			case ShaderNodeType::NORMAL: ImGui::Text("Normal"); break;
			case ShaderNodeType::UV0: ImGui::Text("UV0"); break;
			default: ASSERT(false); break;
		}
		return false;
	}
};

template <ShaderEditorResource::ValueType TYPE>
struct ConstNode : ShaderEditorResource::Node
{
	explicit ConstNode(ShaderEditorResource& resource)
		: Node(resource)
	{
		m_value[0] = m_value[1] = m_value[2] = m_value[3] = 0;
		m_int_value = 0;
	}

	ShaderNodeType getType() const override {
		switch(TYPE) {
			case ShaderEditorResource::ValueType::VEC4: return ShaderNodeType::VEC4;
			case ShaderEditorResource::ValueType::VEC3: return ShaderNodeType::VEC3;
			case ShaderEditorResource::ValueType::VEC2: return ShaderNodeType::VEC2;
			case ShaderEditorResource::ValueType::FLOAT: return ShaderNodeType::NUMBER;
			default: ASSERT(false); return ShaderNodeType::NUMBER;
		}
	}

	void serialize(OutputMemoryStream& blob) override
	{
		blob.write(m_value);
		blob.write(m_int_value);
	}

	void deserialize(InputMemoryStream& blob) override 
	{
		blob.read(m_value);
		blob.read(m_int_value);
	}

	ShaderEditorResource::ValueType getOutputType(int) const override { return TYPE; }

	void printInputValue(u32 idx, OutputMemoryStream& blob) const {
		const Input input = getInput(m_resource, m_id, idx);
		if (input) {
			input.printReference(blob);
			return;
		}
		blob << m_value[idx];
	}

	bool generate(OutputMemoryStream& blob) override {
		for (u32 i = 0; i < 4; ++i) {
			const Input input = getInput(m_resource, m_id, i);
			if (input) input.node->generateOnce(blob);
		}
		return true;
	}

	void printReference(OutputMemoryStream& blob, int output_idx) const override {
		switch(TYPE) {
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
		switch (TYPE) {
			case ShaderEditorResource::ValueType::VEC4: channels_count = 4; break;
			case ShaderEditorResource::ValueType::VEC3: channels_count = 3; break;
			case ShaderEditorResource::ValueType::VEC2: channels_count = 2; break;
			default: channels_count = 1; break;
		}
			
		switch(TYPE) {
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
				switch (TYPE) {
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
		switch (TYPE) {
			case ShaderEditorResource::ValueType::VEC4:
			case ShaderEditorResource::ValueType::VEC3:
			case ShaderEditorResource::ValueType::VEC2:
				return true;
			default: return false;
		}
	}

	bool hasOutputPins() const override { return true; }

	float m_value[4];
	int m_int_value;
};

struct SampleNode : ShaderEditorResource::Node
{
	explicit SampleNode(ShaderEditorResource& resource, IAllocator& allocator)
		: Node(resource)
		, m_texture(allocator)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::SAMPLE; }


	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override { blob.writeString(m_texture.c_str()); }
	void deserialize(InputMemoryStream& blob) override { m_texture = blob.readString(); }
	ShaderEditorResource::ValueType getOutputType(int) const override { return ShaderEditorResource::ValueType::VEC4; }

	bool generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) input0.node->generateOnce(blob);
		blob << "\t\tvec4 v" << m_id << " = ";
		char var_name[64];
		Shader::toTextureVarName(Span(var_name), m_texture.c_str());
		blob << "texture(" << var_name << ", ";
		if (input0) input0.printReference(blob);
		else blob << "v_uv";
		blob << ");\n";
		return true;
	}

	bool onGUI() override {
		inputSlot();
		ImGui::Text("UV");

		ImGui::SameLine();
		outputSlot();
		return inputString("Texture", &m_texture);
	}

	String m_texture;
};

struct AppendNode : ShaderEditorResource::Node {
	explicit AppendNode(ShaderEditorResource& resource)
		: Node(resource)
	{}
	
	ShaderNodeType getType() const override { return ShaderNodeType::APPEND; }
	
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

	bool generate(OutputMemoryStream& blob) override {
		const Input input0 = getInput(m_resource, m_id, 0);
		if (input0) input0.node->generateOnce(blob);
		const Input input1 = getInput(m_resource, m_id, 1);
		if (input1) input1.node->generateOnce(blob);
		return true;
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
		: Node(resource)
		, m_define(allocator)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::STATIC_SWITCH; }
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
		ImGui::SetNextItemWidth(80);
		return inputString("##param", &m_define);
	}

	void serialize(OutputMemoryStream& blob) override { blob.writeString(m_define.c_str()); }
	void deserialize(InputMemoryStream& blob) override { m_define = blob.readString(); }
	
	const char* getOutputTypeName() const {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return "float";
		return toString(input.node->getOutputType(input.output_idx));
	}

	bool generate(OutputMemoryStream& blob) override {
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
			blob << getOutputTypeName() << " v" << m_id << " = "; \
			input1.printReference(blob);
			blob << ";\n";
		}
		blob << "#endif\n";
		return true;
	}
	
	ShaderEditorResource::ValueType getOutputType(int) const override {
		const Input input = getInput(m_resource, m_id, 0);
		if (input) return input.node->getOutputType(input.output_idx);
		return ShaderEditorResource::ValueType::FLOAT;
	}

	String m_define;
};

template <ShaderNodeType Type>
struct ParameterNode : ShaderEditorResource::Node {
	explicit ParameterNode(ShaderEditorResource& resource, IAllocator& allocator)
		: Node(resource)
		, m_name(allocator)
	{}

	ShaderNodeType getType() const override { return Type; }
	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override { blob.writeString(m_name.c_str()); }
	void deserialize(InputMemoryStream& blob) override { m_name = blob.readString(); }

	bool onGUI() override {
		const ImU32 color = ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
		switch(Type) {
			case ShaderNodeType::SCALAR_PARAM: ImGuiEx::NodeTitle("Scalar param", color); break;
			case ShaderNodeType::VEC4_PARAM: ImGuiEx::NodeTitle("Vec4 param", color); break;
			case ShaderNodeType::COLOR_PARAM: ImGuiEx::NodeTitle("Color param", color); break;
			default: ASSERT(false); ImGuiEx::NodeTitle("Error"); break;
		}
		
		outputSlot();
		return inputString("##name", &m_name);
	}

	bool generate(OutputMemoryStream& blob) override {
		switch(Type) {
			case ShaderNodeType::SCALAR_PARAM: blob << "\tfloat v"; break;
			case ShaderNodeType::VEC4_PARAM: blob << "\tvec4 v"; break;
			case ShaderNodeType::COLOR_PARAM: blob << "\tvec4 v"; break;
			default: ASSERT(false); blob << "\tfloat v"; break;
		}
		char var_name[64];
		Shader::toUniformVarName(Span(var_name), m_name.c_str());

		blob << m_id << " = " << var_name << ";";
		return true;
	}

	String m_name;
};

struct PinNode : ShaderEditorResource::Node {
	explicit PinNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::PIN; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }
	
	bool generate(OutputMemoryStream& blob) override {
		const Input input = getInput(m_resource, m_id, 0);
		if (!input) return error("Missing input");
		input.node->generateOnce(blob);
		return true;
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
		: Node(resource)
		, m_vertex_decl(gpu::PrimitiveType::TRIANGLE_STRIP)
		, m_attributes_names(resource.m_allocator)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::PBR; }

	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return false; }

	void serialize(OutputMemoryStream& blob) override {
		blob.write(m_type);
		blob.write(m_vertex_decl);
		blob.write(m_is_masked);
		blob.write(m_attributes_names.size());
		for (const String& a : m_attributes_names) {
			blob.writeString(a.c_str());
		}
	}

	void deserialize(InputMemoryStream& blob) override {
		blob.read(m_type);
		blob.read(m_vertex_decl);
		blob.read(m_is_masked);
		u32 c;
		blob.read(c);
		m_attributes_names.reserve(c);
		for (u32 i = 0; i < c; ++i) {
			m_attributes_names.emplace(blob.readString(), m_resource.m_allocator);
		}
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
			case gpu::AttributeType::U16:
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

	bool generate(OutputMemoryStream& blob) override;

	bool onGUI() override {
		ImGuiEx::NodeTitle(m_type == Type::SURFACE ? "PBR Surface" : "PBR Particles");

		bool changed = ImGui::Combo("Type", (i32*)&m_type, "SURFACE\0PARTICLES\0");

		inputSlot(); ImGui::TextUnformatted("Albedo");
		inputSlot(); ImGui::TextUnformatted("Normal");
		inputSlot(); ImGui::TextUnformatted("Opacity");
		inputSlot(); ImGui::TextUnformatted("Roughness");
		inputSlot(); ImGui::TextUnformatted("Metallic");
		inputSlot(); ImGui::TextUnformatted("Emission");
		inputSlot(); ImGui::TextUnformatted("AO");
		inputSlot(); ImGui::TextUnformatted("Translucency");
		inputSlot(); ImGui::TextUnformatted("Shadow");
		inputSlot(); ImGui::TextUnformatted("Position offset");

		ImGui::Checkbox("Masked", &m_is_masked);

		if (m_type == Type::PARTICLES && ImGui::Button("Copy vertex declaration")) {
			m_show_fs = true;
		}

		FileSelector& fs = m_resource.m_editor.m_app.getFileSelector();
		if (fs.gui("Select particle", &m_show_fs, "par", false)) {
			// TODO emitter index
			m_vertex_decl = ParticleEditor::getVertexDecl(fs.getPath(), 0, m_attributes_names, m_resource.m_editor.m_app);
			if (m_vertex_decl.attributes_count < 1 || m_vertex_decl.attributes[0].components_count != 3) {
				logError("First particle shader input must be position (have 3 components)");
			}
			changed = true;
		}

		return changed;
	}

	enum class Type : u32 {
		SURFACE,
		PARTICLES
	};

	Array<String> m_attributes_names;
	gpu::VertexDecl m_vertex_decl;
	Type m_type = Type::SURFACE;
	bool m_show_fs = false;
	bool m_is_masked = false;
};

struct ParticleStreamNode : ShaderEditorResource::Node {
	explicit ParticleStreamNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::PARTICLE_STREAM; }

	bool hasInputPins() const override { return false; }
	bool hasOutputPins() const override { return true; }
	
	void serialize(OutputMemoryStream& blob) override { blob.write(m_stream); }
	void deserialize(InputMemoryStream& blob) override { blob.read(m_stream); }

	ShaderEditorResource::ValueType getOutputType(int idx) const override {
		const Node* n = m_resource.m_nodes[0];
		ASSERT(n->getType() == ShaderNodeType::PBR);
		const PBRNode* pbr = (const PBRNode*)n;
		if (m_stream >= pbr->m_vertex_decl.attributes_count) return ShaderEditorResource::ValueType::FLOAT;
		return toType(pbr->m_vertex_decl.attributes[m_stream]);
	}

	bool generate(OutputMemoryStream& blob) override { return true; }

	void printReference(OutputMemoryStream& blob, int output_idx) const override {
		const Node* n = m_resource.m_nodes[0];
		ASSERT(n->getType() == ShaderNodeType::PBR);
		const PBRNode* pbr = (const PBRNode*)n;
		if (m_stream >= pbr->m_vertex_decl.attributes_count) return;
		
		blob << "v_" << pbr->m_attributes_names[m_stream].c_str();
	}

	bool onGUI() override {
		ImGuiEx::NodeTitle("Particle stream");
		outputSlot();
		const Node* n = m_resource.m_nodes[0];
		ASSERT(n->getType() == ShaderNodeType::PBR);
		const PBRNode* pbr = (const PBRNode*)n;
		const char* preview = m_stream < (u32)pbr->m_attributes_names.size() ? pbr->m_attributes_names[m_stream].c_str() : "N/A";
		ImGui::TextUnformatted(preview);
		return false;
	}

	u32 m_stream = 0;
};

bool PBRNode::generate(OutputMemoryStream& blob) {
	blob << "import \"pipelines/surface_base.inc\"\n\n";
	
	IAllocator& allocator = m_resource.m_allocator;
	Array<String> uniforms(allocator);
	Array<String> defines(allocator);
	Array<String> textures(allocator);
	Array<u32> particle_streams(allocator);
	Array<ShaderEditorResource*> functions(allocator);
	
	auto add_function = [&](FunctionCallNode* n){
		const i32 idx = functions.indexOf(n->m_function_resource);
		if (idx < 0) functions.push(n->m_function_resource);
	};

	auto add_particle_stream = [&](ParticleStreamNode* n){
		const i32 idx = particle_streams.indexOf(n->m_stream);
		if (idx < 0) particle_streams.push(n->m_stream);
	};

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
		switch(n->getType()) {
			case ShaderNodeType::PARTICLE_STREAM:
				add_particle_stream((ParticleStreamNode*)n);
				break;
			case ShaderNodeType::SCALAR_PARAM:
				add_uniform((ParameterNode<ShaderNodeType::SCALAR_PARAM>*)n, "float");
				break;
			case ShaderNodeType::VEC4_PARAM:
				add_uniform((ParameterNode<ShaderNodeType::VEC4_PARAM>*)n, "vec4");
				break;
			case ShaderNodeType::COLOR_PARAM:
				add_uniform((ParameterNode<ShaderNodeType::COLOR_PARAM>*)n, "color");
				break;
			case ShaderNodeType::FUNCTION_CALL:
				add_function((FunctionCallNode*)n);
				break;
			case ShaderNodeType::STATIC_SWITCH:
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
		if (n->getType() == ShaderNodeType::SAMPLE) add_texture((SampleNode*)n);
	}
	blob << "},\n";
	
	if (m_type == Type::PARTICLES && !m_attributes_names.empty()) {
		blob << "vertex_preface = [[\n";
		for (u32 i : particle_streams) {
			blob << "\tlayout(location = " << i << ") in " << typeToString(m_vertex_decl.attributes[i]) << " i_" << m_attributes_names[i].c_str() << ";\n";
			blob << "\tlayout(location = " << i + 1 << ") out " << typeToString(m_vertex_decl.attributes[i]) << " v_" << m_attributes_names[i].c_str() << ";\n";
		}
		blob << R"#(
				layout (location = 0) out vec2 v_uv;
			]],
			vertex = [[
				vec2 pos = vec2(gl_VertexID & 1, (gl_VertexID & 2) * 0.5);
				v_uv = pos;
		)#";
		for (u32 i : particle_streams) {
			const String& a = m_attributes_names[i];
			blob << "\t\tv_" << a.c_str() << " = i_" << a.c_str() << ";\n";
		}
		blob << R"#(
				pos = pos * 2 - 1;
				gl_Position = Pass.projection * ((Pass.view * u_model * vec4(i_)#" << m_attributes_names[0].c_str() << R"#(.xyz, 1)) + vec4(pos.xy, 0, 0));
			]],

			fragment_preface = [[
			)#";
		// TODO vertex shader functions
		for (ShaderEditorResource* f : functions) {
			f->clearGeneratedFlags();
			String s(m_resource.m_allocator);
			if (!f->generate(&s)) return false;
			blob << s.c_str() << "\n\n";
		}
		for (u32 i : particle_streams) {
			blob << "\tlayout(location = " << i + 1 << ") in " << typeToString(m_vertex_decl.attributes[i]) << " v_" << m_attributes_names[i].c_str() << ";\n";
		}
		blob << R"#(
				layout (location = 0) in vec2 v_uv;
			]],
		)#";
	}
	else {
		blob << "fragment_preface = [[\n";
		for (ShaderEditorResource* f : functions) {
			f->clearGeneratedFlags();
			String s(m_resource.m_allocator);
			if (!f->generate(&s)) return false;
			blob << s.c_str() << "\n\n";
		}
		blob << "]],\n\n";
	}


	blob << "fragment = [[\n";
	
	bool need_local_position = false;
	for (Node* n : m_resource.m_nodes) {
		if (n->getType() == ShaderNodeType::POSITION) {
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

	blob << "\tdata.V = vec3(0);\n";
	blob << "\tdata.wpos = vec3(0);\n";
	if (m_is_masked) {
		blob << "\tif (data.alpha < 0.5) discard;\n";
	}
	blob << "]]\n";
	Input po_input = getInput(m_resource, m_id, lengthOf(fields));
	if (po_input) {
		blob << ", vertex [[";
		po_input.node->generateOnce(blob);
		blob << "v_wpos += ";
		po_input.printReference(blob);
		blob << ";\n";
		blob << "]]\n";
	}

	if (need_local_position) blob << ",\nneed_local_position = true\n";
	blob << "})\n";
	return true;
}

struct BackfaceSwitchNode : ShaderEditorResource::Node {
	explicit BackfaceSwitchNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::BACKFACE_SWITCH; }
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

	bool generate(OutputMemoryStream& blob) override {
		const Input inputA = getInput(m_resource, m_id, 0);
		const Input inputB = getInput(m_resource, m_id, 1);
		if (!inputA && !inputB) return error("Missing inputs");
		
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
		return true;
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
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::IF; }
	bool hasInputPins() const override { return true; }
	bool hasOutputPins() const override { return true; }

	void serialize(OutputMemoryStream& blob) override {}
	void deserialize(InputMemoryStream& blob) override {}

	bool generate(OutputMemoryStream& blob) override {
		const Input inputA = getInput(m_resource, m_id, 0);
		const Input inputB = getInput(m_resource, m_id, 1);
		const Input inputGT = getInput(m_resource, m_id, 2);
		const Input inputEQ = getInput(m_resource, m_id, 3);
		const Input inputLT = getInput(m_resource, m_id, 4);
		if (!inputA || !inputB) return error("Missing input");
		if (!inputGT && !inputEQ && !inputLT) return error("Missing input");
		
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
		return true;
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
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return ShaderNodeType::VERTEX_ID; }
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

template <ShaderNodeType Type>
struct UniformNode : ShaderEditorResource::Node
{
	explicit UniformNode(ShaderEditorResource& resource)
		: Node(resource)
	{}

	ShaderNodeType getType() const override { return Type; }
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
			case ShaderNodeType::SCREEN_POSITION: return ShaderEditorResource::ValueType::VEC2;
			case ShaderNodeType::VIEW_DIR: return ShaderEditorResource::ValueType::VEC3;
			case ShaderNodeType::SCENE_DEPTH:
			case ShaderNodeType::PIXEL_DEPTH:
			case ShaderNodeType::TIME:
				return ShaderEditorResource::ValueType::FLOAT;
			default: 
				ASSERT(false);
				return ShaderEditorResource::ValueType::FLOAT;
		}
	}

	static const char* getVarName() {
		switch (Type) {
			case ShaderNodeType::TIME: return "Global.time";
			case ShaderNodeType::VIEW_DIR: return "Pass.view_dir.xyz";
			case ShaderNodeType::PIXEL_DEPTH: return "toLinearDepth(Pass.inv_projection, gl_FragCoord.z)";
			case ShaderNodeType::SCENE_DEPTH: return "toLinearDepth(Pass.inv_projection, texture(u_depthbuffer, gl_FragCoord.xy / Global.framebuffer_size).x)";
			case ShaderNodeType::SCREEN_POSITION: return "(gl_FragCoord.xy / Global.framebuffer_size)";
			default: ASSERT(false); return "Error";
		}
	}

	static const char* getName() {
		switch (Type) {
			case ShaderNodeType::TIME: return "Time";
			case ShaderNodeType::VIEW_DIR: return "View direction";
			case ShaderNodeType::PIXEL_DEPTH: return "Pixel depth";
			case ShaderNodeType::SCENE_DEPTH: return "Scene depth";
			case ShaderNodeType::SCREEN_POSITION: return "Screen position";
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

ShaderEditorResource::ValueType ShaderEditorResource::getFunctionOutputType() const {
	for (const Node* n : m_nodes) {
		if (n->getType() == ShaderNodeType::FUNCTION_OUTPUT) {
			const Input input = getInput(*this, n->m_id, 0);
			if (!input) return ShaderEditorResource::ValueType::NONE;

			return input.node->getOutputType(input.output_idx);
		}
	}
	ASSERT(false);
	return ShaderEditorResource::ValueType::NONE;
}

ShaderResourceEditorType ShaderEditorResource::getShaderType() const {
	
	switch (m_nodes[0]->getType()) {
		case ShaderNodeType::PBR: return ((PBRNode*)m_nodes[0])->m_type == PBRNode::Type::PARTICLES 
			? ShaderResourceEditorType::PARTICLE 
			: ShaderResourceEditorType::SURFACE;
		case ShaderNodeType::FUNCTION_OUTPUT: return ShaderResourceEditorType::FUNCTION;
		default: ASSERT(false); return ShaderResourceEditorType::SURFACE;
	}
}

void ShaderEditorResource::init(ShaderResourceEditorType type) {
	Node* node = nullptr;
	switch(type) {
		case ShaderResourceEditorType::PARTICLE:
		case ShaderResourceEditorType::SURFACE: {
			PBRNode* output = LUMIX_NEW(m_allocator, PBRNode)(*this);
			output->m_type = (type == ShaderResourceEditorType::PARTICLE) ? PBRNode::Type::PARTICLES : PBRNode::Type::SURFACE;
			node = output;
			break;
		}
		case ShaderResourceEditorType::FUNCTION: {
			node = LUMIX_NEW(m_allocator, FunctionOutputNode)(*this);
			break;
		}
	}
	ASSERT(node);
	m_nodes.push(node);
	node->m_pos.x = 50;
	node->m_pos.y = 50;
	node->m_id = ++m_last_node_id;
}

ShaderEditorResource::Node* ShaderEditorResource::createNode(int type) {
	switch ((ShaderNodeType)type) {
		case ShaderNodeType::PBR:						return LUMIX_NEW(m_allocator, PBRNode)(*this);
		case ShaderNodeType::PIN:						return LUMIX_NEW(m_allocator, PinNode)(*this);
		case ShaderNodeType::VEC4:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC4>)(*this);
		case ShaderNodeType::VEC3:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC3>)(*this);
		case ShaderNodeType::VEC2:						return LUMIX_NEW(m_allocator, ConstNode<ValueType::VEC2>)(*this);
		case ShaderNodeType::NUMBER:					return LUMIX_NEW(m_allocator, ConstNode<ValueType::FLOAT>)(*this);
		case ShaderNodeType::SAMPLE:					return LUMIX_NEW(m_allocator, SampleNode)(*this, m_allocator);
		case ShaderNodeType::MULTIPLY:					return LUMIX_NEW(m_allocator, OperatorNode<ShaderNodeType::MULTIPLY>)(*this);
		case ShaderNodeType::ADD:						return LUMIX_NEW(m_allocator, OperatorNode<ShaderNodeType::ADD>)(*this);
		case ShaderNodeType::DIVIDE:					return LUMIX_NEW(m_allocator, OperatorNode<ShaderNodeType::DIVIDE>)(*this);
		case ShaderNodeType::SUBTRACT:					return LUMIX_NEW(m_allocator, OperatorNode<ShaderNodeType::SUBTRACT>)(*this);
		case ShaderNodeType::PARTICLE_STREAM:			return LUMIX_NEW(m_allocator, ParticleStreamNode)(*this);
		case ShaderNodeType::SWIZZLE:					return LUMIX_NEW(m_allocator, SwizzleNode)(*this);
		case ShaderNodeType::TIME:						return LUMIX_NEW(m_allocator, UniformNode<ShaderNodeType::TIME>)(*this);
		case ShaderNodeType::VIEW_DIR:					return LUMIX_NEW(m_allocator, UniformNode<ShaderNodeType::VIEW_DIR>)(*this);
		case ShaderNodeType::PIXEL_DEPTH:				return LUMIX_NEW(m_allocator, UniformNode<ShaderNodeType::PIXEL_DEPTH>)(*this);
		case ShaderNodeType::SCENE_DEPTH:				return LUMIX_NEW(m_allocator, UniformNode<ShaderNodeType::SCENE_DEPTH>)(*this);
		case ShaderNodeType::SCREEN_POSITION:			return LUMIX_NEW(m_allocator, UniformNode<ShaderNodeType::SCREEN_POSITION>)(*this);
		case ShaderNodeType::VERTEX_ID:					return LUMIX_NEW(m_allocator, VertexIDNode)(*this);
		case ShaderNodeType::BACKFACE_SWITCH:			return LUMIX_NEW(m_allocator, BackfaceSwitchNode)(*this);
		case ShaderNodeType::IF:						return LUMIX_NEW(m_allocator, IfNode)(*this);
		case ShaderNodeType::STATIC_SWITCH:				return LUMIX_NEW(m_allocator, StaticSwitchNode)(*this, m_allocator);
		case ShaderNodeType::FUNCTION_INPUT:			return LUMIX_NEW(m_allocator, FunctionInputNode)(*this, m_allocator);
		case ShaderNodeType::FUNCTION_OUTPUT:			return LUMIX_NEW(m_allocator, FunctionOutputNode)(*this);
		case ShaderNodeType::FUNCTION_CALL:				return LUMIX_NEW(m_allocator, FunctionCallNode)(*this);
		case ShaderNodeType::ONEMINUS:					return LUMIX_NEW(m_allocator, OneMinusNode)(*this);
		case ShaderNodeType::CODE:						return LUMIX_NEW(m_allocator, CodeNode)(*this, m_allocator);
		case ShaderNodeType::APPEND:					return LUMIX_NEW(m_allocator, AppendNode)(*this);
		case ShaderNodeType::FRESNEL:					return LUMIX_NEW(m_allocator, FresnelNode)(*this);
		case ShaderNodeType::POSITION:					return LUMIX_NEW(m_allocator, PositionNode)(*this);
		case ShaderNodeType::NORMAL:					return LUMIX_NEW(m_allocator, VaryingNode<ShaderNodeType::NORMAL>)(*this);
		case ShaderNodeType::UV0:						return LUMIX_NEW(m_allocator, VaryingNode<ShaderNodeType::UV0>)(*this);
		case ShaderNodeType::SCALAR_PARAM:				return LUMIX_NEW(m_allocator, ParameterNode<ShaderNodeType::SCALAR_PARAM>)(*this, m_allocator);
		case ShaderNodeType::COLOR_PARAM:				return LUMIX_NEW(m_allocator, ParameterNode<ShaderNodeType::COLOR_PARAM>)(*this, m_allocator);
		case ShaderNodeType::VEC4_PARAM:				return LUMIX_NEW(m_allocator, ParameterNode<ShaderNodeType::VEC4_PARAM>)(*this, m_allocator);
		case ShaderNodeType::MIX:						return LUMIX_NEW(m_allocator, MixNode)(*this);
		
		case ShaderNodeType::ABS:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::ABS>)(*this);
		case ShaderNodeType::ALL:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::ALL>)(*this);
		case ShaderNodeType::ANY:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::ANY>)(*this);
		case ShaderNodeType::CEIL:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::CEIL>)(*this);
		case ShaderNodeType::COS:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::COS>)(*this);
		case ShaderNodeType::EXP:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::EXP>)(*this);
		case ShaderNodeType::EXP2:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::EXP2>)(*this);
		case ShaderNodeType::FLOOR:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::FLOOR>)(*this);
		case ShaderNodeType::FRACT:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::FRACT>)(*this);
		case ShaderNodeType::LOG:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::LOG>)(*this);
		case ShaderNodeType::LOG2:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::LOG2>)(*this);
		case ShaderNodeType::NORMALIZE:					return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::NORMALIZE>)(*this);
		case ShaderNodeType::NOT:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::NOT>)(*this);
		case ShaderNodeType::ROUND:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::ROUND>)(*this);
		case ShaderNodeType::SATURATE:					return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::SATURATE>)(*this);
		case ShaderNodeType::SIN:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::SIN>)(*this);
		case ShaderNodeType::SQRT:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::SQRT>)(*this);
		case ShaderNodeType::TAN:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::TAN>)(*this);
		case ShaderNodeType::TRANSPOSE:					return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::TRANSPOSE>)(*this);
		case ShaderNodeType::TRUNC:						return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::TRUNC>)(*this);
		case ShaderNodeType::LENGTH:					return LUMIX_NEW(m_allocator, BuiltinFunctionCallNode<ShaderNodeType::LENGTH>)(*this);

		case ShaderNodeType::DOT:						return LUMIX_NEW(m_allocator, BinaryBuiltinFunctionCallNode<ShaderNodeType::DOT>)(*this);
		case ShaderNodeType::CROSS:						return LUMIX_NEW(m_allocator, BinaryBuiltinFunctionCallNode<ShaderNodeType::CROSS>)(*this);
		case ShaderNodeType::MIN:						return LUMIX_NEW(m_allocator, BinaryBuiltinFunctionCallNode<ShaderNodeType::MIN>)(*this);
		case ShaderNodeType::MAX:						return LUMIX_NEW(m_allocator, BinaryBuiltinFunctionCallNode<ShaderNodeType::MAX>)(*this);
		case ShaderNodeType::POW:						return LUMIX_NEW(m_allocator, PowerNode)(*this);
		case ShaderNodeType::DISTANCE:					return LUMIX_NEW(m_allocator, BinaryBuiltinFunctionCallNode<ShaderNodeType::DISTANCE>)(*this);
	}

	ASSERT(false);
	return nullptr;
}

struct ShaderEditorWindow : public AssetEditorWindow, NodeEditor {
	using Node = ShaderEditorResource::Node;
	using Link = NodeEditorLink;

	struct INodeTypeVisitor {
		struct ICreator {
			virtual void create(ShaderEditorWindow& editor, ImVec2 pos) = 0;
		};

		virtual bool beginCategory(const char* name) { return true; }
		virtual void endCategory() {}
		virtual INodeTypeVisitor& visitType(const char* label, ICreator& creator, char shortcut = 0) = 0;
		
		INodeTypeVisitor& visitType(const char* label, ShaderNodeType type, char shortcut = 0);
	};

	explicit ShaderEditorWindow(const Path& path, ShaderEditor& editor, StudioApp& app, IAllocator& allocator)
		: NodeEditor(app.getAllocator())
		, AssetEditorWindow(app)
		, m_editor(editor)
		, m_allocator(allocator)
		, m_app(app)
		, m_source(allocator)
		, m_resource(path, editor, allocator)
	{
		m_resource.load(app);
		pushUndo(NO_MERGE_UNDO);
		m_dirty = false;
	}

	void windowGUI() override {
		if (m_source_open) {
			ImGui::SetNextWindowSize(ImVec2(300, 300), ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Shader source", &m_source_open)) {
				if (m_source.length() == 0) {
					ImGui::Text("Empty");
				} else {
					ImGui::SetNextItemWidth(-1);
					ImGui::InputTextMultiline("##src", m_source.getMutableData(), m_source.length(), ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiInputTextFlags_ReadOnly);
				}
			}
			ImGui::End();
		}

		onGUIMenu();
		
		FileSelector& fs = m_app.getFileSelector();
		if (fs.gui("Save As", &m_show_save_as, "sed", true)) saveAs(fs.getPath());

		ImGui::BeginChild("canvas");
		nodeEditorGUI(m_resource.m_nodes, m_resource.m_links);
		ImGui::EndChild();
	}

	const Path& getPath() override { return m_resource.m_path; }

	void pushUndo(u32 tag) override {
		m_dirty = true;
		SimpleUndoRedo::pushUndo(tag);
		m_resource.generate(&m_source);
	}

	const char* getName() const override { return "shader_editor"; }

	void saveAs(const char* path) {
		// TODO update shaders using a function when the function is changed
		os::OutputFile file;
		FileSystem& fs = m_app.getEngine().getFileSystem();
	
		OutputMemoryStream blob(m_allocator);
		m_resource.serialize(blob);
		if (!fs.saveContentSync(Path(path), blob)) {
			logError("Could not save ", path);
			return;
		}

		m_resource.m_path = path;
		m_dirty = false;
	}

	void load(const char* path) {
		FileSystem& fs = m_app.getEngine().getFileSystem();
		OutputMemoryStream data(m_allocator);
		if (!fs.getContentSync(Path(path), data)) {
			logError("Failed to load ", path);
			return;
		}

		InputMemoryStream blob(data);
		m_resource.deserialize(blob);
		m_resource.m_path = path;

		clearUndoStack();
		pushUndo(NO_MERGE_UNDO);
	}

	void serialize(OutputMemoryStream& blob) override {
		m_resource.serialize(blob);
	}

	void deserialize(InputMemoryStream& blob) override {
		m_resource.clear();
		m_resource.deserialize(blob);
	}

	void onCanvasClicked(ImVec2 pos, i32 hovered_link) override {
		struct : INodeTypeVisitor {
			INodeTypeVisitor& visitType(const char* label, ICreator& creator, char shortcut) override {
				if (shortcut && os::isKeyDown((os::Keycode)shortcut)) {
					creator.create(*editor, pos);
					if (hovered_link >= 0) editor->splitLink(editor->m_resource.m_nodes.back(), editor->m_resource.m_links, hovered_link);
					editor->pushUndo(NO_MERGE_UNDO);
				}
				return *this;
			}
			ShaderEditorWindow* editor;
			ImVec2 pos;
			i32 hovered_link;
		} visitor;
		visitor.editor = this;
		visitor.pos = pos;
		visitor.hovered_link = hovered_link;
		visitNodeTypes(visitor);
	}

	void onLinkDoubleClicked(ShaderEditorWindow::Link& link, ImVec2 pos) override {
		ShaderEditorResource::Node* n = addNode(ShaderNodeType::PIN, pos);
		ShaderEditorResource::Link new_link;
		new_link.color = link.color;
		new_link.from = n->m_id | OUTPUT_FLAG; 
		new_link.to = link.to;
		link.to = n->m_id;
		m_resource.m_links.push(new_link);
		pushUndo(SimpleUndoRedo::NO_MERGE_UNDO);
	}

	void onContextMenu(ImVec2 pos) override {
		static char filter[64] = "";
		ImGui::SetNextItemWidth(150);
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::InputTextWithHint("##filter", "Filter", filter, sizeof(filter));
		if (filter[0]) {
			struct : INodeTypeVisitor {
				INodeTypeVisitor& visitType(const char* label, ICreator& creator, char shortcut) override {
					if (!created && findInsensitive(label, filter)) {
						if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::MenuItem(label)) {
							creator.create(*editor, pos);
							editor->pushUndo(NO_MERGE_UNDO);
							filter[0] = '\0';
							ImGui::CloseCurrentPopup();
							created = true;
						}
					}
					return *this;
				}
				bool created = false;
				ImVec2 pos;
				ShaderEditorWindow* editor;
			} visitor;
			visitor.editor = this;
			visitor.pos = pos;
			visitNodeTypes(visitor);
		}
		else {
			struct : INodeTypeVisitor {
				bool beginCategory(const char* name) override { return ImGui::BeginMenu(name); }
				void endCategory() override { ImGui::EndMenu(); }

				INodeTypeVisitor& visitType(const char* label, ICreator& creator, char shortcut) override {
					if (ImGui::MenuItem(label)) {
						creator.create(*editor, pos);
						editor->pushUndo(NO_MERGE_UNDO);
					}
					return *this;
				}

				ImVec2 pos;
				ShaderEditorWindow* editor;
			} visitor;
			visitor.editor = this;
			visitor.pos = pos;
			visitNodeTypes(visitor);
		}
	}

	void onGUIMenu() {
		CommonActions& actions = m_app.getCommonActions();
		if (m_app.checkShortcut(actions.del)) deleteSelectedNodes();
		else if (m_app.checkShortcut(actions.save)) saveAs(m_resource.m_path.c_str());
		else if (m_app.checkShortcut(actions.undo)) undo();
		else if (m_app.checkShortcut(actions.redo)) redo();
		
		if(ImGui::BeginMenuBar()) {
			if(ImGui::BeginMenu("File")) {
				ImGui::MenuItem("View source", nullptr, &m_source_open);
				if (menuItem(actions.save, true)) saveAs(m_resource.m_path.c_str());
				if (ImGui::MenuItem("Save as")) m_show_save_as = true;
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit")) {
				if (menuItem(actions.undo, canUndo())) undo();
				if (menuItem(actions.redo, canRedo())) redo();
				if (ImGui::MenuItem(ICON_FA_BRUSH "Clear")) deleteUnreachable();
				ImGui::EndMenu();
			}
			if (ImGuiEx::IconButton(ICON_FA_SAVE, "Save")) saveAs(m_resource.m_path.c_str());
			if (ImGuiEx::IconButton(ICON_FA_UNDO, "Undo", canUndo())) undo();
			if (ImGuiEx::IconButton(ICON_FA_REDO, "Redo", canRedo())) redo();
			if (ImGuiEx::IconButton(ICON_FA_BRUSH, "Clear")) deleteUnreachable();
			ImGui::EndMenuBar();
		}
	}

	void deleteSelectedNodes() {
		if (m_is_any_item_active) return;
		m_resource.deleteSelectedNodes();
		pushUndo(NO_MERGE_UNDO);
	}

	void deleteUnreachable() {
		m_resource.deleteUnreachable();
		pushUndo(NO_MERGE_UNDO);
	}

	ShaderEditorResource::Node* addNode(ShaderNodeType node_type, ImVec2 pos) {
		Node* n = m_resource.createNode((int)node_type);
		n->m_id = ++m_resource.m_last_node_id;
		n->m_pos = pos;
		m_resource.m_nodes.push(n);
		if (m_half_link_start) {
			if (m_half_link_start & OUTPUT_FLAG) {
				if (n->hasInputPins()) m_resource.m_links.push({u32(m_half_link_start) & ~OUTPUT_FLAG, u32(n->m_id)});
			}
			else {
				if (n->hasOutputPins()) m_resource.m_links.push({u32(n->m_id), u32(m_half_link_start)});
			}
			m_half_link_start = 0;
		}
		return n;
	}

	void visitNodeTypes(INodeTypeVisitor& visitor) {
		if (visitor.beginCategory("Constants")) {
			visitor.visitType("Time", ShaderNodeType::TIME)
			.visitType("Vertex ID", ShaderNodeType::VERTEX_ID)
			.visitType("View direction", ShaderNodeType::VIEW_DIR)
			.endCategory();
		}

		if (visitor.beginCategory("Functions")) {
			for (const auto& fn : m_editor.m_functions) {
				const StaticString<MAX_PATH> name(Path::getBasename(fn->m_path.c_str()));
				struct : INodeTypeVisitor::ICreator {
					void create(ShaderEditorWindow& editor, ImVec2 pos) override {
						FunctionCallNode* node = (FunctionCallNode*)editor.addNode(ShaderNodeType::FUNCTION_CALL, pos);
						node->m_function_resource = res;
					};
					ShaderEditorResource* res;
				} creator;
				creator.res = fn.get();
				visitor.visitType(name, creator);
			}
			visitor.endCategory();
		}

		if (visitor.beginCategory("Math")) {
			visitor.visitType("Abs", ShaderNodeType::ABS)
			.visitType("All", ShaderNodeType::ALL)
			.visitType("Any", ShaderNodeType::ANY)
			.visitType("Ceil", ShaderNodeType::CEIL)
			.visitType("Cos", ShaderNodeType::COS)
			.visitType("Exp", ShaderNodeType::EXP, 'E')
			.visitType("Exp2", ShaderNodeType::EXP2)
			.visitType("Floor", ShaderNodeType::FLOOR)
			.visitType("Fract", ShaderNodeType::FRACT)
			.visitType("Log", ShaderNodeType::LOG)
			.visitType("Log2", ShaderNodeType::LOG2)
			.visitType("Normalize", ShaderNodeType::NORMALIZE, 'N')
			.visitType("Not", ShaderNodeType::NOT)
			.visitType("Round", ShaderNodeType::ROUND)
			.visitType("Saturate", ShaderNodeType::SATURATE)
			.visitType("Sin", ShaderNodeType::SIN)
			.visitType("Sqrt", ShaderNodeType::SQRT)
			.visitType("Tan", ShaderNodeType::TAN)
			.visitType("Transpose", ShaderNodeType::TRANSPOSE)
			.visitType("Trunc", ShaderNodeType::TRUNC)
			.visitType("Cross", ShaderNodeType::CROSS)
			.visitType("Distance", ShaderNodeType::DISTANCE)
			.visitType("Dot", ShaderNodeType::DOT, 'D')
			.visitType("Length", ShaderNodeType::LENGTH, 'L')
			.visitType("Max", ShaderNodeType::MAX)
			.visitType("Min", ShaderNodeType::MIN)
			.visitType("Power", ShaderNodeType::POW, 'P')
			.visitType("Add", ShaderNodeType::ADD, 'A')
			.visitType("Append", ShaderNodeType::APPEND)
			.visitType("Divide", ShaderNodeType::DIVIDE)
			.visitType("Mix", ShaderNodeType::MIX, 'X')
			.visitType("Multiply", ShaderNodeType::MULTIPLY, 'M')
			.visitType("One minus", ShaderNodeType::ONEMINUS, 'O')
			.visitType("Subtract", ShaderNodeType::SUBTRACT)
			.endCategory();
		}

		if (visitor.beginCategory("Parameters")) {
			visitor.visitType("Color", ShaderNodeType::COLOR_PARAM, 'C')
			.visitType("Scalar", ShaderNodeType::SCALAR_PARAM, 'P')
			.visitType("Vec4", ShaderNodeType::VEC4_PARAM, 'V')
			.endCategory();
		}

		if (visitor.beginCategory("Utility")) {
			visitor.visitType("Fresnel", ShaderNodeType::FRESNEL)
			.visitType("Custom code", ShaderNodeType::CODE)
			.visitType("Backface switch", ShaderNodeType::BACKFACE_SWITCH)
			.visitType("If", ShaderNodeType::IF, 'I')
			.visitType("Pixel depth", ShaderNodeType::PIXEL_DEPTH)
			.visitType("Scene depth", ShaderNodeType::SCENE_DEPTH)
			.visitType("Screen position", ShaderNodeType::SCREEN_POSITION)
			.visitType("Static switch", ShaderNodeType::STATIC_SWITCH)
			.visitType("Swizzle", ShaderNodeType::SWIZZLE, 'S')
			.endCategory();
		}

		if (visitor.beginCategory("Vertex")) {
			visitor.visitType("Normal", ShaderNodeType::NORMAL)
			.visitType("Position", ShaderNodeType::POSITION)
			.visitType("UV0", ShaderNodeType::UV0)
			.endCategory();
		}

		switch (m_resource.getShaderType()) {
			case ShaderResourceEditorType::SURFACE: break;
			case ShaderResourceEditorType::FUNCTION: 
				visitor.visitType("Function input", ShaderNodeType::FUNCTION_INPUT);
				break;
			case ShaderResourceEditorType::PARTICLE: {
				if (visitor.beginCategory("Particles")) {
					PBRNode* o = (PBRNode*)m_resource.m_nodes[0];
					for (const String& a : o->m_attributes_names) {
						struct : INodeTypeVisitor::ICreator {
							void create(ShaderEditorWindow& editor, ImVec2 pos) override {
								ParticleStreamNode* node = (ParticleStreamNode*)editor.addNode(ShaderNodeType::PARTICLE_STREAM, pos);
								node->m_stream = stream;
							}
							u32 stream;
						} creator;
						creator.stream = u32(&a - o->m_attributes_names.begin());
						visitor.visitType(a.c_str(), creator);
					}
					visitor.endCategory();
				}
				break;
			}
		}

		visitor
			.visitType("Sample", ShaderNodeType::SAMPLE, 'T')
			.visitType("Vector 4", ShaderNodeType::VEC4, '4')
			.visitType("Vector 3", ShaderNodeType::VEC3, '3')
			.visitType("Vector 2", ShaderNodeType::VEC2, '2')
			.visitType("Number", ShaderNodeType::NUMBER, '1');
	}

	StudioApp& m_app;
	IAllocator& m_allocator;
	ShaderEditor& m_editor;
	ShaderEditorResource m_resource;
	String m_source;
	ImGuiEx::Canvas m_canvas;
	bool m_source_open = false;
	bool m_show_save_as = false;
};

void ShaderEditor::registerDependencies(const ShaderEditorResource& res) {
	for (ShaderEditorResource::Node* n : res.m_nodes) {
		if (n->getType() == ShaderNodeType::FUNCTION_CALL) {
			FunctionCallNode* fn = (FunctionCallNode*)n;
			m_app.getAssetCompiler().registerDependency(res.m_path, fn->m_resource.m_path);
		}
	}
}

void ShaderEditor::open(const Path& path) {
	IAllocator& allocator = m_app.getAllocator();
	UniquePtr<ShaderEditorWindow> win = UniquePtr<ShaderEditorWindow>::create(allocator, path, *this, m_app, m_app.getAllocator());
	m_app.getAssetBrowser().addWindow(win.move());
}

ShaderEditorWindow::INodeTypeVisitor& ShaderEditorWindow::INodeTypeVisitor::visitType(const char* label, ShaderNodeType type, char shortcut ) {
	struct : ICreator {
		void create(ShaderEditorWindow& editor, ImVec2 pos) override {
			editor.addNode(type, pos);
		};
		ShaderNodeType type;
	} creator;
	creator.type = type;
	return visitType(label, creator, shortcut);
}

LUMIX_STUDIO_ENTRY(shader_editor) {
	PROFILE_FUNCTION();
	return LUMIX_NEW(app.getAllocator(), ShaderEditor)(app);
}

}

} // namespace Lumix