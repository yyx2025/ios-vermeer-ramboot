# SoftwareAES

Software AES-CBC through IOAESAccelerator.

built into the kernel and used by the sheng crypto path.

A request is processed in 4096-byte pieces through a kernel buffer, each piece
with its own key schedule, by the kernel's `aes_encrypt_cbc` and
`aes_decrypt_cbc` (the registered corecrypto). `native_aes_completed`,
`native_aes_bytes` and `native_aes_ticks` count the completed requests, their
bytes and the time spent in them.

Build and interface details: [documentation](https://unixnu.com/drivers/software-aes). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

K70 iOS16 startup diagnostics mirror each load/init/start step and the
existing NIST CBC/descriptor self-test result to the enqueue-only USB log.
The provider still registers only after all existing tests succeed.
v11.36 also reports direct CBC, descriptor/rejection contracts, and raw
encrypt/decrypt results separately, including byte and IV comparisons against
the existing public NIST vectors. No key material or request contents are logged.
