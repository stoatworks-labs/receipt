#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# bash, not zsh: zsh has no PIPESTATUS, and the closed-stdout check below
# reads the harness's own exit status out of a pipeline.
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out -- the exact strings the
#                 plugin hands the driver (`rctest --dump-shaders`). Then a
#                 grep for every GLSL 4.10 reserved word used as an
#                 identifier, because Apple's compiler and glslc accept some
#                 (`packed`) that Mesa refuses.
#   offline       the control laws, the model's promises, the names a host
#                 shows, and the cue sheet's kinds. No GL.
#   physics       every rendered check, at TWO rasters: 320x180, which is what
#                 CI renders at, and 1280x720. Each drives the real plugin and
#                 reads the printer back out of the picture:
#                   --dither    the dot density on flat greys is the tone,
#                               within each dither's bound
#                   --history   a vertical line thickens (and runs on) by
#                               what the heat carry predicts, in dots; not
#                               under full history control
#                   --budget    a block over the supply's budget prints at
#                               the predicted sag; the banding sits on the
#                               block boundaries
#                   --slip      a forced repeat or stall moves exactly the
#                               stated rows
#                   --grid      every dot lands on its whole-pixel cell
#                   --printing  Printing scrolls in elapsed time at 60, 50
#                               and 30 fps alike, and tears at the length
#                   --resize    an output resize mid-print keeps every row
#                   --alpha     opaque paper, transparent off it, Mix fades
#                               both; a clear clip prints blank
#                   --negative  every model check FAILS on a perturbed
#                               printer
#                 and again on Apple's SOFTWARE renderer (CI's), which is not
#                 bit-repeatable.
#   pipe          the fleet's frame contract, plus what a closed stdout (exit
#                 1, not SIGPIPE's 141), an unknown cue, a failed render, a
#                 stepped option, a stepped integer and a ramped slider do.
#   sweep         does every control change the picture. A GLSL uniform whose
#                 name does not match the C++ is ignored without a word.
#   bench         the render cost by stage, for the record.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

