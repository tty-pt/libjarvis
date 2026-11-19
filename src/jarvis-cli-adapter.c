#include "../include/ttypt/jarvis-types.h"
#include <ttypt/xy.h>

#include <stdio.h>
#include <string.h>

extern void jarvis_desktop_populate_defaults(jarvis_world_t *w);
extern int  jarvis_desktop_execute_fallback(const jarvis_dsl_t *dsl, jarvis_adapter_result_t *res);

/* Default built-in XY listener when running standalone CLI */
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
#if !defined(__wasm__) && !defined(_WIN32)
	if (jarvis_desktop_execute_fallback(dsl, res) == 0) {
		return 0;
	}
#endif
	if (dsl && res && !res->handled) {
		res->handled = true;
		res->success = true;
		if (strcmp(dsl->op, "answer_question") == 0) {
			snprintf(res->output, sizeof(res->output), "%s", dsl->args.answer);
		} else {
			snprintf(res->output, sizeof(res->output), "executed:%s", dsl->op);
		}
	}
	return 0;
}
