#include "ankurafathom/abm/models/schelling.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using Model=ankurafathom::abm::models::Schelling;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream in(argv[1]); nlohmann::json plan; in>>plan;
        std::ofstream out(argv[2]); out<<nlohmann::json{{"plan",plan}}.dump()<<'\n';
        for(const auto& c:plan.at("cases")) {
            const Model::Parameters parameters{c.at("width"),c.at("height"),c.at("wrap"),c.at("moore"),c.at("numerator"),c.at("denominator")};
            std::vector<Model::State> initial;
            for(const auto& a:c.at("agents")) initial.push_back({a.at(0),{a.at(1),a.at(2)}});
            for(std::uint32_t r=0;r<plan.at("replications").get<std::uint32_t>();++r) {
                Model model(parameters,initial,{plan.at("seed"),plan.at("scenario"),r,plan.at("order_stream"),plan.at("relocation_stream")});
                for(const auto& tick:plan.at("ticks")) {
                    while(model.sweeps()<tick.get<std::uint32_t>()) model.step();
                    auto states=nlohmann::json::array();
                    for(const auto& a:model.states()) states.push_back({a.group,a.position.x,a.position.y});
                    out<<nlohmann::json{{"case",c.at("id")},{"replication",r},{"tick",tick},{"agents",states},{"moves",model.last_moves()}}.dump()<<'\n';
                }
            }
        }
        if(!out) throw std::runtime_error("failed to write Schelling observations");
        std::cout<<"Schelling trajectories written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
