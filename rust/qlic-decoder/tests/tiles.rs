use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{
    Codec, Container, Error, LimitKind, Limits, Mode, Transform, crc32, decode_rgba, parse_qst1,
};

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn retained_profile(relative: &str) -> Vec<u8> {
    fs::read(repository_root().join(relative)).unwrap()
}

fn rewrite_outer_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - 4;
    let checksum = crc32(&bytes[..footer]).to_le_bytes();
    bytes[footer..].copy_from_slice(&checksum);
}

fn stored_tiles(
    width: u32,
    height: u32,
    channels: u8,
    tile_height: u32,
    payload: &[u8],
) -> Vec<u8> {
    let mut file = Vec::new();
    file.extend_from_slice(b"QLIC");
    file.extend_from_slice(&width.to_le_bytes());
    file.extend_from_slice(&height.to_le_bytes());
    file.push(Mode::Tiles as u8);
    file.push(Transform::Identity as u8);
    file.push(channels);
    file.push(0x80 | Codec::Store as u8);
    file.extend_from_slice(&tile_height.to_le_bytes());
    file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    file.extend_from_slice(payload);
    file.extend_from_slice(&crc32(&file).to_le_bytes());
    file
}

fn one_band_tiles(native: &[u8]) -> Vec<u8> {
    let outer = Container::parse(native, &Limits::default()).unwrap();
    assert_eq!(outer.header.mode, Mode::Native);
    assert_eq!(outer.header.codec, Codec::Store);
    let qst1 = parse_qst1(outer.payload, &Limits::default()).unwrap();
    let mut payload = Vec::new();
    payload.extend_from_slice(&1_u32.to_le_bytes());
    payload.extend_from_slice(&(outer.payload.len() as u32).to_le_bytes());
    payload.extend_from_slice(outer.payload);
    stored_tiles(
        outer.header.width,
        outer.header.height,
        qst1.info.channels,
        outer.header.height,
        &payload,
    )
}

fn stored_native(qst1: &[u8]) -> Vec<u8> {
    let stream = parse_qst1(qst1, &Limits::default()).unwrap();
    let mut file = Vec::new();
    file.extend_from_slice(b"QLIC");
    file.extend_from_slice(&stream.info.width.to_le_bytes());
    file.extend_from_slice(&stream.info.height.to_le_bytes());
    file.push(Mode::Native as u8);
    file.push(Transform::Identity as u8);
    file.push(0);
    file.push(0x80 | Codec::Store as u8);
    file.extend_from_slice(&0_u32.to_le_bytes());
    file.extend_from_slice(&(qst1.len() as u64).to_le_bytes());
    file.extend_from_slice(qst1);
    file.extend_from_slice(&crc32(&file).to_le_bytes());
    file
}

fn tile_chunk(file: &[u8], index: usize) -> Vec<u8> {
    let outer = Container::parse(file, &Limits::default()).unwrap();
    assert_eq!(outer.header.mode, Mode::Tiles);
    let count = u32::from_le_bytes(outer.payload[..4].try_into().unwrap()) as usize;
    assert!(index < count);
    let mut offset = 4 + count * 4;
    for current in 0..count {
        let size_offset = 4 + current * 4;
        let size = u32::from_le_bytes(
            outer.payload[size_offset..size_offset + 4]
                .try_into()
                .unwrap(),
        ) as usize;
        if current == index {
            return outer.payload[offset..offset + size].to_vec();
        }
        offset += size;
    }
    unreachable!("validated tile chunk index")
}

fn rewrite_nested_and_outer_crc(bytes: &mut [u8], qst1_start: usize, qst1_size: usize) {
    let qst1_end = qst1_start + qst1_size;
    let mut qst1 = bytes[qst1_start..qst1_end].to_vec();
    qst1[26..30].fill(0);
    let checksum = crc32(&qst1).to_le_bytes();
    bytes[qst1_start + 26..qst1_start + 30].copy_from_slice(&checksum);
    rewrite_outer_crc(bytes);
}

