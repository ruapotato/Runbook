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

func _done() -> void:
	print("client_test: %d checks, %d failures" % [checks, failures])
	quit(1 if failures > 0 else 0)
