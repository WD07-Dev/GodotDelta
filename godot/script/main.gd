extends Control
signal watch_state_changed(running: bool)

@onready var advanced_toggle: CheckBox = $Margin/Root/AdvancedToggle
@onready var runtime_status_label: Label = $Margin/Root/RuntimeStatus
@onready var tabs: TabContainer = $Margin/Root/Tabs
@onready var patch_tab: VBoxContainer = $Margin/Root/Tabs/Patch
@onready var path_dialog: FileDialog = $PathDialog
@onready var log_output: TextEdit = $Margin/Root/LogOutput

var gddelta_executable_path := ""
var current_path_target: LineEdit = null
var watch_pid := -1
var watch_log_path := ""
var watch_log_position := 0
var watch_log_poll_accumulator := 0.0
var command_thread: Thread = null
var command_running := false
var command_completion: Callable = Callable()
var command_log_path := ""
var command_log_position := 0
var command_log_poll_accumulator := 0.0

func _ready() -> void:
	gddelta_executable_path = _default_gddelta_path()
	_update_runtime_status()
	_update_advanced_state()

func _exit_tree() -> void:
	stop_watch_process()

func _process(delta: float) -> void:
	_poll_command_log(delta)
	_poll_command_thread()

	if(watch_pid == -1): return;

	if(!OS.is_process_running(watch_pid)):
		append_log(tr("WATCH_PROCESS_EXITED"))
		watch_pid = -1
		_reset_watch_log_state()
		_emit_watch_state_changed()
		return

	watch_log_poll_accumulator += delta
	if(watch_log_poll_accumulator < 0.2): return;

	watch_log_poll_accumulator = 0.0
	_poll_watch_log()

func _on_advanced_toggle_toggled(_toggled_on: bool) -> void:
	_update_advanced_state()

func is_advanced_enabled() -> bool:
	return advanced_toggle.button_pressed

func is_watch_running() -> bool:
	return watch_pid != -1

func is_command_running() -> bool:
	return command_running

func run_gddelta(args: Array, completion: Callable = Callable()) -> int:
	if(command_running):
		append_log(tr("GDDELTA_COMMAND_ALREADY_RUNNING"))
		return ERR_BUSY

	var executable := gddelta_executable_path.strip_edges()
	if(executable.is_empty()):
		append_log(tr("GDDELTA_EXECUTABLE_PATH_IS_EMPTY"))
		return ERR_FILE_NOT_FOUND

	append_log("> " + executable + " " + " ".join(args))
	append_log(tr("GDDELTA_COMMAND_STARTED"))
	var command_args := PackedStringArray(args)
	command_args.append("--log-file")
	command_args.append(_resolve_command_log_path())
	command_thread = Thread.new()
	command_running = true
	command_completion = completion
	_set_command_controls_enabled(false)
	var error := command_thread.start(_run_gddelta_worker.bind(executable, command_args))
	if(error != OK):
		command_thread = null
		command_running = false
		command_completion = Callable()
		_reset_command_log_state()
		_set_command_controls_enabled(true)
		append_log(tr("FAILED_TO_START_GDDELTA_COMMAND_THREAD"))
		return error

	return OK

func append_log(message: String) -> void:
	log_output.text += message + "\n"
	log_output.scroll_vertical = log_output.get_line_count()

func validate_required_paths(required_fields: Array) -> bool:
	for entry in required_fields:
		var label: String = entry[0]
		var edit: LineEdit = entry[1]
		if(edit.text.strip_edges().is_empty()):
			append_log(tr("FIELD_IS_REQUIRED") % label)
			return false

	return true

func browse_path(target: LineEdit, mode: FileDialog.FileMode, filters: PackedStringArray = PackedStringArray()) -> void:
	if(target == null):
		append_log(tr("PATH_TARGET_IS_MISSING"))
		return

	current_path_target = target
	path_dialog.file_mode = mode
	path_dialog.filters = filters
	path_dialog.current_path = target.text
	if(mode == FileDialog.FILE_MODE_OPEN_DIR):
		path_dialog.current_dir = target.text
	else:
		path_dialog.current_dir = target.text.get_base_dir()
	path_dialog.popup_centered_ratio(0.75)

func start_watch_process(
	base_path: String,
	project_path: String,
	patch_path: String,
	sandbox_path: String,
	interval_text: String
) -> bool:
	var executable := gddelta_executable_path.strip_edges()
	if(executable.is_empty()):
		append_log(tr("GDDELTA_EXECUTABLE_PATH_IS_EMPTY"))
		return false

	var args := PackedStringArray([
		"watch-dev-build-patch",
		base_path,
		project_path,
		patch_path,
		sandbox_path,
		interval_text,
		"--log-file",
		_resolve_watch_log_path(sandbox_path),
	])
	append_log("> " + executable + " " + " ".join(args))
	var pid := OS.create_process(executable, args, false)
	if(pid == -1):
		append_log(tr("FAILED_TO_START_WATCH_PROCESS"))
		return false

	watch_pid = pid
	append_log(tr("STARTED_WATCH_PROCESS_WITH_PID") % watch_pid)
	_emit_watch_state_changed()
	return true

