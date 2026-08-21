# terminal.gd — a terminal, not a text box with a log above it.
#
# LIFTED WHOLE from ~/NOMINAL/game/scripts/terminal.gd, with the machine
# swapped for the API and an X11 selection added. The first version I wrote
# for RUNBOOK had a scrollback above and a stagnant input line below -- which
# is precisely the thing the note below was written to stop somebody building
# again, and I built it again anyway, from scratch, without reading it. A
# playtester said "that is not a terminal" and was right.
#
# WHAT THIS ONE TALKS TO. NOMINAL's ran commands on an emulated machine and
# completed filenames by asking it to `ls`. There is no machine here yet (that
# is M4), so `on_command` is the RUNBOOK API and completion is driven by what
# the API says exists: the verbs `help` advertises, the appliances installed,
# the endpoints each one has. Same rule, same reason -- a completion is proof
# the thing is there.
#
# David: "The terminal especially is a piece of junk. I don't want a history
# scroll back and a stagnant input line. I want it to resemble a real
# terminal. When you type, it goes into the actual terminal space."
#
# So there is no LineEdit anywhere. This control owns the screen: the
# transcript and the line being typed are the same buffer, drawn by the same
# code, with one block cursor sitting where the next character will land. Keys
# arrive through _gui_input and are handled here -- printable characters,
# backspace, left and right, home and end, and a command history on the up and
# down arrows, which is the thing you miss within about ten seconds of not
# having it.
#
# The control never interprets a command. It hands the line to on_command and
# prints whatever comes back, so this file cannot know anything the machine
# does not. Tab completion obeys the same rule: the FIRST word is completed
# against the list of programs the guest is built with, and every later word
# is completed by asking the machine to `ls` the directory in question. No
# guessed filenames, ever -- a terminal that offers you a path the machine
# does not have is lying to you about the machine.

extends Control

var mono: Font
var bg := Color("#0b0e13")
var fg := Color("#cfd8e3")
var accent := Color("#6fdc96")

# Set by the desktop.
var on_command: Callable = func(_s: String) -> String: return ""
var prompt_fn: Callable = func() -> String: return "$ "

# Set by whoever opens the window, because a terminal on YOUR workstation and
# a console on somebody else's machine are not the same thing and must not
# claim to be.
var banner: PackedStringArray = []
var lines: PackedStringArray = []
var cur := ""              # the line being typed
var caret := 0             # where in it the cursor is
var history: PackedStringArray = []
var hpos := -1
var scroll := 0            # how many lines up from the bottom we are looking
var blink := 0.0
var busy := false
# A COMMAND THAT IS NOT FINISHED YET.
#
# Typing `for i in dev sys proc` and pressing enter answered "expected do",
# so you could not type the chroot line the way the boot output prints it --
# across lines, like every shell on earth. If a line opens a construct, the
# terminal keeps it and shows a continuation prompt instead of running it.
var pending := ""

# Reverse history search, Ctrl-R. It is not a mode with its own widget: the
# line being typed IS the match, and only the prompt in front of it changes.
var rsearch := ""          # what has been typed into the search
var rsearch_on := false
var rsearch_at := 0        # index in history the current match came from
var rsearch_saved := ""    # the line to put back if the search is abandoned

# --------------------------------------------------------------- selection
# Dragging over the transcript selects it, and selecting puts it in PRIMARY --
# the X11 convention, and the reason middle-click paste feels like nothing at
# all when it works and like a missing limb when it does not.
const Clip := preload("res://scripts/clip.gd")
var sel_from := Vector2i(-1, -1)     # (row in the buffer, column)
var sel_to := Vector2i(-1, -1)
var selecting := false

const LINE_H := 15
const PAD := 6
const MAX_LINES := 4000

# EVERY PROGRAM THE GUEST IS BUILT WITH. Copied by hand from the `for p in
# ...` loop in tools/mkguest.sh, which is the authority: that loop is what
# compiles the userland onto the disk. THIS LIST MUST BE UPDATED WHEN
# mkguest.sh CHANGES -- add a guest program there and it will not complete
# here until you add it here too.
# THE VERBS, READ OUT OF `help` RATHER THAN LISTED HERE.
#
# --health already asserts that every verb `help` advertises actually
# dispatches, so completing against `help` completes against something proved
# to work. A hardcoded list would be a second copy of the API surface, and the
# second copy is always the one that goes stale.
var COMMANDS: PackedStringArray = PackedStringArray()

