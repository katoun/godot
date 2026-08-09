/**************************************************************************/
/*  gdscript_baseline_jit_facade.inc                                      */
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

// Executable-code ownership and public typed/dynamic invocation facade.
} // namespace

GDScriptBaselineJIT::GDScriptBaselineJIT(void *p_entry_point, void *p_typed_entry_point, uint64_t p_code_size, const Vector<Variant::Type> &p_typed_argument_types, const Vector<Ref<StructLayout>> &p_typed_argument_struct_layouts, Variant::Type p_typed_return_type, const Ref<StructLayout> &p_typed_return_struct_layout, int p_ptrcall_count, bool p_requires_self, bool p_optimizing, int p_ssa_node_count, int p_eliminated_node_count, int p_scalar_replaced_math_value_count) :
		entry_point(p_entry_point), typed_entry_point(p_typed_entry_point), code_size(p_code_size), typed_argument_types(p_typed_argument_types), typed_argument_struct_layouts(p_typed_argument_struct_layouts), typed_return_type(p_typed_return_type), typed_return_struct_layout(p_typed_return_struct_layout), ptrcall_count(p_ptrcall_count), ssa_node_count(p_ssa_node_count), eliminated_node_count(p_eliminated_node_count), scalar_replaced_math_value_count(p_scalar_replaced_math_value_count), requires_self(p_requires_self), optimizing(p_optimizing) {
}

