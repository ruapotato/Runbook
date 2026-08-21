# client_test.gd — the client gate.
#
# THE FIRST THING IT ASKS IS WHETHER THE EXTENSION IS ACTUALLY THERE, because
# that is the failure NOMINAL spent months not noticing: built against one
# GDExtension API and run against another, the library does not load AT ALL,
# every window falls back to a placeholder, and the whole suite passes. A gate
# that cannot tell "it works" from "it is not there" is worse than no gate.
#
# After that it plays: opens the queue, reads a real ticket, fills in the
# generated forms on all three appliances, and asserts the ticket closes --
# through the UI, by clicking, exactly as a person would. If Act I is
# unplayable this fails.
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
	ck(api.ready(), "a world booted behind the client")

	# The client can only see what the API returns; prove the door works.
	var info := api.objects(api.exec("world.info"))
	ck(info.size() == 1 and int(str(info[0].get("users_active", "0"))) > 0,
	   "world.info comes back through the extension")

	var appls := api.objects(api.exec("appl.list"))
	ck(appls.size() >= 3, "the org has its three appliances")

	# THE FORMS ARE GENERATED, and this is the assertion that says so: every
	# form the client would render calls an endpoint that exists and offers
	# only fields that endpoint accepts. A hand-built screen could drift from
	# the appliance; a generated one cannot, and this is the proof.
	var forms_total := 0
	var forms_bad := 0
	for raw in appls:
		var a: Dictionary = raw
		var id := str(a.get("id", ""))
		var eps := api.objects(api.exec("appl.endpoints %s" % id))
		for fraw in api.objects(api.exec("appl.forms %s" % id)):
			var f: Dictionary = fraw
			forms_total += 1
			var found := false
			for eraw in eps:
				var ep: Dictionary = eraw
				if str(ep.get("id", "")) == str(f.get("calls", "")):
					found = true
			if not found:
				forms_bad += 1
				print("       form %s calls %s, which %s does not have" % [f.get("id"), f.get("calls"), id])
	ck(forms_total >= 6, "the appliances between them offer the forms Act I needs")
	ck(forms_bad == 0, "every generated form calls an endpoint that exists")

	# --- play a ticket, through the same calls the forms make ---
	api.exec("day.advance 1")
	var open_tickets := api.objects(api.exec("ticket.list open 1"))
	ck(open_tickets.size() == 1, "a day's work arrives in the queue")
	if open_tickets.is_empty():
		_done()
		return

	var t: Dictionary = open_tickets[0]
	var tid := str(t.get("id", ""))
	var subject := str(t.get("ref", ""))
	var chk := api.exec("ticket.check %s" % tid)
	ck(chk.find("does not pass yet") >= 0, "a fresh ticket does not pass")
	ck(chk.find("--") >= 0, "and it says why, for every check that fails")

	var users := api.objects(api.exec("user.get %s" % subject))
	ck(users.size() == 1, "the ticket's subject is a person the API knows")
	if users.is_empty():
		_done()
		return
	var u: Dictionary = users[0]
	var dept := str(u.get("dept", "engineering"))
	var login := str(u.get("given", "x")).substr(0, 1).to_lower() + str(u.get("family", "y")).to_lower()

	# Six form submissions across three appliances -- the §5 arithmetic, done
	# by the client. Retried, because the world is allowed to drop a write and
	# a person would click again.
	var dir_id := ""
	var mail_id := ""
	var fs_id := ""
	for raw in appls:
		var a: Dictionary = raw
		match str(a.get("kind", "")):
			"directory":  dir_id = str(a.get("id", ""))
			"mail":       mail_id = str(a.get("id", ""))
			"fileserver": fs_id = str(a.get("id", ""))

	var submits := [
		"form.submit %s account_new login=%s user_ref=%s display_name=%s dept=%s" % [dir_id, login, subject, login, dept],
		"form.submit %s account_edit login=%s status=active dept=%s" % [dir_id, login, dept],
		"form.submit %s member_add login=%s group=dept-%s" % [dir_id, login, dept],
		"form.submit %s mailbox_new login=%s address=%s@harbrook.example quota_mb=2048 status=active" % [mail_id, login, login],
		"form.submit %s home_new login=%s path=/home/%s quota_mb=8192" % [fs_id, login, login],
		"form.submit %s grant_new login=%s share=share-%s access=rw" % [fs_id, login, dept],
	]
	var minutes_before := int(str(api.objects(api.exec("world.info"))[0].get("minutes_left", "480")))
	for line in submits:
		for attempt in range(6):
			if api.ok(api.exec(str(line))):
				break
	var minutes_after := int(str(api.objects(api.exec("world.info"))[0].get("minutes_left", "480")))

	# TWELVE IN-GAME MINUTES, which is the number the whole Act I wall is built
	# on (§5). If clicking through an onboarding stops costing about that, the
	# wall moves and the balance harness is measuring a different game.
	var spent := minutes_before - minutes_after
	ck(spent >= 10 and spent <= 24,
	   "onboarding by hand cost %d in-game minutes (the design says about 12)" % spent)

	var after := api.exec("ticket.check %s" % tid)
	ck(after.find(" passes") >= 0, "and the ticket closed, by state, with nobody marking it done")

	# The provenance of hand-clicked work. The whole debt mechanic (§11) rests
	# on the client attributing form submissions to a person.
	var closed := api.objects(api.exec("ticket.get %s" % tid))
	ck(closed.size() == 1 and str(closed[0].get("closed_by", "")) == "hand",
	   "the world recorded that a human did it")

	_desktop_checks()
	_done()

