extends VBoxContainer

@onready var dev_base_path_edit: LineEdit = %LiveBasePath
@onready var dev_project_path_edit: LineEdit = %LiveProjectPath
@onready var dev_patch_path_edit: LineEdit = %LivePatchPath
@onready var sandbox_path_edit: LineEdit = %LiveSandboxPath
@onready var interval_edit: LineEdit = %LiveIntervalPath
@onready var watch_button: Button = %WatchButton

func _ready() -> void:
	var controller := _controller()
	if(controller != null): _on_watch_state_changed(controller.is_watch_running())

func _on_watch_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;

	if(controller.is_watch_running()):
		if(!controller.stop_watch_process()):
			controller.append_log(tr("FAILED_TO_STOP_WATCH_PROCESS"))
		return

	if(!controller.validate_required_paths([
		[tr("BASE_PATH"), dev_base_path_edit],
		[tr("PROJECT_PATH"), dev_project_path_edit],
		[tr("LIVE_PATCH_PCK"), dev_patch_path_edit],
		[tr("SANDBOX_DIRECTORY"), sandbox_path_edit],
	])): return;

	controller.start_watch_process(
		dev_base_path_edit.text,
		dev_project_path_edit.text,
		dev_patch_path_edit.text,
		sandbox_path_edit.text,
		interval_edit.text
	)

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

func _on_dev_patch_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;

	controller.browse_path(
		dev_patch_path_edit,
		FileDialog.FILE_MODE_SAVE_FILE,
		PackedStringArray(["*.pck ; Patch Pack"])
	)

func _on_sandbox_browse_pressed() -> void:
	var controller := _controller()
	if(controller == null): return;

	controller.browse_path(sandbox_path_edit, FileDialog.FILE_MODE_OPEN_DIR)

func _on_watch_state_changed(running: bool) -> void:
	if(running):
		watch_button.text = tr("STOP_WATCH")
		return

	watch_button.text = tr("START_WATCH")

func _controller() -> Node:
	return get_tree().current_scene
