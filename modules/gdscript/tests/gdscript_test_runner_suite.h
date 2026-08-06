/**************************************************************************/
/*  gdscript_test_runner_suite.h                                          */
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

#pragma once

#include "../gdscript_cache.h"
#include "gdscript_test_runner.h"

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
#include "../gdscript_baseline_jit.h"
#include "../gdscript_optimization_profile.h"
#endif

#ifdef TOOLS_ENABLED
#include "core/os/os.h"
#endif

namespace GDScriptTests {

class TestGDScriptCacheAccessor {
public:
	static bool has_shallow(String p_path) {
		return GDScriptCache::singleton->shallow_gdscript_cache.has(p_path);
	}

	static bool has_full(String p_path) {
		return GDScriptCache::singleton->full_gdscript_cache.has(p_path);
	}
};

// TODO: Handle some cases failing on release builds. See: https://github.com/godotengine/godot/pull/88452
#ifdef TOOLS_ENABLED
TEST_SUITE("[Modules][GDScript]") {
	TEST_CASE("Script compilation and runtime") {
		bool print_filenames = OS::get_singleton()->get_cmdline_args().find("--print-filenames") != nullptr;
		bool use_binary_tokens = OS::get_singleton()->get_cmdline_args().find("--use-binary-tokens") != nullptr;
		GDScriptTestRunner runner("modules/gdscript/tests/scripts", true, print_filenames, use_binary_tokens);
		int fail_count = runner.run_tests();
		INFO("Make sure `*.out` files have expected results.");
		REQUIRE_MESSAGE(fail_count == 0, "All GDScript tests should pass.");
	}
}
#endif // TOOLS_ENABLED

TEST_CASE("[Modules][GDScript] Load source code dynamically and run it") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> gdscript = memnew(GDScript);
	gdscript->set_source_code(R"(
extends RefCounted

func _init():
	set_meta("result", 42)
)");
	// A spurious `Condition "err" is true` message is printed (despite parsing being successful and returning `OK`).
	// Silence it.
	ERR_PRINT_OFF;
	const Error error = gdscript->reload();
	ERR_PRINT_ON;
	CHECK_MESSAGE(error == OK, "The script should parse successfully.");

	// Run the script by assigning it to a reference-counted object.
	Ref<RefCounted> ref_counted = memnew(RefCounted);
	ref_counted->set_script(gdscript);
	CHECK_MESSAGE(int(ref_counted->get_meta("result")) == 42, "The script should assign object metadata successfully.");
}

TEST_CASE("[Modules][GDScript] Struct expressions use specialized VM instructions") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> gdscript = memnew(GDScript);
	gdscript->set_source_code(R"(
extends RefCounted

struct Pair:
	var first: int
	var second: int

func construct_pair(first: int, second: int) -> Pair:
	return Pair(first, second)

func assign_pair(value: Pair) -> Pair:
	var copy: Pair = value
	return copy

func mutate_pair(value: Pair) -> int:
	value.first = value.second
	return value.first

func equal_pair(left: Pair, right: Pair) -> bool:
	return left == right

func box_pair(value: Pair) -> Variant:
	var boxed: Variant = value
	return boxed

func unbox_pair(value: Variant) -> Pair:
	var typed: Pair = value
	return typed
)");

	ERR_PRINT_OFF;
	const Error error = gdscript->reload();
	ERR_PRINT_ON;
	REQUIRE_MESSAGE(error == OK, "The struct bytecode test script should parse successfully.");

	Ref<RefCounted> instance = memnew(RefCounted);
	instance->set_script(gdscript);
	const Variant first = instance->call(SNAME("construct_pair"), 3, 7);
	const Variant same = instance->call(SNAME("assign_pair"), first);
	const Variant different = instance->call(SNAME("construct_pair"), 3, 8);
	REQUIRE(first.get_type() == Variant::STRUCT);
	REQUIRE(same.get_type() == Variant::STRUCT);
	CHECK(bool(instance->call(SNAME("equal_pair"), first, same)));
	CHECK_FALSE(bool(instance->call(SNAME("equal_pair"), first, different)));
	CHECK(int64_t(instance->call(SNAME("mutate_pair"), first)) == 7);

	const Variant boxed = instance->call(SNAME("box_pair"), first);
	const Variant unboxed = instance->call(SNAME("unbox_pair"), boxed);
	REQUIRE(boxed.get_type() == Variant::STRUCT);
	REQUIRE(unboxed.get_type() == Variant::STRUCT);
	CHECK(bool(instance->call(SNAME("equal_pair"), first, unboxed)));
}

