#!/bin/sh
# Builds the JSON fuzzer (libFuzzer + ASan + UBSan) and the regression test.
# Linux, clang >= 14 with compiler-rt (Ubuntu: libclang-rt-18-dev).
#   ./build.sh [outdir]      (default: ./out)
#   out/fuzz_json corpus/json -max_total_time=1200      a campaign
#   out/fuzz_json regress/*                             the found defects
#   out/test_json                                       the same, values checked
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
app=$here/../../launcher/app
inc="-I$here/../../third_party/sdl3/include -I$app"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fsanitize=unsigned-integer-overflow -fno-sanitize-recover=unsigned-integer-overflow"
src="$here/sdl_stubs.cpp $app/ot_update.cpp $app/ot_util.cpp"
${CXX:-clang++} -std=c++17 -g -O1 -fsanitize=fuzzer $san -fprofile-instr-generate -fcoverage-mapping \
    $inc "$here/fuzz_json.cpp" $src -o "$out/fuzz_json"
${CXX:-clang++} -std=c++17 -g -O1 $san $inc "$here/test_json.cpp" $src -o "$out/test_json"
