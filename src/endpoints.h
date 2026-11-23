// Where the program talks to on the internet: data/branding/endpoints.ini (embedded in the executable when it is built;
// REALENGINE_ENDPOINTS=<file> reads another one: the tests' local server). The updater (updater.h) and the report uploads
// (report_upload.h) read it. Nothing secret of the user goes here: a token in it, if any, is a bot's token limited to
// opening issues in the reports repository (see the file's comments).
#pragma once
#include <stdbool.h>
#include <stddef.h>

// The value of key in [section] ("" when missing). The text is the embedded file, or the override.
const char* endpoints_get(const char* section, const char* key);
// Reads the configuration again (from text when not NULL: the tests; else the override or the embedded one).
void endpoints_reload(const char* text);
// {owner} and {repo} of [github] replaced in s.
void endpoints_expand(const char* s, char* out, size_t cap);

// The parser itself, for any INI text: value of key in section (case-insensitive names), into out. False when missing.
bool ini_lookup(const char* text, const char* section, const char* key, char* out, size_t cap);
