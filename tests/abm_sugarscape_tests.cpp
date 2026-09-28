#include "ankurafathom/abm/models/sugarscape.hpp"
#include <iostream>
using namespace ankurafathom;
using Model=abm::models::Sugarscape;
void require(bool v,const char* msg) { if(!v) throw std::runtime_error(msg); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } require(rejected,"invalid Sugarscape accepted"); }
void same(const Model& a,const Model& b) {
    require(a.sugar()==b.sugar() && a.ledger()==b.ledger() && a.sweeps()==b.sweeps() && a.active_ids()==b.active_ids(),"Sugarscape snapshot changed");
    for(auto id:a.active_ids()) require(a.state(id)==b.state(id),"Sugarscape agent changed");
}
int main() {
    try {
        // Axial vision excludes rich diagonal; richest beats nearer; then distance breaks equal sugar.
        Model axial({3,3,false,0},{0,0,0,0,0,4,0,0,9},{0,0,0,0,0,4,0,0,9},{{3,1,3,{1,1}}},{});
        axial.scripted_step({0},{{2,1}});
        require(axial.state(0)==Model::State{6,1,3,{2,1}} && axial.sugar()[8]==9,"axial harvest differs");
        Model distance({4,1,false,0},{0,4,0,4},{0,4,0,4},{{1,1,3,{0,0}}},{});
        rejects([&] { distance.scripted_step({0},{{3,0}}); });
        distance.scripted_step({0},{{1,0}});
        require(distance.state(0).reserve==4,"nearest tie/harvest/metabolism order");
        Model stay({2,1,false,0},{3,3},{3,3},{{1,1,1,{0,0}}},{});
        stay.scripted_step({0},{{0,0}}); require(stay.state(0).reserve==3,"own cell excluded");
        Model zero({2,1,false,0},{0,9},{0,9},{{2,1,0,{0,0}}},{});
        zero.scripted_step({0},{{0,0}}); require(zero.state(0).reserve==1 && zero.sugar()[1]==9,"zero vision moved");
        // A prior harvest changes the next choice, and vacated cells can be used immediately.
        Model sequential({3,1,false,0},{0,5,1},{0,5,1},{{3,1,2,{0,0}},{3,1,2,{2,0}}},{});
        sequential.scripted_step({0,1},{{1,0},{2,0}});
        require(sequential.state(0).reserve==7 && sequential.state(1).reserve==3,"sequential occupancy/harvest ignored");
        Model vacancy({3,1,false,0},{3,0,4},{3,0,4},{{2,1,2,{0,0}},{2,1,2,{1,0}}},{});
        vacancy.scripted_step({0,1},{{2,0},{0,0}});
        require(vacancy.state(1).reserve==4,"vacated rich cell unavailable");
        // Zero after metabolism retires; deficit consumes only available reserve, never creates negative sugar.
        Model death({2,1,false,1},{0,0},{0,0},{{1,2,1,{0,0}},{2,1,1,{1,0}}},{},17);
        death.scripted_step({17,18},{{0,0},{1,0}});
        require(death.active_ids()==std::vector<std::uint64_t>{18} && death.ledger()==Model::Ledger{3,0,2,1},"death budget or stable ID differs");
        rejects([&] { death.scripted_step({17,18},{{0,0},{1,0}}); });
        death.step(); require(death.active_ids().empty() && death.ledger().consumed==3 && death.ledger().deaths==2,"exact-zero starvation differs");
        // A starving mover releases its destination before the next agent chooses.
        Model released({3,1,false,0},{0,2,0},{0,2,0},{{1,4,1,{0,0}},{2,1,1,{2,0}}},{});
        released.scripted_step({0,1},{{1,0},{2,0}});
        require(!released.store().alive({0,0}) && released.store().active_count()==1,"starvation did not retire");
        // Regrowth is after every activation, including on occupied cells, capped per cell.
        Model growth({2,1,false,3},{2,5},{0,4},{{1,1,0,{0,0}}},{});
        growth.scripted_step({0},{{0,0}});
        require(growth.active_ids().empty() && growth.sugar()==std::vector<std::int64_t>{2,5} && growth.ledger()==Model::Ledger{5,3,1,1},"regrowth timing/cap/death budget");
        growth.step(); require(growth.ledger().regrown==3,"full land regrew beyond cap");
        Model torus({4,1,true,0},{0,0,0,5},{0,0,0,5},{{2,1,1,{0,0}}},{});
        torus.scripted_step({0},{{3,0}});
        Model bounded({4,1,false,0},{0,0,0,5},{0,0,0,5},{{2,1,1,{0,0}}},{});
        rejects([&] { bounded.scripted_step({0},{{3,0}}); });
        bounded.scripted_step({0},{{0,0}});
        Model tiny({2,1,true,0},{0,3},{0,3},{{2,1,1000000,{0,0}}},{});
        tiny.step(); require(tiny.state(0).reserve==4 && tiny.state(0).position==abm::GridPoint{1,0},"periodic vision duplicated harvest");
        // Bad second action restores the first action, death, landscape, ledger, and RNG clock.
        Model failed({3,1,false,1},{0,2,0},{0,2,0},{{1,4,1,{0,0}},{2,1,1,{2,0}}},{});
        const auto before=failed;
        rejects([&] { failed.scripted_step({0,1},{{1,0},{1,0}}); }); same(failed,before);
        rejects([&] { failed.scripted_step({0,0},{{1,0},{1,0}}); }); same(failed,before);
        rejects([&] { failed.scripted_step({0},{{1,0}}); });
        rejects([&] { failed.scripted_step({0,1},{{1,0}}); });
        auto retry=before; failed.step(); retry.step(); same(failed,retry);
        require(before.sweeps()==0 && before.ledger().deaths==0,"copy shares committed state");
        // A late cumulative-resource overflow rolls back an otherwise valid harvest/metabolism.
        constexpr auto maximum=std::numeric_limits<std::int64_t>::max();
        Model overflow({1,1,false,1},{maximum},{maximum-1},{{1,1,0,{0,0}}},{});
        auto saved=overflow;
        rejects([&] { overflow.step(); }); same(overflow,saved);
        rejects([&] { Model({1,1,false,0},{maximum},{maximum},{{1,1,0,{0,0}}},{}); });
        rejects([] { Model({0,1,false,0},{},{},{},{}); });
        rejects([] { Model({1000000,2,false,0},{},{},{},{}); });
        rejects([] { Model({1,1,false,-1},{0},{0},{},{}); });
        rejects([] { Model({1,1,false,0},{},{0},{},{}); });
        rejects([] { Model({1,1,false,0},{0},{-1},{},{}); });
        rejects([] { Model({1,1,false,0},{-1},{0},{},{}); });
        rejects([] { Model({1,1,false,0},{0},{1},{},{}); });
        for(auto a:std::vector<Model::State>{{0,1,1,{0,0}},{1,0,1,{0,0}},{1,1,-1,{0,0}},{1,1,1000001,{0,0}},{1,1,1,{-1,0}},{1,1,1,{1,0}}})
            rejects([&] { Model({1,1,true,0},{0},{0},{a},{}); });
        rejects([] { Model({2,1,true,0},{0,0},{0,0},{{1,1,1,{0,0}},{1,1,1,{0,0}}},{}); });
        rejects([] { Model({1,1,false,0},{0},{0},{},{0,0,0,7,7}); });
        rejects([] { Model({1,1,false,0},{0},{0},{},{0,65536,0,7,8}); });
        rejects([] { Model({1,1,false,0},{0},{0},{},{0,0,65536,7,8}); });
        rejects([] { Model({1,1,false,0},{0},{0},{},{0,0,0,65536,8}); });
        rejects([] { Model({1,1,false,0},{0},{0},{},{},std::uint64_t{1}<<48); });
        Model empty({1,1,false,0},{0},{0},{},{});
        for(unsigned i=0;i<65536;++i) empty.step();
        rejects([&] { empty.step(); }); require(empty.sweeps()==65536,"exhausted clock advanced");
        Model random({4,2,true,1},{0,3,0,3,3,0,3,0},{0,3,0,3,3,0,3,0},
                     {{3,1,2,{0,0}},{3,2,2,{2,0}},{3,1,2,{1,1}},{3,2,2,{3,1}}},
                     {0xFEDCBA9876543210ULL,65535,65535,65535,65534},4294967298ULL);
        auto golden=random;
        // Independent Python ray geometry/Philox recurrence, upper entity/key bits.
        for(std::int64_t tick=1;tick<=4;++tick) {
            golden.step();
            require(golden.state(4294967298ULL)==Model::State{5,1,2,{3,0}}
                    && golden.state(4294967299ULL)==Model::State{5-tick,2,2,{2,1}}
                    && golden.state(4294967300ULL)==Model::State{5,1,2,{1,0}}
                    && golden.state(4294967301ULL)==Model::State{5-tick,2,2,{0,1}},"high-address Sugarscape recurrence differs");
            require(golden.sugar()==std::vector<std::int64_t>{0,1,0,1,1,0,1,0}
                    && golden.ledger()==Model::Ledger{24,4*tick,6*tick,0},"high-address landscape/ledger differs");
        }
        auto replay=random;
        for(unsigned i=0;i<20;++i) { (void)random.active_ids(); (void)random.ledger(); random.step(); replay.step(); same(random,replay); }
        std::cout<<"Sugarscape movement, harvest, starvation, regrowth, conservation, rollback and address contracts passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
