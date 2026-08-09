/**************************************************************************/
/*  gdscript_optimization_profile.cpp                                     */
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

#include "gdscript_optimization_profile.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"

namespace {

constexpr const char *PROFILE_HEADER = "gdscript-optimization-profile-v1";
Mutex profile_mutex;
HashMap<String, uint32_t> profile_hints;
bool profile_loaded = false;

Error reject_profile(Error p_error) {
	MutexLock lock(profile_mutex);
	profile_hints.clear();
	profile_loaded = true;
	return p_error;
}

} // namespace

String GDScriptOptimizationProfile::get_default_path() {
	return "res://.godot/gdscript_optimization_profile";
}

bool GDScriptOptimizationProfile::has_hint(const String &p_key, uint32_t p_fingerprint) {
	if (p_key.is_empty()) {
		return false;
	}

	bool needs_load = false;
	{
		MutexLock lock(profile_mutex);
		needs_load = !profile_loaded;
	}
	if (needs_load) {
		load();
	}

	MutexLock lock(profile_mutex);
	const uint32_t *fingerprint = profile_hints.getptr(p_key);
	return fingerprint != nullptr && *fingerprint == p_fingerprint;
}

Error GDScriptOptimizationProfile::load(const String &p_path) {
	const String path = p_path.is_empty() ? get_default_path() : p_path;
	HashMap<String, uint32_t> loaded_hints;

	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ, &error);
	if (file.is_null()) {
		MutexLock lock(profile_mutex);
		profile_hints.clear();
		profile_loaded = true;
		return error;
	}

	if (file->get_line().strip_edges() != PROFILE_HEADER) {
		return reject_profile(ERR_FILE_CORRUPT);
	}
	while (!file->eof_reached()) {
		const String line = file->get_line().strip_edges();
		if (line.is_empty()) {
			continue;
		}
		const PackedStringArray fields = line.split("\t", false);
		if (fields.size() != 3 || !fields[1].is_valid_int() || !fields[2].is_valid_int()) {
			return reject_profile(ERR_FILE_CORRUPT);
		}
		const int64_t fingerprint = fields[1].to_int();
		if (fingerprint < 0 || fingerprint > UINT32_MAX || fields[0].is_empty()) {
			return reject_profile(ERR_FILE_CORRUPT);
		}
		loaded_hints.insert(fields[0], uint32_t(fingerprint));
	}

	MutexLock lock(profile_mutex);
	profile_hints = loaded_hints;
	profile_loaded = true;
	return OK;
}

Error GDScriptOptimizationProfile::save(const Vector<Entry> &p_entries, const String &p_path) {
	const String path = p_path.is_empty() ? get_default_path() : p_path;
	const Error directory_error = DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	ERR_FAIL_COND_V(directory_error != OK, directory_error);
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE, &error);
	ERR_FAIL_COND_V(file.is_null(), error);

	Vector<Entry> entries = p_entries;
	entries.sort();
	file->store_line(PROFILE_HEADER);
	for (const Entry &entry : entries) {
		if (entry.key.is_empty() || entry.key.contains("\t") || entry.key.contains("\n")) {
			continue;
		}
		file->store_line(entry.key + "\t" + String::num_uint64(entry.fingerprint) + "\t" + String::num_uint64(entry.call_count));
	}
	file->close();
	return load(path);
}

void GDScriptOptimizationProfile::clear() {
	MutexLock lock(profile_mutex);
	profile_hints.clear();
	profile_loaded = true;
}
