#!/usr/bin/env python3
"""Fit the shared input current of a leaky-integrate-and-fire (LIF) population so
its mean firing rate hits a target, using the StrayLight (pre-B300, non-secret)
Solver winch (L-BFGS-B). Honest learning tool: the objective uses the closed-form
LIF rate 1000/(tau*ln(I/(I-vth))) (smooth, so the optimizer converges), and the
answer is checked against an independent Euler ODE simulation of every neuron (the
external cross-check), not the optimizer's own number.

Single neuron is the default (--neurons 1); --neurons N with --vth-spread S models
a heterogeneous population (thresholds spread +/-S around vth) doing rate coding,
its mean rate still exactly = mean of per-neuron closed-form rates.

  python3 snn_fit.py --target-rate 50 --tau 10 --vth 1.0 [--neurons 8 --vth-spread 0.3] --json
Set SL_SOLVER_DIR to point at the Solver install if it is not auto-found.
"""
import sys, os, json, argparse, math

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

def thresholds(vth, n, spread):
    # Deterministic linear spread of thresholds across the population. n==1 (or
    # spread==0) gives every neuron the same vth, so the population reduces exactly
    # to the single-neuron case (backward compatible).
    if n == 1 or spread == 0.0:
        return [vth] * n
    return [vth * (1.0 + spread * (2.0 * i / (n - 1) - 1.0)) for i in range(n)]

def analytic_rate(I, tau, vth):
    # Closed-form LIF rate (no refractory). 0 below threshold (silent neuron).
    if I <= vth:
        return 0.0
    return 1000.0 / (tau * math.log(I / (I - vth)))

def analytic_pop(I, tau, vths):
    return sum(analytic_rate(I, tau, v) for v in vths) / len(vths)

def sim_pop(I, tau, vths, dt=0.1, steps=40000):
    # Real primitive: Euler integration of dv/dt = (-v + I)/tau with reset, for
    # every neuron; returns the population MEAN rate (Hz). Independent of the
    # closed form, so agreement is a genuine check.
    total = 0
    for vth in vths:
        v = 0.0
        for _ in range(steps):
            v += dt * (-v + I) / tau
            if v >= vth:
                v = 0.0; total += 1
    return 1000.0 * total / (len(vths) * steps * dt)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--target-rate', type=float, default=50.0)  # Hz (population mean)
    ap.add_argument('--tau', type=float, default=10.0)          # ms
    ap.add_argument('--vth', type=float, default=1.0)
    ap.add_argument('--neurons', type=int, default=1)
    ap.add_argument('--vth-spread', type=float, default=0.0)    # fractional +/- around vth
    ap.add_argument('--restarts', type=int, default=12)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    if a.neurons < 1 or a.neurons > 256:
        print(json.dumps({"error": "neurons must be in [1,256]"})); sys.exit(2)

    sd = find_solver()
    if not sd:
        print(json.dumps({"error": "Solver not found; set SL_SOLVER_DIR"})); sys.exit(2)
    sys.path.insert(0, sd)
    import solver_continuous as sc
    _orig = sc.scalar_expr
    sc.scalar_expr = lambda e: e if callable(e) else _orig(e)   # accept a callable objective

    tgt, tau, vth = a.target_rate, a.tau, a.vth
    vths = thresholds(vth, a.neurons, a.vth_spread)
    vmin = min(vths)
    # objective: squared error of the closed-form POPULATION mean rate vs target.
    # Smooth + monotonic in I, so the finite-difference winch converges (the raw
    # ODE rate is a stair-step in I and stalls it). Euler sim is the oracle below.
    def obj(x):
        I = float(x[0])
        if I <= vmin:
            return (0.0 - tgt) ** 2 + (vmin - I) ** 2   # push back into firing region
        return (analytic_pop(I, tau, vths) - tgt) ** 2

    rng_lo, rng_hi = vmin + 1e-3, max(vths) + 50.0
    best, be = None, math.inf
    for k in range(a.restarts):
        x0 = [vmin + 1e-3 + (rng_hi - rng_lo) * ((k + 0.5) / a.restarts)]
        res = sc._winch({"f": obj, "x0": x0, "bounds": [[rng_lo, rng_hi]],
                         "max_iter": 1000, "tol": 1e-12})
        if res["value"] < be:
            be, best = res["value"], float(res["x"][0])

    I = best
    sim = sim_pop(I, tau, vths)
    exact = analytic_pop(I, tau, vths)          # oracle (mean of closed-form rates)
    out = {"target_rate_hz": tgt, "tau_ms": tau, "vth": vth,
           "neurons": a.neurons, "vth_spread": a.vth_spread,
           "fit_current": round(I, 9),
           "sim_rate_hz": round(sim, 6),
           "analytic_rate_hz": round(exact, 6),
           "sim_vs_target_hz": round(abs(sim - tgt), 6),
           "analytic_vs_target_hz": round(abs(exact - tgt), 6),
           "sim_vs_analytic_hz": round(abs(sim - exact), 6),
           "backend": "solver.winch"}
    print(json.dumps(out) if a.json else json.dumps(out, indent=1))
    # success: closed-form fit hit the target, the simulated population fires near
    # it, and the independent ODE sim agrees with the closed-form oracle.
    tol = max(0.5, 0.02 * tgt)
    ok = abs(exact - tgt) <= 1e-2 and abs(sim - tgt) <= tol and abs(sim - exact) <= tol
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
