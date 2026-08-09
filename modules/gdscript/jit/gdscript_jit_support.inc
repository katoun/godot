/**************************************************************************/
/*  gdscript_jit_support.inc                                              */
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

#include "../vm/gdscript_function.h"

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

static bool is_unboxed_math_type(Variant::Type p_type) {
	return p_type == Variant::VECTOR2 || p_type == Variant::VECTOR3 || p_type == Variant::COLOR;
}

static bool is_unboxed_large_value_type(Variant::Type p_type) {
	return p_type == Variant::TRANSFORM2D || p_type == Variant::AABB || p_type == Variant::BASIS || p_type == Variant::TRANSFORM3D || p_type == Variant::PROJECTION;
}

static bool is_unboxed_struct_type(Variant::Type p_type) {
	return is_unboxed_math_type(p_type) || is_unboxed_large_value_type(p_type);
}

static bool is_unboxed_value_type(Variant::Type p_type) {
	return p_type == Variant::BOOL || p_type == Variant::INT || p_type == Variant::FLOAT || is_unboxed_struct_type(p_type);
}

static bool is_unboxed_user_struct_layout(const Ref<StructLayout> &p_layout) {
	if (p_layout.is_null() || !p_layout->is_finalized() || !p_layout->is_trivial()) {
		return false;
	}
	for (int i = 0; i < p_layout->get_field_count(); i++) {
		const StructLayout::Field &field = p_layout->get_field(i);
		if (field.type == Variant::STRUCT) {
			if (!is_unboxed_user_struct_layout(field.struct_layout)) {
				return false;
			}
		} else if (!is_unboxed_value_type(field.type)) {
			return false;
		}
	}
	return true;
}

static bool is_unboxed_native_type(Variant::Type p_type, const Ref<StructLayout> &p_layout = Ref<StructLayout>()) {
	return p_type == Variant::STRUCT ? is_unboxed_user_struct_layout(p_layout) : is_unboxed_value_type(p_type);
}

static int get_unboxed_value_size(Variant::Type p_type) {
	switch (p_type) {
		case Variant::BOOL:
			return sizeof(uint8_t);
		case Variant::INT:
			return sizeof(int64_t);
		case Variant::FLOAT:
			return sizeof(double);
		case Variant::VECTOR2:
			return sizeof(Vector2);
		case Variant::VECTOR3:
			return sizeof(Vector3);
		case Variant::COLOR:
			return sizeof(Color);
		case Variant::TRANSFORM2D:
			return sizeof(Transform2D);
		case Variant::AABB:
			return sizeof(AABB);
		case Variant::BASIS:
			return sizeof(Basis);
		case Variant::TRANSFORM3D:
			return sizeof(Transform3D);
		case Variant::PROJECTION:
			return sizeof(Projection);
		default:
			return 0;
	}
}

static int get_unboxed_value_alignment(Variant::Type p_type) {
	switch (p_type) {
		case Variant::VECTOR2:
			return alignof(Vector2);
		case Variant::VECTOR3:
			return alignof(Vector3);
		case Variant::COLOR:
			return alignof(Color);
		case Variant::TRANSFORM2D:
			return alignof(Transform2D);
		case Variant::AABB:
			return alignof(AABB);
		case Variant::BASIS:
			return alignof(Basis);
		case Variant::TRANSFORM3D:
			return alignof(Transform3D);
		case Variant::PROJECTION:
			return alignof(Projection);
		default:
			return alignof(uint64_t);
	}
}

static int get_unboxed_native_size(Variant::Type p_type, const Ref<StructLayout> &p_layout = Ref<StructLayout>()) {
	return p_type == Variant::STRUCT && p_layout.is_valid() ? p_layout->get_native_size() : get_unboxed_value_size(p_type);
}

static int get_unboxed_native_alignment(Variant::Type p_type, const Ref<StructLayout> &p_layout = Ref<StructLayout>()) {
	return p_type == Variant::STRUCT && p_layout.is_valid() ? p_layout->get_native_alignment() : get_unboxed_value_alignment(p_type);
}

