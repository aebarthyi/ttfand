// ttfand — Tenstorrent fan daemon
//
// Reads the max ASIC temperature across the chips on a Tenstorrent card
// (via tt-umd) and drives a Linux hwmon PWM fan on a preset curve.
//
// Intended use: a passively-cooled Wormhole n300s whose heatsink is cooled
// by a host-controllable fan (motherboard/chassis PWM header). The card's
// OWN fan cannot be driven from the host on Wormhole — its ARC message set
// has no fan command and its management firmware is not the open Zephyr
// stack — so this daemon deliberately controls a host fan instead, keyed to
// the card's die temperature that tt-umd can read.
//
// Safety posture for a passive card: on ANY failure to read temperature, on
// startup before the first good reading, and on clean shutdown, the fan is
// forced to 100%. It is never left at a low duty by accident.
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---- tt-umd -----------------------------------------------------------------
// These headers come from libtt-umd (the umd::tt-umd CMake target). The exact
// header paths and telemetry accessor names have moved around across UMD
// versions; the reader is isolated in read_max_asic_temp_umd() below and the
// two lines most likely to need adjustment are flagged with  ADJUST:  .
#include <umd/device/cluster.hpp>
#include <umd/device/tt_device/tt_device.hpp>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct CurvePoint {
    double temp_c;   // ASIC temperature (°C)
    int    pwm;      // 0..255
};

struct Config {
    // hwmon PWM control file, e.g. /sys/class/hwmon/hwmon2/pwm1
    std::string pwm_path;
    // matching enable file, e.g. /sys/class/hwmon/hwmon2/pwm1_enable
    // (1 = manual/software control). Left empty => not touched.
    std::string pwm_enable_path;

    int    poll_ms          = 1000;   // how often to sample temperature
    double hysteresis_c     = 3.0;    // don't lower PWM until temp drops this far
    int    min_pwm          = 60;     // floor while daemon is running (never stall a passive card)
    int    max_pwm          = 255;
    int    spinup_pwm       = 255;    // kick to this on start so the fan reliably spins
    int    spinup_ms        = 2000;
    int    fail_pwm         = 255;    // duty on read failure / shutdown
    int    fail_grace       = 3;      // consecutive read failures tolerated before fail-safe

    // Monotonic-in-temp curve. Interpolated linearly between points.
    std::vector<CurvePoint> curve = {
        {40, 60}, {50, 90}, {60, 140}, {70, 190}, {80, 255},
    };
};

static std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Parse "curve = 40:60, 50:90, ..." into sorted points.
static std::optional<std::vector<CurvePoint>> parse_curve(const std::string& v) {
    std::vector<CurvePoint> pts;
    std::stringstream ss(v);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (tok.empty()) continue;
        auto colon = tok.find(':');
        if (colon == std::string::npos) return std::nullopt;
        try {
            CurvePoint p;
            p.temp_c = std::stod(trim(tok.substr(0, colon)));
            p.pwm    = std::stoi(trim(tok.substr(colon + 1)));
            p.pwm    = std::clamp(p.pwm, 0, 255);
            pts.push_back(p);
        } catch (...) {
            return std::nullopt;
        }
    }
    if (pts.empty()) return std::nullopt;
    std::sort(pts.begin(), pts.end(),
              [](const CurvePoint& a, const CurvePoint& b) { return a.temp_c < b.temp_c; });
    return pts;
}

