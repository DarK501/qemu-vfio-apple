#!/usr/bin/env python3
"""Check hook install/removal ordering and callback guarding in apple_dma."""

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[3]
SRC = (ROOT / "contrib/apple-dma/linux/apple_dma_main.c").read_text()


def body(name):
    start = SRC.index(f"static void {name}(") if f"static void {name}(" in SRC \
        else SRC.index(f"static int {name}(")
    return SRC[start:SRC.index("\n}\n", start)]


def order(text, *needles):
    positions = [text.index(n) for n in needles]
    assert positions == sorted(positions), (needles, positions)


probe = body("apple_dma_probe")
order(probe, "apple_dma_bind_managed_pci(ad)", "list_add_tail_rcu",
      "ad->ready = true", "apple_dma_install_hook(ad)")

bind = body("apple_dma_bind_managed_pci")
assert "set_dma_ops" not in bind

remove = body("apple_dma_remove_instance")
order(remove, "apple_dma_restore_hook(ad)", "synchronize_rcu",
      "ad->ready = false", "list_del_rcu", "synchronize_srcu")
assert remove.count("synchronize_rcu") == 2
assert remove.rindex("synchronize_rcu") < remove.index("synchronize_srcu")

remove_cb = body("apple_dma_remove")
order(remove_cb, "apple_dma_remove_instance(ad)",
      "apple_dma_release_resources")
assert "ad->ready = false" not in remove_cb

ops = SRC[SRC.index("static const struct dma_map_ops apple_dma_ops"):]
ops = ops[:ops.index("};")]
for fn in re.findall(r"=\s*(apple_dma_\w+),", ops):
    assert fn.startswith("apple_dma_g_"), f"unguarded callback {fn}"

assert "list_for_each_entry(ad, &apple_dma_instances" not in SRC
print("ok")
