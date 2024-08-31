/**************************************************************************/
/*  object_utils.h                                                              */
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

#ifndef OBJECT_UTILS_H
#define OBJECT_UTILS_H

#include "core/extension/gdextension_interface.h"
#include "core/object/message_queue.h"
#include "core/object/object_id.h"
#include "core/os/memory.h"
#include "core/os/rw_lock.h"
#include "core/os/spin_lock.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/callable_bind.h"
#include "core/variant/variant.h"

template <typename T>
class TypedArray;

enum PropertyHint {
	PROPERTY_HINT_NONE, ///< no hint provided.
	PROPERTY_HINT_RANGE, ///< hint_text = "min,max[,step][,or_greater][,or_less][,hide_slider][,radians_as_degrees][,degrees][,exp][,suffix:<keyword>] range.
	PROPERTY_HINT_ENUM, ///< hint_text= "val1,val2,val3,etc"
	PROPERTY_HINT_ENUM_SUGGESTION, ///< hint_text= "val1,val2,val3,etc"
	PROPERTY_HINT_EXP_EASING, /// exponential easing function (Math::ease) use "attenuation" hint string to revert (flip h), "positive_only" to exclude in-out and out-in. (ie: "attenuation,positive_only")
	PROPERTY_HINT_LINK,
	PROPERTY_HINT_FLAGS, ///< hint_text= "flag1,flag2,etc" (as bit flags)
	PROPERTY_HINT_LAYERS_2D_RENDER,
	PROPERTY_HINT_LAYERS_2D_PHYSICS,
	PROPERTY_HINT_LAYERS_2D_NAVIGATION,
	PROPERTY_HINT_LAYERS_3D_RENDER,
	PROPERTY_HINT_LAYERS_3D_PHYSICS,
	PROPERTY_HINT_LAYERS_3D_NAVIGATION,
	PROPERTY_HINT_FILE, ///< a file path must be passed, hint_text (optionally) is a filter "*.png,*.wav,*.doc,"
	PROPERTY_HINT_DIR, ///< a directory path must be passed
	PROPERTY_HINT_GLOBAL_FILE, ///< a file path must be passed, hint_text (optionally) is a filter "*.png,*.wav,*.doc,"
	PROPERTY_HINT_GLOBAL_DIR, ///< a directory path must be passed
	PROPERTY_HINT_RESOURCE_TYPE, ///< a resource object type
	PROPERTY_HINT_MULTILINE_TEXT, ///< used for string properties that can contain multiple lines
	PROPERTY_HINT_EXPRESSION, ///< used for string properties that can contain multiple lines
	PROPERTY_HINT_PLACEHOLDER_TEXT, ///< used to set a placeholder text for string properties
	PROPERTY_HINT_COLOR_NO_ALPHA, ///< used for ignoring alpha component when editing a color
	PROPERTY_HINT_OBJECT_ID,
	PROPERTY_HINT_TYPE_STRING, ///< a type string, the hint is the base type to choose
	PROPERTY_HINT_NODE_PATH_TO_EDITED_NODE, // Deprecated.
	PROPERTY_HINT_OBJECT_TOO_BIG, ///< object is too big to send
	PROPERTY_HINT_NODE_PATH_VALID_TYPES,
	PROPERTY_HINT_SAVE_FILE, ///< a file path must be passed, hint_text (optionally) is a filter "*.png,*.wav,*.doc,". This opens a save dialog
	PROPERTY_HINT_GLOBAL_SAVE_FILE, ///< a file path must be passed, hint_text (optionally) is a filter "*.png,*.wav,*.doc,". This opens a save dialog
	PROPERTY_HINT_INT_IS_OBJECTID, // Deprecated.
	PROPERTY_HINT_INT_IS_POINTER,
	PROPERTY_HINT_ARRAY_TYPE,
	PROPERTY_HINT_LOCALE_ID,
	PROPERTY_HINT_LOCALIZABLE_STRING,
	PROPERTY_HINT_NODE_TYPE, ///< a node object type
	PROPERTY_HINT_HIDE_QUATERNION_EDIT, /// Only Node3D::transform should hide the quaternion editor.
	PROPERTY_HINT_PASSWORD,
	PROPERTY_HINT_LAYERS_AVOIDANCE,
	PROPERTY_HINT_MAX,
};