#ifdef GDSCRIPT_BASELINE_JIT_ENABLED
TEST_CASE("[Modules][GDScript] Baseline JIT unboxes primitive frames and calls") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> gdscript = memnew(GDScript);
	gdscript->set_source_code(R"(
extends RefCounted

func integer_math(limit: int) -> int:
	var index: int = 0
	var total: int = 0
	while index < limit:
		if (index & 1) == 0:
			total += index * 2
		else:
			total -= 1
		index += 1
	return total

func float_math(limit: int) -> float:
	var index: int = 0
	var value: float = 1.5
	while index < limit:
		value = value * 1.25 + 0.5
		index += 1
	return value

func bool_branch(condition: bool) -> int:
	var value: int = 1
	if condition:
		value = 7
	else:
		value = 9
	return value

func wide_integer(value: int) -> int:
	return value + 4_000_000_000

func float_not_equal(left: float, right: float) -> bool:
	return left != right

func float_less(left: float, right: float) -> bool:
	return left < right

func mixed_signature(integer: int, scalar: float, enabled: bool) -> float:
	var adjusted: int = integer + 2
	var result: float = scalar * 2.0
	if enabled:
		result += 1.0
	if adjusted < 0:
		result = -result
	return result

func no_arguments() -> int:
	var value: int = 40
	return value + 2

func call_mixed_signature() -> float:
	return mixed_signature(4, 1.5, true)

func interpreter_fallback(value: int) -> String:
	return str(value)
)");

	ERR_PRINT_OFF;
	const Error error = gdscript->reload();
	ERR_PRINT_ON;
	REQUIRE_MESSAGE(error == OK, "The baseline JIT test script should parse successfully.");

	const HashMap<StringName, GDScriptFunction *> &functions = gdscript->get_member_functions();
	for (const StringName &function_name : { SNAME("integer_math"), SNAME("float_math"), SNAME("bool_branch"), SNAME("wide_integer"), SNAME("float_not_equal"), SNAME("float_less"), SNAME("mixed_signature"), SNAME("no_arguments") }) {
		const GDScriptFunction *const *function = functions.getptr(function_name);
		REQUIRE_MESSAGE(function != nullptr, "The expected test function should be compiled.");
		CHECK_MESSAGE((*function)->has_baseline_jit(), vformat("Function '%s' should have baseline native code.", function_name));
		CHECK_MESSAGE((*function)->has_typed_baseline_jit(), vformat("Function '%s' should expose the primitive typed entry.", function_name));
	}
	const GDScriptFunction *const *script_call_function = functions.getptr(SNAME("call_mixed_signature"));
	REQUIRE(script_call_function != nullptr);
	CHECK_FALSE_MESSAGE((*script_call_function)->has_baseline_jit(), "The caller's dynamic dispatch opcode should stay in the interpreter.");
	const GDScriptFunction *const *fallback_function = functions.getptr(SNAME("interpreter_fallback"));
	REQUIRE(fallback_function != nullptr);
	CHECK_FALSE_MESSAGE((*fallback_function)->has_baseline_jit(), "Unsupported bytecode should keep the function on the interpreter.");

	Ref<RefCounted> ref_counted = memnew(RefCounted);
	ref_counted->set_script(gdscript);
	CHECK(int64_t(ref_counted->call(SNAME("integer_math"), 6)) == 9);
	CHECK(Math::is_equal_approx(double(ref_counted->call(SNAME("float_math"), 3)), 4.8359375));
	CHECK(int64_t(ref_counted->call(SNAME("bool_branch"), true)) == 7);
	CHECK(int64_t(ref_counted->call(SNAME("bool_branch"), false)) == 9);
	CHECK(int64_t(ref_counted->call(SNAME("wide_integer"), 5'000'000'000)) == 9'000'000'000);
	CHECK(bool(ref_counted->call(SNAME("float_not_equal"), Math::NaN, 1.0)));
	CHECK_FALSE(bool(ref_counted->call(SNAME("float_less"), Math::NaN, 1.0)));
	CHECK(Math::is_equal_approx(double(ref_counted->call(SNAME("mixed_signature"), 4, 1.5, true)), 4.0));
	CHECK(int64_t(ref_counted->call(SNAME("no_arguments"))) == 42);
	CHECK(Math::is_equal_approx(double(ref_counted->call(SNAME("call_mixed_signature"))), 4.0));
	// A strictly convertible argument cannot use the raw typed entry, but the
	// Variant-frame entry still executes the function with unboxed native locals.
	CHECK(int64_t(ref_counted->call(SNAME("wide_integer"), 5'000'000'000.0)) == 9'000'000'000);
	CHECK(String(ref_counted->call(SNAME("interpreter_fallback"), 42)) == "42");

	Ref<GDScript> reload_script = memnew(GDScript);
	reload_script->set_source_code("extends RefCounted\nfunc increment(value: int) -> int:\n\treturn value + 1\n");
	REQUIRE(reload_script->reload() == OK);
	Ref<RefCounted> reload_instance = memnew(RefCounted);
	reload_instance->set_script(reload_script);
	CHECK(int64_t(reload_instance->call(SNAME("increment"), 40)) == 41);
	reload_script->set_source_code("extends RefCounted\nfunc increment(value: int) -> int:\n\treturn value + 2\n");
	REQUIRE(reload_script->reload(true) == OK);
	const GDScriptFunction *const *reloaded_function = reload_script->get_member_functions().getptr(SNAME("increment"));
	REQUIRE(reloaded_function != nullptr);
	CHECK((*reloaded_function)->has_typed_baseline_jit());
	CHECK(int64_t(reload_instance->call(SNAME("increment"), 40)) == 42);
}

TEST_CASE("[Modules][GDScript] Baseline JIT lowers unboxed native calls to ptrcall") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> gdscript = memnew(GDScript);
	gdscript->set_source_code(R"(
extends RefCounted

func native_bool_roundtrip(value: bool) -> bool:
	set_block_signals(value)
	return is_blocking_signals()

func native_integer_result() -> int:
	return get_reference_count()

func native_static_result() -> bool:
	return Thread.is_main_thread()

func native_nonprimitive_fallback() -> bool:
	return has_meta(&"missing")
)");

	ERR_PRINT_OFF;
	const Error error = gdscript->reload();
	ERR_PRINT_ON;
	REQUIRE_MESSAGE(error == OK, "The ptrcall lowering test script should parse successfully.");

	const HashMap<StringName, GDScriptFunction *> &functions = gdscript->get_member_functions();
	const GDScriptFunction *const *bool_function = functions.getptr(SNAME("native_bool_roundtrip"));
	const GDScriptFunction *const *integer_function = functions.getptr(SNAME("native_integer_result"));
	const GDScriptFunction *const *static_function = functions.getptr(SNAME("native_static_result"));
	const GDScriptFunction *const *fallback_function = functions.getptr(SNAME("native_nonprimitive_fallback"));
	REQUIRE(bool_function != nullptr);
	REQUIRE(integer_function != nullptr);
	REQUIRE(static_function != nullptr);
	REQUIRE(fallback_function != nullptr);
	CHECK((*bool_function)->has_typed_baseline_jit());
	CHECK((*bool_function)->get_baseline_jit_ptrcall_count() == 2);
	CHECK((*integer_function)->has_typed_baseline_jit());
	CHECK((*integer_function)->get_baseline_jit_ptrcall_count() == 1);
	CHECK((*static_function)->has_typed_baseline_jit());
	CHECK((*static_function)->get_baseline_jit_ptrcall_count() == 1);
	CHECK_FALSE_MESSAGE((*fallback_function)->has_baseline_jit(), "A StringName argument still requires the Variant interpreter path.");

	Ref<RefCounted> ref_counted = memnew(RefCounted);
	ref_counted->set_script(gdscript);
	CHECK(bool(ref_counted->call(SNAME("native_bool_roundtrip"), true)));
	CHECK_FALSE(bool(ref_counted->call(SNAME("native_bool_roundtrip"), false)));
	CHECK(int64_t(ref_counted->call(SNAME("native_integer_result"))) > 0);
	CHECK(bool(ref_counted->call(SNAME("native_static_result"))));
	CHECK_FALSE(bool(ref_counted->call(SNAME("native_nonprimitive_fallback"))));

	Ref<GDScript> float_script = memnew(GDScript);
	float_script->set_source_code(R"(
extends Node2D

func native_float_roundtrip(value: float) -> float:
	move_local_x(0.0, false)
	set_rotation(value)
	return get_rotation()
)");
	REQUIRE(float_script->reload() == OK);
	const GDScriptFunction *const *float_function = float_script->get_member_functions().getptr(SNAME("native_float_roundtrip"));
	REQUIRE(float_function != nullptr);
	CHECK((*float_function)->has_typed_baseline_jit());
	CHECK((*float_function)->get_baseline_jit_ptrcall_count() == 3);
	Object *float_instance = ClassDB::instantiate(SNAME("Node2D"));
	REQUIRE(float_instance != nullptr);
	float_instance->set_script(float_script);
	CHECK(Math::is_equal_approx(double(float_instance->call(SNAME("native_float_roundtrip"), 0.375)), 0.375));
	// An implicitly convertible argument rejects the raw entry, then exercises
	// ptrcall lowering through the interpreter-compatible Variant-frame entry.
	CHECK(Math::is_equal_approx(double(float_instance->call(SNAME("native_float_roundtrip"), 1)), 1.0));

	float_script->set_source_code(R"(
extends Node2D

func native_float_roundtrip(value: float) -> float:
	move_local_x(0.0, false)
	set_rotation(value + 0.125)
	return get_rotation()
)");
	REQUIRE(float_script->reload(true) == OK);
	float_function = float_script->get_member_functions().getptr(SNAME("native_float_roundtrip"));
	REQUIRE(float_function != nullptr);
	CHECK((*float_function)->get_baseline_jit_ptrcall_count() == 3);
	CHECK(Math::is_equal_approx(double(float_instance->call(SNAME("native_float_roundtrip"), 0.375)), 0.5));
	memdelete(float_instance);
}

TEST_CASE("[Modules][GDScript] Baseline JIT keeps engine math values unboxed") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> gdscript = memnew(GDScript);
	gdscript->set_source_code(R"(
extends Node2D

func vector2_math(left: Vector2, right: Vector2, scale: float) -> Vector2:
	var value: Vector2 = left
	value = value + right
	return -value * scale

func vector2_equal(left: Vector2, right: Vector2) -> bool:
	return left == right

func color_math(left: Color, right: Color, scale: float) -> Color:
	var value: Color = left + right
	return value / scale

func native_vector2_color(position_value: Vector2, color_value: Color) -> Color:
	set_position(position_value)
	set_modulate(color_value)
	return get_modulate()
)");
	REQUIRE_MESSAGE(gdscript->reload() == OK, "The unboxed math test script should parse successfully.");

	const HashMap<StringName, GDScriptFunction *> &functions = gdscript->get_member_functions();
	for (const StringName &function_name : { SNAME("vector2_math"), SNAME("vector2_equal"), SNAME("color_math"), SNAME("native_vector2_color") }) {
		const GDScriptFunction *const *function = functions.getptr(function_name);
		REQUIRE(function != nullptr);
		CHECK_MESSAGE((*function)->has_typed_baseline_jit(), vformat("Function '%s' should expose the math-value typed entry.", function_name));
	}
	const GDScriptFunction *const *native_function = functions.getptr(SNAME("native_vector2_color"));
	REQUIRE(native_function != nullptr);
	CHECK((*native_function)->get_baseline_jit_ptrcall_count() == 3);

	Object *node_2d = ClassDB::instantiate(SNAME("Node2D"));
	REQUIRE(node_2d != nullptr);
	node_2d->set_script(gdscript);
	CHECK(Vector2(node_2d->call(SNAME("vector2_math"), Vector2(1.0, -2.0), Vector2(3.0, 5.0), 2.0)).is_equal_approx(Vector2(-8.0, -6.0)));
	CHECK(bool(node_2d->call(SNAME("vector2_equal"), Vector2(2.0, 4.0), Vector2(2.0, 4.0))));
	CHECK_FALSE(bool(node_2d->call(SNAME("vector2_equal"), Vector2(2.0, 4.0), Vector2(2.0, 5.0))));
	CHECK(Color(node_2d->call(SNAME("color_math"), Color(0.2, 0.4, 0.6, 0.8), Color(0.4, 0.2, 0.0, 0.2), 2.0)).is_equal_approx(Color(0.3, 0.3, 0.3, 0.5)));
	const Color roundtrip_color(0.15, 0.35, 0.55, 0.75);
	CHECK(Color(node_2d->call(SNAME("native_vector2_color"), Vector2(7.0, -3.0), roundtrip_color)).is_equal_approx(roundtrip_color));
	CHECK(Vector2(node_2d->get(SNAME("position"))).is_equal_approx(Vector2(7.0, -3.0)));
	memdelete(node_2d);

	Ref<GDScript> vector3_script = memnew(GDScript);
	vector3_script->set_source_code(R"(
extends Node3D

const BASE_VELOCITY := Vector3(1.0, 2.0, 3.0)

func vector3_phi(left: Vector3, right: Vector3, choose_right: bool, scale: float) -> Vector3:
	var selected: Vector3 = left
	if choose_right:
		selected = right
	return selected * scale

func scalar_replaced_velocity(direction: Vector3, speed: float, gravity: float) -> float:
	var velocity: Vector3 = direction * speed
	velocity.y += gravity
	return velocity.length()

func scalar_replaced_y(direction: Vector3, speed: float) -> float:
	var velocity: Vector3 = direction * speed
	return velocity.y

func scalar_replaced_constant() -> float:
	var velocity: Vector3 = BASE_VELOCITY * 4.0
	return velocity.y

func scalar_replaced_ptrcall(direction: Vector3, speed: float) -> Vector3:
	var velocity: Vector3 = direction * speed
	set_position(velocity)
	return get_position()

func native_vector3_roundtrip(value: Vector3) -> Vector3:
	set_position(value)
	return get_position()
)");
	REQUIRE(vector3_script->reload() == OK);
	const HashMap<StringName, GDScriptFunction *> &vector3_functions = vector3_script->get_member_functions();
	const GDScriptFunction *const *phi_function = vector3_functions.getptr(SNAME("vector3_phi"));
	const GDScriptFunction *const *scalar_replaced_function = vector3_functions.getptr(SNAME("scalar_replaced_velocity"));
	const GDScriptFunction *const *scalar_replaced_y_function = vector3_functions.getptr(SNAME("scalar_replaced_y"));
	const GDScriptFunction *const *scalar_replaced_constant_function = vector3_functions.getptr(SNAME("scalar_replaced_constant"));
	const GDScriptFunction *const *scalar_replaced_ptrcall_function = vector3_functions.getptr(SNAME("scalar_replaced_ptrcall"));
	const GDScriptFunction *const *vector3_native_function = vector3_functions.getptr(SNAME("native_vector3_roundtrip"));
	REQUIRE(phi_function != nullptr);
	REQUIRE(scalar_replaced_function != nullptr);
	REQUIRE(scalar_replaced_y_function != nullptr);
	REQUIRE(scalar_replaced_constant_function != nullptr);
	REQUIRE(scalar_replaced_ptrcall_function != nullptr);
	REQUIRE(vector3_native_function != nullptr);
	CHECK((*phi_function)->has_typed_baseline_jit());
	CHECK((*scalar_replaced_function)->has_typed_baseline_jit());
	CHECK((*scalar_replaced_y_function)->has_typed_baseline_jit());
	CHECK((*scalar_replaced_constant_function)->has_typed_baseline_jit());
	CHECK((*scalar_replaced_ptrcall_function)->has_typed_baseline_jit());
	CHECK((*scalar_replaced_ptrcall_function)->get_baseline_jit_ptrcall_count() == 2);
	CHECK((*vector3_native_function)->has_typed_baseline_jit());
	CHECK((*vector3_native_function)->get_baseline_jit_ptrcall_count() == 2);

	Object *node_3d = ClassDB::instantiate(SNAME("Node3D"));
	REQUIRE(node_3d != nullptr);
	node_3d->set_script(vector3_script);
	for (uint32_t i = 0; i <= GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		const Vector3 result = node_3d->call(SNAME("vector3_phi"), Vector3(1.0, 2.0, 3.0), Vector3(-2.0, 4.0, 6.0), true, 0.5);
		CHECK(result.is_equal_approx(Vector3(-1.0, 2.0, 3.0)));
	}
	CHECK_MESSAGE((*phi_function)->has_optimizing_jit(), "The compact SSA tier should preserve unboxed Vector3 phis.");
	for (uint32_t i = 0; i <= GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		CHECK(Math::is_equal_approx(double(node_3d->call(SNAME("scalar_replaced_velocity"), Vector3(1.0, 2.0, 3.0), 2.0, -1.0)), 7.0));
		CHECK(Math::is_equal_approx(double(node_3d->call(SNAME("scalar_replaced_y"), Vector3(1.0, 2.0, 3.0), 4.0)), 8.0));
		CHECK(Math::is_equal_approx(double(node_3d->call(SNAME("scalar_replaced_constant"))), 8.0));
		CHECK(Vector3(node_3d->call(SNAME("scalar_replaced_ptrcall"), Vector3(1.0, -2.0, 3.0), 3.0)).is_equal_approx(Vector3(3.0, -6.0, 9.0)));
	}
	CHECK_MESSAGE((*scalar_replaced_function)->has_optimizing_jit(), "Component mutation and length should compile in the scalar-replacing SSA tier.");
	CHECK_MESSAGE((*scalar_replaced_y_function)->has_optimizing_jit(), "A component-only consumer should compile in the scalar-replacing SSA tier.");
	CHECK_MESSAGE((*scalar_replaced_constant_function)->has_optimizing_jit(), "Constant math components should propagate through scalar replacement.");
	CHECK_MESSAGE((*scalar_replaced_ptrcall_function)->has_optimizing_jit(), "A native pointer boundary should materialize a scalar-replaced Vector3.");
	CHECK((*scalar_replaced_function)->get_optimizing_jit_scalar_replaced_math_value_count() > 0);
	CHECK((*scalar_replaced_y_function)->get_optimizing_jit_scalar_replaced_math_value_count() > 0);
	CHECK_MESSAGE((*scalar_replaced_y_function)->get_optimizing_jit_eliminated_node_count() >= 2, "The component-only consumer should eliminate unused Vector3 component computations.");
	CHECK_MESSAGE((*scalar_replaced_constant_function)->get_optimizing_jit_eliminated_node_count() >= 3, "Constant Vector3 arithmetic should fold independently per component.");
	const Vector3 roundtrip_vector(8.0, -4.0, 2.5);
	CHECK(Vector3(node_3d->call(SNAME("native_vector3_roundtrip"), roundtrip_vector)).is_equal_approx(roundtrip_vector));
	memdelete(node_3d);
}

TEST_CASE("[Modules][GDScript] Baseline JIT keeps large engine values in native frames") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> transform_2d_script = memnew(GDScript);
	transform_2d_script->set_source_code(R"(
extends Node2D

func transform2d_math(left: Transform2D, right: Transform2D, scale: float) -> Transform2D:
	var value: Transform2D = left
	value = value * right
	return value / scale

func transform2d_equal(left: Transform2D, right: Transform2D) -> bool:
	return left == right

func native_transform2d(value: Transform2D) -> Transform2D:
	set_transform(value)
	return get_transform()
)");
	REQUIRE_MESSAGE(transform_2d_script->reload() == OK, "The large Transform2D test script should parse successfully.");

	const HashMap<StringName, GDScriptFunction *> &transform_2d_functions = transform_2d_script->get_member_functions();
	for (const StringName &function_name : { SNAME("transform2d_math"), SNAME("transform2d_equal"), SNAME("native_transform2d") }) {
		const GDScriptFunction *const *function = transform_2d_functions.getptr(function_name);
		REQUIRE(function != nullptr);
		CHECK_MESSAGE((*function)->has_typed_baseline_jit(), vformat("Function '%s' should expose the pointer-based large-value entry.", function_name));
	}
	const GDScriptFunction *const *transform_2d_math_function = transform_2d_functions.getptr(SNAME("transform2d_math"));
	const GDScriptFunction *const *native_transform_2d_function = transform_2d_functions.getptr(SNAME("native_transform2d"));
	REQUIRE(transform_2d_math_function != nullptr);
	REQUIRE(native_transform_2d_function != nullptr);
	CHECK((*native_transform_2d_function)->get_baseline_jit_ptrcall_count() == 2);

	Object *node_2d = ClassDB::instantiate(SNAME("Node2D"));
	REQUIRE(node_2d != nullptr);
	node_2d->set_script(transform_2d_script);
	const Transform2D transform_2d_left(Math::deg_to_rad(real_t(25.0)), Vector2(3.0, -2.0));
	const Transform2D transform_2d_right(Math::deg_to_rad(real_t(-10.0)), Vector2(-1.0, 4.0));
	const Transform2D expected_transform_2d = (transform_2d_left * transform_2d_right) / real_t(2.0);
	// An implicitly convertible scalar rejects the raw typed entry and exercises
	// the large-value pointers in the interpreter-compatible Variant frame.
	CHECK(Transform2D(node_2d->call(SNAME("transform2d_math"), transform_2d_left, transform_2d_right, 2)).is_equal_approx(expected_transform_2d));
	for (uint32_t i = 0; i <= GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		CHECK(Transform2D(node_2d->call(SNAME("transform2d_math"), transform_2d_left, transform_2d_right, 2.0)).is_equal_approx(expected_transform_2d));
		CHECK(bool(node_2d->call(SNAME("transform2d_equal"), transform_2d_left, transform_2d_left)));
		CHECK_FALSE(bool(node_2d->call(SNAME("transform2d_equal"), transform_2d_left, transform_2d_right)));
		CHECK(Transform2D(node_2d->call(SNAME("native_transform2d"), transform_2d_left)).is_equal_approx(transform_2d_left));
	}
	CHECK_MESSAGE((*transform_2d_math_function)->has_optimizing_jit(), "Transform2D arithmetic should remain native in the compact SSA tier.");
	CHECK_MESSAGE((*native_transform_2d_function)->has_optimizing_jit(), "A Transform2D ptrcall boundary should use native result storage in the compact SSA tier.");
	CHECK(Transform2D(node_2d->call(SNAME("transform2d_math"), transform_2d_left, transform_2d_right, 2)).is_equal_approx(expected_transform_2d));
	memdelete(node_2d);

	Ref<GDScript> transform_3d_script = memnew(GDScript);
	transform_3d_script->set_source_code(R"(
extends Node3D

func basis_math(left: Basis, right: Basis, scale: float) -> Basis:
	var value: Basis = left
	value = value * right
	return value * scale

func transform3d_math(left: Transform3D, right: Transform3D, scale: float) -> Transform3D:
	var value: Transform3D = left * right
	return value / scale

func aabb_roundtrip(value: AABB) -> AABB:
	var copy: AABB = value
	return copy

func aabb_equal(left: AABB, right: AABB) -> bool:
	return left == right

func projection_math(left: Projection, right: Projection) -> Projection:
	var value: Projection = left
	return value * right

func native_transform3d(value: Transform3D) -> Transform3D:
	set_transform(value)
	return get_transform()
)");
	REQUIRE_MESSAGE(transform_3d_script->reload() == OK, "The large 3D value test script should parse successfully.");

	const HashMap<StringName, GDScriptFunction *> &transform_3d_functions = transform_3d_script->get_member_functions();
	for (const StringName &function_name : { SNAME("basis_math"), SNAME("transform3d_math"), SNAME("aabb_roundtrip"), SNAME("aabb_equal"), SNAME("projection_math"), SNAME("native_transform3d") }) {
		const GDScriptFunction *const *function = transform_3d_functions.getptr(function_name);
		REQUIRE(function != nullptr);
		CHECK_MESSAGE((*function)->has_typed_baseline_jit(), vformat("Function '%s' should expose the pointer-based large-value entry.", function_name));
	}
	const GDScriptFunction *const *basis_function = transform_3d_functions.getptr(SNAME("basis_math"));
	const GDScriptFunction *const *transform_3d_function = transform_3d_functions.getptr(SNAME("transform3d_math"));
	const GDScriptFunction *const *aabb_function = transform_3d_functions.getptr(SNAME("aabb_roundtrip"));
	const GDScriptFunction *const *projection_function = transform_3d_functions.getptr(SNAME("projection_math"));
	const GDScriptFunction *const *native_transform_3d_function = transform_3d_functions.getptr(SNAME("native_transform3d"));
	REQUIRE(basis_function != nullptr);
	REQUIRE(transform_3d_function != nullptr);
	REQUIRE(aabb_function != nullptr);
	REQUIRE(projection_function != nullptr);
	REQUIRE(native_transform_3d_function != nullptr);
	CHECK((*native_transform_3d_function)->get_baseline_jit_ptrcall_count() == 2);

	Object *node_3d = ClassDB::instantiate(SNAME("Node3D"));
	REQUIRE(node_3d != nullptr);
	node_3d->set_script(transform_3d_script);
	const Basis basis_left(Vector3(0.0, 1.0, 0.0), Math::deg_to_rad(real_t(35.0)));
	const Basis basis_right(Vector3(1.0, 0.0, 0.0), Math::deg_to_rad(real_t(-20.0)));
	const Basis expected_basis = (basis_left * basis_right) * real_t(1.5);
	const Transform3D transform_3d_left(basis_left, Vector3(2.0, -3.0, 4.0));
	const Transform3D transform_3d_right(basis_right, Vector3(-1.0, 5.0, 2.0));
	const Transform3D expected_transform_3d = (transform_3d_left * transform_3d_right) / real_t(2.0);
	const AABB aabb(Vector3(-2.0, 3.0, 1.0), Vector3(8.0, 4.0, 6.0));
	const AABB other_aabb(Vector3(-2.0, 3.0, 2.0), Vector3(8.0, 4.0, 6.0));
	const Projection projection_left = Projection::create_perspective(65.0, 1.6, 0.1, 250.0);
	const Projection projection_right(transform_3d_right);
	const Projection expected_projection = projection_left * projection_right;
	for (uint32_t i = 0; i <= GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		CHECK(Basis(node_3d->call(SNAME("basis_math"), basis_left, basis_right, 1.5)).is_equal_approx(expected_basis));
		CHECK(Transform3D(node_3d->call(SNAME("transform3d_math"), transform_3d_left, transform_3d_right, 2.0)).is_equal_approx(expected_transform_3d));
		CHECK(AABB(node_3d->call(SNAME("aabb_roundtrip"), aabb)).is_equal_approx(aabb));
		CHECK(bool(node_3d->call(SNAME("aabb_equal"), aabb, aabb)));
		CHECK_FALSE(bool(node_3d->call(SNAME("aabb_equal"), aabb, other_aabb)));
		CHECK(Projection(node_3d->call(SNAME("projection_math"), projection_left, projection_right)).is_same(expected_projection));
		CHECK(Transform3D(node_3d->call(SNAME("native_transform3d"), transform_3d_left)).is_equal_approx(transform_3d_left));
	}
	CHECK_MESSAGE((*basis_function)->has_optimizing_jit(), "Basis arithmetic should remain native in the compact SSA tier.");
	CHECK_MESSAGE((*transform_3d_function)->has_optimizing_jit(), "Transform3D arithmetic should remain native in the compact SSA tier.");
	CHECK_MESSAGE((*aabb_function)->has_optimizing_jit(), "AABB copies should remain native in the compact SSA tier.");
	CHECK_MESSAGE((*projection_function)->has_optimizing_jit(), "Projection arithmetic should remain native in the compact SSA tier.");
	CHECK_MESSAGE((*native_transform_3d_function)->has_optimizing_jit(), "A Transform3D ptrcall boundary should use native result storage in the compact SSA tier.");
	memdelete(node_3d);
}

