# g2048.gd — 2048, because a desktop with no games on it is not a desktop.
#
# David asked for real apps to make the OS feel lived in and worth exploring.
# A game is the clearest test of whether this is a desktop or a diorama: it
# has to take keys, keep state and be actually playable, and none of that is
# faked by the OS underneath.
#
# It writes its high score to /root/.2048 through the machine's own shell, so
# the score survives, and `cat /root/.2048` shows it. Even the toy is honest.
#
# The tiles slide, because the player said so: "2048 works but needs animations
# of the blocks sliding." A board that teleports makes you re-read all sixteen
# cells after every key to work out what just happened; a board that slides
# tells you. So the grid is still the only state the RULES touch -- _slide,
# _move and _stuck are unchanged in what they compute -- and the animation is a
# separate, throwaway record of where each tile came from, rebuilt every move
# and thrown away when it lands. Nothing is animated by owning a position.

extends Control

var mono: Font
var machine: Object = null
var g := []                 # 4x4, 0 = empty -- still the whole game state
var score := 0
var best := 0
var over := false
var won := false
var rng := RandomNumberGenerator.new()

# The animation, which the rules know nothing about. `moving` holds EVERY
# non-zero tile of the board as it was before the move, with the cell it came
# from and the cell it went to, so during the slide the grid is not drawn at
# all -- these records are the board. A merge contributes two of them, both
# landing on the same cell, which is exactly what a merge looks like.
var moving: Array = []      # {"v", "from", "to", "merge"}, cells as Vector2(col, row)
var pops: Array = []        # cells that merged, for the bump when they land
var spawns: Array = []      # cells that just appeared, for the scale-in
var t_anim := 0.0           # seconds since the current move started
var anim_on := false
var queued := -1            # one keypress held over; see _gui_input

const N := 4
const TILE := Color("#eee4da")
const BOARD := Color("#bbada0")
const EMPTY := Color("#cdc1b4")
const SLIDE := 0.12         # tiles travel for this long, whatever the distance
const POP := 0.10           # then the merged ones bump and the new one arrives


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	rng.randomize()
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.2048")
		if t.strip_edges().is_valid_int():
			best = int(t.strip_edges())
	_new_game()
	set_process(true)


func take_focus() -> void:
	grab_focus()


func _new_game() -> void:
	g = []
	for i in range(N):
		g.append([0, 0, 0, 0])
	score = 0
	over = false
	won = false
	moving = []
	pops = []
	spawns = []
	queued = -1
	_spawn(); _spawn()
	# Start halfway down the timeline: nothing slid, but the two opening tiles
	# still get their scale-in, so a restart reads as a restart.
	t_anim = SLIDE
	anim_on = true
	queue_redraw()


# Nothing redraws while the board is at rest. A puzzle that repaints sixty
# times a second to show a static grid would be the desktop's biggest idle
# cost, and this window is often left open.
func _process(dt: float) -> void:
	if not anim_on:
		return
	t_anim += dt
	if t_anim >= SLIDE + POP:
		anim_on = false
	queue_redraw()
	# A held-over key goes in as soon as the SLIDE is done rather than waiting
	# out the pop, so hammering the arrows stays as fast as the slide allows.
	if t_anim >= SLIDE and queued >= 0:
		var d := queued
		queued = -1
		if not over:
			_do_move(d)


func _spawn() -> void:
	var free: Array = []
	for r in range(N):
		for c in range(N):
			if g[r][c] == 0:
				free.append([r, c])
	if free.is_empty():
		return
	var p: Array = free[rng.randi_range(0, free.size() - 1)]
	g[p[0]][p[1]] = 4 if rng.randf() < 0.1 else 2
	spawns.append(Vector2(p[1], p[0]))


