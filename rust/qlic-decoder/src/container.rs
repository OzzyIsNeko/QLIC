use std::mem::size_of;

use crate::crc32::crc32;
use crate::cursor::Cursor;
use crate::hdr::{AlphaAssociation, ColorAuthority, SampleType, parse_qsw2};
use crate::{Error, LimitKind, Limits, Result};

const HEADER_SIZE: usize = 28;
const FOOTER_SIZE: usize = 4;
const CODEC_CRC_FLAG: u8 = 0x80;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum Mode {
    Gray = 1,
    GrayAlpha = 2,
    Rgb = 3,
    Rgba = 4,
    Palette = 5,
    Separable = 7,
    Native = 9,
    Filtered = 10,
    PaletteStream = 11,
    PredictedPalette = 12,
    CompressedPalette = 13,
    Tiles = 14,
    TileModel = 15,
    GrayModel = 16,
    Animation = 17,
    Blocks = 18,
    NativeWide = 19,
    HdrWide = 20,
}

impl TryFrom<u8> for Mode {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            1 => Ok(Self::Gray),
            2 => Ok(Self::GrayAlpha),
            3 => Ok(Self::Rgb),
            4 => Ok(Self::Rgba),
            5 => Ok(Self::Palette),
            7 => Ok(Self::Separable),
            9 => Ok(Self::Native),
            10 => Ok(Self::Filtered),
            11 => Ok(Self::PaletteStream),
            12 => Ok(Self::PredictedPalette),
            13 => Ok(Self::CompressedPalette),
            14 => Ok(Self::Tiles),
            15 => Ok(Self::TileModel),
            16 => Ok(Self::GrayModel),
            17 => Ok(Self::Animation),
            18 => Ok(Self::Blocks),
            19 => Ok(Self::NativeWide),
            20 => Ok(Self::HdrWide),
            _ => Err(Error::InvalidMode(value)),
        }
    }
}

impl Mode {
    const fn has_external_palette(self) -> bool {
        matches!(
            self,
            Self::Palette | Self::PaletteStream | Self::PredictedPalette
        )
    }

