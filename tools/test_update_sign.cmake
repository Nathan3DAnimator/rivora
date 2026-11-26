# (2026-11-25, item 22) The updater's signature, offline: a key pair made here for the test (never the release key,
# which lives outside every repository), a package signed with it, then
#   - the signed package passes --verify;
#   - one byte changed: refused;
#   - signed by another key: refused.
# cmake -DUPDATER=<rivora_updater.exe> -DWORK=<folder> -P tools/test_update_sign.cmake
if(NOT UPDATER OR NOT WORK)
    message(FATAL_ERROR "FAIL: update_sign: UPDATER and WORK are needed")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run_updater out_var code_var)
    execute_process(COMMAND "${UPDATER}" ${ARGN} OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE r)
    string(STRIP "${o}${e}" oe)
    set(${out_var} "${oe}" PARENT_SCOPE)
    set(${code_var} "${r}" PARENT_SCOPE)
endfunction()

# two test keys: the project's and a stranger's
run_updater(pub_a code "--keygen" "${WORK}/test_a.key")
if(NOT code EQUAL 0 OR NOT pub_a MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "FAIL: update_sign: --keygen gave '${pub_a}' (${code})")
endif()
run_updater(pub_b code "--keygen" "${WORK}/test_b.key")

# a package (some bytes of a zip), signed with key A
string(REPEAT "Rivora update package payload 0123456789\n" 2000 body)
file(WRITE "${WORK}/Rivora_test.zip" "${body}")
run_updater(sig code "--sign" "${WORK}/test_a.key" "${WORK}/Rivora_test.zip")
if(NOT code EQUAL 0 OR NOT EXISTS "${WORK}/Rivora_test.zip.sig")
    message(FATAL_ERROR "FAIL: update_sign: --sign failed: ${sig}")
endif()

set(fails 0)
run_updater(o code "--verify" "${pub_a}" "${WORK}/Rivora_test.zip")
if(code EQUAL 0)
    message("update_sign: the signed package passes (${o})")
else()
    message("FAIL: update_sign: the signed package was refused: ${o}")
    math(EXPR fails "${fails} + 1")
endif()

# another key's public half: refused
run_updater(o code "--verify" "${pub_b}" "${WORK}/Rivora_test.zip")
if(code EQUAL 0)
    message("FAIL: update_sign: a package checked against another key passed")
    math(EXPR fails "${fails} + 1")
else()
    message("update_sign: against another key, refused (${o})")
endif()

# one byte changed (its .sig kept): refused
string(SUBSTRING "${body}" 0 1000 head)
string(SUBSTRING "${body}" 1001 -1 tail)
file(WRITE "${WORK}/Rivora_test.zip" "${head}X${tail}")
run_updater(o code "--verify" "${pub_a}" "${WORK}/Rivora_test.zip")
if(code EQUAL 0)
    message("FAIL: update_sign: a package with one byte changed passed")
    math(EXPR fails "${fails} + 1")
else()
    message("update_sign: one byte changed, refused (${o})")
endif()

file(REMOVE "${WORK}/test_a.key" "${WORK}/test_b.key") # (test keys: never kept)
if(fails EQUAL 0)
    message("update_sign: OK")
endif()
