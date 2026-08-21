# gsand.gd — the falling-sand toy, which is a cellular automaton wearing a
# paintbrush.
#
# There is one bug that every sand game gets, and it is worth naming up front:
# if you always scan each row left to right, every pile drifts left. A grain
# that could go down-left or down-right is asked "down-left?" first, and because
# the grain to its left was already moved this frame, down-left is free more
# often than down-right. Do that sixty times a second and a cone of sand walks
# off the side of the screen. So the scan direction ALTERNATES every frame, and
# the two diagonals are tried in that same alternating order. A pile built here
# is symmetric because the asymmetry is cancelled, not because it was hidden.
#
# The other structural rule is the scan ORDER: rows are visited bottom-up, so a
# falling grain moves into a row that has already been processed and cannot fall
# twice in one frame. Steam and fire move the other way, so a per-cell frame
# stamp stops anything from being updated twice regardless of direction.
#
# PERFORMANCE, because this is the part that decides whether a sand game is
# playable. The grid is 128x96 -- twelve thousand cells -- and GDScript costs
# about a microsecond per cell that actually has to think, which is what sets
# that number. Three things keep it inside a frame:
#
#   Chunks. The grid is divided into 16x16 chunks and a chunk is simulated only
#   if something moved in it, or next to it, last frame. A settled pile wakes
#   nobody: measured at 0.06 ms/step, versus 8 ms with everything in free fall.
#
#   An inlined hot path. A grain falling one row is most of what this file ever
#   does, and routed through _update -> _powder -> _open -> _swap -> _wake that
#   is six GDScript calls per grain per frame. It is written out longhand in
#   _step instead.
#
#   Run-length drawing. Colour is material plus a row band, and the band is
#   constant across a row, so a run of one colour is a run of one material and
#   the whole grid draws in a few hundred draw_rect calls rather than twelve
#   thousand. That is why the materials are flat colours banded by row rather
#   than speckled per cell: speckle would look better and would cost twelve
#   thousand draw calls a frame.
#
# Measured on this machine: 0.06 ms/step settled, 3.5 ms/step for a working
# scene of sand, water, oil and fire, 8 ms/step with 62% of the grid in free
# fall at once. The last of those is a second of pathological worst case after
# you paint a slab across the whole top of the screen.
#
# Same contract as g2048.gd: the desktop does .new(), sets mono and machine,
# then take_focus(). Everything is draw_rect / draw_line / draw_string.
#
# There is no score in a sandbox, so what persists is the high-water mark: the
# most particles ever alive at once, in /root/.sand. `cat /root/.sand` agrees
# with the panel.

extends Control

var mono: Font
var machine: Object = null

# The grid is fixed. Resizing the window changes the pixel size of a cell and
# nothing else, so a drag cannot alter the physics under a running pile.
const GW := 128
const GH := 96
const NCELL := GW * GH

# Chunk bookkeeping. 16 is a power of two so the divisions are shifts.
const CS_SH := 4
const CS_M := 15
const CCW := (GW + 15) >> 4
const CCH := (GH + 15) >> 4

const EMPTY := 0
const SAND := 1
const WATER := 2
const OIL := 3
const FIRE := 4
const WOOD := 5
const STONE := 6
const STEAM := 7
const ACID := 8
const PLANT := 9
const ASH := 10
const LAVA := 11
const NMAT := 12

# Density decides who sinks through whom. A cell may move into a target that is
# empty, or that is a fluid strictly lighter than itself -- that single rule
# gives sand sinking through water, water sinking through oil (so oil floats),
# and ash resting on water but sinking in oil, with no special cases.
const DENS := [0, 8, 5, 3, 1, 100, 100, 1, 5, 100, 4, 7]
const FLUID := [false, false, true, true, true, false, false, true, true, false, false, true]
const BURNS := [false, false, false, true, false, true, false, false, false, true, false, false]

# The four orthogonal neighbours, typed so the arithmetic below stays integer.
const NBX: Array[int] = [1, -1, 0, 0]
const NBY: Array[int] = [0, 0, 1, -1]

const MAT_NAME := ["erase", "sand", "water", "oil", "fire", "wood", "stone",
	"steam", "acid", "plant", "ash", "lava"]

