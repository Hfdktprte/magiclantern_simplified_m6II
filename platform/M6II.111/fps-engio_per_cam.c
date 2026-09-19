#include "dryos.h"
#include "fps-engio_per_cam.h"

int get_fps_register_a(void)
{
    return *(volatile uint32_t *)FPS_REGISTER_A;
}

int get_fps_register_a_default(void)
{
    return *(volatile uint32_t *)(FPS_REGISTER_A + 4);
}

int get_fps_register_b(void)
{
    return *(volatile uint32_t *)FPS_REGISTER_B;
}
