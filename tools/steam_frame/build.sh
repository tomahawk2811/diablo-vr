#!/usr/bin/env bash
# Builds the VR mode for the Steam Frame (Linux ARM64) inside the pinned environment from
# docker/linux.Dockerfile. The target architecture is the Docker host's: arm64 on Apple Silicon.
#
#   tools/steam_frame/build.sh [step...]
#
# Steps (default: image dxvk game package):
#   image    build the Docker image
#   dxvk     fetch dxvk-native at the pinned commit, apply patches/dxvk-*.patch and build its
#            d3d11 and dxgi into build-frame/dxvk-install
#   probe    build the OpenXR probe (tools/steam_frame/probe) into build-frame/probe
#   game     configure and build the game into build-frame/game. VR=ON also turns the VR mode on
#            (-DDEVILUTIONX_VR=ON); the default is the flat game
#   package  stage build-frame/package/DiabloVR: the game, its own SDL2, the assets and the
#            launcher (VR=ON adds the DXVK libraries and vrassets/). Writes BUILD-METADATA.json
#            and fails if the ABI check or the package allowlist does
#   smoke    not in the default list. Runs the package under Xvfb with the user's DIABDAT.MPQ
#            mounted read-only (DIABDAT=/path/to/DIABDAT.MPQ), waits for the
#            main menu, saves build-frame/smoke/menu.png and checks that SIGTERM ends the game
#   render   not in the default list. Needs a VR=ON package. Draws real levels with the VR mode's
#            headset-free renderer on lavapipe (software Vulkan) and leaves the pictures in
#            build-frame/render/level<N>/. LEVELS="0 2" picks them: 0 is the town.
#            SETTINGS="DEVILUTIONX_VR_MSAA=1" passes settings of the VR mode to the run
#
# Set VR=ON for the game and package steps alike. The default is OFF.
#
# Everything is built under build-frame/, which git ignores.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
image="${DIABLO_VR_IMAGE:-diablo-vr-linux}"
out=build-frame
dxvk_tag=v3.1.1
dxvk_commit=b1a1c99ab52b687cf950d62c88bc2fa316b41663

in_container() {
	docker run --rm -v "$repo":/src -w /src "$image" bash -c "set -euo pipefail; $1"
}

step_image() {
	docker build -t "$image" -f "$repo/tools/steam_frame/docker/linux.Dockerfile" "$repo/tools/steam_frame/docker"
}

step_dxvk() {
	local src="$repo/$out/dxvk-src"
	if [ ! -d "$src/.git" ]; then
		git clone -q --depth 1 --branch "$dxvk_tag" --recurse-submodules --shallow-submodules https://github.com/doitsujin/dxvk.git "$src"
	fi
	[ "$(git -C "$src" rev-parse HEAD)" = "$dxvk_commit" ] || { echo "dxvk is not at $dxvk_commit" >&2; exit 1; }
	# Back to the pinned tree, then the patches, so a changed patch is picked up.
	git -C "$src" reset -q --hard
	git -C "$src" clean -q -fd -- src include
	for patch in "$repo"/tools/steam_frame/patches/dxvk-*.patch; do
		git -C "$src" apply "$patch"
	done
	in_container "
		cd $out/dxvk-src
		# libdxvk_d3d11 needs libdxvk_dxgi, and has to find it beside itself in the package.
		[ -f /src/$out/dxvk-build/build.ninja ] || LDFLAGS='-Wl,-rpath,\$ORIGIN' meson setup --buildtype release --strip --prefix /src/$out/dxvk-install --libdir lib \
			-Denable_dxgi=true -Denable_d3d11=true -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false \
			-Dnative_sdl2=enabled -Dnative_sdl3=disabled -Dnative_glfw=disabled -Dbuild_id=false \
			--force-fallback-for=libdisplay-info /src/$out/dxvk-build
		ninja -C /src/$out/dxvk-build install > /dev/null
		rm -f subprojects/.wraplock
		ls -l /src/$out/dxvk-install/lib
	"
}

step_probe() {
	in_container "
		cmake -S tools/steam_frame/probe -B $out/probe -GNinja -DCMAKE_BUILD_TYPE=Release -DDXVK_PREFIX=/src/$out/dxvk-install
		cmake --build $out/probe
	"
}

