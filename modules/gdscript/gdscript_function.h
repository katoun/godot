/**************************************************************************/
/*  gdscript_function.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
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

#pragma once

#include "gdscript_utility_functions.h"

#include "core/object/ref_counted.h"
#include "core/object/script_language.h"
#include "core/os/thread.h"
#include "core/string/string_name.h"
#include "core/templates/pair.h"
#include "core/templates/self_list.h"
#include "core/variant/struct_value.h"
#include "core/variant/variant.h"

class GDScriptInstance;
class GDScript;
class GDScriptCompiledModule;
namespace GDScriptCompiledModuleImplementation {
class Internals;
class RuntimeBuilder;
}
#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
class GDScriptBaselineJIT;
#endif

class GDScriptDataType {
public:
	Vector<GDScriptDataType> container_element_types;

	enum Kind {
		VARIANT, // Can be any type.
		BUILTIN,
		NATIVE,
		SCRIPT,
		GDSCRIPT,
	};

	Kind kind = VARIANT;

	Variant::Type builtin_type = Variant::NIL;
	StringName native_type;
	Script *script_type = nullptr;
	Ref<Script> script_type_ref;
	Ref<StructLayout> struct_layout;

	_FORCE_INLINE_ bool has_type() const { return kind != VARIANT; }

	bool is_type(const Variant &p_variant, bool p_allow_implicit_conversion = false) const;

	bool can_contain_object() const {
		if (kind == BUILTIN) {
			switch (builtin_type) {
				case Variant::ARRAY:
					if (has_container_element_type(0)) {
						return container_element_types[0].can_contain_object();
					}
					return true;
				case Variant::DICTIONARY:
					if (has_container_element_types()) {
						return get_container_element_type_or_variant(0).can_contain_object() || get_container_element_type_or_variant(1).can_contain_object();
					}
					return true;
				case Variant::NIL:
				case Variant::OBJECT:
				case Variant::STRUCT:
					return true;
				default:
					return false;
			}
		}
		return true;
	}

	void set_container_element_type(int p_index, const GDScriptDataType &p_element_type) {
		ERR_FAIL_COND(p_index < 0);
		while (p_index >= container_element_types.size()) {
			container_element_types.push_back(GDScriptDataType());
		}
		container_element_types.write[p_index] = GDScriptDataType(p_element_type);
	}

	GDScriptDataType get_container_element_type(int p_index) const {
		ERR_FAIL_INDEX_V(p_index, container_element_types.size(), GDScriptDataType());
		return container_element_types[p_index];
	}

	GDScriptDataType get_container_element_type_or_variant(int p_index) const {
		if (p_index < 0 || p_index >= container_element_types.size()) {
			return GDScriptDataType();
		}
		return container_element_types[p_index];
	}

	bool has_container_element_type(int p_index) const {
		return p_index >= 0 && p_index < container_element_types.size();
	}

	bool has_container_element_types() const {
		return !container_element_types.is_empty();
	}

	GDScriptDataType() = default;

	bool operator==(const GDScriptDataType &p_other) const {
		return kind == p_other.kind &&
				builtin_type == p_other.builtin_type &&
				native_type == p_other.native_type &&
				(script_type == p_other.script_type || script_type_ref == p_other.script_type_ref) &&
				struct_layout == p_other.struct_layout &&
				container_element_types == p_other.container_element_types;
	}

	bool operator!=(const GDScriptDataType &p_other) const {
		return !(*this == p_other);
	}

	void operator=(const GDScriptDataType &p_other) {
		kind = p_other.kind;
		builtin_type = p_other.builtin_type;
		native_type = p_other.native_type;
		script_type = p_other.script_type;
		script_type_ref = p_other.script_type_ref;
		struct_layout = p_other.struct_layout;
		container_element_types = p_other.container_element_types;
	}

	GDScriptDataType(const GDScriptDataType &p_other) {
		*this = p_other;
	}

	~GDScriptDataType() {}
};

class GDScriptFunction {
public:
	enum OpcodeOperandKind : uint8_t {
		OPERAND_NONE,
		OPERAND_FRAME_SLOT,
		OPERAND_TYPED_FRAME_SLOT,
		OPERAND_CONSTANT_ADDRESS,
		OPERAND_NAME_INDEX,
		OPERAND_FUNCTION_INDEX,
		OPERAND_JUMP_TARGET,
		OPERAND_ARGUMENT_COUNT,
		OPERAND_VARIADIC_FRAME_SLOTS,
		OPERAND_STRUCT_FIELD_INDEX,
		OPERAND_NATIVE_API_RELOCATION,
		OPERAND_STATIC_VARIABLE_INDEX,
		OPERAND_GLOBAL_INDEX,
		OPERAND_TYPE_ID,
		OPERAND_OPERATOR,
		OPERAND_BOOLEAN,
		OPERAND_OPERATOR_FEEDBACK_INDEX,
		OPERAND_CALL_FEEDBACK_INDEX,
		OPERAND_TYPE_METADATA,
		OPERAND_IMMEDIATE,
	};

	enum OpcodeControlFlowKind : uint8_t {
		CONTROL_FLOW_NEXT,
		CONTROL_FLOW_BRANCH,
		CONTROL_FLOW_JUMP,
		CONTROL_FLOW_DEFAULT_ARGUMENT,
		CONTROL_FLOW_SUSPEND,
		CONTROL_FLOW_RETURN,
		CONTROL_FLOW_TERMINATE,
	};

	enum OpcodeTypeConstraint : uint8_t {
		TYPE_CONSTRAINT_NONE,
		TYPE_CONSTRAINT_DYNAMIC,
		TYPE_CONSTRAINT_BOOL,
		TYPE_CONSTRAINT_INT,
		TYPE_CONSTRAINT_FLOAT,
		TYPE_CONSTRAINT_MATH,
		TYPE_CONSTRAINT_STRUCT,
		TYPE_CONSTRAINT_BUILTIN,
		TYPE_CONSTRAINT_ARRAY,
		TYPE_CONSTRAINT_DICTIONARY,
		TYPE_CONSTRAINT_NATIVE,
		TYPE_CONSTRAINT_SCRIPT,
		TYPE_CONSTRAINT_ITERATOR,
	};

	// Values deliberately match the portable compiled-module relocation tables.
	enum OpcodeRelocationKind : int8_t {
		RELOCATION_NONE = -1,
		RELOCATION_OPERATOR,
		RELOCATION_SETTER,
		RELOCATION_GETTER,
		RELOCATION_KEYED_SETTER,
		RELOCATION_KEYED_GETTER,
		RELOCATION_INDEXED_SETTER,
		RELOCATION_INDEXED_GETTER,
		RELOCATION_BUILTIN_METHOD,
		RELOCATION_CONSTRUCTOR,
		RELOCATION_UTILITY,
		RELOCATION_GDSCRIPT_UTILITY,
		RELOCATION_METHOD_BIND,
		RELOCATION_FUNCTION,
	};

	enum OpcodeResultOperand : int8_t {
		OPCODE_RESULT_NONE = INT8_MIN,
		OPCODE_RESULT_VARIADIC_LAST = -1,
		OPCODE_RESULT_VARIADIC_LAST_MINUS_1 = -2,
		OPCODE_RESULT_VARIADIC_LAST_MINUS_2 = -3,
	};

	struct OpcodeOperandList {
		uint64_t packed_kinds = 0;
		uint8_t count = 0;
	};

	struct OpcodeDescriptor {
		const char *name = nullptr;
		// Zero means the instruction is variadic and has an encoded argument count.
		uint8_t instruction_size = 0;
		OpcodeOperandList operand_kinds;
		int8_t result_operand = OPCODE_RESULT_NONE;
		OpcodeControlFlowKind control_flow_kind = CONTROL_FLOW_NEXT;
		OpcodeTypeConstraint type_constraints = TYPE_CONSTRAINT_NONE;
		OpcodeRelocationKind relocation_kind = RELOCATION_NONE;
	};

	enum Opcode {
#define GDSCRIPT_OPCODE(m_name, m_size, m_operands, m_result, m_flow, m_types, m_relocation) OPCODE_##m_name,
#include "gdscript_opcode.inc"
#undef GDSCRIPT_OPCODE
		OPCODE_COUNT,
	};

	static const OpcodeDescriptor &get_opcode_descriptor(Opcode p_opcode);
	static OpcodeOperandKind get_operand_kind(const int *p_code, int p_code_size, int p_ip, int p_word_offset);
	static int get_instruction_size(const int *p_code, int p_code_size, int p_ip);
	static int get_result_operand(const int *p_code, int p_code_size, int p_ip);

	enum MathOperatorMetadata {
		MATH_OPERATOR_SHIFT = 0,
		MATH_LEFT_TYPE_SHIFT = 8,
		MATH_RIGHT_TYPE_SHIFT = 16,
		MATH_RESULT_TYPE_SHIFT = 24,
		MATH_METADATA_MASK = 0xff,
	};

	static int make_math_operator_metadata(Variant::Operator p_operator, Variant::Type p_left_type, Variant::Type p_right_type, Variant::Type p_result_type) {
		return (int(p_operator) << MATH_OPERATOR_SHIFT) |
				(int(p_left_type) << MATH_LEFT_TYPE_SHIFT) |
				(int(p_right_type) << MATH_RIGHT_TYPE_SHIFT) |
				(int(p_result_type) << MATH_RESULT_TYPE_SHIFT);
	}

	static Variant::Operator get_math_operator(int p_metadata) {
		return Variant::Operator((p_metadata >> MATH_OPERATOR_SHIFT) & MATH_METADATA_MASK);
	}

	static Variant::Type get_math_left_type(int p_metadata) {
		return Variant::Type((p_metadata >> MATH_LEFT_TYPE_SHIFT) & MATH_METADATA_MASK);
	}

	static Variant::Type get_math_right_type(int p_metadata) {
		return Variant::Type((p_metadata >> MATH_RIGHT_TYPE_SHIFT) & MATH_METADATA_MASK);
	}

	static Variant::Type get_math_result_type(int p_metadata) {
		return Variant::Type((p_metadata >> MATH_RESULT_TYPE_SHIFT) & MATH_METADATA_MASK);
	}

	static int make_math_component_metadata(Variant::Type p_type, int p_component) {
		return int(p_type) | (p_component << 8);
	}

	static Variant::Type get_math_component_type(int p_metadata) {
		return Variant::Type(p_metadata & 0xff);
	}

	static int get_math_component_index(int p_metadata) {
		return (p_metadata >> 8) & 0xff;
	}

	enum Address {
		ADDR_BITS = 24,
		ADDR_MASK = ((1 << ADDR_BITS) - 1),
		ADDR_TYPE_MASK = ~ADDR_MASK,
		ADDR_TYPE_STACK = 0,
		ADDR_TYPE_CONSTANT = 1,
		ADDR_TYPE_MEMBER = 2,
		ADDR_TYPE_MAX = 3,
	};

	enum FixedAddresses {
		ADDR_STACK_SELF = 0,
		ADDR_STACK_CLASS = 1,
		ADDR_STACK_NIL = 2,
		FIXED_ADDRESSES_MAX = 3,
		ADDR_SELF = ADDR_STACK_SELF | (ADDR_TYPE_STACK << ADDR_BITS),
		ADDR_CLASS = ADDR_STACK_CLASS | (ADDR_TYPE_STACK << ADDR_BITS),
		ADDR_NIL = ADDR_STACK_NIL | (ADDR_TYPE_STACK << ADDR_BITS),
	};

	struct StackDebug {
		int line;
		int pos;
		bool added;
		StringName identifier;
	};

private:
	friend class GDScript;
	friend class GDScriptCompiler;
	friend class GDScriptByteCodeGenerator;
	friend class GDScriptCompiledModule;
	friend class GDScriptCompiledModuleImplementation::Internals;
	friend class GDScriptCompiledModuleImplementation::RuntimeBuilder;
	friend class GDScriptLanguage;
#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
	friend class GDScriptBaselineJIT;
#endif

	static constexpr int FEEDBACK_CACHE_SIZE = 4;

	struct OperatorFeedback {
		struct Entry {
			// Published last so readers see the evaluator and return type initialized.
			SafeNumeric<uint32_t> signature;
			Variant::Type return_type = Variant::NIL;
			Variant::ValidatedOperatorEvaluator evaluator = nullptr;
		};

		Entry entries[FEEDBACK_CACHE_SIZE];
		SafeFlag saturated;
	};

	struct CallFeedback {
		struct Entry {
			SafeNumeric<uint64_t> receiver_script_id;
			// Published last so readers see the script ID and function initialized.
			SafeNumeric<uint64_t> epoch;
			SafeNumeric<uintptr_t> function;
		};

		Entry entries[FEEDBACK_CACHE_SIZE];
		SafeNumeric<uint64_t> saturated_epoch;
	};

	StringName name;
	StringName source;
	bool _static = false;
	Vector<GDScriptDataType> argument_types;
	GDScriptDataType return_type;
	MethodInfo method_info;
	Variant rpc_config;

	GDScript *_script = nullptr;
	int _initial_line = 0;
	int _argument_count = 0;
	int _vararg_index = -1;
	int _stack_size = 0;
	int _instruction_args_size = 0;

	SelfList<GDScriptFunction> function_list{ this };
	mutable Variant nil;
	TightLocalVector<Pair<int, Variant::Type>> temporary_slots;
	List<StackDebug> stack_debug;

	Vector<int> code;
	Vector<int> default_arguments;
	Vector<Variant> constants;
	HashMap<StringName, Variant> constant_map;
	Vector<StringName> global_names;
	Vector<Variant::ValidatedOperatorEvaluator> operator_funcs;
	Vector<Variant::ValidatedSetter> setters;
	Vector<Variant::ValidatedGetter> getters;
	Vector<Variant::ValidatedKeyedSetter> keyed_setters;
	Vector<Variant::ValidatedKeyedGetter> keyed_getters;
	Vector<Variant::ValidatedIndexedSetter> indexed_setters;
	Vector<Variant::ValidatedIndexedGetter> indexed_getters;
	Vector<Variant::ValidatedBuiltInMethod> builtin_methods;
	Vector<Variant::ValidatedConstructor> constructors;
	Vector<Variant::ValidatedUtilityFunction> utilities;
	Vector<GDScriptUtilityFunctions::FunctionPtr> gds_utilities;
	Vector<MethodBind *> methods;
	Vector<GDScriptFunction *> lambdas;
	SafeNumeric<uintptr_t> *_operator_feedback_ptr = nullptr;
	SafeNumeric<uintptr_t> *_call_feedback_ptr = nullptr;
	int _operator_feedback_count = 0;
	int _call_feedback_count = 0;
	Mutex feedback_mutex;
#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
	GDScriptBaselineJIT *_baseline_jit = nullptr;
	SafeNumeric<uintptr_t> _optimizing_jit_ptr;
	SafeNumeric<uint32_t> _jit_call_count;
	SafeFlag _optimizing_jit_attempted;
	Mutex _jit_mutex;

	GDScriptBaselineJIT *_get_optimizing_jit() const;
	GDScriptBaselineJIT *_get_active_jit() const;
	void _maybe_compile_optimizing_jit();
#endif

	int _code_size = 0;
	int _default_arg_count = 0;
	int _constant_count = 0;
	int _global_names_count = 0;
	int _operator_funcs_count = 0;
	int _setters_count = 0;
	int _getters_count = 0;
	int _keyed_setters_count = 0;
	int _keyed_getters_count = 0;
	int _indexed_setters_count = 0;
	int _indexed_getters_count = 0;
	int _builtin_methods_count = 0;
	int _constructors_count = 0;
	int _utilities_count = 0;
	int _gds_utilities_count = 0;
	int _methods_count = 0;
	int _lambdas_count = 0;

	int *_code_ptr = nullptr;
	const int *_default_arg_ptr = nullptr;
	mutable Variant *_constants_ptr = nullptr;
	const StringName *_global_names_ptr = nullptr;
	const Variant::ValidatedOperatorEvaluator *_operator_funcs_ptr = nullptr;
	const Variant::ValidatedSetter *_setters_ptr = nullptr;
	const Variant::ValidatedGetter *_getters_ptr = nullptr;
	const Variant::ValidatedKeyedSetter *_keyed_setters_ptr = nullptr;
	const Variant::ValidatedKeyedGetter *_keyed_getters_ptr = nullptr;
	const Variant::ValidatedIndexedSetter *_indexed_setters_ptr = nullptr;
	const Variant::ValidatedIndexedGetter *_indexed_getters_ptr = nullptr;
	const Variant::ValidatedBuiltInMethod *_builtin_methods_ptr = nullptr;
	const Variant::ValidatedConstructor *_constructors_ptr = nullptr;
	const Variant::ValidatedUtilityFunction *_utilities_ptr = nullptr;
	const GDScriptUtilityFunctions::FunctionPtr *_gds_utilities_ptr = nullptr;
	MethodBind **_methods_ptr = nullptr;
	GDScriptFunction **_lambdas_ptr = nullptr;

#ifdef DEBUG_ENABLED
	CharString func_cname;
	const char *_func_cname = nullptr;

	Vector<String> operator_names;
	Vector<String> setter_names;
	Vector<String> getter_names;
	Vector<String> builtin_methods_names;
	Vector<String> constructors_names;
	Vector<String> utilities_names;
	Vector<String> gds_utilities_names;

	struct Profile {
		StringName signature;
		SafeNumeric<uint64_t> call_count;
		SafeNumeric<uint64_t> self_time;
		SafeNumeric<uint64_t> total_time;
		SafeNumeric<uint64_t> frame_call_count;
		SafeNumeric<uint64_t> frame_self_time;
		SafeNumeric<uint64_t> frame_total_time;
		uint64_t last_frame_call_count = 0;
		uint64_t last_frame_self_time = 0;
		uint64_t last_frame_total_time = 0;
		typedef struct NativeProfile {
			uint64_t call_count;
			uint64_t total_time;
			String signature;
		} NativeProfile;
		HashMap<String, NativeProfile> native_calls;
		HashMap<String, NativeProfile> last_native_calls;
	} profile;
#endif

	String _get_call_error(const String &p_where, const Variant **p_argptrs, int p_argcount, const Variant &p_ret, const Callable::CallError &p_err) const;
	String _get_callable_call_error(const String &p_where, const Callable &p_callable, const Variant **p_argptrs, int p_argcount, const Variant &p_ret, const Callable::CallError &p_err) const;
	Variant _get_default_variant_for_data_type(const GDScriptDataType &p_data_type);

public:
	static constexpr int MAX_CALL_DEPTH = 2048; // Limit to try to avoid crash because of a stack overflow.

	struct CallState {
		Signal completed;
		GDScript *script = nullptr;
		GDScriptInstance *instance = nullptr;
#ifdef DEBUG_ENABLED
		StringName function_name;
		String script_path;
#endif
		Vector<uint8_t> stack;
		int stack_size = 0;
		int ip = 0;
		int line = 0;
		int defarg = 0;
		Variant result;
	};

	_FORCE_INLINE_ StringName get_name() const { return name; }
	_FORCE_INLINE_ StringName get_source() const { return source; }
	_FORCE_INLINE_ GDScript *get_script() const { return _script; }
	_FORCE_INLINE_ bool is_static() const { return _static; }
	_FORCE_INLINE_ bool is_vararg() const { return _vararg_index >= 0; }
	_FORCE_INLINE_ MethodInfo get_method_info() const { return method_info; }
	_FORCE_INLINE_ int get_argument_count() const { return _argument_count; }
	_FORCE_INLINE_ Variant get_rpc_config() const { return rpc_config; }
	_FORCE_INLINE_ int get_max_stack_size() const { return _stack_size; }
	_FORCE_INLINE_ bool has_baseline_jit() const {
#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
		return _baseline_jit != nullptr;
#else
		return false;
#endif
	}
	bool has_typed_baseline_jit() const;
	int get_baseline_jit_ptrcall_count() const;
	bool has_optimizing_jit() const;
	int get_optimizing_jit_ssa_node_count() const;
	int get_optimizing_jit_eliminated_node_count() const;
	int get_optimizing_jit_scalar_replaced_math_value_count() const;
	String get_optimization_profile_key() const;
	uint32_t get_optimization_fingerprint() const;

	Variant get_constant(int p_idx) const;
	StringName get_global_name(int p_idx) const;

	Variant call(GDScriptInstance *p_instance, const Variant **p_args, int p_argcount, Callable::CallError &r_err, CallState *p_state = nullptr);
	void debug_get_stack_member_state(int p_line, List<Pair<StringName, int>> *r_stackvars) const;

#ifdef DEBUG_ENABLED
	void _profile_native_call(uint64_t p_t_taken, const String &p_function_name, const String &p_instance_class_name = String());
	void disassemble(const Vector<String> &p_code_lines) const;
#endif

	GDScriptFunction();
	~GDScriptFunction();
};

class GDScriptFunctionState : public RefCounted {
	GDCLASS(GDScriptFunctionState, RefCounted);

	friend class GDScriptFunction;

	GDScriptFunction *function = nullptr;
	GDScriptFunction::CallState state;

	SelfList<GDScriptFunctionState> scripts_list;
	SelfList<GDScriptFunctionState> instances_list;

	Variant _signal_callback(const Variant **p_args, int p_argcount, Callable::CallError &r_error);
	Variant resume(const Variant &p_arg);

protected:
	static void _bind_methods();

public:
#ifdef DEBUG_ENABLED
	// Returns a human-readable representation of the function.
	String get_readable_function() {
		return state.function_name;
	}
#endif

	void _clear_stack();
	void _clear_connections();

	GDScriptFunctionState();
	~GDScriptFunctionState();
};
