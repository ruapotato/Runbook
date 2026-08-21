# desk.gd — the desktop, as a support engineer would find it.
#
# Modelled on MATE, and on ~/NOMINAL/game/scripts/de.gd which was modelled on
# the same thing: light panels top AND bottom, a blue wallpaper, an
# Applications / Places / System menu bar in the top-left, a clock in the
# top-right, a window list along the bottom with a show-desktop button before
# it and a workspace pager after it. Bright, not dark. Classic, not
# fashionable.
#
# THE METAPHOR IS LOAD-BEARING, NOT NOSTALGIA. The player is the entire IT
# department of a growing company, and the thing they sit in front of is a
# workstation running an unremarkable corporate desktop. Every appliance is a
# window; the queue is a window; the terminal is a window. Act II happens when
# the player realises the terminal has been in the Applications menu since the
# first morning and everything the forms do goes through it.
#
# WHAT DID NOT COME ACROSS FROM NOMINAL: the ship, the customer, the phone
# call, the fourteen little games. That desktop ran ON an emulated machine and
# its windows were that machine's programs. This one is a workstation, and its
# windows are the work. There is no machine under it yet; there will be at M4,
# when the player's scripts have to run somewhere real.
#
# EVERYTHING HERE IS A CLIENT OF THE API (decision 7). Every window paints what
# RunbookWorld.exec() returned. The desktop has no privileged access, cannot
# see a struct, and physically cannot do anything a player's script could not.
extends Control

const UiFont := preload("res://scripts/uifont.gd")
# THE ICON SET, LIFTED WHOLE from ~/NOMINAL/game/scripts/icons.gd.
#
# Twenty-odd icons authored in normalised coordinates against one palette, one
# corner treatment and one border weight, so a folder and a terminal look like
# they weigh the same in a column. It was already written, already argued
# about, and rebuilding it would have been exactly the yak-shave decision 3
# warns against. Shapes, not letters: a set that spells out its meaning in
# glyphs is a set that gave up.
const Icons  := preload("res://scripts/icons.gd")
const Clip   := preload("res://scripts/clip.gd")
const Queue  := preload("res://scripts/queue.gd")
const WebUI  := preload("res://scripts/webui.gd")
# NOMINAL's terminal, not the one I wrote and then had to throw away. See the
# note at the top of that file.
const Term   := preload("res://scripts/terminal.gd")
const Files  := preload("res://scripts/files.gd")

# --- palette: light panels, blue wall (MATE's default, near enough) ---
# NOMINAL's palette, verbatim, because it was already right: light panels, a
# blue wall, a blue titlebar on the focused window and a grey one on the rest.
# The icon set in icons.gd was authored against these nine colours, so drifting
# away from them is how a desktop stops reading as one thing.
const PANEL_BG   := Color("#d8d8d8")
const PANEL_HI   := Color("#eaeaea")
const PANEL_EDGE := Color("#9aa0a6")
const PANEL_INK  := Color("#1b1b1b")
const MENU_HOT   := Color("#3c6eb4")
const WALL_TOP   := Color("#3f6699")
const WALL_BOT   := Color("#1d3050")
const WIN_BG     := Color("#ededed")
const WIN_EDGE   := Color("#8b929b")
const TITLE_ON   := Color("#3c6eb4")
const TITLE_OFF  := Color("#b6bcc4")
const TITLE_INK  := Color("#ffffff")
const TITLE_INK2 := Color("#48505a")
const TOP_H      := 25.0
const FOOT_H     := 27.0
const TITLE_H    := 22.0
const WORKSPACES := 4

var api: RunbookApi
var mono: Font
var top: Control
var foot: Control
var windows: Array = []
var focused: Control = null
var menu_open := -1          # which top-level menu, -1 for none
var drag: Control = null
var drag_off := Vector2.ZERO
var sizing: Control = null
var cascade := 0
var boot_error := ""
var workspace := 0
var desktop_shown := true

# WINDOWS REMEMBER THE SIZE YOU GAVE THEM, per application, for the session.
# Straight from de.gd, and for the reason it gives: anything you have to do
# twice in a session you will do ten times in a shift. Position is not
# remembered -- that is the cascade's job, and a remembered position is how
# two windows end up exactly on top of each other.
var _geom := {}

# --------------------------------------------------------------- the menus
# Applications, Places, System. Places lists the appliances, because in this
# game the places you go ARE the appliances -- and it is discovered from the
# API, so buying a second mail server puts it in the menu. That is the whole
# of Act III arriving in the furniture.
const MENUS := ["Applications", "Places", "System"]

# IDEMPOTENT, AND CALLED FROM BOTH ENDS.
#
# A SceneTree script that builds the desktop inside its own _init() adds it to
# a tree that is not ready yet, so _ready() is deferred and every _launch()
# before the next frame runs against a null api. The gate hit exactly that and
# reported "three windows opened: FAIL" with no windows and no explanation.
# Guarding the point of use rather than each caller is the difference between
# a bug fixed and a bug that comes back the next time somebody adds one.
func _ensure() -> void:
	if api != null:
		return
	mono = UiFont.mono()
	api = RunbookApi.new()
	if not api.ready():
		boot_error = api.last_error if api.last_error != "" else "the world did not boot"

	# THE PANELS ARE PART OF EXISTING, not part of being ready.
	#
	# They were built in _ready(), which meant anything that launched a window
	# before the first frame -- the gate does exactly that -- ran against a
	# null panel and died three calls deep in _tasklist(). Whatever the
	# desktop needs in order to answer a question belongs here, where the
	# first question builds it.
	top = Control.new()
	top.mouse_filter = Control.MOUSE_FILTER_STOP
	top.draw.connect(_draw_top)
	top.gui_input.connect(_top_input)
	add_child(top)

	foot = Control.new()
	foot.mouse_filter = Control.MOUSE_FILTER_STOP
	foot.draw.connect(_draw_foot)
	foot.gui_input.connect(_foot_input)
	add_child(foot)
	_relayout_desktop()

