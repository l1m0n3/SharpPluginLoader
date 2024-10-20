#include "D3DModule.h"

#include <dxgi1_4.h>

#include "CoreClr.h"
#include "Config.h"
#include "Log.h"
#include "NativePluginFramework.h"

#include <Windows.h>
#include <imgui_impl.h>

#include "imgui_impl_dx12.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <filesystem>
#include <thread>

#include "ChunkModule.h"
#include "HResultHandler.h"
#include "LoaderConfig.h"
#include "PatternScan.h"

// DirectXTK12 References SerializeRootSignature so we need to link this
#pragma comment(lib, "d3d12.lib")

void D3DModule::initialize(CoreClr* coreclr) {
    if (!preloader::LoaderConfig::get().get_imgui_rendering_enabled()) {
        dlog::debug("Skipping D3D module initialization because imgui rendering is disabled");
        return;
    }

    // Directory for delay loaded DLLs
    AddDllDirectory(TEXT("nativePC/plugins/CSharp/Loader"));

    m_core_render = coreclr->get_method<void()>(
        config::SPL_CORE_ASSEMBLY_NAME,
        L"SharpPluginLoader.Core.Rendering.Renderer",
        L"Render"
    );
    m_core_imgui_render = coreclr->get_method<ImDrawData * ()>(
        config::SPL_CORE_ASSEMBLY_NAME,
        L"SharpPluginLoader.Core.Rendering.Renderer",
        L"ImGuiRender"
    );
    m_core_initialize_imgui = coreclr->get_method<ImGuiContext*(MtSize, MtSize, bool, const char*)>(
        config::SPL_CORE_ASSEMBLY_NAME,
        L"SharpPluginLoader.Core.Rendering.Renderer",
        L"Initialize"
    );
    m_core_get_custom_fonts = coreclr->get_method<int(CustomFont**)>(
        config::SPL_CORE_ASSEMBLY_NAME,
        L"SharpPluginLoader.Core.Rendering.Renderer",
        L"GetCustomFonts"
    );
    m_core_resolve_custom_fonts = coreclr->get_method<void()>(
        config::SPL_CORE_ASSEMBLY_NAME,
        L"SharpPluginLoader.Core.Rendering.Renderer",
        L"ResolveCustomFonts"
    );
    m_get_singleton = coreclr->get_method<void* (const char*)>(
        config::SPL_CORE_ASSEMBLY_NAME,
        L"SharpPluginLoader.Core.SingletonManager",
        L"GetSingletonNative"
    );

    const auto play = (void*)NativePluginFramework::get_repository_address("GUITitle:Play");
    m_title_menu_ready_hook = safetyhook::create_inline(play, title_menu_ready_hook);

    coreclr->add_internal_call("LoadTexture", (void*)load_texture);
    coreclr->add_internal_call("UnloadTexture", (void*)unload_texture);
    coreclr->add_internal_call("RegisterTexture", (void*)register_texture);
}

void D3DModule::shutdown() {
    m_d3d_present_hook.reset();

    if (m_is_d3d12) {
        m_d3d_execute_command_lists_hook.reset();
        m_d3d_signal_hook.reset();
    }
}

