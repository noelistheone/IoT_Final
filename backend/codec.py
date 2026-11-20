"""Decode the firmware's explicitly serialized, little-endian journal records."""
import struct
import zlib

LEVELS = ("UNASSESSED", "CLEAR", "WATCH", "HIGH")
TRENDS = ("UNKNOWN", "RISING", "STABLE", "FALLING")
EVENTS = ("NONE", "BOOT", "EPISODE_START", "FALLING", "RECOVERED", "CONFIG_CHANGED",
          "SENSOR_FAULT", "SENSOR_RESTORED", "TIME_ANCHOR", "GAP", "MAINTENANCE", "CONFIG")
CLIMATE_FIELDS = ("co2_raw_ppm", "temperature_c", "humidity_rh", "pressure_hpa", "barometer_temperature_c")
CLIMATE_SCALES = (1, 100, 100, 10, 100)

def decode(wire):
    try:
        raw = bytes.fromhex(wire)
    except (ValueError, TypeError) as exc:
        raise ValueError("Invalid record hex") from exc
    if not ((len(raw) == 96 and raw[0] == 1) or (len(raw) == 112 and raw[0] == 2 and raw[1] == 1)) or raw[1] not in (1, 2):
        raise ValueError("Unsupported record schema or size")
    if zlib.crc32(raw[:-4]) != struct.unpack_from("<I", raw, len(raw)-4)[0]:
        raise ValueError("Record CRC mismatch")
    flags, = struct.unpack_from("<H", raw, 2)
    seq, mono, utc = struct.unpack_from("<IQQ", raw, 20)
    if not seq or flags & ~63 or (bool(flags & 1) != (utc == 0)):
        raise ValueError("Invalid identity or time flags")
    boot = raw[4:20].hex()
    record = dict(record_id=f"{boot}:{seq}", boot_id=boot, sequence=seq, monotonic_ms=mono,
                  measured_at=utc or None, time_quality="UTC_UNKNOWN" if flags & 1 else "SYNCED",
                  demo=bool(flags & 2), partial=bool(flags & 4), interrupted=bool(flags & 8),
                  range_warn=bool(flags & 16), bypass=bool(flags & 32), flags=flags,
                  record_type="MINUTE" if raw[1] == 1 else "EVENT")
    p = raw[40:92]
    if raw[1] == 1:
        mean, lo, hi, valid, expected, rev, valid_ms, high_ms, red_ms, window_ms, level, trend, warm, error, missed, index = struct.unpack_from("<iiiHHIIIIIBBHHHI", p)
        if level > 3 or trend > 3 or not 1 <= expected <= 60 or valid + warm + error + missed != expected:
            raise ValueError("Invalid sample accounting")
        if not 1 <= window_ms <= 60000 or max(valid_ms, high_ms, red_ms) > window_ms or max(high_ms, red_ms) > valid_ms:
            raise ValueError("Invalid coverage durations")
        if valid and not 380 <= lo <= mean <= hi <= 32000:
            raise ValueError("Invalid concentration statistics")
        if not valid and (mean or lo or hi or valid_ms or high_ms or red_ms):
            raise ValueError("Empty windows must not contain measurements")
        if not rev:
            raise ValueError("Missing configuration revision")
        record.update(mean=mean if valid else None, minimum=lo if valid else None, maximum=hi if valid else None,
                      valid_count=valid, expected_count=expected, config_revision=rev, valid_ms=valid_ms,
                      observed_high_ms=high_ms, red_valid_ms=red_ms, window_ms=window_ms, level=LEVELS[level],
                      trend=TRENDS[trend], warmup_count=warm, error_count=error, missed_count=missed,
                      window_index=None if index == 0xFFFFFFFF else index)
        if raw[0] == 2:
            values = struct.unpack_from("<5h", raw, 88)
            mask, co2_count, barometer_count = struct.unpack_from("<3H", raw, 98)
            if mask & ~31 or co2_count > 120 or barometer_count > 120:
                raise ValueError("Invalid climate sample accounting")
            if bool(mask & 7) != bool(co2_count) or bool(mask & 24) != bool(barometer_count):
                raise ValueError("Climate values require source samples")
            record.update({field: values[i] / CLIMATE_SCALES[i] if mask & (1 << i) else None for i, field in enumerate(CLIMATE_FIELDS)})
            record.update(co2_raw_count=co2_count, barometer_count=barometer_count)
    else:
        if p[0] >= len(EVENTS) or p[1] > 3:
            raise ValueError("Invalid event enum")
        sequence, peak, high, red, valid, rev, extra = struct.unpack_from("<IiIIIIQ", p, 20)
        episode_id = f"{p[4:20].hex()}:{sequence}" if sequence else None
        if p[0] == 2:
            episode_id = record["record_id"]
        record.update(event=EVENTS[p[0]], level=LEVELS[p[1]], episode_id=episode_id, peak=peak,
                      high_ms=high, red_ms=red, valid_ms=valid, config_revision=rev, extra=extra)
        if p[0] in (4, 5):
            record["known_missing_ms"] = extra
    return record
