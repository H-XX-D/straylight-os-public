"""straylight_photonics — SAX-shaped Python SDK over the StrayLight C++ photonic
device. Same engine the OS runs; this is the learning/porting skin. A toy model
written against this ports to real silicon by swapping the backend behind run().

    import straylight_photonics as slp
    dev = slp.Device()
    dev.set_mzi(0, 3.1416/2, 0.0)      # a 50/50 splitter
    U = dev.unitary()                  # the mesh S-matrix (numpy complex NxN)
    y = dev.run([1, 0, 0, 0])          # propagate a field
"""
import ctypes, os
import numpy as np

_LIB = os.environ.get("SL_PHOTONICS_LIB", os.path.join(os.path.dirname(__file__), "libstraylight_photonics.so"))
_l = ctypes.CDLL(_LIB)
_l.sl_photonics_new.restype = ctypes.c_void_p
_l.sl_photonics_modes.restype = ctypes.c_int
for fn, args in {
    "sl_photonics_free": [ctypes.c_void_p],
    "sl_photonics_num_mzis": [ctypes.c_void_p],
    "sl_photonics_set_mzi": [ctypes.c_void_p, ctypes.c_int, ctypes.c_double, ctypes.c_double],
    "sl_photonics_set_output_phase": [ctypes.c_void_p, ctypes.c_int, ctypes.c_double],
    "sl_photonics_set_loss_db": [ctypes.c_void_p, ctypes.c_double],
    "sl_photonics_set_crosstalk_db": [ctypes.c_void_p, ctypes.c_double],
    "sl_photonics_set_wavelength_nm": [ctypes.c_void_p, ctypes.c_double],
    "sl_photonics_unitary": [ctypes.c_void_p, ctypes.POINTER(ctypes.c_double)],
    "sl_photonics_run": [ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double)],
}.items():
    getattr(_l, fn).argtypes = args


class Device:
    def __init__(self):
        self._d = _l.sl_photonics_new()
        self.N = _l.sl_photonics_modes()

    def __del__(self):
        try: _l.sl_photonics_free(self._d)
        except Exception: pass

    @property
    def num_mzis(self): return _l.sl_photonics_num_mzis(self._d)

    # programming primitives
    def set_mzi(self, i, theta, phi): _l.sl_photonics_set_mzi(self._d, i, theta, phi)
    def set_output_phase(self, mode, p): _l.sl_photonics_set_output_phase(self._d, mode, p)
    def set_loss_db(self, v): _l.sl_photonics_set_loss_db(self._d, v)
    def set_crosstalk_db(self, v): _l.sl_photonics_set_crosstalk_db(self._d, v)
    def set_wavelength_nm(self, v): _l.sl_photonics_set_wavelength_nm(self._d, v)

    # the mesh transform == its scattering matrix (lossless mesh). SAX-style name.
    def unitary(self):
        buf = (ctypes.c_double * (2 * self.N * self.N))()
        _l.sl_photonics_unitary(self._d, buf)
        a = np.array(buf).reshape(self.N, self.N, 2)
        return a[..., 0] + 1j * a[..., 1]

    def sdict(self):  # SAX calls the device transform the s-matrix; same object
        return self.unitary()

    def run(self, x):
        x = np.asarray(x, dtype=complex).reshape(self.N)
        ib = (ctypes.c_double * (2 * self.N))()
        for i in range(self.N): ib[2*i], ib[2*i+1] = x[i].real, x[i].imag
        ob = (ctypes.c_double * (2 * self.N))()
        _l.sl_photonics_run(self._d, ib, ob)
        o = np.array(ob).reshape(self.N, 2)
        return o[..., 0] + 1j * o[..., 1]