#[test]
fn decodes_literal_first_rgba_tile_band_pair_exactly() {
    let directory = "retained/native-rgba-tile-bands-final-pairs";
    let baseline = retained_profile(&format!("{directory}/0000-baseline.qlic"));
    let candidate = retained_profile(&format!("{directory}/0000-candidate.qlic"));
    let outer = Container::parse(&candidate, &Limits::default()).unwrap();
    assert_eq!(outer.header.mode, Mode::Tiles);
    assert_eq!(outer.header.codec, Codec::Store);
    assert_eq!(outer.header.index_bits, 4);
    assert_eq!(outer.header.palette_count, 2_048);
    assert_eq!(
        u32::from_le_bytes(outer.payload[..4].try_into().unwrap()),
        3
    );

    let baseline = decode_rgba(&baseline, &Limits::default()).unwrap();
    let candidate = decode_rgba(&candidate, &Limits::default()).unwrap();
    assert_eq!((candidate.width, candidate.height), (2_635, 6_000));
    assert_eq!(crc32(&candidate.rgba), 0x02db_e989);
    assert_eq!(candidate, baseline);
}

#[test]
fn decodes_literal_second_rgba_tile_band_pair_with_mode39_bands_exactly() {
    let directory = "retained/native-rgba-tile-bands-final-pairs";
    let baseline = retained_profile(&format!("{directory}/0001-baseline.qlic"));
    let candidate = retained_profile(&format!("{directory}/0001-candidate.qlic"));
    let outer = Container::parse(&candidate, &Limits::default()).unwrap();
    assert_eq!(outer.header.mode, Mode::Tiles);
    assert_eq!(outer.header.codec, Codec::Store);
    assert_eq!(outer.header.index_bits, 4);
    assert_eq!(outer.header.palette_count, 256);
    assert_eq!(
        u32::from_le_bytes(outer.payload[..4].try_into().unwrap()),
        23
    );

    let baseline = decode_rgba(&baseline, &Limits::default()).unwrap();
    let candidate = decode_rgba(&candidate, &Limits::default()).unwrap();
    assert_eq!((candidate.width, candidate.height), (2_359, 5_722));
    assert_eq!(crc32(&candidate.rgba), 0x9a4e_c244);
    assert_eq!(candidate, baseline);
}

#[test]
fn decodes_retained_mode39_rgba_event_band_and_keeps_scope_explicit() {
    let candidate =
        retained_profile("retained/native-rgba-tile-bands-final-pairs/0001-candidate.qlic");
    let qst1 = tile_chunk(&candidate, 1);
    let native = stored_native(&qst1);
    let stream = parse_qst1(&qst1, &Limits::default()).unwrap();
    assert_eq!((stream.info.width, stream.info.height), (2_359, 256));
    assert_eq!(stream.info.channels, 4);
    assert_eq!(stream.info.flags, 0);
    assert_eq!(stream.info.mode, 39);
    assert_eq!(stream.info.transform, 2);
    assert_eq!(stream.info.tile_log, 0);
    assert_eq!(stream.info.pixel_crc32, 0xe46b_8db7);
    let decoded = decode_rgba(&native, &Limits::default()).unwrap();
    assert_eq!(crc32(&decoded.rgba), 0xe46b_8db7);

    let mut unsupported_channels = native.clone();
    unsupported_channels[28 + 12] = 1;
    unsupported_channels[28 + 15] = 0;
    rewrite_nested_and_outer_crc(&mut unsupported_channels, 28, qst1.len());
    assert_eq!(
        decode_rgba(&unsupported_channels, &Limits::default()),
        Err(Error::UnsupportedQst1Channels {
            mode: 39,
            channels: 1,
        })
    );

    for (offset, value, expected) in [
        (
            28 + 15,
            0,
            Error::UnsupportedQst1Transform {
                mode: 39,
                transform: 0,
            },
        ),
        (
            28 + 13,
            2,
            Error::UnsupportedQst1Flags { mode: 39, flags: 2 },
        ),
    ] {
        let mut unsupported = native.clone();
        unsupported[offset] = value;
        rewrite_nested_and_outer_crc(&mut unsupported, 28, qst1.len());
        assert_eq!(decode_rgba(&unsupported, &Limits::default()), Err(expected));
    }
}

