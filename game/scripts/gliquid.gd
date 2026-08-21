# gliquid.gd — a Liquid War clone: you do not command the fighters, only where
# they want to be.
#
# Liquid War's whole idea is that the controls are one point. You move a cursor;
# three hundred little fighters flow toward it, around walls, through each
# other, and wherever your fluid touches the enemy's, fighters get worn down and
# change sides. Nobody is issuing orders. The tactics come out of the shape of
# the map and the shape of the two blobs.
#
# The only clever part is how a fighter knows which way to go. Steering straight
# at the cursor walks the whole army into the nearest wall, so instead each side
# gets a distance field: a breadth-first flood from its cursor over the open
# cells, one integer per cell. A fighter then only has to look at its eight
# neighbours and step to the lowest number, which is a perfect shortest path
# around any obstacle for the cost of one flood per side per cursor move. That
# is the same trick the original used, and it is why the flow bends around a
# corner as a body instead of piling up on the wrong side of it.
#
# Everything is draw_rect, draw_line, draw_circle and draw_string, like the rest
# of the games here. The win tally lives in /root/.liquid, written through the
# machine's own shell, so `cat /root/.liquid` in the terminal agrees with the
# HUD.

extends Control

var mono: Font
var machine: Object = null

# The map is a fixed grid and the window is whatever size it is; everything
# below is in cell units and only the drawing knows about pixels. A resize
# therefore cannot disturb a match in progress.
const GW := 64
const GH := 40
const NCELL := GW * GH
const ARMY := 300                  # fighters per side
const MAXHP := 40.0
const REVIVE := 10.0               # hp a fighter is left with when it changes sides
const DPS := 26.0                  # damage per second, per attacker, capped below
const HEAL := 7.0                  # regeneration when nothing hostile is near
const SPEED := 9.0                 # cells / second
# HOW CLOSE IS CLOSE ENOUGH. Every fighter used to drive at the cursor POINT,
# which meant three hundred of them converged on one cell and stacked into a
# single overlapping dot -- "it's not a blob, it's just one overlapping dot".
# Inside this radius the drive falls away and the shove takes over, so the
# army spreads into a disc around the cursor and behaves like the liquid the
# game is named after.
const ATK_R := 0.9                 # cells; reach of a fighter
const FAR := 1 << 28               # BFS "unreachable"
const LIMIT := 180.0               # seconds; whoever is ahead at the bell wins
const TOP := 34.0                  # HUD strip
const FOOT := 16.0

# The eight neighbour offsets, and the same eight normalised, held as flat
# arrays. Everything in the per-fighter loop below is spelled out longhand --
# no _idx(), no _open(), no Vector2 built per neighbour -- because six hundred
# fighters times eight neighbours is five thousand calls a frame, and in
# GDScript the call overhead alone was costing more than the work inside.
const OX := [-1, 0, 1, -1, 1, -1, 0, 1]
const OY := [-1, -1, -1, 0, 0, 1, 1, 1]
const ONX := [-0.7071, 0.0, 0.7071, -1.0, 1.0, -0.7071, 0.0, 0.7071]
const ONY := [-0.7071, -1.0, -0.7071, 0.0, 0.0, 0.7071, 1.0, 0.7071]
# A fighter re-reads the flow field every third frame. The field is a map of
# where to walk, not a twitch reflex; at sixty frames a second nobody can see
# the fifty milliseconds, and it takes two thirds of the steering work away.
const FLOW_EVERY := 3
# An army converging on one cursor is not spread evenly: it bunches to eight or
# more fighters per cell, so the nine-bucket scan was walking seventy neighbours
# per fighter and the tick cost 20 ms. Two things fix that, and the second one
# is not just speed:
const ATK_R2 := ATK_R * ATK_R

const BG := Color("#f3f1ea")
const WALL := Color("#5c5750")
const WALL_TOP := Color("#7a746b")
const INK := Color("#2f2a24")
const FAINT := Color("#7d7468")
const C1 := Color("#2f6ea8")        # you
const C1_PALE := Color("#a9c4dc")
const C2 := Color("#b0512f")        # the computer
const C2_PALE := Color("#e0b09c")

