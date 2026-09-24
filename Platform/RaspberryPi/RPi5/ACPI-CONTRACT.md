# Raspberry Pi 5 native driver contract, revision 1

The ACPI namespace describes BCM2712 and RP1 resource owners. A boot-generated
`RPIGRAPH` SSDT retains the board's complete firmware device-tree connection
data as ACPI packages. This includes clocks, resets, supplies, DMA requests,
pin groups, GPIO names, PHYs, firmware services, media endpoints, reserved
memory descriptions, and bootloader-applied overlays. Native Windows drivers
bind to the existing project HIDs and consume this contract through Acpi.sys.

This is a firmware interface for implementing those drivers. It does not make
Windows consume Linux bindings automatically. Initial RP1 GPIO, clock, UART,
I²C and SPI implementations are in the matching Windows driver repository.
Reset, DMA, mailbox, IOMMU, media and board-service drivers still need
implementations. ACPI resource
and PnP behavior on a physical Pi must be tested in addition to compilation and
ACPICA execution. Arbitrary overlay endpoints require a native bus/platform
driver to enumerate children from the connection graph, or an appropriate
SSDT with SPB connection resources and Windows hardware IDs.

## Resource and binding rules

* `_HID`/`_UID` identify physical functions; `_HRV` distinguishes the recognized
  BCM2712 layout (0 = C layout, 1 = D0) or the RP1 SYSINFO chip ID. IDs without a
  revision remain available for existing INF matches. Never parse the hardware
  ID list to decide which function a driver owns; use the INF or ACPI data.
* `_CRS` is authoritative for **CPU physical MMIO and GIC interrupts**. Use the
  translated PnP resource list. A graph `reg`, `ranges` or `interrupts` property
  is source binding data, not permission to map additional hardware.
* RP1 BAR1 and BAR2 are independently assigned by PCI. SRAM is not necessarily
  at peripheral base + 4 MiB. Firmware validates BAR2 before advertising SRAM.
* `_CCA = 0` remains in force. Current firmware programs a 64 GiB **identity**
  PCI inbound DMA aperture. Do not copy Linux's PCI `dma-ranges` translation
  into a Windows DMA address. Use the OS DMA adapter and actual bus-master
  address width; perform the required cache maintenance. RP1's local peripheral
  DMA addresses start at `0xC040000000`; CPU BAR addresses are a different view.
* `_DSD` properties under the device-properties UUID
  `daffd814-6eba-4d8c-8a91-bc9bbf4aa301` identify shared providers and contract
  revision. Provider links are absolute ACPI path **strings**, so method
  evaluation returns only the integer/string/buffer/package types in the WDK
  interface. Resolve them to the provider's PnP device/interface; do not treat
  a path as an OS handle. These links are data for the native drivers. They are not
  a substitute for Windows driver load order, interface acquisition, resource
  ownership, and PnP power relations. `_DEP` is not a general driver dependency
  mechanism; boot SD/USB/Ethernet/fan devices do not depend on new drivers.
* A controller being present does not activate its pins. Respect graph pin
  groups, status, controller/target roles, and endpoint configuration. The pin
  controller must arbitrate conflicting GPIO, UART, SPI, I2C, PWM, audio and
  display uses. Preserve firmware-configured boot functions until their owners
  explicitly transfer control.

The Ethernet GEM range, fan PWM1 range and conditional `RPI00F1` CID, RNG and
temperature ranges, SD host ranges/interrupts and Bay Trail `_DSM`, xHCI ranges,
and external PCI compatibility resources retain their previous layouts.

## Connection graph (`RPI1040`, `\_SB.DGRF`)

`_DSM` UUID: **a95b0d30-818e-4a96-a47b-726d07230501**, revision **1**.

| Function | Arg3 | Result |
| --- | --- | --- |
| 0 | ignored | Buffer `{0x0f}`: functions 0 through 3 supported |
| 1 | ignored | Package `{schema_version = 1, node_count, BCM_layout_revision}` |
| 2 | Package `{node_index}` | Node metadata, or an empty package for invalid input |
| 3 | Package `{node_index, property_index, byte_offset}` | Package `{name, total_byte_length, data_chunk}` (at most 4096 bytes), or an empty package for invalid input |

Unknown UUID/revision returns Buffer `{0}`. `_STA` is zero if the graph SSDT
could not be installed. Function 2 returns:

```
Package {
  absolute_DT_path_string,
  parent_node_index,             // 0xFFFFFFFF for the root
  ACPI_primary_owner_path,        // absolute string, or "" when unmapped
  property_count
}
```

