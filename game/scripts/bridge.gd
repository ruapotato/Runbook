# bridge.gd — the ship, and the console that narrates it.
#
# THE ONE IDEA. Every action on this screen prints the command that performed
# it, in the strip along the bottom, in the same syntax a script would use.
# Give the shields a bar and the console says `power shields 3`. Send somebody
# to the fire and it says `send Vane 2`. There is no separate "scripting mode"
# to graduate into, and no tutorial that teaches the syntax: the player learns
# the language by playing, because playing is the language being spoken.
#
# That is why the console is not a log window off to one side. It is under the
# ship, always visible, and it scrolls while you play. By the time somebody
# wants a script they have watched the vocabulary go past a hundred times.
#
# WHAT THIS WINDOW IS NOT is the only interface to the ship. Everything here
# can be typed into the terminal (`rb power shields 3`) or written in a script
# (`do("power shields 3")`), because this window has no private channel -- it
# calls api.exec() with a line of text, like everything else in this client.
extends Control

const UiFont := preload("res://scripts/uifont.gd")

var api: RunbookApi
var mono: Font
var echo: Callable            # set by the desk: where the command lines go

# --- what the last refresh saw -------------------------------------------
var ship: Dictionary = {}
var enemy: Dictionary = {}
var rooms: Array = []
var crew: Array = []
var events: PackedStringArray = PackedStringArray()

# --- what the player is doing --------------------------------------------
# WHO IS SELECTED, which is the whole crew interface. FTL asks you to drag a
# person to a room; a mouse drag is one gesture that cannot be typed, so this
# is two clicks instead -- pick a person, pick a room -- and each of them has
# a name the console can print.
var picked := ""
var hover_room := -1
var console: PackedStringArray = PackedStringArray()

# The palette. Not theme.gd: that file exists to make vendor appliances look
# different from each other, and there is exactly one ship.
const BG      := Color("#0d1117")
const PANEL   := Color("#161d26")
const EDGE    := Color("#2b3a4a")
const INK     := Color("#c9d6e3")
const DIM     := Color("#66798c")
const HULL    := Color("#4fa96b")
const SHIELD  := Color("#4a8fd0")
const POWER   := Color("#e0b642")
const FIRE    := Color("#e0662a")
const VACUUM  := Color("#233044")
const BAD     := Color("#c0453b")

func setup(a: RunbookApi, on_echo: Callable) -> void:
	api = a
	echo = on_echo
	mono = UiFont.mono()
	refresh()

# ONE COMMAND, ONE ECHO, ONE REFRESH -- and in that order, so the console
# shows what was sent even when the ship refuses it. A rejected command is
# still a command the player typed, and hiding it teaches them that the
# console is a highlight reel rather than the truth.
func act(line: String) -> void:
	var resp := api.exec(line)
	console.append(line)
	if not api.ok(resp):
		var why := api.error_text(resp)
		console.append("  " + (why if why != "" else "refused"))
	while console.size() > 6:
		console.remove_at(0)
	if echo.is_valid():
		echo.call(line)
	refresh()

func refresh() -> void:
	var s := api.objects(api.exec("status"))
	if not s.is_empty():
		ship = s[0]
	var en := api.objects(api.exec("enemy"))
	if not en.is_empty():
		enemy = en[0]
	rooms = api.objects(api.exec("rooms"))
	crew = api.objects(api.exec("crew"))
	var lg := api.body_lines(api.exec("log"))
	events = PackedStringArray()
	for l in lg:
		events.append(str(l))
	queue_redraw()

# EVERY VALUE OUT OF THE API IS A STRING.
#
# api.fields() hands back what the protocol said, verbatim, and does not guess
# at types -- which is right, because a client that guesses is a client that
# guesses wrong on the one field where it matters. So the guessing happens
# here, once, in three functions, instead of at forty call sites.
#
# GDScript has no bool() constructor, which is the specific way this went
# wrong: `bool(room.get("breach"))` is a runtime error, and a runtime error in
# _gui_input aborts the handler silently -- so clicks on the power pips did
# nothing at all, and nothing anywhere said why.
func _i(d: Dictionary, k: String, dflt: int = 0) -> int:
	return int(str(d.get(k, dflt)))

