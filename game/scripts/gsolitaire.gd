# gsolitaire.gd — Klondike, dealt and enforced properly, with cards that are
# drawn rather than fetched.
#
# There are no image assets in this project and there never will be, so every
# card here is a rounded rectangle, a rank string and a suit built out of
# circles, a triangle and a stem. That constraint turned out to be the useful
# one: a suit made of primitives has to stay legible at a 30-pixel card, which
# is exactly the size this window gets when somebody tiles it next to a
# terminal, and a scanned card image would have gone to mush there.
#
# The rules are the real ones and they are enforced in one place, _can_drop:
# descending alternating colours on the tableau, kings only into an empty
# column, foundations from the ace up in suit, one card at a time onto them.
# Everything the mouse does routes through it, so there is no move the UI can
# make that the rules have not agreed to.
#
# Interaction is click-to-pick then click-to-place rather than drag. Dragging a
# thirteen-card run inside a 400-pixel window means holding a button while your
# pointer leaves the window, and this desktop's windows do not capture the
# mouse. Clicking a face-down card flips it if it is the bottom of its pile,
# and A shovels everything that can go home, home.
#
# Same contract as g2048.gd -- .new(), mono and machine set from outside, then
# take_focus(). The best time goes to /root/.solitaire through the machine's
# own shell, so `cat /root/.solitaire` shows the seconds the HUD shows.

extends Control

var mono: Font
var machine: Object = null

const TOP := 40.0
# Suit order is fixed everywhere: 0 spades, 1 hearts, 2 diamonds, 3 clubs.
# 1 and 2 are the red ones, which is the only thing the rules ask of it.
const RANKS := ["A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"]

const FELT := Color("#dfe6da")
const SLOT := Color("#cdd5c8")
const CARD := Color("#fbfaf6")
const BACK := Color("#7f93b0")
const BACK_LINE := Color("#63779a")
const EDGE := Color("#9b968a")
const INK := Color("#2f2a24")
const RED := Color("#b02f22")
const BLACK := Color("#2b2b2b")
const FAINT := Color("#6f6a5f")
const PICK := Color("#c8a02e")

var stock: Array = []        # dicts {"r": 0..12, "s": 0..3}
var waste: Array = []
var found: Array = []        # four piles, low to high
var tab: Array = []          # seven piles of {"c": card, "up": bool}
var draw_n := 1
var sel: Dictionary = {}     # {"p": "waste"/"tab"/"found", "i": int, "j": int}
var moves := 0
var elapsed := 0.0
var running := false
var won := false
var best := 0
var flyers: Array = []       # the win cascade; see _process
var fly_t := 0.0
var rng := RandomNumberGenerator.new()


func _ready() -> void:
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	clip_contents = true
	rng.randomize()
	if machine:
		var t: String = machine.sh_on(0, "cat /root/.solitaire")
		if t.strip_edges().is_valid_int():
			best = int(t.strip_edges())
	_deal()
	set_process(true)


func take_focus() -> void:
	grab_focus()


func _deal() -> void:
	var deck: Array = []
	for s in range(4):
		for r in range(13):
			deck.append({"r": r, "s": s})
	deck.shuffle()
	tab = []
	for i in range(7):
		var pile: Array = []
		for j in range(i + 1):
			# Only the last card of each column starts face up. That single
			# rule is where all of Klondike's information comes from.
			pile.append({"c": deck.pop_back(), "up": j == i})
		tab.append(pile)
	stock = deck
	waste = []
	found = [[], [], [], []]
	sel = {}
	moves = 0
	elapsed = 0.0
	running = false
	won = false
	flyers = []
	queue_redraw()


func _is_red(card: Dictionary) -> bool:
	return int(card["s"]) == 1 or int(card["s"]) == 2


