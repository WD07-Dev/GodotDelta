extends VBoxContainer

@onready var dev_base_path_edit: LineEdit = %DevBasePath
@onready var dev_project_path_edit: LineEdit = %DevProjectPath
@onready var sandbox_path_edit: LineEdit = %SandboxPath

func _on_dev_build_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;

	if(!controller.validate_required_paths([
		[tr("BASE_PATH"), dev_base_path_edit],
		[tr("PROJECT_PATH"), dev_project_path_edit],
		[tr("SANDBOX_DIRECTORY"), sandbox_path_edit],
	])): return;

	controller.run_gddelta([
		"dev-build",
		dev_base_path_edit.text,
		dev_project_path_edit.text,
		sandbox_path_edit.text,
	])

func _on_run_button_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;

	if(!controller.validate_required_paths([
		[tr("SANDBOX_DIRECTORY"), sandbox_path_edit],
	])): return;

	controller.run_sandbox_game(dev_base_path_edit.text, sandbox_path_edit.text)

func _on_dev_base_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;

	controller.browse_path(
		dev_base_path_edit,
		FileDialog.FILE_MODE_OPEN_FILE,
		PackedStringArray(["*.pck, *.exe ; Game Pack Or Executable"])
	)

func _on_dev_project_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;
	controller.browse_path(dev_project_path_edit, FileDialog.FILE_MODE_OPEN_DIR)

func _on_sandbox_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;
	controller.browse_path(sandbox_path_edit, FileDialog.FILE_MODE_OPEN_DIR)

func _controller() -> Node:
	return get_tree().current_scene
