/**************************************************************************/
/*  object.h                                                              */
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

#ifndef OBJECT_H
#define OBJECT_H

#include "core/extension/gdextension_interface.h"
#include "core/object/message_queue.h"
#include "core/object/object_id.h"
#include "core/object/object_utils.h"
#include "core/os/rw_lock.h"
#include "core/os/spin_lock.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/list.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/callable_bind.h"
#include "core/variant/variant.h"

template <typename T>
class TypedArray;

// API used to extend in GDExtension and other C compatible compiled languages.
class MethodBind;
class GDExtension;

class ScriptInstance;

class Object {
public:
	typedef Object self_type;

	enum ConnectFlags {
		CONNECT_DEFERRED = 1,
		CONNECT_PERSIST = 2, // hint for scene to save this connection
		CONNECT_ONE_SHOT = 4,
		CONNECT_REFERENCE_COUNTED = 8,
		CONNECT_INHERITED = 16, // Used in editor builds.
	};

	struct Connection {
		::Signal signal;
		Callable callable;

		uint32_t flags = 0;
		bool operator<(const Connection &p_conn) const;

		operator Variant() const;

		Connection() {}
		Connection(const Variant &p_variant);
	};

private:
#ifdef DEBUG_ENABLED
	friend struct _ObjectDebugLock;
#endif
	friend bool predelete_handler(Object *);
	friend void postinitialize_handler(Object *);

	ObjectGDExtension *_extension = nullptr;
	GDExtensionClassInstancePtr _extension_instance = nullptr;

	struct SignalData {
		struct Slot {
			int reference_count = 0;
			Connection conn;
			List<Connection>::Element *cE = nullptr;
		};

		MethodInfo user;
		HashMap<Callable, Slot, HashableHasher<Callable>> slot_map;
		bool removable = false;
	};

	HashMap<StringName, SignalData> signal_map;
	List<Connection> connections;
#ifdef DEBUG_ENABLED
	SafeRefCount _lock_index;
#endif
	bool _block_signals = false;
	int _predelete_ok = 0;
	ObjectID _instance_id;
	bool _predelete();
	void _initialize();
	void _postinitialize();
	bool _can_translate = true;
	bool _emitting = false;
#ifdef TOOLS_ENABLED
	bool _edited = false;
	uint32_t _edited_version = 0;
	HashSet<String> editor_section_folding;
#endif
	ScriptInstance *script_instance = nullptr;
	Variant script; // Reference does not exist yet, store it in a Variant.
	HashMap<StringName, Variant> metadata;
	HashMap<StringName, Variant *> metadata_properties;
	mutable const StringName *_class_name_ptr = nullptr;

	void _add_user_signal(const String &p_name, const Array &p_args = Array());
	bool _has_user_signal(const StringName &p_name) const;
	void _remove_user_signal(const StringName &p_name);
	Error _emit_signal(const Variant **p_args, int p_argcount, Callable::CallError &r_error);
	TypedArray<Dictionary> _get_signal_list() const;
	TypedArray<Dictionary> _get_signal_connection_list(const StringName &p_signal) const;
	TypedArray<Dictionary> _get_incoming_connections() const;
	void _set_bind(const StringName &p_set, const Variant &p_value);
	Variant _get_bind(const StringName &p_name) const;
	void _set_indexed_bind(const NodePath &p_name, const Variant &p_value);
	Variant _get_indexed_bind(const NodePath &p_name) const;
	int _get_method_argument_count_bind(const StringName &p_name) const;

	_FORCE_INLINE_ void _construct_object(bool p_reference);

	friend class RefCounted;
	bool type_is_reference = false;

	BinaryMutex _instance_binding_mutex;
	struct InstanceBinding {
		void *binding = nullptr;
		void *token = nullptr;
		GDExtensionInstanceBindingFreeCallback free_callback = nullptr;
		GDExtensionInstanceBindingReferenceCallback reference_callback = nullptr;
	};
	InstanceBinding *_instance_bindings = nullptr;
	uint32_t _instance_binding_count = 0;

	Object(bool p_reference);

protected:
	_FORCE_INLINE_ bool _instance_binding_reference(bool p_reference) {
		bool can_die = true;
		if (_instance_bindings) {
			MutexLock instance_binding_lock(_instance_binding_mutex);
			for (uint32_t i = 0; i < _instance_binding_count; i++) {
				if (_instance_bindings[i].reference_callback) {
					if (!_instance_bindings[i].reference_callback(_instance_bindings[i].token, _instance_bindings[i].binding, p_reference)) {
						can_die = false;
					}
				}
			}
		}
		return can_die;
	}

