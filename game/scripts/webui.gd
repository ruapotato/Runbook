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
const Clip   := preload("res://scripts/clip.gd")

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

var edits: Array = []          # one control per field: a box, or a list
var field_types: Dictionary = {}   # collection -> {field -> {type, of, values}}
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

# WHAT A FIELD IS, ASKED ONCE PER COLLECTION AND REMEMBERED.
func _types_for(coll: String) -> Dictionary:
	if field_types.has(coll):
		return field_types[coll]
	var d := {}
	for raw in api.objects(api.exec("appl.fields %s %s" % [inst, coll])):
		var f: Dictionary = raw
		d[str(f.get("name", ""))] = f
	field_types[coll] = d
	return d

func _endpoint_of(form: Dictionary) -> Dictionary:
	for raw in endpoints:
		var e: Dictionary = raw
		if str(e.get("id", "")) == str(form.get("calls", "")):
			return e
	return {}

# WHAT GOES IN A LIST, asked of the world rather than guessed.
#
# `of: users` and `of: departments` are the org's, not this appliance's, so
# they come from the game; anything else is one of this appliance's own
# collections and comes from its list endpoint. Either way the client is
# reading it out of the API, which means a picker can never offer something
# that is not there -- the same rule the terminal's tab completion follows.
func _options_for(f: Dictionary, coll: String, is_key: bool) -> Array:
	var out := []
	var of := str(f.get("of", ""))
	var ftype := str(f.get("type", "text"))

	if ftype == "enum":
		return api.list_of(f, "values")
	if is_key or ftype == "ref":
		var want := of
		if is_key:
			want = coll
		if want == "departments":
			for raw in api.body_lines(api.exec("depts")):
				out.append(str(raw))
			return out
		if want == "users":
			# A NAME FIRST, AND ONLY THE PEOPLE WHO STILL NEED ONE.
			#
			# "user ref seems like a big list of random names, not really sure
			# what that means" -- because it was every person in the company,
			# id first, in creation order. What a user_ref actually means on
			# a New Account form is "which of the people who work here is this
			# account for", and the only ones that can be true of are the ones
			# who do not have an account yet. So: those, by name, with the id
			# after it because the id is what the API wants and the player
			# will need to recognise it in a ticket.
			var have := {}
			for raw in api.records(api.exec("api.call %s list_accounts" % inst)):
				var a: Dictionary = raw
				have[str(a.get("user_ref", ""))] = true
			var unprovisioned := []
			var everyone := []
			for raw in api.objects(api.exec("user.list")):
				var u: Dictionary = raw
				var label := "%s %s  (%s, %s)" % [u.get("given", ""), u.get("family", ""),
												  u.get("id", ""), u.get("dept", "")]
				everyone.append(label)
				if not have.has(str(u.get("id", ""))):
					unprovisioned.append(label)
			# If everybody has an account there is nothing waiting, and an
			# empty list would be worse than a long one.
			return unprovisioned if not unprovisioned.is_empty() else everyone
		# One of this appliance's collections: find the list endpoint that
		# serves it and read the key field off every record.
		for raw in endpoints:
			var e: Dictionary = raw
			if str(e.get("op", "")) != "list" or str(e.get("collection", "")) != want:
				continue
			var keyname := _key_of(want)
			for rec in api.records(api.exec("api.call %s %s" % [inst, str(e.get("id", ""))])):
				var r: Dictionary = rec
				if r.has(keyname):
					out.append(str(r[keyname]))
			break
	return out

func _key_of(coll: String) -> String:
	# The first field of a collection whose type is "key" IS its key.
	for k in _types_for(coll).keys():
		var f: Dictionary = _types_for(coll)[k]
		if str(f.get("type", "")) == "key":
			return str(k)
	return "name"

func _clear_edits() -> void:
	for raw in edits:
		var e: Control = raw
		if is_instance_valid(e):
			e.queue_free()
	edits.clear()

