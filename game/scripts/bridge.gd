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
var hover_vent := -1
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

# ------------------------------------------------------ the deck plan
# A SHIP IS NOT A SPREADSHEET.
#
# The first version of this drew eight equal boxes in a four-by-two grid,
# which is a table of rooms rather than a picture of a ship, and a playtester
# said so in one sentence. It matters more than it looks like it does: you are
# supposed to glance at this and know instantly that the fire is aft, that the
# breach is two rooms from the person nearest it, and that the corridor is
# what connects them. Equal boxes in a grid tell you none of that, because
# every room is the same distance from every other one.
#
# So the Kestrel has a shape: engines aft, a nose forward pointing at the
# raider, and a spine corridor down the middle that everything opens onto.
# Rooms are different sizes because they are different rooms.
#
# Grid units, +x forward. The hull is 9 wide and 4.8 deep.
const DECK_W := 10.4
const DECK_H := 4.8
const PLAN := {
	0: Rect2(1.4, 0.0, 2.0, 1.9),    # reactor    -- amidships, upper
	1: Rect2(5.8, 0.0, 1.9, 2.4),    # shields    -- forward, upper
	2: Rect2(0.0, 1.0, 1.4, 2.8),    # engines    -- aft
	3: Rect2(5.8, 2.4, 1.9, 2.4),    # weapons    -- forward, lower
	4: Rect2(3.4, 0.0, 2.4, 1.9),    # oxygen     -- amidships, upper
	5: Rect2(1.4, 2.9, 2.0, 1.9),    # medbay     -- amidships, lower
	6: Rect2(3.4, 2.9, 2.4, 1.9),    # computer   -- amidships, lower
	7: Rect2(1.4, 1.9, 4.4, 1.0),    # corridor   -- the spine
	8: Rect2(7.75, 1.7, 1.2, 1.4),   # teleporter -- the pad, in the bow
}

# THIS AND core/ship.c's LINKS TABLE ARE THE SAME DECK PLAN.
#
# The model walks crew and spreads fire along an adjacency list; this draws
# the rooms. If they disagree the game becomes unreadable in the worst
# possible way -- everything works, and nothing is where it looks. Anything
# moved here has to move there.

func _console_h() -> float:
	return 108.0

# The pixel rectangle the whole ship is drawn in, and the scale that gets it
# there. Measured rather than assumed, because the window is resizable and a
# deck plan that overflows its window is worse than no picture at all.
func _deck() -> Rect2:
	var top := 58.0
	var bottom := size.y - _console_h()
	var avail := Rect2(20, top + 10, size.x - 40, bottom - top - 20)
	if avail.size.x < 20 or avail.size.y < 20:
		return Rect2(20, top, 10, 10)
	var s: float = minf(avail.size.x / DECK_W, avail.size.y / DECK_H)
	var w := DECK_W * s
	var h := DECK_H * s
	return Rect2(avail.position.x + (avail.size.x - w) / 2.0,
				 avail.position.y + (avail.size.y - h) / 2.0, w, h)

func _scale() -> float:
	return _deck().size.x / DECK_W

func _room_rect(n: int) -> Rect2:
	if not PLAN.has(n):
		return Rect2()
	var d := _deck()
	var s := _scale()
	var p: Rect2 = PLAN[n]
	return Rect2(d.position.x + p.position.x * s + 2.0,
				 d.position.y + p.position.y * s + 2.0,
				 p.size.x * s - 4.0, p.size.y * s - 4.0)

func _room_at(p: Vector2) -> int:
	for i in range(rooms.size()):
		if _room_rect(i).has_point(p):
			return i
	return -1

# ------------------------------------------------------------ painting
func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	_draw_status()
	_draw_hull()
	for i in range(rooms.size()):
		_draw_room(i)
	_draw_doors()
	_draw_vents()
	_draw_crew()
	_draw_console()

func _text(at: Vector2, s: String, col: Color, px := 12) -> void:
	draw_string(mono, at, s, HORIZONTAL_ALIGNMENT_LEFT, -1, px, col)

func _bar(r: Rect2, frac: float, col: Color) -> void:
	draw_rect(r, PANEL)
	if frac > 0.0:
		draw_rect(Rect2(r.position, Vector2(r.size.x * clampf(frac, 0, 1), r.size.y)), col)
	draw_rect(r, EDGE, false, 1.0)

