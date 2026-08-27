use std::mem::size_of;
use std::ops::Range;

use crate::cursor::Cursor;
use crate::qst1::decode_qst1_bytes;
use crate::{Container, Error, LimitKind, Limits, Mode, Result};

const QSW1_HEADER_SIZE: usize = 16;
const QSW1_METHOD_BYTE_SLICES: u8 = 0;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qsw1Slice {
    /// Byte range within the parsed QSW1 payload occupied by this QST1 stream.
    ///
    /// Mode 19 uses `Container::payload`; mode 20 uses
    /// `Qsw2Descriptor::pixel_payload`.
    pub range: Range<usize>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qsw1Descriptor {
    pub bits_per_sample: u8,
    pub channels: u8,
    pub sample_crc32: u32,
    pub sample_count: usize,
    pub decoded_bytes: usize,
    pub slices: Vec<Qsw1Slice>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum WideSamples {
    U16(Vec<u16>),
    U32(Vec<u32>),
}

impl WideSamples {
    pub fn len(&self) -> usize {
        match self {
            Self::U16(samples) => samples.len(),
            Self::U32(samples) => samples.len(),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn as_u16(&self) -> Option<&[u16]> {
        match self {
            Self::U16(samples) => Some(samples),
            Self::U32(_) => None,
        }
    }

    pub fn as_u32(&self) -> Option<&[u32]> {
        match self {
            Self::U16(_) => None,
            Self::U32(samples) => Some(samples),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WideImage {
    pub width: u32,
    pub height: u32,
    pub channels: u8,
    pub bits_per_sample: u8,
    /// Number of bytes between adjacent rows in native `u16`/`u32` storage.
    pub stride: usize,
    /// Interleaved samples in row-major channel order.
    pub samples: WideSamples,
}

pub fn parse_qsw1(container: &Container<'_>) -> Result<Qsw1Descriptor> {
    let header = &container.header;
    if header.mode != Mode::NativeWide {
        return Err(Error::NotWideImage);
    }
    if header.payload_size
        != u64::try_from(container.payload.len())
            .map_err(|_| Error::ArithmeticOverflow("QSW1 payload length"))?
    {
        return Err(Error::InvalidQsw1(
            "stored payload size does not match outer header",
        ));
    }

    let channels = u8::try_from(header.palette_count)
        .map_err(|_| Error::InvalidQsw1("channel count does not fit in QSW1"))?;
    parse_qsw1_shape(
        container.payload,
        header.width,
        header.height,
        header.index_bits,
        channels,
    )
}

pub(crate) fn parse_qsw1_shape(
    payload: &[u8],
    width: u32,
    height: u32,
    bits: u8,
    channels: u8,
) -> Result<Qsw1Descriptor> {
    let slice_count = bits.div_ceil(8);
    let table_size = usize::from(slice_count)
        .checked_mul(8)
        .ok_or(Error::ArithmeticOverflow("QSW1 slice table"))?;
    let data_offset = QSW1_HEADER_SIZE
        .checked_add(table_size)
        .ok_or(Error::ArithmeticOverflow("QSW1 data offset"))?;
    if payload.len() < data_offset {
        return Err(Error::InvalidQsw1("truncated header or slice table"));
    }

    let mut cursor = Cursor::new(payload);
    if cursor.take(4)? != b"QSW1" {
        return Err(Error::InvalidQsw1("invalid magic"));
    }
    if cursor.read_u8()? != QSW1_METHOD_BYTE_SLICES {
        return Err(Error::InvalidQsw1("unsupported byte-slice method"));
    }
    if cursor.read_u8()? != bits {
        return Err(Error::InvalidQsw1("precision does not match outer header"));
    }
    if cursor.read_u8()? != channels {
        return Err(Error::InvalidQsw1(
            "channel count does not match outer header",
        ));
    }
    if cursor.read_u8()? != slice_count {
        return Err(Error::InvalidQsw1("slice count does not match precision"));
    }
    let sample_crc32 = cursor.read_u32_le()?;
    if cursor.read_u32_le()? != 0 {
        return Err(Error::InvalidQsw1("reserved field is not zero"));
    }

    let slice_capacity = usize::from(slice_count);
    let mut lengths = Vec::new();
    lengths
        .try_reserve_exact(slice_capacity)
        .map_err(|_| Error::AllocationFailed("QSW1 slice lengths"))?;
    for _ in 0..slice_count {
        let length = cursor.read_u64_le()?;
        if length == 0 {
            return Err(Error::InvalidQsw1("zero-length byte slice"));
        }
        lengths.push(
            usize::try_from(length).map_err(|_| Error::ArithmeticOverflow("QSW1 slice length"))?,
        );
    }
    if cursor.position() != data_offset {
        return Err(Error::InvalidQsw1("slice table size mismatch"));
    }

    let mut stream_end = data_offset;
    let mut slices = Vec::new();
    slices
        .try_reserve_exact(lengths.len())
        .map_err(|_| Error::AllocationFailed("QSW1 slice descriptors"))?;
    for length in lengths {
        let start = stream_end;
        stream_end = start
            .checked_add(length)
            .ok_or(Error::ArithmeticOverflow("QSW1 stream end"))?;
        if stream_end > payload.len() {
            return Err(Error::InvalidQsw1("byte slice exceeds payload"));
        }
        slices.push(Qsw1Slice {
            range: start..stream_end,
        });
    }
    if stream_end != payload.len() {
        return Err(Error::InvalidQsw1("trailing data after byte slices"));
    }

    let sample_count_u64 = u64::from(width)
        .checked_mul(u64::from(height))
        .and_then(|value| value.checked_mul(u64::from(channels)))
        .ok_or(Error::ArithmeticOverflow("wide sample count"))?;
    let storage = if bits <= 16 { 2_u64 } else { 4_u64 };
    let decoded_bytes_u64 = sample_count_u64
        .checked_mul(storage)
        .ok_or(Error::ArithmeticOverflow("wide decoded bytes"))?;
    let sample_count = usize::try_from(sample_count_u64)
        .map_err(|_| Error::ArithmeticOverflow("wide sample count"))?;
    let decoded_bytes = usize::try_from(decoded_bytes_u64)
        .map_err(|_| Error::ArithmeticOverflow("wide decoded bytes"))?;

    Ok(Qsw1Descriptor {
        bits_per_sample: bits,
        channels,
        sample_crc32,
        sample_count,
        decoded_bytes,
        slices,
    })
}

fn fallible_zeroed<T: Clone>(length: usize, zero: T, context: &'static str) -> Result<Vec<T>> {
    let mut output = Vec::new();
    output
        .try_reserve_exact(length)
        .map_err(|_| Error::AllocationFailed(context))?;
    output.resize(length, zero);
    Ok(output)
}

fn crc_byte(mut crc: u32, byte: u8) -> u32 {
    crc ^= u32::from(byte);
    for _ in 0..8 {
        crc = if crc & 1 != 0 {
            0xedb8_8320 ^ (crc >> 1)
        } else {
            crc >> 1
        };
    }
    crc
}

fn canonical_crc_u16(samples: &[u16], bytes_per_sample: usize) -> u32 {
    let mut crc = u32::MAX;
    for &sample in samples {
        let bytes = sample.to_le_bytes();
        for &byte in &bytes[..bytes_per_sample] {
            crc = crc_byte(crc, byte);
        }
    }
    crc ^ u32::MAX
}

fn canonical_crc_u32(samples: &[u32], bytes_per_sample: usize) -> u32 {
    let mut crc = u32::MAX;
    for &sample in samples {
        let bytes = sample.to_le_bytes();
        for &byte in &bytes[..bytes_per_sample] {
            crc = crc_byte(crc, byte);
        }
    }
    crc ^ u32::MAX
}

fn checked_stride(width: u32, channels: u8, storage: usize) -> Result<usize> {
    usize::try_from(width)
        .map_err(|_| Error::ArithmeticOverflow("wide row width"))?
        .checked_mul(usize::from(channels))
        .and_then(|samples| samples.checked_mul(storage))
        .ok_or(Error::ArithmeticOverflow("wide row stride"))
}

fn merge_u16_slice(
    samples: &mut [u16],
    decoded: &[u8],
    shift: u32,
    top_maximum: Option<u16>,
) -> Result<()> {
    if samples.len() != decoded.len() {
        return Err(Error::InvalidQsw1("decoded byte-slice length mismatch"));
    }
    for (sample, &part) in samples.iter_mut().zip(decoded) {
        if top_maximum.is_some_and(|maximum| u16::from(part) > maximum) {
            return Err(Error::InvalidQsw1("nonzero unused high sample bits"));
        }
        *sample |= u16::from(part) << shift;
    }
    Ok(())
}

fn merge_u32_slice(
    samples: &mut [u32],
    decoded: &[u8],
    shift: u32,
    top_maximum: Option<u16>,
) -> Result<()> {
    if samples.len() != decoded.len() {
        return Err(Error::InvalidQsw1("decoded byte-slice length mismatch"));
    }
    for (sample, &part) in samples.iter_mut().zip(decoded) {
        if top_maximum.is_some_and(|maximum| u16::from(part) > maximum) {
            return Err(Error::InvalidQsw1("nonzero unused high sample bits"));
        }
        *sample |= u32::from(part) << shift;
    }
    Ok(())
}

pub(crate) fn decode_qsw1_pixels(
    payload: &[u8],
    descriptor: &Qsw1Descriptor,
    width: u32,
    height: u32,
    limits: &Limits,
) -> Result<WideImage> {
    limits.validate()?;
    let decoded_bytes = u64::try_from(descriptor.decoded_bytes)
        .map_err(|_| Error::ArithmeticOverflow("wide decoded bytes"))?;
    if decoded_bytes > limits.max_decoded_bytes {
        return Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            limit: limits.max_decoded_bytes,
            actual: decoded_bytes,
        });
    }

    let bytes_per_sample = usize::from(descriptor.bits_per_sample.div_ceil(8));
    let top_bits = descriptor.bits_per_sample - ((descriptor.slices.len() - 1) as u8 * 8);
    let top_maximum = (1_u16 << top_bits) - 1;
    let samples = if descriptor.bits_per_sample <= 16 {
        let mut samples =
            fallible_zeroed(descriptor.sample_count, 0_u16, "wide u16 sample buffer")?;
        for (slice_index, slice) in descriptor.slices.iter().enumerate() {
            let stream = payload
                .get(slice.range.clone())
                .ok_or(Error::InvalidQsw1("byte slice is outside payload"))?;
            let decoded = decode_qst1_bytes(stream, width, height, descriptor.channels, limits)?;
            let shift = u32::try_from(slice_index)
                .map_err(|_| Error::ArithmeticOverflow("wide byte-slice shift"))?
                * 8;
            let top = (slice_index + 1 == descriptor.slices.len()).then_some(top_maximum);
            merge_u16_slice(&mut samples, &decoded, shift, top)?;
        }
        if canonical_crc_u16(&samples, bytes_per_sample) != descriptor.sample_crc32 {
            return Err(Error::InvalidQsw1("decoded sample checksum mismatch"));
        }
        WideSamples::U16(samples)
    } else {
        let mut samples =
            fallible_zeroed(descriptor.sample_count, 0_u32, "wide u32 sample buffer")?;
        for (slice_index, slice) in descriptor.slices.iter().enumerate() {
            let stream = payload
                .get(slice.range.clone())
                .ok_or(Error::InvalidQsw1("byte slice is outside payload"))?;
            let decoded = decode_qst1_bytes(stream, width, height, descriptor.channels, limits)?;
            let shift = u32::try_from(slice_index)
                .map_err(|_| Error::ArithmeticOverflow("wide byte-slice shift"))?
                * 8;
            let top = (slice_index + 1 == descriptor.slices.len()).then_some(top_maximum);
            merge_u32_slice(&mut samples, &decoded, shift, top)?;
        }
        if canonical_crc_u32(&samples, bytes_per_sample) != descriptor.sample_crc32 {
            return Err(Error::InvalidQsw1("decoded sample checksum mismatch"));
        }
        WideSamples::U32(samples)
    };

    let storage = if descriptor.bits_per_sample <= 16 {
        size_of::<u16>()
    } else {
        size_of::<u32>()
    };
    Ok(WideImage {
        width,
        height,
        channels: descriptor.channels,
        bits_per_sample: descriptor.bits_per_sample,
        stride: checked_stride(width, descriptor.channels, storage)?,
        samples,
    })
}

pub fn decode_wide(bytes: &[u8], limits: &Limits) -> Result<WideImage> {
    let container = Container::parse(bytes, limits)?;
    let descriptor = parse_qsw1(&container)?;
    decode_qsw1_pixels(
        container.payload,
        &descriptor,
        container.header.width,
        container.header.height,
        limits,
    )
}

#[cfg(test)]
mod tests {
    use super::{canonical_crc_u16, canonical_crc_u32, merge_u16_slice, merge_u32_slice};
    use crate::Error;

    #[test]
    fn canonical_sample_crc_uses_only_declared_little_endian_bytes() {
        assert_eq!(
            canonical_crc_u16(&[0, 1, 511, 512, 1022, 1023], 2),
            0x0039_8066
        );
        assert_eq!(
            canonical_crc_u32(&[0, 1, 65_535, 65_536, 131_070, 131_071], 3),
            0x534e_dc57
        );
    }

    #[test]
    fn byte_slice_merge_rejects_unused_high_bits() {
        let mut u16_samples = [0_u16; 2];
        assert!(matches!(
            merge_u16_slice(&mut u16_samples, &[3, 4], 8, Some(3)),
            Err(Error::InvalidQsw1("nonzero unused high sample bits"))
        ));
        let mut u32_samples = [0_u32; 2];
        assert!(matches!(
            merge_u32_slice(&mut u32_samples, &[1, 2], 16, Some(1)),
            Err(Error::InvalidQsw1("nonzero unused high sample bits"))
        ));
    }
}
