# api.gd — the only door.
#
# Every screen in this client is painted from something this returned. There
# is no second channel into the world: the desktop calls exec() with the same
# lines a player types at a socket, gets the same text back, and has to parse
# it like anybody else (handoff decision 7).
#
# That is not a purity exercise. It is what makes the API DISCOVERABLE rather
# than announced -- the player finds, somewhere in Act I, that the buttons
# they have been clicking were sending `api.call` all along, and it is true,
# because it is the only thing the buttons can do.
extends RefCounted
# A real class name, so every caller gets the declared return types instead of
# Variant. Without it Godot cannot infer `var x := api.exec(...)` and the whole
# client fails to parse -- which is a good error to get at load time and a
# terrible one to get from a typo, so: named.
class_name RunbookApi

var world: Object = null
var last_error := ""

func _init() -> void:
	if ClassDB.class_exists("RunbookWorld"):
		world = ClassDB.instantiate("RunbookWorld")
	if world == null:
		last_error = "the RunbookWorld extension did not load"

func ready() -> bool:
	return world != null and world.ready()

func boot(seed_value: int) -> String:
	return "" if world == null else str(world.boot(seed_value))

# One line in, the whole response out. The trailing "." terminator is stripped
# here so nothing above this file has to know about it.
func exec(line: String) -> String:
	if world == null:
		return "-ERR no world\n"
	var out := str(world.exec(line))
	if out.ends_with(".\n"):
		out = out.substr(0, out.length() - 2)
	return out

# The status code off the "+OK 201 400 ms" line, or -1 for a -ERR.
func status(response: String) -> int:
	if not response.begins_with("+OK"):
		return -1
	var head := response.get_slice("\n", 0).split(" ", false)
	if head.size() >= 2 and head[1].is_valid_int():
		return int(head[1])
	return 200

# DID IT ACTUALLY WORK -- which is not the same question as the status code,
# and the difference is a whole vendor's personality. Halcyon answers 200 to
# everything and puts the error in the body, in XML. A client that trusted the
# status code would show the player a green tick over a home folder that does
# not exist, and the player would learn to distrust the game rather than the
# vendor.
func ok(response: String) -> bool:
	var st := status(response)
	if st < 0 or st >= 400:
		return false
	if response.find("<error") >= 0 or response.find("\"error\"") >= 0:
		return false
	return true

func error_text(response: String) -> String:
	if response.begins_with("-ERR"):
		return response.get_slice("\n", 0).substr(5)
	for raw in body_lines(response):
		var l := str(raw)
		var i := l.find("\"error\":\"")
		if i >= 0:
			return l.substr(i + 9).get_slice("\"", 0)
		i = l.find("<error")
		if i >= 0:
			var j := l.find(">", i)
			return l.substr(j + 1).get_slice("<", 0)
	return ""

# Everything after the +OK line, without empties.
func body_lines(response: String) -> Array:
	var out := []
	var lines := response.split("\n")
	for i in range(1, lines.size()):
		var l := str(lines[i]).strip_edges()
		if l != "":
			out.append(l)
	return out

# EVERY object in the response, INCLUDING one sitting on the +OK line.
#
# Short answers put their object there -- `+OK {"org":...}` -- and long ones
# put a header there and the objects underneath. Reading only the body meant
# world.info parsed to nothing and the panel clock showed question marks,
# which looked like the clock being broken rather than the reader being wrong.
func _object_lines(response: String) -> Array:
	var out := body_lines(response)
	var head := response.get_slice("\n", 0)
	var brace := head.find("{")
	if brace >= 0:
		out.push_front(head.substr(brace))
	return out