func stop_watch_process() -> bool:
	if(watch_pid == -1):
		_emit_watch_state_changed()
		return true

	var previous_pid := watch_pid
	var success := OS.kill(previous_pid) == OK

	watch_pid = -1
	_reset_watch_log_state()
	_emit_watch_state_changed()
	if(!success):
		append_log(tr("FAILED_TO_STOP_WATCH_PROCESS_WITH_PID") % previous_pid)
		return false

	append_log(tr("STOPPED_WATCH_PROCESS_WITH_PID") % previous_pid)
	return true

func run_sandbox_game(base_path: String, sandbox_path: String) -> void:
	var executable := _resolve_sandbox_executable(base_path, sandbox_path)
	if(executable.is_empty()):
		append_log(tr("FAILED_TO_FIND_RUNNABLE_GAME_EXECUTABLE_IN_SANDBOX"))
		return

	append_log("> " + executable)
	var pid := OS.create_process(executable, PackedStringArray(), false)
	if(pid == -1):
		append_log(tr("FAILED_TO_START_SANDBOX_GAME_PROCESS"))
		return

	append_log(tr("STARTED_SANDBOX_GAME_WITH_PID") % pid)

func _update_advanced_state() -> void:
	var advanced_enabled := advanced_toggle.button_pressed
	tabs.get_tab_bar().set_tab_hidden(1, !advanced_enabled)
	tabs.get_tab_bar().set_tab_hidden(2, !advanced_enabled)
	if(!advanced_enabled && tabs.current_tab > 0):
		tabs.current_tab = 0

	if(patch_tab.has_method("set_advanced_enabled")):
		patch_tab.call("set_advanced_enabled", advanced_enabled)

func _on_path_dialog_file_selected(path: String) -> void:
	if(current_path_target != null):
		current_path_target.text = path

func _on_path_dialog_dir_selected(dir: String) -> void:
	if(current_path_target != null):
		current_path_target.text = dir

func _default_gddelta_path() -> String:
	var executable_dir := OS.get_executable_path().get_base_dir()
	var tools_windows_path := executable_dir.path_join("tools").path_join("gddelta.exe")
	if(FileAccess.file_exists(tools_windows_path)):
		return tools_windows_path

	var tools_linux_path := executable_dir.path_join("tools").path_join("gddelta")
	if(FileAccess.file_exists(tools_linux_path)):
		return tools_linux_path
	return "";

func _update_runtime_status() -> void:
	var executable := gddelta_executable_path.strip_edges()
	if(executable.is_empty()):
		runtime_status_label.text = tr("RUNTIME_STATUS_MISSING")
		return

	if(FileAccess.file_exists(executable)):
		runtime_status_label.text = tr("RUNTIME_STATUS_READY") % executable
		return

	runtime_status_label.text = tr("RUNTIME_STATUS_MISSING_AT") % executable

func _prepare_gdre_tools() -> void:
	var executable := gddelta_executable_path.strip_edges()
	if(executable.is_empty()):
		_update_runtime_status()
		append_log(tr("FAILED_TO_FIND_GDDELTA_EXECUTABLE"))
		return

	if(!FileAccess.file_exists(executable)):
		_update_runtime_status()
		append_log(tr("FAILED_TO_FIND_GDDELTA_EXECUTABLE"))
		return

	var output: Array = []
	var exit_code := OS.execute(executable, PackedStringArray(["bootstrap"]), output, true, false)
	_update_runtime_status()
	if(exit_code != 0):
		append_log(tr("FAILED_TO_PREPARE_GDRE_TOOLS"))
		for line in output:
			append_log(str(line))

func _resolve_watch_log_path(sandbox_dir: String) -> String:
	var resolved_sandbox_dir := sandbox_dir.strip_edges()
	if(resolved_sandbox_dir.is_empty()):
		resolved_sandbox_dir = OS.get_user_data_dir()

	var log_file_path := resolved_sandbox_dir.path_join(".gddelta_watch.log")
	_ensure_watch_log_file(log_file_path)
	watch_log_path = log_file_path
	watch_log_position = 0
	watch_log_poll_accumulator = 0.0
	return log_file_path

