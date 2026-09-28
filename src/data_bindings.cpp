#include "data_bindings.hpp"
#include "ankurafathom/runtime/data_inputs.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace ankurafathom::ir {
namespace {
using Json = nlohmann::json;
namespace data = runtime::data;

void object(const Json& value, std::initializer_list<const char*> keys, const std::string& at) {
    if (!value.is_object()) throw Error("IR_TYPE", at, "expected an object");
    for (const auto& [key, unused] : value.items()) {
        (void)unused;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            std::string escaped;
            for (const char c : key) escaped += c=='~' ? "~0" : c=='/' ? "~1" : std::string(1,c);
            throw Error("IR_FIELD", at+"/"+escaped, "unknown field");
        }
    }
}
const Json& field(const Json& value, const char* key, const std::string& at) {
    if (!value.contains(key)) throw Error("IR_MISSING", at+"/"+key, "required field is missing");
    return value.at(key);
}
std::string text(const Json& value, const char* key, const std::string& at, bool empty=false) {
    const auto& item=field(value,key,at);
    if (!item.is_string() || (!empty && item.get_ref<const std::string&>().empty()))
        throw Error("IR_TYPE",at+"/"+key,"expected a string");
    return item.get<std::string>();
}
const Json& array(const Json& value, const std::string& at, std::size_t limit) {
    if (!value.is_array() || value.empty() || value.size()>limit)
        throw Error("IR_TYPE",at,"expected a nonempty bounded array");
    return value;
}
bool identifier(const std::string& value) {
    const auto alpha=[](char c) { return (c>='A' && c<='Z') || (c>='a' && c<='z') || c=='_'; };
    return !value.empty() && alpha(value.front()) && std::all_of(value.begin(),value.end(),[&](char c) {
        return alpha(c) || (c>='0' && c<='9');
    });
}
data::Type type(const std::string& value, const std::string& at) {
    if (value=="bool") return data::Type::boolean;
    if (value=="i32") return data::Type::i32;
    if (value=="i64") return data::Type::i64;
    if (value=="u64") return data::Type::u64;
    if (value=="f64") return data::Type::f64;
    if (value=="string") return data::Type::string;
    throw Error("IR_TYPE",at,"unsupported column type");
}
data::Schema binding_schema(const Json& binding,const std::string& at) {
    const auto& schema_json=field(binding,"schema",at);
    object(schema_json,{"columns","key_column"},at+"/schema");
    data::Schema schema{{},text(schema_json,"key_column",at+"/schema")};
    const auto& columns=array(field(schema_json,"columns",at+"/schema"),at+"/schema/columns",1024);
    for (std::size_t c=0;c<columns.size();++c) {
        const auto cp=at+"/schema/columns/"+std::to_string(c);
        object(columns[c],{"name","type","unit","categories"},cp);
        data::Column column{text(columns[c],"name",cp),type(text(columns[c],"type",cp),cp+"/type"),
                            text(columns[c],"unit",cp,true),{}};
        if (columns[c].contains("categories")) {
            const auto& categories=columns[c].at("categories");
            if (!categories.is_array()) throw Error("IR_TYPE",cp+"/categories","expected an array");
            for (const auto& item:categories) {
                if (!item.is_string()) throw Error("IR_TYPE",cp+"/categories","expected string categories");
                column.categories.push_back(item.get<std::string>());
            }
        }
        schema.columns.push_back(std::move(column));
    }
    return schema;
}
data::Value key_value(const Json& value, data::Type type, const std::string& at) {
    switch (type) {
    case data::Type::boolean: if (value.is_boolean()) return value.get<bool>(); break;
    case data::Type::string: if (value.is_string()) return value.get<std::string>(); break;
    case data::Type::f64:
        if (value.is_number() && std::isfinite(value.get<double>())) return value.get<double>();
        break;
    case data::Type::u64:
        if (value.is_number_unsigned()) return value.get<std::uint64_t>();
        break;
    case data::Type::i32:
    case data::Type::i64:
        if (value.is_number_integer() && (!value.is_number_unsigned() ||
            value.get<std::uint64_t>()<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
            const auto n=value.get<std::int64_t>();
            if (type==data::Type::i64) return n;
            if (n>=std::numeric_limits<std::int32_t>::min() && n<=std::numeric_limits<std::int32_t>::max())
                return static_cast<std::int32_t>(n);
        }
        break;
    }
    throw Error("IR_DATA_KEY",at,"key must match the declared type and integer range exactly");
}
}

