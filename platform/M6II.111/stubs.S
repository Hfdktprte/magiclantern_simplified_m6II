/** \file
 * Entry points into the firmware image.
 *
 * These are the functions that we can call from our tasks
 * in the Canon 1.1.1 firmware for the EOS M6 Mark II
 *
 */
/*
 * Copyright (C) 2025 Magic Lantern Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

// A big chunk of this file was auto-generated from Ghidra project
// using R.180 as a stubs source

#include <stub.h>

/** Startup **/
THUMB_FN(0xe0040104, cstart)
THUMB_FN(0xe059a5f4, bzero32)
THUMB_FN(0xe028bcfc, create_init_task)
THUMB_FN(0xe0599200, dcache_clean)
THUMB_FN(0xe005e63c, dcache_clean_multicore)
THUMB_FN(0xe05992d4, icache_invalidate)
THUMB_FN(0xe0040236, init_task)
THUMB_FN(0xe02be544, memcpy_dryos)
THUMB_FN(0xe0576e86, change_mmu_tables)

/** Tasks **/
DATA_PTR(0x1010, current_interrupt)
DATA_PTR(0x1028, current_task)
THUMB_FN(0xe058599a, msleep)
THUMB_FN(0xe059da7a, task_create)
THUMB_FN(0xe059dc92, task_create_ex)
DATA_PTR(0x4118, task_max)

/** File I/O **/
THUMB_FN(0xe0579faa, _FIO_CreateFile)
THUMB_FN(0xe0579ef8, _FIO_OpenFile)
THUMB_FN(0xe057a750, FIO_CloseFile)
THUMB_FN(0xe057a382, _FIO_ReadFile)
THUMB_FN(0xe057a534, _FIO_WriteFile)
THUMB_FN(0xe057a476, FIO_SeekSkipFile)
THUMB_FN(0xe057a2ea, _FIO_RemoveFile)
THUMB_FN(0xe057b34e, _FIO_RenameFile)
THUMB_FN(0xe057b7d6, _FIO_CreateDirectory)
THUMB_FN(0xe057bc30, _FIO_FindFirstEx)
THUMB_FN(0xe057bd2c, FIO_FindNextEx)
THUMB_FN(0xe057bdee, FIO_FindClose)
THUMB_FN(0xe057b9ba, FIO_Flush)
THUMB_FN(0xe057a8ea, _FIO_GetFileSize64)

/** Bitmap **/
DATA_PTR(0x78c78, bmp_vram_info)

/** Debug messages **/
THUMB_FN(0xe0599848, DryosDebugMsg)

/** Eventprocs (call by name) **/
THUMB_FN(0xe05830b6, call)

/** GUI timers / DMA allocation **/
THUMB_FN(0xe03d8ba2, SetHPTimerNextTick)
THUMB_FN(0xe03d8b32, SetHPTimerAfterNow)
THUMB_FN(0xe0584d14, SetTimerAfter)
THUMB_FN(0xe0584dca, CancelTimer)
THUMB_FN(0xe058c174, _alloc_dma_memory)
THUMB_FN(0xe058c168, _free_dma_memory)
THUMB_FN(0xe0360e94, _malloc)
THUMB_FN(0xe0360ecc, _free)

/** Interrupts **/
THUMB_FN(0xe01c54c0, cli_spin_lock)
THUMB_FN(0xe00af448, send_software_interrupt)
THUMB_FN(0xdf002c52, _request_RPC)
THUMB_FN(0xdf002c72, clear_RPC_request)

/** Memory allocation **/
THUMB_FN(0xe058b9b8, _AllocateMemory)
THUMB_FN(0xe058bb70, _FreeMemory)
THUMB_FN(0xe058b842, GetMemoryInformation)
THUMB_FN(0xe058b816, GetSizeOfMaxRegion)

/** Memory info **/
THUMB_FN(0xe03653a0, memmap_info)
THUMB_FN(0xe010eb7c, smemShowFix)
THUMB_FN(0xe03926ec, malloc_info)
THUMB_FN(0xe0392784, sysmem_info)

/** Misc **/
THUMB_FN(0xe0587590, uart_printf)
THUMB_FN(0xe0061238, vsnprintf)

/** Tasks **/
DATA_PTR(0x4154, task_dispatch_hook)
THUMB_FN(0xe008faa8, task_trampoline)
THUMB_FN(0xe01b765c, _get_task_info_by_id)

/** Recursive locks **/
THUMB_FN(0xe0598e34, CreateRecursiveLock)
THUMB_FN(0xe0577944, AcquireRecursiveLock)
THUMB_FN(0xe0577a0e, ReleaseRecursiveLock)

/** Semaphores **/
THUMB_FN(0xe0599358, create_named_semaphore)
THUMB_FN(0xe05994ce, give_semaphore)
THUMB_FN(0xe0599454, take_semaphore)
THUMB_FN(0xe0599424, take_semaphore_now)

/** Event flags required by Bilal's DIGIC 8 mem2mem path **/
THUMB_FN(0xe0598de6, CreateEventFlag_strictly)
THUMB_FN(0xe0576f3a, DeleteEventFlag)
THUMB_FN(0xe0577058, WaitForAnyEventFlag)
THUMB_FN(0xe0577254, SetEventFlag)
THUMB_FN(0xe0577282, ClearEventFlag)