static int align_native_offset(int p_offset, int p_alignment) {
	return (p_offset + p_alignment - 1) & ~(p_alignment - 1);
}

static int get_math_component_count(Variant::Type p_type) {
	switch (p_type) {
		case Variant::VECTOR2:
			return 2;
		case Variant::VECTOR3:
			return 3;
		case Variant::COLOR:
			return 4;
		default:
			return 0;
	}
}

static bool math_uses_double_components(Variant::Type p_type) {
#ifdef REAL_T_IS_DOUBLE
	return p_type != Variant::COLOR;
#else
	return false;
#endif
}

static int get_math_component_size(Variant::Type p_type) {
	return p_type == Variant::COLOR ? sizeof(float) : sizeof(real_t);
}

static constexpr int NATIVE_VALUE_ALIGNMENT = MAX(alignof(uint64_t), MAX(MAX(alignof(Transform2D), alignof(AABB)), MAX(MAX(alignof(Basis), alignof(Transform3D)), alignof(Projection))));
static constexpr int NATIVE_VALUE_MAX_SIZE = MAX(MAX(MAX(sizeof(Vector2), sizeof(Vector3)), MAX(sizeof(Color), sizeof(Transform2D))), MAX(MAX(sizeof(AABB), sizeof(Basis)), MAX(sizeof(Transform3D), sizeof(Projection))));
static constexpr int NATIVE_VALUE_STRIDE = (NATIVE_VALUE_MAX_SIZE + NATIVE_VALUE_ALIGNMENT - 1) & ~(NATIVE_VALUE_ALIGNMENT - 1);

static const void *get_unboxed_variant_data(const Variant *p_value, Variant::Type p_type) {
	switch (p_type) {
		case Variant::BOOL:
			return VariantInternal::get_bool(p_value);
		case Variant::INT:
			return VariantInternal::get_int(p_value);
		case Variant::FLOAT:
			return VariantInternal::get_float(p_value);
		case Variant::VECTOR2:
			return VariantInternal::get_vector2(p_value);
		case Variant::VECTOR3:
			return VariantInternal::get_vector3(p_value);
		case Variant::COLOR:
			return VariantInternal::get_color(p_value);
		case Variant::TRANSFORM2D:
			return VariantInternal::get_transform2d(p_value);
		case Variant::AABB:
			return VariantInternal::get_aabb(p_value);
		case Variant::BASIS:
			return VariantInternal::get_basis(p_value);
		case Variant::TRANSFORM3D:
			return VariantInternal::get_transform(p_value);
		case Variant::PROJECTION:
			return VariantInternal::get_projection(p_value);
		default:
			return nullptr;
	}
}

static bool get_struct_value(const Variant *p_value, StructValue &r_value) {
	if (p_value == nullptr || p_value->get_type() != Variant::STRUCT) {
		return false;
	}
	r_value = VariantInternalAccessor<StructValue>::get(p_value);
	return r_value.is_valid();
}

static void SLJIT_FUNC invoke_construct_struct(const StructLayout *p_layout, void *r_native_data) {
	DEV_ASSERT(p_layout != nullptr);
	p_layout->construct_native(r_native_data);
}

static void SLJIT_FUNC invoke_unbox_struct_variant(const Variant *p_value, const StructLayout *p_layout, void *r_native_data) {
	DEV_ASSERT(p_layout != nullptr);
	StructValue value;
	if (get_struct_value(p_value, value) && p_layout->is_compatible(value.get_layout())) {
		p_layout->unbox_native(value, r_native_data);
	}
}

static void SLJIT_FUNC invoke_store_struct_variant(Variant *p_destination, const void *p_native_data, const StructLayout *p_layout) {
	DEV_ASSERT(p_layout != nullptr);
	*p_destination = p_layout->box_native(p_native_data);
}

static sljit_sw SLJIT_FUNC invoke_equal_struct(const StructLayout *p_layout, const void *p_left, const void *p_right) {
	DEV_ASSERT(p_layout != nullptr);
	return p_layout->native_equal(p_left, p_right);
}

static void SLJIT_FUNC invoke_ptrcall(MethodBind *p_method, Object *p_instance, const void **p_arguments, void *r_return) {
	p_method->ptrcall(p_instance, p_arguments, r_return);
}

