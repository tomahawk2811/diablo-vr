#!/usr/bin/env bash
# Runs the installed game on a Steam Frame with nobody wearing it, over SSH, and brings back
# what it measured or drew.
#
#   tools/steam_frame/unattended.sh guard
#   tools/steam_frame/unattended.sh bench [--level N] [--repeat N] ["NAME=value NAME=value" ...]
#   tools/steam_frame/unattended.sh session [--seconds N] [--no-dump] [NAME=value ...]
#   tools/steam_frame/unattended.sh steam [--seconds N] [DEVILUTIONX_VR_NAME=value ...]
#
#   guard    says whether the Frame is free to use, and why not if it is not. Exit 0: free.
#   bench    the VR mode's own benchmark (--vr-bench): what a headset frame draws, both eyes,
#            at 1728x1728, along a fixed camera path for 20 seconds, with no headset session.
#            It draws all of each eye image, the part the lenses hide included. Each quoted
#            argument is one configuration, a list of DEVILUTIONX_VR_ settings (none: the
#            defaults). Every configuration is run --repeat times (default 2) and the results
#            are printed as a table and kept in build-frame/bench/.
#   session  the VR mode itself, in the headset, for --seconds (default 60): the debug autostart
#            takes it into a game, both eyes are saved every 150 frames, and the last pictures
#            and the log come back to build-frame/session/. Nobody wearing it means no tracked
#            head, so pass DEVILUTIONX_VR_FORCE_RENDER=1 to have the world drawn anyway.
#            Saving the eyes stalls the game for some 45 ms each time: --no-dump when the
#            frame pacing is what is being looked at.
#
#   steam    the same, but started the way the Steam library starts it: in the Steam Linux
#            Runtime container, with the title's own SteamVR settings (refresh rate) and with
#            controllers. Settings for the run travel in the launcher's one-shot file.
#
# SteamVR runs one VR application at a time: starting ours ends whatever else is running, in the
# face of whoever wears the headset. So every run first checks that no other Steam title is
# running and that the headset is in standby (nobody has it on), and stops if not. If the headset
# is shared, FRAME_GUARD_CMD can name a command of your own (a booking check, say) that has to
# succeed before each run too. A session launched over SSH has no controllers; SteamVR only gives
# them to titles started from the Steam library.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
frame="steamos@${FRAME_HOST:-frame.local}"
title="${TITLE:-DiabloVR}"
ssh_opts=(-o ConnectTimeout=5 -o BatchMode=yes)
remote_data='$HOME/.local/share/DiabloVR'

die() {
	echo "$*" >&2
	exit 1
}

remote() {
	# shellcheck disable=SC2029
	ssh "${ssh_opts[@]}" "$frame" "$@"
}

# Prints nothing and returns 0 when the Frame is free; otherwise prints the reason.
guard() {
	local state
	if [[ -n "${FRAME_GUARD_CMD:-}" ]] && ! state="$(bash -c "$FRAME_GUARD_CMD" 2>&1)"; then
		echo "FRAME_GUARD_CMD said no: ${state%%$'\n'*}"
		return 1
	fi
	state="$(remote '
		running="$(ps -eo args | grep -E "reaper SteamLaunch|lepton-dev" | grep -v grep | head -3)"
		# The launcher starts the game as ./devilutionx from its folder, so its command line does not name the title.
		for p in $(pgrep -x devilutionx); do [ "$(readlink /proc/$p/cwd)" = "$HOME/devkit-game/'"$title"'" ] && running="$running devilutionx($p)"; done
		last="$(grep -E " - (entering|leaving) standby" ~/.local/share/Steam/logs/vrserver.txt | tail -1)"
		echo "running=$running"
		echo "standby=$last"
	')" || {
		echo "the Frame is not reachable at $frame"
		return 1
	}
	if [[ -n "$(sed -n 's/^running=//p' <<< "$state")" ]]; then
		echo "something is running on the Frame: $(sed -n 's/^running=//p' <<< "$state" | cut -c1-160)"
		return 1
	fi
	if ! grep -q '^standby=.*entering standby' <<< "$state"; then
		echo "the headset is not in standby, so someone may be wearing it: $(sed -n 's/^standby=//p' <<< "$state" | cut -c1-120)"
		return 1
	fi
}