std::optional<std::size_t> resolve_model_data(const Json& bindings, const std::filesystem::path& base, Model& model) {
    array(bindings,"/data",1024);
    std::set<std::string> ids, targets;
    // Stage all bindings before changing the model's effective defaults.
    auto parameters=model.parameters;
    std::vector<ParameterDataReceipt> receipts;
    std::vector<SeriesData> series;
    std::optional<std::size_t> population_binding;
    for (std::size_t i=0;i<bindings.size();++i) {
        const auto at="/data/"+std::to_string(i);
        const auto& binding=bindings[i];
        object(binding,{"id","source","schema","use","required"},at);
        const auto id=text(binding,"id",at);
        if (!identifier(id) || !ids.insert(id).second) throw Error("IR_ID",at+"/id","invalid or duplicate binding id");
        if (binding.contains("required") && binding.at("required")!=Json(true))
            throw Error("IR_DATA",at+"/required","only required bindings are currently supported");
        const auto source=text(binding,"source",at);
        if (source.find('\0')!=std::string::npos || source.find("://")!=std::string::npos)
            throw Error("IR_DATA",at+"/source","source must be a local file path");
        const auto path=base/std::filesystem::path(source);
        auto schema=binding_schema(binding,at);
        const auto& use=field(binding,"use",at);
        if (!use.is_object()) throw Error("IR_TYPE",at+"/use","expected an object");
        const auto kind=text(use,"kind",at+"/use");
        if (kind=="population_init" && model.kind==Model::Kind::abm) {
            if (population_binding) throw Error("IR_DATA",at+"/use/kind","only one population_init binding is supported");
            population_binding=i;
            continue; // Fields and records need the population schema, parsed next.
        }
        if (kind!="parameter_table" && model.kind==Model::Kind::abm)
            throw Error("IR_DATA",at+"/use/kind","typed ABM supports parameter_table and population_init bindings");
        if (kind=="exogenous_series") {
            object(use,{"kind","time_column","value_column","interpolate","extrapolate"},at+"/use");
            if (id=="t" || id=="dt" || Expression::builtin(id) || model.parameters.contains(id))
                throw Error("IR_ID",at+"/id","series function conflicts with a reserved name or parameter");
            const auto time_column=text(use,"time_column",at+"/use");
            const auto value_column=text(use,"value_column",at+"/use");
            const auto interpolate=text(use,"interpolate",at+"/use");
            if (interpolate!="hold" && interpolate!="linear")
                throw Error("IR_DATA",at+"/use/interpolate","expected hold or linear");
            if (text(use,"extrapolate",at+"/use")!="hold")
                throw Error("IR_DATA",at+"/use/extrapolate","only endpoint holding is supported");
            if (time_column!=schema.key_column)
                throw Error("IR_DATA",at+"/use/time_column","series time must be the unique table key");
            const auto column=[&](const std::string& name,const char* key)->const data::Column& {
                const auto found=std::find_if(schema.columns.begin(),schema.columns.end(),[&](const auto& c) { return c.name==name; });
                if (found==schema.columns.end() || found->type!=data::Type::f64)
                    throw Error("IR_DATA",at+"/use/"+key,"series requires an f64 source column");
                return *found;
            };
            const auto& tc=column(time_column,"time_column");
            const auto& vc=column(value_column,"value_column");
            const auto dimension=[&](const std::string& unit,const char* key) {
                try { return Dimension::parse(unit); }
                catch (const std::invalid_argument& error) { throw Error("IR_UNIT",at+"/use/"+key,error.what()); }
            };
            const auto input_unit=dimension(tc.unit,"time_column"),output_unit=dimension(vc.unit,"value_column");
            if (!(input_unit==Dimension::parse(model.time_unit)))
                throw Error("IR_UNIT",at+"/use/time_column","series time unit differs from model time unit");
            const data::SeriesSpec spec{tc.name,vc.name,tc.unit,vc.unit,
                interpolate=="hold" ? data::Interpolation::hold : data::Interpolation::linear};
            try {
                series.push_back({id,path.string(),i,input_unit,output_unit,spec,
                    data::ExogenousSeries(data::load_file(path,schema),spec)});
            } catch (const data::Error& error) {
                auto suffix=error.pointer;
                for (std::size_t c=0;c<schema.columns.size();++c)
                    if (suffix=="/schema/"+schema.columns[c].name) {
                        suffix="/schema/columns/"+std::to_string(c);break;
                    }
                throw Error(error.code,at+suffix,error.what());
            }
            continue;
        }
        object(use,{"kind","key","parameters"},at+"/use");
        if (kind!="parameter_table")
            throw Error("IR_DATA",at+"/use/kind","unsupported data binding kind");
        const auto& mappings=array(field(use,"parameters",at+"/use"),at+"/use/parameters",1024);
        std::vector<data::ParameterField> fields;
        std::map<std::string,std::string> mapped_columns;
        for (std::size_t m=0;m<mappings.size();++m) {
            const auto mp=at+"/use/parameters/"+std::to_string(m);
            object(mappings[m],{"parameter","column"},mp);
            const auto parameter=text(mappings[m],"parameter",mp), column=text(mappings[m],"column",mp);
            if (!model.parameters.contains(parameter)) throw Error("IR_REF",mp+"/parameter","unknown model parameter");
            if (!targets.insert(parameter).second) throw Error("IR_DATA",mp+"/parameter","parameter is bound more than once");
            const auto col=std::find_if(schema.columns.begin(),schema.columns.end(),[&](const auto& c) { return c.name==column; });
            if (col==schema.columns.end() || col->type!=data::Type::f64)
                throw Error("IR_DATA",mp+"/column","source must name an f64 column");
            try {
                if (!(Dimension::parse(col->unit)==model.parameter_units.at(parameter)))
                    throw std::invalid_argument("source column unit differs from parameter unit");
            } catch (const std::invalid_argument& error) { throw Error("IR_UNIT",mp+"/column",error.what()); }
            fields.push_back({parameter,column,col->unit});
            mapped_columns.emplace(parameter,column);
        }
        const auto key_column=std::find_if(schema.columns.begin(),schema.columns.end(),[&](const auto& c) { return c.name==schema.key_column; });
        if (key_column==schema.columns.end()) throw Error("IR_REF",at+"/schema/key_column","unknown key column");
        const auto key=key_value(field(use,"key",at+"/use"),key_column->type,at+"/use/key");
        try {
            const data::ParameterTable table(data::load_file(path,schema),fields);
            const auto values=table.values(key);
            for (const auto& [name,value]:values.values) parameters.at(name)=value;
            receipts.push_back({id,path.string(),values.file_hash,values.canonical_hash,values.source_key,
                                std::move(mapped_columns),values.values});
        } catch (const data::Error& error) {
            // Loader row pointers refer to source rows; prefix by binding for attribution.
            auto suffix=error.pointer=="/key" ? "/use/key" : error.pointer;
            for (std::size_t c=0;c<schema.columns.size();++c)
                if (suffix=="/schema/"+schema.columns[c].name) {
                    suffix="/schema/columns/"+std::to_string(c);
                    break;
                }
            throw Error(error.code,at+suffix,error.what());
        }
    }
    model.parameters=std::move(parameters);
    model.parameter_data=std::move(receipts);
    model.series_data=std::move(series);
    return population_binding;
}

