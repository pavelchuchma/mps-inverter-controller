# -*- coding: utf-8 -*-
"""
Query inverter serial number via HS/MS/MSX RS232 protocol: QID<CRC><CR>
Windows example using COM1.

Protocol notes:
- Serial settings: 2400 baud, 8 data bits, no parity, 1 stop bit (8N1).  :contentReference[oaicite:2]{index=2}
- Command: QID <CRC> <CR> and response: (XXXXXXXXXXXXXX <CRC> <CR>          :contentReference[oaicite:3]{index=3}
- CRC is CRC-16/XMODEM over the ASCII payload, then append 2 bytes CRC (hi, lo).
- Reserved CRC bytes (0x28 '(' , 0x0D CR, 0x0A LF) are incremented by 1.
"""

import serial
from typing import Tuple

RESERVED = {0x28, 0x0D, 0x0A}  # '(', CR, LF


def crc16_xmodem(data: bytes) -> int:
    """CRC-16/XMODEM (poly 0x1021, init 0x0000, no xorout)."""
    crc = 0x0000
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def adjust_crc_bytes(crc_hi: int, crc_lo: int) -> Tuple[int, int]:
    """Increment reserved CRC bytes by 1 (device expects adjusted bytes)."""
    if crc_hi in RESERVED:
        crc_hi = (crc_hi + 1) & 0xFF
    if crc_lo in RESERVED:
        crc_lo = (crc_lo + 1) & 0xFF
    return crc_hi, crc_lo


def build_frame(payload_ascii: str) -> bytes:
    """Build frame: PAYLOAD + CRC(hi,lo adjusted) + CR."""
    payload = payload_ascii.encode("ascii")
    crc = crc16_xmodem(payload)
    crc_hi = (crc >> 8) & 0xFF
    crc_lo = crc & 0xFF
    crc_hi, crc_lo = adjust_crc_bytes(crc_hi, crc_lo)
    return payload + bytes([crc_hi, crc_lo, 0x0D])  # CR


def read_until_cr(ser: serial.Serial, max_len: int = 256) -> bytes:
    """Read bytes until CR (0x0D) or until max_len."""
    buf = bytearray()
    while len(buf) < max_len:
        b = ser.read(1)
        if not b:
            break  # timeout
        buf += b
        if b == b"\r":
            break
    return bytes(buf)


def parse_and_verify_response(resp: bytes) -> Tuple[str, bool]:
    """
    Response format: b'(' + ASCII_DATA + CRC(2 bytes) + b'\\r'
    Returns (serial_string, crc_ok)
    """
    if not resp or resp[-1:] != b"\r":
        raise ValueError(f"Response not terminated by CR. Raw={resp!r}")

    body = resp[:-1]  # without CR
    if len(body) < 1 + 2:
        raise ValueError(f"Response too short. Raw={resp!r}")

    recv_crc_hi = body[-2]
    recv_crc_lo = body[-1]
    payload = body[:-2]  # includes leading '('

    # verify CRC
    calc = crc16_xmodem(payload)
    calc_hi = (calc >> 8) & 0xFF
    calc_lo = calc & 0xFF
    calc_hi, calc_lo = adjust_crc_bytes(calc_hi, calc_lo)

    crc_ok = (recv_crc_hi == calc_hi) and (recv_crc_lo == calc_lo)

    # extract serial number text after '('
    if payload[:1] != b"(":
        raise ValueError(f"Unexpected response start byte (expected '('). Raw={resp!r}")

    serial_txt = payload[1:].decode("ascii", errors="replace").strip()
    return serial_txt, crc_ok


def send_command_and_get_payload(ser: serial.Serial, cmd_ascii: str, max_len: int = 512) -> tuple:
    """Send a command (ASCII), read response, verify CRC and return (payload_text, crc_ok, raw_resp).

    payload_text: ASCII text inside the response payload (without leading '(')
    crc_ok: boolean whether CRC matched
    raw_resp: raw bytes read from serial
    """
    frame = build_frame(cmd_ascii)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    ser.write(frame)
    ser.flush()
    resp = read_until_cr(ser, max_len=max_len)
    payload_text, crc_ok = parse_and_verify_response(resp)
    return payload_text, crc_ok, resp


