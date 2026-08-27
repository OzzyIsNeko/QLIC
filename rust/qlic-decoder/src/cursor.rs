use crate::{Error, Result};

#[derive(Clone, Copy, Debug)]
pub(crate) struct Cursor<'a> {
    bytes: &'a [u8],
    position: usize,
}

impl<'a> Cursor<'a> {
    pub(crate) const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, position: 0 }
    }

    pub(crate) const fn position(&self) -> usize {
        self.position
    }

    pub(crate) const fn remaining(&self) -> usize {
        self.bytes.len() - self.position
    }

    pub(crate) fn take(&mut self, length: usize) -> Result<&'a [u8]> {
        let end = self
            .position
            .checked_add(length)
            .ok_or(Error::ArithmeticOverflow("cursor position"))?;
        let value = self.bytes.get(self.position..end).ok_or(Error::Truncated {
            offset: self.position,
            needed: length,
            remaining: self.remaining(),
        })?;
        self.position = end;
        Ok(value)
    }

    pub(crate) fn read_u8(&mut self) -> Result<u8> {
        self.take(1)?
            .first()
            .copied()
            .ok_or(Error::ArithmeticOverflow("one-byte cursor read"))
    }

    fn read_array<const LENGTH: usize>(&mut self) -> Result<[u8; LENGTH]> {
        let source = self.take(LENGTH)?;
        let mut output = [0_u8; LENGTH];
        for (target, &byte) in output.iter_mut().zip(source) {
            *target = byte;
        }
        Ok(output)
    }

    pub(crate) fn read_u32_le(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.read_array()?))
    }

    pub(crate) fn read_u16_le(&mut self) -> Result<u16> {
        Ok(u16::from_le_bytes(self.read_array()?))
    }

    pub(crate) fn read_u64_le(&mut self) -> Result<u64> {
        Ok(u64::from_le_bytes(self.read_array()?))
    }
}

#[cfg(test)]
mod tests {
    use super::Cursor;
    use crate::Error;

    #[test]
    fn reads_little_endian_values_and_tracks_bounds() {
        let mut cursor = Cursor::new(&[7, 1, 2, 3, 4, 5]);
        assert_eq!(cursor.read_u8().unwrap(), 7);
        assert_eq!(cursor.read_u32_le().unwrap(), 0x0403_0201);
        assert_eq!(cursor.position(), 5);
        assert_eq!(cursor.remaining(), 1);
        assert!(matches!(
            cursor.read_u32_le(),
            Err(Error::Truncated {
                offset: 5,
                needed: 4,
                remaining: 1
            })
        ));
    }
}
