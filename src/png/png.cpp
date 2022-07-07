/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "png2dds/png.hpp"

#include "spng.h"
#include <fmt/format.h>

#include <stdexcept>

namespace {

// RAII wrapper around the spng_ctx object.
class spng_context final {
public:
	static constexpr std::size_t limit = 1024UL * 1024UL * 64UL;

	explicit spng_context(const std::string& png)
		: _ctx{spng_ctx_new(0)} {
		if (_ctx == nullptr) { throw std::runtime_error{fmt::format("libspng context creation failed for {:s}", png)}; }

		/* Ignore chunk CRCs and their calculations. */
		spng_set_crc_action(_ctx, SPNG_CRC_USE, SPNG_CRC_USE);

		/* Set memory usage limits for storing standard and unknown chunks. */
		spng_set_chunk_limits(_ctx, limit, limit);
	}

	spng_context(const spng_context&) = delete;
	spng_context(spng_context&&) noexcept = delete;
	spng_context& operator=(const spng_context&) = delete;
	spng_context& operator=(spng_context&&) noexcept = delete;

	~spng_context() { spng_ctx_free(_ctx); }

	spng_ctx* get() { return _ctx; }

private:
	spng_ctx* _ctx;
};

} // anonymous namespace

namespace png2dds {

image decode(std::size_t file_index, const std::string& png, const std::vector<std::uint8_t>& buffer) {
	spng_context context{png};

	if (const int ret = spng_set_png_buffer(context.get(), buffer.data(), buffer.size()); ret != 0) {
		throw std::runtime_error{fmt::format("Could not set PNG file to buffer {:s}: {:s}", png, spng_strerror(ret))};
	}

	spng_ihdr header{};
	if (const int ret = spng_get_ihdr(context.get(), &header); ret != 0) {
		throw std::runtime_error{fmt::format("Could not read header data of {:s}: {:s}", png, spng_strerror(ret))};
	}

	image result(file_index, header.width, header.height);

	constexpr spng_format format = SPNG_FMT_RGBA8;
	// The png2dds buffer may be larger than the image size calculated by libspng because the buffer must ensure that
	// the pixels of the width and the row are divisible by 4.
	std::size_t file_size{};
	if (const int ret = spng_decoded_image_size(context.get(), format, &file_size); ret != 0) {
		throw std::runtime_error{fmt::format("Could not calculate decoded size of {:s}: {:s}", png, spng_strerror(ret))};
	}

	if (file_size > result.buffer().size()) {
		throw std::runtime_error{
			fmt::format("Could not fit {:s} into the buffer. Expected size: {:d}, calculated size: {:d}", png,
				result.buffer().size(), file_size)};
	}

	if (const int ret = spng_decode_image(context.get(), nullptr, 0, format, SPNG_DECODE_PROGRESSIVE); ret != 0) {
		throw std::runtime_error{fmt::format("Could not initialize decoding of {:s}: {:s}", png, spng_strerror(ret))};
	}

	int ret{};
	spng_row_info row_info{};
	const auto file_width = file_size / header.height;
	do {
		ret = spng_get_row_info(context.get(), &row_info);
		if (ret != 0) { break; }
		ret = spng_decode_row(context.get(), &result.get_byte(0UL, row_info.row_num), file_width);

	} while (ret == 0);

	if (ret != SPNG_EOI) {
		throw std::runtime_error{fmt::format("Progressive decode error in {:s}: {:s}", png, spng_strerror(ret))};
	}

	return result;
}

} // namespace png2dds
