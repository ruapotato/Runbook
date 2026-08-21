# webui.gd — an appliance's web interface, generated from its spec.
#
# Handoff decision 6: appliances are declarative specs, not scenes. There is
# no hand-built screen for the directory and another for the mail server.
# There is THIS, once, and it renders whatever `appl.forms` and
# `appl.endpoints` came back with -- so a new appliance is a YAML file and
# nothing else, which is what makes M8's content scale and what lets an agent
# author one.
#
# THE UI IS GOOD, AND IT NEVER BECOMES BAD (§5). It becomes INSUFFICIENT. Every
# form here submits in one click, tells you plainly what happened, and costs
# two in-game minutes. At five tickets a day that is a pleasant afternoon; at
# forty it is the whole day and then some, and the arithmetic -- not the
# interface -- is what pushes the player towards a script. If a playtester
# calls Act I tedious, Act I is too LONG, not too slow.
extends Control

const Themes := preload("res://scripts/theme.gd")
const UiFont := preload("res://scripts/uifont.gd")

var api: RunbookApi
var inst := ""
var model := ""
var vendor := ""
var th: Dictionary = {}
var mono: Font

var forms: Array = []          # from appl.forms
var endpoints: Array = []      # from appl.endpoints
var tab := 0                   # which form, or -1..-n for browse views
var browse: Array = []         # list endpoints, for the Records tabs

var edits: Array = []          # LineEdits for the current form
var result_line := ""
var result_good := false
var rows: Array = []           # what a browse view last fetched
var busy := false

func setup(a: RunbookApi, instance: String) -> void:
	api = a
	inst = instance
	mono = UiFont.mono()

	var info := api.exec("appl.info %s" % inst)
	var o := api.objects(info)
	if o.size() > 0:
		model = str(o[0].get("model", inst))
		vendor = str(o[0].get("vendor", ""))
		th = Themes.of(str(o[0].get("theme", "plain")))
	else:
		th = Themes.of("plain")

	forms = api.objects(api.exec("appl.forms %s" % inst))
	endpoints = api.objects(api.exec("appl.endpoints %s" % inst))
	for raw in endpoints:
		var e: Dictionary = raw
		if str(e.get("op", "")) == "list":
			browse.append(e)
	_build()

func _clear_edits() -> void:
	for raw in edits:
		var e: LineEdit = raw
		if is_instance_valid(e):
			e.queue_free()
	edits.clear()

func _build() -> void:
	_clear_edits()
	result_line = ""
	rows.clear()
	if tab >= 0 and tab < forms.size():
		var fields := api.list_of(forms[tab], "fields")
		for i in range(fields.size()):
			var le := LineEdit.new()
			le.placeholder_text = str(fields[i])
			le.set_meta("field", str(fields[i]))
			# THE VENDOR'S LOOK GOES ALL THE WAY DOWN, including into the
			# engine's own controls. A Godot LineEdit is unmistakably a Godot
			# LineEdit, and one of those on every appliance is exactly how the
			# vendor-quality mechanic dies quietly (§14).
			le.add_theme_color_override("font_color", th["ink"])
			le.add_theme_color_override("font_placeholder_color", th["dim"])
			le.add_theme_color_override("caret_color", th["ink"])
			var sb := StyleBoxFlat.new()
			sb.bg_color = th["field"]
			sb.border_color = th["edge"]
			sb.set_border_width_all(1)
			sb.content_margin_left = 6.0
			le.add_theme_stylebox_override("normal", sb)
			var sf := sb.duplicate()
			sf.border_color = th["accent"]
			sf.set_border_width_all(2)
			le.add_theme_stylebox_override("focus", sf)
			add_child(le)
			edits.append(le)
	_layout()
	queue_redraw()

func _layout() -> void:
	var pad: float = th.get("pad", 8.0)
	var row: float = th.get("row", 24.0)
	var top := 58.0
	for i in range(edits.size()):
		var le: LineEdit = edits[i]
		le.position = Vector2(pad + 130.0, top + i * (row + 6.0))
		le.size = Vector2(maxf(160.0, size.x - pad * 2.0 - 140.0), row)

func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		_layout()

func _tab_rects() -> Array:
	var out := []
	var x := 4.0
	var n := forms.size() + browse.size()
	for i in range(n):
		var label := _tab_label(i)
		var w := mono.get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x + 16.0
		out.append(Rect2(x, 26.0, w, 20.0))
		x += w + 2.0
	return out

