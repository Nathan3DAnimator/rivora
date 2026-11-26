# (2026-11-24) After the import_obj test: the import's report is not in the project's Content folder any more (it showed up
# in the Content Browser as ImportReport.txt) but in <project>/Saved/ImportReports/<source name>.txt.
#   cmake -DPROJECT=<test project folder> -DNAME=<source name without its extension> -P tools/check_import_report.cmake
file(GLOB_RECURSE txt "${PROJECT}/Content/*.txt")
if(txt)
    message(FATAL_ERROR "FAIL: text files in the Content folder after the import: ${txt}")
endif()
if(NOT EXISTS "${PROJECT}/Saved/ImportReports/${NAME}.txt")
    message(FATAL_ERROR "FAIL: no ${PROJECT}/Saved/ImportReports/${NAME}.txt")
endif()
message(STATUS "import report check: OK (Saved/ImportReports/${NAME}.txt, no .txt in Content)")
