# gsetris.gd — sand tetris. The pieces fall as tetrominoes; the moment one
# locks it stops being a piece and becomes eight-by-eight cells of coloured
# SAND, which then flows, slumps and fills the holes underneath it.
#
# That one change rewrites the whole game. You do not clear a row -- rows stop
# existing the instant the first piece dissolves. You clear by building a
# CONNECTED PATH OF ONE COLOUR FROM THE LEFT WALL TO THE RIGHT WALL. When such
# a path exists, the entire connected mass of that colour goes, and everything
# resting on it slumps into the hole, which is usually how the next bridge gets
# made. Stacking is no longer the skill; pouring is.
#
# The test for a bridge is exactly what it sounds like: a flood fill from every
# filled cell in column zero, expanding only into neighbours of the same colour,
# asking whether it ever reaches the last column. Components are disjoint by
# construction -- a cell belongs to exactly one same-colour component -- so one
# visited stamp covers all of them and the whole check touches only the cells
# that are actually attached to the left wall.
#
# The tetromino tables and the kick list are gblocks.gd's, scaled up by the
# block size, because a piece here is four BLOCKS of sand and its collision test
# is against the sand grid rather than a well of cells. The sand itself obeys
# the same rules as gsand.gd, including the alternating scan direction that
# stops every pile drifting one way.
#
# Same contract as g2048.gd: .new(), mono and machine set from outside, then
# take_focus(). All draw_rect / draw_line / draw_string. The best score goes to
# /root/.sandtris through the machine's own shell.

extends Control

var mono: Font
var machine: Object = null

# Eight cells to a block, ten blocks across, fifteen down. The block size is
# what decides how sandy the game feels: at 1 it is Tetris, at 8 a locked piece
# collapses into something you can genuinely pour.
const BLK := 8
const BCOLS := 10
const BROWS := 15
const GW := BLK * BCOLS          # 80 sand cells across
const GH := BLK * BROWS          # 120 down
const NCELL := GW * GH
const SHIFT := BLK / 2           # sideways step: half a block, for aim
const TOP := 40.0
const LOCK := 0.25               # seconds a landed piece gets before it sets
const RESETS_MAX := 10
const FLASH := 0.28              # how long a found bridge glows before it goes

# gblocks.gd's rotation tables, in block units. I, J, L, O, S, T, Z.
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

const KICKS := [
	Vector2i(0, 0), Vector2i(-1, 0), Vector2i(1, 0), Vector2i(-2, 0),
	Vector2i(2, 0), Vector2i(0, -1), Vector2i(-1, -1), Vector2i(1, -1),
]

# Four colours, not seven. A bridge needs one colour to reach both walls, and
# with seven you would be waiting all night. Two shades each, banded by row, so
# the sand has some depth to it and a settled row still merges into one rect.
const SANDC := [
	Color("#00000000"), Color("#00000000"),
	Color("#d9b451"), Color("#cba845"),          # amber
	Color("#4f8fc0"), Color("#4682b3"),          # blue
	Color("#5aa860"), Color("#4f9b55"),          # green
	Color("#c05f52"), Color("#b25448"),          # red
]
const NCOL := 4

const BG := Color("#f2f0ea")
const WELL := Color("#1b1a18")
const PANEL := Color("#e3e0d6")
const INK := Color("#2f2a24")
const FAINT := Color("#7d7468")

var grid := PackedByteArray()          # 0 empty, else 1..NCOL
var flash := PackedByteArray()         # 1 = part of a found bridge, going away
var seen := PackedByteArray()          # flood-fill visited stamps
var stack := PackedInt32Array()        # flood-fill stack, reused every check
var doomed := PackedInt32Array()       # cells the current flash will remove

var bag: Array = []
var piece := -1
var pcol := 1
var nxt := -1
var ncol := 1
var rot := 0
var pos := Vector2i(0, 0)              # in SAND cells, always a multiple of SHIFT
var over := false
var paused := false
var score := 0
var best := 0
var cleared := 0                       # sand cells removed, all game
var bridges := 0
var levelno := 1
var fallacc := 0.0
var lock_t := -1.0
var resets := 0
var soft := false
var flash_t := 0.0
var frame := 0
var vstamp := 0
var rng := RandomNumberGenerator.new()

var cell_px := 4.0
var org := Vector2.ZERO


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	clip_contents = true
	rng.randomize()
	grid.resize(NCELL)
	flash.resize(NCELL)
	seen.resize(NCELL)
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.sandtris")
		if t.strip_edges().is_valid_int():
			best = int(t.strip_edges())
	_new_game()
	set_process(true)


func take_focus() -> void:
	grab_focus()


