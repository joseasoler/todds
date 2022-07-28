/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "png2dds/pipeline.hpp"

#include "png2dds/dds.hpp"
#include "png2dds/dds_image.hpp"
#include "png2dds/image.hpp"
#include "png2dds/pixel_block_image.hpp"
#include "png2dds/png.hpp"
#include "png2dds/vector.hpp"

#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>
#include <dds_defs.h>
#include <fmt/format.h>
#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <atomic>
#include <chrono>
#include <future>

namespace otbb = oneapi::tbb;

using png2dds::dds_image;
using png2dds::image;
using png2dds::pixel_block_image;
using png2dds::pipeline::paths_vector;

namespace {

constexpr std::size_t error_file_index = std::numeric_limits<std::size_t>::max();

constexpr DDS_HEADER_DXT10 header_extension{DXGI_FORMAT_BC7_UNORM, D3D10_RESOURCE_DIMENSION_TEXTURE2D, 0U, 1U, 0U};

struct png_file {
	png2dds::vector<std::uint8_t> buffer;
	std::size_t file_index;
};

class load_png_file final {
public:
	explicit load_png_file(const paths_vector& paths, std::atomic<std::size_t>& counter) noexcept
		: _paths{paths}
		, _counter{counter} {}

	png_file operator()(otbb::flow_control& flow) const {
		std::size_t index = _counter++;
		if (index >= _paths.size()) {
			flow.stop();
			return {};
		}
		boost::nowide::ifstream ifs{_paths[index].first, std::ios::in | std::ios::binary};
		return {{std::istreambuf_iterator<char>{ifs}, {}}, index};
	}

private:
	const paths_vector& _paths;
	std::atomic<std::size_t>& _counter;
};

class decode_png_image final {
public:
	explicit decode_png_image(
		const paths_vector& paths, bool vflip, otbb::concurrent_queue<std::string>& error_log) noexcept
		: _paths{paths}
		, _vflip{vflip}
		, _error_log{error_log} {}

	image operator()(const png_file& file) const {
		try {
			return png2dds::png::decode(file.file_index, _paths[file.file_index].first.string(), file.buffer, _vflip);
		} catch (const std::runtime_error& exc) { _error_log.push(fmt::format("PNG Decoding error -> {:s}", exc.what())); }
		return {error_file_index, 0U, 0U};
	}

private:
	const paths_vector& _paths;
	bool _vflip;
	otbb::concurrent_queue<std::string>& _error_log;
};

class get_pixel_blocks final {
public:
	pixel_block_image operator()(const image& image) const { return pixel_block_image{image}; }
};

class encode_bc1_image final {
public:
	explicit encode_bc1_image(unsigned int level) noexcept
		: _level{level} {}

	dds_image operator()(const pixel_block_image& pixel_image) const {
		if (pixel_image.file_index() != error_file_index) [[likely]] {
			return png2dds::dds::bc1_encode(_level, pixel_image);
		}
		return dds_image{};
	}

private:
	unsigned int _level;
};

class encode_bc7_image final {
public:
	explicit encode_bc7_image(unsigned int level) noexcept
		: _params{png2dds::dds::bc7_encode_params(level)} {}

	dds_image operator()(const pixel_block_image& pixel_image) const {
		if (pixel_image.file_index() != error_file_index) [[likely]] {
			return png2dds::dds::bc7_encode(_params, pixel_image);
		}
		return dds_image{};
	}

private:
	png2dds::dds::bc7_params _params;
};

class save_bc1_file final {
public:
	explicit save_bc1_file(const paths_vector& paths) noexcept
		: _paths{paths} {}

	void operator()(const dds_image& dds_img) const {
		if (dds_img.file_index() == error_file_index) [[unlikely]] { return; }
		boost::nowide::ofstream ofs{_paths[dds_img.file_index()].second, std::ios::out | std::ios::binary};
		ofs << "DDS ";
		const std::size_t block_size_bytes = dds_img.blocks().size() * sizeof(std::uint64_t);
		const auto header = dds_img.header();
		ofs.write(header.data(), header.size());
		ofs.write(reinterpret_cast<const char*>(dds_img.blocks().data()), static_cast<std::ptrdiff_t>(block_size_bytes));
		ofs.close();
	}

private:
	const paths_vector& _paths;
};

/** Takes into account BC7's header extension. */
class save_bc7_file final {
public:
	explicit save_bc7_file(const paths_vector& paths) noexcept
		: _paths{paths} {}

