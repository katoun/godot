/**************************************************************************/
/*  gdscript_compact_ssa.inc                                              */
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

// Compact SSA construction, optimization, and scalar replacement.
class CompactSSAPlan {
public:
	enum ValueKind {
		VALUE_INPUT,
		VALUE_CONSTANT,
		VALUE_PHI,
		VALUE_INT_OPERATOR,
		VALUE_FLOAT_OPERATOR,
		VALUE_MATH_OPERATOR,
		VALUE_MATH_COMPONENT,
		VALUE_MATH_EXTRACT,
		VALUE_MATH_UPDATE,
		VALUE_MATH_SET_COMPONENT,
		VALUE_MATH_LENGTH,
		VALUE_STRUCT_CONSTRUCT,
		VALUE_STRUCT_FIELD,
		VALUE_STRUCT_FIELD_COMPONENT,
		VALUE_STRUCT_UPDATE,
		VALUE_STRUCT_EQUAL,
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
		int component_index = -1;
		int struct_field_index = -1;
		Ref<StructLayout> struct_layout;
		Vector<int> inputs;
		// Math aggregates have no frame slot in the optimizing tier. Their
		// independently live scalar values are tracked here instead.
		Vector<int> components;
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
	const Vector<Ref<StructLayout>> &stack_struct_layouts;
	int max_ptrcall_argument_count = 0;
	int max_materialized_value_count = 2;
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
	int scalar_replaced_math_value_count = 0;

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
			case GDScriptFunction::OPCODE_ASSIGN_STRUCT:
			case GDScriptFunction::OPCODE_JUMP_IF_BOOL:
			case GDScriptFunction::OPCODE_JUMP_IF_NOT_BOOL:
			case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT:
				return 3;
			case GDScriptFunction::OPCODE_ASSIGN_MATH:
			case GDScriptFunction::OPCODE_GET_MATH_COMPONENT:
			case GDScriptFunction::OPCODE_SET_MATH_COMPONENT:
			case GDScriptFunction::OPCODE_MATH_LENGTH:
			case GDScriptFunction::OPCODE_GET_STRUCT_FIELD:
			case GDScriptFunction::OPCODE_SET_STRUCT_FIELD:
			case GDScriptFunction::OPCODE_EQUAL_STRUCT:
				return 4;
			case GDScriptFunction::OPCODE_CONSTRUCT_STRUCT:
				return code[p_ip + 1] + 3;
			case GDScriptFunction::OPCODE_OPERATOR_INT:
			case GDScriptFunction::OPCODE_OPERATOR_FLOAT:
				return 5;
			case GDScriptFunction::OPCODE_OPERATOR_MATH:
				return 6;
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

	int add_value(ValueKind p_kind, Variant::Type p_type, int p_block = -1, int p_ip = -1, const Ref<StructLayout> &p_struct_layout = Ref<StructLayout>()) {
		Value value;
		value.kind = p_kind;
		value.type = p_type;
		value.block = p_block;
		value.ip = p_ip;
		value.struct_layout = p_struct_layout;
		values.push_back(value);
		return values.size() - 1;
	}

	void add_math_components(int p_value, ValueKind p_component_kind) {
		const Variant::Type type = values[p_value].type;
		const int component_count = get_math_component_count(type);
		DEV_ASSERT(component_count > 0);
		Vector<int> components;
		for (int component = 0; component < component_count; component++) {
			const int component_value = add_value(p_component_kind, type, values[p_value].block, values[p_value].ip);
			values.write[component_value].component_index = component;
			components.push_back(component_value);
		}
		values.write[p_value].components = components;
		scalar_replaced_math_value_count++;
	}

	int get_math_component_value(int p_value, int p_component) const {
		p_value = resolve(p_value);
		DEV_ASSERT(p_value >= 0 && is_unboxed_math_type(values[p_value].type));
		DEV_ASSERT(p_component >= 0 && p_component < values[p_value].components.size());
		return resolve(values[p_value].components[p_component]);
	}

