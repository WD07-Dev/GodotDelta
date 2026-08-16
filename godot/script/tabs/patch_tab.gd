extends VBoxContainer

@onready var base_path_edit: LineEdit = $BaseRow/BasePath
@onready var project_label: Label = $ProjectLabel
@onready var project_row: HBoxContainer = $ProjectRow
@onready var project_path_edit: LineEdit = $ProjectRow/ProjectPath
@onready var patch_path_edit: LineEdit = $PatchOutputRow/PatchPath
@onready var overwrite_checkbox: CheckBox = $OverwriteCheckBox
@onready var apply_output_label: Label = $ApplyLabel
@onready var apply_output_row: HBoxContainer = $ApplyOutputRow
@onready var apply_output_path_edit: LineEdit = $ApplyOutputRow/ApplyOutputPath
@onready var make_patch_button: Button = $PatchButtons/MakePatchButton

func _ready() -> void:
	_update_apply_output_visibility()

func set_advanced_enabled(enabled: bool) -> void:
	project_label.visible = enabled
	project_row.visible = enabled
	make_patch_button.visible = enabled

func _on_make_patch_pressed() -> void:
	var controller := _controller()
	if(controller == null): return

	if(!controller.validate_required_paths([
		[tr("BASE_PATH"), base_path_edit],
		[tr("PROJECT_PATH"), project_path_edit],
		[tr("PATCH_OUTPUT"), patch_path_edit],
	])): return;

	var output_path := patch_path_edit.text.strip_edges()
	var extension := output_path.get_extension().to_lower()
	if(extension.is_empty()):
		output_path += ".gdmod"
		patch_path_edit.text = output_path

	var args: Array = [
		"make",
		base_path_edit.text,
		project_path_edit.text,
		output_path,
	]
	args.append_array(controller.get_base_key_args())
	controller.run_gddelta(args)

func _on_apply_pressed() -> void:
	var controller := _controller()
	if(controller == null): return

	if(!controller.validate_required_paths([
		[tr("BASE_PATH"), base_path_edit],
		[tr("PATCH_PATH"), patch_path_edit],
	])): return;

	var args: Array = [
		"apply",
		base_path_edit.text,
		patch_path_edit.text,
	]
	var output_path := apply_output_path_edit.text.strip_edges()
	if(!overwrite_checkbox.button_pressed && !output_path.is_empty()):
		args.append(output_path)
	args.append_array(controller.get_base_key_args())

	controller.run_gddelta(args)

func _on_overwrite_check_box_toggled(_toggled_on: bool) -> void:
	_update_apply_output_visibility()

func _on_base_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;

	controller.browse_path(
		base_path_edit,
		FileDialog.FILE_MODE_OPEN_FILE,
		PackedStringArray(["*.pck, *.exe ; Game Pack Or Executable"])
	)

func _on_project_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;
	controller.browse_path(project_path_edit, FileDialog.FILE_MODE_OPEN_DIR)

func _on_patch_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;
	if(controller.is_advanced_enabled()):
		if(patch_path_edit.text.strip_edges().is_empty()):
			patch_path_edit.text = "mod.gdmod"
		controller.browse_path(
			patch_path_edit,
			FileDialog.FILE_MODE_SAVE_FILE,
			PackedStringArray([
				"*.gdmod ; GDMOD Package",
				"*.pck ; Patch Pack",
			])
		)
		return;

	controller.browse_path(
		patch_path_edit,
		FileDialog.FILE_MODE_OPEN_FILE,
		PackedStringArray(["*.pck, *.gdmod ; Patch Or GDMOD"])
	)

func _on_apply_output_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return

	controller.browse_path(apply_output_path_edit, FileDialog.FILE_MODE_OPEN_DIR)

func _controller() -> Node:
	return get_tree().current_scene

func _update_apply_output_visibility() -> void:
	var show_output_path := !overwrite_checkbox.button_pressed
	apply_output_label.visible = show_output_path
	apply_output_row.visible = show_output_path
