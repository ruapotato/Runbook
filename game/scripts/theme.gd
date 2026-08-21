# theme.gd — one palette per vendor, and why that is a mechanic.
#
# Handoff §14: "Godot's default Controls all look like Godot. If a dozen
# vendor appliances share one look, the vendor-quality mechanic dies quietly.
# Theme variation per vendor comes from the spec file and is a launch
# requirement, not polish."
#
# So the theme name arrives from the appliance spec -- `theme: veridian_slate`
# -- and this file is where a name becomes a look. The looks are not decorative
# variations on each other. Veridian's web UI is roomy, cool and legible;
# Halcyon's is cramped, beige, and puts its labels in the wrong place, because
# Halcyon is the vendor whose API lies about status codes and a company that
# ships one usually ships the other.
#
# A player who has met both will pay double to avoid the cheap one, and
# FEELING that preference is the game teaching something true about
# procurement. It cannot be felt if everything is Godot grey.
extends RefCounted

const THEMES := {
	"veridian_slate": {
		"bg":     Color("#f4f6f8"),
		"panel":  Color("#e3e8ee"),
		"edge":   Color("#9aa7b4"),
		"ink":    Color("#16202b"),
		"dim":    Color("#5d6f80"),
		"accent": Color("#2f6fb0"),
		"field":  Color("#ffffff"),
		"ok":     Color("#1f7a4d"),
		"bad":    Color("#a12a2a"),
		"pad":    10.0,
		"row":    26.0,
	},
	"halcyon_amber": {
		"bg":     Color("#efe7d4"),
		"panel":  Color("#ded2b4"),
		"edge":   Color("#8d7d54"),
		"ink":    Color("#2b2415"),
		"dim":    Color("#6d6144"),
		"accent": Color("#9a6b1e"),
		"field":  Color("#fdfaf0"),
		"ok":     Color("#5d6b22"),
		"bad":    Color("#8d3a12"),
		"pad":    5.0,      # cramped, on purpose
		"row":    21.0,
	},
	"plain": {
		"bg":     Color("#eeeeee"),
		"panel":  Color("#dddddd"),
		"edge":   Color("#999999"),
		"ink":    Color("#111111"),
		"dim":    Color("#666666"),
		"accent": Color("#555555"),
		"field":  Color("#ffffff"),
		"ok":     Color("#2a7a2a"),
		"bad":    Color("#992222"),
		"pad":    8.0,
		"row":    24.0,
	},
}

static func of(name: String) -> Dictionary:
	return THEMES.get(name, THEMES["plain"])
