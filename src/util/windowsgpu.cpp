#include "util/windowsgpu.h"
#include <cwchar>
#include <map>
#include <tuple>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <dxgi.h>
#endif

namespace tak::proc {
GpuSample windowsgpu_detail::aggregate(const std::vector<Counter>& engines,
                                      const std::vector<Counter>& dedicatedMemory,
                                      const std::vector<Adapter>& adapters) {
    using Key = std::tuple<unsigned, unsigned, unsigned, unsigned>;
    std::map<Key, double> totals;
    for (const auto& c : engines) {
        if (!c.valid || !std::isfinite(c.value) || c.value < 0) continue;
        unsigned pid=0, high=0, low=0, physical=0, engine=0; int end=0;
        if (std::swscanf(c.instance.c_str(),L"pid_%u_luid_0x%x_0x%x_phys_%u_eng_%u_engtype_%n",
                         &pid,&high,&low,&physical,&engine,&end) != 5 || end <= 0 ||
            size_t(end) >= c.instance.size()) continue;
        totals[{high,low,physical,engine}] += c.value;
    }
    GpuSample g;
    Key selected{};
    for (const auto& [key,busy] : totals) {
        const double percent = std::clamp(busy,0.0,100.0);
        if (!g.ok || percent > g.utilPct) {
            selected=key;g.utilPct=percent;g.ok=true;g.systemWide=true;
        }
    }
    if (!g.ok) return g;
    const auto [high,low,physical,engine] = selected;
    g.name="GPU (Windows busiest engine)";
    for (const auto& a : adapters)
        if (a.high==high && a.low==low) { g.name=a.name;g.memTotal=a.dedicatedBytes;break; }
    for (const auto& c : dedicatedMemory) {
        if (!c.valid || !std::isfinite(c.value) || c.value < 0) continue;
        unsigned h=0,l=0,p=0; int end=0;
        if (std::swscanf(c.instance.c_str(),L"luid_0x%x_0x%x_phys_%u%n",&h,&l,&p,&end)!=3 ||
            end<=0 || size_t(end)!=c.instance.size()) continue;
        if (h==high && l==low && p==physical && c.value<double(SIZE_MAX)) {
            g.memUsed=size_t(c.value);break;
        }
    }
    return g;
}

#if defined(_WIN32)
namespace {
std::vector<windowsgpu_detail::Counter> counters(PDH_HCOUNTER counter) {
    if (!counter) return {};
    constexpr DWORD format=PDH_FMT_DOUBLE|PDH_FMT_NOCAP100;
    for (int retry=0;retry<3;++retry) {
        DWORD bytes=0,count=0;
        if (PdhGetFormattedCounterArrayW(counter,format,&bytes,&count,nullptr)!=PDH_STATUS(PDH_MORE_DATA) ||
            bytes==0 || bytes>16*1024*1024) return {};
        std::vector<unsigned char> storage(bytes);
        auto* items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        const auto status=PdhGetFormattedCounterArrayW(counter,format,&bytes,&count,items);
        if (status==PDH_STATUS(PDH_MORE_DATA)) continue;
        if (status!=ERROR_SUCCESS || size_t(count)*sizeof(*items)>storage.size()) return {};
        std::vector<windowsgpu_detail::Counter> out;
        for (DWORD i=0;i<count;++i) {
            const auto& item=items[i];
            if (item.szName && (item.FmtValue.CStatus==PDH_CSTATUS_VALID_DATA ||
                               item.FmtValue.CStatus==PDH_CSTATUS_NEW_DATA))
                out.push_back({item.szName,item.FmtValue.doubleValue});
        }
        return out;
    }
    return {};
}
std::vector<windowsgpu_detail::Adapter> adapters() {
    std::vector<windowsgpu_detail::Adapter> out;
    IDXGIFactory1* factory=nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory)))) return out;
    for (UINT i=0;;++i) {
        IDXGIAdapter1* adapter=nullptr;
        if (FAILED(factory->EnumAdapters1(i,&adapter))) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
            const int n=WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,nullptr,0,nullptr,nullptr);
            std::string name(n>0 ? n : 0,'\0');
            if(n>0) { WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name.data(),n,nullptr,nullptr);name.pop_back(); }
            out.push_back({uint32_t(desc.AdapterLuid.HighPart),desc.AdapterLuid.LowPart,
                           std::move(name),desc.DedicatedVideoMemory});
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}
}
#endif
GpuSample windowsGpuSample() {
#if defined(_WIN32)
    struct Query { PDH_HQUERY handle=nullptr;~Query(){if(handle)PdhCloseQuery(handle);} } query;
    if (PdhOpenQueryW(nullptr,0,&query.handle)!=ERROR_SUCCESS) return {};
    PDH_HCOUNTER engine=nullptr,memory=nullptr;
    // English counter names are localized by PDH, including on non-English Windows.
    // Keep the wildcard and use the array API so every process/engine is sampled.
    if (PdhAddEnglishCounterW(query.handle,L"\\GPU Engine(*)\\Utilization Percentage",0,&engine)!=ERROR_SUCCESS)
        return {};
    PdhAddEnglishCounterW(query.handle,L"\\GPU Adapter Memory(*)\\Dedicated Usage",0,&memory);
    if (PdhCollectQueryData(query.handle)!=ERROR_SUCCESS) return {};
    Sleep(200); // This sampler runs on the stats worker, never on the rendering thread.
    if (PdhCollectQueryData(query.handle)!=ERROR_SUCCESS) return {};
    return windowsgpu_detail::aggregate(counters(engine),counters(memory),adapters());
#else
    return {};
#endif
}
}
