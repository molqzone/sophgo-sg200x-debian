#!/usr/bin/env python3
"""Send a Linux->C906L command over the shared page + mailbox doorbell.

Runs ON THE BOARD as root (needs /dev/mem for the shared page and the mailbox,
and /dev/cvi-rtos-cmdqu for the doorbell):

    sudo python3 /usr/bin/guidance-cmd.py format
    sudo python3 /usr/bin/guidance-cmd.py ping
    sudo python3 /usr/bin/guidance-cmd.py led on
    sudo python3 /usr/bin/guidance-cmd.py led off
    sudo python3 /usr/bin/guidance-cmd.py status
    sudo python3 /usr/bin/guidance-cmd.py log [--new]
    sudo python3 /usr/bin/guidance-cmd.py park
    sudo python3 /usr/bin/guidance-cmd.py restart

`log` streams the C906L console from the shared-page log ring (the firmware's
XR_LOG/STDIO output; Ctrl-C stops). `--new` skips the bytes already retained
in the 1460-byte ring and only follows new output.

Protocol: format the shared page if needed, write the 24-byte Command slot at
shared-page offset 4064, then publish one mailbox request (ip_id=IP_SYSTEM,
cmd_id=0x7f) into a context slot and ring the doorbell by writing the mailbox
registers from /dev/mem -- /dev/cvi-rtos-cmdqu only exists on images that
load the vendor media chain. The C906L mailbox ISR wakes the command task,
which reads the slot, acts on it, and writes the reply into a mailbox slot
(ip_id echoed, rtos_valid=1) with the command seq as the payload word.

The reply is polled from the mailbox buffer instead of completing through
RTOS_CMDQU_SEND_WAIT: on the maixcam image the ca53-side mailbox interrupt
(PLIC 101, Linux IRQ 32) is registered by rtos_cmdqu but never fires, so the
kernel's wait list can never be woken and every SEND_WAIT ends in ETIMEDOUT.
The reply bytes themselves are latched and readable, so this tool polls them
and recycles the slot afterwards. Consumed slots are cleared by zeroing the
valid byte; /dev/mem commits that byte write on this image.

Slot offsets come from shared-page-xr's PageLayout: the region sits at
PAGE_SIZE - sizeof(Region<Command>) = 4096 - 32 = 4064, with write_state at
4088 and seq at 4092; see User/guidance_contract.hpp for the payload layout.
The page header is shared-page-xr's PageHeader (32 bytes).
"""

import fcntl
import glob
import mmap
import os
import struct
import sys
import time

SHARED_PAGE_PHYS = 0x8FFDC000
MAILBOX_PHYS = 0x01900000
MAILBOX_CONTEXT = 0x01900400
REGION_PAYLOAD_OFFSET = 4064
REGION_STATE_OFFSET = REGION_PAYLOAD_OFFSET + 24
REGION_SEQ_OFFSET = REGION_STATE_OFFSET + 4
HEADER_SIZE = 32
PAGE_SIZE = 4096

PAGE_MAGIC = 0x31506461  # shared-page-xr PAGE_MAGIC
ABI_VERSION = 1
SLOT_COUNT = 64
SAMPLE_SIZE = 40
PAYLOAD_SIZE = 24
WIRE_TAG = 1  # guidance::WIRE_TAG

CMD_MAGIC = 0x31444D47  # "GMD1", guidance::GUIDANCE_CMD_MAGIC
CMD_PING = 1
CMD_LED = 2
CMD_PARK = 3
REJECTED = 0xFFFFFFFF

PAGE_LOG_OFFSET = 32 + 64 * 40 + 8  # firmware PageLogRing (User/main.cpp)
PAGE_LOG_INDEX_OFFSET = PAGE_LOG_OFFSET
PAGE_LOG_BYTES_OFFSET = PAGE_LOG_OFFSET + 4
PAGE_LOG_RING_SIZE = 1460

IP_SYSTEM = 6
DOORBELL_CMD = 0x7F
DOORBELL_BLOCK = 1
DOORBELL_TIMEOUT_MS = 1000
REPLY_TIMEOUT_S = 3.0

