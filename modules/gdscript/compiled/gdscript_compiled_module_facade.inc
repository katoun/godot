/**************************************************************************/
/*  gdscript_compiled_module_facade.inc                                   */
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

// Stable public facade, cache integration, and project manifest generation.
} // namespace GDScriptCompiledModuleImplementation

using namespace GDScriptCompiledModuleImplementation;

String GDScriptCompiledModule::get_rejection_reason_name(RejectionReason p_reason) {
	switch (p_reason) {
		case REJECTION_NONE:
			return "none";
		case REJECTION_FORMAT_VERSION:
			return "format-version mismatch";
		case REJECTION_BYTECODE_VERSION:
			return "bytecode-version mismatch";
		case REJECTION_ENGINE_API:
			return "engine API mismatch";
		case REJECTION_CHANGED_DEPENDENCY:
			return "changed dependency";
		case REJECTION_UNKNOWN_METADATA:
			return "unknown metadata";
		case REJECTION_FAILED_RELOCATION:
			return "failed relocation";
		case REJECTION_INVALID_BYTECODE:
			return "invalid bytecode";
		case REJECTION_UNSUPPORTED_FEATURE:
			return "unsupported feature";
		case REJECTION_SOURCE_MISMATCH:
			return "source mismatch";
		case REJECTION_CORRUPT_MODULE:
			return "corrupt module";
	}
	return "unknown rejection";
}

String GDScriptCompiledModule::Rejection::describe() const {
	const String reason_name = GDScriptCompiledModule::get_rejection_reason_name(reason);
	return detail.is_empty() ? reason_name : reason_name + ": " + detail;
}

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

Error GDScriptCompiledModule::create(GDScript *p_script, const Vector<uint8_t> &p_fallback_tokens, Vector<uint8_t> &r_module, Summary *r_summary,
		DebugInfoMode p_debug_info) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_fallback_tokens.is_empty(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_debug_info != DEBUG_INFO_FULL && p_debug_info != DEBUG_INFO_STRIPPED, ERR_INVALID_PARAMETER);

	Vector<ClassRecord> class_records;
	Vector<FunctionRecord> records;
	int skipped_functions = 0;
	if (!Internals::make_local_records(p_script, class_records, records, &skipped_functions)) {
		return ERR_UNAVAILABLE;
	}

	HashMap<String, GDScript *> dependency_map;
	HashSet<String> visited_dependencies;
	Internals::collect_dependency_closure(p_script, p_script, dependency_map, visited_dependencies);
	Vector<Dependency> dependencies;
	for (const KeyValue<String, GDScript *> &entry : dependency_map) {
		Dependency dependency;
		dependency.path = entry.key;
		dependency.source_fingerprint = Internals::dependency_source_fingerprint(entry.value);
		Vector<ClassRecord> dependency_classes;
		Vector<FunctionRecord> dependency_functions;
		if (dependency.source_fingerprint == 0 || !Internals::make_local_records(entry.value, dependency_classes, dependency_functions)) {
			return ERR_UNAVAILABLE;
		}
		dependency.module_fingerprint = get_local_module_fingerprint(dependency_classes, dependency_functions);
		dependency.schema_fingerprint = get_schema_fingerprint(dependency_classes);
		dependency.engine_api_fingerprint = get_engine_api_fingerprint();
		dependencies.push_back(dependency);
	}
	dependencies.sort();

	const uint64_t source_fingerprint = fingerprint_source(p_script->source);
	const uint64_t module_fingerprint = get_local_module_fingerprint(class_records, records);
	const uint64_t schema_fingerprint = get_schema_fingerprint(class_records);
	const uint64_t dependency_fingerprint = get_dependency_fingerprint(dependencies);
	const uint64_t engine_api_fingerprint = get_engine_api_fingerprint();
	ParsedModule candidate;
	candidate.path = GDScript::canonicalize_path(p_script->get_script_path());
	candidate.source_fingerprint = source_fingerprint;
	candidate.module_fingerprint = module_fingerprint;
	candidate.schema_fingerprint = schema_fingerprint;
	candidate.dependency_fingerprint = dependency_fingerprint;
	candidate.engine_api_fingerprint = engine_api_fingerprint;
	candidate.fallback_tokens = p_fallback_tokens;
	candidate.dependencies = dependencies;
	candidate.classes = class_records;
	candidate.functions = records;
	candidate.has_debug_info = p_debug_info == DEBUG_INFO_FULL;
	if (!candidate.has_debug_info) {
		for (ClassRecord &record : candidate.classes) {
			record.debug_members.clear();
		}
		for (FunctionRecord &record : candidate.functions) {
			record.debug_profile_identifier.clear();
			record.debug_source_positions.clear();
			record.debug_locals.clear();
		}
	}
	String candidate_error;
	if (ModuleVerifier(candidate).verify(&candidate_error) != OK) {
		// Keep export/cache creation fail-closed as well. The caller already has
		// the binary-token fallback when a function cannot be represented by the
		// portable module or a compiler invariant is violated.
		ERR_PRINT("GDScript compiler produced a non-portable module for '" + candidate.path + "': " + candidate_error);
		return ERR_UNAVAILABLE;
	}
	const Error serialize_error = serialize_module(candidate, r_module);
	if (serialize_error != OK) {
		return serialize_error;
	}

	if (r_summary != nullptr) {
		*r_summary = Summary();
		r_summary->path = GDScript::canonicalize_path(p_script->get_script_path());
		r_summary->source_fingerprint = source_fingerprint;
		r_summary->module_fingerprint = module_fingerprint;
		r_summary->schema_fingerprint = schema_fingerprint;
		r_summary->dependency_fingerprint = dependency_fingerprint;
		r_summary->engine_api_fingerprint = engine_api_fingerprint;
		r_summary->dependencies = dependencies;
		r_summary->skipped_functions = skipped_functions;
		r_summary->has_debug_info = candidate.has_debug_info;
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
	ModuleEnvelope envelope;
	const Error error = read_envelope(p_module, envelope, r_error);
	if (error != OK) {
		return error;
	}
	if ((envelope.flags & ENVELOPE_FLAG_HAS_FALLBACK) == 0 || envelope.fallback_tokens.is_empty()) {
		if (r_error != nullptr) {
			*r_error = "Compiled GDScript module has no embedded token fallback.";
		}
		return ERR_UNAVAILABLE;
	}
	r_fallback_tokens = envelope.fallback_tokens;
	if (r_source_fingerprint != nullptr) {
		*r_source_fingerprint = envelope.source_fingerprint;
	}
	return OK;
}

