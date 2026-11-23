#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

namespace tak {
inline std::wstring quoteWindowsArgument(const std::wstring& text) {
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t c:text) {
        if(c==L'\\') {++slashes;continue;}
        result.append(slashes*(c==L'"'?2:1),L'\\');slashes=0;
        if(c==L'"')result+=L'\\';
        result+=c;
    }
    result.append(slashes*2,L'\\');result+=L'"';return result;
}
// GUI entry points receive ANSI argv from the CRT. Keep the application's
// UTF-8 path contract even when Windows user/temp directories contain Unicode.
inline int utf8Main(int (*entry)(int,char**)) {
    int count=0;
    wchar_t** wide=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!wide)return 2;
    struct Free {wchar_t** p;~Free(){LocalFree(p);}} free{wide};
    std::vector<std::string> values;values.reserve(size_t(count));
    for(int i=0;i<count;++i) {
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,nullptr,0,nullptr,nullptr);
        if(size<=0)return 2;
        std::string value(size_t(size),'\0');
        if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,value.data(),size,nullptr,nullptr))return 2;
        value.resize(size_t(size-1));values.push_back(std::move(value));
    }
    std::vector<char*> args;args.reserve(values.size()+1);
    for(auto& value:values)args.push_back(value.data());
    args.push_back(nullptr);
    return entry(count,args.data());
}
}
#endif
