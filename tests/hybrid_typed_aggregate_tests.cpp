#include "ankurafathom/hybrid/population_aggregate.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <iostream>

struct Person {};
using Store=ankurafathom::abm::PopulationStore<Person>;
using Reduction=ankurafathom::hybrid::PopulationReduction<Person>;
using K=Reduction::Kind;
using Message=ankurafathom::hybrid::AggregateMessage<Person>;
using Snapshot=ankurafathom::hybrid::PopulationSnapshot<Person>;
using Publication=ankurafathom::hybrid::ScalarPublication;
using Aggregate=ankurafathom::hybrid::PopulationAggregate<Person>;
using SD=ankurafathom::hybrid::SignalSD<Message>;
using Input=ankurafathom::devs::Input<Message>;
using Simulator=ankurafathom::devs::Simulator<Message>;
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
template<class F> void rejects(F f) {
    bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }
    require(caught,"invalid aggregate operation accepted");
}
Store people(std::vector<double> values) {
    Store store(9,{{"value",ankurafathom::des::FieldKind::real},{"selected",ankurafathom::des::FieldKind::boolean}},100);
    for(double x:values) store.spawn({x,true});
    return store;
}
double value(Store::Reference r,const Store& s) { return std::get<double>(s.field(r,0)); }
std::vector<Reduction> reductions() {
    return {{"sum",K::sum,value},{"mean",K::mean,value,{},0},{"count",K::count},
            {"min",K::minimum,value,{},-1},{"max",K::maximum,value,{},1}};
}
SD consumer(double dt=.5) {
    return SD({{"area",0,false}},{{SD::boundary,0,[](const auto&,const auto& v,double) { return v.at(0); },false}},
              {0,0,0,0,0},dt);
}
std::vector<Input> snapshot(double time,std::uint64_t revision,Store population,std::size_t source=7) {
    return {{source,0,Message{Snapshot{time,revision,std::move(population)}}}};
}
std::vector<Input> signals(double time,std::uint64_t revision,std::vector<double> values,std::size_t source=7) {
    return {{source,0,Message{Publication{time,revision,std::move(values)}}}};
}
void reducers() {
    auto s=people({-4,2,8,1000});s.retire({9,103});
    const std::vector<double> expected{6,2,3,-4,8};
    auto rs=reductions();
    for(std::size_t i=0;i<rs.size();++i) require(std::abs(rs[i].evaluate(s)-expected[i])<1e-14,"reducer hand values");
    s.update({9,101},{2.,false});
    auto predicate=[](Store::Reference r,const Store& store) { return std::get<bool>(store.field(r,1)); };
    require(Reduction("selected",K::sum,value,predicate).evaluate(s)==4,"filter ignored");
    require(Reduction("selected",K::count,{},predicate).evaluate(s)==2,"filtered count wrong");
    auto empty=people({});
    const std::vector<double> empties{0,0,0,-1,1};
    for(std::size_t i=0;i<rs.size();++i) require(rs[i].evaluate(empty)==empties[i],"empty fallback");
    for(auto kind:{K::mean,K::minimum,K::maximum}) rejects([&] { (void)Reduction("empty",kind,value).evaluate(empty); });
    const auto maximum=std::numeric_limits<double>::max();
    const auto same=people({maximum,maximum}),opposite=people({-maximum,maximum});
    require(Reduction("mean",K::mean,value).evaluate(same)==maximum,"finite mean overflowed");
    require(Reduction("mean",K::mean,value).evaluate(opposite)==0,"opposite extreme mean");
    rejects([&] { (void)Reduction("sum",K::sum,value).evaluate(same); });
    for(double invalid:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { (void)Reduction("bad",K::sum,[invalid](auto,const auto&) { return invalid; }).evaluate(s); });
    rejects([] { Reduction("",K::count); });
    rejects([] { Reduction("x",static_cast<K>(99),value); });
    rejects([] { Reduction("x",K::count,value); });
    rejects([] { Reduction("x",K::sum); });
    rejects([] { Reduction("x",K::sum,value,{},0); });
    rejects([] { Reduction("x",K::mean,value,{},std::numeric_limits<double>::infinity()); });
}
void atomic_contract() {
    const auto p=people({1,3});
    Aggregate a(p,reductions());
    require(a.index("mean")==1 && std::isinf(a.time_advance()),"aggregate initial state");
    rejects([&] { (void)a.index("unknown"); });
    rejects([&] { Aggregate duplicate(p,{{"x",K::count},{"x",K::count}}); });
    rejects([&] { Aggregate none(p,{}); });
    rejects([&] { a.internal_transition(); });
    a.external_transition_at(.125,.125,snapshot(.125,4,p));
    const auto first=a.output();
    require(std::get<Publication>(first.at(0).value)==Publication{.125,4,{4,2,2,1,3}},"aggregate publication");
    rejects([&] { a.external_transition(0,snapshot(.125,5,p)); });
    rejects([&] { a.confluent_transition(snapshot(.125,4,p)); });
    require(std::get<Publication>(a.output().at(0).value)==std::get<Publication>(first[0].value),"failed confluence lost pending batch");
    a.confluent_transition(snapshot(.125,5,people({8})));
    require(a.last()->values[0]==8 && a.last()->revision==5,"same-time newer revision lost");
    a.internal_transition();
    auto copy=a.clone();
    a.external_transition_at(.25,.125,snapshot(.25,6,p));
    require(!dynamic_cast<const Aggregate&>(*copy).output().size(),"copy shares pending state");
    a.internal_transition();
    const auto before=*a.last();
    rejects([&] { a.external_transition_at(.5,.25,snapshot(.5,7,p,8)); });
    rejects([&] { a.external_transition_at(.5,.25,snapshot(.5,6,p)); });
    rejects([&] { a.external_transition_at(.5,.25,snapshot(.25,7,p)); });
    rejects([&] { a.external_transition_at(.5,.125,snapshot(.5,7,p)); });
    auto multi=snapshot(.5,7,p);multi.push_back(multi[0]);
    rejects([&] { a.external_transition_at(.5,.25,multi); });
    auto wrong=snapshot(.5,7,p);wrong[0].port=4;
    rejects([&] { a.external_transition_at(.5,.25,wrong); });
    Store schema(9,{{"different",ankurafathom::des::FieldKind::real}},100);
    rejects([&] { a.external_transition_at(.5,.25,snapshot(.5,7,schema)); });
    require(a.last()==before && a.time()==.25,"rejected snapshot changed aggregate state");
    Aggregate throwing(p,{{"sum",K::sum,value},{"mean",K::mean,value}});
    throwing.external_transition(0,snapshot(0,0,p));throwing.internal_transition();
    rejects([&] { throwing.external_transition(.5,snapshot(.5,1,people({}))); });
    require(throwing.time()==0 && throwing.last()->values==std::vector<double>({4,2}),"later reducer failure partially committed");
    throwing.external_transition(.5,snapshot(.5,1,p));
    require(throwing.output().size()==1,"unchanged values must still publish revision");
    Aggregate exhaustion(p,reductions());
    exhaustion.external_transition(0,snapshot(0,std::numeric_limits<std::uint64_t>::max(),p));exhaustion.internal_transition();
    rejects([&] { exhaustion.external_transition(.5,snapshot(.5,0,p)); });
}
void consumer_contract() {
    auto sd=consumer();
    sd.external_transition(0,signals(0,0,{2,0,0,0,0}));
    sd.external_transition(.375,signals(.375,1,{6,0,0,0,0}));
    require(sd.state()[0]==.75,"off-grid input used retroactively");
    sd.confluent_transition(signals(.5,2,{4,0,0,0,0}));
    require(sd.state()[0]==1.5 && sd.time()==.5,"confluent input order");
    sd.internal_transition();
    require(sd.state()[0]==3.5 && sd.time()==1,"latched rate after tick");
    auto before=sd.state();
    rejects([&] { sd.external_transition(.125,signals(1.125,3,{1})); });
    rejects([&] { sd.external_transition(.125,signals(1.125,3,{1,0,0,0,0},8)); });
    rejects([&] { sd.external_transition(.125,signals(1.125,2,{1,0,0,0,0})); });
    rejects([&] { sd.external_transition(.125,signals(1.125,3,{NAN,0,0,0,0})); });
    rejects([&] { sd.external_transition_at(1.125,.125,signals(1.25,3,{1,0,0,0,0})); });
    rejects([&] { sd.external_transition(.75,signals(1.75,3,{1,0,0,0,0})); });
    require(sd.state()==before && sd.time()==1 && sd.revision()==2,"invalid signals changed SD state");
    SD overflow({{"x",0}},{{SD::boundary,0,[](const auto&,const auto&,double) { return 2.; }}},{0},std::numeric_limits<double>::max());
    rejects([&] { overflow.internal_transition(); });
    require(overflow.time()==0 && overflow.state()[0]==0,"overflow step changed SD");
    SD tick({{"x",0}},{},{0},std::numeric_limits<double>::max());
    rejects([&] { tick.internal_transition(); });
    require(tick.time()==0,"tick overflow committed integration");
    SD strict({{"x",1}},{{0,SD::boundary,[](const auto&,const auto& v,double) { return v[0]; }}},{4},.5);
    rejects([&] { strict.external_transition(.375,signals(.375,0,{0})); });
    require(strict.time()==0 && strict.state()[0]==1 && strict.signals()[0]==4 && !strict.revision(),"failed integration latched new values");
    SD nonlinear({{"x",2}},{{SD::boundary,0,[](const auto& x,const auto& v,double time) { return x[0]*v[0]+time; }}},{1},.5);
    nonlinear.internal_transition();require(nonlinear.state()[0]==3,"stock-dependent expression");
    nonlinear.internal_transition();require(nonlinear.state()[0]==4.75,"time-dependent expression");
    auto fractional=consumer(.1);
    fractional.external_transition(0,signals(0,0,{1,0,0,0,0}));
    fractional.internal_transition();fractional.internal_transition();
    fractional.external_transition_at(.3,.3-.2,signals(.3,1,{2,0,0,0,0}));
    require(fractional.time()==.3,"absolute publication timestamp rounded through elapsed time");
    fractional.internal_transition(); // The exact third grid deadline is 3*.1.
    require(fractional.time()==3*.1 && fractional.next_event_time()==.4,"fractional grid drift");
}

