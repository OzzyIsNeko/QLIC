use crate::cursor::Cursor;
use crate::{Error, LimitKind, Limits, Result, crc32};

const HEADER_SIZE: usize = 30;
const PROB_BITS: u32 = 12;
const PROB_ONE: u16 = 1 << PROB_BITS;
const PROB_INIT: u16 = PROB_ONE >> 1;
const ADAPT_DEFAULT: u8 = 5;
const ADAPT_FAST: u8 = 4;
const ADAPT_SLOW: u8 = 6;
const RANGE_TOP: u32 = 1 << 24;
const RUN_BITS: usize = 24;
const PATTERN_MAX: usize = 127;
const ACTIVITY_CONTEXTS: usize = 12;
const ERROR_CONTEXTS: usize = 6;
const BASE_CONTEXTS: usize = ACTIVITY_CONTEXTS * ERROR_CONTEXTS;
const PREDICTOR_CONTEXTS: usize = BASE_CONTEXTS * 4;
const MAX_MAGNITUDE_BITS: usize = 10;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qst1Info {
    pub width: u32,
    pub height: u32,
    pub channels: u8,
    pub flags: u8,
    pub mode: u8,
    pub transform: u8,
    pub tile_log: u8,
    pub control: u8,
    pub adaptation: u8,
    pub sample_bits: u8,
    pub pixel_crc32: u32,
    pub payload_size: u32,
    pub palette_count: u16,
    pub container_crc32: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct Qst1Stream<'a> {
    pub info: Qst1Info,
    pub palette: &'a [u8],
    pub payload: &'a [u8],
}

fn invalid(message: &'static str) -> Error {
    Error::InvalidQst1(message)
}

fn limit(kind: LimitKind, limit: u64, actual: u64) -> Result<()> {
    if actual > limit {
        Err(Error::LimitExceeded {
            kind,
            limit,
            actual,
        })
    } else {
        Ok(())
    }
}

fn mode_valid(mode: u8) -> bool {
    matches!(mode, 0..=4 | 8..=27 | 29..=54)
}

fn checksum_with_zeroed_field(bytes: &[u8]) -> u32 {
    let mut crc = u32::MAX;
    for (offset, &stored) in bytes.iter().enumerate() {
        let byte = if (26..30).contains(&offset) {
            0
        } else {
            stored
        };
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

pub fn parse_qst1<'a>(bytes: &'a [u8], limits: &Limits) -> Result<Qst1Stream<'a>> {
    limits.validate()?;
    let file_size = u64::try_from(bytes.len())
        .map_err(|_| Error::ArithmeticOverflow("QST1 file byte count"))?;
    limit(LimitKind::FileBytes, limits.max_file_bytes, file_size)?;
    if bytes.len() < HEADER_SIZE {
        return Err(Error::Truncated {
            offset: 0,
            needed: HEADER_SIZE,
            remaining: bytes.len(),
        });
    }
    let mut cursor = Cursor::new(&bytes[..HEADER_SIZE]);
    if cursor.take(4)? != b"QST1" {
        return Err(invalid("invalid magic"));
    }
    let width = cursor.read_u32_le()?;
    let height = cursor.read_u32_le()?;
    let channels = cursor.read_u8()?;
    let flags = cursor.read_u8()?;
    let mode = cursor.read_u8()?;
    let transform = cursor.read_u8()?;
    let tile_log = cursor.read_u8()?;
    let control = cursor.read_u8()?;
    let pixel_crc32 = cursor.read_u32_le()?;
    let payload_size = cursor.read_u32_le()?;
    let container_crc32 = cursor.read_u32_le()?;

    if width == 0 || height == 0 {
        return Err(invalid("invalid dimensions"));
    }
    if !matches!(channels, 1 | 3 | 4) {
        return Err(invalid("invalid channel count"));
    }
    if flags & !31 != 0 {
        return Err(invalid("invalid flags"));
    }
    if !mode_valid(mode) {
        return Err(invalid("invalid entropy mode"));
    }
    if transform >= 41 {
        return Err(invalid("invalid transform"));
    }
    if tile_log > 7 {
        return Err(invalid("invalid tile logarithm"));
    }
    let gray = flags & 1 != 0;
    let constant_alpha = flags & 2 != 0;
    let sample_bits = (flags >> 2) & 7;
    if gray && channels != 3 {
        return Err(invalid("gray flag requires three channels"));
    }
    if constant_alpha && channels != 4 {
        return Err(invalid("constant-alpha flag requires four channels"));
    }
    if sample_bits != 0 && mode == 42 {
        return Err(invalid("sample grid is invalid with split mode"));
    }
    if mode == 1 && ((flags & 3) != 0 || transform != 0 || channels < 3) {
        return Err(invalid("invalid palette-mode header"));
    }
    if mode != 1 && (channels == 1 || gray) && transform != 0 {
        return Err(invalid("gray stream has a color transform"));
    }
    if matches!(mode, 38 | 39) && tile_log != 0 {
        return Err(invalid("entropy mode requires tile-log zero"));
    }
    if mode == 40 && tile_log != 1 {
        return Err(invalid("pattern mode requires tile-log one"));
    }
    let adaptation = if constant_alpha || control == 0 {
        ADAPT_DEFAULT
    } else if matches!(control, ADAPT_FAST | ADAPT_SLOW) {
        control
    } else {
        return Err(invalid("invalid adaptation control"));
    };

    let pixels = u64::from(width)
        .checked_mul(u64::from(height))
        .ok_or(Error::ArithmeticOverflow("QST1 pixel count"))?;
    limit(LimitKind::Pixels, limits.max_pixels, pixels)?;
    if mode == 39 && pixels > 0x00ff_ffff {
        return Err(invalid("mode-39 plane exceeds event rank limit"));
    }
    let rgba_bytes = pixels
        .checked_mul(4)
        .ok_or(Error::ArithmeticOverflow("QST1 RGBA byte count"))?;
    limit(
        LimitKind::DecodedBytes,
        limits.max_decoded_bytes,
        rgba_bytes,
    )?;
    if usize::try_from(rgba_bytes).is_err() {
        return Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            limit: usize::MAX as u64,
            actual: rgba_bytes,
        });
    }
    limit(
        LimitKind::PayloadBytes,
        limits.max_payload_bytes,
        u64::from(payload_size),
    )?;

    let mut payload_offset = HEADER_SIZE;
    let palette_count = if mode == 1 {
        let count_bytes = bytes
            .get(HEADER_SIZE..HEADER_SIZE + 2)
            .ok_or_else(|| invalid("truncated palette count"))?;
        let count = u16::from_le_bytes([count_bytes[0], count_bytes[1]]);
        if count == 0 || count > 256 {
            return Err(invalid("invalid palette count"));
        }
        let palette_bytes = usize::from(count)
            .checked_mul(usize::from(channels))
            .ok_or(Error::ArithmeticOverflow("QST1 palette size"))?;
        payload_offset = payload_offset
            .checked_add(2)
            .and_then(|offset| offset.checked_add(palette_bytes))
            .ok_or(Error::ArithmeticOverflow("QST1 payload offset"))?;
        count
    } else {
        0
    };
    let payload_len = usize::try_from(payload_size)
        .map_err(|_| Error::ArithmeticOverflow("QST1 payload byte count"))?;
    let expected_size = payload_offset
        .checked_add(payload_len)
        .ok_or(Error::ArithmeticOverflow("QST1 stream size"))?;
    if expected_size != bytes.len() {
        return Err(invalid("declared payload size does not match stream"));
    }
    let actual_container_crc = checksum_with_zeroed_field(bytes);
    if actual_container_crc != container_crc32 {
        return Err(invalid("container checksum mismatch"));
    }
    let palette_start = if mode == 1 {
        HEADER_SIZE + 2
    } else {
        HEADER_SIZE
    };
    Ok(Qst1Stream {
        info: Qst1Info {
            width,
            height,
            channels,
            flags,
            mode,
            transform,
            tile_log,
            control,
            adaptation,
            sample_bits,
            pixel_crc32,
            payload_size,
            palette_count,
            container_crc32,
        },
        palette: &bytes[palette_start..payload_offset],
        payload: &bytes[payload_offset..],
    })
}

struct RangeDecoder<'a> {
    source: &'a [u8],
    next: usize,
    range: u32,
    code: u32,
    adaptation: u32,
}

impl<'a> RangeDecoder<'a> {
    fn new(source: &'a [u8], adaptation: u8) -> Result<Self> {
        if source.len() < 5 {
            return Err(invalid("range payload is truncated"));
        }
        let mut code = 0_u32;
        for &byte in &source[..5] {
            code = code.wrapping_shl(8) | u32::from(byte);
        }
        Ok(Self {
            source,
            next: 5,
            range: u32::MAX,
            code,
            adaptation: u32::from(adaptation),
        })
    }

    fn byte(&mut self) -> Result<u8> {
        let byte = self
            .source
            .get(self.next)
            .copied()
            .ok_or_else(|| invalid("range payload is truncated"))?;
        self.next += 1;
        Ok(byte)
    }

    fn bit_probability(&mut self, probability: u16) -> Result<bool> {
        let current = u32::from(probability);
        let bound = (self.range >> PROB_BITS) * current;
        let bit = self.code >= bound;
        if bit {
            self.code -= bound;
            self.range -= bound;
        } else {
            self.range = bound;
        }
        for _ in 0..2 {
            if self.range >= RANGE_TOP {
                break;
            }
            self.range <<= 8;
            self.code = self.code.wrapping_shl(8) | u32::from(self.byte()?);
        }
        Ok(bit)
    }

    fn bit(&mut self, probability: &mut u16) -> Result<bool> {
        let bit = self.bit_probability(*probability)?;
        update_probability(probability, bit, self.adaptation);
        Ok(bit)
    }
}

fn update_probability(probability: &mut u16, bit: bool, adaptation: u32) {
    if bit {
        *probability = probability.wrapping_sub(*probability >> adaptation);
    } else {
        *probability = probability.wrapping_add((PROB_ONE - *probability) >> adaptation);
    }
}

struct ProbabilityModel {
    unary: Vec<u16>,
    mantissa: Vec<u16>,
    nonzero_sign: Vec<u16>,
    signed_hint: Vec<u16>,
    predictor_tree: [u16; 8],
    predictor_tree_wide: [u16; 32],
}

impl ProbabilityModel {
    fn new() -> Result<Self> {
        Ok(Self {
            unary: fallible_filled(
                PREDICTOR_CONTEXTS * (MAX_MAGNITUDE_BITS + 1),
                PROB_INIT,
                "QST1 unary model",
            )?,
            mantissa: fallible_filled(
                PREDICTOR_CONTEXTS * (MAX_MAGNITUDE_BITS + 1) * MAX_MAGNITUDE_BITS,
                PROB_INIT,
                "QST1 mantissa model",
            )?,
            nonzero_sign: fallible_filled(PREDICTOR_CONTEXTS, PROB_INIT, "QST1 sign model")?,
            signed_hint: fallible_filled(
                PREDICTOR_CONTEXTS * 2,
                PROB_INIT,
                "QST1 hinted-sign model",
            )?,
            predictor_tree: [PROB_INIT; 8],
            predictor_tree_wide: [PROB_INIT; 32],
        })
    }

    fn unary(&mut self, context: usize, bit: usize) -> Result<&mut u16> {
        let index = context
            .checked_mul(MAX_MAGNITUDE_BITS + 1)
            .and_then(|base| base.checked_add(bit))
            .ok_or(Error::ArithmeticOverflow("QST1 unary context"))?;
        self.unary
            .get_mut(index)
            .ok_or_else(|| invalid("unary context is out of range"))
    }

    fn mantissa(&mut self, context: usize, length: usize, bit: usize) -> Result<&mut u16> {
        let index = context
            .checked_mul(MAX_MAGNITUDE_BITS + 1)
            .and_then(|base| base.checked_add(length))
            .and_then(|base| base.checked_mul(MAX_MAGNITUDE_BITS))
            .and_then(|base| base.checked_add(bit))
            .ok_or(Error::ArithmeticOverflow("QST1 mantissa context"))?;
        self.mantissa
            .get_mut(index)
            .ok_or_else(|| invalid("mantissa context is out of range"))
    }

    fn sign(&mut self, context: usize, hint: i32) -> Result<&mut u16> {
        if hint == 0 {
            return self
                .nonzero_sign
                .get_mut(context)
                .ok_or_else(|| invalid("sign context is out of range"));
        }
        let index = context
            .checked_mul(2)
            .and_then(|base| base.checked_add(usize::from(hint < 0)))
            .ok_or(Error::ArithmeticOverflow("QST1 hinted-sign context"))?;
        self.signed_hint
            .get_mut(index)
            .ok_or_else(|| invalid("hinted-sign context is out of range"))
    }

    fn tree3(&mut self, decoder: &mut RangeDecoder<'_>) -> Result<usize> {
        let mut node = 1_usize;
        for _ in 0..3 {
            let bit = usize::from(
                decoder.bit(
                    self.predictor_tree
                        .get_mut(node)
                        .ok_or_else(|| invalid("predictor tree node is out of range"))?,
                )?,
            );
            node = node * 2 + bit;
        }
        Ok(node & 7)
    }

    fn tree5(&mut self, decoder: &mut RangeDecoder<'_>) -> Result<usize> {
        let mut node = 1_usize;
        for _ in 0..5 {
            let bit = usize::from(
                decoder.bit(
                    self.predictor_tree_wide
                        .get_mut(node)
                        .ok_or_else(|| invalid("wide predictor tree node is out of range"))?,
                )?,
            );
            node = node * 2 + bit;
        }
        Ok(node & 31)
    }
}

struct EventModel {
    unary: Vec<u16>,
    mantissa: Vec<u16>,
    nonzero_sign: Vec<u16>,
    signed_hint: Vec<u16>,
    run_unary: Vec<u16>,
    run_mantissa: Vec<u16>,
    predictor_tree: [u16; 8],
}

impl EventModel {
    fn new() -> Result<Self> {
        Ok(Self {
            unary: fallible_filled(
                BASE_CONTEXTS * (MAX_MAGNITUDE_BITS + 1),
                PROB_INIT,
                "QST1 mode-39 unary model",
            )?,
            mantissa: fallible_filled(
                BASE_CONTEXTS * (MAX_MAGNITUDE_BITS + 1) * MAX_MAGNITUDE_BITS,
                PROB_INIT,
                "QST1 mode-39 mantissa model",
            )?,
            nonzero_sign: fallible_filled(BASE_CONTEXTS, PROB_INIT, "QST1 mode-39 sign model")?,
            signed_hint: fallible_filled(
                BASE_CONTEXTS * 2,
                PROB_INIT,
                "QST1 mode-39 hinted-sign model",
            )?,
            run_unary: fallible_filled(
                BASE_CONTEXTS * (RUN_BITS + 1),
                PROB_INIT,
                "QST1 mode-39 run unary model",
            )?,
            run_mantissa: fallible_filled(
                BASE_CONTEXTS * (RUN_BITS + 1) * RUN_BITS,
                PROB_INIT,
                "QST1 mode-39 run mantissa model",
            )?,
            predictor_tree: [PROB_INIT; 8],
        })
    }

    fn unary(&mut self, context: usize, bit: usize) -> Result<&mut u16> {
        let index = context
            .checked_mul(MAX_MAGNITUDE_BITS + 1)
            .and_then(|base| base.checked_add(bit))
            .ok_or(Error::ArithmeticOverflow("QST1 mode-39 unary context"))?;
        self.unary
            .get_mut(index)
            .ok_or_else(|| invalid("mode-39 unary context is out of range"))
    }

