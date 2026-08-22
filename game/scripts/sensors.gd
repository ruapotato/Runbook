# sensors.gd — the other ship, and the only decision your gun offers.
#
# WHY THIS IS A SEPARATE WINDOW. The bridge is damage control: your rooms,
# your people, your fires. This is the opposite job -- it is where you decide
# what to do to THEM -- and putting both in one panel meant the enemy was two
# bars in a corner that nobody looked at while their hull was the only thing
# that could end the fight.
#
# THE DECISION. Their hull is what wins, which is exactly why aiming at it is
# usually wrong. Their shields are what stops you reaching it. Their weapons
# are what is killing you. Their engines are why a tenth of your shots go
# wide. Every one of those is a way of saying "survive longer instead of
# winning sooner", and choosing between them is what FTL's combat is made of.
#
# And, like everything else here, a click is a command: clicking their weapon
# room sends `fire weapons`, which is a line you can type and a line a script
# can send.
extends Control

const UiFont := preload("res://scripts/uifont.gd")

var api: RunbookApi
var mono: Font
var echo: Callable

var enemy: Dictionary = {}
var rooms: Array = []
var me: Dictionary = {}
var console: PackedStringArray = PackedStringArray()
var hover := -1

const BG     := Color("#0d1117")
const PANEL  := Color("#161d26")
const EDGE   := Color("#2b3a4a")
const INK    := Color("#c9d6e3")
const DIM    := Color("#66798c")
const POWER  := Color("#e0b642")
const SHIELD := Color("#4a8fd0")
const BAD    := Color("#c0453b")
const HULL   := Color("#4fa96b")
const FIRE   := Color("#e0662a")

func setup(a: RunbookApi, on_echo: Callable) -> void:
	api = a
	echo = on_echo
	mono = UiFont.mono()
	refresh()

func _i(d: Dictionary, k: String, dflt: int = 0) -> int: return int(str(d.get(k, dflt)))
func _f(d: Dictionary, k: String, dflt: float = 0.0) -> float: return float(str(d.get(k, dflt)))

func refresh() -> void:
	var e := api.objects(api.exec("enemy"))
	if not e.is_empty():
		enemy = e[0]
	rooms = api.objects(api.exec("enemy.rooms"))
	var s := api.objects(api.exec("status"))
	if not s.is_empty():
		me = s[0]
	queue_redraw()

func act(line: String) -> void:
	var resp := api.exec(line)
	console.append(line)
	if not api.ok(resp):
		var why := api.error_text(resp)
		console.append("  " + (why if why != "" else "refused"))
	while console.size() > 4:
		console.remove_at(0)
	if echo.is_valid():
		echo.call(line)
	refresh()

# ------------------------------------------------------------- geometry
# Their ship, drawn nose-on-left because it is pointing at you. Four rooms
# down its spine, big enough to be click targets rather than a list.
func _hull_rect() -> Rect2:
	var top := _head_h() + 22.0
	var bottom := size.y - 92.0
	# CAPPED, AND CENTRED IN WHAT IS LEFT. In a tall narrow window the rooms
	# stretched to the full height and the raider stopped reading as a ship at
	# all -- four columns of text with a red outline round them. A ship has
	# proportions; the space above and below it is just space.
	var avail: float = maxf(60.0, bottom - top)
	var h: float = minf(avail, maxf(120.0, size.x * 0.34))
	return Rect2(30, top + (avail - h) / 2.0, size.x - 60, h)

func _room_rect(n: int) -> Rect2:
	var h := _hull_rect()
	var count: int = maxi(1, rooms.size())
	var w := h.size.x / float(count)
	return Rect2(h.position.x + n * w + 4, h.position.y + 4, w - 8, h.size.y - 8)

func _room_at(p: Vector2) -> int:
	for i in range(rooms.size()):
		if _room_rect(i).has_point(p):
			return i
	return -1

# ------------------------------------------------------------- painting
func _text(at: Vector2, s: String, col: Color, px := 12) -> void:
	draw_string(mono, at, s, HORIZONTAL_ALIGNMENT_LEFT, -1, px, col)

func _bar(r: Rect2, frac: float, col: Color) -> void:
	draw_rect(r, PANEL)
	if frac > 0.0:
		draw_rect(Rect2(r.position, Vector2(r.size.x * clampf(frac, 0, 1), r.size.y)), col)
	draw_rect(r, EDGE, false, 1.0)

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	_draw_head()
	_draw_ship()
	_draw_foot()

