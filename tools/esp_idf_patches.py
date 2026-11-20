"""Generate local backports for the pinned ESP-IDF without modifying its install."""


def replace_exact(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError("ESP-IDF changed; review the I2C backports before building")
    return text.replace(old, new)


def write_changed(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def generate(root, framework):
    # Espressif 2ab0921: classic ESP32 FIFO reads require the DPORT workaround.
    header = (framework / "components/hal/esp32/include/hal/i2c_ll.h").read_text()
    header = replace_exact(header,
        "ptr[i] = HAL_FORCE_READ_U32_REG_FIELD(hw->fifo_data, data);",
        "ptr[i] = DPORT_REG_READ((uint32_t)&hw->fifo_data);")
    header = replace_exact(header, '#include "soc/dport_reg.h"',
        '#include "soc/dport_reg.h"\n#include "soc/dport_access.h"')
    overlay = root / "include/aircue_sdk_patch"
    write_changed(overlay / "hal/i2c_ll.h", header)

    return overlay
