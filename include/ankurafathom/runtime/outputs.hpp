#pragma once
#include "ankurafathom/runtime/experiment.hpp"
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace arrow { class Table; }

namespace ankurafathom::runtime {
enum class OutputFormat { csv, parquet, arrow_ipc };
OutputFormat parse_output_format(std::string_view name);
OutputFormat infer_output_format(const std::filesystem::path& path);
bool arrow_output_available() noexcept;

// Original manifest JSON, checked against the complete trajectory set before
// publication. Replay retains the original manifest and its run identity.
struct OutputProvenance { std::string manifest_json; };

// Observations retain runtime order; trajectory addresses must increase strictly.
// All formats reject invalid or duplicate observations before publishing anything.
std::string observations_csv(std::span<const Trajectory> trajectories, bool single_run=false,
                             const OutputProvenance* provenance=nullptr);
std::shared_ptr<arrow::Table> observation_table(std::span<const Trajectory> trajectories,
                                               const OutputProvenance* provenance=nullptr);
void write_observations(const std::filesystem::path& path, OutputFormat format,
                        std::span<const Trajectory> trajectories, bool single_run=false,
                        const OutputProvenance* provenance=nullptr);
// Read-only verification of schema 0.2 observations against an explicit manifest.
// Requires neither original model/data files nor the original execution build.
// Throws on schema, metadata, lineage or exact numeric identity mismatch.
void verify_observations(const std::filesystem::path& path, OutputFormat format,
                         const OutputProvenance& provenance);
// Verify using the embedded provenance from the same decoded binary table.
// Returns the verified manifest; this checks internal consistency, not authenticity.
// CSV has no embedded manifest and is unsupported here.
OutputProvenance verify_embedded_observations(const std::filesystem::path& path, OutputFormat format);
} // namespace ankurafathom::runtime
