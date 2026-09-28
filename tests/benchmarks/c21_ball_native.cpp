// C21 event-contact ball only. Root localization is workload code, not a
// general production state-event/DAE API. Flight uses the native SD integrator.
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/sd/model.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace ankurafathom;
using Json=nlohmann::json;
class Ball final:public devs::Atomic<int> {
public:
    explicit Ball(const Json& p):dt(p.at("dt")),beta(p.at("beta")),gravity(p.at("g")),mu(p.at("mu")),
        horizon(p.at("horizon")),cutoff(p.at("height_cutoff")),localize(p.value("localize",true)) {
        model.add_stock("height",p.at("height"),false);model.add_stock("velocity",0.,false);
        model.add_flow(sd::Model::boundary,0,[](const auto& state,double){return state[1];},false);
        model.add_flow(sd::Model::boundary,1,[g=gravity,b=beta](const auto& state,double){return -g-b*state[1]*std::abs(state[1]);},false);
        if(dt<=0 || beta<0 || gravity<=0 || mu<=0 || mu>1 || horizon<=0 || cutoff<=0)throw std::invalid_argument("invalid ball parameters");
        prepare();
    }
    double time_advance()const override{return deadline-now;}
    std::optional<double> next_event_time()const override{return deadline;}
    std::vector<devs::PortValue<int>> output()const override{return {};}
    void external_transition(double,const std::vector<devs::Input<int>>&)override {throw std::logic_error("closed ball workload");}
    void confluent_transition(const std::vector<devs::Input<int>>&)override {throw std::logic_error("closed ball workload");}
    void internal_transition()override {
        now=deadline;model=planned;
        if(event) {
            if(ascending) {
                const double h=model.state()[0];apices.push_back({{"time",now},{"height",h}});
                model.add_to_stock(1,-model.state()[1]);ascending=false;
                if(h<cutoff){rest=true;model.add_to_stock(0,-h);}
            } else {
                const double incoming=model.state()[1],outgoing=-mu*incoming;
                impacts.push_back({{"time",now},{"before",incoming},{"after",outgoing},{"height_residual",model.state()[0]}});
                model.add_to_stock(0,-model.state()[0]);model.add_to_stock(1,outgoing-incoming);ascending=true;
            }
        }
        samples.push_back({{"time",now},{"height",model.state()[0]},{"velocity",model.state()[1]},{"event",event},{"rest",rest}});
        prepare();
    }
    Json result()const{return {{"impacts",impacts},{"apices",apices},{"samples",samples},{"rest",rest},{"stop_time",now},
                              {"final_height",model.state()[0]},{"final_velocity",model.state()[1]}};}
private:
    double dt,beta,gravity,mu,horizon,cutoff;bool localize;
    sd::Model model,planned;double now=0,deadline=0;bool ascending=false,event=false,rest=false;
    Json impacts=Json::array(),apices=Json::array(),samples=Json::array();
    sd::Model trial(double duration)const {auto copy=model;if(duration>0)copy.step(now,duration,sd::Integrator::rk4);return copy;}
    void prepare() {
        if(rest || now>=horizon){deadline=std::numeric_limits<double>::infinity();return;}
        const double span=std::min(dt,horizon-now);planned=trial(span);
        const std::size_t guard=ascending?1:0;event=planned.state()[guard]<=0;
        double duration=span;
        if(event && localize) {
            double lo=0,hi=span;
            for(int i=0;i<80;++i) {
                const double mid=lo+(hi-lo)/2;
                if(mid==lo || mid==hi || hi-lo<=1e-13)break;
                if(trial(mid).state()[guard]>0)lo=mid;else hi=mid;
            }
            duration=lo+(hi-lo)/2;planned=trial(duration);
        }
        deadline=now+duration;
        if(deadline<=now)throw std::runtime_error("state event cannot advance clock");
    }
};
int main(int argc,char** argv){try {
    if(argc!=2)throw std::invalid_argument("c21_ball_native plan.json");std::ifstream file(argv[1]);Json p;file>>p;
    devs::Simulator<int> sim;auto ball=std::make_unique<Ball>(p);auto* observed=ball.get();sim.add(std::move(ball));
    int count=0;while(sim.step())if(++count>1000000)throw std::runtime_error("event budget exhausted");
    std::cout<<observed->result().dump()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