func _f(d: Dictionary, k: String, dflt: float = 0.0) -> float:
	return float(str(d.get(k, dflt)))

func _yes(d: Dictionary, k: String) -> bool:
	return str(d.get(k, "false")) == "true"

# --------------------------------------------------------------- geometry
# THE DECK PLAN IS NOT A GRID OF EQUAL BOXES. A ship you can read at a glance
# has a shape: the reactor aft, the bridge forward, a corridor down the spine.
# The layout is fixed because the ship is fixed -- one ship, one fight.
const PLAN := [
	Rect2(0, 0, 1, 1),   # 0 reactor
	Rect2(1, 0, 1, 1),   # 1 shields
	Rect2(0, 1, 1, 1),   # 2 engines
	Rect2(2, 0, 1, 1),   # 3 weapons
	Rect2(1, 1, 1, 1),   # 4 oxygen
	Rect2(2, 1, 1, 1),   # 5 medbay
	Rect2(3, 0, 1, 1),   # 6 computer
	Rect2(3, 1, 1, 1),   # 7 corridor
]

func _deck() -> Rect2:
	var top := 58.0
	var bottom := size.y - _console_h()
	var h := bottom - top - 16.0
	var w := size.x - 32.0
	return Rect2(16, top, w, h)

func _console_h() -> float:
	return 108.0

func _room_rect(n: int) -> Rect2:
	if n < 0 or n >= PLAN.size():
		return Rect2()
	var d := _deck()
	var cw := d.size.x / 4.0
	var ch := d.size.y / 2.0
	var p: Rect2 = PLAN[n]
	return Rect2(d.position.x + p.position.x * cw + 3,
				 d.position.y + p.position.y * ch + 3,
				 cw - 6, ch - 6)

func _room_at(p: Vector2) -> int:
	for i in range(rooms.size()):
		if _room_rect(i).has_point(p):
			return i
	return -1

# --------------------------------------------------------------- painting
func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	_draw_status()
	for i in range(rooms.size()):
		_draw_room(i)
	_draw_crew()
	_draw_console()

func _text(at: Vector2, s: String, col: Color, px := 12) -> void:
	draw_string(mono, at, s, HORIZONTAL_ALIGNMENT_LEFT, -1, px, col)

func _bar(r: Rect2, frac: float, col: Color) -> void:
	draw_rect(r, PANEL)
	if frac > 0.0:
		draw_rect(Rect2(r.position, Vector2(r.size.x * clampf(frac, 0, 1), r.size.y)), col)
	draw_rect(r, EDGE, false, 1.0)

