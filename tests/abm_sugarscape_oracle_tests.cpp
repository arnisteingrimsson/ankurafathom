#include "ankurafathom/abm/models/sugarscape.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using Model=ankurafathom::abm::models::Sugarscape;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream in(argv[1]); nlohmann::json plan; in>>plan;
        std::ofstream out(argv[2]); out<<nlohmann::json{{"plan",plan}}.dump()<<'\n';
        for(const auto& c:plan.at("cases")) {
            const Model::Parameters parameters{c.at("width"),c.at("height"),c.at("wrap"),c.at("regrowth")};
            std::vector<Model::State> initial;
            for(const auto& a:c.at("agents")) initial.push_back({a.at(0),a.at(1),a.at(2),{a.at(3),a.at(4)}});
            for(std::uint32_t r=0;r<plan.at("replications").get<std::uint32_t>();++r) {
                Model model(parameters,c.at("capacity"),c.at("sugar"),initial,
                            {plan.at("seed"),plan.at("scenario"),r,plan.at("order_stream"),plan.at("movement_stream")});
                for(const auto& tick:plan.at("ticks")) {
                    while(model.sweeps()<tick.get<std::uint32_t>()) model.step();
                    auto agents=nlohmann::json::array();
                    for(auto id:model.active_ids()) {
                        const auto a=model.state(id);
                        agents.push_back({id,a.reserve,a.metabolism,a.vision,a.position.x,a.position.y});
                    }
                    const auto l=model.ledger();
                    out<<nlohmann::json{{"case",c.at("id")},{"replication",r},{"tick",tick},
                        {"agents",agents},{"sugar",model.sugar()},
                        {"ledger",{{"initial",l.initial},{"regrown",l.regrown},{"consumed",l.consumed},{"deaths",l.deaths}}}}.dump()<<'\n';
                }
            }
        }
        if(!out) throw std::runtime_error("failed to write Sugarscape observations");
        std::cout<<"Sugarscape trajectories written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
