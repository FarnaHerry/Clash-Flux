// Shared desktop entry; HuxerUI selects the native platform backend.
#include <huxerui/app.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace {
LONG WINAPI DemoCrashFilter(EXCEPTION_POINTERS* info) {
  char path[MAX_PATH];
  GetTempPathA(MAX_PATH, path);
  std::strcat(path, "box-cat-demo-crash.txt");
  if (FILE* log = std::fopen(path, "ab")) {
    std::fprintf(log, "exception 0x%lx at %p\n", info->ExceptionRecord->ExceptionCode,
        info->ExceptionRecord->ExceptionAddress);
    void* frames[48];
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    IMAGEHLP_LINE64 line{.SizeOfStruct = sizeof(IMAGEHLP_LINE64)};
    for (USHORT i = 0; i < count; ++i) {
      DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
      DWORD64 displacement = 0;
      DWORD lineDisplacement = 0;
      if (SymFromAddr(process, address, &displacement, symbol)) {
        std::fprintf(log, "  %s+0x%llx", symbol->Name, displacement);
        if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line))
          std::fprintf(log, " %s:%lu", line.FileName, line.LineNumber);
        std::fputc('\n', log);
      } else {
        std::fprintf(log, "  %p\n", frames[i]);
      }
    }
    std::fclose(log);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace
#endif

namespace boxcat {
extern bool smoke;
extern bool smokePassed;
extern float preview;
extern bool previewFailure;
extern bool brandPreview;
extern bool tricksPreview;
int RunDemoApplication();
}

int main(int argc, char** argv) {
#ifdef _WIN32
  SetUnhandledExceptionFilter(DemoCrashFilter);
#endif
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::puts("box-cat-demo 0.1.0 (simulated runtime)");
    return 0;
  }
  boxcat::brandPreview = argc >= 2 && std::string_view(argv[1]) == "--brand-preview";
  boxcat::tricksPreview = argc >= 2 && std::string_view(argv[1]) == "--tricks";
  boxcat::smoke = (argc == 2 && std::string_view(argv[1]) == "--smoke-test") ||
      (argc == 3 && (boxcat::brandPreview || boxcat::tricksPreview) &&
       std::string_view(argv[2]) == "--smoke-test");
  if (argc == 3 && (std::string_view(argv[1]) == "--preview-progress" ||
                    std::string_view(argv[1]) == "--preview-failure")) {
    char* end = nullptr;
    const float value = std::strtof(argv[2], &end);
    if (end == argv[2] || *end || !std::isfinite(value) || value < 0 || value > 1) {
      std::fputs("Preview progress requires a number between 0 and 1\n", stderr);
      return 2;
    }
    boxcat::preview = value;
    boxcat::previewFailure = std::string_view(argv[1]) == "--preview-failure";
  } else if (argc > 1 && !boxcat::smoke && !((boxcat::brandPreview || boxcat::tricksPreview) && argc == 2)) {
    std::puts("Usage: box_cat_demo [--version | --smoke-test | --brand-preview [--smoke-test] | --tricks [--smoke-test] | --preview-progress 0..1 | --preview-failure 0..1]");
    return std::string_view(argv[1]) == "--help" ? 0 : 2;
  }
  const int result = boxcat::RunDemoApplication();
  return boxcat::smoke && !boxcat::smokePassed ? 1 : result;
}
