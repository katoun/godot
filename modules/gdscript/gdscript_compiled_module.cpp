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
#include "core/version.h"

namespace GDScriptCompiledModuleImplementation {

constexpr uint32_t MODULE_MAGIC = 0x4d534447; // "GDSM", little endian.
constexpr uint32_t MANIFEST_MAGIC = 0x4d504447; // "GDPM", little endian.
constexpr uint32_t MAX_COLLECTION_SIZE = 1 << 20;
constexpr uint32_t MAX_BLOB_SIZE = 256 << 20;

enum ConstantKind : uint32_t {
	CONSTANT_VARIANT,
	CONSTANT_RESOURCE,
	CONSTANT_GDSCRIPT,
	CONSTANT_NATIVE_CLASS,
};

enum ClassFlags : uint32_t {
	CLASS_FLAG_TOOL = 1 << 0,
	CLASS_FLAG_ABSTRACT = 1 << 1,
	CLASS_FLAG_MASK = CLASS_FLAG_TOOL | CLASS_FLAG_ABSTRACT,
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

	Vector<uint8_t> bytes() {
		Vector<uint8_t> result;
		const uint32_t length = u32();
		if (failed || length > MAX_BLOB_SIZE || offset + length > size) {
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
		const Vector<uint8_t> value = bytes();
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

	uint32_t count() {
		const uint32_t value = u32();
		if (value > MAX_COLLECTION_SIZE) {
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

void write_int_vector(Writer &p_writer, const Vector<int> &p_values) {
	p_writer.u32(p_values.size());
	for (int value : p_values) {
		p_writer.u32(uint32_t(value));
	}
}

Vector<int> read_int_vector(Reader &p_reader) {
	Vector<int> values;
	const uint32_t count = p_reader.count();
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
	record.code = read_int_vector(p_reader);
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
	const uint32_t dependency_count = payload.count();
	r_module.dependencies.resize(dependency_count);
	for (uint32_t i = 0; i < dependency_count; i++) {
		r_module.dependencies.write[i].path = payload.string();
		r_module.dependencies.write[i].source_fingerprint = payload.u64();
	}
	const uint32_t class_count = payload.count();
	r_module.classes.resize(class_count);
	for (uint32_t i = 0; i < class_count; i++) {
		r_module.classes.write[i] = read_class(payload);
	}
	const uint32_t function_count = payload.count();
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
	r_record.flags = (p_script->tool ? uint32_t(CLASS_FLAG_TOOL) : 0) | (p_script->_is_abstract ? uint32_t(CLASS_FLAG_ABSTRACT) : 0);
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
	r_record.code = p_function->code;
	r_record.default_arguments = p_function->default_arguments;
	r_record.global_names = p_function->global_names;
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
		if (GDScriptUtilityFunctions::get_function(s.name) != p_function->gds_utilities[i]) {
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
	if (p_record.code != p_function->code) {
		return fail("bytecode mismatch");
	}
	if (p_record.default_arguments != p_function->default_arguments || p_record.global_names != p_function->global_names) {
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

	p_function->code = p_record.code;
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
	hash = hash_murmur3_one_32(GDScriptFunction::OPCODE_END, hash);
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
	Writer payload;
	payload.string(GDScript::canonicalize_path(p_script->get_script_path()));
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
			module.source_fingerprint != fingerprint_source(p_source)) {
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