func _new_game() -> void:
	for i in range(NCELL):
		grid[i] = 0
		flash[i] = 0
		seen[i] = 0
	vstamp = 0
	doomed = PackedInt32Array()
	bag = []
	score = 0
	cleared = 0
	bridges = 0
	levelno = 1
	over = false
	paused = false
	flash_t = 0.0
	nxt = _from_bag()
	ncol = rng.randi_range(1, NCOL)
	_spawn()
	queue_redraw()


func _from_bag() -> int:
	if bag.is_empty():
		bag = [0, 1, 2, 3, 4, 5, 6]
		bag.shuffle()
	return bag.pop_back()


func _spawn() -> void:
	piece = nxt
	pcol = ncol
	nxt = _from_bag()
	ncol = rng.randi_range(1, NCOL)
	rot = 0
	pos = Vector2i(3 * BLK, 0)
	fallacc = 0.0
	lock_t = -1.0
	resets = 0
	soft = false
	if not _fits(piece, rot, pos):
		over = true
		_save()


func _save() -> void:
	if score <= best:
		return
	best = score
	if machine:
		machine.sh_on(0, 'echo "%d" > /root/.sandtris' % best)


# ---------------------------------------------------------------- the piece

# A piece cell is a BLK x BLK square of sand cells. Collision is tested against
# the sand, cell by cell, which is what lets a piece rest on a slope.
func _fits(p: int, r: int, at: Vector2i) -> bool:
	for c in SHAPES[p][r % 4]:
		var o: Vector2i = at + (c as Vector2i) * BLK
		if o.x < 0 or o.x + BLK > GW or o.y + BLK > GH:
			return false
		if o.y + BLK <= 0:
			continue
		var y0: int = max(0, o.y)
		for y in range(y0, o.y + BLK):
			var rowbase := y * GW
			for x in range(o.x, o.x + BLK):
				if grid[rowbase + x] != 0:
					return false
	return true


func _touch() -> void:
	if lock_t >= 0.0 and resets < RESETS_MAX:
		lock_t = 0.0
		resets += 1


func _shift(dx: int) -> void:
	if _fits(piece, rot, pos + Vector2i(dx * SHIFT, 0)):
		pos.x += dx * SHIFT
		_touch()
		queue_redraw()


func _rotate(dr: int) -> void:
	var r := (rot + dr + 4) % 4
	for k in KICKS:
		var off: Vector2i = (k as Vector2i) * BLK
		if _fits(piece, r, pos + off):
			rot = r
			pos += off
			_touch()
			queue_redraw()
			return


func _drop_y() -> int:
	var y := pos.y
	while _fits(piece, rot, Vector2i(pos.x, y + 1)):
		y += 1
	return y


func _hard_drop() -> void:
	var y := _drop_y()
	score += (y - pos.y) / BLK
	pos.y = y
	_lock()


# The dissolve. Four blocks of solid colour are written straight into the sand
# grid and the piece ceases to exist; from the next frame the physics owns them.
func _lock() -> void:
	for c in SHAPES[piece][rot % 4]:
		var o: Vector2i = pos + (c as Vector2i) * BLK
		for y in range(max(0, o.y), o.y + BLK):
			var rowbase := y * GW
			for x in range(o.x, o.x + BLK):
				grid[rowbase + x] = pcol
	_spawn()
	queue_redraw()


# ---------------------------------------------------------------- the sand

# Powder physics, and the same alternation as gsand.gd: the scan direction and
# the order the two diagonals are tried both flip every frame, or every pile
# leans the way the loop runs.
func _sand_step() -> void:
	frame += 1
	var d := 1 if (frame & 1) == 0 else -1
	var y := GH - 2
	while y >= 0:
		var rowbase := y * GW
		var x := 0 if d > 0 else GW - 1
		while x >= 0 and x < GW:
			var i := rowbase + x
			var m := grid[i]
			if m != 0 and flash[i] == 0:
				var b := i + GW
				if grid[b] == 0:
					grid[b] = m; grid[i] = 0
				else:
					var xa := x + d
					if xa >= 0 and xa < GW and grid[b + d] == 0 and grid[i + d] == 0:
						grid[b + d] = m; grid[i] = 0
					else:
						var xb := x - d
						if xb >= 0 and xb < GW and grid[b - d] == 0 and grid[i - d] == 0:
							grid[b - d] = m; grid[i] = 0
			x += d
		y -= 1


