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
		uint64_t engine_api_fingerprint = 0;
		Vector<Dependency> dependencies;
		Vector<ClassSummary> classes;
		Vector<FunctionSummary> functions;
		int skipped_functions = 0;

		bool operator<(const Summary &p_other) const { return path < p_other.path; }
	};

	static constexpr uint32_t FORMAT_VERSION = 4;
	static constexpr uint32_t BYTECODE_VERSION = 1;

	static uint64_t fingerprint_bytes(const uint8_t *p_data, uint64_t p_size);
	static uint64_t fingerprint_source(const String &p_source);
	static uint64_t get_engine_api_fingerprint();

	static Error create(GDScript *p_script, const Vector<uint8_t> &p_fallback_tokens, Vector<uint8_t> &r_module, Summary *r_summary = nullptr);
	static Error extract_fallback(const Vector<uint8_t> &p_module, Vector<uint8_t> &r_fallback_tokens, uint64_t *r_source_fingerprint = nullptr, String *r_error = nullptr);
	// Creates only the nested GDScript resource graph. This lets cyclic
	// dependencies resolve class identities before either module is fully
	// loaded, without invoking the parser for a valid module.
	static Error prepare_shallow(GDScript *p_script, const Vector<uint8_t> &p_module, String *r_error = nullptr);
	// Builds the complete runtime class/function graph directly from portable
	// metadata and verified bytecode.
	static Error build_runtime(GDScript *p_script, const Vector<uint8_t> &p_module, bool p_keep_state, String *r_error = nullptr);
	static Error apply(GDScript *p_script, const Vector<uint8_t> &p_module, String *r_error = nullptr);

	static String get_editor_cache_path(const String &p_script_path);
	static Error load_editor_cache(const String &p_script_path, const String &p_source, Vector<uint8_t> &r_module);
	static Error save_editor_cache(GDScript *p_script, const Vector<uint8_t> &p_fallback_tokens, Vector<uint8_t> *r_module = nullptr);

	// The export manifest is the output of the first whole-project pass. It
	// records the dependency graph, accepted bytecode fingerprints, and which
	// functions consume PGO hints, without embedding target-specific pointers.
	static Vector<uint8_t> create_project_manifest(Vector<Summary> p_modules);
};
