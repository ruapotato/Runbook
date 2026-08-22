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
const Bridge := preload("res://scripts/bridge.gd")
# NOMINAL's terminal, not the one I wrote and then had to throw away. See the
# note at the top of that file.
const Term   := preload("res://scripts/terminal.gd")
const Files  := preload("res://scripts/files.gd")
const Editor := preload("res://scripts/editor.gd")

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
# Applications, Places, System.
#
# PLACES IS PLACES, which took a correction. It listed the appliances, on the
# reasoning that in this game the places you go ARE the appliances -- which is
# a nice sentence and wrong about the menu it is describing. In MATE, Places
# is Home Folder, Documents, Computer: locations on the disk, opening a file
# browser. Appliances are programs, and programs go in Applications.
#
# Getting that backwards is exactly the kind of clever that makes a desktop
# feel unfamiliar for no gain, which is the opposite of why this shape was
# chosen (§14, and the fact that everybody has used one of these).
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
		_launch("Bridge")

var _clock_cache: Dictionary = {}
var _clock_age := 99.0
var _last_minute := -1
var _clock_flash := 0.0
var _rec_rect := Rect2()
var _rec_steps := -1          # -1 when not recording

# THE FIGHT RUNS IN REAL TIME, AND IN FIXED STEPS.
#
# The desktop is the only thing that advances the ship: it calls `tick`, which
# is the same command a player can type and a script can send. Nothing in the
# world moves on its own.
#
# THE STEP IS FIXED AT A FIFTIETH OF A SECOND and the remainder is carried, so
# the fight a player sees on a 144Hz monitor is the same fight, tick for tick,
# as the one the balance harness ran headless. Passing the frame delta
# straight through would make the game's arithmetic a function of the frame
# rate -- which is how determinism dies quietly, and how the fastest computer
# in the room ends up playing a slightly different game from everybody else.
const STEP := 0.02
const MAX_CATCHUP := 10          # steps per frame; past this we drop time

var _accum := 0.0

func _tick_world(dt: float) -> void:
	_accum += minf(dt, 0.25)
	var steps := 0
	while _accum >= STEP and steps < MAX_CATCHUP:
		api.exec("tick %.4f" % STEP)
		_accum -= STEP
		steps += 1
	if steps == 0:
		return
	# Only the bridge repaints per tick. A file browser does not change
	# because the ship took a hit, and repainting everything sixty times a
	# second would cost more than the fight does.
	var b := _find_window("Bridge")
	if b != null and is_instance_valid(b):
		var bc: Node = b.get_meta("content")
		if bc.has_method("refresh"):
			bc.refresh()

func _process(dt: float) -> void:
	if api == null or top == null:
		return
	_tick_world(dt)
	_clock_age += dt
	_cwd_age += dt
	if _clock_age > 0.25:
		_clock_age = 0.0
		if _rec_steps >= 0:
			var rs: Array = api.objects(api.exec("rec.status"))
			if rs.size() > 0:
				var r0: Dictionary = rs[0]
				if str(r0.get("recording", "false")) == "true":
					_rec_steps = int(str(r0.get("steps", "0")))
				else:
					_rec_steps = -1
		var info: Array = api.objects(api.exec("status"))
		if info.size() > 0:
			_clock_cache = info[0]
			var m := int(str(_clock_cache.get("clock", "0")))
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
		"Bridge":     return "sysmon"
		"Terminal":   return "term"
		"directory":  return "svc"
		"mail":       return "chat"
		"fileserver": return "files"
		"printer":    return "pkg"
		_:            return "app"

