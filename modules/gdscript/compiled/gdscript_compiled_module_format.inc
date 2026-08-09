/**************************************************************************/
/*  gdscript_compiled_module_format.inc                                   */
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

// Pointer-free binary format encoding and decoding.
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
	if (constant.kind > CONSTANT_ENGINE_SINGLETON) {
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

Vector<uint8_t> write_debug_section(const Vector<ClassRecord> &p_classes, const Vector<FunctionRecord> &p_functions) {
	Writer debug;
	debug.u32(GDScriptCompiledModule::DEBUG_INFO_VERSION);
	debug.u32(p_classes.size());
	for (const ClassRecord &script_class : p_classes) {
		debug.string(script_class.identity);
		debug.u32(script_class.debug_members.size());
		for (const DebugMemberRecord &member : script_class.debug_members) {
			debug.string(member.name);
			debug.u32(uint32_t(member.line));
		}
	}
	debug.u32(p_functions.size());
	for (const FunctionRecord &function : p_functions) {
		debug.string(function.identity);
		debug.string(function.debug_profile_identifier);
		debug.u32(function.debug_source_positions.size());
		for (const DebugSourcePositionRecord &position : function.debug_source_positions) {
			debug.u32(uint32_t(position.code_offset));
			debug.u32(uint32_t(position.line));
			debug.u32(uint32_t(position.column));
		}
		debug.u32(function.debug_locals.size());
		for (const DebugLocalRecord &local : function.debug_locals) {
			debug.u32(uint32_t(local.line));
			debug.u32(uint32_t(local.slot));
			debug.u32(local.added ? 1 : 0);
			debug.string(local.name);
		}
	}
	return debug.data;
}

bool read_debug_section(const Vector<uint8_t> &p_data, ParsedModule &r_module) {
	if (p_data.is_empty()) {
		return true;
	}
	Reader debug(p_data.ptr(), p_data.size());
	if (debug.u32() != GDScriptCompiledModule::DEBUG_INFO_VERSION) {
		return false;
	}
	HashMap<String, int> classes;
	for (int i = 0; i < r_module.classes.size(); i++) {
		classes.insert(r_module.classes[i].identity, i);
	}
	HashSet<String> decoded_classes;
	const uint32_t class_count = debug.count(MAX_MODULE_CLASSES);
	if (class_count != uint32_t(r_module.classes.size())) {
		return false;
	}
	for (uint32_t i = 0; i < class_count; i++) {
		const String identity = debug.string();
		const int *class_index = classes.getptr(identity);
		if (class_index == nullptr || decoded_classes.has(identity)) {
			return false;
		}
		decoded_classes.insert(identity);
		ClassRecord &script_class = r_module.classes.write[*class_index];
		const uint32_t member_count = debug.count();
		script_class.debug_members.resize(member_count);
		for (uint32_t member_index = 0; member_index < member_count; member_index++) {
			DebugMemberRecord &member = script_class.debug_members.write[member_index];
			member.name = debug.string();
			member.line = int32_t(debug.u32());
		}
	}

	HashMap<String, int> functions;
	for (int i = 0; i < r_module.functions.size(); i++) {
		functions.insert(r_module.functions[i].identity, i);
	}
	HashSet<String> decoded_functions;
	const uint32_t function_count = debug.count(MAX_MODULE_FUNCTIONS);
	if (function_count != uint32_t(r_module.functions.size())) {
		return false;
	}
	for (uint32_t i = 0; i < function_count; i++) {
		const String identity = debug.string();
		const int *function_index = functions.getptr(identity);
		if (function_index == nullptr || decoded_functions.has(identity)) {
			return false;
		}
		decoded_functions.insert(identity);
		FunctionRecord &function = r_module.functions.write[*function_index];
		function.debug_profile_identifier = debug.string();
		const uint32_t position_count = debug.count(MAX_FUNCTION_CODE_WORDS);
		function.debug_source_positions.resize(position_count);
		for (uint32_t position_index = 0; position_index < position_count; position_index++) {
			DebugSourcePositionRecord &position = function.debug_source_positions.write[position_index];
			position.code_offset = int32_t(debug.u32());
			position.line = int32_t(debug.u32());
			position.column = int32_t(debug.u32());
		}
		const uint32_t local_count = debug.count();
		function.debug_locals.resize(local_count);
		for (uint32_t local_index = 0; local_index < local_count; local_index++) {
			DebugLocalRecord &local = function.debug_locals.write[local_index];
			local.line = int32_t(debug.u32());
			local.slot = int32_t(debug.u32());
			const uint32_t added = debug.u32();
			if (added > 1) {
				debug.failed = true;
			}
			local.added = added != 0;
			local.name = debug.string();
		}
	}
	if (debug.failed || debug.offset != debug.size) {
		return false;
	}
	r_module.has_debug_info = true;
	return true;
}

Error serialize_module(const ParsedModule &p_module, Vector<uint8_t> &r_module) {
	if (p_module.fallback_tokens.is_empty() || p_module.classes.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	Writer payload;
	payload.string(p_module.path);
	payload.u64(p_module.module_fingerprint);
	payload.u64(p_module.schema_fingerprint);
	payload.u64(p_module.dependency_fingerprint);
	payload.u32(p_module.dependencies.size());
	for (const GDScriptCompiledModule::Dependency &dependency : p_module.dependencies) {
		payload.string(dependency.path);
		payload.u64(dependency.source_fingerprint);
		payload.u64(dependency.module_fingerprint);
		payload.u64(dependency.schema_fingerprint);
		payload.u64(dependency.engine_api_fingerprint);
	}
	payload.u32(p_module.classes.size());
	for (const ClassRecord &record : p_module.classes) {
		write_class(payload, record);
	}
	payload.u32(p_module.functions.size());
	for (const FunctionRecord &record : p_module.functions) {
		write_function(payload, record);
	}
	payload.bytes(p_module.has_debug_info ? write_debug_section(p_module.classes, p_module.functions) : Vector<uint8_t>());

	Writer module;
	module.u32(MODULE_MAGIC);
	module.u32(GDScriptCompiledModule::ENVELOPE_VERSION);
	module.u32(GDScriptCompiledModule::FORMAT_VERSION);
	module.u32(GDScriptCompiledModule::BYTECODE_VERSION);
	module.u32(GDScriptCompiledModule::ENVELOPE_FLAG_HAS_FALLBACK);
	module.u64(p_module.engine_api_fingerprint);
	module.u64(p_module.source_fingerprint);
	module.u64(GDScriptCompiledModule::fingerprint_bytes(payload.data.ptr(), payload.data.size()));
	module.u64(GDScriptCompiledModule::fingerprint_bytes(p_module.fallback_tokens.ptr(), p_module.fallback_tokens.size()));
	module.u32(p_module.fallback_tokens.size());
	module.u32(payload.data.size());
	const int header_size = module.data.size();
	ERR_FAIL_COND_V(header_size != GDScriptCompiledModule::ENVELOPE_HEADER_SIZE, ERR_BUG);
	module.data.resize(header_size + p_module.fallback_tokens.size() + payload.data.size());
	memcpy(module.data.ptrw() + header_size, p_module.fallback_tokens.ptr(), p_module.fallback_tokens.size());
	memcpy(module.data.ptrw() + header_size + p_module.fallback_tokens.size(), payload.data.ptr(), payload.data.size());
	r_module = module.data;
	return OK;
}

Error parse_module(const Vector<uint8_t> &p_module, ParsedModule &r_module, String *r_error,
		GDScriptCompiledModule::Rejection *r_rejection = nullptr) {
	ModuleEnvelope envelope;
	const Error envelope_error = read_envelope(p_module, envelope, r_error, r_rejection);
	if (envelope_error != OK) {
		return envelope_error;
	}
	const bool fallback_available = !envelope.fallback_tokens.is_empty();
	auto fail = [&](GDScriptCompiledModule::RejectionReason p_reason, const String &p_message, Error p_error = ERR_FILE_CORRUPT) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		set_rejection(r_rejection, p_reason, p_message, fallback_available);
		return p_error;
	};
	if ((envelope.flags & ~GDScriptCompiledModule::ENVELOPE_FLAG_HAS_FALLBACK) != 0) {
		return fail(GDScriptCompiledModule::REJECTION_UNSUPPORTED_FEATURE, "Compiled GDScript module requires unsupported envelope features.", ERR_UNAVAILABLE);
	}
	if (((envelope.flags & GDScriptCompiledModule::ENVELOPE_FLAG_HAS_FALLBACK) != 0) != fallback_available) {
		return fail(GDScriptCompiledModule::REJECTION_UNKNOWN_METADATA, "Compiled GDScript fallback flag and token section disagree.");
	}
	if (envelope.format_version != GDScriptCompiledModule::FORMAT_VERSION) {
		return fail(GDScriptCompiledModule::REJECTION_FORMAT_VERSION, "Unsupported compiled GDScript module format.", ERR_UNAVAILABLE);
	}
	if (envelope.bytecode_version != GDScriptCompiledModule::BYTECODE_VERSION) {
		return fail(GDScriptCompiledModule::REJECTION_BYTECODE_VERSION, "Unsupported GDScript bytecode version.", ERR_UNAVAILABLE);
	}
	r_module.engine_api_fingerprint = envelope.engine_api_fingerprint;
	r_module.source_fingerprint = envelope.source_fingerprint;
	r_module.fallback_tokens = envelope.fallback_tokens;
	const uint8_t *payload_data = p_module.ptr() + envelope.payload_offset;
	if (GDScriptCompiledModule::fingerprint_bytes(payload_data, envelope.payload_size) != envelope.payload_fingerprint) {
		return fail(GDScriptCompiledModule::REJECTION_CORRUPT_MODULE, "Compiled GDScript module payload checksum mismatch.");
	}

	Reader payload(payload_data, envelope.payload_size);
	r_module.path = payload.string();
	r_module.module_fingerprint = payload.u64();
	r_module.schema_fingerprint = payload.u64();
	r_module.dependency_fingerprint = payload.u64();
	const uint32_t dependency_count = payload.count(MAX_MODULE_CLASSES);
	r_module.dependencies.resize(dependency_count);
	for (uint32_t i = 0; i < dependency_count; i++) {
		r_module.dependencies.write[i].path = payload.string();
		r_module.dependencies.write[i].source_fingerprint = payload.u64();
		r_module.dependencies.write[i].module_fingerprint = payload.u64();
		r_module.dependencies.write[i].schema_fingerprint = payload.u64();
		r_module.dependencies.write[i].engine_api_fingerprint = payload.u64();
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
	const Vector<uint8_t> debug_section = payload.bytes();
	if (!read_debug_section(debug_section, r_module)) {
		return fail(GDScriptCompiledModule::REJECTION_UNKNOWN_METADATA, "Malformed or unsupported compiled GDScript debug section.");
	}
	if (payload.failed || payload.offset != payload.size || r_module.fallback_tokens.is_empty() || r_module.classes.is_empty()) {
		return fail(GDScriptCompiledModule::REJECTION_UNKNOWN_METADATA, "Malformed compiled GDScript module payload.");
	}
	set_rejection(r_rejection, GDScriptCompiledModule::REJECTION_NONE, String(), fallback_available);
	return OK;
}
