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

#include "core/object/method_bind.h"
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
static_assert(sizeof(sljit_sw) == sizeof(void *), "The GDScript baseline JIT requires 64-bit pointers.");
static_assert(sizeof(Variant::Type) == sizeof(sljit_s32), "The GDScript baseline JIT requires a 32-bit Variant type tag.");

static void SLJIT_FUNC invoke_ptrcall(MethodBind *p_method, Object *p_instance, const void **p_arguments, void *r_return) {
	p_method->ptrcall(p_instance, p_arguments, r_return);
}

class CompactSSAPlan {
public:
	enum ValueKind {
		VALUE_INPUT,
		VALUE_CONSTANT,
		VALUE_PHI,
		VALUE_INT_OPERATOR,
		VALUE_FLOAT_OPERATOR,
		VALUE_PTRCALL_RESULT,
	};

	struct Value {
		ValueKind kind = VALUE_INPUT;
		Variant::Type type = Variant::NIL;
		Variant::Operator op = Variant::OP_MAX;
		int block = -1;
		int ip = -1;
		int constant_index = -1;
		int replacement = -1;
		int frame_offset = -1;
		Vector<int> inputs;
		uint64_t constant_bits = 0;
		bool is_constant = false;
		bool live = false;
	};

	struct Instruction {
		GDScriptFunction::Opcode opcode = GDScriptFunction::OPCODE_END;
		int ip = 0;
		int output = -1;
		Vector<int> inputs;
	};

	struct Block {
		int start = 0;
		int end = 0;
		Vector<int> predecessors;
		Vector<int> successors;
		Vector<int> instructions;
		Vector<int> in_values;
		Vector<int> out_values;
		Vector<int> phis;
		bool reachable = false;
	};

private:
	const int *code = nullptr;
	int code_size = 0;
	int stack_size = 0;
	const Variant *constants = nullptr;
	int constant_count = 0;
	const Vector<Variant::Type> &stack_types;
	int max_ptrcall_argument_count = 0;
	Vector<Value> values;
	Vector<Instruction> instructions;
	Vector<Block> blocks;
	Vector<int> ip_to_instruction;
	Vector<int> ip_to_block;
	Vector<int> initial_values;
	Vector<int> constant_values;
	int frame_size = 0;
	int phi_scratch_offset = 0;
	int max_phi_copies = 0;
	int ptrcall_values_offset = 0;
	int ptrcall_arguments_offset = 0;
	int ptrcall_return_offset = 0;
	int eliminated_node_count = 0;

	static bool is_unary(Variant::Operator p_operator) {
		return p_operator == Variant::OP_NEGATE || p_operator == Variant::OP_POSITIVE || p_operator == Variant::OP_BIT_NEGATE;
	}

	int instruction_length(int p_ip) const {
		switch (GDScriptFunction::Opcode(code[p_ip])) {
			case GDScriptFunction::OPCODE_ASSIGN_NULL:
			case GDScriptFunction::OPCODE_RETURN:
			case GDScriptFunction::OPCODE_LINE:
			case GDScriptFunction::OPCODE_JUMP:
				return 2;
			case GDScriptFunction::OPCODE_ASSIGN_BOOL:
			case GDScriptFunction::OPCODE_ASSIGN_INT:
			case GDScriptFunction::OPCODE_ASSIGN_FLOAT:
			case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
			case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL:
				return 3;
			case GDScriptFunction::OPCODE_OPERATOR_INT:
			case GDScriptFunction::OPCODE_OPERATOR_FLOAT:
				return 5;
			case GDScriptFunction::OPCODE_JUMP_COMPARE_INT:
			case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT:
				return 6;
			case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
			case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN:
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
			case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN:
				return 4 + code[p_ip + 1];
			case GDScriptFunction::OPCODE_END:
				return 1;
			default:
				return 0;
		}
	}

	int add_value(ValueKind p_kind, Variant::Type p_type, int p_block = -1, int p_ip = -1) {
		Value value;
		value.kind = p_kind;
		value.type = p_type;
		value.block = p_block;
		value.ip = p_ip;
		values.push_back(value);
		return values.size() - 1;
	}

	int resolve(int p_value) const {
		while (p_value >= 0 && values[p_value].replacement >= 0) {
			p_value = values[p_value].replacement;
		}
		return p_value;
	}

