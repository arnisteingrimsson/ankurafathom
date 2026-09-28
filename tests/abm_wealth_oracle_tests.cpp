#include "ankurafathom/abm/models/wealth.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <numeric>
using namespace ankurafathom;
using Wealth=abm::models::WealthExchange;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream in(argv[1]); nlohmann::json plan; in>>plan;
        std::ofstream out(argv[2]); out<<nlohmann::json{{"plan",plan}}.dump()<<'\n';
        for(const auto& c:plan.at("cases")) {
            const auto n=c.at("n").get<std::size_t>();
            std::vector<std::uint64_t> vertices(n); std::iota(vertices.begin(),vertices.end(),0);
            std::optional<abm::CsrNetwork> network;
            const auto topology=c.at("topology").get<std::string>();
            if(topology!="mixed") {
                std::vector<abm::CsrNetwork::Edge> edges;
                if(topology=="ring") {
                    for(std::size_t i=0;i<n;++i) edges.emplace_back(i,(i+1)%n);
                } else if(topology=="star") {
                    for(std::size_t i=1;i<n;++i) edges.emplace_back(0,i);
                } else throw std::invalid_argument("unknown wealth topology");
                network.emplace(vertices,edges);
            }
            for(std::uint32_t r=0;r<plan.at("replications").get<std::uint32_t>();++r) {
                Wealth model(std::vector<std::int64_t>(n,c.at("initial").get<std::int64_t>()),network,
                    {plan.at("seed"),plan.at("scenario"),r,plan.at("order_stream"),plan.at("recipient_stream")});
                for(auto tick:plan.at("ticks")) {
                    while(model.sweeps()<tick.get<std::uint32_t>()) model.step();
                    out<<nlohmann::json{{"case",c.at("id")},{"replication",r},{"tick",tick},{"wealth",model.wealth()}}.dump()<<'\n';
                }
            }
        }
        if(!out) throw std::runtime_error("failed to write wealth observations");
        std::cout<<"wealth trajectories written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
