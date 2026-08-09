/**************************************************************************/
/*  gdscript_compiled_module_encoder.inc                                  */
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

// Runtime metadata and bytecode to portable-record encoding.
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
		if (object != nullptr && Engine::get_singleton() != nullptr) {
			List<Engine::Singleton> singletons;
			Engine::get_singleton()->get_singletons(&singletons);
			for (const Engine::Singleton &singleton : singletons) {
				if (singleton.ptr == object) {
					r_constant.kind = CONSTANT_ENGINE_SINGLETON;
					r_constant.name = singleton.name;
					// Prefer the public singleton class name over a platform-specific
					// implementation class (for example, a DisplayServer backend).
					r_constant.owner = object->is_class(singleton.name) ? String(singleton.name) : object->get_class();
					return true;
				}
			}
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
		case CONSTANT_ENGINE_SINGLETON: {
			Object *object = p_value.get_validated_object();
			return object != nullptr && Engine::get_singleton() != nullptr && Engine::get_singleton()->has_singleton(p_constant.name) &&
					Engine::get_singleton()->get_singleton_object(p_constant.name) == object && object->is_class(p_constant.owner);
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

uint64_t get_schema_fingerprint(const Vector<ClassRecord> &p_classes) {
	Writer serialized;
	serialized.u32(p_classes.size());
	for (const ClassRecord &record : p_classes) {
		write_class(serialized, record);
	}
	return GDScriptCompiledModule::fingerprint_bytes(serialized.data.ptr(), serialized.data.size());
}

uint64_t get_local_module_fingerprint(const Vector<ClassRecord> &p_classes, const Vector<FunctionRecord> &p_functions) {
	Writer serialized;
	serialized.u32(p_classes.size());
	for (const ClassRecord &record : p_classes) {
		write_class(serialized, record);
	}
	serialized.u32(p_functions.size());
	for (const FunctionRecord &record : p_functions) {
		write_function(serialized, record);
	}
	return GDScriptCompiledModule::fingerprint_bytes(serialized.data.ptr(), serialized.data.size());
}

uint64_t get_dependency_fingerprint(Vector<GDScriptCompiledModule::Dependency> p_dependencies) {
	p_dependencies.sort();
	Writer serialized;
	serialized.u32(p_dependencies.size());
	for (const GDScriptCompiledModule::Dependency &dependency : p_dependencies) {
		serialized.string(dependency.path);
		serialized.u64(dependency.source_fingerprint);
		serialized.u64(dependency.module_fingerprint);
		serialized.u64(dependency.schema_fingerprint);
		serialized.u64(dependency.engine_api_fingerprint);
	}
	return GDScriptCompiledModule::fingerprint_bytes(serialized.data.ptr(), serialized.data.size());
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
#ifdef TOOLS_ENABLED
	Vector<StringName> debug_member_names;
	for (const KeyValue<StringName, int> &member : p_script->member_lines) {
		debug_member_names.push_back(member.key);
	}
	debug_member_names.sort();
	for (const StringName &name : debug_member_names) {
		r_record.debug_members.push_back({ String(name), p_script->member_lines[name] });
	}
#endif
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

void Internals::add_script_dependency(GDScript *p_root, GDScript *p_dependency, HashMap<String, GDScript *> &r_dependencies) {
	if (p_dependency == nullptr || p_dependency == p_root) {
		return;
	}
	const String path = GDScript::canonicalize_path(p_dependency->get_script_path());
	if (path.is_empty() || path == GDScript::canonicalize_path(p_root->get_script_path())) {
		return;
	}
	r_dependencies.insert(path, p_dependency);
}

void Internals::collect_dependency_from_data_type(GDScript *p_root, const GDScriptDataType &p_type, HashMap<String, GDScript *> &r_dependencies) {
	Script *script = p_type.script_type;
	if (script == nullptr && p_type.script_type_ref.is_valid()) {
		script = p_type.script_type_ref.ptr();
	}
	add_script_dependency(p_root, Object::cast_to<GDScript>(script), r_dependencies);
	for (const GDScriptDataType &element_type : p_type.container_element_types) {
		collect_dependency_from_data_type(p_root, element_type, r_dependencies);
	}
}

void Internals::collect_dependencies_from_function(GDScript *p_root, GDScriptFunction *p_function,
		HashMap<String, GDScript *> &r_dependencies, HashSet<GDScriptFunction *> &r_visited) {
	if (p_function == nullptr || r_visited.has(p_function)) {
		return;
	}
	r_visited.insert(p_function);
	for (const Variant &constant : p_function->constants) {
		if (constant.get_type() == Variant::OBJECT) {
			add_script_dependency(p_root, Object::cast_to<GDScript>(constant.get_validated_object()), r_dependencies);
		}
	}
	for (const Variant &default_argument : p_function->method_info.default_arguments) {
		if (default_argument.get_type() == Variant::OBJECT) {
			add_script_dependency(p_root, Object::cast_to<GDScript>(default_argument.get_validated_object()), r_dependencies);
		}
	}
	for (const GDScriptDataType &argument_type : p_function->argument_types) {
		collect_dependency_from_data_type(p_root, argument_type, r_dependencies);
	}
	collect_dependency_from_data_type(p_root, p_function->return_type, r_dependencies);
	for (GDScriptFunction *lambda : p_function->lambdas) {
		collect_dependencies_from_function(p_root, lambda, r_dependencies, r_visited);
	}
}

void Internals::collect_dependencies_from_script(GDScript *p_root, GDScript *p_script, HashMap<String, GDScript *> &r_dependencies) {
	add_script_dependency(p_root, p_script->base.ptr(), r_dependencies);
	for (const KeyValue<StringName, GDScript::MemberInfo> &entry : p_script->member_indices) {
		collect_dependency_from_data_type(p_root, entry.value.data_type, r_dependencies);
	}
	for (const KeyValue<StringName, GDScript::MemberInfo> &entry : p_script->static_variables_indices) {
		collect_dependency_from_data_type(p_root, entry.value.data_type, r_dependencies);
	}
	for (const KeyValue<StringName, Variant> &entry : p_script->constants) {
		if (entry.value.get_type() == Variant::OBJECT) {
			add_script_dependency(p_root, Object::cast_to<GDScript>(entry.value.get_validated_object()), r_dependencies);
		}
	}
	for (const KeyValue<StringName, Variant> &entry : p_script->member_default_values) {
		if (entry.value.get_type() == Variant::OBJECT) {
			add_script_dependency(p_root, Object::cast_to<GDScript>(entry.value.get_validated_object()), r_dependencies);
		}
	}
	for (const KeyValue<StringName, Ref<StructLayout>> &entry : p_script->struct_layouts) {
		for (int field_index = 0; field_index < entry.value->get_field_count(); field_index++) {
			const Variant default_value = entry.value->get_default_value(field_index);
			if (default_value.get_type() == Variant::OBJECT) {
				add_script_dependency(p_root, Object::cast_to<GDScript>(default_value.get_validated_object()), r_dependencies);
			}
		}
	}
	HashSet<GDScriptFunction *> visited;
	for (const KeyValue<StringName, GDScriptFunction *> &entry : p_script->member_functions) {
		collect_dependencies_from_function(p_root, entry.value, r_dependencies, visited);
	}
	collect_dependencies_from_function(p_root, p_script->initializer, r_dependencies, visited);
	collect_dependencies_from_function(p_root, p_script->implicit_initializer, r_dependencies, visited);
	collect_dependencies_from_function(p_root, p_script->implicit_ready, r_dependencies, visited);
	collect_dependencies_from_function(p_root, p_script->static_initializer, r_dependencies, visited);
	for (const KeyValue<StringName, Ref<GDScript>> &entry : p_script->subclasses) {
		collect_dependencies_from_script(p_root, entry.value.ptr(), r_dependencies);
	}
}

void Internals::collect_dependency_closure(GDScript *p_root, GDScript *p_script, HashMap<String, GDScript *> &r_dependencies, HashSet<String> &r_visited) {
	if (p_script == nullptr) {
		return;
	}
	const String path = GDScript::canonicalize_path(p_script->get_script_path());
	if (path.is_empty() || r_visited.has(path)) {
		return;
	}
	r_visited.insert(path);

	HashMap<String, GDScript *> direct_dependencies;
	collect_dependencies_from_script(p_script, p_script, direct_dependencies);
	Vector<String> paths;
	for (const KeyValue<String, GDScript *> &entry : direct_dependencies) {
		paths.push_back(entry.key);
	}
	paths.sort();
	const String root_path = GDScript::canonicalize_path(p_root->get_script_path());
	for (const String &dependency_path : paths) {
		GDScript *dependency = *direct_dependencies.getptr(dependency_path);
		if (dependency_path != root_path) {
			r_dependencies.insert(dependency_path, dependency);
		}
		collect_dependency_closure(p_root, dependency, r_dependencies, r_visited);
	}
}

bool Internals::make_local_records(GDScript *p_script, Vector<ClassRecord> &r_classes, Vector<FunctionRecord> &r_functions, int *r_skipped_functions) {
	HashMap<GDScriptFunction *, String> function_identities;
	Vector<GDScriptFunction *> functions;
	collect_functions(p_script, "root", function_identities, functions);
	HashMap<GDScript *, String> class_identities;
	Vector<GDScript *> classes;
	collect_classes(p_script, "root", class_identities, classes);
	for (GDScript *script_class : classes) {
		ClassRecord record;
		if (!make_class_record(script_class, *class_identities.getptr(script_class), class_identities, function_identities, record)) {
			return false;
		}
		r_classes.push_back(record);
	}
	int skipped = 0;
	for (GDScriptFunction *function : functions) {
		FunctionRecord record;
		if (make_record(function, *function_identities.getptr(function), function_identities, record)) {
			r_functions.push_back(record);
		} else {
			skipped++;
		}
	}
	r_classes.sort();
	r_functions.sort();
	if (r_skipped_functions != nullptr) {
		*r_skipped_functions = skipped;
	}
	return true;
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
	for (const GDScriptFunction::SourcePosition &position : p_function->source_positions) {
		r_record.debug_source_positions.push_back({ position.code_offset, position.line, position.column });
	}
	for (const GDScriptFunction::StackDebug &local : p_function->stack_debug) {
		r_record.debug_locals.push_back({ local.line, local.pos, local.added, String(local.identifier) });
	}
#ifdef DEBUG_ENABLED
	r_record.debug_profile_identifier = p_function->profile.signature;
#endif
	if (r_record.debug_profile_identifier.is_empty()) {
		r_record.debug_profile_identifier = r_record.source + "::" + itos(r_record.initial_line) + "::" + String(r_record.name);
	}
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
