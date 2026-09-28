#pragma once
#include "ankurafathom/ir/model.hpp"
#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/runtime/outputs.hpp"
#include <nlohmann/json.hpp>
namespace ankurafathom::ir::provenance {
using Json=nlohmann::json;
Json read_input(const std::string& path,InputIdentity& identity,const std::string& pointer);
Json parse_input(std::string_view bytes,InputIdentity& identity,const std::string& pointer);
Json inputs(const Model&,const std::optional<runtime::Experiment>&,const std::optional<InputIdentity>&,
            std::uint64_t seed,std::size_t threads);
Json result(std::span<const runtime::Trajectory>);
Json manifest(Json inputs,Json result,const std::string& output,runtime::OutputFormat);
Json memory_manifest(Json inputs,Json result);
Json read_manifest(const std::string& path);
Json validate_manifest(Json value);
void require_equal(const Json& actual,const Json& expected,const std::string& pointer);
void publish(const std::string& manifest_path,const Json& manifest,const std::string& output,
             runtime::OutputFormat,std::span<const runtime::Trajectory>,bool single);
bool same_destination(const std::string&,const std::string&);
struct Bundle {
    Json manifest;
    Model model;
    std::optional<runtime::Experiment> experiment;
    std::optional<InputIdentity> experiment_input;
    std::filesystem::path root;
    std::vector<std::string> files;
};
Bundle load_bundle(const std::string& directory,bool require_build=true);
Json create_bundle(const std::string& manifest_path,const std::string& directory);
bool within_bundle(const std::string& path,const std::filesystem::path& root);
}