    fn mantissa(&mut self, context: usize, length: usize, bit: usize) -> Result<&mut u16> {
        let index = context
            .checked_mul(MAX_MAGNITUDE_BITS + 1)
            .and_then(|base| base.checked_add(length))
            .and_then(|base| base.checked_mul(MAX_MAGNITUDE_BITS))
            .and_then(|base| base.checked_add(bit))
            .ok_or(Error::ArithmeticOverflow("QST1 mode-39 mantissa context"))?;
        self.mantissa
            .get_mut(index)
            .ok_or_else(|| invalid("mode-39 mantissa context is out of range"))
    }

    fn sign(&mut self, context: usize, hint: i32) -> Result<&mut u16> {
        if hint == 0 {
            return self
                .nonzero_sign
                .get_mut(context)
                .ok_or_else(|| invalid("mode-39 sign context is out of range"));
        }
        let index = context
            .checked_mul(2)
            .and_then(|base| base.checked_add(usize::from(hint < 0)))
            .ok_or(Error::ArithmeticOverflow(
                "QST1 mode-39 hinted-sign context",
            ))?;
        self.signed_hint
            .get_mut(index)
            .ok_or_else(|| invalid("mode-39 hinted-sign context is out of range"))
    }

    fn tree3(&mut self, decoder: &mut RangeDecoder<'_>) -> Result<usize> {
        let mut node = 1_usize;
        for _ in 0..3 {
            let bit = usize::from(
                decoder.bit(
                    self.predictor_tree
                        .get_mut(node)
                        .ok_or_else(|| invalid("mode-39 predictor tree node is out of range"))?,
                )?,
            );
            node = node * 2 + bit;
        }
        Ok(node & 7)
    }

    fn run_unary(&mut self, context: usize, bit: usize) -> Result<&mut u16> {
        let index = context
            .checked_mul(RUN_BITS + 1)
            .and_then(|base| base.checked_add(bit))
            .ok_or(Error::ArithmeticOverflow("QST1 mode-39 run unary context"))?;
        self.run_unary
            .get_mut(index)
            .ok_or_else(|| invalid("mode-39 run unary context is out of range"))
    }

    fn run_mantissa(&mut self, context: usize, length: usize, bit: usize) -> Result<&mut u16> {
        let index = context
            .checked_mul(RUN_BITS + 1)
            .and_then(|base| base.checked_add(length))
            .and_then(|base| base.checked_mul(RUN_BITS))
            .and_then(|base| base.checked_add(bit))
            .ok_or(Error::ArithmeticOverflow(
                "QST1 mode-39 run mantissa context",
            ))?;
        self.run_mantissa
            .get_mut(index)
            .ok_or_else(|| invalid("mode-39 run mantissa context is out of range"))
    }
}

struct Coarse52Model {
    zero: Vec<u16>,
    magnitude: Vec<u16>,
    nonzero_sign: Vec<u16>,
    signed_hint: Vec<u16>,
    unary: Vec<u16>,
    mantissa: Vec<u16>,
}

struct Root52Model {
    zero: Vec<u16>,
    magnitude: Vec<u16>,
    nonzero_sign: Vec<u16>,
    signed_hint: Vec<u16>,
    unary: Vec<u16>,
    mantissa: Vec<u16>,
}

struct Exact52Model {
    nonzero_sign: Vec<u16>,
    signed_hint: Vec<u16>,
}

struct Mode52Model {
    fine: ProbabilityModel,
    coarse: Coarse52Model,
    root: Root52Model,
    exact: Exact52Model,
}

impl Mode52Model {
    fn new() -> Result<Self> {
        Ok(Self {
            fine: ProbabilityModel::new()?,
            coarse: Coarse52Model {
                zero: fallible_filled(BASE_CONTEXTS, PROB_INIT, "QST1 mode-52 coarse zero")?,
                magnitude: fallible_filled(
                    BASE_CONTEXTS,
                    PROB_INIT,
                    "QST1 mode-52 coarse magnitude",
                )?,
                nonzero_sign: fallible_filled(
                    BASE_CONTEXTS,
                    PROB_INIT,
                    "QST1 mode-52 coarse sign",
                )?,
                signed_hint: fallible_filled(
                    BASE_CONTEXTS * 2,
                    PROB_INIT,
                    "QST1 mode-52 coarse hinted sign",
                )?,
                unary: fallible_filled(
                    BASE_CONTEXTS * (MAX_MAGNITUDE_BITS + 1),
                    PROB_INIT,
                    "QST1 mode-52 coarse unary",
                )?,
                mantissa: fallible_filled(
                    BASE_CONTEXTS * (MAX_MAGNITUDE_BITS + 1) * MAX_MAGNITUDE_BITS,
                    PROB_INIT,
                    "QST1 mode-52 coarse mantissa",
                )?,
            },
            root: Root52Model {
                zero: fallible_filled(ACTIVITY_CONTEXTS, PROB_INIT, "QST1 mode-52 root zero")?,
                magnitude: fallible_filled(
                    ACTIVITY_CONTEXTS,
                    PROB_INIT,
                    "QST1 mode-52 root magnitude",
                )?,
                nonzero_sign: fallible_filled(
                    ACTIVITY_CONTEXTS,
                    PROB_INIT,
                    "QST1 mode-52 root sign",
                )?,
                signed_hint: fallible_filled(
                    ACTIVITY_CONTEXTS * 2,
                    PROB_INIT,
                    "QST1 mode-52 root hinted sign",
                )?,
                unary: fallible_filled(
                    ACTIVITY_CONTEXTS * (MAX_MAGNITUDE_BITS + 1),
                    PROB_INIT,
                    "QST1 mode-52 root unary",
                )?,
                mantissa: fallible_filled(
                    ACTIVITY_CONTEXTS * (MAX_MAGNITUDE_BITS + 1) * MAX_MAGNITUDE_BITS,
                    PROB_INIT,
                    "QST1 mode-52 root mantissa",
                )?,
            },
            exact: Exact52Model {
                nonzero_sign: fallible_filled(
                    32 * BASE_CONTEXTS * 3,
                    PROB_INIT,
                    "QST1 mode-52 exact sign",
                )?,
                signed_hint: fallible_filled(
                    32 * BASE_CONTEXTS * 3 * 2,
                    PROB_INIT,
                    "QST1 mode-52 exact hinted sign",
                )?,
            },
        })
    }
}

fn decode_root_bit(
    decoder: &mut RangeDecoder<'_>,
    fine: &mut u16,
    coarse: &mut u16,
    root: &mut u16,
    slow: u32,
) -> Result<bool> {
    let coarse_mix = (u32::from(*coarse) + u32::from(*root) + 1) >> 1;
    let mixed = ((u32::from(*fine) + coarse_mix + 1) >> 1) as u16;
    let bit = decoder.bit_probability(mixed)?;
    update_probability(fine, bit, decoder.adaptation);
    update_probability(coarse, bit, decoder.adaptation + slow);
    update_probability(root, bit, decoder.adaptation + slow);
    Ok(bit)
}

struct RootProbabilities<'a> {
    fine: &'a mut u16,
    coarse: &'a mut u16,
    root: &'a mut u16,
}

#[derive(Clone, Copy)]
struct ChildRootMix {
    slow: u32,
    weight: u32,
    child_rate: i32,
}

fn decode_child_root_bit(
    decoder: &mut RangeDecoder<'_>,
    child: &mut u16,
    probabilities: RootProbabilities<'_>,
    mix: ChildRootMix,
) -> Result<bool> {
    let coarse_mix = (u32::from(*probabilities.coarse) + u32::from(*probabilities.root) + 1) >> 1;
    let parent_mix = (u32::from(*probabilities.fine) + coarse_mix + 1) >> 1;
    let mixed = ((mix.weight * u32::from(*child) + (8 - mix.weight) * parent_mix + 4) >> 3) as u16;
    let bit = decoder.bit_probability(mixed)?;
    update_probability(
        child,
        bit,
        adjusted_adaptation(decoder.adaptation, mix.child_rate)?,
    );
    update_probability(probabilities.fine, bit, decoder.adaptation);
    update_probability(probabilities.coarse, bit, decoder.adaptation + mix.slow);
    update_probability(probabilities.root, bit, decoder.adaptation + mix.slow);
    Ok(bit)
}

fn decode_exact_root_sign(
    decoder: &mut RangeDecoder<'_>,
    exact: &mut u16,
    fine: &mut u16,
    coarse: &mut u16,
    root: &mut u16,
    slow: u32,
    exact_rate: i32,
) -> Result<bool> {
    let coarse_mix = (u32::from(*coarse) + u32::from(*root) + 1) >> 1;
    let parent_mix = (u32::from(*fine) + coarse_mix + 1) >> 1;
    let mixed = ((5 * u32::from(*exact) + 3 * parent_mix + 4) >> 3) as u16;
    let bit = decoder.bit_probability(mixed)?;
    update_probability(
        exact,
        bit,
        adjusted_adaptation(decoder.adaptation, exact_rate)?,
    );
    update_probability(fine, bit, decoder.adaptation);
    update_probability(coarse, bit, decoder.adaptation + slow);
    update_probability(root, bit, decoder.adaptation + slow);
    Ok(bit)
}

fn decode_cross_exact_sign(
    decoder: &mut RangeDecoder<'_>,
    child: &mut u16,
    exact: &mut u16,
    probabilities: RootProbabilities<'_>,
    mix: CrossExactMix,
) -> Result<bool> {
    let mixed =
        ((mix.weight * u32::from(*child) + (16 - mix.weight) * u32::from(*exact) + 8) >> 4) as u16;
    let bit = decoder.bit_probability(mixed)?;
    update_probability(
        child,
        bit,
        adjusted_adaptation(decoder.adaptation, mix.child_rate)?,
    );
    update_probability(
        exact,
        bit,
        adjusted_adaptation(decoder.adaptation, mix.exact_rate)?,
    );
    update_probability(probabilities.fine, bit, decoder.adaptation);
    update_probability(probabilities.coarse, bit, decoder.adaptation + mix.slow);
    update_probability(probabilities.root, bit, decoder.adaptation + mix.slow);
    Ok(bit)
}

#[derive(Clone, Copy)]
struct CrossExactMix {
    slow: u32,
    weight: u32,
    child_rate: i32,
    exact_rate: i32,
}

fn adjusted_adaptation(adaptation: u32, encoded_rate: i32) -> Result<u32> {
    if encoded_rate >= 0 {
        adaptation
            .checked_sub(encoded_rate as u32)
            .ok_or_else(|| invalid("invalid probability adaptation rate"))
    } else {
        adaptation
            .checked_add(encoded_rate.unsigned_abs())
            .ok_or(Error::ArithmeticOverflow("probability adaptation rate"))
    }
}

fn activity_context(activity: i32) -> usize {
    if activity < 0 {
        return 0;
    }
    let activity = activity as u32;
    if activity > 1_024 {
        11
    } else if activity <= 2 {
        activity as usize
    } else {
        nbits(activity - 1) + 1
    }
}

fn error_context(activity: usize, previous_bits: usize, previous_sign: i32, signed: bool) -> usize {
    if !signed {
        let bucket = if previous_bits == 0 {
            0
        } else if previous_bits <= 2 {
            1
        } else {
            2
        };
        return activity * 3 + bucket;
    }
    let bucket = if previous_bits == 0 {
        0
    } else if previous_bits <= 2 {
        if previous_sign > 0 { 1 } else { 2 }
    } else if previous_bits <= 4 {
        if previous_sign > 0 { 3 } else { 4 }
    } else {
        5
    };
    activity * ERROR_CONTEXTS + bucket
}

fn predictor_context(predictor: usize) -> usize {
    if predictor == 0 {
        0
    } else if predictor <= 4 {
        BASE_CONTEXTS
    } else if predictor <= 10 {
        BASE_CONTEXTS * 2
    } else {
        BASE_CONTEXTS * 3
    }
}

fn clamp_sample(value: i32, maximum: i32) -> i32 {
    value.clamp(0, maximum)
}

fn median_edge(left: i32, up: i32, upper_left: i32) -> i32 {
    let maximum = left.max(up);
    let minimum = left.min(up);
    if upper_left >= maximum {
        minimum
    } else if upper_left <= minimum {
        maximum
    } else {
        up + left - upper_left
    }
}

fn paeth(left: i32, up: i32, upper_left: i32) -> i32 {
    let estimate = left + up - upper_left;
    let left_distance = (estimate - left).abs();
    let up_distance = (estimate - up).abs();
    let corner_distance = (estimate - upper_left).abs();
    if left_distance <= up_distance && left_distance <= corner_distance {
        left
    } else if up_distance <= corner_distance {
        up
    } else {
        upper_left
    }
}

fn gradient_adaptive(
    left: i32,
    up: i32,
    upper_left: i32,
    upper_right: i32,
    second_left: i32,
    second_up: i32,
    maximum: i32,
) -> i32 {
    let horizontal =
        (left - second_left).abs() + (up - upper_left).abs() + (upper_right - up).abs();
    let vertical = (left - upper_left).abs() + (up - second_up).abs() + (upper_right - up).abs();
    let difference = vertical - horizontal;
    let mut prediction;
    if difference > 80 {
        prediction = left;
    } else if difference < -80 {
        prediction = up;
    } else {
        prediction = ((left + up) >> 1) + ((upper_right - upper_left) >> 2);
        if difference > 32 {
            prediction = (prediction + left) >> 1;
        } else if difference > 8 {
            prediction = (3 * prediction + left) >> 2;
        } else if difference < -32 {
            prediction = (prediction + up) >> 1;
        } else if difference < -8 {
            prediction = (3 * prediction + up) >> 2;
        }
    }
    clamp_sample(prediction, maximum)
}

#[allow(clippy::too_many_arguments)]
fn predict_basic(
    predictor: usize,
    left: i32,
    up: i32,
    upper_left: i32,
    upper_right: i32,
    maximum: i32,
) -> i32 {
    match predictor {
        0 => median_edge(left, up, upper_left),
        1 => left,
        2 => up,
        3 => (left + up + 1) >> 1,
        4 => clamp_sample(up + left - upper_left, maximum),
        5 => upper_right,
        6 => (left + upper_right + 1) >> 1,
        _ => clamp_sample((3 * (left + up) - 2 * upper_left + 2) >> 2, maximum),
    }
}

