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
	switch (p_id) {
		case 0: // New Component...
			_show_new_component_dialog();
			break;
		case 1: // Load...
			_show_component_file_dialog();
			break;
		case 2: // Clear Components
			// Clear all components
			EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
			undo_redo->create_action("Clear All Components");
			undo_redo->add_do_method(current_node, "remove_all_components");
			undo_redo->add_undo_method(this, "_refresh_components");
			undo_redo->commit_action();
			_refresh_components();
			break;
	}
}

void ComponentInspectorPlugin::_show_component_menu() {
	if (component_menu && component_menu->is_inside_tree()) {
		// Simple popup without complex positioning
		component_menu->popup_centered();
	}
}

void ComponentInspectorPlugin::_component_resource_selected(const Ref<Resource> &p_resource) {
	// Handle component resource selection
	if (p_resource.is_valid()) {
		print_line("Component resource selected: " + p_resource->get_class());
	}
}

void ComponentInspectorPlugin::_component_resource_changed(const Ref<Resource> &p_resource) {
	// Handle component resource change
	if (p_resource.is_valid()) {
		print_line("Component resource changed: " + p_resource->get_class());
	}
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
	print_line("Component property changed: " + p_property);
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
}

/*
void ComponentInspectorPlugin::_create_component_dropdown() {
	if (!component_dropdown) {
		return;
	}
	
	component_dropdown->clear();
	
	// Add "New Component..." option (like Scripts dropdown)
	component_dropdown->add_item("New Component...");
	component_dropdown->set_item_icon(component_dropdown->get_item_count() - 1, EditorNode::get_singleton()->get_class_icon("Script"));
	
	// Add "Load..." option (like Scripts dropdown)
	component_dropdown->add_item("Load...");
	component_dropdown->set_item_icon(component_dropdown->get_item_count() - 1, EditorNode::get_singleton()->get_class_icon("Folder"));
	
	// Add "Clear Components" option (like Script section)
	component_dropdown->add_item("Clear Components");
	component_dropdown->set_item_icon(component_dropdown->get_item_count() - 1, EditorNode::get_singleton()->get_class_icon("Clear"));
	
	// Add separator
	component_dropdown->add_separator();
	
	// Get all available component classes
	Array component_classes = NodeComponent::get_all_component_classes();
	
	for (int i = 0; i < component_classes.size(); i++) {
		String class_name = component_classes[i];
		component_dropdown->add_item(class_name);
		// Add icon for component class
		component_dropdown->set_item_icon(component_dropdown->get_item_count() - 1, EditorNode::get_singleton()->get_class_icon("Node"));
	}
	
	// Button directly opens file dialog - no signal connection needed
}
*/