func _build() -> void:
	_clear_edits()
	result_line = ""
	rows.clear()
	# A BROWSE TAB FILLS ITSELF. Clicking "shares" and being shown an empty
	# box with a Refresh button is the appliance asking the operator to do its
	# job for it. The API call costs in-game time either way -- the difference
	# is only whether the player had to ask twice.
	if tab < 0:
		_refresh()
	if tab >= 0 and tab < forms.size():
		var form: Dictionary = forms[tab]
		var ep := _endpoint_of(form)
		var coll := str(ep.get("collection", ""))
		var op := str(ep.get("op", ""))
		var types := _types_for(coll)
		var fields := api.list_of(form, "fields")
		for i in range(fields.size()):
			var fname := str(fields[i])
			var f: Dictionary = types.get(fname, {})
			var ftype := str(f.get("type", "text"))
			# THE FIX A PLAYTEST ASKED FOR. A key field on an endpoint that
			# edits or deletes something existing is a CHOICE among the things
			# that exist -- "Edit account" cannot ask you to remember a login.
			# On a create it is still a box, because the whole point of a
			# create is that the thing is not there yet.
			var is_key := ftype == "key" and op != "create"
			if ftype == "enum" or ftype == "ref" or is_key:
				var opts := _options_for(f, coll, is_key)
				if not opts.is_empty():
					var ob := OptionButton.new()
					ob.set_meta("field", fname)
					ob.set_meta("picker", true)
					ob.add_item("— %s —" % fname)
					for o in opts:
						ob.add_item(str(o))
					_style_picker(ob)
					# PICK THE PERSON AND THE REST FOLLOWS.
					#
					# "I don't see the point of a ref user, if we have to enter
					# the dept anyway" -- which is right, and the answer is not
					# to remove the field. A department is a fact ABOUT the
					# person; asking for both is asking the operator to look
					# something up that the form is already holding. So
					# choosing a user fills in their department, their name,
					# and the login the convention gives them.
					#
					# It is also the first place the game says out loud that
					# these things are connected, which is the idea the whole
					# job rests on.
					if ftype == "ref" and str(f.get("of", "")) == "users":
						ob.item_selected.connect(func(_i): _user_picked(ob))
					add_child(ob)
					edits.append(ob)
					continue
			var le := LineEdit.new()
			le.placeholder_text = fname
			le.set_meta("field", fname)
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
			# X11 SELECTION, IN A FORM FIELD TOO.
			#
			# Godot's LineEdit already does Ctrl-C and Ctrl-V against the
			# system clipboard. What it has never done is PRIMARY: select some
			# text and it is immediately pastable with the middle button,
			# without a keystroke, into any other field or the terminal. On a
			# desktop that claims to be a desktop, that is not a nicety --
			# copying a login out of one appliance and into another is most of
			# what this job is.
			le.text_change_rejected.connect(func(_r): pass)
			le.gui_input.connect(func(ev): _field_input(le, ev))
			add_child(le)
			edits.append(le)
	_layout()
	queue_redraw()

# Selection out, middle-click in. The LineEdit keeps everything else it does.
func _field_input(le: LineEdit, e: InputEvent) -> void:
	if e is InputEventMouseButton:
		var mb := e as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_MIDDLE and mb.pressed:
			var s := Clip.get_primary()
			if s != "":
				# One line only: a form field is one line, and pasting a
				# transcript into it would be neither useful nor honest.
				le.insert_text_at_caret(s.get_slice("\n", 0))
			le.accept_event()
			return
		if mb.button_index == MOUSE_BUTTON_LEFT and not mb.pressed:
			# The selection is whatever it is once the button comes up.
			var sel := le.get_selected_text()
			if sel != "":
				Clip.set_primary(sel)

func _field(name: String) -> Control:
	for raw in edits:
		var c: Control = raw
		if str(c.get_meta("field")) == name:
			return c
	return null

func _set_field(name: String, value: String) -> void:
	var c := _field(name)
	if c == null:
		return
	if bool(c.get_meta("picker", false)):
		var ob: OptionButton = c
		for i in range(ob.item_count):
			if ob.get_item_text(i) == value:
				ob.select(i)
				return
	else:
		var le: LineEdit = c
		if str(le.text).strip_edges() == "":
			le.text = value

# THE ORG'S NAMING CONVENTION, applied here so the form can suggest a login.
#
# It is a SUGGESTION and the box stays editable, because the directory may
# already have it -- and finding that out is the player's job, which is the
# whole of exception class 1. Filling it in is help; filling it in and
# refusing to let go would be doing the interesting part for them.
func _convention(given: String, family: String) -> String:
	var out := ""
	if given.length() > 0:
		out = given.substr(0, 1).to_lower()
	for i in range(family.length()):
		var c := family[i]
		if c.to_lower() != c.to_upper() or (c >= "0" and c <= "9"):
			out += c.to_lower()
	return out