#[allow(clippy::too_many_arguments)]
fn predict_adaptive(
    predictor: usize,
    left: i32,
    up: i32,
    upper_left: i32,
    upper_right: i32,
    second_left: i32,
    second_up: i32,
    maximum: i32,
) -> i32 {
    match predictor {
        0 => median_edge(left, up, upper_left),
        1 => paeth(left, up, upper_left),
        2 => left,
        3 => up,
        4 => (left + up + 1) >> 1,
        5 => clamp_sample(up + left - upper_left, maximum),
        6 => upper_right,
        7 => (left + upper_right + 1) >> 1,
        8 => (up + upper_right + 1) >> 1,
        9 => clamp_sample(2 * left - second_left, maximum),
        10 => clamp_sample(2 * up - second_up, maximum),
        11 => clamp_sample(left + (((up - upper_left) * 3) >> 2), maximum),
        12 => clamp_sample(up + (((left - upper_left) * 3) >> 2), maximum),
        13 => gradient_adaptive(
            left,
            up,
            upper_left,
            upper_right,
            second_left,
            second_up,
            maximum,
        ),
        14 => clamp_sample((left + up + upper_right + upper_left + 2) >> 2, maximum),
        15 => clamp_sample(
            (5 * left + 2 * up - 3 * upper_left + upper_right + 2) >> 2,
            maximum,
        ),
        16 => clamp_sample((left + 3 * up + 2) >> 2, maximum),
        17 => clamp_sample((3 * left + up + 2) >> 2, maximum),
        18 => clamp_sample(
            (5 * up + 2 * left - 3 * upper_left + upper_right + 2) >> 2,
            maximum,
        ),
        19 => clamp_sample((2 * left + up - upper_left + 1) >> 1, maximum),
        20 => clamp_sample((left + 2 * up - upper_left + 1) >> 1, maximum),
        21 => clamp_sample(left + ((upper_right - upper_left) >> 1), maximum),
        22 => clamp_sample(up + ((upper_right - upper_left) >> 1), maximum),
        23 => clamp_sample((left + up + upper_right + 1) / 3, maximum),
        24 => clamp_sample((2 * left + up + upper_right + 2) >> 2, maximum),
        25 => clamp_sample((left + 2 * up + upper_left + 2) >> 2, maximum),
        26 => clamp_sample((3 * left + 3 * up - 2 * upper_left + 2) >> 2, maximum),
        27 => clamp_sample(
            (4 * up + left - 2 * upper_left + upper_right + 2) >> 2,
            maximum,
        ),
        28 => clamp_sample(
            (4 * left + up - 2 * upper_left + upper_right + 2) >> 2,
            maximum,
        ),
        29 => clamp_sample((left + up + upper_right - upper_left + 1) >> 1, maximum),
        30 => clamp_sample(
            (6 * left + 2 * up - 5 * upper_left + upper_right + 2) >> 2,
            maximum,
        ),
        _ => clamp_sample(
            (2 * left + 6 * up - 5 * upper_left + upper_right + 2) >> 2,
            maximum,
        ),
    }
}

#[allow(clippy::too_many_arguments)]
fn sign_hint(
    left: i32,
    up: i32,
    upper_left: i32,
    upper_right: i32,
    second_left: i32,
    second_up: i32,
    prediction: i32,
    maximum: i32,
) -> i32 {
    let reference = gradient_adaptive(
        left,
        up,
        upper_left,
        upper_right,
        second_left,
        second_up,
        maximum,
    );
    (reference > prediction) as i32 - (reference < prediction) as i32
}

fn decode_unsigned_magnitude(
    decoder: &mut RangeDecoder<'_>,
    model: &mut ProbabilityModel,
    context: usize,
    depth: usize,
    explicit_zero: bool,
) -> Result<usize> {
    let mut length = 0_usize;
    if explicit_zero {
        if decoder.bit(model.unary(context, 0)?)? {
            length = 1;
            if decoder.bit(model.unary(context, 1)?)? {
                length = 2;
                while length < depth && decoder.bit(model.unary(context, length)?)? {
                    length += 1;
                }
            }
        }
    } else {
        while length < depth && decoder.bit(model.unary(context, length)?)? {
            length += 1;
        }
    }
    if length == 0 {
        return Ok(0);
    }
    let mut value = 1_usize << (length - 1);
    for bit in (0..length - 1).rev() {
        if decoder.bit(model.mantissa(context, length, bit)?)? {
            value |= 1_usize << bit;
        }
    }
    Ok(value)
}

fn tile_geometry(width: usize, height: usize, tile_log: u8) -> Result<(usize, usize)> {
    let tile = 1_usize << tile_log;
    let across = width
        .checked_add(tile - 1)
        .ok_or(Error::ArithmeticOverflow("QST1 tile columns"))?
        >> tile_log;
    let down = height
        .checked_add(tile - 1)
        .ok_or(Error::ArithmeticOverflow("QST1 tile rows"))?
        >> tile_log;
    Ok((across, down))
}

fn decode_base_plane(
    decoder: &mut RangeDecoder<'_>,
    width: usize,
    height: usize,
    depth: usize,
    tile_log: u8,
) -> Result<Vec<u16>> {
    let pixels = width
        .checked_mul(height)
        .ok_or(Error::ArithmeticOverflow("QST1 base-plane pixels"))?;
    let mut plane = fallible_filled(pixels, 0_u16, "QST1 base plane")?;
    let mut model = ProbabilityModel::new()?;
    let (tiles_across, tiles_down) = tile_geometry(width, height, tile_log)?;
    let mut tile_predictors = if tile_log == 0 {
        Vec::new()
    } else {
        fallible_filled(
            tiles_across
                .checked_mul(tiles_down)
                .ok_or(Error::ArithmeticOverflow("QST1 predictor tile count"))?,
            0_u8,
            "QST1 predictor tiles",
        )?
    };
    for predictor in &mut tile_predictors {
        *predictor = model.tree3(decoder)? as u8;
    }
    let half = 1_i32 << (depth - 1);
    let maximum = (1_i32 << depth) - 1;
    for y in 0..height {
        let mut previous_bits = 0_usize;
        for x in 0..width {
            let position = y * width + x;
            let left = if x != 0 {
                i32::from(plane[position - 1])
            } else if y != 0 {
                i32::from(plane[position - width])
            } else {
                half
            };
            let up = if y != 0 {
                i32::from(plane[position - width])
            } else {
                left
            };
            let upper_left = if x != 0 && y != 0 {
                i32::from(plane[position - width - 1])
            } else {
                up
            };
            let upper_right = if y != 0 && x + 1 < width {
                i32::from(plane[position - width + 1])
            } else {
                up
            };
            let predictor = if tile_log == 0 {
                0
            } else {
                usize::from(tile_predictors[(y >> tile_log) * tiles_across + (x >> tile_log)])
            };
            let activity =
                (left - upper_left).abs() + (upper_left - up).abs() + (up - upper_right).abs();
            let context = error_context(activity_context(activity), previous_bits, 0, false);
            let magnitude = decode_unsigned_magnitude(decoder, &mut model, context, depth, false)?;
            let residual = if magnitude & 1 != 0 {
                -(((magnitude + 1) >> 1) as i32)
            } else {
                (magnitude >> 1) as i32
            };
            let prediction = predict_basic(predictor, left, up, upper_left, upper_right, maximum);
            plane[position] = ((prediction + residual) & maximum) as u16;
            previous_bits = nbits(magnitude as u32);
        }
    }
    Ok(plane)
}

fn decode_mode37_plane(
    decoder: &mut RangeDecoder<'_>,
    width: usize,
    height: usize,
    depth: usize,
    tile_log: u8,
) -> Result<Vec<u16>> {
    let pixels = width
        .checked_mul(height)
        .ok_or(Error::ArithmeticOverflow("QST1 mode-37 pixels"))?;
    let mut plane = fallible_filled(pixels, 0_u16, "QST1 mode-37 plane")?;
    let mut model = ProbabilityModel::new()?;
    let (tiles_across, tiles_down) = tile_geometry(width, height, tile_log)?;
    let mut tile_predictors = if tile_log == 0 {
        Vec::new()
    } else {
        fallible_filled(
            tiles_across
                .checked_mul(tiles_down)
                .ok_or(Error::ArithmeticOverflow("QST1 mode-37 tile count"))?,
            0_u8,
            "QST1 mode-37 predictor tiles",
        )?
    };
    for predictor in &mut tile_predictors {
        *predictor = model.tree5(decoder)? as u8;
    }
    let mut upper_bits = fallible_filled(width, 0_u8, "QST1 upper magnitude state")?;
    let mut upper_sign = fallible_filled(width, 0_i8, "QST1 upper sign state")?;
    let half = 1_i32 << (depth - 1);
    let maximum = (1_i32 << depth) - 1;
    for y in 0..height {
        let mut previous_bits = 0_usize;
        let mut previous_sign = 0_i32;
        for x in 0..width {
            let position = y * width + x;
            let left = if x != 0 {
                i32::from(plane[position - 1])
            } else if y != 0 {
                i32::from(plane[position - width])
            } else {
                half
            };
            let up = if y != 0 {
                i32::from(plane[position - width])
            } else {
                left
            };
            let upper_left = if x != 0 && y != 0 {
                i32::from(plane[position - width - 1])
            } else {
                up
            };
            let upper_right = if y != 0 && x + 1 < width {
                i32::from(plane[position - width + 1])
            } else {
                up
            };
            let second_left = if x > 1 {
                i32::from(plane[position - 2])
            } else {
                left
            };
            let second_up = if y > 1 {
                i32::from(plane[position - width * 2])
            } else {
                up
            };
            let predictor = if tile_log == 0 {
                0
            } else {
                usize::from(tile_predictors[(y >> tile_log) * tiles_across + (x >> tile_log)])
            };
            let mut activity = (left - upper_left).abs()
                + (upper_left - up).abs()
                + (up - upper_right).abs()
                + (((left - second_left).abs() + (up - second_up).abs()) >> 1);
            let up_bits = usize::from(upper_bits[x]);
            activity += if up_bits <= 2 { up_bits } else { 3 } as i32;
            let (context_bits, context_sign) = if up_bits > previous_bits {
                (up_bits, i32::from(upper_sign[x]))
            } else {
                (previous_bits, previous_sign)
            };
            let base_context =
                error_context(activity_context(activity), context_bits, context_sign, true);
            let context = base_context + predictor_context(predictor);
            let magnitude = decode_unsigned_magnitude(decoder, &mut model, context, depth, true)?;
            let prediction = if predictor == 6 {
                upper_right
            } else if predictor == 14 {
                clamp_sample((left + up + upper_right + upper_left + 2) >> 2, maximum)
            } else if predictor == 23 {
                clamp_sample((left + up + upper_right + 1) / 3, maximum)
            } else {
                predict_adaptive(
                    predictor,
                    left,
                    up,
                    upper_left,
                    upper_right,
                    second_left,
                    second_up,
                    maximum,
                )
            };
            let residual = if magnitude == 0 {
                0
            } else {
                let hint = sign_hint(
                    left,
                    up,
                    upper_left,
                    upper_right,
                    second_left,
                    second_up,
                    prediction,
                    maximum,
                );
                let mut negative = decoder.bit(model.sign(context, hint)?)?;
                if context_sign != 0 {
                    negative ^= context_sign < 0;
                }
                negative ^= hint < 0;
                if negative {
                    -(magnitude as i32)
                } else {
                    magnitude as i32
                }
            };
            plane[position] = ((prediction + residual) & maximum) as u16;
            previous_bits = nbits(magnitude as u32);
            previous_sign = residual.signum();
            upper_bits[x] = previous_bits.min(15) as u8;
            upper_sign[x] = previous_sign as i8;
        }
    }
    Ok(plane)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ChannelStateFlow {
    Independent,
    First,
    Middle,
    Last,
}

fn mode52_unary_index(context: usize, bit: usize) -> Result<usize> {
    context
        .checked_mul(MAX_MAGNITUDE_BITS + 1)
        .and_then(|base| base.checked_add(bit))
        .ok_or(Error::ArithmeticOverflow("QST1 mode-52 unary context"))
}

fn mode52_mantissa_index(context: usize, length: usize, bit: usize) -> Result<usize> {
    context
        .checked_mul(MAX_MAGNITUDE_BITS + 1)
        .and_then(|base| base.checked_add(length))
        .and_then(|base| base.checked_mul(MAX_MAGNITUDE_BITS))
        .and_then(|base| base.checked_add(bit))
        .ok_or(Error::ArithmeticOverflow("QST1 mode-52 mantissa context"))
}

fn probability<'a>(
    probabilities: &'a mut [u16],
    index: usize,
    message: &'static str,
) -> Result<&'a mut u16> {
    probabilities.get_mut(index).ok_or_else(|| invalid(message))
}

fn local_zero_context(left: usize, up: usize, left_sign: i32, up_sign: i32) -> usize {
    if left == 0 {
        usize::from(up != 0) * 2
    } else if up == 0 {
        1
    } else if left_sign == up_sign {
        3
    } else {
        4
    }
}

fn channel_state(activity: i32, residual: i32, length: usize) -> u8 {
    let residual_class = if residual > 0 {
        if length <= 2 { 1 } else { 2 }
    } else if residual < 0 {
        if length <= 2 { 3 } else { 4 }
    } else {
        0
    };
    (if activity > 0 { 5 } else { 0 }) + residual_class
}

fn channel_zero_state(state: u8) -> Result<usize> {
    const LOOKUP: [usize; 10] = [0, 1, 1, 2, 2, 3, 4, 4, 5, 5];
    LOOKUP
        .get(usize::from(state))
        .copied()
        .ok_or_else(|| invalid("mode-52 channel state is out of range"))
}

fn channel_magnitude_state(state: u8) -> Result<usize> {
    const LOOKUP: [usize; 10] = [0, 1, 2, 1, 2, 0, 1, 2, 1, 2];
    LOOKUP
        .get(usize::from(state))
        .copied()
        .ok_or_else(|| invalid("mode-52 channel state is out of range"))
}

fn channel_pair_states(state: u8) -> Result<(usize, usize)> {
    let high = state >> 4;
    let low = state & 15;
    Ok((
        channel_zero_state(high)? * 6 + channel_zero_state(low)?,
        channel_magnitude_state(high)? * 3 + channel_magnitude_state(low)?,
    ))
}

fn decode_reused_predictor_tiles(
    decoder: &mut RangeDecoder<'_>,
    model: &mut ProbabilityModel,
    width: usize,
    height: usize,
    tile_log: u8,
    context: &'static str,
) -> Result<(Vec<u8>, usize)> {
    let (tiles_across, tiles_down) = tile_geometry(width, height, tile_log)?;
    let tile_count = if tile_log == 0 {
        0
    } else {
        tiles_across
            .checked_mul(tiles_down)
            .ok_or(Error::ArithmeticOverflow(context))?
    };
    let mut tile_predictors = fallible_filled(tile_count, 0_u8, context)?;
    let mut same = [[PROB_INIT; 4]; 2];
    for tile in 0..tile_count {
        let tile_x = tile % tiles_across;
        let mut predictor = None;
        if tile_x != 0 {
            let reference = usize::from(tile_predictors[tile - 1]);
            let group = predictor_context(reference) / BASE_CONTEXTS;
            if !decoder.bit(&mut same[0][group])? {
                predictor = Some(reference);
            }
        }
        if predictor.is_none()
            && tile >= tiles_across
            && (tile_x == 0 || tile_predictors[tile - tiles_across] != tile_predictors[tile - 1])
        {
            let reference = usize::from(tile_predictors[tile - tiles_across]);
            let group = predictor_context(reference) / BASE_CONTEXTS;
            if !decoder.bit(&mut same[1][group])? {
                predictor = Some(reference);
            }
        }
        tile_predictors[tile] = match predictor {
            Some(predictor) => predictor as u8,
            None => model.tree5(decoder)? as u8,
        };
    }
    Ok((tile_predictors, tiles_across))
}

const fn weighted_division_table() -> [u32; 64] {
    let mut table = [0_u32; 64];
    let mut index = 0_usize;
    while index < table.len() {
        table[index] = (1_u32 << 24) / (index as u32 + 1);
        index += 1;
    }
    table
}

const WEIGHTED_DIVISION: [u32; 64] = weighted_division_table();

struct WeightedPredictor {
    prediction: [i32; 4],
    combined: i32,
    prediction_error: [Vec<u32>; 4],
    error: Vec<i32>,
    row_size: usize,
}

