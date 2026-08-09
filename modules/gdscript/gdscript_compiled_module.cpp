/**************************************************************************/
/*  gdscript_compiled_module.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             Godot Engine                               */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdscript_compiled_module.h"

#include "gdscript.h"
#include "gdscript_cache.h"
#include "gdscript_optimization_profile.h"
#include "gdscript_utility_functions.h"

#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
#include "gdscript_baseline_jit.h"
#endif

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/rb_set.h"
#include "core/variant/type_info.h"
#include "core/version.h"

#include <utility>

namespace GDScriptCompiledModuleImplementation {

constexpr uint32_t MODULE_MAGIC = 0x4d534447; // "GDSM", little endian.
constexpr uint32_t MANIFEST_MAGIC = 0x4d504447; // "GDPM", little endian.
constexpr uint32_t MAX_COLLECTION_SIZE = 1 << 20;
constexpr uint32_t MAX_BLOB_SIZE = 256 << 20;
constexpr uint32_t MAX_STRING_SIZE = 1 << 20;
constexpr uint32_t MAX_MODULE_CLASSES = 1 << 16;
constexpr uint32_t MAX_MODULE_FUNCTIONS = 1 << 18;
constexpr uint32_t MAX_FUNCTION_CODE_WORDS = 1 << 20;
constexpr uint32_t MAX_TOTAL_CODE_WORDS = 1 << 24;
constexpr uint32_t MAX_FRAME_SLOTS = 1 << 20;
constexpr uint32_t MAX_FUNCTION_ARGUMENTS = 1 << 12;
constexpr uint32_t MAX_INSTRUCTION_ARGUMENTS = 1 << 16;
constexpr uint32_t MAX_FEEDBACK_SLOTS = 1 << 20;
constexpr uint32_t MAX_STRUCT_FIELDS = 1 << 16;
constexpr uint32_t MAX_GRAPH_DEPTH = 1 << 10;
constexpr uint64_t MAX_TOTAL_METADATA_ENTRIES = 1 << 24;
constexpr uint64_t MAX_TOTAL_CONSTANT_BYTES = 128 << 20;

enum ConstantKind : uint32_t {
	CONSTANT_VARIANT,
	CONSTANT_RESOURCE,
	CONSTANT_GDSCRIPT,
	CONSTANT_NATIVE_CLASS,
};

enum ClassFlags : uint32_t {
	CLASS_FLAG_TOOL = 1 << 0,
	CLASS_FLAG_ABSTRACT = 1 << 1,
	CLASS_FLAG_STATIC_UNLOAD = 1 << 2,
	CLASS_FLAG_MASK = CLASS_FLAG_TOOL | CLASS_FLAG_ABSTRACT | CLASS_FLAG_STATIC_UNLOAD,
};

enum RelocationTable : uint32_t {
	RELOC_OPERATOR,
	RELOC_SETTER,
	RELOC_GETTER,
	RELOC_KEYED_SETTER,
	RELOC_KEYED_GETTER,
	RELOC_INDEXED_SETTER,
	RELOC_INDEXED_GETTER,
	RELOC_BUILTIN_METHOD,
	RELOC_CONSTRUCTOR,
	RELOC_UTILITY,
	RELOC_GDSCRIPT_UTILITY,
	RELOC_METHOD_BIND,
	RELOC_LAMBDA,
	RELOC_TABLE_MAX,
};

static_assert(int(GDScriptFunction::RELOCATION_OPERATOR) == int(RELOC_OPERATOR));
static_assert(int(GDScriptFunction::RELOCATION_SETTER) == int(RELOC_SETTER));
static_assert(int(GDScriptFunction::RELOCATION_GETTER) == int(RELOC_GETTER));
static_assert(int(GDScriptFunction::RELOCATION_KEYED_SETTER) == int(RELOC_KEYED_SETTER));
static_assert(int(GDScriptFunction::RELOCATION_KEYED_GETTER) == int(RELOC_KEYED_GETTER));
static_assert(int(GDScriptFunction::RELOCATION_INDEXED_SETTER) == int(RELOC_INDEXED_SETTER));
static_assert(int(GDScriptFunction::RELOCATION_INDEXED_GETTER) == int(RELOC_INDEXED_GETTER));
static_assert(int(GDScriptFunction::RELOCATION_BUILTIN_METHOD) == int(RELOC_BUILTIN_METHOD));
static_assert(int(GDScriptFunction::RELOCATION_CONSTRUCTOR) == int(RELOC_CONSTRUCTOR));
static_assert(int(GDScriptFunction::RELOCATION_UTILITY) == int(RELOC_UTILITY));
static_assert(int(GDScriptFunction::RELOCATION_GDSCRIPT_UTILITY) == int(RELOC_GDSCRIPT_UTILITY));
static_assert(int(GDScriptFunction::RELOCATION_METHOD_BIND) == int(RELOC_METHOD_BIND));
static_assert(int(GDScriptFunction::RELOCATION_FUNCTION) == int(RELOC_LAMBDA));

struct Writer {
	Vector<uint8_t> data;

	void u32(uint32_t p_value) {
		const int old_size = data.size();
		data.resize(old_size + 4);
		uint8_t *w = data.ptrw() + old_size;
		w[0] = uint8_t(p_value);
		w[1] = uint8_t(p_value >> 8);
		w[2] = uint8_t(p_value >> 16);
		w[3] = uint8_t(p_value >> 24);
	}

	void u64(uint64_t p_value) {
		u32(uint32_t(p_value));
		u32(uint32_t(p_value >> 32));
	}

	void bytes(const uint8_t *p_data, uint32_t p_size) {
		u32(p_size);
		if (p_size == 0) {
			return;
		}
		const int old_size = data.size();
		data.resize(old_size + p_size);
		memcpy(data.ptrw() + old_size, p_data, p_size);
	}

	void bytes(const Vector<uint8_t> &p_data) {
		bytes(p_data.ptr(), p_data.size());
	}

	void string(const String &p_string) {
		const CharString utf8 = p_string.utf8();
		bytes(reinterpret_cast<const uint8_t *>(utf8.get_data()), utf8.length());
	}
};

struct Reader {
	const uint8_t *data = nullptr;
	uint64_t size = 0;
	uint64_t offset = 0;
	bool failed = false;

	Reader() = default;
	Reader(const uint8_t *p_data, uint64_t p_size) :
			data(p_data), size(p_size) {}

	uint32_t u32() {
		if (failed || offset + 4 > size) {
			failed = true;
			return 0;
		}
		const uint8_t *r = data + offset;
		offset += 4;
		return uint32_t(r[0]) | (uint32_t(r[1]) << 8) | (uint32_t(r[2]) << 16) | (uint32_t(r[3]) << 24);
	}

	uint64_t u64() {
		const uint64_t low = u32();
		const uint64_t high = u32();
		return low | (high << 32);
	}

	Vector<uint8_t> bytes(uint32_t p_limit = MAX_BLOB_SIZE) {
		Vector<uint8_t> result;
		const uint32_t length = u32();
		if (failed || length > p_limit || offset + length > size) {
			failed = true;
			return result;
		}
		result.resize(length);
		if (length > 0) {
			memcpy(result.ptrw(), data + offset, length);
		}
		offset += length;
		return result;
	}

	String string() {
		const Vector<uint8_t> value = bytes(MAX_STRING_SIZE);
		if (failed || value.is_empty()) {
			return String();
		}
		String result;
		if (result.append_utf8(reinterpret_cast<const char *>(value.ptr()), value.size()) != OK) {
			failed = true;
			return String();
		}
		return result;
	}

	uint32_t count(uint32_t p_limit = MAX_COLLECTION_SIZE) {
		const uint32_t value = u32();
		if (value > p_limit) {
			failed = true;
			return 0;
		}
		return value;
	}
};

struct ConstantData {
	ConstantKind kind = CONSTANT_VARIANT;
	Vector<uint8_t> encoded;
	String name;
	String owner;
};

struct DataTypeRecord {
	uint32_t kind = GDScriptDataType::VARIANT;
	uint32_t builtin_type = Variant::NIL;
	String native_type;
	String script_path;
	String script_class;
	Vector<uint8_t> struct_layout;
	Vector<DataTypeRecord> container_element_types;
};

struct PropertyRecord {
	uint32_t type = Variant::NIL;
	String name;
	String class_name;
	uint32_t hint = PROPERTY_HINT_NONE;
	String hint_string;
	uint32_t usage = PROPERTY_USAGE_DEFAULT;
};

struct MethodRecord {
	String name;
	PropertyRecord return_value;
	uint32_t flags = METHOD_FLAGS_DEFAULT;
	int32_t id = 0;
	Vector<PropertyRecord> arguments;
	Vector<ConstantData> default_arguments;
	int32_t return_value_metadata = 0;
	Vector<int> argument_metadata;
};

struct NamedConstantRecord {
	String name;
	ConstantData value;
};

struct MemberRecord {
	String name;
	int32_t index = 0;
	String setter;
	String getter;
	bool own_member = false;
	bool has_default_value = false;
	ConstantData default_value;
	DataTypeRecord data_type;
	PropertyRecord property;
};

struct BindingRecord {
	String name;
	String identity;
};

struct MethodBindingRecord {
	String name;
	String identity;
	bool is_static = false;
	int32_t default_argument_count = 0;
	Vector<DataTypeRecord> argument_types;
	DataTypeRecord return_type;
	MethodRecord method;
	ConstantData rpc_config;
};

struct StructFieldRecord {
	String name;
	uint32_t type = Variant::NIL;
	String nested_layout;
	ConstantData default_value;
};

struct StructLayoutRecord {
	String name;
	String type_identifier;
	uint32_t schema_version = 0;
	uint64_t schema_fingerprint = 0;
	Vector<StructFieldRecord> fields;
};

struct SignalRecord {
	String name;
	MethodRecord method;
};

struct LambdaRecord {
	String identity;
	int32_t capture_count = 0;
	bool use_self = false;
};

struct ClassRecord {
	String identity;
	String owner_identity;
	String script_path;
	String local_name;
	String global_name;
	String fully_qualified_name;
	String native_base;
	String script_base_path;
	String script_base_class;
	String icon_path;
	uint32_t flags = 0;
	Vector<MemberRecord> members;
	Vector<MemberRecord> static_members;
	Vector<StructLayoutRecord> struct_layouts;
	Vector<NamedConstantRecord> constants;
	Vector<MethodBindingRecord> methods;
	Vector<BindingRecord> subclasses;
	Vector<SignalRecord> signals;
	Vector<LambdaRecord> lambdas;
	ConstantData rpc_config;
	String initializer;
	String implicit_initializer;
	String implicit_ready;
	String static_initializer;
};

struct Symbol {
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint32_t hash = 0;
	String name;
	String owner;
};

struct FunctionRecord {
	String identity;
	String name;
	String source;
	uint32_t fingerprint = 0;
	int32_t initial_line = 0;
	int32_t argument_count = 0;
	int32_t vararg_index = -1;
	int32_t stack_size = 0;
	int32_t instruction_args_size = 0;
	int32_t operator_feedback_count = 0;
	int32_t call_feedback_count = 0;
	int32_t default_argument_count = 0;
	bool is_static = false;
	bool profile_guided = false;
	Vector<DataTypeRecord> argument_types;
	DataTypeRecord return_type;
	MethodRecord method;
	ConstantData rpc_config;
	Vector<NamedConstantRecord> local_constants;
	Vector<int> code;
	Vector<int> default_arguments;
	Vector<ConstantData> constants;
	Vector<StringName> global_names;
	Vector<Pair<int, Variant::Type>> temporary_slots;
	Vector<Vector<Symbol>> relocations;
};

struct ParsedModule {
	String path;
	uint64_t source_fingerprint = 0;
	uint64_t engine_api_fingerprint = 0;
	Vector<uint8_t> fallback_tokens;
	Vector<GDScriptCompiledModule::Dependency> dependencies;
	Vector<ClassRecord> classes;
	Vector<FunctionRecord> functions;
};

class Internals {
public:
	static bool make_relocations(const GDScriptFunction *p_function, const HashMap<GDScriptFunction *, String> &p_identities, Vector<Vector<Symbol>> &r_tables);
	static void add_function_tree(GDScriptFunction *p_function, const String &p_identity, HashMap<GDScriptFunction *, String> &r_identities, Vector<GDScriptFunction *> &r_functions);
	static void collect_functions(GDScript *p_script, const String &p_class_identity, HashMap<GDScriptFunction *, String> &r_identities, Vector<GDScriptFunction *> &r_functions);
	static void collect_classes(GDScript *p_script, const String &p_identity, HashMap<GDScript *, String> &r_identities, Vector<GDScript *> &r_classes);
	static uint64_t dependency_source_fingerprint(GDScript *p_script);
	static void add_script_dependency(GDScript *p_root, GDScript *p_dependency, HashMap<String, uint64_t> &r_dependencies);
	static void collect_dependencies_from_function(GDScript *p_root, GDScriptFunction *p_function, HashMap<String, uint64_t> &r_dependencies, HashSet<GDScriptFunction *> &r_visited);
	static void collect_dependencies_from_script(GDScript *p_root, GDScript *p_script, HashMap<String, uint64_t> &r_dependencies);
	static bool make_function_metadata(GDScriptFunction *p_function, FunctionRecord &r_record);
	static bool make_symbolic_global_code(const GDScriptFunction *p_function, Vector<int> &r_code, Vector<StringName> &r_names);
	static bool resolve_symbolic_global_code(const FunctionRecord &p_record, Vector<int> &r_code, String *r_error = nullptr);
	static bool function_metadata_matches(const FunctionRecord &p_record, GDScriptFunction *p_function);
	static bool make_record(GDScriptFunction *p_function, const String &p_identity, const HashMap<GDScriptFunction *, String> &p_identities, FunctionRecord &r_record);
	static bool make_class_record(GDScript *p_script, const String &p_identity, const HashMap<GDScript *, String> &p_class_identities,
			const HashMap<GDScriptFunction *, String> &p_function_identities, ClassRecord &r_record);
	static bool validate_class_record(const ClassRecord &p_record, GDScript *p_script, const HashMap<GDScript *, String> &p_class_identities,
			const HashMap<GDScriptFunction *, String> &p_function_identities);
	static bool validate_relocations(const FunctionRecord &p_record, GDScriptFunction *p_function, const HashMap<String, GDScriptFunction *> &p_functions);
	static bool validate_record(const FunctionRecord &p_record, GDScriptFunction *p_function, const HashMap<String, GDScriptFunction *> &p_functions,
			String *r_error = nullptr);
	static void install_record(const FunctionRecord &p_record, GDScriptFunction *p_function, const HashMap<String, GDScriptFunction *> &p_functions);
};

class ModuleVerifier {
	struct FunctionInfo {
		const FunctionRecord *record = nullptr;
		Vector<uint8_t> boundaries;
		Vector<int> instructions;
		Vector<uint8_t> reachable;
		Vector<Vector<uint8_t>> relocation_used;
	};

	const ParsedModule &module;
	String error;
	HashMap<String, const ClassRecord *> classes;
	HashMap<String, const FunctionRecord *> functions;
	HashMap<String, const StructLayoutRecord *> layout_records;
	HashMap<String, Ref<StructLayout>> verified_layouts;
	HashSet<String> layouts_being_verified;
	HashMap<String, String> function_owners;
	HashSet<String> bound_methods;
	HashMap<String, int> class_member_counts;
	Vector<FunctionInfo> function_info;
	uint64_t metadata_entries = 0;
	uint64_t constant_bytes = 0;
	uint64_t total_code_words = 0;

	Error fail(const String &p_message);
	Error add_metadata_entries(uint64_t p_count, const String &p_context);
	Error verify_constant(const ConstantData &p_constant, Variant *r_decoded = nullptr);
	Error verify_data_type(const DataTypeRecord &p_type, int p_depth = 0);
	Error verify_property(const PropertyRecord &p_property);
	Error verify_method(const MethodRecord &p_method);
	Error verify_layout(const String &p_identifier, int p_depth = 0);
	Error index_and_verify_metadata();
	Error verify_function_graph();
	Error decode_instructions();
	Error validate_operands_and_tables();
	Error validate_frames_and_temporaries();
	Error validate_control_flow();
	Error validate_typed_constraints();
	Error validate_operations();
	Error validate_relocations();
	Error validate_final_limits();

public:
	explicit ModuleVerifier(const ParsedModule &p_module) :
			module(p_module) {}
	Error verify(String *r_error = nullptr);
};

class RuntimeBuilder {
	struct ShellSnapshot {
		GDScript *script = nullptr;
		GDScript *owner = nullptr;
		String path;
		StringName local_name;
		StringName global_name;
		String fully_qualified_name;
		String icon_path;
		HashMap<StringName, Ref<GDScript>> subclasses;
	};

	struct StagedClass {
		const ClassRecord *record = nullptr;
		GDScript *script = nullptr;
		Ref<GDScriptNativeClass> native;
		Ref<GDScript> base;
		HashMap<StringName, GDScript::MemberInfo> member_indices;
		HashSet<StringName> members;
		HashMap<StringName, GDScript::MemberInfo> static_variables_indices;
		Vector<Variant> static_variables;
		HashMap<StringName, Variant> member_default_values;
		HashMap<StringName, Ref<StructLayout>> struct_layouts;
		HashMap<StringName, Variant> constants;
		HashMap<StringName, GDScriptFunction *> member_functions;
		HashMap<StringName, Ref<GDScript>> subclasses;
		HashMap<StringName, MethodInfo> signals;
		Dictionary rpc_config;
		HashMap<GDScriptFunction *, GDScript::LambdaInfo> lambda_info;
		GDScriptFunction *initializer = nullptr;
		GDScriptFunction *implicit_initializer = nullptr;
		GDScriptFunction *implicit_ready = nullptr;
		GDScriptFunction *static_initializer = nullptr;
	};

	struct ResolvedFunction {
		Vector<GDScriptDataType> argument_types;
		GDScriptDataType return_type;
		MethodInfo method;
		Variant rpc_config;
		HashMap<StringName, Variant> local_constants;
		Vector<Variant> constants;
		Vector<int> code;
	};

	GDScript *root = nullptr;
	ParsedModule module;
	HashMap<String, GDScript *> classes;
	HashMap<String, const ClassRecord *> class_records;
	HashMap<String, int> staged_class_indices;
	Vector<StagedClass> staged_classes;
	HashMap<String, GDScriptFunction *> functions;
	HashMap<String, const FunctionRecord *> function_records;
	Vector<ResolvedFunction> resolved_functions;
	HashSet<String> root_function_identities;
	HashSet<String> lambda_function_identities;
	HashMap<String, const StructLayoutRecord *> layout_records;
	HashMap<String, Ref<StructLayout>> layouts_by_identifier;
	HashSet<String> layouts_being_built;
	Vector<ShellSnapshot> shell_snapshots;
	String error;

	Error fail(Error p_error, const String &p_message);
	GDScript *find_script(const String &p_path, const String &p_class, Error &r_error);
	Error decode_constant(const ConstantData &p_constant, Variant &r_value);
	Error decode_property(const PropertyRecord &p_record, PropertyInfo &r_property);
	Error decode_method(const MethodRecord &p_record, MethodInfo &r_method);
	Error decode_data_type(const DataTypeRecord &p_record, GDScriptDataType &r_type, int p_depth = 0);
	Error build_layout(const String &p_identifier);
	Error stage_classes();
	Error validate_function_code(const FunctionRecord &p_record);
	Error resolve_function_symbols();
	Error stage_functions();
	Error resolve_function_relocations(const FunctionRecord &p_record, GDScriptFunction *p_function);
	Error link_functions();
	Error verify_inheritance();
	void capture_shells(GDScript *p_script, HashSet<GDScript *> &r_visited);
	void restore_shells();
	void discard_functions();
	void commit(bool p_keep_state);

public:
	static Error prepare_shell_graph(GDScript *p_script, const ParsedModule &p_module, HashMap<String, GDScript *> *r_classes, String *r_error);
	Error build(GDScript *p_script, const ParsedModule &p_module, bool p_keep_state, String *r_error);
};

void write_int_vector(Writer &p_writer, const Vector<int> &p_values) {
	p_writer.u32(p_values.size());
	for (int value : p_values) {
		p_writer.u32(uint32_t(value));
	}
}

Vector<int> read_int_vector(Reader &p_reader, uint32_t p_limit = MAX_COLLECTION_SIZE) {
	Vector<int> values;
	const uint32_t count = p_reader.count(p_limit);
	if (p_reader.failed) {
		return values;
	}
	values.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		values.write[i] = int32_t(p_reader.u32());
	}
	return values;
}

void write_constant(Writer &p_writer, const ConstantData &p_constant) {
	p_writer.u32(p_constant.kind);
	p_writer.bytes(p_constant.encoded);
	p_writer.string(p_constant.name);
	p_writer.string(p_constant.owner);
}

ConstantData read_constant(Reader &p_reader) {
	ConstantData constant;
	constant.kind = ConstantKind(p_reader.u32());
	if (constant.kind > CONSTANT_NATIVE_CLASS) {
		p_reader.failed = true;
	}
	constant.encoded = p_reader.bytes();
	constant.name = p_reader.string();
	constant.owner = p_reader.string();
	return constant;
}

void write_data_type(Writer &p_writer, const DataTypeRecord &p_type) {
	p_writer.u32(p_type.kind);
	p_writer.u32(p_type.builtin_type);
	p_writer.string(p_type.native_type);
	p_writer.string(p_type.script_path);
	p_writer.string(p_type.script_class);
	p_writer.bytes(p_type.struct_layout);
	p_writer.u32(p_type.container_element_types.size());
	for (const DataTypeRecord &element_type : p_type.container_element_types) {
		write_data_type(p_writer, element_type);
	}
}

DataTypeRecord read_data_type(Reader &p_reader, int p_depth = 0) {
	DataTypeRecord type;
	if (p_depth > Variant::MAX_RECURSION_DEPTH) {
		p_reader.failed = true;
		return type;
	}
	type.kind = p_reader.u32();
	type.builtin_type = p_reader.u32();
	if (type.kind > GDScriptDataType::GDSCRIPT || type.builtin_type >= Variant::VARIANT_MAX) {
		p_reader.failed = true;
	}
	type.native_type = p_reader.string();
	type.script_path = p_reader.string();
	type.script_class = p_reader.string();
	type.struct_layout = p_reader.bytes();
	const uint32_t element_count = p_reader.count();
	type.container_element_types.resize(element_count);
	for (uint32_t i = 0; i < element_count; i++) {
		type.container_element_types.write[i] = read_data_type(p_reader, p_depth + 1);
	}
	return type;
}

void write_property(Writer &p_writer, const PropertyRecord &p_property) {
	p_writer.u32(p_property.type);
	p_writer.string(p_property.name);
	p_writer.string(p_property.class_name);
	p_writer.u32(p_property.hint);
	p_writer.string(p_property.hint_string);
	p_writer.u32(p_property.usage);
}

PropertyRecord read_property(Reader &p_reader) {
	PropertyRecord property;
	property.type = p_reader.u32();
	property.name = p_reader.string();
	property.class_name = p_reader.string();
	property.hint = p_reader.u32();
	property.hint_string = p_reader.string();
	property.usage = p_reader.u32();
	if (property.type >= Variant::VARIANT_MAX || property.hint >= PROPERTY_HINT_MAX) {
		p_reader.failed = true;
	}
	return property;
}

void write_method(Writer &p_writer, const MethodRecord &p_method) {
	p_writer.string(p_method.name);
	write_property(p_writer, p_method.return_value);
	p_writer.u32(p_method.flags);
	p_writer.u32(uint32_t(p_method.id));
	p_writer.u32(p_method.arguments.size());
	for (const PropertyRecord &argument : p_method.arguments) {
		write_property(p_writer, argument);
	}
	p_writer.u32(p_method.default_arguments.size());
	for (const ConstantData &argument : p_method.default_arguments) {
		write_constant(p_writer, argument);
	}
	p_writer.u32(uint32_t(p_method.return_value_metadata));
	write_int_vector(p_writer, p_method.argument_metadata);
}

MethodRecord read_method(Reader &p_reader) {
	MethodRecord method;
	method.name = p_reader.string();
	method.return_value = read_property(p_reader);
	method.flags = p_reader.u32();
	method.id = int32_t(p_reader.u32());
	const uint32_t argument_count = p_reader.count();
	method.arguments.resize(argument_count);
	for (uint32_t i = 0; i < argument_count; i++) {
		method.arguments.write[i] = read_property(p_reader);
	}
	const uint32_t default_count = p_reader.count();
	method.default_arguments.resize(default_count);
	for (uint32_t i = 0; i < default_count; i++) {
		method.default_arguments.write[i] = read_constant(p_reader);
	}
	method.return_value_metadata = int32_t(p_reader.u32());
	method.argument_metadata = read_int_vector(p_reader);
	return method;
}

void write_named_constant(Writer &p_writer, const NamedConstantRecord &p_constant) {
	p_writer.string(p_constant.name);
	write_constant(p_writer, p_constant.value);
}

NamedConstantRecord read_named_constant(Reader &p_reader) {
	NamedConstantRecord constant;
	constant.name = p_reader.string();
	constant.value = read_constant(p_reader);
	return constant;
}

void write_member(Writer &p_writer, const MemberRecord &p_member) {
	p_writer.string(p_member.name);
	p_writer.u32(uint32_t(p_member.index));
	p_writer.string(p_member.setter);
	p_writer.string(p_member.getter);
	p_writer.u32(p_member.own_member ? 1 : 0);
	p_writer.u32(p_member.has_default_value ? 1 : 0);
	if (p_member.has_default_value) {
		write_constant(p_writer, p_member.default_value);
	}
	write_data_type(p_writer, p_member.data_type);
	write_property(p_writer, p_member.property);
}

MemberRecord read_member(Reader &p_reader) {
	MemberRecord member;
	member.name = p_reader.string();
	member.index = int32_t(p_reader.u32());
	member.setter = p_reader.string();
	member.getter = p_reader.string();
	const uint32_t own_member = p_reader.u32();
	const uint32_t has_default_value = p_reader.u32();
	if (member.index < 0 || own_member > 1 || has_default_value > 1) {
		p_reader.failed = true;
	}
	member.own_member = own_member != 0;
	member.has_default_value = has_default_value != 0;
	if (member.has_default_value) {
		member.default_value = read_constant(p_reader);
	}
	member.data_type = read_data_type(p_reader);
	member.property = read_property(p_reader);
	return member;
}

void write_binding(Writer &p_writer, const BindingRecord &p_binding) {
	p_writer.string(p_binding.name);
	p_writer.string(p_binding.identity);
}

BindingRecord read_binding(Reader &p_reader) {
	BindingRecord binding;
	binding.name = p_reader.string();
	binding.identity = p_reader.string();
	return binding;
}

void write_method_binding(Writer &p_writer, const MethodBindingRecord &p_method) {
	p_writer.string(p_method.name);
	p_writer.string(p_method.identity);
	p_writer.u32(p_method.is_static ? 1 : 0);
	p_writer.u32(uint32_t(p_method.default_argument_count));
	p_writer.u32(p_method.argument_types.size());
	for (const DataTypeRecord &argument_type : p_method.argument_types) {
		write_data_type(p_writer, argument_type);
	}
	write_data_type(p_writer, p_method.return_type);
	write_method(p_writer, p_method.method);
	write_constant(p_writer, p_method.rpc_config);
}

MethodBindingRecord read_method_binding(Reader &p_reader) {
	MethodBindingRecord method;
	method.name = p_reader.string();
	method.identity = p_reader.string();
	const uint32_t is_static = p_reader.u32();
	method.default_argument_count = int32_t(p_reader.u32());
	if (is_static > 1 || method.default_argument_count < 0) {
		p_reader.failed = true;
	}
	method.is_static = is_static != 0;
	const uint32_t argument_count = p_reader.count();
	method.argument_types.resize(argument_count);
	for (uint32_t i = 0; i < argument_count; i++) {
		method.argument_types.write[i] = read_data_type(p_reader);
	}
	method.return_type = read_data_type(p_reader);
	method.method = read_method(p_reader);
	method.rpc_config = read_constant(p_reader);
	return method;
}

void write_struct_layout(Writer &p_writer, const StructLayoutRecord &p_layout) {
	p_writer.string(p_layout.name);
	p_writer.string(p_layout.type_identifier);
	p_writer.u32(p_layout.schema_version);
	p_writer.u64(p_layout.schema_fingerprint);
	p_writer.u32(p_layout.fields.size());
	for (const StructFieldRecord &field : p_layout.fields) {
		p_writer.string(field.name);
		p_writer.u32(field.type);
		p_writer.string(field.nested_layout);
		write_constant(p_writer, field.default_value);
	}
}

StructLayoutRecord read_struct_layout(Reader &p_reader) {
	StructLayoutRecord layout;
	layout.name = p_reader.string();
	layout.type_identifier = p_reader.string();
	layout.schema_version = p_reader.u32();
	layout.schema_fingerprint = p_reader.u64();
	const uint32_t field_count = p_reader.count();
	layout.fields.resize(field_count);
	for (uint32_t i = 0; i < field_count; i++) {
		StructFieldRecord &field = layout.fields.write[i];
		field.name = p_reader.string();
		field.type = p_reader.u32();
		field.nested_layout = p_reader.string();
		field.default_value = read_constant(p_reader);
		if (field.type >= Variant::VARIANT_MAX || (field.type == Variant::STRUCT) != !field.nested_layout.is_empty()) {
			p_reader.failed = true;
		}
	}
	return layout;
}

void write_class(Writer &p_writer, const ClassRecord &p_class) {
	p_writer.string(p_class.identity);
	p_writer.string(p_class.owner_identity);
	p_writer.string(p_class.script_path);
	p_writer.string(p_class.local_name);
	p_writer.string(p_class.global_name);
	p_writer.string(p_class.fully_qualified_name);
	p_writer.string(p_class.native_base);
	p_writer.string(p_class.script_base_path);
	p_writer.string(p_class.script_base_class);
	p_writer.string(p_class.icon_path);
	p_writer.u32(p_class.flags);

	p_writer.u32(p_class.members.size());
	for (const MemberRecord &member : p_class.members) {
		write_member(p_writer, member);
	}
	p_writer.u32(p_class.static_members.size());
	for (const MemberRecord &member : p_class.static_members) {
		write_member(p_writer, member);
	}
	p_writer.u32(p_class.struct_layouts.size());
	for (const StructLayoutRecord &layout : p_class.struct_layouts) {
		write_struct_layout(p_writer, layout);
	}
	p_writer.u32(p_class.constants.size());
	for (const NamedConstantRecord &constant : p_class.constants) {
		write_named_constant(p_writer, constant);
	}
	p_writer.u32(p_class.methods.size());
	for (const MethodBindingRecord &method : p_class.methods) {
		write_method_binding(p_writer, method);
	}
	p_writer.u32(p_class.subclasses.size());
	for (const BindingRecord &subclass : p_class.subclasses) {
		write_binding(p_writer, subclass);
	}
	p_writer.u32(p_class.signals.size());
	for (const SignalRecord &signal : p_class.signals) {
		p_writer.string(signal.name);
		write_method(p_writer, signal.method);
	}
	p_writer.u32(p_class.lambdas.size());
	for (const LambdaRecord &lambda : p_class.lambdas) {
		p_writer.string(lambda.identity);
		p_writer.u32(uint32_t(lambda.capture_count));
		p_writer.u32(lambda.use_self ? 1 : 0);
	}
	write_constant(p_writer, p_class.rpc_config);
	p_writer.string(p_class.initializer);
	p_writer.string(p_class.implicit_initializer);
	p_writer.string(p_class.implicit_ready);
	p_writer.string(p_class.static_initializer);
}

