#include "../include/ttypt/jarvis-types.h"
#include <ttypt/xy.h>

#include <stdio.h>
#include <string.h>

extern void jarvis_desktop_populate_defaults(jarvis_world_t *w);
extern int  jarvis_desktop_execute_fallback(const jarvis_dsl_t *dsl, jarvis_adapter_result_t *res);

/* Default built-in XY listeners when running standalone CLI */
XY_IMPL(int, jarvis_xy_observe, jarvis_world_t *, world)
{
#if !defined(__wasm__) && !defined(_WIN32)
	jarvis_desktop_populate_defaults(world);
#else
	(void)world;
#endif
	return 0;
}

XY_IMPL(int, jarvis_xy_execute, const jarvis_dsl_t *, dsl, jarvis_adapter_result_t *, res)
{
	(void)dsl;
	(void)res;
	return 0;
}

XY_IMPL(int, jarvis_xy_confirm, const char *, description)
{
	(void)description;
	return 1;
}

XY_IMPL(int, jarvis_xy_speak_clause, const char *, session_id, const char *, clause_text, int, is_final)
{
	(void)session_id;
	(void)clause_text;
	(void)is_final;
	return 0;
}

XY_IMPL(int, jarvis_xy_tts_interrupt, int, dummy)
{
	(void)dummy;
	return 0;
}

XY_IMPL(int, jarvis_xy_voice_status, int, state, const char *, partial_text)
{
	(void)state;
	(void)partial_text;
	return 0;
}

XY_IMPL(int, jarvis_xy_prompt, const char *, prompt_label, const char *, candidates_lines, char *, out_selection, size_t, out_sz)
{
	(void)prompt_label;
	(void)candidates_lines;
	if (!out_selection || out_sz == 0) return 0;
	out_selection[0] = '\0';
	return 0;
}

XY_IMPL(int, jarvis_xy_notify, const char *, title, const char *, body, int, play_earcon)
{
	(void)title;
	(void)body;
	(void)play_earcon;
	return 0;
}

