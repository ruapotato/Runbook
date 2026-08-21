# gblocks.gd — the falling-blocks puzzle, with the two details that decide
# whether it feels like the real thing or like a clone somebody wrote in a day.
#
# The first is WALL KICKS. A piece that refuses to rotate because its corner is
# one column inside the wall is the single most common way these games feel
# wrong: you press the key, nothing happens, and there is no way to learn why.
# So a rotation that collides is retried at a short list of offsets -- sideways
# first, then up -- and only fails if every one of them collides. That is what
# makes tucking an S under an overhang possible instead of lucky.
#
# The second is the LOCK DELAY. If a piece welds to the stack the instant it
# lands, the last half-second of every placement is a race you lose. Here a
# landed piece gets LOCK seconds before it sets, and any move or rotation that
# succeeds resets that timer -- but only RESETS_MAX times, so you cannot hover
# a piece forever by spinning it.
#
# Randomness is a 7-bag: all seven shapes shuffled, dealt, reshuffled. Pure
# random hands out four S pieces in a row often enough to lose you a game you
# played correctly, and there is nothing to learn from that.
#
# Same contract as g2048.gd -- .new(), mono and machine set from outside, then
# take_focus(). All primitives; this project has no sprites and no audio.
#
# The high score goes to /root/.blocks through the machine's own shell, so
# `cat /root/.blocks` shows what the HUD shows.

extends Control

var mono: Font
var machine: Object = null

const COLS := 10
const ROWS := 20
const TOP := 40.0
const LOCK := 0.5
const RESETS_MAX := 12

# Each piece is its four rotations, written out as cell offsets. Rotation
# tables beat rotating a matrix at runtime: the offsets ARE the definition of
# how this game's pieces turn, and a reader can check them against a real board
# without simulating anything. Order: I, J, L, O, S, T, Z.
const SHAPES := [
	[[Vector2i(0,1),Vector2i(1,1),Vector2i(2,1),Vector2i(3,1)],
	 [Vector2i(2,0),Vector2i(2,1),Vector2i(2,2),Vector2i(2,3)],
	 [Vector2i(0,2),Vector2i(1,2),Vector2i(2,2),Vector2i(3,2)],
	 [Vector2i(1,0),Vector2i(1,1),Vector2i(1,2),Vector2i(1,3)]],
	[[Vector2i(0,0),Vector2i(0,1),Vector2i(1,1),Vector2i(2,1)],
	 [Vector2i(1,0),Vector2i(2,0),Vector2i(1,1),Vector2i(1,2)],
	 [Vector2i(0,1),Vector2i(1,1),Vector2i(2,1),Vector2i(2,2)],
	 [Vector2i(1,0),Vector2i(1,1),Vector2i(0,2),Vector2i(1,2)]],
	[[Vector2i(2,0),Vector2i(0,1),Vector2i(1,1),Vector2i(2,1)],
	 [Vector2i(1,0),Vector2i(1,1),Vector2i(1,2),Vector2i(2,2)],
	 [Vector2i(0,1),Vector2i(1,1),Vector2i(2,1),Vector2i(0,2)],
	 [Vector2i(0,0),Vector2i(1,0),Vector2i(1,1),Vector2i(1,2)]],
	[[Vector2i(1,0),Vector2i(2,0),Vector2i(1,1),Vector2i(2,1)],
	 [Vector2i(1,0),Vector2i(2,0),Vector2i(1,1),Vector2i(2,1)],
	 [Vector2i(1,0),Vector2i(2,0),Vector2i(1,1),Vector2i(2,1)],
	 [Vector2i(1,0),Vector2i(2,0),Vector2i(1,1),Vector2i(2,1)]],
	[[Vector2i(1,0),Vector2i(2,0),Vector2i(0,1),Vector2i(1,1)],
	 [Vector2i(1,0),Vector2i(1,1),Vector2i(2,1),Vector2i(2,2)],
	 [Vector2i(1,1),Vector2i(2,1),Vector2i(0,2),Vector2i(1,2)],
	 [Vector2i(0,0),Vector2i(0,1),Vector2i(1,1),Vector2i(1,2)]],
	[[Vector2i(1,0),Vector2i(0,1),Vector2i(1,1),Vector2i(2,1)],
	 [Vector2i(1,0),Vector2i(1,1),Vector2i(2,1),Vector2i(1,2)],
	 [Vector2i(0,1),Vector2i(1,1),Vector2i(2,1),Vector2i(1,2)],
	 [Vector2i(1,0),Vector2i(0,1),Vector2i(1,1),Vector2i(1,2)]],
	[[Vector2i(0,0),Vector2i(1,0),Vector2i(1,1),Vector2i(2,1)],
	 [Vector2i(2,0),Vector2i(1,1),Vector2i(2,1),Vector2i(1,2)],
	 [Vector2i(0,1),Vector2i(1,1),Vector2i(1,2),Vector2i(2,2)],
	 [Vector2i(1,0),Vector2i(0,1),Vector2i(1,1),Vector2i(0,2)]],
]