CMDQU_DEV = "/dev/cvi-rtos-cmdqu"
RTOS_CMDQU_SEND = 0x40087201  # _IOW('r', 1, unsigned long), non-blocking

# Mirror of the vendor cmdqu_t (packed, 8 bytes).
CMDQU = struct.Struct("<BBHI")


class Page:
    """Coherent-ish access to one shared page (no caching on the C906L)."""

    def __init__(self, phys):
        self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.mm = mmap.mmap(self.fd, PAGE_SIZE, offset=phys)

    def read(self, offset, size):
        self.mm.seek(offset)
        return self.mm.read(size)

    def write(self, offset, data):
        self.mm.seek(offset)
        self.mm.write(data)

    def u32(self, offset):
        return struct.unpack("<I", self.read(offset, 4))[0]

    def put_u32(self, offset, value):
        self.write(offset, struct.pack("<I", value))


class Mailbox:
    """Mailbox register/buffer view for reply polling (reads only + valid
    byte clears)."""

    def __init__(self):
        self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.mm = mmap.mmap(self.fd, 0x1000, offset=MAILBOX_PHYS)

    def slot(self, index):
        base = MAILBOX_CONTEXT - MAILBOX_PHYS + index * 8
        b = self.mm[base:base + 8]
        ip = b[0]
        cmd_block = b[1]
        resv = struct.unpack("<H", b[2:4])[0]
        param = struct.unpack("<I", b[4:8])[0]
        return {
            "ip": ip,
            "cmd": cmd_block & 0x7F,
            "block": (cmd_block >> 7) & 1,
            "linux_valid": resv & 0xFF,
            "rtos_valid": resv >> 8,
            "param": param,
        }

    def read32(self, offset):
        return struct.unpack_from("<I", self.mm, offset)[0]

    def write32(self, offset, value):
        struct.pack_into("<I", self.mm, offset, value)

    def clear_slot(self, index):
        """Zero the slot's valid byte so the driver treats it as free."""
        base = MAILBOX_CONTEXT - MAILBOX_PHYS + index * 8
        struct.pack_into("<I", self.mm, base, 0)

    def drop_stale_replies(self):
        """Free reply slots left behind by earlier commands so the kernel's
        send finds a free slot and the poll only sees fresh replies."""
        dropped = 0
        for index in range(8):
            s = self.slot(index)
            if s["rtos_valid"] != 0 and s["linux_valid"] == 0:
                self.clear_slot(index)
                dropped += 1
        return dropped


PAGE = Page(SHARED_PAGE_PHYS)
MAILBOX = Mailbox()


def header_fields():
    return struct.unpack("<8I", PAGE.read(0, HEADER_SIZE))


def page_ready():
    (magic, abi, page_size, slots, sample, payload, region, tag) = header_fields()
    return (
        magic == PAGE_MAGIC
        and abi == ABI_VERSION
        and page_size == PAGE_SIZE
        and slots == SLOT_COUNT
        and sample == SAMPLE_SIZE
        and payload == PAYLOAD_SIZE
        and region == REGION_PAYLOAD_OFFSET
        and tag == WIRE_TAG
    )


def format_page():
    PAGE.write(0, bytes(PAGE_SIZE))
    PAGE.write(0, struct.pack("<8I", PAGE_MAGIC, ABI_VERSION, PAGE_SIZE,
                              SLOT_COUNT, SAMPLE_SIZE, PAYLOAD_SIZE,
                              REGION_PAYLOAD_OFFSET, WIRE_TAG))


def encode_command(kind, payload=b""):
    return struct.pack("<IHH16s", CMD_MAGIC, kind, next_seq(), payload[:16])


def next_seq():
    """Read the region's current seq so Linux increments it per command.

    The command slot carries a 16-bit seq while the region counter is 32-bit;
    the tool keeps them in lockstep so each accepted command bumps the slot
    seq by exactly one.
    """
    return (PAGE.u32(REGION_SEQ_OFFSET) + 1) & 0xFFFF


