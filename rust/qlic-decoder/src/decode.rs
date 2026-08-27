use crate::qst1::{Qst1Stream, decode_qst1_rgba, decode_qst1_rgba_into, parse_qst1};
use crate::{
    Codec, Container, Error, Header, LimitKind, Limits, Mode, Result, Transform, decompress_lzms,
};

/// One decoded 8-bit image in tightly packed, row-major RGBA order.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RgbaImage {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

impl RgbaImage {
    /// Number of bytes between adjacent rows.
    pub fn stride(&self) -> usize {
        // decode_rgba validates allocation. Saturation covers manually
        // constructed values on narrower targets.
        (self.width as usize).saturating_mul(4)
    }
}

fn invalid(message: &'static str) -> Error {
    Error::InvalidPixelData(message)
}

fn checked_usize(value: u64, context: &'static str) -> Result<usize> {
    usize::try_from(value).map_err(|_| Error::ArithmeticOverflow(context))
}

fn checked_pixels(header: &Header) -> Result<usize> {
    let pixels = u64::from(header.width)
        .checked_mul(u64::from(header.height))
        .ok_or(Error::ArithmeticOverflow("pixel decode count"))?;
    checked_usize(pixels, "pixel decode count")
}

fn checked_rgba_len(header: &Header, limits: &Limits) -> Result<usize> {
    let bytes = u64::from(header.width)
        .checked_mul(u64::from(header.height))
        .and_then(|value| value.checked_mul(4))
        .ok_or(Error::ArithmeticOverflow("decoded RGBA byte count"))?;
    if bytes > limits.max_decoded_bytes {
        return Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            limit: limits.max_decoded_bytes,
            actual: bytes,
        });
    }
    checked_usize(bytes, "decoded RGBA byte count")
}

fn filled_vec(length: usize, context: &'static str) -> Result<Vec<u8>> {
    let mut output = Vec::new();
    output
        .try_reserve_exact(length)
        .map_err(|_| Error::AllocationFailed(context))?;
    output.resize(length, 0);
    Ok(output)
}

fn copied_vec(source: &[u8], context: &'static str) -> Result<Vec<u8>> {
    let mut output = Vec::new();
    output
        .try_reserve_exact(source.len())
        .map_err(|_| Error::AllocationFailed(context))?;
    output.extend_from_slice(source);
    Ok(output)
}

fn mode_bpp(mode: Mode) -> Option<usize> {
    match mode {
        Mode::Gray => Some(1),
        Mode::GrayAlpha => Some(2),
        Mode::Rgb => Some(3),
        Mode::Rgba => Some(4),
        _ => None,
    }
}

fn row_pack(width: u32, bits: u8) -> Result<usize> {
    let bit_count = u64::from(width)
        .checked_mul(u64::from(bits))
        .ok_or(Error::ArithmeticOverflow("palette row bit count"))?;
    checked_usize(
        bit_count
            .checked_add(7)
            .ok_or(Error::ArithmeticOverflow("palette row rounding"))?
            / 8,
        "palette row byte count",
    )
}

fn sample_size(header: &Header) -> Result<usize> {
    if header.mode == Mode::Palette {
        return row_pack(header.width, header.index_bits)?
            .checked_mul(header.height as usize)
            .ok_or(Error::ArithmeticOverflow("packed palette sample bytes"));
    }
    checked_pixels(header)?
        .checked_mul(mode_bpp(header.mode).ok_or_else(|| invalid("invalid sample mode"))?)
        .ok_or(Error::ArithmeticOverflow("sample byte count"))
}

fn supported_transform(header: &Header) -> bool {
    match header.mode {
        Mode::Gray | Mode::GrayAlpha => matches!(
            header.transform,
            Transform::Identity | Transform::IdentityRaw | Transform::IdentityRle
        ),
        Mode::Rgb | Mode::Rgba => matches!(
            header.transform,
            Transform::Identity
                | Transform::GreenDelta
                | Transform::IdentityRaw
                | Transform::GreenDeltaRaw
                | Transform::IdentityRle
                | Transform::GreenDeltaRle
                | Transform::RedDelta
                | Transform::BlueDelta
                | Transform::BlueDeltaPlanarMed
        ),
        Mode::Palette => matches!(
            header.transform,
            Transform::Identity
                | Transform::IdentityRaw
                | Transform::IdentityRle
                | Transform::IndexRle
        ),
        Mode::Separable => matches!(
            header.transform,
            Transform::Identity | Transform::SeparableDelta
        ),
        _ => false,
    }
}

fn fixed_payload_size(header: &Header) -> Result<Option<usize>> {
    if header.mode == Mode::Separable {
        let bpp = match header.index_bits {
            1..=4 => usize::from(header.index_bits),
            _ => return Err(invalid("invalid separable base mode")),
        };
        let row_bytes = (header.width as usize)
            .checked_mul(bpp)
            .ok_or(Error::ArithmeticOverflow("separable first row size"))?;
        return Ok(Some(
            (header.height as usize)
                .checked_sub(1)
                .and_then(|rows| rows.checked_mul(bpp))
                .and_then(|columns| row_bytes.checked_add(columns))
                .ok_or(Error::ArithmeticOverflow("separable table size"))?,
        ));
    }
    let exact = match header.transform {
        Transform::IdentityRaw | Transform::GreenDeltaRaw | Transform::BlueDeltaPlanarMed => {
            Some(sample_size(header)?)
        }
        Transform::Identity
        | Transform::GreenDelta
        | Transform::RedDelta
        | Transform::BlueDelta => {
            let row_bytes = if header.mode == Mode::Palette {
                row_pack(header.width, header.index_bits)?
            } else {
                (header.width as usize)
                    .checked_mul(
                        mode_bpp(header.mode)
                            .ok_or_else(|| invalid("invalid row-filter sample mode"))?,
                    )
                    .ok_or(Error::ArithmeticOverflow("filtered row byte count"))?
            };
            Some(
                row_bytes
                    .checked_add(1)
                    .and_then(|row| row.checked_mul(header.height as usize))
                    .ok_or(Error::ArithmeticOverflow("filtered payload byte count"))?,
            )
        }
        Transform::SeparableDelta => None,
        Transform::IdentityRle | Transform::GreenDeltaRle | Transform::IndexRle => None,
        _ => None,
    };
    Ok(exact)
}

