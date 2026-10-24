// Shared desktop entry; HuxerUI selects the native platform backend.
#include <huxerui/app.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string_view>

namespace boxcat {
extern bool smoke;
extern bool smokePassed;
extern float preview;
extern bool previewFailure;
extern bool brandPreview;
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::puts("box-cat-demo 0.1.0 (simulated runtime)");
    return 0;
  }
  boxcat::brandPreview = argc >= 2 && std::string_view(argv[1]) == "--brand-preview";
  boxcat::smoke = (argc == 2 && std::string_view(argv[1]) == "--smoke-test") ||
      (argc == 3 && boxcat::brandPreview && std::string_view(argv[2]) == "--smoke-test");
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
  } else if (argc > 1 && !boxcat::smoke && !(boxcat::brandPreview && argc == 2)) {
    std::puts("Usage: box_cat_demo [--version | --smoke-test | --brand-preview [--smoke-test] | --preview-progress 0..1 | --preview-failure 0..1]");
    return std::string_view(argv[1]) == "--help" ? 0 : 2;
  }
  const int result = huxerui::RunApplication();
  return boxcat::smoke && !boxcat::smokePassed ? 1 : result;
}
