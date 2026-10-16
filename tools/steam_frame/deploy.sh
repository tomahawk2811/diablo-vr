#!/usr/bin/env bash
# Copies the package to a Steam Frame and registers it as a devkit title, which then shows up in
# the Frame's Steam library.
#
#   tools/steam_frame/deploy.sh [--package DIR] [--data FILE] [--launch-args "A B"]
#                               [--no-register] [--logs]
#
#   --package DIR    the staged package (default build-frame/package/DiabloVR, made by
#                    `tools/steam_frame/build.sh game package`)
#   --data FILE      also copy the user's own DIABDAT.MPQ to ~/.local/share/DiabloVR/ on the
#                    Frame, where the launcher keeps the game's data. Skipped when the file there
#                    already has the same size and SHA-256. Never put it in git or in the package.
#   --launch-args A  extra arguments for the registered title (a VR package gets --vr anyway)
#   --no-register    only copy files
#   --logs           print the last run's log (run.log) and exit
#
# FRAME_HOST (default frame.local) is the Frame's host name; the login is steamos@. TITLE
# (default DiabloVR, alphanumeric) names the title and its folder under ~/devkit-game. It is a
# native Linux ARM64 title: compat tool SteamLinuxRuntime_4-arm64, steam_play=0. Start it from the
# Steam library, since a binary started over SSH sees no controllers.
#
# Every step is checked and the script stops at the first one that fails.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
frame="steamos@${FRAME_HOST:-frame.local}"
title="${TITLE:-DiabloVR}"
remote_dir="devkit-game/$title"
data_dir=".local/share/DiabloVR"
package="$root/build-frame/package/DiabloVR"
data_file=""
register=1
logs=0
launch_args=""

die() {
	echo "$*" >&2
	exit 1
}

[[ "$title" =~ ^[A-Za-z0-9]+$ ]] || die "TITLE must be alphanumeric"

while (($#)); do
	case "$1" in
		--package) package="${2:?--package needs a folder}"; shift 2 ;;
		--data) data_file="${2:?--data needs a file}"; shift 2 ;;
		--launch-args) launch_args="${2:?--launch-args needs a value}"; shift 2 ;;
		--no-register) register=0; shift ;;
		--logs) logs=1; shift ;;
		*) die "unknown option $1" ;;
	esac
done

ssh_opts=(-o ConnectTimeout=5 -o BatchMode=yes)
# macOS rsync is 2.6: no --info. No -E either, so no AppleDouble files. -rlpt keeps the
# executable bits and leaves owners alone.
rsync_opts=(-rlpt --partial --exclude '._*' --exclude .DS_Store -e "ssh ${ssh_opts[*]}")

remote() {
	# shellcheck disable=SC2029
	ssh "${ssh_opts[@]}" "$frame" "$@"
}

if ((logs)); then
	exec ssh "${ssh_opts[@]}" "$frame" "cat ~/$data_dir/run.log"
fi

[[ -x "$package/devilutionx" && -x "$package/LAUNCH-STEAM-FRAME.sh" && -f "$package/BUILD-METADATA.json" ]] ||
	die "$package is not a package (run tools/steam_frame/build.sh game package)"

# The checks that the package step ran, again, plus: do the files still match their hashes?
vr_flag=()
[[ -d "$package/vrassets" ]] && vr_flag=(--vr)
python3 "$root/tools/steam_frame/check_package_allowlist.py" "$package" ${vr_flag[@]+"${vr_flag[@]}"} ||
	die "the package failed the allowlist check"

sums="$(python3 - "$package" <<'EOF'
import json, os, sys, hashlib
root = sys.argv[1]
meta = json.load(open(os.path.join(root, "BUILD-METADATA.json")))
for rel, info in meta["files"].items():
    h = hashlib.sha256()
    with open(os.path.join(root, rel), "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    if h.hexdigest() != info["sha256"]:
        sys.exit("%s does not match BUILD-METADATA.json (rebuild the package)" % rel)
    print("%s  %s" % (info["sha256"], rel))
EOF
)" || die "the package does not match its BUILD-METADATA.json"
[[ -n "$sums" ]] || die "BUILD-METADATA.json lists no files"

if [[ -n "$data_file" ]]; then
	[[ -f "$data_file" ]] || die "$data_file is not a file"
	[[ "$(head -c 3 "$data_file")" == "MPQ" ]] || die "$data_file is not an MPQ archive"
fi

remote true 2> /dev/null || die "The Frame is not reachable at $frame (USB cable, charge, ssh key)."

# Never under a running game: it would be reading the files being replaced.
# The launcher starts it as ./devilutionx from its folder, so it is found by that folder.
if remote "for p in \$(pgrep -x devilutionx); do [ \"\$(readlink /proc/\$p/cwd)\" = \"\$HOME/$remote_dir\" ] && exit 0; done; exit 1"; then
	die "$title is running on the Frame. Quit it first."
