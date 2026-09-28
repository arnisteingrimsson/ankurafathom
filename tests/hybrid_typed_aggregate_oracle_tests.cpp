#include "ankurafathom/hybrid/population_aggregate.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

struct Person {};
using Store=ankurafathom::abm::PopulationStore<Person>;
using Message=ankurafathom::hybrid::AggregateMessage<Person>;
using Aggregate=ankurafathom::hybrid::PopulationAggregate<Person>;
using Reduction=Aggregate::Reduction;
using K=Reduction::Kind;
using SD=ankurafathom::hybrid::SignalSD<Message>;
using json=nlohmann::json;
Store population(const json& records) {
    Store result(7,{{"value",ankurafathom::des::FieldKind::real},{"selected",ankurafathom::des::FieldKind::boolean}},100);
    for(const auto& record:records) {
        auto ref=result.spawn({record.at("value").get<double>(),record.at("selected").get<bool>()});
        if(!record.at("alive").get<bool>()) result.retire(ref);
    }
    return result;
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan and output paths");
        std::ifstream input(argv[1]);const auto plan=json::parse(input);
        std::ofstream output(argv[2]);output.exceptions(std::ios::badbit|std::ios::failbit);
        std::size_t rows=0;
        for(const auto& c:plan.at("cases")) for(int order:plan.at("orders")) {
            const auto projection=[](Store::Reference ref,const Store& s) { return std::get<double>(s.field(ref,0)); };
            const auto filter=[selected=c.at("filtered").get<bool>()](Store::Reference ref,const Store& s) {
                return !selected || std::get<bool>(s.field(ref,1));
            };
            auto aggregate=std::make_unique<Aggregate>(population(c.at("snapshots")[0].at("records")),std::vector<Reduction>{
                {"sum",K::sum,projection,filter},{"mean",K::mean,projection,filter,0},
                {"count",K::count,{},filter},{"min",K::minimum,projection,filter,-1},
                {"max",K::maximum,projection,filter,1}});
            std::vector<SD::Stock> stocks;
            std::vector<SD::Flow> flows;
            for(std::size_t i=0;i<5;++i) {
                stocks.push_back({plan.at("reducers")[i].get<std::string>()+"_area",0,false});
                flows.push_back({SD::boundary,i,[i](const auto&,const auto& values,double) { return values.at(i); },false});
            }
            auto consumer=std::make_unique<SD>(stocks,flows,std::vector<double>(5,0),c.at("dt").get<double>());
            ankurafathom::devs::Simulator<Message> sim;
            std::size_t aid,sid;
            if(order==0) { aid=sim.add(std::move(aggregate));sid=sim.add(std::move(consumer)); }
            else { sid=sim.add(std::move(consumer));aid=sim.add(std::move(aggregate)); }
            sim.connect(aid,1,sid,0);
            for(const auto& s:c.at("snapshots"))
                sim.inject(s.at("time"),aid,0,Message{Aggregate::Snapshot{s.at("time"),s.at("revision"),population(s.at("records"))}});
            for(double horizon:plan.at("horizons")) {
                (void)sim.run_until_transactional(horizon);
                const auto& sd=dynamic_cast<const SD&>(sim.model(sid));
                output<<json{{"case",c.at("id")},{"order",order},{"horizon",horizon},{"time",sd.time()},
                             {"revision",sd.revision().value()},{"signals",sd.signals()},{"stocks",sd.state()}}.dump()<<'\n';
                ++rows;
            }
        }
        std::cout<<rows<<" typed aggregate/SD snapshots generated\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
