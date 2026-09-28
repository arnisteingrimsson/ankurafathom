#include "ankurafathom/runtime/outputs.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include "ankurafathom/runtime/observation_identity.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>

#if FATHOM_HAS_ARROW
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/arrow/reader.h>
#endif

namespace ankurafathom::runtime {
namespace {
struct Lineage {
    std::string id,manifest;
    std::map<std::uint32_t,std::string> parameters;
    std::uint64_t replications,rows;
};
Lineage parse_lineage(const OutputProvenance& provenance) {
    using Json=nlohmann::json;
    auto fail=[] { throw std::invalid_argument("invalid result provenance or scenario parameters"); };
    auto manifest=Json::parse(provenance.manifest_json);
    if((manifest.at("manifest_version")!="0.2" && manifest.at("manifest_version")!="0.3") ||
       manifest.at("output").at("schema_version")!="0.2") fail();
    Lineage lineage;
    lineage.id=manifest.at("id").get<std::string>();
    manifest.erase("id");
    if(sha256(manifest.dump())!=lineage.id) fail();
    manifest["id"]=lineage.id;
    lineage.manifest=manifest.dump();
    const auto& result=manifest.at("result");
    if(!result.at("rows").is_number_unsigned() || !result.at("trajectories").is_number_unsigned()) fail();
    lineage.rows=result.at("rows").get<std::uint64_t>();
    auto integer=[&](const Json& value,std::uint64_t minimum,std::uint64_t maximum) {
        if(!value.is_number_unsigned() || value<minimum || value>maximum) fail();
        return value.get<std::uint64_t>();
    };
    const auto& input=manifest.at("inputs");
    lineage.replications=integer(input.at("replications"),1,65536);
    const auto& scenarios=input.at("scenarios");
    if(!scenarios.is_array() || scenarios.empty() || scenarios.size()>65536 ||
       result.at("trajectories")!=scenarios.size()*lineage.replications) fail();
    std::optional<std::uint32_t> previous;
    auto parameters=[&](const Json& values) {
        if(!values.is_object()) fail();
        for(const auto& [name,value]:values.items())
            if(name.empty() || !value.is_number() || !std::isfinite(value.get<double>())) fail();
    };
    for(const auto& scenario:scenarios) {
        const auto id=static_cast<std::uint32_t>(integer(scenario.at("id"),0,65535));
        if(previous && id<=*previous) fail();
        previous=id;
        const auto& effective=scenario.at("effective_parameters");
        const auto& overrides=scenario.at("overrides");
        parameters(effective);parameters(overrides);
        for(const auto& [name,value]:overrides.items())
            if(!effective.contains(name) || std::bit_cast<std::uint64_t>(value.get<double>())!=
                std::bit_cast<std::uint64_t>(effective.at(name).get<double>())) fail();
        lineage.parameters.emplace(id,effective.dump());
    }
    return lineage;
}
Lineage validate_lineage(std::span<const Trajectory> trajectories,const OutputProvenance& provenance) {
    using Json=nlohmann::json;
    auto lineage=parse_lineage(provenance);
    const auto identity=observation_identity(trajectories);
    if(Json::parse(lineage.manifest).at("result")!=Json{{"encoding","ordered-ieee754-le-v1"},{"sha256",identity.sha256},
        {"trajectories",identity.trajectories},{"rows",identity.rows}} ||
       trajectories.size()!=lineage.parameters.size()*lineage.replications)
        throw std::invalid_argument("result provenance does not match trajectories");
    std::size_t index=0;
    for(const auto& [id,parameters]:lineage.parameters) {
        (void)parameters;
        for(std::uint32_t replication=0;replication<lineage.replications;++replication) {
            const auto& trajectory=trajectories[index++];
            if(trajectory.scenario!=id || trajectory.replication!=replication)
                throw std::invalid_argument("result addresses do not match provenance");
        }
    }
    return lineage;
}
void validate_rows(std::span<const Trajectory> trajectories) {
    std::optional<std::pair<std::uint32_t,std::uint32_t>> previous;
    for(const auto& trajectory:trajectories) {
        const auto address=std::pair{trajectory.scenario,trajectory.replication};
        if(trajectory.scenario>65535 || trajectory.replication>65535 || (previous && address<=*previous))
            throw std::invalid_argument("result trajectory addresses must increase strictly and fit in 16 bits");
        previous=address;
        validate_observations(trajectory.observations);
    }
}
void validate_single(std::span<const Trajectory> trajectories,bool single_run) {
    if(single_run && (trajectories.size()!=1 || trajectories[0].scenario!=0 || trajectories[0].replication!=0))
        throw std::invalid_argument("single-run output requires one trajectory at address (0,0)");
}
void csv_string(std::ostream& out,const std::string& text) {
    if(text.find_first_of(",\"\r\n")==std::string::npos) { out<<text;return; }
    out<<'"';
    for(const char c:text) { if(c=='"') out<<'"';out<<c; }
    out<<'"';
}
bool csv_record(std::istream& input,std::vector<std::string>& fields) {
    fields.clear();
    if(input.peek()==std::char_traits<char>::eof()) {
        if(input.bad()) throw std::runtime_error("cannot read CSV result");
        return false;
    }
    enum class State { start,plain,quoted,closed };
    State state=State::start;std::string field;char c;
    const auto finish=[&] {
        fields.push_back(std::move(field));field.clear();state=State::start;
        if(fields.size()>7) throw std::invalid_argument("too many CSV result columns");
    };
    while(input.get(c)) {
        if(state==State::quoted) {
            if(c=='"') {
                if(input.peek()=='"') { input.get();field+='"'; }
                else state=State::closed;
            } else field+=c;
        } else if(c==',') finish();
        else if(c=='\n' || c=='\r') {
            if(c=='\r' && input.get()!='\n') throw std::invalid_argument("CSV result requires LF or CRLF records");
            finish();return true;
        } else if(c=='"') {
            if(state!=State::start) throw std::invalid_argument("unexpected quote in CSV result");
            state=State::quoted;
        } else {
            if(state==State::closed) throw std::invalid_argument("characters after closing CSV quote");
            field+=c;state=State::plain;
        }
    }
    if(input.bad()) throw std::runtime_error("cannot read CSV result");
    if(state==State::quoted) throw std::invalid_argument("unterminated CSV result field");
    finish();return true;
}
template<class T> T csv_number(const std::string& text) {
    T value{};
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if(text.empty() || error!=std::errc{} || end!=text.data()+text.size())
        throw std::invalid_argument("invalid CSV result number");
    return value;
}
#if FATHOM_HAS_ARROW
void check(const arrow::Status& status) {
    if(!status.ok()) throw std::runtime_error(status.ToString());
}
template<class T> T unwrap(arrow::Result<T> result) {
    check(result.status());return std::move(result).ValueUnsafe();
}
std::shared_ptr<arrow::Table> read_result_table(const std::filesystem::path& path,OutputFormat format) {
    auto file=unwrap(arrow::io::ReadableFile::Open(path.string()));
    if(format==OutputFormat::parquet) {
        auto reader=unwrap(parquet::arrow::OpenFile(file,arrow::default_memory_pool()));
        return unwrap(reader->ReadTable());
    }
    auto reader=unwrap(arrow::ipc::RecordBatchFileReader::Open(file));return unwrap(reader->ToTable());
}
std::map<std::string,std::string> result_metadata(const arrow::Table& table) {
    const auto metadata=table.schema()->metadata();
    if(!metadata) throw std::invalid_argument("result metadata is missing");
    std::map<std::string,std::string> values;
    for(std::int64_t i=0;i<metadata->size();++i)
        if(!values.emplace(metadata->key(i),metadata->value(i)).second)
            throw std::invalid_argument("duplicate result metadata key");
    return values;
}
#endif
} // namespace

OutputFormat parse_output_format(std::string_view name) {
    if(name=="csv") return OutputFormat::csv;
    if(name=="parquet") return OutputFormat::parquet;
    if(name=="arrow") return OutputFormat::arrow_ipc;
    throw std::invalid_argument("output format must be csv, parquet or arrow");
}
OutputFormat infer_output_format(const std::filesystem::path& path) {
    const auto extension=path.extension().string();
    if(extension==".parquet") return OutputFormat::parquet;
    if(extension==".arrow" || extension==".ipc") return OutputFormat::arrow_ipc;
    return OutputFormat::csv;
}
bool arrow_output_available() noexcept { return FATHOM_HAS_ARROW; }

std::string observations_csv(std::span<const Trajectory> trajectories,bool single_run,const OutputProvenance* provenance) {
    validate_rows(trajectories);validate_single(trajectories,single_run);
    const auto lineage=provenance?std::optional{validate_lineage(trajectories,*provenance)}:std::nullopt;
    std::ostringstream out;out.exceptions(std::ios::badbit|std::ios::failbit);
    out.imbue(std::locale::classic());out<<std::setprecision(17);
    if(!single_run || lineage) out<<"scenario,replication,";
    out<<"time,output_id,value";
    if(lineage) out<<",manifest_id,scenario_parameters";
    out<<'\n';
    for(const auto& trajectory:trajectories) for(const auto& row:trajectory.observations) {
        if(!single_run || lineage) out<<trajectory.scenario<<','<<trajectory.replication<<',';
        out<<row.time<<',';csv_string(out,row.output_id);out<<','<<row.value;
        if(lineage) { out<<','<<lineage->id<<',';csv_string(out,lineage->parameters.at(trajectory.scenario)); }
        out<<'\n';
    }
    return out.str();
}

std::shared_ptr<arrow::Table> observation_table(std::span<const Trajectory> trajectories,const OutputProvenance* provenance) {
#if FATHOM_HAS_ARROW
    validate_rows(trajectories);
    const auto lineage=provenance?std::optional{validate_lineage(trajectories,*provenance)}:std::nullopt;
    arrow::UInt32Builder scenario,replication;
    arrow::DoubleBuilder time,value;
    arrow::StringBuilder output_id,manifest_id,scenario_parameters;
    for(const auto& trajectory:trajectories) for(const auto& row:trajectory.observations) {
        check(scenario.Append(trajectory.scenario));check(replication.Append(trajectory.replication));
        check(time.Append(row.time));check(output_id.Append(row.output_id));check(value.Append(row.value));
        if(lineage) {
            check(manifest_id.Append(lineage->id));
            check(scenario_parameters.Append(lineage->parameters.at(trajectory.scenario)));
        }
    }
    auto schema=arrow::schema({arrow::field("scenario",arrow::uint32(),false),
        arrow::field("replication",arrow::uint32(),false),arrow::field("time",arrow::float64(),false),
        arrow::field("output_id",arrow::utf8(),false),arrow::field("value",arrow::float64(),false)},
        arrow::key_value_metadata({"ankurafathom.schema_version","ankurafathom.table"},{"0.1","observations"}));
    std::vector<std::shared_ptr<arrow::Array>> columns{unwrap(scenario.Finish()),unwrap(replication.Finish()),
        unwrap(time.Finish()),unwrap(output_id.Finish()),unwrap(value.Finish())};
    if(lineage) {
        schema=unwrap(schema->AddField(schema->num_fields(),arrow::field("manifest_id",arrow::utf8(),false)));
        schema=unwrap(schema->AddField(schema->num_fields(),arrow::field("scenario_parameters",arrow::utf8(),false)));
        schema=schema->WithMetadata(arrow::key_value_metadata(
            {"ankurafathom.schema_version","ankurafathom.table","ankurafathom.manifest_id","ankurafathom.manifest"},
            {"0.2","observations",lineage->id,lineage->manifest}));
        columns.push_back(unwrap(manifest_id.Finish()));columns.push_back(unwrap(scenario_parameters.Finish()));
    }
    auto table=arrow::Table::Make(std::move(schema),std::move(columns));
    check(table->ValidateFull());return table;
#else
    (void)trajectories;(void)provenance;
    throw std::runtime_error("Arrow/Parquet output is unavailable in this build");
#endif
}

void write_observations(const std::filesystem::path& path,OutputFormat format,
                        std::span<const Trajectory> trajectories,bool single_run,const OutputProvenance* provenance) {
    validate_single(trajectories,single_run);
    if(format==OutputFormat::csv) { write_text_atomically(path,observations_csv(trajectories,single_run,provenance));return; }
    if(format!=OutputFormat::parquet && format!=OutputFormat::arrow_ipc)
        throw std::invalid_argument("unknown output format");
#if FATHOM_HAS_ARROW
    const auto table=observation_table(trajectories,provenance);
    write_file_atomically(path,[&](int descriptor) {
        struct Descriptor {
            int value;
            ~Descriptor() { if(value>=0) ::close(value); }
        } duplicate{::dup(descriptor)};
        if(duplicate.value<0) throw std::system_error(errno,std::generic_category(),"cannot duplicate result descriptor");
        // Transfer only after a successful Open; the guard covers setup and
        // allocation failures before Arrow assumes descriptor ownership.
        auto sink=unwrap(arrow::io::FileOutputStream::Open(duplicate.value));
        duplicate.value=-1;
        if(format==OutputFormat::parquet) {
            auto properties=parquet::WriterProperties::Builder().compression(parquet::Compression::UNCOMPRESSED)
                ->disable_dictionary()->build();
            auto arrow_properties=parquet::ArrowWriterProperties::Builder().store_schema()->set_use_threads(false)->build();
            check(parquet::arrow::WriteTable(*table,arrow::default_memory_pool(),sink,65536,properties,arrow_properties));
        } else {
            auto writer=unwrap(arrow::ipc::MakeFileWriter(sink,table->schema()));
            check(writer->WriteTable(*table,65536));check(writer->Close());
        }
        check(sink->Flush());check(sink->Close());
    });
#else
    (void)trajectories;
    throw std::runtime_error("Arrow/Parquet output is unavailable in this build");
#endif
}

namespace {
void verify_loaded_observations(const std::filesystem::path& path,OutputFormat format,const OutputProvenance& provenance,
                                std::shared_ptr<arrow::Table> loaded={}) {
#if !FATHOM_HAS_ARROW
    (void)loaded;
#endif
    const auto lineage=parse_lineage(provenance);
    const auto total=lineage.parameters.size()*lineage.replications;
    // Match the experiment coordinator's bounded trajectory catalog. Rows remain
    // materialized, as in the existing output adapter; this is not a streaming API.
    if(total>1000000) throw std::invalid_argument("result verification supports at most 1000000 trajectories");
    std::vector<Trajectory> trajectories;trajectories.reserve(total);
    std::map<std::uint32_t,std::size_t> offsets;
    for(const auto& [scenario,parameters]:lineage.parameters) {
        (void)parameters;offsets.emplace(scenario,trajectories.size());
        for(std::uint32_t replication=0;replication<lineage.replications;++replication)
            trajectories.push_back({scenario,replication,{}});
    }
    std::size_t previous=0;std::uint64_t rows=0;
    const auto append=[&](std::uint32_t scenario,std::uint32_t replication,double time,std::string id,double value,
                          std::string_view manifest_id,std::string_view parameters) {
        const auto offset=offsets.find(scenario);
        if(offset==offsets.end() || replication>=lineage.replications)
            throw std::invalid_argument("result address is absent from manifest");
        const auto index=offset->second+replication;
        if(index<previous || rows>=lineage.rows) throw std::invalid_argument("result row order or count differs");
        if(manifest_id!=lineage.id || parameters!=lineage.parameters.at(scenario))
            throw std::invalid_argument("result row lineage differs from manifest");
        previous=index;++rows;
        trajectories[index].observations.push_back({time,std::move(id),value});
    };
    const std::vector<std::string> names{"scenario","replication","time","output_id","value","manifest_id","scenario_parameters"};
    if(format==OutputFormat::csv) {
        std::ifstream input(path,std::ios::binary);
        if(!input) throw std::runtime_error("cannot open CSV result");
        std::vector<std::string> fields;
        if(!csv_record(input,fields) || fields!=names) throw std::invalid_argument("result CSV schema differs");
        while(csv_record(input,fields)) {
            if(fields.size()!=7) throw std::invalid_argument("result CSV row width differs");
            append(csv_number<std::uint32_t>(fields[0]),csv_number<std::uint32_t>(fields[1]),
                   csv_number<double>(fields[2]),fields[3],csv_number<double>(fields[4]),fields[5],fields[6]);
        }
    } else if(format==OutputFormat::parquet || format==OutputFormat::arrow_ipc) {
#if FATHOM_HAS_ARROW
        const auto table=loaded?std::move(loaded):read_result_table(path,format);
        check(table->ValidateFull());
        if(table->num_columns()!=7 || static_cast<std::uint64_t>(table->num_rows())!=lineage.rows)
            throw std::invalid_argument("result table shape differs");
        const std::vector<std::shared_ptr<arrow::DataType>> types{arrow::uint32(),arrow::uint32(),arrow::float64(),
            arrow::utf8(),arrow::float64(),arrow::utf8(),arrow::utf8()};
        for(int i=0;i<7;++i)
            if(!table->field(i)->Equals(arrow::field(names[i],types[i],false),false) || table->column(i)->null_count()!=0)
                throw std::invalid_argument("result table schema or nullability differs");
        auto values=result_metadata(*table);
        if(values["ankurafathom.schema_version"]!="0.2" || values["ankurafathom.table"]!="observations" ||
           values["ankurafathom.manifest_id"]!=lineage.id || values["ankurafathom.manifest"]!=lineage.manifest)
            throw std::invalid_argument("embedded result provenance differs from manifest");
        arrow::TableBatchReader reader(*table);
        std::shared_ptr<arrow::RecordBatch> batch;
        while(true) {
            check(reader.ReadNext(&batch));if(!batch) break;
            const auto& scenario=static_cast<const arrow::UInt32Array&>(*batch->column(0));
            const auto& replication=static_cast<const arrow::UInt32Array&>(*batch->column(1));
            const auto& time=static_cast<const arrow::DoubleArray&>(*batch->column(2));
            const auto& id=static_cast<const arrow::StringArray&>(*batch->column(3));
            const auto& value=static_cast<const arrow::DoubleArray&>(*batch->column(4));
            const auto& manifest_id=static_cast<const arrow::StringArray&>(*batch->column(5));
            const auto& parameters=static_cast<const arrow::StringArray&>(*batch->column(6));
            for(std::int64_t i=0;i<batch->num_rows();++i)
                append(scenario.Value(i),replication.Value(i),time.Value(i),id.GetString(i),value.Value(i),
                       manifest_id.GetView(i),parameters.GetView(i));
        }
#else
        throw std::runtime_error("Arrow/Parquet verification is unavailable in this build");
#endif
    } else throw std::invalid_argument("unknown result format");
    (void)validate_lineage(trajectories,provenance);
}
} // namespace
void verify_observations(const std::filesystem::path& path,OutputFormat format,const OutputProvenance& provenance) {
    verify_loaded_observations(path,format,provenance);
}
OutputProvenance verify_embedded_observations(const std::filesystem::path& path,OutputFormat format) {
    if(format!=OutputFormat::parquet && format!=OutputFormat::arrow_ipc)
        throw std::invalid_argument("embedded verification requires Arrow or Parquet");
#if FATHOM_HAS_ARROW
    const auto table=read_result_table(path,format);
    const auto metadata=result_metadata(*table);
    const auto manifest=metadata.find("ankurafathom.manifest");
    if(manifest==metadata.end() || manifest->second.empty() || manifest->second.size()>256*1024*1024)
        throw std::invalid_argument("embedded manifest is missing, empty or exceeds 256 MiB");
    OutputProvenance provenance{manifest->second};
    verify_loaded_observations(path,format,provenance,table);
    return provenance;
#else
    (void)path;
    throw std::runtime_error("Arrow/Parquet verification is unavailable in this build");
#endif
}
} // namespace ankurafathom::runtime