# WHAT IS ACTUALLY ON THE DISK, asked of the machine.
#
# NOMINAL hardcoded this list and left a comment saying it must be updated
# when the guest userland changes -- which is the kind of note that is true
# right up until it is not. `ls /bin /sbin /usr/bin` is one command and it
# cannot go stale, and it means completion is proof the program is there.
func _load_commands() -> void:
	COMMANDS = PackedStringArray()
	for dir in ["/bin", "/sbin", "/usr/bin", "/usr/sbin"]:
		for line in str(on_command.call("ls " + dir)).split("\n"):
			var f := str(line).split(" ", false)
			# ls prints `-0755  <size>  name`.
			if f.size() >= 3 and str(f[0]).length() == 5:
				COMMANDS.append(str(f[2]))
	COMMANDS.sort()

# AND THE GAME'S VERBS, AFTER `rb`. The one place the terminal knows anything
# about RUNBOOK: `rb ` is followed by an API verb, and those come from `help`,
# which --health has already proved every entry of.
var RB_VERBS: PackedStringArray = PackedStringArray()

func _load_rb_verbs() -> void:
	RB_VERBS = PackedStringArray()
	for line in str(on_command.call("rb help")).split("\n"):
		var l := str(line).strip_edges()
		if l == "" or l.begins_with("+OK") or l.begins_with("-ERR") or l.begins_with("."):
			continue
		if str(line).begins_with(" ") or str(line).begins_with("\t"):
			continue
		var verb := l.get_slice(" ", 0)
		if verb != "":
			RB_VERBS.append(verb)
	RB_VERBS.sort()

# Words after which the next word is a command again, so `... && ls fo<Tab>`
# completes a program name and not a file in the current directory.
# There is no shell grammar here -- no pipes, no `&&`, no `do`. One verb per
# line, which is what an API is. So the first word is the only place a verb
# can go.
const CMD_AFTER := []


func _ready() -> void:
	if lines.is_empty():
		lines = banner.duplicate()
	focus_mode = Control.FOCUS_ALL
	mouse_filter = Control.MOUSE_FILTER_STOP
	if mono == null:
		mono = preload("res://scripts/uifont.gd").mono()
	set_process(true)


func take_focus() -> void:
	grab_focus()


func _process(dt: float) -> void:
	blink += dt
	if blink > 0.5:
		blink -= 0.5
		queue_redraw()


# ------------------------------------------------------------------ output --

func write(s: String) -> void:
	if s == "":
		return
	var parts := s.split("\n")
	for i in range(parts.size()):
		if i == parts.size() - 1 and parts[i] == "":
			break
		lines.append(parts[i])
	_trim()
	scroll = 0
	queue_redraw()


# The screen went black. Used when a machine is power cycled: what follows is
# this boot, not this boot appended to the last one.
func clear() -> void:
	lines = PackedStringArray()
	cur = ""
	caret = 0
	pending = ""
	rsearch_on = false
	scroll = 0
	queue_redraw()


func _trim() -> void:
	while lines.size() > MAX_LINES:
		lines.remove_at(0)


# Type a whole string as though at the keyboard, for tests and screenshots.
func feed(s: String) -> void:
	for ch in s:
		if ch == "\n":
			_enter()
		else:
			cur = cur.insert(caret, ch)
			caret += 1
	queue_redraw()


func _incomplete(s2: String) -> bool:
	var t := s2.strip_edges()
	if t.ends_with("\\"):
		return true
	# `for` is open until `done`; a trailing `do` or `;` is also unfinished.
	var has_for := t.begins_with("for ") or t.find("; for ") >= 0
	if has_for and t.find("done") < 0:
		return true
	if t.ends_with(";") or t.ends_with("do") or t.ends_with("&&") or t.ends_with("||"):
		return true
	return false


