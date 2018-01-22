#!/bin/bash -eu

: "${SRC:?SRC must point to the repository root}"
: "${OUT:?OUT must point to the output directory}"

CXX="${CXX:-clang++}"
JOBS="${WAVECAIRN_BUILD_JOBS:-4}"
mkdir -p "$OUT/obj"

pids=()
wait_for_slot() {
  while [ "${#pids[@]}" -ge "$JOBS" ]; do
    wait "${pids[0]}"
    pids=("${pids[@]:1}")
  done
}

objects=()
for source in "$SRC"/src/*.cpp; do
  name="$(basename "${source%.cpp}")"
  object="$OUT/obj/${name}.o"
  objects+=("$object")
  wait_for_slot
  "$CXX" $CXXFLAGS -std=c++20 -I"$SRC/include" -c "$source" -o "$object" &
  pids+=("$!")
done
for pid in "${pids[@]}"; do
  wait "$pid"
done

# Link targets in a small parallel batch to avoid multiplying peak memory use.
pids=()
LINK_JOBS=2
for harness in "$SRC"/fuzz/*_fuzzer.cc; do
  target="$(basename "${harness%.cc}")"
  while [ "${#pids[@]}" -ge "$LINK_JOBS" ]; do
    wait "${pids[0]}"
    pids=("${pids[@]:1}")
  done
  "$CXX" $CXXFLAGS -std=c++20 -I"$SRC/include" \
    "$harness" "${objects[@]}" $LIB_FUZZING_ENGINE -o "$OUT/$target" &
  pids+=("$!")
done
for pid in "${pids[@]}"; do
  wait "$pid"
done
