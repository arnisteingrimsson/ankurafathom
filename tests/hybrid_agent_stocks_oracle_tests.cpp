#include "ankurafathom/hybrid/agent_stocks.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
struct Person {};
using Model=ankurafathom::hybrid::AgentStocks<Person>;
using E=Model::Endpoint;
using json=nlohmann::json;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream input(argv[1]);const auto plan=json::parse(input);
        std::ofstream output(argv[2]);output.exceptions(std::ios::failbit|std::ios::badbit);
        std::size_t count=0;
        for(const auto& c:plan.at("cases")) for(double dt:plan.at("steps")) {
            const auto initial=c.at("initial").get<std::vector<double>>();
            const auto rates=c.at("rates").get<std::vector<double>>();
            const double reservoir=c.at("reservoir"),supply=c.at("supply_rate");
            Model::Store store(3,{{"work",ankurafathom::des::FieldKind::real},
                                  {"rate",ankurafathom::des::FieldKind::real}});
            for(std::size_t i=0;i<initial.size();++i) store.spawn({initial[i],rates.at(i)});
            Model model(std::move(store),{{0}},{{"reservoir",reservoir},{"completed",0},{"exposure",0}});
            model.add_agent_flow(E::agent(0),E::global(1),[](auto ref,const auto& s,const auto&,double) {
                return std::get<double>(s.field(ref,0))*std::get<double>(s.field(ref,1));
            });
            model.add_agent_flow(E::global(0),E::agent(0),[supply,n=initial.size()](auto,const auto&,const auto& g,double) {
                return supply*g[0]/n;
            });
            model.add_global_flow(E::boundary(),E::global(2),[](const auto& s,const auto&,double) { return Model::sum(s,0); });
            for(double time:plan.at("times")) {
                while(model.time()<time) model.step(dt);
                std::vector<double> agents;
                for(std::size_t i=0;i<initial.size();++i) agents.push_back(std::get<double>(model.store().field({3,i},0)));
                output<<json{{"case",c.at("id")},{"dt",dt},{"time",model.time()},
                             {"agents",agents},{"sum",model.sum(0)},{"globals",model.global_state()}}.dump()<<'\n';
                ++count;
            }
        }
        std::cout<<count<<" agent-stock observations generated\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
