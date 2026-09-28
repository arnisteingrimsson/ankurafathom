#pragma once
#include "ankurafathom/runtime/data.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <span>
#include <utility>

namespace ankurafathom::runtime::data {
namespace inputs_detail {
inline std::size_t numeric_column(const Table& table,const std::string& name,
                                  const std::string& unit,const std::string& pointer) {
    std::size_t index;
    try { index=table.column_index(name); }
    catch(const Error&) { throw Error("DATA_MAPPING",pointer,"unknown source column"); }
    const auto& column=table.schema().columns[index];
    if(column.type!=Type::f64) throw Error("DATA_MAPPING",pointer,"numeric model inputs require an f64 source column");
    if(column.unit!=unit) throw Error("DATA_UNIT",pointer,"source and expected unit declarations differ");
    return index;
}
} // namespace inputs_detail

struct ParameterField { std::string parameter,column,unit; };
struct ParameterValues {
    std::map<std::string,double> values;
    Value source_key;
    std::string file_hash,canonical_hash;
};
class ParameterTable {
public:
    ParameterTable(Table table,std::span<const ParameterField> fields):table_(std::move(table)) {
        if(fields.empty()) throw Error("DATA_MAPPING","/parameters","parameter mappings must be nonempty");
        for(std::size_t i=0;i<fields.size();++i) {
            const auto& field=fields[i];const auto pointer="/parameters/"+std::to_string(i);
            if(field.parameter.empty() || bindings_.contains(field.parameter))
                throw Error("DATA_MAPPING",pointer,"parameter names must be nonempty and unique");
            bindings_.emplace(field.parameter,inputs_detail::numeric_column(table_,field.column,field.unit,pointer+"/column"));
        }
    }
    ParameterValues values(const Value& key) const {
        const auto index=table_.find_row(key);
        if(!index) throw Error("DATA_KEY","/key","parameter table has no matching key");
        const auto& row=table_.rows()[*index];
        ParameterValues result{{},row[table_.column_index(table_.schema().key_column)],table_.file_hash(),table_.canonical_hash()};
        for(const auto& [name,column]:bindings_) result.values.emplace(name,std::get<double>(row[column]));
        return result;
    }
private:
    Table table_;
    std::map<std::string,std::size_t> bindings_;
};

enum class Interpolation { hold,linear };
struct SeriesSpec {
    std::string time_column,value_column,time_unit,value_unit;
    Interpolation interpolation=Interpolation::hold;
};
struct SampleGrid { double start,step;std::size_t count; };
struct SampledSeries {
    std::vector<double> times,values;
    std::string file_hash,canonical_hash;
};
class ExogenousSeries {
public:
    const std::string& file_hash() const noexcept { return file_hash_; }
    const std::string& canonical_hash() const noexcept { return canonical_hash_; }
    ExogenousSeries(const Table& table,const SeriesSpec& spec)
        :interpolation_(spec.interpolation),file_hash_(table.file_hash()),canonical_hash_(table.canonical_hash()) {
        if(interpolation_!=Interpolation::hold && interpolation_!=Interpolation::linear)
            throw Error("DATA_SERIES","/interpolation","unsupported interpolation policy");
        if(spec.time_column!=table.schema().key_column)
            throw Error("DATA_SERIES","/time_column","series time must be the unique table key");
        const auto time=inputs_detail::numeric_column(table,spec.time_column,spec.time_unit,"/time_column");
        const auto value=inputs_detail::numeric_column(table,spec.value_column,spec.value_unit,"/value_column");
        if(table.rows().empty()) throw Error("DATA_SERIES","/source","a series needs at least one sample");
        times_.reserve(table.rows().size());values_.reserve(table.rows().size());
        for(const auto& row:table.rows()) { times_.push_back(std::get<double>(row[time]));values_.push_back(std::get<double>(row[value])); }
    }
    double value_at(double time) const {
        if(!std::isfinite(time)) throw Error("DATA_SERIES","/time","series query time must be finite");
        const auto next=std::lower_bound(times_.begin(),times_.end(),time);
        if(next==times_.end()) return values_.back();
        const auto right=static_cast<std::size_t>(next-times_.begin());
        if(right==0 || *next==time) return values_[right];
        const auto left=right-1;
        if(interpolation_==Interpolation::hold) return values_[left];
        const auto width=times_[right]-times_[left];
        // A finite interval can overflow when its endpoints straddle zero.
        // Scaling all three operands by 1/2 retains a finite denominator.
        const auto alpha=std::isfinite(width)?(time-times_[left])/width:
            (time*.5-times_[left]*.5)/(times_[right]*.5-times_[left]*.5);
        if(!std::isfinite(alpha) || alpha<0 || alpha>1)
            throw Error("DATA_SERIES","/time","interpolation fraction is outside [0,1]");
        const auto value=std::lerp(values_[left],values_[right],alpha);
        if(!std::isfinite(value)) throw Error("DATA_SERIES","/value","interpolation produced a nonfinite value");
        return value;
    }
    SampledSeries sample(const SampleGrid& grid) const {
        if(!std::isfinite(grid.start) || grid.start<0 || !std::isfinite(grid.step) || grid.step<=0 || grid.count==0 || grid.count>1000000)
            throw Error("DATA_GRID","/grid","grid needs finite nonnegative start, positive step and 1–1000000 samples");
        SampledSeries result{{},{},file_hash_,canonical_hash_};result.times.reserve(grid.count);result.values.reserve(grid.count);
        for(std::size_t i=0;i<grid.count;++i) {
            const auto time=grid.start+static_cast<double>(i)*grid.step;
            if(!std::isfinite(time) || (!result.times.empty() && time<=result.times.back()))
                throw Error("DATA_GRID","/grid","sample clock overflows or fails to advance");
            result.times.push_back(time);result.values.push_back(value_at(time));
        }
        return result;
    }
private:
    Interpolation interpolation_;
    std::string file_hash_,canonical_hash_;
    std::vector<double> times_,values_;
};
} // namespace ankurafathom::runtime::data
