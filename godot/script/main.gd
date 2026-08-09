extends Control

@onready var advanced_toggle: CheckBox = %AdvancedToggle
@onready var runtime_status_label: Label = %RuntimeStatus
@onready var tabs: TabContainer = $Margin/Root/Tabs
@onready var path_dialog: FileDialog = %PathDialog
@onready var base_path_edit: LineEdit = %BasePath
@onready var project_label: Label = $Margin/Root/Tabs/Patch/ProjectLabel
@onready var project_row: HBoxContainer = $Margin/Root/Tabs/Patch/ProjectRow
@onready var project_path_edit: LineEdit = %ProjectPath
@onready var patch_label: Label = $Margin/Root/Tabs/Patch/PatchLabel
@onready var patch_output_row: HBoxContainer = $Margin/Root/Tabs/Patch/PatchOutputRow
@onready var patch_path_edit: LineEdit = %PatchPath
@onready var apply_output_path_edit: LineEdit = %ApplyOutputPath
@onready var make_patch_button: Button = %MakePatchButton
@onready var dev_mode_tab: VBoxContainer = %DevMode
@onready var dev_base_path_edit: LineEdit = %DevBasePath
@onready var dev_project_path_edit: LineEdit = %DevProjectPath
@onready var dev_patch_path_edit: LineEdit = %DevPatchPath
@onready var sandbox_path_edit: LineEdit = %SandboxPath
@onready var interval_edit: LineEdit = %IntervalPath
@onready var watch_button: Button = %WatchButton
@onready var log_output: TextEdit = %LogOutput

var gddelta_executable_path := ""
var current_path_target: LineEdit
var watch_pid := -1
var watch_log_path := ""
var watch_log_position := 0
var watch_log_poll_accumulator := 0.0

func _ready() -> void:
	gddelta_executable_path = _default_gddelta_path();
	runtime_status_label.text = "gddelta: " + gddelta_executable_path;
	_update_advanced_state();
	_prepare_gdre_tools();


func _exit_tree() -> void:
	_stop_watch_process();


func _process(delta: float) -> void:
	if(watch_pid == -1): return;

	if(!OS.is_process_running(watch_pid)):
		append_log(tr("WATCH_PROCESS_EXITED"));
		watch_pid = -1;
		_reset_watch_log_state();
		_update_watch_button();
		return;

	watch_log_poll_accumulator += delta;
	if(watch_log_poll_accumulator < 0.2): return;

	watch_log_poll_accumulator = 0;
	_poll_watch_log();


func _on_advanced_toggle_toggled(_toggled_on: bool) -> void:
	_update_advanced_state();


func _update_advanced_state() -> void:
	var advanced_enabled := advanced_toggle.button_pressed;
	project_label.visible = advanced_enabled;
	project_row.visible = advanced_enabled;
	make_patch_button.visible = advanced_enabled;
	dev_mode_tab.visible = advanced_enabled;
	tabs.get_tab_bar().set_tab_hidden(1, !advanced_enabled);
	if(!advanced_enabled && tabs.current_tab == 1):
		tabs.current_tab = 0;

func _on_make_patch_pressed() -> void:
	if(!_validate_required_paths([
		[tr("BASE_PATH"), base_path_edit],
		[tr("PROJECT_PATH"), project_path_edit],
		[tr("PATCH_OUTPUT"), patch_path_edit],
	])): return;

	run_gddelta([
		"make-pck",
		base_path_edit.text,
		project_path_edit.text,
		patch_path_edit.text,
	]);

func _on_apply_pressed() -> void:
	if(!_validate_required_paths([
		[tr("BASE_PATH"), base_path_edit],
		[tr("PATCH_PATH"), patch_path_edit],
		[tr("OUTPUT_DIRECTORY"), apply_output_path_edit],
	])): return;

	run_gddelta([
		"apply",
		base_path_edit.text,
		patch_path_edit.text,
		apply_output_path_edit.text,
	]);


func _on_dev_build_pressed() -> void:
	if(!_validate_required_paths([
		[tr("BASE_PATH"), dev_base_path_edit],
		[tr("PROJECT_PATH"), dev_project_path_edit],
		[tr("SANDBOX_DIRECTORY"), sandbox_path_edit],
	])): return;

	run_gddelta([
		"dev-build",
		dev_base_path_edit.text,
		dev_project_path_edit.text,
		sandbox_path_edit.text,
	]);


