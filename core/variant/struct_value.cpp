/**************************************************************************/
/*  struct_value.cpp                                                      */
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
/* "Software"), to deal in the Software without restriction, including  */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "struct_value.h"

#include "core/templates/hashfuncs.h"
#include "core/variant/variant_internal.h"

#define STRUCT_NATIVE_TYPE_INFO(m_variant_type, m_type) \
	case Variant::m_variant_type: { \
		r_size = sizeof(m_type); \
		r_alignment = alignof(m_type); \
		r_trivial = std::is_trivially_copyable_v<m_type> && std::is_trivially_destructible_v<m_type>; \
		return true; \
	}

bool StructLayout::_get_native_type_info(Variant::Type p_type, uint32_t &r_size, uint32_t &r_alignment, bool &r_trivial) {
	switch (p_type) {
		STRUCT_NATIVE_TYPE_INFO(BOOL, bool);
		STRUCT_NATIVE_TYPE_INFO(INT, int64_t);
		STRUCT_NATIVE_TYPE_INFO(FLOAT, double);
		STRUCT_NATIVE_TYPE_INFO(VECTOR2, Vector2);
		STRUCT_NATIVE_TYPE_INFO(VECTOR2I, Vector2i);
		STRUCT_NATIVE_TYPE_INFO(RECT2, Rect2);
		STRUCT_NATIVE_TYPE_INFO(RECT2I, Rect2i);
		STRUCT_NATIVE_TYPE_INFO(VECTOR3, Vector3);
		STRUCT_NATIVE_TYPE_INFO(VECTOR3I, Vector3i);
		STRUCT_NATIVE_TYPE_INFO(TRANSFORM2D, Transform2D);
		STRUCT_NATIVE_TYPE_INFO(VECTOR4, Vector4);
		STRUCT_NATIVE_TYPE_INFO(VECTOR4I, Vector4i);
		STRUCT_NATIVE_TYPE_INFO(PLANE, Plane);
		STRUCT_NATIVE_TYPE_INFO(QUATERNION, Quaternion);
		STRUCT_NATIVE_TYPE_INFO(AABB, AABB);
		STRUCT_NATIVE_TYPE_INFO(BASIS, Basis);
		STRUCT_NATIVE_TYPE_INFO(TRANSFORM3D, Transform3D);
		STRUCT_NATIVE_TYPE_INFO(PROJECTION, Projection);
		STRUCT_NATIVE_TYPE_INFO(COLOR, Color);
		default:
			r_size = sizeof(Variant);
			r_alignment = alignof(Variant);
			r_trivial = false;
			return true;
	}
}

#undef STRUCT_NATIVE_TYPE_INFO

const void *StructLayout::_get_trivial_variant_data(const Variant &p_value, Variant::Type p_type) {
	switch (p_type) {
		case Variant::BOOL:
			return VariantInternal::get_bool(&p_value);
		case Variant::INT:
			return VariantInternal::get_int(&p_value);
		case Variant::FLOAT:
			return VariantInternal::get_float(&p_value);
		case Variant::VECTOR2:
			return VariantInternal::get_vector2(&p_value);
		case Variant::VECTOR2I:
			return VariantInternal::get_vector2i(&p_value);
		case Variant::RECT2:
			return VariantInternal::get_rect2(&p_value);
		case Variant::RECT2I:
			return VariantInternal::get_rect2i(&p_value);
		case Variant::VECTOR3:
			return VariantInternal::get_vector3(&p_value);
		case Variant::VECTOR3I:
			return VariantInternal::get_vector3i(&p_value);
		case Variant::TRANSFORM2D:
			return VariantInternal::get_transform2d(&p_value);
		case Variant::VECTOR4:
			return VariantInternal::get_vector4(&p_value);
		case Variant::VECTOR4I:
			return VariantInternal::get_vector4i(&p_value);
		case Variant::PLANE:
			return VariantInternal::get_plane(&p_value);
		case Variant::QUATERNION:
			return VariantInternal::get_quaternion(&p_value);
		case Variant::AABB:
			return VariantInternal::get_aabb(&p_value);
		case Variant::BASIS:
			return VariantInternal::get_basis(&p_value);
		case Variant::TRANSFORM3D:
			return VariantInternal::get_transform(&p_value);
		case Variant::PROJECTION:
			return VariantInternal::get_projection(&p_value);
		case Variant::COLOR:
			return VariantInternal::get_color(&p_value);
		default:
			return nullptr;
	}
}