#[test]
fn one_band_wrapper_matches_the_small_retained_mode45_rgba_stream() {
    let native = retained_profile("retained/mode45-current-streams/0013.qlic");
    let tiled = one_band_tiles(&native);
    let original = decode_rgba(&native, &Limits::default()).unwrap();
    let decoded = decode_rgba(&tiled, &Limits::default()).unwrap();
    assert_eq!((decoded.width, decoded.height), (1_024, 1_024));
    assert_eq!(crc32(&decoded.rgba), 0x069a_52be);
    assert_eq!(decoded, original);
}

#[test]
fn tile_tables_enforce_chunk_pixel_and_decoded_byte_limits() {
    let mut too_many_payload = Vec::new();
    too_many_payload.extend_from_slice(&257_u32.to_le_bytes());
    too_many_payload.resize(4 + 257 * 4, 0);
    let too_many = stored_tiles(1, 257, 4, 1, &too_many_payload);
    assert!(matches!(
        decode_rgba(&too_many, &Limits::default()),
        Err(Error::LimitExceeded {
            kind: LimitKind::Chunks,
            limit: 256,
            actual: 257
        })
    ));

    let three_band =
        retained_profile("retained/native-rgba-tile-bands-final-pairs/0000-candidate.qlic");
    let chunk_limits = Limits {
        max_chunks: 2,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&three_band, &chunk_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::Chunks,
            limit: 2,
            actual: 3
        })
    ));

    let native = retained_profile("retained/mode45-current-streams/0013.qlic");
    let tiled = one_band_tiles(&native);
    let pixel_limits = Limits {
        max_pixels: 1_048_575,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&tiled, &pixel_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::Pixels,
            actual: 1_048_576,
            ..
        })
    ));
    let byte_limits = Limits {
        max_decoded_bytes: 4_194_303,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&tiled, &byte_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual: 4_194_304,
            ..
        })
    ));
}

#[test]
fn mode39_rejects_oversized_event_planes_before_pixel_allocation() {
    let candidate =
        retained_profile("retained/native-rgba-tile-bands-final-pairs/0001-candidate.qlic");
    let qst1 = tile_chunk(&candidate, 1);
    let mut native = stored_native(&qst1);
    for offset in [4, 28 + 4] {
        native[offset..offset + 4].copy_from_slice(&4_096_u32.to_le_bytes());
    }
    for offset in [8, 28 + 8] {
        native[offset..offset + 4].copy_from_slice(&4_096_u32.to_le_bytes());
    }
    rewrite_nested_and_outer_crc(&mut native, 28, qst1.len());
    assert!(matches!(
        decode_rgba(&native, &Limits::default()),
        Err(Error::InvalidQst1("mode-39 plane exceeds event rank limit"))
    ));
}

#[test]
fn mode39_public_decode_never_panics_on_truncation_or_repaired_crc_mutation() {
    let candidate =
        retained_profile("retained/native-rgba-tile-bands-final-pairs/0001-candidate.qlic");
    let qst1 = tile_chunk(&candidate, 1);
    let original = stored_native(&qst1);
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 1 << 20,
        max_decoded_bytes: 4 << 20,
        ..Limits::default()
    };
    for length in 0..original.len() {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&original[..length], &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on mode-39 prefix of {length} bytes"
        );
    }
    let mut offsets = (28..58).collect::<Vec<_>>();
    offsets.extend((58..original.len() - 4).step_by(47));
    offsets.push(original.len() - 5);
    offsets.sort_unstable();
    offsets.dedup();
    for offset in offsets {
        let mut mutated = original.clone();
        mutated[offset] ^= 0x5a;
        rewrite_nested_and_outer_crc(&mut mutated, 28, qst1.len());
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on repaired mode-39 mutation at {offset}"
        );
    }
}

