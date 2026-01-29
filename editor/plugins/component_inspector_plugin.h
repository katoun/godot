/**************************************************************************/
/*  component_inspector_plugin.h                                         */
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

#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/script/script_create_dialog.h"
#include "scene/main/node.h"
#include "scene/main/node_component.h"
#include "scene/gui/option_button.h"
#include "scene/gui/box_container.h"

class ComponentInspectorPlugin : public EditorInspectorPlugin {
	GDCLASS(ComponentInspectorPlugin, EditorInspectorPlugin);

public:
	ComponentInspectorPlugin();
	~ComponentInspectorPlugin();

	virtual bool can_handle(Object *p_object) override;
	virtual void parse_begin(Object *p_object) override;
	virtual bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide = false) override;
	virtual void parse_end();

private:
	Node *current_node = nullptr;
	Array current_components;
	VBoxContainer *components_container = nullptr;
	HashMap<NodeComponent *, Control *> component_property_editors;
	EditorFileDialog *component_file_dialog = nullptr;
	ScriptCreateDialog *component_script_dialog = nullptr;
	
	// Script section style dropdown (like Script section)
	Button *component_button = nullptr;
	PopupMenu *component_menu = nullptr;
	Vector<String> available_component_scripts;

	// Script property menu for each component
	PopupMenu *component_script_menu = nullptr;
	NodeComponent *current_script_menu_component = nullptr;

	void _populate_component_menu();
	void _find_component_scripts(const String &p_path, List<String> &r_scripts);
	void _show_component_menu();
	void _edit_component_script(NodeComponent *p_component);
	void _show_component_script_menu(NodeComponent *p_component, Button *p_button);
	void _component_script_menu_selected(int p_id);
	void _component_menu_selected(int p_id);
	void _component_resource_selected(const Ref<Resource> &p_resource);
	void _component_resource_changed(const Ref<Resource> &p_resource);
	void _remove_component_button_pressed(NodeComponent *p_component);
	void _component_property_changed(NodeComponent *p_component, const String &p_property);
	void _refresh_components();
	void _create_component_property_editor(NodeComponent *p_component);
	void _remove_component_property_editor(NodeComponent *p_component);
	void _add_components_section();
	void _show_new_component_dialog();
	void _show_component_file_dialog();
	void _component_file_selected(const String &p_path);
	void _create_new_component_script(const String &p_path);

	// Helper methods for component property editor
	Panel *_create_component_header(NodeComponent *p_component, const String &p_component_name);
	Control *_create_property_editor(NodeComponent *p_component, const PropertyInfo &p_property);
};