func _draw_status() -> void:
	draw_rect(Rect2(0, 0, size.x, 52), PANEL)
	draw_line(Vector2(0, 52), Vector2(size.x, 52), EDGE, 1.0)

	var hull := _f(ship, "hull")
	var hull_max := maxf(1.0, _f(ship, "hull_max", 16.0))
	_text(Vector2(14, 18), str(ship.get("ship", "Kestrel")), INK, 13)
	_bar(Rect2(14, 26, 150, 12), hull / hull_max, HULL if hull > hull_max * 0.35 else BAD)
	_text(Vector2(170, 36), "hull %d" % int(hull), DIM, 11)

	# Shields as pips, because a shield layer is a discrete thing you either
	# have or do not, and a smooth bar would lie about that.
	var sh := _i(ship, "shields")
	for i in range(4):
		var r := Rect2(240 + i * 16, 26, 12, 12)
		draw_rect(r, SHIELD if i < sh else PANEL)
		draw_rect(r, EDGE, false, 1.0)
	_text(Vector2(240, 20), "shields", DIM, 10)

	_text(Vector2(320, 20), "gun", DIM, 10)
	_bar(Rect2(320, 26, 90, 12), _f(ship, "weapon") / 100.0, POWER)

	var free := _i(ship, "power_free")
	_text(Vector2(430, 20), "reactor", DIM, 10)
	for i in range(_i(ship, "power_total", 8)):
		var r := Rect2(430 + i * 11, 26, 8, 12)
		draw_rect(r, POWER if i < free else PANEL)
		draw_rect(r, EDGE, false, 1.0)

	# THE ENEMY, on the same screen, in the same units. FTL puts them in a
	# separate panel you have to look at; there is one raider and it fits.
	var ex := size.x - 250.0
	_text(Vector2(ex, 18), str(enemy.get("name", "raider")), BAD, 13)
	_bar(Rect2(ex, 26, 110, 12), _f(enemy, "hull") / maxf(1.0, _f(enemy, "hull_max", 18.0)), BAD)
	_text(Vector2(ex + 120, 20), "their gun", DIM, 10)
	_bar(Rect2(ex + 120, 26, 110, 12), _f(enemy, "charge") / 100.0, FIRE)

	var clock := _i(ship, "clock")
	var st := "PAUSED — space to fly" if _yes(ship, "paused") else "%d:%02d" % [clock / 60, clock % 60]
	_text(Vector2(size.x / 2.0 - 60, 46), st, POWER if _yes(ship, "paused") else DIM, 11)

func _draw_room(n: int) -> void:
	var r := _room_rect(n)
	var room: Dictionary = rooms[n]
	var oxy := _f(room, "oxygen", 100.0)

	# VACUUM IS VISIBLE BEFORE IT IS FATAL. A room draining is the cue to move
	# somebody, and a player who only finds out when the crew die has been
	# given a puzzle with the clues removed.
	var floor_col := PANEL.lerp(VACUUM, clampf((100.0 - oxy) / 100.0, 0, 1))
	draw_rect(r, floor_col)

	var fire := _f(room, "fire")
	if fire > 0.0:
		draw_rect(Rect2(r.position, Vector2(r.size.x, r.size.y)), Color(FIRE.r, FIRE.g, FIRE.b, clampf(fire / 100.0, 0.15, 0.75)))
	if _yes(room, "breach"):
		draw_rect(r, BAD, false, 3.0)
	draw_rect(r, EDGE if n != hover_room else INK, false, 1.0)

	var sys := str(room.get("system", "none"))
	_text(r.position + Vector2(8, 16), str(room.get("name", "?")), INK, 12)
	if sys == "none":
		return

	# The power pips ARE the button. Click the pip you want lit: clicking the
	# third pip asks for three bars, clicking a lit pip below the current
	# level takes power back. One gesture, one command, and the command reads
	# exactly like what you did.
	var cap := _i(room, "cap")
	var bars := _i(room, "bars")
	var dmg := _i(room, "damage")
	for i in range(cap):
		var pr := _pip_rect(n, i)
		var broken := i >= cap - dmg
		var col := POWER if i < bars else PANEL
		if broken:
			col = BAD.darkened(0.4)
		draw_rect(pr, col)
		draw_rect(pr, EDGE, false, 1.0)

	if fire > 0.0:
		_text(r.position + Vector2(r.size.x - 34, 16), "FIRE", FIRE, 11)

func _pip_rect(n: int, i: int) -> Rect2:
	var r := _room_rect(n)
	return Rect2(r.position.x + 8 + i * 16, r.position.y + r.size.y - 24, 12, 14)

func _draw_crew() -> void:
	# Crew stack along the bottom of their room, and the selected one gets a
	# ring. Names are drawn because `send Vane 2` needs the player to know
	# which dot is Vane.
	var per_room := {}
	for raw in crew:
		var c: Dictionary = raw
		if not _yes(c, "alive"):
			continue
		var rn := _i(c, "room")
		var idx := int(per_room.get(rn, 0))
		per_room[rn] = idx + 1
		var rr := _room_rect(rn)
		var at := rr.position + Vector2(rr.size.x - 22, 34 + idx * 22)
		var name := str(c.get("name", "?"))
		var hp := _f(c, "health", 100.0)
		var col := INK if hp > 40 else BAD
		draw_circle(at, 7, col)
		if name == picked:
			draw_arc(at, 11, 0, TAU, 24, POWER, 2.0)
		_text(at + Vector2(-9 - mono.get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, 10).x, 4), name, col, 10)

