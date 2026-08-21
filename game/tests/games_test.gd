# games_test.gd — every game in the menu must actually open.
#
# It did not, and nothing here noticed: the games were lifted from a project
# where inference-from-Variant is a warning into one where 4.7 makes it an
# error, so all ten failed to PARSE. The desktop then called .new() on a null
# script, and the second error buried the first. A playtester found it in
# about four seconds.
#
# Nothing in this file knows what any game does. It asserts the one thing the
# menu promises: click it, and a window opens.
extends SceneTree

func _init() -> void:
	var packed: PackedScene = load("res://scenes/desk.tscn")
	var desk: Node = packed.instantiate()
	root.add_child(desk)
	desk.size = Vector2(1280, 800)
	desk._relayout_desktop()

	var bad := 0
	for raw in desk.GAMES:
		var g: Dictionary = raw
		desk._launch(str(g["kind"]))
		if desk._find_window(str(g["label"])) == null:
			print("  FAIL  ", g["label"], " did not open")
			bad += 1
		else:
			print("  PASS  ", g["label"])
	print("games_test: %d games, %d failures" % [desk.GAMES.size(), bad])
	quit(1 if bad > 0 else 0)
