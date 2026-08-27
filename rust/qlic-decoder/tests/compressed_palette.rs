use std::fs::{self, File};
use std::io::Read;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{
    Codec, Container, Error, LimitKind, Limits, Mode, Transform, crc32, decode_rgba,
};

const HEADER_SIZE: usize = 28;
const FOOTER_SIZE: usize = 4;

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn fixture(name: &str) -> Vec<u8> {
    fs::read(
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../tests/fixtures")
            .join(name),
    )
    .unwrap()
}

fn rewrite_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - FOOTER_SIZE;
    let checksum = crc32(&bytes[..footer]).to_le_bytes();
    bytes[footer..].copy_from_slice(&checksum);
}

fn stored_cpal(
    width: u32,
    height: u32,
    transform: Transform,
    index_bits: u8,
    palette_count: u32,
    payload: &[u8],
) -> Vec<u8> {
    let mut file = Vec::new();
    file.extend_from_slice(b"QLIC");
    file.extend_from_slice(&width.to_le_bytes());
    file.extend_from_slice(&height.to_le_bytes());
    file.push(Mode::CompressedPalette as u8);
    file.push(transform as u8);
    file.push(index_bits);
    file.push(0x80 | Codec::Store as u8);
    file.extend_from_slice(&palette_count.to_le_bytes());
    file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    file.extend_from_slice(payload);
    file.extend_from_slice(&crc32(&file).to_le_bytes());
    file
}

fn palette() -> Vec<u8> {
    [[1, 2, 3, 4], [10, 20, 30, 40], [250, 240, 230, 220]].concat()
}

fn expected_pixels() -> Vec<u8> {
    let palette = palette();
    [0_usize, 1, 2, 2, 1, 0]
        .into_iter()
        .flat_map(|index| palette[index * 4..index * 4 + 4].iter().copied())
        .collect()
}

fn palette_runs() -> Vec<u8> {
    // (run length minus one, global palette index)
    vec![0, 0, 0, 1, 1, 2, 0, 1, 0, 0]
}

fn delta_palette(palette: &[u8]) -> Vec<u8> {
    palette
        .iter()
        .enumerate()
        .map(|(index, &sample)| {
            if index < 4 {
                sample
            } else {
                sample.wrapping_sub(palette[index - 4])
            }
        })
        .collect()
}

fn planar_palette(count: usize) -> Vec<u8> {
    let mut palette = Vec::with_capacity(count * 4);
    for index in 0..count {
        palette.extend_from_slice(&[
            index as u8,
            (index >> 8) as u8,
            (index as u8).wrapping_mul(17).wrapping_add(3),
            255_u8.wrapping_sub((index as u8).wrapping_mul(7)),
        ]);
    }
    palette
}

fn planar_payload(palette: &[u8], indices: &[u16], layout: u8) -> Vec<u8> {
    assert_eq!(palette.len() % 4, 0);
    let count = palette.len() / 4;
    let mut payload = Vec::with_capacity(1 + palette.len() + indices.len() * 2);
    payload.push(layout);
    for channel in 0..4 {
        let mut previous = 0_u8;
        for index in 0..count {
            let value = palette[index * 4 + channel];
            payload.push(if index == 0 {
                value
            } else {
                value.wrapping_sub(previous)
            });
            previous = value;
        }
    }
    match layout {
        0 => {
            for index in indices {
                payload.extend_from_slice(&index.to_le_bytes());
            }
        }
        1 => {
            payload.extend(indices.iter().map(|index| *index as u8));
            payload.extend(indices.iter().map(|index| (*index >> 8) as u8));
        }
        _ => payload.extend(std::iter::repeat_n(0, indices.len() * 2)),
    }
    payload
}

fn pixels_from_palette(palette: &[u8], indices: &[u16]) -> Vec<u8> {
    indices
        .iter()
        .flat_map(|&index| {
            let offset = usize::from(index) * 4;
            palette[offset..offset + 4].iter().copied()
        })
        .collect()
}

fn all_qlic_files(directory: &Path, output: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(directory).unwrap() {
        let path = entry.unwrap().path();
        if path.is_dir() {
            all_qlic_files(&path, output);
        } else if path
            .extension()
            .is_some_and(|extension| extension == "qlic")
        {
            output.push(path);
        }
    }
}

fn qlic_mode(path: &Path) -> Option<u8> {
    let mut header = [0_u8; 13];
    let mut file = File::open(path).ok()?;
    file.read_exact(&mut header).ok()?;
    (header[..4] == *b"QLIC").then_some(header[12])
}

fn retained_mode13_files() -> Vec<PathBuf> {
    let mut files = Vec::new();
    all_qlic_files(&repository_root().join("retained"), &mut files);
    files.retain(|path| qlic_mode(path) == Some(Mode::CompressedPalette as u8));
    files.sort_unstable();
    files
}