# One row, slid left and merged. Returns the new row, the points scored, and
# where each tile came from: [[source index, destination index, value, merged]].
# The numbers are what they always were -- the third return exists only so the
# drawing code can animate a tile from where it was to where it went, and no
# rule reads it.
func _slide(rowv: Array) -> Array:
	var a: Array = []           # [value, index it came from], gaps removed
	for j in range(rowv.size()):
		if rowv[j] != 0:
			a.append([rowv[j], j])
	var out: Array = []
	var moves: Array = []
	var pts := 0
	var i := 0
	while i < a.size():
		var dst := out.size()
		if i + 1 < a.size() and a[i][0] == a[i + 1][0]:
			out.append(a[i][0] * 2)
			pts += a[i][0] * 2
			if a[i][0] * 2 == 2048:
				won = true
			# Both halves travel to the same cell, still showing their old
			# value; the doubled number appears when they get there.
			moves.append([a[i][1], dst, a[i][0], true])
			moves.append([a[i + 1][1], dst, a[i][0], true])
			i += 2
		else:
			out.append(a[i][0])
			moves.append([a[i][1], dst, a[i][0], false])
			i += 1
	while out.size() < N:
		out.append(0)
	return [out, pts, moves]


# The (row, column) that position i of line r lands on, for each direction.
# The rotation used to be written out four times for the read and four more
# for the write-back; the animation needs a fifth and sixth copy of it to map
# tile motions into board cells, so it lives in one place now.
func _cell(dir: int, r: int, i: int) -> Vector2i:
	match dir:
		1: return Vector2i(r, N - 1 - i)
		2: return Vector2i(i, r)
		3: return Vector2i(N - 1 - i, r)
	return Vector2i(r, i)


# dir 0 left, 1 right, 2 up, 3 down. Rotate into "left", slide, rotate back.
# Returns [did anything change, the tile motions to animate].
func _move(dir: int) -> Array:
	var before := str(g)
	var work: Array = []
	var motions: Array = []
	for r in range(N):
		var line: Array = []
		for i in range(N):
			var s := _cell(dir, r, i)
			line.append(g[s.x][s.y])
		var res := _slide(line)
		score += res[1]
		work.append(res[0])
		for mv in res[2]:
			var from := _cell(dir, r, mv[0])
			var to := _cell(dir, r, mv[1])
			motions.append({
				"v": mv[2],
				"from": Vector2(from.y, from.x),   # x is the column, y the row
				"to": Vector2(to.y, to.x),
				"merge": mv[3],
			})
	for r in range(N):
		for i in range(N):
			var d := _cell(dir, r, i)
			g[d.x][d.y] = work[r][i]
	return [str(g) != before, motions]


func _stuck() -> bool:
	for r in range(N):
		for c in range(N):
			if g[r][c] == 0:
				return false
			if c + 1 < N and g[r][c] == g[r][c + 1]:
				return false
			if r + 1 < N and g[r][c] == g[r + 1][c]:
				return false
	return true


func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and e.pressed:
		grab_focus()
		return
	if not (e is InputEventKey) or not e.pressed:
		return
	var k := e as InputEventKey
	accept_event()
	if k.keycode == KEY_R:
		_new_game(); return
	if over:
		return
	var d := -1
	match k.keycode:
		KEY_LEFT, KEY_A:  d = 0
		KEY_RIGHT, KEY_D: d = 1
		KEY_UP, KEY_W:    d = 2
		KEY_DOWN, KEY_S:  d = 3
	if d < 0:
		return
	# A KEY PRESSED MID-SLIDE IS QUEUED, NOT DROPPED, AND ONLY ONE OF THEM.
	# 2048 is played by hammering the arrows, and a key that does nothing reads
	# as a stuck board -- but an unbounded queue lets a held key keep playing
	# moves after you have let go, which reads as the board playing itself.
	# One deep is the honest middle. Either way the grid is never touched while
	# `moving` still describes it, so no half-applied state can exist.
	if anim_on and t_anim < SLIDE:
		queued = d
		return
	_do_move(d)


# The move, and the record of it. Same order as before -- slide, spawn, then
# ask whether that spawn ended the game -- because that order is the rule.
func _do_move(d: int) -> void:
	var res := _move(d)
	if not res[0]:
		return
	moving = res[1]
	pops = []
	for m in moving:
		if m["merge"]:
			pops.append(m["to"])
	spawns = []
	_spawn()
	if _stuck():
		over = true
		if score > best:
			best = score
			if machine:
				machine.sh_on(0, 'echo "%d" > /root/.2048' % best)
	t_anim = 0.0
	anim_on = true
	queue_redraw()