# Parallel arrays rather than an array of dictionaries: six hundred fighters
# get touched several times a frame, and a dictionary lookup per field per
# fighter per frame is the difference between smooth and not.
# ONE FIGHTER PER CELL, WHICH IS WHAT MAKES IT LIQUID WAR.
#
# The first version had continuous positions and a soft shove between
# neighbours, which is a perfectly good way to write a flocking demo and is
# not this game: three hundred fighters converged on the cursor, interpenetrated
# because nothing actually stopped them, and became a single mangled ball with
# no edge. Both armies occupied the same space and every fighter had an enemy
# next to it, so the interior was not protected and the whole thing resolved
# in seconds.
#
# Liquid War is a GRID. A cell holds one fighter or nothing. A fighter steps
# into an empty neighbour that is closer to its cursor, and if the closest
# neighbour holds an ENEMY it attacks instead of moving. Everything the game
# is famous for falls out of those two rules:
#
#   * the mass has a hard edge, because a cell is occupied or it is not;
#   * only the surface fights, because an interior fighter has no enemy
#     adjacent to it -- which is the thing that was missing;
#   * the shapes flow round obstacles, because the only steering is "which
#     way is downhill".
#
# It is also less code than the physics it replaces.
var cell := PackedInt32Array()     # which cell each fighter is in
var hp := PackedFloat32Array()
var team := PackedByteArray()
var holder := PackedInt32Array()   # which fighter is in each cell, -1 for none
var move_t := PackedFloat32Array() # time owed, so fighters move at a speed

# `holder`, not `owner`: Control already has one, and shadowing it is a parse
# error rather than a subtle bug, which is the good kind of collision.
var frame := 0
var blocked := PackedByteArray()
var field0 := PackedInt32Array()   # distance-to-cursor flood, team 0
var field1 := PackedInt32Array()
var fcell := [-1, -1]              # cell each field was flooded from
var cursor := [Vector2.ZERO, Vector2.ZERO]
var pop := [0, 0]
var over := false
var winner := -1
var clock := 0.0
var wins := [0, 0]
var held := {}
var ai_t := 0.0
var ai_goal := Vector2.ZERO
var rng := RandomNumberGenerator.new()

func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	rng.randomize()
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.liquid")
		var p := t.strip_edges().split(" ", false)
		if p.size() >= 2 and p[0].is_valid_int() and p[1].is_valid_int():
			wins = [int(p[0]), int(p[1])]
	_new_game()
	set_process(true)


func take_focus() -> void:
	grab_focus()


# --------------------------------------------------------------------- map --

func _idx(cx: int, cy: int) -> int:
	return cy * GW + cx


func _open(cx: int, cy: int) -> bool:
	if cx < 0 or cy < 0 or cx >= GW or cy >= GH:
		return false
	return blocked[_idx(cx, cy)] == 0