func _user_picked(ob: OptionButton) -> void:
	if ob.selected <= 0:
		return
	# The label reads "Alma Barrow  (u_00041, sales)".
	var label := ob.get_item_text(ob.selected)
	var lp := label.find("(")
	if lp < 0:
		return
	var name := label.substr(0, lp).strip_edges()
	var id := label.substr(lp + 1).get_slice(",", 0).strip_edges()
	var u := api.objects(api.exec("user.get " + id))
	if u.is_empty():
		return
	var d: Dictionary = u[0]
	_set_field("display_name", name)
	_set_field("dept", str(d.get("dept", "")))
	_set_field("login", _convention(str(d.get("given", "")), str(d.get("family", ""))))
	queue_redraw()

func _style_picker(ob: OptionButton) -> void:
	ob.add_theme_color_override("font_color", th["ink"])
	ob.add_theme_color_override("font_hover_color", th["ink"])
	ob.add_theme_color_override("font_focus_color", th["ink"])
	var sb := StyleBoxFlat.new()
	sb.bg_color = th["field"]
	sb.border_color = th["edge"]
	sb.set_border_width_all(1)
	sb.content_margin_left = 6.0
	for st in ["normal", "hover", "pressed", "focus", "disabled"]:
		ob.add_theme_stylebox_override(st, sb)

	# THE POPUP TOO. Godot's PopupMenu is dark by default, so a bright
	# appliance opened a black menu -- "drop down menus are dark godot menus
	# on the bright UI, looks off". The list is part of the appliance and has
	# to be the appliance's colours; a vendor whose menus look like somebody
	# else's is a vendor whose theming has stopped meaning anything (§14).
	var pop := ob.get_popup()
	pop.add_theme_color_override("font_color", th["ink"])
	pop.add_theme_color_override("font_hover_color", Color.WHITE)
	pop.add_theme_color_override("font_separator_color", th["dim"])
	var pb := StyleBoxFlat.new()
	pb.bg_color = th["field"]
	pb.border_color = th["edge"]
	pb.set_border_width_all(1)
	pop.add_theme_stylebox_override("panel", pb)
	var hb := StyleBoxFlat.new()
	hb.bg_color = th["accent"]
	pop.add_theme_stylebox_override("hover", hb)

func _layout() -> void:
	var pad: float = th.get("pad", 8.0)
	var row: float = th.get("row", 24.0)
	var top := 58.0
	for i in range(edits.size()):
		var c: Control = edits[i]
		c.position = Vector2(pad + 130.0, top + i * (row + 6.0))
		c.size = Vector2(maxf(160.0, size.x - pad * 2.0 - 140.0), row)

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
		var c: Control = edits[i]
		draw_string(mono, Vector2(pad, top + i * (row + 6.0) + row * 0.7),
			str(c.get_meta("field")), HORIZONTAL_ALIGNMENT_LEFT, 124, 12, th["ink"])
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
		var c: Control = raw
		var v := ""
		if bool(c.get_meta("picker", false)):
			var ob: OptionButton = c
			# Item 0 is the "— field —" placeholder, which means "left blank".
			if ob.selected > 0:
				v = ob.get_item_text(ob.selected)
				# A user picker shows "u_00041  Alma Barrow"; the API wants the
				# id. Everything the player needs to read, nothing the API has
				# to parse.
				# The label reads "Alma Barrow  (u_00041, sales)"; the API
				# wants the id. Everything the player needs to read, nothing
				# the API has to parse.
				var lp := v.find("(")
				if lp >= 0:
					v = v.substr(lp + 1).get_slice(",", 0).strip_edges()
				else:
					v = v.get_slice("  ", 0)
		else:
			var le: LineEdit = c
			v = str(le.text)
		v = v.strip_edges()
		if v != "":
			# QUOTED IF IT NEEDS TO BE. This used to refuse any value with a
			# space in it and say so as though the player had made a mistake,
			# which is how a display name of "Alma Barrow" -- that is to say,
			# a name -- became a validation error.
			if v.find(" ") >= 0:
				v = "\"%s\"" % v
			line += " %s=%s" % [str(c.get_meta("field")), v]
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
