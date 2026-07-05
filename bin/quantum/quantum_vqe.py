#!/usr/bin/env python3
"""Variational Quantum Eigensolver (VQE) for an N-qubit spin chain, using the
StrayLight (pre-B300, non-secret) Solver winch (L-BFGS-B) as the classical
optimizer. Honest learning tool: the ansatz is a real gate circuit (Ry layers +
CNOT entanglers on a 2^N statevector), the objective is the true energy
expectation, and the result is checked against numpy exact diagonalization
(the external oracle), not the optimizer's own number.

  python3 quantum_vqe.py --model tfim --qubits 4 --J 1.0 --h 1.0 --json
Set SL_SOLVER_DIR to point at the Solver install if it is not auto-found.
"""
import sys, os, json, argparse
import numpy as np

def find_solver():
    for c in [
        os.environ.get('SL_SOLVER_DIR'),
        os.path.expanduser('~/Solver'),
        os.path.expanduser('~/.local/share/straylight/Solver'),
        '/opt/straylight/Solver',
        '/usr/share/straylight/Solver',
    ]:
        if c and os.path.isfile(os.path.join(c, 'solver_continuous.py')):
            return c
    return None

I2 = np.eye(2, dtype=complex)
X = np.array([[0, 1], [1, 0]], dtype=complex)
Y = np.array([[0, -1j], [1j, 0]], dtype=complex)
Z = np.array([[1, 0], [0, -1]], dtype=complex)

def ry(t):
    c, s = np.cos(t / 2), np.sin(t / 2)
    return np.array([[c, -s], [s, c]], dtype=complex)

def two_site(P, i, j, N):
    """P (x) P acting on qubits i,j, identity elsewhere (qubit 0 = most sig)."""
    m = np.array([[1]], dtype=complex)
    for q in range(N):
        m = np.kron(m, P if (q == i or q == j) else I2)
    return m

def one_site(P, i, N):
    m = np.array([[1]], dtype=complex)
    for q in range(N):
        m = np.kron(m, P if q == i else I2)
    return m

def hamiltonian(model, J, h, N):
    H = np.zeros((2 ** N, 2 ** N), dtype=complex)
    if model == 'heisenberg':                       # J sum (XX+YY+ZZ) open chain
        for i in range(N - 1):
            H += J * (two_site(X, i, i + 1, N) + two_site(Y, i, i + 1, N) + two_site(Z, i, i + 1, N))
        return H
    for i in range(N - 1):                           # tfim: -J sum ZZ - h sum X
        H += -J * two_site(Z, i, i + 1, N)
    for i in range(N):
        H += -h * one_site(X, i, N)
    return H

def apply_1q(state, U, q, N):
    t = state.reshape([2] * N)
    t = np.tensordot(U, t, axes=([1], [q]))         # U on axis q -> result axis 0
    t = np.moveaxis(t, 0, q)
    return t.reshape(-1)

def cnot_perm(c, t, N):
    """Index permutation for CNOT(control c, target t); qubit 0 = most sig bit."""
    cb, tb = 1 << (N - 1 - c), 1 << (N - 1 - t)
    return np.array([(x ^ tb) if (x & cb) else x for x in range(2 ** N)])

def make_ansatz(N, L):
    perms = [cnot_perm(q, q + 1, N) for q in range(N - 1)]
    def ansatz(theta):
        s = np.zeros(2 ** N, dtype=complex); s[0] = 1.0
        p = 0
        for _ in range(L):
            for q in range(N):
                s = apply_1q(s, ry(theta[p]), q, N); p += 1
            for k in range(N - 1):
                s = s[perms[k]]
        for q in range(N):                           # final rotation layer
            s = apply_1q(s, ry(theta[p]), q, N); p += 1
        return s
    return ansatz, N * (L + 1)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--model', default='tfim', choices=['tfim', 'heisenberg'])
    ap.add_argument('--qubits', type=int, default=2)
    ap.add_argument('--layers', type=int, default=0)     # 0 -> auto (= qubits)
    ap.add_argument('--J', type=float, default=1.0)
    ap.add_argument('--h', type=float, default=1.0)
    ap.add_argument('--restarts', type=int, default=16)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    N = a.qubits
    if N < 2 or N > 10:
        print(json.dumps({"error": "qubits must be in [2,10] (dense eigh oracle)"})); sys.exit(2)
    L = a.layers if a.layers > 0 else N

    sd = find_solver()
    if not sd:
        print(json.dumps({"error": "Solver not found; set SL_SOLVER_DIR"})); sys.exit(2)
    sys.path.insert(0, sd)
    import solver_continuous as sc
    _orig = sc.scalar_expr
    sc.scalar_expr = lambda e: e if callable(e) else _orig(e)

    H = hamiltonian(a.model, a.J, a.h, N)
    ansatz, P = make_ansatz(N, L)
    def energy(x):
        psi = ansatz(np.asarray(x))
        return float(np.real(np.vdot(psi, H @ psi)))

    rng = np.random.default_rng(0); best, be = None, np.inf
    for _ in range(a.restarts):
        x0 = (rng.random(P) * 2 * np.pi).tolist()
        res = sc._winch({"f": energy, "x0": x0, "bounds": [[-7, 7]] * P, "max_iter": 2000, "tol": 1e-12})
        if res["value"] < be:
            be, best = res["value"], np.asarray(res["x"])

    w, v = np.linalg.eigh(H)                          # external oracle
    exact = float(w[0]); gs = v[:, 0]
    psi = ansatz(best)
    fidelity = float(abs(np.vdot(gs, psi)) ** 2)

    out = {"model": a.model, "qubits": N, "layers": L, "params": P, "J": a.J, "h": a.h,
           "vqe_energy": round(be, 9), "exact_energy": round(exact, 9),
           "abs_error": round(abs(be - exact), 11), "state_fidelity": round(fidelity, 9),
           "backend": "solver.winch",
           "angles": [round(float(t), 9) for t in best]}
    print(json.dumps(out) if a.json else json.dumps(out, indent=1))
    sys.exit(0 if abs(be - exact) < 1e-4 and fidelity > 0.99 else 1)

if __name__ == '__main__':
    main()
