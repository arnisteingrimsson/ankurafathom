#pragma once
#include "ankurafathom/abm/typed_population.hpp"
#include "ankurafathom/abm/spatial_snapshot.hpp"
#include <array>
#include <limits>

namespace ankurafathom::abm::models {
class Boids {
public:
    struct Agent {};
    using Population=TypedPopulation<Agent>;
    using Store=Population::Store;
    using Snapshot=SpatialSnapshot<Agent>;
    using State=std::array<double,4>; // x, y, vx, vy
    struct Parameters {
        double width=10,height=8;
        bool wrap=true;
        double dt=.125,vision=3,separation_radius=1,alignment=1,cohesion=.3,separation=.5;
        double max_acceleration=2,max_speed=2,softening=.1,bin_width=1;
    };
    struct Draws {
        std::uint64_t seed=0;
        std::uint32_t scenario=0,replication=0,position_stream=1201,velocity_stream=1202;
    };
    Boids(Parameters parameters,const std::vector<State>& agents,std::uint64_t first_id=0)
        :parameters_(checked(parameters)),population_(initial(parameters_,agents,first_id)) {}
    static Boids seeded(Parameters parameters,std::size_t count,Draws draws,std::uint64_t first_id=0) {
        const auto p=checked(parameters);
        if(count>1000000 || first_id>Store::max_entity_id || (count && count-1>Store::max_entity_id-first_id))
            throw std::invalid_argument("Boids population/address limit exceeded");
        if(draws.position_stream==draws.velocity_stream) throw std::invalid_argument("Boids streams overlap");
        for(auto stream:{draws.position_stream,draws.velocity_stream})
            (void)rng::pack_counter({draws.scenario,draws.replication,first_id,0,stream,0});
        std::vector<State> agents;
        for(std::size_t i=0;i<count;++i) {
            const auto u=[&](std::uint32_t stream,std::uint32_t axis) {
                return rng::uniform_open(rng::draw(draws.seed,{draws.scenario,draws.replication,first_id+i,0,stream,axis})[0]);
            };
            agents.push_back({std::min(p.width*u(draws.position_stream,0),std::nextafter(p.width,0.)),
                              std::min(p.height*u(draws.position_stream,1),std::nextafter(p.height,0.)),
                              (p.max_speed/2)*(2*u(draws.velocity_stream,0)-1),
                              (p.max_speed/2)*(2*u(draws.velocity_stream,1)-1)});
        }
        return Boids(p,agents,first_id);
    }
    const Store& store()const noexcept { return population_.store(); }
    std::uint64_t ticks()const noexcept { return ticks_; }
    std::vector<State> states()const {
        std::vector<State> result;
        for(auto id=store().first_id();id<store().next_id();++id) result.push_back(state(store(),{0,id}));
        return result;
    }
    void step() {
        if(ticks_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Boids tick clock exhausted");
        // One value-owned index for the common pre-tick snapshot. Build the phase
        // afresh so copies and failed/retried ticks never retain a stale index.
        Population candidate(store(),validator(parameters_,store().size()));
        Snapshot frozen(candidate.store(),binding(parameters_));
        candidate.add_phase([p=parameters_,frozen=std::move(frozen)](Store::Reference agent,const Store& s) {
            return advance(p,agent,s,frozen);
        });
        candidate.step(); population_=std::move(candidate); ++ticks_;
    }
private:
    using Vector=std::array<double,2>;
    static Parameters checked(Parameters p) {
        for(auto value:{p.width,p.height,p.dt,p.max_speed,p.softening,p.bin_width})
            if(!std::isfinite(value) || value<=0) throw std::invalid_argument("Boids positive parameter invalid");
        for(auto value:{p.vision,p.separation_radius,p.alignment,p.cohesion,p.separation,p.max_acceleration})
            if(!std::isfinite(value) || value<0) throw std::invalid_argument("Boids nonnegative parameter invalid");
        if(p.separation_radius>p.vision || !std::isfinite(2*p.width) || !std::isfinite(2*p.height))
            throw std::invalid_argument("Boids radius/box invalid");
        // Validate spatial bin-count bounds even for empty populations.
        (void)ContinuousSpace({0,0},{p.width,p.height},p.bin_width,p.wrap);
        return p;
    }
    static Snapshot::Continuous binding(const Parameters& p) { return {{0,1},{0,0},{p.width,p.height},p.bin_width,p.wrap}; }
    static State state(const Store& s,Store::Reference agent) {
        return {std::get<double>(s.field(agent,0)),std::get<double>(s.field(agent,1)),
                std::get<double>(s.field(agent,2)),std::get<double>(s.field(agent,3))};
    }
    static Population initial(const Parameters& p,const std::vector<State>& agents,std::uint64_t first) {
        if(agents.size()>1000000) throw std::invalid_argument("Boids population limit exceeded");
        Store s(0,{{"x",des::FieldKind::real},{"y",des::FieldKind::real},{"vx",des::FieldKind::real},{"vy",des::FieldKind::real}},first);
        std::vector<Store::Record> records;
        for(auto a:agents) records.push_back({a[0],a[1],a[2],a[3]});
        s.spawn_many(records);
        return Population(std::move(s),validator(p,agents.size()));
    }
    static Population::Validator validator(const Parameters& p,std::size_t n) {
        return [p,n](const Store& candidate) {
            if(candidate.size()!=n || candidate.active_count()!=n) throw std::logic_error("Boids membership changed");
            for(auto id=candidate.first_id();id<candidate.next_id();++id) {
                const auto a=state(candidate,{0,id});
                for(auto v:a) if(!std::isfinite(v)) throw std::overflow_error("nonfinite Boids state");
                if(a[0]<0 || a[0]>=p.width || a[1]<0 || a[1]>=p.height) throw std::invalid_argument("Boids coordinates must be canonical");
                const auto speed=std::hypot(a[2],a[3]);
                if(!std::isfinite(speed) || speed/p.max_speed>1+16*std::numeric_limits<double>::epsilon()) throw std::invalid_argument("Boids speed limit exceeded");
            }
            (void)Snapshot(candidate,binding(p));
        };
    }
    static double displacement(double raw,double length,bool wrap) {
        if(wrap) { if(raw>length/2) raw-=length; else if(raw< -length/2) raw+=length; }
        return raw;
    }
    static Vector cap(Vector v,double maximum) {
        const auto norm=std::hypot(v[0],v[1]);
        if(!std::isfinite(norm)) throw std::overflow_error("nonfinite Boids steering/velocity");
        if(norm>maximum) { const auto scale=maximum/norm; v[0]*=scale; v[1]*=scale; }
        return v;
    }
    static void boundary(double& position,double& velocity,double length,bool wrap) {
        if(!std::isfinite(position)) throw std::overflow_error("Boids position overflow");
        const double period=wrap ? length : 2*length;
        double r=std::fmod(position,period); if(r<0) r+=period;
        if(wrap) position=r>=length ? 0 : r;
        else if(r==0 || r>=period) { position=0; velocity=std::abs(velocity); }
        else if(r==length) { position=std::nextafter(length,0.); velocity=-std::abs(velocity); }
        else if(r<length) position=r;
        else { position=2*length-r; velocity=-velocity; }
    }
    static Store::Record advance(const Parameters& p,Store::Reference agent,const Store& s,const Snapshot& frozen) {
        const auto own=state(s,agent);
        const auto neighbors=frozen.neighbors(agent,p.vision,true,false);
        Vector velocity_sum{0,0},cohesion{0,0},separation{0,0};
        std::size_t close=0;
        for(auto id:neighbors) {
            const auto other=state(s,{s.store_id(),id});
            const Vector delta{displacement(other[0]-own[0],p.width,p.wrap),displacement(other[1]-own[1],p.height,p.wrap)};
            for(unsigned d=0;d<2;++d) { velocity_sum[d]+=other[d+2]; cohesion[d]+=delta[d]; }
            const double distance=std::hypot(delta[0],delta[1]);
            if(distance<=p.separation_radius) {
                const auto softened=std::hypot(distance,p.softening);
                if(!std::isfinite(softened)) throw std::overflow_error("Boids separation overflow");
                for(unsigned d=0;d<2;++d) separation[d]-=(delta[d]/softened)/softened;
                ++close;
            }
        }
        Vector acceleration{0,0};
        for(unsigned d=0;d<2;++d) {
            if(!neighbors.empty()) acceleration[d]=p.alignment*(velocity_sum[d]/neighbors.size()-own[d+2])+p.cohesion*cohesion[d]/neighbors.size();
            if(close) acceleration[d]+=p.separation*separation[d]/close;
        }
        acceleration=cap(acceleration,p.max_acceleration);
        auto velocity=cap({own[2]+p.dt*acceleration[0],own[3]+p.dt*acceleration[1]},p.max_speed);
        Vector position{own[0]+p.dt*velocity[0],own[1]+p.dt*velocity[1]};
        boundary(position[0],velocity[0],p.width,p.wrap); boundary(position[1],velocity[1],p.height,p.wrap);
        return {position[0],position[1],velocity[0],velocity[1]};
    }
    Parameters parameters_;
    Population population_;
    std::uint64_t ticks_=0;
};
} // namespace ankurafathom::abm::models
