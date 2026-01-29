/**************************************************************************/
/*  component_inspector_plugin.cpp                                       */
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

#include "component_inspector_plugin.h"

#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/inspector/editor_resource_picker.h"
#include "editor/docks/inspector_dock.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/button.h"
#include "scene/gui/box_container.h"
#include "scene/gui/spin_box.h"
#include "scene/main/node_component.h"
#include "modules/gdscript/gdscript.h"
#include "core/io/dir_access.h"
#include "core/io/resource_loader.h"

ComponentInspectorPlugin::ComponentInspectorPlugin() {
	component_menu = nullptr;
	component_file_dialog = nullptr;
	component_script_dialog = nullptr;
	component_script_menu = nullptr;
	current_script_menu_component = nullptr;
}

ComponentInspectorPlugin::~ComponentInspectorPlugin() {
	// Clean up dialogs and menus
	if (component_menu && component_menu->is_inside_tree()) {
		component_menu->queue_free();
	}
	if (component_file_dialog && component_file_dialog->is_inside_tree()) {
		component_file_dialog->queue_free();
	}
	if (component_script_dialog && component_script_dialog->is_inside_tree()) {
		component_script_dialog->queue_free();
	}
	if (component_script_menu && component_script_menu->is_inside_tree()) {
		component_script_menu->queue_free();
	}
}

bool ComponentInspectorPlugin::can_handle(Object *p_object) {
	Node *node = Object::cast_to<Node>(p_object);
	if (!node) {
		return false;
	}
	
	// Only show Components section if the node has components or if we want to show the section
	return true;
}

void ComponentInspectorPlugin::parse_begin(Object *p_object) {
	Node *node = Object::cast_to<Node>(p_object);
	if (!node) {
		return;
	}
	
	current_node = node;
	current_components = node->get_components();
	
	// Don't add components section here - we'll add it after the script property
}

bool ComponentInspectorPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	// Check if this is the script property - if so, add components section after it
	if (p_path == "script") {
		_add_components_section();
		return false; // Let the script property be handled normally
	}
	
	return false;
}

void ComponentInspectorPlugin::parse_end() {
	// Clean up
	current_node = nullptr;
	current_components.clear();
	components_container = nullptr;
	component_button = nullptr;
	component_property_editors.clear();

	// Don't clean up file dialog - we want to reuse it
}

void ComponentInspectorPlugin::_component_menu_selected(int p_id) {
	if (!current_node) {
		return;
	}

	// Handle menu options
	if (p_id == 0) { // New Component...
		_show_new_component_dialog();
	} else if (p_id == 1) { // Quick Load...
		_show_component_file_dialog();
	} else if (p_id == 2) { // Load...
		_show_component_file_dialog();
	} else if (p_id == 3) { // Clear Components
		// Clear all components
		EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
		undo_redo->create_action("Clear All Components");
		undo_redo->add_do_method(current_node, "remove_all_components");
		undo_redo->add_undo_method(this, "_refresh_components");
		undo_redo->commit_action();
		_refresh_components();
	} else if (p_id >= 100) { // Available component scripts
		// Get the script path from the available scripts list
		int script_index = p_id - 100;
		if (script_index >= 0 && script_index < available_component_scripts.size()) {
			String script_path = available_component_scripts[script_index];
			_component_file_selected(script_path);
		}
	}
}

void ComponentInspectorPlugin::_populate_component_menu() {
	if (!component_menu) {
		return;
	}

	// Clear existing menu items
	component_menu->clear();
	available_component_scripts.clear();

	// Add "New Component..." option
	component_menu->add_icon_item(EditorNode::get_singleton()->get_editor_theme()->get_icon("ScriptCreate", "EditorIcons"), "New Component...", 0);
	component_menu->add_separator();

	// Scan for available NodeComponent scripts in the project
	// Search in res:// directory for GDScript files
	List<String> component_scripts;
	_find_component_scripts("res://", component_scripts);

	// Add available component scripts to the menu
	int script_index = 0;
	for (const String &script_path : component_scripts) {
		// Load the script to get its class name
		Ref<GDScript> script = ResourceLoader::load(script_path);
		if (script.is_valid()) {
			String class_name = script->get_global_name();
			if (class_name.is_empty()) {
				class_name = script_path.get_file().get_basename();
			}

			// Add to menu with icon
			component_menu->add_icon_item(EditorNode::get_singleton()->get_class_icon("GDScript"), class_name, 100 + script_index);
			available_component_scripts.push_back(script_path);
			script_index++;
		}
	}

	if (component_scripts.size() > 0) {
		component_menu->add_separator();
	}

	// Add "Quick Load..." and "Load..." options
	component_menu->add_icon_item(EditorNode::get_singleton()->get_editor_theme()->get_icon("LoadQuick", "EditorIcons"), "Quick Load...", 1);
	component_menu->add_icon_item(EditorNode::get_singleton()->get_editor_theme()->get_icon("Load", "EditorIcons"), "Load...", 2);
	component_menu->add_separator();

	// Add "Clear Components" option
	component_menu->add_icon_item(EditorNode::get_singleton()->get_class_icon("Remove"), "Clear Components", 3);
}