def send_and_print_command(ser: serial.Serial, cmd_ascii: str) -> tuple:
    """Send an arbitrary ASCII command to the inverter and print response details.

    Returns (payload_text, crc_ok, raw_resp).
    """
    print(f"\n--- Send command: {cmd_ascii} ---")
    # Show TX frame bytes for debugging
    tx_frame = build_frame(cmd_ascii)
    print("TX (hex):", tx_frame.hex(" "))

    # Use existing helper to send and receive
    payload_text, crc_ok, raw = send_command_and_get_payload(ser, cmd_ascii)

    print("RX (raw):", raw)
    try:
        print("RX (hex):", raw.hex(" "))
    except Exception:
        # raw may be empty or non-bytes
        pass
    print("Payload:", payload_text)
    print("CRC OK:", crc_ok)

    return payload_text, crc_ok, raw


def query_qpigs(ser: serial.Serial) -> dict:
    """Query QPIGS and print each reported value on its own line with units.

    Returns a dict mapping field name -> raw token (string)."""
    payload_text, crc_ok, raw = send_command_and_get_payload(ser, "QPIGS")
    print("\nQPIGS raw (hex):", raw.hex(" "))
    print("QPIGS payload:", payload_text)
    print("QPIGS CRC OK:", crc_ok)

    tokens = payload_text.split()

    # Field definitions in the order described by the protocol (name, unit)
    field_defs = [
        ("Grid voltage", "V"),            # BBB.B
        ("Grid frequency", "Hz"),         # CC.C
        ("AC output voltage", "V"),       # DDD.D
        ("AC output frequency", "Hz"),    # EE.E
        ("AC output apparent power", "VA"), # FFFF
        ("AC output active power", "W"),  # GGGG
        ("Output load percent", "%"),     # HHH
        ("BUS voltage", "V"),             # III
        ("Battery voltage", "V"),         # JJ.JJ
        ("Battery charging current", "A"),# KKK
        ("Battery capacity", "%"),        # OOO
        ("Inverter heat sink temp", "C"), # TTTT
        ("PV input current for battery", "A"), # EEEE
        ("PV input voltage", "V"),        # UUU.U
        ("Battery voltage from SCC", "V"),# WW.WW
        ("Battery discharge current", "A"), # PPPPP
        ("Device status bits (b7..b0)", ""), # b7..b0
        ("Battery voltage offset for fans on", "(10mV units)"), # QQ
        ("EEPROM version", ""),           # VV
        ("PV charging power", "W"),       # MMMMM
        ("Additional status bits (b10..b8)", ""), # b10..b8
    ]

    results = {}
    for idx, (name, unit) in enumerate(field_defs):
        token = tokens[idx] if idx < len(tokens) else ""
        formatted = token
        # Try to normalize numeric tokens for nicer output
        if token:
            try:
                if '.' in token:
                    # float-like
                    v = float(token)
                    # choose formatting precision based on decimals in token
                    dec = len(token.split('.')[-1])
                    formatted = f"{v:.{dec}f}"
                else:
                    # integer-like
                    iv = int(token)
                    formatted = str(iv)
            except ValueError:
                # leave as-is
                formatted = token

        if unit:
            print(f"{name}: {formatted} {unit}")
        else:
            print(f"{name}: {formatted}")
        results[name] = token

    return results


def query_qmod(ser: serial.Serial) -> tuple:
    """Query QMOD and print the returned mode code and human-readable name.

    Returns (code, name)
    """
    payload_text, crc_ok, raw = send_command_and_get_payload(ser, "QMOD")
    print("\nQMOD raw (hex):", raw.hex(" "))
    print("QMOD payload:", payload_text)
    print("QMOD CRC OK:", crc_ok)

    code = (payload_text.strip()[:1]) if payload_text else ""
    mode_names = {
        'P': 'Power On',
        'S': 'Standby',
        'L': 'Line (Grid) mode',
        'B': 'Battery mode',
        'F': 'Fault mode',
        'H': 'Power saving mode',
    }
    name = mode_names.get(code, 'Unknown')

    print(f"Mode code: {code}")
    print(f"Mode name: {name}")

    return code, name


