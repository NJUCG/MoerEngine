[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
    // Placeholder cooperative probe entry. The real cooperative-matrix shader path
    // will be wired here once the bundled DXC toolchain is upgraded.
    (void)dispatch_thread_id;
}