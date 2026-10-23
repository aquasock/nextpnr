#!/usr/bin/env python3
"""Gate check: an internal net connects bel pins and nothing else.

`create_internal_net` models a link the silicon hard-wires -- `DQS` to a
serializer's `TCLK`, `DDRDLL` to a `DQS`'s `DLLSTEP`.  The whole point is that
it carries no configuration: the vendor tool refuses to let a design route
those nets itself, so the model must not offer the router a way in or out
either.  A pip appearing on one of these wires would silently turn a
hard-wired link into a routable one and let a design take a path the hardware
does not have.

  test_internal_net_has_no_pips
      The wire exists, both bel pins are attached to it, and the tile type has
      no pip whose source or sink is that wire.

  test_internal_net_rejects_a_second_definition
      Creating the same internal net twice raises instead of quietly
      attaching more pins to the first wire.

Run it with Apicula on the path, as the generator itself needs it:

    source tools/openflow_env.sh && python3 tests/check_internal_net.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))

from himbaechel_dbgen.chip import Chip, PinType  # noqa: E402
import gowin_arch_gen as gen  # noqa: E402


def _tile():
    chip = Chip("gowin", "TEST", 2, 2)
    tt = chip.create_tile_type("TEST_TILE")
    src = tt.create_bel("DQS0", "DQS", z=700)
    dst = tt.create_bel("OSER0", "OSER8_MEM", z=701)
    return tt, src, dst


def test_internal_net_has_no_pips():
    tt, src, dst = _tile()
    gen.create_internal_net(
        tt, "INT_DQSW0", [(src, "DQSW0", PinType.OUTPUT),
                          (dst, "TCLK", PinType.INPUT)])
    assert tt.has_wire("INT_DQSW0"), "the internal net was not created"
    wire = tt.wires[tt._wire2idx[tt.strs.id("INT_DQSW0")]]
    assert len(wire.bel_pins) == 2, f"expected 2 bel pins, got {len(wire.bel_pins)}"
    for pip in tt.pips:
        assert pip.src_wire != wire.index and pip.dst_wire != wire.index, \
            "an internal net must not carry pips"


def test_internal_net_rejects_a_second_definition():
    tt, src, dst = _tile()
    gen.create_internal_net(tt, "INT_X", [(src, "DQSW0", PinType.OUTPUT)])
    try:
        gen.create_internal_net(tt, "INT_X", [(dst, "TCLK", PinType.INPUT)])
    except ValueError:
        return
    raise AssertionError("a duplicate internal net was accepted")


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except AssertionError as exc:
                failures += 1
                print(f"FAIL {name}: {exc}")
            else:
                print(f"ok   {name}")
    sys.exit(1 if failures else 0)
