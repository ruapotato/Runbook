# tactical.gd — the Map: what is happening out there.
#
# The bridge shows the inside of your ship and the sensors window shows the
# inside of theirs. This is the gap between them, and it exists because of one
# change to the model: a shot now takes about a second to cross.
#
# THAT SECOND IS THE WHOLE POINT. Volleys used to resolve in the instant they
# were fired, so the most dramatic thing in the game -- three shots in the air,
# aimed at a ship you are still deciding how to defend -- happened in no time
# at all and could not be drawn. Now you can watch them come, and every roll
# that decides what they do happens on arrival: moving somebody or dropping a
# shield while they are in flight still changes the outcome.
#
# So this window is not a diagram. It is a countdown you can act on.
extends Control

const UiFont := preload("res://scripts/uifont.gd")

var api: RunbookApi
var mono: Font

var me: Dictionary = {}
var enemy: Dictionary = {}
var shots: Array = []

const BG     := Color("#080b10")
const PANEL  := Color("#161d26")
const EDGE   := Color("#2b3a4a")
const INK    := Color("#c9d6e3")
const DIM    := Color("#66798c")
const POWER  := Color("#e0b642")
const SHIELD := Color("#4a8fd0")
const HULL   := Color("#4fa96b")
const BAD    := Color("#c0453b")
const FIRE   := Color("#e0662a")

func setup(a: RunbookApi) -> void:
	api = a
	mono = UiFont.mono()
	refresh()

func _i(d: Dictionary, k: String, dflt: int = 0) -> int: return int(str(d.get(k, dflt)))
func _f(d: Dictionary, k: String, dflt: float = 0.0) -> float: return float(str(d.get(k, dflt)))

func refresh() -> void:
	var s := api.objects(api.exec("status"))
	if not s.is_empty():
		me = s[0]
	var e := api.objects(api.exec("enemy"))
	if not e.is_empty():
		enemy = e[0]
	shots = api.objects(api.exec("shots"))
	queue_redraw()

func _text(at: Vector2, s: String, col: Color, px := 12) -> void:
	draw_string(mono, at, s, HORIZONTAL_ALIGNMENT_LEFT, -1, px, col)

func _bar(r: Rect2, frac: float, col: Color) -> void:
	draw_rect(r, PANEL)
	if frac > 0.0:
		draw_rect(Rect2(r.position, Vector2(r.size.x * clampf(frac, 0, 1), r.size.y)), col)
	draw_rect(r, EDGE, false, 1.0)

func _mine_x() -> float:  return size.x * 0.18
func _theirs_x() -> float: return size.x * 0.82
func _mid_y() -> float:   return size.y * 0.46

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	_draw_stars()
	_draw_ships()
	_draw_shots()
	_draw_foot()

# A FEW STARS, PLACED BY ARITHMETIC rather than by a random number generator.
# A starfield that moves every frame is a starfield that draws the eye away
# from three shots crossing the middle of the screen, which is the only thing
# in this window worth looking at.
func _draw_stars() -> void:
	for i in range(70):
		var x := fmod(float(i) * 137.508, 1.0) * size.x
		var y := fmod(float(i) * 71.317, 1.0) * size.y
		var b := 0.10 + fmod(float(i) * 0.37, 1.0) * 0.16
		draw_rect(Rect2(x, y, 1.5, 1.5), Color(1, 1, 1, b))

func _ship_poly(at: Vector2, facing: float, scale: float) -> PackedVector2Array:
	var p := PackedVector2Array([
		Vector2(1.5, 0), Vector2(0.2, -0.75), Vector2(-1.1, -0.6),
		Vector2(-1.3, 0), Vector2(-1.1, 0.6), Vector2(0.2, 0.75),
	])
	var out := PackedVector2Array()
	for v in p:
		out.append(at + Vector2(v.x * facing, v.y) * scale)
	return out

