# gflappy.gd — the other game, the one you play with one hand while you think.
#
# 2048 proved the desktop can hold a game that keeps state. This one proves it
# can hold a game that keeps TIME: real physics on a real frame clock, a thing
# that is losing while you are reading this comment. A turn-based puzzle can be
# faked by a menu. A falling bird cannot.
#
# Same contract as g2048.gd -- the desktop does .new(), sets mono and machine,
# and calls take_focus(). Everything is drawn with primitives because this
# project owns no sprites, no textures and no audio, and inventing an asset
# folder to hold a bird would be the tail wagging the dog.
#
# The high score goes to /root/.flappy through the machine's own shell, so
# `cat /root/.flappy` in the terminal shows it. The toy stays honest.

extends Control

var mono: Font
var machine: Object = null

# The world is measured in virtual units, not pixels: WORLD_H tall, and as
# wide as the window makes it. That is the whole resize story. A short window
# and a tall window are the same game at different zoom levels, so nobody wins
# by dragging the corner.
#
# WHAT THAT GOT WRONG, and what the player actually saw:
#
#   "the pipes show up far off the small app window, kinda ignoring the window
#    size. They also fall off the back."
#
# Two separate causes, both real. First, pipe SPACING and SPEED were absolute
# unit counts while the world width was whatever the aspect ratio produced. A
# 520x400 window gives a world ~430 units wide, which is what those numbers
# were tuned against; a short wide window gives one thousands of units wide, so
# the first pipe spawned a screen and a half to the right and crawled in over
# half a minute, and a tall narrow window gives one ~120 units wide, narrower
# than the 150-unit spacing itself. Spacing and speed are now fractions of the
# world width, so the rhythm -- about 1.7s of thinking between pipes -- is
# identical at every window size, and the scale is floored so the world can
# never get narrower than WW_MIN.
#
# Second, this control did its own scrolling but not its own housekeeping: it
# spawned pipes past the right edge and kept them alive well past the left one,
# and de.gd does not set clip_contents on a window's content (desktop.gd does).
# An unclipped Control draws wherever you tell it to, so those pipes were
# painted onto the desktop OUTSIDE the window frame -- sticking out of the
# right side before they arrived and sliding out of the left side afterwards,
# which is the "fall off the back". Pipes now enter and leave exactly at the
# world edges, and _ready clips this control regardless.
const WORLD_H := 240.0
const WW_MIN := 200.0        # narrowest world we will play in; below this the
                             # scale stops shrinking and the ground fills the
                             # leftover height instead of the game deforming
const TOP := 26.0            # header strip, in real pixels

const GRAVITY := 620.0       # units/s^2 -- a flap arcs over in about 0.7s
const FLAP := -215.0         # impulse: rises ~37 units, half the starting gap
const V_MAX := 380.0         # terminal velocity, so a long fall stays readable
const BIRD_R := 7.0
const PIPE_W := 26.0
const GAP_START := 78.0      # 5.5 bird-diameters. Forgiving, not free.
const GAP_MIN := 60.0        # where the squeeze stops; below this it is luck
# Fractions of the world width, not unit counts. The old absolute values --
# 90 units/s and 150 units apart -- are what these produce in the default
# 520x400 window, so the game feels the same there and stays sane everywhere.
const SPEED_F := 0.21        # world widths per second
const SPEED_MAX_F := 0.31
const SPEED_GAIN := 0.005    # per pipe passed, until SPEED_MAX_F
const SPACING_F := 0.35      # gap between pipe centres, as a share of the width
const BIRD_F := 0.28         # the bird's column, as a share of the width
const MARGIN := 26.0         # keeps a gap off the ceiling and the floor

# A calm daytime palette. This desktop is light and classic; a neon bird would
# look like it wandered in from a different operating system.
const SKY_HI := Color("#cfe6f2")
const SKY_LO := Color("#eaf3f7")
const GROUND := Color("#cbbf95")
const GRASS := Color("#8fa860")
const PIPE := Color("#7f9c5a")
const PIPE_DARK := Color("#65803f")
const BIRD := Color("#e2b23c")
const INK := Color("#3b4048")
const FAINT := Color("#7b838e")

