#include "ankurafathom/ir/model.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/runtime/outputs.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>

namespace ir=ankurafathom::ir;
namespace rt=ankurafathom::runtime;
using Json=nlohmann::json;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    try {
        require(argc==2,"expected fixture directory");
        const auto root=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(root);
        const auto model_path=root/"model.json",source=root/"values.csv";
        const Json binding={{"id","rates"},{"source","values.csv"},
            {"schema",{{"key_column","key"},{"columns",Json::array({
                {{"name","key"},{"type","u64"},{"unit",""}},
                {{"name","rate"},{"type","f64"},{"unit","kg/day"}}})}}},
            {"use",{{"kind","parameter_table"},{"key",UINT64_MAX},
                {"parameters",Json::array({{{"parameter","rate"},{"column","rate"}}})}}}};
        const Json document={{"ir_version","0.1"},{"name","snapshot"},
            {"time",{{"unit","day"},{"dt",1},{"horizon",4}}},
            {"parameters",Json::array({{{"id","rate"},{"value",1},{"unit","kg/day"}}})},
            {"components",Json::array({{{"id","total"},{"kind","stock"},{"init",0},{"unit","kg"}},
                {{"id","inflow"},{"kind","flow"},{"source",nullptr},{"destination","total"},{"expr","rate"},{"unit","kg/day"}}})},
            {"outputs",Json::array({{{"id","total_ts"},{"stock","total"}}})},
            {"data",Json::array({binding})}};
        rt::write_text_atomically(model_path,document.dump());
        rt::write_text_atomically(source,"key,rate\n18446744073709551615,2.5\n");
        if(!rt::data::available()) {
            try { (void)ir::load_file(model_path.string()); }
            catch(const ir::Error& e) {
                require(e.code=="DATA_UNAVAILABLE" && e.pointer=="/data/0/source","unavailable diagnostic");
                std::cout<<"IR reader availability verified\n";return 0;
            }
            throw std::runtime_error("reader unexpectedly available");
        }
        const auto model=ir::load_file(model_path.string());
        require(model.parameters.at("rate")==2.5,"file did not override literal");
        require(model.parameter_data.size()==1,"missing receipt");
        const auto& receipt=model.parameter_data.front();
        require(receipt.source==source.string() && receipt.id=="rates","source resolution");
        require(std::get<std::uint64_t>(receipt.source_key)==UINT64_MAX,"key truncated");
        require(receipt.values.at("rate")==2.5 && receipt.columns.at("rate")=="rate","mapping receipt");
        require(receipt.file_hash.size()==64 && receipt.canonical_hash.size()==64,"missing hashes");
        rt::write_text_atomically(source,"key,rate\n18446744073709551615,9\n");
        require(ir::run(model).back().value==10,"loaded snapshot reread source");
        require(ir::run(model,{{"rate",4}}).back().value==16,"scenario override precedence");
        const auto reloaded=ir::load_file(model_path.string());
        require(reloaded.parameters.at("rate")==9 && reloaded.parameter_data[0].file_hash!=receipt.file_hash &&
                reloaded.parameter_data[0].canonical_hash!=receipt.canonical_hash,"reload failed to capture changed data");
        // Failed loads cannot alter an already constructed model or its receipt.
        rt::write_text_atomically(source,"key,rate\n18446744073709551615,nan\n");
        try { (void)ir::load_file(model_path.string());throw std::runtime_error("invalid source accepted"); }
        catch(const ir::Error&) {}
        require(ir::run(model).back().value==10 && receipt.values.at("rate")==2.5,"failed reload mutated snapshot");
        rt::Experiment experiment{123,{},3};
        for(std::uint32_t i=0;i<64;++i) experiment.scenarios.push_back({i,i%2?std::map<std::string,double>{{"rate",4}}:std::map<std::string,double>{}});
        const auto callback=[&](const rt::Scenario& scenario,std::uint32_t replication,std::uint64_t seed) {
            const auto rows=ir::run(model,scenario.parameters,seed,scenario.id,replication);
            require(rows.back().value==(scenario.id%2?16:10),"ensemble binding or override changed");
            std::vector<rt::Observation> result;
            for(const auto& row:rows) result.push_back({row.time,row.output_id,row.value});
            return result;
        };
        const auto expected=rt::observations_csv(rt::run_experiment(experiment,callback,{1}));
        for(const std::size_t threads:{8,32})
            require(rt::observations_csv(rt::run_experiment(experiment,callback,{threads}))==expected,"thread-dependent bound trajectory");
        std::cout<<"IR snapshot, receipts, reload isolation and 192 trajectories at 1/8/32 threads passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
