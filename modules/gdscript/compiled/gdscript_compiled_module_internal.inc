/**************************************************************************/
/*  gdscript_compiled_module_internal.inc                                 */
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

#include "../gdscript.h"
#include "../gdscript_cache.h"
#include "../jit/gdscript_optimization_profile.h"
#include "../gdscript_utility_functions.h"

#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
#include "../jit/gdscript_baseline_jit.h"
#endif

#include "core/config/engine.h"
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
	CONSTANT_ENGINE_SINGLETON,
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
	uint64_t collection_entries = 0;
	bool failed = false;

	Reader() = default;
	Reader(const uint8_t *p_data, uint64_t p_size) :
			data(p_data), size(p_size) {}

	uint32_t u32() {
		if (failed || offset > size || size - offset < 4) {
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
		if (failed || length > p_limit || offset > size || length > size - offset) {
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
		// Every serialized collection element consumes at least one 32-bit word.
		// Reject impossible and aggregate-excessive counts before resize() can
		// amplify a tiny hostile payload into a large allocation.
		if (failed || offset > size || value > p_limit || value > (size - offset) / 4 ||
				collection_entries > MAX_TOTAL_METADATA_ENTRIES - value) {
			failed = true;
			return 0;
		}
		collection_entries += value;
		return value;
	}
};

struct ModuleEnvelope {
	uint32_t format_version = 0;
	uint32_t bytecode_version = 0;
	uint32_t flags = 0;
	uint64_t engine_api_fingerprint = 0;
	uint64_t source_fingerprint = 0;
	uint64_t payload_fingerprint = 0;
	uint64_t fallback_fingerprint = 0;
	Vector<uint8_t> fallback_tokens;
	uint64_t payload_offset = 0;
	uint32_t payload_size = 0;
};

void set_rejection(GDScriptCompiledModule::Rejection *r_rejection, GDScriptCompiledModule::RejectionReason p_reason,
		const String &p_detail, bool p_fallback_available) {
	if (r_rejection == nullptr) {
		return;
	}
	r_rejection->reason = p_reason;
	r_rejection->detail = p_detail;
	r_rejection->fallback_available = p_fallback_available;
}

Error read_envelope(const Vector<uint8_t> &p_module, ModuleEnvelope &r_envelope, String *r_error,
		GDScriptCompiledModule::Rejection *r_rejection = nullptr) {
	auto fail = [&](const String &p_message) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		set_rejection(r_rejection, GDScriptCompiledModule::REJECTION_CORRUPT_MODULE, p_message, false);
		return ERR_FILE_CORRUPT;
	};
	Reader header(p_module.ptr(), p_module.size());
	if (header.u32() != MODULE_MAGIC) {
		return fail("Not a compiled GDScript module.");
	}
	if (header.u32() != GDScriptCompiledModule::ENVELOPE_VERSION) {
		return fail("Unsupported compiled GDScript envelope version.");
	}
	r_envelope.format_version = header.u32();
	r_envelope.bytecode_version = header.u32();
	r_envelope.flags = header.u32();
	r_envelope.engine_api_fingerprint = header.u64();
	r_envelope.source_fingerprint = header.u64();
	r_envelope.payload_fingerprint = header.u64();
	r_envelope.fallback_fingerprint = header.u64();
	const uint32_t fallback_size = header.u32();
	r_envelope.payload_size = header.u32();
	const uint64_t remaining_size = header.offset <= header.size ? header.size - header.offset : 0;
	if (header.failed || header.offset != GDScriptCompiledModule::ENVELOPE_HEADER_SIZE || fallback_size > MAX_BLOB_SIZE ||
			r_envelope.payload_size > MAX_BLOB_SIZE || fallback_size > remaining_size || r_envelope.payload_size != remaining_size - fallback_size) {
		return fail("Invalid compiled GDScript envelope sizes.");
	}
	if (fallback_size > 0) {
		r_envelope.fallback_tokens.resize(fallback_size);
		memcpy(r_envelope.fallback_tokens.ptrw(), p_module.ptr() + header.offset, fallback_size);
		if (GDScriptCompiledModule::fingerprint_bytes(r_envelope.fallback_tokens.ptr(), r_envelope.fallback_tokens.size()) != r_envelope.fallback_fingerprint) {
			return fail("Embedded GDScript token fallback checksum mismatch.");
		}
	}
	r_envelope.payload_offset = header.offset + fallback_size;
	return OK;
}

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

