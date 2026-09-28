#include "provenance.hpp"
#include "fathom_build.hpp"
#include "ankurafathom/runtime/sha256.hpp"
#include "ankurafathom/runtime/observation_identity.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include <bit>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/utsname.h>
namespace ankurafathom::ir::provenance {
namespace {
std::string hash(std::string_view bytes) { runtime::Sha256 h;h.update(bytes);return h.digest(); }
Json identity(const InputIdentity& input) {
    return {{"path",input.path},{"file_sha256",input.file_hash},{"canonical_sha256",input.canonical_hash},
            {"canonical_json",input.canonical_json}};
}
Json build() {
    struct utsname platform{};
    if(::uname(&platform)!=0) throw Error("IR_PROVENANCE","/execution/build","cannot identify platform");
    Json value={{"version",FATHOM_VERSION},{"source_sha256",FATHOM_SOURCE_HASH},
        {"git_commit",std::string(FATHOM_GIT_COMMIT).empty()?Json(nullptr):Json(FATHOM_GIT_COMMIT)},{"compiler",FATHOM_COMPILER},{"build_type",FATHOM_BUILD_TYPE},
        {"configured_system",FATHOM_BUILD_SYSTEM},{"compiler_flags",FATHOM_BUILD_FLAGS},
        {"sanitizers",FATHOM_BUILD_SANITIZERS},{"arrow",FATHOM_BUILD_ARROW},
        {"system",platform.sysname},{"release",platform.release},{"machine",platform.machine},
        {"pointer_bits",sizeof(void*)*8},{"double_bits",sizeof(double)*8},
        {"byte_order",std::endian::native==std::endian::little?"little":"big"}};
#ifdef _LIBCPP_VERSION
    value["stdlib"]="libc++";value["stdlib_version"]=_LIBCPP_VERSION;
#elif defined(__GLIBCXX__)
    value["stdlib"]="libstdc++";value["stdlib_version"]=__GLIBCXX__;
#else
    value["stdlib"]="unknown";value["stdlib_version"]=nullptr;
#endif
    return value;
}
void exact_keys(const Json& value,std::initializer_list<const char*> names,const std::string& at) {
    if(!value.is_object() || value.size()!=names.size()) throw Error("IR_MANIFEST",at,"invalid manifest object");
    for(const auto* name:names) if(!value.contains(name)) throw Error("IR_MANIFEST",at,"missing manifest field");
}
}
Json read_input(const std::string& path,InputIdentity& input,const std::string& pointer) {
    std::ifstream file(path,std::ios::binary);
    if(!file) throw Error("IR_IO",pointer,"cannot open JSON input: "+path);
    std::string bytes;char block[65536];
    while(file.read(block,sizeof(block)) || file.gcount()) {
        if(bytes.size()+static_cast<std::size_t>(file.gcount())>256*1024*1024)
            throw Error("IR_IO",pointer,"JSON input exceeds 256 MiB limit");
        bytes.append(block,static_cast<std::size_t>(file.gcount()));
    }
    if(!file.eof()) throw Error("IR_IO",pointer,"cannot read JSON input");
    auto document=parse_input(bytes,input,pointer);
    input.path=std::filesystem::absolute(path).string();
    return document;
}
Json parse_input(std::string_view bytes,InputIdentity& input,const std::string& pointer) {
    if(bytes.size()>256*1024*1024) throw Error("IR_IO",pointer,"JSON input exceeds 256 MiB limit");
    if(bytes.find('\0')!=std::string_view::npos) throw Error("IR_JSON",pointer,"JSON input contains a raw NUL byte");
    Json document;
    try { document=Json::parse(bytes); }
    catch(const Json::exception& e) { throw Error("IR_JSON",pointer,e.what()); }
    const auto canonical=document.dump();
    input={"",hash(bytes),hash(canonical),canonical};
    return document;
}
Json inputs(const Model& model,const std::optional<runtime::Experiment>& experiment,
            const std::optional<InputIdentity>& experiment_input,std::uint64_t seed,std::size_t threads) {
    Json data=Json::array();
    const auto document=Json::parse(model.input.canonical_json);
    const auto append=[&](const std::string& id,const std::string& source,const std::string& file_hash,const std::string& canonical_hash) {
        for(const auto& declaration:document.at("data")) if(declaration.at("id")==id) {
            data.push_back({{"binding",declaration},{"path",source},{"file_sha256",file_hash},{"canonical_sha256",canonical_hash}});return;
        }
        throw Error("IR_PROVENANCE","/data","missing binding declaration");
    };
    for(const auto& binding:model.parameter_data) append(binding.id,binding.source,binding.file_hash,binding.canonical_hash);
    for(const auto& binding:model.series_data) append(binding.id,binding.source,binding.series.file_hash(),binding.series.canonical_hash());
    if(model.population_data) {
        const auto& binding=*model.population_data;
        append(binding.id,binding.source,binding.receipt.file_hash,binding.receipt.canonical_hash);
    }
    std::sort(data.begin(),data.end(),[](const auto& a,const auto& b) { return a.at("binding").at("id")<b.at("binding").at("id"); });
    Json scenarios=Json::array();
    if(experiment) for(const auto& scenario:experiment->scenarios) {
        auto effective=model.parameters;
        for(const auto& [name,value]:scenario.parameters) effective.at(name)=value;
        scenarios.push_back({{"id",scenario.id},{"overrides",scenario.parameters},{"effective_parameters",effective}});
    } else scenarios.push_back({{"id",0},{"overrides",Json::object()},{"effective_parameters",model.parameters}});
    return {{"model",identity(model.input)},{"data",data},
        {"experiment",experiment_input ? identity(*experiment_input) : Json(nullptr)},
        {"scenarios",scenarios},{"replications",experiment?experiment->replications:1},
        {"execution",{{"seed",seed},{"threads",threads},{"backend","cpu-reference"},
          {"precision","binary64;ffp-contract=off;no-fast-math;ordered-observations-v1"},{"build",build()}}}};
}
Json result(std::span<const runtime::Trajectory> trajectories) {
    const auto identity=runtime::observation_identity(trajectories);
    return {{"encoding","ordered-ieee754-le-v1"},{"sha256",identity.sha256},
            {"trajectories",identity.trajectories},{"rows",identity.rows}};
}
static Json envelope(Json input,Json output,Json declaration,const char* version) {
    const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};if(!::gmtime_r(&now,&utc)) throw Error("IR_PROVENANCE","","cannot read UTC time");
    std::ostringstream time;time<<std::put_time(&utc,"%Y-%m-%dT%H:%M:%SZ");
    Json value={{"manifest_version",version},{"created_utc",time.str()},{"inputs",std::move(input)},
        {"result",std::move(output)},{"output",std::move(declaration)},
        {"validation",{{"model","pass"},{"execution","pass"},{"accuracy","not_assessed"}}}};
    value["id"]=hash(value.dump());return value;
}
Json manifest(Json input,Json output,const std::string& path,runtime::OutputFormat format) {
    return envelope(std::move(input),std::move(output),
        {{"schema_version","0.2"},{"path",std::filesystem::absolute(path).string()},
         {"format",format==runtime::OutputFormat::csv?"csv":format==runtime::OutputFormat::parquet?"parquet":"arrow"}},"0.2");
}
Json memory_manifest(Json input,Json output) {
    return validate_manifest(envelope(std::move(input),std::move(output),
        {{"schema_version","0.2"},{"path",""},{"format","memory"}},"0.3"));
}
Json read_manifest(const std::string& path) {
    InputIdentity unused;auto value=read_input(path,unused,"/manifest");
    return validate_manifest(std::move(value));
}
Json validate_manifest(Json value) {
    exact_keys(value,{"manifest_version","created_utc","inputs","result","output","validation","id"},"/manifest");
    if((value.at("manifest_version")!="0.1" && value.at("manifest_version")!="0.2" && value.at("manifest_version")!="0.3") || !value.at("id").is_string()) throw Error("IR_MANIFEST","/manifest","unsupported manifest");
    const auto id=value.at("id");value.erase("id");
    if(id!=hash(value.dump())) throw Error("IR_MANIFEST","/manifest/id","manifest checksum differs");
    value["id"]=id;
    const bool memory=value.at("manifest_version")=="0.3";
    try {
        if(value.at("validation")!=Json{{"model","pass"},{"execution","pass"},{"accuracy","not_assessed"}})
            throw Error("IR_MANIFEST","/manifest/validation","unsupported validation verdict");
        if(!value.at("created_utc").is_string()) throw Error("IR_MANIFEST","/manifest/created_utc","invalid timestamp");
        const auto timestamp=value.at("created_utc").get<std::string>();
        const std::string pattern="0000-00-00T00:00:00Z";
        if(timestamp.size()!=pattern.size()) throw Error("IR_MANIFEST","/manifest/created_utc","invalid UTC timestamp");
        for(std::size_t i=0;i<pattern.size();++i)
            if(pattern[i]=='0' ? (timestamp[i]<'0' || timestamp[i]>'9') : timestamp[i]!=pattern[i])
                throw Error("IR_MANIFEST","/manifest/created_utc","invalid UTC timestamp");
        exact_keys(value.at("result"),{"encoding","sha256","trajectories","rows"},"/manifest/result");
        if(value.at("result").at("encoding")!="ordered-ieee754-le-v1" || !value.at("result").at("sha256").is_string() ||
           !value.at("result").at("trajectories").is_number_unsigned() || !value.at("result").at("rows").is_number_unsigned())
            throw Error("IR_MANIFEST","/manifest/result","invalid numeric identity");
        if(value.at("manifest_version")=="0.2" || memory) {
            exact_keys(value.at("output"),{"path","format","schema_version"},"/manifest/output");
            if(value.at("output").at("schema_version")!="0.2") throw Error("IR_MANIFEST","/manifest/output/schema_version","unsupported result schema");
        } else exact_keys(value.at("output"),{"path","format"},"/manifest/output");
        const auto& input=value.at("inputs");
        exact_keys(input,{"model","data","experiment","scenarios","replications","execution"},"/manifest/inputs");
        for(const auto* key:{"model","experiment"}) if(!input.at(key).is_null()) {
            const auto& item=input.at(key);
            exact_keys(item,{"path","file_sha256","canonical_sha256","canonical_json"},"/manifest/inputs");
            for(const auto* field:{"path","file_sha256","canonical_sha256","canonical_json"})
                if(!item.at(field).is_string() || (item.at(field).get_ref<const std::string&>().empty() && !(memory && std::string_view(field)=="path")))
                    throw Error("IR_MANIFEST","/manifest/inputs","invalid input identity");
            const auto path=item.at("path").get<std::string>();
            if(path.find('\0')!=std::string::npos || (!path.empty() && !std::filesystem::path(path).is_absolute()))
                throw Error("IR_MANIFEST","/manifest/inputs","input paths must be absolute local paths");
            if(memory) {
                const auto canonical=item.at("canonical_json").get<std::string>();
                const auto raw_hash=item.at("file_sha256").get<std::string>();
                if(raw_hash.size()!=64 || raw_hash.find_first_not_of("0123456789abcdef")!=std::string::npos ||
                   hash(canonical)!=item.at("canonical_sha256") || Json::parse(canonical).dump()!=canonical)
                    throw Error("IR_MANIFEST","/manifest/inputs","invalid captured JSON identity");
            }
        }
        if(input.at("model").is_null() || !input.at("data").is_array()) throw Error("IR_MANIFEST","/manifest/inputs","missing inputs");
        const auto& execution=input.at("execution");
        if(!execution.at("seed").is_number_unsigned() || !execution.at("threads").is_number_unsigned() ||
           execution.at("threads").get<std::uint64_t>()<1 || execution.at("threads").get<std::uint64_t>()>256)
            throw Error("IR_MANIFEST","/manifest/inputs/execution","invalid execution settings");
        if(!value.at("output").at("path").is_string() || !value.at("output").at("format").is_string())
            throw Error("IR_MANIFEST","/manifest/output","invalid output declaration");
        const auto output_path=value.at("output").at("path").get<std::string>();
        const auto output_format=value.at("output").at("format").get<std::string>();
        if(memory) {
            if(!output_path.empty() || output_format!="memory")
                throw Error("IR_MANIFEST","/manifest/output","in-memory output requires an empty path and memory format");
        } else if(output_path.empty() || output_path.find('\0')!=std::string::npos || !std::filesystem::path(output_path).is_absolute() ||
           (output_format!="csv" && output_format!="parquet" && output_format!="arrow"))
            throw Error("IR_MANIFEST","/manifest/output","invalid output path or format");
    } catch(const Json::exception& e) { throw Error("IR_MANIFEST","/manifest",e.what()); }
    return value;
}
void require_equal(const Json& actual,const Json& expected,const std::string& pointer) {
    if(actual.dump()!=expected.dump()) throw Error("IR_REPLAY_MISMATCH",pointer,"replay identity differs");
}
bool same_destination(const std::string& a,const std::string& b) {
    return runtime::same_file(a,b) || std::filesystem::weakly_canonical(a)==std::filesystem::weakly_canonical(b);
}
void publish(const std::string& path,const Json& manifest,const std::string& output,
             runtime::OutputFormat format,std::span<const runtime::Trajectory> trajectories,bool single) {
    const auto bytes=manifest.dump(2)+"\n";
    // Prepare and flush the manifest before publishing results; commit the manifest
    // last. Two file renames are not a filesystem-wide atomic transaction.
    runtime::write_file_atomically(path,[&](int fd) {
        std::size_t offset=0;
        while(offset<bytes.size()) {
            const auto n=::write(fd,bytes.data()+offset,std::min<std::size_t>(bytes.size()-offset,16*1024*1024));
            if(n<0) { if(errno==EINTR) continue;throw std::system_error(errno,std::generic_category(),"manifest write failed"); }
            if(n==0) throw std::runtime_error("manifest write made no progress");
            offset+=static_cast<std::size_t>(n);
        }
        while(::fsync(fd)!=0) if(errno!=EINTR) throw std::system_error(errno,std::generic_category(),"manifest flush failed");
        const runtime::OutputProvenance lineage{manifest.dump()};
        runtime::write_observations(output,format,trajectories,single,&lineage);
    });
}
}
