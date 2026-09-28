#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ankurafathom::abm {

// Contract: docs/SEMANTICS.md, "ABM spatial indexes and neighborhoods".

struct GridPoint {
    std::int64_t x,y;
    auto operator<=>(const GridPoint&)const=default;
};

class GridSpace {
public:
    GridSpace(std::int64_t width,std::int64_t height,bool wrap)
        :width_(width),height_(height),wrap_(wrap) {
        if(width<=0 || height<=0 || width>2147483647 || height>2147483647)
            throw std::invalid_argument("grid dimensions must be positive signed 32-bit integers");
        const auto area=static_cast<std::uint64_t>(width)*static_cast<std::uint64_t>(height);
        if(area>cells_.max_size()) throw std::length_error("grid occupancy is too large");
        cells_.resize(static_cast<std::size_t>(area));
    }
    std::size_t size()const noexcept { return positions_.size(); }
    const std::map<std::uint64_t,GridPoint>& positions()const noexcept { return positions_; }
    GridPoint position(std::uint64_t id)const { return positions_.at(id); }
    std::optional<std::uint64_t> occupant(GridPoint point)const { return cells_[index(normalize(point))]; }
    void add(std::uint64_t id,GridPoint point) {
        point=normalize(point);
        if(positions_.contains(id) || cells_[index(point)]) throw std::invalid_argument("duplicate agent or occupied grid cell");
        // Map insertion is the only allocating operation; do it before changing occupancy.
        positions_.emplace(id,point); cells_[index(point)]=id;
    }
    void remove(std::uint64_t id) {
        const auto point=positions_.at(id);
        cells_[index(point)].reset(); positions_.erase(id);
    }
    void move(std::uint64_t id,GridPoint point) { move_many({{id,point}}); }
    void move_many(const std::vector<std::pair<std::uint64_t,GridPoint>>& moves) {
        if(moves.empty()) return;
        std::set<std::uint64_t> seen;
        std::vector<std::pair<std::uint64_t,GridPoint>> targets;
        for(const auto& [id,point]:moves) {
            (void)positions_.at(id);
            if(!seen.insert(id).second) throw std::invalid_argument("duplicate grid move");
            targets.emplace_back(id,normalize(point));
        }
        GridSpace candidate(*this);
        for(const auto& [id,point]:targets) { (void)point; candidate.cells_[index(positions_.at(id))].reset(); }
        for(const auto& [id,point]:targets) {
            const auto target=index(point);
            if(candidate.cells_[target]) throw std::invalid_argument("grid movement collision");
            candidate.cells_[target]=id; candidate.positions_.at(id)=point;
        }
        *this=std::move(candidate);
    }
    std::vector<std::uint64_t> neighbors(std::uint64_t id,std::uint64_t radius,bool moore=true,bool include_self=false)const {
        const auto origin=positions_.at(id);
        const auto xs=axis(origin.x,width_,radius),ys=axis(origin.y,height_,radius);
        std::vector<std::uint64_t> result;
        for(auto x:xs) for(auto y:ys) {
            auto dx=std::abs(origin.x-x),dy=std::abs(origin.y-y);
            if(wrap_) { dx=std::min(dx,width_-dx); dy=std::min(dy,height_-dy); }
            const auto distance=static_cast<std::uint64_t>(moore ? std::max(dx,dy) : dx+dy);
            if(distance>radius) continue;
            const auto other=cells_[index({x,y})];
            if(other && (include_self || *other!=id)) result.push_back(*other);
        }
        std::sort(result.begin(),result.end());
        return result;
    }
private:
    static std::int64_t wrapped(std::int64_t coordinate,std::int64_t extent) {
        const auto remainder=coordinate%extent; return remainder<0 ? remainder+extent : remainder;
    }
    GridPoint normalize(GridPoint point)const {
        if(wrap_) return {wrapped(point.x,width_),wrapped(point.y,height_)};
        if(point.x<0 || point.x>=width_ || point.y<0 || point.y>=height_) throw std::out_of_range("grid coordinate outside bounds");
        return point;
    }
    std::size_t index(GridPoint point)const { return static_cast<std::size_t>(point.y*width_+point.x); }
    std::vector<std::int64_t> axis(std::int64_t center,std::int64_t extent,std::uint64_t radius)const {
        const auto distance=static_cast<std::int64_t>(std::min(radius,static_cast<std::uint64_t>(extent-1)));
        std::vector<std::int64_t> result;
        if(wrap_ && 2*distance+1>=extent) {
            for(std::int64_t i=0;i<extent;++i) result.push_back(i);
        } else if(wrap_) {
            for(auto i=center-distance;i<=center+distance;++i) result.push_back(wrapped(i,extent));
        } else for(auto i=std::max(std::int64_t{0},center-distance);i<=std::min(extent-1,center+distance);++i) result.push_back(i);
        return result;
    }
    std::int64_t width_,height_;
    bool wrap_;
    std::vector<std::optional<std::uint64_t>> cells_;
    std::map<std::uint64_t,GridPoint> positions_;
};