impl WeightedPredictor {
    fn new(width: usize) -> Result<Self> {
        let row_size = width
            .checked_add(2)
            .ok_or(Error::ArithmeticOverflow("QST1 weighted row size"))?;
        let cells = row_size
            .checked_mul(2)
            .ok_or(Error::ArithmeticOverflow("QST1 weighted row cells"))?;
        Ok(Self {
            prediction: [0; 4],
            combined: 0,
            prediction_error: [
                fallible_filled(cells, 0_u32, "QST1 weighted predictor error row")?,
                fallible_filled(cells, 0_u32, "QST1 weighted predictor error row")?,
                fallible_filled(cells, 0_u32, "QST1 weighted predictor error row")?,
                fallible_filled(cells, 0_u32, "QST1 weighted predictor error row")?,
            ],
            error: fallible_filled(cells, 0_i32, "QST1 weighted combined-error row")?,
            row_size,
        })
    }

    fn error_weight(error: u64, maximum: u32) -> Result<u32> {
        let truncated = error
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow("QST1 weighted error"))?
            as u32;
        let shift = nbits(truncated).saturating_sub(6);
        let reduced = usize::try_from(error >> shift)
            .map_err(|_| Error::ArithmeticOverflow("QST1 weighted error index"))?;
        let division = *WEIGHTED_DIVISION
            .get(reduced)
            .ok_or_else(|| invalid("weighted error index is out of range"))?;
        Ok(4 + ((maximum * division) >> shift))
    }

    fn average(prediction: &[i32; 4], mut weight: [u32; 4]) -> Result<i32> {
        let weight_sum = weight.iter().try_fold(0_u32, |sum, &value| {
            sum.checked_add(value)
                .ok_or(Error::ArithmeticOverflow("QST1 weighted sum"))
        })?;
        let shift = nbits(weight_sum)
            .checked_sub(5)
            .ok_or_else(|| invalid("weighted probability sum is too small"))?;
        let mut reduced_sum = 0_u32;
        for value in &mut weight {
            *value >>= shift;
            reduced_sum = reduced_sum
                .checked_add(*value)
                .ok_or(Error::ArithmeticOverflow("QST1 reduced weighted sum"))?;
        }
        let division_index = usize::try_from(
            reduced_sum
                .checked_sub(1)
                .ok_or_else(|| invalid("weighted probability sum is zero"))?,
        )
        .map_err(|_| Error::ArithmeticOverflow("QST1 weighted division index"))?;
        let division = i64::from(
            *WEIGHTED_DIVISION
                .get(division_index)
                .ok_or_else(|| invalid("weighted division index is out of range"))?,
        );
        let mut sum = i64::from(reduced_sum >> 1) - 1;
        for (&sample, &sample_weight) in prediction.iter().zip(&weight) {
            sum = sum
                .checked_add(i64::from(sample) * i64::from(sample_weight))
                .ok_or(Error::ArithmeticOverflow("QST1 weighted prediction"))?;
        }
        i32::try_from(
            sum.checked_mul(division)
                .ok_or(Error::ArithmeticOverflow("QST1 weighted prediction"))?
                >> 24,
        )
        .map_err(|_| Error::ArithmeticOverflow("QST1 weighted prediction"))
    }

    #[allow(clippy::too_many_arguments)]
    fn predict(
        &mut self,
        x: usize,
        y: usize,
        width: usize,
        up: i32,
        left: i32,
        upper_right: i32,
        upper_left: i32,
        second_up: i32,
    ) -> Result<i32> {
        const BASE_WEIGHT: [u32; 4] = [13, 12, 12, 11];

        let (current, previous) = if y & 1 != 0 {
            (0, self.row_size)
        } else {
            (self.row_size, 0)
        };
        let north = previous
            .checked_add(x)
            .ok_or(Error::ArithmeticOverflow("QST1 weighted north index"))?;
        let northeast = if x + 1 < width { north + 1 } else { north };
        let northwest = if x != 0 { north - 1 } else { north };
        let mut weight = [0_u32; 4];
        for predictor in 0..4 {
            let errors = self
                .prediction_error
                .get(predictor)
                .ok_or_else(|| invalid("weighted predictor is out of range"))?;
            let error = u64::from(errors[north])
                + u64::from(errors[northeast])
                + u64::from(errors[northwest]);
            weight[predictor] = Self::error_weight(error, BASE_WEIGHT[predictor])?;
        }

        let up = up << 3;
        let left = left << 3;
        let upper_right = upper_right << 3;
        let upper_left = upper_left << 3;
        let second_up = second_up << 3;
        let west_error = if x != 0 {
            self.error[current + x - 1]
        } else {
            0
        };
        let north_error = self.error[north];
        let northwest_error = self.error[northwest];
        let northeast_error = self.error[northeast];
        let west_north_error = west_error + north_error;
        self.prediction[0] = left + upper_right - up;
        self.prediction[1] = up - ((west_north_error + northeast_error) >> 2);
        self.prediction[2] = left - ((west_north_error + northwest_error) >> 2);
        self.prediction[3] = up
            - ((4 * northwest_error
                + 3 * northeast_error
                + 23 * (second_up - up)
                + 2 * (upper_left - left))
                >> 5);
        self.combined = Self::average(&self.prediction, weight)?;
        if ((north_error ^ west_error) | (north_error ^ northwest_error)) <= 0 {
            self.combined = self
                .combined
                .clamp(left.min(up).min(upper_right), left.max(up).max(upper_right));
        }
        Ok((self.combined + 3) >> 3)
    }

    fn update(&mut self, x: usize, y: usize, value: u16) -> Result<()> {
        let (current, previous) = if y & 1 != 0 {
            (0, self.row_size)
        } else {
            (self.row_size, 0)
        };
        let scaled_value = i32::from(value) << 3;
        self.error[current + x] = self.combined - scaled_value;
        for predictor in 0..4 {
            let difference = i64::from(self.prediction[predictor]) - i64::from(scaled_value);
            let error = u32::try_from((difference.abs() + 3) >> 3)
                .map_err(|_| Error::ArithmeticOverflow("QST1 weighted predictor error"))?;
            self.prediction_error[predictor][current + x] = error;
            let propagated = &mut self.prediction_error[predictor][previous + x + 1];
            *propagated = propagated
                .checked_add(error)
                .ok_or(Error::ArithmeticOverflow("QST1 weighted propagated error"))?;
        }
        Ok(())
    }
}

#[allow(clippy::too_many_arguments)]
fn decode_local_root_plane(
    decoder: &mut RangeDecoder<'_>,
    width: usize,
    height: usize,
    depth: usize,
    tile_log: u8,
    refined_sign: bool,
    weighted_prediction: bool,
    flow: ChannelStateFlow,
    mut channel_state_buffer: Option<&mut [u8]>,
) -> Result<Vec<u16>> {
    let pixels = width
        .checked_mul(height)
        .ok_or(Error::ArithmeticOverflow("QST1 mode-52 pixels"))?;
    let has_input = matches!(flow, ChannelStateFlow::Middle | ChannelStateFlow::Last);
    let has_output = matches!(flow, ChannelStateFlow::First | ChannelStateFlow::Middle);
    let magnitude_slow = if refined_sign { 2 } else { 1 };
    let sign_slow = if refined_sign { 2 } else { 1 };
    if matches!(flow, ChannelStateFlow::Independent) && channel_state_buffer.is_some()
        || !matches!(flow, ChannelStateFlow::Independent)
            && channel_state_buffer
                .as_ref()
                .is_none_or(|state| state.len() != pixels)
    {
        return Err(invalid("invalid mode-52 channel-state buffer"));
    }

    let mut plane = fallible_filled(pixels, 0_u16, "QST1 mode-52 plane")?;
    let mut model = Mode52Model::new()?;
    let (tile_predictors, tiles_across) = decode_reused_predictor_tiles(
        decoder,
        &mut model.fine,
        width,
        height,
        tile_log,
        "QST1 mode-52 predictor tiles",
    )?;

    let context_stride = if tile_log == 0 {
        BASE_CONTEXTS
    } else {
        PREDICTOR_CONTEXTS
    };
    let cross_states = if has_input {
        5 * if has_output { 6 } else { 36 }
    } else {
        5
    };
    let sign_base_states = if has_input {
        if has_output { 30 } else { 180 }
    } else {
        0
    };
    let magnitude_states = if has_input {
        if has_output { 3 } else { 9 }
    } else {
        0
    };
    let mut cross_zero = fallible_filled(
        cross_states * context_stride,
        0_u16,
        "QST1 mode-52 local zero model",
    )?;
    let mut cross_sign = fallible_filled(
        sign_base_states * 3 * context_stride,
        0_u16,
        "QST1 mode-52 cross-channel sign model",
    )?;
    let mut cross_magnitude = fallible_filled(
        magnitude_states * context_stride,
        0_u16,
        "QST1 mode-52 cross-channel magnitude model",
    )?;
    let mut upper_bits = fallible_filled(width, 0_u8, "QST1 mode-52 upper lengths")?;
    let mut upper_sign = fallible_filled(width, 0_i8, "QST1 mode-52 upper signs")?;
    let mut weighted = if weighted_prediction {
        Some(WeightedPredictor::new(width)?)
    } else {
        None
    };
    let half = 1_i32 << (depth - 1);
    let maximum = (1_i32 << depth) - 1;

    for y in 0..height {
        let mut previous_bits = 0_usize;
        let mut previous_sign = 0_i32;
        for x in 0..width {
            let position = y * width + x;
            let left = if x != 0 {
                i32::from(plane[position - 1])
            } else if y != 0 {
                i32::from(plane[position - width])
            } else {
                half
            };
            let up = if y != 0 {
                i32::from(plane[position - width])
            } else {
                left
            };
            let upper_left = if x != 0 && y != 0 {
                i32::from(plane[position - width - 1])
            } else {
                up
            };
            let upper_right = if y != 0 && x + 1 < width {
                i32::from(plane[position - width + 1])
            } else {
                up
            };
            let second_left = if x > 1 {
                i32::from(plane[position - 2])
            } else {
                left
            };
            let second_up = if y > 1 {
                i32::from(plane[position - width * 2])
            } else {
                up
            };
            let predictor = if tile_log == 0 {
                0
            } else {
                usize::from(tile_predictors[(y >> tile_log) * tiles_across + (x >> tile_log)])
            };
            let mut activity = (left - upper_left).abs()
                + (upper_left - up).abs()
                + (up - upper_right).abs()
                + (((left - second_left).abs() + (up - second_up).abs()) >> 1);
            let up_bits = usize::from(upper_bits[x]);
            let up_sign = i32::from(upper_sign[x]);
            activity += if up_bits <= 2 { up_bits } else { 3 } as i32;
            let (context_bits, context_sign) = if up_bits > previous_bits {
                (up_bits, up_sign)
            } else {
                (previous_bits, previous_sign)
            };
            let activity_class = activity_context(activity);
            let base_context = error_context(activity_class, context_bits, context_sign, true);
            let context = base_context + predictor_context(predictor);
            let local_state = local_zero_context(previous_bits, up_bits, previous_sign, up_sign);
            let stored_state = if has_input {
                *channel_state_buffer
                    .as_deref()
                    .and_then(|state| state.get(position))
                    .ok_or_else(|| invalid("mode-52 channel state is truncated"))?
            } else {
                0
            };
            let (cross_zero_state, cross_magnitude_state) = if has_input {
                if has_output {
                    (
                        channel_zero_state(stored_state)?,
                        channel_magnitude_state(stored_state)?,
                    )
                } else {
                    channel_pair_states(stored_state)?
                }
            } else {
                (0, 0)
            };
            let zero_state = local_state + 5 * cross_zero_state;
            let zero_index = zero_state * context_stride + context;
            let fine_zero_index = mode52_unary_index(context, 0)?;
            let fine_zero_value = *model
                .fine
                .unary
                .get(fine_zero_index)
                .ok_or_else(|| invalid("mode-52 fine zero context is out of range"))?;
            let child_zero = probability(
                &mut cross_zero,
                zero_index,
                "mode-52 local zero context is out of range",
            )?;
            if *child_zero == 0 {
                *child_zero = fine_zero_value;
            }
            let nonzero = decode_child_root_bit(
                decoder,
                child_zero,
                RootProbabilities {
                    fine: probability(
                        &mut model.fine.unary,
                        fine_zero_index,
                        "mode-52 fine zero context is out of range",
                    )?,
                    coarse: probability(
                        &mut model.coarse.zero,
                        base_context,
                        "mode-52 coarse zero context is out of range",
                    )?,
                    root: probability(
                        &mut model.root.zero,
                        activity_class,
                        "mode-52 root zero context is out of range",
                    )?,
                },
                ChildRootMix {
                    slow: 0,
                    weight: 5,
                    child_rate: 0,
                },
            )?;

            let mut length = 0_usize;
            if nonzero {
                length = 1;
                let fine_index = mode52_unary_index(context, 1)?;
                let more = if has_input {
                    let cross_index = cross_magnitude_state * context_stride + context;
                    let fine_value =
                        *model.fine.unary.get(fine_index).ok_or_else(|| {
                            invalid("mode-52 fine magnitude context is out of range")
                        })?;
                    let child = probability(
                        &mut cross_magnitude,
                        cross_index,
                        "mode-52 cross magnitude context is out of range",
                    )?;
                    if *child == 0 {
                        *child = fine_value;
                    }
                    decode_child_root_bit(
                        decoder,
                        child,
                        RootProbabilities {
                            fine: probability(
                                &mut model.fine.unary,
                                fine_index,
                                "mode-52 fine magnitude context is out of range",
                            )?,
                            coarse: probability(
                                &mut model.coarse.magnitude,
                                base_context,
                                "mode-52 coarse magnitude context is out of range",
                            )?,
                            root: probability(
                                &mut model.root.magnitude,
                                activity_class,
                                "mode-52 root magnitude context is out of range",
                            )?,
                        },
                        ChildRootMix {
                            slow: magnitude_slow,
                            weight: if refined_sign { 4 } else { 5 },
                            child_rate: if refined_sign { -1 } else { 0 },
                        },
                    )?
                } else {
                    decode_root_bit(
                        decoder,
                        probability(
                            &mut model.fine.unary,
                            fine_index,
                            "mode-52 fine magnitude context is out of range",
                        )?,
                        probability(
                            &mut model.coarse.magnitude,
                            base_context,
                            "mode-52 coarse magnitude context is out of range",
                        )?,
                        probability(
                            &mut model.root.magnitude,
                            activity_class,
                            "mode-52 root magnitude context is out of range",
                        )?,
                        magnitude_slow,
                    )?
                };
                if more {
                    length = 2;
                    while length < depth {
                        let fine_index = mode52_unary_index(context, length)?;
                        let coarse_index = mode52_unary_index(base_context, length)?;
                        let root_index = mode52_unary_index(activity_class, length)?;
                        if !decode_root_bit(
                            decoder,
                            probability(
                                &mut model.fine.unary,
                                fine_index,
                                "mode-52 fine unary context is out of range",
                            )?,
                            probability(
                                &mut model.coarse.unary,
                                coarse_index,
                                "mode-52 coarse unary context is out of range",
                            )?,
                            probability(
                                &mut model.root.unary,
                                root_index,
                                "mode-52 root unary context is out of range",
                            )?,
                            0,
                        )? {
                            break;
                        }
                        length += 1;
                    }
                }
            }

            let mut magnitude = if length == 0 {
                0_usize
            } else {
                1_usize << (length - 1)
            };
            if length != 0 {
                for bit in (0..length - 1).rev() {
                    let fine_index = mode52_mantissa_index(context, length, bit)?;
                    let coarse_index = mode52_mantissa_index(base_context, length, bit)?;
                    let root_index = mode52_mantissa_index(activity_class, length, bit)?;
                    if decode_root_bit(
                        decoder,
                        probability(
                            &mut model.fine.mantissa,
                            fine_index,
                            "mode-52 fine mantissa context is out of range",
                        )?,
                        probability(
                            &mut model.coarse.mantissa,
                            coarse_index,
                            "mode-52 coarse mantissa context is out of range",
                        )?,
                        probability(
                            &mut model.root.mantissa,
                            root_index,
                            "mode-52 root mantissa context is out of range",
                        )?,
                        1,
                    )? {
                        magnitude |= 1_usize << bit;
                    }
                }
            }

            let weighted_value = match weighted.as_mut() {
                Some(state) => Some(state.predict(
                    x,
                    y,
                    width,
                    up,
                    left,
                    upper_right,
                    upper_left,
                    second_up,
                )?),
                None => None,
            };
            let prediction = if weighted_prediction && predictor == 31 {
                weighted_value.ok_or_else(|| invalid("weighted predictor is unavailable"))?
            } else if predictor == 6 {
                upper_right
            } else if predictor == 14 {
                clamp_sample((left + up + upper_right + upper_left + 2) >> 2, maximum)
            } else if predictor == 23 {
                clamp_sample((left + up + upper_right + 1) / 3, maximum)
            } else {
                predict_adaptive(
                    predictor,
                    left,
                    up,
                    upper_left,
                    upper_right,
                    second_left,
                    second_up,
                    maximum,
                )
            };
            let residual = if magnitude == 0 {
                0
            } else {
                let hint = sign_hint(
                    left,
                    up,
                    upper_left,
                    upper_right,
                    second_left,
                    second_up,
                    prediction,
                    maximum,
                );
                let length_class = if length <= 1 {
                    0
                } else if length <= 3 {
                    1
                } else {
                    2
                };
                let exact_base = (predictor * BASE_CONTEXTS + base_context) * 3 + length_class;
                let fine_index = if hint == 0 {
                    context
                } else {
                    context * 2 + usize::from(hint < 0)
                };
                let coarse_index = if hint == 0 {
                    base_context
                } else {
                    base_context * 2 + usize::from(hint < 0)
                };
                let root_index = if hint == 0 {
                    activity_class
                } else {
                    activity_class * 2 + usize::from(hint < 0)
                };
                let exact_index = if hint == 0 {
                    exact_base
                } else {
                    exact_base * 2 + usize::from(hint < 0)
                };
                let mut negative = if has_input {
                    let sign_base = cross_zero_state * 5 + local_state;
                    let hint_class = if hint < 0 { 2 } else { usize::from(hint > 0) };
                    let cross_index =
                        (sign_base + sign_base_states * hint_class) * context_stride + context;
                    if hint == 0 {
                        let parent_value =
                            *model.fine.nonzero_sign.get(fine_index).ok_or_else(|| {
                                invalid("mode-52 fine sign context is out of range")
                            })?;
                        let child = probability(
                            &mut cross_sign,
                            cross_index,
                            "mode-52 cross sign context is out of range",
                        )?;
                        if *child == 0 {
                            *child = parent_value;
                        }
                        decode_cross_exact_sign(
                            decoder,
                            child,
                            probability(
                                &mut model.exact.nonzero_sign,
                                exact_index,
                                "mode-52 exact sign context is out of range",
                            )?,
                            RootProbabilities {
                                fine: probability(
                                    &mut model.fine.nonzero_sign,
                                    fine_index,
                                    "mode-52 fine sign context is out of range",
                                )?,
                                coarse: probability(
                                    &mut model.coarse.nonzero_sign,
                                    coarse_index,
                                    "mode-52 coarse sign context is out of range",
                                )?,
                                root: probability(
                                    &mut model.root.nonzero_sign,
                                    root_index,
                                    "mode-52 root sign context is out of range",
                                )?,
                            },
                            CrossExactMix {
                                slow: sign_slow,
                                weight: if refined_sign {
                                    [4, 8, 12][length_class]
                                } else {
                                    12
                                },
                                child_rate: if refined_sign {
                                    [1, 2, 3][length_class]
                                } else {
                                    1
                                },
                                exact_rate: if refined_sign {
                                    [0, 2, 2][length_class]
                                } else {
                                    2
                                },
                            },
                        )?
                    } else {
                        let parent_value =
                            *model.fine.signed_hint.get(fine_index).ok_or_else(|| {
                                invalid("mode-52 hinted sign context is out of range")
                            })?;
                        let child = probability(
                            &mut cross_sign,
                            cross_index,
                            "mode-52 cross hinted-sign context is out of range",
                        )?;
                        if *child == 0 {
                            *child = parent_value;
                        }
                        decode_cross_exact_sign(
                            decoder,
                            child,
                            probability(
                                &mut model.exact.signed_hint,
                                exact_index,
                                "mode-52 exact hinted-sign context is out of range",
                            )?,
                            RootProbabilities {
                                fine: probability(
                                    &mut model.fine.signed_hint,
                                    fine_index,
                                    "mode-52 hinted sign context is out of range",
                                )?,
                                coarse: probability(
                                    &mut model.coarse.signed_hint,
                                    coarse_index,
                                    "mode-52 coarse hinted-sign context is out of range",
                                )?,
                                root: probability(
                                    &mut model.root.signed_hint,
                                    root_index,
                                    "mode-52 root hinted-sign context is out of range",
                                )?,
                            },
                            CrossExactMix {
                                slow: sign_slow,
                                weight: if refined_sign {
                                    [4, 8, 12][length_class]
                                } else {
                                    12
                                },
                                child_rate: if refined_sign {
                                    [1, 2, 3][length_class]
                                } else {
                                    1
                                },
                                exact_rate: if refined_sign {
                                    [0, 2, 2][length_class]
                                } else {
                                    2
                                },
                            },
                        )?
                    }
                } else if hint == 0 {
                    decode_exact_root_sign(
                        decoder,
                        probability(
                            &mut model.exact.nonzero_sign,
                            exact_index,
                            "mode-52 exact sign context is out of range",
                        )?,
                        probability(
                            &mut model.fine.nonzero_sign,
                            fine_index,
                            "mode-52 fine sign context is out of range",
                        )?,
                        probability(
                            &mut model.coarse.nonzero_sign,
                            coarse_index,
                            "mode-52 coarse sign context is out of range",
                        )?,
                        probability(
                            &mut model.root.nonzero_sign,
                            root_index,
                            "mode-52 root sign context is out of range",
                        )?,
                        sign_slow,
                        if refined_sign {
                            [-1, 0, 1][length_class]
                        } else {
                            -1
                        },
                    )?
                } else {
                    decode_exact_root_sign(
                        decoder,
                        probability(
                            &mut model.exact.signed_hint,
                            exact_index,
                            "mode-52 exact hinted-sign context is out of range",
                        )?,
                        probability(
                            &mut model.fine.signed_hint,
                            fine_index,
                            "mode-52 hinted sign context is out of range",
                        )?,
                        probability(
                            &mut model.coarse.signed_hint,
                            coarse_index,
                            "mode-52 coarse hinted-sign context is out of range",
                        )?,
                        probability(
                            &mut model.root.signed_hint,
                            root_index,
                            "mode-52 root hinted-sign context is out of range",
                        )?,
                        sign_slow,
                        if refined_sign {
                            [-1, 0, 1][length_class]
                        } else {
                            -1
                        },
                    )?
                };
                if context_sign != 0 {
                    negative ^= context_sign < 0;
                }
                negative ^= hint < 0;
                if negative {
                    -(magnitude as i32)
                } else {
                    magnitude as i32
                }
            };
            plane[position] = ((prediction + residual) & maximum) as u16;
            if let Some(state) = weighted.as_mut() {
                state.update(x, y, plane[position])?;
            }
            previous_bits = length;
            previous_sign = residual.signum();
            upper_bits[x] = length.min(15) as u8;
            upper_sign[x] = previous_sign as i8;
            if has_output {
                let next = channel_state(activity, residual, length);
                let stored = if matches!(flow, ChannelStateFlow::Middle) {
                    (stored_state << 4) | next
                } else {
                    next
                };
                *channel_state_buffer
                    .as_deref_mut()
                    .and_then(|state| state.get_mut(position))
                    .ok_or_else(|| invalid("mode-52 channel state is truncated"))? = stored;
            }
        }
    }
    Ok(plane)
}