    const fn may_have_internal_count(self) -> bool {
        matches!(
            self,
            Self::CompressedPalette
                | Self::Tiles
                | Self::TileModel
                | Self::Animation
                | Self::NativeWide
                | Self::HdrWide
        )
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum Transform {
    Identity = 0,
    GreenDelta = 1,
    IdentityRaw = 2,
    GreenDeltaRaw = 3,
    IdentityRle = 4,
    GreenDeltaRle = 5,
    IndexRle = 6,
    SeparableDelta = 7,
    RedDelta = 8,
    BlueDelta = 9,
    CompressedPaletteDelta = 10,
    BlueDeltaPlanarMed = 11,
    CompressedPaletteTiles = 12,
    CompressedPalettePlanar = 13,
}

impl TryFrom<u8> for Transform {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0 => Ok(Self::Identity),
            1 => Ok(Self::GreenDelta),
            2 => Ok(Self::IdentityRaw),
            3 => Ok(Self::GreenDeltaRaw),
            4 => Ok(Self::IdentityRle),
            5 => Ok(Self::GreenDeltaRle),
            6 => Ok(Self::IndexRle),
            7 => Ok(Self::SeparableDelta),
            8 => Ok(Self::RedDelta),
            9 => Ok(Self::BlueDelta),
            10 => Ok(Self::CompressedPaletteDelta),
            11 => Ok(Self::BlueDeltaPlanarMed),
            12 => Ok(Self::CompressedPaletteTiles),
            13 => Ok(Self::CompressedPalettePlanar),
            _ => Err(Error::InvalidTransform(value)),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum Codec {
    Store = 0,
    Lzms = 3,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Header {
    pub width: u32,
    pub height: u32,
    pub mode: Mode,
    pub transform: Transform,
    pub index_bits: u8,
    pub codec: Codec,
    pub palette_count: u32,
    pub payload_size: u64,
    pub palette_size: usize,
    pub compressed_size: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ImageInfo {
    pub width: u32,
    pub height: u32,
    pub frame_count: u32,
    pub animated: bool,
    pub channels: u32,
    pub bits_per_sample: u32,
    pub sample_type: Option<SampleType>,
    pub alpha_association: Option<AlphaAssociation>,
    pub color_authority: Option<ColorAuthority>,
    pub has_icc: bool,
    pub has_cicp: bool,
    pub has_mastering_display: bool,
    pub has_content_light: bool,
}

#[derive(Clone, Copy, Debug)]
pub struct Container<'a> {
    pub header: Header,
    pub palette: &'a [u8],
    pub payload: &'a [u8],
    pub container_crc32: u32,
}

#[repr(C)]
struct CImageAllocationLayout {
    width: u32,
    height: u32,
    pixels: *const u8,
}

#[repr(C)]
struct CAnimationFrameAllocationLayout {
    image: CImageAllocationLayout,
    delay_ms: u32,
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

fn palette_count_ok(count: u32, bits: u8) -> bool {
    if count == 0 || !(1..=16).contains(&bits) || count > 65_536 {
        return false;
    }
    bits == 16 || count <= (1_u32 << bits)
}

fn palette_bits(count: u32) -> Option<u8> {
    if count == 0 || count > 65_536 {
        return None;
    }
    Some(match count {
        1..=2 => 1,
        3..=4 => 2,
        5..=16 => 4,
        17..=256 => 8,
        _ => (u32::BITS - (count - 1).leading_zeros()) as u8,
    })
}

fn validate_mode_header(
    mode: Mode,
    transform: Transform,
    index_bits: u8,
    codec: Codec,
    palette_count: u32,
) -> Result<()> {
    if transform == Transform::BlueDeltaPlanarMed
        && (!matches!(mode, Mode::Rgb | Mode::Rgba)
            || index_bits != 0
            || palette_count != 0
            || codec != Codec::Lzms)
    {
        return Err(Error::InvalidHeader("invalid planar MED header"));
    }
    if transform == Transform::CompressedPalettePlanar
        && (mode != Mode::CompressedPalette
            || palette_count <= 256
            || palette_bits(palette_count) != Some(index_bits))
    {
        return Err(Error::InvalidHeader(
            "invalid planar compressed-palette header",
        ));
    }

    match mode {
        Mode::Native
            if transform != Transform::Identity || index_bits != 0 || palette_count != 0 =>
        {
            return Err(Error::InvalidHeader("invalid native stream header"));
        }
        Mode::Filtered if !(1..=4).contains(&index_bits) || palette_count != 0 => {
            return Err(Error::InvalidHeader("invalid filtered stream header"));
        }
        Mode::PaletteStream if !palette_count_ok(palette_count, index_bits) => {
            return Err(Error::InvalidHeader("invalid palette stream header"));
        }
        Mode::PredictedPalette
            if !palette_count_ok(palette_count, index_bits) || transform != Transform::IndexRle =>
        {
            return Err(Error::InvalidHeader("invalid predicted-palette header"));
        }
        Mode::CompressedPalette
            if !palette_count_ok(palette_count, index_bits)
                || !matches!(
                    transform,
                    Transform::IdentityRaw
                        | Transform::IndexRle
                        | Transform::CompressedPaletteDelta
                        | Transform::CompressedPaletteTiles
                        | Transform::CompressedPalettePlanar
                ) =>
        {
            return Err(Error::InvalidHeader("invalid compressed-palette header"));
        }
        Mode::Tiles
            if !matches!(index_bits, 1 | 3 | 4)
                || palette_count == 0
                || transform != Transform::Identity
                || codec != Codec::Store =>
        {
            return Err(Error::InvalidHeader("invalid tile stream header"));
        }
        Mode::TileModel
            if !matches!(index_bits, 1 | 3 | 4)
                || palette_count == 0
                || transform != Transform::Identity
                || codec != Codec::Store =>
        {
            return Err(Error::InvalidHeader("invalid tile-model header"));
        }
        Mode::GrayModel
            if transform != Transform::Identity || index_bits != 0 || palette_count != 0 =>
        {
            return Err(Error::InvalidHeader("invalid gray-model header"));
        }
        Mode::Animation
            if transform != Transform::Identity || index_bits != 0 || palette_count == 0 =>
        {
            return Err(Error::InvalidHeader("invalid animation header"));
        }
        Mode::Blocks
            if transform != Transform::Identity
                || !matches!(index_bits, 16 | 64)
                || palette_count != 0
                || codec != Codec::Store =>
        {
            return Err(Error::InvalidHeader("invalid block stream header"));
        }
        Mode::Palette if !palette_count_ok(palette_count, index_bits) => {
            return Err(Error::InvalidHeader("invalid palette header"));
        }
        _ => {}
    }

    if !mode.has_external_palette() && !mode.may_have_internal_count() && palette_count != 0 {
        return Err(Error::InvalidHeader("unexpected palette data"));
    }
    Ok(())
}

impl<'a> Container<'a> {
    pub fn parse(bytes: &'a [u8], limits: &Limits) -> Result<Self> {
        limits.validate()?;
        let file_bytes =
            u64::try_from(bytes.len()).map_err(|_| Error::ArithmeticOverflow("file byte count"))?;
        limit(LimitKind::FileBytes, limits.max_file_bytes, file_bytes)?;

        if bytes.len() < HEADER_SIZE {
            return Err(Error::Truncated {
                offset: 0,
                needed: HEADER_SIZE,
                remaining: bytes.len(),
            });
        }
        let mut cursor = Cursor::new(&bytes[..HEADER_SIZE]);
        if cursor.take(4)? != b"QLIC" {
            return Err(Error::InvalidMagic);
        }
        let width = cursor.read_u32_le()?;
        let height = cursor.read_u32_le()?;
        let raw_mode = cursor.read_u8()?;
        let raw_transform = cursor.read_u8()?;
        let index_bits = cursor.read_u8()?;
        let packed_codec = cursor.read_u8()?;
        let palette_count = cursor.read_u32_le()?;
        let payload_size = cursor.read_u64_le()?;

        if packed_codec & CODEC_CRC_FLAG == 0 {
            return Err(Error::MissingContainerChecksum);
        }
        if width == 0 || height == 0 {
            return Err(Error::InvalidHeader("invalid dimensions"));
        }
        let pixels = u64::from(width)
            .checked_mul(u64::from(height))
            .ok_or(Error::ArithmeticOverflow("pixel count"))?;
        limit(LimitKind::Pixels, limits.max_pixels, pixels)?;
        let rgba_bytes = pixels
            .checked_mul(4)
            .ok_or(Error::ArithmeticOverflow("RGBA byte count"))?;
        if usize::try_from(rgba_bytes).is_err() {
            return Err(Error::LimitExceeded {
                kind: LimitKind::Pixels,
                limit: (usize::MAX / 4) as u64,
                actual: pixels,
            });
        }
        limit(
            LimitKind::PayloadBytes,
            limits.max_payload_bytes,
            payload_size,
        )?;
        let mode = Mode::try_from(raw_mode)?;
        let transform = Transform::try_from(raw_transform)?;
        if packed_codec & !(CODEC_CRC_FLAG | 3) != 0 {
            return Err(Error::InvalidCodec(packed_codec));
        }
        let codec = match packed_codec & !CODEC_CRC_FLAG {
            0 => Codec::Store,
            3 => Codec::Lzms,
            _ => return Err(Error::InvalidCodec(packed_codec)),
        };
        validate_mode_header(mode, transform, index_bits, codec, palette_count)?;

        if matches!(mode, Mode::NativeWide | Mode::HdrWide) {
            let channels = u64::from(palette_count);
            let bits = u32::from(index_bits);
            let minimum_bits = if mode == Mode::HdrWide { 8 } else { 9 };
            if transform != Transform::Identity
                || !(minimum_bits..=24).contains(&bits)
                || !matches!(channels, 1 | 3 | 4)
                || codec != Codec::Store
            {
                return Err(Error::InvalidHeader("invalid wide stream header"));
            }
            let storage = if bits <= 16 { 2 } else { 4 };
            let decoded_bytes = pixels
                .checked_mul(channels)
                .and_then(|value| value.checked_mul(storage))
                .ok_or(Error::ArithmeticOverflow("wide decoded byte count"))?;
            if usize::try_from(decoded_bytes).is_err() {
                return Err(Error::LimitExceeded {
                    kind: LimitKind::DecodedBytes,
                    limit: usize::MAX as u64,
                    actual: decoded_bytes,
                });
            }
            limit(
                LimitKind::DecodedBytes,
                limits.max_decoded_bytes,
                decoded_bytes,
            )?;
        } else {
            limit(
                LimitKind::DecodedBytes,
                limits.max_decoded_bytes,
                rgba_bytes,
            )?;
        }

        if mode == Mode::Animation {
            let frames = u64::from(palette_count);
            limit(LimitKind::Frames, u64::from(limits.max_frames), frames)?;
            let table_bytes = frames
                .checked_mul(size_of::<CAnimationFrameAllocationLayout>() as u64)
                .ok_or(Error::ArithmeticOverflow("animation frame table"))?;
            let animation_bytes = rgba_bytes
                .checked_mul(frames)
                .and_then(|value| value.checked_add(table_bytes))
                .ok_or(Error::ArithmeticOverflow("animation allocation"))?;
            limit(
                LimitKind::AnimationBytes,
                limits.max_animation_bytes,
                animation_bytes,
            )?;
        }

        let palette_size = if mode.has_external_palette() {
            usize::try_from(palette_count)
                .ok()
                .and_then(|count| count.checked_mul(4))
                .ok_or(Error::ArithmeticOverflow("external palette size"))?
        } else {
            0
        };
        let payload_start = HEADER_SIZE
            .checked_add(palette_size)
            .ok_or(Error::ArithmeticOverflow("payload offset"))?;
        if bytes.len() < HEADER_SIZE + FOOTER_SIZE {
            return Err(Error::Truncated {
                offset: bytes.len(),
                needed: HEADER_SIZE + FOOTER_SIZE,
                remaining: bytes.len(),
            });
        }
        let body_end = bytes.len() - FOOTER_SIZE;
        let mut footer = Cursor::new(bytes.get(body_end..).ok_or(Error::Truncated {
            offset: body_end,
            needed: FOOTER_SIZE,
            remaining: bytes.len().saturating_sub(body_end),
        })?);
        let actual_crc = footer.read_u32_le()?;
        let expected_crc = crc32(&bytes[..body_end]);
        if actual_crc != expected_crc {
            return Err(Error::ContainerChecksumMismatch {
                expected: expected_crc,
                actual: actual_crc,
            });
        }
        if payload_start > body_end {
            return Err(Error::InvalidHeader("palette exceeds file size"));
        }
        let compressed_size = body_end - payload_start;
        Ok(Self {
            header: Header {
                width,
                height,
                mode,
                transform,
                index_bits,
                codec,
                palette_count,
                payload_size,
                palette_size,
                compressed_size,
            },
            palette: &bytes[HEADER_SIZE..payload_start],
            payload: &bytes[payload_start..body_end],
            container_crc32: actual_crc,
        })
    }

    pub fn info(&self, limits: &Limits) -> Result<ImageInfo> {
        if self.header.mode == Mode::HdrWide {
            return Ok(parse_qsw2(self, limits)?.info());
        }
        let wide = self.header.mode == Mode::NativeWide;
        let animated = matches!(self.header.mode, Mode::Animation);
        Ok(ImageInfo {
            width: self.header.width,
            height: self.header.height,
            frame_count: if animated {
                self.header.palette_count
            } else {
                1
            },
            animated,
            channels: if wide { self.header.palette_count } else { 4 },
            bits_per_sample: if wide {
                self.header.index_bits as u32
            } else {
                8
            },
            sample_type: Some(SampleType::UnsignedInteger),
            alpha_association: None,
            color_authority: None,
            has_icc: false,
            has_cicp: false,
            has_mastering_display: false,
            has_content_light: false,
        })
    }
}

pub fn parse_info(bytes: &[u8], limits: &Limits) -> Result<ImageInfo> {
    Container::parse(bytes, limits)?.info(limits)
}
