#include "ankurafathom/abm/network.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <numeric>
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream input(argv[1]); nlohmann::json plan; input>>plan;
        std::ofstream out(argv[2]); out<<nlohmann::json{{"plan",plan}}.dump()<<'\n';
        for(const auto& c:plan.at("cases")) {
            std::vector<std::uint64_t> vertices(c.at("n").get<std::size_t>()); std::iota(vertices.begin(),vertices.end(),0);
            for(std::uint32_t r=0;r<plan.at("replications").get<std::uint32_t>();++r) {
                const ankurafathom::abm::NetworkDraws draws{plan.at("seed"),plan.at("scenario"),r,plan.at("stream")};
                const auto kind=c.at("kind").get<std::string>();
                auto graph=kind=="erdos_renyi" ? ankurafathom::abm::erdos_renyi(vertices,c.at("probability"),draws) :
                    kind=="watts_strogatz" ? ankurafathom::abm::watts_strogatz(vertices,c.at("degree"),c.at("probability"),draws) :
                    ankurafathom::abm::barabasi_albert(vertices,c.at("m"),draws);
                out<<nlohmann::json{{"case",c.at("id")},{"replication",r},{"edges",graph.edges()}}.dump()<<'\n';
            }
        }
        if(!out) throw std::runtime_error("failed to write generator report");
        std::cout<<"generator ensembles written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
