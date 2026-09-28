#include "ankurafathom/hybrid/dynamic_agent_stocks.hpp"
#include <iostream>

namespace af=ankurafathom;
struct Person {};
using Core=af::hybrid::AgentStocks<Person>;
using E=Core::Endpoint;
using Atomic=af::hybrid::DynamicAgentStocksAtomic<Person>;
using M=af::hybrid::DynamicAgentStockMessage<Person>;
using Input=af::hybrid::AgentStockInput<Person>;
using Life=af::abm::PopulationLifecycleInput<Person>;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }require(caught,"invalid dynamic stock operation accepted"); }
Core::Store store(std::uint64_t first=0) {
    using K=af::des::FieldKind;
    Core::Store s(9,{{"work",K::real},{"done",K::real},{"rate",K::real},{"enabled",K::boolean}},first);
    s.spawn({10.,0.,2.,true});s.spawn({4.,1.,1.,true});return s;
}
Core core() {
    auto s=store();s.configure_network(af::abm::CsrNetwork({0,1},{{0,1}},false));
    Core c(s,{{0,true},{1,true}},{{"global",0,true}});
    c.add_agent_flow(E::agent(0),E::agent(1),[](auto ref,const auto& s,const auto&,double) { return std::get<double>(s.field(ref,2)); });
    c.add_agent_flow(E::agent(0),E::global(0),[](auto,const auto&,const auto&,double) { return .5; });return c;
}
double field(const Core& c,std::uint64_t id,std::size_t f=0) { return std::get<double>(c.store().field({9,id},f)); }
Core accretion() {
    Core::Store s(9,store().schema());s.spawn({0.,0.,2.,true});
    Core c(s,{{0,true},{1,true}},{{"global",0,true}});
    c.add_agent_flow(E::boundary(),E::agent(0),[](auto ref,const auto& s,const auto&,double) {
        return std::get<bool>(s.field(ref,3))?std::get<double>(s.field(ref,2)):0.;
    });return c;
}
void core_transactions() {
    auto c=core();c.step_to(.25);require(c.time()==.25 && c.sum(0)+c.sum(1)+c.global_state()[0]==15,"conservative Euler baseline");
    Core::Change change;change.updates={{{9,0},2,4.}};
    change.pulses={{E::agent(0),Core::Reference{9,0},-1},{E::global(0),{},1}};
    change.retirements={{9,1}};change.births={{6.,2.,3.,true}};
    auto result=c.apply(change);
    require(result.born==std::vector<Core::Reference>{{9,2}} && result.retired==std::vector<Core::Reference>{{9,1}},"lifecycle identities");
    require(result.birth_amounts==std::vector<double>{6,2} && result.retired_amounts==std::vector<double>{3.625,1.25},"retired/born stock accounting");
    require(result.agent_pulses==std::vector<double>{-1,0} && result.global_pulses==std::vector<double>{1},"pulse receipt");
    require(c.sum(0)+c.sum(1)+c.global_state()[0]==18.125 && c.store().network()->edges().empty() && c.store().network()->vertices()==std::vector<std::uint64_t>{0,2},"lifecycle conservation or network membership");
    const auto before=c;const auto original=c.store().record({9,0});
    for(int mode=0;mode<14;++mode) {
        Core::Change bad;
        if(mode==0) bad.updates={{{9,0},0,0.}};
        if(mode==1) bad.updates={{{9,0},2,1.},{{9,0},2,2.}};
        if(mode==2) bad.updates={{{9,0},2,std::int64_t{1}}};
        if(mode==3) bad.updates={{{9,0},2,NAN}};
        if(mode==4) bad.pulses={{E::agent(2),Core::Reference{9,0},1}};
        if(mode==5) bad.pulses={{E::agent(0),Core::Reference{8,0},1}};
        if(mode==6) bad.pulses={{E::global(0),Core::Reference{9,0},1}};
        if(mode==7) bad.pulses={{E::agent(0),Core::Reference{9,1},1}};
        if(mode==8) bad.pulses={{E::agent(0),Core::Reference{9,0},-100}};
        if(mode==9) bad.pulses={{E::global(0),{},-100}};
        if(mode==10) bad.retirements={{9,0},{9,0}};
        if(mode==11) { bad.retirements={{9,0}};bad.births={{-1.,0.,1.,true}}; }
        if(mode==12) { bad.births={{1.,0.,1.,true}};bad.pulses={{E::agent(0),Core::Reference{9,3},1}}; }
        if(mode==13) bad.pulses={{E::global(0),{},std::numeric_limits<double>::max()},{E::global(0),{},std::numeric_limits<double>::max()}};
        rejects([&] { c.apply(bad); });
        require(c.time()==before.time() && c.store().next_id()==3 && c.store().record({9,0})==original && c.global_state()==before.global_state(),"failed discrete change leaked state");
    }
    change={};change.pulses={{E::agent(0),Core::Reference{9,0},-100},{E::agent(0),Core::Reference{9,0},100},
                            {E::global(0),{},-100},{E::global(0),{},100}};
    c.apply(change);require(c.store().record({9,0})==original,"net pulse application clipped intermediate amount");
    c.step_to(.5);require(field(c,0)==7.25 && field(c,2)==5.125,"post-change rates/newborn integration");
    auto frozen=accretion();frozen.apply({});rejects([&] { frozen.add_global_flow(E::boundary(),E::global(0),[](const auto&,const auto&,double) { return 1.; }); });
    Core::Store last(9,store().schema(),Core::Store::max_entity_id);last.spawn({1.,0.,0.,true});Core exhausted(last,{{0,true}});
    change={};change.retirements={{9,Core::Store::max_entity_id}};change.births={{1.,0.,0.,true}};
    rejects([&] { exhausted.apply(change); });require(exhausted.store().active_count()==1,"failed birth retired final ID");
    auto exact=accretion();exact.step_to(.1);exact.step_to(.3);require(exact.time()==.3,"absolute target drift");
    rejects([&] { exact.step_to(.3); });rejects([&] { exact.step_to(INFINITY); });
    Core empty(Core::Store(9,store().schema()),{{0,true}});empty.step_to(.25);
    change={};change.births={{2.,0.,1.,true}};empty.apply(change);empty.step_to(.5);
    change={};change.retirements={{9,0}};const auto removal=empty.apply(change);empty.step_to(1);
    require(empty.store().active_count()==0 && removal.retired_amounts[0]==2 && empty.time()==1,"empty population integration/lifecycle");
    Core reordered(store(),{{1,true},{0,true}});change={};change.pulses={{E::agent(0),Core::Reference{9,0},1}};change.births={{3.,2.,1.,true}};
    const auto receipt=reordered.apply(change);
    require(receipt.agent_pulses==std::vector<double>{0,1} && receipt.birth_amounts==std::vector<double>{2,3},"receipt vectors do not follow binding order");
}
af::devs::Input<M> control(double time,std::string channel,std::uint64_t revision,Core::Change change={},std::size_t owner=7) {
    return {owner,0,M{Input{time,std::move(channel),revision,std::move(change)}}};
}
void atomic_timing() {
    Atomic a(accretion(),.25,{"a","z"});a.internal_transition();
    Core::Change change;change.births={{10.,0.,4.,true}};change.pulses={{E::agent(0),Core::Reference{9,0},1},{E::global(0),{},2}};
    a.external_transition_at(.125,.125,{control(.125,"a",0,change)});
    require(a.core().time()==.125 && field(a.core(),0)==1.25 && field(a.core(),1)==10 && a.revision()==1,"off-grid integration/newborn timing");
    require(std::get<af::hybrid::AgentStockCommit<Person>>(a.output()[1].value).discrete->birth_amounts[0]==10,"published discrete receipt");
    a.internal_transition();require(a.next_event_time()==.25,"off-grid event shifted tick");
    change={};change.retirements={{9,0}};
    a.confluent_transition({control(.25,"a",1,change)});
    require(field(a.core(),1)==10.5 && !a.core().store().alive({9,0}) && a.revision()==2,"tick/lifecycle confluence double integration");
    require(std::get<af::hybrid::AgentStockCommit<Person>>(a.output()[1].value).discrete->retired_amounts[0]==1.5,"retirement did not accrue through timestamp");
    change={};change.pulses={{E::agent(0),Core::Reference{9,1},1}};
    a.confluent_transition({control(.25,"z",0,change)});
    require(field(a.core(),1)==11.5 && a.revision()==3,"pending confluence reintegrated");
    a.internal_transition();require(a.next_event_time()==.5,"confluence shifted absolute tick");
    for(const auto& bad:std::vector<af::devs::Input<M>>{control(.375,"z",0),control(.375,"z",1,{},8),control(.375,"unknown",0),control(.5,"z",1)}) {
        rejects([&] { a.external_transition_at(.375,.125,{bad}); });
        require(a.core().time()==.25 && a.input_revision("z")==0 && field(a.core(),1)==11.5,"invalid input consumed integration/revision");
    }
    change={};change.pulses={{E::agent(0),Core::Reference{9,1},-100}};
    rejects([&] { a.external_transition_at(.375,.125,{control(.375,"z",1,change)}); });
    a.external_transition_at(.375,.125,{control(.375,"z",1)});a.internal_transition();a.internal_transition();
    require(a.core().time()==.5 && field(a.core(),1)==12.5,"failed-input retry changed trajectory");
    auto copy=a.clone();a.internal_transition();a.internal_transition();
    require(dynamic_cast<const Atomic&>(*copy).core().time()==.5 && a.core().time()==.75,"dynamic atomic clone aliases core");
}
void canonical_lifecycle() {
    for(const bool reverse:{false,true}) {
        Atomic a(accretion(),.25,{"a","z"});
        Core::Change first,last;first.births={{1.,0.,1.,true}};last.births={{2.,0.,2.,true}};
        Life life{0,9,{},{{3.,0.,3.,true}},af::abm::PopulationLifecycleTarget{9,a.core().store().schema()}};
        Life earlier{0,4,{},{{4.,0.,4.,true}},life.target};
        std::vector<af::devs::Input<M>> bag{control(0,"z",0,last),{8,3,M{life}},control(0,"a",0,first),{8,3,M{earlier}}};
        if(reverse) std::reverse(bag.begin(),bag.end());a.confluent_transition(bag);
        require(field(a.core(),1)==1 && field(a.core(),2)==2 && field(a.core(),3)==4 && field(a.core(),4)==3,"canonical cross-channel birth order");
        rejects([&] { a.confluent_transition({{9,3,M{life}}}); });
        require(a.core().store().next_id()==5 && a.revision()==1,"lifecycle replay consumed IDs");
        life.sequence=10;life.target->store=8;rejects([&] { a.confluent_transition({{8,3,M{life}}}); });
        life.target.reset();rejects([&] { a.confluent_transition({{8,3,M{life}}}); });
        Core::Change duplicate;duplicate.updates={{{9,0},2,3.}};
        rejects([&] { a.confluent_transition({control(0,"a",1,duplicate),control(0,"z",1,duplicate)}); });
        require(a.input_revision("a")==0 && a.input_revision("z")==0 && field(a.core(),0,2)==2,"cross-channel duplicate update consumed revisions");
    }
}
class Failing final:public af::devs::Atomic<M> {
public:
    std::unique_ptr<af::devs::Atomic<M>> clone()const override { return std::make_unique<Failing>(*this); }
    double time_advance()const override { return INFINITY; }
    std::vector<af::devs::PortValue<M>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<af::devs::Input<M>>&)override { throw std::runtime_error("downstream failure"); }
    void confluent_transition(const std::vector<af::devs::Input<M>>& bag)override { external_transition(0,bag); }
};
void publication_retry_and_observation() {
    for(const bool dense:{false,true}) {
        af::devs::Simulator<M> sim;const auto owner=sim.add(std::make_unique<Atomic>(accretion(),.25,std::vector<std::string>{"a"}));
        const auto fail=sim.add(std::make_unique<Failing>());sim.run_until_transactional(0);
        Core::Change change;change.births={{10.,0.,4.,true}};
        sim.inject(.125,owner,0,M{Input{.125,"a",0,change}});sim.step_transactional();
        sim.connect(owner,Atomic::snapshot_port,fail,0);rejects([&] { sim.step_transactional(); });
        const auto& pending=dynamic_cast<const Atomic&>(sim.model(owner));
        require(pending.core().time()==.125 && pending.core().store().next_id()==2 && pending.revision()==1 && pending.output().size()==2,"failed publication lost committed lifecycle");
        sim.disconnect(owner,Atomic::snapshot_port,fail,0);
        if(dense) for(const double t:{.125,.1875,.25,.3125,.375,.4375}) sim.run_until_transactional(t);
        sim.run_until_transactional(.5);const auto& end=dynamic_cast<const Atomic&>(sim.model(owner));
        require(end.core().time()==.5 && end.revision()==3 && field(end.core(),0)==1 && field(end.core(),1)==11.5,"retry/observations changed Euler intervals");
    }
    Core passive(Core::Store(9,store().schema()),{{0,true}});Atomic overflow(passive,std::numeric_limits<double>::max());overflow.internal_transition();
    rejects([&] { overflow.internal_transition(); });require(overflow.core().time()==0 && overflow.revision()==0,"tick overflow committed integration");
}
int main() {
    try { core_transactions();atomic_timing();canonical_lifecycle();publication_retry_and_observation();std::cout<<"dynamic agent stock lifecycle, pulses, clocks, rollback and canonical inputs passed\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
