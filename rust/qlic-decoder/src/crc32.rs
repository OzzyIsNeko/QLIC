/// Computes the IEEE CRC-32 used by QLIC containers.
pub fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = u32::MAX;
    for &byte in bytes {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                0xedb8_8320 ^ (crc >> 1)
            } else {
                crc >> 1
            };
        }
    }
    crc ^ u32::MAX
}

#[cfg(test)]
mod tests {
    use super::crc32;

    #[test]
    fn matches_standard_check_value() {
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
    }
}
