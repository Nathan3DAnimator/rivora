# The first run of a game packaged a moment ago (ctest game_first_run, notas-dev/jogo.md). The driver keeps its own
# shader cache per executable, and a packaged game is an executable it has never seen: without the engine's pipeline
# cache (vkctx.c) its first frame took 4.6 s compiling every shader (RTX 3060). Here the game gets a name nobody ran
# before, the editor packages it (and fills a cache of the test's own, as it fills the user's), and the game's first run
# must show its first frame quickly and hold the frame rate. A copy with an empty cache is run too: the cold reference.
#   cmake -DEDITOR=<realengine.exe> -DOUT=<folder> -DMAX_START_MS=<ms> -DMIN_FPS=<fps> -P test_first_run.cmake
string(TIMESTAMP stamp "%Y%m%d%H%M%S")
string(RANDOM LENGTH 6 ALPHABET "abcdefghijklmnopqrstuvwxyz" salt)
set(name "First${stamp}${salt}")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}" "${OUT}/proj")
set(ENV{REALENGINE_PIPELINE_CACHE} "${OUT}/pipelines.bin")
# (from a blank project: the build's own Content holds what the other tests made, 89 meshes, 17 MB)
execute_process(COMMAND "${EDITOR}" --new-project "${OUT}/proj/Blank" --template 0 --save "${OUT}/${name}.rlevel" --package "${OUT}/pkg"
                --bench 1 RESULT_VARIABLE r)
if(NOT r EQUAL 0 OR NOT EXISTS "${OUT}/pkg/${name}/${name}.exe")
    message(FATAL_ERROR "packaging ${name} failed (${r})")
endif()
if(NOT EXISTS "${OUT}/pipelines.bin")
    message(FATAL_ERROR "the editor left no pipeline cache")
endif()

# the game's first run: warm from the editor's cache
execute_process(COMMAND "${OUT}/pkg/${name}/${name}.exe" --validation --bench 2 --min-fps ${MIN_FPS} --max-start-ms ${MAX_START_MS}
                RESULT_VARIABLE r OUTPUT_VARIABLE out ERROR_VARIABLE out)
string(REPLACE ";" "," out "${out}") # (a ; would split CMake's list)
string(REGEX MATCHALL "\\[RealEngine\\] (Start|Measure|First|FAIL)[^\n]*" lines "${out}")
foreach(l IN LISTS lines)
    message(STATUS "first run: ${l}")
endforeach()

# the cold reference: another executable nobody ran, an empty cache (only reported)
file(COPY_FILE "${OUT}/pkg/${name}/${name}.exe" "${OUT}/pkg/${name}/${name}Cold.exe")
set(ENV{REALENGINE_PIPELINE_CACHE} "${OUT}/cold.bin")
execute_process(COMMAND "${OUT}/pkg/${name}/${name}Cold.exe" --bench 1 OUTPUT_VARIABLE cold ERROR_VARIABLE cold)
string(REGEX MATCH "\\[RealEngine\\] Start[^\n]*" cold_line "${cold}")
message(STATUS "cold reference: ${cold_line}")

if(NOT r EQUAL 0)
    message(FATAL_ERROR "the first run of a freshly packaged game failed (${r})")
endif()
