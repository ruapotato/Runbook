# shot.gd — render the desktop and save a picture of it.
#
# Not a gate: a way to LOOK at the thing without borrowing somebody's screen.
# Every screenshot in a commit message or a handoff should come from here, so
# what is shown is what the code actually draws.
extends SceneTree

var frames := 0

func _init() -> void:
	var packed: PackedScene = load("res://scenes/desk.tscn")
	var scene: Node = packed.instantiate()
	root.add_child(scene)
	root.size = Vector2i(1280, 800)
	process_frame.connect(_tick)

func _place(desk: Node, key: String, at: Vector2, sz: Vector2) -> void:
	var w: Node = desk._find_window(key)
	if w == null:
		return
	w.position = at
	w.size = sz
	desk._relayout_win(w)

func _tick() -> void:
	frames += 1
	if frames == 3:
		var desk: Node = root.get_child(root.get_child_count() - 1)
		if desk.has_method("_launch"):
			var api: RunbookApi = desk.api

			# A FIGHT ALREADY IN TROUBLE, because a screenshot of a ship at
			# full hull with nothing on fire shows none of what this is.
			api.exec("power shields 2")
			api.exec("power weapons 2")
			api.exec("power computer 2")
			api.exec("resume")
			for i in range(300):
				api.exec("tick 0.25")
			api.exec("pause")

			desk._launch("Bridge")
			desk._launch("Terminal")
			desk._launch("Files")

			_place(desk, "Bridge", Vector2(8, 30), Vector2(820, 520))
			_place(desk, "Files", Vector2(840, 30), Vector2(432, 300))
			_place(desk, "Terminal", Vector2(300, 470), Vector2(880, 300))

			var fw: Node = desk._find_window("Files")
			if fw != null:
				var fc: Node = fw.get_meta("content")
				fc.cwd = "/root/examples"
				fc.refresh()
			var bw: Node = desk._find_window("Bridge")
			if bw != null:
				var bc: Node = bw.get_meta("content")
				bc.refresh()
			var t: Node = desk._find_window("Terminal")
			if t != null:
				var tc: Node = t.get_meta("content")
				tc.feed("rb crew\n")
				# The premise, in the screenshot: a click, arriving as text.
				desk._echo_command("power shields 3")
				desk._echo_command("send Vane 2")
	if frames < 14:
		return
	var img := root.get_texture().get_image()
	var path := OS.get_environment("RUNBOOK_SHOT")
	if path == "":
		path = "res://shot.png"
	img.save_png(path)
	print("shot: wrote ", path, " ", img.get_width(), "x", img.get_height())
	quit(0)
