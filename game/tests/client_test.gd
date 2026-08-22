# client_test.gd — the client gate.
#
# THE FIRST THING IT ASKS IS WHETHER THE EXTENSION IS ACTUALLY THERE, because
# that is the failure NOMINAL spent months not noticing: built against one
# GDExtension API and run against another, the library does not load AT ALL,
# every window falls back to a placeholder, and the whole suite passes. A gate
# that cannot tell "it works" from "it is not there" is worse than no gate.
#
# After that it plays: opens the bridge, clicks a power pip, picks up a crew
# member and puts them in another room, and asserts the ship changed -- through
# the widgets, by clicking, exactly as a person would.
#
# AND IT ASSERTS THE PREMISE. Every click has to print the command that
# performed it. That is not a nicety about logging; it is the reason this game
# exists, and it is the one thing that would be silently easy to lose.
extends SceneTree

var failures := 0
var checks := 0

func ck(cond: bool, what: String) -> void:
	checks += 1
	if cond:
		print("  PASS  ", what)
	else:
		print("  FAIL  ", what)
		failures += 1

func _init() -> void:
	print("client_test: starting")

	ck(ClassDB.class_exists("RunbookWorld"),
	   "the RunbookWorld extension loaded (if this fails, everything below is a placeholder)")
	if not ClassDB.class_exists("RunbookWorld"):
		_done()
		return

	var api := RunbookApi.new()
	ck(api.ready(), "a ship booted behind the client")

	# The client can only see what the API returns; prove the door works.
	var st := api.objects(api.exec("status"))
	ck(st.size() == 1 and int(str(st[0].get("hull", "0"))) > 0,
	   "the ship's state comes back through the extension")

	var rooms := api.objects(api.exec("rooms"))
	ck(rooms.size() == 9, "nine rooms, one line each")
	var crew := api.objects(api.exec("crew"))
	ck(crew.size() >= 3, "and a crew to put in them")

	# THE ECONOMY, ASSERTED. A script gets scheduling slices in proportion to
	# the computer's power, and with the computer dark it gets none. If this
	# ever passes with zero bars, automation has stopped being a trade and the
	# game has lost its only real decision.
	api.exec("power computer 0")
	api.exec("resume")
	var enemy_before := int(str((api.objects(api.exec("enemy"))[0] as Dictionary).get("hull", "0")))
	api.exec("run /root/examples/gunner.py")
	for i in range(400):
		api.exec("tick 0.25")
	var enemy_dark := int(str((api.objects(api.exec("enemy"))[0] as Dictionary).get("hull", "0")))
	ck(enemy_dark == enemy_before,
	   "a script with no power in the computer does nothing at all")

	_desktop_checks()
	_done()

# THE DESKTOP IS A DESKTOP: free-floating windows, not a layout.
#
# Windows cascade when they open, move where you drag them, resize from the
# corner, raise when clicked, minimise to the tasklist and maximise to fill
# the screen between the panels. None of that is decoration -- somebody with
# the bridge, a terminal and a script editor open arranges them the way THEY
# want, and a client that tiled them into a grid would be making that decision
# for them.
func _desktop_checks() -> void:
	var packed: PackedScene = load("res://scenes/desk.tscn")
	var desk: Node = packed.instantiate()
	root.add_child(desk)
	desk.size = Vector2(1280, 800)
	desk._relayout_desktop()

	desk._launch("Bridge")
	desk._launch("Terminal")
	desk._launch("Files")
	ck(desk.windows.size() == 3, "three windows opened")
	if desk.windows.size() < 3:
		return

	var a: Control = desk.windows[0]
	var b: Control = desk.windows[1]
	ck(a.position != b.position, "windows cascade rather than stacking on one spot")
	# OVERLAP IS THE PROOF. A tiling layout gives windows that never intersect;
	# a floating one gives windows that cascade over each other until somebody
	# drags them apart. The first version of this check asserted that neither
	# window ENCLOSED the other, which a cascade happily does when the second
	# window is smaller -- so it failed on a desktop that was working exactly
	# as intended.
	ck(Rect2(a.position, a.size).intersects(Rect2(b.position, b.size)),
	   "and they float over each other rather than being tiled into a grid")

	# Drag: press on the title bar, move, release.
	var before := a.position
	desk._raise(a)
	var down := InputEventMouseButton.new()
	down.button_index = MOUSE_BUTTON_LEFT
	down.pressed = true
	down.position = Vector2(40, 8)
	desk._bar_input(a, down)
	var move := InputEventMouseMotion.new()
	move.position = before + Vector2(190, 90) + Vector2(40, 8)
	desk._input(move)
	ck(a.position != before, "a window moves when you drag its title bar")
	var up := InputEventMouseButton.new()
	up.button_index = MOUSE_BUTTON_LEFT
	up.pressed = false
	desk._input(up)
	var parked := a.position
	desk._input(move)
	ck(a.position == parked, "and stops when you let go")

	# Resize from the corner grip.
	var sz := a.size
	desk._grip_input(a, down)
	var grow := InputEventMouseMotion.new()
	grow.position = a.position + sz + Vector2(120, 70)
	desk._input(grow)
	ck(a.size != sz, "a window resizes from its corner")
	desk._input(up)

	# Raise: whichever was clicked last is the focused one.
	desk._raise(b)
	ck(desk.focused == b, "clicking a window raises it")

	# Maximise fills the screen between the panels, and unmaximise restores.
	var restore := b.size
	desk._toggle_max(b)
	ck(b.position.y == desk.TOP_H and b.size.x == desk.size.x,
	   "maximise fills the space between the panels")
	desk._toggle_max(b)
	ck(b.size == restore, "and unmaximise puts it back where it was")

	# Minimise leaves it in the tasklist, which is where you get it back.
	b.visible = false
	b.set_meta("shown", false)
	ck(desk._tasklist().size() == 3, "a minimised window is still in the tasklist")

	# The workspace pager actually moves windows out of sight.
	desk._go_workspace(1)
	ck(not a.visible, "switching workspace hides the windows on the old one")
	desk._go_workspace(0)
	ck(a.visible, "and switching back brings them out again")

	# And the games are really there.
	desk._launch("game:gsolitaire")
	ck(desk._find_window("Solitaire") != null, "the desktop has solitaire on it, like every desktop")

	_menu_checks(desk)
	_bridge_checks(desk)
	_sensors_checks(desk)
	_map_checks(desk)
	_terminal_checks(desk)
	_recorder_checks()

