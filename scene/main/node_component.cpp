/**************************************************************************/
/*  node_component.cpp                                                    */
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

#include "node_component.h"

#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/string/print_string.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

// Forward declarations
class Node;

void NodeComponent::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &NodeComponent::set_enabled);
    ClassDB::bind_method(D_METHOD("is_enabled"), &NodeComponent::is_enabled);
    ClassDB::bind_method(D_METHOD("get_owner_node"), &NodeComponent::get_owner_node);
    ClassDB::bind_method(D_METHOD("get_component_name"), &NodeComponent::get_component_name);
    ClassDB::bind_method(D_METHOD("set_component_name", "name"), &NodeComponent::set_component_name);
    
    // Lifecycle methods - these can be overridden in GDScript
    ClassDB::bind_method(D_METHOD("_ready"), &NodeComponent::_ready);
    ClassDB::bind_method(D_METHOD("_process", "delta"), &NodeComponent::_process);
    ClassDB::bind_method(D_METHOD("_physics_process", "delta"), &NodeComponent::_physics_process);
    ClassDB::bind_method(D_METHOD("_notification", "what"), &NodeComponent::_notification);
    ClassDB::bind_method(D_METHOD("_enter_tree"), &NodeComponent::_enter_tree);
    ClassDB::bind_method(D_METHOD("_exit_tree"), &NodeComponent::_exit_tree);
    ClassDB::bind_method(D_METHOD("_input", "event"), &NodeComponent::_input);
    ClassDB::bind_method(D_METHOD("_unhandled_input", "event"), &NodeComponent::_unhandled_input);
    ClassDB::bind_method(D_METHOD("_unhandled_key_input", "event"), &NodeComponent::_unhandled_key_input);
    ClassDB::bind_method(D_METHOD("_shortcut_input", "event"), &NodeComponent::_shortcut_input);
    
    // Editor methods - only bind the ones that can be called from GDScript
    ClassDB::bind_method(D_METHOD("_get_configuration_warnings"), &NodeComponent::_get_configuration_warnings);
    // Note: _validate_property is NOT bound because PropertyInfo is not a valid scripting type
    
    // Component management
    ClassDB::bind_method(D_METHOD("get_component_by_class", "class_name"), &NodeComponent::get_component_by_class);
    ClassDB::bind_method(D_METHOD("get_components"), &NodeComponent::get_components);
    ClassDB::bind_method(D_METHOD("has_component", "class_name"), &NodeComponent::has_component);
    
    // Signals - Simplified signal declarations
    ADD_SIGNAL(MethodInfo("enabled_changed", PropertyInfo(Variant::BOOL, "enabled")));
    ADD_SIGNAL(MethodInfo("owner_changed", PropertyInfo(Variant::OBJECT, "owner")));
    
    // Properties
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "component_name"), "set_component_name", "get_component_name");
    
    // Property groups
    ADD_GROUP("Component", "component_");
    ADD_GROUP("Lifecycle", "");
}

NodeComponent::NodeComponent() {
    enabled = true;
    owner_node = nullptr;
    component_name = "";
    is_ready_called = false;
    is_in_tree = false;
}

NodeComponent::~NodeComponent() {
    // Disconnect from owner node if still connected
    if (owner_node) {
        _disconnect_from_owner();
    }
}

void NodeComponent::set_owner_node(Node *p_node) {
    if (owner_node == p_node) {
        return;
    }
    
    // Disconnect from previous owner
    if (owner_node) {
        _disconnect_from_owner();
    }
    
    owner_node = p_node;
    
    // Connect to new owner
    if (owner_node) {
        _connect_to_owner();
        emit_signal("owner_changed", owner_node);
    }
}

Node *NodeComponent::get_owner_node() const {
    return owner_node;
}

void NodeComponent::set_enabled(bool p_enabled) {
    if (enabled == p_enabled) {
        return;
    }
    
    enabled = p_enabled;
    emit_signal("enabled_changed", enabled);
    
    // Note: We'll implement _component_enabled_changed in Node class later
    // For now, we'll just emit the signal
}

bool NodeComponent::is_enabled() const {
    return enabled;
}

void NodeComponent::set_component_name(const String &p_name) {
    component_name = p_name;
}

String NodeComponent::get_component_name() const {
    if (component_name.is_empty()) {
        return get_class();
    }
    return component_name;
}

