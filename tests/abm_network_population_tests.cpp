#include "ankurafathom/abm/population_atomic.hpp"
#include <iostream>
namespace {
using namespace ankurafathom;
struct Person {};
using A=abm::PopulationAtomic<Person>; using Store=A::Store; using P=A::Async; using S=A::Sync; using E=Store::Edge; using Input=devs::Input<A::Message>; using Sim=devs::Simulator<A::Message>;
void require(bool b,const char* text) { if(!b) throw std::runtime_error(text); }
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } require(caught,"invalid network operation accepted"); }
E edge(std::uint64_t a,std::uint64_t b) { return {{7,a},{7,b}}; }
Store store(bool directed=false,std::uint64_t first=0) { Store s(7,{{"n",des::FieldKind::integer}},first); s.spawn_many({{std::int64_t(0)},{std::int64_t(0)},{std::int64_t(0)}}); s.configure_network(abm::CsrNetwork({first,first+1,first+2},{{first,first+1},{first+1,first+2}},directed)); return s; }
std::int64_t n(const Store& s,std::uint64_t id) { return std::get<std::int64_t>(s.field({7,id},0)); }
std::int64_t degree(const Store& s,std::uint64_t id) { return static_cast<std::int64_t>(s.network()->neighbors(id).size()); }
P::Effects update(const Store& s,Store::Reference r) { P::Effects e; e.updates.push_back({r,{n(s,r.id)+degree(s,r.id)}}); return e; }
void membership() {
    for(bool directed:{false,true}) {
        auto s=store(directed,10);auto original=s;
        rejects([&] { s.configure_network(abm::CsrNetwork({10,11},{})); });
        require(s.network()->vertices()==original.network()->vertices(),"failed configuration changed vertices");
        s.update({7,10},{std::int64_t(5)}); require(s.network()->edges()==original.network()->edges(),"field update changed graph");
        const auto child=s.spawn({std::int64_t(9)}); require(child.id==13 && degree(s,13)==0,"newborn vertex not isolated");
        s.edit_network({edge(13,10)},{}); s.retire({7,11});
        require(s.network()->vertices()==std::vector<std::uint64_t>({10,12,13}) && s.network()->edge_count()==1,"retirement failed incident-edge cleanup");
        require(original.network()->edge_count()==2 && original.alive({7,11}),"store copies share topology");
        rejects([&] { s.spawn({true}); }); rejects([&] { s.retire({8,10}); });
        require(s.next_id()==14 && s.network()->edge_count()==1,"failed membership mutation changed graph or IDs");
        rejects([&] { s.edit_network({{{8,10},{7,12}}},{}); });
        rejects([&] { s.edit_network({edge(11,12)},{}); });
        rejects([&] { s.edit_network({edge(12,12)},{}); });
        s.retire({7,10});s.retire({7,12});s.retire({7,13});
        require(s.network()->vertices().empty() && s.network()->edges().empty(),"empty live graph invalid");
    }
}
void population_validators() {
    const auto validate=[](const Store& s) { if(s.network()->edge_count()>2) throw std::runtime_error("too many edges"); };
    S sync(store(),validate);
    P asynchronous(store(),[](const auto&,const Store&) { return P::Effects{}; },20,validate);
    rejects([&] { sync.edit_network({edge(0,2)},{}); });
    rejects([&] { asynchronous.edit_network({edge(0,2)},{}); });
    require(sync.store().network()->edge_count()==2 && asynchronous.store().network()->edge_count()==2,"network edit bypassed validator or failed rollback");
    sync.edit_network({edge(0,2)},{edge(0,1)}); asynchronous.edit_network({edge(0,2)},{edge(0,1)});
    require(degree(sync.store(),2)==2 && degree(asynchronous.store(),2)==2,"valid edit retry differs");
}
void phase_membership() {
    S p(store());
    p.add_phase([](auto r,const Store& s) { return Store::Record{degree(s,r.id)}; },{},[](auto r,const Store&) { return S::Lifecycle{r.id==1,r.id==1 ? std::vector<Store::Record>{{std::int64_t(9)}} : std::vector<Store::Record>{}}; });
    p.add_phase([](auto r,const Store& s) { return Store::Record{n(s,r.id)+10*degree(s,r.id)}; });
    p.step();require(n(p.store(),0)==1 && n(p.store(),2)==1 && n(p.store(),3)==9 && p.store().network()->edges().empty(),"phase topology snapshot or cleanup differs");
}
P population() { P p(store(),[](const auto& t,const Store& s) { return update(s,t.agent); });p.schedule({1,{7,0},"count",0});p.schedule({2,{7,1},"future",0});return p; }
A atomic() { return A(population(),[](const auto& c,const Store& s) { return update(s,c.agent); }); }
std::vector<Input> inputs() { return {{0,3,A::LifecycleInput{1,0,{{7,1}},{{std::int64_t(8)}}}},{0,4,A::NetworkInput{1,1,{edge(0,3)}, {}}},{0,4,A::NetworkInput{1,0,{edge(0,2)}, {}}},{0,0,A::Command{1,0,{7,0},"count"}}}; }
void ordering_and_rollback() {
    for(bool reverse:{false,true}) {
        auto a=atomic();auto bag=inputs();if(reverse) std::reverse(bag.begin(),bag.end());a.confluent_transition(bag);
        require(n(a.store(),0)==3 && a.store().next_id()==4 && degree(a.store(),0)==2 && a.async().pending_count()==0,"timer/lifecycle/graph/command ordering differs");
        const auto result=std::get<A::Result>(a.output()[0].value);require(result.snapshot.network()->edges()==a.store().network()->edges(),"result omitted committed topology");
        a.internal_transition();auto clone=a.clone();a.external_transition(1,{{0,4,A::NetworkInput{2,0,{}, {edge(0,3)}}}});
        require(dynamic_cast<A&>(*clone).store().network()->edge_count()==2 && a.store().network()->edge_count()==1,"atomic clone shares graph");
    }
    auto a=atomic();auto bag=inputs();bag.push_back({0,4,A::NetworkInput{1,2,{}, {edge(2,3)}}});
    rejects([&] { a.confluent_transition(bag); });
    require(a.now()==0 && a.store().next_id()==3 && n(a.store(),0)==0 && a.store().alive({7,1}) && a.store().network()->edge_count()==2 && a.async().pending_count()==2 && a.output().empty(),"failed edge edit did not restore timers/lifecycle/graph");
    for(auto edit:std::vector<A::NetworkInput>{{1,2,{edge(0,2)},{}},{1,2,{}, {edge(0,2)}},{1,2,{edge(3,0)},{}},{1,2,{edge(0,0)},{}},{1,2,{edge(0,1)},{}},{1,1,{},{}},{.5,2,{},{}}}) {
        bag=inputs();bag.push_back({0,4,edit});rejects([&] { a.confluent_transition(bag); });require(a.store().next_id()==3 && n(a.store(),0)==0,"failed batch leaked ID or timer effect");
    }
    bag=inputs();bag[1].port=0;rejects([&] { a.confluent_transition(bag); });
    a.confluent_transition(inputs());require(n(a.store(),0)==3,"retry differs");
    bool fail=true;S p(store());p.add_phase([&](auto r,const Store& s) { if(fail) throw std::runtime_error("phase failed");return Store::Record{degree(s,r.id)}; });A sync(p,1);
    rejects([&] { sync.confluent_transition({{0,4,A::NetworkInput{1,0,{edge(0,2)},{edge(0,1)}}}}); });
    require(sync.store().network()->edges()==store().network()->edges() && sync.now()==0,"later phase failure retained edits");
    fail=false;sync.confluent_transition({{0,4,A::NetworkInput{1,0,{edge(0,2)},{edge(0,1)}}}});require(n(sync.store(),2)==2,"sync phase did not read edited topology");
}
class Receiver:public devs::Atomic<A::Message> {
public:
    explicit Receiver(const bool* fail):fail_(fail) {}
    std::unique_ptr<devs::Atomic<A::Message>> clone()const override { return std::make_unique<Receiver>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<A::Message>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<Input>& bag)override { for(const auto& i:bag) { edges=std::get<A::Result>(i.value).snapshot.network()->edges();++count; }if(*fail_) throw std::runtime_error("receiver failure"); }
    void confluent_transition(const std::vector<Input>& bag)override { external_transition(0,bag); }
    std::vector<abm::CsrNetwork::Edge> edges; int count=0;
private:const bool* fail_;
};
void checked_publication() {
    bool fail=true;Sim sim;const auto id=sim.add(std::make_unique<A>(atomic()));const auto receiver=sim.add(std::make_unique<Receiver>(&fail));sim.connect(id,1,receiver,0);
    for(const auto& input:inputs()) sim.inject(1,id,input.port,input.value);
    sim.step_transactional();rejects([&] { sim.step_transactional(); });
    const auto& population=dynamic_cast<const A&>(sim.model(id));require(population.store().next_id()==4 && population.store().network()->edge_count()==2 && !population.output().empty() && dynamic_cast<const Receiver&>(sim.model(receiver)).count==0,"failed downstream publication lost graph state");
    fail=false;sim.step_transactional();require(dynamic_cast<const Receiver&>(sim.model(receiver)).count==1 && dynamic_cast<const A&>(sim.model(id)).store().next_id()==4,"publication retry repeated lifecycle/graph edits");
}
}
int main() { try { membership();population_validators();phase_membership();ordering_and_rollback();checked_publication();std::cout<<"population topology invariants passed\n"; } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
