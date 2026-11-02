// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/theme_repository.h"
#include "ui/theme.h"
#include "oslib/http_client.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <iostream>
#include <stdexcept>

namespace
{
const std::string url = "https://themes.example/catalog.json";
const std::string catalog = R"({"formatVersion":1,"themes":[{
    "id":"example","name":"Example","creator":"Theme Artist",
    "description":"A test theme.","version":"1.0",
    "package":"example.zip","screenshots":["example.png"]
}]})";
bool initialized = false;
int initCalls = 0, getCalls = 0;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void checkCatalog(const std::vector<themes::repository::Theme>& themes)
{
    check(themes.size() == 1 && themes.front().id == "example", "Catalogue was not loaded");
}
}

// Model the Windows client's requirement without networking or platform APIs.
namespace http
{
void init() { initialized = true; ++initCalls; }
void term() { initialized = false; }
int get(const std::string& request, std::vector<u8>& content, const Headers*, Headers*, int)
{
    ++getCalls;
    check(request == url, "Unexpected request URL");
    if (!initialized) return 500;
    content.assign(catalog.begin(), catalog.end());
    return 200;
}
}

// Installation is not involved in these catalogue transport tests.
namespace themes
{
std::string directory() { throw std::runtime_error("Unexpected directory access"); }
Theme load(const std::string&) { throw std::runtime_error("Unexpected theme load"); }
std::string install(const std::string&, bool) { throw std::runtime_error("Unexpected installation"); }
}

int main(int argc, char** argv)
{
    try
    {
        check(argc == 2, "Expected a test scenario");
        const std::string scenario = argv[1];
        if (scenario == "custom_transport")
        {
            int transportCalls = 0;
            checkCatalog(themes::repository::fetch(url, [&](const std::string& request, std::vector<u8>& content) {
                check(request == url, "Unexpected custom transport URL");
                ++transportCalls;
                content.assign(catalog.begin(), catalog.end());
                return 200;
            }));
            check(transportCalls == 1 && initCalls == 0 && getCalls == 0,
                "Custom transport unexpectedly used the native HTTP client");
        }
        else
        {
            check(scenario == "cold_start" || scenario == "after_shutdown", "Unknown test scenario");
            checkCatalog(themes::repository::fetch(url));
            check(initialized && initCalls > 0 && getCalls == 1, "HTTP client was not initialized before fetching");
            if (scenario == "after_shutdown")
            {
                // Other app services can shut down the shared HTTP client.
                http::term();
                checkCatalog(themes::repository::fetch(url));
                check(initialized && getCalls == 2, "HTTP client was not reinitialized");
            }
        }
        std::cout << scenario << ": passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
