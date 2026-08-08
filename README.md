# Qualcomm Secure Execution Environment supplicant

[![Continuous integration](https://github.com/wrobelda/qsee-supplicant/actions/workflows/ci.yml/badge.svg)](https://github.com/wrobelda/qsee-supplicant/actions/workflows/ci.yml)

A Trusted Execution Environment (TEE) is an isolated execution environment
separate from the host operating system. Qualcomm Secure Execution Environment
(QSEE) is the older of Qualcomm's TEE implementations. Qualcomm also provides the
newer Qualcomm TEE (QTEE) object interface.

Programs running inside a TEE are called Trusted
Applications (TAs). A TA can perform security-sensitive work without giving the
host operating system direct access to its memory and internal state.

These Qualcomm TEE implementations are used by a range of Qualcomm
systems-on-chip (SoCs), typically in Android devices, for which Qualcomm
provides a complete userspace software stack.

A userspace stack is required because some TAs need operations that only the
host operating system can provide, such as reading and writing files. They request
those operations through listener services. These services run in the host
operating system as handlers in a userspace daemon called a supplicant. An
additional daemon is required to manage dynamic TA lifetimes by loading them
into QSEE and keeping them loaded.

As mentioned, Qualcomm provides their closed-source stack for Android. Its
proprietary nature aside, it cannot be used on native Linux because it uses
Qualcomm's proprietary QSEECOM kernel interface, not the Linux TEE subsystem.

Qualcomm *does* provide a native, open-source Linux stack for the newer QTEE
technology. No equivalent stack exists for QSEE.

This project provides that missing stack: userspace daemons that manage QSEE TA
lifetimes and provide operating-system services to TAs through listeners.

## QSEE communication interface and listener services

Older QSEE systems use the QSEE communication interface (QSEECOM) to load TAs,
send commands to them, and carry numeric listener requests. `qsee-supplicant`
implements the userspace listener side of the Linux QSEECOM TEE driver. It is a
single machine-wide service because the kernel provides one listener request
queue for each QSEECOM TEE device, not one queue for each TA.

This project requires the QSEECOM TEE driver provided by
[`goodix-fp-spi-linux`](https://github.com/wrobelda/goodix-fp-spi-linux).
That driver implements QSEECOM through the Linux TEE subsystem and exposes the
`/dev/teeprivN` device used by the supplicant and loader. The driver is not yet
included in mainline Linux, so the running kernel must include the driver from
that repository.

As explained in the overview, some TAs need operations that only the host
operating system can provide, such as reading and writing files. They request
those operations through listener services. Each listener has a numeric service
identifier and a defined request and response format. A supplicant is a daemon
running in the normal Linux environment. It registers the listeners, receives
requests from the TEE, performs the permitted operations, and returns the
results.

The current implementation provides two filesystem listener protocols. Its
listener handlers are separate from the kernel request loop, so further QSEECOM
listener protocols can be implemented without rewriting device discovery,
registration, or request delivery.

## Programs and services

The project installs two programs because listener registration and TA loading
have separate lifetimes and security domains. The supplicant handles
machine-wide listener requests and sensitive persistent objects. Each loader
holds the load authority and kernel session for one named TA. Keeping these
roles in separate processes prevents a loader from sharing the supplicant's
request-processing state or storage descriptors:

- `qsee-supplicant` is the single machine-wide listener daemon. It registers
  the File System (FS) and GlobalPlatform File System (GPFS) listener services,
  processes their requests, and stores their files below the configured state
  directory.
- `qsee-app-loader` loads one dynamic TA and keeps its kernel session open so
  the TA remains loaded. One loader process is used for each TA that the system
  needs to keep resident. The program takes the TA firmware name as its only
  argument.

Together, these programs provide the listener-service and dynamic-TA lifetime
functions normally handled by Qualcomm's `qseecomd` on Android.

The supplied systemd units reflect this split. `qsee-supplicant.service` runs
the listener daemon. `qsee-app-loader@.service` is a template unit; an instance
such as `qsee-app-loader@example-ta.service` runs
`qsee-app-loader example-ta`. The template wants and starts after the
supplicant because a TA may request listener services as soon as it is loaded.
If the supplicant stops or restarts, systemd leaves the loader processes
running so they retain their application sessions while listener services are
re-registered.

Keeping the kernel sessions in separate processes is intentional. Restarting
the supplicant re-registers its listeners without closing the loader's session
or unloading an already resident TA. Restarting one loader affects only its TA.
The supplicant does not contain a list of TAs and does not load them itself.

## Supported listener services

A QSEECOM TA can ask normal Linux software to perform an operation by sending a
request to a numbered listener service. The supplicant registers each supported
listener with the kernel through `/dev/teeprivN`. The kernel delivers requests
for all registered listeners through the Linux TEE subsystem's
`TEE_IOC_SUPPL_RECV` receive operation. The supplicant selects the handler using
the listener number returned in `arg.func`.

This release implements the two file services below:

| Listener | Service | Status |
|---:|---|---|
| 10 | File system (FS) | Implemented and tested |
| 28672 | GlobalPlatform file system (GPFS) | Implemented and tested |

FS and GPFS are separate protocols. FS provides conventional operations on
files and directories. GPFS stores objects using its own request format,
including atomic replacement and protocol-requested backup creation. Supporting
one does not provide the other.

Qualcomm's Android `qseecomd` registers more numeric listener services. The
following services are known but are not implemented by this project:

| Listener | Service |
|---:|---|
| 11 | Time |
| 4352 | GlobalPlatform request cancellation |
| 8192 | Replay Protected Memory Block (RPMB) |
| 12288 | Service labelled `SSD` by `qseecomd`; its full name has not been established |
| 16384 | Secure user interface (secure UI) |
| 36864 | Interrupt |
| 45056 | Secure processor |

This is not an exhaustive QSEECOM service registry. It records the listener
numbers identified in the available `qseecomd` traces; those traces contained
one additional registration whose service number was not captured. A service
should be added only with its request format, response format, and error
behavior documented and tested. A TA needs only the listeners that it actually
uses.

Qualcomm's Mink inter-process communication (MinkIPC)
[`minkipc` listener implementations](https://github.com/qualcomm/minkipc/tree/main/listeners)
provide a public reference for FS 10, time 11, RPMB 8192, and GPFS 28672.
MinkIPC is the newer, broader object interface, but its listener services retain
the numeric service identifiers and shared-buffer message formats used by older
QSEECOM. Legacy QSEECOM exposes this listener subset through numeric session
registration and one supplicant receive queue. MinkIPC exposes the same services
as callback objects registered with QTEE. The service protocol definitions and
handlers therefore apply to both. Only the registration and request-delivery
code differs. This project implements the legacy QSEECOM side and used MinkIPC
to verify the service protocols; it does not include MinkIPC source code.

## Build and test

```sh
make
make check
make DESTDIR="$pkgdir" install
```

The tests use a temporary directory and do not require TEE hardware. They cover
listener request parsing, safe path handling, restrictive file modes, atomic
GPFS object replacement, and protocol-required backup files.

## Configuration and state

```sh
qsee-supplicant --state-dir /var/lib/qsee-supplicant
```

The FS and GPFS listener protocols allow a TA to include a pathname in a
file-operation request. Legacy QSEECOM was predominantly used
on Android devices, so TAs written for it commonly send Android pathnames such
as `/data/vendor/secureapp/object.so`. The daemon treats these as names within
its own storage namespace, not as paths to files elsewhere on the Linux host.

`--state-dir` selects the root of that namespace. With
`--state-dir /var/lib/qsee-supplicant`, a request for
`/data/vendor/secureapp/object.so` accesses
`/var/lib/qsee-supplicant/data/vendor/secureapp/object.so`.

The daemon rejects any pathname that cannot be confined to this directory. This
includes a parent-directory component such as `..`, a symbolic link in any
component of the path, and control characters. Protocol pathname fields have a
fixed size and must contain a terminating null byte; a request without one is
malformed and is rejected. These checks prevent a trusted application from
using the listener to access files outside the configured state directory.

Directories and objects are created 0700 and 0600. Do not pre-create empty
objects unless the requesting TA's protocol requires them; an empty object and
a missing object have different filesystem semantics.
Back up the entire state directory only as sensitive, device-bound data.

The supplied systemd and OpenRC units run as root. The Linux QSEECOM TEE driver
requires the Linux `CAP_SYS_ADMIN` administrative capability and normally
creates `/dev/teeprivN` mode 0600 owned by root, so merely assigning the
capability to a dedicated account is not enough. The systemd unit bounds
capabilities to `CAP_SYS_ADMIN` and confines filesystem, network, and kernel
access. A downstream may use a dedicated account only when its udev and Linux
Security Module (LSM) policy also grants that account access to the TEE node and
state directory.

## Lifecycle

Start the daemon before any service that may load a dependent TA. Listener
registration is logged as `event=listener_registered`. Loss of the TEE device
closes every registration and every outstanding listener-side file handle,
then retries discovery with bounded exponential
backoff. The termination and interrupt signals (`SIGTERM` and `SIGINT`) close
`/dev/teeprivN`, which unregisters the listeners, and cause a clean exit.
Service managers should restart a failed daemon.

Only one legacy-QSEECOM supplicant may serve a TEE device.

For each dynamic TA that must remain loaded, enable the loader instance named
after its firmware name. For example:

```sh
systemctl enable --now qsee-supplicant.service
systemctl enable --now qsee-app-loader@example-ta.service
```

## TODO

Application loading is functional, but restart recovery, readiness reporting,
and application lifecycle reporting are not implemented yet:

- [ ] Make `qsee-app-loader` attach to an application that is already loaded before
  attempting a privileged load. If loading races with another process, retry
  the unprivileged attach. This allows a loader to restart while existing client
  sessions keep the application resident.
- [ ] Change the loader service to report readiness only after it has loaded or
  attached to its application. `After=` alone only orders process startup; it
  does not report that the application is available.
- [ ] Expose loaded QSEECOM applications as kernel devices with sysfs state and
  udev add/remove events. The Linux TEE core already provides a `tee` bus for
  TEE client devices. OP-TEE enumerates advertised device TAs and registers
  devices named `optee-ta-<UUID>` on that bus, with
  `MODALIAS=tee:<UUID>`. These devices describe advertised services rather than
  the current loaded-session set. The QTEE driver does not currently register
  loaded objects or applications there. QSEECOM identifies applications by
  name rather than UUID, so reusing the `tee` bus requires a defined name-based
  identity and modalias convention; a separate QSEECOM-only class should not be
  introduced without first resolving that generic interface.
