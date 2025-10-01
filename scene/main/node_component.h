/**************************************************************************/
/*  node_component.h                                                      */
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

#include "core/object/object.h"
#include "core/variant/typed_array.h"

class Node;
class SceneTree;
class Viewport;
class Window;
class InputEvent;

class NodeComponent : public Object {
    GDCLASS(NodeComponent, Object);
    
protected:
    Node *owner_node = nullptr;
    bool enabled = true;
    String component_name;
    bool is_ready_called = false;
    bool is_in_tree = false;
    
    static void _bind_methods();
    
    // Internal lifecycle methods called by owner node
    void _process_component(double p_delta);
    void _physics_process_component(double p_delta);
    void _notification_component(int p_what);
    void _input_component(const Ref<InputEvent> &p_event);
    void _unhandled_input_component(const Ref<InputEvent> &p_event);
    void _unhandled_key_input_component(const Ref<InputEvent> &p_event);
    void _shortcut_input_component(const Ref<InputEvent> &p_event);
    
    // Owner connection management
    void _connect_to_owner();
    void _disconnect_from_owner();
    
    // Owner signal callbacks
    void _on_owner_ready();
    void _on_owner_enter_tree();
    void _on_owner_exit_tree();
    
    // Property system integration
    void _get_property_list_component(List<PropertyInfo> *p_list) const;
    bool _get_component(const StringName &p_name, Variant &r_ret) const;
    bool _set_component(const StringName &p_name, const Variant &p_property);

    // Internal editor method - not bound to scripting
    virtual void _validate_property(PropertyInfo &p_property) const;
    
public:
    // Core component interface
    void set_owner_node(Node *p_node);
    Node *get_owner_node() const;
    
    void set_enabled(bool p_enabled);
    bool is_enabled() const;
    
    void set_component_name(const String &p_name);
    String get_component_name() const;
    
    // Lifecycle hooks (can be overridden in GDScript)
    virtual void _ready();
    virtual void _process(double p_delta);
    virtual void _physics_process(double p_delta);
    virtual void _notification(int p_what);
    virtual void _enter_tree();
    virtual void _exit_tree();
    virtual void _input(const Ref<InputEvent> &p_event);
    virtual void _unhandled_input(const Ref<InputEvent> &p_event);
    virtual void _unhandled_key_input(const Ref<InputEvent> &p_event);
    virtual void _shortcut_input(const Ref<InputEvent> &p_event);
    
    // Property system integration
    virtual void _get_property_list(List<PropertyInfo> *p_list) const;
    virtual bool _get(const StringName &p_name, Variant &r_ret) const;
    virtual bool _set(const StringName &p_name, const Variant &p_property);
    
    // Editor integration
    virtual PackedStringArray _get_configuration_warnings() const;
    
    // Component management
    NodeComponent *get_component_by_class(const StringName &p_class_name) const;
    Array get_components() const;
    bool has_component(const StringName &p_class_name) const;
    
    // Utility methods
    bool is_ready() const;
    bool is_inside_tree() const;
    SceneTree *get_tree() const;
    Viewport *get_viewport() const;
    Window *get_window() const;
    
    // Signal emission helpers
    void emit_signal_component(const StringName &p_signal, const Variant **p_args, int p_argcount);
    template<typename... VarArgs>
    void emit_signal_component(const StringName &p_signal, VarArgs... p_args);
    
    // Editor integration
    PackedStringArray get_configuration_warnings() const;
    void validate_property(PropertyInfo &p_property) const;
    
    // Debug and utility - removed override specifier
    virtual String to_string() const;
    
    // Static utility methods
    static bool is_component_class(const StringName &p_class_name);
    static Array get_all_component_classes();
    
    NodeComponent();
    virtual ~NodeComponent();
};