static double SLJIT_FUNC invoke_math_sqrt(double p_value) {
	return Math::sqrt(p_value);
}

static float SLJIT_FUNC invoke_math_sqrt_f32(float p_value) {
	return Math::sqrt(p_value);
}

static void SLJIT_FUNC invoke_store_large_variant(Variant *p_destination, const void *p_source, sljit_sw p_type) {
	const Variant::Type type = Variant::Type(p_type);
	if (p_destination->get_type() != type) {
		VariantInternal::initialize(p_destination, type);
	}
	switch (type) {
		case Variant::TRANSFORM2D:
			*VariantInternal::get_transform2d(p_destination) = *static_cast<const Transform2D *>(p_source);
			break;
		case Variant::AABB:
			*VariantInternal::get_aabb(p_destination) = *static_cast<const AABB *>(p_source);
			break;
		case Variant::BASIS:
			*VariantInternal::get_basis(p_destination) = *static_cast<const Basis *>(p_source);
			break;
		case Variant::TRANSFORM3D:
			*VariantInternal::get_transform(p_destination) = *static_cast<const Transform3D *>(p_source);
			break;
		case Variant::PROJECTION:
			*VariantInternal::get_projection(p_destination) = *static_cast<const Projection *>(p_source);
			break;
		default:
			break;
	}
}

#define EVALUATE_LARGE_SCALABLE_TYPE(m_type, m_variant_type) \
	case Variant::m_variant_type: { \
		const m_type &left = *static_cast<const m_type *>(p_left); \
		if (op == Variant::OP_EQUAL || op == Variant::OP_NOT_EQUAL) { \
			const bool equal = left == *static_cast<const m_type *>(p_right); \
			*static_cast<uint8_t *>(r_result) = op == Variant::OP_EQUAL ? equal : !equal; \
		} else if (right_type == Variant::m_variant_type) { \
			*static_cast<m_type *>(r_result) = left * *static_cast<const m_type *>(p_right); \
		} else if (right_type == Variant::INT) { \
			const int64_t scalar = *static_cast<const int64_t *>(p_right); \
			*static_cast<m_type *>(r_result) = op == Variant::OP_MULTIPLY ? left * scalar : left / scalar; \
		} else { \
			const double scalar = *static_cast<const double *>(p_right); \
			*static_cast<m_type *>(r_result) = op == Variant::OP_MULTIPLY ? left * scalar : left / scalar; \
		} \
	} break

static void SLJIT_FUNC invoke_large_math_operator(const void *p_left, const void *p_right, void *r_result, sljit_sw p_metadata) {
	const Variant::Operator op = GDScriptFunction::get_math_operator(p_metadata);
	const Variant::Type left_type = GDScriptFunction::get_math_left_type(p_metadata);
	const Variant::Type right_type = GDScriptFunction::get_math_right_type(p_metadata);
	switch (left_type) {
		EVALUATE_LARGE_SCALABLE_TYPE(Transform2D, TRANSFORM2D);
		EVALUATE_LARGE_SCALABLE_TYPE(Basis, BASIS);
		EVALUATE_LARGE_SCALABLE_TYPE(Transform3D, TRANSFORM3D);
		case Variant::AABB: {
			const bool equal = *static_cast<const AABB *>(p_left) == *static_cast<const AABB *>(p_right);
			*static_cast<uint8_t *>(r_result) = op == Variant::OP_EQUAL ? equal : !equal;
		} break;
		case Variant::PROJECTION: {
			const Projection &left = *static_cast<const Projection *>(p_left);
			if (op == Variant::OP_EQUAL || op == Variant::OP_NOT_EQUAL) {
				const bool equal = left == *static_cast<const Projection *>(p_right);
				*static_cast<uint8_t *>(r_result) = op == Variant::OP_EQUAL ? equal : !equal;
			} else {
				*static_cast<Projection *>(r_result) = left * *static_cast<const Projection *>(p_right);
			}
		} break;
		default:
			break;
	}
}

#undef EVALUATE_LARGE_SCALABLE_TYPE