# THE HULL, DRAWN UNDERNEATH THE ROOMS. A nose that points at the enemy and
# two engine nacelles at the back, so the ship has a front and the player
# never has to work out which way it is facing.
func _draw_hull() -> void:
	var d := _deck()
	var s := _scale()
	var o := d.position

	# Engine nacelles, aft of the engine room.
	for yy in [0.7, 3.4]:
		var nac := Rect2(o.x - 0.85 * s, o.y + yy * s, 0.9 * s, 0.7 * s)
		draw_rect(nac, PANEL.darkened(0.25))
		draw_rect(nac, EDGE, false, 1.0)
		# The glow is the engines' power, so a dead engine room stops glowing.
		var eb := 0
		if rooms.size() > 2:
			eb = _i(rooms[2] as Dictionary, "bars")
		if eb > 0:
			draw_rect(Rect2(nac.position.x - 0.3 * s, nac.position.y + nac.size.y * 0.25,
							0.3 * s, nac.size.y * 0.5),
					  Color(SHIELD.r, SHIELD.g, SHIELD.b, clampf(eb / 4.0, 0.2, 0.9)))

	# THE STERN IS ANGLED TOO. Square corners at the back left two dark
	# rectangles either side of the engine room that read as empty rooms --
	# somewhere you might try to send somebody. A hull should have no corner
	# that looks like a place.
	var body := PackedVector2Array([
		o + Vector2(0.0, 0.9) * s,
		o + Vector2(0.55, 0.0) * s,
		o + Vector2(7.7, 0.0) * s,
		o + Vector2(8.6, 1.2) * s,        # shoulder, upper
		o + Vector2(10.4, 2.4) * s,       # the point
		o + Vector2(8.6, 3.6) * s,        # shoulder, lower
		o + Vector2(7.7, 4.8) * s,
		o + Vector2(0.55, 4.8) * s,
		o + Vector2(0.0, 3.9) * s,
	])
	draw_colored_polygon(body, PANEL.darkened(0.45))
	draw_polyline(body + PackedVector2Array([body[0]]), EDGE, 2.0)

	# THE SHIELD BUBBLE, drawn around the hull, one ring per layer up. This is
	# the only place shields are a shape rather than a number, and it is the
	# thing you actually watch: the ring goes and the next shot is on the hull.
	# AN ELLIPSE, NOT A CIRCLE. A circle wide enough to clear the nose is
	# twice as tall as the ship, so it left the window at the top and bottom
	# and read as two unexplained curves rather than as a bubble around
	# anything. The ring has to hug the hull to be a shield.
	# CLAMPED TO THE SPACE THE DECK IS DRAWN IN. Sized off the deck alone,
	# the rings ran up into the status strip and across the raider's own
	# health bar -- the shield bubble does not belong on top of the numbers.
	var sh := _i(ship, "shields")
	var ctr := d.get_center()
	var head := 58.0
	var room_above := ctr.y - head - 6.0
	var room_below := (size.y - _console_h() - 6.0) - ctr.y
	var ry_max: float = maxf(20.0, minf(room_above, room_below))
	for i in range(sh):
		var rx: float = minf((DECK_W * 0.55 + i * 0.18) * s, ctr.x - 6.0)
		var ry: float = minf((DECK_H * 0.66 + i * 0.18) * s, ry_max)
		_ring(ctr, rx, ry, Color(SHIELD.r, SHIELD.g, SHIELD.b, 0.55 - i * 0.11))

func _ring(c: Vector2, rx: float, ry: float, col: Color) -> void:
	var pts := PackedVector2Array()
	for i in range(65):
		var a := TAU * float(i) / 64.0
		pts.append(c + Vector2(cos(a) * rx, sin(a) * ry))
	draw_polyline(pts, col, 2.0)

