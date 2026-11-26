# RAM16G ownership proof before handoff

Analysis companion to [ram16g-plan2](ram16g-plan2.md). **CMA, lower DXE and FD currently contribute 0 proved MiB.** A successful EFI map describes firmware allocation types; it does not certify secure-world permissions, Gunyah memory-parcel ownership, or the last DMA reference. The m120c-120 map must be joined with those independent receipts before a slab is admitted.

This lane changes documentation only. No tablet access, image build, memory-map change, SCM call or push is authorized here. Future behavior must remain behind a default-off boot argument.

## Reference scope

Ubuntu v1.4.0 is commit `5b7b8a5cada36c43eed2cc153ab430bea69c6b00`; its `kernel/source.json:3–9` pins Linux `83f0f89bed14a0cb7d582a26e72bced0387870f4`. References below use that Git object, not the newer working-tree HEAD of the read-only reference clones. Native base is `af341efd`; firmware pins are recorded in `unixnu.lock:23–48`.

## Fix-image gate

> **HOLD — 0 slabs admitted by this analysis.** For each proposed 2 MiB slab, require the successful final EFI descriptor coverage, affirmative HLOS ownership with no outstanding secure/VM borrow, and complete CPU/DMA lifetime evidence from the same native boot through ordinary resume. Missing observations are UNKNOWN and keep the slab excluded. A static DT reservation, disabled Linux driver, free CMA counter, successful CPU read, or absent SMMU fault cannot substitute for an ownership receipt. No state-changing SCM/RM operation is a read-only test.

The source comparison, slab owner matrix and exact read specifications are filled in by the subsequent analysis commit.