	friend class GDExtensionMethodBind;
	_ALWAYS_INLINE_ const ObjectGDExtension *_get_extension() const { return _extension; }
	_ALWAYS_INLINE_ GDExtensionClassInstancePtr _get_extension_instance() const { return _extension_instance; }
	virtual void _initialize_classv() { initialize_class(); }
	virtual bool _setv(const StringName &p_name, const Variant &p_property) { return false; };
	virtual bool _getv(const StringName &p_name, Variant &r_property) const { return false; };
	virtual void _get_property_listv(List<PropertyInfo> *p_list, bool p_reversed) const {};
	virtual void _validate_propertyv(PropertyInfo &p_property) const {};
	virtual bool _property_can_revertv(const StringName &p_name) const { return false; };
	virtual bool _property_get_revertv(const StringName &p_name, Variant &r_property) const { return false; };
	virtual void _notificationv(int p_notification, bool p_reversed) {}

	static void _bind_methods();
#ifndef DISABLE_DEPRECATED
	static void _bind_compatibility_methods();
#else
	static void _bind_compatibility_methods() {}
#endif
	bool _set(const StringName &p_name, const Variant &p_property) { return false; };
	bool _get(const StringName &p_name, Variant &r_property) const { return false; };
	void _get_property_list(List<PropertyInfo> *p_list) const {};
	void _validate_property(PropertyInfo &p_property) const {};
	bool _property_can_revert(const StringName &p_name) const { return false; };
	bool _property_get_revert(const StringName &p_name, Variant &r_property) const { return false; };
	void _notification(int p_notification) {}

	_FORCE_INLINE_ static void (*_get_bind_methods())() {
		return &Object::_bind_methods;
	}
	_FORCE_INLINE_ static void (*_get_bind_compatibility_methods())() {
		return &Object::_bind_compatibility_methods;
	}
	_FORCE_INLINE_ bool (Object::*_get_get() const)(const StringName &p_name, Variant &r_ret) const {
		return &Object::_get;
	}
	_FORCE_INLINE_ bool (Object::*_get_set() const)(const StringName &p_name, const Variant &p_property) {
		return &Object::_set;
	}
	_FORCE_INLINE_ void (Object::*_get_get_property_list() const)(List<PropertyInfo> *p_list) const {
		return &Object::_get_property_list;
	}
	_FORCE_INLINE_ void (Object::*_get_validate_property() const)(PropertyInfo &p_property) const {
		return &Object::_validate_property;
	}
	_FORCE_INLINE_ bool (Object::*_get_property_can_revert() const)(const StringName &p_name) const {
		return &Object::_property_can_revert;
	}
	_FORCE_INLINE_ bool (Object::*_get_property_get_revert() const)(const StringName &p_name, Variant &) const {
		return &Object::_property_get_revert;
	}
	_FORCE_INLINE_ void (Object::*_get_notification() const)(int) {
		return &Object::_notification;
	}
	static void get_valid_parents_static(List<String> *p_parents);
	static void _get_valid_parents_static(List<String> *p_parents);

	Variant _call_bind(const Variant **p_args, int p_argcount, Callable::CallError &r_error);
	Variant _call_deferred_bind(const Variant **p_args, int p_argcount, Callable::CallError &r_error);

	virtual const StringName *_get_class_namev() const {
		static StringName _class_name_static;
		if (unlikely(!_class_name_static)) {
			StringName::assign_static_unique_class_name(&_class_name_static, "Object");
		}
		return &_class_name_static;
	}

	TypedArray<StringName> _get_meta_list_bind() const;
	TypedArray<Dictionary> _get_property_list_bind() const;
	TypedArray<Dictionary> _get_method_list_bind() const;

	void _clear_internal_resource_paths(const Variant &p_var);

	friend class ClassDB;
	friend class PlaceholderExtensionInstance;

	bool _disconnect(const StringName &p_signal, const Callable &p_callable, bool p_force = false);

#ifdef TOOLS_ENABLED
	struct VirtualMethodTracker {
		void **method;
		bool *initialized;
		VirtualMethodTracker *next;
	};

	mutable VirtualMethodTracker *virtual_method_list = nullptr;
#endif

public: // Should be protected, but bug in clang++.
	static void initialize_class();
	_FORCE_INLINE_ static void register_custom_data_to_otdb() {}

public:
	static constexpr bool _class_is_enabled = true;

	void notify_property_list_changed();

	static void *get_class_ptr_static() {
		static int ptr;
		return &ptr;
	}

	void detach_from_objectdb();
	_FORCE_INLINE_ ObjectID get_instance_id() const { return _instance_id; }