# Kick offsets tried in order when a rotation collides. One column each way is
# the common case (a piece against a wall or a tower); two columns is for the
# I piece, which is four long and needs the room; the upward kicks are what let
# a T spin into a slot with its floor already occupied.
const KICKS := [
	Vector2i(0, 0), Vector2i(-1, 0), Vector2i(1, 0), Vector2i(-2, 0),
	Vector2i(2, 0), Vector2i(0, -1), Vector2i(-1, -1), Vector2i(1, -1),
]

# Muted, distinguishable at a 12-pixel cell, and at home on a light desktop.
const COLOURS := [
	Color("#4a8f9c"), Color("#3f5fa0"), Color("#c08040"), Color("#c9a83f"),
	Color("#5f9a4f"), Color("#8a5fa8"), Color("#b5533f"),
]
const BG := Color("#f2f0ea")
const WELL := Color("#e3e0d6")
const GRID := Color("#d6d2c6")
const INK := Color("#2f2a24")
const FAINT := Color("#7d7468")
const GHOST := Color(0.18, 0.16, 0.14, 0.16)

# The standard clear table, scaled by level. A tetris is worth more than four
# singles by a factor of two, which is the entire reason to build a well.
const SCORE_TABLE := [0, 100, 300, 500, 800]

var well: Array = []         # ROWS x COLS, -1 empty else colour index
var bag: Array = []
var piece := -1
var rot := 0
var pos := Vector2i(3, 0)
var nxt := -1
var held := -1
var used_hold := false       # one hold per piece, or hold is a free undo
var score := 0
var best := 0
var lines := 0
var levelno := 1
var over := false
var paused := false
var fall := 0.0
var lock_t := -1.0           # < 0 means the piece is not resting on anything
var resets := 0
var rng := RandomNumberGenerator.new()


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	clip_contents = true
	rng.randomize()
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.blocks")
		if t.strip_edges().is_valid_int():
			best = int(t.strip_edges())
	_new_game()
	set_process(true)


func take_focus() -> void:
	grab_focus()


func _new_game() -> void:
	well = []
	for y in range(ROWS):
		var row := []
		for x in range(COLS):
			row.append(-1)
		well.append(row)
	bag = []
	held = -1
	used_hold = false
	score = 0
	lines = 0
	levelno = 1
	over = false
	paused = false
	fall = 0.0
	nxt = _draw_from_bag()
	_spawn()
	queue_redraw()


func _draw_from_bag() -> int:
	if bag.is_empty():
		bag = [0, 1, 2, 3, 4, 5, 6]
		bag.shuffle()
	return bag.pop_back()


func _spawn() -> void:
	piece = nxt
	nxt = _draw_from_bag()
	rot = 0
	pos = Vector2i(3, 0)
	used_hold = false
	lock_t = -1.0
	resets = 0
	# Game over is not "the well is full" -- it is "the new piece has nowhere to
	# stand". Checked here, once, at the only moment it can become true.
	if not _fits(piece, rot, pos):
		over = true
		if score > best:
			best = score
			if machine:
				machine.sh_on(0, 'echo "%d" > /root/.blocks' % best)


func _cells(p: int, r: int, at: Vector2i) -> Array:
	var out: Array = []
	for c in SHAPES[p][r % 4]:
		out.append(at + (c as Vector2i))
	return out


func _fits(p: int, r: int, at: Vector2i) -> bool:
	for c in _cells(p, r, at):
		var v: Vector2i = c
		if v.x < 0 or v.x >= COLS or v.y >= ROWS:
			return false
		if v.y >= 0 and well[v.y][v.x] != -1:
			return false
	return true


# Any successful action while the piece is resting buys it more time, up to a
# limit. Without the limit you can spin an S piece on top of the stack until
# the heat death of the desktop.
func _touch() -> void:
	if lock_t >= 0.0 and resets < RESETS_MAX:
		lock_t = 0.0
		resets += 1


func _shift(dx: int) -> bool:
	if _fits(piece, rot, pos + Vector2i(dx, 0)):
		pos += Vector2i(dx, 0)
		_touch()
		return true
	return false


func _rotate(dr: int) -> bool:
	var r := (rot + dr + 4) % 4
	for k in KICKS:
		if _fits(piece, r, pos + (k as Vector2i)):
			rot = r
			pos += (k as Vector2i)
			_touch()
			return true
	return false


func _drop_row() -> int:
	var y := pos.y
	while _fits(piece, rot, Vector2i(pos.x, y + 1)):
		y += 1
	return y


