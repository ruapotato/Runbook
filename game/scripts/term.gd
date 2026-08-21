# term.gd — the terminal, which is the same API the buttons use.
#
# This is where Act II starts, and it starts as a DISCOVERY rather than an
# unlock: the terminal has been in the launcher menu since the first morning,
# and everything typed into it goes through the identical proto_exec() that
# every form submission goes through. The player finds the API; it was never
# hidden and it was never announced.
#
# `help` is the whole of the documentation, and --health asserts that every
# verb it advertises actually dispatches, so this window cannot lie about what
# the game can do.
extends Control

const UiFont := preload("res://scripts/uifont.gd")
const Themes := preload("res://scripts/theme.gd")

var api: RunbookApi
var mono: Font
var th: Dictionary
var lines: Array = []
var input := ""
var history: Array = []
var hist_at := -1
var scroll := 0

func setup(a: RunbookApi) -> void:
	api = a
	mono = UiFont.mono()
	th = Themes.of("plain")
	th = th.duplicate()
	th["bg"] = Color("#101418")
	th["ink"] = Color("#d6e2ea")
	th["dim"] = Color("#7d8f9c")
	th["accent"] = Color("#7fd0a0")
	th["bad"] = Color("#e08a7a")
	_say("RUNBOOK/1 — this is the same API the forms use.")
	_say("Type 'help'. Every response ends with a lone '.'")
	_say("")

func _say(s: String) -> void:
	for l in s.split("\n"):
		lines.append(str(l))
	while lines.size() > 800:
		lines.pop_front()

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), th["bg"])
	var row := 14.0
	var visible := int((size.y - 22.0) / row)
	var first: int = maxi(0, lines.size() - visible - scroll)
	var y := 14.0
	for i in range(first, mini(lines.size(), first + visible)):
		var l := str(lines[i])
		var col: Color = th["ink"]
		if l.begins_with("-ERR"): col = th["bad"]
		elif l.begins_with("+OK"): col = th["accent"]
		elif l.begins_with("$ "):  col = th["dim"]
		draw_string(mono, Vector2(6, y), l, HORIZONTAL_ALIGNMENT_LEFT, size.x - 12, 11, col)
		y += row
	draw_line(Vector2(0, size.y - 20), Vector2(size.x, size.y - 20), th["dim"], 1.0)
	draw_string(mono, Vector2(6, size.y - 6), "$ " + input + "_",
		HORIZONTAL_ALIGNMENT_LEFT, size.x - 12, 11, th["ink"])

func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and e.pressed:
		if e.button_index == MOUSE_BUTTON_WHEEL_UP:
			scroll = mini(scroll + 3, maxi(0, lines.size() - 4)); queue_redraw()
		elif e.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			scroll = maxi(scroll - 3, 0); queue_redraw()
		else:
			grab_focus()
		return
	if not (e is InputEventKey and e.pressed):
		return
	var k := e as InputEventKey
	match k.keycode:
		KEY_ENTER, KEY_KP_ENTER:
			_run()
		KEY_BACKSPACE:
			if input.length() > 0:
				input = input.substr(0, input.length() - 1)
		KEY_UP:
			if history.size() > 0:
				hist_at = maxi(0, (history.size() - 1) if hist_at < 0 else hist_at - 1)
				input = str(history[hist_at])
		KEY_DOWN:
			if hist_at >= 0 and hist_at < history.size() - 1:
				hist_at += 1
				input = str(history[hist_at])
			else:
				hist_at = -1
				input = ""
		_:
			var ch := char(k.unicode)
			if k.unicode >= 32:
				input += ch
	accept_event()
	queue_redraw()

func _run() -> void:
	var line := input.strip_edges()
	input = ""
	scroll = 0
	hist_at = -1
	if line == "":
		return
	history.append(line)
	_say("$ " + line)
	if line == "clear":
		lines.clear()
		return
	_say(api.exec(line))
	queue_redraw()
