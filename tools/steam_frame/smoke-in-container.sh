#!/usr/bin/env bash
# The body of build.sh's smoke step. Runs inside the build image with the repo mounted at /src and
# the user's DIABDAT.MPQ mounted read-only at /diabdat/DIABDAT.MPQ. It starts the packaged game
# through its launcher under Xvfb, waits for the main menu, saves a screenshot, and checks that
# SIGTERM ends the game.
set -euo pipefail

pkg=/src/build-frame/package/DiabloVR
smoke=/src/build-frame/smoke
export XDG_DATA_HOME="$smoke/xdg"
data="$XDG_DATA_HOME/DiabloVR"
shot="$smoke/menu.png"

[[ -f /diabdat/DIABDAT.MPQ ]] || { echo "smoke: DIABDAT.MPQ is not mounted" >&2; exit 1; }
rm -rf "$smoke"
mkdir -p "$data"
# The data folder on the Frame holds the file itself. Here it is a link to the read-only mount,
# so the game's data is never copied into the repo.
ln -s /diabdat/DIABDAT.MPQ "$data/DIABDAT.MPQ"

Xvfb :99 -screen 0 1280x720x24 > "$smoke/xvfb.log" 2>&1 &
xvfb=$!
trap 'kill "$xvfb" 2> /dev/null || true' EXIT
for _ in $(seq 50); do
	[[ -S /tmp/.X11-unix/X99 ]] && break
	sleep 0.2
done
[[ -S /tmp/.X11-unix/X99 ]] || { echo "smoke: Xvfb did not start" >&2; exit 1; }

export DISPLAY=:99
# No GPU in the container: SDL's own software renderer, and no sound device.
export SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy
unset SDL_VIDEODRIVER # the launcher defaults it to x11

cd "$pkg"

echo "== launcher, --version (this log becomes run.prev.log)"
./LAUNCH-STEAM-FRAME.sh --version
cat "$data/run.log"
grep -q '^DevilutionX v' "$data/run.log"

echo "== launcher, one-shot arguments"
# -n skips the startup videos. A normal launch must not inherit it.
printf '%s\n' "-n" > "$data/launch-once.args"
./LAUNCH-STEAM-FRAME.sh &
game=$!
sleep 1
[[ ! -e "$data/launch-once.args" ]] || { echo "smoke: launch-once.args was not consumed" >&2; exit 1; }
grep -q '^DevilutionX v' "$data/run.prev.log" || { echo "smoke: run.prev.log is missing the previous launch" >&2; exit 1; }
head -1 "$data/run.log"
grep -q '(once: -n)' "$data/run.log" || { echo "smoke: the one-shot argument is not in run.log" >&2; exit 1; }

echo "== waiting for the main menu"
# With the startup videos skipped the title and the main menu follow at once. The menu is dark art
# on black, so the test is "not black", and the picture is checked by eye afterwards (menu.png). The
# game goes on to its intro movie after half a minute at the menu, so the frame is taken early.
mkdir -p "$smoke/frames"
lit() { awk -v m="$1" 'BEGIN { exit !(m > 0.005) }'; }
grab() { # $1 = name; prints the mean brightness of the screen
	import -window root "$smoke/frames/$1.png"
	convert "$smoke/frames/$1.png" -colorspace Gray -format '%[fx:mean]' info:
}
found=0
for i in $(seq 90); do
	kill -0 "$game" 2> /dev/null || { echo "smoke: the game exited early" >&2; tail -20 "$data/run.log" >&2; exit 1; }
	sleep 1
	mean="$(grab "f$i")"
	echo "  ${i}s: mean brightness $mean"
	if lit "$mean"; then
		found=1
		break
	fi
done
[[ "$found" = 1 ]] || { echo "smoke: the screen stayed black" >&2; exit 1; }
sleep 3
mean="$(grab menu)"
cp "$smoke/frames/menu.png" "$shot"
echo "screenshot: $shot (mean brightness $mean)"
lit "$mean" || { echo "smoke: the screenshot is black" >&2; exit 1; }

echo "== SIGTERM"
kill -0 "$game" || { echo "smoke: the game is gone before SIGTERM" >&2; exit 1; }
kill -TERM "$game"
start=$SECONDS
status=0
for _ in $(seq 50); do
	kill -0 "$game" 2> /dev/null || break
	sleep 0.2
done
if kill -0 "$game" 2> /dev/null; then
	echo "smoke: the game ignored SIGTERM for 10 seconds" >&2
	kill -KILL "$game"
	exit 1
fi
wait "$game" || status=$?
echo "exited with status $status after about $((SECONDS - start))s"

echo "== log"
cat "$data/run.log"
[[ ! -e "$data/launch-once.args" ]]
echo "smoke ok"
