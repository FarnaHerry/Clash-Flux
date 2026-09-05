// Smoke test — verifies the project compiles + a binary runs.
// Add more tests as tests/test_*.cpp files; mcpp test discovers them
// automatically (one binary per file).
import std;

int main() {
    std::println("test_smoke: ok");
    return 0;
}
