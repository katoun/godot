/**************************************************************************/
/*  gdscript_compiled_module_relocator.inc                                */
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

// Validation and installation of freshly compiled runtime pointer tables.
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
	if (!p_record.debug_profile_identifier.is_empty()) {
		p_function->source_positions.clear();
		for (const DebugSourcePositionRecord &position : p_record.debug_source_positions) {
			p_function->source_positions.push_back({ position.code_offset, position.line, position.column });
		}
		p_function->stack_debug.clear();
		for (const DebugLocalRecord &local : p_record.debug_locals) {
			GDScriptFunction::StackDebug entry;
			entry.line = local.line;
			entry.pos = local.slot;
			entry.added = local.added;
			entry.identifier = local.name;
			p_function->stack_debug.push_back(entry);
		}
#ifdef DEBUG_ENABLED
		p_function->profile.signature = p_record.debug_profile_identifier;
#endif
	}

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
