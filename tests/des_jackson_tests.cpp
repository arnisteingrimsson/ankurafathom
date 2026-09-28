#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/des/process.hpp"
#include "ankurafathom/des/routing.hpp"
#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <string>

namespace {
using namespace ankurafathom;
using Json = nlohmann::json;
using Values = std::map<std::string, double>;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void close(double actual, double expected, const std::string& message) {
    require(std::abs(actual-expected) <= 2e-9*std::max({1.0,std::abs(actual),std::abs(expected)}), message);
}

Values replication(const Json& plan, const Json& spec, std::uint32_t rep) {
    const double warm=plan.at("warmup"), window=plan.at("window"), end=warm+window;
    const std::uint64_t seed=plan.at("native_seed");
    const std::uint32_t scenario=spec.at("scenario");
    const bool branched=spec.contains("branch_probability");
    std::vector<des::Entity> arrivals;
    double arrival=0;
    for (std::uint64_t id=0;;++id) {
        arrival+=rng::exponential(plan.at("arrival_rate"), rng::draw(seed,
            {scenario,rep,id,0,plan.at("arrival_stream"),0})[0]);
        if (arrival>end) break;
        arrivals.push_back({id,arrival,1});
    }
    devs::Simulator<des::Entity> sim;
    auto source=std::make_unique<des::ScheduledSource<>>(arrivals);
    const auto* emitted=source.get();
    auto previous=sim.add(std::move(source));
    std::array<des::MultiServer<>*,3> stations{};
    std::array<std::size_t,3> ids{};
    std::array<des::ExponentialService,3> services{};
    for (std::size_t i=0;i<3;++i) {
        services[i]={spec.at("service_rates").at(i),seed,scenario,rep,plan.at("service_streams").at(i)};
        auto station=std::make_unique<des::MultiServer<>>(1,1,des::QueueOptions{},false,services[i]);
        stations[i]=station.get();
        ids[i]=sim.add(std::move(station));
        if (!branched || i==0) sim.connect(previous,i==0 ? 0 : 1,ids[i],0);
        previous=ids[i];
    }
    std::optional<std::size_t> router_id;
    des::ProbabilityRouter<>* router=nullptr;
    const des::BernoulliRouting routing{branched ? spec.at("branch_probability").get<double>() : 0.0,
        seed,scenario,rep,branched ? plan.at("routing_stream").get<std::uint32_t>() : 0};
    if (branched) {
        auto model=std::make_unique<des::ProbabilityRouter<>>(routing);
        router=model.get();
        router_id=sim.add(std::move(model));
        sim.connect(ids[0],1,*router_id,0);
        sim.connect(*router_id,1,ids[1],0);
        sim.connect(*router_id,2,ids[2],0);
    }
    std::array<std::size_t,2> delivered{}, measured_routes{};
    std::array<std::vector<des::Entity>,3> completed;
    std::array<double,3> queues{}, busy{}, initial_queues{}, initial_busy{};
    std::array<std::uint64_t,3> initial_completed{}, departures{};
    std::array<double,27> joint{};
    double observed=0;
    const auto integrate=[&](double until) {
        const double elapsed=std::max(0.0,std::min(until,end)-std::max(observed,warm));
        std::size_t cell=0;
        for (std::size_t i=0;i<3;++i) {
            queues[i]+=elapsed*stations[i]->waiting();
            busy[i]+=elapsed*stations[i]->busy_servers();
            cell=3*cell+std::min(std::size_t{2},stations[i]->waiting()+stations[i]->busy_servers());
        }
        joint.at(cell)+=elapsed;
        observed=until;
    };
    const auto advance=[&](double until) {
        while (std::isfinite(sim.next_time()) && sim.next_time()<=until) {
            integrate(sim.next_time());
            const auto step=*sim.step();
            for (const auto& event:step.emissions) if (router_id && event.source==*router_id) {
                require(event.port==1 || event.port==2,"unexpected routing port");
                require((event.port==1)==routing.matches(event.value.id),"routing draw differs");
                ++delivered.at(event.port-1);
                if (step.time>warm && step.time<=end) ++measured_routes.at(event.port-1);
            }
            for (const auto& event:step.emissions) for (std::size_t i=0;i<3;++i) if (event.source==ids[i]) {
                require(event.port==1 && (completed[i].empty() || event.value.id>completed[i].back().id),
                        "FIFO identity/order changed");
                if (!branched || i==0) require(event.value.id==completed[i].size(),"station skipped an entity");
                else require((i==1)==routing.matches(event.value.id),"entity completed at wrong branch");
                require(event.value.service_duration==1,"station overwrote source service metadata");
                completed[i].push_back(event.value);
            }
            for (std::size_t i=0;i<3;++i) {
                require(stations[i]->accepted_count()==(i==0 ? emitted->emitted_count() :
                        branched ? delivered[i-1] : stations[i-1]->completed_count()),
                        "inter-station conservation failed");
                require(stations[i]->accepted_count()==stations[i]->completed_count()+stations[i]->waiting()+stations[i]->busy_servers()
                        && stations[i]->rejected_count()==0,"station conservation failed");
            }
            if (router) require(router->received_count()==stations[0]->completed_count()
                    && delivered[0]+delivered[1]+router->output().size()==router->received_count(),
                    "router conservation failed");
        }
        if (std::isfinite(until)) integrate(until);
    };
    advance(warm);
    for (std::size_t i=0;i<3;++i) {
        initial_queues[i]=stations[i]->mean_queue_length(warm)*warm;
        initial_busy[i]=stations[i]->utilization(warm)*warm;
        initial_completed[i]=stations[i]->completed_count();
    }
    advance(end);
    for (std::size_t i=0;i<3;++i) {
        close(queues[i],stations[i]->mean_queue_length(end)*end-initial_queues[i],"observer/station queue integral differs");
        close(busy[i],stations[i]->utilization(end)*end-initial_busy[i],"observer/station service integral differs");
        departures[i]=stations[i]->completed_count()-initial_completed[i];
    }
    close(std::accumulate(joint.begin(),joint.end(),0.0),window,"joint probability mass does not sum to one");
    advance(std::numeric_limits<double>::infinity()); // Complete cohorts without censoring waits/cycles.
    Values values;
    double cycles=0;
    std::size_t cycle_count=0;
    const auto overlap=[&](double begin, double finish) { return std::max(0.0,std::min(finish,end)-std::max(begin,warm)); };
    for (std::size_t i=0;i<3;++i) {
        const auto expected=(!branched || i==0) ? arrivals.size() : static_cast<std::size_t>(
            std::count_if(arrivals.begin(),arrivals.end(),[&](const auto& job) { return (i==1)==routing.matches(job.id); }));
        require(completed[i].size()==expected && stations[i]->waiting()==0 && stations[i]->busy_servers()==0,
                "drain lost jobs");
        double wait=0, all_wait=0, queue_area=0, busy_area=0, prior_finish=0;
        std::size_t cohort=0;
        for (const auto& job:completed[i]) {
            const double entry=i==0 ? arrivals.at(job.id).arrived_at : completed[branched ? 0 : i-1].at(job.id).completed_at;
            const double start=std::max(entry,prior_finish);
            // Independent tandem max-plus recurrence checks every service completion.
            prior_finish=start+services[i].duration(job.id);
            close(job.entered_at,entry,"station-entry timestamp differs");
            close(job.completed_at,prior_finish,"completion differs from max-plus reference");
            all_wait+=start-entry;
            queue_area+=overlap(entry,start);
            busy_area+=overlap(start,job.completed_at);
            if (entry>warm && entry<=end) { wait+=start-entry; ++cohort; }
            if ((branched ? i>0 : i==2) && job.arrived_at>warm && job.arrived_at<=end) {
                cycles+=job.completed_at-job.arrived_at; ++cycle_count;
            }
        }
        require(cohort>0,"empty stage cohort");
        close(queue_area,queues[i],"individual waiting intervals differ from queue integral");
        close(busy_area,busy[i],"individual service intervals differ from busy integral");
        close(all_wait,stations[i]->total_waiting_time(),"individual waits differ from station total");
        const auto prefix="s"+std::to_string(i)+"_";
        values[prefix+"queue"]=queues[i]/window;
        values[prefix+"utilization"]=busy[i]/window;
        values[prefix+"wait"]=wait/cohort;
        values[prefix+"throughput"]=static_cast<double>(departures[i])/window;
    }
    require(cycle_count>0,"empty cycle cohort");
    require(cycle_count==static_cast<std::size_t>(std::count_if(arrivals.begin(),arrivals.end(),
        [&](const auto& job) { return job.arrived_at>warm && job.arrived_at<=end; })),"cycle cohort lost entities");
    values["cycle"]=cycles/cycle_count;
    if (branched) {
        require(measured_routes[0]+measured_routes[1]>0,"empty routing cohort");
        values["match_fraction"]=static_cast<double>(measured_routes[0])/(measured_routes[0]+measured_routes[1]);
    }
    for (std::size_t cell=0;cell<27;++cell)
        values["p"+std::to_string(cell/9)+std::to_string((cell/3)%3)+std::to_string(cell%3)]=joint[cell]/window;
    return values;
}
}