#[test]
fn decodes_all_store_cpal_transforms_exactly() {
    let palette = palette();
    let expected = expected_pixels();

    let mut packed = palette.clone();
    packed.extend_from_slice(&[0x24, 0x06]);

    let mut index_runs = palette.clone();
    index_runs.extend_from_slice(&palette_runs());

    let mut delta_runs = delta_palette(&palette);
    delta_runs.extend_from_slice(&palette_runs());

    let mut tile_local = vec![3];
    tile_local.extend_from_slice(&palette);
    tile_local.extend_from_slice(&[2, 0, 0, 0, 0xa4, 0x01]);

    for (transform, payload) in [
        (Transform::IdentityRaw, packed),
        (Transform::IndexRle, index_runs),
        (Transform::CompressedPaletteDelta, delta_runs),
        (Transform::CompressedPaletteTiles, tile_local),
    ] {
        let file = stored_cpal(3, 2, transform, 2, 3, &payload);
        let decoded = decode_rgba(&file, &Limits::default()).unwrap();
        assert_eq!((decoded.width, decoded.height), (3, 2));
        assert_eq!(decoded.rgba, expected, "transform {transform:?}");
    }
}

#[test]
fn decodes_both_planar_palette_layouts_and_palette_boundaries() {
    for (layout, count, bits, indices) in [
        (0, 257_usize, 9, [0_u16, 1, 255, 256, 42, 17]),
        (1, 65_536_usize, 16, [0_u16, 256, 65_535, 42, 1, 4096]),
    ] {
        let palette = planar_palette(count);
        let payload = planar_payload(&palette, &indices, layout);
        let file = stored_cpal(
            3,
            2,
            Transform::CompressedPalettePlanar,
            bits,
            count as u32,
            &payload,
        );
        let decoded = decode_rgba(&file, &Limits::default()).unwrap();
        assert_eq!((decoded.width, decoded.height), (3, 2));
        assert_eq!(decoded.rgba, pixels_from_palette(&palette, &indices));
    }
}

#[test]
fn matches_source_rgba_crcs_for_retained_planar_lzms_candidates() {
    for (name, width, height, expected_crc) in [
        ("cpal-planar-smoke-pg.qlic", 864, 540, 0x98b6_19fc),
        ("cpal-planar-smoke-floor01.qlic", 512, 512, 0xd3a3_c862),
    ] {
        let path = repository_root().join("retained").join(name);
        if !path.is_file() {
            continue;
        }
        let file = fs::read(&path).unwrap();
        let container = Container::parse(&file, &Limits::default()).unwrap();
        assert_eq!(container.header.mode, Mode::CompressedPalette);
        assert_eq!(
            container.header.transform,
            Transform::CompressedPalettePlanar
        );
        assert_eq!(container.header.codec, Codec::Lzms);
        let decoded = decode_rgba(&file, &Limits::default()).unwrap();
        assert_eq!((decoded.width, decoded.height), (width, height));
        assert_eq!(crc32(&decoded.rgba), expected_crc, "{}", path.display());
    }
}

#[test]
fn matches_fixed_c_decoder_crcs_for_retained_lzms_cpal_fixtures() {
    for (name, transform, width, height, expected_crc) in [
        (
            "cpalette-lzms.qlic",
            Transform::IdentityRaw,
            512,
            512,
            0xe1d7_051e,
        ),
        (
            "tile-palette-lzms.qlic",
            Transform::CompressedPaletteTiles,
            542,
            699,
            0x5553_3a44,
        ),
    ] {
        let file = fixture(name);
        let container = Container::parse(&file, &Limits::default()).unwrap();
        assert_eq!(container.header.mode, Mode::CompressedPalette);
        assert_eq!(container.header.transform, transform);
        assert_eq!(container.header.codec, Codec::Lzms);
        let decoded = decode_rgba(&file, &Limits::default()).unwrap();
        assert_eq!((decoded.width, decoded.height), (width, height));
        assert_eq!(crc32(&decoded.rgba), expected_crc);
    }
}

#[test]
fn decodes_every_curated_mode13_stream() {
    let files = retained_mode13_files();
    let file_count = files.len();
    let mut transforms = [0_usize; 14];
    let mut codecs = [0_usize; 4];
    let mut total_bytes = 0_u64;

    // Fixed local corpus; the full census remains a separate benchmark gate.
    assert!(file_count >= 12);
    for path in files {
        let file = fs::read(&path).unwrap();
        total_bytes = total_bytes.checked_add(file.len() as u64).unwrap();
        let container = Container::parse(&file, &Limits::default()).unwrap();
        transforms[container.header.transform as usize] += 1;
        codecs[container.header.codec as usize] += 1;
        let decoded = decode_rgba(&file, &Limits::default())
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
        assert_eq!(
            (decoded.width, decoded.height),
            (container.header.width, container.header.height),
            "{}",
            path.display()
        );
    }

    assert!(total_bytes >= 1_000_000);
    assert!(transforms[Transform::CompressedPalettePlanar as usize] >= 12);
    assert_eq!(transforms.iter().sum::<usize>(), file_count);
    assert!(codecs[Codec::Lzms as usize] >= 12);
    assert_eq!(codecs.iter().sum::<usize>(), file_count);
}