func _tile_colour(v: int) -> Color:
	match v:
		2:    return Color("#eee4da")
		4:    return Color("#ede0c8")
		8:    return Color("#f2b179")
		16:   return Color("#f59563")
		32:   return Color("#f67c5f")
		64:   return Color("#f65e3b")
		128:  return Color("#edcf72")
		256:  return Color("#edcc61")
		512:  return Color("#edc850")
		1024: return Color("#edc53f")
		_:    return Color("#edc22e")


# Top-left pixel of a cell. Takes a FRACTIONAL cell -- (1.5, 0) is halfway
# between the first two columns of the top row -- which is the whole trick: a
# sliding tile is drawn at a cell coordinate that happens to be between cells.
func _cell_at(cv: Vector2, cell: float, pad: float, top: float) -> Vector2:
	return Vector2(10.0 + pad + cv.x * (cell + pad), top + pad + cv.y * (cell + pad))


# One tile, scaled about its own centre so a pop or a scale-in does not shove
# the tile sideways in its slot.
func _draw_tile(pos: Vector2, cell: float, v: int, sc: float) -> void:
	var d: float = cell * max(0.02, sc)
	var o := pos + Vector2(cell - d, cell - d) * 0.5
	draw_rect(Rect2(o, Vector2(d, d)), _tile_colour(v))
	var col := Color("#776e65") if v <= 4 else Color("#f9f6f2")
	draw_string(mono, Vector2(o.x, o.y + d * 0.62), str(v),
		HORIZONTAL_ALIGNMENT_CENTER, d, int(clamp(d * 0.38, 10, 26)), col)


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), Color("#faf8ef"))
	draw_string(mono, Vector2(10, 20), "2048",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 18, Color("#776e65"))
	draw_string(mono, Vector2(size.x - 190, 20),
		"score %d    best %d" % [score, best],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, Color("#776e65"))
	draw_string(mono, Vector2(10, 36), "arrows or wasd, R restarts",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, Color("#a89a8f"))

	var pad := 6.0
	var top := 46.0
	var side: float = min(size.x - 20, size.y - top - 12)
	if side < 40:
		return
	var cell := (side - pad * (N + 1)) / N
	draw_rect(Rect2(10, top, side, side), BOARD)
	# The empty slots first, always all sixteen of them: a tile in flight is
	# between cells, so the hole it left has to be there to fly out of.
	for r in range(N):
		for c in range(N):
			draw_rect(Rect2(_cell_at(Vector2(c, r), cell, pad, top),
				Vector2(cell, cell)), EMPTY)

	if anim_on and t_anim < SLIDE:
		# Ease out: tiles leave fast and settle, which is what makes a 0.12s
		# slide readable instead of merely brief.
		var u: float = 1.0 - pow(1.0 - t_anim / SLIDE, 3.0)
		for m in moving:
			_draw_tile(_cell_at((m["from"] as Vector2).lerp(m["to"], u), cell, pad, top),
				cell, m["v"], 1.0)
	else:
		var pu := 1.0
		if anim_on:
			pu = clamp((t_anim - SLIDE) / POP, 0.0, 1.0)
		for r in range(N):
			for c in range(N):
				var v: int = g[r][c]
				if v == 0:
					continue
				var key := Vector2(c, r)
				var sc := 1.0
				if anim_on:
					if key in spawns:
						sc = pu                          # scale in from nothing
					elif key in pops:
						sc = 1.0 + 0.18 * sin(PI * pu)   # bump, and back to 1
				_draw_tile(_cell_at(key, cell, pad, top), cell, v, sc)
	if over:
		draw_rect(Rect2(10, top, side, side), Color(0.93, 0.89, 0.85, 0.72))
		draw_string(mono, Vector2(10, top + side / 2), "no moves left -- R to try again",
			HORIZONTAL_ALIGNMENT_CENTER, side, 14, Color("#776e65"))
	elif won:
		draw_string(mono, Vector2(10, top + side + 10), "2048! keep going.",
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color("#2f7a3f"))