TEST_CASE("[Modules][GDScript] Baseline JIT keeps trivial user structs in native frames") {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> gdscript = memnew(GDScript);
	gdscript->set_source_code(R"(
extends RefCounted

struct Motion:
	var position: Vector3
	var velocity: Vector3
	var lifetime: float

struct Batch:
	var first: Motion
	var second: Motion

struct Named:
	var label: String

struct GameplayState:
	var screen_position: Vector2
	var velocity: Vector3
	var tint: Color

func make_motion(position: Vector3, velocity: Vector3, lifetime: float) -> Motion:
	return Motion(position, velocity, lifetime)

func update_motion(value: Motion, delta: Vector3) -> Motion:
	var copy: Motion = value
	copy.position = copy.position + delta
	copy.lifetime = copy.lifetime - 1.0
	return copy

func motion_equal(left: Motion, right: Motion) -> bool:
	return left == right

func motion_position(value: Motion) -> Vector3:
	return value.position

func motion_lifetime(value: Motion) -> float:
	return value.lifetime

func make_batch(first: Motion, second: Motion) -> Batch:
	return Batch(first, second)

func first_motion(value: Batch) -> Motion:
	return value.first

func box_motion(value: Motion) -> Variant:
	var boxed: Variant = value
	return boxed

func named_roundtrip(value: Named) -> Named:
	return value

func make_gameplay_state(screen_position: Vector2, velocity: Vector3, tint: Color) -> GameplayState:
	return GameplayState(screen_position, velocity, tint)

func gameplay_metric(value: GameplayState, offset: Vector2, acceleration: Vector3, fade: float) -> float:
	var copy: GameplayState = value
	copy.screen_position = copy.screen_position + offset
	copy.velocity = copy.velocity + acceleration
	copy.tint = copy.tint * fade
	return copy.screen_position.x + copy.velocity.y + copy.tint.a

func choose_gameplay_state(left: GameplayState, right: GameplayState, choose_right: bool) -> GameplayState:
	var selected: GameplayState = left
	if choose_right:
		selected = right
	return selected
)");
	REQUIRE_MESSAGE(gdscript->reload() == OK, "The native user-struct test script should parse successfully.");

	const HashMap<StringName, GDScriptFunction *> &functions = gdscript->get_member_functions();
	for (const StringName &function_name : { SNAME("make_motion"), SNAME("update_motion"), SNAME("motion_equal"), SNAME("motion_position"), SNAME("motion_lifetime"), SNAME("make_batch"), SNAME("first_motion"), SNAME("make_gameplay_state"), SNAME("gameplay_metric"), SNAME("choose_gameplay_state") }) {
		const GDScriptFunction *const *function = functions.getptr(function_name);
		REQUIRE(function != nullptr);
		CHECK_MESSAGE((*function)->has_baseline_jit(), vformat("Function '%s' should keep its trivial struct values in a native frame.", function_name));
		CHECK_MESSAGE((*function)->has_typed_baseline_jit(), vformat("Function '%s' should expose the pointer-based struct ABI.", function_name));
	}
	const GDScriptFunction *const *box_function = functions.getptr(SNAME("box_motion"));
	const GDScriptFunction *const *fallback_function = functions.getptr(SNAME("named_roundtrip"));
	REQUIRE(box_function != nullptr);
	REQUIRE(fallback_function != nullptr);
	CHECK_MESSAGE((*box_function)->has_baseline_jit(), "A dynamic return should box the native struct only at its Variant boundary.");
	CHECK_FALSE((*box_function)->has_typed_baseline_jit());
	CHECK_FALSE_MESSAGE((*fallback_function)->has_baseline_jit(), "A non-trivial struct should remain on the interpreter.");

	Ref<RefCounted> instance = memnew(RefCounted);
	instance->set_script(gdscript);
	const Vector3 position(2.0, -3.0, 4.0);
	const Vector3 velocity(0.5, 1.5, -2.0);
	const Vector3 delta(3.0, 2.0, -1.0);
	const Variant motion = instance->call(SNAME("make_motion"), position, velocity, 8.0);
	REQUIRE(motion.get_type() == Variant::STRUCT);
	CHECK(Vector3(instance->call(SNAME("motion_position"), motion)).is_equal_approx(position));
	CHECK(Math::is_equal_approx(double(instance->call(SNAME("motion_lifetime"), motion)), 8.0));

	const Variant updated = instance->call(SNAME("update_motion"), motion, delta);
	REQUIRE(updated.get_type() == Variant::STRUCT);
	CHECK(Vector3(instance->call(SNAME("motion_position"), updated)).is_equal_approx(position + delta));
	CHECK(Math::is_equal_approx(double(instance->call(SNAME("motion_lifetime"), updated)), 7.0));
	CHECK(Vector3(instance->call(SNAME("motion_position"), motion)).is_equal_approx(position));
	CHECK(bool(instance->call(SNAME("motion_equal"), motion, motion)));
	CHECK_FALSE(bool(instance->call(SNAME("motion_equal"), motion, updated)));

	const Variant batch = instance->call(SNAME("make_batch"), updated, motion);
	REQUIRE(batch.get_type() == Variant::STRUCT);
	const Variant first = instance->call(SNAME("first_motion"), batch);
	REQUIRE(first.get_type() == Variant::STRUCT);
	CHECK(bool(instance->call(SNAME("motion_equal"), first, updated)));
	const Variant boxed = instance->call(SNAME("box_motion"), updated);
	REQUIRE(boxed.get_type() == Variant::STRUCT);
	CHECK(bool(instance->call(SNAME("motion_equal"), boxed, updated)));

	const GDScriptFunction *const *make_gameplay_state = functions.getptr(SNAME("make_gameplay_state"));
	const GDScriptFunction *const *choose_gameplay_state = functions.getptr(SNAME("choose_gameplay_state"));
	const GDScriptFunction *const *motion_equal_function = functions.getptr(SNAME("motion_equal"));
	REQUIRE(make_gameplay_state != nullptr);
	REQUIRE(choose_gameplay_state != nullptr);
	REQUIRE(motion_equal_function != nullptr);
	Variant gameplay_state;
	for (uint32_t i = 0; i < GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		gameplay_state = instance->call(SNAME("make_gameplay_state"), Vector2(1.0, 2.0), Vector3(3.0, 4.0, 5.0), Color(0.2, 0.4, 0.6, 0.8));
	}
	REQUIRE(gameplay_state.get_type() == Variant::STRUCT);
	CHECK_MESSAGE((*make_gameplay_state)->has_optimizing_jit(), "Struct construction and typed returns should remain in the compact SSA tier.");
	const GDScriptFunction *const *gameplay_metric = functions.getptr(SNAME("gameplay_metric"));
	REQUIRE(gameplay_metric != nullptr);
	CHECK_FALSE((*gameplay_metric)->has_optimizing_jit());
	for (uint32_t i = 0; i < GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		const double metric = instance->call(SNAME("gameplay_metric"), gameplay_state, Vector2(0.5, -0.5), Vector3(1.0, 2.0, 3.0), 0.5);
		CHECK(Math::is_equal_approx(metric, 7.9));
	}
	CHECK_MESSAGE((*gameplay_metric)->has_optimizing_jit(), "Struct field operations should remain in the compact SSA tier.");
	CHECK_MESSAGE((*gameplay_metric)->get_optimizing_jit_scalar_replaced_math_value_count() >= 3, "Vector2, Vector3, and Color struct fields should be split into scalar SSA components.");
	CHECK_MESSAGE((*gameplay_metric)->get_optimizing_jit_eliminated_node_count() >= 3, "Field forwarding should eliminate temporary gameplay-struct updates that do not escape.");

	for (uint32_t i = 0; i < GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		const Variant selected = instance->call(SNAME("choose_gameplay_state"), gameplay_state, gameplay_state, true);
		CHECK(selected.get_type() == Variant::STRUCT);
	}
	CHECK_MESSAGE((*choose_gameplay_state)->has_optimizing_jit(), "Struct values should preserve their aligned native layout across SSA phis.");

	for (uint32_t i = 0; i < GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		CHECK(bool(instance->call(SNAME("motion_equal"), motion, motion)));
	}
	CHECK_MESSAGE((*motion_equal_function)->has_optimizing_jit(), "Struct equality should remain in the compact SSA tier.");
}

