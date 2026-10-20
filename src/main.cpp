#include "panel.hpp"
#include <fcntl.h>
#include <io.h>
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    const bool privateFixture=argc==2 && std::wstring(argv[1])==L"--private-fixture";
    if (argc!=1 && !privateFixture) return 2;
    _setmode(_fileno(stdin),_O_BINARY); _setmode(_fileno(stdout),_O_BINARY);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        { audio::Panel panel(privateFixture); return panel.Run(); }
    } catch(const winrt::hresult_error& error) { std::cerr << winrt::to_string(error.message()) << '\n'; }
      catch(const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