#[allow(clippy::too_many_arguments)]
fn decode_mode45_plane(
    decoder: &mut RangeDecoder<'_>,
    width: usize,
    height: usize,
    depth: usize,
    tile_log: u8,
    flow: ChannelStateFlow,
    mut channel_state_buffer: Option<&mut [u8]>,
) -> Result<Vec<u16>> {
    let pixels = width
        .checked_mul(height)
        .ok_or(Error::ArithmeticOverflow("QST1 mode-45 pixels"))?;
    let has_input = matches!(flow, ChannelStateFlow::Middle | ChannelStateFlow::Last);
    let has_output = matches!(flow, ChannelStateFlow::First | ChannelStateFlow::Middle);
    if matches!(flow, ChannelStateFlow::Independent) && channel_state_buffer.is_some()
        || !matches!(flow, ChannelStateFlow::Independent)
            && channel_state_buffer
                .as_ref()
                .is_none_or(|state| state.len() != pixels)
    {
        return Err(invalid("invalid mode-45 channel-state buffer"));
    }

    let mut plane = fallible_filled(pixels, 0_u16, "QST1 mode-45 plane")?;
    let mut model = ProbabilityModel::new()?;
    let (tile_predictors, tiles_across) = decode_reused_predictor_tiles(
        decoder,
        &mut model,
        width,
        height,
        tile_log,
        "QST1 mode-45 predictor tiles",
    )?;
    let context_stride = if tile_log == 0 {
        BASE_CONTEXTS
    } else {
        PREDICTOR_CONTEXTS
    };
    let cross_states = if has_input {
        135 * if has_output { 6 } else { 36 }
    } else {
        135
    };
    let sign_base_states = if has_input {
        if has_output { 30 } else { 180 }
    } else {
        0
    };
    let magnitude_states = if has_input {
        if has_output { 3 } else { 9 }
    } else {
        0
    };
    let mut cross_zero = fallible_filled(
        cross_states * context_stride,
        0_u16,
        "QST1 mode-45 spatial zero model",
    )?;
    let mut cross_sign = fallible_filled(
        sign_base_states * 3 * context_stride,
        0_u16,
        "QST1 mode-45 cross-channel sign model",
    )?;
    let mut cross_magnitude = fallible_filled(
        magnitude_states * context_stride,
        0_u16,
        "QST1 mode-45 cross-channel magnitude model",
    )?;
    let mut upper_bits = fallible_filled(width, 0_u8, "QST1 mode-45 upper lengths")?;
    let mut upper_sign = fallible_filled(width, 0_i8, "QST1 mode-45 upper signs")?;
    let half = 1_i32 << (depth - 1);
    let maximum = (1_i32 << depth) - 1;

    for y in 0..height {
        let mut previous_bits = 0_usize;
        let mut previous_sign = 0_i32;
        let mut previous_upper_bits = 0_usize;
        let mut previous_upper_sign = 0_i32;
        let mut west_west_bits = 0_usize;
        let mut west_west_sign = 0_i32;
        for x in 0..width {
            let position = y * width + x;
            let left = if x != 0 {
                i32::from(plane[position - 1])
            } else if y != 0 {
                i32::from(plane[position - width])
            } else {
                half
            };
            let up = if y != 0 {
                i32::from(plane[position - width])
            } else {
                left
            };
            let upper_left = if x != 0 && y != 0 {
                i32::from(plane[position - width - 1])
            } else {
                up
            };
            let upper_right = if y != 0 && x + 1 < width {
                i32::from(plane[position - width + 1])
            } else {
                up
            };
            let second_left = if x > 1 {
                i32::from(plane[position - 2])
            } else {
                left
            };
            let second_up = if y > 1 {
                i32::from(plane[position - width * 2])
            } else {
                up
            };
            let predictor = if tile_log == 0 {
                0
            } else {
                usize::from(tile_predictors[(y >> tile_log) * tiles_across + (x >> tile_log)])
            };
            let mut activity = (left - upper_left).abs()
                + (upper_left - up).abs()
                + (up - upper_right).abs()
                + (((left - second_left).abs() + (up - second_up).abs()) >> 1);
            let up_bits = usize::from(upper_bits[x]);
            let up_sign = i32::from(upper_sign[x]);
            activity += if up_bits <= 2 { up_bits } else { 3 } as i32;
            let (context_bits, context_sign) = if up_bits > previous_bits {
                (up_bits, up_sign)
            } else {
                (previous_bits, previous_sign)
            };
            let context =
                error_context(activity_context(activity), context_bits, context_sign, true)
                    + predictor_context(predictor);
            let local_state = local_zero_context(previous_bits, up_bits, previous_sign, up_sign);
            let northeast_bits = if x + 1 < width {
                usize::from(upper_bits[x + 1])
            } else {
                0
            };
            let northeast_sign = if x + 1 < width {
                i32::from(upper_sign[x + 1])
            } else {
                0
            };
            let northeast_reference = if up_bits != 0 { up_sign } else { previous_sign };
            let northeast_state = if northeast_bits != 0 {
                if (up_bits == 0 && previous_bits == 0) || northeast_sign == northeast_reference {
                    1
                } else {
                    2
                }
            } else {
                0
            };
            let northwest_reference = if up_bits != 0 { up_sign } else { previous_sign };
            let northwest_state = if previous_upper_bits != 0 {
                if (up_bits == 0 && previous_bits == 0)
                    || previous_upper_sign == northwest_reference
                {
                    1
                } else {
                    2
                }
            } else {
                0
            };
            let west_west_state = if west_west_bits != 0 {
                if previous_bits == 0 || west_west_sign == previous_sign {
                    1
                } else {
                    2
                }
            } else {
                0
            };
            let stored_state = if has_input {
                *channel_state_buffer
                    .as_deref()
                    .and_then(|state| state.get(position))
                    .ok_or_else(|| invalid("mode-45 channel state is truncated"))?
            } else {
                0
            };
            let (cross_zero_state, cross_magnitude_state) = if has_input {
                if has_output {
                    (
                        channel_zero_state(stored_state)?,
                        channel_magnitude_state(stored_state)?,
                    )
                } else {
                    channel_pair_states(stored_state)?
                }
            } else {
                (0, 0)
            };
            let spatial_state =
                local_state + 5 * northeast_state + 15 * northwest_state + 45 * west_west_state;
            let zero_state = spatial_state + 135 * cross_zero_state;
            let zero_index = zero_state * context_stride + context;
            let fine_zero_index = mode52_unary_index(context, 0)?;
            let fine_zero_value = *model
                .unary
                .get(fine_zero_index)
                .ok_or_else(|| invalid("mode-45 fine zero context is out of range"))?;
            let child_zero = probability(
                &mut cross_zero,
                zero_index,
                "mode-45 spatial zero context is out of range",
            )?;
            if *child_zero == 0 {
                *child_zero = fine_zero_value;
            }
            let nonzero = decoder.bit(child_zero)?;
            update_probability(
                probability(
                    &mut model.unary,
                    fine_zero_index,
                    "mode-45 fine zero context is out of range",
                )?,
                nonzero,
                decoder.adaptation,
            );

            let mut length = 0_usize;
            if nonzero {
                length = 1;
                let fine_index = mode52_unary_index(context, 1)?;
                let more = if has_input {
                    let cross_index = cross_magnitude_state * context_stride + context;
                    let fine_value = *model
                        .unary
                        .get(fine_index)
                        .ok_or_else(|| invalid("mode-45 fine magnitude context is out of range"))?;
                    let child = probability(
                        &mut cross_magnitude,
                        cross_index,
                        "mode-45 cross magnitude context is out of range",
                    )?;
                    if *child == 0 {
                        *child = fine_value;
                    }
                    let more = decoder.bit(child)?;
                    update_probability(
                        probability(
                            &mut model.unary,
                            fine_index,
                            "mode-45 fine magnitude context is out of range",
                        )?,
                        more,
                        decoder.adaptation,
                    );
                    more
                } else {
                    decoder.bit(probability(
                        &mut model.unary,
                        fine_index,
                        "mode-45 fine magnitude context is out of range",
                    )?)?
                };
                if more {
                    length = 2;
                    while length < depth
                        && decoder.bit(probability(
                            &mut model.unary,
                            mode52_unary_index(context, length)?,
                            "mode-45 unary context is out of range",
                        )?)?
                    {
                        length += 1;
                    }
                }
            }
            let mut magnitude = if length == 0 {
                0_usize
            } else {
                1_usize << (length - 1)
            };
            for bit in (0..length.saturating_sub(1)).rev() {
                if decoder.bit(probability(
                    &mut model.mantissa,
                    mode52_mantissa_index(context, length, bit)?,
                    "mode-45 mantissa context is out of range",
                )?)? {
                    magnitude |= 1_usize << bit;
                }
            }
            let prediction = if predictor == 6 {
                upper_right
            } else if predictor == 14 {
                clamp_sample((left + up + upper_right + upper_left + 2) >> 2, maximum)
            } else if predictor == 23 {
                clamp_sample((left + up + upper_right + 1) / 3, maximum)
            } else {
                predict_adaptive(
                    predictor,
                    left,
                    up,
                    upper_left,
                    upper_right,
                    second_left,
                    second_up,
                    maximum,
                )
            };
            let residual = if magnitude == 0 {
                0
            } else {
                let hint = sign_hint(
                    left,
                    up,
                    upper_left,
                    upper_right,
                    second_left,
                    second_up,
                    prediction,
                    maximum,
                );
                let fine_index = if hint == 0 {
                    context
                } else {
                    context * 2 + usize::from(hint < 0)
                };
                let mut negative = if has_input {
                    let sign_base = cross_zero_state * 5 + local_state;
                    let hint_class = if hint < 0 { 2 } else { usize::from(hint > 0) };
                    let cross_index =
                        (sign_base + sign_base_states * hint_class) * context_stride + context;
                    if hint == 0 {
                        let parent_value = *model
                            .nonzero_sign
                            .get(fine_index)
                            .ok_or_else(|| invalid("mode-45 fine sign context is out of range"))?;
                        let child = probability(
                            &mut cross_sign,
                            cross_index,
                            "mode-45 cross sign context is out of range",
                        )?;
                        if *child == 0 {
                            *child = parent_value;
                        }
                        let negative = decoder.bit(child)?;
                        update_probability(
                            probability(
                                &mut model.nonzero_sign,
                                fine_index,
                                "mode-45 fine sign context is out of range",
                            )?,
                            negative,
                            decoder.adaptation,
                        );
                        negative
                    } else {
                        let parent_value = *model.signed_hint.get(fine_index).ok_or_else(|| {
                            invalid("mode-45 hinted sign context is out of range")
                        })?;
                        let child = probability(
                            &mut cross_sign,
                            cross_index,
                            "mode-45 cross hinted-sign context is out of range",
                        )?;
                        if *child == 0 {
                            *child = parent_value;
                        }
                        let negative = decoder.bit(child)?;
                        update_probability(
                            probability(
                                &mut model.signed_hint,
                                fine_index,
                                "mode-45 hinted sign context is out of range",
                            )?,
                            negative,
                            decoder.adaptation,
                        );
                        negative
                    }
                } else if hint == 0 {
                    decoder.bit(probability(
                        &mut model.nonzero_sign,
                        fine_index,
                        "mode-45 fine sign context is out of range",
                    )?)?
                } else {
                    decoder.bit(probability(
                        &mut model.signed_hint,
                        fine_index,
                        "mode-45 hinted sign context is out of range",
                    )?)?
                };
                if context_sign != 0 {
                    negative ^= context_sign < 0;
                }
                negative ^= hint < 0;
                if negative {
                    -(magnitude as i32)
                } else {
                    magnitude as i32
                }
            };
            plane[position] = ((prediction + residual) & maximum) as u16;
            west_west_bits = previous_bits;
            west_west_sign = previous_sign;
            previous_bits = length;
            previous_sign = residual.signum();
            upper_bits[x] = length.min(15) as u8;
            upper_sign[x] = previous_sign as i8;
            if has_output {
                let next = channel_state(activity, residual, length);
                let stored = if matches!(flow, ChannelStateFlow::Middle) {
                    (stored_state << 4) | next
                } else {
                    next
                };
                *channel_state_buffer
                    .as_deref_mut()
                    .and_then(|state| state.get_mut(position))
                    .ok_or_else(|| invalid("mode-45 channel state is truncated"))? = stored;
            }
            previous_upper_bits = up_bits;
            previous_upper_sign = up_sign;
        }
    }
    Ok(plane)
}

