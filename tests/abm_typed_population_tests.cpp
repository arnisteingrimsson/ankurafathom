#include "ankurafathom/abm/typed_population.hpp"
#include "ankurafathom/abm/async_population.hpp"
#include <iostream>
#include <random>

namespace {
using namespace ankurafathom;
struct Person {};
using Sync=abm::TypedPopulation<Person>;
using Async=abm::AsyncPopulation<Person>;
using Store=Sync::Store;
using Ref=Store::Reference;
void require(bool b,const char* message) { if(!b) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } require(caught,"invalid operation succeeded"); }
Store fresh() { return Store(7,{{"n",des::FieldKind::integer},{"label",des::FieldKind::string},{"enabled",des::FieldKind::boolean},{"weight",des::FieldKind::real}}); }
Store::Record record(std::int64_t n) { return {n,std::string("person"),true,1.0}; }
std::int64_t value(const Store& s,Ref r) { return std::get<std::int64_t>(s.field(r,"n")); }
void synchronous() {
    Sync p(fresh()); auto a=p.spawn(record(1)),b=p.spawn(record(2)),c=p.spawn(record(3));
    p.add_phase([=](Ref r,const Store& s) { auto v=s.record(r); v[0]=value(s,{7,(r.id+1)%3}); return v; });
    p.add_phase([](Ref r,const Store& s) { auto v=s.record(r); v[0]=2*value(s,r); return v; });
    p.step(); require(value(p.store(),a)==4 && value(p.store(),b)==6 && value(p.store(),c)==2,"typed Jacobi phases differ from hand rotation");
    auto copy=p; copy.retire(b); const auto d=copy.spawn(record(9));
    require(d.id==3 && copy.store().active_count()==3 && p.store().alive(b),"typed liveness/identity/copy differs");
    p.add_phase([&](Ref r,const Store& s) { if(r==b) p.retire(a); return s.record(r); });
    rejects([&] { p.step(); });
    require(value(p.store(),a)==4 && value(p.store(),b)==6 && p.store().alive(a),"failed phase partly committed");
    Sync invalid(fresh()); invalid.spawn(record(5));
    invalid.add_phase([](Ref r,const Store& s) { auto v=s.record(r); v[0]=5.0; return v; });
    rejects([&] { invalid.step(); }); require(value(invalid.store(),{7,0})==5,"wrong-type update committed");
    rejects([&] { invalid.spawn({std::int64_t(1)}); });
    require(invalid.store().next_id()==1,"failed typed spawn consumed an ID");
}
Async::Effects increment(const Async::Timer& timer,const Store& s) {
    Async::Effects e; auto v=s.record(timer.agent); v[0]=value(s,timer.agent)+1;
    e.updates.push_back({timer.agent,v}); return e;
}
void order_and_lifecycle() {
    Async p(fresh(),[](const Async::Timer& timer,const Store& s) {
        Async::Effects e; auto v=s.record(timer.agent);
        std::int64_t total=0;
        for(std::uint64_t i=s.first_id();i<s.next_id();++i) if(s.alive({s.store_id(),i})) total+=value(s,{s.store_id(),i});
        v[0]=total; e.updates.push_back({timer.agent,v}); return e;
    });
    const auto a=p.spawn(record(1)),b=p.spawn(record(2));
    p.schedule({1,b,"work",0}); p.schedule({1,a,"work",0}); p.schedule({1,a,"work",0});
    const auto trace=p.step();
    require(trace.size()==3 && trace[0].agent==a && trace[1].agent==a && trace[2].agent==b,"timer tie order differs");
    require(value(p.store(),a)==5 && value(p.store(),b)==7 && p.now()==1,"async did not use latest staged records");
    const auto cancelled=p.schedule({2,a,"cancel",0}); p.cancel(cancelled);
    rejects([&] { p.cancel(cancelled); });
    p.schedule({3,b,"retire",0}); p.retire(b);
    require(p.pending_count()==0 && !p.store().alive(b),"retirement left live timers");
    auto copy=p; require(copy.spawn(record(4)).id==2 && p.store().next_id()==2,"async copies share IDs");
    p.run_until(4); require(p.now()==4 && std::isinf(p.next_time()),"idle horizon did not advance");
    rejects([&] { p.schedule({3,a,"past",0}); });
    rejects([&] { p.schedule({4,b,"dead",0}); });
    rejects([&] { p.schedule({4,{99,a.id},"foreign",0}); });
    rejects([&] { p.schedule({4,a,"",0}); });
    rejects([&] { p.run_until(std::numeric_limits<double>::quiet_NaN()); });
}
void timestamp_rollback() {
    bool fail=true;
    Async p(fresh(),[&](const Async::Timer& timer,const Store& s) {
        if(timer.kind=="fail" && fail) throw std::runtime_error("injected later-event failure");
        auto e=increment(timer,s);
        if(timer.kind=="birth") e.births.push_back({record(10),{{timer.time,"child",0}}});
        return e;
    });
    auto a=p.spawn(record(0));
    p.schedule({0.25,a,"ok",0}); p.schedule({1,a,"birth",0}); p.schedule({1,a,"fail",0});
    rejects([&] { p.run_until(2); });
    require(p.now()==0.25 && p.next_time()==1 && p.pending_count()==2 && p.store().next_id()==1 && value(p.store(),a)==1,
            "timestamp rollback lost earlier commit or consumed new IDs");
    auto baseline=p; fail=false;
    auto left=p.step(),right=baseline.step();
    require(left==right && left.size()==3 && left.back().agent.id==1 && left.back().id==3,"retry timer/agent IDs differ");
    require(value(p.store(),a)==3 && value(p.store(),{7,1})==11,"birth timer effects differ");
    const auto before=p.pending_count();
    Async::Effects bad; bad.updates.push_back({a,record(99)}); bad.retirements.push_back(a);
    rejects([&] { p.apply(bad); }); require(value(p.store(),a)==3 && p.pending_count()==before,"conflicting effect committed");
    bad={}; bad.births.push_back({record(8),{{0,"past",0}}});
    rejects([&] { p.apply(bad); }); require(p.store().next_id()==2,"failed newborn timer consumed identity");
}
void boundaries_and_cycles() {
    Async p(fresh(),increment); auto a=p.spawn(record(0));
    p.schedule({1,a,"one",0}); p.schedule({std::nextafter(1.,2.),a,"after",0});
    p.run_until(1); require(value(p.store(),a)==1 && p.pending_count()==1,"nearby timer pulled into earlier horizon");
    p.run_until(std::nextafter(1.,2.)); require(value(p.store(),a)==2,"adjacent timer lost");
    Async cycle(fresh(),[](const Async::Timer& t,const Store& s) {
        auto e=increment(t,s); e.schedules.push_back({t.time,t.agent,"again",0}); return e;
    },5);
    auto c=cycle.spawn(record(0)); auto id=cycle.schedule({0,c,"again",0});
    rejects([&] { cycle.step(); });
    require(cycle.now()==0 && cycle.pending_count()==1 && value(cycle.store(),c)==0,"zero-time cycle did not roll back");
    cycle.cancel(id); require(cycle.schedule({1,c,"again",0})==1,"cycle rollback consumed timer IDs");
    Async* active=nullptr;
    Async recursive(fresh(),[&](const Async::Timer& t,const Store& s) { active->run_until(t.time); return increment(t,s); }); active=&recursive;
    auto r=recursive.spawn(record(0)); recursive.schedule({1,r,"work",0});
    rejects([&] { recursive.step(); }); require(recursive.now()==0 && recursive.pending_count()==1,"recursive dispatch changed state");
}
void independent_calendar() {
    // Independent sorted-vector event calendar, deliberately unrelated to heap code.
    Async p(fresh(),increment);
    for(int i=0;i<9;++i) p.spawn(record(0));
    std::vector<Async::Timer> expected;
    std::mt19937 random(81723);
    for(int i=0;i<300;++i) {
        const double t=double(random()%31)/4;
        const Ref a{7,random()%9}; const auto id=p.schedule({t,a,"tick",0});
        if(i%7==0) p.cancel(id); else expected.push_back({t,a,id,"tick",0});
    }
    std::sort(expected.begin(),expected.end(),[](const auto& a,const auto& b) { return std::tie(a.time,a.agent.id,a.id)<std::tie(b.time,b.agent.id,b.id); });
    std::vector<Async::Timer> actual;
    while(std::isfinite(p.next_time())) { auto batch=p.step(); actual.insert(actual.end(),batch.begin(),batch.end()); }
    require(actual==expected,"heap differs from independent sorted calendar");
    for(std::uint64_t i=0;i<9;++i) {
        const auto count=std::count_if(expected.begin(),expected.end(),[&](const auto& t) { return t.agent.id==i; });
        require(value(p.store(),{7,i})==count,"event conservation differs");
    }
}
}
int main() {
    try { synchronous(); order_and_lifecycle(); timestamp_rollback(); boundaries_and_cycles(); independent_calendar();
        std::cout<<"Typed Jacobi, async sorted-calendar agreement, lifecycle, exact clock and timestamp rollback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