enum PropertyUsageFlags {
	PROPERTY_USAGE_NONE = 0,
	PROPERTY_USAGE_STORAGE = 1 << 1,
	PROPERTY_USAGE_EDITOR = 1 << 2,
	PROPERTY_USAGE_INTERNAL = 1 << 3,
	PROPERTY_USAGE_CHECKABLE = 1 << 4, // Used for editing global variables.
	PROPERTY_USAGE_CHECKED = 1 << 5, // Used for editing global variables.
	PROPERTY_USAGE_GROUP = 1 << 6, // Used for grouping props in the editor.
	PROPERTY_USAGE_CATEGORY = 1 << 7,
	PROPERTY_USAGE_SUBGROUP = 1 << 8,
	PROPERTY_USAGE_CLASS_IS_BITFIELD = 1 << 9,
	PROPERTY_USAGE_NO_INSTANCE_STATE = 1 << 10,
	PROPERTY_USAGE_RESTART_IF_CHANGED = 1 << 11,
	PROPERTY_USAGE_SCRIPT_VARIABLE = 1 << 12,
	PROPERTY_USAGE_STORE_IF_NULL = 1 << 13,
	PROPERTY_USAGE_UPDATE_ALL_IF_MODIFIED = 1 << 14,
	PROPERTY_USAGE_SCRIPT_DEFAULT_VALUE = 1 << 15, // Deprecated.
	PROPERTY_USAGE_CLASS_IS_ENUM = 1 << 16,
	PROPERTY_USAGE_NIL_IS_VARIANT = 1 << 17,
	PROPERTY_USAGE_ARRAY = 1 << 18, // Used in the inspector to group properties as elements of an array.
	PROPERTY_USAGE_ALWAYS_DUPLICATE = 1 << 19, // When duplicating a resource, always duplicate, even with subresource duplication disabled.
	PROPERTY_USAGE_NEVER_DUPLICATE = 1 << 20, // When duplicating a resource, never duplicate, even with subresource duplication enabled.
	PROPERTY_USAGE_HIGH_END_GFX = 1 << 21,
	PROPERTY_USAGE_NODE_PATH_FROM_SCENE_ROOT = 1 << 22,
	PROPERTY_USAGE_RESOURCE_NOT_PERSISTENT = 1 << 23,
	PROPERTY_USAGE_KEYING_INCREMENTS = 1 << 24, // Used in inspector to increment property when keyed in animation player.
	PROPERTY_USAGE_DEFERRED_SET_RESOURCE = 1 << 25, // Deprecated.
	PROPERTY_USAGE_EDITOR_INSTANTIATE_OBJECT = 1 << 26, // For Object properties, instantiate them when creating in editor.
	PROPERTY_USAGE_EDITOR_BASIC_SETTING = 1 << 27, //for project or editor settings, show when basic settings are selected.
	PROPERTY_USAGE_READ_ONLY = 1 << 28, // Mark a property as read-only in the inspector.
	PROPERTY_USAGE_SECRET = 1 << 29, // Export preset credentials that should be stored separately from the rest of the export config.

	PROPERTY_USAGE_DEFAULT = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR,
	PROPERTY_USAGE_NO_EDITOR = PROPERTY_USAGE_STORAGE,
};

#define ADD_SIGNAL(m_signal) ::ClassDB::add_signal(get_class_static(), m_signal)
#define ADD_PROPERTY(m_property, m_setter, m_getter) ::ClassDB::add_property(get_class_static(), m_property, _scs_create(m_setter), _scs_create(m_getter))
#define ADD_PROPERTYI(m_property, m_setter, m_getter, m_index) ::ClassDB::add_property(get_class_static(), m_property, _scs_create(m_setter), _scs_create(m_getter), m_index)
#define ADD_PROPERTY_DEFAULT(m_property, m_default) ::ClassDB::set_property_default_value(get_class_static(), m_property, m_default)
#define ADD_GROUP(m_name, m_prefix) ::ClassDB::add_property_group(get_class_static(), m_name, m_prefix)
#define ADD_GROUP_INDENT(m_name, m_prefix, m_depth) ::ClassDB::add_property_group(get_class_static(), m_name, m_prefix, m_depth)
#define ADD_SUBGROUP(m_name, m_prefix) ::ClassDB::add_property_subgroup(get_class_static(), m_name, m_prefix)
#define ADD_SUBGROUP_INDENT(m_name, m_prefix, m_depth) ::ClassDB::add_property_subgroup(get_class_static(), m_name, m_prefix, m_depth)
#define ADD_LINKED_PROPERTY(m_property, m_linked_property) ::ClassDB::add_linked_property(get_class_static(), m_property, m_linked_property)

