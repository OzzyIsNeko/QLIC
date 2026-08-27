use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{Error, LimitKind, Limits, crc32, decode_rgba};

fn fixture_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn fixture(name: &str) -> Vec<u8> {
    fs::read(fixture_dir().join(name)).unwrap()
}

fn rewrite_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - 4;
    let checksum = crc32(&bytes[..footer]).to_le_bytes();
    bytes[footer..].copy_from_slice(&checksum);
}

fn stored_file(
    width: u32,
    height: u32,
    mode: u8,
    transform: u8,
    index_bits: u8,
    palette: &[[u8; 4]],
    payload: &[u8],
) -> Vec<u8> {
    let mut file = Vec::new();
    file.extend_from_slice(b"QLIC");
    file.extend_from_slice(&width.to_le_bytes());
    file.extend_from_slice(&height.to_le_bytes());
    file.extend_from_slice(&[mode, transform, index_bits, 0x80]);
    file.extend_from_slice(&(palette.len() as u32).to_le_bytes());
    file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    for color in palette {
        file.extend_from_slice(color);
    }
    file.extend_from_slice(payload);
    let checksum = crc32(&file);
    file.extend_from_slice(&checksum.to_le_bytes());
    file
}

#[test]
fn matches_production_rgba_crcs_for_the_staged_fixtures() {
    let vectors = [
        ("gray-rle.qlic", 3, 31, 0xd59a_4f1e),
        ("palette.qlic", 17, 1, 0x5f37_0f87),
        ("rgb-lzms.qlic", 257, 9, 0x1052_8ad2),
        ("planar-med-lzms.qlic", 3, 2, 0xd042_03d4),
        ("separable.qlic", 1, 1, 0x0c46_3091),
    ];
    for (name, width, height, expected_crc) in vectors {
        let image = decode_rgba(&fixture(name), &Limits::default()).unwrap();
        assert_eq!((image.width, image.height), (width, height), "{name}");
        assert_eq!(image.stride(), width as usize * 4, "{name}");
        assert_eq!(image.rgba.len(), width as usize * height as usize * 4);
        assert_eq!(crc32(&image.rgba), expected_crc, "{name}");
    }
}

#[test]
fn decodes_raw_gray_alpha_and_rgba_green_delta() {
    let gray_alpha = stored_file(2, 1, 2, 2, 0, &[], &[10, 20, 30, 40]);
    assert_eq!(
        decode_rgba(&gray_alpha, &Limits::default()).unwrap().rgba,
        [10, 10, 10, 20, 30, 30, 30, 40]
    );

    let rgba = stored_file(1, 1, 4, 3, 0, &[], &[10, 1, 255, 20]);
    assert_eq!(
        decode_rgba(&rgba, &Limits::default()).unwrap().rgba,
        [11, 10, 9, 20]
    );
}

#[test]
fn decodes_filtered_samples_and_packed_palette_rows() {
    // First RGB row uses Sub; the second uses Up.
    let filtered_rgb = stored_file(
        2,
        2,
        3,
        0,
        0,
        &[],
        &[1, 1, 2, 3, 3, 3, 3, 2, 6, 6, 6, 6, 6, 6],
    );
    assert_eq!(
        decode_rgba(&filtered_rgb, &Limits::default()).unwrap().rgba,
        [1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255,]
    );

    let palette = [[1, 2, 3, 4], [5, 6, 7, 8], [9, 10, 11, 12]];
    let filtered_palette = stored_file(3, 1, 5, 0, 2, &palette, &[0, 0x24]);
    assert_eq!(
        decode_rgba(&filtered_palette, &Limits::default())
            .unwrap()
            .rgba,
        palette.concat()
    );
}

#[test]
fn decodes_filtered_red_and_blue_delta_samples() {
    let red_delta = stored_file(1, 1, 3, 8, 0, &[], &[0, 10, 1, 255]);
    assert_eq!(
        decode_rgba(&red_delta, &Limits::default()).unwrap().rgba,
        [10, 11, 9, 255]
    );

    let blue_delta = stored_file(1, 1, 3, 9, 0, &[], &[0, 10, 1, 255]);
    assert_eq!(
        decode_rgba(&blue_delta, &Limits::default()).unwrap().rgba,
        [11, 9, 10, 255]
    );
}

