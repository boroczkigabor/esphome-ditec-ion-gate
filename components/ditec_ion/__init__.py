"""Ditec ION4B / ION6B gate (RCB100 BLE module) for ESPHome.

Platform-only component: use it as `cover: - platform: ditec_ion`. See cover.py.
"""

import esphome.codegen as cg

ditec_ion_ns = cg.esphome_ns.namespace("ditec_ion")