	int get_constant_value(int p_index) {
		if (constant_values[p_index] >= 0) {
			return constant_values[p_index];
		}
		const Variant::Type type = constants[p_index].get_type();
		const int value_index = add_value(VALUE_CONSTANT, type);
		Value &value = values.write[value_index];
		value.constant_index = p_index;
		value.is_constant = true;
		switch (type) {
			case Variant::BOOL:
				value.constant_bits = *VariantInternal::get_bool(&constants[p_index]);
				break;
			case Variant::INT:
				value.constant_bits = uint64_t(*VariantInternal::get_int(&constants[p_index]));
				break;
			case Variant::FLOAT:
				memcpy(&value.constant_bits, VariantInternal::get_float(&constants[p_index]), sizeof(uint64_t));
				break;
			default:
				break;
		}
		constant_values.write[p_index] = value_index;
		return value_index;
	}

	int address_value(int p_address, const Vector<int> &p_environment) {
		const int type = (p_address & GDScriptFunction::ADDR_TYPE_MASK) >> GDScriptFunction::ADDR_BITS;
		const int index = p_address & GDScriptFunction::ADDR_MASK;
		if (type == GDScriptFunction::ADDR_TYPE_CONSTANT) {
			return get_constant_value(index);
		}
		DEV_ASSERT(type == GDScriptFunction::ADDR_TYPE_STACK);
		return p_environment[index];
	}

	bool set_inputs(int p_value, const Vector<int> &p_inputs) {
		if (values[p_value].inputs == p_inputs) {
			return false;
		}
		values.write[p_value].inputs = p_inputs;
		return true;
	}

	bool simulate_block(int p_block) {
		Block &block = blocks.write[p_block];
		Vector<int> environment = block.in_values;
		bool changed = false;
		for (int instruction_index : block.instructions) {
			Instruction &instruction = instructions.write[instruction_index];
			const int ip = instruction.ip;
			Vector<int> inputs;
			switch (instruction.opcode) {
				case GDScriptFunction::OPCODE_ASSIGN_NULL: {
					const int destination = code[ip + 1] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = initial_values[destination];
				} break;
				case GDScriptFunction::OPCODE_ASSIGN_BOOL:
				case GDScriptFunction::OPCODE_ASSIGN_INT:
				case GDScriptFunction::OPCODE_ASSIGN_FLOAT: {
					inputs.push_back(address_value(code[ip + 2], environment));
					const int destination = code[ip + 1] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = inputs[0];
				} break;
				case GDScriptFunction::OPCODE_OPERATOR_INT:
				case GDScriptFunction::OPCODE_OPERATOR_FLOAT: {
					inputs.push_back(address_value(code[ip + 1], environment));
					const Variant::Operator op = Variant::Operator(code[ip + 4]);
					if (!is_unary(op)) {
						inputs.push_back(address_value(code[ip + 2], environment));
					}
					changed = set_inputs(instruction.output, inputs) || changed;
					const int destination = code[ip + 3] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = instruction.output;
				} break;
				case GDScriptFunction::OPCODE_JUMP_COMPARE_INT:
				case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT:
					inputs.push_back(address_value(code[ip + 1], environment));
					inputs.push_back(address_value(code[ip + 2], environment));
					break;
				case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
				case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL:
				case GDScriptFunction::OPCODE_RETURN:
					inputs.push_back(address_value(code[ip + 1], environment));
					break;
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN: {
					const bool has_return = instruction.opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || instruction.opcode == GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN;
					const int instruction_argument_count = code[ip + 1];
					const int argument_count = code[ip + 2 + instruction_argument_count];
					for (int i = 0; i < argument_count; i++) {
						inputs.push_back(address_value(code[ip + 2 + i], environment));
					}
					if (has_return) {
						changed = set_inputs(instruction.output, inputs) || changed;
						const int destination = code[ip + 1 + instruction_argument_count] & GDScriptFunction::ADDR_MASK;
						environment.write[destination] = instruction.output;
					}
				} break;
				default:
					break;
			}
			if (instruction.inputs != inputs) {
				instruction.inputs = inputs;
				changed = true;
			}
		}
		if (block.out_values != environment) {
			block.out_values = environment;
			changed = true;
		}
		return changed;
	}