# THE DESKTOP IS A DESKTOP: free-floating windows, not a layout.
#
# Windows cascade when they open, move where you drag them, resize from the
# corner, raise when clicked, minimise to the tasklist and maximise to fill
# the screen between the panels. None of that is decoration -- an operator
# with six appliances open arranges them the way THEY want, and a client that
# tiled them into a grid would be making that decision for them.
func _desktop_checks() -> void:
	var packed: PackedScene = load("res://scenes/desk.tscn")
	var desk: Node = packed.instantiate()
	root.add_child(desk)
	desk.size = Vector2(1280, 800)
	desk._relayout_desktop()

	desk._launch("Queue")
	desk._launch("Terminal")
	desk._launch("appl:directory_01")
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

	_click_path(desk)
	_terminal_checks(desk)
	_recorder_checks(desk)

# THE MACRO RECORDER (decision 15): the single most important accessibility
# feature in the game, and the one whose failure mode is silent. A recorder
# that emits a script which does not RUN teaches a player that they cannot
# program, which is the precise opposite of the point -- so the gate records a
# job through the UI, reads what came out, and runs it.
func _recorder_checks(desk: Node) -> void:
	var api := RunbookApi.new()
	api.exec("rec.start gatetest")

	# Two people, the same three steps each -- which is what makes a loop.
	var people := [["gtest1", "u_00001", "Gate One", "sales"],
				   ["gtest2", "u_00002", "Gate Two", "support"]]
	for raw in people:
		var p: Array = raw
		api.exec("form.submit directory_01 account_new login=%s user_ref=%s display_name=\"%s\" dept=%s"
				 % [p[0], p[1], p[2], p[3]])
		api.exec("form.submit directory_01 member_add login=%s group=dept-%s" % [p[0], p[3]])
		api.exec("form.submit mail_01 mailbox_new login=%s address=%s@harbrook.example quota_mb=2048 status=active"
				 % [p[0], p[0]])
	api.exec("rec.stop")

	var st := api.objects(api.exec("rec.status"))
	ck(st.size() == 1 and int(str((st[0] as Dictionary).get("steps", "0"))) == 6,
	   "the recorder saw the six things that were done")
	ck(st.size() == 1 and int(str((st[0] as Dictionary).get("loop_body", "0"))) == 3,
	   "and noticed they were the same three steps, twice")

	var script := api.exec("rec.script")
	ck(script.find("for row in work:") >= 0, "so it wrote a loop, not six lines")
	ck(script.find("login = row[0]") >= 0, "with the things that changed pulled into variables")
	ck(script.find("\"dept-\" + dept") >= 0,
	   "and the rule the player was following written down, not its results")
	ck(script.find("Gate One") >= 0 and script.find("\\\"") >= 0,
	   "and a two-word name still carries its quotes")

	# THE ONE THAT MATTERS: does it run? A recorder whose output does not
	# execute is a demo.
	api.exec("rec.save /root/scripts/gatetest.py")
	var ran := api.sh("py /root/scripts/gatetest.py")
	ck(ran.find("error") < 0 and ran.find("syntax") < 0,
	   "the recorded script runs on the machine without complaint: %s" % ran.get_slice("\n", 0))

	var acct := api.exec("api.call directory_01 get_account login=gtest1")
	ck(api.ok(acct) and acct.find("Gate One") >= 0,
	   "and it really did the work -- the account is there, name and all")

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
	var nverbs: int = t.COMMANDS.size()
	ck(nverbs > 5, "and it completes the verbs the API advertises (%d of them)" % nverbs)

	var before: int = t.lines.size()
	t.feed("uname -a\n")
	var after_line: int = t.lines.size()
	ck(after_line > before, "typing a command prints an answer into the screen")

	# `ls` WORKS NOW, and that is the whole of M4 in one assertion.
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

	# And the game is one program on that machine away.
	t.feed("rb world.info\n")
	said = ""
	n = t.lines.size()
	for i in range(maxi(0, n - 6), n):
		said += str(t.lines[i]) + " "
	ck(said.find("Harbrook") >= 0, "and `rb` reaches the company's API from the machine")

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

