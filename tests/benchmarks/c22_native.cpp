// ARGESIM C22 workload adapter. Custom queue policies inside one DEVS atomic;
// this tests native scheduling, not declarative queue-policy library support.
#include "ankurafathom/devs/simulator.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
using Json=nlohmann::json;
using namespace ankurafathom;
struct Job { int id,group;double arrival,service,start=-1,finish=-1;bool reneged=false,released=false; };
class Queues:public devs::Atomic<int> {
public:
    explicit Queues(const Json& input):mode(input.at("mode")),arrival_first(input.value("arrival_first",false)),
        queues(input.value("queues",4)),busy(queues.size(),-1),deadlines(queues.size(),inf) {
        if(queues.empty())throw std::invalid_argument("empty queues");
        for(const auto& j:input.at("jobs"))jobs.push_back({j.at("id"),j.at("class"),j.at("arrival"),j.at("service")});
        if(jobs.empty())throw std::invalid_argument("empty workload");
    }
    double time_advance()const override{return next()-now;}
    std::optional<double> next_event_time()const override{return next();}
    std::vector<devs::PortValue<int>> output()const override{return {};}
    void external_transition(double,const std::vector<devs::Input<int>>&)override{throw std::logic_error("closed workload");}
    void confluent_transition(const std::vector<devs::Input<int>>&)override{throw std::logic_error("closed workload");}
    void internal_transition()override{
        const double t=next();if(!std::isfinite(t))throw std::logic_error("passive transition");
        area+=(t-now)*waiting();now=t;
        if(arrival_first)arrive();
        for(std::size_t q=0;q<queues.size();++q)if(deadlines[q]==now){
            auto& job=jobs.at(busy[q]);job.finish=now;trace.push_back({{"time",now},{"id",job.id},{"event","finish"},{"queue",q+1}});
            ++terminated;busy[q]=-1;deadlines[q]=inf;
        }
        dispatch();
        if(mode=="reneging")for(std::size_t q=0;q<queues.size();++q){
            auto& line=queues[q];
            for(auto it=line.begin();it!=line.end();){auto& j=jobs.at(*it);
                if(j.arrival+9<=now){j.reneged=true;j.finish=now;++terminated;
                    trace.push_back({{"time",now},{"id",j.id},{"event","renege"},{"queue",q+1}});it=line.erase(it);
                }else ++it;
            }
        }
        if(mode=="jockeying")jockey();
        if(!arrival_first)arrive();
        if(mode=="classing" && now>=10)call();
        max_waiting=std::max(max_waiting,waiting());
        queue_trace.push_back({now,waiting()});
        int active=0;for(int id:busy)active+=id>=0;
        if(static_cast<int>(arrived)!=terminated+waiting()+active)throw std::runtime_error("population conservation");
    }
    Json result()const{
        if(terminated!=static_cast<int>(jobs.size()))throw std::runtime_error("unfinished workload: class policy deadlock");
        Json records=Json::array();double waits=0,max_wait=0;std::vector<double> sum(5),maxima(5);std::vector<int> counts(5);
        for(const auto& j:jobs){const double wait=(j.reneged?j.finish:j.start)-j.arrival;waits+=wait;max_wait=std::max(max_wait,wait);
            records.push_back({{"id",j.id},{"class",j.group},{"arrival",j.arrival},{"start",j.start},{"finish",j.finish},{"reneged",j.reneged},{"wait",wait}});
            sum.at(j.group-1)+=wait;maxima.at(j.group-1)=std::max(maxima.at(j.group-1),wait);++counts.at(j.group-1);
        }
        Json classes=Json::array();for(int c=0;c<5;++c)classes.push_back({{"class",c+1},{"mean_wait",counts[c]?sum[c]/counts[c]:0},{"max_wait",maxima[c]}});
        return {{"jobs",records},{"events",trace},{"queue_trace",queue_trace},{"horizon",now},{"queue_area",area},
            {"queue_mean",area/now},{"queue_max",max_waiting},{"wait_mean",waits/jobs.size()},{"wait_max",max_wait},{"classes",classes}};
    }
private:
    static constexpr double inf=std::numeric_limits<double>::infinity();
    std::string mode;bool arrival_first;std::vector<std::deque<int>> queues;std::vector<int> busy;std::vector<double> deadlines;
    std::vector<Job> jobs;std::size_t arrived=0;int terminated=0,current=0,max_waiting=0;double now=0,area=0;
    Json trace=Json::array(),queue_trace=Json::array();
    int waiting()const{int total=0;for(const auto& q:queues)total+=static_cast<int>(q.size());return total;}
    int length(std::size_t q)const{return static_cast<int>(queues[q].size())+(busy[q]>=0);}
    double next()const{
        if(terminated==static_cast<int>(jobs.size()))return inf;
        double t=arrived<jobs.size()?jobs[arrived].arrival:inf;
        for(double d:deadlines)t=std::min(t,d);
        if(mode=="reneging")for(const auto& q:queues)for(int id:q)t=std::min(t,jobs[id].arrival+9);
        if(mode=="classing" && now<10)t=std::min(t,10.);
        return t;
    }
    void dispatch(){
        for(std::size_t q=0;q<queues.size();++q)if(busy[q]<0 && !queues[q].empty()){
            const int id=queues[q].front();auto& j=jobs.at(id);
            if(mode=="classing" && !j.released)continue;
            queues[q].pop_front();busy[q]=id;j.start=now;deadlines[q]=now+j.service;
            trace.push_back({{"time",now},{"id",j.id},{"event","start"},{"queue",q+1}});
        }
    }
    void arrive(){
        while(arrived<jobs.size() && jobs[arrived].arrival==now){
            std::size_t q=0;for(std::size_t i=1;i<queues.size();++i)if(length(i)<length(q))q=i;
            const int id=static_cast<int>(arrived++);queues[q].push_back(id);
            trace.push_back({{"time",now},{"id",jobs[id].id},{"event","arrive"},{"queue",q+1}});
            dispatch();
        }
    }
    void jockey(){
        while(true){int destination=-1,source=-1;
            for(std::size_t d=0;d<queues.size() && destination<0;++d)
                for(std::size_t s=0;s<queues.size();++s)if(length(s)>=length(d)+2){destination=static_cast<int>(d);break;}
            if(destination<0)return;
            for(std::size_t s=0;s<queues.size();++s)if(length(s)>=length(destination)+2 &&
                (source<0 || std::abs(static_cast<int>(s)-destination)<std::abs(source-destination)))source=static_cast<int>(s);
            const int id=queues[source].back();queues[source].pop_back();queues[destination].push_back(id);
            trace.push_back({{"time",now},{"id",jobs[id].id},{"event","jockey"},{"from",source+1},{"queue",destination+1}});
            dispatch();
        }
    }
    bool class_present(int c)const{
        for(int id:busy)if(id>=0 && jobs[id].group==c)return true;
        for(const auto& q:queues)for(int id:q)if(jobs[id].group==c && jobs[id].released)return true;
        return false;
    }
    void call(){
        if(waiting()==0 && std::all_of(busy.begin(),busy.end(),[](int i){return i<0;}))return;
        if(current && class_present(current))return;
        // Empty classes are skipped at the same physical timestamp, descending.
        for(int n=0;n<5;++n){current=current<=1?5:current-1;
            trace.push_back({{"time",now},{"event","call"},{"class",current}});
            // Published MatlabGPSS solution: a call releases a snapshot batch;
            // later arrivals wait in the incoming chain until a subsequent call.
            for(auto& q:queues){for(int id:q)jobs[id].released=jobs[id].group==current;
                std::stable_partition(q.begin(),q.end(),[&](int id){return jobs[id].released;});}
            dispatch();if(class_present(current))return;
        }
        throw std::runtime_error("class operator failed to select work");
    }
};
int main(int argc,char** argv){try{
    if(argc!=2)throw std::invalid_argument("c22_native workload.json");std::ifstream file(argv[1]);Json input;file>>input;
    auto workload=std::make_unique<Queues>(input);auto* result=workload.get();devs::Simulator<int> sim;sim.add(std::move(workload));
    std::size_t steps=0;while(sim.step())if(++steps>1000000)throw std::runtime_error("event limit");
    auto value=result->result();value["kernel_steps"]=steps;std::cout<<value.dump()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