var bird_y := WORLD_H * 0.45
var vel := 0.0
# Each: {"x", "gap_y", "gap", "scored"}. The gap HEIGHT is stored per pipe and
# not recomputed from the current score, because otherwise the pipe you were
# already lined up on narrowed around its own centre the instant you scored the
# one before it.
var pipes: Array = []
var score := 0
var best := 0
var state := 0               # 0 ready, 1 flying, 2 dead
var flap_t := 0.0            # counts down; the wing is up while it runs
var shake := 0.0             # a short jolt on death, purely for the feel
var rng := RandomNumberGenerator.new()
var last_ww := 0.0           # world width the pipe positions were written against


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	# de.gd hands this control a size but no clipping, and a game that scrolls
	# things off both edges MUST clip: without this, a pipe leaving the screen
	# keeps being drawn across the wallpaper.
	clip_contents = true
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	rng.randomize()
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.flappy")
		if t.strip_edges().is_valid_int():
			best = int(t.strip_edges())
	_new_game()
	set_process(true)


func take_focus() -> void:
	grab_focus()


# ------------------------------------------------------------------- world --

# How many real pixels one virtual unit is worth. Normally the world height
# fills the window, but a window narrow enough that WORLD_H-fit would leave the
# world under WW_MIN units wide is fitted to its WIDTH instead -- the sky then
# stops short of the bottom and _draw's ground fills the rest. Fitting to the
# smaller of the two is what stops an extreme aspect ratio from producing a
# playfield the game was never tuned for.
func _scale() -> float:
	return max(0.01, min(max(1.0, size.y - TOP) / WORLD_H, max(1.0, size.x) / WW_MIN))


# Virtual units across the window. x = 0 is the left edge and x = _world_w() is
# the right edge, exactly, at every window size -- that is the invariant the
# spawner and the despawner both key off.
func _world_w() -> float:
	return max(WW_MIN, size.x / _scale())


# The bird sits at a fixed FRACTION across, not a fixed unit column. The old
# clamp(ww * 0.28, 30, 110) meant its pixel position jumped when the window
# resized, because the clamp bit at some widths and not others.
func _bird_x() -> float:
	return _world_w() * BIRD_F


func _spacing() -> float:
	# Never closer than three pipe widths, whatever the window does.
	return max(PIPE_W * 3.0, _world_w() * SPACING_F)


func _new_game() -> void:
	bird_y = WORLD_H * 0.45
	vel = 0.0
	pipes = []
	score = 0
	state = 0
	flap_t = 0.0
	shake = 0.0
	last_ww = _world_w()
	queue_redraw()


# A resize changes how many units fit across the window, so pipe positions --
# which are in units -- have to be rewritten or the field slides sideways under
# the player mid-flight. Each pipe keeps its FRACTION of the world width, which
# is the same rule the bird already follows (BIRD_F) and the same rule the
# vertical axis gets for free (the world is always WORLD_H tall, so bird_y
# needs no treatment at all). A resize is then a pure zoom: nothing enters or
# leaves the frame, the bird-to-pipe distances keep their proportions so no
# phantom collision and no lost point, and because speed is also a fraction of
# the width, the time until the next pipe arrives is unchanged too.
#
# Preserving absolute PIXEL positions instead was the tempting version, and it
# is wrong: shrinking the window then strands pipes past the new right edge.
func _resync_scale() -> void:
	var ww := _world_w()
	if last_ww <= 0.0:
		last_ww = ww
		return
	if is_equal_approx(ww, last_ww):
		return
	var f := ww / last_ww
	for p in pipes:
		p["x"] = p["x"] * f
	last_ww = ww


# Difficulty leans in over the first dozen pipes and then holds. The point is
# that pipe 30 feels different from pipe 3, not that pipe 30 is impossible.
func _gap_h() -> float:
	return max(GAP_MIN, GAP_START - float(score) * 1.5)


func _speed() -> float:
	return _world_w() * min(SPEED_MAX_F, SPEED_F + float(score) * SPEED_GAIN)


func _spawn(at_x: float) -> void:
	var g := _gap_h()
	# lo/hi keep the whole gap inside the world with MARGIN to spare, so the
	# gap is always somewhere the bird can physically be.
	var lo := MARGIN + g * 0.5
	var hi := max(lo, WORLD_H - MARGIN - g * 0.5)
	pipes.append({
		"x": at_x,
		"gap_y": rng.randf_range(lo, hi),
		"gap": g,
		"scored": false,
	})


func _flap() -> void:
	if state == 2:
		return
	state = 1
	vel = FLAP
	flap_t = 0.12
	queue_redraw()


