// C17R workload adapter. Hex topology/collision/intervention rules live here;
// production SyncPopulation supplies frozen-phase execution and SD supplies RK4.
#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/sd/model.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <vector>
using namespace ankurafathom;
using Json = nlohmann::json;
using Cell = std::array<int,6>; // empty=-1, susceptible=0, infected=1, recovered=2
constexpr std::array<int,6> dx{1,0,-1,-1,0,1}, dy{0,1,1,0,-1,-1};
double uniform(std::uint32_t word) { return (static_cast<double>(word)+.5)/4294967296.; }
void require(bool ok) { if(!ok) throw std::invalid_argument("invalid C17 workload"); }
Cell collide(const Cell& before,double u) {
    unsigned mask=0; for(unsigned d=0;d<6;++d) if(before[d]>=0) mask|=1U<<d;
    int shift=0;
    if(mask==9 || mask==18 || mask==36) shift=u<.5?1:5;
    else if(mask==21 || mask==42) shift=1;
    Cell after{}; after.fill(-1);
    for(int d=0;d<6;++d) after[(d+shift)%6]=before[d];
    return after;
}
Cell disease(const Cell& before,double alpha,double beta,const std::uint32_t* words) {
    const int infected=static_cast<int>(std::count(before.begin(),before.end(),1));
    const double probability=infected==0?0.:alpha==1.?1.:-std::expm1(infected*std::log1p(-alpha));
    Cell after=before;
    for(int d=0;d<6;++d) {
        if(before[d]==0 && uniform(words[d])<probability) after[d]=1;
        else if(before[d]==1 && uniform(words[d])<beta) after[d]=2;
    }
    return after;
}
Json local(const Json& input) {
    Json result=Json::array();
    for(const auto& spec:input.at("cases")) {
        const Cell initial=spec.at("state").get<Cell>();
        const auto words=spec.at("words").get<std::array<std::uint32_t,6>>();
        abm::SyncPopulation<Cell> population;population.spawn(initial);
        population.add_phase([&](std::size_t i,const auto& snapshot) {
            return spec.at("collision").get<bool>()?collide(snapshot[i].value,spec.at("u")):snapshot[i].value;
        });
        population.add_phase([&](std::size_t i,const auto& snapshot) {
            return disease(snapshot[i].value,spec.at("alpha"),spec.at("beta"),words.data());
        });
        population.step();result.push_back(population.records()[0].value);
    }
    return result;
}
Json ca(const Json& spec) {
    const int n=spec.at("n"),steps=spec.at("steps");require(n>0 && n<=100 && steps>=0 && steps<=1000);
    const int cells=n*n;
    auto initial=spec.at("initial").get<std::vector<Cell>>();require(initial.size()==static_cast<std::size_t>(cells));
    int total=0;for(const auto& c:initial) for(int s:c) {require(s>=-1 && s<=2);total+=s>=0;}
    const double alpha=spec.at("alpha"),beta=spec.at("beta");require(alpha>=0 && alpha<=1 && beta>=0 && beta<=1);
    const std::string movement=spec.at("movement");require(movement=="fhp" || movement=="diffusion");
    const auto words=spec.at("words").get<std::vector<std::uint32_t>>();
    require(words.size()==static_cast<std::size_t>(steps*cells*7));
    std::vector<std::array<int,6>> permutations;
    if(movement=="diffusion") {
        permutations=spec.at("permutations").get<std::vector<std::array<int,6>>>();
        require(permutations.size()==static_cast<std::size_t>(steps*cells));
        for(auto p:permutations) {std::sort(p.begin(),p.end());require(p==std::array<int,6>{0,1,2,3,4,5});}
    }
    const Json intervention=spec.at("intervention");const std::string kind=intervention.at("kind");
    require(kind=="none" || kind=="hard" || kind=="soft");
    std::vector<int> priority;
    if(kind=="hard") {
        priority=intervention.at("priority").get<std::vector<int>>();auto ordered=priority;
        std::sort(ordered.begin(),ordered.end());require(ordered.size()==static_cast<std::size_t>(cells*6));
        for(int i=0;i<cells*6;++i) require(ordered[i]==i);
        require(intervention.at("target")==0 || intervention.at("target")==1);
        const double f=intervention.at("fraction");require(f>=0 && f<=1);
    }
    if(kind!="none") {const double f=intervention.at("threshold");require(f>=0 && f<=1);}
    if(kind=="soft") {
        const double f=intervention.at("fraction"),duration=intervention.at("duration");
        require(f>=0 && f<=1 && duration>=0);
        require(intervention.at("parameter")=="alpha" || intervention.at("parameter")=="beta");
        require(intervention.at("shape")=="linear" || intervention.at("shape")=="smooth");
    }
    abm::SyncPopulation<Cell> population;for(auto c:initial) population.spawn(c);
    int tick=0,trigger=-1;double current_alpha=alpha,current_beta=beta;
    population.add_phase([&](std::size_t i,const auto& snapshot) {
        Cell arrived{};const int x=static_cast<int>(i)%n,y=static_cast<int>(i)/n;
        for(int d=0;d<6;++d) {
            const int from=((y-dy[d]+n)%n)*n+(x-dx[d]+n)%n;
            arrived[d]=snapshot[from].value[d];
        }
        return arrived;
    });
    population.add_phase([&](std::size_t i,const auto& snapshot) {
        if(movement=="fhp") return collide(snapshot[i].value,uniform(words[(tick*cells+i)*7+6]));
        Cell shuffled{};const auto& permutation=permutations[tick*cells+i];
        for(int d=0;d<6;++d) shuffled[permutation[d]]=snapshot[i].value[d];
        return shuffled;
    });
    population.add_phase([&](std::size_t i,const auto& snapshot) {
        return disease(snapshot[i].value,current_alpha,current_beta,&words[(tick*cells+i)*7]);
    });
    auto snapshot=[&]() {
        Json states=Json::array();std::array<int,3> counts{};
        for(const auto& record:population.records()) {
            states.push_back(record.value);for(int s:record.value) if(s>=0) ++counts[s];
        }
        require(std::accumulate(counts.begin(),counts.end(),0)==total);
        return Json{{"states",states},{"counts",counts}};
    };
    Json samples=Json::array({snapshot()}),events=Json::array(),rates=Json::array();
    for(tick=0;tick<steps;++tick) {
        int infected=0;for(const auto& record:population.records()) infected+=static_cast<int>(std::count(record.value.begin(),record.value.end(),1));
        if(kind!="none" && trigger<0 && infected>=intervention.at("threshold").get<double>()*total) {
            trigger=tick;int changed=0;
            if(kind=="hard") {
                const int target=intervention.at("target");int eligible=0;
                for(const auto& record:population.records()) eligible+=static_cast<int>(std::count(record.value.begin(),record.value.end(),target));
                const int count=static_cast<int>(std::floor(intervention.at("fraction").get<double>()*eligible));
                for(int slot:priority) {
                    if(changed==count) break;
                    Cell cell=population.records()[slot/6].value;
                    if(cell[slot%6]==target) {cell[slot%6]=2;population.replace(slot/6,cell);++changed;}
                }
                require(changed==count);
            }
            events.push_back({{"time",tick},{"infected_before",infected},{"changed",changed}});
        }
        double reduction=1.;
        if(kind=="soft" && trigger>=0) {
            const double duration=intervention.at("duration");
            double progress=duration==0?1.:std::clamp((tick-trigger)/duration,0.,1.);
            if(intervention.at("shape")=="smooth") progress=progress*progress*(3.-2.*progress);
            reduction=1.-(1.-intervention.at("fraction").get<double>())*progress;
        }
        current_alpha=alpha;current_beta=beta;
        if(kind=="soft" && intervention.at("parameter")=="alpha") current_alpha*=reduction;
        if(kind=="soft" && intervention.at("parameter")=="beta") current_beta*=reduction;
        rates.push_back({current_alpha,current_beta});population.step();samples.push_back(snapshot());
    }
    return {{"samples",samples},{"events",events},{"rates",rates}};
}
Json ode(const Json& spec) {
    const auto initial=spec.at("initial").get<std::array<double,3>>();
    const double population=initial[0]+initial[1]+initial[2],alpha=spec.at("alpha"),beta=spec.at("beta");
    const double gamma=-spec.at("contacts").get<double>()*std::log1p(-alpha)/population,delta=-std::log1p(-beta);
    sd::Model model;const auto s=model.add_stock("S",initial[0]),i=model.add_stock("I",initial[1]),r=model.add_stock("R",initial[2]);
    model.add_flow(s,i,[=](const auto& state,double){return gamma*state[s]*state[i];});
    model.add_flow(i,r,[=](const auto& state,double){return delta*state[i];});
    const int substeps=spec.at("substeps"),horizon=spec.at("horizon");require(substeps>0 && horizon>0);
    Json samples=Json::array({model.state()});const double dt=1./substeps;
    for(int t=0;t<horizon;++t) {
        for(int k=0;k<substeps;++k) model.step(t+k*dt,dt,sd::Integrator::rk4);
        samples.push_back(model.state());
    }
    return {{"gamma",gamma},{"delta",delta},{"samples",samples}};
}
int main(int argc,char** argv) {try {
    require(argc==2);std::ifstream stream(argv[1]);Json input;stream>>input;
    const std::string mode=input.at("mode");require(mode=="ca" || mode=="local" || mode=="ode");
    std::cout<<(mode=="ca"?ca(input):mode=="ode"?ode(input):local(input)).dump()<<'\n';
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}}