#define ADD_ARRAY_COUNT(m_label, m_count_property, m_count_property_setter, m_count_property_getter, m_prefix) ClassDB::add_property_array_count(get_class_static(), m_label, m_count_property, _scs_create(m_count_property_setter), _scs_create(m_count_property_getter), m_prefix)
#define ADD_ARRAY_COUNT_WITH_USAGE_FLAGS(m_label, m_count_property, m_count_property_setter, m_count_property_getter, m_prefix, m_property_usage_flags) ClassDB::add_property_array_count(get_class_static(), m_label, m_count_property, _scs_create(m_count_property_setter), _scs_create(m_count_property_getter), m_prefix, m_property_usage_flags)
#define ADD_ARRAY(m_array_path, m_prefix) ClassDB::add_property_array(get_class_static(), m_array_path, m_prefix)

// Helper macro to use with PROPERTY_HINT_ARRAY_TYPE for arrays of specific resources:
// PropertyInfo(Variant::ARRAY, "fallbacks", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("Font")
#define MAKE_RESOURCE_TYPE_HINT(m_type) vformat("%s/%s:%s", Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, m_type)

struct PropertyInfo {
	Variant::Type type = Variant::NIL;
	String name;
	StringName class_name; // For classes
	PropertyHint hint = PROPERTY_HINT_NONE;
	String hint_string;
	uint32_t usage = PROPERTY_USAGE_DEFAULT;

	// If you are thinking about adding another member to this class, ask the maintainer (Juan) first.

	_FORCE_INLINE_ PropertyInfo added_usage(uint32_t p_fl) const {
		PropertyInfo pi = *this;
		pi.usage |= p_fl;
		return pi;
	}

	operator Dictionary() const;

	static PropertyInfo from_dict(const Dictionary &p_dict);

	PropertyInfo() {}

	PropertyInfo(const Variant::Type p_type, const String &p_name, const PropertyHint p_hint = PROPERTY_HINT_NONE, const String &p_hint_string = "", const uint32_t p_usage = PROPERTY_USAGE_DEFAULT, const StringName &p_class_name = StringName()) :
			type(p_type),
			name(p_name),
			hint(p_hint),
			hint_string(p_hint_string),
			usage(p_usage) {
		if (hint == PROPERTY_HINT_RESOURCE_TYPE) {
			class_name = hint_string;
		} else {
			class_name = p_class_name;
		}
	}

	PropertyInfo(const StringName &p_class_name) :
			type(Variant::OBJECT),
			class_name(p_class_name) {}

	explicit PropertyInfo(const GDExtensionPropertyInfo &pinfo) :
			type((Variant::Type)pinfo.type),
			name(*reinterpret_cast<StringName *>(pinfo.name)),
			class_name(*reinterpret_cast<StringName *>(pinfo.class_name)),
			hint((PropertyHint)pinfo.hint),
			hint_string(*reinterpret_cast<String *>(pinfo.hint_string)),
			usage(pinfo.usage) {}

	bool operator==(const PropertyInfo &p_info) const {
		return ((type == p_info.type) &&
				(name == p_info.name) &&
				(class_name == p_info.class_name) &&
				(hint == p_info.hint) &&
				(hint_string == p_info.hint_string) &&
				(usage == p_info.usage));
	}

	bool operator<(const PropertyInfo &p_info) const {
		return name < p_info.name;
	}
};

TypedArray<Dictionary> convert_property_list(const List<PropertyInfo> *p_list);

enum MethodFlags {
	METHOD_FLAG_NORMAL = 1,
	METHOD_FLAG_EDITOR = 2,
	METHOD_FLAG_CONST = 4,
	METHOD_FLAG_VIRTUAL = 8,
	METHOD_FLAG_VARARG = 16,
	METHOD_FLAG_STATIC = 32,
	METHOD_FLAG_OBJECT_CORE = 64,
	METHOD_FLAGS_DEFAULT = METHOD_FLAG_NORMAL,
};

