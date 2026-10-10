#pragma once
/* Version 1 native data contract. Layouts and enum values are frozen for this ABI.
 * This header has no ESP-IDF or interpreter dependency. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_image_types_v1.h"

#define SOLAR_OS_IMLIB_THRESHOLDS_MAX 8U
#define SOLAR_OS_IMLIB_BLOBS_MAX 64U
#define SOLAR_OS_IMLIB_BINS_MAX 256U
#define SOLAR_OS_IMLIB_MAX_WIDTH 640U
#define SOLAR_OS_IMLIB_MAX_HEIGHT 480U

typedef enum {
    SOLAR_OS_IMLIB_HISTOGRAM, SOLAR_OS_IMLIB_STATISTICS,
    SOLAR_OS_IMLIB_BINARY, SOLAR_OS_IMLIB_INVERT,
    SOLAR_OS_IMLIB_MEAN, SOLAR_OS_IMLIB_GAUSSIAN, SOLAR_OS_IMLIB_MEDIAN,
    SOLAR_OS_IMLIB_ERODE, SOLAR_OS_IMLIB_DILATE,
    SOLAR_OS_IMLIB_OPENING, SOLAR_OS_IMLIB_CLOSING,
    SOLAR_OS_IMLIB_DIFFERENCE, SOLAR_OS_IMLIB_BLOBS,
    SOLAR_OS_IMLIB_OPERATIONS_COUNT,
} solar_os_imlib_operation_t;
typedef struct {
    int16_t l_min, l_max, a_min, a_max, b_min, b_max;
} solar_os_imlib_threshold_t;
typedef struct {
    /* Format is GRAY8 or RGB565_LE. A zero crop/output dimension selects its
     * remaining/default extent. Bounds are checked before worker admission. */
    solar_os_raster_image_convert_options_t image;
    uint32_t timeout_ms, ksize, bins, x_stride, y_stride;
    uint32_t area_threshold, pixels_threshold, margin, max_blobs;
    bool invert, merge;
    size_t threshold_count;
    solar_os_imlib_threshold_t thresholds[SOLAR_OS_IMLIB_THRESHOLDS_MAX];
} solar_os_imlib_options_t;
typedef struct {
    int mean, median, mode, stdev, min, max, lower_quartile, upper_quartile;
} solar_os_imlib_statistics_t;
typedef struct {
    uint32_t x, y, width, height, pixels, code, count;
    float cx, cy, rotation;
} solar_os_imlib_blob_t;
typedef struct {
    solar_os_imlib_operation_t operation;
    solar_os_raster_image_convert_options_t image_options;
    uint64_t preprocess_us, process_us, output_us, elapsed_us;
    size_t workspace_peak_bytes;
    /* Transformations return one owned immutable image; analyses leave NULL.
     * Measurement channels are gray, or L/A/B for RGB565. Histograms contain
     * normalized fractions. Blob bounds/centroids use source-image pixels;
     * pixels counts matching pixels at the processed resolution. */
    solar_os_raster_image_t *image;
    unsigned channels, bins[3];
    float histogram[3][SOLAR_OS_IMLIB_BINS_MAX];
    solar_os_imlib_statistics_t statistics[3];
    size_t count;
    bool truncated;
    solar_os_imlib_blob_t blobs[SOLAR_OS_IMLIB_BLOBS_MAX];
} solar_os_imlib_result_t;
typedef bool (*solar_os_imlib_cancel_fn)(void *user);