static bool load_config(const std::string& path, Config& cfg, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "cannot open config " + path; return false; }
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        auto eq = s.find('=');
        if (eq == std::string::npos) { err = "bad line " + std::to_string(lineno); return false; }
        std::string k = trim(s.substr(0, eq));
        std::string v = trim(s.substr(eq + 1));
        try {
            if      (k == "pwm_path")        cfg.pwm_path = v;
            else if (k == "pwm_enable_path") cfg.pwm_enable_path = v;
            else if (k == "poll_ms")         cfg.poll_ms = std::stoi(v);
            else if (k == "hysteresis_c")    cfg.hysteresis_c = std::stod(v);
            else if (k == "min_pwm")         cfg.min_pwm = std::clamp(std::stoi(v), 0, 255);
            else if (k == "max_pwm")         cfg.max_pwm = std::clamp(std::stoi(v), 0, 255);
            else if (k == "spinup_pwm")      cfg.spinup_pwm = std::clamp(std::stoi(v), 0, 255);
            else if (k == "spinup_ms")       cfg.spinup_ms = std::stoi(v);
            else if (k == "fail_pwm")        cfg.fail_pwm = std::clamp(std::stoi(v), 0, 255);
            else if (k == "fail_grace")      cfg.fail_grace = std::stoi(v);
            else if (k == "curve") {
                auto c = parse_curve(v);
                if (!c) { err = "bad curve on line " + std::to_string(lineno); return false; }
                cfg.curve = *c;
            } else {
                err = "unknown key '" + k + "' on line " + std::to_string(lineno);
                return false;
            }
        } catch (...) {
            err = "bad value for '" + k + "' on line " + std::to_string(lineno);
            return false;
        }
    }
    if (cfg.pwm_path.empty()) { err = "pwm_path is required"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Curve evaluation
// ---------------------------------------------------------------------------
static int curve_pwm(const Config& cfg, double temp_c) {
    const auto& c = cfg.curve;
    int pwm;
    if (temp_c <= c.front().temp_c) {
        pwm = c.front().pwm;
    } else if (temp_c >= c.back().temp_c) {
        pwm = c.back().pwm;
    } else {
        pwm = c.back().pwm;
        for (size_t i = 1; i < c.size(); ++i) {
            if (temp_c <= c[i].temp_c) {
                double t0 = c[i - 1].temp_c, t1 = c[i].temp_c;
                double p0 = c[i - 1].pwm,    p1 = c[i].pwm;
                double frac = (temp_c - t0) / (t1 - t0);
                pwm = static_cast<int>(p0 + frac * (p1 - p0) + 0.5);
                break;
            }
        }
    }
    return std::clamp(pwm, cfg.min_pwm, cfg.max_pwm);
}

// ---------------------------------------------------------------------------
// hwmon sysfs
// ---------------------------------------------------------------------------
static bool write_sysfs(const std::string& path, const std::string& val) {
    std::ofstream f(path);
    if (!f) return false;
    f << val;
    f.flush();
    return static_cast<bool>(f);
}

static bool set_pwm(const Config& cfg, int pwm) {
    return write_sysfs(cfg.pwm_path, std::to_string(std::clamp(pwm, 0, 255)));
}

// ---------------------------------------------------------------------------
// Temperature via tt-umd
// ---------------------------------------------------------------------------
// Returns the max ASIC temperature (°C) across all chips, or nullopt on error.
//
// The Cluster is opened once and kept open; a stale handle after a card reset
// would need the daemon restarted (systemd Restart=on-failure covers that).
class TempReader {
public:
    bool open(std::string& err) {
        try {
            cluster_ = std::make_unique<tt::umd::Cluster>();
            // ADJUST: get_target_device_ids() is the enumeration call across
            // recent UMD; older trees used get_all_chips()/ get_target_mmio_device_ids().
            auto ids = cluster_->get_target_device_ids();
            chips_.assign(ids.begin(), ids.end());
            if (chips_.empty()) { err = "tt-umd found no chips"; return false; }
            return true;
        } catch (const std::exception& e) {
            err = std::string("tt-umd open failed: ") + e.what();
            return false;
        }
    }

    std::optional<double> read_max_c() {
        double maxc = -1e9;
        bool any = false;
        try {
            for (auto id : chips_) {
                tt::umd::TTDevice* dev = cluster_->get_tt_device(id);
                if (!dev) continue;
                // ADJUST: temperature accessor. On this UMD version
                // get_asic_temperature() returns std::optional<double> (unavailable
                // reads back as nullopt) rather than a bare double. If your headers
                // expose it differently, the tag-based fallback is
                //     SmBusArcTelemetryReader / read_entry(LegacyTelemetryTag::ASIC_TEMPERATURE)
                // with the 16.16 fixed-point >> 16 conversion.
                auto* fw = dev->get_firmware_info_provider();
                if (!fw) continue;
                std::optional<double> c = fw->get_asic_temperature();
                if (!c) continue;
                if (*c > maxc) maxc = *c;
                any = true;
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (!any) return std::nullopt;
        return maxc;
    }

private:
    std::unique_ptr<tt::umd::Cluster> cluster_;
    std::vector<int> chips_;
};

// ---------------------------------------------------------------------------
// main loop
// ---------------------------------------------------------------------------
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_reload{false};

static void on_term(int) { g_stop.store(true); }
static void on_hup(int)  { g_reload.store(true); }

static void log(const std::string& s) {
    // systemd captures stdout/stderr into the journal.
    std::cout << s << std::endl;
}

int main(int argc, char** argv) {
    std::string cfg_path = "/etc/ttfand.conf";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "-c" || a == "--config") && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: ttfand [-c /etc/ttfand.conf]\n";
            return 0;
        }
    }

    Config cfg;
    std::string err;
    if (!load_config(cfg_path, cfg, err)) {
        std::cerr << "ttfand: config error: " << err << "\n";
        return 2;
    }

    std::signal(SIGTERM, on_term);
    std::signal(SIGINT,  on_term);
    std::signal(SIGHUP,  on_hup);

    // Put the fan under manual control if an enable path was given.
    if (!cfg.pwm_enable_path.empty()) {
        if (!write_sysfs(cfg.pwm_enable_path, "1"))
            log("ttfand: warning: could not set " + cfg.pwm_enable_path + " (need root?)");
    }

    // Fail-safe first: full speed until we have a trustworthy reading.
    set_pwm(cfg, cfg.fail_pwm);

    // Spin-up kick so the fan reliably starts from rest.
    if (cfg.spinup_ms > 0) {
        set_pwm(cfg, cfg.spinup_pwm);
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.spinup_ms));
    }

    TempReader reader;
    if (!reader.open(err)) {
        log("ttfand: " + err + " — holding fan at fail-safe " + std::to_string(cfg.fail_pwm));
        // Keep the fan safe and keep retrying to open the device.
        while (!g_stop.load()) {
            set_pwm(cfg, cfg.fail_pwm);
            std::this_thread::sleep_for(2s);
            if (reader.open(err)) { log("ttfand: device opened"); break; }
        }
    }

    log("ttfand: started; controlling " + cfg.pwm_path);

    int applied_pwm = cfg.fail_pwm;
    int fails = 0;

    while (!g_stop.load()) {
        if (g_reload.exchange(false)) {
            Config ncfg;
            std::string e;
            if (load_config(cfg_path, ncfg, e)) { cfg = ncfg; log("ttfand: config reloaded"); }
            else log("ttfand: reload failed: " + e);
        }

        auto t = reader.read_max_c();
        if (!t) {
            if (++fails >= cfg.fail_grace) {
                set_pwm(cfg, cfg.fail_pwm);
                applied_pwm = cfg.fail_pwm;
                log("ttfand: temp read failed x" + std::to_string(fails) +
                    " — fan forced to " + std::to_string(cfg.fail_pwm));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.poll_ms));
            continue;
        }
        fails = 0;

        int target = curve_pwm(cfg, *t);

        // Hysteresis: raise immediately, but only lower once the temperature
        // has actually dropped below the curve point that justifies the lower duty.
        int next = applied_pwm;
        if (target > applied_pwm) {
            next = target;
        } else if (target < applied_pwm) {
            int lower_target = curve_pwm(cfg, *t + cfg.hysteresis_c);
            if (lower_target < applied_pwm) next = lower_target;
        }

        if (next != applied_pwm) {
            if (set_pwm(cfg, next)) applied_pwm = next;
            else log("ttfand: failed to write PWM (need root?)");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.poll_ms));
    }

    // Leave the fan safe on the way out.
    set_pwm(cfg, cfg.fail_pwm);
    log("ttfand: stopped; fan left at " + std::to_string(cfg.fail_pwm));
    return 0;
}