TEST_CASE("[Modules][GDScript] Compact SSA JIT promotes hot functions and consumes export profiles") {
	GDScriptLanguage::get_singleton()->init();
	GDScriptOptimizationProfile::clear();

	Ref<GDScript> gdscript = memnew(GDScript);
	gdscript->set_source_code(R"(
extends RefCounted

func hot_integer(limit: int) -> int:
	var index: int = 0
	var total: int = 0
	while index < limit:
		var tripled: int = index * 3
		var repeated: int = index * 3
		var dead: int = (index + 91) * 7
		if (index & 1) == 0:
			total += tripled
		else:
			total += repeated
		index += 1
	return total

func constant_path(value: int) -> int:
	var folded: int = 7
	folded *= 6
	var condition: bool = true
	if condition:
		return value + folded
	return -1

func hot_native(value: bool) -> bool:
	set_block_signals(value)
	return is_blocking_signals()
)");
	REQUIRE(gdscript->reload() == OK);
	Ref<RefCounted> instance = memnew(RefCounted);
	instance->set_script(gdscript);

	const GDScriptFunction *const *hot_integer = gdscript->get_member_functions().getptr(SNAME("hot_integer"));
	const GDScriptFunction *const *constant_path = gdscript->get_member_functions().getptr(SNAME("constant_path"));
	const GDScriptFunction *const *hot_native = gdscript->get_member_functions().getptr(SNAME("hot_native"));
	REQUIRE(hot_integer != nullptr);
	REQUIRE(constant_path != nullptr);
	REQUIRE(hot_native != nullptr);
	CHECK_FALSE((*hot_integer)->has_optimizing_jit());

	for (uint32_t i = 1; i < GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		CHECK(int64_t(instance->call(SNAME("hot_integer"), 10)) == 135);
	}
	CHECK_FALSE((*hot_integer)->has_optimizing_jit());
	CHECK(int64_t(instance->call(SNAME("hot_integer"), 10)) == 135);
	CHECK((*hot_integer)->has_optimizing_jit());
	CHECK((*hot_integer)->get_optimizing_jit_ssa_node_count() > 0);
	CHECK((*hot_integer)->get_optimizing_jit_eliminated_node_count() >= 2);
	// A convertible argument misses the raw entry but still uses the optimized
	// Variant-compatible entry after normal GDScript argument conversion.
	CHECK(int64_t(instance->call(SNAME("hot_integer"), 10.0)) == 135);

	for (uint32_t i = 0; i < GDScriptBaselineJIT::OPTIMIZING_CALL_THRESHOLD; i++) {
		CHECK(int64_t(instance->call(SNAME("constant_path"), 5)) == 47);
		CHECK(bool(instance->call(SNAME("hot_native"), bool(i & 1))) == bool(i & 1));
	}
	CHECK((*constant_path)->has_optimizing_jit());
	CHECK((*constant_path)->get_optimizing_jit_eliminated_node_count() > 0);
	CHECK((*hot_native)->has_optimizing_jit());

	const String script_path = OS::get_singleton()->get_temp_path().path_join("gdscript_ssa_profile_source.gd");
	const String profile_path = OS::get_singleton()->get_temp_path().path_join("gdscript_ssa_profile");
	Ref<GDScript> profiled_script = memnew(GDScript);
	profiled_script->set_path(script_path);
	profiled_script->set_source_code("extends RefCounted\nfunc profiled(value: int) -> int:\n\treturn (value + 2) * 3\n");
	REQUIRE(profiled_script->reload() == OK);
	const GDScriptFunction *const *profiled = profiled_script->get_member_functions().getptr(SNAME("profiled"));
	REQUIRE(profiled != nullptr);
	CHECK_FALSE((*profiled)->has_optimizing_jit());

	GDScriptOptimizationProfile::Entry entry;
	entry.key = (*profiled)->get_optimization_profile_key();
	entry.fingerprint = (*profiled)->get_optimization_fingerprint();
	entry.call_count = 1000;
	Vector<GDScriptOptimizationProfile::Entry> entries;
	entries.push_back(entry);
	REQUIRE(GDScriptOptimizationProfile::save(entries, profile_path) == OK);
	REQUIRE(profiled_script->reload(true) == OK);
	profiled = profiled_script->get_member_functions().getptr(SNAME("profiled"));
	REQUIRE(profiled != nullptr);
	CHECK_MESSAGE((*profiled)->has_optimizing_jit(), "An exact portable profile match should eagerly compile the SSA tier.");

	profiled_script->set_source_code("extends RefCounted\nfunc profiled(value: int) -> int:\n\treturn (value + 3) * 3\n");
	REQUIRE(profiled_script->reload(true) == OK);
	profiled = profiled_script->get_member_functions().getptr(SNAME("profiled"));
	REQUIRE(profiled != nullptr);
	CHECK_FALSE_MESSAGE((*profiled)->has_optimizing_jit(), "A changed bytecode fingerprint must reject a stale export profile.");
	GDScriptOptimizationProfile::clear();
}
#endif // GDSCRIPT_BASELINE_JIT_ENABLED

