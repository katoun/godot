/**************************************************************************/
/*  struct_value.h                                                        */
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

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/variant.h"

class StructValue;

class StructLayout : public RefCounted {
public:
	struct Operations {
		Error (*construct)(const StructLayout &p_layout, void *r_native_data) = nullptr;
		void (*copy)(const StructLayout &p_layout, void *r_native_data, const void *p_native_data) = nullptr;
		void (*destroy)(const StructLayout &p_layout, void *p_native_data) = nullptr;
		bool (*equal)(const StructLayout &p_layout, const void *p_left, const void *p_right) = nullptr;
		uint32_t (*hash)(const StructLayout &p_layout, const void *p_native_data) = nullptr;
	};

	struct Field {
		StringName name;
		Variant::Type type = Variant::NIL;
		Ref<StructLayout> struct_layout;
		Variant default_value;
		uint32_t native_offset = 0;
		uint32_t native_size = 0;
		uint32_t native_alignment = 1;
		bool trivial = false;
	};

private:
	StringName type_identifier;
	uint32_t schema_version = 0;
	uint64_t schema_hash = 0;
	Vector<Field> fields;
	HashMap<StringName, int> field_indices;
	uint32_t native_size = 0;
	uint32_t native_alignment = 1;
	bool trivial = true;
	bool finalized = false;
	Operations operations;

	static bool _get_native_type_info(Variant::Type p_type, uint32_t &r_size, uint32_t &r_alignment, bool &r_trivial);
	static const void *_get_trivial_variant_data(const Variant &p_value, Variant::Type p_type);
	static Variant _get_native_field_value(const Field &p_field, const void *p_native_data);
	Error _construct_native_from_values(void *r_native_data, const Vector<Variant> &p_values) const;
	static Error _default_construct(const StructLayout &p_layout, void *r_native_data);
	static void _default_copy(const StructLayout &p_layout, void *r_native_data, const void *p_native_data);
	static void _default_destroy(const StructLayout &p_layout, void *p_native_data);
	static bool _default_equal(const StructLayout &p_layout, const void *p_left, const void *p_right);
	static uint32_t _default_hash(const StructLayout &p_layout, const void *p_native_data);

public:
	StringName get_type_identifier() const { return type_identifier; }
	uint32_t get_schema_version() const { return schema_version; }
	uint64_t get_schema_hash() const { return schema_hash; }
	int get_field_count() const { return fields.size(); }
	const Field &get_field(int p_index) const;
	int find_field(const StringName &p_name) const;
	uint32_t get_native_size() const { return native_size; }
	uint32_t get_native_alignment() const { return native_alignment; }
	bool is_trivial() const { return trivial; }
	bool is_finalized() const { return finalized; }
	const Operations &get_operations() const { return operations; }

	Error add_field(const StringName &p_name, Variant::Type p_type, const Variant &p_default_value = Variant(), const Ref<StructLayout> &p_struct_layout = Ref<StructLayout>());
	Error finalize();
	bool is_compatible(const Ref<StructLayout> &p_other) const;
	Variant get_default_value(int p_index) const;
	Dictionary to_dictionary() const;
	static Ref<StructLayout> from_dictionary(const Dictionary &p_data, Error *r_error = nullptr, int p_recursion_count = 0);

	Error construct_native(void *r_native_data) const;
	void copy_native(void *r_native_data, const void *p_native_data) const;
	void destroy_native(void *p_native_data) const;
	bool native_equal(const void *p_left, const void *p_right) const;
	uint32_t native_hash(const void *p_native_data) const;
	StructValue box_native(const void *p_native_data) const;
	Error unbox_native(const StructValue &p_value, void *r_native_data) const;

	StructLayout(const StringName &p_type_identifier, uint32_t p_schema_version);
};

class StructValue {
	struct Data {
		SafeRefCount refcount;
		Ref<StructLayout> layout;
		Vector<Variant> values;

		Data() { refcount.init(); }
	};

	mutable Data *_data = nullptr;

	void _ref(const StructValue &p_from) const;
	void _unref() const;
	void _detach();

public:
	bool is_valid() const { return _data != nullptr && _data->layout.is_valid(); }
	Ref<StructLayout> get_layout() const;
	int get_field_count() const;
	Variant get(int p_index) const;
	Variant get(const StringName &p_name) const;
	const Variant *getptr(int p_index) const;
	const Variant *getptr(const StringName &p_name) const;
	Error set(int p_index, const Variant &p_value);
	Error set(const StringName &p_name, const Variant &p_value);
	bool has_field(const StringName &p_name) const;
	StructValue duplicate() const;
	bool is_same_instance(const StructValue &p_other) const { return _data == p_other._data; }
	bool operator==(const StructValue &p_other) const;
	bool operator!=(const StructValue &p_other) const { return !(*this == p_other); }
	uint32_t hash() const;
	uint32_t recursive_hash(int p_recursion_count) const;
	bool recursive_equal(const StructValue &p_other, int p_recursion_count, bool p_semantic_comparison = true) const;
	const void *id() const { return _data; }
	Dictionary to_dictionary() const;
	static StructValue from_dictionary(const Dictionary &p_data, Error *r_error = nullptr, int p_recursion_count = 0);

	void operator=(const StructValue &p_other);
	StructValue(const Dictionary &p_data);
	StructValue(const Ref<StructLayout> &p_layout);
	StructValue(const StructValue &p_other);
	StructValue() = default;
	~StructValue();
};