struct DebugMemberRecord {
	String name;
	int32_t line = 0;

	bool operator<(const DebugMemberRecord &p_other) const { return name < p_other.name; }
};

struct DebugSourcePositionRecord {
	int32_t code_offset = 0;
	int32_t line = 0;
	int32_t column = 0;
};

struct DebugLocalRecord {
	int32_t line = 0;
	int32_t slot = 0;
	bool added = false;
	String name;
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
	Vector<DebugMemberRecord> debug_members;

	bool operator<(const ClassRecord &p_other) const { return identity < p_other.identity; }
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
	String debug_profile_identifier;
	Vector<DebugSourcePositionRecord> debug_source_positions;
	Vector<DebugLocalRecord> debug_locals;

	bool operator<(const FunctionRecord &p_other) const { return identity < p_other.identity; }
};

struct ParsedModule {
	String path;
	uint64_t source_fingerprint = 0;
	uint64_t module_fingerprint = 0;
	uint64_t schema_fingerprint = 0;
	uint64_t dependency_fingerprint = 0;
	uint64_t engine_api_fingerprint = 0;
	Vector<uint8_t> fallback_tokens;
	Vector<GDScriptCompiledModule::Dependency> dependencies;
	Vector<ClassRecord> classes;
	Vector<FunctionRecord> functions;
	bool has_debug_info = false;
};

class Internals {
public:
	static bool make_relocations(const GDScriptFunction *p_function, const HashMap<GDScriptFunction *, String> &p_identities, Vector<Vector<Symbol>> &r_tables);
	static void add_function_tree(GDScriptFunction *p_function, const String &p_identity, HashMap<GDScriptFunction *, String> &r_identities, Vector<GDScriptFunction *> &r_functions);
	static void collect_functions(GDScript *p_script, const String &p_class_identity, HashMap<GDScriptFunction *, String> &r_identities, Vector<GDScriptFunction *> &r_functions);
	static void collect_classes(GDScript *p_script, const String &p_identity, HashMap<GDScript *, String> &r_identities, Vector<GDScript *> &r_classes);
	static uint64_t dependency_source_fingerprint(GDScript *p_script);
	static void add_script_dependency(GDScript *p_root, GDScript *p_dependency, HashMap<String, GDScript *> &r_dependencies);
	static void collect_dependency_from_data_type(GDScript *p_root, const GDScriptDataType &p_type, HashMap<String, GDScript *> &r_dependencies);
	static void collect_dependencies_from_function(GDScript *p_root, GDScriptFunction *p_function, HashMap<String, GDScript *> &r_dependencies, HashSet<GDScriptFunction *> &r_visited);
	static void collect_dependencies_from_script(GDScript *p_root, GDScript *p_script, HashMap<String, GDScript *> &r_dependencies);
	static void collect_dependency_closure(GDScript *p_root, GDScript *p_script, HashMap<String, GDScript *> &r_dependencies, HashSet<String> &r_visited);
	static bool make_local_records(GDScript *p_script, Vector<ClassRecord> &r_classes, Vector<FunctionRecord> &r_functions, int *r_skipped_functions = nullptr);
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
	GDScriptCompiledModule::RejectionReason failure_context = GDScriptCompiledModule::REJECTION_INVALID_BYTECODE;

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
	Error validate_debug_metadata();
	Error validate_final_limits();

public:
	explicit ModuleVerifier(const ParsedModule &p_module) :
			module(p_module) {}
	Error verify(String *r_error = nullptr);
	GDScriptCompiledModule::RejectionReason get_rejection_reason() const { return failure_context; }
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
