# queue.gd — the ticket queue, and the acceptance checks.
#
# The most important window in the game, and the one that must never lie.
#
# THERE IS NO RESOLVE BUTTON, and there is not going to be one (decision 9).
# A ticket closes when the world says its acceptance checks are true. What
# this window offers instead is a Check button, which asks -- and asking is
# free, because knowing whether you did the job is the oracle, not a resource.
#
# EVERY FAILING CHECK SAYS WHY. The temptation to withhold the reason, to make
# the player "work it out", is the diagnosis-as-content trap that killed every
# earlier attempt in this lineage (§2). The difficulty here is volume. It is
# never the interface being coy.
extends Control

const UiFont := preload("res://scripts/uifont.gd")
const Themes := preload("res://scripts/theme.gd")

var api: RunbookApi
var mono: Font
var th: Dictionary

var tickets: Array = []
var selected := -1
var checks: Array = []          # [{mark, id, doc, why}]
var scroll := 0
var showing_closed := false

func setup(a: RunbookApi) -> void:
	api = a
	mono = UiFont.mono()
	th = Themes.of("veridian_slate")
	refresh()

func refresh() -> void:
	# Reading the queue settles it: anything whose work is done is already
	# closed by the time it reaches the screen.
	var resp := api.exec("ticket.list %s 200" % ("closed" if showing_closed else "open"))
	tickets = api.objects(resp)
	if selected >= tickets.size():
		selected = -1
	# NOT checks.clear(). Checking a ticket settles it, settling changes the
	# queue, and the queue is re-read afterwards -- so clearing here wiped the
	# verdict the player had just asked for, every single time, and the panel
	# went back to saying "Press Check". The verdict is cleared when the
	# SELECTION changes, which is the only time it stops being about the
	# ticket on screen.
	queue_redraw()

func _row_h() -> float: return 18.0

func _list_rect() -> Rect2:
	return Rect2(0, 46, size.x * 0.45, size.y - 46)

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), th["bg"])
	draw_rect(Rect2(0, 0, size.x, 24), th["panel"])
	draw_line(Vector2(0, 24), Vector2(size.x, 24), th["edge"], 1.0)

	var stats := api.objects(api.exec("ticket.stats"))
	var s: Dictionary = stats[0] if stats.size() > 0 else {}
	var head := "day %s   open %s   closed %s   within SLA %s%%" % [
		s.get("day", "?"), s.get("open", "?"), s.get("closed", "?"), s.get("within_sla_pct", "?")]
	draw_string(mono, Vector2(8, 17), head, HORIZONTAL_ALIGNMENT_LEFT, size.x - 16, 12, th["ink"])

	for raw in _buttons():
		var b: Dictionary = raw
		var br: Rect2 = b["rect"]
		draw_rect(br, th["accent"] if b.get("on", false) else th["panel"])
		draw_rect(br, th["edge"], false, 1.0)
		draw_string(mono, Vector2(br.position.x + 8, br.position.y + 15),
			str(b["label"]), HORIZONTAL_ALIGNMENT_LEFT, -1, 12,
			Color.WHITE if b.get("on", false) else th["ink"])

	var lr := _list_rect()
	draw_line(Vector2(lr.size.x, 46), Vector2(lr.size.x, size.y), th["edge"], 1.0)
	var y := lr.position.y + 12.0
	var i := scroll
	while i < tickets.size() and y < size.y - 4:
		var t: Dictionary = tickets[i]
		if i == selected:
			draw_rect(Rect2(0, y - 11, lr.size.x, _row_h()), th["panel"])
		var late := str(t.get("breached", "false")) == "true"
		var chasing := t.has("chasing")
		var label := "%s  %s" % [t.get("id", "?"), t.get("type", "?")]
		if chasing:
			label += "  (chasing %s)" % t.get("chasing", "")
		draw_string(mono, Vector2(6, y), label, HORIZONTAL_ALIGNMENT_LEFT,
			lr.size.x - 12, 11, th["bad"] if late else th["ink"])
		y += _row_h()
		i += 1

	if selected >= 0 and selected < tickets.size():
		_draw_detail(tickets[selected], lr.size.x + 10.0)
	else:
		draw_string(mono, Vector2(lr.size.x + 10, 66), "Pick a ticket.",
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12, th["dim"])