struct PatternModel {
    run_unary: [[u16; RUN_BITS + 1]; 2],
    run_mantissa: [[[u16; RUN_BITS]; RUN_BITS + 1]; 2],
    fixed_bits: [[u16; 10]; 3],
}

impl PatternModel {
    fn new() -> Self {
        Self {
            run_unary: [[PROB_INIT; RUN_BITS + 1]; 2],
            run_mantissa: [[[PROB_INIT; RUN_BITS]; RUN_BITS + 1]; 2],
            fixed_bits: [[PROB_INIT; 10]; 3],
        }
    }

    fn run_uint(&mut self, decoder: &mut RangeDecoder<'_>, context: usize) -> Result<u32> {
        let mut bits = 0_usize;
        while bits < RUN_BITS && decoder.bit(&mut self.run_unary[context][bits])? {
            bits += 1;
        }
        if bits == 0 {
            return Ok(0);
        }
        let mut value = 1_u32 << (bits - 1);
        for bit in (0..bits - 1).rev() {
            if decoder.bit(&mut self.run_mantissa[context][bits][bit])? {
                value |= 1_u32 << bit;
            }
        }
        Ok(value)
    }

    fn bits(
        &mut self,
        decoder: &mut RangeDecoder<'_>,
        context: usize,
        count: usize,
    ) -> Result<u32> {
        if count > self.fixed_bits[context].len() {
            return Err(invalid("fixed symbol is too wide"));
        }
        let mut value = 0_u32;
        for bit in (0..count).rev() {
            if decoder.bit(&mut self.fixed_bits[context][bit])? {
                value |= 1_u32 << bit;
            }
        }
        Ok(value)
    }
}

fn nbits(value: u32) -> usize {
    (u32::BITS - value.leading_zeros()) as usize
}

fn fallible_filled<T: Clone>(length: usize, value: T, context: &'static str) -> Result<Vec<T>> {
    let mut output = Vec::new();
    output
        .try_reserve_exact(length)
        .map_err(|_| Error::AllocationFailed(context))?;
    output.resize(length, value);
    Ok(output)
}

fn decode_event_run(
    decoder: &mut RangeDecoder<'_>,
    model: &mut EventModel,
    context: usize,
) -> Result<usize> {
    let mut length = 0_usize;
    while length < RUN_BITS && decoder.bit(model.run_unary(context, length)?)? {
        length += 1;
    }
    if length == 0 {
        return Ok(0);
    }
    let mut value = 1_usize << (length - 1);
    for bit in (0..length - 1).rev() {
        if decoder.bit(model.run_mantissa(context, length, bit)?)? {
            value |= 1_usize << bit;
        }
    }
    Ok(value)
}

fn decode_event_magnitude(
    decoder: &mut RangeDecoder<'_>,
    model: &mut EventModel,
    context: usize,
    depth: usize,
) -> Result<usize> {
    let mut length = 0_usize;
    while length < depth && decoder.bit(model.unary(context, length)?)? {
        length += 1;
    }
    if length == 0 {
        return Ok(0);
    }
    let mut value = 1_usize << (length - 1);
    for bit in (0..length - 1).rev() {
        if decoder.bit(model.mantissa(context, length, bit)?)? {
            value |= 1_usize << bit;
        }
    }
    Ok(value)
}

fn event_prediction(
    left: i32,
    up: i32,
    upper_left: i32,
    upper_right: i32,
    second_left: i32,
    second_up: i32,
    maximum: i32,
) -> i32 {
    let edge = (left - up).abs() + (left - upper_left).abs() + (up - upper_left).abs();
    let horizontal =
        (left - second_left).abs() + (up - upper_left).abs() + (upper_right - up).abs();
    let vertical = (left - upper_left).abs() + (up - second_up).abs() + (upper_right - up).abs();
    let low = (maximum >> 5).max(4);
    let high = maximum >> 1;
    let gap = (maximum >> 4).max(8);
    if edge <= low {
        (left + up + 1) >> 1
    } else if horizontal + gap < vertical {
        left
    } else if vertical + gap < horizontal {
        up
    } else if edge >= high {
        paeth(left, up, upper_left)
    } else {
        gradient_adaptive(
            left,
            up,
            upper_left,
            upper_right,
            second_left,
            second_up,
            maximum,
        )
    }
}

fn event_order_position(rank: usize, width: usize, height: usize, order: usize) -> Result<usize> {
    let (x, y) = match order {
        1 => (rank / height, rank % height),
        2..=5 => {
            const SLOPES: [i64; 4] = [1, -1, 2, -2];
            let band = rank / height;
            let y = rank % height;
            let width_i64 = i64::try_from(width)
                .map_err(|_| Error::ArithmeticOverflow("QST1 mode-39 order width"))?;
            let x = (i64::try_from(band)
                .map_err(|_| Error::ArithmeticOverflow("QST1 mode-39 order band"))?
                + SLOPES[order - 2]
                    * i64::try_from(y)
                        .map_err(|_| Error::ArithmeticOverflow("QST1 mode-39 order row"))?)
            .rem_euclid(width_i64);
            (
                usize::try_from(x)
                    .map_err(|_| Error::ArithmeticOverflow("QST1 mode-39 order column"))?,
                y,
            )
        }
        6 => {
            let y = rank / width;
            let mut x = rank % width;
            if y & 1 != 0 {
                x = width - 1 - x;
            }
            (x, y)
        }
        7 => {
            let x = rank / height;
            let mut y = rank % height;
            if x & 1 != 0 {
                y = height - 1 - y;
            }
            (x, y)
        }
        _ => (rank % width, rank / width),
    };
    y.checked_mul(width)
        .and_then(|row| row.checked_add(x))
        .ok_or(Error::ArithmeticOverflow("QST1 mode-39 order position"))
}

fn decode_mode39_plane(
    decoder: &mut RangeDecoder<'_>,
    width: usize,
    height: usize,
    depth: usize,
) -> Result<Vec<u16>> {
    const MAX_EVENT_PIXELS: usize = 0x00ff_ffff;

    let pixels = width
        .checked_mul(height)
        .ok_or(Error::ArithmeticOverflow("QST1 mode-39 pixels"))?;
    if pixels > MAX_EVENT_PIXELS {
        return Err(invalid("mode-39 plane exceeds event rank limit"));
    }
    let mut model = EventModel::new()?;
    let order = model.tree3(decoder)?;
    let events = decode_event_run(decoder, &mut model, 0)?;
    if events > pixels {
        return Err(invalid("mode-39 event count exceeds pixel count"));
    }
    // Event residuals are stored as their two's-complement bit patterns until
    // row-major reconstruction overwrites each slot with its decoded sample.
    // One fallible plane allocation for mode 39.
    let mut plane = fallible_filled(pixels, 0_u16, "QST1 mode-39 plane")?;
    let mut previous_rank: Option<usize> = None;
    let mut previous_bits = 0_usize;
    let mut previous_sign = 0_i32;
    let half = 1_usize << (depth - 1);
    for _ in 0..events {
        let skip_context = error_context(activity_context(0), previous_bits, previous_sign, true);
        let skip = decode_event_run(decoder, &mut model, skip_context)?;
        let rank = match previous_rank {
            Some(previous) => previous
                .checked_add(skip)
                .and_then(|value| value.checked_add(1))
                .ok_or(Error::ArithmeticOverflow("QST1 mode-39 event rank"))?,
            None => skip,
        };
        if rank >= pixels {
            return Err(invalid("mode-39 event rank exceeds pixel count"));
        }
        let context = error_context(
            activity_context(
                i32::try_from(skip.min(512))
                    .map_err(|_| Error::ArithmeticOverflow("QST1 mode-39 skip activity"))?,
            ),
            previous_bits,
            previous_sign,
            true,
        );
        let magnitude_minus_one = decode_event_magnitude(decoder, &mut model, context, depth)?;
        let magnitude = magnitude_minus_one
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow("QST1 mode-39 magnitude"))?;
        if magnitude > half {
            return Err(invalid("mode-39 residual magnitude is out of range"));
        }
        let mut negative = decoder.bit(model.sign(context, previous_sign)?)?;
        if previous_sign != 0 {
            negative ^= previous_sign < 0;
        }
        let residual = if negative {
            -(magnitude as i32)
        } else {
            magnitude as i32
        };
        let position = event_order_position(rank, width, height, order)?;
        *plane
            .get_mut(position)
            .ok_or_else(|| invalid("mode-39 order position exceeds plane"))? =
            (residual as i16) as u16;
        previous_bits = nbits(magnitude as u32);
        previous_sign = (residual > 0) as i32 - (residual < 0) as i32;
        previous_rank = Some(rank);
    }

    let maximum = (1_i32 << depth) - 1;
    let half_sample = 1_i32 << (depth - 1);
    for y in 0..height {
        for x in 0..width {
            let position = y * width + x;
            let left = if x != 0 {
                i32::from(plane[position - 1])
            } else if y != 0 {
                i32::from(plane[position - width])
            } else {
                half_sample
            };
            let up = if y != 0 {
                i32::from(plane[position - width])
            } else {
                left
            };
            let upper_left = if x != 0 && y != 0 {
                i32::from(plane[position - width - 1])
            } else {
                up
            };
            let upper_right = if y != 0 && x + 1 < width {
                i32::from(plane[position - width + 1])
            } else {
                up
            };
            let second_left = if x > 1 {
                i32::from(plane[position - 2])
            } else {
                left
            };
            let second_up = if y > 1 {
                i32::from(plane[position - width * 2])
            } else {
                up
            };
            let prediction = event_prediction(
                left,
                up,
                upper_left,
                upper_right,
                second_left,
                second_up,
                maximum,
            );
            let residual = i32::from(plane[position] as i16);
            plane[position] = ((prediction + residual) & maximum) as u16;
        }
    }
    Ok(plane)
}

