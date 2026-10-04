#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "http_test_util.h"

// Plan IV.3 "Done": a corpus of malformed and smuggling-style inputs produces 400s (or
// the stated 4xx/5xx) with zero crashes. File names start with the expected status:
// tests/corpus/reject/<status>_<description>.txt (escaped format, see corpus README).

namespace {

namespace fs = std::filesystem;

lb::http::ParserLimits corpus_limits() {
    lb::http::ParserLimits l;
    l.max_start_line_bytes = 1024;
    l.max_field_section_bytes = 2048;
    l.max_head_bytes = SIZE_MAX;
    l.max_field_count = 20;
    l.max_chunk_line_bytes = 64;
    return l;
}

std::vector<fs::path> corpus_files() {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(fs::path(LB_SOURCE_DIR) / "tests" / "corpus" / "reject")) {
        if (entry.path().extension() == ".txt") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

TEST(HttpCorpus, EveryRejectCaseFailsWithItsStatus) {
    const auto files = corpus_files();
    ASSERT_GE(files.size(), 30u);
    for (const auto& path : files) {
        SCOPED_TRACE(path.filename().string());
        std::ifstream in(path, std::ios::binary);
        const std::string escaped((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string input = lbtest::decode_escaped(escaped);
        const int expected = std::stoi(path.filename().string().substr(0, 3));

        const auto o = lbtest::parse_request(input, corpus_limits(), /*finish_at_end=*/true);
        EXPECT_EQ(o.last, lb::http::ParseEvent::Error);
        EXPECT_EQ(o.error_status, expected) << o.error_detail;
    }
}