# The bridge test. Flood from every filled cell in column zero, same colour
# only; if the flood ever touches the last column, the whole component goes.
# Same-colour components are disjoint, so one visited stamp does for all of them
# and nothing outside the left-wall-connected sand is ever touched.
func _find_bridges() -> void:
	vstamp += 1
	if vstamp > 250:
		vstamp = 1
		for i in range(NCELL):
			seen[i] = 0
	var found := PackedInt32Array()
	for sy in range(GH):
		var s := sy * GW
		var c := grid[s]
		if c == 0 or seen[s] == vstamp or flash[s] != 0:
			continue
		var comp := PackedInt32Array()
		stack.resize(0)
		stack.append(s)
		seen[s] = vstamp
		var reached := false
		while stack.size() > 0:
			var i: int = stack[stack.size() - 1]
			stack.resize(stack.size() - 1)
			comp.append(i)
			var x := i % GW
			if x == GW - 1:
				reached = true
			if x > 0 and grid[i - 1] == c and seen[i - 1] != vstamp:
				seen[i - 1] = vstamp; stack.append(i - 1)
			if x < GW - 1 and grid[i + 1] == c and seen[i + 1] != vstamp:
				seen[i + 1] = vstamp; stack.append(i + 1)
			if i >= GW and grid[i - GW] == c and seen[i - GW] != vstamp:
				seen[i - GW] = vstamp; stack.append(i - GW)
			if i + GW < NCELL and grid[i + GW] == c and seen[i + GW] != vstamp:
				seen[i + GW] = vstamp; stack.append(i + GW)
		if reached:
			found.append_array(comp)
	if found.is_empty():
		return
	# Found, but not gone yet: the mass glows for FLASH seconds so the player can
	# see the shape of the thing they built before it drops out from under
	# everything. It counts as solid while it glows.
	for i in found:
		flash[i] = 1
	doomed = found
	flash_t = FLASH
	bridges += 1
	# A wider mass is worth more than a bare thread across the floor, and the
	# level multiplier is why a long game is worth playing.
	score += found.size() * levelno
	cleared += found.size()
	levelno = 1 + cleared / 900


func _resolve_flash() -> void:
	for i in doomed:
		grid[i] = 0
		flash[i] = 0
	doomed = PackedInt32Array()
	_save()


# ---------------------------------------------------------------- loop

func _gravity() -> float:
	# Sand cells per second. One block is BLK cells, so level one is about one
	# and a half blocks a second.
	return min(90.0, 12.0 * pow(1.18, float(levelno - 1)))


func _process(dt: float) -> void:
	if over or paused:
		return
	if flash_t > 0.0:
		flash_t -= dt
		if flash_t <= 0.0:
			_resolve_flash()
		queue_redraw()
	_sand_step()
	# Checked every third frame, not every frame: sand takes many frames to
	# slump into a bridge and the flood fill is the most expensive thing here.
	if flash_t <= 0.0 and (frame % 3) == 0:
		_find_bridges()
	var speed := _gravity() * (10.0 if soft else 1.0)
	fallacc += dt * speed
	while fallacc >= 1.0:
		fallacc -= 1.0
		if _fits(piece, rot, pos + Vector2i(0, 1)):
			pos.y += 1
			if soft:
				score += 1
		else:
			if lock_t < 0.0:
				lock_t = 0.0
			break
	if lock_t >= 0.0:
		lock_t += dt
		if _fits(piece, rot, pos + Vector2i(0, 1)):
			lock_t = -1.0        # the sand moved out from under it
		elif lock_t >= LOCK:
			_lock()
	queue_redraw()


func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and (e as InputEventMouseButton).pressed:
		accept_event()
		grab_focus()
		return
	if not (e is InputEventKey):
		return
	var k := e as InputEventKey
	accept_event()
	if not k.pressed:
		if k.keycode == KEY_DOWN or k.keycode == KEY_S:
			soft = false
		return
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
		KEY_DOWN, KEY_S:  soft = true
		KEY_UP, KEY_X, KEY_W: _rotate(1)
		KEY_Z:            _rotate(-1)
		KEY_SPACE:        _hard_drop()
		_: return


# ---------------------------------------------------------------- drawing

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	draw_string(mono, Vector2(10, 20), "sandtris", HORIZONTAL_ALIGNMENT_LEFT, -1, 18, INK)
	draw_string(mono, Vector2(10, 34),
		"arrows move   Z/X rotate   space drops   join one colour wall to wall   P pauses   R restarts",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)

	var panel: float = clamp(size.x * 0.24, 62.0, 104.0)
	var availw: float = size.x - panel - 24.0
	var availh: float = size.y - TOP - 12.0
	if availw < 20.0 or availh < 20.0:
		return
	cell_px = min(availw / float(GW), availh / float(GH))
	if cell_px <= 0.0:
		return
	var bw := cell_px * GW
	var bh := cell_px * GH
	org = Vector2(10.0 + max(0.0, (availw - bw) * 0.5), TOP + (availh - bh) * 0.5)

	draw_rect(Rect2(org, Vector2(bw, bh)), WELL)
	_draw_sand()

	if not over:
		# The ghost matters more here than in blocks: the piece lands on a slope,
		# not a flat row, and the slope is what decides where the sand goes.
		var gy := _drop_y()
		# The ghost is the piece's own colour, faint: a neutral grey ghost on a
		# near-black well reads as a solid dark block, which is exactly the thing
		# a player must not mistake it for.
		var gc: Color = SANDC[pcol * 2]
		gc.a = 0.3
		for c in SHAPES[piece][rot % 4]:
			var o: Vector2i = Vector2i(pos.x, gy) + (c as Vector2i) * BLK
			draw_rect(Rect2(org + Vector2(o.x, o.y) * cell_px,
				Vector2(BLK, BLK) * cell_px), gc)
		for c2 in SHAPES[piece][rot % 4]:
			var o2: Vector2i = pos + (c2 as Vector2i) * BLK
			_draw_block(org + Vector2(o2.x, o2.y) * cell_px, BLK * cell_px, pcol)
	draw_rect(Rect2(org, Vector2(bw, bh)), FAINT, false, 1.0)

	_draw_panel(panel, bw)

	if over or paused:
		var box := Rect2(org.x, org.y + bh * 0.5 - 18.0, bw, 36.0)
		draw_rect(box, Color(0.95, 0.94, 0.91, 0.9))
		var msg := "buried -- R to try again" if over else "paused -- P to resume"
		draw_string(mono, Vector2(box.position.x, box.position.y + 23.0), msg,
			HORIZONTAL_ALIGNMENT_CENTER, box.size.x, 12, INK)