void D3DModule::common_initialize() {
    const uintptr_t callIsD3D12 = PatternScanner::find_first(
        Pattern::from_string("05 7D 14 00 4C 8B 8D D8 08 00 00 84 C0 0F B6 85 F0 08 00 00")
    );
    const auto offset = *(int*)callIsD3D12;
    const auto isD3D12 = (bool(*)())(callIsD3D12 + 4 + offset);
    m_is_d3d12 = isD3D12();
    dlog::debug("Found cD3DRender::isD3D12 at {:p}", (void*)isD3D12);

    m_title_menu_ready_hook.reset();

    dlog::debug("Initializing D3D module for {}", m_is_d3d12 ? "D3D12" : "D3D11");

    const auto game_window_name = std::format("MONSTER HUNTER: WORLD({})", NativePluginFramework::get_game_revision());
    dlog::debug("Looking for game window: {}", game_window_name);

    m_game_window = FindWindowA(nullptr, game_window_name.c_str());
    if (!m_game_window) {
        dlog::error("Failed to find game window ({})", GetLastError());
        return;
    }

    const auto window_class = new WNDCLASSEX;
    window_class->cbSize = sizeof(WNDCLASSEX);
    window_class->style = CS_HREDRAW | CS_VREDRAW;
    window_class->lpfnWndProc = DefWindowProc;
    window_class->cbClsExtra = 0;
    window_class->cbWndExtra = 0;
    window_class->hInstance = GetModuleHandle(nullptr);
    window_class->hIcon = nullptr;
    window_class->hCursor = nullptr;
    window_class->hbrBackground = nullptr;
    window_class->lpszMenuName = nullptr;
    window_class->lpszClassName = TEXT("SharpPluginLoader");
    window_class->hIconSm = nullptr;

    if (!RegisterClassEx(window_class)) {
        dlog::error("Failed to register window class ({})", GetLastError());
        return;
    }

    m_temp_window_class = window_class;

    m_temp_window = CreateWindow(
        window_class->lpszClassName,
        TEXT("SharpPluginLoader DX Hook"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        100, 100,
        nullptr,
        nullptr,
        window_class->hInstance,
        nullptr
    );

    if (!m_temp_window) {
        dlog::error("Failed to create temporary window ({})", GetLastError());
        return;
    }

    if (m_is_d3d12) {
        initialize_for_d3d12_alt();
    } else {
        initialize_for_d3d11_alt();
    }

    DestroyWindow(m_temp_window);
    UnregisterClass(window_class->lpszClassName, window_class->hInstance);
}

void D3DModule::initialize_for_d3d12() {
    HMODULE dxgi;

    if ((dxgi = GetModuleHandleA("dxgi.dll")) == nullptr) {
        dlog::error("Failed to find dxgi.dll");
        return;
    }

    if ((m_d3d12_module = GetModuleHandleA("d3d12.dll")) == nullptr) {
        dlog::error("Failed to find d3d12.dll");
        return;
    }

    decltype(CreateDXGIFactory)* create_dxgi_factory;
    if ((create_dxgi_factory = (decltype(create_dxgi_factory))GetProcAddress(dxgi, "CreateDXGIFactory")) == nullptr) {
        dlog::error("Failed to find CreateDXGIFactory");
        return;
    }

    IDXGIFactory* factory;
    if (FAILED(create_dxgi_factory(IID_PPV_ARGS(&factory)))) {
        dlog::error("Failed to create DXGI factory");
        return;
    }

    IDXGIAdapter* adapter;
    if (FAILED(factory->EnumAdapters(0, &adapter))) {
        dlog::error("Failed to enumerate DXGI adapters");
        return;
    }

    decltype(D3D12CreateDevice)* d3d12_create_device;
    if ((d3d12_create_device = (decltype(d3d12_create_device))GetProcAddress(m_d3d12_module, "D3D12CreateDevice")) == nullptr) {
        dlog::error("Failed to find D3D12CreateDevice");
        return;
    }

    ID3D12Device* device;
    if (FAILED(d3d12_create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        dlog::error("Failed to create D3D12 device");
        return;
    }

    constexpr D3D12_COMMAND_QUEUE_DESC queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = 0,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0
    };

    ID3D12CommandQueue* command_queue;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue)))) {
        dlog::error("Failed to create D3D12 command queue");
        return;
    }

    ID3D12CommandAllocator* command_allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator)))) {
        dlog::error("Failed to create D3D12 command allocator");
        return;
    }

    ID3D12CommandList* command_list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator, nullptr, IID_PPV_ARGS(&command_list)))) {
        dlog::error("Failed to create D3D12 command list");
        return;
    }

    constexpr DXGI_RATIONAL refresh_rate = {
        .Numerator = 60,
        .Denominator = 1
    };

    DXGI_MODE_DESC buffer_desc = {
        .Width = 100,
        .Height = 100,
        .RefreshRate = refresh_rate,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
        .Scaling = DXGI_MODE_SCALING_UNSPECIFIED
    };

    constexpr DXGI_SAMPLE_DESC sample_desc = {
        .Count = 1,
        .Quality = 0
    };

    DXGI_SWAP_CHAIN_DESC sd = {
        .BufferDesc = buffer_desc,
        .SampleDesc = sample_desc,
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = 2,
        .OutputWindow = m_temp_window,
        .Windowed = TRUE,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
    };

    IDXGISwapChain* swap_chain;
    if (FAILED(factory->CreateSwapChain(command_queue, &sd, &swap_chain))) {
        dlog::error("Failed to create DXGI swap chain");
        return;
    }

    // SwapChainVFT[8]: Present
    // SwapChainVFT[13]: ResizeBuffers
    // CommandQueueVFT[10]: ExecuteCommandLists
    // CommandQueueVFT[14]: Signal

    const auto swap_chain_vft = *(void***)swap_chain;
    const auto command_queue_vft = *(void***)command_queue;

    const auto present = swap_chain_vft[8];
    const auto resize_buffers = swap_chain_vft[13];
    const auto execute_command_lists = command_queue_vft[10];
    const auto signal = command_queue_vft[14];
    
    m_d3d_present_hook = safetyhook::create_inline(present, d3d12_present_hook);
    m_d3d_execute_command_lists_hook = safetyhook::create_inline(execute_command_lists, d3d12_execute_command_lists_hook);
    m_d3d_signal_hook = safetyhook::create_inline(signal, d3d12_signal_hook);
    m_d3d_resize_buffers_hook = safetyhook::create_inline(resize_buffers, d3d_resize_buffers_hook);

    device->Release();
    command_queue->Release();
    command_allocator->Release();
    command_list->Release();
    swap_chain->Release();
    factory->Release();
}

