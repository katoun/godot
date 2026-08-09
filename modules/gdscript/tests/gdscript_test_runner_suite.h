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
#include "../gdscript_compiled_module.h"
#include "../gdscript_resource_format.h"
#include "gdscript_test_runner.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "scene/main/node.h"
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

TEST_CASE("[Modules][GDScript] Opcode descriptors cover every bytecode instruction") {
	const int invalid_opcodes[] = { -1, GDScriptFunction::OPCODE_COUNT, INT32_MAX };
	for (int invalid_opcode : invalid_opcodes) {
		const int invalid_instruction[] = { invalid_opcode };
		CHECK(GDScriptFunction::get_instruction_size(invalid_instruction, 1, 0) == -1);
	}
	for (int opcode_index = 0; opcode_index < GDScriptFunction::OPCODE_COUNT; opcode_index++) {
		const GDScriptFunction::Opcode opcode = GDScriptFunction::Opcode(opcode_index);
		const GDScriptFunction::OpcodeDescriptor &descriptor = GDScriptFunction::get_opcode_descriptor(opcode);
		CAPTURE(opcode_index);
		CAPTURE(descriptor.name);
		REQUIRE(descriptor.name != nullptr);

		const int argument_words = descriptor.instruction_size == 0 ? 3 : 0;
		const int expected_size = descriptor.instruction_size > 0 ? descriptor.instruction_size : 2 + argument_words + descriptor.operand_kinds.count - 2;
		Vector<int> instruction;
		instruction.resize(expected_size);
		instruction.fill(0);
		instruction.write[0] = opcode;
		if (descriptor.instruction_size == 0) {
			REQUIRE(descriptor.operand_kinds.count >= 2);
			instruction.write[1] = argument_words;
		}
		CHECK(GDScriptFunction::get_instruction_size(instruction.ptr(), instruction.size(), 0) == expected_size);
		for (int word = 1; word < expected_size; word++) {
			CHECK(GDScriptFunction::get_operand_kind(instruction.ptr(), instruction.size(), 0, word) != GDScriptFunction::OPERAND_NONE);
		}
		const int result = GDScriptFunction::get_result_operand(instruction.ptr(), instruction.size(), 0);
		CHECK(result < expected_size);

		for (int truncated_size = 1; truncated_size < expected_size; truncated_size++) {
			CHECK(GDScriptFunction::get_instruction_size(instruction.ptr(), truncated_size, 0) == -1);
		}
		if (descriptor.instruction_size == 0) {
			instruction.write[1] = -1;
			CHECK(GDScriptFunction::get_instruction_size(instruction.ptr(), instruction.size(), 0) == -1);
			instruction.write[1] = INT32_MAX;
			CHECK(GDScriptFunction::get_instruction_size(instruction.ptr(), instruction.size(), 0) == -1);
		}
	}
	CHECK(GDScriptFunction::get_opcode_descriptor(GDScriptFunction::OPCODE_END).control_flow_kind == GDScriptFunction::CONTROL_FLOW_TERMINATE);
}

