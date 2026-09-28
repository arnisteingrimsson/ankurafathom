#pragma once

#include "ankurafathom/abm/typed_population.hpp"
#include <cmath>

namespace ankurafathom::abm::models {

// Synchronous frozen-neighbor hazards on a fixed undirected contact graph.
// See docs/ABM_SIR.md. This is not the continuous-time SIR process.
class SIR {
public:
    struct Agent {};
    using Population=TypedPopulation<Agent>;
    using Store=Population::Store;
    enum State : std::int64_t { susceptible=0,infected=1,recovered=2 };
    struct Parameters { double infection_rate=.4,recovery_rate=.2,dt=.25; };
    struct Draws {
        std::uint64_t seed=0;
        std::uint32_t scenario=0,replication=0,infection_stream=1301,recovery_stream=1302;
    };
    struct Counts {
        std::size_t susceptible=0,infected=0,recovered=0;
        bool operator==(const Counts&)const=default;
    };
    SIR(Parameters parameters,std::vector<std::int64_t> states,CsrNetwork network,Draws draws,
        std::uint64_t first_id=0)
        :parameters_(checked(parameters)),population_(initial(states,std::move(network),first_id)),draws_(draws) {
        if(draws.infection_stream==draws.recovery_stream) throw std::invalid_argument("SIR streams overlap");
        for(auto stream:{draws.infection_stream,draws.recovery_stream})
            (void)rng::pack_counter({draws.scenario,draws.replication,first_id,0,stream,0});
        (void)probability(parameters_.recovery_rate,parameters_.dt,1);
        for(auto id:store().network()->vertices())
            (void)probability(parameters_.infection_rate,parameters_.dt,store().network()->neighbors(id).size());
    }
    const Store& store()const noexcept { return population_.store(); }
    std::uint32_t ticks()const noexcept { return ticks_; }
    double time()const noexcept { return static_cast<double>(ticks_)*parameters_.dt; }
    std::vector<std::int64_t> states()const {
        std::vector<std::int64_t> result;
        for(auto id=store().first_id();id<store().next_id();++id) result.push_back(state(store(),id));
        return result;
    }
    Counts counts()const {
        Counts result;
        for(auto value:states()) {
            if(value==susceptible) ++result.susceptible;
            else if(value==infected) ++result.infected;
            else ++result.recovered;
        }
        return result;
    }
    double infection_probability(std::uint64_t id)const {
        return infection_probability(store(),id,parameters_);
    }
    double recovery_probability()const { return probability(parameters_.recovery_rate,parameters_.dt,1); }
    void step() {
        check_step();
        std::vector<double> uniforms;
        for(auto id=store().first_id();id<store().next_id();++id) {
            const auto stream=state(store(),id)==infected ? draws_.recovery_stream : draws_.infection_stream;
            uniforms.push_back(rng::uniform_open(rng::draw(draws_.seed,
                {draws_.scenario,draws_.replication,id,ticks_,stream,0})[0]));
        }
        advance(uniforms);
    }
    // One [0,1) variate per stable ID; comparisons are strictly u < probability.
    void scripted_step(const std::vector<double>& uniforms) { check_step(); advance(uniforms); }
private:
    static Parameters checked(Parameters p) {
        if(!std::isfinite(p.infection_rate) || p.infection_rate<0 || !std::isfinite(p.recovery_rate)
           || p.recovery_rate<0 || !std::isfinite(p.dt) || p.dt<=0)
            throw std::invalid_argument("SIR requires finite nonnegative rates and positive dt");
        return p;
    }
    static double probability(double rate,double dt,std::size_t contacts) {
        const auto hazard=(rate*dt)*static_cast<double>(contacts);
        if(!std::isfinite(hazard)) throw std::overflow_error("SIR integrated hazard overflow");
        return -std::expm1(-hazard);
    }
    static std::int64_t state(const Store& s,std::uint64_t id) {
        return std::get<std::int64_t>(s.field({0,id},0));
    }
    static double infection_probability(const Store& s,std::uint64_t id,Parameters p) {
        (void)state(s,id); // Reject absent IDs, even on an empty contact set.
        std::size_t infectious=0;
        for(auto other:s.network()->neighbors(id)) if(state(s,other)==infected) ++infectious;
        return probability(p.infection_rate,p.dt,infectious);
    }
    static void validate(const Store& s,std::size_t n) {
        if(s.size()!=n || s.active_count()!=n || !s.network() || s.network()->directed())
            throw std::logic_error("SIR population/network invariant");
        for(auto id=s.first_id();id<s.next_id();++id)
            if(state(s,id)<susceptible || state(s,id)>recovered) throw std::invalid_argument("invalid SIR state");
    }
    static Population initial(const std::vector<std::int64_t>& states,CsrNetwork network,std::uint64_t first) {
        if(states.size()>1000000 || network.directed()) throw std::invalid_argument("SIR requires bounded population and undirected graph");
        Store s(0,{{"state",des::FieldKind::integer}},first);
        std::vector<Store::Record> records;
        for(auto value:states) records.push_back({value});
        s.spawn_many(records); s.configure_network(std::move(network));
        return Population(std::move(s),[n=states.size()](const Store& candidate) { validate(candidate,n); });
    }
    void check_step()const {
        if(ticks_>65535) throw std::out_of_range("SIR tick exceeds draw address");
        const auto next=static_cast<double>(ticks_+1)*parameters_.dt;
        if(!std::isfinite(next) || next<=time()) throw std::overflow_error("SIR clock cannot advance");
    }
    void advance(const std::vector<double>& uniforms) {
        if(uniforms.size()!=store().size()) throw std::invalid_argument("SIR requires one variate per agent");
        const auto old=states();
        Population candidate(store(),[old](const Store& s) {
            validate(s,old.size());
            for(auto id=s.first_id();id<s.next_id();++id) {
                const auto before=old[id-s.first_id()],after=state(s,id);
                if(after<before || after>before+1) throw std::logic_error("invalid SIR transition");
            }
        });
        candidate.add_phase([p=parameters_,uniforms](Store::Reference agent,const Store& snapshot) {
            const auto u=uniforms[agent.id-snapshot.first_id()];
            if(!std::isfinite(u) || u<0 || u>=1) throw std::invalid_argument("SIR variate outside [0,1)");
            auto value=state(snapshot,agent.id);
            if(value==susceptible && u<infection_probability(snapshot,agent.id,p)) value=infected;
            else if(value==infected && u<probability(p.recovery_rate,p.dt,1)) value=recovered;
            return Store::Record{value};
        });
        candidate.step();
        population_=std::move(candidate); ++ticks_;
    }
    Parameters parameters_;
    Population population_;
    Draws draws_;
    std::uint32_t ticks_=0;
};

} // namespace ankurafathom::abm::models