func _draw_detail(t: Dictionary, x: float) -> void:
	var w := size.x - x - 8.0
	var y := 62.0
	draw_string(mono, Vector2(x, y), str(t.get("id", "")), HORIZONTAL_ALIGNMENT_LEFT, w, 13, th["ink"])
	y += 18
	draw_string(mono, Vector2(x, y), "opened %s, due %s" % [t.get("opened", "?"), t.get("due", "?")],
		HORIZONTAL_ALIGNMENT_LEFT, w, 11, th["dim"])
	y += 22

	# The prose. It is flavour and onboarding, and it is ALWAYS redundant with
	# the structured fields below it -- §7. If a human has to read free text to
	# know what to do, a script cannot do it either.
	var desc := str(t.get("description", ""))
	for line in _wrap(desc, w, 12):
		draw_string(mono, Vector2(x, y), line, HORIZONTAL_ALIGNMENT_LEFT, w, 12, th["ink"])
		y += 15
	y += 6

	draw_string(mono, Vector2(x, y), "subject: %s" % t.get("ref", "?"),
		HORIZONTAL_ALIGNMENT_LEFT, w, 11, th["dim"])
	y += 16
	for k in t.keys():
		# What is left after these is the ticket's OWN fields -- rehire,
		# also_dept, share_override -- and they are drawn in the accent colour
		# because they are the whole reason this ticket is different from the
		# last one. Nothing is hidden; there is nothing to diagnose (§2).
		if k in ["id", "type", "opened", "due", "sla_minutes", "state", "breached",
				 "subject", "kind", "ref", "fields", "description", "acceptance",
				 "closed_day", "closed_by", "chasing"]:
			continue
		draw_string(mono, Vector2(x, y), "%s: %s" % [k, t[k]],
			HORIZONTAL_ALIGNMENT_LEFT, w, 11, th["accent"])
		y += 15
	y += 8

	if checks.is_empty():
		draw_string(mono, Vector2(x, y), "Press Check to see what the world thinks.",
			HORIZONTAL_ALIGNMENT_LEFT, w, 11, th["dim"])
		return
	# WRAPPED, NOT CLIPPED. The reason a check fails is the single most useful
	# sentence in this window, and the first version cut it off mid-word at
	# the window edge -- "the login is first initial plus family name, with a
	# number onl". A game that half-tells you why is worse than one that does
	# not tell you, because you stop reading it.
	for raw in checks:
		var c: Dictionary = raw
		var col: Color = th["ok"] if c["mark"] == "PASS" else (th["dim"] if c["mark"] == "n/a" else th["bad"])
		var first := true
		for l in _wrap(str(c["doc"]), w - 30, 11):
			draw_string(mono, Vector2(x, y), "%-4s %s" % [c["mark"] if first else "", l],
				HORIZONTAL_ALIGNMENT_LEFT, w, 11, col)
			first = false
			y += 13
		if str(c["why"]) != "":
			for l in _wrap(str(c["why"]), w - 30, 10):
				draw_string(mono, Vector2(x + 26, y), l, HORIZONTAL_ALIGNMENT_LEFT, w - 26, 10, th["bad"])
				y += 12
		y += 3

func _wrap(s: String, w: float, sz: int) -> Array:
	var out := []
	var line := ""
	for word in s.split(" "):
		var t: String = line + (" " if line != "" else "") + str(word)
		if mono.get_string_size(t, HORIZONTAL_ALIGNMENT_LEFT, -1, sz).x > w and line != "":
			out.append(line)
			line = str(word)
		else:
			line = t
	if line != "":
		out.append(line)
	return out

func _buttons() -> Array:
	return [
		{"rect": Rect2(6, 26, 62, 19), "label": "Refresh"},
		{"rect": Rect2(72, 26, 52, 19), "label": "Check"},
		{"rect": Rect2(128, 26, 74, 19), "label": "Closed", "on": showing_closed},
		{"rect": Rect2(206, 26, 78, 19), "label": "Go home"},
	]

func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and e.pressed:
		if e.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			scroll = mini(scroll + 3, maxi(0, tickets.size() - 4)); queue_redraw(); return
		if e.button_index == MOUSE_BUTTON_WHEEL_UP:
			scroll = maxi(scroll - 3, 0); queue_redraw(); return
		if e.button_index != MOUSE_BUTTON_LEFT:
			return
		for raw in _buttons():
			var b: Dictionary = raw
			if (b["rect"] as Rect2).has_point(e.position):
				match str(b["label"]):
					"Refresh": refresh()
					"Check": _check()
					"Closed": showing_closed = not showing_closed; scroll = 0; refresh()
					"Go home": _go_home()
				return
		var lr := _list_rect()
		if e.position.x < lr.size.x and e.position.y > lr.position.y:
			var idx := scroll + int((e.position.y - lr.position.y - 2.0) / _row_h())
			if idx >= 0 and idx < tickets.size():
				selected = idx
				checks.clear()
				_check()

# What the desktop should put in the X11 selection when this window is
# clicked: the ticket you are looking at. Retyping "TCK-00042" into a terminal
# is the sort of small friction that adds up over a shift.
func selected_text() -> String:
	if selected < 0 or selected >= tickets.size():
		return ""
	return str((tickets[selected] as Dictionary).get("id", ""))

func _check() -> void:
	if selected < 0 or selected >= tickets.size():
		return
	var id := str(tickets[selected].get("id", ""))
	var resp := api.exec("ticket.check %s" % id)
	checks.clear()
	for raw in api.body_lines(resp):
		var l := str(raw)
		var mark := "    "
		var rest := l
		if l.begins_with("PASS"):
			mark = "PASS"; rest = l.substr(4)
		elif l.begins_with("n/a"):
			mark = "n/a"; rest = l.substr(3)
		var cname := rest.strip_edges().get_slice(" ", 0)
		var tail := rest.strip_edges().substr(cname.length()).strip_edges()
		var why := ""
		var sep := tail.find("  -- ")
		if sep >= 0:
			why = tail.substr(sep + 5)
			tail = tail.substr(0, sep)
		checks.append({"mark": mark, "id": cname, "doc": tail.strip_edges(), "why": why})
	refresh_after_check()

func refresh_after_check() -> void:
	var keep := selected
	refresh()
	selected = keep if keep < tickets.size() else -1
	queue_redraw()

# END THE DAY. There is no "sleep" resource and no penalty for going home
# early -- the day ends when you decide it does, and everything you did not
# get to is still there tomorrow, with tomorrow's on top of it. That is the
# entire punishment for falling behind (§2: no fail screen).
func _go_home() -> void:
	api.exec("day.advance 1")
	scroll = 0
	selected = -1
	checks.clear()
	refresh()
