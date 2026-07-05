#pragma once
// Shell out to the StrayLight Solver decompose tool (winch / L-BFGS-B, the
// non-secret pre-B300 optimizer), parse its JSON, and write the synthesized
// phases into the engine. The GUI "Decompose & Program" button and the
// standalone parse test both call this, so the exact path the button runs is
// the path the numpy oracle verifies.
#include "photonics_engine.h"
#include <string>
#include <cstdio>
#include <cstdlib>

namespace straylight::photonics {

// Find `key` in `s` from `from`, parse the double after the next ':'. Advances
// `from` past the parsed number. Returns false if key/number not found.
inline bool sl_scan_after(const std::string& s, const char* key, size_t& from, double& out) {
    size_t k = s.find(key, from);
    if (k == std::string::npos) return false;
    size_t colon = s.find(':', k);
    if (colon == std::string::npos) return false;
    const char* start = s.c_str() + colon + 1;
    char* end = nullptr;
    out = std::strtod(start, &end);
    if (end == start) return false;
    from = static_cast<size_t>(end - s.c_str());
    return true;
}

// Run the decompose tool for `target` ("dft"|"hadamard"), parse the JSON, and
// write 6 theta/phi (by MZI application order) + 4 output phases into `eng`.
// Does NOT call recompute -- the caller does. Tool command base defaults to the
// installed CLI and is overridable via env SL_PHOTONICS_DECOMPOSE (for tests).
// Returns true on success; `msg` is always set to a human-readable status.
inline bool sl_decompose_into_engine(engine::EngineState& eng,
                                     const std::string& target,
                                     std::string& msg) {
    const char* envtool = std::getenv("SL_PHOTONICS_DECOMPOSE");
    std::string base = (envtool && *envtool) ? envtool : "straylight-photonics-decompose";
    std::string cmd = base + " --target " + target + " --modes 4 --json 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) { msg = "decompose failed: cannot launch '" + base + "'"; return false; }
    std::string out;
    char buf[1024]; size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
    pclose(pipe);

    double th[6], ph[6], op[4], fid = 0.0;
    size_t pos = 0;
    bool ok = true;
    for (int i = 0; i < 6 && ok; ++i)
        ok = sl_scan_after(out, "\"theta\"", pos, th[i]) &&
             sl_scan_after(out, "\"phi\"", pos, ph[i]);
    if (ok) {  // output_phase: 4 doubles after the '['
        size_t ap = out.find("\"output_phase\"");
        size_t br = (ap == std::string::npos) ? std::string::npos : out.find('[', ap);
        if (br == std::string::npos) { ok = false; }
        else {
            const char* p = out.c_str() + br + 1; char* e = nullptr;
            for (int r = 0; r < 4 && ok; ++r) {
                op[r] = std::strtod(p, &e);
                if (e == p) { ok = false; break; }
                p = e; while (*p == ',' || *p == ' ') ++p;
            }
        }
    }
    { size_t fp = 0; sl_scan_after(out, "\"fidelity\"", fp, fid); }

    if (!ok) {
        msg = out.empty() ? "decompose failed: no output (tool missing or Solver not found)"
                          : ("decompose failed: " + out.substr(0, 160));
        return false;
    }
    for (auto& m : eng.grid)
        if (m.active) { m.theta = th[m.order]; m.phi = ph[m.order]; }
    for (int r = 0; r < engine::kN; ++r) eng.output_phase[r] = op[r];

    char b[176];
    std::snprintf(b, sizeof(b),
        "synthesized %s (tool fidelity %.4f%%, solver.winch)", target.c_str(), fid * 100.0);
    msg = b;
    return true;
}

} // namespace straylight::photonics
