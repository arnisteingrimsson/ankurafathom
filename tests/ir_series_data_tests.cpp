#include "ankurafathom/ir/model.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/runtime/outputs.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

namespace ir=ankurafathom::ir;
namespace rt=ankurafathom::runtime;
using Json=nlohmann::json;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    try {
        require(argc==3,"expected fixture directory and example path");
        const auto root=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(root);
        std::ifstream input(argv[2]);Json document;input>>document;
        const auto path=root/"model.json",source=root/"series.csv";
        document["data"][0]["source"]="series.csv";
        rt::write_text_atomically(path,document.dump());
        rt::write_text_atomically(source,"time,factor\n4,5\n0,1\n");
        if(!rt::data::available()) {
            try { (void)ir::load_file(path.string()); }
            catch(const ir::Error& e) {
                require(e.code=="DATA_UNAVAILABLE" && e.pointer=="/data/0/source","unavailable diagnostic");
                std::cout<<"Series IR reader availability verified\n";return 0;
            }
            throw std::runtime_error("reader unexpectedly available");
        }
        const auto original=ir::load_file(path.string());
        const auto& receipt=original.series_data.at(0);
        require(receipt.id=="seasonality" && receipt.source==source.string(),"source receipt");
        require(receipt.series.file_hash().size()==64 && receipt.series.canonical_hash().size()==64,"missing hashes");
        auto copy=original;
        auto moved=std::move(copy);
        rt::write_text_atomically(source,"time,factor\n0,9\n4,9\n");
        const auto fresh=ir::load_file(path.string());
        require(fresh.series_data[0].series.file_hash()!=receipt.series.file_hash() &&
                fresh.series_data[0].series.canonical_hash()!=receipt.series.canonical_hash(),"reload hashes unchanged");
        require(fresh.series_data[0].series.value_at(2)==9 && receipt.series.value_at(2)==3,"snapshot mutated");
        rt::write_text_atomically(source,"broken\n");
        rt::Experiment experiment{12,{},3};
        for(std::uint32_t i=0;i<64;++i) experiment.scenarios.push_back({i,i%2?std::map<std::string,double>{{"rate",4}}:std::map<std::string,double>{}});
        const auto callback=[&](const rt::Scenario& scenario,std::uint32_t replication,std::uint64_t seed) {
            const auto rows=ir::run(moved,scenario.parameters,seed,scenario.id,replication);
            std::vector<rt::Observation> result;
            for(const auto& row:rows) {
                if(row.time==4 && row.output_id=="total_ts") require(row.value==(scenario.id%2?48:24),"RK4 stage sampling or override failed");
                result.push_back({row.time,row.output_id,row.value});
            }
            return result;
        };
        const auto expected=rt::observations_csv(rt::run_experiment(experiment,callback,{1}));
        for(const std::size_t threads:{8,32})
            require(rt::observations_csv(rt::run_experiment(experiment,callback,{threads}))==expected,"thread-dependent series trajectory");
        moved.integrator=ankurafathom::sd::Integrator::euler;
        for(const auto& row:ir::run(moved)) if(row.time==4 && row.output_id=="total_ts")
            require(row.value==20,"Euler did not use left-boundary sampling");
        std::cout<<"Series receipts, copied/moved snapshot ownership and 192 trajectories at 1/8/32 threads passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
