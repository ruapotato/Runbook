# uifont.gd — the one font this desktop draws with, and it is MONOSPACED.
#
# Every app called its font `mono` and then set it to ThemeDB.fallback_font,
# which is Godot's built-in proportional sans. Nothing on a screen made of
# terminals survives that. A playtester's first screenshot of the game is the
# bootloader menu, and it came out like this:
#
#   +----------------------------------------------+
#   | NomnixOS 11.4                 |
#   | NomnixOS 11.4 (single user)        |
#   | rescue medium                 |
#   +----------------------------------------------+
#
# The same bytes out of the same kernel, in a real terminal, are square. The
# padding spaces are narrower than the letters, so every run of spaces loses
# ground and the right-hand border walks off. That is not a cosmetic problem:
# `ls -l`, `df`, `ps`, `svc` and `pkg verify` are ALL column-formatted, and
# they are the entire diagnostic surface of the game. A terminal that cannot
# hold a column makes every one of them harder to read than the shell that
# produced it.
#
# The OS lays its own output out in character cells -- it pads with spaces and
# counts them -- so the fix belongs in the font, not in a layout pass here:
# give the drawing a font whose cell is the width the OS already assumed.
# DejaVu Sans Mono is under game/fonts/ with its licence beside it.
#
# It is loaded once and shared. A FontFile is a resource with rasterised
# glyph caches in it; thirty apps each loading their own would rasterise the
# same glyphs thirty times.

extends RefCounted

static var _mono: Font = null


static func mono() -> Font:
	if _mono == null:
		var r: Resource = load("res://fonts/DejaVuSansMono.ttf")
		if r is Font:
			_mono = r as Font
		else:
			# Never leave an app without a font -- a missing import should cost
			# you alignment, not the whole screen.
			push_warning("uifont: monospace font missing, falling back to the proportional default")
			_mono = ThemeDB.fallback_font
	return _mono
