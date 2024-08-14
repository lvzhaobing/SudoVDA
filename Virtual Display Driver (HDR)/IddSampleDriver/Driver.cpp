/*++

Copyright (c) Microsoft Corporation

Abstract:

	This module contains a sample implementation of an indirect display driver. See the included README.md file and the
	various TODO blocks throughout this file and all accompanying files for information on building a production driver.

	MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"
#include "edid.h"

#include <tuple>
#include <list>
#include <iostream>
#include <thread>
#include <mutex>

#include <AdapterOption.h>
#include <vdd_ioctl.h>

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

LUID preferredAdapterLuid{};
bool preferredAdapterChanged = false;

std::mutex monitorListOp;
std::queue<size_t> freeConnectorSlots;
std::list<IndirectMonitorContext*> monitorCtxList;

DWORD watchdogTimeout = 3; // seconds
DWORD watchdogCountdown = 0;
std::thread watchdogThread;

#pragma region SampleMonitors

static constexpr DWORD IDD_SAMPLE_MONITOR_COUNT = 10;

// Default modes reported for edid-less monitors. The first mode is set as preferred
static const struct VirtualMonitorMode s_DefaultModes[] =
{
	{800, 600, 30},
	{800, 600, 60},
	{800, 600, 90},
	{800, 600, 120},
	{800, 600, 144},
	{800, 600, 165},
	{800, 600, 180},
	{800, 600, 240},
	{1280, 720, 30},
	{1280, 720, 60},
	{1280, 720, 90},
	{1280, 720, 130},
	{1280, 720, 144},
	{1280, 720, 165},
	{1280, 720, 180},
	{1366, 768, 30},
	{1366, 768, 60},
	{1366, 768, 90},
	{1366, 768, 120},
	{1366, 768, 144},
	{1366, 768, 165},
	{1366, 768, 180},
	{1366, 768, 240},
	{1920, 1080, 30},
	{1920, 1080, 60},
	{1920, 1080, 90},
	{1920, 1080, 120},
	{1920, 1080, 144},
	{1920, 1080, 165},
	{1920, 1080, 180},
	{1920, 1080, 240},
	{2560, 1440, 30},
	{2560, 1440, 60},
	{2560, 1440, 90},
	{2560, 1440, 120},
	{2560, 1440, 144},
	{2560, 1440, 165},
	{2560, 1440, 180},
	{2560, 1440, 240},
	{3840, 2160, 30},
	{3840, 2160, 60},
	{3840, 2160, 90},
	{3840, 2160, 120},
	{3840, 2160, 144},
	{3840, 2160, 165},
	{3840, 2160, 180},
	{3840, 2160, 240},
};

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
	Mode.totalSize.cx = Mode.activeSize.cx = Width;
	Mode.totalSize.cy = Mode.activeSize.cy = Height;

	// See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
	Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
	Mode.AdditionalSignalInfo.videoStandard = 255;

	Mode.vSyncFreq.Numerator = VSync;
	Mode.vSyncFreq.Denominator = 1;
	Mode.hSyncFreq.Numerator = VSync * Height;
	Mode.hSyncFreq.Denominator = 1;

	Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

	Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
	IDDCX_MONITOR_MODE Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

	return Mode;
}

static IDDCX_MONITOR_MODE2 CreateIddCxMonitorMode2(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
	IDDCX_MONITOR_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	Mode.BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8 | IDDCX_BITS_PER_COMPONENT_10;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

	return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
	IDDCX_TARGET_MODE Mode = {};

	Mode.Size = sizeof(Mode);
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

	return Mode;
}

static IDDCX_TARGET_MODE2 CreateIddCxTargetMode2(DWORD Width, DWORD Height, DWORD VSync)
{
	IDDCX_TARGET_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8 | IDDCX_BITS_PER_COMPONENT_10;
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

	return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD IddSampleDriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD IddSampleDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY IddSampleDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED IddSampleAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES IddSampleAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION IddSampleParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES IddSampleMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES IddSampleMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN IddSampleMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN IddSampleMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO IddSampleAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA IddSampleMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 IddSampleParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 IddSampleMonitorQueryModes2;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 IddSampleAdapterCommitModes2;

EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP IddSampleMonitorSetGammaRamp;

struct IndirectDeviceContextWrapper
{
	IndirectDeviceContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

struct IndirectMonitorContextWrapper
{
	IndirectMonitorContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
	_In_ HINSTANCE hInstance,
	_In_ UINT dwReason,
	_In_opt_ LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(hInstance);
	UNREFERENCED_PARAMETER(lpReserved);
	UNREFERENCED_PARAMETER(dwReason);

	return TRUE;
}

void LoadSettings() {
	HKEY hKey;
	DWORD bufferSize;
	LONG lResult;

	// Open the registry key
	lResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SudoMaker\\SudoVDA", 0, KEY_READ, &hKey);
	if (lResult != ERROR_SUCCESS) {
		return;
	}

	// Query gpuName
	wchar_t gpuName[128];
	bufferSize = sizeof(gpuName);
	lResult = RegQueryValueExW(hKey, L"gpuName", NULL, NULL, (LPBYTE)gpuName, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		AdapterOption adapterOpt = AdapterOption();
		adapterOpt.selectGPU(gpuName);

		preferredAdapterLuid = adapterOpt.adapterLuid;
		preferredAdapterChanged = adapterOpt.hasTargetAdapter;
	}

	// Query watchdog
	DWORD _watchdogTimeout;
	bufferSize = sizeof(DWORD);
	lResult = RegQueryValueExW(hKey, L"watchdog", NULL, NULL, (LPBYTE)&_watchdogTimeout, &bufferSize);
	if (lResult == ERROR_SUCCESS) {
		watchdogTimeout = _watchdogTimeout;
	}

	// Close the registry key
	RegCloseKey(hKey);
}

void DisconnectAllMonitors() {
	std::lock_guard<std::mutex> lg(monitorListOp);

	if (monitorCtxList.empty()) {
		return;
	}

	for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
		auto* ctx = *it;
		// Remove the monitor
		freeConnectorSlots.push(ctx->connectorId);
		IddCxMonitorDeparture(ctx->GetMonitor());
	}

	monitorCtxList.clear();
}

void RunWatchdog() {
	if (watchdogTimeout) {
		watchdogCountdown = watchdogTimeout;
		watchdogThread = std::thread([]{
			for (;;) {
				if (watchdogTimeout) {
					Sleep(1000);

					if (!watchdogCountdown || monitorCtxList.empty()) {
						continue;
					}

					watchdogCountdown -= 1;

					if (!watchdogCountdown) {
						DisconnectAllMonitors();
					}
				} else {
					DisconnectAllMonitors();
					return;
				}
			}
		});
	}
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
	PDRIVER_OBJECT  pDriverObject,
	PUNICODE_STRING pRegistryPath
)
{
	LoadSettings();

	WDF_DRIVER_CONFIG Config;
	NTSTATUS Status;

	WDF_OBJECT_ATTRIBUTES Attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

	WDF_DRIVER_CONFIG_INIT(&Config,
		IddSampleDeviceAdd
	);

	Config.EvtDriverUnload = IddSampleDriverUnload;

	Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	RunWatchdog();

	return Status;
}

_Use_decl_annotations_
void IddSampleDriverUnload(_In_ WDFDRIVER) {
	if (watchdogTimeout > 0) {
		watchdogTimeout = 0;
		watchdogThread.join();
	} else {
		DisconnectAllMonitors();
	}
}

VOID IddSampleIoDeviceControl(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode
);

_Use_decl_annotations_
NTSTATUS IddSampleDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

	UNREFERENCED_PARAMETER(Driver);

	// Register for power callbacks - in this sample only power-on is needed
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = IddSampleDeviceD0Entry;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	IDD_CX_CLIENT_CONFIG IddConfig;
	IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

	// If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
	// redirects IoDeviceControl requests to an internal queue.
	IddConfig.EvtIddCxDeviceIoControl = IddSampleIoDeviceControl;

	IddConfig.EvtIddCxAdapterInitFinished = IddSampleAdapterInitFinished;

	IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = IddSampleMonitorGetDefaultModes;
	IddConfig.EvtIddCxMonitorAssignSwapChain = IddSampleMonitorAssignSwapChain;
	IddConfig.EvtIddCxMonitorUnassignSwapChain = IddSampleMonitorUnassignSwapChain;

	IddConfig.EvtIddCxParseMonitorDescription = IddSampleParseMonitorDescription;
	IddConfig.EvtIddCxMonitorQueryTargetModes = IddSampleMonitorQueryModes;
	IddConfig.EvtIddCxAdapterCommitModes = IddSampleAdapterCommitModes;

	if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo))
	{
		IddConfig.EvtIddCxAdapterQueryTargetInfo = IddSampleAdapterQueryTargetInfo;
	// }

	// if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetDefaultHdrMetaData))
	// {
		IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = IddSampleMonitorSetDefaultHdrMetadata;
	// }

	// if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxParseMonitorDescription2))
	// {
		IddConfig.EvtIddCxParseMonitorDescription2 = IddSampleParseMonitorDescription2;
	// }

	// if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorQueryTargetModes2))
	// {
		IddConfig.EvtIddCxMonitorQueryTargetModes2 = IddSampleMonitorQueryModes2;
	// }

	// if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterCommitModes2))
	// {
		IddConfig.EvtIddCxAdapterCommitModes2 = IddSampleAdapterCommitModes2;
	// }

	// if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetGammaRamp))
	// {
		IddConfig.EvtIddCxMonitorSetGammaRamp = IddSampleMonitorSetGammaRamp;
	}

	Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
	{
		// Automatically cleanup the context when the WDF object is about to be deleted
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
		if (pContext)
		{
			pContext->Cleanup();
		}
	};

	WDFDEVICE Device = nullptr;
	Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	Status = WdfDeviceCreateDeviceInterface(
		Device,
		&SUVDA_INTERFACE_GUID,
		NULL
	);

	if (!NT_SUCCESS(Status)) {
		return Status;
	}

	Status = IddCxDeviceInitialize(Device);

	// Create a new device context object and attach it to the WDF device object
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext = new IndirectDeviceContext(Device);

	return Status;
}

_Use_decl_annotations_
NTSTATUS IddSampleDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
	UNREFERENCED_PARAMETER(PreviousState);

	// This function is called by WDF to start the device in the fully-on power state.

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext->InitAdapter();

	return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice()
{
	AdapterLuid = preferredAdapterLuid;
}

HRESULT Direct3DDevice::Init()
{
	// The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
	// created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
	if (FAILED(hr))
	{
		return hr;
	}

	// Find the specified render adapter
	hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
	if (FAILED(hr))
	{
		return hr;
	}

	// Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
	hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
	if (FAILED(hr))
	{
		// If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
		// system is in a transient state.
		return hr;
	}

	return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

	// Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
	m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
	// Alert the swap-chain processing thread to terminate
	SetEvent(m_hTerminateEvent.Get());

	if (m_hThread.Get())
	{
		// Wait for the thread to terminate
		WaitForSingleObject(m_hThread.Get(), INFINITE);
	}
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

	RunCore();

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	WdfObjectDelete((WDFOBJECT)m_hSwapChain);
	m_hSwapChain = nullptr;

	AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		return;
	}

	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	if (FAILED(hr))
	{
		return;
	}

	// Acquire and release buffers in a loop
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		IDXGIResource* pSurface;

		if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
			IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
			BufferInArgs.Size = sizeof(BufferInArgs);
			IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}
		else
		{
			IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
			hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);
			pSurface = Buffer.MetaData.pSurface;
		}

		// Ask for the next buffer from the producer
		// IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
		// hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

		// AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
		if (hr == E_PENDING)
		{
			// We must wait for a new buffer
			HANDLE WaitHandles [] =
			{
				m_hAvailableBufferEvent,
				m_hTerminateEvent.Get()
			};
			DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
			if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
			{
				// We have a new buffer, so try the AcquireBuffer again
				continue;
			}
			else if (WaitResult == WAIT_OBJECT_0 + 1)
			{
				// We need to terminate
				break;
			}
			else
			{
				// The wait was cancelled or something unexpected happened
				hr = HRESULT_FROM_WIN32(WaitResult);
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			// We have new frame to process, the surface has a reference on it that the driver has to release
			AcquiredBuffer.Attach(pSurface);

			// ==============================
			// TODO: Process the frame here
			//
			// This is the most performance-critical section of code in an IddCx driver. It's important that whatever
			// is done with the acquired surface be finished as quickly as possible. This operation could be:
			//  * a GPU copy to another buffer surface for later processing (such as a staging surface for mapping to CPU memory)
			//  * a GPU encode operation
			//  * a GPU VPBlt to another surface
			//  * a GPU custom compute shader encode operation
			// ==============================

			// We have finished processing this frame hence we release the reference on it.
			// If the driver forgets to release the reference to the surface, it will be leaked which results in the
			// surfaces being left around after swapchain is destroyed.
			// NOTE: Although in this sample we release reference to the surface here; the driver still
			// owns the Buffer.MetaData.pSurface surface until IddCxSwapChainReleaseAndAcquireBuffer returns
			// S_OK and gives us a new frame, a driver may want to use the surface in future to re-encode the desktop
			// for better quality if there is no new frame for a while
			AcquiredBuffer.Reset();

			// Indicate to OS that we have finished inital processing of the frame, it is a hint that
			// OS could start preparing another frame
			hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
			if (FAILED(hr))
			{
				break;
			}

			// ==============================
			// TODO: Report frame statistics once the asynchronous encode/send work is completed
			//
			// Drivers should report information about sub-frame timings, like encode time, send time, etc.
			// ==============================
			// IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
		}
		else
		{
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
	m_WdfDevice(WdfDevice)
{
	m_Adapter = {};
	for (size_t i = 0; i < IDD_SAMPLE_MONITOR_COUNT; i++) {
		freeConnectorSlots.push(i);
	}
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
	// ==============================
	// TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
	// numbers are used for telemetry and may be displayed to the user in some situations.
	//
	// This is also where static per-adapter capabilities are determined.
	// ==============================

	IDDCX_ADAPTER_CAPS AdapterCaps = {};
	AdapterCaps.Size = sizeof(AdapterCaps);

	if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
		AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16 | IDDCX_ADAPTER_FLAGS_REMOTE_ALL_TARGET_MODES_MONITOR_COMPATIBLE;
	}

	// Declare basic feature support for the adapter (required)
	AdapterCaps.MaxMonitorsSupported = IDD_SAMPLE_MONITOR_COUNT;
	AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
	AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

	// Declare your device strings for telemetry (required)
	AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"SudoMaker Virtual Display Adapter";
	AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"SudoMaker";
	AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"SudoVDA";

	// Declare your hardware and firmware versions (required)
	IDDCX_ENDPOINT_VERSION Version = {};
	Version.Size = sizeof(Version);
	Version.MajorVer = 1;
	AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
	AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

	// Initialize a WDF context that can store a pointer to the device context object
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	// Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	if (NT_SUCCESS(Status))
	{
		// Store a reference to the WDF adapter handle
		m_Adapter = AdapterInitOut.AdapterObject;

		// Store the device context object into the WDF object context
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		pContext->pContext = this;
	}
}

NTSTATUS IndirectDeviceContext::CreateMonitor(IndirectMonitorContext*& pMonitorContext, uint8_t* edidData, const GUID& containerId, const VirtualMonitorMode& preferredMode) {
	// ==============================
	// TODO: In a real driver, the EDID should be retrieved dynamically from a connected physical monitor. The EDIDs
	// provided here are purely for demonstration.
	// Monitor manufacturers are required to correctly fill in physical monitor attributes in order to allow the OS
	// to optimize settings like viewing distance and scale factor. Manufacturers should also use a unique serial
	// number every single device to ensure the OS can tell the monitors apart.
	// ==============================

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

	// In the sample driver, we report a monitor right away but a real driver would do this when a monitor connection event occurs
	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = (UINT)freeConnectorSlots.front();

	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	MonitorInfo.MonitorDescription.DataSize = sizeof(edid_base);
	MonitorInfo.MonitorDescription.pData = edidData;
	MonitorInfo.MonitorContainerId = containerId;

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	// Create a monitor object with the specified monitor descriptor
	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (NT_SUCCESS(Status))
	{
		freeConnectorSlots.pop();
		// Create a new monitor context object and attach it to the Idd monitor object
		auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
		pMonitorContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);
		pMonitorContextWrapper->pContext = pMonitorContext;

		pMonitorContext->monitorGuid = containerId;
		pMonitorContext->connectorId = MonitorInfo.ConnectorIndex;
		pMonitorContext->pEdidData = edidData;
		pMonitorContext->preferredMode = preferredMode;
		pMonitorContext->m_Adapter = m_Adapter;

		// Tell the OS that the monitor has been plugged in
		IDARG_OUT_MONITORARRIVAL ArrivalOut;
		Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
		if (NT_SUCCESS(Status)) {
			pMonitorContext->adapterLuid = ArrivalOut.OsAdapterLuid;
			pMonitorContext->targetId = ArrivalOut.OsTargetId;
		}
	} else {
		// Avoid memory leak
		free(edidData);
	}

	return Status;
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) :
	m_Monitor(Monitor)
{
	// Store context for later use
	monitorCtxList.emplace_back(this);
}

IndirectMonitorContext::~IndirectMonitorContext()
{
	m_ProcessingThread.reset();
	if (pEdidData && pEdidData != edid_base) {
		free(pEdidData);
	}
}

IDDCX_MONITOR IndirectMonitorContext::GetMonitor() const {
	return m_Monitor;
}

void IndirectMonitorContext::AssignSwapChain(const IDDCX_MONITOR& MonitorObject, const IDDCX_SWAPCHAIN& SwapChain, const LUID& RenderAdapter, const HANDLE& NewFrameEvent)
{
	m_ProcessingThread.reset();

	auto Device = make_shared<Direct3DDevice>(RenderAdapter);
	if (FAILED(Device->Init()))
	{
		// It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
		// swap-chain and try again.
		WdfObjectDelete(SwapChain);
	}
	else
	{
		// Create a new swap-chain processing thread
		m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));

		//create an event to get notified new cursor data
		HANDLE mouseEvent = CreateEventA(
			nullptr, //TODO set proper SECURITY_ATTRIBUTES
			false,
			false,
			"arbitraryMouseEventName");;

		if (!mouseEvent)
		{
			//do error handling
			return;
		}

		//set up cursor capabilities
		IDDCX_CURSOR_CAPS cursorInfo = {};
		cursorInfo.Size = sizeof(cursorInfo);
		cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL; //TODO play around with XOR cursors
		cursorInfo.AlphaCursorSupport = true;
		cursorInfo.MaxX = 64; //TODO figure out correct maximum value
		cursorInfo.MaxY = 64; //TODO figure out correct maximum value

		//prepare IddCxMonitorSetupHardwareCursor arguments
		IDARG_IN_SETUP_HWCURSOR hwCursor = {};
		hwCursor.CursorInfo = cursorInfo;
		hwCursor.hNewCursorDataAvailable = mouseEvent; //this event will be called when new cursor data is available

		NTSTATUS Status = IddCxMonitorSetupHardwareCursor(
			MonitorObject, //handle to the monitor we want to enable hardware mouse on
			&hwCursor
		);

		if (FAILED(Status))
		{
			//do error handling
		}
	}
}

void IndirectMonitorContext::UnassignSwapChain()
{
	// Stop processing the last swap-chain
	m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

// void IndirectDeviceContext::CreateMonitor() {
//  auto connectorIndex = freeConnectorSlots.front();
// 	std::string idx = std::to_string(connectorIndex);
// 	std::string serialStr = "VDD2408";
// 	serialStr += idx;
// 	std::string dispName = "SudoVDD #";
// 	dispName += idx;
// 	GUID containerId;
// 	CoCreateGuid(&containerId);
// 	uint8_t* edidData = generate_edid(0xAA55BB01 + connectorIndex, serialStr.c_str(), dispName.c_str());

// 	VirtualMonitorInfo mInfo = {
// 		containerId,
// 		edidData,
// 		{3000 + connectorIndex * 2, 2120 + connectorIndex, 120},
// 		nullptr
// 	};

//  IndirectMonitorContext*& pMonitorContext;
// 	CreateMonitor(pMonitorContext, edidData, containerId, {});
// }

_Use_decl_annotations_
NTSTATUS IddSampleAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
	// UNREFERENCED_PARAMETER(AdapterObject);
	// UNREFERENCED_PARAMETER(pInArgs);

	if (NT_SUCCESS(pInArgs->AdapterInitStatus)) {
		if (preferredAdapterChanged) {
			IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{preferredAdapterLuid};
			IddCxAdapterSetRenderAdapter(AdapterObject, &inArgs);
			preferredAdapterChanged = false;
		}
	}

	return pInArgs->AdapterInitStatus;

	// auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
	// if (NT_SUCCESS(pInArgs->AdapterInitStatus))
	// {
	// 	for (size_t i = 0; i < 3; i++) {
	// 		pDeviceContextWrapper->pContext->CreateMonitor();
	// 	}
	// }

	// return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	// For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

	// ==============================
	// TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
	// through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
	// should be turned off).
	// ==============================

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleAdapterCommitModes2(
	IDDCX_ADAPTER AdapterObject,
	const IDARG_IN_COMMITMODES2* pInArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
	// this sample driver, we hard-code the EDID, so this function can generate known modes.
	// ==============================

	if (pInArgs->MonitorDescription.DataSize != sizeof(edid_base))
		return STATUS_INVALID_PARAMETER;

	pOutArgs->MonitorModeBufferOutputCount = std::size(s_DefaultModes);

	VirtualMonitorMode* pPreferredMode = nullptr;

	for (auto &it: monitorCtxList) {
		if (memcmp(pInArgs->MonitorDescription.pData, it->pEdidData, sizeof(edid_base)) == 0) {
			if (it->preferredMode.Width) {
				pOutArgs->MonitorModeBufferOutputCount += 1;
				pPreferredMode = &it->preferredMode;
			}
			break;
		}
	}

	if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++)
		{
			pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
				s_DefaultModes[ModeIndex].Width,
				s_DefaultModes[ModeIndex].Height,
				s_DefaultModes[ModeIndex].VSync,
				IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
			);
		}

		pOutArgs->PreferredMonitorModeIdx = 1;

		if (pPreferredMode && pPreferredMode->Width) {
			pInArgs->pMonitorModes[std::size(s_DefaultModes)] = CreateIddCxMonitorMode(
				pPreferredMode->Width,
				pPreferredMode->Height,
				pPreferredMode->VSync,
				IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
			);

			pOutArgs->PreferredMonitorModeIdx = std::size(s_DefaultModes);
		}

		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
NTSTATUS IddSampleParseMonitorDescription2(
	const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
	IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs
)
{
	if (pInArgs->MonitorDescription.DataSize != sizeof(edid_base))
		return STATUS_INVALID_PARAMETER;

	pOutArgs->MonitorModeBufferOutputCount = std::size(s_DefaultModes);

	VirtualMonitorMode* pPreferredMode = nullptr;

	for (auto &it: monitorCtxList) {
		if (memcmp(pInArgs->MonitorDescription.pData, it->pEdidData, sizeof(edid_base)) == 0) {
			if (it->preferredMode.Width) {
				pOutArgs->MonitorModeBufferOutputCount += 1;
				pPreferredMode = &it->preferredMode;
			}
			break;
		}
	}

	if (pInArgs->MonitorModeBufferInputCount < pOutArgs->MonitorModeBufferOutputCount)
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}
	else
	{
		for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++)
		{
			pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode2(
				s_DefaultModes[ModeIndex].Width,
				s_DefaultModes[ModeIndex].Height,
				s_DefaultModes[ModeIndex].VSync,
				IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
			);
		}

		pOutArgs->PreferredMonitorModeIdx = 1;

		if (pPreferredMode && pPreferredMode->Width) {
			pInArgs->pMonitorModes[std::size(s_DefaultModes)] = CreateIddCxMonitorMode2(
				pPreferredMode->Width,
				pPreferredMode->Height,
				pPreferredMode->VSync,
				IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
			);

			pOutArgs->PreferredMonitorModeIdx = std::size(s_DefaultModes);
		}

		return STATUS_SUCCESS;
	}
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
	// ==============================
	// TODO: In a real driver, this function would be called to generate monitor modes for a monitor with no EDID.
	// Drivers should report modes that are guaranteed to be supported by the transport protocol and by nearly all
	// monitors (such 640x480, 800x600, or 1024x768). If the driver has access to monitor modes from a descriptor other
	// than an EDID, those modes would also be reported here.
	// ==============================

	UNREFERENCED_PARAMETER(MonitorObject);

	for (DWORD ModeIndex = 0; ModeIndex < std::size(s_DefaultModes); ModeIndex++)
	{
		pInArgs->pDefaultMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
			s_DefaultModes[ModeIndex].Width,
			s_DefaultModes[ModeIndex].Height,
			s_DefaultModes[ModeIndex].VSync,
			IDDCX_MONITOR_MODE_ORIGIN_DRIVER
		);
	}

	pOutArgs->DefaultMonitorModeBufferOutputCount = std::size(s_DefaultModes);
	pOutArgs->PreferredMonitorModeIdx = 1;

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	UNREFERENCED_PARAMETER(MonitorObject);

	vector<IDDCX_TARGET_MODE> TargetModes;

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	for (size_t i = 0; i < std::size(s_DefaultModes); i++) {
		if (i == std::size(s_DefaultModes) - 1) {

		} else {
			TargetModes.push_back(CreateIddCxTargetMode(
				s_DefaultModes[i].Width,
				s_DefaultModes[i].Height,
				s_DefaultModes[i].VSync
			));
		}
	}

	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	if (pMonitorContextWrapper->pContext->preferredMode.Width) {
		TargetModes.push_back(CreateIddCxTargetMode(
			pMonitorContextWrapper->pContext->preferredMode.Width,
			pMonitorContextWrapper->pContext->preferredMode.Height,
			pMonitorContextWrapper->pContext->preferredMode.VSync
		));
	}

	pOutArgs->TargetModeBufferOutputCount = (UINT) TargetModes.size();

	if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
	{
		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorQueryModes2(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES2* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	UNREFERENCED_PARAMETER(MonitorObject);

	vector<IDDCX_TARGET_MODE2> TargetModes;

	// Create a set of modes supported for frame processing and scan-out. These are typically not based on the
	// monitor's descriptor and instead are based on the static processing capability of the device. The OS will
	// report the available set of modes for a given output as the intersection of monitor modes with target modes.

	for (size_t i = 0; i < std::size(s_DefaultModes); i++) {
		if (i == std::size(s_DefaultModes) - 1) {

		} else {
			TargetModes.push_back(CreateIddCxTargetMode2(
				s_DefaultModes[i].Width,
				s_DefaultModes[i].Height,
				s_DefaultModes[i].VSync
			));
		}
	}

	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	if (pMonitorContextWrapper->pContext->preferredMode.Width) {
		TargetModes.push_back(CreateIddCxTargetMode2(
			pMonitorContextWrapper->pContext->preferredMode.Width,
			pMonitorContextWrapper->pContext->preferredMode.Height,
			pMonitorContextWrapper->pContext->preferredMode.VSync
		));
	}

	pOutArgs->TargetModeBufferOutputCount = (UINT) TargetModes.size();

	if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
	{
		copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);

	if (preferredAdapterChanged && memcmp(&pInArgs->RenderAdapterLuid, &preferredAdapterLuid, sizeof(LUID))) {
		IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{preferredAdapterLuid};
		IddCxAdapterSetRenderAdapter(pMonitorContextWrapper->pContext->m_Adapter, &inArgs);
		preferredAdapterChanged = false;
		return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
	}

	pMonitorContextWrapper->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	pMonitorContextWrapper->pContext->UnassignSwapChain();
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleAdapterQueryTargetInfo(
	IDDCX_ADAPTER AdapterObject,
	IDARG_IN_QUERYTARGET_INFO* pInArgs,
	IDARG_OUT_QUERYTARGET_INFO* pOutArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);
	pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE;
	pOutArgs->DitheringSupport.Rgb = IDDCX_BITS_PER_COMPONENT_8 | IDDCX_BITS_PER_COMPONENT_10;

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorSetDefaultHdrMetadata(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorSetGammaRamp(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_SET_GAMMARAMP* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

#pragma endregion

VOID IddSampleIoDeviceControl(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode
)
{
	// Reset watchdog
	watchdogCountdown = watchdogTimeout;

	NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
	size_t bytesReturned = 0;

	auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);

	switch (IoControlCode) {
	case IOCTL_ADD_VIRTUAL_DISPLAY: {
		if (freeConnectorSlots.empty()) {
			Status = STATUS_TOO_MANY_NODES;
			break;
		}

		if (InputBufferLength < sizeof(VIRTUAL_DISPLAY_PARAMS) || OutputBufferLength < sizeof(VIRTUAL_DISPLAY_OUTPUT)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		PVIRTUAL_DISPLAY_PARAMS params;
		PVIRTUAL_DISPLAY_OUTPUT output;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(VIRTUAL_DISPLAY_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(VIRTUAL_DISPLAY_OUTPUT), (PVOID*)&output, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		// Validate and add the virtual display
		if (params->Width > 0 && params->Height > 0 && params->RefreshRate > 0) {
			std::lock_guard<std::mutex> lg(monitorListOp);

			IndirectMonitorContext* pMonitorContext;
			uint8_t* edidData = generate_edid(params->MonitorGuid.Data1, params->SerialNumber, params->DeviceName);
			Status = pDeviceContextWrapper->pContext->CreateMonitor(pMonitorContext, edidData, params->MonitorGuid, {params->Width, params->Height, params->RefreshRate});

			if (!NT_SUCCESS(Status)) {
				break;
			}

			output->AdapterLuid = pMonitorContext->adapterLuid;
			output->TargetId = pMonitorContext->targetId;
			bytesReturned = sizeof(VIRTUAL_DISPLAY_OUTPUT);
		}
		else {
			Status = STATUS_INVALID_PARAMETER;
		}

		break;
	}
	case IOCTL_REMOVE_VIRTUAL_DISPLAY: {
		if (InputBufferLength < sizeof(VIRTUAL_DISPLAY_REMOVE_PARAMS)) {
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}

		PVIRTUAL_DISPLAY_REMOVE_PARAMS params;
		Status = WdfRequestRetrieveInputBuffer(Request, sizeof(VIRTUAL_DISPLAY_REMOVE_PARAMS), (PVOID*)&params, NULL);
		if (!NT_SUCCESS(Status)) {
			break;
		}

		Status = STATUS_NOT_FOUND;

		std::lock_guard<std::mutex> lg(monitorListOp);

		for (auto it = monitorCtxList.begin(); it != monitorCtxList.end(); ++it) {
			auto* ctx = *it;
			if (ctx->monitorGuid == params->MonitorGuid) {
				// Remove the monitor
				freeConnectorSlots.push(ctx->connectorId);
				IddCxMonitorDeparture(ctx->GetMonitor());
				monitorCtxList.erase(it);
				Status = STATUS_SUCCESS;
				break;
			}
		}

		break;
	}
	case IOCTL_DRIVER_PING: {
		Status = STATUS_SUCCESS;
		break;
	}
	default:
		break;
	}

	WdfRequestCompleteWithInformation(Request, Status, bytesReturned);
}
