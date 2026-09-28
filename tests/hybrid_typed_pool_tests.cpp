#include "ankurafathom/hybrid/typed_agent_pool_atomic.hpp"
#include "ankurafathom/des/reference_resource.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <array>
#include <iostream>

namespace af=ankurafathom;
struct Worker {};struct Job {};
using Core=af::hybrid::TypedAgentPool<Worker>;
using Control=af::hybrid::TypedPoolControl<Worker>;
using Token=af::des::EntityToken<Job>;
using M=std::variant<Control,Core::Result,Core::Notification,af::abm::PopulationResult<Worker>,
    af::des::Seize,af::des::Release,af::des::Grant,Token,af::des::QueuePull>;
using Atomic=af::hybrid::TypedAgentPoolAtomic<Worker,M>;
using Input=af::devs::Input<M>;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }require(caught,"invalid typed pool operation accepted"); }
Core::Record record(std::int64_t capacity,bool enabled=true) { return {capacity,std::int64_t{0},enabled,std::int64_t{0},std::int64_t{0}}; }
Core::Store store(std::vector<std::int64_t> capacities={2,1},std::uint64_t first=0) {
    using F=af::des::FieldKind;
    Core::Store s(7,{{"capacity",F::integer},{"allocated",F::integer},{"enabled",F::boolean},{"assigned",F::integer},{"released",F::integer}},first);
    for(const auto n:capacities) s.spawn(record(n));return s;
}
std::size_t capacity(Core::Reference ref,const Core::Store& s) {
    const auto n=std::get<std::int64_t>(s.field(ref,0));if(n<0) throw std::invalid_argument("negative worker capacity");
    return std::get<bool>(s.field(ref,2))?static_cast<std::size_t>(n):0;
}
Core::Record notify(const Core::Notification& value,const Core::Store& s) {
    auto r=s.record(value.agent);const auto index=value.assigned?3:4;
    r[index]=std::get<std::int64_t>(r[index])+static_cast<std::int64_t>(value.units);return r;
}
Core core(af::des::QueueDiscipline discipline=af::des::QueueDiscipline::fifo,std::size_t bound=100) {
    return Core(store(),1,capacity,8,discipline,notify,bound);
}
std::int64_t field(const Core& c,std::uint64_t id,std::size_t column) { return std::get<std::int64_t>(c.store().field({7,id},column)); }
void ownership() {
    auto c=core();Core::Change change;change.time=.5;change.requests={{20,2},{10,2},{30,1}};
    auto r=c.transact(change);
    require(r.grants.size()==1 && r.grants[0].request_id==10 && field(c,0,1)==2 && field(c,1,1)==0,"canonical request order or FIFO head blocking");
    require(c.pool().waiting()==2 && r.notifications==std::vector<Core::Notification>{{10,{7,0},2,true}},"initial assignment notification");
    const auto original=c;const auto stats=c.statistics(2);
    for(int bad=0;bad<8;++bad) {
        Core::Change invalid;invalid.time=1;
        if(bad==0) invalid.retirements={{7,0}};
        if(bad==1) { auto row=c.store().record({7,0});row[0]=std::int64_t{1};invalid.updates={{{7,0},row}}; }
        if(bad==2) { auto row=c.store().record({7,0});row[2]=false;invalid.updates={{{7,0},row}}; }
        if(bad==3) invalid.releases={{10},{10}};
        if(bad==4) invalid.requests={{10,1}};
        if(bad==5) invalid.requests={{40,1,0,true}};
        if(bad==6) { auto row=record(1);row[1]=std::int64_t{1};invalid.births={row}; }
        if(bad==7) { auto row=c.store().record({7,0});row[1]=std::int64_t{0};invalid.updates={{{7,0},row}}; }
        rejects([&] { c.transact(invalid); });
        require(c.time()==.5 && c.assignments()==original.assignments() && c.statistics(2)==stats && c.store().next_id()==2 && field(c,0,3)==2,"failed transaction changed owner");
    }
    change={};change.time=1;change.releases={{10}};change.retirements={{7,0}};change.births={record(3)};
    r=c.transact(change);
    require(r.born==std::vector<Core::Reference>{{7,2}} && !c.store().alive({7,0}) && c.pool().capacity()==4,"release/retirement/birth order");
    require(r.grants.size()==2 && r.grants[0].request_id==20 && r.grants[1].request_id==30,"waiting grants after workforce change");
    require(c.assignments().at(20)==std::vector<Core::Share>{{{7,1},1},{{7,2},1}} && field(c,2,1)==2,"split grant stable workforce order");
    require(r.notifications==std::vector<Core::Notification>{{10,{7,0},2,false},{20,{7,1},1,true},{20,{7,2},1,true},{30,{7,2},1,true}},"canonical broker batch ordering");
    const auto projected=c.statistics(2);
    require(projected.capacity_time==7 && projected.allocated_time==4 && projected.waiting_request_time==1 && projected.live_agent_time==4,"time weighted workforce accounting");
    require(c.time()==1 && c.statistics(2)==projected && original.time()==.5,"statistics observation or copy changed core");
    rejects([&] { c.add_phase([](auto ref,const auto& s) { return s.record(ref); }); });
    rejects([&] { c.statistics(.5); });rejects([&] { c.statistics(INFINITY); });
    Core::Change backwards;backwards.time=.75;rejects([&] { c.transact(backwards); });
}
void rules_and_rollback() {
    Core c(store({1,2}),1,capacity,8,af::des::QueueDiscipline::fifo,notify);
    c.add_phase([](auto ref,const auto& s) {
        auto r=s.record(ref);r[0]=std::get<std::int64_t>(s.field({7,1-ref.id},0));return r;
    });
    Core::Change change;change.run_phases=true;change.requests={{4,3}};
    c.transact(change);require(field(c,0,0)==2 && field(c,1,0)==1,"phase was not Jacobi");
    change={};change.time=1;change.run_phases=true;
    rejects([&] { c.transact(change); });require(field(c,0,0)==2 && c.time()==0,"invalid phase changed capacity");
    change.releases={{4}};c.transact(change);require(field(c,0,0)==1 && field(c,1,0)==2,"released phase did not run");
    for(int mode=0;mode<3;++mode) {
        Core broken(store(),1,capacity,8,af::des::QueueDiscipline::fifo,[mode](const auto& n,const auto& s) {
            auto r=notify(n,s);
            if(mode==0) r[1]=std::int64_t{0};
            if(mode==1) r[0]=std::int64_t{9};
            if(mode==2) throw std::runtime_error("injected handler failure");
            return r;
        });
        change={};change.requests={{1,2}};rejects([&] { broken.transact(change); });
        require(broken.pool().allocated()==0 && broken.assignments().empty() && field(broken,0,3)==0,"broker failure leaked allocation");
        change.requests.clear();change.births={record(1)};require(broken.transact(change).born[0].id==2,"failure consumed identity");
    }
    auto bounded=core(af::des::QueueDiscipline::fifo,1);change={};change.requests={{1,3}};
    rejects([&] { bounded.transact(change); });require(bounded.pool().allocated()==0,"topic overflow leaked pool grant");
    change.requests={{1,2}};bounded.transact(change);require(field(bounded,0,1)==2,"topic overflow consumed request ID");
    change={};change.releases={{1}};bounded.transact(change);require(field(bounded,0,1)==0 && field(bounded,0,4)==2,"unassignment handler");
    Core dependent(store(),1,[](auto ref,const auto& s) { return capacity(ref,s)+static_cast<std::size_t>(std::get<std::int64_t>(s.field(ref,1))); },8);
    change={};change.requests={{1,1}};rejects([&] { dependent.transact(change); });require(dependent.pool().allocated()==0,"allocation-dependent capacity accepted");
    Core invalid_phase(store(),1,capacity,8);invalid_phase.add_phase([](auto ref,const auto& s) { auto r=s.record(ref);r[1]=std::int64_t{9};return r; });
    change={};change.run_phases=true;rejects([&] { invalid_phase.transact(change); });
    auto mismatched=store();auto row=mismatched.record({7,0});row[1]=std::int64_t{1};mismatched.update({7,0},row);
    rejects([&] { Core bad(mismatched,1,capacity,8); });rejects([] { Core bad(store(),0,{},8); });
    rejects([] { Core bad(store(),2,capacity,8); });rejects([] { Core bad(store(),1,capacity,0); });
    rejects([] { Core bad(store(),1,[](auto,const auto&) { return std::numeric_limits<std::size_t>::max(); },8); });
    const auto max=std::numeric_limits<std::int64_t>::max();
    rejects([&] { Core bad(store({max,max,max}),1,capacity,8); });
    Core huge(store({max}),1,capacity,8);rejects([&] { huge.statistics(std::numeric_limits<double>::max()); });
    Core widest(store({max,max,1}),1,capacity,std::numeric_limits<std::size_t>::max());
    change={};change.requests={{1,std::numeric_limits<std::size_t>::max()}};widest.transact(change);
    require(widest.pool().available()==0 && widest.assignments().at(1).size()==3 && field(widest,2,1)==1,"full size_t capacity split");
    change={};change.releases={{1}};widest.transact(change);
    require(widest.pool().available()==std::numeric_limits<std::size_t>::max(),"full size_t capacity release");
    Core last(store({1},Core::Store::max_entity_id),1,capacity,8);change={};change.births={record(1)};
    rejects([&] { last.transact(change); });require(last.store().active_count()==1,"exhausted birth changed workforce");
    Core reentrant(store(),1,capacity,8);reentrant.add_phase([&reentrant](auto ref,const auto& s) {
        Core::Change nested;reentrant.transact(nested);return s.record(ref);
    });
    change={};change.run_phases=true;rejects([&] { reentrant.transact(change); });
    change.run_phases=false;reentrant.transact(change);require(reentrant.pool().capacity()==3,"busy guard not cleared after failure");
}
void queue_equivalence() {
    for(const auto discipline:{af::des::QueueDiscipline::fifo,af::des::QueueDiscipline::lifo,af::des::QueueDiscipline::priority}) {
        auto c=core(discipline);af::des::ResourcePool plain(3,8,discipline);
        std::vector<std::uint64_t> active;
        for(int i=0;i<12;++i) {
            Core::Change change;change.time=i*.25;
            if(i<4) change.requests={{static_cast<std::uint64_t>(2*i+2),2,1},{static_cast<std::uint64_t>(2*i+1),1,-1}};
            if(!active.empty()) { change.releases={{active.front()}};active.erase(active.begin()); }
            std::sort(change.requests.begin(),change.requests.end(),[](auto a,auto b) { return a.request_id<b.request_id; });
            std::vector<af::devs::Input<af::des::ResourceMessage>> bag;
            for(auto x:change.releases) bag.push_back({0,0,x});for(auto x:change.requests) bag.push_back({0,0,x});
            plain.external_transition(.25,bag);const auto output=plain.output();const auto result=c.transact(change);
            require(output.size()==result.grants.size() && plain.waiting()==c.pool().waiting() && plain.available()==c.pool().available(),"typed/static pool mismatch");
            for(std::size_t j=0;j<output.size();++j) {
                const auto grant=std::get<af::des::Grant>(output[j].value);
                require(grant.request_id==result.grants[j].request_id && grant.units==result.grants[j].units,"queue discipline grant mismatch");active.push_back(grant.request_id);
            }
            if(!output.empty()) plain.internal_transition();
        }
    }
}
void atomic_contract() {
    Atomic a(core());Core::Change c;c.time=.25;c.requests={{(std::uint64_t{65535}<<48)|5,1}};
    a.external_transition_at(.25,.25,{{8,2,M{Control{0,c}}}});
    require(a.time_advance()==0 && a.output()[0].port==65536 && a.core().pool().allocated()==1,"typed pool atomic routing");
    auto copy=a.clone();auto invalid=c;invalid.time=.5;
    rejects([&] { a.confluent_transition({{8,2,M{Control{1,invalid}}}}); });
    require(a.revision()==0 && a.output().size()==4,"failed confluence lost pending publication");
    c.requests.clear();c.releases={{(std::uint64_t{65535}<<48)|5}};
    a.confluent_transition({{8,2,M{Control{1,c}}}});a.internal_transition();
    require(a.core().pool().allocated()==0 && dynamic_cast<Atomic&>(*copy).core().pool().allocated()==1,"atomic clone aliases ledger");
    c.time=.5;
    for(const auto& input:std::vector<Input>{{8,2,M{Control{1,c}}},{9,2,M{Control{2,c}}},{8,0,M{Control{2,c}}},{8,2,M{af::des::Seize{3,1}}}})
        rejects([&] { a.external_transition_at(.5,.25,{input}); });
    c.releases.clear();a.external_transition_at(.5,.25,{{8,2,M{Control{2,c}}},{9,0,M{af::des::Seize{3,1}}}});
    require(a.core().pool().allocated()==1 && a.revision()==2,"atomic retry or merged request");
}
void process_orders() {
    std::array<int,4> order{0,1,2,3};
    do {
        af::devs::Simulator<M> sim;std::array<std::size_t,4> ids{};
        for(const auto kind:order) {
            std::unique_ptr<af::devs::Atomic<M>> model;
            if(kind==0) model=std::make_unique<Atomic>(Core(store({1}),1,capacity,8,af::des::QueueDiscipline::fifo,notify));
            if(kind==1) model=std::make_unique<af::des::ReferenceSeize<Job,M>>(9,4,2,1);
            if(kind==2) model=std::make_unique<af::des::ReferenceDelay<Job,M>>(9,.5);
            if(kind==3) model=std::make_unique<af::des::ReferenceRelease<Job,M>>(9,4);
            ids[kind]=sim.add(std::move(model));
        }
        sim.connect(ids[1],3,ids[0],0);sim.connect(ids[0],3,ids[1],2);
        sim.connect(ids[1],1,ids[2],0);sim.connect(ids[2],1,ids[3],0);sim.connect(ids[3],3,ids[0],0);
        sim.inject(0,ids[1],0,M{Token{{9,1}}});sim.inject(0,ids[1],0,M{Token{{9,0}}});
        sim.inject(0,ids[1],0,M{Token{{9,2}}});
        Core::Change workforce;workforce.time=.25;workforce.births={record(1)};
        sim.inject(.25,ids[0],2,M{Control{0,workforce}});
        for(const double t:{0.,.125,.25,.5,.75,1.,1.5}) sim.run_until_transactional(t);
        const auto& c=dynamic_cast<const Atomic&>(sim.model(ids[0])).core();
        require(c.pool().allocated()==0 && c.pool().waiting()==0 && field(c,0,3)+field(c,1,3)==3 && field(c,0,4)+field(c,1,4)==3,"typed process network did not release workforce");
        const auto stats=c.statistics(1.5);
        require(stats.capacity_time==2.75 && stats.allocated_time==1.5 && stats.waiting_request_time==.75,"typed process timing/accounting mismatch");
    }while(std::next_permutation(order.begin(),order.end()));
}
class Failing final:public af::devs::Atomic<M> {
public:
    std::unique_ptr<af::devs::Atomic<M>> clone()const override { return std::make_unique<Failing>(*this); }
    double time_advance()const override { return INFINITY; }
    std::vector<af::devs::PortValue<M>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<Input>&)override { throw std::runtime_error("downstream failure"); }
    void confluent_transition(const std::vector<Input>& bag)override { external_transition(0,bag); }
};
void publication_retry() {
    af::devs::Simulator<M> sim;const auto owner=sim.add(std::make_unique<Atomic>(core()));
    const auto fail=sim.add(std::make_unique<Failing>());
    sim.connect(owner,Atomic::notification_port,fail,0);sim.inject(.25,owner,0,M{af::des::Seize{1,2}});
    sim.step_transactional();rejects([&] { sim.step_transactional(); });
    const auto& pending=dynamic_cast<const Atomic&>(sim.model(owner));
    require(pending.core().pool().allocated_units()==2 && field(pending.core(),0,3)==2 && pending.output().size()==4,"failed publication rolled back committed grant");
    sim.disconnect(owner,Atomic::notification_port,fail,0);sim.run_until_transactional(.25);
    sim.inject(.5,owner,0,M{af::des::Release{1}});sim.run_until_transactional(.5);
    const auto& done=dynamic_cast<const Atomic&>(sim.model(owner));
    require(done.core().pool().allocated_units()==0 && field(done.core(),0,3)==2 && field(done.core(),0,4)==2,"publication retry duplicated broker work");
}
int main() {
    ownership();rules_and_rollback();queue_equivalence();atomic_contract();process_orders();publication_retry();
    std::cout<<"typed agent pool ownership, broker rollback, phases, disciplines and 24 process orders passed\n";
}