#[test]
fn rejects_invalid_tile_tables_trailing_data_nested_shapes_and_outer_codec() {
    for (name, file) in [
        (
            "zero chunks",
            stored_tiles(1, 1, 4, 1, &0_u32.to_le_bytes()),
        ),
        (
            "wrong count",
            stored_tiles(1, 1, 4, 1, &2_u32.to_le_bytes()),
        ),
        (
            "truncated table",
            stored_tiles(1, 2, 4, 1, &[2, 0, 0, 0, 0, 0, 0, 0]),
        ),
        (
            "oversized chunk",
            stored_tiles(1, 1, 4, 1, &[1, 0, 0, 0, 100, 0, 0, 0]),
        ),
    ] {
        assert!(
            matches!(
                decode_rgba(&file, &Limits::default()),
                Err(Error::InvalidPixelData(_))
            ),
            "{name}"
        );
    }

    let native = retained_profile("retained/mode45-current-streams/0013.qlic");
    let valid = one_band_tiles(&native);
    let qst1_size = u32::from_le_bytes(valid[32..36].try_into().unwrap()) as usize;
    let mut trailing = valid[..valid.len() - 4].to_vec();
    trailing.push(0);
    let payload_size = u64::from_le_bytes(trailing[20..28].try_into().unwrap()) + 1;
    trailing[20..28].copy_from_slice(&payload_size.to_le_bytes());
    trailing.extend_from_slice(&crc32(&trailing).to_le_bytes());
    assert!(matches!(
        decode_rgba(&trailing, &Limits::default()),
        Err(Error::InvalidPixelData("trailing tile stream data"))
    ));

    for (offset, value) in [(36 + 4, 1_u32), (36 + 8, 1_u32)] {
        let mut wrong_shape = valid.clone();
        wrong_shape[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        rewrite_nested_and_outer_crc(&mut wrong_shape, 36, qst1_size);
        assert!(matches!(
            decode_rgba(&wrong_shape, &Limits::default()),
            Err(Error::InvalidPixelData(
                "tile stream chunk dimensions do not match its band"
            ))
        ));
    }
    let mut wrong_channels = valid.clone();
    wrong_channels[36 + 12] = 3;
    rewrite_nested_and_outer_crc(&mut wrong_channels, 36, qst1_size);
    assert!(matches!(
        decode_rgba(&wrong_channels, &Limits::default()),
        Err(Error::InvalidPixelData(
            "tile stream chunk channels do not match header"
        ))
    ));

    let mut lzms_outer = valid;
    lzms_outer[15] = 0x80 | Codec::Lzms as u8;
    rewrite_outer_crc(&mut lzms_outer);
    assert!(matches!(
        decode_rgba(&lzms_outer, &Limits::default()),
        Err(Error::InvalidHeader("invalid tile stream header"))
    ));
}

#[test]
fn tile_public_decode_never_panics_on_truncation_or_repaired_crc_mutation() {
    let native = retained_profile("retained/mode45-current-streams/0013.qlic");
    let original = one_band_tiles(&native);
    let qst1_size = u32::from_le_bytes(original[32..36].try_into().unwrap()) as usize;
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 2 << 20,
        max_decoded_bytes: 8 << 20,
        ..Limits::default()
    };
    for length in 0..original.len() {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&original[..length], &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on tile-stream prefix of {length} bytes"
        );
    }

    for offset in 0..36 {
        let mut mutated = original.clone();
        mutated[offset] ^= 0x5a;
        rewrite_outer_crc(&mut mutated);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on repaired outer mutation at {offset}"
        );
    }
    let mut offsets = (36..66).collect::<Vec<_>>();
    offsets.extend((66..original.len() - 4).step_by(251));
    offsets.push(original.len() - 5);
    offsets.sort_unstable();
    offsets.dedup();
    for offset in offsets {
        let mut mutated = original.clone();
        mutated[offset] ^= 0xa5;
        rewrite_nested_and_outer_crc(&mut mutated, 36, qst1_size);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on repaired nested mutation at {offset}"
        );
    }
}