Read properties with function 3; advance `byte_offset` by the returned chunk
length until reaching `total_byte_length`. The chunk is a Buffer when nonempty.
An offset equal to the property length returns **Integer zero** instead,
including for boolean properties. Acpi.sys cannot marshal an empty Buffer in
a returned package reliably; this marker represents zero bytes, not byte `00`.
Reject any other integer chunk value. This bounds
each ACPI V1 return argument below its 16-bit length limit (V2 has a larger
length field). Do not evaluate
the private `NODS` object, which contains the complete graph in fixed-size
package pages. Its internal representation is not a driver ABI.

All nodes and all property bytes are retained, in traversal order. Integers in
property buffers are **big endian DT cells**, strings retain their terminating
NULs, string lists remain NUL-separated, and a total length of zero represents
an empty/boolean property. The parent links and `#*-cells` properties supply
specifier widths. Resolve phandles against `phandle`/`linux,phandle` properties
within the same graph; do not persist node indices or phandle values across
boots. The original `__symbols__` and aliases are retained when supplied.

The firmware's hardware-address path map associates DT functions with ACPI
resource owners, including grouped display/MIPI devices. The SD hosts reference
their additional configuration-resource owners through `_DSD`. Empty owner strings
also occur for logical nodes, HAL-owned functions, pin groups and new overlay
children. They do not authorize creating arbitrary resource-owning PDOs. A bus
driver can enumerate a configured endpoint through its parent's existing
resource owner and publish appropriate Windows IDs. For an I2C camera, for
example, the graph retains the address, compatible string, supply and clock
references, GPIO specifiers, and remote media endpoint together.

Use `IOCTL_ACPI_EVAL_METHOD` on the graph PDO (or its provider interface);
allocate output according to the returned required size. A chunk may exceed
a small fixed buffer. Drivers find their records by the ACPI owner path and
DT compatible/address, not by an assumed node number. A graph provider can
index the records and offer typed clock/reset/pin/endpoint queries to clients.
There is no binary FDT header or OS FDT configuration-table dependency.

The encoder rejects an input or generated table over 16 MiB, paths that do not
fit 1024 bytes including the terminator, depth over 64 levels, property names
over 255 bytes, or collections over 32,640 nodes/properties. It never publishes
a truncated graph; installation failure leaves the graph provider absent.

Graph values are the bootloader description obtained from `FdtPlatformGetBase`.
UEFI resource allocation, PCI compatibility mode and DMA setup deliberately
override some Linux address mappings. Such mappings must be taken from `_CRS`
and this contract. The graph does not reserve Linux CMA pools on Windows.

## Shared resource owners

| ACPI device / HID | Responsibility |
| --- | --- |
| `RST0` / `RPI1021` | BCM2712 reset registers and PCIe PHY rescal |
| `SYS0` / `RPI1022` | AXI/USB arbitration syscon, CPU L2 interrupt controller and GSI 270 |
| `SDX0` / `RPI1023`, `SDX1` / `RPI1024` | SD/SDIO configuration banks; referenced from each SD host's `_DSD` |
| `BORD` / `RPI1025` | Board regulators, PHY reset, camera enables and LEDs through GPIO connections |
| `TIM0` / `RPI1026` | System counter and ARM-owned timer channels 1/3 (GSIs 97/99); channels 0/2 remain firmware-owned |
| `PERF` / `RPI1027` | Two AXI performance-counter banks |
| `PCIE` / `RPI1028` | External PCIe controller registers, excluding compatibility ECAM page, and MIP1 registers |
| `PWMA` / `RPI1029`, `AONI` / `RPI102A` | Original-tree AON PWM and BSC interrupt controller; present only when explicitly described by the boot DT |
| `FCLK`, `FRST`, `FPWR` / `RPI1030`–`RPI1032` | VideoCore firmware clock, reset and power-domain clients of MBX0 |
| `RP1B.IRQ0` / `RPI0011` | RP1 PCIe interrupt routing/SYSINFO, PCIe2 controller and MIP0 registers |
| `RP1B.SRAM` / `RPI0012` | Actual BAR2 and RP1 firmware shared memory |
| `RP1B.RST1` / `RPI0013` | RP1 peripheral reset register block, including atomic aliases |

These are cooperating provider interfaces, not independent permission for
clients to reset shared hardware. Do not reset the RP1 bridge, clocks, embedded
processors or firmware while xHCI, Ethernet, fan or another function is active.