def query_qpiri(ser: serial.Serial) -> dict:
    """Query QPIRI and print parsed device rating information (one value per line with units).

    Returns a dict mapping field name -> raw token.
    """
    payload_text, crc_ok, raw = send_command_and_get_payload(ser, "QPIRI")
    print("\nQPIRI raw (hex):", raw.hex(" "))
    print("QPIRI payload:", payload_text)
    print("QPIRI CRC OK:", crc_ok)

    tokens = payload_text.split()

    # Field definitions from protocol (4.6 QPIRI). Some fields are device/model dependent.
    field_defs = [
        ("Grid rating voltage", "V"),    # BBB.B
        ("Grid rating current", "A"),    # CC.C
        ("AC output rating voltage", "V"), # DDD.D
        ("AC output rating frequency", "Hz"), # EE.E
        ("AC output rating current", "A"), # FF.F
        ("AC output rating apparent power", "VA"), # HHHH
        ("AC output rating active power", "W"), # IIII
        ("Battery rating voltage", "V"), # JJ.J
        ("Battery re-charge voltage", "V"), # KK.K
        ("Battery under voltage", "V"), # JJ.J (reserved label overlap in spec)
        ("Battery bulk voltage", "V"), # KK.K
        ("Battery float voltage", "V"), # LL.L
        ("Battery type", ""), # O (0 AGM,1 Flooded,2 User)
        ("Max AC charging current", "A"), # PP
        ("Current max charging current", "A"), # QQ0 - model-dependent
        ("Input voltage range", ""), # O (0 Appliance,1 UPS)
        ("Output source priority", ""), # P (0 utility,1 solar,2 SBU)
        ("Charger source priority", ""), # Q
        ("Parallel max number", ""), # R
        ("Machine type", ""), # SS
        ("Topology", ""), # T
        ("Output mode", ""), # U
        ("Battery re-discharge voltage", "V"), # VV.V
        ("PV OK condition for parallel", ""), # W
        ("PV power balance", ""), # X
    ]

    results = {}
    for idx, (name, unit) in enumerate(field_defs):
        token = tokens[idx] if idx < len(tokens) else ""
        formatted = token
        if token:
            try:
                if '.' in token:
                    v = float(token)
                    dec = len(token.split('.')[-1])
                    formatted = f"{v:.{dec}f}"
                else:
                    iv = int(token)
                    formatted = str(iv)
            except ValueError:
                formatted = token

        if unit:
            print(f"{name}: {formatted} {unit}")
        else:
            print(f"{name}: {formatted}")
        results[name] = token

    return results


def main():
    port = "COM1"
    baud = 2400

    # Open serial once and reuse for multiple queries
    with serial.Serial(
            port=port,
            baudrate=baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=2.0,          # seconds
            write_timeout=2.0,
    ) as ser:
        # Example: send arbitrary command QSID
        # try:
        #     send_and_print_command(ser, "MUCHGC020")
        # except Exception as e:
        #     print("command failed:", e)

        # Query current device mode QMOD
        try:
            print("\n--- QMOD (device mode) ---")
            query_qmod(ser)
        except Exception as e:
            print("QMOD failed:", e)

        # Query device info (previous example used QPIRI)
        try:
            print("--- QPIRI (device rating info) ---")
            query_qpiri(ser)
        except Exception as e:
            print("QPIRI failed:", e)

        # Query general status parameters QPIGS
        try:
            print("\n--- QPIGS (general status parameters) ---")
            query_qpigs(ser)
        except Exception as e:
            print("QPIGS failed:", e)


if __name__ == "__main__":
    main()