# The worktree's .git can be a file that points outside the bind mount, so git does not work in
# the container. The revision is read here and goes in as GIT_COMMIT_HASH, which the root
# CMakeLists uses for the version suffix of a "-dev" VERSION. A tree with changes gets "-dirty".
# (Without a hash CMake just leaves the suffix out.)
git_rev() {
	local rev
	rev="$(git -C "$repo" rev-parse --short HEAD)" || { echo "cannot read the git revision" >&2; return 1; }
	if [ -n "$(git -C "$repo" status --porcelain --untracked-files=normal)" ]; then
		rev="$rev-dirty"
	fi
	printf '%s\n' "$rev"
}

step_game() {
	# shellcheck disable=SC2016
	local rev rpath='$ORIGIN/lib'
	rev="$(git_rev)"
	mkdir -p "$repo/$out"
	python3 "$repo/tools/steam_frame/make_build_metadata.py" source "$repo/$out/game-source.json"
	# SDL2 is built shared: the VR renderer's DXVK looks up libSDL2-2.0.so.0 by soname at run time
	# and has to find the game's own copy. Everything else is built from source and linked static.
	# The install RPATH is used at build time, so the binary finds lib/ beside it once packaged.
	in_container "
		cmake -S . -B $out/game -GNinja -DCMAKE_BUILD_TYPE=Release \\
			-DGIT_COMMIT_HASH=$rev \\
			-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_INSTALL_RPATH='$rpath' \\
			-DDEVILUTIONX_VR=${VR:-OFF} \\
			-DBUILD_TESTING=OFF \\
			-DDISABLE_ZERO_TIER=ON \\
			-DDISCORD_INTEGRATION=OFF \\
			-DDEVILUTIONX_STATIC_SDL2=OFF \\
			-DDEVILUTIONX_SYSTEM_SDL2=OFF \\
			-DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF \\
			-DDEVILUTIONX_SYSTEM_SDL_AUDIOLIB=OFF \\
			-DDEVILUTIONX_SYSTEM_ZLIB=OFF \\
			-DDEVILUTIONX_SYSTEM_BZIP2=OFF \\
			-DDEVILUTIONX_SYSTEM_LUA=OFF \\
			-DDEVILUTIONX_SYSTEM_LIBSODIUM=OFF \\
			-DDEVILUTIONX_SYSTEM_SHEENBIDI=OFF \\
			-DDEVILUTIONX_SYSTEM_MAGIC_ENUM=OFF
		cmake --build $out/game --parallel
	"
}