# Two shades per material, alternating every two rows. The banding reads as
# sediment layers and, crucially, leaves whole rows as single colour runs.
const MAT_COL := [
	Color("#00000000"), Color("#00000000"),
	Color("#d8bd77"), Color("#cbae66"),          # sand
	Color("#3f79b8"), Color("#3a6fa9"),          # water
	Color("#6b5330"), Color("#61492a"),          # oil
	Color("#ff9b2e"), Color("#ff8419"),          # fire (mostly overridden)
	Color("#8a5f36"), Color("#7e5530"),          # wood
	Color("#8b8b8b"), Color("#828282"),          # stone
	Color("#c9d6e2"), Color("#b9c9d8"),          # steam
	Color("#8fd23f"), Color("#83c435"),          # acid
	Color("#3f9142"), Color("#38833b"),          # plant
	Color("#6f6a63"), Color("#65605a"),          # ash
	Color("#e0561f"), Color("#d34a17"),          # lava
]

const BG := Color("#f2f0ea")
const PANEL := Color("#e3e0d6")
const CANVAS := Color("#1b1a18")
const INK := Color("#2f2a24")
const FAINT := Color("#7d7468")
const SEL := Color("#2f2a24")
const TOP := 40.0

var mat := PackedByteArray()
var aux := PackedByteArray()          # fire/steam life, liquid flow direction
var stamp := PackedByteArray()        # frame stamp: this cell already moved
var cact := PackedByteArray()         # chunks to simulate this frame
var cnext := PackedByteArray()        # chunks to simulate next frame

var rowcnt := PackedInt32Array()      # non-empty cells per row; empty rows are free
var pal: Array = []                   # colour id -> Color

var brush := 5
var sel := SAND
var paused := false
var alive := 0
var nhot := 0                         # fire + lava cells; zero means water need not look
var best := 0
var frame := 1
var fstamp := 1
var acc := 0.0
var painting := 0                     # 0 none, 1 paint, 2 erase
var last_cell := Vector2i(-1, -1)
var rng := RandomNumberGenerator.new()

# Filled in by _draw so _gui_input can hit-test the same rectangles the user is
# looking at, rather than a second copy of the layout that can drift from it.
var pw := 90.0                        # panel width
var cell_px := 4.0
var org := Vector2.ZERO
var btn_rects: Array = []             # [Rect2, id]


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	clip_contents = true
	rng.randomize()
	mat.resize(NCELL)
	aux.resize(NCELL)
	stamp.resize(NCELL)
	cact.resize(CCW * CCH)
	cnext.resize(CCW * CCH)
	rowcnt.resize(GH)
	_build_palette()
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.sand")
		if t.strip_edges().is_valid_int():
			best = int(t.strip_edges())
	_clear()
	set_process(true)


func take_focus() -> void:
	grab_focus()


# Colour ids 0..23 are material x band; 24..27 are the four stages of a dying
# flame; 28..31 the same for cooling lava. Keeping them as small integers is
# what lets the run-length pass compare cells with one integer compare.
func _build_palette() -> void:
	pal = []
	for i in range(NMAT * 2):
		pal.append(MAT_COL[i])
	pal.append(Color("#fff2b0")); pal.append(Color("#ffc23d"))
	pal.append(Color("#ff8a1f")); pal.append(Color("#e0521c"))
	pal.append(Color("#ffb04a")); pal.append(Color("#f07a22"))


func _clear() -> void:
	for i in range(NCELL):
		mat[i] = EMPTY
		aux[i] = 0
	for c in range(CCW * CCH):
		cact[c] = 0
		cnext[c] = 0
	for y in range(GH):
		rowcnt[y] = 0
	alive = 0
	nhot = 0
	queue_redraw()


# ---------------------------------------------------------------- simulation

# Waking is ONE array write. A cell that moves must also wake the chunks around
# it -- the grain sitting one row above a chunk boundary is waiting on this
# vacancy -- but doing that per cell costs eight branches on the hottest path in
# the file. So the neighbourhood is added once per frame instead, by dilating
# the whole chunk map by one at the end of _step. Eighty chunks, once; versus
# eight branches times ten thousand grains.
func _wake(x: int, y: int) -> void:
	cnext[(y >> CS_SH) * CCW + (x >> CS_SH)] = 1


