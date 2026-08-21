# /root/examples/selftest.py -- does this language actually work?
#
# Run it: py /root/examples/selftest.py
#
# It is here for two reasons. The first is that a scripting language you
# cannot trust is worse than none: a player debugging their own logic against
# an interpreter that is quietly wrong will conclude they cannot program, and
# that is the exact opposite of what this game is for.
#
# The second is that it is the clearest documentation of what the language
# HAS. Everything below is a thing you can use.

# SELF-COUNTING, because the first version of this file had a hardcoded total
# and got it wrong -- it reported "59 of 62 checks passed" while every single
# check was passing. A test that can be wrong about its own arithmetic is a
# test that will one day be wrong about yours.
results = []

def check(name, got, want):
    if got == want:
        append(results, 1)
        return 1
    print("FAIL", name, "got", got, "want", want)
    append(results, 0)
    return 0

# --- arithmetic -----------------------------------------------------------
check("add", 2 + 3, 5)
check("sub", 10 - 4, 6)
check("mul", 6 * 7, 42)
check("div", 9 / 2, 4)
check("floordiv", 9 // 2, 4)
check("mod", 9 % 4, 1)
check("pow", 2 ** 10, 1024)
check("neg", 0 - 5, -5)
check("precedence", 2 + 3 * 4, 14)
check("parens", (2 + 3) * 4, 20)

# --- comparison and logic -------------------------------------------------
check("lt", 1 < 2, True)
check("gt", 1 > 2, False)
check("le", 2 <= 2, True)
check("ge", 2 >= 3, False)
check("eq", 3 == 3, True)
check("ne", 3 != 3, False)
check("and", True and False, False)
check("or", True or False, True)
check("not", not False, True)

# --- variables and assignment ---------------------------------------------
x = 1
x = x + 1
check("assign", x, 2)
x += 3
check("pluseq", x, 5)
x -= 1
check("minuseq", x, 4)
x *= 2
check("stareq", x, 8)

# --- strings --------------------------------------------------------------
s = "hello"
check("strlen", len(s), 5)
check("concat", "ab" + "cd", "abcd")
check("streq", "abc" == "abc", True)
check("sub", sub("abcdef", 1, 3), "bc")
check("sub-tail", sub("abcdef", 3), "def")
check("lower", lower("ABC"), "abc")
check("upper", upper("abc"), "ABC")
check("find-hit", find("hello world", "world"), 6)
check("find-miss", find("hello", "zzz"), -1)
check("str-of-int", str(42), "42")
check("int-of-str", int("42"), 42)
check("int-negative", int("-7"), -7)

# --- lists ----------------------------------------------------------------
l = [1, 2, 3]
check("list-len", len(l), 3)
check("list-index", l[0], 1)
check("list-last", l[2], 3)
l[1] = 20
check("list-set", l[1], 20)

total = 0
for v in l:
    total = total + v
check("list-for", total, 24)

# --- dicts ----------------------------------------------------------------
d = {"a": 1, "b": 2}
check("dict-len", len(d), 2)
check("dict-get", d["a"], 1)
d["c"] = 3
check("dict-set", d["c"], 3)

# --- control flow ---------------------------------------------------------
n = 0
if 1 < 2:
    n = 1
elif 1 < 3:
    n = 2
else:
    n = 3
check("if", n, 1)

n = 0
if 2 < 1:
    n = 1
elif 1 < 3:
    n = 2
else:
    n = 3
check("elif", n, 2)

n = 0
if 2 < 1:
    n = 1
else:
    n = 3
check("else", n, 3)

i = 0
acc = 0
while i < 5:
    acc = acc + i
    i = i + 1
check("while", acc, 10)

acc = 0
for i in [1, 2, 3, 4, 5]:
    if i == 3:
        continue
    if i == 5:
        break
    acc = acc + i
check("break-continue", acc, 7)

# --- functions ------------------------------------------------------------
def double(v):
    return v * 2

def addup(a, b):
    return a + b

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

check("call", double(21), 42)
check("two-args", addup(2, 3), 5)
check("recursion", fib(10), 55)

def early(v):
    if v:
        return "yes"
    return "no"

check("early-return", early(True), "yes")
check("no-return-path", early(False), "no")

# --- the ship, which is why the language is here --------------------------
s = json(ship())
check("ship-answers", has(s, "hull"), True)
check("ship-has-rooms", find(ship(), "shields") > 0, True)
check("do-works", find(do("power shields 2"), "+OK"), 0)
check("do-refuses", find(do("power shields 99"), "-ERR"), 0)

parts = split("a,b,c", ",")
check("split-count", len(parts), 3)
check("split-mid", parts[1], "b")

# --- lists you build, dicts you walk --------------------------------------
built = []
for i in [1, 2, 3]:
    append(built, i * i)
check("append", len(built), 3)
check("append-value", built[2], 9)
check("join", join(["a", "b", "c"], "-"), "a-b-c")

d2 = {"x": 1}
d2["y"] = 2
ks = keys(d2)
check("keys-count", len(ks), 2)
check("keys-first", ks[0], "x")
check("has-yes", has(d2, "x"), True)
check("has-no", has(d2, "zzz"), False)

check("strip", strip("  padded  "), "padded")
check("replace", replace("a-b-c", "-", "+"), "a+b+c")
check("replace-delete", replace("hello", "l", ""), "heo")

# --- files, because a script that cannot keep a note is told everything twice
write("/tmp/selftest.txt", "kept")
check("read-back", read("/tmp/selftest.txt"), "kept")
check("read-missing", read("/tmp/does-not-exist"), nil)

# --- the report -----------------------------------------------------------
passed = 0
for v in results:
    passed = passed + v
print("selftest:", passed, "of", len(results), "checks passed")
if passed == len(results):
    print("selftest: OK")
else:
    print("selftest: FAILED")