#[test]
fn decodes_palette_runs_green_delta_rle_and_separable_delta() {
    let palette = [[1, 2, 3, 4], [5, 6, 7, 8]];
    let index_runs = stored_file(4, 1, 5, 6, 1, &palette, &[1, 0, 1, 1]);
    assert_eq!(
        decode_rgba(&index_runs, &Limits::default()).unwrap().rgba,
        [1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8, 5, 6, 7, 8,]
    );

    let green_rle = stored_file(1, 1, 4, 5, 0, &[], &[0, 10, 0, 1, 0, 255, 0, 20]);
    assert_eq!(
        decode_rgba(&green_rle, &Limits::default()).unwrap().rgba,
        [11, 10, 9, 20]
    );

    let separable_delta = stored_file(2, 2, 7, 7, 1, &[], &[10, 10, 5]);
    assert_eq!(
        decode_rgba(&separable_delta, &Limits::default())
            .unwrap()
            .rgba,
        [
            10, 10, 10, 255, 20, 20, 20, 255, 15, 15, 15, 255, 25, 25, 25, 255,
        ]
    );
}

#[test]
fn rejects_unsupported_modes_transforms_and_output_caps_explicitly() {
    assert!(matches!(
        decode_rgba(&fixture("blocks.qlic"), &Limits::default()),
        Err(Error::UnsupportedPixelMode(18))
    ));

    let unsupported = stored_file(1, 1, 1, 3, 0, &[], &[7]);
    assert!(matches!(
        decode_rgba(&unsupported, &Limits::default()),
        Err(Error::UnsupportedPixelTransform {
            mode: 1,
            transform: 3
        })
    ));

    let limits = Limits {
        max_decoded_bytes: 3,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&stored_file(1, 1, 1, 2, 0, &[], &[7]), &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual: 4,
            ..
        })
    ));
}

#[test]
fn rejects_malformed_filtered_rle_palette_and_separable_payloads() {
    let bad_filter = stored_file(1, 1, 1, 0, 0, &[], &[6, 0]);
    assert!(matches!(
        decode_rgba(&bad_filter, &Limits::default()),
        Err(Error::InvalidPixelData("invalid row filter"))
    ));

    let mut excessive_run = fixture("gray-rle.qlic");
    excessive_run[28] = 0x5d;
    rewrite_crc(&mut excessive_run);
    assert!(matches!(
        decode_rgba(&excessive_run, &Limits::default()),
        Err(Error::InvalidPixelData("run exceeds expected sample size"))
    ));

    let palette = [[1, 2, 3, 4], [5, 6, 7, 8]];
    let bad_index = stored_file(1, 1, 5, 2, 2, &palette, &[3]);
    assert!(matches!(
        decode_rgba(&bad_index, &Limits::default()),
        Err(Error::InvalidPixelData("palette index out of range"))
    ));

    let short_separable = stored_file(2, 2, 7, 0, 1, &[], &[1, 2]);
    assert!(matches!(
        decode_rgba(&short_separable, &Limits::default()),
        Err(Error::InvalidPixelData(
            "declared payload size does not match fixed representation"
        ))
    ));

    let mut stored_size_mismatch = fixture("gray-rle.qlic");
    stored_size_mismatch[20..28].copy_from_slice(&3_u64.to_le_bytes());
    rewrite_crc(&mut stored_size_mismatch);
    assert!(matches!(
        decode_rgba(&stored_size_mismatch, &Limits::default()),
        Err(Error::InvalidPixelData(
            "stored payload size does not match header"
        ))
    ));
}

#[test]
fn public_decode_rejects_truncation_and_mutation_without_panicking() {
    let names = [
        "gray-rle.qlic",
        "palette.qlic",
        "rgb-lzms.qlic",
        "planar-med-lzms.qlic",
        "separable.qlic",
    ];
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 1 << 18,
        max_decoded_bytes: 1 << 20,
        ..Limits::default()
    };
    for name in names {
        let original = fixture(name);
        for length in 0..original.len() {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let _ = decode_rgba(&original[..length], &limits);
            }));
            assert!(
                result.is_ok(),
                "panicked on {name} prefix of {length} bytes"
            );
        }

        // Preserve the outer CRC so mutations exercise payload and header
        // validation below the container-integrity check.
        for offset in 12..original.len() - 4 {
            let mut mutated = original.clone();
            mutated[offset] ^= 0xa5;
            rewrite_crc(&mut mutated);
            let result = catch_unwind(AssertUnwindSafe(|| {
                let _ = decode_rgba(&mutated, &limits);
            }));
            assert!(result.is_ok(), "panicked on {name} mutation at {offset}");
        }
    }
}
