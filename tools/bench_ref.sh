#!/bin/sh
# The optimization's reference measure (notas-dev/render.md "Otimização geral"): the same scenes before and after each
# phase, Epic at the screen's size, in the background (REALENGINE_BACKGROUND=1): the GPU minimum and each pass's ms.
#   sh tools/bench_ref.sh [label]          (from the repository; the build in build/Release)
# The haunted house is measured when its project exists (--scene haunted made it next to the other projects).
cd "$(dirname "$0")/../build/Release" || exit 1
L=${1:-run}
HOUSE="${REALENGINE_HAUNTED_PROJECT:-$HOME/Documents/Real Engine Projects/CasaAssombrada/CasaAssombrada.rproject}"
run() { # name, args...
    n=$1; shift
    out=$(REALENGINE_BACKGROUND=1 REALENGINE_DEMO_AUTOPLAY=1 timeout 600 ./realengine.exe "$@" --bench 6 2>&1)
    gpu=$(echo "$out" | grep "GPU minimum" | sed 's/.*GPU minimum: \([0-9.]*\) ms.*/\1/')
    fps=$(echo "$out" | grep "Measure:" | sed 's/.*Measure: \([0-9]*\) fps.*/\1/')
    passes=$(echo "$out" | grep "Render: GPU" | sed 's/.*Render: GPU \(.*\) ms; CPU.*/\1/')
    printf '%s | %-8s | %5s fps | GPU min %6s ms | %s\n' "$L" "$n" "$fps" "$gpu" "$passes"
}
run heavy --project ../test_projects/Heavy/Heavy.rproject --scene heavy --scalability 3
[ -f "$HOUSE" ] && run casa --project "$HOUSE" --play --scalability 3
run rpg --project ../test_projects/DemoRPG/DemoRPG.rproject --play --scalability 3
run house --scene house --scalability 3