func _draw_ships() -> void:
	var y := _mid_y()
	var s: float = clampf(size.x * 0.035, 16.0, 34.0)

	var mine := _ship_poly(Vector2(_mine_x(), y), 1.0, s)
	draw_colored_polygon(mine, PANEL.darkened(0.2))
	draw_polyline(mine + PackedVector2Array([mine[0]]), HULL, 2.0)

	var theirs := _ship_poly(Vector2(_theirs_x(), y), -1.0, s)
	draw_colored_polygon(theirs, PANEL.darkened(0.35))
	draw_polyline(theirs + PackedVector2Array([theirs[0]]), BAD, 2.0)

	# Shield bubbles, one ring per layer, on both. This is the clearest place
	# in the game to see that a shot has to get through something first.
	_rings(Vector2(_mine_x(), y), s, _i(me, "shields"))
	_rings(Vector2(_theirs_x(), y), s, _i(enemy, "shields"))

	# ENGINE GLOW AS EVASION. Evasion is the one mechanic in this game that is
	# invisible in its effects -- a shot that misses just does not happen --
	# so it gets a shape here, and it is the shape of the thing that causes it.
	_wash(Vector2(_mine_x(), y), -1.0, s, _f(me, "evade") / 100.0, HULL)
	_wash(Vector2(_theirs_x(), y), 1.0, s, _f(enemy, "evade") / 100.0, BAD)

	_label(Vector2(_mine_x(), y + s + 26), str(me.get("ship", "Kestrel")),
		   _f(me, "hull") / maxf(1.0, _f(me, "hull_max", 16.0)),
		   _i(me, "evade"), _f(me, "weapon") / 100.0, HULL)
	_label(Vector2(_theirs_x(), y + s + 26), str(enemy.get("name", "raider")),
		   _f(enemy, "hull") / maxf(1.0, _f(enemy, "hull_max", 18.0)),
		   _i(enemy, "evade"), _f(enemy, "charge") / 100.0, BAD)

func _rings(at: Vector2, s: float, layers: int) -> void:
	for i in range(layers):
		draw_arc(at, s * (1.7 + i * 0.22), 0, TAU, 40,
				 Color(SHIELD.r, SHIELD.g, SHIELD.b, 0.5 - i * 0.12), 2.0)

func _wash(at: Vector2, dir: float, s: float, evade: float, col: Color) -> void:
	if evade <= 0.0:
		return
	var n := 1 + int(evade * 14.0)
	for i in range(n):
		var d := s * (1.35 + i * 0.16)
		draw_line(at + Vector2(dir * d, -s * 0.3), at + Vector2(dir * (d + s * 0.25), -s * 0.3),
				  Color(col.r, col.g, col.b, 0.5), 2.0)
		draw_line(at + Vector2(dir * d, s * 0.3), at + Vector2(dir * (d + s * 0.25), s * 0.3),
				  Color(col.r, col.g, col.b, 0.5), 2.0)

func _label(at: Vector2, name: String, hull: float, evade: int, charge: float, col: Color) -> void:
	var w := mono.get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x
	_text(at + Vector2(-w / 2.0, 0), name, col, 12)
	_bar(Rect2(at.x - 55, at.y + 8, 110, 10), hull, col)
	_text(at + Vector2(-55, 34), "evade %d%%" % evade, DIM, 10)
	_bar(Rect2(at.x + 5, at.y + 25, 50, 9), charge, POWER)

# THE SHOTS. Each one on its own lane so a volley of three reads as three
# things rather than one thick line, and each one drawn as a streak that
# points where it is going.
func _draw_shots() -> void:
	var y := _mid_y()
	var a := Vector2(_mine_x(), y)
	var b := Vector2(_theirs_x(), y)
	for raw in shots:
		var sh: Dictionary = raw
		var incoming := str(sh.get("from", "them")) == "them"
		var t: float = clampf(_f(sh, "across") / 100.0, 0.0, 1.0)
		var lane := _i(sh, "lane")
		var off := Vector2(0, (float(lane) - 1.0) * 16.0)
		var from := (b if incoming else a) + off
		var to := (a if incoming else b) + off
		var at := from.lerp(to, t)
		var dir := (to - from).normalized()
		var col := FIRE if incoming else POWER
		draw_line(at - dir * 16.0, at, Color(col.r, col.g, col.b, 0.55), 3.0)
		draw_circle(at, 3.5, col)

	if shots.is_empty():
		var msg := "nothing in the air"
		var w := mono.get_string_size(msg, HORIZONTAL_ALIGNMENT_LEFT, -1, 11).x
		_text(Vector2(size.x / 2.0 - w / 2.0, y - 34), msg, DIM.darkened(0.3), 11)

func _draw_foot() -> void:
	var y := size.y - 30.0
	draw_line(Vector2(0, y), Vector2(size.x, y), EDGE, 1.0)
	var incoming := 0
	for raw in shots:
		if str((raw as Dictionary).get("from", "")) == "them":
			incoming += 1
	if incoming > 0:
		_text(Vector2(12, y + 20), "%d INCOMING -- about a second" % incoming, FIRE, 12)
	elif bool(str(me.get("paused", "false")) == "true"):
		_text(Vector2(12, y + 20), "paused. thinking is free.", POWER, 11)
	else:
		_text(Vector2(12, y + 20), "quiet.", DIM, 11)
	var clock := _i(me, "clock")
	_text(Vector2(size.x - 70, y + 20), "%d:%02d" % [clock / 60, clock % 60], DIM, 11)
