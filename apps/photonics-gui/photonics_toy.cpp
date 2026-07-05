// apps/photonics-gui/photonics_toy.cpp
// A toy model on the soft photonic device, the kind someone would write to learn
// photonic computing. Uses only the device primitives. Prints numbers an external
// numpy check verifies (unitarity + matrix-multiply), so "it works" means it
// matches math we did not author.
#include "photonics_device.h"
#include <cstdio>

using namespace straylight::photonics;
using cd = Device::cd;

int main() {
    Device dev;
    std::printf("photonic device: %d modes, %d MZIs\n", Device::N, dev.num_mzis());

    // Lesson 1: a 50/50 beam splitter. First set every crossing to pass-through
    // (theta=pi, the bar state -- no mode mixing), then set the first crossing to
    // theta=pi/2 (balanced splitter). Light in mode 0 splits 50/50 to modes 0,1.
    // The real minutiae: an MZI mesh has no idle elements -- you must route the
    // light through every crossing it passes, or it scatters.
    for (int i = 0; i < dev.num_mzis(); ++i) dev.set_mzi(i, M_PI, 0.0);  // pass-through
    dev.set_mzi(0, M_PI / 2.0, 0.0);                                      // splitter
    Device::Vec in{}; in[0] = cd(1, 0);
    auto s = dev.run(in);
    std::printf("L1 splitter: in[0]=1 -> |out0|^2=%.4f |out1|^2=%.4f (expect 0.5/0.5)\n",
                std::norm(s[0]), std::norm(s[1]));

    // Lesson 2: the whole mesh is a programmable linear transform U. Inject a
    // vector x, read U*x. Print U and y so numpy can recompute and check.
    for (int i = 0; i < dev.num_mzis(); ++i) dev.set_mzi(i, 0.3 + 0.5 * i, 0.15 + 0.35 * i);
    Device::Vec x{ cd(1, 0), cd(0, 1), cd(-1, 0), cd(0, -1) };
    auto y = dev.run(x);
    std::printf("L2 matmul: y =");
    for (int i = 0; i < Device::N; ++i) std::printf(" (%.6f,%.6f)", y[i].real(), y[i].imag());
    std::printf("\n");
    auto U = dev.unitary();
    std::printf("UMATRIX");
    for (int r = 0; r < Device::N; ++r)
        for (int c = 0; c < Device::N; ++c)
            std::printf(" %.12f %.12f", U.at(r, c).real(), U.at(r, c).imag());
    std::printf("\n");
    std::printf("XVEC");
    for (int i = 0; i < Device::N; ++i) std::printf(" %.12f %.12f", x[i].real(), x[i].imag());
    std::printf("\n");

    // Lesson 3: a real impairment. 0.5 dB/MZI insertion loss drops total power --
    // the analog minutiae a learner needs to see (an ideal sim would hide it).
    double p0 = 0; for (auto v : dev.run(x)) p0 += std::norm(v);
    dev.set_loss_db(0.5);
    double p1 = 0; for (auto v : dev.run(x)) p1 += std::norm(v);
    std::printf("L3 loss: total power %.4f -> %.4f (%.2f dB) at 0.5 dB/MZI\n",
                p0, p1, 10.0 * std::log10(p1 / p0));
    return 0;
}
