"""Small shared helpers for the byte-oriented bus tabs (UART/I2C/SPI)."""


def bytes_from_hex(text: str) -> bytes:
    """Accepts hex with or without spaces ("48 65" or "4865"). Empty/
    whitespace-only input is a valid zero-length payload."""
    cleaned = text.strip().replace(" ", "")
    if not cleaned:
        return b""
    if len(cleaned) % 2 != 0:
        raise ValueError("odd number of hex digits")
    return bytes.fromhex(cleaned)


def bytes_to_display(data: bytes) -> str:
    if not data:
        return "(empty)"
    hex_str = " ".join(f"{b:02x}" for b in data)
    ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
    return f"{hex_str}   |{ascii_str}|"
