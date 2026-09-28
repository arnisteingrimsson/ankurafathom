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
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
int main(int argc,char** argv) {
    try {
        require(argc==3,"expected fixture root and example");
        const auto root=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(root);
        std::ifstream input(argv[2]);Json doc;input>>doc;
        const auto path=root/"model.json",source=root/"agents.csv";
        doc["data"][0]["source"]="agents.csv";
        doc["parameters"][0]["value"]=99; // Table defaults must replace this value.
        doc["data"].push_back({{"id","assumptions"},{"source","assumptions.csv"},
            {"schema",{{"key_column","practice"},{"columns",Json::array({
                {{"name","practice"},{"type","string"},{"unit",""}},
                {{"name","gain"},{"type","f64"},{"unit","1"}}})}}},
            {"use",{{"kind","parameter_table"},{"key","disputes"},
                {"parameters",Json::array({{{"parameter","gain"},{"column","gain"}}})}}}});
        const auto assumptions=root/"assumptions.csv";
        rt::write_text_atomically(assumptions,"practice,gain\ndisputes,2\n");
        rt::write_text_atomically(path,doc.dump());
        rt::write_text_atomically(source,"key,work,ready,role\n18446744073709551615,3,false,partner\n18446744073709551614,1,true,analyst\n");
        if(!rt::data::available()) {
            try { (void)ir::load_file(path.string()); }
            catch(const ir::Error& e) {
                require(e.code=="DATA_UNAVAILABLE" && e.pointer=="/data/1/source","unavailable diagnostic");
                std::cout<<"Population IR reader availability verified\n";return 0;
            }
            throw std::runtime_error("reader unexpectedly available");
        }
        const auto original=ir::load_file(path.string());
        require(original.parameters.at("gain")==2 && original.parameter_data.size()==1,"parameter and population composition");
        const auto& binding=original.population_data.value();const auto& receipt=binding.receipt;
        require(binding.source==source.string() && binding.population=="people","source receipt");
        require(receipt.entries.size()==2 && receipt.entries[0].agent.id==0 && receipt.entries[1].agent.id==1,"canonical agent IDs");
        require(std::get<std::uint64_t>(receipt.entries[1].source_key)==UINT64_MAX,"source key truncated");
        require(std::get<std::string>(original.typed_abm->agents[0][2])=="analyst","key ordering did not determine records");
        require(receipt.file_hash.size()==64 && receipt.canonical_hash.size()==64,"missing hashes");
        auto copied=original;auto moved=std::move(copied);
        rt::write_text_atomically(assumptions,"practice,gain\ndisputes,7\n");
        rt::write_text_atomically(source,"key,work,ready,role\n18446744073709551615,3,false,partner\n18446744073709551614,2,true,analyst\n");
        const auto fresh=ir::load_file(path.string());
        require(fresh.parameters.at("gain")==7 && moved.parameters.at("gain")==2,"parameter snapshot mutated");
        require(fresh.parameter_data[0].canonical_hash!=moved.parameter_data[0].canonical_hash,"parameter hash unchanged");
        require(fresh.population_data->receipt.file_hash!=receipt.file_hash && fresh.population_data->receipt.canonical_hash!=receipt.canonical_hash,"reload hash unchanged");
        require(std::get<std::int64_t>(moved.typed_abm->agents[0][0])==1,"source change mutated snapshot");
        rt::write_text_atomically(source,"broken\n");
        rt::write_text_atomically(assumptions,"broken\n");
        rt::Experiment experiment{9,{},3};
        for(std::uint32_t i=0;i<64;++i) experiment.scenarios.push_back({i,i%2?std::map<std::string,double>{{"gain",3}}:std::map<std::string,double>{}});
        const auto callback=[&](const rt::Scenario& scenario,std::uint32_t replication,std::uint64_t seed) {
            const auto rows=ir::run(moved,scenario.parameters,seed,scenario.id,replication);
            std::vector<rt::Observation> result;
            for(const auto& row:rows) {
                if(row.time==2 && row.output_id=="first") require(row.value==(scenario.id%2?106:76),"independent phase recurrence mismatch");
                result.push_back({row.time,row.output_id,row.value});
            }
            return result;
        };
        const auto expected=rt::observations_csv(rt::run_experiment(experiment,callback,{1}));
        for(const std::size_t threads:{8,32}) require(rt::observations_csv(rt::run_experiment(experiment,callback,{threads}))==expected,"thread-dependent initialization");
        require(receipt.entries.size()==2,"execution changed initial receipt");
        std::cout<<"Population receipts, full-width source keys, copy/move ownership and 192 trajectories at 1/8/32 threads passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
