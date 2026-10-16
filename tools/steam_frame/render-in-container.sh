#!/usr/bin/env bash
# The body of build.sh's render step. Runs inside the build image with the repo mounted at /src
# and the user's DIABDAT.MPQ mounted read-only at /diabdat/DIABDAT.MPQ. It starts the packaged
# game with the VR mode's headset-free renderer (DEVILUTIONX_VR_OFFSCREEN) on lavapipe, Mesa's
# software Vulkan, so the whole D3D11 on DXVK and HLSL on vkd3d path draws real levels with no
# headset and no GPU. The pictures go to build-frame/render/level<N>/ as PNGs: eye0..eye7 are
# the view from the player turned in 45 degree steps, stereo0 and stereo1 one frame for both eyes.
#
#   LEVELS="0 2 5"   which levels to draw: 0 is the town, 1 to 16 the dungeon (default "0 2")
#   SETTINGS="DEVILUTIONX_VR_MSAA=1 ..."   settings of the VR mode for the run
set -euo pipefail

pkg=/src/build-frame/package/DiabloVR
render=/src/build-frame/render
export XDG_DATA_HOME="$render/xdg"
data="$XDG_DATA_HOME/DiabloVR"

[[ -f /diabdat/DIABDAT.MPQ ]] || { echo "render: DIABDAT.MPQ is not mounted" >&2; exit 1; }
[[ -f "$pkg/lib/libdxvk_d3d11.so.0" ]] || { echo "render: the package has no DXVK (build it with VR=ON)" >&2; exit 1; }
rm -rf "$render"
mkdir -p "$data"
ln -s /diabdat/DIABDAT.MPQ "$data/DIABDAT.MPQ"

Xvfb :99 -screen 0 1280x720x24 > "$render/xvfb.log" 2>&1 &
xvfb=$!
trap 'kill "$xvfb" 2> /dev/null || true' EXIT
for _ in $(seq 50); do
	[[ -S /tmp/.X11-unix/X99 ]] && break
	sleep 0.2
done
[[ -S /tmp/.X11-unix/X99 ]] || { echo "render: Xvfb did not start" >&2; exit 1; }

export DISPLAY=:99
export SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy
unset SDL_VIDEODRIVER
export DXVK_LOG_LEVEL=info DXVK_LOG_PATH="$render"
# Starts a game by itself, so the renderer has a level to draw.
export DEVILUTIONX_VR_AUTOSTART=1
for setting in ${SETTINGS:-}; do
	export "${setting?}"
done

cd "$pkg"
failed=0
for level in ${LEVELS:-0 2}; do
	shots="$render/level$level"
	mkdir -p "$shots"
	export DEVILUTIONX_VR_OFFSCREEN="$shots"
	export DEVILUTIONX_VR_DEBUG_LEVEL="$level"
	echo "== level $level"
	./LAUNCH-STEAM-FRAME.sh --vr -n --verbose &
	game=$!
	# Software rendering: minutes, not seconds.
	for _ in $(seq "${RENDER_TIMEOUT:-600}"); do
		[[ -e "$shots/done" ]] && break
		kill -0 "$game" 2> /dev/null || break
		sleep 1
	done
	if [[ -e "$shots/done" ]]; then
		for bmp in "$shots"/*.bmp; do
			convert "$bmp" "${bmp%.bmp}.png"
			rm -f "$bmp"
		done
		for png in "$shots"/eye*.png "$shots"/stereo*.png; do
			echo "  $(basename "$png"): mean brightness $(convert "$png" -colorspace Gray -format '%[fx:mean]' info:)"
		done
	else
		echo "render: level $level did not finish" >&2
		# What the game window shows says where it stopped.
		import -window root "$shots/stuck.png" || true
		failed=1
	fi
	kill -TERM "$game" 2> /dev/null || true
	for _ in $(seq 50); do
		kill -0 "$game" 2> /dev/null || break
		sleep 0.2
	done
	kill -KILL "$game" 2> /dev/null || true
	wait "$game" 2> /dev/null || true
	cp "$data/run.log" "$shots/run.log"
	grep -E 'VR:|err:|rror' "$shots/run.log" | grep -v 'VR: perf' | cut -c1-240 | sort | uniq -c | sort -rn | sed -n 1,30p || true
done
exit "$failed"