# THE SENSORS WINDOW IS WHERE THE GUN'S DECISION LIVES.
#
# Clicking their weapon room has to send `fire weapons` -- not `fire`, not a
# room index the player never saw. The whole point of giving the enemy an
# interior is that "what do I shoot" becomes a sentence.
func _sensors_checks(desk: Node) -> void:
	desk._launch("Sensors")
	var win: Node = desk._find_window("Sensors")
	if win == null:
		ck(false, "the sensors window opened")
		return
	var sv: Node = win.get_meta("content")
	sv.size = Vector2(660, 430)
	sv.refresh()
	ck(sv.rooms.size() == 4, "sensors painted the raider's four rooms")
	if sv.rooms.size() < 4:
		return

	var wp := -1
	for i in range(sv.rooms.size()):
		if str((sv.rooms[i] as Dictionary).get("system", "")) == "weapons":
			wp = i
	ck(wp >= 0, "their weapon room is one of them")
	if wp < 0:
		return

	var api: RunbookApi = sv.api
	api.exec("pause")
	api.exec("power weapons 3")
	# Charge the gun by playing, not by reaching past the protocol.
	api.exec("resume")
	for i in range(60):
		api.exec("tick 0.5")
	api.exec("pause")

	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	var rr: Rect2 = sv._room_rect(wp)
	click.position = rr.get_center()
	sv._gui_input(click)

	var said := ""
	for l in sv.console:
		said += str(l) + "\n"
	ck(said.find("fire weapons") >= 0,
	   "clicking their weapon room sends `fire weapons`, which is a line you could type")

# THE MAP SHOWS WHAT IS IN THE AIR.
#
# A shot takes about a second to cross, and that second is the only reason
# this window exists. If `shots` ever comes back empty while a volley is in
# flight, the window is a diagram of nothing.
func _map_checks(desk: Node) -> void:
	desk._launch("Map")
	var win: Node = desk._find_window("Map")
	if win == null:
		ck(false, "the map opened")
		return
	var tv: Node = win.get_meta("content")
	var api: RunbookApi = tv.api

	api.exec("resume")
	var seen := 0
	for i in range(400):
		api.exec("tick 0.1")
		tv.refresh()
		if tv.shots.size() > 0:
			seen = tv.shots.size()
			break
	ck(seen > 0, "the map caught %d shot(s) actually in flight" % seen)