	bool merge_block(int p_block) {
		Block &block = blocks.write[p_block];
		if (p_block == 0) {
			if (!block.reachable || block.in_values != initial_values) {
				block.reachable = true;
				block.in_values = initial_values;
				return true;
			}
			return false;
		}

		Vector<int> reachable_predecessors;
		for (int predecessor : block.predecessors) {
			if (blocks[predecessor].reachable && !blocks[predecessor].out_values.is_empty()) {
				reachable_predecessors.push_back(predecessor);
			}
		}
		if (reachable_predecessors.is_empty()) {
			return false;
		}

		Vector<int> merged;
		merged.resize(stack_size);
		merged.fill(-1);
		for (int slot = 0; slot < stack_size; slot++) {
			int incoming = blocks[reachable_predecessors[0]].out_values[slot];
			bool differs = false;
			for (int i = 1; i < reachable_predecessors.size(); i++) {
				if (blocks[reachable_predecessors[i]].out_values[slot] != incoming) {
					differs = true;
					break;
				}
			}
			if ((differs || block.phis[slot] >= 0) && incoming >= 0) {
				if (block.phis[slot] < 0) {
					block.phis.write[slot] = add_value(VALUE_PHI, stack_types[slot], p_block);
				}
				incoming = block.phis[slot];
			}
			merged.write[slot] = incoming;
		}

		const bool changed = !block.reachable || block.in_values != merged;
		block.reachable = true;
		block.in_values = merged;
		return changed;
	}

	void finish_phi_inputs() {
		for (int block_index = 0; block_index < blocks.size(); block_index++) {
			Block &block = blocks.write[block_index];
			if (!block.reachable) {
				continue;
			}
			for (int slot = 0; slot < stack_size; slot++) {
				const int phi = block.phis[slot];
				if (phi < 0) {
					continue;
				}
				Vector<int> phi_inputs;
				for (int predecessor : block.predecessors) {
					if (blocks[predecessor].reachable) {
						phi_inputs.push_back(blocks[predecessor].out_values[slot]);
					}
				}
				values.write[phi].inputs = phi_inputs;
			}
		}
	}

	static bool constant_equal(const Value &p_left, const Value &p_right) {
		return p_left.is_constant && p_right.is_constant && p_left.type == p_right.type && p_left.constant_bits == p_right.constant_bits;
	}

	bool fold_operator(Value &r_value) {
		const int left_index = resolve(r_value.inputs[0]);
		const Value &left = values[left_index];
		if (!left.is_constant) {
			return false;
		}
		const bool unary = is_unary(r_value.op);
		const int right_index = unary ? -1 : resolve(r_value.inputs[1]);
		if (!unary && !values[right_index].is_constant) {
			return false;
		}

		if (r_value.kind == VALUE_INT_OPERATOR) {
			const uint64_t lhs = left.constant_bits;
			const uint64_t rhs = unary ? 0 : values[right_index].constant_bits;
			switch (r_value.op) {
				case Variant::OP_ADD: r_value.constant_bits = lhs + rhs; break;
				case Variant::OP_SUBTRACT: r_value.constant_bits = lhs - rhs; break;
				case Variant::OP_MULTIPLY: r_value.constant_bits = lhs * rhs; break;
				case Variant::OP_NEGATE: r_value.constant_bits = uint64_t(0) - lhs; break;
				case Variant::OP_POSITIVE: r_value.constant_bits = lhs; break;
				case Variant::OP_BIT_AND: r_value.constant_bits = lhs & rhs; break;
				case Variant::OP_BIT_OR: r_value.constant_bits = lhs | rhs; break;
				case Variant::OP_BIT_XOR: r_value.constant_bits = lhs ^ rhs; break;
				case Variant::OP_BIT_NEGATE: r_value.constant_bits = ~lhs; break;
				case Variant::OP_EQUAL: r_value.constant_bits = int64_t(lhs) == int64_t(rhs); break;
				case Variant::OP_NOT_EQUAL: r_value.constant_bits = int64_t(lhs) != int64_t(rhs); break;
				case Variant::OP_LESS: r_value.constant_bits = int64_t(lhs) < int64_t(rhs); break;
				case Variant::OP_LESS_EQUAL: r_value.constant_bits = int64_t(lhs) <= int64_t(rhs); break;
				case Variant::OP_GREATER: r_value.constant_bits = int64_t(lhs) > int64_t(rhs); break;
				case Variant::OP_GREATER_EQUAL: r_value.constant_bits = int64_t(lhs) >= int64_t(rhs); break;
				default: return false;
			}
		} else {
			double lhs;
			double rhs = 0.0;
			memcpy(&lhs, &left.constant_bits, sizeof(lhs));
			if (!unary) {
				memcpy(&rhs, &values[right_index].constant_bits, sizeof(rhs));
			}
			double result = 0.0;
			switch (r_value.op) {
				case Variant::OP_ADD: result = lhs + rhs; break;
				case Variant::OP_SUBTRACT: result = lhs - rhs; break;
				case Variant::OP_MULTIPLY: result = lhs * rhs; break;
				case Variant::OP_DIVIDE: result = lhs / rhs; break;
				case Variant::OP_NEGATE: result = -lhs; break;
				case Variant::OP_POSITIVE: result = lhs; break;
				case Variant::OP_EQUAL: r_value.constant_bits = lhs == rhs; break;
				case Variant::OP_NOT_EQUAL: r_value.constant_bits = lhs != rhs; break;
				case Variant::OP_LESS: r_value.constant_bits = lhs < rhs; break;
				case Variant::OP_LESS_EQUAL: r_value.constant_bits = lhs <= rhs; break;
				case Variant::OP_GREATER: r_value.constant_bits = lhs > rhs; break;
				case Variant::OP_GREATER_EQUAL: r_value.constant_bits = lhs >= rhs; break;
				default: return false;
			}
			if (r_value.type == Variant::FLOAT) {
				memcpy(&r_value.constant_bits, &result, sizeof(result));
			}
		}
		r_value.is_constant = true;
		return true;
	}