void ComponentInspectorPlugin::_find_component_scripts(const String &p_path, List<String> &r_scripts) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		return;
	}

	dir->list_dir_begin();
	String file_name = dir->get_next();

	while (!file_name.is_empty()) {
		if (file_name == "." || file_name == "..") {
			file_name = dir->get_next();
			continue;
		}

		String full_path = p_path + "/" + file_name;

		if (dir->current_is_dir()) {
			// Recursively search subdirectories
			_find_component_scripts(full_path, r_scripts);
		} else if (file_name.ends_with(".gd")) {
			// Check if this script extends NodeComponent
			Ref<GDScript> script = ResourceLoader::load(full_path);
			if (script.is_valid()) {
				Ref<Script> base_script = script->get_base_script();
				String base_type = script->get_instance_base_type();

				// Check if extends NodeComponent
				if (base_type == "NodeComponent" ||
					(base_script.is_valid() && base_script->get_instance_base_type() == "NodeComponent")) {
					r_scripts.push_back(full_path);
				}
			}
		}

		file_name = dir->get_next();
	}

	dir->list_dir_end();
}

void ComponentInspectorPlugin::_show_component_menu() {
	if (component_menu && component_menu->is_inside_tree() && component_button) {
		// Populate menu with current available components
		_populate_component_menu();

		// Position the popup menu below the button, similar to resource pickers
		Rect2 button_rect = component_button->get_screen_rect();
		component_menu->set_position(button_rect.position + Vector2(0, button_rect.size.height));
		component_menu->reset_size();
		component_menu->popup();
	}
}

void ComponentInspectorPlugin::_edit_component_script(NodeComponent *p_component) {
	if (!p_component) {
		return;
	}

	Ref<Script> script = p_component->get_script();
	if (script.is_valid()) {
		EditorNode::get_singleton()->edit_resource(script);
	}
}

void ComponentInspectorPlugin::_show_component_script_menu(NodeComponent *p_component, Button *p_button) {
	if (!p_component || !p_button) {
		return;
	}

	current_script_menu_component = p_component;

	// Create the menu if it doesn't exist
	if (!component_script_menu) {
		component_script_menu = memnew(PopupMenu);
		component_script_menu->connect("id_pressed", callable_mp(this, &ComponentInspectorPlugin::_component_script_menu_selected));
		EditorNode::get_singleton()->get_gui_base()->add_child(component_script_menu);
	}

	// Clear and populate menu
	component_script_menu->clear();
	component_script_menu->add_icon_item(EditorNode::get_singleton()->get_editor_theme()->get_icon("Edit", "EditorIcons"), "Edit", 0);
	component_script_menu->add_icon_item(EditorNode::get_singleton()->get_class_icon("Remove"), "Clear", 1);
	component_script_menu->add_separator();
	component_script_menu->add_icon_item(EditorNode::get_singleton()->get_editor_theme()->get_icon("Folder", "EditorIcons"), "Show in FileSystem", 2);

	// Position and show menu
	Rect2 button_rect = p_button->get_screen_rect();
	component_script_menu->set_position(button_rect.position + Vector2(0, button_rect.size.height));
	component_script_menu->reset_size();
	component_script_menu->popup();
}