	template <typename T>
	static T *cast_to(Object *p_object) {
		return p_object ? dynamic_cast<T *>(p_object) : nullptr;
	}

	template <typename T>
	static const T *cast_to(const Object *p_object) {
		return p_object ? dynamic_cast<const T *>(p_object) : nullptr;
	}

	enum {
		NOTIFICATION_POSTINITIALIZE = 0,
		NOTIFICATION_PREDELETE = 1,
		NOTIFICATION_EXTENSION_RELOADED = 2,
		// Internal notification to send after NOTIFICATION_PREDELETE, not bound to scripting.
		NOTIFICATION_PREDELETE_CLEANUP = 3,
	};

	/* TYPE API */
	static void get_inheritance_list_static(List<String> *p_inheritance_list) { p_inheritance_list->push_back("Object"); }

	static String get_class_static() { return "Object"; }
	static String get_parent_class_static() { return String(); }

	virtual String get_class() const {
		if (_extension) {
			return _extension->class_name.operator String();
		}
		return "Object";
	}
	virtual String get_save_class() const { return get_class(); } //class stored when saving

	virtual bool is_class(const String &p_class) const {
		if (_extension && _extension->is_class(p_class)) {
			return true;
		}
		return (p_class == "Object");
	}
	virtual bool is_class_ptr(void *p_ptr) const { return get_class_ptr_static() == p_ptr; }

	_FORCE_INLINE_ const StringName &get_class_name() const {
		if (_extension) {
			// Can't put inside the unlikely as constructor can run it
			return _extension->class_name;
		}

		if (unlikely(!_class_name_ptr)) {
			// While class is initializing / deinitializing, constructors and destructurs
			// need access to the proper class at the proper stage.
			return *_get_class_namev();
		}
		return *_class_name_ptr;
	}

	StringName get_class_name_for_extension(const GDExtension *p_library) const;

	/* IAPI */

	void set(const StringName &p_name, const Variant &p_value, bool *r_valid = nullptr);
	Variant get(const StringName &p_name, bool *r_valid = nullptr) const;
	void set_indexed(const Vector<StringName> &p_names, const Variant &p_value, bool *r_valid = nullptr);
	Variant get_indexed(const Vector<StringName> &p_names, bool *r_valid = nullptr) const;

	void get_property_list(List<PropertyInfo> *p_list, bool p_reversed = false) const;
	void validate_property(PropertyInfo &p_property) const;
	bool property_can_revert(const StringName &p_name) const;
	Variant property_get_revert(const StringName &p_name) const;

	bool has_method(const StringName &p_method) const;
	int get_method_argument_count(const StringName &p_method, bool *r_is_valid = nullptr) const;
	void get_method_list(List<MethodInfo> *p_list) const;
	Variant callv(const StringName &p_method, const Array &p_args);
	virtual Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error);
	virtual Variant call_const(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error);

	template <typename... VarArgs>
	Variant call(const StringName &p_method, VarArgs... p_args) {
		Variant args[sizeof...(p_args) + 1] = { p_args..., Variant() }; // +1 makes sure zero sized arrays are also supported.
		const Variant *argptrs[sizeof...(p_args) + 1];
		for (uint32_t i = 0; i < sizeof...(p_args); i++) {
			argptrs[i] = &args[i];
		}
		Callable::CallError cerr;
		const Variant ret = callp(p_method, sizeof...(p_args) == 0 ? nullptr : (const Variant **)argptrs, sizeof...(p_args), cerr);
		return (cerr.error == Callable::CallError::CALL_OK) ? ret : Variant();
	}

	void notification(int p_notification, bool p_reversed = false);
	virtual String to_string();

	// Used mainly by script, get and set all INCLUDING string.
	virtual Variant getvar(const Variant &p_key, bool *r_valid = nullptr) const;
	virtual void setvar(const Variant &p_key, const Variant &p_value, bool *r_valid = nullptr);

	/* SCRIPT */

// When in debug, some non-virtual functions can be overridden for multithreaded guards.
#ifdef DEBUG_ENABLED
#define MTVIRTUAL virtual
#else
#define MTVIRTUAL
#endif

	MTVIRTUAL void set_script(const Variant &p_script);
	MTVIRTUAL Variant get_script() const;

	MTVIRTUAL bool has_meta(const StringName &p_name) const;
	MTVIRTUAL void set_meta(const StringName &p_name, const Variant &p_value);
	MTVIRTUAL void remove_meta(const StringName &p_name);
	MTVIRTUAL Variant get_meta(const StringName &p_name, const Variant &p_default = Variant()) const;
	MTVIRTUAL void get_meta_list(List<StringName> *p_list) const;
	MTVIRTUAL void merge_meta_from(const Object *p_src);

