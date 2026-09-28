#include "ankurafathom/ir/model.hpp"
#include <array>
#include <iostream>
#include <map>

namespace {
using namespace ankurafathom;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void close(double a,double b) { require(std::abs(a-b)<1e-10*std::max({1.0,std::abs(a),std::abs(b)}),"numeric mismatch"); }
template<class F> void rejects(F call) {
    bool caught=false;
    try { call(); } catch (const std::exception&) { caught=true; }
    require(caught,"invalid input accepted");
}

void addressing_and_rollback() {
    des::ExponentialService policy{1.25,1234567,7,9,201};
    const auto expected=[&](std::uint64_t id) {
        return -std::log((static_cast<double>(rng::draw(1234567,{7,9,id,0,201,0})[0])+.5)/4294967296.0)/1.25;
    };
    std::vector<devs::Input<des::Entity>> bag{{0,0,{3,0,11}}, {0,0,{1,0,99}}, {0,0,{2,0,123}}};
    std::map<std::uint64_t,double> first;
    for (bool reverse:{false,true}) {
        if (reverse) std::reverse(bag.begin(),bag.end());
        des::MultiServer<> station(3,2,{},false,policy);
        station.external_transition(0,bag);
        while (std::isfinite(station.time_advance())) {
            for (const auto& out:station.output()) {
                close(out.value.completed_at,2*expected(out.value.id));
                if (reverse) close(out.value.completed_at,first.at(out.value.id));
                else first[out.value.id]=out.value.completed_at;
                require(out.value.service_duration==(out.value.id==3 ? 11 : out.value.id==1 ? 99 : 123),
                        "source duration was mutated");
            }
            station.internal_transition();
        }
    }
    des::MultiServer<> station(1,1,{1,des::QueueDiscipline::fifo},true,policy);
    station.external_transition(0,{{0,0,{1,0,1}}});
    const double deadline=*station.next_event_time();
    rejects([&] { station.confluent_transition({{0,0,{2,0,1}}, {0,0,{1,0,1}}}); });
    require(station.completed_count()==0 && station.accepted_count()==1 && *station.next_event_time()==deadline,
            "failed confluence committed state");
    auto clone=station.clone();
    const std::vector<devs::Input<des::Entity>> next{{0,0,{2,0,1}}};
    station.confluent_transition(next); clone->confluent_transition(next);
    close(*station.next_event_time(),deadline+expected(2));
    require(station.output()[0].value.completed_at==clone->output()[0].value.completed_at,"clone/redraw differs");
    rejects([&] { station.external_transition(0,{{0,0,{1ULL<<48,0,1}}}); });
    require(station.accepted_count()==2 && station.waiting()==0,"invalid ID changed state");
    // A dropped job cannot advance another entity's service stream.
    des::MultiServer<> loss(1,1,{0,des::QueueDiscipline::fifo},true,policy);
    loss.external_transition(0,{{0,0,{1,0,1}}, {0,0,{7,0,1}}});
    require(loss.output().size()==1 && loss.output()[0].port==2,"loss publication missing");
    loss.internal_transition(); // clear rejection publication
    loss.confluent_transition(next);
    close(*loss.next_event_time(),deadline+expected(2));
    for (double rate:{0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { (void)des::MultiServer<>(1,1,{},false,des::ExponentialService{rate}); });
    for (int field=0;field<3;++field) {
        auto invalid=policy;
        if (field==0) invalid.scenario=65536;
        if (field==1) invalid.replication=65536;
        if (field==2) invalid.stream=65536;
        rejects([&] { (void)des::MultiServer<>(1,1,{},false,invalid); });
    }
    for (int field=0;field<4;++field) {
        auto changed=policy;
        if (field==0) ++changed.seed;
        if (field==1) ++changed.scenario;
        if (field==2) ++changed.replication;
        if (field==3) ++changed.stream;
        require(changed.duration(1)!=policy.duration(1),"RNG context field ignored");
    }
    auto boundary=policy; boundary.stream=65535;
    require(std::isfinite(boundary.duration(0xFFFFFFFFFFFFULL)),"maximum address rejected");
    des::MultiServer<> overflow(1,1,{},false,policy);
    rejects([&] { overflow.external_transition_at(1e30,1e30,{{0,0,{1,1e30,1}}}); });
    require(overflow.accepted_count()==0,"unrepresentable sampled deadline committed state");
    // A policy validates its rate/address without drawing a nonexistent entity 0.
    auto tiny=policy; tiny.rate=std::numeric_limits<double>::denorm_min();
    des::MultiServer<> invalid_draw(1,1,{},false,tiny);
    rejects([&] { invalid_draw.external_transition(0,{{0,0,{1,0,1}}}); });
    require(invalid_draw.accepted_count()==0,"nonfinite sampled service committed state");
    // A legacy downstream stage still uses the original source duration.
    const auto upstream=station.output()[0].value;
    des::MultiServer<> legacy(1,3);
    legacy.external_transition_at(upstream.completed_at,upstream.completed_at,{{0,0,upstream}});
    close(*legacy.next_event_time(),upstream.completed_at+3*upstream.service_duration);
}

void declarative_reference(const std::string& path) {
    const auto model=ir::load_file(path);
    for (std::uint32_t replication:{0U,9U}) {
        const auto& process=*model.process;
        const auto& jobs=process.schedule;
        std::vector<std::vector<double>> starts(3), finishes(3), entries(3);
        for (std::size_t i=0;i<3;++i) {
            const auto& spec=*process.servers[i].service;
            double previous=0;
            for (std::size_t j=0;j<jobs.size();++j) {
                const double entry=i==0 ? jobs[j].arrived_at : finishes[i-1][j];
                const double start=std::max(previous,entry);
                // Plain recurrence, no DES engine/station objects.
                const auto word=rng::draw(1234567,{7,replication,jobs[j].id,0,spec.stream,0})[0];
                const double duration=-std::log((static_cast<double>(word)+.5)/4294967296.0)/spec.rate;
                previous=start+duration;
                entries[i].push_back(entry); starts[i].push_back(start); finishes[i].push_back(previous);
            }
        }
        const auto rows=ir::run(model,{},1234567,7,replication);
        const auto repeated=ir::run(model,{},1234567,7,replication);
        require(rows.size()==repeated.size(),"repeat row count differs");
        for (std::size_t index=0;index<rows.size();++index) {
            const auto& row=rows[index];
            require(row.value==repeated[index].value,"repeat trajectory differs");
            std::map<std::string,double> expected;
            double cycle=0;
            for (std::size_t i=0;i<3;++i) {
                double count=0, queue=0, busy=0;
                for (std::size_t j=0;j<jobs.size();++j) {
                    if (finishes[i][j]<=row.time) { ++count; if (i==2) cycle+=finishes[i][j]-jobs[j].arrived_at; }
                    queue+=std::max(0.0,std::min(row.time,starts[i][j])-entries[i][j]);
                    busy+=std::max(0.0,std::min(row.time,finishes[i][j])-starts[i][j]);
                }
                const auto prefix="s"+std::to_string(i)+"_";
                expected[prefix+"completed"]=count;
                expected[prefix+"queue_mean"]=row.time==0 ? 0 : queue/row.time;
                expected[prefix+"utilization"]=row.time==0 ? 0 : busy/row.time;
                if (i==2) { expected["completed"]=count; expected["total"]=count; }
            }
            expected["cycle_total"]=cycle;
            close(row.value,expected.at(row.output_id));
        }
    }
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"expected standalone and hybrid fixtures");
        addressing_and_rollback(); declarative_reference(argv[1]); declarative_reference(argv[2]);
        std::cout<<"Station services: addressing, scaling, clone/retry, loss, validation, standalone/hybrid max-plus reference passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