# Rotationally symmetric obstacles: whatever is placed on the left half is
# placed again, turned 180 degrees, on the right. A Liquid War map that favours
# one side is just a bug you cannot see, and symmetry is a cheaper guarantee of
# fairness than any amount of playtesting.
func _make_map() -> void:
	for _try in range(24):
		blocked = PackedByteArray()
		blocked.resize(NCELL)
		blocked.fill(0)
		for x in range(GW):
			blocked[_idx(x, 0)] = 1
			blocked[_idx(x, GH - 1)] = 1
		for y in range(GH):
			blocked[_idx(0, y)] = 1
			blocked[_idx(GW - 1, y)] = 1
		for _b in range(rng.randi_range(5, 8)):
			var w := rng.randi_range(2, 4)
			var h := rng.randi_range(4, 14)
			if rng.randf() < 0.45:
				var s := w
				w = h
				h = s
			var bx := rng.randi_range(4, GW / 2 - w - 1)
			var by := rng.randi_range(2, GH - h - 2)
			for x in range(bx, bx + w):
				for y in range(by, by + h):
					blocked[_idx(x, y)] = 1
					blocked[_idx(GW - 1 - x, GH - 1 - y)] = 1
		# A clear pocket around each spawn, or the armies start inside a wall.
		for d in range(2):
			var c := _spawn_centre(d)
			for x in range(int(c.x) - 4, int(c.x) + 5):
				for y in range(int(c.y) - 4, int(c.y) + 5):
					if x > 0 and y > 0 and x < GW - 1 and y < GH - 1:
						blocked[_idx(x, y)] = 0
		# Reject any map where the two sides cannot reach each other: a match
		# that cannot be fought looks exactly like a frozen game.
		var probe := _flood(_cell_of(_spawn_centre(0)))
		if probe[_cell_of(_spawn_centre(1))] < FAR:
			return
	# Twenty-four tries failed, which means the generator is having a bad day.
	# An empty arena is a poor map but it is always playable.
	blocked.fill(0)
	for x in range(GW):
		blocked[_idx(x, 0)] = 1
		blocked[_idx(x, GH - 1)] = 1
	for y in range(GH):
		blocked[_idx(0, y)] = 1
		blocked[_idx(GW - 1, y)] = 1


func _spawn_centre(t: int) -> Vector2:
	return Vector2(7.5, GH * 0.5) if t == 0 else Vector2(GW - 8.5, GH * 0.5)


func _cell_of(p: Vector2) -> int:
	var cx := int(clamp(p.x, 0, GW - 1))
	var cy := int(clamp(p.y, 0, GH - 1))
	return _idx(cx, cy)


# Breadth-first flood over open cells from one start. Eight-way, but a diagonal
# is only allowed when both of its orthogonal neighbours are open, so nothing
# slips through the corner where two blocks touch.
func _flood(start: int) -> PackedInt32Array:
	var d := PackedInt32Array()
	d.resize(NCELL)
	d.fill(FAR)
	if blocked[start] == 1:
		# Cursor parked in a wall: flood from the nearest open cell instead, so
		# the army still has somewhere to go.
		var best := -1
		var bd := 1 << 30
		var sx := start % GW
		var sy := start / GW
		for i in range(NCELL):
			if blocked[i] == 0:
				var dx := i % GW - sx
				var dy := i / GW - sy
				var q := dx * dx + dy * dy
				if q < bd:
					bd = q
					best = i
		if best < 0:
			return d
		start = best
	var q2 := PackedInt32Array()
	q2.push_back(start)
	d[start] = 0
	var qi := 0
	while qi < q2.size():
		var c: int = q2[qi]
		qi += 1
		var cx := c % GW
		var cy := c / GW
		var nd: int = d[c] + 1
		for oy in range(-1, 2):
			for ox in range(-1, 2):
				if ox == 0 and oy == 0:
					continue
				var ax := cx + ox
				var ay := cy + oy
				if ax < 0 or ay < 0 or ax >= GW or ay >= GH:
					continue
				var ai := _idx(ax, ay)
				if blocked[ai] == 1 or d[ai] <= nd:
					continue
				if ox != 0 and oy != 0:
					if blocked[_idx(ax, cy)] == 1 or blocked[_idx(cx, ay)] == 1:
						continue
				d[ai] = nd
				q2.push_back(ai)
	return d


# ------------------------------------------------------------------- match --

