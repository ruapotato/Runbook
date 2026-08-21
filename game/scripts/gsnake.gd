# gsnake.gd — snake, the game that proves the desktop's clock belongs to the app.
#
# 2048 showed a window can hold state between keypresses. Flappy showed it can
# hold time. Snake is the third thing: a game where the KEY YOU PRESS NOW is
# resolved on a tick that has not happened yet. That is a queue, and a queue is
# where toy games get it wrong -- press up-then-right faster than one tick and
# a naive game eats one of the two, which the player reads as "the desktop
# dropped my key" rather than "I was early". So there is a two-deep turn
# buffer, and the reversal check is made against the direction the snake will
# actually be facing when the turn lands, not the one it is facing now.
#
# Same contract as g2048.gd: the desktop does .new(), sets mono and machine,
# then take_focus(). Everything is draw_rect / draw_line / draw_string, because
# this project owns no sprites and no audio and a snake does not need any.
#
# The best score goes to /root/.snake through the machine's own shell, so
# `cat /root/.snake` in the terminal shows the same number the HUD shows.

extends Control

var mono: Font
var machine: Object = null

# The board is a fixed grid of cells, always. Resizing changes the pixel size
# of a cell and nothing else, so a window drag can never make the game easier
# by handing you more room to turn around in.
const COLS := 26
const ROWS := 18
const TOP := 40.0            # header strip, in real pixels

# Speed ramp. The snake starts at one step per START seconds and loses STEP_OFF
# of that per food eaten, floored at FASTEST. Eight or nine apples in you are
# playing at roughly twice the opening pace, which is the point at which the
# two-deep turn buffer starts earning its keep.
const START := 0.145
const STEP_OFF := 0.0045
const FASTEST := 0.055

const BG := Color("#f2f0ea")
const BOARD := Color("#e3e0d6")
const GRID := Color("#d6d2c6")
const INK := Color("#2f2a24")
const FAINT := Color("#7d7468")
const BODY := Color("#4f7a3a")
const HEAD := Color("#3c6129")
const FOOD := Color("#b5462f")
const DEAD := Color("#8a5a4a")

var snake: Array = []        # Vector2i, index 0 is the head
var dir := Vector2i(1, 0)
var turns: Array = []        # queued direction changes, at most two
var food := Vector2i(0, 0)
var grow := 0                # cells still owed to the tail
var score := 0
var best := 0
var over := false
var paused := false
var acc := 0.0
var rng := RandomNumberGenerator.new()


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	clip_contents = true
	rng.randomize()
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.snake")
		if t.strip_edges().is_valid_int():
			best = int(t.strip_edges())
	_new_game()
	set_process(true)


func take_focus() -> void:
	grab_focus()


func _new_game() -> void:
	snake = []
	var r := ROWS / 2
	for i in range(5):
		snake.append(Vector2i(6 - i, r))
	dir = Vector2i(1, 0)
	turns = []
	grow = 0
	score = 0
	over = false
	paused = false
	acc = 0.0
	_place_food()
	queue_redraw()


# Food never lands under the snake. Picking at random and retrying is fine
# while the board is mostly empty and terrible once it is nearly full, so this
# collects the free cells instead -- 468 of them at worst, once per apple.
func _place_food() -> void:
	var free: Array = []
	for y in range(ROWS):
		for x in range(COLS):
			var c := Vector2i(x, y)
			if not (c in snake):
				free.append(c)
	if free.is_empty():
		return
	food = free[rng.randi_range(0, free.size() - 1)]


func _interval() -> float:
	return max(FASTEST, START - STEP_OFF * float(score))


# The board is only repainted on a tick. A snake window left open in the corner
# of the desktop should cost the compositor one frame every seventh of a
# second, not sixty, and while it is paused or dead it should cost nothing.
func _process(dt: float) -> void:
	if over or paused:
		return
	acc += dt
	while acc >= _interval():
		acc -= _interval()
		_step()
		if over:
			return


func _step() -> void:
	if not turns.is_empty():
		dir = turns.pop_front()
	var head: Vector2i = snake[0] + dir
	if head.x < 0 or head.y < 0 or head.x >= COLS or head.y >= ROWS:
		_die(); return
	# The tail cell vacates on the same tick unless we are growing, so chasing
	# your own tail one cell behind is legal -- as it is in every snake worth
	# playing. Hence the search stops one short when grow is zero.
	var body_end: int = snake.size() if grow > 0 else snake.size() - 1
	for i in range(body_end):
		if snake[i] == head:
			_die(); return
	snake.push_front(head)
	if head == food:
		score += 1
		grow += 3
		_place_food()
	if grow > 0:
		grow -= 1
	else:
		snake.pop_back()
	queue_redraw()


