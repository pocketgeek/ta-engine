#include "util/jpeg.h"

#include <jpeglib.h>

#include <csetjmp>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace ta::jpeg {

namespace {

struct ErrorMgr {
    jpeg_error_mgr pub;
    std::jmp_buf jump;
};

void onError(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<ErrorMgr*>(cinfo->err)->jump, 1);
}

} // namespace

Image load(const std::vector<uint8_t>& d) {
    jpeg_decompress_struct cinfo{};
    ErrorMgr err{};
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = onError;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        throw std::runtime_error("JPEG decode failed");
    }

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
}

} // namespace ta::jpeg