class ContinuousSpace {
public:
    using Point=std::vector<double>;
    ContinuousSpace(Point lower,Point upper,double bin_width,bool wrap)
        :lower_(std::move(lower)),upper_(std::move(upper)),bin_width_(bin_width),wrap_(wrap) {
        if(lower_.empty() || lower_.size()>3 || upper_.size()!=lower_.size() || !std::isfinite(bin_width) || bin_width<=0)
            throw std::invalid_argument("invalid continuous space dimensions or bin width");
        for(std::size_t d=0;d<lower_.size();++d) {
            const double length=upper_[d]-lower_[d];
            if(!std::isfinite(lower_[d]) || !std::isfinite(upper_[d]) || !std::isfinite(length) || length<=0)
                throw std::invalid_argument("invalid continuous space bounds");
            const double count=std::max(1.,std::ceil(length/bin_width));
            if(!std::isfinite(count) || count<1 || count>2147483647)
                throw std::invalid_argument("continuous bin count exceeds signed 32-bit range");
            lengths_.push_back(length); bins_.push_back(static_cast<std::int64_t>(count));
        }
    }
    std::size_t size()const noexcept { return positions_.size(); }
    const std::map<std::uint64_t,Point>& positions()const noexcept { return positions_; }
    const Point& position(std::uint64_t id)const { return positions_.at(id); }
    void add(std::uint64_t id,Point point) {
        point=normalize(std::move(point));
        if(positions_.contains(id)) throw std::invalid_argument("duplicate continuous-space agent");
        ContinuousSpace candidate(*this);
        candidate.positions_.emplace(id,point); candidate.buckets_[bin(point)].insert(id);
        *this=std::move(candidate);
    }
    void remove(std::uint64_t id) {
        const auto bucket=bin(positions_.at(id));
        auto found=buckets_.find(bucket);
        found->second.erase(id);
        if(found->second.empty()) buckets_.erase(found);
        positions_.erase(id);
    }
    void move(std::uint64_t id,Point point) { move_many({{id,std::move(point)}}); }
    void move_many(const std::vector<std::pair<std::uint64_t,Point>>& moves) {
        if(moves.empty()) return;
        std::set<std::uint64_t> seen;
        std::vector<std::pair<std::uint64_t,Point>> targets;
        for(const auto& [id,point]:moves) {
            (void)positions_.at(id);
            if(!seen.insert(id).second) throw std::invalid_argument("duplicate continuous-space move");
            targets.emplace_back(id,normalize(point));
        }
        ContinuousSpace candidate(*this);
        for(const auto& [id,point]:targets) {
            candidate.remove(id);
            candidate.positions_.emplace(id,point); candidate.buckets_[bin(point)].insert(id);
        }
        *this=std::move(candidate);
    }
    double distance(const Point& a,const Point& b)const {
        const auto left=normalize(a),right=normalize(b);
        double result=0;
        for(std::size_t d=0;d<left.size();++d) {
            double delta=std::abs(left[d]-right[d]);
            if(wrap_) delta=std::min(delta,lengths_[d]-delta);
            result=std::hypot(result,delta);
        }
        return result;
    }
    std::vector<std::uint64_t> neighbors(std::uint64_t id,double radius,bool include_self=false)const {
        if(!std::isfinite(radius) || radius<0) throw std::invalid_argument("invalid continuous query radius");
        const auto& origin=positions_.at(id);
        std::vector<std::vector<std::int64_t>> axes;
        for(std::size_t d=0;d<origin.size();++d) axes.push_back(axis(d,origin[d]-lower_[d],radius));
        std::vector<std::uint64_t> result;
        std::vector<std::int64_t> key(origin.size());
        std::function<void(std::size_t)> visit=[&](std::size_t d) {
            if(d<key.size()) {
                for(auto value:axes[d]) { key[d]=value; visit(d+1); }
                return;
            }
            const auto found=buckets_.find(key);
            if(found==buckets_.end()) return;
            for(auto other:found->second)
                if((include_self || other!=id) && distance(origin,positions_.at(other))<=radius) result.push_back(other);
        };
        visit(0);
        std::sort(result.begin(),result.end());
        return result;
    }
private:
    Point normalize(Point point)const {
        if(point.size()!=lower_.size()) throw std::invalid_argument("continuous coordinate dimension differs");
        for(std::size_t d=0;d<point.size();++d) {
            if(!std::isfinite(point[d])) throw std::invalid_argument("continuous coordinate must be finite");
            if(wrap_) {
                const double offset=point[d]-lower_[d];
                if(!std::isfinite(offset)) throw std::overflow_error("continuous wrapping overflow");
                double remainder=std::fmod(offset,lengths_[d]);
                if(remainder<0) remainder+=lengths_[d];
                point[d]=lower_[d]+remainder;
                if(point[d]>=upper_[d]) point[d]=lower_[d];
            } else if(point[d]<lower_[d] || point[d]>=upper_[d]) throw std::out_of_range("continuous coordinate outside bounds");
        }
        return point;
    }
    std::vector<std::int64_t> bin(const Point& point)const {
        std::vector<std::int64_t> key;
        for(std::size_t d=0;d<point.size();++d)
            key.push_back(std::min(bins_[d]-1,static_cast<std::int64_t>(std::floor((point[d]-lower_[d])/bin_width_))));
        return key;
    }
    std::vector<std::int64_t> axis(std::size_t d,double center,double radius)const {
        std::set<std::int64_t> selected;
        const double length=lengths_[d];
        const auto range=[&](double low,double high) {
            const auto begin=std::min(bins_[d]-1,static_cast<std::int64_t>(std::floor(low/bin_width_)));
            const auto end=std::min(bins_[d]-1,static_cast<std::int64_t>(std::floor(high/bin_width_)));
            for(auto i=begin;i<=end;++i) selected.insert(i);
        };
        if(wrap_ && radius>=length/2) range(0,length);
        else {
            range(radius>=center ? 0 : center-radius,radius>=length-center ? length : center+radius);
            if(wrap_ && radius>center) range(length-(radius-center),length);
            if(wrap_ && radius>=length-center) range(0,radius-(length-center));
        }
        return {selected.begin(),selected.end()};
    }
    Point lower_,upper_,lengths_;
    double bin_width_;
    bool wrap_;
    std::vector<std::int64_t> bins_;
    std::map<std::uint64_t,Point> positions_;
    std::map<std::vector<std::int64_t>,std::set<std::uint64_t>> buckets_;
};

} // namespace ankurafathom::abm
