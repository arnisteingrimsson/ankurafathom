#include "ankurafathom/abm/spatial_snapshot.hpp"
#include "ankurafathom/abm/population_atomic.hpp"
#include <iostream>
using namespace ankurafathom;
struct Agent {};
using Sync=abm::TypedPopulation<Agent>;
using Async=abm::AsyncPopulation<Agent>;
using Store=Sync::Store;
using Snapshot=abm::SpatialSnapshot<Agent>;
void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
template<class F> void rejects(F f) { bool b=false; try { f(); } catch(const std::exception&) { b=true; } require(b,"invalid operation accepted"); }
Store initial() {
    Store s(7,{{"x",des::FieldKind::integer},{"y",des::FieldKind::integer}},4);
    s.spawn_many({{std::int64_t(0),std::int64_t(0)},{std::int64_t(1),std::int64_t(0)}}); return s;
}
int main() {
    try {
        const Snapshot::Binding binding=Snapshot::Grid{0,1,2,1,true};
        const auto validate=[binding](const Store& s) { (void)Snapshot(s,binding); };
        const auto swap=[](Store::Reference r,const Store& s) { auto v=s.record(r); v[0]=std::int64_t(1)-std::get<std::int64_t>(v[0]); return v; };
        auto s=initial(); Snapshot frozen(s,binding);
        require(frozen.neighbors({7,4},1)==std::vector<std::uint64_t>{5},"grid binding lost nonzero IDs");
        rejects([&] { frozen.neighbors({8,4},1); }); rejects([&] { frozen.neighbors({7,4},.5); });
        Sync p(s,validate); p.add_phase(swap); p.step();
        require(std::get<std::int64_t>(p.store().field({7,4},0))==1,"simultaneous swap failed");
        p.update({7,4},{std::int64_t(3),std::int64_t(0)});
        require(std::get<std::int64_t>(p.store().field({7,4},0))==3,"periodic binding changed raw coordinate");
        rejects([&] { p.spawn({std::int64_t(0),std::int64_t(0)}); });
        require(p.store().next_id()==6,"failed spawn consumed ID");
        rejects([&] { p.update({7,4},{std::int64_t(2),std::int64_t(0)}); });
        require(std::get<std::int64_t>(p.store().field({7,4},0))==3,"collision committed");
        Sync bad(s,validate); bad.add_phase(swap);
        bad.add_phase([](auto r,const Store& v) { auto a=v.record(r); a[0]=std::int64_t(0); return a; });
        bad.add_phase(swap); rejects([&] { bad.step(); });
        require(bad.store().record({7,4})==s.record({7,4}),"invalid intermediate phase committed");
        auto retired=s; retired.retire({7,5}); Snapshot one(retired,binding);
        require(one.neighbors({7,4},10).empty() && frozen.neighbors({7,4},10).size()==1,"snapshot shares source liveness");
        rejects([&] { one.neighbors({7,5},1); });
        rejects([&] { Snapshot(s,Snapshot::Grid{0,0,2,1,true}); });
        bool fail=true;
        Async a(s,[&](const Async::Timer& t,const Store& v) {
            Async::Effects e;
            if(t.kind=="swap") {
                for(auto id:{4u,5u}) e.updates.push_back({{7,id},swap({7,id},v)});
                e.schedules.push_back({t.time,{7,5},"later"});
            } else if(fail) e.updates.push_back({{7,5},v.record({7,4})});
            return e;
        },100,validate);
        a.schedule({2,{7,4},"swap"}); rejects([&] { a.step(); });
        require(a.now()==0 && a.pending_count()==1 && a.next_time()==2 && a.store().record({7,4})==s.record({7,4}),"validator failed timestamp rollback");
        fail=false; auto trace=a.step(); require(trace.size()==2 && trace[1].id==1,"retry consumed timer identity");
        using Atomic=abm::PopulationAtomic<Agent>;
        Atomic atom(Sync(s,validate),1,[swap](const auto&,const Store& v) {
            Async::Effects e; for(auto id:{4u,5u}) e.updates.push_back({{7,id},swap({7,id},v)}); return e;
        });
        atom.external_transition_at(.5,.5,{{0,0,Atomic::Command{.5,0,{7,4},"swap"}}});
        require(std::get<std::int64_t>(atom.store().field({7,4},0))==1,"DEVS input batch validated partial swap");
        Store c(3,{{"x",des::FieldKind::real},{"y",des::FieldKind::real}});
        c.spawn_many({{-.25,0.},{1.75,0.},{.25,0.}});
        Snapshot cs(c,Snapshot::Continuous{{0,1},{0,0},{2,2},.3,true});
        require(cs.neighbors({3,0},0)==std::vector<std::uint64_t>{1},"colocated periodic peer excluded");
        require(cs.neighbors({3,0},.5)==std::vector<std::uint64_t>({1,2}),"exact periodic boundary lost");
        rejects([&] { Snapshot(c,Snapshot::Continuous{{0,1},{0,0},{2,2},.3,false}); });
        rejects([&] { Snapshot(c,Snapshot::Grid{0,1,2,2,false}); });
        rejects([&] { cs.neighbors({3,0},std::numeric_limits<double>::infinity()); });
        Store huge(0,{{"x",des::FieldKind::integer}}); huge.spawn({std::int64_t(9007199254740992LL)});
        rejects([&] { Snapshot(huge,Snapshot::Continuous{{0},{0},{1e17},1e16,false}); });
        std::cout<<"spatial population snapshots, batch validation and rollback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