`BORD` revision 2 (`_HRV = 2`) has seven separate, exclusive, output-only GPIO
connections in this order: main GPIO28 (Wi-Fi enable), AON GPIO4 (SD power),
AON GPIO9 (activity LED), RP1 GPIO32 (Ethernet PHY reset), RP1 GPIO34 (camera 0
enable), RP1 GPIO44 (power LED), RP1 GPIO46 (camera 1 enable). Earlier firmware
grouped these into three connections; a driver must check the revision and
resource count before using the new indices. Independent connections let a
client release an LED without releasing a supply or PHY reset line.

Polarity, startup delays and regulator settings are in the graph records.
LEDs and PHY reset are active-low; enables are active-high. Wi-Fi requires
150 ms startup delay. The Ethernet PHY is address 1; its reset requires a
5 ms pulse coordinated with the Ethernet driver. The board driver holds
Wi-Fi/SD power and PHY reset deasserted, and does not offer an uncoordinated
reset operation. RP1 GPIO support must cover the four named internal pins.
Fan GPIO45/channel3 and its 50 MHz clock handoff must be retained while
`RPI00F1` is advertised. Do not claim those pins independently.

## Windows 40-pin header

`RHPX` (`RPI1050`, compatible `MSFT8000`) exposes Resource Hub Proxy resources.
The first 52 descriptors are adjacent shared GPIO I/O and edge/active-both
interrupt pairs for GPIO2–27, in GPIO order. GPIO0/1 remain reserved. GPIO pin
numbers in this interface are native RP1 GPIO numbers, not header positions.
I²C descriptors 52–55 are I²C1/0/2/3 respectively, with placeholder address
0xFFFF and speed zero. I²C1 is therefore the default. SPI descriptors 56/57 are
SPI0 CS0/CS1, 8-bit modes 0–3 at 100 kHz–4 MHz. Runtime connection settings come
from Windows applications; these descriptors do not enable pins at boot.

Header UART0/2/3/4 retain HID `RPI0007` and gain compatible ID `RPI0070`.
Each `_CRS` has its existing 0x100-byte register resource, the shared GIC IRQ,
and one exclusive `PinFunction` resource. TX/RX routes are GPIO14/15 at function
4, GPIO4/5 at function 2, GPIO8/9 at function 2, and GPIO12/13 at function 2.
`SerCx-FriendlyName` publishes UART0/2/3/4 serial-interface names. UART1's HAT
pins and UART5's internal pins receive no header compatible ID.

Header I²C0–3 retain HID `RPI0005` and gain compatible ID `RPI0050`. Each has
its 0x1000-byte resource, shared IRQ and exclusive pull-up `PinFunction` 3:
SDA/SCL GPIO8/9, 2/3, 4/5 and 6/7. I²C4–6 remain unmodified internal instances.

SPI0 retains HID `RPI0006` and gains compatible ID `RPI0060`. Its 0x130-byte
register resource and shared IRQ are followed by `PinFunction` 0 for GPIO9/10/11
(MISO/MOSI/SCLK), then two exclusive pull-up/output-only `GpioIo` connections
for GPIO8 and GPIO7 (CS0 and CS1). The driver opens only the selected CS
connection when a target opens. GPIO-managed CS keeps the target selected
through FIFO refill gaps. No other SPI instance receives the compatible ID.

GpioClx owns mux arbitration; SerCx2 and SpbCx claim/release function connections
with target lifetime. Header drivers lease their local interrupt route through
the RP1 interrupt provider and acquire the shared clock provider. These
kernel-client APIs are defined in the Windows repository's `common` headers.
The complete provider set must be installed for all RHPX resources to resolve.
The graph remains descriptive data; it cannot authorize a driver to bypass
pin ownership or assume arbitrary alternate routes are usable.

FADT declares a control-method power button. `PNP0C0C` uses GIO0 `_AEI`: GPIO20, falling edge,
50 ms debounce. GIO0 `_EVT(20)` sends Notify `0x80`. It requires a functioning
GPIO event provider; this firmware does not claim unimplemented sleep wakeup.

## VideoCore mailbox ownership (`SOCB.MBX0`, `RPI1012`)

UEFI runtime RTC services initially own this mailbox. Merely receiving MBX0's
PnP resources does **not** grant permission to touch its hardware. Native
mailbox drivers must negotiate a one-way handoff first.

`_DSM` UUID: **a95b0d30-818e-4a96-a47b-726d07230502**, revision **1**.

