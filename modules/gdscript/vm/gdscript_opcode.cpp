/**************************************************************************/
/*  gdscript_opcode.cpp                                                   */
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

#include "gdscript_function.h"

#include <initializer_list>

static GDScriptFunction::OpcodeOperandList _gdscript_opcode_operands(std::initializer_list<GDScriptFunction::OpcodeOperandKind> p_kinds) {
	GDScriptFunction::OpcodeOperandList result;
	uint32_t shift = 0;
	for (GDScriptFunction::OpcodeOperandKind kind : p_kinds) {
		DEV_ASSERT(shift < 64);
		result.packed_kinds |= uint64_t(kind) << shift;
		result.count++;
		shift += 5;
	}
	return result;
}

static GDScriptFunction::OpcodeOperandKind _gdscript_opcode_declared_operand(const GDScriptFunction::OpcodeOperandList &p_operands, int p_index) {
	if (p_index < 0 || p_index >= p_operands.count) {
		return GDScriptFunction::OPERAND_NONE;
	}
	return GDScriptFunction::OpcodeOperandKind((p_operands.packed_kinds >> (p_index * 5)) & 0x1f);
}

const GDScriptFunction::OpcodeDescriptor &GDScriptFunction::get_opcode_descriptor(Opcode p_opcode) {
#define GDSCRIPT_OPERANDS(...) _gdscript_opcode_operands({ __VA_ARGS__ })
#define GDSCRIPT_OPCODE(m_name, m_size, m_operands, m_result, m_flow, m_types, m_relocation) \
	{ #m_name, m_size, m_operands, m_result, m_flow, m_types, m_relocation },
	static const OpcodeDescriptor descriptors[] = {
#include "gdscript_opcode.inc"
	};
#undef GDSCRIPT_OPCODE
#undef GDSCRIPT_OPERANDS
	static_assert(sizeof(descriptors) / sizeof(descriptors[0]) == OPCODE_COUNT, "Every GDScript opcode must have one descriptor.");
	ERR_FAIL_INDEX_V(int(p_opcode), OPCODE_COUNT, descriptors[OPCODE_END]);
	return descriptors[p_opcode];
}

int GDScriptFunction::get_instruction_size(const int *p_code, int p_code_size, int p_ip) {
	if (p_code == nullptr || p_ip < 0 || p_ip >= p_code_size || p_code[p_ip] < 0 || p_code[p_ip] >= OPCODE_COUNT) {
		return -1;
	}
	const OpcodeDescriptor &descriptor = get_opcode_descriptor(Opcode(p_code[p_ip]));
	if (descriptor.instruction_size > 0) {
		return descriptor.instruction_size <= p_code_size - p_ip ? descriptor.instruction_size : -1;
	}
	if (p_ip + 1 >= p_code_size || p_code[p_ip + 1] < 0 || descriptor.operand_kinds.count < 2 ||
			_gdscript_opcode_declared_operand(descriptor.operand_kinds, 0) != OPERAND_ARGUMENT_COUNT ||
			_gdscript_opcode_declared_operand(descriptor.operand_kinds, 1) != OPERAND_VARIADIC_FRAME_SLOTS) {
		return -1;
	}
	const int suffix_count = descriptor.operand_kinds.count - 2;
	const int64_t size = int64_t(2) + p_code[p_ip + 1] + suffix_count;
	return size <= p_code_size - p_ip ? int(size) : -1;
}

GDScriptFunction::OpcodeOperandKind GDScriptFunction::get_operand_kind(const int *p_code, int p_code_size, int p_ip, int p_word_offset) {
	const int instruction_size = get_instruction_size(p_code, p_code_size, p_ip);
	if (instruction_size < 0 || p_word_offset <= 0 || p_word_offset >= instruction_size) {
		return OPERAND_NONE;
	}
	const OpcodeDescriptor &descriptor = get_opcode_descriptor(Opcode(p_code[p_ip]));
	if (descriptor.instruction_size > 0) {
		return _gdscript_opcode_declared_operand(descriptor.operand_kinds, p_word_offset - 1);
	}
	const int argument_words = p_code[p_ip + 1];
	if (p_word_offset == 1) {
		return OPERAND_ARGUMENT_COUNT;
	}
	if (p_word_offset <= argument_words + 1) {
		return OPERAND_FRAME_SLOT;
	}
	return _gdscript_opcode_declared_operand(descriptor.operand_kinds, p_word_offset - argument_words);
}

int GDScriptFunction::get_result_operand(const int *p_code, int p_code_size, int p_ip) {
	if (get_instruction_size(p_code, p_code_size, p_ip) < 0) {
		return -1;
	}
	const OpcodeDescriptor &descriptor = get_opcode_descriptor(Opcode(p_code[p_ip]));
	if (descriptor.result_operand == OPCODE_RESULT_NONE) {
		return -1;
	}
	if (descriptor.result_operand > 0) {
		return descriptor.result_operand;
	}
	if (descriptor.instruction_size > 0) {
		return -1;
	}
	return 2 + p_code[p_ip + 1] + descriptor.result_operand;
}