void D3DModule::initialize_for_d3d11() {
    if ((m_d3d11_module = GetModuleHandleA("d3d11.dll")) == nullptr) {
        dlog::error("Failed to find d3d11.dll");
        return;
    }

    decltype(D3D11CreateDeviceAndSwapChain)* d3d11_create_device_and_swap_chain;
    if ((d3d11_create_device_and_swap_chain = (decltype(d3d11_create_device_and_swap_chain))GetProcAddress(m_d3d11_module, "D3D11CreateDeviceAndSwapChain")) == nullptr) {
        dlog::error("Failed to find D3D11CreateDeviceAndSwapChain");
        return;
    }

    constexpr D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    D3D_FEATURE_LEVEL feature_level;

    constexpr DXGI_RATIONAL refresh_rate = {
        .Numerator = 60,
        .Denominator = 1
    };

    constexpr DXGI_MODE_DESC buffer_desc = {
        .Width = 100,
        .Height = 100,
        .RefreshRate = refresh_rate,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
        .Scaling = DXGI_MODE_SCALING_UNSPECIFIED
    };

    constexpr DXGI_SAMPLE_DESC sample_desc = {
        .Count = 1,
        .Quality = 0
    };

    DXGI_SWAP_CHAIN_DESC sd = {
        .BufferDesc = buffer_desc,
        .SampleDesc = sample_desc,
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = 1,
        .OutputWindow = m_temp_window,
        .Windowed = TRUE,
        .SwapEffect = DXGI_SWAP_EFFECT_DISCARD,
        .Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
    };

    IDXGISwapChain* swap_chain;
    ID3D11Device* device;
    ID3D11DeviceContext* device_context;

    if (FAILED(d3d11_create_device_and_swap_chain(
        nullptr, 
        D3D_DRIVER_TYPE_HARDWARE, 
        nullptr, 0, 
        feature_levels, 
        _countof(feature_levels), 
        D3D11_SDK_VERSION, 
        &sd, &swap_chain, &device, &feature_level, &device_context))) {
        dlog::error("Failed to create D3D11 device and swap chain");
        return;
    }

    // SwapChainVFT[8]: Present
    const auto swap_chain_vft = *(void***)swap_chain;
    const auto present = swap_chain_vft[8];

    m_d3d_present_hook = safetyhook::create_inline(present, d3d11_present_hook);
}