pub(crate) fn decode_payload<'a>(
    container: &Container<'a>,
    limits: &Limits,
) -> Result<Option<Vec<u8>>> {
    let expected = checked_usize(container.header.payload_size, "payload byte count")?;
    match container.header.codec {
        Codec::Store => {
            if container.payload.len() != expected {
                return Err(invalid("stored payload size does not match header"));
            }
            Ok(None)
        }
        Codec::Lzms => Ok(Some(decompress_lzms(container.payload, expected, limits)?)),
    }
}

fn predict(filter: u8, left: u8, up: u8, upper_left: u8) -> Result<u8> {
    Ok(match filter {
        0 => 0,
        1 => left,
        2 => up,
        3 => ((u16::from(left) + u16::from(up)) >> 1) as u8,
        4 => {
            let left = i32::from(left);
            let up = i32::from(up);
            let upper_left = i32::from(upper_left);
            let estimate = left + up - upper_left;
            let left_distance = (estimate - left).abs();
            let up_distance = (estimate - up).abs();
            let corner_distance = (estimate - upper_left).abs();
            if left_distance <= up_distance && left_distance <= corner_distance {
                left as u8
            } else if up_distance <= corner_distance {
                up as u8
            } else {
                upper_left as u8
            }
        }
        5 => (i32::from(left) + i32::from(up) - i32::from(upper_left)).clamp(0, 255) as u8,
        _ => return Err(invalid("invalid row filter")),
    })
}

fn unfilter_rows(payload: &[u8], row_bytes: usize, height: u32, bpp: usize) -> Result<Vec<u8>> {
    let rows = height as usize;
    let encoded_row = row_bytes
        .checked_add(1)
        .ok_or(Error::ArithmeticOverflow("filtered row size"))?;
    let expected = encoded_row
        .checked_mul(rows)
        .ok_or(Error::ArithmeticOverflow("filtered payload size"))?;
    if payload.len() != expected {
        return Err(invalid("unexpected filtered payload size"));
    }
    let sample_len = row_bytes
        .checked_mul(rows)
        .ok_or(Error::ArithmeticOverflow("unfiltered sample size"))?;
    let mut samples = filled_vec(sample_len, "unfiltered samples")?;
    let mut input = 0;
    for y in 0..rows {
        let filter = payload[input];
        input += 1;
        let row_start = y * row_bytes;
        for x in 0..row_bytes {
            let position = row_start + x;
            let left = if x >= bpp { samples[position - bpp] } else { 0 };
            let up = if y != 0 {
                samples[position - row_bytes]
            } else {
                0
            };
            let upper_left = if y != 0 && x >= bpp {
                samples[position - row_bytes - bpp]
            } else {
                0
            };
            samples[position] = payload[input].wrapping_add(predict(filter, left, up, upper_left)?);
            input += 1;
        }
    }
    Ok(samples)
}

fn read_varint(data: &[u8], position: &mut usize) -> Result<usize> {
    let mut value = 0_usize;
    let mut shift = 0_u32;
    while *position < data.len() {
        let byte = data[*position];
        *position += 1;
        let chunk = usize::from(byte & 0x7f);
        if shift >= usize::BITS || (chunk != 0 && chunk > (usize::MAX >> shift)) {
            return Err(invalid("invalid run-length varint"));
        }
        value |= chunk << shift;
        if byte & 0x80 == 0 {
            return Ok(value);
        }
        shift = shift
            .checked_add(7)
            .ok_or(Error::ArithmeticOverflow("run-length varint shift"))?;
    }
    Err(invalid("truncated run-length varint"))
}

fn decode_rle(data: &[u8], expected: usize) -> Result<Vec<u8>> {
    let mut output = filled_vec(expected, "RLE samples")?;
    let mut input = 0;
    let mut written = 0_usize;
    while input < data.len() {
        let run_minus_one = read_varint(data, &mut input)?;
        let value = *data
            .get(input)
            .ok_or_else(|| invalid("truncated run value"))?;
        input += 1;
        let run = run_minus_one
            .checked_add(1)
            .ok_or_else(|| invalid("run length overflow"))?;
        let end = written
            .checked_add(run)
            .filter(|&end| end <= expected)
            .ok_or_else(|| invalid("run exceeds expected sample size"))?;
        output[written..end].fill(value);
        written = end;
    }
    if written != expected {
        return Err(invalid("run payload does not fill expected sample size"));
    }
    Ok(output)
}

fn unpack_index(row: &[u8], x: u32, bits: u8) -> Result<u32> {
    if !(1..=16).contains(&bits) {
        return Err(invalid("invalid palette index bit width"));
    }
    if bits == 8 {
        return row
            .get(x as usize)
            .copied()
            .map(u32::from)
            .ok_or_else(|| invalid("packed palette row is truncated"));
    }
    if bits == 16 {
        let offset = (x as usize)
            .checked_mul(2)
            .ok_or(Error::ArithmeticOverflow("16-bit palette index offset"))?;
        let end = offset
            .checked_add(2)
            .ok_or(Error::ArithmeticOverflow("16-bit palette index end"))?;
        let bytes = row
            .get(offset..end)
            .ok_or_else(|| invalid("packed palette row is truncated"))?;
        return Ok(u32::from(u16::from_le_bytes([bytes[0], bytes[1]])));
    }
    let bit = (x as usize)
        .checked_mul(usize::from(bits))
        .ok_or(Error::ArithmeticOverflow("packed palette bit offset"))?;
    let byte = bit >> 3;
    let shift = bit & 7;
    let needed = (shift + usize::from(bits) + 7) >> 3;
    let mut value = 0_u32;
    for offset in 0..needed {
        let source = byte
            .checked_add(offset)
            .and_then(|position| row.get(position))
            .ok_or_else(|| invalid("packed palette row is truncated"))?;
        value |= u32::from(*source) << (offset * 8);
    }
    Ok((value >> shift) & ((1_u32 << bits) - 1))
}