| Function | Result |
| --- | --- |
| 0 | Buffer `{0x0F}` |
| 1 | Package `{1, native_request, firmware_active, transport_fault}` |
| 2 | Request native ownership: integer 0 success, 1 busy/timeout, 2 transport fault, 3 handoff unavailable |
| 3 | Package `{1, 0x80, 24, 0, 0xC0000000, 0x40000000}`: RTC-region version/space/bytes and CPU base/VideoCore base/window bytes |

Arg3 is unused. A query also returns integer 3 if the handoff page is unavailable.
Function 2 waits for an in-flight runtime transaction to finish. A timed-out
firmware transport latches a fault so ownership is never granted over an
unquiesced operation. A busy result can be retried. The native request remains
set; **there is no release operation before reboot**. Do not access registers
until function 2 returns zero. Subsequent UEFI mailbox transactions return
`EFI_UNSUPPORTED` without hardware access, including RTC GetTime/SetTime and
alarm operations. The native stack must provide its RTC/firmware services
before opting into this handoff. `Pi5Mailbox` checks function 3, prepares its
DMA buffer, and registers the RTC operation-region handler before requesting
ownership. A failed ownership attempt must be treated as irreversible too.

The mailbox is under `SOCB` so its DMA aperture describes CPU `[0, 1 GiB)`
at VideoCore `[0xC0000000, 4 GiB)`. Its MMIO remains CPU-physical with no
translation. Windows 26200 returns an identity logical address for this
non-PCI common-buffer adapter despite `_DMA`; a native owner must constrain
the HAL allocation to the aperture and validate the mapping before applying
the explicit VideoCore alias. Never assume an arbitrary CPU or pool address
is usable for DMA. The original HAL address is retained for buffer release.

`RTC0` is an `ACPI000E` Time and Alarm Device (`RPI101B` compatible ID), with
an operation-region `_DEP` on `SOCB.MBX0`. `_GCP` advertises only real time,
at one-second resolution. Wake alarms are not advertised. `_GRT` and `_SRT`
call the same serialized `MBX0.RTIM` method. It exchanges the standard
16-byte ACPI real-time buffer through vendor region space `0x80`, offset 0,
length 24: command DWORD (1 get, 2 set), status DWORD (0 success,
`0xFFFFFFFF` failure), then four time DWORDs. `_REG` gates access. The
mailbox function driver registers this region on its own PDO, serializes
commands with other mailbox clients, and translates PMIC UTC epoch seconds.
Failed gets return a zeroed time buffer with Valid=0; failed sets return
`0xFFFFFFFF`. The same PMIC source backs UEFI time before the handoff.

The handoff flags reside in a dedicated runtime page with separate cache lines
for request, active and fault. They are accessed through AML, not mapped by a
native driver. Firmware serializes its callers, publishes active before
sampling request, and orders/cleans accesses. Only the mailbox provider may
perform native transactions; clock, power, reset, RTC and other property-tag
clients must share its serialization and DMA-buffer ownership.

## AON GPIO bank 0 and SD voltage (`GIO1`, `RPI1020`, UID 1)

All bank-0 DATA accesses use this AML broker, including the native GPIO driver.
Direct read-modify-write on MMIO offset 4 races SD voltage switching. Other
GPIO registers and bank 1 remain available to the native GPIO driver. Implement
this AON controller with passive-level access; it has no verified D0 interrupt
route in the current reference tree.

`_DSM` UUID: **a95b0d30-818e-4a96-a47b-726d07230503**, revision **1**.

| Function | Arg3 | Result |
| --- | --- | --- |
| 0 | ignored | Buffer `{0x07}` |
| 1 | ignored | Bank-0 DATA value |
| 2 | Package `{mask, value}` | Updated DATA; all-ones integer on invalid input, nonexistent pins, or an attempt to write pin 3 |

The shared AML mutex protects all DATA read/modify/write operations. The SD
host's existing voltage `_DSM` retains exclusive pin-3 control and waits the
DT's 5 ms regulator settling time. No new GPIO-driver dependency is placed on
the boot SD host. `UPDT`/`SDVL` are internal AML helpers, not driver interfaces.

## Interrupts, DMA and multimedia

RP1 children retain **shared, level-high GSI 261**. Their local source numbers
are `raspberrypi,rp1-interrupts`, with the IRQ0 provider reference in `_DSD`.
The PIO local sources 50/51 are edge-triggered internally; that does not change
the upstream GIC line into an edge interrupt. Only USB sources 31/36 are enabled
by firmware and remain reserved for the inbox xHCI controllers. An IRQ provider
must preserve them and the existing INTA transport, enable only requested
sources, and provide interrupt-safe acknowledge/mask operations to clients.
RP1 MSI-X reconfiguration must never happen while these ACPI xHCI devices run.

