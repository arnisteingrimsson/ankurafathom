#pragma once
#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/runtime/sobol_directions.hpp"
#include "ankurafathom/rng/philox.hpp"
#include "ankurafathom/rng/integer.hpp"
#include <bit>
#include <numeric>

namespace ankurafathom::runtime {
struct GridAxis { std::string name;std::vector<double> values; };
struct ParameterBounds { std::string name;double lower,upper; };

namespace detail {
inline void design_count(std::size_t count,std::uint32_t first) {
    if(!count || first>65535 || count>65536-first) throw std::invalid_argument("design scenario count/first ID exceeds 16-bit address");
}
template<class Axis> void canonical_axes(std::vector<Axis>& axes) {
    if(axes.empty() || axes.size()>32) throw std::invalid_argument("design needs 1 to 32 parameters");
    std::sort(axes.begin(),axes.end(),[](const auto& a,const auto& b) { return a.name<b.name; });
    for(std::size_t i=0;i<axes.size();++i) if(axes[i].name.empty() || (i && axes[i-1].name==axes[i].name))
        throw std::invalid_argument("empty or duplicate design parameter name");
}
inline void bounds(std::vector<ParameterBounds>& axes) {
    canonical_axes(axes);
    for(const auto& axis:axes) if(!std::isfinite(axis.lower) || !std::isfinite(axis.upper) || axis.lower>=axis.upper)
        throw std::invalid_argument("design bounds must be finite and strictly increasing");
}
inline std::uint32_t design_word(std::uint64_t seed,std::uint32_t index,std::uint32_t dimension,
                                 std::uint32_t retry,std::uint32_t purpose) {
    if(index>65535 || dimension>31 || retry>65535) throw std::out_of_range("LHS draw address exhausted");
    return rng::philox4x32_10({index,dimension,retry,purpose},
        {static_cast<std::uint32_t>(seed),static_cast<std::uint32_t>(seed>>32)})[0];
}
inline std::array<std::uint32_t,32> sobol_directions(std::size_t dimension) {
    if(dimension>=sobol_polynomials.size()) throw std::invalid_argument("Sobol dimension exceeds pinned table");
    std::array<std::uint32_t,32> values{};
    if(dimension==0) {
        for(std::size_t j=0;j<32;++j) values[j]=std::uint32_t{1}<<(31-j);
        return values;
    }
    const auto& row=sobol_polynomials[dimension];const auto degree=static_cast<std::size_t>(std::bit_width(row.polynomial)-1);
    const auto coefficients=(row.polynomial^(std::uint32_t{1}<<degree)^1U)>>1;
    for(std::size_t j=0;j<degree;++j) values[j]=row.initial[j]<<(31-j);
    for(std::size_t j=degree;j<32;++j) {
        values[j]=values[j-degree]^(values[j-degree]>>degree);
        for(std::size_t k=1;k<degree;++k) if((coefficients>>(degree-1-k))&1U) values[j]^=values[j-k];
    }
    return values;
}
inline double sobol_coordinate(std::uint32_t index,const std::array<std::uint32_t,32>& directions) {
    auto gray=index^(index>>1);std::uint32_t value=0;
    for(std::size_t j=0;gray;++j,gray>>=1) if(gray&1U) value^=directions[j];
    return static_cast<double>(value)/4294967296.;
}
} // namespace detail

inline std::vector<Scenario> expand_grid(std::vector<GridAxis> axes,std::uint32_t first_id=0) {
    detail::canonical_axes(axes);detail::design_count(1,first_id);
    std::size_t count=1;
    for(const auto& axis:axes) {
        if(axis.values.empty() || axis.values.size()>(65536-first_id)/count)
            throw std::invalid_argument("grid axis is empty or Cartesian product exceeds scenario address");
        for(const double value:axis.values) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite grid parameter value");
        count*=axis.values.size();
    }
    std::vector<Scenario> result;result.reserve(count);
    for(std::size_t i=0;i<count;++i) {
        Scenario scenario{static_cast<std::uint32_t>(first_id+i),{}};auto cursor=i;
        for(auto axis=axes.rbegin();axis!=axes.rend();++axis) {
            scenario.parameters.emplace(axis->name,axis->values[cursor%axis->values.size()]);cursor/=axis->values.size();
        }
        result.push_back(std::move(scenario));
    }
    return result;
}

inline std::vector<Scenario> expand_lhs(std::vector<ParameterBounds> axes,std::size_t count,
                                        std::uint64_t design_seed,std::uint32_t first_id=0) {
    detail::bounds(axes);detail::design_count(count,first_id);
    std::vector<Scenario> result(count);
    for(std::size_t i=0;i<count;++i) result[i].id=static_cast<std::uint32_t>(first_id+i);
    for(std::size_t dimension=0;dimension<axes.size();++dimension) {
        std::vector<std::uint32_t> permutation(count);std::iota(permutation.begin(),permutation.end(),0U);
        for(std::size_t i=count-1;i>0;--i) {
            std::uint32_t retry=0;
            const auto chosen=rng::uniform_index(i+1,[&] {
                return detail::design_word(design_seed,static_cast<std::uint32_t>(i),static_cast<std::uint32_t>(dimension),retry++,0x4c485350U);
            });
            std::swap(permutation[i],permutation[chosen]);
        }
        const auto& axis=axes[dimension];
        for(std::size_t i=0;i<count;++i) {
            const auto word=detail::design_word(design_seed,static_cast<std::uint32_t>(i),static_cast<std::uint32_t>(dimension),0,0x4c48534aU);
            const auto unit=(static_cast<double>(permutation[i])+rng::uniform_open(word))/static_cast<double>(count);
            result[i].parameters.emplace(axis.name,std::lerp(axis.lower,axis.upper,unit));
        }
    }
    return result;
}

inline std::vector<Scenario> expand_sobol(std::vector<ParameterBounds> axes,std::size_t count,std::uint32_t first_id=0) {
    detail::bounds(axes);detail::design_count(count,first_id);
    if(!std::has_single_bit(count)) throw std::invalid_argument("Sobol scenario count must be a power of two");
    std::vector<Scenario> result(count);
    for(std::size_t i=0;i<count;++i) result[i].id=static_cast<std::uint32_t>(first_id+i);
    for(std::size_t dimension=0;dimension<axes.size();++dimension) {
        const auto directions=detail::sobol_directions(dimension);const auto& axis=axes[dimension];
        for(std::size_t i=0;i<count;++i) result[i].parameters.emplace(axis.name,
            std::lerp(axis.lower,axis.upper,detail::sobol_coordinate(static_cast<std::uint32_t>(i),directions)));
    }
    return result;
}
} // namespace ankurafathom::runtime