func _resolve_command_log_path() -> String:
	var log_file_path := OS.get_user_data_dir().path_join(".gddelta_command.log")
	_ensure_watch_log_file(log_file_path)
	command_log_path = log_file_path
	command_log_position = 0
	command_log_poll_accumulator = 0.0
	return log_file_path

func _ensure_watch_log_file(path: String) -> void:
	var dir_path := path.get_base_dir()
	if(!dir_path.is_empty()):
		DirAccess.make_dir_recursive_absolute(dir_path)

	var file := FileAccess.open(path, FileAccess.WRITE)
	if(file != null): file.store_string("")

func _reset_watch_log_state() -> void:
	watch_log_path = ""
	watch_log_position = 0
	watch_log_poll_accumulator = 0.0

func _reset_command_log_state() -> void:
	command_log_path = ""
	command_log_position = 0
	command_log_poll_accumulator = 0.0

func _poll_command_log(delta: float) -> void:
	if(!command_running): return;
	if(command_log_path.is_empty() || !FileAccess.file_exists(command_log_path)): return;

	command_log_poll_accumulator += delta
	if(command_log_poll_accumulator < 0.1): return;

	command_log_poll_accumulator = 0.0
	_poll_log_file(command_log_path, "command")

func _poll_watch_log() -> void:
	if(watch_log_path.is_empty() || !FileAccess.file_exists(watch_log_path)): return;
	_poll_log_file(watch_log_path, "watch")

func _poll_log_file(path: String, kind: String) -> void:
	var file := FileAccess.open(path, FileAccess.READ)
	if(file == null): return;

	var file_length := file.get_length()
	var position := watch_log_position if kind == "watch" else command_log_position
	if(position > file_length):
		position = 0
	if(position == file_length): return;

	file.seek(position)
	var chunk := file.get_buffer(file_length - position).get_string_from_utf8()
	position = file.get_position()
	if(kind == "watch"):
		watch_log_position = position
	else:
		command_log_position = position
	for line in chunk.split("\n", false):
		append_log(line.rstrip("\r"))

func _resolve_sandbox_executable(base_path: String, sandbox_path: String) -> String:
	var resolved_sandbox_path := sandbox_path.strip_edges()
	if(resolved_sandbox_path.is_empty()): return "";

	var resolved_base_path := base_path.strip_edges()
	if(!resolved_base_path.is_empty()):
		var base_file_name := resolved_base_path.get_file()
		var base_extension := resolved_base_path.get_extension().to_lower()
		if(base_extension == "exe"):
			var direct_candidate := resolved_sandbox_path.path_join(base_file_name)
			if(FileAccess.file_exists(direct_candidate)):
				return direct_candidate
		elif(!base_file_name.is_empty()):
			var stem_candidate := resolved_sandbox_path.path_join(resolved_base_path.get_basename().get_file() + ".exe")
			if(FileAccess.file_exists(stem_candidate)):
				return stem_candidate

	var sandbox_files := DirAccess.get_files_at(resolved_sandbox_path)
	for file_name in sandbox_files:
		if(file_name.get_extension().to_lower() == "exe"):
			return resolved_sandbox_path.path_join(file_name)

	return ""

func _emit_watch_state_changed() -> void:
	watch_state_changed.emit(watch_pid != -1)

func _run_gddelta_worker(executable: String, args: PackedStringArray) -> Dictionary:
	var output: Array = []
	var exit_code := OS.execute(executable, args, output, true, false)
	return {
		"exit_code": exit_code,
	}

func _poll_command_thread() -> void:
	if(command_thread == null || !command_running): return;
	if(command_thread.is_alive()): return;

	var result: Variant = command_thread.wait_to_finish()
	command_thread = null
	command_running = false
	_poll_log_file(command_log_path, "command")
	_reset_command_log_state()
	_set_command_controls_enabled(true)

	var exit_code := ERR_BUG
	if(result is Dictionary):
		exit_code = int(result.get("exit_code", ERR_BUG))

	append_log(tr("EXIT_CODE") % exit_code)

	var completion := command_completion
	command_completion = Callable()
	if(completion.is_valid()):
		completion.call(exit_code)

func _set_command_controls_enabled(enabled: bool) -> void:
	advanced_toggle.disabled = !enabled
	_set_control_tree_enabled(tabs, enabled)

	var tab_bar := tabs.get_tab_bar()
	if(tab_bar != null):
		for tab_index in tabs.get_tab_count():
			tab_bar.set_tab_disabled(tab_index, !enabled)

func _set_control_tree_enabled(node: Node, enabled: bool) -> void:
	for child in node.get_children():
		if(child is Button):
			child.disabled = !enabled
		elif(child is CheckBox):
			child.disabled = !enabled
		elif(child is LineEdit):
			child.editable = enabled

		_set_control_tree_enabled(child, enabled)