fn decode_samples(
    samples: &[u8],
    header: &Header,
    palette: &[u8],
    limits: &Limits,
) -> Result<RgbaImage> {
    let rgba_len = checked_rgba_len(header, limits)?;
    let mut rgba = filled_vec(rgba_len, "RGBA pixels")?;
    let pixels = checked_pixels(header)?;

    if header.mode == Mode::Palette {
        let row_bytes = row_pack(header.width, header.index_bits)?;
        let expected = row_bytes
            .checked_mul(header.height as usize)
            .ok_or(Error::ArithmeticOverflow("packed palette payload size"))?;
        if samples.len() != expected {
            return Err(invalid("packed palette payload size mismatch"));
        }
        for y in 0..header.height {
            let start = y as usize * row_bytes;
            let row = &samples[start..start + row_bytes];
            for x in 0..header.width {
                let index = unpack_index(row, x, header.index_bits)?;
                if index >= header.palette_count {
                    return Err(invalid("palette index out of range"));
                }
                let source = index as usize * 4;
                let target = (y as usize * header.width as usize + x as usize) * 4;
                let color = palette
                    .get(source..source + 4)
                    .ok_or_else(|| invalid("external palette is truncated"))?;
                rgba[target..target + 4].copy_from_slice(color);
            }
        }
        return Ok(RgbaImage {
            width: header.width,
            height: header.height,
            rgba,
        });
    }

    let bpp = mode_bpp(header.mode).ok_or_else(|| invalid("invalid sample mode"))?;
    let expected = pixels
        .checked_mul(bpp)
        .ok_or(Error::ArithmeticOverflow("sample payload size"))?;
    if samples.len() != expected {
        return Err(invalid("sample payload size mismatch"));
    }
    for pixel in 0..pixels {
        let source = pixel * bpp;
        let target = pixel * 4;
        match header.mode {
            Mode::Gray => {
                rgba[target..target + 3].fill(samples[source]);
                rgba[target + 3] = 255;
            }
            Mode::GrayAlpha => {
                rgba[target..target + 3].fill(samples[source]);
                rgba[target + 3] = samples[source + 1];
            }
            Mode::Rgb | Mode::Rgba => {
                let (red, green, blue) = match header.transform {
                    Transform::GreenDelta | Transform::GreenDeltaRaw | Transform::GreenDeltaRle => {
                        let green = samples[source];
                        (
                            green.wrapping_add(samples[source + 1]),
                            green,
                            green.wrapping_add(samples[source + 2]),
                        )
                    }
                    Transform::RedDelta => {
                        let red = samples[source];
                        (
                            red,
                            red.wrapping_add(samples[source + 1]),
                            red.wrapping_add(samples[source + 2]),
                        )
                    }
                    Transform::BlueDelta => {
                        let blue = samples[source];
                        (
                            blue.wrapping_add(samples[source + 1]),
                            blue.wrapping_add(samples[source + 2]),
                            blue,
                        )
                    }
                    _ => (samples[source], samples[source + 1], samples[source + 2]),
                };
                rgba[target] = red;
                rgba[target + 1] = green;
                rgba[target + 2] = blue;
                rgba[target + 3] = if header.mode == Mode::Rgba {
                    samples[source + 3]
                } else {
                    255
                };
            }
            _ => return Err(invalid("invalid sample mode")),
        }
    }
    Ok(RgbaImage {
        width: header.width,
        height: header.height,
        rgba,
    })
}

fn decode_index_runs(
    runs: &[u8],
    header: &Header,
    palette: &[u8],
    limits: &Limits,
) -> Result<RgbaImage> {
    let pixels = checked_pixels(header)?;
    let mut rgba = filled_vec(checked_rgba_len(header, limits)?, "palette-run RGBA pixels")?;
    let mut input = 0;
    let mut pixel = 0_usize;
    while input < runs.len() {
        let run_minus_one = read_varint(runs, &mut input)?;
        let index = read_varint(runs, &mut input)?;
        let run = run_minus_one
            .checked_add(1)
            .ok_or_else(|| invalid("palette run length overflow"))?;
        let end = pixel
            .checked_add(run)
            .filter(|&end| end <= pixels)
            .ok_or_else(|| invalid("palette run exceeds pixel count"))?;
        if index >= header.palette_count as usize {
            return Err(invalid("palette run index out of range"));
        }
        let source = index
            .checked_mul(4)
            .ok_or(Error::ArithmeticOverflow("palette run color offset"))?;
        let color = palette
            .get(source..source + 4)
            .ok_or_else(|| invalid("external palette is truncated"))?;
        for target in pixel..end {
            rgba[target * 4..target * 4 + 4].copy_from_slice(color);
        }
        pixel = end;
    }
    if pixel != pixels {
        return Err(invalid("palette runs do not fill the image"));
    }
    Ok(RgbaImage {
        width: header.width,
        height: header.height,
        rgba,
    })
}

fn internal_palette_size(header: &Header) -> Result<usize> {
    usize::try_from(header.palette_count)
        .ok()
        .and_then(|count| count.checked_mul(4))
        .ok_or(Error::ArithmeticOverflow("internal palette size"))
}

fn decode_internal_packed_palette(
    indices: &[u8],
    header: &Header,
    palette: &[u8],
    limits: &Limits,
) -> Result<RgbaImage> {
    let mut palette_header = *header;
    palette_header.mode = Mode::Palette;
    palette_header.transform = Transform::IdentityRaw;
    decode_samples(indices, &palette_header, palette, limits)
}