// Lifecycle methods - default implementations
void NodeComponent::_ready() {
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_process(double p_delta) {
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_physics_process(double p_delta) {
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_notification(int p_what) {
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_enter_tree() {
    is_in_tree = true;
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_exit_tree() {
    is_in_tree = false;
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_input(const Ref<InputEvent> &p_event) {
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_unhandled_input(const Ref<InputEvent> &p_event) {
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_unhandled_key_input(const Ref<InputEvent> &p_event) {
    // Default implementation - can be overridden in GDScript
}

void NodeComponent::_shortcut_input(const Ref<InputEvent> &p_event) {
    // Default implementation - can be overridden in GDScript
}

// Editor methods
PackedStringArray NodeComponent::_get_configuration_warnings() const {
    PackedStringArray warnings;
    
    // Default implementation - can be overridden in GDScript
    if (!owner_node) {
        warnings.append("Component is not attached to any node");
    }
    
    return warnings;
}

void NodeComponent::_validate_property(PropertyInfo &p_property) const {
    // Default implementation - can be overridden in GDScript
}

// Component management methods - simplified for now
NodeComponent *NodeComponent::get_component_by_class(const StringName &p_class_name) const {
    if (!owner_node) {
        return nullptr;
    }
    
    // We'll implement this method in Node class
    // For now, return nullptr
    return nullptr;
}

Array NodeComponent::get_components() const {
    if (!owner_node) {
        return Array();
    }
    
    // We'll implement this method in Node class
    // For now, return empty array
    return Array();
}

bool NodeComponent::has_component(const StringName &p_class_name) const {
    if (!owner_node) {
        return false;
    }
    
    // We'll implement this method in Node class
    // For now, return false
    return false;
}

// Internal methods
void NodeComponent::_connect_to_owner() {
    if (!owner_node) {
        return;
    }
    
    // Connect to owner's lifecycle signals
    owner_node->connect("ready", callable_mp(this, &NodeComponent::_on_owner_ready));
    owner_node->connect("tree_entered", callable_mp(this, &NodeComponent::_on_owner_enter_tree));
    owner_node->connect("tree_exiting", callable_mp(this, &NodeComponent::_on_owner_exit_tree));
}

void NodeComponent::_disconnect_from_owner() {
    if (!owner_node) {
        return;
    }
    
    // Disconnect from owner's lifecycle signals
    if (owner_node->is_connected("ready", callable_mp(this, &NodeComponent::_on_owner_ready))) {
        owner_node->disconnect("ready", callable_mp(this, &NodeComponent::_on_owner_ready));
    }
    if (owner_node->is_connected("tree_entered", callable_mp(this, &NodeComponent::_on_owner_enter_tree))) {
        owner_node->disconnect("tree_entered", callable_mp(this, &NodeComponent::_on_owner_enter_tree));
    }
    if (owner_node->is_connected("tree_exiting", callable_mp(this, &NodeComponent::_on_owner_exit_tree))) {
        owner_node->disconnect("tree_exiting", callable_mp(this, &NodeComponent::_on_owner_exit_tree));
    }
}

void NodeComponent::_on_owner_ready() {
    if (!is_ready_called && enabled) {
        is_ready_called = true;
        _ready();
    }
}

void NodeComponent::_on_owner_enter_tree() {
    if (enabled) {
        _enter_tree();
    }
}

void NodeComponent::_on_owner_exit_tree() {
    if (enabled) {
        _exit_tree();
    }
}

// Called by owner node during processing
void NodeComponent::_process_component(double p_delta) {
    if (!enabled || !owner_node) {
        return;
    }
    
    _process(p_delta);
}

void NodeComponent::_physics_process_component(double p_delta) {
    if (!enabled || !owner_node) {
        return;
    }
    
    _physics_process(p_delta);
}

void NodeComponent::_notification_component(int p_what) {
    if (!enabled || !owner_node) {
        return;
    }
    
    _notification(p_what);
}

void NodeComponent::_input_component(const Ref<InputEvent> &p_event) {
    if (!enabled || !owner_node) {
        return;
    }
    
    _input(p_event);
}

void NodeComponent::_unhandled_input_component(const Ref<InputEvent> &p_event) {
    if (!enabled || !owner_node) {
        return;
    }
    
    _unhandled_input(p_event);
}

void NodeComponent::_unhandled_key_input_component(const Ref<InputEvent> &p_event) {
    if (!enabled || !owner_node) {
        return;
    }
    
    _unhandled_key_input(p_event);
}

void NodeComponent::_shortcut_input_component(const Ref<InputEvent> &p_event) {
    if (!enabled || !owner_node) {
        return;
    }
    
    _shortcut_input(p_event);
}

// Utility methods
bool NodeComponent::is_ready() const {
    return is_ready_called;
}

bool NodeComponent::is_inside_tree() const {
    return is_in_tree && owner_node && owner_node->is_inside_tree();
}

SceneTree *NodeComponent::get_tree() const {
    if (!owner_node) {
        return nullptr;
    }
    return owner_node->get_tree();
}

Viewport *NodeComponent::get_viewport() const {
    if (!owner_node) {
        return nullptr;
    }
    return owner_node->get_viewport();
}

Window *NodeComponent::get_window() const {
    if (!owner_node) {
        return nullptr;
    }
    return owner_node->get_window();
}

// Signal emission helpers
void NodeComponent::emit_signal_component(const StringName &p_signal, const Variant **p_args, int p_argcount) {
    if (!owner_node) {
        return;
    }
    
    // Emit signal on the owner node so it can be connected to from outside
    owner_node->emit_signalp(p_signal, p_args, p_argcount);
}

template<typename... VarArgs>
void NodeComponent::emit_signal_component(const StringName &p_signal, VarArgs... p_args) {
    if (!owner_node) {
        return;
    }
    
    Variant args[sizeof...(p_args) + 1] = { p_args..., Variant() };
    const Variant *argptrs[sizeof...(p_args) + 1];
    for (uint32_t i = 0; i < sizeof...(p_args); i++) {
        argptrs[i] = &args[i];
    }
    
    emit_signal_component(p_signal, sizeof...(p_args) == 0 ? nullptr : (const Variant **)argptrs, sizeof...(p_args));
}

// Property system integration
void NodeComponent::_get_property_list(List<PropertyInfo> *p_list) const {
    // Default implementation - can be overridden in GDScript
    // Add basic properties
    p_list->push_back(PropertyInfo(Variant::BOOL, "enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT));
    p_list->push_back(PropertyInfo(Variant::STRING, "component_name", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT));
}

bool NodeComponent::_get(const StringName &p_name, Variant &r_ret) const {
    if (p_name == "enabled") {
        r_ret = enabled;
        return true;
    } else if (p_name == "component_name") {
        r_ret = component_name;
        return true;
    }
    return false;
}

bool NodeComponent::_set(const StringName &p_name, const Variant &p_property) {
    if (p_name == "enabled") {
        set_enabled(p_property);
        return true;
    } else if (p_name == "component_name") {
        set_component_name(p_property);
        return true;
    }
    return false;
}

void NodeComponent::_get_property_list_component(List<PropertyInfo> *p_list) const {
    _get_property_list(p_list);
}

bool NodeComponent::_get_component(const StringName &p_name, Variant &r_ret) const {
    return _get(p_name, r_ret);
}

bool NodeComponent::_set_component(const StringName &p_name, const Variant &p_property) {
    return _set(p_name, p_property);
}

// Editor integration
PackedStringArray NodeComponent::get_configuration_warnings() const {
    return _get_configuration_warnings();
}

void NodeComponent::validate_property(PropertyInfo &p_property) const {
    _validate_property(p_property);
}

// Debug and utility
// Alternative fix using vformat
String NodeComponent::to_string() const {
    String owner_name = owner_node ? owner_node->get_name() : "None";
    return vformat("[NodeComponent:%s] Name: %s Owner: %s Enabled: %s", 
                   get_class(), 
                   component_name.is_empty() ? get_class() : component_name,
                   owner_name,
                   enabled ? "true" : "false");
}

// Static utility methods
bool NodeComponent::is_component_class(const StringName &p_class_name) {
    return ClassDB::is_parent_class(p_class_name, "NodeComponent");
}

Array NodeComponent::get_all_component_classes() {
    Array result;
    LocalVector<StringName> classes;
    ClassDB::get_class_list(classes);
    
    for (const StringName &class_name : classes) {
        if (is_component_class(class_name)) {
            result.append(class_name);
        }
    }
    
    return result;
}