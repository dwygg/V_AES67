#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include "logger.h"

// ---- P4: Routing Contract ----

struct RouteEntry {
    int     sourceChannel  = 0;     // WASAPI loopback channel index
    int     destStream     = 0;     // AES67 stream index
    float   gain           = 1.0f;  // linear gain (0.0 = silence, 1.0 = pass-through)
    bool    mute           = false;
};

struct StreamInfo {
    std::string name;
    std::string address;
    uint16_t    port = 5004;
};

struct RoutingTable {
    std::vector<StreamInfo> destinations;
    std::vector<RouteEntry> routes;

    bool LoadFromFile(const char* filename) {
        std::ifstream f(filename);
        if (!f) {
            Logger::Instance().Warn("routing.json not found, using defaults (ch0→stream0, ch1→stream0)");
            ApplyDefaults();
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string json = ss.str();

        // Minimal JSON parser for routing table (no external dependency)
        destinations.clear();
        routes.clear();

        // Parse destinations
        auto dstBlock = extractArray(json, "destinations");
        for (const auto& item : dstBlock) {
            StreamInfo si;
            si.name    = jsonStr(item, "name");
            si.address = jsonStr(item, "address");
            si.port    = (uint16_t)jsonInt(item, "port", 5004);
            destinations.push_back(si);
        }

        // Parse routes
        auto routeBlock = extractArray(json, "routes");
        for (const auto& item : routeBlock) {
            RouteEntry re;
            re.sourceChannel = jsonInt(item, "source", 0);
            re.destStream    = jsonInt(item, "destination", 0);
            re.gain          = (float)jsonDouble(item, "gain", 1.0);
            re.mute          = item.find("\"mute\":true") != std::string::npos
                               || item.find("\"mute\": true") != std::string::npos;
            routes.push_back(re);
        }

        if (destinations.empty()) {
            Logger::Instance().Warn("routing.json has no destinations, applying defaults");
            ApplyDefaults();
        }
        Logger::Instance().Info("Routing loaded: %zu destinations, %zu routes",
            destinations.size(), routes.size());
        return true;
    }

    std::string ToJson() const {
        std::ostringstream ss;
        ss << "{\n  \"destinations\": [\n";
        for (size_t i = 0; i < destinations.size(); i++) {
            ss << "    {\"name\":\"" << destinations[i].name
               << "\",\"address\":\"" << destinations[i].address
               << "\",\"port\":" << destinations[i].port << "}";
            if (i + 1 < destinations.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ],\n  \"routes\": [\n";
        for (size_t i = 0; i < routes.size(); i++) {
            ss << "    {\"source\":" << routes[i].sourceChannel
               << ",\"destination\":" << routes[i].destStream
               << ",\"gain\":" << routes[i].gain
               << ",\"mute\":" << (routes[i].mute ? "true" : "false") << "}";
            if (i + 1 < routes.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ]\n}";
        return ss.str();
    }

private:
    void ApplyDefaults() {
        destinations.clear();
        destinations.push_back({"AES67 Stream 1", "239.69.1.128", 5004});
        routes.clear();
        routes.push_back({0, 0, 1.0f, false});  // L → stream
        routes.push_back({1, 0, 1.0f, false});  // R → stream
    }

    // -- minimal JSON helpers (no library dependency) --
    static std::string jsonStr(const std::string& obj, const char* key) {
        size_t p = obj.find(std::string("\"") + key + "\"");
        if (p == std::string::npos) return "";
        p = obj.find(":", p);
        if (p == std::string::npos) return "";
        p = obj.find("\"", p + 1);
        if (p == std::string::npos) return "";
        size_t q = obj.find("\"", p + 1);
        if (q == std::string::npos) return "";
        return obj.substr(p + 1, q - p - 1);
    }

    static int jsonInt(const std::string& obj, const char* key, int def) {
        size_t p = obj.find(std::string("\"") + key + "\"");
        if (p == std::string::npos) return def;
        p = obj.find(":", p);
        if (p == std::string::npos) return def;
        // skip whitespace + colon
        for (p++; p < obj.size() && (obj[p] == ' ' || obj[p] == ':' || obj[p] == '\t'); p++) {}
        int sign = 1;
        if (p < obj.size() && obj[p] == '-') { sign = -1; p++; }
        int val = 0;
        while (p < obj.size() && obj[p] >= '0' && obj[p] <= '9') {
            val = val * 10 + (obj[p] - '0'); p++;
        }
        return sign * val;
    }

    static double jsonDouble(const std::string& obj, const char* key, double def) {
        size_t p = obj.find(std::string("\"") + key + "\"");
        if (p == std::string::npos) return def;
        p = obj.find(":", p);
        if (p == std::string::npos) return def;
        for (p++; p < obj.size() && (obj[p] == ' ' || obj[p] == ':' || obj[p] == '\t'); p++) {}
        char* end = nullptr;
        double val = strtod(obj.c_str() + p, &end);
        return (end != obj.c_str() + p) ? val : def;
    }

    static std::vector<std::string> extractArray(const std::string& json, const char* key) {
        std::vector<std::string> items;
        std::string search = std::string("\"") + key + "\"";
        size_t p = json.find(search);
        if (p == std::string::npos) return items;
        p = json.find("[", p);
        if (p == std::string::npos) return items;
        p++; // skip [
        int depth = 1;
        while (p < json.size() && depth > 0) {
            if (json[p] == '{') {
                size_t start = p;
                int innerDepth = 1; p++;
                while (p < json.size() && innerDepth > 0) {
                    if (json[p] == '{') innerDepth++;
                    else if (json[p] == '}') innerDepth--;
                    if (innerDepth > 0) p++;
                }
                items.push_back(json.substr(start, p - start + 1));
                p++;
            } else if (json[p] == ']') {
                depth--;
                p++;
            } else {
                p++;
            }
        }
        return items;
    }
};