fn local_palette_bits(count: usize) -> u8 {
    match count {
        0..=2 => 1,
        3..=4 => 2,
        5..=16 => 4,
        17..=256 => 8,
        _ => (usize::BITS - (count - 1).leading_zeros()) as u8,
    }
}

fn decode_tile_local_palette(
    payload: &[u8],
    header: &Header,
    limits: &Limits,
) -> Result<RgbaImage> {
    const MIN_TILE_LOG: u8 = 3;
    const MAX_TILE_LOG: u8 = 6;

    let palette_size = internal_palette_size(header)?;
    let tile_log = *payload
        .first()
        .ok_or_else(|| invalid("truncated tile-palette payload"))?;
    if !(MIN_TILE_LOG..=MAX_TILE_LOG).contains(&tile_log) {
        return Err(invalid("invalid tile-palette size"));
    }
    let palette_end = 1_usize
        .checked_add(palette_size)
        .ok_or(Error::ArithmeticOverflow("tile-palette data offset"))?;
    let palette = payload
        .get(1..palette_end)
        .ok_or_else(|| invalid("truncated tile-palette palette"))?;
    let width = header.width as usize;
    let height = header.height as usize;
    let tile_size = 1_usize << tile_log;
    let max_local_count = tile_size
        .checked_mul(tile_size)
        .ok_or(Error::ArithmeticOverflow("tile-local palette capacity"))?
        .min(header.palette_count as usize);
    let mut local_palette = Vec::new();
    local_palette
        .try_reserve_exact(max_local_count)
        .map_err(|_| Error::AllocationFailed("tile-local palette"))?;
    let mut rgba = filled_vec(
        checked_rgba_len(header, limits)?,
        "tile-palette RGBA pixels",
    )?;
    let mut position = palette_end;

    for y0 in (0..height).step_by(tile_size) {
        let tile_height = (height - y0).min(tile_size);
        for x0 in (0..width).step_by(tile_size) {
            let tile_width = (width - x0).min(tile_size);
            let tile_pixels = tile_width
                .checked_mul(tile_height)
                .ok_or(Error::ArithmeticOverflow("tile-palette pixel count"))?;
            let local_count_minus_one = read_varint(payload, &mut position)?;
            if local_count_minus_one >= tile_pixels
                || local_count_minus_one >= header.palette_count as usize
            {
                return Err(invalid("invalid tile-local palette size"));
            }
            let local_count = local_count_minus_one
                .checked_add(1)
                .ok_or(Error::ArithmeticOverflow("tile-local palette size"))?;
            local_palette.clear();
            let mut previous = 0_usize;
            for index in 0..local_count {
                let gap = read_varint(payload, &mut position)?;
                let global_index = if index == 0 {
                    gap
                } else {
                    previous
                        .checked_add(gap)
                        .and_then(|value| value.checked_add(1))
                        .ok_or_else(|| invalid("invalid tile-local palette gap"))?
                };
                if global_index >= header.palette_count as usize {
                    return Err(invalid("tile-local palette index out of range"));
                }
                local_palette.push(global_index);
                previous = global_index;
            }

            let local_bits = local_palette_bits(local_count);
            let packed_bytes = tile_pixels
                .checked_mul(usize::from(local_bits))
                .and_then(|bits| bits.checked_add(7))
                .map(|bits| bits >> 3)
                .ok_or(Error::ArithmeticOverflow("tile-local packed index size"))?;
            let packed_end = position
                .checked_add(packed_bytes)
                .ok_or(Error::ArithmeticOverflow("tile-local packed index end"))?;
            let packed = payload
                .get(position..packed_end)
                .ok_or_else(|| invalid("truncated tile-local indices"))?;
            for y in 0..tile_height {
                for x in 0..tile_width {
                    let pixel = y
                        .checked_mul(tile_width)
                        .and_then(|value| value.checked_add(x))
                        .ok_or(Error::ArithmeticOverflow("tile-local pixel index"))?;
                    let local_index = unpack_index(packed, pixel as u32, local_bits)? as usize;
                    let global_index = *local_palette
                        .get(local_index)
                        .ok_or_else(|| invalid("tile-local index out of range"))?;
                    let source = global_index
                        .checked_mul(4)
                        .ok_or(Error::ArithmeticOverflow("tile-palette color offset"))?;
                    let color = palette
                        .get(source..source + 4)
                        .ok_or_else(|| invalid("tile-palette color is truncated"))?;
                    let target = (y0 + y)
                        .checked_mul(width)
                        .and_then(|value| value.checked_add(x0 + x))
                        .and_then(|value| value.checked_mul(4))
                        .ok_or(Error::ArithmeticOverflow("tile-palette output offset"))?;
                    rgba[target..target + 4].copy_from_slice(color);
                }
            }
            position = packed_end;
        }
    }
    if position != payload.len() {
        return Err(invalid("trailing tile-palette data"));
    }
    Ok(RgbaImage {
        width: header.width,
        height: header.height,
        rgba,
    })
}

