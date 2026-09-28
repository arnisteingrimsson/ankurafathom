// Instrumented Python host: avoid injecting sanitizer runtimes into user Python.
#include <Python.h>
int main(int argc,char** argv) {
    PyConfig config;PyConfig_InitPythonConfig(&config);
    PyStatus status=PyConfig_SetBytesString(&config,&config.program_name,FATHOM_PYTHON_EXECUTABLE);
    if(!PyStatus_Exception(status)) status=PyConfig_SetBytesArgv(&config,argc,argv);
    if(!PyStatus_Exception(status)) status=Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if(PyStatus_Exception(status)) Py_ExitStatusException(status);
    return Py_RunMain();
}