func _desktop_items() -> Array:
	_ensure()
	return [{"label": "Bridge", "kind": "Bridge", "icon": "sysmon"},
			{"label": "Terminal", "kind": "Terminal", "icon": "term"},
			{"label": "Files", "kind": "Files", "icon": "files"},
			{"label": "Editor", "kind": "Editor", "icon": "editor"}]

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
	# fight's clock and the ship's hull, because those are the two numbers a
	# player checks without wanting to look away from what they are doing.
	if not _clock_cache.is_empty():
		var i0: Dictionary = _clock_cache
		var secs := int(str(i0.get("clock", "0")))
		var hull := int(str(i0.get("hull", "0")))
		var hull_max: int = maxi(1, int(str(i0.get("hull_max", "16"))))
		var paused := str(i0.get("paused", "true")) == "true"
		# THE RIGHT-HAND END, MEASURED RATHER THAN GUESSED, and laid out from
		# the edge inwards. The first version put the bar at a fixed offset
		# from the clock and the label at a fixed offset from the bar, and the
		# last letter came out under the bar. Panels are the one place where
		# measuring is not optional: the strings change every second.
		var clock := "%02d:%02d" % [secs / 60, secs % 60]
		if paused:
			clock = "PAUSED"
		var cw := mono.get_string_size(clock, HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x
		var label := "hull %d" % hull
		var lw := mono.get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, 11).x
		var x := top.size.x - 10.0
		top.draw_string(mono, Vector2(x - cw, 17), clock, HORIZONTAL_ALIGNMENT_LEFT, -1, 12,
			Color("#8a5a12") if paused else PANEL_INK)
		x -= cw + 12.0
		top.draw_line(Vector2(x, 4), Vector2(x, TOP_H - 4), PANEL_EDGE, 1.0)
		x -= 10.0

		# The hull, as a bar, in the notification area. It goes red under a
		# third, which is not a warning about failure -- it is the failure,
		# arriving slowly enough to do something about.
		var bw := 74.0
		top.draw_rect(Rect2(x - bw, 7, bw, 11), Color("#c9c5be"))
		var frac: float = clampf(float(hull) / float(hull_max), 0.0, 1.0)
		var barcol: Color = MENU_HOT if frac > 0.34 else Color("#c0453b")
		if _clock_flash > 0.0:
			barcol = barcol.lerp(Color.WHITE, _clock_flash * 0.7)
		top.draw_rect(Rect2(x - bw, 7, bw * frac, 11), barcol)
		top.draw_rect(Rect2(x - bw, 7, bw, 11), PANEL_EDGE, false, 1.0)
		x -= bw + 8.0
		top.draw_string(mono, Vector2(x - lw, 17), label,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, PANEL_INK)
		x -= lw + 14.0

		# THE RECORD BUTTON, in the notification area, on from the first
		# morning. Decision 15 calls the recorder the single most important
		# accessibility feature in the game, and a feature behind a menu is a
		# feature most people never find. It is a red dot. Everybody knows
		# what a red dot does.
		_rec_rect = Rect2(x - 74, 5, 70, TOP_H - 10)
		var recording := _rec_steps >= 0
		top.draw_rect(_rec_rect, Color("#c0453c") if recording else PANEL_HI)
		top.draw_rect(_rec_rect, PANEL_EDGE, false, 1.0)
		top.draw_circle(_rec_rect.position + Vector2(11, _rec_rect.size.y * 0.5), 4.0,
			Color.WHITE if recording else Color("#c0453c"))
		top.draw_string(mono, Vector2(_rec_rect.position.x + 20, 17),
			("%d" % _rec_steps) if recording else "record",
			HORIZONTAL_ALIGNMENT_LEFT, 46, 11,
			Color.WHITE if recording else PANEL_INK)

