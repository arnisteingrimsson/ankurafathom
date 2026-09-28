#include "ankurafathom/hybrid/signal_rate_source.hpp"
#include "ankurafathom/hybrid/signal_select.hpp"
#include "ankurafathom/hybrid/publishing_signal_sd.hpp"
#include <iostream>

struct Opportunity {};
using Token=ankurafathom::des::EntityToken<Opportunity>;
using M=std::variant<Token,ankurafathom::hybrid::ScalarPublication,ankurafathom::hybrid::StockPulse>;
using Source=ankurafathom::hybrid::SignalRateSource<Opportunity,M>;
using Router=ankurafathom::hybrid::SignalSelect<Opportunity,M>;
using SD=ankurafathom::hybrid::SignalSD<M>;
using Publisher=ankurafathom::hybrid::PublishingSignalSD<M>;
int main() {
    SD core({{"arrival_rate",1}},{{SD::boundary,0,[](const auto&,const auto&,double) { return .25; }}},{0},.5);
    ankurafathom::devs::Simulator<M> sim;
    auto sd=sim.add(std::make_unique<Publisher>(std::move(core),[](const auto& state,const auto&,double) {
        return std::vector<double>{state[0],state[0]/(state[0]+1)};
    }));
    Source::Store records(7,{{"arrival",ankurafathom::des::FieldKind::real},{"rate",ankurafathom::des::FieldKind::real}});
    auto source=sim.add(std::make_unique<Source>(records,std::vector<double>{0,0},[](const auto& v,double) { return v[0]; },
        [](auto,const auto& v,double time) { return Source::Record{time,v[0]}; },Source::Draws{123,0,0,7},100));
    auto router=sim.add(std::make_unique<Router>(7,std::vector<double>{0,0},[](const auto& v,double) {
        return std::vector<double>{v[1],1-v[1]};
    },Router::Draws{123,0,0,8}));
    sim.connect(sd,1,source,0);sim.connect(sd,1,router,Router::signal_port);sim.connect(source,1,router,0);
    std::cout<<"time,entity,branch,arrival_rate\n";
    for(const auto& step:sim.run_until_transactional(4)) for(const auto& out:step.emissions) if(out.source==router) {
        const auto& token=std::get<Token>(out.value);
        std::cout<<step.time<<','<<token.entity.id<<','<<out.port<<','<<std::get<double>(token.snapshot->record[1])<<'\n';
    }
}