void D3DModule::initialize_for_d3d12_alt() {
    const auto present_call = NativePluginFramework::get_repository_address("D3DRender12:SwapChainPresentCall");
    if (!present_call) {
        dlog::error("Failed to find SwapChainPresentCall");
        return;
    }

    if ((m_d3d12_module = GetModuleHandleA("d3d12.dll")) == nullptr) {
        dlog::error("Failed to find d3d12.dll");
        return;
    }

    m_d3d_present_hook_alt = safetyhook::create_mid(present_call, [](safetyhook::Context& ctx) {
        const auto self = NativePluginFramework::get_module<D3DModule>();
        const auto prm = NativePluginFramework::get_module<PrimitiveRenderingModule>();
        const auto& config = preloader::LoaderConfig::get();

        const auto swap_chain = (IDXGISwapChain*)ctx.rcx;

        if (self->m_is_inside_present) {
            return;
        }

        self->m_is_inside_present = true;

        if (!self->m_is_initialized) {
            self->d3d12_initialize_imgui(swap_chain);

            if (!self->m_texture_manager) {
                self->m_texture_manager = std::make_unique<TextureManager>(
                    self->m_d3d12_device,
                    self->m_d3d12_command_queue,
                    self->m_d3d12_srv_heap
                );
            }

            if (config.get_primitive_rendering_enabled()) {
                prm->late_init(self.get(), swap_chain);
            }
        }

        if (!self->m_d3d12_command_queue) {
            self->m_is_inside_present = false;
            return;
        }

        const auto facility = (uintptr_t)self->m_get_singleton("sFacility");

        // Check if Steamworks is active. This is a very hacky fix for the AutoSteamworks app,
        // which sometimes sends invalid input events that trip up ImGui.
        // So we just disable ImGui rendering when Steamworks is active. Ideally we should
        // check if the app is even running, but whatever. This is (probably) a temporary fix.
        // +0x348 is the offset to cSteamControl, +0x444 is the offset from that to the mState field.
        if (facility && *(u32*)(facility + 0x348 + 0x444) > 5) {
            self->m_is_inside_present = false;
            return;
        }

        self->d3d12_present_hook_core(swap_chain, prm);
        self->m_is_inside_present = false;
    });

    const auto render_singleton = (uintptr_t)m_get_singleton("sMhRender");
    const auto renderer = *(uintptr_t*)(render_singleton + 0x78);

    m_d3d12_command_queue = *(ID3D12CommandQueue**)(renderer + 0x20);
    const auto swap_chain = *(IDXGISwapChain3**)(renderer + 0x1470);
    const auto swap_chain_vft = *(void***)swap_chain;
    const auto cmd_queue_vft = *(void***)m_d3d12_command_queue;

    const auto resize_buffers = swap_chain_vft[13];
    const auto signal = cmd_queue_vft[14];

    dlog::debug("D3D12 Command Queue found at {:p}", (void*)m_d3d12_command_queue);

    m_d3d_resize_buffers_hook = safetyhook::create_inline(resize_buffers, d3d_resize_buffers_hook);
    m_d3d_signal_hook = safetyhook::create_inline(signal, d3d12_signal_hook);
}

void D3DModule::initialize_for_d3d11_alt() {
    const auto present_call = NativePluginFramework::get_repository_address("D3DRender11:SwapChainPresentCall");
    if (!present_call) {
        dlog::error("Failed to find SwapChainPresentCall");
        return;
    }

    m_d3d_present_hook_alt = safetyhook::create_mid(present_call, [](safetyhook::Context& ctx) {
        const auto self = NativePluginFramework::get_module<D3DModule>();
        const auto prm = NativePluginFramework::get_module<PrimitiveRenderingModule>();
        const auto& config = preloader::LoaderConfig::get();

        const auto swap_chain = (IDXGISwapChain*)ctx.rcx;

        if (self->m_is_inside_present) {
            return;
        }

        self->m_is_inside_present = true;

        if (!self->m_is_initialized) {
            self->d3d11_initialize_imgui(swap_chain);

            if (!self->m_texture_manager) {
                self->m_texture_manager = std::make_unique<TextureManager>(self->m_d3d11_device, self->m_d3d11_device_context);
            }

            if (config.get_primitive_rendering_enabled()) {
                prm->late_init(self.get(), swap_chain);
            }
        }

        self->d3d11_present_hook_core(swap_chain, prm);
        self->m_is_inside_present = false;
    });
}