fi

echo "== copying the package to $frame:$remote_dir"
remote "mkdir -p ~/$remote_dir" || die "could not create ~/$remote_dir on the Frame"
rsync "${rsync_opts[@]}" --delete "$package/" "$frame:$remote_dir/" || die "rsync of the package failed"

echo "== checking the installed files against the build"
printf '%s\n' "$sums" | remote "cd ~/$remote_dir && sha256sum --quiet -c -" || die "installed files do not match the build"
# Nothing extra and nothing missing: the same file list, plus BUILD-METADATA.json itself.
want="$({ printf '%s\n' "$sums" | sed 's/^[0-9a-f]*  /.\//'; echo ./BUILD-METADATA.json; } | LC_ALL=C sort)"
have="$(remote "cd ~/$remote_dir && find . -type f | LC_ALL=C sort")" || die "could not list the installed files"
[[ "$want" == "$have" ]] || die "the installed file list differs from the build"
remote "cd ~/$remote_dir && test -x devilutionx && test -x LAUNCH-STEAM-FRAME.sh" ||
	die "devilutionx or LAUNCH-STEAM-FRAME.sh is not executable on the Frame"
echo "ok: $(printf '%s\n' "$sums" | wc -l | tr -d ' ') files match"

if [[ -n "$data_file" ]]; then
	echo "== game data"
	want_size="$(wc -c < "$data_file" | tr -d ' ')"
	want_sha="$(shasum -a 256 "$data_file" | cut -d' ' -f1)"
	remote "mkdir -p ~/$data_dir" || die "could not create ~/$data_dir on the Frame"
	have="$(remote "f=\$HOME/$data_dir/DIABDAT.MPQ; if [ -f \"\$f\" ]; then echo \"\$(stat -c %s \"\$f\") \$(sha256sum \"\$f\" | cut -d' ' -f1)\"; fi")" ||
		die "could not look at the data file on the Frame"
	if [[ "$have" == "$want_size $want_sha" ]]; then
		echo "DIABDAT.MPQ is already on the Frame with the same size and SHA-256, skipping"
	else
		rsync "${rsync_opts[@]}" "$data_file" "$frame:$data_dir/DIABDAT.MPQ" || die "rsync of the data file failed"
		have="$(remote "f=\$HOME/$data_dir/DIABDAT.MPQ; echo \"\$(stat -c %s \"\$f\") \$(sha256sum \"\$f\" | cut -d' ' -f1)\"")" ||
			die "could not check the data file on the Frame"
		[[ "$have" == "$want_size $want_sha" ]] || die "DIABDAT.MPQ on the Frame does not match ($have)"
		echo "copied DIABDAT.MPQ ($want_size bytes), SHA-256 matches"
	fi
fi

# The Frame's default settings of the VR mode, once. A file that is already there is the user's.
if [[ -d "$package/vrassets" ]]; then
	echo "== settings"
	remote "mkdir -p ~/$data_dir" || die "could not create ~/$data_dir on the Frame"
	if remote "test -e ~/$data_dir/vr-settings.env"; then
		echo "vr-settings.env is already on the Frame, left alone"
	else
		rsync "${rsync_opts[@]}" "$root/tools/steam_frame/vr-settings.env" "$frame:$data_dir/vr-settings.env" || die "rsync of vr-settings.env failed"
		echo "copied the default vr-settings.env"
	fi
fi

if ((register)); then
	echo "== registering $title"
	# The devkit service's own shortcut tool, as the other native Frame ports use it.
	argv='"LAUNCH-STEAM-FRAME.sh"'
	# A VR package always starts in VR: the shortcut is rewritten on every deploy, and one
	# without --vr starts the flat game.
	if ((${#vr_flag[@]})) && [[ " $launch_args " != *" --vr "* ]]; then
		launch_args="--vr${launch_args:+ $launch_args}"
	fi
	for a in $launch_args; do
		[[ "$a" =~ ^[A-Za-z0-9._=-]+$ ]] || die "bad launch argument $a"
		argv+=', "'$a'"'
	done
	parms='{"gameid": "'$title'", "directory": "/home/steamos/'$remote_dir'", "force_appid": "", "argv": ['$argv'], "env": {}, "settings": {"steam_play": "0", "compat_tool": "SteamLinuxRuntime_4-arm64"}, "lepton_args": ""}'
	remote "cd ~/devkit-utils && python3 steam-client-create-shortcut --parms '$parms'" || die "registering the title failed"
	echo "Registered $title. Start it from the Frame's Steam library; read the log with: $0 --logs"
fi