step_package() {
	local pkg="$out/package/DiabloVR" vr_args=() dxvk="$out/dxvk-install/lib"
	[ -x "$repo/$out/game/devilutionx" ] || { echo "no $out/game/devilutionx: run the game step first" >&2; exit 1; }
	[ -f "$repo/$out/game-source.json" ] || { echo "no $out/game-source.json: run the game step first" >&2; exit 1; }
	rm -rf "${repo:?}/$pkg"
	# Staged in the container so strip is the arm64 one; the modes are set here so they do not
	# depend on the umask of whoever built.
	in_container "
		pkg=$pkg
		mkdir -p \$pkg/lib
		cp $out/game/devilutionx \$pkg/devilutionx
		# The shared SDL2 is a chain of symlinks to libSDL2-2.0.so.0.NNNN.N; the package needs the file.
		cp -L $out/game/_deps/sdl2-build/libSDL2-2.0.so.0 \$pkg/lib/libSDL2-2.0.so.0
		strip --strip-unneeded \$pkg/devilutionx \$pkg/lib/libSDL2-2.0.so.0
		# The game looks for assets/ (or devilutionx.mpq) beside the executable. Without smpq
		# the build leaves them loose in assets/, as the Windows build does.
		if [ -f $out/game/devilutionx.mpq ]; then cp $out/game/devilutionx.mpq \$pkg/; else cp -R $out/game/assets \$pkg/assets; fi
		# The Hellfire mod is found as mods/hf/ in the working directory, which the launcher sets.
		cp -R $out/game/mods \$pkg/mods
		cp tools/steam_frame/LAUNCH-STEAM-FRAME.sh \$pkg/
		if [ '${VR:-OFF}' = ON ]; then
			for lib in libdxvk_d3d11.so.0 libdxvk_dxgi.so.0; do
				if [ -f $dxvk/\$lib ]; then cp -L $dxvk/\$lib \$pkg/lib/\$lib; else echo \"warning: $dxvk/\$lib is missing, run the dxvk step\" >&2; fi
			done
			rsync -a --exclude '.DS_Store' --exclude '._*' vrassets/ \$pkg/vrassets/
		fi
		find \$pkg -type d -exec chmod 755 {} +
		find \$pkg -type f -exec chmod 644 {} +
		chmod 755 \$pkg/devilutionx \$pkg/LAUNCH-STEAM-FRAME.sh
		# The loader has to find SDL2 through the RUNPATH alone, from a clean environment.
		cd \$pkg
		env -u LD_LIBRARY_PATH ldd ./devilutionx | grep -E 'SDL2|not found' || true
		! env -u LD_LIBRARY_PATH ldd ./devilutionx | grep -q 'not found'
		env -u LD_LIBRARY_PATH ./devilutionx --version
	"
	if [ "${VR:-OFF}" = ON ]; then vr_args=(--vr); fi
	local tools
	tools="$(in_container "
		echo gcc=\$(g++ --version | head -1)
		echo cmake=\$(cmake --version | head -1)
		echo ninja=\$(ninja --version)
		echo ld=\$(ld --version | head -1)
		echo image=$image
	")"
	local tool_args=()
	while IFS= read -r line; do tool_args+=(--tool "$line"); done <<< "$tools"
	python3 "$repo/tools/steam_frame/make_build_metadata.py" package "$repo/$pkg" --source "$repo/$out/game-source.json" ${vr_args[@]+"${vr_args[@]}"} "${tool_args[@]}"
	python3 "$repo/tools/steam_frame/check_linux_abi.py" "$repo/$pkg"
	python3 "$repo/tools/steam_frame/check_package_allowlist.py" "$repo/$pkg" ${vr_args[@]+"${vr_args[@]}"}
	du -sh "$repo/$pkg"
	(
		cd "$repo/$pkg"
		find . -type f -not -path './assets/*' -not -path './vrassets/*' -exec ls -l {} + | awk '{print $5, $9}' | sort -k2
		echo "assets/: $(find assets -type f 2> /dev/null | wc -l | tr -d ' ') files; vrassets/: $(find vrassets -type f 2> /dev/null | wc -l | tr -d ' ') files"
	)
}

# Runs the package under Xvfb, with the user's own DIABDAT.MPQ mounted read-only (it is never
# copied into the repo), and leaves build-frame/smoke/menu.png.
step_smoke() {
	local data="${DIABDAT:-}"
	[ -f "$data" ] || { echo "set DIABDAT=/path/to/your/DIABDAT.MPQ (it is mounted read-only, never copied)" >&2; exit 1; }
	[ -x "$repo/$out/package/DiabloVR/devilutionx" ] || { echo "no package: run the package step first" >&2; exit 1; }
	docker run --rm -v "$repo":/src -v "$data":/diabdat/DIABDAT.MPQ:ro -w /src "$image" bash /src/tools/steam_frame/smoke-in-container.sh
}

# Draws real levels with the VR mode's headset-free renderer on software Vulkan and leaves the
# pictures in build-frame/render/. Needs a package built with VR=ON. LEVELS="0 2" picks the levels.
step_render() {
	local data="${DIABDAT:-}"
	[ -f "$data" ] || { echo "set DIABDAT=/path/to/your/DIABDAT.MPQ (it is mounted read-only, never copied)" >&2; exit 1; }
	[ -x "$repo/$out/package/DiabloVR/devilutionx" ] || { echo "no package: run the package step first" >&2; exit 1; }
	docker run --rm -v "$repo":/src -v "$data":/diabdat/DIABDAT.MPQ:ro -w /src \
		-e LEVELS="${LEVELS:-0 2}" -e RENDER_TIMEOUT="${RENDER_TIMEOUT:-600}" -e SETTINGS="${SETTINGS:-}" "$image" bash /src/tools/steam_frame/render-in-container.sh
}

[ $# -gt 0 ] || set -- image dxvk game package
for step in "$@"; do
	case "$step" in
	image | dxvk | probe | game | package | smoke | render) "step_$step" ;;
	*)
		echo "unknown step: $step" >&2
		exit 1
		;;
	esac
done
