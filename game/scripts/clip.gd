# clip.gd — two clipboards, because X11 has two and everybody's hands know it.
#
# PRIMARY is the selection. You drag over some text and it is in PRIMARY
# immediately, with no keystroke; middle-click pastes it. Nothing you do with
# Ctrl-C touches it.
#
# CLIPBOARD is the explicit one. Ctrl-C puts something in it; Ctrl-V takes it
# out. Nothing you select touches it.
#
# The whole point is that they are INDEPENDENT: you can have a login in the
# clipboard and a path in the selection and paste either without losing the
# other. Anyone who has used X11 for a week does this constantly without
# thinking about it, and on a desktop that does not have it they discover they
# were relying on it.
#
# Godot's DisplayServer has a real primary selection on Linux
# (clipboard_get_primary), so on the platform where the convention comes from
# this is the SYSTEM's PRIMARY -- select in the terminal here, middle-click
# into a browser, and it arrives. Everywhere else it falls back to a buffer
# kept in this file, which behaves identically inside the game.
# BOTH BUFFERS ARE KEPT HERE AS WELL AS HANDED TO THE SYSTEM.
#
# The headless display server has no clipboard at all and says so with an
# engine error, which failed the gate for a reason that had nothing to do with
# the game. Keeping our own copy means the two buffers behave identically
# whether or not the platform underneath has them -- and where the platform
# does, they are the SYSTEM's, so selecting in this terminal and
# middle-clicking into a browser works, which is the point of using X11's
# convention rather than inventing one.
extends RefCounted

static var _primary := ""
static var _clipboard := ""

static func _has(feature: int) -> bool:
	return DisplayServer.has_feature(feature)

static func set_primary(s: String) -> void:
	if s == "":
		return
	_primary = s
	if _has(DisplayServer.FEATURE_CLIPBOARD_PRIMARY):
		DisplayServer.clipboard_set_primary(s)

static func get_primary() -> String:
	if _has(DisplayServer.FEATURE_CLIPBOARD_PRIMARY):
		var s := DisplayServer.clipboard_get_primary()
		if s != "":
			return s
	return _primary

static func set_clipboard(s: String) -> void:
	if s == "":
		return
	_clipboard = s
	if _has(DisplayServer.FEATURE_CLIPBOARD):
		DisplayServer.clipboard_set(s)

static func get_clipboard() -> String:
	if _has(DisplayServer.FEATURE_CLIPBOARD):
		var s := DisplayServer.clipboard_get()
		if s != "":
			return s
	return _clipboard