class RejectingSink final:public ankurafathom::devs::Atomic<Message> {
public:
    std::unique_ptr<ankurafathom::devs::Atomic<Message>> clone()const override { return std::make_unique<RejectingSink>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<ankurafathom::devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<Input>&)override { throw std::runtime_error("downstream failure"); }
    void confluent_transition(const std::vector<Input>& bag)override { external_transition(0,bag); }
};
void coupled_rollback() {
    Simulator sim;
    const auto a=sim.add(std::make_unique<Aggregate>(people({1}),reductions()));
    const auto sd=sim.add(std::make_unique<SD>(consumer()));
    const auto sink=sim.add(std::make_unique<RejectingSink>());
    sim.connect(a,1,sd,0);sim.connect(a,1,sink,0);
    auto population=people({2,4});
    sim.inject(.375,a,0,Message{Snapshot{.375,1,population}});
    population.update({9,100},{100.,true}); // Queued snapshot is independently owned.
    (void)sim.step_transactional(); // Snapshot accepted; output still pending.
    rejects([&] { (void)sim.step_transactional(); });
    const auto& aggregate=dynamic_cast<const Aggregate&>(sim.model(a));
    const auto& consumer=dynamic_cast<const SD&>(sim.model(sd));
    require(aggregate.last()->values[0]==6 && aggregate.output().size()==1,"failed downstream step lost publication");
    require(consumer.time()==0 && !consumer.revision() && consumer.state()[0]==0,"downstream failure did not restore consumer");
    sim.disconnect(a,1,sink,0);
    (void)sim.step_transactional();
    (void)sim.run_until_transactional(1);
    const auto& final=dynamic_cast<const SD&>(sim.model(sd));
    require(final.state()[0]==3.75 && final.revision()==1,"retry lost/duplicated area");
}
int main() {
    try {
        reducers();atomic_contract();consumer_contract();coupled_rollback();
        std::cout<<"Typed aggregates: reducers, stream ownership, snapshot timing, confluence, copy and checked rollback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
