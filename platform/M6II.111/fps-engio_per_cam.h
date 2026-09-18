#ifndef _fps_engio_per_cam_h_
#define _fps_engio_per_cam_h_

/*
 * EOS M6 Mark II 1.1.1 DIGIC 8 sensor timing registers.
 *
 * Verified on camera in Canon movie modes:
 *   FHD24: A=0x05ff, B≈0x0824
 *   FHD30: A=0x05ff, B≈0x0684
 *   FHD60: A=0x05ff, B≈0x0684 with doubled TG clock
 *   4K24 : A=0x044f, B≈0x0b54
 *   4K30 : A=0x044f, B≈0x0910
 *
 * Same register layout used by Bilal's M50 DIGIC 8 implementation.
 */
#define FPS_REGISTER_A               0xD0406198
#define FPS_REGISTER_B               0xD04061A4
#define FPS_REGISTER_CONFIRM_CHANGES 0xD0406190

int get_fps_register_a(void);
int get_fps_register_a_default(void);
int get_fps_register_b(void);

#endif
