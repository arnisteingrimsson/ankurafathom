#include "ankurafathom/runtime/design.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using json=nlohmann::json;
namespace rt=ankurafathom::runtime;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan/output paths");
        std::ifstream in(argv[1]);const auto plan=json::parse(in);std::ofstream out(argv[2]);out.exceptions(std::ios::badbit|std::ios::failbit);
        std::size_t configurations=0;
        for(const auto& c:plan.at("cases")) for(const std::string order:plan.at("axis_orders")) {
            std::vector<rt::Scenario> scenarios;
            if(c.at("kind")=="grid") {
                std::vector<rt::GridAxis> axes;
                for(auto it=c.at("axes").begin();it!=c.at("axes").end();++it) axes.push_back({it.key(),it.value().get<std::vector<double>>()});
                if(order=="reverse") std::reverse(axes.begin(),axes.end());
                scenarios=rt::expand_grid(std::move(axes),c.at("first_id"));
            } else {
                std::vector<rt::ParameterBounds> bounds;
                for(auto it=c.at("bounds").begin();it!=c.at("bounds").end();++it) bounds.push_back({it.key(),it.value()[0],it.value()[1]});
                if(order=="reverse") std::reverse(bounds.begin(),bounds.end());
                if(c.at("kind")=="lhs") scenarios=rt::expand_lhs(std::move(bounds),c.at("count"),c.at("design_seed"),c.at("first_id"));
                else scenarios=rt::expand_sobol(std::move(bounds),c.at("count"),c.at("first_id"));
            }
            json values=json::array();
            for(const auto& scenario:scenarios) {
                json coordinates=json::array();for(const auto& [name,value]:scenario.parameters) { (void)name;coordinates.push_back(value); }
                values.push_back(json::array({scenario.id,coordinates}));
            }
            out<<json{{"case",c.at("id")},{"axis_order",order},{"scenarios",values}}.dump()<<'\n';++configurations;
        }
        std::cout<<configurations<<" native scenario designs generated\n";
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