void ComponentInspectorPlugin::_component_script_menu_selected(int p_id) {
	if (!current_script_menu_component) {
		return;
	}

	Ref<Script> script = current_script_menu_component->get_script();
	if (!script.is_valid()) {
		return;
	}

	switch (p_id) {
		case 0: { // Edit
			EditorNode::get_singleton()->edit_resource(script);
		} break;

		case 1: { // Clear (remove component)
			if (current_node) {
				current_node->remove_component(current_script_menu_component);
				_refresh_components();
			}
		} break;

		case 2: { // Show in FileSystem
			String script_path = script->get_path();
			if (!script_path.is_empty()) {
				FileSystemDock::get_singleton()->navigate_to_path(script_path);
			}
		} break;
	}

	current_script_menu_component = nullptr;
}

void ComponentInspectorPlugin::_component_resource_selected(const Ref<Resource> &p_resource) {
	// Handle component resource selection
	// Currently unused
}

void ComponentInspectorPlugin::_component_resource_changed(const Ref<Resource> &p_resource) {
	// Handle component resource change
	// Currently unused
}

void ComponentInspectorPlugin::_remove_component_button_pressed(NodeComponent *p_component) {
	if (!current_node || !p_component) {
		return;
	}
	
	// Remove component directly (no undo/redo for now to avoid callable issues)
	current_node->remove_component(p_component);
	_refresh_components();
}

void ComponentInspectorPlugin::_component_property_changed(NodeComponent *p_component, const String &p_property) {
	if (!p_component) {
		return;
	}

	// For now, just update the component directly
	// TODO: Add proper undo/redo support and get the actual value from the control
}

void ComponentInspectorPlugin::_refresh_components() {
	if (!current_node || !components_container) {
		return;
	}
	
	// Clear existing components display
	for (int i = components_container->get_child_count() - 1; i >= 0; i--) {
		Node *child = components_container->get_child(i);
		child->queue_free();
	}
	component_property_editors.clear();
	
	// Get current components
	current_components = current_node->get_components();

	// Create component editors
	for (int i = 0; i < current_components.size(); i++) {
		NodeComponent *component = Object::cast_to<NodeComponent>(current_components[i]);
		if (component) {
			_create_component_property_editor(component);
		}
	}

	// Add separator after all components (if there are any)
	if (current_components.size() > 0) {
		HSeparator *separator = memnew(HSeparator);
		components_container->add_child(separator);
	}
}

void ComponentInspectorPlugin::_create_component_property_editor(NodeComponent *p_component) {
	if (!p_component || !components_container) {
		return;
	}

	// Get a better component name
	String component_name = p_component->get_class();
	Ref<Script> script = p_component->get_script();
	if (script.is_valid()) {
		// Try to get the class_name from the script first
		String global_name = script->get_global_name();
		if (!global_name.is_empty()) {
			component_name = global_name;
		} else {
			// Fall back to filename
			String script_path = script->get_path();
			if (!script_path.is_empty()) {
				component_name = script_path.get_file().get_basename();
			}
		}
	}

	// Create component header (capitalize name for word separation)
	Panel *component_header = _create_component_header(p_component, component_name.capitalize());
	components_container->add_child(component_header);

	// Add Script property row (similar to Script section on nodes)
	if (script.is_valid()) {
		HBoxContainer *script_row = memnew(HBoxContainer);
		script_row->add_theme_constant_override("separation", 4);

		// Script label
		Label *script_label = memnew(Label);
		script_label->set_text("Script");
		script_label->set_custom_minimum_size(Size2(120, 0));
		script_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		script_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		script_row->add_child(script_label);

		// Script value button (shows script path, clickable)
		Button *script_button = memnew(Button);
		script_button->set_text(script->get_path());
		script_button->set_text_alignment(HORIZONTAL_ALIGNMENT_LEFT);
		script_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		script_button->set_focus_mode(Control::FOCUS_NONE);
		script_button->set_tooltip_text(script->get_path());
		script_button->connect("pressed", callable_mp(this, &ComponentInspectorPlugin::_edit_component_script).bind(p_component));
		script_row->add_child(script_button);

		// Script menu button (dropdown with options)
		Button *script_menu_button = memnew(Button);
		script_menu_button->set_flat(true);
		script_menu_button->set_focus_mode(Control::FOCUS_NONE);
		script_menu_button->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon("GuiOptionArrow", "EditorIcons"));
		script_menu_button->set_custom_minimum_size(Size2(28, 0));
		script_menu_button->connect("pressed", callable_mp(this, &ComponentInspectorPlugin::_show_component_script_menu).bind(p_component, script_menu_button));
		script_row->add_child(script_menu_button);

		components_container->add_child(script_row);
	}

	// Get component property list
	List<PropertyInfo> property_list;
	p_component->get_property_list(&property_list);

	for (const PropertyInfo &property : property_list) {
		// Filter out private properties
		if (property.name.begins_with("_")) {
			continue;
		}

		// Only show properties with PROPERTY_USAGE_EDITOR flag (skip storage-only, etc.)
		if (!(property.usage & PROPERTY_USAGE_EDITOR)) {
			continue;
		}

		// Filter out known internal NodeComponent properties
		if (property.name == "component_name" ||
			property.name == "enabled" ||
			property.name == "script" ||
			property.name == "is_ready_called" ||
			property.name == "owner_node") {
			continue;
		}

		// Filter out properties that look like script filenames
		if (property.name.ends_with(".gd") || property.name.ends_with(".cs")) {
			continue;
		}

		// Filter out properties that are clearly internal
		if (property.name.find("Component") != -1 ||
			property.name.find("Lifecycle") != -1) {
			continue;
		}

		// Create property row
		HBoxContainer *property_row = memnew(HBoxContainer);

		// Property label
		Label *property_label = memnew(Label);
		property_label->set_text(property.name.capitalize());
		property_label->set_custom_minimum_size(Size2(120, 0));
		property_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		property_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		property_row->add_child(property_label);

		// Property editor based on type
		Control *property_editor = _create_property_editor(p_component, property);

		if (property_editor) {
			property_editor->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			property_row->add_child(property_editor);
		}

		// Add the property row to the components container
		components_container->add_child(property_row);
	}

	// Store reference for cleanup
	component_property_editors[p_component] = component_header;
}


