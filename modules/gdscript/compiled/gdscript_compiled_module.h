/**************************************************************************/
/*  gdscript_compiled_module.h                                            */
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

#pragma once

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

class GDScript;

// A portable container for GDScript VM bytecode. The serialized form never
// contains process addresses. Pointer tables used by the VM are represented by
// symbolic relocations and resolved only after all fingerprints are checked.
class GDScriptCompiledModule {
public:
	struct Dependency {
		String path;
		uint64_t source_fingerprint = 0;
		uint64_t module_fingerprint = 0;
		uint64_t schema_fingerprint = 0;
		uint64_t engine_api_fingerprint = 0;

		bool operator<(const Dependency &p_other) const { return path < p_other.path; }
	};

	struct FunctionSummary {
		String identity;
		uint32_t bytecode_fingerprint = 0;
		bool profile_guided = false;

		bool operator<(const FunctionSummary &p_other) const { return identity < p_other.identity; }
	};

	struct ClassSummary {
		String identity;
		uint64_t metadata_fingerprint = 0;

		bool operator<(const ClassSummary &p_other) const { return identity < p_other.identity; }
	};

	struct Summary {
		String path;
		uint64_t source_fingerprint = 0;
		uint64_t module_fingerprint = 0;
		uint64_t schema_fingerprint = 0;
		uint64_t dependency_fingerprint = 0;
		uint64_t engine_api_fingerprint = 0;
		Vector<Dependency> dependencies;
		Vector<ClassSummary> classes;
		Vector<FunctionSummary> functions;
		int skipped_functions = 0;
		bool has_debug_info = false;

		bool operator<(const Summary &p_other) const { return path < p_other.path; }
	};

	// The envelope remains stable across payload format revisions so an older
	// engine can recover the embedded token stream without understanding newer
	// class metadata or bytecode.
	static constexpr uint32_t ENVELOPE_VERSION = 1;
	static constexpr uint32_t ENVELOPE_HEADER_SIZE = 60;
	static constexpr uint32_t ENVELOPE_FLAG_HAS_FALLBACK = 1 << 0;
	static constexpr uint32_t FORMAT_VERSION = 7;
	static constexpr uint32_t DEBUG_INFO_VERSION = 1;
	// Version 2 stores OPCODE_STORE_GLOBAL operands as indices into the
	// function's symbolic name table. They are relocated to the running
	// language's global-array indices only after module verification.
	static constexpr uint32_t BYTECODE_VERSION = 2;

	static uint64_t fingerprint_bytes(const uint8_t *p_data, uint64_t p_size);
	static uint64_t fingerprint_source(const String &p_source);
	static uint64_t get_engine_api_fingerprint();

	enum DebugInfoMode {
		DEBUG_INFO_FULL,
		DEBUG_INFO_STRIPPED,
	};

	enum RejectionReason {
		REJECTION_NONE,
		REJECTION_FORMAT_VERSION,
		REJECTION_BYTECODE_VERSION,
		REJECTION_ENGINE_API,
		REJECTION_CHANGED_DEPENDENCY,
		REJECTION_UNKNOWN_METADATA,
		REJECTION_FAILED_RELOCATION,
		REJECTION_INVALID_BYTECODE,
		REJECTION_UNSUPPORTED_FEATURE,
		REJECTION_SOURCE_MISMATCH,
		REJECTION_CORRUPT_MODULE,
	};

	struct Rejection {
		RejectionReason reason = REJECTION_NONE;
		String detail;
		bool fallback_available = false;

		String describe() const;
	};

	static String get_rejection_reason_name(RejectionReason p_reason);

	static Error create(GDScript *p_script, const Vector<uint8_t> &p_fallback_tokens, Vector<uint8_t> &r_module, Summary *r_summary = nullptr,
			DebugInfoMode p_debug_info = DEBUG_INFO_FULL);
	static Error extract_fallback(const Vector<uint8_t> &p_module, Vector<uint8_t> &r_fallback_tokens, uint64_t *r_source_fingerprint = nullptr, String *r_error = nullptr);
	static Error get_dependencies(const Vector<uint8_t> &p_module, Vector<Dependency> &r_dependencies, String *r_error = nullptr);
	// Performs pointer-free structural, semantic, control-flow, type, and
	// symbolic-relocation validation. The deterministic module registry also
	// validates the complete dependency closure and handles metadata cycles.
	static Error verify(const Vector<uint8_t> &p_module, String *r_error = nullptr, Rejection *r_rejection = nullptr);
	// Creates only the nested GDScript resource graph. This lets cyclic
	// dependencies resolve class identities before either module is fully
	// loaded, without invoking the parser for a valid module.
	static Error prepare_shallow(GDScript *p_script, const Vector<uint8_t> &p_module, String *r_error = nullptr, Rejection *r_rejection = nullptr);
	// Builds the complete runtime class/function graph directly from portable
	// metadata and verified bytecode.
	static Error build_runtime(GDScript *p_script, const Vector<uint8_t> &p_module, bool p_keep_state, String *r_error = nullptr, Rejection *r_rejection = nullptr);
	static Error apply(GDScript *p_script, const Vector<uint8_t> &p_module, String *r_error = nullptr, Rejection *r_rejection = nullptr);

	enum TestBytecodeMutation {
		TEST_MUTATE_UNKNOWN_OPCODE,
		TEST_MUTATE_TRUNCATED_INSTRUCTION,
		TEST_MUTATE_INVALID_JUMP_TARGET,
		TEST_MUTATE_INVALID_FRAME_SLOT,
		TEST_MUTATE_INVALID_TYPED_FRAME_SLOT,
		TEST_MUTATE_INVALID_CONSTANT_INDEX,
		TEST_MUTATE_INVALID_NAME_INDEX,
		TEST_MUTATE_INVALID_FUNCTION_INDEX,
		TEST_MUTATE_INVALID_ARGUMENT_COUNT,
		TEST_MUTATE_INVALID_STRUCT_FIELD_INDEX,
		TEST_MUTATE_INVALID_NATIVE_API_RELOCATION,
	};

	// Produces a checksum-valid but verifier-invalid module. This keeps hostile
	// bytecode tests independent of the serialized metadata layout.
	static Error make_test_bytecode_mutation(const Vector<uint8_t> &p_module, TestBytecodeMutation p_mutation,
			Vector<uint8_t> &r_module, String *r_opcode = nullptr);

	static String get_editor_cache_path(const String &p_script_path);
	static Error load_editor_cache(const String &p_script_path, const String &p_source, Vector<uint8_t> &r_module, String *r_error = nullptr);
	static Error save_editor_cache(GDScript *p_script, const Vector<uint8_t> &p_fallback_tokens, Vector<uint8_t> *r_module = nullptr);

	// The export manifest is the output of the first whole-project pass. It
	// records the dependency graph, accepted bytecode fingerprints, and which
	// functions consume PGO hints, without embedding target-specific pointers.
	static Vector<uint8_t> create_project_manifest(Vector<Summary> p_modules);
};