BCM DMA has two resource banks, global channels 0–5 and 6–11. Firmware patches
separate masks and their union from the boot DT. Both audited D0 trees assign
`0x003f` and `0x07c0`, union **0x07ff**. The physical test board's bootloader DT
instead assigns `0x003f` and `0x0f80`, union **0x0fbf**. Do not hard-code either
assignment: `_CRS` includes interrupts only for channels assigned at this boot. Missing masks
expose no DMA device. DMA request specifiers/flags are retained in the graph.
This describes a native DMA service; it does not advertise an unimplemented
Windows HAL DMA extension/CSRT interface.

`MMU0` resources are IOMMU2, IOMMU4, IOMMU5 and the common translation cache,
in that order. PiSP BE references IOMMU2 with a 32-bit IOVA aperture; the display
engine references IOMMU4; RP1 CSI/DSI/DPI/VEC reference IOMMU5 and the
`0x1000000000` IOVA offset. A common provider must serialize cache management
and own mappings. These are BCM2712 multimedia IOMMUs, not an ARM SMMU that can
be made supported by inventing an IORT entry.

DISP owns shared HDMI/HD/DDC/L2 registers once. MIP0/1 each own combined
CSI/DSI/front-end registers; VID0 owns shared DPI/VEC configuration. Their graph
records retain individual register names, local interrupts, clocks, DMA
requests and endpoint relationships. A grouped driver must arbitrate shared
blocks. SRAM has **no advertised general-purpose allocation pool**: use only
the described mailbox area at BAR2 + `0xff00`, size `0x100`, under the firmware
provider's ownership; the rest may contain live firmware state.

## PCI and reference differences

The existing Windows single-function ECAM compatibility mode and NVMe path are
preserved. PCIE references PCI1 and advertises its actual maximum config bus.
Full PCIe topology/MSI needs native Windows platform/HAL support for the
existing Arm DEN0115 configuration service and BCM MSI controller. A function
driver must not repoint the config index behind PCI.sys or reset an active root.
Select the firmware's native DEN0115 mode only with an OS stack that supports
it. The MSI doorbell address in the graph is not a CPU MMIO allocation.

CPU/GIC/PMU/architected timer/cache/PSCI remain represented by DSDT plus
MADT/GTDT/PPTT/FADT. Linux `gpiomem` nodes are aliases of GPIO/pinctrl owners,
not additional conflicting resource consumers. Placeholder bootloader NVMEM
nodes and disabled/unwired PCIe0 are retained as graph data. The old D0 overlay
contains an AON IRQ through a disabled controller and old SD pad ranges that
the newer explicit D0 tree removes. SDX0 includes those pad ranges only when
the boot DT explicitly supplies the recognized registers. No unverified AON interrupt is advertised;
the recognized D0 pinctrl register sizes are authoritative over stale aliases.
The original tree also contains AON PWM/BSC interrupt providers absent from the
newer tree; their `_STA` follows validated boot-DT presence and status. The BSC
provider is separate from GIO1 and does not establish an AON GPIO IRQ route.
Legacy controllers disabled in the original tree and removed from the newer
tree are retained as graph data, without guessed resource ownership. This
includes blocks removed in D0 silicon. The unused `bcm2836-l1-intc` node is also
graph-only; CPU interrupt delivery uses the GIC described by MADT.

References: [original firmware base](https://github.com/raspberrypi/firmware/tree/1e403e23baab5673f0494a200f57cd01287d5b1a/boot),
[D0 overlay](https://github.com/raspberrypi/firmware/blob/bead686816848038563a542dc854346ab13253a2/boot/overlays/bcm2712d0.dtbo),
[explicit D0 source](https://github.com/raspberrypi/linux/blob/c2376b915276625f6119799ac64aa17ba161b367/arch/arm64/boot/dts/broadcom/bcm2712d0-rpi-5-b.dts),
[RP1 peripheral specification](https://datasheets.raspberrypi.com/rp1/rp1-peripherals.pdf),
[Windows namespace/driver requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/device-management-namespace-objects),
[Windows ACPI V1 argument layout](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/acpiioct/ns-acpiioct-_acpi_method_argument_v1),
[D0 hardware pruning](https://github.com/raspberrypi/linux/commit/8be0890e7464324e66c2821989352f01b412aae0),
[unused local interrupt node removal](https://lists.openwall.net/linux-kernel/2024/12/12/1325).