void D3DModule::d3d12_initialize_imgui(IDXGISwapChain* swap_chain) {
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&m_d3d12_device)))) {
        dlog::error("Failed to get D3D12 device in present hook");
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swap_chain->GetDesc(&desc))) {
        dlog::error("Failed to get DXGI swap chain description");
        return;
    }

    RECT client_rect;
    GetClientRect(desc.OutputWindow, &client_rect);

    const MtSize viewport_size = { desc.BufferDesc.Width, desc.BufferDesc.Height };
    const MtSize window_size = { 
        (u32)(client_rect.right - client_rect.left), 
        (u32)(client_rect.bottom - client_rect.top) 
    };
    
    const auto& config = preloader::LoaderConfig::get();
    const auto context = m_core_initialize_imgui(viewport_size, window_size, true, config.get_menu_key().c_str());

    igSetCurrentContext(context);

    imgui_load_fonts();

    CreateEvent(nullptr, FALSE, FALSE, nullptr);

    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    m_game_window = desc.OutputWindow;
    desc.Windowed = GetWindowLongPtr(desc.OutputWindow, GWL_STYLE) & WS_POPUP ? FALSE : TRUE;

    m_d3d12_buffer_count = desc.BufferCount;
    m_d3d12_frame_contexts.resize(desc.BufferCount, FrameContext{});

    constexpr D3D12_DESCRIPTOR_HEAP_DESC dp_imgui_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = D3D12_DESCRIPTOR_HEAP_SIZE,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        .NodeMask = 0
    };

    if (FAILED(m_d3d12_device->CreateDescriptorHeap(&dp_imgui_desc, IID_PPV_ARGS(m_d3d12_srv_heap.GetAddressOf())))) {
        dlog::error("Failed to create D3D12 descriptor heap for back buffers");
        return;
    }

    ComPtr<ID3D12CommandAllocator> command_allocator;
    if (FAILED(m_d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(command_allocator.GetAddressOf())))) {
        dlog::error("Failed to create D3D12 command allocator");
        return;
    }

    for (auto i = 0u; i < desc.BufferCount; ++i) {
        m_d3d12_frame_contexts[i].CommandAllocator = command_allocator;
    }

    if (FAILED(m_d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, 
        command_allocator.Get(), nullptr, IID_PPV_ARGS(m_d3d12_command_list.GetAddressOf())))) {
        dlog::error("Failed to create D3D12 command list");
        return;
    }

    if (FAILED(m_d3d12_command_list->Close())) {
        dlog::error("Failed to close D3D12 command list");
        return;
    }

    const D3D12_DESCRIPTOR_HEAP_DESC back_buffer_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = desc.BufferCount,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 1
    };

    if (FAILED(m_d3d12_device->CreateDescriptorHeap(&back_buffer_desc, IID_PPV_ARGS(m_d3d12_back_buffers.GetAddressOf())))) {
        dlog::error("Failed to create D3D12 descriptor heap for back buffers");
        return;
    }

    const auto rtv_descriptor_size = m_d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = m_d3d12_back_buffers->GetCPUDescriptorHandleForHeapStart();

    for (auto i = 0u; i < desc.BufferCount; ++i) {
        ComPtr<ID3D12Resource> back_buffer;
        if (FAILED(swap_chain->GetBuffer(i, IID_PPV_ARGS(back_buffer.GetAddressOf())))) {
            dlog::error("Failed to get DXGI swap chain buffer");
            return;
        }

        const auto buffer_desc = back_buffer->GetDesc();
        dlog::debug("Creating RTV for back buffer {}, with size {}x{}", i, buffer_desc.Width, buffer_desc.Height);
        
        m_d3d12_device->CreateRenderTargetView(back_buffer.Get(), nullptr, rtv_handle);
        m_d3d12_frame_contexts[i].RenderTargetDescriptor = rtv_handle;
        m_d3d12_frame_contexts[i].RenderTarget = back_buffer;

        rtv_handle.ptr += rtv_descriptor_size;
    }

    if (!ImGui_ImplWin32_Init(m_game_window)) {
        dlog::error("Failed to initialize ImGui Win32");
        return;
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    if (!ImGui_ImplDX12_Init(m_d3d12_device, desc.BufferCount,
        DXGI_FORMAT_R8G8B8A8_UNORM, m_d3d12_srv_heap.Get(),
        m_d3d12_srv_heap->GetCPUDescriptorHandleForHeapStart(),
        m_d3d12_srv_heap->GetGPUDescriptorHandleForHeapStart())) {
        dlog::error("Failed to initialize ImGui D3D12");
        return;
    }

    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        dlog::error("Failed to create ImGui D3D12 device objects");
        return;
    }

    if (GetWindowLongPtr(m_game_window, GWLP_WNDPROC) != (LONG_PTR)my_window_proc) {
        m_game_window_proc = (WNDPROC)SetWindowLongPtr(m_game_window, GWLP_WNDPROC, (LONG_PTR)my_window_proc);
    }

    m_is_initialized = true;

    dlog::debug("Initialized D3D12");
}