TEST_CASE("[Modules][GDScript] Compiled module rejects hostile binary data without partial installation") {
	GDScriptLanguage::get_singleton()->init();
	const String script_path = OS::get_singleton()->get_temp_path().path_join("hostile_portable_module.gd");
	const String source = R"(
extends RefCounted

struct Sample:
	var value: int
	var tint: Color = Color(0.25, 0.5, 0.75, 1.0)

var accumulator: int = 1

func evaluate(value: int, target: Object) -> Array:
	var adjusted: int = value
	if adjusted > 2:
		adjusted += 3
	else:
		adjusted -= 1
	var sample: Sample = Sample(adjusted)
	sample.value += accumulator
	var dynamic_target: Variant = target
	dynamic_target.set_meta(&"sample_value", sample.value)
	var values: Array[int] = [sample.value, adjusted]
	var closure := func(extra: int) -> int: return sample.value + extra
	accumulator += 1
	return [closure.call(values[1]), target.get_meta(&"sample_value"), sample.value, accumulator]
)";

	Ref<GDScript> source_script = memnew(GDScript);
	source_script->set_path(script_path);
	source_script->set_source_code(source);
	REQUIRE(source_script->reload() == OK);
	const Vector<uint8_t> tokens = source_script->get_as_binary_tokens();
	REQUIRE_FALSE(tokens.is_empty());
	Vector<uint8_t> module;
	REQUIRE(GDScriptCompiledModule::create(source_script.ptr(), tokens, module) == OK);
	REQUIRE(GDScriptCompiledModule::verify(module) == OK);

	// Round-trip into a runtime graph built without the parser/compiler and
	// compare a stateful call sequence with the source-compiled graph.
	Ref<GDScript> direct_script = memnew(GDScript);
	direct_script->set_path(script_path + ".direct");
	direct_script->set_binary_tokens_source(tokens);
	String direct_error;
	REQUIRE_MESSAGE(GDScriptCompiledModule::prepare_shallow(direct_script.ptr(), module, &direct_error) == OK, direct_error);
	REQUIRE_MESSAGE(GDScriptCompiledModule::build_runtime(direct_script.ptr(), module, false, &direct_error) == OK, direct_error);
	Ref<RefCounted> source_instance = memnew(RefCounted);
	Ref<RefCounted> direct_instance = memnew(RefCounted);
	source_instance->set_script(source_script);
	direct_instance->set_script(direct_script);
	Ref<RefCounted> source_target = memnew(RefCounted);
	Ref<RefCounted> direct_target = memnew(RefCounted);
	for (int value : Vector<int>{ 1, 4, -2, 9 }) {
		const Variant source_result = source_instance->call(SNAME("evaluate"), value, source_target.ptr());
		const Variant direct_result = direct_instance->call(SNAME("evaluate"), value, direct_target.ptr());
		CHECK(source_result == direct_result);
	}

	auto read_u32_le = [](const Vector<uint8_t> &p_bytes, int p_offset) {
		return uint32_t(p_bytes[p_offset]) | (uint32_t(p_bytes[p_offset + 1]) << 8) | (uint32_t(p_bytes[p_offset + 2]) << 16) |
				(uint32_t(p_bytes[p_offset + 3]) << 24);
	};
	auto write_u32_le = [](Vector<uint8_t> &r_bytes, int p_offset, uint32_t p_value) {
		for (int byte = 0; byte < 4; byte++) {
			r_bytes.write[p_offset + byte] = uint8_t(p_value >> (byte * 8));
		}
	};
	auto write_u64_le = [](Vector<uint8_t> &r_bytes, int p_offset, uint64_t p_value) {
		for (int byte = 0; byte < 8; byte++) {
			r_bytes.write[p_offset + byte] = uint8_t(p_value >> (byte * 8));
		}
	};
	const int fallback_size = read_u32_le(module, 52);
	const int payload_size = read_u32_le(module, 56);
	const int payload_offset = GDScriptCompiledModule::ENVELOPE_HEADER_SIZE + fallback_size;
	REQUIRE(payload_offset + payload_size == module.size());

	// Exercise every structural truncation region, including all header bytes,
	// evenly spaced payload cuts, and every final payload byte.
	Vector<int> truncation_points;
	for (int cut = 0; cut <= int(GDScriptCompiledModule::ENVELOPE_HEADER_SIZE); cut++) {
		truncation_points.push_back(cut);
	}
	const int truncation_stride = MAX(1, module.size() / 256);
	for (int cut = GDScriptCompiledModule::ENVELOPE_HEADER_SIZE; cut < module.size(); cut += truncation_stride) {
		truncation_points.push_back(cut);
	}
	for (int cut = MAX(0, module.size() - 128); cut < module.size(); cut++) {
		truncation_points.push_back(cut);
	}
	for (int cut : truncation_points) {
		CAPTURE(cut);
		const Vector<uint8_t> truncated = module.slice(0, cut);
		CHECK(GDScriptCompiledModule::verify(truncated) != OK);
	}

	Vector<uint8_t> corrupt_fallback = module;
	corrupt_fallback.write[GDScriptCompiledModule::ENVELOPE_HEADER_SIZE + fallback_size / 2] ^= 0x80;
	Vector<uint8_t> recovered_tokens;
	CHECK(GDScriptCompiledModule::extract_fallback(corrupt_fallback, recovered_tokens) == ERR_FILE_CORRUPT);
	CHECK(GDScriptCompiledModule::verify(corrupt_fallback) == ERR_FILE_CORRUPT);
	Vector<uint8_t> corrupt_payload = module;
	corrupt_payload.write[payload_offset + payload_size / 2] ^= 0x40;
	CHECK(GDScriptCompiledModule::verify(corrupt_payload) == ERR_FILE_CORRUPT);
	CHECK(GDScriptCompiledModule::extract_fallback(corrupt_payload, recovered_tokens) == OK);
	CHECK(recovered_tokens == tokens);

	// Envelope and payload counts must fail before allocation. The dependency
	// count is the first collection in the portable payload for this script.
	Vector<uint8_t> oversized_envelope = module;
	write_u32_le(oversized_envelope, 52, UINT32_MAX);
	CHECK(GDScriptCompiledModule::verify(oversized_envelope) == ERR_FILE_CORRUPT);
	Vector<uint8_t> oversized_payload_count = module;
	const int path_size = read_u32_le(module, payload_offset);
	const int dependency_count_offset = payload_offset + 4 + path_size + 24;
	REQUIRE(dependency_count_offset + 4 <= module.size());
	write_u32_le(oversized_payload_count, dependency_count_offset, UINT32_MAX);
	write_u64_le(oversized_payload_count, 36,
			GDScriptCompiledModule::fingerprint_bytes(oversized_payload_count.ptr() + payload_offset, payload_size));
	GDScriptCompiledModule::Rejection oversized_rejection;
	CHECK(GDScriptCompiledModule::verify(oversized_payload_count, nullptr, &oversized_rejection) != OK);
	CHECK(oversized_rejection.reason == GDScriptCompiledModule::REJECTION_UNKNOWN_METADATA);

	// Generate checksum-valid semantic corruption for the decoder/verifier. All
	// failures must leave an already executable script graph byte-for-byte live.
	const GDScriptFunction *const *source_evaluate = source_script->get_member_functions().getptr(SNAME("evaluate"));
	REQUIRE(source_evaluate != nullptr);
	const GDScriptFunction *stable_function = *source_evaluate;
	const GDScriptCompiledModule::TestBytecodeMutation mutations[] = {
		GDScriptCompiledModule::TEST_MUTATE_UNKNOWN_OPCODE,
		GDScriptCompiledModule::TEST_MUTATE_TRUNCATED_INSTRUCTION,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_JUMP_TARGET,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_FRAME_SLOT,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_TYPED_FRAME_SLOT,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_CONSTANT_INDEX,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_NAME_INDEX,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_FUNCTION_INDEX,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_ARGUMENT_COUNT,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_STRUCT_FIELD_INDEX,
		GDScriptCompiledModule::TEST_MUTATE_INVALID_NATIVE_API_RELOCATION,
	};
	for (GDScriptCompiledModule::TestBytecodeMutation mutation : mutations) {
		CAPTURE(mutation);
		Vector<uint8_t> hostile_module;
		String mutated_opcode;
		REQUIRE_MESSAGE(GDScriptCompiledModule::make_test_bytecode_mutation(module, mutation, hostile_module, &mutated_opcode) == OK,
				"The security corpus must contain an operand for every requested mutation.");
		CAPTURE(mutated_opcode);
		GDScriptCompiledModule::Rejection rejection;
		CHECK(GDScriptCompiledModule::verify(hostile_module, &direct_error, &rejection) == ERR_INVALID_DATA);
		CHECK(rejection.reason == GDScriptCompiledModule::REJECTION_INVALID_BYTECODE);
		CHECK(GDScriptCompiledModule::extract_fallback(hostile_module, recovered_tokens) == OK);
		CHECK(recovered_tokens == tokens);
		CHECK(GDScriptCompiledModule::build_runtime(source_script.ptr(), hostile_module, false, &direct_error, &rejection) == ERR_INVALID_DATA);
		const GDScriptFunction *const *current_evaluate = source_script->get_member_functions().getptr(SNAME("evaluate"));
		REQUIRE(current_evaluate != nullptr);
		CHECK(*current_evaluate == stable_function);
		CHECK(source_script->is_valid());
	}

	// A deterministic mutation fuzzer runs in ordinary and sanitizer builds.
	// Half of the corpus is arbitrary bytes; the other half preserves the
	// envelope/fallback checksum and recomputes the payload checksum so mutations
	// reach the portable decoder and verifier instead of stopping at the header.
	uint64_t random_state = 0x9e3779b97f4a7c15ULL;
	auto next_random = [&]() {
		random_state ^= random_state << 13;
		random_state ^= random_state >> 7;
		random_state ^= random_state << 17;
		return random_state;
	};
	int rejected_random_inputs = 0;
	for (int iteration = 0; iteration < 256; iteration++) {
		Vector<uint8_t> random_bytes;
		random_bytes.resize(next_random() % 2048);
		for (int byte = 0; byte < random_bytes.size(); byte++) {
			random_bytes.write[byte] = uint8_t(next_random());
		}
		if (GDScriptCompiledModule::verify(random_bytes) != OK) {
			rejected_random_inputs++;
		}
		GDScriptCompiledModule::extract_fallback(random_bytes, recovered_tokens);
	}
	CHECK(rejected_random_inputs == 256);
	int deep_rejections = 0;
	for (int iteration = 0; iteration < 512; iteration++) {
		Vector<uint8_t> fuzzed = module;
		const int mutation_count = 1 + int(next_random() % 8);
		for (int mutation = 0; mutation < mutation_count; mutation++) {
			const int offset = payload_offset + int(next_random() % payload_size);
			fuzzed.write[offset] ^= uint8_t(1 + next_random() % 255);
		}
		write_u64_le(fuzzed, 36, GDScriptCompiledModule::fingerprint_bytes(fuzzed.ptr() + payload_offset, payload_size));
		if (GDScriptCompiledModule::verify(fuzzed) != OK) {
			deep_rejections++;
		}
		CHECK(GDScriptCompiledModule::extract_fallback(fuzzed, recovered_tokens) == OK);
		CHECK(recovered_tokens == tokens);
	}
	CHECK(deep_rejections > 480);

	source_target.unref();
	direct_target.unref();
	source_instance.unref();
	direct_instance.unref();
	source_script->clear();
	direct_script->clear();
	GDScriptCache::remove_script(script_path);
}