func _draw_room(n: int) -> void:
	var r := _room_rect(n)
	var room: Dictionary = rooms[n]
	var oxy := _f(room, "oxygen", 100.0)
	var s := _scale()

	# VACUUM IS VISIBLE BEFORE IT IS FATAL. A room draining is the cue to move
	# somebody, and a player who only finds out when the crew die has been
	# given a puzzle with the clues removed.
	var floor_col := PANEL.lerp(VACUUM, clampf((100.0 - oxy) / 100.0, 0, 1))
	draw_rect(r, floor_col)

	# Deck plating, so a big room does not read as a blank slab.
	var step := 0.5 * s
	var gx := r.position.x + step
	while gx < r.position.x + r.size.x - 2:
		draw_line(Vector2(gx, r.position.y + 2), Vector2(gx, r.position.y + r.size.y - 2),
				  Color(1, 1, 1, 0.025), 1.0)
		gx += step

	var fire := _f(room, "fire")
	if fire > 0.0:
		_draw_fire(r, fire)
	if _yes(room, "breach"):
		_draw_breach(r)

	draw_rect(r, EDGE if n != hover_room else INK, false, 2.0 if n == hover_room else 1.0)

	var sys := str(room.get("system", "none"))
	_text(r.position + Vector2(7, 15), str(room.get("name", "?")), INK, 11)
	if sys == "none":
		return

	_draw_system_glyph(r, sys, _i(room, "bars"))

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

func _pip_rect(n: int, i: int) -> Rect2:
	var r := _room_rect(n)
	return Rect2(r.position.x + 7 + i * 15, r.position.y + r.size.y - 20, 11, 13)

# FLAMES, NOT A WASH. An orange rectangle says "this room is tinted"; three
# flame shapes that get taller as it spreads say "this room is burning", and
# the difference is whether the screen is worth looking at.
func _draw_fire(r: Rect2, fire: float) -> void:
	var f := clampf(fire / 100.0, 0.0, 1.0)
	draw_rect(r, Color(FIRE.r, FIRE.g * 0.6, 0.0, 0.10 + 0.25 * f))
	var n := 2 + int(f * 4.0)
	var base := r.position.y + r.size.y - 6
	for i in range(n):
		var x := r.position.x + r.size.x * (float(i) + 0.5) / float(n)
		var h := (8.0 + 22.0 * f)
		draw_colored_polygon(PackedVector2Array([
			Vector2(x - 5, base), Vector2(x, base - h), Vector2(x + 5, base)]),
			Color(FIRE.r, FIRE.g, 0.15, 0.85))
		draw_colored_polygon(PackedVector2Array([
			Vector2(x - 2.5, base), Vector2(x, base - h * 0.55), Vector2(x + 2.5, base)]),
			Color(1.0, 0.88, 0.4, 0.9))

# A HOLE, not a red border. A breach is the one thing on this ship that cannot
# be repaired by leaving it alone, and it should look like damage.
func _draw_breach(r: Rect2) -> void:
	var c := r.get_center()
	var pts := PackedVector2Array()
	var rad: float = minf(r.size.x, r.size.y) * 0.22
	for i in range(9):
		var a := TAU * float(i) / 9.0
		var rr: float = rad * (0.55 if i % 2 == 1 else 1.0)
		pts.append(c + Vector2(cos(a), sin(a)) * rr)
	draw_colored_polygon(pts, BG)
	draw_polyline(pts + PackedVector2Array([pts[0]]), BAD, 2.0)
	draw_rect(r, BAD, false, 2.0)

