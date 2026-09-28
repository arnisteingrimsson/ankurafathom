#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/des/process.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>

namespace {
using namespace ankurafathom;
using Json = nlohmann::json;
using Values = std::map<std::string,double>;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void close(double actual, double expected, const std::string& message) {
    require(std::abs(actual-expected) <= 2e-9*std::max({1.0,std::abs(actual),std::abs(expected)}),message);
}
std::vector<des::Entity> schedule(const Json& spec, std::uint64_t seed, std::uint32_t rep, double end) {
    std::vector<des::Entity> result;
    const double rate=spec.at("arrival_rate");
    const std::string service=spec.at("service");
    double time=0;
    for (std::uint64_t id=0;;++id) {
        const auto word=[&](std::uint32_t stream) {
            return rng::draw(seed,{spec.at("scenario").get<std::uint32_t>(),rep,id,0,stream,0})[0];
        };
        time+=rng::exponential(rate,word(100));
        if (time>end) break;
        double duration=1;
        if (service=="exponential") duration=rng::exponential(1,word(101));
        else if (service=="erlang2") duration=rng::exponential(2,word(101))+rng::exponential(2,word(102));
        else if (service=="two_point") duration=rng::bernoulli(0.8,word(101)) ? 0.25 : 4.0;
        else require(service=="deterministic","unknown service distribution");
        result.push_back({id,time,duration});
    }
    return result;
}

Values replication(const Json& spec, std::uint64_t seed, std::uint32_t rep, double warm, double window) {
    const double end=warm+window;
    const std::size_t capacity=spec.at("servers");
    const bool finite=!spec.at("queue_capacity").is_null();
    des::QueueOptions options;
    if (finite) options.capacity=spec.at("queue_capacity").get<std::size_t>();
    const auto arrivals=schedule(spec,seed,rep,end);
    devs::Simulator<des::Entity> sim;
    auto source=std::make_unique<des::ScheduledSource<>>(arrivals);
    const auto* emitted=source.get();
    const auto source_id=sim.add(std::move(source));
    auto server=std::make_unique<des::MultiServer<>>(capacity,1,options,finite);
    const auto* station=server.get();
    const auto server_id=sim.add(std::move(server));
    sim.connect(source_id,0,server_id,0);
    std::vector<des::Entity> completed;
    std::set<std::uint64_t> completed_ids, rejected_ids;
    std::vector<double> occupancy(finite ? capacity+*options.capacity+1 : 0,0);
    double observed_time=0, queue_area=0, busy_area=0;
    const auto integrate=[&](double until) {
        const double elapsed=std::max(0.0,std::min(until,end)-std::max(observed_time,warm));
        queue_area+=elapsed*station->waiting();
        busy_area+=elapsed*station->busy_servers();
        if (finite) occupancy.at(station->waiting()+station->busy_servers())+=elapsed;
        observed_time=until;
    };
    const auto advance=[&](double until) {
        while (sim.next_time()<=until && std::isfinite(sim.next_time())) {
            integrate(sim.next_time());
            devs::StepResult<des::Entity> step;
            try { step=*sim.step(); }
            catch (const std::exception& error) {
                std::ostringstream context;
                context<<std::setprecision(17)<<"event time "<<sim.now()<<": "<<error.what();
                throw std::runtime_error(context.str());
            }
            for (const auto& event:step.emissions) if (event.source==server_id) {
                if (event.port==des::MultiServer<>::output_port) {
                    require(completed_ids.insert(event.value.id).second,"duplicate completion");
                    completed.push_back(event.value);
                } else {
                    require(event.port==des::MultiServer<>::rejection_port &&
                            rejected_ids.insert(event.value.id).second,"duplicate/unknown rejection publication");
                }
            }
            require(station->accepted_count()+station->rejected_count()==emitted->emitted_count(),
                    "offered/admitted/rejected conservation failed");
            require(station->accepted_count()==station->completed_count()+station->waiting()+station->busy_servers(),
                    "station conservation failed");
            require(station->busy_servers()<=capacity && (!finite || station->waiting()<=*options.capacity),
                    "capacity bound failed");
        }
        if (std::isfinite(until)) integrate(until);
    };
    advance(warm);
    const double initial_queue_area=station->mean_queue_length(warm)*warm;
    const double initial_busy_area=station->utilization(warm)*capacity*warm;
    const auto initial_emitted=emitted->emitted_count();
    const auto initial_rejected=station->rejected_count();
    const auto initial_completed=station->completed_count();
    const auto initial_accepted=station->accepted_count();
    require(rejected_ids.size()==initial_rejected,"warm-up boundary left rejection publications pending");
    advance(end);
    close(queue_area,station->mean_queue_length(end)*end-initial_queue_area,"station/observer queue area differs");
    close(busy_area,station->utilization(end)*capacity*end-initial_busy_area,"station/observer busy area differs");
    const auto offers=emitted->emitted_count()-initial_emitted;
    const auto losses=station->rejected_count()-initial_rejected;
    const auto accepted=station->accepted_count()-initial_accepted;
    const auto departures=station->completed_count()-initial_completed;
    require(offers>0 && accepted>0 && offers==accepted+losses,"invalid measured cohort");
    require(rejected_ids.size()==station->rejected_count(),"measurement boundary left rejection publications pending");
    if (finite) close(std::accumulate(occupancy.begin(),occupancy.end(),0.0),window,"state probabilities not normalized");
    // Stop input at end, then finish every admitted arrival in the measurement cohort.
    // Later arrivals cannot delay these jobs in a FIFO station.
    advance(std::numeric_limits<double>::infinity());
    require(station->waiting()==0 && station->busy_servers()==0 &&
            station->completed_count()==station->accepted_count(),"drain left accepted jobs censored");
    double wait=0, all_wait=0, entity_queue_area=0, entity_busy_area=0, previous_finish=0;
    std::size_t cohort=0;
    const auto overlap=[&](double begin,double finish) {
        return std::max(0.0,std::min(finish,end)-std::max(begin,warm));
    };
    for (const auto& entity:completed) {
        require(!rejected_ids.contains(entity.id),"rejected job also completed");
        const double start=entity.completed_at-entity.service_duration;
        const double waiting=start-entity.arrived_at;
        require(waiting>=-1e-9,"negative wait");
        all_wait+=waiting;
        entity_queue_area+=overlap(entity.arrived_at,start);
        entity_busy_area+=overlap(start,entity.completed_at);
        if (!finite) {
            // Independent Lindley recursion, with absolute completion times.
            const double expected_start=std::max(previous_finish,entity.arrived_at);
            close(start,expected_start,"M/G/1 completion differs from Lindley recursion");
            previous_finish=expected_start+entity.service_duration;
        }
        if (entity.arrived_at>warm && entity.arrived_at<=end) { wait+=waiting; ++cohort; }
    }
    require(cohort==accepted && completed.size()+rejected_ids.size()==arrivals.size(),"cohort identity accounting failed");
    close(all_wait,station->total_waiting_time(),"drained waiting time differs from individual histories");
    close(entity_queue_area,queue_area,"clipped individual waits differ from queue area");
    close(entity_busy_area,busy_area,"clipped individual service differs from busy area");
    Values values{{"queue",queue_area/window},{"utilization",busy_area/(capacity*window)},
                  {"wait",wait/cohort},{"throughput",static_cast<double>(departures)/window},
                  {"blocking",static_cast<double>(losses)/offers}};
    for (std::size_t n=0;n<occupancy.size();++n) values["p"+std::to_string(n)]=occupancy[n]/window;
    return values;
}
}

