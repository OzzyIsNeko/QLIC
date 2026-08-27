#![forbid(unsafe_code)]

//! Pure Rust decoding for QLIC.
//!
//! Use [`decode_rgba`] for an owned ordinary 8-bit image or
//! [`decode_rgba_into`] to reuse caller storage. [`decode_animation`] handles
//! QAN1/QAN2 animation, [`decode_wide`] preserves exact 9--24-bit integer
//! samples, [`decode_hdr`] preserves integer HDR samples and color metadata,
//! and [`parse_info`] reads only the header. [`validate`] fully verifies any
//! supported family without returning its decoded storage.
//!
//! The crate has no dependencies, forbids unsafe code, and does not call the C
//! codec. It decodes every stream selected in QLIC's current 3,167-file
//! accepted corpus. Historical, experimental, and future modes outside the
//! implemented grammar return typed errors.

mod animation;
mod container;
mod crc32;
mod cursor;
mod decode;
mod error;
mod hdr;
mod limits;
mod lzms;
mod qst1;
mod wide;

pub use animation::{Animation, AnimationFrame, decode_animation};
pub use container::{Codec, Container, Header, ImageInfo, Mode, Transform, parse_info};
pub use crc32::crc32;
pub use decode::{RgbaImage, decode_rgba, decode_rgba_into};
pub use error::{Error, LimitKind, Result};
pub use hdr::{
    AlphaAssociation, Cicp, ColorAuthority, ContentLight, HdrImage, MasteringDisplay,
    MetadataBlock, Qsw2Descriptor, SampleType, decode_hdr, parse_qsw2,
};
pub use limits::Limits;
pub use lzms::{decompress_lzms, decompress_lzms_into};
pub use qst1::{Qst1Info, Qst1Stream, parse_qst1};
pub use wide::{Qsw1Descriptor, Qsw1Slice, WideImage, WideSamples, decode_wide, parse_qsw1};

/// Fully decode and verify any supported QLIC family, then release its output.
pub fn validate(bytes: &[u8], limits: &Limits) -> Result<()> {
    let container = Container::parse(bytes, limits)?;
    match container.header.mode {
        Mode::Animation => {
            decode_animation(bytes, limits)?;
        }
        Mode::NativeWide => {
            decode_wide(bytes, limits)?;
        }
        Mode::HdrWide => {
            decode_hdr(bytes, limits)?;
        }
        _ => {
            decode_rgba(bytes, limits)?;
        }
    }
    Ok(())
}