fn decode_planar_compressed_palette(
    payload: &[u8],
    header: &Header,
    limits: &Limits,
) -> Result<RgbaImage> {
    const INTERLEAVED_16: u8 = 0;
    const SPLIT_16: u8 = 1;

    let pixels = checked_pixels(header)?;
    let palette_size = internal_palette_size(header)?;
    let index_bytes = pixels
        .checked_mul(2)
        .ok_or(Error::ArithmeticOverflow("planar palette index bytes"))?;
    let palette_end = 1_usize
        .checked_add(palette_size)
        .ok_or(Error::ArithmeticOverflow("planar palette data offset"))?;
    let expected = palette_end
        .checked_add(index_bytes)
        .ok_or(Error::ArithmeticOverflow("planar palette payload size"))?;
    if payload.len() != expected {
        return Err(invalid("planar palette payload size mismatch"));
    }

    let layout = *payload
        .first()
        .ok_or_else(|| invalid("planar palette payload size mismatch"))?;
    if !matches!(layout, INTERLEAVED_16 | SPLIT_16) {
        return Err(invalid("invalid planar palette layout"));
    }

    let encoded_palette = payload
        .get(1..palette_end)
        .ok_or_else(|| invalid("planar palette payload size mismatch"))?;
    let indices = payload
        .get(palette_end..expected)
        .ok_or_else(|| invalid("planar palette payload size mismatch"))?;
    let palette_count = usize::try_from(header.palette_count)
        .map_err(|_| Error::ArithmeticOverflow("planar palette entry count"))?;
    let mut palette = filled_vec(palette_size, "planar palette")?;
    for channel in 0..4_usize {
        let plane_start = channel
            .checked_mul(palette_count)
            .ok_or(Error::ArithmeticOverflow("planar palette plane offset"))?;
        let plane_end = plane_start
            .checked_add(palette_count)
            .ok_or(Error::ArithmeticOverflow("planar palette plane size"))?;
        let plane = encoded_palette
            .get(plane_start..plane_end)
            .ok_or_else(|| invalid("planar palette payload size mismatch"))?;
        let mut previous = 0_u8;
        for (index, &delta) in plane.iter().enumerate() {
            let value = if index == 0 {
                delta
            } else {
                previous.wrapping_add(delta)
            };
            let target = index
                .checked_mul(4)
                .and_then(|offset| offset.checked_add(channel))
                .ok_or(Error::ArithmeticOverflow("planar palette color offset"))?;
            *palette
                .get_mut(target)
                .ok_or_else(|| invalid("planar palette payload size mismatch"))? = value;
            previous = value;
        }
    }

    let mut rgba = filled_vec(
        checked_rgba_len(header, limits)?,
        "planar palette RGBA pixels",
    )?;
    for pixel in 0..pixels {
        let index = match layout {
            INTERLEAVED_16 => {
                let offset = pixel
                    .checked_mul(2)
                    .ok_or(Error::ArithmeticOverflow("planar palette index offset"))?;
                let high_offset = offset
                    .checked_add(1)
                    .ok_or(Error::ArithmeticOverflow("planar palette index offset"))?;
                let low = *indices
                    .get(offset)
                    .ok_or_else(|| invalid("planar palette payload size mismatch"))?;
                let high = *indices
                    .get(high_offset)
                    .ok_or_else(|| invalid("planar palette payload size mismatch"))?;
                usize::from(u16::from_le_bytes([low, high]))
            }
            SPLIT_16 => {
                let high_offset = pixels
                    .checked_add(pixel)
                    .ok_or(Error::ArithmeticOverflow("planar palette index offset"))?;
                let low = *indices
                    .get(pixel)
                    .ok_or_else(|| invalid("planar palette payload size mismatch"))?;
                let high = *indices
                    .get(high_offset)
                    .ok_or_else(|| invalid("planar palette payload size mismatch"))?;
                usize::from(low) | (usize::from(high) << 8)
            }
            _ => return Err(invalid("invalid planar palette layout")),
        };
        if index >= palette_count {
            return Err(invalid("planar palette index out of range"));
        }
        let source = index
            .checked_mul(4)
            .ok_or(Error::ArithmeticOverflow("planar palette color offset"))?;
        let source_end = source
            .checked_add(4)
            .ok_or(Error::ArithmeticOverflow("planar palette color size"))?;
        let color = palette
            .get(source..source_end)
            .ok_or_else(|| invalid("planar palette index out of range"))?;
        let target = pixel
            .checked_mul(4)
            .ok_or(Error::ArithmeticOverflow("planar palette RGBA offset"))?;
        let target_end = target
            .checked_add(4)
            .ok_or(Error::ArithmeticOverflow("planar palette RGBA size"))?;
        rgba.get_mut(target..target_end)
            .ok_or_else(|| invalid("planar palette RGBA size mismatch"))?
            .copy_from_slice(color);
    }

    Ok(RgbaImage {
        width: header.width,
        height: header.height,
        rgba,
    })
}

fn decode_compressed_palette(
    payload: &[u8],
    header: &Header,
    limits: &Limits,
) -> Result<RgbaImage> {
    if header.transform == Transform::CompressedPaletteTiles {
        return decode_tile_local_palette(payload, header, limits);
    }
    if header.transform == Transform::CompressedPalettePlanar {
        return decode_planar_compressed_palette(payload, header, limits);
    }

    let palette_size = internal_palette_size(header)?;
    let (encoded_palette, indices) = payload
        .split_at_checked(palette_size)
        .ok_or_else(|| invalid("compressed-palette payload is truncated"))?;
    match header.transform {
        Transform::IdentityRaw => {
            decode_internal_packed_palette(indices, header, encoded_palette, limits)
        }
        Transform::IndexRle => decode_index_runs(indices, header, encoded_palette, limits),
        Transform::CompressedPaletteDelta => {
            let mut palette = filled_vec(palette_size, "delta palette")?;
            for (index, &delta) in encoded_palette.iter().enumerate() {
                palette[index] = if index < 4 {
                    delta
                } else {
                    palette[index - 4].wrapping_add(delta)
                };
            }
            decode_index_runs(indices, header, &palette, limits)
        }
        transform => Err(Error::UnsupportedPixelTransform {
            mode: header.mode as u8,
            transform: transform as u8,
        }),
    }
}

fn med_predict(left: u8, up: u8, upper_left: u8) -> u8 {
    let low = left.min(up);
    let high = left.max(up);
    (i32::from(left) + i32::from(up) - i32::from(upper_left)).clamp(i32::from(low), i32::from(high))
        as u8
}

fn planar_step(plane: &mut [u8], position: usize, width: usize) -> u8 {
    let value = plane[position].wrapping_add(med_predict(
        plane[position - 1],
        plane[position - width],
        plane[position - width - 1],
    ));
    plane[position] = value;
    value
}