func _on_watch_pressed() -> void:
	if(watch_pid != -1):
		if(!_stop_watch_process()):
			append_log(tr("FAILED_TO_STOP_WATCH_PROCESS"));
			return;
		return;

	var executable := gddelta_executable_path.strip_edges()
	if(executable.is_empty()):
		append_log(tr("GDDELTA_EXECUTABLE_PATH_IS_EMPTY"))
		return;

	if(!_validate_required_paths([
		[tr("BASE_PATH"), dev_base_path_edit],
		[tr("PROJECT_PATH"), dev_project_path_edit],
		[tr("LIVE_PATCH_PCK"), dev_patch_path_edit],
		[tr("SANDBOX_DIRECTORY"), sandbox_path_edit],
	])): return;

	var args := PackedStringArray([
		"watch-dev-build-patch",
		dev_base_path_edit.text,
		dev_project_path_edit.text,
		dev_patch_path_edit.text,
		sandbox_path_edit.text,
		interval_edit.text,
		"--log-file",
		_resolve_watch_log_path(),
	]);
	append_log("> " + executable + " " + " ".join(args))
	var pid := OS.create_process(executable, args, false)
	if(pid == -1):
		append_log(tr("FAILED_TO_START_WATCH_PROCESS"))
		return;
	watch_pid = pid
	append_log(tr("STARTED_WATCH_PROCESS_WITH_PID") % watch_pid)
	_update_watch_button();


func _on_run_button_pressed() -> void:
	if(!_validate_required_paths([
		[tr("SANDBOX_DIRECTORY"), sandbox_path_edit],
	])): return;

	var executable := _resolve_sandbox_executable();
	if(executable.is_empty()):
		append_log(tr("FAILED_TO_FIND_RUNNABLE_GAME_EXECUTABLE_IN_SANDBOX"))
		return;

	append_log("> " + executable);
	var pid := OS.create_process(executable, PackedStringArray(), false)
	if(pid == -1):
		append_log(tr("FAILED_TO_START_SANDBOX_GAME_PROCESS"))
		return;
	append_log(tr("STARTED_SANDBOX_GAME_WITH_PID") % pid)


func _on_base_browse_pressed() -> void:
	_open_file_dialog(base_path_edit, FileDialog.FILE_MODE_OPEN_FILE, PackedStringArray(["*.pck, *.exe ; Game Pack Or Executable"]))


func _on_project_browse_pressed() -> void:
	_open_file_dialog(project_path_edit, FileDialog.FILE_MODE_OPEN_DIR)


func _on_patch_browse_pressed() -> void:
	_open_file_dialog(patch_path_edit, FileDialog.FILE_MODE_SAVE_FILE, PackedStringArray(["*.pck ; Patch Pack"]))


func _on_apply_output_browse_pressed() -> void:
	_open_file_dialog(apply_output_path_edit, FileDialog.FILE_MODE_OPEN_DIR)


func _on_dev_base_browse_pressed() -> void:
	_open_file_dialog(dev_base_path_edit, FileDialog.FILE_MODE_OPEN_FILE, PackedStringArray(["*.pck, *.exe ; Game Pack Or Executable"]))


func _on_dev_project_browse_pressed() -> void:
	_open_file_dialog(dev_project_path_edit, FileDialog.FILE_MODE_OPEN_DIR)


func _on_dev_patch_browse_pressed() -> void:
	_open_file_dialog(dev_patch_path_edit, FileDialog.FILE_MODE_SAVE_FILE, PackedStringArray(["*.pck ; Patch Pack"]))


func _on_sandbox_browse_pressed() -> void:
	_open_file_dialog(sandbox_path_edit, FileDialog.FILE_MODE_OPEN_DIR)


func _on_path_dialog_file_selected(path: String) -> void:
	if(current_path_target != null): current_path_target.text = path;


func _on_path_dialog_dir_selected(dir: String) -> void:
	if(current_path_target != null): current_path_target.text = dir;

func run_gddelta(args: Array[String]) -> void:
	var executable := gddelta_executable_path.strip_edges()
	if(executable.is_empty()):
		append_log(tr("GDDELTA_EXECUTABLE_PATH_IS_EMPTY"))
		return;

	var output := []
	append_log("> " + executable + " " + " ".join(args))
	var exit_code := OS.execute(executable, PackedStringArray(args), output, true, false)
	if(output.is_empty()):
		append_log(tr("NO_OUTPUT"))
	else:
		for line in output:
			append_log(str(line))
	append_log(tr("EXIT_CODE") % exit_code);


func append_log(message: String) -> void:
	log_output.text += message + "\n"
	log_output.scroll_vertical = log_output.get_line_count()


func _validate_required_paths(required_fields: Array) -> bool:
	for entry in required_fields:
		var label: String = entry[0]
		var edit: LineEdit = entry[1]
		if(edit.text.strip_edges().is_empty()):
			append_log(tr("FIELD_IS_REQUIRED") % label)
			return false

	return true;