func _enter() -> void:
	var line := cur
	lines.append((prompt_fn.call() if pending == "" else "> ") + line)
	cur = ""
	caret = 0

	# Join it to whatever came before, and if the whole thing is still open,
	# ask for more rather than running a fragment.
	var whole := line if pending == "" else pending + "; " + line
	if whole.strip_edges().ends_with("\\"):
		whole = whole.strip_edges().substr(0, whole.strip_edges().length() - 1)
	if _incomplete(whole):
		pending = whole
		queue_redraw()
		return
	pending = ""
	line = whole
	if line.strip_edges() != "":
		history.append(line)
	hpos = -1
	_trim()
	queue_redraw()

	busy = true
	var out: String = on_command.call(line)
	busy = false
	write(out)


# ------------------------------------------------------------------- input --

# ONE ARITHMETIC, SHARED WITH _draw(). Two copies of "which line is at the top
# of the screen" is how a selection highlights the wrong row after a scroll, so
# the window is computed once, here, and the drawing asks for it too.
func _screen() -> PackedStringArray:
	var s: PackedStringArray = lines.duplicate()
	s.append(_live_prompt() + cur)
	return s


func _rows() -> int:
	return maxi(1, int((size.y - PAD * 2) / LINE_H))


func _first_row(total: int) -> int:
	return maxi(0, total - scroll - _rows())