func _dilate() -> void:
	var out := PackedByteArray()
	out.resize(CCW * CCH)
	for cy in range(CCH):
		for cx in range(CCW):
			if cnext[cy * CCW + cx] == 0:
				continue
			var y0: int = max(0, cy - 1)
			var y1: int = min(CCH - 1, cy + 1)
			var x0: int = max(0, cx - 1)
			var x1: int = min(CCW - 1, cx + 1)
			for ny in range(y0, y1 + 1):
				for nx in range(x0, x1 + 1):
					out[ny * CCW + nx] = 1
	cnext = out


func _swap(a: int, b: int, ax: int, ay: int, bx: int, by: int) -> void:
	var m := mat[a]
	var v := aux[a]
	var n := mat[b]
	# A swap only changes a row's population when exactly one side was empty.
	if ay != by and (m == EMPTY) != (n == EMPTY):
		if m == EMPTY:
			rowcnt[ay] += 1
			rowcnt[by] -= 1
		else:
			rowcnt[ay] -= 1
			rowcnt[by] += 1
	mat[a] = n
	aux[a] = aux[b]
	mat[b] = m
	aux[b] = v
	stamp[b] = fstamp
	stamp[a] = fstamp
	_wake(ax, ay)
	_wake(bx, by)


func _kill(i: int, x: int, y: int) -> void:
	if mat[i] != EMPTY:
		alive -= 1
		rowcnt[y] -= 1
	if mat[i] == FIRE or mat[i] == LAVA:
		nhot -= 1
	mat[i] = EMPTY
	aux[i] = 0
	stamp[i] = fstamp
	_wake(x, y)


func _become(i: int, m: int, a: int, x: int, y: int) -> void:
	if mat[i] == EMPTY and m != EMPTY:
		alive += 1
		rowcnt[y] += 1
	elif mat[i] != EMPTY and m == EMPTY:
		alive -= 1
		rowcnt[y] -= 1
	if mat[i] == FIRE or mat[i] == LAVA:
		nhot -= 1
	if m == FIRE or m == LAVA:
		nhot += 1
	mat[i] = m
	aux[i] = a
	stamp[i] = fstamp
	_wake(x, y)


func _step() -> void:
	frame += 1
	fstamp += 1
	if fstamp > 250:
		fstamp = 1
		for i in range(NCELL):
			stamp[i] = 0
	# THE ALTERNATION. Odd frames sweep right-to-left and prefer the left
	# diagonal; even frames do the opposite. Without this the pile drifts.
	var d := 1 if (frame & 1) == 0 else -1
	cact = cnext
	cnext = PackedByteArray()
	cnext.resize(CCW * CCH)

	var y := GH - 1
	while y >= 0:
		if rowcnt[y] == 0:
			y -= 1
			continue
		var rowbase := y * GW
		var cy := (y >> CS_SH) * CCW
		var cybelow := ((y + 1) >> CS_SH) * CCW
		var ci := 0 if d > 0 else CCW - 1
		while ci >= 0 and ci < CCW:
			if cact[cy + ci] == 0:
				ci += d
				continue
			var x0 := ci << CS_SH
			var x1: int = min(x0 + 16, GW) - 1
			var x := x0 if d > 0 else x1
			while x >= x0 and x <= x1:
				var i := rowbase + x
				var m := mat[i]
				# Stone and wood never act; they are only ever acted upon.
				if m == EMPTY or m == STONE or m == WOOD or stamp[i] == fstamp:
					x += d
					continue
				# THE HOT PATH, WRITTEN OUT. Powders are most of the cells in a
				# busy sandbox and a grain falling one row is most of what they
				# do. As a chain of small functions that is six GDScript calls
				# per grain per frame, and at ten thousand grains the calls cost
				# more than the physics they are wrapping. Inlined it is about
				# twenty operations and no calls at all.
				if m == SAND or m == ASH:
					if y + 1 < GH:
						var b := i + GW
						var bx := x
						var t := mat[b]
						if t != EMPTY and not (FLUID[t] and DENS[t] < DENS[m]):
							# Straight down is blocked, so the two diagonals, in
							# the frame's alternating order -- a fixed order here
							# is the drift bug.
							t = -1
							var xa := x + d
							if xa >= 0 and xa < GW:
								var td: int = mat[b + d]
								if td == EMPTY or (FLUID[td] and DENS[td] < DENS[m]):
									b += d
									bx = xa
									t = td
							if t < 0:
								var xb := x - d
								if xb >= 0 and xb < GW:
									var te: int = mat[b - d]
									if te == EMPTY or (FLUID[te] and DENS[te] < DENS[m]):
										b -= d
										bx = xb
										t = te
						if t >= 0:
							if t == EMPTY:
								rowcnt[y] -= 1
								rowcnt[y + 1] += 1
							var av := aux[i]
							mat[i] = t
							aux[i] = aux[b]
							mat[b] = m
							aux[b] = av
							stamp[b] = fstamp
							stamp[i] = fstamp
							cnext[cy + (x >> CS_SH)] = 1
							cnext[cybelow + (bx >> CS_SH)] = 1
					x += d
					continue
				# The inside of a body of liquid has nowhere to go and
				# nothing to react with: six neighbours of the same stuff and
				# every rule below is a foregone conclusion. Skipping it here
				# is what makes a standing pool cost almost nothing.
				if m == WATER or m == OIL or m == ACID:
					if x > 0 and x < GW - 1 and y > 0 and y + 1 < GH:
						var bb := i + GW
						if mat[bb] == m and mat[i - 1] == m and mat[i + 1] == m \
								and mat[i - GW] == m and mat[bb - 1] == m and mat[bb + 1] == m:
							x += d
							continue
				_update(i, x, y, m, d)
				x += d
			ci += d
		y -= 1
	_dilate()


