# editor.gd — a text editor for files on the machine.
#
# THIS IS WHERE THE RECORDER HANDS OFF. Decision 15 calls the macro recorder
# the on-ramp to scripting, and an on-ramp that ends at a file you cannot open
# is a cliff with extra steps. The player records a job, gets a script about
# work they just did, and it opens HERE, ready to change -- with a Run button
# next to it so the first edit they make gets an answer in a second.
#
# It edits the machine's real files through the machine's own shell, so a file
# saved here is a file `ls` can see and `py` can run. There is no separate
# notion of "the game's scripts" anywhere.
#
# It is a Godot TextEdit and not a hand-drawn one, because a text box is the
# one control where everybody's hands already know what to do -- selection,
# arrow keys, home and end, undo. Writing that from scratch to keep the
# aesthetic would be trading something that works for something that matches.
extends Control

const UiFont := preload("res://scripts/uifont.gd")
const Themes := preload("res://scripts/theme.gd")

var api: RunbookApi
var mono: Font
var th: Dictionary
var path := ""
var note := ""
var note_good := true
var out_lines: PackedStringArray = []
var edit: TextEdit

func setup(a: RunbookApi, file_path: String, initial: String = "") -> void:
	api = a
	mono = UiFont.mono()
	th = Themes.of("plain")
	path = file_path

	edit = TextEdit.new()
	edit.add_theme_font_override("font", mono)
	edit.add_theme_font_size_override("font_size", 12)
	edit.add_theme_color_override("font_color", th["ink"])
	edit.add_theme_color_override("caret_color", th["ink"])
	var sb := StyleBoxFlat.new()
	sb.bg_color = th["field"]
	sb.border_color = th["edge"]
	sb.set_border_width_all(1)
	sb.content_margin_left = 6.0
	sb.content_margin_top = 4.0
	edit.add_theme_stylebox_override("normal", sb)
	edit.add_theme_stylebox_override("focus", sb)
	add_child(edit)

	if initial != "":
		edit.text = initial
		note = "not saved yet — press Save"
		note_good = false
	else:
		var content := api.sh("cat " + path)
		if content.find("cannot read") >= 0:
			edit.text = "# " + path + "\n"
			note = "new file"
		else:
			edit.text = content
			note = "opened " + path
	_layout()

func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		_layout()

func _layout() -> void:
	if edit == null:
		return
	var bottom := 22.0 + (out_lines.size() * 14.0 if out_lines.size() > 0 else 0.0)
	edit.position = Vector2(6, 52)
	edit.size = Vector2(maxf(80.0, size.x - 12), maxf(60.0, size.y - 58 - bottom))

func _buttons() -> Array:
	return [
		{"rect": Rect2(6, 26, 46, 20), "label": "Save"},
		{"rect": Rect2(56, 26, 40, 20), "label": "Run"},
		{"rect": Rect2(100, 26, 62, 20), "label": "Reload"},
	]

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), th["bg"])
	draw_rect(Rect2(0, 0, size.x, 24), th["panel"])
	draw_line(Vector2(0, 24), Vector2(size.x, 24), th["edge"], 1.0)
	draw_string(mono, Vector2(8, 17), path, HORIZONTAL_ALIGNMENT_LEFT, size.x - 200, 12, th["ink"])
	draw_string(mono, Vector2(size.x - 8, 17), note, HORIZONTAL_ALIGNMENT_RIGHT,
		size.x * 0.5, 11, th["ok"] if note_good else th["bad"])

	for raw in _buttons():
		var b: Dictionary = raw
		var r: Rect2 = b["rect"]
		draw_rect(r, th["accent"] if str(b["label"]) == "Run" else th["panel"])
		draw_rect(r, th["edge"], false, 1.0)
		draw_string(mono, Vector2(r.position.x + 8, r.position.y + 14), str(b["label"]),
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12,
			Color.WHITE if str(b["label"]) == "Run" else th["ink"])

	# What it printed, under the text. A Run button whose output you have to go
	# and find in another window is a Run button nobody presses twice.
	var y := size.y - out_lines.size() * 14.0 - 4.0
	for raw in out_lines:
		var l := str(raw)
		draw_string(mono, Vector2(8, y), l, HORIZONTAL_ALIGNMENT_LEFT, size.x - 16, 11,
			th["bad"] if l.find("error") >= 0 or l.find("FAIL") >= 0 else th["dim"])
		y += 14.0

func _gui_input(e: InputEvent) -> void:
	if not (e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT):
		return
	for raw in _buttons():
		var b: Dictionary = raw
		if (b["rect"] as Rect2).has_point(e.position):
			match str(b["label"]):
				"Save": _save()
				"Run": _run()
				"Reload": setup(api, path)
			queue_redraw()
			return

# SAVED THROUGH THE MACHINE, a line at a time, because that is the only way
# in: there is no host-side file write for the client to call, and there
# should not be one. `ed` is on the disk and would do this in one command, but
# driving a line editor from a GUI is exactly the sort of cleverness that
# breaks on the first apostrophe.
func _save() -> void:
	var first := true
	for raw in edit.text.split("\n"):
		var l := str(raw).replace("'", "")
		var op := ">" if first else ">>"
		api.sh("echo '" + l + "' " + op + " " + path)
		first = false
	note = "saved"
	note_good = true
	out_lines = PackedStringArray()
	_layout()

func _run() -> void:
	_save()
	var cmd := ("py " if path.ends_with(".py") else "sh ") + path
	var result := api.sh(cmd)
	out_lines = PackedStringArray()
	for raw in result.split("\n"):
		var l := str(raw).strip_edges()
		if l != "":
			out_lines.append(l)
	# The last handful, so a long run does not push the editor off the window.
	while out_lines.size() > 8:
		out_lines.remove_at(0)
	note = "ran " + cmd
	note_good = result.find("error") < 0
	_layout()

func selected_text() -> String:
	return edit.get_selected_text() if edit != null else ""
