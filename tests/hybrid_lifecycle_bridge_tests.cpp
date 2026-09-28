#include "ankurafathom/hybrid/event_to_lifecycle.hpp"
#include "ankurafathom/hybrid/population_result_publisher.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <array>
#include <iostream>

struct Person {};
struct Trigger { std::uint64_t key; std::vector<ankurafathom::des::EntityRef<Person>> retirements; std::vector<ankurafathom::abm::PopulationStore<Person>::Record> births; };
using Message=ankurafathom::hybrid::LifecycleBridgeMessage<Trigger,Person>;
using Bridge=ankurafathom::hybrid::EventToLifecycle<Trigger,Person>;
using Pop=ankurafathom::abm::PopulationAtomic<Person,Message>;
using Store=Pop::Store;
using Input=ankurafathom::devs::Input<Message>;
using Sim=ankurafathom::devs::Simulator<Message>;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }require(caught,"invalid lifecycle operation accepted"); }
Store initial(std::uint64_t first=0) {
    Store s(8,{{"work",ankurafathom::des::FieldKind::real},{"name",ankurafathom::des::FieldKind::string}},first);
    s.spawn({1.,std::string("first")});s.spawn({3.,std::string("second")});
    s.configure_network(ankurafathom::abm::CsrNetwork({first,first+1},{{first,first+1}},false));return s;
}
Bridge bridge(const Store& s=initial(),Bridge::Limits limits={}) {
    return Bridge(s,[](const Trigger& e) { return e.key; },[](const Trigger& e,double) { return Bridge::Change{e.retirements,e.births}; },limits);
}
Input event(std::uint64_t key,std::vector<Store::Record> births={},std::vector<Store::Reference> retirements={}) {
    return {7,0,Message{Trigger{key,std::move(retirements),std::move(births)}}};
}
void bridge_contract() {
    auto b=bridge();
    rejects([&] { b.internal_transition(); });rejects([&] { b.confluent_transition({event(1)}); });
    b.external_transition(.125,{event(4,{{4.,std::string("four")}}),event(2,{{2.,std::string("two")}},{{8,0}})});
    auto old=b.output();
    require(old.size()==2 && std::get<Pop::LifecycleInput>(old[0].value).sequence==2 && std::get<Pop::LifecycleInput>(old[1].value).sequence==4,"birth event ordering");
    require(b.event_count()==2 && b.time()==.125,"accepted event accounting");
    auto copy=b.clone();
    rejects([&] { b.external_transition(0,{event(5)}); });
    rejects([&] { b.confluent_transition({event(5),event(2)}); });
    require(b.event_count()==2 && b.output().size()==2,"failed confluence consumed events");
    b.confluent_transition({event(5)});
    require(b.output().size()==1 && b.event_count()==3 && copy->output().size()==2,"clone or no-op event");
    b.internal_transition();
    std::vector<std::vector<Input>> bad{
        {event(6),event(6)},{event(6),event(2)},
        {event(6,{{NAN,std::string("bad")}})},{event(6,{{1.}})},
        {event(6,{{std::int64_t{1},std::string("bad")}})},
        {event(6,{},{{9,0}})},{event(6,{},{{8,Store::max_entity_id+1}})},
        {event(6,{},{{8,0},{8,0}})},{event(6,{},{{8,0}}),event(7,{},{{8,0}})},{}
    };
    auto wrong=event(6);wrong.port=1;bad.push_back({wrong});
    for(const auto& bag:bad) {
        rejects([&] { b.external_transition_at(.25,.125,bag); });
        require(b.time()==.125 && b.event_count()==3 && b.output().empty(),"bad lifecycle bag committed partially");
    }
    rejects([&] { b.external_transition_at(.25,.1,{event(6)}); });
    rejects([&] { b.external_transition_at(INFINITY,INFINITY,{event(6)}); });
    b.external_transition_at(.25,.125,{event(6)});require(b.event_count()==4,"failed keys retained");
    auto bounded=bridge(initial(),{1,1});
    rejects([&] { bounded.external_transition(0,{event(1,{{1.,std::string("a")},{2.,std::string("b")}})}); });
    rejects([&] { bounded.external_transition(0,{event(1,{},{{8,0},{8,1}})}); });
    rejects([&] { bounded.external_transition(0,{event(1,{{1.,std::string("a")}}),event(2,{{2.,std::string("b")}})}); });
    require(bounded.event_count()==0,"limit failure consumed keys");
    auto zero=bridge(initial(),{0,0});zero.external_transition(0,{event(std::numeric_limits<std::uint64_t>::max())});
    require(zero.event_count()==1,"full uint64 event sequence unsupported");
    rejects([] { Bridge(initial(),{},{}); });
}
void population_ownership() {
    Sim sim;
    auto bid=sim.add(std::make_unique<Bridge>(bridge()));
    Pop::Sync runtime(initial());runtime.add_phase([](auto ref,const Store& s) { auto r=s.record(ref);std::get<double>(r[0])+=1;return r; });
    auto pid=sim.add(std::make_unique<Pop>(std::move(runtime),.5));sim.connect(bid,1,pid,3);
    sim.inject(.5,bid,0,event(3,{{10.,std::string("new")}},{{8,0}}).value);
    sim.run_until_transactional(.5);
    const auto& p=dynamic_cast<const Pop&>(sim.model(pid));
    require(p.store().next_id()==3 && p.store().active_count()==2 && !p.store().alive({8,0}),"population did not own ID allocation");
    require(std::get<double>(p.store().field({8,1},0))==4 && std::get<double>(p.store().field({8,2},0))==10,"bridge birth reran prior tick");
    require(p.store().network()->edges().empty(),"retirement retained network edge");
    sim.run_until_transactional(1);
    require(std::get<double>(dynamic_cast<const Pop&>(sim.model(pid)).store().field({8,2},0))==11,"newborn missed next phase");
    // The bridge validates namespaces/schema, while population validates live references.
    sim.inject(1.25,bid,0,event(4,{},{{8,99}}).value);sim.step_transactional();
    rejects([&] { sim.step_transactional(); });
    require(sim.model(bid).output().size()==1 && dynamic_cast<const Pop&>(sim.model(pid)).store().next_id()==3,"invalid live reference partially changed population");
    // Allocation exhaustion is a receiving population error, not a shadow bridge allocator.
    Store full(8,initial().schema(),Store::max_entity_id);full.spawn({1.,std::string("last")});
    Sim exhausted;auto b=exhausted.add(std::make_unique<Bridge>(bridge(full)));
    auto population=exhausted.add(std::make_unique<Pop>(Pop::Sync(full),1));exhausted.connect(b,1,population,3);
    exhausted.inject(.25,b,0,event(1,{{2.,std::string("overflow")}}).value);exhausted.step_transactional();
    rejects([&] { exhausted.step_transactional(); });
    require(exhausted.model(b).output().size()==1 && dynamic_cast<const Pop&>(exhausted.model(population)).store().active_count()==1,"ID exhaustion lost pending birth");
}
class Failing final:public ankurafathom::devs::Atomic<Message> {
public:
    std::unique_ptr<ankurafathom::devs::Atomic<Message>> clone()const override { return std::make_unique<Failing>(*this); }
    double time_advance()const override { return INFINITY; }
    std::vector<ankurafathom::devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<Input>&)override { throw std::runtime_error("downstream failure"); }
    void confluent_transition(const std::vector<Input>& bag)override { external_transition(0,bag); }
};
void destination_and_retry() {
    auto b=bridge();b.external_transition(.25,{event(9,{{5.,std::string("new")}},{{8,0}})});
    auto value=b.output()[0].value;
    Store wrong_store(9,initial().schema());Pop wrong_namespace(Pop::Sync(wrong_store),1);
    rejects([&] { wrong_namespace.external_transition(.25,{{7,3,value}}); });
    Store wrong_fields(8,{{"other",ankurafathom::des::FieldKind::real},{"name",ankurafathom::des::FieldKind::string}});
    Pop wrong_schema(Pop::Sync(wrong_fields),1);
    rejects([&] { wrong_schema.external_transition(.25,{{7,3,value}}); });
    Pop direct(Pop::Sync(initial()),1);
    direct.external_transition(.25,{{7,3,value}});direct.internal_transition();
    auto replay=std::get<Pop::LifecycleInput>(value);replay.time=.5;replay.retirements={};
    rejects([&] { direct.external_transition(.25,{{8,3,Message{replay}}}); });
    require(direct.now()==.25 && direct.store().next_id()==3,"cross-producer lifecycle replay committed");
    replay.sequence=10;direct.external_transition(.25,{{8,3,Message{replay}}});
    require(direct.store().next_id()==4,"rejected sequence poisoned later birth");
    Sim sim;auto bid=sim.add(std::make_unique<Bridge>(bridge()));
    auto pid=sim.add(std::make_unique<Pop>(Pop::Sync(initial()),1));auto fail=sim.add(std::make_unique<Failing>());
    sim.connect(bid,1,pid,3);sim.connect(bid,1,fail,0);
    sim.inject(.25,bid,0,event(11,{{7.,std::string("born")}},{{8,0}}).value);sim.step_transactional();
    rejects([&] { sim.step_transactional(); });
    require(sim.model(bid).output().size()==1 && dynamic_cast<const Pop&>(sim.model(pid)).store().next_id()==2,"failed delivery consumed allocation or publication");
    sim.disconnect(bid,1,fail,0);sim.run_until_transactional(.25);
    const auto& pop=dynamic_cast<const Pop&>(sim.model(pid));
    require(pop.store().next_id()==3 && pop.store().active_count()==2 && std::get<double>(pop.store().field({8,2},0))==7,"retry duplicated birth or lost target sequence");
}
struct Ticket {};
using Token=ankurafathom::des::EntityToken<Ticket>;
using M=std::variant<Token,ankurafathom::des::QueuePull,ankurafathom::abm::PopulationCommand<Person>,
    ankurafathom::abm::PopulationResult<Person>,ankurafathom::abm::PopulationTopicInput<Person>,
    ankurafathom::abm::PopulationLifecycleInput<Person>,ankurafathom::abm::PopulationNetworkInput<Person>,
    ankurafathom::hybrid::PopulationSnapshot<Person>,ankurafathom::hybrid::ScalarPublication>;