func _validate_dialog_target_path(target: LineEdit) -> bool:
	if(target == null):
		append_log(tr("PATH_TARGET_IS_MISSING"))
		return false;

	return true


func _open_file_dialog(target: LineEdit, mode: FileDialog.FileMode, filters: PackedStringArray = PackedStringArray()) -> void:
	if(!_validate_dialog_target_path(target)): return;

	current_path_target = target
	path_dialog.file_mode = mode
	path_dialog.filters = filters
	path_dialog.current_path = target.text
	path_dialog.current_dir = target.text if mode == FileDialog.FILE_MODE_OPEN_DIR else target.text.get_base_dir()
	path_dialog.popup_centered_ratio(0.75)


func _default_gddelta_path() -> String:
	var executable_dir := OS.get_executable_path().get_base_dir()
	var windows_path := executable_dir.path_join("gddelta.exe")
	if(FileAccess.file_exists(windows_path)):
		return windows_path

	var linux_path := executable_dir.path_join("gddelta")
	if(FileAccess.file_exists(linux_path)):
		return linux_path

	return windows_path;


func _prepare_gdre_tools() -> void:
	var executable := gddelta_executable_path.strip_edges()
	if(executable.is_empty()):
		return

	var output := []
	var exit_code := OS.execute(executable, PackedStringArray(["bootstrap"]), output, true, false)
	if(exit_code != 0):
		append_log(tr("FAILED_TO_PREPARE_GDRE_TOOLS"))
		for line in output:
			append_log(str(line))


func _update_watch_button() -> void:
	if(watch_pid == -1):
		watch_button.text = tr("START_WATCH")
	else:
		watch_button.text = tr("STOP_WATCH")


func _stop_watch_process() -> bool:
	if(watch_pid == -1):
		_update_watch_button()
		return true

	var previous_pid := watch_pid
	var success := OS.kill(previous_pid) == OK

	watch_pid = -1
	_reset_watch_log_state()
	_update_watch_button()
	if(!success):
		append_log(tr("FAILED_TO_STOP_WATCH_PROCESS_WITH_PID") % previous_pid)
		return false
	append_log(tr("STOPPED_WATCH_PROCESS_WITH_PID") % previous_pid)
	return true


func _resolve_watch_log_path() -> String:
	var sandbox_dir := sandbox_path_edit.text.strip_edges()
	if(sandbox_dir.is_empty()):
		sandbox_dir = OS.get_user_data_dir()
	var log_file_path := sandbox_dir.path_join(".gddelta_watch.log")
	_ensure_watch_log_file(log_file_path)
	watch_log_path = log_file_path
	watch_log_position = 0
	watch_log_poll_accumulator = 0;
	return log_file_path


func _ensure_watch_log_file(path: String) -> void:
	var dir_path := path.get_base_dir()
	if(!dir_path.is_empty()):
		DirAccess.make_dir_recursive_absolute(dir_path)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if(file != null):
		file.store_string("")


func _reset_watch_log_state() -> void:
	watch_log_path = ""
	watch_log_position = 0
	watch_log_poll_accumulator = 0.0


func _poll_watch_log() -> void:
	if(watch_log_path.is_empty() || !FileAccess.file_exists(watch_log_path)): return;

	var file := FileAccess.open(watch_log_path, FileAccess.READ)
	if(file == null): return;

	var file_length := file.get_length()
	if(watch_log_position > file_length):
		watch_log_position = 0;
	if(watch_log_position == file_length):
		return;

	file.seek(watch_log_position)
	var chunk := file.get_buffer(file_length - watch_log_position).get_string_from_utf8()
	watch_log_position = file.get_position()
	for line in chunk.split("\n", false):
		append_log(line.rstrip("\r"))


func _resolve_sandbox_executable() -> String:
	var sandbox_dir := sandbox_path_edit.text.strip_edges()
	if(sandbox_dir.is_empty()): return "";

	var base_path := dev_base_path_edit.text.strip_edges()
	if(!base_path.is_empty()):
		var base_file_name := base_path.get_file()
		var base_extension := base_path.get_extension().to_lower()
		if(base_extension == "exe"):
			var direct_candidate := sandbox_dir.path_join(base_file_name)
			if(FileAccess.file_exists(direct_candidate)):
				return direct_candidate
		elif(!base_file_name.is_empty()):
			var stem_candidate := sandbox_dir.path_join(base_path.get_basename().get_file() + ".exe")
			if(FileAccess.file_exists(stem_candidate)):
				return stem_candidate

	var sandbox_files := DirAccess.get_files_at(sandbox_dir)
	for file_name in sandbox_files:
		if(file_name.get_extension().to_lower() == "exe"):
			return sandbox_dir.path_join(file_name)

	return "";