int main(int argc,char** argv) {
    try {
        require(argc==3,"usage: des_jackson_tests jackson-plan.json report.json");
        std::ifstream input(argv[1]); Json plan; input>>plan;
        require(plan.at("version")==1,"unknown Jackson plan version");
        Json report={{"plan_version",1},{"native_seed",plan.at("native_seed")},
                     {"warmup",plan.at("warmup")},{"window",plan.at("window")},{"compiler",__VERSION__},
                     {"ci_method","normal approximation across independent replication means"},{"cases",Json::array()}};
        bool passed=true;
        for (const auto& spec:plan.at("cases")) {
            const std::size_t count=spec.at("replications");
            require(count>=16,"replication minimum not met");
            std::map<std::string,std::vector<double>> samples;
            for (std::size_t rep=0;rep<count;++rep) {
                try {
                    for (const auto& [key,value]:replication(plan,spec,static_cast<std::uint32_t>(rep)))
                        samples[key].push_back(value);
                } catch (const std::exception& error) {
                    throw std::runtime_error(spec.at("name").get<std::string>()+" replication "+std::to_string(rep)+": "+error.what());
                }
            }
            Json result={{"name",spec.at("name")},{"replications",count},{"metrics",Json::object()}};
            for (const auto& [key,gate]:spec.at("metrics").items()) {
                const auto& values=samples.at(key);
                const double mean=std::accumulate(values.begin(),values.end(),0.0)/count;
                double squares=0;
                for (double value:values) squares+=(value-mean)*(value-mean);
                const double se=std::sqrt(squares/(count-1)/count);
                const double target=gate.at("target"), tolerance=gate.at("tolerance");
                const bool valid=std::isfinite(mean) && std::abs(mean-target)<=tolerance;
                passed=passed && valid;
                result["metrics"][key]={{"target",target},{"mean",mean},{"tolerance",tolerance},
                    {"standard_error",se},{"ci95",{mean-1.959963984540054*se,mean+1.959963984540054*se}},{"pass",valid}};
                if (!valid) std::cerr<<spec.at("name")<<' '<<key<<": "<<mean<<" target="<<target<<" gate="<<tolerance<<'\n';
            }
            report["cases"].push_back(result);
            std::cout<<spec.at("name")<<": "<<count<<" replications checked\n"<<std::flush;
        }
        report["pass"]=passed;
        std::ofstream output(argv[2]); output<<std::setw(2)<<report<<'\n';
        require(output.good(),"could not write Jackson report");
        require(passed,"Jackson acceptance gate failed; inspect without retuning thresholds");
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
