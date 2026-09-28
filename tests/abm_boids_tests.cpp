#include "ankurafathom/abm/models/boids.hpp"
#include <iostream>
using Model=ankurafathom::abm::models::Boids;
void require(bool v,const char* m) { if(!v) throw std::runtime_error(m); }
void near(double a,double b) { require(std::abs(a-b)<2e-12,"Boids hand value differs"); }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } require(rejected,"invalid Boids operation accepted"); }
int main() {
    try {
        Model::Parameters p; p.dt=.5; p.alignment=p.cohesion=p.separation=0;
        Model inertial(p,{{9.5,1,2,0}}); inertial.step();
        near(inertial.states()[0][0],.5); near(inertial.states()[0][2],2);
        p.width=p.height=2; p.wrap=false; p.dt=3;
        Model walls(p,{{1,.5,1,-.5}}); walls.step();
        near(walls.states()[0][0],0); near(walls.states()[0][1],1);
        near(walls.states()[0][2],1); near(walls.states()[0][3],.5);
        p.dt=10; Model many(p,{{1,1,1,0}}); many.step();
        near(many.states()[0][0],1); near(many.states()[0][2],-1);
        p.dt=1; Model exact(p,{{1,1,1,0},{1,0,-1,0}}); exact.step();
        require(exact.states()[0][0]==std::nextafter(2.,0.) && exact.states()[0][2]==-1,"upper-wall representation/orientation");
        require(exact.states()[1][0]==0 && exact.states()[1][2]==1,"lower-wall orientation");
        // Synchronous alignment: both velocities come from the old snapshot.
        p=Model::Parameters{}; p.dt=.5; p.cohesion=p.separation=0; p.max_acceleration=10;
        Model alignment(p,{{3,3,1,0},{4,3,0,0}}); alignment.step();
        near(alignment.states()[0][2],.5); near(alignment.states()[1][2],.5);
        near(alignment.states()[0][0],3.25); near(alignment.states()[1][0],4.25);
        p.alignment=0; p.cohesion=1;
        Model seam(p,{{.25,3,0,0},{9.75,3,0,0}}); seam.step();
        near(seam.states()[0][2],-.25); near(seam.states()[1][2],.25);
        p.vision=5; Model antipodal(p,{{1,3,0,0},{6,3,0,0}}); antipodal.step();
        near(antipodal.states()[0][2],2); near(antipodal.states()[1][2],-2);
        // Inclusive radii, regularized separation and colocated peers.
        p.cohesion=0; p.separation=1; p.vision=p.separation_radius=1; p.softening=1;
        Model separate(p,{{3,3,0,0},{4,3,0,0}}); separate.step();
        near(separate.states()[0][2],-.25); near(separate.states()[1][2],.25);
        p.alignment=1; p.vision=p.separation_radius=0;
        Model colocated(p,{{3,3,1,0},{3,3,0,0}}); colocated.step();
        near(colocated.states()[0][2],.5); near(colocated.states()[1][2],.5);
        p.vision=3; p.separation=0; p.alignment=100; p.max_acceleration=.5;
        Model capped(p,{{3,3,0,0},{4,3,1,0}}); capped.step(); near(capped.states()[0][2],.25);
        p.max_acceleration=0; Model stationary(p,{{3,3,0,0},{4,3,1,0}}); stationary.step(); near(stationary.states()[0][2],0);
        p=Model::Parameters{};
        Model empty(p,{}); empty.step(); require(empty.states().empty() && empty.ticks()==1,"empty Boids clock");
        for(auto invalid:{-1.,0.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            auto bad=p; bad.dt=invalid; rejects([&] { Model(bad,{}); });
            bad=p; bad.softening=invalid; rejects([&] { Model(bad,{}); });
        }
        auto bad=p; bad.separation_radius=4; rejects([&] { Model(bad,{}); });
        bad=p; bad.alignment=-1; rejects([&] { Model(bad,{}); });
        bad=p; bad.bin_width=1e-20; rejects([&] { Model(bad,{}); });
        rejects([&] { Model(p,{{10,1,0,0}}); });
        rejects([&] { Model(p,{{-1,1,0,0}}); });
        rejects([&] { Model(p,{{1,1,3,0}}); });
        rejects([&] { Model(p,{{1,1,std::numeric_limits<double>::quiet_NaN(),0}}); });
        rejects([&] { Model::seeded(p,0,{0,0,0,7,7}); });
        rejects([&] { Model::seeded(p,0,{0,65536,0,7,8}); });
        rejects([&] { Model::seeded(p,0,{0,0,65536,7,8}); });
        rejects([&] { Model::seeded(p,0,{0,0,0,7,65536}); });
        rejects([&] { Model::seeded(p,2,{},Model::Store::max_entity_id); });
        // A later agent overflows after an earlier rule produced a valid update.
        p.dt=10; p.max_speed=1e308; p.alignment=p.cohesion=p.separation=0;
        Model failure(p,{{1,1,0,0},{8,7,1e308,0}});
        const auto initial=failure.states(); rejects([&] { failure.step(); });
        require(failure.states()==initial && failure.ticks()==0,"failed Boids tick partially committed");
        rejects([&] { failure.step(); }); require(failure.states()==initial,"failed retry changed state");
        p=Model::Parameters{};
        auto random=Model::seeded(p,12,{0xFEDCBA9876543210ULL,65535,65535,65535,65534},4294967298ULL);
        // Independent Python Philox values, including upper seed/entity bits.
        require(random.states()[0]==Model::State{7.951671545160934,4.67956858035177,.523783931741491,.884030717657879},"high-address Boids initializer differs");
        require(random.states()[1]==Model::State{9.50231704278849,5.801081684418023,-.6755026711616665,.996746135642752},"second high-address Boids initializer differs");
        auto replay=random;
        for(unsigned i=0;i<8;++i) { (void)random.states(); random.step(); replay.step(); require(random.states()==replay.states(),"Boids observation/copy changed trajectory"); }
        auto other_bins=p; other_bins.bin_width=.3;
        Model a(p,random.states()),b(other_bins,random.states()); a.step(); b.step();
        require(a.states()==b.states(),"spatial bin size changed Boids semantics");
        auto reversed=random.states(); std::reverse(reversed.begin(),reversed.end());
        Model c(p,reversed); c.step(); auto restored=c.states(); std::reverse(restored.begin(),restored.end());
        for(std::size_t i=0;i<restored.size();++i) for(unsigned j=0;j<4;++j) near(restored[i][j],a.states()[i][j]);
        std::cout<<"Boids hand steering, synchronous phases, boundary/cap invariants, replay and rollback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
