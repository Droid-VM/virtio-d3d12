#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * vkd3d debug-control hooks.
 *
 * libvkd3d-proton.a calls these; upstream defines them in libs/d3d12core/main.c
 * behind IVKD3DDebugControlInterface, which the UMD build leaves out. Nothing
 * in a driver process can toggle them, so every behavior stays off.
 * ------------------------------------------------------------------------- */

bool vkd3d_debug_control_is_test_suite(void) { return false; }
bool vkd3d_debug_control_explode_on_vvl_error(void) { return false; }
bool vkd3d_debug_control_has_out_of_spec_test_behavior(uint32_t behavior) { (void)behavior; return false; }
uint32_t vkd3d_debug_control_get_behavior_flags(void) { return 0; }
bool vkd3d_debug_control_mute_message_id(const char *vuid) { (void)vuid; return false; }