fn put_pattern(
    plane: &mut [u16],
    width: usize,
    height: usize,
    block_x: usize,
    block_y: usize,
    values: [u16; 4],
) -> Result<()> {
    let x = block_x
        .checked_mul(2)
        .ok_or(Error::ArithmeticOverflow("QST1 pattern x coordinate"))?;
    let y = block_y
        .checked_mul(2)
        .ok_or(Error::ArithmeticOverflow("QST1 pattern y coordinate"))?;
    let first = y
        .checked_mul(width)
        .and_then(|row| row.checked_add(x))
        .ok_or(Error::ArithmeticOverflow("QST1 pattern offset"))?;
    *plane
        .get_mut(first)
        .ok_or_else(|| invalid("pattern block is outside the image"))? = values[0];
    if x + 1 < width {
        *plane
            .get_mut(first + 1)
            .ok_or_else(|| invalid("pattern block is outside the image"))? = values[1];
    }
    if y + 1 < height {
        let lower = first
            .checked_add(width)
            .ok_or(Error::ArithmeticOverflow("QST1 lower pattern offset"))?;
        *plane
            .get_mut(lower)
            .ok_or_else(|| invalid("pattern block is outside the image"))? = values[2];
        if x + 1 < width {
            *plane
                .get_mut(lower + 1)
                .ok_or_else(|| invalid("pattern block is outside the image"))? = values[3];
        }
    }
    Ok(())
}

fn decode_pattern_plane(
    decoder: &mut RangeDecoder<'_>,
    width: usize,
    height: usize,
    depth: usize,
) -> Result<Vec<u16>> {
    let pixels = width
        .checked_mul(height)
        .ok_or(Error::ArithmeticOverflow("QST1 pattern pixel count"))?;
    let block_width = width
        .checked_add(1)
        .ok_or(Error::ArithmeticOverflow("QST1 pattern block width"))?
        / 2;
    let block_height = height
        .checked_add(1)
        .ok_or(Error::ArithmeticOverflow("QST1 pattern block height"))?
        / 2;
    let blocks = block_width
        .checked_mul(block_height)
        .ok_or(Error::ArithmeticOverflow("QST1 pattern block count"))?;
    if blocks == 0 || blocks > 0x00ff_ffff {
        return Err(invalid("invalid pattern block count"));
    }
    let mut plane = fallible_filled(pixels, 0_u16, "QST1 pattern plane")?;
    let mut model = PatternModel::new();
    let dictionary_minus_one = model.run_uint(decoder, 0)?;
    if dictionary_minus_one >= PATTERN_MAX as u32 {
        return Err(invalid("pattern dictionary is too large"));
    }
    let dictionary_len = dictionary_minus_one as usize + 1;
    let mut dictionary = [[0_u16; 4]; PATTERN_MAX];
    for values in &mut dictionary[..dictionary_len] {
        for value in values {
            *value = model.bits(decoder, 0, depth)? as u16;
        }
    }
    let token_bits = nbits(dictionary_len as u32);
    let mut block = 0_usize;
    while block < blocks {
        let token = model.bits(decoder, 1, token_bits)? as usize;
        let run = (model.run_uint(decoder, 1)? as usize)
            .checked_add(1)
            .ok_or_else(|| invalid("pattern run overflows"))?;
        let end = block
            .checked_add(run)
            .filter(|&end| end <= blocks)
            .ok_or_else(|| invalid("pattern run exceeds block count"))?;
        if token > dictionary_len {
            return Err(invalid("pattern dictionary token is out of range"));
        }
        for current in block..end {
            let values = if token == 0 {
                let mut values = [0_u16; 4];
                for value in &mut values {
                    *value = model.bits(decoder, 2, depth)? as u16;
                }
                values
            } else {
                dictionary[token - 1]
            };
            put_pattern(
                &mut plane,
                width,
                height,
                current % block_width,
                current / block_width,
                values,
            )?;
        }
        block = end;
    }
    Ok(plane)
}

fn floor_shift(value: i32, shift: u32) -> i32 {
    if value >= 0 {
        value >> shift
    } else {
        -(((value.unsigned_abs() + (1_u32 << shift) - 1) >> shift) as i32)
    }
}

fn rg_blend(red: i32, green: i32, transform: u8) -> i32 {
    const WEIGHTS: [i32; 18] = [
        8, 16, 24, 40, 48, 56, 64, 12, 20, 28, 18, 22, 26, 19, 21, 23, 25, 27,
    ];
    let red_weight = WEIGHTS[usize::from(transform - 11)];
    floor_shift(red_weight * red + (64 - red_weight) * green, 6)
}

fn rg_luma_blend(red: i32, green: i32, transform: u8) -> i32 {
    const WEIGHTS: [i32; 6] = [0, 16, 20, 22, 24, 32];
    let red_weight = WEIGHTS[usize::from(transform - 29)];
    floor_shift(red_weight * red + (64 - red_weight) * green, 6)
}

fn rg_luma_lift(red_delta: i32, blue_delta: i32) -> i32 {
    floor_shift(red_delta + blue_delta, 2)
}

fn normal_map_blue_predictor(red: i32, green: i32) -> i32 {
    let x = red - 128;
    let y = green - 128;
    let quadratic = 255 - ((x * x + y * y + 127) >> 8);
    quadratic.max(128)
}

fn normal_map_sphere_predictor(red: i32, green: i32) -> i32 {
    const CORRECTION: [i32; 64] = [
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4,
        5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 11, 12, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 24, 25,
        27, 29, 31, 34, 37, 41, 45, 53, 63,
    ];
    let x = red - 128;
    let y = green - 128;
    let radius_squared = x * x + y * y;
    if radius_squared >= 16_129 {
        return 128;
    }
    255 - ((radius_squared + 127) >> 8) - CORRECTION[(radius_squared >> 8) as usize]
}

fn color_plane_depth(transform: u8, plane: usize) -> usize {
    if plane == 0 || transform == 0 || transform == 40 && plane == 1 {
        8
    } else {
        9
    }
}

fn inverse_color_transform(transform: u8, first: u16, second: u16, third: u16) -> Result<[u8; 3]> {
    let first = i32::from(first);
    let second = i32::from(second);
    let third = i32::from(third);
    let (red, green, blue) = match transform {
        0 => (first, second, third),
        1 => {
            let green = first;
            (second - 256 + green, green, third - 256 + green)
        }
        2 => {
            let luma = first;
            let orange = second - 256;
            let green_delta = third - 256;
            let temporary = luma - (green_delta >> 1);
            let green = green_delta + temporary;
            let blue = temporary - (orange >> 1);
            (blue + orange, green, blue)
        }
        3 => {
            let red = first;
            (red, second - 256 + red, third - 256 + red)
        }
        4 => {
            let blue = first;
            (second - 256 + blue, third - 256 + blue, blue)
        }
        5 => {
            let green = first;
            let red = second - 256 + green;
            (red, green, third - 256 + ((red + green) >> 1))
        }
        6 => {
            let green = first;
            let blue = second - 256 + green;
            (third - 256 + ((blue + green) >> 1), green, blue)
        }
        7 => {
            let red = first;
            let blue = second - 256 + red;
            (red, third - 256 + ((red + blue) >> 1), blue)
        }
        8 => {
            let blue = first;
            let red = second - 256 + blue;
            (red, third - 256 + ((red + blue) >> 1), blue)
        }
        9 => {
            let red = first;
            let green = second - 256 + red;
            (red, green, third - 256 + green)
        }
        10 => {
            let blue = first;
            let green = second - 256 + blue;
            (third - 256 + green, green, blue)
        }
        11..=28 => {
            let green = first;
            let red = second - 256 + green;
            (red, green, third - 256 + rg_blend(red, green, transform))
        }
        29..=34 => {
            let red_delta = second - 256;
            let blend_delta = rg_luma_blend(red_delta, 0, transform);
            let blue_delta = third - 256 + blend_delta;
            let green = first - rg_luma_lift(red_delta, blue_delta);
            (green + red_delta, green, green + blue_delta)
        }
        35 => {
            let red = first;
            let green = second - 256 + red;
            (red, green, third - 256 + floor_shift(green + red, 1))
        }
        36 => {
            let blue = first;
            let red = second - 256 + blue;
            (
                red,
                third - 256 + floor_shift(40 * red + 24 * blue, 6),
                blue,
            )
        }
        37 => {
            let blue = first;
            let green = second - 256 + blue;
            (
                third - 256 + floor_shift(40 * green + 24 * blue, 6),
                green,
                blue,
            )
        }
        38 => {
            let red = first;
            let green = second - 128;
            (
                red,
                green,
                third - 256 + normal_map_blue_predictor(red, green),
            )
        }
        39 => {
            let red = first;
            let green = second - 128;
            (
                red,
                green,
                third - 256 + normal_map_sphere_predictor(red, green),
            )
        }
        40 => {
            let red = first;
            let green = second;
            (
                red,
                green,
                third - 256 + normal_map_sphere_predictor(red, green),
            )
        }
        _ => return Err(invalid("invalid inverse color transform")),
    };
    Ok([red as u8, green as u8, blue as u8])
}

fn apply_sample_grid(samples: &mut [u8], channels: usize, bits: u8) -> Result<()> {
    if bits == 0 {
        return Ok(());
    }
    let maximum = (1_u32 << bits) - 1;
    let color_channels = if channels == 1 { 1 } else { 3 };
    for pixel in samples.chunks_exact_mut(channels) {
        for sample in &mut pixel[..color_channels] {
            let compact = u32::from(*sample);
            if compact > maximum {
                return Err(invalid("sample exceeds declared compact grid"));
            }
            *sample = ((compact * 255 + maximum / 2) / maximum) as u8;
        }
    }
    Ok(())
}