#[test]
fn retained_mode13_decode_pairs_match_exactly() {
    let files = retained_mode13_files();
    let mut compared = 0_usize;
    let mut explicitly_unsupported_baselines = 0_usize;

    for candidate in files.iter().filter(|path| {
        path.file_name()
            .is_some_and(|name| name.to_string_lossy().ends_with("-candidate.qlic"))
            && path.parent().and_then(Path::file_name).is_some_and(|name| {
                let name = name.to_string_lossy();
                name.contains("decode-pairs") || name == "cpal-planar-full-pairs"
            })
    }) {
        let candidate_name = candidate.file_name().unwrap().to_string_lossy();
        let baseline_name = candidate_name.replace("-candidate.qlic", "-baseline.qlic");
        let baseline = candidate.with_file_name(baseline_name);
        if !baseline.is_file() {
            continue;
        }
        let baseline_file = fs::read(&baseline).unwrap();
        let candidate_file = fs::read(candidate).unwrap();
        let baseline_image = match decode_rgba(&baseline_file, &Limits::default()) {
            Ok(image) => image,
            Err(
                Error::UnsupportedPixelMode(_)
                | Error::UnsupportedPixelTransform { .. }
                | Error::UnsupportedQst1Mode(_)
                | Error::UnsupportedQst1Transform { .. }
                | Error::UnsupportedQst1Channels { .. }
                | Error::UnsupportedQst1Flags { .. },
            ) => {
                explicitly_unsupported_baselines += 1;
                continue;
            }
            Err(error) => panic!("{}: {error}", baseline.display()),
        };
        let candidate_image = decode_rgba(&candidate_file, &Limits::default())
            .unwrap_or_else(|error| panic!("{}: {error}", candidate.display()));
        assert_eq!(candidate_image, baseline_image, "{}", candidate.display());
        compared += 1;
    }

    assert_eq!(compared + explicitly_unsupported_baselines, 10);
    assert!(compared >= 10);
}

#[test]
fn rejects_malformed_cpal_payloads() {
    let palette = palette();

    let mut raw_trailing = palette.clone();
    raw_trailing.extend_from_slice(&[0xa4, 0x06, 0]);
    assert!(matches!(
        decode_rgba(
            &stored_cpal(3, 2, Transform::IdentityRaw, 2, 3, &raw_trailing),
            &Limits::default()
        ),
        Err(Error::InvalidPixelData(
            "packed palette payload size mismatch"
        ))
    ));

    let mut short_runs = palette.clone();
    short_runs.extend_from_slice(&[0, 0]);
    assert!(matches!(
        decode_rgba(
            &stored_cpal(3, 2, Transform::IndexRle, 2, 3, &short_runs),
            &Limits::default()
        ),
        Err(Error::InvalidPixelData(
            "palette runs do not fill the image"
        ))
    ));

    let mut bad_tile_log = vec![2];
    bad_tile_log.extend_from_slice(&palette);
    assert!(matches!(
        decode_rgba(
            &stored_cpal(3, 2, Transform::CompressedPaletteTiles, 2, 3, &bad_tile_log),
            &Limits::default()
        ),
        Err(Error::InvalidPixelData("invalid tile-palette size"))
    ));

    let mut tile_trailing = vec![3];
    tile_trailing.extend_from_slice(&palette);
    tile_trailing.extend_from_slice(&[2, 0, 0, 0, 0xa4, 0x01, 0]);
    assert!(matches!(
        decode_rgba(
            &stored_cpal(
                3,
                2,
                Transform::CompressedPaletteTiles,
                2,
                3,
                &tile_trailing
            ),
            &Limits::default()
        ),
        Err(Error::InvalidPixelData("trailing tile-palette data"))
    ));
}