void ComponentInspectorPlugin::_remove_component_property_editor(NodeComponent *p_component) {
	if (component_property_editors.has(p_component)) {
		Control *editor = component_property_editors[p_component];
		if (editor && editor->get_parent()) {
			editor->get_parent()->remove_child(editor);
			editor->queue_free();
		}
		component_property_editors.erase(p_component);
	}
}

void ComponentInspectorPlugin::_add_components_section() {
	if (!current_node) {
		return;
	}

	// Add vertical spacer above Components section (like Script section)
	Control *spacer = memnew(Control);
	spacer->set_custom_minimum_size(Size2(0, 10));
	add_custom_control(spacer);

	// Create components section header (similar to Script section)
	HBoxContainer *header = memnew(HBoxContainer);
	header->set_name("ComponentsHeader");
	
	// Components label
	Label *components_label = memnew(Label);
	components_label->set_text("Components");
	components_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	components_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	header->add_child(components_label);

	// Create container for buttons (like Script section)
	HBoxContainer *buttons_container = memnew(HBoxContainer);
	buttons_container->add_theme_constant_override("separation", 0);

	// Quick Load button (folder icon) - directly opens file dialog
	Button *quick_load_button = memnew(Button);
	quick_load_button->set_flat(true);
	quick_load_button->set_focus_mode(Control::FOCUS_NONE);
	quick_load_button->set_tooltip_text("Quick Load Component");
	quick_load_button->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon("LoadQuick", "EditorIcons"));
	quick_load_button->set_custom_minimum_size(Size2(28, 28));
	quick_load_button->connect("pressed", callable_mp(this, &ComponentInspectorPlugin::_show_component_file_dialog));
	buttons_container->add_child(quick_load_button);

	// Dropdown menu button (arrow icon) - shows menu with options
	component_button = memnew(Button);
	component_button->set_flat(true);
	component_button->set_focus_mode(Control::FOCUS_NONE);
	component_button->set_tooltip_text("Component Options");
	component_button->set_button_icon(EditorNode::get_singleton()->get_editor_theme()->get_icon("GuiOptionArrow", "EditorIcons"));
	component_button->set_custom_minimum_size(Size2(28, 28));
	component_button->connect("pressed", callable_mp(this, &ComponentInspectorPlugin::_show_component_menu));
	buttons_container->add_child(component_button);

	// Create popup menu (will be populated dynamically when shown)
	component_menu = memnew(PopupMenu);
	component_menu->connect("id_pressed", callable_mp(this, &ComponentInspectorPlugin::_component_menu_selected));

	// Add popup menu to scene tree
	EditorNode::get_singleton()->get_gui_base()->add_child(component_menu);

	header->add_child(buttons_container);
	
	// Add to inspector
	add_custom_control(header);
	
	// Create components list container
	components_container = memnew(VBoxContainer);
	components_container->set_name("ComponentsList");
	components_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	
	// Add to inspector
	add_custom_control(components_container);
	
	// Create file dialog for component scripts (only if it doesn't exist)
	if (!component_file_dialog) {
		component_file_dialog = memnew(EditorFileDialog);
		component_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
		component_file_dialog->add_filter("*.gd", "GDScript Component");
		component_file_dialog->add_filter("*.cs", "C# Component");
		component_file_dialog->connect("file_selected", callable_mp(this, &ComponentInspectorPlugin::_component_file_selected));
		// Add to the main scene tree
		EditorNode::get_singleton()->get_gui_base()->add_child(component_file_dialog);
	}
	
	// Refresh components display
	_refresh_components();
}

