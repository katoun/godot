/**************************************************************************/
/*  gdscript_sljit_compiler.inc                                           */
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

// Baseline and optimizing native-code lowering through SLJIT.
class BaselineCompiler {
	struct PendingJump {
		struct sljit_jump *jump = nullptr;
		int target = 0;
	};

	const int *code = nullptr;
	int code_size = 0;
	int stack_size = 0;
	int constant_count = 0;
	const Variant *constants = nullptr;
	int method_count = 0;
	MethodBind *const *methods = nullptr;
	Vector<Variant::Type> argument_types;
	Vector<Ref<StructLayout>> argument_struct_layouts;
	Variant::Type return_type = Variant::NIL;
	Ref<StructLayout> return_struct_layout;
	struct sljit_compiler *compiler = nullptr;
	Vector<struct sljit_label *> labels;
	Vector<PendingJump> pending_jumps;
	Vector<Variant::Type> stack_slot_types;
	Vector<Ref<StructLayout>> stack_slot_struct_layouts;
	Vector<sljit_sw> stack_slot_offsets;
	bool typed_entry = false;
	sljit_sw variant_type_offset = 0;
	sljit_sw variant_data_offset = 0;
	sljit_sw ptrcall_arguments_offset = 0;
	sljit_s32 native_frame_size = 0;
	int max_ptrcall_argument_count = 0;
	int ptrcall_count = 0;
	bool requires_self = false;
	bool optimizing = false;
	CompactSSAPlan *ssa_plan = nullptr;
	int ssa_node_count = 0;
	int eliminated_node_count = 0;
	int scalar_replaced_math_value_count = 0;

	static bool are_compatible_struct_layouts(const Ref<StructLayout> &p_left, const Ref<StructLayout> &p_right) {
		return p_left.is_valid() && p_right.is_valid() && p_left->is_compatible(p_right);
	}

	static bool is_comparison(Variant::Operator p_operator) {
		return p_operator >= Variant::OP_EQUAL && p_operator <= Variant::OP_GREATER_EQUAL;
	}

	static bool is_supported_int_operator(Variant::Operator p_operator) {
		return is_comparison(p_operator) || p_operator == Variant::OP_ADD || p_operator == Variant::OP_SUBTRACT ||
				p_operator == Variant::OP_MULTIPLY || p_operator == Variant::OP_NEGATE || p_operator == Variant::OP_POSITIVE ||
				p_operator == Variant::OP_SHIFT_LEFT || p_operator == Variant::OP_SHIFT_RIGHT || p_operator == Variant::OP_BIT_AND ||
				p_operator == Variant::OP_BIT_OR || p_operator == Variant::OP_BIT_XOR || p_operator == Variant::OP_BIT_NEGATE;
	}

	static bool is_supported_float_operator(Variant::Operator p_operator) {
		return is_comparison(p_operator) || p_operator == Variant::OP_ADD || p_operator == Variant::OP_SUBTRACT ||
				p_operator == Variant::OP_MULTIPLY || p_operator == Variant::OP_DIVIDE || p_operator == Variant::OP_NEGATE ||
				p_operator == Variant::OP_POSITIVE;
	}

	static bool is_supported_math_operator(Variant::Operator p_operator, Variant::Type p_left_type, Variant::Type p_right_type, Variant::Type p_result_type) {
		if (is_unboxed_large_value_type(p_left_type)) {
			if (p_operator == Variant::OP_EQUAL || p_operator == Variant::OP_NOT_EQUAL) {
				return p_right_type == p_left_type && p_result_type == Variant::BOOL;
			}
			if (p_operator == Variant::OP_MULTIPLY && p_result_type == p_left_type) {
				if (p_left_type == Variant::PROJECTION) {
					return p_right_type == Variant::PROJECTION;
				}
				return p_left_type != Variant::AABB && (p_right_type == p_left_type || p_right_type == Variant::INT || p_right_type == Variant::FLOAT);
			}
			return p_operator == Variant::OP_DIVIDE && p_left_type != Variant::AABB && p_left_type != Variant::PROJECTION && p_result_type == p_left_type && (p_right_type == Variant::INT || p_right_type == Variant::FLOAT);
		}

		if (!is_unboxed_math_type(p_left_type) && !is_unboxed_math_type(p_right_type)) {
			return false;
		}
		if (p_operator == Variant::OP_NEGATE || p_operator == Variant::OP_POSITIVE) {
			return is_unboxed_math_type(p_left_type) && p_right_type == Variant::NIL && p_result_type == p_left_type;
		}
		if (p_operator == Variant::OP_EQUAL || p_operator == Variant::OP_NOT_EQUAL) {
			return is_unboxed_math_type(p_left_type) && p_right_type == p_left_type && p_result_type == Variant::BOOL;
		}
		if (p_operator == Variant::OP_ADD || p_operator == Variant::OP_SUBTRACT) {
			return is_unboxed_math_type(p_left_type) && p_right_type == p_left_type && p_result_type == p_left_type;
		}
		if (p_operator == Variant::OP_MULTIPLY) {
			return (is_unboxed_math_type(p_left_type) && p_result_type == p_left_type && (p_right_type == p_left_type || p_right_type == Variant::INT || p_right_type == Variant::FLOAT)) ||
					((p_left_type == Variant::INT || p_left_type == Variant::FLOAT) && is_unboxed_math_type(p_right_type) && p_result_type == p_right_type);
		}
		if (p_operator == Variant::OP_DIVIDE) {
			return is_unboxed_math_type(p_left_type) && p_result_type == p_left_type && (p_right_type == p_left_type || p_right_type == Variant::INT || p_right_type == Variant::FLOAT);
		}
		return false;
	}

	static int get_int_condition(Variant::Operator p_operator) {
		switch (p_operator) {
			case Variant::OP_EQUAL:
				return SLJIT_EQUAL;
			case Variant::OP_NOT_EQUAL:
				return SLJIT_NOT_EQUAL;
			case Variant::OP_LESS:
				return SLJIT_SIG_LESS;
			case Variant::OP_LESS_EQUAL:
				return SLJIT_SIG_LESS_EQUAL;
			case Variant::OP_GREATER:
				return SLJIT_SIG_GREATER;
			case Variant::OP_GREATER_EQUAL:
				return SLJIT_SIG_GREATER_EQUAL;
			default:
				return -1;
		}
	}

	static int get_float_condition(Variant::Operator p_operator) {
		switch (p_operator) {
			case Variant::OP_EQUAL:
				return SLJIT_ORDERED_EQUAL;
			case Variant::OP_NOT_EQUAL:
				return SLJIT_UNORDERED_OR_NOT_EQUAL;
			case Variant::OP_LESS:
				return SLJIT_ORDERED_LESS;
			case Variant::OP_LESS_EQUAL:
				return SLJIT_ORDERED_LESS_EQUAL;
			case Variant::OP_GREATER:
				return SLJIT_ORDERED_GREATER;
			case Variant::OP_GREATER_EQUAL:
				return SLJIT_ORDERED_GREATER_EQUAL;
			default:
				return -1;
		}
	}

	bool is_valid_address(int p_address, bool p_destination = false) const {
		int address_type = (p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		int address_index = p_address & GDScriptFunction::ADDR_MASK;
		if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
			return address_index >= 0 && address_index < stack_size;
		}
		if (!p_destination && address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
			return address_index >= 0 && address_index < constant_count;
		}
		return false;
	}

