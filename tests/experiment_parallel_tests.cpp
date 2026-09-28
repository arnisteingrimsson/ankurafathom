#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/rng/philox.hpp"
#include <bit>
#include <chrono>
#include <iostream>

namespace rt=ankurafathom::runtime;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }require(caught,"invalid parallel experiment accepted"); }
rt::Experiment experiment(std::size_t count=1000,std::uint32_t replications=3) {
    rt::Experiment e{std::numeric_limits<std::uint64_t>::max(),{},replications};
    for(std::size_t i=0;i<count;++i) e.scenarios.push_back({static_cast<std::uint32_t>(65536-count+i),{{"growth",.25+(i%7)/8.}}});
    return e;
}
std::vector<rt::Observation> execute(const rt::Scenario& scenario,std::uint32_t replication,std::uint64_t seed) {
    std::vector<rt::Observation> rows;double state=1;
    for(std::uint32_t step=0;step<8;++step) {
        const ankurafathom::rng::DrawAddress address{scenario.id,replication,(std::uint64_t{1}<<48)-1,step,65535,0};
        const auto u=ankurafathom::rng::uniform_open(ankurafathom::rng::draw(seed,address)[0]);
        state+=scenario.parameters.at("growth")*u;
        rows.push_back({step*.125,"stock",state});rows.push_back({step*.125,"draw",u});
        if((scenario.id+replication+step)%7==0) std::this_thread::yield();
    }
    return rows;
}
void same(const std::vector<rt::Trajectory>& a,const std::vector<rt::Trajectory>& b) {
    require(a.size()==b.size(),"trajectory count changed");
    for(std::size_t i=0;i<a.size();++i) {
        require(a[i].scenario==b[i].scenario && a[i].replication==b[i].replication && a[i].observations.size()==b[i].observations.size(),"trajectory merge order changed");
        for(std::size_t j=0;j<a[i].observations.size();++j) {
            const auto& x=a[i].observations[j];const auto& y=b[i].observations[j];
            require(x.time==y.time && x.output_id==y.output_id && std::bit_cast<std::uint64_t>(x.value)==std::bit_cast<std::uint64_t>(y.value),"thread count changed decoded numeric bits");
        }
    }
    const auto x=rt::summarize(a),y=rt::summarize(b);require(x.size()==y.size(),"summary shape changed");
    for(std::size_t i=0;i<x.size();++i) require(x[i].scenario==y[i].scenario && x[i].time==y[i].time && x[i].output_id==y[i].output_id &&
        x[i].count==y[i].count && x[i].mean==y[i].mean && x[i].sample_variance==y[i].sample_variance,"summary changed across threads");
}
void determinism() {
    const auto e=experiment();const auto baseline=rt::run_experiment(e,execute);
    const auto caller=std::this_thread::get_id();
    for(const std::size_t threads:{2,7,32}) {
        std::size_t calls=0;rt::ExecutionOptions options;options.threads=threads;
        options.progress=[&](const auto& progress) {
            require(std::this_thread::get_id()==caller && progress.total==3000 && progress.completed==calls,"unordered or worker-thread progress");++calls;return true;
        };
        same(baseline,rt::run_experiment(e,execute,options));require(calls==3001,"missing progress notifications");
    }
    const auto boundary=experiment(1,65536);const auto values=rt::run_experiment(boundary,[](const auto& s,std::uint32_t rep,std::uint64_t seed) {
        require(seed==std::numeric_limits<std::uint64_t>::max() && s.id==65535,"maximum seed/scenario changed");
        return std::vector<rt::Observation>{{0,"replication",static_cast<double>(rep)}};
    },{7});
    require(values.size()==65536 && values.back().replication==65535 && values.back().observations[0].value==65535,"maximum replication address changed");
}
void isolation_and_errors() {
    const auto e=experiment(20,1);
    for(const std::size_t threads:{1,2,7}) {
        const auto source_clock=rt::run_experiment(e,[](const auto&,auto,auto) {
            return std::vector<rt::Observation>{{-2,"x",3},{-1,"x",4},{0,"x",5}};
        },{threads});
        for(const auto& result:source_clock) require(result.observations.front().time==-2 && result.observations.back().time==0,
            "imported source clock changed during execution");
        const auto isolated=rt::run_experiment(e,[calls=0](const auto&,auto,auto)mutable {
            return std::vector<rt::Observation>{{0,"copy_count",static_cast<double>(++calls)}};
        },{threads});
        for(const auto& result:isolated) require(result.observations[0].value==1,"trajectory callbacks share mutable value state");
        bool earliest=false;
        try { rt::run_experiment(e,[](const auto& scenario,auto,auto) {
            if(scenario.id==65518) { for(int j=0;j<1000;++j) std::this_thread::yield();throw std::runtime_error("earliest"); }
            if(scenario.id==65524) throw std::runtime_error("later");
            return std::vector<rt::Observation>{{0,"x",1}};
        },{threads}); }catch(const std::runtime_error& error) { earliest=std::string(error.what())=="earliest"; }
        require(earliest,"failure selection depends on worker completion order");
        for(int bad=0;bad<5;++bad) rejects([&] { rt::run_experiment(e,[bad](const auto&,auto,auto) {
            if(bad==0) return std::vector<rt::Observation>{{0,"x",NAN}};
            if(bad==1) return std::vector<rt::Observation>{{NAN,"x",1}};
            if(bad==2) return std::vector<rt::Observation>{{0,"",1}};
            if(bad==3) return std::vector<rt::Observation>{{0,"x",1},{0,"x",2}};
            return std::vector<rt::Observation>{{INFINITY,"x",1}};
        },{threads}); });
    }
    rejects([&] { rt::run_experiment(e,execute,{0}); });rejects([&] { rt::run_experiment(e,execute,{257}); });
    rejects([] { rt::run_experiment(experiment(17,65536),execute); });
}
void cancellation_and_join() {
    const auto e=experiment(100,1);
    for(const std::size_t threads:{1,2,7}) {
        std::atomic<int> active=0;std::size_t calls=0;rt::ExecutionOptions options;options.threads=threads;
        options.progress=[&](const auto& progress) { require(progress.completed==calls++,"cancel progress order");return progress.completed<7; };
        bool cancelled=false;
        try { rt::run_experiment(e,[&active](const auto&,auto,auto) {
            ++active;std::this_thread::sleep_for(std::chrono::milliseconds(1));--active;
            return std::vector<rt::Observation>{{0,"x",1}};
        },options); }catch(const rt::ExperimentCancelled& error) { cancelled=error.completed==7 && error.total==100; }
        require(cancelled && active.load()==0 && calls==8,"cancellation returned partial result or left worker alive");
        calls=0;options.progress=[&](const auto& progress) { ++calls;if(progress.completed==5) throw std::runtime_error("progress failed");return true; };
        rejects([&] { rt::run_experiment(e,[&active](const auto&,auto,auto) {
            ++active;std::this_thread::sleep_for(std::chrono::milliseconds(1));--active;return std::vector<rt::Observation>{};
        },options); });require(active.load()==0 && calls==6,"progress failure did not join workers");
        std::stop_source stop;options.progress={};options.stop=stop.get_token();cancelled=false;
        try { rt::run_experiment(e,[&stop](const auto&,auto,auto) { stop.request_stop();return std::vector<rt::Observation>{}; },options); }
        catch(const rt::ExperimentCancelled& error) { cancelled=error.completed==0 && error.total==100; }
        require(cancelled,"external cancellation not observed before merge");
        std::atomic<int> executed=0;
        rejects([&] { rt::run_experiment(e,[&executed](const auto&,auto,auto) { ++executed;return std::vector<rt::Observation>{}; },options); });
        require(executed.load()==0,"pre-cancelled runner executed work");
    }
}
int main() {
    try { determinism();isolation_and_errors();cancellation_and_join();std::cout<<"1000-scenario deterministic execution, high addresses, cancellation, ordered errors and joins passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
