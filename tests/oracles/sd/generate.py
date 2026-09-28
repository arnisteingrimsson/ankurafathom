#!/usr/bin/env python3
"""Generate or verify independent SD trajectories; never calls the C++ runtime."""
import argparse
import hashlib
import json
import platform
from pathlib import Path

import numpy as np
import scipy
from scipy.integrate import solve_ivp

VERSIONS = ("2.4.3", "1.17.1")
PRIMARY = dict(method="DOP853", rtol=2e-13, atol=2e-15, max_step=0.0625)
TIGHT = dict(method="DOP853", rtol=4e-14, atol=4e-16, max_step=0.03125)
SECONDARY = dict(method="Radau", rtol=2e-13, atol=2e-15, max_step=0.03125)
REFERENCE_GATE = 2e-11  # absolute cross-solver / closed-form discrepancy
REGEN_GATE = 2e-12     # stored vs regenerated values; cross-platform rounding allowed


def specifications():
    return [
        dict(name="sir", initial=[0.99, 0.01, 0.0], stocks=["susceptible", "infectious", "recovered"],
             parameters=dict(beta=0.6, gamma=0.2), horizon=32.0, sample_dt=0.5, base_dt=0.25, scales=[1.0]*3),
        dict(name="lotka_volterra", initial=[4.0, 2.0], stocks=["prey", "predator"],
             parameters=dict(alpha=1.0, beta=0.5, delta=0.25, gamma=0.75),
             horizon=8.0, sample_dt=0.25, base_dt=0.0625, scales=[4.0, 2.0]),
        dict(name="bass", initial=[0.0], stocks=["adopted_fraction"], parameters=dict(p=0.03, q=0.7),
             horizon=16.0, sample_dt=0.5, base_dt=0.5, scales=[1.0]),
        dict(name="logistic", initial=[0.1], stocks=["population_fraction"], parameters=dict(r=0.8, capacity=1.0),
             horizon=12.0, sample_dt=0.5, base_dt=0.5, scales=[1.0]),
        dict(name="oscillator", initial=[1.0, 0.0], stocks=["position", "velocity"], parameters=dict(damping=0.2, omega=1.0),
             horizon=12.0, sample_dt=0.25, base_dt=0.125, scales=[1.0, 1.0]),
        dict(name="time_growth", initial=[1.0], stocks=["stock"], parameters={},
             horizon=2.0, sample_dt=0.125, base_dt=0.125, scales=[8.0]),
    ]


def rhs(spec):
    p, name = spec["parameters"], spec["name"]
    if name == "sir":
        def f(t, y):
            s, i, r = y
            infection, recovery = p["beta"]*s*i, p["gamma"]*i
            return [-infection, infection-recovery, recovery]
    elif name == "lotka_volterra":
        def f(t, y):
            x, z = y
            return [p["alpha"]*x-p["beta"]*x*z, p["delta"]*x*z-p["gamma"]*z]
    elif name == "bass":
        def f(t, y):
            return [(p["p"]+p["q"]*y[0])*(1-y[0])]
    elif name == "logistic":
        def f(t, y):
            return [p["r"]*y[0]*(1-y[0]/p["capacity"])]
    elif name == "oscillator":
        def f(t, y):
            return [y[1], -2*p["damping"]*y[1]-p["omega"]**2*y[0]]
    else:
        def f(t, y):
            return [t*y[0]]
    return f


def exact(spec, times):
    p, name = spec["parameters"], spec["name"]
    if name == "bass":
        e = np.exp(-(p["p"]+p["q"])*times)
        return ((1-e)/(1+p["q"]/p["p"]*e))[:, None]
    if name == "logistic":
        return (p["capacity"]/(1+(p["capacity"]/spec["initial"][0]-1)*np.exp(-p["r"]*times)))[:, None]
    if name == "oscillator":
        d, omega = p["damping"], p["omega"]
        w = np.sqrt(omega**2-d**2)
        envelope = np.exp(-d*times)
        return np.column_stack((envelope*(np.cos(w*times)+d/w*np.sin(w*times)),
                                -envelope*omega**2/w*np.sin(w*times)))
    if name == "time_growth":
        return np.exp(times**2/2)[:, None]
    return None