# THE CONSOLE. Everything above prints into here, and here is the only place
# the player is ever taught the syntax.
func _draw_console() -> void:
	var y := size.y - _console_h()
	draw_rect(Rect2(0, y, size.x, _console_h()), Color("#0a0e13"))
	draw_line(Vector2(0, y), Vector2(size.x, y), EDGE, 1.0)

	# The ship's own narration on the left, the commands on the right. They
	# are different things: one is what happened, the other is what you said.
	var ey := y + 18.0
	for i in range(maxi(0, events.size() - 4), events.size()):
		_text(Vector2(12, ey), str(events[i]), DIM, 11)
		ey += 14

	var cx := size.x / 2.0
	draw_line(Vector2(cx - 10, y + 6), Vector2(cx - 10, size.y - 6), EDGE, 1.0)
	var cy := y + 18.0
	for line in console:
		var l := str(line)
		var col := DIM if l.begins_with("  ") else HULL
		_text(Vector2(cx + 4, cy), ("" if l.begins_with("  ") else "> ") + l, col, 11)
		cy += 14
	if console.is_empty():
		_text(Vector2(cx + 4, cy), "every click prints its command here.", DIM, 11)
		_text(Vector2(cx + 4, cy + 16), "the same words work in the terminal:", DIM, 11)
		_text(Vector2(cx + 4, cy + 30), "  rb power shields 3", HULL, 11)
		_text(Vector2(cx + 4, cy + 44), "and in a script:  do(\"fire\")", HULL, 11)

# ------------------------------------------------------------- interaction
func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion:
		var h := _room_at(e.position)
		if h != hover_room:
			hover_room = h
			queue_redraw()
		return

	if e is InputEventKey and e.pressed:
		match e.keycode:
			KEY_SPACE:
				act("resume" if _yes(ship, "paused") else "pause")
				accept_event()
			KEY_F:
				act("fire")
				accept_event()
		return

	if not (e is InputEventMouseButton and e.pressed):
		return
	var mb := e as InputEventMouseButton

	# Right-click seals a door. It is the only two-state thing on the ship and
	# it gets the button nobody else wants.
	if mb.button_index == MOUSE_BUTTON_RIGHT:
		var n := _room_at(mb.position)
		if n >= 0:
			var shut := str((rooms[n] as Dictionary).get("door", "open")) == "open"
			act("door %d %s" % [n, "shut" if shut else "open"])
		return

	if mb.button_index != MOUSE_BUTTON_LEFT:
		return

	# A crew dot first: clicking a person picks them up, clicking them again
	# puts them down.
	for raw in crew:
		var c: Dictionary = raw
		if not _yes(c, "alive"):
			continue
		var rr := _room_rect(_i(c, "room"))
		if rr.has_point(mb.position) and mb.position.x > rr.position.x + rr.size.x - 40:
			var nm := str(c.get("name", ""))
			picked = "" if picked == nm else nm
			queue_redraw()
			return

	var n := _room_at(mb.position)
	if n < 0:
		return
	var room: Dictionary = rooms[n]

	# A pip, if they hit one. Clicking pip i asks for i+1 bars, except when
	# that is what is already lit -- then it means "take one back", which is
	# what a player expects from clicking the top of a bar twice.
	var cap := _i(room, "cap")
	for i in range(cap):
		if _pip_rect(n, i).has_point(mb.position):
			var want := i + 1
			if want == _i(room, "bars"):
				want = i
			act("power %s %d" % [room.get("system", "?"), want])
			return

	# Otherwise: somewhere in the room. If a person is picked, that is a
	# destination.
	if picked != "":
		act("send %s %d" % [picked, n])
		picked = ""
		return
