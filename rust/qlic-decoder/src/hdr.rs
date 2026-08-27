use crate::cursor::Cursor;
use crate::wide::{Qsw1Descriptor, WideImage, decode_qsw1_pixels, parse_qsw1_shape};
use crate::{Container, Error, ImageInfo, LimitKind, Limits, Mode, Result};

const QSW2_HEADER_SIZE: usize = 32;
const QSW2_CHUNK_HEADER_SIZE: usize = 16;
const QSW2_VERSION: u8 = 1;
const CHUNK_CRITICAL: u32 = 1;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum SampleType {
    UnsignedInteger = 1,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum AlphaAssociation {
    None = 0,
    Straight = 1,
    Premultiplied = 2,
}

impl TryFrom<u8> for AlphaAssociation {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0 => Ok(Self::None),
            1 => Ok(Self::Straight),
            2 => Ok(Self::Premultiplied),
            _ => Err(Error::InvalidQsw2("invalid alpha association")),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum ColorAuthority {
    Unspecified = 0,
    IccOnly = 1,
    CicpOnly = 2,
    IccPreferred = 3,
    CicpPreferred = 4,
}

impl TryFrom<u8> for ColorAuthority {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0 => Ok(Self::Unspecified),
            1 => Ok(Self::IccOnly),
            2 => Ok(Self::CicpOnly),
            3 => Ok(Self::IccPreferred),
            4 => Ok(Self::CicpPreferred),
            _ => Err(Error::InvalidQsw2("invalid color authority")),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Cicp {
    pub color_primaries: u16,
    pub transfer_characteristics: u16,
    pub matrix_coefficients: u16,
    pub full_range: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MasteringDisplay {
    pub primary_x: [u16; 3],
    pub primary_y: [u16; 3],
    pub white_x: u16,
    pub white_y: u16,
    pub max_luminance: u32,
    pub min_luminance: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ContentLight {
    pub max_cll: u16,
    pub max_fall: u16,
}

/// An opaque ancillary QSW2 metadata block, preserved in wire order.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MetadataBlock<'a> {
    pub tag: [u8; 4],
    pub data: &'a [u8],
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qsw2Descriptor<'a> {
    pub version: u8,
    pub width: u32,
    pub height: u32,
    pub sample_type: SampleType,
    pub bits_per_sample: u8,
    pub channels: u8,
    pub alpha_association: AlphaAssociation,
    pub color_authority: ColorAuthority,
    pub chunk_count: u32,
    pub metadata_bytes: u64,
    pub pixel_payload: &'a [u8],
    pub pixels: Qsw1Descriptor,
    pub icc: Option<&'a [u8]>,
    pub cicp: Option<Cicp>,
    pub mastering_display: Option<MasteringDisplay>,
    pub content_light: Option<ContentLight>,
    pub metadata: Vec<MetadataBlock<'a>>,
}

/// Exact integer HDR/wide samples plus self-describing color metadata.
///
/// ICC bytes borrow the original encoded input. Fixed-size metadata is copied,
/// and `pixels` owns the reconstructed integer sample buffer. No color-space or
/// alpha conversion is performed.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HdrImage<'a> {
    pub pixels: WideImage,
    pub alpha_association: AlphaAssociation,
    pub color_authority: ColorAuthority,
    pub icc: Option<&'a [u8]>,
    pub cicp: Option<Cicp>,
    pub mastering_display: Option<MasteringDisplay>,
    pub content_light: Option<ContentLight>,
    pub metadata: Vec<MetadataBlock<'a>>,
}

impl Qsw2Descriptor<'_> {
    pub fn info(&self) -> ImageInfo {
        ImageInfo {
            width: self.width,
            height: self.height,
            frame_count: 1,
            animated: false,
            channels: u32::from(self.channels),
            bits_per_sample: u32::from(self.bits_per_sample),
            sample_type: Some(self.sample_type),
            alpha_association: Some(self.alpha_association),
            color_authority: Some(self.color_authority),
            has_icc: self.icc.is_some(),
            has_cicp: self.cicp.is_some(),
            has_mastering_display: self.mastering_display.is_some(),
            has_content_light: self.content_light.is_some(),
        }
    }
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

fn parse_cicp(payload: &[u8]) -> Result<Cicp> {
    if payload.len() != 8 || payload[7] != 0 || payload[6] > 1 {
        return Err(Error::InvalidQsw2("invalid CICP chunk"));
    }
    let mut cursor = Cursor::new(payload);
    Ok(Cicp {
        color_primaries: cursor.read_u16_le()?,
        transfer_characteristics: cursor.read_u16_le()?,
        matrix_coefficients: cursor.read_u16_le()?,
        full_range: cursor.read_u8()? != 0,
    })
}

fn parse_mastering_display(payload: &[u8]) -> Result<MasteringDisplay> {
    if payload.len() != 24 {
        return Err(Error::InvalidQsw2("invalid MDCV chunk"));
    }
    let mut cursor = Cursor::new(payload);
    let mut primary_x = [0; 3];
    let mut primary_y = [0; 3];
    for index in 0..3 {
        primary_x[index] = cursor.read_u16_le()?;
        primary_y[index] = cursor.read_u16_le()?;
    }
    let value = MasteringDisplay {
        primary_x,
        primary_y,
        white_x: cursor.read_u16_le()?,
        white_y: cursor.read_u16_le()?,
        max_luminance: cursor.read_u32_le()?,
        min_luminance: cursor.read_u32_le()?,
    };
    if value.max_luminance < value.min_luminance {
        return Err(Error::InvalidQsw2("invalid mastering luminance"));
    }
    Ok(value)
}

fn parse_content_light(payload: &[u8]) -> Result<ContentLight> {
    if payload.len() != 4 {
        return Err(Error::InvalidQsw2("invalid CLLI chunk"));
    }
    let mut cursor = Cursor::new(payload);
    let value = ContentLight {
        max_cll: cursor.read_u16_le()?,
        max_fall: cursor.read_u16_le()?,
    };
    if value.max_cll < value.max_fall {
        return Err(Error::InvalidQsw2("invalid content-light metadata"));
    }
    Ok(value)
}

pub fn parse_qsw2<'a>(container: &Container<'a>, limits: &Limits) -> Result<Qsw2Descriptor<'a>> {
    limits.validate()?;
    let header = &container.header;
    if header.mode != Mode::HdrWide {
        return Err(Error::NotHdrImage);
    }
    if header.payload_size
        != u64::try_from(container.payload.len())
            .map_err(|_| Error::ArithmeticOverflow("QSW2 payload length"))?
    {
        return Err(Error::InvalidQsw2(
            "stored payload size does not match outer header",
        ));
    }
    if container.payload.len() < QSW2_HEADER_SIZE {
        return Err(Error::InvalidQsw2("truncated fixed header"));
    }

    let mut cursor = Cursor::new(container.payload);
    if cursor.take(4)? != b"QSW2" {
        return Err(Error::InvalidQsw2("invalid magic"));
    }
    let version = cursor.read_u8()?;
    if version != QSW2_VERSION {
        return Err(Error::InvalidQsw2("unsupported version"));
    }
    if cursor.read_u8()? != SampleType::UnsignedInteger as u8 {
        return Err(Error::InvalidQsw2("unsupported sample type"));
    }
    let bits_per_sample = cursor.read_u8()?;
    let channels = cursor.read_u8()?;
    if bits_per_sample != header.index_bits || u32::from(channels) != header.palette_count {
        return Err(Error::InvalidQsw2(
            "sample shape does not match outer header",
        ));
    }
    let alpha_association = AlphaAssociation::try_from(cursor.read_u8()?)?;
    let color_authority = ColorAuthority::try_from(cursor.read_u8()?)?;
    if cursor.read_u16_le()? != 0 {
        return Err(Error::InvalidQsw2(
            "reserved fixed-header field is not zero",
        ));
    }
    let chunk_count = cursor.read_u32_le()?;
    let declared_metadata = cursor.read_u64_le()?;
    let declared_pixels = cursor.read_u64_le()?;

    if chunk_count == 0 {
        return Err(Error::InvalidQsw2("chunk count is zero"));
    }
    limit(
        LimitKind::Chunks,
        u64::from(limits.max_chunks),
        u64::from(chunk_count),
    )?;
    limit(
        LimitKind::MetadataBytes,
        limits.max_metadata_bytes,
        declared_metadata,
    )?;
    if declared_pixels == 0 || usize::try_from(declared_pixels).is_err() {
        return Err(Error::InvalidQsw2("invalid pixel payload size"));
    }
    match (channels, alpha_association) {
        (4, AlphaAssociation::Straight | AlphaAssociation::Premultiplied)
        | (1 | 3, AlphaAssociation::None) => {}
        _ => return Err(Error::InvalidQsw2("invalid alpha association")),
    }

    let mut metadata_bytes = 0_u64;
    let mut pixel_payload = None;
    let mut icc = None;
    let mut cicp = None;
    let mut mastering_display = None;
    let mut content_light = None;
    let mut metadata = Vec::new();

    for _ in 0..chunk_count {
        if cursor.remaining() < QSW2_CHUNK_HEADER_SIZE {
            return Err(Error::InvalidQsw2("truncated chunk header"));
        }
        let tag = [
            cursor.read_u8()?,
            cursor.read_u8()?,
            cursor.read_u8()?,
            cursor.read_u8()?,
        ];
        let flags = cursor.read_u32_le()?;
        let length64 = cursor.read_u64_le()?;
        if flags & !CHUNK_CRITICAL != 0 {
            return Err(Error::InvalidQsw2("invalid chunk flags"));
        }
        let length = usize::try_from(length64)
            .map_err(|_| Error::InvalidQsw2("chunk length does not fit this platform"))?;
        let chunk = cursor
            .take(length)
            .map_err(|_| Error::InvalidQsw2("truncated chunk payload"))?;

        if tag == *b"PIXL" {
            if pixel_payload.is_some() || flags != CHUNK_CRITICAL || length64 != declared_pixels {
                return Err(Error::InvalidQsw2("invalid PIXL chunk"));
            }
            pixel_payload = Some(chunk);
            continue;
        }

        metadata_bytes = metadata_bytes
            .checked_add(length64)
            .ok_or(Error::ArithmeticOverflow("QSW2 metadata size"))?;
        limit(
            LimitKind::MetadataBytes,
            limits.max_metadata_bytes,
            metadata_bytes,
        )?;
        match tag {
            [b'I', b'C', b'C', b'P'] => {
                if icc.is_some() || flags != 0 || chunk.is_empty() {
                    return Err(Error::InvalidQsw2("invalid ICCP chunk"));
                }
                icc = Some(chunk);
            }
            [b'C', b'I', b'C', b'P'] => {
                if cicp.is_some() || flags != 0 {
                    return Err(Error::InvalidQsw2("invalid CICP chunk"));
                }
                cicp = Some(parse_cicp(chunk)?);
            }
            [b'M', b'D', b'C', b'V'] => {
                if mastering_display.is_some() || flags != 0 {
                    return Err(Error::InvalidQsw2("invalid MDCV chunk"));
                }
                mastering_display = Some(parse_mastering_display(chunk)?);
            }
            [b'C', b'L', b'L', b'I'] => {
                if content_light.is_some() || flags != 0 {
                    return Err(Error::InvalidQsw2("invalid CLLI chunk"));
                }
                content_light = Some(parse_content_light(chunk)?);
            }
            _ if flags & CHUNK_CRITICAL != 0 => {
                return Err(Error::UnsupportedCriticalQsw2Chunk(tag));
            }
            _ => metadata.push(MetadataBlock { tag, data: chunk }),
        }
    }

    if cursor.remaining() != 0 {
        return Err(Error::InvalidQsw2("trailing data after chunks"));
    }
    if metadata_bytes != declared_metadata {
        return Err(Error::InvalidQsw2("declared metadata size mismatch"));
    }
    let color_valid = match color_authority {
        ColorAuthority::Unspecified => icc.is_none() && cicp.is_none(),
        ColorAuthority::IccOnly => icc.is_some() && cicp.is_none(),
        ColorAuthority::CicpOnly => icc.is_none() && cicp.is_some(),
        ColorAuthority::IccPreferred | ColorAuthority::CicpPreferred => {
            icc.is_some() && cicp.is_some()
        }
    };
    if !color_valid {
        return Err(Error::InvalidQsw2(
            "color authority does not match ICC/CICP presence",
        ));
    }
    let pixel_payload = pixel_payload.ok_or(Error::InvalidQsw2("missing PIXL chunk"))?;
    let pixels = parse_qsw1_shape(
        pixel_payload,
        header.width,
        header.height,
        bits_per_sample,
        channels,
    )
    .map_err(|_| Error::InvalidQsw2("invalid embedded QSW1 shape"))?;

    Ok(Qsw2Descriptor {
        version,
        width: header.width,
        height: header.height,
        sample_type: SampleType::UnsignedInteger,
        bits_per_sample,
        channels,
        alpha_association,
        color_authority,
        chunk_count,
        metadata_bytes,
        pixel_payload,
        pixels,
        icc,
        cicp,
        mastering_display,
        content_light,
        metadata,
    })
}

pub fn decode_hdr<'a>(bytes: &'a [u8], limits: &Limits) -> Result<HdrImage<'a>> {
    let container = Container::parse(bytes, limits)?;
    let descriptor = parse_qsw2(&container, limits)?;
    let pixels = decode_qsw1_pixels(
        descriptor.pixel_payload,
        &descriptor.pixels,
        descriptor.width,
        descriptor.height,
        limits,
    )?;
    Ok(HdrImage {
        pixels,
        alpha_association: descriptor.alpha_association,
        color_authority: descriptor.color_authority,
        icc: descriptor.icc,
        cicp: descriptor.cicp,
        mastering_display: descriptor.mastering_display,
        content_light: descriptor.content_light,
        metadata: descriptor.metadata,
    })
}
