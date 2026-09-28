// Test adapter only: this JSON request format is not the model's data-binding IR.
#include "ankurafathom/runtime/data.hpp"
#include "ankurafathom/runtime/population_init.hpp"
#include "ankurafathom/runtime/data_inputs.hpp"
#include <nlohmann/json.hpp>
#include <bit>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>

namespace data=ankurafathom::runtime::data;
using Json=nlohmann::json;
struct BoundAgent {};
template<class V> Json encoded(const V& value) {
    return std::visit([](const auto& v)->Json {
        if constexpr(std::is_same_v<std::decay_t<decltype(v)>,double>) return Json{{"f64_bits",std::bit_cast<std::uint64_t>(v)}};
        else return v;
    },value);
}
data::Type type(const std::string& value) {
    const std::map<std::string,data::Type> types{{"bool",data::Type::boolean},{"i32",data::Type::i32},{"i64",data::Type::i64},
        {"u64",data::Type::u64},{"f64",data::Type::f64},{"string",data::Type::string}};
    return types.at(value);
}
int main(int argc,char** argv) {
    try {
        if(argc==1) { std::cout<<Json{{"available",data::available()}}<<'\n';return 0; }
        if(argc!=3) throw std::invalid_argument("usage: runtime_data_probe source request.json");
        std::ifstream input(argv[2]);Json request;input>>request;
        data::Schema schema{{},request.at("key")};
        for(const auto& item:request.at("columns")) schema.columns.push_back({item.at("name"),type(item.at("type")),
            item.value("unit",std::string{}),item.value("categories",std::vector<std::string>{})});
        data::ReadOptions options;
        if(request.contains("max_file_bytes")) options.max_file_bytes=request.at("max_file_bytes");
        if(request.contains("max_rows")) options.max_rows=request.at("max_rows");
        if(request.contains("format")) {
            const std::map<std::string,data::Format> formats{{"csv",data::Format::csv},{"parquet",data::Format::parquet},{"arrow",data::Format::arrow_ipc}};
            options.format=formats.at(request.at("format"));
        }
        const auto table=data::load_file(argv[1],std::move(schema),options);
        Json rows=Json::array();
        for(std::size_t i=0;i<table.rows().size();++i) {
            const auto& row=table.rows()[i];Json values=Json::array();
            for(const auto& value:row) std::visit([&](const auto& v) {
                if constexpr(std::is_same_v<std::decay_t<decltype(v)>,double>) values.push_back(Json{{"f64_bits",std::bit_cast<std::uint64_t>(v)}});
                else values.push_back(v);
            },value);
            rows.push_back(std::move(values));
            if(table.find_row(row[table.column_index(table.schema().key_column)])!=i)
                throw std::runtime_error("key lookup differs from canonical order");
        }
        // The loaded table owns its strings and values independently of the source.
        if(request.value("remove_after_load",false)) {
            std::filesystem::remove(argv[1]);
            for(const auto& row:table.rows()) (void)table.find_row(row[table.column_index(table.schema().key_column)]);
        }
        const auto key_type=table.schema().columns[table.column_index(table.schema().key_column)].type;
        data::Value wrong=key_type==data::Type::string?data::Value{false}:data::Value{std::string("wrong")};
        bool rejected=false;try { (void)table.find_row(wrong); } catch(const data::Error& error) { rejected=error.code=="DATA_KEY"; }
        if(!rejected) throw std::runtime_error("wrong lookup type accepted");
        rejected=false;try { (void)table.column_index("_absent_column_"); }
        catch(const data::Error& error) { rejected=error.code=="DATA_SCHEMA"; }
        if(!rejected) throw std::runtime_error("unknown column lookup accepted");
        if(key_type==data::Type::f64) for(const auto invalid:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            rejected=false;try { (void)table.find_row(invalid); } catch(const data::Error& error) { rejected=error.code=="DATA_KEY"; }
            if(!rejected) throw std::runtime_error("nonfinite key lookup accepted");
        }
        Json lookups=Json::array();
        for(const auto& key:request.value("lookups",Json::array())) {
            data::Value value;
            switch(key_type) {
                case data::Type::boolean:value=key.get<bool>();break;
                case data::Type::i32:value=key.get<std::int32_t>();break;
                case data::Type::i64:value=key.get<std::int64_t>();break;
                case data::Type::u64:value=key.get<std::uint64_t>();break;
                case data::Type::f64:value=key.get<double>();break;
                case data::Type::string:value=key.get<std::string>();break;
            }
            const auto found=table.find_row(value);lookups.push_back(found?Json(*found):Json(nullptr));
        }
        Json names=Json::array();for(const auto& column:table.schema().columns) names.push_back(column.name);
        Json result{{"file_hash",table.file_hash()},{"canonical_hash",table.canonical_hash()},
            {"columns",names},{"rows",rows},{"lookups",lookups}};
        if(request.contains("series")) {
            const auto& spec=request.at("series");
            const data::ExogenousSeries series(table,{spec.at("time"),spec.at("value"),spec.at("time_unit"),spec.at("value_unit"),
                spec.at("interpolate")=="hold"?data::Interpolation::hold:data::Interpolation::linear});
            Json values=Json::array();for(const double time:spec.at("queries")) values.push_back(encoded(data::Value{series.value_at(time)}));
            const auto& grid=spec.at("grid");const auto samples=series.sample({grid.at("start"),grid.at("step"),grid.at("count")});
            Json times=Json::array(),sample_values=Json::array();
            for(const auto time:samples.times) times.push_back(encoded(data::Value{time}));
            for(const auto value:samples.values) sample_values.push_back(encoded(data::Value{value}));
            result["series"]={{"queries",values},{"times",times},{"values",sample_values},{"file_hash",samples.file_hash},{"canonical_hash",samples.canonical_hash}};
        }
        if(request.contains("parameters")) {
            const auto& spec=request.at("parameters");std::vector<data::ParameterField> fields;
            for(const auto& field:spec.at("fields")) fields.push_back({field.at("parameter"),field.at("column"),field.at("unit")});
            const data::ParameterTable parameters(table,fields);Json receipts=Json::array();
            for(const auto& key:spec.at("keys")) {
                data::Value value;
                switch(key_type) {
                    case data::Type::boolean:value=key.get<bool>();break;
                    case data::Type::i32:value=key.get<std::int32_t>();break;
                    case data::Type::i64:value=key.get<std::int64_t>();break;
                    case data::Type::u64:value=key.get<std::uint64_t>();break;
                    case data::Type::f64:value=key.get<double>();break;
                    case data::Type::string:value=key.get<std::string>();break;
                }
                const auto receipt=parameters.values(value);Json mapped=Json::object();
                for(const auto& [name,number]:receipt.values) mapped[name]=encoded(data::Value{number});
                receipts.push_back(Json{{"key",encoded(receipt.source_key)},{"values",mapped},{"file_hash",receipt.file_hash},{"canonical_hash",receipt.canonical_hash}});
            }
            result["parameters"]=receipts;
        }
        if(request.contains("population")) {
            const auto& spec=request.at("population");
            using Store=ankurafathom::abm::PopulationStore<BoundAgent>;
            using Kind=ankurafathom::des::FieldKind;
            const std::map<std::string,Kind> kinds{{"real",Kind::real},{"integer",Kind::integer},{"boolean",Kind::boolean},{"string",Kind::string}};
            std::vector<ankurafathom::des::EntityField> target;
            for(const auto& field:spec.at("schema")) target.push_back({field.at("name"),kinds.at(field.at("kind"))});
            Store population(spec.value("store_id",1U),target,spec.value("first_id",std::uint64_t{0}));
            population.configure_network(ankurafathom::abm::CsrNetwork({}, {},true));
            std::vector<data::PopulationField> bindings;
            for(const auto& field:spec.at("fields")) bindings.push_back({field.at("field"),field.at("column"),field.value("unit",std::string{})});
            const auto receipt=data::initialize_population(population,table,bindings);
            Json agents=Json::array();
            for(const auto& entry:receipt.entries) {
                Json values=Json::object();
                for(const auto& field:target) values[field.name]=encoded(population.field(entry.agent,field.name));
                agents.push_back(Json{{"key",encoded(entry.source_key)},{"store",entry.agent.store},{"id",entry.agent.id},{"values",values}});
            }
            result["population"]={{"file_hash",receipt.file_hash},{"canonical_hash",receipt.canonical_hash},
                {"agents",agents},{"next_id",population.next_id()},{"vertices",population.network()->vertices()}};
        }
        std::cout<<result<<'\n';return 0;
    } catch(const data::Error& error) {
        std::cerr<<Json{{"code",error.code},{"pointer",error.pointer},{"message",error.what()}}<<'\n';return 1;
    } catch(const std::exception& error) {
        std::cerr<<Json{{"code","PROBE_ERROR"},{"message",error.what()}}<<'\n';return 2;
    }
}