/** GUI **/
DATA_PTR(0x11960, gui_task_list)
THUMB_FN(0xe0557032, SetGUIRequestMode)
THUMB_FN(0xe01dcb18, gui_main_task)
THUMB_FN(0xe03db75a, gui_massive_event_loop)
THUMB_FN(0xe01dcc4a, gui_enqueue_message)
DATA_PTR(0x5488, gui_main_struct)

/** Dialog API **/
THUMB_FN(0xe0562b32, dialog_redraw)
THUMB_FN(0xe055f3a0, dialog_set_property_str)

/** GUI timers **/
THUMB_FN(0xe069e62e, StartDateTimer)
THUMB_FN(0xe069e6e2, CancelDateTimer)

/** Queue **/
THUMB_FN(0xe057df00, msg_queue_create)
THUMB_FN(0xe057dfc4, msg_queue_receive)
THUMB_FN(0xe057e122, msg_queue_post)
THUMB_FN(0xe057e158, msg_queue_count)

/** Memory **/
THUMB_FN(0xe0663838, AllocateMemoryResource)
THUMB_FN(0xe0663866, AllocateContinuousMemoryResource)
THUMB_FN(0xe0663894, FreeMemoryResource)
THUMB_FN(0xe0586524, AddMemoryChunk)
THUMB_FN(0xe05865a8, GetFirstChunkFromSuite)
THUMB_FN(0xe0586dc8, GetNextMemoryChunk)
THUMB_FN(0xe058636a, GetMemoryAddressOfMemoryChunk)
THUMB_FN(0xe0586774, DeleteMemorySuite)
THUMB_FN(0xe0586a00, CreateMemorySuite)
THUMB_FN(0xe0586046, CreateMemoryChunk)

/** DIGIC 8 engine power/resource management (M6II ROM-mapped) **/
THUMB_FN(0xe0630eb6, PwrMng_WakeSubChips)
THUMB_FN(0xe0630ee4, PwrMng_SuspendSubChips)
THUMB_FN(0xe057978a, CreateResLockEntry)
THUMB_FN(0xe0579972, LockEngineResources)
THUMB_FN(0xe0579a0c, UnLockEngineResources)

/** DIGIC 8 EDMAC API (M6II ROM-mapped)
 * ROM0.BIN is based at 0xE0000000.  Do not add the old +0x40000 shift here.
 * The 0x4B RAW writer specifically calls edmac_set_address at 0xE0580962
 * and edmac_set_size at 0xE058096E.
 */
THUMB_FN(0xe057ff40, ConnectReadEDmac_maybe)
THUMB_FN(0xe057ff78, edmac_reset_channel)
THUMB_FN(0xe0580062, RegisterEDmacCompleteCBR)
THUMB_FN(0xe05803fa, UnregisterEDmacCompleteCBR)
THUMB_FN(0xe058048a, edmac_reset_boomer_vdkick)
THUMB_FN(0xe0580502, edmac_select_boomer)
THUMB_FN(0xe0580596, edmac_stop_boomer_maybe)
THUMB_FN(0xe05805da, edmac_reset_packunpack_mode)
THUMB_FN(0xe0580782, StartEDmac_maybe)
THUMB_FN(0xe0580962, edmac_set_address)
THUMB_FN(0xe058096e, edmac_set_size)
THUMB_FN(0xe0580e5a, edmac_set_transfer_mode)

/** M6II SD/UHS (verified from ROM0 1.1.1) **/
/* Canon storage query used by DebugSTG_GetUHS2CardCapability. */
THUMB_FN(0xe057d584, M6II_GetUHS2CardCapability)
/* Canon's mounted-card UHS-II query; DebugSTG_CheckUHS2Mode calls this with 'B'. */
THUMB_FN(0xe057d62a, M6II_IsUhs2Mode)

/* Canon UHS-I driver calls, verified from M6II 1.1.1 ROM. */
THUMB_FN(0xe017879c, M6II_SdChangeClockSpeed)
THUMB_FN(0xe0179f06, M6II_SdCARDGetSpeed)

/** Uncategorized **/
DATA_PTR(0x14684, sd_device)
THUMB_FN(0xe0818adc, _LoadCalendarFromRTC)
THUMB_FN(0xe08f942e, ErrForCamera_handler)
THUMB_FN(0xe0599c42, dm_set_store_level)

/** Properties **/
THUMB_FN(0xe058c8dc, _prop_cleanup)
THUMB_FN(0xe058c332, prop_register_slave)
THUMB_FN(0xe058c400, _prop_request_change)

/** Compositor **/
THUMB_FN(0xe027640c, refreshVrmsSurface)
DATA_PTR(0x11ad8, display_refresh_needed)
DATA_PTR(0x78b74, RENDERER_LayersMetadata)
THUMB_FN(0xe009acba, XCM_SetSourceSurface)
THUMB_FN(0xe009af70, XCM_GetSourceSurface)
THUMB_FN(0xe009afa4, XCM_SetSourceArea)
THUMB_FN(0xe027679a, XOC_SetLayerEnable)
DATA_PTR(0x78ce8, _pXCM)
THUMB_FN(0xe017cef0, XimrExe)
DATA_PTR(0x11ac4, winsys_sem)
DATA_PTR(0x11a8c, _rgb_vram_info)

/** App handlers **/
THUMB_FN(0xe0520974, PlayMain_handler)
THUMB_FN(0xe06ef5b0, ShootOlcApp_handler)
THUMB_FN(0xe0433e4c, LiveViewApp_handler)
THUMB_FN(0xe03a06ca, PlayMovieGuideApp_handler)
DATA_PTR(0xf0b4, LiveViewApp_dialog)