	Ref<StructLayout> get_stack_struct_layout(int p_slot) const {
		return p_slot >= 0 && p_slot < stack_struct_layouts.size() ? stack_struct_layouts[p_slot] : Ref<StructLayout>();
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
		if (is_unboxed_math_type(type)) {
			add_math_components(value_index, VALUE_MATH_COMPONENT);
			for (int component = 0; component < get_math_component_count(type); component++) {
				double component_value = 0.0;
				switch (type) {
					case Variant::VECTOR2:
						component_value = (*VariantInternal::get_vector2(&constants[p_index]))[component];
						break;
					case Variant::VECTOR3:
						component_value = (*VariantInternal::get_vector3(&constants[p_index]))[component];
						break;
					case Variant::COLOR:
						component_value = (*VariantInternal::get_color(&constants[p_index]))[component];
						break;
					default:
						break;
				}
				Value &component_node = values.write[values[value_index].components[component]];
				component_node.is_constant = true;
				memcpy(&component_node.constant_bits, &component_value, sizeof(component_value));
			}
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
				case GDScriptFunction::OPCODE_ASSIGN_FLOAT:
				case GDScriptFunction::OPCODE_ASSIGN_MATH: {
					inputs.push_back(address_value(code[ip + 2], environment));
					const int destination = code[ip + 1] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = inputs[0];
				} break;
				case GDScriptFunction::OPCODE_ASSIGN_STRUCT: {
					inputs.push_back(address_value(code[ip + 2], environment));
					const int destination = code[ip + 1] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = inputs[0];
				} break;
				case GDScriptFunction::OPCODE_CONSTRUCT_STRUCT: {
					const int argument_count = code[ip + 1] - 2;
					for (int i = 0; i < argument_count; i++) {
						inputs.push_back(address_value(code[ip + i + 2], environment));
					}
					changed = set_inputs(instruction.output, inputs) || changed;
					const int destination = code[ip + argument_count + 2] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = instruction.output;
				} break;
				case GDScriptFunction::OPCODE_GET_STRUCT_FIELD: {
					inputs.push_back(address_value(code[ip + 1], environment));
					changed = set_inputs(instruction.output, inputs) || changed;
					if (is_unboxed_math_type(values[instruction.output].type)) {
						for (int component : values[instruction.output].components) {
							changed = set_inputs(component, inputs) || changed;
						}
					}
					const int destination = code[ip + 2] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = instruction.output;
				} break;
				case GDScriptFunction::OPCODE_SET_STRUCT_FIELD: {
					inputs.push_back(address_value(code[ip + 1], environment));
					inputs.push_back(address_value(code[ip + 2], environment));
					changed = set_inputs(instruction.output, inputs) || changed;
					const int destination = code[ip + 1] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = instruction.output;
				} break;
				case GDScriptFunction::OPCODE_EQUAL_STRUCT: {
					inputs.push_back(address_value(code[ip + 1], environment));
					inputs.push_back(address_value(code[ip + 2], environment));
					changed = set_inputs(instruction.output, inputs) || changed;
					const int destination = code[ip + 3] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = instruction.output;
				} break;
				case GDScriptFunction::OPCODE_OPERATOR_INT:
				case GDScriptFunction::OPCODE_OPERATOR_FLOAT:
				case GDScriptFunction::OPCODE_OPERATOR_MATH: {
					inputs.push_back(address_value(code[ip + 1], environment));
					const Variant::Operator op = instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_MATH ? GDScriptFunction::get_math_operator(code[ip + 5]) : Variant::Operator(code[ip + 4]);
					if (!is_unary(op)) {
						inputs.push_back(address_value(code[ip + 2], environment));
					}
					changed = set_inputs(instruction.output, inputs) || changed;
					if (instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_MATH && is_unboxed_math_type(values[instruction.output].type)) {
						const int metadata = code[ip + 5];
						const Variant::Type left_type = GDScriptFunction::get_math_left_type(metadata);
						const Variant::Type right_type = GDScriptFunction::get_math_right_type(metadata);
						for (int component = 0; component < values[instruction.output].components.size(); component++) {
							Vector<int> component_inputs;
							component_inputs.push_back(is_unboxed_math_type(left_type) ? get_math_component_value(inputs[0], component) : resolve(inputs[0]));
							if (!is_unary(op)) {
								component_inputs.push_back(is_unboxed_math_type(right_type) ? get_math_component_value(inputs[1], component) : resolve(inputs[1]));
							}
							changed = set_inputs(values[instruction.output].components[component], component_inputs) || changed;
						}
					}
					const int destination = code[ip + 3] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = instruction.output;
				} break;
				case GDScriptFunction::OPCODE_GET_MATH_COMPONENT: {
					inputs.push_back(address_value(code[ip + 1], environment));
					changed = set_inputs(instruction.output, inputs) || changed;
					const int destination = code[ip + 2] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = instruction.output;
				} break;
				case GDScriptFunction::OPCODE_SET_MATH_COMPONENT: {
					inputs.push_back(address_value(code[ip + 1], environment));
					inputs.push_back(address_value(code[ip + 2], environment));
					changed = set_inputs(instruction.output, inputs) || changed;
					const int component = GDScriptFunction::get_math_component_index(code[ip + 3]);
					Vector<int> components = values[resolve(inputs[0])].components;
					const int component_value = values[instruction.output].components[component];
					Vector<int> component_inputs;
					component_inputs.push_back(inputs[1]);
					changed = set_inputs(component_value, component_inputs) || changed;
					components.write[component] = component_value;
					if (values[instruction.output].components != components) {
						values.write[instruction.output].components = components;
						changed = true;
					}
					const int destination = code[ip + 1] & GDScriptFunction::ADDR_MASK;
					environment.write[destination] = instruction.output;
				} break;
				case GDScriptFunction::OPCODE_MATH_LENGTH: {
					inputs.push_back(address_value(code[ip + 1], environment));
					changed = set_inputs(instruction.output, inputs) || changed;
					const int destination = code[ip + 2] & GDScriptFunction::ADDR_MASK;
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
				case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT:
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
					block.phis.write[slot] = add_value(VALUE_PHI, stack_types[slot], p_block, -1, get_stack_struct_layout(slot));
					if (is_unboxed_math_type(stack_types[slot])) {
						add_math_components(block.phis[slot], VALUE_PHI);
					}
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
				if (is_unboxed_math_type(values[phi].type)) {
					for (int component = 0; component < values[phi].components.size(); component++) {
						Vector<int> component_inputs;
						for (int input : phi_inputs) {
							component_inputs.push_back(get_math_component_value(input, component));
						}
						values.write[values[phi].components[component]].inputs = component_inputs;
					}
				}
			}
		}
	}

