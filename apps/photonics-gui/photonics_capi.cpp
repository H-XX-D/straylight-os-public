// apps/photonics-gui/photonics_capi.cpp
// C ABI boundary so a Python SDK (ctypes) drives the StrayLight photonic device.
// Same C++ engine the OS uses; this is only the SDK seam (C++ core, Python skin,
// matching how Qiskit Aer / SAX / Lava are all built).
#include "photonics_device.h"
using namespace straylight::photonics;

extern "C" {
int  sl_photonics_modes()                                  { return Device::N; }
void* sl_photonics_new()                                   { return new Device(); }
void sl_photonics_free(void* d)                            { delete static_cast<Device*>(d); }
int  sl_photonics_num_mzis(void* d)                        { return static_cast<Device*>(d)->num_mzis(); }
void sl_photonics_set_mzi(void* d, int i, double th, double ph) { static_cast<Device*>(d)->set_mzi(i, th, ph); }
void sl_photonics_set_output_phase(void* d, int m, double p)    { static_cast<Device*>(d)->set_output_phase(m, p); }
void sl_photonics_set_loss_db(void* d, double v)           { static_cast<Device*>(d)->set_loss_db(v); }
void sl_photonics_set_crosstalk_db(void* d, double v)      { static_cast<Device*>(d)->set_crosstalk_db(v); }
void sl_photonics_set_wavelength_nm(void* d, double v)     { static_cast<Device*>(d)->set_wavelength_nm(v); }

// out[2*N*N]: interleaved (re,im), row-major NxN unitary (the S-matrix).
void sl_photonics_unitary(void* d, double* out) {
    auto U = static_cast<Device*>(d)->unitary();
    int N = Device::N, k = 0;
    for (int r = 0; r < N; ++r) for (int c = 0; c < N; ++c) { auto z = U.at(r, c); out[k++] = z.real(); out[k++] = z.imag(); }
}
// in[2*N] -> out[2*N], interleaved (re,im).
void sl_photonics_run(void* d, const double* in, double* out) {
    int N = Device::N; Device::Vec x{};
    for (int i = 0; i < N; ++i) x[i] = Device::cd(in[2*i], in[2*i+1]);
    auto y = static_cast<Device*>(d)->run(x);
    for (int i = 0; i < N; ++i) { out[2*i] = y[i].real(); out[2*i+1] = y[i].imag(); }
}
}