# The whole rulebook. Everything the mouse does asks this first, so an illegal
# move is not merely refused by the UI -- it has no path into the state at all.
func _can_drop(card: Dictionary, dest: String, i: int) -> bool:
	if dest == "found":
		if not found[i].is_empty():
			var t: Dictionary = found[i][found[i].size() - 1]
			return int(card["s"]) == int(t["s"]) and int(card["r"]) == int(t["r"]) + 1
		# THE FOUNDATIONS ARE LABELLED, SO THEY HAVE TO MEAN IT. Each empty
		# slot is drawn with its own suit under it -- spade, heart, diamond,
		# club, left to right -- and any ace was accepted into any of them, so
		# the first ace off the deck landed in the spade slot whatever it was.
		# A playtester watched A(club) go home under a spade. Either the
		# placeholder is a promise or it should not be drawn.
		return int(card["r"]) == 0 and int(card["s"]) == i
	if dest == "tab":
		if tab[i].is_empty():
			return int(card["r"]) == 12          # kings only, into empty columns
		var e: Dictionary = tab[i][tab[i].size() - 1]
		if not e["up"]:
			return false
		var tc: Dictionary = e["c"]
		return int(card["r"]) == int(tc["r"]) - 1 and _is_red(card) != _is_red(tc)
	return false


func _start_clock() -> void:
	if not running and not won:
		running = true


func _process(dt: float) -> void:
	if running and not won:
		elapsed += dt
		queue_redraw()
	if won:
		_cascade(dt)


# The win cascade. A banner alone is a weak ending for a game that took four
# minutes, so the foundations empty themselves across the felt: one card every
# fly_t seconds, thrown sideways, bouncing off the bottom edge until it leaves
# the window. It is the classic ending and it costs thirty lines.
func _cascade(dt: float) -> void:
	fly_t -= dt
	if fly_t <= 0.0:
		fly_t = 0.12
		var pool: Array = []
		for i in range(4):
			if not found[i].is_empty():
				pool.append(i)
		if not pool.is_empty():
			var i: int = pool[rng.randi_range(0, pool.size() - 1)]
			var c: Dictionary = found[i].pop_back()
			var gm := _geom()
			flyers.append({
				"c": c,
				"p": Vector2(gm[3] + (3 + i) * (gm[0] + gm[2]), TOP + 6.0),
				"v": Vector2(rng.randf_range(-150.0, 150.0), rng.randf_range(-90.0, 10.0)),
			})
	var keep: Array = []
	for f in flyers:
		var v: Vector2 = f["v"]
		var p: Vector2 = f["p"]
		v.y += 620.0 * dt
		p += v * dt
		if p.y > size.y - 20.0 and v.y > 0.0:
			p.y = size.y - 20.0
			v.y = -v.y * 0.72
		f["v"] = v
		f["p"] = p
		if p.x > -120.0 and p.x < size.x + 120.0:
			keep.append(f)
	flyers = keep
	queue_redraw()


func _check_win() -> void:
	for i in range(4):
		if found[i].size() != 13:
			return
	won = true
	running = false
	fly_t = 0.0
	var secs := int(elapsed)
	if best == 0 or secs < best:
		best = secs
		if machine:
			machine.sh_on(0, 'echo "%d" > /root/.solitaire' % best)


# ---- geometry -------------------------------------------------------------
#
# Everything is derived from the card width, which is derived from the window,
# so there is exactly one number to get right. Returns
# [card w, card h, gap, left margin, tableau top, face-up fan, face-down fan].
func _geom() -> Array:
	var g: float = clamp(size.x * 0.012, 3.0, 10.0)
	var cw: float = (size.x - g * 8.0) / 7.0
	var ch: float = cw * 1.42
	var avail: float = size.y - TOP - 6.0
	# The top row plus a face-up column of at least six cards has to fit; if it
	# does not, the card shrinks until it does rather than the pile running off
	# the bottom of the window.
	var need: float = ch * 2.0 + g + 6.0 * 8.0
	if need > avail and avail > 40.0:
		ch = (avail - g - 48.0) / 2.0
		cw = ch / 1.42
	var left: float = (size.x - (cw * 7.0 + g * 6.0)) * 0.5
	var tab_y: float = TOP + 4.0 + ch + g * 1.6
	var deepest := 1
	for p in tab:
		deepest = max(deepest, (p as Array).size())
	var room: float = max(20.0, size.y - tab_y - ch - 4.0)
	var fan: float = clamp(room / float(max(1, deepest - 1)), 4.0, ch * 0.32)
	return [cw, ch, g, left, tab_y, fan, fan * 0.5]