PopulationData resolve_population_data(const Json& binding,std::size_t index,const std::filesystem::path& base,
                                       const std::string& population,TypedAbm& spec) {
    const std::string at="/data/"+std::to_string(index);
    object(binding,{"id","source","schema","use","required"},at);
    const auto id=text(binding,"id",at);
    if(!identifier(id)) throw Error("IR_ID",at+"/id","invalid binding id");
    if(binding.contains("required") && binding.at("required")!=Json(true))
        throw Error("IR_DATA",at+"/required","only required bindings are supported");
    const auto source=text(binding,"source",at);
    if(source.find('\0')!=std::string::npos || source.find("://")!=std::string::npos)
        throw Error("IR_DATA",at+"/source","source must be a local file path");
    const auto path=base/std::filesystem::path(source);
    auto schema=binding_schema(binding,at);
    const auto& use=field(binding,"use",at);
    object(use,{"kind","population","fields"},at+"/use");
    if(text(use,"kind",at+"/use")!="population_init")
        throw Error("IR_DATA",at+"/use/kind","expected population_init");
    if(text(use,"population",at+"/use")!=population)
        throw Error("IR_REF",at+"/use/population","unknown population");
    const auto& mappings=array(field(use,"fields",at+"/use"),at+"/use/fields",1024);
    std::vector<data::PopulationField> fields;
    for(std::size_t i=0;i<mappings.size();++i) {
        const auto mp=at+"/use/fields/"+std::to_string(i);
        object(mappings[i],{"field","column"},mp);
        const auto target=text(mappings[i],"field",mp),column=text(mappings[i],"column",mp);
        if(!spec.units.contains(target)) throw Error("IR_REF",mp+"/field","unknown population field");
        const auto found=std::find_if(schema.columns.begin(),schema.columns.end(),[&](const auto& c) { return c.name==column; });
        if(found==schema.columns.end()) throw Error("IR_REF",mp+"/column","unknown source column");
        try {
            if(!(Dimension::parse(found->unit)==spec.units.at(target)))
                throw std::invalid_argument("source and population field dimensions differ");
        } catch(const std::invalid_argument& error) { throw Error("IR_UNIT",mp+"/column",error.what()); }
        fields.push_back({target,column,found->unit});
    }
    try {
        const auto table=data::load_file(path,schema);
        if(table.rows().size()>spec.agent_limit)
            throw Error("IR_DATA",at+"/source","source population exceeds agent_limit");
        AgentStore store(0,spec.fields);
        auto receipt=data::initialize_population(store,table,fields);
        std::vector<AgentStore::Record> records;
        records.reserve(receipt.entries.size());
        for(std::size_t row=0;row<receipt.entries.size();++row) {
            auto record=store.record(receipt.entries[row].agent);
            // IR expressions expose integer fields through exact binary64 values.
            // Keep the same range as inline records, even for unmapped outputs.
            for(std::size_t f=0;f<record.size();++f) if(const auto* n=std::get_if<std::int64_t>(&record[f])) {
                if(*n < -9007199254740991LL || *n > 9007199254740991LL) {
                    const auto mapping=std::find_if(fields.begin(),fields.end(),[&](const auto& item) { return item.field==spec.fields[f].name; });
                    throw data::Error("DATA_RANGE","/rows/"+std::to_string(row)+"/"+mapping->column,
                                      "population integer exceeds the IR exact numeric range");
                }
            }
            records.push_back(std::move(record));
        }
        PopulationData result{id,path.string(),population,std::move(fields),std::move(receipt)};
        spec.agents=std::move(records);
        return result;
    } catch(const data::Error& error) {
        auto suffix=error.pointer;
        if(suffix=="/fields" || suffix.starts_with("/fields/")) suffix="/use"+suffix;
        for(std::size_t c=0;c<schema.columns.size();++c)
            if(suffix=="/schema/"+schema.columns[c].name) { suffix="/schema/columns/"+std::to_string(c);break; }
        throw Error(error.code,at+suffix,error.what());
    }
}

}
