/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include <png2dds/memory.hpp>

namespace png2dds::memory {
chunk::chunk(std::size_t size)
	: _memory(size) {}

std::span<std::uint8_t> chunk::span() noexcept { return {_memory.data(), _memory.size()}; }

} // namespace png2dds::memory
