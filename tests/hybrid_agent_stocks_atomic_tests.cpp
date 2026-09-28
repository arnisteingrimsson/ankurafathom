#include "ankurafathom/hybrid/agent_stocks_atomic.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <array>
#include <iostream>

struct Person {};
using Core=ankurafathom::hybrid::AgentStocks<Person>;
using E=Core::Endpoint;
using Message=ankurafathom::hybrid::AggregateMessage<Person>;
using Atomic=ankurafathom::hybrid::AgentStocksAtomic<Person>;
using Aggregate=ankurafathom::hybrid::PopulationAggregate<Person>;
using Reduction=Aggregate::Reduction;
using Snapshot=Atomic::Snapshot;
using SD=ankurafathom::hybrid::SignalSD<Message>;
using Simulator=ankurafathom::devs::Simulator<Message>;
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
template<class F> void rejects(F f) {
    bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }
    require(caught,"invalid agent-stock publisher operation accepted");
}
void close(double actual,double expected) {
    require(std::abs(actual-expected)<2e-12*std::max(1.,std::abs(expected)),"agent-stock publisher numeric mismatch");
}
Core make_core(std::size_t n=3,double rate=.5) {
    Core::Store store(6,{{"work",ankurafathom::des::FieldKind::real}});
    for(std::size_t i=0;i<n;++i) store.spawn({2.});
    Core result(std::move(store),{{0}},{{"done",0},{"exposure",0}});
    result.add_agent_flow(E::agent(0),E::global(0),[rate](auto ref,const auto& s,const auto&,double) {
        return rate*std::get<double>(s.field(ref,0));
    });
    result.add_global_flow(E::boundary(),E::global(1),[](const auto& s,const auto&,double) { return Core::sum(s,0); });
    return result;
}
struct Graph {
    Simulator sim;
    std::array<std::size_t,3> ids;
    Graph(std::array<std::size_t,3> order,double dt,double sd_dt,std::size_t n=3) {
        const auto core=make_core(n);
        std::array<std::unique_ptr<ankurafathom::devs::Atomic<Message>>,3> models;
        models[0]=std::make_unique<Atomic>(core,dt);
        models[1]=std::make_unique<Aggregate>(core.store(),std::vector<Reduction>{
            {"work",Reduction::Kind::sum,[](auto ref,const auto& s) { return std::get<double>(s.field(ref,0)); }}});
        models[2]=std::make_unique<SD>(std::vector<SD::Stock>{{"exposure",0}},
            std::vector<SD::Flow>{{SD::boundary,0,[](const auto&,const auto& signals,double) { return signals[0]; }}},
            std::vector<double>{0},sd_dt);
        for(auto index:order) ids[index]=sim.add(std::move(models[index]));
        sim.connect(ids[0],1,ids[1],0);sim.connect(ids[1],1,ids[2],0);
    }
    const Atomic& source()const { return dynamic_cast<const Atomic&>(sim.model(ids[0])); }
    const Aggregate& aggregate()const { return dynamic_cast<const Aggregate&>(sim.model(ids[1])); }
    const SD& consumer()const { return dynamic_cast<const SD&>(sim.model(ids[2])); }
};
void ordered_composition() {
    std::array<std::size_t,3> order{0,1,2};
    do {
        for(double sd_dt:{.125,.25,.375,.5}) {
            Graph graph(order,.25,sd_dt);
            for(int tick=0;tick<=16;++tick) {
                const double time=tick*.25;
                (void)graph.sim.run_until_transactional(time);
                const double remaining=6*std::pow(.875,tick),area=(6-remaining)/.5;
                close(graph.source().core().sum(0),remaining);
                close(graph.source().core().global_state()[0]+remaining,6);
                close(graph.source().core().global_state()[1],area);
                close(graph.consumer().state()[0],area);
                close(graph.consumer().signals()[0],remaining);
                require(graph.source().revision()==static_cast<std::uint64_t>(tick) &&
                    graph.aggregate().last()->revision==static_cast<std::uint64_t>(tick) &&
                    graph.consumer().revision()==static_cast<std::uint64_t>(tick),"settled snapshot revision mismatch");
            }
        }
    } while(std::next_permutation(order.begin(),order.end()));
    Graph dense({0,1,2},.25,.5),sparse({2,1,0},.25,.5);
    for(int tick=0;tick<=64;++tick) {
        (void)dense.sim.run_until_transactional(tick/16.);
        if(tick%4==0) {
            (void)sparse.sim.run_until_transactional(tick/16.);
            require(dense.consumer().state()==sparse.consumer().state() &&
                    dense.source().core().sum(0)==sparse.source().core().sum(0),"observation density changed state");
        }
    }
    Graph empty({0,1,2},.25,.5,0);
    (void)empty.sim.run_until_transactional(1);
    require(empty.consumer().state()[0]==0 && empty.consumer().signals()[0]==0,"empty publisher composition");
}
void direct_contract() {
    Atomic source(make_core(),.1);
    require(source.next_event_time()==0 && source.output().size()==1,"missing initial publication");
    const auto initial=source.output();
    source.internal_transition(); // Publish time zero; do not integrate.
    require(source.core().time()==0 && source.output().empty(),"publication integrated core");
    for(std::uint64_t tick=1;tick<=30;++tick) {
        source.internal_transition();
        require(source.core().time()==tick*.1 && source.revision()==tick,"absolute agent clock drift");
        auto copy=source.clone();source.internal_transition();
        require(copy->output().size()==1 && source.output().empty(),"clone shares pending publication");
    }
    require(std::get<Snapshot>(initial[0].value).population.field({6,0},0)==Core::Store::Value{2.},"old snapshot changed with source");
    rejects([&] { source.external_transition(0,{}); });
    rejects([&] { source.confluent_transition({}); });
    rejects([] { Atomic(make_core(),0); });
    rejects([] { Atomic(make_core(),NAN); });
    auto advanced=make_core();advanced.step(.25);
    rejects([&] { Atomic(advanced,.25); });
    Atomic invalid(make_core(1,4),.5);invalid.internal_transition();
    rejects([&] { invalid.internal_transition(); });
    require(invalid.core().time()==0 && invalid.revision()==0 && invalid.core().sum(0)==2 &&
            invalid.output().empty() && invalid.next_event_time()==.5,"failed tick partially committed");
    Atomic overflow(make_core(0),std::numeric_limits<double>::max());overflow.internal_transition();
    rejects([&] { overflow.internal_transition(); });
    require(overflow.core().time()==0 && overflow.revision()==0,"future deadline overflow changed state");
}
class Reject final:public ankurafathom::devs::Atomic<Message> {
public:
    std::unique_ptr<ankurafathom::devs::Atomic<Message>> clone()const override { return std::make_unique<Reject>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<ankurafathom::devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<ankurafathom::devs::Input<Message>>&)override { throw std::runtime_error("downstream failure"); }
    void confluent_transition(const std::vector<ankurafathom::devs::Input<Message>>& bag)override { external_transition(0,bag); }
};
void rollback_and_budget() {
    Graph graph({0,1,2},.25,.25);
    rejects([&] { (void)graph.sim.run_until_transactional(0,0); });
    (void)graph.sim.run_until_transactional(0);
    require(graph.source().revision()==0 && graph.consumer().signals()[0]==6,"initial budget retry lost values");
    const auto reject=graph.sim.add(std::make_unique<Reject>());
    graph.sim.connect(graph.ids[0],1,reject,0);
    (void)graph.sim.step_transactional(); // Integrate source and consumer to .25.
    require(graph.source().core().time()==.25 && graph.source().output().size()==1,"tick did not stage publication");
    rejects([&] { (void)graph.sim.step_transactional(); });
    require(graph.source().revision()==1 && graph.source().output().size()==1 && graph.aggregate().last()->revision==0,
            "failed source publication lost pending state");
    graph.sim.disconnect(graph.ids[0],1,reject,0);
    (void)graph.sim.run_until_transactional(.25);
    require(graph.source().revision()==1 && graph.consumer().signals()[0]==5.25 && graph.consumer().state()[0]==1.5,
            "source retry integrated twice or lost values");
    (void)graph.sim.run_until_transactional(.5);
    require(graph.consumer().state()[0]==2.8125,"subsequent interval did not use retried values");
}
int main() {
    try {
        ordered_composition();direct_contract();rollback_and_budget();
        std::cout<<"Agent stock publisher: 6 declaration orders, 4 clock ratios, 408 analytic snapshots, rollback and budgets passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