func _new_game() -> void:
	_make_map()
	cell = PackedInt32Array()
	hp = PackedFloat32Array()
	team = PackedByteArray()
	move_t = PackedFloat32Array()
	holder = PackedInt32Array()
	holder.resize(NCELL)
	holder.fill(-1)

	# Fill outward from each side's spawn, so an army starts as a BLOB rather
	# than a scatter -- which is what it will be for the rest of the match, so
	# starting any other way is just a second of settling nobody watches.
	for t in range(2):
		var c0 := _cell_of(_spawn_centre(t))
		var seen := {}
		var queue: Array = [c0]
		seen[c0] = true
		var placed := 0
		while placed < ARMY and not queue.is_empty():
			var at: int = queue.pop_front()
			var ax: int = at % GW
			var ay: int = int(at / GW)
			if blocked[at] == 0 and holder[at] < 0:
				holder[at] = cell.size()
				cell.push_back(at)
				hp.push_back(MAXHP)
				team.push_back(t)
				move_t.push_back(rng.randf())
				placed += 1
			for k in range(8):
				var bx: int = ax + OX[k]
				var by: int = ay + OY[k]
				if bx < 0 or by < 0 or bx >= GW or by >= GH:
					continue
				var bc: int = by * GW + bx
				if seen.has(bc) or blocked[bc] == 1:
					continue
				seen[bc] = true
				queue.push_back(bc)

	cursor = [_spawn_centre(0), _spawn_centre(1)]
	fcell = [-1, -1]
	ai_goal = _spawn_centre(1)
	ai_t = 0.0
	pop = [ARMY, ARMY]
	clock = 0.0
	frame = 0
	over = false
	winner = -1


func _save() -> void:
	if machine:
		machine.sh_on(0, 'echo "%d %d" > /root/.liquid' % [wins[0], wins[1]])


func _process(dt: float) -> void:
	if over:
		return
	var h: float = min(dt, 0.05)
	clock += h
	frame += 1
	_move_cursors(h)
	_refresh_fields()
	_step(h)
	_count()
	# Two armies that both chase the other's centre of mass grind against each
	# other and trade fighters one for one: a test match sat at 304 against 296
	# after a full two and a half minutes. So, as in the original, the clock is
	# part of the win condition -- wipe them out, or be ahead when it runs out.
	if pop[0] == 0 or pop[1] == 0 or clock >= LIMIT:
		over = true
		if pop[0] == pop[1]:
			winner = -1
		else:
			winner = 0 if pop[0] > pop[1] else 1
		if winner >= 0:
			wins[winner] += 1
			_save()
	queue_redraw()


# THE TICK. Every fighter that has earned a step takes one.
#
# Acting in a rotating order matters: a fixed order means the fighter with the
# lowest index always gets the cell first, and a mass that always resolves the
# same way develops a grain -- it flows better one direction than the other,
# which you can see and cannot explain.
func _refresh_fields() -> void:
	# Only re-flood when a cursor has actually crossed into another cell.
	# Otherwise this would be two floods a frame for no change in the answer.
	var c0 := _cell_of(cursor[0])
	if c0 != fcell[0]:
		field0 = _flood(c0)
		fcell[0] = c0
	var c1 := _cell_of(cursor[1])
	if c1 != fcell[1]:
		field1 = _flood(c1)
		fcell[1] = c1


func _step(dt: float) -> void:
	var wall := blocked
	var speed := SPEED
	var n := cell.size()

	# Rotate who goes first.
	var start := frame % maxi(1, n)

	for idx in range(n):
		var i: int = (start + idx) % n
		move_t[i] += dt * speed
		if move_t[i] < 1.0:
			continue
		move_t[i] -= 1.0

		var t: int = team[i]
		var c: int = cell[i]
		var cx: int = c % GW
		var cy: int = int(c / GW)
		var f: PackedInt32Array = field0 if t == 0 else field1
		var here: int = f[c]

		# The best step downhill, and what is in the way. An enemy in the best
		# cell is a fight; an ally is a queue.
		var best := here
		var best_c := -1
		var foe := -1
		for k in range(8):
			var ax: int = cx + OX[k]
			var ay: int = cy + OY[k]
			if ax < 0 or ay < 0 or ax >= GW or ay >= GH:
				continue
			var ac: int = ay * GW + ax
			if wall[ac] == 1:
				continue
			# No squeezing through a corner join, same as any grid game.
			if OX[k] != 0 and OY[k] != 0:
				if wall[cy * GW + ax] == 1 or wall[ay * GW + cx] == 1:
					continue
			var dv: int = f[ac]
			if dv >= FAR or dv >= best:
				continue
			var occ: int = holder[ac]
			if occ >= 0 and team[occ] != t:
				# An enemy downhill. Remember the closest one; keep looking in
				# case there is an empty cell that is closer still, because
				# advancing beats fighting when you can.
				if foe < 0:
					foe = occ
				continue
			if occ >= 0:
				continue                      # an ally: wait for them to move
			best = dv
			best_c = ac

		if best_c >= 0:
			holder[c] = -1
			holder[best_c] = i
			cell[i] = best_c
			continue

		if foe >= 0:
			# ATTACK. Only the surface can reach this line: an interior
			# fighter has allies on every side and never finds a foe.
			hp[foe] -= DPS / speed
			if hp[foe] <= 0.0:
				# Converted, not killed -- the loser joins the winner weak,
				# which is what makes a won engagement snowball and what
				# Liquid War actually does.
				team[foe] = t
				hp[foe] = REVIVE
			continue

		# Nowhere to go: heal. A fighter safe inside the mass comes back.
		hp[i] = minf(MAXHP, hp[i] + HEAL / speed)