func _hard_drop() -> void:
	var y := _drop_row()
	score += 2 * (y - pos.y)
	pos.y = y
	_lock()


func _soft_drop() -> void:
	if _fits(piece, rot, pos + Vector2i(0, 1)):
		pos.y += 1
		score += 1
		fall = 0.0
	else:
		if lock_t < 0.0:
			lock_t = 0.0


func _hold() -> void:
	if used_hold:
		return
	var swap := held
	held = piece
	if swap < 0:
		# First hold of the game: the slot was empty, so the preview piece is
		# the one that comes out, and the bag refills the preview. Taking `nxt`
		# here rather than a fresh draw is what keeps the preview truthful.
		piece = nxt
		nxt = _draw_from_bag()
	else:
		piece = swap
	rot = 0
	pos = Vector2i(3, 0)
	lock_t = -1.0
	resets = 0
	used_hold = true
	if not _fits(piece, rot, pos):
		over = true


func _lock() -> void:
	for c in _cells(piece, rot, pos):
		var v: Vector2i = c
		if v.y >= 0:
			well[v.y][v.x] = piece
	var cleared := 0
	var y := ROWS - 1
	while y >= 0:
		var full := true
		for x in range(COLS):
			if well[y][x] == -1:
				full = false
				break
		if full:
			well.remove_at(y)
			var row := []
			for x in range(COLS):
				row.append(-1)
			well.insert(0, row)
			cleared += 1
			# Do not decrement y: the row that fell into this slot has not been
			# checked yet, and four-line clears depend on it being checked.
		else:
			y -= 1
	if cleared > 0:
		lines += cleared
		score += SCORE_TABLE[cleared] * levelno
		levelno = 1 + lines / 10
	_spawn()
	queue_redraw()


# Gravity per level, seconds per row. Levelling every ten lines means a player
# who clears singles gets fast without ever having built anything, which is the
# pressure that makes them stop doing that.
func _gravity() -> float:
	return max(0.05, 0.8 * pow(0.85, float(levelno - 1)))


func _process(dt: float) -> void:
	if over or paused:
		return
	if lock_t >= 0.0:
		lock_t += dt
		if lock_t >= LOCK:
			if _fits(piece, rot, pos + Vector2i(0, 1)):
				lock_t = -1.0      # something moved out from under it
			else:
				_lock()
				return
		queue_redraw()
	fall += dt
	if fall >= _gravity():
		fall = 0.0
		if _fits(piece, rot, pos + Vector2i(0, 1)):
			pos.y += 1
			if not _fits(piece, rot, pos + Vector2i(0, 1)):
				if lock_t < 0.0:
					lock_t = 0.0
		elif lock_t < 0.0:
			lock_t = 0.0
		queue_redraw()


func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and (e as InputEventMouseButton).pressed:
		grab_focus()
		return
	if not (e is InputEventKey) or not (e as InputEventKey).pressed:
		return
	var k := e as InputEventKey
	accept_event()
	if k.keycode == KEY_R:
		_new_game(); return
	if k.keycode == KEY_P:
		if not over:
			paused = not paused
			queue_redraw()
		return
	if over or paused:
		return
	match k.keycode:
		KEY_LEFT, KEY_A:  _shift(-1)
		KEY_RIGHT, KEY_D: _shift(1)
		KEY_DOWN, KEY_S:  _soft_drop()
		KEY_UP, KEY_X, KEY_W: _rotate(1)
		KEY_Z:            _rotate(-1)
		KEY_SPACE:        _hard_drop()
		KEY_C, KEY_SHIFT: _hold()
		_: return
	queue_redraw()


func _geom() -> Array:
	# The side panel needs room for a 4x4 preview and three lines of text; it
	# takes a fifth of the width, floored, so a 300px window still shows it.
	var panel: float = clamp(size.x * 0.24, 62.0, 110.0)
	var cell: float = min((size.x - panel - 22.0) / float(COLS),
		(size.y - TOP - 12.0) / float(ROWS))
	if cell < 3.0:
		return []
	var w := cell * COLS
	var h := cell * ROWS
	return [Vector2(10.0 + max(0.0, (size.x - panel - 20.0 - w) * 0.5),
		TOP + (size.y - TOP - 12.0 - h) * 0.5), cell, panel]


func _draw_cell(o: Vector2, cell: float, idx: int, alpha: float) -> void:
	var col: Color = COLOURS[idx]
	col.a = alpha
	draw_rect(Rect2(o + Vector2(1, 1), Vector2(cell - 2.0, cell - 2.0)), col)
	# A lighter top edge gives the block a face without a texture, and reads at
	# cell sizes down to about six pixels.
	draw_line(o + Vector2(1.5, 2.0), o + Vector2(cell - 1.5, 2.0),
		Color(1, 1, 1, 0.35 * alpha), max(1.0, cell * 0.12))


