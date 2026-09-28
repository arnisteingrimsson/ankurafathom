#pragma once

#include "ankurafathom/abm/population_store.hpp"
#include "ankurafathom/abm/space.hpp"
#include "ankurafathom/rng/integer.hpp"
#include "ankurafathom/rng/philox.hpp"

namespace ankurafathom::abm::models {

// Declared sequential, single-resource variant. See docs/ABM_SUGARSCAPE.md.
class Sugarscape {
public:
    struct Agent {};
    using Store=PopulationStore<Agent>;
    struct State {
        std::int64_t reserve,metabolism,vision;
        GridPoint position;
        bool operator==(const State&)const=default;
    };
    struct Parameters {
        std::int64_t width=6,height=6;
        bool wrap=true;
        std::int64_t regrowth=1;
    };
    struct Draws {
        std::uint64_t seed=0;
        std::uint32_t scenario=0,replication=0,order_stream=1201,movement_stream=1202;
    };
    struct Ledger {
        std::int64_t initial=0,regrown=0,consumed=0;
        std::size_t deaths=0;
        bool operator==(const Ledger&)const=default;
    };
    Sugarscape(Parameters parameters,std::vector<std::int64_t> capacity,
               std::vector<std::int64_t> sugar,std::vector<State> agents,Draws draws,
               std::uint64_t first_id=0)
        :parameters_(checked(parameters)),capacity_(std::move(capacity)),sugar_(std::move(sugar)),
         store_(0,{{"reserve",des::FieldKind::integer},{"metabolism",des::FieldKind::integer},
                   {"vision",des::FieldKind::integer},{"x",des::FieldKind::integer},{"y",des::FieldKind::integer}},first_id),
         draws_(draws) {
        if(draws.order_stream==draws.movement_stream) throw std::invalid_argument("Sugarscape streams overlap");
        for(auto stream:{draws.order_stream,draws.movement_stream})
            (void)rng::pack_counter({draws.scenario,draws.replication,first_id,0,stream,0});
        const auto area=static_cast<std::size_t>(parameters_.width*parameters_.height);
        if(capacity_.size()!=area || sugar_.size()!=area || agents.size()>area)
            throw std::invalid_argument("Sugarscape landscape/population size");
        for(std::size_t i=0;i<area;++i) {
            if(capacity_[i]<0 || sugar_[i]<0 || sugar_[i]>capacity_[i])
                throw std::invalid_argument("Sugarscape sugar outside capacity");
            ledger_.initial=add(ledger_.initial,sugar_[i]);
        }
        std::vector<Store::Record> records;
        GridSpace grid(parameters_.width,parameters_.height,parameters_.wrap);
        for(const auto& a:agents) {
            if(a.reserve<=0 || a.metabolism<=0 || a.vision<0 || a.vision>1000000)
                throw std::invalid_argument("Sugarscape requires positive reserve/metabolism and bounded nonnegative vision");
            canonical(a.position);
            grid.add(records.size(),a.position);
            records.push_back({a.reserve,a.metabolism,a.vision,a.position.x,a.position.y});
            ledger_.initial=add(ledger_.initial,a.reserve);
        }
        store_.spawn_many(records);
        validate();
    }
    const Store& store()const noexcept { return store_; }
    const auto& sugar()const noexcept { return sugar_; }
    const auto& capacity()const noexcept { return capacity_; }
    const Ledger& ledger()const noexcept { return ledger_; }
    std::uint32_t sweeps()const noexcept { return sweeps_; }
    State state(std::uint64_t id)const {
        const auto value=[&](std::size_t field) { return std::get<std::int64_t>(store_.field({0,id},field)); };
        return {value(0),value(1),value(2),{value(3),value(4)}};
    }
    std::vector<std::uint64_t> active_ids()const {
        std::vector<std::uint64_t> result;
        for(auto id=store_.first_id();id<store_.next_id();++id) if(store_.alive({0,id})) result.push_back(id);
        return result;
    }
    void step() {
        check_step();
        auto order=active_ids();
        for(auto remaining=order.size();remaining>1;--remaining) {
            // Rank address, independent of holes left by retired IDs.
            const auto j=index(remaining,store_.first_id()+remaining-1,draws_.order_stream);
            std::swap(order[remaining-1],order[j]);
        }
        sweep(order,[&](std::size_t,std::uint64_t id,const std::vector<GridPoint>& choices) {
            return choices[index(choices.size(),id,draws_.movement_stream)];
        });
    }
    void scripted_step(const std::vector<std::uint64_t>& order,const std::vector<GridPoint>& destinations) {
        check_step();
        if(order.size()!=destinations.size()) throw std::invalid_argument("one Sugarscape destination per activation required");
        sweep(order,[&](std::size_t i,std::uint64_t,const auto&) { return destinations[i]; });
    }
private:
    static Parameters checked(Parameters p) {
        if(p.width<=0 || p.height<=0 || p.width>1000000 || p.height>1000000 || p.width*p.height>1000000 || p.regrowth<0)
            throw std::invalid_argument("Sugarscape requires 1..1000000 cells and nonnegative regrowth");
        return p;
    }
    static std::int64_t add(std::int64_t a,std::int64_t b) {
        if(b<0 || a<0 || a>std::numeric_limits<std::int64_t>::max()-b)
            throw std::overflow_error("Sugarscape resource ledger overflow");
        return a+b;
    }
    void canonical(GridPoint p)const {
        if(p.x<0 || p.x>=parameters_.width || p.y<0 || p.y>=parameters_.height)
            throw std::invalid_argument("Sugarscape position is not canonical");
    }
    std::size_t cell(GridPoint p)const { return static_cast<std::size_t>(p.y*parameters_.width+p.x); }
    std::vector<GridPoint> choices(std::uint64_t id,const GridSpace& grid)const {
        const auto a=state(id);
        std::vector<GridPoint> result;
        std::int64_t best=-1,distance=std::numeric_limits<std::int64_t>::max();
        // Enumerating canonical cells deduplicates short periodic axes and yields
        // the declared row-major tie order, without iterating an unbounded vision.
        for(std::int64_t y=0;y<parameters_.height;++y) for(std::int64_t x=0;x<parameters_.width;++x) {
            if(x!=a.position.x && y!=a.position.y) continue;
            auto dx=std::abs(x-a.position.x),dy=std::abs(y-a.position.y);
            if(parameters_.wrap) { dx=std::min(dx,parameters_.width-dx); dy=std::min(dy,parameters_.height-dy); }
            const auto d=dx+dy;
            if(d>a.vision) continue;
            const GridPoint p{x,y};
            const auto occupant=grid.occupant(p);
            if(occupant && *occupant!=id) continue;
            const auto amount=sugar_[cell(p)];
            if(amount>best || (amount==best && d<distance)) { result.clear(); best=amount; distance=d; }
            if(amount==best && d==distance) result.push_back(p);
        }
        return result; // Own cell is always eligible.
    }
    void validate()const {
        auto total=ledger_.consumed;
        for(auto amount:sugar_) total=add(total,amount);
        GridSpace grid(parameters_.width,parameters_.height,parameters_.wrap);
        for(auto id:active_ids()) {
            const auto a=state(id);
            if(a.reserve<=0) throw std::logic_error("Sugarscape live reserve is not positive");
            canonical(a.position); grid.add(id,a.position); total=add(total,a.reserve);
        }
        if(total!=add(ledger_.initial,ledger_.regrown) || store_.active_count()+ledger_.deaths!=store_.size())
            throw std::logic_error("Sugarscape conservation failed");
    }
    void check_step()const {
        if(sweeps_>65535) throw std::out_of_range("Sugarscape sweep exceeds draw address");
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
        if(sorted!=active_ids()) throw std::invalid_argument("Sugarscape activation is not a live ID permutation");
        auto candidate=*this;
        GridSpace grid(parameters_.width,parameters_.height,parameters_.wrap);
        for(auto id:order) grid.add(id,state(id).position);
        for(std::size_t i=0;i<order.size();++i) {
            const auto id=order[i];
            const auto options=candidate.choices(id,grid);
            const auto to=select(i,id,options);
            if(std::find(options.begin(),options.end(),to)==options.end())
                throw std::invalid_argument("Sugarscape destination does not maximize sugar then minimize distance");
            auto a=candidate.state(id);
            grid.move(id,to); a.position=to;
            a.reserve=add(a.reserve,candidate.sugar_[cell(to)]);
            candidate.sugar_[cell(to)]=0;
            const auto consumed=std::min(a.reserve,a.metabolism);
            a.reserve-=consumed;
            candidate.ledger_.consumed=add(candidate.ledger_.consumed,consumed);
            candidate.store_.update({0,id},{a.reserve,a.metabolism,a.vision,to.x,to.y});
            if(a.reserve==0) { candidate.store_.retire({0,id}); grid.remove(id); ++candidate.ledger_.deaths; }
        }
        for(std::size_t i=0;i<sugar_.size();++i) {
            const auto grown=std::min(parameters_.regrowth,capacity_[i]-candidate.sugar_[i]);
            candidate.sugar_[i]+=grown;
            candidate.ledger_.regrown=add(candidate.ledger_.regrown,grown);
        }
        candidate.validate();
        ++candidate.sweeps_;
        *this=std::move(candidate);
    }
    Parameters parameters_;
    std::vector<std::int64_t> capacity_,sugar_;
    Store store_;
    Draws draws_;
    Ledger ledger_;
    std::uint32_t sweeps_=0;
};

} // namespace ankurafathom::abm::models
