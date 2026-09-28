#include "ankurafathom/abm/space.hpp"
#include <iostream>
#include <random>

namespace {
using namespace ankurafathom::abm;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool caught=false; try { function(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid spatial operation succeeded");
}
void grid_contract() {
    for(bool wrap:{false,true}) for(std::int64_t width:{1,2,5}) for(std::int64_t height:{1,3,4}) {
        GridSpace grid(width,height,wrap);
        std::uint64_t id=0;
        for(std::int64_t x=0;x<width;++x) for(std::int64_t y=0;y<height;++y) grid.add(id++,{x,y});
        for(const auto& [agent,point]:grid.positions()) for(std::uint64_t radius:{0,1,2,8}) for(bool moore:{false,true}) {
            std::vector<std::uint64_t> expected;
            for(const auto& [other,position]:grid.positions()) {
                if(agent==other) continue;
                auto dx=std::abs(point.x-position.x),dy=std::abs(point.y-position.y);
                if(wrap) { dx=std::min(dx,width-dx); dy=std::min(dy,height-dy); }
                const auto distance=moore ? std::max(dx,dy) : dx+dy;
                if(static_cast<std::uint64_t>(distance)<=radius) expected.push_back(other);
            }
            require(grid.neighbors(agent,radius,moore)==expected,"bucketed grid differs from exhaustive distance oracle");
        }
    }
    GridSpace grid(3,2,true);
    grid.add(10,{-1,0}); grid.add(11,{0,0});
    require(grid.position(10)==GridPoint{2,0},"wrapped placement differs");
    auto snapshot=grid;
    grid.move_many({{10,{0,0}},{11,{2,0}}});
    require(grid.position(10)==GridPoint{0,0} && snapshot.position(10)==GridPoint{2,0},"swap or independent snapshot failed");
    rejects([&] { grid.move_many({{10,{1,1}},{11,{1,1}}}); });
    rejects([&] { grid.move_many({{10,{1,1}},{10,{2,1}}}); });
    rejects([&] { grid.move(10,{2,0}); });
    rejects([&] { grid.add(10,{1,1}); });
    rejects([&] { grid.add(12,{2,0}); });
    require(grid.position(10)==GridPoint{0,0} && grid.size()==2,"failed grid mutation changed state");
    grid.remove(11); grid.move(10,{2,0});
    require(grid.occupant({2,0})==10 && !grid.occupant({0,0}),"grid indexes disagree after removal");
    GridSpace bounded(2,2,false);
    rejects([&] { bounded.add(0,{-1,0}); });
    rejects([&] { GridSpace bad(0,1,false); });
}
void continuous_contract() {
    std::mt19937_64 random(20260925);
    for(std::size_t dimensions:{1,2,3}) for(bool wrap:{false,true}) for(double bin_width:{.7,3.,20.}) {
        ContinuousSpace space(std::vector<double>(dimensions,-2),std::vector<double>(dimensions,8),bin_width,wrap);
        for(std::uint64_t id=0;id<64;++id) {
            std::vector<double> point;
            for(std::size_t d=0;d<dimensions;++d) point.push_back(-2+static_cast<double>(random()%10000)/1000);
            space.add(id,point);
        }
        for(const auto& [id,point]:space.positions()) for(double radius:{0.,.1,.75,2.,5.,20.}) {
            std::vector<std::uint64_t> expected;
            for(const auto& [other,position]:space.positions()) {
                if(id==other) continue;
                double squared=0;
                for(std::size_t d=0;d<dimensions;++d) {
                    double delta=std::abs(point[d]-position[d]);
                    if(wrap) delta=std::min(delta,10-delta);
                    squared+=delta*delta;
                }
                if(std::sqrt(squared)<=radius) expected.push_back(other);
            }
            require(space.neighbors(id,radius)==expected,"continuous bins differ from exhaustive distance oracle");
        }
    }
    ContinuousSpace periodic({0,0},{10,10},3,true);
    periodic.add(0,{9.5,5}); periodic.add(1,{.25,5}); periodic.add(2,{9.5,5});
    require(periodic.neighbors(0,1)==std::vector<std::uint64_t>({1,2}),"short periodic boundary bin lost neighbor");
    require(periodic.neighbors(0,0)==std::vector<std::uint64_t>({2}),"self exclusion removed colocated peer");
    auto snapshot=periodic;
    periodic.move_many({{0,{10.5,-1}},{1,{11,11}}});
    require(periodic.position(0)==std::vector<double>({.5,9}) && snapshot.position(0)[0]==9.5,"periodic move/snapshot failed");
    rejects([&] { periodic.move_many({{0,{2,2}},{1,{std::numeric_limits<double>::quiet_NaN(),0}}}); });
    require(periodic.position(0)==std::vector<double>({.5,9}),"failed continuous batch partially moved");
    rejects([&] { periodic.move_many({{0,{2,2}},{0,{3,3}}}); });
    rejects([&] { periodic.add(0,{1,1}); });
    rejects([&] { (void)periodic.neighbors(0,-1); });
    periodic.remove(2);
    require(periodic.size()==2,"continuous removal count differs");
    ContinuousSpace bounded({0,0},{10,10},1,false);
    rejects([&] { bounded.add(0,{10,1}); });
    rejects([&] { bounded.add(0,{1}); });
    rejects([&] { ContinuousSpace bad({0},{0},1,false); });
    rejects([&] { ContinuousSpace bad({0},{1},0,false); });
    ContinuousSpace tiny({0},{1e-300},1e300,false);
    tiny.add(0,{0}); tiny.add(1,{5e-301});
    require(tiny.neighbors(0,1e-300)==std::vector<std::uint64_t>{1},"underflowing bin quotient lost the single bin");
}
}
int main() {
    try { grid_contract(); continuous_contract(); std::cout<<"Grid/continuous neighborhood oracles, periodic bounds and atomic movement passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
