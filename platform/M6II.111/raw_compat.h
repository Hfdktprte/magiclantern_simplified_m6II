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
 * Bilal's raw.c expects a per-camera dynamic_ranges object for the photo-LV
 * DXO estimator. Keep this preprocessor-only because raw_compat.h is force-
 * included while preprocessing both C sources and magiclantern.lds.S.
 * A real C definition here would leak into the linker script.
 * This table is unrelated to RAW EDMAC geometry/redirection/mlv_lite capture.
 */
#ifndef dynamic_ranges
#define dynamic_ranges ((int[]){ \
    1255, 1237, 1188, 1120, 1045, 964, 883, 785, 685, 599 \
})
#endif

#endif /* M6II_RAW_COMPAT_H */