Error GDScriptCompiledModule::get_dependencies(const Vector<uint8_t> &p_module, Vector<Dependency> &r_dependencies, String *r_error) {
	ParsedModule module;
	const Error error = parse_module(p_module, module, r_error);
	if (error != OK) {
		return error;
	}
	r_dependencies = module.dependencies;
	return OK;
}

Error GDScriptCompiledModule::verify(const Vector<uint8_t> &p_module, String *r_error, Rejection *r_rejection) {
	ParsedModule module;
	const Error parse_error = parse_module(p_module, module, r_error, r_rejection);
	if (parse_error != OK) {
		return parse_error;
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		const String detail = "Engine/Variant API fingerprint mismatch.";
		if (r_error != nullptr) {
			*r_error = detail;
		}
		set_rejection(r_rejection, REJECTION_ENGINE_API, detail, !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	ModuleRegistry registry;
	const Error result = registry.verify_graph(module, r_error);
	if (result != OK) {
		set_rejection(r_rejection, registry.get_rejection_reason(), r_error != nullptr ? *r_error : String(), !module.fallback_tokens.is_empty());
	} else {
		set_rejection(r_rejection, REJECTION_NONE, String(), !module.fallback_tokens.is_empty());
	}
	return result;
}

Error GDScriptCompiledModule::prepare_shallow(GDScript *p_script, const Vector<uint8_t> &p_module, String *r_error, Rejection *r_rejection) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	ParsedModule module;
	Error parse_error = parse_module(p_module, module, r_error, r_rejection);
	if (parse_error != OK) {
		return parse_error;
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		const String detail = "Engine/Variant API fingerprint mismatch.";
		if (r_error != nullptr) {
			*r_error = detail;
		}
		set_rejection(r_rejection, REJECTION_ENGINE_API, detail, !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	if ((!p_script->source.is_empty() && module.source_fingerprint != fingerprint_source(p_script->source)) ||
			(!p_script->binary_tokens.is_empty() && module.fallback_tokens != p_script->binary_tokens)) {
		const String detail = "Source or binary-token fingerprint mismatch.";
		if (r_error != nullptr) {
			*r_error = detail;
		}
		set_rejection(r_rejection, REJECTION_SOURCE_MISMATCH, detail, !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	ModuleRegistry registry;
	if (registry.verify_graph(module, r_error) != OK) {
		set_rejection(r_rejection, registry.get_rejection_reason(), r_error != nullptr ? *r_error : String(), !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	const Error result = RuntimeBuilder::prepare_shell_graph(p_script, module, nullptr, r_error);
	if (result != OK) {
		set_rejection(r_rejection, REJECTION_UNKNOWN_METADATA, r_error != nullptr ? *r_error : String(), !module.fallback_tokens.is_empty());
	} else {
		set_rejection(r_rejection, REJECTION_NONE, String(), !module.fallback_tokens.is_empty());
	}
	return result;
}

Error GDScriptCompiledModule::build_runtime(GDScript *p_script, const Vector<uint8_t> &p_module, bool p_keep_state, String *r_error, Rejection *r_rejection) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	ParsedModule module;
	Error parse_error = parse_module(p_module, module, r_error, r_rejection);
	if (parse_error != OK) {
		return parse_error;
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		const String detail = "Engine/Variant API fingerprint mismatch.";
		if (r_error != nullptr) {
			*r_error = detail;
		}
		set_rejection(r_rejection, REJECTION_ENGINE_API, detail, !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	if ((!p_script->source.is_empty() && module.source_fingerprint != fingerprint_source(p_script->source)) ||
			(!p_script->binary_tokens.is_empty() && module.fallback_tokens != p_script->binary_tokens)) {
		const String detail = "Source or binary-token fingerprint mismatch.";
		if (r_error != nullptr) {
			*r_error = detail;
		}
		set_rejection(r_rejection, REJECTION_SOURCE_MISMATCH, detail, !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	ModuleRegistry registry;
	if (registry.verify_graph(module, r_error) != OK) {
		set_rejection(r_rejection, registry.get_rejection_reason(), r_error != nullptr ? *r_error : String(), !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	RuntimeBuilder builder;
	const Error result = builder.build(p_script, module, p_keep_state, r_error);
	if (result != OK) {
		set_rejection(r_rejection, REJECTION_FAILED_RELOCATION, r_error != nullptr ? *r_error : String(), !module.fallback_tokens.is_empty());
	} else {
		set_rejection(r_rejection, REJECTION_NONE, String(), !module.fallback_tokens.is_empty());
	}
	return result;
}

Error GDScriptCompiledModule::apply(GDScript *p_script, const Vector<uint8_t> &p_module, String *r_error, Rejection *r_rejection) {
	ERR_FAIL_NULL_V(p_script, ERR_INVALID_PARAMETER);
	ParsedModule module;
	Error error = parse_module(p_module, module, r_error, r_rejection);
	if (error != OK) {
		return error;
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		const String detail = "Engine/Variant API fingerprint mismatch.";
		if (r_error != nullptr) {
			*r_error = detail;
		}
		set_rejection(r_rejection, REJECTION_ENGINE_API, detail, !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	if ((!p_script->source.is_empty() && module.source_fingerprint != fingerprint_source(p_script->source)) ||
			(!p_script->binary_tokens.is_empty() && module.fallback_tokens != p_script->binary_tokens)) {
		const String detail = "Source or binary-token fingerprint mismatch.";
		if (r_error != nullptr) {
			*r_error = detail;
		}
		set_rejection(r_rejection, REJECTION_SOURCE_MISMATCH, detail, !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	ModuleRegistry registry;
	if (registry.verify_graph(module, r_error) != OK) {
		set_rejection(r_rejection, registry.get_rejection_reason(), r_error != nullptr ? *r_error : String(), !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
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
		const String detail = "Compiled class table does not match the freshly compiled script.";
		if (r_error != nullptr) {
			*r_error = detail;
		}
		set_rejection(r_rejection, REJECTION_UNKNOWN_METADATA, detail, !module.fallback_tokens.is_empty());
		return ERR_INVALID_DATA;
	}
	HashSet<String> validated_classes;
	for (const ClassRecord &record : module.classes) {
		GDScript *const *script_class = classes.getptr(record.identity);
		if (record.identity.is_empty() || validated_classes.has(record.identity) || script_class == nullptr ||
				!Internals::validate_class_record(record, *script_class, class_identities, identities)) {
			const String detail = "Class metadata verification failed for '" + record.identity + "'.";
			if (r_error != nullptr) {
				*r_error = detail;
			}
			set_rejection(r_rejection, REJECTION_UNKNOWN_METADATA, detail, !module.fallback_tokens.is_empty());
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
			const String detail = "Bytecode verification or symbolic relocation failed for '" + record.identity + "': " +
					(function == nullptr ? String("function is missing") : verification_error) + ".";
			if (r_error != nullptr) {
				*r_error = detail;
			}
			set_rejection(r_rejection, REJECTION_INVALID_BYTECODE, detail, !module.fallback_tokens.is_empty());
			return ERR_INVALID_DATA;
		}
	}
	for (const FunctionRecord &record : module.functions) {
		Internals::install_record(record, *functions.getptr(record.identity), functions);
	}
	set_rejection(r_rejection, REJECTION_NONE, String(), !module.fallback_tokens.is_empty());
	return OK;
}

Error GDScriptCompiledModule::make_test_bytecode_mutation(const Vector<uint8_t> &p_module, TestBytecodeMutation p_mutation,
		Vector<uint8_t> &r_module, String *r_opcode) {
	ParsedModule module;
	Error parse_error = parse_module(p_module, module, nullptr);
	if (parse_error != OK) {
		return parse_error;
	}
	if (r_opcode != nullptr) {
		r_opcode->clear();
	}

	for (FunctionRecord &function : module.functions) {
		if (function.code.is_empty()) {
			continue;
		}
		if (p_mutation == TEST_MUTATE_UNKNOWN_OPCODE) {
			function.code.write[0] = GDScriptFunction::OPCODE_COUNT;
			if (r_opcode != nullptr) {
				*r_opcode = "unknown";
			}
			return serialize_module(module, r_module);
		}
		if (p_mutation == TEST_MUTATE_TRUNCATED_INSTRUCTION) {
			function.code.resize(2);
			function.code.write[0] = GDScriptFunction::OPCODE_ASSIGN;
			function.code.write[1] = GDScriptFunction::OPCODE_END;
			if (r_opcode != nullptr) {
				*r_opcode = GDScriptFunction::get_opcode_descriptor(GDScriptFunction::OPCODE_ASSIGN).name;
			}
			return serialize_module(module, r_module);
		}

		for (int ip = 0; ip < function.code.size();) {
			const int instruction_size = GDScriptFunction::get_instruction_size(function.code.ptr(), function.code.size(), ip);
			if (instruction_size <= 0) {
				return ERR_BUG;
			}
			const GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(function.code[ip]);
			const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(opcode);
			for (int word = 1; word < instruction_size; word++) {
				const GDScriptFunction::OpcodeOperandKind kind = GDScriptFunction::get_operand_kind(function.code.ptr(), function.code.size(), ip, word);
				bool matches = false;
				int invalid_value = 0;
				switch (p_mutation) {
					case TEST_MUTATE_INVALID_JUMP_TARGET:
						matches = kind == GDScriptFunction::OPERAND_JUMP_TARGET;
						invalid_value = function.code.size();
						break;
					case TEST_MUTATE_INVALID_FRAME_SLOT:
						matches = kind == GDScriptFunction::OPERAND_FRAME_SLOT;
						invalid_value = function.stack_size;
						break;
					case TEST_MUTATE_INVALID_TYPED_FRAME_SLOT:
						matches = kind == GDScriptFunction::OPERAND_TYPED_FRAME_SLOT;
						invalid_value = function.stack_size;
						break;
					case TEST_MUTATE_INVALID_CONSTANT_INDEX:
						matches = kind == GDScriptFunction::OPERAND_CONSTANT_ADDRESS;
						invalid_value = (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS) | function.constants.size();
						break;
					case TEST_MUTATE_INVALID_NAME_INDEX:
						matches = kind == GDScriptFunction::OPERAND_NAME_INDEX;
						invalid_value = function.global_names.size();
						break;
					case TEST_MUTATE_INVALID_FUNCTION_INDEX:
						matches = kind == GDScriptFunction::OPERAND_FUNCTION_INDEX;
						invalid_value = descriptor.relocation_kind >= 0 ? function.relocations[descriptor.relocation_kind].size() : 0;
						break;
					case TEST_MUTATE_INVALID_ARGUMENT_COUNT:
						matches = kind == GDScriptFunction::OPERAND_ARGUMENT_COUNT;
						invalid_value = function.instruction_args_size + 1;
						break;
					case TEST_MUTATE_INVALID_STRUCT_FIELD_INDEX:
						matches = kind == GDScriptFunction::OPERAND_STRUCT_FIELD_INDEX;
						invalid_value = 1 << 16;
						break;
					case TEST_MUTATE_INVALID_NATIVE_API_RELOCATION:
						matches = kind == GDScriptFunction::OPERAND_NATIVE_API_RELOCATION;
						invalid_value = descriptor.relocation_kind >= 0 ? function.relocations[descriptor.relocation_kind].size() : 0;
						break;
					case TEST_MUTATE_UNKNOWN_OPCODE:
					case TEST_MUTATE_TRUNCATED_INSTRUCTION:
						break;
				}
				if (matches) {
					function.code.write[ip + word] = invalid_value;
					if (r_opcode != nullptr) {
						*r_opcode = descriptor.name;
					}
					return serialize_module(module, r_module);
				}
			}
			ip += instruction_size;
		}
	}
	return ERR_DOES_NOT_EXIST;
}

String GDScriptCompiledModule::get_editor_cache_path(const String &p_script_path) {
	const String key = GDScript::canonicalize_path(p_script_path) + ":" + String::num_uint64(get_engine_api_fingerprint()) + ":" + itos(FORMAT_VERSION);
	return "res://.godot/gdscript_compiled/" + key.sha256_text() + ".gdm";
}

Error GDScriptCompiledModule::load_editor_cache(const String &p_script_path, const String &p_source, Vector<uint8_t> &r_module, String *r_error) {
	auto fail = [&](Error p_error, const String &p_message) {
		if (r_error != nullptr) {
			*r_error = p_message;
		}
		return p_error;
	};
	const String cache_path = get_editor_cache_path(p_script_path);
	if (!FileAccess::exists(cache_path)) {
		return fail(ERR_FILE_NOT_FOUND, "No compiled editor-cache module exists.");
	}
	Vector<uint8_t> module_bytes = FileAccess::get_file_as_bytes(cache_path);
	ParsedModule module;
	ModuleRegistry registry;
	String detail;
	if (parse_module(module_bytes, module, &detail) != OK) {
		return fail(ERR_INVALID_DATA, "Editor-cache module parsing failed: " + detail);
	}
	if (module.engine_api_fingerprint != get_engine_api_fingerprint()) {
		return fail(ERR_INVALID_DATA, "Editor-cache engine API fingerprint changed.");
	}
	if (module.source_fingerprint != fingerprint_source(p_source)) {
		return fail(ERR_INVALID_DATA, "Editor-cache source fingerprint changed.");
	}
	if (registry.verify_graph(module, &detail) != OK) {
		return fail(ERR_INVALID_DATA, "Editor-cache dependency verification failed: " + detail);
	}
	r_module = module_bytes;
	if (r_error != nullptr) {
		r_error->clear();
	}
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
		payload.u64(module.module_fingerprint);
		payload.u64(module.schema_fingerprint);
		payload.u64(module.dependency_fingerprint);
		payload.u64(module.engine_api_fingerprint);
		payload.u32(uint32_t(module.skipped_functions));
		payload.u32(module.has_debug_info ? 1 : 0);
		payload.u32(module.dependencies.size());
		for (const Dependency &dependency : module.dependencies) {
			payload.string(dependency.path);
			payload.u64(dependency.source_fingerprint);
			payload.u64(dependency.module_fingerprint);
			payload.u64(dependency.schema_fingerprint);
			payload.u64(dependency.engine_api_fingerprint);
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
	manifest.u32(3);
	manifest.u64(fingerprint_bytes(payload.data.ptr(), payload.data.size()));
	manifest.u32(payload.data.size());
	const int header_size = manifest.data.size();
	manifest.data.resize(header_size + payload.data.size());
	memcpy(manifest.data.ptrw() + header_size, payload.data.ptr(), payload.data.size());
	return manifest.data;
}