func _ready() -> void:
	_ensure()
	set_process_input(true)
	resized.connect(_relayout_desktop)
	_relayout_desktop()
	if boot_error == "":
		_launch("Queue")

var _clock_cache: Dictionary = {}
var _clock_age := 99.0
var _last_minute := -1
var _clock_flash := 0.0

func _process(dt: float) -> void:
	# THE CLOCK ONLY MOVES WHEN YOU SPEND TIME, and that is the design (§10):
	# the day advances at the speed of the fiction, not the wall. But a bar
	# that never moves while you are reading a ticket looks broken rather than
	# paused, so when it DOES move it flashes -- which is also the only
	# feedback the desktop gives that a form cost you two minutes.
	if api == null or top == null:
		return
	_clock_age += dt
	_cwd_age += dt
	if _clock_age > 0.25:
		_clock_age = 0.0
		var info: Array = api.objects(api.exec("world.info"))
		if info.size() > 0:
			_clock_cache = info[0]
			var m := int(str(_clock_cache.get("minute", "0")))
			if _last_minute >= 0 and m != _last_minute:
				_clock_flash = 0.9
			_last_minute = m
		top.queue_redraw()
	if _clock_flash > 0.0:
		_clock_flash = maxf(0.0, _clock_flash - dt)
		top.queue_redraw()

func _relayout_desktop() -> void:
	if size.x < 2.0:
		return
	if top:
		top.position = Vector2.ZERO
		top.size = Vector2(size.x, TOP_H)
	if foot:
		foot.position = Vector2(0, size.y - FOOT_H)
		foot.size = Vector2(size.x, FOOT_H)
	queue_redraw()

# ------------------------------------------------------------------ the wall
func _draw() -> void:
	# Seventy-two bands, as in de.gd. Enough that the banding is invisible at
	# any window size anybody plays at, cheap enough to repaint every frame.
	for i in range(72):
		var t := float(i) / 71.0
		draw_rect(Rect2(0, size.y * t, size.x, size.y / 71.0 + 1.0),
			WALL_TOP.lerp(WALL_BOT, t))

	# THE ONE THING THE DESKTOP SAYS FOR ITSELF, and it is a warning, not
	# decoration. A client painting an empty desktop because the extension did
	# not load looks exactly like a client painting an empty desktop because
	# nothing has happened yet. NOMINAL's suite stayed green for months on that
	# distinction; this says which it is, in the middle of the screen.
	if boot_error != "":
		draw_string(mono, Vector2(40, size.y * 0.5), "RUNBOOK could not start: %s" % boot_error,
			HORIZONTAL_ALIGNMENT_LEFT, size.x - 80, 16, Color("#ffd0d0"))
		draw_string(mono, Vector2(40, size.y * 0.5 + 24), "Build it with:  make gdext",
			HORIZONTAL_ALIGNMENT_LEFT, size.x - 80, 13, Color("#e8e8e8"))
		return

	_draw_desktop_icons()
	if menu_open >= 0:
		_draw_menu()

# Desktop icons, down the left, like every corporate desktop anyone has ever
# been handed. Double-click opens; single-click selects, which is what the
# highlight is for.
var icon_sel := -1

func _icon_rects() -> Array:
	var out := []
	var y := TOP_H + 12.0
	for i in range(_desktop_items().size()):
		out.append(Rect2(12, y, 92, 56))
		y += 62.0
	return out

# An appliance's icon comes from its KIND, so a new appliance model gets a
# sensible one without anybody drawing anything: a directory is a filing
# system, mail is a conversation, a file server is folders.
static func _icon_for(kind: String) -> String:
	match kind:
		"Queue":      return "notes"
		"Terminal":   return "term"
		"directory":  return "svc"
		"mail":       return "chat"
		"fileserver": return "files"
		"printer":    return "pkg"
		_:            return "app"

func _desktop_items() -> Array:
	_ensure()
	var out := [{"label": "Queue", "kind": "Queue", "icon": "notes"},
				{"label": "Terminal", "kind": "Terminal", "icon": "term"},
				{"label": "Files", "kind": "Files", "icon": "files"}]
	for raw in api.objects(api.exec("appl.list")):
		var a: Dictionary = raw
		out.append({"label": str(a.get("id", "?")), "kind": "appl:%s" % a.get("id", "?"),
					"icon": _icon_for(str(a.get("kind", "")))})
	return out