	void optimize_values() {
		// Local value numbering removes repeated pure expressions without
		// requiring global alias or invalidation guards.
		for (const Block &block : blocks) {
			if (!block.reachable) {
				continue;
			}
			Vector<int> expressions;
			for (int instruction_index : block.instructions) {
				const Instruction &instruction = instructions[instruction_index];
				if (instruction.output < 0) {
					continue;
				}
				Value &value = values.write[instruction.output];
				if (value.kind != VALUE_INT_OPERATOR && value.kind != VALUE_FLOAT_OPERATOR) {
					continue;
				}
				for (int candidate_index : expressions) {
					const Value &candidate = values[resolve(candidate_index)];
					if (candidate.kind != value.kind || candidate.op != value.op || candidate.inputs.size() != value.inputs.size()) {
						continue;
					}
					bool same = true;
					for (int i = 0; i < value.inputs.size(); i++) {
						if (resolve(candidate.inputs[i]) != resolve(value.inputs[i])) {
							same = false;
							break;
						}
					}
					if (same) {
						value.replacement = resolve(candidate_index);
						eliminated_node_count++;
						break;
					}
				}
				if (value.replacement < 0) {
					expressions.push_back(instruction.output);
				}
			}
		}

		bool changed = true;
		while (changed) {
			changed = false;
			for (int i = 0; i < values.size(); i++) {
				Value &value = values.write[i];
				if (value.replacement >= 0 || value.is_constant) {
					continue;
				}
				if ((value.kind == VALUE_INT_OPERATOR || value.kind == VALUE_FLOAT_OPERATOR) && !value.inputs.is_empty()) {
					if (fold_operator(value)) {
						eliminated_node_count++;
						changed = true;
					}
				} else if (value.kind == VALUE_PHI && !value.inputs.is_empty()) {
					int first = -1;
					bool same = true;
					for (int input : value.inputs) {
						input = resolve(input);
						if (input == i) {
							continue;
						}
						if (first < 0) {
							first = input;
						} else if (!constant_equal(values[first], values[input])) {
							same = false;
							break;
						}
					}
					if (same && first >= 0 && values[first].is_constant) {
						value.is_constant = true;
						value.constant_bits = values[first].constant_bits;
						changed = true;
					}
				}
			}
		}
	}

	void mark_live(int p_value) {
		p_value = resolve(p_value);
		if (p_value < 0 || values[p_value].live || values[p_value].is_constant) {
			return;
		}
		Value &value = values.write[p_value];
		value.live = true;
		for (int input : value.inputs) {
			mark_live(input);
		}
	}

	void find_live_values() {
		for (const Instruction &instruction : instructions) {
			switch (instruction.opcode) {
				case GDScriptFunction::OPCODE_JUMP_COMPARE_INT:
				case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT:
				case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
				case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL:
				case GDScriptFunction::OPCODE_RETURN:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN:
				case GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN:
					for (int input : instruction.inputs) {
						mark_live(input);
					}
					break;
				default:
					break;
			}
		}
		for (Value &value : values) {
			if ((value.kind == VALUE_INT_OPERATOR || value.kind == VALUE_FLOAT_OPERATOR) && value.replacement < 0 && !value.is_constant && !value.live) {
				eliminated_node_count++;
			}
		}
	}

