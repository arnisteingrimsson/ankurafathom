#include "ankurafathom/c_api.h"
#include "ankurafathom/ir/model.hpp"
#include "ankurafathom/runtime/observation_identity.hpp"
#include "provenance.hpp"
#include <cstring>
#include <memory>
#include "ankurafathom/runtime/outputs.hpp"
#if FATHOM_HAS_ARROW
#include <arrow/c/bridge.h>
#include <arrow/table.h>
#endif

namespace ir=ankurafathom::ir;
namespace rt=ankurafathom::runtime;
struct fathom_model { ir::Model value; };
struct fathom_results {
    std::vector<rt::Trajectory> trajectories;
    std::vector<std::uint64_t> offsets;
    rt::ObservationIdentity identity;
    std::string manifest;
};
namespace {
thread_local fathom_error error_record{};
struct Failure { fathom_status status;const char* code;const char* message; };
void require(bool condition,const char* message) {
    if(!condition) throw Failure{FATHOM_INVALID_ARGUMENT,"FATHOM_ARGUMENT",message};
}
template<std::size_t N> void copy_text(char (&destination)[N],std::string_view source,std::uint32_t flag) noexcept {
    std::size_t i=0;
    for(;i<N-1 && i<source.size() && source[i];++i) destination[i]=source[i];
    destination[i]='\0';
    if(i<source.size()) error_record.truncated|=flag;
}
fathom_status fail(fathom_status status,std::string_view code,std::string_view pointer,std::string_view message) noexcept {
    error_record={};
    error_record.status=status;
    copy_text(error_record.code,code,1);copy_text(error_record.pointer,pointer,2);copy_text(error_record.message,message,4);
    return status;
}
template<class F> fathom_status boundary(F&& function) noexcept {
    error_record={};
    try { function();error_record={};return FATHOM_OK; }
    catch(const Failure& e) { return fail(e.status,e.code,"",e.message); }
    catch(const rt::ExperimentCancelled& e) { return fail(FATHOM_CANCELLED,"FATHOM_CANCELLED","",e.what()); }
    catch(const ir::Error& e) { return fail(FATHOM_IR_ERROR,e.code,e.pointer,e.what()); }
    catch(const std::bad_alloc&) { return fail(FATHOM_OUT_OF_MEMORY,"FATHOM_MEMORY","","allocation failed"); }
    catch(const std::exception& e) { return fail(FATHOM_RUNTIME_ERROR,"FATHOM_RUNTIME","",e.what()); }
    catch(...) { return fail(FATHOM_RUNTIME_ERROR,"FATHOM_RUNTIME","","unknown C++ exception"); }
}
std::string_view bytes(const char* data,std::size_t size) {
    require(data!=nullptr,"JSON pointer is required");
    require(size<=256*1024*1024,"JSON input exceeds 256 MiB limit");
    return {data,size};
}
}
extern "C" {
uint32_t fathom_abi_version(void) { return FATHOM_ABI_VERSION; }
const fathom_error* fathom_last_error(void) { return &error_record; }
void fathom_model_free(fathom_model* model) { delete model; }
void fathom_results_free(fathom_results* results) { delete results; }
fathom_status fathom_load_json(const char* json,size_t size,const char* base_directory,fathom_model** model) {
    return boundary([&] {
        require(model && !*model,"model destination must point to NULL");
        const auto input=bytes(json,size);
        std::optional<std::filesystem::path> base;
        if(base_directory) {
            require(*base_directory!='\0',"base directory cannot be empty");
            base=base_directory;
            require(base->is_absolute(),"base directory must be absolute");
        }
        auto owned=std::make_unique<fathom_model>(fathom_model{ir::load_json(input,base?&*base:nullptr)});
        *model=owned.release();
    });
}
fathom_status fathom_load_file(const char* path,fathom_model** model) {
    return boundary([&] {
        require(model && !*model,"model destination must point to NULL");
        require(path && *path,"model path is required");
        auto owned=std::make_unique<fathom_model>(fathom_model{ir::load_file(path)});
        *model=owned.release();
    });
}
fathom_status fathom_run(const fathom_model* model,const char* experiment_json,size_t experiment_size,
                        const fathom_run_options* options,fathom_results** results) {
    return fathom_run_with_callbacks(model,experiment_json,experiment_size,options,nullptr,results);
}
fathom_status fathom_run_with_callbacks(const fathom_model* model,const char* experiment_json,size_t experiment_size,
                        const fathom_run_options* options,const fathom_run_callbacks* callbacks,fathom_results** results) {
    return boundary([&] {
        require(model,"model is required");require(results && !*results,"results destination must point to NULL");
        const fathom_run_options defaults=FATHOM_RUN_OPTIONS_INIT;
        const auto settings=options?*options:defaults;
        if(settings.abi_version!=FATHOM_ABI_VERSION)
            throw Failure{FATHOM_ABI_MISMATCH,"FATHOM_ABI","unsupported run-options ABI version"};
        require(settings.threads>=1 && settings.threads<=256,"threads must be in [1, 256]");
        require(settings.reserved==0 && settings.override_seed<=1,"invalid run-options flags");
        require(settings.override_seed || settings.seed==0,"seed requires override_seed");
        require(experiment_json || experiment_size==0,"NULL experiment must have zero size");
        const fathom_run_callbacks default_callbacks=FATHOM_RUN_CALLBACKS_INIT;
        const auto hooks=callbacks?*callbacks:default_callbacks;
        if(hooks.abi_version!=FATHOM_ABI_VERSION)
            throw Failure{FATHOM_ABI_MISMATCH,"FATHOM_ABI","unsupported callbacks ABI version"};
        require(hooks.reserved==0,"invalid callbacks flags");
        rt::ExecutionOptions execution{settings.threads};
        if(hooks.progress) execution.progress=[hooks](const rt::ExperimentProgress& progress) {
            const auto decision=hooks.progress(progress.completed,progress.total,hooks.context);
            require(decision==0 || decision==1,"progress callback must return 0 or 1");
            return decision==1;
        };
        auto owned=std::make_unique<fathom_results>();
        std::optional<rt::Experiment> experiment;
        std::optional<ir::InputIdentity> experiment_input;
        if(experiment_json) {
            experiment_input.emplace();
            experiment=ir::load_experiment_json(bytes(experiment_json,experiment_size),model->value,
                settings.override_seed?std::optional{settings.seed}:std::nullopt,&*experiment_input);
            owned->trajectories=rt::run_experiment(*experiment,[model](const rt::Scenario& scenario,std::uint32_t replication,std::uint64_t seed) {
                const auto rows=ir::run(model->value,scenario.parameters,seed,scenario.id,replication);
                std::vector<rt::Observation> observations;observations.reserve(rows.size());
                for(const auto& row:rows) observations.push_back({row.time,row.output_id,row.value});
                return observations;
            },execution);
        } else {
            if(execution.progress && !execution.progress({0,1})) throw rt::ExperimentCancelled(0,1);
            rt::Trajectory trajectory{0,0,{}};
            for(const auto& row:ir::run(model->value,{},settings.seed))
                trajectory.observations.push_back({row.time,row.output_id,row.value});
            owned->trajectories.push_back(std::move(trajectory));
            if(execution.progress && !execution.progress({1,1})) throw rt::ExperimentCancelled(1,1);
        }
        owned->identity=rt::observation_identity(owned->trajectories);
        std::uint64_t offset=0;
        for(const auto& trajectory:owned->trajectories) {
            owned->offsets.push_back(offset);offset+=trajectory.observations.size();
        }
        const auto input=ir::provenance::inputs(model->value,experiment,experiment_input,
            experiment?experiment->seed:settings.seed,settings.threads);
        owned->manifest=ir::provenance::memory_manifest(input,ir::provenance::result(owned->trajectories)).dump();
        *results=owned.release();
    });
}
static fathom_status export_arrow(const fathom_results* results,ArrowArrayStream* stream,bool with_manifest) {
    return boundary([&] {
        require(results && stream,"results and stream destination are required");
        require(!stream->release,"stream destination must be released or zero-initialized");
#if FATHOM_HAS_ARROW
        // The shared_ptr overload owns the table; the reference overload would
        // leave the reader dangling when this entry point returns.
        const auto lineage=with_manifest?std::optional{rt::OutputProvenance{results->manifest}}:std::nullopt;
        auto reader=std::make_shared<arrow::TableBatchReader>(rt::observation_table(results->trajectories,lineage?&*lineage:nullptr));
        reader->set_chunksize(65536);
        struct Export {
            ArrowArrayStream value{};
            ~Export() { if(value.release) value.release(&value); }
        } exported;
        const auto status=arrow::ExportRecordBatchReader(std::move(reader),&exported.value);
        if(status.IsOutOfMemory()) throw std::bad_alloc{};
        if(!status.ok()) throw std::runtime_error(status.ToString());
        *stream=exported.value;
        exported.value.release=nullptr;
#else
        (void)with_manifest;
        throw Failure{FATHOM_UNAVAILABLE,"FATHOM_UNAVAILABLE","Arrow C Stream export is unavailable in this build"};
#endif
    });
}
fathom_status fathom_results_arrow(const fathom_results* results,ArrowArrayStream* stream) {
    return export_arrow(results,stream,false);
}
fathom_status fathom_results_arrow_with_manifest(const fathom_results* results,ArrowArrayStream* stream) {
    return export_arrow(results,stream,true);
}
fathom_status fathom_results_manifest(const fathom_results* results,const char** json,size_t* size) {
    return boundary([&] {
        require(results && json && size,"results, JSON and size destinations are required");
        *json=results->manifest.c_str();*size=results->manifest.size();
    });
}
fathom_status fathom_results_size(const fathom_results* results,uint64_t* trajectories,uint64_t* rows) {
    return boundary([&] {
        require(results && trajectories && rows,"results and both count destinations are required");
        require(trajectories!=rows,"count destinations must be distinct");
        *trajectories=results->identity.trajectories;*rows=results->identity.rows;
    });
}
fathom_status fathom_results_sha256(const fathom_results* results,const char** digest) {
    return boundary([&] {
        require(results && digest,"results and digest destination are required");
        *digest=results->identity.sha256.c_str();
    });
}
fathom_status fathom_results_observation(const fathom_results* results,uint64_t index,fathom_observation* observation) {
    return boundary([&] {
        require(results && observation,"results and observation destination are required");
        if(index>=results->identity.rows) throw Failure{FATHOM_OUT_OF_RANGE,"FATHOM_RANGE","observation index out of range"};
        const auto position=static_cast<std::size_t>(std::upper_bound(results->offsets.begin(),results->offsets.end(),index)-results->offsets.begin()-1);
        const auto& trajectory=results->trajectories[position];
        const auto& row=trajectory.observations[index-results->offsets[position]];
        *observation={trajectory.scenario,trajectory.replication,row.time,row.value,row.output_id.c_str(),row.output_id.size()};
    });
}
}