	Ref<StructLayout> get_constant_struct_layout(int p_address) const {
		const int address_type = (p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		const int address_index = p_address & GDScriptFunction::ADDR_MASK;
		if (address_type != GDScriptFunction::ADDR_TYPE_CONSTANT || address_index < 0 || address_index >= constant_count || constants == nullptr || constants[address_index].get_type() != Variant::STRUCT) {
			return Ref<StructLayout>();
		}
		const StructValue &value = VariantInternalAccessor<StructValue>::get(&constants[address_index]);
		return value.is_valid() ? value.get_layout() : Ref<StructLayout>();
	}

	Ref<StructLayout> get_address_struct_layout(int p_address) const {
		const int address_type = (p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		const int address_index = p_address & GDScriptFunction::ADDR_MASK;
		if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
			return get_constant_struct_layout(p_address);
		}
		if (address_type == GDScriptFunction::ADDR_TYPE_STACK && address_index >= 0 && address_index < stack_slot_struct_layouts.size()) {
			return stack_slot_struct_layouts[address_index];
		}
		return Ref<StructLayout>();
	}

	bool mark_address_type(int p_address, Variant::Type p_type, const Ref<StructLayout> &p_struct_layout = Ref<StructLayout>()) {
		if (!is_unboxed_native_type(p_type, p_struct_layout) || !is_valid_address(p_address)) {
			return false;
		}

		int address_type = (p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		int address_index = p_address & GDScriptFunction::ADDR_MASK;
		if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
			if (constants == nullptr || constants[address_index].get_type() != p_type) {
				return false;
			}
			// Boxed struct constants do not expose the layout's native byte representation.
			return p_type != Variant::STRUCT;
		}

		Variant::Type &slot_type = stack_slot_types.write[address_index];
		if (slot_type != Variant::NIL && slot_type != p_type) {
			return false;
		}
		if (p_type == Variant::STRUCT) {
			Ref<StructLayout> &slot_layout = stack_slot_struct_layouts.write[address_index];
			if (slot_layout.is_valid() && !are_compatible_struct_layouts(slot_layout, p_struct_layout)) {
				return false;
			}
			slot_layout = p_struct_layout;
		}
		slot_type = p_type;
		return true;
	}

	bool validate_ptrcall(int p_ip, GDScriptFunction::Opcode p_opcode) {
		if (p_ip + 2 > code_size) {
			return false;
		}

		const bool is_static = p_opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || p_opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN;
		const bool has_return = p_opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || p_opcode == GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN;
		const int instruction_argument_count = code[p_ip + 1];
		if (instruction_argument_count < (is_static ? 1 : 2) || p_ip + 4 + instruction_argument_count > code_size) {
			return false;
		}

		const int argument_count = code[p_ip + 2 + instruction_argument_count];
		if (argument_count < 0 || instruction_argument_count != argument_count + (is_static ? 1 : 2)) {
			return false;
		}

		const int method_index = code[p_ip + 3 + instruction_argument_count];
		if (method_index < 0 || method_index >= method_count || methods == nullptr || methods[method_index] == nullptr) {
			return false;
		}
		MethodBind *method = methods[method_index];
		if (method->is_vararg() || method->is_static() != is_static || method->has_return() != has_return || method->get_argument_count() != argument_count) {
			return false;
		}

		for (int i = 0; i < argument_count; i++) {
			if (!mark_address_type(code[p_ip + 2 + i], method->get_argument_type(i))) {
				return false;
			}
		}

		if (!is_static) {
			if (code[p_ip + 2 + argument_count] != GDScriptFunction::ADDR_SELF) {
				return false;
			}
			requires_self = true;
		}

		const int destination = code[p_ip + 1 + instruction_argument_count];
		if (!is_valid_address(destination, true)) {
			return false;
		}
		if (has_return && !mark_address_type(destination, method->get_return_info().type)) {
			return false;
		}

		// A zero-argument void call has no unboxed values to expose to ptrcall.
		if (argument_count == 0 && !has_return) {
			return false;
		}

		max_ptrcall_argument_count = MAX(max_ptrcall_argument_count, argument_count);
		ptrcall_count++;
		return true;
	}

	bool validate() {
		if (!code || code_size <= 0) {
			return false;
		}
		stack_slot_types.resize(stack_size);
		stack_slot_types.fill(Variant::NIL);
		stack_slot_struct_layouts.resize(stack_size);
		for (int i = 0; i < argument_types.size(); i++) {
			if (argument_types[i] == Variant::NIL) {
				continue;
			}
			const int stack_index = GDScriptFunction::FIXED_ADDRESSES_MAX + i;
			const Ref<StructLayout> layout = i < argument_struct_layouts.size() ? argument_struct_layouts[i] : Ref<StructLayout>();
			if (stack_index >= stack_size || !mark_address_type(stack_index, argument_types[i], layout)) {
				return false;
			}
		}

		Vector<uint8_t> boundaries;
		boundaries.resize(code_size + 1);
		boundaries.fill(0);
		Vector<int> jump_targets;
		bool has_native_work = false;
		int ip = 0;
		while (ip < code_size) {
			boundaries.write[ip] = 1;
			GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(code[ip]);
			switch (opcode) {
				case GDScriptFunction::OPCODE_ASSIGN_NULL:
					// A validated no-return call is followed by cleanup of its
					// temporary target. The target stays NIL in the Variant frame;
					// it has no corresponding native slot to clear.
					if (ip + 2 > code_size || !is_valid_address(code[ip + 1], true)) {
						return false;
					}
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_ASSIGN_BOOL:
				case GDScriptFunction::OPCODE_ASSIGN_INT:
				case GDScriptFunction::OPCODE_ASSIGN_FLOAT: {
					if (ip + 3 > code_size || !is_valid_address(code[ip + 1], true) || !is_valid_address(code[ip + 2])) {
						return false;
					}
					Variant::Type type = opcode == GDScriptFunction::OPCODE_ASSIGN_BOOL ? Variant::BOOL : (opcode == GDScriptFunction::OPCODE_ASSIGN_INT ? Variant::INT : Variant::FLOAT);
					if (!mark_address_type(code[ip + 1], type) || !mark_address_type(code[ip + 2], type)) {
						return false;
					}
					ip += 3;
				} break;
				case GDScriptFunction::OPCODE_ASSIGN_MATH: {
					if (ip + 4 > code_size || !is_valid_address(code[ip + 1], true) || !is_valid_address(code[ip + 2])) {
						return false;
					}
					const Variant::Type type = Variant::Type(code[ip + 3]);
					if (!is_unboxed_struct_type(type) || !mark_address_type(code[ip + 1], type) || !mark_address_type(code[ip + 2], type)) {
						return false;
					}
					has_native_work = has_native_work || is_unboxed_large_value_type(type);
					ip += 4;
				} break;
				case GDScriptFunction::OPCODE_CONSTRUCT_STRUCT: {
					if (ip + 2 > code_size) {
						return false;
					}
					const int instruction_argument_count = code[ip + 1];
					if (instruction_argument_count < 2 || ip + instruction_argument_count + 3 > code_size) {
						return false;
					}
					const int argument_count = code[ip + instruction_argument_count + 2];
					if (argument_count < 0 || instruction_argument_count != argument_count + 2) {
						return false;
					}
					const int destination = code[ip + argument_count + 2];
					const int layout_address = code[ip + argument_count + 3];
					const Ref<StructLayout> layout = get_constant_struct_layout(layout_address);
					if (!is_valid_address(destination, true) || !is_unboxed_user_struct_layout(layout) || (argument_count != 0 && argument_count != layout->get_field_count()) || !mark_address_type(destination, Variant::STRUCT, layout)) {
						return false;
					}
					for (int i = 0; i < argument_count; i++) {
						const StructLayout::Field &field = layout->get_field(i);
						if (!mark_address_type(code[ip + i + 2], field.type, field.struct_layout)) {
							return false;
						}
					}
					has_native_work = true;
					ip += instruction_argument_count + 3;
				} break;
				case GDScriptFunction::OPCODE_ASSIGN_STRUCT: {
					if (ip + 3 > code_size || !is_valid_address(code[ip + 1], true) || !is_valid_address(code[ip + 2])) {
						return false;
					}
					Ref<StructLayout> layout = get_address_struct_layout(code[ip + 1]);
					if (layout.is_null()) {
						layout = get_address_struct_layout(code[ip + 2]);
					}
					if (!mark_address_type(code[ip + 1], Variant::STRUCT, layout) || !mark_address_type(code[ip + 2], Variant::STRUCT, layout)) {
						return false;
					}
					has_native_work = true;
					ip += 3;
				} break;
				case GDScriptFunction::OPCODE_GET_STRUCT_FIELD:
				case GDScriptFunction::OPCODE_SET_STRUCT_FIELD: {
					const bool is_set = opcode == GDScriptFunction::OPCODE_SET_STRUCT_FIELD;
					if (ip + 4 > code_size || !is_valid_address(code[ip + 1], is_set) || !is_valid_address(code[ip + 2], !is_set)) {
						return false;
					}
					const Ref<StructLayout> layout = get_address_struct_layout(code[ip + 1]);
					const int field_index = code[ip + 3];
					if (!is_unboxed_user_struct_layout(layout) || field_index < 0 || field_index >= layout->get_field_count() || !mark_address_type(code[ip + 1], Variant::STRUCT, layout)) {
						return false;
					}
					const StructLayout::Field &field = layout->get_field(field_index);
					if (!mark_address_type(code[ip + 2], field.type, field.struct_layout)) {
						return false;
					}
					has_native_work = true;
					ip += 4;
				} break;
				case GDScriptFunction::OPCODE_EQUAL_STRUCT: {
					if (ip + 4 > code_size || !is_valid_address(code[ip + 1]) || !is_valid_address(code[ip + 2]) || !is_valid_address(code[ip + 3], true)) {
						return false;
					}
					Ref<StructLayout> layout = get_address_struct_layout(code[ip + 1]);
					if (layout.is_null()) {
						layout = get_address_struct_layout(code[ip + 2]);
					}
					if (!mark_address_type(code[ip + 1], Variant::STRUCT, layout) || !mark_address_type(code[ip + 2], Variant::STRUCT, layout) || !mark_address_type(code[ip + 3], Variant::BOOL)) {
						return false;
					}
					has_native_work = true;
					ip += 4;
				} break;
				case GDScriptFunction::OPCODE_BOX_STRUCT: {
					if (typed_entry || ip + 3 > code_size || !is_valid_address(code[ip + 1], true) || !is_valid_address(code[ip + 2])) {
						return false;
					}
					const Ref<StructLayout> layout = get_address_struct_layout(code[ip + 2]);
					if (!mark_address_type(code[ip + 2], Variant::STRUCT, layout)) {
						return false;
					}
					has_native_work = true;
					ip += 3;
				} break;
				case GDScriptFunction::OPCODE_OPERATOR_INT: {
					if (ip + 5 > code_size || !is_valid_address(code[ip + 1]) || !is_valid_address(code[ip + 3], true)) {
						return false;
					}
					Variant::Operator op = Variant::Operator(code[ip + 4]);
					if (!is_supported_int_operator(op) || (op != Variant::OP_NEGATE && op != Variant::OP_POSITIVE && op != Variant::OP_BIT_NEGATE && !is_valid_address(code[ip + 2]))) {
						return false;
					}
					if (!mark_address_type(code[ip + 1], Variant::INT) ||
							(op != Variant::OP_NEGATE && op != Variant::OP_POSITIVE && op != Variant::OP_BIT_NEGATE && !mark_address_type(code[ip + 2], Variant::INT)) ||
							!mark_address_type(code[ip + 3], is_comparison(op) ? Variant::BOOL : Variant::INT)) {
						return false;
					}
					has_native_work = true;
					ip += 5;
				} break;
				case GDScriptFunction::OPCODE_OPERATOR_FLOAT: {
					if (ip + 5 > code_size || !is_valid_address(code[ip + 1]) || !is_valid_address(code[ip + 3], true)) {
						return false;
					}
					Variant::Operator op = Variant::Operator(code[ip + 4]);
					if (!is_supported_float_operator(op) || (op != Variant::OP_NEGATE && op != Variant::OP_POSITIVE && !is_valid_address(code[ip + 2]))) {
						return false;
					}
					if (!mark_address_type(code[ip + 1], Variant::FLOAT) ||
							(op != Variant::OP_NEGATE && op != Variant::OP_POSITIVE && !mark_address_type(code[ip + 2], Variant::FLOAT)) ||
							!mark_address_type(code[ip + 3], is_comparison(op) ? Variant::BOOL : Variant::FLOAT)) {
						return false;
					}
					has_native_work = true;
					ip += 5;
				} break;
				case GDScriptFunction::OPCODE_OPERATOR_MATH: {
					if (ip + 6 > code_size || !is_valid_address(code[ip + 1]) || !is_valid_address(code[ip + 3], true)) {
						return false;
					}
					const int metadata = code[ip + 5];
					const Variant::Operator op = GDScriptFunction::get_math_operator(metadata);
					const Variant::Type left_type = GDScriptFunction::get_math_left_type(metadata);
					const Variant::Type right_type = GDScriptFunction::get_math_right_type(metadata);
					const Variant::Type result_type = GDScriptFunction::get_math_result_type(metadata);
					const bool unary = op == Variant::OP_NEGATE || op == Variant::OP_POSITIVE;
					if (!is_supported_math_operator(op, left_type, right_type, result_type) || (!unary && !is_valid_address(code[ip + 2]))) {
						return false;
					}
					if (!mark_address_type(code[ip + 1], left_type) || (!unary && !mark_address_type(code[ip + 2], right_type)) || !mark_address_type(code[ip + 3], result_type)) {
						return false;
					}
					has_native_work = true;
					ip += 6;
				} break;
				case GDScriptFunction::OPCODE_GET_MATH_COMPONENT:
				case GDScriptFunction::OPCODE_SET_MATH_COMPONENT: {
					if (ip + 4 > code_size || !is_valid_address(code[ip + 1], opcode == GDScriptFunction::OPCODE_SET_MATH_COMPONENT) || !is_valid_address(code[ip + 2], opcode == GDScriptFunction::OPCODE_GET_MATH_COMPONENT)) {
						return false;
					}
					const int metadata = code[ip + 3];
					const Variant::Type type = GDScriptFunction::get_math_component_type(metadata);
					const int component = GDScriptFunction::get_math_component_index(metadata);
					if (!is_unboxed_math_type(type) || component < 0 || component >= get_math_component_count(type)) {
						return false;
					}
					const int math_address = code[ip + 1];
					const int float_address = code[ip + 2];
					if (!mark_address_type(math_address, type) || !mark_address_type(float_address, Variant::FLOAT)) {
						return false;
					}
					has_native_work = true;
					ip += 4;
				} break;
				case GDScriptFunction::OPCODE_MATH_LENGTH: {
					if (ip + 4 > code_size || !is_valid_address(code[ip + 1]) || !is_valid_address(code[ip + 2], true)) {
						return false;
					}
					const Variant::Type type = Variant::Type(code[ip + 3]);
					if ((type != Variant::VECTOR2 && type != Variant::VECTOR3) || !mark_address_type(code[ip + 1], type) || !mark_address_type(code[ip + 2], Variant::FLOAT)) {
						return false;
					}
					has_native_work = true;
					ip += 4;
				} break;
				case GDScriptFunction::OPCODE_JUMP_COMPARE_INT:
				case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT: {
					if (ip + 6 > code_size || !is_valid_address(code[ip + 1]) || !is_valid_address(code[ip + 2]) || !is_comparison(Variant::Operator(code[ip + 3]))) {
						return false;
					}
					Variant::Type type = opcode == GDScriptFunction::OPCODE_JUMP_COMPARE_INT ? Variant::INT : Variant::FLOAT;
					if (!mark_address_type(code[ip + 1], type) || !mark_address_type(code[ip + 2], type)) {
						return false;
					}
					jump_targets.push_back(code[ip + 5]);
					has_native_work = true;
					ip += 6;
				} break;
				case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
				case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL:
					if (ip + 3 > code_size || !is_valid_address(code[ip + 1])) {
						return false;
					}
					if (!mark_address_type(code[ip + 1], Variant::BOOL)) {
						return false;
					}
					jump_targets.push_back(code[ip + 2]);
					has_native_work = true;
					ip += 3;
					break;
				case GDScriptFunction::OPCODE_JUMP:
					if (ip + 2 > code_size) {
						return false;
					}
					jump_targets.push_back(code[ip + 1]);
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN: {
					if (!validate_ptrcall(ip, opcode)) {
						return false;
					}
					has_native_work = true;
					ip += 4 + code[ip + 1];
				} break;
				case GDScriptFunction::OPCODE_RETURN:
					if (ip + 2 > code_size || !is_valid_address(code[ip + 1])) {
						return false;
					}
					if (typed_entry && !mark_address_type(code[ip + 1], return_type, return_struct_layout)) {
						return false;
					}
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT: {
					if (ip + 3 > code_size || !is_valid_address(code[ip + 1])) {
						return false;
					}
					const Ref<StructLayout> expected_layout = get_constant_struct_layout(code[ip + 2]);
					if (!mark_address_type(code[ip + 1], Variant::STRUCT, expected_layout) || (typed_entry && !are_compatible_struct_layouts(expected_layout, return_struct_layout))) {
						return false;
					}
					has_native_work = true;
					ip += 3;
				} break;
				case GDScriptFunction::OPCODE_LINE:
					if (ip + 2 > code_size) {
						return false;
					}
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_END:
					ip += 1;
					break;
				default:
					return false;
			}
		}

		boundaries.write[code_size] = 1;
		for (int target : jump_targets) {
			if (target < 0 || target > code_size || !boundaries[target]) {
				return false;
			}
		}

		stack_slot_offsets.resize(stack_size);
		stack_slot_offsets.fill(-1);
		int native_offset = 0;
		for (int i = 0; i < stack_slot_types.size(); i++) {
			if (stack_slot_types[i] != Variant::NIL) {
				const Ref<StructLayout> layout = stack_slot_struct_layouts[i];
				native_offset = align_native_offset(native_offset, MAX(NATIVE_VALUE_ALIGNMENT, get_unboxed_native_alignment(stack_slot_types[i], layout)));
				stack_slot_offsets.write[i] = native_offset;
				native_offset += get_unboxed_native_size(stack_slot_types[i], layout);
			}
		}
		if (native_offset > SLJIT_MAX_LOCAL_SIZE) {
			return false;
		}
		ptrcall_arguments_offset = align_native_offset(native_offset, NATIVE_VALUE_ALIGNMENT);
		const uint64_t frame_size = uint64_t(ptrcall_arguments_offset) + uint64_t(max_ptrcall_argument_count) * sizeof(void *);
		if (frame_size > SLJIT_MAX_LOCAL_SIZE) {
			return false;
		}
		native_frame_size = sljit_s32(frame_size);
		return has_native_work;
	}

	void emit_load_base(int p_address, int p_register) {
		int address_type = (p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
			sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(constants));
		} else {
			DEV_ASSERT(!typed_entry);
			sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_MEM1(SLJIT_S0), address_type * sizeof(Variant *));
		}
	}

	sljit_sw get_address_offset(int p_address, sljit_sw p_field_offset) const {
		return (p_address & GDScriptFunction::ADDR_MASK) * sizeof(Variant) + p_field_offset;
	}

	void emit_load_int(int p_address, int p_register) {
		if (((p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS) == GDScriptFunction::ADDR_TYPE_STACK) {
			sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[p_address & GDScriptFunction::ADDR_MASK]);
			return;
		}
		emit_load_base(p_address, p_register);
		sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_MEM1(p_register), get_address_offset(p_address, variant_data_offset));
	}

	void emit_load_bool(int p_address, int p_register) {
		if (((p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS) == GDScriptFunction::ADDR_TYPE_STACK) {
			sljit_emit_op1(compiler, SLJIT_MOV_U8, p_register, 0, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[p_address & GDScriptFunction::ADDR_MASK]);
			return;
		}
		emit_load_base(p_address, p_register);
		sljit_emit_op1(compiler, SLJIT_MOV_U8, p_register, 0, SLJIT_MEM1(p_register), get_address_offset(p_address, variant_data_offset));
	}

	void emit_load_float(int p_address, int p_float_register) {
		if (((p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS) == GDScriptFunction::ADDR_TYPE_STACK) {
			sljit_emit_fop1(compiler, SLJIT_MOV_F64, p_float_register, 0, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[p_address & GDScriptFunction::ADDR_MASK]);
			return;
		}
		emit_load_base(p_address, SLJIT_R2);
		sljit_emit_fop1(compiler, SLJIT_MOV_F64, p_float_register, 0, SLJIT_MEM1(SLJIT_R2), get_address_offset(p_address, variant_data_offset));
	}

	void emit_store_type(int p_address, Variant::Type p_type) {
		emit_load_base(p_address, SLJIT_R2);
		sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R2), get_address_offset(p_address, variant_type_offset), SLJIT_IMM, p_type);
	}

	void emit_store_int(int p_address, int p_register) {
		DEV_ASSERT(((p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS) == GDScriptFunction::ADDR_TYPE_STACK);
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[p_address & GDScriptFunction::ADDR_MASK], p_register, 0);
	}

	void emit_store_bool(int p_address, int p_register) {
		DEV_ASSERT(((p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS) == GDScriptFunction::ADDR_TYPE_STACK);
		sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[p_address & GDScriptFunction::ADDR_MASK], p_register, 0);
	}

	void emit_store_float(int p_address, int p_float_register) {
		DEV_ASSERT(((p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS) == GDScriptFunction::ADDR_TYPE_STACK);
		sljit_emit_fop1(compiler, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[p_address & GDScriptFunction::ADDR_MASK], p_float_register, 0);
	}

	void emit_unboxed_pointer(int p_address, int p_register) {
		const int address_type = (p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		if (address_type == GDScriptFunction::ADDR_TYPE_STACK) {
			sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, SLJIT_SP, 0, SLJIT_IMM, stack_slot_offsets[p_address & GDScriptFunction::ADDR_MASK]);
			return;
		}

		DEV_ASSERT(address_type == GDScriptFunction::ADDR_TYPE_CONSTANT);
		const int constant_index = p_address & GDScriptFunction::ADDR_MASK;
		const Variant::Type constant_type = constants[constant_index].get_type();
		if (is_unboxed_large_value_type(constant_type)) {
			sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(get_unboxed_variant_data(&constants[constant_index], constant_type)));
			return;
		}
		sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(constants));
		sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, p_register, 0, SLJIT_IMM, get_address_offset(p_address, variant_data_offset));
	}

	void emit_copy_memory(int p_destination_base, sljit_sw p_destination_offset, int p_source_base, sljit_sw p_source_offset, int p_size) {
		int copied = 0;
		while (p_size - copied >= int(sizeof(sljit_sw))) {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_MEM1(p_source_base), p_source_offset + copied);
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(p_destination_base), p_destination_offset + copied, SLJIT_R3, 0);
			copied += sizeof(sljit_sw);
		}
		if (p_size - copied >= int(sizeof(uint32_t))) {
			sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_MEM1(p_source_base), p_source_offset + copied);
			sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(p_destination_base), p_destination_offset + copied, SLJIT_R3, 0);
			copied += sizeof(uint32_t);
		}
		while (copied < p_size) {
			sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_R3, 0, SLJIT_MEM1(p_source_base), p_source_offset + copied);
			sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(p_destination_base), p_destination_offset + copied, SLJIT_R3, 0);
			copied++;
		}
	}

	void emit_zero_memory(int p_destination_base, sljit_sw p_destination_offset, int p_size) {
		int cleared = 0;
		while (p_size - cleared >= int(sizeof(sljit_sw))) {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(p_destination_base), p_destination_offset + cleared, SLJIT_IMM, 0);
			cleared += sizeof(sljit_sw);
		}
		if (p_size - cleared >= int(sizeof(uint32_t))) {
			sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(p_destination_base), p_destination_offset + cleared, SLJIT_IMM, 0);
			cleared += sizeof(uint32_t);
		}
		while (cleared < p_size) {
			sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(p_destination_base), p_destination_offset + cleared, SLJIT_IMM, 0);
			cleared++;
		}
	}

	void emit_copy_unboxed_value(int p_destination, int p_source, Variant::Type p_type, const Ref<StructLayout> &p_layout = Ref<StructLayout>()) {
		emit_unboxed_pointer(p_source, SLJIT_R0);
		emit_unboxed_pointer(p_destination, SLJIT_R1);
		emit_copy_memory(SLJIT_R1, 0, SLJIT_R0, 0, get_unboxed_native_size(p_type, p_layout));
	}

	void emit_construct_struct(int p_ip) {
		const int argument_count = code[p_ip + 1] - 2;
		const int destination = code[p_ip + argument_count + 2];
		const Ref<StructLayout> layout = get_constant_struct_layout(code[p_ip + argument_count + 3]);

		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(layout.ptr()));
		emit_unboxed_pointer(destination, SLJIT_R1);
		sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2V(P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_construct_struct));

		for (int i = 0; i < argument_count; i++) {
			const StructLayout::Field &field = layout->get_field(i);
			emit_unboxed_pointer(code[p_ip + i + 2], SLJIT_R0);
			emit_unboxed_pointer(destination, SLJIT_R1);
			emit_copy_memory(SLJIT_R1, field.native_offset, SLJIT_R0, 0, field.native_size);
		}
	}

	void emit_struct_field(int p_ip, bool p_set) {
		const Ref<StructLayout> layout = get_address_struct_layout(code[p_ip + 1]);
		const StructLayout::Field &field = layout->get_field(code[p_ip + 3]);
		emit_unboxed_pointer(code[p_ip + 1], SLJIT_R0);
		emit_unboxed_pointer(code[p_ip + 2], SLJIT_R1);
		if (p_set) {
			emit_copy_memory(SLJIT_R0, field.native_offset, SLJIT_R1, 0, field.native_size);
		} else {
			emit_copy_memory(SLJIT_R1, 0, SLJIT_R0, field.native_offset, field.native_size);
		}
	}

	void emit_equal_struct(int p_ip) {
		const Ref<StructLayout> layout = get_address_struct_layout(code[p_ip + 1]);
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(layout.ptr()));
		emit_unboxed_pointer(code[p_ip + 1], SLJIT_R1);
		emit_unboxed_pointer(code[p_ip + 2], SLJIT_R2);
		sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3(W, P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_equal_struct));
		emit_store_bool(code[p_ip + 3], SLJIT_R0);
	}

	void emit_box_struct(int p_ip) {
		const Ref<StructLayout> layout = get_address_struct_layout(code[p_ip + 2]);
		emit_variant_pointer(code[p_ip + 1], SLJIT_R0);
		emit_unboxed_pointer(code[p_ip + 2], SLJIT_R1);
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(layout.ptr()));
		sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_store_struct_variant));
	}

	void emit_ptrcall(int p_ip, GDScriptFunction::Opcode p_opcode) {
		const bool is_static = p_opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || p_opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN;
		const bool has_return = p_opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || p_opcode == GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN;
		const int instruction_argument_count = code[p_ip + 1];
		const int argument_count = code[p_ip + 2 + instruction_argument_count];
		MethodBind *method = methods[code[p_ip + 3 + instruction_argument_count]];

		for (int i = 0; i < argument_count; i++) {
			emit_unboxed_pointer(code[p_ip + 2 + i], SLJIT_R0);
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ptrcall_arguments_offset + i * sizeof(void *), SLJIT_R0, 0);
		}

		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(method));
		if (is_static) {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0);
		} else {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S1, 0);
		}
		if (argument_count == 0) {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, 0);
		} else {
			sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R2, 0, SLJIT_SP, 0, SLJIT_IMM, ptrcall_arguments_offset);
		}
		if (has_return) {
			const int destination = code[p_ip + 1 + instruction_argument_count];
			emit_zero_memory(SLJIT_SP, stack_slot_offsets[destination & GDScriptFunction::ADDR_MASK], get_unboxed_value_size(method->get_return_info().type));
			emit_unboxed_pointer(destination, SLJIT_R3);
		} else {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, 0);
		}
		sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS4V(P, P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_ptrcall));
	}

	void emit_initialize_unboxed_slots() {
		for (int i = 0; i < stack_slot_types.size(); i++) {
			Variant::Type type = stack_slot_types[i];
			if (type == Variant::NIL) {
				continue;
			}
			const Ref<StructLayout> layout = stack_slot_struct_layouts[i];
			const int native_size = get_unboxed_native_size(type, layout);

			sljit_sw native_offset = stack_slot_offsets[i];
			int argument_index = i - GDScriptFunction::FIXED_ADDRESSES_MAX;
			if (argument_index >= 0 && argument_index < argument_types.size()) {
				if (typed_entry) {
					sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), argument_index * sizeof(void *));
					emit_copy_memory(SLJIT_SP, native_offset, SLJIT_R0, 0, native_size);
				} else {
					if (type == Variant::STRUCT) {
						emit_variant_pointer(i, SLJIT_R0);
						sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(layout.ptr()));
						sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R2, 0, SLJIT_SP, 0, SLJIT_IMM, native_offset);
						sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_unbox_struct_variant));
					} else if (is_unboxed_large_value_type(type)) {
						emit_load_base(i, SLJIT_R0);
						sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0), get_address_offset(i, variant_data_offset));
						emit_copy_memory(SLJIT_SP, native_offset, SLJIT_R0, 0, native_size);
					} else {
						emit_load_base(i, SLJIT_R0);
						emit_copy_memory(SLJIT_SP, native_offset, SLJIT_R0, get_address_offset(i, variant_data_offset), native_size);
					}
				}
				continue;
			}

			emit_zero_memory(SLJIT_SP, native_offset, native_size);
		}
	}

	void emit_spill_unboxed_slots(int p_return_address) {
		DEV_ASSERT(!typed_entry);
		const bool return_is_stack = ((p_return_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS) == GDScriptFunction::ADDR_TYPE_STACK;
		const int return_slot = p_return_address & GDScriptFunction::ADDR_MASK;
		for (int i = 0; i < stack_slot_types.size(); i++) {
			Variant::Type type = stack_slot_types[i];
			if (type == Variant::NIL) {
				continue;
			}

			if (type == Variant::STRUCT) {
				if (!return_is_stack || i != return_slot) {
					continue;
				}
				emit_variant_pointer(i, SLJIT_R0);
				sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R1, 0, SLJIT_SP, 0, SLJIT_IMM, stack_slot_offsets[i]);
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(stack_slot_struct_layouts[i].ptr()));
				sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_store_struct_variant));
				continue;
			}

			if (is_unboxed_large_value_type(type)) {
				if (!return_is_stack || i != return_slot) {
					continue;
				}
				emit_variant_pointer(i, SLJIT_R0);
				sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R1, 0, SLJIT_SP, 0, SLJIT_IMM, stack_slot_offsets[i]);
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, type);
				sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(P, P, W), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_store_large_variant));
				continue;
			}

			emit_load_base(i, SLJIT_R2);
			sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R2), get_address_offset(i, variant_type_offset), SLJIT_IMM, type);
			emit_copy_memory(SLJIT_R2, get_address_offset(i, variant_data_offset), SLJIT_SP, stack_slot_offsets[i], get_unboxed_value_size(type));
		}
	}

	void emit_return_value(int p_address) {
		if (typed_entry) {
			emit_unboxed_pointer(p_address, SLJIT_R0);
			emit_copy_memory(SLJIT_S2, 0, SLJIT_R0, 0, get_unboxed_native_size(return_type, return_struct_layout));
			sljit_emit_return_void(compiler);
			return;
		}

		emit_spill_unboxed_slots(p_address);
		emit_variant_pointer(p_address, SLJIT_R0);
		sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
	}

	void emit_variant_pointer(int p_address, int p_register) {
		emit_load_base(p_address, p_register);
		sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, p_register, 0, SLJIT_IMM, (p_address & GDScriptFunction::ADDR_MASK) * sizeof(Variant));
	}

	void emit_materialized_int_comparison(int p_condition) {
		struct sljit_jump *true_jump = sljit_emit_cmp(compiler, p_condition, SLJIT_R0, 0, SLJIT_R1, 0);
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0);
		struct sljit_jump *end_jump = sljit_emit_jump(compiler, SLJIT_JUMP);
		sljit_set_label(true_jump, sljit_emit_label(compiler));
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 1);
		sljit_set_label(end_jump, sljit_emit_label(compiler));
	}

	void emit_materialized_float_comparison(int p_condition) {
		struct sljit_jump *true_jump = sljit_emit_fcmp(compiler, p_condition, SLJIT_FR0, 0, SLJIT_FR1, 0);
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0);
		struct sljit_jump *end_jump = sljit_emit_jump(compiler, SLJIT_JUMP);
		sljit_set_label(true_jump, sljit_emit_label(compiler));
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 1);
		sljit_set_label(end_jump, sljit_emit_label(compiler));
	}

	void add_pending_jump(struct sljit_jump *p_jump, int p_target) {
		PendingJump pending;
		pending.jump = p_jump;
		pending.target = p_target;
		pending_jumps.push_back(pending);
	}

	void emit_int_operator(int p_ip) {
		int left_address = code[p_ip + 1];
		int right_address = code[p_ip + 2];
		int destination = code[p_ip + 3];
		Variant::Operator op = Variant::Operator(code[p_ip + 4]);
		emit_load_int(left_address, SLJIT_R0);

		if (op != Variant::OP_NEGATE && op != Variant::OP_POSITIVE && op != Variant::OP_BIT_NEGATE) {
			emit_load_int(right_address, SLJIT_R1);
		}

		if (is_comparison(op)) {
			emit_materialized_int_comparison(get_int_condition(op));
			emit_store_bool(destination, SLJIT_R0);
			return;
		}

		switch (op) {
			case Variant::OP_ADD:
				sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				break;
			case Variant::OP_SUBTRACT:
				sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				break;
			case Variant::OP_MULTIPLY:
				sljit_emit_op2(compiler, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				break;
			case Variant::OP_NEGATE:
				sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_IMM, 0, SLJIT_R0, 0);
				break;
			case Variant::OP_POSITIVE:
				break;
			case Variant::OP_SHIFT_LEFT:
				sljit_emit_op2(compiler, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				break;
			case Variant::OP_SHIFT_RIGHT:
				sljit_emit_op2(compiler, SLJIT_ASHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				break;
			case Variant::OP_BIT_AND:
				sljit_emit_op2(compiler, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				break;
			case Variant::OP_BIT_OR:
				sljit_emit_op2(compiler, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				break;
			case Variant::OP_BIT_XOR:
				sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
				break;
			case Variant::OP_BIT_NEGATE:
				sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, -1);
				break;
			default:
				break;
		}
		emit_store_int(destination, SLJIT_R0);
	}

	void emit_float_operator(int p_ip) {
		int left_address = code[p_ip + 1];
		int right_address = code[p_ip + 2];
		int destination = code[p_ip + 3];
		Variant::Operator op = Variant::Operator(code[p_ip + 4]);
		emit_load_float(left_address, SLJIT_FR0);

		if (op != Variant::OP_NEGATE && op != Variant::OP_POSITIVE) {
			emit_load_float(right_address, SLJIT_FR1);
		}

		if (is_comparison(op)) {
			emit_materialized_float_comparison(get_float_condition(op));
			emit_store_bool(destination, SLJIT_R0);
			return;
		}

		switch (op) {
			case Variant::OP_ADD:
				sljit_emit_fop2(compiler, SLJIT_ADD_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
				break;
			case Variant::OP_SUBTRACT:
				sljit_emit_fop2(compiler, SLJIT_SUB_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
				break;
			case Variant::OP_MULTIPLY:
				sljit_emit_fop2(compiler, SLJIT_MUL_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
				break;
			case Variant::OP_DIVIDE:
				sljit_emit_fop2(compiler, SLJIT_DIV_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
				break;
			case Variant::OP_NEGATE:
				sljit_emit_fop1(compiler, SLJIT_NEG_F64, SLJIT_FR0, 0, SLJIT_FR0, 0);
				break;
			case Variant::OP_POSITIVE:
				break;
			default:
				break;
		}
		emit_store_float(destination, SLJIT_FR0);
	}

	void emit_load_math_operand(int p_pointer_register, Variant::Type p_type, Variant::Type p_math_type, int p_component, int p_float_register) {
		if (is_unboxed_math_type(p_type)) {
			sljit_emit_fop1(compiler, math_uses_double_components(p_type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32, p_float_register, 0, SLJIT_MEM1(p_pointer_register), p_component * get_math_component_size(p_type));
			return;
		}
		if (p_type == Variant::FLOAT) {
			sljit_emit_fop1(compiler, SLJIT_MOV_F64, p_float_register, 0, SLJIT_MEM1(p_pointer_register), 0);
			if (!math_uses_double_components(p_math_type)) {
				sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_F64, p_float_register, 0, p_float_register, 0);
			}
			return;
		}

		DEV_ASSERT(p_type == Variant::INT);
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_MEM1(p_pointer_register), 0);
		sljit_emit_fop1(compiler, math_uses_double_components(p_math_type) ? SLJIT_CONV_F64_FROM_SW : SLJIT_CONV_F32_FROM_SW, p_float_register, 0, SLJIT_R3, 0);
	}

	void emit_math_operator_values(int p_left_pointer, int p_right_pointer, int p_destination_pointer, Variant::Operator p_operator, Variant::Type p_left_type, Variant::Type p_right_type, Variant::Type p_result_type) {
		const Variant::Type math_type = is_unboxed_struct_type(p_left_type) ? p_left_type : p_right_type;
		if (is_unboxed_large_value_type(math_type)) {
			DEV_ASSERT(p_left_pointer == SLJIT_R0 && p_right_pointer == SLJIT_R1 && p_destination_pointer == SLJIT_R2);
			const int metadata = GDScriptFunction::make_math_operator_metadata(p_operator, p_left_type, p_right_type, p_result_type);
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, metadata);
			sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS4V(P, P, P, W), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_large_math_operator));
			return;
		}
		const int component_count = get_math_component_count(math_type);
		DEV_ASSERT(component_count > 0);

		if (p_operator == Variant::OP_EQUAL || p_operator == Variant::OP_NOT_EQUAL) {
			Vector<struct sljit_jump *> mismatch_jumps;
			int mismatch_condition = SLJIT_UNORDERED_OR_NOT_EQUAL;
			if (!math_uses_double_components(math_type)) {
				mismatch_condition |= SLJIT_32;
			}
			for (int component = 0; component < component_count; component++) {
				emit_load_math_operand(p_left_pointer, p_left_type, math_type, component, SLJIT_FR0);
				emit_load_math_operand(p_right_pointer, p_right_type, math_type, component, SLJIT_FR1);
				mismatch_jumps.push_back(sljit_emit_fcmp(compiler, mismatch_condition, SLJIT_FR0, 0, SLJIT_FR1, 0));
			}
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, p_operator == Variant::OP_EQUAL ? 1 : 0);
			struct sljit_jump *end_jump = sljit_emit_jump(compiler, SLJIT_JUMP);
			struct sljit_label *mismatch_label = sljit_emit_label(compiler);
			for (struct sljit_jump *jump : mismatch_jumps) {
				sljit_set_label(jump, mismatch_label);
			}
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, p_operator == Variant::OP_EQUAL ? 0 : 1);
			sljit_set_label(end_jump, sljit_emit_label(compiler));
			sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(p_destination_pointer), 0, SLJIT_R3, 0);
			return;
		}

		DEV_ASSERT(p_result_type == math_type);
		for (int component = 0; component < component_count; component++) {
			emit_load_math_operand(p_left_pointer, p_left_type, math_type, component, SLJIT_FR0);
			if (p_operator != Variant::OP_NEGATE && p_operator != Variant::OP_POSITIVE) {
				emit_load_math_operand(p_right_pointer, p_right_type, math_type, component, SLJIT_FR1);
			}
			const bool use_f64 = math_uses_double_components(math_type);
			const int add_op = use_f64 ? SLJIT_ADD_F64 : SLJIT_ADD_F32;
			const int sub_op = use_f64 ? SLJIT_SUB_F64 : SLJIT_SUB_F32;
			const int mul_op = use_f64 ? SLJIT_MUL_F64 : SLJIT_MUL_F32;
			const int div_op = use_f64 ? SLJIT_DIV_F64 : SLJIT_DIV_F32;
			const int neg_op = use_f64 ? SLJIT_NEG_F64 : SLJIT_NEG_F32;
			const int move_op = use_f64 ? SLJIT_MOV_F64 : SLJIT_MOV_F32;
			switch (p_operator) {
				case Variant::OP_ADD:
					sljit_emit_fop2(compiler, add_op, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
					break;
				case Variant::OP_SUBTRACT:
					sljit_emit_fop2(compiler, sub_op, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
					break;
				case Variant::OP_MULTIPLY:
					sljit_emit_fop2(compiler, mul_op, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
					break;
				case Variant::OP_DIVIDE:
					sljit_emit_fop2(compiler, div_op, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
					break;
				case Variant::OP_NEGATE:
					sljit_emit_fop1(compiler, neg_op, SLJIT_FR0, 0, SLJIT_FR0, 0);
					break;
				case Variant::OP_POSITIVE:
					break;
				default:
					DEV_ASSERT(false);
					break;
			}
			sljit_emit_fop1(compiler, move_op, SLJIT_MEM1(p_destination_pointer), component * get_math_component_size(math_type), SLJIT_FR0, 0);
		}
	}

	void emit_math_operator(int p_ip) {
		const int metadata = code[p_ip + 5];
		const Variant::Operator op = GDScriptFunction::get_math_operator(metadata);
		const Variant::Type left_type = GDScriptFunction::get_math_left_type(metadata);
		const Variant::Type right_type = GDScriptFunction::get_math_right_type(metadata);
		const Variant::Type result_type = GDScriptFunction::get_math_result_type(metadata);
		emit_unboxed_pointer(code[p_ip + 1], SLJIT_R0);
		if (op != Variant::OP_NEGATE && op != Variant::OP_POSITIVE) {
			emit_unboxed_pointer(code[p_ip + 2], SLJIT_R1);
		}
		emit_unboxed_pointer(code[p_ip + 3], SLJIT_R2);
		emit_math_operator_values(SLJIT_R0, SLJIT_R1, SLJIT_R2, op, left_type, right_type, result_type);
	}

	void emit_math_component_get(int p_ip) {
		const int metadata = code[p_ip + 3];
		const Variant::Type type = GDScriptFunction::get_math_component_type(metadata);
		const int component = GDScriptFunction::get_math_component_index(metadata);
		emit_unboxed_pointer(code[p_ip + 1], SLJIT_R0);
		sljit_emit_fop1(compiler, math_uses_double_components(type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_R0), component * get_math_component_size(type));
		if (!math_uses_double_components(type)) {
			sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_F32, SLJIT_FR0, 0, SLJIT_FR0, 0);
		}
		emit_store_float(code[p_ip + 2], SLJIT_FR0);
	}

	void emit_math_component_set(int p_ip) {
		const int metadata = code[p_ip + 3];
		const Variant::Type type = GDScriptFunction::get_math_component_type(metadata);
		const int component = GDScriptFunction::get_math_component_index(metadata);
		emit_load_float(code[p_ip + 2], SLJIT_FR0);
		if (!math_uses_double_components(type)) {
			sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_F64, SLJIT_FR0, 0, SLJIT_FR0, 0);
		}
		emit_unboxed_pointer(code[p_ip + 1], SLJIT_R0);
		sljit_emit_fop1(compiler, math_uses_double_components(type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_R0), component * get_math_component_size(type), SLJIT_FR0, 0);
	}

	void emit_math_length_from_pointer(int p_pointer, Variant::Type p_type, int p_destination_offset = -1) {
		const bool use_f64 = math_uses_double_components(p_type);
		const int move_op = use_f64 ? SLJIT_MOV_F64 : SLJIT_MOV_F32;
		const int mul_op = use_f64 ? SLJIT_MUL_F64 : SLJIT_MUL_F32;
		const int add_op = use_f64 ? SLJIT_ADD_F64 : SLJIT_ADD_F32;
		sljit_emit_fop1(compiler, move_op, SLJIT_FR0, 0, SLJIT_MEM1(p_pointer), 0);
		sljit_emit_fop2(compiler, mul_op, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR0, 0);
		for (int component = 1; component < get_math_component_count(p_type); component++) {
			sljit_emit_fop1(compiler, move_op, SLJIT_FR1, 0, SLJIT_MEM1(p_pointer), component * get_math_component_size(p_type));
			sljit_emit_fop2(compiler, mul_op, SLJIT_FR1, 0, SLJIT_FR1, 0, SLJIT_FR1, 0);
			sljit_emit_fop2(compiler, add_op, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
		}
		if (use_f64) {
			sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1(F64, F64), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_math_sqrt));
		} else {
			sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1(F32, F32), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_math_sqrt_f32));
			sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_F32, SLJIT_FR0, 0, SLJIT_FR0, 0);
		}
		if (p_destination_offset >= 0) {
			sljit_emit_fop1(compiler, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), p_destination_offset, SLJIT_FR0, 0);
		}
	}

	void emit_math_length(int p_ip) {
		emit_unboxed_pointer(code[p_ip + 1], SLJIT_R0);
		emit_math_length_from_pointer(SLJIT_R0, Variant::Type(code[p_ip + 3]));
		emit_store_float(code[p_ip + 2], SLJIT_FR0);
	}

	const CompactSSAPlan::Value &get_ssa_value(int p_value) const {
		p_value = ssa_plan->get_resolved_value(p_value);
		return ssa_plan->get_values()[p_value];
	}

	static int get_ssa_value_size(const CompactSSAPlan::Value &p_value) {
		return get_unboxed_native_size(p_value.type, p_value.struct_layout);
	}

	static int get_ssa_value_alignment(const CompactSSAPlan::Value &p_value) {
		return get_unboxed_native_alignment(p_value.type, p_value.struct_layout);
	}

	void emit_ssa_load_raw(int p_value, int p_register) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_value);
		if (value.is_constant) {
			sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_IMM, sljit_sw(value.constant_bits));
		} else {
			DEV_ASSERT(value.frame_offset >= 0);
			sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_MEM1(SLJIT_SP), value.frame_offset);
		}
	}

	void emit_ssa_load_float(int p_value, int p_float_register) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_value);
		if (value.is_constant) {
			double constant;
			memcpy(&constant, &value.constant_bits, sizeof(constant));
			sljit_emit_fset64(compiler, p_float_register, constant);
		} else {
			DEV_ASSERT(value.frame_offset >= 0);
			sljit_emit_fop1(compiler, SLJIT_MOV_F64, p_float_register, 0, SLJIT_MEM1(SLJIT_SP), value.frame_offset);
		}
	}

	void emit_ssa_load_math_component(int p_value, int p_float_register) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_value);
		DEV_ASSERT(is_unboxed_math_type(value.type) && value.components.is_empty());
		if (value.is_constant) {
			double constant;
			memcpy(&constant, &value.constant_bits, sizeof(constant));
			sljit_emit_fset64(compiler, p_float_register, constant);
			if (!math_uses_double_components(value.type)) {
				sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_F64, p_float_register, 0, p_float_register, 0);
			}
		} else {
			DEV_ASSERT(value.frame_offset >= 0);
			sljit_emit_fop1(compiler, math_uses_double_components(value.type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32, p_float_register, 0, SLJIT_MEM1(SLJIT_SP), value.frame_offset);
		}
	}

	void emit_ssa_store_math_component(const CompactSSAPlan::Value &p_value, int p_float_register) {
		DEV_ASSERT(p_value.frame_offset >= 0 && is_unboxed_math_type(p_value.type) && p_value.components.is_empty());
		sljit_emit_fop1(compiler, math_uses_double_components(p_value.type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), p_value.frame_offset, p_float_register, 0);
	}

	void emit_ssa_load_component_operand(int p_value, Variant::Type p_math_type, int p_float_register) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_value);
		if (is_unboxed_math_type(value.type)) {
			emit_ssa_load_math_component(p_value, p_float_register);
			return;
		}
		if (value.type == Variant::FLOAT) {
			emit_ssa_load_float(p_value, p_float_register);
			if (!math_uses_double_components(p_math_type)) {
				sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_F64, p_float_register, 0, p_float_register, 0);
			}
			return;
		}
		DEV_ASSERT(value.type == Variant::INT);
		emit_ssa_load_raw(p_value, SLJIT_R3);
		sljit_emit_fop1(compiler, math_uses_double_components(p_math_type) ? SLJIT_CONV_F64_FROM_SW : SLJIT_CONV_F32_FROM_SW, p_float_register, 0, SLJIT_R3, 0);
	}

	void emit_ssa_initialize_inputs() {
		for (int slot = 0; slot < stack_slot_types.size(); slot++) {
			const int initial = ssa_plan->get_initial_value(slot);
			if (initial < 0) {
				continue;
			}
			const CompactSSAPlan::Value &value = get_ssa_value(initial);
			if (is_unboxed_math_type(value.type) && !value.components.is_empty()) {
				const int argument_index = slot - GDScriptFunction::FIXED_ADDRESSES_MAX;
				for (int component = 0; component < value.components.size(); component++) {
					const CompactSSAPlan::Value &component_value = get_ssa_value(value.components[component]);
					if (!component_value.live || component_value.frame_offset < 0) {
						continue;
					}
					const int source_offset = component * get_math_component_size(value.type);
					const int move_op = math_uses_double_components(value.type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32;
					if (argument_index >= 0 && argument_index < argument_types.size()) {
						if (typed_entry) {
							sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), argument_index * sizeof(void *));
							sljit_emit_fop1(compiler, move_op, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_R0), source_offset);
						} else {
							emit_load_base(slot, SLJIT_R0);
							sljit_emit_fop1(compiler, move_op, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_R0), get_address_offset(slot, variant_data_offset) + source_offset);
						}
						emit_ssa_store_math_component(component_value, SLJIT_FR0);
					} else {
						emit_zero_memory(SLJIT_SP, component_value.frame_offset, get_math_component_size(value.type));
					}
				}
				continue;
			}
			if (!value.live || value.frame_offset < 0) {
				continue;
			}

			const int argument_index = slot - GDScriptFunction::FIXED_ADDRESSES_MAX;
			if (argument_index >= 0 && argument_index < argument_types.size()) {
				if (typed_entry) {
					sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), argument_index * sizeof(void *));
					emit_copy_memory(SLJIT_SP, value.frame_offset, SLJIT_R0, 0, get_ssa_value_size(value));
				} else {
					if (value.type == Variant::STRUCT) {
						emit_zero_memory(SLJIT_SP, value.frame_offset, get_ssa_value_size(value));
						emit_variant_pointer(slot, SLJIT_R0);
						sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(value.struct_layout.ptr()));
						sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R2, 0, SLJIT_SP, 0, SLJIT_IMM, value.frame_offset);
						sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_unbox_struct_variant));
					} else {
						emit_load_base(slot, SLJIT_R0);
						if (is_unboxed_large_value_type(value.type)) {
							sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0), get_address_offset(slot, variant_data_offset));
							emit_copy_memory(SLJIT_SP, value.frame_offset, SLJIT_R0, 0, get_unboxed_value_size(value.type));
						} else {
							emit_copy_memory(SLJIT_SP, value.frame_offset, SLJIT_R0, get_address_offset(slot, variant_data_offset), get_unboxed_value_size(value.type));
						}
					}
				}
			} else {
				emit_zero_memory(SLJIT_SP, value.frame_offset, get_ssa_value_size(value));
			}
		}
	}

	void emit_ssa_operator(const CompactSSAPlan::Instruction &p_instruction) {
		const int output = ssa_plan->get_resolved_value(p_instruction.output);
		if (output != p_instruction.output) {
			return;
		}
		const CompactSSAPlan::Value &value = ssa_plan->get_values()[output];
		if (!value.live || value.is_constant || value.frame_offset < 0) {
			if (value.kind != CompactSSAPlan::VALUE_MATH_OPERATOR || !is_unboxed_math_type(value.type) || value.components.is_empty()) {
				return;
			}
		}
		if (value.kind == CompactSSAPlan::VALUE_MATH_OPERATOR) {
			const int metadata = code[p_instruction.ip + 5];
			const Variant::Type left_type = GDScriptFunction::get_math_left_type(metadata);
			const Variant::Type right_type = GDScriptFunction::get_math_right_type(metadata);
			const Variant::Type math_type = is_unboxed_struct_type(left_type) ? left_type : right_type;
			if (is_unboxed_large_value_type(math_type)) {
				emit_ssa_pointer(value.inputs[0], 0, SLJIT_R0);
				emit_ssa_pointer(value.inputs[1], 1, SLJIT_R1);
				sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R2, 0, SLJIT_SP, 0, SLJIT_IMM, value.frame_offset);
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, metadata);
				sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS4V(P, P, P, W), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_large_math_operator));
				return;
			}
			if (value.op == Variant::OP_EQUAL || value.op == Variant::OP_NOT_EQUAL) {
				Vector<struct sljit_jump *> mismatch_jumps;
				int mismatch_condition = SLJIT_UNORDERED_OR_NOT_EQUAL;
				if (!math_uses_double_components(math_type)) {
					mismatch_condition |= SLJIT_32;
				}
				for (int component = 0; component < get_math_component_count(math_type); component++) {
					emit_ssa_load_math_component(ssa_plan->get_component_value(value.inputs[0], component), SLJIT_FR0);
					emit_ssa_load_math_component(ssa_plan->get_component_value(value.inputs[1], component), SLJIT_FR1);
					mismatch_jumps.push_back(sljit_emit_fcmp(compiler, mismatch_condition, SLJIT_FR0, 0, SLJIT_FR1, 0));
				}
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, value.op == Variant::OP_EQUAL ? 1 : 0);
				struct sljit_jump *end_jump = sljit_emit_jump(compiler, SLJIT_JUMP);
				struct sljit_label *mismatch_label = sljit_emit_label(compiler);
				for (struct sljit_jump *jump : mismatch_jumps) {
					sljit_set_label(jump, mismatch_label);
				}
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, value.op == Variant::OP_EQUAL ? 0 : 1);
				sljit_set_label(end_jump, sljit_emit_label(compiler));
				sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_SP), value.frame_offset, SLJIT_R0, 0);
				return;
			}

			const bool use_f64 = math_uses_double_components(math_type);
			for (int component_index = 0; component_index < value.components.size(); component_index++) {
				const CompactSSAPlan::Value &component = get_ssa_value(value.components[component_index]);
				if (!component.live || component.is_constant || component.frame_offset < 0) {
					continue;
				}
				emit_ssa_load_component_operand(component.inputs[0], math_type, SLJIT_FR0);
				if (component.inputs.size() > 1) {
					emit_ssa_load_component_operand(component.inputs[1], math_type, SLJIT_FR1);
				}
				switch (value.op) {
					case Variant::OP_ADD: sljit_emit_fop2(compiler, use_f64 ? SLJIT_ADD_F64 : SLJIT_ADD_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0); break;
					case Variant::OP_SUBTRACT: sljit_emit_fop2(compiler, use_f64 ? SLJIT_SUB_F64 : SLJIT_SUB_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0); break;
					case Variant::OP_MULTIPLY: sljit_emit_fop2(compiler, use_f64 ? SLJIT_MUL_F64 : SLJIT_MUL_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0); break;
					case Variant::OP_DIVIDE: sljit_emit_fop2(compiler, use_f64 ? SLJIT_DIV_F64 : SLJIT_DIV_F32, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0); break;
					case Variant::OP_NEGATE: sljit_emit_fop1(compiler, use_f64 ? SLJIT_NEG_F64 : SLJIT_NEG_F32, SLJIT_FR0, 0, SLJIT_FR0, 0); break;
					case Variant::OP_POSITIVE: break;
					default: break;
				}
				emit_ssa_store_math_component(component, SLJIT_FR0);
			}
			return;
		}

		if (value.kind == CompactSSAPlan::VALUE_INT_OPERATOR) {
			emit_ssa_load_raw(value.inputs[0], SLJIT_R0);
			if (value.inputs.size() > 1) {
				emit_ssa_load_raw(value.inputs[1], SLJIT_R1);
			}
			if (is_comparison(value.op)) {
				emit_materialized_int_comparison(get_int_condition(value.op));
			} else {
				switch (value.op) {
					case Variant::OP_ADD: sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;
					case Variant::OP_SUBTRACT: sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;
					case Variant::OP_MULTIPLY: sljit_emit_op2(compiler, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;
					case Variant::OP_NEGATE: sljit_emit_op2(compiler, SLJIT_SUB, SLJIT_R0, 0, SLJIT_IMM, 0, SLJIT_R0, 0); break;
					case Variant::OP_POSITIVE: break;
					case Variant::OP_SHIFT_LEFT: sljit_emit_op2(compiler, SLJIT_SHL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;
					case Variant::OP_SHIFT_RIGHT: sljit_emit_op2(compiler, SLJIT_ASHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;
					case Variant::OP_BIT_AND: sljit_emit_op2(compiler, SLJIT_AND, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;
					case Variant::OP_BIT_OR: sljit_emit_op2(compiler, SLJIT_OR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;
					case Variant::OP_BIT_XOR: sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0); break;
					case Variant::OP_BIT_NEGATE: sljit_emit_op2(compiler, SLJIT_XOR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, -1); break;
					default: break;
				}
			}
			sljit_emit_op1(compiler, value.type == Variant::BOOL ? SLJIT_MOV_U8 : SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), value.frame_offset, SLJIT_R0, 0);
			return;
		}

		emit_ssa_load_float(value.inputs[0], SLJIT_FR0);
		if (value.inputs.size() > 1) {
			emit_ssa_load_float(value.inputs[1], SLJIT_FR1);
		}
		if (is_comparison(value.op)) {
			emit_materialized_float_comparison(get_float_condition(value.op));
			sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_SP), value.frame_offset, SLJIT_R0, 0);
			return;
		}
		switch (value.op) {
			case Variant::OP_ADD: sljit_emit_fop2(compiler, SLJIT_ADD_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0); break;
			case Variant::OP_SUBTRACT: sljit_emit_fop2(compiler, SLJIT_SUB_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0); break;
			case Variant::OP_MULTIPLY: sljit_emit_fop2(compiler, SLJIT_MUL_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0); break;
			case Variant::OP_DIVIDE: sljit_emit_fop2(compiler, SLJIT_DIV_F64, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0); break;
			case Variant::OP_NEGATE: sljit_emit_fop1(compiler, SLJIT_NEG_F64, SLJIT_FR0, 0, SLJIT_FR0, 0); break;
			case Variant::OP_POSITIVE: break;
			default: break;
		}
		sljit_emit_fop1(compiler, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), value.frame_offset, SLJIT_FR0, 0);
	}

	void emit_ssa_math_extract(const CompactSSAPlan::Instruction &p_instruction) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_instruction.output);
		if (!value.live || value.is_constant || value.frame_offset < 0) {
			return;
		}
		const int component = GDScriptFunction::get_math_component_index(code[p_instruction.ip + 3]);
		const int component_value = ssa_plan->get_component_value(value.inputs[0], component);
		emit_ssa_load_math_component(component_value, SLJIT_FR0);
		const CompactSSAPlan::Value &source_component = get_ssa_value(component_value);
		if (!math_uses_double_components(source_component.type)) {
			sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_F32, SLJIT_FR0, 0, SLJIT_FR0, 0);
		}
		sljit_emit_fop1(compiler, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), value.frame_offset, SLJIT_FR0, 0);
	}

	void emit_ssa_math_component_set(const CompactSSAPlan::Instruction &p_instruction) {
		const CompactSSAPlan::Value &aggregate = get_ssa_value(p_instruction.output);
		const int component_index = GDScriptFunction::get_math_component_index(code[p_instruction.ip + 3]);
		const CompactSSAPlan::Value &component = get_ssa_value(aggregate.components[component_index]);
		if (!component.live || component.is_constant || component.frame_offset < 0) {
			return;
		}
		emit_ssa_load_float(component.inputs[0], SLJIT_FR0);
		if (!math_uses_double_components(component.type)) {
			sljit_emit_fop1(compiler, SLJIT_CONV_F32_FROM_F64, SLJIT_FR0, 0, SLJIT_FR0, 0);
		}
		emit_ssa_store_math_component(component, SLJIT_FR0);
	}

	void emit_ssa_math_length(const CompactSSAPlan::Instruction &p_instruction) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_instruction.output);
		if (!value.live || value.is_constant || value.frame_offset < 0) {
			return;
		}
		const Variant::Type math_type = Variant::Type(code[p_instruction.ip + 3]);
		const bool use_f64 = math_uses_double_components(math_type);
		const int mul_op = use_f64 ? SLJIT_MUL_F64 : SLJIT_MUL_F32;
		const int add_op = use_f64 ? SLJIT_ADD_F64 : SLJIT_ADD_F32;
		for (int component = 0; component < get_math_component_count(math_type); component++) {
			emit_ssa_load_math_component(ssa_plan->get_component_value(value.inputs[0], component), SLJIT_FR1);
			sljit_emit_fop2(compiler, mul_op, SLJIT_FR1, 0, SLJIT_FR1, 0, SLJIT_FR1, 0);
			if (component == 0) {
				sljit_emit_fop1(compiler, use_f64 ? SLJIT_MOV_F64 : SLJIT_MOV_F32, SLJIT_FR0, 0, SLJIT_FR1, 0);
			} else {
				sljit_emit_fop2(compiler, add_op, SLJIT_FR0, 0, SLJIT_FR0, 0, SLJIT_FR1, 0);
			}
		}
		if (use_f64) {
			sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1(F64, F64), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_math_sqrt));
		} else {
			sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS1(F32, F32), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_math_sqrt_f32));
			sljit_emit_fop1(compiler, SLJIT_CONV_F64_FROM_F32, SLJIT_FR0, 0, SLJIT_FR0, 0);
		}
		sljit_emit_fop1(compiler, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), value.frame_offset, SLJIT_FR0, 0);
	}

	void emit_ssa_struct_construct(const CompactSSAPlan::Instruction &p_instruction) {
		const CompactSSAPlan::Value &output = get_ssa_value(p_instruction.output);
		if (!output.live || output.frame_offset < 0 || output.struct_layout.is_null()) {
			return;
		}
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(output.struct_layout.ptr()));
		sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R1, 0, SLJIT_SP, 0, SLJIT_IMM, output.frame_offset);
		sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS2V(P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_construct_struct));

		for (int i = 0; i < p_instruction.inputs.size(); i++) {
			const StructLayout::Field &field = output.struct_layout->get_field(i);
			emit_ssa_pointer(p_instruction.inputs[i], i, SLJIT_R0);
			emit_copy_memory(SLJIT_SP, output.frame_offset + field.native_offset, SLJIT_R0, 0, field.native_size);
		}
	}

	void emit_ssa_struct_field(const CompactSSAPlan::Instruction &p_instruction, bool p_set) {
		const int output_index = ssa_plan->get_resolved_value(p_instruction.output);
		if (output_index != p_instruction.output) {
			return;
		}
		const CompactSSAPlan::Value &output = ssa_plan->get_values()[output_index];
		if (p_set) {
			if (!output.live || output.frame_offset < 0 || output.struct_layout.is_null()) {
				return;
			}
			const StructLayout::Field &field = output.struct_layout->get_field(output.struct_field_index);
			emit_ssa_pointer(output.inputs[0], 0, SLJIT_R0);
			emit_copy_memory(SLJIT_SP, output.frame_offset, SLJIT_R0, 0, get_ssa_value_size(output));
			emit_ssa_pointer(output.inputs[1], 1, SLJIT_R0);
			emit_copy_memory(SLJIT_SP, output.frame_offset + field.native_offset, SLJIT_R0, 0, field.native_size);
			return;
		}

		const CompactSSAPlan::Value &source = get_ssa_value(output.inputs[0]);
		DEV_ASSERT(source.type == Variant::STRUCT && source.struct_layout.is_valid());
		const StructLayout::Field &field = source.struct_layout->get_field(output.struct_field_index);
		if (is_unboxed_math_type(output.type) && !output.components.is_empty()) {
			emit_ssa_pointer(output.inputs[0], 0, SLJIT_R0);
			const int move_op = math_uses_double_components(output.type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32;
			for (int component = 0; component < output.components.size(); component++) {
				const CompactSSAPlan::Value &component_value = get_ssa_value(output.components[component]);
				if (!component_value.live || component_value.is_constant || component_value.frame_offset < 0) {
					continue;
				}
				sljit_emit_fop1(compiler, move_op, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_R0), field.native_offset + component * get_math_component_size(output.type));
				emit_ssa_store_math_component(component_value, SLJIT_FR0);
			}
			return;
		}
		if (!output.live || output.frame_offset < 0) {
			return;
		}
		emit_ssa_pointer(output.inputs[0], 0, SLJIT_R0);
		emit_copy_memory(SLJIT_SP, output.frame_offset, SLJIT_R0, field.native_offset, field.native_size);
	}

	void emit_ssa_struct_equal(const CompactSSAPlan::Instruction &p_instruction) {
		const CompactSSAPlan::Value &output = get_ssa_value(p_instruction.output);
		if (!output.live || output.frame_offset < 0) {
			return;
		}
		const CompactSSAPlan::Value &left = get_ssa_value(output.inputs[0]);
		DEV_ASSERT(left.type == Variant::STRUCT && left.struct_layout.is_valid());
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(left.struct_layout.ptr()));
		emit_ssa_pointer(output.inputs[0], 0, SLJIT_R1);
		emit_ssa_pointer(output.inputs[1], 1, SLJIT_R2);
		sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3(W, P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_equal_struct));
		sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_SP), output.frame_offset, SLJIT_R0, 0);
	}

	void emit_ssa_pointer(int p_value, int p_argument, int p_register) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_value);
		if (value.kind == CompactSSAPlan::VALUE_CONSTANT && value.constant_index >= 0) {
			if (is_unboxed_large_value_type(value.type)) {
				sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(get_unboxed_variant_data(&constants[value.constant_index], value.type)));
			} else {
				sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(constants));
				sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, p_register, 0, SLJIT_IMM, value.constant_index * sizeof(Variant) + variant_data_offset);
			}
		} else if (value.is_constant) {
			const int offset = ssa_plan->get_ptrcall_values_offset() + p_argument * NATIVE_VALUE_STRIDE;
			sljit_emit_op1(compiler, value.type == Variant::BOOL ? SLJIT_MOV_U8 : SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), offset, SLJIT_IMM, sljit_sw(value.constant_bits));
			sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, SLJIT_SP, 0, SLJIT_IMM, offset);
		} else if (is_unboxed_math_type(value.type) && !value.components.is_empty()) {
			const int offset = ssa_plan->get_ptrcall_values_offset() + p_argument * NATIVE_VALUE_STRIDE;
			const int move_op = math_uses_double_components(value.type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32;
			for (int component = 0; component < value.components.size(); component++) {
				emit_ssa_load_math_component(value.components[component], SLJIT_FR0);
				sljit_emit_fop1(compiler, move_op, SLJIT_MEM1(SLJIT_SP), offset + component * get_math_component_size(value.type), SLJIT_FR0, 0);
			}
			sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, SLJIT_SP, 0, SLJIT_IMM, offset);
		} else {
			DEV_ASSERT(value.frame_offset >= 0);
			sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, SLJIT_SP, 0, SLJIT_IMM, value.frame_offset);
		}
	}

	void emit_ssa_ptrcall(const CompactSSAPlan::Instruction &p_instruction) {
		const int ip = p_instruction.ip;
		const bool is_static = p_instruction.opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || p_instruction.opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN;
		const bool has_return = p_instruction.opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || p_instruction.opcode == GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN;
		const int instruction_argument_count = code[ip + 1];
		const int argument_count = code[ip + 2 + instruction_argument_count];
		MethodBind *method = methods[code[ip + 3 + instruction_argument_count]];

		for (int i = 0; i < argument_count; i++) {
			emit_ssa_pointer(p_instruction.inputs[i], i, SLJIT_R0);
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ssa_plan->get_ptrcall_arguments_offset() + i * sizeof(void *), SLJIT_R0, 0);
		}
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(method));
		sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, is_static ? SLJIT_IMM : SLJIT_S1, 0);
		if (argument_count == 0) {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, 0);
		} else {
			sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R2, 0, SLJIT_SP, 0, SLJIT_IMM, ssa_plan->get_ptrcall_arguments_offset());
		}
		if (has_return) {
			const CompactSSAPlan::Value &output = get_ssa_value(p_instruction.output);
			const int result_offset = output.live && !output.is_constant && output.frame_offset >= 0 ? output.frame_offset : ssa_plan->get_ptrcall_return_offset();
			emit_zero_memory(SLJIT_SP, result_offset, get_unboxed_value_size(output.type));
			sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R3, 0, SLJIT_SP, 0, SLJIT_IMM, result_offset);
		} else {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, 0);
		}
		sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS4V(P, P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_ptrcall));
		if (has_return) {
			const CompactSSAPlan::Value &output = get_ssa_value(p_instruction.output);
			if (is_unboxed_math_type(output.type) && !output.components.is_empty()) {
				const int move_op = math_uses_double_components(output.type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32;
				for (int component = 0; component < output.components.size(); component++) {
					const CompactSSAPlan::Value &component_value = get_ssa_value(output.components[component]);
					if (!component_value.live || component_value.frame_offset < 0) {
						continue;
					}
					sljit_emit_fop1(compiler, move_op, SLJIT_FR0, 0, SLJIT_MEM1(SLJIT_SP), ssa_plan->get_ptrcall_return_offset() + component * get_math_component_size(output.type));
					emit_ssa_store_math_component(component_value, SLJIT_FR0);
				}
			}
		}
	}

	void emit_ssa_edge_copies(int p_predecessor, int p_successor) {
		const CompactSSAPlan::Block &target = ssa_plan->get_blocks()[p_successor];
		int predecessor_input = -1;
		int reachable_index = 0;
		for (int predecessor : target.predecessors) {
			if (!ssa_plan->get_blocks()[predecessor].reachable) {
				continue;
			}
			if (predecessor == p_predecessor) {
				predecessor_input = reachable_index;
				break;
			}
			reachable_index++;
		}
		DEV_ASSERT(predecessor_input >= 0);

		int copy_offset = 0;
		for (int phi : target.phis) {
			if (phi < 0) {
				continue;
			}
			const CompactSSAPlan::Value &destination = get_ssa_value(phi);
			if (destination.is_constant) {
				continue;
			}
			if (is_unboxed_math_type(destination.type) && !destination.components.is_empty()) {
				const int incoming = ssa_plan->get_values()[phi].inputs[predecessor_input];
				for (int component = 0; component < destination.components.size(); component++) {
					const CompactSSAPlan::Value &component_destination = get_ssa_value(destination.components[component]);
					if (!component_destination.live || component_destination.is_constant || component_destination.frame_offset < 0) {
						continue;
					}
					copy_offset = align_native_offset(copy_offset, get_math_component_size(destination.type));
					emit_ssa_load_math_component(ssa_plan->get_component_value(incoming, component), SLJIT_FR0);
					sljit_emit_fop1(compiler, math_uses_double_components(destination.type) ? SLJIT_MOV_F64 : SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ssa_plan->get_phi_scratch_offset() + copy_offset, SLJIT_FR0, 0);
					copy_offset += get_math_component_size(destination.type);
				}
				continue;
			}
			if (!destination.live || destination.frame_offset < 0) {
				continue;
			}
			copy_offset = align_native_offset(copy_offset, get_ssa_value_alignment(destination));
			emit_ssa_pointer(ssa_plan->get_values()[phi].inputs[predecessor_input], 0, SLJIT_R0);
			emit_copy_memory(SLJIT_SP, ssa_plan->get_phi_scratch_offset() + copy_offset, SLJIT_R0, 0, get_ssa_value_size(destination));
			copy_offset += get_ssa_value_size(destination);
		}
		copy_offset = 0;
		for (int phi : target.phis) {
			if (phi < 0) {
				continue;
			}
			const CompactSSAPlan::Value &destination = get_ssa_value(phi);
			if (destination.is_constant) {
				continue;
			}
			if (is_unboxed_math_type(destination.type) && !destination.components.is_empty()) {
				for (int component = 0; component < destination.components.size(); component++) {
					const CompactSSAPlan::Value &component_destination = get_ssa_value(destination.components[component]);
					if (!component_destination.live || component_destination.is_constant || component_destination.frame_offset < 0) {
						continue;
					}
					copy_offset = align_native_offset(copy_offset, get_math_component_size(destination.type));
					emit_copy_memory(SLJIT_SP, component_destination.frame_offset, SLJIT_SP, ssa_plan->get_phi_scratch_offset() + copy_offset, get_math_component_size(destination.type));
					copy_offset += get_math_component_size(destination.type);
				}
				continue;
			}
			if (!destination.live || destination.frame_offset < 0) {
				continue;
			}
			copy_offset = align_native_offset(copy_offset, get_ssa_value_alignment(destination));
			emit_copy_memory(SLJIT_SP, destination.frame_offset, SLJIT_SP, ssa_plan->get_phi_scratch_offset() + copy_offset, get_ssa_value_size(destination));
			copy_offset += get_ssa_value_size(destination);
		}
	}

	void emit_ssa_jump(int p_predecessor, int p_successor) {
		emit_ssa_edge_copies(p_predecessor, p_successor);
		add_pending_jump(sljit_emit_jump(compiler, SLJIT_JUMP), p_successor);
	}

	bool get_ssa_constant_branch(const CompactSSAPlan::Instruction &p_instruction, bool &r_taken) const {
		if (p_instruction.opcode == GDScriptFunction::OPCODE_JUMP_IF_BOOL || p_instruction.opcode == GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL) {
			const CompactSSAPlan::Value &condition = get_ssa_value(p_instruction.inputs[0]);
			if (!condition.is_constant) {
				return false;
			}
			r_taken = bool(condition.constant_bits) == (p_instruction.opcode == GDScriptFunction::OPCODE_JUMP_IF_BOOL);
			return true;
		}

		const CompactSSAPlan::Value &left = get_ssa_value(p_instruction.inputs[0]);
		const CompactSSAPlan::Value &right = get_ssa_value(p_instruction.inputs[1]);
		if (!left.is_constant || !right.is_constant) {
			return false;
		}
		const Variant::Operator op = Variant::Operator(code[p_instruction.ip + 3]);
		bool comparison = false;
		if (p_instruction.opcode == GDScriptFunction::OPCODE_JUMP_COMPARE_INT) {
			const int64_t lhs = int64_t(left.constant_bits);
			const int64_t rhs = int64_t(right.constant_bits);
			switch (op) {
				case Variant::OP_EQUAL: comparison = lhs == rhs; break;
				case Variant::OP_NOT_EQUAL: comparison = lhs != rhs; break;
				case Variant::OP_LESS: comparison = lhs < rhs; break;
				case Variant::OP_LESS_EQUAL: comparison = lhs <= rhs; break;
				case Variant::OP_GREATER: comparison = lhs > rhs; break;
				case Variant::OP_GREATER_EQUAL: comparison = lhs >= rhs; break;
				default: return false;
			}
		} else {
			double lhs;
			double rhs;
			memcpy(&lhs, &left.constant_bits, sizeof(lhs));
			memcpy(&rhs, &right.constant_bits, sizeof(rhs));
			switch (op) {
				case Variant::OP_EQUAL: comparison = lhs == rhs; break;
				case Variant::OP_NOT_EQUAL: comparison = lhs != rhs; break;
				case Variant::OP_LESS: comparison = lhs < rhs; break;
				case Variant::OP_LESS_EQUAL: comparison = lhs <= rhs; break;
				case Variant::OP_GREATER: comparison = lhs > rhs; break;
				case Variant::OP_GREATER_EQUAL: comparison = lhs >= rhs; break;
				default: return false;
			}
		}
		r_taken = comparison == bool(code[p_instruction.ip + 4]);
		return true;
	}

	void emit_ssa_return(const CompactSSAPlan::Instruction &p_instruction) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_instruction.inputs[0]);
		if (typed_entry) {
			emit_ssa_pointer(p_instruction.inputs[0], 0, SLJIT_R0);
			emit_copy_memory(SLJIT_S2, 0, SLJIT_R0, 0, get_ssa_value_size(value));
			sljit_emit_return_void(compiler);
			return;
		}

		if (value.kind == CompactSSAPlan::VALUE_CONSTANT && value.constant_index >= 0) {
			const int address = value.constant_index | (GDScriptFunction::ADDR_TYPE_CONSTANT << GDScriptFunction::ADDR_BITS);
			emit_variant_pointer(address, SLJIT_R0);
			sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
			return;
		}

		const int address = code[p_instruction.ip + 1];
		DEV_ASSERT(((address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS) == GDScriptFunction::ADDR_TYPE_STACK);
		if (value.type == Variant::STRUCT) {
			emit_variant_pointer(address, SLJIT_R0);
			emit_ssa_pointer(p_instruction.inputs[0], 0, SLJIT_R1);
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(value.struct_layout.ptr()));
			sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_store_struct_variant));
			emit_variant_pointer(address, SLJIT_R0);
			sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
			return;
		}
		if (is_unboxed_large_value_type(value.type)) {
			emit_variant_pointer(address, SLJIT_R0);
			emit_ssa_pointer(p_instruction.inputs[0], 0, SLJIT_R1);
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, value.type);
			sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS3V(P, P, W), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_store_large_variant));
			emit_variant_pointer(address, SLJIT_R0);
			sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
			return;
		}
		emit_load_base(address, SLJIT_R2);
		sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R2), get_address_offset(address, variant_type_offset), SLJIT_IMM, value.type);
		emit_ssa_pointer(p_instruction.inputs[0], 0, SLJIT_R1);
		emit_copy_memory(SLJIT_R2, get_address_offset(address, variant_data_offset), SLJIT_R1, 0, get_unboxed_value_size(value.type));
		emit_variant_pointer(address, SLJIT_R0);
		sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
	}

	void *compile_ssa(uint64_t &r_code_size) {
		labels.resize(ssa_plan->get_blocks().size());
		sljit_emit_enter(compiler, 0, typed_entry ? SLJIT_ARGS3V(P, P, P) : SLJIT_ARGS2(P, P, P), 4 | SLJIT_ENTER_FLOAT(2), 3, ssa_plan->get_frame_size());
		emit_ssa_initialize_inputs();

		for (int block_index = 0; block_index < ssa_plan->get_blocks().size(); block_index++) {
			const CompactSSAPlan::Block &block = ssa_plan->get_blocks()[block_index];
			if (!block.reachable) {
				continue;
			}
			labels.write[block_index] = sljit_emit_label(compiler);
			for (int instruction_index : block.instructions) {
				const CompactSSAPlan::Instruction &instruction = ssa_plan->get_instructions()[instruction_index];
				switch (instruction.opcode) {
					case GDScriptFunction::OPCODE_OPERATOR_INT:
					case GDScriptFunction::OPCODE_OPERATOR_FLOAT:
					case GDScriptFunction::OPCODE_OPERATOR_MATH:
						emit_ssa_operator(instruction);
						break;
					case GDScriptFunction::OPCODE_GET_MATH_COMPONENT:
						emit_ssa_math_extract(instruction);
						break;
					case GDScriptFunction::OPCODE_SET_MATH_COMPONENT:
						emit_ssa_math_component_set(instruction);
						break;
					case GDScriptFunction::OPCODE_MATH_LENGTH:
						emit_ssa_math_length(instruction);
						break;
					case GDScriptFunction::OPCODE_CONSTRUCT_STRUCT:
						emit_ssa_struct_construct(instruction);
						break;
					case GDScriptFunction::OPCODE_GET_STRUCT_FIELD:
						emit_ssa_struct_field(instruction, false);
						break;
					case GDScriptFunction::OPCODE_SET_STRUCT_FIELD:
						emit_ssa_struct_field(instruction, true);
						break;
					case GDScriptFunction::OPCODE_EQUAL_STRUCT:
						emit_ssa_struct_equal(instruction);
						break;
					case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
					case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN:
					case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
					case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN:
						emit_ssa_ptrcall(instruction);
						break;
					default:
						break;
				}
			}

			const CompactSSAPlan::Instruction &terminator = ssa_plan->get_instructions()[block.instructions[block.instructions.size() - 1]];
			switch (terminator.opcode) {
				case GDScriptFunction::OPCODE_JUMP_COMPARE_INT:
				case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT:
				case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
				case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL: {
					const int target_ip = (terminator.opcode == GDScriptFunction::OPCODE_JUMP_COMPARE_INT || terminator.opcode == GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT) ? code[terminator.ip + 5] : code[terminator.ip + 2];
					const int target = ssa_plan->get_block_at_ip(target_ip);
					const int fallthrough = ssa_plan->get_block_at_ip(block.end);
					bool taken = false;
					if (get_ssa_constant_branch(terminator, taken)) {
						emit_ssa_jump(block_index, taken ? target : fallthrough);
						break;
					}

					struct sljit_jump *taken_jump = nullptr;
					if (terminator.opcode == GDScriptFunction::OPCODE_JUMP_IF_BOOL || terminator.opcode == GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL) {
						emit_ssa_load_raw(terminator.inputs[0], SLJIT_R0);
						const int condition = terminator.opcode == GDScriptFunction::OPCODE_JUMP_IF_BOOL ? SLJIT_NOT_EQUAL : SLJIT_EQUAL;
						taken_jump = sljit_emit_cmp(compiler, condition, SLJIT_R0, 0, SLJIT_IMM, 0);
					} else if (terminator.opcode == GDScriptFunction::OPCODE_JUMP_COMPARE_INT) {
						emit_ssa_load_raw(terminator.inputs[0], SLJIT_R0);
						emit_ssa_load_raw(terminator.inputs[1], SLJIT_R1);
						int condition = get_int_condition(Variant::Operator(code[terminator.ip + 3]));
						if (!code[terminator.ip + 4]) condition ^= 1;
						taken_jump = sljit_emit_cmp(compiler, condition, SLJIT_R0, 0, SLJIT_R1, 0);
					} else {
						emit_ssa_load_float(terminator.inputs[0], SLJIT_FR0);
						emit_ssa_load_float(terminator.inputs[1], SLJIT_FR1);
						int condition = get_float_condition(Variant::Operator(code[terminator.ip + 3]));
						if (!code[terminator.ip + 4]) condition ^= 1;
						taken_jump = sljit_emit_fcmp(compiler, condition, SLJIT_FR0, 0, SLJIT_FR1, 0);
					}
					emit_ssa_jump(block_index, fallthrough);
					sljit_set_label(taken_jump, sljit_emit_label(compiler));
					emit_ssa_jump(block_index, target);
				} break;
				case GDScriptFunction::OPCODE_JUMP:
					emit_ssa_jump(block_index, ssa_plan->get_block_at_ip(code[terminator.ip + 1]));
					break;
				case GDScriptFunction::OPCODE_RETURN:
				case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT:
					emit_ssa_return(terminator);
					break;
				case GDScriptFunction::OPCODE_END:
					if (typed_entry) {
						sljit_emit_return_void(compiler);
					} else {
						emit_variant_pointer(GDScriptFunction::ADDR_NIL, SLJIT_R0);
						sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
					}
					break;
				default:
					if (block.end < code_size) {
						emit_ssa_jump(block_index, ssa_plan->get_block_at_ip(block.end));
					}
					break;
			}
		}

		if (typed_entry) {
			sljit_emit_return_void(compiler);
		} else {
			emit_variant_pointer(GDScriptFunction::ADDR_NIL, SLJIT_R0);
			sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
		}
		for (const PendingJump &pending : pending_jumps) {
			DEV_ASSERT(labels[pending.target] != nullptr);
			sljit_set_label(pending.jump, labels[pending.target]);
		}

		void *generated_code = sljit_generate_code(compiler, 0, nullptr);
		if (generated_code) {
			r_code_size = sljit_get_generated_code_size(compiler);
		}
		sljit_free_compiler(compiler);
		compiler = nullptr;
		return generated_code;
	}