	void allocate_frame() {
		int offset = 0;
		for (Value &value : values) {
			if (value.live && !value.is_constant && value.replacement < 0) {
				value.frame_offset = offset;
				offset += sizeof(uint64_t);
			}
		}

		for (const Block &block : blocks) {
			int copies = 0;
			for (int phi : block.phis) {
				if (phi >= 0 && values[resolve(phi)].live && !values[resolve(phi)].is_constant) {
					copies++;
				}
			}
			max_phi_copies = MAX(max_phi_copies, copies);
		}
		phi_scratch_offset = offset;
		offset += max_phi_copies * sizeof(uint64_t);
		ptrcall_values_offset = offset;
		offset += max_ptrcall_argument_count * sizeof(uint64_t);
		ptrcall_arguments_offset = offset;
		offset += max_ptrcall_argument_count * sizeof(void *);
		ptrcall_return_offset = offset;
		offset += sizeof(uint64_t);
		frame_size = offset;
	}

public:
	CompactSSAPlan(const int *p_code, int p_code_size, int p_stack_size, const Variant *p_constants, int p_constant_count, const Vector<Variant::Type> &p_stack_types, int p_max_ptrcall_argument_count) :
			code(p_code), code_size(p_code_size), stack_size(p_stack_size), constants(p_constants), constant_count(p_constant_count), stack_types(p_stack_types), max_ptrcall_argument_count(p_max_ptrcall_argument_count) {}

	bool build() {
		Vector<uint8_t> starts;
		starts.resize(code_size + 1);
		starts.fill(0);
		starts.write[0] = 1;
		int ip = 0;
		while (ip < code_size) {
			const GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(code[ip]);
			const int length = instruction_length(ip);
			if (length <= 0) {
				return false;
			}
			const int next = ip + length;
			switch (opcode) {
				case GDScriptFunction::OPCODE_JUMP_COMPARE_INT:
				case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT:
					starts.write[code[ip + 5]] = 1;
					if (next < code_size) starts.write[next] = 1;
					break;
				case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
				case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL:
					starts.write[code[ip + 2]] = 1;
					if (next < code_size) starts.write[next] = 1;
					break;
				case GDScriptFunction::OPCODE_JUMP:
					starts.write[code[ip + 1]] = 1;
					if (next < code_size) starts.write[next] = 1;
					break;
				case GDScriptFunction::OPCODE_RETURN:
				case GDScriptFunction::OPCODE_END:
					if (next < code_size) starts.write[next] = 1;
					break;
				default:
					break;
			}
			ip = next;
		}

		ip_to_block.resize(code_size + 1);
		ip_to_block.fill(-1);
		for (int start = 0; start < code_size;) {
			DEV_ASSERT(starts[start]);
			int end = start + instruction_length(start);
			while (end < code_size && !starts[end]) {
				end += instruction_length(end);
			}
			Block block;
			block.start = start;
			block.end = end;
			block.phis.resize(stack_size);
			block.phis.fill(-1);
			blocks.push_back(block);
			for (int cursor = start; cursor < end; cursor += instruction_length(cursor)) {
				ip_to_block.write[cursor] = blocks.size() - 1;
			}
			start = end;
		}

		ip_to_instruction.resize(code_size + 1);
		ip_to_instruction.fill(-1);
		initial_values.resize(stack_size);
		initial_values.fill(-1);
		for (int slot = 0; slot < stack_size; slot++) {
			if (stack_types[slot] != Variant::NIL) {
				initial_values.write[slot] = add_value(VALUE_INPUT, stack_types[slot]);
			}
		}
		constant_values.resize(constant_count);
		constant_values.fill(-1);

		for (int block_index = 0; block_index < blocks.size(); block_index++) {
			Block &block = blocks.write[block_index];
			for (int cursor = block.start; cursor < block.end; cursor += instruction_length(cursor)) {
				Instruction instruction;
				instruction.opcode = GDScriptFunction::Opcode(code[cursor]);
				instruction.ip = cursor;
				if (instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_INT || instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_FLOAT) {
					const int destination = code[cursor + 3] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_INT ? VALUE_INT_OPERATOR : VALUE_FLOAT_OPERATOR, stack_types[destination], block_index, cursor);
					values.write[instruction.output].op = Variant::Operator(code[cursor + 4]);
				} else if (instruction.opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || instruction.opcode == GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN) {
					const int instruction_argument_count = code[cursor + 1];
					const int destination = code[cursor + 1 + instruction_argument_count] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(VALUE_PTRCALL_RESULT, stack_types[destination], block_index, cursor);
				}
				instructions.push_back(instruction);
				const int instruction_index = instructions.size() - 1;
				block.instructions.push_back(instruction_index);
				ip_to_instruction.write[cursor] = instruction_index;
			}

			const int last_ip = instructions[block.instructions[block.instructions.size() - 1]].ip;
			const GDScriptFunction::Opcode last_opcode = GDScriptFunction::Opcode(code[last_ip]);
			auto add_successor = [&](int p_target_ip) {
				const int successor = ip_to_block[p_target_ip];
				if (successor >= 0 && !block.successors.has(successor)) {
					block.successors.push_back(successor);
				}
			};
			switch (last_opcode) {
				case GDScriptFunction::OPCODE_JUMP_COMPARE_INT:
				case GDScriptFunction::OPCODE_JUMP_COMPARE_FLOAT:
					add_successor(code[last_ip + 5]);
					if (block.end < code_size) add_successor(block.end);
					break;
				case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
				case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL:
					add_successor(code[last_ip + 2]);
					if (block.end < code_size) add_successor(block.end);
					break;
				case GDScriptFunction::OPCODE_JUMP:
					add_successor(code[last_ip + 1]);
					break;
				case GDScriptFunction::OPCODE_RETURN:
				case GDScriptFunction::OPCODE_END:
					break;
				default:
					if (block.end < code_size) add_successor(block.end);
					break;
			}
		}
		for (int block_index = 0; block_index < blocks.size(); block_index++) {
			for (int successor : blocks[block_index].successors) {
				blocks.write[successor].predecessors.push_back(block_index);
			}
		}