void D3DModule::d3d11_initialize_imgui(IDXGISwapChain* swap_chain) {
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&m_d3d11_device)))) {
        dlog::error("Failed to get D3D11 device in present hook");
        return;
    }

    m_d3d11_device->GetImmediateContext(&m_d3d11_device_context);

    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(swap_chain->GetDesc(&desc))) {
        dlog::error("Failed to get DXGI swap chain description");
        return;
    }

    RECT client_rect;
    GetClientRect(desc.OutputWindow, &client_rect);

    const MtSize viewport_size = { desc.BufferDesc.Width, desc.BufferDesc.Height };
    const MtSize window_size = { 
        (u32)(client_rect.right - client_rect.left), 
        (u32)(client_rect.bottom - client_rect.top) 
    };

    const auto& config = preloader::LoaderConfig::get();
    const auto context = m_core_initialize_imgui(viewport_size, window_size, false, config.get_menu_key().c_str());
    igSetCurrentContext(context);

    imgui_load_fonts();

    if (!ImGui_ImplWin32_Init(m_game_window)) {
        dlog::error("Failed to initialize ImGui Win32");
        return;
    }

    if (!ImGui_ImplDX11_Init(m_d3d11_device, m_d3d11_device_context)) {
        dlog::error("Failed to initialize ImGui D3D11");
        return;
    }

    m_game_window_proc = (WNDPROC)SetWindowLongPtr(m_game_window, GWLP_WNDPROC, (LONG_PTR)my_window_proc);
    m_is_initialized = true;

    dlog::debug("Initialized D3D11");
}

void D3DModule::d3d12_deinitialize_imgui() {
    dlog::debug("Uninitializing D3D12 ImGui");

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    m_d3d12_frame_contexts.clear();
    m_d3d12_back_buffers = nullptr;
    m_d3d12_srv_heap = nullptr;
    m_d3d12_command_list = nullptr;
    m_d3d12_fence = nullptr;
    m_d3d12_fence_value = 0;
    m_d3d12_buffer_count = 0;
}

void D3DModule::d3d11_deinitialize_imgui() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    m_d3d11_device_context = nullptr;
    m_d3d11_device = nullptr;
}

void D3DModule::imgui_load_fonts() {
    if (m_fonts_loaded) {
        return;
    }

    const auto& io = *igGetIO();
    ImFontAtlas_Clear(io.Fonts);

    CustomFont* custom_fonts;
    const int custom_font_count = m_core_get_custom_fonts(&custom_fonts);

    const auto& chunk_module = NativePluginFramework::get_module<ChunkModule>();
    const auto& default_chunk = chunk_module->request_chunk("Default");
    const auto& roboto = default_chunk->get_file("/Resources/Roboto-Medium.ttf");
    const auto& noto_sans_jp = default_chunk->get_file("/Resources/NotoSansJP-Regular.ttf");
    const auto& fa6 = default_chunk->get_file("/Resources/fa-solid-900.ttf");

    ImFontConfig* font_cfg = ImFontConfig_ImFontConfig();
    font_cfg->FontDataOwnedByAtlas = false;
    font_cfg->MergeMode = false;

    ImFontAtlas_AddFontFromMemoryTTF(io.Fonts, roboto->Contents.data(), (i32)roboto->size(), 16.0f, font_cfg, nullptr);
    font_cfg->MergeMode = true;
    ImFontAtlas_AddFontFromMemoryTTF(io.Fonts, noto_sans_jp->Contents.data(), (i32)noto_sans_jp->size(), 18.0f, font_cfg, s_japanese_glyph_ranges);
    ImFontAtlas_AddFontFromMemoryTTF(io.Fonts, fa6->Contents.data(), (i32)fa6->size(), 16.0f, font_cfg, icons_ranges);

    for (int i = 0; i < custom_font_count; ++i) {
        auto& font = custom_fonts[i];
        font.Font = ImFontAtlas_AddFontFromFileTTF(io.Fonts, font.Path, font.Size, font.Config, font.GlyphRanges);
        dlog::debug("Loaded custom font: {} - {}", font.Name, font.Path);
    }

    ImFontAtlas_Build(io.Fonts);

    ImFontConfig_destroy(font_cfg);

    m_core_resolve_custom_fonts();

    m_fonts_loaded = true;
}

TextureHandle D3DModule::register_texture(void* texture) {
    const auto& self = NativePluginFramework::get_module<D3DModule>();
    if (!self->m_texture_manager) {
        dlog::error("Cannot register texture during Buffer Resize event");
        return nullptr;
    }

    if (self->m_is_d3d12 && !self->m_d3d12_command_queue) {
        dlog::error("Cannot register texture during Buffer Resize event (D3D12)");
        return nullptr;
    }

    return self->m_texture_manager->register_texture(texture);
}