void ComponentInspectorPlugin::_create_component_property_editor(NodeComponent *p_component) {
	if (!p_component || !components_container) {
		return;
	}
	
	// Create a proper Inspector category (like Node, Node3D headers)
	// This will make it look exactly like the main Inspector headers
	
	// Get a better component name
	String component_name = p_component->get_class();
	Ref<Script> script = p_component->get_script();
	if (script.is_valid()) {
		String script_path = script->get_path();
		if (!script_path.is_empty()) {
			component_name = script_path.get_file().get_basename();
		}
	}
	
	// Create a header that matches EditorInspectorCategory styling using Panel
	Panel *component_header = memnew(Panel);
	component_header->set_name("ComponentHeader_" + component_name);
	component_header->set_custom_minimum_size(Size2(0, 24)); // Match Inspector category height
	
	// Apply Inspector category background styling
	Ref<StyleBox> category_bg = EditorNode::get_singleton()->get_editor_theme()->get_stylebox("bg", "EditorInspectorCategory");
	if (category_bg.is_valid()) {
		component_header->add_theme_style_override("panel", category_bg);
		print_line("Applied Inspector category background to component header");
	} else {
		print_line("Failed to get Inspector category background style");
	}
	
	// Create the header content container with proper vertical alignment
	HBoxContainer *header_content = memnew(HBoxContainer);
	header_content->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	header_content->set_offset(SIDE_LEFT, 8);
	header_content->set_offset(SIDE_RIGHT, -8);
	header_content->set_offset(SIDE_TOP, 4);
	header_content->set_offset(SIDE_BOTTOM, -4);
	header_content->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	component_header->add_child(header_content);
	
	// Component name (centered, bold font like Inspector categories)
	Label *component_label = memnew(Label);
	component_label->set_text(component_name);
	component_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	component_label->set_v_size_flags(Control::SIZE_EXPAND_FILL); // Ensure label expands vertically
	component_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	component_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	component_label->set_autowrap_mode(TextServer::AUTOWRAP_OFF);
	// Apply bold font like Inspector categories
	component_label->add_theme_font_override("font", EditorNode::get_singleton()->get_editor_theme()->get_font("bold", "EditorFonts"));
	component_label->add_theme_font_size_override("font_size", EditorNode::get_singleton()->get_editor_theme()->get_font_size("bold_size", "EditorFonts"));
	header_content->add_child(component_label);
	
	// Right side controls with proper vertical alignment
	HBoxContainer *right_controls = memnew(HBoxContainer);
	right_controls->set_h_size_flags(Control::SIZE_SHRINK_CENTER);
	right_controls->set_v_size_flags(Control::SIZE_EXPAND_FILL); // Ensure controls container expands vertically
	right_controls->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	
	// Enabled checkbox (compact, no text)
	CheckBox *enabled_checkbox = memnew(CheckBox);
	enabled_checkbox->set_pressed(p_component->is_enabled());
	enabled_checkbox->set_tooltip_text("Enable/Disable Component");
	enabled_checkbox->connect("toggled", callable_mp(p_component, &NodeComponent::set_enabled));
	right_controls->add_child(enabled_checkbox);
	
	// Remove button (X button, compact)
	Button *remove_button = memnew(Button);
	remove_button->set_text("X");
	remove_button->set_custom_minimum_size(Size2(20, 20));
	remove_button->set_tooltip_text("Remove Component");
	remove_button->connect("pressed", callable_mp(this, &ComponentInspectorPlugin::_remove_component_button_pressed).bind(p_component));
	right_controls->add_child(remove_button);
	
	// Add right controls to header content
	header_content->add_child(right_controls);
	
	// Add the header to the components container
	components_container->add_child(component_header);
	
	// Now add the component's @export properties under the category header
	// Get component property list
	List<PropertyInfo> property_list;
	p_component->get_property_list(&property_list);
	
	for (const PropertyInfo &property : property_list) {
		// Debug: Let's see what properties we have
		print_line("Component property: " + property.name + " usage: " + itos(property.usage) + " type: " + itos(property.type));
		
		// Filter out private properties
		if (property.name.begins_with("_")) {
			continue;
		}
		
		// Filter out known internal NodeComponent properties
		if (property.name == "component_name" || 
			property.name == "enabled" || 
			property.name == "script" || 
			property.name == "is_ready_called" || 
			property.name == "is_in_tree" ||
			property.name == "owner_node") {
			continue;
		}
		
		// Filter out properties that are clearly internal (contain "Component" or "Lifecycle")
		if (property.name.find("Component") != -1 || 
			property.name.find("Lifecycle") != -1) {
			continue;
		}
		
		// Create property editor
		HBoxContainer *property_row = memnew(HBoxContainer);
		
		// Property label
		Label *property_label = memnew(Label);
		property_label->set_text(property.name.capitalize());
		property_label->set_custom_minimum_size(Size2(120, 0));
		property_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		property_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
		property_row->add_child(property_label);
		
		// Property editor based on type
		Control *property_editor = nullptr;
		
		switch (property.type) {
			case Variant::BOOL: {
				CheckBox *checkbox = memnew(CheckBox);
				checkbox->set_pressed(p_component->get(property.name));
				checkbox->connect("toggled", callable_mp(this, &ComponentInspectorPlugin::_component_property_changed).bind(p_component, property.name));
				property_editor = checkbox;
			} break;
			
			case Variant::STRING: {
				LineEdit *line_edit = memnew(LineEdit);
				line_edit->set_text(p_component->get(property.name));
				line_edit->connect("text_changed", callable_mp(this, &ComponentInspectorPlugin::_component_property_changed).bind(p_component, property.name));
				property_editor = line_edit;
			} break;
			
			case Variant::INT: {
				SpinBox *spin_box = memnew(SpinBox);
				spin_box->set_value(p_component->get(property.name));
				spin_box->connect("value_changed", callable_mp(this, &ComponentInspectorPlugin::_component_property_changed).bind(p_component, property.name));
				property_editor = spin_box;
			} break;
			
			case Variant::FLOAT: {
				SpinBox *spin_box = memnew(SpinBox);
				spin_box->set_value(p_component->get(property.name));
				spin_box->set_step(0.01);
				spin_box->connect("value_changed", callable_mp(this, &ComponentInspectorPlugin::_component_property_changed).bind(p_component, property.name));
				property_editor = spin_box;
			} break;
			
			default: {
				// Generic property editor for other types
				Label *value_label = memnew(Label);
				value_label->set_text(p_component->get(property.name));
				property_editor = value_label;
			} break;
		}
		
		if (property_editor) {
			property_editor->set_h_size_flags(Control::SIZE_EXPAND_FILL);
			property_row->add_child(property_editor);
		}
		
		// Add the property row to the components container
		components_container->add_child(property_row);
	}
	
	// Add separator
	HSeparator *separator = memnew(HSeparator);
	components_container->add_child(separator);
	
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
	
	// Create components section header (similar to Script section)
	HBoxContainer *header = memnew(HBoxContainer);
	header->set_name("ComponentsHeader");
	
	// Components label
	Label *components_label = memnew(Label);
	components_label->set_text("Components");
	components_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	components_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	header->add_child(components_label);
	
	// Create button with popup menu (like Script section)
	Button *component_button = memnew(Button);
	component_button->set_custom_minimum_size(Size2(150, 0));
	component_button->set_text("Add Component");
	component_button->set_flat(true);
	component_button->set_theme_type_variation("InspectorActionButton");
	
	// Create popup menu (like Script section)
	component_menu = memnew(PopupMenu);
	component_menu->add_icon_item(EditorNode::get_singleton()->get_class_icon("Script"), "New Component...", 0);
	component_menu->add_icon_item(EditorNode::get_singleton()->get_class_icon("Folder"), "Load...", 1);
	component_menu->add_separator();
	component_menu->add_icon_item(EditorNode::get_singleton()->get_class_icon("Remove"), "Clear Components", 2);
	
	component_button->connect("pressed", callable_mp(this, &ComponentInspectorPlugin::_show_component_menu));
	component_menu->connect("id_pressed", callable_mp(this, &ComponentInspectorPlugin::_component_menu_selected));
	
	// Add popup menu to scene tree
	EditorNode::get_singleton()->get_gui_base()->add_child(component_menu);
	
	header->add_child(component_button);
	
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
