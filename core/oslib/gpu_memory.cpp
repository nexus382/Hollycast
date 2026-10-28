/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "gpu_memory.h"

#if defined(_WIN32) && defined(HAVE_D3D11)
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <future>
#include <mutex>
#include <utility>
#include <vector>
#endif

namespace gpu_memory {

#if defined(_WIN32) && defined(HAVE_D3D11)
namespace {

using Microsoft::WRL::ComPtr;

struct Adapter {
	std::uint32_t index;
	ComPtr<IDXGIAdapter3> dxgi;
};

struct AdapterCache {
	std::mutex mutex;
	ComPtr<IDXGIFactory1> factory;
	std::vector<Adapter> adapters;
	std::future<std::pair<ComPtr<IDXGIFactory1>, std::vector<Adapter>>> refresh;

	bool refreshIfNeeded()
	{
		try
		{
			if (refresh.valid())
			{
				if (refresh.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
					return false;
				auto result = refresh.get();
				factory = std::move(result.first);
				adapters = std::move(result.second);
			}
			if (factory != nullptr && factory->IsCurrent())
				return true;

			// Factory creation and adapter discovery can take a frame or longer.
			// Publish a completed snapshot without waiting or holding this mutex
			// in the worker. A single task also covers display/adapter changes.
			refresh = std::async(std::launch::async, [] {
				ComPtr<IDXGIFactory1> newFactory;
				std::vector<Adapter> newAdapters;
				if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&newFactory))))
				{
					for (UINT index = 0;; ++index)
					{
						ComPtr<IDXGIAdapter1> adapter;
						if (newFactory->EnumAdapters1(index, &adapter) != S_OK)
							break;

						DXGI_ADAPTER_DESC1 description {};
						if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
							continue;

						ComPtr<IDXGIAdapter3> adapter3;
						if (SUCCEEDED(adapter.As(&adapter3)))
							newAdapters.push_back({ index, std::move(adapter3) });
					}
				}
				return std::make_pair(std::move(newFactory), std::move(newAdapters));
			});
		}
		catch (const std::exception&)
		{
			// Diagnostics must remain optional if a worker cannot be started or
			// completed. The next scheduled sample can retry discovery.
		}
		return false;
	}
};

AdapterCache adapterCache;

} // namespace
#endif

Stats queryCurrentProcess()
{
#if defined(_WIN32) && defined(HAVE_D3D11)
	Stats stats;
	std::lock_guard<std::mutex> lock(adapterCache.mutex);
	if (!adapterCache.refreshIfNeeded())
		return stats;
	for (const Adapter& adapter : adapterCache.adapters)
	{
		// Report every hardware adapter instead of guessing which adapter the
		// renderer uses. Each value comes directly from DXGI for this process.
		DXGI_QUERY_VIDEO_MEMORY_INFO info {};
		if (SUCCEEDED(adapter.dxgi->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
			stats.adapters.push_back({ adapter.index, static_cast<std::uint64_t>(info.CurrentUsage),
				static_cast<std::uint64_t>(info.Budget) });
	}
	return stats;
#endif
	return {};
}

} // namespace gpu_memory