func _draw_mini(p: int, box: Rect2) -> void:
	if p < 0:
		return
	var cell: float = min(box.size.x, box.size.y) / 4.0
	var cs: Array = SHAPES[p][0]
	var minx := 9; var maxx := -9; var miny := 9; var maxy := -9
	for c in cs:
		var v: Vector2i = c
		minx = min(minx, v.x); maxx = max(maxx, v.x)
		miny = min(miny, v.y); maxy = max(maxy, v.y)
	var ox: float = box.position.x + (box.size.x - (maxx - minx + 1) * cell) * 0.5
	var oy: float = box.position.y + (box.size.y - (maxy - miny + 1) * cell) * 0.5
	for c in cs:
		var v: Vector2i = c
		_draw_cell(Vector2(ox + (v.x - minx) * cell, oy + (v.y - miny) * cell), cell, p, 1.0)


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	draw_string(mono, Vector2(10, 20), "blocks", HORIZONTAL_ALIGNMENT_LEFT, -1, 18, INK)
	draw_string(mono, Vector2(10, 34),
		"arrows move   Z/X rotate   space drops   C holds   P pauses   R restarts",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)

	var gm := _geom()
	if gm.is_empty():
		return
	var org: Vector2 = gm[0]
	var cell: float = gm[1]
	var panel: float = gm[2]
	draw_rect(Rect2(org, Vector2(cell * COLS, cell * ROWS)), WELL)
	if cell >= 9.0:
		for x in range(1, COLS):
			draw_line(org + Vector2(x * cell, 0), org + Vector2(x * cell, cell * ROWS),
				GRID, 1.0)
		for y in range(1, ROWS):
			draw_line(org + Vector2(0, y * cell), org + Vector2(cell * COLS, y * cell),
				GRID, 1.0)

	for y in range(ROWS):
		for x in range(COLS):
			if well[y][x] >= 0:
				_draw_cell(org + Vector2(x * cell, y * cell), cell, well[y][x], 1.0)

	if not over and piece >= 0:
		# The ghost is not decoration: at level ten the piece crosses the well
		# faster than you can count columns, and without it every placement is
		# an estimate.
		var gy := _drop_row()
		for c in _cells(piece, rot, Vector2i(pos.x, gy)):
			var v: Vector2i = c
			if v.y >= 0:
				draw_rect(Rect2(org + Vector2(v.x * cell + 1.0, v.y * cell + 1.0),
					Vector2(cell - 2.0, cell - 2.0)), GHOST)
		for c in _cells(piece, rot, pos):
			var v2: Vector2i = c
			if v2.y >= 0:
				_draw_cell(org + Vector2(v2.x * cell, v2.y * cell), cell, piece, 1.0)
	draw_rect(Rect2(org, Vector2(cell * COLS, cell * ROWS)), FAINT, false, 1.0)

	var px: float = min(size.x - panel - 6.0, org.x + cell * COLS + 8.0)
	var py := TOP + 4.0
	draw_string(mono, Vector2(px, py + 10), "next", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
	var pw: float = max(24.0, panel - 12.0)
	draw_rect(Rect2(px, py + 14, pw, pw * 0.8), WELL)
	_draw_mini(nxt, Rect2(px, py + 14, pw, pw * 0.8))
	var hy: float = py + 14 + pw * 0.8 + 12.0
	draw_string(mono, Vector2(px, hy), "hold", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
	draw_rect(Rect2(px, hy + 4, pw, pw * 0.8), WELL)
	_draw_mini(held, Rect2(px, hy + 4, pw, pw * 0.8))
	var ty: float = hy + 4 + pw * 0.8 + 16.0
	if ty < size.y - 8:
		draw_string(mono, Vector2(px, ty), "score", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
		draw_string(mono, Vector2(px, ty + 13), str(score), HORIZONTAL_ALIGNMENT_LEFT, -1, 13, INK)
		draw_string(mono, Vector2(px, ty + 29), "best %d" % best,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
		draw_string(mono, Vector2(px, ty + 43), "lines %d" % lines,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
		draw_string(mono, Vector2(px, ty + 57), "level %d" % levelno,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)

	if over or paused:
		var box := Rect2(org.x, org.y + cell * ROWS * 0.5 - 18.0, cell * COLS, 36.0)
		draw_rect(box, Color(0.95, 0.94, 0.91, 0.9))
		var msg := "stack out -- R to try again" if over else "paused -- P to resume"
		draw_string(mono, Vector2(box.position.x, box.position.y + 23.0), msg,
			HORIZONTAL_ALIGNMENT_CENTER, box.size.x, 12, INK)