Variant StructLayout::_get_native_field_value(const Field &p_field, const void *p_native_data) {
	const uint8_t *field_data = static_cast<const uint8_t *>(p_native_data) + p_field.native_offset;
	if (p_field.type == Variant::STRUCT) {
		return p_field.struct_layout->box_native(field_data);
	}
	if (!p_field.trivial) {
		return *reinterpret_cast<const Variant *>(field_data);
	}

#define STRUCT_BOX_NATIVE(m_variant_type, m_type) \
	case Variant::m_variant_type: \
		return *reinterpret_cast<const m_type *>(field_data)

	switch (p_field.type) {
		STRUCT_BOX_NATIVE(BOOL, bool);
		STRUCT_BOX_NATIVE(INT, int64_t);
		STRUCT_BOX_NATIVE(FLOAT, double);
		STRUCT_BOX_NATIVE(VECTOR2, Vector2);
		STRUCT_BOX_NATIVE(VECTOR2I, Vector2i);
		STRUCT_BOX_NATIVE(RECT2, Rect2);
		STRUCT_BOX_NATIVE(RECT2I, Rect2i);
		STRUCT_BOX_NATIVE(VECTOR3, Vector3);
		STRUCT_BOX_NATIVE(VECTOR3I, Vector3i);
		STRUCT_BOX_NATIVE(TRANSFORM2D, Transform2D);
		STRUCT_BOX_NATIVE(VECTOR4, Vector4);
		STRUCT_BOX_NATIVE(VECTOR4I, Vector4i);
		STRUCT_BOX_NATIVE(PLANE, Plane);
		STRUCT_BOX_NATIVE(QUATERNION, Quaternion);
		STRUCT_BOX_NATIVE(AABB, AABB);
		STRUCT_BOX_NATIVE(BASIS, Basis);
		STRUCT_BOX_NATIVE(TRANSFORM3D, Transform3D);
		STRUCT_BOX_NATIVE(PROJECTION, Projection);
		STRUCT_BOX_NATIVE(COLOR, Color);
		default:
			return Variant();
	}

#undef STRUCT_BOX_NATIVE
}

Error StructLayout::_construct_native_from_values(void *r_native_data, const Vector<Variant> &p_values) const {
	ERR_FAIL_COND_V(!finalized, ERR_UNCONFIGURED);
	ERR_FAIL_NULL_V(r_native_data, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_values.size() != fields.size(), ERR_INVALID_PARAMETER);
	memset(r_native_data, 0, native_size);
	auto destroy_constructed_fields = [this, r_native_data](int p_count) {
		for (int i = p_count - 1; i >= 0; i--) {
			const Field &field = fields[i];
			void *field_data = static_cast<uint8_t *>(r_native_data) + field.native_offset;
			if (field.type == Variant::STRUCT) {
				field.struct_layout->destroy_native(field_data);
			} else if (!field.trivial) {
				reinterpret_cast<Variant *>(field_data)->~Variant();
			}
		}
	};
	for (int i = 0; i < fields.size(); i++) {
		const Field &field = fields[i];
		uint8_t *destination = static_cast<uint8_t *>(r_native_data) + field.native_offset;
		if (field.type == Variant::STRUCT) {
			const StructValue nested = p_values[i];
			const Error err = field.struct_layout->unbox_native(nested, destination);
			if (err != OK) {
				destroy_constructed_fields(i);
				return err;
			}
		} else if (field.trivial) {
			const void *source = _get_trivial_variant_data(p_values[i], field.type);
			if (source == nullptr) {
				destroy_constructed_fields(i);
				return ERR_INVALID_DATA;
			}
			memcpy(destination, source, field.native_size);
		} else {
			memnew_placement(destination, Variant(p_values[i]));
		}
	}
	return OK;
}

