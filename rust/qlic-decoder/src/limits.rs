use crate::{Error, Result};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Limits {
    pub max_file_bytes: u64,
    pub max_payload_bytes: u64,
    pub max_pixels: u64,
    pub max_animation_bytes: u64,
    pub max_decoded_bytes: u64,
    pub max_metadata_bytes: u64,
    pub max_frames: u32,
    pub max_chunks: u32,
}

impl Limits {
    pub(crate) fn validate(&self) -> Result<()> {
        if self.max_file_bytes == 0 {
            return Err(Error::InvalidLimits("max_file_bytes"));
        }
        if self.max_payload_bytes == 0 {
            return Err(Error::InvalidLimits("max_payload_bytes"));
        }
        if self.max_pixels == 0 {
            return Err(Error::InvalidLimits("max_pixels"));
        }
        if self.max_animation_bytes == 0 {
            return Err(Error::InvalidLimits("max_animation_bytes"));
        }
        if self.max_decoded_bytes == 0 {
            return Err(Error::InvalidLimits("max_decoded_bytes"));
        }
        if self.max_metadata_bytes == 0 {
            return Err(Error::InvalidLimits("max_metadata_bytes"));
        }
        if self.max_frames == 0 {
            return Err(Error::InvalidLimits("max_frames"));
        }
        if self.max_chunks == 0 {
            return Err(Error::InvalidLimits("max_chunks"));
        }
        Ok(())
    }
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_file_bytes: 536_870_912,
            max_payload_bytes: 536_870_912,
            max_pixels: 67_108_864,
            max_animation_bytes: 536_870_912,
            max_decoded_bytes: 536_870_912,
            max_metadata_bytes: 16_777_216,
            max_frames: 100_000,
            max_chunks: 256,
        }
    }
}
