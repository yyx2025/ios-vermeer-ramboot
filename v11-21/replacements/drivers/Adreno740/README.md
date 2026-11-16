# Adreno740 K70 iOS16 replacement

`NativeAdrenoPublish.cpp` starts from the exact retained iOS16 input of CI run
43091498480, SHA256 8ccdce16a900346db1e65a124395e0fb2b8f3d78206b603f3880cbc56fdfece5.

CPU BO view cleanup now explicitly unmaps every returned `IOMemoryMap` and
releases the reference transferred by `removeMappingForDescriptor`. The
prior raw-pointer loop discarded that reference, leaving user PTEs present
when `NativeAdrenoMemory::release` returned the physical backing to the VM.
Failed unmap retains map, view and core backing, and blocks further calls.
The existing multiple-lease policy and native GPU fence/TLB retirement remain.

The production cleanup function is exercised by
`tools/check_gpu_view_lifetime.py`; the old function fails the live-PTE check
and the repaired function passes multiple-map, port-retention, token-isolation
and failure-retention cases. Full iOS16 compilation/linking, final binary
checks and K70 runtime acceptance are recorded separately.

GPU execution passed on v11.34 (SCM, production ZAP, fence and scratch).
Desktop GPU frame completion and stable interaction remain unverified.