func _draw_desktop_icons() -> void:
	var items := _desktop_items()
	var rects := _icon_rects()
	for i in range(items.size()):
		var r: Rect2 = rects[i]
		if i == icon_sel:
			draw_rect(r, Color(1, 1, 1, 0.18))
			draw_rect(r, Color(1, 1, 1, 0.35), false, 1.0)
		var it: Dictionary = items[i]
		Icons.draw_icon(self, Vector2(r.position.x + (r.size.x - 34.0) * 0.5, r.position.y + 2), 34.0,
			str(it.get("icon", "app")))
		# A drop shadow under desktop labels, because white text on a mid-blue
		# wallpaper is unreadable at 11px without one. Every desktop that has
		# ever existed does this.
		draw_string(mono, Vector2(r.position.x + 1, r.position.y + 47), str(it["label"]),
			HORIZONTAL_ALIGNMENT_CENTER, r.size.x, 11, Color(0, 0, 0, 0.55))
		draw_string(mono, Vector2(r.position.x, r.position.y + 46), str(it["label"]),
			HORIZONTAL_ALIGNMENT_CENTER, r.size.x, 11, Color("#ffffff"))

# ----------------------------------------------------------- the top panel
func _menu_tabs() -> Array:
	var out := []
	var x := 6.0
	for m in MENUS:
		var w := mono.get_string_size(str(m), HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x + 16.0
		out.append(Rect2(x, 2, w, TOP_H - 4))
		x += w
	return out

func _draw_top() -> void:
	top.draw_rect(Rect2(0, 0, top.size.x, TOP_H), PANEL_BG)
	top.draw_line(Vector2(0, 0), Vector2(top.size.x, 0), PANEL_HI, 1.0)
	top.draw_line(Vector2(0, TOP_H - 1), Vector2(top.size.x, TOP_H - 1), PANEL_EDGE, 1.0)

	var tabs := _menu_tabs()
	for i in range(MENUS.size()):
		var r: Rect2 = tabs[i]
		if i == menu_open:
			top.draw_rect(r, MENU_HOT)
		top.draw_string(mono, Vector2(r.position.x + 8, 17), str(MENUS[i]),
			HORIZONTAL_ALIGNMENT_LEFT, -1, 12,
			Color.WHITE if i == menu_open else PANEL_INK)

	# The clock, MATE-style: right-hand end of the top panel. Here it is the
	# in-game clock, because the day budget is the only currency there is and
	# a player should never have to go looking for how much of it is left.
	if not _clock_cache.is_empty():
		var i0: Dictionary = _clock_cache
		var mins := int(str(i0.get("minute", "0")))
		var left := int(str(i0.get("minutes_left", "480")))
		# THE RIGHT-HAND END, MEASURED RATHER THAN GUESSED, and laid out from
		# the edge inwards. The first version put the bar at a fixed offset
		# from the clock and the label at a fixed offset from the bar, and
		# "480 min left" came out as "480 min lef" with the bar painted over
		# the last letter. Panels are the one place where measuring is not
		# optional: the strings change every minute.
		var clock := "Day %s  %02d:%02d" % [i0.get("day", "?"), 9 + int(mins / 60.0), mins % 60]
		var cw := mono.get_string_size(clock, HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x
		var label := "%d min left" % left
		var lw := mono.get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, 11).x
		var x := top.size.x - 10.0
		top.draw_string(mono, Vector2(x - cw, 17), clock, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, PANEL_INK)
		x -= cw + 12.0
		top.draw_line(Vector2(x, 4), Vector2(x, TOP_H - 4), PANEL_EDGE, 1.0)
		x -= 10.0

		# The day, as a bar, in the notification area. It goes amber under the
		# last hour -- which is not a warning about failure, because there is
		# no failure here. It is a clock.
		var bw := 74.0
		top.draw_rect(Rect2(x - bw, 7, bw, 11), Color("#c9c5be"))
		var frac: float = clampf(float(left) / 480.0, 0.0, 1.0)
		var barcol: Color = MENU_HOT if left > 60 else Color("#d98a2b")
		if _clock_flash > 0.0:
			barcol = barcol.lerp(Color.WHITE, _clock_flash * 0.7)
		top.draw_rect(Rect2(x - bw, 7, bw * frac, 11), barcol)
		top.draw_rect(Rect2(x - bw, 7, bw, 11), PANEL_EDGE, false, 1.0)
		x -= bw + 8.0
		top.draw_string(mono, Vector2(x - lw, 17), label,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, PANEL_INK)

func _top_input(e: InputEvent) -> void:
	if not (e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT):
		return
	var tabs := _menu_tabs()
	for i in range(tabs.size()):
		if (tabs[i] as Rect2).has_point(e.position):
			menu_open = -1 if menu_open == i else i
			queue_redraw()
			return
	menu_open = -1
	queue_redraw()

# --------------------------------------------------------- the menu itself
# THE GAMES, AND WHY THEY ARE NOT A JOKE.
#
# Lifted whole from NOMINAL, which had ten of them and was right to. Every
# corporate desktop that has ever been issued to anybody had solitaire and
# minesweeper on it; a workstation without them is a workstation nobody
# believes in.
#
# And they are the win condition, sitting there in the menu from the first
# morning. This game is about making yourself progressively unnecessary, and
# it is won by going on holiday for seven days (§12). The moment a player
# leaves a script running, opens Solitaire, and watches the queue empty itself
# in the window behind it is the moment the whole design is arguing for.
# Nothing else in the game can say that as economically as a deck of cards.
const GAMES := [
	{"label": "Solitaire",  "kind": "game:gsolitaire", "icon": "cards"},
	{"label": "Mines",      "kind": "game:gmines",     "icon": "mines"},
	{"label": "Blocks",     "kind": "game:gblocks",    "icon": "blocks"},
	{"label": "Snake",      "kind": "game:gsnake",     "icon": "snake"},
	{"label": "2048",       "kind": "game:g2048",      "icon": "tiles"},
	{"label": "Worms",      "kind": "game:gworms",     "icon": "worms"},
	{"label": "Flappy",     "kind": "game:gflappy",    "icon": "flappy"},
	{"label": "Liquid",     "kind": "game:gliquid",    "icon": "liquid"},
	{"label": "Sand",       "kind": "game:gsand",      "icon": "sand"},
	{"label": "Sandtris",   "kind": "game:gsetris",    "icon": "sandtris"},
]

func _menu_items(which: int) -> Array:
	match which:
		0:
			var apps := [{"label": "Ticket queue", "kind": "Queue", "icon": "notes"},
						 {"label": "Terminal", "kind": "Terminal", "icon": "term"},
						 {"label": "Files", "kind": "Files", "icon": "files"},
						 {"label": "— Games —", "kind": "", "icon": "game"}]
			apps.append_array(GAMES)
			return apps
		1:
			var out := []
			for raw in api.objects(api.exec("appl.list")):
				var a: Dictionary = raw
				out.append({"label": str(a.get("id", "?")), "kind": "appl:%s" % a.get("id", "?"),
							"icon": _icon_for(str(a.get("kind", ""))),
							"sub": str(a.get("model", ""))})
			return out
		_:
			return [{"label": "Go home (end the day)", "kind": "sys:day", "icon": "clock"},
					{"label": "How the queue works", "kind": "sys:help", "icon": "manual"},
					{"label": "The API, in full", "kind": "sys:api", "icon": "term"},
					{"label": "Where the org stands", "kind": "sys:stats", "icon": "sysmon"},
					{"label": "Quit", "kind": "sys:quit", "icon": "app"}]

func _menu_rect() -> Rect2:
	if menu_open < 0:
		return Rect2()
	var tabs := _menu_tabs()
	var items := _menu_items(menu_open)
	var w := 190.0
	for raw in items:
		var it: Dictionary = raw
		var lw := mono.get_string_size(str(it["label"]), HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x
		if it.has("sub"):
			lw += mono.get_string_size(str(it["sub"]), HORIZONTAL_ALIGNMENT_LEFT, -1, 10).x + 24.0
		w = maxf(w, lw + 50.0)
	return Rect2((tabs[menu_open] as Rect2).position.x, TOP_H, w, 6 + items.size() * 22.0)

func _draw_menu() -> void:
	var r := _menu_rect()
	draw_rect(Rect2(r.position + Vector2(2, 2), r.size), Color(0, 0, 0, 0.22))
	draw_rect(r, PANEL_HI)
	draw_rect(r, PANEL_EDGE, false, 1.0)
	var items := _menu_items(menu_open)
	var y := r.position.y + 17.0
	for raw in items:
		var it: Dictionary = raw
		var sep := str(it.get("kind", "")) == ""
		if sep:
			draw_line(Vector2(r.position.x + 6, y - 5), Vector2(r.position.x + r.size.x - 6, y - 5),
				Color("#c4bfb7"), 1.0)
		else:
			Icons.draw_icon(self, Vector2(r.position.x + 6, y - 12), 15.0, str(it.get("icon", "app")))
		draw_string(mono, Vector2(r.position.x + 26, y), str(it["label"]),
			HORIZONTAL_ALIGNMENT_LEFT, r.size.x - 36, 12,
			Color("#8a857d") if sep else PANEL_INK)
		if it.has("sub"):
			draw_string(mono, Vector2(r.position.x + r.size.x - 8, y), str(it["sub"]),
				HORIZONTAL_ALIGNMENT_RIGHT, 200, 10, Color("#7b756c"))
		y += 22.0

# -------------------------------------------------------- the bottom panel
func _show_desktop_rect() -> Rect2: return Rect2(4, 4, 20, FOOT_H - 8)

func _pager_rect() -> Rect2:
	if foot == null:
		return Rect2()
	return Rect2(foot.size.x - (WORKSPACES * 22 + 8), 5, WORKSPACES * 22, FOOT_H - 10)

func _tasklist() -> Array:
	if foot == null:
		return []
	# One button per window ON THIS WORKSPACE. MATE shows only the current
	# workspace's windows, and so does this: a pager that changed nothing
	# would be furniture.
	var out := []
	var x := 30.0
	var avail: float = _pager_rect().position.x - 36.0
	var mine := []
	for raw in windows:
		var w: Control = raw
		if is_instance_valid(w) and int(w.get_meta("ws")) == workspace:
			mine.append(w)
	if mine.is_empty():
		return out
	var each: float = minf(178.0, maxf(60.0, avail / float(mine.size()) - 3.0))
	for raw in mine:
		var w: Control = raw
		out.append({"win": w, "rect": Rect2(x, 4, each, FOOT_H - 8)})
		x += each + 3.0
	return out

func _draw_foot() -> void:
	foot.draw_rect(Rect2(0, 0, foot.size.x, FOOT_H), PANEL_BG)
	foot.draw_line(Vector2(0, 0), Vector2(foot.size.x, 0), PANEL_HI, 1.0)

	var sd := _show_desktop_rect()
	foot.draw_rect(sd, PANEL_HI)
	foot.draw_rect(sd, PANEL_EDGE, false, 1.0)
	foot.draw_line(sd.position + Vector2(4, 5), sd.position + Vector2(sd.size.x - 4, 5), PANEL_INK, 1.0)
	foot.draw_line(sd.position + Vector2(4, 9), sd.position + Vector2(sd.size.x - 4, 9), PANEL_INK, 1.0)

	for raw in _tasklist():
		var b: Dictionary = raw
		var w: Control = b["win"]
		var r: Rect2 = b["rect"]
		var on: bool = w.visible and focused == w
		foot.draw_rect(r, PANEL_HI if on else PANEL_BG)
		foot.draw_rect(r, PANEL_EDGE, false, 1.0)
		if on:
			foot.draw_line(r.position + Vector2(1, 1), r.position + Vector2(r.size.x - 1, 1), MENU_HOT, 2.0)
		Icons.draw_icon(foot, Vector2(r.position.x + 4, r.position.y + 3), 14.0, str(w.get_meta("icon", "app")))
		var label := str(w.get_meta("title"))
		if not w.visible:
			label = "[%s]" % label
		foot.draw_string(mono, Vector2(r.position.x + 22, r.position.y + 15), label,
			HORIZONTAL_ALIGNMENT_LEFT, r.size.x - 28, 11, PANEL_INK)

	var pr := _pager_rect()
	for i in range(WORKSPACES):
		var cell := Rect2(pr.position.x + i * 22, pr.position.y, 20, pr.size.y)
		foot.draw_rect(cell, MENU_HOT if i == workspace else Color("#b9b5ae"))
		foot.draw_rect(cell, PANEL_EDGE, false, 1.0)
		# A dot per window living on that workspace, which is what a pager is
		# actually for: knowing where you left the thing.
		var n := 0
		for raw in windows:
			var w: Control = raw
			if is_instance_valid(w) and int(w.get_meta("ws")) == i:
				n += 1
		for k in range(mini(n, 3)):
			foot.draw_rect(Rect2(cell.position.x + 3 + k * 5, cell.position.y + 4, 3, 3),
				Color.WHITE if i == workspace else Color("#5c5750"))

func _foot_input(e: InputEvent) -> void:
	if not (e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT):
		return
	if _show_desktop_rect().has_point(e.position):
		_toggle_show_desktop()
		return
	var pr := _pager_rect()
	if pr.has_point(e.position):
		_go_workspace(int((e.position.x - pr.position.x) / 22.0))
		return
	for raw in _tasklist():
		var b: Dictionary = raw
		if (b["rect"] as Rect2).has_point(e.position):
			var w: Control = b["win"]
			if w.visible and focused == w:
				w.visible = false
			else:
				w.visible = true
				_raise(w)
			foot.queue_redraw()
			return

func _toggle_show_desktop() -> void:
	desktop_shown = not desktop_shown
	for raw in windows:
		var w: Control = raw
		if is_instance_valid(w) and int(w.get_meta("ws")) == workspace:
			w.visible = not desktop_shown or not w.get_meta("was_visible", true)
			if desktop_shown:
				w.visible = bool(w.get_meta("was_visible", true))
			else:
				w.set_meta("was_visible", w.visible)
				w.visible = false
	foot.queue_redraw()

func _go_workspace(n: int) -> void:
	workspace = clampi(n, 0, WORKSPACES - 1)
	desktop_shown = true
	for raw in windows:
		var w: Control = raw
		if is_instance_valid(w):
			w.visible = int(w.get_meta("ws")) == workspace and bool(w.get_meta("shown", true))
	foot.queue_redraw()
	queue_redraw()

# ---------------------------------------------------------------- windows
func _win(title: String, rect: Rect2, content: Control, icon: String = "app") -> Control:
	var key := title
	if _geom.has(key):
		rect.size = _geom[key]
	rect.size.x = minf(rect.size.x, size.x - 16.0)
	rect.size.y = minf(rect.size.y, size.y - TOP_H - FOOT_H - 8.0)
	var w := Control.new()
	w.position = rect.position
	w.size = rect.size
	w.set_meta("title", title)
	w.set_meta("key", key)
	w.set_meta("icon", icon)
	w.set_meta("ws", workspace)
	w.set_meta("shown", true)
	w.set_meta("maxed", false)
	w.draw.connect(func(): _draw_win(w))
	add_child(w)

	var bar := Control.new()
	bar.size = Vector2(rect.size.x, TITLE_H)
	bar.gui_input.connect(func(ev): _bar_input(w, ev))
	w.add_child(bar)
	w.set_meta("bar", bar)

	var grip := Control.new()
	grip.size = Vector2(18, 18)
	grip.position = Vector2(rect.size.x - 18, rect.size.y - 18)
	grip.gui_input.connect(func(ev): _grip_input(w, ev))
	w.add_child(grip)
	w.set_meta("grip", grip)

	# CLIP. Without this a control paints wherever it likes, and a window that
	# draws past its own rect scribbles on the desktop. A window is a window
	# because it has edges. (de.gd learned this from a falling-pipes game.)
	content.clip_contents = true
	content.position = Vector2(3, TITLE_H + 2)
	content.size = Vector2(rect.size.x - 6, rect.size.y - TITLE_H - 5)
	content.mouse_filter = Control.MOUSE_FILTER_STOP
	w.add_child(content)
	w.set_meta("content", content)
	windows.append(w)
	_raise(w)
	return w

# Three buttons, right-hand end of the title bar, in MATE's order: minimise,
# maximise, close.
func _btn_rects(w: Control) -> Array:
	var y := 4.0
	return [Rect2(w.size.x - 60, y, 15, 14), Rect2(w.size.x - 42, y, 15, 14), Rect2(w.size.x - 22, y, 15, 14)]

func _draw_win(w: Control) -> void:
	var on: bool = focused == w
	w.draw_rect(Rect2(0, 0, w.size.x, w.size.y), WIN_BG)
	w.draw_rect(Rect2(0, 0, w.size.x, w.size.y), WIN_EDGE, false, 1.0)
	w.draw_rect(Rect2(1, 1, w.size.x - 2, TITLE_H - 1), TITLE_ON if on else TITLE_OFF)
	Icons.draw_icon(w, Vector2(4, 4), 14.0, str(w.get_meta("icon", "app")))
	w.draw_string(mono, Vector2(23, 16), str(w.get_meta("title")),
		HORIZONTAL_ALIGNMENT_LEFT, w.size.x - 94, 12, TITLE_INK if on else TITLE_INK2)

	var ink: Color = TITLE_INK if on else TITLE_INK2
	var b := _btn_rects(w)
	for i in range(3):
		var r: Rect2 = b[i]
		w.draw_rect(r, Color(1, 1, 1, 0.16 if on else 0.0))
		w.draw_rect(r, ink, false, 1.0)
		match i:
			0:  # minimise
				w.draw_line(r.position + Vector2(3, r.size.y - 4), r.position + Vector2(r.size.x - 3, r.size.y - 4), ink, 1.0)
			1:  # maximise
				w.draw_rect(Rect2(r.position + Vector2(3, 3), Vector2(r.size.x - 6, r.size.y - 6)), ink, false, 1.0)
			_:  # close
				w.draw_line(r.position + Vector2(4, 4), r.position + Vector2(r.size.x - 4, r.size.y - 4), ink, 1.0)
				w.draw_line(r.position + Vector2(r.size.x - 4, 4), r.position + Vector2(4, r.size.y - 4), ink, 1.0)

	for i in range(3):
		var d := 4.0 + i * 4.0
		w.draw_line(Vector2(w.size.x - d, w.size.y - 2), Vector2(w.size.x - 2, w.size.y - d), WIN_EDGE, 1.0)

func _relayout_win(w: Control) -> void:
	if w.has_meta("key") and not bool(w.get_meta("maxed")):
		_geom[w.get_meta("key")] = w.size
	(w.get_meta("bar") as Control).size = Vector2(w.size.x, TITLE_H)
	(w.get_meta("grip") as Control).position = Vector2(w.size.x - 18, w.size.y - 18)
	(w.get_meta("content") as Control).size = Vector2(w.size.x - 6, w.size.y - TITLE_H - 5)
	w.queue_redraw()

func _raise(w: Control) -> void:
	move_child(w, get_child_count() - 1)
	move_child(top, get_child_count() - 1)
	move_child(foot, get_child_count() - 1)
	focused = w
	for raw in windows:
		var x: Control = raw
		if is_instance_valid(x):
			x.queue_redraw()
	if foot: foot.queue_redraw()

func _bar_input(w: Control, e: InputEvent) -> void:
	if not (e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT):
		return
	_raise(w)
	var b := _btn_rects(w)
	if (b[2] as Rect2).has_point(e.position):
		windows.erase(w)
		w.queue_free()
		if foot: foot.queue_redraw()
		return
	if (b[1] as Rect2).has_point(e.position):
		_toggle_max(w)
		return
	if (b[0] as Rect2).has_point(e.position):
		w.visible = false
		w.set_meta("shown", false)
		if foot: foot.queue_redraw()
		return
	if e.double_click:
		_toggle_max(w)
		return
	drag = w
	drag_off = e.position

func _toggle_max(w: Control) -> void:
	if bool(w.get_meta("maxed")):
		w.position = w.get_meta("restore_pos")
		w.size = w.get_meta("restore_size")
		w.set_meta("maxed", false)
	else:
		w.set_meta("restore_pos", w.position)
		w.set_meta("restore_size", w.size)
		w.position = Vector2(0, TOP_H)
		w.size = Vector2(size.x, size.y - TOP_H - FOOT_H)
		w.set_meta("maxed", true)
	_relayout_win(w)

func _grip_input(w: Control, e: InputEvent) -> void:
	if e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT:
		_raise(w)
		w.set_meta("maxed", false)
		sizing = w

func _input(e: InputEvent) -> void:
	if menu_open >= 0 and e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT:
		var r := _menu_rect()
		if r.has_point(e.position):
			var i := int((e.position.y - r.position.y - 3.0) / 22.0)
			var items := _menu_items(menu_open)
			if i >= 0 and i < items.size():
				_activate(str((items[i] as Dictionary)["kind"]))
			menu_open = -1
			queue_redraw()
			accept_event()
			return
		if e.position.y > TOP_H:
			menu_open = -1
			queue_redraw()

	if e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT \
	   and e.position.y > TOP_H and e.position.y < size.y - FOOT_H:
		var rects := _icon_rects()
		var hit := -1
		for i in range(rects.size()):
			if (rects[i] as Rect2).has_point(e.position):
				hit = i
		# Only when the click landed on the wallpaper rather than a window --
		# windows are drawn after and eat their own input, so anything that
		# reaches here with no window under it is a desktop click.
		var over_window := false
		for raw in windows:
			var w: Control = raw
			if is_instance_valid(w) and w.visible and Rect2(w.position, w.size).has_point(e.position):
				over_window = true
		if not over_window:
			if hit >= 0 and (icon_sel == hit or e.double_click):
				_activate(str((_desktop_items()[hit] as Dictionary)["kind"]))
				icon_sel = -1
			else:
				icon_sel = hit
			queue_redraw()

	if e is InputEventMouseMotion and drag != null:
		drag.position = e.position - drag_off
		drag.position.y = clampf(drag.position.y, TOP_H, size.y - FOOT_H - TITLE_H)
		queue_redraw()
	elif e is InputEventMouseMotion and sizing != null:
		sizing.size = (e.position - sizing.position).max(Vector2(240, 130))
		_relayout_win(sizing)
	elif e is InputEventMouseButton and not e.pressed:
		drag = null
		sizing = null

	# The queue's ticket list is text somebody will want to copy -- a login, a
	# user id, a share name. Selecting inside a drawn Control is more than
	# this desktop needs, but a click on a ticket putting its id in PRIMARY
	# costs one line and saves retyping "TCK-00042" into the terminal.
	if e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT:
		for raw in windows:
			var w: Control = raw
			if not is_instance_valid(w) or not w.visible:
				continue
			var c: Node = w.get_meta("content")
			if c.has_method("selected_text") and Rect2(w.position, w.size).has_point(e.position):
				var s := str(c.selected_text())
				if s != "":
					Clip.set_primary(s)

func _find_window(key: String) -> Control:
	for raw in windows:
		var w: Control = raw
		if is_instance_valid(w) and str(w.get_meta("key")) == key:
			return w
	return null

func _activate(kind: String) -> void:
	match kind:
		"sys:day":
			api.exec("day.advance 1")
			for raw in windows:
				var w: Control = raw
				if is_instance_valid(w):
					var c: Node = w.get_meta("content")
					if c.has_method("refresh"):
						c.refresh()
		"sys:help":
			# IT USED TO OPEN A TERMINAL AND TYPE `help`, which answers a
			# question nobody in Act I is asking. Somebody who has just sat
			# down wants to know what the job IS.
			_help_window()
		"sys:api":
			_launch("Terminal")
			var t := _find_window("Terminal")
			if t != null:
				var tc: Node = t.get_meta("content")
				tc.feed("help\n")
		"sys:stats":
			_launch("Terminal")
			var t2 := _find_window("Terminal")
			if t2 != null:
				var tc2: Node = t2.get_meta("content")
				tc2.feed("ticket.stats\n")
		"sys:quit":
			get_tree().quit()
		_:
			_launch(kind)

# THE ONE PIECE OF PROSE IN THE GAME THAT IS NOT A TICKET.
#
# Everything else on this desktop is generated from a spec or read out of the
# world. This is not: it is the induction talk you would get on your first
# morning, and it exists because "System > Help did nothing" was a fair
# complaint about a menu item that opened a terminal and typed `help`.
#
# It says what the job is and stops. It does not explain the mechanics of the
# acceptance checks, because the checks explain themselves in the queue, and
# it does not hint at Act II, because finding the API is the discovery the
# whole design is built around.
const HELP_TEXT := """RUNBOOK

You are the entire IT department of Harbrook Industries.

Forty people work here. More start every week, and each of them arrives as a
ticket in your queue with nothing set up: no account, no mailbox, no home
folder, no access.

THE QUEUE
  Open it from Applications. Click a ticket and press Check, and the game
  shows you every condition that has to be true before it closes -- and, for
  the ones that are not true yet, why.

  There is no Resolve button. A ticket closes when the world agrees the work
  is done, and not before. Nothing you can say to the game will close one.

THE APPLIANCES
  Places lists the machines this company runs: the directory, the mail
  server, the file server. Each has forms. Filling one in costs two minutes of
  your day, and you get 480 minutes.

THE DAY
  Top right. When it runs out the day ends and tomorrow's arrivals join
  whatever you did not finish. That is the only punishment there is -- there
  is no losing this game, only falling further behind.

  System > Go home ends the day early.

WHAT YOU ARE AIMED AT
  The company doubles, and then doubles again. Doing this by hand stops
  working long before that. How you deal with it is up to you."""

func _help_window() -> void:
	var existing := _find_window("Help")
	if existing != null:
		existing.visible = true
		_raise(existing)
		return
	var c := Control.new()
	c.set_meta("text", HELP_TEXT)
	c.draw.connect(func(): _draw_help(c))
	cascade = (cascade + 1) % 7
	_win("Help", Rect2(Vector2(90 + cascade * 20, TOP_H + 30), Vector2(560, 470)), c, "manual")

func _draw_help(c: Control) -> void:
	c.draw_rect(Rect2(Vector2.ZERO, c.size), Color("#fbfbf7"))
	var y := 20.0
	for raw in str(c.get_meta("text")).split("\n"):
		var line := str(raw)
		var sz := 12
		var col := Color("#1b1b1b")
		if line.length() > 0 and line == line.to_upper() and line.strip_edges() != "":
			sz = 13
			col = Color("#3c6eb4")
		elif line.begins_with("  "):
			col = Color("#4a5560")
		c.draw_string(mono, Vector2(14, y), line, HORIZONTAL_ALIGNMENT_LEFT, c.size.x - 28, sz, col)
		y += 15.0

# The prompt is the machine's own working directory, asked of the machine.
# A prompt that guessed would be lying about where the next command lands.
var _cwd_cache := "/"
var _cwd_age := 99.0

func _shell_prompt() -> String:
	if api != null and _cwd_age > 0.5:
		_cwd_age = 0.0
		var c := str(api.sh("pwd")).strip_edges()
		if c != "" and c.begins_with("/"):
			_cwd_cache = c.get_slice("\n", 0)
	return "%s$ " % _cwd_cache

func _launch(kind: String) -> void:
	_ensure()
	var key := kind
	if kind.begins_with("appl:"):
		key = kind.substr(5)
	elif kind.begins_with("game:"):
		for raw in GAMES:
			var g: Dictionary = raw
			if str(g["kind"]) == kind:
				key = str(g["label"])
	var existing := _find_window(key)
	if existing != null:
		existing.set_meta("ws", workspace)
		existing.set_meta("shown", true)
		existing.visible = true
		_raise(existing)
		if foot: foot.queue_redraw()
		return

	cascade = (cascade + 1) % 7
	var at := Vector2(60 + cascade * 26, TOP_H + 18 + cascade * 22)

	if kind == "Queue":
		var q := Queue.new()
		q.setup(api)
		_win("Queue", Rect2(at, Vector2(760, 470)), q, "notes")
	elif kind == "Terminal":
		var t := Term.new()
		# The terminal knows nothing about this game: it takes a line, hands
		# it somewhere, and prints what comes back. That is the only contract,
		# and it is why the same control served an emulated Unix in NOMINAL
		# and serves an API here.
		# A SHELL ON A REAL MACHINE, not a command box for the game's API.
		#
		# The player's workstation is an emulated RV64IM computer with a disk,
		# an init, a package database and /bin/sh on it, and this is a terminal
		# onto that machine -- `ls`, `cat`, `grep`, pipes, redirection, for
		# loops, and shell scripts in files.
		#
		# The game is reachable from it through /bin/rb, which is a program on
		# that disk like any other. That is decision 13 the right way up: the
		# machine is the thing, and the game is something a program on it can
		# ask about.
		t.on_command = func(line: String) -> String: return api.sh(line)
		t.prompt_fn  = func() -> String: return _shell_prompt()
		t.banner = PackedStringArray([
			"NomnixOS 11.4 — your workstation.",
			"",
			"This is a real machine: ls, cat, grep, pipes, for loops, scripts in files.",
			"`rb` is the company's API from here — try `rb help`, then `rb ticket.list open`.",
			"",
		])
		t._load_commands()
		t.focus_mode = Control.FOCUS_ALL
		_win("Terminal", Rect2(at, Vector2(720, 420)), t, "term")
		# Only if there is a tree to take focus in. Focus is a scene-tree
		# concept and asking for it outside one is an engine error, not a
		# no-op -- which the client gate, building a desktop before the first
		# frame, found immediately.
		if t.is_inside_tree():
			t.grab_focus()
	elif kind == "Files":
		var fb := Files.new()
		fb.setup(api)
		_win("Files", Rect2(at, Vector2(560, 430)), fb, "files")
	elif kind.begins_with("appl:"):
		var id := kind.substr(5)
		var u := WebUI.new()
		u.setup(api, id)
		var info := api.objects(api.exec("appl.info %s" % id))
		var ik := "app"
		if info.size() > 0:
			ik = _icon_for(str((info[0] as Dictionary).get("kind", "")))
		_win(id, Rect2(at, Vector2(640, 400)), u, ik)
	elif kind.begins_with("game:"):
		# The games are lifted wholesale and know nothing about this game.
		# They are Controls that draw themselves and take input, which is all a
		# window needs of anything.
		var script_name := kind.substr(5)
		var label := script_name
		var icon := "game"
		for raw in GAMES:
			var g: Dictionary = raw
			if str(g["kind"]) == kind:
				label = str(g["label"])
				icon = str(g["icon"])
		# A LOAD THAT FAILED IS NOT A GDScript, and calling .new() on it throws
		# a second error that says nothing about the first. The parse error is
		# the diagnosis; this is just noise on top of it, and it took a
		# playtester's log to separate the two.
		var res: Resource = load("res://scripts/%s.gd" % script_name)
		if res == null or not (res is GDScript):
			push_error("runbook: %s.gd did not load; see the parse error above" % script_name)
			return
		var inst: Object = (res as GDScript).new()
		if inst is Control:
			var c: Control = inst
			c.focus_mode = Control.FOCUS_ALL
			_win(label, Rect2(at, Vector2(520, 400)), c, icon)
			if c.is_inside_tree():
				c.grab_focus()
	if foot: foot.queue_redraw()
