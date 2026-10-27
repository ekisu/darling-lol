# PR #41 CPUID and security validation

Candidate: `e948b4cff0424e0937396fca6602a820b7b42811`. The original CPUID
implementation remains excluded. The new security work verifies file signatures
and corrects unsupported syscall responses. It does **not** implement full SIP,
AMFI policy, protected running-code pages, or an Apple-authenticated trust cache.

## CPUID results

The [recorded machine-readable result](../validation/pr41-cpuid-ryzen5500.json)
includes the original patch, harness and binary SHA256, compiler, CPU identity,
host platform, observations and benchmark.
It was produced on an AMD Ryzen 5 5500 with Linux 7.2.9.

- The candidate reports AVX512ER despite the host lacking it, and reports no
  AVX512F prerequisite. This is both an unsupported instruction advertisement
  and an internally inconsistent feature table. VMX, PCID and x2APIC are also
  reported despite being absent on this host; these are additional advertised
  architectural differences, not all userspace instruction features.
- CPUID leaf 0xD advertises the AVX state bit but returns zero size/offset for
  its required subleaf 2 component.
- The handler advances RIP and preserves flags in the direct context test.
  This is a unit test, not proof of kernel delivery.
- Installing the candidate loses an existing SIGSEGV handler. Both an explicit
  signal and a real write to a PROT_NONE page terminate the isolated child
  with SIGSEGV instead of reaching the preinstalled handler.
- `ARCH_SET_CPUID(0)` returns `ENODEV` on this host. The setup in #41 ignores
  that result; it would continue without emulation. There is no trapped
  execution, inheritance result or trapped performance measurement here.
- The native Linux CPUID leaf-1 baseline is about **36–37 ns/call**, median of
  nine batches of 10,000 calls with single-CPU affinity requested. This is not
  Darling/game performance and cannot establish acceptable trapping overhead.

