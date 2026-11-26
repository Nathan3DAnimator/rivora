# (2026-11-25, bug-fixer) The editor's children end with it: Play > VR Preview starts the game's executable
# (rivora_game.exe) as a child; the editor quits after some frames, and no rivora_game.exe of this build may be left.
# Before: one started by the UI sweep stayed open and took the GPU through two suites (72 false fps failures).
# cmake -DENGINE=<rivora.exe> -DPROJECT=<new project folder> -P tools/test_child_cleanup.cmake
get_filename_component(dir "${ENGINE}" DIRECTORY)
file(TO_NATIVE_PATH "${dir}/rivora_game.exe" game)
file(REMOVE_RECURSE "${PROJECT}")
execute_process(COMMAND "${ENGINE}" --new-project "${PROJECT}" --template 0 --vr-preview --quit-after 240
                OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r TIMEOUT 180)
string(REGEX MATCH "[^\n]*(VR Preview|Prévia em VR)[^\n]*" vr "${o}${e}")
message("child cleanup: the editor ended (${r}); ${vr}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
# the game processes of this build still alive (by their path)
execute_process(COMMAND powershell -NoProfile -Command
                "@(Get-Process rivora_game -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq '${game}' }).Count"
                OUTPUT_VARIABLE n OUTPUT_STRIP_TRAILING_WHITESPACE)
if(n STREQUAL "0")
    message("child cleanup: OK (no rivora_game.exe left)")
else()
    message("FAIL: child cleanup: ${n} rivora_game.exe of this build still running after the editor ended (killed now)")
    execute_process(COMMAND powershell -NoProfile -Command
                    "Get-Process rivora_game -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq '${game}' } | Stop-Process -Force")
endif()
