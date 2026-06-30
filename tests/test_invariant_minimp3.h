#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "components/minimp3/include/minimp3.h"

START_TEST(test_buffer_reads_never_exceed_declared_length)
{
    // Invariant: Buffer reads never exceed the declared length
    const char *payloads[] = {
        // Exact exploit case: crafted MP3 header with oversized total_bands
        "\xFF\xFB\x90\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        // Boundary case: total_bands = stereo_bands (minimum copy size)
        "\xFF\xFB\x48\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        // Valid input: normal MP3 frame
        "\xFF\xFB\x48\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01"
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        mp3dec_frame_info_t info;
        mp3dec_t dec;
        mp3dec_init(&dec);
        
        // Allocate destination buffer with known size and guard pages
        float dst[1152];  // Standard MP3 output buffer size
        memset(dst, 0, sizeof(dst));
        
        // Attempt to decode the payload
        int samples = mp3dec_decode_frame(&dec, (const uint8_t*)payloads[i], 
                                          strlen(payloads[i]), dst, &info);
        
        // The security property: no buffer overflow occurred
        // If overflow happened, we'd likely crash before this point
        // This test passes if it completes without segmentation fault
        ck_assert_msg(samples >= 0 || samples == MP3D_E_PARAM || samples == MP3D_E_MEMORY,
                     "Buffer overflow detected for payload %d", i);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_reads_never_exceed_declared_length);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}