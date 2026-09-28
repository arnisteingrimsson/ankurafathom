#include "ankurafathom/hybrid/entity_agent.hpp"
#include "ankurafathom/abm/statechart.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <array>
#include <iostream>

struct Person {};
using Owner=ankurafathom::hybrid::EntityAgentAtomic<Person>;
using M=ankurafathom::hybrid::EntityAgentMessage<Person>;
using Pop=Owner::Population;using Store=Pop::Store;using Token=Owner::Token;
using Input=ankurafathom::devs::Input<M>;
using Sim=ankurafathom::devs::Simulator<M>;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }require(caught,"invalid entity-agent operation accepted"); }
Store basic() {
    Store s(8,{{"work",ankurafathom::des::FieldKind::real},{"ready",ankurafathom::des::FieldKind::boolean}});
    s.spawn({1.,true});s.spawn({2.,false});s.spawn({3.,true});return s;
}
Owner synchronous() {
    Pop::Sync runtime(basic());
    runtime.add_phase([](auto ref,const Store& s) { auto r=s.record(ref);std::get<double>(r[0])+=1;return r; });
    Pop population(std::move(runtime),.5,[](const Pop::Command& c,const Store& s) {
        Pop::Effects e;auto r=s.record(c.agent);
        if(c.kind=="retire") e.retirements.push_back(c.agent);
        else {
            if(c.kind=="complete") std::get<double>(r[0])+=100;
            else if(c.kind=="ready") r[1]=true;
            else throw std::invalid_argument("unknown command");
            e.updates.push_back({c.agent,r});
        }
        return e;
    });
    return Owner(std::move(population),[](auto r,const Store& s,double) { return std::get<bool>(s.field(r,1)); },"complete");
}
Input command(double t,std::uint64_t id,std::string name) { return {7,0,M{Pop::Command{t,0,{8,id},std::move(name)}}}; }
Input returned(Token token) { return {9,Owner::return_port,M{std::move(token)}}; }
void owner_contract() {
    auto owner=synchronous();
    require(owner.output().empty() && owner.next_event_time()==0 && owner.launched_count()==0,"owner initial phase");
    owner.internal_transition();
    require(owner.output().size()==2 && owner.launched_count()==2 && owner.status({8,1})==Owner::Status::waiting,"initial eligibility");
    const Token a=owner.issued({8,0}),c=owner.issued({8,2});
    require(a.snapshot->time==0 && a.snapshot->schema==basic().schema() && a.snapshot->record==basic().record({8,0}),"dispatch snapshot");
    auto copy=owner.clone();owner.internal_transition();
    require(copy->output().size()==2 && owner.output().empty(),"owner clone aliases pending process dispatch");
    owner.external_transition(.125,{command(.125,1,"ready")});
    require(owner.launched_count()==3 && owner.issued({8,1}).snapshot->time==.125,"late eligibility");
    owner.internal_transition();
    owner.external_transition_at(.25,.125,{returned(a)});owner.internal_transition();
    require(owner.returned_count()==1 && owner.in_flight_count()==2 && std::get<double>(owner.store().field({8,0},0))==101,"return not applied to authoritative record");
    std::vector<Token> bad{a,c,c,c,c,c};
    bad[1].snapshot.reset();bad[2].snapshot->record[0]=999.;bad[3].priority=1;
    bad[4].leases.push_back({7,2,1});bad[5].entity.store=9;
    for(const auto& token:bad) {
        rejects([&] { owner.external_transition_at(.375,.125,{returned(token)}); });
        require(owner.time()==.25 && owner.returned_count()==1 && owner.in_flight_count()==2,"bad return changed ledger");
    }
    rejects([&] { owner.external_transition_at(.375,.125,{command(.375,2,"retire")}); });
    require(owner.store().alive({8,2}) && owner.time()==.25,"in-flight retirement committed");
    auto wrong=returned(c);wrong.port=0;
    rejects([&] { owner.external_transition_at(.375,.125,{wrong}); });
    rejects([&] { owner.external_transition_at(.375,.125,{returned(c),returned(c)}); });
    require(owner.returned_count()==1,"duplicate return partially committed");
    owner.confluent_transition({returned(c)});
    require(owner.time()==.5 && owner.returned_count()==2 && std::get<double>(owner.store().field({8,2},0))==104,"return/tick confluence");
    require(owner.issued({8,2}).snapshot->record==basic().record({8,2}),"dispatch snapshot became live state");
    owner.internal_transition();
    owner.external_transition_at(.625,.125,{command(.625,0,"retire")});owner.internal_transition();
    require(!owner.store().alive({8,0}) && owner.returned_count()==2,"returned agent cannot retire");
    rejects([&] { owner.external_transition_at(.75,.125,{returned(a)}); });
    rejects([&] { owner.external_transition_at(1.5,.875,{returned(owner.issued({8,1}))}); });
    rejects([] { Owner(Pop(Pop::Sync(basic()),1),{},"complete"); });
    rejects([] { Owner(Pop(Pop::Sync(basic()),1),[](auto,const auto&,double) { return true; },""); });
}
void timer_retirement_and_initial_confluence() {
    Pop::Async runtime(basic(),[](const auto& timer,const Store&) { Pop::Effects e;e.retirements.push_back(timer.agent);return e; });
    runtime.schedule({.25,{8,0},"retire",0});
    Pop pop(std::move(runtime),[](const auto&,const Store&) { return Pop::Effects{}; });
    Owner owner(std::move(pop),[](auto ref,const Store&,double) { return ref.id==0; },"complete");
    owner.internal_transition();const auto token=owner.issued({8,0});owner.internal_transition();
    rejects([&] { owner.internal_transition(); });
    require(owner.time()==0 && owner.store().alive({8,0}) && owner.next_event_time()==.25,"timer retirement stranded in-flight identity");
    owner.external_transition(.125,{returned(token)});owner.internal_transition();owner.internal_transition();
    require(!owner.store().alive({8,0}) && owner.returned_count()==1,"returned identity blocked later timer retirement");
    auto initial=synchronous();
    initial.confluent_transition({{7,3,M{Pop::LifecycleInput{0,0,{},{{4.,true}}}}}});
    require(initial.launched_count()==3 && initial.store().next_id()==4 && initial.issued({8,3}).snapshot->time==0,"initial lifecycle confluence");
    initial.internal_transition();
    auto reserved=command(.125,1,"ready");std::get<Pop::Command>(reserved.value).sequence=Owner::completion_sequence;
    rejects([&] { initial.external_transition(.125,{reserved}); });
    require(initial.time()==0,"reserved sequence committed");
    // Ordinary same-agent commands precede the reserved completion action.
    initial.external_transition(.125,{returned(initial.issued({8,0})),command(.125,0,"ready")});
    require(initial.returned_count()==1 && std::get<double>(initial.store().field({8,0},0))==101,"same-agent input/return ordering");
}
using Chart=ankurafathom::abm::Statechart<Person>;
Store chart_store() {
    return Store(8,{{"state",ankurafathom::des::FieldKind::integer},{"entered",ankurafathom::des::FieldKind::real},
        {"generation",ankurafathom::des::FieldKind::integer},{"work",ankurafathom::des::FieldKind::real},
        {"duration",ankurafathom::des::FieldKind::real}});
}
Store::Record newborn(double work,double duration) { return {std::int64_t{-1},0.,std::int64_t{-1},work,duration}; }
Owner chart_owner(std::vector<double> durations,bool retire=false,double ready_delay=.125) {
    auto s=chart_store();for(std::size_t i=0;i<durations.size();++i) s.spawn(newborn(10*i,durations[i]));
    Chart::Transition ready;ready.name="ready";ready.source=0;ready.target=1;ready.trigger=Chart::Trigger::timeout;ready.value=ready_delay;
    ready.action=[](auto ref,const Store& store) { auto r=store.record(ref);std::get<double>(r[3])+=1;return r; };
    Chart::Transition progress;progress.name="progress";progress.source=1;progress.target=2;progress.trigger=Chart::Trigger::timeout;progress.value=.25;
    progress.action=[](auto ref,const Store& store) { auto r=store.record(ref);std::get<double>(r[3])+=10;return r; };
    std::vector<Chart::Transition> transitions{ready,progress};
    for(std::int64_t state:{1,2}) {
        Chart::Transition finish;finish.name="finish"+std::to_string(state);finish.source=state;finish.target=3;
        finish.trigger=Chart::Trigger::message;finish.message="complete";
        finish.action=[](auto ref,const Store& store) { auto r=store.record(ref);std::get<double>(r[3])+=100;return r; };
        if(retire) finish.lifecycle=[](auto ref,const Store&) { return Chart::Lifecycle{ref.id==1,{}}; };
        transitions.push_back(finish);
    }
    Chart chart(s,{"state","entered","generation"},{0,1,2,3},transitions);
    Pop::Async runtime(s,[chart](const auto& timer,const Store& store) { return chart.on_timer(timer,store); });
    for(auto id=s.first_id();id<s.next_id();++id) runtime.apply(chart.start({8,id},runtime.store(),0,0));
    Pop pop(std::move(runtime),[chart](const Pop::Command& c,const Store& store) {
        if(c.kind=="change_duration") { Pop::Effects e;auto r=store.record(c.agent);r[4]=10.;e.updates.push_back({c.agent,r});return e; }
        return chart.message(c.agent,store,c.kind,c.time);
    });
    pop.configure_births([chart](auto ref,const Store& store,double time) {
        const auto e=chart.start(ref,store,0,time);Pop::Async::Birth result{e.updates.at(0).second,{}};
        for(const auto& timer:e.schedules) result.timers.push_back({timer.time,timer.kind,timer.generation});return result;
    });
    return Owner(std::move(pop),[](auto ref,const Store& store,double) { return std::get<std::int64_t>(store.field(ref,0))==1; },"complete");
}
void chart_delay() {
    using Delay=ankurafathom::des::ReferenceDelay<Person,M>;
    for(bool reverse:{false,true}) for(bool retire:{false,true}) {
        Sim sim;std::size_t owner,delay;
        auto o=std::make_unique<Owner>(chart_owner({.125,.25,.5},retire));
        auto d=std::make_unique<Delay>(8,[](const Token& token) { return std::get<double>(token.snapshot->record[4]); });
        if(reverse) { delay=sim.add(std::move(d));owner=sim.add(std::move(o)); }
        else { owner=sim.add(std::move(o));delay=sim.add(std::move(d)); }
        sim.connect(owner,5,delay,0);sim.connect(delay,1,owner,6);
        sim.inject(.25,owner,0,M{Pop::Command{.25,0,{8,2},"change_duration"}});
        for(double time:{0.,.125,.25,.375,.5,.625,1.}) {
            sim.run_until_transactional(time);
            const auto& live=dynamic_cast<const Owner&>(sim.model(owner));
            const std::size_t complete=(time>=.25)+(time>=.375)+(time>=.625);
            require(live.returned_count()==complete && live.launched_count()==(time>=.125?3:0),"chart/process timing mismatch");
            if(time>=.625) {
                require(std::get<double>(live.store().field({8,0},3))==101 && std::get<double>(live.store().field({8,2},3))==131,"current chart state ignored by process return");
                require(live.store().alive({8,1})!=retire,"return lifecycle policy");
                if(!retire) require(std::get<double>(live.store().field({8,1},3))==121,"timer-before-return confluence");
                require(std::get<double>(live.store().field({8,2},4))==10 && std::get<double>(live.issued({8,2}).snapshot->record[4])==.5,"process read mutable live fields");
            }
        }
    }
    auto zero=chart_owner({.25},false,0);zero.internal_transition();
    require(zero.launched_count()==1 && std::get<double>(zero.issued({8,0}).snapshot->record[3])==1,"zero-time chart initialization precedes dispatch");
}
void resource_path() {
    using Seize=ankurafathom::des::ReferenceSeize<Person,M>;
    using Delay=ankurafathom::des::ReferenceDelay<Person,M>;
    using Release=ankurafathom::des::ReferenceRelease<Person,M>;
    using Pool=ankurafathom::des::TypedResourcePool<M>;
    std::array<int,5> order{0,1,2,3,4};
    do {
        Sim sim;std::array<std::size_t,5> ids{};
        for(int role:order) {
            if(role==0) ids[role]=sim.add(std::make_unique<Owner>(chart_owner({.25,.5,.125})));
            if(role==1) ids[role]=sim.add(std::make_unique<Seize>(8,11,1,1));
            if(role==2) ids[role]=sim.add(std::make_unique<Pool>(1,1));
            if(role==3) ids[role]=sim.add(std::make_unique<Delay>(8,[](const Token& t) { return std::get<double>(t.snapshot->record[4]); }));
            if(role==4) ids[role]=sim.add(std::make_unique<Release>(8,11));
        }
        sim.connect(ids[0],5,ids[1],0);sim.connect(ids[1],3,ids[2],0);sim.connect(ids[2],2,ids[1],2);
        sim.connect(ids[1],1,ids[3],0);sim.connect(ids[3],1,ids[4],0);sim.connect(ids[4],3,ids[2],0);sim.connect(ids[4],1,ids[0],6);
        sim.inject(.5,ids[0],3,M{Pop::LifecycleInput{.5,0,{}, {newborn(30,.25)}}});
        const std::array<double,7> times{0,.125,.375,.625,.875,1,1.25};
        const std::array<std::size_t,7> completions{0,0,1,1,2,3,4};
        for(std::size_t i=0;i<times.size();++i) {
            sim.run_until_transactional(times[i]);
            const auto& owner=dynamic_cast<const Owner&>(sim.model(ids[0]));
            const auto& pool=dynamic_cast<const Pool&>(sim.model(ids[2])).pool();
            require(owner.returned_count()==completions[i],"shared resource completion schedule");
            require(pool.allocated_units()+pool.available()==1,"resource conservation");
            require(owner.in_flight_count()==pool.allocated_units()+pool.waiting(),"agent/process/resource ownership mismatch");
        }
        const auto& owner=dynamic_cast<const Owner&>(sim.model(ids[0]));
        require(owner.store().next_id()==4 && owner.returned_count()==4 && owner.in_flight_count()==0,"newborn shared identity allocation");
        for(std::uint64_t id=0;id<4;++id) require(std::get<double>(owner.store().field({8,id},3))==111+10*id,"statechart/resource final work");
    }while(std::next_permutation(order.begin(),order.end()));
}
class Failing final:public ankurafathom::devs::Atomic<M> {
public:
    std::unique_ptr<ankurafathom::devs::Atomic<M>> clone()const override { return std::make_unique<Failing>(*this); }
    double time_advance()const override { return INFINITY; }
    std::vector<ankurafathom::devs::PortValue<M>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<Input>&)override { throw std::runtime_error("downstream failure"); }
    void confluent_transition(const std::vector<Input>& bag)override { external_transition(0,bag); }
};
void retry() {
    Sim sim;auto owner=sim.add(std::make_unique<Owner>(synchronous()));
    auto delay=sim.add(std::make_unique<ankurafathom::des::ReferenceDelay<Person,M>>(8,.25));auto fail=sim.add(std::make_unique<Failing>());
    sim.connect(owner,5,delay,0);sim.connect(owner,5,fail,0);sim.step_transactional();
    rejects([&] { sim.step_transactional(); });
    require(dynamic_cast<const Owner&>(sim.model(owner)).launched_count()==2 && sim.model(owner).output().size()==2,"dispatch retry lost ledger or tokens");
    require(dynamic_cast<const ankurafathom::des::ReferenceDelay<Person,M>&>(sim.model(delay)).accepted_count()==0,"failed dispatch partially entered DES");
    sim.disconnect(owner,5,fail,0);sim.connect(delay,1,owner,6);sim.run_until_transactional(.25);
    require(dynamic_cast<const Owner&>(sim.model(owner)).returned_count()==2,"dispatch retry duplicated journey");
}
int main() { try { owner_contract();timer_retirement_and_initial_confluence();chart_delay();resource_path();retry();std::cout<<"Entity-agent ownership, charts and 120 resource graph orders passed\n"; }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