cmd="${1:-}"
[[ -n "$cmd" ]] || die "usage: $0 guard | bench ... | session ..."
shift

case "$cmd" in
guard)
	if reason="$(guard)"; then
		echo "the Frame is free"
	else
		die "$reason"
	fi
	;;

bench)
	level=2
	repeat=2
	configs=()
	while (($#)); do
		case "$1" in
		--level) level="${2:?}"; shift 2 ;;
		--repeat) repeat="${2:?}"; shift 2 ;;
		*) configs+=("$1"); shift ;;
		esac
	done
	((${#configs[@]})) || configs=("")
	out="$root/build-frame/bench"
	mkdir -p "$out"
	printf '%-58s %7s %7s %7s %7s %7s\n' "settings" "frames" "gpu50" "gpu95" "scene" "post"
	n=0
	for config in "${configs[@]}"; do
		for ((r = 1; r <= repeat; r++)); do
			reason="$(guard)" || die "stopping: $reason"
			n=$((n + 1))
			for setting in $config; do
				[[ "$setting" =~ ^[A-Z][A-Z0-9_]*=[A-Za-z0-9._/,-]*$ ]] || die "bad setting $setting"
			done
			# The benchmark quits by itself; the timeout is for a build that hangs.
			remote "
				d=$remote_data
				rm -rf \$d/bench && mkdir -p \$d/bench
				cd ~/devkit-game/$title || exit 1
				export XDG_RUNTIME_DIR=/run/user/\$(id -u) SDL_AUDIODRIVER=dummy DEVILUTIONX_VR_OFFSCREEN=\$d/bench DEVILUTIONX_VR_BENCH_WIDTH=1728 DEVILUTIONX_VR_BENCH_HEIGHT=1728 $config
				timeout 120 ./LAUNCH-STEAM-FRAME.sh --vr -n --verbose --vr-bench $level > /dev/null 2>&1
				cat \$d/bench/bench.json 2> /dev/null
				rm -rf \$d/bench \$d/single_*.sv \$d/devilutionx_*.log
			" > "$out/run$n.json" || true
			python3 - "$out/run$n.json" "${config:-defaults}" <<'EOF'
import json, sys
try:
    b = json.load(open(sys.argv[1]))
    print("%-58s %7d %7.2f %7.2f %7.2f %7.2f" % (sys.argv[2][:58], b["frames"], b["gpu_ms"]["p50"], b["gpu_ms"]["p95"],
                                               b["gpu_p50_ms"]["scene"], b["gpu_p50_ms"]["post"]))
except Exception as error:
    print("%-58s no result (%s)" % (sys.argv[2][:58], error))
EOF
			# Let the GPU cool between runs: on a desk the headset throttles within minutes.
			sleep "${COOLDOWN:-15}"
		done
	done
	;;

session)
	seconds=60
	settings=()
	dump='DEVILUTIONX_VR_DUMP=$d/vr-dumps'
	while (($#)); do
		case "$1" in
		--seconds) seconds="${2:?}"; shift 2 ;;
		--no-dump) dump=""; shift ;;
		*)
			[[ "$1" =~ ^[A-Z][A-Z0-9_]*=[A-Za-z0-9._/,-]*$ ]] || die "bad setting $1"
			settings+=("$1")
			shift
			;;
		esac
	done
	reason="$(guard)" || die "stopping: $reason"
	out="$root/build-frame/session"
	rm -rf "$out"
	mkdir -p "$out"
	remote "
		d=$remote_data
		rm -rf \$d/vr-dumps && mkdir -p \$d/vr-dumps
		had_save=\$(ls \$d/single_*.sv 2> /dev/null | wc -l)
		cd ~/devkit-game/$title || exit 1
		export XDG_RUNTIME_DIR=/run/user/\$(id -u) SDL_AUDIODRIVER=dummy DEVILUTIONX_VR_AUTOSTART=1 $dump ${settings[*]:-}
		timeout -s TERM $seconds ./LAUNCH-STEAM-FRAME.sh --vr -n --verbose > /dev/null 2>&1
		# The hero the autostart made is not the user's.
		[ \"\$had_save\" != 0 ] || rm -f \$d/single_*.sv
		rm -f \$d/devilutionx_*.log
	" || true
	scp -q "${ssh_opts[@]}" "$frame:.local/share/DiabloVR/run.log" "$out/run.log" || die "no log came back"
	if [[ -n "$dump" ]]; then
		scp -q "${ssh_opts[@]}" "$frame:.local/share/DiabloVR/vr-dumps/eye.bmp" "$out/eye0.bmp" || echo "no eye picture came back" >&2
		scp -q "${ssh_opts[@]}" "$frame:.local/share/DiabloVR/vr-dumps/eye-right.bmp" "$out/eye1.bmp" 2> /dev/null || true
	fi
	remote "rm -rf $remote_data/vr-dumps"
	grep -E 'VR: (eye|display period|controllers|first frame|session|xrEndFrame|refresh)|ERROR' "$out/run.log" | cut -c1-200 | sort | uniq -c | sort -rn | head -20
	grep 'VR: perf' "$out/run.log" | tail -3 | cut -c1-200
	;;

steam)
	seconds=60
	settings=()
	while (($#)); do
		case "$1" in
		--seconds) seconds="${2:?}"; shift 2 ;;
		*)
			[[ "$1" =~ ^DEVILUTIONX_VR_[A-Z0-9_]+=[A-Za-z0-9._/,-]*$ ]] || die "bad setting $1 (the launcher only takes DEVILUTIONX_VR_ settings from a file)"
			settings+=("$1")
			shift
			;;
		esac
	done
	reason="$(guard)" || die "stopping: $reason"
	out="$root/build-frame/session"
	rm -rf "$out"
	mkdir -p "$out"
	# Steam starts the title with its registered arguments, so what this run adds travels in the
	# launcher's one-shot file, which it deletes before the game starts.
	remote "
		d=$remote_data
		ls \$d/single_*.sv > /dev/null 2>&1 && touch \$d/.had-save
		printf '%s\n' DEVILUTIONX_VR_AUTOSTART=1 ${settings[*]:-} > \$d/launch-once.env
		id=\$(python3 - <<'PY'
import glob, re
for f in glob.glob('/home/steamos/.local/share/Steam/userdata/*/config/shortcuts.vdf'):
    d = open(f, 'rb').read()
    for m in re.finditer(rb'\x02appid\x00(....)\x01[Aa]pp[Nn]ame\x00([^\x00]*)\x00', d, re.S):
        if m.group(2).decode(errors='replace').endswith('$title'):
            print((int.from_bytes(m.group(1), 'little') << 32) | 0x02000000)
PY
)
		[ -n \"\$id\" ] || { echo 'the title is not registered with Steam'; rm -f \$d/launch-once.env; exit 1; }
		printf 'steam://rungameid/%s\n' \"\$id\" > ~/.steam/steam.pipe
		sleep $seconds
		mine() { for p in \$(pgrep -x devilutionx); do [ \"\$(readlink /proc/\$p/cwd)\" = \"\$HOME/devkit-game/$title\" ] && echo \$p; done; }
		pid=\$(mine | head -1)
		[ -n \"\$pid\" ] && kill -TERM \$pid
		for i in 1 2 3 4 5 6 7 8 9 10; do [ -z \"\$(mine)\" ] && break; sleep 1; done
		[ -n \"\$(mine)\" ] && { echo 'the game ignored SIGTERM, killing it'; kill -KILL \$(mine); }
		if [ -e \$d/launch-once.env ]; then echo 'the title never started: launch-once.env was not used'; rm -f \$d/launch-once.env; fi
		[ -e \$d/.had-save ] || rm -f \$d/single_*.sv
		rm -f \$d/.had-save \$d/devilutionx_*.log
		[ -n \"\$pid\" ] && echo \"ran as pid \$pid\"
	" || true
	scp -q "${ssh_opts[@]}" "$frame:.local/share/DiabloVR/run.log" "$out/run.log" || die "no log came back"
	grep -E '\[launch\]|VR: (eye|display period|controllers|first frame|session|refresh|pacing)|ERROR|fdm_injection: (created|device)' "$out/run.log" | cut -c1-200 | sort | uniq -c | sort -rn | head -24
	grep 'VR: perf' "$out/run.log" | tail -2 | cut -c1-200
	;;

*)
	die "unknown command $cmd"
	;;
esac
