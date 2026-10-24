# DoomOS full-review task list

Use this as the manual backlog if scheduled MCP tasks are rate-limited. Goal:
confirm DoomOS is suitable in all respects for high-performance dhewm3.

## R1 — Boot / runtime / SMP
- [ ] HSM bring-up of secondary harts; softgles2 worker count vs `SMP`
- [ ] `dos_thread` spin / pause hints under TCG (no wasted host cycles)
- [ ] Heap: long map-load fragmentation; large texture arenas
- [ ] Timer resolution for `[rprof]` and `Sys_Milliseconds`
- [ ] Write findings → `review-boot-smp.md`

## R2 — softgles2 RVV
- [ ] Kernel hit rate on Mars City (interaction / overlay / VM)
- [ ] Cube / pow / bilinear costs under TCG after latest optimisations
- [ ] Sparse pack occupancy; HiZ cull rate
- [ ] MT bin/mainbusy/wait balance at SMP=4/6/8
- [ ] VLEN 256 vs 1024 recommendation for TCG vs HW
- [ ] Write findings → `review-softgles2.md`

## R3 — Engine + gl4es + I/O
- [ ] `com_maxCatchupTics` / frame loop under multi-second frames
- [ ] Texture upload path (`EndLevelLoad`) time breakdown
- [ ] idSIMD_RVV selected + covers TransformVerts / joints
- [ ] gl4es FPE cost vs specialised kernels
- [ ] virtio-gpu present: copy vs zero-copy
- [ ] Write findings → `review-engine-gl4es.md`

## R4 — QEMU 11 host profile
- [ ] Document recommended `run.sh` env for daily TCG
- [ ] tb-size / pin / SMP matrix
- [ ] Write findings → `review-qemu11-tcg.md`

## Exit criteria
DoomOS is “suitable for higher engine perf” when:
1. TCG scenic frames have a clear measured bottleneck with a next lever
2. No correctness blockers in SMP / FS / present
3. A documented TCG profile and HW profile both build and run
4. softgles2 interaction path stays bit-exact to the scalar VM oracle
