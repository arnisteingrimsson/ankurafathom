#include "ankurafathom/abm/models/wealth.hpp"
#include <iostream>
using namespace ankurafathom;
using Wealth=abm::models::WealthExchange;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } require(rejected,"invalid wealth operation accepted"); }
int main() {
    try {
        Wealth w({1,0,0},{},{});
        w.scripted_step({0,1,2},{1,2,0});
        require(w.wealth()==std::vector<std::int64_t>({1,0,0}),"received wealth unavailable in same sweep");
        require(w.sweeps()==1,"sweep clock incorrect");
        const auto before=w.wealth();
        rejects([&] { w.scripted_step({0,1,2},{1,99,0}); });
        require(w.wealth()==before && w.sweeps()==1,"late invalid recipient did not roll back sweep");
        rejects([&] { w.scripted_step({0,0,2},{1,2,0}); });
        rejects([&] { w.scripted_step({0,1},{1,0}); });
        rejects([&] { w.scripted_step({0,1,2},{1}); });
        auto copy=w; copy.scripted_step({2,1,0},{99,99,1});
        require(copy.wealth()==std::vector<std::int64_t>({0,1,0}) && w.wealth()==before,"wealth copy shares state");
        w.scripted_step({0,1,2},{0,99,99});
        require(w.wealth()==before,"self-transfer changed wealth");
        Wealth directed({2,0,1},abm::CsrNetwork({7,8,9},{{7,8}},true),{},7);
        directed.scripted_step({7,8,9},{8,99,99});
        require(directed.wealth()==std::vector<std::int64_t>({1,1,1}),"outgoing or isolated semantics differ");
        rejects([&] { directed.scripted_step({8,7,9},{99,9,99}); });
        require(directed.wealth()==std::vector<std::int64_t>({1,1,1}) && directed.sweeps()==1,"network failure committed");
        rejects([] { Wealth({-1},{},{}); });
        rejects([] { Wealth({std::numeric_limits<std::int64_t>::max(),1},{},{}); });
        rejects([] { Wealth({1},abm::CsrNetwork({1},{}),{}); });
        rejects([] { Wealth({1},{},{0,0,0,7,7}); });
        rejects([] { Wealth({},{},{0,65536,0,7,8}); });
        rejects([] { Wealth({},{},{0,0,65536,7,8}); });
        rejects([] { Wealth({},{},{0,0,0,65536,8}); });
        rejects([] { Wealth({},{},{},std::uint64_t{1}<<48); });
        Wealth maximum({std::numeric_limits<std::int64_t>::max(),0},{},{});
        maximum.scripted_step({0,1},{1,1});
        require(maximum.wealth()==std::vector<std::int64_t>({std::numeric_limits<std::int64_t>::max()-1,1}),"64-bit wealth transfer overflowed");
        Wealth empty({},{},{});
        for(unsigned i=0;i<65536;++i) empty.step();
        rejects([&] { empty.step(); });
        require(empty.sweeps()==65536 && empty.wealth().empty(),"exhausted clock changed state");
        Wealth zero({0,0,0},{},{}),single({4},{},{});
        for(unsigned i=0;i<10;++i) { zero.step(); single.step(); }
        require(zero.wealth()==std::vector<std::int64_t>({0,0,0}) && single.wealth()==std::vector<std::int64_t>({4}),"degenerate stochastic transfer changed wealth");
        Wealth random({1,2,3,4},{},{7319,9,4,71,72}); auto replay=random;
        for(unsigned i=0;i<20;++i) {
            (void)random.wealth(); random.step(); replay.step();
            require(random.wealth()==replay.wealth(),"observation or copy perturbed draws");
        }
        // Independent Python Philox/transfer recurrence, including high entity/key bits.
        Wealth addressed({1,2,3,4},{},{0xFEDCBA9876543210ULL,65535,65535,65535,65534},4294967298ULL);
        for(const auto& expected:std::vector<std::vector<std::int64_t>>{{2,2,2,4},{3,3,1,3},{2,3,2,3},{1,5,1,3}}) {
            addressed.step(); require(addressed.wealth()==expected,"high-address wealth recurrence differs");
        }
        std::cout<<"wealth sequential histories, exact conservation, copy/replay, bounds and failed-sweep rollback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
