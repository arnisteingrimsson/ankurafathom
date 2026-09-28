#pragma once

#include "ankurafathom/abm/typed_population.hpp"
#include <cmath>

namespace ankurafathom::abm::models {

// Jacobi Bass adoption with linear probability dt*(p+q*a).
// See docs/SEMANTICS.md and docs/HYBRID_BASS_MEAN_FIELD.md for the two limits.
class Bass {
public:
    struct Agent {};
    using Population=TypedPopulation<Agent>;
    using Store=Population::Store;
    struct Parameters { double innovation=.03,imitation=.7,dt=.125; };
    struct Draws {
        std::uint64_t seed=0;
        std::uint32_t scenario=0,replication=0,stream=1501;
    };
    Bass(Parameters parameters,std::vector<std::int64_t> states,Draws draws,std::uint64_t first_id=0)
        :parameters_(checked(parameters)),population_(initial(states,first_id)),draws_(draws) {
        (void)rng::pack_counter({draws.scenario,draws.replication,first_id,0,draws.stream,0});
        if(!states.empty()) (void)rng::pack_counter({draws.scenario,draws.replication,store().next_id()-1,0,draws.stream,0});
    }
    const Store& store()const noexcept { return population_.store(); }
    std::uint32_t ticks()const noexcept { return ticks_; }
    double time()const noexcept { return static_cast<double>(ticks_)*parameters_.dt; }
    std::vector<std::int64_t> states()const {
        std::vector<std::int64_t> result;
        for(auto id=store().first_id();id<store().next_id();++id) result.push_back(state(store(),id));
        return result;
    }
    std::size_t adopted()const {
        std::size_t count=0;
        for(auto value:states()) count+=static_cast<std::size_t>(value);
        return count;
    }
    double adoption_probability()const {
        const auto fraction=store().size() ? static_cast<double>(adopted())/static_cast<double>(store().size()) : 0.;
        return parameters_.dt*(parameters_.innovation+parameters_.imitation*fraction);
    }
    void step() {
        check_step();
        std::vector<double> uniforms;
        for(auto id=store().first_id();id<store().next_id();++id)
            uniforms.push_back(rng::uniform_open(rng::draw(draws_.seed,
                {draws_.scenario,draws_.replication,id,ticks_,draws_.stream,0})[0]));
        advance(uniforms);
    }
    // One finite [0,1) uniform per agent, including adopted agents; strict u < p.
    void scripted_step(const std::vector<double>& uniforms) { check_step(); advance(uniforms); }
private:
    static Parameters checked(Parameters p) {
        if(!std::isfinite(p.innovation) || p.innovation<0 || !std::isfinite(p.imitation) || p.imitation<0
           || !std::isfinite(p.dt) || p.dt<=0)
            throw std::invalid_argument("Bass requires nonnegative finite rates and positive finite dt");
        const auto maximum=p.dt*(p.innovation+p.imitation);
        if(!std::isfinite(maximum) || maximum>1)
            throw std::invalid_argument("Bass linear adoption probability exceeds one");
        return p;
    }
    static std::int64_t state(const Store& store,std::uint64_t id) {
        return std::get<std::int64_t>(store.field({0,id},0));
    }
    static void validate(const Store& store,std::size_t n) {
        if(store.size()!=n || store.active_count()!=n) throw std::logic_error("Bass membership changed");
        for(auto id=store.first_id();id<store.next_id();++id)
            if(state(store,id)<0 || state(store,id)>1) throw std::invalid_argument("invalid Bass adoption state");
    }
    static Population initial(const std::vector<std::int64_t>& states,std::uint64_t first) {
        if(states.size()>1000000) throw std::invalid_argument("Bass population exceeds allocation limit");
        Store store(0,{{"adopted",des::FieldKind::integer}},first);
        std::vector<Store::Record> records;
        for(auto value:states) records.push_back({value});
        store.spawn_many(records);
        return Population(std::move(store),[n=states.size()](const Store& s) { validate(s,n); });
    }
    void check_step()const {
        if(ticks_>65535) throw std::out_of_range("Bass tick exceeds draw address");
        const auto next=static_cast<double>(ticks_+1)*parameters_.dt;
        if(!std::isfinite(next) || next<=time()) throw std::overflow_error("Bass clock cannot advance");
    }
    void advance(const std::vector<double>& uniforms) {
        if(uniforms.size()!=store().size()) throw std::invalid_argument("Bass requires one variate per agent");
        for(auto u:uniforms) if(!std::isfinite(u) || u<0 || u>=1)
            throw std::invalid_argument("Bass variate outside [0,1)");
        const auto before=states();
        const auto probability=adoption_probability();
        Population candidate(store(),[before](const Store& s) {
            validate(s,before.size());
            for(auto id=s.first_id();id<s.next_id();++id)
                if(state(s,id)<before[id-s.first_id()]) throw std::logic_error("Bass adoption reversed");
        });
        candidate.add_phase([uniforms,probability](Store::Reference agent,const Store& snapshot) {
            const auto old=state(snapshot,agent.id);
            return Store::Record{std::int64_t{old || uniforms[agent.id-snapshot.first_id()]<probability}};
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
