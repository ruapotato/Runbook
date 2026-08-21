# files.gd — a file browser onto the machine's own disk.
#
# NOT A COPY OF IT. Every listing and every byte shown here comes back from
# `ls` and `cat` run on the emulated machine, through the same shell the
# terminal uses. Two opinions about what is on a disk is one opinion too many,
# and keeping a mirror in the client is exactly the mistake NOMINAL's
# model/view rule exists to stop -- the moment a script writes a file, this
# window is already right, because it never knew anything of its own.
#
# It exists because "the filesystem on the computer we are on seems to not
# exist" was a fair thing to say about a desktop with no way to see one.
extends Control

const UiFont := preload("res://scripts/uifont.gd")
const Themes := preload("res://scripts/theme.gd")
const Icons  := preload("res://scripts/icons.gd")
const Clip   := preload("res://scripts/clip.gd")

var api: RunbookApi
var mono: Font
var th: Dictionary

var cwd := "/root"
var entries: Array = []          # [{name, kind, size}]
var scroll := 0
var viewing := ""                # a file being shown, "" for the listing
var content: PackedStringArray = []
var note := ""
# Set by the desktop: how to open a script for editing. A file browser that
# knew how to build an editor window would be a file browser that knew about
# windows.
var on_edit: Callable = func(_p: String) -> void: pass

func setup(a: RunbookApi) -> void:
	api = a
	mono = UiFont.mono()
	th = Themes.of("plain")
	refresh()

func refresh() -> void:
	entries.clear()
	var listing := api.sh("ls " + cwd)
	for raw in listing.split("\n"):
		var line := str(raw)
		var f := line.split(" ", false)
		# `ls` prints `d0755  <size>  name`. Anything else is an error message,
		# and showing it is better than showing an empty folder.
		if f.size() < 3 or str(f[0]).length() != 5:
			if line.strip_edges() != "" and note == "":
				note = line.strip_edges()
			continue
		entries.append({"name": str(f[2]), "kind": str(f[0])[0], "size": str(f[1])})
	entries.sort_custom(func(a2, b2):
		if a2["kind"] == b2["kind"]:
			return str(a2["name"]) < str(b2["name"])
		return a2["kind"] == "d")
	queue_redraw()

func _row_h() -> float: return 17.0

func _open(name: String) -> void:
	var path := cwd + ("" if cwd.ends_with("/") else "/") + name
	for raw in entries:
		var e: Dictionary = raw
		if str(e["name"]) != name:
			continue
		if str(e["kind"]) == "d":
			cwd = path
			scroll = 0
			viewing = ""
			note = ""
			refresh()
		elif name.ends_with(".py") or name.ends_with(".sh"):
			# A SCRIPT OPENS IN THE EDITOR, not the viewer. Double-clicking a
			# program and getting a read-only page of it is the behaviour of a
			# file manager that does not believe you are going to change
			# anything.
			if on_edit.is_valid():
				on_edit.call(path)
		else:
			# `cat`, on the machine. A file the machine cannot read shows the
			# machine's own complaint, which is the useful thing to see.
			viewing = path
			content = api.sh("cat " + path).split("\n")
			scroll = 0
		queue_redraw()
		return

func _up() -> void:
	if cwd == "/":
		return
	var cut := cwd.rfind("/")
	cwd = "/" if cut <= 0 else cwd.substr(0, cut)
	scroll = 0
	viewing = ""
	refresh()

func _buttons() -> Array:
	var out := [{"rect": Rect2(6, 26, 34, 19), "label": "up"},
				{"rect": Rect2(44, 26, 58, 19), "label": "refresh"}]
	if viewing != "":
		out.append({"rect": Rect2(106, 26, 54, 19), "label": "close"})
	return out

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), th["bg"])
	draw_rect(Rect2(0, 0, size.x, 24), th["panel"])
	draw_line(Vector2(0, 24), Vector2(size.x, 24), th["edge"], 1.0)
	draw_string(mono, Vector2(8, 17), viewing if viewing != "" else cwd,
		HORIZONTAL_ALIGNMENT_LEFT, size.x - 16, 12, th["ink"])

	for raw in _buttons():
		var b: Dictionary = raw
		var r: Rect2 = b["rect"]
		draw_rect(r, th["panel"])
		draw_rect(r, th["edge"], false, 1.0)
		draw_string(mono, Vector2(r.position.x + 6, r.position.y + 14), str(b["label"]),
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, th["ink"])

	if viewing != "":
		_draw_file()
		return

	var y := 62.0
	var i := scroll
	while i < entries.size() and y < size.y - 4:
		var e: Dictionary = entries[i]
		var is_dir := str(e["kind"]) == "d"
		Icons.draw_icon(self, Vector2(8, y - 11), 13.0, "files" if is_dir else "notes")
		draw_string(mono, Vector2(26, y), str(e["name"]),
			HORIZONTAL_ALIGNMENT_LEFT, size.x - 110, 12,
			th["accent"] if is_dir else th["ink"])
		if not is_dir:
			draw_string(mono, Vector2(size.x - 10, y), str(e["size"]),
				HORIZONTAL_ALIGNMENT_RIGHT, 80, 10, th["dim"])
		y += _row_h()
		i += 1
	if entries.is_empty():
		draw_string(mono, Vector2(10, 62), note if note != "" else "(empty)",
			HORIZONTAL_ALIGNMENT_LEFT, size.x - 20, 11, th["dim"])

func _draw_file() -> void:
	var y := 60.0
	var i := scroll
	while i < content.size() and y < size.y - 4:
		draw_string(mono, Vector2(8, y), str(content[i]),
			HORIZONTAL_ALIGNMENT_LEFT, size.x - 16, 11, th["ink"])
		y += 14.0
		i += 1

func selected_text() -> String:
	return viewing if viewing != "" else cwd

func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and e.pressed:
		if e.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			scroll = mini(scroll + 3, maxi(0, (content.size() if viewing != "" else entries.size()) - 3))
			queue_redraw(); return
		if e.button_index == MOUSE_BUTTON_WHEEL_UP:
			scroll = maxi(scroll - 3, 0); queue_redraw(); return
		if e.button_index != MOUSE_BUTTON_LEFT:
			return
		for raw in _buttons():
			var b: Dictionary = raw
			if (b["rect"] as Rect2).has_point(e.position):
				match str(b["label"]):
					"up": _up()
					"refresh": refresh()
					"close": viewing = ""; scroll = 0; queue_redraw()
				return
		if viewing == "" and e.position.y > 50.0:
			var idx := scroll + int((e.position.y - 51.0) / _row_h())
			if idx >= 0 and idx < entries.size():
				_open(str((entries[idx] as Dictionary)["name"]))
