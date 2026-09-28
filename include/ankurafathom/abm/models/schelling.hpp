#pragma once

#include "ankurafathom/abm/typed_population.hpp"
#include "ankurafathom/abm/spatial_snapshot.hpp"
#include <numeric>

namespace ankurafathom::abm::models {

// Sequential relocation workload. See docs/SEMANTICS.md.
class Schelling {
public:
    struct Agent {};
    using Population=TypedPopulation<Agent>;
    using Store=Population::Store;
    using Snapshot=SpatialSnapshot<Agent>;
    struct State {
        std::int64_t group;
        GridPoint position;
        bool operator==(const State&)const=default;
    };
    struct Parameters {
        std::int64_t width=6,height=6;
        bool wrap=true,moore=true;
        std::uint32_t numerator=1,denominator=2;
    };
    struct Draws {
        std::uint64_t seed=0;
        std::uint32_t scenario=0,replication=0,order_stream=1101,relocation_stream=1102;
    };
    Schelling(Parameters parameters,std::vector<State> agents,Draws draws,std::uint64_t first_id=0)
        :parameters_(checked(parameters)),population_(initial(parameters_,agents,first_id)),draws_(draws) {
        if(draws.order_stream==draws.relocation_stream) throw std::invalid_argument("Schelling streams overlap");
        for(auto stream:{draws.order_stream,draws.relocation_stream})
            (void)rng::pack_counter({draws.scenario,draws.replication,first_id,0,stream,0});
    }
    const Store& store()const noexcept { return population_.store(); }
    std::uint32_t sweeps()const noexcept { return sweeps_; }
    std::size_t last_moves()const noexcept { return last_moves_; }
    std::vector<State> states()const {
        std::vector<State> result;
        for(auto id=store().first_id();id<store().next_id();++id) result.push_back(state(store(),id));
        return result;
    }
    bool satisfied(std::uint64_t id)const { return satisfied(store(),id); }
    void step() {
        check_step();
        std::vector<std::uint64_t> order(store().size());
        std::iota(order.begin(),order.end(),store().first_id());
        for(auto remaining=order.size();remaining>1;--remaining) {
            const auto j=index(remaining,store().first_id()+remaining-1,draws_.order_stream);
            std::swap(order[remaining-1],order[j]);
        }
        sweep(order,[&](std::size_t,std::uint64_t agent,const std::vector<GridPoint>& empty) {
            return empty[index(empty.size(),agent,draws_.relocation_stream)];
        });
    }
    void scripted_step(const std::vector<std::uint64_t>& order,const std::vector<GridPoint>& destinations) {
        check_step();
        if(destinations.size()!=order.size()) throw std::invalid_argument("one Schelling destination per activation required");
        sweep(order,[&](std::size_t i,std::uint64_t,const auto&) { return destinations[i]; });
    }
private:
    static Parameters checked(Parameters p) {
        if(p.width<=0 || p.height<=0 || p.width>1000000 || p.height>1000000 || p.width*p.height>1000000)
            throw std::invalid_argument("Schelling grid requires 1..1000000 cells");
        if(p.denominator==0 || p.denominator>1000000 || p.numerator>p.denominator)
            throw std::invalid_argument("invalid Schelling satisfaction fraction");
        return p;
    }
    static State state(const Store& s,std::uint64_t id) {
        return {std::get<std::int64_t>(s.field({0,id},0)),
                {std::get<std::int64_t>(s.field({0,id},1)),std::get<std::int64_t>(s.field({0,id},2))}};
    }
    static Snapshot::Grid binding(Parameters p) { return {1,2,p.width,p.height,p.wrap}; }
    static Population initial(Parameters p,const std::vector<State>& agents,std::uint64_t first) {
        if(agents.size()>static_cast<std::size_t>(p.width*p.height)) throw std::invalid_argument("Schelling population exceeds grid area");
        Store s(0,{{"group",des::FieldKind::integer},{"x",des::FieldKind::integer},{"y",des::FieldKind::integer}},first);
        std::vector<Store::Record> records;
        std::vector<std::int64_t> groups;
        for(const auto& a:agents) {
            if(a.group<0 || a.group>1) throw std::invalid_argument("Schelling groups must be binary");
            records.push_back({a.group,a.position.x,a.position.y}); groups.push_back(a.group);
        }
        s.spawn_many(records);
        return Population(std::move(s),[p,groups=std::move(groups)](const Store& candidate) {
            if(candidate.size()!=groups.size() || candidate.active_count()!=groups.size()) throw std::logic_error("Schelling membership changed");
            for(auto id=candidate.first_id();id<candidate.next_id();++id) {
                const auto a=state(candidate,id);
                if(a.group!=groups[id-candidate.first_id()]) throw std::logic_error("Schelling group changed");
                if(a.position.x<0 || a.position.x>=p.width || a.position.y<0 || a.position.y>=p.height)
                    throw std::invalid_argument("Schelling positions must be canonical grid cells");
            }
            (void)Snapshot(candidate,binding(p));
        });
    }
    bool satisfied(const Store& s,std::uint64_t id)const {
        const auto own=state(s,id);
        const auto neighbors=Snapshot(s,binding(parameters_)).neighbors({0,id},1,parameters_.moore,false);
        std::uint64_t same=0;
        for(auto other:neighbors) if(state(s,other).group==own.group) ++same;
        return same*parameters_.denominator>=neighbors.size()*parameters_.numerator;
    }
    std::vector<GridPoint> vacancies(const Store& s)const {
        std::vector<bool> occupied(static_cast<std::size_t>(parameters_.width*parameters_.height),false);
        for(auto id=s.first_id();id<s.next_id();++id) {
            const auto p=state(s,id).position;
            occupied[static_cast<std::size_t>(p.y*parameters_.width+p.x)]=true;
        }
        std::vector<GridPoint> result;
        for(std::int64_t y=0;y<parameters_.height;++y) for(std::int64_t x=0;x<parameters_.width;++x)
            if(!occupied[static_cast<std::size_t>(y*parameters_.width+x)]) result.push_back({x,y});
        return result;
    }
    void check_step()const {
        if(sweeps_>65535) throw std::out_of_range("Schelling sweep exceeds draw address");
    }
    std::size_t index(std::uint64_t bound,std::uint64_t entity,std::uint32_t stream)const {
        std::uint32_t retry=0;
        return static_cast<std::size_t>(rng::uniform_index(bound,[&] {
            return rng::draw(draws_.seed,{draws_.scenario,draws_.replication,entity,sweeps_,stream,retry++})[0];
        }));
    }
    template<class Select>
    void sweep(const std::vector<std::uint64_t>& order,Select select) {
        auto sorted=order; std::sort(sorted.begin(),sorted.end());
        if(sorted.size()!=store().size()) throw std::invalid_argument("Schelling activation must include every agent");
        for(std::size_t i=0;i<sorted.size();++i)
            if(sorted[i]!=store().first_id()+i) throw std::invalid_argument("Schelling activation is not an ID permutation");
        auto candidate=population_;
        std::size_t moves=0;
        for(std::size_t i=0;i<order.size();++i) {
            const auto id=order[i];
            if(satisfied(candidate.store(),id)) continue;
            const auto empty=vacancies(candidate.store());
            if(empty.empty()) continue;
            const auto to=select(i,id,empty);
            if(std::find(empty.begin(),empty.end(),to)==empty.end()) throw std::invalid_argument("Schelling destination is not vacant");
            auto record=candidate.store().record({0,id}); record[1]=to.x; record[2]=to.y;
            candidate.update({0,id},record); ++moves;
        }
        population_=std::move(candidate); last_moves_=moves; ++sweeps_;
    }
    Parameters parameters_;
    Population population_;
    Draws draws_;
    std::uint32_t sweeps_=0;
    std::size_t last_moves_=0;
};

} // namespace ankurafathom::abm::models