const StructLayout::Field &StructLayout::get_field(int p_index) const {
	static const Field empty_field;
	ERR_FAIL_INDEX_V(p_index, fields.size(), empty_field);
	return fields[p_index];
}

int StructLayout::find_field(const StringName &p_name) const {
	const int *index = field_indices.getptr(p_name);
	return index == nullptr ? -1 : *index;
}

Error StructLayout::add_field(const StringName &p_name, Variant::Type p_type, const Variant &p_default_value, const Ref<StructLayout> &p_struct_layout) {
	ERR_FAIL_COND_V(finalized, ERR_ALREADY_IN_USE);
	ERR_FAIL_COND_V(p_name.is_empty(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(field_indices.has(p_name), ERR_ALREADY_EXISTS);
	ERR_FAIL_INDEX_V(p_type, Variant::VARIANT_MAX, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_type == Variant::STRUCT && (p_struct_layout.is_null() || !p_struct_layout->is_finalized()), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_type != Variant::STRUCT && p_struct_layout.is_valid(), ERR_INVALID_PARAMETER);

	Field field;
	field.name = p_name;
	field.type = p_type;
	field.struct_layout = p_struct_layout;
	if (p_type == Variant::STRUCT) {
		field.native_size = p_struct_layout->get_native_size();
		field.native_alignment = p_struct_layout->get_native_alignment();
		field.trivial = p_struct_layout->is_trivial();
		field.default_value = p_default_value.get_type() == Variant::NIL ? Variant(StructValue(p_struct_layout)) : p_default_value;
		ERR_FAIL_COND_V(field.default_value.get_type() != Variant::STRUCT || !StructValue(field.default_value).get_layout()->is_compatible(p_struct_layout), ERR_INVALID_PARAMETER);
	} else {
		_get_native_type_info(p_type, field.native_size, field.native_alignment, field.trivial);
		if (p_default_value.get_type() == Variant::NIL && p_type != Variant::NIL && p_type != Variant::OBJECT) {
			Callable::CallError error;
			Variant::construct(p_type, field.default_value, nullptr, 0, error);
			ERR_FAIL_COND_V(error.error != Callable::CallError::CALL_OK, ERR_CANT_CREATE);
		} else {
			field.default_value = p_default_value;
		}
		ERR_FAIL_COND_V(p_type != Variant::NIL && field.default_value.get_type() != p_type && !(p_type == Variant::OBJECT && field.default_value.get_type() == Variant::NIL), ERR_INVALID_PARAMETER);
	}

	field_indices[p_name] = fields.size();
	fields.push_back(field);
	return OK;
}

Error StructLayout::finalize() {
	ERR_FAIL_COND_V(finalized, ERR_ALREADY_IN_USE);
	ERR_FAIL_COND_V(type_identifier.is_empty(), ERR_INVALID_PARAMETER);
	uint64_t offset = 0;
	native_alignment = 1;
	trivial = true;
	uint32_t hash_low = hash_murmur3_one_64(String(type_identifier).hash64());
	uint32_t hash_high = hash_murmur3_one_64(String(type_identifier).hash64(), 0x85ebca6b);
	auto hash_component = [&hash_low, &hash_high](uint64_t p_component) {
		hash_low = hash_murmur3_one_64(p_component, hash_low);
		hash_high = hash_murmur3_one_64(p_component, hash_high);
	};
	hash_component(schema_version);
	for (int i = 0; i < fields.size(); i++) {
		Field &field = fields.write[i];
		offset = (offset + field.native_alignment - 1) & ~uint64_t(field.native_alignment - 1);
		ERR_FAIL_COND_V(offset > UINT32_MAX, ERR_OUT_OF_MEMORY);
		field.native_offset = uint32_t(offset);
		offset += field.native_size;
		ERR_FAIL_COND_V(offset > UINT32_MAX, ERR_OUT_OF_MEMORY);
		native_alignment = MAX(native_alignment, field.native_alignment);
		trivial = trivial && field.trivial;
		hash_component(String(field.name).hash64());
		hash_component(field.type);
		if (field.type == Variant::STRUCT) {
			hash_component(field.struct_layout->get_schema_hash());
		}
	}
	offset = (offset + native_alignment - 1) & ~uint64_t(native_alignment - 1);
	ERR_FAIL_COND_V(offset > UINT32_MAX, ERR_OUT_OF_MEMORY);
	native_size = MAX(uint32_t(offset), uint32_t(1));
	schema_hash = (uint64_t(hash_fmix32(hash_high)) << 32) | hash_fmix32(hash_low);
	finalized = true;
	return OK;
}

bool StructLayout::is_compatible(const Ref<StructLayout> &p_other) const {
	return p_other.is_valid() && finalized && p_other->finalized && type_identifier == p_other->type_identifier && schema_version == p_other->schema_version && schema_hash == p_other->schema_hash;
}

Variant StructLayout::get_default_value(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, fields.size(), Variant());
	return fields[p_index].default_value;
}

String StructLayout::get_type_descriptor() const {
	ERR_FAIL_COND_V(!finalized, String());
	return String(type_identifier) + "@" + String::num_uint64(schema_version) + "#" + String::num_uint64(schema_hash, 16).pad_zeros(16);
}

Dictionary StructLayout::to_dictionary() const {
	ERR_FAIL_COND_V(!finalized, Dictionary());
	Dictionary result;
	result["type_identifier"] = String(type_identifier);
	result["schema_version"] = int64_t(schema_version);
	result["schema_fingerprint"] = String::num_uint64(schema_hash, 16).pad_zeros(16);

	Array serialized_fields;
	serialized_fields.resize(fields.size());
	for (int i = 0; i < fields.size(); i++) {
		const Field &field = fields[i];
		Dictionary serialized_field;
		serialized_field["name"] = String(field.name);
		serialized_field["type"] = int64_t(field.type);
		if (field.type == Variant::STRUCT) {
			serialized_field["struct_layout"] = field.struct_layout->to_dictionary();
		}
		serialized_fields[i] = serialized_field;
	}
	result["fields"] = serialized_fields;
	return result;
}

Ref<StructLayout> StructLayout::from_dictionary(const Dictionary &p_data, Error *r_error, int p_recursion_count) {
	if (r_error != nullptr) {
		*r_error = ERR_INVALID_DATA;
	}
	ERR_FAIL_COND_V(p_recursion_count > Variant::MAX_RECURSION_DEPTH, Ref<StructLayout>());
	ERR_FAIL_COND_V(!p_data.has("type_identifier") || !p_data.has("schema_version") || !p_data.has("fields"), Ref<StructLayout>());

	const Variant identifier_value = p_data["type_identifier"];
	const Variant version_value = p_data["schema_version"];
	const Variant fields_value = p_data["fields"];
	ERR_FAIL_COND_V(identifier_value.get_type() != Variant::STRING && identifier_value.get_type() != Variant::STRING_NAME, Ref<StructLayout>());
	ERR_FAIL_COND_V(version_value.get_type() != Variant::INT, Ref<StructLayout>());
	ERR_FAIL_COND_V(fields_value.get_type() != Variant::ARRAY, Ref<StructLayout>());
	const int64_t serialized_version = version_value;
	ERR_FAIL_COND_V(serialized_version < 0 || uint64_t(serialized_version) > UINT32_MAX, Ref<StructLayout>());

	Ref<StructLayout> result;
	result.instantiate(StringName(identifier_value), uint32_t(serialized_version));
	const Array serialized_fields = fields_value;
	for (int i = 0; i < serialized_fields.size(); i++) {
		const Variant field_value = serialized_fields[i];
		ERR_FAIL_COND_V(field_value.get_type() != Variant::DICTIONARY, Ref<StructLayout>());
		const Dictionary serialized_field = field_value;
		ERR_FAIL_COND_V(!serialized_field.has("name") || !serialized_field.has("type"), Ref<StructLayout>());

		const Variant name_value = serialized_field["name"];
		const Variant type_value = serialized_field["type"];
		ERR_FAIL_COND_V(name_value.get_type() != Variant::STRING && name_value.get_type() != Variant::STRING_NAME, Ref<StructLayout>());
		ERR_FAIL_COND_V(type_value.get_type() != Variant::INT, Ref<StructLayout>());
		const int64_t serialized_type = type_value;
		ERR_FAIL_COND_V(serialized_type < 0 || serialized_type >= Variant::VARIANT_MAX, Ref<StructLayout>());

		Ref<StructLayout> nested_layout;
		if (serialized_type == Variant::STRUCT) {
			ERR_FAIL_COND_V(!serialized_field.has("struct_layout") || serialized_field["struct_layout"].get_type() != Variant::DICTIONARY, Ref<StructLayout>());
			Error nested_error = OK;
			nested_layout = from_dictionary(serialized_field["struct_layout"], &nested_error, p_recursion_count + 1);
			ERR_FAIL_COND_V(nested_error != OK || nested_layout.is_null(), Ref<StructLayout>());
		}
		const Variant default_value = serialized_field.get("default_value", Variant()); // Compatibility with manifests emitted before schema descriptors stopped carrying defaults.
		const Error field_error = result->add_field(StringName(name_value), Variant::Type(serialized_type), default_value, nested_layout);
		ERR_FAIL_COND_V(field_error != OK, Ref<StructLayout>());
	}

	ERR_FAIL_COND_V(result->finalize() != OK, Ref<StructLayout>());
	const StringName fingerprint_key = p_data.has("schema_fingerprint") ? SNAME("schema_fingerprint") : SNAME("schema_hash");
	if (p_data.has(fingerprint_key)) {
		const Variant schema_hash_value = p_data[fingerprint_key];
		if (fingerprint_key == SNAME("schema_fingerprint")) {
			ERR_FAIL_COND_V(schema_hash_value.get_type() != Variant::STRING && schema_hash_value.get_type() != Variant::STRING_NAME, Ref<StructLayout>());
			ERR_FAIL_COND_V(String(schema_hash_value).to_lower() != String::num_uint64(result->get_schema_fingerprint(), 16).pad_zeros(16), Ref<StructLayout>());
		} else {
			ERR_FAIL_COND_V(schema_hash_value.get_type() != Variant::INT || uint64_t(int64_t(schema_hash_value)) != result->get_schema_hash(), Ref<StructLayout>());
		}
	}
	if (r_error != nullptr) {
		*r_error = OK;
	}
	return result;
}

Error StructLayout::_default_construct(const StructLayout &p_layout, void *r_native_data) {
	ERR_FAIL_NULL_V(r_native_data, ERR_INVALID_PARAMETER);
	Vector<Variant> defaults;
	defaults.resize(p_layout.fields.size());
	for (int i = 0; i < p_layout.fields.size(); i++) {
		defaults.write[i] = p_layout.fields[i].default_value;
	}
	return p_layout._construct_native_from_values(r_native_data, defaults);
}

void StructLayout::_default_copy(const StructLayout &p_layout, void *r_native_data, const void *p_native_data) {
	ERR_FAIL_COND(!p_layout.finalized);
	ERR_FAIL_NULL(r_native_data);
	ERR_FAIL_NULL(p_native_data);
	if (p_layout.trivial) {
		memcpy(r_native_data, p_native_data, p_layout.native_size);
		return;
	}
	memset(r_native_data, 0, p_layout.native_size);
	for (const Field &field : p_layout.fields) {
		void *destination = static_cast<uint8_t *>(r_native_data) + field.native_offset;
		const void *source = static_cast<const uint8_t *>(p_native_data) + field.native_offset;
		if (field.type == Variant::STRUCT) {
			field.struct_layout->copy_native(destination, source);
		} else if (field.trivial) {
			memcpy(destination, source, field.native_size);
		} else {
			memnew_placement(destination, Variant(*reinterpret_cast<const Variant *>(source)));
		}
	}
}

void StructLayout::_default_destroy(const StructLayout &p_layout, void *p_native_data) {
	ERR_FAIL_COND(!p_layout.finalized);
	ERR_FAIL_NULL(p_native_data);
	if (p_layout.trivial) {
		return;
	}
	for (int i = p_layout.fields.size() - 1; i >= 0; i--) {
		const Field &field = p_layout.fields[i];
		void *field_data = static_cast<uint8_t *>(p_native_data) + field.native_offset;
		if (field.type == Variant::STRUCT) {
			field.struct_layout->destroy_native(field_data);
		} else if (!field.trivial) {
			reinterpret_cast<Variant *>(field_data)->~Variant();
		}
	}
}

bool StructLayout::_default_equal(const StructLayout &p_layout, const void *p_left, const void *p_right) {
	ERR_FAIL_COND_V(!p_layout.finalized, false);
	ERR_FAIL_NULL_V(p_left, false);
	ERR_FAIL_NULL_V(p_right, false);
	for (const Field &field : p_layout.fields) {
		if (_get_native_field_value(field, p_left) != _get_native_field_value(field, p_right)) {
			return false;
		}
	}
	return true;
}

uint32_t StructLayout::_default_hash(const StructLayout &p_layout, const void *p_native_data) {
	ERR_FAIL_COND_V(!p_layout.finalized, 0);
	ERR_FAIL_NULL_V(p_native_data, 0);
	uint32_t hash = hash_murmur3_one_64(p_layout.schema_hash);
	for (const Field &field : p_layout.fields) {
		hash = hash_murmur3_one_32(_get_native_field_value(field, p_native_data).hash(), hash);
	}
	return hash_fmix32(hash);
}

Error StructLayout::construct_native(void *r_native_data) const {
	ERR_FAIL_NULL_V(operations.construct, ERR_UNCONFIGURED);
	return operations.construct(*this, r_native_data);
}

void StructLayout::copy_native(void *r_native_data, const void *p_native_data) const {
	ERR_FAIL_NULL(operations.copy);
	operations.copy(*this, r_native_data, p_native_data);
}

void StructLayout::destroy_native(void *p_native_data) const {
	ERR_FAIL_NULL(operations.destroy);
	operations.destroy(*this, p_native_data);
}

bool StructLayout::native_equal(const void *p_left, const void *p_right) const {
	ERR_FAIL_NULL_V(operations.equal, false);
	return operations.equal(*this, p_left, p_right);
}

uint32_t StructLayout::native_hash(const void *p_native_data) const {
	ERR_FAIL_NULL_V(operations.hash, 0);
	return operations.hash(*this, p_native_data);
}

StructValue StructLayout::box_native(const void *p_native_data) const {
	ERR_FAIL_COND_V(!finalized, StructValue());
	ERR_FAIL_NULL_V(p_native_data, StructValue());
	Ref<StructLayout> self = const_cast<StructLayout *>(this);
	StructValue result(self);
	for (int i = 0; i < fields.size(); i++) {
		result.set(i, _get_native_field_value(fields[i], p_native_data));
	}
	return result;
}

Error StructLayout::unbox_native(const StructValue &p_value, void *r_native_data) const {
	ERR_FAIL_NULL_V(r_native_data, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(!is_compatible(p_value.get_layout()), ERR_INVALID_DATA);
	Vector<Variant> values;
	values.resize(fields.size());
	for (int i = 0; i < fields.size(); i++) {
		values.write[i] = p_value.get(i);
	}
	return _construct_native_from_values(r_native_data, values);
}

StructLayout::StructLayout(const StringName &p_type_identifier, uint32_t p_schema_version) :
		type_identifier(p_type_identifier), schema_version(p_schema_version) {
	ERR_FAIL_COND(type_identifier.is_empty());
	operations.construct = _default_construct;
	operations.copy = _default_copy;
	operations.destroy = _default_destroy;
	operations.equal = _default_equal;
	operations.hash = _default_hash;
}

void StructValue::_ref(const StructValue &p_from) const {
	if (_data == p_from._data) {
		return;
	}
	_unref();
	if (p_from._data != nullptr && p_from._data->refcount.ref()) {
		_data = p_from._data;
	}
}

void StructValue::_unref() const {
	if (_data != nullptr && _data->refcount.unref()) {
		memdelete(_data);
	}
	_data = nullptr;
}

void StructValue::_detach() {
	ERR_FAIL_NULL(_data);
	if (_data->refcount.get() == 1) {
		return;
	}
	Data *detached = memnew(Data);
	detached->layout = _data->layout;
	detached->values = _data->values;
	_unref();
	_data = detached;
}

Ref<StructLayout> StructValue::get_layout() const {
	return _data == nullptr ? Ref<StructLayout>() : _data->layout;
}

int StructValue::get_field_count() const {
	return _data == nullptr ? 0 : _data->values.size();
}

Variant StructValue::get(int p_index) const {
	ERR_FAIL_NULL_V(_data, Variant());
	ERR_FAIL_INDEX_V(p_index, _data->values.size(), Variant());
	return _data->values[p_index];
}

Variant StructValue::get(const StringName &p_name) const {
	ERR_FAIL_NULL_V(_data, Variant());
	const int index = _data->layout->find_field(p_name);
	ERR_FAIL_COND_V(index < 0, Variant());
	return _data->values[index];
}

const Variant *StructValue::getptr(int p_index) const {
	ERR_FAIL_NULL_V(_data, nullptr);
	ERR_FAIL_INDEX_V(p_index, _data->values.size(), nullptr);
	return &_data->values[p_index];
}

const Variant *StructValue::getptr(const StringName &p_name) const {
	ERR_FAIL_NULL_V(_data, nullptr);
	const int index = _data->layout->find_field(p_name);
	return index < 0 ? nullptr : &_data->values[index];
}

Error StructValue::set(int p_index, const Variant &p_value) {
	ERR_FAIL_NULL_V(_data, ERR_UNCONFIGURED);
	ERR_FAIL_INDEX_V(p_index, _data->values.size(), ERR_INVALID_PARAMETER);
	const StructLayout::Field &field = _data->layout->get_field(p_index);
	if (field.type == Variant::STRUCT) {
		ERR_FAIL_COND_V(p_value.get_type() != Variant::STRUCT, ERR_INVALID_DATA);
		const StructValue nested = p_value;
		ERR_FAIL_COND_V(!field.struct_layout->is_compatible(nested.get_layout()), ERR_INVALID_DATA);
	} else if (field.type != Variant::NIL && p_value.get_type() != field.type && !(field.type == Variant::OBJECT && p_value.get_type() == Variant::NIL)) {
		return ERR_INVALID_DATA;
	}
	_detach();
	_data->values.write[p_index] = p_value;
	return OK;
}

Error StructValue::set(const StringName &p_name, const Variant &p_value) {
	ERR_FAIL_NULL_V(_data, ERR_UNCONFIGURED);
	const int index = _data->layout->find_field(p_name);
	ERR_FAIL_COND_V(index < 0, ERR_DOES_NOT_EXIST);
	return set(index, p_value);
}

bool StructValue::has_field(const StringName &p_name) const {
	return _data != nullptr && _data->layout->find_field(p_name) >= 0;
}

StructValue StructValue::duplicate() const {
	if (_data == nullptr) {
		return StructValue();
	}
	StructValue result(_data->layout);
	result._data->values = _data->values;
	return result;
}

bool StructValue::operator==(const StructValue &p_other) const {
	return recursive_equal(p_other, 0);
}

bool StructValue::recursive_equal(const StructValue &p_other, int p_recursion_count, bool p_semantic_comparison) const {
	if (_data == p_other._data) {
		return true;
	}
	if (_data == nullptr || p_other._data == nullptr || !_data->layout->is_compatible(p_other._data->layout) || _data->values.size() != p_other._data->values.size()) {
		return false;
	}
	if (p_recursion_count > Variant::MAX_RECURSION_DEPTH) {
		ERR_PRINT("Max StructValue recursion reached");
		return true;
	}
	p_recursion_count++;
	for (int i = 0; i < _data->values.size(); i++) {
		if (!_data->values[i].hash_compare(p_other._data->values[i], p_recursion_count, p_semantic_comparison)) {
			return false;
		}
	}
	return true;
}

uint32_t StructValue::hash() const {
	return recursive_hash(0);
}

uint32_t StructValue::recursive_hash(int p_recursion_count) const {
	if (_data == nullptr) {
		return 0;
	}
	if (p_recursion_count > Variant::MAX_RECURSION_DEPTH) {
		ERR_PRINT("Max StructValue recursion reached");
		return 0;
	}
	uint32_t hash_value = hash_murmur3_one_64(_data->layout->get_schema_hash());
	p_recursion_count++;
	for (const Variant &value : _data->values) {
		hash_value = hash_murmur3_one_32(value.recursive_hash(p_recursion_count), hash_value);
	}
	return hash_fmix32(hash_value);
}

Dictionary StructValue::to_dictionary() const {
	if (_data == nullptr) {
		return Dictionary();
	}
	Dictionary result;
	result["layout"] = _data->layout->to_dictionary();
	Dictionary serialized_fields;
	for (int i = 0; i < _data->values.size(); i++) {
		serialized_fields[_data->layout->get_field(i).name] = _data->values[i];
	}
	result["fields"] = serialized_fields;
	return result;
}

StructValue StructValue::from_dictionary(const Dictionary &p_data, Error *r_error, int p_recursion_count) {
	if (r_error != nullptr) {
		*r_error = ERR_INVALID_DATA;
	}
	if (p_data.is_empty()) {
		if (r_error != nullptr) {
			*r_error = OK;
		}
		return StructValue();
	}
	ERR_FAIL_COND_V(p_recursion_count > Variant::MAX_RECURSION_DEPTH, StructValue());
	ERR_FAIL_COND_V(!p_data.has("layout") || (!p_data.has("fields") && !p_data.has("values")), StructValue());
	ERR_FAIL_COND_V(p_data["layout"].get_type() != Variant::DICTIONARY, StructValue());

	Error layout_error = OK;
	Ref<StructLayout> layout = StructLayout::from_dictionary(p_data["layout"], &layout_error, p_recursion_count + 1);
	ERR_FAIL_COND_V(layout_error != OK || layout.is_null(), StructValue());
	StructValue result(layout);
	if (p_data.has("fields")) {
		ERR_FAIL_COND_V(p_data["fields"].get_type() != Variant::DICTIONARY, StructValue());
		const Dictionary serialized_fields = p_data["fields"];
		ERR_FAIL_COND_V(serialized_fields.size() != layout->get_field_count(), StructValue());
		for (int i = 0; i < layout->get_field_count(); i++) {
			const StringName field_name = layout->get_field(i).name;
			ERR_FAIL_COND_V(!serialized_fields.has(field_name), StructValue());
			ERR_FAIL_COND_V(result.set(i, serialized_fields[field_name]) != OK, StructValue());
		}
	} else {
		// Compatibility with the initial positional wire representation.
		ERR_FAIL_COND_V(p_data["values"].get_type() != Variant::ARRAY, StructValue());
		const Array serialized_values = p_data["values"];
		ERR_FAIL_COND_V(serialized_values.size() != layout->get_field_count(), StructValue());
		for (int i = 0; i < serialized_values.size(); i++) {
			ERR_FAIL_COND_V(result.set(i, serialized_values[i]) != OK, StructValue());
		}
	}
	if (r_error != nullptr) {
		*r_error = OK;
	}
	return result;
}

void StructValue::operator=(const StructValue &p_other) {
	_ref(p_other);
}

StructValue::StructValue(const Dictionary &p_data) {
	*this = from_dictionary(p_data);
}

StructValue::StructValue(const Ref<StructLayout> &p_layout) {
	ERR_FAIL_COND(p_layout.is_null() || !p_layout->is_finalized());
	_data = memnew(Data);
	_data->layout = p_layout;
	_data->values.resize(p_layout->get_field_count());
	for (int i = 0; i < _data->values.size(); i++) {
		_data->values.write[i] = p_layout->get_default_value(i);
	}
}

StructValue::StructValue(const StructValue &p_other) {
	_ref(p_other);
}

StructValue::~StructValue() {
	_unref();
}
