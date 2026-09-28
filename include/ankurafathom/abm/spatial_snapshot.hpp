#pragma once
#include "ankurafathom/abm/space.hpp"
#include "ankurafathom/des/runtime_entity_store.hpp"

namespace ankurafathom::abm {
// Value-owned index derived from one immutable live population snapshot.
template<class Tag>
class SpatialSnapshot {
public:
    using Store=des::RuntimeEntityStore<Tag>;
    using Reference=typename Store::Reference;
    struct Grid { std::size_t x,y; std::int64_t width,height; bool wrap=false; };
    struct Continuous { std::vector<std::size_t> fields; std::vector<double> lower,upper; double bin_width; bool wrap=false; };
    using Binding=std::variant<Grid,Continuous>;
    SpatialSnapshot(const Store& store,const Binding& binding)
        :store_id_(store.store_id()),index_(build(store,binding)) {}
    std::vector<std::uint64_t> neighbors(Reference agent,double radius,bool moore=true,bool include_self=false)const {
        if(agent.store!=store_id_) throw std::invalid_argument("foreign spatial agent");
        if(!std::isfinite(radius) || radius<0) throw std::invalid_argument("invalid spatial radius");
        if(const auto* grid=std::get_if<GridSpace>(&index_)) {
            if(radius>9007199254740991. || std::floor(radius)!=radius) throw std::invalid_argument("invalid integer grid radius");
            return grid->neighbors(agent.id,static_cast<std::uint64_t>(radius),moore,include_self);
        }
        return std::get<ContinuousSpace>(index_).neighbors(agent.id,radius,include_self);
    }
private:
    using Index=std::variant<GridSpace,ContinuousSpace>;
    static Index build(const Store& store,const Binding& binding) {
        if(const auto* b=std::get_if<Grid>(&binding)) {
            if(b->x==b->y || store.schema().at(b->x).kind!=des::FieldKind::integer || store.schema().at(b->y).kind!=des::FieldKind::integer)
                throw std::invalid_argument("grid needs distinct integer coordinate fields");
            GridSpace result(b->width,b->height,b->wrap);
            for(auto id=store.first_id();id<store.next_id();++id) if(store.alive({store.store_id(),id}))
                result.add(id,{std::get<std::int64_t>(store.field({store.store_id(),id},b->x)),std::get<std::int64_t>(store.field({store.store_id(),id},b->y))});
            return result;
        }
        const auto& b=std::get<Continuous>(binding);
        if(b.fields.size()!=b.lower.size() || b.fields.size()!=b.upper.size()) throw std::invalid_argument("coordinate dimension differs");
        std::set<std::size_t> seen;
        for(auto field:b.fields) {
            const auto kind=store.schema().at(field).kind;
            if(!seen.insert(field).second || (kind!=des::FieldKind::real && kind!=des::FieldKind::integer))
                throw std::invalid_argument("continuous space needs distinct numeric coordinate fields");
        }
        ContinuousSpace result(b.lower,b.upper,b.bin_width,b.wrap);
        for(auto id=store.first_id();id<store.next_id();++id) if(store.alive({store.store_id(),id})) {
            ContinuousSpace::Point point;
            for(auto field:b.fields) {
                const auto value=store.field({store.store_id(),id},field);
                if(const auto* n=std::get_if<std::int64_t>(&value)) {
                    if(*n < -9007199254740991LL || *n > 9007199254740991LL) throw std::overflow_error("coordinate exceeds exact numeric range");
                    point.push_back(static_cast<double>(*n));
                } else point.push_back(std::get<double>(value));
            }
            result.add(id,std::move(point));
        }
        return result;
    }
    std::uint32_t store_id_;
    Index index_;
};
} // namespace ankurafathom::abm
