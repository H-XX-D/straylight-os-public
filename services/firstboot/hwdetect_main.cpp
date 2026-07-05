// services/firstboot/hwdetect_main.cpp
// Entry point for straylight-hwdetect binary.
// Must be run as root (needs modprobe).
#include "hwdetect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>

static constexpr const char* kReportPath =
    "/var/lib/straylight/hwdetect.json";
static constexpr const char* kStateDir =
    "/var/lib/straylight";

int main(int argc, char* argv[]) {
    // Allow override of output path for testing.
    const char* out = kReportPath;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            out = argv[++i];
        }
    }

    // Ensure output directory exists.
    std::error_code ec;
    std::filesystem::create_directories(kStateDir, ec);
    if (ec) {
        fprintf(stderr, "hwdetect: cannot create %s: %s\n",
                kStateDir, ec.message().c_str());
        return EXIT_FAILURE;
    }

    fprintf(stdout, "straylight-hwdetect: scanning hardware...\n");
    fflush(stdout);

    auto report = straylight::hwdetect::detect_all();

    fprintf(stdout,
        "hwdetect: kernel=%s  devices=%d  loaded=%d  builtin=%d"
        "  missing=%d  dkms=%d  fw_missing=%d\n",
        report.kernel_version.c_str(),
        report.total, report.loaded, report.builtin,
        report.missing, report.dkms, report.firmware_missing);

    if (!straylight::hwdetect::write_report(report, out)) {
        fprintf(stderr, "hwdetect: failed to write report to %s\n", out);
        return EXIT_FAILURE;
    }

    fprintf(stdout, "hwdetect: report written to %s\n", out);
    fflush(stdout);

    // Exit 0 even if some drivers are missing — OOBE will display the status.
    // A non-zero exit would block the firstboot service.
    return EXIT_SUCCESS;
}