# Everything the hot path in _step did not already handle. Powders never arrive
# here -- they are dealt with inline, where the calls cost more than the rules.
func _update(i: int, x: int, y: int, m: int, d: int) -> void:
	match m:
		WATER:
			# Only water that could possibly be near a flame bothers looking.
			if nhot > 0 and _quench(i, x, y):
				return
			_liquid(i, x, y, m, d, 3)
		ACID:
			_wake(x, y)
			if _corrode(i, x, y):
				return
			_liquid(i, x, y, m, d, 3)
		OIL:
			_liquid(i, x, y, m, d, 1)
		LAVA:
			_lava(i, x, y, d)
		FIRE:
			_fire(i, x, y)
		STEAM:
			_steam(i, x, y, d)
		PLANT:
			_plant(i, x, y)


# May this material move into whatever is sitting in the target cell? Empty
# always; a lighter fluid gets displaced; a solid never does.
func _open(m: int, t: int) -> bool:
	if t == EMPTY:
		return true
	return FLUID[t] and DENS[t] < DENS[m]


# Liquids fall like powders, then spread sideways. `reach` is how many cells of
# level ground they will cross in one frame: water levels out fast, oil crawls.
# The direction is remembered in aux, so a puddle keeps running the way it was
# running instead of jittering in place.
func _liquid(i: int, x: int, y: int, m: int, d: int, reach: int) -> void:
	if y + 1 < GH:
		var b := i + GW
		if _open(m, mat[b]):
			_swap(i, b, x, y, x, y + 1)
			return
		var xa := x + d
		if xa >= 0 and xa < GW and _open(m, mat[b + d]):
			_swap(i, b + d, x, y, xa, y + 1)
			return
		var xb := x - d
		if xb >= 0 and xb < GW and _open(m, mat[b - d]):
			_swap(i, b - d, x, y, xb, y + 1)
			return
	var fd := 1 if (aux[i] & 1) == 0 else -1
	var dest := -1
	var dx := 0
	for s in range(reach):
		var nx := x + fd * (s + 1)
		if nx < 0 or nx >= GW:
			break
		if not _open(m, mat[i + fd * (s + 1)]):
			break
		dest = i + fd * (s + 1)
		dx = nx
	if dest < 0:
		aux[i] = aux[i] ^ 1          # blocked: try the other way next frame
		return
	_swap(i, dest, x, y, dx, y)