TEST_CASE("[Modules][GDScript] Loading keeps ResourceCache and GDScriptCache in sync") {
	GDScriptLanguage::get_singleton()->init();
	const String path = TestUtils::get_temp_path("gdscript_load_test.gd");

	{
		Ref<FileAccess> fa = FileAccess::open(path, FileAccess::ModeFlags::WRITE);
		fa->store_string("extends Node\n");
		fa->close();
	}

	CHECK(!ResourceCache::has(path));
	CHECK(!TestGDScriptCacheAccessor::has_shallow(path));
	CHECK(!TestGDScriptCacheAccessor::has_full(path));

	Ref<GDScript> loaded = ResourceLoader::load(path);

	CHECK(ResourceCache::has(path));
	CHECK(!TestGDScriptCacheAccessor::has_shallow(path));
	CHECK(TestGDScriptCacheAccessor::has_full(path));
}

TEST_CASE("[Modules][GDScript] Validate built-in API") {
	GDScriptLanguage *lang = GDScriptLanguage::get_singleton();

	// Validate methods.
	List<MethodInfo> builtin_methods;
	lang->get_public_functions(&builtin_methods);

	SUBCASE("[Modules][GDScript] Validate built-in methods") {
		for (const MethodInfo &mi : builtin_methods) {
			for (int64_t i = 0; i < mi.arguments.size(); ++i) {
				TEST_COND((mi.arguments[i].name.is_empty() || mi.arguments[i].name.begins_with("_unnamed_arg")),
						vformat("Unnamed argument in position %d of built-in method '%s'.", i, mi.name));
			}
		}
	}

	// Validate annotations.
	List<MethodInfo> builtin_annotations;
	lang->get_public_annotations(&builtin_annotations);

	SUBCASE("[Modules][GDScript] Validate built-in annotations") {
		for (const MethodInfo &ai : builtin_annotations) {
			for (int64_t i = 0; i < ai.arguments.size(); ++i) {
				TEST_COND((ai.arguments[i].name.is_empty() || ai.arguments[i].name.begins_with("_unnamed_arg")),
						vformat("Unnamed argument in position %d of built-in annotation '%s'.", i, ai.name));
			}
		}
	}
}

} // namespace GDScriptTests