func _count() -> void:
	var a := 0
	for i in range(team.size()):
		if team[i] == 0:
			a += 1
	pop = [a, team.size() - a]


# ------------------------------------------------------------------ player --

func _move_cursors(dt: float) -> void:
	# WASD nudges the cursor for anyone who would rather not hold the mouse in
	# the window; the mouse sets it outright in _gui_input.
	var d := Vector2.ZERO
	if held.get(KEY_W, false) or held.get(KEY_UP, false):
		d.y -= 1.0
	if held.get(KEY_S, false) or held.get(KEY_DOWN, false):
		d.y += 1.0
	if held.get(KEY_A, false) or held.get(KEY_LEFT, false):
		d.x -= 1.0
	if held.get(KEY_D, false) or held.get(KEY_RIGHT, false):
		d.x += 1.0
	if d != Vector2.ZERO:
		cursor[0] = _clamp_cursor(cursor[0] + d.normalized() * 22.0 * dt)
	_ai_cursor(dt)


func _clamp_cursor(p: Vector2) -> Vector2:
	return Vector2(clamp(p.x, 0.5, GW - 0.5), clamp(p.y, 0.5, GH - 0.5))


# The computer plays the only two moves this game really has: go where the
# enemy is thin, and get out of where you are being beaten.
#
# It scores the map in coarse blocks rather than cells, because a cursor that
# reacts to individual fighters twitches, and a twitching cursor tears its own
# army into ribbons. It re-decides twice a second and then walks the cursor
# there at a limited speed, so its blob arrives as a blob.
const BW := 8
const BH := 5


