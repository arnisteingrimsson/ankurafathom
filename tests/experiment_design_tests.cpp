#include "ankurafathom/runtime/design.hpp"
#include <iostream>

namespace rt=ankurafathom::runtime;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }require(caught,"invalid design accepted"); }
void same(const std::vector<rt::Scenario>& a,const std::vector<rt::Scenario>& b) {
    require(a.size()==b.size(),"design size mismatch");for(std::size_t i=0;i<a.size();++i) require(a[i].id==b[i].id && a[i].parameters==b[i].parameters,"canonical design order changed");
}
int main() {
    try {
        const auto grid=rt::expand_grid({{"z",{10,20,30}},{"a",{1,2}}},100);
        require(grid.size()==6 && grid[0].id==100 && grid[5].id==105,"grid addresses");
        for(std::size_t i=0;i<6;++i) require(grid[i].parameters.at("a")==1+i/3 && grid[i].parameters.at("z")==10+10*(i%3),"grid mixed radix order");
        same(grid,rt::expand_grid({{"a",{1,2}},{"z",{10,20,30}}},100));
        const std::array<std::array<double,2>,8> known{{{0,0},{.5,.5},{.75,.25},{.25,.75},{.375,.375},{.875,.875},{.625,.125},{.125,.625}}};
        const auto sobol=rt::expand_sobol({{"b",0,1},{"a",0,1}},8);
        for(std::size_t i=0;i<8;++i) require(sobol[i].parameters.at("a")==known[i][0] && sobol[i].parameters.at("b")==known[i][1],"Sobol hand points");
        const auto shifted=rt::expand_sobol({{"a",0,1},{"b",0,1}},8,17);
        for(std::size_t i=0;i<8;++i) require(shifted[i].id==17+i && shifted[i].parameters==sobol[i].parameters,"scenario ID shifted sample coordinates");
        for(const std::size_t n:{1,7,64,1000,65536}) {
            const auto lhs=rt::expand_lhs({{"b",0,1},{"a",0,1}},n,0);
            same(lhs,rt::expand_lhs({{"a",0,1},{"b",0,1}},n,0));
            for(const std::string name:{"a","b"}) {
                std::vector<bool> strata(n,false);
                for(const auto& scenario:lhs) {
                    const auto u=scenario.parameters.at(name);const auto stratum=static_cast<std::size_t>(u*n);
                    require(u>0 && u<1 && stratum<n && !strata[stratum],"LHS stratum repeated or endpoint emitted");strata[stratum]=true;
                }
                require(std::all_of(strata.begin(),strata.end(),[](bool present) { return present; }),"LHS stratum missing");
            }
        }
        const auto last=rt::expand_sobol({{"x",0,1}},1,65535);require(last[0].id==65535,"last design scenario ID");
        const auto max=std::numeric_limits<double>::max();
        for(const auto& point:rt::expand_lhs({{"x",-max,max}},8,7)) require(std::isfinite(point.parameters.at("x")),"finite extreme bounds overflow");
        rejects([] { rt::expand_grid({}); });rejects([] { rt::expand_grid({{"a",{}}}); });
        rejects([] { rt::expand_grid({{"a",{NAN}}}); });rejects([] { rt::expand_grid({{"a",{1}},{"a",{2}}}); });
        rejects([] { rt::expand_grid({{"a",std::vector<double>(257,1)},{"b",std::vector<double>(257,1)}}); });
        rejects([] { rt::expand_lhs({{"a",0,1}},0,0); });rejects([] { rt::expand_lhs({{"a",1,1}},8,0); });
        rejects([] { rt::expand_lhs({{"a",0,INFINITY}},8,0); });rejects([] { rt::expand_sobol({{"a",0,1}},7); });
        rejects([] { rt::expand_sobol({{"a",0,1}},2,65535); });rejects([] { rt::expand_sobol({{"a",0,1}},1,65536); });
        rejects([] { rt::detail::design_word(0,0,32,0,0); });rejects([] { rt::detail::design_word(0,0,0,65536,0); });
        std::cout<<"canonical grid, LHS stratification, Sobol points, bounds and address guards passed\n";
    }catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