	void operator()(const dds_image& dds_img) const {
		if (dds_img.file_index() == error_file_index) [[unlikely]] { return; }
		boost::nowide::ofstream ofs{_paths[dds_img.file_index()].second, std::ios::out | std::ios::binary};
		ofs << "DDS ";
		const std::size_t block_size_bytes = dds_img.blocks().size() * sizeof(std::uint64_t);
		const auto header = dds_img.header();
		ofs.write(header.data(), header.size());
		ofs.write(reinterpret_cast<const char*>(&header_extension), sizeof(header_extension));
		ofs.write(reinterpret_cast<const char*>(dds_img.blocks().data()), static_cast<std::ptrdiff_t>(block_size_bytes));
		ofs.close();
	}

private:
	const paths_vector& _paths;
};

void error_reporting(
	std::atomic<std::size_t>& progress, std::size_t total, otbb::concurrent_queue<std::string>& error_log) {
	using namespace std::chrono_literals;
	std::size_t last_progress{};
	std::string error_str;
	bool requires_newline = false;

	while (progress < total) {
		while (error_log.try_pop(error_str)) {
			if (requires_newline) {
				boost::nowide::cerr << '\n';
				requires_newline = false;
			}
			boost::nowide::cerr << error_str << '\n';
		}
		const std::size_t current_progress = progress;

		if (current_progress > last_progress && current_progress < total) {
			last_progress = current_progress;
			boost::nowide::cout << fmt::format("\rProgress: {:d}/{:d}", current_progress, total);
			boost::nowide::cout.flush();
			requires_newline = true;
			std::this_thread::sleep_for(50ms);
		}
	}

	while (error_log.try_pop(error_str)) { boost::nowide::cerr << error_str << '\n'; }
	boost::nowide::cout << fmt::format("\rProgress: {:d}/{:d}\n", total, total);
	boost::nowide::cout.flush();
}

otbb::filter<pixel_block_image, dds_image> encoding_filter(png2dds::format::type format_type, unsigned int level) {
	switch (format_type) {
	case png2dds::format::type::bc1:
		return otbb::make_filter<pixel_block_image, dds_image>(otbb::filter_mode::parallel, encode_bc1_image{level});
	case png2dds::format::type::bc7:
		return otbb::make_filter<pixel_block_image, dds_image>(otbb::filter_mode::parallel, encode_bc7_image{level});
	}
}

otbb::filter<dds_image, void> save_filter(png2dds::format::type format_type, const paths_vector& paths) {
	switch (format_type) {
	case png2dds::format::type::bc1:
		return otbb::make_filter<dds_image, void>(otbb::filter_mode::parallel, save_bc1_file{paths});
	case png2dds::format::type::bc7:
		return otbb::make_filter<dds_image, void>(otbb::filter_mode::parallel, save_bc7_file{paths});
	}
}

} // Anonymous namespace

namespace png2dds::pipeline {

void encode_as_dds(std::size_t tokens, const args::data& arguments, const paths_vector& paths) {
	// Variables referenced by the filters.
	std::atomic<std::size_t> counter;
	otbb::concurrent_queue<std::string> error_log;

	auto error_report = arguments.verbose ? std::async(std::launch::async, error_reporting, std::ref(counter),
																						paths.size(), std::ref(error_log)) :
																					std::future<void>{};

	const otbb::filter<void, void> filters =
		otbb::make_filter<void, png_file>(otbb::filter_mode::serial_in_order, load_png_file(paths, counter)) &
		otbb::make_filter<png_file, image>(
			otbb::filter_mode::parallel, decode_png_image(paths, arguments.vflip, error_log)) &
		otbb::make_filter<image, pixel_block_image>(otbb::filter_mode::parallel, get_pixel_blocks{}) &
		encoding_filter(arguments.format, arguments.level) & save_filter(arguments.format, paths);

	otbb::parallel_pipeline(tokens, filters);

	if (error_report.valid()) {
		// Wait until the error report is done.
		error_report.get();
	}
}
} // namespace png2dds::pipeline
