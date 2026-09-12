// MSVC 18.0 compatibility for C++23's imported standard library module.

#if defined(_MSC_VER)
#include <charconv>

namespace std {

// The VS 18.0 std module can leave these <charconv> members undefined when
// floating-point formatting is instantiated from an imported module.
constexpr int _General_precision_tables_2<float>::_Max_P;
constexpr int _General_precision_tables_2<double>::_Max_P;

namespace {
[[maybe_unused]] const volatile int* const clashfluxCharconvFloatMaxP =
    &_General_precision_tables_2<float>::_Max_P;
[[maybe_unused]] const volatile int* const clashfluxCharconvDoubleMaxP =
    &_General_precision_tables_2<double>::_Max_P;
}

} // namespace std
#endif
