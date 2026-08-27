use std::fmt;

pub type Result<T> = std::result::Result<T, Error>;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LimitKind {
    FileBytes,
    PayloadBytes,
    Pixels,
    AnimationBytes,
    DecodedBytes,
    MetadataBytes,
    Frames,
    Chunks,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Error {
    InvalidLimits(&'static str),
    LimitExceeded {
        kind: LimitKind,
        limit: u64,
        actual: u64,
    },
    Truncated {
        offset: usize,
        needed: usize,
        remaining: usize,
    },
    ArithmeticOverflow(&'static str),
    InvalidMagic,
    MissingContainerChecksum,
    ContainerChecksumMismatch {
        expected: u32,
        actual: u32,
    },
    InvalidMode(u8),
    InvalidTransform(u8),
    InvalidCodec(u8),
    InvalidHeader(&'static str),
    NotWideImage,
    InvalidQsw1(&'static str),
    NotHdrImage,
    InvalidQsw2(&'static str),
    UnsupportedCriticalQsw2Chunk([u8; 4]),
    InvalidLzms(&'static str),
    InvalidQst1(&'static str),
    UnsupportedQst1Mode(u8),
    UnsupportedQst1Transform {
        mode: u8,
        transform: u8,
    },
    UnsupportedQst1Channels {
        mode: u8,
        channels: u8,
    },
    UnsupportedQst1Flags {
        mode: u8,
        flags: u8,
    },
    UnsupportedPixelMode(u8),
    UnsupportedPixelTransform {
        mode: u8,
        transform: u8,
    },
    InvalidPixelData(&'static str),
    InvalidAnimation(&'static str),
    AllocationFailed(&'static str),
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidLimits(name) => write!(formatter, "invalid decode limit: {name}"),
            Self::LimitExceeded {
                kind,
                limit,
                actual,
            } => write!(
                formatter,
                "resource limit exceeded for {kind:?}: {actual} > {limit}"
            ),
            Self::Truncated {
                offset,
                needed,
                remaining,
            } => write!(
                formatter,
                "truncated input at byte {offset}: need {needed}, have {remaining}"
            ),
            Self::ArithmeticOverflow(context) => {
                write!(formatter, "integer overflow while computing {context}")
            }
            Self::InvalidMagic => formatter.write_str("not a QLIC file"),
            Self::MissingContainerChecksum => {
                formatter.write_str("corrupt file: missing container checksum")
            }
            Self::ContainerChecksumMismatch { expected, actual } => write!(
                formatter,
                "corrupt file: container checksum mismatch ({actual:#010x} != {expected:#010x})"
            ),
            Self::InvalidMode(mode) => write!(formatter, "corrupt file: invalid mode {mode}"),
            Self::InvalidTransform(transform) => {
                write!(formatter, "corrupt file: invalid transform {transform}")
            }
            Self::InvalidCodec(codec) => {
                write!(formatter, "corrupt file: invalid codec byte {codec:#04x}")
            }
            Self::InvalidHeader(message) => write!(formatter, "corrupt file: {message}"),
            Self::NotWideImage => formatter.write_str("QLIC image is not a wide sample stream"),
            Self::InvalidQsw1(message) => write!(formatter, "corrupt QSW1 stream: {message}"),
            Self::NotHdrImage => {
                formatter.write_str("QLIC image is not a self-describing HDR stream")
            }
            Self::InvalidQsw2(message) => write!(formatter, "corrupt QSW2 stream: {message}"),
            Self::UnsupportedCriticalQsw2Chunk(tag) => write!(
                formatter,
                "unsupported critical QSW2 chunk {:?}",
                String::from_utf8_lossy(tag)
            ),
            Self::InvalidLzms(message) => write!(formatter, "invalid LZMS stream: {message}"),
            Self::InvalidQst1(message) => write!(formatter, "invalid QST1 stream: {message}"),
            Self::UnsupportedQst1Mode(mode) => {
                write!(
                    formatter,
                    "QST1 mode {mode} is not implemented by the Rust decoder"
                )
            }
            Self::UnsupportedQst1Transform { mode, transform } => write!(
                formatter,
                "QST1 mode {mode}, transform {transform} is not implemented by the Rust decoder"
            ),
            Self::UnsupportedQst1Channels { mode, channels } => write!(
                formatter,
                "QST1 mode {mode} with {channels} channels is not implemented by the Rust decoder"
            ),
            Self::UnsupportedQst1Flags { mode, flags } => write!(
                formatter,
                "QST1 mode {mode} with flags {flags:#04x} is not implemented by the Rust decoder"
            ),
            Self::UnsupportedPixelMode(mode) => {
                write!(
                    formatter,
                    "pixel decoding is not implemented for QLIC mode {mode}"
                )
            }
            Self::UnsupportedPixelTransform { mode, transform } => write!(
                formatter,
                "pixel decoding is not implemented for QLIC mode {mode}, transform {transform}"
            ),
            Self::InvalidPixelData(message) => {
                write!(formatter, "corrupt pixel payload: {message}")
            }
            Self::InvalidAnimation(message) => {
                write!(formatter, "corrupt animation payload: {message}")
            }
            Self::AllocationFailed(context) => {
                write!(formatter, "allocation failed while preparing {context}")
            }
        }
    }
}

impl std::error::Error for Error {}
