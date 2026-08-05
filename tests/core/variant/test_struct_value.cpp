/**************************************************************************/
/*  test_struct_value.cpp                                                 */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_struct_value)

#include "core/debugger/debugger_marshalls.h"
#include "core/io/json.h"
#include "core/io/marshalls.h"
#include "core/variant/container_type_validate.h"
#include "core/variant/struct_value.h"
#include "core/variant/variant_parser.h"
#include "scene/main/multiplayer_api.h"

namespace TestStructValue {

static Ref<StructLayout> make_projectile_layout(uint32_t p_version = 1) {
	Ref<StructLayout> layout;
	layout.instantiate(SNAME("test.ProjectileState"), p_version);
	ERR_FAIL_COND_V(layout->add_field(SNAME("position"), Variant::VECTOR3, Vector3(1, 2, 3)) != OK, Ref<StructLayout>());
	ERR_FAIL_COND_V(layout->add_field(SNAME("velocity"), Variant::VECTOR3, Vector3(4, 5, 6)) != OK, Ref<StructLayout>());
	ERR_FAIL_COND_V(layout->add_field(SNAME("lifetime"), Variant::FLOAT, 2.5) != OK, Ref<StructLayout>());
	ERR_FAIL_COND_V(layout->finalize() != OK, Ref<StructLayout>());
	return layout;
}

static void *get_aligned_storage(Vector<uint8_t> &r_storage, const Ref<StructLayout> &p_layout) {
	r_storage.resize(p_layout->get_native_size() + p_layout->get_native_alignment() - 1);
	const uintptr_t unaligned = reinterpret_cast<uintptr_t>(r_storage.ptrw());
	const uintptr_t aligned = (unaligned + p_layout->get_native_alignment() - 1) & ~uintptr_t(p_layout->get_native_alignment() - 1);
	return reinterpret_cast<void *>(aligned);
}

TEST_CASE("[Core][Variant][StructValue] Layout metadata is stable and preserves existing Variant type numbers") {
	CHECK(int(Variant::PACKED_VECTOR4_ARRAY) == 38);
	CHECK(int(Variant::STRUCT) == 39);

	const Ref<StructLayout> layout = make_projectile_layout();
	REQUIRE(layout.is_valid());
	CHECK(layout->get_type_identifier() == SNAME("test.ProjectileState"));
	CHECK(layout->get_schema_version() == 1);
	CHECK(layout->get_schema_fingerprint() == layout->get_schema_hash());
	CHECK(layout->get_type_descriptor().begins_with("test.ProjectileState@1#"));
	CHECK(layout->get_field_count() == 3);
	CHECK(layout->get_field(0).name == SNAME("position"));
	CHECK(layout->get_field(1).type == Variant::VECTOR3);
	CHECK(layout->get_field(2).type == Variant::FLOAT);
	CHECK(layout->get_field(0).native_offset < layout->get_field(1).native_offset);
	CHECK(layout->get_field(1).native_offset < layout->get_field(2).native_offset);
	CHECK(layout->get_native_size() % layout->get_native_alignment() == 0);
	CHECK(layout->is_trivial());

	const StructLayout::Operations &operations = layout->get_operations();
	CHECK(operations.construct != nullptr);
	CHECK(operations.copy != nullptr);
	CHECK(operations.destroy != nullptr);
	CHECK(operations.equal != nullptr);
	CHECK(operations.hash != nullptr);

	const Ref<StructLayout> identical_layout = make_projectile_layout();
	REQUIRE(identical_layout.is_valid());
	CHECK(layout->get_schema_hash() == identical_layout->get_schema_hash());
	CHECK(layout->is_compatible(identical_layout));

	const Ref<StructLayout> newer_layout = make_projectile_layout(2);
	REQUIRE(newer_layout.is_valid());
	CHECK_FALSE(layout->is_compatible(newer_layout));
}

TEST_CASE("[Core][Variant][StructValue] Boxed values detach on write") {
	const Ref<StructLayout> layout = make_projectile_layout();
	REQUIRE(layout.is_valid());

	StructValue value(layout);
	REQUIRE(value.set(SNAME("lifetime"), 4.0) == OK);
	Variant original = value;
	Variant copy = original;
	CHECK(StructValue(original).is_same_instance(StructValue(copy)));

	bool valid = false;
	copy.set_named(SNAME("lifetime"), 9.0, valid);
	REQUIRE(valid);
	const StructValue original_value = original;
	const StructValue copied_value = copy;
	CHECK_FALSE(original_value.is_same_instance(copied_value));
	CHECK(double(original_value.get(SNAME("lifetime"))) == 4.0);
	CHECK(double(copied_value.get(SNAME("lifetime"))) == 9.0);
	CHECK(original != copy);
	CHECK(original.hash() != copy.hash());

	StructValue equivalent(layout);
	REQUIRE(equivalent.set(SNAME("lifetime"), 4.0) == OK);
	CHECK(original == Variant(equivalent));
	CHECK(original.hash() == Variant(equivalent).hash());

	Dictionary keys;
	keys[original] = "found";
	CHECK(keys.get(equivalent, Variant()) == Variant("found"));
}

TEST_CASE("[Core][Variant][StructValue] Native storage uses layout operations") {
	const Ref<StructLayout> layout = make_projectile_layout();
	REQUIRE(layout.is_valid());

	Vector<uint8_t> first_storage;
	Vector<uint8_t> second_storage;
	void *first = get_aligned_storage(first_storage, layout);
	void *second = get_aligned_storage(second_storage, layout);
	REQUIRE(layout->construct_native(first) == OK);

	const Vector3 *native_position = reinterpret_cast<const Vector3 *>(static_cast<const uint8_t *>(first) + layout->get_field(0).native_offset);
	CHECK(*native_position == Vector3(1, 2, 3));
	layout->copy_native(second, first);
	CHECK(layout->native_equal(first, second));
	CHECK(layout->native_hash(first) == layout->native_hash(second));

	const StructValue boxed = layout->box_native(first);
	CHECK(boxed.get(SNAME("velocity")) == Variant(Vector3(4, 5, 6)));
	layout->destroy_native(second);
	layout->destroy_native(first);

	Ref<StructLayout> nontrivial;
	nontrivial.instantiate(SNAME("test.NamedState"), 1);
	REQUIRE(nontrivial->add_field(SNAME("name"), Variant::STRING, "Godot") == OK);
	REQUIRE(nontrivial->finalize() == OK);
	CHECK_FALSE(nontrivial->is_trivial());

	Vector<uint8_t> nontrivial_storage;
	void *native = get_aligned_storage(nontrivial_storage, nontrivial);
	REQUIRE(nontrivial->construct_native(native) == OK);
	const Variant *native_name = reinterpret_cast<const Variant *>(static_cast<const uint8_t *>(native) + nontrivial->get_field(0).native_offset);
	CHECK(*native_name == Variant("Godot"));
	nontrivial->destroy_native(native);
}

TEST_CASE("[Core][Variant][StructValue] Nested trivial layouts stay inline") {
	Ref<StructLayout> coordinates;
	coordinates.instantiate(SNAME("test.Coordinates"), 1);
	REQUIRE(coordinates->add_field(SNAME("x"), Variant::FLOAT, 12.0) == OK);
	REQUIRE(coordinates->add_field(SNAME("y"), Variant::FLOAT, 24.0) == OK);
	REQUIRE(coordinates->finalize() == OK);

	Ref<StructLayout> vertex;
	vertex.instantiate(SNAME("test.Vertex"), 1);
	REQUIRE(vertex->add_field(SNAME("coordinates"), Variant::STRUCT, Variant(), coordinates) == OK);
	REQUIRE(vertex->add_field(SNAME("enabled"), Variant::BOOL, true) == OK);
	REQUIRE(vertex->finalize() == OK);
	CHECK(vertex->is_trivial());
	CHECK(vertex->get_field(0).native_size == coordinates->get_native_size());

	Vector<uint8_t> storage;
	void *native = get_aligned_storage(storage, vertex);
	REQUIRE(vertex->construct_native(native) == OK);
	const StructValue boxed = vertex->box_native(native);
	const StructValue boxed_coordinates = boxed.get(SNAME("coordinates"));
	CHECK(double(boxed_coordinates.get(SNAME("x"))) == 12.0);
	CHECK(double(boxed_coordinates.get(SNAME("y"))) == 24.0);
	CHECK(bool(boxed.get(SNAME("enabled"))));
	List<PropertyInfo> properties;
	Variant(boxed).get_property_list(&properties);
	REQUIRE(properties.front() != nullptr);
	CHECK(properties.front()->get().class_name == coordinates->get_type_identifier());
	CHECK(properties.front()->get().hint_string == coordinates->get_type_descriptor());
	vertex->destroy_native(native);
}

TEST_CASE("[Core][Variant][StructValue] Schema manifests and Variant serializers round-trip") {
	const Ref<StructLayout> layout = make_projectile_layout();
	REQUIRE(layout.is_valid());
	StructValue value(layout);
	REQUIRE(value.set(SNAME("position"), Vector3(10, 20, 30)) == OK);
	REQUIRE(value.set(SNAME("lifetime"), 7.25) == OK);
	const Variant source = value;
	const Dictionary manifest = value.to_dictionary();
	REQUIRE(manifest.has("layout"));
	REQUIRE(manifest.has("fields"));
	CHECK_FALSE(manifest.has("values"));
	const Dictionary layout_manifest = manifest["layout"];
	CHECK(layout_manifest["type_identifier"] == Variant("test.ProjectileState"));
	CHECK(layout_manifest["schema_fingerprint"] == Variant(String::num_uint64(layout->get_schema_fingerprint(), 16).pad_zeros(16)));
	CHECK_FALSE(layout_manifest.has("schema_hash"));
	const Array field_layouts = layout_manifest["fields"];
	for (const Variant &field_layout_variant : field_layouts) {
		const Dictionary field_layout = field_layout_variant;
		CHECK_FALSE(field_layout.has("native_offset"));
		CHECK_FALSE(field_layout.has("native_size"));
		CHECK_FALSE(field_layout.has("native_alignment"));
		CHECK_FALSE(field_layout.has("default_value"));
	}
	const Dictionary named_fields = manifest["fields"];
	CHECK(named_fields["position"] == Variant(Vector3(10, 20, 30)));
	CHECK(named_fields["lifetime"] == Variant(7.25));

	Error manifest_error = OK;
	const StructValue manifest_copy = StructValue::from_dictionary(value.to_dictionary(), &manifest_error);
	REQUIRE(manifest_error == OK);
	CHECK(manifest_copy == value);
	CHECK(manifest_copy.get_layout()->is_compatible(layout));

	int encoded_size = 0;
	REQUIRE(encode_variant(source, nullptr, encoded_size) == OK);
	Vector<uint8_t> encoded;
	encoded.resize(encoded_size);
	int written_size = 0;
	REQUIRE(encode_variant(source, encoded.ptrw(), written_size) == OK);
	CHECK(written_size == encoded_size);
	Variant decoded;
	int decoded_size = 0;
	REQUIRE(decode_variant(decoded, encoded.ptr(), encoded.size(), &decoded_size) == OK);
	CHECK(decoded_size == encoded_size);
	CHECK(decoded == source);

	String text;
	REQUIRE(VariantWriter::write_to_string(source, text) == OK);
	CHECK(text.begins_with("StructValue("));
	VariantParser::StreamString stream;
	stream.s = text;
	String error_text;
	int error_line = 0;
	Variant parsed;
	const Error parse_error = VariantParser::parse(&stream, parsed, error_text, error_line);
	REQUIRE(parse_error == OK);
	CHECK(parsed == source);

	CHECK(DebuggerMarshalls::parse_type_from_variant(source) == layout->get_type_descriptor());

	int rpc_size = 0;
	REQUIRE(MultiplayerAPI::encode_and_compress_variant(source, nullptr, rpc_size, false) == OK);
	Vector<uint8_t> rpc_data;
	rpc_data.resize(rpc_size);
	int rpc_written = 0;
	REQUIRE(MultiplayerAPI::encode_and_compress_variant(source, rpc_data.ptrw(), rpc_written, false) == OK);
	CHECK(rpc_written == rpc_size);
	Variant rpc_decoded;
	int rpc_read = 0;
	REQUIRE(MultiplayerAPI::decode_and_decompress_variant(rpc_decoded, rpc_data.ptr(), rpc_data.size(), &rpc_read, false) == OK);
	CHECK(rpc_read == rpc_size);
	CHECK(rpc_decoded == source);
}

TEST_CASE("[Core][Variant][StructValue] Typed containers retain and enforce schema descriptors") {
	const Ref<StructLayout> layout = make_projectile_layout();
	REQUIRE(layout.is_valid());
	StructValue value(layout);
	REQUIRE(value.set(SNAME("lifetime"), 8.0) == OK);

	ContainerType struct_type;
	struct_type.builtin_type = Variant::STRUCT;
	struct_type.struct_layout = layout;
	Array array;
	array.set_typed(struct_type);
	array.push_back(value);
	REQUIRE(array.size() == 1);
	CHECK(array.get_element_type().struct_layout->is_compatible(layout));
	CHECK(StructValue(array.get_typed_type_descriptor()).get_layout()->is_compatible(layout));
	ContainerTypeValidate validator;
	validator.type = Variant::STRUCT;
	validator.struct_layout = layout;
	CHECK(validator.test_validate(value));
	const StructValue incompatible_value(make_projectile_layout(2));
	CHECK_FALSE(validator.test_validate(incompatible_value));
	Variant concatenated;
	bool concatenation_valid = false;
	Variant::evaluate(Variant::OP_ADD, array, array, concatenated, concatenation_valid);
	REQUIRE(concatenation_valid);
	const Array concatenated_array = concatenated;
	CHECK(concatenated_array.is_same_typed(array));

	int encoded_size = 0;
	REQUIRE(encode_variant(array, nullptr, encoded_size) == OK);
	Vector<uint8_t> encoded;
	encoded.resize(encoded_size);
	int written_size = 0;
	REQUIRE(encode_variant(array, encoded.ptrw(), written_size) == OK);
	Variant decoded_variant;
	REQUIRE(decode_variant(decoded_variant, encoded.ptr(), encoded.size()) == OK);
	const Array decoded_array = decoded_variant;
	CHECK(decoded_array.is_same_typed(array));
	CHECK(decoded_array[0] == Variant(value));

	const Variant json_native = JSON::to_native(JSON::from_native(array));
	REQUIRE(json_native.get_type() == Variant::ARRAY);
	const Array json_array = json_native;
	CHECK(json_array.is_same_typed(array));
	CHECK(json_array[0] == Variant(value));

	String text;
	REQUIRE(VariantWriter::write_to_string(array, text) == OK);
	CHECK(text.begins_with("Array[StructValue("));
	VariantParser::StreamString stream;
	stream.s = text;
	String error_text;
	int error_line = 0;
	Variant parsed;
	REQUIRE(VariantParser::parse(&stream, parsed, error_text, error_line) == OK);
	const Array parsed_array = parsed;
	CHECK(parsed_array.is_same_typed(array));
	CHECK(parsed_array[0] == Variant(value));

	ContainerType string_type;
	string_type.builtin_type = Variant::STRING;
	Dictionary dictionary;
	dictionary.set_typed(string_type, struct_type);
	dictionary["projectile"] = value;
	CHECK(StructValue(dictionary.get_typed_value_type_descriptor()).get_layout()->is_compatible(layout));
	REQUIRE(encode_variant(dictionary, nullptr, encoded_size) == OK);
	encoded.resize(encoded_size);
	REQUIRE(encode_variant(dictionary, encoded.ptrw(), written_size) == OK);
	REQUIRE(decode_variant(decoded_variant, encoded.ptr(), encoded.size()) == OK);
	const Dictionary decoded_dictionary = decoded_variant;
	CHECK(decoded_dictionary.is_same_typed(dictionary));
	CHECK(decoded_dictionary["projectile"] == Variant(value));

	const Variant json_native_dictionary = JSON::to_native(JSON::from_native(dictionary));
	REQUIRE(json_native_dictionary.get_type() == Variant::DICTIONARY);
	const Dictionary json_dictionary = json_native_dictionary;
	CHECK(json_dictionary.is_same_typed(dictionary));
	CHECK(json_dictionary["projectile"] == Variant(value));
}

} // namespace TestStructValue
