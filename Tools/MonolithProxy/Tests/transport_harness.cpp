// Standalone transport harness. Does not run proxy main or editor recovery.
#define main monolith_proxy_unused_main
#include "../monolith_proxy.cpp"
#undef main

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    parse_monolith_url(argv[1]);
    std::string line;
    while (std::getline(std::cin, line))
    {
        const json command = json::parse(line);
        const double start = now_seconds();
        if (command.value("health", false))
        {
            const bool up = check_monolith_up();
            std::cout << json({{"healthy", up}, {"error", g_http_error}}).dump() << std::endl;
        }
        else
        {
            // Exercise real transport on a worker while the main thread checks
            // health: each thread must own an independent connection/session.
            std::string response, error;
            if (command.value("concurrent_health", false))
            {
                std::thread worker([&]() {
                    response = post_monolith(command["request"].dump(), command.value("timeout", 1.0));
                    error = g_http_error;
                });
                const double health_start = now_seconds();
                const bool healthy = check_monolith_up();
                const double health_ms = (now_seconds() - health_start) * 1000;
                worker.join();
                std::cout << json({{"response", response}, {"error", error}, {"healthy", healthy},
                    {"health_ms", health_ms}}).dump() << std::endl;
            }
            else
            {
                response = post_monolith(command["request"].dump(), command.value("timeout", 1.0));
                std::cout << json({{"response", response}, {"error", g_http_error},
                    {"duration_ms", (now_seconds() - start) * 1000}}).dump() << std::endl;
            }
        }
    }
}