func _ai_cursor(dt: float) -> void:
	ai_t -= dt
	if ai_t <= 0.0:
		ai_t = 0.5
		var mine := []
		var theirs := []
		mine.resize(BW * BH); theirs.resize(BW * BH)
		mine.fill(0.0); theirs.fill(0.0)
		for i in range(cell.size()):
			var bx := int(clamp(float(cell[i] % GW) / GW * BW, 0, BW - 1))
			var by := int(clamp(float(int(cell[i] / GW)) / GH * BH, 0, BH - 1))
			var w: float = hp[i] / MAXHP        # a hurt fighter is worth less
			if team[i] == 1:
				mine[by * BW + bx] += w
			else:
				theirs[by * BW + bx] += w
		# Attack unless the whole army is losing. An earlier version required
		# local superiority before it would move on a block, which sounds
		# prudent and is in fact a deadlock: at the start it has nobody near the
		# enemy, so no block ever qualifies, and it sat in its corner for the
		# full 150 seconds of a test match without a single fighter changing
		# sides. You have to walk over there to be strong over there.
		var attack: bool = float(pop[1]) >= 0.85 * float(pop[0])
		var best := -1.0e18
		var goal := ai_goal
		var rally := Vector2.ZERO
		var rw := 0.0
		for by2 in range(BH):
			for bx2 in range(BW):
				# Strength "nearby" is the block plus its neighbours: the fight
				# in one block is decided by who can walk into it.
				var m := 0.0
				var e := 0.0
				for oy in range(-1, 2):
					for ox in range(-1, 2):
						var xx := bx2 + ox
						var yy := by2 + oy
						if xx < 0 or yy < 0 or xx >= BW or yy >= BH:
							continue
						m += mine[yy * BW + xx]
						e += theirs[yy * BW + xx]
				var c := Vector2((bx2 + 0.5) / BW * GW, (by2 + 0.5) / BH * GH)
				if m > rw:
					rw = m
					rally = c
				# The candidate has to have enemies standing in it, not merely
				# next to it. Scoring on the neighbourhood alone picked the
				# empty block beside their blob -- the thinnest place near the
				# enemy is the place the enemy is not -- and the computer then
				# parked eight cells short of contact and stood there for the
				# rest of the match.
				if theirs[by2 * BW + bx2] <= 0.0 or not attack:
					continue
				# Of the places the enemy is, go for the thin one, and prefer
				# the ones near enough that the army arrives as an army. The
				# minus sign on e is the whole tactic: a weak cluster is a meal,
				# a strong one is a wall.
				var sc := m - 1.2 * e - c.distance_to(cursor[1]) * 0.15
				if sc > best:
					best = sc
					goal = c
		if best <= -1.0e17:
			# Losing overall, or the enemy is nowhere it can see: fall back on
			# its own largest mass. That pulls the stragglers home into one blob
			# instead of feeding them to the front a few at a time, and fighters
			# with nobody hostile beside them heal.
			goal = rally if rw > 0.0 else cursor[1]
		ai_goal = goal

	var to: Vector2 = ai_goal - cursor[1]
	# ELEVEN, NOT TWENTY. The computer's cursor moved at more than twice its
	# own army's speed, so it crossed the whole map in three seconds and the
	# battle was decided at the player's spawn before the player had moved.
	# A commander can outpace their army a little. Not by double.
	var step := 11.0 * dt
	if to.length() <= step:
		cursor[1] = _clamp_cursor(ai_goal)
	else:
		cursor[1] = _clamp_cursor(cursor[1] + to.normalized() * step)


func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and e.pressed:
		grab_focus()
		accept_event()
		return
	if e is InputEventMouseMotion:
		var g := _to_grid((e as InputEventMouseMotion).position)
		if g.x > -900.0:
			cursor[0] = _clamp_cursor(g)
		return
	if not (e is InputEventKey):
		return
	var k := e as InputEventKey
	if k.pressed and k.keycode == KEY_R:
		accept_event()
		_new_game()
		return
	match k.keycode:
		KEY_W, KEY_A, KEY_S, KEY_D, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT:
			held[k.keycode] = k.pressed
			accept_event()


# ------------------------------------------------------------------ render --

func _cell_px() -> float:
	return min(size.x / float(GW), (size.y - TOP - FOOT) / float(GH))


func _origin() -> Vector2:
	var c := _cell_px()
	return Vector2((size.x - GW * c) * 0.5, TOP + (size.y - TOP - FOOT - GH * c) * 0.5)


