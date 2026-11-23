// Real Engine: a failed IM_ASSERT ends in a crash report with the expression and the line (src/crashlog.c).
#ifdef __cplusplus
extern "C"
#endif
void re_imgui_assert_fail(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : re_imgui_assert_fail(#_EXPR, __FILE__, __LINE__))
// Real Engine: this imgui copy comes from the Tlender, whose DragScalar and TempInputScalar call two hooks (Blender's
// soft/hard number ranges and typed expressions). The engine answers them neutrally in src/imgui_hooks.c.
#ifdef __cplusplus
extern "C" {
#endif
int tl_imgui_temp_input_apply(const char* buf, int data_type, void* p_data, const char* format, float x0, float y0,
                              float x1, float y1);
int num_limits_field(double lo, double hi, int flags, double* hlo, double* hhi);
void num_limits_group(bool on);
// Real Engine: the descriptions (src/ui_tips.c). ItemAdd reports the hovered labeled widget (its rectangle, its window's
// flags and x span, its viewport's work area); BeginTooltip reports that a widget showed its own tooltip.
void re_imgui_hover_label(const char* label, unsigned int id, float x0, float y0, float x1, float y1, int window_flags,
                          float win_x0, float win_x1, float vp_x0, float vp_y0, float vp_x1, float vp_y1);
void re_imgui_tooltip_begun(void);
// Real Engine: the Assistant's "Show me where" (src/assistant_ui.c): while GREHighlight, every labeled item drawn
extern int GREHighlight;
void re_imgui_highlight_item(const char* label, float x0, float y0, float x1, float y1);
// Real Engine: every combo's list can be searched (sugestão #60, imgui_widgets.cpp): the field's hint in the editor's
// language (src/imgui_hooks.c), and whether a row's label passes the open combo's search (rows drawn by hand ask it)
const char* re_imgui_search_hint(void);
bool re_imgui_combo_filter_pass(const char* label);
#ifdef __cplusplus
}
#endif
