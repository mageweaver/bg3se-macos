# -*- coding: utf-8 -*-
# dump_functor_bytes.py — Ghidra headless (Jython) script.
# Dumps raw instruction bytes at the known 4.1.1.6995620 functor/damage addresses
# from the archived Ghidra project, so masked ARM64 signatures can be scanned
# against the 4.1.1.7209685 binary (Wave 2 Goal 2.2 damage-event recovery).
#
# Usage:
#   ~/ghidra/support/analyzeHeadless ~ BaldursGate3 \
#       -process AARCH64-64-cpu0x0 -noanalysis -readOnly \
#       -scriptPath <this dir> -postScript dump_functor_bytes.py <output.json>
import json

TARGETS = {
    "ExecuteStatsFunctor":            0x105783a38,
    "ExecuteFunctors_AttackTarget":   0x105787918,
    "ExecuteFunctors_AttackPosition": 0x105787c6c,
    "ExecuteFunctors_Move":           0x10578975c,
    "ExecuteFunctors_Target":         0x10578a918,
    "ExecuteFunctors_NearbyAttacked": 0x10578e4d8,
    "ExecuteFunctors_NearbyAttacking":0x10578fba8,
    "ExecuteFunctors_Equip":          0x105790a28,
    "ExecuteFunctors_Source":         0x105792a90,
    "ExecuteFunctors_Interrupt":      0x1057965e4,
    "ProcessDealDamageFunctors":      0x10538f374,
    "DealDamageFunctor_ApplyDamage":  0x10538e8fc,
}
N_BYTES = 512

args = getScriptArgs()
outpath = args[0] if len(args) > 0 else "/tmp/functor_bytes_6995620.json"

out = {
    "program": currentProgram.getName(),
    "image_base": str(currentProgram.getImageBase()),
    "build": "4.1.1.6995620",
    "n_bytes": N_BYTES,
}
fm = currentProgram.getFunctionManager()
targets = {}
for name in TARGETS:
    a = TARGETS[name]
    addr = toAddr(a)
    rec = {"addr": "0x%x" % a}
    try:
        bs = getBytes(addr, N_BYTES)
        rec["bytes"] = ''.join(['%02x' % (b & 0xff) for b in bs])
    except Exception, e:  # Jython 2.x
        rec["error"] = str(e)
    f = fm.getFunctionAt(addr)
    if f is not None:
        rec["func_name"] = f.getName()
        rec["func_size"] = f.getBody().getNumAddresses()
    targets[name] = rec
out["targets"] = targets

fp = open(outpath, "w")
json.dump(out, fp, indent=1, sort_keys=True)
fp.close()
print("WROTE " + outpath)