GDScriptBaselineJIT *GDScriptBaselineJIT::_compile(const GDScriptFunction *p_function, bool p_optimizing) {
	ERR_FAIL_NULL_V(p_function, nullptr);

	auto is_typed_abi_type = [](const GDScriptDataType &p_type) {
		return p_type.kind == GDScriptDataType::BUILTIN && is_unboxed_native_type(p_type.builtin_type, p_type.struct_layout);
	};
	Vector<Variant::Type> typed_argument_types;
	Vector<Ref<StructLayout>> typed_argument_struct_layouts;
	bool has_typed_signature = !p_function->is_vararg() && is_typed_abi_type(p_function->return_type);
	for (const GDScriptDataType &argument_type : p_function->argument_types) {
		Variant::Type builtin_type = is_typed_abi_type(argument_type) ? argument_type.builtin_type : Variant::NIL;
		typed_argument_types.push_back(builtin_type);
		typed_argument_struct_layouts.push_back(builtin_type == Variant::STRUCT ? argument_type.struct_layout : Ref<StructLayout>());
		has_typed_signature = has_typed_signature && builtin_type != Variant::NIL;
	}
	const Ref<StructLayout> typed_return_struct_layout = has_typed_signature && p_function->return_type.builtin_type == Variant::STRUCT ? p_function->return_type.struct_layout : Ref<StructLayout>();

	BaselineCompiler compiler(p_function->_code_ptr, p_function->_code_size, p_function->_stack_size, p_function->_constants_ptr, p_function->_constant_count, p_function->_methods_ptr, p_function->_methods_count, typed_argument_types, typed_argument_struct_layouts, Variant::NIL, Ref<StructLayout>(), false, p_optimizing);
	uint64_t generated_size = 0;
	void *generated_code = compiler.compile(generated_size);
	if (!generated_code) {
		return nullptr;
	}

	void *typed_generated_code = nullptr;
	if (has_typed_signature) {
		BaselineCompiler typed_compiler(p_function->_code_ptr, p_function->_code_size, p_function->_stack_size, p_function->_constants_ptr, p_function->_constant_count, p_function->_methods_ptr, p_function->_methods_count, typed_argument_types, typed_argument_struct_layouts, p_function->return_type.builtin_type, typed_return_struct_layout, true, p_optimizing);
		uint64_t typed_generated_size = 0;
		typed_generated_code = typed_compiler.compile(typed_generated_size);
		generated_size += typed_generated_size;
	}

	return memnew(GDScriptBaselineJIT(generated_code, typed_generated_code, generated_size, typed_argument_types, typed_argument_struct_layouts, has_typed_signature ? p_function->return_type.builtin_type : Variant::NIL, typed_return_struct_layout, compiler.get_ptrcall_count(), compiler.get_requires_self(), p_optimizing, compiler.get_ssa_node_count(), compiler.get_eliminated_node_count(), compiler.get_scalar_replaced_math_value_count()));
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

	const void **raw_arguments = reinterpret_cast<const void **>(alloca(MAX(1, p_argument_count) * sizeof(void *)));
	int native_argument_size = 0;
	int native_argument_alignment = 1;
	for (int i = 0; i < p_argument_count; i++) {
		if (typed_argument_types[i] == Variant::STRUCT) {
			const Ref<StructLayout> &layout = typed_argument_struct_layouts[i];
			native_argument_alignment = MAX(native_argument_alignment, int(layout->get_native_alignment()));
			native_argument_size = align_native_offset(native_argument_size, layout->get_native_alignment());
			native_argument_size += layout->get_native_size();
		}
	}
	uint8_t *native_argument_allocation = reinterpret_cast<uint8_t *>(alloca(MAX(1, native_argument_size + native_argument_alignment - 1)));
	uint8_t *native_argument_data = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(native_argument_allocation) + native_argument_alignment - 1) & ~(uintptr_t(native_argument_alignment) - 1));
	int native_argument_offset = 0;
	for (int i = 0; i < p_argument_count; i++) {
		if (p_arguments[i]->get_type() != typed_argument_types[i]) {
			return false;
		}
		if (typed_argument_types[i] == Variant::STRUCT) {
			const Ref<StructLayout> &layout = typed_argument_struct_layouts[i];
			StructValue value;
			if (!get_struct_value(p_arguments[i], value) || !layout->is_compatible(value.get_layout())) {
				return false;
			}
			native_argument_offset = align_native_offset(native_argument_offset, layout->get_native_alignment());
			void *destination = native_argument_data + native_argument_offset;
			if (layout->unbox_native(value, destination) != OK) {
				return false;
			}
			raw_arguments[i] = destination;
			native_argument_offset += layout->get_native_size();
			continue;
		}
		const void *source = get_unboxed_variant_data(p_arguments[i], typed_argument_types[i]);
		if (source == nullptr) {
			return false;
		}
		raw_arguments[i] = source;
	}

	const int return_size = get_unboxed_native_size(typed_return_type, typed_return_struct_layout);
	const int return_alignment = MAX(NATIVE_VALUE_ALIGNMENT, get_unboxed_native_alignment(typed_return_type, typed_return_struct_layout));
	uint8_t *raw_return_allocation = reinterpret_cast<uint8_t *>(alloca(MAX(1, return_size + return_alignment - 1)));
	uint8_t *raw_return = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(raw_return_allocation) + return_alignment - 1) & ~(uintptr_t(return_alignment) - 1));
	memset(raw_return, 0, return_size);
	typedef void(SLJIT_FUNC * TypedEntryPoint)(const void *const *, Object *, void *);
	reinterpret_cast<TypedEntryPoint>(typed_entry_point)(raw_arguments, p_self, raw_return);
	switch (typed_return_type) {
		case Variant::BOOL:
			r_return = bool(*reinterpret_cast<const uint8_t *>(raw_return));
			break;
		case Variant::INT:
			r_return = *reinterpret_cast<const int64_t *>(raw_return);
			break;
		case Variant::FLOAT:
			r_return = *reinterpret_cast<const double *>(raw_return);
			break;
		case Variant::VECTOR2:
			r_return = *reinterpret_cast<const Vector2 *>(raw_return);
			break;
		case Variant::VECTOR3:
			r_return = *reinterpret_cast<const Vector3 *>(raw_return);
			break;
		case Variant::COLOR:
			r_return = *reinterpret_cast<const Color *>(raw_return);
			break;
		case Variant::TRANSFORM2D:
			r_return = *reinterpret_cast<const Transform2D *>(raw_return);
			break;
		case Variant::AABB:
			r_return = *reinterpret_cast<const AABB *>(raw_return);
			break;
		case Variant::BASIS:
			r_return = *reinterpret_cast<const Basis *>(raw_return);
			break;
		case Variant::TRANSFORM3D:
			r_return = *reinterpret_cast<const Transform3D *>(raw_return);
			break;
		case Variant::PROJECTION:
			r_return = *reinterpret_cast<const Projection *>(raw_return);
			break;
		case Variant::STRUCT:
			r_return = typed_return_struct_layout->box_native(raw_return);
			break;
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
