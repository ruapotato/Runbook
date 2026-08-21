# gmines.gd — minesweeper, and the one rule everybody fakes.
#
# The rule is: your first click is never a mine. Games that "fix" this by
# re-rolling the board until the clicked cell is clear are lying about the odds
# and still open with a 1 you have to guess from. The real version, and the one
# here, lays no mines at all until the first click lands, then places them in
# any cell outside the clicked cell AND its eight neighbours -- so the opening
# click always has a zero under it and always flood-fills into a real opening.
# The board is not generated and then patched; it does not exist yet.
#
# The other thing worth doing properly is the chord: clicking a revealed number
# whose flag count already matches it opens everything else around it. That is
# how minesweeper is actually played at speed, and a version without it is a
# version you click four hundred times.
#
# Same contract as g2048.gd -- the desktop does .new(), sets mono and machine,
# then take_focus(). Drawn entirely with primitives; there are no assets here.
#
# Best times go to /root/.mines through the machine's own shell, one number per
# difficulty on a single line, so `cat /root/.mines` reads "12 87 240" and the
# HUD shows the same seconds it does.

extends Control

var mono: Font
var machine: Object = null

# beginner, intermediate, expert -- the standard three, because their mine
# densities are the ones every solver technique was tuned against.
const LEVELS := [
	{"name": "beginner", "w": 9, "h": 9, "m": 10},
	{"name": "intermediate", "w": 16, "h": 16, "m": 40},
	{"name": "expert", "w": 30, "h": 16, "m": 99},
]
const TOP := 44.0

const BG := Color("#f2f0ea")
const HIDDEN := Color("#cfcabb")
const HIDDEN_HI := Color("#e0dcd0")
const OPEN := Color("#e8e5db")
const LINE := Color("#b3ad9d")
const INK := Color("#2f2a24")
const FAINT := Color("#7d7468")
const FLAG := Color("#b5462f")
const BOOM := Color("#d06a4a")
const NUMS := [
	Color("#2f2a24"), Color("#2f5fa8"), Color("#3f7a3a"), Color("#a8402f"),
	Color("#5a3fa8"), Color("#a86a2f"), Color("#2f8a8a"), Color("#5a5248"),
	Color("#7d7468"),
]

var level := 1
var W := 16
var H := 16
var MINES := 40
var mine: Array = []         # H rows of W bools
var shown: Array = []
var flag: Array = []
var near: Array = []         # adjacent mine count, computed once at placement
var laid := false            # have the mines been placed yet
var over := false
var won := false
var boom := Vector2i(-1, -1)
var elapsed := 0.0
var running := false
var best := [0, 0, 0]        # seconds, 0 meaning "never finished this one"
var hover := Vector2i(-1, -1)
var rng := RandomNumberGenerator.new()


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	clip_contents = true
	rng.randomize()
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.mines")
		var parts := t.strip_edges().split(" ", false)
		for i in range(min(3, parts.size())):
			if parts[i].is_valid_int():
				best[i] = int(parts[i])
	_new_game(level)
	set_process(true)


func take_focus() -> void:
	grab_focus()


func _new_game(lv: int) -> void:
	level = clampi(lv, 0, 2)
	var L: Dictionary = LEVELS[level]
	W = L["w"]; H = L["h"]; MINES = L["m"]
	mine = []; shown = []; flag = []; near = []
	for y in range(H):
		var a := []; var b := []; var c := []; var d := []
		for x in range(W):
			a.append(false); b.append(false); c.append(false); d.append(0)
		mine.append(a); shown.append(b); flag.append(c); near.append(d)
	laid = false
	over = false
	won = false
	boom = Vector2i(-1, -1)
	elapsed = 0.0
	running = false
	queue_redraw()


# Mines are laid AFTER the first click, avoiding that cell and its neighbours,
# which is what guarantees the opening is a zero and therefore an opening.
func _lay(safe: Vector2i) -> void:
	var pool: Array = []
	for y in range(H):
		for x in range(W):
			if absi(x - safe.x) <= 1 and absi(y - safe.y) <= 1:
				continue
			pool.append(Vector2i(x, y))
	pool.shuffle()
	var n: int = min(MINES, pool.size())
	for i in range(n):
		var p: Vector2i = pool[i]
		mine[p.y][p.x] = true
	for y in range(H):
		for x in range(W):
			var k := 0
			for dy in range(-1, 2):
				for dx in range(-1, 2):
					if dx == 0 and dy == 0:
						continue
					var nx := x + dx; var ny := y + dy
					if nx >= 0 and ny >= 0 and nx < W and ny < H and mine[ny][nx]:
						k += 1
			near[y][x] = k
	laid = true