func _die() -> void:
	state = 2
	shake = 0.25
	if score > best:
		best = score
		if machine:
			machine.sh_on(0, 'echo "%d" > /root/.flappy' % best)


func _process(dt: float) -> void:
	# Godot will hand you a monstrous dt after a stall or a window drag. Cap it
	# or the bird teleports through a pipe and the player is told they lost to
	# something they never saw.
	dt = min(dt, 0.05)
	_resync_scale()
	if shake > 0.0:
		shake = max(0.0, shake - dt)
	if flap_t > 0.0:
		flap_t = max(0.0, flap_t - dt)

	var ww := _world_w()

	if state == 0:
		# Idle bob, so the thing looks alive before you touch a key.
		bird_y = WORLD_H * 0.45 + sin(Time.get_ticks_msec() / 320.0) * 4.0
		queue_redraw()
		return
	if state == 2:
		queue_redraw()
		return

	vel = min(V_MAX, vel + GRAVITY * dt)
	bird_y += vel * dt

	var spd := _speed()
	for p in pipes:
		p["x"] -= spd * dt
	# Gone the moment its right edge crosses the left edge of the world. The
	# old test held pipes until x + PIPE_W < -20, which on an unclipped control
	# meant twenty-odd units of pipe still being painted outside the window.
	while pipes.size() > 0 and pipes[0]["x"] + PIPE_W <= 0.0:
		pipes.pop_front()
	# Stock the field to the right edge and NO FURTHER: a pipe born at ww is
	# already at the boundary, so every live pipe overlaps the visible rect.
	var sp := _spacing()
	while pipes.is_empty() or pipes[pipes.size() - 1]["x"] < ww - sp:
		_spawn(ww)

	var bx := _bird_x()
	for p in pipes:
		if not p["scored"] and p["x"] + PIPE_W < bx:
			p["scored"] = true
			score += 1
		# Circle against the pillar, near enough: the bird is small and the
		# corners it can clip are the corners it deserves to clip.
		var g: float = p["gap"]
		if bx + BIRD_R > p["x"] and bx - BIRD_R < p["x"] + PIPE_W:
			if bird_y - BIRD_R < p["gap_y"] - g * 0.5 or bird_y + BIRD_R > p["gap_y"] + g * 0.5:
				_die()

	# The ceiling bumps you; the ground kills you. Being pinned to the top of
	# the screen is punishment enough without ending the run for it.
	if bird_y < BIRD_R:
		bird_y = BIRD_R
		vel = max(vel, 0.0)
	if bird_y + BIRD_R >= WORLD_H:
		bird_y = WORLD_H - BIRD_R
		_die()
	queue_redraw()


# ------------------------------------------------------------------- input --

func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and e.pressed:
		grab_focus()
		if e.button_index == MOUSE_BUTTON_LEFT:
			accept_event()
			if state == 2:
				_new_game()
			else:
				_flap()
		return
	if not (e is InputEventKey) or not e.pressed:
		return
	var k := e as InputEventKey
	match k.keycode:
		KEY_R:
			accept_event()
			_new_game()
		KEY_SPACE, KEY_UP, KEY_W:
			accept_event()
			if state == 2:
				_new_game()
			else:
				_flap()


# ------------------------------------------------------------------ render --

