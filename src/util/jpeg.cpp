#include "util/jpeg.h"

#include <jpeglib.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace tak::jpeg {

namespace {

// libjpeg's error_exit must not return. Throw instead of the classic
// setjmp/longjmp: on Win64 MinGW the longjmp is an SEH unwind that clobbered
// the caller's stack (heap/stack corruption after a corrupt JPEG), whereas a
// C++ throw unwinds through the C frames via the ordinary unwind tables.
void onError(j_common_ptr) {
    throw std::runtime_error("JPEG decode failed");
}
void onEncodeError(j_common_ptr) {
    throw std::runtime_error("JPEG encode failed");
}

// A destination that appends to a std::vector. Not jpeg_mem_dest: that one hands its
// malloc'd buffer back only when the image finishes, so an error thrown part way
// (onEncodeError) would leak the buffer it had grown.
struct VectorDest {
    jpeg_destination_mgr pub{};   // first: libjpeg sees only this
    std::vector<uint8_t>* out = nullptr;
    JOCTET chunk[16384];
};
void destInit(j_compress_ptr c) {
    auto* d = reinterpret_cast<VectorDest*>(c->dest);
    d->pub.next_output_byte = d->chunk;
    d->pub.free_in_buffer = sizeof d->chunk;
}
boolean destFull(j_compress_ptr c) {   // libjpeg calls this with the chunk full, whatever free_in_buffer says
    auto* d = reinterpret_cast<VectorDest*>(c->dest);
    d->out->insert(d->out->end(), d->chunk, d->chunk + sizeof d->chunk);
    destInit(c);
    return TRUE;
}
void destFinish(j_compress_ptr c) {
    auto* d = reinterpret_cast<VectorDest*>(c->dest);
    d->out->insert(d->out->end(), d->chunk, d->chunk + (sizeof d->chunk - d->pub.free_in_buffer));
}

} // namespace

Image load(const std::vector<uint8_t>& d) {
    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr err{};
    cinfo.err = jpeg_std_error(&err);
    err.error_exit = onError;

    try {
        jpeg_create_decompress(&cinfo);
        jpeg_mem_src(&cinfo, d.data(), static_cast<unsigned long>(d.size()));
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);

        Image img;
        img.width = int(cinfo.output_width);
        img.height = int(cinfo.output_height);
        img.rgba.resize(size_t(img.width) * img.height * 4);

        std::vector<uint8_t> row(size_t(img.width) * 3);
        uint8_t* rowPtr = row.data();
        while (cinfo.output_scanline < cinfo.output_height) {
            int y = int(cinfo.output_scanline);
            jpeg_read_scanlines(&cinfo, &rowPtr, 1);
            uint8_t* dst = &img.rgba[size_t(y) * img.width * 4];
            for (int x = 0; x < img.width; ++x) {
                dst[x * 4] = row[x * 3];
                dst[x * 4 + 1] = row[x * 3 + 1];
                dst[x * 4 + 2] = row[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return img;
    } catch (...) {
        jpeg_destroy_decompress(&cinfo);
        throw;
    }
}

std::vector<uint8_t> encode(int width, int height, const std::vector<uint8_t>& rgba, int quality) {
    if (width <= 0 || height <= 0 || rgba.size() != size_t(width) * height * 4)
        throw std::runtime_error("jpeg::encode: buffer size mismatch");

    jpeg_compress_struct cinfo{};
    jpeg_error_mgr err{};
    cinfo.err = jpeg_std_error(&err);
    err.error_exit = onEncodeError;

    std::vector<uint8_t> out;
    VectorDest dest;
    dest.out = &out;
    dest.pub.init_destination = destInit;
    dest.pub.empty_output_buffer = destFull;
    dest.pub.term_destination = destFinish;

    try {
        jpeg_create_compress(&cinfo);
        cinfo.dest = &dest.pub;
        cinfo.image_width = JDIMENSION(width);
        cinfo.image_height = JDIMENSION(height);
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&cinfo);   // YCbCr, with the colour planes halved both ways (4:2:0)
        jpeg_set_quality(&cinfo, quality, TRUE);
        // 4:4:4: every plane at the image's own size (see jpeg.h).
        for (int c = 0; c < cinfo.num_components; ++c)
            cinfo.comp_info[c].h_samp_factor = cinfo.comp_info[c].v_samp_factor = 1;
        jpeg_start_compress(&cinfo, TRUE);

        std::vector<uint8_t> row(size_t(width) * 3);
        uint8_t* rowPtr = row.data();
        while (cinfo.next_scanline < cinfo.image_height) {
            const uint8_t* src = &rgba[size_t(cinfo.next_scanline) * width * 4];
            for (int x = 0; x < width; ++x) {
                row[x * 3] = src[x * 4];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + 2];
            }
            jpeg_write_scanlines(&cinfo, &rowPtr, 1);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        return out;
    } catch (...) {
        jpeg_destroy_compress(&cinfo);
        throw;
    }
}

} // namespace tak::jpeg
