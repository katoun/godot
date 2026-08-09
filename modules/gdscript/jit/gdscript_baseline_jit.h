/**************************************************************************/
/*  gdscript_baseline_jit.h                                               */
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

#pragma once

#include "core/typedefs.h"
#include "core/templates/vector.h"
#include "core/variant/struct_value.h"
#include "core/variant/variant.h"

class GDScriptFunction;
class Object;

class GDScriptBaselineJIT {
	void *entry_point = nullptr;
	void *typed_entry_point = nullptr;
	uint64_t code_size = 0;
	Vector<Variant::Type> typed_argument_types;
	Vector<Ref<StructLayout>> typed_argument_struct_layouts;
	Variant::Type typed_return_type = Variant::NIL;
	Ref<StructLayout> typed_return_struct_layout;
	int ptrcall_count = 0;
	int ssa_node_count = 0;
	int eliminated_node_count = 0;
	int scalar_replaced_math_value_count = 0;
	bool requires_self = false;
	bool optimizing = false;

	GDScriptBaselineJIT(void *p_entry_point, void *p_typed_entry_point, uint64_t p_code_size, const Vector<Variant::Type> &p_typed_argument_types, const Vector<Ref<StructLayout>> &p_typed_argument_struct_layouts, Variant::Type p_typed_return_type, const Ref<StructLayout> &p_typed_return_struct_layout, int p_ptrcall_count, bool p_requires_self, bool p_optimizing, int p_ssa_node_count, int p_eliminated_node_count, int p_scalar_replaced_math_value_count);
	static GDScriptBaselineJIT *_compile(const GDScriptFunction *p_function, bool p_optimizing);

public:
	static constexpr uint32_t OPTIMIZING_CALL_THRESHOLD = 64;

	static GDScriptBaselineJIT *compile(const GDScriptFunction *p_function);
	static GDScriptBaselineJIT *compile_optimized(const GDScriptFunction *p_function);

	Variant *execute(Variant **p_variant_addresses, Object *p_self) const;
	bool execute_typed(const Variant **p_arguments, int p_argument_count, Object *p_self, Variant &r_return) const;
	bool has_typed_entry() const { return typed_entry_point != nullptr; }
	bool has_ptrcalls() const { return ptrcall_count > 0; }
	int get_ptrcall_count() const { return ptrcall_count; }
	bool is_optimizing() const { return optimizing; }
	int get_ssa_node_count() const { return ssa_node_count; }
	int get_eliminated_node_count() const { return eliminated_node_count; }
	int get_scalar_replaced_math_value_count() const { return scalar_replaced_math_value_count; }
	uint64_t get_code_size() const { return code_size; }

	~GDScriptBaselineJIT();
};