func _top_input(e: InputEvent) -> void:
	if not (e is InputEventMouseButton and e.pressed and e.button_index == MOUSE_BUTTON_LEFT):
		return
	if _rec_rect.has_point(e.position):
		_toggle_record()
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
			var apps := [{"label": "Bridge", "kind": "Bridge", "icon": "sysmon"},
						 {"label": "Terminal", "kind": "Terminal", "icon": "term"},
						 {"label": "Files", "kind": "Files", "icon": "files"},
						 {"label": "Script editor", "kind": "Editor", "icon": "editor"},
						 {"label": "", "kind": "", "icon": "app"}]
			apps.append_array(GAMES)
			return apps
		1:
			# PLACES, as a desktop means it: somewhere on the disk, opening a
			# file browser. The previous administrator's home directory is in
			# here because it is worth finding, and finding it by accident
			# while looking for something else is how anybody finds anything.
			return [{"label": "Home Folder", "kind": "go:/root", "icon": "files"},
					{"label": "Scripts", "kind": "go:/root/scripts", "icon": "editor"},
					{"label": "Examples", "kind": "go:/root/examples", "icon": "manual"},
					{"label": "Vane's notes", "kind": "go:/home/vane", "icon": "files"},
					{"label": "", "kind": "", "icon": "app"},
					{"label": "Computer", "kind": "go:/", "icon": "sysmon"}]
		_:
			return [{"label": "Stop time", "kind": "sys:pause", "icon": "clock"},
					{"label": "How the fight works", "kind": "sys:help", "icon": "manual"},
					{"label": "The API, in full", "kind": "sys:api", "icon": "term"},
					{"label": "The ship, as one object", "kind": "sys:stats", "icon": "sysmon"},
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
	var h := 6.0
	for raw in items:
		h += _menu_row_h(raw as Dictionary)
	return Rect2((tabs[menu_open] as Rect2).position.x, TOP_H, w, h)

# ONE PLACE THAT KNOWS HOW TALL A MENU ROW IS.
#
# A blank separator is 8 pixels and everything else is 22, and for a while the
# DRAWING knew that and the CLICK MATH did not -- the click math divided by 22
# and assumed a uniform grid. So every item below the separator was drawn 14
# pixels above where clicking it landed, and the Applications menu, which has
# a separator right before the games, launched the wrong game every time.
#
# The bug was not the arithmetic. It was having the arithmetic twice.
func _menu_row_h(it: Dictionary) -> float:
	if str(it.get("kind", "")) == "" and str(it.get("label", "")) == "":
		return 8.0
	return 22.0

# The rectangle of every row, in order. Drawing and hit-testing both read this
# and neither one does its own sums.
func _menu_rows() -> Array:
	var r := _menu_rect()
	var out: Array = []
	var y := r.position.y + 6.0
	for raw in _menu_items(menu_open):
		var it: Dictionary = raw
		var h := _menu_row_h(it)
		out.append(Rect2(r.position.x, y, r.size.x, h))
		y += h
	return out

func _draw_menu() -> void:
	var r := _menu_rect()
	draw_rect(Rect2(r.position + Vector2(2, 2), r.size), Color(0, 0, 0, 0.22))
	draw_rect(r, PANEL_HI)
	draw_rect(r, PANEL_EDGE, false, 1.0)
	var items := _menu_items(menu_open)
	var rows := _menu_rows()
	for i in range(items.size()):
		var it: Dictionary = items[i]
		var row: Rect2 = rows[i]
		var sep := str(it.get("kind", "")) == ""
		if sep and str(it.get("label", "")) == "":
			draw_line(Vector2(r.position.x + 6, row.position.y + 4),
				Vector2(r.position.x + r.size.x - 6, row.position.y + 4), Color("#c4bfb7"), 1.0)
			continue
		var base := row.position.y + 15.0
		if sep:
			draw_line(Vector2(r.position.x + 6, row.position.y + 1),
				Vector2(r.position.x + r.size.x - 6, row.position.y + 1), Color("#c4bfb7"), 1.0)
		else:
			Icons.draw_icon(self, Vector2(r.position.x + 6, row.position.y + 4), 15.0,
				str(it.get("icon", "app")))
		draw_string(mono, Vector2(r.position.x + 26, base), str(it["label"]),
			HORIZONTAL_ALIGNMENT_LEFT, r.size.x - 36, 12,
			Color("#8a857d") if sep else PANEL_INK)
		if it.has("sub"):
			draw_string(mono, Vector2(r.position.x + r.size.x - 8, base), str(it["sub"]),
				HORIZONTAL_ALIGNMENT_RIGHT, 200, 10, Color("#7b756c"))

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
			var items := _menu_items(menu_open)
			var rows := _menu_rows()
			for i in range(items.size()):
				if (rows[i] as Rect2).has_point(e.position):
					_activate(str((items[i] as Dictionary)["kind"]))
					break
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
		"sys:pause":
			api.exec("pause")
			_echo_command("pause")
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
				tc.feed("rb help\n")
		"sys:stats":
			_launch("Terminal")
			var t2 := _find_window("Terminal")
			if t2 != null:
				var tc2: Node = t2.get_meta("content")
				tc2.feed("rb ship\n")
		"sys:quit":
			get_tree().quit()
		_:
			# A Place: open the file browser there. GDScript's match has no
			# guard clause, so the prefix test lives in the default arm.
			if kind.begins_with("go:"):
				_launch("Files")
				var fw := _find_window("Files")
				if fw != null:
					var fc: Node = fw.get_meta("content")
					fc.cwd = kind.substr(3)
					fc.viewing = ""
					fc.scroll = 0
					fc.refresh()
			else:
				_launch(kind)
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
const HELP_TEXT := """THE KESTREL

A raider has caught you. There is one ship, one fight, and no second chance
at it -- but the ship's computer keeps its files between deaths, so anything
you write survives you.

THE BRIDGE
  Eight rooms. Click the little amber squares in a room to give that system
  power; click a lit one to take it back. The reactor has eight bars and
  every system wants them, so the whole game is deciding which.

  Click a crew member, then click a room, to send them there. What they do
  when they arrive is decided by where they are: a room on fire gets put
  out, a damaged system gets repaired, a working one gets manned.

  Right-click a room to seal its door. A shut door stops fire spreading and
  holds air in.

  SPACE stops time. Thinking is free; FTL was right about that.
  F fires the gun, when it is charged.

THE CONSOLE
  Under the ship. Every single thing you click prints there as a command --
  `power shields 3`, `send Vane 2`, `fire`. Those are not descriptions of
  what you did. They are what you did.

  So you can type them instead. Open the Terminal and try `rb power shields
  3`. It is the same command, and your clicks show up in that window too.

THE COMPUTER
  It is a real machine with a real shell and real files. `py` is a Python
  subset; /root/examples has scripts that already work.

  `run /root/examples/gunner.py` starts a script running INSIDE the fight.
  It watches the gun and fires it the moment it is charged, forever, while
  you deal with the fire in the engine room.

WHAT IT COSTS
  Scripts run on the ship's computer, and the computer runs on reactor bars
  -- the same bars the shields want. With no power in the computer, your
  automation does nothing at all.

  That is the game. Automation is a trade, not a cheat."""

# START, or STOP AND SHOW YOU WHAT YOU DID.
#
# Stopping does not put the script in a dialog with an OK button. It opens it
# in an editor, on the machine, with a Run button -- because the point is not
# that the player SEES a script, it is that they change one. The first edit
# somebody makes to a program about their own work is the moment this whole
# design is aiming at.
func _toggle_record() -> void:
	if _rec_steps >= 0:
		api.exec("rec.stop")
		_rec_steps = -1
		var name := "recorded"
		var st := api.objects(api.exec("rec.status"))
		if st.size() > 0:
			name = str((st[0] as Dictionary).get("name", "recorded"))
		var path := "/root/scripts/%s.py" % name
		api.exec("rec.save " + path)
		_edit_file(path)
	else:
		api.exec("rec.start recorded")
		_rec_steps = 0
	if top: top.queue_redraw()

func _edit_file(path: String) -> void:
	var key := path.get_file()
	var existing := _find_window(key)
	if existing != null:
		existing.visible = true
		_raise(existing)
		return
	var ed := Editor.new()
	ed.setup(api, path)
	cascade = (cascade + 1) % 7
	_win(key, Rect2(Vector2(120 + cascade * 20, TOP_H + 26 + cascade * 18), Vector2(660, 480)),
		 ed, "editor")

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

# CLICKS ARRIVE IN THE TERMINAL AS TEXT.
#
# This is the premise, made literal. A player with a shell open watches the
# lines their own clicking sends -- `power shields 3`, `send Vane 2`, `fire`
# -- scroll past in the same window where they could have typed them. Nobody
# has to be told that the interface is a front end for a command language;
# they watch it be one.
#
# It is printed with a marker rather than a prompt, because these lines were
# not typed here and pretending otherwise would put a lie in the scrollback.
func _echo_command(line: String) -> void:
	var t := _find_window("Terminal")
	if t == null or not is_instance_valid(t):
		return
	var tc: Node = t.get_meta("content")
	if tc.has_method("write"):
		tc.write("[bridge] rb %s\n" % line)

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
	if kind.begins_with("game:"):
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

	if kind == "Bridge":
		var b := Bridge.new()
		# WHERE THE COMMANDS GO. The bridge prints every command it sends into
		# its own console strip, and hands it here as well so the desktop can
		# put it somewhere the player is already looking -- the terminal, if
		# one is open. Somebody who has a shell up sees their clicks arrive in
		# it as text, which is the entire pitch of this game in one detail.
		b.setup(api, func(line: String) -> void: _echo_command(line))
		b.focus_mode = Control.FOCUS_ALL
		_win("Bridge", Rect2(at, Vector2(860, 560)), b, "sysmon")
		if is_inside_tree():
			b.grab_focus()
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
			"NomnixOS 11.4 — the Kestrel's computer.",
			"",
			"A real machine: ls, cat, grep, pipes, for loops, scripts in files.",
			"`rb` is the ship from here — `rb help`, `rb rooms`, `rb power shields 3`.",
			"`py` is a Python subset. /root/examples has scripts that already work.",
			"",
			"Anything you click on the bridge shows up here as the line that did it.",
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
	elif kind == "Editor":
		_edit_file("/root/scripts/untitled.py")
	elif kind == "Files":
		var fb := Files.new()
		fb.setup(api)
		fb.on_edit = func(p: String) -> void: _edit_file(p)
		_win("Files", Rect2(at, Vector2(560, 430)), fb, "files")
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