# WHAT A ROOM IS, AT A GLANCE. Names are drawn too, but a shape is read
# faster than a word and a player under fire is not reading.
func _draw_system_glyph(r: Rect2, sys: String, bars: int) -> void:
	var c := r.get_center() + Vector2(0, -2)
	var lit: Color = INK if bars > 0 else DIM.darkened(0.3)
	var u: float = minf(r.size.x, r.size.y) * 0.22
	match sys:
		"reactor":
			draw_arc(c, u, 0, TAU, 24, lit, 2.0)
			for i in range(6):
				var a := TAU * float(i) / 6.0
				draw_line(c + Vector2(cos(a), sin(a)) * u * 1.2,
						  c + Vector2(cos(a), sin(a)) * u * 1.7, lit, 2.0)
		"shields":
			draw_arc(c + Vector2(0, u * 0.6), u * 1.4, PI, TAU, 24, lit, 2.5)
			draw_arc(c + Vector2(0, u * 0.6), u * 0.9, PI, TAU, 24, lit, 1.5)
		"engines":
			for i in range(3):
				var x := c.x - u + i * u * 0.9
				draw_polyline(PackedVector2Array([
					Vector2(x + u * 0.5, c.y - u), Vector2(x - u * 0.2, c.y),
					Vector2(x + u * 0.5, c.y + u)]), lit, 2.0)
		"weapons":
			draw_line(c + Vector2(-u * 1.4, 0), c + Vector2(u * 1.0, 0), lit, 3.0)
			draw_colored_polygon(PackedVector2Array([
				Vector2(c.x + u * 0.8, c.y - u * 0.7), Vector2(c.x + u * 1.7, c.y),
				Vector2(c.x + u * 0.8, c.y + u * 0.7)]), lit)
		"oxygen":
			draw_arc(c, u * 1.2, 0, TAU, 24, lit, 2.0)
			draw_arc(c, u * 0.5, 0, TAU, 16, lit, 1.5)
		"medbay":
			draw_rect(Rect2(c.x - u * 0.3, c.y - u, u * 0.6, u * 2.0), lit)
			draw_rect(Rect2(c.x - u, c.y - u * 0.3, u * 2.0, u * 0.6), lit)
		"teleporter":
			# A pad, seen from above, with somebody standing on it.
			draw_arc(c, u * 1.3, 0, TAU, 24, lit, 2.0)
			draw_arc(c, u * 0.8, 0, TAU, 20, lit, 1.0)
			for i in range(4):
				var a := TAU * float(i) / 4.0 + PI / 4.0
				draw_line(c + Vector2(cos(a), sin(a)) * u * 1.3,
						  c + Vector2(cos(a), sin(a)) * u * 1.9, lit, 2.0)
		"computer":
			draw_rect(Rect2(c.x - u * 1.2, c.y - u * 0.9, u * 2.4, u * 1.6), Color(0, 0, 0, 0))
			draw_rect(Rect2(c.x - u * 1.2, c.y - u * 0.9, u * 2.4, u * 1.6), lit, false, 2.0)
			for i in range(3):
				var yy := c.y - u * 0.5 + i * u * 0.45
				draw_line(Vector2(c.x - u * 0.8, yy), Vector2(c.x + u * (0.2 + 0.2 * i), yy), lit, 1.5)

# DOORS ARE ON THE WALLS, which is where doors are. Each room's door is drawn
# on the edge it shares with the spine corridor, so `door 4 shut` has a place
# on the screen and sealing a room is something you can SEE, not a state you
# have to remember.
func _door_mark(n: int) -> Rect2:
	if n == 7 or not PLAN.has(n):
		return Rect2()
	var d := _deck()
	var s := _scale()
	var p: Rect2 = PLAN[n]
	if n == 8:
		# The pad is forward of shields and weapons, so its door is on its
		# aft wall rather than on the spine.
		var pp: Rect2 = PLAN[8]
		return Rect2(d.position.x + pp.position.x * s - 3,
					 d.position.y + (pp.position.y + pp.size.y / 2.0) * s - 0.35 * s,
					 6, 0.7 * s)
	var spine: Rect2 = PLAN[7]
	var cx: float = clampf(p.position.x + p.size.x / 2.0,
						   spine.position.x + 0.3, spine.position.x + spine.size.x - 0.3)
	var gx := d.position.x + cx * s
	var gy: float
	if p.position.y + p.size.y <= spine.position.y + 0.01:
		gy = d.position.y + (p.position.y + p.size.y) * s      # room is above
	elif p.position.y >= spine.position.y + spine.size.y - 0.01:
		gy = d.position.y + p.position.y * s                   # room is below
	else:
		# Engines: aft of the spine, so the door is on its forward wall.
		gx = d.position.x + (p.position.x + p.size.x) * s
		gy = d.position.y + (spine.position.y + spine.size.y / 2.0) * s
		return Rect2(gx - 3, gy - 0.35 * s, 6, 0.7 * s)
	return Rect2(gx - 0.35 * s, gy - 3, 0.7 * s, 6)

# THE AIRLOCK HATCH, ON THE OUTER WALL.
#
# Every room that touches the hull gets a little hatch drawn on its outside
# edge. Clicking it opens the room to space: the fire dies on its own and
# anybody still in there suffocates. It is on the OUTSIDE because that is
# where an airlock is, and because a control that vents a room must not sit
# anywhere near the controls that power one.
func _vent_mark(n: int) -> Rect2:
	if not PLAN.has(n) or n == 7:
		return Rect2()
	var d := _deck()
	var s := _scale()
	var p: Rect2 = PLAN[n]
	var cx := d.position.x + (p.position.x + p.size.x / 2.0) * s
	# Rooms in the upper half vent through the top of the hull, lower half
	# through the bottom, and the two forward rooms through the bow.
	if n == 8:
		return Rect2(d.position.x + (p.position.x + p.size.x) * s + 2,
					 d.position.y + (p.position.y + p.size.y / 2.0) * s - 9, 12, 18)
	if p.position.y + p.size.y <= DECK_H / 2.0 + 0.01:
		return Rect2(cx - 9, d.position.y + p.position.y * s - 13, 18, 12)
	if p.position.y >= DECK_H / 2.0 - 0.01:
		return Rect2(cx - 9, d.position.y + (p.position.y + p.size.y) * s + 1, 18, 12)
	# The engine room straddles the middle; it vents aft.
	return Rect2(d.position.x + p.position.x * s - 13,
				 d.position.y + (p.position.y + p.size.y / 2.0) * s - 9, 12, 18)

