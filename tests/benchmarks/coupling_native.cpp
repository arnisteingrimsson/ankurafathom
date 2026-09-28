#include "ankurafathom/hybrid/clocked_sd.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
using namespace ankurafathom;
using Json=nlohmann::json;
class ZeroEvents final:public devs::Atomic<double> {
public:
    const std::vector<double> times{.13,.37,.91,1.61,2.03,3.77};
    double time_advance()const override {return index<times.size()?times[index]-now:INFINITY;}
    std::optional<double> next_event_time()const override {return index<times.size()?times[index]:INFINITY;}
    std::vector<devs::PortValue<double>> output()const override {return {{0,0.}};}
    void internal_transition()override {now=times.at(index++);}
    void external_transition(double,const std::vector<devs::Input<double>>&)override {throw std::logic_error("source input");}
    void confluent_transition(const std::vector<devs::Input<double>>&)override {throw std::logic_error("source confluence");}
private:std::size_t index=0;double now=0;
};
sd::Model model(bool decay) {
    sd::Model result;const auto stock=result.add_stock("amount",decay?60.:2.);
    if(decay) result.add_flow(stock,sd::Model::boundary,[stock](const auto& state,double){return .4*state[stock];});
    else result.add_flow(sd::Model::boundary,stock,[](const auto&,double){return 3.;});
    return result;
}
int main() {try {
    Json results=Json::array();
    for(bool decay:{false,true}) for(double dt:{.5,.25,.125,.0625}) {
        devs::Simulator<double> sim;const auto source=sim.add(std::make_unique<ZeroEvents>());
        auto component=std::make_unique<hybrid::ClockedSD<double>>(model(decay),dt);auto* observed=component.get();
        const auto target=sim.add(std::move(component));sim.connect(source,0,target,0);
        auto standalone=model(decay);double previous=0.;Json samples=Json::array();
        while(previous<4.) {
            const auto event=sim.step();if(!event) throw std::runtime_error("missing clock event");
            const double time=event->time;if(time>4.) throw std::runtime_error("missed horizon");
            standalone.step(previous,time-previous);
            samples.push_back({{"time",time},{"coupled",observed->model().state()[0]},
                               {"standalone",standalone.state()[0]},{"clock",observed->time()}});
            previous=time;
        }
        results.push_back({{"decay",decay},{"dt",dt},{"samples",samples}});
    }
    std::cout<<results.dump()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