using P=ankurafathom::abm::PopulationAtomic<Person,M>;
using B=ankurafathom::hybrid::EventToLifecycle<Token,Person,M>;
using D=ankurafathom::des::ReferenceDelay<Ticket,M>;
using A=ankurafathom::hybrid::PopulationResultPublisher<Person,M>;
std::unique_ptr<P> population(bool asynchronous) {
    if(!asynchronous) {
        P::Sync runtime(initial());runtime.add_phase([](auto ref,const Store& s) { auto r=s.record(ref);std::get<double>(r[0])+=1;return r; });
        return std::make_unique<P>(std::move(runtime),.5);
    }
    P::Async runtime(initial(),[](const auto& timer,const Store& s) {
        P::Effects effects;auto r=s.record(timer.agent);std::get<double>(r[0])+=2;effects.updates.push_back({timer.agent,r});return effects;
    });
    runtime.schedule({.25,{8,0},"due",0});runtime.schedule({.5,{8,1},"due",0});runtime.schedule({2,{8,0},"cancel",0});
    auto pop=std::make_unique<P>(std::move(runtime));
    pop->configure_births([](auto ref,const Store& s,double time) {
        return P::Async::Birth{s.record(ref),{{time+.25,"due",0},{time+2,"cancel",0}}};
    });return pop;
}
void des_histories() {
    std::array<int,4> order{0,1,2,3};
    do { for(bool asynchronous:{false,true}) {
        ankurafathom::devs::Simulator<M> sim;std::array<std::size_t,4> ids{};
        for(int role:order) {
            if(role==0) ids[role]=sim.add(std::make_unique<D>(7,.125));
            if(role==1) ids[role]=sim.add(std::make_unique<B>(initial(),[](const Token& t) { return t.entity.id; },[](const Token& t,double) {
                B::Change e;
                if(t.entity.id==0) e={{{8,0}},{{5.,std::string("five")}}};
                if(t.entity.id==1) e={{},{{2.,std::string("two")}}};
                if(t.entity.id==2) e={{{8,2}},{{4.,std::string("four")}}};
                if(t.entity.id==3) e={{{8,1},{8,3},{8,4}}, {}};
                if(t.entity.id==4) e={{},{{7.,std::string("seven")}}};
                return e;
            }));
            if(role==2) ids[role]=sim.add(population(asynchronous));
            if(role==3) ids[role]=sim.add(std::make_unique<A>(initial()));
        }
        sim.connect(ids[0],1,ids[1],0);sim.connect(ids[1],1,ids[2],3);sim.connect(ids[2],1,ids[3],0);
        for(std::uint64_t i=0;i<5;++i) sim.inject(.125+.25*i,ids[0],0,M{Token{{7,i}}});
        const std::array<double,7> sync_sum{4,8,12,10,0,7,8},async_sum{4,8,14,13,0,7,9};
        const std::array<std::size_t,7> counts{2,2,3,3,0,1,1},allocated{2,3,4,5,5,6,6};
        for(std::size_t k=0;k<7;++k) {
            sim.run_until_transactional(.25*k);
            const auto& p=dynamic_cast<const P&>(sim.model(ids[2]));
            const auto& snapshot=dynamic_cast<const A&>(sim.model(ids[3])).snapshot().population;
            double sum=0;
            for(auto id=snapshot.first_id();id<snapshot.next_id();++id) if(snapshot.alive({8,id})) sum+=std::get<double>(snapshot.field({8,id},0));
            require(sum==(asynchronous?async_sum[k]:sync_sum[k]),"DES-triggered population work history");
            require(p.store().active_count()==counts[k] && snapshot.active_count()==counts[k] && snapshot.next_id()==allocated[k],"DES-triggered membership history");
            if(k) require(snapshot.network()->edges().empty(),"DES retirement left incident edge");
        }
        if(asynchronous) {
            sim.run_until_transactional(2.5);
            const auto& p=dynamic_cast<const P&>(sim.model(ids[2]));
            require(p.store().active_count()==1 && std::get<double>(p.store().field({8,5},0))==9,"retired timers survived cancellation");
        }
    }}while(std::next_permutation(order.begin(),order.end()));
}
int main() { try { bridge_contract();population_ownership();destination_and_retry();des_histories();std::cout<<"Lifecycle bridge: contracts, ownership, 336 DES/sync/async snapshots passed\n"; }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