func _process(dt: float) -> void:
	if running and not over:
		elapsed += dt
		queue_redraw()      # once a second's worth of digits would be enough,
		                    # but the timer is the only moving thing here and a
		                    # board this small costs less to repaint than to
		                    # track dirty rectangles for


# Flood fill, iterative. A recursive version blows the stack on a 30x16 expert
# board that opens most of itself in one click, and this is the exact click
# people make.
func _open(start: Vector2i) -> void:
	var stack: Array = [start]
	while not stack.is_empty():
		var p: Vector2i = stack.pop_back()
		if p.x < 0 or p.y < 0 or p.x >= W or p.y >= H:
			continue
		if shown[p.y][p.x] or flag[p.y][p.x]:
			continue
		shown[p.y][p.x] = true
		if mine[p.y][p.x]:
			boom = p
			_lose()
			return
		if near[p.y][p.x] == 0:
			for dy in range(-1, 2):
				for dx in range(-1, 2):
					if dx != 0 or dy != 0:
						stack.append(Vector2i(p.x + dx, p.y + dy))


func _flags_around(p: Vector2i) -> int:
	var k := 0
	for dy in range(-1, 2):
		for dx in range(-1, 2):
			if dx == 0 and dy == 0:
				continue
			var nx := p.x + dx; var ny := p.y + dy
			if nx >= 0 and ny >= 0 and nx < W and ny < H and flag[ny][nx]:
				k += 1
	return k


# The chord: a satisfied number opens its unflagged neighbours. If you flagged
# wrong, this is how you find out, and that is fair -- the information was on
# the board.
func _chord(p: Vector2i) -> void:
	if not shown[p.y][p.x] or near[p.y][p.x] == 0:
		return
	if _flags_around(p) != near[p.y][p.x]:
		return
	for dy in range(-1, 2):
		for dx in range(-1, 2):
			if dx == 0 and dy == 0:
				continue
			var q := Vector2i(p.x + dx, p.y + dy)
			if q.x < 0 or q.y < 0 or q.x >= W or q.y >= H:
				continue
			if not flag[q.y][q.x] and not shown[q.y][q.x]:
				_open(q)
				if over:
					return


func _flag_count() -> int:
	var k := 0
	for y in range(H):
		for x in range(W):
			if flag[y][x]:
				k += 1
	return k


# You win by revealing every safe cell. Flags are decoration -- a board can be
# fully solved with none placed at all, and marking the last mine is not a win.
func _check_win() -> void:
	for y in range(H):
		for x in range(W):
			if not mine[y][x] and not shown[y][x]:
				return
	won = true
	over = true
	running = false
	var secs := int(elapsed)
	if best[level] == 0 or secs < best[level]:
		best[level] = secs
		if machine:
			machine.sh_on(0, 'echo "%d %d %d" > /root/.mines' % [best[0], best[1], best[2]])


func _lose() -> void:
	over = true
	running = false
	for y in range(H):
		for x in range(W):
			if mine[y][x]:
				shown[y][x] = true


func _geom() -> Array:
	var cell: float = min((size.x - 16.0) / float(W), (size.y - TOP - 10.0) / float(H))
	if cell < 4.0:
		return []
	var w := cell * W
	var h := cell * H
	return [Vector2((size.x - w) * 0.5, TOP + (size.y - TOP - 10.0 - h) * 0.5), cell]


func _at(pos: Vector2) -> Vector2i:
	var gm := _geom()
	if gm.is_empty():
		return Vector2i(-1, -1)
	var org: Vector2 = gm[0]
	var cell: float = gm[1]
	var c := Vector2i(int(floor((pos.x - org.x) / cell)), int(floor((pos.y - org.y) / cell)))
	if c.x < 0 or c.y < 0 or c.x >= W or c.y >= H:
		return Vector2i(-1, -1)
	return c


func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseMotion:
		var h := _at((e as InputEventMouseMotion).position)
		if h != hover:
			hover = h
			queue_redraw()
		return
	if e is InputEventMouseButton and (e as InputEventMouseButton).pressed:
		grab_focus()
		var mb := e as InputEventMouseButton
		var c := _at(mb.position)
		if c.x < 0 or over:
			return
		if mb.button_index == MOUSE_BUTTON_RIGHT:
			if not shown[c.y][c.x]:
				flag[c.y][c.x] = not flag[c.y][c.x]
				queue_redraw()
			return
		if mb.button_index == MOUSE_BUTTON_MIDDLE:
			_chord(c)
			if not over:
				_check_win()
			queue_redraw()
			return
		if mb.button_index != MOUSE_BUTTON_LEFT:
			return
		if not laid:
			_lay(c)
			running = true
		if shown[c.y][c.x]:
			_chord(c)          # left-click on a number chords too, so you never
			                   # need a middle button you might not have
		elif not flag[c.y][c.x]:
			_open(c)
		if not over:
			_check_win()
		queue_redraw()
		return
	if not (e is InputEventKey) or not (e as InputEventKey).pressed:
		return
	var k := e as InputEventKey
	accept_event()
	match k.keycode:
		KEY_R: _new_game(level)
		KEY_1: _new_game(0)
		KEY_2: _new_game(1)
		KEY_3: _new_game(2)