int main(int argc,char** argv) {
    try {
        require(argc==3,"usage: des_statistical_tests queue-plan.json report.json");
        std::ifstream input(argv[1]); Json plan; input>>plan;
        require(plan.at("version")==1,"unknown queue validation plan");
        Json report={{"plan_version",1},{"native_seed",plan.at("native_seed")},
                     {"warmup",plan.at("warmup")},{"window",plan.at("window")},
                     {"compiler",__VERSION__},{"ci_method","normal approximation across independent replication means"},
                     {"cases",Json::array()}};
        bool passed=true;
        for (const auto& spec:plan.at("cases")) {
            const std::size_t count=spec.at("replications");
            require(count>=16,"replication minimum not met");
            std::map<std::string,std::vector<double>> samples;
            for (std::size_t rep=0;rep<count;++rep) {
                try {
                    for (const auto& [key,value]:replication(spec,plan.at("native_seed"),static_cast<std::uint32_t>(rep),
                                                           plan.at("warmup"),plan.at("window")))
                        samples[key].push_back(value);
                } catch (const std::exception& error) {
                    throw std::runtime_error(spec.at("name").get<std::string>()+" replication "+
                                             std::to_string(rep)+": "+error.what());
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
                const double allowed=tolerance==0 ? plan.at("numerical_epsilon").get<double>() : tolerance;
                const bool valid=std::isfinite(mean) && std::abs(mean-target)<=allowed;
                passed=passed && valid;
                result["metrics"][key]={{"target",target},{"mean",mean},{"tolerance",tolerance},
                    {"standard_error",se},{"ci95",{mean-1.959963984540054*se,mean+1.959963984540054*se}},
                    {"pass",valid}};
                if (!valid) std::cerr<<spec.at("name")<<' '<<key<<": "<<mean<<" target="<<target<<" gate="<<tolerance<<'\n';
            }
            report["cases"].push_back(result);
            std::cout<<spec.at("name")<<": "<<count<<" replications checked\n"<<std::flush;
        }
        report["pass"]=passed;
        std::ofstream output(argv[2]); output<<std::setw(2)<<report<<'\n';
        require(output.good(),"could not write statistical report");
        require(passed,"queue statistical acceptance gate failed; inspect report without retuning thresholds");
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
