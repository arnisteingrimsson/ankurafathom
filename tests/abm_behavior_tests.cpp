#include "ankurafathom/abm/population_atomic.hpp"
#include "ankurafathom/abm/statechart.hpp"
#include "ankurafathom/abm/spatial_snapshot.hpp"
#include <iostream>
namespace {
using namespace ankurafathom;
struct Person {};
using S=abm::TypedPopulation<Person>; using P=abm::AsyncPopulation<Person>; using C=abm::Statechart<Person>; using A=abm::PopulationAtomic<Person>; using Store=S::Store;
void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
template<class F> void rejects(F f) { bool failed=false; try { f(); } catch(const std::exception&) { failed=true; } require(failed,"invalid behavior accepted"); }
std::int64_t integer(const Store& s,Store::Reference r,std::size_t f) { return std::get<std::int64_t>(s.field(r,f)); }
Store spatial() { Store s(7,{{"x",des::FieldKind::integer},{"y",des::FieldKind::integer},{"n",des::FieldKind::integer}}); s.spawn({std::int64_t(0),std::int64_t(0),std::int64_t(1)}); s.spawn({std::int64_t(1),std::int64_t(0),std::int64_t(3)}); return s; }
void validate_space(const Store& s) { (void)abm::SpatialSnapshot<Person>(s,abm::SpatialSnapshot<Person>::Grid{0,1,4,1,false}); }
void phase_snapshots() {
    S p(spatial(),validate_space);
    p.add_phase([](auto,const Store&)->Store::Record { throw std::runtime_error("retired action evaluated"); },
        [](auto,const Store&)->std::vector<S::Emission> { throw std::runtime_error("retired publisher evaluated"); },
        [](auto r,const Store& s) { auto v=s.record(r); v[2]=integer(s,{7,0},2)+integer(s,{7,1},2); return S::Lifecycle{true,{v}}; });
    p.add_phase([](auto r,const Store& s) { auto v=s.record(r); v[2]=integer(s,r,2)+10; return v; });
    auto copy=p; p.step();
    require(p.store().next_id()==4 && p.store().active_count()==2 && !p.store().alive({7,0}) && integer(p.store(),{7,2},2)==14 && integer(p.store(),{7,3},2)==14,"phase snapshot or newborn next-phase participation differs");
    require(copy.store().next_id()==2 && copy.store().alive({7,0}),"copy shares behavior state");
}
void phase_rollback() {
    bool fail=true; S p(spatial(),validate_space);
    p.add_phase([](auto r,const Store& s) { return s.record(r); },{},[](auto r,const Store& s) { return S::Lifecycle{true,{s.record(r)}}; });
    p.add_phase([&](auto r,const Store& s) { if(fail) throw std::runtime_error("later phase failed"); return s.record(r); });
    rejects([&] { p.step(); }); require(p.store().next_id()==2 && p.store().alive({7,0}),"later failure lost retired agents/IDs");
    fail=false; p.step(); require(p.store().next_id()==4 && p.store().alive({7,2}),"retry IDs differ");
    S collision(spatial(),validate_space);
    collision.add_phase([](auto r,const Store& s) { return s.record(r); },{},[](auto r,const Store& s) { auto v=s.record(r); v[0]=std::int64_t(0); return S::Lifecycle{true,{v}}; });
    rejects([&] { collision.step(); }); require(collision.store().next_id()==2,"collision leaked births");
    S queued(spatial());
    queued.add_phase([](auto r,const Store& s) { return s.record(r); },[](auto r,const Store&) { return std::vector<S::Emission>{{"topic",r,1}}; });
    queued.add_phase([](auto r,const Store& s) { return s.record(r); },{},[](auto,const Store&) { return S::Lifecycle{true,{}}; });
    rejects([&] { queued.step(); }); require(queued.store().active_count()==2 && queued.pending_publications().empty(),"queued endpoint failure not atomic");
}
Store chart_store() { Store s(7,{{"state",des::FieldKind::integer},{"entered",des::FieldKind::real},{"generation",des::FieldKind::integer},{"n",des::FieldKind::integer}}); s.spawn({std::int64_t(-1),0.,std::int64_t(-1),std::int64_t(0)}); return s; }
C::Transition replacement(double duration=1) {
    C::Transition t; t.name="replace"; t.source=0; t.target=0; t.trigger=C::Trigger::timeout; t.value=duration;
    t.action=[](auto,const Store&)->Store::Record { throw std::runtime_error("retired action evaluated"); };
    t.publish=[](auto,const Store&)->std::vector<C::Emission> { throw std::runtime_error("retired publisher evaluated"); };
    t.lifecycle=[](auto r,const Store& s) { auto v=s.record(r); v[2]=std::int64_t(-1); v[3]=integer(s,r,3)+1; return S::Lifecycle{true,{v}}; };
    return t;
}
P population(const C& chart,std::size_t budget=20) { P p(chart_store(),[chart](const auto& t,const Store& s) { return chart.on_timer(t,s); },budget); p.apply(chart.start({7,0},p.store(),0,0)); return p; }
void charts() {
    C chart(chart_store(),{"state","entered","generation"},{0},{replacement()},"chart/",0);
    auto p=population(chart); p.schedule({3,{7,0},"chart/replace",0});
    auto first=p.step(); require(first.size()==1 && p.store().next_id()==2 && !p.store().alive({7,0}) && p.pending_count()==1 && p.next_time()==2,"replacement did not cancel parent timers");
    require(integer(p.store(),{7,1},2)==0 && std::get<double>(p.store().field({7,1},1))==1 && integer(p.store(),{7,1},3)==1,"newborn initialization differs");
    auto second=p.step(); require(second[0].agent.id==1 && integer(p.store(),{7,2},3)==2,"newborn timer missing");
    C missing(chart_store(),{"state","entered","generation"},{0},{replacement()}); auto invalid=population(missing);
    rejects([&] { invalid.step(); }); require(invalid.now()==0 && invalid.store().next_id()==1 && invalid.pending_count()==1,"missing initializer failure not atomic");
    auto bad=replacement(); bad.lifecycle=[](auto r,const Store& s) { return S::Lifecycle{true,{s.record(r)}}; };
    auto wrong_epoch=population(C(chart_store(),{"state","entered","generation"},{0},{bad},"chart/",0));
    rejects([&] { wrong_epoch.step(); }); require(wrong_epoch.store().next_id()==1,"invalid epoch allocated IDs");
    auto disabled=replacement(); disabled.guard=[](auto,const Store&) { return false; }; disabled.lifecycle=[](auto,const Store&)->S::Lifecycle { throw std::runtime_error("disabled lifecycle evaluated"); };
    auto guarded=population(C(chart_store(),{"state","entered","generation"},{0},{disabled},"chart/",0)); guarded.step(); require(guarded.store().next_id()==1,"false guard birthed");
    C instant(chart_store(),{"state","entered","generation"},{0},{replacement(0)},"chart/",0); auto bounded=population(instant,3);
    rejects([&] { bounded.step(); }); require(bounded.store().next_id()==1 && bounded.store().alive({7,0}) && bounded.pending_count()==1,"immediate reproduction budget did not restore timestamp");
}
}
int main() { try { phase_snapshots(); phase_rollback(); charts(); std::cout<<"behavior lifecycle invariants passed\n"; } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