func _fmt(secs: int) -> String:
	return "%d:%02d" % [secs / 60, secs % 60]


# A flag: pole plus a triangle. Drawn rather than written, because a glyph font
# that has the flag character is not a font this project can promise it has.
func _draw_flag(o: Vector2, cell: float) -> void:
	var p := o + Vector2(cell * 0.38, cell * 0.2)
	draw_line(p, p + Vector2(0, cell * 0.6), INK, max(1.0, cell * 0.07))
	var pts := PackedVector2Array([p, p + Vector2(cell * 0.3, cell * 0.14),
		p + Vector2(0, cell * 0.28)])
	draw_polygon(pts, PackedColorArray([FLAG, FLAG, FLAG]))


func _draw_mine(o: Vector2, cell: float) -> void:
	var c := o + Vector2(cell * 0.5, cell * 0.5)
	var r: float = cell * 0.24
	for a in range(4):
		var v := Vector2(cos(PI * a / 4.0), sin(PI * a / 4.0)) * r * 1.55
		draw_line(c - v, c + v, INK, max(1.0, cell * 0.06))
	draw_circle(c, r, INK)
	draw_circle(c - Vector2(r * 0.3, r * 0.35), r * 0.22, Color(1, 1, 1, 0.7))


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	draw_string(mono, Vector2(10, 20), "mines", HORIZONTAL_ALIGNMENT_LEFT, -1, 18, INK)
	var left: int = MINES - _flag_count()
	var bs := "--" if best[level] == 0 else _fmt(best[level])
	draw_string(mono, Vector2(size.x - 210, 20),
		"mines %d   %s   best %s" % [left, _fmt(int(elapsed)), bs],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, INK)
	draw_string(mono, Vector2(10, 34),
		"left opens, right flags, click a number to chord   1/2/3 size   R restarts",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)

	var gm := _geom()
	if gm.is_empty():
		draw_string(mono, Vector2(10, TOP + 20), "window too small for the %s board" %
			LEVELS[level]["name"], HORIZONTAL_ALIGNMENT_LEFT, -1, 11, FAINT)
		return
	var org: Vector2 = gm[0]
	var cell: float = gm[1]
	var fs: int = int(clamp(cell * 0.62, 7, 20))

	for y in range(H):
		for x in range(W):
			var o := org + Vector2(x * cell, y * cell)
			var r := Rect2(o + Vector2(1, 1), Vector2(cell - 1.0, cell - 1.0))
			if shown[y][x]:
				var bgc := OPEN
				if mine[y][x]:
					bgc = BOOM if Vector2i(x, y) == boom else OPEN
				draw_rect(r, bgc)
				if mine[y][x]:
					_draw_mine(o, cell)
				elif near[y][x] > 0:
					draw_string(mono, Vector2(o.x, o.y + cell * 0.5 + fs * 0.38),
						str(near[y][x]), HORIZONTAL_ALIGNMENT_CENTER, cell, fs,
						NUMS[near[y][x]])
			else:
				var hi := HIDDEN_HI if Vector2i(x, y) == hover and not over else HIDDEN
				draw_rect(r, hi)
				# A one-pixel light top-left edge is all the bevel a flat theme
				# wants; a full 3D border would fight the rest of the desktop.
				draw_line(o + Vector2(1, 1), o + Vector2(cell - 1.0, 1),
					Color(1, 1, 1, 0.5), 1.0)
				draw_line(o + Vector2(1, 1), o + Vector2(1, cell - 1.0),
					Color(1, 1, 1, 0.5), 1.0)
				if flag[y][x]:
					_draw_flag(o, cell)
	draw_rect(Rect2(org, Vector2(cell * W, cell * H)), LINE, false, 1.0)

	if over:
		var box := Rect2(org.x, org.y + cell * H * 0.5 - 18.0, cell * W, 36.0)
		draw_rect(box, Color(0.95, 0.94, 0.91, 0.88))
		var msg := "cleared in %s -- R for another" % _fmt(int(elapsed)) if won \
			else "that one was a mine -- R to try again"
		draw_string(mono, Vector2(box.position.x, box.position.y + 23.0), msg,
			HORIZONTAL_ALIGNMENT_CENTER, box.size.x, 13, INK)
