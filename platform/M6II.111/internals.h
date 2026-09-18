/**
 * Camera internals for M6II 1.1.1
 */

/** This camera has a DIGIC VIII chip */
#define CONFIG_DIGIC_VIII

// has inter-core RPC (so far this has always been dependent on SGI, 0xc)
#define CONFIG_RPC

// Cam has MMU (by itself, does nothing, see CONFIG_MMU_REMAP)
#define CONFIG_MMU

/** Digic 8 does not have bitmap font in ROM, try to load it from card **/
#define CONFIG_NO_BFNT

/** disable SRM for now
 * in current state SRM_AllocateMemoryResourceFor1stJob makes camera crash
 * even if just one buffer is requested.
 */
#define CONFIG_MEMORY_SRM_NOT_WORKING

/* has LV */
#define CONFIG_LIVEVIEW

#define CONFIG_STATE_OBJECT_HOOKS
#define CONFIG_EVF_STATE_SYNC

#define CONFIG_NO_DEDICATED_MOVIE_MODE

// include edmac-d8.c from platform dir
#define CONFIG_EDMAC_MEMCPY_D8

/*
 * SD-overclock test branch: keep the single M6II MMU ROM-remap page free for
 * Bilal's sd_uhs setup hook.  The low-bit RAW EDMAC ROM hook lives on the
 * separate RAW branch and must not compete for the same remap page here.
 */
/* #define CONFIG_EDMAC_RAW_PATCH */

/** Large total memory, leading to unusual memory mapping,
 * CACHEABLE / UNCACHEABLE changes
 */
#define CONFIG_MEM_2GB

#define CONFIG_MALLOC_STRUCT_V2

#define CONFIG_TASK_STRUCT_V2_SMP
#define CONFIG_TASK_ATTR_STRUCT_V5