func _cell_w() -> float:
	return maxf(1.0, mono.get_string_size("M", HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x)


func _cell_at(p: Vector2) -> Vector2i:
	var s := _screen()
	var row := _first_row(s.size()) + int(floor((p.y - PAD) / float(LINE_H)))
	row = clampi(row, 0, maxi(0, s.size() - 1))
	return Vector2i(row, maxi(0, int(round((p.x - PAD) / _cell_w()))))


func _selected_text() -> String:
	if sel_from.x < 0 or sel_to.x < 0:
		return ""
	var a := sel_from
	var b := sel_to
	if b.x < a.x or (b.x == a.x and b.y < a.y):
		var sw := a; a = b; b = sw
	var all := _screen()
	var out := PackedStringArray()
	for i in range(a.x, mini(b.x + 1, all.size())):
		var l := str(all[i])
		var s0: int = clampi(a.y if i == a.x else 0, 0, l.length())
		var s1: int = clampi(b.y if i == b.x else l.length(), 0, l.length())
		out.append(l.substr(s0, maxi(0, s1 - s0)))
	return "\n".join(out)


func insert_text(s: String) -> void:
	# A pasted newline runs the line, exactly as a terminal does.
	for ch in s:
		if ch == "\n" or ch == "\r":
			_enter()
		else:
			cur = cur.insert(caret, ch)
			caret += 1
	queue_redraw()


func _mouse(e: InputEvent) -> bool:
	if e is InputEventMouseButton:
		var mb := e as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_LEFT:
			if mb.pressed:
				grab_focus()
				sel_from = _cell_at(mb.position)
				sel_to = sel_from
				selecting = true
			else:
				selecting = false
				Clip.set_primary(_selected_text())
			queue_redraw()
			return true
		# MIDDLE-CLICK PASTES THE SELECTION -- not the clipboard. That
		# distinction IS the feature: two buffers, independent, so a login can
		# sit in one while a path sits in the other.
		if mb.button_index == MOUSE_BUTTON_MIDDLE and mb.pressed:
			insert_text(Clip.get_primary())
			return true
	elif e is InputEventMouseMotion and selecting:
		sel_to = _cell_at((e as InputEventMouseMotion).position)
		queue_redraw()
		return true
	return false


func _gui_input(e: InputEvent) -> void:
	if _mouse(e):
		accept_event()
		return
	if e is InputEventKey and e.pressed:
		var k0 := e as InputEventKey
		# Ctrl-Shift-C / Ctrl-Shift-V, because plain Ctrl-C is the interrupt
		# and always will be. Every terminal emulator makes this choice and
		# every user has the muscle memory for it.
		if k0.ctrl_pressed and k0.shift_pressed and k0.keycode == KEY_C:
			Clip.set_clipboard(_selected_text())
			accept_event()
			return
		if k0.ctrl_pressed and k0.shift_pressed and k0.keycode == KEY_V:
			insert_text(Clip.get_clipboard())
			accept_event()
			return
	if e is InputEventMouseButton and e.pressed:
		grab_focus()
		if e.button_index == MOUSE_BUTTON_WHEEL_UP:
			scroll = min(scroll + 3, max(0, lines.size() - 4))
			queue_redraw()
		elif e.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			scroll = max(scroll - 3, 0)
			queue_redraw()
		return

	if not (e is InputEventKey) or not e.pressed:
		return
	var k := e as InputEventKey
	accept_event()

	# An empty prompt means there is nothing here to type at -- a console
	# showing a machine that never booted. Swallow the key rather than
	# collecting a line nobody will ever run.
	if str(prompt_fn.call()) == "":
		return

	# A search in progress eats most keys; the ones it does not want end the
	# search and are then handled below exactly as they always were.
	if rsearch_on and _rsearch_key(k):
		queue_redraw()
		return

	match k.keycode:
		KEY_ENTER, KEY_KP_ENTER:
			_enter()
			return
		KEY_BACKSPACE:
			if caret > 0:
				cur = cur.substr(0, caret - 1) + cur.substr(caret)
				caret -= 1
			queue_redraw(); return
		KEY_DELETE:
			if caret < cur.length():
				cur = cur.substr(0, caret) + cur.substr(caret + 1)
			queue_redraw(); return
		KEY_LEFT:
			caret = max(0, caret - 1); queue_redraw(); return
		KEY_RIGHT:
			caret = min(cur.length(), caret + 1); queue_redraw(); return
		KEY_HOME:
			caret = 0; queue_redraw(); return
		KEY_END:
			caret = cur.length(); queue_redraw(); return
		KEY_UP:
			if history.size() > 0:
				hpos = history.size() - 1 if hpos < 0 else max(0, hpos - 1)
				cur = history[hpos]
				caret = cur.length()
			queue_redraw(); return
		KEY_DOWN:
			if hpos >= 0:
				hpos += 1
				if hpos >= history.size():
					hpos = -1
					cur = ""
				else:
					cur = history[hpos]
				caret = cur.length()
			queue_redraw(); return
		KEY_PAGEUP:
			scroll = min(scroll + 10, max(0, lines.size() - 4)); queue_redraw(); return
		KEY_PAGEDOWN:
			scroll = max(scroll - 10, 0); queue_redraw(); return
		KEY_U:
			if k.ctrl_pressed:
				cur = cur.substr(caret)
				caret = 0
				queue_redraw(); return
		KEY_C:
			if k.ctrl_pressed:
				lines.append(prompt_fn.call() + cur + "^C")
				cur = ""; caret = 0
				queue_redraw(); return
		KEY_A:
			if k.ctrl_pressed:
				caret = 0; queue_redraw(); return
		KEY_E:
			if k.ctrl_pressed:
				caret = cur.length(); queue_redraw(); return
		KEY_K:
			if k.ctrl_pressed:
				cur = cur.substr(0, caret)
				queue_redraw(); return
		KEY_W:
			if k.ctrl_pressed:
				# Back over the spaces, then over the word they follow.
				var i := caret
				while i > 0 and cur[i - 1] == " ":
					i -= 1
				while i > 0 and cur[i - 1] != " ":
					i -= 1
				cur = cur.substr(0, i) + cur.substr(caret)
				caret = i
				queue_redraw(); return
		KEY_L:
			if k.ctrl_pressed:
				# The screen goes; the line you are halfway through does not.
				# clear() drops it on purpose -- a power cycle really does take
				# your typing with it -- so this must not call clear().
				lines = PackedStringArray()
				scroll = 0
				queue_redraw(); return
		KEY_R:
			if k.ctrl_pressed:
				if history.size() > 0:
					rsearch_on = true
					rsearch = ""
					rsearch_saved = cur
					rsearch_at = history.size() - 1
				queue_redraw(); return

	# TAB AND FRIENDS ARE NOT TEXT. Godot reports Tab with unicode 0, and the
	# old guard let anything >= 32 through -- but the keycode branch below
	# never ran for Tab, so a NUL went into the line buffer and Godot then
	# refused to render it: "Unicode parsing error... Unexpected NUL
	# character". Filter on the CODE POINT being printable, not on the key.
	if k.keycode == KEY_TAB:
		_tab()
		return
	var ch := char(k.unicode)
	if k.unicode >= 32 and k.unicode != 127 and ch != "" and ch != "\u0000":
		cur = cur.insert(caret, ch)
		caret += 1
		scroll = 0
		queue_redraw()


# -------------------------------------------------------------- completion --

# Tab. Complete the word the caret is sitting in the middle (or the end) of.
# One match goes straight in; several are pushed as far as they agree and then
# listed; none does nothing at all, quietly, which is what a shell does.
func _tab() -> void:
	var start := _word_start()
	var word := cur.substr(start, caret - start)

	# A word with a slash in it is a path even in command position -- typing
	# `/bin/l<Tab>` means the file, not the program name.
	var cands: PackedStringArray
	if word.find("/") < 0 and _at_command(start):
		cands = _command_matches(word)
	else:
		cands = _path_matches(word)
	if cands.is_empty():
		return

	# Only the last segment of a path is ours to replace: `ls` named the
	# entries in the directory, not the directory itself.
	var cut := word.rfind("/")
	var stem := word.substr(0, cut + 1)
	var base := word.substr(cut + 1)

	if cands.size() == 1:
		var only := cands[0]
		# A directory gets a slash so you can carry on into it; anything else
		# gets a space, because you are done with that word -- unless there is
		# already a space there, completing in the middle of a line.
		var tail := ""
		if not only.ends_with("/") and not cur.substr(caret).begins_with(" "):
			tail = " "
		_replace_word(start, stem + only + tail)
		return

	var common := _common_prefix(cands)
	if common.length() > base.length():
		_replace_word(start, stem + common)
		return

	# They agree on nothing more. Show them, above the prompt, and leave the
	# line exactly as it was -- the prompt below is redrawn with it intact.
	lines.append(_live_prompt() + cur)
	for row in _columns(cands):
		lines.append(row)
	_trim()
	scroll = 0
	queue_redraw()


func _word_start() -> int:
	var i := caret
	while i > 0 and cur[i - 1] != " ":
		i -= 1
	return i


# Is the word starting at `start` the command, rather than an argument to one?
func _at_command(start: int) -> bool:
	var before := cur.substr(0, start).strip_edges()
	if before == "":
		return true
	var words := before.split(" ", false)
	return CMD_AFTER.has(words[words.size() - 1])


func _command_matches(prefix: String) -> PackedStringArray:
	var out := PackedStringArray()
	for c in COMMANDS:
		if c.begins_with(prefix):
			out.append(c)
	out.sort()
	return out


# ASK THE MACHINE. The terminal has no idea what is on the disk and must not
# pretend to: it runs `ls` on the directory being completed and reads the
# answer, so a completion is proof the file is there. Lifted from NOMINAL,
# which is where that rule was written.
#
# The one addition: after `rb`, the words being completed are the game's API
# verbs rather than filenames, because that is what `rb` takes.
func _path_matches(word: String) -> PackedStringArray:
	var out := PackedStringArray()
	var head := cur.substr(0, caret)
	var words := head.split(" ", false)

	if words.size() > 0 and str(words[0]) == "rb":
		var argn: int = words.size() - (0 if head.ends_with(" ") else 1)
		if argn <= 1:
			if RB_VERBS.is_empty():
				_load_rb_verbs()
			for v in RB_VERBS:
				if str(v).begins_with(word):
					out.append(str(v))
			out.sort()
			return out

	var cut := word.rfind("/")
	var dir := word.substr(0, cut + 1)   # "" when the word has no slash
	var base := word.substr(cut + 1)

	var listing := str(on_command.call("ls " + (dir if dir != "" else ".")))
	for line in listing.split("\n"):
		var f := str(line).split(" ", false)
		# ls prints `d0755  <size>  name`. Anything that does not start with a
		# type-and-mode field is not an entry -- it is an error, or the path
		# echoed back because it was not a directory at all.
		if f.size() < 3 or str(f[0]).length() != 5 or "dl-".find(str(f[0])[0]) < 0:
			continue
		var name := str(f[2])
		if not name.begins_with(base):
			continue
		out.append((dir + name + "/") if str(f[0])[0] == "d" else (dir + name))
	out.sort()
	return out


func _replace_word(start: int, text: String) -> void:
	cur = cur.substr(0, start) + text + cur.substr(caret)
	caret = start + text.length()
	scroll = 0
	queue_redraw()


func _common_prefix(items: PackedStringArray) -> String:
	var out: String = items[0]
	for s in items:
		while out != "" and not s.begins_with(out):
			out = out.substr(0, out.length() - 1)
	return out


# Candidates across the screen the way a shell lays them out: down the first
# column, then the next, sized to the window we are actually drawn in.
func _columns(items: PackedStringArray) -> PackedStringArray:
	var w := 0
	for s in items:
		w = max(w, s.length())
	w += 2
	var avail := 80
	if mono != null and size.x > 0:
		var cw := mono.get_string_size("M", HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x
		if cw > 0:
			avail = int((size.x - PAD * 2) / cw)
	var cols: int = max(1, avail / w)
	var rows: int = (items.size() + cols - 1) / cols
	var out := PackedStringArray()
	for r in range(rows):
		var line := ""
		for c in range(cols):
			var i := c * rows + r
			if i < items.size():
				line += items[i].rpad(w)
		out.append(line.strip_edges(false, true))
	return out


# ------------------------------------------------------------ history search --

# The prompt in front of the line being typed. During a Ctrl-R it says so,
# which is the whole of the search's user interface.
func _live_prompt() -> String:
	if rsearch_on:
		return "(reverse-i-search)`%s': " % rsearch
	return "> " if pending != "" else str(prompt_fn.call())


# Walk backwards from `from` for a history entry containing the search text.
# No match leaves the line alone, the way a shell just stops moving.
func _rsearch_find(from: int) -> void:
	var i: int = min(from, history.size() - 1)
	while i >= 0:
		if history[i].find(rsearch) >= 0:
			rsearch_at = i
			cur = history[i]
			caret = cur.length()
			return
		i -= 1


# True if the search consumed the key. False ends the search -- with the match
# still in the line -- and lets the normal handling have it, so Enter runs what
# you found and the arrows start editing it.
func _rsearch_key(k: InputEventKey) -> bool:
	match k.keycode:
		KEY_BACKSPACE:
			if rsearch != "":
				rsearch = rsearch.substr(0, rsearch.length() - 1)
				_rsearch_find(history.size() - 1)
			return true
		KEY_ESCAPE:
			rsearch_on = false
			return true
		KEY_R:
			if k.ctrl_pressed:
				_rsearch_find(rsearch_at - 1)
				return true
		KEY_G:
			if k.ctrl_pressed:
				# Abandoned: you get back the line you were typing.
				cur = rsearch_saved
				caret = cur.length()
				rsearch_on = false
				return true
		KEY_C:
			if k.ctrl_pressed:
				cur = rsearch_saved
				caret = cur.length()
				rsearch_on = false
				return false
	if k.unicode >= 32 and k.unicode != 127:
		rsearch += char(k.unicode)
		_rsearch_find(history.size() - 1)
		return true
	rsearch_on = false
	return false


# ------------------------------------------------------------------ render --

# IS THIS LINE A DIAGNOSTIC, OR A LINE THAT HAPPENS TO CONTAIN THE WORD.
#
# The rule was "does the line contain cannot/fail/not found/refusing anywhere",
# and a playtester ran `pkg` with no arguments and watched one line of the grey
# usage text come out RED: "chrooting into it -- which you cannot do when the".
# It is prose, in the middle of a wrapped paragraph, and colouring it red says
# an error happened when nothing did. Colour that fires on the wrong line is
# worse than no colour, because red on this screen means "here is your fault".
#
# Every diagnostic in this system is written the unix way -- `tool: what went
# wrong` -- so that is what is matched: an unindented line whose first word
# ends in a colon, with the complaint after it. A wrapped continuation of that
# message is indented or has no such prefix, so it stays plain, which is also
# what it looks like in a real terminal.
const ERR_WORDS := ["cannot", "fail", "not found", "no such", "refus",
	"denied", "invalid", "unknown", "not a ", "missing"]

func _is_error(line: String) -> bool:
	if line == "" or line.begins_with(" ") or line.begins_with("\t"):
		return false
	var c := line.find(": ")
	if c < 1 or c > 20:
		return false
	# `tool:` and `tool: path:` are prefixes; a sentence with a colon in it is
	# not, and a sentence has spaces before the colon.
	if line.substr(0, c).find(" ") >= 0:
		return false
	var rest := line.substr(c + 2).to_lower()
	for w in ERR_WORDS:
		if rest.find(w) >= 0:
			return true
	return false


func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), bg)

	var rows := int((size.y - PAD * 2) / LINE_H)
	if rows < 1:
		return

	# The prompt line is part of the screen, not a separate widget below it.
	var screen: PackedStringArray = lines.duplicate()
	var prompt: String = "> " if pending != "" else str(prompt_fn.call())
	# What sits in front of the line being typed is not always the prompt --
	# during a Ctrl-R it is the search. The transcript colouring below still
	# wants the real prompt, so they are two different strings.
	var live := _live_prompt()
	screen.append(live + cur)

	var last := screen.size() - scroll
	var first := _first_row(screen.size())
	var y := PAD + LINE_H

	# The selection, painted under the text, in the same coordinates the mouse
	# handler produced -- both go through _first_row().
	var sa := sel_from
	var sb := sel_to
	if sa.x >= 0 and sb.x >= 0:
		if sb.x < sa.x or (sb.x == sa.x and sb.y < sa.y):
			var sw := sa; sa = sb; sb = sw
		var cwm := _cell_w()
		for i in range(maxi(first, sa.x), mini(last, sb.x + 1)):
			var l := str(screen[i])
			var s0: int = clampi(sa.y if i == sa.x else 0, 0, l.length())
			var s1: int = clampi(sb.y if i == sb.x else l.length(), 0, l.length())
			if s1 > s0:
				draw_rect(Rect2(PAD + s0 * cwm, PAD + (i - first) * LINE_H + 2,
					(s1 - s0) * cwm, LINE_H), Color(0.30, 0.45, 0.68, 0.55))
	for i in range(first, last):
		var line := screen[i]
		var col := fg
		if i == screen.size() - 1 and scroll == 0:
			# the line being typed: prompt in the accent colour
			draw_string(mono, Vector2(PAD, y), live,
				HORIZONTAL_ALIGNMENT_LEFT, -1, 13, accent)
			var pw := mono.get_string_size(live, HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x
			draw_string(mono, Vector2(PAD + pw, y), cur,
				HORIZONTAL_ALIGNMENT_LEFT, -1, 13, fg)
			if has_focus() and blink < 0.25 and live != "":
				var cw := mono.get_string_size(cur.substr(0, caret),
					HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x
				var w := mono.get_string_size("M", HORIZONTAL_ALIGNMENT_LEFT, -1, 13).x
				draw_rect(Rect2(PAD + pw + cw, y - 11, w, 14), fg)
				if caret < cur.length():
					draw_string(mono, Vector2(PAD + pw + cw, y), cur[caret],
						HORIZONTAL_ALIGNMENT_LEFT, -1, 13, bg)
		else:
			if _is_error(line):
				col = Color("#e06c75")
			elif line.begins_with(prompt) or line.find("# ") == 0:
				col = accent
			draw_string(mono, Vector2(PAD, y), line,
				HORIZONTAL_ALIGNMENT_LEFT, -1, 13, col)
		y += LINE_H

	if scroll > 0:
		var note := "-- scrolled back %d lines, PageDown to return --" % scroll
		draw_string(mono, Vector2(PAD, size.y - 4), note,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color("#d3b06a"))