func _to_px(wx: float, wy: float, s: float, jitter: float) -> Vector2:
	return Vector2(wx * s + jitter, TOP + wy * s)


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), SKY_LO)
	if size.y <= TOP + 20.0 or size.x < 60.0:
		return

	var s := _scale()
	var ww := _world_w()
	var jitter := 0.0
	if shake > 0.0:
		jitter = sin(Time.get_ticks_msec() / 18.0) * shake * 6.0

	# Sky, as a handful of bands rather than a gradient we cannot ask for.
	for i in range(6):
		var t := float(i) / 6.0
		draw_rect(
			Rect2(0, TOP + t * (size.y - TOP), size.x, (size.y - TOP) / 6.0 + 1.0),
			SKY_HI.lerp(SKY_LO, t))

	# Two lazy clouds, parked. They do not scroll, because a cloud that scrolls
	# at pipe speed reads as a wall you are allowed to fly through.
	for c in [Vector2(ww * 0.20, 42.0), Vector2(ww * 0.68, 66.0)]:
		var p := _to_px(c.x, c.y, s, jitter)
		draw_circle(p, 11.0 * s, Color(1, 1, 1, 0.62))
		draw_circle(p + Vector2(10.0 * s, 3.0 * s), 8.0 * s, Color(1, 1, 1, 0.62))
		draw_circle(p - Vector2(10.0 * s, -2.0 * s), 7.0 * s, Color(1, 1, 1, 0.62))

	for p in pipes:
		var gh: float = p["gap"]
		var x: float = p["x"] * s + jitter
		var w: float = PIPE_W * s
		var top_h: float = (p["gap_y"] - gh * 0.5) * s
		var bot_y: float = TOP + (p["gap_y"] + gh * 0.5) * s
		draw_rect(Rect2(x, TOP, w, top_h), PIPE)
		draw_rect(Rect2(x, bot_y, w, size.y - bot_y), PIPE)
		# Lips, so the gap edge is unmistakable at a glance.
		var lip := max(4.0, 6.0 * s)
		draw_rect(Rect2(x - 2.0 * s, TOP + top_h - lip, w + 4.0 * s, lip), PIPE_DARK)
		draw_rect(Rect2(x - 2.0 * s, bot_y, w + 4.0 * s, lip), PIPE_DARK)

	# Ground line at the bottom of the world, which is also the kill line.
	var gy := TOP + WORLD_H * s
	draw_rect(Rect2(0, gy - 3.0 * s, size.x, 3.0 * s), GRASS)
	if gy < size.y:
		draw_rect(Rect2(0, gy, size.x, size.y - gy), GROUND)

	_draw_bird(s, jitter)

	# Header: score on the left, best on the right, controls underneath in
	# grey until you are actually flying and no longer need to be told.
	draw_rect(Rect2(0, 0, size.x, TOP), Color("#f2f1ee"))
	draw_line(Vector2(0, TOP), Vector2(size.x, TOP), Color("#c9c6c0"), 1.0)
	draw_string(mono, Vector2(10, 18), "flappy  %d" % score,
		HORIZONTAL_ALIGNMENT_LEFT, -1, 15, INK)
	# max(): in a narrow window "best" was being aligned from a negative x and
	# printed off the left edge, over the score.
	draw_string(mono, Vector2(max(size.x * 0.5, size.x - 130.0), 18), "best %d" % best,
		HORIZONTAL_ALIGNMENT_RIGHT, min(120.0, size.x * 0.5 - 10.0), 12, FAINT)

	if state == 0:
		_banner("space, up or click to flap", "get through the gaps")
	elif state == 2:
		_banner("down at %d %s" % [score, "pipe" if score == 1 else "pipes"],
			"R or click to go again")


func _draw_bird(s: float, jitter: float) -> void:
	var c := _to_px(_bird_x(), bird_y, s, jitter)
	var r := BIRD_R * s
	draw_circle(c, r, BIRD)
	draw_circle(c, r, Color(0.55, 0.40, 0.10, 0.5), false, max(1.0, s))
	# Wing: up right after a flap, down the rest of the time. Two states is
	# all the animation a circle needs to look like it is trying.
	var wy: float = -r * 0.55 if flap_t > 0.0 else r * 0.45
	draw_rect(Rect2(c.x - r * 0.9, c.y + wy - r * 0.16,
		r * 0.9, max(1.0, r * 0.34)), Color("#c9962c"))
	# Beak, and an eye that looks where it is going.
	draw_rect(Rect2(c.x + r * 0.6, c.y - r * 0.12, r * 0.6, max(1.0, r * 0.3)),
		Color("#d97b31"))
	draw_circle(c + Vector2(r * 0.34, -r * 0.32), max(1.0, r * 0.26), Color.WHITE)
	draw_circle(c + Vector2(r * 0.40, -r * 0.32), max(1.0, r * 0.13), INK)


func _banner(line1: String, line2: String) -> void:
	var h := 46.0
	var y := TOP + (size.y - TOP) * 0.5 - h * 0.5
	var w: float = min(size.x - 20.0, 260.0)
	var x := (size.x - w) * 0.5
	if w < 90.0 or size.y - TOP < 70.0:
		return
	draw_rect(Rect2(x, y, w, h), Color(0.97, 0.97, 0.95, 0.88))
	draw_rect(Rect2(x, y, w, h), Color("#b9b5ad"), false, 1.0)
	draw_string(mono, Vector2(x, y + 20), line1,
		HORIZONTAL_ALIGNMENT_CENTER, w, 14, INK)
	draw_string(mono, Vector2(x, y + 36), line2,
		HORIZONTAL_ALIGNMENT_CENTER, w, 11, FAINT)