# THE MENU OPENS THE THING YOU CLICKED ON.
#
# It did not. Drawing gave a blank separator 8 pixels and everything else 22;
# hit-testing divided by 22 and assumed a uniform grid. So every item below
# the separator was drawn 14 pixels above where clicking it landed, and the
# Applications menu -- which has a separator right before the games -- launched
# the wrong game every single time.
#
# Nothing in the old gate could see it, because every check drove _launch()
# directly and none of them went through the menu. So: click the rows, at the
# coordinates the menu says they are at, and assert the right window opened.
func _menu_checks(desk: Node) -> void:
	desk.menu_open = 0
	var items: Array = desk._menu_items(0)
	var rows: Array = desk._menu_rows()
	ck(items.size() == rows.size(), "every menu item has a row rectangle")

	# Find a game BELOW the separator -- the ones that were broken.
	var target := -1
	for i in range(items.size()):
		if str((items[i] as Dictionary).get("kind", "")).begins_with("game:"):
			target = i
	ck(target > 0, "the Applications menu has games under a separator")
	if target < 0:
		return

	var want: Dictionary = items[target]
	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	# NEAR THE TOP OF THE ROW, NOT ITS CENTRE. The old arithmetic was out by
	# 14 pixels against a 22-pixel row, which means the exact centre of every
	# row still rounded back to the right index -- and a gate that clicks
	# centres would have passed on the broken build. A person clicks wherever
	# the label is.
	var trow: Rect2 = rows[target]
	click.position = Vector2(trow.position.x + 30, trow.position.y + 3)
	desk._input(click)
	ck(desk._find_window(str(want["label"])) != null,
	   "clicking the last game in the menu opens %s, not something else" % want["label"])

	# And the same for an item ABOVE the separator, which always worked and
	# must keep working.
	desk.menu_open = 0
	items = desk._menu_items(0)
	rows = desk._menu_rows()
	var frow: Rect2 = rows[0]
	click.position = Vector2(frow.position.x + 30, frow.position.y + 3)
	desk._input(click)
	ck(desk._find_window(str((items[0] as Dictionary)["label"])) != null,
	   "and the first item still opens the first item")

# THE PATH A PERSON ACTUALLY TAKES.
#
# Everything above drove the API. A player drives WIDGETS: they click a power
# pip and a crew member. Those are different code paths, and the one a human
# uses is the one nothing had tested -- a pip whose hit rectangle was two
# pixels off would pass every API check in this file and waste a playtest.
func _bridge_checks(desk: Node) -> void:
	var win: Node = desk._find_window("Bridge")
	if win == null:
		ck(false, "the bridge opened")
		return
	var br: Node = win.get_meta("content")
	br.size = Vector2(860, 560)
	br.refresh()
	ck(br.rooms.size() == 9, "the bridge painted nine rooms from the API")

	# --- click the third power pip in the shields room ---
	var shields := -1
	for i in range(br.rooms.size()):
		if str((br.rooms[i] as Dictionary).get("system", "")) == "shields":
			shields = i
	ck(shields >= 0, "the shields room is on the deck plan")
	if shields < 0:
		return

	var api: RunbookApi = br.api
	api.exec("pause")
	api.exec("power shields 1")
	br.refresh()
	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	var pip: Rect2 = br._pip_rect(shields, 2)
	click.position = pip.get_center()
	br._gui_input(click)

	var after := api.objects(api.exec("rooms"))
	ck(int(str((after[shields] as Dictionary).get("bars", "0"))) == 3,
	   "clicking the third power pip gives the shields three bars")

	# THE PREMISE, ASSERTED. The click has to have printed the command.
	var said := ""
	for l in br.console:
		said += str(l) + "\n"
	ck(said.find("power shields 3") >= 0,
	   "and the console shows `power shields 3` -- the command the click sent")

	# --- pick somebody up and put them somewhere ---
	var who := str((br.crew[0] as Dictionary).get("name", ""))
	var from := int(str((br.crew[0] as Dictionary).get("room", "0")))
	# WHERE THEY ARE DRAWN, asked of the bridge rather than recomputed here.
	# A test that works out the dot's position independently is a test that
	# passes while the picture and the click disagree.
	var at: Vector2 = br._crew_pos(who)
	click.position = at
	br._gui_input(click)
	ck(br.picked == who, "clicking a crew member picks them up")

	var dest := 0 if from != 0 else 1
	var dr: Rect2 = br._room_rect(dest)
	click.position = dr.get_center()
	br._gui_input(click)
	ck(br.picked == "", "and clicking a room puts them down")
	said = ""
	for l in br.console:
		said += str(l) + "\n"
	ck(said.find("send %s %d" % [who, dest]) >= 0,
	   "with `send %s %d` in the console, which is a line a script can use" % [who, dest])

	# AND IT ARRIVED IN THE TERMINAL. The desk hands every bridge command to
	# any open shell, which is the detail that teaches the whole premise.
	var tw: Node = desk._find_window("Terminal")
	if tw != null:
		var t: Node = tw.get_meta("content")
		var seen := ""
		var n: int = t.lines.size()
		for i in range(maxi(0, n - 30), n):
			seen += str(t.lines[i]) + " "
		ck(seen.find("power shields 3") >= 0,
		   "and the same line showed up in the open terminal, as text")

