#include <iostream>
#include <string_view>

import clashflux.api;

int main(int argc, char** argv) {
    if (argc != 5 || std::string_view(argv[1]) != "--reconcile-mode") return 2;
    api::ClashApi client(argv[2], argv[3]);
    const auto result = api::reconcileMode(client, argv[4]);
    if (!result.ok) {
        std::cerr << result.error << '\n';
        return 1;
    }
    std::cout << "reconciled mode: " << result.actual << '\n';
    return 0;
}