# Water touching fire or lava flashes to steam and takes the fire with it.
func _quench(i: int, x: int, y: int) -> bool:
	for k in range(4):
		var nx := x + NBX[k]
		var ny := y + NBY[k]
		if nx < 0 or nx >= GW or ny < 0 or ny >= GH:
			continue
		var j := ny * GW + nx
		var t := mat[j]
		if t == FIRE:
			_kill(j, nx, ny)
			if rng.randi() % 3 == 0:
				_become(i, STEAM, 90, x, y)
				return true
			return false
		if t == LAVA:
			_become(j, STONE, 0, nx, ny)
			_become(i, STEAM, 120, x, y)
			return true
	return false


# Acid eats one neighbour and is used up doing it. Stone survives, which is the
# only reason to ever build anything out of stone.
func _corrode(i: int, x: int, y: int) -> bool:
	if rng.randi() % 4 != 0:
		return false
	var k := rng.randi() % 4
	var nx := x + NBX[k]
	var ny := y + NBY[k]
	if nx < 0 or nx >= GW or ny < 0 or ny >= GH:
		return false
	var j := ny * GW + nx
	var t := mat[j]
	if t == EMPTY or t == ACID or t == STONE or t == STEAM:
		return false
	_kill(j, nx, ny)
	_kill(i, x, y)
	return true


func _lava(i: int, x: int, y: int, d: int) -> void:
	_wake(x, y)
	for k in range(4):
		var nx := x + NBX[k]
		var ny := y + NBY[k]
		if nx < 0 or nx >= GW or ny < 0 or ny >= GH:
			continue
		var j := ny * GW + nx
		var t := mat[j]
		if t == WATER:
			_become(j, STEAM, 120, nx, ny)
			_become(i, STONE, 0, x, y)
			return
		if BURNS[t] and rng.randi() % 3 == 0:
			_become(j, FIRE, 40 | (128 if t == WOOD else 0), nx, ny)
	if y > 0 and mat[i - GW] == EMPTY and rng.randi() % 24 == 0:
		_become(i - GW, FIRE, 14, x, y - 1)
	# Viscous: lava only moves on one frame in three, so it crawls and piles up
	# instead of sheeting out like water.
	if frame % 3 == 0:
		_liquid(i, x, y, LAVA, d, 1)


func _fire(i: int, x: int, y: int) -> void:
	_wake(x, y)
	var life := aux[i] & 127
	var ash := (aux[i] & 128) != 0
	if life <= 1:
		if ash and rng.randi() % 2 == 0:
			_become(i, ASH, 0, x, y)
		else:
			_kill(i, x, y)
		return
	aux[i] = (life - 1) | (128 if ash else 0)
	stamp[i] = fstamp
	for k in range(4):
		var nx := x + NBX[k]
		var ny := y + NBY[k]
		if nx < 0 or nx >= GW or ny < 0 or ny >= GH:
			continue
		var j := ny * GW + nx
		var t := mat[j]
		if BURNS[t] and rng.randi() % 5 == 0:
			var l := 60 if t == WOOD else 26
			_become(j, FIRE, l | (128 if t == WOOD else 0), nx, ny)


func _steam(i: int, x: int, y: int, d: int) -> void:
	_wake(x, y)
	var life := aux[i]
	if life <= 1:
		_become(i, WATER, 0, x, y)
		return
	aux[i] = life - 1
	stamp[i] = fstamp
	# Rising is the mirror of falling: a gas moves into anything HEAVIER.
	if y > 0:
		var a := i - GW
		if _rise(a):
			_swap(i, a, x, y, x, y - 1)
			return
		var xa := x + d
		if xa >= 0 and xa < GW and _rise(a + d):
			_swap(i, a + d, x, y, xa, y - 1)
			return
	var xs := x + d
	if xs >= 0 and xs < GW and mat[i + d] == EMPTY:
		_swap(i, i + d, x, y, xs, y)


func _rise(j: int) -> bool:
	var t := mat[j]
	if t == EMPTY:
		return true
	return FLUID[t] and DENS[t] > DENS[STEAM]


# Plants drink. A plant cell next to water occasionally turns that water into
# more plant, which is why a seed on a wet floor becomes a hedge.
func _plant(i: int, x: int, y: int) -> void:
	if rng.randi() % 12 != 0:
		return
	_wake(x, y)
	var k := rng.randi() % 4
	var nx := x + NBX[k]
	var ny := y + NBY[k]
	if nx < 0 or nx >= GW or ny < 0 or ny >= GH:
		return
	var j := ny * GW + nx
	if mat[j] == WATER:
		_become(j, PLANT, 0, nx, ny)


