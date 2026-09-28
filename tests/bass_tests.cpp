#include "ankurafathom/abm/models/bass.hpp"
#include <iostream>
using Model=ankurafathom::abm::models::Bass;
void require(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught=false; try { f(); } catch(const std::exception&) { caught=true; } require(caught,"invalid Bass accepted"); }
int main() {
    try {
        Model m({.1,.8,1},{1,0,0,0},{});
        const auto p=m.adoption_probability();
        require(std::abs(p-.3)<1e-15,"Bass normalization");
        m.scripted_step({.9,std::nextafter(p,0.),p,.4});
        require(m.states()==std::vector<std::int64_t>({1,1,0,0}),"strict boundary or Jacobi snapshot failed");
        require(m.ticks()==1 && m.time()==1,"Bass clock");
        auto copy=m;
        rejects([&] { m.scripted_step({0,0,0,1}); });
        require(m.states()==copy.states() && m.time()==copy.time(),"Bass rollback");
        rejects([&] { m.scripted_step({0}); });
        m.scripted_step({0,0,0,0});copy.scripted_step({0,0,0,0});
        require(m.states()==copy.states() && m.adopted()==4,"Bass retry/copy");
        m.step();require(m.adopted()==4,"adoption absorption");
        Model zero({0,.7,.25},{0,0,0},{});zero.scripted_step({0,0,0});require(zero.adopted()==0,"imitation spontaneously seeded");
        Model certain({1,0,1},{0,1,0},{});certain.scripted_step({.999,.9,0});require(certain.adopted()==3,"probability-one adoption");
        Model empty({}, {},{});empty.step();require(empty.adopted()==0 && empty.ticks()==1,"empty Bass");
        const auto nan=std::numeric_limits<double>::quiet_NaN();
        rejects([&] { m.scripted_step({0,0,nan,0}); });
        rejects([] { Model({.5,.6,1},{0},{}); });
        rejects([] { Model({-1,0,1},{0},{}); });
        rejects([] { Model({0,0,0},{0},{}); });
        rejects([&] { Model({nan,0,1},{0},{}); });
        rejects([] { Model({}, {2},{}); });
        rejects([] { Model({}, {},{0,65536,0,0}); });
        rejects([] { Model({}, {0,0},{},(std::uint64_t{1}<<48)-1); });
        // Independent direct recurrence, including high seed/entity/stream bits.
        for(std::uint32_t rep=0;rep<16;++rep) {
            const std::uint64_t first=4294967298ULL,seed=0xFEDCBA9876543210ULL;
            Model actual({.08,.6,.25},{1,0,0,0,0},{seed,65535,rep,65535},first);
            std::vector<std::int64_t> expected{1,0,0,0,0};
            for(std::uint32_t tick=0;tick<20;++tick) {
                double sum=0;for(auto x:expected)sum+=x;
                const auto probability=.25*(.08+.6*(sum/5));
                auto next=expected;
                for(std::size_t i=0;i<expected.size();++i) if(!expected[i])
                    next[i]=ankurafathom::rng::bernoulli(probability,ankurafathom::rng::draw(seed,{65535,rep,first+i,tick,65535,0})[0]);
                expected=next;actual.step();require(actual.states()==expected,"Bass addressed recurrence");
                // Golden states from the independent Python Philox recurrence.
                if(rep==0 && tick==3) require(actual.states()==std::vector<std::int64_t>({1,1,1,0,0}),"high-address four-tick golden");
                if(rep==0 && tick==7) require(actual.states()==std::vector<std::int64_t>({1,1,1,1,0}),"high-address eight-tick golden");
                if(rep==0 && tick==15) require(actual.states()==std::vector<std::int64_t>({1,1,1,1,1}),"high-address sixteen-tick golden");
            }
        }
        Model exhausted({}, {},{});
        for(int i=0;i<65536;++i) exhausted.step();
        rejects([&] { exhausted.step(); });require(exhausted.ticks()==65536,"address exhaustion rollback");
        Model clock_overflow({0,0,std::numeric_limits<double>::max()}, {},{});
        clock_overflow.step();rejects([&] { clock_overflow.step(); });
        require(clock_overflow.ticks()==1 && clock_overflow.time()==std::numeric_limits<double>::max(),"clock overflow rollback");
        std::cout<<"Bass snapshots, boundaries, invariants, rollback, 320 addressed ticks and exhaustion passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
