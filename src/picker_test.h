// The asset picker's and Create from the selection's test (--picker-test, ctest asset_picker; notas-dev/content-browser.md)
#pragma once
#include <stdbool.h>

struct Editor;
bool picker_test_setup(struct Editor* ed); // the project's 1000 textures written (Content/PickerTest)
void picker_test_frame(struct Editor* ed); // after each editor frame
bool picker_test_finished(bool* ok);