TextureHandle D3DModule::load_texture(const char* path, u32* out_width, u32* out_height) {
    const auto& self = NativePluginFramework::get_module<D3DModule>();
    if (!self->m_texture_manager) {
        dlog::error("Cannot load texture during Buffer Resize event");
        return nullptr;
    }

    if (self->m_is_d3d12 && !self->m_d3d12_command_queue) {
        dlog::error("Cannot load texture during Buffer Resize event (D3D12)");
        return nullptr;
    }

    return self->m_texture_manager->load_texture(path, out_width, out_height);
}

void D3DModule::unload_texture(TextureHandle handle) {
    const auto& self = NativePluginFramework::get_module<D3DModule>();
    if (!self->m_texture_manager) {
        return;
    }

    self->m_texture_manager->unload_texture(handle);
}

bool D3DModule::is_d3d12() {
    return m_is_d3d12;
}

void D3DModule::title_menu_ready_hook(void* gui) {
    const auto self = NativePluginFramework::get_module<D3DModule>();

    std::thread t(&D3DModule::common_initialize, self);
    self->m_title_menu_ready_hook.call(gui);
    t.join();

    self->m_title_menu_ready_hook = {};
}

template<typename T, typename F, typename ...Args>
auto invoke_if(const std::optional<std::shared_ptr<T>>& opt, F func, Args&&... args) {
    return opt.and_then(std::bind(func, std::forward<Args>(args)...));
}

HRESULT D3DModule::d3d12_present_hook(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    const auto self = NativePluginFramework::get_module<D3DModule>();
    const auto prm = NativePluginFramework::get_module<PrimitiveRenderingModule>();
    const auto& config = preloader::LoaderConfig::get();

    if (self->m_is_inside_present) {
        return self->m_d3d_present_hook.call<HRESULT>(swap_chain, sync_interval, flags);
    }

    self->m_is_inside_present = true;
    
    if (!self->m_is_initialized) {
        self->d3d12_initialize_imgui(swap_chain);
        
        if (!self->m_texture_manager) {
            self->m_texture_manager = std::make_unique<TextureManager>(
                self->m_d3d12_device, 
                self->m_d3d12_command_queue, 
                self->m_d3d12_srv_heap
            );
        }

        if (config.get_primitive_rendering_enabled()) {
            prm->late_init(self.get(), swap_chain);
        }
    }

    if (!self->m_d3d12_command_queue) {
        return self->m_d3d_present_hook.call<HRESULT>(swap_chain, sync_interval, flags);
    }

    const auto facility = (uintptr_t)self->m_get_singleton("sFacility");

    // Check if Steamworks is active. This is a very hacky fix for the AutoSteamworks app,
    // which sometimes sends invalid input events that trip up ImGui.
    // So we just disable ImGui rendering when Steamworks is active. Ideally we should
    // check if the app is even running, but whatever. This is (probably) a temporary fix.
    // +0x348 is the offset to cSteamControl, +0x444 is the offset from that to the mState field.
    if (facility && *(u32*)(facility + 0x348 + 0x444) > 5) {
        return self->m_d3d_present_hook.call<HRESULT>(swap_chain, sync_interval, flags);
    }

    self->d3d12_present_hook_core(swap_chain, prm);

    const HRESULT result = self->m_d3d_present_hook.call<HRESULT>(swap_chain, sync_interval, flags);
    self->m_is_inside_present = false;

    return result;
}

void D3DModule::d3d12_present_hook_core(IDXGISwapChain* swap_chain, const std::shared_ptr<PrimitiveRenderingModule>& prm) {
    const auto swap_chain3 = (IDXGISwapChain3*)swap_chain;
    const auto& config = preloader::LoaderConfig::get();

    if (config.get_primitive_rendering_enabled()) {
        m_core_render();
        prm->render_primitives_for_d3d12(swap_chain3, m_d3d12_command_queue);
    }

    // Start new frame
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImDrawData* draw_data = m_core_imgui_render();

    const FrameContext& frame_ctx = m_d3d12_frame_contexts[swap_chain3->GetCurrentBackBufferIndex()];
    frame_ctx.CommandAllocator->Reset();

    D3D12_RESOURCE_BARRIER barrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
        .Transition = {
            .pResource = frame_ctx.RenderTarget.Get(),
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = D3D12_RESOURCE_STATE_PRESENT,
            .StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET
        }
    };

    m_d3d12_command_list->Reset(frame_ctx.CommandAllocator.Get(), nullptr);
    m_d3d12_command_list->ResourceBarrier(1, &barrier);
    m_d3d12_command_list->OMSetRenderTargets(1, &frame_ctx.RenderTargetDescriptor, FALSE, nullptr);
    m_d3d12_command_list->SetDescriptorHeaps(1, m_d3d12_srv_heap.GetAddressOf());

    ImGui_ImplDX12_RenderDrawData(draw_data, m_d3d12_command_list.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    m_d3d12_command_list->ResourceBarrier(1, &barrier);
    m_d3d12_command_list->Close();

    m_d3d12_command_queue->ExecuteCommandLists(1, (ID3D12CommandList* const*)m_d3d12_command_list.GetAddressOf());

    if (igGetIO()->ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        igUpdatePlatformWindows();
        igRenderPlatformWindowsDefault(nullptr, m_d3d12_command_list.Get());
    }
}