def solve(spec, times, options):
    result = solve_ivp(rhs(spec), (0, spec["horizon"]), spec["initial"], t_eval=times, **options)
    if not result.success or result.status != 0 or not np.array_equal(result.t, times) or not np.isfinite(result.y).all():
        raise RuntimeError(f'{spec["name"]}: reference integration failed: {result.message}')
    return result.y.T


def generate():
    if (np.__version__, scipy.__version__) != VERSIONS:
        raise RuntimeError(f"Install pinned versions from requirements.txt; found {np.__version__}, {scipy.__version__}")
    models = []
    for spec in specifications():
        times = np.arange(round(spec["horizon"]/spec["sample_dt"])+1)*spec["sample_dt"]
        values = solve(spec, times, PRIMARY)
        tight = solve(spec, times, TIGHT)
        secondary = solve(spec, times, SECONDARY)
        diagnostics = dict(tight_dop853_max_abs_gap=float(np.max(np.abs(values-tight))),
                           radau_max_abs_gap=float(np.max(np.abs(values-secondary))))
        analytic = exact(spec, times)
        if analytic is not None:
            diagnostics["closed_form_max_abs_gap"] = float(np.max(np.abs(values-analytic)))
        if max(diagnostics.values()) > REFERENCE_GATE:
            raise RuntimeError(f'{spec["name"]}: reference self-check failed: {diagnostics}')
        if spec["name"] == "sir":
            if np.max(np.abs(values.sum(axis=1)-1)) > 2e-13 or np.min(values) < 0:
                raise RuntimeError("SIR reference violates conservation/positivity")
        if spec["name"] == "lotka_volterra":
            p = spec["parameters"]
            x, z = values.T
            h = p["delta"]*x-p["gamma"]*np.log(x)+p["beta"]*z-p["alpha"]*np.log(z)
            if np.min(values) <= 0 or np.max(np.abs(h-h[0])) > REFERENCE_GATE:
                raise RuntimeError("Lotka-Volterra reference violates its first integral")
        models.append(dict(**spec, times=times.tolist(), values=values.tolist(), checks=diagnostics))
    return dict(schema_version=1, generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                versions=dict(python=platform.python_version(), numpy=np.__version__, scipy=scipy.__version__),
                primary=PRIMARY, tight=TIGHT, secondary=SECONDARY,
                reference_absolute_gate=REFERENCE_GATE, regeneration_absolute_gate=REGEN_GATE, models=models)


def verify(saved, regenerated):
    for field in ("schema_version", "generator_sha256", "primary", "tight", "secondary", "reference_absolute_gate", "regeneration_absolute_gate"):
        if saved[field] != regenerated[field]:
            raise RuntimeError(f"reference metadata changed: {field}; explicitly regenerate and review")
    for package in ("numpy", "scipy"):
        if saved["versions"][package] != regenerated["versions"][package]:
            raise RuntimeError(f"reference package version changed: {package}")
    if len(saved["models"]) != len(regenerated["models"]):
        raise RuntimeError("model count changed")
    for a, b in zip(saved["models"], regenerated["models"]):
        for key in specifications()[0]:
            if a[key] != b[key]:
                raise RuntimeError(f'{a["name"]}: specification changed: {key}')
        if a["times"] != b["times"]:
            raise RuntimeError(f'{a["name"]}: observation times changed')
        old, new = np.array(a["values"]), np.array(b["values"])
        if old.shape != new.shape or not np.isfinite(old).all():
            raise RuntimeError(f'{a["name"]}: invalid stored trajectory shape or values')
        if np.max(np.abs(old-new)) > REGEN_GATE:
            raise RuntimeError(f'{a["name"]}: regenerated trajectory exceeds tolerance')
    print("Six stored SD references agree with regenerated DOP853; tighter DOP853, Radau, and analytic checks pass")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("reference.json"))
    args = parser.parse_args()
    result = generate()
    if args.verify:
        verify(json.loads(args.output.read_text()), result)
    else:
        args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+"\n")
        print(f"Wrote {len(result['models'])} independent SD reference trajectories to {args.output}")


if __name__ == "__main__":
    main()