fn decode_planar_med(mut residual: Vec<u8>, header: &Header, limits: &Limits) -> Result<RgbaImage> {
    let channels = mode_bpp(header.mode).ok_or_else(|| invalid("invalid planar MED mode"))?;
    if !matches!(channels, 3 | 4) {
        return Err(invalid("invalid planar MED mode"));
    }
    let pixels = checked_pixels(header)?;
    let expected = pixels
        .checked_mul(channels)
        .ok_or(Error::ArithmeticOverflow("planar MED payload size"))?;
    if residual.len() != expected {
        return Err(invalid("invalid planar MED payload size"));
    }
    let mut rgba = filled_vec(checked_rgba_len(header, limits)?, "planar MED RGBA pixels")?;
    let width = header.width as usize;
    let height = header.height as usize;
    let (blue, remainder) = residual.split_at_mut(pixels);
    let (red_delta, remainder) = remainder.split_at_mut(pixels);
    let (green_delta, alpha) = remainder.split_at_mut(pixels);

    for x in 0..width {
        if x != 0 {
            blue[x] = blue[x].wrapping_add(blue[x - 1]);
            red_delta[x] = red_delta[x].wrapping_add(red_delta[x - 1]);
            green_delta[x] = green_delta[x].wrapping_add(green_delta[x - 1]);
            if channels == 4 {
                alpha[x] = alpha[x].wrapping_add(alpha[x - 1]);
            }
        }
        let target = x * 4;
        rgba[target] = blue[x].wrapping_add(red_delta[x]);
        rgba[target + 1] = blue[x].wrapping_add(green_delta[x]);
        rgba[target + 2] = blue[x];
        rgba[target + 3] = if channels == 4 { alpha[x] } else { 255 };
    }
    for y in 1..height {
        let row = y * width;
        blue[row] = blue[row].wrapping_add(blue[row - width]);
        red_delta[row] = red_delta[row].wrapping_add(red_delta[row - width]);
        green_delta[row] = green_delta[row].wrapping_add(green_delta[row - width]);
        if channels == 4 {
            alpha[row] = alpha[row].wrapping_add(alpha[row - width]);
        }
        let target = row * 4;
        rgba[target] = blue[row].wrapping_add(red_delta[row]);
        rgba[target + 1] = blue[row].wrapping_add(green_delta[row]);
        rgba[target + 2] = blue[row];
        rgba[target + 3] = if channels == 4 { alpha[row] } else { 255 };
        for x in 1..width {
            let position = row + x;
            let blue_value = planar_step(blue, position, width);
            let red_value = planar_step(red_delta, position, width);
            let green_value = planar_step(green_delta, position, width);
            let alpha_value = if channels == 4 {
                planar_step(alpha, position, width)
            } else {
                255
            };
            let target = position * 4;
            rgba[target] = blue_value.wrapping_add(red_value);
            rgba[target + 1] = blue_value.wrapping_add(green_value);
            rgba[target + 2] = blue_value;
            rgba[target + 3] = alpha_value;
        }
    }
    Ok(RgbaImage {
        width: header.width,
        height: header.height,
        rgba,
    })
}

fn decode_separable(payload: &[u8], header: &Header, limits: &Limits) -> Result<RgbaImage> {
    let base_mode = match header.index_bits {
        1 => Mode::Gray,
        2 => Mode::GrayAlpha,
        3 => Mode::Rgb,
        4 => Mode::Rgba,
        _ => return Err(invalid("invalid separable base mode")),
    };
    let bpp = mode_bpp(base_mode).ok_or_else(|| invalid("invalid separable base mode"))?;
    let width = header.width as usize;
    let height = header.height as usize;
    let row_bytes = width
        .checked_mul(bpp)
        .ok_or(Error::ArithmeticOverflow("separable first row size"))?;
    let expected = height
        .checked_sub(1)
        .and_then(|rows| rows.checked_mul(bpp))
        .and_then(|columns| row_bytes.checked_add(columns))
        .ok_or(Error::ArithmeticOverflow("separable table size"))?;
    if payload.len() != expected {
        return Err(invalid("separable payload size mismatch"));
    }

    let table_storage;
    let table = if header.transform == Transform::SeparableDelta {
        let mut decoded = filled_vec(expected, "separable delta table")?;
        decoded[..bpp].copy_from_slice(&payload[..bpp]);
        let mut input = bpp;
        for x in 1..width {
            for channel in 0..bpp {
                let position = x * bpp + channel;
                decoded[position] = decoded[position - bpp].wrapping_add(payload[input]);
                input += 1;
            }
        }
        for y in 1..height {
            let current = row_bytes + (y - 1) * bpp;
            let previous = if y == 1 { 0 } else { current - bpp };
            for channel in 0..bpp {
                decoded[current + channel] =
                    decoded[previous + channel].wrapping_add(payload[input]);
                input += 1;
            }
        }
        table_storage = decoded;
        table_storage.as_slice()
    } else {
        payload
    };

    let sample_len = row_bytes
        .checked_mul(height)
        .ok_or(Error::ArithmeticOverflow("separable sample size"))?;
    let mut samples = filled_vec(sample_len, "separable samples")?;
    let first_row = &table[..row_bytes];
    let columns = &table[row_bytes..];
    for y in 0..height {
        let column = if y == 0 {
            &first_row[..bpp]
        } else {
            &columns[(y - 1) * bpp..y * bpp]
        };
        for x in 0..width {
            for channel in 0..bpp {
                samples[(y * width + x) * bpp + channel] = first_row[x * bpp + channel]
                    .wrapping_add(column[channel])
                    .wrapping_sub(first_row[channel]);
            }
        }
    }
    let sample_header = Header {
        mode: base_mode,
        transform: Transform::IdentityRaw,
        index_bits: 0,
        palette_count: 0,
        palette_size: 0,
        ..*header
    };
    decode_samples(&samples, &sample_header, &[], limits)
}

