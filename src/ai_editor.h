// The AI editors (ai_editor.c): the Behavior Tree Editor, the Real Brain Editor, the AI component's Details
#pragma once
#include <stdbool.h>

struct Editor;
struct CAIAgent;

void ai_editor_open(const char* vpath);          // a .rbt or .rbrain ("/Content/...")
void ai_editor_blackboard_mode(bool on); // the Behavior Tree editor in its Blackboard mode (screenshots)
void ai_editor_draw(struct Editor* ed, double now);
bool ai_editor_add_menu(const char* folder_vpath); // the Content Browser's Add menu (inside an open popup)
bool ai_component_details(struct CAIAgent* a);    // true when changed