void D3DModule::d3d12_execute_command_lists_hook(ID3D12CommandQueue* command_queue, UINT num_command_lists, ID3D12CommandList* const* command_lists) {
    const auto self = NativePluginFramework::get_module<D3DModule>();

    if (!self->m_d3d12_command_queue && command_queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        dlog::debug("Found D3D12 command queue");
        self->m_d3d12_command_queue = command_queue;

        if (self->m_texture_manager) {
            self->m_texture_manager->update_command_queue(command_queue);
        }
    }

    return self->m_d3d_execute_command_lists_hook.call<void>(command_queue, num_command_lists, command_lists);
}

UINT64 D3DModule::d3d12_signal_hook(ID3D12CommandQueue* command_queue, ID3D12Fence* fence, UINT64 value) {
    const auto self = NativePluginFramework::get_module<D3DModule>();

    if (self->m_d3d12_command_queue == command_queue) {
        self->m_d3d12_fence = fence;
        self->m_d3d12_fence_value = value;
    }

    return self->m_d3d_signal_hook.call<UINT64>(command_queue, fence, value);
}

HRESULT D3DModule::d3d_resize_buffers_hook(IDXGISwapChain* swap_chain, UINT buffer_count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    const auto self = NativePluginFramework::get_module<D3DModule>();
    const auto prm = NativePluginFramework::get_module<PrimitiveRenderingModule>();

    dlog::debug("ResizeBuffers called, resetting...");

    if (self->m_is_initialized) {
        self->m_is_initialized = false;
        if (self->m_is_d3d12) {
            self->d3d12_deinitialize_imgui();
        }
        else {
            self->d3d11_deinitialize_imgui();
        }
    }

    prm->shutdown();

    return self->m_d3d_resize_buffers_hook.call<HRESULT>(swap_chain, buffer_count, w, h, format, flags);
}

HRESULT D3DModule::d3d11_present_hook(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    const auto self = NativePluginFramework::get_module<D3DModule>();
    const auto prm = NativePluginFramework::get_module<PrimitiveRenderingModule>();
    const auto& config = preloader::LoaderConfig::get();

    if (self->m_is_inside_present) {
        return self->m_d3d_present_hook.call<HRESULT>(swap_chain, sync_interval, flags);
    }

    self->m_is_inside_present = true;

    if (!self->m_is_initialized) {
        self->d3d11_initialize_imgui(swap_chain);

        if (!self->m_texture_manager) {
            self->m_texture_manager = std::make_unique<TextureManager>(self->m_d3d11_device, self->m_d3d11_device_context);
        }
        
        if (config.get_primitive_rendering_enabled()) {
            prm->late_init(self.get(), swap_chain);
        }
    }

    self->d3d11_present_hook_core(swap_chain, prm);

    const auto result = self->m_d3d_present_hook.call<HRESULT>(swap_chain, sync_interval, flags);
    self->m_is_inside_present = false;

    return result;
}

void D3DModule::d3d11_present_hook_core(IDXGISwapChain* swap_chain, const std::shared_ptr<PrimitiveRenderingModule>& prm) const {
    const auto& config = preloader::LoaderConfig::get();

    if (config.get_primitive_rendering_enabled()) {
        m_core_render();
        prm->render_primitives_for_d3d11(m_d3d11_device_context);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    const auto draw_data = m_core_imgui_render();

    ImGui_ImplDX11_RenderDrawData(draw_data);

    if (igGetIO()->ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        igUpdatePlatformWindows();
        igRenderPlatformWindowsDefault(nullptr, nullptr);
    }
}


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT D3DModule::my_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const auto self = NativePluginFramework::get_module<D3DModule>();
    if (self->m_is_initialized) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
    }
    return CallWindowProc(self->m_game_window_proc, hwnd, msg, wparam, lparam);
}