ClassRecord read_class(Reader &p_reader) {
	ClassRecord script_class;
	script_class.identity = p_reader.string();
	script_class.owner_identity = p_reader.string();
	script_class.script_path = p_reader.string();
	script_class.local_name = p_reader.string();
	script_class.global_name = p_reader.string();
	script_class.fully_qualified_name = p_reader.string();
	script_class.native_base = p_reader.string();
	script_class.script_base_path = p_reader.string();
	script_class.script_base_class = p_reader.string();
	script_class.icon_path = p_reader.string();
	script_class.flags = p_reader.u32();
	if ((script_class.flags & ~CLASS_FLAG_MASK) != 0) {
		p_reader.failed = true;
	}

	uint32_t count = p_reader.count();
	script_class.members.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		script_class.members.write[i] = read_member(p_reader);
	}
	count = p_reader.count();
	script_class.static_members.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		script_class.static_members.write[i] = read_member(p_reader);
	}
	count = p_reader.count();
	script_class.struct_layouts.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		script_class.struct_layouts.write[i] = read_struct_layout(p_reader);
	}
	count = p_reader.count();
	script_class.constants.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		script_class.constants.write[i] = read_named_constant(p_reader);
	}
	count = p_reader.count();
	script_class.methods.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		script_class.methods.write[i] = read_method_binding(p_reader);
	}
	count = p_reader.count();
	script_class.subclasses.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		script_class.subclasses.write[i] = read_binding(p_reader);
	}
	count = p_reader.count();
	script_class.signals.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		script_class.signals.write[i].name = p_reader.string();
		script_class.signals.write[i].method = read_method(p_reader);
	}
	count = p_reader.count();
	script_class.lambdas.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		LambdaRecord &lambda = script_class.lambdas.write[i];
		lambda.identity = p_reader.string();
		lambda.capture_count = int32_t(p_reader.u32());
		const uint32_t use_self = p_reader.u32();
		if (lambda.capture_count < 0 || use_self > 1) {
			p_reader.failed = true;
		}
		lambda.use_self = use_self != 0;
	}
	script_class.rpc_config = read_constant(p_reader);
	script_class.initializer = p_reader.string();
	script_class.implicit_initializer = p_reader.string();
	script_class.implicit_ready = p_reader.string();
	script_class.static_initializer = p_reader.string();
	return script_class;
}

void write_symbol(Writer &p_writer, const Symbol &p_symbol) {
	p_writer.u32(uint32_t(p_symbol.x));
	p_writer.u32(uint32_t(p_symbol.y));
	p_writer.u32(uint32_t(p_symbol.z));
	p_writer.u32(p_symbol.hash);
	p_writer.string(p_symbol.name);
	p_writer.string(p_symbol.owner);
}

Symbol read_symbol(Reader &p_reader) {
	Symbol symbol;
	symbol.x = int32_t(p_reader.u32());
	symbol.y = int32_t(p_reader.u32());
	symbol.z = int32_t(p_reader.u32());
	symbol.hash = p_reader.u32();
	symbol.name = p_reader.string();
	symbol.owner = p_reader.string();
	return symbol;
}

void write_function(Writer &p_writer, const FunctionRecord &p_record) {
	p_writer.string(p_record.identity);
	p_writer.string(p_record.name);
	p_writer.string(p_record.source);
	p_writer.u32(p_record.fingerprint);
	p_writer.u32(uint32_t(p_record.initial_line));
	p_writer.u32(uint32_t(p_record.argument_count));
	p_writer.u32(uint32_t(p_record.vararg_index));
	p_writer.u32(uint32_t(p_record.stack_size));
	p_writer.u32(uint32_t(p_record.instruction_args_size));
	p_writer.u32(uint32_t(p_record.operator_feedback_count));
	p_writer.u32(uint32_t(p_record.call_feedback_count));
	p_writer.u32(uint32_t(p_record.default_argument_count));
	p_writer.u32(p_record.is_static ? 1 : 0);
	p_writer.u32(p_record.profile_guided ? 1 : 0);
	p_writer.u32(p_record.argument_types.size());
	for (const DataTypeRecord &argument_type : p_record.argument_types) {
		write_data_type(p_writer, argument_type);
	}
	write_data_type(p_writer, p_record.return_type);
	write_method(p_writer, p_record.method);
	write_constant(p_writer, p_record.rpc_config);
	p_writer.u32(p_record.local_constants.size());
	for (const NamedConstantRecord &constant : p_record.local_constants) {
		write_named_constant(p_writer, constant);
	}
	write_int_vector(p_writer, p_record.code);
	write_int_vector(p_writer, p_record.default_arguments);

	p_writer.u32(p_record.constants.size());
	for (const ConstantData &constant : p_record.constants) {
		write_constant(p_writer, constant);
	}

	p_writer.u32(p_record.global_names.size());
	for (const StringName &name : p_record.global_names) {
		p_writer.string(name);
	}

	p_writer.u32(p_record.temporary_slots.size());
	for (const Pair<int, Variant::Type> &slot : p_record.temporary_slots) {
		p_writer.u32(uint32_t(slot.first));
		p_writer.u32(uint32_t(slot.second));
	}

	p_writer.u32(RELOC_TABLE_MAX);
	for (const Vector<Symbol> &table : p_record.relocations) {
		p_writer.u32(table.size());
		for (const Symbol &symbol : table) {
			write_symbol(p_writer, symbol);
		}
	}
}

FunctionRecord read_function(Reader &p_reader) {
	FunctionRecord record;
	record.identity = p_reader.string();
	record.name = p_reader.string();
	record.source = p_reader.string();
	record.fingerprint = p_reader.u32();
	record.initial_line = int32_t(p_reader.u32());
	record.argument_count = int32_t(p_reader.u32());
	record.vararg_index = int32_t(p_reader.u32());
	record.stack_size = int32_t(p_reader.u32());
	record.instruction_args_size = int32_t(p_reader.u32());
	record.operator_feedback_count = int32_t(p_reader.u32());
	record.call_feedback_count = int32_t(p_reader.u32());
	record.default_argument_count = int32_t(p_reader.u32());
	const uint32_t is_static = p_reader.u32();
	const uint32_t profile_guided = p_reader.u32();
	if (record.argument_count < 0 || record.stack_size < 0 || record.instruction_args_size < 0 || record.operator_feedback_count < 0 ||
			record.call_feedback_count < 0 || record.default_argument_count < 0 || is_static > 1 || profile_guided > 1) {
		p_reader.failed = true;
	}
	record.is_static = is_static != 0;
	record.profile_guided = profile_guided != 0;
	const uint32_t argument_type_count = p_reader.count();
	record.argument_types.resize(argument_type_count);
	for (uint32_t i = 0; i < argument_type_count; i++) {
		record.argument_types.write[i] = read_data_type(p_reader);
	}
	record.return_type = read_data_type(p_reader);
	record.method = read_method(p_reader);
	record.rpc_config = read_constant(p_reader);
	const uint32_t local_constant_count = p_reader.count();
	record.local_constants.resize(local_constant_count);
	for (uint32_t i = 0; i < local_constant_count; i++) {
		record.local_constants.write[i] = read_named_constant(p_reader);
	}
	record.code = read_int_vector(p_reader, MAX_FUNCTION_CODE_WORDS);
	record.default_arguments = read_int_vector(p_reader);

	const uint32_t constant_count = p_reader.count();
	record.constants.resize(constant_count);
	for (uint32_t i = 0; i < constant_count; i++) {
		record.constants.write[i] = read_constant(p_reader);
	}

	const uint32_t name_count = p_reader.count();
	record.global_names.resize(name_count);
	for (uint32_t i = 0; i < name_count; i++) {
		record.global_names.write[i] = p_reader.string();
	}

	const uint32_t slot_count = p_reader.count();
	record.temporary_slots.resize(slot_count);
	for (uint32_t i = 0; i < slot_count; i++) {
		const int slot = int32_t(p_reader.u32());
		const uint32_t type = p_reader.u32();
		if (type >= Variant::VARIANT_MAX) {
			p_reader.failed = true;
		}
		record.temporary_slots.write[i] = Pair<int, Variant::Type>(slot, Variant::Type(type));
	}

	const uint32_t table_count = p_reader.count();
	if (table_count != RELOC_TABLE_MAX) {
		p_reader.failed = true;
		return record;
	}
	record.relocations.resize(RELOC_TABLE_MAX);
	for (uint32_t table = 0; table < table_count; table++) {
		const uint32_t symbol_count = p_reader.count();
		record.relocations.write[table].resize(symbol_count);
		for (uint32_t i = 0; i < symbol_count; i++) {
			record.relocations.write[table].write[i] = read_symbol(p_reader);
		}
	}
	return record;
}

Error parse_module(const Vector<uint8_t> &p_module, ParsedModule &r_module, String *r_error) {
	auto fail = [&](const String &p_message) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		return ERR_FILE_CORRUPT;
	};

	Reader header(p_module.ptr(), p_module.size());
	if (header.u32() != MODULE_MAGIC) {
		return fail("Not a compiled GDScript module.");
	}
	if (header.u32() != GDScriptCompiledModule::FORMAT_VERSION) {
		return fail("Unsupported compiled GDScript module format.");
	}
	if (header.u32() != GDScriptCompiledModule::BYTECODE_VERSION) {
		return fail("Unsupported GDScript bytecode version.");
	}
	r_module.engine_api_fingerprint = header.u64();
	r_module.source_fingerprint = header.u64();
	const uint64_t payload_fingerprint = header.u64();
	const uint32_t payload_size = header.u32();
	if (header.failed || payload_size > MAX_BLOB_SIZE || header.offset + payload_size != header.size) {
		return fail("Invalid compiled GDScript module payload size.");
	}
	const uint8_t *payload_data = p_module.ptr() + header.offset;
	if (GDScriptCompiledModule::fingerprint_bytes(payload_data, payload_size) != payload_fingerprint) {
		return fail("Compiled GDScript module checksum mismatch.");
	}

	Reader payload(payload_data, payload_size);
	r_module.path = payload.string();
	r_module.fallback_tokens = payload.bytes();
	const uint32_t dependency_count = payload.count(MAX_MODULE_CLASSES);
	r_module.dependencies.resize(dependency_count);
	for (uint32_t i = 0; i < dependency_count; i++) {
		r_module.dependencies.write[i].path = payload.string();
		r_module.dependencies.write[i].source_fingerprint = payload.u64();
	}
	const uint32_t class_count = payload.count(MAX_MODULE_CLASSES);
	r_module.classes.resize(class_count);
	for (uint32_t i = 0; i < class_count; i++) {
		r_module.classes.write[i] = read_class(payload);
	}
	const uint32_t function_count = payload.count(MAX_MODULE_FUNCTIONS);
	r_module.functions.resize(function_count);
	for (uint32_t i = 0; i < function_count; i++) {
		r_module.functions.write[i] = read_function(payload);
	}
	if (payload.failed || payload.offset != payload.size || r_module.fallback_tokens.is_empty() || r_module.classes.is_empty()) {
		return fail("Malformed compiled GDScript module payload.");
	}
	return OK;
}

bool is_pointer_free_variant(const Variant &p_value, int p_depth = 0) {
	if (p_depth > Variant::MAX_RECURSION_DEPTH) {
		return false;
	}
	switch (p_value.get_type()) {
		case Variant::OBJECT:
		case Variant::RID:
		case Variant::CALLABLE:
		case Variant::SIGNAL:
			return false;
		case Variant::ARRAY: {
			const Array array = p_value;
			if (array.get_typed_script().get_type() == Variant::OBJECT && array.get_typed_script().get_validated_object() != nullptr) {
				return false;
			}
			for (const Variant &value : array) {
				if (!is_pointer_free_variant(value, p_depth + 1)) {
					return false;
				}
			}
			return true;
		}
		case Variant::DICTIONARY: {
			const Dictionary dictionary = p_value;
			if ((dictionary.get_typed_key_script().get_type() == Variant::OBJECT && dictionary.get_typed_key_script().get_validated_object() != nullptr) ||
					(dictionary.get_typed_value_script().get_type() == Variant::OBJECT && dictionary.get_typed_value_script().get_validated_object() != nullptr)) {
				return false;
			}
			for (const Variant &key : dictionary.keys()) {
				if (!is_pointer_free_variant(key, p_depth + 1) || !is_pointer_free_variant(dictionary[key], p_depth + 1)) {
					return false;
				}
			}
			return true;
		}
		case Variant::STRUCT: {
			const StructValue value = p_value;
			return value.is_valid() && is_pointer_free_variant(value.to_dictionary(), p_depth + 1);
		}
		default:
			return true;
	}
}

String canonicalize_qualified_script_name(const String &p_name) {
	const int separator = p_name.find("::");
	if (separator < 0) {
		return GDScript::canonicalize_path(p_name);
	}
	return GDScript::canonicalize_path(p_name.left(separator)) + p_name.substr(separator);
}

bool encode_constant(const Variant &p_value, ConstantData &r_constant) {
	if (p_value.get_type() == Variant::OBJECT) {
		Object *object = p_value;
		if (GDScriptNativeClass *native_class = Object::cast_to<GDScriptNativeClass>(object)) {
			r_constant.kind = CONSTANT_NATIVE_CLASS;
			r_constant.name = native_class->get_name();
			return true;
		}
		if (GDScript *script = Object::cast_to<GDScript>(object)) {
			r_constant.kind = CONSTANT_GDSCRIPT;
			r_constant.name = GDScript::canonicalize_path(script->get_script_path());
			r_constant.owner = canonicalize_qualified_script_name(script->get_fully_qualified_name());
			return !r_constant.name.is_empty();
		}
		if (Resource *resource = Object::cast_to<Resource>(object)) {
			r_constant.kind = CONSTANT_RESOURCE;
			r_constant.name = resource->get_path();
			return !r_constant.name.is_empty();
		}
		return false;
	}
	if (!is_pointer_free_variant(p_value)) {
		return false;
	}

	int encoded_size = 0;
	if (encode_variant(p_value, nullptr, encoded_size, false) != OK || encoded_size < 0) {
		return false;
	}
	r_constant.kind = CONSTANT_VARIANT;
	r_constant.encoded.resize(encoded_size);
	if (encoded_size > 0 && encode_variant(p_value, r_constant.encoded.ptrw(), encoded_size, false) != OK) {
		return false;
	}
	return true;
}

bool constant_matches(const ConstantData &p_constant, const Variant &p_value) {
	switch (p_constant.kind) {
		case CONSTANT_VARIANT: {
			Variant decoded;
			int used = 0;
			return decode_variant(decoded, p_constant.encoded.ptr(), p_constant.encoded.size(), &used, false) == OK && used == p_constant.encoded.size() && decoded == p_value;
		}
		case CONSTANT_RESOURCE: {
			Resource *resource = Object::cast_to<Resource>(p_value.get_validated_object());
			return resource != nullptr && resource->get_path() == p_constant.name;
		}
		case CONSTANT_GDSCRIPT: {
			GDScript *script = Object::cast_to<GDScript>(p_value.get_validated_object());
			return script != nullptr && GDScript::canonicalize_path(script->get_script_path()) == p_constant.name &&
					canonicalize_qualified_script_name(script->get_fully_qualified_name()) == p_constant.owner;
		}
		case CONSTANT_NATIVE_CLASS: {
			GDScriptNativeClass *native_class = Object::cast_to<GDScriptNativeClass>(p_value.get_validated_object());
			return native_class != nullptr && native_class->get_name() == p_constant.name;
		}
	}
	return false;
}

bool make_data_type_record(const GDScriptDataType &p_type, DataTypeRecord &r_record, int p_depth = 0) {
	if (p_depth > Variant::MAX_RECURSION_DEPTH || p_type.kind < GDScriptDataType::VARIANT || p_type.kind > GDScriptDataType::GDSCRIPT ||
			p_type.builtin_type < Variant::NIL || p_type.builtin_type >= Variant::VARIANT_MAX) {
		return false;
	}
	r_record.kind = p_type.kind;
	r_record.builtin_type = p_type.builtin_type;
	r_record.native_type = p_type.native_type;

	Script *script_type = p_type.script_type;
	if (script_type == nullptr && p_type.script_type_ref.is_valid()) {
		script_type = p_type.script_type_ref.ptr();
	}
	if (script_type != nullptr) {
		if (GDScript *gdscript = Object::cast_to<GDScript>(script_type)) {
			r_record.script_path = GDScript::canonicalize_path(gdscript->get_script_path());
			r_record.script_class = canonicalize_qualified_script_name(gdscript->get_fully_qualified_name());
		} else {
			r_record.script_path = script_type->get_path();
			r_record.script_class = script_type->get_global_name();
		}
	}
	if ((p_type.kind == GDScriptDataType::SCRIPT || p_type.kind == GDScriptDataType::GDSCRIPT) &&
			(script_type == nullptr || r_record.script_path.is_empty())) {
		return false;
	}

	if (p_type.struct_layout.is_valid()) {
		if (p_type.builtin_type != Variant::STRUCT || !p_type.struct_layout->is_finalized()) {
			return false;
		}
		ConstantData layout;
		if (!encode_constant(p_type.struct_layout->to_dictionary(), layout) || layout.kind != CONSTANT_VARIANT) {
			return false;
		}
		r_record.struct_layout = layout.encoded;
	}

	for (const GDScriptDataType &element_type : p_type.container_element_types) {
		DataTypeRecord element_record;
		if (!make_data_type_record(element_type, element_record, p_depth + 1)) {
			return false;
		}
		r_record.container_element_types.push_back(element_record);
	}
	return true;
}

PropertyRecord make_property_record(const PropertyInfo &p_property) {
	PropertyRecord record;
	record.type = p_property.type;
	record.name = p_property.name;
	record.class_name = p_property.class_name;
	record.hint = p_property.hint;
	record.hint_string = p_property.hint_string;
	record.usage = p_property.usage;
	return record;
}

bool make_method_record(const MethodInfo &p_method, MethodRecord &r_record) {
	r_record.name = p_method.name;
	r_record.return_value = make_property_record(p_method.return_val);
	r_record.flags = p_method.flags;
	r_record.id = p_method.id;
	for (const PropertyInfo &argument : p_method.arguments) {
		r_record.arguments.push_back(make_property_record(argument));
	}
	for (const Variant &argument : p_method.default_arguments) {
		ConstantData encoded;
		if (!encode_constant(argument, encoded)) {
			return false;
		}
		r_record.default_arguments.push_back(encoded);
	}
	r_record.return_value_metadata = p_method.return_val_metadata;
	r_record.argument_metadata = p_method.arguments_metadata;
	return true;
}

bool Internals::make_function_metadata(GDScriptFunction *p_function, FunctionRecord &r_record) {
	r_record.name = p_function->name;
	r_record.source = GDScript::canonicalize_path(p_function->source);
	r_record.default_argument_count = p_function->_default_arg_count;
	for (const GDScriptDataType &argument_type : p_function->argument_types) {
		DataTypeRecord type;
		if (!make_data_type_record(argument_type, type)) {
			return false;
		}
		r_record.argument_types.push_back(type);
	}
	if (!make_data_type_record(p_function->return_type, r_record.return_type) || !make_method_record(p_function->method_info, r_record.method) ||
			!encode_constant(p_function->rpc_config, r_record.rpc_config)) {
		return false;
	}
	Vector<StringName> constant_names;
	for (const KeyValue<StringName, Variant> &constant : p_function->constant_map) {
		constant_names.push_back(constant.key);
	}
	constant_names.sort();
	for (const StringName &name : constant_names) {
		NamedConstantRecord constant;
		constant.name = name;
		if (!encode_constant(p_function->constant_map[name], constant.value)) {
			return false;
		}
		r_record.local_constants.push_back(constant);
	}
	return true;
}

bool Internals::function_metadata_matches(const FunctionRecord &p_record, GDScriptFunction *p_function) {
	FunctionRecord current;
	if (!make_function_metadata(p_function, current)) {
		return false;
	}
	Writer expected;
	expected.string(p_record.name);
	expected.string(p_record.source);
	expected.u32(uint32_t(p_record.default_argument_count));
	expected.u32(p_record.argument_types.size());
	for (const DataTypeRecord &type : p_record.argument_types) {
		write_data_type(expected, type);
	}
	write_data_type(expected, p_record.return_type);
	write_method(expected, p_record.method);
	write_constant(expected, p_record.rpc_config);
	expected.u32(p_record.local_constants.size());
	for (const NamedConstantRecord &constant : p_record.local_constants) {
		write_named_constant(expected, constant);
	}

	Writer actual;
	actual.string(current.name);
	actual.string(current.source);
	actual.u32(uint32_t(current.default_argument_count));
	actual.u32(current.argument_types.size());
	for (const DataTypeRecord &type : current.argument_types) {
		write_data_type(actual, type);
	}
	write_data_type(actual, current.return_type);
	write_method(actual, current.method);
	write_constant(actual, current.rpc_config);
	actual.u32(current.local_constants.size());
	for (const NamedConstantRecord &constant : current.local_constants) {
		write_named_constant(actual, constant);
	}
	return expected.data == actual.data;
}

uint32_t get_portable_function_fingerprint(const FunctionRecord &p_record) {
	Writer serialized;
	write_int_vector(serialized, p_record.code);
	serialized.u32(p_record.global_names.size());
	for (const StringName &name : p_record.global_names) {
		serialized.string(name);
	}
	serialized.u32(p_record.constants.size());
	for (const ConstantData &constant : p_record.constants) {
		write_constant(serialized, constant);
	}
	serialized.u32(p_record.argument_types.size());
	for (const DataTypeRecord &type : p_record.argument_types) {
		write_data_type(serialized, type);
	}
	write_data_type(serialized, p_record.return_type);
	const uint64_t fingerprint = GDScriptCompiledModule::fingerprint_bytes(serialized.data.ptr(), serialized.data.size());
	return uint32_t(fingerprint) ^ uint32_t(fingerprint >> 32);
}

template <typename T>
bool find_typed_member_symbol(T p_pointer, Symbol &r_symbol, T (*p_getter)(Variant::Type, const StringName &)) {
	for (int type = 0; type < Variant::VARIANT_MAX; type++) {
		List<StringName> members;
		Variant::get_member_list(Variant::Type(type), &members);
		for (const StringName &member : members) {
			if (p_getter(Variant::Type(type), member) == p_pointer) {
				r_symbol.x = type;
				r_symbol.name = member;
				return true;
			}
		}
	}
	return false;
}

template <typename T>
bool find_type_symbol(T p_pointer, Symbol &r_symbol, T (*p_getter)(Variant::Type)) {
	for (int type = 0; type < Variant::VARIANT_MAX; type++) {
		if (p_getter(Variant::Type(type)) == p_pointer) {
			r_symbol.x = type;
			return true;
		}
	}
	return false;
}

