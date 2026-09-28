#include "ankurafathom/abm/population_atomic.hpp"
#include <iostream>

namespace {
using namespace ankurafathom;
struct Person {};
using A=abm::PopulationAtomic<Person>;
using P=A::Async; using S=A::Sync; using Store=A::Store;
using Message=A::Message; using Command=A::Command; using Result=A::Result;
using Sim=devs::Simulator<Message>;
void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
template<class F> void rejects(F f) { bool b=false; try { f(); } catch(const std::exception&) { b=true; } require(b,"invalid atomic operation accepted"); }
Store store() { Store s(2,{{"n",des::FieldKind::integer}}); s.spawn({std::int64_t(1)}); return s; }
std::int64_t n(const Store& s) { return std::get<std::int64_t>(s.field({2,0},0)); }
P::Effects set(const Store& s,std::int64_t value) { P::Effects e; e.updates.push_back({{s.store_id(),0},{value}}); return e; }
const A& atomic(const Sim& sim,std::size_t id) { return dynamic_cast<const A&>(sim.model(id)); }
P async() { P p(store(),[](const auto&,const Store& s) { return set(s,n(s)*2); }); p.schedule({1,{2,0},"double",0}); return p; }
A::InputRule input() { return [](const Command& c,const Store& s) { return set(s,n(s)+std::stoll(c.kind)); }; }
void ordering() {
    for(bool reverse:{false,true}) {
        A a(async(),input());
        std::vector<devs::Input<Message>> bag{{8,0,Command{1,1,{2,0},"3"}},{9,0,Command{1,0,{2,0},"5"}}};
        if(reverse) std::reverse(bag.begin(),bag.end());
        a.confluent_transition(bag);
        require(n(a.store())==10 && a.time_advance()==0,"async input/timer confluence differs");
        const auto result=std::get<Result>(a.output().front().value);
        require(result.timers.size()==1 && result.commands[0].sequence==0 && n(result.snapshot)==10,"result not canonical/committed");
        a.internal_transition(); require(std::isinf(a.time_advance()),"publication repeated event");
    }
    S s(store()); s.add_phase([](auto r,const Store& snapshot) { auto v=snapshot.record(r); v[0]=n(snapshot)*2; return v; });
    A a(s,0.1,input());
    a.confluent_transition({{0,0,Command{0.1,0,{2,0},"2"}}});
    require(n(a.store())==6,"sync phase did not read same-time input");
    a.confluent_transition({{0,0,Command{0.1,1,{2,0},"1"}}});
    require(n(a.store())==7,"publication confluence repeated sync tick");
    a.internal_transition(); require(a.next_event_time()==0.2,"sync tick drifted");
    auto copy=a.clone(); copy->internal_transition(); require(n(a.store())==7,"atomic clone shares state");
}
void rollback() {
    A a(async(),input());
    rejects([&] { a.confluent_transition({{0,0,Command{1,0,{2,0},"3"}},{0,0,Command{1,1,{2,0},"bad"}}}); });
    require(n(a.store())==1 && a.async().pending_count()==1 && a.output().empty(),"failed command committed earlier timer or command");
    rejects([&] { a.external_transition_at(2,2,{{0,0,Command{2,0,{2,0},"1"}}}); });
    rejects([&] { a.confluent_transition({{0,0,Command{1,0,{2,0},"1"}},{0,0,Command{1,0,{2,0},"1"}}}); });
    rejects([&] { a.external_transition_at(.5,.5,{{0,1,Command{.5,0,{2,0},"1"}}}); });
    rejects([&] { a.external_transition_at(.5,.5,{{0,0,Command{std::nextafter(.5,1.),0,{2,0},"1"}}}); });
    a.confluent_transition({{0,0,Command{1,0,{2,0},"3"}}}); require(n(a.store())==5,"atomic retry differs");
}
class Receiver final:public devs::Atomic<Message> {
public:
    explicit Receiver(const bool* fail):fail_(fail) {}
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<Receiver>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<devs::Input<Message>>& bag)override {
        for(const auto& i:bag) { value=n(std::get<Result>(i.value).snapshot); ++count; }
        if(*fail_) throw std::runtime_error("downstream failure");
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override { external_transition(0,bag); }
    std::int64_t value=0; int count=0;
private: const bool* fail_;
};
void coupled_retry() {
    bool fail=true; Sim sim;
    const auto id=sim.add(std::make_unique<A>(async(),input()));
    const auto receiver=sim.add(std::make_unique<Receiver>(&fail)); sim.connect(id,1,receiver,0);
    sim.inject(1,id,0,Command{1,0,{2,0},"3"});
    sim.step_transactional(); require(n(atomic(sim,id).store())==5,"detection did not commit");
    rejects([&] { sim.step_transactional(); });
    require(n(atomic(sim,id).store())==5 && !atomic(sim,id).output().empty() && dynamic_cast<const Receiver&>(sim.model(receiver)).count==0,
            "publication failure lost committed result or receiver rollback");
    fail=false; sim.step_transactional();
    require(dynamic_cast<const Receiver&>(sim.model(receiver)).value==5 && atomic(sim,id).output().empty(),"publication retry reran rules");
}
void calendar_agreement() {
    P direct(store(),[](const auto&,const Store& s) { return set(s,n(s)+1); });
    for(int i=0;i<60;++i) direct.schedule({double((i*17)%31)*.03,{2,0},"increment",0});
    Sim sim; sim.add(std::make_unique<A>(direct));
    std::map<double,std::pair<std::vector<P::Timer>,std::int64_t>> expected;
    while(std::isfinite(direct.next_time())) {
        const auto time=direct.next_time(); auto timers=direct.step(); expected.emplace(time,std::make_pair(timers,n(direct.store())));
    }
    std::size_t publications=0;
    for(const auto& step:sim.run_until_transactional(1)) for(const auto& emission:step.emissions) {
        const auto& r=std::get<Result>(emission.value); const auto& reference=expected.at(step.time);
        require(r.time==step.time && r.timers==reference.first && n(r.snapshot)==reference.second,"DEVS calendar diverges from standalone async trace");
        ++publications;
    }
    require(publications==expected.size(),"missing or duplicate timer publications");
    auto make=[](std::size_t budget) {
        P p(store(),[](const auto&,const Store& s) { return set(s,n(s)*2); },budget);
        p.schedule({1,{2,0},"double",0});
        return A(std::move(p),[](const Command& c,const Store& s) {
            auto e=set(s,n(s)+3); e.schedules.push_back({c.time,c.agent,"double",0}); return e;
        });
    };
    auto okay=make(2); okay.confluent_transition({{0,0,Command{1,0,{2,0},"input"}}});
    require(n(okay.store())==10 && std::get<Result>(okay.output()[0].value).timers.size()==2,"post-input instant timer not included in transaction");
    auto bounded=make(1); rejects([&] { bounded.confluent_transition({{0,0,Command{1,0,{2,0},"input"}}}); });
    require(n(bounded.store())==1 && bounded.now()==0 && bounded.async().pending_count()==1,"combined event budget failed to restore timestamp");
}
}
int main() { try { ordering(); rollback(); coupled_retry(); calendar_agreement(); std::cout<<"Typed population DEVS confluence, publication and checked rollback passed\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