struct MethodInfo {
	String name;
	PropertyInfo return_val;
	uint32_t flags = METHOD_FLAGS_DEFAULT;
	int id = 0;
	List<PropertyInfo> arguments;
	Vector<Variant> default_arguments;
	int return_val_metadata = 0;
	Vector<int> arguments_metadata;

	int get_argument_meta(int p_arg) const {
		ERR_FAIL_COND_V(p_arg < -1 || p_arg > arguments.size(), 0);
		if (p_arg == -1) {
			return return_val_metadata;
		}
		return arguments_metadata.size() > p_arg ? arguments_metadata[p_arg] : 0;
	}

	inline bool operator==(const MethodInfo &p_method) const { return id == p_method.id && name == p_method.name; }
	inline bool operator<(const MethodInfo &p_method) const { return id == p_method.id ? (name < p_method.name) : (id < p_method.id); }

	operator Dictionary() const;

	static MethodInfo from_dict(const Dictionary &p_dict);

	MethodInfo() {}

	explicit MethodInfo(const GDExtensionMethodInfo &pinfo) :
			name(*reinterpret_cast<StringName *>(pinfo.name)),
			return_val(PropertyInfo(pinfo.return_value)),
			flags(pinfo.flags),
			id(pinfo.id) {
		for (uint32_t j = 0; j < pinfo.argument_count; j++) {
			arguments.push_back(PropertyInfo(pinfo.arguments[j]));
		}
		const Variant *def_values = (const Variant *)pinfo.default_arguments;
		for (uint32_t j = 0; j < pinfo.default_argument_count; j++) {
			default_arguments.push_back(def_values[j]);
		}
	}

	void _push_params(const PropertyInfo &p_param) {
		arguments.push_back(p_param);
	}

	template <typename... VarArgs>
	void _push_params(const PropertyInfo &p_param, VarArgs... p_params) {
		arguments.push_back(p_param);
		_push_params(p_params...);
	}

	MethodInfo(const String &p_name) { name = p_name; }

	template <typename... VarArgs>
	MethodInfo(const String &p_name, VarArgs... p_params) {
		name = p_name;
		_push_params(p_params...);
	}

	MethodInfo(Variant::Type ret) { return_val.type = ret; }
	MethodInfo(Variant::Type ret, const String &p_name) {
		return_val.type = ret;
		name = p_name;
	}

	template <typename... VarArgs>
	MethodInfo(Variant::Type ret, const String &p_name, VarArgs... p_params) {
		name = p_name;
		return_val.type = ret;
		_push_params(p_params...);
	}

	MethodInfo(const PropertyInfo &p_ret, const String &p_name) {
		return_val = p_ret;
		name = p_name;
	}

	template <typename... VarArgs>
	MethodInfo(const PropertyInfo &p_ret, const String &p_name, VarArgs... p_params) {
		return_val = p_ret;
		name = p_name;
		_push_params(p_params...);
	}
};

// API used to extend in GDExtension and other C compatible compiled languages.
class MethodBind;
class GDExtension;

struct ObjectGDExtension {
	GDExtension *library = nullptr;
	ObjectGDExtension *parent = nullptr;
	List<ObjectGDExtension *> children;
	StringName parent_class_name;
	StringName class_name;
	bool editor_class = false;
	bool reloadable = false;
	bool is_virtual = false;
	bool is_abstract = false;
	bool is_exposed = true;
#ifdef TOOLS_ENABLED
	bool is_runtime = false;
	bool is_placeholder = false;
#endif
	GDExtensionClassSet set;
	GDExtensionClassGet get;
	GDExtensionClassGetPropertyList get_property_list;
	GDExtensionClassFreePropertyList2 free_property_list2;
	GDExtensionClassPropertyCanRevert property_can_revert;
	GDExtensionClassPropertyGetRevert property_get_revert;
	GDExtensionClassValidateProperty validate_property;
#ifndef DISABLE_DEPRECATED
	GDExtensionClassNotification notification;
	GDExtensionClassFreePropertyList free_property_list;
#endif // DISABLE_DEPRECATED
	GDExtensionClassNotification2 notification2;
	GDExtensionClassToString to_string;
	GDExtensionClassReference reference;
	GDExtensionClassReference unreference;
	GDExtensionClassGetRID get_rid;

