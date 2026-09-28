#include "ankurafathom/abm/space.hpp"
#include "ankurafathom/abm/sync_population.hpp"
#include <nlohmann/json.hpp>
#include <bit>
#include <cmath>
#include <iostream>
#include <vector>
using namespace ankurafathom;
using Json=nlohmann::json;

std::vector<std::vector<std::uint64_t>> neighbors(int n,bool wrap) {
    abm::GridSpace grid(n,n,wrap);
    for(int i=0;i<n*n;++i) grid.add(i,{i%n,i/n});
    std::vector<std::vector<std::uint64_t>> result;
    for(int i=0;i<n*n;++i) result.push_back(grid.neighbors(i,1,false));
    return result;
}

int main() { try {
    Json percolation=Json::array(),ising=Json::array();
    for(int n:{3,4}) {
        const auto adjacent=neighbors(n,false);std::vector<unsigned> reachable;
        for(unsigned occupied=0;occupied<(1U<<(n*n));++occupied) {
            abm::SyncPopulation<int> fire;
            for(int i=0;i<n*n;++i) fire.spawn(((occupied>>i)&1U) && i%n==0);
            fire.add_phase([&](std::size_t i,const auto& state) {
                if(!((occupied>>i)&1U)) return 0;
                bool burning=state[i].value;
                for(auto j:adjacent[i]) burning=burning || state[j].value;
                return static_cast<int>(burning);
            });
            unsigned reached=0,previous=~0U;
            for(int t=0;t<=n*n;++t) {
                reached=0;
                for(int i=0;i<n*n;++i) if(fire.records()[i].value) reached|=1U<<i;
                if(reached==previous) break;
                previous=reached;fire.step();
            }
            reachable.push_back(reached);
        }
        percolation.push_back({{"size",n},{"reachable",reachable}});
    }
    constexpr int n=3,sites=n*n;
    const auto adjacent=neighbors(n,true);
    for(double temperature:{1.,2.269185314213022,4.}) {
        Json states=Json::array();
        for(unsigned mask=0;mask<(1U<<sites);++mask) {
            std::vector<int> spin;for(int i=0;i<sites;++i) spin.push_back((mask>>i)&1U?1:-1);
            int energy_twice=0,magnetization=0;
            for(int i=0;i<sites;++i) {magnetization+=spin[i];for(auto j:adjacent[i]) energy_twice-=spin[i]*spin[j];}
            std::vector<double> probabilities;std::vector<unsigned> endpoints;
            for(int selected=0;selected<sites;++selected) {
                int local=0;for(auto j:adjacent[selected]) local+=spin[j];
                const int delta=2*spin[selected]*local;
                const double acceptance=delta<=0?1.:std::exp(-delta/temperature);
                probabilities.push_back(acceptance);
                for(double uniform:{0.25,0.75}) {
                    abm::SyncPopulation<int> population;for(int s:spin) population.spawn(s);
                    // Only the selected site may change: one sequential Metropolis
                    // attempt, not a simultaneous whole-lattice update.
                    population.add_phase([&](std::size_t i,const auto& snapshot) {
                        if(i!=static_cast<std::size_t>(selected)) return snapshot[i].value;
                        int sum=0;for(auto j:adjacent[i]) sum+=snapshot[j].value;
                        const int change=2*snapshot[i].value*sum;
                        return (change<=0 || uniform<std::exp(-change/temperature))?-snapshot[i].value:snapshot[i].value;
                    });
                    population.step();unsigned endpoint=0;
                    for(int i=0;i<sites;++i) if(population.records()[i].value==1) endpoint|=1U<<i;
                    endpoints.push_back(endpoint);
                }
            }
            states.push_back({{"energy",energy_twice/2},{"magnetization",magnetization},
                              {"acceptance",probabilities},{"endpoints",endpoints}});
        }
        ising.push_back({{"size",n},{"temperature",temperature},{"states",states}});
    }
    std::cout<<Json({{"percolation",percolation},{"ising",ising}}).dump()<<'\n';
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
