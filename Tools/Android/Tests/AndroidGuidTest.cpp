#include <Guid.hpp>

#include <array>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

int main()
{
    std::set<std::string> values;
    std::mutex mutex;
    bool valid = true;
    std::array<std::thread, 4> workers;
    for (auto& worker : workers)
        worker = std::thread([&] {
            for (int i = 0; i < 1000; ++i)
            {
                const auto guid = xg::newGuid();
                const auto text = guid.str();
                std::lock_guard lock(mutex);
                valid &= guid.isValid() && text.size() == 36 && text[14] == '4'
                    && std::string("89ab").find(text[19]) != std::string::npos
                    && xg::Guid(text) == guid && values.insert(text).second;
            }
        });
    for (auto& worker : workers) worker.join();
    if (!valid || values.size() != 4000) return 1;
    std::cout << "PASS: 4000 UUIDv4 values across 4 native threads, unique and round-trippable\n";
}
