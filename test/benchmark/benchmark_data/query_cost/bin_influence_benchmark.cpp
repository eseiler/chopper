// ---------------------------------------------------------------------------------------------------
// Copyright (c) 2006-2023, Knut Reinert & Freie Universität Berlin
// Copyright (c) 2016-2023, Knut Reinert & MPI für molekulare Genetik
// This file may be used, modified and/or redistributed under the terms of the 3-clause BSD-License
// shipped with this file and also available at: https://github.com/seqan/chopper/blob/main/LICENSE.md
// ---------------------------------------------------------------------------------------------------

// clang-format off
// https://github.com/seqan/raptor/blob/main/test/performance/bin_influence_benchmark.cpp

#include <benchmark/benchmark.h>

#include <filesystem>

#include <seqan3/alphabet/nucleotide/dna4.hpp>
#include <seqan3/core/algorithm/detail/execution_handler_parallel.hpp>
#include <seqan3/search/dream_index/interleaved_bloom_filter.hpp>
#include <seqan3/search/views/minimiser_hash.hpp>
#include <seqan3/test/performance/sequence_generator.hpp>
#include <seqan3/utility/views/slice.hpp>
#include <seqan3/utility/views/zip.hpp>

#include <raptor/adjust_seed.hpp>

#include <hibf/build/bin_size_in_bits.hpp>

#define USE_UNIT_TEST_PARAMETERS 0

static constexpr size_t operator""_MiB(unsigned long long int number)
{
    return number << 23;
}

static constexpr size_t operator""_GiB(unsigned long long int number)
{
    return number << 33;
}

#if USE_UNIT_TEST_PARAMETERS
static constexpr size_t const genome_size{1ULL << 19};
static constexpr size_t const read_size{100};
static constexpr size_t const read_count{128};
static constexpr size_t const max_ibf_size{16_MiB};
static constexpr size_t const construct_threads{2};
#else
static constexpr size_t const genome_size{1ULL << 30};
static constexpr size_t const read_size{250};
static constexpr size_t const read_count{1ULL << 20};
static constexpr size_t const max_ibf_size{128_GiB};
static constexpr size_t const construct_threads{64};
#endif

static constexpr size_t const hash_num{2u};
static constexpr size_t const window_size{32};
static constexpr size_t const kmer_size{32};
static std::filesystem::path tmp_index_storage{"/scratch/seiler/bin_influence"};
static inline auto hash_adaptor = seqan3::views::minimiser_hash(seqan3::ungapped{kmer_size},
                                                                seqan3::window_size{window_size},
                                                                seqan3::seed{raptor::adjust_seed(kmer_size)});

static std::vector<seqan3::dna4> const genome{seqan3::test::generate_sequence<seqan3::dna4>(genome_size, 0, 0)};
static std::vector<std::vector<seqan3::dna4>> const reads{
    [](auto const & genome)
    {
        std::vector<std::vector<seqan3::dna4>> result(read_count);
        size_t i{};
        for (auto && read_start :
             seqan3::test::generate_numeric_sequence<size_t>(read_count, 0, genome_size - read_size - 1, 0))
        {
            auto v = genome | seqan3::views::slice(read_start, read_start + read_size);
            result[i++].assign(v.begin(), v.end());
        }
        return result;
    }(genome)};

using ibf_t = seqan3::interleaved_bloom_filter<seqan3::data_layout::uncompressed>;

static std::vector<size_t> cardinality(size_t const bin_count, auto && hash_adaptor)
{
    std::vector<size_t> cardinalities(bin_count);

    size_t const chunked_genome_size{(genome_size + bin_count - 1) / bin_count};
    auto chunked_genomes = genome | seqan3::views::chunk(chunked_genome_size);

    size_t const workload_size = std::clamp<size_t>(std::bit_ceil(bin_count / construct_threads), 8u, 64u);
    auto workload = seqan3::views::zip(chunked_genomes, std::views::iota(0u)) | seqan3::views::chunk(workload_size);

    auto worker = [&cardinalities, &hash_adaptor](auto && payload, auto &&)
    {
        for (auto && [sequence, bin_number] : payload)
            cardinalities[bin_number] += std::ranges::distance(sequence | hash_adaptor);
    };

    seqan3::detail::execution_handler_parallel executioner{construct_threads};
    executioner.bulk_execute(std::move(worker), std::move(workload), []() {});

    return cardinalities;
}