# LAID OUT FROM MEASURED WIDTHS, LEFT TO RIGHT, WRAPPING.
#
# The first version put each field at a fixed x, which fits in a 900-pixel
# window and overlaps itself into unreadable mush in a 460-pixel one -- and a
# 460-pixel one is what you get when the bridge, the map and a terminal are
# also open, which is the arrangement this window is FOR. Panels are the one
# place where measuring is not optional.
var _hx := 0.0
var _hy := 0.0

func _hfield(w: float) -> Vector2:
	if _hx + w > size.x - 12.0:
		_hx = 12.0
		_hy += 30.0
	var at := Vector2(_hx, _hy)
	_hx += w + 14.0
	return at

func _head_h() -> float:
	# Two rows if it is narrow, one if it is wide. Asked of the same layout
	# code that draws it, so they cannot disagree.
	return 66.0 if size.x > 620.0 else 96.0

func _draw_head() -> void:
	var h := _head_h()
	draw_rect(Rect2(0, 0, size.x, h), PANEL)
	draw_line(Vector2(0, h), Vector2(size.x, h), EDGE, 1.0)

	_hx = 12.0
	_hy = 14.0

	var hull := _f(enemy, "hull")
	var hmax: float = maxf(1.0, _f(enemy, "hull_max", 18.0))
	var nm := str(enemy.get("name", "raider"))
	var at := _hfield(mono.get_string_size(nm, HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x)
	_text(at + Vector2(0, 12), nm, BAD, 13)

	at = _hfield(150.0)
	_bar(Rect2(at.x, at.y + 3, 100, 12), hull / hmax, BAD)
	_text(at + Vector2(106, 14), "hull %d" % int(hull), DIM, 11)

	var sh := _i(enemy, "shields")
	var shmax: int = maxi(sh, _i(enemy, "shields_max", 2))
	at = _hfield(maxf(60.0, shmax * 16.0))
	_text(at, "shields", DIM, 10)
	for i in range(shmax):
		var r := Rect2(at.x + i * 16, at.y + 4, 12, 12)
		draw_rect(r, SHIELD if i < sh else PANEL)
		draw_rect(r, EDGE, false, 1.0)

	at = _hfield(84.0)
	_text(at, "their gun", DIM, 10)
	_bar(Rect2(at.x, at.y + 4, 80, 12), _f(enemy, "charge") / 100.0, BAD)

	# THE COUNTDOWN, not a charge percentage. Same reason as the bridge: a
	# number you can act on beats a number you have to interpret.
	var fin := _f(enemy, "fires_in") / 10.0
	at = _hfield(112.0)
	if fin < 0.0:
		_text(at + Vector2(0, 12), "GUN OUT", HULL, 12)
	else:
		_text(at + Vector2(0, 12), "fires in %.1fs" % fin, FIRE if fin < 3.0 else DIM, 12)

	at = _hfield(92.0)
	_text(at + Vector2(0, 12), "evade %d%%" % _i(enemy, "evade"), DIM, 11)

	var w := _f(me, "weapon") / 100.0
	at = _hfield(84.0)
	_text(at, "your gun", DIM, 10)
	_bar(Rect2(at.x, at.y + 4, 80, 12), w, POWER if w >= 1.0 else DIM)

	_text(Vector2(12, h - 6),
		  "click a room to fire at it" if w >= 1.0 else "your gun is %d%% charged" % int(w * 100),
		  POWER if w >= 1.0 else DIM, 11)

func _draw_ship() -> void:
	var h := _hull_rect()
	# A blunt, ugly hull pointing back at you. It is a raider, not a cruiser.
	var body := PackedVector2Array([
		Vector2(h.position.x - 22, h.get_center().y),
		Vector2(h.position.x, h.position.y - 10),
		Vector2(h.end.x + 14, h.position.y - 4),
		Vector2(h.end.x + 22, h.get_center().y),
		Vector2(h.end.x + 14, h.end.y + 4),
		Vector2(h.position.x, h.end.y + 10),
	])
	draw_colored_polygon(body, PANEL.darkened(0.45))
	draw_polyline(body + PackedVector2Array([body[0]]), BAD.darkened(0.2), 2.0)

	# The bubble hugs the hull and stays inside the window. A ring wide
	# enough to look impressive is a ring that leaves through the side.
	var sh := _i(enemy, "shields")
	var rx: float = minf(h.size.x * 0.58, size.x * 0.44)
	var ry: float = minf(h.size.y * 0.80, (size.y - h.position.y - 40.0) * 0.5)
	for i in range(sh):
		var pts := PackedVector2Array()
		for k in range(49):
			var a := TAU * float(k) / 48.0
			pts.append(h.get_center() + Vector2(cos(a) * (rx + i * 8), sin(a) * (ry + i * 8)))
		draw_polyline(pts, Color(SHIELD.r, SHIELD.g, SHIELD.b, 0.45 - i * 0.12), 2.0)

	for i in range(rooms.size()):
		_draw_room(i)

func _draw_room(n: int) -> void:
	var r := _room_rect(n)
	var room: Dictionary = rooms[n]
	var cap := _i(room, "cap")
	var dmg := _i(room, "damage")
	var dead := cap > 0 and dmg >= cap

	draw_rect(r, PANEL.darkened(0.15) if not dead else Color("#2a1414"))
	draw_rect(r, INK if n == hover else EDGE, 2.0 if n == hover else 1.0)
	_text(r.position + Vector2(8, 17), str(room.get("name", "?")), INK if not dead else BAD, 12)

	# Damage as pips, the same shape as your own power bars, so a player who
	# has learned to read one can read the other without being told.
	for i in range(cap):
		var pr := Rect2(r.position.x + 8 + i * 15, r.position.y + r.size.y - 20, 11, 13)
		draw_rect(pr, BAD.darkened(0.4) if i >= cap - dmg else HULL)
		draw_rect(pr, EDGE, false, 1.0)
	if dead:
		_text(r.position + Vector2(8, r.size.y - 28), "OUT", BAD, 11)

	# What knocking it out actually buys you. Said plainly, because a player
	# who has to guess will always aim at the hull.
	var why := ""
	match str(room.get("system", "")):
		"shields": why = "in the way"
		"weapons": why = "killing you"
		"engines": why = "why you miss"
		_:         why = "ends the fight"
	_text(r.position + Vector2(8, 34), why, DIM, 10)

	if n == hover:
		var c := r.get_center()
		draw_arc(c, 16, 0, TAU, 32, POWER, 1.5)
		draw_line(c - Vector2(24, 0), c - Vector2(10, 0), POWER, 1.5)
		draw_line(c + Vector2(10, 0), c + Vector2(24, 0), POWER, 1.5)
		draw_line(c - Vector2(0, 24), c - Vector2(0, 10), POWER, 1.5)
		draw_line(c + Vector2(0, 10), c + Vector2(0, 24), POWER, 1.5)

func _draw_foot() -> void:
	var y := size.y - 86.0
	draw_rect(Rect2(0, y, size.x, 86), Color("#0a0e13"))
	draw_line(Vector2(0, y), Vector2(size.x, y), EDGE, 1.0)
	var cy := y + 18.0
	for line in console:
		var l := str(line)
		_text(Vector2(12, cy), ("" if l.begins_with("  ") else "> ") + l,
			  DIM if l.begins_with("  ") else HULL, 11)
		cy += 14
	if console.is_empty():
		_text(Vector2(12, cy), "clicking their weapon room sends:  fire weapons", DIM, 11)
		_text(Vector2(12, cy + 16), "the same line works in the terminal:  rb fire weapons", DIM, 11)
		_text(Vector2(12, cy + 32), "and in a script:  do(\"fire shields\")", HULL, 11)

# ---------------------------------------------------------- interaction
func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion:
		var h := _room_at(e.position)
		if h != hover:
			hover = h
			queue_redraw()
		return
	if not (e is InputEventMouseButton and e.pressed):
		return
	var mb := e as InputEventMouseButton
	if mb.button_index != MOUSE_BUTTON_LEFT:
		return
	var n := _room_at(mb.position)
	if n < 0:
		return
	var room: Dictionary = rooms[n]
	# Their unarmed hold has no system name, so `fire hull` is what that room
	# means -- and `fire hull` is the line somebody would type.
	var sys := str(room.get("system", "none"))
	act("fire " + ("hull" if sys == "none" else sys))
