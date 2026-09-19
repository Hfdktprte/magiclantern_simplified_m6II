#ifndef M6II_CRX_PROBE_H
#define M6II_CRX_PROBE_H

#include <stdint.h>

#define M6II_CRX_CAP_MAGIC 0x43525841u /* 'CRXA' */
#define M6II_CRX_CAP_VERSION 1u
#define M6II_CRX_CAP_MAX 12u
#define M6II_CRX_BLOB1_SIZE 0x100u
#define M6II_CRX_BLOB2_SIZE 0x100u

enum m6ii_crx_cap_kind
{
    M6II_CRX_CAP_INIT = 1,
    M6II_CRX_CAP_SET_PARAM = 2,
    M6II_CRX_CAP_START = 3,
};

struct m6ii_crx_cap_record
{
    uint32_t kind;
    uint32_t seq;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t reserved;
    uint8_t blob1[M6II_CRX_BLOB1_SIZE];
    uint8_t blob2[M6II_CRX_BLOB2_SIZE];
} __attribute__((packed));

struct m6ii_crx_cap_file
{
    uint32_t magic;
    uint32_t version;
    uint32_t record_count;
    uint32_t record_size;
    struct m6ii_crx_cap_record records[M6II_CRX_CAP_MAX];
} __attribute__((packed));

void m6ii_crx_cap_reset(void);
int m6ii_crx_cap_install(void);
int m6ii_crx_cap_remove(void);
uint32_t m6ii_crx_cap_count(void);
const struct m6ii_crx_cap_file *m6ii_crx_cap_data(void);

#endif
