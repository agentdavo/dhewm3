# demorun TCG profile snapshot — 2026-07-21T18:53:42+01:00
# TCG_PROFILE=1 RENDER_PROF=1 VLEN=256 FB=512x288 SMP=6 PIN_HARTS=1
# Log: riscv/docs/perf/demorun-tcg-profile.log (still running if map loading)

## Early-frame [rprof] (menu / soft_particle / load UI)
[rprof] draws=3 tris=18 frags=112554 skipxp=0 mt_engaged=1 mt_blits=1 | vertex=0.6ms dispatch=216.4ms raster=46.3ms present=0.0ms total=263.4ms | present copy_gamma=0.0ms xfer=0.0ms flush=0.0ms
[rprof-vs] simt=3 draws/54 verts kernel=0 batches/0 verts scalar=0 draws/0 transformed-verts
[rprof-mt] bin=0.3ms release=0.0ms mainbusy=215.7ms wait=0.0ms | frags=112554 | hart busy/tiles h0=215.7ms/264 h1=202.7ms/324 h2=202.0ms/336 h3=215.5ms/304 h4=207.4ms/280 h5=202.7ms/292 | shade=556.1ms (44%) wrapper=690.2ms lanes=117979 occ=95%
[rprof] draws=3 tris=18 frags=112554 skipxp=0 mt_engaged=1 mt_blits=1 | vertex=0.5ms dispatch=216.2ms raster=43.0ms present=0.0ms total=259.8ms | present copy_gamma=0.0ms xfer=0.0ms flush=0.0ms
[rprof-vs] simt=3 draws/54 verts kernel=0 batches/0 verts scalar=0 draws/0 transformed-verts
[rprof-mt] bin=0.7ms release=0.0ms mainbusy=215.4ms wait=0.0ms | frags=112554 | hart busy/tiles h0=215.4ms/280 h1=210.4ms/276 h2=212.6ms/328 h3=212.0ms/292 h4=211.2ms/352 h5=210.4ms/272 | shade=557.6ms (43%) wrapper=714.6ms lanes=117979 occ=95%
[rprof] draws=3 tris=18 frags=112554 skipxp=0 mt_engaged=1 mt_blits=1 | vertex=1.0ms dispatch=231.2ms raster=26.2ms present=0.0ms total=258.5ms | present copy_gamma=0.0ms xfer=0.0ms flush=0.0ms
[rprof-vs] simt=3 draws/54 verts kernel=0 batches/0 verts scalar=0 draws/0 transformed-verts
[rprof-mt] bin=0.3ms release=0.0ms mainbusy=223.1ms wait=7.6ms | frags=112554 | hart busy/tiles h0=223.1ms/296 h1=228.2ms/280 h2=219.9ms/372 h3=219.3ms/268 h4=224.5ms/272 h5=217.1ms/312 | shade=581.1ms (43%) wrapper=751.3ms lanes=117979 occ=95%
[rprof] draws=3 tris=18 frags=112554 skipxp=0 mt_engaged=1 mt_blits=1 | vertex=0.5ms dispatch=210.9ms raster=38.9ms present=0.0ms total=250.4ms | present copy_gamma=0.0ms xfer=0.0ms flush=0.0ms
[rprof-vs] simt=3 draws/54 verts kernel=0 batches/0 verts scalar=0 draws/0 transformed-verts
[rprof-mt] bin=0.6ms release=0.0ms mainbusy=210.2ms wait=0.0ms | frags=112554 | hart busy/tiles h0=210.2ms/324 h1=202.0ms/308 h2=202.0ms/316 h3=200.5ms/280 h4=200.3ms/252 h5=201.6ms/320 | shade=505.6ms (41%) wrapper=711.2ms lanes=117979 occ=95%
[rprof] draws=3 tris=18 frags=112554 skipxp=0 mt_engaged=1 mt_blits=1 | vertex=0.7ms dispatch=234.2ms raster=47.9ms present=0.0ms total=282.9ms | present copy_gamma=0.0ms xfer=0.0ms flush=0.0ms
[rprof-vs] simt=3 draws/54 verts kernel=0 batches/0 verts scalar=0 draws/0 transformed-verts
[rprof-mt] bin=0.3ms release=0.0ms mainbusy=233.7ms wait=0.0ms | frags=112554 | hart busy/tiles h0=233.7ms/288 h1=227.8ms/260 h2=232.4ms/356 h3=226.8ms/332 h4=233.3ms/252 h5=229.2ms/312 | shade=593.0ms (42%) wrapper=790.4ms lanes=117979 occ=95%
[rprof] draws=3 tris=18 frags=112554 skipxp=0 mt_engaged=1 mt_blits=1 | vertex=0.8ms dispatch=221.5ms raster=42.6ms present=0.0ms total=264.9ms | present copy_gamma=0.0ms xfer=0.0ms flush=0.0ms
[rprof-vs] simt=3 draws/54 verts kernel=0 batches/0 verts scalar=0 draws/0 transformed-verts
[rprof-mt] bin=0.6ms release=0.0ms mainbusy=220.6ms wait=0.0ms | frags=112554 | hart busy/tiles h0=220.6ms/284 h1=218.3ms/272 h2=217.1ms/304 h3=215.5ms/336 h4=213.9ms/372 h5=209.2ms/232 | shade=507.8ms (39%) wrapper=786.9ms lanes=117979 occ=95%
[rprof] draws=3 tris=18 frags=112554 skipxp=0 mt_engaged=1 mt_blits=1 | vertex=0.7ms dispatch=211.0ms raster=36.6ms present=0.0ms total=248.3ms | present copy_gamma=0.0ms xfer=0.0ms flush=0.0ms
[rprof-vs] simt=3 draws/54 verts kernel=0 batches/0 verts scalar=0 draws/0 transformed-verts
[rprof-mt] bin=0.6ms release=0.0ms mainbusy=210.2ms wait=0.0ms | frags=112554 | hart busy/tiles h0=210.2ms/280 h1=205.6ms/272 h2=205.9ms/280 h3=203.1ms/368 h4=206.2ms/336 h5=207.2ms/264 | shade=543.9ms (43%) wrapper=694.4ms lanes=117979 occ=95%
[rprof] draws=4 tris=20 frags=112554 skipxp=0 mt_engaged=1 mt_blits=1 | vertex=0.7ms dispatch=218.1ms raster=41.6ms present=0.0ms total=260.5ms | present copy_gamma=0.0ms xfer=0.0ms flush=0.0ms
[rprof-vs] simt=4 draws/60 verts kernel=0 batches/0 verts scalar=0 draws/0 transformed-verts
[rprof-mt] bin=0.3ms release=0.0ms mainbusy=210.6ms wait=7.0ms | frags=112554 | hart busy/tiles h0=210.6ms/324 h1=208.2ms/364 h2=211.5ms/276 h3=207.2ms/292 h4=206.7ms/264 h5=206.0ms/280 | shade=520.2ms (41%) wrapper=730.3ms lanes=117979 occ=95%

## Observations
- present=0 (headless/no virtio-gpu or zero-copy path; good)
- dispatch/mainbusy ~200-240ms dominates at 512x288 with ~112k frags
- shade ~40-47% of hart busy; wrapper (coverage/Z/interp/blend/store) ~53-60%
- VS kernel batches=0 so far (overlay/MVP kernels not matching these draws)
- heatHaze.vfp loaded — candidate for a future specialized kernel
- MT balance OK across 6 harts; wait mostly small
- occ=95% so sparse-lane pack rarely fires