func _slot_pos(kind: String, i: int) -> Vector2:
	var gm := _geom()
	var cw: float = gm[0]; var g: float = gm[2]; var left: float = gm[3]
	match kind:
		"stock": return Vector2(left, TOP + 4.0)
		"waste": return Vector2(left + cw + g, TOP + 4.0)
		"found": return Vector2(left + (3 + i) * (cw + g), TOP + 4.0)
	return Vector2(left + i * (cw + g), gm[4])


# The y of every card in a tableau pile, so hit testing and drawing can never
# disagree about where a card is.
func _tab_offsets(i: int) -> Array:
	var gm := _geom()
	var fan_u: float = gm[5]; var fan_d: float = gm[6]
	var ys: Array = []
	var y := 0.0
	for e in tab[i]:
		ys.append(y)
		y += fan_u if (e as Dictionary)["up"] else fan_d
	return ys


func _hit(pos: Vector2) -> Dictionary:
	var gm := _geom()
	var cw: float = gm[0]; var ch: float = gm[1]
	for kind in ["stock", "waste"]:
		if Rect2(_slot_pos(kind, 0), Vector2(cw, ch)).has_point(pos):
			return {"p": kind, "i": 0, "j": -1}
	for i in range(4):
		if Rect2(_slot_pos("found", i), Vector2(cw, ch)).has_point(pos):
			return {"p": "found", "i": i, "j": -1}
	for i in range(7):
		var base := _slot_pos("tab", i)
		var ys := _tab_offsets(i)
		# Back to front: the card on top of the fan is the one you clicked.
		for j in range(ys.size() - 1, -1, -1):
			if Rect2(base + Vector2(0, ys[j]), Vector2(cw, ch)).has_point(pos):
				return {"p": "tab", "i": i, "j": j}
		if tab[i].is_empty() and Rect2(base, Vector2(cw, ch)).has_point(pos):
			return {"p": "tab", "i": i, "j": -1}
	return {}


# ---- moves ----------------------------------------------------------------

func _selected_cards() -> Array:
	if sel.is_empty():
		return []
	match String(sel["p"]):
		"waste":
			if waste.is_empty():
				return []
			return [waste[waste.size() - 1]]
		"found":
			var f: Array = found[int(sel["i"])]
			if f.is_empty():
				return []
			return [f[f.size() - 1]]
		"tab":
			var pile: Array = tab[int(sel["i"])]
			var out: Array = []
			for j in range(int(sel["j"]), pile.size()):
				out.append((pile[j] as Dictionary)["c"])
			return out
	return []


func _take_selected() -> void:
	match String(sel["p"]):
		"waste": waste.pop_back()
		"found": found[int(sel["i"])].pop_back()
		"tab":
			var pile: Array = tab[int(sel["i"])]
			while pile.size() > int(sel["j"]):
				pile.pop_back()
			# Turning the newly exposed card is part of the move, not a
			# separate click: there is never a reason not to turn it.
			if not pile.is_empty():
				var e: Dictionary = pile[pile.size() - 1]
				e["up"] = true


func _try_place(dest: String, i: int) -> bool:
	var cards := _selected_cards()
	if cards.is_empty():
		return false
	if dest == "found" and cards.size() > 1:
		return false            # foundations take one card at a time, always
	if not _can_drop(cards[0], dest, i):
		return false
	if String(sel["p"]) == dest and int(sel["i"]) == i:
		return false
	_take_selected()
	for c in cards:
		if dest == "found":
			found[i].append(c)
		else:
			tab[i].append({"c": c, "up": true})
	moves += 1
	sel = {}
	_check_win()
	return true


# A single card sent home if anywhere will have it. Used by the auto key and by
# the double-click, which is how anyone actually finishes a game.
func _send_home(from: String, i: int, j: int) -> bool:
	var card: Dictionary
	if from == "waste":
		if waste.is_empty():
			return false
		card = waste[waste.size() - 1]
	else:
		var pile: Array = tab[i]
		if pile.is_empty() or j != pile.size() - 1 or not pile[j]["up"]:
			return false
		card = (pile[j] as Dictionary)["c"]
	for f in range(4):
		if _can_drop(card, "found", f):
			var keep := sel
			sel = {"p": from, "i": i, "j": j}
			var ok := _try_place("found", f)
			if not ok:
				sel = keep
			return ok
	return false


