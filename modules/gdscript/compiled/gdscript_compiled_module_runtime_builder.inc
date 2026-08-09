/**************************************************************************/
/*  gdscript_compiled_module_runtime_builder.inc                          */
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

// Direct construction of runtime GDScript classes and functions.
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
		case CONSTANT_ENGINE_SINGLETON: {
			if (Engine::get_singleton() == nullptr || !Engine::get_singleton()->has_singleton(p_constant.name)) {
				return fail(ERR_DOES_NOT_EXIST, "Could not resolve engine singleton '" + p_constant.name + "'.");
			}
			Object *singleton = Engine::get_singleton()->get_singleton_object(p_constant.name);
			if (singleton == nullptr || !singleton->is_class(p_constant.owner)) {
				return fail(ERR_INVALID_DATA, "Engine singleton '" + p_constant.name + "' has an incompatible native class.");
			}
			r_value = singleton;
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
#ifdef TOOLS_ENABLED
		script->member_lines.clear();
		for (const DebugMemberRecord &member : staged.record->debug_members) {
			script->member_lines.insert(member.name, member.line);
		}
#endif
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