	_FORCE_INLINE_ bool is_class(const String &p_class) const {
		const ObjectGDExtension *e = this;
		while (e) {
			if (p_class == e->class_name.operator String()) {
				return true;
			}
			e = e->parent;
		}
		return false;
	}
	void *class_userdata = nullptr;

#ifndef DISABLE_DEPRECATED
	GDExtensionClassCreateInstance create_instance;
#endif // DISABLE_DEPRECATED
	GDExtensionClassCreateInstance2 create_instance2;
	GDExtensionClassFreeInstance free_instance;
	GDExtensionClassGetVirtual get_virtual;
	GDExtensionClassGetVirtualCallData get_virtual_call_data;
	GDExtensionClassCallVirtualWithData call_virtual_with_data;
	GDExtensionClassRecreateInstance recreate_instance;

#ifdef TOOLS_ENABLED
	void *tracking_userdata = nullptr;
	void (*track_instance)(void *p_userdata, void *p_instance) = nullptr;
	void (*untrack_instance)(void *p_userdata, void *p_instance) = nullptr;
#endif
};

#define GDVIRTUAL_CALL(m_name, ...) _gdvirtual_##m_name##_call<false>(__VA_ARGS__)
#define GDVIRTUAL_CALL_PTR(m_obj, m_name, ...) m_obj->_gdvirtual_##m_name##_call<false>(__VA_ARGS__)

#define GDVIRTUAL_REQUIRED_CALL(m_name, ...) _gdvirtual_##m_name##_call<true>(__VA_ARGS__)
#define GDVIRTUAL_REQUIRED_CALL_PTR(m_obj, m_name, ...) m_obj->_gdvirtual_##m_name##_call<true>(__VA_ARGS__)

#ifdef DEBUG_METHODS_ENABLED
#define GDVIRTUAL_BIND(m_name, ...) ::ClassDB::add_virtual_method(get_class_static(), _gdvirtual_##m_name##_get_method_info(), true, sarray(__VA_ARGS__));
#else
#define GDVIRTUAL_BIND(m_name, ...)
#endif
#define GDVIRTUAL_IS_OVERRIDDEN(m_name) _gdvirtual_##m_name##_overridden()
#define GDVIRTUAL_IS_OVERRIDDEN_PTR(m_obj, m_name) m_obj->_gdvirtual_##m_name##_overridden()

/*
 * The following is an incomprehensible blob of hacks and workarounds to
 * compensate for many of the fallacies in C++. As a plus, this macro pretty
 * much alone defines the object model.
 */

#define GDCLASS(m_class, m_inherits)                                                                                                             \
private:                                                                                                                                         \
	void operator=(const m_class &p_rval) {}                                                                                                     \
	friend class ::ClassDB;                                                                                                                      \
                                                                                                                                                 \
