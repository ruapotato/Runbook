# /root/examples/selftest.sh -- does the shell actually work?
#
#   sh /root/examples/selftest.sh
#
# The same argument as selftest.py: a shell you cannot trust teaches a player
# that they cannot script, which is the opposite of the point. Everything
# below is a thing you can use in your own scripts.

echo "--- echo and quoting ---"
echo hello world
echo "quoted string with spaces"
echo 'single quotes too'

echo "--- variables ---"
NAME=harbrook
echo $NAME
echo "in a string: $NAME"

echo "--- command substitution ---"
HOST=$(cat /etc/hostname)
echo "host is $HOST"

echo "--- pipelines ---"
echo one two three | wc
echo abc | rev
echo abc | rev | rev

echo "--- redirection ---"
echo first > /tmp/sh_test
echo second >> /tmp/sh_test
cat /tmp/sh_test

echo "--- for loops ---"
for i in a b c; do echo "loop $i"; done

echo "--- and, or, exit status ---"
true && echo "and ran"
false || echo "or ran"
true
echo "status $?"

echo "--- globbing ---"
ls /bin/rb

echo "--- calling the game ---"
rb world.info | head

echo "--- a loop over the queue, which is the first real script anybody writes"
for t in $(rb ticket.list open 3 | grep TCK); do echo "saw a ticket"; done

echo "selftest.sh: done"
