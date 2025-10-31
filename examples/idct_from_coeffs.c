#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../libgpujpeg/gpujpeg.h"
#include "../libgpujpeg/gpujpeg_decoder.h"

// Simple example showing how to inject external quantized coefficients (host buffer)
// and produce decoded RGB output using the new APIs.
// Usage:
//   ./idct_from_coeffs input.jpg [coeffs.bin] output.pnm
// If coeffs.bin is omitted the example uses zero coefficients (will produce a flat image).

static uint8_t* read_file(const char* fname, size_t* out_size)
{
    FILE* f = fopen(fname, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = sz;
    return buf;
}

static int write_file(const char* fname, const void* data, size_t size)
{
    FILE* f = fopen(fname, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, size, f) != size) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s input.jpg [coeffs.bin] output.pnm\n", argv[0]);
        return 1;
    }
    const char* jpeg_file = argv[1];
    const char* coeffs_file = NULL;
    const char* out_file = NULL;
    if (argc == 3) {
        coeffs_file = NULL;
        out_file = argv[2];
    } else {
        coeffs_file = argv[2];
        out_file = argv[3];
    }

    size_t jpeg_size = 0;
    uint8_t* jpeg_buf = read_file(jpeg_file, &jpeg_size);
    if (!jpeg_buf) {
        fprintf(stderr, "Failed to read %s\n", jpeg_file);
        return 1;
    }

    struct gpujpeg_image_info info;
    memset(&info, 0, sizeof info);
    if (gpujpeg_decoder_get_image_info2(jpeg_buf, jpeg_size, &info, 0, 0) != GPUJPEG_NOERR) {
        fprintf(stderr, "Failed to get image info from %s\n", jpeg_file);
        free(jpeg_buf);
        return 1;
    }

    // Create decoder and initialize for image parameters
    struct gpujpeg_decoder* dec = gpujpeg_decoder_create(0);
    if (!dec) {
        fprintf(stderr, "Failed to create decoder\n");
        free(jpeg_buf);
        return 1;
    }

    // Force output to RGB for PNM saving
    info.param_image.color_space = GPUJPEG_RGB;

    if (gpujpeg_decoder_init(dec, &info.param, &info.param_image) != 0) {
        fprintf(stderr, "Failed to init decoder for image parameters\n");
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    // Prepare output descriptor for normal decode
    struct gpujpeg_decoder_output out_norm;
    gpujpeg_decoder_output_set_default(&out_norm);
    out_norm.type = GPUJPEG_DECODER_OUTPUT_INTERNAL_BUFFER;

    // 1) Full decode first to obtain a reference RGB image (and to cause Huffman decode
    //    so that internal quantized buffer is populated). We'll keep the image for
    //    validating later results.
    if (gpujpeg_decoder_decode(dec, jpeg_buf, jpeg_size, &out_norm) != GPUJPEG_NOERR) {
        fprintf(stderr, "Failed to decode image (to obtain reference)\n");
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    // Save reference image to file (optional) - use provided output name with suffix
    char ref_out_name[1024];
    snprintf(ref_out_name, sizeof(ref_out_name), "%s.ref.pnm", out_file);
    {
        struct gpujpeg_image_parameters save_param = out_norm.param_image;
        save_param.color_space = GPUJPEG_RGB;
        save_param.pixel_format = GPUJPEG_444_U8_P012;
        if (gpujpeg_image_save_to_file(ref_out_name, out_norm.data, out_norm.data_size, &save_param) == 0) {
            printf("Wrote reference decoded output to %s\n", ref_out_name);
        }
    }
    // Keep a copy of reference image bytes for later comparison because internal buffers
    // in decoder are reused and subsequent processing may overwrite them.
    uint8_t* ref_image = malloc(out_norm.data_size);
    if (!ref_image) {
        fprintf(stderr, "Out of memory allocating reference image copy\n");
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }
    memcpy(ref_image, out_norm.data, out_norm.data_size);

    size_t coeff_count = gpujpeg_decoder_get_coefficients_count(dec);
    if (coeff_count == 0) {
        fprintf(stderr, "Decoder reports zero coefficient count (is decoder initialized?)\n");
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    int16_t* coeffs = malloc(coeff_count * sizeof(int16_t));
    if (!coeffs) {
        fprintf(stderr, "Out of memory\n");
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    if (coeffs_file) {
        size_t fsz = 0;
        uint8_t* cb = read_file(coeffs_file, &fsz);
        if (!cb) {
            fprintf(stderr, "Failed to read coeffs file %s\n", coeffs_file);
            free(coeffs);
            gpujpeg_decoder_destroy(dec);
            free(jpeg_buf);
            return 1;
        }
        size_t expected = coeff_count * sizeof(int16_t);
        if (fsz < expected) {
            fprintf(stderr, "Coefficient file too small: expected %zu bytes, got %zu\n", expected, fsz);
            free(cb);
            free(coeffs);
            gpujpeg_decoder_destroy(dec);
            free(jpeg_buf);
            return 1;
        }
        memcpy(coeffs, cb, expected);
        free(cb);
    } else {
        // zero coefficients -> flat image
        memset(coeffs, 0, coeff_count * sizeof(int16_t));
    }

    // === Flow A: export quantized coefficients extracted from JPEG to disk ===
    // Allocate buffer and fetch coefficients from decoder (device -> host)
    int16_t* coeffs_from_jpeg = malloc(coeff_count * sizeof(int16_t));
    if (!coeffs_from_jpeg) {
        fprintf(stderr, "Out of memory allocating coeffs_from_jpeg\n");
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    if (gpujpeg_decoder_get_quantized_coefficients_host(dec, coeffs_from_jpeg, coeff_count) != GPUJPEG_NOERR) {
        fprintf(stderr, "Failed to export quantized coefficients from decoder\n");
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    // Dump coefficients to disk (filename: <out_file>.from_jpeg.coeffs.bin)
    char dumped_coeffs_name[1024];
    snprintf(dumped_coeffs_name, sizeof(dumped_coeffs_name), "%s.from_jpeg.coeffs.bin", out_file);
    if (write_file(dumped_coeffs_name, coeffs_from_jpeg, coeff_count * sizeof(int16_t)) != 0) {
        fprintf(stderr, "Failed to write dumped coefficients to %s\n", dumped_coeffs_name);
    } else {
        printf("Dumped quantized coefficients extracted from JPEG to %s\n", dumped_coeffs_name);
    }
    // Also dump metadata required to initialize a decoder: gpujpeg_parameters and gpujpeg_image_parameters
    char dumped_meta_name[1024];
    snprintf(dumped_meta_name, sizeof(dumped_meta_name), "%s.from_jpeg.meta.bin", out_file);
    {
        FILE* mf = fopen(dumped_meta_name, "wb");
        if (mf) {
            if (fwrite(&info.param, 1, sizeof(info.param), mf) != sizeof(info.param) ||
                fwrite(&info.param_image, 1, sizeof(info.param_image), mf) != sizeof(info.param_image)) {
                fprintf(stderr, "Failed to write dumped metadata to %s\n", dumped_meta_name);
            } else {
                printf("Dumped decoder metadata to %s\n", dumped_meta_name);
            }
            fclose(mf);
        } else {
            fprintf(stderr, "Failed to open metadata file %s for writing\n", dumped_meta_name);
        }
    }

    // Now, process those coefficients through external coefficients path and get image
    if (gpujpeg_decoder_set_quantized_coefficients_host(dec, coeffs_from_jpeg, coeff_count) != GPUJPEG_NOERR) {
        fprintf(stderr, "Failed to set quantized coefficients (from JPEG dump)\n");
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    struct gpujpeg_decoder_output out_from_dump;
    gpujpeg_decoder_output_set_default(&out_from_dump);
    out_from_dump.type = GPUJPEG_DECODER_OUTPUT_INTERNAL_BUFFER;

    if (gpujpeg_decoder_process_external_coefficients(dec, &out_from_dump) != GPUJPEG_NOERR) {
        fprintf(stderr, "Processing coefficients (from JPEG dump) failed\n");
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    // Save this result for inspection
    char out_from_dump_name[1024];
    snprintf(out_from_dump_name, sizeof(out_from_dump_name), "%s.from_jpeg.pnm", out_file);
    {
        struct gpujpeg_image_parameters save_param = out_from_dump.param_image;
        save_param.color_space = GPUJPEG_RGB;
        save_param.pixel_format = GPUJPEG_444_U8_P012;
        if (gpujpeg_image_save_to_file(out_from_dump_name, out_from_dump.data, out_from_dump.data_size, &save_param) == 0) {
            printf("Wrote image reconstructed from extracted coeffs to %s\n", out_from_dump_name);
        }
    }

    // Validate: compare reference decode (copied ref_image) with out_from_dump.data
    if (out_norm.data_size == out_from_dump.data_size && memcmp(ref_image, out_from_dump.data, out_norm.data_size) == 0) {
        printf("[OK] Image from extracted coeffs matches direct decode output\n");
    } else {
        printf("[WARN] Image from extracted coeffs differs from direct decode output\n");
    }

    // === Flow B: read dumped coeff file (or user-provided coeffs_file) and process ===
    // If user provided coeffs_file we already read it above into `coeffs` buffer.
    // If not, use the dumped coefficients file we just created.
    if (!coeffs_file) {
        // reuse coeffs buffer and fill from dumped file we just wrote
        memcpy(coeffs, coeffs_from_jpeg, coeff_count * sizeof(int16_t));
    }
    // Process coefficients read from disk (coeffs)
    // Create a fresh decoder instance for Flow B - do not reuse `dec` from Flow A
    struct gpujpeg_decoder* dec2 = gpujpeg_decoder_create(0);
    if (!dec2) {
        fprintf(stderr, "Failed to create decoder instance for Flow B\n");
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    // Read decoder init parameters from disk (prefer meta next to provided coeffs_file, else the dumped meta)
    struct gpujpeg_parameters param_disk;
    struct gpujpeg_image_parameters param_image_disk;
    int have_meta = 0;
    if (coeffs_file) {
        char meta_path[1024];
        snprintf(meta_path, sizeof(meta_path), "%s.meta.bin", coeffs_file);
        size_t msz = 0;
        uint8_t* mbuf = read_file(meta_path, &msz);
        if (mbuf && msz >= sizeof(param_disk) + sizeof(param_image_disk)) {
            memcpy(&param_disk, mbuf, sizeof(param_disk));
            memcpy(&param_image_disk, mbuf + sizeof(param_disk), sizeof(param_image_disk));
            have_meta = 1;
        }
        free(mbuf);
    }
    if (!have_meta) {
        // try dumped_meta_name created by this program
        size_t msz = 0;
        uint8_t* mbuf = read_file(dumped_meta_name, &msz);
        if (mbuf && msz >= sizeof(param_disk) + sizeof(param_image_disk)) {
            memcpy(&param_disk, mbuf, sizeof(param_disk));
            memcpy(&param_image_disk, mbuf + sizeof(param_disk), sizeof(param_image_disk));
            have_meta = 1;
        }
        free(mbuf);
    }
    if (!have_meta) {
        fprintf(stderr, "Missing decoder metadata for Flow B; expected %s or %s.meta.bin\n", dumped_meta_name, coeffs_file ? coeffs_file : "<coeffs path>");
        gpujpeg_decoder_destroy(dec2);
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    if (gpujpeg_decoder_init(dec2, &param_disk, &param_image_disk) != 0) {
        fprintf(stderr, "Failed to init decoder (Flow B) for image parameters from disk meta\n");
        gpujpeg_decoder_destroy(dec2);
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    size_t coeff_count2 = gpujpeg_decoder_get_coefficients_count(dec2);
    if (coeff_count2 != coeff_count) {
        fprintf(stderr, "Coefficient count mismatch between decoders: %zu vs %zu\n", coeff_count2, coeff_count);
        gpujpeg_decoder_destroy(dec2);
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    if (gpujpeg_decoder_set_quantized_coefficients_host(dec2, coeffs, coeff_count2) != GPUJPEG_NOERR) {
        fprintf(stderr, "Failed to set quantized coefficients (from disk read) on Flow B decoder\n");
        gpujpeg_decoder_destroy(dec2);
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    struct gpujpeg_decoder_output out_from_disk;
    gpujpeg_decoder_output_set_default(&out_from_disk);
    out_from_disk.type = GPUJPEG_DECODER_OUTPUT_INTERNAL_BUFFER;

    if (gpujpeg_decoder_process_external_coefficients(dec2, &out_from_disk) != GPUJPEG_NOERR) {
        fprintf(stderr, "Processing coefficients (from disk) failed on Flow B decoder\n");
        gpujpeg_decoder_destroy(dec2);
        free(coeffs_from_jpeg);
        free(coeffs);
        gpujpeg_decoder_destroy(dec);
        free(jpeg_buf);
        return 1;
    }

    // Save this result
    char out_from_disk_name[1024];
    snprintf(out_from_disk_name, sizeof(out_from_disk_name), "%s.from_disk.pnm", out_file);
    {
        struct gpujpeg_image_parameters save_param = out_from_disk.param_image;
        save_param.color_space = GPUJPEG_RGB;
        save_param.pixel_format = GPUJPEG_444_U8_P012;
        if (gpujpeg_image_save_to_file(out_from_disk_name, out_from_disk.data, out_from_disk.data_size, &save_param) == 0) {
            printf("Wrote image reconstructed from disk coeffs to %s\n", out_from_disk_name);
        }
    }

    // Validate: compare out_from_dump.data with out_from_disk.data
    if (out_from_dump.data_size == out_from_disk.data_size && memcmp(out_from_dump.data, out_from_disk.data, out_from_dump.data_size) == 0) {
        printf("[OK] Image from dumped coeffs matches image from disk-read coeffs\n");
    } else {
        printf("[WARN] Image from dumped coeffs differs from image from disk-read coeffs\n");
    }

    // cleanup aux buffer
    // destroy Flow B decoder
    gpujpeg_decoder_destroy(dec2);
    free(coeffs_from_jpeg);
    free(ref_image);

    // Save the final user-visible output (the original example behavior): save the last produced image
    {
        struct gpujpeg_image_parameters save_param = out_from_disk.param_image;
        save_param.color_space = GPUJPEG_RGB;
        save_param.pixel_format = GPUJPEG_444_U8_P012;
        if (gpujpeg_image_save_to_file(out_file, out_from_disk.data, out_from_disk.data_size, &save_param) != 0) {
            fprintf(stderr, "Failed to save output to %s\n", out_file);
        } else {
            printf("Wrote final output to %s\n", out_file);
        }
    }

    free(coeffs);
    gpujpeg_decoder_destroy(dec);
    free(jpeg_buf);
    return 0;
}