func _draw_vents() -> void:
	for n in range(rooms.size()):
		var m := _vent_mark(n)
		if m.size.x <= 0:
			continue
		var open := str((rooms[n] as Dictionary).get("vent", "shut")) == "open"
		draw_rect(m, VACUUM if open else PANEL)
		draw_rect(m, POWER if n == hover_vent else (SHIELD if open else EDGE),
				  2.0 if n == hover_vent else 1.0)
		# An open hatch shows the dark outside and a couple of streaming
		# marks, so "this room is emptying" is visible from across the screen.
		if open:
			for i in range(2):
				var a := m.get_center() + Vector2(0, -4 + i * 8)
				draw_line(a, a + (m.get_center() - _room_rect(n).get_center()).normalized() * 10.0,
						  SHIELD, 1.5)

func _draw_doors() -> void:
	for n in range(rooms.size()):
		var m := _door_mark(n)
		if m.size.x <= 0:
			continue
		var shut := str((rooms[n] as Dictionary).get("door", "open")) != "open"
		if shut:
			draw_rect(m, BAD)
			draw_rect(m, BAD.lightened(0.3), false, 1.0)
		else:
			# An open door is a gap in the wall with two jambs, which is what
			# an open door looks like on every deck plan ever drawn.
			draw_rect(m, floor_tint())
			if m.size.x > m.size.y:
				draw_line(m.position, m.position + Vector2(0, m.size.y), HULL, 2.0)
				draw_line(m.position + Vector2(m.size.x, 0), m.position + m.size, HULL, 2.0)
			else:
				draw_line(m.position, m.position + Vector2(m.size.x, 0), HULL, 2.0)
				draw_line(m.position + Vector2(0, m.size.y), m.position + m.size, HULL, 2.0)

func floor_tint() -> Color:
	return PANEL

# WHERE A PERSON STANDS. Inside the room, spread out, so two people in the
# reactor are two dots and not one dot on top of another.
func _crew_pos(name: String) -> Vector2:
	var rn := -1
	var step := -1
	var across := 0.0
	for raw in crew:
		var c: Dictionary = raw
		if not _yes(c, "alive"):
			continue
		if str(c.get("name", "")) == name:
			rn = _i(c, "room")
			step = _i(c, "walking_to", -1)
			across = _f(c, "across") / 100.0
			break
	if rn < 0:
		return Vector2.ZERO
	# Count who else is in that room ahead of this one.
	var before := 0
	for raw in crew:
		var c: Dictionary = raw
		if not _yes(c, "alive"):
			continue
		if str(c.get("name", "")) == name:
			break
		if _i(c, "room") == rn:
			before += 1
	var r := _room_rect(rn)
	var s := _scale()
	var here := Vector2(r.position.x + 0.35 * s + before * 0.55 * s,
						r.position.y + r.size.y - 0.72 * s)
	if step < 0 or not PLAN.has(step):
		return here
	# WALKING IS DRAWN, NOT IMPLIED. Somebody crossing a doorway is shown in
	# the doorway, moving, because "they take four seconds to get there" is
	# only a real cost if you can watch it being paid.
	var nr := _room_rect(step)
	var there := Vector2(nr.position.x + nr.size.x * 0.5,
						 nr.position.y + nr.size.y - 0.72 * s)
	return here.lerp(there, clampf(across, 0.0, 1.0))