		bool changed = true;
		int iterations = 0;
		while (changed && iterations++ < blocks.size() * 4 + 8) {
			changed = false;
			for (int block_index = 0; block_index < blocks.size(); block_index++) {
				changed = merge_block(block_index) || changed;
				if (blocks[block_index].reachable) {
					changed = simulate_block(block_index) || changed;
				}
			}
		}
		if (changed) {
			return false;
		}

		finish_phi_inputs();
		optimize_values();
		find_live_values();
		allocate_frame();
		return frame_size <= SLJIT_MAX_LOCAL_SIZE;
	}

	const Vector<Value> &get_values() const { return values; }
	const Vector<Instruction> &get_instructions() const { return instructions; }
	const Vector<Block> &get_blocks() const { return blocks; }
	const Instruction &get_instruction_at(int p_ip) const { return instructions[ip_to_instruction[p_ip]]; }
	int get_block_at_ip(int p_ip) const { return ip_to_block[p_ip]; }
	int get_resolved_value(int p_value) const { return resolve(p_value); }
	int get_initial_value(int p_slot) const { return initial_values[p_slot]; }
	int get_frame_size() const { return frame_size; }
	int get_phi_scratch_offset() const { return phi_scratch_offset; }
	int get_ptrcall_values_offset() const { return ptrcall_values_offset; }
	int get_ptrcall_arguments_offset() const { return ptrcall_arguments_offset; }
	int get_ptrcall_return_offset() const { return ptrcall_return_offset; }
	int get_node_count() const { return values.size(); }
	int get_eliminated_node_count() const { return eliminated_node_count; }
};

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
	Variant::Type return_type = Variant::NIL;
	struct sljit_compiler *compiler = nullptr;
	Vector<struct sljit_label *> labels;
	Vector<PendingJump> pending_jumps;
	Vector<Variant::Type> stack_slot_types;
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
		ptrcall_arguments_offset = native_slot_count * sizeof(uint64_t);
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
		sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(constants));
		sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, p_register, 0, SLJIT_IMM, get_address_offset(p_address, variant_data_offset));
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
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), stack_slot_offsets[destination & GDScriptFunction::ADDR_MASK], SLJIT_IMM, 0);
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

	const CompactSSAPlan::Value &get_ssa_value(int p_value) const {
		p_value = ssa_plan->get_resolved_value(p_value);
		return ssa_plan->get_values()[p_value];
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

	void emit_ssa_initialize_inputs() {
		for (int slot = 0; slot < stack_slot_types.size(); slot++) {
			const int initial = ssa_plan->get_initial_value(slot);
			if (initial < 0) {
				continue;
			}
			const CompactSSAPlan::Value &value = get_ssa_value(initial);
			if (!value.live || value.frame_offset < 0) {
				continue;
			}

			const int argument_index = slot - GDScriptFunction::FIXED_ADDRESSES_MAX;
			if (argument_index >= 0 && argument_index < argument_types.size()) {
				if (typed_entry) {
					sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), argument_index * sizeof(uint64_t));
				} else {
					emit_load_base(slot, SLJIT_R0);
					sljit_emit_op1(compiler, stack_slot_types[slot] == Variant::BOOL ? SLJIT_MOV_U8 : SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0), get_address_offset(slot, variant_data_offset));
				}
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), value.frame_offset, SLJIT_R0, 0);
			} else {
				sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), value.frame_offset, SLJIT_IMM, 0);
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

	void emit_ssa_pointer(int p_value, int p_argument, int p_register) {
		const CompactSSAPlan::Value &value = get_ssa_value(p_value);
		if (value.kind == CompactSSAPlan::VALUE_CONSTANT && value.constant_index >= 0) {
			sljit_emit_op1(compiler, SLJIT_MOV, p_register, 0, SLJIT_IMM, reinterpret_cast<sljit_sw>(constants));
			sljit_emit_op2(compiler, SLJIT_ADD, p_register, 0, p_register, 0, SLJIT_IMM, value.constant_index * sizeof(Variant) + variant_data_offset);
		} else if (value.is_constant) {
			const int offset = ssa_plan->get_ptrcall_values_offset() + p_argument * sizeof(uint64_t);
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), offset, SLJIT_IMM, sljit_sw(value.constant_bits));
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
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), result_offset, SLJIT_IMM, 0);
			sljit_emit_op2(compiler, SLJIT_ADD, SLJIT_R3, 0, SLJIT_SP, 0, SLJIT_IMM, result_offset);
		} else {
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R3, 0, SLJIT_IMM, 0);
		}
		sljit_emit_icall(compiler, SLJIT_CALL, SLJIT_ARGS4V(P, P, P, P), SLJIT_IMM, SLJIT_FUNC_ADDR(invoke_ptrcall));
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

		int copy = 0;
		for (int phi : target.phis) {
			if (phi < 0) {
				continue;
			}
			const CompactSSAPlan::Value &destination = get_ssa_value(phi);
			if (!destination.live || destination.is_constant || destination.frame_offset < 0) {
				continue;
			}
			emit_ssa_load_raw(ssa_plan->get_values()[phi].inputs[predecessor_input], SLJIT_R0);
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), ssa_plan->get_phi_scratch_offset() + copy * sizeof(uint64_t), SLJIT_R0, 0);
			copy++;
		}
		copy = 0;
		for (int phi : target.phis) {
			if (phi < 0) {
				continue;
			}
			const CompactSSAPlan::Value &destination = get_ssa_value(phi);
			if (!destination.live || destination.is_constant || destination.frame_offset < 0) {
				continue;
			}
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_SP), ssa_plan->get_phi_scratch_offset() + copy * sizeof(uint64_t));
			sljit_emit_op1(compiler, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), destination.frame_offset, SLJIT_R0, 0);
			copy++;
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
			emit_ssa_load_raw(p_instruction.inputs[0], SLJIT_R0);
			sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
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
		emit_load_base(address, SLJIT_R2);
		sljit_emit_op1(compiler, SLJIT_MOV32, SLJIT_MEM1(SLJIT_R2), get_address_offset(address, variant_type_offset), SLJIT_IMM, value.type);
		emit_ssa_load_raw(p_instruction.inputs[0], SLJIT_R1);
		sljit_emit_op1(compiler, value.type == Variant::BOOL ? SLJIT_MOV_U8 : SLJIT_MOV, SLJIT_MEM1(SLJIT_R2), get_address_offset(address, variant_data_offset), SLJIT_R1, 0);
		emit_variant_pointer(address, SLJIT_R0);
		sljit_emit_return(compiler, SLJIT_MOV, SLJIT_R0, 0);
	}

	void *compile_ssa(uint64_t &r_code_size) {
		labels.resize(ssa_plan->get_blocks().size());
		sljit_emit_enter(compiler, 0, typed_entry ? SLJIT_ARGS2(W, P, P) : SLJIT_ARGS2(P, P, P), 4 | SLJIT_ENTER_FLOAT(2), 2, ssa_plan->get_frame_size());
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
						emit_ssa_operator(instruction);
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
					emit_ssa_return(terminator);
					break;
				case GDScriptFunction::OPCODE_END:
					if (typed_entry) {
						sljit_emit_return(compiler, SLJIT_MOV, SLJIT_IMM, 0);
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
			sljit_emit_return(compiler, SLJIT_MOV, SLJIT_IMM, 0);
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
	BaselineCompiler(const int *p_code, int p_code_size, int p_stack_size, const Variant *p_constants, int p_constant_count, MethodBind *const *p_methods, int p_method_count, const Vector<Variant::Type> &p_argument_types, Variant::Type p_return_type, bool p_typed_entry, bool p_optimizing = false) :
			code(p_code), code_size(p_code_size), stack_size(p_stack_size), constant_count(p_constant_count), constants(p_constants), method_count(p_method_count), methods(p_methods), argument_types(p_argument_types), return_type(p_return_type), typed_entry(p_typed_entry), optimizing(p_optimizing) {
		Variant probe;
		variant_type_offset = reinterpret_cast<uint8_t *>(VariantInternal::get_type_ptr(&probe)) - reinterpret_cast<uint8_t *>(&probe);
		variant_data_offset = reinterpret_cast<uint8_t *>(VariantInternal::get_int(&probe)) - reinterpret_cast<uint8_t *>(&probe);
	}

	int get_ptrcall_count() const { return ptrcall_count; }
	bool get_requires_self() const { return requires_self; }
	int get_ssa_node_count() const { return ssa_node_count; }
	int get_eliminated_node_count() const { return eliminated_node_count; }

	void *compile(uint64_t &r_code_size) {
		if (!validate()) {
			return nullptr;
		}
		if (optimizing) {
			ssa_plan = memnew(CompactSSAPlan(code, code_size, stack_size, constants, constant_count, stack_slot_types, max_ptrcall_argument_count));
			if (!ssa_plan->build()) {
				memdelete(ssa_plan);
				ssa_plan = nullptr;
				return nullptr;
			}
			ssa_node_count = ssa_plan->get_node_count();
			eliminated_node_count = ssa_plan->get_eliminated_node_count();
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
		sljit_emit_enter(compiler, 0, typed_entry ? SLJIT_ARGS2(W, P, P) : SLJIT_ARGS2(P, P, P), 4 | SLJIT_ENTER_FLOAT(2), 2, native_frame_size);
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

GDScriptBaselineJIT::GDScriptBaselineJIT(void *p_entry_point, void *p_typed_entry_point, uint64_t p_code_size, const Vector<Variant::Type> &p_typed_argument_types, Variant::Type p_typed_return_type, int p_ptrcall_count, bool p_requires_self, bool p_optimizing, int p_ssa_node_count, int p_eliminated_node_count) :
		entry_point(p_entry_point), typed_entry_point(p_typed_entry_point), code_size(p_code_size), typed_argument_types(p_typed_argument_types), typed_return_type(p_typed_return_type), ptrcall_count(p_ptrcall_count), ssa_node_count(p_ssa_node_count), eliminated_node_count(p_eliminated_node_count), requires_self(p_requires_self), optimizing(p_optimizing) {
}

GDScriptBaselineJIT *GDScriptBaselineJIT::_compile(const GDScriptFunction *p_function, bool p_optimizing) {
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

	BaselineCompiler compiler(p_function->_code_ptr, p_function->_code_size, p_function->_stack_size, p_function->_constants_ptr, p_function->_constant_count, p_function->_methods_ptr, p_function->_methods_count, typed_argument_types, Variant::NIL, false, p_optimizing);
	uint64_t generated_size = 0;
	void *generated_code = compiler.compile(generated_size);
	if (!generated_code) {
		return nullptr;
	}

	void *typed_generated_code = nullptr;
	if (has_typed_signature) {
		BaselineCompiler typed_compiler(p_function->_code_ptr, p_function->_code_size, p_function->_stack_size, p_function->_constants_ptr, p_function->_constant_count, p_function->_methods_ptr, p_function->_methods_count, typed_argument_types, p_function->return_type.builtin_type, true, p_optimizing);
		uint64_t typed_generated_size = 0;
		typed_generated_code = typed_compiler.compile(typed_generated_size);
		generated_size += typed_generated_size;
	}

	return memnew(GDScriptBaselineJIT(generated_code, typed_generated_code, generated_size, typed_argument_types, has_typed_signature ? p_function->return_type.builtin_type : Variant::NIL, compiler.get_ptrcall_count(), compiler.get_requires_self(), p_optimizing, compiler.get_ssa_node_count(), compiler.get_eliminated_node_count()));
}

GDScriptBaselineJIT *GDScriptBaselineJIT::compile(const GDScriptFunction *p_function) {
	return _compile(p_function, false);
}

GDScriptBaselineJIT *GDScriptBaselineJIT::compile_optimized(const GDScriptFunction *p_function) {
	return _compile(p_function, true);
}

Variant *GDScriptBaselineJIT::execute(Variant **p_variant_addresses, Object *p_self) const {
	if (requires_self && p_self == nullptr) {
		return nullptr;
	}
	typedef Variant *(SLJIT_FUNC *EntryPoint)(Variant **, Object *);
	return reinterpret_cast<EntryPoint>(entry_point)(p_variant_addresses, p_self);
}

bool GDScriptBaselineJIT::execute_typed(const Variant **p_arguments, int p_argument_count, Object *p_self, Variant &r_return) const {
	if (typed_entry_point == nullptr || p_argument_count != typed_argument_types.size() || (requires_self && p_self == nullptr)) {
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

	typedef uint64_t(SLJIT_FUNC * TypedEntryPoint)(const uint64_t *, Object *);
	const uint64_t result = reinterpret_cast<TypedEntryPoint>(typed_entry_point)(raw_arguments, p_self);
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