#ifdef TOOLS_ENABLED
	void set_edited(bool p_edited);
	bool is_edited() const;
	// This function is used to check when something changed beyond a point, it's used mainly for generating previews.
	uint32_t get_edited_version() const;
#endif

	void set_script_instance(ScriptInstance *p_instance);
	_FORCE_INLINE_ ScriptInstance *get_script_instance() const { return script_instance; }

	// Some script languages can't control instance creation, so this function eases the process.
	void set_script_and_instance(const Variant &p_script, ScriptInstance *p_instance);

	void add_user_signal(const MethodInfo &p_signal);

	template <typename... VarArgs>
	Error emit_signal(const StringName &p_name, VarArgs... p_args) {
		Variant args[sizeof...(p_args) + 1] = { p_args..., Variant() }; // +1 makes sure zero sized arrays are also supported.
		const Variant *argptrs[sizeof...(p_args) + 1];
		for (uint32_t i = 0; i < sizeof...(p_args); i++) {
			argptrs[i] = &args[i];
		}
		return emit_signalp(p_name, sizeof...(p_args) == 0 ? nullptr : (const Variant **)argptrs, sizeof...(p_args));
	}

	MTVIRTUAL Error emit_signalp(const StringName &p_name, const Variant **p_args, int p_argcount);
	MTVIRTUAL bool has_signal(const StringName &p_name) const;
	MTVIRTUAL void get_signal_list(List<MethodInfo> *p_signals) const;
	MTVIRTUAL void get_signal_connection_list(const StringName &p_signal, List<Connection> *p_connections) const;
	MTVIRTUAL void get_all_signal_connections(List<Connection> *p_connections) const;
	MTVIRTUAL int get_persistent_signal_connection_count() const;
	MTVIRTUAL void get_signals_connected_to_this(List<Connection> *p_connections) const;

	MTVIRTUAL Error connect(const StringName &p_signal, const Callable &p_callable, uint32_t p_flags = 0);
	MTVIRTUAL void disconnect(const StringName &p_signal, const Callable &p_callable);
	MTVIRTUAL bool is_connected(const StringName &p_signal, const Callable &p_callable) const;

	template <typename... VarArgs>
	void call_deferred(const StringName &p_name, VarArgs... p_args) {
		MessageQueue::get_singleton()->push_call(this, p_name, p_args...);
	}

	void set_deferred(const StringName &p_property, const Variant &p_value);

	void set_block_signals(bool p_block);
	bool is_blocking_signals() const;

	Variant::Type get_static_property_type(const StringName &p_property, bool *r_valid = nullptr) const;
	Variant::Type get_static_property_type_indexed(const Vector<StringName> &p_path, bool *r_valid = nullptr) const;

	// Translate message (internationalization).
	String tr(const StringName &p_message, const StringName &p_context = "") const;
	String tr_n(const StringName &p_message, const StringName &p_message_plural, int p_n, const StringName &p_context = "") const;

	bool _is_queued_for_deletion = false; // Set to true by SceneTree::queue_delete().
	bool is_queued_for_deletion() const;

	_FORCE_INLINE_ void set_message_translation(bool p_enable) { _can_translate = p_enable; }
	_FORCE_INLINE_ bool can_translate_messages() const { return _can_translate; }

#ifdef TOOLS_ENABLED
	virtual void get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const;
	void editor_set_section_unfold(const String &p_section, bool p_unfolded);
	bool editor_is_section_unfolded(const String &p_section);
	const HashSet<String> &editor_get_section_folding() const { return editor_section_folding; }
	void editor_clear_section_folding() { editor_section_folding.clear(); }

#endif

	// Used by script languages to store binding data.
	void *get_instance_binding(void *p_token, const GDExtensionInstanceBindingCallbacks *p_callbacks);
	// Used on creation by binding only.
	void set_instance_binding(void *p_token, void *p_binding, const GDExtensionInstanceBindingCallbacks *p_callbacks);
	bool has_instance_binding(void *p_token);
	void free_instance_binding(void *p_token);

#ifdef TOOLS_ENABLED
	void clear_internal_extension();
	void reset_internal_extension(ObjectGDExtension *p_extension);
	bool is_extension_placeholder() const { return _extension && _extension->is_placeholder; }
#endif

	void clear_internal_resource_paths();

	_ALWAYS_INLINE_ bool is_ref_counted() const { return type_is_reference; }

	void cancel_free();

	Object();
	virtual ~Object();
};

bool predelete_handler(Object *p_object);
void postinitialize_handler(Object *p_object);

#endif // OBJECT_H
