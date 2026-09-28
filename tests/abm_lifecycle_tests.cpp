#include "ankurafathom/abm/population_atomic.hpp"
#include "ankurafathom/abm/spatial_snapshot.hpp"
#include <iostream>
namespace {
using namespace ankurafathom;
struct Person {};
using A=abm::PopulationAtomic<Person>; using Store=A::Store; using P=A::Async;
using Input=devs::Input<A::Message>;
void require(bool condition,const char* text) { if(!condition) throw std::runtime_error(text); }
template<class F> void rejects(F f) { bool failed=false; try { f(); } catch(const std::exception&) { failed=true; } require(failed,"invalid lifecycle accepted"); }
Store store() { Store s(7,{{"x",des::FieldKind::integer},{"y",des::FieldKind::integer},{"n",des::FieldKind::integer}}); s.spawn({std::int64_t(0),std::int64_t(0),std::int64_t(1)}); return s; }
Store::Record record(std::int64_t x,std::int64_t n) { return {x,std::int64_t(0),n}; }
std::int64_t n(const Store& s,std::uint64_t id) { return std::get<std::int64_t>(s.field({7,id},2)); }
void validate(const Store& s) { (void)abm::SpatialSnapshot<Person>(s,abm::SpatialSnapshot<Person>::Grid{0,1,4,1,false}); }
A::InputRule command() { return [](const A::Command& c,const Store& s) { P::Effects e; auto r=s.record(c.agent); r[2]=n(s,c.agent.id)+10; e.updates.push_back({c.agent,r}); return e; }; }
void synchronous() {
    for(bool reverse:{false,true}) {
        A::Sync p(store(),validate); p.add_phase([](auto ref,const Store& s) { auto r=s.record(ref); r[2]=n(s,ref.id)*2; return r; });
        A a(p,1,command());
        // A birth in an earlier sequence may reuse a cell released by a later input.
        std::vector<Input> bag{{0,3,A::LifecycleInput{1,2,{{7,0}}, {record(1,4)}}},{0,3,A::LifecycleInput{1,0,{}, {record(0,3)}}},{0,0,A::Command{1,0,{7,1},"add"}}};
        if(reverse) std::reverse(bag.begin(),bag.end());
        a.confluent_transition(bag);
        require(!a.store().alive({7,0}) && a.store().next_id()==3 && n(a.store(),1)==26 && n(a.store(),2)==8,"lifecycle ordering/ID/tick mismatch");
        a.internal_transition(); auto clone=a.clone();
        a.external_transition_at(1.5,.5,{{0,3,A::LifecycleInput{1.5,0,{{7,1}},{record(0,5)}}}});
        require(a.store().next_id()==4 && n(a.store(),3)==5,"IDs reused");
        require(dynamic_cast<A&>(*clone).store().alive({7,1}),"clone shares lifecycle state");
    }
}
P population() {
    P p(store(),[](const auto& timer,const Store& s) { P::Effects e; auto r=s.record(timer.agent); r[2]=n(s,timer.agent.id)+1; e.updates.push_back({timer.agent,r}); return e; },100,validate);
    p.schedule({1,{7,0},"due",0}); p.schedule({2,{7,0},"future",0}); return p;
}
void asynchronous() {
    A a(population(),command());
    a.configure_births([](auto ref,const Store& s,double time) { auto r=s.record(ref); r[2]=n(s,ref.id)+100; return P::Birth{r,{{time,"immediate",0},{time+2,"future",0}}}; });
    a.confluent_transition({{0,3,A::LifecycleInput{1,0,{{7,0}},{record(0,3)}}},{0,0,A::Command{1,0,{7,1},"add"}}});
    const auto result=std::get<A::Result>(a.output()[0].value);
    require(result.timers.size()==2 && result.timers[0].agent.id==0 && result.timers[1].agent.id==1,"timer/lifecycle closure order");
    require(n(a.store(),1)==114 && a.async().pending_count()==1 && a.async().next_time()==3,"newborn initialization or timer cancellation");
    rejects([&] { a.configure_births({}); });
}
void rollback() {
    A a(population());
    const auto check=[&] { require(a.now()==0 && a.store().next_id()==1 && n(a.store(),0)==1 && a.async().pending_count()==2 && a.output().empty(),"failed lifecycle mutated runtime"); };
    const std::vector<std::vector<Input>> invalid{
        {{0,3,A::LifecycleInput{1,0,{}, {record(0,3)}}}}, // occupied
        {{0,3,A::LifecycleInput{1,0,{{7,0},{7,0}}, {}}}},
        {{0,3,A::LifecycleInput{1,0,{{8,0}}, {}}}},
        {{0,3,A::LifecycleInput{1,0,{{7,0}}, {{true,std::int64_t(0),std::int64_t(3)}}}}},
        {{0,3,A::LifecycleInput{1,0,{}, {}}},{0,3,A::LifecycleInput{1,0,{}, {}}}},
        {{0,2,A::LifecycleInput{1,0,{}, {}}}},
        {{0,3,A::LifecycleInput{.5,0,{}, {}}}},
        {{0,3,A::LifecycleInput{1,0,{{7,1}}, {record(1,3)}}}}
    };
    for(const auto& bag:invalid) { rejects([&] { a.confluent_transition(bag); }); check(); }
    a.configure_births([](auto ref,const Store& s,double time) { return P::Birth{s.record(ref),{{time-1,"past",0}}}; });
    rejects([&] { a.confluent_transition({{0,3,A::LifecycleInput{1,0,{{7,0}},{record(0,3)}}}}); }); check();
    a.configure_births({});
    a.confluent_transition({{0,3,A::LifecycleInput{1,0,{{7,0}},{record(0,3)}}}});
    require(n(a.store(),1)==3 && a.async().pending_count()==0,"retry changed IDs or timers");
    A::Sync p(store()); A sync(p,1); sync.configure_births([](auto ref,const Store& s,double t) { return P::Birth{s.record(ref),{{t,"bad",0}}}; });
    rejects([&] { sync.confluent_transition({{0,3,A::LifecycleInput{1,0,{}, {record(1,3)}}}}); });
    require(sync.store().next_id()==1,"sync accepted birth timer");
}
void pending_endpoint() {
    P p(store(),[](const auto& timer,const Store&) { P::Effects e; e.publications.push_back({"topic",timer.agent,timer.agent,1}); return e; });
    p.schedule({1,{7,0},"emit",0}); A a(p);
    rejects([&] { a.confluent_transition({{0,3,A::LifecycleInput{1,0,{{7,0}},{}}}}); });
    require(a.store().alive({7,0}) && a.async().pending_count()==1 && a.async().pending_publications().empty(),"queued endpoint retirement failed rollback");
}
}
int main() { try { synchronous(); asynchronous(); rollback(); pending_endpoint(); std::cout<<"population lifecycle invariants passed\n"; } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
