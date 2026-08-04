/**************************************************************************/
/*  gdscript_baseline_jit.cpp                                             */
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
/* distribute, sublicense, and/or sell copies of the Software, and to      */
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

#include "gdscript_baseline_jit.h"

#ifdef GDSCRIPT_BASELINE_JIT_ENABLED

#include "gdscript_function.h"

#include "core/variant/variant_internal.h"

#define SLJIT_CONFIG_AUTO 1
#define SLJIT_VERBOSE 0
#define SLJIT_DEBUG 0

#if !defined(__APPLE__)
#define SLJIT_WX_EXECUTABLE_ALLOCATOR 1
#endif

#include "sljitLir.h"

namespace {

static_assert(sizeof(sljit_sw) == sizeof(int64_t), "The GDScript baseline JIT requires 64-bit integer registers.");
static_assert(sizeof(Variant::Type) == sizeof(sljit_s32), "The GDScript baseline JIT requires a 32-bit Variant type tag.");

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
	Vector<Variant::Type> argument_types;
	Variant::Type return_type = Variant::NIL;
	struct sljit_compiler *compiler = nullptr;
	Vector<struct sljit_label *> labels;
	Vector<PendingJump> pending_jumps;
	Vector<Variant::Type> stack_slot_types;
	Vector<sljit_sw> stack_slot_offsets;
	bool typed_entry = false;
	sljit_sw variant_type_offset = 0;
	sljit_sw variant_data_offset = 0;
	sljit_s32 native_frame_size = 0;