RCTEST="$BUILD/rctest"

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler (tools/check-shaders.sh, which CI
# runs too), then the reserved words.
#---------------------------------------------------------------------------
step "shaders"
if out=$(tools/check-shaders.sh "$RCTEST" 2>&1); then
	pass "$( printf '%s\n' "$out" | tail -1 | sed 's/^ *//' )"
else
	fail "a shader does not compile, or the dump is stale"
	printf '%s\n' "$out" | sed 's/^/      /'
fi
dir="$( mktemp -d )"
"$RCTEST" --dump-shaders "$dir" >/dev/null
# The GLSL 4.10 reserved words (s3.6 of the spec: keywords reserved for future
# use, plus the ones the fleet has been bitten by) as identifiers. Apple's
# compiler and glslc accept `packed`; Mesa's llvmpipe, which the Arena gate
# runs on, does not. Matched as whole words, on lines that are not comments.
reserved='common|partition|active|asm|class|union|enum|typedef|template|this|packed|resource|goto|inline|noinline|public|static|extern|external|interface|long|short|half|fixed|unsigned|superp|input|output|hvec2|hvec3|hvec4|fvec2|fvec3|fvec4|sampler3DRect|filter|sizeof|cast|namespace|using|row_major|patch|sample|subroutine|flat|near|far'
hits=$( cat "$dir"/*.vert "$dir"/*.frag | sed 's|//.*||' | grep -nwE "($reserved)" || true )
if [ -z "$hits" ]; then
	pass "no GLSL 4.10 reserved word (nor near/far, MSVC's macros) used as an identifier"
else
	fail "a GLSL 4.10 reserved word appears in a shader:"
	printf '%s\n' "$hits" | sed 's/^/      /'
fi
rm -rf "$dir"

step "offline (no GL)"
if out=$("$RCTEST" --offline 2>&1); then
	pass "rctest --offline: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
else
	fail "rctest --offline"
	printf '%s\n' "$out" | sed 's/^/      /'
fi

CHECKS="dither history budget slip grid printing resize alpha negative"
for size in 320x180 1280x720; do
	step "physics at $size"
	for check in $CHECKS; do
		if out=$("$RCTEST" --$check --size $size 2>&1); then
			pass "rctest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "rctest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

# The whole physics list again on Apple's SOFTWARE renderer, which is what
# GitHub's macOS runners have. It is not repeatable at the last bit (repousse's
# resize check failed CI by one ulp), so a check that asserts exactness on this
# Mac's GPU is found here before CI finds it.
step "physics at 320x180 on the software renderer (CI's)"
for check in $CHECKS; do
	if out=$(RCTEST_RENDERER=software "$RCTEST" --$check --size 320x180 2>&1); then
		pass "rctest --$check (software): $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "rctest --$check at 320x180 on the software renderer -- run: RCTEST_RENDERER=software $RCTEST --$check --size 320x180"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be exactly
# two frames out and a clean exit -- a partial frame is the end of the stream,
# never a frame -- and a cue naming no parameter must be refused rather than
# silently doing nothing to the wall.
#---------------------------------------------------------------------------
step "pipe"
W=64; H=36
frame=$(( W * H * 4 ))
raw=$( mktemp ); cues=$( mktemp ); out=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
got=$( "$RCTEST" --pipe --size ${W}x${H} < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever rctest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$RCTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi
# A reader that hangs up early (`| head -c 1`, ffmpeg dying) must end the run
# with exit 1 and a message, not SIGPIPE's silent 141: rctest ignores SIGPIPE,
# so the closed stdout is a failed write it sees.
head -c $(( frame * 20 )) /dev/zero > "$raw"
"$RCTEST" --pipe --size ${W}x${H} < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout (| head -c 1) ends the run with exit 1, not SIGPIPE's 141"
else
	fail "a closed stdout gave exit $status, not 1"
fi
# A failed render ends the run with exit 1 too.
"$RCTEST" --pipe --size ${W}x${H} --fail-render-at 3 < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 1 ]; then
	pass "a failed render ends the run with exit 1"
else
	fail "a failed render gave exit $status, not 1"
fi
# An option and an integer STEP between cues. Static has no clock, so each
# frame of a scripted run is compared with the same frame of a run that held
# one value throughout: A at frame 0 and B at frame 4 must give frames 0-3 of
# the A run and 4-5 of the B run. A ramp would put a value between on frame 2
# and match neither.
python3 -c "import sys; sys.stdout.buffer.write(bytes([128,128,128,255]) * ($W * $H * 6))" > "$raw"
steps() {  # steps NAME A B : scripted A->B at frame 4 against constant A and constant B
	printf '0 %s %s\n4 %s %s\n' "$1" "$2" "$1" "$3" > "$cues"
	"$RCTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
	"$RCTEST" --pipe --size ${W}x${H} --set "$1=$2" < "$raw" > "$out.a" 2>/dev/null
	"$RCTEST" --pipe --size ${W}x${H} --set "$1=$3" < "$raw" > "$out.b" 2>/dev/null
	python3 - "$out" "$out.a" "$out.b" $frame <<'PY'
import sys
s, a, b = (open(p, 'rb').read() for p in sys.argv[1:4]); n = int(sys.argv[4])
f = lambda d, i: d[i * n:(i + 1) * n]
ok = len(s) == 6 * n and all(f(s, i) == f(a, i) for i in range(4)) and all(f(s, i) == f(b, i) for i in (4, 5)) and f(a, 2) != f(b, 2)
sys.exit(0 if ok else 1)
PY
}
if steps "Paper Tint" 0 5; then
	pass "an option cue steps (Paper Tint: frames 0-3 White, 4-5 Green), no stock between"
else
	fail "an option cue did not step -- see loadScript/valueAt in tools/rctest/main.cpp"
fi
if steps "Strobe Blocks" 1 8; then
	pass "an integer cue steps (Strobe Blocks: frames 0-3 one block, 4-5 eight)"
else
	fail "an integer cue did not step"
fi
# A slider RAMPS: Mix 0 at frame 0 and 1 at frame 4 puts 0.5 on frame 2.
printf '0 Mix 0\n4 Mix 1\n' > "$cues"
"$RCTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
"$RCTEST" --pipe --size ${W}x${H} --set "Mix=0.5" < "$raw" > "$out.a" 2>/dev/null
if python3 - "$out" "$out.a" $frame <<'PY'
import sys
s, a = (open(p, 'rb').read() for p in sys.argv[1:3]); n = int(sys.argv[3])
sys.exit(0 if len(s) == 6 * n and s[2 * n:3 * n] == a[2 * n:3 * n] else 1)
PY
then
	pass "a slider cue ramps (Mix 0 -> 1 over frames 0-4 is 0.5 on frame 2)"
else
	fail "a slider cue did not ramp"
fi
rm -f "$out.a" "$out.b"
rm -f "$raw" "$cues" "$out"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$RCTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$RCTEST" --bench --frames 60 2>&1 | sed -n '5,17p' | sed 's/^/   /'

BUNDLE="$BUILD/Receipt.bundle"
BIN="$BUNDLE/Contents/MacOS/Receipt"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.receipt" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi
	about=$(sed -n 's/.*versionFallback = "v\([^"]*\)".*/\1/p' source/StoatworksAbout.h)
	if [ "$version" = "0.1.0" ] && [ "$about" = "$version" ]; then
		pass "CFBundleVersion $version, and StoatworksAbout.h says v$about"
	else
		fail "CFBundleVersion is '$version' and StoatworksAbout.h says '$about' -- they must both be the project() VERSION"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Receipt.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Receipt" "id:          RC01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