def write_command(payload):
    """Publish the slot: payload, then the matching region seq as release."""
    PAGE.write(REGION_PAYLOAD_OFFSET, payload)
    PAGE.put_u32(REGION_SEQ_OFFSET, struct.unpack("<H", payload[6:8])[0])


def send_doorbell(seq):
    """Ring the mailbox doorbell through the vendor cmdqu driver.

    The context buffer is only writable through the driver: a raw store from
    /dev/mem is taken as a whole 32-bit write with the other three byte lanes
    zeroed, so a payload plus header can never be composed here.  The driver's
    memcpy_toio path writes words and works.  Fall back to the direct register
    sequence only when the device node is missing, and say so.
    """
    # cmdqu_t packs cmd_id:7 | block:1 into one byte; the kernel driver
    # overwrites the slot's resv with its own valid bits, so ip_id, this
    # command byte and param_ptr are all that reach the c906L.
    cmd_block = DOORBELL_CMD | (DOORBELL_BLOCK << 7)
    if os.path.exists(CMDQU_DEV):
        fd = os.open(CMDQU_DEV, os.O_RDWR)
        try:
            fcntl.ioctl(fd, RTOS_CMDQU_SEND,
                        CMDQU.pack(IP_SYSTEM, cmd_block, DOORBELL_BLOCK, seq))
            return
        finally:
            os.close(fd)

    slot = None
    for index in range(8):
        state = MAILBOX.slot(index)
        if not state["linux_valid"] and not state["rtos_valid"]:
            slot = index
            break
    if slot is None:
        raise OSError("no free mailbox slot")

    base = MAILBOX_CONTEXT - MAILBOX_PHYS + slot * 8
    MAILBOX.write32(base + 4, seq)                            # param word
    header = IP_SYSTEM | (cmd_block << 8) | (1 << 16)         # ip, cmd/block, linux_valid
    MAILBOX.write32(base, header)
    bit = 1 << slot
    MAILBOX.write32(0x08, MAILBOX.read32(0x08) | bit)         # enable[CPU2]
    MAILBOX.write32(0x30, bit)                                # set[CPU2].clr
    MAILBOX.write32(0x60, bit)                                # mbox_set doorbell


def poll_reply(seq):
    """Wait for the C906L reply slot and recycle it.

    @return reply param word, or None on timeout.
    """
    deadline = time.monotonic() + REPLY_TIMEOUT_S
    while True:
        for index in range(8):
            s = MAILBOX.slot(index)
            if (s["ip"] == IP_SYSTEM and s["cmd"] == DOORBELL_CMD and
                    s["block"] == DOORBELL_BLOCK and s["rtos_valid"] == 1 and
                    s["linux_valid"] == 0):
                param = s["param"]
                MAILBOX.clear_slot(index)
                return param
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.02)


def rproc_device():
    """Locate the cv181x-c906_1 remoteproc sysfs directory."""
    for entry in sorted(glob.glob("/sys/class/remoteproc/remoteproc*")):
        if os.path.exists(os.path.join(entry, "state")):
            return entry
    return None


def restart_cycle():
    """Park the C906L (quiesce, in-flight DMA drained) then cycle remoteproc.

    Resetting the remote processor while it is bus-mastering stalls the shared
    AXI fabric and kills the USB network data path; parking first makes the
    reset land on an idle core.
    """
    rc = run_command(CMD_PARK)
    if rc != 0:
        return rc
    dev = rproc_device()
    if dev is None:
        print("no remoteproc device with a state attribute found")
        return 1
    with open(os.path.join(dev, "state"), "w") as handle:
        handle.write("stop")
    time.sleep(0.5)
    with open(os.path.join(dev, "state"), "w") as handle:
        handle.write("start")
    time.sleep(1.0)
    with open(os.path.join(dev, "state")) as handle:
        print("rproc state: %s" % handle.read().strip())
    return 0