	static bool is_unboxed_type(Variant::Type p_type) {
		return p_type == Variant::BOOL || p_type == Variant::INT || p_type == Variant::FLOAT;
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

	bool mark_address_type(int p_address, Variant::Type p_type) {
		if (!is_unboxed_type(p_type) || !is_valid_address(p_address)) {
			return false;
		}

		int address_type = (p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		int address_index = p_address & GDScriptFunction::ADDR_MASK;
		if (address_type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
			return constants != nullptr && constants[address_index].get_type() == p_type;
		}

		Variant::Type &slot_type = stack_slot_types.write[address_index];
		if (slot_type != Variant::NIL && slot_type != p_type) {
			return false;
		}
		slot_type = p_type;
		return true;
	}

	bool validate() {
		if (!code || code_size <= 0) {
			return false;
		}
		stack_slot_types.resize(stack_size);
		stack_slot_types.fill(Variant::NIL);

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
				case GDScriptFunction::OPCODE_RETURN:
					if (ip + 2 > code_size || !is_valid_address(code[ip + 1])) {
						return false;
					}
					if (typed_entry && !mark_address_type(code[ip + 1], return_type)) {
						return false;
					}
					ip += 2;
					break;
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

		if (typed_entry) {
			for (int i = 0; i < argument_types.size(); i++) {
				int stack_index = GDScriptFunction::FIXED_ADDRESSES_MAX + i;
				if (stack_index >= stack_size || !mark_address_type(stack_index, argument_types[i])) {
					return false;
				}
			}
		}

		stack_slot_offsets.resize(stack_size);
		stack_slot_offsets.fill(-1);
		int native_slot_count = 0;
		for (int i = 0; i < stack_slot_types.size(); i++) {
			if (stack_slot_types[i] != Variant::NIL) {
				stack_slot_offsets.write[i] = native_slot_count++ * sizeof(uint64_t);
			}
		}
		if (native_slot_count > SLJIT_MAX_LOCAL_SIZE / int(sizeof(uint64_t))) {
			return false;
		}
		native_frame_size = native_slot_count * sizeof(uint64_t);
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

	void emit_initialize_unboxed_slots() {
		for (int i = 0; i < stack_slot_types.size(); i++) {
			Variant::Type type = stack_slot_types[i];
			if (type == Variant::NIL) {
				continue;
			}

			sljit_sw native_offset = stack_slot_offsets[i];
			int argument_index = i - GDScriptFunction::FIXED_ADDRESSES_MAX;
			if (argument_index >= 0 && argument_index < argument_types.size()) {
				if (typed_entry) {
					sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), argument_index * sizeof(uint64_t));
					sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), native_offset, SLJIT_R0, 0);
				} else {
					emit_load_base(i, SLJIT_R0);
					if (type == Variant::BOOL) {
						sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), get_address_offset(i, variant_data_offset));
					} else {
						sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), get_address_offset(i, variant_data_offset));
					}
					sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), native_offset, SLJIT_R1, 0);
				}
				continue;
			}

			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), native_offset, SLJIT_IMM, 0);
		}
	}

	void emit_spill_unboxed_slots() {
		DEV_ASSERT(!typed_entry);
		for (int i = 0; i < stack_slot_types.size(); i++) {
			Variant::Type type = stack_slot_types[i];
			if (type == Variant::NIL) {
				continue;
			}

			emit_store_type(i, type);
			if (type == Variant::BOOL) {
				sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[i]);
				sljit_emit_op1(compiler, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_R2), get_address_offset(i, variant_data_offset), SLJIT_R1, 0);
			} else {
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[i]);
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_R2), get_address_offset(i, variant_data_offset), SLJIT_R1, 0);
			}
		}
	}

	void emit_return_value(int p_address) {
		if (typed_entry) {
			if (return_type == Variant::BOOL) {
				emit_load_bool(p_address, SLJIT_R0);
			} else {
				// Integer values and IEEE-754 doubles both occupy one raw slot.
				emit_load_int(p_address, SLJIT_R0);
			}
			sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
			return;
		}

		emit_spill_unboxed_slots();
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

public:
	BaselineCompiler(const int *p_code, int p_code_size, int p_stack_size, const Variant *p_constants, int p_constant_count, const Vector<Variant::Type> &p_argument_types, Variant::Type p_return_type, bool p_typed_entry) :
			code(p_code), code_size(p_code_size), stack_size(p_stack_size), constant_count(p_constant_count), constants(p_constants), argument_types(p_argument_types), return_type(p_return_type), typed_entry(p_typed_entry) {
		Variant probe;
		variant_type_offset = reinterpret_cast<uint8_t *>(VariantInternal::get_type_ptr(&probe)) - reinterpret_cast<uint8_t *>(&probe);
		variant_data_offset = reinterpret_cast<uint8_t *>(VariantInternal::get_int(&probe)) - reinterpret_cast<uint8_t *>(&probe);
	}

	void *compile(uint64_t &r_code_size) {
		if (!validate()) {
			return nullptr;
		}

		compiler = sljit_create_compiler(nullptr);
		if (!compiler) {
			return nullptr;
		}

		labels.resize(code_size + 1);
		sljit_emit_enter(compiler, 0, typed_entry ? SLJIT_ARGS1(W, P) : SLJIT_ARGS1(P, P), 3 | SLJIT_ENTER_FLOAT(2), 1, native_frame_size);
		emit_initialize_unboxed_slots();

		int ip = 0;
		while (ip < code_size) {
			labels.write[ip] = sljit_emit_label(compiler);
			GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(code[ip]);
			switch (opcode) {
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
				case GDScriptFunction::OPCODE_OPERATOR_INT:
					emit_int_operator(ip);
					ip += 5;
					break;
				case GDScriptFunction::OPCODE_OPERATOR_FLOAT:
					emit_float_operator(ip);
					ip += 5;
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
				case GDScriptFunction::OPCODE_RETURN:
					emit_return_value(code[ip + 1]);
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_LINE:
					ip += 2;
					break;
				case GDScriptFunction::OPCODE_END:
					if (typed_entry) {
						sljit_emit_return(compiler, SLJIT_MOV, SLJIT_IMM, 0);
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
			sljit_emit_return(compiler, SLJIT_MOV, SLJIT_IMM, 0);
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

} // namespace

GDScriptBaselineJIT::GDScriptBaselineJIT(void *p_entry_point, void *p_typed_entry_point, uint64_t p_code_size, const Vector<Variant::Type> &p_typed_argument_types, Variant::Type p_typed_return_type) :
		entry_point(p_entry_point), typed_entry_point(p_typed_entry_point), code_size(p_code_size), typed_argument_types(p_typed_argument_types), typed_return_type(p_typed_return_type) {
}

GDScriptBaselineJIT *GDScriptBaselineJIT::compile(const GDScriptFunction *p_function) {
	ERR_FAIL_NULL_V(p_function, nullptr);

	auto is_typed_abi_type = [](Variant::Type p_type) {
		return p_type == Variant::BOOL || p_type == Variant::INT || p_type == Variant::FLOAT;
	};
	Vector<Variant::Type> typed_argument_types;
	bool has_typed_signature = !p_function->is_vararg() && p_function->return_type.kind == GDScriptDataType::BUILTIN && is_typed_abi_type(p_function->return_type.builtin_type);
	for (const GDScriptDataType &argument_type : p_function->argument_types) {
		Variant::Type builtin_type = argument_type.kind == GDScriptDataType::BUILTIN && is_typed_abi_type(argument_type.builtin_type) ? argument_type.builtin_type : Variant::NIL;
		typed_argument_types.push_back(builtin_type);
		has_typed_signature = has_typed_signature && builtin_type != Variant::NIL;
	}

	BaselineCompiler compiler(p_function->_code_ptr, p_function->_code_size, p_function->_stack_size, p_function->_constants_ptr, p_function->_constant_count, typed_argument_types, Variant::NIL, false);
	uint64_t generated_size = 0;
	void *generated_code = compiler.compile(generated_size);
	if (!generated_code) {
		return nullptr;
	}

	void *typed_generated_code = nullptr;
	if (has_typed_signature) {
		BaselineCompiler typed_compiler(p_function->_code_ptr, p_function->_code_size, p_function->_stack_size, p_function->_constants_ptr, p_function->_constant_count, typed_argument_types, p_function->return_type.builtin_type, true);
		uint64_t typed_generated_size = 0;
		typed_generated_code = typed_compiler.compile(typed_generated_size);
		generated_size += typed_generated_size;
	}

	return memnew(GDScriptBaselineJIT(generated_code, typed_generated_code, generated_size, typed_argument_types, has_typed_signature ? p_function->return_type.builtin_type : Variant::NIL));
}

Variant *GDScriptBaselineJIT::execute(Variant **p_variant_addresses) const {
	typedef Variant *(SLJIT_FUNC *EntryPoint)(Variant **);
	return reinterpret_cast<EntryPoint>(entry_point)(p_variant_addresses);
}

bool GDScriptBaselineJIT::execute_typed(const Variant **p_arguments, int p_argument_count, Variant &r_return) const {
	if (typed_entry_point == nullptr || p_argument_count != typed_argument_types.size()) {
		return false;
	}

	uint64_t *raw_arguments = reinterpret_cast<uint64_t *>(alloca(MAX(1, p_argument_count) * sizeof(uint64_t)));
	for (int i = 0; i < p_argument_count; i++) {
		if (p_arguments[i]->get_type() != typed_argument_types[i]) {
			return false;
		}
		switch (typed_argument_types[i]) {
			case Variant::BOOL:
				raw_arguments[i] = *VariantInternal::get_bool(p_arguments[i]);
				break;
			case Variant::INT:
				raw_arguments[i] = uint64_t(*VariantInternal::get_int(p_arguments[i]));
				break;
			case Variant::FLOAT:
				memcpy(&raw_arguments[i], VariantInternal::get_float(p_arguments[i]), sizeof(uint64_t));
				break;
			default:
				return false;
		}
	}

	typedef uint64_t(SLJIT_FUNC * TypedEntryPoint)(const uint64_t *);
	const uint64_t result = reinterpret_cast<TypedEntryPoint>(typed_entry_point)(raw_arguments);
	switch (typed_return_type) {
		case Variant::BOOL:
			r_return = bool(result);
			break;
		case Variant::INT:
			r_return = int64_t(result);
			break;
		case Variant::FLOAT: {
			double value;
			memcpy(&value, &result, sizeof(value));
			r_return = value;
		} break;
		default:
			return false;
	}
	return true;
}

GDScriptBaselineJIT::~GDScriptBaselineJIT() {
	if (entry_point) {
		sljit_free_code(entry_point, nullptr);
	}
	if (typed_entry_point) {
		sljit_free_code(typed_entry_point, nullptr);
	}
}

#endif // GDSCRIPT_BASELINE_JIT_ENABLED
