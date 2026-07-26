#pragma once
#include "dsp_chain.h"
#include "logger.h"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cmath>

// P7: Per-stream DSP configuration table.
// Manages DSP settings for each destination stream, persisted as dsp.json.
// Mirrors the RoutingTable pattern in routing.h.

struct DspConfig {
    std::vector<DspSettings> streams;

    // --- JSON helpers (minimal, no external dependency) ---

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

    static bool jsonBool(const std::string& obj, const char* key, bool def) {
        if (obj.find(std::string("\"") + key + "\":true")  != std::string::npos) return true;
        if (obj.find(std::string("\"") + key + "\": true") != std::string::npos) return true;
        if (obj.find(std::string("\"") + key + "\":false") != std::string::npos) return false;
        if (obj.find(std::string("\"") + key + "\": false") != std::string::npos) return false;
        return def;
    }

    static std::vector<std::string> extractArray(const std::string& json, const char* key) {
        std::vector<std::string> items;
        std::string search = std::string("\"") + key + "\"";
        size_t p = json.find(search);
        if (p == std::string::npos) return items;
        p = json.find("[", p);
        if (p == std::string::npos) return items;
        p++;
        int depth = 1;
        while (p < json.size() && depth > 0) {
            if (json[p] == '{') {
                size_t start = p; p++;
                int innerDepth = 1;
                while (p < json.size() && innerDepth > 0) {
                    if (json[p] == '{') innerDepth++;
                    else if (json[p] == '}') innerDepth--;
                    if (innerDepth > 0) p++;
                }
                items.push_back(json.substr(start, p - start + 1));
                p++;
            } else if (json[p] == ']') {
                depth--; p++;
            } else {
                p++;
            }
        }
        return items;
    }

    // --- File I/O ---

    bool LoadFromFile(const char* filename) {
        std::ifstream f(filename);
        if (!f) {
            Logger::Instance().Warn("dsp.json not found, using defaults (gain=1.0, no mute)");
            ApplyDefaults(1);  // at least 1 stream
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string json = ss.str();

        streams.clear();

        auto streamItems = extractArray(json, "streams");
        for (const auto& item : streamItems) {
            DspSettings s;
            s.gain = (float)jsonDouble(item, "gain", 1.0);
            s.mute = jsonBool(item, "mute", false);
            // P7b: parse optional EQ bands array
            auto bandItems = extractArray(item, "bands");
            for (size_t b = 0; b < bandItems.size() && b < 3; b++) {
                s.bands[b].freqHz  = (float)jsonDouble(bandItems[b], "freq", s.bands[b].freqHz);
                s.bands[b].gainDb  = (float)jsonDouble(bandItems[b], "gain_db", s.bands[b].gainDb);
                s.bands[b].Q       = (float)jsonDouble(bandItems[b], "q", s.bands[b].Q);
                s.bands[b].enabled = jsonBool(bandItems[b], "enabled", false);
            }
            streams.push_back(s);
        }

        if (streams.empty()) {
            Logger::Instance().Warn("dsp.json has no streams, applying defaults");
            ApplyDefaults(1);
        }
        Logger::Instance().Info("DSP config loaded: %zu streams", streams.size());
        return true;
    }

    std::string ToJson() const {
        std::ostringstream ss;
        ss << "{\n  \"streams\": [\n";
        for (size_t i = 0; i < streams.size(); i++) {
            ss << "    {\"gain\":" << streams[i].gain
               << ",\"mute\":" << (streams[i].mute ? "true" : "false");
            // P7b: serialize EQ bands
            ss << ",\"bands\":[";
            for (int b = 0; b < 3; b++) {
                if (b > 0) ss << ",";
                ss << "{\"freq\":" << streams[i].bands[b].freqHz
                   << ",\"gain_db\":" << streams[i].bands[b].gainDb
                   << ",\"q\":" << streams[i].bands[b].Q
                   << ",\"enabled\":" << (streams[i].bands[b].enabled ? "true" : "false")
                   << "}";
            }
            ss << "]}";
            if (i + 1 < streams.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ]\n}";
        return ss.str();
    }

    // P7b: redesign all biquad coefficients after config load or sample rate change
    void RedesignAll(float sampleRate) {
        for (auto& s : streams) s.RedesignAll(sampleRate);
    }

    void ApplyDefaults(size_t streamCount) {
        streams.clear();
        for (size_t i = 0; i < streamCount; i++) {
            streams.push_back(DspSettings{1.0f, false});
        }
    }

    // Ensure we have DSP settings for at least `count` streams.
    // Called after routing reconfig to keep DSP config and destination count in sync.
    void SyncCount(size_t count) {
        while (streams.size() < count) {
            streams.push_back(DspSettings{1.0f, false});
        }
        if (streams.size() > count) {
            streams.resize(count);
        }
    }
};
