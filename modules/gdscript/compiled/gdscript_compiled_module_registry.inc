/**************************************************************************/
/*  gdscript_compiled_module_registry.inc                                 */
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

// Deterministic dependency loading and transitive verification.
class ModuleRegistry {
public:
	enum State : uint8_t {
		STATE_UNLOADED,
		STATE_LOADING_METADATA,
		STATE_RESOLVING,
		STATE_VERIFIED,
		STATE_READY,
	};

private:
	struct Entry {
		State state = STATE_UNLOADED;
		ParsedModule module;
	};

	HashMap<String, Entry> entries;
	String error;
	String root_path;
	GDScriptCompiledModule::RejectionReason rejection_reason = GDScriptCompiledModule::REJECTION_CHANGED_DEPENDENCY;

	Error fail(const String &p_message, GDScriptCompiledModule::RejectionReason p_reason = GDScriptCompiledModule::REJECTION_CHANGED_DEPENDENCY) {
		error = p_message;
		rejection_reason = p_reason;
		return ERR_INVALID_DATA;
	}

	bool metadata_matches(const GDScriptCompiledModule::Dependency &p_expected, const ParsedModule &p_actual) const {
		return p_actual.path == p_expected.path && p_actual.source_fingerprint == p_expected.source_fingerprint &&
				p_actual.module_fingerprint == p_expected.module_fingerprint && p_actual.schema_fingerprint == p_expected.schema_fingerprint &&
				p_actual.engine_api_fingerprint == p_expected.engine_api_fingerprint;
	}

	Error register_module(const String &p_path, const Vector<uint8_t> &p_bytes, const GDScriptCompiledModule::Dependency *p_expected = nullptr) {
		const String path = GDScript::canonicalize_path(p_path);
		Entry *existing = entries.getptr(path);
		if (existing != nullptr) {
			if (p_expected != nullptr && !metadata_matches(*p_expected, existing->module)) {
				return fail("Dependency metadata changed while resolving '" + path + "'.");
			}
			return OK;
		}

		Entry loading;
		loading.state = STATE_LOADING_METADATA;
		entries.insert(path, loading);
		Entry *entry = entries.getptr(path);
		String parse_error;
		if (parse_module(p_bytes, entry->module, &parse_error) != OK || entry->module.path != path) {
			entries.erase(path);
			return fail("Could not load dependency metadata for '" + path + "': " + parse_error);
		}
		if (p_expected != nullptr && !metadata_matches(*p_expected, entry->module)) {
			entries.erase(path);
			return fail("Source, module, API, or schema fingerprint mismatch for dependency '" + path + "'.");
		}
		return OK;
	}

	Error validate_source_dependency(const GDScriptCompiledModule::Dependency &p_dependency, const String &p_remapped) {
		Ref<GDScript> cached = GDScriptCache::get_cached_script(p_dependency.path);
		uint64_t source_fingerprint = 0;
		if (FileAccess::exists(p_remapped)) {
			source_fingerprint = GDScriptCompiledModule::fingerprint_source(FileAccess::get_file_as_string(p_remapped));
		} else if (cached.is_valid() && !cached->get_source_code().is_empty()) {
			source_fingerprint = GDScriptCompiledModule::fingerprint_source(cached->get_source_code());
		}
		if (source_fingerprint != p_dependency.source_fingerprint || p_dependency.engine_api_fingerprint != GDScriptCompiledModule::get_engine_api_fingerprint()) {
			return fail("Source or engine API fingerprint mismatch for dependency '" + p_dependency.path + "'.");
		}

		Vector<uint8_t> module_bytes;
		if (cached.is_valid()) {
			module_bytes = cached->get_compiled_module_source();
		}
#ifdef TOOLS_ENABLED
		if (module_bytes.is_empty()) {
			const String cache_path = GDScriptCompiledModule::get_editor_cache_path(p_dependency.path);
			if (FileAccess::exists(cache_path)) {
				module_bytes = FileAccess::get_file_as_bytes(cache_path);
			}
		}
#endif
		if (!module_bytes.is_empty()) {
			Error register_error = register_module(p_dependency.path, module_bytes, &p_dependency);
			return register_error == OK ? resolve_entry(p_dependency.path) : register_error;
		}

		if (cached.is_valid() && cached->is_valid()) {
			Vector<ClassRecord> classes;
			Vector<FunctionRecord> functions;
			if (!Internals::make_local_records(cached.ptr(), classes, functions) ||
					get_local_module_fingerprint(classes, functions) != p_dependency.module_fingerprint ||
					get_schema_fingerprint(classes) != p_dependency.schema_fingerprint) {
				return fail("Runtime metadata fingerprint mismatch for dependency '" + p_dependency.path + "'.");
			}
			return OK;
		}
		return fail("No compiled metadata is available for dependency '" + p_dependency.path + "'.");
	}