func _auto() -> bool:
	var any := false
	var spin := true
	while spin:
		spin = false
		if _send_home("waste", 0, -1):
			spin = true; any = true
		for i in range(7):
			if not tab[i].is_empty() and _send_home("tab", i, tab[i].size() - 1):
				spin = true; any = true
	return any


func _hit_stock() -> void:
	_start_clock()
	if stock.is_empty():
		# Recycling reverses the waste, which is what keeps a draw-three game
		# honest: the order you saw it in is the order it comes back in.
		while not waste.is_empty():
			stock.append(waste.pop_back())
	else:
		for n in range(draw_n):
			if stock.is_empty():
				break
			waste.append(stock.pop_back())
	sel = {}
	moves += 1


func _gui_input(e: InputEvent) -> void:
	if e is InputEventMouseButton and (e as InputEventMouseButton).pressed:
		grab_focus()
		var mb := e as InputEventMouseButton
		if mb.button_index != MOUSE_BUTTON_LEFT:
			sel = {}
			queue_redraw()
			return
		if won:
			return
		var h := _hit(mb.position)
		if h.is_empty():
			sel = {}
			queue_redraw()
			return
		var p := String(h["p"])
		if p == "stock":
			_hit_stock()
			queue_redraw()
			return
		_start_clock()
		if mb.double_click:
			# Double-click is "send this home", the move you make thirty times
			# at the end of a game and never want to aim for.
			if p == "waste":
				_send_home("waste", 0, -1)
			elif p == "tab":
				_send_home("tab", int(h["i"]), int(h["j"]))
			queue_redraw()
			return
		if p == "tab" and int(h["j"]) >= 0:
			var e2: Dictionary = tab[int(h["i"])][int(h["j"])]
			if not e2["up"]:
				# A face-down card is only ever turnable if it is the bottom of
				# the fan, and then it turns for free.
				if int(h["j"]) == tab[int(h["i"])].size() - 1:
					e2["up"] = true
					sel = {}
				queue_redraw()
				return
		if not sel.is_empty():
			if _try_place(p, int(h["i"])):
				queue_redraw()
				return
			if sel["p"] == p and int(sel["i"]) == int(h["i"]) and int(sel["j"]) == int(h["j"]):
				sel = {}                   # clicking the selection drops it
				queue_redraw()
				return
		if p == "waste" and waste.is_empty():
			sel = {}
		elif p == "found" and found[int(h["i"])].is_empty():
			sel = {}
		elif p == "tab" and int(h["j"]) < 0:
			sel = {}
		else:
			sel = {"p": p, "i": int(h["i"]), "j": int(h["j"])}
		queue_redraw()
		return
	if not (e is InputEventKey) or not (e as InputEventKey).pressed:
		return
	var k := e as InputEventKey
	accept_event()
	match k.keycode:
		KEY_R: _deal()
		KEY_A:
			if not won:
				_start_clock()
				_auto()
		KEY_1: draw_n = 1
		KEY_3: draw_n = 3
		KEY_ESCAPE: sel = {}
	queue_redraw()


# ---- drawing --------------------------------------------------------------
#
# A rounded rect out of two overlapping rectangles and four corner circles.
# Godot's draw_rect has no corner radius, and a card with square corners on a
# felt background reads as a tile, not a card.
func _round_rect(r: Rect2, rad: float, col: Color) -> void:
	var q: float = min(rad, min(r.size.x, r.size.y) * 0.5)
	draw_rect(Rect2(r.position + Vector2(q, 0), Vector2(r.size.x - q * 2.0, r.size.y)), col)
	draw_rect(Rect2(r.position + Vector2(0, q), Vector2(r.size.x, r.size.y - q * 2.0)), col)
	draw_circle(r.position + Vector2(q, q), q, col)
	draw_circle(r.position + Vector2(r.size.x - q, q), q, col)
	draw_circle(r.position + Vector2(q, r.size.y - q), q, col)
	draw_circle(r.position + Vector2(r.size.x - q, r.size.y - q), q, col)


