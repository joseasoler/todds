/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "filter_pixel_blocks.hpp"

namespace todds::pipeline::impl {
class get_pixel_blocks final {
public:
	pixel_block_data operator()(std::unique_ptr<mipmap_image> image) const {
		TracyZoneScopedN("pixel_blocks");
		if (image == nullptr) [[unlikely]]
		{
			return {{}, error_file_index};
		}
		TracyZoneFileIndex(image->file_index());

		return pixel_block_data{todds::to_pixel_blocks(*image), image->file_index()};
	}
};

oneapi::tbb::filter<std::unique_ptr<mipmap_image>, pixel_block_data> pixel_blocks_filter() {
	return oneapi::tbb::make_filter<std::unique_ptr<mipmap_image>, pixel_block_data>(
		oneapi::tbb::filter_mode::parallel, get_pixel_blocks{});
}

} // namespace todds::pipeline::impl