TEST_CASE("[Modules][GDScript] Portable compiled modules verify and relocate VM bytecode") {
	GDScriptLanguage::get_singleton()->init();
	const StringName symbolic_autoload = SNAME("__PortableCompiledModuleAutoload");
	const StringName relocated_autoload_slot = SNAME("__PortableCompiledModuleRelocatedSlot");
	const StringName symbolic_engine_singleton = SNAME("__PortableCompiledModuleEngineSingleton");
	const String symbolic_autoload_setting = "autoload/" + String(symbolic_autoload);
	Node *original_autoload = memnew(Node);
	original_autoload->set_meta(SNAME("slot"), 17);
	Node *relocated_autoload = memnew(Node);
	relocated_autoload->set_meta(SNAME("slot"), 29);
	struct AutoloadSettingGuard {
		String setting;
		StringName name;
		StringName relocated_name;
		StringName engine_singleton_name;
		Node *original = nullptr;
		Node *relocated = nullptr;
		~AutoloadSettingGuard() {
			ProjectSettings::get_singleton()->set_setting(setting, Variant());
			GDScriptLanguage::get_singleton()->add_global_constant(name, Variant());
			GDScriptLanguage::get_singleton()->add_global_constant(relocated_name, Variant());
			GDScriptLanguage::get_singleton()->add_global_constant(engine_singleton_name, Variant());
			Engine::get_singleton()->remove_singleton(engine_singleton_name);
			memdelete(original);
			memdelete(relocated);
		}
	} autoload_guard{ symbolic_autoload_setting, symbolic_autoload, relocated_autoload_slot, symbolic_engine_singleton, original_autoload, relocated_autoload };
	ProjectSettings::get_singleton()->set_setting(symbolic_autoload_setting, "*res://modules/gdscript/tests/scripts/lsp/local_variables.gd");
	GDScriptLanguage::get_singleton()->add_global_constant(symbolic_autoload, original_autoload);
	GDScriptLanguage::get_singleton()->add_global_constant(relocated_autoload_slot, relocated_autoload);
	Engine::get_singleton()->add_singleton(Engine::Singleton(symbolic_engine_singleton, original_autoload));
	GDScriptLanguage::get_singleton()->add_global_constant(symbolic_engine_singleton, original_autoload);
	const String module_path = OS::get_singleton()->get_temp_path().path_join("portable_gdscript_module.gdm");
	const String script_path = module_path.get_basename() + ".gd";
	Ref<GDScript> gdscript = memnew(GDScript);
	gdscript->set_path(script_path);
	const String metadata_source = R"(
@tool
@icon("res://portable_compiled_module.svg")
extends RefCounted

signal computed(value: int)

const SCALE := 2
var amount: int = 1:
	set(value):
		amount = value
	get:
		return amount
static var calls: int = 0

struct Coordinates:
	var x: float = 1.5
	var tint: Color = Color(0.25, 0.5, 0.75, 1.0)

struct Sample:
	var value: int = 3
	var coordinates: Coordinates

class Nested:
	var enabled: bool = true

	func state() -> bool:
		return enabled

@abstract class AbstractNested:
	pass

var nested_value: Nested

static func scale(value: int = SCALE) -> int:
	return value * SCALE

func count_values(...values: Array) -> int:
	return values.size()

func read_sample(sample: Sample) -> int:
	return sample.value

func read_nested(value: Nested) -> bool:
	return value.enabled

func make_offset(offset: int) -> Callable:
	return func(value: int) -> int: return value + offset

func autoload_value() -> int:
	return int(__PortableCompiledModuleAutoload.get_meta("slot"))

func input_singleton() -> Variant:
	return __PortableCompiledModuleEngineSingleton

@rpc("any_peer", "call_remote", "reliable")
func compute(value: int) -> String:
	calls += 1
	var text: String = str(scale(value))
	set_meta("compiled_module_result", text)
	return text.to_upper()
)";
	gdscript->set_source_code(metadata_source);
	REQUIRE(gdscript->reload() == OK);
	const Variant *amount_default = gdscript->get_member_default_values().getptr(SNAME("amount"));
	const Variant *calls_default = gdscript->get_member_default_values().getptr(SNAME("calls"));
	REQUIRE(amount_default != nullptr);
	REQUIRE(calls_default != nullptr);
	CHECK(*amount_default == Variant(1));
	CHECK(*calls_default == Variant(0));
	CHECK(gdscript->get_struct_layouts().size() == 2);
	const Ref<StructLayout> *sample_layout = gdscript->get_struct_layouts().getptr(SNAME("Sample"));
	REQUIRE(sample_layout != nullptr);
	CHECK((*sample_layout)->get_field_count() == 2);
	CHECK((*sample_layout)->get_default_value(0) == Variant(3));

	const Vector<uint8_t> tokens = gdscript->get_as_binary_tokens();
	REQUIRE_FALSE(tokens.is_empty());
	Vector<uint8_t> module;
	GDScriptCompiledModule::Summary summary;
	REQUIRE(GDScriptCompiledModule::create(gdscript.ptr(), tokens, module, &summary) == OK);
	CHECK_FALSE(module.is_empty());
	CHECK(summary.has_debug_info);
	Vector<uint8_t> stripped_module;
	GDScriptCompiledModule::Summary stripped_summary;
	REQUIRE(GDScriptCompiledModule::create(gdscript.ptr(), tokens, stripped_module, &stripped_summary,
			GDScriptCompiledModule::DEBUG_INFO_STRIPPED) == OK);
	CHECK_FALSE(stripped_summary.has_debug_info);
	CHECK(stripped_module.size() < module.size());
	String verifier_error;
	CHECK_MESSAGE(GDScriptCompiledModule::verify(module, &verifier_error) == OK, verifier_error);
	CHECK_MESSAGE(GDScriptCompiledModule::verify(stripped_module, &verifier_error) == OK, verifier_error);
	{
		HashMap<StringName, int> &global_map = const_cast<HashMap<StringName, int> &>(GDScriptLanguage::get_singleton()->get_global_map());
		const int original_index = global_map[symbolic_autoload];
		global_map.erase(symbolic_autoload);
		String unresolved_global_error;
		CHECK(GDScriptCompiledModule::verify(module, &unresolved_global_error) == ERR_INVALID_DATA);
		CHECK(unresolved_global_error.contains("symbolic global"));
		global_map.insert(symbolic_autoload, original_index);
	}
	CHECK(summary.engine_api_fingerprint == GDScriptCompiledModule::get_engine_api_fingerprint());
	CHECK(summary.source_fingerprint == GDScriptCompiledModule::fingerprint_source(gdscript->get_source_code()));
	CHECK(summary.module_fingerprint != 0);
	CHECK(summary.schema_fingerprint != 0);
	CHECK(summary.dependency_fingerprint != 0);
	CHECK(summary.skipped_functions == 0);
	CHECK(summary.classes.size() == 3);
	for (const GDScriptCompiledModule::ClassSummary &script_class : summary.classes) {
		CHECK_FALSE(script_class.identity.is_empty());
		CHECK(script_class.metadata_fingerprint != 0);
	}
	CHECK_FALSE(summary.functions.is_empty());
	auto get_root_metadata_fingerprint = [](const GDScriptCompiledModule::Summary &p_summary) {
		for (const GDScriptCompiledModule::ClassSummary &script_class : p_summary.classes) {
			if (script_class.identity == "root") {
				return script_class.metadata_fingerprint;
			}
		}
		return uint64_t(0);
	};
	const uint64_t root_metadata_fingerprint = get_root_metadata_fingerprint(summary);
	REQUIRE(root_metadata_fingerprint != 0);

	auto create_changed_metadata_summary = [&](const String &p_source, GDScriptCompiledModule::Summary &r_changed_summary) {
		gdscript->set_source_code(p_source);
		if (gdscript->reload() != OK) {
			return false;
		}
		Vector<uint8_t> changed_module;
		return GDScriptCompiledModule::create(gdscript.ptr(), gdscript->get_as_binary_tokens(), changed_module, &r_changed_summary) == OK;
	};
	GDScriptCompiledModule::Summary changed_default_summary;
	REQUIRE(create_changed_metadata_summary(metadata_source.replace("var amount: int = 1", "var amount: int = 2"), changed_default_summary));
	CHECK(get_root_metadata_fingerprint(changed_default_summary) != root_metadata_fingerprint);
	GDScriptCompiledModule::Summary changed_struct_summary;
	REQUIRE(create_changed_metadata_summary(metadata_source.replace("var value: int = 3", "var value: int = 4"), changed_struct_summary));
	CHECK(get_root_metadata_fingerprint(changed_struct_summary) != root_metadata_fingerprint);
	GDScriptCompiledModule::Summary changed_method_summary;
	REQUIRE(create_changed_metadata_summary(metadata_source.replace("static func scale", "func scale"), changed_method_summary));
	CHECK(get_root_metadata_fingerprint(changed_method_summary) != root_metadata_fingerprint);
	GDScriptCompiledModule::Summary changed_flag_summary;
	REQUIRE(create_changed_metadata_summary(metadata_source.replace("@tool\n", ""), changed_flag_summary));
	CHECK(get_root_metadata_fingerprint(changed_flag_summary) != root_metadata_fingerprint);
	GDScriptCompiledModule::Summary changed_static_unload_summary;
	REQUIRE(create_changed_metadata_summary(metadata_source.replace("@tool\n", "@tool\n@static_unload\n"), changed_static_unload_summary));
	CHECK(get_root_metadata_fingerprint(changed_static_unload_summary) != root_metadata_fingerprint);
	GDScriptCompiledModule::Summary changed_icon_summary;
	REQUIRE(create_changed_metadata_summary(metadata_source.replace("portable_compiled_module.svg", "portable_compiled_module_alt.svg"), changed_icon_summary));
	CHECK(get_root_metadata_fingerprint(changed_icon_summary) != root_metadata_fingerprint);
	gdscript->set_source_code(metadata_source);
	REQUIRE(gdscript->reload() == OK);

	Vector<uint8_t> fallback;
	uint64_t source_fingerprint = 0;
	CHECK(GDScriptCompiledModule::extract_fallback(module, fallback, &source_fingerprint) == OK);
	CHECK(fallback == tokens);
	CHECK(source_fingerprint == summary.source_fingerprint);
	CHECK(GDScriptCompiledModule::apply(gdscript.ptr(), module) == OK);

	// Forge a checksum-valid module with a stale builtin API hash. The
	// independent verifier must reject it before shallow construction mutates
	// even the target script shell. Fallback extraction deliberately remains a
	// structural operation so older token-based loading can still recover.
	auto read_u32_le = [](const Vector<uint8_t> &p_bytes, int p_offset) {
		return uint32_t(p_bytes[p_offset]) | (uint32_t(p_bytes[p_offset + 1]) << 8) | (uint32_t(p_bytes[p_offset + 2]) << 16) |
				(uint32_t(p_bytes[p_offset + 3]) << 24);
	};
	auto write_u32_le = [](Vector<uint8_t> &r_bytes, int p_offset, uint32_t p_value) {
		for (int byte = 0; byte < 4; byte++) {
			r_bytes.write[p_offset + byte] = uint8_t(p_value >> (byte * 8));
		}
	};
	auto write_u64_le = [](Vector<uint8_t> &r_bytes, int p_offset, uint64_t p_value) {
		for (int byte = 0; byte < 8; byte++) {
			r_bytes.write[p_offset + byte] = uint8_t(p_value >> (byte * 8));
		}
	};
	const int module_fallback_size = read_u32_le(module, 52);
	const int module_payload_size = read_u32_le(module, 56);
	const int module_payload_offset = GDScriptCompiledModule::ENVELOPE_HEADER_SIZE + module_fallback_size;
	REQUIRE(module_payload_offset + module_payload_size == module.size());
	Vector<uint8_t> stale_api_module = module;
	const uint32_t to_upper_hash = Variant::get_builtin_method_hash(Variant::STRING, SNAME("to_upper"));
	int api_hash_offset = -1;
	for (int offset = module_payload_offset; offset + 4 <= stale_api_module.size(); offset++) {
		if (read_u32_le(stale_api_module, offset) == to_upper_hash) {
			REQUIRE(api_hash_offset == -1);
			api_hash_offset = offset;
		}
	}
	REQUIRE(api_hash_offset >= module_payload_offset);
	stale_api_module.write[api_hash_offset] ^= 1;
	write_u64_le(stale_api_module, 36, GDScriptCompiledModule::fingerprint_bytes(stale_api_module.ptr() + module_payload_offset, module_payload_size));
	String stale_api_error;
	GDScriptCompiledModule::Rejection stale_api_rejection;
	CHECK(GDScriptCompiledModule::verify(stale_api_module, &stale_api_error, &stale_api_rejection) == ERR_INVALID_DATA);
	CHECK(stale_api_error.contains("API hash"));
	CHECK(stale_api_rejection.reason == GDScriptCompiledModule::REJECTION_FAILED_RELOCATION);
	CHECK(stale_api_rejection.fallback_available);
	CHECK(GDScriptCompiledModule::extract_fallback(stale_api_module, fallback) == OK);
	CHECK(fallback == tokens);

	int compatibility_case = 0;
	auto check_compatible_fallback = [&](const Vector<uint8_t> &p_rejected_module, GDScriptCompiledModule::RejectionReason p_reason) {
		String rejection_error;
		GDScriptCompiledModule::Rejection rejection;
		CHECK(GDScriptCompiledModule::verify(p_rejected_module, &rejection_error, &rejection) != OK);
		CHECK(rejection.reason == p_reason);
		CHECK(rejection.fallback_available);
		Vector<uint8_t> recovered_tokens;
		REQUIRE(GDScriptCompiledModule::extract_fallback(p_rejected_module, recovered_tokens) == OK);
		CHECK(recovered_tokens == tokens);

		Ref<GDScript> fallback_candidate = memnew(GDScript);
		const String fallback_path = module_path.get_basename() + "_compat_" + itos(compatibility_case++) + ".gd";
		fallback_candidate->set_path(fallback_path);
		fallback_candidate->set_binary_tokens_source(recovered_tokens);
		fallback_candidate->set_compiled_module_source(p_rejected_module);
		REQUIRE(fallback_candidate->reload() == OK);
		CHECK(fallback_candidate->is_valid());
		CHECK(fallback_candidate->get_compiled_module_fallback_reason().contains(GDScriptCompiledModule::get_rejection_reason_name(p_reason)));
		Ref<RefCounted> fallback_instance = memnew(RefCounted);
		fallback_instance->set_script(fallback_candidate);
		CHECK(String(fallback_instance->call(SNAME("compute"), 2)) == "4");
		fallback_instance.unref();
		fallback_candidate->clear();
		GDScriptCache::remove_script(fallback_path);
		fallback_candidate.unref();
	};

	Vector<uint8_t> format_mismatch_module = module;
	write_u32_le(format_mismatch_module, 8, GDScriptCompiledModule::FORMAT_VERSION + 1);
	check_compatible_fallback(format_mismatch_module, GDScriptCompiledModule::REJECTION_FORMAT_VERSION);
	Vector<uint8_t> bytecode_mismatch_module = module;
	write_u32_le(bytecode_mismatch_module, 12, GDScriptCompiledModule::BYTECODE_VERSION + 1);
	check_compatible_fallback(bytecode_mismatch_module, GDScriptCompiledModule::REJECTION_BYTECODE_VERSION);
	Vector<uint8_t> engine_mismatch_module = module;
	write_u64_le(engine_mismatch_module, 20, GDScriptCompiledModule::get_engine_api_fingerprint() ^ 1);
	check_compatible_fallback(engine_mismatch_module, GDScriptCompiledModule::REJECTION_ENGINE_API);
	Vector<uint8_t> unsupported_feature_module = module;
	write_u32_le(unsupported_feature_module, 16, GDScriptCompiledModule::ENVELOPE_FLAG_HAS_FALLBACK | (1 << 8));
	check_compatible_fallback(unsupported_feature_module, GDScriptCompiledModule::REJECTION_UNSUPPORTED_FEATURE);

	Vector<uint8_t> unknown_metadata_module = module;
	unknown_metadata_module.push_back(0);
	write_u32_le(unknown_metadata_module, 56, module_payload_size + 1);
	write_u64_le(unknown_metadata_module, 36,
			GDScriptCompiledModule::fingerprint_bytes(unknown_metadata_module.ptr() + module_payload_offset, module_payload_size + 1));
	check_compatible_fallback(unknown_metadata_module, GDScriptCompiledModule::REJECTION_UNKNOWN_METADATA);

	Vector<uint8_t> invalid_bytecode_module = module;
	int bytecode_fingerprint_offset = -1;
	for (const GDScriptCompiledModule::FunctionSummary &function : summary.functions) {
		int candidate_offset = -1;
		int matches = 0;
		for (int offset = module_payload_offset; offset + 4 <= module.size(); offset++) {
			if (read_u32_le(module, offset) == function.bytecode_fingerprint) {
				candidate_offset = offset;
				matches++;
			}
		}
		if (matches == 1) {
			bytecode_fingerprint_offset = candidate_offset;
			break;
		}
	}
	REQUIRE(bytecode_fingerprint_offset >= module_payload_offset);
	write_u32_le(invalid_bytecode_module, bytecode_fingerprint_offset, read_u32_le(invalid_bytecode_module, bytecode_fingerprint_offset) ^ 1);
	write_u64_le(invalid_bytecode_module, 36,
			GDScriptCompiledModule::fingerprint_bytes(invalid_bytecode_module.ptr() + module_payload_offset, module_payload_size));
	check_compatible_fallback(invalid_bytecode_module, GDScriptCompiledModule::REJECTION_INVALID_BYTECODE);
	check_compatible_fallback(stale_api_module, GDScriptCompiledModule::REJECTION_FAILED_RELOCATION);

	// Exercise the actual .gdm resource path, where fallback extraction must be
	// independent of the rejected payload format.
	const String incompatible_module_path = module_path.get_basename() + "_future.gdm";
	{
		Ref<FileAccess> incompatible_file = FileAccess::open(incompatible_module_path, FileAccess::WRITE);
		REQUIRE(incompatible_file.is_valid());
		REQUIRE(incompatible_file->store_buffer(format_mismatch_module));
	}
	Ref<GDScript> incompatible_resource = ResourceLoader::load(incompatible_module_path, "GDScript", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(incompatible_resource.is_valid());
	CHECK(incompatible_resource->is_valid());
	CHECK(incompatible_resource->get_compiled_module_fallback_reason().contains("format-version mismatch"));
	incompatible_resource->clear();
	GDScriptCache::remove_script(incompatible_module_path);
	incompatible_resource.unref();
	CHECK(DirAccess::remove_absolute(incompatible_module_path) == OK);

	Ref<GDScript> rejected_script = memnew(GDScript);
	const String rejected_path = "res://__unverified_module_must_not_mutate.gd";
	rejected_script->set_path(rejected_path);
	rejected_script->set_binary_tokens_source(tokens);
	CHECK(GDScriptCompiledModule::prepare_shallow(rejected_script.ptr(), stale_api_module, &stale_api_error) == ERR_INVALID_DATA);
	CHECK(rejected_script->get_path() == rejected_path);
	CHECK(rejected_script->get_subclasses().is_empty());

	// Reconstruct a second runtime graph using only the portable records. No
	// parser, analyzer, compiler, or freshly compiled verification oracle is
	// available on this object.
	Ref<GDScript> direct_script = memnew(GDScript);
	direct_script->set_path(module_path.get_basename() + "_direct.gd");
	direct_script->set_binary_tokens_source(tokens);
	String direct_error;
	Ref<RefCounted> direct_instance;
	{
		// Simulate another process assigning a different global-array index to
		// the same autoload name. Portable bytecode must resolve the name at
		// load time; copying the compiler's original numeric index returns 17.
		HashMap<StringName, int> &global_map = const_cast<HashMap<StringName, int> &>(GDScriptLanguage::get_singleton()->get_global_map());
		const int original_index = global_map[symbolic_autoload];
		struct GlobalIndexGuard {
			HashMap<StringName, int> &map;
			StringName name;
			int index;
			~GlobalIndexGuard() { map[name] = index; }
		} global_index_guard{ global_map, symbolic_autoload, original_index };
		global_map[symbolic_autoload] = global_map[relocated_autoload_slot];

		REQUIRE_MESSAGE(GDScriptCompiledModule::prepare_shallow(direct_script.ptr(), module, &direct_error) == OK, direct_error);
		REQUIRE_MESSAGE(GDScriptCompiledModule::build_runtime(direct_script.ptr(), module, true, &direct_error) == OK, direct_error);
		CHECK(direct_script->is_valid());
		CHECK(direct_script->is_tool());
		CHECK(direct_script->get_subclasses().size() == 2);
		CHECK(direct_script->get_struct_layouts().size() == 2);
		CHECK(direct_script->get_member_line(SNAME("compute")) == gdscript->get_member_line(SNAME("compute")));
		CHECK(direct_script->get_member_line(SNAME("compute")) > 0);
		const GDScriptFunction *const *original_compute_ptr = gdscript->get_member_functions().getptr(SNAME("compute"));
		const GDScriptFunction *const *direct_compute_ptr = direct_script->get_member_functions().getptr(SNAME("compute"));
		REQUIRE(original_compute_ptr != nullptr);
		REQUIRE(direct_compute_ptr != nullptr);
		const GDScriptFunction *original_compute = *original_compute_ptr;
		const GDScriptFunction *direct_compute = *direct_compute_ptr;
		CHECK(direct_compute->get_name() == SNAME("compute"));
		CHECK(direct_compute->get_source() == GDScript::canonicalize_path(script_path));
		CHECK(direct_compute->get_debug_profile_identifier() == original_compute->get_debug_profile_identifier());
		CHECK_FALSE(direct_compute->get_debug_source_positions().is_empty());
		bool has_column = false;
		for (const GDScriptFunction::SourcePosition &position : direct_compute->get_debug_source_positions()) {
			has_column = has_column || position.column > 0;
		}
		CHECK(has_column);
		bool has_value_local = false;
		bool has_text_local = false;
		for (const GDScriptFunction::StackDebug &local : direct_compute->get_debug_stack_entries()) {
			has_value_local = has_value_local || (local.added && local.identifier == SNAME("value"));
			has_text_local = has_text_local || (local.added && local.identifier == SNAME("text"));
		}
		CHECK(has_value_local);
		CHECK(has_text_local);
		const Variant *direct_amount_default = direct_script->get_member_default_values().getptr(SNAME("amount"));
		REQUIRE(direct_amount_default != nullptr);
		CHECK(*direct_amount_default == Variant(1));
		direct_instance = memnew(RefCounted);
		direct_instance->set_script(direct_script);
		CHECK(String(direct_instance->call(SNAME("compute"), 5)) == "10");
		CHECK(int(direct_instance->call(SNAME("autoload_value"))) == 29);
		CHECK(direct_instance->call(SNAME("input_singleton")) == GDScriptLanguage::get_singleton()->get_any_global_constant(symbolic_engine_singleton));
	}
	Callable offset = direct_instance->call(SNAME("make_offset"), 4);
	CHECK(int(offset.call(6)) == 10);
	const Ref<GDScript> *direct_nested_script = direct_script->get_subclasses().getptr(SNAME("Nested"));
	REQUIRE(direct_nested_script != nullptr);
	Ref<RefCounted> direct_nested = memnew(RefCounted);
	direct_nested->set_script(*direct_nested_script);
	CHECK(bool(direct_instance->call(SNAME("read_nested"), direct_nested)));
	CHECK(int(direct_instance->call(SNAME("count_values"), 1, 2, 3)) == 3);
	direct_instance->set(SNAME("amount"), 9);
	CHECK(int(direct_instance->get(SNAME("amount"))) == 9);
	Ref<GDScript> stripped_direct_script = memnew(GDScript);
	stripped_direct_script->set_path(module_path.get_basename() + "_stripped_direct.gd");
	stripped_direct_script->set_binary_tokens_source(tokens);
	REQUIRE_MESSAGE(GDScriptCompiledModule::build_runtime(stripped_direct_script.ptr(), stripped_module, false, &direct_error) == OK, direct_error);
	const GDScriptFunction *const *stripped_compute_ptr = stripped_direct_script->get_member_functions().getptr(SNAME("compute"));
	REQUIRE(stripped_compute_ptr != nullptr);
	const GDScriptFunction *stripped_compute = *stripped_compute_ptr;
	CHECK(stripped_compute->get_debug_source_positions().is_empty());
	CHECK(stripped_compute->get_debug_stack_entries().is_empty());
	Ref<RefCounted> stripped_instance = memnew(RefCounted);
	stripped_instance->set_script(stripped_direct_script);
	CHECK(String(stripped_instance->call(SNAME("compute"), 3)) == "6");
	direct_script->set_compiled_module_source(module);
	REQUIRE(direct_script->reload(true) == OK);
	CHECK(int(direct_instance->get(SNAME("amount"))) == 9);
	CHECK(direct_script->get_compiled_module_fallback_reason().is_empty());

	Ref<RefCounted> instance = memnew(RefCounted);
	instance->set_script(gdscript);
	CHECK(String(instance->call(SNAME("compute"), 21)) == "42");
	CHECK(String(instance->get_meta(SNAME("compiled_module_result"))) == "42");

	{
		Ref<FileAccess> file = FileAccess::open(module_path, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		REQUIRE(file->store_buffer(module));
	}
	Ref<GDScript> loaded_module = ResourceLoader::load(module_path, "GDScript", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded_module.is_valid());
	String loaded_module_error;
	CHECK_MESSAGE(GDScriptCompiledModule::apply(loaded_module.ptr(), module, &loaded_module_error) == OK, loaded_module_error);
	CHECK(loaded_module->get_binary_tokens_source() == tokens);
	CHECK(loaded_module->get_compiled_module_source() == module);
	Ref<RefCounted> loaded_instance = memnew(RefCounted);
	loaded_instance->set_script(loaded_module);
	CHECK(String(loaded_instance->call(SNAME("compute"), 7)) == "14");

	Ref<GDScript> wrong_path_script = memnew(GDScript);
	wrong_path_script->set_path(module_path.get_basename() + "_other.gd");
	wrong_path_script->set_source_code(gdscript->get_source_code());
	REQUIRE(wrong_path_script->reload() == OK);
	String metadata_error;
	CHECK(GDScriptCompiledModule::apply(wrong_path_script.ptr(), module, &metadata_error) == ERR_INVALID_DATA);
	CHECK(metadata_error.contains("Class metadata verification failed"));

	Vector<uint8_t> corrupt = module;
	corrupt.write[corrupt.size() - 1] ^= 0x80;
	CHECK(GDScriptCompiledModule::extract_fallback(corrupt, fallback) == OK);
	CHECK(fallback == tokens);
	CHECK(GDScriptCompiledModule::verify(corrupt) == ERR_FILE_CORRUPT);
	Ref<GDScript> fallback_script = memnew(GDScript);
	fallback_script->set_path(module_path.get_basename() + "_fallback.gd");
	fallback_script->set_binary_tokens_source(tokens);
	fallback_script->set_compiled_module_source(corrupt);
	REQUIRE(fallback_script->reload() == OK);
	CHECK(fallback_script->get_compiled_module_fallback_reason().contains("corrupt module"));
	CHECK(fallback_script->get_compiled_module_fallback_reason().contains("checksum mismatch"));

	Ref<GDScript> changed_script = memnew(GDScript);
	changed_script->set_source_code("extends RefCounted\nfunc compute(value: int) -> String:\n\treturn str(value + 1)\n");
	REQUIRE(changed_script->reload() == OK);
	CHECK(GDScriptCompiledModule::apply(changed_script.ptr(), module) == ERR_INVALID_DATA);

	Ref<GDScript> cached_script = memnew(GDScript);
	const String cached_script_path = "res://__portable_gdscript_cache_test.gd";
	cached_script->set_path(cached_script_path);
	cached_script->set_source_code("extends RefCounted\nfunc cached(value: int) -> int:\n\treturn value * 3\n");
	REQUIRE(cached_script->reload() == OK);
	const Vector<uint8_t> cached_tokens = cached_script->get_as_binary_tokens();
	REQUIRE(GDScriptCompiledModule::save_editor_cache(cached_script.ptr(), cached_tokens) == OK);
	Vector<uint8_t> cached_module;
	CHECK(GDScriptCompiledModule::load_editor_cache(cached_script_path, cached_script->get_source_code(), cached_module) == OK);
	CHECK_FALSE(cached_module.is_empty());
	String cache_invalidation_error;
	CHECK(GDScriptCompiledModule::load_editor_cache(cached_script_path, cached_script->get_source_code() + "\n# stale", cached_module, &cache_invalidation_error) == ERR_INVALID_DATA);
	CHECK(cache_invalidation_error.contains("source fingerprint changed"));
	CHECK(DirAccess::remove_absolute(GDScriptCompiledModule::get_editor_cache_path(cached_script_path)) == OK);

	Vector<GDScriptCompiledModule::Summary> modules;
	modules.push_back(summary);
	CHECK_FALSE(GDScriptCompiledModule::create_project_manifest(modules).is_empty());

	offset = Callable();
	stripped_instance.unref();
	stripped_direct_script->clear();
	stripped_direct_script.unref();
	direct_nested.unref();
	instance.unref();
	direct_instance.unref();
	loaded_instance.unref();
	gdscript->clear();
	direct_script->clear();
	loaded_module->clear();
	wrong_path_script->clear();
	changed_script->clear();
	cached_script->clear();
	rejected_script->clear();
	fallback_script->clear();
	GDScriptCache::remove_script(module_path);
	GDScriptCache::remove_script(script_path);
	GDScriptCache::remove_script(module_path.get_basename() + "_other.gd");
	GDScriptCache::remove_script(module_path.get_basename() + "_direct.gd");
	GDScriptCache::remove_script(module_path.get_basename() + "_stripped_direct.gd");
	GDScriptCache::remove_script(module_path.get_basename() + "_fallback.gd");
	GDScriptCache::remove_script(cached_script_path);
	gdscript.unref();
	direct_script.unref();
	loaded_module.unref();
	wrong_path_script.unref();
	changed_script.unref();
	cached_script.unref();
	rejected_script.unref();
	fallback_script.unref();
	CHECK(DirAccess::remove_absolute(module_path) == OK);
}

TEST_CASE("[Modules][GDScript] Compiled module registry resolves cycles and transitively invalidates dependencies") {
	GDScriptLanguage::get_singleton()->init();
	const String temporary_directory = OS::get_singleton()->get_temp_path();
	const String leaf_path = temporary_directory.path_join("portable_module_dependency_leaf.gd");
	const String middle_path = temporary_directory.path_join("portable_module_dependency_middle.gd");
	const String root_path = temporary_directory.path_join("portable_module_dependency_root.gd");
	const String cycle_a_path = temporary_directory.path_join("portable_module_dependency_cycle_a.gd");
	const String cycle_b_path = temporary_directory.path_join("portable_module_dependency_cycle_b.gd");
	const String future_module_path = temporary_directory.path_join("portable_module_dependency_future.gdm");
	struct DependencyFileGuard {
		Vector<String> paths;
		~DependencyFileGuard() {
			for (const String &path : paths) {
				Ref<GDScript> script = GDScriptCache::get_cached_script(path);
				if (script.is_valid()) {
					script->clear();
				}
				GDScriptCache::remove_script(path);
				const String cache_path = GDScriptCompiledModule::get_editor_cache_path(path);
				if (FileAccess::exists(cache_path)) {
					DirAccess::remove_absolute(cache_path);
				}
				if (FileAccess::exists(path)) {
					DirAccess::remove_absolute(path);
				}
			}
		}
	} dependency_file_guard{ { leaf_path, middle_path, root_path, cycle_a_path, cycle_b_path, future_module_path } };

	auto write_source = [](const String &p_path, const String &p_source) {
		Error error = OK;
		Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
		if (file.is_null() || error != OK) {
			return false;
		}
		file->store_string(p_source);
		return file->get_error() == OK;
	};
	const String leaf_source = "extends RefCounted\nstruct Payload:\n\tvar value: int = 1\nfunc leaf_value() -> int:\n\treturn 1\n";
	const String middle_source = "extends \"" + leaf_path + "\"\nfunc middle_value() -> int:\n\treturn leaf_value() + 1\n";
	const String root_source = "extends \"" + middle_path + "\"\nfunc root_value() -> int:\n\treturn middle_value() + 1\n";
	REQUIRE(write_source(leaf_path, leaf_source));
	REQUIRE(write_source(middle_path, middle_source));
	REQUIRE(write_source(root_path, root_source));

	Error load_error = OK;
	Ref<GDScript> root_script = GDScriptCache::get_full_script(root_path, load_error);
	REQUIRE_MESSAGE(load_error == OK, "Could not load transitive dependency test scripts.");
	REQUIRE(root_script.is_valid());
	Ref<GDScript> middle_script = GDScriptCache::get_cached_script(middle_path);
	Ref<GDScript> leaf_script = GDScriptCache::get_cached_script(leaf_path);
	REQUIRE(middle_script.is_valid());
	REQUIRE(leaf_script.is_valid());

	auto save_cache = [](const Ref<GDScript> &p_script) {
		Vector<uint8_t> module;
		const Error error = GDScriptCompiledModule::save_editor_cache(p_script.ptr(), p_script->get_as_binary_tokens(), &module);
		if (error == OK) {
			p_script->set_compiled_module_source(module);
		}
		return error;
	};
	REQUIRE(save_cache(leaf_script) == OK);
	REQUIRE(save_cache(middle_script) == OK);
	Vector<uint8_t> root_module;
	GDScriptCompiledModule::Summary root_summary;
	REQUIRE(GDScriptCompiledModule::create(root_script.ptr(), root_script->get_as_binary_tokens(), root_module, &root_summary) == OK);
	REQUIRE(save_cache(root_script) == OK);
	CHECK(root_summary.module_fingerprint != 0);
	CHECK(root_summary.schema_fingerprint != 0);
	CHECK(root_summary.dependency_fingerprint != 0);
	REQUIRE(root_summary.dependencies.size() == 2);
	CHECK(root_summary.dependencies[0].path == leaf_path);
	CHECK(root_summary.dependencies[1].path == middle_path);
	for (const GDScriptCompiledModule::Dependency &dependency : root_summary.dependencies) {
		CHECK(dependency.source_fingerprint != 0);
		CHECK(dependency.module_fingerprint != 0);
		CHECK(dependency.schema_fingerprint != 0);
		CHECK(dependency.engine_api_fingerprint == GDScriptCompiledModule::get_engine_api_fingerprint());
	}
	Vector<GDScriptCompiledModule::Dependency> serialized_dependencies;
	REQUIRE(GDScriptCompiledModule::get_dependencies(root_module, serialized_dependencies) == OK);
	REQUIRE(serialized_dependencies.size() == root_summary.dependencies.size());
	for (int dependency_index = 0; dependency_index < serialized_dependencies.size(); dependency_index++) {
		CHECK(serialized_dependencies[dependency_index].path == root_summary.dependencies[dependency_index].path);
		CHECK(serialized_dependencies[dependency_index].module_fingerprint == root_summary.dependencies[dependency_index].module_fingerprint);
	}
	Vector<uint8_t> future_module = root_module;
	for (int byte = 0; byte < 4; byte++) {
		future_module.write[8 + byte] = uint8_t((GDScriptCompiledModule::FORMAT_VERSION + 1) >> (byte * 8));
	}
	{
		Ref<FileAccess> future_file = FileAccess::open(future_module_path, FileAccess::WRITE);
		REQUIRE(future_file.is_valid());
		REQUIRE(future_file->store_buffer(future_module));
	}
	ResourceFormatLoaderGDScript dependency_loader;
	List<String> fallback_dependencies;
	dependency_loader.get_dependencies(future_module_path, &fallback_dependencies);
	CHECK(fallback_dependencies.find(middle_path) != nullptr);
	String registry_error;
	CHECK_MESSAGE(GDScriptCompiledModule::verify(root_module, &registry_error) == OK, registry_error);
	Ref<GDScript> direct_root = memnew(GDScript);
	direct_root->set_path(root_path + ".direct");
	direct_root->set_binary_tokens_source(root_script->get_as_binary_tokens());
	REQUIRE_MESSAGE(GDScriptCompiledModule::prepare_shallow(direct_root.ptr(), root_module, &registry_error) == OK, registry_error);
	REQUIRE_MESSAGE(GDScriptCompiledModule::build_runtime(direct_root.ptr(), root_module, false, &registry_error) == OK, registry_error);
	Ref<RefCounted> direct_root_instance = memnew(RefCounted);
	direct_root_instance->set_script(direct_root);
	CHECK(int(direct_root_instance->call(SNAME("root_value"))) == 3);
	direct_root_instance.unref();
	direct_root->clear();
	Vector<uint8_t> loaded_root_cache;
	CHECK(GDScriptCompiledModule::load_editor_cache(root_path, root_source, loaded_root_cache) == OK);

	const String changed_leaf_source = leaf_source.replace("var value: int = 1", "var value: String = \"changed\"");
	REQUIRE(write_source(leaf_path, changed_leaf_source));
	CHECK(GDScriptCompiledModule::load_editor_cache(root_path, root_source, loaded_root_cache) == ERR_INVALID_DATA);
	GDScriptCompiledModule::Rejection dependency_rejection;
	CHECK(GDScriptCompiledModule::verify(root_module, &registry_error, &dependency_rejection) == ERR_INVALID_DATA);
	CHECK(dependency_rejection.reason == GDScriptCompiledModule::REJECTION_CHANGED_DEPENDENCY);
	CHECK(dependency_rejection.fallback_available);
	Vector<uint8_t> root_fallback_tokens;
	REQUIRE(GDScriptCompiledModule::extract_fallback(root_module, root_fallback_tokens) == OK);
	Ref<GDScript> dependency_fallback = memnew(GDScript);
	const String dependency_fallback_path = root_path + ".fallback";
	dependency_fallback->set_path(dependency_fallback_path);
	dependency_fallback->set_binary_tokens_source(root_fallback_tokens);
	dependency_fallback->set_compiled_module_source(root_module);
	REQUIRE(dependency_fallback->reload() == OK);
	CHECK(dependency_fallback->is_valid());
	CHECK(dependency_fallback->get_compiled_module_fallback_reason().contains("changed dependency"));
	dependency_fallback->clear();
	GDScriptCache::remove_script(dependency_fallback_path);
	dependency_fallback.unref();
	REQUIRE(write_source(leaf_path, leaf_source));

	const String cycle_a_source = "extends RefCounted\nconst Peer = preload(\"" + cycle_b_path + "\")\nfunc value_a() -> int:\n\treturn 1\n";
	const String cycle_b_source = "extends RefCounted\nconst Peer = preload(\"" + cycle_a_path + "\")\nfunc value_b() -> int:\n\treturn 2\n";
	REQUIRE(write_source(cycle_a_path, cycle_a_source));
	REQUIRE(write_source(cycle_b_path, cycle_b_source));
	Ref<GDScript> cycle_a = GDScriptCache::get_full_script(cycle_a_path, load_error);
	REQUIRE_MESSAGE(load_error == OK, "Could not load mutually dependent script A.");
	REQUIRE(cycle_a.is_valid());
	Ref<GDScript> cycle_b = GDScriptCache::get_cached_script(cycle_b_path);
	REQUIRE(cycle_b.is_valid());
	REQUIRE(save_cache(cycle_b) == OK);
	REQUIRE(save_cache(cycle_a) == OK);
	Vector<uint8_t> cycle_module;
	GDScriptCompiledModule::Summary cycle_summary;
	REQUIRE(GDScriptCompiledModule::create(cycle_a.ptr(), cycle_a->get_as_binary_tokens(), cycle_module, &cycle_summary) == OK);
	REQUIRE(cycle_summary.dependencies.size() == 1);
	CHECK(cycle_summary.dependencies[0].path == cycle_b_path);
	CHECK_MESSAGE(GDScriptCompiledModule::verify(cycle_module, &registry_error) == OK, registry_error);
	Vector<GDScriptCompiledModule::Summary> ordered_summaries{ root_summary, cycle_summary };
	Vector<GDScriptCompiledModule::Summary> reversed_summaries{ cycle_summary, root_summary };
	CHECK(GDScriptCompiledModule::create_project_manifest(ordered_summaries) == GDScriptCompiledModule::create_project_manifest(reversed_summaries));
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