# ---------------------------------------------------------------- painting

func _paint_at(gx: int, gy: int) -> void:
	var r := brush - 1
	var m := sel
	var x0: int = max(0, gx - r)
	var x1: int = min(GW - 1, gx + r)
	var y0: int = max(0, gy - r)
	var y1: int = min(GH - 1, gy + r)
	var rr := r * r + r
	for y in range(y0, y1 + 1):
		var dy := y - gy
		for x in range(x0, x1 + 1):
			var dx := x - gx
			if dx * dx + dy * dy > rr:
				continue
			var i := y * GW + x
			if m == EMPTY:
				if mat[i] != EMPTY:
					_kill(i, x, y)
				continue
			var a := 0
			if m == FIRE:
				a = 40
			elif m == STEAM:
				a = 160
			# The brush overwrites whatever was there. A sandbox where the
			# paint refuses to go down because a grain is in the way is a
			# sandbox you argue with.
			_become(i, m, a, x, y)


func _paint_line(a: Vector2i, b: Vector2i) -> void:
	var n: int = max(abs(b.x - a.x), abs(b.y - a.y))
	if n <= 0:
		_paint_at(b.x, b.y)
		return
	# A fast drag must not leave the brush stamped in disconnected blobs, so
	# the gap between this frame's mouse position and the last one is filled in.
	for s in range(n + 1):
		var t := float(s) / float(n)
		_paint_at(int(round(lerp(float(a.x), float(b.x), t))),
			int(round(lerp(float(a.y), float(b.y), t))))


func _cell_under(p: Vector2) -> Vector2i:
	if cell_px <= 0.0:
		return Vector2i(-1, -1)
	var gx := int(floor((p.x - org.x) / cell_px))
	var gy := int(floor((p.y - org.y) / cell_px))
	if gx < 0 or gy < 0 or gx >= GW or gy >= GH:
		return Vector2i(-1, -1)
	return Vector2i(gx, gy)


# ---------------------------------------------------------------- input

func _process(dt: float) -> void:
	if paused:
		return
	acc += dt
	# Fixed timestep, capped at two steps: a stalled desktop must not hand the
	# sand a hundred frames of catch-up and freeze the window doing them.
	var n := 0
	while acc >= 1.0 / 60.0 and n < 2:
		acc -= 1.0 / 60.0
		_step()
		n += 1
	if n == 0:
		return
	_record()
	queue_redraw()


# The sandbox has no score, so what it keeps is the high-water mark: the most
# particles that were ever alive at once. Written through the machine's own
# shell like every other game here, so `cat /root/.sand` agrees with the panel.
# Only called when the number actually goes up, so a still screen writes nothing.
func _record() -> void:
	if alive <= best:
		return
	best = alive
	if machine:
		machine.sh_on(0, 'echo "%d" > /root/.sand' % best)