#[test]
fn rejects_malformed_planar_palette_headers_and_payloads() {
    let palette = planar_palette(257);
    let indices = [0_u16, 1, 255, 256, 42, 17];
    let payload = planar_payload(&palette, &indices, 0);

    for (count, bits) in [(256, 9), (257, 10), (65_537, 16)] {
        let file = stored_cpal(
            3,
            2,
            Transform::CompressedPalettePlanar,
            bits,
            count,
            &payload,
        );
        assert!(matches!(
            Container::parse(&file, &Limits::default()),
            Err(Error::InvalidHeader(_))
        ));
    }

    let mut wrong_mode = stored_cpal(3, 2, Transform::CompressedPalettePlanar, 9, 257, &payload);
    wrong_mode[12] = Mode::Rgba as u8;
    rewrite_crc(&mut wrong_mode);
    assert!(matches!(
        Container::parse(&wrong_mode, &Limits::default()),
        Err(Error::InvalidHeader(
            "invalid planar compressed-palette header"
        ))
    ));

    let mut invalid_layout = payload.clone();
    invalid_layout[0] = 2;
    assert!(matches!(
        decode_rgba(
            &stored_cpal(
                3,
                2,
                Transform::CompressedPalettePlanar,
                9,
                257,
                &invalid_layout
            ),
            &Limits::default()
        ),
        Err(Error::InvalidPixelData("invalid planar palette layout"))
    ));

    for damaged in [&payload[..payload.len() - 1], payload.as_slice()] {
        let mut bytes = damaged.to_vec();
        if bytes.len() == payload.len() {
            bytes.push(0);
        }
        assert!(matches!(
            decode_rgba(
                &stored_cpal(3, 2, Transform::CompressedPalettePlanar, 9, 257, &bytes),
                &Limits::default()
            ),
            Err(Error::InvalidPixelData(
                "planar palette payload size mismatch"
            ))
        ));
    }

    let mut bad_index = payload.clone();
    let index_start = 1 + palette.len();
    bad_index[index_start..index_start + 2].copy_from_slice(&257_u16.to_le_bytes());
    assert!(matches!(
        decode_rgba(
            &stored_cpal(3, 2, Transform::CompressedPalettePlanar, 9, 257, &bad_index),
            &Limits::default()
        ),
        Err(Error::InvalidPixelData("planar palette index out of range"))
    ));
}

#[test]
fn planar_palette_enforces_limits_and_never_panics_on_damage() {
    let palette = planar_palette(257);
    let indices = [0_u16, 1, 255, 256, 42, 17];
    let payload = planar_payload(&palette, &indices, 1);
    let original = stored_cpal(3, 2, Transform::CompressedPalettePlanar, 9, 257, &payload);

    for limits in [
        Limits {
            max_pixels: 5,
            ..Limits::default()
        },
        Limits {
            max_decoded_bytes: 23,
            ..Limits::default()
        },
        Limits {
            max_payload_bytes: payload.len() as u64 - 1,
            ..Limits::default()
        },
    ] {
        assert!(matches!(
            decode_rgba(&original, &limits),
            Err(Error::LimitExceeded { .. })
        ));
    }

    for length in [
        0,
        1,
        HEADER_SIZE - 1,
        HEADER_SIZE,
        original.len() / 2,
        original.len() - 1,
    ] {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&original[..length], &Limits::default());
        }));
        assert!(result.is_ok(), "panicked on a {length}-byte prefix");
    }

    for offset in [
        4,
        8,
        12,
        13,
        14,
        15,
        16,
        20,
        HEADER_SIZE,
        HEADER_SIZE + 1,
        original.len() / 2,
        original.len() - FOOTER_SIZE - 1,
    ] {
        let mut mutated = original.clone();
        mutated[offset] ^= 0xa5;
        rewrite_crc(&mut mutated);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &Limits::default());
        }));
        assert!(
            result.is_ok(),
            "panicked on a rechecksummed planar mutation at {offset}"
        );
    }
}

#[test]
fn cpal_enforces_limits_and_never_panics_on_truncation_or_sampled_damage() {
    let original = fixture("tile-palette-lzms.qlic");
    let container = Container::parse(&original, &Limits::default()).unwrap();
    let decoded_bytes = u64::from(container.header.width) * u64::from(container.header.height) * 4;
    let limits = Limits {
        max_decoded_bytes: decoded_bytes - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&original, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual,
            ..
        }) if actual == decoded_bytes
    ));

    let payload_limits = Limits {
        max_payload_bytes: container.header.payload_size - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&original, &payload_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::PayloadBytes,
            actual,
            ..
        }) if actual == container.header.payload_size
    ));

    for length in [
        0,
        1,
        HEADER_SIZE - 1,
        HEADER_SIZE,
        original.len() / 2,
        original.len() - 1,
    ] {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&original[..length], &Limits::default());
        }));
        assert!(result.is_ok(), "panicked on a {length}-byte prefix");
    }

    for offset in [
        4,
        8,
        12,
        13,
        14,
        15,
        16,
        20,
        HEADER_SIZE,
        original.len() / 2,
        original.len() - FOOTER_SIZE - 1,
    ] {
        let mut mutated = original.clone();
        mutated[offset] ^= 0xa5;
        rewrite_crc(&mut mutated);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &Limits::default());
        }));
        assert!(
            result.is_ok(),
            "panicked on a rechecksummed mutation at {offset}"
        );
    }
}
