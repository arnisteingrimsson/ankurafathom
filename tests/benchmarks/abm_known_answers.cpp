#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/abm/space.hpp"
#include "ankurafathom/rng/philox.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <map>
using namespace ankurafathom;
using Json=nlohmann::json;
int main(int argc,char** argv){try{
    if(argc!=2)throw std::invalid_argument("abm_known_answers plan.json");
    std::ifstream file(argv[1]);Json plan;file>>plan;Json result=Json::array();
    for(const auto& test:plan.at("life")){
        const int width=test.at("width"),height=test.at("height"),steps=test.at("steps");
        abm::GridSpace grid(width,height,test.at("wrap"));abm::SyncPopulation<int> cells;
        const auto initial=test.at("initial").get<std::vector<int>>();
        for(int i=0;i<width*height;++i){cells.spawn(initial.at(i));grid.add(i,{i%width,i/width});}
        cells.add_phase([&](std::size_t i,const auto& state){int count=0;
            for(auto neighbor:grid.neighbors(i,1,true))count+=state.at(neighbor).value;
            return static_cast<int>(count==3 || (state.at(i).value && count==2));});
        Json trajectory=Json::array();
        for(int t=0;t<=steps;++t){std::vector<int> values;for(const auto& cell:cells.records())values.push_back(cell.value);trajectory.push_back(values);if(t<steps)cells.step();}
        result.push_back({{"id",test.at("id")},{"states",trajectory}});
    }
    Json walks=Json::array();
    const int size=plan.at("walkers"),steps=plan.at("walk_steps"),reps=plan.at("replications");
    for(int rep=0;rep<reps;++rep){abm::SyncPopulation<int> population;for(int i=0;i<size;++i)population.spawn(0);
        std::uint32_t tick=0;population.add_phase([&](std::size_t i,const auto& state){
            const auto word=rng::draw(2026092801ULL,{1,static_cast<std::uint32_t>(rep),i,tick,701,0})[0];
            return state[i].value+((word&1U)?1:-1);});
        for(int t=1;t<=steps;++t){tick=t-1;population.step();if(t==1 || t==8 || t==32 || t==128){
            std::map<int,int> histogram;double sum=0,squared=0;for(const auto& a:population.records()){++histogram[a.value];sum+=a.value;squared+=a.value*a.value;}
            Json counts=Json::array();for(const auto& [position,count]:histogram)counts.push_back({position,count});
            walks.push_back({{"replication",rep},{"time",t},{"mean",sum/size},{"msd",squared/size},{"histogram",counts}});
        }}
    }
    std::cout<<Json({{"life",result},{"walk",walks}}).dump()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