func _draw_crew() -> void:
	for raw in crew:
		var c: Dictionary = raw
		if not _yes(c, "alive"):
			continue
		var name := str(c.get("name", "?"))
		var at := _crew_pos(name)
		if at == Vector2.ZERO:
			continue
		var hp := _f(c, "health", 100.0)
		var col := HULL if hp > 60 else (POWER if hp > 30 else BAD)
		# A person, not a dot: a head and shoulders, so a crew member reads as
		# somebody standing in a room rather than a status light.
		draw_circle(at + Vector2(0, -7), 4.0, col)
		draw_colored_polygon(PackedVector2Array([
			at + Vector2(-5, 4), at + Vector2(-3.5, -3),
			at + Vector2(3.5, -3), at + Vector2(5, 4)]), col)
		# WHERE THEY ARE GOING, as a line to the room. Three people crossing
		# a ship at once is unreadable without it -- you cannot tell who is
		# heading for the fire and who is wandering back from one.
		var dest := _i(c, "dest", -1)
		if dest >= 0 and dest != _i(c, "room") and PLAN.has(dest):
			var dr := _room_rect(dest)
			draw_line(at, dr.get_center(), Color(col.r, col.g, col.b, 0.35), 1.0)
			draw_arc(dr.get_center(), 6, 0, TAU, 16, Color(col.r, col.g, col.b, 0.5), 1.0)
		if name == picked:
			draw_arc(at, 13, 0, TAU, 24, POWER, 2.0)
		# A BACKING PLATE UNDER THE NAME. Rooms have labels too, and in the
		# corridor -- which is one deck-unit tall -- a crew name landed on top
		# of the word "corridor" and neither was readable. The name has to
		# win: `send Vane 2` needs the player to know which figure is Vane.
		var nw := mono.get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, 10).x
		draw_rect(Rect2(at.x - nw / 2.0 - 2, at.y + 6, nw + 4, 12), Color(0, 0, 0, 0.55))
		_text(at + Vector2(-nw / 2.0, 16), name, col, 10)

