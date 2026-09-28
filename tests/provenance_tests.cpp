#include "provenance.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include <bit>
#include <iostream>
namespace p=ankurafathom::ir::provenance;
namespace rt=ankurafathom::runtime;
using Json=nlohmann::json;
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("expected report path");
        const std::uint64_t bits[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,
            0x0010000000000000ULL,0x7fefffffffffffffULL,0xffefffffffffffffULL,0x3fb999999999999aULL};
        Json cases=Json::array();
        for(std::uint32_t variant=0;variant<32;++variant) {
            std::vector<rt::Trajectory> trajectories;
            Json encoded=Json::array();
            for(std::uint32_t i=0;i<3;++i) {
                rt::Trajectory trajectory{i*100,variant,{}};Json rows=Json::array();
                for(std::uint32_t j=0;j<8;++j) {
                    const auto b=bits[(j+variant)%8];const double time=j*.5;
                    const auto id=std::string("value,\"\nλ-")+std::to_string(j);
                    trajectory.observations.push_back({time,id,std::bit_cast<double>(b)});
                    rows.push_back({{"time_bits",std::bit_cast<std::uint64_t>(time)},{"id",id},{"value_bits",b}});
                }
                if(variant%2) { std::reverse(trajectory.observations.begin(),trajectory.observations.end());std::reverse(rows.begin(),rows.end()); }
                encoded.push_back({{"scenario",trajectory.scenario},{"replication",trajectory.replication},{"rows",rows}});
                trajectories.push_back(std::move(trajectory));
            }
            cases.push_back({{"trajectories",encoded},{"identity",p::result(trajectories)}});
        }
        for(int kind=0;kind<4;++kind) {
            std::vector<rt::Trajectory> bad{{0,0,{{0,"a",1}}}};
            if(kind==0) bad.push_back(bad[0]);
            if(kind==1) bad[0].scenario=65536;
            if(kind==2) bad[0].observations.push_back(bad[0].observations[0]);
            if(kind==3) bad[0].observations[0].value=std::numeric_limits<double>::infinity();
            bool rejected=false;try { (void)p::result(bad); } catch(const std::exception&) { rejected=true; }
            if(!rejected) throw std::runtime_error("invalid numeric identity accepted");
        }
        rt::write_text_atomically(argv[1],Json{{"cases",cases},{"empty",p::result({})}}.dump(2)+"\n");
        std::cout<<"32 ordered numeric identities and four invalid controls passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