# One pass per row, merging equal colours into single rects, reading the grid
# directly -- the row band is constant across a row, so a run of equal colour is
# a run of equal cell value. The mass that is currently flashing is drawn after,
# straight from the doomed list, so the run-length pass never has to ask about
# it.
func _draw_sand() -> void:
	var w := cell_px
	var wq := w + 0.5
	for y in range(GH):
		var rowbase := y * GW
		var band := (y >> 1) & 1
		var yy := org.y + y * w
		var x0 := 0
		while x0 < GW:
			var m0 := grid[rowbase + x0]
			var x1 := x0 + 1
			while x1 < GW and grid[rowbase + x1] == m0:
				x1 += 1
			if m0 != 0:
				draw_rect(Rect2(org.x + x0 * w, yy, (x1 - x0) * w + 0.5, wq),
					SANDC[m0 * 2 + band])
			x0 = x1
	if flash_t <= 0.0:
		return
	# The found bridge, glowing, on top of whatever colour it used to be.
	var glow := Color("#fbf3d0")
	glow.a = clamp(flash_t / FLASH, 0.25, 1.0)
	for i in doomed:
		draw_rect(Rect2(org.x + float(i % GW) * w, org.y + float(i / GW) * w, wq, wq), glow)


func _draw_block(o: Vector2, s: float, c: int) -> void:
	draw_rect(Rect2(o, Vector2(s, s)), SANDC[c * 2])
	draw_line(o + Vector2(1.0, 1.5), o + Vector2(s - 1.0, 1.5),
		Color(1, 1, 1, 0.35), max(1.0, s * 0.08))


func _draw_panel(panel: float, bw: float) -> void:
	var px: float = min(size.x - panel - 4.0, org.x + bw + 10.0)
	draw_rect(Rect2(px - 4.0, TOP, panel, size.y - TOP - 12.0), PANEL)
	var py := TOP + 4.0
	draw_string(mono, Vector2(px, py + 10), "next", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
	var pwid: float = max(24.0, panel - 14.0)
	var box := Rect2(px, py + 14, pwid, pwid * 0.8)
	draw_rect(box, WELL)
	if nxt >= 0:
		var cs: Array = SHAPES[nxt][0]
		var minx := 9; var maxx := -9; var miny := 9; var maxy := -9
		for c in cs:
			var v: Vector2i = c
			minx = min(minx, v.x); maxx = max(maxx, v.x)
			miny = min(miny, v.y); maxy = max(maxy, v.y)
		var cs_px: float = min(box.size.x, box.size.y) / 4.0
		var ox: float = box.position.x + (box.size.x - (maxx - minx + 1) * cs_px) * 0.5
		var oy: float = box.position.y + (box.size.y - (maxy - miny + 1) * cs_px) * 0.5
		for c3 in cs:
			var v3: Vector2i = c3
			_draw_block(Vector2(ox + (v3.x - minx) * cs_px, oy + (v3.y - miny) * cs_px),
				cs_px, ncol)
	var ty: float = py + 14 + pwid * 0.8 + 18.0
	if ty > size.y - 8.0:
		return
	draw_string(mono, Vector2(px, ty), "score", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
	draw_string(mono, Vector2(px, ty + 14), str(score), HORIZONTAL_ALIGNMENT_LEFT, -1, 13, INK)
	draw_string(mono, Vector2(px, ty + 30), "best %d" % best,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
	draw_string(mono, Vector2(px, ty + 44), "bridges %d" % bridges,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
	draw_string(mono, Vector2(px, ty + 58), "level %d" % levelno,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