func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton:
		var mb := e as InputEventMouseButton
		accept_event()
		grab_focus()
		if mb.button_index == MOUSE_BUTTON_WHEEL_UP and mb.pressed:
			brush = min(24, brush + 1)
			queue_redraw()
			return
		if mb.button_index == MOUSE_BUTTON_WHEEL_DOWN and mb.pressed:
			brush = max(1, brush - 1)
			queue_redraw()
			return
		if not mb.pressed:
			painting = 0
			last_cell = Vector2i(-1, -1)
			_record()
			return
		if mb.button_index == MOUSE_BUTTON_LEFT:
			for br in btn_rects:
				if (br[0] as Rect2).has_point(mb.position):
					_hit(br[1])
					queue_redraw()
					return
		var c := _cell_under(mb.position)
		if c.x < 0:
			return
		painting = 2 if mb.button_index == MOUSE_BUTTON_RIGHT else 1
		var keep := sel
		if painting == 2:
			sel = EMPTY
		_paint_at(c.x, c.y)
		sel = keep
		last_cell = c
		queue_redraw()
		return
	if e is InputEventMouseMotion:
		accept_event()
		if painting == 0:
			return
		var c2 := _cell_under((e as InputEventMouseMotion).position)
		if c2.x < 0:
			last_cell = Vector2i(-1, -1)
			return
		var keep2 := sel
		if painting == 2:
			sel = EMPTY
		if last_cell.x >= 0:
			_paint_line(last_cell, c2)
		else:
			_paint_at(c2.x, c2.y)
		sel = keep2
		last_cell = c2
		queue_redraw()
		return
	if not (e is InputEventKey) or not (e as InputEventKey).pressed:
		return
	var k := e as InputEventKey
	accept_event()
	match k.keycode:
		KEY_SPACE: paused = not paused
		KEY_PERIOD:
			paused = true
			_step()
		KEY_C: _clear()
		KEY_BRACKETLEFT: brush = max(1, brush - 1)
		KEY_BRACKETRIGHT: brush = min(24, brush + 1)
		KEY_1: sel = SAND
		KEY_2: sel = WATER
		KEY_3: sel = OIL
		KEY_4: sel = FIRE
		KEY_5: sel = WOOD
		KEY_6: sel = STONE
		KEY_7: sel = STEAM
		KEY_8: sel = ACID
		KEY_9: sel = PLANT
		KEY_0: sel = ASH
		KEY_L: sel = LAVA
		KEY_E: sel = EMPTY
		_: return
	queue_redraw()


func _hit(id: int) -> void:
	if id >= 0:
		sel = id
		return
	match id:
		-1: paused = not paused
		-2:
			paused = true
			_step()
		-3: _clear()
		-4: brush = max(1, brush - 1)
		-5: brush = min(24, brush + 1)


# ---------------------------------------------------------------- drawing

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	draw_string(mono, Vector2(10, 20), "sand", HORIZONTAL_ALIGNMENT_LEFT, -1, 18, INK)
	draw_string(mono, Vector2(10, 34),
		"drag to paint   right-drag erases   wheel sizes the brush   space pauses   . steps   C clears",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)

	btn_rects = []
	pw = clamp(size.x * 0.24, 62.0, 96.0)
	var avail_w: float = size.x - pw - 18.0
	var avail_h: float = size.y - TOP - 10.0
	if avail_w < 20.0 or avail_h < 20.0:
		return
	cell_px = min(avail_w / float(GW), avail_h / float(GH))
	if cell_px <= 0.0:
		return
	var bw := cell_px * GW
	var bh := cell_px * GH
	org = Vector2(pw + 14.0 + (avail_w - bw) * 0.5, TOP + (avail_h - bh) * 0.5)

	_draw_panel()

	draw_rect(Rect2(org, Vector2(bw, bh)), CANVAS)
	_draw_cells()
	draw_rect(Rect2(org, Vector2(bw, bh)), FAINT, false, 1.0)

	if paused:
		draw_string(mono, Vector2(org.x + 4, org.y + 14), "paused",
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color("#f0e6c0"))


# The whole reason the materials are flat. Colour is material plus a row band,
# and the band is constant across a row, so a run of equal colour IS a run of
# equal material -- which means the run-length pass can read the grid directly
# and never build a scratch row. That halves the cost of this loop, and this
# loop was costing as much as the physics.
func _draw_cells() -> void:
	var w := cell_px
	var wq := w + 0.5                # a hair of overlap, so no seams show
	for y in range(GH):
		if rowcnt[y] == 0:
			continue
		var rowbase := y * GW
		var band := (y >> 1) & 1
		var yy := org.y + y * w
		var x0 := 0
		while x0 < GW:
			var m0 := mat[rowbase + x0]
			var x1 := x0 + 1
			while x1 < GW and mat[rowbase + x1] == m0:
				x1 += 1
			if m0 != EMPTY:
				if m0 == FIRE:
					# Flames are the one thing drawn per cell: a fire's colour
					# is how much life it has left, and there are never many.
					for fx in range(x0, x1):
						draw_rect(Rect2(org.x + fx * w, yy, wq, wq),
							pal[24 + clampi(3 - ((aux[rowbase + fx] & 127) >> 4), 0, 3)])
				else:
					draw_rect(Rect2(org.x + x0 * w, yy, (x1 - x0) * w + 0.5, wq),
						pal[m0 * 2 + band])
			x0 = x1


