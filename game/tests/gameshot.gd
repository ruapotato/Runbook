# gameshot.gd — run one game for a while and take its picture.
#
# The liquid bug is a thing you can only see. Guessing at it from the source
# cost twenty minutes; a screenshot cost one.
extends SceneTree

var frames := 0
var target: Control = null

func _init() -> void:
	var name := OS.get_environment("RUNBOOK_GAME")
	if name == "":
		name = "gliquid"
	var res: Resource = load("res://scripts/%s.gd" % name)
	var inst: Object = (res as GDScript).new()
	target = inst as Control
	root.add_child(target)
	target.size = Vector2(720, 520)
	root.size = Vector2i(720, 520)
	process_frame.connect(_tick)

func _tick() -> void:
	frames += 1
	if frames < int(OS.get_environment("RUNBOOK_FRAMES") if OS.get_environment("RUNBOOK_FRAMES") != "" else "120"):
		return
	var img := root.get_texture().get_image()
	var path := OS.get_environment("RUNBOOK_SHOT")
	img.save_png(path if path != "" else "res://game.png")
	print("gameshot: ", path)
	quit(0)