def run_command(kind, payload=b""):
    if not page_ready():
        print("shared page unformatted; formatting...")
        format_page()
    payload_bytes = encode_command(kind, payload)
    seq = struct.unpack("<H", payload_bytes[6:8])[0]
    dropped = MAILBOX.drop_stale_replies()
    if dropped:
        print("recycled %d stale reply slot(s)" % dropped)
    write_command(payload_bytes)
    send_doorbell(seq)
    param = poll_reply(seq)
    if param is None:
        print("no doorbell reply within %.1fs (C906L command task not running?)" %
              REPLY_TIMEOUT_S)
        return 1
    if param == REJECTED:
        print("rejected by C906L (page unformatted or invalid command)")
        return 1
    if param != seq:
        print("warning: reply param 0x%08x does not echo seq %d" % (param, seq))
    print("accepted, reply param=0x%08x" % param)
    return 0


def log_stream(from_start):
    """Stream the C906L console ring (shared-page log) to stdout.

    The firmware appends every console byte to the ring (User/main.cpp
    AppendToPageLog); write_index counts total bytes and wraps at 2^32.
    """
    total = PAGE.u32(PAGE_LOG_INDEX_OFFSET)
    if from_start or total < PAGE_LOG_RING_SIZE:
        seen = 0
    else:
        seen = total - PAGE_LOG_RING_SIZE
    try:
        while True:
            total = PAGE.u32(PAGE_LOG_INDEX_OFFSET)
            if total < seen:
                seen = 0 if total < PAGE_LOG_RING_SIZE else total - PAGE_LOG_RING_SIZE
            if total - seen > PAGE_LOG_RING_SIZE:
                sys.stderr.write(
                    "[guidance-cmd] ring overrun, %d bytes lost\n"
                    % (total - seen - PAGE_LOG_RING_SIZE))
                seen = total - PAGE_LOG_RING_SIZE
            while seen < total:
                start = seen % PAGE_LOG_RING_SIZE
                chunk = min(total - seen, PAGE_LOG_RING_SIZE - start, 256)
                sys.stdout.buffer.write(
                    PAGE.read(PAGE_LOG_BYTES_OFFSET + start, chunk))
                sys.stdout.flush()
                seen += chunk
            time.sleep(0.2)
    except KeyboardInterrupt:
        sys.stdout.flush()
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] not in ("ping", "led", "status", "format",
                                           "log", "park", "restart"):
        print(__doc__.strip())
        return 2

    if argv[1] == "status":
        print("page header      : %s" % ("FORMATTED" if page_ready()
                                         else "UNFORMATTED %s" %
                                         (header_fields(),)))
        print("region seq      :", PAGE.u32(REGION_SEQ_OFFSET))
        print("write_state     :", PAGE.u32(REGION_STATE_OFFSET))
        magic, kind, seq = struct.unpack("<IHH", PAGE.read(REGION_PAYLOAD_OFFSET, 8))
        print("last command    : magic=0x%08x type=%d seq=%d" % (magic, kind, seq))
        for index in range(8):
            s = MAILBOX.slot(index)
            if s["rtos_valid"] or s["linux_valid"] or s["ip"] or s["param"]:
                print("mailbox slot%d   : ip=%d cmd=0x%02x blk=%d lin=%d rto=%d "
                      "param=0x%08x" % (index, s["ip"], s["cmd"], s["block"],
                                        s["linux_valid"], s["rtos_valid"],
                                        s["param"]))
        return 0

    if argv[1] == "format":
        format_page()
        print("page formatted (%s)" % ("FORMATTED" if page_ready() else "FAILED",))
        return 0 if page_ready() else 1

    if argv[1] == "log":
        return log_stream("--new" not in argv[2:])

    if argv[1] == "park":
        return run_command(CMD_PARK)

    if argv[1] == "restart":
        return restart_cycle()

    if argv[1] == "ping":
        return run_command(CMD_PING)

    level = (argv[2] if len(argv) > 2 else "").lower()
    if level not in ("on", "off"):
        print("usage: guidance-cmd.py led on|off")
        return 2
    return run_command(CMD_LED, bytes([1 if level == "on" else 0]))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
