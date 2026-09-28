#include "ankurafathom/abm/space.hpp"
#include "ankurafathom/abm/network.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

namespace {
using Json=nlohmann::json;
using namespace ankurafathom::abm;
using Id=std::uint64_t;
GridPoint grid_point(const Json& value) { return {value.at(0).get<std::int64_t>(),value.at(1).get<std::int64_t>()}; }
std::vector<CsrNetwork::Edge> edges(const Json& values) {
    std::vector<CsrNetwork::Edge> result;
    for(const auto& value:values) result.emplace_back(value.at(0).get<Id>(),value.at(1).get<Id>());
    return result;
}
Json run(const Json& spec) {
    Json phases=Json::array();
    const auto kind=spec.at("kind").get<std::string>();
    if(kind=="grid") {
        GridSpace space(spec.at("width"),spec.at("height"),spec.at("wrap"));
        for(const auto& agent:spec.at("agents")) space.add(agent.at(0).get<Id>(),grid_point(agent.at(1)));
        for(const auto& phase:spec.at("phases")) {
            std::vector<std::pair<Id,GridPoint>> moves;
            for(const auto& move:phase) moves.emplace_back(move.at(0).get<Id>(),grid_point(move.at(1)));
            space.move_many(moves);
            Json rows=Json::array();
            for(const auto& query:spec.at("queries"))
                rows.push_back(space.neighbors(query.at(0).get<Id>(),query.at(1).get<Id>(),query.at(2),query.at(3)));
            phases.push_back(rows);
        }
    } else if(kind=="continuous") {
        ContinuousSpace space(spec.at("lower").get<ContinuousSpace::Point>(),spec.at("upper").get<ContinuousSpace::Point>(),
                              spec.at("bin_width"),spec.at("wrap"));
        for(const auto& agent:spec.at("agents")) space.add(agent.at(0).get<Id>(),agent.at(1).get<ContinuousSpace::Point>());
        for(const auto& phase:spec.at("phases")) {
            std::vector<std::pair<Id,ContinuousSpace::Point>> moves;
            for(const auto& move:phase) moves.emplace_back(move.at(0).get<Id>(),move.at(1).get<ContinuousSpace::Point>());
            space.move_many(moves);
            Json rows=Json::array();
            for(const auto& query:spec.at("queries"))
                rows.push_back(space.neighbors(query.at(0).get<Id>(),query.at(1).get<double>(),query.at(2)));
            phases.push_back(rows);
        }
    } else if(kind=="network") {
        CsrNetwork network(spec.at("vertices").get<std::vector<Id>>(),edges(spec.at("edges")),spec.at("directed"));
        for(const auto& phase:spec.at("phases")) {
            network.edit(edges(phase.at("add")),edges(phase.at("remove")));
            Json rows=Json::array();
            for(const auto& query:spec.at("queries")) {
                const auto neighbors=network.neighbors(query.get<Id>());
                rows.push_back(std::vector<Id>(neighbors.begin(),neighbors.end()));
            }
            phases.push_back(rows);
        }
    } else throw std::invalid_argument("unknown foundation oracle case");
    return {{"id",spec.at("id")},{"phases",phases}};
}
}

int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream input(argv[1]); Json plan; input>>plan;
        if(plan.at("version")!=1) throw std::invalid_argument("unsupported foundation plan");
        Json report={{"version",1},{"cases",Json::array()}};
        for(const auto& spec:plan.at("cases")) report["cases"].push_back(run(spec));
        std::ofstream output(argv[2]); output<<report.dump(2)<<'\n';
        if(!output.good()) throw std::runtime_error("cannot write foundation report");
        std::cout<<"ABM foundation native report: "<<report.at("cases").size()<<" cases\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