fn decode_stream_bytes_into(stream: &Qst1Stream<'_>, output: &mut [u8]) -> Result<()> {
    let width = stream.info.width as usize;
    let height = stream.info.height as usize;
    let pixels = width
        .checked_mul(height)
        .ok_or(Error::ArithmeticOverflow("QST1 decoded pixel count"))?;
    let channels = usize::from(stream.info.channels);
    let output_len = pixels
        .checked_mul(channels)
        .ok_or(Error::ArithmeticOverflow("QST1 decoded sample bytes"))?;
    if output.len() != output_len {
        return Err(invalid("sample destination does not match the QST1 shape"));
    }
    let mut decoder = RangeDecoder::new(stream.payload, stream.info.adaptation)?;
    output.fill(0);

    match stream.info.mode {
        0 => {
            let plane = decode_base_plane(&mut decoder, width, height, 8, stream.info.tile_log)?;
            match (stream.info.channels, stream.info.flags & 1 != 0) {
                (1, false) => {
                    for (target, &sample) in output.iter_mut().zip(&plane) {
                        *target = sample as u8;
                    }
                }
                (3, true) => {
                    for (pixel, &sample) in output.chunks_exact_mut(3).zip(&plane) {
                        pixel.fill(sample as u8);
                    }
                }
                _ => {
                    return Err(Error::UnsupportedQst1Channels {
                        mode: stream.info.mode,
                        channels: stream.info.channels,
                    });
                }
            }
        }
        1 => {
            let palette_count = usize::from(stream.info.palette_count);
            let depth = nbits((palette_count - 1) as u32).max(1);
            let indices =
                decode_base_plane(&mut decoder, width, height, depth, stream.info.tile_log)?;
            for (pixel, &index) in output.chunks_exact_mut(channels).zip(&indices) {
                let index = usize::from(index);
                if index >= palette_count {
                    return Err(invalid("palette index is out of range"));
                }
                let start = index
                    .checked_mul(channels)
                    .ok_or(Error::ArithmeticOverflow("QST1 palette offset"))?;
                let color = stream
                    .palette
                    .get(start..start + channels)
                    .ok_or_else(|| invalid("QST1 palette is truncated"))?;
                pixel.copy_from_slice(color);
            }
        }
        37 => {
            if stream.info.channels == 1 || stream.info.flags & 1 != 0 {
                let gray =
                    decode_mode37_plane(&mut decoder, width, height, 8, stream.info.tile_log)?;
                if channels == 1 {
                    for (target, &sample) in output.iter_mut().zip(&gray) {
                        *target = sample as u8;
                    }
                } else {
                    for (pixel, &sample) in output.chunks_exact_mut(3).zip(&gray) {
                        pixel.fill(sample as u8);
                    }
                }
            } else {
                if !matches!(stream.info.channels, 3 | 4) {
                    return Err(Error::UnsupportedQst1Channels {
                        mode: stream.info.mode,
                        channels: stream.info.channels,
                    });
                }
                let second_depth = color_plane_depth(stream.info.transform, 1);
                let third_depth = color_plane_depth(stream.info.transform, 2);
                let first =
                    decode_mode37_plane(&mut decoder, width, height, 8, stream.info.tile_log)?;
                let second = decode_mode37_plane(
                    &mut decoder,
                    width,
                    height,
                    second_depth,
                    stream.info.tile_log,
                )?;
                let third = decode_mode37_plane(
                    &mut decoder,
                    width,
                    height,
                    third_depth,
                    stream.info.tile_log,
                )?;
                for pixel in 0..pixels {
                    let color = inverse_color_transform(
                        stream.info.transform,
                        first[pixel],
                        second[pixel],
                        third[pixel],
                    )?;
                    let target = pixel * channels;
                    output[target..target + 3].copy_from_slice(&color);
                }
                if channels == 4 {
                    if stream.info.flags & 2 != 0 {
                        for pixel in output.chunks_exact_mut(4) {
                            pixel[3] = stream.info.control;
                        }
                    } else {
                        let alpha = decode_mode37_plane(
                            &mut decoder,
                            width,
                            height,
                            8,
                            stream.info.tile_log,
                        )?;
                        for (pixel, &sample) in alpha.iter().enumerate() {
                            output[pixel * 4 + 3] = sample as u8;
                        }
                    }
                }
            }
        }
        39 => {
            if !matches!(stream.info.channels, 3 | 4) {
                return Err(Error::UnsupportedQst1Channels {
                    mode: stream.info.mode,
                    channels: stream.info.channels,
                });
            }
            let transform_supported = match stream.info.channels {
                3 => matches!(stream.info.transform, 2 | 7),
                4 => stream.info.transform == 2,
                _ => false,
            };
            if !transform_supported {
                return Err(Error::UnsupportedQst1Transform {
                    mode: stream.info.mode,
                    transform: stream.info.transform,
                });
            }
            if stream.info.flags != 0 {
                return Err(Error::UnsupportedQst1Flags {
                    mode: stream.info.mode,
                    flags: stream.info.flags,
                });
            }
            let first = decode_mode39_plane(&mut decoder, width, height, 8)?;
            let second = decode_mode39_plane(&mut decoder, width, height, 9)?;
            let third = decode_mode39_plane(&mut decoder, width, height, 9)?;
            for pixel in 0..pixels {
                let color = inverse_color_transform(
                    stream.info.transform,
                    first[pixel],
                    second[pixel],
                    third[pixel],
                )?;
                let target = pixel * channels;
                output[target..target + 3].copy_from_slice(&color);
            }
            if channels == 4 {
                drop(first);
                drop(second);
                drop(third);
                let alpha = decode_mode39_plane(&mut decoder, width, height, 8)?;
                for (pixel, &sample) in alpha.iter().enumerate() {
                    output[pixel * 4 + 3] = sample as u8;
                }
            }
        }
        40 => {
            if !matches!(stream.info.channels, 1 | 3) {
                return Err(Error::UnsupportedQst1Channels {
                    mode: stream.info.mode,
                    channels: stream.info.channels,
                });
            }
            if stream.info.flags != 0 {
                return Err(Error::UnsupportedQst1Flags {
                    mode: stream.info.mode,
                    flags: stream.info.flags,
                });
            }
            let transform_supported = match stream.info.channels {
                1 => stream.info.transform == 0,
                3 => matches!(stream.info.transform, 3 | 4),
                _ => false,
            };
            if !transform_supported {
                return Err(Error::UnsupportedQst1Transform {
                    mode: stream.info.mode,
                    transform: stream.info.transform,
                });
            }
            let first = decode_pattern_plane(&mut decoder, width, height, 8)?;
            if channels == 1 {
                for (target, &sample) in output.iter_mut().zip(&first) {
                    *target = sample as u8;
                }
            } else {
                let second = decode_pattern_plane(&mut decoder, width, height, 9)?;
                let third = decode_pattern_plane(&mut decoder, width, height, 9)?;
                for pixel in 0..pixels {
                    let color = inverse_color_transform(
                        stream.info.transform,
                        first[pixel],
                        second[pixel],
                        third[pixel],
                    )?;
                    output[pixel * 3..pixel * 3 + 3].copy_from_slice(&color);
                }
            }
        }
        45 => {
            if stream.info.flags & 1 != 0 {
                return Err(Error::UnsupportedQst1Flags {
                    mode: stream.info.mode,
                    flags: stream.info.flags,
                });
            }
            if !matches!(stream.info.channels, 3 | 4) {
                return Err(Error::UnsupportedQst1Channels {
                    mode: stream.info.mode,
                    channels: stream.info.channels,
                });
            }
            let second_depth = color_plane_depth(stream.info.transform, 1);
            let third_depth = color_plane_depth(stream.info.transform, 2);
            let mut state = fallible_filled(pixels, 0_u8, "QST1 mode-45 channel state")?;
            let first = decode_mode45_plane(
                &mut decoder,
                width,
                height,
                8,
                stream.info.tile_log,
                ChannelStateFlow::First,
                Some(&mut state),
            )?;
            let second = decode_mode45_plane(
                &mut decoder,
                width,
                height,
                second_depth,
                stream.info.tile_log,
                ChannelStateFlow::Middle,
                Some(&mut state),
            )?;
            let third = decode_mode45_plane(
                &mut decoder,
                width,
                height,
                third_depth,
                stream.info.tile_log,
                ChannelStateFlow::Last,
                Some(&mut state),
            )?;
            for pixel in 0..pixels {
                let color = inverse_color_transform(
                    stream.info.transform,
                    first[pixel],
                    second[pixel],
                    third[pixel],
                )?;
                let target = pixel * channels;
                output[target..target + 3].copy_from_slice(&color);
            }
            if channels == 4 {
                if stream.info.flags & 2 != 0 {
                    for pixel in output.chunks_exact_mut(4) {
                        pixel[3] = stream.info.control;
                    }
                } else {
                    let alpha = decode_mode45_plane(
                        &mut decoder,
                        width,
                        height,
                        8,
                        stream.info.tile_log,
                        ChannelStateFlow::Independent,
                        None,
                    )?;
                    for (pixel, &sample) in alpha.iter().enumerate() {
                        output[pixel * 4 + 3] = sample as u8;
                    }
                }
            }
        }
        mode @ (52..=54) => {
            let weighted_prediction = mode == 54;
            if weighted_prediction && stream.info.flags != 0
                || !weighted_prediction && stream.info.flags & 1 != 0
            {
                return Err(Error::UnsupportedQst1Flags {
                    mode: stream.info.mode,
                    flags: stream.info.flags,
                });
            }
            if weighted_prediction && stream.info.channels != 3
                || !weighted_prediction && !matches!(stream.info.channels, 3 | 4)
            {
                return Err(Error::UnsupportedQst1Channels {
                    mode: stream.info.mode,
                    channels: stream.info.channels,
                });
            }
            if weighted_prediction && !matches!(stream.info.transform, 32 | 33 | 35) {
                return Err(Error::UnsupportedQst1Transform {
                    mode: stream.info.mode,
                    transform: stream.info.transform,
                });
            }
            let refined_sign = mode >= 53;
            let second_depth = color_plane_depth(stream.info.transform, 1);
            let third_depth = color_plane_depth(stream.info.transform, 2);
            let mut state = fallible_filled(pixels, 0_u8, "QST1 local-root channel state")?;
            let first = decode_local_root_plane(
                &mut decoder,
                width,
                height,
                8,
                stream.info.tile_log,
                refined_sign,
                weighted_prediction,
                ChannelStateFlow::First,
                Some(&mut state),
            )?;
            let second = decode_local_root_plane(
                &mut decoder,
                width,
                height,
                second_depth,
                stream.info.tile_log,
                refined_sign,
                weighted_prediction,
                ChannelStateFlow::Middle,
                Some(&mut state),
            )?;
            let third = decode_local_root_plane(
                &mut decoder,
                width,
                height,
                third_depth,
                stream.info.tile_log,
                refined_sign,
                weighted_prediction,
                ChannelStateFlow::Last,
                Some(&mut state),
            )?;
            for pixel in 0..pixels {
                let color = inverse_color_transform(
                    stream.info.transform,
                    first[pixel],
                    second[pixel],
                    third[pixel],
                )?;
                let target = pixel * channels;
                output[target..target + 3].copy_from_slice(&color);
            }
            if channels == 4 {
                if stream.info.flags & 2 != 0 {
                    for pixel in output.chunks_exact_mut(4) {
                        pixel[3] = stream.info.control;
                    }
                } else {
                    let alpha = decode_local_root_plane(
                        &mut decoder,
                        width,
                        height,
                        8,
                        stream.info.tile_log,
                        refined_sign,
                        weighted_prediction,
                        ChannelStateFlow::Independent,
                        None,
                    )?;
                    for (pixel, &sample) in alpha.iter().enumerate() {
                        output[pixel * 4 + 3] = sample as u8;
                    }
                }
            }
        }
        mode => return Err(Error::UnsupportedQst1Mode(mode)),
    }
    apply_sample_grid(output, channels, stream.info.sample_bits)?;
    if crc32(output) != stream.info.pixel_crc32 {
        return Err(invalid("decoded pixel checksum mismatch"));
    }
    Ok(())
}

fn decode_stream_bytes(stream: &Qst1Stream<'_>) -> Result<Vec<u8>> {
    let pixels = usize::try_from(stream.info.width)
        .map_err(|_| Error::ArithmeticOverflow("QST1 decoded width"))?
        .checked_mul(
            usize::try_from(stream.info.height)
                .map_err(|_| Error::ArithmeticOverflow("QST1 decoded height"))?,
        )
        .ok_or(Error::ArithmeticOverflow("QST1 decoded pixel count"))?;
    let output_len = pixels
        .checked_mul(usize::from(stream.info.channels))
        .ok_or(Error::ArithmeticOverflow("QST1 decoded sample bytes"))?;
    let mut output = fallible_filled(output_len, 0_u8, "QST1 decoded samples")?;
    decode_stream_bytes_into(stream, &mut output)?;
    Ok(output)
}

pub(crate) fn decode_qst1_bytes(
    bytes: &[u8],
    expected_width: u32,
    expected_height: u32,
    expected_channels: u8,
    limits: &Limits,
) -> Result<Vec<u8>> {
    let stream = parse_qst1(bytes, limits)?;
    if stream.info.width != expected_width || stream.info.height != expected_height {
        return Err(invalid("dimensions do not match the enclosing stream"));
    }
    if stream.info.channels != expected_channels {
        return Err(invalid("channel count does not match the enclosing stream"));
    }
    decode_stream_bytes(&stream)
}

pub(crate) fn decode_qst1_rgba(
    bytes: &[u8],
    expected_width: u32,
    expected_height: u32,
    limits: &Limits,
) -> Result<Vec<u8>> {
    let stream = parse_qst1(bytes, limits)?;
    let pixels = usize::try_from(expected_width)
        .map_err(|_| Error::ArithmeticOverflow("QST1 RGBA width"))?
        .checked_mul(
            usize::try_from(expected_height)
                .map_err(|_| Error::ArithmeticOverflow("QST1 RGBA height"))?,
        )
        .ok_or(Error::ArithmeticOverflow("QST1 RGBA pixel count"))?;
    let rgba_len = pixels
        .checked_mul(4)
        .ok_or(Error::ArithmeticOverflow("QST1 RGBA byte count"))?;
    let mut rgba = fallible_filled(rgba_len, 0_u8, "QST1 RGBA pixels")?;
    decode_qst1_rgba_into(
        &stream,
        expected_width,
        expected_height,
        stream.info.channels,
        &mut rgba,
    )?;
    Ok(rgba)
}

pub(crate) fn decode_qst1_rgba_into(
    stream: &Qst1Stream<'_>,
    expected_width: u32,
    expected_height: u32,
    expected_channels: u8,
    rgba: &mut [u8],
) -> Result<()> {
    if stream.info.width != expected_width || stream.info.height != expected_height {
        return Err(invalid("dimensions do not match the enclosing image"));
    }
    if stream.info.channels != expected_channels {
        return Err(invalid("channel count does not match the enclosing image"));
    }
    let pixels = usize::try_from(expected_width)
        .map_err(|_| Error::ArithmeticOverflow("QST1 RGBA width"))?
        .checked_mul(
            usize::try_from(expected_height)
                .map_err(|_| Error::ArithmeticOverflow("QST1 RGBA height"))?,
        )
        .ok_or(Error::ArithmeticOverflow("QST1 RGBA pixel count"))?;
    let rgba_len = pixels
        .checked_mul(4)
        .ok_or(Error::ArithmeticOverflow("QST1 RGBA byte count"))?;
    if rgba.len() != rgba_len {
        return Err(invalid("RGBA destination does not match the QST1 shape"));
    }
    match stream.info.channels {
        1 => {
            let samples = decode_stream_bytes(stream)?;
            for (pixel, &value) in rgba.chunks_exact_mut(4).zip(&samples) {
                pixel[0] = value;
                pixel[1] = value;
                pixel[2] = value;
                pixel[3] = 255;
            }
        }
        3 => {
            let samples = decode_stream_bytes(stream)?;
            for (pixel, color) in rgba.chunks_exact_mut(4).zip(samples.chunks_exact(3)) {
                pixel[..3].copy_from_slice(color);
                pixel[3] = 255;
            }
        }
        4 => decode_stream_bytes_into(stream, rgba)?,
        channels => {
            return Err(Error::UnsupportedQst1Channels {
                mode: stream.info.mode,
                channels,
            });
        }
    }
    Ok(())
}

#[cfg(test)]
mod transform_tests {
    use super::{floor_shift, inverse_color_transform, normal_map_sphere_predictor};

    fn floor_div(value: i32, divisor: i32) -> i32 {
        value.div_euclid(divisor)
    }

    fn forward_transform(transform: u8, red: i32, green: i32, blue: i32) -> [u16; 3] {
        let values = match transform {
            0 => [red, green, blue],
            1 => [green, red - green + 256, blue - green + 256],
            2 => {
                let orange = red - blue;
                let temporary = blue + (orange >> 1);
                let green_delta = green - temporary;
                let luma = temporary + (green_delta >> 1);
                [luma, orange + 256, green_delta + 256]
            }
            3 => [red, green - red + 256, blue - red + 256],
            4 => [blue, red - blue + 256, green - blue + 256],
            5 => [green, red - green + 256, blue - ((red + green) >> 1) + 256],
            6 => [green, blue - green + 256, red - ((blue + green) >> 1) + 256],
            7 => [red, blue - red + 256, green - ((red + blue) >> 1) + 256],
            8 => [blue, red - blue + 256, green - ((red + blue) >> 1) + 256],
            9 => [red, green - red + 256, blue - green + 256],
            10 => [blue, green - blue + 256, red - green + 256],
            11..=28 => {
                const WEIGHTS: [i32; 18] = [
                    8, 16, 24, 40, 48, 56, 64, 12, 20, 28, 18, 22, 26, 19, 21, 23, 25, 27,
                ];
                let red_weight = WEIGHTS[usize::from(transform - 11)];
                let blend = floor_div(red_weight * red + (64 - red_weight) * green, 64);
                [green, red - green + 256, blue - blend + 256]
            }
            29..=34 => {
                const WEIGHTS: [i32; 6] = [0, 16, 20, 22, 24, 32];
                let red_delta = red - green;
                let blue_delta = blue - green;
                let lift = floor_div(red_delta + blue_delta, 4);
                let red_weight = WEIGHTS[usize::from(transform - 29)];
                let blend = floor_div(red_weight * red + (64 - red_weight) * green, 64);
                [green + lift, red_delta + 256, blue - blend + 256]
            }
            35 => [
                red,
                green - red + 256,
                blue - floor_div(green + red, 2) + 256,
            ],
            36 => [
                blue,
                red - blue + 256,
                green - floor_div(40 * red + 24 * blue, 64) + 256,
            ],
            37 => [
                blue,
                green - blue + 256,
                red - floor_div(40 * green + 24 * blue, 64) + 256,
            ],
            38 => {
                let x = red - 128;
                let y = green - 128;
                let prediction = (255 - ((x * x + y * y + 127) >> 8)).max(128);
                [red, green + 128, blue - prediction + 256]
            }
            39 => {
                let prediction = normal_map_sphere_predictor(red, green);
                [red, green + 128, blue - prediction + 256]
            }
            40 => {
                let prediction = normal_map_sphere_predictor(red, green);
                [red, green, blue - prediction + 256]
            }
            _ => unreachable!(),
        };
        [values[0] as u16, values[1] as u16, values[2] as u16]
    }

    #[test]
    fn explicit_floor_shift_matches_floor_division_across_transform_range() {
        for (value, shift, expected) in [
            (-129, 6, -3),
            (-65, 6, -2),
            (-64, 6, -1),
            (-63, 6, -1),
            (-3, 1, -2),
            (-1, 1, -1),
            (0, 6, 0),
            (63, 6, 0),
            (64, 6, 1),
            (129, 6, 2),
        ] {
            assert_eq!(floor_shift(value, shift), expected, "{value} >> {shift}");
        }
        for shift in [1, 2, 6] {
            let divisor = 1_i32 << shift;
            for value in -16_320..=16_320 {
                assert_eq!(
                    floor_shift(value, shift),
                    value.div_euclid(divisor),
                    "{value} >> {shift}"
                );
            }
        }
    }

    #[test]
    fn all_color_transforms_roundtrip_rgb_extrema() {
        const SAMPLES: [i32; 10] = [0, 1, 2, 63, 64, 127, 128, 191, 254, 255];
        for transform in 0..=40 {
            for red in SAMPLES {
                for green in SAMPLES {
                    for blue in SAMPLES {
                        let encoded = forward_transform(transform, red, green, blue);
                        assert!(encoded[0] <= 255, "transform {transform} first plane");
                        let second_max = if transform == 0 || transform == 40 {
                            255
                        } else {
                            511
                        };
                        let third_max = if transform == 0 { 255 } else { 511 };
                        assert!(
                            encoded[1] <= second_max,
                            "transform {transform} second plane"
                        );
                        assert!(encoded[2] <= third_max, "transform {transform} third plane");
                        assert_eq!(
                            inverse_color_transform(transform, encoded[0], encoded[1], encoded[2])
                                .unwrap(),
                            [red as u8, green as u8, blue as u8],
                            "transform {transform}, RGB({red}, {green}, {blue})"
                        );
                    }
                }
            }
        }
    }
}
