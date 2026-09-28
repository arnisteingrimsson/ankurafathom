#include "ankurafathom/abm/statechart.hpp"
#include <iostream>

namespace {
using namespace ankurafathom;
struct Person {};
using Pop=abm::AsyncPopulation<Person>;
using Chart=abm::Statechart<Person>;
using Store=Pop::Store;
using Ref=Pop::Reference;
void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } require(caught,"invalid chart operation accepted"); }
Store fresh() { return Store(12,{{"state",des::FieldKind::integer},{"entered",des::FieldKind::real},
    {"generation",des::FieldKind::integer},{"counter",des::FieldKind::integer},{"ready",des::FieldKind::boolean}}); }
Store::Record newborn() { return {std::int64_t(-1),0.0,std::int64_t(-1),std::int64_t(0),true}; }
std::int64_t integer(const Store& s,Ref r,const std::string& field) { return std::get<std::int64_t>(s.field(r,field)); }
Chart::Transition timeout(std::string name,std::int64_t from,std::int64_t to,double delay,int priority=0) {
    Chart::Transition t; t.name=std::move(name); t.source=from; t.target=to; t.trigger=Chart::Trigger::timeout; t.value=delay; t.priority=priority; return t;
}
Chart::Transition message(std::string name,std::int64_t from,std::int64_t to,std::string event,int priority=0) {
    auto t=timeout(std::move(name),from,to,0,priority); t.trigger=Chart::Trigger::message; t.message=std::move(event); return t;
}
Chart::Transition rate(std::string name,std::int64_t from,std::int64_t to,double value,std::uint32_t stream) {
    auto t=timeout(std::move(name),from,to,value); t.trigger=Chart::Trigger::rate; t.stream=stream; return t;
}
void hand_trace() {
    auto s=fresh();
    auto gated=message("a-guard",0,2,"start",-1);
    gated.guard=[](Ref,const Store&) { return false; };
    auto chosen=message("b-go",0,1,"start");
    chosen.action=[](Ref r,const Store& store) { auto v=store.record(r); v[3]=integer(store,r,"counter")+1; return v; };
    const Chart chart(s,{"state","entered","generation"},{0,1,2},
        {timeout("old",0,2,5),message("z-go",0,2,"start"),chosen,gated,timeout("finish",1,2,2)});
    Pop p(s,[chart](const Pop::Timer& t,const Store& store) {
        if(t.kind=="input") return chart.message(t.agent,store,"start",t.time);
        return chart.on_timer(t,store);
    });
    const auto a=p.spawn(newborn()); p.apply(chart.start(a,p.store(),0,0)); p.schedule({1,a,"input",0});
    p.run_until(1);
    require(integer(p.store(),a,"state")==1 && integer(p.store(),a,"counter")==1 && integer(p.store(),a,"generation")==1,
            "message priority, guard or action trace differs");
    p.run_until(3); require(integer(p.store(),a,"state")==2 && integer(p.store(),a,"generation")==2,"timeout failed to enter terminal state");
    p.run_until(5); require(integer(p.store(),a,"generation")==2 && p.pending_count()==0,"stale timer re-fired after exit");
    rejects([&] { (void)chart.start(a,p.store(),0,5); });
    rejects([&] { (void)chart.message(a,p.store(),"start",2); });
}
void simultaneous_and_guard() {
    auto s=fresh(); const auto a=s.spawn(newborn());
    auto first=timeout("a",0,1,1),last=timeout("z",0,2,1);
    for(bool reverse:{false,true}) {
        Chart chart(s,{"state","entered","generation"},{0,1,2},reverse ? std::vector{last,first} : std::vector{first,last});
        Pop p(s,[chart](const auto& t,const auto& store) { return chart.on_timer(t,store); });
        p.apply(chart.start(a,p.store(),0,0)); p.run_until(1);
        require(integer(p.store(),a,"state")==1,"timed transition tie depends on declaration order");
    }
    first.guard=[](Ref,const Store&) { return false; };
    Chart chart(s,{"state","entered","generation"},{0,1},{first});
    Pop p(s,[chart](const auto& t,const auto& store) { return chart.on_timer(t,store); });
    p.apply(chart.start(a,p.store(),0,0)); p.run_until(9);
    require(integer(p.store(),a,"state")==0 && p.pending_count()==0,"false timed guard resampled");
}
void exact_rate_paths() {
    auto s=fresh(); const auto a=s.spawn(newborn());
    Chart chart(s,{"state","entered","generation"},{0},{rate("jump",0,0,0.7,71),rate("disabled",0,0,0,72)});
    for(std::uint64_t seed=0;seed<12;++seed) {
        const Chart::Draws draws{seed,3,11};
        Pop p(s,[chart,draws](const auto& t,const auto& store) { return chart.on_timer(t,store,draws); });
        p.apply(chart.start(a,p.store(),0,0,draws));
        double expected=0;
        for(std::uint32_t generation=0;generation<24;++generation) {
            const auto word=rng::draw(seed,{3,11,a.id,generation,71,0})[0];
            expected+=-std::log((double(word)+0.5)/4294967296.0)/0.7;
            require(p.next_time()==expected && p.pending_count()==1,"rate timer differs from addressed inverse-CDF oracle");
            const auto trace=p.step();
            require(trace.size()==1 && p.now()==expected && integer(p.store(),a,"generation")==generation+1,
                    "rate self-transition failed to re-arm with next generation");
        }
    }
}
void failures() {
    auto s=fresh(); const auto a=s.spawn(newborn());
    bool fail=true;
    auto t=timeout("loop",0,0,1);
    t.action=[&](Ref r,const Store& store) { auto v=store.record(r); v[3]=std::string("wrong"); if(!fail) v[3]=std::int64_t(1); return v; };
    Chart chart(s,{"state","entered","generation"},{0},{t});
    Pop p(s,[chart](const auto& timer,const auto& store) { return chart.on_timer(timer,store); });
    p.apply(chart.start(a,p.store(),0,0)); rejects([&] { p.step(); });
    require(p.now()==0 && integer(p.store(),a,"generation")==0 && p.next_time()==1,"failed action changed chart/timer/clock");
    fail=false; p.step(); require(p.now()==1 && integer(p.store(),a,"counter")==1 && p.next_time()==2,"chart retry differs");
    auto v=p.store().record(a); v[2]=std::int64_t(65535); Pop::Effects e; e.updates.push_back({a,v}); p.apply(e);
    auto reenter=message("again",0,0,"again"); Chart limited(s,{"state","entered","generation"},{0},{reenter});
    rejects([&] { (void)limited.message(a,p.store(),"again",1); });
    rejects([&] { Chart bad(s,{"state","state","generation"},{0},{t}); });
    rejects([&] { Chart bad(s,{"entered","state","generation"},{0},{t}); });
    rejects([&] { Chart bad(s,{"state","entered","generation"},{0},{t,t}); });
    rejects([&] { Chart bad(s,{"state","entered","generation"},{0},{timeout("bad",0,1,1)}); });
    rejects([&] { Chart bad(s,{"state","entered","generation"},{0},{timeout("bad",0,0,-1)}); });
    rejects([&] { Chart bad(s,{"state","entered","generation"},{0},{rate("a",0,0,1,2),rate("b",0,0,2,2)}); });
    Chart tiny(s,{"state","entered","generation"},{0},{timeout("tiny",0,0,1e-300)});
    rejects([&] { (void)tiny.start(a,s,0,1); });
    Chart zero(s,{"state","entered","generation"},{0},{timeout("zero",0,0,0)});
    Pop z(s,[zero](const auto& timer,const auto& store) { return zero.on_timer(timer,store); },4);
    z.apply(zero.start(a,z.store(),0,0)); rejects([&] { z.step(); });
    require(integer(z.store(),a,"generation")==0 && z.pending_count()==1,"instant statechart cycle escaped timestamp rollback");
}
}
int main() {
    try { hand_trace(); simultaneous_and_guard(); exact_rate_paths(); failures();
        std::cout<<"Statechart hand traces, guards, priorities, 288 exact rate firings and transactional failures passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
