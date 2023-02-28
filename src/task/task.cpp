/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "png2dds/task.hpp"

#include "png2dds/pipeline.hpp"
#include "png2dds/regex.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>

#include <algorithm>
#include <string_view>

namespace fs = boost::filesystem;
using png2dds::pipeline::paths_vector;

namespace {

constexpr std::string_view png_extension{".png"};
constexpr std::string_view txt_extension{".txt"};

bool has_extension(const fs::path& path, std::string_view extension) {
	return boost::to_lower_copy(path.extension().string()) == extension;
}

/**
 * Checks if a source path is a valid input.
 * @param path Source path to be checked.
 * @param regex A regex constructed with an empty pattern will always return true.
 * @param scratch Scratch used for regular expression evaluation.
 * @return True if the path should be processed.
 */
bool is_valid_source(const fs::path& path, const png2dds::regex& regex, png2dds::regex::scratch_type& scratch) {
	return has_extension(path, png_extension) && regex.match(scratch, path.string());
}

fs::path to_dds_path(const fs::path& png_path, const fs::path& output) {
	constexpr std::string_view dds_extension{".dds"};
	return (output / png_path.stem()) += dds_extension.data();
}

void add_files(const fs::path& png_path, const fs::path& dds_path, paths_vector& paths, bool overwrite) {
	if (overwrite || !fs::exists(dds_path)) { paths.emplace_back(png_path, dds_path); }
}

void process_directory(paths_vector& paths, const fs::path& input, const fs::path& output, bool different_output,
	const png2dds::regex& regex, png2dds::regex::scratch_type& scratch, bool overwrite, std::size_t depth) {
	fs::path current_output = output;
	const fs::directory_entry dir{input};
	for (fs::recursive_directory_iterator itr{dir}; itr != fs::recursive_directory_iterator{}; ++itr) {
		const fs::path& current_input = itr->path();
		if (is_valid_source(current_input, regex, scratch)) {
			const fs::path output_current = current_input.parent_path();
			if (different_output) { current_output = output / fs::relative(current_input.parent_path(), input); }
			const fs::path dds_path =
				to_dds_path(current_input, different_output ? current_output : current_input.parent_path());
			add_files(current_input, dds_path, paths, overwrite);
			if (different_output && !fs::exists(current_output)) {
				// Create the output folder if necessary.
				fs::create_directories(current_output);
			}
		}
		if (static_cast<unsigned int>(itr.depth()) >= depth) { itr.disable_recursion_pending(); }
	}
}

paths_vector get_paths(const png2dds::args::data& arguments) {
	const fs::path& input = arguments.input;
	const bool different_output = static_cast<bool>(arguments.output);
	const fs::path output = different_output ? arguments.output.value() : input.parent_path();
	const bool overwrite = arguments.overwrite;
	const auto depth = arguments.depth;

	const auto& regex = arguments.regex;
	png2dds::regex::scratch_type scratch = regex.allocate_scratch();

	paths_vector paths{};
	if (fs::is_directory(input)) {
		process_directory(paths, input, output, different_output, regex, scratch, overwrite, depth);
	} else if (is_valid_source(input, arguments.regex, scratch)) {
		const fs::path dds_path = to_dds_path(input, output);
		add_files(input, dds_path, paths, overwrite);
	} else if (has_extension(input, txt_extension)) {
		boost::nowide::fstream stream{input};
		std::string buffer;
		while (std::getline(stream, buffer)) {
			const fs::path current_path{buffer};
			if (fs::is_directory(current_path)) {
				process_directory(paths, current_path, current_path, false, regex, scratch, overwrite, depth);
			} else if (is_valid_source(current_path, arguments.regex, scratch)) {
				const fs::path dds_path = to_dds_path(current_path, current_path.parent_path());
				add_files(current_path, dds_path, paths, overwrite);
			} else {
				boost::nowide::cerr << current_path.string() << " is not a PNG file or a directory.\n";
			}
		}
	}

	// Process the list in order ignoring duplicates.
	std::sort(paths.begin(), paths.end());
	paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

	return paths;
}

} // anonymous namespace

namespace png2dds {

void run(const args::data& arguments) {
	pipeline::input input_data;
	input_data.paths = get_paths(arguments);
	if (input_data.paths.empty()) { return; }
	input_data.parallelism = arguments.threads;
	input_data.mipmaps = arguments.mipmaps;
	input_data.format = arguments.format;
	input_data.quality = arguments.quality;
	input_data.vflip = arguments.vflip;
	input_data.verbose = arguments.verbose;

	// Launch the parallel pipeline.
	pipeline::encode_as_dds(input_data);
}

} // namespace png2dds