public:                                                                                                                                          \
	typedef m_class self_type;                                                                                                                   \
	static constexpr bool _class_is_enabled = !bool(GD_IS_DEFINED(ClassDB_Disable_##m_class)) && m_inherits::_class_is_enabled;                  \
	virtual String get_class() const override {                                                                                                  \
		if (_get_extension()) {                                                                                                                  \
			return _get_extension()->class_name.operator String();                                                                               \
		}                                                                                                                                        \
		return String(#m_class);                                                                                                                 \
	}                                                                                                                                            \
	virtual const StringName *_get_class_namev() const override {                                                                                \
		static StringName _class_name_static;                                                                                                    \
		if (unlikely(!_class_name_static)) {                                                                                                     \
			StringName::assign_static_unique_class_name(&_class_name_static, #m_class);                                                          \
		}                                                                                                                                        \
		return &_class_name_static;                                                                                                              \
	}                                                                                                                                            \
	static _FORCE_INLINE_ void *get_class_ptr_static() {                                                                                         \
		static int ptr;                                                                                                                          \
		return &ptr;                                                                                                                             \
	}                                                                                                                                            \
	static _FORCE_INLINE_ String get_class_static() {                                                                                            \
		return String(#m_class);                                                                                                                 \
	}                                                                                                                                            \
	static _FORCE_INLINE_ String get_parent_class_static() {                                                                                     \
		return m_inherits::get_class_static();                                                                                                   \
	}                                                                                                                                            \
	static void get_inheritance_list_static(List<String> *p_inheritance_list) {                                                                  \
		m_inherits::get_inheritance_list_static(p_inheritance_list);                                                                             \
		p_inheritance_list->push_back(String(#m_class));                                                                                         \
	}                                                                                                                                            \
	virtual bool is_class(const String &p_class) const override {                                                                                \
		if (_get_extension() && _get_extension()->is_class(p_class)) {                                                                           \
			return true;                                                                                                                         \
		}                                                                                                                                        \
		return (p_class == (#m_class)) ? true : m_inherits::is_class(p_class);                                                                   \
	}                                                                                                                                            \
	virtual bool is_class_ptr(void *p_ptr) const override { return (p_ptr == get_class_ptr_static()) ? true : m_inherits::is_class_ptr(p_ptr); } \
                                                                                                                                                 \
	static void get_valid_parents_static(List<String> *p_parents) {                                                                              \
		if (m_class::_get_valid_parents_static != m_inherits::_get_valid_parents_static) {                                                       \
			m_class::_get_valid_parents_static(p_parents);                                                                                       \
		}                                                                                                                                        \
                                                                                                                                                 \
		m_inherits::get_valid_parents_static(p_parents);                                                                                         \
	}                                                                                                                                            \
                                                                                                                                                 \
protected:                                                                                                                                       \
	_FORCE_INLINE_ static void (*_get_bind_methods())() {                                                                                        \
		return &m_class::_bind_methods;                                                                                                          \
	}                                                                                                                                            \
	_FORCE_INLINE_ static void (*_get_bind_compatibility_methods())() {                                                                          \
		return &m_class::_bind_compatibility_methods;                                                                                            \
	}                                                                                                                                            \
                                                                                                                                                 \
public:                                                                                                                                          \
	static void initialize_class() {                                                                                                             \
		static bool initialized = false;                                                                                                         \
		if (initialized) {                                                                                                                       \
			return;                                                                                                                              \
		}                                                                                                                                        \
		m_inherits::initialize_class();                                                                                                          \
		::ClassDB::_add_class<m_class>();                                                                                                        \
		if (m_class::_get_bind_methods() != m_inherits::_get_bind_methods()) {                                                                   \
			_bind_methods();                                                                                                                     \
		}                                                                                                                                        \
		if (m_class::_get_bind_compatibility_methods() != m_inherits::_get_bind_compatibility_methods()) {                                       \
			_bind_compatibility_methods();                                                                                                       \
		}                                                                                                                                        \
		initialized = true;                                                                                                                      \
	}                                                                                                                                            \
                                                                                                                                                 \
protected:                                                                                                                                       \
	virtual void _initialize_classv() override {                                                                                                 \
		initialize_class();                                                                                                                      \
	}                                                                                                                                            \
	_FORCE_INLINE_ bool (Object::*_get_get() const)(const StringName &p_name, Variant &) const {                                                 \
		return (bool(Object::*)(const StringName &, Variant &) const) & m_class::_get;                                                           \
	}                                                                                                                                            \
	virtual bool _getv(const StringName &p_name, Variant &r_ret) const override {                                                                \
		if (m_class::_get_get() != m_inherits::_get_get()) {                                                                                     \
			if (_get(p_name, r_ret)) {                                                                                                           \
				return true;                                                                                                                     \
			}                                                                                                                                    \
		}                                                                                                                                        \
		return m_inherits::_getv(p_name, r_ret);                                                                                                 \
	}                                                                                                                                            \
	_FORCE_INLINE_ bool (Object::*_get_set() const)(const StringName &p_name, const Variant &p_property) {                                       \
		return (bool(Object::*)(const StringName &, const Variant &)) & m_class::_set;                                                           \
	}                                                                                                                                            \
	virtual bool _setv(const StringName &p_name, const Variant &p_property) override {                                                           \
		if (m_inherits::_setv(p_name, p_property)) {                                                                                             \
			return true;                                                                                                                         \
		}                                                                                                                                        \
		if (m_class::_get_set() != m_inherits::_get_set()) {                                                                                     \
			return _set(p_name, p_property);                                                                                                     \
		}                                                                                                                                        \
		return false;                                                                                                                            \
	}                                                                                                                                            \
	_FORCE_INLINE_ void (Object::*_get_get_property_list() const)(List<PropertyInfo> * p_list) const {                                           \
		return (void(Object::*)(List<PropertyInfo> *) const) & m_class::_get_property_list;                                                      \
	}                                                                                                                                            \
	virtual void _get_property_listv(List<PropertyInfo> *p_list, bool p_reversed) const override {                                               \
		if (!p_reversed) {                                                                                                                       \
			m_inherits::_get_property_listv(p_list, p_reversed);                                                                                 \
		}                                                                                                                                        \
		p_list->push_back(PropertyInfo(Variant::NIL, get_class_static(), PROPERTY_HINT_NONE, get_class_static(), PROPERTY_USAGE_CATEGORY));      \
		::ClassDB::get_property_list(#m_class, p_list, true, this);                                                                              \
		if (m_class::_get_get_property_list() != m_inherits::_get_get_property_list()) {                                                         \
			_get_property_list(p_list);                                                                                                          \
		}                                                                                                                                        \
		if (p_reversed) {                                                                                                                        \
			m_inherits::_get_property_listv(p_list, p_reversed);                                                                                 \
		}                                                                                                                                        \
	}                                                                                                                                            \
	_FORCE_INLINE_ void (Object::*_get_validate_property() const)(PropertyInfo & p_property) const {                                             \
		return (void(Object::*)(PropertyInfo &) const) & m_class::_validate_property;                                                            \
	}                                                                                                                                            \
	virtual void _validate_propertyv(PropertyInfo &p_property) const override {                                                                  \
		m_inherits::_validate_propertyv(p_property);                                                                                             \
		if (m_class::_get_validate_property() != m_inherits::_get_validate_property()) {                                                         \
			_validate_property(p_property);                                                                                                      \
		}                                                                                                                                        \
	}                                                                                                                                            \
	_FORCE_INLINE_ bool (Object::*_get_property_can_revert() const)(const StringName &p_name) const {                                            \
		return (bool(Object::*)(const StringName &) const) & m_class::_property_can_revert;                                                      \
	}                                                                                                                                            \
	virtual bool _property_can_revertv(const StringName &p_name) const override {                                                                \
		if (m_class::_get_property_can_revert() != m_inherits::_get_property_can_revert()) {                                                     \
			if (_property_can_revert(p_name)) {                                                                                                  \
				return true;                                                                                                                     \
			}                                                                                                                                    \
		}                                                                                                                                        \
		return m_inherits::_property_can_revertv(p_name);                                                                                        \
	}                                                                                                                                            \
	_FORCE_INLINE_ bool (Object::*_get_property_get_revert() const)(const StringName &p_name, Variant &) const {                                 \
		return (bool(Object::*)(const StringName &, Variant &) const) & m_class::_property_get_revert;                                           \
	}                                                                                                                                            \
	virtual bool _property_get_revertv(const StringName &p_name, Variant &r_ret) const override {                                                \
		if (m_class::_get_property_get_revert() != m_inherits::_get_property_get_revert()) {                                                     \
			if (_property_get_revert(p_name, r_ret)) {                                                                                           \
				return true;                                                                                                                     \
			}                                                                                                                                    \
		}                                                                                                                                        \
		return m_inherits::_property_get_revertv(p_name, r_ret);                                                                                 \
	}                                                                                                                                            \
	_FORCE_INLINE_ void (Object::*_get_notification() const)(int) {                                                                              \
		return (void(Object::*)(int)) & m_class::_notification;                                                                                  \
	}                                                                                                                                            \
	virtual void _notificationv(int p_notification, bool p_reversed) override {                                                                  \
		if (!p_reversed) {                                                                                                                       \
			m_inherits::_notificationv(p_notification, p_reversed);                                                                              \
		}                                                                                                                                        \
		if (m_class::_get_notification() != m_inherits::_get_notification()) {                                                                   \
			_notification(p_notification);                                                                                                       \
		}                                                                                                                                        \
		if (p_reversed) {                                                                                                                        \
			m_inherits::_notificationv(p_notification, p_reversed);                                                                              \
		}                                                                                                                                        \
	}                                                                                                                                            \
                                                                                                                                                 \
private:

#define OBJ_SAVE_TYPE(m_class)                                          \
public:                                                                 \
	virtual String get_save_class() const override { return #m_class; } \
                                                                        \
private:

#endif // OBJECT_UTILS_H