func _die() -> void:
	over = true
	if score > best:
		best = score
		if machine:
			machine.sh_on(0, 'echo "%d" > /root/.snake' % best)
	queue_redraw()


# The direction a queued turn will be measured against: the last thing already
# in the buffer, or the live direction if the buffer is empty. Without this,
# up-then-left pressed inside one tick lets the second key see the OLD facing,
# and left is a legal turn from right, so the snake reverses into itself.
func _pending_dir() -> Vector2i:
	if turns.is_empty():
		return dir
	return turns[turns.size() - 1]


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
	if k.keycode == KEY_P or k.keycode == KEY_SPACE:
		if not over:
			paused = not paused
			queue_redraw()
		return
	if over:
		return
	var d := Vector2i.ZERO
	match k.keycode:
		KEY_LEFT, KEY_A:  d = Vector2i(-1, 0)
		KEY_RIGHT, KEY_D: d = Vector2i(1, 0)
		KEY_UP, KEY_W:    d = Vector2i(0, -1)
		KEY_DOWN, KEY_S:  d = Vector2i(0, 1)
	if d == Vector2i.ZERO:
		return
	paused = false
	var prev := _pending_dir()
	if d == prev or d == -prev:
		return
	if turns.size() < 2:
		turns.append(d)


# Geometry, recomputed every draw because the window can be any size: cell is
# whatever squares off inside the space left under the header, and the board is
# centred in the leftovers so a wide window does not leave it jammed left.
func _geom() -> Array:
	var cell: float = min((size.x - 16.0) / float(COLS), (size.y - TOP - 10.0) / float(ROWS))
	if cell < 2.0:
		return []
	var w := cell * COLS
	var h := cell * ROWS
	return [Vector2((size.x - w) * 0.5, TOP + (size.y - TOP - 10.0 - h) * 0.5), cell]


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), BG)
	draw_string(mono, Vector2(10, 20), "snake", HORIZONTAL_ALIGNMENT_LEFT, -1, 18, INK)
	draw_string(mono, Vector2(size.x - 170, 20), "score %d    best %d" % [score, best],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 12, INK)
	draw_string(mono, Vector2(10, 34), "arrows or wasd   P pauses   R restarts",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)

	var gm := _geom()
	if gm.is_empty():
		return
	var org: Vector2 = gm[0]
	var cell: float = gm[1]
	draw_rect(Rect2(org, Vector2(cell * COLS, cell * ROWS)), BOARD)
	# Grid lines only when a cell is big enough for them to be a hint rather
	# than a texture; below that they would fill in the whole board.
	if cell >= 9.0:
		for x in range(1, COLS):
			draw_line(org + Vector2(x * cell, 0), org + Vector2(x * cell, cell * ROWS), GRID, 1.0)
		for y in range(1, ROWS):
			draw_line(org + Vector2(0, y * cell), org + Vector2(cell * COLS, y * cell), GRID, 1.0)

	var inset: float = clamp(cell * 0.12, 0.5, 2.0)
	draw_rect(Rect2(org + Vector2(food.x * cell + inset, food.y * cell + inset),
		Vector2(cell - inset * 2.0, cell - inset * 2.0)), FOOD)

	for i in range(snake.size()):
		var c: Vector2i = snake[i]
		var col := HEAD if i == 0 else BODY
		if over:
			col = DEAD if i == 0 else col.lerp(BG, 0.45)
		draw_rect(Rect2(org + Vector2(c.x * cell + inset, c.y * cell + inset),
			Vector2(cell - inset * 2.0, cell - inset * 2.0)), col)

	draw_rect(Rect2(org, Vector2(cell * COLS, cell * ROWS)), FAINT, false, 1.0)

	if over or paused:
		var box := Rect2(org.x, org.y + cell * ROWS * 0.5 - 18.0, cell * COLS, 36.0)
		draw_rect(box, Color(0.95, 0.94, 0.91, 0.88))
		var msg := "you ate yourself -- R to try again" if over else "paused -- P to resume"
		if over and score == best and best > 0:
			msg = "best yet: %d -- R to try again" % best
		draw_string(mono, Vector2(box.position.x, box.position.y + 23.0), msg,
			HORIZONTAL_ALIGNMENT_CENTER, box.size.x, 13, INK)
