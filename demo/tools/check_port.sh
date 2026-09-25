#!/usr/bin/env bash
#
# The browser demo's CPU half (demo/printer.js) against the plugin's own C++.
#
#   demo/tools/check_port.sh
#
# check_port.mjs compiles refprint.cpp against source/Printer.cpp and
# source/Controls.cpp (unchanged) and against Receipt.h's and Receipt.cpp's own
# text, cut out at run time, then compares fired bits, densities, heat, the
# feed, the uploads and the uniforms exactly. It says what it covers and what
# it cannot. Called from tools/verify.sh; exits 3 (skip) without node or a C++
# compiler.
#
set -uo pipefail
cd "$(dirname "$0")/../.."

command -v node >/dev/null 2>&1 || { echo "skipped: node not installed"; exit 3; }
command -v c++ >/dev/null 2>&1 || { echo "skipped: no C++ compiler"; exit 3; }
node demo/tools/check_port.mjs
