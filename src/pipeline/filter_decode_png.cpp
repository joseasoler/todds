/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "filter_decode_png.hpp"

#include "todds/alpha_coverage.hpp"
#include "todds/filter.hpp"
#include "todds/mipmap_image.hpp"
#include "todds/png.hpp"

#include <fmt/format.h>
#include <oneapi/tbb/parallel_for.h>
#include <opencv2/imgproc.hpp>

namespace {

void process_image(todds::mipmap_image& mipmap_img, todds::filter::type filter) {
	// constexpr std::uint8_t default_alpha_reference = 245U;
	// const float desired_coverage = todds::alpha_coverage(default_alpha_reference, original_image);
	const auto input = static_cast<cv::Mat>(mipmap_img.get_image(0UL));

	using blocked_range = oneapi::tbb::blocked_range<size_t>;
	oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<size_t>(1UL, mipmap_img.mipmap_count()),
		[&mipmap_img, &input, filter](const blocked_range& range) {
			for (std::size_t mipmap_index = range.begin(); mipmap_index < range.end(); ++mipmap_index) {
				todds::image& current_image = mipmap_img.get_image(mipmap_index);
				auto output = static_cast<cv::Mat>(current_image);
				cv::resize(input, output, output.size(), 0, 0, static_cast<int>(filter));
				// ToDo Solve alpha coverage issues on Windows
				// todds::scale_alpha_to_coverage(desired_coverage, default_alpha_reference, current_image);
			}
		});
}

} // Anonymous namespace

namespace todds::pipeline::impl {

class decode_png final {
public:
	explicit decode_png(vector<file_data>& files_data, const paths_vector& paths, bool vflip, bool mipmaps,
		filter::type filter, error_queue& errors) noexcept
		: _files_data{files_data}
		, _paths{paths}
		, _vflip{vflip}
		, _errors{errors}
		, _mipmaps{mipmaps}
		, _filter{filter} {}

	std::unique_ptr<mipmap_image> operator()(const png_file& file) const {
		std::unique_ptr<mipmap_image> result{};

		// If the data is empty, assume that load_png_file already reported an error.
		if (!file.buffer.empty()) {
			const std::string& path = _paths[file.file_index].first.string();
			try {
				auto& file_data = _files_data[file.file_index];
				// Load the first image of the mipmap image and reserve the memory for the rest of the images.
				result = png::decode(file.file_index, path, file.buffer, _vflip, file_data.width, file_data.height, _mipmaps);
				file_data.mipmaps = result->mipmap_count();
				process_image(*result, _filter);
			} catch (const std::runtime_error& exc) {
				_errors.push(fmt::format("PNG Decoding error {:s} -> {:s}", path, exc.what()));
			}
		}

		return result;
	}

private:
	vector<file_data>& _files_data;
	const paths_vector& _paths;
	bool _vflip;
	error_queue& _errors;
	bool _mipmaps;
	filter::type _filter;
};

oneapi::tbb::filter<png_file, std::unique_ptr<mipmap_image>> decode_png_filter(vector<file_data>& files_data,
	const paths_vector& paths, bool vflip, bool mipmaps, filter::type filter, error_queue& errors) {
	return oneapi::tbb::make_filter<png_file, std::unique_ptr<mipmap_image>>(
		oneapi::tbb::filter_mode::serial_in_order, decode_png(files_data, paths, vflip, mipmaps, filter, errors));
}

} // namespace todds::pipeline::impl