# THE MACRO RECORDER (decision 15): the single most important accessibility
# feature in the game, and the one whose failure mode is silent. A recorder
# that emits a script which does not RUN teaches a player that they cannot
# program, which is the precise opposite of the point -- so the gate records a
# fight through the API, reads what came out, and runs it.
func _recorder_checks() -> void:
	var api := RunbookApi.new()
	api.exec("rec.start gatetest")

	# Two rounds of the same two steps, which is what makes a loop.
	for pair in [["3", "Vane"], ["2", "Ash"]]:
		var p: Array = pair
		api.exec("power shields %s" % p[0])
		api.exec("send %s 4" % p[1])
	api.exec("rec.stop")

	var st := api.objects(api.exec("rec.status"))
	ck(st.size() == 1 and int(str((st[0] as Dictionary).get("steps", "0"))) == 4,
	   "the recorder saw the four things that were done")
	ck(st.size() == 1 and int(str((st[0] as Dictionary).get("loop_body", "0"))) == 2,
	   "and noticed they were the same two steps, twice")

	var script := api.exec("rec.script")
	ck(script.find("for row in work:") >= 0, "so it wrote a loop, not four lines")
	ck(script.find("bars = row[0]") >= 0, "with the things that changed pulled into variables")
	ck(script.find("do(\"power shields \" + bars)") >= 0,
	   "and the command written as something a player can edit")

	# THE ONE THAT MATTERS: does it run? A recorder whose output does not
	# execute is a demo.
	api.exec("rec.save /root/scripts/gatetest.py")
	var ran := api.sh("py /root/scripts/gatetest.py")
	ck(ran.find("error") < 0 and ran.find("syntax") < 0,
	   "the recorded script runs on the machine without complaint: %s" % ran.get_slice("\n", 0))

	var rooms := api.objects(api.exec("rooms"))
	ck(int(str((rooms[1] as Dictionary).get("bars", "0"))) == 2,
	   "and it really did the work -- the shields are where the script left them")

# THE TERMINAL IS A TERMINAL, and the two clipboards are X11's two.
#
# Both of these came out of the first playtest -- "the terminal is not the one
# from NOMINAL, it has an input box at the bottom rather than being inline",
# and "we need copy paste, and highlight buffer with middle mouse paste like
# X11, so you get 2 copy buffers just like in X11". Neither is the sort of
# thing that fails loudly when it breaks; it just quietly stops feeling right.
# So they are asserted.
func _terminal_checks(desk: Node) -> void:
	desk._launch("Terminal")
	var win: Node = desk._find_window("Terminal")
	if win == null:
		ck(false, "the terminal opened")
		return
	var t: Node = win.get_meta("content")

	ck(t.get("cur") != null and t.get("caret") != null,
	   "the line being typed lives in the transcript, not in a text box below it")

	var before: int = t.lines.size()
	t.feed("uname -a\n")
	ck(t.lines.size() > before, "typing a command prints an answer into the screen")

	# `ls` WORKS, and that is the whole of the machine in one assertion.
	#
	# This check used to require that `ls` explain itself politely, because
	# the terminal was an API console and a playtester typed `ls` into it
	# within seconds of sitting down. The answer to that complaint was not a
	# better error message. It was a machine.
	t.feed("ls /\n")
	var said := ""
	var n: int = t.lines.size()
	for i in range(maxi(0, n - 20), n):
		said += str(t.lines[i]) + " "
	ck(said.find("bin") >= 0 and said.find("etc") >= 0,
	   "and `ls /` lists a real filesystem, because this is a real shell")

	# And the ship is one program on that machine away.
	t.feed("rb rooms\n")
	said = ""
	n = t.lines.size()
	for i in range(maxi(0, n - 12), n):
		said += str(t.lines[i]) + " "
	ck(said.find("shields") >= 0, "and `rb` reaches the ship from the machine")

	# PRIMARY: select, and it is pastable with the middle button. Nothing
	# touched the clipboard.
	var Clip: GDScript = load("res://scripts/clip.gd")
	Clip.set_clipboard("CLIPBOARD-VALUE")
	t.sel_from = Vector2i(0, 0)
	t.sel_to = Vector2i(0, 6)
	var picked := str(t._selected_text())
	ck(picked.length() == 6, "dragging over the transcript selects text")
	Clip.set_primary(picked)
	ck(str(Clip.get_primary()) == picked, "and selecting puts it in PRIMARY")
	ck(str(Clip.get_clipboard()) == "CLIPBOARD-VALUE",
	   "without disturbing the clipboard -- two buffers, which is the whole point")
	var line_before := str(t.cur)
	t.insert_text(str(Clip.get_primary()))
	ck(str(t.cur) == line_before + picked, "middle-click pastes the selection into the line")

func _done() -> void:
	print("client_test: %d checks, %d failures" % [checks, failures])
	quit(1 if failures > 0 else 0)
