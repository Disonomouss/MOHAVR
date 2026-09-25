// Loader-free smoke test for an OpenXR runtime DLL: does what the OpenXR loader does
// (GetProcAddress of the plain name "xrNegotiateLoaderRuntimeInterface", negotiate), then
// xrCreateInstance -> xrGetSystem -> xrDestroyInstance with no graphics extension.
//
// Built x86 and x64 by build.ps1; the x86 build is the one that matters for MOHA.exe.
//
//   smoke.exe <path to openxr_simulator.dll>     exit 0 = runtime usable from this bitness
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <openxr/openxr.h>
#include <loader_interfaces.h>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: smoke <runtime.dll>\n"); return 2; }
    std::printf("process: %d-bit\n", (int)(sizeof(void*) * 8));

    HMODULE rt = LoadLibraryA(argv[1]);
    if (!rt) { std::printf("FAIL LoadLibrary error %lu\n", GetLastError()); return 1; }

    auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(rt, "xrNegotiateLoaderRuntimeInterface");
    if (!negotiate) { std::printf("FAIL no undecorated xrNegotiateLoaderRuntimeInterface export\n"); return 1; }

    XrNegotiateLoaderInfo li{};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof(li);
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr{};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof(rr);
    XrResult r = negotiate(&li, &rr);
    if (XR_FAILED(r) || !rr.getInstanceProcAddr) { std::printf("FAIL negotiate %d\n", r); return 1; }
    std::printf("negotiate OK: interface v%u, api %u.%u\n", rr.runtimeInterfaceVersion,
                (unsigned)XR_VERSION_MAJOR(rr.runtimeApiVersion), (unsigned)XR_VERSION_MINOR(rr.runtimeApiVersion));

    PFN_xrGetInstanceProcAddr gipa = rr.getInstanceProcAddr;
    PFN_xrCreateInstance createInstance = nullptr;
    gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&createInstance);
    if (!createInstance) { std::printf("FAIL no xrCreateInstance\n"); return 1; }

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName, "MOHAVR smoke");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance inst = XR_NULL_HANDLE;
    r = createInstance(&ci, &inst);
    if (XR_FAILED(r)) { std::printf("FAIL xrCreateInstance %d\n", r); return 1; }

    PFN_xrGetInstanceProperties getProps = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrDestroyInstance destroy = nullptr;
    gipa(inst, "xrGetInstanceProperties", (PFN_xrVoidFunction*)&getProps);
    gipa(inst, "xrGetSystem", (PFN_xrVoidFunction*)&getSystem);
    gipa(inst, "xrDestroyInstance", (PFN_xrVoidFunction*)&destroy);

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (getProps && XR_SUCCEEDED(getProps(inst, &props))) std::printf("runtime: %s\n", props.runtimeName);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    r = getSystem ? getSystem(inst, &sgi, &sys) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(r)) { std::printf("FAIL xrGetSystem %d\n", r); return 1; }
    std::printf("xrGetSystem OK: system %llu\n", (unsigned long long)sys);

    if (destroy) destroy(inst);
    std::printf("PASS\n");
    return 0;
}
