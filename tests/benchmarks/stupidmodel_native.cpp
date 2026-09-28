// Computational StupidModel workload, pinned to Isaac (JASSS 2011).
// Scripted initial conditions/random inputs dock native populations and grids.
#include "ankurafathom/abm/space.hpp"
#include "ankurafathom/abm/sync_population.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
using namespace ankurafathom;
using Json=nlohmann::json;
struct Bug { double size; };
struct Landscape {int width,height;std::vector<double> production;};
Landscape read_landscape(const std::string& path) {
    std::ifstream file(path);if(!file)throw std::invalid_argument("missing landscape");
    std::string line;for(int i=0;i<3;++i)if(!std::getline(file,line))throw std::invalid_argument("missing headers");
    std::map<std::pair<int,int>,double> values;int width=0,height=0;
    while(std::getline(file,line)) {
        std::istringstream row(line);int x,y;double rate;std::string extra;
        if(!(row>>x>>y>>rate) || row>>extra || x<0 || y<0 || !std::isfinite(rate) || rate<0 || x>=1000 || y>=1000)
            throw std::invalid_argument("invalid landscape row");
        if(!values.emplace(std::pair{x,y},rate).second)throw std::invalid_argument("duplicate landscape cell");
        width=std::max(width,x+1);height=std::max(height,y+1);
    }
    if(values.empty() || values.size()!=static_cast<std::size_t>(width*height))throw std::invalid_argument("incomplete rectangular landscape");
    std::vector<double> rates;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)rates.push_back(values.at({x,y}));
    return {width,height,rates};
}
class World {
public:
    explicit World(const Json& input):spec(input),version(input.at("version")),
        land(version>=15?read_landscape(input.at("landscape")):Landscape{input.at("width"),input.at("height"),{}}),
        cells(land.width,land.height,version<15),bugs_grid(land.width,land.height,version<15),
        hunters_grid(land.width,land.height,version<15),draws(input.at("uniforms").get<std::vector<double>>()) {
        if(version<1 || version>16)throw std::invalid_argument("invalid version");
        const int area=land.width*land.height;
        food=input.value("food",std::vector<double>(area,0.));
        if(food.size()!=static_cast<std::size_t>(area))throw std::invalid_argument("food size");
        for(int i=0;i<area;++i)cells.add(i,{i%land.width,i/land.width});
        for(const auto& initial:input.at("agents")) {
            double size=input.value("initial_size",0.);
            if(version>=14)size=std::max(0.,input.value("initial_mean",.1)+input.value("initial_sd",.03)*initial.at("normal").get<double>());
            if(initial.contains("size"))size=initial.at("size"); // explicit state controls
            const auto id=bugs.spawn({size});const int cell=initial.at("cell");bugs_grid.add(id,cells.position(cell));
        }
        if(version==16)for(int cell:input.at("hunters").get<std::vector<int>>()) {
            const auto id=hunters.spawn(0);hunters_grid.add(id,cells.position(cell));
        }
        check();
    }
    Json run() {
        Json states=Json::array({snapshot()});
        for(int step=0;step<spec.at("steps").get<int>() && !stopped;++step) {advance();states.push_back(snapshot());}
        return {{"id",spec.at("id")},{"version",version},{"states",states},{"draws_used",cursor},
                {"stopped",stopped},{"width",land.width},{"height",land.height}};
    }
private:
    Json spec;int version;Landscape land;abm::GridSpace cells,bugs_grid,hunters_grid;
    abm::SyncPopulation<Bug> bugs;abm::SyncPopulation<int> hunters;
    std::vector<double> food,draws;std::size_t cursor=0;int tick=0;bool stopped=false;
    std::size_t births=0,deaths=0,predation=0,failed_births=0;double produced=0,consumed=0;
    std::vector<std::uint64_t> move_order;
    double uniform(){const double u=draws.at(cursor++);if(!std::isfinite(u) || u<0 || u>=1)throw std::invalid_argument("uniform outside [0,1)");return u;}
    template<class T> void shuffle(std::vector<T>& values){for(auto n=values.size();n>1;--n)std::swap(values[n-1],values[static_cast<std::size_t>(uniform()*n)]);}
    std::vector<std::uint64_t> living()const {std::vector<std::uint64_t> ids;for(const auto& a:bugs.records())if(a.alive)ids.push_back(a.id);return ids;}
    int cell_of(const abm::GridSpace& grid,std::uint64_t id)const{const auto p=grid.position(id);return static_cast<int>(p.y*land.width+p.x);}
    std::vector<std::uint64_t> hood(int cell,int radius,bool center)const{return cells.neighbors(cell,radius,true,center);}
    void retire(std::uint64_t id){bugs_grid.remove(id);bugs.despawn(id);}
    void move(std::uint64_t id) {
        const int old=cell_of(bugs_grid,id);std::vector<std::uint64_t> available;
        for(auto cell:hood(old,4,false))if(!bugs_grid.occupant(cells.position(cell)))available.push_back(cell);
        int chosen=old;
        if(version<11) {
            if(!available.empty())chosen=available.at(static_cast<std::size_t>(uniform()*available.size()));
        } else {
            double best=food[old];for(auto cell:available)best=std::max(best,food[cell]);
            if(best>food[old]) {
                std::erase_if(available,[&](auto cell){return food[cell]!=best;});
                chosen=available.at(static_cast<std::size_t>(uniform()*available.size()));
            }
        }
        bugs_grid.move(id,cells.position(chosen));
    }
    void advance() {
        if(version>=3)for(std::size_t cell=0;cell<food.size();++cell){
            const double delta=version>=15?land.production[cell]:spec.value("max_produce",.01)*uniform();
            food[cell]+=delta;produced+=delta;
        }
        auto ids=living();move_order=ids;
        if(version==9)shuffle(move_order);
        if(version>=10)std::stable_sort(move_order.begin(),move_order.end(),[&](auto a,auto b){return bugs.records()[a].value.size>bugs.records()[b].value.size;});
        for(auto id:move_order)move(id);
        if(version>=2)for(auto id:ids) {
            const int cell=cell_of(bugs_grid,id);
            const double amount=version==2?spec.value("extraction_rate",.1):std::min(spec.value("max_extract",1.),food[cell]);
            if(version>=3)food[cell]-=amount;
            consumed+=amount;auto value=bugs.records()[id].value;value.size+=amount;bugs.replace(id,value);
        }
        if(version>=12) {
            shuffle(ids);
            for(auto id:ids)if(bugs.records()[id].value.size>10.) {
                const int cell=cell_of(bugs_grid,id);
                for(int attempt=0;attempt<5;++attempt) {
                    auto search=hood(cell,3,false);shuffle(search);bool success=false;
                    for(std::size_t j=0;j<std::min(std::size_t{5},search.size());++j)if(!bugs_grid.occupant(cells.position(search[j]))) {
                        const auto child=bugs.spawn({spec.value("initial_size",0.)});bugs_grid.add(child,cells.position(search[j]));++births;success=true;break;
                    }
                    if(!success)++failed_births;
                }
                retire(id);++deaths;
            }
            for(auto id:living())if(uniform()<spec.value("exit_probability",.05)){retire(id);++deaths;}
        }
        if(version==16) {
            std::vector<std::uint64_t> order;for(const auto& h:hunters.records())order.push_back(h.id);shuffle(order);
            for(auto id:order) {
                auto search=hood(cell_of(hunters_grid,id),1,true);shuffle(search);bool done=false;
                for(auto cell:search) {
                    const auto other=hunters_grid.occupant(cells.position(cell));
                    if(other && *other!=id){done=true;break;}
                    const auto prey=bugs_grid.occupant(cells.position(cell));
                    if(prey){retire(*prey);++predation;hunters_grid.move(id,cells.position(cell));done=true;break;}
                }
                if(!done)hunters_grid.move(id,cells.position(search.at(static_cast<std::size_t>(uniform()*search.size()))));
            }
        }
        ++tick;
        if(version>=12)stopped=bugs.active_count()==0 || tick>=1000;
        else if(version>=7)for(auto id:living())stopped=stopped || bugs.records()[id].value.size>=100.;
        check();
    }
    void check()const {
        if(bugs.active_count()!=bugs_grid.size())throw std::runtime_error("population/grid disagreement");
        if(spec.at("agents").size()+births!=bugs.active_count()+deaths+predation)throw std::runtime_error("population balance");
        for(double f:food)if(!std::isfinite(f) || f<0)throw std::runtime_error("invalid food");
        for(auto id:living())if(!std::isfinite(bugs.records()[id].value.size) || bugs.records()[id].value.size<0)throw std::runtime_error("invalid size");
    }
    Json snapshot()const {
        Json records=Json::array(),predators=Json::array();double sum=0,smallest=INFINITY,largest=0;
        std::vector<int> histogram(11);
        for(auto id:living()) {const double size=bugs.records()[id].value.size;
            records.push_back({id,cell_of(bugs_grid,id),size});sum+=size;smallest=std::min(smallest,size);largest=std::max(largest,size);
            ++histogram.at(static_cast<std::size_t>(std::min(10.,std::floor(size))));
        }
        for(const auto& h:hunters.records())predators.push_back({h.id,cell_of(hunters_grid,h.id)});
        const auto count=bugs.active_count();
        return {{"tick",tick},{"agents",records},{"hunters",predators},{"food",food},{"births",births},{"deaths",deaths},
            {"predation",predation},{"failed_births",failed_births},{"produced",produced},{"consumed",consumed},
            {"histogram",histogram},{"minimum",count?smallest:0.},{"mean",count?sum/count:0.},{"maximum",largest},{"move_order",move_order}};
    }
};
int main(int argc,char** argv){try {
    if(argc!=2)throw std::invalid_argument("stupidmodel_native case.json");
    std::ifstream input(argv[1]);Json spec;input>>spec;World world(spec);std::cout<<world.run().dump()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