public:
	BaselineCompiler(const int *p_code, int p_code_size, int p_stack_size, const Variant *p_constants, int p_constant_count, MethodBind *const *p_methods, int p_method_count, const Vector<Variant::Type> &p_argument_types, const Vector<Ref<StructLayout>> &p_argument_struct_layouts, Variant::Type p_return_type, const Ref<StructLayout> &p_return_struct_layout, bool p_typed_entry, bool p_optimizing = false) :
			code(p_code), code_size(p_code_size), stack_size(p_stack_size), constant_count(p_constant_count), constants(p_constants), method_count(p_method_count), methods(p_methods), argument_types(p_argument_types), argument_struct_layouts(p_argument_struct_layouts), return_type(p_return_type), return_struct_layout(p_return_struct_layout), typed_entry(p_typed_entry), optimizing(p_optimizing) {
		Variant probe;
		variant_type_offset = reinterpret_cast<uint8_t *>(VariantInternal::get_type_ptr(&probe)) - reinterpret_cast<uint8_t *>(&probe);
		variant_data_offset = reinterpret_cast<uint8_t *>(VariantInternal::get_int(&probe)) - reinterpret_cast<uint8_t *>(&probe);
	}

	int get_ptrcall_count() const { return ptrcall_count; }
	bool get_requires_self() const { return requires_self; }
	int get_ssa_node_count() const { return ssa_node_count; }
	int get_eliminated_node_count() const { return eliminated_node_count; }
	int get_scalar_replaced_math_value_count() const { return scalar_replaced_math_value_count; }

	void *compile(uint64_t &r_code_size) {
		if (!validate()) {
			return nullptr;
		}
		if (optimizing) {
			ssa_plan = memnew(CompactSSAPlan(code, code_size, stack_size, constants, constant_count, stack_slot_types, stack_slot_struct_layouts, max_ptrcall_argument_count));
			if (!ssa_plan->build()) {
				memdelete(ssa_plan);
				ssa_plan = nullptr;
				return nullptr;
			}
			ssa_node_count = ssa_plan->get_node_count();
			eliminated_node_count = ssa_plan->get_eliminated_node_count();
			scalar_replaced_math_value_count = ssa_plan->get_scalar_replaced_math_value_count();
		}

		compiler = sljit_create_compiler(nullptr);
		if (!compiler) {
			if (ssa_plan != nullptr) {
				memdelete(ssa_plan);
				ssa_plan = nullptr;
			}
			return nullptr;
		}
		if (optimizing) {
			void *generated_code = compile_ssa(r_code_size);
			memdelete(ssa_plan);
			ssa_plan = nullptr;
			return generated_code;
		}

		labels.resize(code_size + 1);
		sljit_emit_enter(compiler, 0, typed_entry ? SLJIT_ARGS3V(P, P, P) : SLJIT_ARGS2(P, P, P), 4 | SLJIT_ENTER_FLOAT(2), 3, native_frame_size);
		emit_initialize_unboxed_slots();

		int ip = 0;
		while (ip < code_size) {
			labels.write[ip] = sljit_emit_label(compiler);
			GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(code[ip]);
			switch (opcode) {
				case GDScriptFunction::OPCODE_ASSIGN_NULL:
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_ASSIGN_BOOL:
					emit_load_bool(code[ip + 2], SLJIT_R0);
					emit_store_bool(code[ip + 1], SLJIT_R0);
					ip += 3;
					break;
				case GDScriptFunction::OPCODE_ASSIGN_INT:
					emit_load_int(code[ip + 2], SLJIT_R0);
					emit_store_int(code[ip + 1], SLJIT_R0);
					ip += 3;
					break;
				case GDScriptFunction::OPCODE_ASSIGN_FLOAT:
					emit_load_float(code[ip + 2], SLJIT_FR0);
					emit_store_float(code[ip + 1], SLJIT_FR0);
					ip += 3;
					break;
				case GDScriptFunction::OPCODE_ASSIGN_MATH:
					emit_copy_unboxed_value(code[ip + 1], code[ip + 2], Variant::Type(code[ip + 3]));
					ip += 4;
					break;
				case GDScriptFunction::OPCODE_CONSTRUCT_STRUCT:
					emit_construct_struct(ip);
					ip += code[ip + 1] + 3;
					break;
				case GDScriptFunction::OPCODE_ASSIGN_STRUCT: {
					const Ref<StructLayout> layout = get_address_struct_layout(code[ip + 1]);
					emit_copy_unboxed_value(code[ip + 1], code[ip + 2], Variant::STRUCT, layout);
					ip += 3;
				} break;
				case GDScriptFunction::OPCODE_GET_STRUCT_FIELD:
					emit_struct_field(ip, false);
					ip += 4;
					break;
				case GDScriptFunction::OPCODE_SET_STRUCT_FIELD:
					emit_struct_field(ip, true);
					ip += 4;
					break;
				case GDScriptFunction::OPCODE_EQUAL_STRUCT:
					emit_equal_struct(ip);
					ip += 4;
					break;
				case GDScriptFunction::OPCODE_BOX_STRUCT:
					emit_box_struct(ip);
					ip += 3;
					break;
				case GDScriptFunction::OPCODE_OPERATOR_INT:
					emit_int_operator(ip);
					ip += 5;
					break;
				case GDScriptFunction::OPCODE_OPERATOR_FLOAT:
					emit_float_operator(ip);
					ip += 5;
					break;
				case GDScriptFunction::OPCODE_OPERATOR_MATH:
					emit_math_operator(ip);
					ip += 6;
					break;
				case GDScriptFunction::OPCODE_GET_MATH_COMPONENT:
					emit_math_component_get(ip);
					ip += 4;
					break;
				case GDScriptFunction::OPCODE_SET_MATH_COMPONENT:
					emit_math_component_set(ip);
					ip += 4;
					break;
				case GDScriptFunction::OPCODE_MATH_LENGTH:
					emit_math_length(ip);
					ip += 4;
					break;
				case GDScriptFunction::OPCODE_JUMP_COMPARE_INT: {
					emit_load_int(code[ip + 1], SLJIT_R0);
					emit_load_int(code[ip + 2], SLJIT_R1);
					int condition = get_int_condition(Variant::Operator(code[ip + 3]));
					if (!code[ip + 4]) {
						condition ^= 1;
					}
					add_pending_jump(sljit_emit_cmp(compiler, condition, SLJIT_R0, 0, SLJIT_R1, 0), code[ip + 5]);
					ip += 6;
				} break;
				case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT: {
					emit_load_float(code[ip + 1], SLJIT_FR0);
					emit_load_float(code[ip + 2], SLJIT_FR1);
					int condition = get_float_condition(Variant::Operator(code[ip + 3]));
					if (!code[ip + 4]) {
						condition ^= 1;
					}
					add_pending_jump(sljit_emit_fcmp(compiler, condition, SLJIT_FR0, 0, SLJIT_FR1, 0), code[ip + 5]);
					ip += 6;
				} break;
				case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
				case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL: {
					emit_load_bool(code[ip + 1], SLJIT_R0);
					int condition = opcode == GDScriptFunction::OPCODE_JUMP_IF_BOOL ? SLJIT_NOT_EQUAL : SLJIT_EQUAL;
					add_pending_jump(sljit_emit_cmp(compiler, condition, SLJIT_R0, 0, SLJIT_IMM, 0), code[ip + 2]);
					ip += 3;
				} break;
				case GDScriptFunction::OPCODE_JUMP:
					add_pending_jump(sljit_emit_jump(compiler, SLJIT_JUMP), code[ip + 1]);
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN:
					emit_ptrcall(ip, opcode);
					ip += 4 + code[ip + 1];
					break;
				case GDScriptFunction::OPCODE_RETURN:
					emit_return_value(code[ip + 1]);
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT:
					emit_return_value(code[ip + 1]);
					ip += 3;
					break;
				case GDScriptFunction::OPCODE_LINE:
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_END:
					if (typed_entry) {
						sljit_emit_return_void(compiler);
					} else {
						emit_return_value(GDScriptFunction::ADDR_NIL);
					}
					ip += 1;
					break;
				default:
					break;
			}
		}

		labels.write[code_size] = sljit_emit_label(compiler);
		if (typed_entry) {
			sljit_emit_return_void(compiler);
		} else {
			emit_return_value(GDScriptFunction::ADDR_NIL);
		}

		for (const PendingJump &pending : pending_jumps) {
			sljit_set_label(pending.jump, labels[pending.target]);
		}

		void *generated_code = sljit_generate_code(compiler, 0, nullptr);
		if (generated_code) {
			r_code_size = sljit_get_generated_code_size(compiler);
		}
		sljit_free_compiler(compiler);
		compiler = nullptr;
		return generated_code;
	}
};
