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
			# A day's work, done, so the end-of-day report has something to say.
			for line in ["esedgeton u_00041 \"Emlin Sedgeton\" sales",
						 "sharrcroft u_00042 \"Sten Harrcroft\" support"]:
				var f: PackedStringArray = line.split(" ")
				pass
			api.exec("rec.start onboard")
			var people := [["esedgeton", "u_00041", "Emlin Sedgeton", "sales"],
						   ["sharrcroft", "u_00042", "Sten Harrcroft", "support"]]
			for raw in people:
				var p: Array = raw
				api.exec("form.submit directory_01 account_new login=%s user_ref=%s display_name=\"%s\" dept=%s" % [p[0], p[1], p[2], p[3]])
				api.exec("form.submit directory_01 member_add login=%s group=dept-%s" % [p[0], p[3]])
				api.exec("form.submit mail_01 mailbox_new login=%s address=%s@harbrook.example quota_mb=2048 status=active" % [p[0], p[0]])
			api.exec("rec.stop")
			api.exec("rec.save /root/scripts/onboard.py")

			desk._launch("Queue")
			desk._launch("Files")
			desk._edit_file("/root/scripts/onboard.py")
			desk._launch("Terminal")

			_place(desk, "Queue", Vector2(8, 30), Vector2(470, 380))
			_place(desk, "Files", Vector2(8, 418), Vector2(470, 354))
			_place(desk, "onboard.py", Vector2(560, 130), Vector2(700, 400))
			_place(desk, "Terminal", Vector2(490, 540), Vector2(780, 232))

			var fw: Node = desk._find_window("Files")
			if fw != null:
				var fc: Node = fw.get_meta("content")
				fc.cwd = "/home/pvane"
				fc.refresh()
			var q: Node = desk._find_window("Queue")
			if q != null:
				var qc: Node = q.get_meta("content")
				qc._go_home()
			var t: Node = desk._find_window("Terminal")
			if t != null:
				var tc: Node = t.get_meta("content")
				tc.feed("cat /home/pvane/notes.txt\n")
	if frames < 14:
		return
	var img := root.get_texture().get_image()
	var path := OS.get_environment("RUNBOOK_SHOT")
	if path == "":
		path = "res://shot.png"
	img.save_png(path)
	print("shot: wrote ", path, " ", img.get_width(), "x", img.get_height())
	quit(0)