	Error validate_dependency(const GDScriptCompiledModule::Dependency &p_dependency) {
		if (p_dependency.engine_api_fingerprint != GDScriptCompiledModule::get_engine_api_fingerprint()) {
			return fail("Engine API fingerprint mismatch for dependency '" + p_dependency.path + "'.");
		}
		Entry *registered = entries.getptr(p_dependency.path);
		if (registered != nullptr) {
			if (!metadata_matches(p_dependency, registered->module)) {
				return fail("Registered metadata fingerprint mismatch for dependency '" + p_dependency.path + "'.");
			}
			return resolve_entry(p_dependency.path);
		}
		const String remapped = ResourceLoader::path_remap(p_dependency.path);
		if (remapped.get_extension().to_lower() == "gdm") {
			if (!FileAccess::exists(remapped)) {
				return fail("Compiled dependency does not exist: '" + p_dependency.path + "'.");
			}
			Error register_error = register_module(p_dependency.path, FileAccess::get_file_as_bytes(remapped), &p_dependency);
			return register_error == OK ? resolve_entry(p_dependency.path) : register_error;
		}
		if (remapped.get_extension().to_lower() == "gd") {
			return validate_source_dependency(p_dependency, remapped);
		}
		return fail("Dependency '" + p_dependency.path + "' has no verifiable source or compiled module.");
	}

	Error resolve_entry(const String &p_path) {
		Entry *entry = entries.getptr(p_path);
		if (entry == nullptr) {
			return fail("Module registry lost metadata for '" + p_path + "'.");
		}
		if (entry->state == STATE_RESOLVING) {
			// A back-edge into the resolving set is a valid dependency cycle. Its
			// local fingerprints were compared before following this edge.
			return OK;
		}
		if (entry->state >= STATE_VERIFIED) {
			return OK;
		}
		if (entry->state != STATE_LOADING_METADATA) {
			return fail("Invalid module registry transition for '" + p_path + "'.");
		}
		entry->state = STATE_RESOLVING;
		const ParsedModule module = entry->module;
		if (module.engine_api_fingerprint != GDScriptCompiledModule::get_engine_api_fingerprint()) {
			return fail("Engine API fingerprint mismatch for module '" + p_path + "'.", p_path == root_path ?
					GDScriptCompiledModule::REJECTION_ENGINE_API : GDScriptCompiledModule::REJECTION_CHANGED_DEPENDENCY);
		}
		String verification_error;
		ModuleVerifier verifier(module);
		if (verifier.verify(&verification_error) != OK) {
			return fail("Module verification failed for '" + p_path + "': " + verification_error,
					p_path == root_path ? verifier.get_rejection_reason() : GDScriptCompiledModule::REJECTION_CHANGED_DEPENDENCY);
		}
		for (const GDScriptCompiledModule::Dependency &dependency : module.dependencies) {
			if (validate_dependency(dependency) != OK) {
				return ERR_INVALID_DATA;
			}
		}
		entry = entries.getptr(p_path);
		entry->state = STATE_VERIFIED;
		return OK;
	}

public:
	Error verify_graph(const ParsedModule &p_root, String *r_error) {
		root_path = p_root.path;
		rejection_reason = GDScriptCompiledModule::REJECTION_NONE;
		Entry root;
		root.state = STATE_LOADING_METADATA;
		root.module = p_root;
		entries.insert(p_root.path, root);
		Error result = resolve_entry(p_root.path);
		if (result == OK) {
			for (KeyValue<String, Entry> &entry : entries) {
				if (entry.value.state == STATE_VERIFIED) {
					entry.value.state = STATE_READY;
				}
			}
		}
		if (result != OK && r_error != nullptr) {
			*r_error = error.is_empty() ? String("Compiled module dependency resolution failed.") : error;
		}
		return result;
	}

	GDScriptCompiledModule::RejectionReason get_rejection_reason() const { return rejection_reason; }
};