func _draw_panel() -> void:
	draw_rect(Rect2(8, TOP, pw, size.y - TOP - 10.0), PANEL)
	var y := TOP + 4.0
	var listh: float = size.y - TOP - 112.0
	if listh >= float(NMAT) * 12.0:
		# Roomy: one labelled row per material.
		var rowh: float = min(listh / float(NMAT), 19.0)
		for m in range(NMAT):
			var r := Rect2(10, y, pw - 4.0, rowh - 2.0)
			btn_rects.append([r, m])
			_swatch(r, m, rowh - 7.0)
			draw_string(mono, r.position + Vector2(rowh - 1.0, rowh - 6.0), MAT_NAME[m],
				HORIZONTAL_ALIGNMENT_LEFT, r.size.x - rowh, 10, INK)
			y += rowh
	else:
		# Cramped: a grid of swatches with no labels, and the name of whatever is
		# selected underneath. Truncating the list instead would leave lava and
		# plant unreachable by mouse, which is worse than losing the names.
		var cw: float = (pw - 6.0) / 3.0
		var ch: float = clamp(listh / 4.0, 9.0, cw)
		for m in range(NMAT):
			var r := Rect2(10 + float(m % 3) * cw, y + float(m / 3) * ch,
				cw - 2.0, ch - 2.0)
			btn_rects.append([r, m])
			_swatch(r, m, min(r.size.x, r.size.y) - 4.0)
		y += ch * 4.0 + 2.0
		draw_string(mono, Vector2(10, y + 8), MAT_NAME[sel],
			HORIZONTAL_ALIGNMENT_LEFT, pw - 6.0, 10, INK)

	var by: float = size.y - 96.0
	if by < TOP + 4.0:
		return
	var bwd := (pw - 10.0) * 0.5
	var minus := Rect2(10, by, bwd, 15)
	var plus := Rect2(10 + bwd + 2.0, by, bwd, 15)
	btn_rects.append([minus, -4])
	btn_rects.append([plus, -5])
	_button(minus, "-")
	_button(plus, "+")
	draw_string(mono, Vector2(10, by + 27), "brush %d" % brush,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)
	var pb := Rect2(10, by + 31, pw - 4.0, 15)
	var sb := Rect2(10, by + 50, (pw - 6.0) * 0.5, 15)
	var cb := Rect2(10 + (pw - 6.0) * 0.5 + 2.0, by + 50, (pw - 6.0) * 0.5, 15)
	btn_rects.append([pb, -1])
	btn_rects.append([sb, -2])
	btn_rects.append([cb, -3])
	_button(pb, "run" if paused else "pause")
	_button(sb, "step")
	_button(cb, "clear")
	draw_string(mono, Vector2(10, by + 77), "%d alive" % alive,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 9, FAINT)
	draw_string(mono, Vector2(10, by + 88), "peak %d" % best,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 9, FAINT)


# One palette entry: the selection highlight, then the colour chip. The eraser
# has no colour, so it gets a cross instead of a swatch of nothing.
func _swatch(r: Rect2, m: int, s: float) -> void:
	if sel == m:
		draw_rect(r, Color(1, 1, 1, 0.75))
		draw_rect(r, SEL, false, 1.0)
	var sw := Rect2(r.position + Vector2(3, 2), Vector2(s, s))
	if m == EMPTY:
		draw_rect(sw, Color(1, 1, 1, 0.9))
		draw_line(sw.position, sw.position + sw.size, Color("#b04030"), 1.0)
		draw_line(sw.position + Vector2(sw.size.x, 0),
			sw.position + Vector2(0, sw.size.y), Color("#b04030"), 1.0)
	else:
		draw_rect(sw, pal[m * 2])
	draw_rect(sw, FAINT, false, 1.0)


func _button(r: Rect2, label: String) -> void:
	draw_rect(r, Color(1, 1, 1, 0.7))
	draw_rect(r, FAINT, false, 1.0)
	draw_string(mono, Vector2(r.position.x, r.position.y + 11.5), label,
		HORIZONTAL_ALIGNMENT_CENTER, r.size.x, 10, INK)