# The four suits, built from primitives, centred on `c` and `h` tall. They are
# deliberately chunky: at a 26-pixel card the difference between a spade and a
# club has to survive being eight pixels across.
func _suit(c: Vector2, h: float, s: int, col: Color) -> void:
	var w := h * 0.9
	match s:
		1:   # heart: two lobes and a point
			draw_circle(c + Vector2(-w * 0.22, -h * 0.12), w * 0.27, col)
			draw_circle(c + Vector2(w * 0.22, -h * 0.12), w * 0.27, col)
			draw_polygon(PackedVector2Array([
				c + Vector2(-w * 0.48, -h * 0.06), c + Vector2(w * 0.48, -h * 0.06),
				c + Vector2(0, h * 0.46)]), PackedColorArray([col, col, col]))
		2:   # diamond
			draw_polygon(PackedVector2Array([
				c + Vector2(0, -h * 0.46), c + Vector2(w * 0.38, 0),
				c + Vector2(0, h * 0.46), c + Vector2(-w * 0.38, 0)]),
				PackedColorArray([col, col, col, col]))
		0:   # spade: heart upside down, on a stem
			draw_polygon(PackedVector2Array([
				c + Vector2(-w * 0.46, h * 0.1), c + Vector2(w * 0.46, h * 0.1),
				c + Vector2(0, -h * 0.46)]), PackedColorArray([col, col, col]))
			draw_circle(c + Vector2(-w * 0.24, h * 0.13), w * 0.26, col)
			draw_circle(c + Vector2(w * 0.24, h * 0.13), w * 0.26, col)
			draw_rect(Rect2(c + Vector2(-w * 0.08, h * 0.14), Vector2(w * 0.16, h * 0.3)), col)
		_:   # club: three lobes and a stem
			draw_circle(c + Vector2(0, -h * 0.2), w * 0.26, col)
			draw_circle(c + Vector2(-w * 0.28, h * 0.12), w * 0.26, col)
			draw_circle(c + Vector2(w * 0.28, h * 0.12), w * 0.26, col)
			draw_rect(Rect2(c + Vector2(-w * 0.08, h * 0.08), Vector2(w * 0.16, h * 0.36)), col)


func _draw_card(at: Vector2, cw: float, ch: float, card: Dictionary, marked: bool) -> void:
	var r := Rect2(at, Vector2(cw, ch))
	_round_rect(r, cw * 0.14, EDGE if not marked else PICK)
	_round_rect(Rect2(at + Vector2(1.5, 1.5), Vector2(cw - 3.0, ch - 3.0)), cw * 0.13, CARD)
	var col := RED if _is_red(card) else BLACK
	var fs: int = int(clamp(cw * 0.32, 8, 20))
	draw_string(mono, at + Vector2(cw * 0.11, fs + 1.0), RANKS[int(card["r"])],
		HORIZONTAL_ALIGNMENT_LEFT, -1, fs, col)
	# The corner pip goes next to the rank so a fanned pile, which shows only
	# its top strip, still identifies every card in it.
	_suit(at + Vector2(cw * 0.78, fs * 0.72), fs * 0.85, int(card["s"]), col)
	if ch > cw * 1.0:
		_suit(at + Vector2(cw * 0.5, ch * 0.66), min(cw * 0.62, ch * 0.42),
			int(card["s"]), col)


func _draw_back(at: Vector2, cw: float, ch: float) -> void:
	_round_rect(Rect2(at, Vector2(cw, ch)), cw * 0.14, EDGE)
	_round_rect(Rect2(at + Vector2(1.5, 1.5), Vector2(cw - 3.0, ch - 3.0)), cw * 0.13, BACK)
	# Diagonals clipped to the card by solving y = x - d against the edges, so
	# the hatching never spills past the rounded corner it started in.
	var inset := 3.0
	var iw: float = cw - inset * 2.0
	var ih: float = ch - inset * 2.0
	var step: float = max(4.0, cw * 0.22)
	var d := -ih
	while d < iw:
		var x1: float = max(0.0, d)
		var x2: float = min(iw, d + ih)
		if x2 > x1:
			draw_line(at + Vector2(inset + x1, inset + x1 - d),
				at + Vector2(inset + x2, inset + x2 - d), BACK_LINE, 1.0)
		d += step


