#include "ankurafathom/abm/models/boids.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using Model=ankurafathom::abm::models::Boids;
Model::Parameters parameters(const nlohmann::json& p) {
    return {p.at("width"),p.at("height"),p.at("wrap"),p.at("dt"),p.at("vision"),p.at("separation_radius"),
            p.at("alignment"),p.at("cohesion"),p.at("separation"),p.at("max_acceleration"),p.at("max_speed"),p.at("softening"),p.at("bin_width")};
}
int main(int argc,char** argv) {
    try {
        if(argc!=4) throw std::invalid_argument("expected plan, reference and output paths");
        std::ifstream input(argv[1]); nlohmann::json plan; input>>plan;
        std::ifstream oracle(argv[2]); nlohmann::json reference; oracle>>reference;
        std::ofstream out(argv[3]); out<<nlohmann::json{{"plan",plan}}.dump()<<'\n';
        const auto reps=plan.at("replications").get<std::uint32_t>();
        const auto emit=[&](Model& model,const auto& c,std::uint32_t r,bool path) {
            for(const auto& tick:plan.at("ticks")) {
                while(model.ticks()<tick.template get<std::uint64_t>()) model.step();
                out<<nlohmann::json{{"case",c.at("id")},{"replication",r},{"tick",tick},{"agents",model.states()},{"path",path}}.dump()<<'\n';
            }
        };
        for(const auto& c:plan.at("cases")) for(std::uint32_t r=0;r<reps;++r) {
            auto model=Model::seeded(parameters(c.at("parameters")),c.at("n"),
                {plan.at("seed"),plan.at("scenario"),r,plan.at("position_stream"),plan.at("velocity_stream")});
            emit(model,c,r,false);
        }
        for(std::size_t i=0;i<plan.at("cases").size();++i) for(auto r:{0u,1u,reps-1}) {
            const auto& c=plan.at("cases").at(i);
            const auto& row=reference.at("rows").at((i*reps+r)*plan.at("ticks").size());
            if(row.at("case")!=c.at("id") || row.at("replication")!=r || row.at("tick")!=0) throw std::invalid_argument("Boids path initial identity differs");
            Model model(parameters(c.at("parameters")),row.at("agents").get<std::vector<Model::State>>()); emit(model,c,r,true);
        }
        if(!out) throw std::runtime_error("failed to write Boids observations");
        std::cout<<"Boids ensembles and paired paths written\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