func _to_grid(p: Vector2) -> Vector2:
	var c := _cell_px()
	if c <= 0.0:
		return Vector2(-999, -999)
	var o := _origin()
	return Vector2((p.x - o.x) / c, (p.y - o.y) / c)


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	var c := _cell_px()
	if c < 1.0:
		draw_string(mono, Vector2(6, 20), "too small", HORIZONTAL_ALIGNMENT_LEFT, -1, 11, FAINT)
		return
	var o := _origin()
	draw_rect(Rect2(o, Vector2(GW * c, GH * c)), Color("#e7e3d8"))

	# Walls. Runs of blocked cells are merged along each row so a solid block is
	# a handful of rectangles rather than one per cell.
	for y in range(GH):
		var x := 0
		while x < GW:
			if blocked[_idx(x, y)] == 0:
				x += 1
				continue
			var x2 := x
			while x2 < GW and blocked[_idx(x2, y)] == 1:
				x2 += 1
			draw_rect(Rect2(o + Vector2(x * c, y * c), Vector2((x2 - x) * c, c)), WALL)
			if y == 0 or blocked[_idx(x, y - 1)] == 0:
				draw_line(o + Vector2(x * c, y * c + 1.0),
					o + Vector2(x2 * c, y * c + 1.0), WALL_TOP, 1.0)
			x = x2

	# A FIGHTER IS ITS CELL, drawn edge to edge. That is what gives the mass a
	# hard boundary and makes the shape readable at a glance: a filled region
	# with a clean front, not a cloud of dots you have to squint at to find
	# the edge of.
	for i in range(cell.size()):
		var f: float = clamp(hp[i] / MAXHP, 0.0, 1.0)
		var col: Color = C1_PALE.lerp(C1, f) if team[i] == 0 else C2_PALE.lerp(C2, f)
		var gx: float = float(cell[i] % GW)
		var gy: float = float(int(cell[i] / GW))
		draw_rect(Rect2(o.x + gx * c, o.y + gy * c, c + 0.5, c + 0.5), col)

	for t in range(2):
		var p: Vector2 = o + cursor[t] * c
		var col2: Color = C1 if t == 0 else C2
		draw_circle(p, max(3.0, c * 1.1), Color(col2.r, col2.g, col2.b, 0.22))
		draw_line(p + Vector2(-c, 0), p + Vector2(c, 0), col2, 1.5)
		draw_line(p + Vector2(0, -c), p + Vector2(0, c), col2, 1.5)

	_draw_hud()


func _draw_hud() -> void:
	draw_rect(Rect2(0, 0, size.x, TOP), Color(1, 1, 1, 0.8))
	draw_string(mono, Vector2(8, 15), "gliquid", HORIZONTAL_ALIGNMENT_LEFT, -1, 14, INK)
	var hint := "R restarts"
	if size.x >= 560.0:
		hint = "mouse or wasd moves your cursor -- the army follows.   R restarts"
	draw_string(mono, Vector2(8, 29), hint, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)

	# One bar, split where the two populations meet: the score and the shape of
	# the match in the same object.
	var bw: float = min(240.0, max(80.0, size.x * 0.34))
	var bx: float = size.x - bw - 8.0
	var total: float = float(max(1, pop[0] + pop[1]))
	draw_rect(Rect2(bx, 6, bw, 12), C2)
	draw_rect(Rect2(bx, 6, bw * float(pop[0]) / total, 12), C1)
	draw_rect(Rect2(bx, 6, bw, 12), Color(0, 0, 0, 0.25), false, 1.0)
	draw_string(mono, Vector2(bx, 30), "you %d" % pop[0],
		HORIZONTAL_ALIGNMENT_LEFT, bw, 10, C1)
	draw_string(mono, Vector2(bx, 30), "%d computer" % pop[1],
		HORIZONTAL_ALIGNMENT_RIGHT, bw, 10, C2)

	var left: int = int(max(0.0, LIMIT - clock))
	draw_string(mono, Vector2(0, 15), "%d:%02d" % [left / 60, left % 60],
		HORIZONTAL_ALIGNMENT_CENTER, size.x, 12, INK if left > 20 else C2)

	if over:
		var t := "a draw"
		if winner == 0:
			t = "you win"
		elif winner == 1:
			t = "the computer wins"
		draw_rect(Rect2(0, size.y / 2 - 30, size.x, 60), Color(0.98, 0.97, 0.94, 0.9))
		draw_string(mono, Vector2(0, size.y / 2 + 2), t + " -- R for another",
			HORIZONTAL_ALIGNMENT_CENTER, size.x, 15, INK)
		draw_string(mono, Vector2(0, size.y / 2 + 22),
			"match %d - %d, kept in /root/.liquid" % [wins[0], wins[1]],
			HORIZONTAL_ALIGNMENT_CENTER, size.x, 10, FAINT)