void ComponentInspectorPlugin::_show_new_component_dialog() {
	if (!current_node) {
		return;
	}
	
	// Create our own script creation dialog for components
	if (!component_script_dialog) {
		component_script_dialog = memnew(ScriptCreateDialog);
		component_script_dialog->set_title("Attach Component Script");
		// Add to the main scene tree
		EditorNode::get_singleton()->get_gui_base()->add_child(component_script_dialog);
	}
	
	// Configure the dialog for component creation
	String suggested_path = "res://new_component.gd";
	component_script_dialog->set_inheritance_base_type("NodeComponent");
	component_script_dialog->config("NodeComponent", suggested_path);
	
	// Show the dialog
	component_script_dialog->popup_centered();
}

void ComponentInspectorPlugin::_show_component_file_dialog() {
	if (!component_file_dialog) {
		component_file_dialog = memnew(EditorFileDialog);
		component_file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
		component_file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
		component_file_dialog->add_filter("*.gd", "GDScript Component Files");
		component_file_dialog->set_title("Load Component Script");
		component_file_dialog->connect("file_selected", callable_mp(this, &ComponentInspectorPlugin::_component_file_selected));
		EditorNode::get_singleton()->get_gui_base()->add_child(component_file_dialog);
	}
	
	component_file_dialog->popup_centered_ratio(0.5);
}

void ComponentInspectorPlugin::_component_file_selected(const String &p_path) {
	// Load the component script and create a component instance
	Ref<Script> script = ResourceLoader::load(p_path);
	if (script.is_valid()) {
		// For GDScript, check if it extends NodeComponent by looking at the source code
		bool extends_node_component = false;
		if (script->get_class() == "GDScript") {
			String source_code = script->get_source_code();
			if (source_code.find("extends NodeComponent") != -1) {
				extends_node_component = true;
			}
		} else {
			// For other script types, check the base class
			String base_type = script->get_global_name();
			extends_node_component = (base_type == "NodeComponent" || ClassDB::is_parent_class(base_type, "NodeComponent"));
		}
		
		if (extends_node_component) {
			// Add component to node first
			NodeComponent *component = current_node->add_component_by_class("NodeComponent");
			if (component) {
				// Set the script on the component
				component->set_script(script);
				component->set_component_name(script->get_path().get_file().get_basename());
				
				// Refresh components display
				_refresh_components();
			}
		} else {
			// Show error message
			EditorNode::get_singleton()->show_warning("Selected script does not extend NodeComponent");
		}
	}
}

void ComponentInspectorPlugin::_create_new_component_script(const String &p_path) {
	// Create a new component script file
	Ref<GDScript> script;
	script.instantiate();

	// Set the script content
	String script_content = "extends NodeComponent\nclass_name " + p_path.get_file().get_basename() + "\n\n# Component properties\n@export var enabled: bool = true\n\nfunc _ready():\n\tprint(\"Component ready!\")\n";
	script->set_source_code(script_content);

	// Save the script
	Error err = ResourceSaver::save(script, p_path);
	if (err == OK) {
		// Load the script and create component
		_component_file_selected(p_path);
	}
}

