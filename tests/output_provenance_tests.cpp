#include "provenance.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include "ankurafathom/runtime/sha256.hpp"
#include <bit>
#include <fstream>
#include <iostream>
namespace p=ankurafathom::ir::provenance;
namespace rt=ankurafathom::runtime;
using Json=nlohmann::json;
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void seal(Json& manifest) { manifest.erase("id");manifest["id"]=rt::sha256(manifest.dump()); }
int main(int argc,char** argv) {
    try {
        require(argc==3,"expected fixture directory and model path");
        const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
        const auto model=ankurafathom::ir::load_file(argv[2]);
        const auto input=p::inputs(model,std::nullopt,std::nullopt,42,1);
        std::vector<std::pair<std::string,rt::OutputFormat>> formats{{"csv",rt::OutputFormat::csv}};
        if(rt::arrow_output_available()) {
            formats.emplace_back("parquet",rt::OutputFormat::parquet);
            formats.emplace_back("arrow",rt::OutputFormat::arrow_ipc);
        }
        Json report=Json::array();std::size_t controls=0;
        for(const auto& kind:{"mixed","empty","single"}) {
            const bool single=std::string(kind)=="single",empty=std::string(kind)=="empty";
            auto inputs=input;inputs["scenarios"]=Json::array();inputs["replications"]=single?1:2;
            std::vector<rt::Trajectory> trajectories;
            Json expected=Json::array();
            for(const std::uint32_t id:single?std::vector<std::uint32_t>{0}:std::vector<std::uint32_t>{0,19,65535}) {
                const Json parameters{{"decay_rate",0.2},{"zero",-0.0},{"escaped,\"\nλ",id+0.5}};
                inputs["scenarios"].push_back({{"id",id},{"overrides",{{"zero",-0.0}}},{"effective_parameters",parameters}});
                for(std::uint32_t replication=0;replication<(single?1U:2U);++replication) {
                    rt::Trajectory trajectory{id,replication,{}};Json rows=Json::array();
                    if(!empty && id!=19) {
                        const std::uint64_t bits[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
                            0x0010000000000000ULL,0x7fefffffffffffffULL,0xffefffffffffffffULL,0x3fb999999999999aULL};
                        for(std::size_t i=0;i<8;++i) {
                            const double time=i==0?-0.0:static_cast<double>(i);
                            const auto name=std::string("out,\"\nλ-")+std::to_string(i);
                            trajectory.observations.push_back({time,name,std::bit_cast<double>(bits[i])});
                            rows.push_back({{"time_bits",std::bit_cast<std::uint64_t>(time)},{"id",name},{"value_bits",bits[i]}});
                        }
                    }
                    expected.push_back({{"scenario",id},{"replication",replication},{"rows",rows}});
                    trajectories.push_back(std::move(trajectory));
                }
            }
            const auto manifest=p::manifest(inputs,p::result(trajectories),(root/(std::string(kind)+".csv")).string(),rt::OutputFormat::csv);
            const rt::OutputProvenance lineage{manifest.dump()};
            rt::write_text_atomically(root/(std::string(kind)+".json"),manifest.dump());
            for(const auto& [suffix,format]:formats)
                rt::write_observations(root/(std::string(kind)+"."+suffix),format,trajectories,single,&lineage);
            report.push_back({{"kind",kind},{"trajectories",expected}});
            if(single || empty) continue;
            // A checksum alone cannot justify attaching metadata: all association
            // failures below are resealed, except the explicit checksum control.
            std::vector<Json> bad;
            auto changed=[&](auto mutate) { auto copy=manifest;mutate(copy);seal(copy);bad.push_back(std::move(copy)); };
            changed([](Json& x){x["manifest_version"]="0.1";});
            changed([](Json& x){x["output"]["schema_version"]="0.1";});
            changed([](Json& x){x["result"]["sha256"]=std::string(64,'0');});
            changed([](Json& x){x["result"]["rows"]=0;});
            changed([](Json& x){x["result"]["trajectories"]=0;});
            changed([](Json& x){x["result"]["rows"]=x["result"]["rows"].get<double>();});
            changed([](Json& x){x["result"]["trajectories"]=x["result"]["trajectories"].get<double>();});
            changed([](Json& x){x["result"]["encoding"]="unknown";});
            for(const Json& n:{Json(-1),Json(0),Json(65537),Json(1.5),Json("2")})
                changed([&](Json& x){x["inputs"]["replications"]=n;});
            changed([](Json& x){x["inputs"]["scenarios"]=Json::array();});
            changed([](Json& x){x["inputs"]["scenarios"][0]["id"]=1;});
            changed([](Json& x){x["inputs"]["scenarios"][1]["id"]=0;});
            changed([](Json& x){std::swap(x["inputs"]["scenarios"][0],x["inputs"]["scenarios"][1]);});
            for(const Json& n:{Json(-1),Json(65536),Json(1.5),Json("0")})
                changed([&](Json& x){x["inputs"]["scenarios"][0]["id"]=n;});
            changed([](Json& x){x["inputs"]["scenarios"][0]["effective_parameters"]=Json::array();});
            changed([](Json& x){x["inputs"]["scenarios"][0]["effective_parameters"]["zero"]=nullptr;});
            changed([](Json& x){x["inputs"]["scenarios"][0]["effective_parameters"]["zero"]=true;});
            changed([](Json& x){x["inputs"]["scenarios"][0]["effective_parameters"][""]=1;});
            changed([](Json& x){x["inputs"]["scenarios"][0]["overrides"]["absent"]=1;});
            changed([](Json& x){x["inputs"]["scenarios"][0]["overrides"]["zero"]=0.0;});
            changed([](Json& x){x["inputs"]["scenarios"][0]["overrides"]=nullptr;});
            changed([](Json& x){x["inputs"].erase("scenarios");});
            auto checksum=manifest;checksum["id"]=std::string(64,'0');bad.push_back(checksum);
            std::vector<std::string> invalid{"{", "null"};
            for(const auto& item:bad) invalid.push_back(item.dump());
            for(const auto& text:invalid) for(const auto& [suffix,format]:formats) {
                const auto sentinel=root/("sentinel."+suffix);rt::write_text_atomically(sentinel,"KEEP");
                const rt::OutputProvenance wrong{text};bool rejected=false;
                try { rt::write_observations(sentinel,format,trajectories,false,&wrong); }
                catch(const std::exception&) { rejected=true; }
                require(rejected,"invalid output provenance accepted");
                std::ifstream file(sentinel);std::string contents((std::istreambuf_iterator<char>(file)),{});
                require(contents=="KEEP","invalid provenance replaced existing result");++controls;
            }
            // Intact metadata cannot be reused after a value's sign bit changes.
            trajectories[0].observations[0].value=-0.0;
            bool rejected=false;
            try { (void)rt::observations_csv(trajectories,false,&lineage); }
            catch(const std::exception&) { rejected=true; }
            require(rejected,"numeric drift accepted by output provenance");++controls;
        }
        for(const auto& entry:std::filesystem::directory_iterator(root))
            require(!entry.path().filename().string().starts_with(".fathom-"),"temporary result leaked");
        rt::write_text_atomically(root/"expected.json",report.dump());
        std::cout<<"Three lineage fixtures and "<<controls<<" rejection/publication controls passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
