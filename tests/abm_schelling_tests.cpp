#include "ankurafathom/abm/models/schelling.hpp"
#include <iostream>
#include <limits>
using namespace ankurafathom;
using Model=abm::models::Schelling;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } require(rejected,"invalid Schelling operation accepted"); }
int main() {
    try {
        const Model::Parameters line{4,1,false,false,1,1};
        Model sequential(line,{{0,{0,0}},{1,{1,0}}},{});
        require(!sequential.satisfied(0) && !sequential.satisfied(1),"initial opposite neighbors satisfied");
        sequential.scripted_step({0,1},{{3,0},{0,0}});
        require(sequential.states()==std::vector<Model::State>{{0,{3,0}},{1,{1,0}}} && sequential.last_moves()==1,"later satisfaction did not see earlier move");
        sequential.scripted_step({1,0},{{99,99},{99,99}});
        require(sequential.last_moves()==0 && sequential.sweeps()==2,"happy agent consumed destination");
        Model reuse(line,{{0,{0,0}},{1,{1,0}},{1,{2,0}}},{});
        reuse.scripted_step({0,2,1},{{3,0},{0,0},{99,99}});
        require(reuse.states()==std::vector<Model::State>{{0,{3,0}},{1,{1,0}},{1,{0,0}}} && reuse.last_moves()==2,"vacated cell unavailable within sweep");
        Model failed(line,{{0,{0,0}},{1,{1,0}},{0,{2,0}}},{});
        const auto before=failed.states();
        rejects([&] { failed.scripted_step({0,1,2},{{3,0},{2,0},{0,0}}); });
        require(failed.states()==before && failed.sweeps()==0 && failed.last_moves()==0,"late invalid destination committed partial sweep");
        rejects([&] { failed.scripted_step({0,1,2},{{0,0},{0,0},{0,0}}); });
        rejects([&] { failed.scripted_step({0,0,2},{{3,0},{0,0},{0,0}}); });
        rejects([&] { failed.scripted_step({0,1},{{3,0},{0,0}}); });
        rejects([&] { failed.scripted_step({0,1,2},{{3,0}}); });
        failed.scripted_step({0,1,2},{{3,0},{0,0},{99,99}});
        require(failed.last_moves()==2 && failed.sweeps()==1,"valid retry changed schedule");
        auto copied=failed; copied.step();
        require(failed.sweeps()==1 && copied.sweeps()==2,"copy shares sweep state");
        // Inclusive rational boundary and neighborhood selection.
        Model half({3,1,false,false,1,2},{{0,{0,0}},{0,{1,0}},{1,{2,0}}},{});
        require(half.satisfied(1),"exact half boundary excluded");
        Model higher({3,1,false,false,2,3},{{0,{0,0}},{0,{1,0}},{1,{2,0}}},{});
        require(!higher.satisfied(1),"two-thirds threshold rounded");
        Model moore({3,3,false,true,1,1},{{0,{0,0}},{1,{1,1}}},{});
        Model von({3,3,false,false,1,1},{{0,{0,0}},{1,{1,1}}},{});
        require(!moore.satisfied(0) && von.satisfied(0),"neighborhood kind ignored");
        Model wrap({4,1,true,false,1,1},{{0,{0,0}},{1,{3,0}}},{});
        Model bounded({4,1,false,false,1,1},{{0,{0,0}},{1,{3,0}}},{});
        require(!wrap.satisfied(0) && bounded.satisfied(0),"periodic seam ignored");
        Model tiny({2,1,true,true,1,2},{{0,{0,0}},{1,{1,0}}},{});
        require(!tiny.satisfied(0),"small periodic grid counted self");
        tiny.scripted_step({0,1},{{99,99},{99,99}});
        require(tiny.last_moves()==0 && !tiny.satisfied(0),"full grid relocated unhappy agent");
        Model zero({3,1,false,true,0,1},{{0,{0,0}},{1,{1,0}}},{});
        zero.scripted_step({0,1},{{99,99},{99,99}});
        require(zero.last_moves()==0,"zero threshold moved agents");
        Model singleton({1,1,true,true,1,1},{{0,{0,0}}},{});
        singleton.step(); require(singleton.satisfied(0) && singleton.last_moves()==0,"singleton changed");
        rejects([] { Model({0,1,false,true,1,2},{},{}); });
        rejects([] { Model({1000000,2,false,true,1,2},{},{}); });
        rejects([] { Model({1,1,false,true,1,0},{},{}); });
        rejects([] { Model({1,1,false,true,2,1},{},{}); });
        rejects([] { Model({1,1,false,true,1,1000001},{},{}); });
        rejects([] { Model({},{{2,{0,0}}},{}); });
        rejects([] { Model({},{{0,{0,0}},{1,{0,0}}},{}); });
        rejects([] { Model({},{{0,{-1,0}}},{}); });
        rejects([] { Model({},{{0,{6,0}}},{}); });
        rejects([] { Model({1,1,false,true,1,2},{{0,{0,0}},{1,{0,0}}},{}); });
        rejects([] { Model({},{},{0,0,0,7,7}); });
        rejects([] { Model({},{},{0,65536,0,7,8}); });
        rejects([] { Model({},{},{0,0,65536,7,8}); });
        rejects([] { Model({},{},{0,0,0,7,65536}); });
        rejects([] { Model({},{},{},std::uint64_t{1}<<48); });
        Model empty({1,1,false,true,1,2},{},{});
        for(unsigned i=0;i<65536;++i) empty.step();
        rejects([&] { empty.step(); });
        require(empty.sweeps()==65536 && empty.states().empty(),"exhausted clock changed state");
        Model random({4,2,true,true,2,3},{{0,{0,0}},{1,{1,0}},{0,{2,1}},{1,{3,1}}},{7319,9,4,71,72},7);
        auto replay=random;
        for(unsigned i=0;i<20;++i) {
            (void)random.states(); (void)random.satisfied(7); random.step(); replay.step();
            require(random.states()==replay.states() && random.last_moves()==replay.last_moves(),"observation/copy perturbed relocation");
        }
        // Independent Python geometry/Philox recurrence exercises upper address bits.
        Model addressed({4,2,true,true,2,3},{{0,{0,0}},{1,{1,0}},{0,{2,1}},{1,{3,1}}},
                        {0xFEDCBA9876543210ULL,65535,65535,65535,65534},4294967298ULL);
        const std::vector<std::vector<Model::State>> expected{
            {{0,{2,0}},{1,{1,1}},{0,{3,0}},{1,{0,0}}},
            {{0,{2,0}},{1,{0,1}},{0,{1,1}},{1,{1,0}}},
            {{0,{3,1}},{1,{2,0}},{0,{3,0}},{1,{0,1}}},
            {{0,{1,0}},{1,{3,1}},{0,{1,1}},{1,{3,0}}}};
        const std::vector<std::size_t> expected_moves{4,3,4,4};
        for(std::size_t i=0;i<expected.size();++i) {
            addressed.step();
            require(addressed.states()==expected[i] && addressed.last_moves()==expected_moves[i],"high-address Schelling recurrence differs");
        }
        std::cout<<"Schelling sequential/vacancy histories, occupancy, thresholds, boundaries, replay and rollback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
