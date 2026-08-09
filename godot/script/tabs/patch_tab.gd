extends VBoxContainer

@onready var base_path_edit: LineEdit = $BaseRow/BasePath
@onready var project_label: Label = $ProjectLabel
@onready var project_row: HBoxContainer = $ProjectRow
@onready var project_path_edit: LineEdit = $ProjectRow/ProjectPath
@onready var patch_path_edit: LineEdit = $PatchOutputRow/PatchPath
@onready var apply_output_path_edit: LineEdit = $ApplyOutputRow/ApplyOutputPath
@onready var make_patch_button: Button = $PatchButtons/MakePatchButton

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

	controller.run_gddelta([
		"make-pck",
		base_path_edit.text,
		project_path_edit.text,
		patch_path_edit.text,
	])

func _on_apply_pressed() -> void:
	var controller := _controller()
	if(controller == null): return

	if(!controller.validate_required_paths([
		[tr("BASE_PATH"), base_path_edit],
		[tr("PATCH_PATH"), patch_path_edit],
		[tr("OUTPUT_DIRECTORY"), apply_output_path_edit],
	])): return;

	controller.run_gddelta([
		"apply",
		base_path_edit.text,
		patch_path_edit.text,
		apply_output_path_edit.text,
	])

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
		controller.browse_path(
			patch_path_edit,
			FileDialog.FILE_MODE_SAVE_FILE,
			PackedStringArray(["*.pck ; Patch Pack"])
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
