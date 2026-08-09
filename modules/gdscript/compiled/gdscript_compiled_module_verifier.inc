/**************************************************************************/
/*  gdscript_compiled_module_verifier.inc                                 */
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

// Fail-closed metadata, bytecode, control-flow, and relocation verification.
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
		case CONSTANT_ENGINE_SINGLETON: {
			if (!p_constant.encoded.is_empty() || p_constant.name.is_empty() || p_constant.owner.is_empty() || Engine::get_singleton() == nullptr ||
					!Engine::get_singleton()->has_singleton(p_constant.name)) {
				return fail("Invalid engine-singleton constant metadata.");
			}
			Object *singleton = Engine::get_singleton()->get_singleton_object(p_constant.name);
			if (singleton == nullptr || !singleton->is_class(p_constant.owner)) {
				return fail("Engine singleton '" + p_constant.name + "' has an incompatible native class.");
			}
			return OK;
		}
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
	if (module.path.is_empty() || module.path != GDScript::canonicalize_path(module.path) || module.source_fingerprint == 0 || module.module_fingerprint == 0 ||
			module.schema_fingerprint == 0 || module.dependency_fingerprint == 0 || module.engine_api_fingerprint == 0 ||
			module.fallback_tokens.is_empty() || module.classes.is_empty() || module.classes.size() > int(MAX_MODULE_CLASSES) || module.functions.size() > int(MAX_MODULE_FUNCTIONS)) {
		return fail("Compiled module exceeds class/function limits or has no root class.");
	}
	if (add_metadata_entries(module.dependencies.size() + module.classes.size() + module.functions.size(), "module tables") != OK) {
		return ERR_INVALID_DATA;
	}
	const String module_path = GDScript::canonicalize_path(module.path);
	HashSet<String> dependency_paths;
	String previous_dependency;
	for (const GDScriptCompiledModule::Dependency &dependency : module.dependencies) {
		const String canonical_dependency = GDScript::canonicalize_path(dependency.path);
		if (dependency.path.is_empty() || dependency.path != canonical_dependency || canonical_dependency == module_path ||
				dependency.source_fingerprint == 0 || dependency.module_fingerprint == 0 || dependency.schema_fingerprint == 0 ||
				dependency.engine_api_fingerprint == 0 || dependency_paths.has(canonical_dependency) ||
				(!previous_dependency.is_empty() && previous_dependency >= canonical_dependency)) {
			return fail("Invalid or duplicate compiled-module dependency.");
		}
		dependency_paths.insert(canonical_dependency);
		previous_dependency = canonical_dependency;
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
					binding.default_argument_count < 0 || binding.default_argument_count > binding.argument_types.size()) {
				return fail("Invalid method binding '" + binding.name + "' in class '" + record.identity + "'.");
			}
			if (verify_method(binding.method) != OK) {
				return fail("Invalid method binding '" + binding.name + "': " + error);
			}
			if (verify_data_type(binding.return_type) != OK) {
				return fail("Invalid return type for method binding '" + binding.name + "': " + error);
			}
			if (verify_constant(binding.rpc_config) != OK) {
				return fail("Invalid RPC metadata for method binding '" + binding.name + "': " + error);
			}
			for (const DataTypeRecord &argument : binding.argument_types) {
				if (verify_data_type(argument) != OK) {
					return fail("Invalid argument type for method binding '" + binding.name + "': " + error);
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

Error ModuleVerifier::validate_debug_metadata() {
	if (!module.has_debug_info) {
		for (const ClassRecord &script_class : module.classes) {
			if (!script_class.debug_members.is_empty()) {
				return fail("Stripped module contains class debug metadata.");
			}
		}
		for (const FunctionRecord &function : module.functions) {
			if (!function.debug_profile_identifier.is_empty() || !function.debug_source_positions.is_empty() || !function.debug_locals.is_empty()) {
				return fail("Stripped module contains function debug metadata.");
			}
		}
		return OK;
	}

	for (const ClassRecord &script_class : module.classes) {
		HashSet<String> member_names;
		for (const DebugMemberRecord &member : script_class.debug_members) {
			if (member.name.is_empty() || member.line <= 0 || member_names.has(member.name)) {
				return fail("Invalid editor member location in class '" + script_class.identity + "'.");
			}
			member_names.insert(member.name);
		}
		if (add_metadata_entries(script_class.debug_members.size(), "class debug table") != OK) {
			return ERR_INVALID_DATA;
		}
	}

	for (int function_index = 0; function_index < module.functions.size(); function_index++) {
		const FunctionRecord &function = module.functions[function_index];
		const FunctionInfo &info = function_info[function_index];
		if (function.debug_profile_identifier.is_empty()) {
			return fail("Missing profiler identifier for function '" + function.identity + "'.");
		}
		int previous_offset = -1;
		for (const DebugSourcePositionRecord &position : function.debug_source_positions) {
			if (position.code_offset <= previous_offset || position.code_offset < 0 || position.code_offset + 1 >= function.code.size() ||
					!info.boundaries[position.code_offset] || function.code[position.code_offset] != GDScriptFunction::OPCODE_LINE ||
					function.code[position.code_offset + 1] != position.line || position.line <= 0 || position.column < 0) {
				return fail("Invalid source line/column table in function '" + function.identity + "'.");
			}
			previous_offset = position.code_offset;
		}

		HashMap<String, Vector<int>> live_locals;
		int previous_line = -1;
		for (const DebugLocalRecord &local : function.debug_locals) {
			if (local.name.is_empty() || local.line < previous_line || local.line < 0 || local.slot < GDScriptFunction::FIXED_ADDRESSES_MAX || local.slot >= function.stack_size) {
				return fail("Invalid local-variable debug table in function '" + function.identity + "'.");
			}
			previous_line = local.line;
			if (local.added) {
				live_locals[local.name].push_back(local.slot);
			} else {
				Vector<int> *slots = live_locals.getptr(local.name);
				if (slots == nullptr || slots->is_empty() || (*slots)[slots->size() - 1] != local.slot) {
					return fail("Unbalanced local-variable lifetime in function '" + function.identity + "'.");
				}
				slots->remove_at(slots->size() - 1);
				if (slots->is_empty()) {
					live_locals.erase(local.name);
				}
			}
		}
		if (add_metadata_entries(function.debug_source_positions.size() + function.debug_locals.size() + 1, "function debug table") != OK) {
			return ERR_INVALID_DATA;
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
	if (module.schema_fingerprint != get_schema_fingerprint(module.classes) ||
			module.module_fingerprint != get_local_module_fingerprint(module.classes, module.functions) ||
			module.dependency_fingerprint != get_dependency_fingerprint(module.dependencies)) {
		return fail("Compiled module metadata, schema, or dependency fingerprint mismatch.");
	}
	return OK;
}

Error ModuleVerifier::verify(String *r_error) {
	if (r_error != nullptr) {
		r_error->clear();
	}
	failure_context = GDScriptCompiledModule::REJECTION_UNKNOWN_METADATA;
	Error result = index_and_verify_metadata();
	if (result == OK) {
		failure_context = GDScriptCompiledModule::REJECTION_INVALID_BYTECODE;
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
		failure_context = GDScriptCompiledModule::REJECTION_FAILED_RELOCATION;
		result = validate_relocations();
	}
	if (result == OK) {
		failure_context = GDScriptCompiledModule::REJECTION_UNKNOWN_METADATA;
		result = validate_debug_metadata();
	}
	if (result == OK) {
		failure_context = GDScriptCompiledModule::REJECTION_INVALID_BYTECODE;
		result = validate_final_limits();
	}
	if (result != OK && r_error != nullptr) {
		*r_error = error.is_empty() ? String("Compiled module verification failed.") : error;
	}
	if (result == OK) {
		failure_context = GDScriptCompiledModule::REJECTION_NONE;
	}
	return result;
}