# A flat {"k":"v", "k":123} object, which is what every answer in this game is
# shaped like. Deliberately not a JSON parser: the day this needs one is the
# day a response has grown a structure the player's script would also have to
# fight, and that is a signal about the protocol, not about this function.
#
# NESTING IS FLATTENED ON PURPOSE. A ticket carries
# `"subject":{"kind":"user","ref":"u_00041"}`, and what the client wants from
# that is `ref`. Walking straight through the braces gives it, and gives the
# ticket's own `fields` the same way, which is exactly the shape a player's
# script wants too -- one dictionary of everything the ticket said.
func fields(line: String) -> Dictionary:
	var d := {}
	var i := 0
	while true:
		var k0 := line.find("\"", i)
		if k0 < 0: break
		var k1 := line.find("\"", k0 + 1)
		if k1 < 0: break
		var key := line.substr(k0 + 1, k1 - k0 - 1)
		var c := line.find(":", k1)
		if c < 0: break
		var rest := line.substr(c + 1).strip_edges()
		if rest.begins_with("\""):
			# c + 1 is the value's OPENING quote. It was c + 2 for one
			# afternoon, which skipped the opening quote, found the closing
			# one, and returned everything between that and the next key's
			# quote -- so every field in the client came back as "," and the
			# ticket list rendered as punctuation. Off by one, in a parser,
			# reading correct data.
			var v1 := line.find("\"", c + 1)
			var v2 := line.find("\"", v1 + 1)
			if v2 < 0: break
			d[key] = line.substr(v1 + 1, v2 - v1 - 1)
			i = v2 + 1
		elif rest.begins_with("["):
			# AN ARRAY IS KEPT WHOLE, as raw text, and skipped over.
			#
			# Objects are walked into and flattened -- that is how
			# `"subject":{"ref":"u_00041"}` becomes `ref`, which is what every
			# caller actually wants. Arrays are not: walking into
			# `"fields":["login","user_ref","dept"]` turns the second element
			# into a KEY whose value is the third, which is how the New
			# Account form came to render one box labelled login instead of
			# four. list_of() unpacks the raw text when somebody wants it.
			var a0 := line.find("[", c)
			var depth := 0
			var end := a0
			while end < line.length():
				if line[end] == "[": depth += 1
				elif line[end] == "]":
					depth -= 1
					if depth == 0: break
				end += 1
			d[key] = line.substr(a0, end - a0 + 1)
			i = end + 1
		else:
			var end := c + 1
			while end < line.length() and ",}]".find(line[end]) < 0:
				end += 1
			d[key] = line.substr(c + 1, end - c - 1).strip_edges()
			i = end
	return d

# Unpack the quoted items out of a raw "[\"a\",\"b\"]" captured by fields().
func list_of(o: Dictionary, key: String) -> Array:
	var raw := str(o.get(key, ""))
	var out := []
	var i := 0
	while true:
		var a := raw.find("\"", i)
		if a < 0: break
		var b := raw.find("\"", a + 1)
		if b < 0: break
		out.append(raw.substr(a + 1, b - a - 1))
		i = b + 1
	return out

# RECORDS, IN WHATEVER THE VENDOR SPEAKS.
#
# Veridian answers JSON. Halcyon answers XML, because Halcyon is the legacy
# one and that is its whole personality. A browse view that only understood
# JSON showed an empty file server -- which reads as "you have no home
# folders" rather than "this client cannot read this vendor".
#
# The player's scripts hit exactly this wall, in exactly this place, and have
# to write exactly this function. Being made to do it twice is the lesson.
func records(response: String) -> Array:
	if response.find("<record") < 0:
		return objects(response)
	var out := []
	var i := 0
	while true:
		var a := response.find("<record>", i)
		if a < 0: break
		var b := response.find("</record>", a)
		if b < 0: break
		var inner := response.substr(a + 8, b - a - 8)
		var d := {}
		var j := 0
		while true:
			var t0 := inner.find("<", j)
			if t0 < 0: break
			var t1 := inner.find(">", t0)
			if t1 < 0: break
			var tag := inner.substr(t0 + 1, t1 - t0 - 1)
			var e0 := inner.find("</" + tag + ">", t1)
			if e0 < 0: break
			d[tag] = inner.substr(t1 + 1, e0 - t1 - 1)
			j = e0 + tag.length() + 3
		out.append(d)
		i = b + 9
	return out

func objects(response: String) -> Array:
	var out := []
	for raw in _object_lines(response):
		var l := str(raw)
		if l.begins_with("{"):
			out.append(fields(l))
	return out