func _slot(at: Vector2, cw: float, ch: float, glyph: String) -> void:
	_round_rect(Rect2(at, Vector2(cw, ch)), cw * 0.14, SLOT)
	if glyph != "":
		draw_string(mono, at + Vector2(0, ch * 0.55), glyph,
			HORIZONTAL_ALIGNMENT_CENTER, cw, int(clamp(cw * 0.3, 8, 16)), Color(0.42, 0.45, 0.4))


func _fmt(secs: int) -> String:
	return "%d:%02d" % [secs / 60, secs % 60]


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), FELT)
	draw_string(mono, Vector2(10, 20), "solitaire", HORIZONTAL_ALIGNMENT_LEFT, -1, 18, INK)
	var bs := "--" if best == 0 else _fmt(best)
	draw_string(mono, Vector2(size.x - 220, 20),
		"draw %d   moves %d   %s   best %s" % [draw_n, moves, _fmt(int(elapsed)), bs],
		HORIZONTAL_ALIGNMENT_LEFT, -1, 11, INK)
	draw_string(mono, Vector2(10, 34),
		"click to pick, click to place   double-click sends home   A auto   1/3 draw   R deals",
		HORIZONTAL_ALIGNMENT_LEFT, -1, 10, FAINT)

	var gm := _geom()
	var cw: float = gm[0]; var ch: float = gm[1]
	if cw < 16.0 or ch < 20.0:
		draw_string(mono, Vector2(10, TOP + 20), "window too small to deal",
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, FAINT)
		return

	var sp := _slot_pos("stock", 0)
	if stock.is_empty():
		_slot(sp, cw, ch, "O" if waste.is_empty() else "@")
	else:
		_draw_back(sp, cw, ch)

	var wp := _slot_pos("waste", 0)
	if waste.is_empty():
		_slot(wp, cw, ch, "")
	else:
		# Draw-three shows the last three, splayed, because which of them is
		# actually playable is information you need at a glance.
		var show: int = min(3 if draw_n == 3 else 1, waste.size())
		var off: float = cw * 0.22
		for n in range(show):
			var c: Dictionary = waste[waste.size() - show + n]
			var mark := (not sel.is_empty() and String(sel["p"]) == "waste"
				and n == show - 1)
			_draw_card(wp + Vector2(off * n, 0), cw, ch, c, mark)

	for i in range(4):
		var fp := _slot_pos("found", i)
		if found[i].is_empty():
			_slot(fp, cw, ch, "")
			_suit(fp + Vector2(cw * 0.5, ch * 0.5), min(cw, ch) * 0.4, i,
				Color(0.55, 0.58, 0.53))
		else:
			var mk := (not sel.is_empty() and String(sel["p"]) == "found"
				and int(sel["i"]) == i)
			_draw_card(fp, cw, ch, found[i][found[i].size() - 1], mk)

	for i in range(7):
		var base := _slot_pos("tab", i)
		if tab[i].is_empty():
			_slot(base, cw, ch, "")
			continue
		var ys := _tab_offsets(i)
		for j in range(tab[i].size()):
			var e: Dictionary = tab[i][j]
			var at := base + Vector2(0, ys[j])
			if not e["up"]:
				_draw_back(at, cw, ch)
			else:
				var mk2 := (not sel.is_empty() and String(sel["p"]) == "tab"
					and int(sel["i"]) == i and j >= int(sel["j"]))
				_draw_card(at, cw, ch, e["c"], mk2)

	for f in flyers:
		_draw_card(f["p"], cw, ch, f["c"], false)

	if won:
		var box := Rect2(size.x * 0.1, size.y * 0.4, size.x * 0.8, 44.0)
		draw_rect(box, Color(0.98, 0.97, 0.93, 0.9))
		draw_rect(box, EDGE, false, 1.0)
		draw_string(mono, box.position + Vector2(0, 20),
			"all fifty-two home in %s, %d moves" % [_fmt(int(elapsed)), moves],
			HORIZONTAL_ALIGNMENT_CENTER, box.size.x, 13, INK)
		draw_string(mono, box.position + Vector2(0, 36), "R deals another",
			HORIZONTAL_ALIGNMENT_CENTER, box.size.x, 10, FAINT)