# ------------------------------------------------- the status strip
# THE STATUS STRIP, REBUILT AROUND TIME.
#
# A playtester said the interface was unclear about the shields, the hull, and
# what was going on with the enemy ship -- and they were right, because it was
# showing PERCENTAGES. "their gun is at 68%" is a number. "they fire in 4s" is
# a decision, and the difference between those two sentences is most of what
# makes FTL readable.
#
# So: hull with its number on it, shields as discrete layers with the seconds
# until the next one returns, the gun saying READY rather than 100%, and the
# raider with a countdown that turns red when it is nearly up.
func _draw_status() -> void:
	draw_rect(Rect2(0, 0, size.x, 52), PANEL)
	draw_line(Vector2(0, 52), Vector2(size.x, 52), EDGE, 1.0)

	var hull := _f(ship, "hull")
	var hull_max: float = maxf(1.0, _f(ship, "hull_max", 16.0))
	_text(Vector2(14, 16), str(ship.get("ship", "Kestrel")), INK, 13)

	# HULL, SEGMENTED. A smooth bar at 60% is a colour; twelve blocks with
	# four missing is a count you can act on.
	_text(Vector2(14, 46), "HULL %d/%d" % [int(hull), int(hull_max)],
		  HULL if hull > hull_max * 0.35 else BAD, 11)
	var seg := 118.0 / hull_max
	for i in range(int(hull_max)):
		var r := Rect2(14 + i * seg, 24, seg - 2.0, 13)
		var lit := float(i) < hull
		draw_rect(r, (HULL if hull > hull_max * 0.35 else BAD) if lit else PANEL.darkened(0.3))
		draw_rect(r, EDGE, false, 1.0)

	# SHIELDS, WITH THE CLOCK ON THEM. An empty ring that is coming back in
	# two seconds is a completely different situation from an empty ring that
	# is not coming back at all, and the old strip drew them identically.
	var sh := _i(ship, "shields")
	var shmax: int = maxi(sh, _i(ship, "shields_max", 0))
	var sx := 150.0
	_text(Vector2(sx, 16), "SHIELDS", DIM, 10)
	for i in range(maxi(shmax, 1)):
		var c := Vector2(sx + 9 + i * 20, 30)
		draw_circle(c, 8, SHIELD if i < sh else PANEL.darkened(0.3))
		draw_arc(c, 8, 0, TAU, 20, EDGE, 1.0)
	var sin_t := _f(ship, "shield_in") / 10.0
	if sh < shmax and sin_t > 0.0:
		_text(Vector2(sx, 46), "+1 in %.1fs" % sin_t, SHIELD, 10)
	elif shmax == 0:
		_text(Vector2(sx, 46), "no power", BAD, 10)
	else:
		_text(Vector2(sx, 46), "up", DIM, 10)

	# THE GUN, AS A WORD WHEN IT MATTERS.
	var gx := sx + maxi(shmax, 1) * 20 + 24.0
	var w := _f(ship, "weapon") / 100.0
	_text(Vector2(gx, 16), "GUN", DIM, 10)
	_bar(Rect2(gx, 24, 84, 13), w, POWER)
	_text(Vector2(gx, 46), "READY -- press F" if w >= 1.0 else "%d%%" % int(w * 100),
		  POWER if w >= 1.0 else DIM, 10)

	# THE REACTOR, and how many bars are going spare.
	var rx := gx + 100.0
	var free := _i(ship, "power_free")
	var total := _i(ship, "power_total", 8)
	_text(Vector2(rx, 16), "REACTOR", DIM, 10)
	for i in range(total):
		var r := Rect2(rx + i * 11, 24, 8, 13)
		draw_rect(r, POWER if i < free else PANEL.darkened(0.3))
		draw_rect(r, EDGE, false, 1.0)
	_text(Vector2(rx, 46), "%d spare" % free, POWER if free > 0 else DIM, 10)

	# THEM. Name, hull, and the countdown -- which is the single most useful
	# number on this screen and did not exist until a playtest asked what was
	# going on with the enemy ship.
	var ex := size.x - 250.0
	var nose := Vector2(ex - 16, 30)
	var poly := PackedVector2Array([
		nose, nose + Vector2(20, -10), nose + Vector2(31, -5),
		nose + Vector2(31, 5), nose + Vector2(20, 10)])
	draw_colored_polygon(poly, BAD.darkened(0.3))
	draw_polyline(poly + PackedVector2Array([poly[0]]), BAD, 1.5)

	var ehull := _f(enemy, "hull")
	var emax: float = maxf(1.0, _f(enemy, "hull_max", 18.0))
	_text(Vector2(ex + 24, 16), str(enemy.get("name", "raider")), BAD, 12)
	_bar(Rect2(ex + 24, 24, 100, 13), ehull / emax, BAD)
	_text(Vector2(ex + 130, 34), "%d/%d" % [int(ehull), int(emax)], DIM, 11)

	var esh := _i(enemy, "shields")
	for i in range(_i(enemy, "shields_max", 2)):
		var c := Vector2(ex + 30 + i * 16, 46)
		draw_circle(c, 5, SHIELD if i < esh else PANEL.darkened(0.3))

	var fin := _f(enemy, "fires_in") / 10.0
	if fin < 0.0:
		_text(Vector2(ex + 90, 48), "GUN OUT", HULL, 11)
	else:
		_text(Vector2(ex + 90, 48), "fires in %.1fs" % fin,
			  FIRE if fin < 3.0 else DIM, 11)

	var clock := _i(ship, "clock")
	var paused := _yes(ship, "paused")
	if paused:
		_text(Vector2(size.x / 2.0 - 34, 46), "PAUSED", POWER, 12)
	else:
		_text(Vector2(size.x / 2.0 - 20, 46), "%d:%02d" % [clock / 60, clock % 60], DIM, 11)

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
		var hv := -1
		for n in range(rooms.size()):
			var m := _vent_mark(n)
			if m.size.x > 0 and m.grow(3).has_point(e.position):
				hv = n
		var h := _room_at(e.position)
		if h != hover_room or hv != hover_vent:
			hover_room = h
			hover_vent = hv
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

	# THE HATCH BEFORE ANYTHING ELSE, because it sits on the room's edge and
	# a click that lands on both should do the rarer, more deliberate thing.
	for n in range(rooms.size()):
		var m := _vent_mark(n)
		if m.size.x > 0 and m.grow(3).has_point(mb.position):
			var open := str((rooms[n] as Dictionary).get("vent", "shut")) == "open"
			act("vent %d %s" % [n, "shut" if open else "open"])
			return

	# A crew member first: clicking a person picks them up, clicking them
	# again puts them down. Hit-tested against where they were DRAWN, which is
	# the only position a player can be aiming at.
	for raw in crew:
		var c: Dictionary = raw
		if not _yes(c, "alive"):
			continue
		var nm := str(c.get("name", ""))
		var at := _crew_pos(nm)
		if at != Vector2.ZERO and mb.position.distance_to(at) < 14.0:
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