	static bool constant_equal(const Value &p_left, const Value &p_right) {
		if (!p_left.is_constant || !p_right.is_constant || p_left.type != p_right.type) {
			return false;
		}
		if (is_unboxed_math_type(p_left.type) && (!p_left.components.is_empty() || !p_right.components.is_empty())) {
			return p_left.constant_index >= 0 && p_left.constant_index == p_right.constant_index;
		}
		if (is_unboxed_large_value_type(p_left.type)) {
			return p_left.constant_index >= 0 && p_left.constant_index == p_right.constant_index;
		}
		return p_left.constant_bits == p_right.constant_bits;
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

	double get_constant_numeric(int p_value) const {
		const Value &value = values[resolve(p_value)];
		DEV_ASSERT(value.is_constant);
		if (value.type == Variant::INT) {
			return double(int64_t(value.constant_bits));
		}
		double result = 0.0;
		memcpy(&result, &value.constant_bits, sizeof(result));
		return result;
	}

	bool fold_math_component(Value &r_value) {
		if (r_value.inputs.is_empty()) {
			return false;
		}
		for (int input : r_value.inputs) {
			if (!values[resolve(input)].is_constant) {
				return false;
			}
		}

		double result = 0.0;
		if (r_value.kind == VALUE_MATH_SET_COMPONENT) {
			result = get_constant_numeric(r_value.inputs[0]);
			if (!math_uses_double_components(r_value.type)) {
				result = float(result);
			}
		} else if (math_uses_double_components(r_value.type)) {
			const double lhs = get_constant_numeric(r_value.inputs[0]);
			const double rhs = r_value.inputs.size() > 1 ? get_constant_numeric(r_value.inputs[1]) : 0.0;
			switch (r_value.op) {
				case Variant::OP_ADD: result = lhs + rhs; break;
				case Variant::OP_SUBTRACT: result = lhs - rhs; break;
				case Variant::OP_MULTIPLY: result = lhs * rhs; break;
				case Variant::OP_DIVIDE: result = lhs / rhs; break;
				case Variant::OP_NEGATE: result = -lhs; break;
				case Variant::OP_POSITIVE: result = lhs; break;
				default: return false;
			}
		} else {
			const float lhs = float(get_constant_numeric(r_value.inputs[0]));
			const float rhs = r_value.inputs.size() > 1 ? float(get_constant_numeric(r_value.inputs[1])) : 0.0f;
			float float_result = 0.0f;
			switch (r_value.op) {
				case Variant::OP_ADD: float_result = lhs + rhs; break;
				case Variant::OP_SUBTRACT: float_result = lhs - rhs; break;
				case Variant::OP_MULTIPLY: float_result = lhs * rhs; break;
				case Variant::OP_DIVIDE: float_result = lhs / rhs; break;
				case Variant::OP_NEGATE: float_result = -lhs; break;
				case Variant::OP_POSITIVE: float_result = lhs; break;
				default: return false;
			}
			result = float_result;
		}
		memcpy(&r_value.constant_bits, &result, sizeof(result));
		r_value.is_constant = true;
		return true;
	}

	void forward_struct_fields() {
		for (int i = 0; i < values.size(); i++) {
			Value &field_value = values.write[i];
			if (field_value.kind != VALUE_STRUCT_FIELD || field_value.inputs.size() != 1) {
				continue;
			}

			int source = resolve(field_value.inputs[0]);
			int forwarded = -1;
			while (source >= 0) {
				const Value &source_value = values[source];
				if (source_value.kind == VALUE_STRUCT_UPDATE && source_value.inputs.size() == 2) {
					if (source_value.struct_field_index == field_value.struct_field_index) {
						forwarded = resolve(source_value.inputs[1]);
						break;
					}
					source = resolve(source_value.inputs[0]);
					continue;
				}
				if (source_value.kind == VALUE_STRUCT_CONSTRUCT && source_value.inputs.size() == source_value.struct_layout->get_field_count()) {
					forwarded = resolve(source_value.inputs[field_value.struct_field_index]);
				}
				break;
			}

			if (forwarded >= 0) {
				field_value.replacement = forwarded;
				eliminated_node_count++;
				continue;
			}
			if (source >= 0 && source != resolve(field_value.inputs[0])) {
				field_value.inputs.write[0] = source;
				for (int component : field_value.components) {
					values.write[component].inputs.write[0] = source;
				}
			}
		}
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
				if (value.kind != VALUE_INT_OPERATOR && value.kind != VALUE_FLOAT_OPERATOR && value.kind != VALUE_MATH_OPERATOR) {
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
				} else if ((value.kind == VALUE_MATH_COMPONENT || value.kind == VALUE_MATH_SET_COMPONENT) && !value.inputs.is_empty()) {
					if (fold_math_component(value)) {
						eliminated_node_count++;
						changed = true;
					}
				} else if (value.kind == VALUE_PHI && !value.inputs.is_empty() && !is_unboxed_large_value_type(value.type) && (!is_unboxed_math_type(value.type) || value.components.is_empty())) {
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
		if (is_unboxed_math_type(value.type) && !value.components.is_empty()) {
			const Vector<int> components = value.components;
			for (int component : components) {
				mark_live(component);
			}
			return;
		}
		if (value.kind == VALUE_MATH_EXTRACT) {
			DEV_ASSERT(value.inputs.size() == 1);
			mark_live(get_math_component_value(value.inputs[0], value.component_index));
			return;
		}
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
				case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT:
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
			if ((value.kind == VALUE_INT_OPERATOR || value.kind == VALUE_FLOAT_OPERATOR || value.kind == VALUE_MATH_OPERATOR || value.kind == VALUE_MATH_COMPONENT || value.kind == VALUE_MATH_EXTRACT || value.kind == VALUE_MATH_SET_COMPONENT || value.kind == VALUE_MATH_LENGTH || value.kind == VALUE_STRUCT_CONSTRUCT || value.kind == VALUE_STRUCT_FIELD || value.kind == VALUE_STRUCT_FIELD_COMPONENT || value.kind == VALUE_STRUCT_UPDATE || value.kind == VALUE_STRUCT_EQUAL) && value.replacement < 0 && !value.is_constant && !value.live) {
				eliminated_node_count++;
			}
		}
	}

	static bool is_scalar_replaced_aggregate(const Value &p_value) {
		return is_unboxed_math_type(p_value.type) && !p_value.components.is_empty();
	}

	static int get_value_size(const Value &p_value) {
		if (p_value.kind == VALUE_MATH_COMPONENT || p_value.kind == VALUE_MATH_SET_COMPONENT || (p_value.kind == VALUE_PHI && is_unboxed_math_type(p_value.type) && p_value.components.is_empty())) {
			return get_math_component_size(p_value.type);
		}
		return get_unboxed_native_size(p_value.type, p_value.struct_layout);
	}

	static int get_value_alignment(const Value &p_value) {
		if (p_value.kind == VALUE_MATH_COMPONENT || p_value.kind == VALUE_MATH_SET_COMPONENT || (p_value.kind == VALUE_PHI && is_unboxed_math_type(p_value.type) && p_value.components.is_empty())) {
			return get_math_component_size(p_value.type);
		}
		return get_unboxed_native_alignment(p_value.type, p_value.struct_layout);
	}

	void allocate_frame() {
		int offset = 0;
		for (Value &value : values) {
			if (value.live && !value.is_constant && value.replacement < 0 && !is_scalar_replaced_aggregate(value)) {
				offset = align_native_offset(offset, get_value_alignment(value));
				value.frame_offset = offset;
				offset += get_value_size(value);
			}
		}

		for (const Block &block : blocks) {
			int scratch_size = 0;
			for (int phi : block.phis) {
				if (phi >= 0) {
					const Value &phi_value = values[resolve(phi)];
					if (is_scalar_replaced_aggregate(phi_value)) {
						for (int component : phi_value.components) {
							const Value &component_value = values[resolve(component)];
							if (component_value.live && !component_value.is_constant) {
								scratch_size = align_native_offset(scratch_size, get_value_alignment(component_value));
								scratch_size += get_value_size(component_value);
							}
						}
					} else if (phi_value.live && !phi_value.is_constant) {
						scratch_size = align_native_offset(scratch_size, get_value_alignment(phi_value));
						scratch_size += get_value_size(phi_value);
					}
				}
			}
			max_phi_copies = MAX(max_phi_copies, scratch_size);
		}
		offset = align_native_offset(offset, NATIVE_VALUE_ALIGNMENT);
		phi_scratch_offset = offset;
		offset += max_phi_copies;
		offset = align_native_offset(offset, NATIVE_VALUE_ALIGNMENT);
		ptrcall_values_offset = offset;
		offset += MAX(max_ptrcall_argument_count, max_materialized_value_count) * NATIVE_VALUE_STRIDE;
		ptrcall_arguments_offset = offset;
		offset += max_ptrcall_argument_count * sizeof(void *);
		ptrcall_return_offset = offset;
		offset += NATIVE_VALUE_STRIDE;
		frame_size = offset;
	}

public:
	CompactSSAPlan(const int *p_code, int p_code_size, int p_stack_size, const Variant *p_constants, int p_constant_count, const Vector<Variant::Type> &p_stack_types, const Vector<Ref<StructLayout>> &p_stack_struct_layouts, int p_max_ptrcall_argument_count) :
			code(p_code), code_size(p_code_size), stack_size(p_stack_size), constants(p_constants), constant_count(p_constant_count), stack_types(p_stack_types), stack_struct_layouts(p_stack_struct_layouts), max_ptrcall_argument_count(p_max_ptrcall_argument_count) {}

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
				case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT:
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
				initial_values.write[slot] = add_value(VALUE_INPUT, stack_types[slot], -1, -1, get_stack_struct_layout(slot));
				if (is_unboxed_math_type(stack_types[slot])) {
					add_math_components(initial_values[slot], VALUE_MATH_COMPONENT);
				}
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
				if (instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_INT || instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_FLOAT || instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_MATH) {
					const int destination = code[cursor + 3] & GDScriptFunction::ADDR_MASK;
					const ValueKind kind = instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_INT ? VALUE_INT_OPERATOR : (instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_FLOAT ? VALUE_FLOAT_OPERATOR : VALUE_MATH_OPERATOR);
					instruction.output = add_value(kind, stack_types[destination], block_index, cursor);
					values.write[instruction.output].op = instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_MATH ? GDScriptFunction::get_math_operator(code[cursor + 5]) : Variant::Operator(code[cursor + 4]);
					if (instruction.opcode == GDScriptFunction::OPCODE_OPERATOR_MATH && is_unboxed_math_type(stack_types[destination])) {
						add_math_components(instruction.output, VALUE_MATH_COMPONENT);
						for (int component : values[instruction.output].components) {
							values.write[component].op = values[instruction.output].op;
						}
					}
				} else if (instruction.opcode == GDScriptFunction::OPCODE_GET_MATH_COMPONENT) {
					const int destination = code[cursor + 2] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(VALUE_MATH_EXTRACT, stack_types[destination], block_index, cursor);
					values.write[instruction.output].component_index = GDScriptFunction::get_math_component_index(code[cursor + 3]);
				} else if (instruction.opcode == GDScriptFunction::OPCODE_SET_MATH_COMPONENT) {
					const int destination = code[cursor + 1] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(VALUE_MATH_UPDATE, stack_types[destination], block_index, cursor);
					add_math_components(instruction.output, VALUE_MATH_SET_COMPONENT);
				} else if (instruction.opcode == GDScriptFunction::OPCODE_MATH_LENGTH) {
					const int destination = code[cursor + 2] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(VALUE_MATH_LENGTH, stack_types[destination], block_index, cursor);
				} else if (instruction.opcode == GDScriptFunction::OPCODE_CONSTRUCT_STRUCT) {
					const int argument_count = code[cursor + 1] - 2;
					max_materialized_value_count = MAX(max_materialized_value_count, argument_count);
					const int destination = code[cursor + argument_count + 2] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(VALUE_STRUCT_CONSTRUCT, Variant::STRUCT, block_index, cursor, get_stack_struct_layout(destination));
				} else if (instruction.opcode == GDScriptFunction::OPCODE_GET_STRUCT_FIELD) {
					const int destination = code[cursor + 2] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(VALUE_STRUCT_FIELD, stack_types[destination], block_index, cursor, get_stack_struct_layout(destination));
					values.write[instruction.output].struct_field_index = code[cursor + 3];
					if (is_unboxed_math_type(stack_types[destination])) {
						add_math_components(instruction.output, VALUE_STRUCT_FIELD_COMPONENT);
					}
				} else if (instruction.opcode == GDScriptFunction::OPCODE_SET_STRUCT_FIELD) {
					const int destination = code[cursor + 1] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(VALUE_STRUCT_UPDATE, Variant::STRUCT, block_index, cursor, get_stack_struct_layout(destination));
					values.write[instruction.output].struct_field_index = code[cursor + 3];
				} else if (instruction.opcode == GDScriptFunction::OPCODE_EQUAL_STRUCT) {
					instruction.output = add_value(VALUE_STRUCT_EQUAL, Variant::BOOL, block_index, cursor);
				} else if (instruction.opcode == GDScriptFunction::OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN || instruction.opcode == GDScriptFunction::OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN) {
					const int instruction_argument_count = code[cursor + 1];
					const int destination = code[cursor + 1 + instruction_argument_count] & GDScriptFunction::ADDR_MASK;
					instruction.output = add_value(VALUE_PTRCALL_RESULT, stack_types[destination], block_index, cursor, get_stack_struct_layout(destination));
					if (is_unboxed_math_type(stack_types[destination])) {
						add_math_components(instruction.output, VALUE_MATH_COMPONENT);
					}
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
				case GDScriptFunction::OPCODE_RETURN_TYPED_STRUCT:
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
		forward_struct_fields();
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
	int get_component_value(int p_value, int p_component) const { return get_math_component_value(p_value, p_component); }
	int get_initial_value(int p_slot) const { return initial_values[p_slot]; }
	int get_frame_size() const { return frame_size; }
	int get_phi_scratch_offset() const { return phi_scratch_offset; }
	int get_ptrcall_values_offset() const { return ptrcall_values_offset; }
	int get_ptrcall_arguments_offset() const { return ptrcall_arguments_offset; }
	int get_ptrcall_return_offset() const { return ptrcall_return_offset; }
	int get_node_count() const { return values.size(); }
	int get_eliminated_node_count() const { return eliminated_node_count; }
	int get_scalar_replaced_math_value_count() const { return scalar_replaced_math_value_count; }
};