Linux's operation disables the CPUID **instruction** with argument zero,
causing SIGSEGV when supported; zero enables faulting, not native execution.
See the [Linux arch_prctl manual](https://www.kernel.org/pub/linux/docs/man-pages/book/man-pages-6.16.pdf).

On capable hosts the harness checks the exact emulated signature and observes
real trap delivery before measuring the unmodified candidate handler. It also
checks a newly created pthread. Guest exception integration, existing threads,
exec reset/reinitialization, instruction-prefix handling, CPU migration,
XSAVE/XGETBV consistency and representative application workloads still need
validation. A fixed identity table cannot implement instructions a host lacks.

To reproduce (exit 2 means detected candidate failures):

```sh
git fetch origin pull/41/head:refs/remotes/origin/pr41
python3 tools/validate-pr41-cpuid.py
```

Alternatively pass the original patch with `--patch FILE`; the report records
its SHA256 and source, and rejects a patch that differs from the reviewed
original. The test compiles extracted original functions and runs
in isolated subprocesses, disables core files, and bounds execution. No loader,
installed signal handler or real game process is changed.

## Actual file signature verification

`tools/verify-darling-code-signature.py` supports bounded thin little-endian
x86_64 Mach-O images. Unsupported formats fail closed. It checks:

- Load-command, SuperBlob, CodeDirectory, string and hash-table bounds,
  duplicate/overlapping slots, code limits and page count.
- Every sealed code page and available special slot, including the sealing of
  embedded requirements/entitlements; missing nonzero special slots fail.
- Primary and alternate CodeDirectory integrity. CMS verifies the primary
  directory using [OpenSSL CMS verification](https://docs.openssl.org/3.5/man1/openssl-cms/); Apple hash-agility attributes are not interpreted as additional
  authorization for alternate identities.
- Real detached CMS mathematics with OpenSSL. An explicit `--ca-file` additionally
  validates the certificate chain against only the supplied anchors.
- Explicit local trust membership by exact full-image SHA256, independent of
  Apple trust-cache membership.

Examples:

```sh
python3 tools/verify-darling-code-signature.py /path/to/thin-executable --ca-file /path/to/anchors.pem
python3 tools/verify-darling-code-signature.py /path/to/adhoc-executable --allow-adhoc
python3 tools/verify-darling-code-signature.py /path/to/thin-executable --local-trust-cache /path/to/local-cache.json
```

A local cache uses `{"schema":1,"image_sha256":["64-lowercase-hex-digits"]}`.
It is an explicit operator-supplied pin list, not Apple's signed trust cache.
The command never populates it automatically. Exit zero means the requested
file check accepted the snapshot; it does not launch the image or change policy.

No anchor means `cms_verified` can be true while `anchor_verified` stays false.
Ad-hoc verification establishes integrity only and is accepted without trust
only when `--allow-adhoc` is explicit. Identifier and team strings are declared
CodeDirectory metadata, not independently certified developer account identities.
The verifier does not apply Apple's certificate OID/EKU/designated-requirement,
notarization, revocation, timestamp or resource-envelope policies. Legacy SHA1
CodeDirectories are supported for integrity compatibility, not recommended as
new trust anchors. `--info-plist` and `--resources-plist` check their exact seals;
checking CodeResources does not verify every referenced bundle resource.

This is a read-only file-snapshot verifier, not an exec gate: there is no claim
about the bytes later loaded, modified running pages, dynamic libraries or
TOCTOU resistance at launch. It never enables CS_VALID or grants entitlements.
See Apple's [code signature description](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/AboutCS/AboutCS.html).

## Runtime security responses

The isolated kernel candidate now:

- Returns self CS_OPS_STATUS with zero validation/enforcement flags instead of
  the old unconditional CS_VALID. Cross-process task state is unsupported;
  absent PIDs return ESRCH. Null/short status buffers error without writes. Security buffer copies use
  kernel-mediated process_vm_readv/writev; inaccessible buffers return EFAULT
  instead of crashing the loader. A copy spanning valid/invalid pages can fail
  after a partial write.
- Returns errors for unsupported signing identities, cdhashes, entitlement blobs
  and setters. Audit-token APIs do not silently ignore a token's generation and
  credentials. The on-disk verifier is deliberately not a substitute for
  task-owned signing state.
- Reports CSR_VALID_FLAGS as allowed exceptions (SIP disabled), validates exact
  buffer sizes, and rejects unknown exception bits. Unknown operations return a
  negative ENOSYS instead of the previous positive errno/success value.
- Clears AMFI trust-query output and returns ENOTSUP when no implementation or
  authenticated trust-cache source exists. Other AMFI calls return ENOTSUP.
  The preexisting Seatbelt compatibility stub remains; it is not a sandbox.

These changes can expose callers that relied on fabricated CS_VALID or no-op
AMFI successes. They are published for review and have not been installed.
The isolated guest probe reports:

```text
SECURITY_STATE status=0x0 csr_allowances=0xfff fabricated_trust=0 failures=0
```

The [XNU process-signing implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_proc.c)
provides the upstream API reference. Darwin errno values are tested inside
Darling as well as the portable compiled-body contract tests.

## What a full enforcement implementation still requires

SIP is not an enabled flag in csrctl. Its [filesystem protections](https://developer.apple.com/library/archive/documentation/Security/Conceptual/System_Integrity_Protection_Guide/FileSystemProtections/FileSystemProtections.html)
need kernel-enforced protected paths, including rename/link/mmap/metadata
operations and root/host access. Darling also needs task-level debug/injection
restrictions, privileged-operation mediation, an authenticated boot/policy root,
and a definition of which host administrators can change that policy.

Process signing needs immutable executable identity tied to a task and PID
version, launch and dynamic-library verification, enforcement across exec/fork,
page-integrity enforcement on mappings and writes, validated entitlement policy,
and authorized state transitions. Reading `/proc/PID/exe` authenticates the Linux
loader, not necessarily the loaded Mach-O. Rechecking a path also races replacement.

A successful Apple trust-cache query needs actual authenticated entries and the
right CDHash algorithm/cache generation, not a local pin list renamed to AMFI.
Apple describes [trust caches](https://support.apple.com/guide/security-pdf/trust-caches-sec7d38fbf97/web)
as part of its platform security chain. None of these protections can honestly
be supplied by the old fixed responses or by this standalone file verifier.


## Reproducing runtime and regression checks

```sh
python3 -m unittest discover -s tests
python3 scripts/prepare.py --check-only
python3 scripts/validate.py
```

The 70 regression tests include real temporary CA/leaf certificates and CMS
signatures, a linker-produced ad-hoc executable, code/identifier/signature
mutations, untrusted anchors, malformed/overlapping blobs, local cache matches
and misses, and compiled syscall bodies. Tests create no persistent keys.

After applying the source patches and building the base kernel objects:

```sh
python3 tools/build-riot-security-check.py
python3 tools/run-riot-native-check.py riot-security-state-check --library-path /Volumes/SystemRoot/ABSOLUTE_PROJECT_PATH/build/security-kernel-runtime
```

The builder only produces isolated files; it never installs the kernel. Local
validation reused the existing optimized kernel object response file through
`--objects-rsp`, replacing all three changed syscall objects. The runtime probe
ran against that isolated candidate, not the installed production library.
