#ifndef M6II_RAW_COMPAT_H
#define M6II_RAW_COMPAT_H

/* EOS M6 Mark II ColorMatrix1 from LibRaw colordata.cpp. */
#ifndef CAM_COLORMATRIX1
#define CAM_COLORMATRIX1 \
    11498, 10000,  -3759, 10000,  -1516, 10000, \
    -5073, 10000,  12954, 10000,   2349, 10000, \
     -892, 10000,   1867, 10000,   6118, 10000
#endif

/*
 * Bilal's raw.c expects a per-camera dynamic_ranges table for the photo-LV
 * DXO estimator. This fallback mirrors Bilal's M50 temporary table and is not
 * used by RAW EDMAC geometry, buffer redirection, or mlv_lite frame capture.
 * Mark it unused because this compatibility header is force-included for all
 * M6II translation units, while raw.c is the only one that references it.
 */
static int dynamic_ranges[] __attribute__((unused)) = {
    1255, 1237, 1188, 1120, 1045, 964, 883, 785, 685, 599
};

#endif /* M6II_RAW_COMPAT_H */