fn tile_u32(payload: &[u8], offset: usize) -> Result<u32> {
    let end = offset
        .checked_add(4)
        .ok_or(Error::ArithmeticOverflow("tile stream table offset"))?;
    let bytes = payload
        .get(offset..end)
        .ok_or_else(|| invalid("tile stream table is truncated"))?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

fn decode_tiles_into(
    payload: &[u8],
    header: &Header,
    limits: &Limits,
    rgba: &mut [u8],
) -> Result<()> {
    let count_u32 = tile_u32(payload, 0)?;
    if u64::from(count_u32) > u64::from(limits.max_chunks) {
        return Err(Error::LimitExceeded {
            kind: LimitKind::Chunks,
            limit: u64::from(limits.max_chunks),
            actual: u64::from(count_u32),
        });
    }
    if count_u32 == 0 || count_u32 > 65_536 {
        return Err(invalid("invalid tile stream chunk count"));
    }
    let tile_height = header.palette_count;
    let expected_count = 1_u32
        .checked_add((header.height - 1) / tile_height)
        .ok_or(Error::ArithmeticOverflow("tile stream chunk count"))?;
    if count_u32 != expected_count {
        return Err(invalid(
            "tile stream chunk count does not match image height",
        ));
    }
    let count = usize::try_from(count_u32)
        .map_err(|_| Error::ArithmeticOverflow("tile stream chunk count"))?;
    let table_end = count
        .checked_mul(4)
        .and_then(|bytes| bytes.checked_add(4))
        .ok_or(Error::ArithmeticOverflow("tile stream table size"))?;
    if table_end > payload.len() {
        return Err(invalid("tile stream table is truncated"));
    }

    let mut streams = Vec::<Qst1Stream<'_>>::new();
    streams
        .try_reserve_exact(count)
        .map_err(|_| Error::AllocationFailed("tile stream descriptors"))?;
    let mut chunk_offset = table_end;
    for index in 0..count {
        let size_offset = index
            .checked_mul(4)
            .and_then(|offset| offset.checked_add(4))
            .ok_or(Error::ArithmeticOverflow("tile stream size offset"))?;
        let chunk_size = usize::try_from(tile_u32(payload, size_offset)?)
            .map_err(|_| Error::ArithmeticOverflow("tile stream chunk size"))?;
        let chunk_end = chunk_offset
            .checked_add(chunk_size)
            .ok_or(Error::ArithmeticOverflow("tile stream chunk end"))?;
        let chunk = payload
            .get(chunk_offset..chunk_end)
            .ok_or_else(|| invalid("tile stream chunk exceeds payload"))?;
        let index_u32 = u32::try_from(index)
            .map_err(|_| Error::ArithmeticOverflow("tile stream chunk index"))?;
        let y = index_u32
            .checked_mul(tile_height)
            .ok_or(Error::ArithmeticOverflow("tile stream band offset"))?;
        let band_height = header
            .height
            .checked_sub(y)
            .ok_or_else(|| invalid("tile stream band exceeds image"))?
            .min(tile_height);
        let stream = parse_qst1(chunk, limits)?;
        if stream.info.width != header.width || stream.info.height != band_height {
            return Err(invalid(
                "tile stream chunk dimensions do not match its band",
            ));
        }
        if stream.info.channels != header.index_bits {
            return Err(invalid("tile stream chunk channels do not match header"));
        }
        streams.push(stream);
        chunk_offset = chunk_end;
    }
    if chunk_offset != payload.len() {
        return Err(invalid("trailing tile stream data"));
    }

    let row_bytes = usize::try_from(header.width)
        .map_err(|_| Error::ArithmeticOverflow("tile stream row width"))?
        .checked_mul(4)
        .ok_or(Error::ArithmeticOverflow("tile stream row bytes"))?;
    let rgba_len = checked_rgba_len(header, limits)?;
    if rgba.len() != rgba_len {
        return Err(invalid("RGBA destination does not match the image shape"));
    }
    for (index, stream) in streams.iter().enumerate() {
        let index_u32 = u32::try_from(index)
            .map_err(|_| Error::ArithmeticOverflow("tile stream chunk index"))?;
        let y = index_u32
            .checked_mul(tile_height)
            .ok_or(Error::ArithmeticOverflow("tile stream band offset"))?;
        let band_height = header
            .height
            .checked_sub(y)
            .ok_or_else(|| invalid("tile stream band exceeds image"))?
            .min(tile_height);
        let start = usize::try_from(y)
            .map_err(|_| Error::ArithmeticOverflow("tile stream output row"))?
            .checked_mul(row_bytes)
            .ok_or(Error::ArithmeticOverflow("tile stream output offset"))?;
        let band_bytes = usize::try_from(band_height)
            .map_err(|_| Error::ArithmeticOverflow("tile stream band height"))?
            .checked_mul(row_bytes)
            .ok_or(Error::ArithmeticOverflow("tile stream band bytes"))?;
        let end = start
            .checked_add(band_bytes)
            .ok_or(Error::ArithmeticOverflow("tile stream output end"))?;
        let destination = rgba
            .get_mut(start..end)
            .ok_or_else(|| invalid("tile stream band exceeds RGBA output"))?;
        decode_qst1_rgba_into(
            stream,
            header.width,
            band_height,
            header.index_bits,
            destination,
        )?;
    }
    Ok(())
}

fn decode_tiles(payload: &[u8], header: &Header, limits: &Limits) -> Result<RgbaImage> {
    let mut rgba = filled_vec(checked_rgba_len(header, limits)?, "tile stream RGBA pixels")?;
    decode_tiles_into(payload, header, limits, &mut rgba)?;
    Ok(RgbaImage {
        width: header.width,
        height: header.height,
        rgba,
    })
}

/// Decodes one 8-bit still image into caller-owned, tightly packed RGBA8.
///
/// Returns `(width, height)` on success. `rgba` must be exactly
/// `width * height * 4` bytes; call [`crate::parse_info`] first when the shape
/// is not already known. Native QST1 and stored tile-band streams write
/// directly into this buffer. Other supported outer modes may still use
/// bounded temporary storage internally. The destination is unspecified on
/// failure and can be reused for the next call.
pub fn decode_rgba_into(bytes: &[u8], limits: &Limits, rgba: &mut [u8]) -> Result<(u32, u32)> {
    let container = Container::parse(bytes, limits)?;
    let rgba_len = checked_rgba_len(&container.header, limits)?;
    if rgba.len() != rgba_len {
        return Err(invalid("RGBA destination does not match the image shape"));
    }

    if container.header.mode == Mode::Native {
        let owned_payload = decode_payload(&container, limits)?;
        let payload = owned_payload.as_deref().unwrap_or(container.payload);
        let stream = parse_qst1(payload, limits)?;
        decode_qst1_rgba_into(
            &stream,
            container.header.width,
            container.header.height,
            stream.info.channels,
            rgba,
        )?;
    } else if container.header.mode == Mode::Tiles {
        let owned_payload = decode_payload(&container, limits)?;
        let payload = owned_payload.as_deref().unwrap_or(container.payload);
        decode_tiles_into(payload, &container.header, limits, rgba)?;
    } else {
        let image = decode_rgba(bytes, limits)?;
        rgba.copy_from_slice(&image.rgba);
    }
    Ok((container.header.width, container.header.height))
}

/// Decodes the currently supported ordinary 8-bit still-image subset to RGBA.
///
/// Supports modes 1--5 with their production row-filtered, raw, RLE,
/// palette and planar-MED representations, mode-7 separable, and the complete
/// mode-13 compressed-palette grammar, and stored mode-14 bands containing
/// supported QST1 streams. Unsupported native entropy variants, model,
/// animation, block and wide streams return a structured error. No C or FFI
/// fallback is used.
pub fn decode_rgba(bytes: &[u8], limits: &Limits) -> Result<RgbaImage> {
    let container = Container::parse(bytes, limits)?;
    if container.header.mode == Mode::Native {
        checked_rgba_len(&container.header, limits)?;
        let owned_payload = decode_payload(&container, limits)?;
        let payload = owned_payload.as_deref().unwrap_or(container.payload);
        let rgba = decode_qst1_rgba(
            payload,
            container.header.width,
            container.header.height,
            limits,
        )?;
        return Ok(RgbaImage {
            width: container.header.width,
            height: container.header.height,
            rgba,
        });
    }
    if container.header.mode == Mode::CompressedPalette {
        checked_rgba_len(&container.header, limits)?;
        let owned_payload = decode_payload(&container, limits)?;
        let payload = owned_payload.as_deref().unwrap_or(container.payload);
        return decode_compressed_palette(payload, &container.header, limits);
    }
    if container.header.mode == Mode::Tiles {
        checked_rgba_len(&container.header, limits)?;
        let owned_payload = decode_payload(&container, limits)?;
        let payload = owned_payload.as_deref().unwrap_or(container.payload);
        return decode_tiles(payload, &container.header, limits);
    }
    if !matches!(
        container.header.mode,
        Mode::Gray | Mode::GrayAlpha | Mode::Rgb | Mode::Rgba | Mode::Palette | Mode::Separable
    ) {
        return Err(Error::UnsupportedPixelMode(container.header.mode as u8));
    }
    if !supported_transform(&container.header) {
        return Err(Error::UnsupportedPixelTransform {
            mode: container.header.mode as u8,
            transform: container.header.transform as u8,
        });
    }
    checked_rgba_len(&container.header, limits)?;
    if let Some(expected) = fixed_payload_size(&container.header)? {
        let declared = checked_usize(container.header.payload_size, "payload byte count")?;
        if declared != expected {
            return Err(invalid(
                "declared payload size does not match fixed representation",
            ));
        }
    }
    let mut owned_payload = decode_payload(&container, limits)?;

    if container.header.transform == Transform::BlueDeltaPlanarMed {
        let residual = match owned_payload.take() {
            Some(payload) => payload,
            None => copied_vec(container.payload, "stored planar MED payload")?,
        };
        return decode_planar_med(residual, &container.header, limits);
    }
    let payload = owned_payload.as_deref().unwrap_or(container.payload);
    if container.header.mode == Mode::Separable {
        return decode_separable(payload, &container.header, limits);
    }
    if container.header.mode == Mode::Palette && container.header.transform == Transform::IndexRle {
        return decode_index_runs(payload, &container.header, container.palette, limits);
    }

    let samples = match container.header.transform {
        Transform::IdentityRaw | Transform::GreenDeltaRaw => None,
        Transform::IdentityRle | Transform::GreenDeltaRle => {
            Some(decode_rle(payload, sample_size(&container.header)?)?)
        }
        Transform::Identity
        | Transform::GreenDelta
        | Transform::RedDelta
        | Transform::BlueDelta => {
            let (row_bytes, bpp) = if container.header.mode == Mode::Palette {
                (
                    row_pack(container.header.width, container.header.index_bits)?,
                    if container.header.index_bits == 16 {
                        2
                    } else {
                        1
                    },
                )
            } else {
                let bpp = mode_bpp(container.header.mode)
                    .ok_or_else(|| invalid("invalid row-filter sample mode"))?;
                (
                    (container.header.width as usize)
                        .checked_mul(bpp)
                        .ok_or(Error::ArithmeticOverflow("filtered row byte count"))?,
                    bpp,
                )
            };
            Some(unfilter_rows(
                payload,
                row_bytes,
                container.header.height,
                bpp,
            )?)
        }
        _ => {
            return Err(Error::UnsupportedPixelTransform {
                mode: container.header.mode as u8,
                transform: container.header.transform as u8,
            });
        }
    };
    decode_samples(
        samples.as_deref().unwrap_or(payload),
        &container.header,
        container.palette,
        limits,
    )
}