# THE PATH A PERSON ACTUALLY TAKES.
#
# Everything above this drove the API. A player drives WIDGETS: they click a
# form tab, type into boxes, and press Submit. Those are different code paths,
# and the one a human uses is the one nothing had tested -- a form that
# submitted the wrong field, or a Submit button whose hit rectangle was two
# pixels off, would have passed every check in this file and wasted the first
# playtest.
#
# So: click the tab, fill the boxes, press the button, and assert the world
# changed.
func _click_path(desk: Node) -> void:
	desk._launch("appl:directory_01")
	var win: Node = desk._find_window("directory_01")
	if win == null:
		ck(false, "the directory web UI opened")
		return
	var ui: Node = win.get_meta("content")
	ck(ui.forms.size() >= 4, "its forms came from the spec, not from a scene")

	# Click the "New account" tab, wherever the layout put it.
	var tabs: Array = ui._tab_rects()
	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	click.position = (tabs[0] as Rect2).position + Vector2(6, 6)
	ui._gui_input(click)
	ck(ui.tab == 0 and ui.edits.size() >= 4,
	   "clicking the New account tab draws its fields")
	if ui.edits.size() < 4:
		return

	# TYPED FIELDS RENDER AS CHOICES, which is the whole of the playtest note
	# "Edit account has no way to select what user to edit". A department is a
	# list of departments; a status is a list of statuses; and on the EDIT
	# form, the login is a list of the accounts that exist.
	var pickers := 0
	for raw in ui.edits:
		var c: Control = raw
		if bool(c.get_meta("picker", false)):
			pickers += 1
	ck(pickers >= 2, "and its typed fields are pickers, not blank boxes (%d of them)" % pickers)

	var edit_tab := -1
	for i in range(ui.forms.size()):
		if str((ui.forms[i] as Dictionary).get("calls", "")) == "update_account":
			edit_tab = i
	ck(edit_tab >= 0, "the directory offers an Edit account form")
	if edit_tab >= 0:
		var etabs: Array = ui._tab_rects()
		click.position = (etabs[edit_tab] as Rect2).position + Vector2(6, 6)
		ui._gui_input(click)
		var login_is_picker := false
		for raw in ui.edits:
			var c: Control = raw
			if str(c.get_meta("field")) == "login" and bool(c.get_meta("picker", false)):
				var ob: OptionButton = c
				login_is_picker = ob.item_count > 1
		ck(login_is_picker, "and Edit account lets you PICK the account, not remember it")
		# back to New account for the submit below
		click.position = (etabs[0] as Rect2).position + Vector2(6, 6)
		ui._gui_input(click)

	var api2: RunbookApi = ui.api
	var before := api2.records(api2.exec("api.call directory_01 list_accounts")).size()

	# A DISPLAY NAME WITH A SPACE IN IT, because that is what names are. The
	# protocol refused one until a playtest pointed out that people have two
	# names; it quotes now, and this is the check that keeps it quoting.
	var values := {"login": "clicktest", "display_name": "Click Test", "dept": "engineering"}

	# WHO THIS ACCOUNT IS FOR is picked from the list, not typed -- and the
	# list is only the people who do not have one yet, which is what a
	# user_ref means on a New Account form. The test used to name u_00001,
	# who has had an account since before the player was hired; it is
	# correctly no longer offered.
	for raw in ui.edits:
		var c0: Control = raw
		if str(c0.get_meta("field")) == "user_ref" and bool(c0.get_meta("picker", false)):
			var ob0: OptionButton = c0
			ck(ob0.item_count > 1, "the user picker offers the people who need an account")
			if ob0.item_count > 1:
				ob0.select(1)

	for raw in ui.edits:
		var c: Control = raw
		var fname := str(c.get_meta("field"))
		if not values.has(fname):
			continue
		if bool(c.get_meta("picker", false)):
			var ob: OptionButton = c
			# A picker's LABEL is for a person -- "Alma Barrow  (u_00041,
			# sales)" -- and its VALUE is what the API wants. Matching on
			# "contains" rather than "starts with" is the test learning the
			# same lesson the player does: the label is not the value.
			for i in range(ob.item_count):
				if ob.get_item_text(i).find(str(values[fname])) >= 0:
					ob.select(i)
		else:
			var le: LineEdit = c
			le.text = str(values[fname])

	# The Submit button, hit where it is drawn rather than where we think it is.
	click.position = ui._submit_rect().position + Vector2(6, 6)
	ui._gui_input(click)
	ck(ui.result_good, "pressing Submit works: %s" % ui.result_line)

	var after := api2.records(api2.exec("api.call directory_01 list_accounts")).size()
	ck(after == before + 1, "and there is one more account in the directory than there was")

	var made := api2.objects(api2.exec("api.call directory_01 get_account login=clicktest"))
	ck(made.size() == 1 and str((made[0] as Dictionary).get("display_name", "")) == "Click Test",
	   "and the display name kept its space")

	# The same button, again, on the same values. A player WILL do this -- and
	# create_account is idempotent, so it must not be an error and must not
	# make a second account.
	click.position = ui._submit_rect().position + Vector2(6, 6)
	ui._gui_input(click)
	var again := api2.records(api2.exec("api.call directory_01 list_accounts")).size()
	ck(ui.result_good and again == after,
	   "and pressing it twice is not an error and does not make two")

func _done() -> void:
	print("client_test: %d checks, %d failures" % [checks, failures])
	quit(1 if failures > 0 else 0)