Panel *ComponentInspectorPlugin::_create_component_header(NodeComponent *p_component, const String &p_component_name) {
	// Create a header that matches EditorInspectorCategory styling using Panel
	Panel *component_header = memnew(Panel);
	component_header->set_name("ComponentHeader_" + p_component_name);
	component_header->set_custom_minimum_size(Size2(0, 28));

	// Apply Inspector category background styling
	Ref<StyleBox> category_bg = EditorNode::get_singleton()->get_editor_theme()->get_stylebox("bg", "EditorInspectorCategory");
	if (category_bg.is_valid()) {
		component_header->add_theme_style_override("panel", category_bg);
	}

	// Create a MarginContainer for padding
	MarginContainer *margin = memnew(MarginContainer);
	margin->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	margin->add_theme_constant_override("margin_left", 8);
	margin->add_theme_constant_override("margin_right", 8);
	margin->add_theme_constant_override("margin_top", 2);
	margin->add_theme_constant_override("margin_bottom", 2);
	component_header->add_child(margin);

	// Create the header content container
	HBoxContainer *header_content = memnew(HBoxContainer);
	header_content->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	header_content->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	header_content->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	margin->add_child(header_content);

	// Component name (centered, bold font like Inspector categories)
	Label *component_label = memnew(Label);
	component_label->set_text(p_component_name);
	component_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	component_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	component_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	component_label->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	component_label->add_theme_font_override("font", EditorNode::get_singleton()->get_editor_theme()->get_font("bold", "EditorFonts"));
	component_label->add_theme_font_size_override("font_size", EditorNode::get_singleton()->get_editor_theme()->get_font_size("bold_size", "EditorFonts"));
	header_content->add_child(component_label);

	// Right side controls
	HBoxContainer *right_controls = memnew(HBoxContainer);
	right_controls->set_h_size_flags(Control::SIZE_SHRINK_END);
	right_controls->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	right_controls->add_theme_constant_override("separation", 4);

	// Enabled checkbox (compact, no text)
	CheckBox *enabled_checkbox = memnew(CheckBox);
	enabled_checkbox->set_pressed(p_component->is_enabled());
	enabled_checkbox->set_tooltip_text("Enable/Disable Component");
	enabled_checkbox->set_focus_mode(Control::FOCUS_NONE);
	enabled_checkbox->connect("toggled", callable_mp(p_component, &NodeComponent::set_enabled));
	right_controls->add_child(enabled_checkbox);

	// Remove button (X button, compact)
	Button *remove_button = memnew(Button);
	remove_button->set_text("X");
	remove_button->set_flat(true);
	remove_button->set_focus_mode(Control::FOCUS_NONE);
	remove_button->set_custom_minimum_size(Size2(24, 24));
	remove_button->set_tooltip_text("Remove Component");
	remove_button->connect("pressed", callable_mp(this, &ComponentInspectorPlugin::_remove_component_button_pressed).bind(p_component));
	right_controls->add_child(remove_button);

	// Add right controls to header content
	header_content->add_child(right_controls);

	return component_header;
}

Control *ComponentInspectorPlugin::_create_property_editor(NodeComponent *p_component, const PropertyInfo &p_property) {
	Control *property_editor = nullptr;

	switch (p_property.type) {
		case Variant::BOOL: {
			CheckBox *checkbox = memnew(CheckBox);
			checkbox->set_pressed(p_component->get(p_property.name));
			checkbox->connect("toggled", callable_mp(this, &ComponentInspectorPlugin::_component_property_changed).bind(p_component, p_property.name));
			property_editor = checkbox;
		} break;

		case Variant::STRING: {
			LineEdit *line_edit = memnew(LineEdit);
			line_edit->set_text(p_component->get(p_property.name));
			line_edit->connect("text_changed", callable_mp(this, &ComponentInspectorPlugin::_component_property_changed).bind(p_component, p_property.name));
			property_editor = line_edit;
		} break;

		case Variant::INT: {
			SpinBox *spin_box = memnew(SpinBox);
			spin_box->set_value(p_component->get(p_property.name));
			spin_box->connect("value_changed", callable_mp(this, &ComponentInspectorPlugin::_component_property_changed).bind(p_component, p_property.name));
			property_editor = spin_box;
		} break;

		case Variant::FLOAT: {
			SpinBox *spin_box = memnew(SpinBox);
			spin_box->set_value(p_component->get(p_property.name));
			spin_box->set_step(0.01);
			spin_box->connect("value_changed", callable_mp(this, &ComponentInspectorPlugin::_component_property_changed).bind(p_component, p_property.name));
			property_editor = spin_box;
		} break;

		default: {
			// Generic property editor for other types
			Label *value_label = memnew(Label);
			value_label->set_text(p_component->get(p_property.name));
			property_editor = value_label;
		} break;
	}

	return property_editor;
}
