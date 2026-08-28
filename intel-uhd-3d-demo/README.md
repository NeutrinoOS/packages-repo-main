# Intel UHD 3D demo

`intel-uhd-3d-demo` submits Neutrino's bounded Gen9 demo draw through libdrm,
waits for its render fence, verifies the GPU-produced orange pixels, scales the
64x64 result into a fullscreen DRM/KMS framebuffer, and presents it for five
seconds before returning to the framebuffer console. It intentionally does not
run under the desktop: the desktop owns a separate graphical-session lease and
cannot be displaced by an ordinary process. Boot without launching the desktop,
then run the demo from the console. Boot with
`INTEL_UHD.RCS_PROBE` so the driver validates RCS before accepting the request.

The demo also exercises render ABI 1.5 explicit BO cache synchronization in
both directions around submission and fence retirement.

The initial ABI keeps its batch, shader, and render target kernel-owned. The
retired image is staged into the program's render BO before presentation; raw
userspace command submission remains disabled.