func _tab_label(i: int) -> String:
	if i < forms.size():
		return str(forms[i].get("title", forms[i].get("id", "form")))
	var e: Dictionary = browse[i - forms.size()]
	return str(e.get("collection", "records"))

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), th["bg"])

	# The chrome bar: which appliance this is, and whose it is. Naming the
	# vendor on every screen is what makes "I am never buying Halcyon again" a
	# thought a player can have.
	draw_rect(Rect2(0, 0, size.x, 24), th["panel"])
	draw_line(Vector2(0, 24), Vector2(size.x, 24), th["edge"], 1.0)
	draw_string(mono, Vector2(8, 17), "%s — %s" % [model, inst],
		HORIZONTAL_ALIGNMENT_LEFT, size.x - 200, 12, th["ink"])
	draw_string(mono, Vector2(size.x - 8, 17), vendor,
		HORIZONTAL_ALIGNMENT_RIGHT, size.x - 200, 11, th["dim"])

	var tabs := _tab_rects()
	for i in range(tabs.size()):
		var r: Rect2 = tabs[i]
		var on := i == tab or (tab < 0 and i - forms.size() == -tab - 1)
		draw_rect(r, th["field"] if on else th["panel"])
		draw_rect(r, th["edge"], false, 1.0)
		draw_string(mono, Vector2(r.position.x + 8, r.position.y + 14), _tab_label(i),
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12, th["ink"] if on else th["dim"])

	if tab >= 0 and tab < forms.size():
		_draw_form()
	else:
		_draw_browse()

	if result_line != "":
		var y := size.y - 22.0
		draw_rect(Rect2(0, y - 4, size.x, 26), th["panel"])
		draw_string(mono, Vector2(8, y + 12), result_line, HORIZONTAL_ALIGNMENT_LEFT,
			size.x - 16, 12, th["ok"] if result_good else th["bad"])

func _draw_form() -> void:
	var pad: float = th.get("pad", 8.0)
	var row: float = th.get("row", 24.0)
	var top := 58.0
	for i in range(edits.size()):
		var le: LineEdit = edits[i]
		draw_string(mono, Vector2(pad, top + i * (row + 6.0) + row * 0.7),
			str(le.get_meta("field")), HORIZONTAL_ALIGNMENT_LEFT, 124, 12, th["ink"])
	var b := _submit_rect()
	draw_rect(b, th["accent"])
	draw_string(mono, Vector2(b.position.x + 12, b.position.y + 15), "Submit",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color.WHITE)
	draw_string(mono, Vector2(b.position.x + b.size.x + 10, b.position.y + 15),
		"2 minutes", HORIZONTAL_ALIGNMENT_LEFT, -1, 11, th["dim"])

func _submit_rect() -> Rect2:
	var pad: float = th.get("pad", 8.0)
	var row: float = th.get("row", 24.0)
	return Rect2(pad, 58.0 + edits.size() * (row + 6.0) + 8.0, 74, 22)

func _draw_browse() -> void:
	var pad: float = th.get("pad", 8.0)
	var row: float = th.get("row", 24.0)
	var b := _refresh_rect()
	draw_rect(b, th["accent"])
	draw_string(mono, Vector2(b.position.x + 10, b.position.y + 15), "Refresh",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color.WHITE)
	var y := 84.0
	for raw in rows:
		var r: Dictionary = raw
		if y > size.y - 24.0:
			draw_string(mono, Vector2(pad, y), "… more (this is why you will want the API)",
				HORIZONTAL_ALIGNMENT_LEFT, size.x, 11, th["dim"])
			break
		var parts := PackedStringArray()
		for k in r.keys():
			parts.append("%s=%s" % [k, r[k]])
		draw_string(mono, Vector2(pad, y), "  ".join(parts),
			HORIZONTAL_ALIGNMENT_LEFT, size.x - pad * 2, 11, th["ink"])
		y += row * 0.7

func _refresh_rect() -> Rect2:
	return Rect2(th.get("pad", 8.0), 54.0, 78, 22)

func _gui_input(e: InputEvent) -> void:
	if not (e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT):
		return
	var tabs := _tab_rects()
	for i in range(tabs.size()):
		if (tabs[i] as Rect2).has_point(e.position):
			tab = i if i < forms.size() else -(i - forms.size()) - 1
			_build()
			return
	if tab >= 0 and _submit_rect().has_point(e.position):
		_submit()
	elif tab < 0 and _refresh_rect().has_point(e.position):
		_refresh()

func _submit() -> void:
	var f: Dictionary = forms[tab]
	var line := "form.submit %s %s" % [inst, str(f.get("id", ""))]
	for raw in edits:
		var le: LineEdit = raw
		var v := str(le.text).strip_edges()
		if v != "":
			# Spaces would split the argument. The protocol takes identifiers
			# because tickets are structured objects, not prose (decision 5),
			# so a space here is a typo and saying so beats mangling it.
			if v.find(" ") >= 0:
				result_line = "%s cannot contain a space" % str(le.get_meta("field"))
				result_good = false
				queue_redraw()
				return
			line += " %s=%s" % [str(le.get_meta("field")), v]
	var resp := api.exec(line)
	result_good = api.ok(resp)
	if result_good:
		var body := api.body_lines(resp)
		result_line = "done — %s" % (body[0] if body.size() > 0 else "ok")
	else:
		var why := api.error_text(resp)
		result_line = "refused: %s" % (why if why != "" else resp.get_slice("\n", 0))
	queue_redraw()

func _refresh() -> void:
	var e: Dictionary = browse[-tab - 1]
	var resp := api.exec("api.call %s %s" % [inst, str(e.get("id", ""))])
	rows = api.records(resp)
	if rows.is_empty() and not api.ok(resp):
		result_line = api.error_text(resp)
		result_good = false
	queue_redraw()
