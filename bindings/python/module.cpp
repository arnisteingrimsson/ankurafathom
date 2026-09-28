#include "ankurafathom/c_api.h"
#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <memory>
#include <exception>
#include <stdexcept>
#include <string>
namespace nb=nanobind;
using namespace nb::literals;
namespace {
struct ApiError : std::runtime_error {
    fathom_error record;
    explicit ApiError(const fathom_error& error):std::runtime_error(error.message),record(error) {}
};
void check(fathom_status status) {
    if(status!=FATHOM_OK) throw ApiError(*fathom_last_error());
}
using ModelHandle=std::unique_ptr<fathom_model,decltype(&fathom_model_free)>;
using ResultHandle=std::unique_ptr<fathom_results,decltype(&fathom_results_free)>;
struct Model { ModelHandle handle{nullptr,fathom_model_free}; };
struct ReleaseStream {
    void operator()(ArrowArrayStream* stream) const noexcept {
        if(stream->release) stream->release(stream);
        delete stream;
    }
};
Model load(nb::bytes json,const std::optional<std::string>& base) {
    if(base && base->find('\0')!=std::string::npos) throw nb::value_error("base_directory contains NUL");
    const std::string input(json.c_str(),json.size());
    Model model;
    {
        nb::gil_scoped_release unlock;
        fathom_model* handle=nullptr;
        check(fathom_load_json(input.data(),input.size(),base?base->c_str():nullptr,&handle));
        model.handle.reset(handle);
    }
    return model;
}
struct Progress {
    nb::object callback;
    std::exception_ptr error;
    static int32_t invoke(uint64_t completed,uint64_t total,void* context) noexcept {
        auto& state=*static_cast<Progress*>(context);
        nb::gil_scoped_acquire lock;
        try {
            auto decision=state.callback(completed,total);
            if(!PyBool_Check(decision.ptr())) throw nb::type_error("progress callback must return a bool");
            return decision.ptr()==Py_True?1:0;
        } catch(...) { state.error=std::current_exception();return 0; }
    }
};
nb::capsule run(const Model& model,nb::object experiment,uint32_t threads,const std::optional<uint64_t>& seed,bool provenance,nb::object progress) {
    if(!progress.is_none() && !PyCallable_Check(progress.ptr())) throw nb::type_error("progress must be callable or None");
    Progress state{std::move(progress),{}};
    fathom_run_callbacks callbacks=FATHOM_RUN_CALLBACKS_INIT;
    if(!state.callback.is_none()) { callbacks.progress=&Progress::invoke;callbacks.context=&state; }
    std::optional<std::string> input;
    if(!experiment.is_none()) {
        const auto value=nb::cast<nb::bytes>(experiment);
        input.emplace(value.c_str(),value.size());
    }
    fathom_run_options options=FATHOM_RUN_OPTIONS_INIT;
    options.threads=threads;options.override_seed=seed.has_value();options.seed=seed.value_or(0);
    std::unique_ptr<ArrowArrayStream,ReleaseStream> stream(new ArrowArrayStream{});
    fathom_status status;
    fathom_error diagnostic{};
    {
        nb::gil_scoped_release unlock;
        fathom_results* handle=nullptr;
        status=fathom_run_with_callbacks(model.handle.get(),input?input->data():nullptr,input?input->size():0,&options,&callbacks,&handle);
        if(status!=FATHOM_OK) diagnostic=*fathom_last_error();
        ResultHandle results(handle,fathom_results_free);
        if(status==FATHOM_OK) check(provenance?fathom_results_arrow_with_manifest(results.get(),stream.get()):
                         fathom_results_arrow(results.get(),stream.get()));
        // Results are destroyed here. The exported stream owns its table.
    }
    if(state.error) std::rethrow_exception(state.error);
    if(status!=FATHOM_OK) throw ApiError(diagnostic);
    nb::capsule capsule(stream.get(),"arrow_array_stream",[](void* value) noexcept {
        ReleaseStream{}(static_cast<ArrowArrayStream*>(value));
    });
    stream.release();return capsule;
}
nb::object diagnostic_text(const char* value) {
    // Fixed C diagnostics can end in a truncated UTF-8 sequence.
    auto* text=PyUnicode_DecodeUTF8(value,static_cast<Py_ssize_t>(std::char_traits<char>::length(value)),"replace");
    if(!text) throw nb::python_error();
    return nb::steal<nb::object>(text);
}
}
NB_MODULE(_native,module) {
    if(fathom_abi_version()!=FATHOM_ABI_VERSION) throw std::runtime_error("unsupported Fathom ABI version");
    auto error=nb::steal<nb::object>(PyErr_NewException("ankurafathom.FathomError",PyExc_RuntimeError,nullptr));
    if(!error.is_valid()) throw nb::python_error();
    module.attr("FathomError")=error;
    nb::register_exception_translator([](const std::exception_ptr& pointer,void* payload) {
        try { std::rethrow_exception(pointer); }
        catch(const ApiError& e) {
            try {
                auto type=nb::borrow<nb::object>(static_cast<PyObject*>(payload));
                auto instance=type(diagnostic_text(e.record.message));
                instance.attr("status")=e.record.status;instance.attr("truncated")=e.record.truncated;
                instance.attr("code")=diagnostic_text(e.record.code);
                instance.attr("pointer")=diagnostic_text(e.record.pointer);
                PyErr_SetObject(type.ptr(),instance.ptr());
            } catch(nb::python_error& failure) { failure.restore(); }
            catch(const std::bad_alloc&) { PyErr_NoMemory(); }
        }
    },error.ptr());
    nb::class_<Model>(module,"Model")
        .def_static("from_json",&load,"json"_a,"base_directory"_a=nb::none());
    module.def("run_stream",&run,"model"_a,"experiment"_a=nb::none(),"threads"_a=1,"seed"_a=nb::none(),"provenance"_a=false,"progress"_a=nb::none());
}
