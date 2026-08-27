use std::panic::{AssertUnwindSafe, catch_unwind};

use qlic_decoder::{Container, Error, LimitKind, Limits, crc32, decode_rgba, parse_qst1};

// Current production C output for the retained libpng reference image
// benchmarks_and_tools/broad-matrix-2026-07-26/corpus/reference/004_ct1n0g04.png.
// The public encoder canonicalized its pixel-gray input to one channel. The
// promotion helper applies the authoritative channel-3 gray-flag header and
// RGB checksum; the range payload is exactly the production base-plane stream.
const RETAINED_MODE0_HEX: &str = concat!(
    "514c494320000000200000000900008000000000ed0000000000000051535431",
    "2000000020000000010000000404125b80c1cf0000004b249db60048700d51aa",
    "3507bfec6ca855e5b985048ed0310f2f0f67acd097bb57e1f7096f879280dc1",
    "e21b8e2f30a61bc579647ea49ef5a12fbd17c53e10c6384fcc4ff71f0674d79",
    "626939ef3a936beffd990b96ad3218f14264d940bf6cfcdb9d4862309aaab3c",
    "f8f1254d314abf0d65941770480e92fccc9518685e4e727c21df4b51cad0916",
    "869b074d964ec385f426b85aadeff83a3c65a1df485eb1e107f2f4db4c5350",
    "5b1bf7f0927a9bef3d203d629ba75b608ca90f26b1e3be3a05808872d2fdc3",
    "a47d3601653f48695258040b0001b63bbb",
);

fn nibble(byte: u8) -> u8 {
    match byte {
        b'0'..=b'9' => byte - b'0',
        b'a'..=b'f' => byte - b'a' + 10,
        _ => unreachable!("fixture hex is lowercase ASCII"),
    }
}

fn retained_mode0_stream() -> Vec<u8> {
    RETAINED_MODE0_HEX
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| nibble(pair[0]) << 4 | nibble(pair[1]))
        .collect()
}

fn rewrite_outer_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - 4;
    let checksum = crc32(&bytes[..footer]).to_le_bytes();
    bytes[footer..].copy_from_slice(&checksum);
}

fn rewrite_inner_and_outer_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - 4;
    bytes[54..58].fill(0);
    let checksum = crc32(&bytes[28..footer]).to_le_bytes();
    bytes[54..58].copy_from_slice(&checksum);
    rewrite_outer_crc(bytes);
}

fn retained_gray_rgb_stream() -> Vec<u8> {
    let mut file = retained_mode0_stream();
    file[40] = 3;
    file[41] = 1;
    file[46..50].copy_from_slice(&0x895c_8730_u32.to_le_bytes());
    rewrite_inner_and_outer_crc(&mut file);
    file
}

#[test]
fn decodes_retained_mode0_gray_flag_stream_with_exact_rgb_checksum() {
    let file = retained_gray_rgb_stream();
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!((stream.info.width, stream.info.height), (32, 32));
    assert_eq!(stream.info.channels, 3);
    assert_eq!(stream.info.flags, 1);
    assert_eq!(stream.info.mode, 0);
    assert_eq!(stream.info.transform, 0);
    assert_eq!(stream.info.tile_log, 4);
    assert_eq!(stream.info.adaptation, 4);
    assert_eq!(stream.info.pixel_crc32, 0x895c_8730);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!((decoded.width, decoded.height), (32, 32));
    assert_eq!(decoded.rgba.len(), 32 * 32 * 4);
    assert_eq!(crc32(&decoded.rgba), 0x0d0a_052c);
    for (pixel, gray) in [
        (0, 255),
        (54, 247),
        (86, 0),
        (147, 170),
        (213, 124),
        (365, 102),
        (371, 141),
        (492, 222),
    ] {
        assert_eq!(
            &decoded.rgba[pixel * 4..pixel * 4 + 4],
            &[gray, gray, gray, 255]
        );
    }
}

#[test]
fn mode0_keeps_non_gray_rgb_explicitly_unsupported_and_checks_limits() {
    let original = retained_gray_rgb_stream();
    let mut non_gray_rgb = original.clone();
    non_gray_rgb[41] = 0;
    rewrite_inner_and_outer_crc(&mut non_gray_rgb);
    assert!(matches!(
        decode_rgba(&non_gray_rgb, &Limits::default()),
        Err(Error::UnsupportedQst1Channels {
            mode: 0,
            channels: 3
        })
    ));

    let limits = Limits {
        max_pixels: 1_023,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&original, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::Pixels,
            actual: 1_024,
            ..
        })
    ));

    let limits = Limits {
        max_decoded_bytes: 4_095,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&original, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual: 4_096,
            ..
        })
    ));
}

#[test]
fn mode0_gray_public_decode_never_panics_on_truncation_or_malformed_data() {
    let original = retained_gray_rgb_stream();
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 1 << 18,
        max_decoded_bytes: 1 << 20,
        ..Limits::default()
    };

    for length in 0..original.len() {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&original[..length], &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on mode-0 prefix of {length} bytes"
        );
    }

    for offset in 28..original.len() - 4 {
        let mut mutated = original.clone();
        mutated[offset] ^= 0xa5;
        rewrite_inner_and_outer_crc(&mut mutated);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on rechecksummed mode-0 mutation at {offset}"
        );
    }
}