static ibf_t construct_ibf(size_t const bin_count, auto && hash_adaptor, double const fpr)
{
    size_t const bin_size{seqan::hibf::build::bin_size_in_bits({.fpr = fpr, .hash_count = hash_num, .elements = cardinality(bin_count, hash_adaptor)});

    if (bin_size * bin_count > max_ibf_size)
        throw std::runtime_error{"Resulting IBF would be too big. " + std::to_string(bin_size * bin_count)};

    ibf_t ibf{seqan3::bin_count{bin_count}, seqan3::bin_size{bin_size}, seqan3::hash_function_count{hash_num}};

    size_t const chunked_genome_size{(genome_size + bin_count - 1) / bin_count};
    auto chunked_genomes = genome | seqan3::views::chunk(chunked_genome_size);

    size_t const workload_size = std::clamp<size_t>(std::bit_ceil(bin_count / construct_threads), 8u, 64u);
    auto workload = seqan3::views::zip(chunked_genomes, std::views::iota(0u)) | seqan3::views::chunk(workload_size);

    auto worker = [&ibf, &hash_adaptor](auto && payload, auto &&)
    {
        for (auto && [sequence, bin_number] : payload)
            for (auto && hash : sequence | hash_adaptor)
                ibf.emplace(hash, seqan3::bin_index{bin_number});
    };

    seqan3::detail::execution_handler_parallel executioner{construct_threads};
    executioner.bulk_execute(std::move(worker), std::move(workload), []() {});

    return ibf;
}

static std::filesystem::path get_index_path(size_t const bin_count, double const fpr)
{
    std::filesystem::path index_path{tmp_index_storage};
    index_path /= std::to_string(window_size) + "_" + std::to_string(kmer_size) + "_" + std::to_string(fpr) + "_"
                + std::to_string(bin_count) + ".ibf";
    return index_path;
}

static void bulk_count(benchmark::State & state, double && fpr)
{
    size_t const bin_count = static_cast<size_t>(state.range(0));
    std::filesystem::path const index_path = get_index_path(bin_count, fpr);
    ibf_t ibf{};

    if (std::filesystem::exists(index_path))
    {
        std::ifstream is{index_path, std::ios::binary};
        cereal::BinaryInputArchive iarchive{is};
        iarchive(ibf);
    }
    else
    {
        try
        {
            ibf = std::move(construct_ibf(bin_count, hash_adaptor, fpr));
            std::ofstream os{index_path, std::ios::binary};
            cereal::BinaryOutputArchive oarchive{os};
            oarchive(ibf);
        }
        catch (std::runtime_error const & e)
        {
            state.SkipWithError(e.what());
            return;
        }
    }

    auto agent = ibf.counting_agent<uint16_t>();
    std::vector<uint64_t> minimiser{};
    minimiser.reserve(read_size - window_size + 1);

    for (auto _ : state)
    {
        for (auto const & query : reads)
        {
            auto view = query | hash_adaptor | std::views::common;
            minimiser.assign(view.begin(), view.end());

            auto & result = agent.bulk_count(minimiser);
            benchmark::DoNotOptimize(result);
        }
    }
}

#if USE_UNIT_TEST_PARAMETERS
BENCHMARK_CAPTURE(bulk_count, "0.05", 0.05)->RangeMultiplier(2)->Range(64, 128);
#else
BENCHMARK_CAPTURE(bulk_count, "0.0001", 0.0001)->RangeMultiplier(2)->Range(64, 65536);
BENCHMARK_CAPTURE(bulk_count, "0.0005", 0.0005)->RangeMultiplier(2)->Range(64, 65536);
BENCHMARK_CAPTURE(bulk_count, "0.0025", 0.0025)->RangeMultiplier(2)->Range(64, 65536);
BENCHMARK_CAPTURE(bulk_count, "0.0125", 0.0125)->RangeMultiplier(2)->Range(64, 65536);
BENCHMARK_CAPTURE(bulk_count, "0.05", 0.05)->RangeMultiplier(2)->Range(64, 65536);
BENCHMARK_CAPTURE(bulk_count, "0.0625", 0.0625)->RangeMultiplier(2)->Range(64, 65536);
BENCHMARK_CAPTURE(bulk_count, "0.3125", 0.3125)->RangeMultiplier(2)->Range(64, 65536);
#endif

BENCHMARK_MAIN();
