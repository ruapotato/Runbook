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
		# Open a few things and leave them where a person would have dragged
		# them -- overlapping, at angles, one half behind another. A tidy grid
		# would be a lie about how this desktop works: the windows are free
		# floating and the screenshot should say so.
		var desk: Node = root.get_child(root.get_child_count() - 1)
		if desk.has_method("_launch"):
			desk._launch("appl:mail_01")
			desk._launch("appl:fileserver_01")
			desk._launch("Terminal")
			desk._launch("appl:directory_01")
			desk._launch("Files")
			desk._launch("game:gsolitaire")
			_place(desk, "mail_01", Vector2(690, 60), Vector2(560, 236))
			_place(desk, "Terminal", Vector2(300, 430), Vector2(700, 330))
			_place(desk, "fileserver_01", Vector2(742, 316), Vector2(520, 250))
			_place(desk, "Solitaire", Vector2(16, 64), Vector2(410, 300))
			_place(desk, "Files", Vector2(300, 96), Vector2(420, 300))
			_place(desk, "directory_01", Vector2(150, 150), Vector2(560, 300))
			var f: Node = desk._find_window("fileserver_01")
			if f != null:
				var fc: Node = f.get_meta("content")
				fc.tab = -1
				fc._build()
				fc._refresh()
			var t: Node = desk._find_window("Terminal")
			if t != null:
				var tc: Node = t.get_meta("content")
				tc.feed("rb ticket.list open 1\n")
			desk._launch("Queue")
			_place(desk, "Queue", Vector2(24, 300), Vector2(620, 460))
			var q: Node = desk._find_window("Queue")
			if q != null:
				var qc: Node = q.get_meta("content")
				if qc.tickets.size() > 0:
					qc.selected = 0
					qc._check()
	if frames < 14:
		return
	var img := root.get_texture().get_image()
	var path := OS.get_environment("RUNBOOK_SHOT")
	if path == "":
		path = "res://shot.png"
	img.save_png(path)
	print("shot: wrote ", path, " ", img.get_width(), "x", img.get_height())
	quit(0)
