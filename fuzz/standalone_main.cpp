// The standalone driver for a fuzz harness on a compiler without libFuzzer
// (MSVC, or an AppleClang without it). It supplies `main`, feeds each harness a
// deterministic pseudo-random stream, and can replay a corpus of files, so a finding
// can be reproduced the same way on every platform.
//
//   ioscpp_fuzz_plist                       # 10000 random inputs
//   ioscpp_fuzz_plist -runs=50000 -max_len=8192
//   ioscpp_fuzz_plist corpus/seed-1           # replay one input
//
// The `-runs=` and `-max_len=` spellings match libFuzzer's, so the same command
// line works whichever engine built the harness.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);

namespace
{

/// A deterministic xorshift64 generator, so a standalone run is reproducible.
std::uint64_t next(std::uint64_t &state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

int main(int argc, char **argv)
{
    std::size_t runs = 10000;
    std::size_t max_len = 4096;
    std::vector<std::filesystem::path> corpus;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg.starts_with("-runs="))
        {
            runs = std::stoull(arg.substr(6));
        }
        else if (arg.starts_with("-max_len="))
        {
            max_len = std::stoull(arg.substr(9));
        }
        else
        {
            corpus.emplace_back(arg);
        }
    }

    std::size_t count = 0;
    const std::uint8_t empty = 0;

    for (const std::filesystem::path &path : corpus)
    {
        std::vector<std::uint8_t> bytes = read_file(path);
        LLVMFuzzerTestOneInput(bytes.empty() ? &empty : bytes.data(), bytes.size());
        ++count;
    }

    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    std::vector<std::uint8_t> bytes(max_len + 1);
    for (std::size_t i = 0; i < runs; ++i)
    {
        const std::size_t size = static_cast<std::size_t>(next(state) % (max_len + 1));
        for (std::size_t j = 0; j < size; ++j)
        {
            bytes[j] = static_cast<std::uint8_t>(next(state));
        }
        LLVMFuzzerTestOneInput(bytes.data(), size);
        ++count;
    }

    std::fprintf(stderr, "ran %zu inputs\n", count);
    return 0;
}