bool Internals::make_relocations(const GDScriptFunction *p_function, const HashMap<GDScriptFunction *, String> &p_identities, Vector<Vector<Symbol>> &r_tables) {
	r_tables.resize(RELOC_TABLE_MAX);

	for (Variant::ValidatedOperatorEvaluator pointer : p_function->operator_funcs) {
		Symbol symbol;
		bool found = false;
		for (int op = 0; op < Variant::OP_MAX && !found; op++) {
			for (int left = 0; left < Variant::VARIANT_MAX && !found; left++) {
				for (int right = 0; right < Variant::VARIANT_MAX; right++) {
					if (Variant::get_validated_operator_evaluator(Variant::Operator(op), Variant::Type(left), Variant::Type(right)) == pointer) {
						symbol.x = op;
						symbol.y = left;
						symbol.z = right;
						found = true;
						break;
					}
				}
			}
		}
		if (!found) {
			return false;
		}
		r_tables.write[RELOC_OPERATOR].push_back(symbol);
	}

	for (Variant::ValidatedSetter pointer : p_function->setters) {
		Symbol symbol;
		if (!find_typed_member_symbol(pointer, symbol, Variant::get_member_validated_setter)) {
			return false;
		}
		r_tables.write[RELOC_SETTER].push_back(symbol);
	}
	for (Variant::ValidatedGetter pointer : p_function->getters) {
		Symbol symbol;
		if (!find_typed_member_symbol(pointer, symbol, Variant::get_member_validated_getter)) {
			return false;
		}
		r_tables.write[RELOC_GETTER].push_back(symbol);
	}
	for (Variant::ValidatedKeyedSetter pointer : p_function->keyed_setters) {
		Symbol symbol;
		if (!find_type_symbol(pointer, symbol, Variant::get_member_validated_keyed_setter)) {
			return false;
		}
		r_tables.write[RELOC_KEYED_SETTER].push_back(symbol);
	}
	for (Variant::ValidatedKeyedGetter pointer : p_function->keyed_getters) {
		Symbol symbol;
		if (!find_type_symbol(pointer, symbol, Variant::get_member_validated_keyed_getter)) {
			return false;
		}
		r_tables.write[RELOC_KEYED_GETTER].push_back(symbol);
	}
	for (Variant::ValidatedIndexedSetter pointer : p_function->indexed_setters) {
		Symbol symbol;
		if (!find_type_symbol(pointer, symbol, Variant::get_member_validated_indexed_setter)) {
			return false;
		}
		r_tables.write[RELOC_INDEXED_SETTER].push_back(symbol);
	}
	for (Variant::ValidatedIndexedGetter pointer : p_function->indexed_getters) {
		Symbol symbol;
		if (!find_type_symbol(pointer, symbol, Variant::get_member_validated_indexed_getter)) {
			return false;
		}
		r_tables.write[RELOC_INDEXED_GETTER].push_back(symbol);
	}

	for (Variant::ValidatedBuiltInMethod pointer : p_function->builtin_methods) {
		Symbol symbol;
		bool found = false;
		for (int type = 0; type < Variant::VARIANT_MAX && !found; type++) {
			List<StringName> methods;
			Variant::get_builtin_method_list(Variant::Type(type), &methods);
			for (const StringName &method : methods) {
				if (Variant::get_validated_builtin_method(Variant::Type(type), method) == pointer) {
					symbol.x = type;
					symbol.name = method;
					symbol.hash = Variant::get_builtin_method_hash(Variant::Type(type), method);
					found = true;
					break;
				}
			}
		}
		if (!found) {
			return false;
		}
		r_tables.write[RELOC_BUILTIN_METHOD].push_back(symbol);
	}

	for (Variant::ValidatedConstructor pointer : p_function->constructors) {
		Symbol symbol;
		bool found = false;
		for (int type = 0; type < Variant::VARIANT_MAX && !found; type++) {
			for (int constructor = 0; constructor < Variant::get_constructor_count(Variant::Type(type)); constructor++) {
				if (Variant::get_validated_constructor(Variant::Type(type), constructor) == pointer) {
					symbol.x = type;
					symbol.y = constructor;
					List<MethodInfo> constructors;
					Variant::get_constructor_list(Variant::Type(type), &constructors);
					int index = 0;
					for (const MethodInfo &info : constructors) {
						if (index++ == constructor) {
							symbol.hash = info.get_compatibility_hash();
							break;
						}
					}
					found = true;
					break;
				}
			}
		}
		if (!found) {
			return false;
		}
		r_tables.write[RELOC_CONSTRUCTOR].push_back(symbol);
	}

	List<StringName> utility_names;
	Variant::get_utility_function_list(&utility_names);
	for (Variant::ValidatedUtilityFunction pointer : p_function->utilities) {
		Symbol symbol;
		bool found = false;
		for (const StringName &name : utility_names) {
			if (Variant::get_validated_utility_function(name) == pointer) {
				symbol.name = name;
				symbol.hash = Variant::get_utility_function_hash(name);
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
		r_tables.write[RELOC_UTILITY].push_back(symbol);
	}

	List<StringName> gdscript_utility_names;
	GDScriptUtilityFunctions::get_function_list(&gdscript_utility_names);
	for (GDScriptUtilityFunctions::FunctionPtr pointer : p_function->gds_utilities) {
		Symbol symbol;
		bool found = false;
		for (const StringName &name : gdscript_utility_names) {
			if (GDScriptUtilityFunctions::get_function(name) == pointer) {
				symbol.name = name;
				symbol.hash = GDScriptUtilityFunctions::get_function_info(name).get_compatibility_hash();
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
		r_tables.write[RELOC_GDSCRIPT_UTILITY].push_back(symbol);
	}

	for (MethodBind *method : p_function->methods) {
		if (method == nullptr) {
			return false;
		}
		Symbol symbol;
		symbol.owner = method->get_instance_class();
		symbol.name = method->get_name();
		symbol.hash = method->get_hash();
		r_tables.write[RELOC_METHOD_BIND].push_back(symbol);
	}

	for (GDScriptFunction *lambda : p_function->lambdas) {
		const String *identity = p_identities.getptr(lambda);
		if (identity == nullptr) {
			return false;
		}
		Symbol symbol;
		symbol.name = *identity;
		r_tables.write[RELOC_LAMBDA].push_back(symbol);
	}
	return true;
}

void Internals::add_function_tree(GDScriptFunction *p_function, const String &p_identity,
		HashMap<GDScriptFunction *, String> &r_identities, Vector<GDScriptFunction *> &r_functions) {
	if (p_function == nullptr || r_identities.has(p_function)) {
		return;
	}
	r_identities.insert(p_function, p_identity);
	r_functions.push_back(p_function);
	for (int i = 0; i < p_function->lambdas.size(); i++) {
		add_function_tree(p_function->lambdas[i], p_identity + "/lambda:" + itos(i), r_identities, r_functions);
	}
}

void Internals::collect_functions(GDScript *p_script, const String &p_class_identity, HashMap<GDScriptFunction *, String> &r_identities, Vector<GDScriptFunction *> &r_functions) {
	Vector<StringName> names;
	for (const KeyValue<StringName, GDScriptFunction *> &entry : p_script->member_functions) {
		names.push_back(entry.key);
	}
	names.sort();
	for (const StringName &name : names) {
		add_function_tree(p_script->member_functions[name], p_class_identity + "/member:" + String(name), r_identities, r_functions);
	}
	add_function_tree(p_script->implicit_initializer, p_class_identity + "/implicit:new", r_identities, r_functions);
	add_function_tree(p_script->implicit_ready, p_class_identity + "/implicit:ready", r_identities, r_functions);
	add_function_tree(p_script->static_initializer, p_class_identity + "/implicit:static", r_identities, r_functions);

	Vector<StringName> subclass_names;
	for (const KeyValue<StringName, Ref<GDScript>> &entry : p_script->subclasses) {
		subclass_names.push_back(entry.key);
	}
	subclass_names.sort();
	for (const StringName &name : subclass_names) {
		collect_functions(p_script->subclasses[name].ptr(), p_class_identity + "/class:" + String(name), r_identities, r_functions);
	}
}

void Internals::collect_classes(GDScript *p_script, const String &p_identity, HashMap<GDScript *, String> &r_identities, Vector<GDScript *> &r_classes) {
	if (p_script == nullptr || r_identities.has(p_script)) {
		return;
	}
	r_identities.insert(p_script, p_identity);
	r_classes.push_back(p_script);
	Vector<StringName> subclass_names;
	for (const KeyValue<StringName, Ref<GDScript>> &entry : p_script->subclasses) {
		subclass_names.push_back(entry.key);
	}
	subclass_names.sort();
	for (const StringName &name : subclass_names) {
		collect_classes(p_script->subclasses[name].ptr(), p_identity + "/class:" + String(name), r_identities, r_classes);
	}
}

bool Internals::make_class_record(GDScript *p_script, const String &p_identity, const HashMap<GDScript *, String> &p_class_identities,
		const HashMap<GDScriptFunction *, String> &p_function_identities, ClassRecord &r_record) {
	ERR_FAIL_NULL_V(p_script, false);
	r_record.identity = p_identity;
	r_record.script_path = GDScript::canonicalize_path(p_script->get_script_path());
	r_record.local_name = p_script->local_name;
	r_record.global_name = p_script->global_name;
	r_record.fully_qualified_name = canonicalize_qualified_script_name(p_script->fully_qualified_name);
	r_record.icon_path = p_script->simplified_icon_path;
	r_record.flags = (p_script->tool ? uint32_t(CLASS_FLAG_TOOL) : 0) | (p_script->_is_abstract ? uint32_t(CLASS_FLAG_ABSTRACT) : 0) |
			(p_script->static_unload ? uint32_t(CLASS_FLAG_STATIC_UNLOAD) : 0);
	if (p_script->_owner != nullptr) {
		const String *owner_identity = p_class_identities.getptr(p_script->_owner);
		if (owner_identity == nullptr) {
			return false;
		}
		r_record.owner_identity = *owner_identity;
	}
	if (p_script->native.is_valid()) {
		r_record.native_base = p_script->native->get_name();
	}
	if (p_script->base.is_valid()) {
		r_record.script_base_path = GDScript::canonicalize_path(p_script->base->get_script_path());
		r_record.script_base_class = canonicalize_qualified_script_name(p_script->base->fully_qualified_name);
		if (r_record.script_base_path.is_empty()) {
			return false;
		}
	}

	auto make_member = [&](const StringName &p_name, const GDScript::MemberInfo &p_member, bool p_own, MemberRecord &r_member) {
		r_member.name = p_name;
		r_member.index = p_member.index;
		r_member.setter = p_member.setter;
		r_member.getter = p_member.getter;
		r_member.own_member = p_own;
		r_member.property = make_property_record(p_member.property_info);
		if (p_own) {
			const Variant *default_value = p_script->member_default_values.getptr(p_name);
			if (default_value != nullptr) {
				r_member.has_default_value = true;
				if (!encode_constant(*default_value, r_member.default_value)) {
					return false;
				}
			}
		}
		return make_data_type_record(p_member.data_type, r_member.data_type);
	};

	Vector<StringName> names;
	for (const KeyValue<StringName, GDScript::MemberInfo> &member : p_script->member_indices) {
		names.push_back(member.key);
	}
	names.sort();
	for (const StringName &name : names) {
		MemberRecord member;
		if (!make_member(name, p_script->member_indices[name], p_script->members.has(name), member)) {
			return false;
		}
		r_record.members.push_back(member);
	}

	names.clear();
	for (const KeyValue<StringName, GDScript::MemberInfo> &member : p_script->static_variables_indices) {
		names.push_back(member.key);
	}
	names.sort();
	for (const StringName &name : names) {
		MemberRecord member;
		if (!make_member(name, p_script->static_variables_indices[name], true, member)) {
			return false;
		}
		r_record.static_members.push_back(member);
	}

	names.clear();
	for (const KeyValue<StringName, Ref<StructLayout>> &layout : p_script->struct_layouts) {
		names.push_back(layout.key);
	}
	names.sort();
	for (const StringName &name : names) {
		const Ref<StructLayout> &layout = p_script->struct_layouts[name];
		if (layout.is_null() || !layout->is_finalized()) {
			return false;
		}
		StructLayoutRecord layout_record;
		layout_record.name = name;
		layout_record.type_identifier = layout->get_type_identifier();
		layout_record.schema_version = layout->get_schema_version();
		layout_record.schema_fingerprint = layout->get_schema_fingerprint();
		for (int i = 0; i < layout->get_field_count(); i++) {
			const StructLayout::Field &field = layout->get_field(i);
			StructFieldRecord field_record;
			field_record.name = field.name;
			field_record.type = field.type;
			if (field.type == Variant::STRUCT) {
				if (field.struct_layout.is_null() || !field.struct_layout->is_finalized()) {
					return false;
				}
				field_record.nested_layout = field.struct_layout->get_type_identifier();
			}
			if (!encode_constant(layout->get_default_value(i), field_record.default_value)) {
				return false;
			}
			layout_record.fields.push_back(field_record);
		}
		r_record.struct_layouts.push_back(layout_record);
	}

	names.clear();
	for (const KeyValue<StringName, Variant> &constant : p_script->constants) {
		names.push_back(constant.key);
	}
	names.sort();
	for (const StringName &name : names) {
		NamedConstantRecord constant;
		constant.name = name;
		if (!encode_constant(p_script->constants[name], constant.value)) {
			return false;
		}
		r_record.constants.push_back(constant);
	}

	names.clear();
	for (const KeyValue<StringName, GDScriptFunction *> &function : p_script->member_functions) {
		names.push_back(function.key);
	}
	names.sort();
	for (const StringName &name : names) {
		const String *identity = p_function_identities.getptr(p_script->member_functions[name]);
		if (identity == nullptr) {
			return false;
		}
		GDScriptFunction *function = p_script->member_functions[name];
		FunctionRecord function_metadata;
		if (!make_function_metadata(function, function_metadata)) {
			return false;
		}
		MethodBindingRecord method;
		method.name = name;
		method.identity = *identity;
		method.is_static = function->_static;
		method.default_argument_count = function_metadata.default_argument_count;
		method.argument_types = function_metadata.argument_types;
		method.return_type = function_metadata.return_type;
		method.method = function_metadata.method;
		method.rpc_config = function_metadata.rpc_config;
		r_record.methods.push_back(method);
	}

	names.clear();
	for (const KeyValue<StringName, Ref<GDScript>> &subclass : p_script->subclasses) {
		names.push_back(subclass.key);
	}
	names.sort();
	for (const StringName &name : names) {
		const String *identity = p_class_identities.getptr(p_script->subclasses[name].ptr());
		if (identity == nullptr) {
			return false;
		}
		r_record.subclasses.push_back({ String(name), *identity });
	}

	names.clear();
	for (const KeyValue<StringName, MethodInfo> &signal : p_script->_signals) {
		names.push_back(signal.key);
	}
	names.sort();
	for (const StringName &name : names) {
		SignalRecord signal;
		signal.name = name;
		if (!make_method_record(p_script->_signals[name], signal.method)) {
			return false;
		}
		r_record.signals.push_back(signal);
	}

	Vector<String> lambda_identities;
	HashMap<String, GDScript::LambdaInfo> lambdas;
	for (const KeyValue<GDScriptFunction *, GDScript::LambdaInfo> &lambda : p_script->lambda_info) {
		const String *identity = p_function_identities.getptr(lambda.key);
		if (identity == nullptr) {
			return false;
		}
		lambda_identities.push_back(*identity);
		lambdas.insert(*identity, lambda.value);
	}
	lambda_identities.sort();
	for (const String &identity : lambda_identities) {
		const GDScript::LambdaInfo &info = lambdas[identity];
		r_record.lambdas.push_back({ identity, info.capture_count, info.use_self });
	}

	if (!encode_constant(p_script->rpc_config, r_record.rpc_config)) {
		return false;
	}
	auto set_function_identity = [&](GDScriptFunction *p_function, String &r_identity) {
		if (p_function == nullptr) {
			return true;
		}
		const String *identity = p_function_identities.getptr(p_function);
		if (identity == nullptr) {
			return false;
		}
		r_identity = *identity;
		return true;
	};
	return set_function_identity(p_script->initializer, r_record.initializer) &&
			set_function_identity(p_script->implicit_initializer, r_record.implicit_initializer) &&
			set_function_identity(p_script->implicit_ready, r_record.implicit_ready) &&
			set_function_identity(p_script->static_initializer, r_record.static_initializer);
}

bool Internals::validate_class_record(const ClassRecord &p_record, GDScript *p_script, const HashMap<GDScript *, String> &p_class_identities,
		const HashMap<GDScriptFunction *, String> &p_function_identities) {
	ClassRecord current;
	const String *identity = p_class_identities.getptr(p_script);
	if (identity == nullptr || !make_class_record(p_script, *identity, p_class_identities, p_function_identities, current)) {
		return false;
	}
	Writer expected;
	Writer actual;
	write_class(expected, p_record);
	write_class(actual, current);
	return expected.data == actual.data;
}

uint64_t Internals::dependency_source_fingerprint(GDScript *p_script) {
	if (p_script == nullptr) {
		return 0;
	}
	if (!p_script->source.is_empty()) {
		return GDScriptCompiledModule::fingerprint_source(p_script->source);
	}
	const String path = p_script->get_script_path();
	if (path.is_empty() || !FileAccess::exists(path)) {
		return 0;
	}
	return GDScriptCompiledModule::fingerprint_source(FileAccess::get_file_as_string(path));
}

void Internals::add_script_dependency(GDScript *p_root, GDScript *p_dependency, HashMap<String, uint64_t> &r_dependencies) {
	if (p_dependency == nullptr || p_dependency == p_root) {
		return;
	}
	const String path = GDScript::canonicalize_path(p_dependency->get_script_path());
	if (path.is_empty() || path == GDScript::canonicalize_path(p_root->get_script_path())) {
		return;
	}
	r_dependencies.insert(path, dependency_source_fingerprint(p_dependency));
}

void Internals::collect_dependencies_from_function(GDScript *p_root, GDScriptFunction *p_function,
		HashMap<String, uint64_t> &r_dependencies, HashSet<GDScriptFunction *> &r_visited) {
	if (p_function == nullptr || r_visited.has(p_function)) {
		return;
	}
	r_visited.insert(p_function);
	for (const Variant &constant : p_function->constants) {
		if (constant.get_type() == Variant::OBJECT) {
			add_script_dependency(p_root, Object::cast_to<GDScript>(constant.get_validated_object()), r_dependencies);
		}
	}
	for (GDScriptFunction *lambda : p_function->lambdas) {
		collect_dependencies_from_function(p_root, lambda, r_dependencies, r_visited);
	}
}

void Internals::collect_dependencies_from_script(GDScript *p_root, GDScript *p_script, HashMap<String, uint64_t> &r_dependencies) {
	add_script_dependency(p_root, p_script->base.ptr(), r_dependencies);
	for (const KeyValue<StringName, Variant> &entry : p_script->constants) {
		if (entry.value.get_type() == Variant::OBJECT) {
			add_script_dependency(p_root, Object::cast_to<GDScript>(entry.value.get_validated_object()), r_dependencies);
		}
	}
	HashSet<GDScriptFunction *> visited;
	for (const KeyValue<StringName, GDScriptFunction *> &entry : p_script->member_functions) {
		collect_dependencies_from_function(p_root, entry.value, r_dependencies, visited);
	}
	collect_dependencies_from_function(p_root, p_script->implicit_initializer, r_dependencies, visited);
	collect_dependencies_from_function(p_root, p_script->implicit_ready, r_dependencies, visited);
	collect_dependencies_from_function(p_root, p_script->static_initializer, r_dependencies, visited);
	for (const KeyValue<StringName, Ref<GDScript>> &entry : p_script->subclasses) {
		collect_dependencies_from_script(p_root, entry.value.ptr(), r_dependencies);
	}
}

bool Internals::make_symbolic_global_code(const GDScriptFunction *p_function, Vector<int> &r_code, Vector<StringName> &r_names) {
	ERR_FAIL_NULL_V(p_function, false);
	GDScriptLanguage *language = GDScriptLanguage::get_singleton();
	if (language == nullptr) {
		return false;
	}

	r_code = p_function->code;
	r_names = p_function->global_names;
	HashMap<StringName, int> name_indices;
	for (int i = 0; i < r_names.size(); i++) {
		name_indices.insert(r_names[i], i);
	}

	const HashMap<StringName, int> &globals = language->get_global_map();
	for (int ip = 0; ip < r_code.size();) {
		const int length = GDScriptFunction::get_instruction_size(r_code.ptr(), r_code.size(), ip);
		if (length <= 0) {
			return false;
		}
		for (int word = 1; word < length; word++) {
			if (GDScriptFunction::get_operand_kind(r_code.ptr(), r_code.size(), ip, word) != GDScriptFunction::OPERAND_GLOBAL_INDEX) {
				continue;
			}
			const int runtime_index = r_code[ip + word];
			StringName global_name;
			for (const KeyValue<StringName, int> &entry : globals) {
				if (entry.value == runtime_index) {
					global_name = entry.key;
					break;
				}
			}
			if (global_name.is_empty()) {
				return false;
			}
			int *name_index = name_indices.getptr(global_name);
			if (name_index == nullptr) {
				const int new_index = r_names.size();
				r_names.push_back(global_name);
				name_indices.insert(global_name, new_index);
				name_index = name_indices.getptr(global_name);
			}
			r_code.write[ip + word] = *name_index;
		}
		ip += length;
	}
	return true;
}

bool Internals::resolve_symbolic_global_code(const FunctionRecord &p_record, Vector<int> &r_code, String *r_error) {
	GDScriptLanguage *language = GDScriptLanguage::get_singleton();
	if (language == nullptr) {
		if (r_error != nullptr) {
			*r_error = "The GDScript language is not initialized.";
		}
		return false;
	}

	r_code = p_record.code;
	const HashMap<StringName, int> &globals = language->get_global_map();
	for (int ip = 0; ip < r_code.size();) {
		const int length = GDScriptFunction::get_instruction_size(r_code.ptr(), r_code.size(), ip);
		if (length <= 0) {
			if (r_error != nullptr) {
				*r_error = "Invalid instruction while resolving symbolic globals in '" + p_record.identity + "'.";
			}
			return false;
		}
		for (int word = 1; word < length; word++) {
			if (GDScriptFunction::get_operand_kind(r_code.ptr(), r_code.size(), ip, word) != GDScriptFunction::OPERAND_GLOBAL_INDEX) {
				continue;
			}
			const int symbol_index = r_code[ip + word];
			if (symbol_index < 0 || symbol_index >= p_record.global_names.size()) {
				if (r_error != nullptr) {
					*r_error = "Invalid global symbol in function '" + p_record.identity + "'.";
				}
				return false;
			}
			const StringName &name = p_record.global_names[symbol_index];
			const int *runtime_index = globals.getptr(name);
			if (runtime_index == nullptr || *runtime_index < 0 || *runtime_index >= language->get_global_array_size()) {
				if (r_error != nullptr) {
					*r_error = "Could not resolve global symbol '" + String(name) + "' in function '" + p_record.identity + "'.";
				}
				return false;
			}
			r_code.write[ip + word] = *runtime_index;
		}
		ip += length;
	}
	return true;
}

bool Internals::make_record(GDScriptFunction *p_function, const String &p_identity, const HashMap<GDScriptFunction *, String> &p_identities, FunctionRecord &r_record) {
	r_record.identity = p_identity;
	if (!make_function_metadata(p_function, r_record)) {
		return false;
	}
	const uint32_t runtime_fingerprint = p_function->get_optimization_fingerprint();
	r_record.initial_line = p_function->_initial_line;
	r_record.argument_count = p_function->_argument_count;
	r_record.vararg_index = p_function->_vararg_index;
	r_record.stack_size = p_function->_stack_size;
	r_record.instruction_args_size = p_function->_instruction_args_size;
	r_record.operator_feedback_count = p_function->_operator_feedback_count;
	r_record.call_feedback_count = p_function->_call_feedback_count;
	r_record.is_static = p_function->_static;
	r_record.profile_guided = GDScriptOptimizationProfile::has_hint(p_function->get_optimization_profile_key(), runtime_fingerprint);
	r_record.default_arguments = p_function->default_arguments;
	if (!make_symbolic_global_code(p_function, r_record.code, r_record.global_names)) {
		return false;
	}
	for (const Pair<int, Variant::Type> &slot : p_function->temporary_slots) {
		r_record.temporary_slots.push_back(slot);
	}
	if (r_record.code.is_empty() || r_record.code[r_record.code.size() - 1] != GDScriptFunction::OPCODE_END) {
		return false;
	}
	for (const Variant &constant : p_function->constants) {
		ConstantData encoded;
		if (!encode_constant(constant, encoded)) {
			return false;
		}
		r_record.constants.push_back(encoded);
	}
	r_record.fingerprint = get_portable_function_fingerprint(r_record);
	return make_relocations(p_function, p_identities, r_record.relocations);
}

bool validate_dependency(const GDScriptCompiledModule::Dependency &p_dependency) {
	const String remapped = ResourceLoader::path_remap(p_dependency.path);
	if (!FileAccess::exists(remapped)) {
		return false;
	}
	if (remapped.get_extension() == "gdm") {
		ParsedModule dependency_module;
		if (parse_module(FileAccess::get_file_as_bytes(remapped), dependency_module, nullptr) != OK) {
			return false;
		}
		return dependency_module.source_fingerprint == p_dependency.source_fingerprint;
	}
	if (remapped.get_extension() == "gd") {
		return GDScriptCompiledModule::fingerprint_source(FileAccess::get_file_as_string(remapped)) == p_dependency.source_fingerprint;
	}
	// A legacy .gdc has no source fingerprint. It remains a valid fallback,
	// but cannot satisfy a compiled-module dependency.
	return false;
}

bool validate_constructor_symbol(const Symbol &p_symbol) {
	if (p_symbol.x < 0 || p_symbol.x >= Variant::VARIANT_MAX || p_symbol.y < 0 || p_symbol.y >= Variant::get_constructor_count(Variant::Type(p_symbol.x))) {
		return false;
	}
	List<MethodInfo> constructors;
	Variant::get_constructor_list(Variant::Type(p_symbol.x), &constructors);
	int index = 0;
	for (const MethodInfo &info : constructors) {
		if (index++ == p_symbol.y) {
			return info.get_compatibility_hash() == p_symbol.hash;
		}
	}
	return false;
}

Error ModuleVerifier::fail(const String &p_message) {
	error = p_message;
	return ERR_INVALID_DATA;
}

Error ModuleVerifier::add_metadata_entries(uint64_t p_count, const String &p_context) {
	if (p_count > MAX_TOTAL_METADATA_ENTRIES || metadata_entries > MAX_TOTAL_METADATA_ENTRIES - p_count) {
		return fail("Compiled module metadata limit exceeded while validating " + p_context + ".");
	}
	metadata_entries += p_count;
	return OK;
}

Error ModuleVerifier::verify_constant(const ConstantData &p_constant, Variant *r_decoded) {
	if (r_decoded != nullptr) {
		*r_decoded = Variant();
	}
	const uint64_t encoded_size = p_constant.encoded.size();
	if (encoded_size > MAX_TOTAL_CONSTANT_BYTES || constant_bytes > MAX_TOTAL_CONSTANT_BYTES - encoded_size) {
		return fail("Compiled module constant-data limit exceeded.");
	}
	constant_bytes += encoded_size;
	switch (p_constant.kind) {
		case CONSTANT_VARIANT: {
			if (!p_constant.name.is_empty() || !p_constant.owner.is_empty()) {
				return fail("Variant constants contain unexpected symbolic metadata.");
			}
			Variant decoded;
			int used = 0;
			if (decode_variant(decoded, p_constant.encoded.ptr(), p_constant.encoded.size(), &used, false) != OK ||
					used != p_constant.encoded.size() || !is_pointer_free_variant(decoded)) {
				return fail("Invalid pointer-free Variant constant.");
			}
			if (r_decoded != nullptr) {
				*r_decoded = decoded;
			}
			return OK;
		}
		case CONSTANT_RESOURCE:
			if (!p_constant.encoded.is_empty() || p_constant.name.is_empty() || !p_constant.owner.is_empty()) {
				return fail("Invalid resource constant metadata.");
			}
			return OK;
		case CONSTANT_GDSCRIPT:
			if (!p_constant.encoded.is_empty() || p_constant.name.is_empty()) {
				return fail("Invalid GDScript constant metadata.");
			}
			return OK;
		case CONSTANT_NATIVE_CLASS:
			if (!p_constant.encoded.is_empty() || p_constant.name.is_empty() || !p_constant.owner.is_empty() || !ClassDB::class_exists(p_constant.name)) {
				return fail("Invalid native-class constant metadata.");
			}
			return OK;
	}
	return fail("Unknown compiled constant kind.");
}

Error ModuleVerifier::verify_property(const PropertyRecord &p_property) {
	constexpr uint32_t PROPERTY_USAGE_KNOWN_MASK = (uint32_t(1) << 30) - 2;
	if (p_property.type >= Variant::VARIANT_MAX || p_property.hint >= PROPERTY_HINT_MAX || (p_property.usage & ~PROPERTY_USAGE_KNOWN_MASK) != 0) {
		return fail("Unknown property type, hint, or usage flag.");
	}
	if (p_property.type == Variant::OBJECT && !p_property.class_name.is_empty() && !ClassDB::class_exists(p_property.class_name) &&
			!ScriptServer::is_global_class(p_property.class_name)) {
		return fail("Unresolved object class '" + String(p_property.class_name) + "' in property metadata.");
	}
	if (p_property.type == Variant::STRUCT) {
		bool found = false;
		for (const KeyValue<String, Ref<StructLayout>> &entry : verified_layouts) {
			if (entry.value->get_type_identifier() == p_property.class_name || entry.value->get_type_descriptor() == p_property.hint_string) {
				found = true;
				break;
			}
		}
		if (!found) {
			return fail("Unresolved struct layout in property metadata.");
		}
	}
	return OK;
}

Error ModuleVerifier::verify_method(const MethodRecord &p_method) {
	constexpr uint32_t METHOD_FLAGS_KNOWN_MASK = METHOD_FLAG_NORMAL | METHOD_FLAG_EDITOR | METHOD_FLAG_CONST | METHOD_FLAG_VIRTUAL |
			METHOD_FLAG_VARARG | METHOD_FLAG_STATIC | METHOD_FLAG_OBJECT_CORE | METHOD_FLAG_VIRTUAL_REQUIRED;
	if ((p_method.flags & ~METHOD_FLAGS_KNOWN_MASK) != 0 || p_method.default_arguments.size() > p_method.arguments.size() ||
			(!p_method.argument_metadata.is_empty() && p_method.argument_metadata.size() != p_method.arguments.size()) ||
			p_method.return_value_metadata < GodotTypeInfo::METADATA_NONE || p_method.return_value_metadata > GodotTypeInfo::METADATA_OBJECT_IS_REQUIRED) {
		return fail("Invalid method metadata for '" + p_method.name + "'.");
	}
	if (verify_property(p_method.return_value) != OK || add_metadata_entries(p_method.arguments.size() + p_method.default_arguments.size(), "method metadata") != OK) {
		return ERR_INVALID_DATA;
	}
	for (const PropertyRecord &argument : p_method.arguments) {
		if (verify_property(argument) != OK) {
			return ERR_INVALID_DATA;
		}
	}
	for (const ConstantData &argument : p_method.default_arguments) {
		if (verify_constant(argument) != OK) {
			return ERR_INVALID_DATA;
		}
	}
	for (int metadata : p_method.argument_metadata) {
		if (metadata < GodotTypeInfo::METADATA_NONE || metadata > GodotTypeInfo::METADATA_OBJECT_IS_REQUIRED) {
			return fail("Unknown method argument metadata value.");
		}
	}
	return OK;
}

Error ModuleVerifier::verify_data_type(const DataTypeRecord &p_type, int p_depth) {
	if (p_depth > Variant::MAX_RECURSION_DEPTH || p_type.kind > GDScriptDataType::GDSCRIPT || p_type.builtin_type >= Variant::VARIANT_MAX ||
			add_metadata_entries(1, "data types") != OK) {
		return fail("Invalid or excessively nested GDScript data type.");
	}
	const GDScriptDataType::Kind kind = GDScriptDataType::Kind(p_type.kind);
	const Variant::Type builtin = Variant::Type(p_type.builtin_type);
	if (kind == GDScriptDataType::NATIVE && (p_type.native_type.is_empty() || !ClassDB::class_exists(p_type.native_type))) {
		return fail("Unknown native data type '" + p_type.native_type + "'.");
	}
	if ((kind == GDScriptDataType::SCRIPT || kind == GDScriptDataType::GDSCRIPT) && p_type.script_path.is_empty()) {
		return fail("Script data type has no resource path.");
	}
	if (!p_type.struct_layout.is_empty()) {
		Variant serialized_layout;
		int used = 0;
		if (kind != GDScriptDataType::BUILTIN || builtin != Variant::STRUCT ||
				decode_variant(serialized_layout, p_type.struct_layout.ptr(), p_type.struct_layout.size(), &used, false) != OK ||
				used != p_type.struct_layout.size() || serialized_layout.get_type() != Variant::DICTIONARY) {
			return fail("Invalid serialized struct data type.");
		}
		Error layout_error = OK;
		Ref<StructLayout> layout = StructLayout::from_dictionary(serialized_layout, &layout_error);
		if (layout_error != OK || layout.is_null() || layout->get_field_count() > int(MAX_STRUCT_FIELDS)) {
			return fail("Invalid struct layout embedded in a data type.");
		}
	} else if (kind == GDScriptDataType::BUILTIN && builtin == Variant::STRUCT) {
		return fail("Typed struct data type has no layout descriptor.");
	}
	const int expected_elements = kind == GDScriptDataType::BUILTIN && builtin == Variant::ARRAY ? 1 :
			kind == GDScriptDataType::BUILTIN && builtin == Variant::DICTIONARY ? 2 : 0;
	if (p_type.container_element_types.size() > expected_elements || (expected_elements == 0 && !p_type.container_element_types.is_empty())) {
		return fail("Invalid container element type metadata.");
	}
	for (const DataTypeRecord &element : p_type.container_element_types) {
		if (verify_data_type(element, p_depth + 1) != OK) {
			return ERR_INVALID_DATA;
		}
	}
	return OK;
}

Error ModuleVerifier::verify_layout(const String &p_identifier, int p_depth) {
	if (verified_layouts.has(p_identifier)) {
		return OK;
	}
	const StructLayoutRecord *const *record_ptr = layout_records.getptr(p_identifier);
	if (p_depth > int(MAX_GRAPH_DEPTH) || record_ptr == nullptr || layouts_being_verified.has(p_identifier)) {
		return fail("Unknown, cyclic, or excessively nested struct layout '" + p_identifier + "'.");
	}
	const StructLayoutRecord &record = **record_ptr;
	if (record.type_identifier != p_identifier || record.name.is_empty() || record.fields.size() > int(MAX_STRUCT_FIELDS)) {
		return fail("Invalid struct layout record '" + p_identifier + "'.");
	}
	layouts_being_verified.insert(p_identifier);
	Ref<StructLayout> layout;
	layout.instantiate(record.type_identifier, record.schema_version);
	HashSet<StringName> field_names;
	for (const StructFieldRecord &field : record.fields) {
		if (field.name.is_empty() || field.type >= Variant::VARIANT_MAX || field_names.has(field.name) ||
				(field.type == Variant::STRUCT) != !field.nested_layout.is_empty()) {
			layouts_being_verified.erase(p_identifier);
			return fail("Invalid field metadata in struct layout '" + p_identifier + "'.");
		}
		field_names.insert(field.name);
		Ref<StructLayout> nested;
		if (field.type == Variant::STRUCT) {
			if (verify_layout(field.nested_layout, p_depth + 1) != OK) {
				layouts_being_verified.erase(p_identifier);
				return ERR_INVALID_DATA;
			}
			nested = *verified_layouts.getptr(field.nested_layout);
		}
		Variant default_value;
		if (verify_constant(field.default_value, &default_value) != OK) {
			layouts_being_verified.erase(p_identifier);
			return ERR_INVALID_DATA;
		}
		if (field.default_value.kind != CONSTANT_VARIANT) {
			if (field.type != Variant::OBJECT) {
				layouts_being_verified.erase(p_identifier);
				return fail("Non-object struct field has a symbolic default value.");
			}
			default_value = Variant();
		}
		if (layout->add_field(field.name, Variant::Type(field.type), default_value, nested) != OK) {
			layouts_being_verified.erase(p_identifier);
			return fail("Invalid default value in struct layout '" + p_identifier + "'.");
		}
	}
	if (layout->finalize() != OK || layout->get_schema_fingerprint() != record.schema_fingerprint) {
		layouts_being_verified.erase(p_identifier);
		return fail("Struct schema fingerprint mismatch for '" + p_identifier + "'.");
	}
	layouts_being_verified.erase(p_identifier);
	verified_layouts.insert(p_identifier, layout);
	return OK;
}

Error ModuleVerifier::index_and_verify_metadata() {
	if (module.path.is_empty() || module.path != GDScript::canonicalize_path(module.path) || module.source_fingerprint == 0 || module.engine_api_fingerprint == 0 ||
			module.fallback_tokens.is_empty() || module.classes.is_empty() || module.classes.size() > int(MAX_MODULE_CLASSES) || module.functions.size() > int(MAX_MODULE_FUNCTIONS)) {
		return fail("Compiled module exceeds class/function limits or has no root class.");
	}
	if (add_metadata_entries(module.dependencies.size() + module.classes.size() + module.functions.size(), "module tables") != OK) {
		return ERR_INVALID_DATA;
	}
	const String module_path = GDScript::canonicalize_path(module.path);
	HashSet<String> dependency_paths;
	for (const GDScriptCompiledModule::Dependency &dependency : module.dependencies) {
		const String canonical_dependency = GDScript::canonicalize_path(dependency.path);
		if (dependency.path.is_empty() || dependency.path != canonical_dependency || canonical_dependency == module_path ||
				dependency.source_fingerprint == 0 || dependency_paths.has(canonical_dependency)) {
			return fail("Invalid or duplicate compiled-module dependency.");
		}
		dependency_paths.insert(canonical_dependency);
	}

	const ClassRecord *root_record = nullptr;
	HashMap<String, String> qualified_classes;
	for (const ClassRecord &record : module.classes) {
		if (record.identity.is_empty() || classes.has(record.identity) || record.script_path != module_path || record.fully_qualified_name.is_empty() ||
				(record.flags & ~CLASS_FLAG_MASK) != 0 || record.native_base.is_empty() || !ClassDB::class_exists(record.native_base)) {
			return fail("Invalid or duplicate class metadata for '" + record.identity + "'.");
		}
		const String qualified = canonicalize_qualified_script_name(record.fully_qualified_name);
		if (qualified_classes.has(qualified)) {
			return fail("Duplicate qualified class name '" + qualified + "'.");
		}
		qualified_classes.insert(qualified, record.identity);
		classes.insert(record.identity, &record);
		if (record.owner_identity.is_empty()) {
			if (root_record != nullptr || record.identity != "root") {
				return fail("The compiled module must contain exactly one root class.");
			}
			root_record = &record;
		} else if (record.local_name.is_empty()) {
			return fail("Nested class '" + record.identity + "' has no local name.");
		}
	}
	if (root_record == nullptr) {
		return fail("The compiled module has no root class.");
	}

	for (const FunctionRecord &record : module.functions) {
		if (record.identity.is_empty() || record.name.is_empty() || functions.has(record.identity)) {
			return fail("Invalid or duplicate function identity '" + record.identity + "'.");
		}
		functions.insert(record.identity, &record);
	}

	for (const ClassRecord &record : module.classes) {
		for (const StructLayoutRecord &layout : record.struct_layouts) {
			if (layout.type_identifier.is_empty() || layout_records.has(layout.type_identifier)) {
				return fail("Duplicate or empty struct layout identifier in class '" + record.identity + "'.");
			}
			layout_records.insert(layout.type_identifier, &layout);
		}
	}
	for (const KeyValue<String, const StructLayoutRecord *> &entry : layout_records) {
		if (verify_layout(entry.key) != OK) {
			return ERR_INVALID_DATA;
		}
	}

	HashMap<String, int> child_binding_counts;
	for (const ClassRecord &record : module.classes) {
		if (!record.owner_identity.is_empty() && !classes.has(record.owner_identity)) {
			return fail("Nested class '" + record.identity + "' has an unknown owner.");
		}
		const uint64_t class_entries = record.members.size() + record.static_members.size() + record.struct_layouts.size() + record.constants.size() +
				record.methods.size() + record.subclasses.size() + record.signals.size() + record.lambdas.size();
		if (add_metadata_entries(class_entries, "class metadata") != OK) {
			return ERR_INVALID_DATA;
		}
		HashSet<StringName> member_names;
		HashSet<int> member_indices;
		for (const MemberRecord &member : record.members) {
			if (member.name.is_empty() || member.index < 0 || member_names.has(member.name) || member_indices.has(member.index) ||
					verify_data_type(member.data_type) != OK || verify_property(member.property) != OK ||
					(member.has_default_value && verify_constant(member.default_value) != OK)) {
				return fail("Invalid member metadata in class '" + record.identity + "'.");
			}
			member_names.insert(member.name);
			member_indices.insert(member.index);
		}
		for (int index = 0; index < record.members.size(); index++) {
			if (!member_indices.has(index)) {
				return fail("Non-contiguous member indices in class '" + record.identity + "'.");
			}
		}
		class_member_counts.insert(record.identity, record.members.size());

		HashSet<StringName> static_names;
		HashSet<int> static_indices;
		for (const MemberRecord &member : record.static_members) {
			if (member.name.is_empty() || member.index < 0 || static_names.has(member.name) || static_indices.has(member.index) ||
					verify_data_type(member.data_type) != OK || verify_property(member.property) != OK ||
					(member.has_default_value && verify_constant(member.default_value) != OK)) {
				return fail("Invalid static-member metadata in class '" + record.identity + "'.");
			}
			static_names.insert(member.name);
			static_indices.insert(member.index);
		}
		for (int index = 0; index < record.static_members.size(); index++) {
			if (!static_indices.has(index)) {
				return fail("Non-contiguous static-member indices in class '" + record.identity + "'.");
			}
		}

		HashSet<StringName> layout_names;
		for (const StructLayoutRecord &layout : record.struct_layouts) {
			if (layout.name.is_empty() || layout_names.has(layout.name)) {
				return fail("Duplicate struct declaration name in class '" + record.identity + "'.");
			}
			layout_names.insert(layout.name);
		}
		HashSet<StringName> constant_names;
		for (const NamedConstantRecord &constant : record.constants) {
			if (constant.name.is_empty() || constant_names.has(constant.name) || verify_constant(constant.value) != OK) {
				return fail("Invalid class constant metadata in '" + record.identity + "'.");
			}
			constant_names.insert(constant.name);
		}

		HashSet<StringName> method_names;
		for (const MethodBindingRecord &binding : record.methods) {
			const FunctionRecord *const *function_ptr = functions.getptr(binding.identity);
			if (binding.name.is_empty() || binding.identity.is_empty() || method_names.has(binding.name) || function_ptr == nullptr ||
					binding.default_argument_count < 0 || binding.default_argument_count > binding.argument_types.size() ||
					verify_method(binding.method) != OK || verify_data_type(binding.return_type) != OK || verify_constant(binding.rpc_config) != OK) {
				return fail("Invalid method binding in class '" + record.identity + "'.");
			}
			for (const DataTypeRecord &argument : binding.argument_types) {
				if (verify_data_type(argument) != OK) {
					return ERR_INVALID_DATA;
				}
			}
			Writer binding_signature;
			binding_signature.u32(binding.argument_types.size());
			for (const DataTypeRecord &type : binding.argument_types) {
				write_data_type(binding_signature, type);
			}
			write_data_type(binding_signature, binding.return_type);
			write_method(binding_signature, binding.method);
			write_constant(binding_signature, binding.rpc_config);
			const FunctionRecord &function = **function_ptr;
			Writer function_signature;
			function_signature.u32(function.argument_types.size());
			for (const DataTypeRecord &type : function.argument_types) {
				write_data_type(function_signature, type);
			}
			write_data_type(function_signature, function.return_type);
			write_method(function_signature, function.method);
			write_constant(function_signature, function.rpc_config);
			if (binding.name != function.name || binding.is_static != function.is_static ||
					binding.default_argument_count != function.default_argument_count || binding_signature.data != function_signature.data) {
				return fail("Method signature mismatch for function '" + binding.identity + "'.");
			}
			method_names.insert(binding.name);
			if (function_owners.has(binding.identity)) {
				return fail("Function '" + binding.identity + "' is bound more than once.");
			}
			function_owners.insert(binding.identity, record.identity);
			bound_methods.insert(binding.identity);
		}

		HashSet<StringName> subclass_names;
		for (const BindingRecord &binding : record.subclasses) {
			const ClassRecord *const *child_ptr = classes.getptr(binding.identity);
			if (binding.name.is_empty() || subclass_names.has(binding.name) || child_ptr == nullptr ||
					(*child_ptr)->owner_identity != record.identity || (*child_ptr)->local_name != binding.name) {
				return fail("Invalid nested-class binding in '" + record.identity + "'.");
			}
			subclass_names.insert(binding.name);
			const int *binding_count = child_binding_counts.getptr(binding.identity);
			child_binding_counts.insert(binding.identity, (binding_count == nullptr ? 0 : *binding_count) + 1);
		}

		HashSet<StringName> signal_names;
		for (const SignalRecord &signal : record.signals) {
			if (signal.name.is_empty() || signal_names.has(signal.name) || verify_method(signal.method) != OK) {
				return fail("Invalid signal metadata in class '" + record.identity + "'.");
			}
			signal_names.insert(signal.name);
		}
		Variant rpc;
		if (verify_constant(record.rpc_config, &rpc) != OK || record.rpc_config.kind != CONSTANT_VARIANT || rpc.get_type() != Variant::DICTIONARY) {
			return fail("Invalid RPC metadata in class '" + record.identity + "'.");
		}

		auto bind_special = [&](const String &p_identity) -> Error {
			if (p_identity.is_empty()) {
				return OK;
			}
			if (!functions.has(p_identity) || function_owners.has(p_identity)) {
				return fail("Invalid or multiply bound initializer '" + p_identity + "'.");
			}
			function_owners.insert(p_identity, record.identity);
			return OK;
		};
		if (bind_special(record.initializer) != OK || bind_special(record.implicit_initializer) != OK ||
				bind_special(record.implicit_ready) != OK || bind_special(record.static_initializer) != OK) {
			return ERR_INVALID_DATA;
		}
		HashSet<String> lambda_names;
		for (const LambdaRecord &lambda : record.lambdas) {
			if (lambda.identity.is_empty() || lambda.capture_count < 0 || lambda.capture_count > int(MAX_FUNCTION_ARGUMENTS) ||
					lambda_names.has(lambda.identity) || !functions.has(lambda.identity)) {
				return fail("Invalid lambda metadata in class '" + record.identity + "'.");
			}
			lambda_names.insert(lambda.identity);
		}
	}

	for (const ClassRecord &record : module.classes) {
		if (!record.owner_identity.is_empty()) {
			const int *binding_count = child_binding_counts.getptr(record.identity);
			if (binding_count == nullptr || *binding_count != 1) {
				return fail("Nested class '" + record.identity + "' is not bound exactly once.");
			}
		}
		HashSet<String> visited;
		const ClassRecord *cursor = &record;
		for (uint32_t depth = 0; !cursor->owner_identity.is_empty(); depth++) {
			if (depth > MAX_GRAPH_DEPTH || visited.has(cursor->identity)) {
				return fail("Nested-class ownership contains a cycle or exceeds the depth limit.");
			}
			visited.insert(cursor->identity);
			cursor = *classes.getptr(cursor->owner_identity);
		}
	}

	// Local inheritance can be verified without resolving a Resource. External
	// bases are resolved later by the staged runtime builder, after the complete
	// module has passed this verifier.
	HashMap<String, String> local_bases;
	for (const ClassRecord &record : module.classes) {
		if (record.script_base_path.is_empty() != record.script_base_class.is_empty()) {
			return fail("Incomplete script-base metadata for class '" + record.identity + "'.");
		}
		if (!record.script_base_path.is_empty() && GDScript::canonicalize_path(record.script_base_path) == module_path) {
			const String qualified_base = canonicalize_qualified_script_name(record.script_base_class);
			const String *base_identity = qualified_classes.getptr(qualified_base);
			if (base_identity == nullptr || *base_identity == record.identity) {
				return fail("Unknown or self-referential local base class for '" + record.identity + "'.");
			}
			local_bases.insert(record.identity, *base_identity);
		}
	}
	for (const ClassRecord &record : module.classes) {
		HashSet<String> visited;
		String cursor = record.identity;
		for (uint32_t depth = 0; local_bases.has(cursor); depth++) {
			if (depth > MAX_GRAPH_DEPTH || visited.has(cursor)) {
				return fail("Local script inheritance contains a cycle or exceeds the depth limit.");
			}
			visited.insert(cursor);
			cursor = *local_bases.getptr(cursor);
		}
	}

	for (const FunctionRecord &record : module.functions) {
		if (record.initial_line < 0 || record.argument_count < 0 || record.argument_count > int(MAX_FUNCTION_ARGUMENTS) || record.argument_types.size() != record.argument_count ||
				record.method.arguments.size() != record.argument_count || record.stack_size < GDScriptFunction::FIXED_ADDRESSES_MAX + record.argument_count ||
				record.stack_size > int(MAX_FRAME_SLOTS) || record.instruction_args_size < 0 || record.instruction_args_size > int(MAX_INSTRUCTION_ARGUMENTS) ||
				record.operator_feedback_count < 0 || record.operator_feedback_count > int(MAX_FEEDBACK_SLOTS) || record.call_feedback_count < 0 ||
				record.call_feedback_count > int(MAX_FEEDBACK_SLOTS) || record.default_argument_count < 0 ||
				record.default_argument_count > record.argument_count ||
				(record.vararg_index != -1 && (record.vararg_index != GDScriptFunction::FIXED_ADDRESSES_MAX + record.argument_count || record.vararg_index >= record.stack_size)) ||
				record.relocations.size() != RELOC_TABLE_MAX || record.code.is_empty() || record.code.size() > int(MAX_FUNCTION_CODE_WORDS)) {
				return fail("Invalid signature, frame, table, or fingerprint metadata for function '" + record.identity + "'.");
		}
		const bool method_is_static = (record.method.flags & METHOD_FLAG_STATIC) != 0;
		const bool method_is_vararg = (record.method.flags & METHOD_FLAG_VARARG) != 0;
		if (method_is_vararg != (record.vararg_index >= 0) || (bound_methods.has(record.identity) &&
					(record.method.name != record.name || method_is_static != record.is_static || record.method.default_arguments.size() != record.default_argument_count))) {
			return fail("Function signature metadata is internally inconsistent for '" + record.identity + "'.");
		}
		total_code_words += record.code.size();
		if (total_code_words > MAX_TOTAL_CODE_WORDS || verify_method(record.method) != OK || verify_data_type(record.return_type) != OK ||
				verify_constant(record.rpc_config) != OK) {
			return fail("Function metadata limit or type validation failed for '" + record.identity + "'.");
		}
		for (const DataTypeRecord &argument : record.argument_types) {
			if (verify_data_type(argument) != OK) {
				return ERR_INVALID_DATA;
			}
		}
		HashSet<StringName> local_names;
		for (const NamedConstantRecord &constant : record.local_constants) {
			if (constant.name.is_empty() || local_names.has(constant.name) || verify_constant(constant.value) != OK) {
				return fail("Invalid local constant metadata in function '" + record.identity + "'.");
			}
			local_names.insert(constant.name);
		}
		for (const ConstantData &constant : record.constants) {
			if (verify_constant(constant) != OK) {
				return fail("Invalid constant table in function '" + record.identity + "'.");
			}
		}
		HashSet<StringName> global_names;
		for (const StringName &name : record.global_names) {
			if (name.is_empty() || global_names.has(name)) {
				return fail("Invalid or duplicate global-name table entry in function '" + record.identity + "'.");
			}
			global_names.insert(name);
		}
		uint64_t table_entries = record.constants.size() + record.global_names.size() + record.temporary_slots.size() + record.local_constants.size();
		for (const Vector<Symbol> &table : record.relocations) {
			table_entries += table.size();
		}
		if (add_metadata_entries(table_entries, "function tables") != OK) {
			return ERR_INVALID_DATA;
		}
	}
	return verify_function_graph();
}

Error ModuleVerifier::verify_function_graph() {
	HashMap<String, String> lambda_parents;
	for (const FunctionRecord &record : module.functions) {
		for (const Symbol &symbol : record.relocations[RELOC_LAMBDA]) {
			if (symbol.name.is_empty() || !functions.has(symbol.name) || symbol.name == record.identity || lambda_parents.has(symbol.name) || function_owners.has(symbol.name)) {
				return fail("Invalid or multiply owned lambda function '" + symbol.name + "'.");
			}
			lambda_parents.insert(symbol.name, record.identity);
		}
	}
	for (const FunctionRecord &record : module.functions) {
		if (!function_owners.has(record.identity) && !lambda_parents.has(record.identity)) {
			return fail("Unowned function '" + record.identity + "' in compiled function graph.");
		}
	}

	Vector<Pair<String, uint32_t>> queue;
	for (const KeyValue<String, String> &root_entry : function_owners) {
		queue.push_back({ root_entry.key, 0 });
	}
	for (int cursor = 0; cursor < queue.size(); cursor++) {
		const String identity = queue[cursor].first;
		const uint32_t depth = queue[cursor].second;
		if (depth > MAX_GRAPH_DEPTH) {
			return fail("Lambda graph exceeds the recursion limit.");
		}
		const String owner = *function_owners.getptr(identity);
		const FunctionRecord &record = **functions.getptr(identity);
		for (const Symbol &symbol : record.relocations[RELOC_LAMBDA]) {
			if (function_owners.has(symbol.name)) {
				return fail("Lambda graph contains a cycle or shared function '" + symbol.name + "'.");
			}
			function_owners.insert(symbol.name, owner);
			queue.push_back({ symbol.name, depth + 1 });
		}
	}
	if (function_owners.size() != functions.size()) {
		return fail("Lambda graph contains a cycle or unreachable function.");
	}

	HashSet<String> declared_lambdas;
	for (const ClassRecord &record : module.classes) {
		for (const LambdaRecord &lambda : record.lambdas) {
			const String *owner = function_owners.getptr(lambda.identity);
			if (owner == nullptr || *owner != record.identity || declared_lambdas.has(lambda.identity)) {
				return fail("Lambda class ownership mismatch for '" + lambda.identity + "'.");
			}
			declared_lambdas.insert(lambda.identity);
		}
	}
	if (declared_lambdas.size() != lambda_parents.size()) {
		return fail("Lambda metadata does not cover the complete function graph.");
	}
	return OK;
}

Error ModuleVerifier::decode_instructions() {
	function_info.resize(module.functions.size());
	for (int function_index = 0; function_index < module.functions.size(); function_index++) {
		const FunctionRecord &record = module.functions[function_index];
		FunctionInfo &info = function_info.write[function_index];
		info.record = &record;
		info.boundaries.resize(record.code.size());
		info.boundaries.fill(0);
		info.reachable.resize(record.code.size());
		info.reachable.fill(0);
		info.relocation_used.resize(RELOC_TABLE_MAX);
		for (int table = 0; table < int(RELOC_TABLE_MAX); table++) {
			info.relocation_used.write[table].resize(record.relocations[table].size());
			info.relocation_used.write[table].fill(0);
		}
		if (record.code[record.code.size() - 1] != GDScriptFunction::OPCODE_END) {
			return fail("Function '" + record.identity + "' has no bytecode terminator.");
		}
		for (int ip = 0; ip < record.code.size();) {
			info.boundaries.write[ip] = 1;
			info.instructions.push_back(ip);
			const int raw_opcode = record.code[ip];
			if (raw_opcode < 0 || raw_opcode >= GDScriptFunction::OPCODE_COUNT) {
				return fail("Unknown opcode in function '" + record.identity + "'.");
			}
			const GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(raw_opcode);
			const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(opcode);
			const int length = GDScriptFunction::get_instruction_size(record.code.ptr(), record.code.size(), ip);
			if (length <= 0 || (descriptor.instruction_size > 0 && descriptor.operand_kinds.count != descriptor.instruction_size - 1) ||
					(descriptor.instruction_size == 0 && record.code[ip + 1] > record.instruction_args_size) ||
					(opcode == GDScriptFunction::OPCODE_END && ip + length != record.code.size())) {
				return fail("Truncated or inconsistent opcode '" + String(descriptor.name) + "' in function '" + record.identity + "'.");
			}
			const int result = GDScriptFunction::get_result_operand(record.code.ptr(), record.code.size(), ip);
			if (result >= length || (result > 0 && GDScriptFunction::get_operand_kind(record.code.ptr(), record.code.size(), ip, result) != GDScriptFunction::OPERAND_FRAME_SLOT &&
						GDScriptFunction::get_operand_kind(record.code.ptr(), record.code.size(), ip, result) != GDScriptFunction::OPERAND_TYPED_FRAME_SLOT)) {
				return fail("Invalid result operand descriptor for opcode '" + String(descriptor.name) + "'.");
			}
			ip += length;
		}
	}
	return OK;
}

Error ModuleVerifier::validate_operands_and_tables() {
	for (FunctionInfo &info : function_info) {
		const FunctionRecord &record = *info.record;
		for (int ip : info.instructions) {
			const GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(record.code[ip]);
			const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(opcode);
			const int length = GDScriptFunction::get_instruction_size(record.code.ptr(), record.code.size(), ip);
			for (int word = 1; word < length; word++) {
				const int value = record.code[ip + word];
				const GDScriptFunction::OpcodeOperandKind kind = GDScriptFunction::get_operand_kind(record.code.ptr(), record.code.size(), ip, word);
				switch (kind) {
					case GDScriptFunction::OPERAND_CONSTANT_ADDRESS: {
						const uint32_t address = uint32_t(value);
						if ((address >> GDScriptFunction::ADDR_BITS) != GDScriptFunction::ADDR_TYPE_CONSTANT ||
								int(address & GDScriptFunction::ADDR_MASK) >= record.constants.size()) {
							return fail("Invalid constant operand in function '" + record.identity + "'.");
						}
					} break;
					case GDScriptFunction::OPERAND_NAME_INDEX:
						if (value < 0 || value >= record.global_names.size()) {
							return fail("Invalid name-table operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_FUNCTION_INDEX:
					case GDScriptFunction::OPERAND_NATIVE_API_RELOCATION: {
						const int table = int(descriptor.relocation_kind);
						if (table < 0 || table >= int(RELOC_TABLE_MAX) || value < 0 || value >= record.relocations[table].size()) {
							return fail("Invalid symbolic relocation operand in function '" + record.identity + "'.");
						}
						info.relocation_used.write[table].write[value] = 1;
					} break;
					case GDScriptFunction::OPERAND_ARGUMENT_COUNT:
						if (value < 0 || value > record.instruction_args_size || value > int(MAX_INSTRUCTION_ARGUMENTS)) {
							return fail("Invalid instruction argument count in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_STRUCT_FIELD_INDEX:
						if (value < 0 || value >= int(MAX_STRUCT_FIELDS)) {
							return fail("Invalid struct-field operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_STATIC_VARIABLE_INDEX:
						if (value < 0) {
							return fail("Invalid index operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_GLOBAL_INDEX:
						if (value < 0 || value >= record.global_names.size() || GDScriptLanguage::get_singleton() == nullptr ||
								!GDScriptLanguage::get_singleton()->get_global_map().has(record.global_names[value])) {
							return fail("Unresolved symbolic global operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_TYPE_ID:
						if (value < Variant::NIL || value >= Variant::VARIANT_MAX) {
							return fail("Invalid Variant type operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_OPERATOR:
						if (value < 0 || value >= Variant::OP_MAX) {
							return fail("Invalid operator operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_BOOLEAN:
						if (value != 0 && value != 1) {
							return fail("Invalid boolean operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_OPERATOR_FEEDBACK_INDEX:
						if (value < 0 || value >= record.operator_feedback_count) {
							return fail("Invalid operator-feedback operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_CALL_FEEDBACK_INDEX:
						if (value < 0 || value >= record.call_feedback_count) {
							return fail("Invalid call-feedback operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::OPERAND_NONE:
					case GDScriptFunction::OPERAND_VARIADIC_FRAME_SLOTS:
						return fail("Incomplete operand specification for opcode '" + String(descriptor.name) + "'.");
					case GDScriptFunction::OPERAND_FRAME_SLOT:
					case GDScriptFunction::OPERAND_TYPED_FRAME_SLOT:
					case GDScriptFunction::OPERAND_JUMP_TARGET:
					case GDScriptFunction::OPERAND_TYPE_METADATA:
					case GDScriptFunction::OPERAND_IMMEDIATE:
						break;
				}
			}
		}
		for (int table = 0; table < int(RELOC_TABLE_MAX); table++) {
			for (uint8_t used : info.relocation_used[table]) {
				if (!used) {
					return fail("Unused symbolic relocation in function '" + record.identity + "'.");
				}
			}
		}
	}
	return OK;
}

Error ModuleVerifier::validate_frames_and_temporaries() {
	for (const FunctionInfo &info : function_info) {
		const FunctionRecord &record = *info.record;
		HashMap<int, Variant::Type> typed_slots;
		for (const Pair<int, Variant::Type> &slot : record.temporary_slots) {
			if (slot.first < GDScriptFunction::FIXED_ADDRESSES_MAX || slot.first >= record.stack_size || slot.second <= Variant::NIL ||
					slot.second >= Variant::VARIANT_MAX || typed_slots.has(slot.first)) {
				return fail("Invalid typed temporary storage in function '" + record.identity + "'.");
			}
			typed_slots.insert(slot.first, slot.second);
		}
		const String *owner_identity = function_owners.getptr(record.identity);
		if (owner_identity == nullptr) {
			return fail("Function '" + record.identity + "' has no verified class owner.");
		}
		const int *owner_member_count_ptr = class_member_counts.getptr(*owner_identity);
		if (owner_member_count_ptr == nullptr) {
			return fail("Function '" + record.identity + "' refers to an unknown class owner.");
		}
		const int owner_member_count = *owner_member_count_ptr;
		int max_instruction_arguments = 0;
		HashSet<int> operator_feedback;
		HashSet<int> call_feedback;
		for (int ip : info.instructions) {
			const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(GDScriptFunction::Opcode(record.code[ip]));
			const int length = GDScriptFunction::get_instruction_size(record.code.ptr(), record.code.size(), ip);
			if (descriptor.instruction_size == 0) {
				max_instruction_arguments = MAX(max_instruction_arguments, record.code[ip + 1]);
			}
			for (int word = 1; word < length; word++) {
				const GDScriptFunction::OpcodeOperandKind kind = GDScriptFunction::get_operand_kind(record.code.ptr(), record.code.size(), ip, word);
				const int value = record.code[ip + word];
				if (kind == GDScriptFunction::OPERAND_OPERATOR_FEEDBACK_INDEX) {
					operator_feedback.insert(value);
					continue;
				}
				if (kind == GDScriptFunction::OPERAND_CALL_FEEDBACK_INDEX) {
					call_feedback.insert(value);
					continue;
				}
				if (kind != GDScriptFunction::OPERAND_FRAME_SLOT && kind != GDScriptFunction::OPERAND_TYPED_FRAME_SLOT) {
					continue;
				}
				const uint32_t address = uint32_t(value);
				const uint32_t mode = address >> GDScriptFunction::ADDR_BITS;
				const int index = address & GDScriptFunction::ADDR_MASK;
				switch (mode) {
					case GDScriptFunction::ADDR_TYPE_STACK:
						if (index >= record.stack_size || (kind == GDScriptFunction::OPERAND_TYPED_FRAME_SLOT && !typed_slots.has(index))) {
							return fail("Invalid stack-frame operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::ADDR_TYPE_CONSTANT:
						if (kind == GDScriptFunction::OPERAND_TYPED_FRAME_SLOT || index >= record.constants.size()) {
							return fail("Invalid constant-frame operand in function '" + record.identity + "'.");
						}
						break;
					case GDScriptFunction::ADDR_TYPE_MEMBER:
						if (kind == GDScriptFunction::OPERAND_TYPED_FRAME_SLOT || index >= owner_member_count) {
							return fail("Invalid member-frame operand in function '" + record.identity + "'.");
						}
						break;
					default:
						return fail("Unknown frame-address mode in function '" + record.identity + "'.");
				}
			}
		}
		if (max_instruction_arguments != record.instruction_args_size || int(operator_feedback.size()) != record.operator_feedback_count ||
				int(call_feedback.size()) != record.call_feedback_count) {
			return fail("Frame allocation metadata is not minimal or complete in function '" + record.identity + "'.");
		}
		if ((record.default_argument_count == 0 && !record.default_arguments.is_empty()) ||
				(record.default_argument_count > 0 && record.default_arguments.size() != record.default_argument_count + 1)) {
			return fail("Invalid default-argument table size in function '" + record.identity + "'.");
		}
	}
	return OK;
}

Error ModuleVerifier::validate_control_flow() {
	for (FunctionInfo &info : function_info) {
		const FunctionRecord &record = *info.record;
		for (int ip : info.instructions) {
			const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(GDScriptFunction::Opcode(record.code[ip]));
			const int length = GDScriptFunction::get_instruction_size(record.code.ptr(), record.code.size(), ip);
			int jump_count = 0;
			for (int word = 1; word < length; word++) {
				if (GDScriptFunction::get_operand_kind(record.code.ptr(), record.code.size(), ip, word) == GDScriptFunction::OPERAND_JUMP_TARGET) {
					const int target = record.code[ip + word];
					if (target < 0 || target >= info.boundaries.size() || !info.boundaries[target]) {
						return fail("Jump target is not an instruction boundary in function '" + record.identity + "'.");
					}
					jump_count++;
				}
			}
			if ((descriptor.control_flow_kind == GDScriptFunction::CONTROL_FLOW_BRANCH && jump_count != 1) ||
					(descriptor.control_flow_kind == GDScriptFunction::CONTROL_FLOW_JUMP && jump_count != 1) ||
					(descriptor.control_flow_kind != GDScriptFunction::CONTROL_FLOW_BRANCH && descriptor.control_flow_kind != GDScriptFunction::CONTROL_FLOW_JUMP && jump_count != 0) ||
					(descriptor.control_flow_kind == GDScriptFunction::CONTROL_FLOW_DEFAULT_ARGUMENT && (ip != 0 || record.default_argument_count == 0))) {
				return fail("Opcode control-flow shape is inconsistent in function '" + record.identity + "'.");
			}
			if (descriptor.control_flow_kind != GDScriptFunction::CONTROL_FLOW_RETURN && descriptor.control_flow_kind != GDScriptFunction::CONTROL_FLOW_TERMINATE &&
					descriptor.control_flow_kind != GDScriptFunction::CONTROL_FLOW_JUMP && descriptor.control_flow_kind != GDScriptFunction::CONTROL_FLOW_DEFAULT_ARGUMENT &&
					ip + length >= record.code.size()) {
				return fail("Control flow falls past the end of function '" + record.identity + "'.");
			}
		}
		for (int target : record.default_arguments) {
			if (target < 0 || target >= info.boundaries.size() || !info.boundaries[target]) {
				return fail("Default-argument target is not an instruction boundary in function '" + record.identity + "'.");
			}
		}

		Vector<int> worklist;
		worklist.push_back(0);
		for (int cursor = 0; cursor < worklist.size(); cursor++) {
			if (cursor > info.instructions.size() * 2 + record.default_arguments.size()) {
				return fail("Control-flow work limit exceeded in function '" + record.identity + "'.");
			}
			const int ip = worklist[cursor];
			if (info.reachable[ip]) {
				continue;
			}
			info.reachable.write[ip] = 1;
			const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(GDScriptFunction::Opcode(record.code[ip]));
			const int length = GDScriptFunction::get_instruction_size(record.code.ptr(), record.code.size(), ip);
			if (descriptor.control_flow_kind == GDScriptFunction::CONTROL_FLOW_DEFAULT_ARGUMENT) {
				for (int target : record.default_arguments) {
					worklist.push_back(target);
				}
				continue;
			}
			for (int word = 1; word < length; word++) {
				if (GDScriptFunction::get_operand_kind(record.code.ptr(), record.code.size(), ip, word) == GDScriptFunction::OPERAND_JUMP_TARGET) {
					worklist.push_back(record.code[ip + word]);
				}
			}
			if (descriptor.control_flow_kind == GDScriptFunction::CONTROL_FLOW_NEXT || descriptor.control_flow_kind == GDScriptFunction::CONTROL_FLOW_BRANCH ||
					descriptor.control_flow_kind == GDScriptFunction::CONTROL_FLOW_SUSPEND) {
				worklist.push_back(ip + length);
			}
		}
	}
	return OK;
}

static bool verifier_is_direct_int_operator(Variant::Operator p_operator) {
	switch (p_operator) {
		case Variant::OP_ADD:
		case Variant::OP_SUBTRACT:
		case Variant::OP_MULTIPLY:
		case Variant::OP_POWER:
		case Variant::OP_NEGATE:
		case Variant::OP_POSITIVE:
		case Variant::OP_SHIFT_LEFT:
		case Variant::OP_SHIFT_RIGHT:
		case Variant::OP_BIT_OR:
		case Variant::OP_BIT_AND:
		case Variant::OP_BIT_XOR:
		case Variant::OP_BIT_NEGATE:
		case Variant::OP_EQUAL:
		case Variant::OP_NOT_EQUAL:
		case Variant::OP_LESS:
		case Variant::OP_LESS_EQUAL:
		case Variant::OP_GREATER:
		case Variant::OP_GREATER_EQUAL:
			return true;
		default:
			return false;
	}
}

static bool verifier_is_direct_float_operator(Variant::Operator p_operator) {
	switch (p_operator) {
		case Variant::OP_ADD:
		case Variant::OP_SUBTRACT:
		case Variant::OP_MULTIPLY:
		case Variant::OP_DIVIDE:
		case Variant::OP_POWER:
		case Variant::OP_NEGATE:
		case Variant::OP_POSITIVE:
		case Variant::OP_EQUAL:
		case Variant::OP_NOT_EQUAL:
		case Variant::OP_LESS:
		case Variant::OP_LESS_EQUAL:
		case Variant::OP_GREATER:
		case Variant::OP_GREATER_EQUAL:
			return true;
		default:
			return false;
	}
}

static bool verifier_is_direct_comparison_operator(Variant::Operator p_operator) {
	return p_operator == Variant::OP_EQUAL || p_operator == Variant::OP_NOT_EQUAL || p_operator == Variant::OP_LESS ||
			p_operator == Variant::OP_LESS_EQUAL || p_operator == Variant::OP_GREATER || p_operator == Variant::OP_GREATER_EQUAL;
}

static bool verifier_is_direct_math_type(Variant::Type p_type) {
	return p_type == Variant::VECTOR2 || p_type == Variant::VECTOR3 || p_type == Variant::COLOR;
}

static bool verifier_is_direct_large_value_type(Variant::Type p_type) {
	return p_type == Variant::TRANSFORM2D || p_type == Variant::AABB || p_type == Variant::BASIS || p_type == Variant::TRANSFORM3D || p_type == Variant::PROJECTION;
}

static bool verifier_is_direct_native_value_type(Variant::Type p_type) {
	return verifier_is_direct_math_type(p_type) || verifier_is_direct_large_value_type(p_type);
}

static bool verifier_is_direct_math_operator(Variant::Operator p_operator, Variant::Type p_left_type, Variant::Type p_right_type, Variant::Type p_result_type) {
	if (verifier_is_direct_large_value_type(p_left_type)) {
		if (p_operator == Variant::OP_EQUAL || p_operator == Variant::OP_NOT_EQUAL) {
			return p_right_type == p_left_type && p_result_type == Variant::BOOL;
		}
		if (p_operator == Variant::OP_MULTIPLY && p_result_type == p_left_type) {
			return p_left_type == Variant::PROJECTION ? p_right_type == Variant::PROJECTION :
					p_left_type != Variant::AABB && (p_right_type == p_left_type || p_right_type == Variant::INT || p_right_type == Variant::FLOAT);
		}
		return p_operator == Variant::OP_DIVIDE && p_left_type != Variant::AABB && p_left_type != Variant::PROJECTION &&
				p_result_type == p_left_type && (p_right_type == Variant::INT || p_right_type == Variant::FLOAT);
	}
	if (p_operator == Variant::OP_NEGATE || p_operator == Variant::OP_POSITIVE) {
		return verifier_is_direct_math_type(p_left_type) && p_right_type == Variant::NIL && p_result_type == p_left_type;
	}
	if (p_operator == Variant::OP_EQUAL || p_operator == Variant::OP_NOT_EQUAL) {
		return verifier_is_direct_math_type(p_left_type) && p_right_type == p_left_type && p_result_type == Variant::BOOL;
	}
	if (p_operator == Variant::OP_ADD || p_operator == Variant::OP_SUBTRACT) {
		return verifier_is_direct_math_type(p_left_type) && p_right_type == p_left_type && p_result_type == p_left_type;
	}
	if (p_operator != Variant::OP_MULTIPLY && p_operator != Variant::OP_DIVIDE) {
		return false;
	}
	if (verifier_is_direct_math_type(p_left_type) && p_result_type == p_left_type) {
		return p_right_type == p_left_type || p_right_type == Variant::INT || p_right_type == Variant::FLOAT;
	}
	return p_operator == Variant::OP_MULTIPLY && (p_left_type == Variant::INT || p_left_type == Variant::FLOAT) &&
			verifier_is_direct_math_type(p_right_type) && p_result_type == p_right_type;
}

static int verifier_math_component_count(Variant::Type p_type) {
	switch (p_type) {
		case Variant::VECTOR2:
			return 2;
		case Variant::VECTOR3:
			return 3;
		case Variant::COLOR:
			return 4;
		default:
			return 0;
	}
}

static Variant::Type verifier_type_adjust_type(GDScriptFunction::Opcode p_opcode) {
	switch (p_opcode) {
#define GDSCRIPT_TYPE_ADJUST_CASE(m_type) \
	case GDScriptFunction::OPCODE_TYPE_ADJUST_##m_type: \
		return Variant::m_type;
		GDSCRIPT_TYPE_ADJUST_CASE(BOOL)
		GDSCRIPT_TYPE_ADJUST_CASE(INT)
		GDSCRIPT_TYPE_ADJUST_CASE(FLOAT)
		GDSCRIPT_TYPE_ADJUST_CASE(STRING)
		GDSCRIPT_TYPE_ADJUST_CASE(VECTOR2)
		GDSCRIPT_TYPE_ADJUST_CASE(VECTOR2I)
		GDSCRIPT_TYPE_ADJUST_CASE(RECT2)
		GDSCRIPT_TYPE_ADJUST_CASE(RECT2I)
		GDSCRIPT_TYPE_ADJUST_CASE(VECTOR3)
		GDSCRIPT_TYPE_ADJUST_CASE(VECTOR3I)
		GDSCRIPT_TYPE_ADJUST_CASE(TRANSFORM2D)
		GDSCRIPT_TYPE_ADJUST_CASE(VECTOR4)
		GDSCRIPT_TYPE_ADJUST_CASE(VECTOR4I)
		GDSCRIPT_TYPE_ADJUST_CASE(PLANE)
		GDSCRIPT_TYPE_ADJUST_CASE(QUATERNION)
		GDSCRIPT_TYPE_ADJUST_CASE(AABB)
		GDSCRIPT_TYPE_ADJUST_CASE(BASIS)
		GDSCRIPT_TYPE_ADJUST_CASE(TRANSFORM3D)
		GDSCRIPT_TYPE_ADJUST_CASE(PROJECTION)
		GDSCRIPT_TYPE_ADJUST_CASE(COLOR)
		GDSCRIPT_TYPE_ADJUST_CASE(STRING_NAME)
		GDSCRIPT_TYPE_ADJUST_CASE(NODE_PATH)
		GDSCRIPT_TYPE_ADJUST_CASE(RID)
		GDSCRIPT_TYPE_ADJUST_CASE(OBJECT)
		GDSCRIPT_TYPE_ADJUST_CASE(CALLABLE)
		GDSCRIPT_TYPE_ADJUST_CASE(SIGNAL)
		GDSCRIPT_TYPE_ADJUST_CASE(DICTIONARY)
		GDSCRIPT_TYPE_ADJUST_CASE(ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_BYTE_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_INT32_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_INT64_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_FLOAT32_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_FLOAT64_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_STRING_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_VECTOR2_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_VECTOR3_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_COLOR_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(PACKED_VECTOR4_ARRAY)
		GDSCRIPT_TYPE_ADJUST_CASE(STRUCT)
#undef GDSCRIPT_TYPE_ADJUST_CASE
		default:
			return Variant::NIL;
	}
}

Error ModuleVerifier::validate_typed_constraints() {
	for (const FunctionInfo &info : function_info) {
		const FunctionRecord &record = *info.record;
		HashMap<int, Variant::Type> typed_slots;
		for (const Pair<int, Variant::Type> &slot : record.temporary_slots) {
			typed_slots.insert(slot.first, slot.second);
		}
		for (int ip : info.instructions) {
			const GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(record.code[ip]);
			const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(opcode);
			if (descriptor.type_constraints < GDScriptFunction::TYPE_CONSTRAINT_NONE || descriptor.type_constraints > GDScriptFunction::TYPE_CONSTRAINT_ITERATOR) {
				return fail("Unknown opcode type constraint in function '" + record.identity + "'.");
			}
			switch (opcode) {
				case GDScriptFunction::OPCODE_OPERATOR_INT:
					if (!verifier_is_direct_int_operator(Variant::Operator(record.code[ip + 4]))) {
						return fail("Invalid direct integer operator in function '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_OPERATOR_FLOAT:
					if (!verifier_is_direct_float_operator(Variant::Operator(record.code[ip + 4]))) {
						return fail("Invalid direct floating-point operator in function '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_JUMP_COMPARE_INT:
				case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT:
					if (!verifier_is_direct_comparison_operator(Variant::Operator(record.code[ip + 3]))) {
						return fail("Invalid direct comparison operator in function '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_OPERATOR_MATH: {
					const int metadata = record.code[ip + 5];
					const Variant::Operator op = GDScriptFunction::get_math_operator(metadata);
					const Variant::Type left = GDScriptFunction::get_math_left_type(metadata);
					const Variant::Type right = GDScriptFunction::get_math_right_type(metadata);
					const Variant::Type result = GDScriptFunction::get_math_result_type(metadata);
					if (op < 0 || op >= Variant::OP_MAX || left < Variant::NIL || left >= Variant::VARIANT_MAX || right < Variant::NIL ||
							right >= Variant::VARIANT_MAX || result < Variant::NIL || result >= Variant::VARIANT_MAX ||
							!verifier_is_direct_math_operator(op, left, right, result)) {
						return fail("Invalid direct math metadata in function '" + record.identity + "'.");
					}
					const Symbol &symbol = record.relocations[RELOC_OPERATOR][record.code[ip + 4]];
					if (symbol.x != op || symbol.y != left || symbol.z != right) {
						return fail("Direct math metadata disagrees with its relocation in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_GET_MATH_COMPONENT:
				case GDScriptFunction::OPCODE_SET_MATH_COMPONENT: {
					const Variant::Type type = GDScriptFunction::get_math_component_type(record.code[ip + 3]);
					const int component = GDScriptFunction::get_math_component_index(record.code[ip + 3]);
					if (component < 0 || component >= verifier_math_component_count(type)) {
						return fail("Invalid math-component metadata in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_MATH_LENGTH:
					if (record.code[ip + 3] != Variant::VECTOR2 && record.code[ip + 3] != Variant::VECTOR3) {
						return fail("Invalid math length type in function '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_ASSIGN_MATH:
					if (!verifier_is_direct_native_value_type(Variant::Type(record.code[ip + 3]))) {
						return fail("Invalid direct native value assignment in function '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_RETURN_TYPED_BUILTIN:
					if (record.return_type.kind != GDScriptDataType::BUILTIN || record.return_type.builtin_type != uint32_t(record.code[ip + 2])) {
						return fail("Typed return does not match the function signature in '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT:
					if (record.return_type.kind != GDScriptDataType::BUILTIN || record.return_type.builtin_type != Variant::STRUCT) {
						return fail("Struct return does not match the function signature in '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_RETURN_TYPED_ARRAY:
					if (record.return_type.kind != GDScriptDataType::BUILTIN || record.return_type.builtin_type != Variant::ARRAY) {
						return fail("Array return does not match the function signature in '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_RETURN_TYPED_DICTIONARY:
					if (record.return_type.kind != GDScriptDataType::BUILTIN || record.return_type.builtin_type != Variant::DICTIONARY) {
						return fail("Dictionary return does not match the function signature in '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_RETURN_TYPED_NATIVE:
					if (record.return_type.kind != GDScriptDataType::NATIVE) {
						return fail("Native return does not match the function signature in '" + record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPCODE_RETURN_TYPED_SCRIPT:
					if (record.return_type.kind != GDScriptDataType::SCRIPT && record.return_type.kind != GDScriptDataType::GDSCRIPT) {
						return fail("Script return does not match the function signature in '" + record.identity + "'.");
					}
					break;
				default: {
					const Variant::Type adjusted_type = verifier_type_adjust_type(opcode);
					if (adjusted_type != Variant::NIL) {
						const uint32_t address = uint32_t(record.code[ip + 1]);
						const int slot = address & GDScriptFunction::ADDR_MASK;
						const Variant::Type *actual_type = typed_slots.getptr(slot);
						if ((address >> GDScriptFunction::ADDR_BITS) != GDScriptFunction::ADDR_TYPE_STACK || actual_type == nullptr || *actual_type != adjusted_type) {
							return fail("Type-adjust opcode does not match its typed frame slot in function '" + record.identity + "'.");
						}
					}
				} break;
			}
		}
	}
	return OK;
}

static int verifier_variadic_semantic_count_offset(const FunctionRecord &p_record, int p_ip) {
	const int length = GDScriptFunction::get_instruction_size(p_record.code.ptr(), p_record.code.size(), p_ip);
	for (int word = 2; word < length; word++) {
		if (GDScriptFunction::get_operand_kind(p_record.code.ptr(), p_record.code.size(), p_ip, word) == GDScriptFunction::OPERAND_ARGUMENT_COUNT) {
			return p_ip + word;
		}
	}
	return -1;
}

static bool verifier_valid_call_count(int p_actual, int p_declared, int p_default_count, bool p_vararg) {
	return p_actual >= p_declared - p_default_count && (p_vararg || p_actual <= p_declared);
}

Error ModuleVerifier::validate_operations() {
	int maximum_struct_fields = 0;
	for (const KeyValue<String, Ref<StructLayout>> &entry : verified_layouts) {
		maximum_struct_fields = MAX(maximum_struct_fields, entry.value->get_field_count());
	}
	for (const FunctionInfo &info : function_info) {
		const FunctionRecord &record = *info.record;
		for (int ip : info.instructions) {
			const GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(record.code[ip]);
			const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(opcode);
			if (opcode == GDScriptFunction::OPCODE_LINE && record.code[ip + 1] < 0) {
				return fail("Negative source line in function '" + record.identity + "'.");
			}
			if (opcode == GDScriptFunction::OPCODE_GET_STRUCT_FIELD || opcode == GDScriptFunction::OPCODE_SET_STRUCT_FIELD) {
				if (maximum_struct_fields == 0 || record.code[ip + 3] >= maximum_struct_fields) {
					return fail("Struct-field operation cannot match any module layout in function '" + record.identity + "'.");
				}
			}
			if (descriptor.instruction_size != 0) {
				continue;
			}

			const int frame_words = record.code[ip + 1];
			const int semantic_offset = verifier_variadic_semantic_count_offset(record, ip);
			if (semantic_offset < 0) {
				return fail("Variadic opcode has no semantic argument count in function '" + record.identity + "'.");
			}
			const int argument_count = record.code[semantic_offset];
			int expected_frame_words = -1;
			switch (opcode) {
				case GDScriptFunction::OPCODE_CONSTRUCT:
				case GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED:
				case GDScriptFunction::OPCODE_CONSTRUCT_ARRAY:
				case GDScriptFunction::OPCODE_CALL_UTILITY:
				case GDScriptFunction::OPCODE_CALL_UTILITY_VALIDATED:
				case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY:
				case GDScriptFunction::OPCODE_CALL_SELF_BASE:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN:
					expected_frame_words = argument_count + 1;
					break;
				case GDScriptFunction::OPCODE_CONSTRUCT_STRUCT:
				case GDScriptFunction::OPCODE_CONSTRUCT_TYPED_ARRAY:
				case GDScriptFunction::OPCODE_CALL:
				case GDScriptFunction::OPCODE_CALL_RETURN:
				case GDScriptFunction::OPCODE_CALL_ASYNC:
				case GDScriptFunction::OPCODE_CALL_BUILTIN_TYPE_VALIDATED:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN:
					expected_frame_words = argument_count + 2;
					break;
				case GDScriptFunction::OPCODE_CONSTRUCT_DICTIONARY:
					expected_frame_words = argument_count * 2 + 1;
					break;
				case GDScriptFunction::OPCODE_CONSTRUCT_TYPED_DICTIONARY:
					expected_frame_words = argument_count * 2 + 3;
					break;
				case GDScriptFunction::OPCODE_CALL_BUILTIN_STATIC:
					// This opcode stores its argument count at the end of the
					// suffix instead of at the first suffix word.
					expected_frame_words = argument_count + 1;
					break;
				case GDScriptFunction::OPCODE_CREATE_LAMBDA:
				case GDScriptFunction::OPCODE_CREATE_SELF_LAMBDA:
					expected_frame_words = argument_count + 1;
					break;
				default:
					return fail("Unknown variadic opcode semantics in function '" + record.identity + "'.");
			}
			if (argument_count < 0 || expected_frame_words != frame_words) {
				return fail("Variadic frame shape disagrees with its argument count in function '" + record.identity + "'.");
			}

			switch (opcode) {
				case GDScriptFunction::OPCODE_CONSTRUCT: {
					const Variant::Type type = Variant::Type(record.code[semantic_offset + 1]);
					bool matching_constructor = argument_count == 0;
					for (int constructor = 0; constructor < Variant::get_constructor_count(type); constructor++) {
						matching_constructor |= Variant::get_constructor_argument_count(type, constructor) == argument_count;
					}
					if (type <= Variant::NIL || type >= Variant::VARIANT_MAX || !matching_constructor) {
						return fail("Invalid dynamic constructor call in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CONSTRUCT_VALIDATED: {
					const Symbol &symbol = record.relocations[RELOC_CONSTRUCTOR][record.code[semantic_offset + 1]];
					if (Variant::get_constructor_argument_count(Variant::Type(symbol.x), symbol.y) != argument_count) {
						return fail("Validated constructor argument count mismatch in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CONSTRUCT_STRUCT: {
					const int constant_word = ip + 1 + frame_words;
					const uint32_t address = uint32_t(record.code[constant_word]);
					const ConstantData &constant = record.constants[address & GDScriptFunction::ADDR_MASK];
					Variant decoded;
					int used = 0;
					if (constant.kind != CONSTANT_VARIANT || decode_variant(decoded, constant.encoded.ptr(), constant.encoded.size(), &used, false) != OK ||
							used != constant.encoded.size() || decoded.get_type() != Variant::STRUCT || !StructValue(decoded).is_valid() ||
							(argument_count != 0 && StructValue(decoded).get_field_count() != argument_count)) {
						return fail("Invalid struct constructor layout or argument count in function '" + record.identity + "'.");
					}
					const Ref<StructLayout> layout = StructValue(decoded).get_layout();
					const Ref<StructLayout> *verified_layout = verified_layouts.getptr(String(layout->get_type_identifier()));
					if (verified_layout != nullptr && !(*verified_layout)->is_compatible(layout)) {
						return fail("Struct constructor layout disagrees with the verified module schema in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CALL_UTILITY_VALIDATED: {
					const Symbol &symbol = record.relocations[RELOC_UTILITY][record.code[semantic_offset + 1]];
					if (Variant::is_utility_function_vararg(symbol.name) || Variant::get_utility_function_argument_count(symbol.name) != argument_count) {
						return fail("Validated utility call argument count mismatch in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CALL_GDSCRIPT_UTILITY: {
					const Symbol &symbol = record.relocations[RELOC_GDSCRIPT_UTILITY][record.code[semantic_offset + 1]];
					const MethodInfo method = GDScriptUtilityFunctions::get_function_info(symbol.name);
					if (!verifier_valid_call_count(argument_count, GDScriptUtilityFunctions::get_function_argument_count(symbol.name), method.default_arguments.size(),
								GDScriptUtilityFunctions::is_function_vararg(symbol.name))) {
						return fail("GDScript utility call argument count mismatch in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CALL_BUILTIN_TYPE_VALIDATED: {
					const Symbol &symbol = record.relocations[RELOC_BUILTIN_METHOD][record.code[semantic_offset + 1]];
					if (Variant::is_builtin_method_vararg(Variant::Type(symbol.x), symbol.name) ||
							Variant::get_builtin_method_argument_count(Variant::Type(symbol.x), symbol.name) != argument_count) {
						return fail("Validated builtin call argument count mismatch in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CALL_UTILITY: {
					const StringName &name = record.global_names[record.code[semantic_offset + 1]];
					if (!Variant::has_utility_function(name)) {
						return fail("Unknown utility call in function '" + record.identity + "'.");
					}
					const MethodInfo method = Variant::get_utility_function_info(name);
					if (!verifier_valid_call_count(argument_count, method.arguments.size(), method.default_arguments.size(), Variant::is_utility_function_vararg(name))) {
						return fail("Utility call argument count mismatch in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CALL_BUILTIN_STATIC: {
					const Variant::Type type = Variant::Type(record.code[semantic_offset - 2]);
					const StringName &name = record.global_names[record.code[semantic_offset - 1]];
					if (type < Variant::NIL || type >= Variant::VARIANT_MAX || !Variant::has_builtin_method(type, name) || !Variant::is_builtin_method_static(type, name)) {
						return fail("Invalid static builtin call in function '" + record.identity + "'.");
					}
					const MethodInfo method = Variant::get_builtin_method_info(type, name);
					if (!verifier_valid_call_count(argument_count, method.arguments.size(), method.default_arguments.size(), Variant::is_builtin_method_vararg(type, name))) {
						return fail("Static builtin call argument count mismatch in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN: {
					const int relocation_offset = opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC ? semantic_offset - 1 : semantic_offset + 1;
					const Symbol &symbol = record.relocations[RELOC_METHOD_BIND][record.code[relocation_offset]];
					MethodBind *method = ClassDB::get_method(symbol.owner, symbol.name);
					const bool validated = opcode != GDScriptFunction::OPCODE_CALL_METHOD_BIND && opcode != GDScriptFunction::OPCODE_CALL_METHOD_BIND_RET &&
							opcode != GDScriptFunction::OPCODE_CALL_NATIVE_STATIC;
					const bool static_call = opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC ||
							opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN;
					if (method == nullptr || method->is_static() != static_call || (validated ? (method->is_vararg() || method->get_argument_count() != argument_count) :
								!verifier_valid_call_count(argument_count, method->get_argument_count(), method->get_default_argument_count(), method->is_vararg()))) {
						return fail("Native call argument count mismatch in function '" + record.identity + "'.");
					}
				} break;
				case GDScriptFunction::OPCODE_CREATE_LAMBDA:
				case GDScriptFunction::OPCODE_CREATE_SELF_LAMBDA: {
					const Symbol &symbol = record.relocations[RELOC_LAMBDA][record.code[semantic_offset + 1]];
					const String *owner_identity = function_owners.getptr(symbol.name);
					const ClassRecord *const *owner = owner_identity == nullptr ? nullptr : classes.getptr(*owner_identity);
					const LambdaRecord *lambda_record = nullptr;
					if (owner != nullptr) {
						for (const LambdaRecord &lambda : (*owner)->lambdas) {
							if (lambda.identity == symbol.name) {
								lambda_record = &lambda;
								break;
							}
						}
					}
					if (lambda_record == nullptr || lambda_record->capture_count != argument_count ||
							lambda_record->use_self != (opcode == GDScriptFunction::OPCODE_CREATE_SELF_LAMBDA)) {
						return fail("Lambda construction metadata mismatch in function '" + record.identity + "'.");
					}
				} break;
				default:
					break;
			}
		}
	}
	return OK;
}

static bool verifier_symbol_has_zero_numbers(const Symbol &p_symbol) {
	return p_symbol.x == 0 && p_symbol.y == 0 && p_symbol.z == 0 && p_symbol.hash == 0;
}

Error ModuleVerifier::validate_relocations() {
	for (const FunctionRecord &record : module.functions) {
		for (const Symbol &symbol : record.relocations[RELOC_OPERATOR]) {
			if (symbol.x < 0 || symbol.x >= Variant::OP_MAX || symbol.y < 0 || symbol.y >= Variant::VARIANT_MAX || symbol.z < 0 ||
					symbol.z >= Variant::VARIANT_MAX || symbol.hash != 0 || !symbol.name.is_empty() || !symbol.owner.is_empty() ||
					Variant::get_validated_operator_evaluator(Variant::Operator(symbol.x), Variant::Type(symbol.y), Variant::Type(symbol.z)) == nullptr) {
				return fail("Invalid operator relocation in function '" + record.identity + "'.");
			}
		}
		auto validate_member_table = [&](RelocationTable p_table, bool p_setter) -> Error {
			for (const Symbol &symbol : record.relocations[p_table]) {
				const bool found = symbol.x >= 0 && symbol.x < Variant::VARIANT_MAX && symbol.y == 0 && symbol.z == 0 && symbol.hash == 0 &&
						symbol.owner.is_empty() && !symbol.name.is_empty() && (p_setter ?
								Variant::get_member_validated_setter(Variant::Type(symbol.x), symbol.name) != nullptr :
								Variant::get_member_validated_getter(Variant::Type(symbol.x), symbol.name) != nullptr);
				if (!found) {
					return fail("Invalid named-member relocation in function '" + record.identity + "'.");
				}
			}
			return OK;
		};
		if (validate_member_table(RELOC_SETTER, true) != OK || validate_member_table(RELOC_GETTER, false) != OK) {
			return ERR_INVALID_DATA;
		}
		for (int table = int(RELOC_KEYED_SETTER); table <= int(RELOC_INDEXED_GETTER); table++) {
			for (const Symbol &symbol : record.relocations[table]) {
				bool found = symbol.x >= 0 && symbol.x < Variant::VARIANT_MAX && symbol.y == 0 && symbol.z == 0 && symbol.hash == 0 &&
						symbol.name.is_empty() && symbol.owner.is_empty();
				if (found) {
					switch (table) {
						case RELOC_KEYED_SETTER:
							found = Variant::get_member_validated_keyed_setter(Variant::Type(symbol.x)) != nullptr;
							break;
						case RELOC_KEYED_GETTER:
							found = Variant::get_member_validated_keyed_getter(Variant::Type(symbol.x)) != nullptr;
							break;
						case RELOC_INDEXED_SETTER:
							found = Variant::get_member_validated_indexed_setter(Variant::Type(symbol.x)) != nullptr;
							break;
						case RELOC_INDEXED_GETTER:
							found = Variant::get_member_validated_indexed_getter(Variant::Type(symbol.x)) != nullptr;
							break;
					}
				}
				if (!found) {
					return fail("Invalid keyed/indexed relocation in function '" + record.identity + "'.");
				}
			}
		}
		for (const Symbol &symbol : record.relocations[RELOC_BUILTIN_METHOD]) {
			if (symbol.x < 0 || symbol.x >= Variant::VARIANT_MAX || symbol.y != 0 || symbol.z != 0 || symbol.name.is_empty() || !symbol.owner.is_empty() ||
					!Variant::has_builtin_method(Variant::Type(symbol.x), symbol.name) ||
					Variant::get_builtin_method_hash(Variant::Type(symbol.x), symbol.name) != symbol.hash ||
					Variant::get_validated_builtin_method(Variant::Type(symbol.x), symbol.name) == nullptr) {
				return fail("Invalid builtin-method relocation or API hash in function '" + record.identity + "'.");
			}
		}
		for (const Symbol &symbol : record.relocations[RELOC_CONSTRUCTOR]) {
			if (symbol.z != 0 || !symbol.name.is_empty() || !symbol.owner.is_empty() || !validate_constructor_symbol(symbol) ||
					Variant::get_validated_constructor(Variant::Type(symbol.x), symbol.y) == nullptr) {
				return fail("Invalid constructor relocation or API hash in function '" + record.identity + "'.");
			}
		}
		for (const Symbol &symbol : record.relocations[RELOC_UTILITY]) {
			if (symbol.x != 0 || symbol.y != 0 || symbol.z != 0 || symbol.name.is_empty() || !symbol.owner.is_empty() ||
					!Variant::has_utility_function(symbol.name) || Variant::get_utility_function_hash(symbol.name) != symbol.hash ||
					Variant::get_validated_utility_function(symbol.name) == nullptr) {
				return fail("Invalid utility relocation or API hash in function '" + record.identity + "'.");
			}
		}
		for (const Symbol &symbol : record.relocations[RELOC_GDSCRIPT_UTILITY]) {
			if (symbol.x != 0 || symbol.y != 0 || symbol.z != 0 || symbol.name.is_empty() || !symbol.owner.is_empty() ||
					GDScriptUtilityFunctions::get_function(symbol.name) == nullptr ||
					GDScriptUtilityFunctions::get_function_info(symbol.name).get_compatibility_hash() != symbol.hash) {
				return fail("Invalid GDScript utility relocation or API hash in function '" + record.identity + "'.");
			}
		}
		for (const Symbol &symbol : record.relocations[RELOC_METHOD_BIND]) {
			MethodBind *method = ClassDB::get_method(symbol.owner, symbol.name);
			if (symbol.x != 0 || symbol.y != 0 || symbol.z != 0 || symbol.name.is_empty() || symbol.owner.is_empty() || method == nullptr || method->get_hash() != symbol.hash) {
				return fail("Invalid native method relocation or API hash in function '" + record.identity + "'.");
			}
		}
		for (const Symbol &symbol : record.relocations[RELOC_LAMBDA]) {
			if (!verifier_symbol_has_zero_numbers(symbol) || symbol.name.is_empty() || !symbol.owner.is_empty() || !functions.has(symbol.name)) {
				return fail("Invalid function relocation in function '" + record.identity + "'.");
			}
		}
	}
	return OK;
}

Error ModuleVerifier::validate_final_limits() {
	if (metadata_entries > MAX_TOTAL_METADATA_ENTRIES || constant_bytes > MAX_TOTAL_CONSTANT_BYTES || total_code_words > MAX_TOTAL_CODE_WORDS ||
			function_info.size() != module.functions.size()) {
		return fail("Compiled module exceeds a final verification limit.");
	}
	for (const KeyValue<String, Ref<StructLayout>> &entry : verified_layouts) {
		if (entry.value.is_null() || !entry.value->is_finalized() || entry.value->get_native_size() > MAX_BLOB_SIZE ||
				entry.value->get_native_alignment() == 0 || entry.value->get_native_alignment() > MAX_BLOB_SIZE) {
			return fail("Struct layout exceeds native storage limits.");
		}
	}
	for (const FunctionRecord &record : module.functions) {
		if (record.fingerprint != get_portable_function_fingerprint(record)) {
			return fail("Portable function fingerprint mismatch for '" + record.identity + "'.");
		}
	}
	return OK;
}

Error ModuleVerifier::verify(String *r_error) {
	if (r_error != nullptr) {
		r_error->clear();
	}
	Error result = index_and_verify_metadata();
	if (result == OK) {
		result = decode_instructions();
	}
	if (result == OK) {
		result = validate_operands_and_tables();
	}
	if (result == OK) {
		result = validate_frames_and_temporaries();
	}
	if (result == OK) {
		result = validate_control_flow();
	}
	if (result == OK) {
		result = validate_typed_constraints();
	}
	if (result == OK) {
		result = validate_operations();
	}
	if (result == OK) {
		result = validate_relocations();
	}
	if (result == OK) {
		result = validate_final_limits();
	}
	if (result != OK && r_error != nullptr) {
		*r_error = error.is_empty() ? String("Compiled module verification failed.") : error;
	}
	return result;
}

bool Internals::validate_relocations(const FunctionRecord &p_record, GDScriptFunction *p_function, const HashMap<String, GDScriptFunction *> &p_functions) {
	if (p_record.relocations.size() != RELOC_TABLE_MAX ||
			p_record.relocations[RELOC_OPERATOR].size() != p_function->operator_funcs.size() ||
			p_record.relocations[RELOC_SETTER].size() != p_function->setters.size() ||
			p_record.relocations[RELOC_GETTER].size() != p_function->getters.size() ||
			p_record.relocations[RELOC_KEYED_SETTER].size() != p_function->keyed_setters.size() ||
			p_record.relocations[RELOC_KEYED_GETTER].size() != p_function->keyed_getters.size() ||
			p_record.relocations[RELOC_INDEXED_SETTER].size() != p_function->indexed_setters.size() ||
			p_record.relocations[RELOC_INDEXED_GETTER].size() != p_function->indexed_getters.size() ||
			p_record.relocations[RELOC_BUILTIN_METHOD].size() != p_function->builtin_methods.size() ||
			p_record.relocations[RELOC_CONSTRUCTOR].size() != p_function->constructors.size() ||
			p_record.relocations[RELOC_UTILITY].size() != p_function->utilities.size() ||
			p_record.relocations[RELOC_GDSCRIPT_UTILITY].size() != p_function->gds_utilities.size() ||
			p_record.relocations[RELOC_METHOD_BIND].size() != p_function->methods.size() ||
			p_record.relocations[RELOC_LAMBDA].size() != p_function->lambdas.size()) {
		return false;
	}

	for (int i = 0; i < p_function->operator_funcs.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_OPERATOR][i];
		if (s.x < 0 || s.x >= Variant::OP_MAX || s.y < 0 || s.y >= Variant::VARIANT_MAX || s.z < 0 || s.z >= Variant::VARIANT_MAX ||
				Variant::get_validated_operator_evaluator(Variant::Operator(s.x), Variant::Type(s.y), Variant::Type(s.z)) != p_function->operator_funcs[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->setters.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_SETTER][i];
		if (s.x < 0 || s.x >= Variant::VARIANT_MAX || Variant::get_member_validated_setter(Variant::Type(s.x), s.name) != p_function->setters[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->getters.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_GETTER][i];
		if (s.x < 0 || s.x >= Variant::VARIANT_MAX || Variant::get_member_validated_getter(Variant::Type(s.x), s.name) != p_function->getters[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->keyed_setters.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_KEYED_SETTER][i];
		if (s.x < 0 || s.x >= Variant::VARIANT_MAX || Variant::get_member_validated_keyed_setter(Variant::Type(s.x)) != p_function->keyed_setters[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->keyed_getters.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_KEYED_GETTER][i];
		if (s.x < 0 || s.x >= Variant::VARIANT_MAX || Variant::get_member_validated_keyed_getter(Variant::Type(s.x)) != p_function->keyed_getters[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->indexed_setters.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_INDEXED_SETTER][i];
		if (s.x < 0 || s.x >= Variant::VARIANT_MAX || Variant::get_member_validated_indexed_setter(Variant::Type(s.x)) != p_function->indexed_setters[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->indexed_getters.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_INDEXED_GETTER][i];
		if (s.x < 0 || s.x >= Variant::VARIANT_MAX || Variant::get_member_validated_indexed_getter(Variant::Type(s.x)) != p_function->indexed_getters[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->builtin_methods.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_BUILTIN_METHOD][i];
		if (s.x < 0 || s.x >= Variant::VARIANT_MAX || Variant::get_builtin_method_hash(Variant::Type(s.x), s.name) != s.hash ||
				Variant::get_validated_builtin_method(Variant::Type(s.x), s.name) != p_function->builtin_methods[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->constructors.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_CONSTRUCTOR][i];
		if (!validate_constructor_symbol(s) || Variant::get_validated_constructor(Variant::Type(s.x), s.y) != p_function->constructors[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->utilities.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_UTILITY][i];
		if (Variant::get_utility_function_hash(s.name) != s.hash || Variant::get_validated_utility_function(s.name) != p_function->utilities[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->gds_utilities.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_GDSCRIPT_UTILITY][i];
		if (GDScriptUtilityFunctions::get_function_info(s.name).get_compatibility_hash() != s.hash ||
				GDScriptUtilityFunctions::get_function(s.name) != p_function->gds_utilities[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->methods.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_METHOD_BIND][i];
		MethodBind *method = ClassDB::get_method(s.owner, s.name);
		if (method == nullptr || method->get_hash() != s.hash || method != p_function->methods[i]) {
			return false;
		}
	}
	for (int i = 0; i < p_function->lambdas.size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_LAMBDA][i];
		const GDScriptFunction *const *lambda = p_functions.getptr(s.name);
		if (lambda == nullptr || *lambda != p_function->lambdas[i]) {
			return false;
		}
	}
	return true;
}

bool Internals::validate_record(const FunctionRecord &p_record, GDScriptFunction *p_function, const HashMap<String, GDScriptFunction *> &p_functions, String *r_error) {
	auto fail = [&](const String &p_message) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		return false;
	};
	if (p_record.identity.is_empty() || p_record.code.is_empty() || p_record.code[p_record.code.size() - 1] != GDScriptFunction::OPCODE_END) {
		return fail("invalid function identity or bytecode terminator");
	}
	if (!function_metadata_matches(p_record, p_function)) {
		return fail("function signature metadata mismatch");
	}
	Vector<int> symbolic_code;
	Vector<StringName> symbolic_names;
	if (!make_symbolic_global_code(p_function, symbolic_code, symbolic_names)) {
		return fail("could not symbolize runtime global operands");
	}
	if (p_record.code != symbolic_code) {
		return fail("bytecode mismatch");
	}
	if (p_record.default_arguments != p_function->default_arguments || p_record.global_names != symbolic_names) {
		return fail("default-argument or global-name table mismatch");
	}
	if (p_record.fingerprint != get_portable_function_fingerprint(p_record) || p_record.initial_line != p_function->_initial_line ||
			p_record.argument_count != p_function->_argument_count || p_record.vararg_index != p_function->_vararg_index || p_record.stack_size != p_function->_stack_size ||
			p_record.instruction_args_size != p_function->_instruction_args_size || p_record.operator_feedback_count != p_function->_operator_feedback_count ||
			p_record.call_feedback_count != p_function->_call_feedback_count || p_record.is_static != p_function->_static) {
		return fail("function frame metadata mismatch");
	}
	if (p_record.constants.size() != p_function->constants.size() || p_record.temporary_slots.size() != p_function->temporary_slots.size()) {
		return fail("constant or temporary-slot table size mismatch");
	}
	for (int i = 0; i < p_record.constants.size(); i++) {
		if (!constant_matches(p_record.constants[i], p_function->constants[i])) {
			return fail("constant table mismatch at index " + itos(i));
		}
	}
	for (int i = 0; i < p_record.temporary_slots.size(); i++) {
		if (p_record.temporary_slots[i] != p_function->temporary_slots[i] || p_record.temporary_slots[i].first < 0 || p_record.temporary_slots[i].first >= p_record.stack_size) {
			return fail("temporary-slot mismatch at index " + itos(i));
		}
	}
	for (int target : p_record.default_arguments) {
		if (target < 0 || target >= p_record.code.size()) {
			return fail("default-argument jump target is outside bytecode");
		}
	}
	if (!validate_relocations(p_record, p_function, p_functions)) {
		return fail("symbolic relocation mismatch");
	}
	return true;
}

void Internals::install_record(const FunctionRecord &p_record, GDScriptFunction *p_function, const HashMap<String, GDScriptFunction *> &p_functions) {
#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
	if (p_function->_baseline_jit != nullptr) {
		memdelete(p_function->_baseline_jit);
		p_function->_baseline_jit = nullptr;
	}
	if (GDScriptBaselineJIT *optimizing_jit = p_function->_get_optimizing_jit()) {
		memdelete(optimizing_jit);
		p_function->_optimizing_jit_ptr.set(0);
	}
	p_function->_jit_call_count.set(0);
	p_function->_optimizing_jit_attempted.clear();
#endif

	Vector<int> runtime_code;
	String resolution_error;
	ERR_FAIL_COND_MSG(!resolve_symbolic_global_code(p_record, runtime_code, &resolution_error), resolution_error);
	p_function->code = runtime_code;
	p_function->_code_ptr = p_function->code.ptrw();
	p_function->_code_size = p_function->code.size();
	p_function->default_arguments = p_record.default_arguments;
	p_function->_default_arg_ptr = p_function->default_arguments.is_empty() ? nullptr : p_function->default_arguments.ptr();
	p_function->global_names = p_record.global_names;
	p_function->_global_names_ptr = p_function->global_names.is_empty() ? nullptr : p_function->global_names.ptr();

	for (int i = 0; i < p_record.relocations[RELOC_OPERATOR].size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_OPERATOR][i];
		p_function->operator_funcs.write[i] = Variant::get_validated_operator_evaluator(Variant::Operator(s.x), Variant::Type(s.y), Variant::Type(s.z));
	}
	for (int i = 0; i < p_record.relocations[RELOC_SETTER].size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_SETTER][i];
		p_function->setters.write[i] = Variant::get_member_validated_setter(Variant::Type(s.x), s.name);
	}
	for (int i = 0; i < p_record.relocations[RELOC_GETTER].size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_GETTER][i];
		p_function->getters.write[i] = Variant::get_member_validated_getter(Variant::Type(s.x), s.name);
	}
	for (int i = 0; i < p_record.relocations[RELOC_KEYED_SETTER].size(); i++) {
		p_function->keyed_setters.write[i] = Variant::get_member_validated_keyed_setter(Variant::Type(p_record.relocations[RELOC_KEYED_SETTER][i].x));
	}
	for (int i = 0; i < p_record.relocations[RELOC_KEYED_GETTER].size(); i++) {
		p_function->keyed_getters.write[i] = Variant::get_member_validated_keyed_getter(Variant::Type(p_record.relocations[RELOC_KEYED_GETTER][i].x));
	}
	for (int i = 0; i < p_record.relocations[RELOC_INDEXED_SETTER].size(); i++) {
		p_function->indexed_setters.write[i] = Variant::get_member_validated_indexed_setter(Variant::Type(p_record.relocations[RELOC_INDEXED_SETTER][i].x));
	}
	for (int i = 0; i < p_record.relocations[RELOC_INDEXED_GETTER].size(); i++) {
		p_function->indexed_getters.write[i] = Variant::get_member_validated_indexed_getter(Variant::Type(p_record.relocations[RELOC_INDEXED_GETTER][i].x));
	}
	for (int i = 0; i < p_record.relocations[RELOC_BUILTIN_METHOD].size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_BUILTIN_METHOD][i];
		p_function->builtin_methods.write[i] = Variant::get_validated_builtin_method(Variant::Type(s.x), s.name);
	}
	for (int i = 0; i < p_record.relocations[RELOC_CONSTRUCTOR].size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_CONSTRUCTOR][i];
		p_function->constructors.write[i] = Variant::get_validated_constructor(Variant::Type(s.x), s.y);
	}
	for (int i = 0; i < p_record.relocations[RELOC_UTILITY].size(); i++) {
		p_function->utilities.write[i] = Variant::get_validated_utility_function(p_record.relocations[RELOC_UTILITY][i].name);
	}
	for (int i = 0; i < p_record.relocations[RELOC_GDSCRIPT_UTILITY].size(); i++) {
		p_function->gds_utilities.write[i] = GDScriptUtilityFunctions::get_function(p_record.relocations[RELOC_GDSCRIPT_UTILITY][i].name);
	}
	for (int i = 0; i < p_record.relocations[RELOC_METHOD_BIND].size(); i++) {
		const Symbol &s = p_record.relocations[RELOC_METHOD_BIND][i];
		p_function->methods.write[i] = ClassDB::get_method(s.owner, s.name);
	}
	for (int i = 0; i < p_record.relocations[RELOC_LAMBDA].size(); i++) {
		p_function->lambdas.write[i] = *p_functions.getptr(p_record.relocations[RELOC_LAMBDA][i].name);
	}

	p_function->_operator_funcs_ptr = p_function->operator_funcs.is_empty() ? nullptr : p_function->operator_funcs.ptr();
	p_function->_setters_ptr = p_function->setters.is_empty() ? nullptr : p_function->setters.ptr();
	p_function->_getters_ptr = p_function->getters.is_empty() ? nullptr : p_function->getters.ptr();
	p_function->_keyed_setters_ptr = p_function->keyed_setters.is_empty() ? nullptr : p_function->keyed_setters.ptr();
	p_function->_keyed_getters_ptr = p_function->keyed_getters.is_empty() ? nullptr : p_function->keyed_getters.ptr();
	p_function->_indexed_setters_ptr = p_function->indexed_setters.is_empty() ? nullptr : p_function->indexed_setters.ptr();
	p_function->_indexed_getters_ptr = p_function->indexed_getters.is_empty() ? nullptr : p_function->indexed_getters.ptr();
	p_function->_builtin_methods_ptr = p_function->builtin_methods.is_empty() ? nullptr : p_function->builtin_methods.ptr();
	p_function->_constructors_ptr = p_function->constructors.is_empty() ? nullptr : p_function->constructors.ptr();
	p_function->_utilities_ptr = p_function->utilities.is_empty() ? nullptr : p_function->utilities.ptr();
	p_function->_gds_utilities_ptr = p_function->gds_utilities.is_empty() ? nullptr : p_function->gds_utilities.ptr();
	p_function->_methods_ptr = p_function->methods.is_empty() ? nullptr : p_function->methods.ptrw();
	p_function->_lambdas_ptr = p_function->lambdas.is_empty() ? nullptr : p_function->lambdas.ptrw();

#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
	p_function->_baseline_jit = GDScriptBaselineJIT::compile(p_function);
	if (p_function->_baseline_jit != nullptr && p_record.profile_guided) {
		p_function->_optimizing_jit_attempted.set();
		if (GDScriptBaselineJIT *optimizing_jit = GDScriptBaselineJIT::compile_optimized(p_function)) {
			p_function->_optimizing_jit_ptr.set(reinterpret_cast<uintptr_t>(optimizing_jit));
		}
	}
#endif
}

Error RuntimeBuilder::fail(Error p_error, const String &p_message) {
	error = p_message;
	return p_error;
}

Error RuntimeBuilder::prepare_shell_graph(GDScript *p_script, const ParsedModule &p_module, HashMap<String, GDScript *> *r_classes, String *r_error) {
	auto fail_prepare = [&](const String &p_message) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		return ERR_INVALID_DATA;
	};
	if (p_script == nullptr || p_module.classes.is_empty()) {
		return fail_prepare("The compiled module has no root class.");
	}

	HashMap<String, const ClassRecord *> records;
	const ClassRecord *root_record = nullptr;
	for (const ClassRecord &record : p_module.classes) {
		if (record.identity.is_empty() || records.has(record.identity)) {
			return fail_prepare("Duplicate or empty compiled class identity '" + record.identity + "'.");
		}
		records.insert(record.identity, &record);
		if (record.owner_identity.is_empty()) {
			if (root_record != nullptr || record.identity != "root") {
				return fail_prepare("The compiled module must contain exactly one 'root' class.");
			}
			root_record = &record;
		}
	}
	if (root_record == nullptr) {
		return fail_prepare("The compiled module has no root class record.");
	}

	HashMap<String, GDScript *> result;
	result.insert(root_record->identity, p_script);
	p_script->local_name = root_record->local_name;
	p_script->global_name = root_record->global_name;
	p_script->fully_qualified_name = root_record->fully_qualified_name;
	p_script->simplified_icon_path = root_record->icon_path;

	HashSet<String> pending;
	for (const ClassRecord &record : p_module.classes) {
		if (&record != root_record) {
			pending.insert(record.identity);
		}
	}
	while (!pending.is_empty()) {
		bool progressed = false;
		Vector<String> completed;
		for (const String &identity : pending) {
			const ClassRecord *record = *records.getptr(identity);
			GDScript *const *owner_ptr = result.getptr(record->owner_identity);
			if (owner_ptr == nullptr) {
				continue;
			}
			if (record->local_name.is_empty()) {
				return fail_prepare("Nested class '" + identity + "' has no local name.");
			}
			GDScript *owner = *owner_ptr;
			Ref<GDScript> subclass;
			if (const Ref<GDScript> *existing = owner->subclasses.getptr(record->local_name)) {
				subclass = *existing;
			} else {
				subclass = GDScriptLanguage::get_singleton()->get_orphan_subclass(record->fully_qualified_name);
				if (subclass.is_null()) {
					subclass.instantiate();
				}
				owner->subclasses.insert(record->local_name, subclass);
			}
			if (subclass.is_null()) {
				return fail_prepare("Could not allocate nested class '" + identity + "'.");
			}
			subclass->_owner = owner;
			subclass->path = p_script->path;
			subclass->local_name = record->local_name;
			subclass->global_name = record->global_name;
			subclass->fully_qualified_name = record->fully_qualified_name;
			subclass->simplified_icon_path = record->icon_path;
			result.insert(identity, subclass.ptr());
			completed.push_back(identity);
			progressed = true;
		}
		for (const String &identity : completed) {
			pending.erase(identity);
		}
		if (!progressed) {
			return fail_prepare("Compiled nested-class ownership contains a cycle or missing owner.");
		}
	}

	for (const ClassRecord &record : p_module.classes) {
		HashSet<StringName> subclass_names;
		for (const BindingRecord &binding : record.subclasses) {
			const ClassRecord *const *child_record = records.getptr(binding.identity);
			if (binding.name.is_empty() || subclass_names.has(binding.name) || child_record == nullptr || (*child_record)->owner_identity != record.identity ||
					(*child_record)->local_name != binding.name) {
				return fail_prepare("Invalid nested-class binding in '" + record.identity + "'.");
			}
			subclass_names.insert(binding.name);
		}
	}

	if (r_classes != nullptr) {
		*r_classes = result;
	}
	return OK;
}

GDScript *RuntimeBuilder::find_script(const String &p_path, const String &p_class, Error &r_error) {
	const String path = GDScript::canonicalize_path(p_path);
	const String script_class = canonicalize_qualified_script_name(p_class);
	if (path == GDScript::canonicalize_path(module.path)) {
		for (const KeyValue<String, const ClassRecord *> &entry : class_records) {
			if (canonicalize_qualified_script_name(entry.value->fully_qualified_name) == script_class) {
				r_error = OK;
				return *classes.getptr(entry.key);
			}
		}
	}

	Ref<GDScript> dependency = GDScriptCache::get_full_script(path, r_error, root->path);
	if (r_error != OK || dependency.is_null()) {
		return nullptr;
	}
	if (script_class.is_empty() || canonicalize_qualified_script_name(dependency->fully_qualified_name) == script_class) {
		return dependency.ptr();
	}
	GDScript *result = dependency->find_class(script_class);
	if (result == nullptr) {
		r_error = ERR_DOES_NOT_EXIST;
	}
	return result;
}

Error RuntimeBuilder::decode_constant(const ConstantData &p_constant, Variant &r_value) {
	switch (p_constant.kind) {
		case CONSTANT_VARIANT: {
			int used = 0;
			if (decode_variant(r_value, p_constant.encoded.ptr(), p_constant.encoded.size(), &used, false) != OK || used != p_constant.encoded.size()) {
				return fail(ERR_INVALID_DATA, "Could not decode a portable Variant constant.");
			}
			return OK;
		}
		case CONSTANT_RESOURCE: {
			Ref<Resource> resource = ResourceLoader::load(p_constant.name);
			if (resource.is_null()) {
				return fail(ERR_CANT_ACQUIRE_RESOURCE, "Could not load constant resource '" + p_constant.name + "'.");
			}
			r_value = resource;
			return OK;
		}
		case CONSTANT_GDSCRIPT: {
			Error load_error = OK;
			GDScript *script = find_script(p_constant.name, p_constant.owner, load_error);
			if (script == nullptr || load_error != OK) {
				return fail(load_error == OK ? ERR_DOES_NOT_EXIST : load_error, "Could not resolve script constant '" + p_constant.owner + "'.");
			}
			r_value = Ref<GDScript>(script);
			return OK;
		}
		case CONSTANT_NATIVE_CLASS: {
			GDScriptLanguage *language = GDScriptLanguage::get_singleton();
			if (!language->has_any_global_constant(p_constant.name)) {
				return fail(ERR_DOES_NOT_EXIST, "Could not resolve native class '" + p_constant.name + "'.");
			}
			Variant native_value = language->get_any_global_constant(p_constant.name);
			if (Object::cast_to<GDScriptNativeClass>(native_value.get_validated_object()) == nullptr) {
				return fail(ERR_INVALID_DATA, "Global '" + p_constant.name + "' is not a native class.");
			}
			r_value = native_value;
			return OK;
		}
	}
	return fail(ERR_INVALID_DATA, "Unknown compiled constant kind.");
}

Error RuntimeBuilder::decode_property(const PropertyRecord &p_record, PropertyInfo &r_property) {
	if (p_record.type >= Variant::VARIANT_MAX || p_record.hint >= PROPERTY_HINT_MAX) {
		return fail(ERR_INVALID_DATA, "Invalid property type or hint.");
	}
	if (p_record.type == Variant::OBJECT && !p_record.class_name.is_empty() && !ClassDB::class_exists(p_record.class_name)) {
		if (!ScriptServer::is_global_class(p_record.class_name)) {
			return fail(ERR_DOES_NOT_EXIST, "Could not resolve property class '" + String(p_record.class_name) + "'.");
		}
		const String script_path = ScriptServer::get_global_class_path(p_record.class_name);
		Ref<Resource> resource = ResourceLoader::load(script_path, "Script");
		Ref<Script> script = resource;
		if (script.is_null() || script->get_global_name() != p_record.class_name) {
			return fail(ERR_CANT_ACQUIRE_RESOURCE, "Could not load global property class '" + String(p_record.class_name) + "'.");
		}
	}
	if (p_record.type == Variant::STRUCT) {
		bool found = false;
		for (const KeyValue<String, Ref<StructLayout>> &entry : layouts_by_identifier) {
			if (entry.value->get_type_identifier() == p_record.class_name || entry.value->get_type_descriptor() == p_record.hint_string) {
				found = true;
				break;
			}
		}
		if (!found) {
			return fail(ERR_DOES_NOT_EXIST, "Could not resolve struct property layout '" + String(p_record.class_name) + "'.");
		}
	}
	r_property = PropertyInfo(Variant::Type(p_record.type), p_record.name, PropertyHint(p_record.hint), p_record.hint_string, p_record.usage, p_record.class_name);
	return OK;
}

Error RuntimeBuilder::decode_method(const MethodRecord &p_record, MethodInfo &r_method) {
	r_method = MethodInfo();
	r_method.name = p_record.name;
	r_method.flags = p_record.flags;
	r_method.id = p_record.id;
	if (decode_property(p_record.return_value, r_method.return_val) != OK) {
		return ERR_INVALID_DATA;
	}
	for (const PropertyRecord &argument : p_record.arguments) {
		PropertyInfo property;
		if (decode_property(argument, property) != OK) {
			return ERR_INVALID_DATA;
		}
		r_method.arguments.push_back(property);
	}
	for (const ConstantData &argument : p_record.default_arguments) {
		Variant value;
		Error decode_error = decode_constant(argument, value);
		if (decode_error != OK) {
			return decode_error;
		}
		r_method.default_arguments.push_back(value);
	}
	r_method.return_val_metadata = p_record.return_value_metadata;
	r_method.arguments_metadata = p_record.argument_metadata;
	if (!r_method.arguments_metadata.is_empty() && r_method.arguments_metadata.size() != r_method.arguments.size()) {
		return fail(ERR_INVALID_DATA, "Method argument metadata count mismatch for '" + p_record.name + "'.");
	}
	return OK;
}

Error RuntimeBuilder::decode_data_type(const DataTypeRecord &p_record, GDScriptDataType &r_type, int p_depth) {
	if (p_depth > Variant::MAX_RECURSION_DEPTH || p_record.kind > GDScriptDataType::GDSCRIPT || p_record.builtin_type >= Variant::VARIANT_MAX) {
		return fail(ERR_INVALID_DATA, "Invalid or excessively nested GDScript data type.");
	}
	r_type = GDScriptDataType();
	r_type.kind = GDScriptDataType::Kind(p_record.kind);
	r_type.builtin_type = Variant::Type(p_record.builtin_type);
	r_type.native_type = p_record.native_type;

	if (r_type.kind == GDScriptDataType::NATIVE && (r_type.native_type.is_empty() || !ClassDB::class_exists(r_type.native_type))) {
		return fail(ERR_INVALID_DATA, "Unknown native data type '" + String(r_type.native_type) + "'.");
	}
	if (r_type.kind == GDScriptDataType::SCRIPT || r_type.kind == GDScriptDataType::GDSCRIPT) {
		Script *script = nullptr;
		Ref<Script> script_ref;
		if (r_type.kind == GDScriptDataType::GDSCRIPT) {
			Error load_error = OK;
			GDScript *gdscript = find_script(p_record.script_path, p_record.script_class, load_error);
			if (gdscript == nullptr || load_error != OK) {
				return fail(load_error == OK ? ERR_DOES_NOT_EXIST : load_error, "Could not resolve GDScript data type '" + p_record.script_class + "'.");
			}
			script_ref = Ref<GDScript>(gdscript);
			script = gdscript;
		} else {
			Ref<Resource> resource = ResourceLoader::load(p_record.script_path, "Script");
			script_ref = resource;
			script = script_ref.ptr();
			if (script == nullptr || (!p_record.script_class.is_empty() && script->get_global_name() != p_record.script_class)) {
				return fail(ERR_CANT_ACQUIRE_RESOURCE, "Could not load script data type '" + p_record.script_path + "'.");
			}
		}
		r_type.script_type_ref = script_ref;
		r_type.script_type = script;
	}

	if (!p_record.struct_layout.is_empty()) {
		Variant serialized_layout;
		int used = 0;
		if (r_type.kind != GDScriptDataType::BUILTIN || r_type.builtin_type != Variant::STRUCT ||
				decode_variant(serialized_layout, p_record.struct_layout.ptr(), p_record.struct_layout.size(), &used, false) != OK ||
				used != p_record.struct_layout.size() || serialized_layout.get_type() != Variant::DICTIONARY) {
			return fail(ERR_INVALID_DATA, "Invalid serialized struct data type.");
		}
		Error layout_error = OK;
		Ref<StructLayout> decoded_layout = StructLayout::from_dictionary(serialized_layout, &layout_error);
		if (layout_error != OK || decoded_layout.is_null()) {
			return fail(ERR_INVALID_DATA, "Could not reconstruct a struct data type layout.");
		}
		const Ref<StructLayout> *module_layout = layouts_by_identifier.getptr(decoded_layout->get_type_identifier());
		if (module_layout != nullptr) {
			if (!decoded_layout->is_compatible(*module_layout)) {
				return fail(ERR_INVALID_DATA, "Struct data type layout does not match the module layout descriptor.");
			}
			r_type.struct_layout = *module_layout;
		} else {
			r_type.struct_layout = decoded_layout;
		}
	}
	for (const DataTypeRecord &element_record : p_record.container_element_types) {
		GDScriptDataType element_type;
		Error type_error = decode_data_type(element_record, element_type, p_depth + 1);
		if (type_error != OK) {
			return type_error;
		}
		r_type.container_element_types.push_back(element_type);
	}
	return OK;
}

Error RuntimeBuilder::build_layout(const String &p_identifier) {
	if (layouts_by_identifier.has(p_identifier)) {
		return OK;
	}
	const StructLayoutRecord *const *record_ptr = layout_records.getptr(p_identifier);
	if (record_ptr == nullptr || layouts_being_built.has(p_identifier)) {
		return fail(ERR_INVALID_DATA, "Unknown or recursively embedded struct layout '" + p_identifier + "'.");
	}
	const StructLayoutRecord &record = **record_ptr;
	layouts_being_built.insert(p_identifier);
	Ref<StructLayout> layout;
	layout.instantiate(record.type_identifier, record.schema_version);
	for (const StructFieldRecord &field : record.fields) {
		Ref<StructLayout> nested_layout;
		if (field.type == Variant::STRUCT) {
			Error nested_error = build_layout(field.nested_layout);
			if (nested_error != OK) {
				layouts_being_built.erase(p_identifier);
				return nested_error;
			}
			nested_layout = *layouts_by_identifier.getptr(field.nested_layout);
		}
		Variant default_value;
		Error constant_error = decode_constant(field.default_value, default_value);
		if (constant_error != OK || layout->add_field(field.name, Variant::Type(field.type), default_value, nested_layout) != OK) {
			layouts_being_built.erase(p_identifier);
			return fail(ERR_INVALID_DATA, "Could not reconstruct field '" + field.name + "' of struct layout '" + p_identifier + "'.");
		}
	}
	if (layout->finalize() != OK || layout->get_schema_fingerprint() != record.schema_fingerprint) {
		layouts_being_built.erase(p_identifier);
		return fail(ERR_INVALID_DATA, "Struct schema fingerprint mismatch for '" + p_identifier + "'.");
	}
	layouts_being_built.erase(p_identifier);
	layouts_by_identifier.insert(p_identifier, layout);
	return OK;
}

Error RuntimeBuilder::stage_classes() {
	for (int i = 0; i < module.classes.size(); i++) {
		const ClassRecord &record = module.classes[i];
		if (class_records.has(record.identity)) {
			return fail(ERR_INVALID_DATA, "Duplicate class identity '" + record.identity + "'.");
		}
		GDScript *const *script = classes.getptr(record.identity);
		if (script == nullptr) {
			return fail(ERR_INVALID_DATA, "Missing runtime shell for class '" + record.identity + "'.");
		}
		class_records.insert(record.identity, &record);
		StagedClass staged;
		staged.record = &record;
		staged.script = *script;
		staged_class_indices.insert(record.identity, staged_classes.size());
		staged_classes.push_back(staged);
		for (const StructLayoutRecord &layout : record.struct_layouts) {
			if (layout.type_identifier.is_empty() || layout_records.has(layout.type_identifier)) {
				return fail(ERR_INVALID_DATA, "Duplicate or empty struct layout identifier in class '" + record.identity + "'.");
			}
			layout_records.insert(layout.type_identifier, &layout);
		}
	}

	for (const KeyValue<String, const StructLayoutRecord *> &entry : layout_records) {
		Error layout_error = build_layout(entry.key);
		if (layout_error != OK) {
			return layout_error;
		}
	}

	for (StagedClass &staged : staged_classes) {
		const ClassRecord &record = *staged.record;
		GDScriptLanguage *language = GDScriptLanguage::get_singleton();
		if (record.native_base.is_empty() || !language->has_any_global_constant(record.native_base)) {
			return fail(ERR_INVALID_DATA, "Unknown native base '" + record.native_base + "' for class '" + record.identity + "'.");
		}
		Variant native_value = language->get_any_global_constant(record.native_base);
		GDScriptNativeClass *native = Object::cast_to<GDScriptNativeClass>(native_value.get_validated_object());
		if (native == nullptr) {
			return fail(ERR_INVALID_DATA, "Native base symbol '" + record.native_base + "' is not a class.");
		}
		staged.native = Ref<GDScriptNativeClass>(native);

		if (!record.script_base_path.is_empty()) {
			Error base_error = OK;
			GDScript *base = find_script(record.script_base_path, record.script_base_class, base_error);
			if (base == nullptr || base_error != OK) {
				return fail(base_error == OK ? ERR_DOES_NOT_EXIST : base_error, "Could not resolve base script '" + record.script_base_class + "'.");
			}
			staged.base = Ref<GDScript>(base);
		}

		HashSet<int> member_indices;
		for (const MemberRecord &member : record.members) {
			if (member.name.is_empty() || staged.member_indices.has(member.name) || member.index < 0 || member_indices.has(member.index)) {
				return fail(ERR_INVALID_DATA, "Invalid or duplicate member metadata in class '" + record.identity + "'.");
			}
			GDScript::MemberInfo info;
			info.index = member.index;
			info.setter = member.setter;
			info.getter = member.getter;
			if (decode_data_type(member.data_type, info.data_type) != OK || decode_property(member.property, info.property_info) != OK) {
				return ERR_INVALID_DATA;
			}
			staged.member_indices.insert(member.name, info);
			member_indices.insert(member.index);
			if (member.own_member) {
				staged.members.insert(member.name);
			}
			if (member.has_default_value) {
				Variant value;
				Error default_error = decode_constant(member.default_value, value);
				if (default_error != OK) {
					return default_error;
				}
				staged.member_default_values.insert(member.name, value);
			}
		}
		for (int i = 0; i < record.members.size(); i++) {
			if (!member_indices.has(i)) {
				return fail(ERR_INVALID_DATA, "Non-contiguous member indices in class '" + record.identity + "'.");
			}
		}

		HashSet<int> static_indices;
		for (const MemberRecord &member : record.static_members) {
			if (member.name.is_empty() || staged.static_variables_indices.has(member.name) || member.index < 0 || static_indices.has(member.index)) {
				return fail(ERR_INVALID_DATA, "Invalid or duplicate static member metadata in class '" + record.identity + "'.");
			}
			GDScript::MemberInfo info;
			info.index = member.index;
			info.setter = member.setter;
			info.getter = member.getter;
			if (decode_data_type(member.data_type, info.data_type) != OK || decode_property(member.property, info.property_info) != OK) {
				return ERR_INVALID_DATA;
			}
			staged.static_variables_indices.insert(member.name, info);
			static_indices.insert(member.index);
			if (member.has_default_value) {
				Variant value;
				Error default_error = decode_constant(member.default_value, value);
				if (default_error != OK) {
					return default_error;
				}
				staged.member_default_values.insert(member.name, value);
			}
		}
		for (int i = 0; i < record.static_members.size(); i++) {
			if (!static_indices.has(i)) {
				return fail(ERR_INVALID_DATA, "Non-contiguous static member indices in class '" + record.identity + "'.");
			}
		}
		staged.static_variables.resize(record.static_members.size());

		for (const StructLayoutRecord &layout : record.struct_layouts) {
			if (layout.name.is_empty() || staged.struct_layouts.has(layout.name)) {
				return fail(ERR_INVALID_DATA, "Invalid struct declaration metadata in class '" + record.identity + "'.");
			}
			staged.struct_layouts.insert(layout.name, *layouts_by_identifier.getptr(layout.type_identifier));
		}

		for (const NamedConstantRecord &constant : record.constants) {
			if (constant.name.is_empty() || staged.constants.has(constant.name)) {
				return fail(ERR_INVALID_DATA, "Duplicate or empty constant name in class '" + record.identity + "'.");
			}
			Variant value;
			Error constant_error = decode_constant(constant.value, value);
			if (constant_error != OK) {
				return constant_error;
			}
			staged.constants.insert(constant.name, value);
		}

		for (const SignalRecord &signal : record.signals) {
			if (signal.name.is_empty() || staged.signals.has(signal.name)) {
				return fail(ERR_INVALID_DATA, "Duplicate or empty signal name in class '" + record.identity + "'.");
			}
			MethodInfo method;
			if (decode_method(signal.method, method) != OK) {
				return ERR_INVALID_DATA;
			}
			staged.signals.insert(signal.name, method);
		}

		Variant rpc;
		Error rpc_error = decode_constant(record.rpc_config, rpc);
		if (rpc_error != OK || rpc.get_type() != Variant::DICTIONARY) {
			return fail(ERR_INVALID_DATA, "Invalid RPC configuration for class '" + record.identity + "'.");
		}
		staged.rpc_config = rpc;

		for (const BindingRecord &binding : record.subclasses) {
			GDScript *const *subclass = classes.getptr(binding.identity);
			if (binding.name.is_empty() || staged.subclasses.has(binding.name) || subclass == nullptr) {
				return fail(ERR_INVALID_DATA, "Invalid subclass binding in class '" + record.identity + "'.");
			}
			staged.subclasses.insert(binding.name, Ref<GDScript>(*subclass));
		}
	}
	return verify_inheritance();
}

Error RuntimeBuilder::verify_inheritance() {
	for (const StagedClass &staged : staged_classes) {
		HashSet<const GDScript *> visited;
		const GDScript *cursor = staged.script;
		while (cursor != nullptr) {
			if (visited.has(cursor)) {
				return fail(ERR_INVALID_DATA, "Cyclic script inheritance involving '" + staged.record->fully_qualified_name + "'.");
			}
			visited.insert(cursor);
			const int *index = nullptr;
			for (int i = 0; i < staged_classes.size(); i++) {
				if (staged_classes[i].script == cursor) {
					index = staged_class_indices.getptr(staged_classes[i].record->identity);
					break;
				}
			}
			if (index != nullptr) {
				cursor = staged_classes[*index].base.ptr();
			} else {
				cursor = cursor->base.ptr();
			}
		}
	}
	return OK;
}

Error RuntimeBuilder::validate_function_code(const FunctionRecord &p_record) {
	if (p_record.identity.is_empty() || p_record.code.is_empty() || p_record.code[p_record.code.size() - 1] != GDScriptFunction::OPCODE_END ||
			p_record.fingerprint != get_portable_function_fingerprint(p_record)) {
		return fail(ERR_INVALID_DATA, "Invalid bytecode identity, checksum, or terminator for '" + p_record.identity + "'.");
	}
	if (p_record.argument_count < 0 || p_record.argument_types.size() != p_record.argument_count || p_record.method.arguments.size() != p_record.argument_count ||
			p_record.stack_size < GDScriptFunction::FIXED_ADDRESSES_MAX + p_record.argument_count || p_record.instruction_args_size < 0 ||
			p_record.operator_feedback_count < 0 || p_record.call_feedback_count < 0 || p_record.default_argument_count < 0 ||
			p_record.default_argument_count > p_record.argument_count ||
			(p_record.vararg_index != -1 && (p_record.vararg_index != GDScriptFunction::FIXED_ADDRESSES_MAX + p_record.argument_count || p_record.vararg_index >= p_record.stack_size))) {
		return fail(ERR_INVALID_DATA, "Invalid frame or signature metadata for '" + p_record.identity + "'.");
	}
	if ((p_record.default_argument_count == 0 && !p_record.default_arguments.is_empty()) ||
			(p_record.default_argument_count > 0 && p_record.default_arguments.size() != p_record.default_argument_count + 1)) {
		return fail(ERR_INVALID_DATA, "Invalid default-argument table for '" + p_record.identity + "'.");
	}
	if (p_record.relocations.size() != RELOC_TABLE_MAX) {
		return fail(ERR_INVALID_DATA, "Invalid relocation-table count for '" + p_record.identity + "'.");
	}
	for (const Pair<int, Variant::Type> &slot : p_record.temporary_slots) {
		if (slot.first < GDScriptFunction::FIXED_ADDRESSES_MAX || slot.first >= p_record.stack_size || slot.second <= Variant::NIL || slot.second >= Variant::VARIANT_MAX) {
			return fail(ERR_INVALID_DATA, "Invalid typed temporary slot in '" + p_record.identity + "'.");
		}
	}

	Vector<uint8_t> boundaries;
	boundaries.resize(p_record.code.size());
	boundaries.fill(0);
	Vector<int> jump_targets;
	int ip = 0;
	while (ip < p_record.code.size()) {
		boundaries.write[ip] = 1;
		const int raw_opcode = p_record.code[ip];
		if (raw_opcode < 0 || raw_opcode > GDScriptFunction::OPCODE_END) {
			return fail(ERR_INVALID_DATA, "Unknown opcode in '" + p_record.identity + "'.");
		}
		const GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(raw_opcode);
		const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(opcode);
		const int length = GDScriptFunction::get_instruction_size(p_record.code.ptr(), p_record.code.size(), ip);
		if (length <= 0 || (descriptor.instruction_size == 0 && p_record.code[ip + 1] > p_record.instruction_args_size) ||
				(opcode == GDScriptFunction::OPCODE_END && ip + length != p_record.code.size())) {
			return fail(ERR_INVALID_DATA, "Truncated instruction, invalid argument count, or premature bytecode terminator in '" + p_record.identity + "'.");
		}
		if (descriptor.instruction_size > 0 && descriptor.operand_kinds.count != descriptor.instruction_size - 1) {
			return fail(ERR_BUG, "Inconsistent opcode descriptor for '" + String(descriptor.name) + "'.");
		}

		auto valid_address = [&](int p_address, bool p_constant_only, bool p_typed_stack_only) {
			const uint32_t address = uint32_t(p_address);
			const uint32_t mode = address >> GDScriptFunction::ADDR_BITS;
			const int index = address & GDScriptFunction::ADDR_MASK;
			if (p_constant_only) {
				return mode == GDScriptFunction::ADDR_TYPE_CONSTANT && index < p_record.constants.size();
			}
			if (p_typed_stack_only) {
				return mode == GDScriptFunction::ADDR_TYPE_STACK && index >= GDScriptFunction::FIXED_ADDRESSES_MAX && index < p_record.stack_size;
			}
			switch (mode) {
				case GDScriptFunction::ADDR_TYPE_STACK:
					return index < p_record.stack_size;
				case GDScriptFunction::ADDR_TYPE_CONSTANT:
					return index < p_record.constants.size();
				case GDScriptFunction::ADDR_TYPE_MEMBER:
					return true;
				default:
					return false;
			}
		};

		for (int word = 1; word < length; word++) {
			const int value = p_record.code[ip + word];
			switch (GDScriptFunction::get_operand_kind(p_record.code.ptr(), p_record.code.size(), ip, word)) {
				case GDScriptFunction::OPERAND_FRAME_SLOT:
					if (!valid_address(value, false, false)) {
						return fail(ERR_INVALID_DATA, "Invalid frame-slot operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_TYPED_FRAME_SLOT:
					if (!valid_address(value, false, true)) {
						return fail(ERR_INVALID_DATA, "Invalid typed-frame-slot operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_CONSTANT_ADDRESS:
					if (!valid_address(value, true, false)) {
						return fail(ERR_INVALID_DATA, "Invalid constant operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_NAME_INDEX:
					if (value < 0 || value >= p_record.global_names.size()) {
						return fail(ERR_INVALID_DATA, "Invalid name operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_FUNCTION_INDEX:
				case GDScriptFunction::OPERAND_NATIVE_API_RELOCATION:
					if (descriptor.relocation_kind < 0 || descriptor.relocation_kind >= p_record.relocations.size() ||
							value < 0 || value >= p_record.relocations[descriptor.relocation_kind].size()) {
						return fail(ERR_INVALID_DATA, "Invalid symbolic relocation operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_JUMP_TARGET:
					jump_targets.push_back(value);
					break;
				case GDScriptFunction::OPERAND_ARGUMENT_COUNT:
					if (value < 0 || value > p_record.instruction_args_size) {
						return fail(ERR_INVALID_DATA, "Invalid instruction argument count in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_STRUCT_FIELD_INDEX:
				case GDScriptFunction::OPERAND_STATIC_VARIABLE_INDEX:
					if (value < 0) {
						return fail(ERR_INVALID_DATA, "Invalid index operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_GLOBAL_INDEX:
					if (value < 0 || value >= p_record.global_names.size() || GDScriptLanguage::get_singleton() == nullptr ||
							!GDScriptLanguage::get_singleton()->get_global_map().has(p_record.global_names[value])) {
						return fail(ERR_INVALID_DATA, "Unresolved symbolic global operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_TYPE_ID:
					if (value < Variant::NIL || value >= Variant::VARIANT_MAX) {
						return fail(ERR_INVALID_DATA, "Invalid type operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_OPERATOR:
					if (value < 0 || value >= Variant::OP_MAX) {
						return fail(ERR_INVALID_DATA, "Invalid operator operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_BOOLEAN:
					if (value != 0 && value != 1) {
						return fail(ERR_INVALID_DATA, "Invalid boolean operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_OPERATOR_FEEDBACK_INDEX:
					if (value < 0 || value >= p_record.operator_feedback_count) {
						return fail(ERR_INVALID_DATA, "Invalid operator-feedback operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_CALL_FEEDBACK_INDEX:
					if (value < 0 || value >= p_record.call_feedback_count) {
						return fail(ERR_INVALID_DATA, "Invalid call-feedback operand in '" + p_record.identity + "'.");
					}
					break;
				case GDScriptFunction::OPERAND_NONE:
				case GDScriptFunction::OPERAND_VARIADIC_FRAME_SLOTS:
					return fail(ERR_BUG, "Incomplete operand descriptor for '" + String(descriptor.name) + "'.");
				case GDScriptFunction::OPERAND_TYPE_METADATA:
				case GDScriptFunction::OPERAND_IMMEDIATE:
					break;
			}
		}
		const int result_operand = GDScriptFunction::get_result_operand(p_record.code.ptr(), p_record.code.size(), ip);
		if (result_operand >= length) {
			return fail(ERR_BUG, "Invalid result operand descriptor for '" + String(descriptor.name) + "'.");
		}
		ip += length;
	}
	for (int target : jump_targets) {
		if (target < 0 || target >= boundaries.size() || !boundaries[target]) {
			return fail(ERR_INVALID_DATA, "Jump target is not an instruction boundary in '" + p_record.identity + "'.");
		}
	}
	for (int target : p_record.default_arguments) {
		if (target < 0 || target >= boundaries.size() || !boundaries[target]) {
			return fail(ERR_INVALID_DATA, "Default-argument target is not an instruction boundary in '" + p_record.identity + "'.");
		}
	}
	return OK;
}

Error RuntimeBuilder::resolve_function_relocations(const FunctionRecord &p_record, GDScriptFunction *p_function) {
	auto invalid_symbol = [&]() {
		return fail(ERR_INVALID_DATA, "Invalid symbolic relocation in function '" + p_record.identity + "'.");
	};

	p_function->operator_funcs.resize(p_record.relocations[RELOC_OPERATOR].size());
	for (int i = 0; i < p_function->operator_funcs.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_OPERATOR][i];
		if (symbol.x < 0 || symbol.x >= Variant::OP_MAX || symbol.y < 0 || symbol.y >= Variant::VARIANT_MAX || symbol.z < 0 || symbol.z >= Variant::VARIANT_MAX) {
			return invalid_symbol();
		}
		p_function->operator_funcs.write[i] = Variant::get_validated_operator_evaluator(Variant::Operator(symbol.x), Variant::Type(symbol.y), Variant::Type(symbol.z));
		if (p_function->operator_funcs[i] == nullptr) {
			return invalid_symbol();
		}
#ifdef DEBUG_ENABLED
		p_function->operator_names.push_back(Variant::get_operator_name(Variant::Operator(symbol.x)));
#endif
	}

	p_function->setters.resize(p_record.relocations[RELOC_SETTER].size());
	for (int i = 0; i < p_function->setters.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_SETTER][i];
		if (symbol.x < 0 || symbol.x >= Variant::VARIANT_MAX || (p_function->setters.write[i] = Variant::get_member_validated_setter(Variant::Type(symbol.x), symbol.name)) == nullptr) {
			return invalid_symbol();
		}
#ifdef DEBUG_ENABLED
		p_function->setter_names.push_back(symbol.name);
#endif
	}
	p_function->getters.resize(p_record.relocations[RELOC_GETTER].size());
	for (int i = 0; i < p_function->getters.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_GETTER][i];
		if (symbol.x < 0 || symbol.x >= Variant::VARIANT_MAX || (p_function->getters.write[i] = Variant::get_member_validated_getter(Variant::Type(symbol.x), symbol.name)) == nullptr) {
			return invalid_symbol();
		}
#ifdef DEBUG_ENABLED
		p_function->getter_names.push_back(symbol.name);
#endif
	}

	p_function->keyed_setters.resize(p_record.relocations[RELOC_KEYED_SETTER].size());
	for (int i = 0; i < p_function->keyed_setters.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_KEYED_SETTER][i];
		if (symbol.x < 0 || symbol.x >= Variant::VARIANT_MAX || (p_function->keyed_setters.write[i] = Variant::get_member_validated_keyed_setter(Variant::Type(symbol.x))) == nullptr) {
			return invalid_symbol();
		}
	}
	p_function->keyed_getters.resize(p_record.relocations[RELOC_KEYED_GETTER].size());
	for (int i = 0; i < p_function->keyed_getters.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_KEYED_GETTER][i];
		if (symbol.x < 0 || symbol.x >= Variant::VARIANT_MAX || (p_function->keyed_getters.write[i] = Variant::get_member_validated_keyed_getter(Variant::Type(symbol.x))) == nullptr) {
			return invalid_symbol();
		}
	}
	p_function->indexed_setters.resize(p_record.relocations[RELOC_INDEXED_SETTER].size());
	for (int i = 0; i < p_function->indexed_setters.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_INDEXED_SETTER][i];
		if (symbol.x < 0 || symbol.x >= Variant::VARIANT_MAX || (p_function->indexed_setters.write[i] = Variant::get_member_validated_indexed_setter(Variant::Type(symbol.x))) == nullptr) {
			return invalid_symbol();
		}
	}
	p_function->indexed_getters.resize(p_record.relocations[RELOC_INDEXED_GETTER].size());
	for (int i = 0; i < p_function->indexed_getters.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_INDEXED_GETTER][i];
		if (symbol.x < 0 || symbol.x >= Variant::VARIANT_MAX || (p_function->indexed_getters.write[i] = Variant::get_member_validated_indexed_getter(Variant::Type(symbol.x))) == nullptr) {
			return invalid_symbol();
		}
	}

	p_function->builtin_methods.resize(p_record.relocations[RELOC_BUILTIN_METHOD].size());
	for (int i = 0; i < p_function->builtin_methods.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_BUILTIN_METHOD][i];
		if (symbol.x < 0 || symbol.x >= Variant::VARIANT_MAX || Variant::get_builtin_method_hash(Variant::Type(symbol.x), symbol.name) != symbol.hash ||
				(p_function->builtin_methods.write[i] = Variant::get_validated_builtin_method(Variant::Type(symbol.x), symbol.name)) == nullptr) {
			return invalid_symbol();
		}
#ifdef DEBUG_ENABLED
		p_function->builtin_methods_names.push_back(symbol.name);
#endif
	}
	p_function->constructors.resize(p_record.relocations[RELOC_CONSTRUCTOR].size());
	for (int i = 0; i < p_function->constructors.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_CONSTRUCTOR][i];
		if (!validate_constructor_symbol(symbol) || (p_function->constructors.write[i] = Variant::get_validated_constructor(Variant::Type(symbol.x), symbol.y)) == nullptr) {
			return invalid_symbol();
		}
#ifdef DEBUG_ENABLED
		p_function->constructors_names.push_back(Variant::get_type_name(Variant::Type(symbol.x)));
#endif
	}
	p_function->utilities.resize(p_record.relocations[RELOC_UTILITY].size());
	for (int i = 0; i < p_function->utilities.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_UTILITY][i];
		if (Variant::get_utility_function_hash(symbol.name) != symbol.hash || (p_function->utilities.write[i] = Variant::get_validated_utility_function(symbol.name)) == nullptr) {
			return invalid_symbol();
		}
#ifdef DEBUG_ENABLED
		p_function->utilities_names.push_back(symbol.name);
#endif
	}
	p_function->gds_utilities.resize(p_record.relocations[RELOC_GDSCRIPT_UTILITY].size());
	for (int i = 0; i < p_function->gds_utilities.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_GDSCRIPT_UTILITY][i];
		if (GDScriptUtilityFunctions::get_function_info(symbol.name).get_compatibility_hash() != symbol.hash ||
				(p_function->gds_utilities.write[i] = GDScriptUtilityFunctions::get_function(symbol.name)) == nullptr) {
			return invalid_symbol();
		}
#ifdef DEBUG_ENABLED
		p_function->gds_utilities_names.push_back(symbol.name);
#endif
	}
	p_function->methods.resize(p_record.relocations[RELOC_METHOD_BIND].size());
	for (int i = 0; i < p_function->methods.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_METHOD_BIND][i];
		MethodBind *method = ClassDB::get_method(symbol.owner, symbol.name);
		if (method == nullptr || method->get_hash() != symbol.hash) {
			return invalid_symbol();
		}
		p_function->methods.write[i] = method;
	}
	p_function->lambdas.resize(p_record.relocations[RELOC_LAMBDA].size());
	for (int i = 0; i < p_function->lambdas.size(); i++) {
		const Symbol &symbol = p_record.relocations[RELOC_LAMBDA][i];
		GDScriptFunction *const *lambda = functions.getptr(symbol.name);
		if (lambda == nullptr || *lambda == p_function || lambda_function_identities.has(symbol.name)) {
			return invalid_symbol();
		}
		p_function->lambdas.write[i] = *lambda;
		lambda_function_identities.insert(symbol.name);
	}

	p_function->_operator_funcs_count = p_function->operator_funcs.size();
	p_function->_setters_count = p_function->setters.size();
	p_function->_getters_count = p_function->getters.size();
	p_function->_keyed_setters_count = p_function->keyed_setters.size();
	p_function->_keyed_getters_count = p_function->keyed_getters.size();
	p_function->_indexed_setters_count = p_function->indexed_setters.size();
	p_function->_indexed_getters_count = p_function->indexed_getters.size();
	p_function->_builtin_methods_count = p_function->builtin_methods.size();
	p_function->_constructors_count = p_function->constructors.size();
	p_function->_utilities_count = p_function->utilities.size();
	p_function->_gds_utilities_count = p_function->gds_utilities.size();
	p_function->_methods_count = p_function->methods.size();
	p_function->_lambdas_count = p_function->lambdas.size();
	return OK;
}

Error RuntimeBuilder::resolve_function_symbols() {
	resolved_functions.resize(module.functions.size());
	for (int function_index = 0; function_index < module.functions.size(); function_index++) {
		const FunctionRecord &record = module.functions[function_index];
		if (function_records.has(record.identity) || validate_function_code(record) != OK) {
			if (error.is_empty()) {
				fail(ERR_INVALID_DATA, "Duplicate function identity '" + record.identity + "'.");
			}
			return ERR_INVALID_DATA;
		}
		function_records.insert(record.identity, &record);
		ResolvedFunction &resolved = resolved_functions.write[function_index];
		for (const DataTypeRecord &type_record : record.argument_types) {
			GDScriptDataType type;
			if (decode_data_type(type_record, type) != OK) {
				return ERR_INVALID_DATA;
			}
			resolved.argument_types.push_back(type);
		}
		if (decode_data_type(record.return_type, resolved.return_type) != OK || decode_method(record.method, resolved.method) != OK ||
				decode_constant(record.rpc_config, resolved.rpc_config) != OK) {
			return ERR_INVALID_DATA;
		}
		for (const NamedConstantRecord &constant : record.local_constants) {
			if (constant.name.is_empty() || resolved.local_constants.has(constant.name)) {
				return fail(ERR_INVALID_DATA, "Invalid local constant table in '" + record.identity + "'.");
			}
			Variant value;
			if (decode_constant(constant.value, value) != OK) {
				return ERR_INVALID_DATA;
			}
			resolved.local_constants.insert(constant.name, value);
		}
		for (const ConstantData &constant : record.constants) {
			Variant value;
			if (decode_constant(constant, value) != OK) {
				return ERR_INVALID_DATA;
			}
			resolved.constants.push_back(value);
		}
		String global_error;
		if (!Internals::resolve_symbolic_global_code(record, resolved.code, &global_error)) {
			return fail(ERR_INVALID_DATA, global_error);
		}
	}
	return OK;
}

Error RuntimeBuilder::stage_functions() {
	for (const FunctionRecord &record : module.functions) {
		GDScriptFunction *function = memnew(GDScriptFunction);
		functions.insert(record.identity, function);
	}

	for (int function_index = 0; function_index < module.functions.size(); function_index++) {
		const FunctionRecord &record = module.functions[function_index];
		const ResolvedFunction &resolved = resolved_functions[function_index];
		GDScriptFunction *function = *functions.getptr(record.identity);
		function->name = record.name;
		function->source = record.source;
		function->_static = record.is_static;
		function->_initial_line = record.initial_line;
		function->_argument_count = record.argument_count;
		function->_vararg_index = record.vararg_index;
		function->_stack_size = record.stack_size;
		function->_instruction_args_size = record.instruction_args_size;
		function->_operator_feedback_count = record.operator_feedback_count;
		function->_call_feedback_count = record.call_feedback_count;
		function->_default_arg_count = record.default_argument_count;
		if (function->_operator_feedback_count > 0) {
			function->_operator_feedback_ptr = memnew_arr(SafeNumeric<uintptr_t>, function->_operator_feedback_count);
		}
		if (function->_call_feedback_count > 0) {
			function->_call_feedback_ptr = memnew_arr(SafeNumeric<uintptr_t>, function->_call_feedback_count);
		}

		function->argument_types = resolved.argument_types;
		function->return_type = resolved.return_type;
		function->method_info = resolved.method;
		function->rpc_config = resolved.rpc_config;
		function->constant_map = resolved.local_constants;
		function->constants = resolved.constants;
		function->_constant_count = function->constants.size();
		function->code = resolved.code;
		function->default_arguments = record.default_arguments;
		function->global_names = record.global_names;
		function->_global_names_count = function->global_names.size();
		for (const Pair<int, Variant::Type> &slot : record.temporary_slots) {
			function->temporary_slots.push_back(slot);
		}
#ifdef DEBUG_ENABLED
		function->func_cname = (String(function->source) + " - " + String(function->name)).utf8();
		function->_func_cname = function->func_cname.get_data();
		function->profile.signature = String(function->source) + "::" + String(function->name);
#endif
	}

	for (const FunctionRecord &record : module.functions) {
		if (resolve_function_relocations(record, *functions.getptr(record.identity)) != OK) {
			discard_functions();
			return ERR_INVALID_DATA;
		}
	}
	return OK;
}

void RuntimeBuilder::discard_functions() {
	for (const KeyValue<String, GDScriptFunction *> &entry : functions) {
		entry.value->lambdas.clear();
		entry.value->_lambdas_count = 0;
		entry.value->_lambdas_ptr = nullptr;
	}
	for (const KeyValue<String, GDScriptFunction *> &entry : functions) {
		memdelete(entry.value);
	}
	functions.clear();
}

void RuntimeBuilder::capture_shells(GDScript *p_script, HashSet<GDScript *> &r_visited) {
	if (p_script == nullptr || r_visited.has(p_script)) {
		return;
	}
	r_visited.insert(p_script);
	ShellSnapshot snapshot;
	snapshot.script = p_script;
	snapshot.owner = p_script->_owner;
	snapshot.path = p_script->path;
	snapshot.local_name = p_script->local_name;
	snapshot.global_name = p_script->global_name;
	snapshot.fully_qualified_name = p_script->fully_qualified_name;
	snapshot.icon_path = p_script->simplified_icon_path;
	for (const KeyValue<StringName, Ref<GDScript>> &entry : p_script->subclasses) {
		snapshot.subclasses.insert(entry.key, entry.value);
	}
	shell_snapshots.push_back(std::move(snapshot));
	for (const KeyValue<StringName, Ref<GDScript>> &entry : p_script->subclasses) {
		capture_shells(entry.value.ptr(), r_visited);
	}
}

void RuntimeBuilder::restore_shells() {
	for (ShellSnapshot &snapshot : shell_snapshots) {
		for (const KeyValue<StringName, Ref<GDScript>> &entry : snapshot.script->subclasses) {
			const Ref<GDScript> *original = snapshot.subclasses.getptr(entry.key);
			if (original == nullptr || *original != entry.value) {
				entry.value->_owner = nullptr;
			}
		}
		snapshot.script->subclasses.clear();
		for (const KeyValue<StringName, Ref<GDScript>> &entry : snapshot.subclasses) {
			snapshot.script->subclasses.insert(entry.key, entry.value);
		}
		snapshot.script->_owner = snapshot.owner;
		snapshot.script->path = snapshot.path;
		snapshot.script->local_name = snapshot.local_name;
		snapshot.script->global_name = snapshot.global_name;
		snapshot.script->fully_qualified_name = snapshot.fully_qualified_name;
		snapshot.script->simplified_icon_path = snapshot.icon_path;
	}
}

Error RuntimeBuilder::link_functions() {
	HashMap<GDScriptFunction *, GDScript *> function_owners;
	for (StagedClass &staged : staged_classes) {
		const ClassRecord &record = *staged.record;
		for (const MethodBindingRecord &binding : record.methods) {
			GDScriptFunction *const *function_ptr = functions.getptr(binding.identity);
			const FunctionRecord *const *function_record_ptr = function_records.getptr(binding.identity);
			if (binding.name.is_empty() || staged.member_functions.has(binding.name) || function_ptr == nullptr || function_record_ptr == nullptr) {
				return fail(ERR_INVALID_DATA, "Method binding refers to a missing function in class '" + record.identity + "'.");
			}
			const FunctionRecord &function_record = **function_record_ptr;
			Writer binding_signature;
			binding_signature.u32(binding.argument_types.size());
			for (const DataTypeRecord &type : binding.argument_types) {
				write_data_type(binding_signature, type);
			}
			write_data_type(binding_signature, binding.return_type);
			write_method(binding_signature, binding.method);
			write_constant(binding_signature, binding.rpc_config);
			Writer function_signature;
			function_signature.u32(function_record.argument_types.size());
			for (const DataTypeRecord &type : function_record.argument_types) {
				write_data_type(function_signature, type);
			}
			write_data_type(function_signature, function_record.return_type);
			write_method(function_signature, function_record.method);
			write_constant(function_signature, function_record.rpc_config);
			if (binding.name != function_record.name || binding.is_static != function_record.is_static ||
					binding.default_argument_count != function_record.default_argument_count || binding_signature.data != function_signature.data) {
				return fail(ERR_INVALID_DATA, "Method signature metadata mismatch for '" + binding.identity + "'.");
			}
			staged.member_functions.insert(binding.name, *function_ptr);
			root_function_identities.insert(binding.identity);
			if (GDScript *const *owner = function_owners.getptr(*function_ptr)) {
				if (*owner != staged.script) {
					return fail(ERR_INVALID_DATA, "A function is bound to multiple runtime classes.");
				}
			} else {
				function_owners.insert(*function_ptr, staged.script);
			}
		}

		auto bind_special = [&](const String &p_identity, GDScriptFunction *&r_target) -> Error {
			if (p_identity.is_empty()) {
				r_target = nullptr;
				return OK;
			}
			GDScriptFunction *const *function = functions.getptr(p_identity);
			if (function == nullptr) {
				return fail(ERR_INVALID_DATA, "Special initializer refers to missing function '" + p_identity + "'.");
			}
			r_target = *function;
			root_function_identities.insert(p_identity);
			if (GDScript *const *owner = function_owners.getptr(*function)) {
				if (*owner != staged.script) {
					return fail(ERR_INVALID_DATA, "An initializer is bound to multiple runtime classes.");
				}
			} else {
				function_owners.insert(*function, staged.script);
			}
			return OK;
		};
		if (bind_special(record.initializer, staged.initializer) != OK || bind_special(record.implicit_initializer, staged.implicit_initializer) != OK ||
				bind_special(record.implicit_ready, staged.implicit_ready) != OK || bind_special(record.static_initializer, staged.static_initializer) != OK) {
			return ERR_INVALID_DATA;
		}
	}

	for (const String &identity : root_function_identities) {
		if (lambda_function_identities.has(identity)) {
			return fail(ERR_INVALID_DATA, "Function '" + identity + "' is both a class entry point and a lambda.");
		}
	}
	if (root_function_identities.size() + lambda_function_identities.size() != functions.size()) {
		return fail(ERR_INVALID_DATA, "The compiled function graph contains an unowned or multiply owned function.");
	}

	Vector<GDScriptFunction *> queue;
	for (const String &identity : root_function_identities) {
		GDScriptFunction *function = *functions.getptr(identity);
		function->_script = *function_owners.getptr(function);
		queue.push_back(function);
	}
	for (int i = 0; i < queue.size(); i++) {
		GDScriptFunction *parent = queue[i];
		for (GDScriptFunction *lambda : parent->lambdas) {
			if (lambda->_script != nullptr && lambda->_script != parent->_script) {
				return fail(ERR_INVALID_DATA, "Lambda function is reachable from multiple script classes.");
			}
			if (lambda->_script == nullptr) {
				lambda->_script = parent->_script;
				queue.push_back(lambda);
			}
		}
	}
	for (const KeyValue<String, GDScriptFunction *> &entry : functions) {
		if (entry.value->_script == nullptr) {
			return fail(ERR_INVALID_DATA, "Could not determine owning script for function '" + entry.key + "'.");
		}
	}

	for (StagedClass &staged : staged_classes) {
		for (const LambdaRecord &lambda : staged.record->lambdas) {
			GDScriptFunction *const *function = functions.getptr(lambda.identity);
			if (function == nullptr || (*function)->_script != staged.script || staged.lambda_info.has(*function)) {
				return fail(ERR_INVALID_DATA, "Invalid lambda metadata in class '" + staged.record->identity + "'.");
			}
			staged.lambda_info.insert(*function, { lambda.capture_count, lambda.use_self });
		}
	}
	return OK;
}

void RuntimeBuilder::commit(bool p_keep_state) {
	HashMap<GDScriptFunction *, String> old_identities;
	Vector<GDScriptFunction *> old_function_list;
	Internals::collect_functions(root, "root", old_identities, old_function_list);
	HashMap<String, GDScriptFunction *> old_functions;
	RBSet<GDScriptFunction *> old_roots;
	for (GDScriptFunction *function : old_function_list) {
		old_functions.insert(*old_identities.getptr(function), function);
	}
	for (const StagedClass &staged : staged_classes) {
		for (const KeyValue<StringName, GDScriptFunction *> &entry : staged.script->member_functions) {
			old_roots.insert(entry.value);
		}
		if (staged.script->implicit_initializer != nullptr) {
			old_roots.insert(staged.script->implicit_initializer);
		}
		if (staged.script->implicit_ready != nullptr) {
			old_roots.insert(staged.script->implicit_ready);
		}
		if (staged.script->static_initializer != nullptr) {
			old_roots.insert(staged.script->static_initializer);
		}
		staged.script->_invalidate_function_call_caches();
		staged.script->cancel_pending_functions(true);
	}

	for (StagedClass &staged : staged_classes) {
		GDScript *script = staged.script;
		HashMap<StringName, Ref<GDScript>> old_subclasses;
		for (const KeyValue<StringName, Ref<GDScript>> &entry : script->subclasses) {
			old_subclasses.insert(entry.key, entry.value);
		}
		script->native = staged.native;
		script->base = staged.base;
		script->member_indices = staged.member_indices;
		script->members = staged.members;
		script->static_variables_indices = staged.static_variables_indices;
		script->static_variables = staged.static_variables;
		script->member_default_values = staged.member_default_values;
		script->struct_layouts = staged.struct_layouts;
		script->constants = staged.constants;
		script->member_functions = staged.member_functions;
		script->subclasses = staged.subclasses;
		script->_signals = staged.signals;
		script->rpc_config = staged.rpc_config;
		script->lambda_info = staged.lambda_info;
		script->initializer = staged.initializer;
		script->implicit_initializer = staged.implicit_initializer;
		script->implicit_ready = staged.implicit_ready;
		script->static_initializer = staged.static_initializer;
		script->tool = (staged.record->flags & CLASS_FLAG_TOOL) != 0;
		script->_is_abstract = (staged.record->flags & CLASS_FLAG_ABSTRACT) != 0;
		script->static_unload = (staged.record->flags & CLASS_FLAG_STATIC_UNLOAD) != 0;
		script->local_name = staged.record->local_name;
		script->global_name = staged.record->global_name;
		script->fully_qualified_name = staged.record->fully_qualified_name;
		script->simplified_icon_path = staged.record->icon_path;
		for (const KeyValue<StringName, Ref<GDScript>> &entry : old_subclasses) {
			if (!script->subclasses.has(entry.key) || script->subclasses[entry.key] != entry.value) {
				entry.value->_owner = nullptr;
				GDScriptLanguage::get_singleton()->add_orphan_subclass(entry.value->fully_qualified_name, entry.value->get_instance_id());
			}
		}
		script->_static_default_init();
		script->valid = true;
	}

	for (const FunctionRecord &record : module.functions) {
		GDScriptFunction *function = *functions.getptr(record.identity);
		function->_constants_ptr = function->constants.is_empty() ? nullptr : function->constants.ptrw();
		function->_constant_count = function->constants.size();
		Internals::install_record(record, function, functions);
	}

	HashMap<GDScriptFunction *, GDScriptFunction *> replacements;
	for (const KeyValue<String, GDScriptFunction *> &entry : old_functions) {
		GDScriptFunction *replacement = nullptr;
		if (GDScriptFunction *const *candidate = functions.getptr(entry.key)) {
			const int old_required = entry.value->_argument_count - entry.value->_default_arg_count;
			const int new_required = (*candidate)->_argument_count - (*candidate)->_default_arg_count;
			if (new_required <= old_required && (*candidate)->_argument_count >= old_required) {
				replacement = *candidate;
			}
		}
		replacements.insert(entry.value, replacement);
	}
	root->_recurse_replace_function_ptrs(replacements);

#ifdef DEBUG_ENABLED
	if (p_keep_state) {
		for (StagedClass &staged : staged_classes) {
			for (SelfList<GDScriptInstance> *instance = staged.script->instances.first(); instance != nullptr; instance = instance->next()) {
				instance->self()->reload_members();
			}
		}
	}
#endif

	for (GDScriptFunction *function : old_roots) {
		memdelete(function);
	}
	functions.clear(); // Ownership now belongs to the committed GDScript graph.
}

Error RuntimeBuilder::build(GDScript *p_script, const ParsedModule &p_module, bool p_keep_state, String *r_error) {
	root = p_script;
	module = p_module;
	HashSet<GDScript *> captured_shells;
	capture_shells(root, captured_shells);
	Error build_error = prepare_shell_graph(root, module, &classes, &error);
	if (build_error == OK) {
		build_error = stage_classes();
	}
	if (build_error == OK) {
		// Resolve every serialized type, script/resource constant, method
		// signature, RPC value, and process-local global operand before a VM
		// function is allocated. Runtime objects are installed only after this
		// complete module-wide symbolic pass succeeds.
		build_error = resolve_function_symbols();
	}
	if (build_error == OK) {
		build_error = stage_functions();
	}
	if (build_error == OK) {
		build_error = link_functions();
	}
	if (build_error != OK) {
		if (!functions.is_empty()) {
			discard_functions();
		}
		restore_shells();
		if (r_error != nullptr) {
			*r_error = error;
		}
		return build_error;
	}
	commit(p_keep_state);
	if (r_error != nullptr) {
		r_error->clear();
	}
	return OK;
}

} // namespace GDScriptCompiledModuleImplementation

using namespace GDScriptCompiledModuleImplementation;

uint64_t GDScriptCompiledModule::fingerprint_bytes(const uint8_t *p_data, uint64_t p_size) {
	if (p_size == 0 || p_data == nullptr) {
		return 0x9e3779b97f4a7c15ULL;
	}
	const uint32_t murmur = hash_murmur3_buffer(p_data, p_size);
	const uint32_t djb = hash_djb2_buffer(p_data, p_size);
	return (uint64_t(murmur) << 32) | djb;
}

uint64_t GDScriptCompiledModule::fingerprint_source(const String &p_source) {
	const CharString utf8 = p_source.utf8();
	return fingerprint_bytes(reinterpret_cast<const uint8_t *>(utf8.get_data()), utf8.length());
}

uint64_t GDScriptCompiledModule::get_engine_api_fingerprint() {
	uint32_t hash = HashMapHasherDefault::hash(GODOT_VERSION_FULL_CONFIG);
	hash = hash_murmur3_one_32(FORMAT_VERSION, hash);
	hash = hash_murmur3_one_32(BYTECODE_VERSION, hash);
	hash = hash_murmur3_one_32(GDScriptFunction::OPCODE_COUNT, hash);
	for (int opcode = 0; opcode < GDScriptFunction::OPCODE_COUNT; opcode++) {
		const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(GDScriptFunction::Opcode(opcode));
		hash = hash_murmur3_one_32(StringName(descriptor.name).hash(), hash);
		hash = hash_murmur3_one_32(descriptor.instruction_size, hash);
		hash = hash_murmur3_one_32(descriptor.operand_kinds.count, hash);
		hash = hash_murmur3_one_32(uint32_t(descriptor.operand_kinds.packed_kinds), hash);
		hash = hash_murmur3_one_32(uint32_t(descriptor.operand_kinds.packed_kinds >> 32), hash);
		hash = hash_murmur3_one_32(uint8_t(descriptor.result_operand), hash);
		hash = hash_murmur3_one_32(descriptor.control_flow_kind, hash);
		hash = hash_murmur3_one_32(descriptor.type_constraints, hash);
		hash = hash_murmur3_one_32(uint8_t(descriptor.relocation_kind), hash);
	}
	hash = hash_murmur3_one_32(Variant::VARIANT_MAX, hash);
	hash = hash_murmur3_one_32(sizeof(real_t), hash);

	for (int type = 0; type < Variant::VARIANT_MAX; type++) {
		List<StringName> members;
		Variant::get_member_list(Variant::Type(type), &members);
		members.sort_custom<StringName::AlphCompare>();
		for (const StringName &member : members) {
			hash = hash_murmur3_one_32(member.hash(), hash);
			hash = hash_murmur3_one_32(Variant::get_member_type(Variant::Type(type), member), hash);
		}
		List<StringName> methods;
		Variant::get_builtin_method_list(Variant::Type(type), &methods);
		methods.sort_custom<StringName::AlphCompare>();
		for (const StringName &method : methods) {
			hash = hash_murmur3_one_32(method.hash(), hash);
			hash = hash_murmur3_one_32(Variant::get_builtin_method_hash(Variant::Type(type), method), hash);
		}
		List<MethodInfo> constructors;
		Variant::get_constructor_list(Variant::Type(type), &constructors);
		for (const MethodInfo &constructor : constructors) {
			hash = hash_murmur3_one_32(constructor.get_compatibility_hash(), hash);
		}
	}
	List<StringName> utilities;
	Variant::get_utility_function_list(&utilities);
	utilities.sort_custom<StringName::AlphCompare>();
	for (const StringName &utility : utilities) {
		hash = hash_murmur3_one_32(utility.hash(), hash);
		hash = hash_murmur3_one_32(Variant::get_utility_function_hash(utility), hash);
	}
	List<StringName> gdscript_utilities;
	GDScriptUtilityFunctions::get_function_list(&gdscript_utilities);
	gdscript_utilities.sort_custom<StringName::AlphCompare>();
	for (const StringName &utility : gdscript_utilities) {
		hash = hash_murmur3_one_32(utility.hash(), hash);
		hash = hash_murmur3_one_32(GDScriptUtilityFunctions::get_function_info(utility).get_compatibility_hash(), hash);
	}
	const uint32_t mixed = hash_fmix32(hash);
	const uint32_t tail = hash_fmix32(hash_murmur3_one_32(HashMapHasherDefault::hash(GODOT_VERSION_FULL_CONFIG), mixed ^ HASH_MURMUR3_SEED));
	return (uint64_t(mixed) << 32) | tail;
}

Error GDScriptCompiledModule::create(GDScript *p_script, const Vector<uint8_t> &p_fallback_tokens, Vector<uint8_t> &r_module, Summary *r_summary) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_fallback_tokens.is_empty(), ERR_INVALID_PARAMETER);

	HashMap<GDScriptFunction *, String> identities;
	Vector<GDScriptFunction *> functions;
	Internals::collect_functions(p_script, "root", identities, functions);
	HashMap<GDScript *, String> class_identities;
	Vector<GDScript *> classes;
	Internals::collect_classes(p_script, "root", class_identities, classes);
	Vector<ClassRecord> class_records;
	for (GDScript *script_class : classes) {
		ClassRecord record;
		if (!Internals::make_class_record(script_class, *class_identities.getptr(script_class), class_identities, identities, record)) {
			return ERR_UNAVAILABLE;
		}
		class_records.push_back(record);
	}

	Vector<FunctionRecord> records;
	int skipped_functions = 0;
	for (GDScriptFunction *function : functions) {
		FunctionRecord record;
		if (Internals::make_record(function, *identities.getptr(function), identities, record)) {
			records.push_back(record);
		} else {
			skipped_functions++;
		}
	}

	HashMap<String, uint64_t> dependency_map;
	Internals::collect_dependencies_from_script(p_script, p_script, dependency_map);
	Vector<Dependency> dependencies;
	for (const KeyValue<String, uint64_t> &entry : dependency_map) {
		Dependency dependency;
		dependency.path = entry.key;
		dependency.source_fingerprint = entry.value;
		dependencies.push_back(dependency);
	}
	dependencies.sort();

	const uint64_t source_fingerprint = fingerprint_source(p_script->source);
	const uint64_t engine_api_fingerprint = get_engine_api_fingerprint();
	ParsedModule candidate;
	candidate.path = GDScript::canonicalize_path(p_script->get_script_path());
	candidate.source_fingerprint = source_fingerprint;
	candidate.engine_api_fingerprint = engine_api_fingerprint;
	candidate.fallback_tokens = p_fallback_tokens;
	candidate.dependencies = dependencies;
	candidate.classes = class_records;
	candidate.functions = records;
	String candidate_error;
	if (ModuleVerifier(candidate).verify(&candidate_error) != OK) {
		// Keep export/cache creation fail-closed as well. The caller already has
		// the binary-token fallback when a function cannot be represented by the
		// portable module or a compiler invariant is violated.
		ERR_PRINT("GDScript compiler produced a non-portable module for '" + candidate.path + "': " + candidate_error);
		return ERR_UNAVAILABLE;
	}
	Writer payload;
	payload.string(candidate.path);
	payload.bytes(p_fallback_tokens);
	payload.u32(dependencies.size());
	for (const Dependency &dependency : dependencies) {
		payload.string(dependency.path);
		payload.u64(dependency.source_fingerprint);
	}
	payload.u32(class_records.size());
	for (const ClassRecord &record : class_records) {
		write_class(payload, record);
	}
	payload.u32(records.size());
	for (const FunctionRecord &record : records) {
		write_function(payload, record);
	}

	Writer module;
	module.u32(MODULE_MAGIC);
	module.u32(FORMAT_VERSION);
	module.u32(BYTECODE_VERSION);
	module.u64(engine_api_fingerprint);
	module.u64(source_fingerprint);
	module.u64(fingerprint_bytes(payload.data.ptr(), payload.data.size()));
	module.u32(payload.data.size());
	const int header_size = module.data.size();
	module.data.resize(header_size + payload.data.size());
	memcpy(module.data.ptrw() + header_size, payload.data.ptr(), payload.data.size());
	r_module = module.data;

	if (r_summary != nullptr) {
		*r_summary = Summary();
		r_summary->path = GDScript::canonicalize_path(p_script->get_script_path());
		r_summary->source_fingerprint = source_fingerprint;
		r_summary->engine_api_fingerprint = engine_api_fingerprint;
		r_summary->dependencies = dependencies;
		r_summary->skipped_functions = skipped_functions;
		for (const ClassRecord &record : class_records) {
			Writer metadata;
			write_class(metadata, record);
			ClassSummary summary;
			summary.identity = record.identity;
			summary.metadata_fingerprint = fingerprint_bytes(metadata.data.ptr(), metadata.data.size());
			r_summary->classes.push_back(summary);
		}
		r_summary->classes.sort();
		for (const FunctionRecord &record : records) {
			FunctionSummary summary;
			summary.identity = record.identity;
			summary.bytecode_fingerprint = record.fingerprint;
			summary.profile_guided = record.profile_guided;
			r_summary->functions.push_back(summary);
		}
		r_summary->functions.sort();
	}
	return OK;
}

Error GDScriptCompiledModule::extract_fallback(const Vector<uint8_t> &p_module, Vector<uint8_t> &r_fallback_tokens, uint64_t *r_source_fingerprint, String *r_error) {
	ParsedModule module;
	const Error error = parse_module(p_module, module, r_error);
	if (error != OK) {
		return error;
	}
	r_fallback_tokens = module.fallback_tokens;
	if (r_source_fingerprint != nullptr) {
		*r_source_fingerprint = module.source_fingerprint;
	}
	return OK;
}

Error GDScriptCompiledModule::verify(const Vector<uint8_t> &p_module, String *r_error) {
	ParsedModule module;
	const Error parse_error = parse_module(p_module, module, r_error);
	if (parse_error != OK) {
		return parse_error;
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		if (r_error != nullptr) {
			*r_error = "Engine/Variant API fingerprint mismatch.";
		}
		return ERR_INVALID_DATA;
	}
	ModuleVerifier verifier(module);
	return verifier.verify(r_error);
}

Error GDScriptCompiledModule::prepare_shallow(GDScript *p_script, const Vector<uint8_t> &p_module, String *r_error) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	ParsedModule module;
	Error parse_error = parse_module(p_module, module, r_error);
	if (parse_error != OK) {
		return parse_error;
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		if (r_error != nullptr) {
			*r_error = "Engine/Variant API fingerprint mismatch.";
		}
		return ERR_INVALID_DATA;
	}
	if ((!p_script->source.is_empty() && module.source_fingerprint != fingerprint_source(p_script->source)) ||
			(!p_script->binary_tokens.is_empty() && module.fallback_tokens != p_script->binary_tokens)) {
		if (r_error != nullptr) {
			*r_error = "Source or binary-token fingerprint mismatch.";
		}
		return ERR_INVALID_DATA;
	}
	ModuleVerifier verifier(module);
	if (verifier.verify(r_error) != OK) {
		return ERR_INVALID_DATA;
	}
	return RuntimeBuilder::prepare_shell_graph(p_script, module, nullptr, r_error);
}

Error GDScriptCompiledModule::build_runtime(GDScript *p_script, const Vector<uint8_t> &p_module, bool p_keep_state, String *r_error) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	ParsedModule module;
	Error parse_error = parse_module(p_module, module, r_error);
	if (parse_error != OK) {
		return parse_error;
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		if (r_error != nullptr) {
			*r_error = "Engine/Variant API fingerprint mismatch.";
		}
		return ERR_INVALID_DATA;
	}
	if ((!p_script->source.is_empty() && module.source_fingerprint != fingerprint_source(p_script->source)) ||
			(!p_script->binary_tokens.is_empty() && module.fallback_tokens != p_script->binary_tokens)) {
		if (r_error != nullptr) {
			*r_error = "Source or binary-token fingerprint mismatch.";
		}
		return ERR_INVALID_DATA;
	}
	ModuleVerifier verifier(module);
	if (verifier.verify(r_error) != OK) {
		return ERR_INVALID_DATA;
	}
	for (const Dependency &dependency : module.dependencies) {
		if (!validate_dependency(dependency)) {
			if (r_error != nullptr) {
				*r_error = "Dependency fingerprint mismatch for '" + dependency.path + "'.";
			}
			return ERR_INVALID_DATA;
		}
	}
	RuntimeBuilder builder;
	return builder.build(p_script, module, p_keep_state, r_error);
}

Error GDScriptCompiledModule::apply(GDScript *p_script, const Vector<uint8_t> &p_module, String *r_error) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	ParsedModule module;
	Error error = parse_module(p_module, module, r_error);
	if (error != OK) {
		return error;
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		if (r_error != nullptr) {
			*r_error = "Engine/Variant API fingerprint mismatch.";
		}
		return ERR_INVALID_DATA;
	}
	if ((!p_script->source.is_empty() && module.source_fingerprint != fingerprint_source(p_script->source)) ||
			(!p_script->binary_tokens.is_empty() && module.fallback_tokens != p_script->binary_tokens)) {
		if (r_error != nullptr) {
			*r_error = "Source or binary-token fingerprint mismatch.";
		}
		return ERR_INVALID_DATA;
	}
	ModuleVerifier verifier(module);
	if (verifier.verify(r_error) != OK) {
		return ERR_INVALID_DATA;
	}
	for (const Dependency &dependency : module.dependencies) {
		if (!validate_dependency(dependency)) {
			if (r_error != nullptr) {
				*r_error = "Dependency fingerprint mismatch for '" + dependency.path + "'.";
			}
			return ERR_INVALID_DATA;
		}
	}

	HashMap<GDScriptFunction *, String> identities;
	Vector<GDScriptFunction *> function_list;
	Internals::collect_functions(p_script, "root", identities, function_list);
	HashMap<GDScript *, String> class_identities;
	Vector<GDScript *> class_list;
	Internals::collect_classes(p_script, "root", class_identities, class_list);
	HashMap<String, GDScript *> classes;
	for (GDScript *script_class : class_list) {
		classes.insert(*class_identities.getptr(script_class), script_class);
	}
	if (module.classes.size() != classes.size()) {
		if (r_error != nullptr) {
			*r_error = "Compiled class table does not match the freshly compiled script.";
		}
		return ERR_INVALID_DATA;
	}
	HashSet<String> validated_classes;
	for (const ClassRecord &record : module.classes) {
		GDScript *const *script_class = classes.getptr(record.identity);
		if (record.identity.is_empty() || validated_classes.has(record.identity) || script_class == nullptr ||
				!Internals::validate_class_record(record, *script_class, class_identities, identities)) {
			if (r_error != nullptr) {
				*r_error = "Class metadata verification failed for '" + record.identity + "'.";
			}
			return ERR_INVALID_DATA;
		}
		validated_classes.insert(record.identity);
	}
	HashMap<String, GDScriptFunction *> functions;
	for (GDScriptFunction *function : function_list) {
		functions.insert(*identities.getptr(function), function);
	}
	for (const FunctionRecord &record : module.functions) {
		GDScriptFunction *const *function = functions.getptr(record.identity);
		String verification_error;
		if (function == nullptr || !Internals::validate_record(record, *function, functions, &verification_error)) {
			if (r_error != nullptr) {
				*r_error = "Bytecode verification or symbolic relocation failed for '" + record.identity + "': " +
						(function == nullptr ? String("function is missing") : verification_error) + ".";
			}
			return ERR_INVALID_DATA;
		}
	}
	for (const FunctionRecord &record : module.functions) {
		Internals::install_record(record, *functions.getptr(record.identity), functions);
	}
	return OK;
}

String GDScriptCompiledModule::get_editor_cache_path(const String &p_script_path) {
	const String key = GDScript::canonicalize_path(p_script_path) + ":" + String::num_uint64(get_engine_api_fingerprint()) + ":" + itos(FORMAT_VERSION);
	return "res://.godot/gdscript_compiled/" + key.sha256_text() + ".gdm";
}

Error GDScriptCompiledModule::load_editor_cache(const String &p_script_path, const String &p_source, Vector<uint8_t> &r_module) {
	const String cache_path = get_editor_cache_path(p_script_path);
	if (!FileAccess::exists(cache_path)) {
		return ERR_FILE_NOT_FOUND;
	}
	Vector<uint8_t> module_bytes = FileAccess::get_file_as_bytes(cache_path);
	ParsedModule module;
	if (parse_module(module_bytes, module, nullptr) != OK || module.engine_api_fingerprint != get_engine_api_fingerprint() ||
			module.source_fingerprint != fingerprint_source(p_source) || ModuleVerifier(module).verify(nullptr) != OK) {
		return ERR_INVALID_DATA;
	}
	r_module = module_bytes;
	return OK;
}

Error GDScriptCompiledModule::save_editor_cache(GDScript *p_script, const Vector<uint8_t> &p_fallback_tokens, Vector<uint8_t> *r_module) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	Vector<uint8_t> module;
	Error error = create(p_script, p_fallback_tokens, module);
	if (error != OK) {
		return error;
	}
	const String cache_path = get_editor_cache_path(p_script->get_script_path());
	error = DirAccess::make_dir_recursive_absolute(cache_path.get_base_dir());
	if (error != OK) {
		return error;
	}
	Ref<FileAccess> file = FileAccess::open(cache_path, FileAccess::WRITE, &error);
	if (file.is_null()) {
		return error;
	}
	if (!file->store_buffer(module)) {
		return ERR_CANT_CREATE;
	}
	if (r_module != nullptr) {
		*r_module = module;
	}
	return OK;
}

Vector<uint8_t> GDScriptCompiledModule::create_project_manifest(Vector<Summary> p_modules) {
	p_modules.sort();
	Writer payload;
	payload.u64(get_engine_api_fingerprint());
	payload.u32(p_modules.size());
	for (Summary &module : p_modules) {
		module.dependencies.sort();
		module.functions.sort();
		payload.string(module.path);
		payload.u64(module.source_fingerprint);
		payload.u64(module.engine_api_fingerprint);
		payload.u32(uint32_t(module.skipped_functions));
		payload.u32(module.dependencies.size());
		for (const Dependency &dependency : module.dependencies) {
			payload.string(dependency.path);
			payload.u64(dependency.source_fingerprint);
		}
		payload.u32(module.classes.size());
		for (const ClassSummary &script_class : module.classes) {
			payload.string(script_class.identity);
			payload.u64(script_class.metadata_fingerprint);
		}
		payload.u32(module.functions.size());
		for (const FunctionSummary &function : module.functions) {
			payload.string(function.identity);
			payload.u32(function.bytecode_fingerprint);
			payload.u32(function.profile_guided ? 1 : 0);
		}
	}
	Writer manifest;
	manifest.u32(MANIFEST_MAGIC);
	manifest.u32(1);
	manifest.u64(fingerprint_bytes(payload.data.ptr(), payload.data.size()));
	manifest.u32(payload.data.size());
	const int header_size = manifest.data.size();
	manifest.data.resize(header_size + payload.data.size());
	memcpy(manifest.data.ptrw() + header_size, payload.data.ptr(), payload.data.size());
	return manifest.data;
}
