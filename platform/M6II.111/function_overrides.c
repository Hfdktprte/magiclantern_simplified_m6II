/** \file
 * Function overrides needed for R 1.8.0
 */
/*
 * Copyright (C) 2021 Magic Lantern Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <dryos.h>
#include <property.h>
#include <bmp.h>
#include <config.h>
#include <consts.h>
#include <lens.h>
#include <edmac.h>

struct chs_entry
{
    uint8_t head;
    uint8_t sector;
    uint8_t cyl_lsb;
}__attribute__((packed));

struct partition
{
    uint8_t  state;
    struct   chs_entry start;
    uint8_t  type;
    struct   chs_entry end;
    uint32_t start_sector;
    uint32_t size;
}__attribute__((aligned,packed));

struct partition_table
{
    uint8_t  state;
    uint8_t  start_head;
    uint16_t start_cylinder_sector;
    uint8_t  type;
    uint8_t  end_head;
    uint16_t end_cylinder_sector;
    uint32_t sectors_before_partition;
    uint32_t sectors_in_partition;
}__attribute__((packed));

void fsuDecodePartitionTable(void * partIn, struct partition_table * pTable){
    struct partition * part = (struct partition *) partIn;
    pTable->state      = part->state;
    pTable->type       = part->type;
    pTable->start_head = part->start.head;
    pTable->end_head   = part->end.head;
    pTable->sectors_before_partition = part->start_sector;
    pTable->sectors_in_partition     = part->size;
    pTable->start_cylinder_sector = 0;
    pTable->end_cylinder_sector   = 0;
}

void gui_init_end(void){ }

extern void gui_enqueue_message(uint32_t, uint32_t, uint32_t, uint32_t);
void GUI_Control(int bgmt_code, int obj, int arg, int unknown){
    gui_enqueue_message(0, bgmt_code, obj, arg);
}

void LoadCalendarFromRTC(struct tm *tm)
{
    _LoadCalendarFromRTC(tm, 0, 0, 16);
}

int get_task_info_by_id(int unknown_flag, int task_id, void *task_attr)
{
    struct task *task = first_task + (task_id & 0xffff);
    return _get_task_info_by_id(task->taskId, task_attr);
}

extern int _FIO_GetFileSize64(const char *, void *);
int _FIO_GetFileSize(const char * filename, uint32_t * size){
    uint32_t size64[2];
    int code = _FIO_GetFileSize64(filename, &size64);
    *size = size64[0];
    return code;
}

/* Legacy EDMAC entry points are not used by the M6II D8 mlv_lite backend. */
void SetEDmac(unsigned int channel, void *address, struct edmac_info *ptr, int flags) { return; }
void ConnectWriteEDmac(unsigned int channel, unsigned int where) { return; }
void ConnectReadEDmac(unsigned int channel, unsigned int where) { return; }
void StartEDmac(unsigned int channel, int flags) { return; }
void AbortEDmac(unsigned int channel) { return; }
void RegisterEDmacCompleteCBR(int channel, void (*cbr)(void*), void* cbr_ctx) { return; }
void UnregisterEDmacCompleteCBR(int channel) { return; }
void RegisterEDmacAbortCBR(int channel, void (*cbr)(void*), void* cbr_ctx) { return; }
void UnregisterEDmacAbortCBR(int channel) { return; }
void RegisterEDmacPopCBR(int channel, void (*cbr)(void*), void* cbr_ctx) { return; }
void UnregisterEDmacPopCBR(int channel) { return; }
void _EngDrvOut(uint32_t reg, uint32_t value) { return; }

/* D8 has no legacy shamem API. Keep unknown generic reads inert for the first
 * native-14-bit mlv_lite bring-up; verified RAW redirection uses direct MMIO. */
uint32_t shamem_read(uint32_t addr)
{
    return 0;
}

/* Same compatibility exports used by Bilal's M50 port. SRM is intentionally
 * disabled on M6II because Canon SRM allocation is known to crash this port. */
struct memSuite * srm_malloc_suite(int num_requested_buffers)
{
    return 0;
}

void srm_free_suite(struct memSuite * suite)
{
    return;
}

/* ErrCardForLVApp_handler does not exist on this D8 port; mlv_lite only needs
 * an addressable compatibility symbol. */
void ErrCardForLVApp_handler(void)
{
    return;
}

void _engio_write(uint32_t* reg_list) { return; }

unsigned int UnLockEngineResources(struct LockEntry *lockEntry)
{
    return 0;
}
